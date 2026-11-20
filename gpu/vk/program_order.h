/* (Triforce) The programs read ahead in the order the last sessions first needed them (vk_shader.cpp program_cache_load,
   readahead_main; the file triforce_program_order.bin beside the program cache).

   The save's load-in frame is 250 to 330 ms in every lab run that loads saves\castle-town, 220 to 436 ms in the owner's test29
   sessions, and of it the draws read 96 to 108 programs from the program cache themselves ("96 programs read from the cache by
   the draws (73.4 ms), 202 taken read ahead"; the dispatches' program picks, 28 to 42 ms, are mostly compute programs read the
   same way). The read-ahead (program_readahead.h) reads after each program a draw meets, in the pipeline notes' order: the
   order the notes' pipelines were first built, by whichever session built them, which is not the order a load meets them in.
   It begins with the load's first program, at most 32 ahead of the draws, and its shelf lets its oldest go when full (888 of
   1024 programs read ahead let go unused by the owner's load), so a load's first frame outruns it.

   So each session keeps the programs its draws needed from the cache, or found nowhere and translated, in the order it first
   needed them (Record: the first kKeysMax), and writes them beside the cache now and then (this session's first, then those of
   the sessions before it has not met: merge). The next session's read-ahead workers, from the first draw on (the title and the
   file select take seconds), read those programs in that order whenever no draw asks them for anything, and keep them made
   (Ahead) apart from the shelf, which does not let them go: at most kKeptMax at a time, each until a draw takes it or the draws
   have gone kBehind places past it in the order (that session went elsewhere). Where the draws are is the middle place of the
   last kRecent programs they needed for the first time, not the furthest: one program met out of the order's turn (a session
   that first drew it later) does not let go the load's programs. The asks come first, as before. Nothing changes what a draw
   gets: the same records, read the same way, taken as a program read ahead is taken.

   Memory: a program read ahead keeps its stages' SPIR-V packed (spv_store.h, about 24 KB a stage) and no module: kKeptMax
   programs are some 25 to 50 MB, beside the shelf's 512. The warm of their pipelines (pipe_warm.h) keeps its own bounds, made
   for 512 programs ahead of the draws (kLinksMax, kLibsMax): kKeptMax is the same, so that what it makes for the load is not
   let go before the load comes.

   ((Triforce, wp19) Kept until needed. The owner's test37 session took another path than the order's sessions (SDR where
   they were HDR Native with RenoDX: other programs at the file select, some of them the order has far later), and its
   load-in frame (8064, 318 ms) still read 115 programs on the draws' threads: "of the programs the draws read, 64 read ahead
   and let go before, 51 not read ahead yet". Programs met out of the order's turn dragged where the draws are past the load's
   places, so the load's programs read ahead went as "behind" and those not read yet were passed by; and the 512 kept filled
   with programs that session never needed, so the reading stopped. tests/test_program_order.cpp has such a session: the rules
   before drop 64 of its load's 115 and leave 51 unread. Now (keep_all): a program read in this order stays until a draw takes
   it, whatever the draws do meanwhile; nothing is passed by; the bound is the whole order (kKeepAllMax, the file's 4096), so
   the workers read all of it while the title and the file select stand still (seconds; a read is about a millisecond on each
   of four workers); a program an ask of the draws read that this order names is kept with these, not on the shelf, which lets
   its oldest go (this order passed it by while it was parked there); and only a RAM gone short lets them go (all of them:
   vk_shader.cpp). Where the draws are decides nothing then of what is read or kept (only the warm's count, below), so no path
   through the order can drag it. Memory: a kept program is some 40 to 50 KB packed (its stages and their Shader::Info): the
   2411 of test37's order some 100 to 120 MB, the 4096 at most some 200 MB, on a 32 GB PC; none read while the RAM is low, all
   given back when it is short.
   The pipelines' warm (pipe_warm.h, pipe_order.h) makes a program's pipelines once, as it is read, within a window of its own
   ahead of the draws: a program read far ahead would have none made. So the first kKeptMax kept and not taken are read with the
   warm, as before; those beyond are read cold (no warm), and as the draws take warmed ones the nearest cold one is warmed then
   (next_warm), in hand as a read is, so that the warm keeps the window it had.
   (Triforce: a warmed program the draws never take held its room in that window for the whole session: an order recorded in
   HDR replayed in SDR leaves some 300 of its first 512 never drawn, and the window shrank to the rest. So where the draws are
   still moves on as by the rules before, and a kept program kBehind behind it is passed (Ahead::pass): kept, a draw may still
   take it, but out of the window's count and not warmed. The window refills ahead of the draws. And the cold read ahead are
   kept by place beside their set (cold_at), so that the nearest is found without a look at each under the read-ahead's lock,
   which the draws take.)
   RX_VK_PROGRAM_ORDER_KEEP=0: the rules before (512, where the draws are, kBehind).

   Plain functions: tests/test_program_order.cpp checks them. RX_VK_PROGRAM_ORDER=0: neither read nor written, as before. */
