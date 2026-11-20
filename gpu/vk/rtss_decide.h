// (Triforce) RivaTuner Statistics Server as the game's frame limiter (gpu/vk/rtss_limit.cpp): the parts that make no
// system call, so that tests/test_rtss_decide.py builds them on any system (plain C: the test compiles it as C and as
// C++). After NX2navi's runtime/rtss_decide.h, whose rules these are: rtss_parse reads RTSS's shared memory,
// rtss_decide decides each poll of the worker, rtss_poll carries that decision out through the calls rtss_limit.cpp
// gives it (RtssOps), and the state file's lines keep what a profile held before the game changed it.
//
// What Triforce does differently, by the owner's word that RTSS is the limiter whenever it can hold the game:
//   - A give-back that RTSS's own behaviour caused (its frame count past the limit, 30 s without showing its hold, or
//     frames that came sooner than the game's period) is not for the rest of the run: RTSS is tried again after
//     RTSS_RETRY_NS, RTSS_RETRIES times, and at once when RTSS was closed and runs again or another limit is wanted.
//   - Frame generation does not keep RTSS out where every game frame is presented a fixed number of times: the limit
//     wanted is then the one on presented frames, the game's cap times that number (pace_rule.h). Only a mode whose
//     presents do not follow the game's frames keeps it out (RTSS_OUT_*).
//   - Where the profile cannot be changed (no administrator rights: RTSS keeps its profiles under Program Files), RTSS
//     may still hold the game by a limit of the player's own: rtss_own_decide, and then the game's own wait steps
//     aside for that limit.
//   - The game's own wait keeps no ceiling beside RTSS: where the frame starts it is one or the other
//     (rtss_frame_holds), and the frames RTSS holds are checked by their own times (rtss_watch_step).
//   - The period an entry of the shared memory holds when it is first seen is where the watch starts, not a sign of
//     life: an entry a closed program of the same process id left never counts as RTSS measuring the game.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The want's argument: a limit on the frames RTSS counts, in millihertz (>0), 0 for none, or why RTSS is kept out.
// RTSS_UNSET is the want before the first one.
enum {
    RTSS_UNSET = -2,
    RTSS_OUT_SETTING = -1,   // HOS_RTSS=0, for the whole run
    RTSS_OUT_AUTO = -3,      // frame generation on Auto: a frame for each refresh of the screen, whatever the game's rate
    RTSS_OUT_DLSSG = -4,     // DLSS frame generation: Streamline shows its frames on a pace of its own
};

// What keeps RTSS out, for the log.
static inline const char *rtss_out_text(int64_t want) {
    return want == RTSS_OUT_AUTO    ? "frame generation on Auto shows a frame for each refresh of the screen, so holding those does not hold the game"
           : want == RTSS_OUT_DLSSG ? "DLSS frame generation shows its frames on a pace of its own"
                                    : "HOS_RTSS=0";
}

// The limiter that holds the frames while RTSS is kept out, for the log.
static inline const char *rtss_out_limiter(int64_t want) {
    return want == RTSS_OUT_DLSSG ? "the game's own wait or Reflex" : "the game's own wait";
}

// ---- RTSS's shared memory, "RTSSSharedMemoryV2". The header: signature, version, application entry size, offset of
// the application array, its length. An entry: the process id, the executable path (260 bytes), flags, then RTSS's
// last frame rate period for the process: its start and end in ms and the frames in it. The flags' low 16 bits hold
// the API RTSS saw the process present with, a value rather than bits (Vulkan is 0xA), and RTSS_UPDATE_ASKED stays set
// from UpdateProfiles until the process has read its profile again.
#define RTSS_SIGNATURE 0x52545353u   // 'RTSS' as RTSS writes it: the memory holds valid data
#define RTSS_SIGNATURE_DEAD 0xDEADu  // a closed RTSS's block, while a hooked program still keeps it open
#define RTSS_API_MASK 0xFFFFu
#define RTSS_API_VULKAN 0xAu
#define RTSS_UPDATE_ASKED 0x10000000u
enum { RTSS_FLAGS_AT = 4 + 260, RTSS_PERIOD_AT = RTSS_FLAGS_AT + 4, RTSS_ENTRY_MIN = RTSS_PERIOD_AT + 12,
       RTSS_APPS_MAX = 4096 };

typedef struct {
    int running;                 // the signature and a version of 2.0 or later
    int dead;                    // the block of an RTSS that closed
    int listed;                  // the application list holds this process: RTSS has hooked it
    uint32_t flags;              // the entry's flags
    uint32_t t0, t1, frames;     // the period of this process's entry
} RtssApp;

// Reads the memory of size bytes at p for the process pid; never reads past size. p NULL: no block at all.
static inline void rtss_parse(const uint8_t *p, size_t size, uint32_t pid, RtssApp *a) {
    memset(a, 0, sizeof *a);
    if (!p || size < 20) return;
    uint32_t h[5];
    memcpy(h, p, sizeof h);
    a->running = h[0] == RTSS_SIGNATURE && h[1] >= 0x20000;
    a->dead = h[0] == RTSS_SIGNATURE_DEAD;
    for (uint32_t i = 0; a->running && h[2] >= RTSS_ENTRY_MIN && i < h[4] && i < RTSS_APPS_MAX; i++) {
        uint64_t at = (uint64_t)h[3] + (uint64_t)i * h[2];
        if (at + RTSS_ENTRY_MIN > size) break;
        uint32_t id, t[3];
        memcpy(&id, p + at, 4);
        if (id != pid) continue;
        a->listed = 1;
        memcpy(&a->flags, p + at + RTSS_FLAGS_AT, 4);
        memcpy(t, p + at + RTSS_PERIOD_AT, sizeof t);
        a->t0 = t[0], a->t1 = t[1], a->frames = t[2];
        break;
    }
}

// The frame rate RTSS measured for the process over its period; 0 unknown.
static inline double rtss_fps(const RtssApp *a) {
    return a->listed && a->t1 > a->t0 ? 1000.0 * a->frames / (a->t1 - a->t0) : 0;
}

