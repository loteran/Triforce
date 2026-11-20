// (Triforce) Who holds a frame of the game to the Game frame rate cap: RivaTuner Statistics Server, NVIDIA Reflex or
// the game's own wait, and what RivaTuner is asked to hold (gpu/vk/vk_pace.cpp, where the game's frame starts). Plain
// C with no system call, so that tests/test_rtss_limit.py runs every combination of rows through it.
//
// The owner's rule: RivaTuner is the game's limiter whenever it can hold the game. It holds presents, and it counts
// every one, the frames frame generation makes too. So what it can hold depends on where the presents come from:
//
//   No frame generation: one present for each of the game's frames, on the replay's thread. RivaTuner's limit is the
//   cap, and holding the presents holds the game through the present queue.
//
//   The Frame generation row's fixed multipliers (2x to 8x, AMD FSR 3.1's 2x and AMD FSR 4's frame): the display thread
//   (vk_framegen.cpp display_main) presents every frame of every set, the multiplier's number for each of the game's.
//   RivaTuner's limit is the cap times the multiplier, and holding those presents holds the game: the display thread
//   then takes the game's frames at the cap, and the replay's thread waits for it (framegen_record) as the game waits
//   for the replay. The display thread's own pacing of the made frames has to yield meanwhile (pace_display_yields):
//   it spaces the presents by the game's measured frame time, which under RivaTuner's hold is RivaTuner's own spacing
//   read back; it can only ever fall behind that, never catch up, and each hitch would leave the game slower for good.
//
//   The Frame generation row's Auto: a frame for each refresh of the screen, at that refresh's time, however many the
//   game makes (a game frame is given none, one or several). The presents follow the screen, not the game, so no limit
//   on them holds the game to its cap: RivaTuner is kept out (RTSS_OUT_AUTO) and the game's own wait holds the frames.
//
//   NVIDIA DLSS frame generation: the game's one present goes into NVIDIA's Streamline, which shows the made frames
//   and the game's own from inside its closed library, on a pace of its own that cannot be told to yield, and shows
//   one frame for each of the game's while it holds generation back (a menu, a load, a window not in front). What a
//   hold on each of its presents does to that pacing cannot be worked out from this code, so RivaTuner is kept out
//   (RTSS_OUT_DLSSG) and the game's own wait, or Reflex with its row, holds the frames. HOS_RTSS_DLSSG=1 lets RivaTuner
//   try there all the same, with the cap times the multiplier in effect and every safeguard of rtss_decide.h.
#pragma once
#include <stdint.h>

#include "rtss_decide.h"

enum { PACE_GAME = 0, PACE_REFLEX = 1, PACE_RTSS = 2 };
enum {
    PACE_FG_OFF,     // no frame generation: a present for each of the game's frames
    PACE_FG_FIXED,   // the display thread shows mult frames for each of the game's
    PACE_FG_AUTO,    // the display thread shows a frame for each refresh of the screen
    PACE_FG_DLSSG,   // NVIDIA's Streamline shows mult frames for each of the game's (1 while it holds generation back)
};

// What RivaTuner is asked to hold where the game's frame starts: the limit on the frames it counts, every frame
// presented, in millihertz; or why it is kept out (RTSS_OUT_*); RTSS_UNSET while the host has told no period yet.
// period_ns: the period the host paces the game's frames by (the cap in effect). fg, mult: where the presents come
// from and how many for each of the game's frames now. dlssg_try: HOS_RTSS_DLSSG=1.
static inline int64_t pace_rtss_want(uint64_t period_ns, int fg, int mult, int dlssg_try) {
    if (!period_ns) return RTSS_UNSET;
    if (fg == PACE_FG_AUTO) return RTSS_OUT_AUTO;
    if (fg == PACE_FG_DLSSG && !dlssg_try) return RTSS_OUT_DLSSG;
    return rtss_cap_mhz(period_ns) * (fg == PACE_FG_OFF || mult < 1 ? 1 : mult);
}

// Who holds this frame. rtss: RivaTuner holds it (rtss_frame_holds of the worker's word, with no movie playing and the
// frames' own check passed). reflex: Reflex would hold it (vk_dlssg.cpp pace), which is asked only when RivaTuner does
// not: with RivaTuner as the limiter Reflex is given no frame limit, whatever the Reflex frame limit row says. Exactly
// one of the three for every frame: the host waits when, and only when, the answer is PACE_GAME.
static inline int pace_owner(int rtss, int reflex) { return rtss ? PACE_RTSS : reflex ? PACE_REFLEX : PACE_GAME; }

// The display thread's own pacing of made frames stands aside: RivaTuner holds the frames it presents.
static inline int pace_display_yields(int owner, int fg) { return owner == PACE_RTSS && fg == PACE_FG_FIXED; }