#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mkwvk::program_order {

constexpr size_t kKeysMax = 4096;      // (programs a session's order keeps, and the file)
constexpr size_t kKeptMax = 512;       // (programs read ahead in this order and not taken yet, at most: the shelf's and the warm's)
constexpr size_t kKeepAllMax = kKeysMax;   // ((Triforce, wp19) the same, kept until needed: the whole order)
constexpr int64_t kBehind = 256;       // (one kept this many places behind where the draws are goes, and none is read there)
constexpr size_t kRecent = 16;         // (where the draws are: the middle place of the last this many they needed first)
constexpr int64_t kSaveEveryMs = 3000; // (the file written at most this often, when the session's order grew)
constexpr uint32_t kMagic = 0x4f505446;   // ("FTPO")
constexpr uint32_t kVersion = 1;

/// RX_VK_PROGRAM_ORDER: on unless "0" ((Triforce, wp19) and RX_VK_PROGRAM_ORDER_KEEP, Ahead::keep_all, the same way)
inline bool on(const char* env) { return !(env && env[0] == '0' && !env[1]); }

/// A check word of the keys (FNV-1a of their bytes)
inline uint64_t check(const std::vector<uint64_t>& keys) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const uint64_t k : keys)
        for (int i = 0; i < 8; i++) h = (h ^ ((k >> (8 * i)) & 0xFF)) * 1099511628211ull;
    return h;
}

/// The file's bytes: magic, version, count (4 bytes each, and 4 spare), the keys, their check word. ((Triforce, wp18) magic:
/// another file of the same form, pipe_order.h's)
inline std::vector<uint8_t> write(const std::vector<uint64_t>& keys, uint32_t magic = kMagic) {
    std::vector<uint8_t> out(16 + keys.size() * 8 + 8);
    const uint32_t head[4] = {magic, kVersion, (uint32_t)keys.size(), 0};
    std::memcpy(out.data(), head, 16);
    if (!keys.empty()) std::memcpy(out.data() + 16, keys.data(), keys.size() * 8);
    const uint64_t c = check(keys);
    std::memcpy(out.data() + 16 + keys.size() * 8, &c, 8);
    return out;
}

/// The keys of a file's bytes; none when they are not a whole file of this version (cut short, another file, a bad check word)
inline std::vector<uint64_t> read(const uint8_t* data, size_t n, uint32_t magic = kMagic) {
    uint32_t head[4];
    if (!data || n < 24) return {};
    std::memcpy(head, data, 16);
    if (head[0] != magic || head[1] != kVersion || head[2] > kKeysMax || n != 16 + (size_t)head[2] * 8 + 8) return {};
    std::vector<uint64_t> keys(head[2]);
    if (!keys.empty()) std::memcpy(keys.data(), data + 16, keys.size() * 8);
    uint64_t c;
    std::memcpy(&c, data + 16 + keys.size() * 8, 8);
    return c == check(keys) ? keys : std::vector<uint64_t>{};
}

/// This session's keys first, then those of the sessions before it has not, max at most
inline std::vector<uint64_t> merge(const std::vector<uint64_t>& now, const std::vector<uint64_t>& before, size_t max = kKeysMax) {
    std::vector<uint64_t> out;
    std::unordered_set<uint64_t> seen;
    for (const auto* list : {&now, &before})
        for (const uint64_t k : *list) {
            if (out.size() >= max) return out;
            if (k && seen.insert(k).second) out.push_back(k);
        }
    return out;
}

