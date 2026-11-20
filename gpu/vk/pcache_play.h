// (Triforce) The pipeline cache's play file (vk_draw.cpp pcache_init, cached_graphics, cached_compute, pipeline_cache_save),
// apart from Vulkan so that tests/test_pcache_play.py runs its rules. NX2navi's play cache (NX2NAVI_PIPELINE_CACHE_PLAY,
// runtime/vk_pipeline.c pipe_create_in, cache_load, cache_save; chrg6, at 62cc3b13), written for Triforce's cache.
//
// Why. mkw_vk_pipelines.bin holds what the preparation built and what earlier runs made: 2.6 GB on the owner's PC with the
// stage libraries. A file can only be written whole, so a save after a few new pipelines fetched all of it from the driver
// into memory and wrote it again: the owner's test18 start wrote 2,683,916 KB right after its precompile, for 116 KB new,
// on the start-up's own thread. In play the 64 MB growth rule (pipeline_cache_save, 11-14) already waits for more; the start's
// precompile and the game's close did not.
//
// What. Once the preparation is done, what a run compiles goes into a second cache with a file of its own,
// mkw_vk_pipelines_play.bin beside the first (with its own .for, the same shaders' stamp): a creation with the main cache asks
// it first with VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT (the main cache or the driver's own gives the
// pipeline at once), and only when that answers VK_PIPELINE_COMPILE_REQUIRED goes to the play cache with the creation's own
// flags: one there from an earlier run comes back, one asked of the caches only is answered by it too, the rest is compiled
// into it (create). A save writes the play file when it changed, and the main file only when something was compiled into the
// main cache since its last save (the preparation, which builds into it as before) (save_what). The play file is read back at
// the next start; once it has reached kFoldMb (RX_VK_PCACHE_FOLD_MB) it is merged into the main cache at that start, the
// session builds into the main cache as before, its first save writes the main file and the play file is removed (fold_due).
// Without pipelineCreationCacheControl, or RX_VK_PCACHE_PLAY_FILE=0, there is no play cache: everything as before.
// A cache changes how long a pipeline takes to make, never the pipeline: nothing of this can change a picture.
#pragma once

#include <cstdint>

namespace pcache_play {

inline constexpr uint64_t kFoldMb = 64;   // (NX2navi's NX2NAVI_PIPELINE_CACHE_PLAY_MB default)

/// Whether a creation goes through the play cache: with the main cache (not none, not the gate's none), a play cache there,
/// and no preparation under way (it builds into the main cache)
inline bool routes(bool main_cache, bool play_cache, bool preparing) {
    return main_cache && play_cache && !preparing;
}

/// At the start, the play file read: whether it is folded into the main cache now (fold_bytes 0: never)
inline bool fold_due(uint64_t play_bytes, uint64_t fold_bytes) {
    return fold_bytes && play_bytes >= fold_bytes;
}

/// What a save writes: the play file (there is a play cache, its data's size changed since its last write) and the main
/// file (there is no play cache, or pipelines were compiled into the main cache since its last save)
struct SaveWhat {
    bool play = false, main = false;
};
inline SaveWhat save_what(bool play_cache, uint64_t play_now, uint64_t play_saved, int64_t main_built) {
    SaveWhat w;
    w.play = play_cache && play_now && play_now != play_saved;
    w.main = !play_cache || main_built > 0;
    return w;
}

/// One creation (create(cache, info, out) makes it, as vkCreate*Pipelines with one info). route: routes(). fail_bit:
/// VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT, compile_required: VK_PIPELINE_COMPILE_REQUIRED. *into_play is set
/// when the play cache was asked (it made or answered the creation)
template <class Info, class Cache, class Pipeline, class Result, class Create>
Result create(bool route, Cache main, Cache play, const Info* ci, Pipeline* out, uint32_t fail_bit, Result compile_required,
              Create make, bool* into_play) {
    if (!route) return make(main, ci, out);
    Info asked = *ci;
    asked.flags |= fail_bit;
    Result r = make(main, &asked, out);
    if (r != compile_required) return r;
    *out = Pipeline{};
    if (into_play) *into_play = true;
    return make(play, ci, out);
}

}   // namespace pcache_play
