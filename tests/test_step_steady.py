"""The game's time step kept steady while it follows the clock, with HOS_STEP_STEADY=1 (host/step_steady.h; no game).

The real-time step (host/nn_vi.c realtime_step) is the mean of the last five present intervals: after a 50 ms frame at a
cap of 240 the next steps are three periods long, and intervals of 5 and 30 ms in turn give steps of 15 and 20 ms in turn.
NX2navi's steady step (runtime/frame_rate.c ocarina_step, the same game) takes the mean of 12 intervals without their
longest and shortest, snaps it to the paced period within 3%, and repays what is owed at a 64th a frame, 10% of the step
at most.

Compiles host/step_steady.h and runs it on display intervals, beside the mean of five as realtime_step had it: a held cap
of 60 and of 240 gives exactly the period every frame, and with a present's jitter of up to 2% still within 1% of it; a
50 ms hitch at 240 is repaid over most of a second (0.4 to 2.5 s allowed) with no step over 10% past the period, where
the mean of five gave 13 ms steps; 5 and 30 ms in turn give one step; a change from 60 to 30 fps arrives within 12 frames;
the step stays within its bounds; and over 20,000 frames of uneven intervals game time follows the clock within 50 ms. Then
it reads realtime_step: the steady step only with the switch, from the interval just measured and the period the frames
are paced by, within the bounds each path had, and started afresh where the average was; and the switch's row in the guide.

    python -I -B tests/test_step_steady.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "step_steady.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static const double LO = 1.0 / 240, HI = 4.0 / 60;
/* the step as realtime_step made it: the mean of the last five */
typedef struct { double s[5]; unsigned n, next; } Mean5;
static double mean5(Mean5 *m, double dt) {
    m->s[m->next++ % 5] = dt;
    if (m->n < 5) m->n++;
    double sum = 0;
    for (unsigned i = 0; i < m->n; i++) sum += m->s[i];
    return sum / m->n;
}
static uint32_t seed = 11;
static double uniform(void) { seed = seed * 1103515245u + 12345u; return ((seed >> 8) & 0xffff) / 65535.0; }
int main(void) {
    StepSteady s;
    /* a held cap: exactly the period every frame, from the fourth */
    for (int cap = 60; cap <= 240; cap += 180) {
        const double p = 1.0 / cap;
        step_steady_reset(&s);
        for (int i = 0; i < 2000; i++) {
            const double step = step_steady(&s, p, p, 1.0 / 240, HI);
            if (i >= 3) CHECK(step == p);
        }
        /* with a present's jitter of up to 2% of the period: within 1% of it, and game time keeps the clock */
        step_steady_reset(&s);
        double wall = 0, game = 0;
        for (int i = 0; i < 20000; i++) {
            const double dt = p * (1 + (uniform() - 0.5) * 0.04), step = step_steady(&s, dt, p, 1.0 / 240, HI);
            if (i >= 12) CHECK(fabs(step - p) <= 0.01 * p);
            wall += dt, game += step;
        }
        CHECK(fabs(game - wall) <= p);
    }
    /* a 50 ms hitch at 240: repaid over most of a second, no step over 10% past the period; the mean of five lurched */
    {
        const double p = 1.0 / 240;
        Mean5 m = {{0}, 0, 0};
        step_steady_reset(&s);
        double most_new = 0, most_old = 0, wall = 0, game = 0;
        int repaid_at = -1;
        for (int i = 0; i < 2000; i++) {
            const double dt = i == 500 ? 0.050 : p;
            const double step = step_steady(&s, dt, p, LO, HI), old = mean5(&m, dt);
            wall += dt, game += step;
            if (i > 500) {
                if (step > most_new) most_new = step;
                if (old > most_old) most_old = old;
                if (repaid_at < 0 && fabs(s.owed) <= p) repaid_at = i;   /* (all but a period of it) */
            }
        }
        CHECK(most_old >= 3 * p);                /* (13 ms steps after the hitch) */
        CHECK(most_new <= 1.1 * p + 1e-12);      /* (at most 10% over the period) */
        CHECK(repaid_at - 500 >= 100 && repaid_at - 500 <= 600);   /* (over 0.4 to 2.5 s) */
        CHECK(fabs(game - wall) <= p);
        printf("a 50 ms hitch at 240: the mean of five gave steps up to %.1f ms; the steady step %.2f ms, repaid in %d frames\n",
               most_old * 1e3, most_new * 1e3, repaid_at - 500);
    }
    /* 5 and 30 ms in turn: one step, where the mean of five gave 15 and 20 in turn */
    {
        Mean5 m = {{0}, 0, 0};
        step_steady_reset(&s);
        double lo = 1, hi = 0, olo = 1, ohi = 0;
        for (int i = 0; i < 400; i++) {
            const double dt = i % 2 ? 0.030 : 0.005, step = step_steady(&s, dt, 0, LO, HI), old = mean5(&m, dt);
            if (i >= 100) {
                if (step < lo) lo = step;
                if (step > hi) hi = step;
                if (old < olo) olo = old;
                if (old > ohi) ohi = old;
            }
        }
        CHECK(ohi - olo > 0.004 && hi - lo < 0.0006 && fabs((lo + hi) / 2 - 0.0175) < 0.001);
    }
    /* 60 to 30 fps: the step there within 12 frames */
    {
        step_steady_reset(&s);
        for (int i = 0; i < 100; i++) step_steady(&s, 1.0 / 60, 1.0 / 60, 1.0 / 60, HI);
        int at = -1;
        for (int i = 0; i < 40; i++) {
            const double step = step_steady(&s, 1.0 / 30, 1.0 / 60, 1.0 / 60, HI);
            if (at < 0 && step >= 1.0 / 30 * 0.97) at = i;
        }
        CHECK(at >= 0 && at <= 12);
    }
    /* the bounds, and what they took owed */
    {
        step_steady_reset(&s);
        for (int i = 0; i < 50; i++) CHECK(step_steady(&s, 0.001, 0, LO, HI) >= LO);
        CHECK(s.owed < 0 && s.owed >= -STEP_STEADY_OWED_MAX);
        step_steady_reset(&s);
        for (int i = 0; i < 50; i++) CHECK(step_steady(&s, 0.2, 0, LO, HI) <= HI);
        CHECK(s.owed == 0);   /* (a frame past the longest step slows the game, nothing owed for it) */
    }
    /* uneven intervals: game time follows the clock within a step */
    {
        step_steady_reset(&s);
        double wall = 0, game = 0;
        for (int i = 0; i < 20000; i++) {
            const double dt = 0.004 + uniform() * 0.026, step = step_steady(&s, dt, 1.0 / 60, LO, HI);
            CHECK(step >= LO && step <= HI);
            wall += dt, game += step;
        }
        CHECK(fabs(game - wall) <= 0.05);   /* (a base snapped to the period that the frames miss by 2% owes up to some 30 ms) */
    }
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
    vi = (ROOT / "host/nn_vi.c").read_text(encoding="utf-8")
    step = block(vi, "static void realtime_step(int64_t now) {")
    check('getenv("HOS_STEP_STEADY")' in step and "atoi(getenv(\"HOS_STEP_STEADY\")) == 0" in step,
          "the steady step is not on unless HOS_STEP_STEADY=0")
    check("if (dt <= 0 || dt > 0.25) { count = next = 0; carry = 0; step_steady_reset(&steady); return; }" in step,
          "the steady step does not start afresh where the average does")
    check("secs_avg = step_steady(&steady, dt, (double)pace_period_ns / 1e9, timed ? 1.0 / 240.0 : 1.0 / top, 4.0 / 60.0);" in step,
          "the steady step is not taken from this interval and the paced period within each path's bounds")
    check(step.index("const int top = timed ? 60 : fps_max();") < step.index("secs_avg = step_steady("),
          "the bounds are read before they are known")
    check("const double secs = secs_avg;" in step and "const double ratio = secs_avg * 60.0;" in step,
          "the step the game gets is not the one made")
    check('#include "step_steady.h"' in vi, "host/nn_vi.c does not include the rule")
    check("| `HOS_STEP_STEADY=0`" in (ROOT / "docs/development.md").read_text(encoding="utf-8"),
          "docs/development.md has no row for HOS_STEP_STEADY")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="step-steady-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture"
        c.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu17", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host"), str(c), "-o", str(exe), "-lm"],
                       check=True)
        out = subprocess.run([str(exe)], check=True, stdout=subprocess.PIPE, text=True).stdout
    print("\n".join(out.strip().splitlines()[:-1]))
    print("PASS: the steady step holds the period, repays a hitch slowly and keeps the clock")


if __name__ == "__main__":
    main()
