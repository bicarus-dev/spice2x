#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include <d3d9.h>

namespace d3d9_diag {

    std::string format_to_string(D3DFORMAT format);

    std::optional<std::string> diagnose_active_fullscreen_mode(
            IDirect3DDevice9 *device,
            size_t adapter_count,
            const D3DPRESENT_PARAMETERS *present_params,
            const D3DDISPLAYMODEEX *fullscreen_modes);

    void report_display_mode_diagnosis(const std::string &diagnosis);
}
