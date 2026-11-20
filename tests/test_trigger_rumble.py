"""Trigger vibration (Controls): the game's rumble in the motors of an Xbox One or Series pad's triggers too, off by default.

NX2navi's rumble_triggers, here the Insert menu's row (option 51). host/hos_sdl.c loads SDL's trigger rumble call by name
as it loads the others (SDL_RumbleGamepadTriggers in SDL3, SDL_GameControllerRumbleTriggers in SDL2 2.0.14 on) and does
nothing where the loaded SDL has none; host/pad_sdl.c's trigger_poll sends both triggers the light motor's rumble on the
motors' own schedule (rumble_due). Both are compiled from the source and run against stand-ins: SDL3's bool and SDL2's 0
for success, a missing call, nothing ever sent while the row is off, a held rumble sent again before SDL's second ends, a
stop when the row is turned off, and a pad that refuses asked again two seconds later at the soonest and named once in
the log. Then the poll, the option, the row and the readme are read.

    python -I -B tests/test_trigger_rumble.py [--cc gcc]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8")


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(text, marker):
    """text from marker to the end of the first {...} after it"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def run(cc, src, name, tmp, flags):
    path = Path(tmp) / name
    path.write_text(src, encoding="utf-8")
    exe = Path(tmp) / (name + ".exe")
    subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", *flags, "-o", str(exe), str(path)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    check(r.returncode == 0, name + ": " + r.stderr.strip())


# host/hos_sdl.c's two functions over a stand-in for its table of SDL's calls
SDL = r'''#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static int version;
static struct { void *PadRumbleTriggers; } f;
#define CALL(type, fn) ((type)f.fn)
/* HAS */
/* SEND */
static int calls, answer;
static void *got_pad;
static unsigned got_left, got_right, got_ms;
static _Bool sdl3(void *pad, uint16_t left, uint16_t right, uint32_t ms) {
    calls++, got_pad = pad, got_left = left, got_right = right, got_ms = ms;
    return answer != 0;
}
static int sdl2(void *pad, uint16_t left, uint16_t right, uint32_t ms) {
    calls++, got_pad = pad, got_left = left, got_right = right, got_ms = ms;
    return answer;
}
int main(void) {
    static int pad;
    /* an SDL without the call (SDL2 before 2.0.14): nothing called, nothing taken */
    version = 2;
    CHECK(!hos_sdl_pad_has_trigger_rumble() && hos_sdl_pad_rumble_triggers(&pad, 1000, 2000, 500) == 0 && calls == 0);
    /* SDL3: true is taken; the pad, both strengths and the time as given */
    version = 3, f.PadRumbleTriggers = (void *)sdl3, answer = 1;
    CHECK(hos_sdl_pad_has_trigger_rumble());
    CHECK(hos_sdl_pad_rumble_triggers(&pad, 1000, 65535, 500) == 1 && calls == 1);
    CHECK(got_pad == &pad && got_left == 1000 && got_right == 65535 && got_ms == 500);
    answer = 0;
    CHECK(hos_sdl_pad_rumble_triggers(&pad, 1, 2, 3) == 0 && calls == 2);
    /* SDL2: 0 is taken, -1 (a pad without trigger motors) is not */
    version = 2, f.PadRumbleTriggers = (void *)sdl2, answer = 0;
    CHECK(hos_sdl_pad_rumble_triggers(&pad, 7, 8, 1000) == 1 && calls == 3 && got_left == 7 && got_right == 8 && got_ms == 1000);
    answer = -1;
    CHECK(hos_sdl_pad_rumble_triggers(&pad, 7, 8, 1000) == 0 && calls == 4);
    printf("sdl: the trigger call's answer read as SDL3's bool and as SDL2's 0; nothing without the call\n");
    return 0;
}
'''

