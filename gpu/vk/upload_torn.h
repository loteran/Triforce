// (Triforce, wp18-flash) A re-upload of texture storage the CPU was rewriting when its submit was captured (vk_texture.cpp
// torn_later, asked by upload_again_later; vk_memory.cpp memory_rewritten_since_capture), apart from Vulkan so that
// tests/test_upload_torn.py can run it.
//
// test29's tours (r36-t29-tour, r37-t29-tour) showed white frames at the Market after a tour: 310 and 332 cell blinks in the
// flicker reader's 60 s, eight frames with nearly every pixel changed, mean luma up to 214 against 33 to 56. Each one the log's
// "dark frame" lines name (twelve, six a run) is a frame whose replay uploaded a burst of the textures the game keeps rewriting
// from the CPU there (BC6H 1528 x 920 x 3 and 1120 x 1516 x 3, BC7 of the same sizes, BC5 2240 x 3032: 5 to 28 uploads, 17 to 78
// MB), and in each the frame after it uploaded some of them again: the CPU's write of them was not done at that frame's submit.
// The uploads read texture storage as it was at the submit (vk_memory.cpp, "texture storage in the submit-time capture"), and in
// test29 the CPU was still writing those pages while the replay read them: the log's "texture storage read as submitted" lines
// count the pages whose memory differed from that copy at the read, 150,000 to 250,000 in those tours against 0 in test27's, 662
// in test30's and 0 to 9,415 in test31's. A copy taken while the game moves or refills a texture holds part of the bytes passing
// through (half of those pages "changed" between two captures, wp18-storm's notes), and BC6H bytes that are not the texture's
// own decode to large finite colours, which the colour outputs' NaN guard lets through and the bloom spreads over the whole
// picture (docs/design.md section 10: the white flash). The frame after it took the finished bytes, and was right.
//
// Whether such a copy is taken depends on when the game's threads write against the submits: test30 and test31 showed none of it
// in the same storm (r37-t30-tour, r39-p0-tour-1 with the old budget), test29 showed it in both of its tours. So the rule here
// does not wait for the timing to be right: a texture uploaded before, of kMinBytes or more, whose storage the CPU has rewritten
// since the copy the upload would read (some whole page of it differs from that copy now), keeps its image's last bytes for this
// frame, and is uploaded at the next frame with what that frame's capture took (upload_owed::later, as a re-upload the frame's
// budget leaves: vk_texture.cpp upload_again_later). At most kFrames frames in a row, and not again for a texture found so in the
// kGap frames before: one the CPU rewrites at every frame (a movie, an animation) is uploaded as before after its first such
// frame, so its pictures are a frame late once, not half of them dropped. RX_VK_UPLOAD_TORN=0: uploaded at once, as before.
//
// The pages compared: every kStride-th whole page of the storage and its last whole one (a page the texture shares with other
// data at its start or its end is not its own: whatever the CPU writes there is no sign of a refill), each only where a capture
// holds a copy of it (a page no capture took is read from guest memory by the upload anyway).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace upload_torn {

inline constexpr uint64_t kMinBytes = 1ull << 20;   // (the storage at least: the white frames' textures were 4 to 13 MB)
inline constexpr uint64_t kPage = 4096;             // (a capture's copies are of 4 KB pages)
inline constexpr uint64_t kStride = 16;             // (every 16th whole page compared, and the last: a refill writes megabytes)
inline constexpr uint32_t kFrames = 1;              // (frames in a row a re-upload is left so)
inline constexpr uint64_t kGap = 2;                 // (presents: a texture found so this recently is uploaded as before)

/// RX_VK_UPLOAD_TORN as the environment has it: on unless "0"
inline bool on(const char* env) {
    return !env || !*env || std::strtol(env, nullptr, 10) != 0;
}

/// Whether a re-upload is asked about at all: the rule on, a texture in the guest's own memory (below 1 TB: the captures' pages;
/// a virtual pool's storage is not captured), kMinBytes or more
inline bool wanted(bool on, uint64_t gpu, uint64_t bytes) {
    return on && gpu < (1ull << 40) && bytes >= kMinBytes;
}

/// The whole pages of [guest, guest + n): [*first, *end) in page numbers (a page shared at either edge left out)
inline void whole_pages(uint64_t guest, uint64_t n, uint64_t* first, uint64_t* end) {
    *first = (guest + kPage - 1) / kPage;
    *end = (guest + n) / kPage;
    if (*end < *first) *end = *first;
}

/// Whether the CPU rewrote the storage since the copies the upload would read: some page of [first, end) compared (every kStride-th
/// and the last) whose copy (copy(page): a capture's 4 KB, null where none holds it) differs from guest memory now (live(page))
template <class Copy, class Live>
bool rewritten(uint64_t first, uint64_t end, Copy copy, Live live) {
    if (end <= first) return false;
    for (uint64_t p = first;; p += kStride) {
        if (p >= end - 1) p = end - 1;   // (the last whole page, always)
        if (const uint8_t* c = copy(p); c && std::memcmp(c, live(p), kPage) != 0) return true;
        if (p == end - 1) return false;
    }
}

/// The textures found so lately, by their creation number (Texture::id): a slot each, the newest wins it
class Recent {
public:
    static constexpr uint32_t kSlotBits = 12, kSlots = 1u << kSlotBits;
    /// Whether a texture found so at present `frame` may be left for the next frame: not found so in the kGap presents before
    bool may(uint32_t id, uint64_t frame) const {
        const Slot& s = slot_[at(id)];
        return !(s.used && s.id == id && frame >= s.frame && frame - s.frame <= kGap);
    }
    /// It was found so at present `frame` (left or not)
    void seen(uint32_t id, uint64_t frame) {
        Slot& s = slot_[at(id)];
        s.id = id, s.frame = frame, s.used = true;
    }

private:
    struct Slot {
        uint32_t id = 0;
        uint64_t frame = 0;
        bool used = false;
    };
    static uint32_t at(uint32_t id) { return (uint32_t)(((uint64_t)id * 0x9e3779b97f4a7c15ull) >> (64 - kSlotBits)); }
    Slot slot_[kSlots];
};

}  // namespace upload_torn