// ---- the worker's decisions
#define RTSS_OVER_PERIODS 3            // fresh periods in a row RTSS's frame rate of the game may stay above the limit
#define RTSS_OVER_SLACK 1.05           // above limit * this, RTSS is taken not to hold the game to it
#define RTSS_FRESH_NS 2500000000ull    // a period is stale after this, or 2.5 times its own length if longer
#define RTSS_HOLD_IDLE_NS 200000000ull   // no present for this long: RTSS holds nothing (its limiter sits in the present)
#define RTSS_SETTLE_POLLS 2              // polls in a row a want must stay before it acts on the profile
#define RTSS_SETTLE_DOWN_POLLS 10        // (Triforce) and a limit under the one the profile holds (rtss_decide says why)
#define RTSS_UNPROVEN_POLLS 60           // polls (30 s) RTSS may hook the game with the limit and not show it holds it
#define RTSS_RETRY_NS 60000000000ull     // (Triforce) after a give-back RTSS itself caused, it is tried again after this
#define RTSS_RETRIES 3                   // (Triforce) and so many times; then only when RTSS restarts or the limit changes
#define RTSS_WATCH_WINDOW 60             // (Triforce) frames over which RTSS's hold is checked by their own times

enum {   // out of it: for the run, or until it is tried again (rtss_fail_retried)
    RTSS_FAIL_NONE,
    RTSS_FAIL_LIBRARY,       // its hooks library did not load
    RTSS_FAIL_READONLY,      // its profile cannot be written
    RTSS_FAIL_NOT_LIMITING,  // its frame count showed the game past the limit
    RTSS_FAIL_UNPROVEN,      // it hooked the game with the limit for RTSS_UNPROVEN_POLLS and never showed it holds it
    RTSS_FAIL_FAST,          // (Triforce) the frames' own times showed that nothing held the game (rtss_watch_step)
};
// The failures RTSS's own behaviour caused, which a later try may find gone. A library that does not load and a
// profile that cannot be written do not change while the game runs.
static inline int rtss_fail_retried(int fail) {
    return fail == RTSS_FAIL_NOT_LIMITING || fail == RTSS_FAIL_UNPROVEN || fail == RTSS_FAIL_FAST;
}
enum { RTSS_DO_NOTHING, RTSS_DO_APPLY, RTSS_DO_PUT_BACK };
enum {
    RTSS_WHY_HOLDS,         // RTSS holds the game to the want
    RTSS_WHY_NOT_LISTED,    // RTSS has the limit but has not hooked the game yet
    RTSS_WHY_OUT,           // kept out (the want says why)
    RTSS_WHY_FAILED,        // RtssState.fail
    RTSS_WHY_NOT_RUNNING,
    RTSS_WHY_REFUSED,       // RTSS refused this limit
    RTSS_WHY_NEWER,         // the want changed while the poll ran: nothing decided, the next poll takes the new one
    RTSS_WHY_UNSET,         // no want yet
    RTSS_WHY_MOVIE,         // a movie plays: the game's own wait holds it at its rate
    RTSS_WHY_NOT_VULKAN,    // RTSS lists the game but has not seen its Vulkan presents
    RTSS_WHY_UPDATE,        // RTSS has not read the profile the game wrote yet
    RTSS_WHY_NOT_COUNTING,  // RTSS has measured no period of the game's frames lately
    RTSS_WHY_IDLE,          // no picture went out lately (a minimised window): RTSS has none to hold
    RTSS_WHY_SETTLING,      // a new want: it reaches the profile once it stayed RTSS_SETTLE_POLLS polls
    RTSS_WHY_NO_PRESENT,    // no picture went out yet: nothing is written before the first
    // (Triforce)
    RTSS_WHY_NO_LIMIT,      // no limit is wanted, or RTSS has no frame rate limit of its own for the game (rtss_own_decide)
    RTSS_WHY_OTHER,         // a frame time limit or scanline sync of RTSS's own: not acted on (rtss_own_decide)
    RTSS_WHY_ABOVE,         // RTSS's own limit is above the one the game wants (rtss_own_decide)
};

// What the worker keeps between polls.
typedef struct {
    int64_t applied;     // the millihertz the profile holds from this run; -1 untouched
    int64_t refused;     // a limit RTSS refused (out of its range): the game's own limiter keeps that one
    int changed;         // the profile holds a value from this run: put the old one back on exit
    int over;            // fresh periods in a row RTSS's frame rate of the game stayed above the limit
    int fail;            // RTSS_FAIL_*
    int held;            // the poll before stood aside: a fresh period now shows how RTSS holds the game
    int listed;          // (Triforce) the poll before saw the game in RTSS's list: its period's end is known from then
    uint32_t t1, span;   // the end and length (ms) of the newest period seen
    uint64_t fresh_ns;   // when a new period was last seen; 0 never
    int64_t seen;        // the want of the poll before
    int polls;           // polls in a row with that want
    int unproven;        // polls in a row RTSS hooked the game with a limit in the profile and did not show its hold
    // (Triforce) the retry after a give-back
    uint64_t fail_ns;    // when a failure that is tried again came
    int64_t fail_want;   // the want it came at
    int retries;         // tries made since by the clock
    int gone;            // RTSS was seen not running since it
} RtssState;
#define RTSS_STATE_INIT {-1, -1, 0, 0, RTSS_FAIL_NONE, 0, 0, 0, 0, 0, RTSS_UNSET, 0, 0, 0, 0, 0, 0}

// One poll's view.
typedef struct {
    int64_t want;        // the want the poll read
    int current;         // the want is still that one when the poll decides
    int hooks;           // RTSS's hooks library: 1 loaded, 0 failed to load, -1 not tried (rtss_wants_hooks)
    int64_t movie_mhz;   // the rate of a movie that plays, 0 none
    uint64_t now_ns;     // the clock at the poll
    uint64_t hosted_ns;  // the clock after the newest present, 0 none yet
    int fast;            // (Triforce) the frames RTSS was to hold came too soon since the poll before (rtss_watch_step)
    RtssApp app;
} RtssIn;

typedef struct {
    int act;        // RTSS_DO_*
    int64_t mhz;    // the value decided for: the profile's (rtss_target) for a limit, the want for a kept-out one
    int holds;      // RTSS holds the game to it: the game's own wait stands aside
    int why;        // RTSS_WHY_*
    double fps;     // RTSS's frame rate of the game, when the frame count check gave up on it
    int retry;      // (Triforce) this poll tries RTSS again after a give-back: 1 by the clock, 2 RTSS runs again, 3 another limit
} RtssOut;

