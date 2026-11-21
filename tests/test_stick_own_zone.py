"""A dead zone of each stick's own: the Insert menu's "Left stick dead zone" and "Right stick dead zone" rows (Controls).

NX2navi's lstick_deadzone and rstick_deadzone: a worn stick can have a larger dead zone than the other, and a good one a
smaller. Each row is "As Stick dead zone" by default (-1: that row decides for the stick, as it did for both before), then
0% to 50%. host/nn_hid.c's hos_stick_axis_own cuts an axis at its stick's own zone, per axis like the Stick dead zone row
and at the same places (its 5% is that row's 5%), and both pad paths go through it: SDL's (host/pad_sdl.c stick_value)
and XInput's (pads_read).

host/pad_sdl.c lets an axis steer only once it was seen at rest. With a zone of 0 an axis is never exactly at rest, so
for a stick with a zone of its own an axis inside the default dead zone counts as at rest too (stick_rests): a stick
steers from its first push at any zone, and a phantom axis held far out still never does. The same test keeps a resting
stick's noise from counting as a push when the poll works out which pad was used last.

The functions and the poll's two blocks are compiled from the source and run over every axis value; the option's coding,
the rows, the options file's keys and the two tables that document them are checked too.

    python -I -B tests/test_stick_own_zone.py [--cc gcc] [--cxx g++]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
PRESETS = [(5, 1), (10, 2), (15, 3), (20, 4), (30, 5)]   # (a stick's own percent, the Stick dead zone row's value for it)


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


def run(compiler, src, name, tmp, flags):
    path = Path(tmp) / name
    path.write_text(src, encoding="utf-8")
    exe = Path(tmp) / (name + ".exe")
    subprocess.run([compiler, *flags, "-Wall", "-Werror", "-o", str(exe), str(path)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    check(r.returncode == 0, name + ": " + r.stderr.strip())


C = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
/* OPTIONS_ENUM */;
static int option_value[2], option_asked[2];
static int rx_option(int id, int otherwise) {   /* (the renderer's answer for the two rows; any other id is not theirs) */
    CHECK(id == HOS_OPT_LSTICK_ZONE || id == HOS_OPT_RSTICK_ZONE);
    CHECK(otherwise == 0);   /* (without the renderer: none of its own) */
    option_asked[id - HOS_OPT_LSTICK_ZONE]++;
    return option_value[id - HOS_OPT_LSTICK_ZONE];
}
/* AXIS */
/* OWN */
/* AXIS_OWN */
/* VALUE */
/* RESTS */
static struct { unsigned rested; signed char held[4]; } pads[1];
/* the poll's own lines: the four axes cut, then the rule that an axis steers once it was seen at rest, then what the
   poll takes as a push for "the pad used last". w: the axes as SDL gives them in, what steers out */
static uint64_t step(int i, int w[4], int z, const int own[2]) {
    const int16_t raw[4] = {(int16_t)w[0], (int16_t)w[1], (int16_t)w[2], (int16_t)w[3]};
    int32_t v[4];
    uint64_t sig = 0;
    for (int k = 0; k < 4; k++) v[k] = stick_value(raw[k], 0, z, own[k >> 1]);
/* REST_RULE */
/* USED */
    for (int k = 0; k < 4; k++) w[k] = v[k];
    return sig;
}
static int32_t cut_at(int32_t i, int32_t t) {   /* (the documented cut, worked out again) */
    const int32_t a = i < -32767 ? -32767 : i;
    return a > t || a < -t ? a : 0;
}
int main(void) {
    static const int preset[5][2] = {/* PRESETS */};
    /* ---- the cut ---- */
    for (int32_t i = -32768; i <= 32767; i++) {
        const int16_t v = (int16_t)i;
        /* none of its own: the Stick dead zone row's, every preset, as hos_stick_axis alone */
        for (int z = -1; z < 7; z++)
            CHECK(hos_stick_axis_own(v, z, -1) == hos_stick_axis(v, z) && hos_stick_axis_own(v, z, -9) == hos_stick_axis(v, z));
        /* its own: the percent of the axis's travel, whatever the Stick dead zone row says; 0 cuts nothing */
        for (int p = 0; p <= 50; p++) {
            const int32_t want = cut_at(i, (p * 32767 + 50) / 100);
            CHECK(hos_stick_axis_own(v, 0, p) == want && hos_stick_axis_own(v, 5, p) == want);
            CHECK(stick_value(v, 0, 3, p) == want && stick_value(v, 1, 3, p) == -want);
        }
        CHECK(hos_stick_axis_own(v, 0, 0) == (i < -32767 ? -32767 : i));
        CHECK(hos_stick_axis_own(v, 0, 51) == hos_stick_axis_own(v, 0, 50) && hos_stick_axis_own(v, 0, 999) == hos_stick_axis_own(v, 0, 50));
        /* the same places as the Stick dead zone row: a stick's own 5% is that row's 5% */
        for (int k = 0; k < 5; k++) CHECK(hos_stick_axis_own(v, 0, preset[k][0]) == hos_stick_axis(v, preset[k][1]));
        /* at rest, for the rule: never by this test without a zone of its own; with one, inside the default dead zone */
        CHECK(!stick_rests(v, -1));
        for (int p = 0; p <= 50; p += 5) CHECK(stick_rests(v, p) == (i >= -7849 && i <= 7849));
    }
    CHECK(hos_stick_axis_own(16383, 0, 50) == 0 && hos_stick_axis_own(16384, 0, 50) == 0 && hos_stick_axis_own(16385, 0, 50) == 16385);
    CHECK(hos_stick_axis_own(1, 0, 0) == 1 && hos_stick_axis_own(-1, 0, 0) == -1 && hos_stick_axis_own(0, 0, 0) == 0);

    /* ---- the option: one more than the row's value, so 0 (also an option table that does not know the rows) is none ---- */
    static const int coded[][2] = {{0, -1}, {1, 0}, {2, 1}, {9, 8}, {51, 50}, {52, 50}, {999, 50}, {-1, -1}, {-40, -1}};
    for (unsigned k = 0; k < sizeof coded / sizeof *coded; k++) {
        option_value[0] = coded[k][0], option_value[1] = 0;
        CHECK(hos_stick_own(0) == coded[k][1] && hos_stick_own(1) == -1);
        option_value[0] = 0, option_value[1] = coded[k][0];
        CHECK(hos_stick_own(0) == -1 && hos_stick_own(1) == coded[k][1]);
    }
    CHECK(option_asked[0] && option_asked[0] == option_asked[1]);   /* (the left stick's row, the right stick's row) */

    /* ---- the rule that an axis steers once it was seen at rest, with a zone of a stick's own ---- */
    {   /* no dead zone at all (0%): a stick is never exactly at rest. One resting at +300 steers from its first push,
           either way, and its rest is not cut */
        const int own[2] = {0, 0};
        memset(pads, 0, sizeof pads);
        { int w[4] = {300, -120, 0, 0}; step(0, w, 0, own); CHECK(w[0] == 300 && w[1] == -120); }
        { int w[4] = {20000, 300, 0, 0}; step(0, w, 0, own); CHECK(w[0] == 20000 && w[1] == 300); }
        /* ... and one that was already held when the pad was opened, once it is let go */
        memset(pads, 0, sizeof pads);
        for (int n = 0; n < 10; n++) { int w[4] = {25000, 0, 0, 0}; step(0, w, 0, own); CHECK(w[0] == 0); }
        { int w[4] = {4000, 0, 0, 0}; step(0, w, 0, own); CHECK(w[0] == 4000); }
        { int w[4] = {25000, 0, 0, 0}; step(0, w, 0, own); CHECK(w[0] == 25000); }
        /* a phantom axis held far out (a trigger mapped onto a stick) still never steers; the other axes are free */
        memset(pads, 0, sizeof pads);
        for (int n = 0; n < 100; n++) {
            int w[4] = {150, 0, -32768, 32767};
            step(0, w, 0, own);
            CHECK(w[0] == 150 && w[2] == 0 && w[3] == 0);
        }
        /* nor does one held just outside the default dead zone, where no stick rests */
        memset(pads, 0, sizeof pads);
        for (int n = 0; n < 100; n++) { int w[4] = {7850, 0, 0, 0}; step(0, w, 0, own); CHECK(w[0] == 0); }
    }
    {   /* a small zone (5%) and a worn stick resting just past it, at 6%: it steers at once (and drifts, as 5% asks);
           with the Stick dead zone row's own 5% and no zone of the stick's own it waits for a push the other way, as before */
        const int own[2] = {5, 5}, none[2] = {-1, -1};
        memset(pads, 0, sizeof pads);
        { int w[4] = {2000, 0, 0, 0}; step(0, w, 0, own); CHECK(w[0] == 2000); }
        memset(pads, 0, sizeof pads);
        for (int n = 0; n < 10; n++) { int w[4] = {2000, 0, 0, 0}; step(0, w, 1, none); CHECK(w[0] == 0); }
        { int w[4] = {-9000, 0, 0, 0}; step(0, w, 1, none); CHECK(w[0] == -9000); }
        { int w[4] = {2000, 0, 0, 0}; step(0, w, 1, none); CHECK(w[0] == 2000); }
    }
    {   /* a zone wider than the default (40%): rest is the whole of it, and the cut is 40% */
        const int own[2] = {40, 40};
        memset(pads, 0, sizeof pads);
        for (int n = 0; n < 3; n++) { int w[4] = {30000, 0, 0, 0}; step(0, w, 0, own); CHECK(w[0] == 0); }   /* (held from the start) */
        { int w[4] = {12000, 0, 0, 0}; step(0, w, 0, own); CHECK(w[0] == 0); }   /* (inside 40%: at rest, and cut) */
        { int w[4] = {30000, 0, 0, 0}; step(0, w, 0, own); CHECK(w[0] == 30000); }
    }
    {   /* each stick its own: the left at 0%, the right with none (the row's default 24%) */
        const int own[2] = {0, -1};
        memset(pads, 0, sizeof pads);
        { int w[4] = {500, -500, 500, -500}; step(0, w, 0, own); CHECK(w[0] == 500 && w[1] == -500 && w[2] == 0 && w[3] == 0); }
        { int w[4] = {500, -500, 9000, -9000}; step(0, w, 0, own); CHECK(w[2] == 9000 && w[3] == -9000); }
    }

    /* ---- the pad used last: a resting stick's noise past a small zone of its own is no push ---- */
    {
        const int own[2] = {0, 0}, none[2] = {-1, -1};
        memset(pads, 0, sizeof pads);
        uint64_t first;
        { int w[4] = {300, -200, 150, -90}; first = step(0, w, 0, own); }
        for (int n = 0; n < 50; n++) {   /* (noise round the centre, changing sign: the same "nothing held") */
            int w[4] = {n & 1 ? 280 : -310, n & 2 ? 90 : -40, n % 3 ? 7000 : -7000, 0};
            CHECK(step(0, w, 0, own) == first);
        }
        CHECK(first == 0);
        { int w[4] = {20000, 0, 0, 0}; CHECK(step(0, w, 0, own) != 0); }                /* a push is one */
        { int w[4] = {0, 0, 0, -20000}; CHECK(step(0, w, 0, own) != 0); }
        memset(pads, 0, sizeof pads);   /* (and without a zone of its own, as before: what steers is a push) */
        { int w[4] = {300, 0, 0, 0}; CHECK(step(0, w, 0, none) == 0); }
        { int w[4] = {9000, 0, 0, 0}; CHECK(step(0, w, 0, none) == (1ull << 32)); }
        { int w[4] = {-9000, 0, 0, 0}; CHECK(step(0, w, 0, none) == (2ull << 32)); }
    }
    printf("sticks: a zone of its own cuts at its percent, none is the row's; rest is the default zone or wider\n");
    return 0;
}
'''

CXX = r'''#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
/* OPTIONS */
Options opts;
/* TABS */
/* ROW */
/* TX_PERCENT */
/* TX_OWN */
Row rows[] = {/* THE_ROWS */};
/* STOPS */
/* INI_KEY */
static int output_choice(int v) { return v; }
static void parse(const char* text) {
    char line[80];
    snprintf(line, sizeof line, "%s", text);
    char* eq = strchr(line, '=');
    *eq = 0;
    /* LOAD_LOOP */
}
int main() {
    Options fresh;
    CHECK(fresh.lstick_dead_zone == -1 && fresh.rstick_dead_zone == -1 && fresh.stick_dead_zone == 0);   // (out of the box: as before)
    CHECK(rows[0].value == &opts.lstick_dead_zone && rows[1].value == &opts.rstick_dead_zone);
    CHECK(!strcmp(rows[0].name, "Left stick dead zone") && !strcmp(rows[1].name, "Right stick dead zone"));
    for (const Row& r : rows) {
        CHECK(r.lo == -1 && r.hi == 50 && r.step == 1);
        CHECK(r.preset_exempt && !r.restart);   // (a control: the quality preset stays, and it applies at once)
        CHECK(stops_at_ends(r));                // (a scale: 50% does not wrap round to As Stick dead zone)
        CHECK(!strcmp(r.text(-1), "Default (24%)") && !strcmp(r.text(0), "0%") && !strcmp(r.text(8), "8%") && !strcmp(r.text(50), "50%"));
    }
    CHECK(!strcmp(ini_key(rows[0]), "left_stick_dead_zone"));
    CHECK(!strcmp(ini_key(rows[1]), "right_stick_dead_zone"));
    parse("left_stick_dead_zone=8\n");
    CHECK(opts.lstick_dead_zone == 8 && opts.rstick_dead_zone == -1);   // (one stick alone)
    parse("right_stick_dead_zone=0\n");
    CHECK(opts.lstick_dead_zone == 8 && opts.rstick_dead_zone == 0);
    parse("left_stick_dead_zone=-1\n"), parse("right_stick_dead_zone=77\n");
    CHECK(opts.lstick_dead_zone == -1 && opts.rstick_dead_zone == 50);
    parse("right_stick_dead_zone=-30\n");
    CHECK(opts.rstick_dead_zone == -1);
    parse("lstick_deadzone=20\n");   // (NX2navi's key is not this file's)
    CHECK(opts.lstick_dead_zone == -1);
    printf("menu: Left and Right stick dead zone, Default then 0%% to 50%%, saved by their names\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    hid, pad, api = read("host", "nn_hid.c"), read("host", "pad_sdl.c"), read("gpu", "vk", "vk_api.h")
    replay, menu, header = read("gpu", "vk", "vk_replay.cpp"), read("gpu", "vk", "vk_menu.cpp"), read("gpu", "vk", "vk_menu.h")

    # ---- both pad paths cut each stick at its own zone ----
    xinput = block(hid, "static void pads_read(")
    check("const int own[2] = {hos_stick_own(0), hos_stick_own(1)};" in xinput
          and "v[i] = hos_stick_axis_own(thumb[i], zone, own[i >> 1]);" in xinput,
          "XInput's sticks are not cut at their own dead zones (thumb 0 and 1 the left stick's, 2 and 3 the right's)")
    poll = re.sub(r"\s+", " ", block(pad, "static void pads_poll"))
    check("const int own[2] = {hos_stick_own(0), hos_stick_own(1)};" in poll, "SDL's sticks do not read their own dead zones")
    check("int32_t v[4] = {stick_value(raw[0], 0, z, own[0]), stick_value(raw[1], 1, z, own[0]), "
          "stick_value(raw[2], inv_x, z, own[1]), stick_value(raw[3], inv_y ? 0 : 1, z, own[1])};" in poll,
          "SDL's four axes are not cut at their stick's own dead zone (the left stick's two, then the right's)")
    check("const int16_t raw[4] = {hos_sdl_pad_axis(gc, AX_LX), hos_sdl_pad_axis(gc, AX_LY), hos_sdl_pad_axis(gc, AX_RX), "
          "hos_sdl_pad_axis(gc, AX_RY)};" in poll, "raw is not the four axes as SDL gives them, in v's order")
    src = pad
    start = src.index("            /* (a worn stick resting just past a small dead zone")
    end = src.index("}", src.index("if (!(pads[i].rested >> k & 1)) v[k] = 0;", start)) + 1
    rule = src[start:end]
    check("stick_rests(raw[k], own[k >> 1])" in rule, "the rest rule does not ask stick_rests for a stick with a zone of its own")
    used = block(src, "                for (int k = 0; k < 4; k++) {   /* ((Triforce) a resting stick's noise")
    check("stick_rests(raw[k], own[k >> 1]) ? 0 : v[k]" in used and "sig |=" in used,
          "the pad used last counts a resting stick's noise as a push")

    # ---- the options: 52 and 53, one more than the rows' values ----
    ids = re.search(r"enum \{ HOS_OPT_RUMBLE_CURVE = 50, HOS_OPT_RUMBLE_TRIGGERS, HOS_OPT_LSTICK_ZONE, HOS_OPT_RSTICK_ZONE\b[^}]*\}", api)
    check(ids is not None, "HOS_OPT_LSTICK_ZONE and HOS_OPT_RSTICK_ZONE are not options 52 and 53")
    option = block(replay, "t.option = [](int id) -> int {")
    for stick in ("l", "r"):
        check("if (id == HOS_OPT_%sSTICK_ZONE) return std::clamp(opts.%sstick_dead_zone, -1, 50) + 1;" % (stick.upper(), stick) in option,
              "the host cannot see the %s stick's own dead zone, or not as one more than the row's value" % ("left" if stick == "l" else "right"))

    # ---- the rows and the options file ----
    rows = [re.search(r'\{TAB_CONTROLS, "%s stick dead zone", &opts\.%sstick_dead_zone, -1, 50, 1, tx_own_dead_zone,\s*"[^}]*\},'
                      % (name, name[0].lower()), menu) for name in ("Left", "Right")]
    check(all(rows), "the Controls tab has no Left and Right stick dead zone rows (-1 to 50)")
    # ((Triforce, options_version 7) the shared Stick dead zone row is gone: the two rows are the Controller group's, the left
    # first; their -1 is XInput's own, Default)
    check(menu.index('MENU_HEADING(TAB_CONTROLS, "Controller"),') < rows[0].start() < rows[1].start(),
          "the two rows are not in the Controller group, the left first")
    check('"Stick dead zone", &opts.stick_dead_zone' not in menu, "the shared Stick dead zone row is still there")
    loop = block(block(menu, "void menu_init("), "for (Row& r : rows) {")
    cxx = CXX
    for marker, text in (("/* OPTIONS */", re.search(r"struct Options \{.*?\n\};", header, re.S).group(0)),
                         ("/* TABS */", re.search(r"enum MenuTab \{.*?\};", menu, re.S).group(0)),
                         ("/* ROW */", re.search(r"struct Row \{.*?\n\};", menu, re.S).group(0)),
                         ("/* TX_PERCENT */", block(menu, "const char* tx_percent(")),
                         ("/* TX_OWN */", block(menu, "const char* tx_own_dead_zone(")),
                         ("/* THE_ROWS */", rows[0].group(0) + "\n" + rows[1].group(0)),
                         ("/* STOPS */", block(menu, "bool stops_at_ends(")),
                         ("/* INI_KEY */", "bool is_merged(const int*) { return false; }   /* ((Triforce, options_version 7) no merged row here */\n" + block(menu, "bool ini_row(") + "\n" + block(menu, "const char* ini_key(")),
                         ("/* LOAD_LOOP */", loop)):
        cxx = cxx.replace(marker, text)
    c = C
    for marker, text in (("/* OPTIONS_ENUM */", block(api, "enum { HOS_OPT_RUMBLE_CURVE")),
                         ("/* AXIS */", block(hid, "int32_t hos_stick_axis(")),
                         ("/* OWN */", block(hid, "int hos_stick_own(")),
                         ("/* AXIS_OWN */", block(hid, "int32_t hos_stick_axis_own(")),
                         ("/* VALUE */", block(pad, "static int32_t stick_value(")),
                         ("/* RESTS */", block(pad, "static int stick_rests(")),
                         ("/* REST_RULE */", rule), ("/* USED */", used),
                         ("/* PRESETS */", ", ".join("{%d, %d}" % x for x in PRESETS))):
        c = c.replace(marker, text)

    # ---- the tables that document them ----
    check(re.search(r"^\| Left stick dead zone, Right stick dead zone \| Default \(24%\), or 0% to 50%", read("readme.md"), re.M)
          is not None, "readme.md's settings table has no row for the sticks' own dead zones")
    check(re.search(r"^\| `left_stick_dead_zone`, `right_stick_dead_zone` \| -1 Default \(XInput's ~24%\), 0 to 50 percent",
                    read("docs", "development.md"), re.M) is not None,
          "docs/development.md's options table does not say what the two keys' numbers are")

    with tempfile.TemporaryDirectory(prefix="stick-own-zone-") as tmp:
        run(a.cc, c, "sticks.c", tmp, ["-std=c11", "-O1", "-Wextra"])
        run(a.cxx, cxx, "menu.cpp", tmp, ["-std=c++20"])
    print("ok")


if __name__ == "__main__":
    main()
