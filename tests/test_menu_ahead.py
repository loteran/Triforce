"""(Triforce) Opening the options never waits for NVIDIA's or AMD's libraries, nor for the game's button icons.

What the rows ask of the upscalers' libraries (whether NVIDIA DLSS, AMD FSR 4 and FSR 4's frame generation run here: NGX's
start, AMD's loader) was asked at the menu's open on the presenting thread, and the button icons were read there at its
first frame: the first open of our tabs inside the game's Options screen took 2.1 s (test11). Now:

1. gpu/vk/vk_menu.cpp's menu_ahead and ahead_thread, cut out and run against stand-ins for the probes (dlss_probe,
   fsr4_probe, fsr4fg::why), one of which takes 300 ms: menu_ahead returns at once, asks each probe once on another
   thread, and says when the answers are in; before the device is made it asks nothing; with TRIFORCE_MENU_AHEAD=0 it asks
   them on the caller's thread, as before.
2. The source: the menu's two opens call menu_ahead, not the probes; menu_tick asks ahead as soon as the game runs and sets
   the Frame generation row again once the answers are in; the rows read the frame generation's answer as known
   (fsr4fg::why_known, which never asks AMD's library), never fsr4fg::why or offered; the button icons are asked for as the
   game's Options screen comes, and are read on a thread of their own (vk_imgui.cpp read_glyphs), the next begin making
   them a font of the atlas.

python -I -B tests/test_menu_ahead.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def function(source, start):
    at = source.find(start)
    if at < 0:
        raise SystemExit("%s is missing" % start.rstrip(" {("))
    body = source[at:]
    return body[:body.index("\n}\n") + 3]


def between(source, start, end):
    a = source.find(start)
    b = source.find(end, a + 1)
    if a < 0 or b < 0:
        raise SystemExit("not found: %r ... %r" % (start[:50], end[:50]))
    return source[a:b]


FIXTURE = r'''
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
namespace framegen_rows { enum Why { FG_OK, FG_NOT_YET }; }
namespace mkwvk {
struct { void* device = (void*)1; bool offscreen = false; } dev;
std::atomic<int> dlss_calls{0}, fsr_calls{0}, fg_calls{0};
std::thread::id probe_thread;
std::atomic<bool> probe_seen{false};
int64_t prof_now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void logf(const char*, ...) {}
void dlss_probe() {   // (NGX's start: slow)
    probe_thread = std::this_thread::get_id(), probe_seen = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    dlss_calls++;
}
void fsr4_probe() { fsr_calls++; }
namespace fsr4fg {
framegen_rows::Why why() { fg_calls++; return framegen_rows::FG_OK; }
}
/* SECTION */
}  // namespace mkwvk
using namespace mkwvk;
int main(int argc, char** argv) {
    CHECK(argc == 2);
    const bool inline_ = !std::strcmp(argv[1], "inline");
    dev.device = nullptr;
    menu_ahead();
    CHECK(ahead_state.load() == 0 && !dlss_calls && !fsr_calls && !fg_calls);   // (no device yet: nothing asked)
    dev.device = (void*)1;
    const int64_t t0 = prof_now();
    menu_ahead();
    const int64_t took = prof_now() - t0;
    if (inline_) {   // (TRIFORCE_MENU_AHEAD=0: on this thread, as before)
        CHECK(ahead_state.load() == 2 && dlss_calls == 1 && fsr_calls == 1 && fg_calls == 1);
        CHECK(probe_thread == std::this_thread::get_id());
        return std::puts("PASS: TRIFORCE_MENU_AHEAD=0 asks at the open, on the menu's thread"), 0;
    }
    CHECK(took < 100000000LL);   // (at once, though the probe takes 300 ms)
    menu_ahead();   // (a second open, the answers not in yet: nothing more asked)
    for (int i = 0; i < 500 && ahead_state.load() != 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(ahead_state.load() == 2);
    CHECK(dlss_calls == 1 && fsr_calls == 1 && fg_calls == 1);
    CHECK(probe_seen && probe_thread != std::this_thread::get_id());
    menu_ahead();
    CHECK(dlss_calls == 1 && fsr_calls == 1 && fg_calls == 1);   // (answered: asked once in the run)
    return std::puts("PASS: the upscalers asked once, off the menu's thread, the open not waiting"), 0;
}
'''


def source_checks(menu, imgui):
    def need(text, what, where):
        if what not in text:
            raise SystemExit("%s: missing %r" % (where, what))

    tick = function(menu, "void menu_tick() {")
    for bad in ("dlss_probe()", "fsr4_probe()"):
        if bad in tick:
            raise SystemExit("menu_tick asks %s on the menu's thread: the open waits for it" % bad)
    if tick.count("menu_ahead();") < 3:
        raise SystemExit("menu_tick: the upscalers are not asked ahead, and at both opens")
    need(tick, "if (ahead_allowed() && dev.device && !dev.offscreen && game_frames.load(std::memory_order_relaxed) > 0) menu_ahead();",
         "menu_tick (ahead, as soon as the game runs)")
    need(tick, "if (!resynced && ahead_state.load(std::memory_order_acquire) == 2) resynced = true, framegen_row_sync();",
         "menu_tick (the Frame generation row set again)")
    need(tick, "if (in_game_options) ui::want_glyphs();", "menu_tick (the button icons asked for ahead)")
    rows = menu[:menu.index("void menu_tick() {")]
    for bad in ("fsr4fg::offered()", "fsr4fg::why()"):
        if rows.replace("(void)fsr4fg::why()", "").count(bad):
            raise SystemExit("vk_menu.cpp: a row asks AMD's library itself (%s): drawing it waits for the library" % bad)
    need(function(menu, "void framegen_row_sync() {"), "fsr4fg::offered_known()", "framegen_row_sync")
    want = function(imgui, "void want_glyphs() {")
    need(want, "std::thread(read_glyphs).detach();", "vk_imgui.cpp want_glyphs")
    need(function(imgui, "bool begin("), "if (glyphs_state.load(std::memory_order_acquire) == 2) load_glyphs();", "vk_imgui.cpp begin")
    need(function(imgui, "game_font::Reader& font_reader(bool* ok) {"), "std::lock_guard lk(mu);", "vk_imgui.cpp font_reader")
    fsr4 = (VK / "vk_fsr4.cpp").read_text(encoding="utf-8")
    known = function(fsr4, "framegen_rows::Why why_known() {\n    static const bool fsr_off")
    if "probe(" in known or "lock" in known:
        raise SystemExit("vk_fsr4.cpp why_known asks AMD's library or waits for its lock")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    imgui = (VK / "vk_imgui.cpp").read_text(encoding="utf-8")
    source_checks(menu, imgui)
    section = between(menu, "std::atomic<int> ahead_state{0};", "bool row_shown(int i) {")
    with tempfile.TemporaryDirectory(prefix="menu-ahead-") as tmp:
        cpp, exe = Path(tmp) / "ahead.cpp", Path(tmp) / ("ahead" + (".exe" if os.name == "nt" else ""))
        cpp.write_text(FIXTURE.replace("/* SECTION */", section), encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)] +
                       ([] if os.name == "nt" else ["-pthread"]), check=True)
        env = dict(os.environ)
        env.pop("TRIFORCE_MENU_AHEAD", None)
        subprocess.run([str(exe), "ahead"], check=True, timeout=60, env=env)
        env["TRIFORCE_MENU_AHEAD"] = "0"
        subprocess.run([str(exe), "inline"], check=True, timeout=60, env=env)
    print("PASS: the menu's open waits for no library")


if __name__ == "__main__":
    main()
