"""The Insert menu's Vibration curve (Controls): a power on the motors' rumble, before the Rumble strength.

OoT's rumbles are mostly weak ones, a few percent of a motor's strength, and the strength row scales them all alike. The
curve (host/pad_sdl.c rumble_curve, the row's hundredths: 20 to 150, 100 none) lifts the weak ones towards the strong
ones under 1.00, as NX2navi's rumble_curve does. Checked here, with the function compiled from the source: 1.00 gives
every motor value back as it is (the default changes nothing), silence and the top stay, a stronger rumble never comes
out the weaker, under 1.00 nothing gets weaker and over it nothing stronger, and the values follow the power. Then the
poll (the curve before the strength, the motors alone: a Switch controller's HD rumble does not take it), the option id,
the row and its place in the options file, and the two tables that document it.

    python -I -B tests/test_rumble_curve.py [--cc gcc] [--cxx g++]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "g++"]

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


def run(compiler, src, name, tmp, flags, libs=()):
    path = Path(tmp) / name
    path.write_text(src, encoding="utf-8")
    exe = Path(tmp) / (name + ".exe")
    subprocess.run([compiler, *flags, "-Wall", "-Werror", "-o", str(exe), str(path), *libs], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    check(r.returncode == 0, name + ": " + r.stderr.strip())


C = r'''#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
/* CURVE */
/* STRENGTH */
/* (the documented curve, worked out again: the motor's strength as 0 to 1, to the power) */
static double doc(unsigned value, double power) { return pow(value / 65535.0, power) * 65535.0; }
int main(void) {
    static const int lifts[] = {20, 40, 60, 80, 95}, lowers[] = {105, 120, 150};
    /* 1.00, the default: every motor value as it is; so is no curve at all (0, what an option table that does not know
       the row answers, and less) */
    for (unsigned v = 0; v <= 65535; v++)
        CHECK(rumble_curve(v, 100) == v && rumble_curve(v, 0) == v && rumble_curve(v, -60) == v);
    CHECK(rumble_curve(70000, 100) == 65535 && rumble_curve(70000, 60) == 65535);   /* (never past the motor's top) */
    /* under 1.00: silence and the top stay, nothing gets weaker, a stronger rumble never comes out the weaker */
    for (int c = 0; c < 5; c++) {
        unsigned last = 0;
        CHECK(rumble_curve(0, lifts[c]) == 0 && rumble_curve(65535, lifts[c]) == 65535);
        for (unsigned v = 1; v <= 65535; v++) {
            const unsigned o = rumble_curve(v, lifts[c]);
            CHECK(o >= v && o >= last && o <= 65535);
            CHECK(fabs((double)o - doc(v, lifts[c] / 100.0)) <= 1.0);
            last = o;
        }
    }
    /* over 1.00: the same, the other way */
    for (int c = 0; c < 3; c++) {
        unsigned last = 0;
        CHECK(rumble_curve(0, lowers[c]) == 0 && rumble_curve(65535, lowers[c]) == 65535);
        for (unsigned v = 1; v <= 65535; v++) {
            const unsigned o = rumble_curve(v, lowers[c]);
            CHECK(o <= v && o >= last);
            CHECK(fabs((double)o - doc(v, lowers[c] / 100.0)) <= 1.0);
            last = o;
        }
    }
    /* a lower curve never gives less, for any value; past the row's ends it is the end's */
    for (unsigned v = 0; v <= 65535; v += 5) {
        CHECK(rumble_curve(v, 20) >= rumble_curve(v, 60) && rumble_curve(v, 60) >= rumble_curve(v, 100));
        CHECK(rumble_curve(v, 100) >= rumble_curve(v, 150));
        CHECK(rumble_curve(v, 5) == rumble_curve(v, 20) && rumble_curve(v, 999) == rumble_curve(v, 150));
    }
    /* what it is for: OoT's weak rumbles. A tenth of the motor's strength at 0.60 is a quarter; a half gains less, in
       proportion; at 1.50 the tenth is a thirtieth */
    CHECK(rumble_curve(6554, 60) > 16300 && rumble_curve(6554, 60) < 16600);
    CHECK(rumble_curve(32768, 60) > 43100 && rumble_curve(32768, 60) < 43400);
    CHECK((double)rumble_curve(6554, 60) / 6554 > (double)rumble_curve(32768, 60) / 32768);
    CHECK(rumble_curve(6554, 150) > 2000 && rumble_curve(6554, 150) < 2150);
    {   /* through the strength, as the poll calls them: at 1.00 and 100% the motors get the game's values; the curve first,
           then the percentage; Off is silence whatever the curve */
        static const unsigned values[] = {0, 1, 300, 6554, 20000, 43690, 65535};
        uint16_t out[2];
        for (int i = 0; i < 7; i++)
            for (int j = 0; j < 7; j++) {
                rumble_strength(rumble_curve(values[i], 100), rumble_curve(values[j], 100), 100, out);
                CHECK(out[0] == values[i] && out[1] == values[j]);
                rumble_strength(rumble_curve(values[i], 60), rumble_curve(values[j], 60), 0, out);
                CHECK(out[0] == 0 && out[1] == 0);
            }
        rumble_strength(rumble_curve(6554, 60), rumble_curve(0, 60), 50, out);
        CHECK(out[0] == rumble_curve(6554, 60) / 2 && out[1] == 0);
        rumble_strength(rumble_curve(6554, 60), rumble_curve(32768, 60), 300, out);   /* (300%: the stronger at the top) */
        CHECK(out[1] == 65535 && out[0] == (unsigned)((unsigned long long)rumble_curve(6554, 60) * 65535 / rumble_curve(32768, 60)));
    }
    printf("curve: 1.00 is every motor value as it is; under it nothing weaker, over it nothing stronger, the order kept\n");
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
/* TX_SENS */
Row rows[] = {/* THE_ROW */};
/* STOPS */
/* INI_KEY */
static int output_choice(int v) { return v; }
static int parse(const char* text) {
    opts.rumble_curve = 100;
    char line[80];
    snprintf(line, sizeof line, "%s", text);
    char* eq = strchr(line, '=');
    *eq = 0;
    /* LOAD_LOOP */
    return opts.rumble_curve;
}
int main() {
    Options fresh;
    CHECK(fresh.rumble_curve == 100 && fresh.rumble == 100);   // (out of the box: the rumble as it was)
    const Row& r = rows[0];
    CHECK(r.value == &opts.rumble_curve && r.lo == 20 && r.hi == 150 && r.step == 5);
    CHECK(r.preset_exempt && !r.restart);   // (a control: the quality preset stays, and it applies at once)
    CHECK(!strcmp(ini_key(r), "vibration_curve"));
    CHECK(stops_at_ends(r));   // (a scale: 0.20 does not wrap round to 1.50)
    CHECK(!strcmp(r.text(100), "1.00") && !strcmp(r.text(20), "0.20") && !strcmp(r.text(150), "1.50") && !strcmp(r.text(60), "0.60"));
    CHECK(parse("vibration_curve=60\n") == 60 && parse("vibration_curve=100\n") == 100 && parse("vibration_curve=63\n") == 63);
    CHECK(parse("vibration_curve=5\n") == 20 && parse("vibration_curve=900\n") == 150 && parse("vibration_curve=x\n") == 20);
    CHECK(parse("rumble_curve=60\n") == 100);   // (NX2navi's key is not this file's)
    printf("menu: Vibration curve 0.20 to 1.50 by 0.05, 1.00 out of the box, saved as vibration_curve\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    pad, api = read("host", "pad_sdl.c"), read("gpu", "vk", "vk_api.h")
    replay, menu, header = read("gpu", "vk", "vk_replay.cpp"), read("gpu", "vk", "vk_menu.cpp"), read("gpu", "vk", "vk_menu.h")

    # ---- the poll: the curve on each motor's value, then the strength; before the send; not HD rumble's ----
    poll = re.sub(r"\s+", " ", block(pad, "static void pads_poll"))
    check("rx_option(HOS_OPT_RUMBLE_CURVE, 100)" in poll, "the poll does not read the Vibration curve (1.00 without the menu)")
    call = re.search(r"\brumble_strength\(([^;]*)\);", poll)
    check(call is not None, "the poll does not scale the rumble")
    check(re.fullmatch(r"rumble_curve\(\(uint16_t\)rumble_low, curve\), rumble_curve\(\(uint16_t\)rumble_high, curve\), "
                       r"strength, motors", call.group(1)) is not None,
          "the curve is not on both motors' values, heavy then light, before the Rumble strength: " + call.group(1))
    check(call.start() < poll.index("rumble_due("), "the curve comes after the send")
    hd = poll[poll.index("float hv[2][4]"):poll.index("for (int i = 0; i < MAX_PADS; i++) { void *gc = pads[i].gc;")]
    check(hd.count("hd_rumble_encode(") == 3 and "curve" not in hd,
          "HD rumble takes the Vibration curve: it has its own (hd_rumble_felt), and the row says it is not changed")

    # ---- the option: 50, answered from the row's value, kept to its range ----
    ids = re.search(r"enum \{ HOS_OPT_RUMBLE_CURVE = (\d+)\b", api)
    check(ids is not None and ids.group(1) == "50", "HOS_OPT_RUMBLE_CURVE is not option 50")
    option = block(replay, "t.option = [](int id) -> int {")
    check("if (id == HOS_OPT_RUMBLE_CURVE) return std::clamp(opts.rumble_curve, 20, 150);" in option,
          "the host cannot see the menu's Vibration curve (option 50)")
    check('#include "../gpu/vk/vk_api.h"' in pad, "pad_sdl.c does not see the option's name")

    # ---- the row and the options file ----
    row = re.search(r'\{TAB_CONTROLS, "Vibration curve", &opts\.rumble_curve, 20, 150, 5, tx_sens,\s*"[^}]*\},', menu)
    check(row is not None, "the Controls tab has no Vibration curve row (0.20 to 1.50 by 0.05)")
    check("HD rumble is not changed" in re.sub(r'"\s+"', "", row.group(0)), "the row's help does not say HD rumble is left alone")
    strength = re.search(r'\{TAB_CONTROLS, "Rumble strength", &opts\.rumble, 0, 300, 25, tx_rumble,', menu)
    check(strength is not None and strength.start() < row.start(), "the curve's row is not under Rumble strength (0 to 300)")
    loop = block(block(menu, "void menu_init("), "for (Row& r : rows) {")
    check("ini_key(r)" in loop, "menu_init's options loop no longer reads rows by ini_key")
    cxx = CXX
    for marker, text in (("/* OPTIONS */", re.search(r"struct Options \{.*?\n\};", header, re.S).group(0)),
                         ("/* TABS */", re.search(r"enum MenuTab \{.*?\};", menu, re.S).group(0)),
                         ("/* ROW */", re.search(r"struct Row \{.*?\n\};", menu, re.S).group(0)),
                         ("/* TX_SENS */", block(menu, "const char* tx_sens(")),
                         ("/* THE_ROW */", row.group(0)),
                         ("/* STOPS */", block(menu, "bool stops_at_ends(")),
                         ("/* INI_KEY */", block(menu, "bool ini_row(") + "\n" + block(menu, "const char* ini_key(")),
                         ("/* LOAD_LOOP */", loop)):
        cxx = cxx.replace(marker, text)
    c = C.replace("/* CURVE */", block(pad, "static unsigned rumble_curve(")).replace(
        "/* STRENGTH */", block(pad, "static void rumble_strength("))

    # ---- the tables that document it ----
    check(re.search(r"^\| Vibration curve \| 0\.20 to 1\.50", read("readme.md"), re.M) is not None,
          "readme.md's settings table has no Vibration curve row")
    check(re.search(r"^\| Rumble strength \| Off, 25% to 300%", read("readme.md"), re.M) is not None,
          "readme.md's settings table does not give Rumble strength's range (Off, 25% to 300%)")
    check(re.search(r"^\| `vibration_curve` \| hundredths, 20 to 150", read("docs", "development.md"), re.M) is not None,
          "docs/development.md's options table does not say what vibration_curve's number is")

    with tempfile.TemporaryDirectory(prefix="rumble-curve-") as tmp:
        run(a.cc, c, "curve.c", tmp, ["-std=c11", "-O1", "-Wextra"], ["-lm"])
        run(a.cxx, cxx, "menu.cpp", tmp, ["-std=c++20"])
    print("ok")


if __name__ == "__main__":
    main()
