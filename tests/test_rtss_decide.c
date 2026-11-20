/* gpu/vk/rtss_decide.h and gpu/vk/pace_rule.h: the parts of RivaTuner Statistics Server as the game's frame limiter
   (gpu/vk/rtss_limit.cpp, gpu/vk/vk_pace.cpp) that make no system call. The shared memory's reader against a fake
   RTSSSharedMemoryV2, the worker's decisions poll by poll against a fake profile and a fake RTSS clock, the retry after a
   give-back, the hysteresis on a limit that changes in play, the player's own limit where the profile cannot be written,
   the frames' own check, the state file's lines, RTSS's profile texts and who holds a frame. Built as C and as C++ and
   run by tests/test_rtss_decide.py; prints each failed check and exits 1 on any.
   (Triforce, after NX2navi's tests/rtss_limit_check.c, whose cases most of these are, brought to this port's rules) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pace_rule.h"
#include "rtss_decide.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- a fake RTSSSharedMemoryV2: the header and application entries written as RTSS lays them out */
enum { PID = 4242, ESIZE = 1024, OFF = 0x100 };
static void shm_set(uint8_t *v, uint32_t sig, uint32_t ver, uint32_t esize, uint32_t off, uint32_t n) {
    uint32_t h[5] = {sig, ver, esize, off, n};
    memcpy(v, h, sizeof h);
}
static void app_put(uint8_t *v, uint32_t esize, uint32_t off, uint32_t slot, uint32_t pid, uint32_t t0, uint32_t t1,
                    uint32_t frames) {
    uint8_t *e = v + off + (size_t)slot * esize;
    uint32_t t[3] = {t0, t1, frames}, flags = RTSS_API_VULKAN | RTSS_UPDATE_ASKED;
    memcpy(e, &pid, 4);
    memcpy(e + 4 + 260, &flags, 4);   /* (dwFlags after the 260-byte path) */
    memcpy(e + RTSS_PERIOD_AT, t, sizeof t);
}

/* rtss_parse: where the entry is, and that a bad header, a short entry or an array past the memory finds nothing */
static void parse(void) {
    static uint8_t v[65536];
    RtssApp a;
    memset(v, 0, sizeof v);
    shm_set(v, RTSS_SIGNATURE, 0x0002000E, ESIZE, OFF, 3);
    app_put(v, ESIZE, OFF, 0, 7, 0, 0, 0);
    app_put(v, ESIZE, OFF, 1, 8, 0, 0, 0);
    app_put(v, ESIZE, OFF, 2, PID, 1000, 2000, 60);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(a.running && a.listed && a.t0 == 1000 && a.t1 == 2000 && a.frames == 60, "the third entry is ours");
    CHECK(a.flags == (RTSS_API_VULKAN | RTSS_UPDATE_ASKED), "its flags");
    CHECK(rtss_fps(&a) == 60.0, "60 frames in a second");
    rtss_parse(v, sizeof v, 99, &a);
    CHECK(a.running && !a.listed && rtss_fps(&a) == 0, "another process: RTSS runs, the game is not listed");
    shm_set(v, RTSS_SIGNATURE_DEAD, 0x0002000E, ESIZE, OFF, 3);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(!a.running && a.dead && !a.listed, "a closed RTSS's block");
    shm_set(v, 0, 0, 0, 0, 0);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(!a.running && !a.dead, "never initialised");
    shm_set(v, RTSS_SIGNATURE, 0x00010005, ESIZE, OFF, 3);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(!a.running, "a version before 2.0: not running");
    shm_set(v, RTSS_SIGNATURE, 0x0002000E, RTSS_ENTRY_MIN - 1, OFF, 3);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(a.running && !a.listed, "entries shorter than the period: not read");
    rtss_parse(v, 19, PID, &a);
    CHECK(!a.running, "a memory shorter than the header");
    rtss_parse(NULL, 0, PID, &a);
    CHECK(!a.running, "no memory");
    /* entries of the smallest size, ours the last that fits: found; past the memory's end: not read */
    memset(v, 0, sizeof v);
    shm_set(v, RTSS_SIGNATURE, 0x0002000E, RTSS_ENTRY_MIN, OFF, 1000);
    uint32_t fit = (uint32_t)((sizeof v - OFF) / RTSS_ENTRY_MIN) - 1;
    app_put(v, RTSS_ENTRY_MIN, OFF, fit, PID, 1000, 1500, 30);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(a.listed && a.frames == 30, "the last entry inside the memory is read");
    rtss_parse(v, OFF + (size_t)fit * RTSS_ENTRY_MIN + RTSS_ENTRY_MIN, PID, &a);
    CHECK(a.listed, "an entry ending exactly with the memory is read");
    rtss_parse(v, OFF + (size_t)fit * RTSS_ENTRY_MIN + RTSS_ENTRY_MIN - 1, PID, &a);
    CHECK(a.running && !a.listed, "an entry one byte past the memory is not read");
    /* wild sizes: an offset and an entry size near the 32-bit ends read nothing and wrap nothing */
    shm_set(v, RTSS_SIGNATURE, 0x0002000E, 0xFFFFFFFFu, 0xFFFFFF00u, 0xFFFFFFFFu);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(a.running && !a.listed, "wild sizes: nothing");
    /* more than RTSS_APPS_MAX entries: the walk stops there */
    size_t big = OFF + (size_t)5000 * RTSS_ENTRY_MIN;
    uint8_t *b = (uint8_t *)calloc(1, big);
    shm_set(b, RTSS_SIGNATURE, 0x0002000E, RTSS_ENTRY_MIN, OFF, 5000);
    app_put(b, RTSS_ENTRY_MIN, OFF, 4500, PID, 1000, 2000, 60);
    rtss_parse(b, big, PID, &a);
    CHECK(a.running && !a.listed, "an entry past %d is not looked at", RTSS_APPS_MAX);
    app_put(b, RTSS_ENTRY_MIN, OFF, 4095, PID, 1000, 2000, 60);
    rtss_parse(b, big, PID, &a);
    CHECK(a.listed, "entry %d is", RTSS_APPS_MAX - 1);
    free(b);
}

/* ---- a fake profile behind RtssOps, and one poll as the worker makes it */
typedef struct {
    int readonly, no_library, loaded;
    int64_t refuse_above;   /* RTSS refuses a limit above this (millihertz); 0 none */
    int64_t holds;          /* the limit the profile holds from this run; -1 none */
    int applies, firsts, clears, put_backs, last_put_back_why;
} Store;
static int f_apply(void *ctx, int64_t mhz, int first, int clear) {
    Store *st = (Store *)ctx;
    st->applies++, st->firsts += first;
    if (st->readonly) return RTSS_WROTE_READONLY;
    if (st->refuse_above && mhz > st->refuse_above) {
        if (!clear) return RTSS_WROTE_REFUSED;
        st->clears++, st->holds = 0;
        return RTSS_WROTE_REFUSED_CLEARED;
    }
    st->holds = mhz;
    return RTSS_WROTE_HELD;
}
static void f_put_back(void *ctx, const RtssOut *o) {
    Store *st = (Store *)ctx;
    st->put_backs++, st->holds = -1, st->last_put_back_why = o->why;
}

/* RTSS as the worker sees it: polls 500 ms apart; while it runs, lists the game and counts (counting) it measures a
   period of a second every second poll, with frames frames in it (the frame rate); the game presents (presenting) and
   the newest host present is a millisecond before the poll, else it stays where it was (0: none yet) */
