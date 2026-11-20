// (Triforce) A texture's lock-free look in a parallel replay (vk_texture.cpp texture_touch_quick): whether its contents are
// up to date for this submission and this segment, told without the recording lock from what another thread may be
// writing under it. tests/test_parallel_fixes.py (tests/parallel_touch_quick_test.cpp)
//
// The locked look (texture_touch) notes that it looked (checked_epoch = this submission's), uploads when the game wrote the
// texture's memory, and only then notes which submission and segment uploaded it (upload_value, upload_seg). A worker of an
// earlier segment that looked without the lock in between saw "looked at, uploaded" and went on: its draws sampled the
// texture as it was, though the upload it should have made again in its own segment's uploads (the later segment's comes
// after it in the queue) was under way. So the locked look says so first (touching, set before anything else of it, with a
// fence after it, and cleared after everything), and the lock-free one reads, in this order: uploaded and checked_epoch, then touching, then
// upload_value and upload_seg. While a locked look is under way it is not up to date (the caller takes the lock, waits for
// that look's end, and sees what it left); once touching reads zero after checked_epoch read as this submission's, the
// look that set it is over and what it uploaded is noted.
#pragma once
#include <cstdint>

namespace touch_quick {

/// What the lock-free look read of a texture, each field in the order it stands here
struct Seen {
    bool uploaded;
    uint32_t checked_epoch;
    uint32_t touching;        // (not zero: a locked look of it is under way on some thread)
    uint64_t upload_value;    // (the submission its last upload was recorded in)
    uint64_t upload_seg;      // (and the segment)
};

/// Whether the texture needs nothing more for a draw of segment `seg` of submission `rec_value` that samples it
constexpr bool up_to_date(const Seen& t, uint32_t epoch, uint64_t rec_value, uint64_t seg) {
    if (!t.uploaded || t.checked_epoch != epoch) return false;
    if (t.touching) return false;
    return !(t.upload_value == rec_value && t.upload_seg > seg);   // (uploaded by a later segment: again, in this one's)
}

} // namespace touch_quick
