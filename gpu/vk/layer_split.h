/* (Triforce) A new layered ASTC texture's first upload spread over a few frames, some of its layers each (vk_texture.cpp
   texture_touch, split_take). The GPU decodes an ASTC texture (and encodes it again as BC3 or BC1 where the renderer keeps it
   so) in the upload command buffer of the recording that first draws it. In the owner's test7 session one such texture,
   1365 x 1365 x 90 layers with 11 levels (57 MB of ASTC, some 890 MB of texels), was the upload of an area change's frame
   ("slow upload 35.0 ms ... ASTC (BC3)"), that frame's upload command buffer took the GPU 114.8 ms, and the next frame's
   capture waited 117.2 ms for it. The frame's upload budget (new_texture_later) counts bytes of storage, and always lets a
   frame's first upload through: one texture alone made the backlog. Now such a texture, while the hold of the last picture may
   leave textures for later, is uploaded a part of its layers a frame: its draws sample the dummy and its frames are held until
   the last part is in, as for a new texture left whole for the next frame. At the hold's bound, or for a dispatch, what is left
   goes at once. Plain functions: tests/test_layer_split.cpp checks them. RX_VK_UPLOAD_SPLIT_MB=<MB of texels a frame> (128),
   0: whole, as before. */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

namespace mkwvk::layer_split {

using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;

constexpr uint32_t kMaxParts = 6;   // (frames at most: upload_owed::kLaterMax is 8, and a part is a frame)

/// RX_VK_UPLOAD_SPLIT_MB: the texels' MB a part may decode, 0: no split (unset or no number: 128)
inline uint64_t budget(const char* env) {
    if (!env || !*env) return 128ull << 20;
    char* end = nullptr;
    const double v = std::strtod(env, &end);
    if (end == env || *end || v < 0) return 128ull << 20;
    return (uint64_t)(v * 1048576.0);
}

/// The texels an ASTC texture decodes to (RGBA8, every level and layer)
inline uint64_t texel_bytes(uint32_t w, uint32_t h, uint32_t levels, uint32_t layers) {
    uint64_t n = 0;
    for (uint32_t l = 0; l < std::max(1u, levels); l++) n += (uint64_t)std::max(1u, w >> l) * std::max(1u, h >> l) * 4;
    return n * std::max(1u, layers);
}

/// The parts a texture of these texels and layers is uploaded in (1: whole)
inline uint32_t parts(uint64_t texels, uint32_t layers, uint64_t per_part) {
    if (!per_part || layers < 2 || texels <= per_part) return 1;
    const uint64_t p = (texels + per_part - 1) / per_part;
    return (uint32_t)std::min<uint64_t>({p, (uint64_t)kMaxParts, (uint64_t)layers});
}

/// The layers of `remaining` (ranges of the storage, whole layers of layer_size bytes, in order) split into this frame's part,
/// the first `per_frame` layers of them, and the rest
inline void take(const Ranges& remaining, uint64_t layer_size, uint32_t per_frame, Ranges& part, Ranges& rest) {
    part.clear(), rest.clear();
    if (!layer_size) return;
    uint64_t left = (uint64_t)std::max(1u, per_frame) * layer_size;
    for (const auto& [lo, hi] : remaining) {
        if (hi <= lo) continue;
        if (!left) { rest.push_back({lo, hi}); continue; }
        const uint64_t n = std::min(left, hi - lo);
        part.push_back({lo, lo + n});
        left -= n;
        if (lo + n < hi) rest.push_back({lo + n, hi});
    }
}

/// The layers a part takes: the texture's layers over its parts, rounded up
inline uint32_t per_frame(uint32_t layers, uint32_t n_parts) {
    return n_parts ? (layers + n_parts - 1) / n_parts : layers;
}

} // namespace mkwvk::layer_split