typedef struct {
    RtssState s;
    Store st;
    RtssOps ops;
    int running, listed, counting, presenting, polls, fast;
    uint32_t flags, t0, t1, frames;
    int64_t movie_mhz;
    uint64_t now_ns, hosted_ns;
} Run;
static void run_init(Run *r) {
    memset(r, 0, sizeof *r);
    RtssState init = RTSS_STATE_INIT;
    r->s = init;
    r->st.holds = -1;
    r->ops.ctx = &r->st, r->ops.apply = f_apply, r->ops.put_back = f_put_back;
    r->running = r->listed = r->counting = r->presenting = 1;
    r->flags = RTSS_API_VULKAN, r->t0 = 1000, r->t1 = 2000, r->frames = 60;
    r->now_ns = 10000000000ull;
}
/* One poll for want, as rtss_limit.cpp poll makes it: the hooks library loads on the first poll that wants it and stays */
static RtssOut step_ex(Run *r, int64_t want, int current) {
    r->now_ns += 500000000ull;
    if (r->running && r->listed && r->counting && ++r->polls % 2 == 0) r->t0 = r->t1, r->t1 += 1000;
    if (r->presenting) r->hosted_ns = r->now_ns - 1000000;
    RtssIn in;
    memset(&in, 0, sizeof in);
    in.want = want, in.current = current, in.movie_mhz = r->movie_mhz, in.now_ns = r->now_ns, in.hosted_ns = r->hosted_ns;
    in.fast = r->fast, r->fast = 0;
    in.app.running = r->running, in.app.listed = r->running && r->listed, in.app.flags = r->flags;
    in.app.t0 = r->t0, in.app.t1 = r->t1, in.app.frames = r->frames;
    if (r->st.loaded) in.hooks = 1;
    else if (rtss_wants_hooks(&r->s, &in)) in.hooks = r->st.no_library ? 0 : (r->st.loaded = 1);
    else in.hooks = -1;
    return rtss_poll(&r->s, &in, &r->ops);
}
static RtssOut step(Run *r, int64_t want) { return step_ex(r, want, 1); }
/* polls until RTSS holds the game (at most n): the polls it took, n + 1 if never */
static int until_held(Run *r, int64_t want, int n) {
    for (int i = 1; i <= n; i++)
        if (step(r, want).holds) return i;
    return n + 1;
}
static void why_is(const RtssOut *o, const RtssState *s, const char *want, int line) {
    char t[160];
    rtss_why_text(t, sizeof t, o, s->fail);
    if (strcmp(t, want)) fails++, printf("FAIL line %d: the reason is '%s', not '%s'\n", line, t, want);
}
#define WHY(o, r, text) why_is(&(o), &(r).s, text, __LINE__)

/* RTSS's injection delay: it runs but has not hooked the game for 30 polls. The limit goes into the profile at the
   second poll (the first sees the want new), with no second write. Once RTSS lists the game the game's wait stands
   aside only after RTSS has seen its Vulkan presents, read the profile and measured a period of its frames since a poll
   that saw it listed */
static void injection_delay(void) {
    Run r;
    run_init(&r);
    r.listed = 0;
    RtssOut o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_NOTHING && !o.holds && o.why == RTSS_WHY_SETTLING, "a new want: not written yet");
    WHY(o, r, "a new limit: RivaTuner Statistics Server gets it once it stays");
    o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds && o.why == RTSS_WHY_NOT_LISTED, "written, not held yet");
    CHECK(r.st.firsts == 1 && r.s.applied == 60000 && r.s.changed, "the first write of the run");
    for (int i = 0; i < 30; i++) o = step(&r, 60000);
    CHECK(r.st.applies == 1 && !o.holds && o.why == RTSS_WHY_NOT_LISTED, "no write while not listed");
    WHY(o, r, "RivaTuner Statistics Server has not hooked the game yet");
    r.listed = 1, r.flags = 0;
    o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NOT_VULKAN, "listed with no API yet");
    WHY(o, r, "RivaTuner Statistics Server lists the game but has not seen its Vulkan presents yet");
    r.flags = 0x7;   /* (another API's value) */
    o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NOT_VULKAN, "another API");
    r.flags = RTSS_API_VULKAN | RTSS_UPDATE_ASKED;
    o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_UPDATE, "the profile not read yet");
    WHY(o, r, "RivaTuner Statistics Server has not read the game's limit from its profile yet");
    r.flags = RTSS_API_VULKAN, r.counting = 0;
    for (int i = 0; i < 6; i++) o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NOT_COUNTING, "no period for three seconds");
    WHY(o, r, "RivaTuner Statistics Server has measured no frames of the game lately");
    r.counting = 1;
    CHECK(until_held(&r, 60000, 3) <= 2 && r.st.applies == 1, "held once counting, no second write");
}

/* (Triforce) the period an entry holds when it is first seen is where the watch starts, not a sign of life: an entry a
   closed program of the same process id left never counts as RTSS measuring the game */
static void first_period(void) {
    Run r;
    run_init(&r);
    r.counting = 0;   /* (the entry holds one period, which never moves) */
    RtssOut o;
    for (int i = 0; i < 10; i++) o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NOT_COUNTING, "a period that was there at the first look and never moved");
    r.counting = 1;
    CHECK(until_held(&r, 60000, 3) <= 2, "held at the first period that moved");
}

/* RTSS quits while it limits, and comes back: the profile stays for its return */
static void quit(void) {
    Run r;
    run_init(&r);
    CHECK(until_held(&r, 60000, 6) <= 4, "held");
    r.running = 0;
    RtssOut o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NOT_RUNNING && o.act == RTSS_DO_NOTHING, "not running: the game limits");
    WHY(o, r, "RivaTuner Statistics Server is not running");
    CHECK(!r.st.put_backs && r.st.holds == 60000, "the profile stays");
    r.running = 1;
    CHECK(until_held(&r, 60000, 4) <= 3 && r.st.applies == 1, "back: held with no new write");
}

/* Profiles under Program Files without administrator rights: no write is tried again (rights do not change while the
   game runs) */
static void readonly(void) {
    Run r;
    run_init(&r);
    r.st.readonly = 1;
    step(&r, 60000);
    RtssOut o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds && o.why == RTSS_WHY_FAILED && r.s.fail == RTSS_FAIL_READONLY, "failed");
    CHECK(!r.s.changed, "nothing of the run in the profile");
    for (int i = 0; i < 300; i++) o = step(&r, i < 10 ? 60000 : 144000);
    CHECK(r.st.applies == 1 && o.why == RTSS_WHY_FAILED, "no write after it, a minute and more later and at another limit");
    WHY(o, r, "RivaTuner Statistics Server's profile for the game cannot be changed");
    CHECK(rtss_out_of_limit(60000, r.s.fail) && !rtss_fail_retried(r.s.fail), "out of the game's limit, not tried again");
}

/* A limit out of RTSS's range: refused and kept by the game; the earlier limit of this run is cleared to none; a limit
   in range goes to RTSS once it stays */
