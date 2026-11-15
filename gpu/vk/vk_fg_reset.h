// Camera cuts for AMD FSR 3.1 frame generation through AMD's library (vk_ffx_fg.cpp), ported from NX2navi's runtime/vk_fg_reset.h.
// NVIDIA DLSS starts a new history on a cut, another scene size or another target, and vk_fsr3_inputs.h hands that out with the
// frame's depth and motion. AMD's frame generation takes it only in the generation callback (ffxDispatchDescFrameGeneration::reset),
// which its swapchain calls later, on its own present thread, for the frame id the configuration gave. So each frame's cut is noted
// here under its id when the frame is configured, and the callback looks it up. Without it every generated frame of a cut was blended
// from two unrelated pictures: an area load or a cutscene's cut flashed a frame of both.
//
// A ring of kSlots frames; each slot holds the frame id and its cut as one atomic word, so a slot being rewritten never pairs an old
// id with a new frame's answer. An id the ring no longer holds (the callback fell that far behind, or the slot was never written)
// counts as a cut: a lost history costs one plain frame, a wrong one blends two.
// Own header (no Vulkan, no library): gpu/vk/tests/fg_reset_test.cpp checks it.
#pragma once

#include <atomic>
#include <cstdint>

namespace mkwvk::fg_reset {

constexpr int kSlots = 32;
/// Frame ids above this do not fit beside the cut bit; they always count as a cut
constexpr uint64_t kMaxId = UINT64_MAX >> 1;

struct Ring {
    std::atomic<uint64_t> slot[kSlots]{};   // (frame id << 1) | cut; 0 never matches (ids start at 1)
};

/// Frame `id` is a cut (no usable history before it) or not. Ids 0 and past kMaxId are not kept
inline void note(Ring& r, uint64_t id, bool cut) {
    if (id && id <= kMaxId) r.slot[id % kSlots].store(id << 1 | (cut ? 1u : 0u), std::memory_order_release);
}

/// Whether frame `id` is a cut: noted as one, never noted, or noted so long ago that its slot holds another frame
inline bool is_cut(const Ring& r, uint64_t id) {
    if (!id || id > kMaxId) return true;
    const uint64_t v = r.slot[id % kSlots].load(std::memory_order_acquire);
    return (v >> 1) != id || (v & 1);
}

}  // namespace mkwvk::fg_reset
