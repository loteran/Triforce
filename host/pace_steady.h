/* (Triforce) The Steady frame rate row: the period the game's frames are paced by, lowered to one this PC holds in the
   scene and raised again when it has room (host/nn_vi.c steady_period; the present side's floor at it,
   gpu/vk/present_smooth.h present_smooth_paced_at). Plain C with no system call, so that tests/test_pace_steady.py runs it on
   frame times of its own and on the lab's recorded ones.

   Under a cap the PC does not hold (240, with the Market's frames 10.5 ms long and 13.9 at p99) no frame waits, and every
   one goes out the moment it is done: the intervals are the frames' own work, which swings from frame to frame. Here the
   period is one the frames fit in, so they wait for it: the game's own wait (hos_vi_native_acquire) starts them a period
   apart, the steady step (step_steady.h) snaps the game's time step to the period, and the present holds a picture that
   comes early to the period after the last one (present_smooth_paced_at: the start alone leaves the presents as uneven
   as the work after it).

   What a frame took and needed (the caller measures both from its present interval and the pacing's waits in it, the
   acquire's and the present's hold: pace_steady_measure): took, the interval less the longest of those waits, for whether
   it was late (a frame that waited anywhere was on time); needed, the interval less all of them, for how much room there
   is. Needed is kept for the last PACE_STEADY_RING frames that are not hitches, took for the last PACE_STEADY_SHORT. A
   hitch is a frame that took over PACE_STEADY_HITCH times the median of what the last PACE_STEADY_SHORT frames took,
   hitches among them, or a present interval over PACE_STEADY_LOAD_NS (a load), and it is left out: a load or a one-off
   spike moves nothing, and a scene that stays heavier is the median within half that many frames. ((Triforce, test26) The
   median was once that of the frames kept alone: in the lab's run the title's frames, which waited most of their 4.2 ms,
   made it about 2 ms, every Market frame of 11 ms was then a hitch, never kept, and the median never moved: the rate stayed
   at the cap for good.) For PACE_STEADY_SETTLE_NS after a load the frames are not the scene's measure either (the lab's
   Market came in with 40 frames of 15 to 20 ms before its own 10.6 ms ones, and the rate went from the cap to 60 fps where
   80 held): they count for the median, so that the scene is its measure when the settling ends, and nothing else.
     - Lowered quickly: when a quarter of the last PACE_STEADY_SHORT frames took more than the period, to the first
       period of the set that their 90th percentile fits in with 2% to spare (at least one step), half a second after the
       last change at the soonest. After a load, once it has settled, the first lowering waits for PACE_STEADY_FRESH frames
       of the new scene and takes the 90th percentile of those (about 1.4 s of the Market's frames).
     - Raised slowly: once the period has held PACE_STEADY_UP_HOLD_NS since a raise (or the backoff since a lowering), to
       the shortest period of the set that the 90th percentile of what the frames kept needed (PACE_STEADY_UP_MIN of them at
       least) fits in with 5% to spare (the next one only, where the game acquires on another thread: one_step). The gaps
       between a quarter of the frames late (lowered), a tenth with 2% to spare (where a lowering goes) and a tenth with 5% to
       spare (raised) are the hysteresis: a scene whose 90th percentile is within 3% of a rate's period stays where it is
       rather than going up and down with its slower swings. ((Triforce, test27) It was the 95th percentile with 5% to spare,
       and the lab's Market, 11.7 ms at the 90th, 12.3 with 5%, would have stayed at 72 where 80 held.)
     - Not hunting: a lowering within PACE_STEADY_FAILED_NS of a raise says the raise was wrong, and the wait before the
       next raise doubles (PACE_STEADY_BACKOFF_NS, then 12, 24 ... PACE_STEADY_BACKOFF_MAX_NS); a minute with no lowering
       after a raise brings it back, and so does a load (a new scene: the lab's tour came into the Market with the backoff
       of the areas before, and stayed at 62 fps where 74 held).
     - Never past the owner's cap: the set's shortest period is the cap's.
   The set: PACE_STEADY_ANY, rates that suit a display with variable refresh (or with VSync off): the cap and the rates of
   pace_steady_rates under it, about a tenth apart; PACE_STEADY_REFRESH, whole divisions of the display's refresh rate under
   the cap (240 Hz: 240, 120, 80, 60, 48, 40, 34.3, 30), for VSync without variable refresh, where a frame then stays on the
   screen the same number of refreshes every time. 30 fps is the floor of both. */
