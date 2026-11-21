// (Triforce) The options' own sounds, the game's interface sounds, after NX2navi's menu sounds: which sound a tick of the
// menu asks for, and where the sounds wait until the host's sound thread takes them (vk_api.h HOS_OPT_MENU_SOUND;
// host/menu_sound.c reads the game's files and plays them).
//
// Which sound, as NX2navi has it: the options opening and closing; another tab; the cursor on another row, card or button,
// and a value changed with Left and Right; a press on something (confirm, or a click); a step back. The menu's input is
// one long function (vk_menu.cpp menu_tick) that changes its state in many places, so the sound is told from what the
// tick changed: the state is taken before and after it (State), and with the keys the tick saw (Input) it names one
// sound at most (sound_of). The mouse resting on a row moves the cursor without a sound, as NX2navi's does.
//
// Inside the game's own Options screen the options are tabs beside the game's own list (vk_menu.cpp game_options_shown):
// going between that list and our tabs with L and R is a tab's move, not an opening; and Back on one of our tabs is the
// game's Back too (host/nn_hid.c), which leaves the screen with the game's own sound, so ours is not played over it.
// While the game's own list is shown the input and the sounds are the game's.
//
// The queue: the menu's thread puts, the host's sound thread takes, no lock (one of each). It holds kQueue sounds; one
// more is dropped. A sound older than kStaleNs when it is taken is not played: with no sound thread to take them
// (the game has not opened its sound yet, or stopped it) the presses of a while ago would otherwise all sound at once.
//
// No ImGui and no Vulkan: tests/test_menu_sound_events.py compiles it alone.
#pragma once
#include <atomic>
#include <cstdint>

