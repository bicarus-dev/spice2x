#include "d3d9_screenshot.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <external/robin_hood.h>
#include <external/fpng/fpng.h>

#include "avs/game.h"
#include "hooks/graphics/graphics.h"
#include "misc/clipboard.h"
#include "overlay/notifications.h"
#include "util/fileutils.h"
#include "util/logging.h"
#include "util/threadpool.h"

#include "d3d9_device.h"
#include "d3d9_readback.h"

// genpath picks filenames by probing the disk, so the whole save has to be serialised:
// a name is only taken once its file exists, not when genpath hands it out
static std::mutex SCREENSHOT_SAVE_M;

namespace {

// a screen already read out of its surface, so nothing here touches D3D. the bytes
// are still in the surface's format; converting them is left to the encode
struct PendingWrite {
    int screen {};
    D3DFORMAT format {};
    UINT width {};
    UINT height {};
    size_t pitch {};
    std::vector<uint8_t> data;
    std::string path;
    bool saved = false;
};

struct PendingCapture {
    int screen {};
    D3DFORMAT format {};
    UINT width {};
    UINT height {};
    size_t pitch {};
    std::vector<uint8_t> data;
};

// packed 24bpp RGB, what both the png encoder and the api capture consume
constexpr size_t RGB_PIXEL_SIZE = 3;
// the formats surface_to_rgb knows how to convert; the two must stay in sync
static std::optional<size_t> surface_pixel_size(D3DFORMAT format) {
    switch (format) {
        // what back buffers are actually created as in practice
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8R8G8B8:

        // a valid display format, but no supported game has been seen presenting one
        case D3DFMT_A2R10G10B10:
            return 4;

        // valid display formats, but no supported game has been seen presenting one
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
            return 2;

        default:
            return std::nullopt;
    }
}

struct ImageSize {
    size_t row_size {};
    size_t total_size {};
};

static std::optional<ImageSize> compute_image_size(
        UINT width,
        UINT height,
        size_t bytes_per_pixel) {

    if (width == 0 || height == 0
            || width > std::numeric_limits<size_t>::max() / bytes_per_pixel) {
        return std::nullopt;
    }

    const size_t row_size = static_cast<size_t>(width) * bytes_per_pixel;
    if (height > std::numeric_limits<size_t>::max() / row_size) {
        return std::nullopt;
    }

    return ImageSize { row_size, static_cast<size_t>(height) * row_size };
}

static bool resize_pixels(std::vector<uint8_t> &pixels, size_t size) {
    try {
        pixels.resize(size);
        return true;
    } catch (const std::exception &error) {
        log_warning("graphics::d3d9", "failed to allocate image buffer: {}", error.what());
        return false;
    }
}

// timing for the CPU-side Lock+memcpy readback of an already-ready capture surface, logged
// periodically alongside the GPU-side capture timing in d3d9_readback.cpp. written from
// whichever thread did the read, hence the mutex. safe to delete once that question is
// answered.
struct CaptureReadTiming {
    double total_us = 0.0;
    double max_us = 0.0;
    size_t count = 0;
    std::chrono::steady_clock::time_point last_log = std::chrono::steady_clock::now();
};

std::mutex CAPTURE_READ_TIMING_M;
CaptureReadTiming CAPTURE_READ_TIMING;

void record_capture_read(double us) {
    std::lock_guard<std::mutex> lock(CAPTURE_READ_TIMING_M);

    CAPTURE_READ_TIMING.total_us += us;
    CAPTURE_READ_TIMING.max_us = std::max(CAPTURE_READ_TIMING.max_us, us);
    CAPTURE_READ_TIMING.count++;

    const auto now = std::chrono::steady_clock::now();
    if (now - CAPTURE_READ_TIMING.last_log < std::chrono::seconds(2)) {
        return;
    }
    CAPTURE_READ_TIMING.last_log = now;

    log_info("graphics::d3d9",
            "capture cpu readback us (avg/max/n): {:.0f}/{:.0f}/{}",
            CAPTURE_READ_TIMING.total_us / static_cast<double>(CAPTURE_READ_TIMING.count),
            CAPTURE_READ_TIMING.max_us, CAPTURE_READ_TIMING.count);

    CAPTURE_READ_TIMING.total_us = 0.0;
    CAPTURE_READ_TIMING.max_us = 0.0;
    CAPTURE_READ_TIMING.count = 0;
}

// the api capture stages a whole back buffer every frame, so the staging buffer
// is recycled rather than reallocated. returned buffers keep their size, which
// leaves the reuse free of a zero fill
class CaptureBuffers {
public:
    std::vector<uint8_t> take() {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->idle.empty()) {
            return {};
        }

