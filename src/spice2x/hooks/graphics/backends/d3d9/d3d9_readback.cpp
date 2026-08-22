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

// one in-flight copy of a captured frame, moving through two independently-fenced stages:
// GPU render -> GPU ring surface (StretchRect, fenced by render_fence), then GPU ring
// surface -> system memory (GetRenderTargetData, fenced by copy_fence). Measured live that
// GetRenderTargetData returns almost immediately without the DMA transfer actually being
// done - LockRect blocked on it later regardless of D3DLOCK_DONOTWAIT - so the copy needs
// its own fence rather than assuming GetRenderTargetData's return means the data has landed.
enum class SlotStage {
    Idle,
    RenderPending,  // waiting on render_fence: has the StretchRect into gpu_copy finished?
    CopyPending,    // waiting on copy_fence: has GetRenderTargetData's DMA into sysmem_copy landed?
};

struct CaptureSlot {
    SurfacePtr gpu_copy;    // D3DPOOL_DEFAULT, sized/formatted to match desc below
    QueryPtr render_fence;
    D3DSURFACE_DESC desc {};

    SurfacePtr sysmem_copy; // pooled D3DPOOL_SYSTEMMEM, only held between the two stages
    QueryPtr copy_fence;

    SlotStage stage = SlotStage::Idle;
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
    PhaseStats submit;      // GetBackBuffer + StretchRect + Issue, in submit_capture
    PhaseStats render_poll; // each render_fence GetData call, in poll_capture_ring
    PhaseStats readback;    // GetRenderTargetData once render_fence has signalled
    PhaseStats copy_poll;   // each copy_fence GetData call, in poll_capture_ring
    std::chrono::steady_clock::time_point last_log = std::chrono::steady_clock::now();
};

CaptureTimingStats CAPTURE_TIMING;