static void refused(void) {
    Run r;
    run_init(&r);
    r.st.refuse_above = 200000;
    step(&r, 60000);
    step(&r, 60000);
    step(&r, 240000);
    RtssOut o = step(&r, 240000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds && o.why == RTSS_WHY_REFUSED, "refused");
    WHY(o, r, "RivaTuner Statistics Server refused this limit");
    CHECK(r.s.refused == 240000 && r.s.applied == 0 && r.st.clears == 1 && r.st.holds == 0, "the 60 cleared to none");
    for (int i = 0; i < 5; i++) o = step(&r, 240000);
    CHECK(r.st.applies == 2 && o.why == RTSS_WHY_REFUSED && !o.holds, "not written again");
    o = step(&r, 144000);
    CHECK(o.act == RTSS_DO_NOTHING && o.why == RTSS_WHY_SETTLING, "144 settles first");
    o = step(&r, 144000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds && r.st.holds == 144000, "144 written");
    CHECK(until_held(&r, 144000, 3) <= 2, "held from the next polls");
    CHECK(r.st.firsts == 1, "one first write in the run (%d)", r.st.firsts);
    o = step(&r, 240000);
    CHECK(o.act == RTSS_DO_NOTHING && o.why == RTSS_WHY_REFUSED, "the refused limit stays with the game at once");
    run_init(&r);
    r.st.refuse_above = 200000;
    step(&r, 240000);
    o = step(&r, 240000);
    CHECK(o.why == RTSS_WHY_REFUSED && r.s.changed && r.st.clears == 1, "a first refusal clears too, as it stands");
}

/* RTSS's own frame count: each fresh period after a poll that stood aside is checked, and more than 1.05 times the
   limit for three of them in a row puts the profile back. 69 fps at 60 (1.15 times) is caught, 63 is not; a period RTSS
   no longer moves counts neither way; not listed never counts */
static void frame_count(void) {
    Run r;
    run_init(&r);
    r.frames = 144;
    RtssOut o = step(&r, 60000);
    int polls = 1;
    while (!r.s.fail && polls < 40) o = step(&r, 60000), polls++;
    CHECK(o.act == RTSS_DO_PUT_BACK && !o.holds && o.why == RTSS_WHY_FAILED && r.s.fail == RTSS_FAIL_NOT_LIMITING,
          "144 fps at 60 gives up");
    CHECK(polls <= 10, "after three fresh periods it held through (poll %d)", polls);
    CHECK(o.fps == 144.0 && r.st.put_backs == 1 && r.st.last_put_back_why == RTSS_WHY_FAILED && !r.s.changed,
          "put back, with the measured rate");
    o = step(&r, 60000);
    CHECK(o.why == RTSS_WHY_FAILED && r.st.applies == 1, "out until it is tried again");
    WHY(o, r, "RivaTuner Statistics Server did not hold the game to the limit");
    run_init(&r);
    r.frames = 69;
    for (int i = 0; i < 40 && !r.s.fail; i++) o = step(&r, 60000);
    CHECK(r.s.fail == RTSS_FAIL_NOT_LIMITING, "69 fps at 60 is caught");
    run_init(&r);
    r.frames = 63;   /* (1.05 times the limit exactly) */
    for (int i = 0; i < 40; i++) o = step(&r, 60000);
    CHECK(o.holds && !r.s.fail, "63 fps at 60 is not");
    run_init(&r);
    r.frames = 144, r.listed = 0;
    for (int i = 0; i < 20; i++) o = step(&r, 60000);
    CHECK(!r.s.fail && !r.s.over, "not listed: no count");
    run_init(&r);
    r.frames = 144, r.counting = 0;
    for (int i = 0; i < 20; i++) o = step(&r, 60000);
    CHECK(!r.s.fail && !r.s.over && !o.holds && o.why == RTSS_WHY_NOT_COUNTING, "a frozen period: not counting");
    /* two periods over, then one under: the count starts again */
    run_init(&r);
    r.frames = 144;
    for (int i = 0; i < 40 && r.s.over < 2; i++) step(&r, 60000);
    CHECK(r.s.over == 2, "two periods over (%d)", r.s.over);
    r.frames = 60;
    for (int i = 0; i < 2; i++) step(&r, 60000);
    CHECK(r.s.over == 0 && !r.s.fail, "one under: the count starts again");
    /* a want that changed while the poll ran: nothing decided, the count waits */
    run_init(&r);
    r.frames = 144;
    for (int i = 0; i < 40 && r.s.over < 1; i++) step(&r, 60000);
    o = step_ex(&r, 60000, 0);
    CHECK(o.why == RTSS_WHY_NEWER && !o.holds && o.act == RTSS_DO_NOTHING && r.s.over == 1, "a newer want: no count");
}

/* RTSS lists the game with the limit in its profile and pictures go out, but never shows its hold: it may hold the
   frames beside the game's wait, so after RTSS_UNPROVEN_POLLS such polls in a row the profile goes back. Each sign that
   never comes counts; a minimised window and RTSS not hooked do not count, and a poll that shows the hold starts the
   count again */
static void unproven(void) {
    static const struct { uint32_t flags; int period; const char *what, *why; } stuck[] = {
        {RTSS_API_VULKAN | RTSS_UPDATE_ASKED, 1, "the profile never read",
         "RivaTuner Statistics Server has not read the game's limit from its profile yet"},
        {0, 1, "no API seen", "RivaTuner Statistics Server lists the game but has not seen its Vulkan presents yet"},
        {0x7, 1, "another API's presents",
         "RivaTuner Statistics Server lists the game but has not seen its Vulkan presents yet"},
        {RTSS_API_VULKAN, 0, "no period measured", "RivaTuner Statistics Server has measured no frames of the game lately"},
    };
    Run r;
    RtssOut o;
    for (unsigned k = 0; k < sizeof stuck / sizeof *stuck; k++) {
        run_init(&r);
        r.flags = stuck[k].flags;
        if (!stuck[k].period) r.counting = 0, r.t0 = r.t1 = 0;
        step(&r, 60000);
        o = step(&r, 60000);
        CHECK(o.act == RTSS_DO_APPLY, "%s: written", stuck[k].what);
        int polls = 0;
        for (; polls < RTSS_UNPROVEN_POLLS - 1; polls++) o = step(&r, 60000);
        CHECK(!r.s.fail && !o.holds && !r.st.put_backs, "%s: %d polls, not yet", stuck[k].what, polls);
        WHY(o, r, stuck[k].why);
        o = step(&r, 60000);
        CHECK(o.act == RTSS_DO_PUT_BACK && o.why == RTSS_WHY_FAILED && r.s.fail == RTSS_FAIL_UNPROVEN && r.st.put_backs == 1 &&
                  r.st.last_put_back_why == RTSS_WHY_FAILED && !r.s.changed,
              "%s: put back at poll %d", stuck[k].what, RTSS_UNPROVEN_POLLS);
        for (int i = 0; i < 10; i++) o = step(&r, 60000);
        CHECK(r.st.put_backs == 1 && r.st.applies == 1 && o.why == RTSS_WHY_FAILED && !o.holds, "%s: once", stuck[k].what);
        WHY(o, r, "RivaTuner Statistics Server never showed it holds the game");
        CHECK(rtss_out_of_limit(60000, r.s.fail), "%s: a setting of RTSS's own may still hold the game", stuck[k].what);
    }
    run_init(&r);
    r.flags = RTSS_API_VULKAN | RTSS_UPDATE_ASKED;
    step(&r, 60000);
    step(&r, 60000);
    r.presenting = 0;
    for (int i = 0; i < 3 * RTSS_UNPROVEN_POLLS; i++) o = step(&r, 60000);
    CHECK(!r.s.fail && !r.st.put_backs && r.s.unproven == 0, "minimised: not counted");
    r.presenting = 1, r.listed = 0;
    for (int i = 0; i < 3 * RTSS_UNPROVEN_POLLS; i++) o = step(&r, 60000);
    CHECK(!r.s.fail && !r.st.put_backs && o.why == RTSS_WHY_NOT_LISTED, "not hooked: not counted");
    run_init(&r);
    r.flags = RTSS_API_VULKAN | RTSS_UPDATE_ASKED;
    step(&r, 60000);
    step(&r, 60000);
    for (int i = 0; i < RTSS_UNPROVEN_POLLS - 1; i++) step(&r, 60000);
    r.flags = RTSS_API_VULKAN;
    o = step(&r, 60000);
    CHECK(o.holds && r.s.unproven == 0, "shown once: the count starts again");
    r.flags = RTSS_API_VULKAN | RTSS_UPDATE_ASKED;
    for (int i = 0; i < RTSS_UNPROVEN_POLLS - 1; i++) o = step(&r, 60000);
    CHECK(!r.s.fail && o.why == RTSS_WHY_UPDATE, "%d polls again: not yet", RTSS_UNPROVEN_POLLS - 1);
    o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_PUT_BACK && r.s.fail == RTSS_FAIL_UNPROVEN, "the full count again: put back");
}

