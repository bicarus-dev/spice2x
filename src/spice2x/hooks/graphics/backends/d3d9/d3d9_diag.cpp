#include "d3d9_diag.h"

#include "util/deferlog.h"
#include "util/flags_helper.h"
#include "util/logging.h"

namespace d3d9_diag {

    // converts a D3D format value to a readable name for diagnostics
    std::string format_to_string(D3DFORMAT format) {
        switch (format) {
            ENUM_VARIANT(D3DFMT_UNKNOWN);
            ENUM_VARIANT(D3DFMT_R8G8B8);
            ENUM_VARIANT(D3DFMT_A8R8G8B8);
            ENUM_VARIANT(D3DFMT_X8R8G8B8);
            ENUM_VARIANT(D3DFMT_R5G6B5);
            ENUM_VARIANT(D3DFMT_X1R5G5B5);
            ENUM_VARIANT(D3DFMT_A1R5G5B5);
            ENUM_VARIANT(D3DFMT_A4R4G4B4);
            ENUM_VARIANT(D3DFMT_R3G3B2);
            ENUM_VARIANT(D3DFMT_A8);
            ENUM_VARIANT(D3DFMT_A8R3G3B2);
            ENUM_VARIANT(D3DFMT_X4R4G4B4);
            ENUM_VARIANT(D3DFMT_A2B10G10R10);
            ENUM_VARIANT(D3DFMT_A8B8G8R8);
            ENUM_VARIANT(D3DFMT_X8B8G8R8);
            ENUM_VARIANT(D3DFMT_G16R16);
            ENUM_VARIANT(D3DFMT_A2R10G10B10);
            ENUM_VARIANT(D3DFMT_A16B16G16R16);
            ENUM_VARIANT(D3DFMT_A8P8);
            ENUM_VARIANT(D3DFMT_P8);
            ENUM_VARIANT(D3DFMT_L8);
            ENUM_VARIANT(D3DFMT_A8L8);
            ENUM_VARIANT(D3DFMT_A4L4);
            ENUM_VARIANT(D3DFMT_V8U8);
            ENUM_VARIANT(D3DFMT_L6V5U5);
            ENUM_VARIANT(D3DFMT_X8L8V8U8);
            ENUM_VARIANT(D3DFMT_Q8W8V8U8);
            ENUM_VARIANT(D3DFMT_V16U16);
            ENUM_VARIANT(D3DFMT_A2W10V10U10);
            ENUM_VARIANT(D3DFMT_UYVY);
            ENUM_VARIANT(D3DFMT_YUY2);
            ENUM_VARIANT(D3DFMT_DXT1);
            ENUM_VARIANT(D3DFMT_DXT2);
            ENUM_VARIANT(D3DFMT_DXT3);
            ENUM_VARIANT(D3DFMT_DXT4);
            ENUM_VARIANT(D3DFMT_DXT5);
            ENUM_VARIANT(D3DFMT_MULTI2_ARGB8);
            ENUM_VARIANT(D3DFMT_G8R8_G8B8);
            ENUM_VARIANT(D3DFMT_R8G8_B8G8);
            ENUM_VARIANT(D3DFMT_D16_LOCKABLE);
            ENUM_VARIANT(D3DFMT_D32);
            ENUM_VARIANT(D3DFMT_D15S1);
            ENUM_VARIANT(D3DFMT_D24S8);
            ENUM_VARIANT(D3DFMT_D24X8);
            ENUM_VARIANT(D3DFMT_D24X4S4);
            ENUM_VARIANT(D3DFMT_D16);
            ENUM_VARIANT(D3DFMT_L16);
            ENUM_VARIANT(D3DFMT_D32F_LOCKABLE);
            ENUM_VARIANT(D3DFMT_D24FS8);
            ENUM_VARIANT(D3DFMT_D32_LOCKABLE);
            ENUM_VARIANT(D3DFMT_S8_LOCKABLE);
            ENUM_VARIANT(D3DFMT_VERTEXDATA);
            ENUM_VARIANT(D3DFMT_INDEX16);
            ENUM_VARIANT(D3DFMT_INDEX32);
            ENUM_VARIANT(D3DFMT_Q16W16V16U16);
            ENUM_VARIANT(D3DFMT_R16F);
            ENUM_VARIANT(D3DFMT_G16R16F);
            ENUM_VARIANT(D3DFMT_A16B16G16R16F);
            ENUM_VARIANT(D3DFMT_R32F);
            ENUM_VARIANT(D3DFMT_G32R32F);
            ENUM_VARIANT(D3DFMT_A32B32G32R32F);
            ENUM_VARIANT(D3DFMT_CxV8U8);
            ENUM_VARIANT(D3DFMT_A1);
            ENUM_VARIANT(D3DFMT_A2B10G10R10_XR_BIAS);
            ENUM_VARIANT(D3DFMT_BINARYBUFFER);
            default:
                return fmt::to_string(static_cast<uint32_t>(format));
        }
    }

