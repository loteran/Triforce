// (Triforce) The Sharpness row's rules, and the texture LOD bias that goes with a temporal upscaler, as plain functions: no
// Vulkan, no menu. tests/test_sharpen_rules.py compiles and runs them.
//
// One Sharpness value (opts.sharpness, 0 to 100) sharpens the 3D before the HUD where the upscaler can: AMD FSR 3.1 with its
// own RCAS pass (vk_fsr3.cpp), AMD FSR 4 with its own sharpening (vk_fsr4.cpp), NVIDIA DLSS and DLAA with AMD FSR 3.1's RCAS
// pass over their picture (vk_temporal.cpp). The window pass (vk_framegen.cpp framegen_scale) then sharpens nothing: never
// twice, and not the HUD and text. With the game's own upscaler it sharpens the finished picture at the window, as before.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace mkwvk::sharpen_rules {

/// The slider as the upscalers' sharpness, 0 (none) to 1 (the most)
inline float amount(int slider) { return (float)std::clamp(slider, 0, 100) / 100.0f; }

/// (NX2navi 385734d) FSR 3.1 is soft below three quarters of the output's width (its Performance and Ultra Performance: 60%
/// and 49% of its Native AA's fine detail, measured there): the share of extra sharpening, none at 3/4, all of it at 1/2
inline float fsr3_boost(uint32_t render_w, uint32_t output_w) {
    if (!render_w || !output_w) return 0.0f;
    return std::clamp((0.75f - (float)render_w / (float)output_w) / 0.25f, 0.0f, 1.0f);
}
/// FSR 3.1's own sharpness: the slider's, that share of the way to the most. 0 stays 0 (the slider at 0 is off); boost false
/// (RX_VK_FSR3_BOOST=0): the slider's alone
inline float fsr3_sharpness(float s, uint32_t render_w, uint32_t output_w, bool boost = true) {
    s = std::clamp(s, 0.0f, 1.0f);
    if (s <= 0.0f || !boost) return s;
    return s + (1.0f - s) * fsr3_boost(render_w, output_w);
}

/// RCAS's strength in stops below its most (ffx_fsr1.h FsrRcasCon: 0 the most, each stop half), from a sharpness of 0 to 1
/// as AMD's FSR 2 and 3.1 hosts remap theirs (ffx_fsr3upscaler.cpp: sharpenessRemapped = -2 * sharpness + 2): 1 is 0
/// stops, 0 is 2 stops
inline float rcas_stops(float s) { return 2.0f - 2.0f * std::clamp(s, 0.0f, 1.0f); }