// What the profile gets for a want w >= 0 while a movie plays at movie_mhz: w, so that a movie does not rewrite the
// profile at its start and end; but no limit (0) while the movie runs faster than w, where RTSS would hold the movie
// below its rate. The game's own wait holds every movie (rtss_decide RTSS_WHY_MOVIE). (Triforce paces a movie at the
// cap as it paces play, so the movie's rate given here is the want itself and the profile keeps it.)
static inline int64_t rtss_target(int64_t w, int64_t movie_mhz) { return w > 0 && movie_mhz > w ? 0 : w; }

// The profile's FramerateLimit and FramerateLimitDenominator for mhz (>= 0): a whole rate over 1, a fraction over 1000
// where RTSS knows the denominator (has_den), else rounded down to whole frames, never above what was asked.
static inline void rtss_limit_value(int64_t mhz, int has_den, uint32_t *limit, uint32_t *den) {
    *den = 1;
    if (mhz % 1000 == 0) *limit = (uint32_t)(mhz / 1000);
    else if (has_den) *limit = (uint32_t)mhz, *den = 1000;
    else *limit = (uint32_t)(mhz / 1000);
}

// The hooks library is loaded only on a poll that may write the profile: a limit is wanted, RTSS is not out of it,
// and it runs. A poll of a kept-out want never loads it; a later limit does.
static inline int rtss_wants_hooks(const RtssState *s, const RtssIn *in) {
    return in->want >= 0 && !s->fail && in->app.running;
}

// (Triforce) A failure that is tried again is put down: when and at which want.
static inline void rtss_failed(RtssState *s, int fail, const RtssIn *in) {
    s->fail = fail, s->fail_ns = in->now_ns, s->fail_want = in->want, s->gone = 0;
}

// The decision of one poll, in this order: no want does nothing; a want acts on the profile only once it stayed
// RTSS_SETTLE_POLLS polls in a row, so a want that flips does not write and remove the profile at each flip, while the
// game's own wait takes a new want at once. A settled kept-out want puts back what this run wrote (once the hooks are
// loaded); a failure lasts until it is tried again (below); RTSS not running leaves the profile for its return; a
// library that did not load fails the run; nothing is written before the first picture went out; a refused limit
// stays with the game; a settled new limit (rtss_target) goes into the profile. The game's own wait stands aside only
// once RTSS is seen to hold the game: the profile held the limit at the poll before (RTSS reads it after the write),
// RTSS lists the game with its Vulkan presents, has read the profile (RTSS_UPDATE_ASKED clear) and measured a period
// of the game's frames lately (a fresh one: its end moved between two polls that both saw the game listed, within
// RTSS_FRESH_NS, or within 2.5 times the period's length if that is longer), no movie plays and a picture went out
// within RTSS_HOLD_IDLE_NS. An RTSS that lists the game with a limit in its profile while pictures go out, but does
// not show its hold, may hold the frames beside the game's own wait: after RTSS_UNPROVEN_POLLS such polls in a row the
// profile goes back.
// Each fresh period after a poll that stood aside is checked against the limit: more than RTSS_OVER_SLACK times it
// for RTSS_OVER_PERIODS periods in a row puts the profile back; a stale period counts neither way. So does a poll
// that is told the frames came too soon (in->fast).
// (Triforce) After such a give-back (rtss_fail_retried) RTSS is tried again: once RTSS_RETRY_NS have passed,
// RTSS_RETRIES times; and at once, with the count starting again, when RTSS was seen closed and runs again, or when a
// limit other than the one it failed at is wanted.
// (Triforce) The hysteresis on a limit that changes in play (the frames shown for each of the game's: pace_rule.h): a
// limit under the one the profile holds reaches it only once it stayed RTSS_SETTLE_DOWN_POLLS polls (5 s), a higher one
// after RTSS_SETTLE_POLLS. Meanwhile the game's own wait holds the game (the hold is for the limit the profile holds
// alone), and the profile's higher limit sits above every frame the game presents, so it holds none of them; a lower
// one would hold the game under its cap at once, which is why a higher one goes in sooner. A limit that flickers
// between two settles on the higher one and is not written again at each flicker.
static inline RtssOut rtss_decide(RtssState *s, const RtssIn *in) {
    int64_t w = in->want;
    RtssOut o = {RTSS_DO_NOTHING, w, 0, RTSS_WHY_NOT_LISTED, 0, 0};
    if (w == RTSS_UNSET) return o.why = RTSS_WHY_UNSET, o;
    const RtssApp *a = &in->app;
    if (rtss_fail_retried(s->fail)) {
        if (!a->running) s->gone = 1;
        int again = a->running && s->gone                                                               ? 2
                    : w >= 0 && w != s->fail_want                                                       ? 3
                    : w >= 0 && s->retries < RTSS_RETRIES && in->now_ns - s->fail_ns >= RTSS_RETRY_NS ? 1
                                                                                                        : 0;
        if (again) {
            s->retries = again == 1 ? s->retries + 1 : 0;
            s->fail = RTSS_FAIL_NONE, s->gone = 0, s->over = s->unproven = s->held = 0;
            o.retry = again;
        }
    }
    int listed = a->running && a->listed;
    int fresh = listed && s->listed && a->t1 > a->t0 && a->frames && a->t1 != s->t1;   // a period RTSS measured since
    if (fresh) s->span = a->t1 - a->t0, s->fresh_ns = in->now_ns;
    if (listed) s->t1 = a->t1;
    s->listed = listed;
    int held = s->held, unproven = s->unproven;
    s->held = s->unproven = 0;   // until this poll stands aside again, or counts as unproven again
    s->polls = w == s->seen ? s->polls + 1 : 1;
    s->seen = w;
    int settled = s->polls >= RTSS_SETTLE_POLLS;
    if (w < 0) {
        if (settled && in->hooks == 1 && s->changed) o.act = RTSS_DO_PUT_BACK;
        s->over = 0;
        return o.why = RTSS_WHY_OUT, o;
    }
    if (s->fail) return o.why = RTSS_WHY_FAILED, o;
    if (!a->running) return o.why = RTSS_WHY_NOT_RUNNING, o;
    if (in->hooks == 0) return s->fail = RTSS_FAIL_LIBRARY, o.why = RTSS_WHY_FAILED, o;
    if (!in->hosted_ns) return o.why = RTSS_WHY_NO_PRESENT, o;
    int64_t t = o.mhz = rtss_target(w, in->movie_mhz);
    if (t == s->refused) return o.why = RTSS_WHY_REFUSED, o;
    if (t != s->applied && s->polls < (t > 0 && t < s->applied ? RTSS_SETTLE_DOWN_POLLS : RTSS_SETTLE_POLLS))
        return o.why = RTSS_WHY_SETTLING, o;
    if (t != s->applied) {   // RTSS reads it after the write: a later poll sees whether it holds
        o.act = RTSS_DO_APPLY;
        o.why = in->movie_mhz ? RTSS_WHY_MOVIE : a->listed ? RTSS_WHY_UPDATE : RTSS_WHY_NOT_LISTED;
        s->over = 0;
        return o;
    }
    if (!in->current) return o.why = RTSS_WHY_NEWER, o;   // these frames are not the new want's
    if (in->fast && held && t > 0) {   // (Triforce) the frames it was to hold came sooner than the game's period
        rtss_failed(s, RTSS_FAIL_FAST, in);
        o.act = RTSS_DO_PUT_BACK, o.why = RTSS_WHY_FAILED;
        return o;
    }
    uint64_t idle = in->now_ns > in->hosted_ns ? in->now_ns - in->hosted_ns : 0;
    int shown = idle < RTSS_HOLD_IDLE_NS;   // a picture went out lately (one went out: RTSS_WHY_NO_PRESENT above)
    uint64_t stale = (uint64_t)s->span * 2500000ull > RTSS_FRESH_NS ? (uint64_t)s->span * 2500000ull : RTSS_FRESH_NS;
    int unseen = !a->listed                                       ? RTSS_WHY_NOT_LISTED
                 : (a->flags & RTSS_API_MASK) != RTSS_API_VULKAN   ? RTSS_WHY_NOT_VULKAN
                 : a->flags & RTSS_UPDATE_ASKED                    ? RTSS_WHY_UPDATE
                 : !s->fresh_ns || in->now_ns - s->fresh_ns > stale ? RTSS_WHY_NOT_COUNTING
                                                                   : RTSS_WHY_HOLDS;
    if (unseen != RTSS_WHY_HOLDS) {   // hooked with a limit while pictures go out, RTSS holds them beside the game's wait
        s->unproven = a->listed && t > 0 && shown ? unproven + 1 : 0;
        if (s->unproven < RTSS_UNPROVEN_POLLS) return o.why = unseen, o;
        rtss_failed(s, RTSS_FAIL_UNPROVEN, in);
        o.act = RTSS_DO_PUT_BACK, o.why = RTSS_WHY_FAILED;
        return o;
    }
    if (fresh && held && t > 0) {
        double fps = rtss_fps(a);
        s->over = fps > t / 1000.0 * RTSS_OVER_SLACK ? s->over + 1 : 0;
        if (s->over == RTSS_OVER_PERIODS) {
            rtss_failed(s, RTSS_FAIL_NOT_LIMITING, in);
            o.act = RTSS_DO_PUT_BACK, o.why = RTSS_WHY_FAILED, o.fps = fps;
            return o;
        }
    }
    if (in->movie_mhz) return o.why = RTSS_WHY_MOVIE, o;
    if (!shown) return o.why = RTSS_WHY_IDLE, o;
    if (t <= 0) return o.why = RTSS_WHY_NO_LIMIT, o;   // (Triforce) no limit is wanted: nothing for RTSS to hold
    s->held = o.holds = 1;
    return o.why = RTSS_WHY_HOLDS, o;
}

