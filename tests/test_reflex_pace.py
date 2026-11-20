"""NVIDIA Reflex holds the Game frame rate cap without frame generation, with RX_VK_REFLEX_LIMIT=1 (no game, no GPU).

With Reflex on and no frame generation the game's frame could wait twice: the game's own wait where it takes a window
texture (host/nn_vi.c), and Reflex's sleep after the present (vk_reflex.cpp), which held no limit. NX2navi's low latency
(runtime/vk_lowlat.c) lets the driver hold the limit, the sleep's minimum interval, where the frame starts, and its pacer
steps aside. With the switch, vk_reflex.cpp reflex_pace does the same through dlssg_pace.h's rule, asked by vk_pace.cpp
frame_start where the game's frame starts (tests/test_rtss_limit.py runs that: one limiter a frame with RivaTuner and DLSS
frame generation, a movie the game's own wait's).

This cuts Reflex's state and reflex_pace, sleep_now, reflex_sleep, reflex_presented and apply_mode out of vk_reflex.cpp and
runs them with the real Vulkan headers against a fake driver (its sleep mode, its sleep ending no sooner than the minimum
interval after the last frame's start, its markers) and a fake clock, frame after frame as the game makes them: the frame's
start (reflex_pace, then the game's own wait where it answers false), its work, its present and the sleep after it. Without
the switch nothing changes: no interval, one sleep a frame after the present, the game's own wait each frame. With it the
first frame is the game's own wait's, the interval goes to the driver at the next sleep, and from the frame after that every
frame waits exactly once, in Reflex's sleep at its start, the sleep after the present left out, the frames a period apart. A
driver that ignores the interval gives the cap back to the game's own wait after a window of 60 frames, for the rest of the
run, and the interval is taken back. No period (a movie, RivaTuner) and no present for 200 ms give the frame to the game's
own wait at once; a driver that refuses an interval keeps the mode without one. Then it reads the sources: the switch is off
unless set, the sleep mode carries the interval, the row's change is logged as before, vk_pace.cpp asks it only without DLSS
frame generation, and the switch's row is in the development guide.

    python -I -B tests/test_reflex_pace.py [--cxx g++] --headers <Vulkan headers>
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


FIXTURE = r'''#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "dlssg_pace.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s (frame %d)\n", __LINE__, #x, frame_no); std::exit(1); } } while (0)
static int frame_no;
/* ---- the clock, the host, the device, the menu */
static int64_t clock_ns = 1000000000ll;
static int64_t now_ns() { return clock_ns; }
struct { int64_t (*now_ns)(); } host_v{now_ns}, *host = &host_v;
struct { VkDevice device = (VkDevice)(uintptr_t)1; } dev;
static int wanted = 2;       /* (the NVIDIA Reflex row: On + Boost) */
static bool fg = false;
static int wanted_mode() { return wanted; }
static bool framegen_on() { return fg; }
static void log_timings() {}
static std::vector<std::string> lines;
template <class... T> static void logf(const char* fmt, T... a) {
    char b[600];
    if constexpr (sizeof...(a) == 0) std::snprintf(b, sizeof b, "%s", fmt);
    else std::snprintf(b, sizeof b, fmt, a...);
    lines.push_back(b);
}
static bool logged(const char* part) {
    for (auto& l : lines) if (l.find(part) != std::string::npos) return true;
    return false;
}
/* ---- the fake driver: its sleep ends no sooner than the interval after the last frame's start (honours) */
static struct {
    bool honours = true, refuses = false;
    uint32_t interval_us = 0;
    int low = -1, boost = -1, modes = 0, sleeps = 0, markers = 0;
    int64_t last_start = 0, wake = 0;
} D;
static VkResult VKAPI_CALL set_mode(VkDevice, VkSwapchainKHR, const VkLatencySleepModeInfoNV* m) {
    if (D.refuses && m->minimumIntervalUs) return VK_ERROR_INITIALIZATION_FAILED;
    D.modes++, D.low = m->lowLatencyMode, D.boost = m->lowLatencyBoost, D.interval_us = m->minimumIntervalUs;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL sleep_fn(VkDevice, VkSwapchainKHR, const VkLatencySleepInfoNV*) {
    D.sleeps++;
    D.wake = D.honours && D.interval_us && D.last_start ? std::max(clock_ns, D.last_start + (int64_t)D.interval_us * 1000) : clock_ns;
    D.last_start = D.wake;
    return VK_SUCCESS;
}
static void VKAPI_CALL marker_fn(VkDevice, VkSwapchainKHR, const VkSetLatencyMarkerInfoNV*) { D.markers++; }
VkResult vkWaitSemaphores(VkDevice, const VkSemaphoreWaitInfo*, uint64_t) {
    clock_ns = std::max(clock_ns, D.wake);
    return VK_SUCCESS;
}
namespace lab_ab { inline bool on(const char*) { return false; } }   // ((Triforce) the lab's in-run A/B knobs: none here)
@CODE@
/* ---- the game: a frame's start (the host asks; its own wait where the answer is false), its work, its present */
static const int64_t P = 1000000000ll / 240;
static int64_t own_next = 0;       /* (the game's own wait: frames a period apart) */
static int host_waits, frames_held;
static int64_t starts[4096];
static bool frame(int64_t period, int64_t work, bool present = true) {
    frame_no++;
    const int sleeps0 = D.sleeps;
    const bool held = reflex_pace(period);
    if (!held) {
        host_waits++;
        clock_ns = std::max(clock_ns, own_next);
    }
    starts[frame_no % 4096] = clock_ns;
    own_next = clock_ns + P;
    const int at_start = D.sleeps - sleeps0;
    clock_ns += work;
    if (present) {
        reflex_presented();
        const int s = D.sleeps;
        reflex_sleep();
        CHECK(!(held && D.sleeps != s));        /* (the sleep after the present left out while the frame's start takes it) */
    }
    if (held) {
        frames_held++;
        CHECK(at_start <= 1);
    }
    return held;
}
static void reset_all() {
    Reflex& r = rx();
    r.ready = true, r.latency = true, r.swapchain = (VkSwapchainKHR)(uintptr_t)7, r.mode = -1, r.frame_marked = false;
    r.set_mode = set_mode, r.sleep = sleep_fn, r.marker = marker_fn, r.wake = (VkSemaphore)(uintptr_t)9;
}
int main(int argc, char** argv) {
    const std::string c = argc > 1 ? argv[1] : "";
    reset_all();
    if (c == "off") {   /* RX_VK_REFLEX_LIMIT unset: as before */
        for (int i = 0; i < 300; i++) {
            const int s = D.sleeps;
            CHECK(!frame(P, 1000000));
            CHECK(D.sleeps == s + 1);   /* (one sleep a frame, after the present) */
        }
        CHECK(host_waits == 300 && D.interval_us == 0 && D.modes == 1 && rx().want_us.load() == 0 && !rx().game_sleeps.load());
        CHECK(logged("vk: NVIDIA Reflex: on + boost") && !logged("frame limit"));
        return 0;
    }
    if (c == "holds") {
        CHECK(!frame(P, 1000000) && D.interval_us == 0);   /* (no present yet: the game's own wait, and no interval asked) */
        CHECK(!frame(P, 1000000));                        /* (asked now; the driver takes it at the sleep after this present) */
        CHECK(host_waits == 2 && D.interval_us == (P + 500) / 1000 && D.sleeps == 2);
        CHECK(frame(P, 1000000) && D.sleeps == 2);        /* (that sleep held this frame: no second one) */
        for (int i = 0; i < 500; i++) {
            const int s = D.sleeps;
            CHECK(frame(P, 1000000 + (i % 7) * 300000));
            CHECK(D.sleeps == s + 1);                              /* (one sleep a frame, where it starts) */
        }
        CHECK(host_waits == 2 && frames_held == 501 && logged("its sleep holds the frame rate cap where each frame starts (4.167 ms"));
        for (int k = frame_no - 400; k < frame_no; k++) {
            const int64_t d = starts[(k + 1) % 4096] - starts[k % 4096];
            CHECK(d >= P - 1000 && d <= P + 1000);                 /* (the starts a period apart, the driver's interval) */
        }
        /* a movie or RivaTuner: no period; the game's own wait at once, the interval taken back at the next sleep */
        const int waits = host_waits;
        CHECK(!frame(0, 1000000));
        CHECK(host_waits == waits + 1 && D.interval_us == 0);
        CHECK(!frame(0, 1000000) && D.interval_us == 0);
        CHECK(!frame(P, 1000000));   /* (the interval asked again: the driver takes it at the sleep after this present) */
        CHECK(frame(P, 1000000));
        /* a window that shows nothing for 200 ms: the game's own wait */
        for (int i = 0; i < 60; i++) frame(P, 1000000, false);
        clock_ns += 250000000;
        CHECK(!frame(P, 1000000, false));
        CHECK(logged("the game's own wait holds it (no present for 200 ms)"));
        CHECK(!frame(P, 1000000));   /* (presents again: the newest is still old where this frame starts) */
        CHECK(!frame(P, 1000000));   /* (the interval asked again, taken at the sleep after this present) */
        CHECK(frame(P, 1000000));
        /* the row off: not Reflex's */
        wanted = 0;
        CHECK(!frame(P, 1000000) && !frame(P, 1000000));
        return 0;
    }
    if (c == "ignores") {   /* a driver that does not hold the frames to the interval: the game's own wait for the run */
        D.honours = false;
        frame(P, 1000000);
        int held = 0;
        for (int i = 0; i < 200; i++) held += frame(P, 1000000);
        CHECK(held >= 59 && held <= 62);   /* (a window of 60) */
        CHECK(logged("its sleep does not hold the frame rate cap") && rx().pace.bad);
        CHECK(D.interval_us == 0 && !rx().game_sleeps.load());
        const int waits = host_waits;
        for (int i = 0; i < 100; i++) CHECK(!frame(P, 1000000));
        CHECK(host_waits == waits + 100);
        return 0;
    }
    if (c == "refused") {   /* a driver that refuses an interval: the mode without one, the game's own wait */
        D.refuses = true;
        for (int i = 0; i < 50; i++) CHECK(!frame(P, 1000000));
        CHECK(rx().interval_refused && D.interval_us == 0 && D.low == 1 && D.boost == 1 && logged("refused a frame limit"));
        CHECK(logged("the game's own wait holds it (the driver refused the frame limit)"));
        return 0;
    }
    std::fprintf(stderr, "no case %s\n", c.c_str());
    return 2;
}
'''


def check_source(reflex, pace, dev):
    check('getenv("RX_VK_REFLEX_LIMIT")' in block(reflex, "bool reflex_pace(int64_t period_ns) {") and
          "return e && atoi(e) != 0;" in block(reflex, "bool reflex_pace(int64_t period_ns) {"), "RX_VK_REFLEX_LIMIT is not off unless set")
    mode = block(reflex, "void apply_mode() {")
    check("m.minimumIntervalUs = us;" in mode and "r.want_us.load(std::memory_order_acquire)" in mode,
          "the sleep mode does not carry the interval asked for")
    check('if (want != r.mode)\n        logf("vk: NVIDIA Reflex: %s%s"' in mode, "the row's change is not logged as before")
    check("if (r.game_sleeps.load(std::memory_order_acquire)) return;" in block(reflex, "void reflex_sleep() {") and
          "if (sleep_now()) r.slept_after.store(true, std::memory_order_release);" in block(reflex, "void reflex_sleep() {"),
          "the sleep after the present is not left out while the frame's start takes it")
    check("r.interval_us = 0;" in block(reflex, "void reflex_swapchain_created(VkSwapchainKHR swapchain) {"),
          "a new swapchain is taken to have the old interval")
    start = block(pace, "int frame_start() {")
    check("(!dlssg::active() && reflex_pace(rtss || movie ? 0 : period))" in start,
          "vk_pace.cpp does not ask the renderer's own Reflex only without DLSS frame generation, with no period under RivaTuner or a movie")
    check("| `RX_VK_REFLEX_LIMIT=1`" in dev, "docs/development.md has no row for RX_VK_REFLEX_LIMIT")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--headers", required=True, type=Path)
    args = parser.parse_args()
    reflex = (ROOT / "gpu/vk/vk_reflex.cpp").read_text(encoding="utf-8")
    pace = (ROOT / "gpu/vk/vk_pace.cpp").read_text(encoding="utf-8")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check_source(reflex, pace, dev)
    code = block(reflex, "struct Reflex {") + ";\n"
    for marker in ("Reflex& rx() {", "bool on() {", "void mark(VkLatencyMarkerNV m) {", "void mark_shown(VkLatencyMarkerNV m) {",
                   "void apply_mode() {",
                   "void reflex_presented() {", "bool sleep_now() {", "void pace_say(int reflex, uint32_t us, const char* why) {",
                   "void reflex_sleep() {", "bool reflex_pace(int64_t period_ns) {"):
        code += block(reflex, marker) + "\n"
    headers = args.headers / "include" if (args.headers / "include" / "vulkan").is_dir() else args.headers
    with tempfile.TemporaryDirectory(prefix="reflex-pace-") as tmp:
        src, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture"
        src.write_text(FIXTURE.replace("@CODE@", code), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-missing-field-initializers",
                        "-I", str(ROOT / "gpu/vk"), "-I", str(headers), str(src), "-o", str(exe)], check=True)
        for case, env_on in (("off", False), ("holds", True), ("ignores", True), ("refused", True)):
            env = {k: v for k, v in os.environ.items() if k != "RX_VK_REFLEX_LIMIT"}
            if env_on:
                env["RX_VK_REFLEX_LIMIT"] = "1"
            done = subprocess.run([str(exe), case], env=env, capture_output=True, text=True)
            if done.returncode:
                raise SystemExit("FAIL: case %s\n%s%s" % (case, done.stdout, done.stderr))
    print("PASS: with RX_VK_REFLEX_LIMIT=1 Reflex's sleep holds the cap where each frame starts, once a frame, and gives it back")


if __name__ == "__main__":
    main()
