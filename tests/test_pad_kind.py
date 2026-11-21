"""((Triforce) 11-20, ticket "Controls not working properly") host/pad_kind.h: the controllers left out of the game's input
(instruments and dance mats), and hos_sdl.c's list asking it for each one.

    python -I -B tests/test_pad_kind.py [--cc gcc]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

C = r"""
#include "pad_kind.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)
int main(void) {
    CHECK(pad_kind_left_out(6, NULL) && !strcmp(pad_kind_left_out(6, NULL), "a guitar"), "SDL's guitar type");
    CHECK(pad_kind_left_out(7, "x") != NULL, "a drum kit by type");
    CHECK(pad_kind_left_out(5, NULL) != NULL, "a dance mat by type");
    CHECK(pad_kind_left_out(1, "(Guitar Hero )") != NULL, "the ticket's guitar, by its name (typed as a gamepad)");
    CHECK(pad_kind_left_out(0, "Rock Band Drums") != NULL, "drums by name");
    CHECK(pad_kind_left_out(1, "Nintendo Switch Pro Controller") == NULL, "a Switch Pro controller is used");
    CHECK(pad_kind_left_out(1, "DualSense Wireless Controller") == NULL, "a DualSense is used");
    CHECK(pad_kind_left_out(1, "Xbox Series X Controller") == NULL, "an Xbox pad is used");
    CHECK(pad_kind_left_out(2, "Logitech G29") == NULL, "a wheel is left as it was");
    CHECK(pad_kind_left_out(0, NULL) == NULL, "nothing known: used");
    if (fails) return 1;
    puts("pad kind: all checks passed");
    return 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    a = p.parse_args()
    src = (ROOT / "host" / "hos_sdl.c").read_text(encoding="utf-8")
    for need, why in (('#include "pad_kind.h"', "hos_sdl.c includes the rule"),
                      ("if (!pad_left_out((int)got[i], (int32_t)got[i]))", "SDL3's list asks it"),
                      ("if (!pad_left_out(j, id))", "SDL2's list asks it"),
                      ('"SDL_GetJoystickTypeForID" : "SDL_JoystickGetDeviceType"', "the type is looked up in both SDLs"),
                      ('getenv("HOS_SDL_ALL_PADS")', "HOS_SDL_ALL_PADS=1 keeps every controller")):
        if need not in src:
            sys.exit("FAIL: host/hos_sdl.c: %s" % why)
    with tempfile.TemporaryDirectory(prefix="pad-kind-") as tmp:
        c = Path(tmp) / "t.c"
        c.write_text(C, encoding="utf-8")
        exe = Path(tmp) / "t.exe"
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host"), str(c), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout.strip())
        if r.returncode:
            sys.exit(1)


if __name__ == "__main__":
    main()
