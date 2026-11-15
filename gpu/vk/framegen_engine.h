// Who makes the frames between the game's, as plain functions: no Vulkan, no library. gpu/vk/tests/framegen_engine_test.cpp checks it.
//
// The Frame generation row (opts.framegen: 0 off, 1 Auto, 2 to 8 times) and the Frame generation engine row (opts.fg_engine: Auto, NVIDIA
// DLSS-G, AMD, Triforce; RX_VK_FRAMEGEN_VENDOR=auto|nvidia|amd|triforce, when set, goes over it) pick the engine, in this order:
//   1. NVIDIA DLSS Frame Generation (vk_ngxfg.h) where it can run: an RTX 40 or 50, its runtime there, the multiplier within what the
//      GPU makes, SDR;
//   2. AMD FSR 3.1 frame generation through AMD's own FidelityFX library (vk_ffx_fg.h) at 2x, when the library is beside the game and
//      loads, the device has the queues its swapchain needs, and it has not failed in this run (it is vendor neutral: any GPU);
//   3. Triforce's own: AMD FSR 3.1's interpolation as Triforce's own passes (vk_fsr3fi.h) at 2x, the optical-flow interpolation of
//      vk_framegen.cpp otherwise (Auto, 3x and up, and any frame the passes above cannot take).
// `nvidia` makes DLSS-G the only engine (none at all where it cannot run); `amd` takes the library where it can run and Triforce's
// own where it cannot; `triforce` is the third alone.
#pragma once

#include <cstring>

namespace mkwvk::fgengine {

enum class Vendor { Auto, Nvidia, Amd, Triforce };
enum class Engine {
    None,        ///< no made frames (the row is off, or `nvidia` and DLSS-G cannot run)
    DlssG,       ///< NVIDIA DLSS Frame Generation
    AmdLibrary,  ///< AMD FSR 3.1 frame generation, AMD's library and its swapchain
    Fsr3fi,      ///< AMD FSR 3.1 interpolation, Triforce's own passes (2x)
    Flow,        ///< Triforce's optical-flow interpolation
};

/// RX_VK_FRAMEGEN_VENDOR's text: unset, empty and "auto" are Auto; anything else unknown is Auto too, with *known false
inline Vendor parse_vendor(const char* text, bool* known = nullptr) {
    if (known) *known = true;
    if (!text || !*text || !std::strcmp(text, "auto")) return Vendor::Auto;
    if (!std::strcmp(text, "nvidia")) return Vendor::Nvidia;
    if (!std::strcmp(text, "amd")) return Vendor::Amd;
    if (!std::strcmp(text, "triforce")) return Vendor::Triforce;
    if (known) *known = false;
    return Vendor::Auto;
}

/// The Frame generation engine row (opts.fg_engine, saved as frame_generation_engine): the vendors in the order above
constexpr int kEngineRows = 4;
inline Vendor vendor_from_row(int row) {
    switch (row) {
    case 1: return Vendor::Nvidia;
    case 2: return Vendor::Amd;
    case 3: return Vendor::Triforce;
    default: return Vendor::Auto;
    }
}
/// The vendor that rules: RX_VK_FRAMEGEN_VENDOR when it names one (env_set), else the row's. A row on NVIDIA where DLSS Frame Generation
/// cannot run here (a saved choice from another PC) is Auto: the env var keeps its strict meaning (none made), the row never leaves a
/// player with no made frames at all
inline Vendor effective_vendor(bool env_set, Vendor env, int row, bool dlssg_capable) {
    if (env_set) return env;
    const Vendor v = vendor_from_row(row);
    return v == Vendor::Nvidia && !dlssg_capable ? Vendor::Auto : v;
}
/// One bit per row value that can run here (1 << row): Auto and Triforce always, NVIDIA where DLSS Frame Generation can run, AMD where
/// its library can
inline unsigned rows_available(bool dlssg_capable, bool amd_usable) { return 1u | (dlssg_capable ? 2u : 0u) | (amd_usable ? 4u : 0u) | 8u; }
/// The row's next value from v (dir +1 / -1), wrapping over those that can run here; a value that cannot run here steps as Auto does
inline int step_row(int v, int dir, unsigned available) {
    available |= 1u;
    int at = v < 0 || v >= kEngineRows || !(available & (1u << v)) ? 0 : v;
    for (int i = 0; i < kEngineRows; i++) {
        at = (at + (dir > 0 ? 1 : kEngineRows - 1)) % kEngineRows;
        if (available & (1u << at)) return at;
    }
    return 0;
}

/// What the choice depends on, all of it read by the caller
struct Facts {
    Vendor vendor = Vendor::Auto;
    int framegen = 0;            ///< the row: 0 off, 1 Auto, 2 to 8 times
    bool enabled = true;         ///< the interpolation is not off for the run (RX_VK_FRAMEGEN=0, a failed setup, no window)
    bool dlssg_usable = false;   ///< ngxfg::wanted_for(framegen): it can run and makes this multiplier, in SDR
    bool amd_usable = false;     ///< the library is loaded, the device has the swapchain's queues, and it has not failed in this run
    bool fsr3fi_usable = false;  ///< Triforce's own passes of FSR 3.1 can make the frame at this row
};

struct Choice {
    Engine engine = Engine::None;
    const char* why = "";   ///< the reason it is not the one before it in the order (empty: the first that could be)
};

inline const char* name(Engine e) {
    switch (e) {
    case Engine::DlssG: return "NVIDIA DLSS Frame Generation";
    case Engine::AmdLibrary: return "AMD FSR 3.1 frame generation (AMD's library)";
    case Engine::Fsr3fi: return "AMD FSR 3.1 interpolation (Triforce's own passes)";
    case Engine::Flow: return "the renderer's own optical flow";
    case Engine::None: break;
    }
    return "none";
}

/// AMD's library makes 2x only: its swapchain presents one generated frame between two of the game's
inline bool amd_multiplier(int framegen) { return framegen == 2; }

inline Choice triforce_own(const Facts& f, const char* why) {
    if (f.framegen == 2 && f.fsr3fi_usable) return {Engine::Fsr3fi, why};
    return {Engine::Flow, why};
}

inline Choice choose(const Facts& f) {
    if (f.framegen < 1 || !f.enabled) return {Engine::None, f.framegen < 1 ? "the Frame generation row is off" : "frame generation is off for this run"};
    switch (f.vendor) {
    case Vendor::Nvidia:
        if (f.dlssg_usable) return {Engine::DlssG, "(engine nvidia)"};
        return {Engine::None, "(engine nvidia, and DLSS Frame Generation cannot run here)"};
    case Vendor::Triforce:
        return triforce_own(f, "(engine triforce)");
    case Vendor::Amd:
        if (!amd_multiplier(f.framegen)) return triforce_own(f, "(engine amd, but AMD's library makes 2x only)");
        if (!f.amd_usable) return triforce_own(f, "(engine amd, but AMD's library cannot run here)");
        return {Engine::AmdLibrary, "(engine amd)"};
    case Vendor::Auto:
        break;
    }
    if (f.dlssg_usable) return {Engine::DlssG, ""};
    if (!amd_multiplier(f.framegen)) return triforce_own(f, "(AMD's library makes 2x only)");
    if (!f.amd_usable) return triforce_own(f, "(AMD's library cannot run here)");
    return {Engine::AmdLibrary, "(DLSS Frame Generation cannot run here)"};
}

}  // namespace mkwvk::fgengine
