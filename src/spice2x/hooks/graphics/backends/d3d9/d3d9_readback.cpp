#include "d3d9_readback.h"

#include <array>
#include <chrono>
#include <mutex>
#include <vector>

#include "hooks/graphics/graphics.h"
#include "util/logging.h"

namespace d3d9_readback {

namespace {

SurfacePtr create_readback_surface(IDirect3DDevice9 *device, const D3DSURFACE_DESC &desc) {
    IDirect3DSurface9 *surface = nullptr;
    const HRESULT hr = device->CreateOffscreenPlainSurface(
            desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &surface, nullptr);

    if (FAILED(hr) || surface == nullptr) {
        log_warning("graphics::d3d9",
                "failed to create readback surface, hr={}",
                FMT_HRESULT(hr));
        return nullptr;
    }

    return SurfacePtr(surface);
}

size_t surface_bytes(const D3DSURFACE_DESC &desc) {
    size_t bytes_per_pixel = 4;
    switch (desc.Format) {
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
            bytes_per_pixel = 2;
            break;
        default:
            break;
    }

    return static_cast<size_t>(desc.Width) * desc.Height * bytes_per_pixel;
}

// idle surfaces are kept between captures, bucketed by layout so that screens of
// differing resolution do not evict each other. a new device drops everything,
// since system memory surfaces outlive Reset but not the device itself
class ReadbackPool {
public:
    SurfacePtr acquire(IDirect3DDevice9 *device, const D3DSURFACE_DESC &desc) {
        {
            std::lock_guard<std::mutex> lock(this->mutex);

            if (this->device != device) {
                this->drop();
                this->device = device;
            }

            auto *bucket = this->find(desc);
            if (bucket && !bucket->idle.empty()) {
                auto surface = std::move(bucket->idle.back());
                bucket->idle.pop_back();

                const size_t bytes = surface_bytes(desc);
                this->idle_bytes = this->idle_bytes > bytes ? this->idle_bytes - bytes : 0;
                return surface;
            }
        }

        return create_readback_surface(device, desc);
    }

    void release(IDirect3DDevice9 *device, SurfacePtr surface) {
        if (!surface) {
            return;
        }

        D3DSURFACE_DESC desc {};
        if (FAILED(surface->GetDesc(&desc))) {
            return;
        }

        const size_t bytes = surface_bytes(desc);

        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->device != device || this->idle_bytes + bytes > MAX_IDLE_BYTES) {
            return;
        }

        auto *bucket = this->find(desc);
        if (bucket == nullptr) {
            if (this->buckets.size() >= MAX_BUCKETS) {
                return;
            }

            this->buckets.push_back(Bucket { desc.Width, desc.Height, desc.Format, {} });
            bucket = &this->buckets.back();
        }

        if (bucket->idle.size() < MAX_IDLE_PER_BUCKET) {
            bucket->idle.push_back(std::move(surface));
            this->idle_bytes += bytes;
        }
    }

    // every cached surface holds a reference on the device, so they have to go
    // before it does or the device never reaches a zero reference count
    void clear_device(IDirect3DDevice9 *device) {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->device != device) {
            return;
        }

        this->drop();
        this->device = nullptr;
    }

private:
    struct Bucket {
        UINT width;
        UINT height;
        D3DFORMAT format;
        std::vector<SurfacePtr> idle;
    };

    static constexpr size_t MAX_BUCKETS = GRAPHICS_CAPTURE_SCREEN_NO;

    // one returning surface plus one for the next capture; a full screen surface
    // is several megabytes, so the cap matters
    static constexpr size_t MAX_IDLE_PER_BUCKET = 2;

    // a 4K surface is 33MB, so the per bucket count alone does not bound this
    static constexpr size_t MAX_IDLE_BYTES = 64u * 1024 * 1024;

    void drop() {
        this->buckets.clear();
        this->idle_bytes = 0;
    }