        auto buffer = std::move(this->idle.back());
        this->idle.pop_back();
        return buffer;
    }

    void give(std::vector<uint8_t> buffer) {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->idle.size() < MAX_IDLE) {
            this->idle.push_back(std::move(buffer));
        }
    }

private:
    // one per save in flight plus one for the next capture; a full screen is
    // several megabytes, so the cap matters
    static constexpr size_t MAX_IDLE = 2;

    std::mutex mutex;
    std::vector<std::vector<uint8_t>> idle;
};

// deliberately never destroyed, so a save still running at process exit cannot
// hand a buffer back to a dead free list
CaptureBuffers &capture_buffers() {
    static CaptureBuffers *instance = new CaptureBuffers();
    return *instance;
}

// encodes get their own pool: the dispatch below already occupies a worker on its
// pool, so queueing onto that one and waiting could starve itself. never destroyed
// for the same reason as the buffers above
ThreadPool &encode_pool() {
    static auto *instance = new ThreadPool(2);
    return *instance;
}

// where the capture surface read runs when it is allowed off the present thread; see
// try_deliver_capture for when that applies. never destroyed for the same reason as above
ThreadPool &capture_read_pool() {
    static auto *instance = new ThreadPool(2);
    return *instance;
}

// normalize the supported D3D formats to packed 24bpp RGB. callers screen the
// format through surface_pixel_size first, so the black fill below is a fallback
void surface_to_rgb(
        D3DFORMAT format,
        UINT width,
        UINT height,
        const uint8_t *data,
        size_t pitch,
        uint8_t *pixels) {

    for (size_t row = 0; row < height; row++) {
        size_t offset_row = row * width * 3;
        switch (format) {
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
            // the 5 and 6 bit channels are widened by bit replication so that
            // full scale stays full scale
            case D3DFMT_R5G6B5: {
                auto cells = reinterpret_cast<const uint16_t *>(data + row * pitch);
                for (size_t column = 0; column < width; column++) {
                    const uint16_t cell = cells[column];
                    const uint8_t red = (cell >> 11) & 0x1F;
                    const uint8_t green = (cell >> 5) & 0x3F;
                    const uint8_t blue = cell & 0x1F;
                    auto pixel = &pixels[offset_row + column * 3];
                    pixel[0] = (red << 3) | (red >> 2);
                    pixel[1] = (green << 2) | (green >> 4);
                    pixel[2] = (blue << 3) | (blue >> 2);
                }
                break;
            }
            case D3DFMT_X1R5G5B5:
            case D3DFMT_A1R5G5B5: {
                auto cells = reinterpret_cast<const uint16_t *>(data + row * pitch);
                for (size_t column = 0; column < width; column++) {
                    const uint16_t cell = cells[column];
                    const uint8_t red = (cell >> 10) & 0x1F;
                    const uint8_t green = (cell >> 5) & 0x1F;
                    const uint8_t blue = cell & 0x1F;
                    auto pixel = &pixels[offset_row + column * 3];
                    pixel[0] = (red << 3) | (red >> 2);
                    pixel[1] = (green << 3) | (green >> 2);
                    pixel[2] = (blue << 3) | (blue >> 2);
                }
                break;
            }
            case D3DFMT_A2R10G10B10: {
                auto cells = reinterpret_cast<const uint32_t *>(data + row * pitch);
                for (size_t column = 0; column < width; column++) {
                    const uint32_t cell = cells[column];
                    auto pixel = &pixels[offset_row + column * 3];
                    pixel[0] = static_cast<uint8_t>((cell >> 22) & 0xFF);
                    pixel[1] = static_cast<uint8_t>((cell >> 12) & 0xFF);
                    pixel[2] = static_cast<uint8_t>((cell >> 2) & 0xFF);
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
}

} // namespace

using d3d9_readback::BackbufferCopy;

static void save_capture(PendingCapture capture) {
    const auto size = compute_image_size(capture.width, capture.height, RGB_PIXEL_SIZE);
    if (!size.has_value()) {
        capture_buffers().give(std::move(capture.data));
        graphics_capture_skip(capture.screen);
        return;
    }

    auto pixels = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[size->total_size]);
    if (!pixels) {
        log_warning("graphics::d3d9", "failed to allocate capture image buffer");
        capture_buffers().give(std::move(capture.data));
        graphics_capture_skip(capture.screen);
        return;
    }

    // a format we cannot read still has to produce a frame, or api clients stall
    if (capture.data.empty()) {
        std::memset(pixels.get(), 0, size->total_size);
    } else {
        surface_to_rgb(
                capture.format,
                capture.width,
                capture.height,
                capture.data.data(),
                capture.pitch,
                pixels.get());

        capture_buffers().give(std::move(capture.data));
    }

    graphics_capture_enqueue(capture.screen, pixels.release(), capture.width, capture.height);
}

enum class SurfaceRead {
    Ok,
    Unsupported,
    Failed,
};

// copying the surface touches D3D, so it stays on the caller's thread. the bytes come
// out in the surface's own format; converting them is plain memory work for later.
// nothing here waits on the GPU: captures only arrive once the ring's copy fence has
// signalled that the DMA into this surface landed, and screenshots read a surface the
// present thread just finished filling.
static SurfaceRead read_surface_raw(
        const BackbufferCopy &copy,
        size_t &row_size,
        std::vector<uint8_t> &out) {

    const auto bytes_per_pixel = surface_pixel_size(copy.desc.Format);
    if (!bytes_per_pixel.has_value()) {
        static std::once_flag warned;
        std::call_once(warned, [&copy] {
            log_warning("graphics::d3d9",
                    "unsupported surface format {}",
                    static_cast<uint32_t>(copy.desc.Format));
        });
        return SurfaceRead::Unsupported;
    }

    const auto size = compute_image_size(copy.desc.Width, copy.desc.Height, *bytes_per_pixel);
    if (!size.has_value() || !resize_pixels(out, size->total_size)) {
        return SurfaceRead::Failed;
    }

    D3DLOCKED_RECT locked {};
    HRESULT hr = copy.surface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        log_warning("graphics::d3d9", "failed to lock capture surface, hr={}", FMT_HRESULT(hr));
        return SurfaceRead::Failed;
    }

    if (locked.Pitch < 0 || static_cast<size_t>(locked.Pitch) < size->row_size) {
        log_warning("graphics::d3d9", "capture surface has invalid pitch {}", locked.Pitch);
        copy.surface->UnlockRect();
        return SurfaceRead::Failed;
    }

    auto data = reinterpret_cast<const uint8_t *>(locked.pBits);
    for (size_t row = 0; row < copy.desc.Height; row++) {
        std::memcpy(
                out.data() + row * size->row_size,
                data + row * locked.Pitch,
                size->row_size);
    }

    hr = copy.surface->UnlockRect();
    if (FAILED(hr)) {
        log_warning("graphics::d3d9", "failed to unlock capture surface, hr={}", FMT_HRESULT(hr));
        return SurfaceRead::Failed;
    }

    row_size = size->row_size;
    return SurfaceRead::Ok;
}

static SurfaceRead read_capture_surface(
        const BackbufferCopy &copy,
        PendingCapture &capture) {

    capture.screen = copy.screen;
    capture.format = copy.desc.Format;
    capture.width = copy.desc.Width;
    capture.height = copy.desc.Height;

    capture.data = capture_buffers().take();
    const auto result = read_surface_raw(copy, capture.pitch, capture.data);
    if (result == SurfaceRead::Ok) {
        return result;
    }

    capture_buffers().give(std::move(capture.data));
    capture.data.clear();

    return result;
}

static bool write_screenshot_png(
        const std::string &file_path,
        UINT width,
        UINT height,
        const std::vector<uint8_t> &pixels) {

    // a no-op while FPNG_NO_SSE is set, but fpng requires it before any encode
    static std::once_flag fpng_ready;
    std::call_once(fpng_ready, [] { fpng::fpng_init(); });

    log_info("graphics::d3d9", "saving screenshot to {}", file_path);

    if (!fpng::fpng_encode_image_to_file(
            file_path.c_str(),
            pixels.data(),
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height),
            3)) {
        log_warning("graphics::d3d9", "failed to write screenshot png");
        return false;
    }

    return true;
}

