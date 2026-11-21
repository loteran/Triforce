// (Triforce) Checkpoint saves of the pipeline cache during the shader preparation's long builds: the driver's part
// (vk_draw.cpp warm_libraries: about 91,600 stage libraries cold, two hours at 12-16 a second on an RX 7900 XT) and the
// precompile (precompile_run). The cache was written only when the part ended, or when it was stopped in order: a PC that
// restarted 57 minutes into the part (2026-11-18, v0.5.1's preparation) had nothing of it on the disk, AMD's own cache kept
// about 132 MB of it, and the preparation began again from nothing. A crash, a power cut, a Windows update's restart or a
// kill did the same.
//
// Now the watch loop saves the cache every RX_VK_WARM_SAVE_MIN minutes (10 unset; 0: only at the end, as before), once at
// least kMinBuilt were built since the last save (no save for nothing), through pipeline_cache_save's own preparation save,
// so that its rules hold: the carried session's cap (pcache_carry.h, its history kept open as a stopped one's), the file's
// bound (pcache_bound.h), the atomic write, .for and .base. And only when a copy of the data fits in the RAM available with
// 2 GB to spare (the save fetches it whole: 2 to 3 GB with the stage libraries); one that does not is asked again kRetryMs
// later. The workers go on building meanwhile (the cache is internally synchronized; the save runs on a thread of its own).
// A checkpoint does not say the part is made (the prepare's record, lib_check's and the journal are left as they are): the
// next start after a crash runs the part again, and the libraries the checkpoint saved come back as hits, about 0.2 ms each
// against about 60 ms compiled, so it goes on from the last checkpoint.
//
// Intel: not while the workers build (its driver crashed in vkGetPipelineCacheData while pipelines were built with the same
// cache: vk_draw.cpp pipeline_cache_save), unless RX_VK_PCACHE_PLAY_SAVE=1; RX_VK_PCACHE_PLAY_SAVE=0: on no card, as the
// save in play.
//
// The cache grows while a checkpoint fetches it (12-16 stage libraries a second, about 28 KB each), and data fetched into a
// buffer of the size asked just before is VK_INCOMPLETE once anything was added: nothing written. So a checkpoint fetches
// into room(n), the size asked with room for what is built meanwhile (the driver says what it wrote), asks again up to
// kFetchTries times, and the carried session's cap and the bound are looked at again on what was fetched. It is due only
// while the part moves (something built in the last kQuietMs: a part stuck in one build goes silent for Setup's silence
// watch, make_game.py PREPARE_SILENCE), and not with less free on the disk than the data and kDiskSpare (the .tmp is
// written beside the old file); a stop asked gives it up (the stop's own save writes the cache), and the file it writes is
// on the disk before it takes the old one's place (a power cut right after must not leave a damaged file). The rules
// alone, no Vulkan: tests/test_warm_checkpoint.py
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace warm_checkpoint {

inline constexpr unsigned kDefaultMin = 10;     // (RX_VK_WARM_SAVE_MIN unset)
inline constexpr unsigned kMaxMin = 24 * 60;    // (a larger number: a day)
inline constexpr size_t kMinBuilt = 1000;       // (stage libraries, or the precompile's pipelines, since the last save)
inline constexpr uint64_t kRetryMs = 60000;     // (a checkpoint that wrote nothing: asked again a minute later)
inline constexpr uint32_t kIntel = 0x8086;
inline constexpr uint64_t kQuietMs = 120000;    // (nothing built for this long: no checkpoint, as the timing line goes silent)
inline constexpr size_t kRoomMin = 64u << 20;   // (a fetch's room for what is built meanwhile: at least this, or n/32)
inline constexpr int kFetchTries = 3;           // (fetches of a checkpoint that came back VK_INCOMPLETE, the size asked again)
inline constexpr uint64_t kDiskSpare = 1ull << 30;   // (free on the disk beyond the data, for the .tmp beside the old file)
inline constexpr size_t kChunk = 64u << 20;     // (written in parts of this, a stop asked looked at between them)

/// RX_VK_WARM_SAVE_MIN: the minutes between checkpoints. Unset, empty or not a number: kDefaultMin; 0 or below: none (the
/// cache is saved at the end alone, as before)
inline unsigned minutes(const char* env) {
    if (!env || !*env) return kDefaultMin;
    char* end = nullptr;
    const long v = std::strtol(env, &end, 10);
    if (end == env) return kDefaultMin;
    if (v <= 0) return 0;
    return v > (long)kMaxMin ? kMaxMin : (unsigned)v;
}

/// Whether this card takes checkpoints while the workers build: RX_VK_PCACHE_PLAY_SAVE as pipeline_cache_save reads it (set:
/// not 0; unset: any card but Intel's)
inline bool card_ok(uint32_t vendor, const char* play_save) {
    if (play_save) return std::atoi(play_save) != 0;
    return vendor != kIntel;
}

/// Where the next checkpoint counts from: the last save (or the part's start), in ms of the part's clock, and what was built
/// by then
struct Clock {
    uint64_t from_ms = 0;
    size_t from_built = 0;
};

/// When the count last moved: look(now_ms, built) at every look of the watch loop
struct Moved {
    size_t built = 0;
    uint64_t at_ms = 0;
    void look(uint64_t now_ms, size_t b) {
        if (b != built) built = b, at_ms = now_ms;
    }
};

/// Whether a checkpoint is due at now_ms with built made so far: on (minutes), on this card, the minutes past since the last
/// save, at least kMinBuilt built since it, and something built in the last kQuietMs (moved_ms: when the count last moved)
inline bool due(unsigned minutes, bool card, const Clock& c, uint64_t now_ms, size_t built, uint64_t moved_ms) {
    if (!minutes || !card || now_ms < c.from_ms || built < c.from_built || now_ms < moved_ms) return false;
    return now_ms - c.from_ms >= (uint64_t)minutes * 60000 && built - c.from_built >= kMinBuilt && now_ms - moved_ms <= kQuietMs;
}

/// The buffer a checkpoint fetches n bytes of a growing cache into: n with room for what is built meanwhile
inline size_t room(size_t n) {
    const size_t more = n / 32 > kRoomMin ? n / 32 : kRoomMin;
    return n > SIZE_MAX - more ? SIZE_MAX : n + more;
}

/// Whether the disk has room for a checkpoint's n bytes beside the old file: free bytes free (known: free_known)
inline bool disk_fits(bool free_known, uint64_t free, uint64_t n) {
    return !free_known || (free >= kDiskSpare && free - kDiskSpare >= n);
}

/// The clock once a checkpoint begun at now_ms with built made wrote the cache
inline Clock saved(uint64_t now_ms, size_t built) { return Clock{now_ms, built}; }

/// The clock once a checkpoint begun at now_ms wrote nothing (the RAM short, the bound, the carry's cap, nothing new): asked
/// again kRetryMs later, what was built since the last save still counted
inline Clock skipped(const Clock& c, unsigned minutes, uint64_t now_ms) {
    const uint64_t period = (uint64_t)minutes * 60000, next = now_ms + kRetryMs;
    Clock n = c;
    n.from_ms = next >= period ? next - period : 0;
    return n;
}

}  // namespace warm_checkpoint
