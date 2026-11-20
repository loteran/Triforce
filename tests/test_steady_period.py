"""The Steady frame rate's host side as it runs in the game: host/nn_vi.c's own code, two threads, the renderer's hold (no game).

test26's lab runs (r28-st1-s1-1/2: the Market with HOS_FPS_STEADY=1, NVIDIA Reflex on + boost) never lowered the rate:
no "vi: steady frame rate: N fps (was ...)" line, every interval over twice the cap's 4.17 ms. OoT acquires its window
texture on its main thread (pace_sleep's "vi: frame waits" lines are sead::MainThread's) and presents on its Presentation
Thread (hos_vi_native_presented). In the title's light frames both waited most of each 4.2 ms: the acquire's wait on the
main thread and the present's hold on the Presentation Thread, and the interval less both made the frames "need" about
2 ms. The hitch rule measured each frame against the median of the frames kept, and an 11 ms Market frame was over 2.5
times that: every one a hitch, never kept, so the median never moved and nothing was ever late. tests/test_pace_steady.py
had replayed recorded intervals as the frames' work, where the title's frames counted their whole 4.2 ms, which hid it.

test27 (r31-st1-s1-1) lowered, but from the cap straight to 60 fps and never back up, where the Market's frames take 10.6
ms (11.7 at the 90th percentile; 93-96 fps without the row): its 5 s lines said took 10.66 / 11.66 ms, needed 14.63 /
16.23, the acquire's wait 5.8 ms on the main thread and the present's hold 0.3-0.5 ms. OoT's two threads take their
turns (the main thread waits in the acquire, builds the frame, the Presentation Thread replays and presents it, then the
main thread acquires again), and "needed", the interval less the smaller of the two threads' waits, counted the main
thread's wait as idle time beside the other thread's work: 16.2 ms, never room to raise. And the first lowering came
while the Market's first 40 frames, 15 to 20 ms after its load, were the measure.

test29 to test31 (r36, r38-st1-s1-1) held 45 to 60 fps where 72 to 80 held: paced at 55 fps (18.2 ms) the Market's frames
took 15.5 ms against 11.6 unpaced, the game's work growing with the period, and the frames' own measure chased the rate
down. The rate is now chosen by unpaced probes (host/pace_steady.h pace_steady_probed; HOS_FPS_STEADY_PROBE=0 the rule
before them).

This test runs host/nn_vi.c's own pace_handed, pace_sleep, hos_vi_native_acquire, steady_line, steady_period and
hos_vi_native_presented (cut out of the file, compiled with stand-ins for the clock, the sleeps, the log and a renderer that
holds the pictures as gpu/vk/present_smooth.h present_smooth_paced_at does, and answers HOS_OPT_PACE_STEADY with the hold)
on a virtual clock, the acquires on a thread of their own as OoT has them, and each scene a phase of frame times:

  - oot: as the lab's runs have it, the threads in turn: light frames for 10 s (the title, 1.3 ms of work), a load of 262 ms
    and 40 frames of 15 to 20 ms, the Market for 30 s (3 ms on the main thread and 7.6 on the Presentation Thread, 10.6 ms at
    the median and 11.7 at the 90th), light again for 30 s: lowered by the probe after the load has settled, to 80 fps or
    72 (not 60), at 80 at the end of the Market with two changes at most, raised to 120 fps or more within 20 s of the light
    frames;
  - oot-reflex: the same with NVIDIA Reflex's sleep in the present call (0.4 ms, not a wait of the pacing's);
  - grows: test31's Market, 11.6 ms a frame unpaced (12.2 at the 90th), its work growing 0.59 ms for each ms of time step
    past that (15.5 ms paced at 18.2, as measured): with the probes never under 72 fps from 10 s on, at 72 to 80 at the end;
  - grows-unprobed: the same with HOS_FPS_STEADY_PROBE=0: the slide, to 66 fps or under;
  - parallel: the threads side by side instead (the main thread up to two frames ahead, the Presentation Thread the slower
    at 10.5 ms): a rate between 55 and 90 fps at the Market's end, and a handful of changes in its 30 s, not one a second;
  - main-bound: side by side, the main thread the slower (10.5 ms against 3): lowered to a rate it holds, not on down;
  - one-thread: the acquire and the present on the same thread (the game's whole frame between them): lowered and raised;
  - and every run's 5 s line ("vi: steady frame rate: ... held; the last 5.0 s, ...") says what the rule was given.

    python -I -B tests/test_steady_period.py [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = ("oot", "oot-reflex", "grows", "grows-unprobed", "parallel", "main-bound", "one-thread")

PREFIX = r'''#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pace_wait.h"
#include "pace_steady.h"
#include "present_smooth.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
#define MS 1000000LL
typedef long long LONG64;
typedef long LONG;
@IDS@
/* ---- stand-ins: the clock and the sleeps (the virtual clock; one thread runs at a time) ---- */
static volatile int64_t clock_ns = 1000 * MS;
int64_t hos_now_ns(void) { return clock_ns; }
void hos_sleep_ns(int64_t ns) { if (ns > 0) clock_ns += ns; }
void hos_sleep_until_ns(int64_t until, int64_t margin) { (void)margin; if (until > clock_ns) clock_ns = until; }
static int lowered_lines, raised_lines, window_lines, limiter_lines;
static int held_fps = 240;   /* (the rate the rule holds, as its change lines say: a probe's frames at the cap are not a change) */
static void hos_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strstr(line, "vi: steady frame rate:")) {
        if (strstr(line, " held; the last ")) window_lines++;
        if (strstr(line, "another limiter")) limiter_lines++;
        if (strstr(line, " (was ")) {
            double now_fps, was_fps;
            if (sscanf(strstr(line, "rate: ") + 6, "%lf fps (was %lf", &now_fps, &was_fps) == 2) {
                if (now_fps < was_fps) lowered_lines++;
                else raised_lines++;
                held_fps = (int)(now_fps + 0.5);
            }
        }
        printf("LOG %s\n", line);
    }
}
/* ---- the renderer: the period and the steady flag as told, its hold of the pictures, Reflex's sleep ---- */
static int64_t told_period;
static int steady_told;
static int64_t held_ns;
static PresentSmooth smooth;
static int64_t reflex_sleep_ns;
int rx_option(int id, int otherwise) {
    if (id == HOS_OPT_PACE_START) return 0;   /* (the game's own wait: Reflex holds no limit with the Reflex frame limit row off) */
    if (id == HOS_OPT_PACE_STEADY) { const int us = (int)(held_ns / 1000); held_ns -= us * 1000LL; return us; }
    if (id == HOS_OPT_DISPLAY_CAP) return 240;
    return otherwise;
}
void rx_option_set(int id, int value) {
    if (id == HOS_OPT_PACE_PERIOD) told_period = value;
    if (id == HOS_OPT_PACE_STEADY) steady_told = value == 1;
}
/* the present of a picture ready now (vk_replay.cpp smooth_present, the present thread off: on the presenting thread) */
static void renderer_present(void) {
    const int64_t paced = steady_told ? told_period : 0, ready = clock_ns;
    const int64_t at = paced ? present_smooth_paced_at(&smooth, ready, paced) : ready;
    if (at > ready) held_ns += at - ready, clock_ns = at;
    present_smooth_shown(&smooth, clock_ns);
    clock_ns += reflex_sleep_ns;   /* (do_present's Reflex sleep on the way out: not a wait of the pacing's) */
}
static int mode_asked = 1;
int hos_ini_option(const char *key, int otherwise) { return strcmp(key, "steady_frame_rate") ? otherwise : mode_asked; }
static int fps_max(void) { return 240; }
/* ---- host/nn_vi.c's state these functions use, and the rest of hos_vi_native_presented's stand-ins ---- */
static volatile LONG64 presents;
static int64_t next_frame_at, window_start, frames_in_window;
static int64_t shown_at[16];
static volatile LONG64 shown_count;
static volatile int present_interval = 1;
static volatile uint64_t hos_mkw_pacer;
static uint32_t guest32;
#define G32(a) guest32
#define G64(a) ((uint64_t)0)
#define GP(a) ((void *)(uintptr_t)(a))
static int64_t InterlockedIncrement64(volatile LONG64 *p) { return ++*p; }
static uint64_t hos_ticks(void) { return 0; }
static void scan_dt(long long frame) { (void)frame; }
static void realtime_step(int64_t now) { (void)now; }
static void present_trace(int64_t frame, int64_t now) { (void)frame, (void)now; }
'''

SUFFIX = r'''
/* ---- the game's two threads on the virtual clock ---- */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int command, done_flag;   /* (1: an acquire on the main thread; 2: stop) */
static void *main_thread(void *p) {
    (void)p;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!command) pthread_cond_wait(&cv, &mu);
        const int c = command;
        command = 0;
        pthread_mutex_unlock(&mu);
        if (c == 2) return NULL;
        hos_vi_native_acquire(3);
        pthread_mutex_lock(&mu);
        done_flag = 1;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
}
static void acquire_on_main(void) {
    pthread_mutex_lock(&mu);
    done_flag = 0, command = 1;
    pthread_cond_broadcast(&cv);
    while (!done_flag) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}

#define N 20000
static int64_t M[N], R[N], out_at[N];
static int rate_at[N];   /* (the rate held at each frame) */
static int phase_of[N];
static uint32_t seed = 5;
static double uniform(void) { seed = seed * 1103515245u + 12345u; return ((seed >> 8) & 0xffffff) / 16777216.0; }
static double gauss(void) { double s = 0; for (int i = 0; i < 12; i++) s += uniform(); return s - 6; }
/* the phases: {seconds, main thread ms, Presentation Thread ms, the swing's ms} */
typedef struct { double secs, m, r, sd; } Phase;
static int make(const Phase *ph, int nph) {
    int n = 0;
    double slow = 0;
    for (int p = 0; p < nph; p++) {
        double t = 0;
        while (t < ph[p].secs * 1000 && n < N) {
            slow = 0.95 * slow + gauss() * ph[p].sd * 0.31;
            const double m = ph[p].m * (1 + gauss() * 0.05), r = ph[p].r + slow + gauss() * ph[p].sd * 0.4;
            M[n] = (int64_t)((m < 0.2 ? 0.2 : m) * MS), R[n] = (int64_t)((r < 0.2 ? 0.2 : r) * MS), phase_of[n] = p;
            t += (ph[p].m > ph[p].r ? ph[p].m : ph[p].r) > 4.17 ? (ph[p].m > ph[p].r ? ph[p].m : ph[p].r) : 4.17;
            n++;
        }
    }
    return n;
}
/* OoT: the main thread acquires (on a thread of its own) and builds frame k (M) once the Presentation Thread is done with frame
   k - 2; the Presentation Thread replays it (R) once it has it, then presents. Events in the clock's order */
static void run_two(int n) {
    pthread_t th;
    pthread_create(&th, NULL, main_thread, NULL);
    static int64_t handed[N], pres_done[N];
    int km = 0, kp = 0;
    int64_t main_free = clock_ns, pres_free = clock_ns;
    while (kp < n) {
        int64_t ta = INT64_MAX, tb = INT64_MAX;
        if (km < n && km <= kp + 1) ta = main_free > (km >= 2 ? pres_done[km - 2] : 0) ? main_free : pres_done[km - 2];
        if (kp < km) {
            const int64_t start = pres_free > handed[kp] ? pres_free : handed[kp];
            tb = start + R[kp];   /* (its present: the hold and the host's present follow) */
        }
        if (ta <= tb) {
            clock_ns = ta;
            acquire_on_main();
            handed[km] = clock_ns + M[km];
            main_free = handed[km];
            km++;
        } else {
            clock_ns = tb;
            renderer_present();
            out_at[kp] = clock_ns;
            hos_vi_native_presented();
            rate_at[kp] = held_fps;
            pres_done[kp] = pres_free = clock_ns;
            kp++;
        }
    }
    pthread_mutex_lock(&mu);
    command = 2;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    pthread_join(th, NULL);
}
/* one thread: acquire, the whole frame, present */
static void run_one(int n) {
    for (int k = 0; k < n; k++) {
        hos_vi_native_acquire(3);
        clock_ns += M[k] + R[k];
        renderer_present();
        out_at[k] = clock_ns;
        hos_vi_native_presented();
        rate_at[k] = held_fps;
    }
}
/* OoT as the lab's runs show it: the threads in turn. The main thread acquires (on a thread of its own) and builds the frame
   (M), the Presentation Thread replays it (R) and presents, and only then the main thread acquires again */
/* ((test31) grow: a frame's work grows by this much of its time step past grow_from, the time step being its frame's
   interval before: the lab's Market took 11.6 ms unpaced and 15.5 paced at 55 fps, 18.2 ms) */
static double grow;
static int64_t grow_from;
static void run_serial(int n) {
    pthread_t th;
    pthread_create(&th, NULL, main_thread, NULL);
    for (int k = 0; k < n; k++) {
        acquire_on_main();
        const int64_t step = k >= 2 ? out_at[k - 1] - out_at[k - 2] : 0;
        clock_ns += M[k] + R[k] + (step > grow_from ? (int64_t)(grow * (step - grow_from)) : 0);
        renderer_present();
        out_at[k] = clock_ns;
        hos_vi_native_presented();
        rate_at[k] = held_fps;
    }
    pthread_mutex_lock(&mu);
    command = 2;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    pthread_join(th, NULL);
}
static int first_of(int n, int phase) { for (int k = 0; k < n; k++) if (phase_of[k] == phase) return k; return n; }
static double secs(int64_t a, int64_t b) { return (b - a) / 1e9; }
static int changes_in(int a, int b) { int c = 0; for (int k = a + 1; k < b; k++) c += rate_at[k] != rate_at[k - 1]; return c; }

int main(int argc, char **argv) {
    const char *c = argc > 1 ? argv[1] : "oot";
    if (!strcmp(c, "oot") || !strcmp(c, "oot-reflex")) {
        /* test27's Market (r31-st1-s1-1): the title's 1.3 ms frames, a load of 262 ms and 40 frames of 15 to 20 ms, then frames
           of 10.6 ms at the median and 11.7 at the 90th (3 ms on the main thread, 7.6 on the Presentation Thread) */
        static const Phase ph[] = {{10, 0.5, 0.8, 0.05}, {0.59, 3.0, 14.5, 0.0}, {30, 3.0, 7.6, 0.8}, {30, 0.5, 0.8, 0.05}};
        const int n = make(ph, 4);
        const int arrive = first_of(n, 1), market = first_of(n, 2), light = first_of(n, 3);
        R[arrive] = 262 * MS;   /* (the load) */
        for (int k = arrive + 1; k < market; k++) R[k] = (int64_t)((12 + 5 * uniform()) * MS);   /* (15 to 20 ms in all) */
        if (!strcmp(c, "oot-reflex")) {   /* (the lab's figures are with Reflex on: its sleep is part of the 10.6 ms) */
            reflex_sleep_ns = 400000;
            for (int k = 0; k < n; k++) R[k] = R[k] > 2 * reflex_sleep_ns ? R[k] - reflex_sleep_ns : R[k];
        }
        run_serial(n);
        static int64_t took[N];
        int nt = 0;
        for (int k = market; k < light; k++) took[nt++] = M[k] + R[k] + reflex_sleep_ns;
        CHECK(rate_at[arrive - 1] == 240);
        int down = arrive;
        while (down < light && rate_at[down] == 240) down++;
        int up = light;
        while (up < n && rate_at[up] < 120) up++;
        printf("%s: the Market's work %.2f / %.2f ms (p50 / p90); lowered %.2f s after its load, to %d fps; %d fps at its end, "
               "%d changes in its 30 s; raised to %d fps %.1f s after the light frames; %d lowerings, %d raises\n", c,
               pace_steady_select(took, nt, nt / 2) / 1e6, pace_steady_select(took, nt, nt * 9 / 10) / 1e6, secs(out_at[arrive], out_at[down]),
               down < light ? rate_at[down] : 0, rate_at[light - 1], changes_in(down, light), up < n ? rate_at[up] : 0,
               up < n ? secs(out_at[light], out_at[up]) : -1, lowered_lines, raised_lines);
        CHECK(down < light && secs(out_at[arrive], out_at[down]) <= 4.0);   /* (the 2 s settling, then a probe) */
        CHECK(rate_at[down] == 80 || rate_at[down] == 72);                    /* (to 80 fps or the next, not 60: the probe after the load) */
        CHECK(rate_at[light - 1] == 80 && changes_in(down, light) <= 2);     /* (held there) */
        CHECK(up < n && secs(out_at[light], out_at[up]) <= 20.0);
        CHECK(window_lines >= 12 && !limiter_lines);
        return 0;
    }
    if (!strcmp(c, "grows") || !strcmp(c, "grows-unprobed")) {
        /* (test31: r38-st1-s1-1) the Market at 11.6 ms a frame unpaced (12.0 at the 90th), its work growing with the time
           step: 15.5 ms paced at 55 fps (18.2 ms), as the lab measured. The probes hold it near 80 fps for the whole 60 s;
           the frames' own measure (HOS_FPS_STEADY_PROBE=0) slides down, as test29 to test31 did */
        if (!strcmp(c, "grows-unprobed")) putenv((char *)"HOS_FPS_STEADY_PROBE=0");
        static const Phase ph[] = {{10, 0.5, 0.8, 0.05}, {0.59, 3.0, 14.5, 0.0}, {60, 3.0, 8.6, 0.37}};
        const int n = make(ph, 3);
        const int arrive = first_of(n, 1), market = first_of(n, 2);
        R[arrive] = 262 * MS;
        for (int k = arrive + 1; k < market; k++) R[k] = (int64_t)((12 + 5 * uniform()) * MS);
        grow = 0.59, grow_from = 116 * MS / 10;
        run_serial(n);
        static int64_t took[N];
        int nt = 0, lowest = 240, below72 = 0;
        for (int k = market; k < n; k++) took[nt++] = M[k] + R[k];
        int settled = market;
        while (settled < n && secs(out_at[market], out_at[settled]) < 10) settled++;
        for (int k = settled; k < n; k++) lowest = rate_at[k] < lowest ? rate_at[k] : lowest, below72 += rate_at[k] < 72;
        printf("%s: the Market's work unpaced %.2f / %.2f ms (p50 / p90), growing 0.59 ms a ms of time step past 11.6; from 10 s on "
               "%d fps at the lowest, %d fps at the end, %d frames under 72 fps; %d changes in its 60 s; %d lowerings, %d raises\n", c,
               pace_steady_select(took, nt, nt / 2) / 1e6, pace_steady_select(took, nt, nt * 9 / 10) / 1e6, lowest, rate_at[n - 1],
               below72, changes_in(market, n), lowered_lines, raised_lines);
        if (!strcmp(c, "grows-unprobed")) CHECK(lowest <= 66);   /* (the slide this commit stops) */
        else CHECK(lowest >= 72 && rate_at[n - 1] >= 72 && rate_at[n - 1] <= 80 && changes_in(settled, n) <= 4);
        return 0;
    }
    if (!strcmp(c, "main-bound")) {
        static const Phase ph[] = {{10, 0.8, 1.2, 0.1}, {40, 10.5, 3.0, 0.2}};
        const int n = make(ph, 2);
        for (int k = 0; k < n; k++) if (phase_of[k] == 1) M[k] = (int64_t)((10.5 + gauss() * 0.6) * MS);
        run_two(n);
        const int heavy = first_of(n, 1);
        int down = heavy;
        while (down < n && rate_at[down] == 240) down++;
        printf("main-bound: lowered %.2f s after the heavy frames, to %d fps; at the end %d fps; %d lowerings, %d raises\n",
               secs(out_at[heavy], out_at[down]), rate_at[down], rate_at[n - 1], lowered_lines, raised_lines);
        CHECK(down < n && secs(out_at[heavy], out_at[down]) <= 2.5);
        CHECK(rate_at[n - 1] <= 95 && rate_at[n - 1] >= 55);   /* (fits, and not on down) */
        CHECK(lowered_lines <= 4 && window_lines >= 8 && !limiter_lines);
        return 0;
    }
    /* parallel (side by side, the Presentation Thread the slower) and one-thread: no load before the heavy frames */
    static const Phase ph[] = {{10, 0.8, 1.2, 0.1}, {30, 4.0, 10.5, 1.0}, {30, 0.8, 1.2, 0.1}};
    const int n = make(ph, 3);
    if (!strcmp(c, "one-thread")) run_one(n);
    else run_two(n);
    const int heavy = first_of(n, 1), light = first_of(n, 2);
    CHECK(rate_at[heavy - 1] == 240);   /* (the title's light frames: the cap) */
    int down = heavy;
    while (down < light && rate_at[down] == 240) down++;
    int up = light;
    while (up < n && rate_at[up] < 120) up++;
    printf("%s: lowered %.2f s after the heavy frames, to %d fps; %d fps at their end, %d changes in their 30 s; raised to %d fps "
           "%.1f s after the light frames; %d lowerings, %d raises\n", c, secs(out_at[heavy], out_at[down]),
           down < light ? rate_at[down] : 0, rate_at[light - 1], changes_in(down, light), up < n ? rate_at[up] : 0,
           up < n ? secs(out_at[light], out_at[up]) : -1, lowered_lines, raised_lines);
    CHECK(down < light && secs(out_at[heavy], out_at[down]) <= 2.5);   /* (frames late, a probe 2 s after the last at most) */
    CHECK(rate_at[light - 1] <= 90 && rate_at[light - 1] >= 55);
    CHECK(changes_in(down, light) <= 8);
    CHECK(up < n && secs(out_at[light], out_at[up]) <= 20.0);
    CHECK(window_lines >= 12 && !limiter_lines);
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


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    vi = (ROOT / "host/nn_vi.c").read_text(encoding="utf-8")
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    ids = re.search(r"enum \{ HOS_OPT_PACE_PERIOD = 85, HOS_OPT_PACE_START, HOS_OPT_PACE_MOVIE, HOS_OPT_PACE_STEADY \};", api)
    cap = re.search(r"enum \{ HOS_OPT_DISPLAY_CAP = \d+ \};", api)
    if not ids or not cap:
        raise SystemExit("FAIL: gpu/vk/vk_api.h no longer has the pacing ids and HOS_OPT_DISPLAY_CAP as this test knows them")
    region = vi[vi.index("static int64_t pace_period_ns;"):vi.index("void hos_vi_native_acquire(int active) {")]
    steady = vi[vi.index("typedef struct {   /* (what steady_line says"):]
    steady = steady[:steady.index("static int64_t steady_period(int64_t base, int64_t now) {")] + block(steady, "static int64_t steady_period(")
    presented = vi[vi.index("void hos_vi_native_presented(void) {"):]
    presented = presented[:re.search(r"^}", presented, re.M).end()]
    code = "\n".join((region, block(vi, "void hos_vi_native_acquire(int active) {"), steady, presented))
    source = PREFIX.replace("@IDS@", ids.group(0) + "\n" + cap.group(0)) + code + SUFFIX
    env = {k: v for k, v in os.environ.items() if not k.startswith(("HOS_", "RX_"))}
    with tempfile.TemporaryDirectory(prefix="steady-period-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture"
        c.write_text(source, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu17", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
                        "-pthread", "-I", str(ROOT / "host"), "-I", str(ROOT / "gpu/vk"), str(c), "-o", str(exe), "-lm"], check=True)
        for s in SCENARIOS:
            r = subprocess.run([str(exe), s], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
            lines = r.stdout.splitlines()
            shown = [l for l in lines if not l.startswith("LOG ")]
            logs = [l[4:] for l in lines if l.startswith("LOG ")]
            print("\n".join(shown))
            # (each change, in short: when it came and to what)
            changes = [re.search(r"rate: ([\d.]+) fps \(was ([\d.]+)\)", l) for l in logs if " (was " in l]
            print("  changes: " + ", ".join("%s>%s" % (m.group(2), m.group(1)) for m in changes if m))
            # (the 5 s lines of the heavy phase, as the lab will read them)
            windows = [l for l in logs if " held; the last " in l]
            if windows:
                print("  e.g. " + windows[min(3, len(windows) - 1)])
            if r.returncode:
                raise SystemExit("FAIL: %s (exit %d)\n%s" % (s, r.returncode, "\n".join(logs[-6:])))
    print("PASS: host/nn_vi.c's steady frame rate lowers in a heavy scene after light ones, on OoT's two threads and on one")


if __name__ == "__main__":
    main()
