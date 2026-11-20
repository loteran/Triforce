"""The Steady frame rate row: the game paced to a frame rate the PC holds in the scene, its pictures held to it (no game).

Under a Frame rate cap the PC does not hold (the owner's 240, the lab's Market at ~93 fps) nothing waits, and each frame goes
out the moment it is done, as uneven as its work. host/pace_steady.h picks a period the frames fit in (lowered quickly when
they run late, raised slowly when there is room, hitches left out, a backoff against hunting, never past the cap);
host/nn_vi.c steady_period paces the frames' starts by it (the acquire's wait) and gpu/vk/present_smooth.h
present_smooth_paced_at holds each picture to it.

Compiles the three headers the pacing is made of (pace_steady.h, present_smooth.h, step_steady.h) with a model of the frame
loop as host/nn_vi.c has it (the acquire's wait for the display queue with three window textures, the present, the
renderer's hold of the picture on the presenting thread or on the present thread, the steady step), and runs it:

  - on synthetic frame times: a Market-like scene (10.5 ms, a millisecond's slow swing, the lab's 600-frame screenshot spike)
    settles on a rate of the set and stays (no hunting); a scene getting heavier is followed within a second, one getting
    lighter only after a few seconds of room; hitches and loads move nothing; a cost sitting on a boundary between two rates
    for ten minutes changes the rate a few times, not every second, and backs off; the sets (any rate, steps of 240, 144 and
    an unknown refresh, a cap of 60 or 144); another limiter puts the cap back; the present's hold;
  - on the lab's recorded frame times, when they are there (E:\\TriforceDev\\lab-runs, or TRIFORCE_LAB_RUNS): each run's frames
    replayed as each frame's work, the shown intervals of the same frames (those of the run's last 60 s) for the cap as it is
    (the Frame pacing row off, the default; and on), the steady frame rate (any rate; steps of 240 Hz; with the present thread)
    and the start alone without the present's hold, printed: fps, sd, p50, p99, frame-to-frame jitter, frames over 1.5x the
    median (per 1000), how far each shown interval is from the game's time step for it, and the rate changes. The Market runs
    must gain (jitter, frames over 1.5x, time-step mismatch) for at most 30% of their frame rate.

Then it reads the wiring: host/nn_vi.c measures what a frame needed without its own waits and paces by the steady period,
the renderer holds the pictures to it only while the game's own wait paces them, the row, its key and the docs.

    python -I -B tests/test_pace_steady.py [--cc gcc] [--lab DIR]
"""
import argparse
import csv
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
LAB = os.environ.get("TRIFORCE_LAB_RUNS", r"E:\TriforceDev\lab-runs")
# (the lab's runs: the Market's stand after a start (S1), and the Market after a tour of ten areas (S2); the tours' Markets
# with the re-upload bursts of triforce-frame-stability.md are lift, f24-S2 and async, rel's is a good one)
MARKET = ["r25-base-1", "r25-base-2", "f24-S1-1"]
TOURS = ["r24-lift-tour", "f24-S2-1", "r23-async-tour", "r24-rel-tour"]

