// (Triforce) A recording thread's texture looks kept through other textures' ends (vk_draw.cpp texture_view_of), apart from
// Vulkan so that tests/test_view_keep.py can run it. NX2navi's tex_gen_undo and its texture table walked by its entries
// (runtime/vk_texture.c, 0.8.0: "about 7 % more frames where CPU-bound" in Kokiri Forest) do this for its own table of slots;
// this renderer looks textures up otherwise, so the rule is written here for its lookups, not copied.
//
// A recording thread keeps what each texture handle it met resolved to (the slot's version, the texture, its view) and takes it
// again at the next draw that names the handle, until the slot is registered anew or the destroy generation moves. That
// generation moves whenever any texture goes (an eviction, a dead render target let go, which while the camera moves is every
// one to three presents), is made BC3 from BC1, or has its image moved by the compaction; and then every handle of every draw
// was resolved anew on every recording thread: the slot read, the texture found in the cache, its view in its own, under the
// parallel replay's lock. A look stays right while its own texture keeps the views it had: the slot names the same texture
// (its version is unchanged), the cache gives the same texture for it (a texture leaves the cache only by going) and the
// texture the same view (its views go only when it goes, is made again in another format, or is moved). So the textures whose
// views went are noted by their creation number (vk_texture.cpp views_gone: after the views went, before the generation
// moves), and when a thread finds the generation moved, one pass over its looks moves every look of a texture not among them
// to the new generation; the others are resolved anew when next met, as before. Never a look that resolved to no texture or
// to no view (one could be made since), nor a flat texture's 3D copy (copied again from the texture at each look,
// vk_texture.cpp twin3d_view). When more textures went since the last pass than the record keeps, every look is resolved
// anew. RX_VK_VIEW_KEEP=0: every look resolved anew at any texture's end, as before.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace view_keep {

/// RX_VK_VIEW_KEEP: on unless it is "0"
inline bool wanted(const char* env) { return !(env && env[0] == '0'); }

/// Whether a look the pass finds stands: the look is of generation `gen`, of the texture numbered `id` (0: none), `stable`
/// (a view of the texture's own image); `last` is the generation the thread's last pass moved its looks to; `gone` the
/// numbers of the textures whose views went since then, sorted; `told` false when those could not all be told
inline bool stands(uint64_t gen, uint32_t id, bool stable, uint64_t last, const std::vector<uint32_t>& gone, bool told) {
    return told && gen == last && stable && id && !std::binary_search(gone.begin(), gone.end(), id);
}

/// A recording thread's passes
struct Pass {
    uint64_t gen = 0;     // the destroy generation its looks were last moved to
    uint64_t count = 0;   // the record's count then
    bool ever = false;    // (no pass yet: nothing to move)
    uint64_t moved = 0, passes = 0;   // (counts, for the log)
};

/// Called with the destroy generation `gen` read before anything else of a look, on the thread that owns `each`'s looks.
/// When the generation moved since the thread's last pass: since(count, out, &now) appends the numbers of the textures whose
/// views went from count on and gives the record's count now (false: they cannot all be told), and every look that stands
/// takes `gen`. each(fn) calls fn(uint64_t& gen, uint32_t id, bool stable) for every look of the thread. Returns whether a
/// pass ran. (The order holds without a lock between the threads: a texture's number is recorded before the generation moves,
/// so a pass that sees the move reads the number; one whose views go after the record was read moves the generation again.)
template <class Each, class Since>
bool pass(Pass& p, uint64_t gen, Each each, Since since, std::vector<uint32_t>& gone) {
    if (p.ever && p.gen == gen) return false;
    gone.clear();
    uint64_t now = 0;
    const bool told = since(p.count, gone, &now) && p.ever;
    std::sort(gone.begin(), gone.end());
    const uint64_t last = p.gen;
    uint64_t moved = 0;
    each([&](uint64_t& g, uint32_t id, bool stable) {
        if (stands(g, id, stable, last, gone, told)) g = gen, moved++;
    });
    p.gen = gen, p.count = now, p.ever = true;
    p.moved += moved, p.passes++;
    return true;
}

}  // namespace view_keep