namespace menu_sound {
// (vk_api.h HOS_MENU_SOUND_*: vk_menu.cpp holds them against these)
constexpr int kNone = 0, kOpen = 1, kClose = 2, kCursor = 3, kDecide = 4, kCancel = 5, kTabLeft = 6, kTabRight = 7;
// ((Triforce) and the Mods screen's and entry's own, which sound_of never names: the fanfare of a mod installed, and the
// system menu's cursor and "decide" of the game's lists the Mods entry is under. (11-19) The queue took the seven above
// only and dropped these without a word: moving onto Mods, A on it, Quit's dialog and the fanfare were silent)
constexpr int kFanfare = 8, kSysCursor = 9, kSysDecide = 10, kLast = kSysDecide;

// (State::page: the tabs' rows, a page over them, and the restart question, whose two buttons L and R choose between)
constexpr int kPageRows = 0, kPageAsk = 5;
// (State::mode: what the chosen row or card waits for)
constexpr int kModeNone = 0,
              kModeListen = 1,    // the Controls page: the new input of a binding
              kModeEdit = 2,      // Left and Right act on it each press: an item's amount, a flag's On and Off
              kModeConfirm = 3;   // Restart or Quit: its second press

/// What the options show, as far as their sounds go
struct State {
    bool open = false;       // the options are open
    bool embedded = false;   // ...as tabs inside the game's own Options screen
    int page = kPageRows;    // what is shown: the tabs' rows, or a page over them (vk_menu.cpp page; the Motion page is one too)
    int tab = 0;             // the tab, of the tabs' rows
    int group = 0;           // a page's section, which L and R turn: the inventory's, the teleport list's, a name page's
    int cursor = 0;          // the row, card, cell or button chosen
    int mode = kModeNone;
    int64_t value = 0;       // what the chosen row is set to
    bool operator==(const State&) const = default;
};

/// What a tick's input was, after the mouse's clicks were made the keys they stand for
struct Input {
    bool toggle = false;      // the keys that open and close the options over the game (Insert, F1, a gamepad's Home)
    bool back = false;        // Esc, the button B is bound to, a right click
    bool confirm = false;     // Enter, the button A is bound to, a click on what acts by one
    bool tab_left = false, tab_right = false;   // L and R, Q and E, Tab, the tab strip's arrows
    bool left = false, right = false;           // Left and Right, a click on a value's left or right half
    bool move = false;        // Up, Down, Page Up, Page Down, Home, End
    bool clear = false;       // Delete, Backspace (a binding taken away)
    bool click = false;       // the left mouse button went down
    bool drag = false;        // the left mouse button is held on a slider
};

/// The sound of a tick that found the options as `b` and left them as `a`: kNone, or one of the seven
inline int sound_of(const State& b, const State& a, const Input& in) {
    if (!b.open && !a.open) return kNone;   // (closed: the keys are the game's)
    if (!b.open)   // (opened: over the game by its keys, or from the game's own list by L or R, which is a tab's move)
        return in.toggle ? kOpen : a.tab == 0 ? kTabRight : kTabLeft;
    if (!a.open) {
        // (closed by a press that acted: a map to travel to, an event, Restart now, an age switched, the question's Later)
        if (in.confirm || in.left || in.right) return kDecide;
        if (!b.embedded || in.toggle) return kClose;
        // (our tabs inside the game's screen: on to the game's own list with L or R, or by a click on its tab, the first)
        if (in.tab_left || in.tab_right) return in.tab_left ? kTabLeft : kTabRight;
        // (Back is the game's Back too and has the game's sound; and the game's screen may have gone by itself)
        return in.click && !in.back ? kTabLeft : kNone;
    }
    const bool changed = !(a == b);
    if (b.mode == kModeListen) {   // (a binding waited for its input: a key pressed meanwhile is that input, not the menu's)
        if (a.mode == kModeListen) return kNone;
        return a.value != b.value ? kDecide : in.back ? kCancel : kNone;   // (bound; given up; or nothing came in time)
    }
    if ((in.back || in.toggle) && changed) return kCancel;   // (out of a page, a mode, or to the restart question)
    const bool tabs = in.tab_left || in.tab_right;
    if (tabs && changed && b.page != kPageAsk) return in.tab_left ? kTabLeft : kTabRight;
    // (a click on a tab, or on a page's section: by where it is from the one before)
    if (in.click && a.page == b.page && a.tab != b.tab) return a.tab > b.tab ? kTabRight : kTabLeft;
    if (in.click && a.page == b.page && a.group != b.group) return a.group > b.group ? kTabRight : kTabLeft;
    // (a page opened, by a row that may have taken the press for itself; a page left, by a Back its tab took for itself)
    if (a.page != b.page) return a.page == kPageRows ? kCancel : kDecide;
    if (in.confirm) return kDecide;
    if (in.click && (changed || in.left || in.right)) return kDecide;
    const bool arrows = in.move || in.left || in.right || in.clear || tabs;
    if (arrows && changed) return kCursor;
    if ((in.left || in.right) && a.mode == kModeEdit && b.mode == kModeEdit) return kCursor;   // (acts each press)
    if (in.drag && a.value != b.value) return kCursor;   // (a slider drawn along)
    return kNone;
}

constexpr int kQueue = 16;
constexpr int64_t kStaleNs = 250000000;   // (a quarter second)

struct Queue {
    struct Entry { int sound; int64_t ns; };
    Entry entries[kQueue] = {};
    std::atomic<unsigned> head{0}, tail{0};   // (taken up to head, put up to tail)

    /// The menu's thread: a sound asked for at now_ns. false when it is none of the sounds (1 to kLast), or the queue is full
    bool put(int sound, int64_t now_ns) {
        if (sound <= kNone || sound > kLast) return false;
        const unsigned t = tail.load(std::memory_order_relaxed);
        if (t - head.load(std::memory_order_acquire) >= kQueue) return false;
        entries[t % kQueue] = {sound, now_ns};
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
    /// The host's sound thread: the oldest sound waiting that is not stale, 0 when there is none
    int take(int64_t now_ns) {
        for (;;) {
            const unsigned h = head.load(std::memory_order_relaxed);
            if (h == tail.load(std::memory_order_acquire)) return kNone;
            const Entry e = entries[h % kQueue];
            head.store(h + 1, std::memory_order_release);
            if (now_ns - e.ns <= kStaleNs) return e.sound;
        }
    }
};
}   // namespace menu_sound
