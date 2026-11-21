"""The sticks' dead zone is the player's: the Insert menu's "Stick dead zone" row (stick_dead_zone in
triforce_options.ini), issue 752.

Both pad paths, SDL (host/pad_sdl.c) and XInput (host/nn_hid.c), cut each axis at XInput's 7849 (~24%), a lot on a
worn or a precise stick. They now cut through one function, host/nn_hid.c's hos_stick_axis, whose threshold is the
row's (option 11): Default (7849, the same output as before for every input), 5%, 10%, 15%, 20%, 30%.
The function, pad_sdl.c's stick_value and the menu's row, label and ini parsing are compiled alone (gcc, g++) and
checked over every int16 input. Both paths now cut through hos_stick_axis_own, which is hos_stick_axis for a stick with
no dead zone of its own (the Left and Right stick dead zone rows at "As Stick dead zone", their default): checked here
with none; tests/test_stick_own_zone.py has the sticks' own.

python -I -B tests/test_stick_dead_zone.py [--cc gcc] [--cxx g++]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
THRESHOLDS = [7849, 1638, 3277, 4915, 6553, 9830]   # (Default, then 5% ... 30% of 32767)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def run(compiler, src, name, tmp, flags):
    path = Path(tmp) / name
    path.write_text(src, encoding="utf-8")
    exe = Path(tmp) / (name + ".exe")
    subprocess.run([compiler, *flags, "-Wall", "-Werror", "-o", str(exe), str(path)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    check(r.returncode == 0, name + ": " + r.stderr.strip())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    sdl = (ROOT / "host" / "pad_sdl.c").read_text(encoding="utf-8")
    api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (ROOT / "gpu" / "vk" / "vk_menu.h").read_text(encoding="utf-8")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")

    check(not re.search(r"HOS_OPT_\w+ = 11\b", api), "a HOS_OPT_ enum also takes option 11")
    check("int32_t hos_stick_axis(" in hid, "nn_hid.c has no hos_stick_axis")
    pads = block(hid, "static void pads_read(")
    check("rx_option(11, 0)" in pads and "hos_stick_axis_own(thumb[i], zone, own[i >> 1])" in pads,
          "XInput's sticks do not go through hos_stick_axis_own with the row's zone (option 11)")
    check("7849" not in pads, "XInput's sticks still cut at 7849 themselves")
    poll = block(sdl, "static void pads_poll")
    check("rx_option(11, 0)" in poll, "SDL's sticks do not read the row (option 11)")
    check(poll.count("stick_value(") == 4, "SDL's four axes do not all go through stick_value")
    value = block(sdl, "static int32_t stick_value(")
    check("hos_stick_axis_own(" in value and "7849" not in value, "pad_sdl.c's stick_value cuts on its own")
    own = block(hid, "int32_t hos_stick_axis_own(")
    check("if (own < 0) return hos_stick_axis(v, zone);" in own,
          "a stick with no dead zone of its own is no longer cut by hos_stick_axis and the row's zone alone")
    option = replay[replay.index("t.option = "):]
    option = option[:option.index("};") + 2]
    check("if (id == 11) return opts.stick_dead_zone;" in option, "the host cannot see the menu's row (option 11)")
    check("int stick_dead_zone = 0;" in header, "the option's default is not 0 (Default)")
    # ((Triforce, options_version 7) the shared Stick dead zone row is gone: each stick's own row has its Default, XInput's
    # 7849, and an older file's shared zone is moved into each stick's own where it had none (merged_rows.h migrate). The
    # option stays the host's, always 0 now)
    check('"Stick dead zone", &opts.stick_dead_zone' not in menu, "the Insert menu still has a shared Stick dead zone row")
    merged = (ROOT / "gpu" / "vk" / "merged_rows.h").read_text(encoding="utf-8")
    check('"stick_dead_zone"' in merged[merged.index("constexpr const char* kDropped[]"):], "an old stick_dead_zone= line is kept")
    check('if (!strcmp(line, "stick_dead_zone")) before7.stick_shared = atoi(eq + 1);' in menu and
          "merged_rows::migrate(version, before7, opts.output_res, opts.lstick_dead_zone, opts.rstick_dead_zone);" in menu,
          "menu_init does not move an older file's shared dead zone into each stick's own")
    check("\n| Left stick dead zone, Right stick dead zone | Default (24%), or 0% to 50%" in readme,
          "readme.md's settings table has no Left and Right stick dead zone row")

    c = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
''' + block(hid, "int32_t hos_stick_axis(") + "\n" + own + "\n" + block(sdl, "static int32_t stick_value(") + r'''
/* (before issue 752: nn_hid.c's XInput cut, and pad_sdl.c's, which flips first) */
static int32_t old_xinput(int16_t t) { const int a = t < -32767 ? -32767 : t; return (a > 7849 || a < -7849) ? a : 0; }
static int32_t old_sdl(int16_t v, int flip) {
    int32_t a = v < -32767 ? -32767 : v;
    if (flip) a = -a;
    return (a > 7849 || a < -7849) ? a : 0;
}
int main(void) {
    static const int32_t t[6] = {''' + ", ".join(map(str, THRESHOLDS)) + r'''};
    static const int bad[] = {-1, 6, 99, -2147483647 - 1};
    for (int32_t i = -32768; i <= 32767; i++) {
        const int16_t v = (int16_t)i;
        CHECK(hos_stick_axis(v, 0) == old_xinput(v));
        for (int k = 0; k < 4; k++) CHECK(hos_stick_axis(v, bad[k]) == old_xinput(v));   /* (out of range: Default) */
        /* (-1: a stick with no dead zone of its own, the Left and Right stick dead zone rows' default) */
        CHECK(stick_value(v, 0, 0, -1) == old_sdl(v, 0) && stick_value(v, 1, 0, -1) == old_sdl(v, 1));
        CHECK(hos_stick_axis_own(v, 0, -1) == old_xinput(v));
        for (int z = 0; z < 6; z++) {
            const int32_t a = i < -32767 ? -32767 : i, want = a > t[z] || a < -t[z] ? a : 0;
            CHECK(hos_stick_axis(v, z) == want && hos_stick_axis_own(v, z, -1) == want);
            CHECK(stick_value(v, 0, z, -1) == want && stick_value(v, 1, z, -1) == -want);
        }
    }
    for (int z = 0; z < 6; z++)
        CHECK(hos_stick_axis(t[z], z) == 0 && hos_stick_axis(t[z] + 1, z) == t[z] + 1 && hos_stick_axis(-t[z], z) == 0 &&
              hos_stick_axis(-t[z] - 1, z) == -t[z] - 1 && hos_stick_axis(-32768, z) == -32767);
    printf("stick: Default is the old cut for all 65536 inputs, both paths; 5 other thresholds exact\n");
    return 0;
}
'''

    # (the migration: a shared zone moved into a stick's own cuts every input where the shared one did, so a file from before
    # plays the same)
    cxx = r'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "''' + (ROOT / "gpu" / "vk" / "merged_rows.h").as_posix() + r'''"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
''' + block(hid, "int32_t hos_stick_axis(") + "\n" + own + r'''
int main() {
    namespace mr = mkwvk::merged_rows;
    for (int z = 0; z < 6; z++) {
        int out = 0, l = -1, r = 12;   /* (the right stick has a zone of its own: it keeps it) */
        mr::Before7 old;
        old.stick_shared = z;
        mr::migrate(6, old, out, l, r);
        CHECK(l == mr::kSharedZone[z] && r == 12 && out == 0);
        for (int32_t i = -32768; i <= 32767; i++)
            CHECK(hos_stick_axis_own((int16_t)i, 0, l) == hos_stick_axis((int16_t)i, z));
        int l7 = -1, r7 = -1;
        mr::migrate(8, old, out, l7, r7);   /* (a file of version 8 has no shared zone to move; v0.5.1's 7 still had one) */
        CHECK(l7 == -1 && r7 == -1);
    }
    printf("menu: an older file's shared dead zone, moved into each stick's own, cuts all 65536 inputs where it did\n");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="stick-dead-zone-") as tmp:
        run(a.cc, c, "stick.c", tmp, ["-std=c11", "-O1"])
        run(a.cxx, cxx, "menu.cpp", tmp, ["-std=c++17"])
    print("ok")


if __name__ == "__main__":
    main()