// screen 0 keeps the plain name so existing tooling and the clipboard copy are unaffected
static std::string screenshot_path_for_screen(const std::string &primary_path, int screen) {
    if (screen == 0) {
        return primary_path;
    }

    const std::filesystem::path path(primary_path);
    return (path.parent_path() /
            fmt::format("{}_{}{}", path.stem().string(), screen, path.extension().string()))
            .string();
}

// games that crash or hang when the screenshot processor runs on another thread.
// D3DCREATE_MULTITHREADED is not a predictor of this; MDX omits it and threads fine
static bool image_processing_must_be_inline() {
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

    return THREAD_BAN.contains(avs::game::MODEL);
}

static void dispatch_capture_save(PendingCapture capture) {
    auto capture_process = [capture = std::move(capture)]() mutable {
        // an escape from here would cross a thread boundary and terminate
        try {
            save_capture(std::move(capture));
        } catch (const std::exception &error) {
            log_warning("graphics::d3d9", "capture save failed: {}", error.what());
        } catch (...) {
            log_warning("graphics::d3d9", "capture save failed");
        }
    };

    if (image_processing_must_be_inline()) {
        capture_process();
    } else {
        static auto pool = ThreadPool(2);
        pool.add(std::move(capture_process));
    }
}

// a capture waiting to be read out, one per screen; the ring never delivers a second one
// for a screen until this is cleared
static std::array<std::optional<BackbufferCopy>, GRAPHICS_CAPTURE_SCREEN_NO> CAPTURE_PENDING_LOCKS;

