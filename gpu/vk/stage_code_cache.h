/* (Triforce) The stages' decoded code kept by their bytes, for the program picks (vk_shader.cpp read_code, codes_of).

   A program pick that meets a program object (its serial) for the first time reads each of its stages from guest memory, the
   control section and the code area, takes their code key (FNV-1a over both) and decodes the code: its Ampere instructions
   captured and lowered to Maxwell (parse_code). All of it under the shaders' lock: the stall line of the save's load-in frame
   said "the program picks read the stages' code 45.6 ms" (lab run r33-t28-s1-1 frame 14966; 37 to 67 ms in the lab and the
   owner's sessions, 37 ms at the Market's arrival). Measured over the game's own shaders (triforce_shaders.bin, a stage 12.2 KB
   on average, every 20th): the read 1.5 us a stage, the key 9.3 us, the decoding 48.3 us. The game makes program objects again
   for code it has had before (another serial for the same stages, at the same address or another), and each was decoded again.

   This cache keeps what was made of a stage's bytes: a pick that reads bytes met before takes a copy of it, and neither keys
   nor decodes them again. The lookup is by a quick hash of the bytes (quick_hash, four lanes of 64-bit words: 1.2 us a stage),
   and an entry is taken only when its bytes are the same, byte for byte (0.2 us): two stages whose quick hashes are the same
   cost a decoding, never one's code for the other's. What is made of the bytes is made of them alone (and of the switches,
   read once, and the device), so the copy is what decoding them again would give.

   Bounded: the entries' bytes and what was made of them, budget bytes at most (RX_VK_STAGE_CODE_CACHE=<MB>, 64: some 3000 stages
   decoded), the least recently used going first. RX_VK_STAGE_CODE_CACHE=0: nothing kept, each stage decoded again for each
   program object, as before. Its own lock, taken alone or under the shaders' lock, nothing taken under it. Plain code, what is
   kept a template parameter: tests/test_stage_code_cache.py runs it.

   And the lowering only where a pick translates (RX_VK_STAGE_CODE_LAZY, vk_shader.cpp stage_head, codes_whole). A pick needs a
   stage's key, NVN stage and header to find its program, the lowered code only to translate one, and the shaders are prepared
   before the title: the save's load-in frame finds its programs in the program cache, read ahead or read there. So a pick reads
   a stage's head (the key: 9.3 us, of the 60) and the cache keeps that; the code is lowered (and kept whole) when the pick
   translates a variant. A stage that does not lower leaves the program without it, as before: no program in any cache was made
   from such a stage (its key names its stages: a translation lowers each first, and so did the prepare), so a pick that finds
   its program needs no lowering, and one that translates lowers the stages first and, should one not lower, picks again from
   those that do. tests/test_stage_code_lazy.py runs the picks with the switch on and off.

   And read with the shaders' lock let go (RX_VK_STAGE_CODE_UNLOCKED, vk_shader.cpp codes_read): what a pick reads of a program
   object met for the first time (guest memory, the key, this cache) needs nothing the lock keeps, so the pick lets it go
   meanwhile as a draw's own read of a program does (program_read_once.h: a program's release waits until no pick has the lock
   let go), and the replay's other threads go on picking. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mkwvk::stage_code_cache {

constexpr size_t kDefaultMb = 64;

/// RX_VK_STAGE_CODE_CACHE: the budget in bytes (MB given, kDefaultMb unset or unreadable, at most 4096 MB); 0: none
inline size_t budget(const char* env) {
    size_t mb = kDefaultMb;
    if (env && *env) {
        char* end = nullptr;
        const long long v = std::strtoll(env, &end, 10);
        if (end != env && !*end && v >= 0) mb = (size_t)(v < 4096 ? v : 4096);
    }
    return mb << 20;
}

/// RX_VK_STAGE_CODE_LAZY: on unless "0"
inline bool lazy_on(const char* env) { return !(env && env[0] == '0' && !env[1]); }
/// RX_VK_STAGE_CODE_UNLOCKED: on unless "0"
inline bool unlocked_on(const char* env) { return !(env && env[0] == '0' && !env[1]); }

/// The cache's lookup hash of bytes: four lanes of 64-bit words, the tail and the length folded in. Only a lookup's: an entry is
/// taken for bytes equal to its own
inline uint64_t quick_hash(const uint8_t* p, size_t n) {
    constexpr uint64_t m = 0x9E3779B97F4A7C15ull;
    uint64_t lane[4] = {0x243F6A8885A308D3ull, 0x13198A2E03707344ull, 0xA4093822299F31D0ull, 0x082EFA98EC4E6C89ull};
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int k = 0; k < 4; k++) {
            uint64_t w;
            std::memcpy(&w, p + i + 8 * k, 8);
            lane[k] = (lane[k] ^ w) * m;
            lane[k] ^= lane[k] >> 29;
        }
    uint64_t h = 0xCBF29CE484222325ull ^ n;
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    for (int k = 0; k < 4; k++) {
        h = (h ^ lane[k]) * m;
        h ^= h >> 31;
    }
    return h;
}

/// What was made of stages' bytes (Value), by the bytes (Hash: the lookup's, quick_hash; another only in the tests)
template <class Value, uint64_t (*Hash)(const uint8_t*, size_t) = quick_hash> class Cache {
public:
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

    explicit Cache(size_t budget) : budget_(budget) {}

    size_t budget() const { return budget_; }

    /// What was made of bytes equal to these (now the most recently used), or null
    std::shared_ptr<const Value> find(const std::vector<uint8_t>& bytes) {
        if (!budget_) return nullptr;
        const uint64_t h = Hash(bytes.data(), bytes.size());
        std::lock_guard lk(mu_);
        const auto range = by_hash_.equal_range(h);
        for (auto it = range.first; it != range.second; ++it)
            if (*it->second->bytes == bytes) {
                lru_.splice(lru_.begin(), lru_, it->second);
                return it->second->value;
            }
        return nullptr;
    }

    /// value, made of bytes, kept (in place of what was kept for bytes equal to them); cost: its own size, besides the bytes'.
    /// The least recently used go while the whole is over the budget (this one too, were it alone over it)
    void put(const Bytes& bytes, std::shared_ptr<const Value> value, size_t cost) {
        if (!budget_ || !bytes || !value) return;
        const uint64_t h = Hash(bytes->data(), bytes->size());
        std::lock_guard lk(mu_);
        const auto range = by_hash_.equal_range(h);
        auto at = lru_.end();
        for (auto it = range.first; it != range.second; ++it)
            if (*it->second->bytes == *bytes) at = it->second;
        if (at != lru_.end()) {
            used_ -= at->cost;
            at->value = std::move(value), at->cost = bytes->size() + cost;
            used_ += at->cost;
            lru_.splice(lru_.begin(), lru_, at);
        } else {
            lru_.push_front(Entry{bytes, h, std::move(value), bytes->size() + cost});
            by_hash_.emplace(h, lru_.begin());
            used_ += lru_.front().cost;
        }
        while (used_ > budget_ && !lru_.empty()) {
            const auto last = std::prev(lru_.end());
            const auto range2 = by_hash_.equal_range(last->hash);
            for (auto it = range2.first; it != range2.second; ++it)
                if (it->second == last) {
                    by_hash_.erase(it);
                    break;
                }
            used_ -= last->cost;
            lru_.erase(last);
        }
    }

    /// Entries kept and their bytes in all (for the log)
    void usage(size_t* entries, size_t* bytes) const {
        std::lock_guard lk(mu_);
        *entries = lru_.size(), *bytes = used_;
    }

private:
    struct Entry {
        Bytes bytes;
        uint64_t hash;
        std::shared_ptr<const Value> value;
        size_t cost;   // (the bytes' and the value's)
    };
    const size_t budget_;
    mutable std::mutex mu_;
    std::list<Entry> lru_;   // (the most recently used first)
    std::unordered_multimap<uint64_t, typename std::list<Entry>::iterator> by_hash_;
    size_t used_ = 0;
};

} // namespace mkwvk::stage_code_cache
