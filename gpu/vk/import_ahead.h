/* (Triforce) The game's memory imported ahead of its first binding (vk_memory.cpp import_ahead_*). A pool's memory is imported
   into Vulkan (VK_EXT_external_memory_host) at the first binding that reaches it, by 16 MB chunk; the import is the driver's
   page locking, 8 to 17 ms a chunk on the owner's RTX 5090, and a binding the GPU writes, or a texture's storage read by the
   GPU's unswizzle, is imported there and then on the replay's thread: seven in one dispatch made a submit of 79 ms in the
   owner's test4 log, and the "unswizzled on the GPU" uploads of 22 to 32 ms were imports of their storage. The game's pools sit
   at the same addresses from run to run (triforce_mirror_keep.txt relies on that too), so the ranges a run imported on its
   own thread are kept in triforce_imports_ahead.txt beside the game, and in the next runs each is imported by the background
   import thread as soon as the game's pool table holds memory there, before a binding wants it. A binding that comes while it is
   under way waits for it, as for any import in the background. Plain functions: tests/test_import_ahead.cpp checks them.
   RX_VK_IMPORT_AHEAD=1 or 0: on or off on any card; unset: on with NVIDIA's driver only, as the background imports of a
   binding's reads are (RX_VK_ASYNC_IMPORT: background imports slowed an AMD card's own, vk_memory.cpp gpu_buffer_bind). */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace mkwvk::import_ahead {

constexpr size_t kMax = 256;        // (ranges kept in the file at most)
constexpr size_t kPerLook = 4;      // (ranges sent to the background thread at one look, 16 under way at most: a look comes every 8
                                    //  submissions too, and the test9 session's queue submits stalled beside batches of 16, made
                                    //  one after the other; each range is sent once a session, vk_memory.cpp)

/// RX_VK_IMPORT_AHEAD: 1 on, 0 off, unset (or anything else): on for NVIDIA's driver (vendor 0x10de) only, as before test12.
/// ((Triforce, 11-15) Off by default since test12: in the lab, at the title, with test12's binaries, the GPU hung after the
/// write-back of pool 0x1072000000 (a mirror in pieces) in 6 of 12 runs with the imports ahead on and in 0 of 10 with
/// RX_VK_IMPORT_AHEAD=0 (lazy write-back, GPL, BC1, Reflex, the sampler cache and the helper threads cleared, one by one);
/// the ahead list held 0x1072000000..0x1077000000, imported on the background thread while that pool's mirror was in use.
/// The owner's test10 never hung: its RAM was low all session, which pauses the imports ahead. Back on once the race is
/// found and closed. vendor: kept for that, unused)
/// ((Triforce, 11-15, later) A race that fits is closed: the host decommitted the game's freed pages under an import the background
/// thread had locked and not yet put in the table (vk_memory.cpp bg_held, memory_guest_imported; tests/test_bg_import_held.py), and
/// the imports ahead make that window wide where the game frees pages. Still off until a lab A/B with RX_VK_IMPORT_AHEAD=1 at the
/// title on a build with that fix hangs no more; the log's "freed under a background import" lines say whether the window was hit)
/// ((Triforce, 11-15, the lab A/B) On again: with that fix, 10 of 10 lab runs at the title with the imports ahead on were clean
/// and each hit the window 2 to 16 times ("freed under a background import"); with the fix undone (RX_VK_BG_IMPORT_HOLD=0)
/// 4 of 10 hung and others showed the title's video garbled. Runs E:\TriforceDev\lab-runs\holdA*, holdB*)
inline bool on(const char* env, uint32_t vendor) {
    if (env && env[0] == '1' && !env[1]) return true;
    if (env && env[0] == '0' && !env[1]) return false;
    return vendor == 0x10DE;
}

/// (Triforce, test8) A range imported ahead, [lo, hi), on chunk bounds: what an import on a binding's own thread took, its
/// span over the game's pools across chunk ends included (vk_memory.cpp span_game_pools). The first version kept 16 MB chunks
/// and imported each on its own: a binding across their ends then made them one import again on its own thread ("not in the
/// background: an import beside it grows", 6 of the 13 still made so in the owner's test8 session, and the 64 MB imports that
/// took in four of them)
struct Range {
    uint64_t lo = 0, hi = 0;
    bool operator==(const Range& o) const { return lo == o.lo && hi == o.hi; }
};

/// The file's lines: "lo hi" in hex (a range), or "lo" alone (a chunk, as the first version wrote them); lines that are no such
/// numbers, and ranges not on chunk bounds, are left out. Sorted, none inside another, at most kMax
inline std::vector<Range> parse(const char* text, uint64_t chunk) {
    std::vector<Range> out;
    if (!chunk) return out;
    for (const char* p = text; p && *p;) {
        char* end = nullptr;
        const unsigned long long lo = std::strtoull(p, &end, 16);
        unsigned long long hi = 0;
        bool ok = end != p && lo && lo % chunk == 0;
        if (ok && *end == ' ') {
            const char* q = end + 1;
            hi = std::strtoull(q, &end, 16);
            ok = end != q && hi > lo && hi % chunk == 0 && hi - lo <= (1ull << 30);
        } else {
            hi = lo + chunk;
        }
        ok = ok && (*end == '\n' || *end == '\r' || !*end);
        if (ok) out.push_back({lo, hi});
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    std::sort(out.begin(), out.end(), [](const Range& a, const Range& b) { return a.lo != b.lo ? a.lo < b.lo : a.hi > b.hi; });
    std::vector<Range> kept;   // (none inside another)
    for (const Range& r : out)
        if (kept.empty() || r.hi > kept.back().hi) kept.push_back(r);
    if (kept.size() > kMax) kept.resize(kMax);
    return kept;
}

/// [lo, hi) into the list (sorted, none inside another): true when the list changed (the caller writes the file again). Ranges
/// inside the new one go; a new one inside a kept one changes nothing; a full list takes nothing new
inline bool add(std::vector<Range>& list, uint64_t lo, uint64_t hi) {
    if (hi <= lo) return false;
    for (const Range& r : list)
        if (r.lo <= lo && hi <= r.hi) return false;
    const size_t before = list.size();
    std::erase_if(list, [&](const Range& r) { return lo <= r.lo && r.hi <= hi; });
    if (list.size() == before && list.size() >= kMax) return false;
    list.insert(std::lower_bound(list.begin(), list.end(), lo, [](const Range& r, uint64_t v) { return r.lo < v; }), Range{lo, hi});
    return true;
}

/// The file's text for list
inline std::vector<char> text(const std::vector<Range>& list) {
    std::vector<char> out;
    char line[48];
    for (const Range& r : list) {
        const int n = std::snprintf(line, sizeof line, "%llx %llx\n", (unsigned long long)r.lo, (unsigned long long)r.hi);
        out.insert(out.end(), line, line + n);
    }
    return out;
}

/// The ranges of list that the game's pools (sorted by lo) reach now and wanted(range) allows (nothing imported over it,
/// nothing under way in it), at most n of them
template <class Pools, class Wanted>
std::vector<Range> due(const std::vector<Range>& list, const Pools& pools, size_t n, Wanted wanted) {
    std::vector<Range> out;
    for (const Range& r : list) {
        if (out.size() >= n) break;
        auto g = std::lower_bound(pools.begin(), pools.end(), r.hi, [](const auto& p, uint64_t v) { return p.lo < v; });
        if (g == pools.begin() || std::prev(g)->hi <= r.lo) continue;   // (no pool starting below its end reaches into it)
        if (wanted(r)) out.push_back(r);
    }
    return out;
}

} // namespace mkwvk::import_ahead
