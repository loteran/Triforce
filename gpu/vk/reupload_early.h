// (Triforce, 11-16) The re-uploads made before a parallel replay's segments (vk_texture.cpp texture_reupload_early, called by
// vk_replay.cpp replay_handle after the dry pass), apart from Vulkan so that tests/test_reupload_early.py can run it.
//
// After some tours of the lab's scenario S2 the game kept rewriting, from the CPU, the storage of textures the Market samples
// (4 to 12 re-uploads of 28 to 84 MB in ~13% of the frames: frames of 18-26 ms against 12.5). Each re-upload ran in
// texture_touch, which a segment's draw calls under the parallel replay's lock (vk_draw.cpp texture_view_of): the other
// recording threads queued behind it, and the frame's descriptor time grew from 3 to 24 ms over the threads ("the recording
// lock waited for 3.9 ms" a frame there, against ~0.1 in the steady Market).
//
// So the textures the CPU keeps rewriting are looked at once more before the segments begin, on the replay thread, the lock
// not taken by anyone yet: those that were uploaded again from the CPU's writes twice or more within kWindow presents, the
// last time within kWindow, and sampled in this frame or the one before (Hot below). What the look uploads goes into the
// uploads of the recording before the segments, which the queue runs before every segment, as a serial replay's go before
// its recording's draws; the segments' own looks find them looked at in this submission and uploaded by an earlier part of
// it, and go on without the lock (touch_quick.h). The look is texture_touch itself, so what it decides is what a segment's
// first look at the texture would decide, with two differences only:
// - it comes before the segments' recording. What the dry pass found this command buffer writes into a texture (a draw's
//   targets, a dispatch's storage images, copies and clears) is marked as the GPU's before (texture_written), and such a
//   texture is not looked at here (wanted: gpu_written). What the dry pass cannot see (a storage image a draw's program
//   writes, a storage buffer over texture storage) is as in the parallel replay now, whose segments look in any order;
// - a texture not sampled in this command buffer after all is looked at (and uploaded) once in this submission, as its next
//   sampling would have done with the bytes of this submission's capture, the newest there are.
// RX_VK_REUPLOAD_EARLY=0: not, the segments' looks alone, as before.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace reupload_early {

inline constexpr uint64_t kMinBytes = 1ull << 20;   // (the bursts' textures were 4 to 34 MB; a small one holds the lock briefly)
inline constexpr uint64_t kWindow = 120;            // (presents: two re-uploads within it make a texture hot)
inline constexpr uint32_t kHotAfter = 2;            // (re-uploads in a row, each within kWindow of the one before)
inline constexpr size_t kMost = 64;                 // (textures kept: the bursts named 5 to 12)

/// What the list knows of a texture: its creation number (Texture::id) and the textures-gone count when it was last known to
/// be there (texture_gone.h: its pointer is good while its number is not among those gone since), its re-uploads in a row
struct Entry {
    void* tex = nullptr;
    uint32_t id = 0;
    uint64_t since = 0;
    uint64_t last = 0;   // (the present of its last re-upload)
    uint32_t n = 0;      // (re-uploads in a row, each within kWindow presents of the one before)
};

/// A texture as the early look asks about it (vk_texture.cpp fills this from Texture)
struct Seen {
    bool uploaded = false, gpu_written = false, released = false, sparse = false, only_cleared = false;
    uint64_t bytes = 0;        // (its storage)
    uint64_t last_frame = 0;   // (the present a draw last looked at it)
    uint32_t owed_later = 0;   // (frames its upload was left for the next one: owed first)
};

/// Whether a hot texture is looked at before the segments at present `frame`: one uploaded before that the GPU did not draw
/// into (nor marked as drawn into by the dry pass of this command buffer), not finalized, not sparse, not one the GPU only
/// cleared (its letters are the CPU's over the GPU's: texture_touch's own branch), large enough, sampled this frame or the
/// one before
constexpr bool wanted(const Seen& t, uint64_t frame) {
    return t.uploaded && !t.gpu_written && !t.released && !t.sparse && !t.only_cleared && t.bytes >= kMinBytes &&
           t.last_frame + 1 >= frame;
}

class List {
public:
    /// A re-upload of a texture from the CPU's writes at present `frame` (since: the textures-gone count now)
    void note(void* tex, uint32_t id, uint64_t frame, uint64_t since) {
        for (Entry& e : e_)
            if (e.tex == tex && e.id == id) {
                e.n = frame - e.last <= kWindow ? e.n + 1 : 1;
                e.last = frame, e.since = since;
                return;
            }
        if (e_.size() >= kMost) {   // (the one re-uploaded longest ago makes room)
            auto old = std::min_element(e_.begin(), e_.end(), [](const Entry& a, const Entry& b) { return a.last < b.last; });
            *old = Entry{tex, id, since, frame, 1};
            return;
        }
        e_.push_back(Entry{tex, id, since, frame, 1});
    }
    /// The entries hot at present `frame`, in the order they are looked at; those cold for kWindow presents dropped
    std::vector<Entry*> hot(uint64_t frame) {
        std::erase_if(e_, [frame](const Entry& e) { return frame > e.last + kWindow; });
        std::vector<Entry*> out;
        for (Entry& e : e_)
            if (e.n >= kHotAfter) out.push_back(&e);
        return out;
    }
    /// A texture gone (or not one any more): out of the list (the entries hot() handed out are not good after this)
    void forget(const void* tex, uint32_t id) {
        std::erase_if(e_, [tex, id](const Entry& x) { return x.tex == tex && x.id == id; });
    }
    size_t size() const { return e_.size(); }

private:
    std::vector<Entry> e_;
};

/// The order of the early looks: uploads left for this frame by the budget first (they are a frame late already), then the
/// textures re-uploaded most recently
inline void order(std::vector<std::pair<Entry*, Seen>>& v) {
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        if ((a.second.owed_later > 0) != (b.second.owed_later > 0)) return a.second.owed_later > 0;
        return a.first->last > b.first->last;
    });
}

}  // namespace reupload_early