// What a write of the limit did (RtssOps.apply).
enum {
    RTSS_WROTE_READONLY = -1,        // the save did not reach the file
    RTSS_WROTE_REFUSED = 0,          // RTSS refused the value; the profile keeps what it held
    RTSS_WROTE_HELD = 1,             // the profile holds the limit
    RTSS_WROTE_REFUSED_CLEARED = 2,  // refused, and the earlier limit of this run was cleared to none
};

static inline void rtss_written(RtssState *s, RtssOut *o, int r) {
    if (r == RTSS_WROTE_READONLY) {
        s->fail = RTSS_FAIL_READONLY, o->holds = 0, o->why = RTSS_WHY_FAILED;
        return;
    }
    s->changed = 1;
    if (r == RTSS_WROTE_HELD) {
        s->applied = o->mhz;
        return;
    }
    if (r == RTSS_WROTE_REFUSED_CLEARED) s->applied = 0;
    s->refused = o->mhz, o->holds = 0, o->why = RTSS_WHY_REFUSED;
}

// The profile was put back: nothing of this run is in it.
static inline void rtss_put_back_done(RtssState *s) { s->changed = 0, s->applied = -1; }

// The profile calls rtss_poll makes. apply writes mhz (first: the first write of the run, which records what to put
// back; clear: on a refusal, clear this run's earlier limit to none) and returns RTSS_WROTE_*; put_back puts the profile
// back as it was before the run (o says why: kept out, or a check gave up on RTSS).
typedef struct {
    void *ctx;
    int (*apply)(void *ctx, int64_t mhz, int first, int clear);
    void (*put_back)(void *ctx, const RtssOut *o);
} RtssOps;

static inline RtssOut rtss_poll(RtssState *s, const RtssIn *in, const RtssOps *ops) {
    RtssOut o = rtss_decide(s, in);
    if (o.act == RTSS_DO_APPLY) {
        rtss_written(s, &o, ops->apply(ops->ctx, o.mhz, !s->changed, s->applied != 0));
    } else if (o.act == RTSS_DO_PUT_BACK) {
        ops->put_back(ops->ctx, &o);
        rtss_put_back_done(s);
    }
    return o;
}

