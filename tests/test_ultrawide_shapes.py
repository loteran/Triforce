"""The Ultrawide row's shapes by name: the game's own 16:9, and a fixed 16:10, 21:9 or 32:9 for a screen or a window of that
shape (the owner, 11-16: "remove monitors shape as a option keep 16:9 16:10 21:9 32:9"). An options file from before
options_version 6 whose row is 1 had the main monitor's shape, no longer a choice: it is read as the shape nearest the main
monitor's (gpu/vk/ultrawide_rows.h). host/hos_main.c's own ultrawide_ratio and its widescreen sizes (the block
tests/test_widescreen_shapes.py checks with screen ratios), cut out and run: the row's value, the file's version and the main
monitor's size in, the picture, the scene, the camera's ratio and the HUD's share out. And the menu's side: the row's range, what
it says for each value, and the old row's 1 read the same way in menu_init (gpu/vk/vk_menu.cpp).

    python -I -B tests/test_ultrawide_shapes.py [--cc gcc] [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def function(source, head):
    start = source.index(head)
    return source[start:source.index("\n}\n", start) + 3]


def sizes(source):
    """The sizes, from the 16:9 constant to the else branch's log line, as hos_main.c has them (tests/test_widescreen_shapes.py)"""
    start = source.index("        const double game = 16.0 / 9.0;")
    end = source.index("\n", source.index("hos_log(\"boot: widescreen: a %.4f screen", start))
    return source[start:end] + "\n        }\n"


HOST = r'''
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile uint64_t hos_wide_out, hos_wide_scene;
volatile float hos_wide_ratio, hos_wide_squeeze, hos_wide_margin;
float hos_aspect_ratio;
static unsigned out_h = 1440;
uint32_t hos_output_size(int height) { return height ? out_h : out_h * 16 / 9; }
static void hos_log(const char *f, ...) { (void)f; }
@ROWS@
@RATIO@
static void wide(float want) {
@SIZES@
}

int main(int argc, char **argv) {
    if (argc < 6) return 2;
    out_h = (unsigned)atoi(argv[5]);
    const float want = ultrawide_ratio(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    wide(want);
    printf("%.4f %llux%llu %llux%llu %.4f %.4f %.4f %.4f\n", (double)want,
           (unsigned long long)(hos_wide_out & 0xffffffffu), (unsigned long long)(hos_wide_out >> 32),
           (unsigned long long)(hos_wide_scene & 0xffffffffu), (unsigned long long)(hos_wide_scene >> 32),
           (double)hos_wide_ratio, (double)hos_wide_squeeze, (double)hos_wide_margin, (double)hos_aspect_ratio);
    return 0;
}
'''

