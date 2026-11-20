"""On an ultrawide picture a fade covers the whole picture, whatever the Ultrawide menus row says.

The game's UI keeps its 16:9 shape in the middle of a wider picture (nx2/oot_wide.py). Four screens keep the whole picture
(host/oot_wide_hud.h oot_hud_keep_classes): the item wheel's background, the pause and system menus' background, the
event fade and the loading fade. The Insert menu's Ultrawide menus row on Centred takes screens out of that list
(gpu/vk/vk_framegen.cpp wide_sides, kMenus) so that a menu keeps its shape in the middle with blurred or black sides.

It took the loading fade out too (kMenus was 0xa: bit 3 was taken for a menu's): a loading fade then covered the middle
16:9 alone and the picture showed at its left and right while a place loaded. A fade is one colour: both fades stay
whole at every value of the row, as NX2navi's widescreen keeps them.

Compiles host/oot_wide_hud.h and asks it, with the mask vk_framegen.cpp gives the host for each value of the row.

    python -I -B tests/test_wide_fades_whole.py [--cc gcc]
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

C = r'''
#include <stdio.h>
#include <stdlib.h>
#include "oot_wide_hud.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void) {
    const uint32_t menus = KMENUS;
    /* (the table's order is what the bits mean: nx2/oot_wide.py KEEP_VTABLES, main at 0x80020000) */
    CHECK(OOT_HUD_KEEP_N == 4);
    const uint64_t wheel = oot_hud_keep_classes[0], pause = oot_hud_keep_classes[1], event_fade = oot_hud_keep_classes[2],
                   loading_fade = oot_hud_keep_classes[3];
    CHECK(wheel == 0x80020000ULL + 0x4bed098 && pause == 0x80020000ULL + 0x4be4fb0);
    CHECK(event_fade == 0x80020000ULL + 0x4be61c8 && loading_fade == 0x80020000ULL + 0x4be7b30);
    /* the row on Full width: every one of the four whole */
    const uint32_t full = 0xfu, centred = 0xfu & ~menus;
    CHECK((oot_hud_keep_bit(wheel) & full) && (oot_hud_keep_bit(pause) & full) && (oot_hud_keep_bit(event_fade) & full) &&
          (oot_hud_keep_bit(loading_fade) & full));
    /* the row on Centred (blurred sides or black bars): the menus' background in the middle, and both fades still whole */
    CHECK(!(oot_hud_keep_bit(pause) & centred));
    CHECK(oot_hud_keep_bit(event_fade) & centred);
    CHECK(oot_hud_keep_bit(loading_fade) & centred);
    CHECK(oot_hud_keep_bit(wheel) & centred);   /* (the wheel's background is the last frame of play: whole, as before) */
    CHECK(!oot_hud_keep_bit(0x84c00000ULL));    /* (any other screen: the HUD and the menus, in the middle) */
    printf("ultrawide: the menus' background alone leaves the keep list with the row on Centred; both fades stay whole\n");
    return 0;
}
'''


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    framegen = (ROOT / "gpu/vk/vk_framegen.cpp").read_text(encoding="utf-8")
    header = (ROOT / "host/oot_wide_hud.h").read_text(encoding="utf-8")
    lift = (ROOT / "nx2/oot_wide.py").read_text(encoding="utf-8")
    m = re.search(r"constexpr uint32_t kMenus = (0x[0-9a-fA-F]+);", framegen)
    check(m is not None, "vk_framegen.cpp wide_sides has no kMenus")
    check("host->wide_menus(opts.wide_menus ? 0xfu & ~kMenus : 0xfu)" in framegen,
          "wide_sides no longer gives the host the keep mask by the Ultrawide menus row")
    # (the header's table and the lift's list are the same four, in the same order)
    table = re.findall(r"0x84c0([0-9a-f]{4})ULL,", header)
    listed = re.search(r"KEEP_VTABLES = \(([^)]*)\)", lift)
    check(listed is not None and re.findall(r"MAIN \+ 0x4be([0-9a-f]{4})", listed.group(1)) ==
          [t for t in (x[0:4] for x in ("d098", "4fb0", "61c8", "7b30"))],
          "nx2/oot_wide.py KEEP_VTABLES is not the four screens in the order host/oot_wide_hud.h has them")
    check(table == ["d098", "4fb0", "61c8", "7b30"], "host/oot_wide_hud.h's table is not the four screens in that order")
    with tempfile.TemporaryDirectory(prefix="wide-fades-") as tmp:
        src = Path(tmp) / "fades.c"
        src.write_text(C, encoding="utf-8")
        exe = Path(tmp) / "fades.exe"
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Werror", "-DKMENUS=" + m.group(1) + "u", "-I", str(ROOT / "host"), "-o", str(exe),
                        str(src)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        check(r.returncode == 0, "with kMenus = %s: %s" % (m.group(1), r.stderr.strip()))


if __name__ == "__main__":
    main()
