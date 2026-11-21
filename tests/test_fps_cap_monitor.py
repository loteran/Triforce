"""The Game frame rate cap's "Monitor's rate": the cap is the refresh rate of the display the window is on, rounded, from 30
to 240, and the console's 60 when the rate cannot be read.

The rule is gpu/vk/fps_cap.h (the renderer answers the host with it, HOS_OPT_DISPLAY_CAP, and the row shows it); the host's side
is host/nn_vi.c's own fps_cap_of and fps_max, cut out and run against stand-ins: the row's other values as before, HOS_FPS_MAX
over the row, and a cap that stays 60 and unsettled until the renderer answers, since the game asks before the renderer is up.

    python -I -B tests/test_fps_cap_monitor.py [--cc gcc] [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(source, marker):
    start = source.index(marker)
    return source[start:start + re.search(r"^}", source[start:], re.M).end()]


RULE = r'''#include "@ROOT@/gpu/vk/fps_cap.h"
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    using namespace mkwvk::fps_cap;
    CHECK(kMonitor == 6 && kLowest == 30 && kHighest == 240);
    /* the usual displays, as Windows and SDL report them */
    CHECK(of_display(60000) == 60 && of_display(59940) == 60 && of_display(59951) == 60);
    CHECK(of_display(75000) == 75 && of_display(100000) == 100 && of_display(120000) == 120 && of_display(119880) == 120);
    CHECK(of_display(144000) == 144 && of_display(143856) == 144 && of_display(165000) == 165 && of_display(164835) == 165);
    CHECK(of_display(90000) == 90 && of_display(240000) == 240 && of_display(239760) == 240);
    /* rounded to the nearest whole rate */
    CHECK(of_display(74499) == 74 && of_display(74500) == 75);
    /* past what the game's pacing and time step take: the nearest they do */
    CHECK(of_display(360000) == 240 && of_display(500000) == 240 && of_display(240500) == 240);
    CHECK(of_display(24000) == 30 && of_display(23976) == 30 && of_display(29970) == 30 && of_display(1000) == 30);
    CHECK(of_display(48000) == 48 && of_display(50000) == 50);   /* (under 60 is a cap the host takes: 30 to 59) */
    /* not known: 0, which the host answers with 60 */
    CHECK(of_display(0) == 0 && of_display(-1) == 0);
    CHECK(of_display(2147483647) == 240);   /* (no overflow in the rounding) */
    std::puts("fps cap, the display's: all checks passed");
    return 0;
}
'''

HOST = r'''#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
enum { HOS_OPT_DISPLAY_CAP = @ID@ };
static int logs;
static char last_log[256];
static void hos_log(const char *fmt, ...) { logs++; snprintf(last_log, sizeof last_log, "%s", fmt); }
/* (the stand-ins: the options file's row, read once; the renderer, not up until it is told to be) */
static int row, ini_reads, renderer_up, display, asked;
int hos_ini_option(const char *key, int otherwise) { ini_reads++; return strcmp(key, "frame_rate_cap") ? otherwise : row; }
int rx_option(int id, int otherwise) { asked++; return renderer_up && id == HOS_OPT_DISPLAY_CAP ? display : otherwise; }
static volatile long long presents;
@HOST@
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "rule")) {
        /* the row's values as before: 60, 90, 120, 144, 165, 240, and 45, 40, 30 under it */
        static const int caps[] = {60, 90, 120, 144, 165, 240};
        for (int i = 0; i < 6; i++) CHECK(fps_cap_of(i, NULL, 0) == caps[i] && fps_cap_of(i, NULL, -1) == caps[i] && fps_cap_of(i, NULL, 144) == caps[i]);
        CHECK(fps_cap_of(-1, NULL, 0) == 45 && fps_cap_of(-2, NULL, 0) == 40 && fps_cap_of(-3, NULL, 0) == 30);
        CHECK(fps_cap_of(-4, NULL, 0) == 60 && fps_cap_of(7, NULL, 144) == 60 && fps_cap_of(99, NULL, 144) == 60);   /* (past the row: 60) */
        /* Monitor's rate: what the renderer says of the display, 30 to 240 whatever it says; not known: 60; no renderer yet: unsettled */
        CHECK(fps_cap_of(6, NULL, 144) == 144 && fps_cap_of(6, NULL, 60) == 60 && fps_cap_of(6, NULL, 75) == 75 && fps_cap_of(6, NULL, 240) == 240);
        CHECK(fps_cap_of(6, NULL, 50) == 50 && fps_cap_of(6, NULL, 30) == 30);
        CHECK(fps_cap_of(6, NULL, 360) == 240 && fps_cap_of(6, NULL, 24) == 30 && fps_cap_of(6, NULL, 1) == 30);
        CHECK(fps_cap_of(6, NULL, 0) == 60);
        CHECK(fps_cap_of(6, NULL, -1) == 0);
        /* HOS_FPS_MAX over the row, Monitor's rate too, in its own range as before (20 to 240; under 20, 60) */
        CHECK(fps_cap_of(6, "120", -1) == 120 && fps_cap_of(6, "120", 144) == 120 && fps_cap_of(0, "90", 0) == 90);
        CHECK(fps_cap_of(6, "1000", 144) == 240 && fps_cap_of(6, "25", 144) == 25 && fps_cap_of(2, "10", 0) == 60 && fps_cap_of(6, "60", 144) == 60);
        /* no value of the row gives more than 240: "unlimited" is not one of them */
        for (int r = -8; r <= 16; r++)
            for (int d = -1; d <= 1000; d += 7) CHECK(fps_cap_of(r, NULL, d) <= 240 && (fps_cap_of(r, NULL, d) >= 30 || (r == 6 && d < 0)));
    } else if (!strcmp(mode, "late")) {
        /* Monitor's rate while the renderer starts: 60, asked again each time, the options file read once; then the display's, kept */
        row = 6;
        for (int i = 0; i < 500; i++) CHECK(fps_max() == 60);
        CHECK(ini_reads == 1 && asked == 500 && logs == 0);
        renderer_up = 1, display = 144;
        CHECK(fps_max() == 144 && logs == 2 && strstr(last_log, "time step follow"));
        display = 60, renderer_up = 0;   /* (the window moved, anything: the cap is the start's) */
        for (int i = 0; i < 10; i++) CHECK(fps_max() == 144);
        CHECK(asked == 501 && ini_reads == 1);
    } else if (!strcmp(mode, "unknown")) {
        /* a renderer that is up and cannot read the rate (no window, a compositor that does not say): 60, settled and said */
        row = 6, renderer_up = 1, display = 0;
        CHECK(fps_max() == 60 && logs == 1 && strstr(last_log, "not known"));
        display = 120;
        CHECK(fps_max() == 60 && asked == 1);
    } else if (!strcmp(mode, "no answer")) {
        /* a renderer that never answers (the WebGPU one): 60, settled at the first frame presented */
        row = 6;
        for (int i = 0; i < 20; i++) CHECK(fps_max() == 60);
        CHECK(asked == 20 && logs == 0);
        presents = 1;
        CHECK(fps_max() == 60 && logs == 1 && asked == 21);
        CHECK(fps_max() == 60 && asked == 21);
    } else if (!strcmp(mode, "fifty")) {
        /* a 50 Hz display: a cap under 60, which paces by itself */
        row = 6, renderer_up = 1, display = 50;
        CHECK(fps_max() == 50 && strstr(last_log, "keeps real time"));
    } else if (!strcmp(mode, "fixed")) {
        /* a fixed cap asks the renderer nothing and settles at once, as before */
        row = 2;
        CHECK(fps_max() == 120 && asked == 0 && ini_reads == 1);
        CHECK(fps_max() == 120 && ini_reads == 1);
    } else if (!strcmp(mode, "env")) {
        /* HOS_FPS_MAX (set by the test) over Monitor's rate: the renderer is not asked */
        row = 6, renderer_up = 1, display = 144;
        CHECK(fps_max() == 90 && asked == 0);
    } else {
        return 2;
    }
    printf("PASS: %s\n", mode);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()

    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    ident = re.search(r"enum \{ HOS_OPT_DISPLAY_CAP = (\d+) \};", api)
    check(ident, "vk_api.h has no HOS_OPT_DISPLAY_CAP")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    check("if (id == HOS_OPT_DISPLAY_CAP) return fps_cap::of_display(display_refresh_mhz());" in replay,
          "the renderer does not answer HOS_OPT_DISPLAY_CAP with fps_cap.h's rule for the window's display")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    # ((Triforce, options_version 7) the Graphics tab's Frame rate & latency group: Frame rate cap)
    check(re.search(r'\{TAB_GRAPHICS, "Frame rate cap", &opts\.fps_cap, -3, 6, 1, tx_fps_cap,', menu),
          "the Frame rate cap row does not go from 30 (-3) to Monitor's rate (6)")
    label = block(menu, "const char* tx_fps_cap(int v) {")
    check("fps_cap::of_display(display_refresh_mhz())" in label and '"Monitor\'s rate (%d)"' in label,
          "the row does not say what Monitor's rate gives on this display")
    check("Unlimited" not in label and "unlimited" not in label.lower(), "the row offers Unlimited: the game's time step stops at 240")
    vi = (ROOT / "host/nn_vi.c").read_text(encoding="utf-8")
    # the vsync event and the pacing follow a cap that settles late
    vsync = block(vi, "static DWORD WINAPI vsync_main(void *p) {")
    loop = vsync[vsync.index("for (;;)"):]
    check("1000000000LL / fps_max()" in loop, "the vsync event's period is taken once, before Monitor's rate is known")
    presented = block(vi, "void hos_vi_native_presented(void) {")
    check("const int interval = fps_max() < 60 ? 1 : present_interval;" in presented and "fps_max() * interval" in presented,
          "a cap under 60 that settles late is paced with the game's own interval on top")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    check("| Frame rate cap | 30, 40, 45, 60, 90, 120, 144, 165, 240, Monitor's rate" in readme,
          "readme.md's settings table has no Frame rate cap row with Monitor's rate")
    guide = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("| `frame_rate_cap` | 0 60, 1 90, 2 120, 3 144, 4 165, 5 240, 6 the monitor's refresh rate; -1 45, -2 40, -3 30 |" in guide,
          "docs/development.md's options table has no frame_rate_cap")

    host = "\n".join(block(vi, m) for m in ("static int fps_cap_of(int row, const char *env, int display) {", "static int fps_max(void) {"))
    with tempfile.TemporaryDirectory(prefix="fps-cap-monitor-") as tmp:
        work = Path(tmp)
        rule, rule_exe = work / "rule.cpp", work / "rule.exe"
        rule.write_text(RULE.replace("@ROOT@", ROOT.as_posix()), encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(rule), "-o", str(rule_exe)], check=True)
        subprocess.run([str(rule_exe)], check=True)
        src, exe = work / "host.c", work / "host.exe"
        src.write_text(HOST.replace("@ID@", ident.group(1)).replace("@HOST@", host), encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        plain = {k: v for k, v in os.environ.items() if not k.startswith("HOS_")}
        for mode in ("rule", "late", "unknown", "no answer", "fifty", "fixed"):
            subprocess.run([str(exe), mode], env=plain, check=True)
        subprocess.run([str(exe), "env"], env=dict(plain, HOS_FPS_MAX="90"), check=True)


if __name__ == "__main__":
    main()
