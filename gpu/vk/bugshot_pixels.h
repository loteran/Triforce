// (Triforce) The pixels of a bug capture's picture (vk_bugshot.cpp): the window's image as the GPU copied it out, into the
// R, G, B, 255 bytes a PNG is written from. Apart from Vulkan, so tests/test_bug_capture.py compiles it alone. The formats
// are the ones create_swapchain picks (vk_main.cpp): 8-bit RGBA or BGRA, UNORM or sRGB (the bytes are sRGB codes either
// way), and with the Insert menu's HDR a 10-bit HDR10 image, ST 2084 codes of Rec. 2020 light. That one is brought to
// sRGB with the picture's paper white as white, so a PNG viewer shows what an SDR screen would show of the game:
// highlights above white and colours outside sRGB are cut off.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bugshot {

enum Kind { NONE, RGBA8, BGRA8, A2B10G10R10, A2R10G10B10 };

/// What a window image's format is to this conversion, by its VkFormat number (vk_bugshot.cpp checks the numbers against
/// Vulkan's names); NONE: not one it reads
inline Kind kind_of(int vk_format) {
    switch (vk_format) {
    case 37: case 43: return RGBA8;    // (R8G8B8A8_UNORM, R8G8B8A8_SRGB)
    case 44: case 50: return BGRA8;    // (B8G8R8A8_UNORM, B8G8R8A8_SRGB)
    case 64: return A2B10G10R10;       // (A2B10G10R10_UNORM_PACK32: red in the low ten bits)
    case 58: return A2R10G10B10;       // (A2R10G10B10_UNORM_PACK32: blue in the low ten bits)
    default: return NONE;
    }
}

/// ST 2084's curve the display's way: a code 0 to 1 as nits, 0 to 10000
inline double pq_to_nits(double code) {
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    const double p = std::pow(code < 0 ? 0 : code > 1 ? 1 : code, 1.0 / m2);
    return 10000.0 * std::pow((p > c1 ? p - c1 : 0) / (c2 - c3 * p), 1.0 / m1);
}

/// Linear light 0 to 1 as an sRGB code 0 to 255
inline uint8_t srgb8(double linear) {
    const double x = linear < 0 ? 0 : linear > 1 ? 1 : linear;
    const double e = x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
    return (uint8_t)(e * 255.0 + 0.5);
}

/// n texels at px, four bytes each as the GPU wrote them, to R, G, B, 255 in place. hdr10: the image holds ST 2084 codes of
/// Rec. 2020 light (a 10-bit format only), and white_nits is the picture's paper white. False: not a format this reads
inline bool to_rgba8(uint8_t* px, size_t n, Kind kind, bool hdr10, double white_nits) {
    if (kind == NONE) return false;
    if (kind == RGBA8 || kind == BGRA8) {
        for (size_t i = 0; i < n; i++) {
            uint8_t* q = px + i * 4;
            if (kind == BGRA8) {
                const uint8_t b = q[0];
                q[0] = q[2], q[2] = b;
            }
            q[3] = 255;
        }
        return true;
    }
    // (ten bits a channel. HDR: a table from a code to its light, white at 1, and one from light to an sRGB code, so a 4K
    // picture is a few table reads a texel and not nine pow calls)
    constexpr int kSteps = 4096;
    std::vector<float> light(hdr10 ? 1024 : 0);
    std::vector<uint8_t> code8(hdr10 ? kSteps + 1 : 0);
    const double white = white_nits > 1 ? white_nits : 1;
    for (size_t v = 0; v < light.size(); v++) light[v] = (float)(pq_to_nits((double)v / 1023.0) / white);
    for (size_t v = 0; v < code8.size(); v++) code8[v] = srgb8((double)v / kSteps);
    const auto out8 = [&](float x) { return code8[x <= 0 ? 0 : x >= 1 ? kSteps : (int)(x * kSteps + 0.5f)]; };
    for (size_t i = 0; i < n; i++) {
        uint8_t* q = px + i * 4;
        const uint32_t v = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
        const uint32_t lo = v & 1023, mid = (v >> 10) & 1023, hi = (v >> 20) & 1023;
        const uint32_t r = kind == A2B10G10R10 ? lo : hi, g = mid, b = kind == A2B10G10R10 ? hi : lo;
        if (!hdr10) {   // (sRGB codes with ten bits: the nearest of eight)
            q[0] = (uint8_t)((r * 255 + 511) / 1023), q[1] = (uint8_t)((g * 255 + 511) / 1023), q[2] = (uint8_t)((b * 255 + 511) / 1023);
        } else {   // (Rec. 2020 light to Rec. 709's primaries, ITU-R BT.2087's matrix, then sRGB's curve)
            const float lr = light[r], lg = light[g], lb = light[b];
            q[0] = out8(1.6605f * lr - 0.5876f * lg - 0.0728f * lb);
            q[1] = out8(-0.1246f * lr + 1.1329f * lg - 0.0083f * lb);
            q[2] = out8(-0.0182f * lr - 0.1006f * lg + 1.1187f * lb);
        }
        q[3] = 255;
    }
    return true;
}

} // namespace bugshot
