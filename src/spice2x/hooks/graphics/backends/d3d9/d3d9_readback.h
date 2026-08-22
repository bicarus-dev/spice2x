#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <d3d9.h>

namespace d3d9_readback {

    struct SurfaceReleaser {
        void operator()(IDirect3DSurface9 *surface) const {
            surface->Release();
        }
    };

    using SurfacePtr = std::unique_ptr<IDirect3DSurface9, SurfaceReleaser>;

    // system memory copy of a back buffer; locking it neither stalls the GPU nor reads over PCIe
    struct BackbufferCopy {
        int screen {};
        D3DSURFACE_DESC desc {};
        IDirect3DDevice9 *device = nullptr;
        SurfacePtr surface;
        bool pooled = false;

        BackbufferCopy() = default;
        BackbufferCopy(BackbufferCopy &&) noexcept = default;
        BackbufferCopy &operator=(BackbufferCopy &&) noexcept = default;
        BackbufferCopy(const BackbufferCopy &) = delete;
        BackbufferCopy &operator=(const BackbufferCopy &) = delete;
        ~BackbufferCopy();
    };

    // screenshots are rare one-offs, so this always does a plain (non-pooled) copy
    std::optional<BackbufferCopy> acquire_backbuffer_copy(
            IDirect3DDevice9 *device,
            IDirect3DSwapChain9 *swap_chain,
            int screen);

    // called once per screen when a submitted capture is either ready (with pixel data) or
    // has definitively failed; never called for a slot that is still waiting on the GPU
    using CaptureReadyFn = std::function<void(int screen, std::optional<BackbufferCopy> copy)>;

    // queues a GPU-side copy (StretchRect) of the current back buffer for `screen` and returns
    // immediately - never touches system memory or blocks on the GPU. false means every ring
    // slot for this screen is still waiting on a previous copy; treat that exactly like a
    // failed capture (skip it), never fall back to a blocking read - streaming can afford to
    // miss a frame here, the game's frame time cannot.
    bool submit_capture(IDirect3DDevice9 *device, IDirect3DSwapChain9 *swap_chain, int screen);

    // true while `screen` has a capture anywhere in the ring (either stage). continuous
    // callers use this to keep at most one in flight rather than racing to fill every slot -
    // each one is a real StretchRect plus a real VRAM-to-system-RAM DMA transfer, and three of
    // those running back to back competes with the game's own GPU/PCIe usage for no benefit,
    // since nothing consumes completions faster than one at a time anyway.
    bool has_pending_capture(int screen);

    // delivers at most one finished capture per screen per call, reading it back only now
    // that its GPU-side copy has actually finished rendering (so GetRenderTargetData has
    // nothing left to wait for). a screen with nothing outstanding costs a single integer
    // comparison, so this is safe to call unconditionally every Present even when idle.
    void poll_capture_ring(IDirect3DDevice9 *device, const CaptureReadyFn &on_ready);

    // pooled surfaces (and the capture ring's GPU-side ones) hold references on the device -
    // this must run before releasing it, and before Reset()/ResetEx() since the ring's
    // D3DPOOL_DEFAULT surfaces cannot survive either
    void release_device_resources(IDirect3DDevice9 *device);
}