#pragma once
#include <stdint.h>
#include <string.h>

#define PACE_STEADY_RING 512               /* (the frames the raise looks at) */
#define PACE_STEADY_SHORT 64                /* (the frames the lowering looks at) */
#define PACE_STEADY_UP_MIN 256              /* (frames kept before a raise is weighed) */
#define PACE_STEADY_HITCH 2.5               /* (times the recent median: a hitch, left out) */
#define PACE_STEADY_LOAD_NS 250000000LL     /* (a present interval this long is a load, left out) */
#define PACE_STEADY_DOWN_GAP_NS 500000000LL /* (between a change and a lowering, at least) */
#define PACE_STEADY_UP_HOLD_NS 3000000000LL /* (between a raise and the next raise, at least) */
#define PACE_STEADY_BACKOFF_NS 6000000000LL /* (between a lowering and a raise, at first) */
#define PACE_STEADY_BACKOFF_MAX_NS 120000000000LL
#define PACE_STEADY_FAILED_NS 30000000000LL /* (a lowering this soon after a raise doubles the backoff) */
#define PACE_STEADY_CALM_NS 60000000000LL   /* (this long with no lowering after a raise: the backoff is back to its first) */
#define PACE_STEADY_SETTLE_NS 2000000000LL  /* (after a load: not the scene's measure yet) */
#define PACE_STEADY_FRESH 128               /* (frames of the new scene the first lowering after a load takes its measure of) */
#define PACE_STEADY_MAX_PERIODS 24
#define PACE_STEADY_MIN_FPS 30
#define PACE_STEADY_PROBE_FRAMES 24               /* (frames a probe measures, unpaced) */
#define PACE_STEADY_PROBE_SKIP 8                  /* (frames at its start it lets go first: the last waits, the time step's catching up) */
#define PACE_STEADY_PROBE_RING 72                 /* (the last three probes' frames: their 90th percentile is the measure) */
#define PACE_STEADY_PROBE_EVERY_NS 12000000000LL  /* (a probe at least this often) */
#define PACE_STEADY_PROBE_GAP_NS 2000000000LL     /* (frames late again: a probe this long after the last at the soonest, then
                                                     twice as long each time it changes nothing, up to PACE_STEADY_PROBE_EVERY_NS) */

enum { PACE_STEADY_OFF, PACE_STEADY_ANY, PACE_STEADY_REFRESH };
_Static_assert(PACE_STEADY_RING % PACE_STEADY_SHORT == 0, "took[] follows ring[]'s index");

typedef struct {
    int64_t ring[PACE_STEADY_RING];      /* (what the frames kept needed, ns) */
    unsigned count, next;
    int64_t took[PACE_STEADY_SHORT];     /* (what the last frames kept took, ns: next % PACE_STEADY_SHORT is the oldest) */
    int64_t recent[PACE_STEADY_SHORT];   /* (what the last frames took, hitches too: the median a hitch is measured by) */
    unsigned recent_count, recent_next;
    int64_t periods[PACE_STEADY_MAX_PERIODS];   /* (the set, shortest first: periods[0] is the cap's) */
    int n, at;                           /* (how many; the one in force) */
    int64_t since_ns;                    /* (since the last change) */
    int64_t hold_ns;                     /* (what a raise waits for, since the last change) */
    int64_t backoff_ns;                  /* (what a raise waits for after a lowering) */
    int64_t since_up_ns;                 /* (since the last raise; -1 none yet) */
    int64_t frames, changes, raises, lowerings, hitches, kept;
    int64_t loads, unmeasured;           /* (left out too: present intervals over PACE_STEADY_LOAD_NS; none, or nothing left) */
    int64_t settle_ns, settling;         /* (what is left of the settling after a load; the frames left out for it) */
    int fresh;                           /* (a load since the last change: the frames kept are all the new scene's) */
    int one_step;                        /* (set by the caller: a raise goes one rate at a time, pace_steady_measure says why) */
    /* (the probes, set by the caller with probe: pace_steady_probed) */
    int probe;                           /* (1: the rate is the probes'; 0: the frames' own measure as they run, as before) */
    int probing, probe_due;              /* (frames left of the probe under way, 0 none; a probe at the next frame) */
    int64_t probe_ring[PACE_STEADY_PROBE_RING];
    unsigned probe_count, probe_next;
    int64_t probe_now[2 * PACE_STEADY_PROBE_FRAMES];   /* (the probe under way's frames) */
    unsigned probe_now_count, probe_len;               /* (how many so far; how many it measures) */
    int64_t scenes;                                    /* (probes that found a new scene) */
    int64_t since_probe_ns, probe_gap_ns, probes, probe_p50, probe_p90;
    int64_t scratch[PACE_STEADY_RING];   /* (the percentiles' working copy) */
} PaceSteady;

