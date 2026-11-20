"""The present thread (gpu/vk/present_thread.h, vk_replay.cpp present_image / present_out, RX_VK_PRESENT_THREAD; no game,
no GPU).

A frame of OoT here was one chain on the game's presenting thread, the present's last step in it: the pacing's wait for an
early picture (up to 8 ms) and vkQueuePresentKHR held the start of the next frame. With the switch on (the default) the
presenting thread records and submits the blit as before and hands the rest to a thread of its own; the next present
waits first until the one handed over is out.

Compiles present_thread.h and runs it: the rule of which presents go to the thread (only the game's own, with no frame
generation, no NVIDIA DLSS frame generation and no AMD Anti-Lag 2, the switch on); the hand-off returns at once while the
thread is still busy with the present before, a settle waits until that one is out and says how long, two presents are
never in hand at once, and they go out in their order, each once; and on a model frame (a build and a present's last step
of 12 ms each) the frames come about as fast as the longer of the two instead of their sum. Then it reads the source for
what only the game can run: the blit, the overlays, the bug capture's and the frame log's copies and Reflex's submit
marker all recorded and submitted on the presenting thread before the hand-off (the same commands in the same order on
the queue: the same picture); present_image waits for the present before as its first step, under its lock, so the
swapchain is acquired, made again and presented by one thread at a time; the present thread makes no swapchain itself
(one out of date is noted as a resize for the next present); the queue's lock is let go before the hand-off and never
taken by the thread before the pacing's wait; NVIDIA Reflex's present markers carry the id taken at the submit's end;
the switch is on unless =0; the 120-present line and the stall line say what the presenting thread waited for; and the
switch's row is in the development guide.

    python -I -B tests/test_present_thread.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''#include "present_thread.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using clk = std::chrono::steady_clock;
static double ms_since(clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); }
static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

struct Job { int seq = -1; int tail_ms = 0; };

int main() {
    /* the rule: the game's own pictures only, the switch on, none of the presents or stages of their own */
    for (int m = 0; m < 32; m++) {
        const int on = m & 1, games = m >> 1 & 1, fg = m >> 2 & 1, dlssg = m >> 3 & 1, al = m >> 4 & 1;
        CHECK(present_thread_use(on, games, fg, dlssg, al) == (on && games && !fg && !dlssg && !al));
    }

    /* nothing in hand: settle does not wait */
    {
        present_thread::Handoff<Job> h;
        CHECK(h.settle() == -1);
    }

    /* the hand-off returns at once while the thread is busy; settle waits for it to be out, and says how long */
    {
        present_thread::Handoff<Job> h;
        std::atomic<bool> out{false};
        std::thread t([&] {
            const Job j = h.take();
            CHECK(j.seq == 7);
            sleep_ms(j.tail_ms);
            out = true;
            h.done();
        });
        const auto t0 = clk::now();
        h.hand({7, 60});
        CHECK(ms_since(t0) < 40 && !out);   /* (returned while the thread still had it) */
        const int64_t ns = h.settle();
        CHECK(out && ns > 0 && ns >= 20 * 1000000ll && ms_since(t0) >= 50);
        CHECK(h.settle() == -1 && h.handed == 1 && h.taken == 1);
        t.join();
    }

    /* many presents: in order, each once, never two in hand, the presenting thread going on between them */
    {
        present_thread::Handoff<Job> h;
        constexpr int N = 300;
        std::vector<int> seen;
        std::atomic<int> in_hand{0}, most{0};
        std::thread t([&] {
            for (int k = 0; k < N; k++) {
                const Job j = h.take();
                const int now = ++in_hand;
                if (now > most) most = now;
                seen.push_back(j.seq);
                if (k % 7 == 0) std::this_thread::yield();
                if (k % 50 == 0) sleep_ms(1);
                --in_hand;
                h.done();
            }
        });
        int64_t waited = 0, waits = 0;
        for (int k = 0; k < N; k++) {
            const int64_t ns = h.settle();
            if (ns >= 0) waited += ns, waits++;
            h.hand({k, 0});
            for (volatile int spin = 0; spin < (k % 3) * 2000; spin = spin + 1) {}   /* (the next frame's build) */
        }
        h.settle();
        t.join();
        CHECK((int)seen.size() == N && most == 1 && h.handed == N && h.taken == N);
        for (int k = 0; k < N; k++) CHECK(seen[k] == k);
        std::printf("%d presents in their order, one in hand at most; %lld settles waited (%.2f ms in all)\n", N, (long long)waits,
                    waited / 1e6);
    }

    /* the model frame: a build of 12 ms, then the present's last step of 12 ms (the pacing's wait, the present call) */
    {
        constexpr int F = 10, B = 12, P = 12;
        auto t0 = clk::now();
        for (int k = 0; k < F; k++) sleep_ms(B), sleep_ms(P);   /* (as before: both on the presenting thread) */
        const double serial = ms_since(t0);
        present_thread::Handoff<Job> h;
        std::thread t([&] {
            for (int k = 0; k < F; k++) {
                const Job j = h.take();
                sleep_ms(j.tail_ms);
                h.done();
            }
        });
        t0 = clk::now();
        for (int k = 0; k < F; k++) {
            sleep_ms(B);
            h.settle();
            h.hand({k, P});
        }
        h.settle();
        const double handed = ms_since(t0);
        t.join();
        std::printf("model frames (build %d ms, the present's last step %d ms): %.1f ms a frame on one thread, %.1f with the present "
                    "thread\n", B, P, serial / F, handed / F);
        CHECK(handed < serial * 0.8);
    }
    std::puts("ok");
    return 0;
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


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def before(text, *parts):
    at = [text.index(p) for p in parts]
    return at == sorted(at)


def check_source():
    vk = ROOT / "gpu/vk"
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    image = block(replay, "void present_image(VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImageView view) {")
    out = block(replay, "static void present_out(const PresentOut& o, std::unique_lock<std::mutex>& qk, bool on_thread) {")
    # the present before out first, under present_image's lock, before anything of the swapchain is read
    check(before(image, "std::lock_guard lk(present_image_mu);", "present_thread_settle();", "dev.swap_resize.exchange(0",
                 "create_swapchain(width, height", "vkAcquireNextImageKHR("),
          "present_image does not wait for the present before as its first step under its lock")
    # everything the picture is made of recorded and submitted on the presenting thread, before the hand-off
    check(before(image, "vkWaitForFences(dev.device, 1, &f.done", "vkAcquireNextImageKHR(", "blit_crop_to_rect(f.cmd",
                 "if (src && framelog_on())", "menu_record(f.cmd, index", "dlssg::frame(f.cmd", "vkEndCommandBuffer(f.cmd)",
                 "reflex_submit_ends();", "res = vkQueueSubmit2(dev.queue, 1, &si, f.done);", "const PresentOut out{index, smooth_mine",
                 "if (present_thread_wanted()) {", "qk.unlock();\n        return present_thread_hand(out);", "present_out(out, qk, false);"),
          "the blit, the overlays, the copies or Reflex's submit marker are not all on the queue before the hand-off")
    check(image.rstrip().endswith("present_out(out, qk, false);\n}"), "present_image does more after the present's last step")
    check("menu_record(" not in out and "vkQueueSubmit" not in out and "vkCmd" not in out and "f.cmd" not in out,
          "present_out records or submits GPU work: it may run on the present thread after the next frame's submits")
    # the last step: the pacing's wait first, then the present with its markers; on the thread no swapchain is made
    check(before(out, "smooth_present(qk);", "pi.pNext = reflex_present_id(nullptr);", "antilag_present();", "vkQueuePresentKHR(",
                 "reflex_presented();"), "present_out's steps are out of order")
    oo = out[out.index("if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {"):]
    check(before(oo, "if (on_thread) {", "dev.swap_resize.compare_exchange_strong(none,", "return;", "create_swapchain("),
          "the present thread makes the swapchain itself, or does not leave it to the next present as a resize")
    smooth = block(replay, "void smooth_present(std::unique_lock<std::mutex>& qk) {")
    check("if (qk.owns_lock()) qk.unlock();" in smooth and smooth.count("qk.unlock()") == 1,
          "the pacing's wait lets go of a lock the present thread never took")
    # the rule and the switch
    wanted = block(replay, "bool present_thread_wanted() {")
    check("present_thread_use(present_thread_on(), smooth_mine, framegen_on(), dlssg::active(), antilag_running())" in wanted,
          "the present thread is not limited to the game's pictures without frame generation, DLSS frame generation or Anti-Lag 2")
    on = block(replay, "bool present_thread_on() {")
    check('getenv("RX_VK_PRESENT_THREAD")' in on and "e && *e && atoi(e) == 1" in on, "RX_VK_PRESENT_THREAD is not off unless =1 (off by default since 11-16)")
    main = block(replay, "void present_thread_main() {")
    check(before(main, "present_hand.take();", "smooth_mine = o.smooth;", "std::unique_lock<std::mutex> qk(dev.present_mu, std::defer_lock);",
                 "present_out(o, qk, true);", "smooth_mine = false;", "present_hand.done();"),
          "the present thread does not run present_out as the presenting thread's, without the queue's lock, and then say so")
    # only present_image acquires and makes the swapchain in play, only present_out presents
    for f in sorted(vk.glob("*.cpp")):
        text = f.read_text(encoding="utf-8", errors="replace")
        if f.name != "vk_replay.cpp":
            check("vkQueuePresentKHR(" not in text and "vkAcquireNextImageKHR(" not in text, f.name + " presents or acquires itself")
        if f.name not in ("vk_replay.cpp", "vk_main.cpp", "vk_ffx_fg.cpp"):   # (linux fork: AMD's own swapchain, made from vk_main.cpp's)
            check("create_swapchain(" not in text, f.name + " makes the swapchain")
    check(replay.count("vkQueuePresentKHR(") == 2 and out.count("vkQueuePresentKHR(") == 2 and replay.count("vkAcquireNextImageKHR(") == 1,
          "vk_replay.cpp presents or acquires outside present_out and present_image")
    # NVIDIA Reflex: the present's id and markers are the frame's whose submit ended, not the next frame's
    reflex = (vk / "vk_reflex.cpp").read_text(encoding="utf-8")
    ends = block(reflex, "void reflex_submit_ends() {")
    check(before(ends, "r.shown_marked = false;", "if (!on()) return;", "r.shown_id = r.id, r.shown_marked = r.frame_marked;"),
          "reflex_submit_ends does not take the frame's id for its present")
    pid = block(reflex, "const void* reflex_present_id(const void* next) {")
    check("!r.shown_marked" in pid and "mark_shown(VK_LATENCY_MARKER_PRESENT_START_NV);" in pid and "&r.shown_id" in pid and "&r.id" not in pid,
          "the present's id and start marker are not the frame's taken at its submit's end")
    check("mark_shown(VK_LATENCY_MARKER_PRESENT_END_NV);" in block(reflex, "void reflex_presented() {"),
          "the present's end marker is not that frame's")
    check("i.presentID = r.shown_id" in block(reflex, "void mark_shown(VkLatencyMarkerNV m) {"), "mark_shown does not mark shown_id")
    present = block(replay, "void do_present(const nvn_tex* t, const int crop[4]) {")
    check("reflex_sleep(); antilag_input();" in block(present, "struct ReflexSleep {"), "Reflex's sleep is not after the present")
    # what the presenting thread waited for: the 120-present line and the stall line
    check("present_thread_line();" in block(replay, "void log_frame_stats() {"), "the 120-present lines have no present line")
    line = block(replay, "void present_thread_line() {")
    check("went out from the present thread" in line and "waiting for the present before" in line and "the present call" in line,
          "the present line does not say what went out from the thread, the presenting thread's waits and the present call")
    tick = block(replay, "void status_tick() {")
    check("f.present_wait = (present_wait - s_present_wait) / 1e6;" in tick, "the stall line does not name the wait for the present before")
    settle = block(replay, "void present_thread_settle() {")
    check("present_hand.settle()" in settle and "present_fig.wait_ns.fetch_add(ns" in settle, "the waits for the present before are not counted")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("| `RX_VK_PRESENT_THREAD=1` |" in dev, "docs/development.md has no row for RX_VK_PRESENT_THREAD")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="present-thread-") as tmp:
        src, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "present-thread"
        src.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(ROOT / "gpu/vk"), str(src),
                        "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], check=True, stdout=subprocess.PIPE, text=True, timeout=60).stdout
    print("\n".join(out.strip().splitlines()[:-1]))
    print("PASS: the game's presents handed to the present thread after their blit's submit, one in hand, in order; the next "
          "present waits for it first")


if __name__ == "__main__":
    main()
