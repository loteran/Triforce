// (Triforce) The frame chain's rules for its engines (vk_temporal.cpp; NVIDIA DLSS and AMD FSR 4) that need no GPU, as plain
// functions: what a failure does, when a native AA is one, and the shape of FSR 4's context (vk_fsr4.cpp).
// tests/test_fsr4_upscaler.py runs them.
#pragma once

#include <atomic>
#include <cstdint>

namespace mkwvk::temporal_rules {

/// (FSR4-1) An engine's failures, NX2navi's b5789c6 rule: a creation or an evaluation that fails leaves the game's upscale
/// for kRetryNs (a save or an area loading fills the video memory for a few frames, and what the engine held is freed a few
/// frames on), then the engine is tried again; the kFailMax-th failure in a row turns it off until the Upscaler row is set
/// again (rearm). A good evaluation starts the count again. Atomic: the game's and the menu's threads read it without the
/// chain's lock
struct Retry {
    static constexpr int kFailMax = 4;
    static constexpr int64_t kRetryNs = 2'000'000'000;
    std::atomic<int> fails{0};
    std::atomic<int64_t> retry_at{0};   // (steady_clock nanoseconds)
    std::atomic<bool> off{false};

    /// A failure at now: true when it is the one that turns the engine off, else it is tried again from retry_at
    bool failed(int64_t now) {
        const int n = fails.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n >= kFailMax) {
            off.store(true, std::memory_order_relaxed);
            return true;
        }
        retry_at.store(now + kRetryNs, std::memory_order_relaxed);
        return false;
    }
    int count() const { return fails.load(std::memory_order_relaxed); }
    /// Waiting to be tried again after a failure: the game's upscale meanwhile, the engine not asked
    bool waiting(int64_t now) const { return count() && now < retry_at.load(std::memory_order_relaxed); }
    bool is_off() const { return off.load(std::memory_order_relaxed); }
    void succeeded() { fails.store(0, std::memory_order_relaxed); }
    /// The Upscaler row set again: tried anew, the count from 0. true when it was off
    bool rearm() {
        fails.store(0, std::memory_order_relaxed), retry_at.store(0, std::memory_order_relaxed);
        return off.exchange(false, std::memory_order_relaxed);
    }
};

/// (FSR4-2) Whether a native AA (DLAA, FSR 4's: full_scale) anti-aliases at the render size: only while the 3D is drawn at
/// the output's size. The host reads the 3D's scale once, at the start, so one chosen in play while the 3D is smaller scales
/// it up to the output as the engine's upscale does, until the next start draws it at full size
inline bool native_at(bool full_scale, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    return full_scale && rw >= ow && rh >= oh;
}

/// (FSR4-9) FSR 4's context for a frame: the one there is (has: made for ctx_ow x ctx_oh, render sizes ctx_rw x ctx_rh, all
/// sizes up to those with ctx_drs, ctx_hdr colour), or a new one. A fixed share gets one of its own render size without
/// FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION, as AMD's sample makes them (NX2navi's 7e938ec: AMD's driver failed in the dispatch
/// on an RX 7900 XT when the render size changed under one context); the first time the render size changes under the same
/// output (the game's dynamic resolution) one with the flag for every size up to the output's from then on (dynamic)
struct ContextPlan {
    bool remake = false;   ///< a new context
    bool drs = false;      ///< made for every render size up to the output's
    bool dynamic = false;  ///< the render sizes change from now on (the caller keeps it)
    bool went_dynamic = false;   ///< this frame found it (for the log)
};
inline ContextPlan plan_context(bool has, uint32_t ctx_ow, uint32_t ctx_oh, uint32_t ctx_rw, uint32_t ctx_rh, bool ctx_drs,
                                bool ctx_hdr, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool hdr, bool dynamic,
                                bool always_drs) {
    ContextPlan p;
    const bool same = has && ctx_ow == ow && ctx_oh == oh && ctx_hdr == hdr;
    p.dynamic = dynamic;
    if (same && !ctx_drs && (ctx_rw != rw || ctx_rh != rh) && !dynamic) p.dynamic = p.went_dynamic = true;
    p.drs = always_drs || p.dynamic;
    const bool fits = same && (ctx_drs ? rw <= ctx_rw && rh <= ctx_rh : ctx_rw == rw && ctx_rh == rh && !p.drs);
    p.remake = !fits;
    return p;
}

/// (FSR4-10) The frame time FSR 4 is given: since the evaluation before, under 100 ms; else (the first, a long hitch) a 60th,
/// as AMD's frame generation is given it (vk_fsr4.cpp fsr4fg)
inline float frame_ms(bool first, float since_ms) { return !first && since_ms > 0.0f && since_ms < 100.0f ? since_ms : 16.667f; }

}  // namespace mkwvk::temporal_rules
