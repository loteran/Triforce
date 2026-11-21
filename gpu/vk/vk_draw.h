// Draws and dispatches (vk_draw.cpp).
#pragma once

#include <memory>
#include <vector>

#include "vk_internal.h"
#include "vk_state.h"

namespace mkwvk {
/// Something other than the draw path pushed or bound descriptors in the recording (its next push is whole)
void descriptors_disturbed();

/// ((Triforce, S8, FSR3-07) The texture LOD bias while DLSS or FSR scales the 3D with jitter (sharpen_rules.h lod_bias, on
/// the scene's material samplers, lod_biased): an upscaler that drew a frame says its render size and output width (note), and
/// once a present (tick, vk_replay.cpp) the bias follows them, the samplers made anew when it changes. Only the draws into
/// targets of that render size, or of this frame's scene camera's (scene, below), take it (vk_draw.cpp draw_in_scene): no dispatch, shadow map or capture. RX_VK_UPSCALE_LOD_BIAS=0:
/// none, the game's own bias alone (for a lab A/B); a negative number (-1, -1.5): that bias in its place
void upscale_lod_note(uint32_t render_w, uint32_t render_h, uint32_t output_w);
void upscale_lod_tick();
/// ((Triforce, S8) The scene camera's target size this frame, as the upscaler's notes take it in stream order
/// (temporal_note_scene_draw, fsr3_inputs::note_scene_draw): the draws at that size are the scene in the frame the game's
/// dynamic resolution moved to it, before the upscale notes it (sharpen_rules.h lod_scene_now)
void upscale_lod_scene(uint32_t width, uint32_t height);

/// A graphics pipeline as triforce_pipelines.bin keeps it (the precompile's notes): what built it
struct PipeNote {
    uint64_t key, prog, variant;
    uint32_t topo, n, samples, depth_format, has_depth;
    uint32_t color_format[8];
    NvnState s;
};
/// (vk_predict.cpp) The pipelines the notes predict for the kept graphics program variants no note names (key 0: not made yet)
/// (Triforce) A predicted pipeline as what it is made from: the program variant, the note whose vertex layout it takes and
/// the note whose targets and state it takes. 32 bytes against a note's ~26 KB: the plan is kept in a file, and the notes
/// are made again from it (predict_rebuild) when the programs' file can no longer be gone through whole (after the
/// precompile took the noted programs out of its list)
struct PredictPlan {
    uint64_t prog, variant, vertex_note, output_note;
};
std::vector<std::unique_ptr<PipeNote>> predict_pipelines(const std::vector<std::unique_ptr<PipeNote>>& notes, size_t* programs,
                                                         std::vector<PredictPlan>* plan = nullptr);
std::vector<std::unique_ptr<PipeNote>> predict_rebuild(const std::vector<std::unique_ptr<PipeNote>>& notes,
                                                       const std::vector<PredictPlan>& plan);
}

#include <functional>

#include "vk_internal.h"
#include "vk_state.h"

