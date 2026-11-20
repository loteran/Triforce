// (Triforce) Opaque ASTC textures kept as BC1 instead of BC3: half the video memory, the same texels (vk_texture.cpp).
// After NX2navi's astc_bc1 (its 4c5855ce: the ASTC textures of a tour 2,568 instead of 3,104 MiB), written anew for this
// renderer. tests/test_tex_bc1.py
//
// A decoded ASTC texture the renderer keeps compressed is encoded on the GPU (host_shaders/bc_encoder_comp_spv.h). A BC3
// block is eight bytes of alpha and eight of colour, and the colour half is laid out as a BC1 block: two 5:6:5 colours and
// sixteen 2-bit indices. BC3 always reads it as four colours (c0, c1, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1). BC1 reads the same
// eight bytes that way only when c0 > c1; with c0 <= c1 it reads three colours (the third their mean) and index 3 as black,
// transparent in the RGBA format. So a colour half is the same picture as a BC1 block exactly when colour_half_same says so.
// The encoder's EncodeColor(texels, false), which writes BC3's colour half, already orders its colours c0 > c1 and writes
// all sixteen indices 0 when they are equal: every colour half it makes passes. With its push constant bc3 = 0 the encoder
// writes EncodeColor(texels, any texel's alpha under 0.5) alone, eight bytes a block: for texels that are all opaque, the
// same call with the same arguments as BC3's colour half. And BC3's alpha half for such a block is (255, 255, indices 0),
// which decodes to 255, what the BC1 RGB format gives every texel. Which textures are all opaque: astc_opaque.h.
//
// A row on the Graphics tab, Off unless set, because what a test cannot show is whether the GPU decoder agrees with
// astc_opaque.h on every block of the game's textures, and the image changes in vk_texture.cpp have not run in the game.
#pragma once
#include <cstdint>
#include <cstdlib>

namespace tex_bc1 {

/// Whether this run keeps opaque ASTC textures as BC1: RX_VK_TEX_BC1 (0: never, anything else: yes) over the Graphics
/// tab's row as the options file had it at the start (0 Off, 1 On)
inline bool wanted(const char* env, int row) {
    if (env && *env) return std::atoi(env) != 0;
    return row == 1;
}

/// Whether a BC3 block's colour half (its two colours and its indices, as stored) decodes to the same sixteen colours when
/// the same eight bytes are a BC1 block
constexpr bool colour_half_same(uint16_t c0, uint16_t c1, uint32_t indices) {
    if (c0 > c1) return true;
    if (c0 < c1) return false;
    // (equal: BC1's first three colours are that colour, as all four of BC3's are; its fourth is black)
    return ((indices & (indices >> 1)) & 0x55555555u) == 0;
}

/// The bytes a BC1 image's level chain takes: eight a block of 4 x 4 texels (BC3: sixteen)
constexpr uint64_t bytes(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers) {
    uint64_t blocks = 0;
    for (uint32_t l = 0; l < levels; l++) {
        const uint32_t w = (width >> l) ? (width >> l) : 1, h = (height >> l) ? (height >> l) : 1;
        blocks += (uint64_t)((w + 3) / 4) * ((h + 3) / 4);
    }
    return blocks * 8 * layers;
}

/// The Vulkan formats by their numbers (vk_texture.cpp asserts them against the headers): the image's, and a sampled
/// view's for the game's linear or sRGB ASTC format. The RGB formats of BC1: alpha is one whatever the block holds
constexpr int kBc1 = 131, kBc1Srgb = 132, kBc3 = 137, kBc3Srgb = 138;
constexpr int image_format(bool bc1) { return bc1 ? kBc1 : kBc3; }
constexpr int view_format(bool bc1, bool srgb) { return bc1 ? (srgb ? kBc1Srgb : kBc1) : (srgb ? kBc3Srgb : kBc3); }

/// Whether a copy on the GPU between two kept textures may be a copy of their blocks: only between images of one format.
/// A BC1 block is eight bytes and a BC3 block sixteen, so the other pairs must not reach vkCmdCopyImage
constexpr bool block_copy_ok(bool src_bc1, bool dst_bc1) { return src_bc1 == dst_bc1; }

} // namespace tex_bc1