static inline const char *rtss_fail_text(int fail) {
    return fail == RTSS_FAIL_READONLY       ? "RivaTuner Statistics Server's profile for the game cannot be changed"
           : fail == RTSS_FAIL_NOT_LIMITING ? "RivaTuner Statistics Server did not hold the game to the limit"
           : fail == RTSS_FAIL_UNPROVEN     ? "RivaTuner Statistics Server never showed it holds the game"
           : fail == RTSS_FAIL_FAST         ? "the frames came sooner than the limit while RivaTuner Statistics Server was to hold them"
                                            : "RivaTuner Statistics Server's hooks library did not load";
}

// Why the game's own wait paces, for the log line "the game's own wait (<text>)".
static inline void rtss_why_text(char *out, size_t n, const RtssOut *o, int fail) {
    if (o->why == RTSS_WHY_OUT) snprintf(out, n, "RivaTuner Statistics Server kept out: %s", rtss_out_text(o->mhz));
    else if (o->why == RTSS_WHY_FAILED) snprintf(out, n, "%s", rtss_fail_text(fail));
    else if (o->why == RTSS_WHY_NOT_RUNNING) snprintf(out, n, "RivaTuner Statistics Server is not running");
    else if (o->why == RTSS_WHY_REFUSED) snprintf(out, n, "RivaTuner Statistics Server refused this limit");
    else if (o->why == RTSS_WHY_MOVIE) snprintf(out, n, "a movie plays: the game keeps its pictures on time");
    else if (o->why == RTSS_WHY_NOT_VULKAN)
        snprintf(out, n, "RivaTuner Statistics Server lists the game but has not seen its Vulkan presents yet");
    else if (o->why == RTSS_WHY_UPDATE)
        snprintf(out, n, "RivaTuner Statistics Server has not read the game's limit from its profile yet");
    else if (o->why == RTSS_WHY_NOT_COUNTING)
        snprintf(out, n, "RivaTuner Statistics Server has measured no frames of the game lately");
    else if (o->why == RTSS_WHY_IDLE) snprintf(out, n, "no picture went out lately: RivaTuner has none to hold");
    else if (o->why == RTSS_WHY_SETTLING) snprintf(out, n, "a new limit: RivaTuner Statistics Server gets it once it stays");
    else if (o->why == RTSS_WHY_NO_PRESENT) snprintf(out, n, "no picture has gone out yet");
    else if (o->why == RTSS_WHY_NO_LIMIT) snprintf(out, n, "no limit is wanted");
    else snprintf(out, n, "RivaTuner Statistics Server has not hooked the game yet");
}

// ---- where the game's frame starts (the game's presenting thread, each frame)
// The worker says RTSS holds the game (holds) and a present went out within RTSS_HOLD_IDLE_NS (hosted_ns: the clock
// after the newest present, 0 before the first): this frame is RTSS's, the game's own wait steps aside. A minimised
// window shows nothing, RTSS then has no present to hold, and the game's own wait is back with the first frame after.
// (Triforce) And a present went out since the start of the frame two frames before this one (before2_ns, 0 while there
// is none): the game's own wait keeps no ceiling beside RTSS, so the frames of a window that stopped showing would run
// free until the 200 ms were up, a hundred of them and more, and look to rtss_watch_step like an RTSS that holds
// nothing. While RTSS holds the game a present goes out every frame (it is what holds the game), so two frames' time
// without one leaves nothing for RTSS to hold.
static inline int rtss_frame_holds(int holds, uint64_t now_ns, uint64_t hosted_ns, uint64_t before2_ns) {
    if (!holds || !hosted_ns || hosted_ns < before2_ns) return 0;
    return hosted_ns >= now_ns || now_ns - hosted_ns < RTSS_HOLD_IDLE_NS;
}

// The frames RTSS held, checked by their own times, as dlssg_pace.h checks Reflex's: RTSS's own count comes once a
// second and counts presented frames, and a game nothing holds runs free meanwhile. now_ns: where the game's frame
// starts; holds: this frame is RTSS's; period_ns: the game's own period (its cap, whatever is made of the frames
// after). 1 when, of RTSS_WATCH_WINDOW frames in a row that RTSS held to one period, over half came sooner than three
// quarters of it: the caller gives the frames back to the game's own wait at once and tells the worker (RtssIn.fast).
typedef struct {
    uint64_t last_ns;       // the newest frame start RTSS held, 0 when the game's own wait held it
    uint64_t last_period;   // its period
    int window, fast;       // frames counted in the window, those too soon
} RtssWatch;

static inline int rtss_watch_step(RtssWatch *w, uint64_t now_ns, int holds, uint64_t period_ns) {
    int bad = 0;
    if (holds && w->last_ns && now_ns >= w->last_ns && w->last_period == period_ns) {
        if (now_ns - w->last_ns < period_ns / 4 * 3) w->fast++;
        if (++w->window >= RTSS_WATCH_WINDOW) {
            bad = w->fast > RTSS_WATCH_WINDOW / 2;
            w->window = w->fast = 0;
        }
    } else {
        w->window = w->fast = 0;
    }
    w->last_ns = holds ? now_ns : 0;
    w->last_period = holds ? period_ns : 0;
    return bad;
}

// The cap of a period in millihertz, rounded (16666666 ns is 60000); 0 for no period.
static inline int64_t rtss_cap_mhz(uint64_t period_ns) {
    return period_ns ? (int64_t)((1000000000000ull + period_ns / 2) / period_ns) : 0;
}

// ---- the state file (rtss_limit.cpp, triforce_rtss_profile.txt in the game folder): each RTSS profile a run changed,
// as it was before, so that a start after a run that did not close (a kill, a hang, a crash) puts it back. One line
// per profile, so executables that share the folder keep their own:
//   rtss1 <existed 0|1> <old limit> <old denominator> <written limit> <written denominator> <previous limit>
//         <previous denominator> <profile name>
// on one line. The name comes last and may hold spaces; lines that start with # are comments. The line takes a new
// value before the profile does, and the value written before it stays as the previous one until the profile holds the
// new one: a run that ends between the two leaves either value in the profile, and both are the game's.
#define RTSS_SAVED_HEADER                                                                                                 \
    "# RivaTuner Statistics Server's profiles as they were before the game changed them (gpu/vk/rtss_limit.cpp): existed,\n" \
    "# old limit and denominator, the game's limit and denominator, the game's previous ones, profile. The game puts a\n"   \
    "# profile back and removes its line when it closes; a start that finds a line puts that profile back first.\n"
