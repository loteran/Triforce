// (Triforce) The texture budget by live VRAM use, apart from Vulkan so that tests/test_tex_pressure.py can run it against a
// fake heap (vk_texture.cpp texture_frame sets the budget with it, texture_sweep's pass over the budget stops by it). After
// NX2navi's runtime/vk_texture.c: TEX_VRAM_PRESSURE and sweep_pressure (2551-2566, 3484-3549, chrg6 7bc7f63a), evict_idle
// (2926-2995, chrg6 63bcb441) and the commit's free space (commit_is_low, NX2NAVI_TEX_COMMIT_FREE_MB), rewritten for this
// cache (its texture table, its released list and its pool walks are not this renderer's).
//
// Why. The budget was a size: min(70% of what the driver allows, 6144 MB) on a card the driver allows more than 8.5 GB,
// 45% at or below that. On a 32 GB card that cap, not the card, is what the textures ran into: after a tour the Market
// holds 5.4-6.1 GB of textures, 3,250-3,800 of them (2,350 without the tour), and a pass over the budget cut the cache to
// 7/8 of it at once. Batches of 415, 350, 134, 559, 577 and 591 textures were retired every ~4 s for the first ~25 s there,
// each made and uploaded again within seconds (triforce-frame-stability.md, "VRAM budget cycling"; the third audit's P3).
// NX2navi has no size: once the device-local memory in use, less the free space inside the allocator's blocks, is past 80%
// of the budget the driver gives (VK_EXT_memory_budget), idle textures go, the least recently used first, 64 a check,
// until the use is 2% under that line. On a 32 GB card the line is ~24 GB: the Market never reaches it. And the line
// follows what the driver allows now, so another program's use of the card counts.
//
// The rules here:
// - live: whether the line takes the fixed size's place on this card. Decided at the first look that knows the allowance:
//   more than 8.5 GB allowed (a smaller card keeps its 45% and the rules made for it), VK_EXT_memory_budget there (without
//   it the use is the allocator's own count, not the card's), not an integrated GPU (its "VRAM" is the system's RAM: the
//   2.5 GB cap), no RX_VK_VRAM_MB (an explicit size wins), RX_VK_TEX_PRESSURE not 0. And at each look, not while a quality
//   preset's budget applies (a card no bigger than the preset's size: that size is the budget, as before)
// - the size: none; the fixed one while the system's commit is low (commit_low): Windows charges a discrete card's memory
//   to it too (NX2navi: a 42 GB process on a 32 GB PC, half of it device memory), and an allocation that fails there ends
//   the game. NX2navi's figure: less free than the larger of 4 GB and a tenth of the limit
// - the line: past pct% of what the driver allows (80), what the textures hold less the excess over 2% under the line,
//   and so on at each look until the use is 2% under it (acts); the 90% and 97% rules and the compaction's lines stay as
//   they are (the line asks for more than the 90% rule)
// - a pass over the budget: down to the budget itself (the line carries its margin; a size is still cut to 7/8 of it, as
//   before), and at most step live textures a pass (kStep, 64; the render targets drawn over since go whatever their
//   number: nothing comes back for them). While a pass stopped at its step, the next look is kNextLook presents on (the
//   GPU done with what went, its memory back in the blocks), not ten. What such a pass takes has gone unused two seconds
//   (texture_sweep's need_unused, NX2navi's STALE_FRAMES), not the one a size asks for
//
// The step on a card that keeps the fixed size: only with RX_VK_TEX_STEP set (its rules stay as they are). Below: keep
// taken and the come-back pause.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "present_rate.h"

