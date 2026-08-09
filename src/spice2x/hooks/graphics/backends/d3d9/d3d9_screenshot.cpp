#include "d3d9_screenshot.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <external/robin_hood.h>

#ifdef __GNUC__
#include <d3dx9tex.h>
#endif

#include "avs/game.h"
#include "games/io.h"
#include "hooks/graphics/graphics.h"
#include "launcher/launcher.h"
#include "misc/clipboard.h"
#include "misc/eamuse.h"
#include "overlay/notifications.h"
#include "overlay/overlay.h"
#include "util/fileutils.h"
#include "util/libutils.h"
#include "util/logging.h"
#include "util/threadpool.h"

#ifdef __GNUC__
typedef decltype(D3DXSaveSurfaceToFileA) *D3DXSaveSurfaceToFileA_t;
#else
#define D3DXIFF_PNG ((DWORD) 3)

typedef HRESULT (WINAPI *D3DXSaveSurfaceToFileA_t)(
        LPCSTR pDestFile,
        DWORD DestFormat,
        LPDIRECT3DSURFACE9 pSrcSurface,
        CONST PALETTEENTRY *pSrcPalette,
        CONST RECT *pSrcRect);
#endif

namespace {

bool ATTEMPTED_D3DX9_LOAD_LIBRARY = false;
std::mutex SCREENSHOT_SAVE_M;

struct PendingScreenshotSurface {
    int screen;
    D3DSURFACE_DESC desc;
    IDirect3DSurface9 *surface;
};

// convert copied surface to RGB and enqueue it for API capture
void save_capture(
        int screen,
        D3DFORMAT format,
        UINT width,
        UINT height,
        IDirect3DSurface9 *surface) {
    D3DLOCKED_RECT finished_copy {};
    HRESULT hr = surface->LockRect(&finished_copy, nullptr, 0);
    if (FAILED(hr)) {
        log_warning("graphics::d3d9", "failed to lock screenshot surface, hr={}", FMT_HRESULT(hr));
        graphics_capture_skip(screen);
        return;
    }

    const size_t pitch = finished_copy.Pitch;
    auto data = reinterpret_cast<uint8_t *>(finished_copy.pBits);
    auto pixels = std::unique_ptr<uint8_t[]>(new uint8_t[width * height * 3]);
    for (size_t row = 0; row < height; row++) {
        size_t offset_row = row * width * 3;
        switch (format) {
            case D3DFMT_R8G8B8: {
                for (size_t column = 0; column < width; column++) {
                    auto cell = data + row * pitch + column * 3;
                    auto pixel = &pixels[offset_row + column * 3];
                    pixel[0] = cell[0];
                    pixel[1] = cell[1];
                    pixel[2] = cell[2];
                }
                break;
            }
            case D3DFMT_X8R8G8B8:
            case D3DFMT_A8R8G8B8: {
                for (size_t column = 0; column < width; column++) {
                    auto cell = data + row * pitch + column * 4;
                    auto pixel = &pixels[offset_row + column * 3];
                    pixel[0] = cell[2];
                    pixel[1] = cell[1];
                    pixel[2] = cell[0];
                }
                break;
            }
            case D3DFMT_X8B8G8R8:
            case D3DFMT_A8B8G8R8: {
                for (size_t column = 0; column < width; column++) {
                    auto cell = data + row * pitch + column * 4;
                    auto pixel = &pixels[offset_row + column * 3];
                    pixel[0] = cell[0];
                    pixel[1] = cell[1];
                    pixel[2] = cell[2];
                }
                break;
            }
            default: {
                for (size_t column = 0; column < width; column++) {
                    auto pixel = &pixels[offset_row + column * 3];
                    pixel[0] = 0;
                    pixel[1] = 0;
                    pixel[2] = 0;
                }
            }
        }
    }

    hr = surface->UnlockRect();
    if (FAILED(hr)) {
        log_warning("graphics::d3d9", "failed to unlock screenshot surface, hr={}", FMT_HRESULT(hr));
        graphics_capture_skip(screen);
        return;
    }

    graphics_capture_enqueue(screen, pixels.release(), width, height);
}

// save copied surface as PNG and optionally update clipboard and notification
bool save_screenshot(
        const std::string &file_path,
        D3DFORMAT format,
        UINT width,
        UINT height,
        IDirect3DSurface9 *surface,
        bool update_ui = true) {
    // 32-bit XRGB and ARGB surfaces use byte 3 as alpha; force opaque PNG output
    if (format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8 ||
        format == D3DFMT_X8B8G8R8 || format == D3DFMT_A8B8G8R8) {

        D3DLOCKED_RECT finished_copy {};
        HRESULT hr = surface->LockRect(&finished_copy, nullptr, 0);
        if (FAILED(hr)) {
            log_warning("graphics::d3d9", "failed to lock screenshot surface, hr={}", FMT_HRESULT(hr));
            return false;
        }

        const size_t pitch = finished_copy.Pitch;
        auto data = reinterpret_cast<uint8_t *>(finished_copy.pBits);
        for (size_t row = 0; row < height; row++) {
            for (size_t column = 0; column < width; column++) {
                data[row * pitch + column * 4 + 3] = 255;
            }
        }

        hr = surface->UnlockRect();
        if (FAILED(hr)) {
            log_warning("graphics::d3d9", "failed to unlock screenshot surface, hr={}", FMT_HRESULT(hr));
            return false;
        }
    }

    static D3DXSaveSurfaceToFileA_t D3DXSaveSurfaceToFileA_ptr = nullptr;
    if (D3DXSaveSurfaceToFileA_ptr == nullptr) {
        D3DXSaveSurfaceToFileA_ptr = libutils::try_proc<D3DXSaveSurfaceToFileA_t>("D3DXSaveSurfaceToFileA");

        if (!ATTEMPTED_D3DX9_LOAD_LIBRARY && D3DXSaveSurfaceToFileA_ptr == nullptr) {
            ATTEMPTED_D3DX9_LOAD_LIBRARY = true;

            // prefer the newest installed helper while supporting older D3DX9 runtimes
            for (size_t i = 43; i >= 24; i--) {
                auto lib_name = fmt::format("d3dx9_{}.dll", i);
                auto d3dx9 = libutils::try_library(lib_name);
                if (d3dx9 == nullptr) {
                    continue;
                }

                D3DXSaveSurfaceToFileA_ptr = libutils::try_proc<D3DXSaveSurfaceToFileA_t>(
                        d3dx9, "D3DXSaveSurfaceToFileA");
                if (D3DXSaveSurfaceToFileA_ptr == nullptr) {
                    FreeLibrary(d3dx9);
                    continue;
                }

                log_info("graphics::d3d9", "found surface save function in '{}'", lib_name);
                break;
            }
        }
    }

    if (D3DXSaveSurfaceToFileA_ptr == nullptr) {
        log_warning("graphics::d3d9", "Direct3D save helper function not available");
        return false;
    }

    log_info("graphics::d3d9", "saving screenshot to {}", file_path);
        const HRESULT save_result = D3DXSaveSurfaceToFileA_ptr(
            file_path.c_str(), D3DXIFF_PNG, surface, nullptr, nullptr);
        if (FAILED(save_result)) {
        log_warning("graphics::d3d9", "Failed to save screenshot");
        if (update_ui) {
            overlay::notifications::add(
                overlay::notifications::Severity::Error,
                "Screenshot failed to save");
        }
        return false;
    }

    if (update_ui) {
        clipboard::copy_image(file_path);
        overlay::notifications::add(
            overlay::notifications::Severity::Success,
            fmt::format("Screenshot saved: {}", fileutils::basename(file_path)));
    }
    return true;
}

// derive subscreen path without changing the primary filename
std::string screenshot_path_for_screen(const std::string &primary_path, int screen) {
    if (screen == 0) {
        return primary_path;
    }

    const std::filesystem::path path(primary_path);
    return (path.parent_path() /
            fmt::format("{}_{}{}", path.stem().string(), screen, path.extension().string())).string();
}

// copy logical screen backbuffer into a lockable surface for deferred processing
bool copy_screenshot_surface(
        IDirect3DDevice9 *device,
        int screen,
        PendingScreenshotSurface &pending) {
    IDirect3DSwapChain9 *swap_chain = nullptr;
    HRESULT hr = device->GetSwapChain(screen, &swap_chain);
    if (FAILED(hr) || swap_chain == nullptr) {
        log_warning("graphics::d3d9",
                "failed to get swap chain for screen {}, hr={}",
                screen,
                FMT_HRESULT(hr));
        return false;
    }

    IDirect3DSurface9 *buffer = nullptr;
    hr = swap_chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &buffer);
    swap_chain->Release();
    if (FAILED(hr) || buffer == nullptr) {
        log_warning("graphics::d3d9",
                "failed to get back buffer for screen {}, hr={}",
                screen,
                FMT_HRESULT(hr));
        return false;
    }

