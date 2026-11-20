// (Triforce) gpu/vk/dlssg_pace.h, who holds the frame limit with DLSS frame generation (vk_dlssg.cpp pace), driven with
// made-up frame times: when Reflex holds it and the game's own wait steps aside, the cases the game's own wait keeps it
// (no present yet or for 200 ms, no frame generated, the row off, a limit Streamline has yet to take), and the checks
// over windows of frames: frames that come too soon give the limit to the game's own wait for the run, frames the
// multiplier periods apart switch Reflex to the frames on screen, and the same again gives it back. After NX2navi's
// tests/dlssg_pace_check.c. No GPU, no game files. Built and run by tests/test_dlssg_pace.py, as C and as C++.
#include <stdio.h>
#include <string.h>

#include "dlssg_pace.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MS 1000000ull
static const uint64_t P60 = 16666667ull;   // a cap of 60

// A run of n frames d ns apart starting after *t, each followed by a present 1 ms later; the presents' thread takes
// every limit asked for at once (applied follows want). Returns how many were held by Reflex; *last_found the last
// non-SAME window result.
static int run(DlssgPace *p, uint64_t *t, int n, uint64_t d, uint64_t period, int mult, int allowed, uint32_t *applied,
               uint32_t *want, int *last_found) {
    int held = 0;
    for (int i = 0; i < n; i++) {
        *t += d;
        int found;
        held += dlssg_pace_step(p, *t, period, mult, *t - d + 1 * MS, allowed, *applied, want, &found);
        if (found != DLSSG_PACE_SAME && last_found) *last_found = found;
        *applied = *want;   // the presents' thread applies it before the game's next frame
    }
    return held;
}

