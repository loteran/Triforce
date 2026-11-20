// (Triforce) What a texture's upload that did not happen leaves owed (vk_texture.cpp texture_touch), apart from Vulkan so
// that tests/test_upload_owed.py can run it.
//
// A texture is uploaded again when something says its storage changed: page write generations past its last upload's
// (Texture::upload_gen), virtual pages mapped anew (texture_dirty), a GPU buffer that wrote the storage (gpu_wrote_into).
// Each of those is noted as seen when it is asked, before the upload. And an upload can fail for want of memory: no staging
// arena could be made (the log's "vk: staging: no 64 MB arena could be made"), on a card or a PC short of it. Such a texture
// was marked uploaded all the same, at the generation of the try, so that the failing upload is not made again at every
// draw: its image held nothing (a new one: zeros, or for a block-compressed format whatever the memory held), or what it
// held before, until the game wrote other bytes into its memory. Since a capture's copy with the bytes of the page's last
// copy is no write to a texture (RX_VK_TEX_SAME, vk_memory.cpp), that can be never: the image stayed wrong until the texture
// was evicted, the area left or the game started again. (NX2navi had the same with its content hash, its commit 054bd556.)
//
// Now such an upload stays owed. The texture keeps its generation and its "seen" notes, what the try was to bring is kept
// here, and it is tried again some presents later, with whatever was written meanwhile.
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace upload_owed {

/// Byte ranges of a texture's storage, from its start (vk_texture.cpp's Ranges)
using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;

/// The presents a failed upload waits for its next try: 30 after the first failure (as a BC3 image's does, vk_texture.cpp),
/// twice as long after each failure that follows, 480 at most (8 s at 60 presents a second). A try is not free: what was
/// staged before the arena that could not be made was unswizzled or decoded for nothing, and each try asks the driver for
/// memory it may not have
inline constexpr uint64_t kWait = 30, kWaitMax = 480;
inline uint64_t wait(uint32_t failed) {
    uint64_t w = kWait;
    for (uint32_t i = 1; i < failed && w < kWaitMax; i++) w *= 2;
    return std::min(w, kWaitMax);
}

struct Owed {
    uint64_t from = 0;     // the present of the next try: none before it
    uint32_t failed = 0;   // the tries that failed in a row
    uint32_t later = 0;    // ((Triforce) the frames in a row its upload was left for the next one: later() below)
    bool whole = false;    // the whole storage is owed,
    Ranges ranges;         // or these ranges of it (in order, apart from each other)
    bool any() const { return whole || !ranges.empty(); }
};

/// `more` into `into`, which comes out in order, its ranges apart (touching and overlapping ones made one, empty ones dropped)
inline void add(Ranges& into, const Ranges& more) {
    if (&into != &more) into.insert(into.end(), more.begin(), more.end());
    std::sort(into.begin(), into.end());
    size_t n = 0;
    for (size_t i = 0; i < into.size(); i++) {
        if (into[i].second <= into[i].first) continue;
        if (n && into[i].first <= into[n - 1].second) into[n - 1].second = std::max(into[n - 1].second, into[i].second);
        else into[n++] = into[i];
    }
    into.resize(n);
}

/// An upload of `dirty` (null: the whole storage) did not happen at present `frame`: owed with what was owed before, and not
/// tried again before its wait is over (fixed_wait: that many presents each time, a BC3 image's 30; 0: the wait above)
inline void failed(Owed& o, const Ranges* dirty, uint64_t frame, uint64_t fixed_wait = 0) {
    o.failed++;
    o.from = frame + (fixed_wait ? fixed_wait : wait(o.failed));
    if (!dirty) o.whole = true, o.ranges.clear();
    else if (!o.whole) add(o.ranges, *dirty);
}

/// (Triforce) A texture uploaded before, whose new upload (of `dirty`, null: the whole storage) at present `frame` came after
/// that frame's uploads had taken their budget (vk_texture.cpp upload_again_later): owed to the next present, with what was
/// owed before. The image keeps the bytes of its last upload meanwhile. Not a failure: the waits above stay as they were
inline void later(Owed& o, const Ranges* dirty, uint64_t frame) {
    o.later++;
    o.from = frame + 1;
    if (!dirty) o.whole = true, o.ranges.clear();
    else if (!o.whole) add(o.ranges, *dirty);
}
/// Whether an upload may be left for the next frame once more: at most kLaterMax frames in a row, then it is made whatever the
/// frame's uploads took
inline constexpr uint32_t kLaterMax = 8;
inline bool may_wait(const Owed& o) { return o.later < kLaterMax; }
/// (Triforce, 11-16) Whether a texture held before (its image has bytes) may have its new upload left for the next frame once
/// more by the frame's budget (vk_texture.cpp upload_again_later): at most `most` frames in a row (RX_VK_UPLOAD_AGAIN_FRAMES,
/// 2: its last bytes drawn a frame or two at most), never more than kLaterMax
inline bool may_wait_again(const Owed& o, uint32_t most) { return o.later < std::min(most, kLaterMax); }

/// (Triforce, the hold: present_hold.h) A texture whose image has no bytes yet (not uploaded) may have its first upload left
/// for the next frame by the frame's budget too (vk_texture.cpp new_texture_later): not one whose upload found no memory (it
/// waits for that already), nor one left so kLaterMax frames in a row. Its draws sample the dummy meanwhile, and their frame
/// is held
inline bool may_leave_first(const Owed& o, bool uploaded) { return !uploaded && !o.failed && may_wait(o); }
/// Whether a texture's wait is for its first bytes, left by the budget (later above), not for memory
inline bool first_later(const Owed& o, bool uploaded) { return !uploaded && o.later && !o.failed; }

/// Whether the next try is still to be waited for at present `frame`
inline bool waits(const Owed& o, uint64_t frame) { return o.from > frame; }

/// The upload happened, or the GPU drew into the image (which is the content then): nothing is owed
inline void paid(Owed& o) { o = Owed{}; }

}   // namespace upload_owed