/* The rates of PACE_STEADY_ANY, from the fastest */
static const int pace_steady_rates[] = {240, 200, 180, 160, 144, 120, 110, 100, 90, 80, 72, 66, 60, 55, 50, 45, 40, 36, 30};

/* The k-th smallest of a[0..n-1] (reorders a) */
static inline int64_t pace_steady_select(int64_t *a, int n, int k) {
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        const int64_t pivot = a[lo + (hi - lo) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i <= j) {
                const int64_t t = a[i];
                a[i++] = a[j], a[j--] = t;
            }
        }
        if (k <= j) hi = j;
        else if (k >= i) lo = i;
        else break;
    }
    return a[k];
}
/* The q quantile (0..1) of the last `last` frames kept (all of them for last >= count) */
static inline int64_t pace_steady_quantile(PaceSteady *s, unsigned last, double q) {
    const unsigned n = last < s->count ? last : s->count;
    if (!n) return 0;
    for (unsigned i = 0; i < n; i++) s->scratch[i] = s->ring[(s->next + PACE_STEADY_RING - n + i) % PACE_STEADY_RING];
    int k = (int)(q * n);
    if (k > (int)n - 1) k = (int)n - 1;
    return pace_steady_select(s->scratch, (int)n, k);
}

/* The set for a mode, under the cap (fps): refresh, the display's refresh rate (Hz; 0 or less when it is not known, and
   PACE_STEADY_REFRESH then takes PACE_STEADY_ANY's set). The period in force is the cap's: the frames start as they did */
static inline void pace_steady_init(PaceSteady *s, int mode, int cap, int refresh) {
    memset(s, 0, sizeof *s);
    if (cap < 1) cap = 60;
    s->periods[s->n++] = 1000000000LL / cap;
    if (mode == PACE_STEADY_REFRESH && refresh > 0) {
        for (int k = 1; s->n < PACE_STEADY_MAX_PERIODS; k++) {
            const int64_t p = 1000000000LL * k / refresh;
            if (p * PACE_STEADY_MIN_FPS > 1000000000LL) break;
            if (p > s->periods[s->n - 1]) s->periods[s->n++] = p;
        }
        if (s->n > 1 && refresh % cap) {
            /* (a cap that is no whole division of the refresh, 144 on 240 Hz: its own period gives way to the next division,
               so that every period of the set is one; the owner's cap is still never passed) */
            memmove(s->periods, s->periods + 1, sizeof s->periods[0] * (size_t)--s->n);
        }
    } else {
        for (unsigned i = 0; i < sizeof pace_steady_rates / sizeof pace_steady_rates[0] && s->n < PACE_STEADY_MAX_PERIODS; i++) {
            const int64_t p = 1000000000LL / pace_steady_rates[i];
            if (p > s->periods[s->n - 1]) s->periods[s->n++] = p;
        }
    }
    s->backoff_ns = PACE_STEADY_BACKOFF_NS;
    s->hold_ns = PACE_STEADY_UP_HOLD_NS;
    s->since_up_ns = -1;
    s->probe_gap_ns = PACE_STEADY_PROBE_GAP_NS;
    s->probe_due = 1;
}

/* The period the frames are paced by now: the cap's while a probe is under way */
static inline int64_t pace_steady_period(const PaceSteady *s) { return s->probing ? s->periods[0] : s->periods[s->at]; }
/* The period the rule holds, a probe or not */
static inline int64_t pace_steady_held(const PaceSteady *s) { return s->periods[s->at]; }

/* Another limiter held the frames (RivaTuner, Reflex): what was measured is not the frames' work. The cap's period again,
   and nothing kept; the backoff stays */