/* No picture went out for 200 ms (a minimised window) or none yet: RTSS has nothing to hold, and the game's wait takes
   the frames back until pictures go out again. Before the first picture nothing is written */
static void idle(void) {
    Run r;
    run_init(&r);
    r.presenting = 0;
    RtssOut o;
    for (int i = 0; i < 4; i++) o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NO_PRESENT && !r.st.applies, "no host present yet: nothing written");
    WHY(o, r, "no picture has gone out yet");
    r.presenting = 1;
    o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds, "written at the first picture, the want having stayed");
    CHECK(until_held(&r, 60000, 3) <= 2, "held once pictures go out");
    r.presenting = 0;
    r.hosted_ns = r.now_ns + 500000000ull - 199000000ull;   /* (the next poll comes 199 ms after the last present) */
    o = step(&r, 60000);
    CHECK(o.holds, "199 ms: still held");
    o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_IDLE, "minimised");
    WHY(o, r, "no picture went out lately: RivaTuner has none to hold");
    r.presenting = 1;
    o = step(&r, 60000);
    CHECK(o.holds && r.st.applies == 1, "held again, no write");
}

/* Kept out and let in again: the first settled kept-out poll puts back what the run wrote, once; a limit afterwards is
   written once it stays and is the run's first write again */
static void kept_out(void) {
    Run r;
    run_init(&r);
    CHECK(until_held(&r, 60000, 6) <= 4, "held");
    RtssOut o = step(&r, RTSS_OUT_AUTO);
    CHECK(o.act == RTSS_DO_NOTHING && o.why == RTSS_WHY_OUT && !o.holds && r.st.holds == 60000,
          "the game's wait at once, the profile once the want stays");
    o = step(&r, RTSS_OUT_AUTO);
    CHECK(o.act == RTSS_DO_PUT_BACK && o.why == RTSS_WHY_OUT && !o.holds && r.st.holds == -1, "put back");
    CHECK(r.st.last_put_back_why == RTSS_WHY_OUT, "the put back says why");
    o = step(&r, RTSS_OUT_AUTO);
    CHECK(o.act == RTSS_DO_NOTHING && r.st.put_backs == 1, "once");
    for (int i = 0; i < 3; i++) o = step(&r, RTSS_OUT_DLSSG);
    CHECK(o.act == RTSS_DO_NOTHING && o.mhz == RTSS_OUT_DLSSG && r.st.put_backs == 1, "a second reason: nothing more");
    WHY(o, r, "RivaTuner Statistics Server kept out: DLSS frame generation shows its frames on a pace of its own");
    o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_NOTHING && o.why == RTSS_WHY_SETTLING, "a limit again: settles first");
    o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds && r.st.firsts == 2, "written, the run's first write again");
    CHECK(until_held(&r, 60000, 3) <= 2, "held from the next polls");
    /* kept out from the start: the hooks library is never wanted, and nothing is written or put back */
    run_init(&r);
    for (int i = 0; i < 10; i++) o = step(&r, RTSS_OUT_SETTING);
    CHECK(!r.st.loaded && !r.st.applies && !r.st.put_backs && o.why == RTSS_WHY_OUT, "read only");
    WHY(o, r, "RivaTuner Statistics Server kept out: HOS_RTSS=0");
    run_init(&r);
    for (int i = 0; i < 10; i++) o = step(&r, RTSS_OUT_AUTO);
    CHECK(!r.st.loaded && !r.st.applies, "frame generation on Auto from the start: nothing loaded or written");
    CHECK(strstr(rtss_out_text(RTSS_OUT_AUTO), "Auto") && !strcmp(rtss_out_limiter(RTSS_OUT_DLSSG), "the game's own wait or Reflex"),
          "the reasons' texts");
}

/* A want that flips at every poll (a limit, then kept out) never stays two polls: the profile keeps the limit written
   before and the game's wait holds the frames while the want is new. From the start the same flips write nothing */
static void churn(void) {
    Run r;
    run_init(&r);
    step(&r, 60000);
    step(&r, 60000);
    RtssOut o;
    for (int i = 0; i < 20; i++) {
        o = step(&r, i & 1 ? 60000 : RTSS_OUT_AUTO);
        CHECK((i & 1) || !o.holds, "kept out at poll %d: the game's wait", i);
    }
    CHECK(r.st.applies == 1 && !r.st.put_backs && r.st.holds == 60000, "%d writes, %d put backs", r.st.applies, r.st.put_backs);
    run_init(&r);
    for (int i = 0; i < 20; i++) step(&r, i & 1 ? 60000 : RTSS_OUT_AUTO);
    CHECK(!r.st.applies && !r.st.put_backs, "from the start: nothing written");
    run_init(&r);
    for (int i = 0; i < 8; i++) step(&r, i & 2 ? RTSS_OUT_AUTO : 60000);
    CHECK(r.st.applies == 2 && r.st.put_backs == 2, "two polls each: %d writes, %d put backs", r.st.applies, r.st.put_backs);
}

/* (Triforce) The hysteresis on a limit that changes in play (the frames shown for each of the game's, pace_rule.h): a
   higher limit goes into the profile after two polls, a lower one after ten; while a lower one waits the game's wait
   holds the frames and the profile's higher limit holds none of them. A limit that flickers between two is written once,
   the higher one, however it flickers */
static void hysteresis(void) {
    Run r;
    run_init(&r);
    CHECK(until_held(&r, 120000, 6) <= 4 && r.st.holds == 120000, "120 held");
    RtssOut o = step(&r, 240000);
    CHECK(o.why == RTSS_WHY_SETTLING && !o.holds, "240: settling, the game's wait meanwhile");
    o = step(&r, 240000);
    CHECK(o.act == RTSS_DO_APPLY && r.st.holds == 240000 && r.st.applies == 2, "240 written at the second poll");
    CHECK(until_held(&r, 240000, 3) <= 2, "240 held");
    int polls = 0;
    do o = step(&r, 120000), polls++;
    while (o.act != RTSS_DO_APPLY && polls < 40);
    CHECK(polls == RTSS_SETTLE_DOWN_POLLS && r.st.holds == 120000, "120 again written at poll %d", polls);
    /* a limit that flickers, every poll, every two, every four */
    static const int each[] = {1, 2, 4};
    for (unsigned k = 0; k < sizeof each / sizeof *each; k++) {
        run_init(&r);
        until_held(&r, 120000, 6);
        const int before = r.st.applies;
        int held_high = 0, held_low = 0;
        for (int i = 0; i < 400; i++) {
            const int64_t w = (i / each[k]) & 1 ? 240000 : 120000;
            o = step(&r, w);
            CHECK(!o.holds || r.st.holds == w, "flicker every %d: held only at the limit the profile holds", each[k]);
            if (o.holds) *(w == 240000 ? &held_high : &held_low) += 1;
        }
        const int writes = r.st.applies - before;
        CHECK(writes <= 1, "flicker every %d poll(s): %d writes in 400 polls", each[k], writes);
        CHECK(each[k] == 1 ? writes == 0 : writes == 1 && r.st.holds == 240000 && held_high > 0 && held_low <= each[k],
              "flicker every %d: %d writes, held %d at the higher and %d at the lower", each[k], writes, held_high, held_low);
    }
    /* a flicker slower than the hysteresis is followed, a write at each change */
    run_init(&r);
    until_held(&r, 120000, 6);
    for (int i = 0; i < 200; i++) step(&r, (i / 20) & 1 ? 240000 : 120000);
    CHECK(r.st.applies == 1 + 9, "a change every 10 s is followed, a write at each of the 9 (%d writes)", r.st.applies);
}

