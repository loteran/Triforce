// Which texture bindings of a translated shader are read by texel position or asked for their size (vk_shader.cpp
// make_module, for the texture detail of tex_detail.h): a texture bound there must be whole, as a fetch at a level the
// image lacks, or coordinates worked out from a size that is half the game's, read the wrong texels. A sampled read
// (normalised coordinates and derivatives) picks the same level of the game's texture wherever that level is kept.
// tests/test_spv_exact.py
//
// Counted: OpImageFetch, OpImageQuerySizeLod, OpImageQuerySize and OpImageQueryLevels on a sampled, single-sample 2D
// texture or 2D array, the only kinds the texture detail leaves levels out of. Not counted: a size query on an array
// whose result is used for its third component alone, the layer count, which no level changes.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace spv_exact {

using Bindings = std::array<uint64_t, 4>;   // (a bit a binding, 0 to 255; set 0 is the only set the translation uses)

inline bool any(const Bindings& b) { return (b[0] | b[1] | b[2] | b[3]) != 0; }
inline bool has(const Bindings& b, uint32_t binding) { return binding < 256 && (b[binding >> 6] >> (binding & 63) & 1); }

inline Bindings scan(const uint32_t* w, size_t n) {
    Bindings out{};
    if (n < 5 || w[0] != 0x07230203u) return out;
    enum : uint16_t {
        OpTypeImage = 25, OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypePointer = 32,
        OpVariable = 59, OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71,
        OpCompositeExtract = 81, OpCopyObject = 83, OpImageFetch = 95, OpImage = 100, OpImageQuerySizeLod = 103,
        OpImageQuerySize = 104, OpImageQueryLevels = 106,
    };
    struct Image { bool d2, arrayed, ms, sampled; };
    std::unordered_map<uint32_t, uint32_t> binding, inner, pointee, var_type, from;
    std::unordered_map<uint32_t, Image> images;
    struct Use { uint32_t image, result; bool size; };
    std::vector<Use> uses;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        if (!wc || i + wc > n) break;
        const uint32_t* a = w + i + 1;
        const uint32_t na = wc - 1;
        switch (op) {
        case OpDecorate:
            if (na >= 3 && a[1] == 33) binding[a[0]] = a[2];   // (Binding)
            break;
        case OpTypeImage:   // (result, sampled type, dim, depth, arrayed, ms, sampled, format)
            if (na >= 7) images[a[0]] = Image{a[2] == 1, a[4] != 0, a[5] != 0, a[6] == 1};
            break;
        case OpTypeSampledImage: case OpTypeArray: case OpTypeRuntimeArray:
            if (na >= 2) inner[a[0]] = a[1];
            break;
        case OpTypePointer:
            if (na >= 3) pointee[a[0]] = a[2];
            break;
        case OpVariable:
            if (na >= 2) var_type[a[1]] = a[0];
            break;
        case OpLoad: case OpAccessChain: case OpInBoundsAccessChain: case OpImage: case OpCopyObject:
            if (na >= 3) from[a[1]] = a[2];
            break;
        case OpImageFetch: case OpImageQueryLevels:
            if (na >= 3) uses.push_back({a[2], a[1], false});
            break;
        case OpImageQuerySizeLod: case OpImageQuerySize:
            if (na >= 3) uses.push_back({a[2], a[1], true});
            break;
        default: break;
        }
        i += wc;
    }
    if (uses.empty()) return out;
    // (a size query's result used for something other than its third component: every word of every other instruction
    // that equals the result's id counts, a literal that happens to as well: the texture then stays whole, the safe side)
    std::unordered_map<uint32_t, bool> other_use;
    for (const Use& u : uses)
        if (u.size) other_use[u.result] = false;
    if (!other_use.empty())
        for (size_t i = 5; i < n;) {
            const uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
            if (!wc || i + wc > n) break;
            const bool layer_only = op == OpCompositeExtract && wc == 5 && w[i + 4] == 2;
            for (uint32_t k = (op == OpImageQuerySizeLod || op == OpImageQuerySize) ? 3 : 1; k < wc; k++) {
                const auto it = other_use.find(w[i + k]);
                if (it != other_use.end() && !(layer_only && k == 3)) it->second = true;
            }
            i += wc;
        }
    for (const Use& u : uses) {
        uint32_t v = u.image;
        for (int hops = 0; hops < 16; hops++) {
            const auto f = from.find(v);
            if (f == from.end()) break;
            v = f->second;
        }
        const auto vt = var_type.find(v);
        const auto b = binding.find(v);
        if (vt == var_type.end() || b == binding.end() || b->second >= 256) continue;
        const auto p = pointee.find(vt->second);
        if (p == pointee.end()) continue;
        uint32_t t = p->second;
        for (int hops = 0; hops < 8; hops++) {
            const auto in = inner.find(t);
            if (in == inner.end()) break;
            t = in->second;
        }
        const auto im = images.find(t);
        if (im == images.end() || !im->second.d2 || im->second.ms || !im->second.sampled) continue;
        if (u.size && im->second.arrayed && !other_use[u.result]) continue;   // (the layer count alone)
        out[b->second >> 6] |= 1ull << (b->second & 63);
    }
    return out;
}

} // namespace spv_exact