static inline void pace_steady_reset(PaceSteady *s) {
    s->count = s->next = 0;
    s->recent_count = s->recent_next = 0;
    s->at = 0;
    s->since_ns = 0;
    s->hold_ns = PACE_STEADY_UP_HOLD_NS;
    s->probing = 0, s->probe_due = 1, s->probe_count = s->probe_next = 0, s->probe_gap_ns = PACE_STEADY_PROBE_GAP_NS;
}

/* What a frame took and needed (above), from its present interval and the pacing's waits in it: own, those on the presenting
   thread (the acquire's, where the game acquires there too, and the present's hold); other, the acquire's on other threads.
     - took: the interval less the longest wait: a frame that waited anywhere was on time, and one that waited nowhere took
       its whole interval.
     - needed: the interval less all of them. OoT acquires on its main thread and presents on its Presentation Thread, one
       after the other: the main thread waits in the acquire, builds the frame, the Presentation Thread replays and presents
       it, and only then does the main thread acquire again. The lab's Market at 60 fps (test27): 16.5 ms intervals, the
       acquire's wait 5.8 ms and the hold 0.3, leaving 10.4, the frame time it runs at with no pacing (10.6). (A version
       that took the two threads' waits for side by side, the interval less the smaller, came to 16.2 there and was never
       raised.) Where the waits do overlap (two threads working side by side) they would add up to more than the frame had
       to spare and it would need too little: where they add up to the whole interval or more, the longest is taken; and with
       the acquire on another thread a raise goes one rate at a time (PaceSteady.one_step), so that a raise too far is late
       within half a second, lowered again, and the backoff grows */
static inline void pace_steady_measure(int64_t interval, int64_t own, int64_t other, int64_t *took, int64_t *need) {
    const int64_t longest = own > other ? own : other;
    *took = interval - longest;
    *need = interval - (own + other < interval ? own + other : longest);
}

/* The q quantile of a[0..n-1], through the scratch copy */
static inline int64_t pace_steady_quantile_of(PaceSteady *s, const int64_t *a, unsigned n, double q) {
    if (!n) return 0;
    memcpy(s->scratch, a, sizeof a[0] * n);
    int k = (int)(q * n);
    if (k > (int)n - 1) k = (int)n - 1;
    return pace_steady_select(s->scratch, (int)n, k);
}

/* ((Triforce, test31) The probes, PaceSteady.probe. A frame paced slower does more work than it does unpaced: the lab's
   Market took 11.6 ms a frame unpaced (85 fps) and 15.5 ms paced at 55 fps (18.2 ms), whether the game does more in a longer
   time step or the PC's clocks come down while it idles. The frames' own measure as they ran then chased the rate down: held
   at 60 they needed 16 ms and 60 was the rate; a raise was weighed by what they needed at the slower rate, never by what they
   would at the faster one (test29 to test31 held 45 to 60 fps where 72 to 80 held).
   So the rate is chosen by what the frames take unpaced: a probe runs PACE_STEADY_PROBE_SKIP + PACE_STEADY_PROBE_FRAMES
   frames at the cap (pace_steady_period gives the cap's period meanwhile: no wait, no hold; twice as many measured for the
   first probe of a scene, at the start and after a load), takes what its measured frames took (hitches left out) into the
   ring of the last three probes, and the rate becomes the first that ring's 90th percentile fits in with 2% to spare where
   that is slower than the one held, or the first it fits in with 5% to spare where that is faster. A probe whose median is
   over 30% off the ring's is a new scene: the probes before it go (the frames a probe takes in a row swing together, and
   15% came up about once a minute in a Market of 10.6 ms swinging by a millisecond). A probe comes at the start, after a
   load has settled, at least every PACE_STEADY_PROBE_EVERY_NS (some 32 frames, under half a second, every 12 s), and when a quarter of the
   last 64 frames ran late (PACE_STEADY_PROBE_GAP_NS after the last at the soonest, twice that each time it changes
   nothing): frames late do not lower the rate by themselves any more, so it is never under what a probe shows the PC
   holds. 1 when the rate changed */
