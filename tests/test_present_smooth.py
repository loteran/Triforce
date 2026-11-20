"""Frame pacing on the plain present: no picture sooner than seven eighths of the recent interval (no game, no GPU).

The Insert menu's Frame pacing row only spaced frame generation's frames (vk_framegen.cpp display_main); without frame
generation each picture went out the moment the replay finished it. With RX_VK_PACE_SMOOTH=1 and the row on, a picture of
the game's that comes early now waits for seven eighths of the recent interval after the last one (gpu/vk/present_smooth.h,
vk_replay.cpp present_image, after NX2navi's pace_smooth).

Compiles gpu/vk/present_smooth.h as C and as C++ and runs it on frame times of its own: frames handed over 13 and 31 ms apart
in turn (NX2navi's village, mean 22 ms) and a 240 cap's frames whose work swings by up to 2 ms. Where the frames come at their
own times, as on NX2navi's present thread, the spread of the intervals narrows (no interval under 7/8 of the recent one once
it has settled) and the rate stays what it was. Where each wait also holds the next frame (the present on the game's
presenting thread, as here), the 13/31 frames come slower: the test measures by how much, which is why the switch is off by
default. A late picture never waits, a load or a hitch is not the rate, no wait is longer than 50 ms, and a reset starts
afresh. Then it reads vk_replay.cpp: the wait comes after the blit's submit and before the present's Reflex markers, with
the queue's lock let go and taken again where the present needs it, only for the game's own pictures, with the switch and
the row on and neither frame generation running; and the switch's row is in the development guide.

    python -I -B tests/test_present_smooth.py [--cc gcc] [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "present_smooth.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
#define MS 1000000ll
#define N 400

/* frames ready at ready[k] (decoupled: the next one comes at its own time whatever this one waits for); shown[] the times
   they went out, with the floor (on) or as they came */
static void run_decoupled(const int64_t* ready, int64_t* shown, int on) {
    PresentSmooth s = {0, 0};
    for (int k = 0; k < N; k++) {
        const int64_t at = on ? present_smooth_at(&s, ready[k]) : ready[k];
        CHECK(at >= ready[k] && at - ready[k] <= PRESENT_SMOOTH_MAX_NS);
        if (on && s.last_ns && s.avg_ns > 0 && at > ready[k])   /* (toward the floor, at most 8 ms and half the interval) */
            CHECK(at <= s.last_ns + s.avg_ns / 8 * 7 && at - ready[k] <= s.avg_ns / 2);
        shown[k] = at;
        present_smooth_shown(&s, at);
    }
}
/* the next frame's work starts when this one went out (coupled: the wait holds the presenting thread) */
static void run_coupled(const int64_t* work, int64_t* shown, int on) {
    PresentSmooth s = {0, 0};
    int64_t t = 1000 * MS;
    for (int k = 0; k < N; k++) {
        const int64_t ready = t + work[k], at = on ? present_smooth_at(&s, ready) : ready;
        shown[k] = at, t = at;
        present_smooth_shown(&s, at);
    }
}
static void spread(const int64_t* shown, int from, int64_t* lo, int64_t* hi, double* mean) {
    *lo = INT64_MAX, *hi = 0;
    for (int k = from + 1; k < N; k++) {
        const int64_t d = shown[k] - shown[k - 1];
        if (d < *lo) *lo = d;
        if (d > *hi) *hi = d;
    }
    *mean = (double)(shown[N - 1] - shown[from]) / (N - 1 - from);
}
int main(void) {
    static int64_t ready[N], work[N], plain[N], smooth[N];
    int64_t lo0, hi0, lo1, hi1;
    double m0, m1;
    /* the village: 13 and 31 ms in turn, around 22 */
    ready[0] = 1000 * MS;
    for (int k = 1; k < N; k++) ready[k] = ready[k - 1] + (k % 2 ? 13 : 31) * MS;
    run_decoupled(ready, plain, 0);
    run_decoupled(ready, smooth, 1);
    spread(plain, 40, &lo0, &hi0, &m0);
    spread(smooth, 40, &lo1, &hi1, &m1);
    CHECK(lo0 == 13 * MS && hi0 == 31 * MS);
    CHECK(lo1 >= 19 * MS && hi1 <= 25 * MS);           /* (13 to 31 narrowed to about 20 to 24) */
    printf("village 13/31 ms: intervals %.1f to %.1f ms as they come, %.1f to %.1f with the floor; %.2f and %.2f ms a frame\n",
           lo0 / 1e6, hi0 / 1e6, lo1 / 1e6, hi1 / 1e6, m0 / MS, m1 / MS);
    CHECK(m1 > m0 - 0.05 * MS && m1 < m0 + 0.05 * MS);   /* (the rate as it was, but for the last frame's wait) */
    /* a 240 cap's frames: starts on the period, work swinging by up to 2 ms */
    const int64_t P = 1000000000ll / 240;
    uint32_t seed = 3;
    for (int k = 0; k < N; k++) {
        seed = seed * 1103515245u + 12345u;
        ready[k] = 1000 * MS + k * P + 500000 + (int64_t)((seed >> 8) % 2000000);
    }
    run_decoupled(ready, plain, 0);
    run_decoupled(ready, smooth, 1);
    spread(plain, 40, &lo0, &hi0, &m0);
    spread(smooth, 40, &lo1, &hi1, &m1);
    CHECK(lo0 < P / 4 * 3);                               /* (as they come: a short one after a long one) */
    CHECK(lo1 >= P / 8 * 7 - P / 20);                     /* (none sooner than about seven eighths of the period) */
    CHECK(hi1 <= hi0 && m1 > m0 - 0.05 * MS && m1 < m0 + 0.05 * MS);
    /* the same 13/31 work where each wait holds the next frame: slower, by how much is printed (the switch's reason to be off) */
    for (int k = 0; k < N; k++) work[k] = (k % 2 ? 13 : 31) * MS;
    run_coupled(work, plain, 0);
    run_coupled(work, smooth, 1);
    spread(plain, 40, &lo0, &hi0, &m0);
    spread(smooth, 40, &lo1, &hi1, &m1);
    CHECK(m1 > m0 && hi1 - lo1 < hi0 - lo0);
    printf("coupled 13/31 ms: %.1f ms a frame as they come, %.1f ms with the floor (%+.0f%%)\n", m0 / MS, m1 / MS, (m1 / m0 - 1) * 100);
    /* a late picture never waits; nothing waits before the second picture */
    PresentSmooth s = {0, 0};
    CHECK(present_smooth_at(&s, 5 * MS) == 5 * MS);
    present_smooth_shown(&s, 5 * MS);
    CHECK(present_smooth_at(&s, 6 * MS) == 6 * MS && s.avg_ns == 0);
    present_smooth_shown(&s, 21 * MS);   /* (16 ms) */
    CHECK(s.avg_ns == 16 * MS);
    CHECK(present_smooth_at(&s, 50 * MS) == 50 * MS);                      /* (late) */
    CHECK(present_smooth_at(&s, 22 * MS) == 22 * MS + 8 * MS);             /* (early: toward 7/8 of 16 after the last, 8 ms at most) */
    CHECK(present_smooth_at(&s, 31 * MS) == 21 * MS + 14 * MS);            /* (4 ms from the floor: to it) */
    /* a hitch is not the rate: 100 ms or more, or more than three times the recent interval */
    present_smooth_shown(&s, 21 * MS + 150 * MS);
    CHECK(s.avg_ns == 16 * MS && s.last_ns == 171 * MS);
    present_smooth_shown(&s, 171 * MS + 60 * MS);   /* (60 ms against 16) */
    CHECK(s.avg_ns == 16 * MS && s.last_ns == 231 * MS);
    present_smooth_shown(&s, 231 * MS + 30 * MS);   /* (30 ms: under three times 16, the rate) */
    CHECK(s.avg_ns == 16 * MS + (24 - 16) * MS / 4);   /* (30 ms counted as 1.5 x 16 = 24) */
    /* never more than 8 ms, nor half the interval: test16 held one 41 ms at the start (frames 60 to 70 ms apart) */
    PresentSmooth big = {90 * MS, 100 * MS};
    CHECK(present_smooth_at(&big, 101 * MS) == 109 * MS);   /* (the floor 78.75 ms away: 8 ms toward it) */
    CHECK(present_smooth_at(&big, 140 * MS) == 148 * MS);
    CHECK(present_smooth_at(&big, 175 * MS) == 100 * MS + 90 * MS / 8 * 7);   /* (3.75 ms away: to it) */
    PresentSmooth start = {0, 0};
    int64_t at0 = 0, most = 0;
    for (int k = 0; k < 40; k++) {   /* (a start's frames, 60 to 70 ms apart, every other one early by 20 ms) */
        const int64_t ready = at0 + (k % 2 ? 45 : 70) * MS, at = present_smooth_at(&start, ready);
        if (at - ready > most) most = at - ready;
        present_smooth_shown(&start, at), at0 = at;
    }
    CHECK(most <= PRESENT_SMOOTH_MAX_NS && most > 0);
    /* 8 ms against 6 a period, no more than half of it */
    PresentSmooth fast = {6 * MS, 100 * MS};
    CHECK(present_smooth_at(&fast, 100 * MS) == 103 * MS);   /* (the floor 5.25 ms away: 3 ms, half the interval) */
    /* a reset starts afresh */
    present_smooth_reset(&s);
    CHECK(!s.avg_ns && !s.last_ns && present_smooth_at(&s, 172 * MS) == 172 * MS);
    PresentSmooth b = {0, 0};
    int64_t tb = 1000 * MS;
    for (int k = 0; k < 400; k++) { tb += (k % 8 == 3 ? 22 * MS : 10400000); present_smooth_shown(&b, tb); }
    CHECK(b.avg_ns >= 10400000 && b.avg_ns <= 11400000);   /* (counted whole the average went to ~11.9 and the floor held the steady frames) */
    puts("ok");
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


def check_source():
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    wanted = block(replay, "bool smooth_wanted() {")
    check('getenv("RX_VK_PACE_SMOOTH")' in wanted and "!(e && *e && atoi(e) == 0)" in wanted, "RX_VK_PACE_SMOOTH is not on unless =0")
    # ((Triforce) or the Steady frame rate row's hold to its period, whatever the row and the switch say: tests/test_pace_steady.py)
    check("smooth_mine && !framegen_on() && !dlssg::active() && ((on && opts.pace != 0) || pace::steady_floor_ns() > 0)" in wanted,
          "the floor is not limited to the game's own pictures with the Frame pacing row on and no frame generation")
    sp = block(replay, "void smooth_present(std::unique_lock<std::mutex>& qk) {")
    check("present_smooth_reset(&smooth)" in sp[:sp.index("const int64_t now")], "a floor switched off does not start afresh")
    check(sp.index("if (qk.owns_lock()) qk.unlock();") < sp.index("smooth_wait_until(at);"), "the picture waits with the queue's lock held")
    check("present_smooth_shown(&smooth, host->now_ns());" in sp, "the pictures' times are not noted")
    image = block(replay, "void present_image(VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImageView view) {")
    # ((Triforce) the present's last step is present_out, after the blit's submit: on this thread, or on the present thread
    # (RX_VK_PRESENT_THREAD, tests/test_present_thread.py), where the queue's lock was never taken)
    order = [image.index(s) for s in ("res = vkQueueSubmit2(dev.queue, 1, &si, f.done);", "return present_thread_hand(out);",
                                      "present_out(out, qk, false);")]
    check(order == sorted(order), "the present's last step does not come after the blit's submit")
    out = block(replay, "static void present_out(const PresentOut& o, std::unique_lock<std::mutex>& qk, bool on_thread) {")
    order = [out.index(s) for s in ("smooth_present(qk);", "pi.pNext = reflex_present_id(nullptr);", "antilag_present();")]
    check(order == sorted(order), "the wait is not before the present's markers")
    check("if (qk.owns_lock()) qk.unlock();" in out and "if (!qk.owns_lock()) qk.lock();\n        res = vkQueuePresentKHR(dev.queue, &pi);" in out,
          "the present does not take the queue's lock as it may have been let go")
    present = block(replay, "void do_present(const nvn_tex* t, const int crop[4]) {")
    check(present.count("smooth_mine = true;") == 2 and present.count("smooth_mine = false;") == 2,
          "do_present does not mark both of its presents as the game's")
    tail = present[present.rindex("smooth_mine = true;"):]
    check(tail.index("present_image(src, sw, sh, crop, direct_view);") < tail.index("smooth_mine = false;"), "the mark is not around the present")
    check("thread_local bool smooth_mine = false;" in replay, "the mark is not the presenting thread's own")
    check("| `RX_VK_PACE_SMOOTH=0`" in (ROOT / "docs/development.md").read_text(encoding="utf-8"),
          "docs/development.md has no row for RX_VK_PACE_SMOOTH")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="present-smooth-") as tmp:
        src = Path(tmp) / "fixture.c"
        src.write_text(FIXTURE, encoding="utf-8")
        for compiler, std, name in ((args.cc, "-std=gnu11", "c"), (args.cxx, "-std=c++20", "cxx")):
            exe = Path(tmp) / ("present-smooth-" + name)
            lang = ["-x", "c++"] if name == "cxx" else []
            subprocess.run([compiler, std, "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), *lang, str(src),
                            "-o", str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, stdout=subprocess.PIPE, text=True).stdout
    print("\n".join(out.strip().splitlines()[:-1]))
    print("PASS: no picture of the game's sooner than seven eighths of the recent interval, the rate kept where frames come at their own times")


if __name__ == "__main__":
    main()
