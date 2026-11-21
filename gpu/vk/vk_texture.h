// Textures (vk_texture.cpp): one VkImage per texture storage, with every level and layer, the way the GPU sees the
// game's memory. Draws sample views of it, passes render into views of one level and layer, copies and clears
// address it by level and layer. Replaces the first version's image per level and layer (vk_image.cpp), which could
// not sample arrays, cube maps, 3D textures or mip chains.
//
// After totk-pc-recomp's runtime/vk_texture.c (MIT): images keyed by storage address plus descriptor, uploads on first
// use and again when the CPU wrote the storage (page write generations, vk_memory.cpp), no re-upload once the GPU
// wrote the image (it is the content then), every image in GENERAL layout. ASTC, which desktop NVIDIA GPUs do not
// sample, is decoded on the GPU from its block-linear storage by Eden's decoder (host_shaders/astc_decoder.comp,
// with the Switch 2 GOB swizzle); other formats are unswizzled on the CPU straight into the staging memory.
#pragma once

#include <string>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "vk_internal.h"
#include "upload_owed.h"
#include "upload_same.h"

namespace mkwvk {

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;   // the image's own format (views may be others of its class)
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags create_flags = 0;   // (hookshot: as its image was made, for the compaction's copy of it)
    uint32_t width = 1, height = 1, depth = 1, levels = 1, layers = 1;   // the image's (after skip)
    uint32_t skip = 0;        // top levels of the texture left out (huge textures: RX_VK_TEX_MAX_DIM, _MAX_MB)
    bool detail_skip = false; // (Triforce) levels of it are left out by the card's texture detail (tex_detail.h)
    bool whole_asked = false; // (and a shader reads it by texel position: it goes, and is made whole: texture_want_whole)
    // (a huge array kept whole as a sparse resident image instead: each layer gets device memory when it is first
    // written, so only what the game streams in costs VRAM. MKW's terrain arrays, 8192 x 8192 x 68 BC6H: 5.8 GB
    // each, written by the GPU's copies into level 0, which a texture with its top level left out lost)
    bool sparse = false;
    std::vector<std::vector<uint8_t>> tiles;    // (per layer and level: which of its sparse blocks have memory)
    std::vector<uint8_t> tail_bound;            // (per layer, or [0] with SINGLE_MIPTAIL: its mip tail has memory)
    std::vector<VkDeviceMemory> sparse_chunks;  // (the device memory blocks are taken from, 64 MB at a time)
    VkDeviceSize chunk_used = 0;
    VkSparseImageMemoryRequirements sparse_req{};
    uint32_t sparse_type = 0;
    VkDeviceSize sparse_align = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool cube = false;        // CUBE_COMPATIBLE
    bool astc = false;        // ASTC blocks decoded into RGBA8 (block_w x block_h)
    bool bc = false;          // (and kept as BC3: big ones, re-encoded on the GPU after decoding; vk_texture.cpp)
    bool bc_resized = false;  // (hookshot-amd: its BC3 image is a resized copy, uneven block levels on AMD; vk_texture.cpp)
    bool bc1 = false;         // (Triforce) and that kept image is BC1, not BC3: every block of it decodes opaque (tex_bc1.h)
    bool bc1_no = false;      // (Triforce) it was BC1 and had to become BC3 (content with alpha, a copy on the GPU): it stays BC3
    int block_w = 1, block_h = 1, bpb = 0;   // the storage's blocks
    // the game's texture
    uint64_t gpu = 0, guest = 0, guest_size = 0;   // storage (GPU address, guest address, bytes)
    nvn_texdesc d{};
    nvn_texlayout L{};
    int nvn_format = 0;       // the format it was made for
    // contents
    bool uploaded = false;    // from guest memory, as of upload_gen
    WriteGen upload_gen;      // (Triforce: a page write generation, 64 bits: write_gen.h)
    uint64_t bc_retry_frame = 0;   // (hookshot) a BC3 image whose upload failed: tried again from this frame (vk_texture.cpp)
    upload_owed::Owed owed;        // (Triforce) an upload that found no memory: what it was to bring, and when it is tried again
    upload_same::State same;       // (Triforce) a re-upload's check against the bytes its image holds (upload_same.h)
    uint64_t map_sig = 0;     // (a texture in a virtual pool: its pages' mapping when last uploaded)
    std::vector<uint64_t> page_map;   // (and each 64 KB page's guest address then: what a remap changed)
    struct Run { uint64_t off, guest, n; };   // (and its storage as runs of contiguous guest memory)
    std::vector<Run> runs;
    uint32_t runs_maps_gen = ~0u;             // (the GPU mappings' generation the runs were made at)
    bool gpu_written = false; // the GPU rendered, copied or stored into it: the image is the content
    bool only_cleared = false; // (hookshot) all the GPU did was clear it: what the CPU writes after still counts (a font cache's glyphs)
    WriteGen gpu_gen;         // (the memory generation when the GPU last wrote it: CPU writes after it are taken)
    bool released = false;    // its texture was finalized after the GPU wrote it: CPU writes count again
    uint64_t let_go = 0;      // (Triforce) the present at which the game finalized a texture over it, 0 once it is used again:
                              // one uploaded from memory goes first while VRAM is short (texture_sweep)
    uint32_t back_gap = 0;    // ((Triforce) and the longest it stayed let go before it was used again, in presents: it goes
                              // early no sooner than 8 times that after a let-go (tex_pressure.h, keep taken)
    WriteGen release_gen;
    uint64_t gpu_wrote_seen = 0;   // (the newest finished GPU write into its storage it was uploaded after)
    uint64_t last_written = 0;     // (the frame the GPU last wrote it: a render target, each frame it is used)
    uint64_t prev_written = 0;     // ((Triforce) the frame it was written in before last_written's, 0: none: redrawn.h)
    uint64_t first_written = 0;    // (the frame its GPU contents began: written in that frame alone, a table the game baked)
    uint64_t verify_hash = 0;      // (RX_VK_VERIFY_DIM: its guest memory's hash when last uploaded)
    uint32_t checked_epoch = 0;
    uint64_t upload_value = 0;   // (the submission its last upload was recorded in, and the parallel replay's segment
    uint64_t upload_seg = 0;     // that recorded it: one before it in the stream uploads it again)
    std::atomic<uint32_t> touching{0};   // (Triforce) a locked look of it is under way (texture_touch): the look without
                                         // the lock does not take it for up to date meanwhile (touch_quick.h)
    uint64_t last_frame = 0;  // the present it was last used in (the cache's age)
    uint64_t first_render_frame = 0;   // the present it was first rendered into in (0: never)
    uint64_t last_value = 0;  // the timeline value of the last submission that used it
    uint64_t bytes = 0;       // device memory it holds (the texels', or a sparse image's chunks)
    uint64_t pad = 0;         // (what the allocator gave it beyond that: padding and alignment; counted in the budget with bytes)
    uint32_t id = 0;          // (its creation number: diagnostics tell images at one address apart)
    uint64_t first_seen = 0;  // (the present it was made at)
    uint64_t owned_frame = 0; // (the present its storage was last noted as render targets' (gpu_owned))
    uint32_t key_layout = 0, key_cls = 0;   // (the key's storage layout and format class)
    bool vw_listed = false;   // (in the list of GPU-written textures of virtual pools: texture_flush_to_guest)
    uint64_t wb_written = 0;  // (Triforce: the last_written its texels were put into the game's memory for, or are on their way for (bake_writeback_step); 0: not)
    uint64_t wb_done_frame = 0;   // (and the present they got there at; 0: not yet)
    uint64_t wb_burst = 0;        // (the frame the GPU began drawing into it after a frame or more of not doing so: texture_written)
    uint32_t wb_quick = 0;        // (how many such runs in a row began within 30 presents of the one before: a target drawn in turns)
    uint64_t wb_rec = 0;          // (the recording it was last listed in as written early in such a run: texture_end_recording)
    uint64_t wb_value = 0;        // (the submission whose own commands write its texels into its memory; 0: none)
    uint64_t pool_key = 0;    // (a render target's image: its create info's key in the pool it goes to when the texture does)
    std::unordered_map<uint64_t, VkImageView> views;
    // (Triforce) a flat texture a program samples as a 3D one: its one-slice 3D copy (vk_texture.cpp twin3d_view), the layer it
    // was copied from and the texture's contents it was copied at
    VkImage twin3d = VK_NULL_HANDLE;
    VmaAllocation twin3d_alloc = nullptr;
    uint32_t twin3d_layer = 0;
    uint64_t twin3d_value = ~0ull, twin3d_written = ~0ull;
    uint64_t twin3d_rec = 0, twin3d_seg = 0;   // (and the submission and parallel segment that copied it last: an earlier
                                               // segment of that submission copies it again, as an upload: upload_seg)
    std::unordered_map<uint64_t, VkImageView> twin3d_views;
};