    Bucket *find(const D3DSURFACE_DESC &desc) {
        for (auto &bucket : this->buckets) {
            if (bucket.width == desc.Width
                    && bucket.height == desc.Height
                    && bucket.format == desc.Format) {
                return &bucket;
            }
        }

        return nullptr;
    }

    std::mutex mutex;
    std::vector<Bucket> buckets;
    IDirect3DDevice9 *device = nullptr;
    size_t idle_bytes = 0;
};

// deliberately never destroyed: releasing D3D surfaces during static destruction
// would run after d3d9 may already be unloaded
ReadbackPool &pool() {
    static ReadbackPool *instance = new ReadbackPool();
    return *instance;
}

struct QueryReleaser {
    void operator()(IDirect3DQuery9 *query) const {
        query->Release();
    }
};

using QueryPtr = std::unique_ptr<IDirect3DQuery9, QueryReleaser>;

// one in-flight GPU-side copy of a captured frame. Read back only once its StretchRect has
// actually finished, so GetRenderTargetData never has anything left to wait for.
struct CaptureSlot {
    SurfacePtr gpu_copy;    // D3DPOOL_DEFAULT, sized/formatted to match desc below
    QueryPtr fence;
    D3DSURFACE_DESC desc {};
    bool pending = false;
};

// the stream server waits for each frame before requesting the next, so one screen only
// ever has one capture actually in flight - a couple of spares is slack for the occasional
// frame where the GPU has not caught up to the previous copy yet
constexpr size_t CAPTURE_RING_SIZE = 3;

struct CaptureRing {
    std::array<CaptureSlot, CAPTURE_RING_SIZE> slots;
    size_t pending_count = 0;
};

std::array<CaptureRing, GRAPHICS_CAPTURE_SCREEN_NO> CAPTURE_RINGS;

// lightweight timing for the capture path, logged periodically so its actual per-phase
// cost is measured instead of guessed at. safe to delete once that question is answered.
struct PhaseStats {
    double total_us = 0.0;
    double max_us = 0.0;
    size_t count = 0;

    void add(double us) {
        total_us += us;
        max_us = std::max(max_us, us);
        count++;
    }

    double avg_us() const {
        return count ? total_us / static_cast<double>(count) : 0.0;
    }

    void reset() {
        total_us = 0.0;
        max_us = 0.0;
        count = 0;
    }
};

struct CaptureTimingStats {
    PhaseStats submit;    // GetBackBuffer + StretchRect + Issue, in submit_capture
    PhaseStats poll;      // each GetData(..., D3DGETDATA_FLUSH) call, in poll_capture_ring
    PhaseStats readback;  // GetRenderTargetData once a slot's fence has signalled
    std::chrono::steady_clock::time_point last_log = std::chrono::steady_clock::now();
};

CaptureTimingStats CAPTURE_TIMING;

void maybe_log_capture_timing() {
    const auto now = std::chrono::steady_clock::now();
    if (now - CAPTURE_TIMING.last_log < std::chrono::seconds(2)) {
        return;
    }
    CAPTURE_TIMING.last_log = now;

    if (CAPTURE_TIMING.submit.count == 0 && CAPTURE_TIMING.poll.count == 0
            && CAPTURE_TIMING.readback.count == 0) {
        return;
    }

    log_info("graphics::d3d9",
            "capture timing us (avg/max/n): submit {:.0f}/{:.0f}/{}, "
            "poll {:.0f}/{:.0f}/{}, readback {:.0f}/{:.0f}/{}",
            CAPTURE_TIMING.submit.avg_us(), CAPTURE_TIMING.submit.max_us, CAPTURE_TIMING.submit.count,
            CAPTURE_TIMING.poll.avg_us(), CAPTURE_TIMING.poll.max_us, CAPTURE_TIMING.poll.count,
            CAPTURE_TIMING.readback.avg_us(), CAPTURE_TIMING.readback.max_us, CAPTURE_TIMING.readback.count);

    CAPTURE_TIMING.submit.reset();
    CAPTURE_TIMING.poll.reset();
    CAPTURE_TIMING.readback.reset();
}

