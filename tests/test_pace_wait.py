"""The game's frame wait ends on its time (host/pace_wait.h, host/hos_thread.c hos_sleep_until_ns, host/nn_vi.c pace_sleep).

The acquire's wait (hos_vi_native_acquire) slept on the high-resolution waitable timer for all of it, and that timer wakes
up to about 0.5 ms late: at a Frame rate cap of 240 an eighth of a frame, one frame long and, the pacing keeping its phase,
the next one short. It now takes the timer up to a margin before the frame's time and yields the rest away on the clock.

This compiles host/pace_wait.h as it is and runs its wait against a clock, a timer and a yield of its own: a timer that
never wakes early and wakes late by anything from nothing to half a millisecond, and a yield that moves the clock on by a
few microseconds. For every wait from 0.2 to 33 ms and every lateness up to the margin's: the wait ends on its time (within
one yield), the timer is never asked past the margin before it nor for less than its floor of a millisecond, and the time
spent yielding is at most the margin (the margin and the timer's floor where the whole wait is shorter than both). A timer
later than the margin ends the wait then, never later than the timer alone would have; a frame that is late waits for
nothing. Then it reads the sources: hos_sleep_until_ns takes this wait with the core given up once, the acquire's wait goes
through it with the margin unless HOS_PACE_SPIN=0 (the timer alone, as before), and the switch is in the development
guide's table.

    python -I -B tests/test_pace_wait.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "pace_wait.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s (wait %lld, late %lld)\n", __LINE__, #x, (long long)wait_ns, \
                                          (long long)late_ns); exit(1); } } while (0)
static int64_t clock_ns, late_ns, until_ns, margin_ns = PACE_WAIT_MARGIN_NS, wait_ns, yielded, timers;
static const int64_t STEP = 2000;   /* (a yield: the clock a few microseconds on) */
static int64_t now(void) { return clock_ns; }
static void timer(int64_t ns) {
    CHECK(ns >= PACE_WAIT_TIMER_MIN_NS);                  /* (never a wait the timer does not take) */
    CHECK(clock_ns + ns <= until_ns - margin_ns);         /* (never past the margin before the frame's time) */
    timers++;
    clock_ns += ns + late_ns;                             /* (never early, late by late_ns) */
}
static void yield(void) { yielded += STEP, clock_ns += STEP; }
static void run(int64_t wait, int64_t late) {
    wait_ns = wait, late_ns = late, clock_ns = 1000000000LL, until_ns = clock_ns + wait, yielded = 0, timers = 0;
    pace_wait_until(until_ns, margin_ns, now, timer, yield);
}
int main(void) {
    static const int64_t waits[] = {200000, 599999, 600000, 999999, 1000000, 1500000, 1599999, 1600000, 1600001, 2000000,
                                    4166667, 6944444, 8333333, 16666667, 33333333};
    static const int64_t lates[] = {0, 1, 50000, 200000, 400000, 499999, 500000, 599999};
    for (unsigned w = 0; w < sizeof waits / sizeof *waits; w++)
        for (unsigned l = 0; l < sizeof lates / sizeof *lates; l++) {
            run(waits[w], lates[l]);
            CHECK(clock_ns >= until_ns && clock_ns < until_ns + STEP);   /* (on time, within one yield) */
            if (waits[w] >= margin_ns + PACE_WAIT_TIMER_MIN_NS) {
                CHECK(timers == 1);
                CHECK(yielded <= margin_ns - lates[l] + STEP);          /* (only the margin the timer left) */
            } else {
                CHECK(timers == 0);                                     /* (too short for the timer: yielded) */
                CHECK(yielded <= margin_ns + PACE_WAIT_TIMER_MIN_NS);
            }
        }
    /* a timer later than the margin (a busy PC): the wait ends when it wakes, which is never later than the timer for the
       whole wait would have woken (late_ns after the frame's time), and nothing is yielded after it */
    for (int64_t late = 600001; late <= 5000000; late += 300000) {
        run(4166667, late);
        CHECK(clock_ns == until_ns - margin_ns + late && clock_ns < until_ns + late && yielded == 0 && timers == 1);
    }
    /* late already: nothing to wait for */
    wait_ns = 0, clock_ns = 2000000000LL, until_ns = clock_ns - 1000, yielded = timers = 0;
    pace_wait_until(until_ns, margin_ns, now, timer, yield);
    CHECK(clock_ns == 2000000000LL && !yielded && !timers);
    /* a thousand waits of a 240 cap's frames less some work, the timer late by anything up to the margin's lateness: every
       one ends within a yield of its time, and the yielding stays under the margin on average */
    uint32_t seed = 7;
    int64_t yield_sum = 0;
    for (int i = 0; i < 1000; i++) {
        seed = seed * 1103515245u + 12345u;
        const int64_t work = (seed >> 8) % 3000000;
        seed = seed * 1103515245u + 12345u;
        run(4166667 - work, (seed >> 8) % 500001);
        CHECK(clock_ns >= until_ns && clock_ns < until_ns + STEP);
        yield_sum += yielded;
    }
    CHECK(yield_sum / 1000 <= margin_ns);
    /* the rule alone: all but the margin where that is a millisecond or more, else nothing; a negative margin is none */
    CHECK(pace_wait_timed(4166667, PACE_WAIT_MARGIN_NS) == 4166667 - PACE_WAIT_MARGIN_NS);
    CHECK(pace_wait_timed(PACE_WAIT_MARGIN_NS + PACE_WAIT_TIMER_MIN_NS, PACE_WAIT_MARGIN_NS) == PACE_WAIT_TIMER_MIN_NS);
    CHECK(pace_wait_timed(PACE_WAIT_MARGIN_NS + PACE_WAIT_TIMER_MIN_NS - 1, PACE_WAIT_MARGIN_NS) == 0);
    CHECK(pace_wait_timed(5000000, -5) == 5000000 && pace_wait_timed(0, 0) == 0 && pace_wait_timed(-100, 0) == 0);
    CHECK(PACE_WAIT_MARGIN_NS == 600000);
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


def check_sources():
    thread = (ROOT / "host/hos_thread.c").read_text(encoding="utf-8")
    vi = (ROOT / "host/nn_vi.c").read_text(encoding="utf-8")
    hos = (ROOT / "host/hos.h").read_text(encoding="utf-8")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    until = block(thread, "void hos_sleep_until_ns(int64_t until, int64_t margin_ns) {")
    for needle, why in (
            ("const int away = hos_vcore_away();", "the core is not given up for the wait"),
            ("pace_wait_until(until, margin_ns, hos_now_ns, sleep_ns, yield_for_wait);", "the wait is not pace_wait.h's, on sleep_ns's timer"),
            ("if (away) hos_vcore_back();", "the core is not taken back after the wait")):
        if needle not in until:
            raise SystemExit("FAIL: host/hos_thread.c hos_sleep_until_ns: " + why)
    if until.index("hos_vcore_away") > until.index("pace_wait_until") or until.index("pace_wait_until") > until.index("hos_vcore_back"):
        raise SystemExit("FAIL: host/hos_thread.c hos_sleep_until_ns: the core is not given up around the whole wait")
    if "if (!SwitchToThread()) YieldProcessor();" not in block(thread, "static void yield_for_wait(void) {"):
        raise SystemExit("FAIL: host/hos_thread.c: the wait's end is not yielded as a short sleep yields")
    # (sleep_ns takes the timer from a millisecond on: the timer's part of the wait is never shorter)
    if "if (ns < 1000000 && !short_timer) {" not in block(thread, "static void sleep_ns(int64_t ns) {"):
        raise SystemExit("FAIL: host/hos_thread.c sleep_ns no longer takes the timer from 1 ms on (PACE_WAIT_TIMER_MIN_NS)")
    if "void hos_sleep_until_ns(int64_t until, int64_t margin_ns);" not in hos:
        raise SystemExit("FAIL: host/hos.h does not declare hos_sleep_until_ns")
    sleep = block(vi, "static void pace_sleep(int64_t until, int64_t now) {")
    for needle, why in (
            ('getenv("HOS_PACE_SPIN")', "HOS_PACE_SPIN is not read"),
            ("margin = e && *e ? atoll(e) * 1000 : PACE_WAIT_MARGIN_NS;", "the margin is not PACE_WAIT_MARGIN_NS by default, microseconds by the switch"),
            ("if (margin) hos_sleep_until_ns(until, margin);", "the wait does not go to its time"),
            ("else hos_sleep_ns(until - now);", "HOS_PACE_SPIN=0 is not the timer alone, as before"),
            ('"vi: frame waits: ', "the waits' lateness is not logged")):
        if needle not in sleep:
            raise SystemExit("FAIL: host/nn_vi.c pace_sleep: " + why)
    if "if (until > now) pace_sleep(until, now);" not in block(vi, "void hos_vi_native_acquire(int active) {"):
        raise SystemExit("FAIL: host/nn_vi.c hos_vi_native_acquire does not wait through pace_sleep")
    if "| `HOS_PACE_SPIN=0`" not in dev:
        raise SystemExit("FAIL: docs/development.md has no row for HOS_PACE_SPIN")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    check_sources()
    with tempfile.TemporaryDirectory(prefix="pace-wait-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture"
        c.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu17", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host"), str(c), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the frame wait ends on its time whatever the timer's lateness up to the margin, at most the margin yielded")


if __name__ == "__main__":
    main()
