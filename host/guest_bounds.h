/* (Triforce) Bounds checks on what the game passes in that cannot overflow. A check written as `first + n > cap` wraps
   around when the game's offset or count is huge (in 32 bits for ints, in 64 for addresses) and lets the range through;
   these compare by subtraction, in 64 bits, so a range that does not fit is refused whatever its size.
   Plain functions: tests/test_guest_bounds.py compiles and runs them against the old expressions. */
#pragma once
#include <stdint.h>

/* [first, first + n) lies in [0, cap): a start and a count as the game gave them (ints widened, a negative one refused) */
static inline int hos_span_ok(int64_t first, int64_t n, int64_t cap) {
    return first >= 0 && n >= 0 && first <= cap && n <= cap - first;
}

/* [at, at + n) lies in [0, end): addresses and sizes, unsigned */
static inline int hos_range_ok(uint64_t at, uint64_t n, uint64_t end) {
    return at <= end && n <= end - at;
}

/* how many of the n units from x lie in a cap-wide row: n, or what is left after x (0 from cap on) */
static inline uint32_t hos_span_clip(uint32_t x, uint32_t n, uint32_t cap) {
    return x >= cap ? 0 : (uint64_t)x + n <= cap ? n : cap - x;
}
