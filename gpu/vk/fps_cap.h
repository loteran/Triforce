/* The Game frame rate cap row's "Monitor's rate" (vk_menu.cpp), as plain code: the cap a display's refresh rate gives.
   host/nn_vi.c fps_max asks the renderer for it at the start (HOS_OPT_DISPLAY_CAP, vk_replay.cpp's option), and the row
   says the same number beside its name. After NX2navi's fps_limit = display. No Vulkan, no menu:
   tests/test_fps_cap_monitor.py checks it. */
#pragma once

namespace mkwvk::fps_cap {

/// The row's value for Monitor's rate (frame_rate_cap= in the options file): after 60, 90, 120, 144, 165 and 240 (0 to
/// 5); 45, 40 and 30 are -1 to -3
constexpr int kMonitor = 6;
/// The caps the game's pacing and its time step take (host/nn_vi.c): a display under 30 Hz or over 240 gets the nearest
constexpr int kLowest = 30, kHighest = 240;

/// The cap for a display's refresh rate in mHz (59,940 is 60, 143,856 is 144): its rate rounded to whole frames a
/// second, kLowest to kHighest. 0 when the rate is not known (mhz 0 or less): the host then keeps the console's 60.
inline int of_display(int mhz) {
    if (mhz <= 0) return 0;
    const int hz = (int)(((long long)mhz + 500) / 1000);
    return hz < kLowest ? kLowest : hz > kHighest ? kHighest : hz;
}

} // namespace mkwvk::fps_cap