/// A session's order: each program once, as first needed, the first kKeysMax
struct Record {
    std::vector<uint64_t> keys;
    std::unordered_set<uint64_t> seen;
    size_t saved = 0;   // (keys.size() at the last write)
    bool add(uint64_t key) {
        if (!key || keys.size() >= kKeysMax || !seen.insert(key).second) return false;
        keys.push_back(key);
        return true;
    }
    /// Whether to write now: it grew since the last write, and kSaveEveryMs passed (since_ms)
    bool due(int64_t since_ms) const { return keys.size() > saved && since_ms >= kSaveEveryMs; }
};

/// The read-ahead in the sessions' order: where it is, where the draws are, and the programs it keeps made for them
struct Ahead {
    bool keep_all = true;   // ((Triforce, wp19) kept until needed, nothing passed by, the whole order; false: the rules before)
    std::vector<uint64_t> order;
    std::unordered_map<uint64_t, int64_t> place;   // (key -> its place in order)
    std::unordered_map<uint64_t, int64_t> kept;    // (read and kept, not taken yet: key -> place)
    std::unordered_set<uint64_t> chosen;           // (chosen to be read, not kept yet)
    std::unordered_set<uint64_t> cold;             // ((Triforce, wp19) kept or chosen, read without the pipelines' warm yet)
    std::set<std::pair<int64_t, uint64_t>> cold_at;   // ((Triforce) the cold by place, the nearest first: no look at each)
    std::unordered_set<uint64_t> passed;           // ((Triforce, keep_all) kept, kBehind behind where the draws are: pass)
    std::deque<int64_t> recent;                    // (the places of the last kRecent programs the draws needed first)
    int64_t next = 0;        // (the next place to read)
    int64_t furthest = -1;   // (where the draws are: the furthest the middle of recent has been)