namespace tex_pressure {

/// A card of 8.5 GB allowed or less: the fixed budget's 45% and the rules made for such cards
inline constexpr uint64_t kSmallCard = 8704ull << 20;
/// The line, in percent of what the driver allows (NX2navi's VRAM_PRESSURE_DEFAULT), and what RX_VK_TEX_PRESSURE may set
inline constexpr uint64_t kPct = 80, kPctLeast = 50, kPctMost = 95;
/// Live textures a pass over the budget lets go of at most (NX2navi's EVICT_STEP)
inline constexpr size_t kStep = 64;
/// The presents to the next look while a pass stopped at its step
inline constexpr uint64_t kNextLook = 2;

/// The line's percent, by RX_VK_TEX_PRESSURE as the environment has it: kPct unset; 0 for 0 (or anything not a number):
/// the fixed size, as before; else kPctLeast to kPctMost
inline uint64_t pct_of(const char* env) {
    if (!env || !*env) return kPct;
    const long v = strtol(env, nullptr, 10);
    return v <= 0 ? 0 : (uint64_t)std::clamp<long>(v, (long)kPctLeast, (long)kPctMost);
}

/// Whether the line takes the fixed size's place on this card (decided once): pct not 0, the driver's own budget
/// (VK_EXT_memory_budget), more than kSmallCard allowed, not an integrated GPU, no explicit size (RX_VK_VRAM_MB)
inline bool live(uint64_t pct, bool driver_budget, uint64_t allowed, bool integrated, bool size_set) {
    return pct && driver_budget && allowed > kSmallCard && !integrated && !size_set;
}

/// The fixed budget: the rule before (45% of what the driver allows at or below kSmallCard, else 70%; at most 6 GB)
inline uint64_t fixed(uint64_t allowed) {
    return std::min<uint64_t>(allowed / 100 * (allowed <= kSmallCard ? 45 : 70), 6144ull << 20);
}

/// The line itself: pct% of what the driver allows
inline uint64_t line(uint64_t allowed, uint64_t pct) { return allowed / 100 * pct; }

/// What the line's passes go down to: 2% of the allowance under it
inline uint64_t under(uint64_t allowed, uint64_t pct) {
    const uint64_t at = line(allowed, pct);
    return at - std::min(at, allowed / 50);
}

/// Whether the line acts at this look (pressure: the driver's count less the free space the textures can take back): past
/// it, or, when it acted at the look before (acted), still past 2% under it. Its passes go on a step at a time until the use
/// is 2% under the line, not only until it is back under the line (where the next texture made would cross it again)
inline bool acts(uint64_t pressure, uint64_t allowed, uint64_t pct, bool acted) {
    return allowed && pct && (pressure > line(allowed, pct) || (acted && pressure > under(allowed, pct)));
}

/// The budget by live use. The size: none (UINT64_MAX), or `size` (the fixed one) while the system's commit is low. While
/// the line acts (acting, above), at most what the textures hold (total) less the excess of the use over 2% under the line
inline uint64_t budget(uint64_t size, uint64_t total, uint64_t pressure, uint64_t allowed, uint64_t pct, bool commit_low, bool acting) {
    uint64_t b = commit_low ? size : UINT64_MAX;
    const uint64_t to = under(allowed, pct);
    if (acting && pressure > to) {
        const uint64_t over = pressure - to;
        b = std::min(b, total > over ? total - over : 0);
    }
    return b;
}

/// What a pass over the budget goes down to: the budget itself where the line set it (live), else 7/8 of it, as before
inline uint64_t cut_to(uint64_t budget, bool live) { return live ? budget : budget / 8 * 7; }

/// Whether a pass over the budget has done its part: what the textures hold (total) less what it chose (pending) at cut_to.
/// (total can have drifted below the textures' sum: chosen bytes past it mean the budget is met, not a wrapped difference)
inline bool met(uint64_t total, uint64_t pending, uint64_t budget, bool live) {
    return pending >= total || total - pending <= cut_to(budget, live);
}

/// Whether a pass took its step: step live textures chosen (step 0: no step)
inline bool stepped(size_t step, size_t live_chosen) { return step && live_chosen >= step; }

/// The step: RX_VK_TEX_STEP as the environment has it, on any card (0: none); unset, kStep where the line is live, none
/// where the fixed size is
inline size_t step_of(const char* env, bool live) {
    if (env && *env) {
        const long v = strtol(env, nullptr, 10);
        return v > 0 ? (size_t)v : 0;
    }
    return live ? kStep : 0;
}

/// Whether the system's commit is low: less of it free (avail) than want_mb MB (RX_VK_TEX_COMMIT_FREE_MB), -1 for
/// NX2navi's default (the larger of 4 GB and a tenth of the limit), 0: never. A limit of 0: not known, never low
inline bool commit_low(uint64_t avail, uint64_t limit, int64_t want_mb) {
    if (!limit || !want_mb) return false;
    const uint64_t want = want_mb > 0 ? (uint64_t)want_mb << 20 : std::max<uint64_t>(4096ull << 20, limit / 10);
    return avail < want;
}

// ---- keep taken (after NX2navi's TEX_KEEP_TAKEN: vk_texture.c 2777-2795, chrg6 4b87606f)
// The game lets go of a texture (finalizes it) when what uses it leaves the view, and takes it back when that returns. While
// VRAM is short a texture uploaded from memory that the game let go RX_VK_LET_GO_AFTER presents ago (30) goes first, whatever
// its age (texture_sweep's let-go rule). Taken back after it went, it is made and uploaded again, and the frame that wants it
// samples the dummy meanwhile. NX2navi saw that in Kakariko: the hazy air's ASTC textures the game lets go and takes back
// every few hundred frames went as soon as their wait was up, and the frame that needed them came out without the haze,
// darker and sharper, every few seconds. So a texture the game has taken back waits 8 times the longest it stayed let go
// before a take-back (texture_touch notes it) before it goes early; the eviction by age still takes it as before.
// RX_VK_TEX_KEEP_TAKEN=0: the rule's own wait alone, as before.
inline constexpr uint64_t kGapTimes = 8;

/// A let-go texture used again, `presents` after the game let go of it: the longest such gap so far (capped, so that 8 times
/// it fits)
inline uint32_t taken_back(uint32_t gap, uint64_t presents) {
    return (uint32_t)std::max<uint64_t>(gap, std::min<uint64_t>(presents, 1u << 24));
}

/// How long after the game let go of a texture it may go early, in presents at 60 a second: the rule's own wait (after), or
/// with keep taken on 8 times the longest gap before a take-back (gap60: that gap at 60 a second) when that is longer
inline uint64_t let_go_wait(uint64_t after, uint64_t gap60, bool on) { return on ? std::max(after, gap60 * kGapTimes) : after; }

// ---- the come-back pause (after NX2navi's: vk_texture.c 2069-2129, chrg6 90e41c13)
// A texture the line lets go frees its memory only for as long as the game does not ask for it again; when it does, it is
// made and uploaded again and holds the same memory as before, and the frame that wanted it draws the dummy. NX2navi met it
// under its commit rule: with the commit at its low line for whole sessions, 340 to 590 textures a second went, most were
// made again within seconds (178 re-uploads a second against 21 without), a 35-45 ms frame several times a second, and the
// commit stayed where it was. So each texture the line lets go is noted (kSlots slots by its key, the newest wins), and one
// made again within kWithin presents counts as come back. Over each kPeriod, when kMinDrops or more went and half or more
// came back, the line stands aside for kHoldMin presents, doubled at each such period up to kHoldMax; a period in which
// fewer came back ends the doubling. Meanwhile the 90% and 97% rules and the compaction's lines still hold (NX2navi's: below
// half the low line nothing pauses), so the use stays under 90% of what the driver allows. Presents at 60 a second
// (present_rate.h). RX_VK_TEX_COME_BACK=0: no pause.
struct ComeBack {
    static constexpr uint32_t kSlotBits = 13, kSlots = 1u << kSlotBits;
    static constexpr uint64_t kWithin = 600, kPeriod = 240, kMinDrops = 64, kHoldMin = 600, kHoldMax = 7200;
    struct Slot {
        uint64_t key = 0, frame = 0;   // (frame 0: free, or come back already)
    };
    Slot slot[kSlots];
    uint64_t drops = 0, backs = 0, t0 = 0;             // (the period being counted)
    uint64_t last_drops = 0, last_backs = 0;           // (the period judged last: the log's)
    uint64_t hold = 0, held_from = 0, backs_all = 0;   // (the pause's length, its start; come back so far)
    bool holding = false;

