/* (Triforce) Programs read ahead of their first draw from the program cache (vk_shader.cpp program_cache_load). A program
   a draw meets first in a session is read from triforce_programs.bin there and then, on the draw's thread: the record off
   the disk (the file is gigabytes, read where a record is wanted), its stages' shader modules made. In the owner's test4
   logs that was the "programs" phase of the slow submits, up to 53 ms in one. The file holds the records in the order earlier
   runs first made them, so the programs a scene's first draws need lie together in it: when a draw reads one, a worker reads
   the next ones in the file's order and keeps them made, and the draws that come to them take them from it (waiting for one it
   is reading, which is less than reading it). Nothing changes what a draw gets: the same records, read the same way. Plain
   functions, apart from the file and Vulkan: tests/test_program_readahead.cpp checks them. RX_VK_PROGRAM_READAHEAD=<n>: the
   records read after each (32), 0: none, as before. */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <utility>
#include <vector>

// ((Triforce) and the draws' own reads, with the shaders' lock let go, and the read-ahead in the order the last sessions first
// needed their programs: vk_shader.cpp's include lines are in the translation stamp (program_carry.h), this header's are not)
#include "program_order.h"
#include "program_read_once.h"

namespace mkwvk::program_readahead {

constexpr int kDefault = 32;     // (records read ahead after each program a draw read itself)
constexpr size_t kParkedMax = 512;   // (programs read ahead and not taken yet, at most: each holds its shader modules)
constexpr size_t kAsksMax = 4;       // (the newest asks only: an older one's place in the file is behind the play)
constexpr int kWaitMs = 50;          // (a draw waits at most this for a program the worker is reading, then reads it itself)

/// RX_VK_PROGRAM_READAHEAD: the records read after each, 0 to 256 (unset or not a number: kDefault)
inline int count(const char* env) {
    if (!env || !*env) return kDefault;
    char* end = nullptr;
    const long v = std::strtol(env, &end, 10);
    if (end == env || *end) return kDefault;
    return (int)std::clamp<long>(v, 0, 256);
}

/// The keys of the n records after offset `at` in the file's order (order: (offset, key) sorted by offset), the first n for
/// which wanted(key) holds (still on disk, not read ahead already, not being read)
template <class Wanted>
std::vector<uint64_t> next_keys(const std::vector<std::pair<int64_t, uint64_t>>& order, int64_t at, size_t n, Wanted wanted) {
    std::vector<uint64_t> out;
    auto it = std::upper_bound(order.begin(), order.end(), std::make_pair(at, UINT64_MAX));
    for (; it != order.end() && out.size() < n; ++it)
        if (wanted(it->second)) out.push_back(it->second);
    return out;
}

/// An ask to read after offset at, kept with the newest kAsksMax (an ask for a place already asked for is not added again)
inline void ask(std::deque<int64_t>& asks, int64_t at) {
    if (std::find(asks.begin(), asks.end(), at) != asks.end()) return;
    asks.push_back(at);
    while (asks.size() > kAsksMax) asks.pop_front();
}

/* (Triforce, test16) The order to read ahead in. The file's order is the order the records were written, and since the shaders
   are prepared before the title (some 46,600 programs, test7 on) that is the preparation's, which says nothing of what a scene
   draws: loading a save at the file select read 596 programs on the draws' threads in four frames (457.9, 337.4, 162.0 and
   158.7 ms) and took 7 of them read ahead; the 512 read ahead in the menus before it were never taken, and the read-ahead
   stopped with its shelf full of them. The pipeline notes (vk_draw.cpp, triforce_pipelines.bin) name the pipelines play built,
   in the order earlier sessions first built them, each with its program: the programs an area's first frames draw were first
   built together, so they lie together in the notes. So the order is the notes' (each program at its first note), and after
   them the compute programs in the order play first dispatched them (triforce_computes.bin), the file's only for a program
   neither names; and a full shelf lets its oldest go rather than stopping. RX_VK_PROGRAM_READAHEAD_NOTES=0: the file's order
   alone, one thread, no compute programs, and stopping when full, as before. */
constexpr int kWorkers = 4;   // (threads reading ahead, each with a handle of its own on the file: at a load the draws meet a new
                              //  program every few hundred microseconds, and a read is about a millisecond)

/// RX_VK_PROGRAM_READAHEAD_NOTES: on unless "0"
inline bool notes_on(const char* env) { return !(env && env[0] == '0' && !env[1]); }

/// The notes' programs (keys, one a note, in the notes' order; 0: none) as an order to read in: each program once, at the index of
/// its first note
inline std::vector<std::pair<int64_t, uint64_t>> notes_order(const std::vector<uint64_t>& note_programs) {
    std::vector<uint64_t> keys(note_programs);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<bool> taken(keys.size(), false);
    std::vector<std::pair<int64_t, uint64_t>> out;
    for (size_t i = 0; i < note_programs.size(); i++) {
        const size_t at = (size_t)(std::lower_bound(keys.begin(), keys.end(), note_programs[i]) - keys.begin());
        if (!note_programs[i] || taken[at]) continue;
        taken[at] = true;
        out.push_back({(int64_t)i, note_programs[i]});
    }
    return out;
}

/// The key -> index table of an order, sorted by key (index_of looks in it)
inline std::vector<std::pair<uint64_t, int64_t>> by_key(const std::vector<std::pair<int64_t, uint64_t>>& order) {
    std::vector<std::pair<uint64_t, int64_t>> out;
    out.reserve(order.size());
    for (const auto& [at, k] : order) out.push_back({k, at});
    std::sort(out.begin(), out.end());
    return out;
}
/// key's index in the order by_key was made of, -1 when it does not name it
inline int64_t index_of(const std::vector<std::pair<uint64_t, int64_t>>& table, uint64_t key) {
    auto it = std::lower_bound(table.begin(), table.end(), std::make_pair(key, INT64_MIN));
    return it != table.end() && it->first == key ? it->second : -1;
}

/// How many of the oldest parked programs go so that `adding` more fit under max
inline size_t to_let_go(size_t parked, size_t adding, size_t max = kParkedMax) {
    return parked + adding > max ? parked + adding - max : 0;
}

} // namespace mkwvk::program_readahead
