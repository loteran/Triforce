// (Triforce) The kept pipelines no draw has had, given back to the driver while the PC's RAM is short (vk_draw.cpp
// pipelines_release_tick). Apart from Vulkan so that tests/test_pipe_release.py runs it alone.
//
// Why: on a PC of 20 GB or more the precompile builds every pipeline the notes name before the title and keeps it for the
// session (vk_draw.cpp precompile_run). On v0.4.0-test9's 32 GB PC that was 14,535 graphics and 1,033 compute pipelines,
// and the working set grew by 3.6 GB over the precompile (5517 -> 9094 MB): about 235 KB a pipeline at most, its
// programs and notes in that too. The RAM was short all session after (900 to 1,500 MB available, 43 "RAM short" lines),
// with 9.9 GB of the working set outside the game's own memory. How many of those pipelines a session draws depends on
// where it goes; the ones no draw has had are the cheapest to give back: nothing recorded them, no thread's table holds
// them (only graphics_pipeline hands an entry out, and it marks the entry first), and the driver's own cache and ours
// build them again in well under a millisecond when a draw first needs one (the precompile measured 0.2-0.4 ms each
// with the caches warm), on a worker, the draw left out or waiting meanwhile, as for any pipeline new to a session.
// (The idea is NX2navi's pipe_release, which gives back the pipelines no draw has used while video memory is short; the
// code is this back end's own.)
//
// The rules: a pass at most every so many presents while the RAM is short (due), a batch at most a pass (take), and
// only entries the caller says may go: no draw has had them, they are built (ready, with a pipeline) and have no fast
// link. The caller holds the table's locks and destroys what take moved out.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pipe_release {

constexpr size_t kBatch = 512;

/// Whether a pass is due: the RAM short now, and at least every presents since the last pass (last 0: none yet)
inline bool due(bool ram_short, uint64_t presents, uint64_t last, uint64_t every) {
    return ram_short && (last == 0 || presents >= last + every);
}

/// Up to batch entries of map (key -> std::unique_ptr<Entry>) for which may_go(entry) says yes, taken out of it into out.
/// The others stay where they are. Returns how many were taken
template <class Map, class Entry, class MayGo>
size_t take(Map& map, size_t batch, std::vector<std::unique_ptr<Entry>>& out, MayGo&& may_go) {
    size_t n = 0;
    for (auto it = map.begin(); it != map.end() && n < batch;) {
        if (it->second && may_go(*it->second)) {
            out.push_back(std::move(it->second));
            it = map.erase(it);
            n++;
        } else {
            ++it;
        }
    }
    return n;
}

}   // namespace pipe_release