namespace mkwvk {

struct DrawArgs {
    int prim = 0;
    bool indexed = false;
    int itype = 0;            // NVNindexType: byte, short, int
    int count = 0, first = 0, base_vertex = 0, base_instance = 0, instances = 1;
    uint64_t ibuf = 0;
    // indirect: arguments at indirect (stride, up to max), their count at count_addr (or 0)
    uint64_t indirect = 0, count_addr = 0;
    int max = 0, stride = 0;
    // mesh shading (NVN2 task / mesh programs): workgroups, or indirect as above
    bool mesh = false;
    int groups[3] = {0, 1, 1};
    uint32_t first_task = 0;   // (hookshot: a direct mesh draw's first task workgroup, NVN's DrawMeshTasks(first, count))
};

/// A recording starts (its slot's descriptor pool and upload stream are reset); value: the timeline value it signals
void draw_begin_recording(int slot, VkCommandBuffer cmd, uint64_t value);
/// The timeline value the recording this thread is making signals (0: none begun): what an upscaler's context was last used by,
/// so that it is destroyed only once the GPU is past it (vk_ffx3.cpp)
uint64_t draw_recording_value();
/// Publish CPU writes from this slot's used streams before its commands reach the GPU (also for worker slots).
void draw_flush_recording(int slot);
/// The recording's stream still holds [at, at + n) in the mapping at base (a reset starts used over)
bool stream_holds(const uint8_t* base, uint64_t at, uint64_t n);
/// The open render pass ends (before transfers, clears, barriers and the recording's end)
void end_pass(VkCommandBuffer cmd);
/// (Triforce) RX_VK_SPLIT_BARRIER (barrier_rules.h, vk_replay.cpp maybe_split): before a part's split ends the pass, whether one
/// is open (not with the parallel replay); its targets and what its draws sampled and wrote kept for pass_carry_resume
bool pass_carry_take();
/// The pass pass_carry_take kept, begun again on cmd (the next part's) with its targets behind vk_texture.cpp pass_resume_barrier,
/// with what its draws sampled and wrote: draw_hazards and clear_after_draws go on as in one pass. go: false drops it (the split
/// went the old way, with full barriers)
void pass_carry_resume(VkCommandBuffer cmd, bool go);
void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d);
/// Clears of the bound targets, inside the open pass (NOP_CLEAR_COLOR, NOP_CLEAR_DS)
void clear_color_target(VkCommandBuffer cmd, NvnState& s, int index, const float rgba[4], uint32_t mask);
void clear_depth_stencil(VkCommandBuffer cmd, NvnState& s, float depth, bool depth_mask, uint32_t stencil,
                         uint32_t stencil_mask);
