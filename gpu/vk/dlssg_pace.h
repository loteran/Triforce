// (Triforce) Who holds the frame limit while NVIDIA DLSS frame generation runs: Reflex's sleep, where the game's frame
// starts, or the game's own wait (host/nn_vi.c hos_vi_native_acquire). After NX2navi's runtime/dlssg_pace.h, whose rule
// this is. A function of what the game's presenting thread sees and nothing else, so tests/test_dlssg_pace.py drives
// it without a GPU (plain C: the test compiles it as C and as C++).
//
// The period counts the frames the game draws (the Game frame rate cap's, the host's own). Reflex holds it only while
// every one of these is so, and the game's own wait holds the whole period otherwise: the caller allows it (the
// Reflex frame limit row on, frames being generated now, a Streamline whose Reflex sleeps here and refused no limit);
// there is a period (the caller passes 0 while no frame is generated: a menu, a load, a movie); a present went out,
// and within DLSSG_HOLD_IDLE_NS (a minimized window gives the driver no present to time a frame by: Reflex's sleep is
// left out then, vk_dlssg.cpp frame_start); the limit Reflex should hold is the one Streamline last took; and the
// frames have not shown that Reflex does not hold it (for the rest of the run then).
//
// The frames are checked over windows of DLSSG_PACE_WINDOW presents Reflex held to one period and one multiplier.
// Over half of them sooner than three quarters of the period: nothing holds the game, and the game's own wait takes
// the limit for the run. Over half of them about the multiplier times the period apart (within half a period): the
// driver counted the frames on screen, the generated ones too, so Reflex is given the period over the multiplier from
// then on; the same again (a GPU that only managed that rate) gives the limit to the game's own wait for the run. A
// frame a load or a hitch held counts as neither.
#pragma once
#include <stdint.h>

#define DLSSG_HOLD_IDLE_NS 200000000ull   // no present for this long: Reflex's sleep is left out
#define DLSSG_PACE_WINDOW 60              // presents over which Reflex's hold of the limit is checked

enum { DLSSG_PACE_SAME, DLSSG_PACE_FAST, DLSSG_PACE_SHOWN, DLSSG_PACE_SLOW };   // what the last window found

typedef struct {
    uint64_t last_ns;       // the newest frame start Reflex held, 0 when the game's own wait held it
    uint64_t last_period;   // its period
    int last_mult;          // and its multiplier
    int window, fast, slow; // frames counted in the window, those too soon, those about the multiplier periods apart
    int shown;              // Reflex counts the frames on screen: it is given the period over the multiplier
    int bad;                // Reflex did not hold the limit: the game's own wait holds it for the rest of the run
} DlssgPace;

// now: the clock where the game's frame starts. period_ns: the period of the cap in effect (0: no frame is generated
// now, or no limit). mult: frames on screen per drawn frame now. hosted_ns: the clock after the newest present, 0
// before the first. allowed: Reflex may hold the limit. applied_us: the limit Streamline last took. *want_us: the
// limit Reflex should hold, 0 none. *found: what a window that ended here found (DLSSG_PACE_*). Returns 1 when Reflex
// holds this frame's limit: the game's own wait steps aside, and only then.
static inline int dlssg_pace_step(DlssgPace *p, uint64_t now, uint64_t period_ns, int mult, uint64_t hosted_ns,
                                  int allowed, uint32_t applied_us, uint32_t *want_us, int *found) {
    *found = DLSSG_PACE_SAME;
    if (mult < 1) mult = 1;
    if (!p->bad && p->last_ns && now >= p->last_ns && p->last_period == period_ns && p->last_mult == mult) {
        uint64_t d = now - p->last_ns, m = (uint64_t)mult;
        if (d < period_ns / 4 * 3) p->fast++;
        else if (m > 1 && d * 2 > period_ns * (2 * m - 1) && d * 2 < period_ns * (2 * m + 1)) p->slow++;
        if (++p->window >= DLSSG_PACE_WINDOW) {
            if (p->fast > DLSSG_PACE_WINDOW / 2) p->bad = 1, *found = DLSSG_PACE_FAST;
            else if (p->slow > DLSSG_PACE_WINDOW / 2 && p->shown) p->bad = 1, *found = DLSSG_PACE_SLOW;
            else if (p->slow > DLSSG_PACE_WINDOW / 2) p->shown = 1, *found = DLSSG_PACE_SHOWN;
            p->window = p->fast = p->slow = 0;
        }
    } else {
        p->window = p->fast = p->slow = 0;   // a window counts only frames Reflex held to one period and multiplier
    }
    uint64_t us = 0;
    // a present stamped after now went out while this thread read the clock: as recent as one can be
    int live = hosted_ns && (hosted_ns >= now || now - hosted_ns < DLSSG_HOLD_IDLE_NS);
    if (allowed && period_ns && !p->bad && live) {
        uint64_t per = p->shown ? period_ns / (uint64_t)mult : period_ns;
        us = (per + 500) / 1000;
        if (!us) us = 1;
        if (us > 0x7fffffffu) us = 0;   // no limit Reflex could be given: the game's own wait keeps it
    }
    *want_us = (uint32_t)us;
    int held = us && applied_us == (uint32_t)us;
    p->last_ns = held ? now : 0;
    p->last_period = held ? period_ns : 0;
    p->last_mult = mult;
    return held;
}
