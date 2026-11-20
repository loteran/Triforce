// tests/test_bug_capture.py's program for gpu/vk/bugshot_pixels.h: the window's texels as the GPU copies them out, to the
// R, G, B, 255 bytes a bug capture's PNG is written from.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bugshot_pixels.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// ST 2084 the other way (nits to a code 0..1), to make HDR10 texels of known light
static double nits_to_pq(double nits) {
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    const double y = std::pow(nits / 10000.0, m1);
    return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
static uint32_t code10(double nits) { return (uint32_t)(nits_to_pq(nits) * 1023.0 + 0.5); }
// a 10-bit texel, red in the low bits (A2B10G10R10) or blue (A2R10G10B10), as little-endian bytes
static void put10(uint8_t* q, uint32_t r, uint32_t g, uint32_t b, bool red_low) {
    const uint32_t v = 3u << 30 | (red_low ? b : r) << 20 | g << 10 | (red_low ? r : b);
    q[0] = (uint8_t)v, q[1] = (uint8_t)(v >> 8), q[2] = (uint8_t)(v >> 16), q[3] = (uint8_t)(v >> 24);
}
static bool near(int a, int b, int by) { return std::abs(a - b) <= by; }

int main() {
    using namespace bugshot;
    CHECK(kind_of(37) == RGBA8 && kind_of(43) == RGBA8 && kind_of(44) == BGRA8 && kind_of(50) == BGRA8);
    CHECK(kind_of(64) == A2B10G10R10 && kind_of(58) == A2R10G10B10 && kind_of(0) == NONE && kind_of(97) == NONE && kind_of(109) == NONE);
    {   // 8 bits: as they are, or red and blue swapped; alpha gone
        uint8_t px[8] = {10, 20, 30, 0, 200, 100, 50, 7};
        CHECK(to_rgba8(px, 2, RGBA8, false, 200) && !memcmp(px, "\x0a\x14\x1e\xff\xc8\x64\x32\xff", 8));
        uint8_t bg[8] = {30, 20, 10, 0, 50, 100, 200, 7};
        CHECK(to_rgba8(bg, 2, BGRA8, false, 200) && !memcmp(bg, "\x0a\x14\x1e\xff\xc8\x64\x32\xff", 8));
        CHECK(!to_rgba8(px, 2, NONE, false, 200));
    }
    {   // 10 bits of sRGB codes: the nearest of 8, each channel from its own place
        uint8_t px[12];
        put10(px, 1023, 0, 512, true), put10(px + 4, 4, 1019, 2, true), put10(px + 8, 1023, 0, 512, false);
        CHECK(to_rgba8(px, 3, A2B10G10R10, false, 200));
        CHECK(px[0] == 255 && px[1] == 0 && px[2] == 128 && px[3] == 255 && px[4] == 1 && px[5] == 254 && px[6] == 0);
        CHECK(px[8] == 128 && px[9] == 0 && px[10] == 255);   // (read as the other order: red and blue the other way round)
        uint8_t other[4];
        put10(other, 1023, 0, 512, false);
        CHECK(to_rgba8(other, 1, A2R10G10B10, false, 200) && other[0] == 255 && other[1] == 0 && other[2] == 128);
    }
    // the curve itself: ST 2084's fixed points
    CHECK(std::fabs(pq_to_nits(0.0)) < 1e-9 && std::fabs(pq_to_nits(1.0) - 10000.0) < 1e-6);
    CHECK(std::fabs(pq_to_nits(nits_to_pq(100.0)) - 100.0) < 1e-6 && std::fabs(pq_to_nits(0.5081) - 100.0) < 0.5);
    CHECK(srgb8(0) == 0 && srgb8(1) == 255 && srgb8(2) == 255 && srgb8(-1) == 0 && srgb8(0.2159) == 128 && srgb8(0.0021) == 7);
    for (const double white : {80.0, 200.0, 350.0}) {   // HDR10: the paper white is the PNG's white, whatever it is set to
        uint8_t px[28];
        const uint32_t w = code10(white), g = code10(white * 0.2159), k = code10(white * 0.0021);
        put10(px, w, w, w, true);                                      // white
        put10(px + 4, g, g, g, true);                                  // middle grey: sRGB 128
        put10(px + 8, 0, 0, 0, true);                                  // black
        put10(px + 12, code10(4000), code10(4000), code10(4000), true);   // a highlight far above white: cut off at white
        put10(px + 16, k, k, k, true);                                 // a dark grey: sRGB 7
        put10(px + 20, code10(white * 0.6274), code10(white * 0.0691), code10(white * 0.0164), true);   // sRGB's own red, as Rec. 2020 has it
        put10(px + 24, w, 0, 0, true);                                 // Rec. 2020's red: outside sRGB, cut off
        CHECK(to_rgba8(px, 7, A2B10G10R10, true, white));
        CHECK(near(px[0], 255, 1) && near(px[1], 255, 1) && near(px[2], 255, 1) && px[3] == 255);
        CHECK(near(px[4], 128, 2) && near(px[5], 128, 2) && near(px[6], 128, 2));
        CHECK(px[8] == 0 && px[9] == 0 && px[10] == 0);
        CHECK(px[12] == 255 && px[13] == 255 && px[14] == 255);
        CHECK(near(px[16], 7, 2) && near(px[17], 7, 2) && near(px[18], 7, 2));
        CHECK(near(px[20], 255, 2) && px[21] <= 12 && px[22] <= 12);
        CHECK(px[24] == 255 && px[25] == 0 && px[26] == 0);
        uint8_t other[4];   // (and the other channel order)
        put10(other, w, g, 0, false);
        CHECK(to_rgba8(other, 1, A2R10G10B10, true, white) && other[0] > other[1] && other[1] > other[2] && other[3] == 255);
    }
    if (!fails) printf("PASS\n");
    return fails != 0;
}
