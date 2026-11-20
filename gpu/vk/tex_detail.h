// Texture detail by the card's video memory (vk_texture.cpp create): which cards make the game's large material
// textures without their largest mip levels, and which textures that takes. After NX2navi's texture_detail
// (its runtime/vk_tex_detail.h and vk_texture.c "texture detail"; the figures below are its measurements).
// tests/test_tex_detail.py
//
// The material textures of a loaded area are what fills a small card, and three quarters of a mipmapped texture is its
// largest level. Past what the driver allows, Windows moves video memory into system memory and the frame rate falls
// to a fraction. A walk through Castle Town with the allowance taken as a 6 GB card's 4,730 MB held 6,907 MB whole,
// 5,472 MB without the largest level and 4,987 MB without two; a tour sized for an 8 GB card ran 29.6 fps (1% low
// 11.4) whole and 42.8 (36.4) without the largest level, nothing spilled.
//
// The Insert menu's Texture detail row chooses among the same three levels, Automatic (the rule below) unless set;
// RX_VK_TEX_DETAIL still goes over the row (choose).
#pragma once
#include <cstdint>
#include <cstring>

namespace tex_detail {

enum Level { kFull = 0, kReduced = 1, kLow = 2 };

// What the driver allows the process at the start, in MB: full from here on (the cards of 12 GB: 11,250-11,313 MB
// in players' logs; a 10 GB card's ~9,300 MB is under it, as what "full" takes is more than its textures)
constexpr uint64_t kFullMb = 10240;
// low under this (every 6 GB card); reduced between (8 GB cards: ~7,100-7,600 MB)
constexpr uint64_t kLowMb = 6144;
// A texture loses a level only from this size each way, two only from kLowMin: interface art, glyph atlases, lookup
// tables and ramps are smaller or have no mip levels
constexpr uint32_t kMin = 256;
constexpr uint32_t kLowMin = 1024;

/// The level "auto" picks. allowed: bytes, 0 when the driver reports none (then full, as before). integrated: the
/// video memory is the system's own, whose allowance is a share of the PC's memory, not memory of its own: never full
/// (a handheld with 16 GB in all reported 10,361 MB and ended at a save's load with everything whole).
inline int by_vram(uint64_t allowed, bool integrated) {
    const uint64_t mb = allowed >> 20;
    if (!mb) return kFull;
    int level = mb >= kFullMb ? kFull : mb >= kLowMb ? kReduced : kLow;
    if (level == kFull && integrated) level = kReduced;
    return level;
}

/// RX_VK_TEX_DETAIL's value: 0 full, 1 reduced, 2 low; -1 for auto, unset or anything else.
inline int parse(const char* v) {
    if (!v) return -1;
    return !strcmp(v, "full") ? kFull : !strcmp(v, "reduced") ? kReduced : !strcmp(v, "low") ? kLow : -1;
}

/// The Insert menu's Texture detail row (texture_detail= in the options file: 0 Automatic, 1 Full, 2 Reduced, 3 Low) as
/// a level; -1 for Automatic and for a value past the row.
inline int from_row(int row) { return row >= 1 && row <= 3 ? row - 1 : -1; }

/// The level in effect: RX_VK_TEX_DETAIL's (asked: parse()) over the row's (row: from_row()) over what the video
/// memory gives (by_memory: by_vram()).
inline int choose(int asked, int row, int by_memory) { return asked >= 0 ? asked : row >= 0 ? row : by_memory; }

/// The top levels a texture is made without at that detail level. w, h, levels: the texture whole. The caller has
/// checked what the sizes cannot say: a decoded ASTC texture, 2D or a 2D array, one sample, sampled only.
inline uint32_t skip(int level, uint32_t w, uint32_t h, uint32_t levels) {
    if (level <= kFull || levels < 2 || w < kMin || h < kMin) return 0;
    return level >= kLow && levels >= 4 && w >= kLowMin && h >= kLowMin ? 2 : 1;
}

inline const char* name(int level) {
    return level == kFull ? "full" : level == kReduced ? "reduced" : "low";
}

} // namespace tex_detail
