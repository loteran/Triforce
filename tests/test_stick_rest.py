"""A stick axis steers only once it has been seen at rest (host/pad_sdl.c, #1673, #1661).

The block from host/pad_sdl.c's poll that clears an axis not yet seen at rest is compiled over a stand-in pad, behind the
file's own cut of the axes (stick_value, at the default Stick dead zone and with no dead zone of a stick's own, as out of
the box), then checked:

- an axis read as held from the start (a phantom device, a trigger mapped onto a stick) never steers;
- a stick seen inside the dead zone steers from then on, held either way;
- a worn stick that rests just past the dead zone steers once it is pushed the other way (it went past its rest);
- the bits are per axis: one axis held for good leaves the others free.

With a dead zone of a stick's own (the Left and Right stick dead zone rows) what counts as rest is wider than the cut:
tests/test_stick_own_zone.py.

python -I -B tests/test_stick_rest.py [--cc clang]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]


def function(text, marker):
    """text from marker to the end of the first {...} after it"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    src = (ROOT / "host/pad_sdl.c").read_text(encoding="utf-8")
    hid = (ROOT / "host/nn_hid.c").read_text(encoding="utf-8")
    start = src.index("            /* (a worn stick resting just past a small dead zone")
    end = src.index("if (!(pads[i].rested >> k & 1)) v[k] = 0;", start)
    end = src.index("}", end) + 1
    block = src[start:end]
    cut = [function(hid, "int32_t hos_stick_axis("), function(hid, "int32_t hos_stick_axis_own("),
           function(src, "static int32_t stick_value("), function(src, "static int stick_rests(")]
    fixture = r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
static struct { unsigned rested; signed char held[4]; } pads[1];
''' + "\n".join(cut) + r'''
/* the poll's own lines: the axes cut (none turned round here; zone 0 the default Stick dead zone, -1 no dead zone of a
   stick's own), then the rule. w: the axes as SDL gives them in, what steers out */
static void step(int i, int w[4]) {
    const int16_t raw[4] = {(int16_t)w[0], (int16_t)w[1], (int16_t)w[2], (int16_t)w[3]};
    static const int own[2] = {-1, -1};
    int32_t v[4];
    for (int k = 0; k < 4; k++) v[k] = stick_value(raw[k], 0, 0, own[k >> 1]);
''' + block + r'''
    for (int k = 0; k < 4; k++) w[k] = v[k];
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    /* a phantom: axis 2 held at -32767 from the start, never at rest */
    memset(pads, 0, sizeof pads);
    for (int n = 0; n < 100; n++) {
        int w[4] = {0, 0, -32767, 0};
        step(0, w);
        CHECK(w[2] == 0);
    }
    /* the other axes are free: axis 0 seen at rest, then held */
    { int w[4] = {20000, 0, -32767, 0}; step(0, w); CHECK(w[0] == 20000 && w[2] == 0); }
    /* a stick at rest at the start steers either way */
    memset(pads, 0, sizeof pads);
    { int w[4] = {0, 0, 0, 0}; step(0, w); }
    { int w[4] = {-30000, 12000, 0, 0}; step(0, w); CHECK(w[0] == -30000 && w[1] == 12000); }
    /* a worn stick resting past the dead zone (held +9000 from the start): not until pushed the other way */
    memset(pads, 0, sizeof pads);
    for (int n = 0; n < 10; n++) { int w[4] = {9000, 0, 0, 0}; step(0, w); CHECK(w[0] == 0); }
    { int w[4] = {-25000, 0, 0, 0}; step(0, w); CHECK(w[0] == -25000); }
    { int w[4] = {9000, 0, 0, 0}; step(0, w); CHECK(w[0] == 9000); }
    printf("PASS: stick axes steer once seen at rest; a held phantom axis never does\n");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="stick-rest-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / "fixture.exe"
        c.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        print(r.stdout, end="")
        if r.returncode:
            print(r.stderr, end="")
            raise SystemExit("test_stick_rest: failed")


if __name__ == "__main__":
    main()