MENU = r'''
#include <algorithm>
#include <cstdio>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
@ROWS@
@HINT_IN@
@HINTS@
@TEXT@
@HINT@
int main() {
    CHECK(!std::strcmp(tx_ultrawide(0), "16:9") && !std::strcmp(tx_ultrawide(1), "16:10"));
    CHECK(!std::strcmp(tx_ultrawide(2), "21:9") && !std::strcmp(tx_ultrawide(3), "32:9"));
    CHECK(ultrawide_row_ratio(0) == 0.0f && ultrawide_row_ratio(1) == 1.6f && ultrawide_row_ratio(-2) == 0.0f);   /* (the shapes' ratios) */
    CHECK(!std::strcmp(tx_ultrawide(-1), "16:9") && !std::strcmp(tx_ultrawide(4), "32:9"));   /* (past the row: its nearest end) */
    for (int v = 0; v <= 3; v++) CHECK(hn_ultrawide(v, -1) && hn_ultrawide(v, 0) == hn_ultrawide(v, -1) && !hn_ultrawide(v, 1));
    CHECK(std::strstr(hn_ultrawide(1, -1), "16:10") && std::strstr(hn_ultrawide(2, -1), "21:9") && std::strstr(hn_ultrawide(3, -1), "32:9"));
    for (int v = 1; v <= 3; v++) CHECK(std::strstr(hn_ultrawide(v, -1), "black bars"));
    CHECK(!hn_ultrawide(4, -1) && !hn_ultrawide(-1, -1));
    for (int v = 0; v <= 3; v++) CHECK(!std::strstr(tx_ultrawide(v), "onitor") && !std::strstr(hn_ultrawide(v, -1), "onitor"));
    /* (the old row's 1, Monitor's shape, read by the main monitor's shape before options_version 6; as chosen from 6 on) */
    CHECK(ultrawide_row_read(1, 5, 2560, 1440) == 0 && ultrawide_row_read(1, 5, 1920, 1200) == 1 && ultrawide_row_read(1, 5, 2560, 1600) == 1);
    CHECK(ultrawide_row_read(1, 5, 3440, 1440) == 2 && ultrawide_row_read(1, 5, 2560, 1080) == 2 && ultrawide_row_read(1, 5, 5120, 1440) == 3);
    CHECK(ultrawide_row_read(1, 5, 1024, 768) == 1 && ultrawide_row_read(1, 5, 0, 0) == 0 && ultrawide_row_read(1, 0, 3440, 1440) == 2);
    CHECK(ultrawide_row_read(1, 6, 3440, 1440) == 1 && ultrawide_row_read(1, 7, 2560, 1440) == 1);
    for (int r = 0; r <= 3; r++) if (r != 1) CHECK(ultrawide_row_read(r, 5, 3440, 1440) == r && ultrawide_row_read(r, 6, 1920, 1200) == r);
    std::puts("ultrawide row: all checks passed");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()

    rows_h = (ROOT / "gpu/vk/ultrawide_rows.h").read_text(encoding="utf-8")
    host = (ROOT / "host/hos_main.c").read_text(encoding="utf-8")
    # the options file's ultrawide= is the row's number, read with the file's version, and HOS_ASPECT still goes over it
    check('#include "../gpu/vk/ultrawide_rows.h"' in host, "hos_main.c does not take the row's shapes from ultrawide_rows.h")
    check('if (!strncmp(line, "ultrawide=", 10)) row = atoi(line + 10);' in host, "hos_main.c does not read ultrawide= as the row's value")
    check('if (!strncmp(line, "options_version=", 16)) version = atoi(line + 16);' in host,
          "hos_main.c does not read the options file's version beside ultrawide=")
    check("want = ultrawide_ratio(row, version, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));" in host,
          "hos_main.c does not take the picture's shape from the Ultrawide row")
    env = host.index('const char *e = getenv("HOS_ASPECT");')
    check("if (!e) {" in host[env:host.index("want = ultrawide_ratio(", env)], "HOS_ASPECT no longer goes over the Ultrawide row")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    row = re.search(r'\{TAB_DISPLAY, "Ultrawide", &opts\.ultrawide, 0, 3, 1, tx_ultrawide,.*?hn_ultrawide, true\},', menu, re.S)
    check(row, "the Ultrawide row is not 16:9, 16:10, 21:9, 32:9 with a restart")
    check("Monitor's shape" not in function(menu, "const char* tx_ultrawide(int v) {"), "the Ultrawide row still offers Monitor's shape")
    init = function(menu, "void menu_init() {")
    check("opts.ultrawide = ultrawide_row_read(opts.ultrawide, version, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));"
          in init, "menu_init does not read an old file's Monitor's shape as the nearest shape, as the host does")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    check("| Ultrawide | 16:9 (the game's own), 16:10, 21:9, 32:9" in readme, "readme.md's settings table does not list the Ultrawide row's four shapes")
    guide = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("| `ultrawide` | 0 16:9, 1 16:10, 2 21:9, 3 32:9 (before `options_version` 6, 1 was the monitor's shape" in guide,
          "docs/development.md's options table has no ultrawide")

    # (row, the options file's version, the main monitor, the output's height) -> the shape asked, the picture, the scene, the
    # camera's ratio, the HUD's share, the menus' margin, and the shape the present keeps
    cases = (
        # 16:9: the game's own picture, whatever the screen
        ((0, 6, 3440, 1440, 1440), "0.0000 0x0 0x0 0.0000 0.0000 0.0000 0.0000"),
        ((0, 6, 1920, 1080, 1440), "0.0000 0x0 0x0 0.0000 0.0000 0.0000 0.0000"),
        # 16:10 by name: the same picture on a 16:9 screen, an ultrawide one, a 16:10 one or none at all
        ((1, 6, 1920, 1080, 1440), "1.6000 2304x1440 1728x1080 1.6000 1.0000 0.0000 1.6000"),
        ((1, 6, 3440, 1440, 1440), "1.6000 2304x1440 1728x1080 1.6000 1.0000 0.0000 1.6000"),
        ((1, 6, 1920, 1200, 1440), "1.6000 2304x1440 1728x1080 1.6000 1.0000 0.0000 1.6000"),
        ((1, 6, 0, 0, 1440), "1.6000 2304x1440 1728x1080 1.6000 1.0000 0.0000 1.6000"),
        # an options file from before version 6 whose 1 was Monitor's shape: the shape nearest the main monitor's
        ((1, 5, 3440, 1440, 1440), "2.3704 3416x1440 2560x1080 2.3722 0.7494 0.1253 2.3722"),
        ((1, 5, 5120, 1440, 1440), "3.5556 5120x1440 3840x1080 3.5556 0.5000 0.2500 3.5556"),
        ((1, 5, 2560, 1440, 1440), "0.0000 0x0 0x0 0.0000 0.0000 0.0000 0.0000"),
        ((1, 5, 1920, 1200, 1440), "1.6000 2304x1440 1728x1080 1.6000 1.0000 0.0000 1.6000"),
        ((1, 5, 0, 0, 1440), "0.0000 0x0 0x0 0.0000 0.0000 0.0000 0.0000"),
        # 21:9 by name: the same picture on a 16:9 screen, an ultrawide one or none at all (64:27, in whole multiples of 8 pixels)
        ((2, 6, 1920, 1080, 1440), "2.3704 3416x1440 2560x1080 2.3722 0.7494 0.1253 2.3722"),
        ((2, 6, 3440, 1440, 1440), "2.3704 3416x1440 2560x1080 2.3722 0.7494 0.1253 2.3722"),
        ((2, 6, 0, 0, 1440), "2.3704 3416x1440 2560x1080 2.3722 0.7494 0.1253 2.3722"),
        ((2, 5, 1920, 1080, 1080), "2.3704 2560x1080 1920x810 2.3704 0.7500 0.1250 2.3704"),
        # 32:9 by name
        ((3, 6, 1920, 1080, 1440), "3.5556 5120x1440 3840x1080 3.5556 0.5000 0.2500 3.5556"),
        ((3, 5, 2560, 1440, 1080), "3.5556 3840x1080 2880x810 3.5556 0.5000 0.2500 3.5556"),
        # a value past the row: its nearest end, as the menu reads it
        ((-1, 6, 3440, 1440, 1440), "0.0000 0x0 0x0 0.0000 0.0000 0.0000 0.0000"),
        ((7, 6, 1920, 1080, 1440), "3.5556 5120x1440 3840x1080 3.5556 0.5000 0.2500 3.5556"),
    )
    with tempfile.TemporaryDirectory(prefix="ultrawide-") as tmp:
        work = Path(tmp)
        source, exe = work / "host.c", work / "host.exe"
        source.write_text(HOST.replace("@ROWS@", rows_h)
                              .replace("@RATIO@", function(host, "static float ultrawide_ratio(int row, int version, int screen_w, int screen_h) {"))
                              .replace("@SIZES@", sizes(host)), encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", str(source), "-o", str(exe), "-lm"], check=True)
        bad = []
        for case, expect in cases:
            got = subprocess.run([str(exe)] + [str(v) for v in case], capture_output=True, text=True, check=True).stdout.strip()
            if got != expect:
                bad.append("row %d (version %d) on a %dx%d screen at %d: %s, expected %s" % (case + (got, expect)))
        if bad:
            raise SystemExit("FAIL:\n  " + "\n  ".join(bad))
        # at the 4K output the fixed shapes stay within 3840 x 2160's pixels (the render targets' pool)
        for r in (1, 2, 3):
            got = subprocess.run([str(exe), str(r), "6", "1920", "1080", "2160"], capture_output=True, text=True, check=True).stdout.split()
            w, h = (int(v) for v in got[1].split("x"))
            check(w * h <= 3840 * 2160 and w % 8 == 0 and h % 8 == 0 and abs(w / h - float(got[0])) < 0.02,
                  "row %d at the 4K output: a %dx%d picture" % (r, w, h))
        # the menu's own texts, and the old row's 1 read as the host reads it
        source, exe = work / "menu.cpp", work / "menu.exe"
        hints = re.search(r"#define MENU_HINTS\(fn, lo, \.\.\.\).*?\n    \}\n", menu, re.S)
        check(hints, "vk_menu.cpp: MENU_HINTS is gone")
        hint = re.search(r"MENU_HINTS\(hn_ultrawide, 0, .*?\"\)\n", menu, re.S)
        check(hint, "vk_menu.cpp: the Ultrawide row has no line for its values")
        source.write_text(MENU.replace("@ROWS@", rows_h)
                              .replace("@HINT_IN@", function(menu, "const char* hint_in(const char* const* t, int n, int lo, int v, int variant) {"))
                              .replace("@HINTS@", hints.group(0)).replace("@TEXT@", function(menu, "const char* tx_ultrawide(int v) {"))
                              .replace("@HINT@", hint.group(0)), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the Ultrawide row's 16:9, 16:10, 21:9 and 32:9, and an old file's monitor's shape read as the nearest of them")


if __name__ == "__main__":
    main()
