// (Triforce) The System tab's rows for the cheats of the mods' cheat files: what the host names (host/mod_cheats.c), what
// the rows show, and what goes back to the host when the player switches one (vk_api.h HOS_OPT_MODCHEAT_*).
//
// The cheats are the host's: it reads the files at the start and runs the ones that are on, once a game frame. The menu
// has a row for each, with the file's own name: the row table is fixed, so it holds kSlots rows for them that are not
// shown until the host has named its cheats, and a file's cheats past kSlots have no row (the host's log says so). A
// name arrives a byte a call, as the save rows' lines do, with the line the row shows under its help after a line feed.
// A row is On | Off like the menu's toggles; a master code and a cheat that cannot run here have a row that cannot be
// changed. Which cheats are on is kept by the host (triforce_cheats.txt), not in the options file.
//
// The host's calls (text, add, done, take) come on the game's thread, the rows are drawn and pressed on the renderer's:
// names and lines are written before count says the rows are there and never after; the switches are atomics.
//
// No ImGui and no Vulkan: tests/test_mod_cheat_rows.py compiles it alone.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>

namespace mod_cheat_rows {
constexpr int kSlots = 96;
constexpr int kName = 80;    // (a cheat's name is at most 63 bytes; " (cheat)" after one that is a row's of the menu's own)
constexpr int kLine = 200;   // (the line under the row's help)
constexpr int kOn = 1, kAlways = 2, kRefused = 4;   // (vk_api.h HOS_MODCHEAT_*)

struct List {
    char names[kSlots][kName] = {};
    char lines[kSlots][kLine] = {};
    unsigned char bits[kSlots] = {};
    std::atomic<int> on[kSlots] = {};
    std::atomic<int> count{0};   // (the rows shown: 0 until the host's list is whole)
    // the host's side
    char in[kName + kLine] = {};
    int in_n = 0, added = 0;
    int told[kSlots] = {};       // (what the host knows of each switch)

    /// HOS_OPT_MODCHEAT_TEXT: the next byte of the row being named
    void text(int byte) {
        if (byte > 0 && byte < 256 && in_n + 1 < (int)sizeof in) in[in_n++] = (char)byte;
    }
    /// HOS_OPT_MODCHEAT_ADD: the bytes so far are the next row's name and, after a line feed, its line. taken: names a
    /// row may not have (rows of the menu's own that act by their name): such a cheat is shown with " (cheat)" after it
    void add(int row_bits, const char* const* taken = nullptr, int n_taken = 0) {
        in[in_n] = 0;
        in_n = 0;
        if (count.load(std::memory_order_acquire) || added >= kSlots) return;   // (shown already, or no row left)
        const int slot = added++;
        char* feed = strchr(in, '\n');
        if (feed) *feed = 0;
        int n = (int)strlen(in);
        if (n > 63) {   // (not in the middle of a letter)
            n = 63;
            while (n && ((unsigned char)in[n] & 0xC0) == 0x80) n--;
        }
        memcpy(names[slot], in, (size_t)n);
        names[slot][n] = 0;
        if (!n) strcpy(names[slot], "(no name)");
        for (int i = 0; i < n_taken; i++)
            if (!strcmp(names[slot], taken[i])) strcat(names[slot], " (cheat)");
        if (feed) {
            size_t ln = strlen(feed + 1);
            if (ln > kLine - 1) ln = kLine - 1;
            memcpy(lines[slot], feed + 1, ln);
            lines[slot][ln] = 0;
            for (char* p = lines[slot]; *p; p++)
                if (*p == '\n') *p = ' ';
        }
        bits[slot] = (unsigned char)(row_bits & (kOn | kAlways | kRefused));
        const int is_on = (row_bits & kAlways) || ((row_bits & kOn) && !(row_bits & kRefused));
        on[slot].store(is_on, std::memory_order_relaxed);
        told[slot] = is_on;
    }
    /// HOS_OPT_MODCHEAT_COUNT: the list is whole: the rows are shown from now on
    void done(int n) {
        in_n = 0;
        if (count.load(std::memory_order_acquire)) return;
        count.store(n < 0 ? 0 : n < added ? n : added, std::memory_order_release);
    }
    int shown() const { return count.load(std::memory_order_acquire); }
    /// A row that cannot be changed: a master code, or a cheat that cannot run here
    bool fixed(int slot) const { return slot < 0 || slot >= kSlots || (bits[slot] & (kAlways | kRefused)); }
    bool is_on(int slot) const { return slot >= 0 && slot < kSlots && on[slot].load(std::memory_order_relaxed); }
    /// What a row that cannot be changed shows in its value's place
    const char* fixed_text(int slot) const { return slot >= 0 && slot < kSlots && (bits[slot] & kRefused) ? "Not supported" : "Always on"; }
    const char* line(int slot) const { return slot >= 0 && slot < kSlots ? lines[slot] : ""; }
    /// The slot of a row by its name's address (the rows' names are this list's own), -1 for any other row
    int slot_of(const char* name) const {
        const uintptr_t first = (uintptr_t)names[0], at = (uintptr_t)name;
        return at >= first && at - first < sizeof names ? (int)((at - first) / kName) : -1;
    }
    /// A press on a row: Left is On and Right is Off, as they are drawn; confirm flips. true when the switch changed
    bool press(int slot, bool left, bool right, bool confirm) {
        if (slot < 0 || slot >= shown() || fixed(slot)) return false;
        const int was = on[slot].load(std::memory_order_relaxed);
        const int now = confirm ? !was : left ? 1 : right ? 0 : was;
        if (now == was) return false;
        on[slot].store(now, std::memory_order_relaxed);
        return true;
    }
    /// HOS_OPT_MODCHEAT_ASK: the next switch that is not as the host knows it, (slot + 1) * 2 + its state; 0 none
    int take() {
        const int n = shown();
        for (int slot = 0; slot < n; slot++) {
            const int now = on[slot].load(std::memory_order_relaxed);
            if (fixed(slot) || now == told[slot]) continue;
            told[slot] = now;
            return (slot + 1) * 2 + (now ? 1 : 0);
        }
        return 0;
    }
};
}   // namespace mod_cheat_rows
