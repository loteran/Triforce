"""The game's own frame wait and another limiter: one or the other holds each frame, never both and never neither.

The game's frames are paced where it takes a window texture (host/nn_vi.c hos_vi_native_acquire). Another limiter that
holds the same frames (NVIDIA Reflex with DLSS frame generation, gpu/vk/vk_dlssg.cpp pace) made a frame wait twice. The
host now asks the renderer where the frame starts (pace_handed: HOS_OPT_PACE_PERIOD, then HOS_OPT_PACE_START) and its own
wait steps aside only for a frame the renderer answers 1 for, the answer it gives once the other limiter's wait was taken.

This is host/nn_vi.c's own pace_handed and hos_vi_native_acquire, cut out and run against a clock, a sleep and a renderer
that are stand-ins: every frame waits exactly once, in the host or in the renderer's call; the host's wait is what it was
whenever the answer is 0 (and with no renderer, and for an answer that is not 1); the renderer is asked once a frame, with
the period the host paces by, never before the first present, never with HOS_NO_PACING, never with HOS_PACE_HANDOFF=0. The
host's wait goes to its time through hos_sleep_until_ns (the timer and the yielding: tests/test_pace_wait.py), and with
HOS_PACE_SPIN=0 through hos_sleep_ns as before, both to the same time. Each case is a process of its own: the host reads its
switches once.

    python -I -B tests/test_pace_handoff.py [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
CASES = ("own", "handed", "mixed", "let-go", "no-renderer", "odd-answer", "twice", "first", "double", "no-pacing", "switch-off", "movie",
         "spin-off")


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


FIXTURE = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef long long LONG64;
typedef long LONG;
@IDS@
/* ---- stand-ins: the clock, the sleep, the renderer ---- */
static int64_t clock_ns = 5000000000LL, host_slept;
static int host_sleeps, other_waits, asks, sets, set_value, set_before_ask, answer, renderer = 1;
static int movie_told = -1, movie_sets;   /* (whether a movie plays, as the renderer is told before each ask) */
/* (what the other limiter's wait takes, inside the renderer's call: with a frame's 3 ms of work, the period of a cap of 60) */
static int64_t other_wait = 1000000000LL / 60 - 3000000;
int64_t hos_now_ns(void) { return clock_ns; }
void hos_sleep_ns(int64_t ns) { CHECK(ns > 0); host_sleeps++, host_slept += ns, clock_ns += ns; }
/* (the wait to its time, its timer and its yielding together: host/hos_thread.c hos_sleep_until_ns) */
static int until_sleeps;
static int64_t margin_seen;
void hos_sleep_until_ns(int64_t until, int64_t margin) {
    CHECK(until > clock_ns && margin > 0);
    until_sleeps++, host_sleeps++, host_slept += until - clock_ns, clock_ns = until, margin_seen = margin;
}
static void hos_log(const char *fmt, ...) { (void)fmt; }
int rx_option(int id, int otherwise) {
    CHECK(id == HOS_OPT_PACE_START);
    asks++;
    CHECK(set_before_ask == 1 && movie_sets == 1);   /* (the period and the movie are told right before each ask) */
    set_before_ask = movie_sets = 0;
    if (!renderer) return otherwise;
    if (answer == 1) other_waits++, clock_ns += other_wait;   /* (Reflex's sleep, taken inside the call) */
    return answer;
}
void rx_option_set(int id, int value) {
    if (id == HOS_OPT_PACE_MOVIE) { movie_told = value, movie_sets++; return; }
    CHECK(id == HOS_OPT_PACE_PERIOD);
    sets++, set_value = value, set_before_ask++;
}
/* ---- host/nn_vi.c's state these two functions use ---- */
static volatile LONG64 presents;
static int64_t next_frame_at;
static int64_t shown_at[16];
static volatile LONG64 shown_count;
@CODE@
/* a present, as hos_vi_native_presented paces by it (the display queue's times: test_fps_cap_monitor.py reads that code) */
static const int64_t PERIOD = 1000000000LL / 60;
static void present(void) {
    const int64_t now = clock_ns;
    pace_period_ns = PERIOD;
    next_frame_at = next_frame_at && now - next_frame_at < PERIOD ? next_frame_at + PERIOD : now + PERIOD;
    const int64_t n = shown_count, last = n ? shown_at[(n - 1) & 15] : 0;
    shown_at[n & 15] = last + PERIOD > now ? last + PERIOD : now;
    shown_count = n + 1;
    presents++;
}
/* a frame of 3 ms of work between the acquire and the present; returns the host's sleep in it */
static int64_t frame(int active) {
    const int64_t slept = host_slept;
    hos_vi_native_acquire(active);
    clock_ns += 3000000;
    present();
    return host_slept - slept;
}
int main(int argc, char **argv) {
    const char *c = argc > 1 ? argv[1] : "";
    if (!strcmp(c, "first")) {   /* before the first present: no wait, and the renderer is not asked */
        hos_vi_native_acquire(3);
        hos_vi_native_acquire(2);
        CHECK(!asks && !sets && !host_sleeps && !other_waits);
        return 0;
    }
    if (!strcmp(c, "own") || !strcmp(c, "no-renderer") || !strcmp(c, "odd-answer") || !strcmp(c, "spin-off")) {
        /* the answer 0 (the row off), no renderer to ask, or an answer that is not 1: the host's own wait, each frame; to its
           time with the timer and the yielding (PACE_WAIT_MARGIN_NS), and with HOS_PACE_SPIN=0 on the timer alone, as before */
        renderer = strcmp(c, "no-renderer") != 0;
        answer = !strcmp(c, "odd-answer") ? 2 : 0;
        frame(1);   /* (the first present: nothing to wait for before it) */
        for (int i = 0; i < 200; i++) {
            if (!strcmp(c, "odd-answer")) answer = i % 3 == 0 ? 2 : i % 3 == 1 ? -1 : 100;
            const int64_t due = next_frame_at, before = clock_ns;
            frame_waited_other_ns = 0;
            const int64_t slept = frame(1);
            CHECK(slept == due - before && slept > 0);   /* (exactly the wait there was before the hand-off: to the frame's time) */
            CHECK(frame_waited_other_ns == slept && !frame_handed);   /* ((Triforce) counted out of what the frame took: steady_period; no present here, so not the presenting thread) */
        }
        CHECK(asks == 200 && sets == 200 && host_sleeps == 200 && !other_waits && set_value == (int)PERIOD);
        if (!strcmp(c, "spin-off")) CHECK(!until_sleeps);
        else CHECK(until_sleeps == 200 && margin_seen == PACE_WAIT_MARGIN_NS);
        return 0;
    }
    if (!strcmp(c, "handed")) {   /* the answer 1: the other limiter's wait, taken inside the call, and none of the host's */
        answer = 1;
        frame(3);
        for (int i = 0; i < 200; i++) CHECK(frame(3) == 0);
        CHECK(asks == 200 && other_waits == 200 && !host_sleeps && set_value == (int)PERIOD);
        CHECK(frame_handed && !frame_waited_other_ns && !frame_waited_own_ns);   /* ((Triforce) noted for steady_period: not this file's wait's frames) */
        return 0;
    }
    if (!strcmp(c, "mixed")) {   /* the answer changes from frame to frame: one wait a frame, the host's or the other's */
        frame(1);
        unsigned seed = 12345;
        for (int i = 0; i < 2000; i++) {
            seed = seed * 1103515245u + 12345u;
            answer = (seed >> 16) & 1;
            const int sleeps = host_sleeps, others = other_waits;
            const int64_t due = next_frame_at, before = clock_ns;
            const int64_t slept = frame(1);
            CHECK((host_sleeps - sleeps) + (other_waits - others) == 1);   /* (never both, never neither) */
            CHECK(answer ? slept == 0 : slept == due - before);            /* (the moment the answer is 0 the host's wait is back) */
        }
        CHECK(asks == 2000);
        return 0;
    }
    if (!strcmp(c, "let-go")) {
        /* a limiter that answers 1 and holds nothing (1 ms a frame, for 300 frames): each present still put the display queue a
           period on, 3.8 s ahead of the clock by the end. The frame the host takes back waits a period at most, not those
           seconds, and the frames after it wait as they always did: 3 ms of work short of the period */
        for (int active = 1; active <= 3; active += 2) {
            answer = 1, other_wait = 1000000;
            for (int i = 0; i < 300; i++) frame(active);
            CHECK(next_frame_at - clock_ns > 1000000000LL);   /* (the freeze there would be) */
            answer = 0;
            const int sleeps = host_sleeps;
            const int64_t first = frame(active);
            CHECK(first > 0 && first <= PERIOD && host_sleeps == sleeps + 1);
            for (int i = 0; i < 20; i++) {
                const int64_t slept = frame(active);
                CHECK(slept > 0 && slept <= PERIOD);
                if (i >= 4) CHECK(slept == PERIOD - 3000000);   /* (the queue's own times again) */
            }
        }
        return 0;
    }
    if (!strcmp(c, "movie")) {
        /* a movie player's update says a movie plays; the renderer is told so with the next frame's ask, and no longer a
           quarter second after the last update */
        frame(1);
        frame(1);
        CHECK(movie_told == 0);
        movie_seen();
        frame(1);
        CHECK(movie_told == 1);
        for (int i = 0; i < 13; i++) frame(1);   /* (13 frames of a 60th and the time left of the quarter second) */
        CHECK(movie_told == 1);
        for (int i = 0; i < 4; i++) frame(1);
        CHECK(movie_told == 0);
        return 0;
    }
    if (!strcmp(c, "twice")) {   /* the game acquires again before it presents: asked once a frame, the frame's answer stays */
        frame(1);
        answer = 1;
        hos_vi_native_acquire(1);
        hos_vi_native_acquire(1);
        hos_vi_native_acquire(1);
        CHECK(asks == 1 && other_waits == 1 && !host_sleeps);
        present();
        answer = 0;
        hos_vi_native_acquire(1);
        CHECK(asks == 2 && host_sleeps == 1);
        const int64_t at = clock_ns;
        hos_vi_native_acquire(1);   /* (the frame's time has come: nothing more to wait for, and nothing asked) */
        CHECK(asks == 2 && host_sleeps == 1 && clock_ns == at);
        return 0;
    }
    if (!strcmp(c, "double")) {   /* HOS_VI_DOUBLE=1: the two-texture wait, to the last present's interval */
        answer = 0;
        frame(3);
        const int64_t due = next_frame_at, before = clock_ns;
        CHECK(frame(3) == due - before);
        answer = 1;
        CHECK(frame(3) == 0 && other_waits == 1);
        return 0;
    }
    if (!strcmp(c, "no-pacing") || !strcmp(c, "switch-off")) {
        /* HOS_NO_PACING: no wait at all and nothing asked; HOS_PACE_HANDOFF=0: the host's own wait, the renderer never asked */
        answer = 1;
        frame(1);
        for (int i = 0; i < 50; i++) {
            const int64_t due = next_frame_at, before = clock_ns;
            const int64_t slept = frame(1);
            CHECK(!strcmp(c, "no-pacing") ? slept == 0 : slept == due - before);
        }
        CHECK(!asks && !sets && !other_waits);
        return 0;
    }
    fprintf(stderr, "no case %s\n", c);
    return 2;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    vi = (ROOT / "host/nn_vi.c").read_text(encoding="utf-8")
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    # ((Triforce) and HOS_OPT_PACE_STEADY after them, the Steady frame rate row's: tests/test_pace_steady.py)
    ids = re.search(r"enum \{ HOS_OPT_PACE_PERIOD = (\d+), HOS_OPT_PACE_START, HOS_OPT_PACE_MOVIE(?:, HOS_OPT_PACE_STEADY)? \};", api)
    if not ids or int(ids.group(1)) != 85:
        raise SystemExit("FAIL: gpu/vk/vk_api.h no longer has pacing's option ids from 85")
    handed = vi[vi.index("static int64_t pace_period_ns;"):vi.index("void hos_vi_native_acquire(int active) {")]
    if "movie_seen();" not in block(vi, "uint32_t hos_movie_frames(uint64_t player) {")[:200]:
        raise SystemExit("FAIL: a movie player's update no longer tells the frame pacing that a movie plays")
    acquire = block(vi, "void hos_vi_native_acquire(int active) {")
    # (the period the renderer is told is the one the presents are paced by)
    presented = block(vi, "void hos_vi_native_presented(void) {")
    if "pace_period_ns = period;" not in presented:
        raise SystemExit("FAIL: hos_vi_native_presented no longer keeps the period it paces by for pace_handed")
    # (asked after the checks that there is a wait to give up, and before the wait itself)
    order = [acquire.index(s) for s in ("if (no_pacing || !next_frame_at) return;", "if (pace_handed()) { handed_before = 1; return; }",
                                         "pace_sleep(until, now);")]
    if order != sorted(order):
        raise SystemExit("FAIL: hos_vi_native_acquire asks the renderer before it knows it paces, or after its own wait")
    sleep = block(vi, "static void pace_sleep(int64_t until, int64_t now) {")
    if "if (margin) hos_sleep_until_ns(until, margin);" not in sleep or "else hos_sleep_ns(until - now);" not in sleep:
        raise SystemExit("FAIL: the acquire's wait no longer goes to its time, or HOS_PACE_SPIN=0 no longer gives the old wait")
    source = '#include "pace_wait.h"\n' + FIXTURE.replace("@IDS@", ids.group(0)).replace("@CODE@", handed + acquire)
    with tempfile.TemporaryDirectory(prefix="pace-handoff-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / "fixture"
        c.write_text(source, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu17", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-I", str(ROOT / "host"), str(c),
                        "-o", str(exe)], check=True)
        for case in CASES:
            env = {k: v for k, v in os.environ.items() if k not in ("HOS_NO_PACING", "HOS_VI_DOUBLE", "HOS_PACE_HANDOFF", "HOS_PACE_SPIN")}
            if case == "no-pacing":
                env["HOS_NO_PACING"] = "1"
            if case == "switch-off":
                env["HOS_PACE_HANDOFF"] = "0"
            if case == "double":
                env["HOS_VI_DOUBLE"] = "1"
            if case == "spin-off":
                env["HOS_PACE_SPIN"] = "0"
            done = subprocess.run([str(exe), case], env=env, capture_output=True, text=True)
            if done.returncode:
                raise SystemExit("FAIL: case %s\n%s%s" % (case, done.stdout, done.stderr))
    print("PASS: each frame waits once, in the host or in the limiter the renderer answers for (%d cases)" % len(CASES))


if __name__ == "__main__":
    main()
