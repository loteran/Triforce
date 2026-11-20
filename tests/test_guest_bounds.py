"""Bounds checks on the game's arguments that cannot overflow (host/guest_bounds.h; no game).

A check written `first + n > cap` wraps around when the game's start or count is huge and lets the range through. Each
of the checks this replaced is compiled here as it was, with the inputs that wrapped it (under -fwrapv, so the wrap is
the defined one a build gets), and shown to pass them; the new check refuses them, and answers as the old one did for
every input that did not wrap:

- host/nvn_native.c nvnCommandBufferSetViewports and bind_vbufs: `first + n > 16` in an int;
- host/nvdrv.c MAP_BUFFER_EX's remap: `va + offset + size <= map.va + map.size` in 64 bits;
- host/oot_menu.c set_item_key: `buf + n + 1 > HOS_GUEST_SPACE` in 64 bits.

Reads the source for the call sites: each uses the new check and none of the old expressions is left.

    python -I -B tests/test_guest_bounds.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

PROGRAM = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "guest_bounds.h"

static int failed;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failed = 1; } } while (0)

/* the checks as they were (true: the range was taken) */
static int old_span(int first, int n) { return !(n < 0 || first < 0 || first + n > 16); }
static int old_remap(uint64_t map_va, uint64_t map_size, uint64_t va, uint64_t off, uint64_t size) {
    return va + off + size <= map_va + map_size;
}
static int old_item(uint64_t buf, uint64_t n, uint64_t space) { return !(buf + n + 1 > space); }

static int new_remap(uint64_t map_va, uint64_t map_size, uint64_t va, uint64_t off, uint64_t size) {
    const uint64_t at = va - map_va;   /* (as nvdrv.c: map_floor found the mapping at or before va) */
    return hos_range_ok(at, off, map_size) && hos_range_ok(at + off, size, map_size);
}

int main(void) {
    /* SetViewports / BindVertexBuffers: first + n wrapped in an int and passed */
    CHECK(old_span(1, INT32_MAX));                 /* (the overflow the old check let through) */
    CHECK(old_span(INT32_MAX, INT32_MAX));
    CHECK(!hos_span_ok(1, INT32_MAX, 16));
    CHECK(!hos_span_ok(INT32_MAX, INT32_MAX, 16));
    CHECK(!hos_span_ok(INT32_MAX, 1, 16));
    for (int first = -3; first <= 20; first++)
        for (int n = -3; n <= 20; n++) CHECK(hos_span_ok(first, n, 16) == old_span(first, n));
    CHECK(hos_span_ok(0, 16, 16) && hos_span_ok(16, 0, 16) && !hos_span_ok(16, 1, 16) && !hos_span_ok(-1, 0, 16));

    /* MAP_BUFFER_EX remap: a mapping of 1 MiB at 0x400000000; offset or size near 2^64 wrapped the sum */
    const uint64_t mva = 0x400000000ull, msz = 0x100000ull;
    CHECK(old_remap(mva, msz, mva, UINT64_MAX - mva + 1, 0x1000));   /* (offset wraps the sum back to 0x1000) */
    CHECK(old_remap(mva, msz, mva + 0x10, 0x10, UINT64_MAX - 0x10));
    CHECK(!new_remap(mva, msz, mva, UINT64_MAX - mva + 1, 0x1000));
    CHECK(!new_remap(mva, msz, mva + 0x10, 0x10, UINT64_MAX - 0x10));
    CHECK(!new_remap(mva, msz, mva + msz + 0x1000, 0, 0));          /* (past the mapping's end) */
    for (uint64_t at = 0; at <= msz + 0x2000; at += 0x800)
        for (uint64_t off = 0; off <= msz + 0x2000; off += 0x1800)
            for (uint64_t size = 0; size <= msz + 0x2000; size += 0x2800)
                CHECK(new_remap(mva, msz, mva + at, off, size) == old_remap(mva, msz, mva + at, off, size));

    /* set_item_key: buf + n + 1 wrapped for a pointer read wrong */
    const uint64_t space = 1ull << 39;
    CHECK(old_item(UINT64_MAX - 3, 8, space));
    CHECK(!hos_range_ok(UINT64_MAX - 3, 8 + 1, space));
    CHECK(!hos_range_ok(space, 1, space) && hos_range_ok(space - 9, 9, space) && !hos_range_ok(space - 8, 9, space));
    for (uint64_t buf = space - 40; buf <= space + 40; buf++)
        for (uint64_t n = 0; n < 40; n++) CHECK(hos_range_ok(buf, n + 1, space) == old_item(buf, n, space));

    /* hos_span_clip: what is left of a row from x */
    CHECK(hos_span_clip(1, UINT32_MAX, 640) == 639);   /* (x + n in 32 bits: 0, and the whole n was taken) */
    CHECK(hos_span_clip(0, 640, 640) == 640 && hos_span_clip(600, 100, 640) == 40 && hos_span_clip(640, 1, 640) == 0);

    if (!failed) printf("PASS: the bounds checks refuse what wrapped the old ones, and agree with them elsewhere\n");
    return failed;
}
'''


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_source():
    nvn = (ROOT / "host/nvn_native.c").read_text(encoding="utf-8")
    nvdrv = (ROOT / "host/nvdrv.c").read_text(encoding="utf-8")
    menu = (ROOT / "host/oot_menu.c").read_text(encoding="utf-8")
    for text, name in ((nvn, "nvn_native.c"), (nvdrv, "nvdrv.c"), (menu, "oot_menu.c")):
        if '#include "guest_bounds.h"' not in text:
            raise SystemExit("FAIL: host/%s does not include guest_bounds.h" % name)
    if "first + n > 16" in nvn:
        raise SystemExit("FAIL: host/nvn_native.c still checks first + n in an int")
    for fn, marker in (("SetViewports", "NV(nvnCommandBufferSetViewports) {"), ("bind_vbufs", "static void bind_vbufs(")):
        if "hos_span_ok(first, n, 16)" not in block(nvn, marker):
            raise SystemExit("FAIL: host/nvn_native.c %s does not check its first and count with hos_span_ok" % fn)
    remap = nvdrv[nvdrv.index("case 0x06: {               /* MAP_BUFFER_EX"):]
    remap = remap[:remap.index("Handle *h = handle(")]
    if "va + G64(p + 16) + G64(p + 24)" in remap or remap.count("hos_range_ok(") != 2:
        raise SystemExit("FAIL: host/nvdrv.c MAP_BUFFER_EX's remap check is not the one tested here")
    item = block(menu, "static int set_item_key(")
    if "buf + n + 1 >" in item or "hos_range_ok(buf, n + 1, HOS_GUEST_SPACE)" not in item:
        raise SystemExit("FAIL: host/oot_menu.c set_item_key's buffer check is not the one tested here")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="guest-bounds-") as tmp:
        c = Path(tmp) / "bounds.c"
        exe = Path(tmp) / "bounds.exe"
        c.write_text(PROGRAM, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-O2", "-fwrapv", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host"), str(c),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
