// The context policy of AMD FSR 3.1 through its API library (vk_ffx3.cpp), as plain functions: which of a few upscale contexts
// serves a frame, which go, and when one may be destroyed. No Vulkan, no library: gpu/vk/tests/ffx3_policy_test.cpp checks it.
//
// Why contexts are kept per size: AMD's own sample waits for the GPU and makes the context anew whenever the render
// resolution changes, and sets FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION only for its dynamic resolution mode. One context for
// every render size, without that flag, was seen to crash AMD's driver in a dispatch after a switch of the render share. So:
// one context for each (render size, output size) with fixed shares, and one for every size up to the output's while the
// game's dynamic resolution (the Render resolution row's Auto) is on. A context of another render size for the same output
// goes once it is not among the most recent ones, so a row stepped through keeps two for a few frames. (This is NX2navi's
// vk_ffx.cpp policy, which the same game runs on.)
#pragma once

#include <cstdint>

namespace mkwvk::ffx3_policy {

constexpr int kSlots = 4;
/// A context used in the last few asks stays: a frame asks once or twice (the scene, and a second scene after it)
constexpr uint64_t kRecent = 8;
/// A context no upscale used for this long goes (a second scene's, the pause screen's, a while after it closed)
constexpr uint64_t kIdleNs = 90ull * 1000000000ull;

/// One context as the policy sees it (the library's handle is kept beside it, by index)
struct Slot {
    bool live = false;
    uint32_t max_w = 0, max_h = 0;   // the render size it was made for (the largest it takes, with drs)
    uint32_t out_w = 0, out_h = 0;   // its output size
    bool drs = false;                // made for dynamic resolution
    bool hdr = false;                // made for linear HDR colour (the other kind makes it again)
    uint64_t used = 0;               // the ask tick of its last use
    uint64_t used_ns = 0;            // the clock at its last use
    uint64_t fence = 0;              // the GPU's timeline value of the last recording that used it (0: none)
};

/// What a frame asks for
struct Ask {
    uint32_t w = 0, h = 0, ow = 0, oh = 0;
    bool drs = false, hdr = false;
};

/// The render size a context is made for: the size itself, or with dynamic resolution every size up to the output (and a
/// scene drawn larger than it)
inline void made_for(const Ask& a, uint32_t* mw, uint32_t* mh) {
    *mw = !a.drs ? a.w : a.w > a.ow ? a.w : a.ow;
    *mh = !a.drs ? a.h : a.h > a.oh ? a.h : a.oh;
}

inline bool serves(const Slot& s, const Ask& a) {
    if (!s.live || s.out_w != a.ow || s.out_h != a.oh || s.drs != a.drs || s.hdr != a.hdr) return false;
    return a.drs ? a.w <= s.max_w && a.h <= s.max_h : a.w == s.max_w && a.h == s.max_h;
}

/// The slot that serves the ask, or -1
inline int find(const Slot* slots, const Ask& a) {
    int found = -1;
    for (int i = 0; i < kSlots; i++)
        if (serves(slots[i], a)) found = i;
    return found;
}

/// A context of the ask's output size that does not serve it and is not recent: it goes (a bit of the result's mask)
inline unsigned stale_for_size(const Slot* slots, const Ask& a, int found, uint64_t tick) {
    unsigned mask = 0;
    for (int i = 0; i < kSlots; i++) {
        const Slot& s = slots[i];
        if (s.live && i != found && s.out_w == a.ow && s.out_h == a.oh && tick - s.used >= kRecent) mask |= 1u << i;
    }
    return mask;
}

/// The slot a new context takes: a free one, else the least recently used that is not recent; -1 when all are recent.
/// *evict: that slot was taken from a context that goes
inline int pick_slot(const Slot* slots, uint64_t tick, bool* evict) {
    int free_slot = -1, lru = -1;
    *evict = false;
    for (int i = 0; i < kSlots; i++) {
        const Slot& s = slots[i];
        if (!s.live) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (tick - s.used >= kRecent && (lru < 0 || s.used < slots[lru].used)) lru = i;
    }
    if (free_slot >= 0) return free_slot;
    if (lru >= 0) *evict = true;
    return lru;
}

/// The contexts no upscale used for kIdleNs (a bit of the mask each)
inline unsigned idle_mask(const Slot* slots, uint64_t now_ns) {
    unsigned mask = 0;
    for (int i = 0; i < kSlots; i++)
        if (slots[i].live && now_ns - slots[i].used_ns > kIdleNs) mask |= 1u << i;
    return mask;
}

/// A context that was let go of may be destroyed once the GPU is past the last recording that used it
inline bool gpu_past(uint64_t fence, uint64_t completed) { return fence <= completed; }

}  // namespace mkwvk::ffx3_policy