static inline int pace_steady_probed(PaceSteady *s) {
    s->count = s->next = 0;   /* (frames late are counted afresh at the rate held from here) */
    if (s->probe_now_count < PACE_STEADY_PROBE_FRAMES / 2) return 0;   /* (a probe of hitches: nothing measured) */
    const int64_t now50 = pace_steady_quantile_of(s, s->probe_now, s->probe_now_count, 0.5);
    if (s->probe_count) {
        const int64_t ring50 = pace_steady_quantile_of(s, s->probe_ring, s->probe_count, 0.5);
        if (now50 * 100 > ring50 * 130 || now50 * 130 < ring50 * 100) s->probe_count = s->probe_next = 0, s->scenes++;
    }
    for (unsigned i = 0; i < s->probe_now_count; i++) {
        s->probe_ring[s->probe_next] = s->probe_now[i];
        s->probe_next = (s->probe_next + 1) % PACE_STEADY_PROBE_RING;
        if (s->probe_count < PACE_STEADY_PROBE_RING) s->probe_count++;
    }
    const int64_t p90 = pace_steady_quantile_of(s, s->probe_ring, s->probe_count, 0.90);
    s->probe_p90 = p90, s->probe_p50 = pace_steady_quantile_of(s, s->probe_ring, s->probe_count, 0.5);
    int lower = 0, raise = 0;
    while (lower < s->n - 1 && s->periods[lower] < p90 * 102 / 100) lower++;
    while (raise < s->n - 1 && s->periods[raise] < p90 * 105 / 100) raise++;
    const int to = lower > s->at ? lower : raise < s->at ? raise : s->at;
    if (to == s->at) {
        s->probe_gap_ns *= 2;
        if (s->probe_gap_ns > PACE_STEADY_PROBE_EVERY_NS) s->probe_gap_ns = PACE_STEADY_PROBE_EVERY_NS;
        return 0;
    }
    if (to > s->at) s->lowerings++;
    else s->raises++;
    s->at = to, s->changes++, s->since_ns = 0, s->probe_gap_ns = PACE_STEADY_PROBE_GAP_NS;
    return 1;
}

/* A frame presented: took_ns and need_ns, what it took and needed (above; 0 or less: not measured), interval_ns, its present
   interval. 1 when the period changed (lowered or raised), else 0 */