/// Makes texels [x, x + w) x [y, y + h) of level `level`, layers [first, first + count), of a sparse texture resident
/// (device memory bound to the sparse blocks they fall in) before anything writes them; true when they are (always,
/// for a texture that is not sparse). Without a rectangle: all of those layers, every level.
bool texture_resident(Texture* tex, uint32_t level, uint32_t first, uint32_t count, uint32_t x, uint32_t y, uint32_t w,
                      uint32_t h);
bool texture_resident(Texture* tex, uint32_t first, uint32_t count);
void texture_resident_queue(Texture* tex, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void texture_resident_flush(Texture* tex);
/// What the replay's recording is: the main command buffer, and the one uploads and new images' layout changes go to
/// (submitted right before it, so nothing has to end a render pass for them)
void texture_begin_recording(VkCommandBuffer upload_cmd, uint64_t value);
/// (Triforce) A recording's end, its passes ended: what the GPU drew for the first time into small textures in it goes into
/// their storage in guest memory, by commands of this recording (vk_texture.cpp, "what the GPU baked")
void texture_end_recording(VkCommandBuffer cmd);
/// true when anything was recorded into the upload command buffer since texture_begin_recording
bool texture_uploads_recorded();
/// The recording's upload command buffer, to record into (its first use orders it after the work before)
VkCommandBuffer upload_command_buffer();
/// A copy of an RGBA8 region (decoded ASTC kept as RGBA8) into a texture kept as BC3: through the encoder
bool texture_copy_into_bc(VkCommandBuffer cmd, Texture* src, uint32_t src_level, uint32_t src_layer, int sx, int sy,
                          Texture* dst, uint32_t dst_level, uint32_t dst_layer, int dx, int dy, uint32_t w, uint32_t h,
                          uint32_t layers);
/// (Triforce) Before a copy on the GPU writes a kept texture, or copies one's blocks into another: a texture kept as BC1
/// (tex_bc1.h) becomes BC3, its new image filled from the game's memory again when it had been uploaded. True when it is
/// not BC1 (any more); false when its BC3 image could not be made or filled: the copy is then left out. The replay's lock held
bool texture_bc1_to_bc3(Texture* tex, const char* why);
/// A texture already found used again (its age, and its contents from guest memory when written since: texture_get's
/// second half)
Texture* texture_touch(Texture* tex, bool for_sampling);
/// texture_touch without the lock where it has nothing more to do (its contents up to date for this submission):
/// false when the full texture_touch is needed
bool texture_touch_quick(Texture* tex, bool for_sampling);
/// Whether texture_written and texture_rendered would change nothing this frame (a parallel replay: no lock then)
bool texture_marked(const Texture* tex);
/// The parallel replay's segment this thread records (its order in the submission; uploads and mirror syncs a later
/// segment made are made again by an earlier one that needs them, as each segment's uploads precede it)
void texture_set_segment(uint64_t seg);
uint64_t texture_segment();
/// A parallel replay's segments, in queue order: each one's init command buffer (begun; submitted right before its
/// uploads). A new image's first layout change (UNDEFINED -> GENERAL, and its clear) that a segment's worker makes goes
/// to the earliest segment's still being recorded, not to the maker's own uploads: a segment before the maker's that
/// uses it is submitted first, and found it UNDEFINED (VUID-vkCmdDraw-None-09600 with RX_VK_PARALLEL only)
void texture_segments_begin(const VkCommandBuffer* init_cmds, size_t n);
/// This thread records segment `index` of them (-1: none, after it)
void texture_segment_worker(int index);
/// Segment `index` recorded: its init command buffer ended (no new image's change goes to it after). 1: something went
/// to it (submit it before the segment's uploads), 0: nothing, -1: something did and it could not be ended (lost: the
/// replay stops)
int texture_segment_end(int index);
/// Changes with every submission (a texture touched in it is not checked again in it)
uint32_t texture_epoch();
/// Changes whenever a texture is destroyed (pointers to textures kept elsewhere go stale)
uint64_t texture_destroy_generation();
/// (Triforce) Changes at a frame's end when the frame left textures for later (a new texture's first upload, a new ASTC texture's
/// placeholder): a look that resolved to the dummy for want of a texture goes stale, and nothing else does
uint64_t texture_dummy_generation();
/// (Triforce) The texture detail in effect: 0 full, 1 reduced, 2 low (tex_detail.h; chosen at the first call, which needs
/// the device and its allocator). RX_VK_TEX_DETAIL, else the Insert menu's Texture detail row, else the video memory's
int texture_detail_level();
/// (Triforce) What the video memory alone gives this card (tex_detail.h by_vram), whichever level is in effect: the Texture
/// detail row says it beside Automatic. -1 until the level is chosen
int texture_detail_auto();
/// (Triforce) A shader reads the texture by texel position or asks for its size (spv_exact.h) and the texture detail left
/// levels of it out: it is let go at the next sweep and made whole from then on. Any thread of the replay.
void texture_want_whole(Texture* tex);
/// One texture's own end, where the generation above is too wide (any texture's end changes it, and textures go in most
/// frames while the video memory is full): how many textures were destroyed so far, and whether the one with this creation
/// number (Texture::id) was, since a count taken while it was there. True too when it cannot be told (texture_gone.h)
uint64_t texture_destroyed_count();
bool texture_destroyed_since(uint32_t id, uint64_t since);
/// (Triforce) The creation numbers of the textures whose views went (destroyed, made again in another format, moved by the
/// compaction) from record count `since` on, appended to out, and the count now (*now, always). False when they cannot all be
/// told. Each is recorded before texture_destroy_generation moves for it (view_keep.h, vk_draw.cpp texture_view_of)
bool texture_views_gone_since(uint64_t since, std::vector<uint32_t>& out, uint64_t* now);
/// The texture an NVN texture's storage is (made and uploaded at first use, uploaded again when the CPU wrote its
/// storage since). null for formats and targets that have no image (buffers, unknown formats).
Texture* texture_get(const nvn_tex* t, bool for_sampling);
/// A view of it: the view's levels and layers (all when v is null), as view_type, in the view's format
/// (attachment: one level, one layer; storage: one level, every layer, in a storage-capable format)
enum class ViewUse { Sample, Attachment, Storage };
VkImageView texture_view(Texture* tex, const nvn_tex* t, const nvn_view* v, VkImageViewType view_type, ViewUse use,
                         VkFormat* format_out = nullptr, VkFormat force_format = VK_FORMAT_UNDEFINED);
/// The view format an NVN texture and view sample / render in
int view_nvn_format(const nvn_tex* t, const nvn_view* v);
/// The level and layer a view starts at (the texture's levels)
unsigned view_level(const nvn_view* v);
/// The image level of a texture level (the levels a texture left out read as its first)
unsigned image_level(const Texture* tex, unsigned level);
unsigned view_layer(const nvn_view* v);
/// The GPU wrote it (in the submission being recorded)
void texture_written(Texture* tex, const char* why = "?");   // (why: who wrote it, for the parallel replay's order warning)
/// The GPU writes [gpu, gpu + size) in the submission being recorded (a storage buffer, a buffer copy): textures over
/// it are uploaded again from guest memory once that submission is done
void texture_gpu_wrote(uint64_t gpu, uint64_t size);
/// The program the replay draws or dispatches with next (RX_VK_WATCH_TEX / RX_VK_WATCH_DIM logs name it)
void texture_watch_prog(uint64_t prog);
/// A pass renders into it (first_render_frame)
void texture_rendered(Texture* tex);
/// The presents so far (the frame number textures are aged by)
uint64_t texture_frame_no();
/// A texel buffer view over guest memory (texture targets of NVN_TEXTURE_TARGET_BUFFER, and image buffers)
VkBufferView texel_buffer_view(const nvn_tex* t, const nvn_view* v, bool storage);
/// The texture whose image was rendered for the window (present): t's own, or one rendered at its storage
Texture* texture_for_present(const nvn_tex* t);
/// Ages the cache by one present; images nothing used for a while go, after the work that last read them
void texture_frame(uint64_t completed_value);
/// ((Triforce) tex_warm.h, RX_VK_TEX_WARM: a slot registered, noted; at a submission's start after a light frame, the noted
/// textures made and uploaded ahead)
bool texture_warm_on();   // ((Triforce) RX_VK_TEX_WARM: tex_warm.h)
void texture_warm_note(int slot);
void texture_warm_step(int64_t last_frame_work);
void texture_released(const nvn_tex* t);
/// (diagnostics: what guest memory holds of a texture's storage now, level by level, logged)
void texture_log_guest(const Texture* t);
/// (diagnostics, the replay's lock held) the textures the GPU wrote in [gpu, gpu + size), appended to out
void texture_describe_written(uint64_t gpu, uint64_t size, std::string& out);
/// (a host hook's fast check, any thread) whether a texture the GPU wrote may lie in virtual pages [gpu, gpu + size)
bool texture_gpu_written_in(uint64_t gpu, uint64_t size);
/// Textures the GPU wrote in [gpu, gpu + size) read back and written into their memory, block linear, as the GPU
/// would have left them there: before those virtual pages are unmapped or remapped (the work recorded before is
/// submitted; the replay's lock held). How many were.
int texture_flush_to_guest(uint64_t gpu, uint64_t size);
size_t texture_count();
uint64_t texture_made();
void texture_unswizzle_stats(int64_t* bytes, int64_t* ns);
uint64_t texture_bytes();
/// A full memory barrier (all commands, all accesses): what separates dependent work for now
/// (why: its label in RX_VK_PASS_TIMING's barrier lines)
void full_barrier(VkCommandBuffer cmd, const char* why = "barrier (other)");
/// Full barriers recorded and left out (RX_VK_BARRIER_DEDUPE_OFF=1: none), since the start
void barrier_stats(int64_t* made, int64_t* left_out);
/// (hookshot) Set around a draw's sampled-texture lookups (vk_draw.cpp): a new ASTC texture there may wait for a later frame
/// once this frame's uploads passed their budget (texture_touch). Never for compute: a dispatch may bake what it reads once
extern thread_local bool upload_defer_ok;
/// (Triforce, redrawn.h) Whether a draw's target t was written in the frame before too (the scene, drawn every frame), so that a
/// texture the draw samples may be left for the next frame; false for one drawn once and kept (a text box). Null: true
bool texture_redrawn(const Texture* t);
/// (Triforce) Whether this frame left any texture for later so far (a new one not made or not uploaded, a re-upload left), so
/// that a draw that may not leave one must not reuse descriptors a draw that might built in this frame (vk_draw.cpp)
bool texture_frame_left_any();
/// (Triforce) Whether t still owes bytes (its first upload or a re-upload left for later: upload_owed.h)
inline bool texture_owes(const Texture* t) { return t && (t->owed.whole || !t->owed.ranges.empty()); }
/// (Triforce) The hold of the last picture (present_hold.h, vk_replay.cpp do_present): what the draws of the next frame may do
/// with a new texture once its uploads took their budget (present_hold::Mode), set at each present
void texture_hold_mode(int mode);
/// This frame's looks of a draw that left a texture owed its first bytes (it sampled the dummy or the placeholder in its
/// place): not zero, the frame is held. Read at the present, before texture_frame starts the next frame
int64_t texture_frame_owed();
/// New textures whose first upload was left for a later frame, since the start (the hold's budget and the ASTC one's)
int64_t texture_new_later();
/// A draw pass just ended on cmd (end_pass): pass_start_barrier knows nothing else ran after it ((Triforce) on cmd: its own count,
/// cmd_work.h)
void note_pass_end(VkCommandBuffer cmd);
/// (Triforce) The pass just ended on cmd was ended by the replay itself, to keep a draw or a clear apart from its own pass's work
/// (vk_draw.cpp draw_hazards, clear_after_draws): the next pass starts with a full barrier whatever RX_VK_PASS_BARRIER says
void note_pass_split(VkCommandBuffer cmd);
/// The barrier a draw pass starts with. Full, unless the game mode is on (RX_VK_PASS_BARRIER=game, and by default with NVIDIA's
/// driver) and the last thing on cmd was a draw pass with no barrier of the game's after it: then the new pass waits for the
/// earlier work in its fragment stages only, so its vertex work may overlap the previous pass's fragments (NVN asks for a barrier
/// before rendering results are read; only vertex-stage reads of the previous pass's output with no barrier from the game can
/// change order). The rule and why: pass_barrier.h
void pass_start_barrier(VkCommandBuffer cmd);
/// Pass starts with the fragment-stage barrier, since the start
int64_t pass_barriers_relaxed();
/// (Triforce) The barrier a draw pass begun again after a part's split starts with (RX_VK_SPLIT_BARRIER, barrier_rules.h): the
/// attachments' outputs before the next draws' (what one pass orders); nothing right after a full barrier
void pass_resume_barrier(VkCommandBuffer cmd);
/// Passes begun again behind that barrier, since the start
int64_t pass_resumes();
/// (Triforce) RX_VK_EVENT_BARRIER (barrier_rules.h): an event's write held for the next full barrier on cmd, which records it right
/// after itself (event_writes_record), as work: the next full barrier is not left out after it
void event_writes_hold(VkCommandBuffer cmd);
/// Whether writes are held (for cmd; VK_NULL_HANDLE: for any command buffer)
bool event_writes_held(VkCommandBuffer cmd);
/// The writes held for cmd recorded now, behind a full barrier (before a recording's end: its write-backs copy the events' words to
/// the game's memory; before a second write of a held word). Nothing when none is held
void event_writes_flush(VkCommandBuffer cmd, const char* why);
/// (vk_replay.cpp) The held writes recorded into cmd, by full_barrier right after itself. Takes no lock (their buffers were bound at
/// their signals): a full barrier may be recorded under the pools' lock (vk_memory.cpp transfer_barrier)
void event_writes_record(VkCommandBuffer cmd);
void texture_stats(int64_t* uploads, int64_t* astc_gpu, int64_t* bytes_uploaded);
/// (hookshot) whether texture_read_texels' walk of r at that row stride (0: the region's own) and its own image stride
/// stays within avail bytes of the data it is given (overflow-checked: the stride is the game's)
bool texels_fit(const nvn_tex* t, const NVNcopyRegion* r, uint64_t row, uint64_t avail);
/// RX_VK_DUMP_TEX: the textures uploaded next written to that folder as PPM (diagnostics)
void texture_read_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* dst,
                         uint64_t row, uint64_t img, bool write);

} // namespace mkwvk