    // verifies that created fullscreen swap chains entered their requested modes
    std::optional<std::string> diagnose_active_fullscreen_mode(
            IDirect3DDevice9 *device,
            size_t adapter_count,
            const D3DPRESENT_PARAMETERS *present_params,
            const D3DDISPLAYMODEEX *fullscreen_modes)
    {
        if (device == nullptr || present_params == nullptr || adapter_count == 0) {
            return std::nullopt;
        }

        for (size_t head = 0; head < adapter_count; head++) {
            if (present_params[head].Windowed) {
                continue;
            }

            // query the mode that each created swap chain actually entered
            D3DDISPLAYMODE active_mode {};
            const HRESULT hr = device->GetDisplayMode(head, &active_mode);
            if (FAILED(hr)) {
                log_warning(
                        "graphics::d3d9",
                        "failed to query active display mode for swap chain {}, hr={}",
                        head,
                        FMT_HRESULT(hr));
                continue;
            }

            // Ex calls carry the requested mode separately from presentation parameters
            const UINT expected_width = fullscreen_modes
                    ? fullscreen_modes[head].Width
                    : present_params[head].BackBufferWidth;
            const UINT expected_height = fullscreen_modes
                    ? fullscreen_modes[head].Height
                    : present_params[head].BackBufferHeight;
            const UINT expected_refresh = fullscreen_modes
                    ? fullscreen_modes[head].RefreshRate
                    : present_params[head].FullScreen_RefreshRateInHz;

            log_info(
                    "graphics::d3d9",
                    "active display mode for swap chain {}: {}x{} @ {} Hz, {}",
                    head,
                    active_mode.Width,
                    active_mode.Height,
                    active_mode.RefreshRate,
                    format_to_string(active_mode.Format));

            const bool resolution_mismatch =
                active_mode.Width != expected_width || active_mode.Height != expected_height;

            // drivers may report a nominal or rounded refresh rate even when presentation works
            if (resolution_mismatch) {
                return fmt::format(
                        "fullscreen display {} did not enter the requested mode; "
                        "requested {}x{} @ {} Hz, active {}x{} @ {} Hz",
                        head,
                        expected_width,
                        expected_height,
                        expected_refresh,
                        active_mode.Width,
                        active_mode.Height,
                        active_mode.RefreshRate);
            }
        }

        return std::nullopt;
    }

    // reports an active-mode mismatch through immediate and deferred logging
    void report_display_mode_diagnosis(const std::string &diagnosis) {
        log_warning("graphics::d3d9", "{}", diagnosis);
        deferredlogs::defer_error_messages({
            "fullscreen display mode mismatch detected after D3D9 device creation",
            fmt::format("    {}", diagnosis),
            "    if the game launched with white screens, this is likely the cause",
            "    your monitor(s) do not support the requested resolution and refresh rate",
            "    things to try:",
            "      * check monitor rotation",
            "      * enable GPU-side resolution scaling",
            "      * run the game in windowed mode",
            "      * for subscreen games, disable the subscreen and use the overlay instead",
            "      * for subscreen games, use -forceressub and -graphics-force-refresh-sub",
        });
    }
}
