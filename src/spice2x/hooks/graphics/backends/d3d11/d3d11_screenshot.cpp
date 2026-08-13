// dx11 screenshot capture. mirrors the d3d9 backend: copy the current
// backbuffer into a staging texture, force alpha=255, write PNG via
// stb_image_write, push to clipboard and notify.

#include "d3d11_backend.h"

#ifdef SPICE_D3D11

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "d3d11_internal.h"

#include "external/stb_image_write.h"
#include "hooks/graphics/graphics.h"
#include "misc/clipboard.h"
#include "overlay/notifications.h"
#include "util/fileutils.h"

using d3d11_hooks::com_ptr;

namespace {

constexpr auto SCREENSHOT_SESSION_LIFETIME = std::chrono::seconds(1);

struct ScreenshotSession {
    std::unordered_set<IDXGISwapChain *> captured;
    int next_screen = 1;
    HWND primary_window = nullptr;
    std::string primary_path;
    std::chrono::steady_clock::time_point deadline {};
};

ScreenshotSession SCREENSHOT_SESSION;
std::mutex SCREENSHOT_M;

std::string screenshot_path_for_screen(const std::string &primary_path, int screen) {
    if (screen == 0) {
        return primary_path;
    }

    const std::filesystem::path path(primary_path);
    return (path.parent_path() /
            fmt::format("{}_{}{}", path.stem().string(), screen, path.extension().string())).string();
}

bool is_screenshot_subscreen(IDXGISwapChain *swapchain, HWND primary_window) {
    DXGI_SWAP_CHAIN_DESC desc {};
    if (FAILED(swapchain->GetDesc(&desc)) || !desc.OutputWindow) {
        return false;
    }
    RECT client_rect {};
    return desc.OutputWindow != primary_window &&
        desc.OutputWindow != d3d11_hooks::main_hwnd() &&
        IsWindowVisible(desc.OutputWindow) &&
        GetAncestor(desc.OutputWindow, GA_ROOT) == desc.OutputWindow &&
        GetClientRect(desc.OutputWindow, &client_rect) &&
        client_rect.right > client_rect.left &&
        client_rect.bottom > client_rect.top;
}

// copy the swapchain backbuffer into a CPU-readable staging texture and
// flatten it into an RGBA8 buffer (BGRA backbuffers are swizzled,
// alpha is forced to 255).
bool copy_backbuffer_to_rgba(IDXGISwapChain *swapchain,
                             ID3D11Device *device,
                             ID3D11DeviceContext *context,
                             std::vector<uint8_t> &out,
                             uint32_t &out_w, uint32_t &out_h)
{
    ID3D11Texture2D *raw_bb = nullptr;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&raw_bb))) || !raw_bb) {
        return false;
    }
    com_ptr<ID3D11Texture2D> backbuffer(raw_bb);

    D3D11_TEXTURE2D_DESC desc {};
    backbuffer->GetDesc(&desc);

    // MSAA backbuffers can't be CopyResource'd into a non-MS staging target.
    com_ptr<ID3D11Texture2D> resolved;
    ID3D11Texture2D *source = backbuffer.get();
    if (desc.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = desc;
        rd.SampleDesc.Count = 1;
        rd.SampleDesc.Quality = 0;
        rd.Usage = D3D11_USAGE_DEFAULT;
        rd.BindFlags = D3D11_BIND_RENDER_TARGET;
        rd.CPUAccessFlags = 0;
        rd.MiscFlags = 0;
        ID3D11Texture2D *r = nullptr;
        if (FAILED(device->CreateTexture2D(&rd, nullptr, &r)) || !r) {
            return false;
        }
        resolved.reset(r);
        context->ResolveSubresource(resolved.get(), 0, backbuffer.get(), 0, desc.Format);
        source = resolved.get();
    }

    D3D11_TEXTURE2D_DESC sd {};
    sd.Width = desc.Width;
    sd.Height = desc.Height;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = desc.Format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D *raw_staging = nullptr;
    if (FAILED(device->CreateTexture2D(&sd, nullptr, &raw_staging)) || !raw_staging) {
        return false;
    }
    com_ptr<ID3D11Texture2D> staging(raw_staging);
    context->CopyResource(staging.get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    // backbuffers from GetDesc are always fully-typed (never _TYPELESS).
    const bool is_bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
                      || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    out.resize(static_cast<size_t>(desc.Width) * desc.Height * 4);
    const uint8_t *src_base = reinterpret_cast<const uint8_t *>(mapped.pData);
    for (uint32_t y = 0; y < desc.Height; ++y) {
        const uint8_t *row = src_base + static_cast<size_t>(y) * mapped.RowPitch;
        uint8_t *dst = out.data() + static_cast<size_t>(y) * desc.Width * 4;
        for (uint32_t x = 0; x < desc.Width; ++x) {
            dst[x * 4 + 0] = row[x * 4 + (is_bgra ? 2 : 0)];
            dst[x * 4 + 1] = row[x * 4 + 1];
            dst[x * 4 + 2] = row[x * 4 + (is_bgra ? 0 : 2)];
            dst[x * 4 + 3] = 255;
        }
    }

    context->Unmap(staging.get(), 0);

    out_w = desc.Width;
    out_h = desc.Height;
    return true;
}

} // namespace

