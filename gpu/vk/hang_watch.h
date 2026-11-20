// The hang watch's judgement, apart from its thread, the clock and the stacks (vk_hangdump.cpp). After NX2navi's hang
// watch (its runtime/hang_watch.c hw_step): a report when no frame is shown for some seconds, once a stall, a few a run.
// tests/test_hang_watch.py
#pragma once

#include <cstdint>

namespace mkwvk::hangwatch {

constexpr int kReports = 3;   // (at most this many a run: a game that stalls again and again says so three times)

struct Watch {
    int64_t presents = -1;   // the presents counted at the last look
    int still = 0;           // judged seconds running without one
    bool told = false;       // this stall has its report
    int reports = 0;
};

/// One second's look. presents: the frames shown so far. judged: whether a second without one counts (the game's window
/// in front and not minimized: the game may stop showing frames behind another window, and a watch asked for by name
/// judges always). True when a report is due now: `limit` judged seconds running without a present, once per stall (a
/// new one needs a present first), at most kReports a run.
inline bool step(Watch& w, int64_t presents, bool judged, int limit) {
    if (presents != w.presents || !judged) {
        w.presents = presents, w.still = 0, w.told = false;
        return false;
    }
    if (++w.still < limit || w.told || w.reports >= kReports) return false;
    w.told = true;
    w.reports++;
    return true;
}

}  // namespace mkwvk::hangwatch
