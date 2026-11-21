"""The controller, the keys and a test's script on the Mods entry under the game's lists (host/nn_hid.c mods_input,
host/oot_menu.c hos_oot_mods_down, gpu/vk/vk_menu.cpp mods_list_age and menu_tick; no game).

(Triforce, 11-19) The lab's check of port052:
- "pause menu System list, a normal DOWN from Title Screen skips Mods to Quit ... the log says 'the Mods entry has the cursor'
  but the white cursor only appears on Quit. A 16 ms DOWN lands on Mods." The host takes the Down at the last button for
  Mods (the enter word) and, once Mods has the cursor, passes a script's presses on to the menu; a press was new against
  the buttons before, which were taken as none while the game had the cursor, so the Down that came onto Mods, still held,
  was a press of the entry's: on to Quit. It has to work for a controller's press (100 to 300 ms, the D-pad and the stick),
  the keys, the live input file, and a Down held, which repeats at the menu's rate as a pad's.
- "One DOWN from Options on the title did nothing and logged nothing": the game's frames came 80 to 235 ms apart (its window
  behind another one), and the host's 80 ms and the renderer's 150 ms took the list for gone: the Down went to the game,
  or the enter word was dropped, without a line.

This compiles the real code, extracted from the sources, against fakes:
- ((a review of beb9b9e) a Down held from higher up the list goes on onto Mods once the game's cursor is on the last
  button: one word 5 a step (83 ms, 33 past 1.5 s) after it came there and past the first 0.35 s held; a 300 ms press
  from the button above, or one let go before the step, stays on the last button.)
- ((a review of 63a5020) the keys read by their state and the host's queue as one press, menu_keys::key_press: a tap
  between two ticks counts, the queued press of one counted on the state at the tick before does not count again, on the
  Mods screen A opened either.)
- mods_input (nn_hid.c) with the renderer as a fake that takes the enter word at its next tick: a Down held 150 ms or
  300 ms from Title Screen, the D-pad's or the stick's, is no press of the entry's at the first read with Mods chosen, and is
  told as held at each read (the menu repeats it); a 16 ms Down the same; a second Down on Mods is one press; A held as
  the mouse takes Mods is no press; the presses after Mods let the cursor go are the game's again when let go and pressed.
- hos_oot_mods_down (oot_menu.c): the list's last frame is the game's last one (one present since at most) however long
  ago, and within 80 ms as before; three presents and 200 ms after it the list is gone.
- mods_list_age (vk_menu.cpp): 0 while the list was told of at the game's last two frames, its age after.

And reads the source for what only the game shows: the menu repeats the held directions the host tells of while Mods or
its screen has the cursor, as a pad's (0.35 s, then 12 and 30 a second); the keys bound to the game's D-pad, movement and A
act on Mods, Quit and its dialog (and a key's Up that leaves them is not the game's too, mods_input); the enter word not
taken says so in the log.

    python -I -B tests/test_mods_entry_input.py [--cxx clang++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def block(text, head):
    start = text.index(head)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


HID_FIXTURE = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vk_api.h"
static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL:%d %s\n", __LINE__, #x); fails++; } } while (0)
/* the host's options, as the renderer answers them: 1 the menu open, HOS_OPT_MODS_SELECTED Mods has the cursor */
static int menu_open, selected, entered, presses, held_dirs, reports;
int rx_option(int id, int otherwise) {
    if (id == 1) return menu_open;
    if (id == HOS_OPT_MODS_SELECTED) return selected;
    return otherwise;
}
void rx_option_set(int id, int value) {
    if (id == HOS_OPT_MODS_ENTER) entered = value;
    if (id == HOS_OPT_MODS_PRESS) {
        presses |= value & 0xff;
        if (value & HOS_MODS_REPORT) reports++, held_dirs = value >> HOS_MODS_HELD_SHIFT & HOS_MODS_PRESS_DIRS;
    }
}
/* the game's cursor on its list's last button (Title Screen) */
static int on_last = 1;
static int64_t now_ns = 1000000000LL;   /* (a read each 16 ms: read_once) */
int64_t hos_now_ns(void) { return now_ns; }
int hos_oot_mods_down(void) { return on_last; }
%(enum)s;
%(func)s
/* one read: the buttons and stick the devices and the script give; what the game is given */
static uint64_t game;
static int32_t game_stick[4];
static void read_once(uint64_t buttons, int32_t ly) {
    int32_t stick[4] = {0, ly, 0, 0};
    now_ns += 16000000;
    mods_input(&buttons, stick);
    game = buttons, memcpy(game_stick, stick, sizeof stick);
}
/* the renderer's tick between two reads: the enter word taken (Mods chosen, the menu open), its presses read */
static int last_word;
static void tick(void) {
    if (entered == 1 || entered == 5) selected = 1, menu_open = 1, on_last = 0;
    if (entered) last_word = entered;
    entered = 0;
}
static void reset(void) {
    menu_open = selected = entered = presses = held_dirs = reports = last_word = 0, on_last = 1;
    for (int i = 0; i < 8; i++) read_once(0, 0), tick();   /* (nothing held: the statics as after a while of play) */
    presses = reports = 0;
}
/* a Down held ms long from Title Screen, a read every 16 ms (the D-pad's, or the stick's when stick) */
static void down_from_title(int ms, int stick) {
    reset();
    for (int t = 0; t <= ms + 64; t += 16) {
        const int held = t < ms;
        read_once(held && !stick ? 1ULL << B_DOWN : 0, held && stick ? -30000 : 0);
        if (t == 0) CHECK(entered == 1 && !(game >> B_DOWN & 1) && game_stick[1] == 0);   /* (the game never sees it) */
        if (held && t > 0 && selected) CHECK(held_dirs == HOS_MODS_PRESS_DOWN);   /* (told as held: the menu repeats it) */
        if (!held && selected) CHECK(held_dirs == 0);
        tick();
    }
    CHECK(selected && reports > 0);
    CHECK(!(presses & HOS_MODS_PRESS_DOWN));   /* (the Down onto Mods is not the entry's own: no Quit) */
}
/* ((Triforce, 11-19) a review of beb9b9e) a Down held from higher up the list (New Game, Save): the game's own repeat takes
   its cursor onto the last button after above_ms, and the Down goes on onto Mods as a repeat: once held past the menu's
   first delay (350 ms) and a step (83 ms) on the last button, one word, 5; none while held for less. Before, the cursor
   stayed on the last button as long as the Down was held */
static void down_held_from_above(int above_ms, int ms, int stick, int want_ms) {
    reset();
    on_last = 0;
    int words = 0, at = -1;
    for (int t = 0; t <= ms + 64; t += 16) {
        const int held = t < ms;
        if (t >= above_ms && !selected) on_last = 1;   /* (the game's repeat, or its move from the button above) */
        read_once(held && !stick ? 1ULL << B_DOWN : 0, held && stick ? -30000 : 0);
        if (!on_last && !selected) CHECK(held ? (game >> B_DOWN & 1) || game_stick[1] < -16384 : 1);   /* (the game's, up the list) */
        if (on_last && !selected) CHECK(!(game >> B_DOWN & 1) && game_stick[1] == 0);   /* (on the last button: never the game's) */
        if (entered) words++, at = t;
        tick();
    }
    if (want_ms < 0) {
        CHECK(words == 0 && !selected);
    } else {
        CHECK(words == 1 && last_word == 5 && selected && at >= want_ms && at < want_ms + 32);
        CHECK(!(presses & HOS_MODS_PRESS_DOWN));   /* (the repeat's Down onto Mods is no press of the entry's: the menu repeats on) */
    }
}
int main(void) {
    down_held_from_above(500, 1000, 0, 500 + 83);   /* (the game's repeat onto the last button at 500 ms: a step after it) */
    down_held_from_above(500, 1000, 1, 500 + 83);   /* (the stick's) */
    down_held_from_above(16, 300, 0, -1);           /* (a 300 ms press from the button above: the game's move only) */
    down_held_from_above(16, 600, 0, 350);          /* (held on from the button above: the first delay, then Mods) */
    down_held_from_above(500, 560, 0, -1);          /* (let go before the step) */
    down_held_from_above(2000, 2400, 0, 2000 + 33); /* (past 1.5 s held: the fast step) */
    down_from_title(16, 0);
    down_from_title(150, 0);
    down_from_title(300, 0);
    down_from_title(150, 1);
    down_from_title(300, 1);
    /* a second Down on Mods: one press of the entry's (on to Quit) */
    down_from_title(100, 0);
    presses = 0;
    read_once(1ULL << B_DOWN, 0), tick();
    read_once(1ULL << B_DOWN, 0), tick();
    CHECK(presses == HOS_MODS_PRESS_DOWN);
    read_once(0, 0), tick();
    /* the mouse takes Mods with A held (a click's button, or A held as the cursor came): A is no press of the entry's */
    reset();
    read_once(1ULL << B_A, 0);
    selected = menu_open = 1;
    read_once(1ULL << B_A, 0);
    CHECK(!(presses & HOS_MODS_PRESS_A));
    read_once(0, 0), read_once(1ULL << B_A, 0);
    CHECK(presses & HOS_MODS_PRESS_A);
    /* the cursor back to the game with Up held: not the game's until let go */
    presses = 0;
    read_once(1ULL << B_UP, 0);
    selected = menu_open = 0;
    read_once(1ULL << B_UP, 0);
    CHECK(!(game >> B_UP & 1));
    read_once(0, 0), read_once(1ULL << B_UP, 0);
    CHECK(game >> B_UP & 1);
    /* a key bound to the D-pad's Up that took the cursor back (the menu's while it was open: none in the host's read then,
       held as it closes): not the game's until let go, or the game's cursor went up a button too */
    read_once(0, 0);
    selected = menu_open = 1;
    read_once(0, 0);
    selected = menu_open = 0;
    read_once(1ULL << B_UP, 0);
    CHECK(!(game >> B_UP & 1));
    read_once(1ULL << B_UP, 0);
    CHECK(!(game >> B_UP & 1));
    read_once(0, 0), read_once(1ULL << B_UP, 0);
    CHECK(game >> B_UP & 1);
    if (!fails) puts("the Mods entry's input");
    return fails ? 1 : 0;
}
"""

