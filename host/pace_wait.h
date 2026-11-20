/* (Triforce, after NX2navi's runtime/nvn_api_obj.c sleep_until: its behaviour, not its code) The game's frame wait to
   its time, without the timer's lateness (host/nn_vi.c hos_vi_native_acquire, hos_thread.c hos_sleep_until_ns). Plain C
   and no system call, so that tests/test_pace_wait.py runs it against a clock and a timer of its own.

   The frame wait slept on the high-resolution waitable timer for the whole wait (hos_sleep_ns), and that timer never
   wakes early but wakes up to about 0.5 ms late (hos_thread.c sleep_ns; NX2navi measured 0.3 to 0.5 ms on Windows). The
   frame then started late by that much, and since the pacing keeps its phase the frame after it came that much sooner:
   at a Frame rate cap of 240 a period is 4.17 ms, and 0.5 ms is an eighth of it, one frame long, the next short. So the
   timer now takes the wait up to a margin before the frame's time, the margin the timer may be late plus 0.1 ms, and the
   rest is yielded away on the clock (SwitchToThread, as hos_sleep_ns does for waits under a millisecond), as NX2navi's
   pacer and Triforce's own frame generation display thread (gpu/vk/vk_framegen.cpp display_main) wait. The yielding
   costs at most the margin a frame, and a millisecond more where the whole wait is shorter than the timer's floor and
   the margin together; a frame that is late already waits for nothing, as before.
   HOS_PACE_SPIN=0: the timer for the whole wait, as before; =<microseconds>: another margin. */
#pragma once
#include <stdint.h>

#define PACE_WAIT_MARGIN_NS 600000LL       /* (the timer's lateness, 0.5 ms, and 0.1 ms) */
#define PACE_WAIT_TIMER_MIN_NS 1000000LL   /* (hos_thread.c sleep_ns: under a millisecond it yields, it takes no timer) */

/* The part of a wait with left_ns to go to take on the timer: all of it but the margin, where that is a wait the timer
   takes (at least PACE_WAIT_TIMER_MIN_NS); 0: the rest is yielded away */
static inline int64_t pace_wait_timed(int64_t left_ns, int64_t margin_ns) {
    if (margin_ns < 0) margin_ns = 0;
    return left_ns - margin_ns >= PACE_WAIT_TIMER_MIN_NS ? left_ns - margin_ns : 0;
}

/* Waits until the clock reads `until`: the timer's part (timer(ns), never shorter than PACE_WAIT_TIMER_MIN_NS), then
   yield() until the time. A timer that woke later than the margin leaves nothing to yield: the wait ends then, never
   later than the timer alone would have ended it */
static inline void pace_wait_until(int64_t until, int64_t margin_ns, int64_t (*now)(void), void (*timer)(int64_t),
                                   void (*yield)(void)) {
    for (;;) {
        const int64_t left = until - now();
        if (left <= 0) return;
        const int64_t timed = pace_wait_timed(left, margin_ns);
        if (timed) timer(timed);
        else yield();
    }
}
