// (hookshot) Which gamepad buttons confirm and go back in the Insert menu: the ones the Switch's A and B are bound to (the
// Controls page, the Layout preset), so the menu answers to A and B as the game does. With the Nintendo layout, the default,
// A is the right face button and B the bottom one, as on a Switch; with Xbox / PC, A is the bottom one. A binding the menu
// cannot use (none, or not a face button: the menu keeps the D-pad, Home and the shoulders) falls back to the bottom button
// confirming and the right one going back. Plain arithmetic: tests/test_menu_keys.py checks it with the real menu_tick's use.
#pragma once

namespace mkwvk::menu_keys {

// (the gamepad's face buttons by position, as vk_api.h's HOS_PAD_SOUTH ... HOS_PAD_NORTH)
enum { kSouth = 0, kEast = 1, kWest = 2, kNorth = 3 };

/// a and b: the pad positions the Switch's A and B are bound to (-1 none); *confirm, *back: the menu's
inline void confirm_back(int a, int b, int* confirm, int* back) {
    const bool face_a = a >= kSouth && a <= kNorth, face_b = b >= kSouth && b <= kNorth;
    *confirm = face_a ? a : kSouth;
    *back = face_b && b != *confirm ? b : *confirm == kEast ? kSouth : kEast;
}

/// The button prompts' dot for a position (vk_ui.h glyph: 0 the top, 1 the right, 2 the left, 3 the bottom)
inline int dot(int position) {
    switch (position) {
    case kNorth: return 0;
    case kEast: return 1;
    case kWest: return 2;
    default: return 3;
    }
}

/// ((Triforce, 11-19) A key read two ways, its state (down: GetAsyncKeyState at each tick) and the host's queue of presses
/// (queued: a press the window's thread noted since the last tick, a little after the state shows it), as one press: new
/// when the key was not down at the last tick and is down now or was pressed between the ticks. taken: this tick's press is
/// counted where both ways may give it (the Mods entries' game keys, vk_menu.cpp menu_tick); its queued press, still to
/// come, is then not a press again within echo_ns (on that page or the next one). *echo: the queued press was that one
struct KeyPress {
    bool held = false;
    long long echo_at = 0;   // (when a press was counted on the state alone: its queued press still to come; 0 none)
};
constexpr long long kEchoNs = 250000000LL;
inline bool key_press(KeyPress& k, bool down, bool queued, bool taken, bool has_queue, long long now, bool* echo) {
    *echo = queued && k.echo_at && now - k.echo_at < kEchoNs;
    if (*echo) queued = false, k.echo_at = 0;
    const bool went = !k.held && (down || queued);
    k.held = down;
    if (taken && went && !queued && has_queue) k.echo_at = now;
    return went;
}

} // namespace mkwvk::menu_keys