enum { RTSS_NAME_MAX = 260 };

typedef struct {
    int existed;                   // the profile existed before the run changed it
    uint32_t old_limit, old_den;   // what it held then
    uint32_t limit, den;           // what the run wrote last
    uint32_t prev_limit, prev_den;   // what it wrote before, while the profile may still hold it; else limit and den
    char name[RTSS_NAME_MAX];      // the profile's name, the executable's (Triforce.exe)
} RtssSaved;

// The line of r, with its newline: its length, 0 when it does not fit n.
static inline size_t rtss_saved_format(char *out, size_t n, const RtssSaved *r) {
    int k = snprintf(out, n, "rtss1 %d %u %u %u %u %u %u %s\n", r->existed ? 1 : 0, r->old_limit, r->old_den,
                     r->limit, r->den, r->prev_limit, r->prev_den, r->name);
    return k > 0 && (size_t)k < n ? (size_t)k : 0;
}

// One line (len bytes, no newline) into r: 1 when it is a well-formed line of this format, 0 otherwise.
static inline int rtss_saved_parse(const char *line, size_t len, RtssSaved *r) {
    char buf[RTSS_NAME_MAX + 96];
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) len--;
    if (len >= sizeof buf) return 0;
    memcpy(buf, line, len);
    buf[len] = 0;
    int existed = -1, at = 0;
    unsigned v[6];
    if (sscanf(buf, "rtss1 %d %u %u %u %u %u %u %n", &existed, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &at) != 7 ||
        !at)
        return 0;
    size_t name = len - (size_t)at;
    if ((existed != 0 && existed != 1) || !v[1] || !v[3] || !v[5] || !name || name >= RTSS_NAME_MAX) return 0;
    r->existed = existed, r->old_limit = v[0], r->old_den = v[1], r->limit = v[2], r->den = v[3];
    r->prev_limit = v[4], r->prev_den = v[5];
    memcpy(r->name, buf + at, name + 1);
    return 1;
}

// Profile names compare as Windows compares file names: ASCII letters in either case.
static inline int rtss_name_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

// The line of profile name in the file's text (NUL-terminated): 1 and r, or 0. A line that is not well formed is
// passed over, and the lines after it are still read.
static inline int rtss_saved_find(const char *text, const char *name, RtssSaved *r) {
    for (const char *p = text; p && *p;) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (*p != '#' && rtss_saved_parse(p, len, r) && rtss_name_eq(r->name, name)) return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

// The file's text with profile name's line replaced by r's (r NULL: removed): the header, every other profile's
// well-formed line as it was, then r's. Comments, blank and malformed lines are dropped. Writes the text into out and
// returns its length (0 when it does not fit cap); *records is the number of profile lines in it, 0 when the file is
// to go.
static inline size_t rtss_saved_update(const char *text, const char *name, const RtssSaved *r, char *out, size_t cap,
                                       int *records) {
    int head = snprintf(out, cap, "%s", RTSS_SAVED_HEADER);
    size_t at = head > 0 ? (size_t)head : cap;
    *records = 0;
    if (at >= cap) return 0;
    for (const char *p = text; p && *p;) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        RtssSaved other;
        if (*p != '#' && rtss_saved_parse(p, len, &other) && !rtss_name_eq(other.name, name)) {
            size_t k = rtss_saved_format(out + at, cap - at, &other);
            if (!k) return 0;
            at += k, ++*records;
        }
        p = end ? end + 1 : NULL;
    }
    if (r) {
        size_t k = rtss_saved_format(out + at, cap - at, r);
        if (!k) return 0;
        at += k, ++*records;
    }
    return at;
}

// What a start does with a line a run that did not close left (r), given the profile as it is now: whether it exists
// and the limit and denominator it holds. Either value of the line is the game's (RtssSaved.prev_limit).
enum {
    RTSS_PLAN_NONE,     // no profile, and there was none before that run: as it was
    RTSS_PLAN_LEAVE,    // the player changed or removed the profile since: theirs as it is
    RTSS_PLAN_WRITE,    // it still holds what the run wrote: the old limit and denominator back
    RTSS_PLAN_DELETE,   // the same, for a profile that run made: removed
};
static inline int rtss_restore_plan(const RtssSaved *r, int exists, uint32_t limit, uint32_t den) {
    if (!exists) return r->existed ? RTSS_PLAN_LEAVE : RTSS_PLAN_NONE;
    den = den ? den : 1;
    if ((limit != r->limit || den != r->den) && (limit != r->prev_limit || den != r->prev_den)) return RTSS_PLAN_LEAVE;
    return r->existed ? RTSS_PLAN_WRITE : RTSS_PLAN_DELETE;
}

// ---- RTSS's own settings
// RTSS's profile files are ini texts, of which only the [Framerate] section's limits are read (rtss_limit.cpp
// read_settings, as files: no call into RTSS's hooks library): a frame rate limit (Limit over LimitDenominator), a
// frame time limit (LimitTime) and scanline sync (SyncScanline0 and SyncScanline1), 0 off for each.
typedef struct {
    int has_limit, has_den, has_time, has_scan0, has_scan1;   // the file has the key
    long limit, den, time, scan0, scan1;
} RtssProfile;

// n bytes at s are the word lit, in either case
static inline int rtss_word(const char *s, size_t n, const char *lit) {
    size_t i = 0;
    for (; i < n && lit[i]; i++) {
        int x = s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i], y = lit[i] >= 'A' && lit[i] <= 'Z' ? lit[i] + 32 : lit[i];
        if (x != y) return 0;
    }
    return i == n && !lit[i];
}

// A whole number at s (n bytes, a sign allowed): 1 and *v, or 0 for anything else. Held to +-1e9: no limit is larger.
static inline int rtss_number(const char *s, size_t n, long *v) {
    size_t i = 0;
    int neg = 0;
    int64_t x = 0;   // (a long is 32 bits on Windows: the digits are gathered in 64)
    if (i < n && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    if (i >= n) return 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        if (x <= 1000000000) x = x * 10 + (s[i] - '0');
    }
    if (x > 1000000000) x = 1000000000;
    *v = (long)(neg ? -x : x);
    return 1;
}

