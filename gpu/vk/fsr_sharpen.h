// The FSR sharpening row's strength for AMD's library (vk_ffx3.cpp, vk_fsr4.cpp: ffxDispatchDescUpscale's enableSharpening and sharpness),
// as plain functions: no Vulkan, no library. gpu/vk/tests/fsr_sharpen_test.cpp checks them. Ported from NX2navi's NX2NAVI_SHARPNESS
// (vk_ffx.cpp, vk_ffx4.cpp, vk_dlaa_fsr3_boost): the slider 0 to 100 is FSR's own sharpness 0 to 1 (0 off), raised towards 1 as the 3D
// gets smaller than the output, where FSR's upscale is softest.
#pragma once

#include <cstdint>

namespace mkwvk::fsr_sharpen {

/// How much of the extra sharpening a render width takes (ow: the output's): none from three quarters of the output's width up, all of it
/// from half (Performance) down
inline float boost(uint32_t render_w, uint32_t out_w) {
    const double share = out_w ? (double)render_w / out_w : 1.0, k = (0.75 - share) / 0.25;
    return (float)(k < 0 ? 0 : k > 1 ? 1 : k);
}
/// The row (opts.fsr_sharpness, 0 to 100) asks for FSR's sharpening: 0 is off, the library's sharpening is then not enabled at all
inline bool enabled(int row) { return row > 0; }
/// The sharpness handed to the library (0 to 1): the row's percent, then raised towards 1 by the boost of this render size
inline float strength(int row, uint32_t render_w, uint32_t out_w) {
    if (row <= 0) return 0.0f;
    float s = row > 100 ? 1.0f : row / 100.0f;
    s += (1.0f - s) * boost(render_w, out_w);
    return s > 1.0f ? 1.0f : s;
}

}  // namespace mkwvk::fsr_sharpen