namespace d3d11_hooks {

void try_screenshot(IDXGISwapChain *swapchain) {
    const bool primary = swapchain && overlay::OVERLAY &&
        overlay::OVERLAY->uses_swapchain(swapchain);
    if (!swapchain || (!primary && !GRAPHICS_SCREENSHOT_SUBSCREENS)) {
        return;
    }

    std::unique_lock<std::mutex> lock;
    int screen = 0;
    std::string file_path;
    if (primary) {
        if (!graphics_screenshot_consume()) {
            return;
        }
        file_path = graphics_screenshot_genpath();
        if (GRAPHICS_SCREENSHOT_SUBSCREENS) {
            lock = std::unique_lock<std::mutex>(SCREENSHOT_M);
            SCREENSHOT_SESSION = {};
            DXGI_SWAP_CHAIN_DESC desc {};
            if (SUCCEEDED(swapchain->GetDesc(&desc))) {
                SCREENSHOT_SESSION.primary_window = desc.OutputWindow;
            }
        }
    } else {
        lock = std::unique_lock<std::mutex>(SCREENSHOT_M);
        if (SCREENSHOT_SESSION.primary_path.empty() ||
            std::chrono::steady_clock::now() >= SCREENSHOT_SESSION.deadline ||
            SCREENSHOT_SESSION.captured.contains(swapchain) ||
            !is_screenshot_subscreen(swapchain, SCREENSHOT_SESSION.primary_window)) {
            return;
        }
        do {
            screen = SCREENSHOT_SESSION.next_screen++;
            file_path = screenshot_path_for_screen(SCREENSHOT_SESSION.primary_path, screen);
        } while (std::filesystem::exists(file_path));
    }

    if (file_path.empty()) {
        return;
    }

    ID3D11Device *raw_device = nullptr;
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&raw_device))) || !raw_device) {
        return;
    }
    com_ptr<ID3D11Device> device(raw_device);
    ID3D11DeviceContext *raw_ctx = nullptr;
    device->GetImmediateContext(&raw_ctx);
    if (!raw_ctx) {
        return;
    }
    com_ptr<ID3D11DeviceContext> context(raw_ctx);

    std::vector<uint8_t> pixels;
    uint32_t w = 0, h = 0;
    if (!copy_backbuffer_to_rgba(swapchain, device.get(), context.get(), pixels, w, h)) {
        log_warning("graphics::d3d11", "screenshot: failed to capture screen {} backbuffer", screen);
        if (primary) {
            overlay::notifications::add(
                overlay::notifications::Severity::Error,
                "Screenshot failed to capture");
        }
        return;
    }

    log_info("graphics::d3d11", "saving screenshot to {}", file_path);
    if (stbi_write_png(file_path.c_str(), (int) w, (int) h, 4,
                       pixels.data(), (int) w * 4))
    {
        if (primary) {
            clipboard::copy_image(file_path);
            overlay::notifications::add(
                overlay::notifications::Severity::Success,
                fmt::format("Screenshot saved: {}", fileutils::basename(file_path)));
            if (GRAPHICS_SCREENSHOT_SUBSCREENS) {
                SCREENSHOT_SESSION.primary_path = file_path;
                SCREENSHOT_SESSION.deadline =
                    std::chrono::steady_clock::now() + SCREENSHOT_SESSION_LIFETIME;
            }
        } else {
            SCREENSHOT_SESSION.captured.emplace(swapchain);
        }
    } else {
        log_warning("graphics::d3d11", "screenshot: failed to save screen {}", screen);
        if (primary) {
            overlay::notifications::add(
                overlay::notifications::Severity::Error,
                "Screenshot failed to save");
        }
    }
}

}

#endif // SPICE_D3D11
