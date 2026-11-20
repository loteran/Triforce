// (Triforce) Who holds a frame of the game to the Game frame rate cap (vk_pace.cpp): RivaTuner Statistics Server,
// NVIDIA Reflex or the game's own wait. The host asks where the frame starts (host/nn_vi.c hos_vi_native_acquire:
// HOS_OPT_PACE_PERIOD, HOS_OPT_PACE_MOVIE, then HOS_OPT_PACE_START); its own wait is the answer unless another limiter
// is seen to hold the frame.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mkwvk::pace {

enum Owner { GAME = 0, REFLEX = 1, RTSS = 2 };   // (pace_rule.h's PACE_*)

/// HOS_OPT_PACE_PERIOD: the period the host paces the game's frames by, in nanoseconds (the cap in effect: Monitor's
/// rate, a cap under 60 and the game's own interval counted in), told before each frame_start.
void set_period_ns(int ns);
/// HOS_OPT_PACE_MOVIE: 1 while a movie plays (the game's own wait holds a movie), told before each frame_start.
void set_movie(int playing);
/// (Triforce) HOS_OPT_PACE_STEADY, option_set: 1 while the period told is the Steady frame rate row's (host/pace_steady.h).
void set_steady(int on);
/// HOS_OPT_PACE_STEADY, option (on the game's presenting thread, once a frame): the microseconds steady_held counted since
/// the last ask.
int steady_held_us();
/// The period the present holds the game's pictures to (present_smooth.h present_smooth_paced_at): the steady frame rate's
/// while the game's own wait paces the frames, else 0.
int64_t steady_floor_ns();
/// A picture waited ns for steady_floor_ns (vk_replay.cpp smooth_present): counted where that held the game's presenting
/// thread, not on the present thread.
void steady_held(int64_t ns);
/// HOS_OPT_PACE_START, on the game's presenting thread where its frame starts: 1 when another limiter holds this
/// frame and the host's own wait steps aside (RivaTuner Statistics Server, which holds the frame's presents:
/// rtss_limit.cpp; or NVIDIA Reflex with DLSS frame generation, whose sleep is taken inside this call: vk_dlssg.cpp
/// pace); 0, the host's own wait, in every other case.
int frame_start();
/// After a present of the window went out (vk_replay.cpp present_image): RivaTuner's limiter holds nothing while none
/// does.
void presented();
/// The host's clock after the newest present, 0 before the first.
uint64_t presented_ns();
/// The process is about to exit in order (HOS_OPT_EXITING): RivaTuner's profile gets back what it held.
void exiting();
/// Who held the newest frame: an Owner.
int owner();
/// The frame generation's display thread leaves its own pacing of made frames out: RivaTuner holds the frames it
/// presents (pace_rule.h pace_display_yields; vk_framegen.cpp).
bool display_yields();
/// Who holds the frames, and with the game's own wait why, in a line for the performance overlay.
void limiter_text(char* out, size_t n);

} // namespace mkwvk::pace