    void load(std::vector<uint64_t> keys) {
        order = std::move(keys);
        place.clear();
        for (size_t i = 0; i < order.size(); i++) place.emplace(order[i], (int64_t)i);
        kept.clear(), chosen.clear(), cold.clear(), cold_at.clear(), passed.clear(), recent.clear(), next = 0, furthest = -1;
    }
    /// ((Triforce, wp19) The kept and chosen read with the pipelines' warm: at most kKeptMax, the warm's window. ((Triforce) not
    /// those passed: the draws went by them, and their room is the window's again)
    size_t warmed() const {
        const size_t all = kept.size() + chosen.size();
        return all - std::min(cold.size() + passed.size(), all);
    }
    /// Whether key is (to be) read without the warm
    bool is_cold(uint64_t key) const { return cold.count(key) != 0; }
    /// ((Triforce) key (to be) read without the warm: cold, and by its place
    void make_cold(uint64_t key) {
        const auto it = place.find(key);
        if (cold.insert(key).second && it != place.end()) cold_at.emplace(it->second, key);
    }
    /// ((Triforce) key no longer cold
    void uncold(uint64_t key) {
        if (!cold.erase(key)) return;
        if (const auto it = place.find(key); it != place.end()) cold_at.erase({it->second, key});
    }
    /// ((Triforce, wp19) Whether a kept program read cold has its warm due (next_warm would give one). ((Triforce) the nearest
    /// first: the only cold ones it looks past are those chosen and still being read, a worker's few)
    bool warm_due() const {
        if (cold_at.empty() || warmed() >= kKeptMax) return false;
        for (const auto& [at, k] : cold_at)
            if (kept.count(k)) return true;
        return false;
    }
    /// ((Triforce, wp19) The nearest kept program read cold, now that the warm has room: its warm is due (no longer cold); 0: none
    uint64_t next_warm() {
        if (cold_at.empty() || warmed() >= kKeptMax) return 0;
        for (const auto& [at, k] : cold_at)
            if (kept.count(k)) {
                const uint64_t best = k;
                uncold(best);
                return best;
            }
        return 0;
    }
    /// The programs kept (and chosen to be) at most
    size_t max_kept() const { return keep_all ? kKeepAllMax : kKeptMax; }
    /// Whether there is more to read: places left, and room among the kept
    bool more() const { return next < (int64_t)order.size() && kept.size() + chosen.size() < max_kept(); }
    /// Whether the order names key
    bool names(uint64_t key) const { return place.count(key) != 0; }
    /// The next key to read (wanted: still on disk, not read ahead already, not being read), chosen; 0: none now
    template <class Wanted> uint64_t choose(Wanted wanted) {
        while (more()) {
            const int64_t at = next++;
            if (!keep_all && at < furthest - kBehind) continue;   // (the draws went past it)
            const uint64_t k = order[(size_t)at];
            if (!kept.count(k) && !chosen.count(k) && wanted(k)) {
                chosen.insert(k);
                if (keep_all && warmed() > kKeptMax) make_cold(k);   // ((Triforce, wp19) beyond the warm's window: cold)
                return k;
            }
        }
        return 0;
    }
    /// Whether key was chosen here (to be kept apart from the shelf once read)
    bool is_chosen(uint64_t key) const { return chosen.count(key) != 0; }
    /// key, chosen here, read: kept (true), or not (false: nothing made, or taken by a draw meanwhile)
    void read_done(uint64_t key, bool keep) {
        if (chosen.erase(key) && keep) {
            const int64_t at = place.count(key) ? place.at(key) : 0;
            kept.emplace(key, at);
            if (keep_all && at < furthest - kBehind) pass(key);   // ((Triforce) the draws went by it while it was read)
        } else {
            uncold(key);
        }
    }
    /// ((Triforce, wp19, keep_all only) key, which the order names, read by an ask of the draws and not chosen here: kept with
    /// the order's (true), not on the shelf. Not one chosen or kept already, nor beyond the bound
    bool keep(uint64_t key) {
        const auto it = place.find(key);
        if (!keep_all || it == place.end() || chosen.count(key) || kept.size() + chosen.size() >= max_kept()) return false;
        if (!kept.emplace(key, it->second).second) return false;
        if (it->second < furthest - kBehind) pass(key);   // ((Triforce) behind where the draws are already)
        return true;
    }
    /// ((Triforce, keep_all) key, kept, kBehind behind where the draws are: kept still (a draw may yet take it), but no longer
    /// counted in the warm's window, and not warmed if it was read cold (the draws went by it)
    void pass(uint64_t key) {
        if (!kept.count(key)) return;
        uncold(key);
        passed.insert(key);
    }
    /// ((Triforce, wp19) key taken by a draw: no longer kept
    void taken(uint64_t key) { kept.erase(key), uncold(key), passed.erase(key); }
    /// ((Triforce, wp19) Every program kept, to let go, into out (the RAM short). ((Triforce) only their cold marks go: one
    /// chosen and being read keeps its own, so that it is kept cold once read and warmed when the window reaches it)
    void let_go_all(std::vector<uint64_t>& out) {
        for (const auto& [k, at] : kept) out.push_back(k), uncold(k);
        kept.clear(), passed.clear();
    }
    /// A draw needed key for the first time (took it, read it, or found it nowhere): where the draws are moves on, to the middle
    /// place of the last kRecent. The keys kept that are now kBehind behind it, to let go, into out. ((Triforce, wp19) keep_all:
    /// only key leaves the kept (taken); nothing is let go for where the draws are. ((Triforce) where the draws are moves on as
    /// before, and the kept now kBehind behind it are passed (pass): a warmed program the draws never took (an order recorded
    /// in HDR replayed in SDR: some 300 of its first 512) no longer holds the warm's window for the rest of the session)
    void met(uint64_t key, std::vector<uint64_t>& out) {
        kept.erase(key), uncold(key), passed.erase(key);
        const auto it = place.find(key);
        if (it == place.end()) return;
        recent.push_back(it->second);
        if (recent.size() > kRecent) recent.pop_front();
        std::vector<int64_t> s(recent.begin(), recent.end());
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        if (s[s.size() / 2] <= furthest) return;
        const int64_t was = furthest;
        furthest = s[s.size() / 2];
        if (keep_all) {   // ((Triforce) nothing let go: the places now behind, each once, passed)
            for (int64_t at = std::max<int64_t>(0, was - kBehind); at < furthest - kBehind; at++) pass(order[(size_t)at]);
            return;
        }
        for (auto k = kept.begin(); k != kept.end();) {
            if (k->second < furthest - kBehind) {
                out.push_back(k->first);
                k = kept.erase(k);
            } else {
                ++k;
            }
        }
    }
};

} // namespace mkwvk::program_order