/* A movie under RTSS: the want stays the play limit (Triforce's movies are paced at the cap, so their rate is the want's)
   and the game's wait holds the movie; the profile keeps the limit through it, with no write */
static void movie(void) {
    Run r;
    run_init(&r);
    CHECK(until_held(&r, 144000, 6) <= 4, "held");
    r.movie_mhz = 144000;
    RtssOut o = step(&r, 144000);
    CHECK(o.act == RTSS_DO_NOTHING && !o.holds && o.why == RTSS_WHY_MOVIE, "a movie: no write, the game's wait's");
    for (int i = 0; i < 10; i++) o = step(&r, 144000);
    CHECK(!o.holds && o.why == RTSS_WHY_MOVIE && r.st.applies == 1, "through the movie");
    WHY(o, r, "a movie plays: the game keeps its pictures on time");
    r.movie_mhz = 0;
    o = step(&r, 144000);
    CHECK(o.act == RTSS_DO_NOTHING && o.holds && o.why == RTSS_WHY_HOLDS && r.st.applies == 1, "held again after it");
    CHECK(rtss_target(60000, 60000) == 60000 && rtss_target(59940, 60000) == 0 && rtss_target(0, 60000) == 0 &&
              rtss_target(144000, 0) == 144000 && rtss_target(RTSS_OUT_AUTO, 60000) == RTSS_OUT_AUTO,
          "rtss_target");
}

/* No want yet: nothing at all. The hooks library that does not load fails the run */
static void unset_and_library(void) {
    Run r;
    run_init(&r);
    RtssOut o = step(&r, RTSS_UNSET);
    CHECK(o.why == RTSS_WHY_UNSET && o.act == RTSS_DO_NOTHING && !r.st.loaded, "no want");
    run_init(&r);
    r.st.no_library = 1;
    o = step(&r, 60000);
    CHECK(o.why == RTSS_WHY_FAILED && r.s.fail == RTSS_FAIL_LIBRARY && !r.st.applies, "the library did not load");
    WHY(o, r, "RivaTuner Statistics Server's hooks library did not load");
    r.st.no_library = 0;
    for (int i = 0; i < 300; i++) o = step(&r, 60000);
    CHECK(o.why == RTSS_WHY_FAILED && !r.st.loaded, "not tried again");
    run_init(&r);
    r.running = 0;
    o = step(&r, 60000);
    CHECK(!r.st.loaded && o.why == RTSS_WHY_NOT_RUNNING, "not loaded while RTSS does not run");
}

/* (Triforce) The retry after a give-back RTSS itself caused (its count past the limit, 30 s without its hold, frames that
   came too soon): a minute later, three times; at once and with the count afresh when RTSS was closed and runs again, or
   another limit is wanted. A kept-out want is never a reason to try */
static void fail_now(Run *r, int64_t want) {
    r->frames = 144;
    for (int i = 0; i < 60 && !r->s.fail; i++) step(r, want);
    r->frames = (uint32_t)(want / 1000);
}
static void retry(void) {
    Run r;
    run_init(&r);
    fail_now(&r, 60000);
    CHECK(r.s.fail == RTSS_FAIL_NOT_LIMITING && rtss_fail_retried(r.s.fail), "given back");
    RtssOut o;
    int polls = 0, tries = 0;
    for (; polls < 2000; polls++) {
        o = step(&r, 60000);
        if (o.retry) {
            CHECK(o.retry == 1, "by the clock");
            tries++;
            CHECK((uint64_t)polls * 500000000ull + 500000000ull >= RTSS_RETRY_NS, "not before a minute (poll %d)", polls);
            fail_now(&r, 60000);
            polls = 0;
            if (tries == RTSS_RETRIES) break;
        }
    }
    CHECK(tries == RTSS_RETRIES && r.s.retries == RTSS_RETRIES, "tried %d times", tries);
    for (int i = 0; i < 1000; i++) o = step(&r, 60000);
    CHECK(o.why == RTSS_WHY_FAILED && !o.retry && r.s.fail, "then no more by the clock");
    o = step(&r, RTSS_OUT_AUTO);
    o = step(&r, RTSS_OUT_AUTO);
    CHECK(!o.retry && r.s.fail, "a kept-out want: not a try");
    o = step(&r, 144000);
    CHECK(o.retry == 3 && !r.s.fail && r.s.retries == 0, "another limit: at once, the count afresh");
    /* the frames held once the try settles: one limiter again */
    r.frames = 144;
    CHECK(until_held(&r, 144000, 8) <= 6, "held at the new limit");
    /* RTSS closed and started again */
    run_init(&r);
    fail_now(&r, 60000);
    o = step(&r, 60000);
    CHECK(!o.retry && r.s.fail, "not at once");
    r.running = 0;
    for (int i = 0; i < 3; i++) o = step(&r, 60000);
    CHECK(!o.retry && r.s.fail && r.s.gone, "closed: noted");
    r.running = 1;
    o = step(&r, 60000);
    CHECK(o.retry == 2 && !r.s.fail, "runs again: at once");
    CHECK(until_held(&r, 60000, 8) <= 6, "held again");
    /* the unproven give-back and the frames' own check are tried again too */
    run_init(&r);
    r.flags = RTSS_API_VULKAN | RTSS_UPDATE_ASKED;
    for (int i = 0; i < RTSS_UNPROVEN_POLLS + 4; i++) step(&r, 60000);
    CHECK(r.s.fail == RTSS_FAIL_UNPROVEN, "unproven");
    r.flags = RTSS_API_VULKAN;
    for (int i = 0; i < 130 && !step(&r, 60000).retry; i++) {}
    CHECK(!r.s.fail, "unproven: tried again a minute later");
    CHECK(until_held(&r, 60000, 8) <= 6, "and held");
    run_init(&r);
    until_held(&r, 60000, 6);
    r.fast = 1;
    o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_PUT_BACK && r.s.fail == RTSS_FAIL_FAST && r.st.put_backs == 1, "frames too soon: put back");
    WHY(o, r, "the frames came sooner than the limit while RivaTuner Statistics Server was to hold them");
    for (int i = 0; i < 130 && !step(&r, 60000).retry; i++) {}
    CHECK(!r.s.fail, "frames too soon: tried again a minute later");
    /* fast before any hold counts for nothing */
    run_init(&r);
    r.fast = 1;
    step(&r, 60000);
    CHECK(!r.s.fail, "too soon while the game's wait held them: nothing");
}