    static uint32_t at(uint64_t key) { return (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> (64 - kSlotBits)); }
    /// The line let go of the texture with this key at present `now`
    void went(uint64_t key, uint64_t now) {
        Slot& s = slot[at(key)];
        s.key = key, s.frame = now | 1;
        drops++;
    }
    /// A texture made at present `now` (at `rate`): true when the line let it go within kWithin presents at 60 a second
    bool made(uint64_t key, uint64_t now, uint32_t rate) {
        Slot& s = slot[at(key)];
        if (s.key != key || !s.frame || now < s.frame || present_rate::at60(now - s.frame, rate) > kWithin) return false;
        s.frame = 0, backs++, backs_all++;
        return true;
    }
    /// Whether the line stands aside at present `now`, judging the period that ended; *began: a pause began just now
    bool held(uint64_t now, uint32_t rate, bool* began) {
        *began = false;
        if (now < t0 || present_rate::at60(now - t0, rate) >= kPeriod) {
            if (drops >= kMinDrops && backs * 2 >= drops) {
                hold = hold ? std::min(hold * 2, kHoldMax) : kHoldMin;
                held_from = now, holding = true, *began = true;
            } else if (drops >= kMinDrops) {
                hold = 0;
            }
            last_drops = drops, last_backs = backs;
            t0 = now, drops = backs = 0;
        }
        if (holding && present_rate::at60(now - held_from, rate) >= hold) holding = false;
        return holding;
    }
};

/// The budget the log shows: the budget, or while the line sets none what the textures may hold before it is reached
inline uint64_t shown(uint64_t budget, uint64_t total, uint64_t pressure, uint64_t allowed, uint64_t pct) {
    if (budget != UINT64_MAX) return budget;
    const uint64_t at = line(allowed, pct);
    return total + (at > pressure ? at - pressure : 0);
}

}   // namespace tex_pressure
