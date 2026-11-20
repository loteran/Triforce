// (Triforce) A scissor's rectangle in a target without integer overflow. NVN's scissor is the game's x, y, width and
// height as ints; x + width in an int wrapped for a large width (a game that means "to the edge" may say INT_MAX): a
// clear's rectangle came out empty, and the draw's scissor wrapped again in end minus start, to an extent of two billion
// pixels past the target. Here the end is summed in 64 bits; a float scaled to
// pixels is clamped into an int's range before it is converted (a float past it converted to INT_MIN on x86).
// Plain functions: tests/test_rect_clamp.py compiles and runs them against the old expressions.
#pragma once

#include <algorithm>
#include <cstdint>

namespace rect_clamp {

struct Span {
    int32_t at;   // (the offset)
    uint32_t n;   // (the extent)
};

/// A draw's scissor along one axis (vk_draw.cpp dynamic_state): from max(0, x) to min(x + w, cap), empty when that is
/// nothing; the offset is not moved down to cap (as before: an offset past the target with no extent)
inline Span scissor(int32_t x, int32_t w, uint32_t cap) {
    const int64_t x0 = std::max<int64_t>(0, x);
    const int64_t x1 = std::min<int64_t>((int64_t)x + w, (int64_t)cap);
    return {(int32_t)x0, (uint32_t)std::max<int64_t>(0, x1 - x0)};
}

/// A clear's rectangle along one axis (vk_draw.cpp clear_rect): both ends clamped into [0, cap]
inline Span clear(int32_t x, int32_t w, uint32_t cap) {
    const int64_t x0 = std::clamp<int64_t>(x, 0, (int64_t)cap);
    const int64_t x1 = std::clamp<int64_t>((int64_t)x + w, 0, (int64_t)cap);
    return {(int32_t)x0, (uint32_t)std::max<int64_t>(0, x1 - x0)};
}

/// A float pixel coordinate as an int, truncated as a cast does, but clamped into an int's range first (NaN: 0)
inline int32_t px(float v) {
    if (!(v == v)) return 0;
    if (v >= 2147483648.0f) return INT32_MAX;
    if (v <= -2147483648.0f) return INT32_MIN;
    return (int32_t)v;
}

}  // namespace rect_clamp
