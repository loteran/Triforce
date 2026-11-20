/* (Triforce, after NX2navi's runtime/frame_rate.c ocarina_step and its notes, chrg6's: the rule is copied) The game's
   time step kept steady while it follows the clock (host/nn_vi.c realtime_step, with HOS_STEP_STEADY=1). Plain C with no
   system call, so that tests/test_step_steady.py runs it on display intervals of its own.

   The step is the mean of the last five present intervals (HOS_REALTIME_WINDOW): it moves with every one of them. A
   frame of 50 ms at a cap of 240 put the next five steps at 13 ms, three periods, so the game caught all of that frame's
   time up within 21 ms, a lurch after every hitch; intervals of 5 and 30 ms in turn gave steps of 15 and 20 ms in turn
   (NX2navi, the same game). Nothing in the game was written for a step that changes from frame to frame: its physics
   takes the step as the length of the next solve while the bodies' velocities come from the step before. So:
     - the base is the mean of the last 12 intervals without their longest and shortest: one late frame and the short
       one after it leave it alone, intervals alternating in a period of 2, 3 or 4 frames give one value, and a real
       change of frame rate arrives within 12 frames;
     - a base within 3% of the period the frames are paced by is that period: at a cap the game holds, the step is the
       same number every frame (at 60, exactly the one frame the game ships with);
     - what the display showed and the game was not given is owed (the late frame cut from the base, the 3% of the snap).
       Past half a step of it the step repays a 64th of the excess a frame, 10% of the step at most, so the game still
       follows the clock and a hitch is paid back over the next second or so rather than in five frames. More than a
       quarter second owed either way is dropped: the game runs slower than the clock, as it does for a longer frame.
   The step stays within the caller's bounds, and what the bounds took or gave is owed too. */
#pragma once
#include <math.h>

#define STEP_STEADY_WINDOW 12        /* (the intervals the base is the mean of, but for their longest and shortest) */
#define STEP_STEADY_SNAP 0.03        /* (a base this near the paced period is the period) */
#define STEP_STEADY_REPAY_MAX 0.10   /* (of the step: the most a frame repays) */
#define STEP_STEADY_OWED_MAX 0.25    /* (seconds owed either way at most) */

typedef struct {
    double ring[STEP_STEADY_WINDOW];
    double owed;                     /* (seconds the display showed and the game was not given; under 0: given ahead) */
    unsigned count, next;
} StepSteady;

/* A load, a debugger stop, a movie's start: the intervals before say nothing about the next */
static inline void step_steady_reset(StepSteady *s) { s->count = s->next = 0, s->owed = 0; }

/* The step for a display interval dt (seconds, over 0): period, the period the frames are paced by (0: none known); lo and
   hi, the step's bounds */
static inline double step_steady(StepSteady *s, double dt, double period, double lo, double hi) {
    if (dt > hi) dt = hi;
    s->ring[s->next++ % STEP_STEADY_WINDOW] = dt;
    if (s->count < STEP_STEADY_WINDOW) s->count++;
    double sum = 0, least = s->ring[0], most = s->ring[0];
    for (unsigned i = 0; i < s->count; i++) {
        sum += s->ring[i];
        if (s->ring[i] < least) least = s->ring[i];
        if (s->ring[i] > most) most = s->ring[i];
    }
    double base = s->count >= 4 ? (sum - least - most) / (s->count - 2) : sum / s->count;
    if (period > 0 && fabs(base - period) <= STEP_STEADY_SNAP * period) base = period;
    const double over = fabs(s->owed) - base * 0.5;
    double repay = 0;
    if (over > 0) {
        repay = over / 64 < STEP_STEADY_REPAY_MAX * base ? over / 64 : STEP_STEADY_REPAY_MAX * base;
        if (s->owed < 0) repay = -repay;
    }
    double step = base + repay;
    if (step < lo) step = lo;
    if (step > hi) step = hi;
    s->owed += dt - step;
    if (s->owed > STEP_STEADY_OWED_MAX) s->owed = STEP_STEADY_OWED_MAX;
    if (s->owed < -STEP_STEADY_OWED_MAX) s->owed = -STEP_STEADY_OWED_MAX;
    return step;
}