/* (Triforce) the player's own limit where the game cannot write the profile (rtss_own_decide) */
typedef struct {
    RtssOwnState s;
    int running, listed, counting, presenting, polls, fast, other;
    uint32_t flags, t0, t1, frames;
    int64_t limit, movie_mhz;
    uint64_t now_ns, hosted_ns;
} Own;
static void own_init(Own *r, int64_t limit) {
    memset(r, 0, sizeof *r);
    RtssOwnState init = RTSS_OWN_INIT;
    r->s = init;
    r->running = r->listed = r->counting = r->presenting = 1;
    r->flags = RTSS_API_VULKAN, r->t0 = 1000, r->t1 = 2000, r->frames = (uint32_t)(limit / 1000), r->limit = limit;
    r->now_ns = 10000000000ull;
}
static RtssOut own_step(Own *r, int64_t want) {
    r->now_ns += 500000000ull;
    if (r->running && r->listed && r->counting && ++r->polls % 2 == 0) r->t0 = r->t1, r->t1 += 1000;
    if (r->presenting) r->hosted_ns = r->now_ns - 1000000;
    RtssOwnIn in;
    memset(&in, 0, sizeof in);
    in.want = want, in.limit_mhz = r->limit, in.other = r->other, in.movie_mhz = r->movie_mhz;
    in.now_ns = r->now_ns, in.hosted_ns = r->hosted_ns, in.fast = r->fast, r->fast = 0;
    in.app.running = r->running, in.app.listed = r->running && r->listed, in.app.flags = r->flags;
    in.app.t0 = r->t0, in.app.t1 = r->t1, in.app.frames = r->frames;
    return rtss_own_decide(&r->s, &in);
}
static void own_limit(void) {
    Own r;
    RtssOut o;
    own_init(&r, 60000);
    int polls = 0;
    do o = own_step(&r, 60000), polls++;
    while (!o.holds && polls < 10);
    CHECK(o.holds && o.mhz == 60000 && polls <= 4, "the player's 60 at the game's 60: it holds, the game's wait aside (%d)", polls);
    own_init(&r, 50000);
    for (int i = 0; i < 6; i++) o = own_step(&r, 60000);
    CHECK(o.holds, "under the game's: it holds");
    own_init(&r, 90000);
    for (int i = 0; i < 6; i++) o = own_step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_ABOVE, "above the game's: the game's wait holds the frames");
    own_init(&r, 0);
    for (int i = 0; i < 6; i++) o = own_step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NO_LIMIT, "no limit of the player's own");
    own_init(&r, 60000);
    r.other = 1;
    for (int i = 0; i < 6; i++) o = own_step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_OTHER, "a frame time limit or scanline sync: not acted on");
    own_init(&r, 60000);
    r.listed = 0;
    for (int i = 0; i < 6; i++) o = own_step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NOT_LISTED, "not hooked");
    own_init(&r, 60000);
    r.flags = RTSS_API_VULKAN | RTSS_UPDATE_ASKED;
    for (int i = 0; i < 6; i++) o = own_step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_UPDATE, "the profile not read yet");
    own_init(&r, 60000);
    r.counting = 0;
    for (int i = 0; i < 10; i++) o = own_step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NOT_COUNTING, "not counting");
    own_init(&r, 60000);
    for (int i = 0; i < 6; i++) own_step(&r, 60000);
    r.movie_mhz = 60000;
    o = own_step(&r, 60000);
    CHECK(o.holds, "a movie: the player's limit holds it too, with no second limiter beside it");
    r.movie_mhz = 0, r.presenting = 0;
    for (int i = 0; i < 2; i++) o = own_step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_IDLE, "minimised");
    r.presenting = 1;
    o = own_step(&r, 60000);
    CHECK(o.holds, "back");
    r.limit = 55000;
    o = own_step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_SETTLING, "the player changed it: once it stays");
    for (int i = 0; i < 2; i++) o = own_step(&r, 60000);
    CHECK(o.holds && o.mhz == 55000, "the new one holds");
    /* its count past the limit and frames too soon give it back, and it is tried again as RTSS's own are */
    own_init(&r, 60000);
    for (int i = 0; i < 6; i++) own_step(&r, 60000);
    r.frames = 144;
    for (int i = 0; i < 20 && !r.s.fail; i++) o = own_step(&r, 60000);
    CHECK(r.s.fail == RTSS_FAIL_NOT_LIMITING && !o.holds, "the player's limit does not hold the game: given back");
    r.frames = 60;
    for (int i = 0; i < 130 && !own_step(&r, 60000).retry; i++) {}
    CHECK(!r.s.fail, "tried again a minute later");
    own_init(&r, 60000);
    for (int i = 0; i < 6; i++) own_step(&r, 60000);
    r.fast = 1;
    o = own_step(&r, 60000);
    CHECK(r.s.fail == RTSS_FAIL_FAST && !o.holds, "frames too soon: given back");
    o = own_step(&r, 120000);
    CHECK(o.retry == 3 && !r.s.fail, "another limit wanted: at once");
    CHECK(!strcmp(rtss_own_why_text(RTSS_WHY_ABOVE, 0), "RivaTuner Statistics Server's own limit is above the game's"), "text");
}

/* The profile's values for a limit: whole rates as they are, a fraction over 1000 where RTSS knows the denominator,
   else rounded down, so a limit never holds the game above what was asked */
static void limit_value(void) {
    static const struct { int64_t mhz; int has_den; uint32_t limit, den; } cases[] = {
        {59940, 0, 59, 1},   {59940, 1, 59940, 1000}, {60000, 0, 60, 1},  {60000, 1, 60, 1},
        {143856, 0, 143, 1}, {143856, 1, 143856, 1000}, {0, 0, 0, 1},     {240000, 1, 240, 1}, {1920000, 1, 1920, 1}};
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        uint32_t l = 7, d = 7;
        rtss_limit_value(cases[i].mhz, cases[i].has_den, &l, &d);
        CHECK(l == cases[i].limit && d == cases[i].den, "%lld mHz, denominator %d: %u/%u, not %u/%u", (long long)cases[i].mhz,
              cases[i].has_den, l, d, cases[i].limit, cases[i].den);
    }
}

/* The state file (triforce_rtss_profile.txt): its lines, the file's text as the worker rewrites it, and what a start
   does with a line that a run which did not close left behind */