static inline int pace_steady_feed(PaceSteady *s, int64_t took_ns, int64_t need_ns, int64_t interval_ns) {
    s->frames++;
    if (interval_ns > 0) s->since_ns += interval_ns, s->since_probe_ns += interval_ns;
    if (s->since_up_ns >= 0 && interval_ns > 0) s->since_up_ns += interval_ns;
    if (s->since_up_ns > PACE_STEADY_CALM_NS) s->backoff_ns = PACE_STEADY_BACKOFF_NS;
    if (interval_ns > PACE_STEADY_LOAD_NS) {
        /* (a load: a new scene, which the raises that failed in the last one say nothing of. The backoff starts again; the
           frames kept stay, and the lowering takes the new scene's measure within a second if it is heavier) */
        s->backoff_ns = PACE_STEADY_BACKOFF_NS;
        if (s->hold_ns > s->backoff_ns) s->hold_ns = s->backoff_ns;
        s->loads++;
        s->settle_ns = PACE_STEADY_SETTLE_NS;   /* (and the frames kept start again once it has settled) */
        s->count = s->next = 0;
        s->fresh = 1;
        /* (with the probes: the last scene's measure goes, a probe under way stops, and one comes once it has settled) */
        s->probe_count = s->probe_next = 0, s->probing = 0, s->probe_due = 1, s->probe_gap_ns = PACE_STEADY_PROBE_GAP_NS;
        return 0;
    }
    if (interval_ns <= 0 || took_ns <= 0 || need_ns <= 0) {
        s->unmeasured++;
        return 0;
    }
    s->recent[s->recent_next] = took_ns;
    s->recent_next = (s->recent_next + 1) % PACE_STEADY_SHORT;
    if (s->recent_count < PACE_STEADY_SHORT) s->recent_count++;
    if (s->settle_ns > 0) {
        s->settle_ns -= interval_ns;
        s->settling++;
        return 0;
    }
    const int hitch = s->recent_count >= 16 &&
                      (double)took_ns > PACE_STEADY_HITCH * (double)pace_steady_quantile_of(s, s->recent, s->recent_count, 0.5);
    if (s->probe) {
        if (s->probing) {   /* (a probe's frame: unpaced) */
            const int measured = s->probing <= (int)s->probe_len;
            s->probing--;
            if (hitch) s->hitches++;
            else if (measured && s->probe_now_count < 2 * PACE_STEADY_PROBE_FRAMES) s->probe_now[s->probe_now_count++] = took_ns;
            return s->probing ? 0 : pace_steady_probed(s);
        }
        unsigned late = 0;
        if (!hitch) {
            s->kept++;
            s->took[s->next % PACE_STEADY_SHORT] = took_ns;
            s->ring[s->next] = need_ns;
            s->next = (s->next + 1) % PACE_STEADY_RING;
            if (s->count < PACE_STEADY_RING) s->count++;
            if (s->count >= PACE_STEADY_SHORT)
                for (unsigned i = 0; i < PACE_STEADY_SHORT; i++) late += s->took[i] > s->periods[s->at];
        } else s->hitches++;
        if (s->probe_due || s->since_probe_ns >= PACE_STEADY_PROBE_EVERY_NS ||
            (late * 4 >= PACE_STEADY_SHORT && s->since_probe_ns >= s->probe_gap_ns)) {
            /* (the first of a scene measures twice as many: its 90th percentile is the rate until the next) */
            s->probe_len = s->probe_count ? PACE_STEADY_PROBE_FRAMES : 2 * PACE_STEADY_PROBE_FRAMES;
            s->probing = PACE_STEADY_PROBE_SKIP + (int)s->probe_len, s->probe_now_count = 0;
            s->probe_due = 0, s->since_probe_ns = 0, s->probes++;
        }
        return 0;
    }
    if (hitch) {
        s->hitches++;
        return 0;
    }
    s->kept++;
    s->took[s->next % PACE_STEADY_SHORT] = took_ns;
    s->ring[s->next] = need_ns;
    s->next = (s->next + 1) % PACE_STEADY_RING;
    if (s->count < PACE_STEADY_RING) s->count++;
    if (s->fresh && s->count >= 2 * PACE_STEADY_FRESH) s->fresh = 0;   /* (the scene held the rate: its measure is the usual) */
    if (s->count < PACE_STEADY_SHORT) return 0;
    const int64_t period = s->periods[s->at];
    /* (after a load the first lowering waits for PACE_STEADY_FRESH frames of the new scene and takes their measure: the 90th
       percentile of 64 frames swings by half a millisecond from one stretch to the next, a rate either way) */
    if (s->at < s->n - 1 && s->since_ns >= PACE_STEADY_DOWN_GAP_NS && !(s->fresh && s->count < PACE_STEADY_FRESH)) {
        unsigned late = 0;
        for (unsigned i = 0; i < PACE_STEADY_SHORT; i++) late += s->took[i] > period;
        if (late * 4 >= PACE_STEADY_SHORT) {
            const int64_t want = (s->fresh ? pace_steady_quantile(s, s->count, 0.90)
                                           : pace_steady_quantile_of(s, s->took, PACE_STEADY_SHORT, 0.90)) * 102 / 100;
            s->fresh = 0;
            int to = s->at + 1;
            while (to < s->n - 1 && s->periods[to] < want) to++;
            if (s->since_up_ns >= 0 && s->since_up_ns < PACE_STEADY_FAILED_NS) {
                s->backoff_ns *= 2;
                if (s->backoff_ns > PACE_STEADY_BACKOFF_MAX_NS) s->backoff_ns = PACE_STEADY_BACKOFF_MAX_NS;
            }
            s->at = to, s->since_ns = 0, s->hold_ns = s->backoff_ns, s->since_up_ns = -1;
            s->changes++, s->lowerings++;
            return 1;
        }
    }
    /* (weighed every 16th frame: a percentile of a thousand frames is not for every frame) */
    if (s->at > 0 && s->since_ns >= s->hold_ns && s->count >= PACE_STEADY_UP_MIN && !(s->frames & 15)) {
        const int64_t want = pace_steady_quantile(s, PACE_STEADY_RING, 0.90) * 105 / 100;
        int to = s->at;
        while (to > 0 && s->periods[to - 1] >= want && !(s->one_step && to < s->at)) to--;
        if (to < s->at) {
            s->at = to, s->since_ns = 0, s->hold_ns = PACE_STEADY_UP_HOLD_NS, s->since_up_ns = 0;
            s->changes++, s->raises++, s->fresh = 0;
            return 1;
        }
    }
    return 0;
}