class ScopedTimer {
public:
    explicit ScopedTimer(PhaseStats &stats)
        : stats(stats), start(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
        stats.add(std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count());
    }

private:
    PhaseStats &stats;
    std::chrono::steady_clock::time_point start;
};

} // namespace

void release_device_resources(IDirect3DDevice9 *device) {
    pool().clear_device(device);

    // the ring's surfaces are D3DPOOL_DEFAULT and cannot survive Reset()/ResetEx(); recreated
    // lazily on the next submit_capture() either way, so dropping them here is enough
    for (auto &ring : CAPTURE_RINGS) {
        for (auto &slot : ring.slots) {
            slot.gpu_copy.reset();
            slot.fence.reset();
            slot.pending = false;
        }
        ring.pending_count = 0;
    }
}

BackbufferCopy::~BackbufferCopy() {
    if (this->pooled && this->surface) {
        pool().release(this->device, std::move(this->surface));
    }
}

std::optional<BackbufferCopy> acquire_backbuffer_copy(
        IDirect3DDevice9 *device, IDirect3DSwapChain9 *swap_chain, int screen) {

    IDirect3DSurface9 *buffer = nullptr;
    HRESULT hr = swap_chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &buffer);
    if (FAILED(hr) || buffer == nullptr) {
        log_warning("graphics::d3d9",
                "failed to get back buffer for screen {}, hr={}",
                screen,
                FMT_HRESULT(hr));
        return std::nullopt;
    }

    D3DSURFACE_DESC desc {};
    hr = buffer->GetDesc(&desc);
    if (FAILED(hr)) {
        log_warning("graphics::d3d9",
                "failed to acquire back buffer descriptor, hr={}",
                FMT_HRESULT(hr));
        buffer->Release();
        return std::nullopt;
    }

    // GetRenderTargetData rejects multisampled sources. no supported game has been
    // seen presenting one, so resolving is left unimplemented rather than untested
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        static std::once_flag warned;
        std::call_once(warned, [&desc] {
            log_warning("graphics::d3d9",
                    "back buffer is multisampled ({}), screenshots and capture are unsupported",
                    static_cast<uint32_t>(desc.MultiSampleType));
        });
        buffer->Release();
        return std::nullopt;
    }

    auto destination = create_readback_surface(device, desc);
    if (!destination) {
        buffer->Release();
        return std::nullopt;
    }

    hr = device->GetRenderTargetData(buffer, destination.get());
    buffer->Release();

    if (FAILED(hr)) {
        log_warning("graphics::d3d9",
                "failed to copy back buffer contents, hr={}",
                FMT_HRESULT(hr));
        return std::nullopt;
    }

    BackbufferCopy copy;
    copy.screen = screen;
    copy.desc = desc;
    copy.device = device;
    copy.surface = std::move(destination);
    copy.pooled = false;

    return copy;
}