MENU_HOST_FIXTURE = r"""
#include <stdint.h>
#include <stdio.h>
typedef int64_t LONG64;
static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL:%d %s\n", __LINE__, #x); fails++; } } while (0)
LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 v, LONG64 e) { LONG64 o = *p; if (o == e) *p = v; return o; }
LONG64 InterlockedExchange64(volatile LONG64 *p, LONG64 v) { LONG64 o = *p; *p = v; return o; }
static int64_t now_ns = 5000000000LL;
int64_t hos_now_ns(void) { return now_ns; }
volatile LONG64 hos_present_count = 100;
%(state)s
%(func)s
/* the list's frame with its last button focused, as mods_list_tick notes it */
static void list_frame(void) {
    InterlockedExchange64(&mods_last_present, InterlockedCompareExchange64(&hos_present_count, 0, 0));
    InterlockedExchange64(&mods_last_ns, (LONG64)now_ns);
}
int main(void) {
    CHECK(!hos_oot_mods_down());   /* (no list yet) */
    list_frame();
    now_ns += 16000000, hos_present_count++;
    CHECK(hos_oot_mods_down());   /* (the frame before, 16 ms ago) */
    list_frame();
    now_ns += 200000000, hos_present_count++;   /* (the game's next frame 200 ms later: its window behind another one) */
    CHECK(hos_oot_mods_down());
    now_ns += 200000000;   /* (no present since: still that frame) */
    CHECK(hos_oot_mods_down());
    hos_present_count += 2;   /* (three frames past the list's) */
    CHECK(!hos_oot_mods_down());
    list_frame();
    hos_present_count += 5, now_ns += 50000000;   /* (within 80 ms, as before) */
    CHECK(hos_oot_mods_down());
    if (!fails) puts("the list's last frame");
    return fails ? 1 : 0;
}
"""