// set while a pool thread owns a screen's copy, so the present thread leaves that screen
// alone until it is done
static std::atomic<bool> CAPTURE_READ_IN_FLIGHT[GRAPHICS_CAPTURE_SCREEN_NO] {};

// reads the surface and hands the pixels on. the BackbufferCopy dies here too, which
// returns its surface to the pool - also a device call, so it belongs on whichever thread
// was cleared to do the read
static void read_and_dispatch_capture(int screen, BackbufferCopy copy) {
    const auto started = std::chrono::steady_clock::now();
    PendingCapture capture;
    const auto result = read_capture_surface(copy, capture);
    record_capture_read(std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - started).count());

    if (result == SurfaceRead::Failed) {
        graphics_capture_skip(screen);
        return;
    }

    // Ok and Unsupported (reported as a black frame) both dispatch normally
    dispatch_capture_save(std::move(capture));
}

// the read only leaves the present thread while a video stream holds the screen. that is
// the one case where the ~450us it costs per frame actually matters, and it is opt-in by
// nature - nobody is streaming unless they chose to. everything else keeps the inline path,
// notably the api capture module, which legacy companion apps poll for video on any game
// including ones whose device deadlocks under concurrent access (DDR X2, see 8b2f383).
// games already known to dislike threaded image processing are excluded outright.
static void try_deliver_capture(int screen) {
    if (CAPTURE_READ_IN_FLIGHT[screen]) {
        return;
    }

    auto &pending = CAPTURE_PENDING_LOCKS[screen];
    if (!pending.has_value()) {
        return;
    }

    auto copy = std::move(*pending);
    pending.reset();

    if (graphics_capture_continuous_active(screen) && !image_processing_must_be_inline()) {
        CAPTURE_READ_IN_FLIGHT[screen] = true;

        try {
            capture_read_pool().add([screen, copy = std::move(copy)]() mutable {
                // an escape from here would cross a thread boundary and terminate
                try {
                    read_and_dispatch_capture(screen, std::move(copy));
                } catch (const std::exception &error) {
                    log_warning("graphics::d3d9", "capture read failed: {}", error.what());
                    graphics_capture_skip(screen);
                } catch (...) {
                    log_warning("graphics::d3d9", "capture read failed");
                    graphics_capture_skip(screen);
                }
                CAPTURE_READ_IN_FLIGHT[screen] = false;
            });
            return;
        } catch (const std::exception &) {
            // nothing to queue onto; reading it here still makes progress
            CAPTURE_READ_IN_FLIGHT[screen] = false;
        }
    }

    read_and_dispatch_capture(screen, std::move(copy));
}