static void saved(void) {
    RtssSaved a = {0, 0, 1, 60, 1, 60, 1, "Triforce.exe"}, r;
    char line[400], out[2048];
    size_t n = rtss_saved_format(line, sizeof line, &a);
    CHECK(n == strlen(line) && !strcmp(line, "rtss1 0 0 1 60 1 60 1 Triforce.exe\n"), "the line: %s", line);
    CHECK(rtss_saved_parse(line, n - 1, &r) && !r.existed && !r.old_limit && r.old_den == 1 && r.limit == 60 && r.den == 1 &&
              r.prev_limit == 60 && r.prev_den == 1 && !strcmp(r.name, "Triforce.exe"),
          "read back");
    const char *spaced = "rtss1 1 59940 1000 144 1 60 1 My Game.exe\r";
    CHECK(rtss_saved_parse(spaced, strlen(spaced), &r) && r.existed && r.old_limit == 59940 && r.old_den == 1000 &&
              r.limit == 144 && r.prev_limit == 60 && !strcmp(r.name, "My Game.exe"),
          "a name with a space, a line ending in CR");
    static const char *bad[] = {"rtss1 2 0 1 60 1 60 1 x.exe", "rtss1 0 0 0 60 1 60 1 x.exe", "rtss1 0 0 1 60 0 60 1 x.exe",
                                "rtss1 0 0 1 60 1 60 0 x.exe", "rtss1 0 0 1 60 1 60 1",       "rtss1 0 0 1 60 1 60 1 ",
                                "rtss2 0 0 1 60 1 60 1 x.exe", "rtss1 0 0 1 x 1 60 1 x.exe",  "rtss1 0 0 1 60 1 x.exe",
                                "",                            "# rtss1 0 0 1 60 1 60 1 x.exe"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!rtss_saved_parse(bad[i], strlen(bad[i]), &r), "not a line: '%s'", bad[i]);
    CHECK(!rtss_saved_format(line, 20, &a), "a line that does not fit");
    const char *text = RTSS_SAVED_HEADER "rtss1 9 garbage\nrtss1 1 30 1 144 1 144 1 My Game.exe\nrtss1 0 0 1 60 1 60 1 TRIFORCE.EXE";
    CHECK(rtss_saved_find(text, "Triforce.exe", &r) && r.limit == 60 && !r.existed, "ours, the last line, no newline");
    CHECK(rtss_saved_find(text, "my game.exe", &r) && r.limit == 144 && r.old_limit == 30, "the other one");
    CHECK(!rtss_saved_find(text, "Triforce-dev.exe", &r) && !rtss_saved_find("", "Triforce.exe", &r), "none");
    int records = -1;
    RtssSaved a2 = a;
    a2.limit = 144;
    n = rtss_saved_update(text, "Triforce.exe", &a2, out, sizeof out, &records);
    CHECK(n == strlen(out) && records == 2 &&
              !strcmp(out, RTSS_SAVED_HEADER "rtss1 1 30 1 144 1 144 1 My Game.exe\nrtss1 0 0 1 144 1 60 1 Triforce.exe\n"),
          "replaced, the other executable's line kept:\n%s", out);
    char again[2048];
    n = rtss_saved_update(out, "TRIFORCE.exe", NULL, again, sizeof again, &records);
    CHECK(records == 1 && !strcmp(again, RTSS_SAVED_HEADER "rtss1 1 30 1 144 1 144 1 My Game.exe\n"), "removed:\n%s", again);
    n = rtss_saved_update(again, "My Game.exe", NULL, out, sizeof out, &records);
    CHECK(n && !records, "the last line removed: no record left, the file goes");
    n = rtss_saved_update("", "Triforce.exe", &a, out, sizeof out, &records);
    CHECK(records == 1 && !strcmp(out, RTSS_SAVED_HEADER "rtss1 0 0 1 60 1 60 1 Triforce.exe\n"), "a new file:\n%s", out);
    CHECK(!rtss_saved_update(text, "Triforce.exe", &a, out, sizeof RTSS_SAVED_HEADER + 10, &records), "a text that does not fit");
    RtssSaved made = {0, 0, 1, 60, 1, 60, 1, "Triforce.exe"}, had = {1, 30, 1, 60, 1, 60, 1, "Triforce.exe"};
    CHECK(rtss_restore_plan(&made, 1, 60, 1) == RTSS_PLAN_DELETE, "the run's own profile: removed");
    CHECK(rtss_restore_plan(&made, 1, 75, 1) == RTSS_PLAN_LEAVE, "the player set 75 since: theirs");
    CHECK(rtss_restore_plan(&made, 0, 0, 1) == RTSS_PLAN_NONE, "the player removed it, or the run was killed before its write");
    CHECK(rtss_restore_plan(&had, 1, 60, 1) == RTSS_PLAN_WRITE, "the player's profile with 30/1 before: 30/1 back");
    CHECK(rtss_restore_plan(&had, 1, 30, 1) == RTSS_PLAN_LEAVE, "killed before the write landed: the player's 30/1");
    CHECK(rtss_restore_plan(&had, 0, 0, 1) == RTSS_PLAN_LEAVE, "the player removed their profile since: left so");
    CHECK(rtss_restore_plan(&had, 1, 60, 0) == RTSS_PLAN_WRITE, "an RTSS without the denominator reads none: 1");
    RtssSaved frac = {1, 0, 1, 59940, 1000, 59940, 1000, "Triforce.exe"};
    CHECK(rtss_restore_plan(&frac, 1, 59940, 1) == RTSS_PLAN_LEAVE && rtss_restore_plan(&frac, 1, 59940, 1000) == RTSS_PLAN_WRITE,
          "the denominator is compared too");
    RtssSaved moving = {1, 30, 1, 144, 1, 60, 1, "Triforce.exe"}, moving_made = {0, 0, 1, 144, 1, 60, 1, "Triforce.exe"};
    CHECK(rtss_restore_plan(&moving, 1, 60, 1) == RTSS_PLAN_WRITE && rtss_restore_plan(&moving, 1, 144, 1) == RTSS_PLAN_WRITE,
          "either value of the game's: 30/1 back");
    CHECK(rtss_restore_plan(&moving_made, 1, 60, 1) == RTSS_PLAN_DELETE && rtss_restore_plan(&moving_made, 1, 144, 1) == RTSS_PLAN_DELETE,
          "either value of the game's, in a profile the run made: removed");
    CHECK(rtss_restore_plan(&moving, 1, 75, 1) == RTSS_PLAN_LEAVE && rtss_restore_plan(&moving, 1, 60, 1000) == RTSS_PLAN_LEAVE,
          "another value: the player's");
}

/* RTSS's profile texts as files: the [Framerate] section's keys, the game's own over the global one */
static void profiles(void) {
    RtssProfile g, o;
    const char *global = "[OSD]\r\nEnableOSD=1\r\nLimit=99\r\n[Framerate]\r\nLimit=0\r\nLimitDenominator=1\r\nLimitTime=0\r\n"
                         "SyncScanline0=0\r\nSyncScanline1=0\r\n[Hooking]\r\nLimit=5\r\n";
    rtss_profile_parse(global, strlen(global), &g);
    CHECK(g.has_limit && g.limit == 0 && g.has_den && g.den == 1 && g.has_time && !g.time && g.has_scan0 && g.has_scan1,
          "the Global file as RTSS writes it: only [Framerate] read");
    const char *own = " [framerate] \nLimit = 59940\nLimitDenominator=1000\nlimit=30\n";
    rtss_profile_parse(own, strlen(own), &o);
    CHECK(o.has_limit && o.limit == 59940 && o.den == 1000 && !o.has_time, "spaces, case, a key twice keeps its first");
    RtssSettings s = rtss_settings_of(&o, &g);
    CHECK(rtss_settings_mhz(&s) == 59940 && !rtss_settings_other(&s) && rtss_settings_limit(&s), "59.94 of the game's own");
    s = rtss_settings_of(NULL, &g);
    CHECK(rtss_settings_mhz(&s) == 0 && !rtss_settings_limit(&s), "none of its own, none global");
    const char *glim = "[Framerate]\nLimit=144\n";
    rtss_profile_parse(glim, strlen(glim), &g);
    s = rtss_settings_of(NULL, &g);
    CHECK(rtss_settings_mhz(&s) == 144000, "a global 144");
    const char *none = "[Framerate]\nLimit=0\n";
    rtss_profile_parse(none, strlen(none), &o);
    s = rtss_settings_of(&o, &g);
    CHECK(rtss_settings_mhz(&s) == 0, "the game's own says none over a global 144");
    const char *time = "[Framerate]\nLimitTime=16667\n", *scan = "[Framerate]\nSyncScanline0=-40\n";
    rtss_profile_parse(time, strlen(time), &o);
    s = rtss_settings_of(&o, NULL);
    CHECK(rtss_settings_other(&s) && rtss_settings_limit(&s), "a frame time limit");
    rtss_profile_parse(scan, strlen(scan), &o);
    s = rtss_settings_of(&o, NULL);
    CHECK(rtss_settings_other(&s), "scanline sync");
    const char *junk = "[Framerate]\nLimit=6O\nLimitDenominator=99999999999999999999\nLimitTime=-\n";
    rtss_profile_parse(junk, strlen(junk), &o);
    CHECK(!o.has_limit && o.has_den && o.den == 1000000000 && !o.has_time, "no number, past any range");
    rtss_profile_parse(NULL, 5, &o);
    CHECK(!o.has_limit, "no text");
    char text[160];
    s = rtss_settings_of(NULL, NULL);
    s.limit = 59940, s.limit_den = 1000, s.time = 16667, s.scan0 = -40;
    rtss_settings_text(text, sizeof text, &s);
    CHECK(!strcmp(text, "limit 59940/1000, frame time limit 16667, scanline sync -40/0"), "text: %s", text);
    CHECK(rtss_two(RTSS_OUT_AUTO, 0, 1, 1) && rtss_two(60000, RTSS_FAIL_READONLY, 1, 1) && !rtss_two(60000, 0, 1, 1) &&
              !rtss_two(60000, RTSS_FAIL_NOT_LIMITING, 1, 1) && !rtss_two(RTSS_UNSET, 0, 1, 1),
          "two limiters only where RTSS is out of the game's limit and a setting of its own holds the game");
}

