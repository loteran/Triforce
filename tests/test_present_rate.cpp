// gpu/vk/present_rate.h: the presents a second the textures' waits go by. tests/test_present_rate.py
// (after NX2navi's tests/tex_release_check.c, rate_checks)
#include "present_rate.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    using namespace present_rate;
    const uint64_t S = 1000000000ull;
    // ---- below 60 (high off: above 60 counts as 60, the rule of RX_VK_TEX_RATE_HIGH=0)
    CHECK(sample(60, S, true, false) == 256 && sample(60, S, false, false) == 256, "60 presents a second");
    CHECK(sample(240, S, true, false) == 256 && sample(1000, S, true, false) == 256, "above 60, high off: 60, the waits count presents there");
    CHECK(sample(30, S, true, false) == 128, "30 a second: %u", sample(30, S, true, false));
    CHECK(sample(21, S, true, false) == 90, "21 a second, rounded up: %u", sample(21, S, true, false));
    CHECK(sample(59, S, true, false) == 252, "59 a second, rounded up: %u", sample(59, S, true, false));
    CHECK(sample(5, S, true, false) == kLeast && sample(0, S, true, false) == kLeast, "under 15 a second: 15");
    CHECK(sample(30, S, false, false) == 256 && sample(0, S, false, false) == 256, "the rule before: never under 60");
    CHECK(sample(100, 0, true, false) == 256 && sample(100, 0, true, true) == 256, "no time passed: 60");
    CHECK(sample(63, 3 * S, true, false) == 90, "21 a second over three seconds: %u", sample(63, 3 * S, true, false));
    CHECK(sample(1, 600 * S, true, false) == kLeast, "a present after ten minutes of none: 15");

    // ---- above 60 (high on, NX2navi's M19): up to 480 a second, rounded up
    CHECK(sample(60, S, true, true) == 256, "60 a second, high on: %u", sample(60, S, true, true));
    CHECK(sample(94, S, true, true) == 402, "94 a second (the Market), rounded up: %u", sample(94, S, true, true));
    CHECK(sample(120, S, true, true) == 512 && sample(240, S, true, true) == 1024, "120 and 240 a second");
    CHECK(sample(480, S, true, true) == kMost && sample(1000, S, true, true) == kMost, "480 and more: 480");
    CHECK(sample(30, S, true, true) == 128, "below 60 as before with high on: %u", sample(30, S, true, true));
    CHECK(sample(30, S, false, true) == 256 && sample(144, S, false, true) == 615, "high alone: never under 60, above it as it is (%u)",
          sample(144, S, false, true));

    // (the highest of the samples; one not taken yet counts as 60, so a run's first seconds never go below it)
    enum { N = kSeconds };
    uint32_t ring[N] = {0};
    CHECK(of(ring, N) == 256, "no sample yet: %u", of(ring, N));
    for (int i = 0; i < N - 1; i++) ring[i] = sample(21, S, true, true);
    CHECK(of(ring, N) == 256, "seven slow seconds and one not sampled: %u", of(ring, N));
    ring[N - 1] = sample(21, S, true, true);
    CHECK(of(ring, N) == 90, "eight slow seconds: %u", of(ring, N));
    ring[3] = sample(60, S, true, true);
    CHECK(of(ring, N) == 256, "one second at 60 among them: %u", of(ring, N));
    ring[3] = sample(144, S, true, false);
    CHECK(of(ring, N) == 256, "one second at 144 among them, high off: %u", of(ring, N));
    ring[3] = sample(144, S, true, true);
    CHECK(of(ring, N) == 615, "one second at 144 among them, high on: the highest (%u)", of(ring, N));
    for (int i = 0; i < N; i++) ring[i] = sample(60, S, true, true);
    ring[5] = sample(2, S, true, true);   // (an area's load: one second with two presents)
    CHECK(of(ring, N) == 256, "a slow second among fast ones shortens nothing: %u", of(ring, N));

    // (the waits of vk_texture.cpp's eviction, in presents at 60 a second: what they are in seconds at each rate, before and now)
    static const uint64_t waits[] = {30, 60, 120, 600, 1800, 3600};
    static const uint32_t fps[] = {15, 21, 30, 45, 59, 60, 90, 94, 144, 240, 480};
    std::printf("presents/s  wait at 60  high off: presents, seconds   high on: presents, seconds\n");
    for (uint64_t wait : waits)
        for (uint32_t f : fps) {
            // (the first age, in presents, that has passed the wait: high off, the rule below 60 alone; high on, at any rate)
            uint64_t low_only = 0, now = 0;
            const uint32_t rate_low = sample(f, S, true, false), rate = sample(f, S, true, true);
            while (at60(low_only, rate_low) < wait) low_only++;
            while (at60(now, rate) < wait) now++;
            const double now_s = (double)now / f, at60_s = (double)wait / 60;
            if (wait == 60 || wait == 600)
                std::printf("%10u  %10llu  %18llu  %7.2f   %17llu  %7.2f\n", f, (unsigned long long)wait, (unsigned long long)low_only,
                            (double)low_only / f, (unsigned long long)now, now_s);
            // (high on: at every rate never shorter than at 60, and no longer than that by more than a present and the rounding)
            CHECK(now_s >= at60_s - 1e-9, "%llu presents at %u a second: %.3f s, shorter than the %.3f s at 60",
                  (unsigned long long)wait, f, now_s, at60_s);
            CHECK(now_s <= at60_s * 1.02 + 1.0 / f, "%llu presents at %u a second: %.3f s, %.3f at 60", (unsigned long long)wait, f,
                  now_s, at60_s);
            // (high off, as before: below 60 the seconds, at 60 and above the presents it was)
            if (f >= 60) CHECK(low_only == wait, "%llu presents at %u a second, high off: %llu", (unsigned long long)wait, f,
                               (unsigned long long)low_only);
            if (f <= 60) CHECK(now == low_only, "%llu presents at %u a second: high on changes nothing at or below 60 (%llu, %llu)",
                               (unsigned long long)wait, f, (unsigned long long)now, (unsigned long long)low_only);
            // (below 60 never more presents than before; above 60 more: the waits get longer in presents there, never shorter)
            if (f <= 60) CHECK(now <= wait, "%llu presents at %u a second: %llu presents now, more than before", (unsigned long long)wait,
                               f, (unsigned long long)now);
            if (f > 60) CHECK(now > wait, "%llu presents at %u a second: %llu presents now, not more than before", (unsigned long long)wait,
                              f, (unsigned long long)now);
        }
    // (under 15 a second a wait is 15 a second's: a quarter of its presents, longer in seconds than at 60)
    CHECK(at60(15, sample(10, S, true, true)) == 60 && at60(14, sample(10, S, true, true)) < 60, "10 presents a second: 15 presents for 60");
    // (an age is never less than its presents at or below 60, never more above it, whatever the rate given)
    for (uint32_t rate : {0u, 1u, 64u, 89u, 128u, 255u, 256u, 257u, 300u, 1024u, 2048u})
        for (uint64_t n : {0ull, 1ull, 59ull, 60ull, 3600ull, 1ull << 40}) {
            if (rate <= 256) CHECK(at60(n, rate) >= n, "an age of %llu at rate %u: %llu", (unsigned long long)n, rate, (unsigned long long)at60(n, rate));
            if (rate >= 256) CHECK(at60(n, rate) <= n, "an age of %llu at rate %u: %llu", (unsigned long long)n, rate, (unsigned long long)at60(n, rate));
        }
    CHECK(at60(60, 256) == 60 && at60(60, 128) == 120 && at60(30, 128) == 60 && at60(900, 128) == 1800, "30 a second: twice");
    CHECK(at60(120, 512) == 60 && at60(480, 2048) == 60 && at60(94, 402) == 59 && at60(95, 402) == 60, "120, 480, 94 a second");
    // (the tester's log NX2navi's rule was made from: 21 presents a second. 600 presents, ten seconds at 60, were 28.6 s there)
    CHECK(600.0 / 21 > 28.5 && at60(211, sample(21, S, true, true)) >= 600 && at60(210, sample(21, S, true, true)) < 600,
          "21 a second: 211 presents, 10.05 s");
    // (the Market at 94: the eviction's 60 presents were 0.64 s; now 95 presents, 1.01 s)
    {
        uint64_t n = 0;
        while (at60(n, sample(94, S, true, true)) < 60) n++;
        CHECK(n == 95, "94 a second: 60 presents at 60 are %llu presents", (unsigned long long)n);
    }

    // ---- the clock, as texture_frame ticks it: at each present
    {
        Clock c;
        uint64_t t = 5 * S, presents = 100;
        const auto run = [&](uint32_t f, int seconds, bool low = true, bool high = true) {   // (f presents a second for that long)
            for (int i = 0; i < seconds * (int)f; i++) {
                t += S / f, presents++;
                c.tick(presents, t, low, high);
            }
        };
        CHECK(c.rate == 256, "before any present");
        run(60, 20);
        CHECK(c.rate == 256 || c.rate == 257, "60 a second: %u", c.rate);
        run(30, 7);
        CHECK(c.rate == 256 || c.rate == 257, "seven seconds at 30 after 60: nothing shortened yet (%u)", c.rate);
        run(30, 6);
        CHECK(c.rate == 128 || c.rate == 129, "thirteen seconds at 30 (rounded up): %u", c.rate);
        t += 90 * S, presents++;   // (the window behind another for a minute and a half: one present after it)
        c.tick(presents, t, true, true);
        CHECK(c.rate == 128 || c.rate == 129, "a long gap is one slow sample: %u", c.rate);
        run(60, 2);
        CHECK(c.rate >= 250 && c.rate <= 257, "60 again: the waits are presents again within two seconds (%u)", c.rate);
        run(144, 12);
        CHECK(c.rate >= 614 && c.rate <= 616, "144 a second: 144, the waits their seconds (%u)", c.rate);
        run(60, 4);
        CHECK(c.rate >= 614, "four seconds at 60 after 144: the waits still the longer ones (%u)", c.rate);
        run(60, 6);
        CHECK(c.rate >= 256 && c.rate <= 257, "ten seconds at 60 after 144: 60 again (%u)", c.rate);
        run(144, 12, true, false);
        CHECK(c.rate == 256, "144 a second, high off: 60, as before (%u)", c.rate);
        run(4, 12);
        CHECK(c.rate == kLeast, "4 a second: counts as 15 (%u)", c.rate);
        // (RX_VK_TEX_RATE_LOW=0 and RX_VK_TEX_RATE_HIGH=0: 60 whatever the presents)
        Clock off;
        t = 5 * S, presents = 0;
        for (int i = 0; i < 20 * 30; i++) t += S / 30, presents++, off.tick(presents, t, false, false);
        for (int i = 0; i < 20 * 144; i++) t += S / 144, presents++, off.tick(presents, t, false, false);
        CHECK(off.rate == 256, "the rule before: %u", off.rate);
    }
    if (failures) return EXIT_FAILURE;
    std::printf("present rate: all checks passed\n");
    return EXIT_SUCCESS;
}