// by this point the pixels are plain memory, so none of this needs the device
static void dispatch_screenshot_save(std::vector<PendingWrite> writes, size_t screen_count) {
    auto screenshot_process = [writes = std::move(writes), screen_count]() mutable {
        std::lock_guard<std::mutex> lock(SCREENSHOT_SAVE_M);

        std::vector<int> screens;
        screens.reserve(writes.size());
        for (const auto &write : writes) {
            screens.push_back(write.screen);
        }

        const auto base_path = graphics_screenshot_genpath(screens);
        if (base_path.empty()) {
            return;
        }

        for (auto &write : writes) {
            write.path = screenshot_path_for_screen(base_path, write.screen);
        }

        // screens missing from writes either failed to be acquired or failed to read
        size_t failed = screen_count - writes.size();

        // a throw here would otherwise reach a thread boundary and terminate
        auto encode_one = [](PendingWrite &write) {
            try {
                const auto rgb = compute_image_size(write.width, write.height, RGB_PIXEL_SIZE);
                std::vector<uint8_t> pixels;
                if (!rgb.has_value() || !resize_pixels(pixels, rgb->total_size)) {
                    write.saved = false;
                    return;
                }

                surface_to_rgb(
                        write.format,
                        write.width,
                        write.height,
                        write.data.data(),
                        write.pitch,
                        pixels.data());

                // the encode below is the long part; the raw copy is dead by now
                write.data.clear();
                write.data.shrink_to_fit();

                write.saved = write_screenshot_png(
                        write.path, write.width, write.height, pixels);
            } catch (const std::exception &error) {
                log_warning("graphics::d3d9",
                        "screenshot encode failed for {}: {}", write.path, error.what());
                write.saved = false;
            } catch (...) {
                log_warning("graphics::d3d9",
                        "screenshot encode failed for {}", write.path);
                write.saved = false;
            }
        };

        {
            // sized up front and assigned by index: storing a future must not be able
            // to throw once its task is queued, or the screen would encode twice
            std::vector<std::future<void>> pending(writes.empty() ? 0 : writes.size() - 1);
            for (size_t i = 1; i < writes.size(); i++) {
                try {
                    pending[i - 1] = encode_pool().add([&writes, &encode_one, i] {
                        encode_one(writes[i]);
                    });
                } catch (const std::exception &) {
                    // nothing to queue onto; encoding it here still makes progress
                    encode_one(writes[i]);
                }
            }

            if (!writes.empty()) {
                encode_one(writes.front());
            }

            for (auto &task : pending) {
                if (task.valid()) {
                    task.wait();
                }
            }
        }

        std::string primary_path;
        std::string notify_path;
        for (const auto &write : writes) {
            if (!write.saved) {
                failed++;
                continue;
            }
            if (notify_path.empty()) {
                notify_path = write.path;
            }
            if (write.screen == 0) {
                primary_path = write.path;
            }
        }

        // only the primary screen goes to the clipboard, but any saved file is a success
        if (!primary_path.empty()) {
            clipboard::copy_image(primary_path);
        }
        if (!notify_path.empty()) {
            overlay::notifications::add(
                overlay::notifications::Severity::Success,
                fmt::format("Screenshot saved: {}", fileutils::basename(notify_path)));
        } else {
            overlay::notifications::add(
                overlay::notifications::Severity::Error,
                "Screenshot failed to save");
        }

        if (failed > 0) {
            log_warning("graphics::d3d9", "{} screenshot screen(s) missing", failed);
        }
    };

    // genpath and the path building below allocate, so an escape from here would
    // cross a thread boundary and terminate
    auto guarded = [process = std::move(screenshot_process)]() mutable {
        try {
            process();
        } catch (const std::exception &error) {
            log_warning("graphics::d3d9", "screenshot save failed: {}", error.what());
        } catch (...) {
            log_warning("graphics::d3d9", "screenshot save failed");
        }
    };

    if (image_processing_must_be_inline()) {
        guarded();
    } else {
        static auto pool = ThreadPool(2);
        pool.add(std::move(guarded));
    }
}