/* Where the game's frame starts: a hold only with a present within 200 ms and since the frame start two frames back;
   the frames' own times */
static void frame_holds(void) {
    const uint64_t now = 50000000000ull;
    CHECK(rtss_frame_holds(1, now, now - 10000000, 0), "a present 10 ms ago");
    CHECK(rtss_frame_holds(1, now, now - RTSS_HOLD_IDLE_NS + 1, 0), "just under 200 ms");
    CHECK(!rtss_frame_holds(1, now, now - RTSS_HOLD_IDLE_NS, 0), "200 ms with no present");
    CHECK(!rtss_frame_holds(1, now, 0, 0), "no present yet");
    CHECK(!rtss_frame_holds(0, now, now - 1000, 0), "the worker says no");
    CHECK(rtss_frame_holds(1, now, now + 1000, 0), "a present stored after now was read");
    CHECK(rtss_frame_holds(1, now, now - 10000000, now - 33000000), "a present since the frame two back");
    CHECK(rtss_frame_holds(1, now, now - 33000000, now - 33000000), "at its start");
    CHECK(!rtss_frame_holds(1, now, now - 40000000, now - 33000000), "none since the frame two back: a window that stopped showing");
    /* RTSS holds the frames to the period: never too soon */
    RtssWatch w;
    memset(&w, 0, sizeof w);
    const uint64_t p = 1000000000ull / 60;
    uint64_t t = now;
    int bad = 0;
    for (int i = 0; i < 600; i++) bad |= rtss_watch_step(&w, t += p, 1, p);
    CHECK(!bad, "frames a period apart: held");
    /* nothing holds them: 2 ms apart, found within the window */
    int at = -1;
    for (int i = 0; i < 200 && at < 0; i++)
        if (rtss_watch_step(&w, t += 2000000, 1, p)) at = i;
    CHECK(at >= 0 && at < 2 * RTSS_WATCH_WINDOW, "frames that run free: found at frame %d", at);
    /* half too soon, half late (a jitter round the period): not over half */
    memset(&w, 0, sizeof w);
    bad = 0;
    for (int i = 0; i < 600; i++) bad |= rtss_watch_step(&w, t += i & 1 ? p / 2 + p / 4 + 1000 : p + p / 4, 1, p);
    CHECK(!bad, "jitter: held");
    /* the game's own wait's frames between break the window */
    memset(&w, 0, sizeof w);
    bad = 0;
    for (int i = 0; i < 600; i++) bad |= rtss_watch_step(&w, t += 2000000, i % 10 != 0, p);
    CHECK(!bad, "a frame of the game's own wait every ten: the window starts again (%d)", bad);
    for (int i = 0; i < 600; i++) bad |= rtss_watch_step(&w, t += 2000000, 1, i % 10 ? p : p / 2);
    CHECK(!bad, "a period that changes starts it again");
}

/* (Triforce) What RivaTuner is asked to hold for each cap and frame generation mode, and who holds a frame */
static void rule(void) {
    /* the caps the port has (host/nn_vi.c fps_cap_of), Monitor's rates 30 to 240 among them: the host's period is
       1e9 / cap, rounded down to whole nanoseconds, and the cap comes back whole */
    for (int cap = 20; cap <= 240; cap++) {
        const uint64_t period = 1000000000ull / (uint64_t)cap;
        CHECK(rtss_cap_mhz(period) == cap * 1000, "cap %d: %lld", cap, (long long)rtss_cap_mhz(period));
        CHECK(pace_rtss_want(period, PACE_FG_OFF, 1, 0) == cap * 1000, "cap %d without frame generation", cap);
        CHECK(pace_rtss_want(period, PACE_FG_OFF, 4, 0) == cap * 1000, "cap %d: a multiplier means nothing without it", cap);
        for (int m = 2; m <= 8; m++)
            CHECK(pace_rtss_want(period, PACE_FG_FIXED, m, 0) == (int64_t)cap * 1000 * m, "cap %d at %dx", cap, m);
        CHECK(pace_rtss_want(period, PACE_FG_AUTO, 3, 0) == RTSS_OUT_AUTO && pace_rtss_want(period, PACE_FG_AUTO, 1, 1) == RTSS_OUT_AUTO,
              "Auto keeps RTSS out");
        CHECK(pace_rtss_want(period, PACE_FG_DLSSG, 2, 0) == RTSS_OUT_DLSSG, "DLSS frame generation keeps RTSS out");
        for (int m = 1; m <= 4; m++)
            CHECK(pace_rtss_want(period, PACE_FG_DLSSG, m, 1) == (int64_t)cap * 1000 * m, "HOS_RTSS_DLSSG=1: %dx", m);
    }
    CHECK(rtss_cap_mhz(2 * (1000000000ull / 60)) == 30000, "the game's two vsyncs at 60: 30");
    CHECK(pace_rtss_want(0, PACE_FG_OFF, 1, 0) == RTSS_UNSET, "no period yet");
    CHECK(pace_owner(1, 1) == PACE_RTSS && pace_owner(1, 0) == PACE_RTSS && pace_owner(0, 1) == PACE_REFLEX && pace_owner(0, 0) == PACE_GAME,
          "RTSS before Reflex before the game's wait");
    for (int o = 0; o < 3; o++)
        for (int f = PACE_FG_OFF; f <= PACE_FG_DLSSG; f++)
            CHECK(pace_display_yields(o, f) == (o == PACE_RTSS && f == PACE_FG_FIXED), "the display thread yields only to RTSS, owner %d mode %d", o, f);
    CHECK(rtss_out_of_limit(RTSS_OUT_DLSSG, 0) && !rtss_out_of_limit(RTSS_UNSET, 0) && !rtss_out_of_limit(60000, 0), "out of the limit");
}

int main(void) {
    parse();
    injection_delay();
    first_period();
    quit();
    readonly();
    refused();
    frame_count();
    unproven();
    idle();
    kept_out();
    churn();
    hysteresis();
    movie();
    unset_and_library();
    retry();
    own_limit();
    limit_value();
    saved();
    profiles();
    frame_holds();
    rule();
    if (fails) printf("%d checks failed\n", fails);
    else printf("rtss_decide.h and pace_rule.h: every check held\n");
    return fails != 0;
}