void clear_stats(int64_t* clears, int64_t* masked);
/// indirect: the workgroup counts' GPU address (x, y, z unused)
void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect);
void dry_mark_written(NvnState& s, bool compute, bool first);   // (hookshot: the parallel replay's dry pass)
void dry_marks_begin();   // (at the start of each dry pass: the last targets it skipped by are forgotten)
/// (Triforce) The dry pass with a temporal upscaler or a frame generation on (stream_order.h): a draw (NVN primitive prim, or a
/// mesh draw) told to their hooks as draw() tells them, true at the engine's point before the bloom; a dispatch, true where it
/// may be the upscale's. At the first true the dry pass stops telling them, and the segments' recording takes over there in
/// order (vk_replay.cpp replay_handle)
bool dry_temporal_draw(NvnState& s, int prim, bool mesh);
bool dry_temporal_dispatch(NvnState& s);
/// (Triforce) The early tail (vk_replay.cpp, tail_early.h): a dispatch that is the gameplay upscale's, where a frame's tail
/// begins; and the watch on the game's programs that reach guest memory past their bindings (page table, texel buffers), counted
/// while it is on
bool upscale_dispatch(NvnState& s);
void draw_tail_watch(bool on);
uint64_t draw_tail_programs();
/// (Triforce, vk_replay.cpp) While a segment is recorded whose draws the dry pass told the temporal hooks: draw() does not
/// tell them again
bool replay_hooks_told();
/// (Triforce, vk_replay.cpp) The draw or dispatch being recorded: its place in the stream (the command buffer's replay, then
/// its draws and dispatches before it), the same whichever thread records it
uint64_t replay_stream_pos();
/// (Triforce, 11-17, vk_replay.cpp "occlusion queries") RX_VK_OCC_QUERY (default on, =0 off): NVN's samples-passed reports from Vulkan
/// occlusion queries. occ_live: the queries run (set once, at the first recording; false: none of these is called). draw()
/// calls occ_draw_enter as a draw comes in, occ_draw_begin once its pass is open (a count's first draw of a pass begins a
/// query) and occ_draw_issued for each draw it records (a count missing one is not kept); end_pass calls occ_pass_end before
/// the pass ends (a query begun in a pass ends in it)
extern bool occ_live;
void occ_draw_enter();
void occ_draw_begin(VkCommandBuffer cmd);
void occ_draw_issued();
void occ_pass_end(VkCommandBuffer cmd);
void draw_stats(int64_t* draws, int64_t* skipped, int64_t* dispatches, int64_t* pipelines);
void draw_stats2(int64_t* passes, int64_t* mesh, double* pipeline_ms);
void draw_stats3(int64_t* pending_skips, int64_t* waits, double* wait_ms);
/// (Triforce) Of draw_stats3's wait_ms, in microseconds so far: the replay's waits for pipelines (a draw's, waited for whole or
/// from the frame's budget) and for translations (a draw's and a dispatch's program, the dry pass's too): the performance
/// overlay's Pipeline waits and Shader waits (vk_menu.cpp perf_sample). Atomic counters: read from any thread
void draw_wait_split(int64_t* pipe_us, int64_t* shader_us);
/// (Triforce, vk_replay.cpp hold_step, once a present) The draws and dispatches left out since the last present because their
/// program or pipeline was still being made (0 with RX_VK_HOLD_PIPELINES=0): such a frame is held as one with a texture owed
/// its bytes (present_hold.h, pipe_wait.h). And the hold's mode for the next frame's draws (present_hold::Mode): with DEFERS a
/// full-screen draw whose pipeline is not ready is left out, its frame held, rather than waited for
int64_t draw_left_out_take();
void draw_hold_mode(int mode);
int64_t draw_dummy_textures();   // (Triforce: texture bindings that sampled the dummy so far: RX_VK_FRAME_TRACE)
/// (hookshot) pipelines linked from stage libraries in the draw, and the libraries asked of the driver for them: had, and cold;
/// of the cold, those a worker has built since, and the workers' time in them. False: no stage libraries (RX_VK_GPL)
bool draw_stats4(int64_t* links, double* link_ms, int64_t* lib_hits, int64_t* lib_cold, int64_t* lib_built, double* lib_build_ms);
void draw_skip_report(double frames);   // (hookshot) skips by the line that made them
int64_t draw_desc_reused();
/// (RX_VK_DESC_SLOTGEN) Of those, the reuses the global registrations' generation check would have rebuilt for
int64_t draw_slotgen_saves();
/// RX_VK_FLASH_RECORD=<key>,...: the last frames' draws of those programs, written to the log for frames around a flash
void draw_records_log(uint64_t frame_lo, uint64_t frame_hi);
void pass_traces_log(uint64_t frame_lo, uint64_t frame_hi);   // (RX_VK_PASS_TRACE=1)
/// Changes with every uniform update the stream makes (vk_replay.cpp): descriptors bound before may be stale
uint64_t ub_generation();
/// A background thread (shader translation, pipeline builds) reads guest memory from the captures only, never the stream's
/// uniform updates: that list is the replay's, which grows and is pruned while such a thread runs
void ub_thread_off();
/// The pipeline cache to disk (force: whatever was built since the last save; else only after a few new ones)
void pipeline_cache_save(bool force);
/// (Triforce) Once a present, on its thread: the fast-linked pipelines whole builds have replaced are destroyed once no draw
/// recorded with one can still be on its way (gpl_rules.h). Nothing without the fast pipeline link
void gpl_retire_run();
/// (vk_memory.cpp) The menu's Restart and Exit, before the window closes: no verdict on the memory trial for a session ended on purpose
void memory_trial_orderly_end();

/// The stream's uniform updates (UPDATE_UB, vk_replay.cpp) that overlap [addr, addr + n) and may not be in guest
/// memory yet when the GPU reads it: a copy of the range with them applied, at the offset alloc returns in the
/// recording's stream (mapped at base), or UINT64_MAX when none overlaps
uint64_t ub_overlay(uint64_t addr, uint64_t n, uint8_t* base, const std::function<uint64_t(uint64_t)>& alloc);
/// Guest memory as the stream's place sees it: as submitted, the stream's uniform updates before it over it
bool ub_read(uint64_t addr, void* dst, uint64_t n);

} // namespace mkwvk
