"""The options' own sounds, the menu's side (gpu/vk/menu_sound_events.h, gpu/vk/vk_menu.cpp): which of the seven a tick
of the menu asks for, after NX2navi's mapping (its runtime/ui/menu.cpp and option_rows.cpp): open and close; cancel; decide
when something is pressed; the cursor when the selection moves and when a value changes; the tab moves left and right.
Inside the game's own Options screen, a Back that leaves our tabs is the game's Back too, with the game's sound: none of
ours over it. And the queue the sounds wait in for the host's sound thread: in order, bounded, a stale one dropped, one
thread putting while another takes.

menu_sound_events.h is compiled alone and run. The Audio tab's Menu sounds row (0 to 100, 80 by default, under Master
volume) is cut out of vk_menu.cpp with its label and the options file's reading, and run; the source is read for the
hooks in menu_tick (the state before and after, nothing put with the row at 0), the renderer's two options, and the
row's lines in readme.md and docs/development.md. tests/test_menu_sound.py has the host's side.

    python -I -B tests/test_menu_sound_events.py [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


EVENTS = r'''#include "@ROOT@/gpu/vk/menu_sound_events.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace menu_sound;
static State rows(int tab, int cursor, int64_t value = 0) {
    State s;
    s.open = true, s.tab = tab, s.cursor = cursor, s.value = value;
    return s;
}
static State closed() { return State{}; }
static State embedded(State s) { s.embedded = true; return s; }
static State on_page(State s, int page, int cursor = 0, int group = 0, int mode = kModeNone) {
    s.page = page, s.cursor = cursor, s.group = group, s.mode = mode;
    return s;
}
static State with_mode(State s, int mode) { s.mode = mode; return s; }
struct In : Input {
    In& toggle_() { toggle = true; return *this; }
    In& back_() { back = true; return *this; }
    In& confirm_() { confirm = true; return *this; }
    In& tl() { tab_left = true; return *this; }
    In& tr() { tab_right = true; return *this; }
    In& l() { left = true; return *this; }
    In& r() { right = true; return *this; }
    In& move_() { move = true; return *this; }
    In& clear_() { clear = true; return *this; }
    In& click_() { click = true; return *this; }
    In& drag_() { drag = true; return *this; }
};
int main() {
    /* closed before and after: the keys are the game's, whatever they were */
    CHECK(sound_of(closed(), closed(), In().toggle_()) == kNone && sound_of(closed(), closed(), In().confirm_().move_()) == kNone);
    CHECK(sound_of(embedded(closed()), closed(), In().tl()) == kNone);
    /* opened and closed over the game (Insert, F1, Home) */
    CHECK(sound_of(closed(), rows(0, 0), In().toggle_()) == kOpen);
    CHECK(sound_of(rows(2, 3), closed(), In().toggle_()) == kClose);
    CHECK(sound_of(rows(2, 3), closed(), In().back_()) == kClose);
    CHECK(sound_of(embedded(rows(2, 3)), closed(), In().toggle_()) == kClose);
    /* from the game's own list in its Options screen: L to our last tab, R to our first, a tab's move */
    CHECK(sound_of(closed(), embedded(rows(0, 0)), In()) == kTabRight);
    CHECK(sound_of(closed(), embedded(rows(5, 0)), In()) == kTabLeft);
    /* ...and back to it: past our first tab with L, our last with R, a click on its tab */
    CHECK(sound_of(embedded(rows(0, 1)), closed(), In().tl()) == kTabLeft);
    CHECK(sound_of(embedded(rows(5, 1)), closed(), In().tr()) == kTabRight);
    CHECK(sound_of(embedded(rows(3, 1)), closed(), In().click_()) == kTabLeft);
    /* Back on one of ours there is the game's Back too: the game's sound, none of ours over it; nor when its screen went */
    CHECK(sound_of(embedded(rows(3, 1)), closed(), In().back_()) == kNone);
    CHECK(sound_of(embedded(rows(3, 1)), closed(), In().back_().click_()) == kNone);   /* (a right click with a left) */
    CHECK(sound_of(embedded(rows(3, 1)), closed(), In()) == kNone);
    /* closed by a press that acts (a map to travel to, an event, Restart now, the age switched by Left or Right) */
    CHECK(sound_of(on_page(rows(6, 0), 3, 40), closed(), In().confirm_()) == kDecide);
    CHECK(sound_of(rows(6, 9), closed(), In().r()) == kDecide && sound_of(embedded(rows(6, 9)), closed(), In().l()) == kDecide);
    /* the cursor: another row, a value changed with Left or Right, a page, the ends; nothing when nothing moved */
    CHECK(sound_of(rows(1, 2), rows(1, 3), In().move_()) == kCursor);
    CHECK(sound_of(rows(1, 2, 50), rows(1, 2, 45), In().l()) == kCursor);
    CHECK(sound_of(rows(1, 2, 50), rows(1, 2, 55), In().r()) == kCursor);
    CHECK(sound_of(rows(1, 2, 100), rows(1, 2, 100), In().r()) == kNone);   /* (a row that stops at its end) */
    CHECK(sound_of(rows(1, 0), rows(1, 0), In().move_()) == kNone);
    CHECK(sound_of(on_page(rows(0, 0), 1, 9), on_page(rows(0, 0), 1, 9), In().clear_()) == kNone);
    State bound = on_page(rows(3, 0), 1, 9);
    bound.value = 77;
    CHECK(sound_of(on_page(rows(3, 0), 1, 9), bound, In().clear_()) == kCursor);   /* (a binding taken away) */
    /* the mouse resting on a row moves the cursor without a sound, as NX2navi's does; a click on it is a press */
    CHECK(sound_of(rows(1, 2), rows(1, 5), In()) == kNone);
    CHECK(sound_of(rows(1, 2), rows(1, 5), In().click_()) == kDecide);
    CHECK(sound_of(rows(1, 2, 0), rows(1, 2, 1), In().click_().r()) == kDecide);   /* (a click on a value's half) */
    CHECK(sound_of(rows(1, 2), rows(1, 2), In().click_()) == kNone);               /* (on nothing) */
    /* a slider drawn along: the cursor's tick as the value changes */
    CHECK(sound_of(rows(2, 0, 40), rows(2, 0, 45), In().drag_()) == kCursor && sound_of(rows(2, 0, 45), rows(2, 0, 45), In().drag_()) == kNone);
    /* decide: confirm on a row (a toggle flipped, a save row whose answer is waited for, a page's card) */
    CHECK(sound_of(rows(1, 2, 0), rows(1, 2, 1), In().confirm_()) == kDecide);
    CHECK(sound_of(rows(4, 7), rows(4, 7), In().confirm_()) == kDecide);
    CHECK(sound_of(rows(4, 1), with_mode(rows(4, 1), kModeConfirm), In().confirm_()) == kDecide);   /* (Restart: the first press) */
    /* the tabs: L and R, Q and E, Tab, the strip's arrows; a click on a tab by where it is */
    CHECK(sound_of(rows(1, 0), rows(2, 0), In().tr()) == kTabRight && sound_of(rows(1, 0), rows(0, 3), In().tl()) == kTabLeft);
    CHECK(sound_of(rows(5, 0), rows(0, 0), In().tr()) == kTabRight);   /* (around: still the right's) */
    CHECK(sound_of(rows(1, 0), rows(4, 0), In().click_()) == kTabRight && sound_of(rows(4, 0), rows(1, 0), In().click_()) == kTabLeft);
    /* a page's sections, which L and R turn (the inventory, the teleport list, a name page), and their arrows clicked */
    CHECK(sound_of(on_page(rows(5, 0), 4, 3, 1), on_page(rows(5, 0), 4, 20, 2), In().tr()) == kTabRight);
    CHECK(sound_of(on_page(rows(5, 0), 3, 30), on_page(rows(5, 0), 3, 12), In().tl()) == kTabLeft);
    CHECK(sound_of(on_page(rows(5, 0), 6, 3, 2), on_page(rows(5, 0), 6, 0, 1), In().click_()) == kTabLeft);
    /* pages: opened by a press (decide), left by Back (cancel); the Motion page, whose row and Back take the press */
    CHECK(sound_of(rows(3, 4), on_page(rows(3, 4), 1), In().confirm_()) == kDecide);
    CHECK(sound_of(on_page(rows(3, 4), 1, 6), rows(3, 4), In().back_()) == kCancel);
    CHECK(sound_of(rows(3, 2), on_page(rows(3, 2), 7), In()) == kDecide);
    CHECK(sound_of(on_page(rows(3, 2), 7, 1), rows(3, 2), In()) == kCancel);
    /* out of a mode by Back: an amount, a flag's On and Off, a Restart waiting for its second press */
    CHECK(sound_of(on_page(rows(5, 0), 4, 3, 0, kModeEdit), on_page(rows(5, 0), 4, 3), In().back_()) == kCancel);
    CHECK(sound_of(with_mode(rows(4, 1), kModeConfirm), rows(4, 1), In().back_()) == kCancel);
    CHECK(sound_of(rows(4, 1), rows(4, 1), In().back_()) == kNone);
    /* a mode that acts on each press of Left and Right (an amount, a flag): the cursor's tick each time */
    CHECK(sound_of(on_page(rows(5, 0), 4, 3, 0, kModeEdit), on_page(rows(5, 0), 4, 3, 0, kModeEdit), In().r()) == kCursor);
    CHECK(sound_of(on_page(rows(5, 0), 4, 3), on_page(rows(5, 0), 4, 3), In().r()) == kNone);
    /* a binding waits for its input: the keys meanwhile are that input; bound (decide), given up (cancel), timed out (none) */
    const State listen = on_page(rows(3, 0), 1, 9, 0, kModeListen);
    CHECK(sound_of(listen, listen, In().move_().confirm_().tr()) == kNone);
    CHECK(sound_of(listen, bound, In().move_()) == kDecide);
    CHECK(sound_of(listen, on_page(rows(3, 0), 1, 9), In().back_()) == kCancel);
    CHECK(sound_of(listen, on_page(rows(3, 0), 1, 9), In()) == kNone);
    CHECK(sound_of(on_page(rows(3, 0), 1, 9), listen, In().confirm_()) == kDecide);   /* (a binding chosen: it listens) */
    /* the restart question: L, R and the arrows choose between its buttons (the cursor); closing asks it (cancel) */
    CHECK(sound_of(on_page(rows(2, 0), 5, 0), on_page(rows(2, 0), 5, 1), In().tr()) == kCursor);
    CHECK(sound_of(on_page(rows(2, 0), 5, 1), on_page(rows(2, 0), 5, 0), In().l()) == kCursor);
    CHECK(sound_of(rows(2, 0), on_page(rows(2, 0), 5), In().toggle_()) == kCancel);
    CHECK(sound_of(rows(2, 0), on_page(rows(2, 0), 5), In().back_()) == kCancel);
    CHECK(sound_of(on_page(rows(2, 0), 5), closed(), In().toggle_()) == kClose);   /* (Later, by the key that closes) */
    CHECK(sound_of(on_page(rows(2, 0), 5), closed(), In().confirm_()) == kDecide);
    std::puts("sound_of: NX2navi's mapping, the game's own Options screen, the pages and modes");

    /* the queue: one of the seven, in order; 16 wait, a 17th is dropped; a stale one is not played */
    Queue q;
    CHECK(!q.put(kNone, 0) && !q.put(-1, 0) && !q.put(kTabRight + 1, 0) && q.take(0) == kNone);
    for (int i = 0; i < kQueue; i++) CHECK(q.put(1 + i % 7, 1000));
    CHECK(!q.put(kOpen, 1000));
    for (int i = 0; i < kQueue; i++) CHECK(q.take(2000) == 1 + i % 7);
    CHECK(q.take(2000) == kNone);
    CHECK(q.put(kCursor, 0) && q.put(kDecide, kStaleNs) && q.put(kCancel, kStaleNs + 10));
    CHECK(q.take(kStaleNs + 1) == kDecide);   /* (the cursor a quarter second and more ago: dropped) */
    CHECK(q.take(kStaleNs + 10 + kStaleNs + 1) == kNone);   /* (and the cancel too, by now) */
    for (int round = 0; round < 1000; round++) {   /* (around the ring many times: the counters wrap as they will) */
        CHECK(q.put(1 + round % 7, round) && q.put(1 + (round + 3) % 7, round));
        CHECK(q.take(round) == 1 + round % 7 && q.take(round) == 1 + (round + 3) % 7);
    }
    {   /* one thread puts, another takes: every sound once, in order */
        Queue shared;
        constexpr int kN = 200000;
        std::thread put([&] {
            for (int i = 0; i < kN; i++)
                while (!shared.put(1 + i % 7, 0)) std::this_thread::yield();
        });
        int got = 0;
        while (got < kN) {
            const int s = shared.take(0);
            if (s == kNone) { std::this_thread::yield(); continue; }
            CHECK(s == 1 + got % 7);
            got++;
        }
        put.join();
        CHECK(shared.take(0) == kNone);
    }
    std::puts("queue: in order, bounded, stale ones dropped, across two threads");
    return 0;
}
'''

ROW = r'''#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
enum MenuTab { TAB_AUDIO };
@OPTIONS@
static int output_choice(int v) { return v; }
@ROW_STRUCT@;
@TEXT@
Row rows[] = {@ROW@};
@INI@
static int parse(const char* text) {
    opts.menu_sounds = 80;
    char line[80];
    std::snprintf(line, sizeof line, "%s", text);
    char* eq = std::strchr(line, '=');
    *eq = 0;
    @LOOP@
    return opts.menu_sounds;
}
int main() {
    const Row& r = rows[0];
    CHECK(opts.menu_sounds == 80);   /* (the default: NX2navi's menu_sound_volume) */
    CHECK(!std::strcmp(ini_key(r), "menu_sounds") && ini_row(r));
    CHECK(r.lo == 0 && r.hi == 100 && r.step == 5 && !r.restart && r.preset_exempt && r.cheat < 0 && !r.heading);
    CHECK(parse("menu_sounds=0\n") == 0 && parse("menu_sounds=35\n") == 35 && parse("menu_sounds=100\n") == 100);
    CHECK(parse("menu_sounds=250\n") == 100 && parse("menu_sounds=-3\n") == 0 && parse("menu_sounds=x\n") == 0);
    CHECK(parse("menu_sound=20\n") == 80 && parse("master_volume=20\n") == 80);
    CHECK(!std::strcmp(r.text(0), "Off") && !std::strcmp(r.text(5), "5%") && !std::strcmp(r.text(80), "80%") &&
          !std::strcmp(r.text(100), "100%"));
    std::puts("Menu sounds row: menu_sounds=, 0 to 100 (clamped), 80 by default, Off at 0, at once");
    return 0;
}
'''


def run(cxx, src, name, tmp, flags):
    path = Path(tmp) / name
    path.write_text(src, encoding="utf-8")
    exe = Path(tmp) / (name + ".exe")
    subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", *flags, str(path), "-o", str(exe)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    check(r.returncode == 0, name + ": " + r.stderr.strip())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    vk = ROOT / "gpu" / "vk"
    menu = (vk / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (vk / "vk_menu.h").read_text(encoding="utf-8")
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    guide = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")

    # (the hooks: the state before the tick's input and after it, one sound at most, none put with the row at 0)
    tick = block(menu, "void menu_tick() {")
    before = tick.index("const menu_sound::State sound_before = sound_state();")
    check(before < tick.index("bool press[K_N] = {};"), "menu_tick does not take the options' state before its input")
    put = tick.index("if (opts.menu_sounds > 0) sound_queue.put(menu_sound::sound_of(sound_before, sound_state(), in), prof_now());")
    check(tick.index("else if (press[7] && share_row(r)) share_press(") < put < tick.index("names_tick();"),
          "the sound is not told after the tick's input (and with the Menu sounds row above 0 alone)")
    check(tick.count("sound_queue.put(") == 1, "menu_tick puts a sound in more than one place")
    check("int menu_take_sound() { return sound_queue.take(prof_now()); }" in menu, "menu_take_sound does not take from the queue")
    check("if (id == HOS_OPT_MENU_SOUND) return menu_take_sound();" in replay and
          "if (id == HOS_OPT_MENU_SOUND_VOLUME) return std::clamp(opts.menu_sounds, 0, 100);" in replay,
          "the renderer does not answer the host's two options: the sounds asked for, the Menu sounds row")
    check("static_assert(menu_sound::kOpen == HOS_MENU_SOUND_OPEN" in menu, "vk_menu.cpp does not hold the sounds against vk_api.h's")

    # (the row: under Master volume on the Audio tab, its default, its line in the readme and the options file's table)
    row = re.search(r'\{TAB_AUDIO, "Menu sounds", &opts\.menu_sounds, 0, 100, 5, tx_menu_sounds,\s*"[^{}]*?", nullptr, false, true\},',
                    menu)
    check(row, "the Audio tab has no Menu sounds row (0 to 100 in fives, at once, not the quality preset's)")
    master = menu.index('{TAB_AUDIO, "Master volume", &opts.volume,')
    following = re.search(r'\{TAB_\w+, "', menu[master + 10:])
    check(following and master + 10 + following.start() == row.start(), "the Menu sounds row is not the one under Master volume")
    check(re.search(r"\bint menu_sounds = 80;", header), "the Menu sounds row's default is not 80")
    check("\n| Menu sounds | Off, 5% to 100%" in readme, "readme.md's settings table has no Menu sounds row")
    check("| `menu_sounds` |" in guide, "docs/development.md's options table has no menu_sounds")

    init = block(menu, "void menu_init(")
    loop = block(init, "for (Row& r : rows) {")
    check("ini_key(r)" in loop, "menu_init's options loop no longer reads rows by ini_key")
    options = "struct Options { int output_res = 0, resolution = 0, fps_cap = 0, upscale = 0, dlss = 0, language = 0, menu_sounds = 80; } opts;"
    src = ROW.replace("@OPTIONS@", options).replace("@ROW_STRUCT@", block(menu, "struct Row {")) \
        .replace("@TEXT@", block(menu, "const char* tx_percent(int v) {") + "\n" +
                 re.search(r"const char\* tx_menu_sounds\(int v\) \{[^\n]*\}", menu).group(0)) \
        .replace("@ROW@", row.group(0)) \
        .replace("@INI@", block(menu, "bool ini_row(") + "\n" + block(menu, "const char* ini_key(")) \
        .replace("@LOOP@", loop)
    with tempfile.TemporaryDirectory(prefix="menu-sound-events-") as tmp:
        run(a.cxx, EVENTS.replace("@ROOT@", ROOT.as_posix()), "events.cpp", tmp, [] if os.name == "nt" else ["-pthread"])
        run(a.cxx, src, "row.cpp", tmp, [])
    print("PASS: the options' own sounds: which a tick asks for, their queue, the Menu sounds row")


if __name__ == "__main__":
    main()
