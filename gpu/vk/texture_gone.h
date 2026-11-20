// (hookshot) Which textures were destroyed, by their creation number (Texture::id), apart from Vulkan so that
// tests/test_texture_gone.py can run it.
//
// A texture pointer kept from one draw to a later one (vk_temporal.cpp: the scene's colour and depth from the scene's draws
// to the engine's pass, the upscale's output from one frame's dispatch to the next frame's tone mapping) has to be known
// to be the same texture still. texture_destroy_generation says only that some texture went since, anywhere: with the video
// memory full the cache lets textures go in most frames, and each of those frames then lost the engine's picture (the tone
// mapping was not drawn at the output's size, and the frame was the scene's size scaled up: a soft frame, some every second
// on an 8 GB card at 2560x1440). This says whether that one texture went.
#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace texture_gone {

/// The creation numbers of the textures destroyed last, and how many went in all. A texture noted while it was there (its
/// number, and count() then) is there still while its number is not among those that went since. When more went since than
/// are kept, or the number is none (0), it cannot be told: it counts as gone, which costs a frame what the old rule cost it
class Record {
public:
    static constexpr uint64_t kKept = 1024;

    void went(uint32_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        // (Triforce, 11-16: and by id, where it went in the count: gone_since is one look, not a walk of all that went since.
        // The handle cache asks it at every lookup after another texture's end; with hundreds going a frame (the Market's
        // crowd panned past) the walk was 44 ms of a slow frame's texture lookups. A slot overwritten forgets its id)
        const uint32_t old = ids_[n_ % kKept];
        if (old) {
            auto it = at_.find(old);
            if (it != at_.end() && it->second == n_ - kKept) at_.erase(it);
        }
        ids_[n_ % kKept] = id;
        if (id) at_[id] = n_;
        n_++;
    }

    uint64_t count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return n_;
    }

    bool gone_since(uint32_t id, uint64_t since) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (!id || since > n_ || n_ - since > kKept) return true;
        auto it = at_.find(id);
        return it != at_.end() && it->second >= since;
    }

    /// (Triforce) The numbers recorded from `since` on, in the order they went (appended to out), and how many went in all
    /// (*now, always). False, and out left as it was, when they cannot all be told: more went since than are kept, or since is
    /// past the count (view_keep.h)
    bool went_since(uint64_t since, std::vector<uint32_t>& out, uint64_t* now) const {
        std::lock_guard<std::mutex> lk(mu_);
        *now = n_;
        if (since > n_ || n_ - since > kKept) return false;
        for (uint64_t i = since; i < n_; i++) out.push_back(ids_[i % kKept]);
        return true;
    }

private:
    mutable std::mutex mu_;
    uint32_t ids_[kKept] = {};
    std::unordered_map<uint32_t, uint64_t> at_;   // (id -> its place in the count, the last kKept only)
    uint64_t n_ = 0;
};

}  // namespace texture_gone