    D3DSURFACE_DESC desc {};
    hr = buffer->GetDesc(&desc);
    if (FAILED(hr)) {
        log_warning("graphics::d3d9",
                "failed to acquire back buffer descriptor for screen {}, hr={}",
                screen,
                FMT_HRESULT(hr));
        buffer->Release();
        return false;
    }

    IDirect3DSurface9 *temp_surface = nullptr;
    hr = device->CreateRenderTarget(
            desc.Width, desc.Height, desc.Format, desc.MultiSampleType,
            desc.MultiSampleQuality, TRUE, &temp_surface, nullptr);
    if (FAILED(hr) || temp_surface == nullptr) {
        log_warning("graphics::d3d9",
                "failed to acquire temporary surface for screen {}, hr={}",
                screen,
                FMT_HRESULT(hr));
        buffer->Release();
        return false;
    }

    hr = device->StretchRect(buffer, nullptr, temp_surface, nullptr, D3DTEXF_NONE);
    buffer->Release();
    if (FAILED(hr)) {
        log_warning("graphics::d3d9",
                "failed to copy back buffer contents for screen {}, hr={}",
                screen,
                FMT_HRESULT(hr));
        temp_surface->Release();
        return false;
    }

    pending.screen = screen;
    pending.desc = desc;
    pending.surface = temp_surface;
    return true;
}