// The profile text of n bytes into p. A key that is there twice keeps its first value, as the system's ini reader does;
// a value that is not a whole number counts as a key that is not there.
static inline void rtss_profile_parse(const char *text, size_t n, RtssProfile *p) {
    memset(p, 0, sizeof *p);
    int in = 0;
    for (size_t i = 0; text && i < n;) {
        size_t e = i;
        while (e < n && text[e] != '\n') e++;
        size_t a = i, b = e;
        while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
        while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) b--;
        if (a < b && text[a] == '[') {
            in = rtss_word(text + a, b - a, "[Framerate]");
        } else if (in) {
            size_t eq = a;
            while (eq < b && text[eq] != '=') eq++;
            size_t ke = eq, vs = eq < b ? eq + 1 : b;
            while (ke > a && (text[ke - 1] == ' ' || text[ke - 1] == '\t')) ke--;
            while (vs < b && (text[vs] == ' ' || text[vs] == '\t')) vs++;
            long v = 0;
            if (eq < b && rtss_number(text + vs, b - vs, &v)) {
                const char *k = text + a;
                size_t kn = ke - a;
                if (rtss_word(k, kn, "Limit") && !p->has_limit) p->has_limit = 1, p->limit = v;
                else if (rtss_word(k, kn, "LimitDenominator") && !p->has_den) p->has_den = 1, p->den = v;
                else if (rtss_word(k, kn, "LimitTime") && !p->has_time) p->has_time = 1, p->time = v;
                else if (rtss_word(k, kn, "SyncScanline0") && !p->has_scan0) p->has_scan0 = 1, p->scan0 = v;
                else if (rtss_word(k, kn, "SyncScanline1") && !p->has_scan1) p->has_scan1 = 1, p->scan1 = v;
            }
        }
        i = e + 1;
    }
}

// RTSS's own [Framerate] settings for the game: the game's own profile (NULL: it has none), else the global one, and
// a key the game's profile lacks from the global one, as RTSS reads them.
typedef struct {
    long limit, limit_den, time, scan0, scan1;
} RtssSettings;

static inline RtssSettings rtss_settings_of(const RtssProfile *own, const RtssProfile *global) {
    static const RtssProfile none = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const RtssProfile *o = own ? own : &none, *g = global ? global : &none;
    RtssSettings s;
    s.limit = o->has_limit ? o->limit : g->has_limit ? g->limit : 0;
    s.limit_den = o->has_den ? o->den : g->has_den ? g->den : 1;
    s.time = o->has_time ? o->time : g->has_time ? g->time : 0;
    s.scan0 = o->has_scan0 ? o->scan0 : g->has_scan0 ? g->scan0 : 0;
    s.scan1 = o->has_scan1 ? o->scan1 : g->has_scan1 ? g->scan1 : 0;
    return s;
}

// RTSS holds the game's frames by a setting of its own.
static inline int rtss_settings_limit(const RtssSettings *s) { return s->limit > 0 || s->time || s->scan0 || s->scan1; }
// Its frame rate limit in millihertz, 0 none; and whether it limits another way, which is not acted on.
static inline int64_t rtss_settings_mhz(const RtssSettings *s) {
    return s->limit > 0 ? (int64_t)s->limit * 1000 / (s->limit_den > 0 ? s->limit_den : 1) : 0;
}
static inline int rtss_settings_other(const RtssSettings *s) { return s->time != 0 || s->scan0 != 0 || s->scan1 != 0; }

// "limit L/D, frame time limit T, scanline sync S0/S1": the values as the files hold them.
static inline void rtss_settings_text(char *out, size_t n, const RtssSettings *s) {
    snprintf(out, n, "limit %ld/%ld, frame time limit %ld, scanline sync %ld/%ld", s->limit, s->limit_den, s->time, s->scan0,
             s->scan1);
}

// RTSS has no part in the game's limit although it may list the game: the want keeps it out, or its profile cannot be
// written, its hooks library did not load or it never showed it holds the game (its profile put back, a setting of its
// own may still hold the game). A run whose frame checks gave up on RTSS does not count: RTSS was seen not to hold
// the game.
static inline int rtss_out_of_limit(int64_t want, int fail) {
    return want != RTSS_UNSET &&
           (want < 0 || fail == RTSS_FAIL_READONLY || fail == RTSS_FAIL_LIBRARY || fail == RTSS_FAIL_UNPROVEN);
}

// Two limiters: RTSS is out of the game's limit (rtss_out_of_limit), and while it lists the game a setting of its own
// still holds the frames (rtss_settings_limit), beside the game's own wait or Reflex.
static inline int rtss_two(int64_t want, int fail, int listed, int own_limit) {
    return rtss_out_of_limit(want, fail) && listed && own_limit;
}

// ---- (Triforce) the player's own limit, where the game cannot change the profile
// With RTSS_FAIL_READONLY or RTSS_FAIL_LIBRARY the game's limit is not in RTSS's profile. RTSS may still hold the
// game, by a frame rate limit the player set in RTSS: the game's own wait then steps aside for it, so that there is
// one limiter for a player without administrator rights too. Only while all of this is seen: RTSS runs and lists the
// game with Vulkan presents and its profile read; its own frame rate limit for the game is above 0 and at or under
// the limit the game wants (above it the game's own wait is the one that holds, and stepping aside would let the game
// past its cap); no frame time limit or scanline sync (another way of limiting, not acted on); that limit stayed
// RTSS_SETTLE_POLLS polls; RTSS measured a period of the game's frames between two polls that both saw the game
// listed, lately; a picture went out within RTSS_HOLD_IDLE_NS. A movie is held by that limit too (below). Each fresh
// period after a poll that stood aside is checked as rtss_decide checks its own, and a failure is tried again by the
// same rule.
typedef struct {
    int fail;            // RTSS_FAIL_NONE, RTSS_FAIL_NOT_LIMITING or RTSS_FAIL_FAST
    int over, held, listed;
    uint32_t t1, span;
    uint64_t fresh_ns;
    int64_t seen_mhz;    // RTSS's own limit at the poll before
    int polls;           // polls in a row with it
    uint64_t fail_ns;
    int64_t fail_want;
    int retries, gone;
} RtssOwnState;
#define RTSS_OWN_INIT {RTSS_FAIL_NONE, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0}