AGE_FIXTURE = r"""
#include <atomic>
#include <cstdint>
#include <cstdio>
static int fails;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%d %s\n", __LINE__, #x); fails++; } } while (0)
static int64_t now = 9000000000LL;
int64_t prof_now() { return now; }
std::atomic<int64_t> mods_list_ns{0}, mods_list_frame{0};
%(state)s
%(age)s
%(fresh)s
int main() {
    CHECK(!mods_list_fresh());
    mods_list_frame.store(game_frames.load()), mods_list_ns.store(now);   // (a report)
    now += 100000000;
    CHECK(mods_list_fresh() && mods_list_age() == 0);
    now += 135000000, game_frames += 1;   // (235 ms on, the game's next frame)
    CHECK(mods_list_fresh() && mods_list_age() == 0);
    game_frames += 1;
    CHECK(mods_list_fresh());
    game_frames += 1;   // (three frames and no report: gone)
    CHECK(!mods_list_fresh() && mods_list_age() == 235000000);
    mods_list_frame.store(game_frames.load()), mods_list_ns.store(now);
    game_frames += 6, now += 100000000;   // (fast frames: 150 ms as before)
    CHECK(mods_list_fresh() && mods_list_age() == 100000000);
    now += 60000000;
    CHECK(!mods_list_fresh());
    if (!fails) std::puts("the list's age");
    return fails ? 1 : 0;
}
"""