bool submit_capture(IDirect3DDevice9 *device, IDirect3DSwapChain9 *swap_chain, int screen) {
    if (screen < 0 || screen >= static_cast<int>(GRAPHICS_CAPTURE_SCREEN_NO)) {
        return false;
    }

    auto &ring = CAPTURE_RINGS[screen];

    CaptureSlot *slot = nullptr;
    for (auto &candidate : ring.slots) {
        if (!candidate.pending) {
            slot = &candidate;
            break;
        }
    }
    if (!slot) {
        return false;
    }

    ScopedTimer timer(CAPTURE_TIMING.submit);

    IDirect3DSurface9 *buffer = nullptr;
    HRESULT hr = swap_chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &buffer);
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
                "failed to acquire back buffer descriptor, hr={}",
                FMT_HRESULT(hr));
        buffer->Release();
        return false;
    }

    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        static std::once_flag warned;
        std::call_once(warned, [&desc] {
            log_warning("graphics::d3d9",
                    "back buffer is multisampled ({}), screenshots and capture are unsupported",
                    static_cast<uint32_t>(desc.MultiSampleType));
        });
        buffer->Release();
        return false;
    }

    // recreated whenever the back buffer's size/format changes; safe here since a free slot
    // can never have a copy still in flight
    if (!slot->gpu_copy || slot->desc.Width != desc.Width || slot->desc.Height != desc.Height
            || slot->desc.Format != desc.Format) {
        IDirect3DSurface9 *target = nullptr;
        hr = device->CreateRenderTarget(
                desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr);
        if (FAILED(hr) || target == nullptr) {
            log_warning("graphics::d3d9",
                    "failed to create capture ring surface, hr={}", FMT_HRESULT(hr));
            buffer->Release();
            return false;
        }
        slot->gpu_copy.reset(target);
    }

    if (!slot->fence) {
        IDirect3DQuery9 *query = nullptr;
        hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &query);
        if (FAILED(hr) || query == nullptr) {
            log_warning("graphics::d3d9",
                    "failed to create capture fence, hr={}", FMT_HRESULT(hr));
            buffer->Release();
            return false;
        }
        slot->fence.reset(query);
    }

    // GPU-to-GPU: this only ever queues a command, it never waits on anything
    hr = device->StretchRect(buffer, nullptr, slot->gpu_copy.get(), nullptr, D3DTEXF_NONE);
    buffer->Release();

    if (FAILED(hr)) {
        log_warning("graphics::d3d9", "failed to queue capture copy, hr={}", FMT_HRESULT(hr));
        return false;
    }

    slot->fence->Issue(D3DISSUE_END);
    slot->desc = desc;
    slot->pending = true;
    ring.pending_count++;

    return true;
}

void poll_capture_ring(IDirect3DDevice9 *device, const CaptureReadyFn &on_ready) {
    for (size_t screen = 0; screen < CAPTURE_RINGS.size(); screen++) {
        auto &ring = CAPTURE_RINGS[screen];

        // an idle screen - nothing ever requested, or nothing outstanding right now - costs
        // exactly this one comparison: no D3D calls, no locks
        if (ring.pending_count == 0) {
            continue;
        }

        for (auto &slot : ring.slots) {
            if (!slot.pending) {
                continue;
            }

            // D3DGETDATA_FLUSH kicks the command buffer so the query is actually seen by the
            // GPU; it does not block - S_FALSE just means "still working on it"
            HRESULT hr;
            {
                ScopedTimer timer(CAPTURE_TIMING.poll);
                hr = slot.fence->GetData(nullptr, 0, D3DGETDATA_FLUSH);
            }
            if (hr == S_FALSE) {
                continue;
            }

            slot.pending = false;
            ring.pending_count--;

            if (FAILED(hr)) {
                on_ready(static_cast<int>(screen), std::nullopt);
                continue;
            }

            // the copy finished rendering frames ago by this point, so unlike the direct
            // path this GetRenderTargetData has nothing left to wait for
            auto destination = pool().acquire(device, slot.desc);
            if (!destination) {
                on_ready(static_cast<int>(screen), std::nullopt);
                continue;
            }

            const HRESULT copy_hr = [&] {
                ScopedTimer timer(CAPTURE_TIMING.readback);
                return device->GetRenderTargetData(slot.gpu_copy.get(), destination.get());
            }();
            if (FAILED(copy_hr)) {
                log_warning("graphics::d3d9",
                        "failed to copy capture ring surface, hr={}", FMT_HRESULT(copy_hr));
                pool().release(device, std::move(destination));
                on_ready(static_cast<int>(screen), std::nullopt);
                continue;
            }

            BackbufferCopy copy;
            copy.screen = static_cast<int>(screen);
            copy.desc = slot.desc;
            copy.device = device;
            copy.surface = std::move(destination);
            copy.pooled = true;

            on_ready(static_cast<int>(screen), std::move(copy));

            // only one result per screen per call - matches how often the caller consumes
            break;
        }
    }

    maybe_log_capture_timing();
}
}