/// A float as IEEE half (round to nearest; the values here are 0.25..1)
inline uint16_t half_bits(float f) {
    uint32_t x;
    std::memcpy(&x, &f, sizeof x);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t e = (int32_t)((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) return (uint16_t)sign;            // (too small: 0)
    if (e >= 31) return (uint16_t)(sign | 0x7c00u);
    m += 0x1000u;                                  // (round the 13 bits dropped)
    if (m & 0x800000u) return (uint16_t)(sign | (uint32_t)((e + 1) << 10));
    return (uint16_t)(sign | (uint32_t)(e << 10) | (m >> 13));
}
/// cbRCAS's rcasConfig for a sharpness, exactly as ffx_fsr1.h FsrRcasCon writes it for its stops
inline void rcas_config(float s, uint32_t con[4]) {
    const float v = std::exp2(-rcas_stops(s));
    std::memcpy(&con[0], &v, sizeof v);
    const uint32_t h = half_bits(v);
    con[1] = h | (h << 16);
    con[2] = con[3] = 0;
}

/// The window pass's Sharpness: none when the upscaler sharpened the 3D before the HUD (before_hud), else the slider's
inline int window_sharpness(int slider, bool before_hud) { return before_hud ? 0 : std::clamp(slider, 0, 100); }

/// ((Triforce) Whether AMD's RCAS alone (vk_fsr3.cpp fsr3::rcas, its exposure fixed at 1) may sharpen NVIDIA DLSS's or
/// DLAA's picture before the HUD: only the tone mapped, LDR picture (the upscale network's input and output). RCAS's limiter
/// takes 1.0 for the peak (ffx_fsr1.h: peakC (1, -4), hitMax = (1 - mx4) / (4 mn4 - 4)): over linear HDR light, before the
/// bloom (before_bloom) or RenoDX's RGBA16F scene (hdr_scene), an edge across 1.0 gets no sharpening and a highlight whose
/// neighbours are all over 1 gets the most there is, rings round bright sky, fire and speculars, and the bloom is built from
/// that. There the window pass sharpens the finished picture instead (temporal_sharpens_3d false). AMD FSR 3.1's own RCAS
/// divides by its exposure first, and FSR 4 sharpens in its own HDR context: neither is this
inline bool lone_rcas(bool before_bloom, bool hdr_scene) { return !before_bloom && !hdr_scene; }

/// (S4) AMD FSR 1's EASU only enlarges: the window larger than the picture on one axis at least and smaller on neither
inline bool fsr1_enlarges(uint32_t iw, uint32_t ih, uint32_t dw, uint32_t dh) {
    return dw >= iw && dh >= ih && (dw > iw || dh > ih);
}
/// (S4, S6) The Window scaling filter's AMD FSR 1 (3) where it cannot run: at 1:1 the plain copy (0), else Catmull-Rom (1),
/// with the Sharpness the other filters have. fsr1: its path is taken (EASU, and RCAS at a Sharpness above 0)
inline int window_mode(int upscale, bool fsr1, uint32_t iw, uint32_t ih, uint32_t dw, uint32_t dh) {
    if (upscale != 3 || fsr1) return std::clamp(upscale, 0, 3);
    return iw == dw && ih == dh ? 0 : 1;
}

/// (S5) A SPIR-V module's storage images of one format declared as another (OpTypeImage's Image Format operand, for the
/// images with Sampled 2): the window pass's shaders store rgba8, and an HDR Native picture (PQ codes) goes through RGBA16F
/// images with their format changed to rgba16f, no new shader. The count changed; 0 when the words are not SPIR-V
inline size_t storage_format_patch(std::vector<uint32_t>& w, uint32_t from, uint32_t to) {
    if (w.size() < 5 || w[0] != 0x07230203u) return 0;
    size_t n = 0;
    for (size_t i = 5; i < w.size();) {
        const uint32_t op = w[i] & 0xffffu, count = w[i] >> 16;
        if (!count || i + count > w.size()) return 0;
        if (op == 25 /* OpTypeImage */ && count >= 9 && w[i + 7] == 2 && w[i + 8] == from) w[i + 8] = to, n++;
        i += count;
    }
    return n;
}
constexpr uint32_t kSpvRgba16f = 2, kSpvRgba8 = 4;   // (SPIR-V's Image Format numbers)
/// ((Triforce, FG-5) Rgb10A2, VK_FORMAT_A2B10G10R10_UNORM_PACK32's, which a module may name only with the capability below)
constexpr uint32_t kSpvRgb10A2 = 11, kSpvCapStorageImageExtendedFormats = 49;
/// ((Triforce, FG-5) The module declares capability cap: added as the first instruction after the header when it does not
/// (OpCapability, opcode 17, comes before everything else in a module). False when the words are not SPIR-V
inline bool spv_require_capability(std::vector<uint32_t>& w, uint32_t cap) {
    if (w.size() < 5 || w[0] != 0x07230203u) return false;
    for (size_t i = 5; i < w.size();) {
        const uint32_t op = w[i] & 0xffffu, count = w[i] >> 16;
        if (!count || i + count > w.size()) return false;
        if (op == 17 && count == 2 && w[i + 1] == cap) return true;
        if (op != 17) break;   // (the capabilities are first: past them, it is not there)
        i += count;
    }
    w.insert(w.begin() + 5, {(2u << 16) | 17u, cap});
    return true;
}

/// (S8, FSR3-07) The jitter's share of the bias below. NVIDIA's and AMD's guides allow up to -1 (the frames the upscaler
/// gathers resolve detail finer than one frame shows) and warn that it can make high-frequency textures and alpha cutouts
/// shimmer; NX2navi 1fb2877 took the -1. In the lab (Kokiri Forest, walking, FSR 4 Performance: -2.00) every run with it
/// had 5-8 % more shimmer than every run without it, +7..+16 % on the ground, the grass and the far trees (p = 0.029):
/// half of it, -0.5
constexpr float kLodBiasJitter = -0.5f;
/// (S8, FSR3-07; NX2navi 1fb2877) The texture LOD bias while DLSS or FSR scales the 3D with jitter: log2(render / output),
/// which samples the textures for the output's size, and kLodBiasJitter more for the jitter: DLAA and the native AAs -0.5,
/// Quality -1.08 (-1 in quarters), Performance -1.5, Ultra Performance -2.08. 0 without an upscaler, never above 0
inline float lod_bias(uint32_t render_w, uint32_t output_w, bool jitter) {
    if (!render_w || !output_w) return 0.0f;
    return std::min(0.0f, std::log2((float)render_w / (float)output_w) + (jitter ? kLodBiasJitter : 0.0f));
}
/// (S8) Whether a draw renders the jittered scene, the only pass the bias is for: its colour targets, or a depth-only draw's
/// depth target (a depth or alpha prepass of the scene is the scene too), at the upscaler's render size, width and height
/// (target_w, target_h; render_w, render_h). The sun's shadow cascades, the characters' shadow maps, reflection and cube
/// captures, half-size buffers and the passes at the output's size are not jittered and not at the render size: biased,
/// the alpha test of foliage cards ran on a finer level than the shadow's texel, and the shadows crawled. No render size
/// (no upscaler drew): none
inline bool lod_scene(uint32_t target_w, uint32_t target_h, uint32_t render_w, uint32_t render_h) {
    return render_w && render_h && target_w == render_w && target_h == render_h;
}
/// (S8) A target of a draw as lod_scene sees it: the texture's full size and the level drawn into (width 0: none)
struct LodTarget {
    uint32_t width = 0, height = 0, level = 0;
};
/// (S8) The size a draw is judged by (lod_scene): the smallest of its colour targets at their levels, or with none, its depth
/// target's at its level (a depth or alpha prepass). 0 x 0 with neither
inline void lod_scene_size(const LodTarget* color, int n, LodTarget depth, uint32_t* w, uint32_t* h) {
    *w = *h = 0;
    const auto at = [](uint32_t v, uint32_t level) { return std::max(1u, level < 32 ? v >> level : 0u); };
    for (int i = 0; i < n; i++)
        if (color[i].width && color[i].height) {
            const uint32_t cw = at(color[i].width, color[i].level), ch = at(color[i].height, color[i].level);
            *w = *w ? std::min(*w, cw) : cw, *h = *h ? std::min(*h, ch) : ch;
        }
    if (!*w && depth.width && depth.height) *w = at(depth.width, depth.level), *h = at(depth.height, depth.level);
}
/// (S8) This frame's scene size as one word (lod_scene_now): the scene camera's target, which the upscaler's notes keep in
/// stream order (vk_temporal.cpp temporal_note_scene_draw, vk_fsr3_inputs.cpp note_scene_draw), with the frame it was drawn in
inline uint64_t lod_scene_pack(uint32_t w, uint32_t h, uint64_t frame) {
    return (uint64_t)(w & 0xffffu) | (uint64_t)(h & 0xffffu) << 16 | (uint64_t)(uint32_t)frame << 32;
}
/// (S8) Whether a draw of that size (target_w, target_h) renders the jittered scene: at the last upscale's render size
/// (render_w, render_h), or at the size of the scene the upscaler's camera was noted for in this frame or the one before
/// (noted, lod_scene_pack; frame, this frame's number). The game's dynamic resolution makes a new set of render targets
/// at each size, and the last upscale's is the frame before's: by it alone, every frame at a new size was drawn unbiased
/// (a sharpness pop at each step, and never biased when the size goes to and fro every frame)
inline bool lod_scene_now(uint32_t target_w, uint32_t target_h, uint32_t render_w, uint32_t render_h, uint64_t noted,
                          uint64_t frame) {
    if (lod_scene(target_w, target_h, render_w, render_h)) return true;
    if (!noted || (uint32_t)((uint32_t)frame - (uint32_t)(noted >> 32)) > 1) return false;
    return lod_scene(target_w, target_h, (uint32_t)(noted & 0xffffu), (uint32_t)(noted >> 16 & 0xffffu));
}
/// (S8) RX_VK_UPSCALE_LOD_BIAS (value, null when unset): Computed (unset, or a positive whole number), lod_bias's; Off ("0",
/// or anything that does not start with a number, as atoi reads it), the game's own bias alone; Fixed (a negative number,
/// "-1" or "-1.5"), that bias (*fixed, clamped to [-3, 0]) wherever the computed one would apply: the scene's samplers,
/// while an upscaler with jitter runs
enum class LodBiasSwitch { Computed, Off, Fixed };
inline LodBiasSwitch lod_bias_switch(const char* value, float* fixed) {
    *fixed = 0.0f;
    if (!value) return LodBiasSwitch::Computed;
    char* end = nullptr;
    const float v = std::strtof(value, &end);
    if (end != value && v < 0.0f) {
        *fixed = std::max(-3.0f, v);
        return LodBiasSwitch::Fixed;
    }
    return std::atoi(value) == 0 ? LodBiasSwitch::Off : LodBiasSwitch::Computed;
}
/// (S8) The bias a frame wants while an upscaler is chosen (render_w and output_w its last sizes, jitter whether it
/// jitters): Fixed, the switch's value once it has drawn with jitter (taken as it is, lod_bias_step); Computed, lod_bias's;
/// Off, none
inline float lod_bias_want(LodBiasSwitch mode, float fixed, uint32_t render_w, uint32_t output_w, bool jitter) {
    if (mode == LodBiasSwitch::Off) return 0.0f;
    if (mode == LodBiasSwitch::Fixed) return render_w && jitter ? fixed : 0.0f;
    return lod_bias(render_w, output_w, jitter);
}
/// The bias the samplers are made with next, from the one in use (now) and the frame's (want, lod_bias): in quarters, and
/// moved only once want is a fifth of a level from now, so that the game's dynamic resolution, a render size going to and fro
/// across a step, does not make samplers again at every frame (each change makes the frame's descriptors anew). Back to 0
/// at once
inline float lod_bias_next(float now, float want) {
    const float q = std::min(0.0f, std::round(want * 4.0f) / 4.0f);
    if (q == now) return now;
    if (want < 0.0f && std::fabs(want - now) < 0.2f) return now;
    return q;
}
/// (S8) The next bias from lod_bias_want's: a Fixed one as it is asked, the computed one through lod_bias_next
inline float lod_bias_step(LodBiasSwitch mode, float now, float want) {
    return mode == LodBiasSwitch::Fixed ? want : lod_bias_next(now, want);
}
/// The samplers that take it, in the scene's draws (lod_scene): the material textures, filtered linearly within and between levels, wrapping
/// (repeating or mirrored) on one axis at least, with no depth compare and no min/max reduction. Clamped ones (post effects,
/// lookup tables, the HUD and text) keep the game's own bias (NX2navi's dlss_biased)
inline bool lod_biased(bool linear_mip_linear, bool wraps_s, bool wraps_t, bool compare, bool reduction) {
    return linear_mip_linear && (wraps_s || wraps_t) && !compare && !reduction;
}

}  // namespace mkwvk::sharpen_rules