KEYS_FIXTURE = r"""
#include <cstdio>
#include "vk_menu_keys.h"
using namespace mkwvk::menu_keys;
static int fails;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%d %s\n", __LINE__, #x); fails++; } } while (0)
int main() {
    const long long f = 16000000;
    long long t = 1000000000;
    bool echo = false;
    {   // a press seen by its state at one tick, its queued press at the next (the window's thread a little late): one
        KeyPress k;
        CHECK(key_press(k, true, false, true, true, t, &echo) && !echo);
        CHECK(!key_press(k, true, true, true, true, t + f, &echo) && echo);   // (on the entries: the same press)
        CHECK(!key_press(k, false, false, true, true, t + 2 * f, &echo) && !echo);
        // and A opened the Mods screen at the first tick: its queued press there (taken false) is the same press too
        CHECK(key_press(k, true, false, true, true, t + 3 * f, &echo));
        CHECK(!key_press(k, true, true, false, true, t + 4 * f, &echo) && echo);
    }
    {   // a tap between two ticks: its state never seen, its queued press counts
        KeyPress k;
        CHECK(key_press(k, false, true, true, true, t, &echo) && !echo);
        CHECK(!key_press(k, false, false, true, true, t + f, &echo));
    }
    {   // both at one tick: one press, no queued press still to come
        KeyPress k;
        CHECK(key_press(k, true, true, true, true, t, &echo));
        CHECK(!key_press(k, false, false, true, true, t + f, &echo));
        CHECK(key_press(k, false, true, true, true, t + 2 * f, &echo) && !echo);   // (the next tap)
    }
    {   // held from before (the key that brought the cursor onto Mods): its late queued press is no press
        KeyPress k;
        key_press(k, true, false, false, true, t, &echo);   // (down before the entries had the cursor)
        CHECK(!key_press(k, true, true, true, true, t + f, &echo));
    }
    {   // off the entries (taken false): no echo kept, the menu's own use of the key's queued press stays
        KeyPress k;
        CHECK(key_press(k, true, false, false, true, t, &echo));
        CHECK(!key_press(k, true, true, false, true, t + f, &echo) && !echo);
    }
    {   // a queued press long after one counted on the state is a press of its own
        KeyPress k;
        CHECK(key_press(k, true, false, true, true, t, &echo));
        CHECK(!key_press(k, false, false, true, true, t + f, &echo));
        CHECK(key_press(k, false, true, true, true, t + kEchoNs + f, &echo) && !echo);
    }
    if (!fails) std::puts("a key's press, by its state and the host's queue");
    return fails ? 1 : 0;
}
"""


