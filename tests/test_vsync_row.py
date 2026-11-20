"""The Insert menu's VSync row: the swapchain's present mode (gpu/vk/present_mode.h). On, the default, asks for exactly what
was asked for before the row on every driver (mailbox; without it immediate on Windows and FIFO elsewhere), checked here
against that rule written out again; Off asks for immediate wherever the surface lists it, and is On where it does not;
RX_VK_FIFO and RX_VK_IMMEDIATE still go over the row. The swapchain is made again when the row changes
(tests/test_present_semaphores.py runs that part of present_image), and the row's line says what its choice came to on
this driver (gpu/vk/vk_menu.cpp's own hn_vsync, cut out and run).

    python -I -B tests/test_vsync_row.py [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def function(source, start):
    at = source.find(start)
    check(at >= 0, "%s is missing" % start.rstrip(" {("))
    return source[at:source.index("\n}\n", at) + 3]


RULE = r'''#include "@ROOT@/gpu/vk/present_mode.h"
#include <cstdio>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
using namespace mkwvk::present_mode;
/* the choice as vk_main.cpp made it before the row (create_swapchain, v0.4.0), written out again */
static Mode before(bool mailbox, bool immediate, bool windows, bool force_fifo, bool want_immediate) {
    Mode mode = kFifo;
    if (!force_fifo) {
        if (mailbox) mode = kMailbox;
        if (windows ? (want_immediate || mode == kFifo) : want_immediate)
            if (immediate) mode = kImmediate;
    }
    return mode;
}
int main() {
    for (int bits = 0; bits < 32; bits++) {
        const bool mailbox = bits & 1, immediate = bits & 2, windows = bits & 4, fifo = bits & 8, want = bits & 16;
        /* On: what it was, on every driver and with either switch */
        CHECK(choose(mailbox, immediate, true, windows, fifo, want) == before(mailbox, immediate, windows, fifo, want));
        /* Off: immediate where the surface lists it, else On's; RX_VK_FIFO over it */
        const Mode off = choose(mailbox, immediate, false, windows, fifo, want);
        if (fifo) CHECK(off == kFifo);
        else if (immediate) CHECK(off == kImmediate);
        else CHECK(off == choose(mailbox, immediate, true, windows, fifo, want) && off != kImmediate);
        /* never a mode the surface does not list (FIFO always is there) */
        for (int vsync = 0; vsync < 2; vsync++) {
            const Mode m = choose(mailbox, immediate, vsync, windows, fifo, want);
            CHECK(m == kFifo || (m == kMailbox && mailbox) || (m == kImmediate && immediate));
        }
    }
    /* the drivers met: NVIDIA (all three), AMD on Windows (no mailbox), Mesa (all three), a surface with FIFO alone */
    CHECK(choose(true, true, true, true, false, false) == kMailbox && choose(true, true, false, true, false, false) == kImmediate);
    CHECK(choose(false, true, true, true, false, false) == kImmediate && choose(false, true, false, true, false, false) == kImmediate);
    CHECK(choose(true, true, true, false, false, false) == kMailbox && choose(true, true, false, false, false, false) == kImmediate);
    CHECK(choose(false, true, true, false, false, false) == kFifo && choose(false, true, false, false, false, false) == kImmediate);
    CHECK(choose(false, false, true, true, false, false) == kFifo && choose(false, false, false, true, false, false) == kFifo);
    CHECK(!std::strcmp(name(kFifo), "fifo") && !std::strcmp(name(kMailbox), "mailbox") && !std::strcmp(name(kImmediate), "immediate"));
    std::puts("present mode: all checks passed");
    return 0;
}
'''

MENU = r'''#include "@ROOT@/gpu/vk/present_mode.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
namespace present_mode = mkwvk::present_mode;
int now = -1;
int swapchain_present_mode() { return now; }
@HINT@
int main(int argc, char** argv) {
    const char* env = argc > 1 ? argv[1] : "";
    /* every line a value can have, for the help's height: three modes and the two switches, then none */
    for (int v = 0; v < 2; v++) {
        int n = 0;
        while (hn_vsync(v, n)) n++;
        CHECK(n == 5 && !hn_vsync(v, 5));
    }
    if (!std::strcmp(env, "fifo")) {
        for (int v = 0; v < 2; v++) CHECK(hn_vsync(v, -1) == hn_vsync(v, 3) && std::strstr(hn_vsync(v, -1), "RX_VK_FIFO"));
    } else if (!std::strcmp(env, "immediate")) {
        for (int v = 0; v < 2; v++) CHECK(hn_vsync(v, -1) == hn_vsync(v, 4) && std::strstr(hn_vsync(v, -1), "RX_VK_IMMEDIATE"));
    } else {
        CHECK(!hn_vsync(1, -1) && !hn_vsync(0, -1));   /* (no swapchain yet, or offscreen: nothing said) */
        for (now = present_mode::kFifo; now <= present_mode::kImmediate; now++)
            for (int v = 0; v < 2; v++) CHECK(hn_vsync(v, -1) && hn_vsync(v, -1) == hn_vsync(v, now));
        /* On with mailbox: no tearing; Off with immediate: at once and can tear; each says so when the driver gave the other */
        CHECK(std::strstr(hn_vsync(1, present_mode::kMailbox), "no tearing") && std::strstr(hn_vsync(1, present_mode::kFifo), "no tearing"));
        CHECK(std::strstr(hn_vsync(1, present_mode::kImmediate), "as with Off"));
        CHECK(std::strstr(hn_vsync(0, present_mode::kImmediate), "can tear"));
        CHECK(std::strstr(hn_vsync(0, present_mode::kMailbox), "as On") && std::strstr(hn_vsync(0, present_mode::kFifo), "as On"));
    }
    std::printf("vsync row (%s): all checks passed\n", *env ? env : "no switch");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    vk = ROOT / "gpu" / "vk"
    main_cpp = (vk / "vk_main.cpp").read_text(encoding="utf-8")
    create = function(main_cpp, "bool create_swapchain(")
    check("present_mode::choose(has_mailbox, has_immediate, opts.vsync != 0, windows, force_fifo, want_immediate)" in create,
          "create_swapchain does not choose the present mode by present_mode.h's rule and the VSync row")
    for need in ('getenv("RX_VK_FIFO") != nullptr', 'getenv("RX_VK_IMMEDIATE") != nullptr',
                 "has_mailbox |= m == VK_PRESENT_MODE_MAILBOX_KHR", "has_immediate |= m == VK_PRESENT_MODE_IMMEDIATE_KHR",
                 "ci.presentMode = mode;", "present_mode_now.store((int)chosen"):
        check(need in create, "create_swapchain no longer has: " + need)
    check(re.search(r"#ifdef _WIN32\s*const bool windows = true;\s*#else\s*const bool windows = false;\s*#endif", create),
          "create_swapchain no longer tells the rule which system it is built for")
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    present = function(replay, "void present_image(")
    check("const bool vsync_changed = vsync_was >= 0 && vsync_was != opts.vsync;" in present and
          re.search(r"if \(want \|\| !dev\.swapchain \|\| reflex_swapchain_stale\(\) \|\| hdr_changed \|\| vsync_changed( \|\| ffxfg::swapchain_stale\(\))?\)", present) and
          "hdr_was = opts.hdr, vsync_was = opts.vsync;" in present,
          "present_image does not make the swapchain again when the VSync row changes")
    menu = (vk / "vk_menu.cpp").read_text(encoding="utf-8")
    row = re.search(r'\{TAB_DISPLAY, "VSync", &opts\.vsync, 0, 1, 1, tx_onoff,.*?hn_vsync, false, true\},', menu, re.S)
    check(row and "Changes at once" in row.group(0), "the VSync row is not an On / Off row of the Display tab that changes at once")
    header = (vk / "vk_menu.h").read_text(encoding="utf-8")
    check(re.search(r"\bint vsync = 1;", header), "VSync is not on by default: the present mode would not be the one from before the row")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    check("| VSync | On, Off" in readme, "readme.md's settings table has no VSync row")
    guide = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("`RX_VK_FIFO` (set), `RX_VK_IMMEDIATE` (set)" in guide, "docs/development.md does not say the two present mode switches")

    with tempfile.TemporaryDirectory(prefix="vsync-row-") as tmp:
        work = Path(tmp)
        src, exe = work / "rule.cpp", work / "rule.exe"
        src.write_text(RULE.replace("@ROOT@", ROOT.as_posix()), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        src, exe = work / "menu.cpp", work / "menu.exe"
        src.write_text(MENU.replace("@ROOT@", ROOT.as_posix()).replace("@HINT@", function(menu, "const char* hn_vsync(int v, int variant) {")),
                       encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        plain = {k: v for k, v in os.environ.items() if k not in ("RX_VK_FIFO", "RX_VK_IMMEDIATE")}
        subprocess.run([str(exe)], check=True, env=plain)
        subprocess.run([str(exe), "fifo"], check=True, env=dict(plain, RX_VK_FIFO="1"))
        subprocess.run([str(exe), "immediate"], check=True, env=dict(plain, RX_VK_IMMEDIATE="1"))
    print("PASS: the VSync row and the swapchain's present mode")


if __name__ == "__main__":
    main()
