/* (Triforce) When a pool's trial in guest memory ends early (vk_memory.cpp demote_trials). A pool the CPU and the GPU both write
   is tried out of the mirrors: its pieces (or the whole pool) are bound in guest memory, 10 frames to settle, then up to 90
   measured, and the GPU's time a frame is compared with the window before. Only the measured frames were judged, and only after
   ten of them: in the owner's log (RTX 5090, 2026-11-15, test4) a 16 MB pool took the GPU 106 ms a frame in guest memory
   against 7.4 before, and the game's thread waited for that GPU in every capture (memory sync 99 ms a submit, 18 ms a frame
   over 120 presents): some twenty frames of 100 ms, the same again at its second trial, and a 32 MB pool's two trials at 39 ms
   against 7.7. A fresh install tries every such pool (a pool that failed twice is kept in triforce_mirror_keep.txt for later
   runs), which is what a benchmark plays. So a trial far over its baseline ends as soon as a few frames say so, settling or
   measured, the way a slower one ends: mirrored again and measured once more that way, and judged against that (phase 4), so
   an early end that was wrong costs a second measure, not the pool. Of the 84 trials in the owner's five logs the four costly
   ones were 4.7 to 19 times their baseline; every other was under 2.3 times but one, 3.6 times, whose mirrored-again frames were
   as slow (its baseline was off; it stayed in guest memory): under this rule's line too. RX_VK_DEMOTE_QUICK=0: as before. Plain
   functions: tests/test_demote_watch.cpp checks them. */
#pragma once

#include <cstdint>
#include <cstdlib>

namespace mkwvk::demote_watch {

constexpr uint64_t kFrames = 3;      // (the frames since the trial's start, or its measure's, before one is judged: the GPU's time comes
                                     // in as submissions finish, a frame or two late, so the first frames weigh less than they cost)
constexpr double kTimes = 3.0;       // (over kTimes the baseline and kOverMs more: see above. The trial's own rule, twice the
constexpr double kOverMs = 5.0;      //  baseline and 1 ms after 10 measured frames, stays as it was)

/// Whether a trial whose frames so far took ms_a_frame of GPU time each, over frames of them, ends now: far slower than base_ms
inline bool gross(double base_ms, double ms_a_frame, uint64_t frames) {
    return base_ms > 0 && frames >= kFrames && ms_a_frame > base_ms * kTimes + kOverMs;
}

/// RX_VK_DEMOTE_QUICK: on unless "0"
inline bool quick_on(const char* env) {
    return !(env && *env && std::atoi(env) == 0 && env[0] == '0');
}

} // namespace mkwvk::demote_watch