def fill(template, **parts):
    """(the template's %(name)s places filled: its printf formats are left as they are)"""
    for k, v in parts.items():
        template = template.replace("%(" + k + ")s", v)
    return template


def run(cxx, source, name, tmp, lang, extra=()):
    src = Path(tmp) / (name + (".c" if lang == "c" else ".cpp"))
    exe = Path(tmp) / (name + ".exe")
    src.write_text(source, encoding="utf-8")
    std = ["-x", "c", "-std=c11"] if lang == "c" else ["-std=c++20"]
    build = subprocess.run([cxx] + std + ["-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I" + str(VK)] +
                           list(extra) + [str(src), "-o", str(exe)], capture_output=True, text=True)
    check(build.returncode == 0, name + " does not build:\n" + build.stdout + build.stderr)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    check(r.returncode == 0, name + ":\n" + r.stdout + r.stderr)
    print(r.stdout.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    oot = (ROOT / "host" / "oot_menu.c").read_text(encoding="utf-8")
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")

    func = block(hid, "static void mods_input(uint64_t *buttons_io, int32_t stick[4]) {")
    check("mods_input(&buttons, stick);" in block(hid, "static void npad_states(Ctx *c) {"), "npad_states does not take the Mods entry's input")
    enum = block(hid, "enum { B_A = 0")
    win = oot   # (the Windows half's come first: the other half's hos_oot_mods_down is a one-line stub after them)
    state = "\n".join(re.search(r"^static volatile LONG64 " + n + r";[^\n]*$", win, re.M).group(0)
                      for n in ("mods_last_ns", "mods_last_present"))
    down = block(win, "int hos_oot_mods_down(void) {")
    check("InterlockedExchange64(&mods_last_present, InterlockedCompareExchange64(&hos_present_count, 0, 0));" in
          block(win, "static void mods_list_tick(uint64_t screen, int title, ModsCall *call) {"),
          "the list's frame does not note the game's presents")
    age = block(menu, "int64_t mods_list_age() {")
    fresh = re.search(r"^bool mods_list_fresh\(\) \{[^\n]*\}$", menu, re.M)
    check(fresh is not None, "mods_list_fresh is not the list's age against 150 ms")
    game_frames = re.search(r"^std::atomic<int64_t> game_frames\{0\};", menu, re.M)
    check(game_frames is not None, "game_frames is not where mods_list_age reads it")
    with tempfile.TemporaryDirectory(prefix="mods-entry-input-") as tmp:
        run(args.cxx, fill(HID_FIXTURE, enum=enum, func=func), "hid", tmp, "c")
        run(args.cxx, fill(MENU_HOST_FIXTURE, state=state, func=down), "oot", tmp, "c")
        run(args.cxx, fill(AGE_FIXTURE, state=game_frames.group(0), age=age, fresh=fresh.group(0)), "age", tmp, "c++")
        run(args.cxx, KEYS_FIXTURE, "keys", tmp, "c++")

    # the menu: the host's report of the directions held (each read), repeated as a pad's held direction
    opt = block(menu, "void menu_option_set(int id, int value) {")
    check("mods_press.fetch_or(value & 0xff, std::memory_order_relaxed);" in opt and
          "mods_held_dirs.store(value >> HOS_MODS_HELD_SHIFT & HOS_MODS_PRESS_DIRS, std::memory_order_relaxed);" in opt,
          "the menu does not keep the directions the host tells as held")
    tick = block(menu, "void menu_tick() {")
    rep = tick[tick.index("// holding an arrow or the D-pad repeats it"):tick.index("if (open_now.load() && listening != 1) {")]
    check("const bool mods_here = page == kModsRowPage || page == kModsPage;" in rep and "(host_held >> (i - 1) & 1)" in rep and
          "t - mods_held_ns.load(std::memory_order_relaxed) < 250000000LL" in rep,
          "a direction a test's script holds on Mods is not repeated as a pad's")
    check("next_repeat[i] = t + 350000000LL" in rep and "33000000LL : 83000000LL" in rep, "the repeat's rate changed")
    # (the keys bound to the game's D-pad, movement and A on the entries: pressed and held as the menu's own, the Down that
    # came onto Mods taken away with the pad's)
    keys = tick[tick.index("uint32_t game_keys = 0, game_keys_new = 0;"):tick.index("// holding an arrow or the D-pad repeats it")]
    check("{HOS_SW_UP, HOS_KEY_MOVE_UP}, {HOS_SW_DOWN, HOS_KEY_MOVE_DOWN}" in keys and "{HOS_SW_A, -1}" in keys and
          "const bool row = open_now.load() && page == kModsRowPage && listening == 0;" in keys and
          "if (went) game_keys_new |= vk_places[m];" in keys,
          "the keys bound to the game's D-pad, movement and A do not act on the Mods entries")
    # ((a review of 63a5020) one press from one place: the key's state and its queued press as one, the menu's own use of
    # the same key (E, its next tab) not taken there too, and its queued press after A opened the Mods screen not again)
    check("const bool q = own >= 0 ? key_queued[own] : queue && host->take_key_press(vk) && mine;" in keys and
          "menu_keys::key_press(gk_key[vk], down, q, row, queue, t, &echo);" in keys and
          "if (echo && own >= 0) press[own] = false;" in keys and
          re.search(r"if \(row\) \{[^}]*if \(own >= 0\) press\[own\] = false;", keys) is not None,
          "a game key on the entries is still read two ways (its state for A, its queued press as the menu's next tab)")
    check("key_queued[i] = mine && queued;" in tick and "g < gk_n ? gk_q[g] : host->take_key_press(vk) && mine;" in tick,
          "the queued presses the game keys read are not the ones the menu and the L, R and B keys read")
    check(tick.index("uint32_t game_keys = 0, game_keys_new = 0;") < tick.index("bool tab_right = press[11] || press[12]"),
          "the game keys are read after the tabs are taken from the menu's keys")
    # ((a review of beb9b9e) the held Down's repeat onto Mods, word 5: the menu repeats on from it at the step)
    check("entered_by_down = enter == 1 || enter == 5;" in tick and "mods_down_repeat_ns = enter == 5 ? prof_now() : 0;" in tick and
          "held_since[i] = t - 350000000LL, next_repeat[i] = t + 83000000LL, mods_down_repeat_ns = 0;" in rep,
          "the menu does not go on repeating a held Down that came onto Mods by repeat")
    check("held[i] || (game_keys >> i & 1) ||" in rep, "a game key held on Mods does not repeat")
    check(tick.index("press[i] |= (game_keys_new >> i & 1) != 0;") < tick.index("if (entered_by_down) press[2] = false;"),
          "the key's Down that came onto Mods is taken again as the entry's own")
    check('logf("menu: the host\'s word %d for the Mods entry not taken: %s", enter,' in tick,
          "an enter word the menu does not take leaves no line")
    check("title_anim.step(now, sel, std::min<int64_t>(age, mods_entry::kListShownNs));" in
          block(menu, "void draw_title_mods(ui::Ctx& c, bool sel, float cx, float cy, float k, float pitch) {"),
          "the title's entry does not take the list's age by the game's frames (a slow frame blinks it)")
    print("PASS: the Mods entry's input: no Down taken twice, held ones repeated, slow frames keep the list")


if __name__ == "__main__":
    main()