int main(void) {
    uint32_t want = 0, applied = 0;
    int found = 0;
    uint64_t t = 1000 * MS;

    // before the first present: the game's own wait
    DlssgPace p;
    memset(&p, 0, sizeof p);
    CHECK(!dlssg_pace_step(&p, t, P60, 2, 0, 1, 0, &want, &found) && want == 0, "no present yet: own wait, no limit");

    // a present 5 ms ago: Reflex is asked for the drawn period, the game's own wait holds until Streamline took it
    CHECK(!dlssg_pace_step(&p, t, P60, 2, t - 5 * MS, 1, 0, &want, &found) && want == 16667,
          "first frame: limit asked (%u), not yet applied", want);
    CHECK(dlssg_pace_step(&p, t + P60, P60, 2, t, 1, 16667, &want, &found) && want == 16667, "applied: Reflex holds");
    CHECK(!dlssg_pace_step(&p, t + 2 * P60, P60, 2, t, 1, 20000, &want, &found), "another limit applied: own wait");

    // no frame is generated (period 0: a menu, a load, a movie): no limit for Reflex, the game's own wait holds
    CHECK(!dlssg_pace_step(&p, t, 0, 2, t - MS, 1, 16667, &want, &found) && want == 0, "nothing generated: own wait");
    // the row off, or a Streamline without the sleep
    CHECK(!dlssg_pace_step(&p, t, P60, 2, t - MS, 0, 16667, &want, &found) && want == 0, "not allowed: own wait");
    // no present for 200 ms (minimised): the game's own wait; 199 ms still Reflex
    CHECK(!dlssg_pace_step(&p, t, P60, 2, t - 200 * MS, 1, 16667, &want, &found) && want == 0, "idle 200 ms: own wait");
    CHECK(dlssg_pace_step(&p, t, P60, 2, t - 199 * MS, 1, 16667, &want, &found), "idle 199 ms: Reflex");

    // Reflex holds the limit: frames a period apart, no verdict over many windows
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    int held = run(&p, &t, 600, P60, P60, 2, 1, &applied, &want, &found);
    CHECK(held == 599 && !p.bad && !p.shown && found == 0 && want == 16667, "held: %d of 600, bad %d shown %d", held, p.bad, p.shown);

    // a GPU slower than the limit at 1x: frames 2 periods apart, never taken for the shown count
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 300, 2 * P60, P60, 1, 1, &applied, &want, &found);
    CHECK(!p.bad && !p.shown && found == 0, "1x, slow GPU: no verdict");

    // nothing holds the game: 31 of 60 too soon -> the game's own wait for the run; 30 of 60 is not enough
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);   // asked, then held: the next frame counts
    run(&p, &t, 30, 5 * MS, P60, 2, 1, &applied, &want, &found);
    run(&p, &t, 30, P60, P60, 2, 1, &applied, &want, &found);
    CHECK(!p.bad && found == 0, "30 of 60 too soon: Reflex keeps it");
    run(&p, &t, 31, 5 * MS, P60, 2, 1, &applied, &want, &found);
    run(&p, &t, 29, P60, P60, 2, 1, &applied, &want, &found);
    CHECK(p.bad && found == DLSSG_PACE_FAST, "31 of 60 too soon: own wait for the run");
    held = run(&p, &t, 120, P60, P60, 2, 1, &applied, &want, NULL);
    CHECK(held == 0 && want == 0, "after that, the game's own wait, whatever the frames do (%d held)", held);

    // the driver counts the frames on screen: at 2x the drawn frames come two periods apart -> shown units
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);
    run(&p, &t, 60, 2 * P60, P60, 2, 1, &applied, &want, &found);
    CHECK(p.shown && !p.bad && found == DLSSG_PACE_SHOWN && want == 8333, "2x, two periods apart: shown units (%u)", want);
    // it does now: a period apart, Reflex keeps it
    found = 0;
    held = run(&p, &t, 300, P60, P60, 2, 1, &applied, &want, &found);
    CHECK(!p.bad && found == 0 && held == 300, "shown units hold the drawn period");
    // 4x: the period over 4
    run(&p, &t, 2, P60, P60, 4, 1, &applied, &want, NULL);
    CHECK(want == 4167, "4x shown units: %u", want);
    // the generation paused (1x): the period itself
    run(&p, &t, 2, P60, P60, 1, 1, &applied, &want, NULL);
    CHECK(want == 16667, "1x shown units: %u", want);

    // shown units guessed wrong (the driver counts drawn frames): the frames come too soon -> the game's own wait
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    p.shown = 1;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);
    run(&p, &t, 60, P60 / 2, P60, 2, 1, &applied, &want, &found);
    CHECK(p.bad && found == DLSSG_PACE_FAST, "shown units, frames at twice the limit: own wait");

    // the same slow band twice (a GPU that only manages the limit over the multiplier) -> the game's own wait
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 3, 1, &applied, &want, NULL);
    run(&p, &t, 60, 3 * P60, P60, 3, 1, &applied, &want, &found);
    CHECK(p.shown && found == DLSSG_PACE_SHOWN, "3x, three periods apart: shown units");
    run(&p, &t, 61, 3 * P60, P60, 3, 1, &applied, &want, &found);
    CHECK(p.bad && found == DLSSG_PACE_SLOW, "again: own wait for the run");

    // hitches (a load: 10 periods) are neither
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);
    run(&p, &t, 120, 10 * P60, P60, 2, 1, &applied, &want, &found);
    CHECK(!p.bad && !p.shown && found == 0, "hitches: no verdict");

    // a change of the period or the multiplier starts the window again: 40 too soon at one limit, 40 at the next
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);
    run(&p, &t, 40, 5 * MS, P60, 2, 1, &applied, &want, &found);
    run(&p, &t, 40, 5 * MS, P60 / 2, 2, 1, &applied, &want, &found);
    CHECK(!p.bad, "window restarted at the period's change");
    run(&p, &t, 40, 2 * MS, P60 / 2, 3, 1, &applied, &want, &found);
    CHECK(!p.bad, "window restarted at the multiplier's change");

    // frames the game's own wait held are not counted: the limit never applied
    memset(&p, 0, sizeof p), found = 0;
    for (int i = 0; i < 200; i++) {
        t += 5 * MS;
        dlssg_pace_step(&p, t, P60, 2, t - MS, 1, 0, &want, &found);
    }
    CHECK(!p.bad && found == 0, "unapplied: no window");

    // the caps this port has, as the host gives their periods (1000000000 / cap, whole nanoseconds): under 60, the
    // console's, past 60 and Monitor's rate's ends
    static const int caps[] = {30, 40, 45, 60, 75, 90, 120, 144, 165, 240};
    for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; i++) {
        const uint64_t period = 1000000000ull / (uint64_t)caps[i];
        memset(&p, 0, sizeof p);
        dlssg_pace_step(&p, t, period, 2, t - MS, 1, 0, &want, &found);
        const uint32_t us = (uint32_t)((period + 500) / 1000);
        CHECK(want == us && us > 0, "cap %d: Reflex is asked for %u us (%u)", caps[i], us, want);
        CHECK(dlssg_pace_step(&p, t + period, period, 2, t, 1, us, &want, &found), "cap %d: held once taken", caps[i]);
    }
    // the game's own interval of 2 at a cap of 60 (30 fps): twice the period
    memset(&p, 0, sizeof p);
    dlssg_pace_step(&p, t, 1000000000ull / 60 * 2, 2, t - MS, 1, 0, &want, &found);
    CHECK(want == 33333, "interval 2 at 60: %u", want);
    // a period too long for any limit is the game's own wait's
    dlssg_pace_step(&p, t, 3000000000000000ull, 1, t - MS, 1, 0, &want, &found);
    CHECK(want == 0, "a period no limit can hold: %u", want);
    // the clock going back (a present stamped after the frame's start read the clock) holds nothing up
    CHECK(dlssg_pace_step(&p, t, P60, 2, t + MS, 1, 16667, &want, &found) && want == 16667, "a present stamped after the frame start read the clock: Reflex");

    // one or the other, over every input: held exactly when a limit is wanted and it is the one applied, and never
    // while not allowed, without a period, without a recent present, or once the frames showed Reflex does not hold
    for (int allowed = 0; allowed <= 1; allowed++)
        for (int has_period = 0; has_period <= 1; has_period++)
            for (int hosted = 0; hosted <= 2; hosted++)   // none yet, recent, 300 ms ago
                for (int bad = 0; bad <= 1; bad++)
                    for (int app = 0; app <= 2; app++) {   // nothing applied, the wanted limit, another
                        memset(&p, 0, sizeof p);
                        p.bad = bad;
                        const uint64_t h = hosted == 0 ? 0 : hosted == 1 ? t - MS : t - 300 * MS;
                        const uint32_t a = app == 0 ? 0 : app == 1 ? 16667 : 9999;
                        const int r = dlssg_pace_step(&p, t, has_period ? P60 : 0, 2, h, allowed, a, &want, &found);
                        const int may = allowed && has_period && hosted == 1 && !bad;
                        CHECK(r == (may && app == 1), "allowed %d period %d hosted %d bad %d applied %d: held %d", allowed, has_period,
                              hosted, bad, app, r);
                        CHECK((want != 0) == may, "the limit is wanted exactly when Reflex may hold it (%u)", want);
                        CHECK(!r || p.last_ns == t, "a held frame is the window's next start");
                    }

    printf(fails ? "%d checks failed\n" : "all checks passed\n", fails);
    return fails != 0;
}
