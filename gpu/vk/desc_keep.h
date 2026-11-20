// (Triforce) Per-binding descriptor reuse (vk_draw.cpp build_descriptors), apart from Vulkan so that tests/test_desc_keep.py can run
// it. NX2navi's per-binding reuse (runtime/vk_exec_draw.c pb_take and pb_keep, chrg6's) is the model; the rules below are this
// renderer's own, from what each of its descriptors is resolved from.
//
// A draw whose program, bindings, texture slots and uniform updates are all as the draw before it keeps that draw's descriptors whole
// (vk_draw.cpp same_desc). In the owner's test7 session that was 0 to 349 draws a frame of 2,300 to 2,870, and "descriptors" was 1.5
// to 2.2 ms of a replay of 7.4 to 9.8 ms: a draw that binds one uniform buffer of its own, or anything bound anywhere, built every
// descriptor again, each texture through the views' map and the sampler table, each buffer through its pool. Now each descriptor of
// a graphics program keeps its last result beside what it was resolved from, for the last kSlots programs a recording thread drew,
// and a descriptor whose inputs and stamps are all the same takes that result instead of resolving again. What a result stands on,
// by its kind (stamps_of: the stamps are read before the descriptor is resolved, so that one moved by the resolution itself, or by
// another thread meanwhile, makes the next one resolve again):
//  - a sampled texture (TEX): its handle, view type and depth compare as inputs; the slot's version (the slot registered anew), the
//    samplers' generation (a sampler registration's contents changed), the destroy generation (a texture destroyed or made again:
//    a kept view would name freed memory, issue 1126), the dummy generation (a frame left a texture for later: a dummy is looked
//    up again) and the texture epoch (a view's texture is touched once a submission: texture_view_of does it on its first look);
//  - a user uniform buffer (UBO, c3 on): its address and size; the recording, the stream's inline uniform updates (ub_generation:
//    an update into the range is a copy with it in) and the bind stamp (gpu_bind_stamp: a pool gone or imported, a mirror made, the
//    next submission). Only a pool's or a mirror's range is kept: a copy into the recording's stream (an overlay, an unaligned range,
//    a range read where it was bound) is made again each time, as it is today;
//  - a stage's own constants (C1): the recording (they are uploaded once a recording, RecordingRes::c1_at);
//  - a storage buffer the program only reads (SSBO, a single one, not a table): its address and size; the recording and the bind
//    stamp. A pool's or a mirror's range only, as for UBO.
// The rest is resolved every time as before: the driver's banks (c0, c2: kept by their stages' generations already), storage buffers
// the program writes (each binding stamps its pages for the write-back), storage buffer tables, texel buffers and storage images.
// A failed resolution (no sampler, no buffer) is never kept: the next draw tries again.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace desc_keep {

enum Kind : uint8_t { kNone = 0, kTex, kUbo, kC1, kSsbo };

/// The stamps results stand on, read where a descriptor is about to be resolved (only those of its kind are read: stamps_of)
struct Stamps {
    uint64_t rec = 0;       // the recording (vk_draw.cpp desc_rec: moved at each recording's begin)
    uint64_t bind = 0;      // gpu_bind_stamp
    uint64_t ub = 0;        // ub_generation
    uint64_t samp = 0;      // sampler_registrations_generation
    uint64_t destroy = 0;   // texture_destroy_generation
    uint64_t dummy = 0;     // texture_dummy_generation
    uint64_t epoch = 0;     // texture_epoch
};

/// The stamps a result of that kind stands on, in a fixed order (unused ones 0)
inline std::array<uint64_t, 4> stamps_of(Kind k, const Stamps& s) {
    switch (k) {
    case kTex: return {s.samp, s.destroy, s.dummy, s.epoch};
    case kUbo: return {s.rec, s.bind, s.ub, 0};
    case kC1: return {s.rec, 0, 0, 0};
    case kSsbo: return {s.rec, s.bind, 0, 0};
    default: return {~0ull, ~0ull, ~0ull, ~0ull};
    }
}

/// One descriptor's last resolution: what it was resolved from, and its result (a buffer: handle, offset, range; a texture:
/// sampler, view as texture_view_of gave it (null: the dummy is drawn), the Texture). flags: a TEX's slot was a registered one
struct Elem {
    uint8_t kind = kNone;
    uint8_t flags = 0;
    uint32_t version = 0;   // (TEX: the slot's version)
    uint64_t in[2]{};
    std::array<uint64_t, 4> at{};
    uint64_t out[3]{};
};

/// Whether e's result serves a descriptor of kind k with those inputs (version: a TEX's slot version, 0 for the rest), now
inline bool take(const Elem& e, Kind k, uint64_t in0, uint64_t in1, uint32_t version, const Stamps& now) {
    return k != kNone && e.kind == k && e.in[0] == in0 && e.in[1] == in1 && e.version == version && e.at == stamps_of(k, now);
}

/// A descriptor resolved: kept with its inputs and the stamps read before it was resolved (before)
inline void keep(Elem& e, Kind k, uint64_t in0, uint64_t in1, uint32_t version, const Stamps& before, uint64_t out0,
                 uint64_t out1, uint64_t out2, uint8_t flags = 0) {
    e.kind = k, e.flags = flags, e.version = version;
    e.in[0] = in0, e.in[1] = in1;
    e.at = stamps_of(k, before);
    e.out[0] = out0, e.out[1] = out1, e.out[2] = out2;
}

/// Not kept (a failed resolution, or a result that is not to be taken again)
inline void drop(Elem& e) { e.kind = kNone; }

/// (RX_VK_DESC_KEEP=check) Whether a descriptor resolved again came out as its kept result
inline bool same(const Elem& e, uint64_t out0, uint64_t out1, uint64_t out2, uint8_t flags = 0) {
    return e.out[0] == out0 && e.out[1] == out1 && e.out[2] == out2 && e.flags == flags;
}

/// A recording thread's kept results: the last programs drawn, one a slot by the program's address
inline constexpr size_t kSlots = 16;
struct Prog {
    const void* prog = nullptr;
    std::vector<Elem> e;   // (by descriptor, in build_descriptors' order)
};
inline size_t slot_of(const void* prog) {
    const uintptr_t p = (uintptr_t)prog;
    return (size_t)((p >> 6) ^ (p >> 13)) % kSlots;
}
/// prog's kept results, n descriptors: emptied when its slot held another program's, or another count of them
inline Prog& prog_of(std::array<Prog, kSlots>& t, const void* prog, size_t n) {
    Prog& p = t[slot_of(prog)];
    if (p.prog != prog || p.e.size() != n) {
        p.prog = prog;
        p.e.assign(n, Elem{});
    }
    return p;
}

} // namespace desc_keep
