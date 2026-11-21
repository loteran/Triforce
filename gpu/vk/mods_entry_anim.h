// (Triforce, 11-19) The Mods entry under the title's list, animated as the game animates its own entries. Each entry of the
// title's list (Continue, New Game, Options, ...) is the part PaCommonBtnTitle_00 of the Title archive's layout, and the
// part plays its own animations as its button takes and lets go of the cursor: Select, UnSelect, LoopRandom while it has
// it, Decide when it is pressed. The Mods entry is drawn by the renderer one pitch under the list's last entry
// (vk_menu.cpp draw_title_mods); here are its times and amounts, read from the part's curves (the layout's keys: a frame,
// a value and a slope, Hermite between them, 60 frames a second) and from the title layout's (Title_00):
//   Select     every pane of the chosen look from 0 to 1, the unchosen text from 1 to 0, over 1 frame
//   UnSelect   back, over 8 frames
//   LoopRandom while chosen: 0.6, 0.5 at frame 20, 0.6 at 40 (the filigree's strength: 0.47 to 0.6 in the lab's shot)
//   Decide     16 frames: the filigree from 1 to 0.85 of its size from frame 8 (its glow to 0.95), the band's sparkle
//              (W_Bg_01) from 0 at frame 2 to 1 at 16; the game's next screen comes after it
//   the list   Title_00's 15-frame rise from 0 to 1 (its keys' slopes 0.02 and 0.14), as the entry comes with the list;
//              and gone, as UnSelect, once the list has not been seen for 150 ms (vk_menu.cpp mods_list_fresh's: a game
//              frame late, at the 30 fps cap or in a hitch, is not the list going); back after that, its In goes on from
//              where the entry was, not from nothing
// No ImGui and no Vulkan: tests/test_mods_entry_anim.py compiles it alone.
#pragma once
#include <algorithm>
#include <cstdint>

namespace mods_entry {

/// A key of the layout's curves: its frame, its value, its slope (a value a frame)
struct Key {
    float frame, value, slope;
};
/// A curve at frame f: the first key's value before it, the last's after; two keys at one frame are a step
inline float curve(const Key* k, int n, float f) {
    if (f <= k[0].frame) return k[0].value;
    for (int i = 0; i + 1 < n; i++) {
        const Key &a = k[i], &b = k[i + 1];
        if (f > b.frame) continue;
        const float len = b.frame - a.frame;
        if (len <= 0) return b.value;
        const float t = (f - a.frame) / len, t2 = t * t, t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * a.value + (t3 - 2 * t2 + t) * a.slope * len + (-2 * t3 + 3 * t2) * b.value +
               (t3 - t2) * b.slope * len;
    }
    return k[n - 1].value;
}

// (PaCommonBtnTitle_00's curves, as the layout keeps them)
constexpr Key kSelect[] = {{0, 0, 0}, {1, 1, 0}};
constexpr Key kUnSelect[] = {{0, 1, 0}, {8, 0, 0}};
constexpr Key kLoop[] = {{0, 0.6f, 0}, {20, 0.5f, 0}, {40, 0.6f, 0}};
constexpr Key kDecideDeco[] = {{8, 1, 0}, {8, 1, -0.04341f}, {16, 0.85f, 0}};
constexpr Key kDecideGlow[] = {{8, 1, 0}, {8, 1, -0.01246f}, {16, 0.95f, 0}};
constexpr Key kDecideFlash[] = {{0, 0, 0}, {2, 0, 0}, {16, 1, 0}};
// (Title_00's: the list coming in)
constexpr Key kListIn[] = {{0, 0, 0.02f}, {15, 1, 0.14f}};
template <int N>
inline float curve(const Key (&k)[N], float f) { return curve(k, N, f); }

constexpr int64_t kFrameNs = 16666667;                  // (a frame of the layout's: 1/60 s)
constexpr int64_t kDecideNs = 16 * kFrameNs;            // (Decide's length: the Mods screen opens after it)
constexpr int64_t kLoopNs = 40 * kFrameNs;
constexpr int64_t kListGoneNs = 150000000;              // (the list not seen for this long: it went. As mods_list_fresh)
constexpr int64_t kListShownNs = kListGoneNs + 8 * kFrameNs;   // (...and the entry has gone with it after this)
inline float frames(int64_t ns) { return (float)((double)ns / (double)kFrameNs); }

/// How far into the list's In (ns) it shows `amount` (0 to 1): its curve rises all the way, so a search of its 15 frames
inline int64_t in_at(float amount) {
    if (amount <= 0) return 0;
    float lo = 0, hi = 15;
    for (int i = 0; i < 24; i++) {
        const float mid = (lo + hi) * 0.5f;
        (curve(kListIn, mid) < amount ? lo : hi) = mid;
    }
    return (int64_t)((double)hi * (double)kFrameNs);
}

/// The entry's state from one drawing to the next: how chosen it looks (focus) and how much of it shows with the list
struct Anim {
    bool sel = false;          // (it had the cursor at the last step)
    float from = 0;            // (focus when the cursor last came or went)
    int64_t since_ns = 0;      // (...and when)
    bool there = false;        // (the list was seen at the last step)
    int64_t list_ns = 0;       // (when the list came)
    float gone_from = 0;       // (shown when the list went)
    float focus = 0, shown = 0;
    /// One drawing at now: whether the entry has the cursor, how long since the host last told of the list
    void step(int64_t now, bool selected, int64_t list_age_ns) {
        if (!since_ns || selected != sel) from = since_ns ? focus : (selected ? 1.0f : 0.0f), sel = selected, since_ns = now;
        const float f = frames(now - since_ns);
        focus = sel ? std::max(from, curve(kSelect, f)) : from * curve(kUnSelect, f);
        const bool seen = list_age_ns < kListGoneNs;
        if (seen && !there) list_ns = now - in_at(shown);   // ((Triforce, 11-19) from where it is: no blink to nothing)
        if (!seen && there) gone_from = shown;
        there = seen;
        shown = seen ? curve(kListIn, frames(now - list_ns)) : gone_from * curve(kUnSelect, frames(list_age_ns - kListGoneNs));
        shown = std::clamp(shown, 0.0f, 1.0f), focus = std::clamp(focus, 0.0f, 1.0f);
    }
    /// LoopRandom's value now (it runs from the cursor's coming)
    float loop(int64_t now) const { return curve(kLoop, frames((now - since_ns) % kLoopNs)); }
};

/// Decide at t ns after the press: the filigree's size, its glow's, the band's sparkle (0 to 1)
struct Decide {
    float deco = 1, glow = 1, flash = 0;
};
inline Decide decide_at(int64_t t) {
    Decide d;
    if (t < 0) return d;
    const float f = std::min(frames(t), 16.0f);
    d.deco = curve(kDecideDeco, f), d.glow = curve(kDecideGlow, f), d.flash = curve(kDecideFlash, f);
    return d;
}

}   // namespace mods_entry