typedef struct {
    int64_t want;        // the limit the game wants RTSS to hold (> 0), in millihertz of presented frames
    int64_t limit_mhz;   // RTSS's own frame rate limit for the game (rtss_settings_mhz), 0 none or not read
    int other;           // rtss_settings_other
    int64_t movie_mhz;
    uint64_t now_ns, hosted_ns;
    int fast;
    RtssApp app;
} RtssOwnIn;

static inline RtssOut rtss_own_decide(RtssOwnState *s, const RtssOwnIn *in) {
    RtssOut o = {RTSS_DO_NOTHING, in->limit_mhz, 0, RTSS_WHY_NOT_RUNNING, 0, 0};
    const RtssApp *a = &in->app;
    if (s->fail) {
        if (!a->running) s->gone = 1;
        int again = a->running && s->gone                                                   ? 2
                    : in->want != s->fail_want                                              ? 3
                    : s->retries < RTSS_RETRIES && in->now_ns - s->fail_ns >= RTSS_RETRY_NS ? 1
                                                                                            : 0;
        if (again) {
            s->retries = again == 1 ? s->retries + 1 : 0;
            s->fail = RTSS_FAIL_NONE, s->gone = 0, s->over = s->held = 0;
            o.retry = again;
        }
    }
    int listed = a->running && a->listed;
    int fresh = listed && s->listed && a->t1 > a->t0 && a->frames && a->t1 != s->t1;
    if (fresh) s->span = a->t1 - a->t0, s->fresh_ns = in->now_ns;
    if (listed) s->t1 = a->t1;
    s->listed = listed;
    int held = s->held, over = s->over;
    s->held = s->over = 0;
    s->polls = in->limit_mhz == s->seen_mhz ? s->polls + 1 : 1;
    s->seen_mhz = in->limit_mhz;
    if (s->fail) return o.why = RTSS_WHY_FAILED, o;
    if (!a->running) return o;
    if (!a->listed) return o.why = RTSS_WHY_NOT_LISTED, o;
    if ((a->flags & RTSS_API_MASK) != RTSS_API_VULKAN) return o.why = RTSS_WHY_NOT_VULKAN, o;
    if (a->flags & RTSS_UPDATE_ASKED) return o.why = RTSS_WHY_UPDATE, o;
    if (in->other) return o.why = RTSS_WHY_OTHER, o;
    if (in->limit_mhz <= 0) return o.why = RTSS_WHY_NO_LIMIT, o;
    if (in->want <= 0 || in->limit_mhz > in->want) return o.why = RTSS_WHY_ABOVE, o;
    if (s->polls < RTSS_SETTLE_POLLS) return o.why = RTSS_WHY_SETTLING, o;
    uint64_t stale = (uint64_t)s->span * 2500000ull > RTSS_FRESH_NS ? (uint64_t)s->span * 2500000ull : RTSS_FRESH_NS;
    if (!s->fresh_ns || in->now_ns - s->fresh_ns > stale) return o.why = RTSS_WHY_NOT_COUNTING, o;
    if (in->fast && held) {
        s->fail = RTSS_FAIL_FAST, s->fail_ns = in->now_ns, s->fail_want = in->want, s->gone = 0;
        return o.why = RTSS_WHY_FAILED, o;
    }
    if (fresh && held) {
        double fps = rtss_fps(a);
        over = fps > (double)in->limit_mhz / 1000.0 * RTSS_OVER_SLACK ? over + 1 : 0;
        if (over >= RTSS_OVER_PERIODS) {
            s->fail = RTSS_FAIL_NOT_LIMITING, s->fail_ns = in->now_ns, s->fail_want = in->want, s->gone = 0;
            o.why = RTSS_WHY_FAILED, o.fps = fps;
            return o;
        }
    }
    s->over = over;
    // (a movie stays RTSS's here: the player's limit holds it whatever the game does, and the game's wait beside it
    // would be a second limiter. movie_mhz is not looked at)
    if (!in->hosted_ns) return o.why = RTSS_WHY_NO_PRESENT, o;
    if ((in->now_ns > in->hosted_ns ? in->now_ns - in->hosted_ns : 0) >= RTSS_HOLD_IDLE_NS) return o.why = RTSS_WHY_IDLE, o;
    s->held = o.holds = 1;
    return o.why = RTSS_WHY_HOLDS, o;
}

// Why the player's own limit does not hold the game, for the log.
static inline const char *rtss_own_why_text(int why, int fail) {
    switch (why) {
    case RTSS_WHY_FAILED:
        return fail == RTSS_FAIL_FAST ? "the frames came sooner than that limit" : "RivaTuner Statistics Server counted the game past that limit";
    case RTSS_WHY_NOT_RUNNING: return "RivaTuner Statistics Server is not running";
    case RTSS_WHY_NOT_LISTED: return "RivaTuner Statistics Server has not hooked the game";
    case RTSS_WHY_NOT_VULKAN: return "RivaTuner Statistics Server has not seen the game's Vulkan presents";
    case RTSS_WHY_UPDATE: return "RivaTuner Statistics Server has not read its changed profile yet";
    case RTSS_WHY_OTHER: return "RivaTuner Statistics Server limits by frame time or scanline sync, which is left beside the game's own wait";
    case RTSS_WHY_NO_LIMIT: return "RivaTuner Statistics Server has no frame rate limit of its own for the game";
    case RTSS_WHY_ABOVE: return "RivaTuner Statistics Server's own limit is above the game's";
    case RTSS_WHY_SETTLING: return "a new limit in RivaTuner Statistics Server: taken once it stays";
    case RTSS_WHY_NOT_COUNTING: return "RivaTuner Statistics Server has counted no frames of the game lately";
    case RTSS_WHY_MOVIE: return "a movie plays";
    case RTSS_WHY_NO_PRESENT: return "no picture has gone out yet";
    case RTSS_WHY_IDLE: return "no picture went out lately";
    default: return "RivaTuner Statistics Server holds the frames";
    }
}
