// (Triforce, 11-19) The Mods and Quit entries under the pause menu's System list, and Quit's dialog, animated as the game
// animates its own System page buttons and its own dialogs. His "the mods and quit tabs ... have no animations either" and
// "the quit menu submenu also has no animations". Each button of the System list (and of the game's dialogs) is the part
// PaCommonBtnThin_00 of the Common archive's layout; a dialog is SystemWindow_00 with three of them, 188 units apart. The
// parts play their own animations; here are their times and amounts, read from the layouts' curves (the keys: a frame, a
// value and a slope, Hermite between them, 60 frames a second, as mods_entry_anim.h reads the title's):
//   Select     8 frames: the cursor (N_Cursor_00) shown from the first, its alpha from 0 to 1 by frame 4
//   UnSelect   14 frames: its alpha from 1 to 0 by frame 8, hidden at 14
//   In         20 frames: the window's question and buttons (N_In_00) from alpha 0 to 1 by frame 15 (slow to start: its
//              slopes 0.02 and 0.14), and rising into place from 38 units under it (122 to 142 at frame 5, 160 at 20)
//   Out        2 frames: their alpha from 1 to 0, straight
// The buttons' cursor does not slide from one to the next: the button let go fades its cursor out while the new one fades
// its in. No ImGui and no Vulkan: tests/test_pause_mods_anim.py compiles it alone.
#pragma once
#include <algorithm>
#include <cstdint>
#include "mods_entry_anim.h"   // (the layouts' curves: Key, curve, frames)

namespace focus_anim {

using mods_entry::curve;
using mods_entry::frames;
using mods_entry::Key;
using mods_entry::kFrameNs;

// (PaCommonBtnThin_00's: N_Cursor_00's alpha)
constexpr Key kSelect[] = {{0, 0, 0}, {4, 1, 0}};
constexpr Key kUnSelect[] = {{0, 1, 0}, {8, 0, 0}};
constexpr int64_t kSelectNs = 8 * kFrameNs, kUnSelectNs = 14 * kFrameNs;
// (SystemWindow_00's: N_In_00's alpha and its height, its place 160 units up)
constexpr Key kInAlpha[] = {{0, 0, 0.02f}, {15, 1, 0.14f}};
constexpr Key kInY[] = {{0, 122, 5.6094f}, {5, 142, 2.50235f}, {20, 160, 0}};
constexpr Key kOutAlpha[] = {{0, 1, -0.5f}, {2, 0, -0.5f}};
constexpr float kInPlace = 160;
constexpr int64_t kInNs = 20 * kFrameNs, kOutNs = 2 * kFrameNs;

/// A button's cursor from one drawing to the next: how much of it shows (N_Cursor_00's alpha, 0 to 1)
struct Focus {
    bool sel = false;          // (it had the cursor at the last step)
    float from = 0;            // (shown when the cursor last came or went)
    int64_t since_ns = 0;      // (...and when)
    float shown = 0;
    /// One drawing at now: whether the button has the cursor. settle: as it stands once its animation is done (a still,
    /// a picture that compares, a window just opened)
    void step(int64_t now, bool selected, bool settle = false) {
        if (settle || !since_ns) {
            sel = selected, from = shown = selected ? 1.0f : 0.0f, since_ns = now;
            return;
        }
        if (selected != sel) from = shown, sel = selected, since_ns = now;
        const float f = frames(now - since_ns);
        // (a change back half way: from where it is, as mods_entry::Anim, not from the curve's own start)
        shown = sel ? std::max(from, curve(kSelect, f)) : from * curve(kUnSelect, f);
        shown = std::clamp(shown, 0.0f, 1.0f);
    }
};

/// A dialog's window t ns after it opened (In) or after it closed (Out, out = true): its alpha, and how far under its place
/// it is (the layout's units, up positive: 0 in place)
struct Window {
    float alpha = 1, drop = 0;
};
inline Window window_at(int64_t t, bool out = false) {
    Window w;
    if (t < 0) t = 0;
    const float f = frames(t);
    if (out) {
        w.alpha = std::clamp(curve(kOutAlpha, f), 0.0f, 1.0f);
        return w;
    }
    w.alpha = std::clamp(curve(kInAlpha, f), 0.0f, 1.0f);
    w.drop = std::max(0.0f, kInPlace - curve(kInY, f));
    return w;
}
/// (the window takes input once its In has played, as the game's)
inline bool window_ready(int64_t t) { return t >= kInNs; }

}   // namespace focus_anim
