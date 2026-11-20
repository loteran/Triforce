// (Triforce) The whole-draw skip by the signature of a draw's descriptor inputs (vk_draw.cpp desc_sig_walk and draw), apart from
// Vulkan so that tests/test_desc_sig.py can run it. NX2navi's bind_sig (runtime/vk_exec_draw.c) is the model; its code is not
// chrg6's, so this is written anew from what it does, on this renderer's own rules (desc_keep.h).
//
// A draw keeps the descriptors the draw before it pushed only while nothing at all was bound anew, no inline uniform update came and
// no texture slot changed (vk_draw.cpp same_desc): 381 of 2,830 draws a frame in the owner's test18. The others build every
// descriptor again, even when what they read is what the draw before them read: a uniform buffer bound for another stage, a texture
// bound at a slot the program does not sample. Through the per-binding reuse (desc_keep.h) each of their descriptors then takes the
// result it had, and the push finds nothing changed: the build and the compare were the work, 2.0 to 3.3 ms a frame.
//
// Now such a draw first reads its signature: what each of its descriptors would be resolved from (desc_keep.h's inputs: a texture's
// handle and its slot's version, a user uniform buffer's address and size, a read-only storage buffer's address and size words, a
// driver bank's stage generation), and the stamps desc_keep.h's rules give each kind it has (stamps_of), read before the inputs.
// When it equals the signature of the descriptors at the graphics bind point, which are this program's, the build and the push are
// left out: a build would take every one of them from what was kept (each input and stamp is the same) and push nothing.
// That holds only where every descriptor of the last build was kept, so the signature stands for
//  - push programs only (a set's bind point state is not followed), not the mesh fallback, not RenoDX's;
//  - programs whose every descriptor is of a kind the reuse keeps (shape_of): sampled textures, user uniform buffers (c3 to c18),
//    a stage's constants (c1), single read-only storage buffers, and the driver's banks (c0, c2), which their own cache keeps by
//    the stage's generations. Not storage buffers a program writes (each build stamps their pages for the write-back), tables,
//    texel buffers or storage images;
//  - builds that dropped nothing (armed): a failed resolution, a copy in the recording's stream (an inline update's overlay, an
//    unaligned range) or an unbound buffer make the next draw build again.
// The textures a hit samples that the GPU wrote (pass hazards: gfx_sampled) are read again from the last build's textures, as a
// build would. A signature is the bind point's only once that draw's descriptors were bound: a draw left out between its build and
// its bind leaves the last one in place.
// RX_VK_DESC_SIG=0: every such draw builds, as before. =check: a hit builds too, and one whose descriptors or sampled textures come
// out otherwise than what is pushed is counted and logged ("vk: descriptor signature check").
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "desc_keep.h"

namespace desc_sig {

/// The kinds of descriptor a program has, one bit each by desc_keep::Kind (bit kTex, kUbo, kC1, kSsbo); kBank its driver banks
enum : uint8_t { kBank = 1u << 7 };
constexpr uint8_t bit(desc_keep::Kind k) { return (uint8_t)(1u << k); }

/// What a program's descriptors are, from its stages' lists (made once a program: they do not change once it is translated)
struct Shape {
    const void* prog = nullptr;
    bool ok = false;      // (every descriptor of a kind the signature stands for)
    uint8_t kinds = 0;
};
inline constexpr size_t kShapes = 64;
inline size_t shape_slot(const void* prog) {
    const uintptr_t p = (uintptr_t)prog;
    return (size_t)((p >> 6) ^ (p >> 14)) % kShapes;
}

/// The signature's head: the program, the recording (c1 and the banks live in its streams), and the stamps of each kind it has, in
/// desc_keep.h's order (the same stamps a kept result of that kind stands on)
inline void put_head(std::vector<uint64_t>& out, const void* prog, uint8_t kinds, const desc_keep::Stamps& now) {
    out.push_back((uint64_t)(uintptr_t)prog);
    out.push_back(now.rec);
    out.push_back(kinds);
    for (desc_keep::Kind k : {desc_keep::kTex, desc_keep::kUbo, desc_keep::kC1, desc_keep::kSsbo})
        if (kinds & bit(k))
            for (uint64_t v : desc_keep::stamps_of(k, now)) out.push_back(v);
}

/// A signature, and what its build left besides the descriptors: the textures it sampled through a view (draw_sampled keeps those
/// the GPU wrote) and the dummy counts it added (the frame statistics')
struct Sig {
    const void* prog = nullptr;
    bool armed = false;   // (every descriptor of that build was kept: a draw with the same signature may take them all)
    std::vector<uint64_t> w;
    std::vector<const void*> tex;
    int64_t dummies = 0, dummies_known = 0;
};

/// Whether a draw of prog whose signature is now may keep the descriptors bound, which bound's draw built
inline bool hit(const Sig& bound, const Sig& now, const void* prog) {
    return bound.armed && bound.prog == prog && now.prog == prog && !now.w.empty() && bound.w == now.w;
}
/// now's draw built its descriptors and they were bound: its signature is the bind point's (armed: none of them dropped)
inline void commit(Sig& bound, Sig& now, bool armed) {
    std::swap(bound.w, now.w);
    std::swap(bound.tex, now.tex);
    bound.prog = now.prog, bound.armed = armed;
    bound.dummies = now.dummies, bound.dummies_known = now.dummies_known;
    now.prog = nullptr, now.w.clear();
}
/// Descriptors bound that no signature stands for (a program the skip does not take, a build that dropped one)
inline void forget(Sig& bound) {
    bound.prog = nullptr, bound.armed = false;
    bound.w.clear();
}

} // namespace desc_sig
