"""The log's frame intervals against the paced period (host/nn_vi.c hos_vi_native_presented; no game).

The "vi: present intervals" buckets are made for 60 frames a second: at the owner's cap of 240 every interval is in the
first of them, and a change of the pacing shows nowhere. Every 5 s a "vi: frame intervals against the paced period" line now
counts them against the period the frames are paced by (under 0.75 of it, 0.75-0.95, 0.95-1.05, 1.05-1.25, 1.25-2, over 2)
and gives their mean distance from it.

Cuts hos_vi_native_presented out of host/nn_vi.c with stand-ins (as tests/test_present_trace.py does), presents 1,091 frames
at a cap of 240 whose intervals are known, and reads the line: each bucket's count, the mean distance, and the old line kept
as it was (the lab's log_digest.ps1 reads it).

    python -I -B tests/test_present_period_line.py [--cxx clang++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    match = re.search(r"^}", source[start:], re.M)
    return source[start:start + match.end()]


PREFIX = r'''
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef long long LONG64;
static volatile int present_interval = 1;
static int64_t next_frame_at, window_start, frames_in_window, shown_at[16];
static int64_t pace_period_ns;
static volatile LONG64 presents, shown_count;
static volatile uint64_t hos_mkw_pacer;
static uint32_t guest32;
static int64_t clock_now;
static char lines[16][600];
static int nlines;
#define G32(a) guest32
#define G64(a) ((uint64_t)0)
#define GP(a) ((void *)(uintptr_t)(a))
static int64_t InterlockedIncrement64(volatile LONG64 *p) { return ++*p; }
static int64_t hos_now_ns(void) { return clock_now; }
static void hos_sleep_ns(int64_t ns) { (void)ns; }
static void scan_dt(long long frame) { (void)frame; }
static void realtime_step(int64_t now) { (void)now; }
static void present_trace(int64_t frame, int64_t now) { (void)frame, (void)now; }
static void hos_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (nlines < 16) vsnprintf(lines[nlines++], sizeof lines[0], fmt, ap);
    va_end(ap);
}
static uint64_t hos_ticks(void) { return 0; }
static int fps_max(void) { return 240; }
static int64_t steady_period(int64_t base, int64_t now) { (void)now; return base; }   /* ((Triforce) the Steady frame rate row off) */
'''

SUFFIX = r'''
int main(void) {
    const int64_t P = 1000000000LL / 240;
    /* 1,091 intervals: 50 at 3 P, 100 at 1.5 P, 150 at 1.2 P, 200 at 0.9 P, 100 at 0.5 P and 491 at P, in that order, so
       that the 5 s are reached at the last of them */
    static const struct { int n; int64_t num, den; } mix[] = {{50, 3, 1}, {100, 3, 2}, {150, 6, 5}, {200, 9, 10}, {100, 1, 2}, {491, 1, 1}};
    int64_t total = 0, off = 0;
    clock_now = 1000000000LL;
    hos_vi_native_presented();
    for (unsigned m = 0; m < sizeof mix / sizeof *mix; m++)
        for (int i = 0; i < mix[m].n; i++) {
            const int64_t d = P * mix[m].num / mix[m].den;
            total += d, off += d > P ? d - P : P - d;
            clock_now += d;
            hos_vi_native_presented();
        }
    if (total < 5000000000LL || nlines != 3) return 90;   /* (one window: its three lines) */
    if (pace_period_ns != P) return 91;                   /* (the period it counts against is the one it paces by) */
    for (int i = 0; i < nlines; i++) puts(lines[i]);
    printf("expected %.3f\n", off / 1e6 / 1091);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    source = (ROOT / "host/nn_vi.c").read_text(encoding="utf-8")
    actual = block(source, "void hos_vi_native_presented(")
    with tempfile.TemporaryDirectory(prefix="present-period-") as tmp:
        cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        cpp.write_text(PREFIX + actual + "\n" + SUFFIX, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout
    period = re.search(r"vi: frame intervals against the paced period of ([\d.]+) ms: under 0.75 of it (\d+), 0.75-0.95 (\d+), "
                       r"0.95-1.05 (\d+), 1.05-1.25 (\d+), 1.25-2 (\d+), over 2 (\d+); ([\d.]+) ms from it on average", out)
    if not period:
        raise SystemExit("FAIL: no line against the paced period:\n" + out)
    counts = [int(period.group(i)) for i in range(2, 8)]
    if abs(float(period.group(1)) - 4.167) > 0.001 or counts != [100, 200, 491, 150, 100, 50]:
        raise SystemExit("FAIL: the line counts %s against %s ms, not [100, 200, 491, 150, 100, 50]" % (counts, period.group(1)))
    expected = float(re.search(r"expected ([\d.]+)", out).group(1))
    if abs(float(period.group(8)) - expected) > 0.001:
        raise SystemExit("FAIL: the mean distance is %s ms, not %.3f" % (period.group(8), expected))
    if not re.search(r"vi: present intervals: <15.5 ms 1091, 15.5-17.8 0, 17.8-22 0, 22-30 0, 30-40 0, over 40 0", out):
        raise SystemExit("FAIL: the 60 Hz buckets' line changed:\n" + out)
    print("PASS: the frame intervals are counted against the paced period, and the old line is as it was")


if __name__ == "__main__":
    main()
