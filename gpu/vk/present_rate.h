// (Triforce) The presents a second the textures' waits go by (vk_texture.cpp texture_frame, texture_sweep), apart from
// Vulkan so that tests/test_present_rate.py can run it. After NX2navi's runtime/vk_tex_release.h (trl_rate_sample,
// trl_rate, trl_presents) and its vk_texture.c rate_tick.
//
// The eviction's waits count presents: how long a texture has gone unused before it may go (60, RX_VK_EVICT_MIN_UNUSED;
// 120 in a pass under the budget), how long ago the game let go of one (30, RX_VK_LET_GO_AFTER), how long one the GPU
// drew stays (600 near what the driver allows, else 3600: keep_rendered.h), the compaction's hold and its reserve (600
// and 1800: vram_compact.h). They were set at 60 presents a second: a second, half a second, ten seconds and a minute,
// ten and thirty seconds. Below 60 they last longer, at 30 twice as long, and the card short of video memory is the one
// that runs slow (past what the driver allows, the driver pages: 14 fps on an RTX 3060 Ti, 5 on an RTX 4050). There a
// texture stays out of the eviction's reach four times as long, and what the waits hold grows with their length: the
// game lets go and loads by the second, as the player moves. NX2navi measured it (an RTX 4070 SUPER at 2560x1440,
// 2026-11-14): its 120-present wait held 442 MiB at 60 presents a second and 2,105 MiB at 21, on a card then 600 MiB over
// its budget.
//
// So below 60 an age in presents counts for what it is at 60 a second (at60): the waits are the same seconds at 30 as at
// 60. The rate is that of the last 8 seconds, the highest of them, so that a slow second or a few (an area's load, a
// movie's start, a window not in front) shorten nothing and only 8 slow seconds in a row do; fewer than 15 a second count
// as 15. The rate is rounded up, so no wait is shorter than its seconds at 60. RX_VK_TEX_RATE_LOW=0: presents below 60,
// as before.
//
// (Triforce, after NX2navi's M19: vk_texture.c 177-181, vk_tex_release.h TRL_RATE_MAX, chrg6 69945636) Above 60 too: the
// waits were the presents they were there, so at 94 a second (the owner's Market) "60 presents unused" was 0.64 s and
// "30 presents after the game let it go" 0.32 s. NX2navi found its textures going three times as readily as meant at 85 to
// 240 a second, and the ones still on screen showing their stand-ins until they were made again. Now an age above 60 counts
// for what it is at 60 too, up to 480 a second (more count as 480): the waits are their seconds at any rate, and longer in
// presents above 60. RX_VK_TEX_RATE_HIGH=0: presents above 60, as before.
#pragma once

#include <algorithm>
#include <cstdint>

namespace present_rate {

/// A rate is a multiple of 60 presents a second in 1/256: 256 is 60 a second, 64 is 15, 2048 is 480
inline constexpr uint32_t k60 = 256, kLeast = 64, kMost = 2048;
/// The seconds whose highest rate counts
inline constexpr int kSeconds = 8;

/// The rate of `presents` in `ns`, rounded up: an age is then taken for no more than its seconds at 60, and a wait is never
/// shorter than at 60. low: 15 to 60 a second below 60 (false: 60 there, the rule before); high: up to 480 above 60 (false:
/// 60 there, the rule before)
inline uint32_t sample(uint64_t presents, uint64_t ns, bool low, bool high) {
    if (!ns || (!low && !high)) return k60;
    presents = std::min<uint64_t>(presents, 1ull << 24), ns = std::min<uint64_t>(ns, 1ull << 56);   // (no overflow below)
    const uint64_t r = (presents * 256ull * 1000000000ull + ns * 60 - 1) / (ns * 60);
    const uint32_t least = low ? kLeast : k60, most = high ? kMost : k60;
    return r < least ? least : r > most ? most : (uint32_t)r;
}

/// The rate of the last n samples (one a second): the highest of them. A sample not taken yet (0) counts as 60 a second:
/// the first n seconds of a run are never below it
inline uint32_t of(const uint32_t* ring, int n) {
    uint32_t m = kLeast;
    for (int i = 0; i < n; i++) m = std::max(m, ring[i] ? ring[i] : k60);
    return m;
}

/// An age of n presents at the rate, in presents at 60 a second: the same wall time. Below 60 never less than n, above 60
/// never more
inline uint64_t at60(uint64_t n, uint32_t rate) {
    return rate && rate != k60 ? n * k60 / rate : n;
}

/// The rate as the presents come: tick at each present with the count of presents and a clock in ns
struct Clock {
    uint32_t ring[kSeconds] = {};
    uint32_t next = 0, rate = k60;
    uint64_t t0 = 0, f0 = 0;
    bool begun = false;
    /// True when a second's sample was taken (the rate may have changed)
    bool tick(uint64_t presents, uint64_t now_ns, bool low, bool high) {
        if (!begun) return begun = true, t0 = now_ns, f0 = presents, false;
        if (now_ns - t0 < 1000000000ull) return false;
        ring[next++ % kSeconds] = sample(presents - f0, now_ns - t0, low, high);
        t0 = now_ns, f0 = presents;
        rate = of(ring, kSeconds);
        return true;
    }
};

}   // namespace present_rate