// consume pending request, copy surfaces, and dispatch deferred processing
void process_screenshot_request(
        IDirect3DDevice9 *device,
        bool screenshot) {
    const bool capture = !screenshot;
    int capture_screen = 0;
    if (screenshot ? !graphics_screenshot_consume() : !graphics_capture_consume(&capture_screen)) {
        return;
    }

    const bool screenshot_subscreens = screenshot && GRAPHICS_SCREENSHOT_SUBSCREENS;
    std::vector<int> screens { capture_screen };
    if (screenshot_subscreens) {
        screens.clear();
        graphics_screens_get(screens);
        if (std::find(screens.begin(), screens.end(), 0) == screens.end()) {
            screens.insert(screens.begin(), 0);
        }
    }

    std::vector<PendingScreenshotSurface> pending_surfaces;
    pending_surfaces.reserve(screens.size());
    for (auto screen : screens) {
        PendingScreenshotSurface pending {};
        if (copy_screenshot_surface(device, screen, pending)) {
            pending_surfaces.emplace_back(pending);
        } else if (capture) {
            graphics_capture_skip(capture_screen);
            return;
        }
    }

    if (pending_surfaces.empty()) {
        if (screenshot_subscreens) {
            overlay::notifications::add(
                overlay::notifications::Severity::Error,
                "Screenshot failed to capture");
        }
        return;
    }

    auto surface_process = [
            screenshot,
            capture,
            capture_screen,
            screenshot_subscreens,
            screens = std::move(screens),
            pending_surfaces = std::move(pending_surfaces)]() {
        if (capture) {
            const auto &pending = pending_surfaces.front();
            save_capture(
                capture_screen,
                pending.desc.Format,
                pending.desc.Width,
                pending.desc.Height,
                pending.surface);
        }

        if (screenshot) {
            std::lock_guard<std::mutex> lock(SCREENSHOT_SAVE_M);

            if (screenshot_subscreens) {
                const auto screenshot_path = graphics_screenshot_genpath(screens);
                size_t failed = screenshot_path.empty()
                        ? screens.size()
                        : screens.size() - pending_surfaces.size();
                std::string primary_path;

                if (!screenshot_path.empty()) {
                    for (const auto &pending : pending_surfaces) {
                        const auto path = screenshot_path_for_screen(screenshot_path, pending.screen);
                        if (save_screenshot(
                            path,
                            pending.desc.Format,
                            pending.desc.Width,
                            pending.desc.Height,
                            pending.surface,
                            false)) {
                            if (pending.screen == 0) {
                                primary_path = path;
                            }
                        } else {
                            failed++;
                        }
                    }
                }

                if (!primary_path.empty()) {
                    clipboard::copy_image(primary_path);
                    overlay::notifications::add(
                        overlay::notifications::Severity::Success,
                        fmt::format(
                            "Screenshot saved: {}",
                            fileutils::basename(primary_path)));
                } else {
                    overlay::notifications::add(
                        overlay::notifications::Severity::Error,
                        "Screenshot failed to save");
                }

                if (failed > 0) {
                    log_warning(
                        "graphics::d3d9",
                        "failed to capture or save {} screenshot screen(s)",
                        failed);
                }
            } else {
                auto file_path = graphics_screenshot_genpath();
                if (!file_path.empty()) {
                    const auto &pending = pending_surfaces.front();
                    save_screenshot(
                            file_path,
                            pending.desc.Format,
                            pending.desc.Width,
                            pending.desc.Height,
                            pending.surface);
                }
            }
        }

        for (auto &pending : pending_surfaces) {
            pending.surface->Release();
        }
    };

    static const robin_hood::unordered_set<std::string> THREAD_BAN {
            "JMA",
#ifndef SPICE64
            // KFC only crashes under threaded processing in 32-bit builds
            "KFC",
#endif
            "KMA",
            "KLP",
            "LMA",
    };

    if (THREAD_BAN.contains(avs::game::MODEL)) {
        surface_process();
    } else {
        static auto pool = ThreadPool(2);
        pool.add(surface_process);
    }
}

} // namespace

// process pending file screenshot request
void graphics_d3d9_process_screenshot(IDirect3DDevice9 *device) {
    process_screenshot_request(device, true);
}

// process pending API capture request
void graphics_d3d9_process_capture(IDirect3DDevice9 *device) {
    process_screenshot_request(device, false);
}