static void process_screenshot_request(
        IDirect3DDevice9 *device,
        WrappedIDirect3DDevice9 *wrapped_device) {

    std::vector<int> screens { 0 };
    if (GRAPHICS_SCREENSHOT_SUBSCREENS) {
        screens.clear();
        wrapped_device->get_screenshot_screens(screens);
    }

    std::vector<BackbufferCopy> copies;
    copies.reserve(screens.size());
    for (const int screen : screens) {
        IDirect3DSwapChain9 *swap_chain = nullptr;
        HRESULT hr = wrapped_device->get_screenshot_swap_chain(screen, &swap_chain);
        if (FAILED(hr) || swap_chain == nullptr) {
            log_warning("graphics::d3d9",
                    "failed to get swap chain for screen {}, hr={}",
                    screen,
                    FMT_HRESULT(hr));
            continue;
        }

        auto copy = d3d9_readback::acquire_backbuffer_copy(device, swap_chain, screen);
        swap_chain->Release();

        if (copy.has_value()) {
            copies.emplace_back(std::move(*copy));
        }
    }

    if (copies.empty()) {
        return;
    }

    // reading a surface touches the device, and doing that off the present thread
    // has been seen to deadlock games whose device has no internal locking
    std::vector<PendingWrite> writes;
    writes.reserve(copies.size());
    for (const auto &copy : copies) {
        PendingWrite write;
        write.screen = copy.screen;
        write.format = copy.desc.Format;
        write.width = copy.desc.Width;
        write.height = copy.desc.Height;

        if (read_surface_raw(copy, write.pitch, write.data) != SurfaceRead::Ok) {
            continue;
        }

        writes.push_back(std::move(write));
    }

    copies.clear();

    dispatch_screenshot_save(std::move(writes), screens.size());
}

void graphics_d3d9_process_screenshot(
        IDirect3DDevice9 *device,
        WrappedIDirect3DDevice9 *wrapped_device) {
    if (graphics_screenshot_consume()) {
        process_screenshot_request(device, wrapped_device);
    }
}

void graphics_d3d9_process_capture(
        IDirect3DDevice9 *device,
        WrappedIDirect3DDevice9 *wrapped_device) {

    // hand any capture left over from an earlier Present to whichever thread is allowed to
    // read it, before asking the ring whether anything new has shown up
    for (int screen = 0; screen < static_cast<int>(GRAPHICS_CAPTURE_SCREEN_NO); screen++) {
        try_deliver_capture(screen);
    }

    // pick up any submitted frame the ring has finished with; reading the pixels out of it
    // is try_deliver_capture's job, not this callback's
    d3d9_readback::poll_capture_ring(device,
            [](int screen, std::optional<BackbufferCopy> copy) {
        if (!copy.has_value()) {
            graphics_capture_skip(screen);
            return;
        }

        auto &pending = CAPTURE_PENDING_LOCKS[screen];
        if (pending.has_value() || CAPTURE_READ_IN_FLIGHT[screen]) {
            // already working on an older copy for this screen - should not happen given
            // the submit loop below gates on both, but do not clobber it if it somehow does
            graphics_capture_skip(screen);
            return;
        }

        pending = std::move(copy);
        try_deliver_capture(screen);
    });

    // while a client holds a screen, keep exactly one capture moving through the ring so a
    // fresh frame is normally already waiting by the time it's asked for, instead of the
    // on-demand round trip (several Present cycles of GPU queue depth, measured live at
    // ~20ms) being paid in full per delivered frame. resubmission only happens once the
    // previously delivered frame has actually been consumed: a fixed-interval clock here
    // drifted out of phase with the reader's own independent loop and, once behind, never
    // caught back up (measured live: capture wait crept back up to ~15ms with the odd
    // multi-second stall). gating on real consumption keeps production from ever running
    // more than one frame ahead of demand, which is also what keeps it from firing far more
    // often than any reader needs - that overrun previously cost the game real frame time.
    for (int screen = 0; screen < static_cast<int>(GRAPHICS_CAPTURE_SCREEN_NO); screen++) {
        if (d3d9_readback::has_pending_capture(screen)
                || CAPTURE_PENDING_LOCKS[screen].has_value()
                || CAPTURE_READ_IN_FLIGHT[screen]
                || graphics_capture_has_ready_frame(screen)) {
            continue;
        }

        const bool continuous = graphics_capture_continuous_active(screen);
        const bool requested = !continuous && graphics_capture_request_take(screen);
        if (!continuous && !requested) {
            continue;
        }

        IDirect3DSwapChain9 *swap_chain = nullptr;
        HRESULT hr = wrapped_device->get_screenshot_swap_chain(screen, &swap_chain);
        if (FAILED(hr) || swap_chain == nullptr) {
            // transient at worst (e.g. mode change); a continuous screen just tries again
            // next Present, but a one-shot caller is already blocked waiting on this
            if (requested) {
                graphics_capture_skip(screen);
            }
            continue;
        }

        if (!d3d9_readback::submit_capture(device, swap_chain, screen) && requested) {
            graphics_capture_skip(screen);
        }
        swap_chain->Release();
    }
}