void maybe_log_capture_timing() {
    const auto now = std::chrono::steady_clock::now();
    if (now - CAPTURE_TIMING.last_log < std::chrono::seconds(2)) {
        return;
    }
    CAPTURE_TIMING.last_log = now;

    if (CAPTURE_TIMING.submit.count == 0 && CAPTURE_TIMING.render_poll.count == 0
            && CAPTURE_TIMING.readback.count == 0 && CAPTURE_TIMING.copy_poll.count == 0) {
        return;
    }

    log_info("graphics::d3d9",
            "capture timing us (avg/max/n): submit {:.0f}/{:.0f}/{}, "
            "render_poll {:.0f}/{:.0f}/{}, readback {:.0f}/{:.0f}/{}, copy_poll {:.0f}/{:.0f}/{}",
            CAPTURE_TIMING.submit.avg_us(), CAPTURE_TIMING.submit.max_us, CAPTURE_TIMING.submit.count,
            CAPTURE_TIMING.render_poll.avg_us(), CAPTURE_TIMING.render_poll.max_us, CAPTURE_TIMING.render_poll.count,
            CAPTURE_TIMING.readback.avg_us(), CAPTURE_TIMING.readback.max_us, CAPTURE_TIMING.readback.count,
            CAPTURE_TIMING.copy_poll.avg_us(), CAPTURE_TIMING.copy_poll.max_us, CAPTURE_TIMING.copy_poll.count);

    CAPTURE_TIMING.submit.reset();
    CAPTURE_TIMING.render_poll.reset();
    CAPTURE_TIMING.readback.reset();
    CAPTURE_TIMING.copy_poll.reset();
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
    // lazily on the next submit_capture() either way, so dropping them here is enough. any
    // sysmem_copy mid-flight is just released directly - the pool itself is being cleared
    // above regardless, so there is nowhere to return it to.
    for (auto &ring : CAPTURE_RINGS) {
        for (auto &slot : ring.slots) {
            slot.gpu_copy.reset();
            slot.render_fence.reset();
            slot.sysmem_copy.reset();
            slot.copy_fence.reset();
            slot.stage = SlotStage::Idle;
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
        if (candidate.stage == SlotStage::Idle) {
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

    if (!slot->render_fence) {
        IDirect3DQuery9 *query = nullptr;
        hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &query);
        if (FAILED(hr) || query == nullptr) {
            log_warning("graphics::d3d9",
                    "failed to create capture fence, hr={}", FMT_HRESULT(hr));
            buffer->Release();
            return false;
        }
        slot->render_fence.reset(query);
    }

    // GPU-to-GPU: this only ever queues a command, it never waits on anything
    hr = device->StretchRect(buffer, nullptr, slot->gpu_copy.get(), nullptr, D3DTEXF_NONE);
    buffer->Release();

    if (FAILED(hr)) {
        log_warning("graphics::d3d9", "failed to queue capture copy, hr={}", FMT_HRESULT(hr));
        return false;
    }

    slot->render_fence->Issue(D3DISSUE_END);
    slot->desc = desc;
    slot->stage = SlotStage::RenderPending;
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
            if (slot.stage == SlotStage::CopyPending) {
                // D3DGETDATA_FLUSH kicks the command buffer so the query is actually seen by
                // the GPU; it does not block - S_FALSE just means "still working on it"
                HRESULT hr;
                {
                    ScopedTimer timer(CAPTURE_TIMING.copy_poll);
                    hr = slot.copy_fence->GetData(nullptr, 0, D3DGETDATA_FLUSH);
                }
                if (hr == S_FALSE) {
                    continue;
                }

                slot.stage = SlotStage::Idle;
                ring.pending_count--;

                if (FAILED(hr)) {
                    pool().release(device, std::move(slot.sysmem_copy));
                    on_ready(static_cast<int>(screen), std::nullopt);
                    continue;
                }

                BackbufferCopy copy;
                copy.screen = static_cast<int>(screen);
                copy.desc = slot.desc;
                copy.device = device;
                copy.surface = std::move(slot.sysmem_copy);
                copy.pooled = true;

                on_ready(static_cast<int>(screen), std::move(copy));

                // only one result per screen per call - matches how often the caller consumes
                break;
            }

            if (slot.stage != SlotStage::RenderPending) {
                continue;
            }

            HRESULT hr;
            {
                ScopedTimer timer(CAPTURE_TIMING.render_poll);
                hr = slot.render_fence->GetData(nullptr, 0, D3DGETDATA_FLUSH);
            }
            if (hr == S_FALSE) {
                continue;
            }

            if (FAILED(hr)) {
                slot.stage = SlotStage::Idle;
                ring.pending_count--;
                on_ready(static_cast<int>(screen), std::nullopt);
                continue;
            }

            // the render finished by this point, so unlike a direct GetRenderTargetData
            // call this one has nothing to wait for on the render side. its own DMA
            // transfer can still be in flight though - that's what copy_fence is for.
            auto destination = pool().acquire(device, slot.desc);
            if (!destination) {
                slot.stage = SlotStage::Idle;
                ring.pending_count--;
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
                slot.stage = SlotStage::Idle;
                ring.pending_count--;
                on_ready(static_cast<int>(screen), std::nullopt);
                continue;
            }

            if (!slot.copy_fence) {
                IDirect3DQuery9 *query = nullptr;
                const HRESULT query_hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &query);
                if (FAILED(query_hr) || query == nullptr) {
                    log_warning("graphics::d3d9",
                            "failed to create capture copy fence, hr={}", FMT_HRESULT(query_hr));
                    pool().release(device, std::move(destination));
                    slot.stage = SlotStage::Idle;
                    ring.pending_count--;
                    on_ready(static_cast<int>(screen), std::nullopt);
                    continue;
                }
                slot.copy_fence.reset(query);
            }

            slot.copy_fence->Issue(D3DISSUE_END);
            slot.sysmem_copy = std::move(destination);
            slot.stage = SlotStage::CopyPending;
            // still pending overall - just moved to the second stage, so pending_count is
            // deliberately left unchanged here
        }
    }

    maybe_log_capture_timing();
}
}