# host/pad_sdl.c's trigger_poll and rumble_due over its own table of pads and a stand-in SDL
POLL = r'''#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hd_rumble.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
#define MAX_PADS 8
/* PADS */
static int fake_ok = 1, fake_has = 1, sent_n, logged;
static unsigned sent_left, sent_right, sent_ms;
static char last_log[256];
static int hos_sdl_pad_has_trigger_rumble(void) { return fake_has; }
static int hos_sdl_pad_rumble_triggers(void *pad, uint16_t left, uint16_t right, uint32_t ms) {
    CHECK(pad != NULL);
    sent_n++, sent_left = left, sent_right = right, sent_ms = ms;
    return fake_has && fake_ok;
}
static const char *hos_sdl_pad_name(void *pad) { (void)pad; return "pad"; }
static const char *hos_sdl_error(void) { return "That operation is not supported"; }
static void hos_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
    logged++;
}
/* DUE */
/* TRIGGER */
#define MS 1000000LL
int main(void) {
    static int dummy;
    const long long t = 5000 * MS;
    long long now = t;
    for (int i = 0; i < 4; i++) pads[i].gc = &dummy;

    /* the row off (want 0) on a pad that never rumbled there: nothing is ever called, whatever the game plays */
    for (int k = 0; k < 2000; k++) trigger_poll(0, 0, now += 4 * MS);
    CHECK(sent_n == 0 && logged == 0);

    /* on: both triggers the strength asked for, for SDL's second; the same strength is not sent again until half of it
       has gone, then it is (a held rumble does not lapse); a change goes at once */
    now = t;
    trigger_poll(0, 30000, now);
    CHECK(sent_n == 1 && sent_left == 30000 && sent_right == 30000 && sent_ms == 1000);
    CHECK(logged == 1 && strstr(last_log, "trigger vibration on \"pad\""));
    for (now = t + 4 * MS; now < t + 500 * MS; now += 4 * MS) trigger_poll(0, 30000, now);
    CHECK(sent_n == 1);
    trigger_poll(0, 30000, t + 500 * MS);
    CHECK(sent_n == 2 && sent_left == 30000 && sent_ms == 1000);
    trigger_poll(0, 12345, t + 504 * MS);
    CHECK(sent_n == 3 && sent_left == 12345 && sent_right == 12345);
    {   /* held for ten seconds: sent about twice a second, never left for the whole second to run out */
        const int before = sent_n;
        for (now = t + 508 * MS; now < t + 10508 * MS; now += 4 * MS) trigger_poll(0, 12345, now);
        CHECK(sent_n - before >= 19 && sent_n - before <= 20);
    }
    /* the row turned off, or the rumble over, while they play: one stop (no time), then nothing */
    trigger_poll(0, 0, now += 4 * MS);
    CHECK(sent_left == 0 && sent_right == 0 && sent_ms == 0);
    {
        const int before = sent_n;
        for (int k = 0; k < 1000; k++) trigger_poll(0, 0, now += 4 * MS);
        CHECK(sent_n == before);
    }
    CHECK(logged == 1);   /* (said once that it plays, not at every rumble) */

    /* a pad without trigger motors (a DualSense, a pad through XInput): SDL refuses. Said once; asked again only two
       seconds later, however the rumble changes meanwhile; never a stop for what never played */
    sent_n = 0, logged = 0, fake_ok = 0;
    now = t;
    trigger_poll(1, 40000, now);
    CHECK(sent_n == 1 && logged == 1 && strstr(last_log, "\"pad\" took no trigger vibration (That operation is not supported)"));
    for (int k = 1; k < 500; k++) trigger_poll(1, 40000 + (unsigned)k, now + k * 4 * MS);   /* (the game's rumble, changing) */
    CHECK(sent_n == 1);
    trigger_poll(1, 0, now + 1999 * MS);
    CHECK(sent_n == 1);
    trigger_poll(1, 41000, now + 2000 * MS);
    CHECK(sent_n == 2 && logged == 1);
    for (now = t + 2004 * MS; now < t + 20000 * MS; now += 4 * MS) trigger_poll(1, 41000, now);
    CHECK(sent_n >= 9 && sent_n <= 10 && logged == 1);   /* (18 more seconds of rumble: a call every two) */
    for (int k = 0; k < 1000; k++) trigger_poll(1, 0, now += 4 * MS);
    CHECK(sent_n <= 10);
    /* a write that failed once comes back: the pad plays from the next try, and the log says so */
    fake_ok = 1;
    trigger_poll(1, 41000, now += 4000 * MS);
    CHECK(sent_left == 41000 && logged == 2 && strstr(last_log, "trigger vibration on"));
    {   /* ... and one that fails while they play: their rumble is taken as over, and asked for again after the two seconds */
        const int before = sent_n;
        fake_ok = 0;
        trigger_poll(1, 42000, now += 4 * MS);
        CHECK(sent_n == before + 1 && pads[1].trigger == 0);
        trigger_poll(1, 42000, now + 1000 * MS);
        CHECK(sent_n == before + 1);
        fake_ok = 1;
        trigger_poll(1, 42000, now + 2000 * MS);
        CHECK(sent_n == before + 2 && pads[1].trigger == 42000);
    }

    /* an SDL without the call: the same refusal, and the log says what is missing */
    sent_n = 0, logged = 0, fake_has = 0;
    trigger_poll(2, 20000, t);
    CHECK(sent_n == 1 && logged == 1 && strstr(last_log, "this SDL has no call for it"));
    trigger_poll(2, 20000, t + 100 * MS);
    CHECK(sent_n == 1 && logged == 1);
    printf("triggers: nothing while off; the light motor's rumble on the motors' schedule; a refusal asked again after 2 s\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    sdl, sdl_h, pad = read("host", "hos_sdl.c"), read("host", "hos_sdl.h"), read("host", "pad_sdl.c")
    api, replay = read("gpu", "vk", "vk_api.h"), read("gpu", "vk", "vk_replay.cpp")
    menu, header = read("gpu", "vk", "vk_menu.cpp"), read("gpu", "vk", "vk_menu.h")

    # ---- hos_sdl.c: loaded by name like the other optional calls, never required ----
    load = block(sdl, "static int get_all(")
    check('*(FARPROC *)&f.PadRumbleTriggers = GetProcAddress(m, three ? "SDL_RumbleGamepadTriggers" : '
          '"SDL_GameControllerRumbleTriggers");' in load, "hos_sdl.c does not load SDL's trigger rumble call by its two names")
    check(not re.search(r"\bGET2?\(PadRumbleTriggers\b", load),
          "the trigger rumble call is required: an SDL without it (SDL2 before 2.0.14) would not load at all")
    for decl in ("int hos_sdl_pad_has_trigger_rumble(void);",
                 "int hos_sdl_pad_rumble_triggers(void *pad, uint16_t left, uint16_t right, uint32_t ms);"):
        check(decl in sdl_h, "hos_sdl.h does not declare " + decl)

    # ---- the poll: the row read each poll, the light motor's strength, after the motors' own send; HD pads left out ----
    poll = re.sub(r"\s+", " ", block(pad, "static void pads_poll"))
    check("const int triggers = rx_option(HOS_OPT_RUMBLE_TRIGGERS, 0) != 0;" in poll,
          "the poll does not read the Trigger vibration row (off without the menu)")
    check("const uint16_t lo = motors[0], hi = motors[1];" in poll, "hi is no longer the light motor's strength")
    call = "trigger_poll(i, triggers ? hi : 0, now);"
    check(poll.count("trigger_poll(") == 1 and call in poll,
          "the triggers do not get the light motor's rumble (after the curve and the strength), or get it with the row off")
    check(poll.index("if (hd_poll(i, side, now)) continue;") < poll.index("hos_sdl_pad_rumble(gc, lo, hi") < poll.index(call),
          "the triggers' rumble is not after the motors', or reaches a pad that plays HD rumble")
    opened = poll[poll.index("void *gc = hos_sdl_pad_open(opens[j]);"):poll.index("hd_open(slot, gc);")]
    check("pads[slot].trigger = 0, pads[slot].trigger_said = 0;" in opened
          and "pads[slot].trigger_ns = pads[slot].trigger_retry_ns = 0;" in opened,
          "a pad opened into a slot keeps the triggers' state of the pad that was there")

    # ---- the option: 51, off unless the row is on ----
    ids = re.search(r"enum \{ HOS_OPT_RUMBLE_CURVE = 50, HOS_OPT_RUMBLE_TRIGGERS\b", api)
    check(ids is not None, "HOS_OPT_RUMBLE_TRIGGERS is not option 51")
    check("if (id == HOS_OPT_RUMBLE_TRIGGERS) return opts.rumble_triggers != 0;" in block(replay, "t.option = [](int id) -> int {"),
          "the host cannot see the menu's Trigger vibration (option 51)")
    check("int rumble_triggers = 0;" in header, "Trigger vibration is not off by default")
    row = re.search(r'\{TAB_CONTROLS, "Trigger vibration", &opts\.rumble_triggers, 0, 1, 1, tx_onoff,\s*"[^}]*\},', menu)
    check(row is not None, "the Controls tab has no Trigger vibration row")
    check(row.group(0).rstrip().endswith("nullptr, false, true},"),
          "the row is not preset_exempt: turning it on would set the quality preset to Custom")
    check(re.search(r"^\| Trigger vibration \| Off, On", read("readme.md"), re.M) is not None,
          "readme.md's settings table has no Trigger vibration row")

    pads = re.search(r"static struct \{.*?\} pads\[MAX_PADS\];", pad, re.S)
    check(pads is not None, "no pads table in pad_sdl.c")
    with tempfile.TemporaryDirectory(prefix="trigger-rumble-") as tmp:
        run(a.cc, SDL.replace("/* HAS */", block(sdl, "int hos_sdl_pad_has_trigger_rumble(")).replace(
            "/* SEND */", block(sdl, "int hos_sdl_pad_rumble_triggers(")), "sdl.c", tmp, [])
        run(a.cc, POLL.replace("/* PADS */", pads.group(0)).replace("/* DUE */", block(pad, "static int rumble_due(")).replace(
            "/* TRIGGER */", block(pad, "static void trigger_poll(")), "poll.c", tmp, ["-I", str(ROOT / "host")])
    print("ok")


if __name__ == "__main__":
    main()
