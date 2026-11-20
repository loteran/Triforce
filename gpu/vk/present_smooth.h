// (Triforce, after NX2navi's runtime/vk_backend.c pace_smooth: its behaviour, not its code, whose lines are epicmeme546's)
// The Frame pacing row on the plain present (vk_replay.cpp present_image), with RX_VK_PACE_SMOOTH=1: a picture of the
// game's goes out no sooner than seven eighths of the recent interval after the one before it. Plain C with no system
// call, so that tests/test_present_smooth.py runs it on frame times of its own (as C and as C++).
//
// The row ("When each frame is shown ... At an even interval: less stutter") only ever spaced the frames of frame
// generation's display thread (vk_framegen.cpp display_main, RX_VK_PACE); without frame generation every picture went out
// the moment the replay finished it. The game hands its frames over unevenly although each costs about the same: 12 to 28
// ms apart around a mean of 19 when the display thread was written, 13 to 31 ms around 22 in a village in NX2navi (the same
// game), and shown as they come that is a judder at a rate that would look even. Here only a picture that comes early
// waits, until seven eighths of the recent interval after the last one; a late one goes at once, and the rate stays what it
// was but for the early ones' few milliseconds. The recent interval follows each picture's by a quarter, so it follows a
// scene that gets lighter within about a second; an interval of PRESENT_SMOOTH_HITCH_NS or more, or more than three times
// the recent one (a load, a hitch; twice would take the village's 13/31 ms frames for hitches), is not the rate and is left out of it.
//
// ((Triforce, 11-16) A picture waits at most PRESENT_SMOOTH_MAX_NS (8 ms) and at most half the recent interval, and where
// the floor is further than that it waits that long, not to the floor. The owner's test16 held a picture 41 ms at the start
// (frames 60 to 70 ms apart, under the 100 ms hitch line, had become the recent interval, and the old bound was 50 ms) and
// up to 12 ms while their save loaded; in play the waits were 0.3 to 1.7 ms on average, which this leaves as they were.)
//
// On by default since the lab's A/B (vk_replay.cpp smooth_wanted; RX_VK_PACE_SMOOTH=0 off). The present was on the game's
// presenting thread here (NX2navi's has a present thread of its own), so a picture's wait also held the start of the
// game's next frame on that thread: hence the bounds. ((Triforce) With the present thread, RX_VK_PRESENT_THREAD (present_thread.h),
// the wait is that thread's, and the presenting thread waits only where its next picture comes before this one is out.)
#pragma once
#include <stdint.h>

#define PRESENT_SMOOTH_HITCH_NS 100000000ll   // an interval this long or longer is a load or a hitch, not the rate
#define PRESENT_SMOOTH_MAX_NS 8000000ll       // no picture is held longer than this (nor longer than half the recent interval)

typedef struct {
    int64_t avg_ns;    // the recent interval, 0 before the second picture
    int64_t last_ns;   // when the last picture went out, 0 before the first (and after present_smooth_reset)
} PresentSmooth;

// The time a picture ready at now goes out: now, or toward the floor (seven eighths of the recent interval after the last
// picture) where that is later, waiting at most PRESENT_SMOOTH_MAX_NS and half the recent interval
static inline int64_t present_smooth_at(const PresentSmooth* s, int64_t now) {
    if (!s->last_ns || s->avg_ns <= 0) return now;
    const int64_t soonest = s->last_ns + s->avg_ns / 8 * 7;
    if (soonest <= now) return now;
    int64_t most = s->avg_ns / 2;
    if (most > PRESENT_SMOOTH_MAX_NS) most = PRESENT_SMOOTH_MAX_NS;
    return soonest - now <= most ? soonest : now + most;
}

// ((Triforce) The Steady frame rate row, host/pace_steady.h) With the game's frames paced to a period they fit in (paced_ns,
// over 0), the floor is that period after the last picture less a 64th of it, in place of seven eighths of the recent interval:
// the frames start a period apart, but each one's work after its start still swings, and each picture going out as it is
// done leaves the presents as uneven as the work (the lab's Market: 10.6 ms frames, a millisecond either way). Held to the
// period, a picture goes out a period after the one before however early its frame was done; the 64th lets the pictures come
// back toward their frames' ends by that much a frame after a slow one, so that the delay it added does not stay. A late
// picture goes at once, as before, and none waits more than PRESENT_SMOOTH_MAX_NS or half the period. paced_ns 0:
// present_smooth_at
static inline int64_t present_smooth_paced_at(const PresentSmooth* s, int64_t now, int64_t paced_ns) {
    if (paced_ns <= 0) return present_smooth_at(s, now);
    if (!s->last_ns) return now;
    const int64_t soonest = s->last_ns + paced_ns - paced_ns / 64;
    if (soonest <= now) return now;
    int64_t most = paced_ns / 2;
    if (most > PRESENT_SMOOTH_MAX_NS) most = PRESENT_SMOOTH_MAX_NS;
    return soonest - now <= most ? soonest : now + most;
}

// A picture went out at t: the recent interval moves a quarter of the way to its interval, an interval counted at most as one
// and a half times the recent one. ((Triforce, 11-16) The lab's Market after a tour of areas had re-upload bursts in about 13%
// of its frames (18-26 ms against 10.4); counted whole they pulled the recent interval to 15-18 ms, and 75% of the frames then
// waited 2.4-2.8 ms for a floor the steady frames never needed (~70 fps where ~80 was possible: triforce-frame-stability.md).
// Counted at most 1.5x, such bursts leave it within a tenth of the steady rate. (A median was tried: the shown intervals that
// waited for the floor pull it down, and the floor follows it down; the average keeps the frames' total time)
static inline void present_smooth_shown(PresentSmooth* s, int64_t t) {
    if (s->last_ns && t > s->last_ns && t - s->last_ns < PRESENT_SMOOTH_HITCH_NS && (s->avg_ns <= 0 || t - s->last_ns <= 3 * s->avg_ns)) {
        int64_t d = t - s->last_ns;
        if (s->avg_ns > 0 && d > s->avg_ns + s->avg_ns / 2) d = s->avg_ns + s->avg_ns / 2;
        s->avg_ns = s->avg_ns > 0 ? s->avg_ns + (d - s->avg_ns) / 4 : d;
    }
    s->last_ns = t;
}

// The floor is not in effect (switched off, frame generation presents, the row Off): the next picture starts it afresh
static inline void present_smooth_reset(PresentSmooth* s) { s->avg_ns = 0, s->last_ns = 0; }