FIXTURE = r'''#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pace_steady.h"
#include "present_smooth.h"
#include "step_steady.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
#define MS 1000000LL
#define MAXN 60000

/* ---- the frame loop, as host/nn_vi.c and the renderer have it ---- */
enum { FLOOR_NONE, FLOOR_SMOOTH, FLOOR_PACED };
/* (probe: the rate by unpaced probes, PaceSteady.probe; grow: a frame's work grows by this much of its time step, the interval
   before it, past grow_from: test31's Market, 11.6 ms unpaced and 15.5 paced at 18.2 ms) */
typedef struct { int steady, floor, thread, cap, refresh, probe; double grow; int64_t grow_from; } Cfg;
typedef struct {
    int64_t shown[MAXN];    /* (when each picture went out) */
    double step[MAXN];      /* (the game's time step of each frame, s) */
    int64_t period[MAXN];   /* (the period the rule held when each frame started: a probe's frames run at the cap) */
    int64_t changes, lowerings, raises, hitches;
    int64_t set[PACE_STEADY_MAX_PERIODS];
    int nset;
} Run;
/* cost[k]: frame k's work from its start to its present (ns). Frame starts wait for the display queue (three window
   textures: the frame two back's display time, hos_vi_native_acquire); the present's hold (the renderer's floor) is on the
   presenting thread (thread 0: the next frame starts after it, the default) or on the present thread (1: the presenting
   thread goes on, and the next present waits for this one to be out). The host's present (hos_vi_native_presented) comes
   when the present call returns: what the frame needed is its present interval without the acquire's wait and the hold */
static void run(const int64_t *cost, int n, Cfg c, Run *r) {
    static PaceSteady st;
    PresentSmooth ps = {0, 0};
    StepSteady ss;
    int64_t shown_at[16] = {0}, count = 0, t = 1000 * MS, prev_out = 0, last_free = 0, waited = 0, held = 0;
    const int64_t base = 1000000000LL / c.cap;
    int64_t P = base;
    step_steady_reset(&ss);
    r->changes = r->lowerings = r->raises = r->hitches = 0;
    if (c.steady) pace_steady_init(&st, c.steady, c.cap, c.refresh), st.probe = c.probe;
    r->nset = c.steady ? st.n : 0;
    for (int i = 0; i < r->nset; i++) r->set[i] = st.periods[i];
    for (int k = 0; k < n; k++) {
        const int64_t until = count >= 2 ? shown_at[(count - 2) & 15] : 0, start = t > until ? t : until;
        waited += start - t;
        r->period[k] = c.steady ? pace_steady_held(&st) : P;
        const int64_t step = k >= 2 ? r->shown[k - 1] - r->shown[k - 2] : 0;
        const int64_t work = cost[k] + (c.grow > 0 && step > c.grow_from ? (int64_t)(c.grow * (step - c.grow_from)) : 0);
        const int64_t end = start + work, ready = c.thread && prev_out > end ? prev_out : end;
        int64_t out = ready;
        if (c.floor == FLOOR_SMOOTH) out = present_smooth_at(&ps, ready);
        if (c.floor == FLOOR_PACED) out = present_smooth_paced_at(&ps, ready, c.steady ? P : base);
        present_smooth_shown(&ps, out);
        if (!c.thread) held += out - ready;
        prev_out = out, r->shown[k] = out;
        const int64_t free_at = c.thread ? ready : out, interval = last_free ? free_at - last_free : 0;
        if (c.steady) {
            int64_t took, need;   /* (one thread: the acquire's wait and the hold add up) */
            pace_steady_measure(interval, waited + held, 0, &took, &need);
            if (interval > 0 && pace_steady_feed(&st, took, need, interval)) r->changes++;
            P = pace_steady_period(&st) > base ? pace_steady_period(&st) : base;
        }
        waited = held = 0;
        const double dt = interval / 1e9;
        if (dt <= 0 || dt > 0.25) step_steady_reset(&ss), r->step[k] = dt > 0 ? 4.0 / 60 : 1.0 / c.cap;
        else r->step[k] = step_steady(&ss, dt, P / 1e9, 1.0 / 240, 4.0 / 60);
        shown_at[count & 15] = count && shown_at[(count - 1) & 15] + P > free_at ? shown_at[(count - 1) & 15] + P : free_at;
        count++, last_free = free_at, t = free_at;
    }
    if (c.steady) r->lowerings = st.lowerings, r->raises = st.raises, r->hitches = st.hitches;
}

/* ---- what the shown intervals of frames a..b-1 come to ---- */
typedef struct { double fps, sd, p50, p99, jit, over15, mismatch; } Stats;
static int cmp64(const void *a, const void *b) { const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }
static Stats stats(const Run *r, int a, int b) {
    static int64_t d[MAXN], s[MAXN];
    Stats o = {0};
    int n = 0;
    double sum = 0, sq = 0, jit = 0, mis = 0;
    for (int k = a + 1; k < b; k++) {
        d[n] = r->shown[k] - r->shown[k - 1];
        sum += d[n], sq += (double)d[n] * d[n];
        if (n) jit += llabs(d[n] - d[n - 1]);
        mis += fabs(d[n] / 1e9 - r->step[k]);
        n++;
    }
    memcpy(s, d, sizeof d[0] * n);
    qsort(s, n, sizeof s[0], cmp64);
    const double mean = sum / n;
    int over = 0;
    for (int i = 0; i < n; i++) over += d[i] * 2 > s[n / 2] * 3;
    o.fps = n / (sum / 1e9), o.sd = sqrt(sq / n - mean * mean) / 1e6, o.p50 = s[n / 2] / 1e6, o.p99 = s[n * 99 / 100] / 1e6;
    o.jit = jit / (n - 1) / 1e6, o.over15 = over * 1000.0 / n, o.mismatch = mis / n * 1e3;
    return o;
}
static void print(const char *name, Stats s, const Run *r) {
    printf("  %-30s %6.1f fps  sd %5.2f  p50 %5.2f  p99 %6.2f  jitter %5.2f  >1.5x %5.1f/1000  step off %5.2f ms", name, s.fps, s.sd,
           s.p50, s.p99, s.jit, s.over15, s.mismatch);
    if (r->nset) printf("  | %lld changes (%lld down, %lld up), %lld left out", (long long)r->changes, (long long)r->lowerings,
                        (long long)r->raises, (long long)r->hitches);
    putchar('\n');
}

/* ---- synthetic frame times ---- */
static uint32_t seed = 7;
static double uniform(void) { seed = seed * 1103515245u + 12345u; return ((seed >> 8) & 0xffffff) / 16777216.0; }
static double gauss(void) { double s = 0; for (int i = 0; i < 12; i++) s += uniform(); return s - 6; }
/* a Market-like scene: base ms, a slow swing of about sd ms (AR(1), 0.97 a frame) and a fast one, the lab's screenshot every
   600 frames (+11.5 ms) */
static void market(int64_t *c, int n, double base, double sd, int shots) {
    double slow = 0;
    for (int k = 0; k < n; k++) {
        slow = 0.97 * slow + gauss() * sd * 0.243;
        double ms = base + slow + gauss() * sd * 0.5;
        if (shots && k % 600 == 1) ms += 11.5;
        c[k] = (int64_t)((ms < 1 ? 1 : ms) * MS);
    }
}
static int64_t total_ns(const int64_t *c, int n) { int64_t s = 0; for (int k = 0; k < n; k++) s += c[k]; return s; }
static int in_set(const Run *r, int64_t p) { for (int i = 0; i < r->nset; i++) if (r->set[i] == p) return 1; return 0; }
/* the first frame from k on whose period differs from frame k's, n if none */
static int next_change(const Run *r, int k, int n) { for (int i = k + 1; i < n; i++) if (r->period[i] != r->period[k]) return i; return n; }
static double secs_between(const Run *r, int a, int b) { return (r->shown[b] - r->shown[a]) / 1e9; }

static Run R1, R2;
static int64_t C1[MAXN], C2[MAXN];

static void synthetic(void) {
    const Cfg base = {0, FLOOR_NONE, 0, 240, 240, 0, 0, 0}, any = {PACE_STEADY_ANY, FLOOR_PACED, 0, 240, 240, 0, 0, 0};
    /* the sets */
    {
        static PaceSteady s;
        pace_steady_init(&s, PACE_STEADY_ANY, 240, 0);
        CHECK(s.n == 19 && s.periods[0] == 1000000000LL / 240 && s.periods[s.n - 1] == 1000000000LL / 30);
        for (int i = 1; i < s.n; i++) CHECK(s.periods[i] > s.periods[i - 1]);
        pace_steady_init(&s, PACE_STEADY_ANY, 165, 0);
        CHECK(s.periods[0] == 1000000000LL / 165 && s.periods[1] == 1000000000LL / 160);
        pace_steady_init(&s, PACE_STEADY_ANY, 60, 0);
        CHECK(s.n == 7 && s.periods[0] == 1000000000LL / 60 && s.periods[1] == 1000000000LL / 55);
        pace_steady_init(&s, PACE_STEADY_REFRESH, 240, 240);   /* 240, 120, 80, 60, 48, 40, 34.3, 30 */
        CHECK(s.n == 8 && s.periods[0] == 1000000000LL / 240 && s.periods[1] == 2000000000LL / 240 && s.periods[2] == 3000000000LL / 240);
        CHECK(s.periods[7] == 8000000000LL / 240);
        pace_steady_init(&s, PACE_STEADY_REFRESH, 144, 240);   /* (144 is no division of 240: 120 first, never past the cap) */
        CHECK(s.periods[0] == 2000000000LL / 240 && s.periods[1] == 3000000000LL / 240);
        pace_steady_init(&s, PACE_STEADY_REFRESH, 240, 144);   /* 144, 72, 48, 36 */
        CHECK(s.n == 4 && s.periods[0] == 1000000000LL / 144 && s.periods[3] == 4000000000LL / 144);
        pace_steady_init(&s, PACE_STEADY_REFRESH, 120, 240);
        CHECK(s.periods[0] == 1000000000LL / 120 && s.periods[1] == 1000000000LL / 80);
        pace_steady_init(&s, PACE_STEADY_REFRESH, 240, 0);     /* (the refresh not known: any rate's set) */
        CHECK(s.n == 19);
        CHECK(pace_steady_period(&s) == 1000000000LL / 240);   /* (the cap's at the start) */
    }
    /* the present's hold: to the period less a 64th after the last picture, a late one at once, half the period and 8 ms at
       most; paced 0 is present_smooth_at */
    {
        const int64_t P = 1000000000LL / 80;
        PresentSmooth p = {10 * MS, 100 * MS};
        CHECK(present_smooth_paced_at(&p, 101 * MS, 0) == present_smooth_at(&p, 101 * MS));
        CHECK(present_smooth_paced_at(&p, 107 * MS, P) == 100 * MS + P - P / 64);
        CHECK(present_smooth_paced_at(&p, 100 * MS + P, P) == 100 * MS + P);
        CHECK(present_smooth_paced_at(&p, 101 * MS, P) == 101 * MS + P / 2);
        PresentSmooth slow = {40 * MS, 100 * MS};
        CHECK(present_smooth_paced_at(&slow, 101 * MS, 1000000000LL / 30) == 109 * MS);
        PresentSmooth first = {0, 0};
        CHECK(present_smooth_paced_at(&first, 5 * MS, P) == 5 * MS);
    }
    /* a Market-like scene for five minutes: a rate of the set, settled within 5 s, a period the frames fit in, never past the
       cap, few changes after it settled (not hunting), and steadier than the cap as it is */
    {
        const int n = 30000;
        market(C1, n, 10.5, 1.0, 1);
        run(C1, n, base, &R1);
        run(C1, n, any, &R2);
        int settled = 0;
        while (settled < n && secs_between(&R2, 0, settled) < 5) settled++;
        int64_t later = 0;
        for (int k = settled + 1; k < n; k++) {
            CHECK(in_set(&R2, R2.period[k]) && R2.period[k] >= 1000000000LL / 240);
            later += R2.period[k] != R2.period[k - 1];
        }
        const double minutes = secs_between(&R2, settled, n - 1) / 60;
        CHECK(R2.period[n - 1] >= 105 * MS / 10);   /* (no shorter than the frames' median work) */
        CHECK(later <= 4 * minutes + 2);            /* (a change every quarter minute at most) */
        const Stats a = stats(&R1, n / 2, n), b = stats(&R2, n / 2, n);
        puts("synthetic Market (10.5 ms, a slow swing of 1 ms, a +11.5 ms frame every 600), its last half:");
        print("cap 240 as it is", a, &R1);
        print("steady, any rate", b, &R2);
        printf("  (%lld changes after the first 5 s, %.1f a minute)\n", (long long)later, later / minutes);
        CHECK(b.jit < a.jit * 0.6 && b.mismatch < a.mismatch && b.over15 <= a.over15 + 0.5 && b.fps > a.fps * 0.7);
    }
    /* heavier: 6 ms for 30 s, then 14 ms: followed within a second, and the new period fits */
    {
        int n = 0;
        while (total_ns(C1, n) < 30000 * MS) C1[n] = (int64_t)((6 + gauss() * 0.3) * MS), n++;
        const int step_at = n;
        for (int i = 0; i < 4000; i++) C1[n] = (int64_t)((14 + gauss() * 0.5) * MS), n++;
        run(C1, n, any, &R1);
        CHECK(R1.period[step_at - 1] <= 7 * MS);
        const int down = next_change(&R1, step_at, n);
        CHECK(down < n && secs_between(&R1, step_at, down) <= 1.0);
        CHECK(R1.period[n - 1] >= 14 * MS && R1.period[n - 1] <= 18 * MS);
        printf("heavier (6 ms to 14 ms): lowered %.2f s after, to %.0f fps; at the end %.0f fps\n", secs_between(&R1, step_at, down),
               1e9 / R1.period[down], 1e9 / R1.period[n - 1]);
    }
    /* lighter: 14 ms for 30 s, then 6 ms: not raised for 3 s, raised within 15 s, to a rate the frames fit in */
    {
        int n = 0;
        while (total_ns(C1, n) < 30000 * MS) C1[n] = (int64_t)((14 + gauss() * 0.5) * MS), n++;
        const int step_at = n;
        for (int i = 0; i < 6000; i++) C1[n] = (int64_t)((6 + gauss() * 0.3) * MS), n++;
        run(C1, n, any, &R1);
        CHECK(R1.period[step_at - 1] >= 14 * MS);
        const int up = next_change(&R1, step_at, n);
        CHECK(up < n && secs_between(&R1, step_at, up) >= 3.0 && secs_between(&R1, step_at, up) <= 15.0);
        CHECK(R1.period[up] < R1.period[step_at] && R1.period[n - 1] >= 63 * MS / 10 && R1.period[n - 1] <= 8 * MS);
        printf("lighter (14 ms to 6 ms): raised %.1f s after, to %.0f fps; at the end %.0f fps\n", secs_between(&R1, step_at, up),
               1e9 / R1.period[up], 1e9 / R1.period[n - 1]);
    }
    /* hitches do not move it: the same scene with a 60 ms frame every 2 s, three 35 ms frames every 5 s and a 400 ms load every
       30 s lowers it no more than without them, and ends on the same rate */
    {
        const int n = 20000;
        market(C1, n, 10.5, 0.6, 0);
        memcpy(C2, C1, sizeof C1[0] * n);
        for (int k = 0; k < n; k++) {
            if (k % 180 == 90) C2[k] = 60 * MS;
            if (k % 450 >= 200 && k % 450 < 203) C2[k] = 35 * MS;
            if (k % 2800 == 1400) C2[k] = 400 * MS;
        }
        run(C1, n, any, &R1);
        run(C2, n, any, &R2);
        printf("hitches: %lld lowerings without them, %lld with them (%lld frames left out); %.0f and %.0f fps at the end\n",
               (long long)R1.lowerings, (long long)R2.lowerings, (long long)R2.hitches, 1e9 / R1.period[n - 1], 1e9 / R2.period[n - 1]);
        CHECK(R2.lowerings <= R1.lowerings && R2.period[n - 1] == R1.period[n - 1] && R2.hitches > 100);
    }
    /* a cost on the boundary between two rates, swinging slowly either side of it for ten minutes: a few changes a minute at
       most, and the backoff grows */
    {
        const int n = 50000;
        double slow = 0;
        for (int k = 0; k < n; k++) {
            slow = 0.97 * slow + gauss() * 0.15;
            const double ms = 11.3 + 0.6 * sin(k / 1500.0) + slow + gauss() * 0.4;
            C1[k] = (int64_t)(ms * MS);
        }
        run(C1, n, any, &R1);
        const double minutes = secs_between(&R1, 0, n - 1) / 60;
        printf("on a boundary (11.3 ms swinging 0.6 ms, ten minutes): %lld changes (%.1f a minute: %lld down, %lld up)\n",
               (long long)R1.changes, R1.changes / minutes, (long long)R1.lowerings, (long long)R1.raises);
        CHECK(R1.changes <= 3 * minutes + 2);
    }
    /* another limiter: the cap's period again, nothing kept */
    {
        static PaceSteady s;
        pace_steady_init(&s, PACE_STEADY_ANY, 240, 0);
        for (int k = 0; k < 200; k++) pace_steady_feed(&s, 12 * MS, 12 * MS, 12 * MS);
        CHECK(pace_steady_period(&s) > 12 * MS);
        pace_steady_reset(&s);
        CHECK(pace_steady_period(&s) == 1000000000LL / 240 && s.count == 0 && s.recent_count == 0);
        CHECK(!pace_steady_feed(&s, 12 * MS, 12 * MS, 0) && !pace_steady_feed(&s, 12 * MS, 12 * MS, 300 * MS));   /* (none; a load) */
        CHECK(s.count == 0 && s.loads == 1 && s.unmeasured == 1);
    }
    /* (test26) light frames that waited most of the cap's period, then a scene 7 times heavier: the heavy frames are hitches
       only until they are the median of what the frames took, then late, and the rate comes down within 64 frames of it (it
       stayed at the cap for good while the median was the kept frames' alone) */
    {
        static PaceSteady s;
        pace_steady_init(&s, PACE_STEADY_ANY, 240, 0);
        const int64_t cap = 1000000000LL / 240;
        for (int k = 0; k < 1200; k++) CHECK(!pace_steady_feed(&s, 15 * MS / 10, 15 * MS / 10, cap));
        int k = 0;
        while (k < 200 && !pace_steady_feed(&s, 11 * MS, 11 * MS, 11 * MS)) k++;
        printf("light frames (1.5 ms of 4.2) then 11 ms ones: lowered at the %dth of them, to %.0f fps (%lld left out as hitches)\n",
               k + 1, 1e9 / pace_steady_period(&s), (long long)s.hitches);
        CHECK(k < 64 && pace_steady_period(&s) >= 11 * MS && s.hitches < 40);
    }
    /* what a frame took and needed: the waits add up (OoT's threads take turns), the longest where they add up to the interval */
    {
        int64_t took, need;
        pace_steady_measure(12 * MS, 3 * MS, 0, &took, &need);   /* (one thread: acquire wait and hold, 3 ms in all) */
        CHECK(took == 9 * MS && need == 9 * MS);
        pace_steady_measure(16500000, 300000, 5800000, &took, &need);   /* (test27's Market at 60 fps: the hold, the acquire) */
        CHECK(took == 10700000 && need == 10400000);
        pace_steady_measure(12 * MS, 2 * MS, 8 * MS, &took, &need);
        CHECK(took == 4 * MS && need == 2 * MS);
        pace_steady_measure(11 * MS, 0, 0, &took, &need);             /* (late: no wait anywhere) */
        CHECK(took == 11 * MS && need == 11 * MS);
        pace_steady_measure(12 * MS, 7 * MS, 8 * MS, &took, &need);   /* (waits that overlap: the longest) */
        CHECK(took == 4 * MS && need == 4 * MS);
    }
    /* (test27) a load, the 40 heavier frames after it, then the scene: the lowering waits for the settling and goes to the rate
       the scene's 90th percentile fits in (80 fps for 11.7 ms), not the one the arrival's frames would want (60) */
    {
        static PaceSteady s;
        pace_steady_init(&s, PACE_STEADY_ANY, 240, 0);
        const int64_t cap = 1000000000LL / 240;
        for (int k = 0; k < 1200; k++) pace_steady_feed(&s, 13 * MS / 10, 13 * MS / 10, cap);
        pace_steady_feed(&s, 262 * MS, 262 * MS, 262 * MS);
        for (int k = 0; k < 40; k++) CHECK(!pace_steady_feed(&s, (15 + k % 6) * MS, (15 + k % 6) * MS, (15 + k % 6) * MS));
        int k = 0, changed = 0;
        while (k < 600 && !changed) {
            const int64_t t = (int64_t)((10.6 + gauss() * 0.86) * MS);
            changed = pace_steady_feed(&s, t, t, t), k++;
        }
        printf("a load, 40 frames of 15 to 20 ms, then 10.6 ms ones: lowered at the %dth of those, to %.0f fps (%lld settling)\n", k,
               1e9 / pace_steady_period(&s), (long long)s.settling);
        CHECK(changed && pace_steady_period(&s) == 1000000000LL / 80 && s.settling >= 40 && s.loads == 1);
    }
    /* one_step: the raise goes to the next rate only */
    {
        static PaceSteady s;
        pace_steady_init(&s, PACE_STEADY_ANY, 240, 0);
        s.one_step = 1;
        int k = 0;
        while (k < 2000 && pace_steady_period(&s) < 12 * MS) pace_steady_feed(&s, 13 * MS, 13 * MS, 13 * MS), k++;
        const int at = s.at;
        for (k = 0; k < 4000 && s.at == at; k++) pace_steady_feed(&s, 2 * MS, 2 * MS, 1000000000LL / 66);
        CHECK(s.at == at - 1);
    }
    /* steps of 240 Hz: every period a whole number of refreshes */
    {
        const Cfg steps = {PACE_STEADY_REFRESH, FLOOR_PACED, 0, 240, 240, 0, 0, 0};
        const int n = 20000;
        market(C1, n, 10.5, 1.0, 1);
        run(C1, n, steps, &R1);
        for (int k = 0; k < n; k++) CHECK(in_set(&R1, R1.period[k]) || R1.period[k] == 1000000000LL / 240);
        CHECK(R1.period[n - 1] == 3000000000LL / 240 || R1.period[n - 1] == 4000000000LL / 240);
    }
    /* (test31) the probes. A probe's frames run at the cap's period, PACE_STEADY_PROBE_SKIP + PACE_STEADY_PROBE_FRAMES of them,
       and the rate held stays what it was meanwhile */
    {
        static PaceSteady s;
        pace_steady_init(&s, PACE_STEADY_ANY, 240, 0);
        s.probe = 1;
        int k = 0, probing = 0;
        for (; k < 200; k++) {   /* (the first frames: a probe at once, of 11 ms frames, and 80 fps then) */
            probing += pace_steady_period(&s) == 1000000000LL / 240 && s.probing;
            pace_steady_feed(&s, 11 * MS, 11 * MS, 11 * MS);
            if (!s.probing && s.probes) break;
        }
        CHECK(s.probes == 1 && pace_steady_held(&s) == 1000000000LL / 80 && pace_steady_period(&s) == pace_steady_held(&s));
        /* (the first probe of a scene measures twice as many) */
        CHECK(probing == PACE_STEADY_PROBE_SKIP + 2 * PACE_STEADY_PROBE_FRAMES && s.probe_count == 2 * PACE_STEADY_PROBE_FRAMES);
        /* (held at 80 with the frames on time: the next probe is the periodic one, 12 s on, and the rate stays) */
        int64_t at = 0;
        while (at < 13000 * MS) { pace_steady_feed(&s, 11 * MS, 11 * MS, 1000000000LL / 80); at += 1000000000LL / 80; }
        CHECK(s.probes == 2 && pace_steady_held(&s) == 1000000000LL / 80 && s.probe_count == PACE_STEADY_PROBE_RING);
        /* (a probe over 30% off the ring's median is a new scene: the probes before it go, and the rate is its own) */
        s.since_probe_ns = PACE_STEADY_PROBE_EVERY_NS;
        pace_steady_feed(&s, 11 * MS, 11 * MS, 1000000000LL / 80);
        CHECK(s.probing);
        while (s.probing) pace_steady_feed(&s, 6 * MS, 6 * MS, 6 * MS);
        CHECK(s.scenes == 1 && s.probe_count == PACE_STEADY_PROBE_FRAMES && pace_steady_held(&s) == 1000000000LL / 144);
    }
    /* (test31) the Market at 11.6 ms a frame unpaced (12.0 at the 90th), its work growing with the time step (15.5 ms paced at
       18.2 ms, as the lab measured): with the probes it stays near 80 fps for five minutes; the frames' own measure slides */
    {
        Cfg probed = {PACE_STEADY_ANY, FLOOR_PACED, 0, 240, 240, 1, 0.59, 116 * MS / 10}, unprobed = probed;
        unprobed.probe = 0;
        const int n = 25000;
        market(C1, n, 11.6, 0.28, 0);
        run(C1, n, probed, &R1);
        run(C1, n, unprobed, &R2);
        int64_t slowest1 = 0, slowest2 = 0;
        int from = 0;
        while (from < n && secs_between(&R1, 0, from) < 10) from++;
        for (int k = from; k < n; k++) {
            slowest1 = R1.period[k] > slowest1 ? R1.period[k] : slowest1;
            slowest2 = R2.period[k] > slowest2 ? R2.period[k] : slowest2;
        }
        const Stats a = stats(&R1, n / 2, n), b = stats(&R2, n / 2, n);
        printf("work growing with the time step (11.6 ms unpaced, 15.5 at 18.2): probes %.0f fps at the slowest, %.0f at the end "
               "(%lld changes); the frames' own measure %.0f fps at the slowest, %.0f at the end\n", 1e9 / slowest1,
               1e9 / R1.period[n - 1], (long long)R1.changes, 1e9 / slowest2, 1e9 / R2.period[n - 1]);
        print("probes", a, &R1);
        print("the frames' own measure", b, &R2);
        CHECK(slowest1 <= 1000000000LL / 72 && R1.period[n - 1] >= 1000000000LL / 80 && R1.changes <= 12);
        CHECK(slowest1 <= slowest2);   /* (never slower than the frames' own measure; tests/test_steady_period.py shows that one slide) */
    }
    /* (test31) the probes follow a scene getting heavier (within a second) and lighter (within a periodic probe) */
    {
        Cfg probed = {PACE_STEADY_ANY, FLOOR_PACED, 0, 240, 240, 1, 0, 0};
        int n = 0;
        while (total_ns(C1, n) < 30000 * MS) C1[n] = (int64_t)((6 + gauss() * 0.3) * MS), n++;
        const int heavy = n;
        for (int i = 0; i < 2500; i++) C1[n] = (int64_t)((14 + gauss() * 0.5) * MS), n++;
        const int light = n;
        for (int i = 0; i < 4000; i++) C1[n] = (int64_t)((6 + gauss() * 0.3) * MS), n++;
        run(C1, n, probed, &R1);
        const int down = next_change(&R1, heavy, n), up = next_change(&R1, light - 1, n);
        printf("probes: heavier (6 to 14 ms) lowered %.2f s after, to %.0f fps; lighter again raised %.1f s after, to %.0f fps\n",
               secs_between(&R1, heavy, down), 1e9 / R1.period[down], secs_between(&R1, light, up), 1e9 / R1.period[up]);
        CHECK(down < light && secs_between(&R1, heavy, down) <= 1.0 && R1.period[down] >= 14 * MS && R1.period[down] <= 16 * MS);
        CHECK(up < n && secs_between(&R1, light, up) <= 13.0 && R1.period[up] <= 7 * MS);
    }
    puts("ok");
}

/* ---- a recorded run: frames.csv's frame times, as each frame's work ---- */
static void recorded(const char *path, const char *name) {
    static int64_t c[MAXN];
    int n = 0;
    FILE *f = fopen(path, "r");
    CHECK(f);
    long long v;
    while (n < MAXN && fscanf(f, "%lld", &v) == 1) c[n++] = v;
    fclose(f);
    int from = n;   /* (the run's last 60 s, the stability script's window) */
    for (int64_t acc = 0; from > 0 && acc + c[from - 1] <= 60000 * MS; from--) acc += c[from - 1];
    static const struct { const char *name; Cfg c; } cfgs[] = {
        {"cap 240 as it is", {0, FLOOR_NONE, 0, 240, 240, 0, 0, 0}},
        {"cap 240, Frame pacing on", {0, FLOOR_SMOOTH, 0, 240, 240, 0, 0, 0}},
        {"steady, any rate (probes)", {PACE_STEADY_ANY, FLOOR_PACED, 0, 240, 240, 1, 0, 0}},
        {"steady, steps of 240 Hz", {PACE_STEADY_REFRESH, FLOOR_PACED, 0, 240, 240, 1, 0, 0}},
        {"steady, any, present thread", {PACE_STEADY_ANY, FLOOR_PACED, 1, 240, 240, 1, 0, 0}},
        {"steady start alone (no hold)", {PACE_STEADY_ANY, FLOOR_NONE, 0, 240, 240, 1, 0, 0}},
        {"steady, any, no probes", {PACE_STEADY_ANY, FLOOR_PACED, 0, 240, 240, 0, 0, 0}},
    };
    printf("%s (%d frames, the last %d):\n", name, n, n - from);
    Stats s[7];
    for (int i = 0; i < 7; i++) {
        run(c, n, cfgs[i].c, &R1);
        s[i] = stats(&R1, from, n);
        print(cfgs[i].name, s[i], &R1);
    }
    printf("RESULT %s %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n", name, s[0].fps, s[2].fps, s[0].jit, s[2].jit, s[0].over15, s[2].over15,
           s[0].mismatch, s[2].mismatch);
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "trace")) recorded(argv[2], argv[3]);
    else synthetic();
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
    check('#include "pace_steady.h"' in vi, "host/nn_vi.c does not include host/pace_steady.h")
    steady = block(vi, "static int64_t steady_period(int64_t base, int64_t now) {")
    check('getenv("HOS_FPS_STEADY")' in steady and 'hos_ini_option("steady_frame_rate", PACE_STEADY_OFF)' in steady,
          "steady_period does not read HOS_FPS_STEADY over the row's steady_frame_rate")
    check("if (mode == PACE_STEADY_OFF) return base;" in steady, "the row off does not leave the cap's period as before")
    check("pace_steady_measure(interval, own + held, other, &took, &need);" in steady and "pace_steady_feed(&steady, took, need, interval)" in steady,
          "what a frame took and needed is not its present interval without the acquire's waits and the present's hold")
    check("presenting_thread = 1;" in steady and "__atomic_exchange_n(&frame_waited_other_ns, 0, __ATOMIC_RELAXED)" in steady,
          "the acquire's waits on other threads are not told apart from the presenting thread's")
    check("steady_line(&window, &steady, now);" in steady, "the 5 s line of what the rule was given is not written")
    check("rx_option(HOS_OPT_PACE_STEADY, 0)" in steady, "the present's hold is not asked of the renderer")
    hand = steady[steady.index("if (handed) {"):]
    check(hand.index("pace_steady_reset(&steady)") < hand.index("return base;") and "rx_option_set(HOS_OPT_PACE_STEADY, 0);" in hand,
          "a frame another limiter held does not put the cap's period back")
    check("pace_steady_period(&steady) > base ? pace_steady_period(&steady) : base" in steady, "the steady period may be shorter than the cap's")
    check(steady.index("rx_option_set(HOS_OPT_PACE_PERIOD, (int)p);") < steady.index("rx_option_set(HOS_OPT_PACE_STEADY, 1);"),
          "the renderer is not told the period before it is told it is the steady one")
    sleep = block(vi, "static void pace_sleep(int64_t until, int64_t now) {")
    check("if (presenting_thread) frame_waited_own_ns += after - now;" in sleep
          and "else __atomic_fetch_add(&frame_waited_other_ns, (LONG64)(after - now), __ATOMIC_RELAXED);" in sleep,
          "the acquire's wait is not counted out of what the frame took, by its thread")
    check("if (held) __atomic_store_n(&frame_handed, 1, __ATOMIC_RELAXED);" in block(vi, "static int pace_handed(void) {"),
          "a frame another limiter held is not noted")
    presented = block(vi, "void hos_vi_native_presented(void) {")
    order = [presented.index(s) for s in ("const int64_t period = steady_period(1000000000LL / fps_max() * interval, now);",
                                          "pace_period_ns = period;", "realtime_step(now);", "shown_at[n & 15] = last + period > now")]
    check(order == sorted(order), "the presents are not paced by the steady period, or the steady step does not see it")
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    check("enum { HOS_OPT_PACE_PERIOD = 85, HOS_OPT_PACE_START, HOS_OPT_PACE_MOVIE, HOS_OPT_PACE_STEADY };" in api,
          "HOS_OPT_PACE_STEADY is not 88, in pacing's ids")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    check("if (id == HOS_OPT_PACE_STEADY) return pace::steady_held_us();" in replay
          and "if (id == HOS_OPT_PACE_STEADY) return pace::set_steady(value);" in replay, "the renderer does not answer HOS_OPT_PACE_STEADY")
    wanted = block(replay, "bool smooth_wanted() {")
    check("smooth_mine && !framegen_on() && !dlssg::active() && ((on && opts.pace != 0) || pace::steady_floor_ns() > 0)" in wanted,
          "the steady frame rate's hold is not for the game's own pictures alone, without frame generation")
    sp = block(replay, "void smooth_present(std::unique_lock<std::mutex>& qk) {")
    check("present_smooth_paced_at(&smooth, now, paced)" in sp and "const int64_t paced = pace::steady_floor_ns();" in sp,
          "the present does not hold the pictures to the steady period")
    check(sp.index("smooth_wait_until(at);") < sp.index("if (paced) pace::steady_held(host->now_ns() - now);"),
          "the present's hold is not told to the pacing")
    pace = (ROOT / "gpu/vk/vk_pace.cpp").read_text(encoding="utf-8")
    floor = block(pace, "int64_t steady_floor_ns() {")
    check("!steady_on.load(std::memory_order_relaxed) || owner() != GAME" in floor, "the hold is not only while the game's own wait paces")
    check("presenting = true;" in block(pace, "int frame_start() {") and "if (presenting && ns > 0)" in block(pace, "void steady_held(int64_t ns) {"),
          "a hold on the present thread is counted as the presenting thread's")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    row = re.search(r'\{TAB_DISPLAY, "Steady frame rate", &opts\.steady_fps, 0, 2, 1, tx_steady_fps,(.*?)\n     nullptr, true, true\},', menu, re.S)
    check(row is not None, "the Display tab has no Steady frame rate row (Off, Any rate, Refresh steps; read at the start)")
    check(menu.index('"Game frame rate cap"') < menu.index('"Steady frame rate"') < menu.index('"Reflex frame limit"'),
          "the Steady frame rate row is not beside the Game frame rate cap")
    check('return v == 1 ? "Any rate" : v == 2 ? "Refresh steps" : "Off";' in block(menu, "const char* tx_steady_fps(int v) {"),
          "the row's values are not Off, Any rate, Refresh steps")
    check("opts.steady_fps" not in block(menu, "const char* ini_key(const Row& r) {"), "the row's key is not its name's, steady_frame_rate")
    check("int steady_fps = 0;" in (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8"), "the row is not off by default")
    check("| Steady frame rate | Off, Any rate, Refresh steps" in (ROOT / "readme.md").read_text(encoding="utf-8"),
          "readme.md's settings table has no Steady frame rate row")
    guide = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("| `steady_frame_rate` | 0 Off, 1 any rate" in guide and "| `HOS_FPS_STEADY=0`, `=1`, `=2` |" in guide,
          "docs/development.md has no rows for steady_frame_rate and HOS_FPS_STEADY")


def frame_times(run_dir):
    """A lab run's frame times (ns), the first five (the start) left out"""
    with open(os.path.join(run_dir, "frames.csv"), newline="") as f:
        return [int(float(r["ms"]) * 1e6) for r in csv.DictReader(f) if r.get("ms")][5:]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--lab", default=LAB)
    a = p.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="pace-steady-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture"
        c.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host"), "-I", str(ROOT / "gpu/vk"),
                        str(c), "-o", str(exe), "-lm"], check=True)
        out = subprocess.run([str(exe)], check=True, stdout=subprocess.PIPE, text=True).stdout
        print("\n".join(out.strip().splitlines()[:-1]))
        runs = [r for r in MARKET + TOURS if os.path.isfile(os.path.join(a.lab, r, "frames.csv"))]
        if not runs:
            print("(no recorded runs in %s: the lab's frame times are not replayed)" % a.lab)
        for name in runs:
            trace = Path(tmp) / (name + ".txt")
            trace.write_text("\n".join(str(v) for v in frame_times(os.path.join(a.lab, name))) + "\n", encoding="ascii")
            out = subprocess.run([str(exe), "trace", str(trace), name], check=True, stdout=subprocess.PIPE, text=True).stdout
            print(out.rstrip())
            res = [l.split() for l in out.splitlines() if l.startswith("RESULT ")][0]
            fps0, fps1, jit0, jit1, over0, over1, mis0, mis1 = map(float, res[2:])
            if name in MARKET:
                check(jit1 < jit0 * 0.65, "%s: the steady frame rate's jitter %.2f ms is not well under the cap's %.2f" % (name, jit1, jit0))
                check(over1 <= over0 + 0.5, "%s: more frames over 1.5x the median (%.1f against %.1f a thousand)" % (name, over1, over0))
                check(mis1 < mis0, "%s: the shown intervals are no nearer the game's time step" % name)
                check(fps1 > fps0 * 0.7, "%s: the steady frame rate gives up more than 30%% of the frame rate" % name)
    print("PASS: the steady frame rate holds a rate the frames fit in, follows the scene, ignores hitches, and evens the presents")


if __name__ == "__main__":
    main()
