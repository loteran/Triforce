// (Triforce) Which ASTC blocks the GPU decoder (host_shaders/astc_decoder_comp_spv.h, Eden's) writes with alpha 255 in every
// texel, told from the block's 16 bytes alone: what lets a decoded ASTC texture be kept as BC1 instead of BC3 (vk_texture.cpp,
// tex_bc1.h). tests/test_tex_bc1.py
//
// The answer has to be the decoder's, not the ASTC specification's: a block the decoder takes for an error is written as
// (0, 0, 0, 0), alpha 0, and its endpoint modes with alpha may still decode to 255 everywhere. So this says yes only where the
// decoder's own code leaves no other outcome, and no everywhere else (a no keeps the texture BC3, as it was):
//
// - a void extent: the decoder's IsError takes one for an error when it is the HDR kind (bit 9) or bits 10 and 11 are not both
//   set, and FillVoidExtentLDR writes the constant's alpha, bits 112 to 127, divided by 65535: 255 only for 0xFFFF. It does
//   not look at the extent's coordinates, so neither does this.
// - any other block, the errors in DecompressBlock's order: the reserved block modes (bits 0 to 3 zero; bits 0 and 1 zero
//   with bits 6 to 8 set), a weight grid larger than the block, four partitions with two planes, more than 64 weights, the
//   weights' bits under 24 or over 96, and no room left for the colour data (its count of the remaining bits wraps).
// - then the colour endpoint modes. Modes 0, 1, 6, 8 and 9 (luminance and RGB, low dynamic range) give both endpoints the
//   alpha 255 as a constant (ComputeEndpoints; BlueContract and ClampByte pass it through), and the decoder's interpolation
//   of two equal 16-bit values, (65535 * (64 - w) + 65535 * w + 32) / 64, is 65535 for every weight, which it writes as
//   65535 / 65535 = 1. Modes 4, 5, 10, 12 and 13 carry alpha, and 2, 3, 7, 11, 14 and 15 are the HDR ones, which take the
//   decoder's other path: no for all of those, without decoding them (such a block may be opaque all the same).
//   One partition has its mode in bits 13 to 16. Several partitions share one mode when bits 23 and 24 are zero (the mode is
//   bits 25 to 28); when they do not, the modes' other bits sit below the weights and are not looked for: no.
#pragma once
#include <cstdint>

namespace astc_opaque {

/// `n` bits (16 at most) of a block from bit `at`, the lowest first, as the decoder's StreamBits reads them
inline uint32_t bits(const uint8_t* b, unsigned at, unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 3 && at / 8 + i < 16; i++) v |= (uint32_t)b[at / 8 + i] << (8 * i);
    return (v >> (at & 7)) & ((1u << n) - 1);
}

/// The bits `n` weights take in the weight range `index` (0 to 11: 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24 and 32 levels), as
/// the decoder's GetBitLength counts them: the plain bits, and a trit for every 8/5 of a bit or a quint for every 7/3
inline unsigned weight_bits(unsigned n, unsigned index) {
    static const uint8_t plain[12] = {1, 0, 2, 0, 1, 3, 1, 2, 4, 2, 3, 5};
    static const uint8_t kind[12] = {0, 1, 0, 2, 1, 0, 2, 1, 0, 2, 1, 0};   // (1: with a trit, 2: with a quint)
    unsigned total = n * plain[index];
    if (kind[index] == 1) total += (n * 8 + 4) / 5;
    if (kind[index] == 2) total += (n * 7 + 2) / 3;
    return total;
}

/// Whether the decoder writes alpha 255 into every texel of this block of bw x bh texels (see the top of the file). False
/// for a block size the decoder is not given (under 4 or over 12 a side)
inline bool block(const uint8_t b[16], unsigned bw, unsigned bh) {
    if (bw < 4 || bh < 4 || bw > 12 || bh > 12) return false;
    const uint32_t mode = bits(b, 0, 11);
    if ((mode & 0x1FF) == 0x1FC) {   // (a void extent)
        if (mode & 0x200) return false;
        if (!(mode & 0x400) || !bits(b, 11, 1)) return false;
        return b[14] == 0xFF && b[15] == 0xFF;
    }
    if (!(mode & 0xF)) return false;
    if (!(mode & 3) && (mode & 0x1C0) == 0x1C0) return false;
    // (the weight grid, by the decoder's FindLayout and DecodeBlockSize)
    const unsigned a = (mode >> 5) & 3, b2 = (mode >> 7) & 3, b1 = (mode >> 7) & 1;
    unsigned layout, w, h;
    if (mode & 3) {
        if (mode & 8) {
            if (!(mode & 4)) layout = 2, w = a + 2, h = b2 + 8;
            else if (mode & 0x100) layout = 4, w = b1 + 2, h = a + 2;
            else layout = 3, w = a + 2, h = b1 + 6;
        } else if (mode & 4) {
            layout = 1, w = b2 + 8, h = a + 2;
        } else {
            layout = 0, w = b2 + 4, h = a + 2;
        }
    } else if (mode & 0x100) {
        if (!(mode & 0x80)) layout = 9, w = a + 6, h = ((mode >> 9) & 3) + 6;
        else if (mode & 0x20) layout = 8, w = 10, h = 6;
        else layout = 7, w = 6, h = 10;
    } else if (mode & 0x80) {
        layout = 6, w = a + 2, h = 12;
    } else {
        layout = 5, w = 12, h = a + 2;
    }
    if (w > bw || h > bh) return false;
    const bool dual = layout != 9 && (mode & 0x400);
    const unsigned parts = bits(b, 11, 2) + 1;
    if (parts == 4 && dual) return false;
    const unsigned weights = w * h * (dual ? 2 : 1);
    if (weights > 64) return false;
    // (the weights' range, by the decoder's DecodeMaxWeight: bit 4 and two more bits, less 2, six further with bit 9)
    unsigned range = ((mode >> 4) & 1) | (layout < 5 ? (mode & 3) << 1 : (mode & 0xC) >> 1);
    if (range < 2) return false;   // (cannot be: the reserved modes above)
    range = range - 2 + (layout != 9 && (mode & 0x200) ? 6 : 0);
    const unsigned wbits = weight_bits(weights, range);
    if (wbits < 24 || wbits > 96) return false;
    unsigned read, cem;
    if (parts == 1) {
        cem = bits(b, 13, 4), read = 17;
    } else {
        const uint32_t base = bits(b, 23, 6);
        if (base & 3) return false;   // (a mode a partition: not looked for)
        cem = base >> 2, read = 29;
    }
    // (the decoder's last error, its count of the remaining bits wrapping. With one shared mode it cannot: 96 + 29 + 2)
    if (wbits + read + (dual ? 2u : 0u) > 128) return false;
    return cem == 0 || cem == 1 || cem == 6 || cem == 8 || cem == 9;
}

/// Whether every one of `n` blocks (16 bytes each, in any order) is one the decoder writes opaque
inline bool all(const uint8_t* blocks, uint64_t n, unsigned bw, unsigned bh) {
    for (uint64_t i = 0; i < n; i++)
        if (!block(blocks + 16 * i, bw, bh)) return false;
    return true;
}

} // namespace astc_opaque
