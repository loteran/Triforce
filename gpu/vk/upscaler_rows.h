/* The Insert menu's scaling rows (vk_menu.cpp), as plain functions: what the Render resolution row steps through for each
   Upscaler, and the names it shows for DLSS's (and FSR 3.1's and FSR 4's) modes. No Vulkan, no menu: tests/test_upscaler_rows.cpp checks them. */
#pragma once

namespace mkwvk::upscaler_rows {

/// The Upscaler row (opts.dlss, RX_VK_DLSS): the game's own network, NVIDIA DLSS, NVIDIA DLAA, AMD FSR 3.1 (built from AMD's
/// shader sources: gpu/vk/vk_fsr3.cpp), AMD FSR 4 and its native anti-aliasing (AMD's library, a build with FSR 4:
/// gpu/vk/vk_fsr4.cpp), and FSR 3.1's own native anti-aliasing (the same passes with the 3D at full size). The saved
/// values keep their meaning: each came after the ones before, so a saved 1 or 2 is still DLSS or DLAA and a saved 3 FSR 3.1
enum { GAME = 0, DLSS = 1, DLAA = 2, FSR3 = 3, FSR4 = 4, FSR4_AA = 5, FSR3_AA = 6, kUpscalers = 7 };
/// One bit per upscaler that can run here (1 << GAME always): what the row steps through
inline unsigned bit(int upscaler) { return 1u << upscaler; }
/// The DLSS and DLAA bits, for a caller that knows only whether DLSS runs
inline unsigned dlss_bits(bool dlss_ok) { return dlss_ok ? bit(DLSS) | bit(DLAA) : 0u; }
/// FSR 4's two, the same way (fsr4_ok: fsr4_why() == FSR_OK)
inline unsigned fsr4_bits(bool fsr4_ok) { return fsr4_ok ? bit(FSR4) | bit(FSR4_AA) : 0u; }
/// FSR 3.1's two (fsr3_ok: vk_fsr3.cpp says its passes run on this GPU): they are the same passes at two sizes
inline unsigned fsr3_bits(bool fsr3_ok) { return fsr3_ok ? bit(FSR3) | bit(FSR3_AA) : 0u; }
inline bool is_dlss(int upscaler) { return upscaler == DLSS || upscaler == DLAA; }
inline bool is_fsr4(int upscaler) { return upscaler == FSR4 || upscaler == FSR4_AA; }
inline bool is_fsr3(int upscaler) { return upscaler == FSR3 || upscaler == FSR3_AA; }
/// The 3D drawn at full size, the upscaler anti-aliasing alone (DLAA, FSR 4's native AA, FSR 3.1's): the Render resolution
/// row does not apply
inline bool full_scale(int upscaler) { return upscaler == DLAA || upscaler == FSR4_AA || upscaler == FSR3_AA; }
/// FSR 3.1 and FSR 4 scale from a smaller 3D at the shares DLSS's modes use: Quality 1.5x (67%), Balanced 1.7x (58%),
/// Performance 2x (50%) and Ultra Performance 3x (33%) per axis, AMD's own ratios too
inline bool scales_by_mode(int upscaler) { return upscaler == DLSS || upscaler == FSR3 || upscaler == FSR4; }

/// opts.resolution: 0 Auto (the game's dynamic resolution), else a share of the output in percent. 1 to 4 are the values from
/// before (100, 75, 67, 50: saved options keep their meaning); 5 and 6 came with DLSS's Balanced and Ultra Performance. Each
/// is a twelfth of the output (vk_menu.cpp twelfths): 8/12, 7/12, 6/12 and 4/12 are DLSS's own ratios
constexpr int kPercent[] = {0, 100, 75, 67, 50, 58, 33};
constexpr int kValues = sizeof kPercent / sizeof kPercent[0];
inline int percent(int v) { return v > 0 && v < kValues ? kPercent[v] : 0; }

/// The steps the Render resolution row takes, best first after Auto. DLSS: its Quality, Balanced, Performance and Ultra
/// Performance; Auto too, since DLSS follows the game's dynamic size (vk_dlss.cpp makes its feature again when the size
/// leaves the mode's range; FSR 4's context takes any size up to the output). DLAA, FSR 4's and FSR 3.1's native AA: none
/// (the 3D is drawn at full size)
constexpr int kGameSteps[] = {0, 1, 2, 3, 4};
constexpr int kDlssSteps[] = {0, 3, 5, 4, 6};
inline const int* steps(int upscaler, int& n) {
    if (scales_by_mode(upscaler)) return n = sizeof kDlssSteps / sizeof kDlssSteps[0], kDlssSteps;
    return n = sizeof kGameSteps / sizeof kGameSteps[0], kGameSteps;
}

/// DLSS's name for a Render resolution value: nullptr for Auto, or a share that is not one of its modes
inline const char* dlss_mode_name(int v) {
    switch (percent(v)) {
    case 67: return "Quality";
    case 58: return "Balanced";
    case 50: return "Performance";
    case 33: return "Ultra Performance";
    default: return nullptr;
    }
}

/// The Render resolution row's next value from v (dir +1 Right, -1 Left), wrapping. A value that is not one of the upscaler's
/// steps (one saved with another upscaler) moves to its neighbour by size. DLAA, FSR 4's native AA: v, unchanged
inline int step(int upscaler, int v, int dir) {
    if (full_scale(upscaler)) return v;
    int n = 0;
    const int* s = steps(upscaler, n);
    for (int i = 0; i < n; i++)
        if (s[i] == v) return s[(i + (dir > 0 ? 1 : n - 1)) % n];
    // (not a step: where it would sit, Auto first and then larger shares first)
    const int p = percent(v);
    int pos = 1;
    while (pos < n && percent(s[pos]) > p) pos++;
    return dir > 0 ? s[pos % n] : s[pos - 1];
}

/// v as one of the upscaler's steps: itself when it is one, else the step nearest in size (the larger on a tie). Auto stays
/// Auto. DLAA, FSR 4's native AA: v, unchanged (its row does not apply; v comes back with the next upscaler)
inline int snap(int upscaler, int v) {
    if (full_scale(upscaler) || percent(v) == 0) return percent(v) == 0 ? 0 : v;
    int n = 0;
    const int* s = steps(upscaler, n);
    int best = v, best_d = 1000;
    for (int i = 0; i < n; i++) {
        if (s[i] == v) return v;
        if (!percent(s[i])) continue;
        const int d = percent(s[i]) > percent(v) ? percent(s[i]) - percent(v) : percent(v) - percent(s[i]);
        if (d < best_d || (d == best_d && percent(s[i]) > percent(best))) best = s[i], best_d = d;
    }
    return best;
}

/// The order the row shows them in: each engine's native anti-aliasing after it. FSR 3.1's came last (its saved value is
/// after FSR 4's two) and stands with FSR 3.1
constexpr int kOrder[kUpscalers] = {GAME, DLSS, DLAA, FSR3, FSR3_AA, FSR4, FSR4_AA};
/// The Upscaler row's next value from v (dir +1 / -1), wrapping over the choices there are (available: bit() of each that
/// runs here), in kOrder. A value that cannot run here steps as the game's own does
inline int step_upscaler(int v, int dir, unsigned available) {
    available |= bit(GAME);
    int at = 0;   // (v's place in the order; the game's own for a value that cannot run here)
    for (int i = 0; i < kUpscalers; i++)
        if (kOrder[i] == v && (available & bit(v))) at = i;
    for (int i = 0; i < kUpscalers; i++) {
        at = (at + (dir > 0 ? 1 : kUpscalers - 1)) % kUpscalers;
        if (available & bit(kOrder[at])) return kOrder[at];
    }
    return GAME;
}
/// The same, where the choices are DLSS and DLAA only when dlss_ok (a build with NVIDIA's SDK, on a GPU DLSS runs on)
inline int step_upscaler(int v, int dir, bool dlss_ok) { return step_upscaler(v, dir, bit(GAME) | dlss_bits(dlss_ok)); }

// ---- (Triforce, options_version 7) The Upscaler row names the engine alone: Game, NVIDIA DLSS, AMD FSR 3.1, AMD FSR 4. An
// engine's native anti-aliasing (DLAA, FSR 3.1's and FSR 4's Native AA: the same engine with the 3D at full size) is the
// Render resolution row's "Native (100%)" step. opts.dlss keeps its saved values: 2, 5 and 6 are still DLAA and the native
// AAs, shown as their engine at Native (100%) ----
/// The engine of an Upscaler value: DLAA is DLSS's, FSR 4's and FSR 3.1's native AA their own
inline int engine(int upscaler) {
    return upscaler == DLAA ? DLSS : upscaler == FSR4_AA ? FSR4 : upscaler == FSR3_AA ? FSR3
           : upscaler >= GAME && upscaler < kUpscalers ? upscaler : GAME;
}
/// An engine's native anti-aliasing (the game's own has none: itself)
inline int native_of(int engine_value) {
    return engine_value == DLSS ? DLAA : engine_value == FSR4 ? FSR4_AA : engine_value == FSR3 ? FSR3_AA : engine_value;
}
/// The engines in the order the Upscaler row shows them
constexpr int kEngines[] = {GAME, DLSS, FSR3, FSR4};
constexpr int kEngineCount = sizeof kEngines / sizeof kEngines[0];
/// What the two rows choose together: the Upscaler's value (opts.dlss) and the Render resolution's (opts.resolution)
struct Scaling {
    int upscaler, resolution;
};
/// The 3D at the output's full size: an engine's native AA, or the game's own at 100%
inline bool native_size(int upscaler, int resolution) { return full_scale(upscaler) || (upscaler == GAME && percent(resolution) == 100); }
/// The Upscaler row's next engine from upscaler (dir +1 / -1), wrapping over those that run here (available: bit() of each).
/// The Render resolution's Native (100%) goes with it: an engine's native AA becomes the next engine's (the game's own at
/// 100%), and the game's own at 100% the engine's native AA. Otherwise the resolution is the caller's to snap (snap)
inline Scaling step_engine(int upscaler, int resolution, int dir, unsigned available) {
    available |= bit(GAME);
    const bool native = native_size(upscaler, resolution);
    int at = 0;
    for (int i = 0; i < kEngineCount; i++)
        if (kEngines[i] == engine(upscaler) && (available & bit(kEngines[i]))) at = i;
    for (int i = 0; i < kEngineCount; i++) {
        at = (at + (dir > 0 ? 1 : kEngineCount - 1)) % kEngineCount;
        const int e = kEngines[at];
        if (!(available & bit(e))) continue;
        if (e == GAME) return {GAME, native ? 1 : resolution};
        return {native ? native_of(e) : e, resolution};
    }
    return {GAME, resolution};
}
/// The Render resolution row's steps with DLSS or FSR: Auto, Native (100%), then their modes (kDlssSteps). kNativeStep stands
/// for Native in the list: the engine's native AA, the saved resolution left as it is for the engine's other steps
constexpr int kNativeStep = -1;
constexpr int kTemporalSteps[] = {0, kNativeStep, 3, 5, 4, 6};
constexpr int kTemporalStepCount = sizeof kTemporalSteps / sizeof kTemporalSteps[0];
/// The Render resolution row's next value from (upscaler, resolution), dir +1 Right, -1 Left, wrapping. The game's own: its
/// own steps (step). DLSS and FSR: kTemporalSteps, Native (100%) their native AA. A share that is not one of their steps (one
/// saved with the game's own) moves as step does
inline Scaling step_resolution(int upscaler, int resolution, int dir) {
    const int e = engine(upscaler);
    if (!scales_by_mode(e)) return {upscaler, step(upscaler, resolution, dir)};
    const int now = full_scale(upscaler) ? kNativeStep : resolution;
    for (int i = 0; i < kTemporalStepCount; i++) {
        if (kTemporalSteps[i] != now) continue;
        const int next = kTemporalSteps[(i + (dir > 0 ? 1 : kTemporalStepCount - 1)) % kTemporalStepCount];
        return next == kNativeStep ? Scaling{native_of(e), resolution} : Scaling{e, next};
    }
    return {e, step(e, resolution, dir)};
}
/// How the finished picture is scaled to its place in the window (options_version 7: the Window scaling filter row is gone,
/// this is its choice, opts.upscale's values): AMD FSR 1 (3: EASU, and RCAS only with Sharpness above 0, vk_framegen.cpp
/// framegen_scale) when the picture is enlarged; otherwise the plain copy (0), the linear blit the row's default Off gave,
/// which still scales the picture to its place: when the sizes are the same or not known yet, and when the picture is made
/// smaller (or larger one way and smaller the other). (Not Lanczos there: fg_scale.comp's taps are one picture pixel apart,
/// so a picture made smaller is no smoother for it, and the Steam Deck (2048 x 1152 to 1280 x 800) would pay a pass at every
/// present for nothing)
inline int auto_filter(unsigned picture_w, unsigned picture_h, unsigned dest_w, unsigned dest_h) {
    if (!picture_w || !picture_h || !dest_w || !dest_h || (picture_w == dest_w && picture_h == dest_h)) return 0;
    return dest_w >= picture_w && dest_h >= picture_h ? 3 : 0;
}
/// The window's filter for a present: auto_filter, but the plain copy (0) while the picture is HDR Native's own (HDR10 PQ codes
/// in A2B10G10R10, vk_replay.cpp present_native_pq): the blit keeps the 10 bits as they are. (framegen_scale's rgba8 images
/// gave the HDR10 swapchain 8-bit PQ, banding in skies, fog and dark gradients; it now takes PQ through RGBA16F images,
/// (Triforce, S5) sharpen_rules.h, for Sharpness at the window and the sides, but adds no filter pass at every present)
inline int window_filter(unsigned picture_w, unsigned picture_h, unsigned dest_w, unsigned dest_h, bool native_hdr) {
    return native_hdr ? 0 : auto_filter(picture_w, picture_h, dest_w, dest_h);
}

/// The DLSS model row (opts.dlss_model, saved as dlss_model): which of NVIDIA's networks DLSS and DLAA draw with, by its
/// preset (NVSDK_NGX_DLSS_Hint_Render_Preset_*). 0 the transformer, preset K (the default here, the one tested); 1 the second
/// generation transformer, preset M (NVIDIA's own pick for Performance); 2 the same generation as tuned for Ultra
/// Performance, preset L; 3 NVIDIA's default, preset 0 (the runtime picks per mode, and a runtime update can change it)
enum { MODEL_K = 0, MODEL_M = 1, MODEL_L = 2, MODEL_DEFAULT = 3, kModels = 4 };
inline unsigned model_preset(int model) {
    switch (model) {
    case MODEL_M: return 13;
    case MODEL_L: return 12;
    case MODEL_DEFAULT: return 0;
    default: return 11;   // (K, also for a value past the row)
    }
}
/// The preset DLSS is asked for: RX_VK_DLSS_PRESET over the row (env: its value, or nullptr), a letter (A = 1 ... Z = 26,
/// either case) or 0 for NVIDIA's default; anything else leaves the row's
inline bool preset_env(const char* env) {
    return env && ((*env >= 'A' && *env <= 'Z') || (*env >= 'a' && *env <= 'z') || (env[0] == '0' && !env[1]));
}
inline unsigned dlss_preset(int model, const char* env) {
    if (!preset_env(env)) return model_preset(model);
    if (*env == '0') return 0;
    return (unsigned)(*env >= 'a' ? *env - 'a' + 1 : *env - 'A' + 1);
}
/// A preset's letter for the log ('A' + preset - 1), or '0' for NVIDIA's default
inline char preset_letter(unsigned preset) { return preset ? (char)('A' + preset - 1) : '0'; }

/// The Upscaler row is there only in a build with DLSS or FSR 4 (built: dlss_built() || fsr4_built()): elsewhere the game's own is all there is, and
/// the Render resolution row is the game's own's
inline bool upscaler_shown(bool built) { return built; }
/// ...and in any build where FSR 3.1 runs (fsr: vk_fsr3.cpp answered that it runs on this GPU)
inline bool upscaler_shown(bool built, bool fsr) { return built || fsr; }
/// Rows with nothing else to choose here (shown without arrows; Left and Right leave the saved value alone): the Upscaler
/// where DLSS cannot run (dlss_ok: dlss_why() == DLSS_OK), the Render resolution with DLAA or FSR's native AA (the 3D at
/// full size), the DLSS model while DLSS and DLAA are not drawing
inline bool upscaler_fixed(bool dlss_ok) { return !dlss_ok; }
/// The same, from the mask of what can run: fixed when only the game's own can
inline bool upscaler_fixed(unsigned available) { return (available & ~bit(GAME)) == 0; }
inline bool resolution_fixed(int upscaler) { return full_scale(upscaler); }
/// The FSR sharpening row (opts.fsr_sharpness, fsr_sharpen.h): AMD's own RCAS inside AMD's library, which draws FSR 4 and its native AA, and
/// FSR 3.1 where the library draws it (ffx3_library: ffx3_usable(); else Triforce's own passes of FSR 3.1, which have none). Shown in a build
/// where either can run, and fixed (shown without arrows, the saved value kept) while another Upscaler is on
inline bool fsr_sharpen_used(int upscaler, bool ffx3_library) { return is_fsr4(upscaler) || (is_fsr3(upscaler) && ffx3_library); }
inline bool fsr_sharpen_fixed(int upscaler, bool ffx3_library) { return !fsr_sharpen_used(upscaler, ffx3_library); }
inline bool fsr_sharpen_shown(bool fsr4_built, bool ffx3_library) { return fsr4_built || ffx3_library; }
/// The DLSS model row: shown where the Upscaler is (a build with DLSS), with nothing to choose while the game's own upscales
inline bool model_shown(bool built) { return built; }
/// (env_set: RX_VK_DLSS_PRESET names a preset, which wins over the row for this run)
inline bool model_fixed(int upscaler, bool env_set) { return !is_dlss(upscaler) || env_set; }

} // namespace mkwvk::upscaler_rows
