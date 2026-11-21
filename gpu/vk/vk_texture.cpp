// Textures: one VkImage per texture storage (vk_texture.h). After totk-pc-recomp's runtime/vk_texture.c (MIT,
// Copyright (c) 2026 the totk-pc-recomp authors): the cache keyed by storage address plus descriptor, uploads on first
// use and when page write generations show the storage written, the GPU-owned rule (an image the GPU wrote is the
// content), every image in GENERAL layout, deferred destruction. The GPU ASTC decode is Eden's
// (video_core/host_shaders/astc_decoder.comp and renderer_vulkan/vk_compute_pass.cpp ASTCDecoderPass, GPL), the CPU
// unswizzle suyu's (video_core/textures/decoders.cpp, with the Switch 2 GOB swizzle).
//
// Formats share images by class: an image is made in the format first asked for (sRGB as its UNORM twin), with
// MUTABLE_FORMAT, so that a texture rendered as RGBA8 and sampled as RGBA8_SRGB, or written as R32UI and read as
// RGBA8, is one image. A class is the bytes per block for uncompressed colour, the format family for BC and ASTC,
// the format itself for depth / stencil.
#include <algorithm>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#if defined(__linux__)
#include <malloc.h>
#endif
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "vk_texture.h"
#include "vk_menu.h"
#include "vk_draw.h"
#include "vk_gprof.h"
#include "vk_dump.h"
#include "vk_jobs.h"

#include <span>

#include "video_core/textures/astc.h"
#include "video_core/textures/decoders.h"

#include "host_shaders/astc_decoder_comp_spv.h"
#include "host_shaders/bc_encoder_comp_spv.h"
#include "bc_levels.h"
#include "astc_bc_policy.h"
#include "astc_opaque.h"
#include "tex_bc1.h"
#include "touch_quick.h"
#include "tex_warm.h"      // (Triforce: textures made and uploaded ahead in light frames, texture_warm_step)
#include "tex_detail.h"
#include "vram_compact.h"
#include "texture_gone.h"
#include "keep_rendered.h"
#include "present_rate.h"
#include "tex_pressure.h"   // ((Triforce) the texture budget by live VRAM use, after NX2navi's)
#include "present_hold.h"   // ((Triforce) the hold of the last picture: what the draws may leave for the next frame)
#include "layer_split.h"    // ((Triforce) a new layered ASTC texture's first upload in parts)
#include "upload_torn.h"    // ((Triforce, wp18-flash) a re-upload of storage the CPU was rewriting at its submit: torn_later)
#include "target_overlap.h"
#include "reupload_early.h"   // ((Triforce) the re-uploads made before a parallel replay's segments)
#include "upload_earliest.h"   // ((Triforce) a parallel replay's upload into the earliest segment still recorded)
#include "pass_barrier.h"   // ((Triforce) the rule of the barrier a draw pass starts with)
#include "redrawn.h"   // ((Triforce) whether a target is drawn again frame after frame: the new-texture budget's rule)
#include "host_shaders/block_linear_unswizzle_2d_comp_spv.h"
#include "shaders/swizzle_back_comp_spv.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
namespace {

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

// ---- formats ----
std::mutex fmt_mu;
std::unordered_map<int, VkFormatProperties> fmt_cache;
VkFormatProperties format_props(VkFormat f) {
    std::lock_guard lk(fmt_mu);
    auto it = fmt_cache.find((int)f);
    if (it != fmt_cache.end()) return it->second;
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, f, &p);
    fmt_cache[(int)f] = p;
    return p;
}
bool is_bc(VkFormat f) {
    return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK;
}
bool is_astc(VkFormat f) {
    return f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
}
/// The UNORM twin of an sRGB format (the image's format: storage writes and every view format of the class)
VkFormat linear_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
    case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
    default:
        if (is_astc(f) && ((f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1)) return (VkFormat)(f - 1);
        return f;
    }
}
/// 3-component formats are not image formats on NVIDIA: the image holds 4 (alpha one), the upload widens them
VkFormat widened(VkFormat f, int* from_bpb) {
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R32G32B32_SFLOAT: *from_bpb = 12; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VK_FORMAT_R32G32B32_UINT: *from_bpb = 12; return VK_FORMAT_R32G32B32A32_UINT;
    case VK_FORMAT_R32G32B32_SINT: *from_bpb = 12; return VK_FORMAT_R32G32B32A32_SINT;
    default: *from_bpb = 0; return f;
    }
}
/// A storage-capable view format of the image's class (sRGB views cannot be storage)
VkFormat storage_view_format(VkFormat f) {
    f = linear_twin(f);
    if (format_props(f).optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) return f;
    switch (f) {   // (same class, a format storage can be)
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R32_UINT;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R32G32_UINT;
    case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16_SNORM: return VK_FORMAT_R16_UINT;
    case VK_FORMAT_R8_SNORM: return VK_FORMAT_R8_UINT;
    // (compressed: one block a texel, written raw by the game's encoders)
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK: return VK_FORMAT_R32G32_UINT;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK: case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_R32G32B32A32_UINT;
    default: return f;
    }
}
/// The class an image format shares images by (see the top of the file)
uint32_t format_class(VkFormat vf, int nvn_format, int bpb, bool astc_decoded) {
    if (astc_decoded) return 0x30000u | (uint32_t)(nvn_format - 0x79) % 14;
    if (is_depth_format(vf)) return 0x10000u | (uint32_t)vf;
    if (is_bc(vf) || is_astc(vf)) return 0x20000u | (uint32_t)linear_twin(vf);
    int from = 0;
    widened(vf, &from);
    return 0x40000u | (uint32_t)bpb | (from ? 0x100u : 0u);
}

VkComponentSwizzle component(unsigned s) {
    switch (s) {
    case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    case NVN_TEXTURE_SWIZZLE_A: return VK_COMPONENT_SWIZZLE_A;
    default: return VK_COMPONENT_SWIZZLE_IDENTITY;
    }
}

// ---- the cache ----
struct Key {
    uint64_t gpu;
    uint32_t target, width, height, depth, levels, samples, layout, cls;
    bool operator==(const Key& o) const {
        return gpu == o.gpu && target == o.target && width == o.width && height == o.height && depth == o.depth &&
               levels == o.levels && samples == o.samples && layout == o.layout && cls == o.cls;
    }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        uint64_t h = mix(k.gpu, k.target);
        h = mix(h, (uint64_t)k.width << 32 | k.height);
        h = mix(h, (uint64_t)k.depth << 32 | k.levels);
        h = mix(h, (uint64_t)k.samples << 40 | (uint64_t)k.layout << 20 | k.cls);
        return (size_t)h;
    }
};
std::unordered_map<Key, std::unique_ptr<Texture>, KeyHash> textures;
// (Triforce, texture detail: the keys made whole whatever the card's detail level, for the session, and the textures asked
// whole since the last sweep: texture_want_whole)
std::unordered_set<Key, KeyHash> whole_keys;
std::vector<Texture*> whole_pending;
std::unordered_multimap<uint64_t, Texture*> by_gpu;
uint64_t frame_no = 1;
present_rate::Clock tex_rate;   // (Triforce: the presents a second of the last seconds, texture_frame; texture_sweep's waits go by it)
uint64_t total_bytes = 0;

// (RX_VK_LOG_REMADE=1) Textures evicted and made again within 600 frames, and those of them that were then sampled
// empty (storage the GPU rendered, not uploaded): what the eviction's minimum age below is measured by
struct Evicted {
    uint64_t frame;
    bool rendered;
};
std::mutex evicted_mu;
std::unordered_map<uint64_t, Evicted> evicted_at;   // (storage, size and format -> when it went)
std::unordered_map<const Texture*, uint64_t> remade;   // (made again: how many frames after it went)
uint64_t evicted_key(uint64_t gpu, uint32_t w, uint32_t h, uint32_t fmt) {
    return gpu ^ ((uint64_t)w << 48) ^ ((uint64_t)h << 32) ^ ((uint64_t)fmt << 20);
}
bool remade_log_on() {
    static const bool on = getenv("RX_VK_LOG_REMADE") && atoi(getenv("RX_VK_LOG_REMADE")) != 0;
    return on;
}
// (hookshot) Textures uploaded from the game's memory (never drawn by the GPU) that the budget evicted: storage, size and format.
// Made again, such a texture is uploaded from that memory whatever ranges render targets once took over it (upload's "storage
// the GPU rendered" skip below). Skipped, its new image was never filled: whatever the VRAM held before, random-colour blocks on
// characters, shop shelves, floors, which spread over play time on 4-8 GB cards (the ones that evict) and changed each time a
// menu was opened and closed (issue 993). RX_VK_REMADE_SKIP=1: as before
std::mutex from_memory_mu;
std::unordered_map<uint64_t, uint64_t> evicted_from_memory;   // (evicted_key -> guest size)
std::atomic<uint64_t> st_remade_uploaded{0};
bool remade_skip_old() {
    static const bool on = getenv("RX_VK_REMADE_SKIP") && atoi(getenv("RX_VK_REMADE_SKIP")) != 0;
    return on;
}
// ((Triforce, after NX2navi's) The textures the live line let go, and those of them made again within 600 presents: while half or
// more come back the line stands aside a while (tex_pressure.h ComeBack; texture_sweep notes, create() finds, texture_frame
// judges). RX_VK_TEX_COME_BACK=0: no pause)
std::mutex come_back_mu;
tex_pressure::ComeBack come_back;
std::atomic<bool> come_back_noted{false};   // (anything noted: create() looks only then)
bool come_back_on() {
    static const bool on = !(getenv("RX_VK_TEX_COME_BACK") && !atoi(getenv("RX_VK_TEX_COME_BACK")));
    return on;
}

struct Dead {
    uint64_t value;
    VkImage image;
    VmaAllocation allocation;
    std::vector<VkImageView> views;
    std::vector<VkDeviceMemory> memory;   // (a sparse image's)
    VkBuffer buffer = VK_NULL_HANDLE;     // (the BC3 scratch's output buffer: given back with its image)
    VmaAllocation buffer_alloc = nullptr;
    uint64_t pool_key = 0;                // (a render target's image: to the pool below, with its views, not destroyed)
    std::unordered_map<uint64_t, VkImageView> view_map;
    uint64_t bytes = 0;
};
std::deque<Dead> dead;

// ---- (hookshot) Render target images, reused. OoT makes a frame's transient targets in a scratch heap whose offsets shift with
// what came before them in the frame (the dead targets, texture_frame): each shifted one was a texture of its own with a new image,
// and every 60 presents the sweep let go of the dead ones, 300-700 at once, destroyed in the next present: 20-50 ms on the thread
// replaying, once a second while the camera moves (RTX 5070, Kokiri Forest: 10,500 images made and destroyed in 4.5 minutes,
// 17 GB). Now a render target's image whose texture is gone waits in a pool by its create info, views and all, and a new texture
// of the same create info takes it; it is put into GENERAL from UNDEFINED and cleared as a new one is (create), so nothing it held
// shows. Only images made for a target (texture_get's !for_sampling) are pooled. The pool is bounded (RX_VK_TARGET_POOL_MB, default
// 256; 0: none, every image destroyed as before; and none while VRAM is near what the driver allows), and an image unused in it for
// 120 presents is destroyed, at most 16 a present ----
struct Pooled {
    VkImage image;
    VmaAllocation allocation;
    std::unordered_map<uint64_t, VkImageView> views;
    uint64_t bytes, since;
};
std::mutex pool_mu;
std::unordered_map<uint64_t, std::vector<Pooled>> target_pool;   // by pool_key_of
uint64_t pool_bytes, pool_n, st_pool_reused, st_pool_made, st_pool_destroyed;
bool pool_pressed;   // (VRAM near what the driver allows: texture_frame's budget)
bool compact_on() {   // (hookshot, RX_VK_COMPACT=1: the textures in a pool of their own, compacted: compact_step)
    // (Triforce) on by default on NVIDIA cards (RX_VK_COMPACT=0: off; =1: on, any card). Measured there: an RTX 4070 SUPER
    // played with it since 2026-11-10 (a 584,000-present session, 132 compactions, no fault; 85 -> 80.6 fps after a six-area
    // tour against 81 -> 64 without), and with the allowance held to 7300 MB (RX_VK_TEST_ALLOWED_MB, an 8 GB card's) a tour of
    // Hyrule Field, Kakariko, Lake Hylia and Kokiri peaked at 7165 MB of VRAM against 8565 without (over the allowance in 2 of
    // 12 reports against 11) at 69 fps against ~64. On an 8 GB card what is over the allowance is paged to system memory, and
    // the textures and render targets evicted to get back under it are made and uploaded again, 58 to 190 ms a frame (an
    // RX 5700 XT's and an RTX 3060's logs, 2026-11-11). Not measured on AMD or Intel: as it was there until someone does
    // (Triforce, later that day) and on trial on the other makes' cards: memory_trial_on (vk_memory.cpp), which turns it off by
    // itself on a PC where a session with it loses the GPU or two end in a crash
    static const bool v = [] {
        if (const char* e = getenv("RX_VK_COMPACT")) return atoi(e) != 0;
        const bool on = memory_trial_on();
        if (on) logf("vk: the textures' VRAM is compacted (RX_VK_COMPACT=0: off)");
        return on;
    }();
    return v;
}
uint64_t pool_cap() {
    static const uint64_t mb = getenv("RX_VK_TARGET_POOL_MB") ? (uint64_t)std::max(0, atoi(getenv("RX_VK_TARGET_POOL_MB"))) : 256;
    return mb << 20;
}
uint64_t pool_key_of(const VkImageCreateInfo& ci) {
    uint64_t k = mix((uint64_t)ci.flags, (uint64_t)ci.imageType << 32 | (uint64_t)ci.format);
    k = mix(k, (uint64_t)ci.extent.width << 32 | ci.extent.height);
    k = mix(k, (uint64_t)ci.extent.depth << 32 | ci.mipLevels);
    k = mix(k, (uint64_t)ci.arrayLayers << 32 | (uint64_t)ci.samples);
    k = mix(k, (uint64_t)ci.usage << 8 | (uint64_t)ci.tiling);
    return k | 1;   // (0: not pooled)
}
bool pool_take(uint64_t key, VkImage& image, VmaAllocation& allocation, std::unordered_map<uint64_t, VkImageView>& views) {
    std::lock_guard lk(pool_mu);
    auto it = target_pool.find(key);
    if (it == target_pool.end() || it->second.empty()) return false;
    Pooled& p = it->second.back();
    image = p.image, allocation = p.allocation, views = std::move(p.views);
    pool_bytes -= std::min(pool_bytes, p.bytes), pool_n--;
    it->second.pop_back();
    if (it->second.empty()) target_pool.erase(it);
    st_pool_reused++;
    return true;
}
void pool_destroy(Pooled& p) {
    for (auto& kv : p.views)
        if (kv.second) vkDestroyImageView(dev.device, kv.second, nullptr);
    destroy_image(p.image, p.allocation);
    st_pool_destroyed++;
}
/// (texture_frame) a dead render target's image the GPU is done with, into the pool
void pool_put(Dead& d) {
    std::lock_guard lk(pool_mu);
    target_pool[d.pool_key].push_back(Pooled{d.image, d.allocation, std::move(d.view_map), d.bytes, frame_no});
    pool_bytes += d.bytes, pool_n++;
}
/// (texture_frame, each present) the pool over its bound, or all of it while VRAM is short, and what waits in it unused for 120
/// presents: destroyed, at most 16 a present (64 while VRAM is short). Returns how many
size_t pool_trim() {
    std::lock_guard lk(pool_mu);
    // (Triforce, 11-11: with a floor of blocks kept (target_floor_kept) the pool is not emptied while VRAM is short: what it gave
    // back went into blocks the floor keeps, the driver's count did not move, and every target of the next frames was made new)
    const bool give_back = pool_pressed && !target_floor_kept();
    const uint64_t cap = give_back ? 0 : pool_cap();
    const size_t most = give_back ? 64 : 16;
    size_t n = 0;
    for (auto it = target_pool.begin(); it != target_pool.end() && n < most;) {
        auto& v = it->second;
        for (size_t i = 0; i < v.size() && n < most;) {   // (the oldest of a shape first: the front)
            if (pool_bytes <= cap && frame_no - v[i].since <= 120) {
                ++i;
                continue;
            }
            pool_destroy(v[i]);
            pool_bytes -= std::min(pool_bytes, v[i].bytes), pool_n--, n++;
            v.erase(v.begin() + (ptrdiff_t)i);
        }
        it = v.empty() ? target_pool.erase(it) : std::next(it);
    }
    return n;
}

// ---- the recording's upload command buffer ----
thread_local VkCommandBuffer up_cmd = VK_NULL_HANDLE;   // (each recording thread's: a parallel replay's segments
uint64_t rec_value = 0;                                   // record their own uploads)
thread_local bool up_used = false;
thread_local uint64_t tl_seg = 0;
uint32_t epoch = 0;
int64_t st_uploads, st_astc, st_bytes;

VkCommandBuffer upload_cmd() {
    if (!up_used) {
        up_used = true;
        full_barrier(up_cmd);   // (after the work submitted before: it may still read what is replaced)
        checkpoint_mark(up_cmd, rec_value, 'v');   // ((Triforce) RX_VK_CHECKPOINTS: the recording's uploads begun, checkpoint_kinds.h)
    }
    return up_cmd;
}

// ---- (parallel replay) a new image's first layout change, in queue order ----
// The segments' workers record at once, and the one that makes an image first need not be the first in the queue to use
// it: a later segment's worker made it (its UNDEFINED -> GENERAL change in its own uploads, submitted after the earlier
// segment), the earlier one used it (a copy into it again, the BC3 encoder's dispatch: its uploads, at the start of its
// command buffers), and the queue reached that use with the image UNDEFINED (VUID-vkCmdDraw-None-09600, RX_VK_PARALLEL
// only). The change goes to the init command buffer of the earliest segment still being recorded instead: those before
// it were done before the image was made, so never used it, and each segment's init is submitted before its uploads.
struct SegInit {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool used = false;   // (mu)
    // (set under mu; once false, false for the rest of the submission: a segment ended stays ended)
    std::atomic<bool> open{false};
    // ((Triforce, 11-17) its own lock, held while something is recorded into cmd and by its end (texture_segment_end): a
    // segment's end waits only for what is recorded into its own init, not for the others'. It was one lock for all of them,
    // and an upload recorded into one init (UploadRedirect, below: several ms for a big texture) held up every segment's end)
    std::mutex mu;
};
// (the submission's segments, in queue order: made by texture_segments_begin before the workers start, let go after they are
// all done)
std::vector<SegInit> seg_inits;
thread_local int tl_seg_index = -1;   // (the segment this thread records; -1: none)
// ((Triforce, 11-17) an UploadRedirect, below, lives on this thread: up_cmd is an earlier segment's init command buffer already,
// that segment's lock held. An image made meanwhile (by the upload, the BC3 scratch's) has its first layout change recorded
// there, which is where InitRedirect would put it: the earliest open segment, no later than this one)
thread_local bool tl_up_redirect = false;
/// ((Triforce, 11-17) The earliest segment from 0 to `last` still being recorded, its lock taken into `lk`; null when there is
/// none (no lock held). Each is looked at alone, but a segment once ended stays ended: those before the one found had all
/// ended when it was locked, as one look at all of them under one lock would have found
SegInit* seg_init_lock(int last, std::unique_lock<std::mutex>& lk) {
    for (;;) {
        const int i = upload_earliest::target([](int k) { return seg_inits[k].open.load(std::memory_order_acquire); },
                                              (int)seg_inits.size(), last + 1);
        if (i < 0) return nullptr;
        lk = std::unique_lock<std::mutex>(seg_inits[i].mu);
        if (seg_inits[i].open.load(std::memory_order_relaxed)) return &seg_inits[i];
        lk.unlock();   // (it ended meanwhile: the next one)
    }
}
/// While it lives, upload_cmd() is the earliest open segment's init command buffer (a segment's worker only)
struct InitRedirect {
    std::unique_lock<std::mutex> lk;
    SegInit* to = nullptr;
    VkCommandBuffer saved_cmd = VK_NULL_HANDLE;
    bool saved_used = false;
    explicit InitRedirect(bool want = true) {
        if (!want || tl_seg_index < 0 || tl_up_redirect) return;
        to = seg_init_lock(tl_seg_index, lk);
        if (!to) return;   // (none open: its own is, while it records)
        saved_cmd = up_cmd, saved_used = up_used;
        up_cmd = to->cmd, up_used = to->used;
    }
    void done() {
        if (to) {
            to->used = up_used;
            up_cmd = saved_cmd, up_used = saved_used;
            to = nullptr;
        }
        if (lk.owns_lock()) lk.unlock();
    }
    ~InitRedirect() { done(); }
    InitRedirect(const InitRedirect&) = delete;
    InitRedirect& operator=(const InitRedirect&) = delete;
};

// ---- (Triforce, 11-17) a parallel replay's upload, into the earliest segment still being recorded (upload_earliest.h) ----
std::atomic<int64_t> st_up_earliest_n{0}, st_up_earliest_bytes{0};   // (uploads so recorded, and the bytes they asked for)
std::atomic<int64_t> st_seg_end_waits{0}, st_seg_end_wait_ns{0};     // (texture_segment_end's waits for its segment's init lock)
// (the segments' ends waiting for it now: tests/parallel_init_order_test.cpp's upload_close_race lets the upload go on then)
std::atomic<int> st_seg_end_waiting{0};
/// RX_VK_UPLOAD_EARLIEST=0: a segment's uploads in its own upload command buffer, and an earlier segment's twin, as before
bool upload_earliest_on() {
    static const bool v = !getenv("RX_VK_UPLOAD_EARLIEST") || atoi(getenv("RX_VK_UPLOAD_EARLIEST")) != 0;
    return v;
}
/// While it lives, upload_cmd() is the init command buffer of the earliest segment before this one still being recorded, if
/// there is one (that segment's lock held: it cannot end meanwhile, and no other segment's end waits for it); `order` is the
/// segment the upload is in (upload_seg)
struct UploadRedirect {
    std::unique_lock<std::mutex> lk;
    SegInit* to = nullptr;
    uint64_t order;
    VkCommandBuffer saved_cmd = VK_NULL_HANDLE;
    bool saved_used = false;
    explicit UploadRedirect(bool want) : order(tl_seg) {
        if (!want || tl_seg_index < 0 || tl_up_redirect) return;
        to = seg_init_lock(tl_seg_index - 1, lk);
        if (!to) return;   // (this segment is the earliest still recorded: its own uploads, no lock held)
        order = upload_earliest::order_of(tl_seg, tl_seg_index, (int)(to - seg_inits.data()));
        saved_cmd = up_cmd, saved_used = up_used;
        // (its first use begins with upload_cmd's full barrier, whoever records into it first: InitRedirect or this)
        up_cmd = to->cmd, up_used = to->used;
        tl_up_redirect = true;
    }
    void done() {
        if (to) {
            to->used = up_used;
            up_cmd = saved_cmd, up_used = saved_used;
            to = nullptr;
            tl_up_redirect = false;
        }
        if (lk.owns_lock()) lk.unlock();
    }
    ~UploadRedirect() { done(); }
    UploadRedirect(const UploadRedirect&) = delete;
    UploadRedirect& operator=(const UploadRedirect&) = delete;
};

// ---- staging: host-visible arenas, reused once the submission that filled them completed ----
struct Arena {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    uint8_t* mapped = nullptr;
    uint64_t size = 0, used = 0, value = 0;   // value: the last submission that reads it
    std::chrono::steady_clock::time_point used_at;   // (when it last staged something)
};
std::vector<Arena> arenas;
constexpr uint64_t kArena = 64ull << 20;

struct Staging {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t offset = 0;
    uint8_t* data = nullptr;
    VmaAllocation allocation = nullptr;
};
uint64_t completed_value = 0;
// (GPU writes into guest memory by buffers: GPU address -> (end, the submission that wrote it last))
std::map<uint64_t, std::pair<uint64_t, uint64_t>> gpu_wrote;
std::mutex gpu_wrote_mu;
}  // namespace
bool gpu_wrote_into(Texture& tex);
namespace {
Staging staging(uint64_t n, uint64_t align = 256) {
    // Separate slices may already be read by submitted parallel segments. A flush must not share their cache atom.
    align = std::max<uint64_t>(align, dev.props.limits.nonCoherentAtomSize);
    for (Arena& a : arenas) {   // the current one first, then any whose readers completed
        if (a.value == rec_value) {
            const uint64_t at = (a.used + align - 1) & ~(align - 1);
            if (at + n <= a.size) {
                a.used = at + n;
                a.used_at = std::chrono::steady_clock::now();
                return {a.buffer, at, a.mapped + at, a.allocation};
            }
        }
    }
    for (Arena& a : arenas) {
        if (a.value != rec_value && a.value <= completed_value && a.size >= n) {
            a.value = rec_value, a.used = n;
            a.used_at = std::chrono::steady_clock::now();
            return {a.buffer, 0, a.mapped, a.allocation};
        }
    }
    Arena a;
    void* m = nullptr;
    const auto make = [&](uint64_t size) {
        a.size = size, m = nullptr;
        if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, a.buffer,
                           a.allocation, &m))
            return false;
        if (m) return true;
        destroy_buffer(a.buffer, a.allocation);   // (made but not mapped: of no use here)
        a.buffer = VK_NULL_HANDLE, a.allocation = nullptr;
        return false;
    };
    const auto t_make = std::chrono::steady_clock::now();
    // (hookshot) and one of the size asked for when a 64 MB one cannot be made: without resizable BAR an arena is new system
    // memory the driver must pin. On a 16 GB laptop with the game's pools imported and RAM nearly gone, no 64 MB arena could
    // be made even for a 36 KB upload, the idle ones having been given back (texture_frame), and every upload failed until one
    // was free: BC3 textures drawn as noise (RX 5500M, 2.9 GB of RAM available)
    const uint64_t need = (n + 0xFFFFF) & ~0xFFFFFull;
    if (!make(std::max(kArena, need))) {
        const bool small = need < kArena && make(need);
        static std::atomic<int> told{0};
        if (told++ < 8)
            logf("vk: staging: no %llu MB arena could be made (%zu kept); %s", (unsigned long long)(kArena >> 20), arenas.size(),
                 small ? "one of the size asked for instead" : "the upload waits");
        if (!small) return {};
    }
    {   // (hookshot: what making a staging arena costs the replay thread: on a card with resizable BAR it is VRAM the driver hands
        // out, and a scene change made several right after the idle ones were freed)
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_make).count();
        static int slow_logged;
        if (ms > 5 && slow_logged++ < 24) logf("vk: a staging arena of %llu MB took %.1f ms to make", (unsigned long long)(a.size >> 20), ms);
    }
    a.mapped = (uint8_t*)m;
    a.value = rec_value, a.used = n;
    a.used_at = std::chrono::steady_clock::now();
    arenas.push_back(a);
    static int logged;
    if (logged++ < 4 || arenas.size() % 16 == 0)
        logf("vk: staging arenas: %zu (%llu MB in this one)", arenas.size(), (unsigned long long)(a.size >> 20));
    return {a.buffer, 0, a.mapped, a.allocation};
}

// ---- the GPU ASTC decoder (Eden's) ----
struct AstcPush {
    uint32_t block_dims[2], layer_stride, block_size, x_shift, block_height, block_height_mask, native_tiling;
};
VkDescriptorSetLayout astc_set = VK_NULL_HANDLE;
VkPipelineLayout astc_layout = VK_NULL_HANDLE;
VkPipeline astc_pipe = VK_NULL_HANDLE;
bool astc_ready() {
    static int state = 0;   // 0 untried, 1 ready, -1 unavailable
    if (state) return state > 0;
    state = -1;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR || getenv("RX_VK_ASTC_CPU")) return false;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &astc_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AstcPush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &astc_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &astc_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof ASTC_DECODER_COMP_SPV;
    mi.pCode = ASTC_DECODER_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = astc_layout;
    const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &astc_pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) return false;
    state = 1;
    logf("vk: ASTC is decoded on the GPU (Eden's decoder, Switch 2 tiling)");
    return true;
}

VkImageView make_view(Texture* tex, VkImageViewType type, VkFormat f, VkImageAspectFlags aspect, uint32_t level0,
                      uint32_t nlevels, uint32_t layer0, uint32_t nlayers, const VkComponentMapping& cm,
                      VkImageUsageFlags usage) {
    if (!usage || (usage & tex->usage) != usage) return VK_NULL_HANDLE;
    // (block-texel views of a compressed image: one level; several layers where the driver takes them)
    const bool block_view = is_bc(tex->format) && !is_bc(f);
    if (block_view && nlevels != 1) return VK_NULL_HANDLE;
    VkImageViewUsageCreateInfo uci{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    uci.usage = usage & tex->usage;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.pNext = &uci;
    vi.image = tex->image;
    vi.viewType = type;
    vi.format = f;
    vi.components = cm;
    vi.subresourceRange = {aspect, level0, nlevels, layer0, nlayers};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &vi, nullptr, &v) == VK_SUCCESS) return v;
    if (!block_view || nlayers == 1) return VK_NULL_HANDLE;
    // (Vulkan 1.1 says a block-texel view is of a single layer; some drivers hold to it)
    vi.subresourceRange.layerCount = 1;
    if (vi.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY) vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    if (vkCreateImageView(dev.device, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

// ---- big ASTC textures kept as BC3 (8 bits a texel, a quarter of RGBA8): decoded into a scratch image a few
// layers at a time, re-encoded by Eden's encoder (host_shaders/bc_encoder.comp: bounding box along the colours'
// covariance, inset; GPL, as the decoder) into a buffer, copied into the image. Decoded RGBA8 took 6 GB of VRAM at
// the title (the terrain's material arrays, 2048 x 2048 x 68, 1.45 GB each): more than the driver allowed, which
// paged VRAM out and evicted textures that were made and uploaded again. RX_VK_ASTC_RGBA8=1: all as RGBA8.
struct BcPush {
    uint32_t blocks_x, blocks_y, width, height, bc3, out_offset;
};
VkDescriptorSetLayout bc_set = VK_NULL_HANDLE;
VkPipelineLayout bc_layout = VK_NULL_HANDLE;
VkPipeline bc_pipe = VK_NULL_HANDLE;
constexpr uint32_t kBcDim = 2048, kBcLayers = 8;   // the scratch: the largest level a BC3 texture may have
VkImage bc_scratch = VK_NULL_HANDLE;
VmaAllocation bc_scratch_alloc = nullptr;
VkImageView bc_scratch_view = VK_NULL_HANDLE;
VkBuffer bc_out = VK_NULL_HANDLE;
VmaAllocation bc_out_alloc = nullptr;
bool bc_scratch_fresh = false;   // (its layout still to be set, in the next upload command buffer)
// (hookshot-amd) a second scratch for textures re-encoded at another size (uneven block levels on AMD): the decoded level
// is blitted into it, filtered, at the image's size, and encoded from there. Made when an upload of such a texture first
// needs it, given back with the first scratch.
VkImage bc_scratch2 = VK_NULL_HANDLE;
VmaAllocation bc_scratch2_alloc = nullptr;
VkImageView bc_scratch2_view = VK_NULL_HANDLE;
bool bc_scratch2_fresh = false;
// (the scratch and its buffer, 160 MB, are made when an upload first needs them and given back after RX_VK_BC_IDLE_S seconds
// (20) without one: a title or a load decodes hundreds of textures through them, steady gameplay few (the "given back" line says
// whether it did).
// 0: kept for good once made. They have memory objects of their own, so that freeing gives the VRAM back: VMA keeps the empty
// block of its image pool for the next images. RX_VK_BC_POOL=1: from the pool as before)
std::mutex bc_mu;
std::chrono::steady_clock::time_point bc_used_at;
uint64_t bc_last_value = 0;   // (the recording that last used them: they go once it is done)
std::atomic<bool> bc_have{false};
uint32_t bc_idle_s() {
    // (hookshot) 0 by default again, kept once made: given back after 20 s, the garbled/rainbow textures of 4-8 GB cards followed
    // (issue 993): every such log has the "given back" line, an RTX 3050 4 GB player's went away with RX_VK_BC_IDLE_S=0 and not with
    // RX_VK_NO_EVICT=1, and they began with v2.0.3, which brought this give-back. Its cost: 160-288 MB of VRAM held after the first
    // decode. RX_VK_BC_IDLE_S=<seconds>: given back as before
    static const uint32_t v = getenv("RX_VK_BC_IDLE_S") ? (uint32_t)atoi(getenv("RX_VK_BC_IDLE_S")) : 0;
    return v;
}
bool bc_scratch_make() {   // (bc_mu held)
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {kBcDim, kBcDim, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = kBcLayers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    static const bool pool = getenv("RX_VK_BC_POOL") != nullptr;
    if (!create_image(ii, bc_scratch, bc_scratch_alloc, !pool)) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = bc_scratch;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kBcLayers};
    const uint64_t out = (uint64_t)(kBcDim / 4) * (kBcDim / 4) * kBcLayers * 16;
    const VkBufferUsageFlags out_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const auto make_out = [&] {
        if (create_device_buffer(out, out_usage, bc_out, bc_out_alloc, !pool)) return true;
        // (hookshot) VRAM full, the process at its budget (a 4 GB card, the textures evicted after an alt-tab): in system
        // memory, as create_image puts the scratch image there. Only in VRAM, it could not be made, every BC3 texture made
        // again then failed, and they were drawn as coloured noise
        if (!create_buffer(out, out_usage, false, bc_out, bc_out_alloc, nullptr, !pool)) return false;
        static std::atomic<int> told{0};
        if (told++ < 4) logf("vk: VRAM full: the BC3 scratch's output buffer (%llu MB) in system memory", (unsigned long long)(out >> 20));
        return true;
    };
    if (vkCreateImageView(dev.device, &vi, nullptr, &bc_scratch_view) != VK_SUCCESS || !make_out()) {
        if (bc_scratch_view) vkDestroyImageView(dev.device, bc_scratch_view, nullptr);
        destroy_image(bc_scratch, bc_scratch_alloc);
        bc_scratch = VK_NULL_HANDLE, bc_scratch_view = VK_NULL_HANDLE;
        return false;
    }
    bc_scratch_fresh = true;
    bc_have = true;
    return true;
}

bool bc_ready() {
    static int state = 0;
    if (state) return state > 0;
    state = -1;
    if (!astc_ready() || getenv("RX_VK_ASTC_RGBA8") || !dev.features.textureCompressionBC) return false;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, VK_FORMAT_BC3_UNORM_BLOCK, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return false;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &bc_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BcPush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &bc_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &bc_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof BC_ENCODER_COMP_SPV;
    mi.pCode = BC_ENCODER_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = bc_layout;
    const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &bc_pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) return false;
    state = 1;
    logf("vk: big ASTC textures are kept as BC3 (re-encoded on the GPU; RX_VK_ASTC_RGBA8=1: as RGBA8)");
    return true;
}

/// (hookshot-amd) the second scratch (see bc_scratch2; bc_mu held)
bool bc_scratch2_make() {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {kBcDim, kBcDim, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = kBcLayers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    static const bool pool = getenv("RX_VK_BC_POOL") != nullptr;
    if (!create_image(ii, bc_scratch2, bc_scratch2_alloc, !pool)) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = bc_scratch2;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kBcLayers};
    if (vkCreateImageView(dev.device, &vi, nullptr, &bc_scratch2_view) != VK_SUCCESS) {
        destroy_image(bc_scratch2, bc_scratch2_alloc);
        bc_scratch2 = VK_NULL_HANDLE, bc_scratch2_view = VK_NULL_HANDLE;
        return false;
    }
    bc_scratch2_fresh = true;
    return true;
}

/// (hookshot-amd) scratch [0, w) x [0, h), layers [0, n), blitted (linear) into scratch2 at ow x oh, then encoded from there
void bc_encode_resized(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, uint32_t n, Texture& dst,
                       uint32_t level, uint32_t layer);

/// Encodes scratch layers [0, n) (a w x h region at its corner) and copies the blocks into dst's level and layers
void bc_encode_into(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t n, Texture& dst, uint32_t level,
                    uint32_t layer, int dx, int dy, VkImageView from = VK_NULL_HANDLE) {
    full_barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bc_pipe);
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, from ? from : bc_scratch_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi{bc_out, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    wr[0].dstBinding = 0, wr[0].descriptorCount = 1, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wr[0].pImageInfo = &ii;
    wr[1].dstBinding = 1, wr[1].descriptorCount = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr[1].pBufferInfo = &bi;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bc_layout, 0, 2, wr);
    descriptors_disturbed();
    // (Triforce: bc3 = 0 for an image kept as BC1: the encoder then writes the colour half alone, eight bytes a block, and
    // the copy below reads them as the BC1 image's blocks. tex_bc1.h)
    const BcPush pc{(w + 3) / 4, (h + 3) / 4, w, h, dst.bc1 ? 0u : 1u, 0};
    vkCmdPushConstants(cmd, bc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (pc.blocks_x + 7) / 8, (pc.blocks_y + 7) / 8, n);
    full_barrier(cmd);
    const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, n}, {dx, dy, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(cmd, bc_out, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(cmd);   // (the scratch and the buffer are the next part's)
}

void bc_encode_resized(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, uint32_t n, Texture& dst,
                       uint32_t level, uint32_t layer) {
    full_barrier(cmd);
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, n};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, n};
    bl.srcOffsets[1] = {(int32_t)w, (int32_t)h, 1};
    bl.dstOffsets[1] = {(int32_t)ow, (int32_t)oh, 1};
    vkCmdBlitImage(cmd, bc_scratch, VK_IMAGE_LAYOUT_GENERAL, bc_scratch2, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    bc_encode_into(cmd, ow, oh, n, dst, level, layer, 0, 0, bc_scratch2_view);
}

/// The scratch for the upload recording now: made if it is not there, and its layout set (false: it could not be made).
/// resized: the second scratch too (hookshot-amd)
bool bc_scratch_layout(VkCommandBuffer cmd, bool resized = false) {
    {
        std::lock_guard lk(bc_mu);
        if (!bc_have && !bc_scratch_make()) return false;
        if (resized && !bc_scratch2 && !bc_scratch2_make()) return false;
        bc_used_at = std::chrono::steady_clock::now();
        bc_last_value = rec_value;
    }
    // (a segment's worker: a fresh scratch's layout change in queue order, as a new image's)
    InitRedirect redirect((bc_scratch2_fresh && bc_scratch2) || bc_scratch_fresh);
    if (redirect.to) cmd = upload_cmd();
    if (bc_scratch2_fresh && bc_scratch2) {   // (hookshot-amd: the second scratch, once made)
        bc_scratch2_fresh = false;
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = bc_scratch2;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kBcLayers};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    if (!bc_scratch_fresh) return true;
    bc_scratch_fresh = false;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = bc_scratch;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kBcLayers};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    return true;
}
/// (every present: the scratch unused for RX_VK_BC_IDLE_S seconds goes with the images the GPU is done with)
void bc_scratch_idle() {
    if (!bc_have.load(std::memory_order_relaxed) || !bc_idle_s()) return;
    std::lock_guard lk(bc_mu);
    if (!bc_have || std::chrono::steady_clock::now() - bc_used_at < std::chrono::seconds(bc_idle_s())) return;
    Dead d{bc_last_value, bc_scratch, bc_scratch_alloc, {bc_scratch_view}, {}};
    d.buffer = bc_out, d.buffer_alloc = bc_out_alloc;
    dead.push_back(std::move(d));
    bc_scratch = VK_NULL_HANDLE, bc_scratch_alloc = nullptr, bc_scratch_view = VK_NULL_HANDLE, bc_out = VK_NULL_HANDLE, bc_out_alloc = nullptr;
    bc_have = false, bc_scratch_fresh = false;
    const bool second = bc_scratch2 != VK_NULL_HANDLE;
    if (second) {   // (hookshot-amd: the second scratch, 128 MB, with it)
        dead.push_back(Dead{bc_last_value, bc_scratch2, bc_scratch2_alloc, {bc_scratch2_view}, {}});
        bc_scratch2 = VK_NULL_HANDLE, bc_scratch2_alloc = nullptr, bc_scratch2_view = VK_NULL_HANDLE, bc_scratch2_fresh = false;
    }
    logf("vk: the BC3 scratch (%s) unused for %u s: given back", second ? "288 MB, both" : "160 MB", bc_idle_s());
}

/// The storage view of one level (every layer) the ASTC decoder writes
VkImageView astc_target_view(Texture* tex, uint32_t level) {
    const uint64_t key = mix(0xA57C, level);
    auto it = tex->views.find(key);
    if (it != tex->views.end()) return it->second;
    const VkImageView v = make_view(tex, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                    level, 1, 0, tex->layers, {}, VK_IMAGE_USAGE_STORAGE_BIT);
    tex->views[key] = v;
    return v;
}

// ---- uploads ----
struct LevelGeo {
    uint64_t offset;            // from the storage's start
    uint32_t w, h, d;           // texels
    uint32_t tw, th, td;        // blocks
    unsigned tile_h, tile_d;    // block height / depth (log2 GOBs)
    uint64_t swizzled;          // bytes of one layer of it in the storage
};
LevelGeo level_geo(const Texture& tex, uint32_t level) {   // (level: the image's; the storage's is level + skip)
    LevelGeo g{};
    const uint32_t gl = level + tex.skip;
    g.offset = host->tex_level_offset(&tex.d, gl);
    const uint32_t w0 = std::max(1u, tex.d.width);
    const uint32_t h0 = tex.type == VK_IMAGE_TYPE_1D ? 1 : std::max(1u, tex.d.height);
    const uint32_t d0 = tex.type == VK_IMAGE_TYPE_3D ? std::max(1u, tex.d.depth) : 1;
    g.w = std::max(1u, w0 >> gl);
    g.h = std::max(1u, h0 >> gl);
    g.d = std::max(1u, d0 >> gl);
    g.tw = (g.w + tex.block_w - 1) / tex.block_w;
    g.th = (g.h + tex.block_h - 1) / tex.block_h;
    g.td = g.d;
    host->tex_level_tiles(&tex.d, gl, &g.tile_h, &g.tile_d);
    const uint64_t gobs_x = ((uint64_t)g.tw * tex.bpb + 63) / 64;
    const uint64_t rows_y = (g.th + (8ull << g.tile_h) - 1) >> (3 + g.tile_h);
    const uint64_t slices = (g.td + (1ull << g.tile_d) - 1) >> g.tile_d;
    g.swizzled = gobs_x * rows_y * slices * (512ull << (g.tile_h + g.tile_d));
    return g;
}

/// The guest bytes [gpu, gpu + n) in host memory: in place when contiguous, else copied into scratch
const uint8_t* guest_bytes(uint64_t gpu, uint64_t n, std::vector<uint8_t>& scratch) {
    uint64_t avail = 0;
    const uint8_t* p = host->gpu_ptr(gpu, &avail);
    // (below 1 TB the GPU address is the guest's: the storage as it was at the submit being replayed, where the game has
    // rewritten it since (vk_memory.cpp, "texture storage in the capture"))
    if (p && avail >= n) return gpu < (1ull << 40) ? memory_as_submitted(gpu, p, n, scratch) : p;
    if (n >= (32u << 20)) {   // (a big sparse texture's pages, gathered on several threads: the course maps' 349 MB)
        scratch.resize(n);
        const uint32_t nt = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
        const uint64_t per = ((n + nt - 1) / nt + 0xFFFF) & ~0xFFFFull;
        std::vector<std::thread> pool;
        auto part = [&](uint32_t t) {
            const uint64_t a = (uint64_t)t * per, b = std::min(n, a + per);
            for (uint64_t at = a; at < b; at += 0x10000)
                if (!host->gpu_read(gpu + at, scratch.data() + at, std::min<uint64_t>(0x10000, b - at)))
                    std::memset(scratch.data() + at, 0, std::min<uint64_t>(0x10000, b - at));
        };
        for (uint32_t t = 1; t < nt; t++) pool.emplace_back(part, t);
        part(0);
        for (std::thread& t : pool) t.join();
        return scratch.data();
    }
    scratch.assign(n, 0);
    if (!host->gpu_read(gpu, scratch.data(), n)) {
        for (uint64_t at = 0; at < n; at += 0x10000) host->gpu_read(gpu + at, scratch.data() + at, std::min<uint64_t>(0x10000, n - at));
    }
    return scratch.data();
}

void widen_rows(uint8_t* dst, const uint8_t* src, size_t texels, int from, int to) {
    const int comp = from / 3;
    for (size_t i = 0; i < texels; i++) {
        std::memcpy(dst + i * to, src + i * from, from);
        uint8_t* a = dst + i * to + from;
        if (comp == 1) a[0] = 0xFF;
        else if (comp == 2) a[0] = 0x00, a[1] = 0x3C;   // (half 1.0; UNORM16 would want 0xFFFF: close enough for alpha)
        else { const float one = 1.0f; std::memcpy(a, &one, 4); }
    }
}

/// Byte ranges of a texture's storage, from its start (what the CPU wrote since its last upload)
using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;
bool overlaps(const Ranges* dirty, uint64_t a, uint64_t b) {
    if (!dirty) return true;
    for (const auto& r : *dirty)
        if (r.first < b && a < r.second) return true;
    return false;
}

/// The view of one level the ASTC decoder writes: every layer, or one (layer)
VkImageView astc_level_view(Texture* tex, uint32_t level, uint32_t layer) {
    if (layer == ~0u) return astc_target_view(tex, level);
    const uint64_t key = mix(mix(0xA57D, level), layer);
    auto it = tex->views.find(key);
    if (it != tex->views.end()) return it->second;
    const VkImageView v = make_view(tex, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                    level, 1, layer, 1, {}, VK_IMAGE_USAGE_STORAGE_BIT);
    tex->views[key] = v;
    return v;
}

/// ASTC decoded on the GPU (Eden's decoder): the layers dirty overlaps (all of them without dirty), each layer's
/// levels from the first one the image keeps (a texture that leaves out its top levels stages only the tail of
/// every layer's chain: Eden's AssembleInto does the same)
/// (hookshot) why the last upload_astc_gpu on this thread failed, for the log of a BC3 image left to be tried again
thread_local const char* astc_fail = nullptr;

// ---- (Triforce) opaque ASTC textures kept as BC1 instead of BC3: half the video memory, the same texels (tex_bc1.h says
// why they are the same; astc_opaque.h which blocks the decoder writes opaque). The Graphics tab's "Opaque textures as BC1"
// row as the options file had it at the start, Off unless set; RX_VK_TEX_BC1=0 or 1 over it. Only a texture kept as BC3
// today, not a resized one, and only one first met by a draw that samples it: its blocks in the game's memory are looked
// at when it is made, and it is made BC1 when all of them are opaque. Every upload into a BC1 image looks again, at the very
// bytes it stages, and content with alpha (the game wrote its memory anew) makes the texture BC3 for good, as a copy on the
// GPU into it does (texture_bc1_to_bc3): what a copy brings is not looked at. ----
static_assert(tex_bc1::kBc1 == VK_FORMAT_BC1_RGB_UNORM_BLOCK && tex_bc1::kBc1Srgb == VK_FORMAT_BC1_RGB_SRGB_BLOCK &&
              tex_bc1::kBc3 == VK_FORMAT_BC3_UNORM_BLOCK && tex_bc1::kBc3Srgb == VK_FORMAT_BC3_SRGB_BLOCK);
std::atomic<uint64_t> st_bc1_made{0}, st_bc1_saved{0}, st_bc1_back{0};
bool bc1_on() {
    static const bool on = [] {
        if (!tex_bc1::wanted(getenv("RX_VK_TEX_BC1"), tex_bc1_at_start)) return false;
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        const bool can = dev.features.textureCompressionBC &&
                         (format_props(VK_FORMAT_BC1_RGB_UNORM_BLOCK).optimalTilingFeatures & need) == need;
        logf(can ? "vk: opaque ASTC textures are kept as BC1, half of BC3 (the Graphics tab's row or RX_VK_TEX_BC1=0: as BC3)"
                 : "vk: opaque ASTC textures as BC1 was asked for: this GPU has no BC1 images to sample and copy into, as BC3");
        return can;
    }();
    return on;
}
/// Whether every block of one layer's levels, as upload_astc_gpu decodes them, is one the decoder writes opaque. `layer`:
/// the layer's bytes from its first kept level (tail0 into the layer), `tail` of them; `align`: upload_astc_gpu's
bool astc_levels_opaque(const Texture& tex, const uint8_t* layer, uint64_t tail0, uint64_t tail, uint64_t align) {
    static thread_local std::vector<uint8_t> lin;
    for (uint32_t level = 0; level < tex.levels; level++) {
        const LevelGeo g = level_geo(tex, level);
        const uint64_t off = g.offset - tail0;
        if (g.offset < tail0 || off >= tail || (off % align)) continue;   // (a level the upload leaves out, as it does)
        if (g.swizzled > tail - off) return false;                        // (its blocks do not all lie in the layer)
        // (the blocks in rows, by the unswizzle the CPU's ASTC path uses: the decoder's SwizzleOffset with the Switch 2
        // tiling reads block (x, y) from the same bytes)
        const uint64_t n = (uint64_t)g.tw * g.th;
        lin.resize(n * 16);
        Tegra::Texture::UnswizzleTexture(std::span<uint8_t>(lin.data(), n * 16), std::span<const uint8_t>(layer + off, g.swizzled),
                                         16, g.tw, g.th, 1, g.tile_h, g.tile_d, 1);
        if (!astc_opaque::all(lin.data(), n, (unsigned)tex.block_w, (unsigned)tex.block_h)) return false;
    }
    return true;
}
/// (create) Whether the texture's storage in the game's memory holds only opaque blocks now, every layer of it
bool astc_storage_opaque(const Texture& tex) {
    const uint64_t total = tex.guest_size;
    if (!total || tex.bpb != 16 || tex.type != VK_IMAGE_TYPE_2D) return false;
    const uint64_t layer_size = tex.layers > 1 ? tex.L.layer_size : std::max<uint64_t>(tex.L.layer_size, total);
    const uint64_t tail0 = level_geo(tex, 0).offset;
    if (!layer_size || tail0 >= layer_size || (tail0 & 15) || (layer_size & 15)) return false;
    const uint64_t tail = layer_size - tail0;
    const uint64_t align = std::max<uint64_t>(dev.props.limits.minStorageBufferOffsetAlignment, 4);
    std::vector<uint8_t> scratch;
    for (uint32_t layer = 0; layer < tex.layers; layer++) {
        const uint64_t at = (uint64_t)layer * layer_size + tail0;
        if (at >= total || total - at < tail) return false;
        if (!astc_levels_opaque(tex, guest_bytes(tex.gpu + at, tail, scratch), tail0, tail, align)) return false;
    }
    return true;
}
bool bc1_retype(Texture& x, const char* why);   // (below: a BC1 texture's image made again as BC3)
// ((Triforce, 11-16) below: a checked re-upload's copy, upload_same.h. upload_astc_gpu's two reads of a layer read it where it is
// under way, so that what is staged is what the check hashed; any read it does not cover marks it short and the hashes go)
const uint8_t* same_snap_read(const Texture& tex, uint64_t off, uint64_t n);

bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {
    astc_fail = nullptr;
    if (tex.type != VK_IMAGE_TYPE_2D || tex.samples != VK_SAMPLE_COUNT_1_BIT || !astc_ready()) {
        astc_fail = "not a 2D image the GPU decoder takes";
        return false;
    }
    const uint64_t layer_size = tex.layers > 1 ? tex.L.layer_size : std::max<uint64_t>(tex.L.layer_size, total);
    const uint64_t tail0 = level_geo(tex, 0).offset;
    if (!layer_size || tail0 >= layer_size || (tail0 & 15) || (layer_size & 15)) {
        astc_fail = "its layout";
        return false;
    }
    const uint64_t tail = layer_size - tail0;
    std::vector<uint32_t> layers;
    for (uint32_t layer = 0; layer < tex.layers; layer++)
        if (overlaps(dirty, (uint64_t)layer * layer_size + tail0, (uint64_t)(layer + 1) * layer_size)) layers.push_back(layer);
    if (layers.empty()) return true;
    const bool all = layers.size() == tex.layers;
    std::vector<uint8_t> scratch;
    // (Triforce) into an image kept as BC1: the layers' blocks are read once into a copy of their own, that copy is looked
    // at and that copy is staged, so the game writing its memory meanwhile cannot put other blocks into the image than the
    // ones found opaque. A block that is not (or a layer the storage does not hold whole: its missing bytes are zero
    // here, an error block to the decoder): the texture is BC3 from now on, and all of it is encoded again
    std::vector<uint8_t> own;
    if (tex.bc1) {
        own.resize(tail * layers.size());
        const uint64_t level_align = std::max<uint64_t>(dev.props.limits.minStorageBufferOffsetAlignment, 4);   // (align, below)
        bool opaque = true;
        for (size_t k = 0; k < layers.size(); k++) {
            const uint64_t at = (uint64_t)layers[k] * layer_size + tail0;
            if (at < total) {
                const uint64_t n = std::min(tail, total - at);
                if (const uint8_t* src = same_snap_read(tex, at, n))   // ((Triforce) a checked re-upload's copy: same_look)
                    std::memcpy(own.data() + k * tail, src, n);
                else
                    std::memcpy(own.data() + k * tail, guest_bytes(tex.gpu + at, n, scratch), n);
            }
            opaque = opaque && astc_levels_opaque(tex, own.data() + k * tail, tail0, tail, level_align);
        }
        if (!opaque) {
            if (!bc1_retype(tex, "its memory holds blocks with alpha now")) {
                astc_fail = "no memory for its BC3 image";
                return false;
            }
            return upload_astc_gpu(tex, total, nullptr);
        }
    }
    const Staging s = staging(tail * layers.size(), std::max<uint64_t>(256, dev.props.limits.minStorageBufferOffsetAlignment));
    if (!s.buffer) {
        astc_fail = "no staging memory for its ASTC blocks";
        return false;
    }
    for (size_t k = 0; k < layers.size() && !tex.bc1; k++) {
        const uint64_t at = (uint64_t)layers[k] * layer_size + tail0;
        if (at >= total) continue;
        const uint64_t n = std::min(tail, total - at);
        if (const uint8_t* src = same_snap_read(tex, at, n))   // ((Triforce) a checked re-upload's copy: same_look)
            std::memcpy(s.data + k * tail, src, n);
        else
            std::memcpy(s.data + k * tail, guest_bytes(tex.gpu + at, n, scratch), n);
    }
    if (tex.bc1) std::memcpy(s.data, own.data(), own.size());
    const uint64_t staged = tail * layers.size();
    flush_mapped_range(s.allocation, s.offset, staged);   // (encoded blocks read by the GPU decoder, not necessarily coherent)
    st_bytes += (int64_t)staged;
    VkCommandBuffer cmd = upload_cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_pipe);
    const uint64_t align = std::max<uint64_t>(dev.props.limits.minStorageBufferOffsetAlignment, 4);
    if (tex.bc) {   // (decoded into the scratch a run of consecutive layers at a time, re-encoded, copied in)
        if (!bc_scratch_layout(cmd, tex.bc_resized)) {
            astc_fail = "no memory for the BC3 scratch";
            return false;
        }
        for (uint32_t level = 0; level < tex.levels; level++) {
            const LevelGeo g = level_geo(tex, level);
            const uint64_t off = g.offset - tail0;
            if (g.offset < tail0 || off >= tail || (off % align)) continue;
            const uint32_t iw = std::max(1u, tex.width >> level), ih = std::max(1u, tex.height >> level);
            // (hookshot-amd: a resized BC3 image: the level decodes at the game's size, g.w x g.h, and is encoded at iw x ih)
            AstcPush pc{};
            pc.block_dims[0] = (uint32_t)tex.block_w, pc.block_dims[1] = (uint32_t)tex.block_h;
            pc.layer_stride = (uint32_t)tail;
            const uint32_t gobs_x = (g.tw * 16 + 63) >> 6;
            pc.x_shift = 9 + g.tile_h + g.tile_d;
            pc.block_size = gobs_x << pc.x_shift;
            pc.block_height = g.tile_h;
            pc.block_height_mask = (1u << g.tile_h) - 1;
            pc.native_tiling = 1;
            for (size_t k0 = 0; k0 < layers.size();) {
                size_t k1 = k0 + 1;
                // (one layer a run: with runs of up to kBcLayers, an array's layers past the run's first came out wrong in the
                // smaller levels, and Hyrule Field's ground, which samples the terrain's material arrays (1365 x 1365 x 90, ASTC
                // 8x8) at those levels from a distance, had its tiles outlined in dark lines (RX 6600: none with one layer a run,
                // nor with the arrays kept as RGBA8). RX_VK_BC_LAYERS=<n>: up to n a run)
                static const uint32_t run = getenv("RX_VK_BC_LAYERS") ? std::clamp<uint32_t>((uint32_t)atoi(getenv("RX_VK_BC_LAYERS")), 1u, kBcLayers) : 1u;
                while (k1 < layers.size() && k1 - k0 < run && layers[k1] == layers[k1 - 1] + 1) k1++;
                const uint32_t n = (uint32_t)(k1 - k0);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_pipe);
                const uint64_t from = k0 * tail + off;
                VkDescriptorBufferInfo bi{s.buffer, s.offset + from, staged - from};
                VkDescriptorImageInfo ii{VK_NULL_HANDLE, bc_scratch_view, VK_IMAGE_LAYOUT_GENERAL};
                VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
                w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[0].pBufferInfo = &bi;
                w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                w[1].pImageInfo = &ii;
                vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_layout, 0, 2, w);
                vkCmdPushConstants(cmd, astc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                vkCmdDispatch(cmd, (g.tw + 7) / 8, (g.th + 7) / 8, n);
                if (tex.bc_resized)
                    bc_encode_resized(cmd, g.w, g.h, iw, ih, n, tex, level, layers[k0]);
                else
                    bc_encode_into(cmd, iw, ih, n, tex, level, layers[k0], 0, 0);
                k0 = k1;
            }
        }
        st_astc++;
        return true;
    }
    for (uint32_t level = 0; level < tex.levels; level++) {
        const LevelGeo g = level_geo(tex, level);
        const uint64_t off = g.offset - tail0;
        if (g.offset < tail0 || off >= tail || (off % align)) continue;
        AstcPush pc{};
        pc.block_dims[0] = (uint32_t)tex.block_w, pc.block_dims[1] = (uint32_t)tex.block_h;
        pc.layer_stride = (uint32_t)tail;
        const uint32_t gobs_x = (g.tw * 16 + 63) >> 6;
        pc.x_shift = 9 + g.tile_h + g.tile_d;
        pc.block_size = gobs_x << pc.x_shift;
        pc.block_height = g.tile_h;
        pc.block_height_mask = (1u << g.tile_h) - 1;
        pc.native_tiling = 1;
        for (size_t k = 0; k < (all ? 1 : layers.size()); k++) {
            const VkImageView view = astc_level_view(&tex, level, all ? ~0u : layers[k]);
            if (!view) {
                astc_fail = "no view of a level";
                return false;
            }
            const uint64_t from = (all ? 0 : k * tail) + off;
            VkDescriptorBufferInfo bi{s.buffer, s.offset + from, staged - from};
            VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[0].pBufferInfo = &bi;
            w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[1].pImageInfo = &ii;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_layout, 0, 2, w);
            vkCmdPushConstants(cmd, astc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            vkCmdDispatch(cmd, (g.tw + 7) / 8, (g.th + 7) / 8, all ? tex.layers : 1);
        }
    }
    st_astc++;
    return true;
}

// ---- big block-linear textures unswizzled on the GPU (Eden's block_linear_unswizzle_2d.comp, GPL, with the Switch
// 2 tiling): the shader reads the storage where it is, in its imported pool, and writes the image. The course maps
// the title loads at each scene change (six of 349 MB) were copied into staging and unswizzled on the CPU: ~200 ms
// of the replay thread. RX_VK_GPU_UNSWIZZLE=0: on the CPU. ----
struct UnswizzlePush {
    uint32_t origin[3], pad0;
    int32_t destination[3];
    uint32_t bytes_per_block_log2, layer_stride, block_size, x_shift, block_height, block_height_mask, native_tiling;
};
VkDescriptorSetLayout unsw_set = VK_NULL_HANDLE;
VkPipelineLayout unsw_layout = VK_NULL_HANDLE;
VkPipeline unsw_pipe = VK_NULL_HANDLE;
bool unswizzle_ready() {
    static int state = 0;
    if (state) return state > 0;
    state = -1;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    if (getenv("RX_VK_GPU_UNSWIZZLE") && !atoi(getenv("RX_VK_GPU_UNSWIZZLE"))) return false;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &unsw_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UnswizzlePush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &unsw_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &unsw_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof BLOCK_LINEAR_UNSWIZZLE_2D_COMP_SPV;
    mi.pCode = BLOCK_LINEAR_UNSWIZZLE_2D_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = unsw_layout;
    const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &unsw_pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) return false;
    state = 1;
    logf("vk: big block-linear textures are unswizzled on the GPU from their pools (RX_VK_GPU_UNSWIZZLE=0: the CPU)");
    return true;
}

/// ((Triforce, 11-17) upload_unswizzle_gpu's checks before it reads anything (upload_reads_live's too): whether it takes the
/// texture, `vf` the format its view is written in)
bool unswizzle_gpu_takes(const Texture& tex, uint64_t total, VkFormat& vf) {
    if (tex.type != VK_IMAGE_TYPE_2D || tex.astc || tex.bc || !(tex.usage & VK_IMAGE_USAGE_STORAGE_BIT) ||
        is_bc(tex.format) || is_astc(tex.format) || is_depth_format(tex.format) || nvn_tex_is_linear(&tex.d))
        return false;
    if (total < (8u << 20) || tex.block_w != 1 || tex.block_h != 1) return false;
    int widen_from = 0;
    widened(tex.d.format ? vk_format(tex.nvn_format).format : tex.format, &widen_from);
    if (widen_from) return false;
    switch (tex.bpb) {
    case 1: vf = VK_FORMAT_R8_UINT; break;
    case 2: vf = VK_FORMAT_R16_UINT; break;
    case 4: vf = VK_FORMAT_R32_UINT; break;
    case 8: vf = VK_FORMAT_R32G32_UINT; break;
    case 16: vf = VK_FORMAT_R32G32B32A32_UINT; break;
    default: return false;
    }
    return unswizzle_ready();
}
/// The whole texture (every level and layer dirty overlaps) unswizzled on the GPU; false: not a texture for it
bool upload_unswizzle_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {
    VkFormat vf;
    if (!unswizzle_gpu_takes(tex, total, vf)) return false;
    const BufferRef src = gpu_buffer(tex.gpu, total);
    const uint64_t align = std::max<uint64_t>(dev.props.limits.minStorageBufferOffsetAlignment, 16);
    if (!src || (src.offset % align)) return false;
    for (uint32_t level = 0; level < tex.levels; level++) {   // (every level must be possible before any is done)
        const LevelGeo g = level_geo(tex, level);
        if (g.td != 1 || g.tile_d || (g.offset % align) || g.offset >= total) return false;
        // (the shader reads whole GOB-block rows: a level whose last one runs past the storage, which the CPU path
        // reads from a zero-padded copy, would read past the pool and fault the device)
        if (g.offset + (uint64_t)(tex.layers - 1) * tex.L.layer_size + g.swizzled > total) return false;
    }
    VkCommandBuffer cmd = upload_cmd();
    full_barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, unsw_pipe);
    for (uint32_t level = 0; level < tex.levels; level++) {
        const LevelGeo g = level_geo(tex, level);
        bool any = false;
        for (uint32_t layer = 0; layer < tex.layers && !any; layer++)
            any = overlaps(dirty, g.offset + (uint64_t)layer * tex.L.layer_size,
                           g.offset + (uint64_t)layer * tex.L.layer_size + g.swizzled);
        if (!any) continue;
        const uint64_t key = mix(0x5B1Cu, mix(level, (uint64_t)vf));
        VkImageView view;
        if (auto it = tex.views.find(key); it != tex.views.end()) view = it->second;
        else view = tex.views[key] = make_view(&tex, VK_IMAGE_VIEW_TYPE_2D_ARRAY, vf, VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0,
                                               tex.layers, {}, VK_IMAGE_USAGE_STORAGE_BIT);
        if (!view) return false;
        UnswizzlePush pc{};
        pc.bytes_per_block_log2 = (uint32_t)std::countr_zero((uint32_t)tex.bpb);
        pc.layer_stride = (uint32_t)tex.L.layer_size;
        const uint32_t gobs_x = (uint32_t)(((uint64_t)g.tw * tex.bpb + 63) >> 6);
        pc.x_shift = 9 + g.tile_h;
        pc.block_size = gobs_x << pc.x_shift;
        pc.block_height = g.tile_h;
        pc.block_height_mask = (1u << g.tile_h) - 1;
        pc.native_tiling = 1;
        VkDescriptorBufferInfo bi{src.buffer, src.offset + g.offset, total - g.offset};
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[0].pBufferInfo = &bi;
        w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &ii;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, unsw_layout, 0, 2, w);
        vkCmdPushConstants(cmd, unsw_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        checkpoint_mark(cmd, tex.gpu, 'g');
        vkCmdDispatch(cmd, (g.tw + 31) / 32, (g.th + 31) / 32, tex.layers);
        st_bytes += (int64_t)g.swizzled * tex.layers;
    }
    full_barrier(cmd);
    descriptors_disturbed();
    return true;
}

/// The texture from guest memory into its image: all of it, or (dirty) the layers, levels and block rows the CPU
/// wrote. Block-linear levels are rows of blocks a GOB-block high, each self-contained: a written page re-uploads
/// the block rows it falls in.
uint64_t st_made;          // (textures made)
int64_t st_unswizzle_bytes, st_unswizzle_ns;   // (the CPU's unswizzling)
std::map<uint64_t, std::pair<uint64_t, WriteGen>> gpu_owned;   // GPU address -> (end, memory generation then)
thread_local uint64_t tl_watch_prog;
/// (RX_VK_VERIFY_DIM=WxH: the guest memory of textures that size, hashed when uploaded and every 300 frames: a change
/// the write tracking did not report is logged as a missed write)
std::pair<uint32_t, uint32_t> verify_dim() {
    static const std::pair<uint32_t, uint32_t> d = [] {
        unsigned w = 0, h = 0;
        if (const char* e = getenv("RX_VK_VERIFY_DIM")) sscanf(e, "%ux%u", &w, &h);
        return std::pair<uint32_t, uint32_t>{w, h};
    }();
    return d;
}
std::unordered_map<uint32_t, std::vector<uint8_t>> verify_snap;   // (each verified texture's memory when uploaded)
uint64_t guest_hash(const Texture& tex, bool snapshot = false) {
    std::vector<uint8_t> b(tex.guest_size);
    if (!host->gpu_read(tex.gpu, b.data(), b.size())) return 0;
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
    if (snapshot) verify_snap[tex.id] = std::move(b);
    return h;
}
/// (a missed write: which 4 KB pages changed since the upload, and the CPU's next writes to them watched)
void verify_report(const Texture& tex) {
    const auto it = verify_snap.find(tex.id);
    if (it == verify_snap.end()) return;
    std::vector<uint8_t> now(tex.guest_size);
    if (!host->gpu_read(tex.gpu, now.data(), now.size())) return;
    std::string runs;
    int nruns = 0, watched = 0;
    uint64_t changed = 0;
    for (uint64_t off = 0; off < tex.guest_size; off += 4096) {
        const uint64_t n = std::min<uint64_t>(4096, tex.guest_size - off);
        if (!std::memcmp(now.data() + off, it->second.data() + off, n)) continue;
        changed++;
        if (nruns < 12) {
            char b[48];
            snprintf(b, sizeof b, " +%#llx", (unsigned long long)off);
            runs += b, nruns++;
        }
        static int watches;
        if (host->watch_guest_writes && watches < 4 && watched < 2) {
            const uint64_t g = host->gpu_guest(tex.gpu + off);
            if (g) host->watch_guest_writes(g, 4096), watches++, watched++;
        }
    }
    logf("vk: verify: texture %#llx: %llu pages changed since the upload:%s%s", (unsigned long long)tex.gpu,
         (unsigned long long)changed, runs.c_str(), watched ? " (the next CPU writes to two of them watched)" : "");
    {   // (the first changed page as the write tracking has it: ever written, written since the upload, and when)
        for (uint64_t off = 0; off < tex.guest_size; off += 4096) {
            const uint64_t n = std::min<uint64_t>(4096, tex.guest_size - off);
            if (!std::memcmp(now.data() + off, it->second.data() + off, n)) continue;
            const uint64_t g = host->gpu_guest(tex.gpu + off);
            std::vector<std::pair<uint64_t, uint64_t>> ever, since;
            memory_written_runs(g, 4096, WriteGen{}, ever);
            memory_written_runs(g, 4096, tex.upload_gen, since);
            logf("vk: verify: its page +%#llx (guest %#llx): stamped ever %d, since the upload (gen %llu) %d; generation now %llu",
                 (unsigned long long)off, (unsigned long long)g, (int)!ever.empty(), (unsigned long long)tex.upload_gen.v,
                 (int)!since.empty(), (unsigned long long)memory_generation().v);
            break;
        }
    }
    it->second = std::move(now);
}   // (the draw or dispatch the replay is at, for watch_tex)
/// (RX_VK_WATCH_TEX=addr[,addr...]: what happens to the textures over those GPU addresses, 600 lines)
void watch_tex(const Texture* tex, const char* what) {
    static std::vector<uint64_t> addrs = [] {
        std::vector<uint64_t> v;
        if (const char* e = getenv("RX_VK_WATCH_TEX"))
            for (const char* p = e; *p;) {
                char* end;
                const uint64_t a = strtoull(p, &end, 16);
                if (end == p) break;
                v.push_back(a);
                p = *end ? end + 1 : end;
            }
        return v;
    }();
    static const std::pair<uint32_t, uint32_t> dim = [] {
        unsigned w = 0, h = 0;
        if (const char* e = getenv("RX_VK_WATCH_DIM")) sscanf(e, "%ux%u", &w, &h);
        return std::pair<uint32_t, uint32_t>{w, h};
    }();
    static const std::pair<uint64_t, uint64_t> range = [] {   // (RX_VK_WATCH_RANGE=<lo>:<hi>: textures overlapping it)
        unsigned long long a = 0, b = 0;
        if (const char* e = getenv("RX_VK_WATCH_RANGE")) sscanf(e, "%llx:%llx", &a, &b);
        return std::pair<uint64_t, uint64_t>{a, b};
    }();
    if (tex && range.second && tex->gpu < range.second && tex->gpu + std::max<uint64_t>(tex->guest_size, 1) > range.first) {
        static int rlogged;
        if (rlogged++ < 600)
            logf("vk: watch range frame %llu prog %016llx %s #%u %#llx %ux%ux%u x%u levels %u NVN %#x target %d size %llu%s%s",
                 (unsigned long long)frame_no, (unsigned long long)tl_watch_prog, what, tex->id, (unsigned long long)tex->gpu,
                 tex->width, tex->height, tex->depth, tex->layers, tex->levels, tex->nvn_format, tex->d.target,
                 (unsigned long long)tex->guest_size, tex->gpu_written ? " rendered" : "", tex->uploaded ? " uploaded" : "");
    }
    if ((addrs.empty() && !dim.first) || !tex) return;
    {   // (RX_VK_DUMP_ON_WATCH=<frames>: a watched texture made after boot dumps the frames after it, once)
        static const int on_watch = getenv("RX_VK_DUMP_ON_WATCH") ? atoi(getenv("RX_VK_DUMP_ON_WATCH")) : 0;
        static bool requested = false;
        if (on_watch && !requested && frame_no > 1000 && !strcmp(what, "made")) {
            for (const uint64_t a : addrs)
                if (a == tex->gpu) {
                    requested = true;
                    dump_request((int64_t)frame_no + 1, on_watch);
                    logf("vk: watch: texture %#llx made at frame %llu: the next %d frames dumped", (unsigned long long)a,
                         (unsigned long long)frame_no, on_watch);
                }
        }
    }
    static int logged;
    static std::unordered_map<uint32_t, uint64_t> told;   // (a texture's events once a frame)
    const bool by_dim = dim.first && tex->width == dim.first && tex->height == dim.second;
    if (by_dim && !strcmp(what, "written")) {   // ("written" once a frame; the rest each time)
        uint64_t& f = told[tex->id];
        if (f == frame_no || logged >= 20000) return;
        f = frame_no;
    }
    const uint64_t hi = tex->gpu + std::max<uint64_t>(tex->guest_size, 1);
    for (const uint64_t a : by_dim ? std::vector<uint64_t>{tex->gpu} : addrs) {
        if (a < tex->gpu || a >= hi || logged >= (by_dim ? 20000 : 600)) continue;
        logged++;
        // (Triforce) one about to be uploaded from the game's memory: how much of that memory is not zero. A texture the game
        // expects the GPU's texels in (a tile's normal map made again from its tile cache) showed 0 here before bake_writeback_step
        char guest[64] = "";
        if (by_dim && !strcmp(what, "sampled, uploaded") && tex->guest_size && tex->guest_size <= (4u << 20)) {
            static thread_local std::vector<uint8_t> mem;
            mem.resize((size_t)tex->guest_size);
            uint64_t nz = 0;
            if (host->gpu_read(tex->gpu, mem.data(), mem.size()))
                for (uint8_t c : mem) nz += c != 0;
            snprintf(guest, sizeof guest, " memory: %llu of %llu bytes not zero", (unsigned long long)nz,
                     (unsigned long long)tex->guest_size);
        }
        logf("vk: watch tex frame %llu prog %016llx %s #%u %#llx %ux%ux%u x%u levels %u NVN %#x layout %#x class %#x target %d "
             "size %llu%s%s%s", (unsigned long long)frame_no, (unsigned long long)tl_watch_prog, what, tex->id, (unsigned long long)tex->gpu, tex->width,
             tex->height, tex->depth, tex->layers, tex->levels, tex->nvn_format, tex->key_layout, tex->key_cls,
             tex->d.target, (unsigned long long)tex->guest_size, tex->gpu_written ? " rendered" : "",
             tex->uploaded ? " uploaded" : "", guest);
        return;
    }
}
/// Whether [gpu, gpu + n) is storage the GPU's images rendered last (covered by ranges render targets took), and the
/// generation from which the CPU's writes would count (0: not the GPU's)
/// (hookshot) The ranges can overlap (texture_rendered drops only the ones inside a new range), so the bytes they cover are
/// counted once each: summed, two overlapping ranges counted their overlap twice and a range with a gap the GPU never drew
/// read as covered, and a texture over it was never uploaded
WriteGen gpu_owned_since(uint64_t gpu, uint64_t n) {
    uint64_t covered = 0, reach = gpu;   // (reach: the end of what is counted so far; the ranges come in address order)
    const WriteGen none{UINT64_MAX};
    WriteGen since = none;
    auto it = gpu_owned.upper_bound(gpu);
    if (it != gpu_owned.begin()) --it;
    for (; it != gpu_owned.end() && it->first < gpu + n; ++it) {
        const uint64_t a = std::max(it->first, gpu), b = std::min(it->second.first, gpu + n);
        if (b <= a) continue;
        since = std::min(since, it->second.second);
        if (b > std::max(a, reach)) covered += b - std::max(a, reach), reach = b;
    }
    return covered >= n && since != none ? std::max(since, WriteGen{1}) : WriteGen{};
}

// (hookshot, smoothness) What a frame's uploads cost by the way they went: when a frame's uploads take over 50 ms (the stutters at
// area loads and in walking, 180-360 ms frames of 250-450 MB: Discord and the frame trace, 2026-11-09), one log line of the routes
// (ASTC on the GPU, unswizzled on the GPU, on the CPU), their count, MB and ms, and the slowest textures
enum UpRoute { UP_ASTC, UP_GPU, UP_CPU, UP_COUNT };
thread_local int up_route = UP_CPU;
struct UpStat { int64_t n = 0, bytes = 0, ns = 0; };
UpStat up_frame[UP_COUNT];
struct UpSlow { int64_t ns; uint32_t w, h, layers, nvn; int route; uint64_t bytes; };
std::vector<UpSlow> up_slowest;
std::mutex up_mu;   // (uploads may come from a parallel replay's threads)
std::atomic<int64_t> frame_upload_ns{0};   // (this frame's time in upload(), every thread's)
std::atomic<int64_t> frame_upload_bytes{0};   // ((Triforce) and the bytes they were asked for: new_texture_later)
std::atomic<int> frame_astc_admitted{0};   // (Triforce: this frame's new ASTC textures asked about so far: upload_defer)
std::atomic<int64_t> st_deferred{0}, st_deferred_frame{0};
void upload_frame_report() {
    std::lock_guard lk(up_mu);
    int64_t ns = 0;
    for (const UpStat& u : up_frame) ns += u.ns;
    static int logged;
    if (ns >= 50000000 && logged < 300) {
        logged++;
        static const char* names[UP_COUNT] = {"ASTC on the GPU", "unswizzled on the GPU", "on the CPU"};
        char line[900];
        int k = snprintf(line, sizeof line, "vk: a frame's uploads took %.0f ms:", ns / 1e6);
        for (int r = 0; r < UP_COUNT; r++)
            if (up_frame[r].n)
                k += snprintf(line + k, sizeof line - k, " %s %lld (%.1f MB, %.0f ms);", names[r], (long long)up_frame[r].n,
                              up_frame[r].bytes / 1048576.0, up_frame[r].ns / 1e6);
        std::sort(up_slowest.begin(), up_slowest.end(), [](const UpSlow& a, const UpSlow& b) { return a.ns > b.ns; });
        for (size_t i = 0; i < up_slowest.size() && i < 3 && k < (int)sizeof line - 80; i++)
            k += snprintf(line + k, sizeof line - k, " [%ux%u x%u NVN %#x %s %.1f MB %.0f ms]", up_slowest[i].w, up_slowest[i].h,
                          up_slowest[i].layers, up_slowest[i].nvn, names[up_slowest[i].route], up_slowest[i].bytes / 1048576.0,
                          up_slowest[i].ns / 1e6);
        logf("%s", line);
    }
    for (UpStat& u : up_frame) u = {};
    up_slowest.clear();
}
// ---- (Triforce, 11-16) a re-upload checked against the bytes its image holds (upload_same.h): texture_touch asks same_look
// before the upload and same_done after it; upload_inner's CPU path reads the copy the check hashed (same_snap_read) ----
/// (an uploaded texture's storage joins the submit-time capture up to this size: RX_VK_TEX_CAPTURE_MAX_MB=<n>, 2 GB unless set;
/// texture_touch, after an upload and after a check that found nothing to upload)
uint64_t tex_capture_max() {
    static const uint64_t cap_max = getenv("RX_VK_TEX_CAPTURE_MAX_MB") ? (uint64_t)atoi(getenv("RX_VK_TEX_CAPTURE_MAX_MB")) << 20 : (2ull << 30);
    return cap_max;
}
/// RX_VK_UPLOAD_SAME=0: every re-upload made as before, its bytes not looked at
bool upload_same_on() {
    static const bool v = !getenv("RX_VK_UPLOAD_SAME") || atoi(getenv("RX_VK_UPLOAD_SAME")) != 0;
    return v;
}
/// (Triforce, 11-16) RX_VK_UPLOAD_SAME_ASTC=0: ASTC decoded on the GPU not checked, its re-uploads made as before. At the S2 Market
/// after a tour the game rewrote, with the bytes they had, ASTC textures decoded on the GPU too (1536 x 1536 ASTC 5x5 NVN 0x7b of
/// 11 levels, 2030 KB; 768 x 768 NVN 0x7e kept as BC3; 512 x 512), which went on to upload_astc_gpu unchecked: every layer a write
/// reached staged whole and decoded again, about 1.4 GB in 10 s, 90 fps down to 72 and the 1% low from 57 to 33
bool upload_same_astc_on() {
    static const bool v = !getenv("RX_VK_UPLOAD_SAME_ASTC") || atoi(getenv("RX_VK_UPLOAD_SAME_ASTC")) != 0;
    return v;
}
/// (Triforce) why a re-upload went unchecked, for the line of those (same_unchecked): same_look notes its own reasons in
/// tl_same_why, texture_touch those of a re-upload that is not a partial one
enum SameWhy {
    SAME_NONE,          // (checked, or nothing written: not counted)
    SAME_OFF,           // (RX_VK_UPLOAD_SAME=0)
    SAME_ASTC_OFF,      // (RX_VK_UPLOAD_SAME_ASTC=0)
    SAME_SIZE,          // (same_wanted: over kMaxBytes, or uncompressed and 8 MB or more)
    SAME_KIND,          // (same_wanted: its kind or layout; or no units)
    SAME_RESTING,       // (after kMisses checks that found nothing the same)
    SAME_GPU_DIRTY,     // (not partial: a GPU buffer wrote its storage)
    SAME_GPU_WRITTEN,   // (not partial: the GPU drew it)
    SAME_OWED_WHOLE,    // (not partial: owed whole)
    SAME_NO_PARTIAL,    // (not partial: RX_VK_NO_PARTIAL)
    SAME_TWIN,          // (the parallel replay's same data again in an earlier segment)
    SAME_CPU_OVER_GPU,  // (the CPU's write over the GPU's image)
    SAME_WHY_COUNT
};
thread_local int tl_same_why = SAME_NONE;
/// Whether a re-upload of tex is checked: one upload_inner makes on its CPU path, reading the storage through guest_bytes (not kept
/// as BC3 but from ASTC; block-compressed, or under 8 MB, so neither unswizzled on the GPU nor cleared as all zero), or (Triforce,
/// 11-16) ASTC of a 2D image decoded on the GPU (upload_astc_gpu, which reads the layers it stages through guest_bytes as well,
/// the copy where it is under way; kept as RGBA8, BC3 or BC1 alike: the decode, the encode and BC1's look at the blocks go by the
/// bytes alone. Its image's format is RGBA8 or BC: not one the 8 MB rule is for), of one sample and not a packed depth-stencil
/// (which no upload fills), not sparse, of upload_same::kMaxBytes or less, its storage in one piece of host memory (below 1 TB
/// read as submitted, as guest_bytes reads it, above it as it is). why: SAME_NONE, or which SameWhy refused it
bool same_wanted(const Texture& tex, int* why = nullptr) {
    int no = SAME_NONE;
    uint64_t avail = 0;
    if (tex.astc && tex.type == VK_IMAGE_TYPE_2D && !upload_same_astc_on())
        no = SAME_ASTC_OFF;
    else if ((tex.astc && tex.type != VK_IMAGE_TYPE_2D) || (tex.bc && !tex.astc) || tex.sparse ||
             tex.samples != VK_SAMPLE_COUNT_1_BIT || !tex.image || !tex.guest_size ||
             tex.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        no = SAME_KIND;
    else if (tex.guest_size > upload_same::kMaxBytes ||
             (!tex.astc && !is_bc(tex.format) && !is_astc(tex.format) && tex.guest_size >= (8u << 20)))
        no = SAME_SIZE;
    else if (!host->gpu_ptr(tex.gpu, &avail) || avail < tex.guest_size)
        no = SAME_KIND;
    if (why) *why = no;
    return no == SAME_NONE;
}
/// The units of tex's storage as upload_inner's CPU path reads them (upload_same.h add_units): per level and layer, its GOB-block
/// rows where that path goes by rows (2D block-linear, not widened), else the level's and layer's whole span. Kept in step with
/// upload_inner's own loop; a read the copy does not cover is read from memory all the same and the hashes are forgotten
/// (upload_same::copy_read), so a step lost here costs the check, not the picture. (Triforce, 11-16) ASTC decoded on the GPU: one
/// unit a layer, its levels from the first one the image keeps (tail0 into it) to the layer's end, what upload_astc_gpu stages of
/// a layer a write reached, by the same expressions; none when its layout is not one that function takes. A write only before
/// a layer's tail0 reaches no unit, and the check finds nothing to upload, as upload_astc_gpu finds no layer to stage
void same_units(const Texture& tex, std::vector<upload_same::Unit>& u) {
    u.clear();
    const uint64_t total = tex.guest_size;
    if (tex.astc) {
        const uint64_t layer_size = tex.layers > 1 ? tex.L.layer_size : std::max<uint64_t>(tex.L.layer_size, total);
        const uint64_t tail0 = level_geo(tex, 0).offset;
        if (!layer_size || tail0 >= layer_size || (tail0 & 15) || (layer_size & 15)) return;
        for (uint32_t layer = 0; layer < tex.layers; layer++) {
            const uint64_t at = (uint64_t)layer * layer_size + tail0;
            if (at >= total) continue;
            upload_same::add_units(u, at, layer_size - tail0, layer_size - tail0, total);
        }
        upload_same::sort_units(u);
        return;
    }
    const bool linear = nvn_tex_is_linear(&tex.d);
    int widen_from = 0;
    widened(tex.d.format ? vk_format(tex.nvn_format).format : tex.format, &widen_from);
    const uint32_t copy_layers = tex.type == VK_IMAGE_TYPE_3D ? 1 : tex.layers;
    for (uint32_t level = 0; level < tex.levels; level++) {
        const LevelGeo g = level_geo(tex, level);
        for (uint32_t layer = 0; layer < copy_layers; layer++) {
            const uint64_t src_off = g.offset + (uint64_t)layer * tex.L.layer_size;
            if (src_off >= total) continue;
            const uint32_t stride = linear ? (tex.d.stride ? tex.d.stride : host->tex_linear_stride(&tex.d)) : 0;
            const uint64_t span = linear ? (uint64_t)stride * g.th : std::max<uint64_t>(g.swizzled, 1);
            const bool rows = !(linear || tex.astc || widen_from || g.td > 1 || g.tile_d);
            const uint64_t gobs_x = ((uint64_t)g.tw * tex.bpb + 63) / 64;
            upload_same::add_units(u, src_off, span, rows ? gobs_x * (512ull << g.tile_h) : span, total);
        }
    }
    upload_same::sort_units(u);
}
/// The bytes [lo, lo + n) of tex's storage as its upload reads them (guest_bytes): below 1 TB as submitted (the copies of the
/// pages a kept capture took, over guest memory), above it as they are; fn(off, bytes, len) part by part, each within one 4 KB
/// page of the GPU address, in order. False when they are not in one piece of host memory
template <class Fn>
bool same_visit(const Texture& tex, uint64_t lo, uint64_t n, Fn& fn) {
    uint64_t avail = 0;
    const uint8_t* p = host->gpu_ptr(tex.gpu + lo, &avail);
    if (!p || avail < n) return false;
    if (tex.gpu < (1ull << 40)) {
        memory_visit_as_submitted(tex.gpu + lo, p, n,
                                  [](void* c, uint64_t off, const uint8_t* b, uint64_t len) { (*static_cast<Fn*>(c))(off, b, len); }, &fn);
        return true;
    }
    const uint64_t at = tex.gpu + lo;
    for (uint64_t o = 0; o < n;) {
        const uint64_t e = std::min(n, (at + o) / upload_same::kPage * upload_same::kPage + upload_same::kPage - at);
        fn(o, p + o, e - o);
        o = e;
    }
    return true;
}
/// RX_VK_UPLOAD_SAME_KEEP=0: the check hashes every byte it looks at, the kept capture copies' own hashes not used
/// (vk_memory.cpp memory_hash_as_submitted: a page's hash kept with its copy until a capture copies other bytes into it)
bool same_keep_on() {
    static const bool v = !getenv("RX_VK_UPLOAD_SAME_KEEP") || atoi(getenv("RX_VK_UPLOAD_SAME_KEEP")) != 0;
    return v;
}
/// A unit's hash as its bytes are now (0, which no kept hash is, when they cannot be read in one piece)
uint64_t same_hash_now(const Texture& tex, const upload_same::Unit& u) {
    upload_same::UnitHash h(u.hi - u.lo);
    if (tex.gpu < (1ull << 40) && same_keep_on()) {   // (below 1 TB: a page a kept capture holds hashed once, same_visit's parts)
        uint64_t avail = 0;
        const uint8_t* p = host->gpu_ptr(tex.gpu + u.lo, &avail);
        if (!p || avail < u.hi - u.lo) return 0;
        memory_hash_as_submitted(tex.gpu + u.lo, p, u.hi - u.lo,
                                 [](void* c, uint64_t, uint64_t, uint64_t part) { static_cast<upload_same::UnitHash*>(c)->add_hash(part); },
                                 &h);
        return h.done();
    }
    auto add = [&h](uint64_t, const uint8_t* b, uint64_t len) { h.add(b, len); };
    return same_visit(tex, u.lo, u.hi - u.lo, add) ? h.done() : 0;
}
// (the copy the upload under way reads, this thread's: same_look sets it, same_done clears it)
thread_local const Texture* tl_same_tex = nullptr;
thread_local upload_same::Copy tl_same_copy;
thread_local std::vector<uint32_t> tl_same_read;    // (the units the upload reads, and the hashes of their copy)
thread_local std::vector<uint64_t> tl_same_hashes;
thread_local std::unique_ptr<uint8_t[]> tl_same_buf;   // (the copy's bytes, at the storage's offsets: up to kMaxBytes a thread)
thread_local uint64_t tl_same_cap = 0;
/// upload_inner's read of [off, off + n) of tex's storage from a checked re-upload's copy; null when no copy is under way for
/// tex (read memory as before), or when the copy does not hold all of it (read memory, and the hashes are forgotten)
const uint8_t* same_snap_read(const Texture& tex, uint64_t off, uint64_t n) {
    if (tl_same_tex != &tex) return nullptr;
    return upload_same::copy_read(tl_same_copy, off, n);
}
/// The units tl_same_read names, copied as the upload would read them (as submitted) and hashed as copied (tl_same_hashes): the
/// copy the upload under way for tex reads (tl_same_tex), until same_done or same_twin_done. A unit that cannot be read in one
/// piece (or no buffer for the copy) leaves no copy: the upload reads memory as before and the hashes go. Whether it was made
bool same_copy(Texture& tex) {
    const upload_same::State& s = tex.same;
    if (tl_same_cap < tex.guest_size) {
        tl_same_buf.reset(new (std::nothrow) uint8_t[tex.guest_size]);
        tl_same_cap = tl_same_buf ? tex.guest_size : 0;
    }
    uint8_t* const buf = tl_same_buf.get();
    tl_same_hashes.clear();
    tl_same_copy.filled.clear();
    tl_same_copy.used = tl_same_copy.short_read = false;
    tl_same_copy.bytes = buf, tl_same_copy.total = tex.guest_size;
    bool ok = buf != nullptr, overlap = false;
    uint64_t reach = 0;
    for (size_t k = 0; ok && k < tl_same_read.size(); k++) {   // (no buffer for the copy: none, as when unreadable)
        const upload_same::Unit& u = s.units[tl_same_read[k]];
        overlap = overlap || u.lo < reach, reach = std::max(reach, u.hi);
        uint8_t* const dst = buf + u.lo;
        upload_same::UnitHash h(u.hi - u.lo);
        auto copy = [&](uint64_t off, const uint8_t* b, uint64_t len) {
            std::memcpy(dst + off, b, len);
            h.add(dst + off, len);
        };
        if (!(ok = same_visit(tex, u.lo, u.hi - u.lo, copy))) break;
        tl_same_hashes.push_back(h.done());
        if (!tl_same_copy.filled.empty() && u.lo <= tl_same_copy.filled.back().second)
            tl_same_copy.filled.back().second = std::max(tl_same_copy.filled.back().second, u.hi);
        else
            tl_same_copy.filled.push_back({u.lo, u.hi});
    }
    if (ok && overlap)   // (units that overlap, which no layout makes: the hashes of the copy as it ended up)
        for (size_t k = 0; k < tl_same_read.size(); k++) {
            const upload_same::Unit& u = s.units[tl_same_read[k]];
            tl_same_hashes[k] = upload_same::hash_at(buf + u.lo, tex.gpu + u.lo, u.hi - u.lo);
        }
    tl_same_tex = ok ? &tex : nullptr;
    if (!ok) tl_same_hashes.clear();
    return ok;
}
std::atomic<int64_t> st_same_checks{0}, st_same_looked{0}, st_same_ns{0}, st_same_same{0}, st_same_up{0}, st_same_skips{0},
    st_same_uploads{0}, st_same_rests{0}, st_same_forgot{0};
std::atomic<int64_t> st_same_astc_checks{0}, st_same_astc_same{0}, st_same_astc_up{0};   // ((Triforce) of them ASTC decoded on the GPU)
/// texture_touch's look at a re-upload of tex whose CPU writes reached `dirty` (a partial one: uploaded before, not the GPU's):
/// 0 not checked (uploaded as before; why in tl_same_why), 1 nothing to upload (each unit the writes reached holds the image's
/// bytes already), 2 an upload of the units whose bytes changed (`dirty` theirs now) that reads the copy made here, until same_done
int same_look(Texture& tex, Ranges& dirty) {
    tl_same_why = SAME_NONE;
    if (!upload_same_on()) return tl_same_why = SAME_OFF, 0;
    if (dirty.empty() || !same_wanted(tex, &tl_same_why)) return 0;
    if (!upload_same::take(tex.same)) return tl_same_why = SAME_RESTING, 0;
    const auto t0 = std::chrono::steady_clock::now();
    upload_same::State& s = tex.same;
    if (s.units.empty()) same_units(tex, s.units);
    if (s.units.empty()) return tl_same_why = SAME_KIND, 0;
    const uint32_t rest0 = s.rest;
    static thread_local std::vector<uint32_t> up;
    int64_t looked = 0;
    const upload_same::Verdict v = upload_same::check(s, dirty, [&](const upload_same::Unit& u) {
        looked += (int64_t)(u.hi - u.lo);
        return same_hash_now(tex, u);
    }, up);
    st_same_checks.fetch_add(1, std::memory_order_relaxed), st_same_looked.fetch_add(looked, std::memory_order_relaxed);
    st_same_same.fetch_add((int64_t)v.same, std::memory_order_relaxed);
    if (tex.astc) {
        st_same_astc_checks.fetch_add(1, std::memory_order_relaxed);
        st_same_astc_same.fetch_add((int64_t)v.same, std::memory_order_relaxed);
        st_same_astc_up.fetch_add((int64_t)v.changed, std::memory_order_relaxed);
    }
    if (s.rest && !rest0) st_same_rests.fetch_add(1, std::memory_order_relaxed);
    int verdict = 1;
    if (!up.empty()) {
        verdict = 2;
        upload_same::ranges_of(s, up, dirty);
        upload_same::reached(s, dirty, tl_same_read);
        same_copy(tex);
        st_same_up.fetch_add((int64_t)v.changed, std::memory_order_relaxed);
    } else {
        st_same_skips.fetch_add(1, std::memory_order_relaxed);
    }
    st_same_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                         std::memory_order_relaxed);
    static std::atomic<uint64_t> told_at{0};
    uint64_t told = told_at.load(std::memory_order_relaxed);
    if (frame_no >= told + 600 && told_at.compare_exchange_strong(told, frame_no)) {
        logf("vk: re-uploads checked against the bytes their images hold (upload_same.h; since the last line): %lld checks, %.1f MB "
             "looked at in %.2f ms; %.1f MB found the same, not uploaded (%lld re-uploads left out whole), %.1f MB uploaded from "
             "the check's copy (%lld uploads, the hashes of %lld forgotten); %lld textures uploaded unchecked for their next %u "
             "re-uploads after %u checks found nothing the same; of them ASTC decoded on the GPU: %lld checks, %.1f MB the same, "
             "%.1f MB uploaded (RX_VK_UPLOAD_SAME=0: every re-upload as before; RX_VK_UPLOAD_SAME_ASTC=0: ASTC's)",
             (long long)st_same_checks.exchange(0), st_same_looked.exchange(0) / 1048576.0, st_same_ns.exchange(0) / 1e6,
             st_same_same.exchange(0) / 1048576.0, (long long)st_same_skips.exchange(0), st_same_up.exchange(0) / 1048576.0,
             (long long)st_same_uploads.exchange(0), (long long)st_same_forgot.exchange(0), (long long)st_same_rests.exchange(0),
             upload_same::kRest, upload_same::kMisses, (long long)st_same_astc_checks.exchange(0),
             st_same_astc_same.exchange(0) / 1048576.0, st_same_astc_up.exchange(0) / 1048576.0);
    }
    return verdict;
}
/// After the upload same_look prepared (verdict 2): the copy let go, and its hashes kept as the image's when the upload read the
/// copy alone and succeeded (upload_same::commit), else forgotten
void same_done(Texture& tex, bool up_ok) {
    const bool from_copy = up_ok && tl_same_tex == &tex && tl_same_copy.used && !tl_same_copy.short_read;
    tl_same_tex = nullptr;
    upload_same::commit(tex.same, tl_same_read, tl_same_hashes, from_copy);
    tl_same_copy.filled.clear();
    tl_same_copy.bytes = nullptr;
    st_same_uploads.fetch_add(1, std::memory_order_relaxed);
    if (!from_copy) st_same_forgot.fetch_add(1, std::memory_order_relaxed);
}
// ---- (Triforce, 11-16) the parallel replay's twin: an earlier segment's upload again of what a later one uploaded in this
// submission (upload_same.h, "the parallel replay's twin"; texture_touch) ----
/// RX_VK_TWIN_SAME=0: the twin uploads the whole storage again and forgets the kept hashes, as before
bool twin_same_on() {
    static const bool v = !getenv("RX_VK_TWIN_SAME") || atoi(getenv("RX_VK_TWIN_SAME")) != 0;
    return v;
}
/// (the image an upload went into, as upload_same::SegUploads notes it)
uint64_t same_image_id(const Texture& tex) {
    return (uint64_t)(uintptr_t)tex.image;
}
/// Before the twin's upload of `part` (what this submission's uploads into tex asked for, upload_same::twin_ranges): when every
/// unit it reads has a hash kept for the image as it is (upload_same::twin_known), those units copied as submitted and hashed
/// (same_copy), for the upload to read. Whether that copy is under way (same_twin_done after the upload, either way)
bool same_twin_look(Texture& tex, const Ranges& part) {
    if (!twin_same_on() || !upload_same_on() || !same_wanted(tex)) return false;
    const upload_same::State& s = tex.same;
    if (s.units.empty()) return false;   // (never checked: nothing kept)
    upload_same::reached(s, part, tl_same_read);
    if (!upload_same::twin_known(s, tl_same_read)) return false;
    return same_copy(tex);
}
std::atomic<int64_t> st_twin_n{0}, st_twin_bytes{0}, st_twin_all{0}, st_twin_whole{0}, st_twin_kept{0}, st_twin_forgot{0},
    st_twin_plain{0};
/// After the twin's upload (up_ok: made): the copy let go, and the kept hashes kept when the copy held their bytes, unit by unit
/// (upload_same::twin_commit), else forgotten (upload() counted the upload as a change). part: what it asked for (null: the whole
/// storage); looked: same_twin_look's answer. A line every 600 frames
void same_twin_done(Texture& tex, const Ranges* part, bool looked, bool up_ok) {
    if (!twin_same_on()) return;
    bool kept = false;
    if (looked) {
        const bool from_copy = up_ok && tl_same_tex == &tex && tl_same_copy.used && !tl_same_copy.short_read;
        tl_same_tex = nullptr;
        kept = upload_same::twin_commit(tex.same, tl_same_read, tl_same_hashes, from_copy);
        tl_same_copy.filled.clear();
        tl_same_copy.bytes = nullptr;
    }
    uint64_t bytes = part ? 0 : tex.guest_size;
    if (part)
        for (const auto& r : *part) bytes += r.second - r.first;
    st_twin_n.fetch_add(1, std::memory_order_relaxed), st_twin_bytes.fetch_add((int64_t)bytes, std::memory_order_relaxed);
    st_twin_all.fetch_add((int64_t)tex.guest_size, std::memory_order_relaxed);
    if (!part) st_twin_whole.fetch_add(1, std::memory_order_relaxed);
    (looked ? kept ? st_twin_kept : st_twin_forgot : st_twin_plain).fetch_add(1, std::memory_order_relaxed);
    static std::atomic<uint64_t> told_at{0};
    uint64_t told = told_at.load(std::memory_order_relaxed);
    if (frame_no < told + 600 || !told_at.compare_exchange_strong(told, frame_no)) return;
    logf("vk: the parallel replay's twins (an earlier segment's upload again of what a later one uploaded in the submission; since "
         "the last line): %lld, %.1f MB uploaded of %.1f MB whole (%lld of the whole storage); %lld from a copy that held the bytes "
         "the image's hashes are of (kept), %lld from a copy that did not (forgotten), %lld without a copy (RX_VK_TWIN_SAME=0: the "
         "whole storage, the hashes forgotten)", (long long)st_twin_n.exchange(0), st_twin_bytes.exchange(0) / 1048576.0,
         st_twin_all.exchange(0) / 1048576.0, (long long)st_twin_whole.exchange(0), (long long)st_twin_kept.exchange(0),
         (long long)st_twin_forgot.exchange(0), (long long)st_twin_plain.exchange(0));
}
bool upload_inner(Texture& tex, const Ranges* dirty);
/// ((Triforce, 11-17) an upload texture_touch recorded into an earlier segment's init command buffer (UploadRedirect,
/// upload_earliest.h), `part` what it asked for (null: the whole storage): counted, for upload_earliest_report)
void upload_earliest_note(const Texture& tex, const Ranges* part) {
    uint64_t bytes = part ? 0 : tex.guest_size;
    if (part)
        for (const auto& r : *part) bytes += r.second - r.first;
    st_up_earliest_n.fetch_add(1, std::memory_order_relaxed), st_up_earliest_bytes.fetch_add((int64_t)bytes, std::memory_order_relaxed);
}
/// ((Triforce, 11-17) a line every 600 frames (texture_frame), RX_VK_UPLOAD_EARLIEST on or off, so that both arms of an A/B say
/// it: the uploads recorded into an earlier segment's init command buffer and their bytes, and the segments' ends that waited
/// for their init lock, held while such an upload or a new image's first layout change is recorded there)
void upload_earliest_report() {
    if (frame_no % 600) return;
    const int64_t waits = st_seg_end_waits.exchange(0), wait_ns = st_seg_end_wait_ns.exchange(0);
    logf("vk: uploads recorded into an earlier segment's init command buffer (the earliest still recorded), so that no earlier "
         "segment uploads them again (the last 600 frames; RX_VK_UPLOAD_EARLIEST=%d): %lld, %.1f MB; segments' ends that waited "
         "for their init lock (such an upload or a new image's first layout change recorded there): %lld, %.2f ms in all",
         upload_earliest_on() ? 1 : 0, (long long)st_up_earliest_n.exchange(0), st_up_earliest_bytes.exchange(0) / 1048576.0,
         (long long)waits, wait_ns / 1e6);
}
// ((Triforce, 11-16) the re-uploads made without the check above, a line every 600 frames: how many and the bytes asked for (the
// part written, or all of it), by why (SameWhy) and by the way they went (UpRoute). Where a burst of uploads that bring the same
// bytes again goes past the check, and which way it goes: the S2 Market's ASTC decoded on the GPU was found so)
std::atomic<int64_t> st_unchecked_n[SAME_WHY_COUNT][UP_COUNT], st_unchecked_bytes[SAME_WHY_COUNT][UP_COUNT];
void same_unchecked(int why, int route, const Ranges* part, uint64_t whole) {
    if (why <= SAME_NONE || why >= SAME_WHY_COUNT || route < 0 || route >= UP_COUNT) return;
    uint64_t bytes = part ? 0 : whole;
    if (part)
        for (const auto& r : *part) bytes += r.second - r.first;
    st_unchecked_n[why][route].fetch_add(1, std::memory_order_relaxed);
    st_unchecked_bytes[why][route].fetch_add((int64_t)bytes, std::memory_order_relaxed);
    static std::atomic<uint64_t> told_at{0};
    uint64_t told = told_at.load(std::memory_order_relaxed);
    if (frame_no < told + 600 || !told_at.compare_exchange_strong(told, frame_no)) return;
    static const char* const why_name[SAME_WHY_COUNT] = {
        "", "RX_VK_UPLOAD_SAME=0", "RX_VK_UPLOAD_SAME_ASTC=0", "not wanted for its size", "not wanted for its kind or layout",
        "resting after checks that found nothing the same", "not partial: a GPU buffer wrote its storage",
        "not partial: drawn by the GPU", "not partial: owed whole", "not partial: RX_VK_NO_PARTIAL",
        "the parallel replay's twin (the same data again in an earlier segment)", "the CPU's write over the GPU's image"};
    static const char* const route_name[UP_COUNT] = {"ASTC decoded on the GPU", "unswizzled on the GPU", "the CPU's"};
    int64_t wn[SAME_WHY_COUNT] = {}, wb[SAME_WHY_COUNT] = {}, rn[UP_COUNT] = {}, rb[UP_COUNT] = {}, n = 0, b = 0;
    for (int w = SAME_NONE + 1; w < SAME_WHY_COUNT; w++)
        for (int r = 0; r < UP_COUNT; r++) {
            const int64_t k = st_unchecked_n[w][r].exchange(0), x = st_unchecked_bytes[w][r].exchange(0);
            wn[w] += k, wb[w] += x, rn[r] += k, rb[r] += x, n += k, b += x;
        }
    char line[1536];
    int at = snprintf(line, sizeof line, "vk: re-uploads not checked against the bytes their images hold (upload_same.h; since the "
                      "last line): %lld, %.1f MB asked for; by route:", (long long)n, b / 1048576.0);
    for (int r = 0; r < UP_COUNT && at < (int)sizeof line - 160; r++)
        if (rn[r]) at += snprintf(line + at, sizeof line - at, " %s %lld (%.1f MB),", route_name[r], (long long)rn[r], rb[r] / 1048576.0);
    at += snprintf(line + at, sizeof line - at, " by why:");
    for (int w = SAME_NONE + 1; w < SAME_WHY_COUNT && at < (int)sizeof line - 160; w++)
        if (wn[w]) at += snprintf(line + at, sizeof line - at, " %s %lld (%.1f MB),", why_name[w], (long long)wn[w], wb[w] / 1048576.0);
    if (at > 0 && line[at - 1] == ',') line[at - 1] = 0;
    logf("%s", line);
}
/// (Triforce) why this thread's last upload() failed: no staging memory could be had for it (staging: an arena that could not be
/// made). Such an upload stays owed and is tried again (texture_touch, upload_owed.h); any other failure is of an image no
/// upload fills (multisampled, depth and stencil, a size no upload takes), which trying again does not change
thread_local bool upload_starved = false;
bool upload(Texture& tex, const Ranges* dirty) {
    const auto t0 = std::chrono::steady_clock::now();
    up_route = UP_CPU;
    upload_same::changed(tex.same);   // ((Triforce) whatever it writes into the image: the kept hashes go, but a checked upload's)
    const uint64_t image0 = same_image_id(tex);   // ((Triforce, 11-16) the image before it, for the twin's note below)
    upload_starved = false;
    const bool ok = upload_inner(tex, dirty);
    // ((Triforce, 11-16) what it asked for and the image it went into (made again by it: the whole storage), for the parallel
    // replay's twin, upload_same.h. Made or not: what a failed one may have written is in it)
    if (twin_same_on()) upload_same::note_upload(tex.same.seg, rec_value, image0, same_image_id(tex), dirty);
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    frame_upload_ns.fetch_add(ns, std::memory_order_relaxed);
    {   // (Triforce: and its bytes, the part asked for, for the new textures' budget: new_texture_later)
        uint64_t b = 0;
        if (dirty) for (const auto& r : *dirty) b += r.second - r.first;
        frame_upload_bytes.fetch_add((int64_t)(dirty ? b : tex.guest_size), std::memory_order_relaxed);
    }
    std::lock_guard lk(up_mu);
    UpStat& u = up_frame[up_route];
    u.n++, u.bytes += (int64_t)tex.guest_size, u.ns += ns;
    if (ns > 2000000) {
        up_slowest.push_back({ns, tex.width, tex.height, tex.layers, tex.nvn_format, up_route, tex.guest_size});
        if (up_slowest.size() > 64) {
            std::sort(up_slowest.begin(), up_slowest.end(), [](const UpSlow& a, const UpSlow& b) { return a.ns > b.ns; });
            up_slowest.resize(8);
        }
    }
    return ok;
}
/// (hookshot) A big texture whose whole storage is zero: the image cleared to zero on the GPU, not unswizzled on the CPU. All-zero
/// bits are 0 in every format this path takes (not ASTC or BC, not one widened on the way), so the clear is what the unswizzle
/// would have made. The game's shadow map (12288 x 12288 D16, 288 MB, its storage fresh and zero) took 73 ms of the replay
/// thread at a scene change; the check reads the storage once, on several threads, and stops at the first byte that is not 0.
/// RX_VK_ZERO_UPLOAD=0: off. ((Triforce, 11-17) all_zero_takes: its checks before it reads the storage, upload_reads_live's too)
bool all_zero_takes(const Texture& tex, uint64_t total, const Ranges* dirty, bool widened_format) {
    static const bool off = getenv("RX_VK_ZERO_UPLOAD") && atoi(getenv("RX_VK_ZERO_UPLOAD")) == 0;
    if (off || total < (32ull << 20) || tex.astc || tex.bc || tex.sparse || widened_format || !tex.image ||
        tex.type == VK_IMAGE_TYPE_3D || is_bc(tex.format) || is_astc(tex.format))
        return false;
    if (dirty) {   // (only an upload of the whole storage: a part of it says nothing of the rest)
        uint64_t covered = 0;
        for (const auto& r : *dirty) covered += std::min(r.second, total) - std::min(r.first, total);
        if (covered < total) return false;
    }
    // (live memory, as the GPU unswizzle reads it: not a storage the capture kept as it was at its submit, which the CPU path reads)
    return tex.gpu < (1ull << 40) && !memory_has_shadow(tex.gpu, total);
}
bool upload_all_zero(Texture& tex, uint64_t total, const Ranges* dirty, bool widened_format) {
    if (!all_zero_takes(tex, total, dirty, widened_format)) return false;
    uint64_t avail = 0;
    const uint8_t* p = host->gpu_ptr(tex.gpu, &avail);
    if (!p || avail < total) return false;
    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<bool> found{false};
    const uint32_t nthreads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
    const uint64_t per = ((total / nthreads) + 4095) & ~4095ull;
    auto part = [&](uint32_t t) {
        const uint64_t a = std::min<uint64_t>(total, (uint64_t)t * per), b = std::min<uint64_t>(total, a + per);
        for (uint64_t at = a; at < b && !found.load(std::memory_order_relaxed); at += 65536) {
            const uint64_t end = std::min<uint64_t>(b, at + 65536);
            uint64_t acc = 0;
            uint64_t k = at;
            for (; k + 8 <= end; k += 8) {
                uint64_t w;
                std::memcpy(&w, p + k, 8);
                acc |= w;
            }
            for (; k < end; k++) acc |= p[k];
            if (acc) found.store(true, std::memory_order_relaxed);
        }
    };
    if (nthreads > 1) {
        std::vector<std::thread> th;
        for (uint32_t t = 1; t < nthreads; t++) th.emplace_back(part, t);
        part(0);
        for (auto& x : th) x.join();
    } else {
        part(0);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static std::atomic<int> told{0};
    if (found.load()) {
        if (told.fetch_add(1) < 4)
            logf("vk: upload: %ux%u NVN %#x (%llu MB) is not all zero (checked in %.1f ms): unswizzled as before", tex.width,
                 tex.height, tex.nvn_format, (unsigned long long)(total >> 20), ms);
        return false;
    }
    texture_resident_flush(&tex);
    checkpoint_mark(upload_cmd(), tex.gpu, 'u');
    const VkImageSubresourceRange range{tex.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (tex.aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        const VkClearDepthStencilValue z{0.0f, 0};
        vkCmdClearDepthStencilImage(upload_cmd(), tex.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
    } else {
        const VkClearColorValue z{};
        vkCmdClearColorImage(upload_cmd(), tex.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
    }
    if (told.fetch_add(1) < 4)
        logf("vk: upload: %ux%u NVN %#x (%llu MB) is all zero: cleared on the GPU (checked in %.1f ms)", tex.width, tex.height,
             tex.nvn_format, (unsigned long long)(total >> 20), ms);
    return true;
}
/// ((Triforce, 11-17) Whether upload_inner may take a way whose bytes are not fixed when it is recorded: the GPU unswizzle reads
/// the storage in its pool when the GPU runs it, and the all-zero clear is decided from live memory, not the capture's copy (their
/// checks before they read anything: true when either may take it). False: staged from the CPU as it records (ASTC's blocks, the
/// CPU's unswizzle), the bytes as submitted. Only such an upload goes into an earlier segment J's init command buffer
/// (UploadRedirect, upload_earliest.h): one read on the GPU there would run before a segment between J and this one writes that
/// memory on the GPU (a copy out of a texture, a storage buffer), and read what this segment's own uploads would not)
bool upload_reads_live(const Texture& tex, const Ranges* dirty) {
    const uint64_t total = tex.guest_size;
    if (!tex.gpu || tex.astc || tex.bc) return false;   // (ASTC: staged, whichever way it goes; BC: no CPU path into it)
    int widen_from = 0;
    widened(tex.d.format ? vk_format(tex.nvn_format).format : tex.format, &widen_from);
    VkFormat vf;
    return (!(tex.gpu < (1ull << 40) && memory_has_shadow(tex.gpu, total)) && unswizzle_gpu_takes(tex, total, vf)) ||
           all_zero_takes(tex, total, dirty, widen_from != 0);
}
bool upload_inner(Texture& tex, const Ranges* dirty) {
    PROF(PROF_UPLOAD);
    if (!tex.gpu || tex.samples != VK_SAMPLE_COUNT_1_BIT) return false;
    const uint64_t total = tex.guest_size;
    if (!total || (total > (1ull << 31) && !tex.sparse)) return false;   // (a sparse image: only what holds texels)
    // (depth-stencil: the depth aspect only; packed D24S8 storage is not copied into it as is: such targets are
    // rendered, not uploaded)
    if (tex.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return false;
    st_uploads++;
    {   // RX_VK_LOG_UPLOADS=<present>: the uploads from that present on (200 of them; what churns)
        static const int64_t from = getenv("RX_VK_LOG_UPLOADS") ? atoll(getenv("RX_VK_LOG_UPLOADS")) : -1;
        static int logged;
        if (from >= 0 && (int64_t)frame_no >= from && logged < 200) {
            logged++;
            uint64_t part = 0;
            if (dirty)
                for (const auto& r : *dirty) part += r.second - r.first;
            logf("vk: upload #%u %#llx %ux%ux%u x%u levels %u NVN %#x%s: %llu KB of %llu KB (%s), made at present %llu",
                 tex.id, (unsigned long long)tex.gpu, tex.width, tex.height, tex.depth, tex.layers, tex.levels,
                 tex.nvn_format, tex.astc ? " ASTC" : "", (unsigned long long)((dirty ? part : total) >> 10),
                 (unsigned long long)(total >> 10), !tex.uploaded ? "new" : dirty ? "written" : "whole",
                 (unsigned long long)tex.first_seen);
        }
    }
    if (tex.astc && (up_route = UP_ASTC, upload_astc_gpu(tex, total, dirty))) return true;
    up_route = UP_CPU;
    if (tex.bc) return false;   // (no CPU path into BC3)
    // (the GPU's unswizzle reads guest memory as it is: not where the capture kept the storage as it was at its submit.
    // ((Triforce, 11-17) neither it nor the all-zero clear below for an upload recorded into an earlier segment's init command
    // buffer (UploadRedirect, tl_up_redirect): upload_reads_live said neither takes it, and this keeps it so)
    if (!tl_up_redirect && !(tex.gpu < (1ull << 40) && memory_has_shadow(tex.gpu, total)) && upload_unswizzle_gpu(tex, total, dirty))
        return up_route = UP_GPU, true;
    const bool linear = nvn_tex_is_linear(&tex.d);
    int widen_from = 0;
    widened(tex.d.format ? vk_format(tex.nvn_format).format : tex.format, &widen_from);
    if (!tl_up_redirect && upload_all_zero(tex, total, dirty, widen_from != 0)) return true;
    const int src_bpb = tex.bpb;
    const int out_bpb = tex.astc ? 4 : widen_from ? (widen_from == 3 ? 4 : widen_from == 6 ? 8 : 16) : src_bpb;
    const uint32_t copy_layers = tex.type == VK_IMAGE_TYPE_3D ? 1 : tex.layers;
    const VkImageAspectFlags aspect = tex.aspect;
    std::vector<VkBufferImageCopy> copies;
    std::vector<uint8_t> scratch, lin;
    VkBuffer buffer = VK_NULL_HANDLE;
    for (uint32_t level = 0; level < tex.levels; level++) {
        const LevelGeo g = level_geo(tex, level);
        for (uint32_t layer = 0; layer < copy_layers; layer++) {
            const uint64_t src_off = g.offset + (uint64_t)layer * tex.L.layer_size;
            if (src_off >= total) continue;
            const uint32_t stride = linear ? (tex.d.stride ? tex.d.stride : host->tex_linear_stride(&tex.d)) : 0;
            const uint64_t span = linear ? (uint64_t)stride * g.th : std::max<uint64_t>(g.swizzled, 1);
            if (!overlaps(dirty, src_off, src_off + span)) continue;
            // the block rows to upload: all of them, or those a written page falls in (2D block-linear levels)
            const uint32_t rows_per = 8u << g.tile_h;   // (block rows of a GOB block)
            const uint32_t nrows = (g.th + rows_per - 1) / rows_per;
            const uint64_t gobs_x = ((uint64_t)g.tw * src_bpb + 63) / 64;
            const uint64_t row_bytes = gobs_x * (512ull << (g.tile_h + g.tile_d));
            std::vector<std::pair<uint32_t, uint32_t>> runs;   // (first row, rows)
            if (!dirty || linear || tex.astc || widen_from || g.td > 1 || g.tile_d) {
                runs.push_back({0, nrows});
            } else {
                std::vector<uint8_t> mark(nrows, 0);
                for (const auto& r : *dirty) {
                    const uint64_t lo = std::max(r.first, src_off), hi = std::min(r.second, src_off + span);
                    if (lo >= hi) continue;
                    const uint64_t r0 = (lo - src_off) / row_bytes, r1 = std::min<uint64_t>((hi - 1 - src_off) / row_bytes, nrows - 1);
                    for (uint64_t k = r0; k <= r1; k++) mark[k] = 1;
                }
                for (uint32_t k = 0; k < nrows;) {
                    if (!mark[k]) { k++; continue; }
                    uint32_t e = k;
                    while (e < nrows && mark[e]) e++;
                    runs.push_back({k, e - k});
                    k = e;
                }
            }
            for (const auto& [row0, nrow] : runs) {
                const uint32_t y0 = row0 * rows_per;                       // (blocks)
                const uint32_t th = std::min(g.th - y0, nrow * rows_per);   // (blocks)
                const uint64_t block_bytes = (uint64_t)g.tw * th * g.td * src_bpb;
                const uint32_t px_h = std::min(g.h - y0 * (uint32_t)tex.block_h, th * (uint32_t)tex.block_h);
                const uint64_t out_bytes = tex.astc ? (uint64_t)g.w * px_h * g.d * 4 : (uint64_t)g.tw * th * g.td * out_bpb;
                const Staging st = staging((out_bytes + 15) & ~15ull, 16);
                if (!st.buffer) return upload_starved = true, false;   // (Triforce: owed, tried again: texture_touch)
                if (buffer && st.buffer != buffer && !copies.empty()) {   // (a new arena: copy what the last one holds)
                    texture_resident_flush(&tex);
                    checkpoint_mark(upload_cmd(), tex.gpu, 'u');
                    vkCmdCopyBufferToImage(upload_cmd(), buffer, tex.image, VK_IMAGE_LAYOUT_GENERAL,
                                           (uint32_t)copies.size(), copies.data());
                    copies.clear();
                }
                buffer = st.buffer;
                uint8_t* out = st.data;
                uint8_t* blocks = (tex.astc || widen_from) ? (lin.resize(block_bytes), lin.data()) : out;
                if (linear) {
                    const uint64_t row = (uint64_t)g.tw * src_bpb;
                    const uint64_t len = (uint64_t)stride * (g.th - 1) + row;
                    const uint8_t* src = same_snap_read(tex, src_off, len);   // ((Triforce) a checked re-upload's copy: same_look)
                    if (!src) src = guest_bytes(tex.gpu + src_off, len, scratch);
                    for (uint32_t y = 0; y < g.th; y++) std::memcpy(blocks + y * row, src + (uint64_t)y * stride, row);
                } else {
                    // (suyu's unswizzle reads the whole swizzled span without bounds checks: a span the storage
                    // ends in is read from a zero-padded copy)
                    const uint64_t from = src_off + (uint64_t)row0 * row_bytes;
                    const uint64_t need = runs.size() == 1 && row0 == 0 && nrow == nrows ? span : (uint64_t)nrow * row_bytes;
                    // (a big 2D level: its GOB-block rows unswizzled on several threads, each row on its own. The
                    // course maps the title loads at each scene change, 6 of 349 MB, took ~100 ms each on one. Their
                    // storage is sparse: each thread gathers its own rows, 8 MB at a time, rather than all of it
                    // being copied first (a 349 MB scratch beside the 349 MB staged))
                    // (Triforce) from RX_VK_UNSWIZZLE_MT_MB (4; 16 before), a thread for each 2 MB: the screen-sized images the
                    // game writes again (8 to 30 MB, 2560 x 1440) took 19 to 24 ms each on one thread in the owner's test4 logs,
                    // a frame's upload each, and most of them were under 16 MB
                    static const uint64_t mt_bytes = (uint64_t)(getenv("RX_VK_UNSWIZZLE_MT_MB") ? std::max(1, atoi(getenv("RX_VK_UNSWIZZLE_MT_MB"))) : 4) << 20;
                    const uint32_t nthreads = block_bytes >= mt_bytes && g.td == 1 && !g.tile_d
                                                  ? std::clamp(std::min<uint32_t>(std::thread::hardware_concurrency() / 2,
                                                                                  (uint32_t)(block_bytes >> 21)), 1u, 8u) : 1u;
                    uint64_t avail = 0;
                    const uint8_t* direct = host->gpu_ptr(tex.gpu + from, &avail);
                    // ((Triforce) a checked re-upload reads the copy its check hashed, upload_same.h: same_look)
                    const uint8_t* snap = from < total ? same_snap_read(tex, from, std::min<uint64_t>(need, total - from)) : nullptr;
                    const bool gather_parts = !snap && nthreads > 1 && !(direct && avail >= need) && from < total;
                    const uint8_t* src = nullptr;
                    if (gather_parts) {
                    } else if (snap && total - from >= need) {
                        src = snap;
                    } else if (from < total && total - from >= need) {
                        src = guest_bytes(tex.gpu + from, need, scratch);
                    } else {
                        scratch.assign(need, 0);
                        if (snap) std::memcpy(scratch.data(), snap, total - from);
                        else if (from < total) host->gpu_read(tex.gpu + from, scratch.data(), total - from);
                        src = scratch.data();
                    }
                    const auto u0 = std::chrono::steady_clock::now();
                    if (nthreads > 1) {
                        const uint64_t out_row = (uint64_t)g.tw * rows_per * src_bpb;   // (bytes per GOB-block row)
                        const uint32_t per = (nrow + nthreads - 1) / nthreads;
                        const uint32_t chunk = (uint32_t)std::max<uint64_t>(1, (8u << 20) / row_bytes);
                        auto part = [&](uint32_t t) {
                            const uint32_t a0 = t * per, b0 = std::min(nrow, a0 + per);
                            std::vector<uint8_t> mine;
                            for (uint32_t a = a0; a < b0; a += chunk) {
                                const uint32_t b = std::min(b0, a + chunk);
                                const uint32_t h = std::min(th - a * rows_per, (b - a) * rows_per);
                                const uint8_t* rows;
                                uint64_t rows_n;
                                if (gather_parts) {   // (its rows, zero past the storage's end)
                                    rows_n = (uint64_t)(b - a) * row_bytes;
                                    mine.assign(rows_n, 0);
                                    const uint64_t at = from + (uint64_t)a * row_bytes;
                                    for (uint64_t o = 0; o < rows_n && at + o < total; o += 0x10000) {
                                        const uint64_t k = std::min<uint64_t>({0x10000, rows_n - o, total - at - o});
                                        host->gpu_read(tex.gpu + at + o, mine.data() + o, k);
                                    }
                                    rows = mine.data();
                                } else {
                                    rows = src + (uint64_t)a * row_bytes, rows_n = need - (uint64_t)a * row_bytes;
                                }
                                Tegra::Texture::UnswizzleTexture(
                                    std::span<uint8_t>(blocks + a * out_row, (uint64_t)g.tw * h * src_bpb),
                                    std::span<const uint8_t>(rows, rows_n), (u32)src_bpb, g.tw, h, 1, g.tile_h, 0, 1);
                            }
                        };
                        std::vector<std::thread> pool;
                        for (uint32_t t = 1; t < nthreads; t++) pool.emplace_back(part, t);
                        part(0);
                        for (std::thread& t : pool) t.join();
                        static const bool check = getenv("RX_VK_UNSWIZZLE_CHECK") != nullptr;   // (against one thread)
                        if (check && src) {
                            std::vector<uint8_t> ref(block_bytes);
                            Tegra::Texture::UnswizzleTexture(std::span<uint8_t>(ref.data(), block_bytes),
                                                             std::span<const uint8_t>(src, need), (u32)src_bpb, g.tw,
                                                             th, g.td, g.tile_h, g.tile_d, 1);
                            const bool same = !std::memcmp(ref.data(), blocks, block_bytes);
                            logf("vk: unswizzle check %#llx level %u rows %u+%u: %s", (unsigned long long)tex.gpu,
                                 level, row0, nrow, same ? "same" : "DIFFERENT");
                        }
                    } else {
                        Tegra::Texture::UnswizzleTexture(std::span<uint8_t>(blocks, block_bytes),
                                                         std::span<const uint8_t>(src, need), (u32)src_bpb, g.tw, th,
                                                         g.td, g.tile_h, g.tile_d, 1);
                    }
                    st_unswizzle_bytes += (int64_t)block_bytes;
                    st_unswizzle_ns +=
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - u0).count();
                }
                if (tex.astc) {
                    Tegra::Texture::ASTC::DecompressSerial(std::span<const uint8_t>(blocks, block_bytes), g.w, px_h, g.d,
                                                           (u32)tex.block_w, (u32)tex.block_h,
                                                           std::span<uint8_t>(out, (size_t)g.w * px_h * g.d * 4));
                } else if (widen_from) {
                    widen_rows(out, blocks, (size_t)g.tw * th * g.td, src_bpb, out_bpb);
                }
                st_bytes += (int64_t)out_bytes;
                // (a sparse image: only its blocks this holds texels other than zero get memory; the others read
                // zero unbound, and what the copy writes into them is dropped)
                if (tex.sparse) {
                    const VkExtent3D gr = tex.sparse_req.formatProperties.imageGranularity;
                    const uint32_t bw = (uint32_t)tex.block_w, bh = (uint32_t)tex.block_h;
                    const uint64_t row = (uint64_t)g.tw * out_bpb;   // (bytes of a row of blocks)
                    const uint32_t tbw = std::max(1u, gr.width / bw);   // (blocks across a sparse block)
                    const uint32_t ntx = (g.tw + tbw - 1) / tbw;
                    std::vector<uint8_t> hit;   // (sparse block rows x columns this reaches)
                    const uint32_t ty0 = y0 * bh / gr.height, ty1 = ((y0 + th) * bh - 1) / gr.height;
                    hit.assign((size_t)(ty1 - ty0 + 1) * ntx, 0);
                    if (level < tex.sparse_req.imageMipTailFirstLod) {
                        for (uint32_t br = 0; br < th; br++) {
                            const uint8_t* p = out + br * row;
                            const uint32_t ty = (y0 + br) * bh / gr.height - ty0;
                            for (uint32_t tx = 0; tx < ntx; tx++) {
                                uint8_t& h = hit[(size_t)ty * ntx + tx];
                                if (h) continue;
                                const uint64_t a = (uint64_t)tx * tbw * out_bpb, e = std::min<uint64_t>(row, a + (uint64_t)tbw * out_bpb);
                                uint64_t any = 0;
                                for (uint64_t i = a; i + 8 <= e; i += 8) {
                                    uint64_t v;
                                    std::memcpy(&v, p + i, 8);
                                    any |= v;
                                }
                                h = any != 0;
                            }
                        }
                    } else {
                        uint64_t any = 0;
                        for (uint64_t i = 0; i + 8 <= out_bytes && !any; i += 8) {
                            uint64_t v;
                            std::memcpy(&v, out + i, 8);
                            any |= v;
                        }
                        if (any) hit.assign(hit.size(), 1);
                    }
                    bool some = false;
                    for (uint32_t ty = 0; ty < ty1 - ty0 + 1; ty++)
                        for (uint32_t tx = 0; tx < ntx; tx++)
                            if (hit[(size_t)ty * ntx + tx]) {
                                some = true;
                                texture_resident_queue(&tex, level, layer, tx * gr.width, (ty0 + ty) * gr.height,
                                                       gr.width, gr.height);
                            }
                    if (!some) continue;
                }
                flush_mapped_range(st.allocation, st.offset, out_bytes);   // (all CPU conversion workers joined before publication)
                VkBufferImageCopy c{};
                c.bufferOffset = st.offset;
                c.bufferRowLength = tex.astc ? g.w : g.tw * tex.block_w;
                c.bufferImageHeight = tex.astc ? px_h : th * tex.block_h;
                c.imageSubresource = {aspect, level, tex.type == VK_IMAGE_TYPE_3D ? 0 : layer, 1};
                c.imageOffset = {0, (int32_t)(y0 * (uint32_t)tex.block_h), 0};
                c.imageExtent = {g.w, px_h, g.d};
                copies.push_back(c);
            }
        }
    }
    if (!copies.empty()) {
        texture_resident_flush(&tex);
        checkpoint_mark(upload_cmd(), tex.gpu, 'u');
        vkCmdCopyBufferToImage(upload_cmd(), buffer, tex.image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)copies.size(),
                               copies.data());
    }
    return true;
}

/// What the CPU wrote of a texture's storage since gen, as byte ranges from its start. A texture in a virtual
/// (sparse) pool is looked at page by page through its mapping, and is all dirty when its mapping changed.
/// same_ok (Triforce): a capture's stamp of a page with the bytes of its last copy is no write (write_gen_table.h page_tex): only
/// for an uploaded texture's "written since my upload?"
bool texture_dirty(Texture& tex, WriteGen gen, Ranges& out, bool same_ok = false) {
    out.clear();
    constexpr uint64_t kVirtual = 1ull << 40, kVPage = 1ull << 16;
    if (tex.gpu < kVirtual) {
        memory_written_runs(tex.guest, tex.guest_size, gen, out, same_ok);
        return !out.empty();
    }
    // (the page walk only when the GPU mappings changed: the terrain's sparse arrays span over 1 GB, 64 KB pages,
    // and were walked every recording; between changes, runs of contiguous guest memory are checked)
    const uint32_t maps_gen = host->maps_generation() + host->vmap_generation();
    if (tex.runs_maps_gen != maps_gen) {
        tex.runs_maps_gen = maps_gen;
        tex.runs.clear();
        uint64_t sig = 0x5157;
        // (the pages whose mapping changed: only those are dirty. The course maps' sparse pools are remapped every
        // ~15 s at the title, a few pages at a time, and each remap re-uploaded all of six 349 MB maps)
        const size_t npages = (size_t)((tex.guest_size + kVPage - 1) / kVPage);
        const bool had_map = tex.page_map.size() == npages;
        if (!had_map) tex.page_map.assign(npages, 0);
        Ranges remapped;
        for (uint64_t off = 0; off < tex.guest_size; off += kVPage) {
            const uint64_t n = std::min(kVPage, tex.guest_size - off);
            const uint64_t g = host->gpu_guest(tex.gpu + off);
            sig = mix(sig, g);
            uint64_t& was = tex.page_map[(size_t)(off / kVPage)];
            if (had_map && was != g) {
                if (!remapped.empty() && remapped.back().second == off) remapped.back().second = off + n;
                else remapped.push_back({off, off + n});
            }
            was = g;
            if (!g) continue;
            Texture::Run& last = tex.runs.empty() ? tex.runs.emplace_back(Texture::Run{off, g, 0}) : tex.runs.back();
            if (last.off + last.n == off && last.guest + last.n == g) last.n += n;
            else tex.runs.push_back({off, g, n});
        }
        std::erase_if(tex.runs, [](const Texture::Run& r) { return r.n == 0; });
        {   // (RX_VK_LOG_REMAP=1: a big texture's pages mapped anew: to which guest memory, and what images are there)
            static const bool log_remap = getenv("RX_VK_LOG_REMAP") != nullptr;
            static int logged;
            if (log_remap && had_map && !remapped.empty() && tex.guest_size >= (32ull << 20) && logged < 120) {
                for (size_t i = 0; i < remapped.size() && i < 4 && logged < 120; i++, logged++) {
                    const uint64_t g = host->gpu_guest(tex.gpu + remapped[i].first);
                    std::string there;
                    for (const auto& kv : textures) {   // (images over that guest memory, by their storage)
                        const Texture* o = kv.second.get();
                        if (o == &tex || !g || o->gpu >= (1ull << 40) || g < o->gpu || g >= o->gpu + o->guest_size) continue;
                        char b[160];
                        snprintf(b, sizeof b, " #%u %#llx %ux%u x%u NVN %#x%s%s;", o->id, (unsigned long long)o->gpu, o->width,
                                 o->height, o->layers, o->nvn_format, o->gpu_written ? " rendered" : "",
                                 o->uploaded ? " uploaded" : "");
                        if (there.size() < 400) there += b;
                    }
                    logf("vk: remap of #%u %#llx %ux%u NVN %#x: +%#llx..+%#llx now at guest %#llx%s%s", tex.id,
                         (unsigned long long)tex.gpu, tex.width, tex.height, tex.nvn_format,
                         (unsigned long long)remapped[i].first, (unsigned long long)remapped[i].second, (unsigned long long)g,
                         g ? "" : " (unmapped)", there.empty() ? " (no image there)" : there.c_str());
                }
            }
        }
        if (sig != tex.map_sig) {   // (pages mapped or unmapped since; gen 0 asks what was ever written)
            tex.map_sig = sig;
            if (gen && !had_map) {
                out.assign(1, {0, tex.guest_size});
                return true;
            }
            if (gen) out = remapped;   // (and below, what the CPU wrote in the pages mapped all along)
        }
    }
    static thread_local Ranges in_run;
    const size_t remaps = out.size();
    for (const Texture::Run& r : tex.runs) {
        in_run.clear();
        memory_written_runs(r.guest, r.n, gen, in_run, same_ok);
        for (const auto& [lo, hi] : in_run) {
            if (out.size() > remaps && out.back().second == r.off + lo) out.back().second = r.off + hi;
            else out.push_back({r.off + lo, r.off + hi});
        }
    }
    if (remaps && out.size() > remaps) {   // (the two lists in order, overlaps merged)
        std::sort(out.begin(), out.end());
        Ranges merged;
        for (const auto& r : out) {
            if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
            else merged.push_back(r);
        }
        out.swap(merged);
    }
    return !out.empty();
}

uint64_t destroy_gen = 1;   // (bumped when textures go: what was resolved to them goes stale)
texture_gone::Record gone_record;   // (which ones went: texture_destroyed_since, for a pointer kept across draws)
// ((Triforce) and the textures whose views went: those gone, made again in another format (bc1_retype), moved (compact_step).
// Each is noted once its views went and before destroy_gen moves, so that a recording thread's pass that sees the move finds
// it: view_keep.h, vk_draw.cpp texture_view_of)
texture_gone::Record views_gone;

void destroy_later(Texture& tex) {
    gone_record.went(tex.id);
    Dead d{tex.last_value, tex.image, tex.allocation, {}, {}};
    if (tex.pool_key && tex.allocation && tex.sparse_chunks.empty() && pool_cap()) {   // (a render target's: to the pool, its views too)
        d.pool_key = tex.pool_key, d.view_map = std::move(tex.views), d.bytes = tex.bytes + tex.pad;
        tex.views.clear();
    } else {
        for (auto& kv : tex.views)
            if (kv.second) d.views.push_back(kv.second);
    }
    d.memory = tex.sparse_chunks;
    dead.push_back(std::move(d));
    if (tex.twin3d) {   // (Triforce: its one-slice 3D copy goes with it)
        Dead t{tex.last_value, tex.twin3d, tex.twin3d_alloc, {}, {}};
        for (auto& kv : tex.twin3d_views)
            if (kv.second) t.views.push_back(kv.second);
        dead.push_back(std::move(t));
        tex.twin3d = VK_NULL_HANDLE, tex.twin3d_alloc = nullptr;
        tex.twin3d_views.clear();
    }
    total_bytes -= std::min(total_bytes, tex.bytes + tex.pad);
    views_gone.went(tex.id);   // ((Triforce) its views went: noted, then the generation moves (view_keep.h))
    destroy_gen++;
}

/// (Triforce) A texture kept as BC1 gets a BC3 image in its place, empty, and stays BC3 (tex_bc1.h, "opaque ASTC textures
/// kept as BC1" above). The BC1 image and its views go once the GPU is past the texture's last use, as an evicted
/// texture's do; what was resolved to its views is stale (destroy_gen), as after a compaction's move. The caller fills
/// the new image. False, and nothing changed, when the image could not be made
bool bc1_retype(Texture& x, const char* why) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (as create() made the BC1 one, but for the format)
    ci.flags = x.create_flags;
    ci.imageType = x.type;
    ci.format = VK_FORMAT_BC3_UNORM_BLOCK;
    ci.extent = {x.width, x.height, x.depth};
    ci.mipLevels = x.levels;
    ci.arrayLayers = x.layers;
    ci.samples = x.samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = x.usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    if (!(compact_on() && create_image(ci, image, allocation, false, false, true)) && !create_image(ci, image, allocation)) {
        static std::atomic<int> told{0};
        if (told++ < 16)
            logf("vk: texture #%u %#llx %ux%u x%u kept as BC1 (%s): its BC3 image could not be made", x.id,
                 (unsigned long long)x.gpu, x.width, x.height, x.layers, why);
        return false;
    }
    Dead d{std::max(x.last_value, rec_value), x.image, x.allocation, {}, {}};
    for (auto& kv : x.views)
        if (kv.second) d.views.push_back(kv.second);
    x.views.clear();
    dead.push_back(std::move(d));
    total_bytes -= std::min(total_bytes, x.bytes + x.pad);
    x.image = image, x.allocation = allocation;
    x.format = VK_FORMAT_BC3_UNORM_BLOCK;
    x.bc1 = false, x.bc1_no = true;
    views_gone.went(x.id);   // ((Triforce) its views went: noted, then the generation moves (view_keep.h))
    destroy_gen++;
    uint64_t px = 0;
    for (uint32_t l = 0; l < x.levels; l++) px += (uint64_t)std::max(1u, x.width >> l) * std::max(1u, x.height >> l);
    x.bytes = px * x.layers;   // (a byte a texel, as create() counts a BC3 image)
    static const bool by_estimate = getenv("RX_VK_COUNT_BY_ESTIMATE") != nullptr;
    x.pad = by_estimate ? 0 : std::max(x.bytes, allocation_size(x.allocation)) - x.bytes;
    total_bytes += x.bytes + x.pad;
    {   // (its first layout change, in queue order for a parallel replay's worker, as a new image's)
        InitRedirect redirect;
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = x.image;
        b.subresourceRange = {x.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(upload_cmd(), &di);
    }
    const uint64_t n = ++st_bc1_back;
    st_bc1_saved.fetch_sub(std::min<uint64_t>(st_bc1_saved.load(), px * x.layers / 2));
    if (n <= 8 || (n & (n - 1)) == 0)
        logf("vk: texture #%u %#llx %ux%u x%u kept as BC1 is BC3 from now on: %s (%llu such so far)", x.id,
             (unsigned long long)x.gpu, x.width, x.height, x.layers, why, (unsigned long long)n);
    return true;
}

void vw_forget(const Texture& t);   // (below: a destroyed texture leaves the list of GPU-written ones)
void forget_gpu(Texture* tex) {
    vw_forget(*tex);
    auto [a, b] = by_gpu.equal_range(tex->gpu);
    for (auto it = a; it != b; ++it)
        if (it->second == tex) {
            by_gpu.erase(it);
            break;
        }
}

/// (as_target: texture_get's for a render target or a copy's destination, not for sampling: its image goes to the render targets'
/// blocks, and to the target pool when its texture goes)
/// (hookshot) a size and sample count Vulkan can make on this GPU: they come from the game's texture (its width a u32 it
/// reads from a file), and past the device's limits vkCreateImage is undefined, and the uploads' buffers grew with them
bool texture_size_ok(VkImageType type, uint32_t w, uint32_t h, uint32_t z, uint32_t layers, int samples) {
    const VkPhysicalDeviceLimits& lim = dev.props.limits;
    const uint32_t most = type == VK_IMAGE_TYPE_1D ? lim.maxImageDimension1D
                        : type == VK_IMAGE_TYPE_3D ? lim.maxImageDimension3D : lim.maxImageDimension2D;
    return w <= most && h <= most && z <= lim.maxImageDimension3D && layers <= lim.maxImageArrayLayers &&
           (samples <= 1 || samples == 2 || samples == 4 || samples == 8 || samples == 16);
}
Texture* create(const Key& key, const nvn_tex* t, VkFormat vf, bool astc, int bw, int bh, int bpb, bool as_target) {
    PROF_EXACT();   // (texture_get's slow path: PROF_TEXGET samples the rest)
    auto tex = std::make_unique<Texture>();
    Texture& x = *tex;
    x.gpu = t->gpu, x.d = t->d, x.L = t->L, x.nvn_format = t->d.format;
    x.guest = host->gpu_guest(t->gpu);
    x.guest_size = t->L.size;
    x.astc = astc, x.block_w = bw, x.block_h = bh, x.bpb = bpb;
    int from = 0;
    vf = widened(vf, &from);
    const bool depth = is_depth_format(vf);
    x.format = depth ? vf : astc ? VK_FORMAT_R8G8B8A8_UNORM : linear_twin(vf);
    x.aspect = !depth ? VK_IMAGE_ASPECT_COLOR_BIT
             : vf == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT
             : has_stencil(vf) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                               : VK_IMAGE_ASPECT_DEPTH_BIT;
    const nvn_texdesc& d = t->d;
    x.width = std::max(1u, d.width);
    x.height = std::max(1u, d.height);
    switch (d.target) {
    case NVN_TEXTURE_TARGET_1D: x.type = VK_IMAGE_TYPE_1D, x.height = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: x.type = VK_IMAGE_TYPE_1D, x.layers = std::max(1u, d.height), x.height = 1; break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: x.layers = std::max(1u, d.depth); break;
    case NVN_TEXTURE_TARGET_CUBEMAP: x.layers = 6; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: x.layers = std::max(6u, d.depth); break;
    case NVN_TEXTURE_TARGET_3D: x.type = VK_IMAGE_TYPE_3D, x.depth = std::max(1u, d.depth); break;
    default: break;
    }
    const uint32_t max_dim = std::max({x.width, x.height, x.depth});
    const uint32_t max_levels = 1 + (uint32_t)std::floor(std::log2((double)max_dim));
    x.levels = nvn_tex_is_linear(&d) ? 1 : std::clamp<uint32_t>(d.levels, 1, max_levels);
    bool by_detail = false;   // (its top levels are left out by the card's texture detail alone: tex_detail.h)
    {   // huge textures leave out their top levels: wider than RX_VK_TEX_MAX_DIM (default 2048) once they would take
        // 64 MB, and down to RX_VK_TEX_MAX_MB (default 512) whatever their size (MKW's terrain material arrays are
        // 8192 x 8192 x 68 ASTC: 24 GB as RGBA8)
        static const uint32_t max_dim = getenv("RX_VK_TEX_MAX_DIM") ? (uint32_t)atoi(getenv("RX_VK_TEX_MAX_DIM")) : 2048;
        static const uint64_t max_bytes = (uint64_t)(getenv("RX_VK_TEX_MAX_MB") ? atoi(getenv("RX_VK_TEX_MAX_MB")) : 1536) << 20;
        const uint32_t texel = astc ? 4 : std::max(1, bpb / std::max(1, bw * bh));
        auto bytes_at = [&](uint32_t skip) {
            uint64_t px = 0;
            for (uint32_t l = skip; l < x.levels; l++)
                px += (uint64_t)std::max(1u, x.width >> l) * std::max(1u, x.height >> l) * std::max(1u, x.depth >> l);
            return px * x.layers * texel * std::max(1, d.samples);
        };
        // (decoded ASTC only by size: uncompressed maps may be read at exact texel coordinates, and the GPU may
        // render into them, where a level left out is wrong)
        // (a 2D array too big to keep whole, in a format with sparse residency: kept whole, as a sparse image)
        if (!astc && !depth && dev.sparse_images && x.type == VK_IMAGE_TYPE_2D && x.layers > 1 && d.samples <= 1 &&
            bytes_at(0) > max_bytes) {
            VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            if (!is_bc(x.format)) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
            uint32_t np = 0;
            vkGetPhysicalDeviceSparseImageFormatProperties(dev.physical, x.format, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT,
                                                           u, VK_IMAGE_TILING_OPTIMAL, &np, nullptr);
            x.sparse = np > 0;
        }
        // (RX_VK_TEX_SKIP_MB: from how many MB as RGBA8 a decoded ASTC texture wider than max_dim loses its top levels;
        // 64 by default. The Steam Deck's 16 GB are shared with the GPU: lower there, with RX_VK_TEX_MAX_DIM=1024)
        static const uint64_t skip_from = (uint64_t)(getenv("RX_VK_TEX_SKIP_MB") ? atoi(getenv("RX_VK_TEX_SKIP_MB")) : 64) << 20;
        while (!x.sparse && astc && x.skip + 1 < x.levels && bytes_at(x.skip) > skip_from &&
               (std::max(x.width, x.height) >> x.skip) > max_dim)
            x.skip++;
        while (!x.sparse && x.skip + 1 < x.levels && bytes_at(x.skip) > max_bytes) x.skip++;
        if (x.skip) {
            static int logged;
            if (logged++ < 40)
                logf("vk: texture %#llx %ux%ux%u x%u (NVN %#x, %llu MB): its %u top level(s) left out (%llu MB)",
                     (unsigned long long)t->gpu, x.width, x.height, x.depth, x.layers, d.format,
                     (unsigned long long)(bytes_at(0) >> 20), x.skip, (unsigned long long)(bytes_at(x.skip) >> 20));
        }
        // (texture detail, tex_detail.h: on a card the driver allows under 10 GB a material texture is made without its
        // largest level, under 6 GB without two. Only a decoded ASTC texture the game samples: 2D or a 2D array, one
        // sample, mipmapped, 256 texels or more each way, not one first met as a render target or a copy's destination.
        // The Insert menu's Texture detail row says so on any card, and RX_VK_TEX_DETAIL=full|reduced|low over it; read at
        // the first texture)
        // (nor one a shader reads by texel position or asks the size of: whole_keys, texture_want_whole)
        const auto detail_level = [] { return texture_detail_level(); };
        if (!as_target && astc && !x.sparse && x.type == VK_IMAGE_TYPE_2D && d.samples <= 1 &&
            (d.target == NVN_TEXTURE_TARGET_2D || d.target == NVN_TEXTURE_TARGET_2D_ARRAY) && !whole_keys.count(key)) {
            const uint32_t by = tex_detail::skip(detail_level(), x.width, x.height, x.levels);
            if (by > x.skip) {
                x.detail_skip = true;
                static std::atomic<uint64_t> n{0}, whole{0}, kept{0};
                by_detail = !x.skip;
                const uint64_t c = ++n;
                whole += bytes_at(0) >> 2, kept += bytes_at(by) >> 2;   // (texels)
                x.skip = by;
                if (c >= 64 && (c & (c - 1)) == 0)
                    logf("vk: texture detail: %llu textures made without their largest level(s) so far, %llu M texels "
                         "kept of %llu M", (unsigned long long)c, (unsigned long long)(kept.load() >> 20),
                         (unsigned long long)(whole.load() >> 20));
            }
        }
        if (x.skip) {
            x.width = std::max(1u, x.width >> x.skip), x.height = std::max(1u, x.height >> x.skip);
            x.depth = std::max(1u, x.depth >> x.skip);
            x.levels -= x.skip;
        }
    }
    if (!texture_size_ok(x.type, x.width, x.height, x.depth, x.layers, d.samples)) {
        static int logged;
        if (logged++ < 16)
            logf("vk: texture %#llx %ux%ux%u x%u samples %d (NVN %#x): larger than this GPU's images, not made",
                 (unsigned long long)t->gpu, x.width, x.height, x.depth, x.layers, d.samples, d.format);
        return nullptr;
    }
    x.samples = d.samples > 1 ? (VkSampleCountFlagBits)d.samples : VK_SAMPLE_COUNT_1_BIT;
    {   // (decoded ASTC as BC3: 2D, at most the scratch's size; from blocks of 6 x 6 texels up (2.25 bits a texel or
        // less: BC3's 8 keep what they have) from 2 MB as RGBA8 (RX_VK_ASTC_BC_MB), the finer ones (UI, characters)
        // only from 64 MB)
        static const uint64_t low = (uint64_t)(getenv("RX_VK_ASTC_BC_MB") ? atoi(getenv("RX_VK_ASTC_BC_MB")) : 2) << 20;
        // (issue 519: on a card the driver gives under 6 GB, the finer ones too. An RTX 3050 Laptop (4 GB, 3.3 allowed)
        // held 1.75 GB of decoded ASTC in Goron City, drawn every frame: 5.6 GB used of 3.3, paged, 4 fps. As BC3, a
        // quarter. Now 8 GB cards too: astc_bc_policy.h. RX_VK_ASTC_BC_ALL=1 or 0 says so on any card)
        static const bool all = [] {
            if (const char* e = getenv("RX_VK_ASTC_BC_ALL")) return e[0] == '1';
            uint64_t used = 0, allowed = 0;
            const bool small = vram_usage(&used, &allowed) && astc_bc_policy::all_by_vram(allowed);
            if (small) logf("vk: %llu MB of VRAM allowed: all large ASTC textures kept as BC3", (unsigned long long)(allowed >> 20));
            return small;
        }();
        const uint64_t min_bytes = (bw * bh >= 36 || all) ? low : 64ull << 20;
        uint64_t px = 0;
        for (uint32_t l = 0; l < x.levels; l++)
            px += (uint64_t)std::max(1u, x.width >> l) * std::max(1u, x.height >> l);
        // (not one with its top levels left out and a single layer: copies into those levels are blitted into its
        // first, which BC3 cannot take; trimmed, it is small)
        // (hookshot-amd) AMD's drivers sample BC levels wrong when a level's block grid is not level 0's halved: 1365 texels
        // are 342 blocks, but level 2 (341 texels) is 86 blocks, not 342 >> 2 = 85. Ocarina's terrain material arrays
        // (1365 x 1365 x 90) then draw a grid of dark lines along the terrain's seams, worse in the distance (Lichii #448 found
        // it on an RX 6600 XT; an RX 6700 XT here). On AMD such a texture is encoded at the nearest size whose blocks halve
        // evenly down to its 16-texel levels (1365 -> 1536, 683 -> 768; 768 and 1536 stay) and keeps only the levels that
        // halve evenly: the game samples with normalized coordinates, so a resized level covers the same area. Where that
        // would grow it by more than a quarter, or past the scratch, it stays RGBA8. RX_VK_UNEVEN_RGBA8=1: RGBA8 instead of resizing.
        // OFF by default since v2.0.6: in v2.0.5 an RX 6700 XT's GPU time went from 10-13 ms to 70-100 ms a frame in play
        // with it (issue 823, logs of both versions), and AMD players saw rainbow ground (Discord). RX_VK_UNEVEN_BC=0: on
        // (every card); unset or 1: BC3 as in v2.0.4.
        using bc_levels::first_uneven;
        using bc_levels::even_size;
        const bool uneven = first_uneven(x.width, x.height, x.levels) < x.levels;
        static const bool amd_fix = [] {
            if (const char* e = getenv("RX_VK_UNEVEN_BC")) return e[0] == '0';
            return false;
        }();
        static const bool rgba8_instead = getenv("RX_VK_UNEVEN_RGBA8") != nullptr;
        // (one whose levels the texture detail left out is BC3 all the same: a copy into a level it lacks is left out)
        const bool bc_candidate = astc && !(x.skip && x.layers == 1 && !by_detail) && x.type == VK_IMAGE_TYPE_2D &&
                                  x.samples == VK_SAMPLE_COUNT_1_BIT && x.width <= kBcDim && x.height <= kBcDim &&
                                  px * x.layers * 4 >= min_bytes;
        if (bc_candidate && uneven && amd_fix) {
            const uint32_t ew = even_size(x.width, x.levels), eh = even_size(x.height, x.levels);
            const uint32_t el = first_uneven(ew, eh, x.levels);
            const bool resize = ew != x.width || eh != x.height;
            static int n_resized, n_trimmed, n_rgba8;
            static uint64_t mb_saved;
            if (!rgba8_instead && ew <= kBcDim && eh <= kBcDim && ew * 4 <= x.width * 5 && eh * 4 <= x.height * 5 &&
                el >= 1 && bc_ready()) {
                uint64_t epx = 0;
                for (uint32_t l = 0; l < el; l++) epx += (uint64_t)std::max(1u, ew >> l) * std::max(1u, eh >> l);
                mb_saved += (px * x.layers * 4 - epx * x.layers) >> 20;
                int& n = resize ? n_resized : n_trimmed;
                n++;
                if (n <= 4 || (n & (n - 1)) == 0)
                    logf("vk: texture %#llx %ux%u x%u, %u levels: uneven BC block levels on AMD, as BC3 at %ux%u, %u levels "
                         "(%d resized, %d with fewer levels, %d as RGBA8; %llu MB less than RGBA8 so far)",
                         (unsigned long long)t->gpu, x.width, x.height, x.layers, x.levels, ew, eh, el, n_resized, n_trimmed,
                         n_rgba8, (unsigned long long)mb_saved);
                x.width = ew, x.height = eh, x.levels = el;
                x.bc = true, x.bc_resized = resize;
                x.format = VK_FORMAT_BC3_UNORM_BLOCK;
            } else {
                n_rgba8++;
                if (n_rgba8 <= 4 || (n_rgba8 & (n_rgba8 - 1)) == 0)
                    logf("vk: texture %#llx %ux%u x%u, %u levels: uneven BC block levels on AMD, kept as RGBA8 (%d so far)",
                         (unsigned long long)t->gpu, x.width, x.height, x.layers, x.levels, n_rgba8);
            }
        } else if (bc_candidate && bc_ready()) {
            x.bc = true;
            x.format = VK_FORMAT_BC3_UNORM_BLOCK;
            // (Triforce) as BC1 when the row or the switch asks for it and every block its storage holds now is opaque (above,
            // "opaque ASTC textures kept as BC1"). Not one first met as a render target or a copy's destination: its
            // content comes from the GPU, which nothing here looks at
            if (!as_target && bc1_on() && astc_storage_opaque(x)) {
                x.bc1 = true;
                x.format = VK_FORMAT_BC1_RGB_UNORM_BLOCK;
                uint64_t whole = 0;
                for (uint32_t l = 0; l < x.levels; l++) whole += (uint64_t)std::max(1u, x.width >> l) * std::max(1u, x.height >> l);
                const uint64_t saved = st_bc1_saved.fetch_add(whole * x.layers / 2) + whole * x.layers / 2;
                const uint64_t n = ++st_bc1_made;
                if (n <= 4 || (n & (n - 1)) == 0)
                    logf("vk: texture %#llx %ux%u x%u, %u levels: every block opaque, kept as BC1 (%llu so far, %llu MB less than "
                         "as BC3; %llu made BC3 again)", (unsigned long long)t->gpu, x.width, x.height, x.layers, x.levels,
                         (unsigned long long)n, (unsigned long long)(saved >> 20), (unsigned long long)st_bc1_back.load());
            }
        }
    }
    const VkFormatProperties fp = format_props(x.format);
    x.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) x.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageCreateFlags flags = 0;
    if (depth) {
        x.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else if (!is_bc(x.format) && !is_astc(x.format)) {
        // (any format of the class may be a view: attachments and storage where one of them can)
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        x.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (x.samples == VK_SAMPLE_COUNT_1_BIT) x.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    } else if (x.bc1) {
        // (Triforce: an image kept as BC1 is sampled and copied into, nothing else (texture_view gives it no other view):
        // its sRGB twin as a view format is all it needs, and the BC3 image it may become is made the same way, bc1_retype)
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    } else {
        // Raw storage views address one compressed block per texel (8 or 16 bytes).
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT |
                 VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        const VkFormat raw = bpb == 8 ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32B32A32_UINT;
        VkImageFormatProperties limits{};
        // (asked without STORAGE: AMD's driver checks that against the compressed format and says no, though it makes
        // the image and its raw views (RX 6600 XT, amdvlk); without them the game's BC encoders wrote nowhere and
        // Link's clothes showed stale blocks. If the image is then refused, it is made again without)
        // (RX_VK_BC_STORAGE=0: compressed images without it, as before v2.0.8 on AMD: issue 1278, an RX 6700 XT's GPU time 6-12 ms
        // a frame on v2.0.6 and 60-100 ms on v2.0.11, the game's BC encoders running there since 04675cf being the lead)
        static const bool bc_storage = !getenv("RX_VK_BC_STORAGE") || atoi(getenv("RX_VK_BC_STORAGE")) != 0;
        if (bc_storage && (format_props(raw).optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
            vkGetPhysicalDeviceImageFormatProperties(dev.physical, x.format, x.type, VK_IMAGE_TILING_OPTIMAL,
                x.usage, flags, &limits) == VK_SUCCESS)
            x.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        // (and colour attachment views, raw too: OoT renders the slices of two 64^3 BC4 noise volumes as RGBA16UI targets, one
        // texel a block (53509044401d78d2, while an area loads). Without the usage those views were not made, the volumes
        // stayed zero, and the foliage's dithered LOD crossfade, which reads them, left trees bare at mid distance, #1151.
        // 3D images take the usage on the driver's word for the 2D one: the query does not cover 2D_ARRAY_COMPATIBLE's
        // per-slice attachment views)
        // ((Triforce) not for 3D images on NVIDIA's Pascal and older (device IDs below 0x1e00: GTX 10 series, GTX 900): that
        // driver takes the usage and the per-slice views, then loses the device on the first draw or even a clear into one
        // (upstream #2327: GTX 1080, driver 581.80; here a GTX 1070, 581.80, at the area load after the opening cutscene and
        // at every save load). Without the usage the views are not made and the noise pass is left out there (draw(): trees
        // bare at mid distance, as before #1151) until upstream's separate-attachment path is ported. RX_VK_BC3D_TARGET=1:
        // the usage anyway, =0: never)
        static const int bc3d_env = getenv("RX_VK_BC3D_TARGET") ? atoi(getenv("RX_VK_BC3D_TARGET")) : -1;
        const bool old_nvidia = dev.props.vendorID == 0x10de && dev.props.deviceID < 0x1e00;
        const bool bc3d_target = bc3d_env >= 0 ? bc3d_env != 0 : !old_nvidia;
        if (x.type == VK_IMAGE_TYPE_3D && !bc3d_target) {
            static std::atomic<int> told{0};
            if (told++ == 0)
                logf("vk: 3D BC images are not made render targets (%s; NVIDIA before Turing loses the device on them): the "
                     "noise pass is left out (RX_VK_BC3D_TARGET=1: made anyway)",
                     bc3d_env == 0 ? "RX_VK_BC3D_TARGET=0" : "NVIDIA device id below 0x1e00");
        } else if (format_props(raw).optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) {
            // (asked with the flags a 3D image is made with below; a refused image is made again without the usage)
            const VkImageCreateFlags asked = flags | (x.type != VK_IMAGE_TYPE_3D ? 0u
                : VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT | (dev.view_2d_of_3d ? VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT : 0u));
            const VkResult r = vkGetPhysicalDeviceImageFormatProperties(dev.physical, x.format, x.type, VK_IMAGE_TILING_OPTIMAL,
                x.usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, asked, &limits);
            if (r == VK_SUCCESS || x.type == VK_IMAGE_TYPE_3D) x.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            static int told;
            if (r != VK_SUCCESS && told++ < 4)
                logf("vk: BC image %ux%ux%u (vk %d) as a raw colour attachment: the driver says %d%s", x.width, x.height, x.depth,
                     (int)x.format, (int)r, x.type == VK_IMAGE_TYPE_3D ? " (3D: taken)" : "");
        }
    }
    // ((Triforce, 11-18, EstebanGGM's port from NX2navi) on AMD's drivers only a cubemap's image is made cube compatible: a square
    // 2D array of 6 layers or more made so broke terrain textures on an RX 580 and an RX 7800 XT. Elsewhere as before (a cube view
    // of an image without the flag is refused: view_of), until a cube read of a non-cube target is ruled out there too.
    // RX_VK_CUBE_ONLY=1: on every GPU, =0: on none)
    static const int cube_env = getenv("RX_VK_CUBE_ONLY") ? atoi(getenv("RX_VK_CUBE_ONLY")) : -1;
    const bool cube_only = cube_env >= 0 ? cube_env != 0 : dev.props.vendorID == 0x1002;
    const bool cube_target = d.target == NVN_TEXTURE_TARGET_CUBEMAP || d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    if (x.type == VK_IMAGE_TYPE_2D && (!cube_only || cube_target) && x.layers >= 6 && x.width == x.height &&
        x.samples == VK_SAMPLE_COUNT_1_BIT)
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, x.cube = true;
    if (x.type == VK_IMAGE_TYPE_3D) {
        flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        if (dev.view_2d_of_3d) flags |= VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT;
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
    ci.imageType = x.type;
    ci.format = x.format;
    ci.extent = {x.width, x.height, x.depth};
    ci.mipLevels = x.levels;
    ci.arrayLayers = x.layers;
    ci.samples = x.samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = x.usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // (a render target's image, not compressed: from the pool when one of this create info waits there, above)
    const uint64_t pool_key = as_target && !x.sparse && pool_cap() && !is_bc(x.format) && !is_astc(x.format) &&
                                      (x.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
                                  ? pool_key_of(ci)
                                  : 0;
    x.pool_key = pool_key;
    x.create_flags = ci.flags;
    // (hookshot, RX_VK_COMPACT) an image that is not a render target's: from the textures' own pool, the one compacted
    const bool in_textures = compact_on() && !x.sparse && !(as_target && !is_bc(x.format) && !is_astc(x.format));
    if (x.sparse) {
        ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
        bool ok = vkCreateImage(dev.device, &ci, nullptr, &x.image) == VK_SUCCESS;
        if (ok) {
            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(dev.device, x.image, &mr);
            uint32_t n = 0;
            vkGetImageSparseMemoryRequirements(dev.device, x.image, &n, nullptr);
            std::vector<VkSparseImageMemoryRequirements> reqs(n);
            vkGetImageSparseMemoryRequirements(dev.device, x.image, &n, reqs.data());
            ok = false;
            for (const auto& r : reqs)
                if (r.formatProperties.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) x.sparse_req = r, ok = true;
            x.sparse_align = mr.alignment;
            x.sparse_type = UINT32_MAX;
            for (uint32_t i = 0; i < dev.memory.memoryTypeCount && x.sparse_type == UINT32_MAX; i++)
                if ((mr.memoryTypeBits >> i & 1) && (dev.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                    x.sparse_type = i;
            ok = ok && x.sparse_type != UINT32_MAX && x.sparse_align;
            if (!ok) vkDestroyImage(dev.device, x.image, nullptr), x.image = VK_NULL_HANDLE;
        }
        if (!ok) {
            logf("vk: sparse image %ux%u x%u levels %u format %d not made", x.width, x.height, x.layers, x.levels,
                 (int)x.format);
            return nullptr;
        }
        x.tiles.resize((size_t)x.layers * x.levels);
        x.tail_bound.assign(x.layers, 0);
        logf("vk: texture %#llx %ux%u x%u levels %u (NVN %#x): sparse, layers made resident as they are written "
             "(blocks %ux%u, mip tail from level %u, %llu KB%s)",
             (unsigned long long)t->gpu, x.width, x.height, x.layers, x.levels, d.format,
             x.sparse_req.formatProperties.imageGranularity.width, x.sparse_req.formatProperties.imageGranularity.height,
             x.sparse_req.imageMipTailFirstLod, (unsigned long long)(x.sparse_req.imageMipTailSize >> 10),
             x.sparse_req.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT ? ", one for all layers" : " a layer");
    } else if (in_textures && create_image(ci, x.image, x.allocation, false, false, true)) {
        // (made in the textures' pool; one refused there is made as any other, below)
    } else if (!(pool_key && pool_take(pool_key, x.image, x.allocation, x.views)) && (st_pool_made += pool_key != 0, true) &&
               !create_image(ci, x.image, x.allocation, false, as_target && !is_bc(x.format) && !is_astc(x.format)) &&
               // (a compressed image refused: made again without its raw attachment usage, then without its raw storage
               // usage, as #1332 does; its raw views are then not made, as before)
               !((ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && is_bc(x.format) &&
                 (ci.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, x.usage = ci.usage, create_image(ci, x.image, x.allocation))) &&
               !((ci.usage & VK_IMAGE_USAGE_STORAGE_BIT) && (is_bc(x.format) || is_astc(x.format)) &&
                 (ci.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT, x.usage = ci.usage, create_image(ci, x.image, x.allocation)))) {
        static int logged;
        if (logged++ < 16)
            logf("vk: image %ux%ux%u x%u levels %u format %d (NVN %#x) not made", x.width, x.height, x.depth, x.layers,
                 x.levels, (int)x.format, d.format);
        return nullptr;
    }
    // what it costs: the level chain's texels
    uint64_t px = 0;
    for (uint32_t l = 0; l < x.levels; l++)
        px += (uint64_t)std::max(1u, x.width >> l) * std::max(1u, x.height >> l) * std::max(1u, x.depth >> l);
    const uint32_t texel = x.bc || is_bc(x.format) ? 1 : astc ? 4 : std::max(1, from ? (from == 3 ? 4 : from == 6 ? 8 : 16) : bpb);
    x.bytes = x.sparse ? 0 : px * x.layers * texel * x.samples;   // (a sparse image's: as its layers are bound)
    if (x.bc1) x.bytes = tex_bc1::bytes(x.width, x.height, x.levels, x.layers);   // (Triforce: eight bytes a block, half of BC3's)
    // (the budget counts what the allocator gave it: the texels' 8% short of that over a scene's 2000 textures, 3228 MB counted for 3504
    // MB held, and a budget of 4035 MB took 4300. RX_VK_COUNT_BY_ESTIMATE=1: the texels)
    static const bool by_estimate = getenv("RX_VK_COUNT_BY_ESTIMATE") != nullptr;
    if (!x.sparse && !by_estimate) x.pad = std::max(x.bytes, allocation_size(x.allocation)) - x.bytes;
    total_bytes += x.bytes + x.pad;
    InitRedirect redirect;   // (a parallel replay's worker: into the earliest open segment's init command buffer)
    // into GENERAL, and cleared (a target the game renders into before writing it all reads zeros, not garbage)
    VkCommandBuffer cmd = upload_cmd();
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = x.image;
    b.subresourceRange = {x.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    if (depth) {
        const VkClearDepthStencilValue z{0.0f, 0};
        vkCmdClearDepthStencilImage(cmd, x.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &b.subresourceRange);
    } else if (!is_bc(x.format) && !is_astc(x.format)) {
        const VkClearColorValue z{};
        vkCmdClearColorImage(cmd, x.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &b.subresourceRange);
    }
    if (!is_bc(x.format) && !is_astc(x.format)) {
        // (the clear finished before anything after it in this command buffer writes the image: the upload of its contents
        // comes right after, a copy into the same image. Two transfer writes with no barrier between them may run in either
        // order; NVIDIA's ran in order, AMD's (a fast clear) landed last, so a texture uploaded in the recording that made it
        // read zeros there. The menus' text is that case: the CPU writes each string's letters into a new R8 texture, which is
        // drawn once into the string's label, so every menu and title text was blank on AMD cards)
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    Texture* p = tex.get();
    redirect.done();
    static uint32_t ids;
    p->id = ++ids;
    p->first_seen = frame_no;
    st_made++;
    p->key_layout = key.layout, p->key_cls = key.cls;
    {   // (another image at this storage: what tells them apart; content one renders the other does not see)
        static int told;
        auto [a, e] = by_gpu.equal_range(p->gpu);
        for (auto it = a; it != e && told < 80; ++it) {
            const Texture* o = it->second;
            told++;
            logf("vk: texture #%u at %#llx beside #%u: %ux%ux%u x%u levels %u layout %#x class %#x NVN %#x target %d, "
                 "the other %ux%ux%u x%u levels %u layout %#x class %#x NVN %#x target %d%s",
                 p->id, (unsigned long long)p->gpu, o->id, p->width, p->height, p->depth, p->layers, p->levels,
                 key.layout, key.cls, d.format, d.target, o->width, o->height, o->depth, o->layers, o->levels,
                 o->key_layout, o->key_cls, o->nvn_format, o->d.target, o->gpu_written ? " (rendered)" : "");
        }
    }
    textures.emplace(key, std::move(tex));
    by_gpu.emplace(p->gpu, p);
    watch_tex(p, "made");
    if (come_back_noted.load(std::memory_order_relaxed)) {   // ((Triforce) one the live line let go, back: the come-back pause)
        std::lock_guard lk(come_back_mu);
        come_back.made(evicted_key(p->gpu, p->width, p->height, p->nvn_format), frame_no, tex_rate.rate);
    }
    if (remade_log_on()) {   // (one evicted not long ago, made again)
        std::lock_guard lk(evicted_mu);
        auto ev = evicted_at.find(evicted_key(p->gpu, p->width, p->height, p->nvn_format));
        if (ev != evicted_at.end()) {
            const uint64_t ago = frame_no - ev->second.frame;
            if (ago < 600) {
                remade[p] = ago;
                static int told;
                if (told++ < 300)
                    logf("vk: texture #%u %#llx %ux%u NVN %#x made again %llu frames after it was evicted (%s then)", p->id,
                         (unsigned long long)p->gpu, p->width, p->height, p->nvn_format, (unsigned long long)ago,
                         ev->second.rendered ? "rendered" : "not rendered");
            }
            evicted_at.erase(ev);
        }
    }
    static int logged;
    if (logged++ < 40 || textures.size() % 500 == 0 || p->bytes > (64ull << 20))
        logf("vk: texture %#llx %ux%ux%u x%u levels %u NVN %#x -> vk %d%s (%zu textures, %llu MB)",
             (unsigned long long)p->gpu, p->width, p->height, p->depth, p->layers, p->levels, d.format, (int)p->format,
             astc ? " (ASTC decoded)" : "", textures.size(), (unsigned long long)(total_bytes >> 20));
    return p;
}

} // namespace

std::atomic<uint64_t> work_epoch{0};
namespace {
// ((Triforce, P4) the marks a rule reads (the last full barrier, the last pass's end) are the command buffer's own: cmd_work.h)
std::atomic<int64_t> st_barriers_made{0}, st_barriers_left_out{0};
// ((Triforce) RX_VK_EVENT_BARRIER, barrier_rules.h: the command buffer an event's write is held for, the next full barrier on it
// records it)
thread_local VkCommandBuffer late_cmd = VK_NULL_HANDLE;
/// (the held writes, right after a full barrier on cmd. They are work for the next barrier's look: a recording's write-backs copy
/// the events' words to the game's memory, behind a full barrier that must then not be left out)
void late_writes_after(VkCommandBuffer cmd) {
    late_cmd = VK_NULL_HANDLE;
    event_writes_record(cmd);
}
/// ((Triforce, P4) RX_VK_BARRIER_CONTEXT=0: the one count of every thread's commands (cmd_work.h), which the helper threads' work
/// moves too: while they record, the rules stand aside, as before. Per command buffer (unset) they hold beside them: a parallel
/// replay's segment is a command buffer of its own, begun afresh (its start a full barrier, its end one: vk_replay.cpp
/// record_segment) and recorded by one thread, so no rule relaxes across a segment's edge
bool rules_aside() {
    return work_global() && par_active.load(std::memory_order_relaxed);
}
} // namespace
void barrier_stats(int64_t* made, int64_t* left_out) {
    *made = st_barriers_made.load(std::memory_order_relaxed), *left_out = st_barriers_left_out.load(std::memory_order_relaxed);
}
void event_writes_hold(VkCommandBuffer cmd) {
    late_cmd = cmd;
}
bool event_writes_held(VkCommandBuffer cmd) {
    return late_cmd != VK_NULL_HANDLE && (cmd == VK_NULL_HANDLE || cmd == late_cmd);
}
void event_writes_flush(VkCommandBuffer cmd, const char* why) {
    if (event_writes_held(cmd)) full_barrier(cmd, why);
}

// (hookshot) A full barrier right after another one on the same command buffer, no command that does or orders GPU work in between
// (vk_work.h counts them), orders nothing the first did not: left out. The game's own barrier (end of a pass), then the next pass's
// start barrier, then a dispatch's, were two or three in a row. RX_VK_BARRIER_DEDUPE_OFF=1: all recorded, as before.
// ((Triforce, P4) "right after" by the command buffer's own count and mark (cmd_work.h), so beside the helper threads too)
void full_barrier(VkCommandBuffer cmd, const char* why) {
    static const bool off = [] {
        logf("vk: barrier state: %s", work_global()
                 ? "one count of every thread's commands (RX_VK_BARRIER_CONTEXT=0): the barrier rules stand aside while the helper "
                   "threads record, as before"
                 : "each command buffer's own: the barrier rules hold while the helper threads record too (RX_VK_BARRIER_CONTEXT=0: "
                   "one count of every thread's commands, as before)");
        return getenv("RX_VK_BARRIER_DEDUPE_OFF") != nullptr;
    }();
    CmdWork& w = cmd_work(cmd);
    const bool late = late_cmd != VK_NULL_HANDLE && cmd == late_cmd;   // ((Triforce) an event's write held for this barrier)
    if (!off && !rules_aside() && work_right_after(w, w.barrier_at)) {
        st_barriers_left_out.fetch_add(1, std::memory_order_relaxed);
        if (late) late_writes_after(cmd);   // (right after the barrier before, which this one would have repeated)
        return;
    }
    if (!w.known) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true))
            logf("vk: barrier state: no room for another command buffer's (%zu kept): no rule leaves a barrier out on it", kCmdWorkSlots);
    }
    st_barriers_made.fetch_add(1, std::memory_order_relaxed);
    GprofSpan gp(cmd, why);   // (RX_VK_PASS_TIMING: what a barrier costs the GPU that has nothing left to wait for)
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    w.barrier_at = work_at(w);
    if (late) late_writes_after(cmd);
}

namespace {
std::atomic<int64_t> st_pass_relaxed{0};
} // namespace
void note_pass_end(VkCommandBuffer cmd) {
    CmdWork& w = cmd_work(cmd);
    w.pass_end_at = work_at(w);
}
void note_pass_split(VkCommandBuffer cmd) {
    cmd_work(cmd).pass_end_at = UINT64_MAX;   // ((Triforce) the next pass start is a full barrier: pass_barrier.h)
}
int64_t pass_barriers_relaxed() { return st_pass_relaxed.load(std::memory_order_relaxed); }
void pass_start_barrier(VkCommandBuffer cmd) {
    // ((Triforce) the game mode on by default with NVIDIA's driver; RX_VK_PASS_BARRIER=full: every pass start a full barrier, as
    // before; =game: the game mode on any card. pass_barrier.h has the rule and why)
    static const bool game = [] {
        const char* e = getenv("RX_VK_PASS_BARRIER");
        const bool every = getenv("RX_VK_BARRIER_EVERY_DRAW") != nullptr;
        const bool on = pass_barrier::game(e, dev.props.vendorID, every);
        const char* why = every ? "RX_VK_BARRIER_EVERY_DRAW" : e && *e ? "RX_VK_PASS_BARRIER="
                                                                       : dev.props.vendorID == 0x10DE ? "NVIDIA's driver" : "not NVIDIA's driver";
        logf("vk: pass barriers: %s (%s%s; RX_VK_PASS_BARRIER=full: a full one at every pass start, =game: the fragment stages only)",
             on ? "a draw pass right after another, no barrier of the game's between, waits in its fragment stages only"
                : "a full barrier at every pass start",
             why, !every && e && *e ? e : "");
        return on;
    }();
    // ((Triforce, P4) the pass ended on this command buffer with nothing recorded into it since, by its own count (cmd_work.h): beside
    // the helper threads too)
    CmdWork& w = cmd_work(cmd);
    if (!pass_barrier::relaxed(game && !rules_aside(), work_right_after(w, w.pass_end_at))) {
        full_barrier(cmd, "barrier (pass start)");
        return;
    }
    // (the previous pass's writes, all stages, made visible to this pass's fragment-side stages; its vertex, task and mesh stages run
    // as soon as they can, as on the console's GPU)
    st_pass_relaxed.fetch_add(1, std::memory_order_relaxed);
    GprofSpan gp(cmd, "barrier (pass start, fragments)");
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    w.barrier_at = UINT64_MAX;   // (not a full one: the next full barrier is not left out after it)
}

namespace {
std::atomic<int64_t> st_pass_resumed{0};
} // namespace
int64_t pass_resumes() { return st_pass_resumed.load(std::memory_order_relaxed); }
void pass_resume_barrier(VkCommandBuffer cmd) {
    // ((Triforce) RX_VK_SPLIT_BARRIER, barrier_rules.h: the pass a part's split ended, begun again in the next part. Right after a
    // full barrier (something was recorded between: vk_replay.cpp maybe_split) nothing more is needed)
    CmdWork& w = cmd_work(cmd);
    if (work_right_after(w, w.barrier_at)) return;
    // (the attachments' outputs of the draws before, before those of the draws after: what one pass orders. What the draws' shaders
    // read of the targets is draw_hazards', which goes on with the pass's sampled and written targets)
    st_pass_resumed.fetch_add(1, std::memory_order_relaxed);
    GprofSpan gp(cmd, "barrier (a part's split inside a pass: the attachments)");
    const VkPipelineStageFlags2 out = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = out;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    w.barrier_at = UINT64_MAX;   // (not a full one)
}

void texture_begin_recording(VkCommandBuffer upload, uint64_t value) {
    // ((Triforce, P4) a new recording's command buffers start afresh at their begin, whatever their handles held: vk_work.h work_begun)
    up_cmd = upload;
    if (value != rec_value) epoch++;   // (a submission's parts check a texture's memory once: nothing is collected between)
    rec_value = value;
    up_used = false;
    memory_table_begin();
    memory_rb_recording_begun();   // (which readback pages its uploads may refresh: memory_rb_refresh)
}
VkCommandBuffer upload_command_buffer() {
    return upload_cmd();
}
bool texture_uploads_recorded() {
    if (memory_rb_refresh(up_cmd)) up_used = true;   // (PR 430: readback pages from guest memory, last of the uploads)
    memory_mirror_binds_flush();   // (Triforce: the memory of the mirrors' new pieces, bound before this recording is submitted)
    memory_table_flush();   // (vk_memory.cpp: the page table's changes, after this recording's mirror copies)
    if (up_used) full_barrier(up_cmd);   // (before the recording that reads them)
    return up_used;
}

int view_nvn_format(const nvn_tex* t, const nvn_view* v) {
    return v && v->set_format ? v->format : t->d.format;
}
unsigned view_level(const nvn_view* v) {
    return v && v->set_levels ? v->base_level : 0;
}
unsigned image_level(const Texture* tex, unsigned level) {
    return level > tex->skip ? std::min(level - tex->skip, tex->levels - 1) : 0;
}
unsigned view_layer(const nvn_view* v) {
    return v && v->set_layers ? v->min_layer : 0;
}

namespace {
std::atomic<int> detail_auto{-1};   // (what the video memory alone gives this card: texture_detail_auto)
}
int texture_detail_auto() { return detail_auto.load(std::memory_order_relaxed); }
int texture_detail_level() {
    static const int level = [] {
        const int asked = tex_detail::parse(getenv("RX_VK_TEX_DETAIL"));
        // (the Insert menu's Texture detail row as the options file had it at the start, vk_menu.h: Automatic unless set)
        const int row = tex_detail::from_row(tex_detail_at_start);
        uint64_t used = 0, allowed = 0;
        const bool known = vram_usage(&used, &allowed);
        const bool integrated = dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        const int by_memory = tex_detail::by_vram(known ? allowed : 0, integrated);
        detail_auto.store(by_memory, std::memory_order_relaxed);
        const int l = tex_detail::choose(asked, row, by_memory);
        if (asked >= 0)
            logf("vk: texture detail: %s (RX_VK_TEX_DETAIL)", tex_detail::name(l));
        else if (row >= 0)
            logf("vk: texture detail: %s (the Texture detail row; Automatic gives %s for the %llu MB of VRAM allowed here)",
                 tex_detail::name(l), tex_detail::name(by_memory), (unsigned long long)(allowed >> 20));
        else if (l)
            logf("vk: texture detail: %s, for the %llu MB of VRAM allowed here%s (full from %llu MB, low under %llu; "
                 "RX_VK_TEX_DETAIL=full|reduced|low says otherwise)", tex_detail::name(l),
                 (unsigned long long)(allowed >> 20), integrated ? " on an integrated GPU" : "",
                 (unsigned long long)tex_detail::kFullMb, (unsigned long long)tex_detail::kLowMb);
        return l;
    }();
    return level;
}
void texture_want_whole(Texture* tex) {
    if (!tex || !tex->detail_skip || tex->whole_asked) return;
    ParLock lk;
    if (tex->whole_asked) return;
    tex->whole_asked = true;
    whole_pending.push_back(tex);
}

/// The image the game's texture has: its storage and what makes two textures over one storage different images
static Key key_of(const nvn_tex* t, const VkFormatInfo& fi, int bpb) {
    Key k{};
    k.gpu = t->gpu;
    k.target = (uint32_t)t->d.target;
    // (a cube map and a 2D array of 6 layers over the same storage are one image; MS targets stay apart)
    if (k.target == NVN_TEXTURE_TARGET_CUBEMAP) k.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    if (k.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) k.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    if (k.target == NVN_TEXTURE_TARGET_RECTANGLE) k.target = NVN_TEXTURE_TARGET_2D;
    k.width = t->d.width, k.height = t->d.height;
    k.depth = t->d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 : t->d.depth;
    if (k.target == NVN_TEXTURE_TARGET_2D) k.depth = 1;
    k.levels = std::max(1u, t->d.levels);
    k.samples = std::max(1, t->d.samples);
    k.layout = nvn_tex_is_linear(&t->d) ? 0x80000000u | (t->d.stride ? t->d.stride : host->tex_linear_stride(&t->d))
                                        : t->L.tileH | t->L.tileD << 4;
    k.cls = format_class(fi.format, t->d.format, bpb, fi.astc);
    return k;
}

bool create_later(bool for_sampling);   // ((Triforce) below, with the new textures' budget)
void create_timed(int64_t ns);
Texture* texture_get(const nvn_tex* t, bool for_sampling) {
    PROF_HOT(PROF_TEXGET);
    if (!t || !t->gpu || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return nullptr;
    const VkFormatInfo fi = vk_format(t->d.format);
    if (fi.format == VK_FORMAT_UNDEFINED) {
        static int seen[256];
        if (t->d.format >= 0 && t->d.format < 256 && seen[t->d.format]++ < 2)
            logf("vk: NVN format %#x has no Vulkan format: its textures are left out", t->d.format);
        return nullptr;
    }
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(t->d.format, &bw, &bh);
    if (bpb <= 0) return nullptr;
    const Key k = key_of(t, fi, bpb);
    Texture* tex;
    auto it = textures.find(k);
    if (it == textures.end()) {
        if (create_later(for_sampling)) return nullptr;   // (Triforce: not made in this frame, the dummy in its place)
        const auto c0 = std::chrono::steady_clock::now();
        tex = create(k, t, fi.format, fi.astc, bw, bh, bpb, !for_sampling);
        create_timed(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - c0).count());
        if (!tex) return nullptr;
        static const int64_t log_from = getenv("RX_VK_LOG_MADE") ? atoll(getenv("RX_VK_LOG_MADE")) : -1;
        static int made_logged;
        if (log_from >= 0 && (int64_t)frame_no >= log_from && made_logged++ < 300) {
            auto [a, e] = by_gpu.equal_range(tex->gpu);
            int others = 0;
            for (auto it = a; it != e; ++it) others += it->second != tex;
            logf("vk: made #%u %#llx %ux%ux%u x%u levels %u NVN %#x target %d tileH %u, first %s; %d other images at it",
                 tex->id, (unsigned long long)tex->gpu, t->d.width, t->d.height, t->d.depth, tex->layers, t->d.levels,
                 t->d.format, t->d.target, t->L.tileH, for_sampling ? "sampled" : "rendered", others);
        }
    } else {
        tex = it->second.get();
    }
    return texture_touch(tex, for_sampling);
}

// ---- (Triforce, 11-19) Textures made and uploaded ahead, while the frames are light (tex_warm.h, RX_VK_TEX_WARM=1) ----
namespace {
std::mutex warm_mu;
std::deque<std::pair<int, uint64_t>> warm_pending;   // (a registered slot, the frame it was noted in; warm_mu)
// ((Triforce) the slots in warm_pending (warm_mu): one entry a slot. The game registers its slots again and again in play (every
// dynamic-resolution step), and the list filled to kMaxPending with the same render targets, dropping the next area's slots)
std::unordered_set<int> warm_noted;
int64_t st_warm_looked, st_warm_made, st_warm_ns, st_warm_steps;   // (the recording thread's)
uint64_t warm_frame = 0;        // (the frame warm_frame_ns is of)
int64_t warm_frame_ns = 0;      // (this frame's looks ahead so far, over its submissions)
// (what texture_frame last knew of the card's memory and the textures' budget: tex_warm::has_room)
bool warm_known = false;
uint64_t warm_used = 0, warm_allowed = 0, warm_budget = 0;
}
bool texture_warm_on() {
    static const bool on = [] {
        const bool v = tex_warm::on_of(getenv("RX_VK_TEX_WARM"));
        if (v)
            logf("vk: textures made and uploaded ahead in light frames (RX_VK_TEX_WARM): registered slots, %lld ms a frame while the "
                 "frame before had under %lld draws and dispatches", (long long)(tex_warm::kBudgetNs / 1000000),
                 (long long)tex_warm::kLightWork);
        return v;
    }();
    return on;
}
/// A slot was registered (any thread): noted for texture_warm_step
void texture_warm_note(int slot) {
    if (!texture_warm_on()) return;
    std::lock_guard lk(warm_mu);
    if (warm_pending.size() < tex_warm::kMaxPending && warm_noted.insert(slot).second) warm_pending.push_back({slot, frame_no});
}
/// At a submission's start, on the thread that records it, in its recording: when the frame before was light, the noted
/// slots old enough are looked up as a draw's first look does (texture_get: made, and its bytes uploaded by this recording's
/// upload command buffer), until the budget is taken. last_frame_work: the draws and dispatches of the frame before
void texture_warm_step(int64_t last_frame_work) {
    if (warm_frame != frame_no) warm_frame = frame_no, warm_frame_ns = 0;
    if (!tex_warm::may_warm(texture_warm_on(), last_frame_work, frame_upload_ns.load(std::memory_order_relaxed), warm_frame_ns)) return;
    if (!tex_warm::has_room(warm_known, warm_used, warm_allowed, total_bytes, warm_budget)) return;
    const auto t0 = std::chrono::steady_clock::now();
    int64_t spent = 0;
    bool any = false;
    for (;;) {
        std::pair<int, uint64_t> e;
        {
            std::lock_guard lk(warm_mu);
            if (warm_pending.empty() || !tex_warm::ripe(warm_pending.front().second, frame_no)) break;
            e = warm_pending.front();
            warm_pending.pop_front();
            warm_noted.erase(e.first);
        }
        nvn_tex t;
        nvn_view v;
        bool has_view = false;
        if (!texture_of(e.first, &t, &v, &has_view) || !t.gpu) continue;   // (registered anew as nothing since, or let go)
        any = true;
        st_warm_looked++;
        {
            ParLock lk;
            const size_t had = textures.size();
            texture_get(&t, true);
            st_warm_made += textures.size() != had;
        }
        spent = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        if (!tex_warm::goes_on(warm_frame_ns + spent, frame_upload_ns.load(std::memory_order_relaxed))) break;
    }
    warm_frame_ns += spent;
    if (!any) return;
    st_warm_ns += spent, st_warm_steps++;
    static int64_t told;
    if (st_warm_made - told >= 256) {
        size_t left;
        {
            std::lock_guard lk(warm_mu);
            left = warm_pending.size();
        }
        told = st_warm_made;
        logf("vk: textures made ahead in light frames (RX_VK_TEX_WARM): %lld made of %lld looked up, in %lld submissions' starts, "
             "%.0f ms in all; %zu slots noted and not looked up yet", (long long)st_warm_made, (long long)st_warm_looked,
             (long long)st_warm_steps, st_warm_ns / 1e6, left);
    }
}

uint64_t texture_destroy_generation() {
    return destroy_gen;
}
// (Triforce) The frames that left textures for later bumped destroy_gen, which every view a draw resolved is checked against:
// every frame of an area change, while the budget left new textures for the next one, looked up again every texture of every
// draw, and built every draw's descriptors anew, on each recording thread (in the owner's test7 session such a frame had 166.9 ms
// of descriptors in 296; how much of it was this is not known). Only the looks that resolved to the dummy for want of the
// texture need doing again: they are checked
// against this (vk_draw.cpp texture_view_of). RX_VK_DUMMY_GEN=0: destroy_gen, as before
uint64_t dummy_gen = 1;
uint64_t texture_dummy_generation() {
    return dummy_gen;
}
bool dummy_gen_on() {
    static const bool on = !(getenv("RX_VK_DUMMY_GEN") && getenv("RX_VK_DUMMY_GEN")[0] == '0');
    return on;
}

uint64_t texture_destroyed_count() {
    return gone_record.count();
}

bool texture_destroyed_since(uint32_t id, uint64_t since) {
    return gone_record.gone_since(id, since);
}

bool texture_views_gone_since(uint64_t since, std::vector<uint32_t>& out, uint64_t* now) {
    return views_gone.went_since(since, out, now);
}

uint32_t texture_epoch() {
    return epoch;
}

// ---- (Triforce, diagnostics) RX_VK_AUDIT_GPU_ONLY=1: where the game makes textures out of memory only the GPU drew into ----
// On the console what the GPU draws into a texture is in that texture's storage, and the game may use those bytes later: copy
// them, keep them in a cache, make another texture over them (the terrain's normal maps: bake_writeback_step). Here they are
// only in the image unless something writes them back. With this switch a texture's storage is filled with a mark when the GPU
// first draws into it (the word "GPUo" and the texture's number, over and over), and every texture uploaded from memory is
// searched for marks: one that holds them is a texture the game expects the GPU's texels in, wherever the game has moved
// them since, and is logged with the texture the marks are of. A lab switch: the marks replace what that memory held, so a
// game that reads it on the CPU gets them too. Not marked: a texture that had the CPU's texels before the GPU's (mixed
// content), virtual pools, storage over 64 MB.
void bake_note_written(Texture* t);   // (below: what the GPU baked, into the game's memory)
constexpr uint32_t kAuditMark = 0x6f555047;   // ("GPUo")
bool audit_on() {
    static const bool on = getenv("RX_VK_AUDIT_GPU_ONLY") && atoi(getenv("RX_VK_AUDIT_GPU_ONLY")) != 0;
    return on;
}
struct AuditSource { uint32_t w, h, levels, layers; int format; uint64_t gpu, frame; };
std::unordered_map<uint32_t, AuditSource> audit_sources;   // (a marked texture's number -> what it was)
void audit_mark(const Texture& t) {
    if (!t.guest_size || t.guest_size > (64ull << 20) || t.gpu >= (1ull << 39)) return;
    static thread_local std::vector<uint32_t> words;
    words.resize((size_t)(t.guest_size / 8) * 2);
    for (size_t i = 0; i + 1 < words.size(); i += 2) words[i] = kAuditMark, words[i + 1] = t.id;
    host->gpu_write(t.gpu, words.data(), words.size() * 4);
    audit_sources[t.id] = {t.width, t.height, t.levels, t.layers, t.nvn_format, t.gpu, frame_no};
}
void audit_scan(const Texture& t) {
    if (!audit_on() || t.guest_size < 16 || t.guest_size > (64ull << 20) || t.gpu >= (1ull << 39)) return;
    static thread_local std::vector<uint8_t> mem;
    mem.resize((size_t)t.guest_size);
    if (!host->gpu_read(t.gpu, mem.data(), mem.size())) return;
    std::unordered_map<uint32_t, uint64_t> hits;   // (the marked texture's number -> bytes of its marks here)
    const size_t n = mem.size() / 4;
    for (size_t i = 0; i + 1 < n; i++) {
        uint32_t w[2];
        memcpy(w, mem.data() + i * 4, 8);
        if (w[0] == kAuditMark) hits[w[1]] += 8, i++;
    }
    static std::unordered_set<uint64_t> told;
    for (const auto& [id, bytes] : hits) {
        if (bytes < 256 || told.size() >= 800 || !told.insert((uint64_t)t.id << 32 | id).second) continue;
        const auto s = audit_sources.find(id);
        char src[160] = "";
        if (s != audit_sources.end())
            snprintf(src, sizeof src, " (%ux%u x%u levels %u NVN %#x at %#llx, first drawn at frame %llu%s)", s->second.w, s->second.h,
                     s->second.layers, s->second.levels, s->second.format, (unsigned long long)s->second.gpu,
                     (unsigned long long)s->second.frame, s->second.gpu == t.gpu ? "; the same storage" : "; other storage: the bytes were moved");
        logf("vk: audit: texture #%u %#llx %ux%ux%u x%u levels %u NVN %#x, sampled by %016llx at frame %llu, is uploaded from memory "
             "that holds %llu of %llu KB of what only the GPU drew: texture #%u%s", t.id, (unsigned long long)t.gpu, t.width, t.height,
             t.depth, t.layers, t.levels, t.nvn_format, (unsigned long long)tl_watch_prog, (unsigned long long)frame_no,
             (unsigned long long)(bytes >> 10), (unsigned long long)(t.guest_size >> 10), id, src);
    }
}

thread_local bool upload_defer_ok = false;
bool texture_redrawn(const Texture* t) {
    if (!t) return true;
    static const uint64_t window = [] {   // ((Triforce, 11-19) redrawn.h kWindow; RX_VK_REDRAWN_FRAMES=1: v0.5.2's frame-before rule)
        const char* e = getenv("RX_VK_REDRAWN_FRAMES");
        const long v = e && *e ? strtol(e, nullptr, 10) : 0;
        return v >= 1 && v <= 600 ? (uint64_t)v : redrawn::kWindow;
    }();
    return redrawn::drawn_before(t->prev_written, t->last_written, t->gpu_written && !t->released, frame_no, window);
}
/// (Triforce) The hold of the last picture (present_hold.h): the mode do_present sets for the draws of the next frame, this
/// frame's looks that left a texture owed its first bytes, and the new textures left for later
std::atomic<int> hold_mode{present_hold::AS_BEFORE};
std::atomic<int64_t> frame_owed{0}, st_new_later{0}, st_new_later_frame{0};
void texture_hold_mode(int mode) { hold_mode.store(mode, std::memory_order_relaxed); }
int64_t texture_frame_owed() { return frame_owed.load(std::memory_order_relaxed); }
int64_t texture_new_later() {
    return st_new_later.load(std::memory_order_relaxed) + st_deferred.load(std::memory_order_relaxed);
}
/// (Triforce) A new texture (its image has no bytes yet) a draw samples, met once this frame's uploads took RX_VK_UPLOAD_NEW_MS
/// (12) or RX_VK_UPLOAD_NEW_MB (64), while the hold of the last picture may leave textures for later (present_hold.h DEFERS):
/// its first upload is left for the next frame (upload_owed::later), the draw samples the dummy in its place, and the frame is
/// held, so that what was drawn without it is never shown. At an area change one frame took every new texture's upload: 523 ms
/// for 363 MB in the owner's test5 session, and the GPU then worked 60 ms more through that backlog, which the next frame's
/// capture waited for. 12 ms: a frame at 60 presents a second is 16.7 ms, and the frames of an area change spend a few more in
/// their draws and memory sync, so they stay near the frame rate and the game's time step near its own; 64 MB: what the GPU
/// copies, unswizzles or decodes in a few milliseconds, so that no frame's uploads leave it a backlog of hundreds of MB. The
/// first upload of a frame always goes, whatever its size. Not a dispatch's (upload_defer_ok), not for the hold's flush, not
/// one whose upload found no memory, nor one left so upload_owed::kLaterMax frames in a row (upload_owed::may_leave_first)
bool new_texture_over_budget() {
    static const int64_t ns_budget = (int64_t)((getenv("RX_VK_UPLOAD_NEW_MS") ? atof(getenv("RX_VK_UPLOAD_NEW_MS")) : 12.0) * 1e6);
    static const int64_t byte_budget = (int64_t)((getenv("RX_VK_UPLOAD_NEW_MB") ? atof(getenv("RX_VK_UPLOAD_NEW_MB")) : 64.0) * 1048576.0);
    return (ns_budget > 0 && frame_upload_ns.load(std::memory_order_relaxed) >= ns_budget) ||
           (byte_budget > 0 && frame_upload_bytes.load(std::memory_order_relaxed) >= byte_budget);
}
bool new_texture_later(const Texture&) {
    if (!upload_defer_ok || hold_mode.load(std::memory_order_relaxed) != present_hold::DEFERS || !new_texture_over_budget())
        return false;
    st_new_later.fetch_add(1, std::memory_order_relaxed), st_new_later_frame.fetch_add(1, std::memory_order_relaxed);
    frame_owed.fetch_add(1, std::memory_order_relaxed);
    return true;
}
/// (Triforce) A texture whose first upload the budget left for the next frame, met again in this frame: by a draw while the hold
/// may leave textures (it samples the dummy again, and the frame is counted owed), or not (a dispatch, the hold's flush: it is
/// uploaded now)
bool new_texture_waits() {
    if (!upload_defer_ok || hold_mode.load(std::memory_order_relaxed) != present_hold::DEFERS) return false;
    frame_owed.fetch_add(1, std::memory_order_relaxed);
    return true;
}
/// (Triforce, layer_split.h) A new layered ASTC texture's first upload in parts: whether its upload now is one (part, and what is
/// left after it in rest; rest empty: the last part). A texture whose parts are under way (its owed ranges, not uploaded yet)
/// goes on; one not begun is split when its texels are more than a part may decode and the hold may leave textures for later.
/// A dispatch's look or the hold's flush takes what is left at once
// (Triforce) A sampled texture seen for the first time is made (its image, memory, views, and for ASTC the look at its
// blocks) in texture_get, before its upload is asked about: a texture the frame's budget left for later was made all the same.
// In the owner's test8 session the frame of an area change that left 766 new textures for the next had 62 ms of texture
// lookups. Now one seen once this frame's uploads and makes took the new textures' budget (new_texture_over_budget, or
// RX_VK_CREATE_NEW_MS of makes, 8), by a draw while the hold may leave textures, is not made yet: the draw samples the dummy, the
// frame is held, and the next frame's look makes it. The frame's first make always goes. RX_VK_CREATE_NEW_MS=0: made at once,
// as before
std::atomic<int64_t> frame_create_ns{0}, st_created{0}, st_created_ns{0}, st_create_later{0};
void create_timed(int64_t ns) {
    frame_create_ns.fetch_add(ns, std::memory_order_relaxed);
    st_created.fetch_add(1, std::memory_order_relaxed), st_created_ns.fetch_add(ns, std::memory_order_relaxed);
}
bool create_later(bool for_sampling) {
    static const int64_t budget = (int64_t)((getenv("RX_VK_CREATE_NEW_MS") ? atof(getenv("RX_VK_CREATE_NEW_MS")) : 8.0) * 1e6);
    if (!budget || !for_sampling || !upload_defer_ok || hold_mode.load(std::memory_order_relaxed) != present_hold::DEFERS) return false;
    const int64_t made = frame_create_ns.load(std::memory_order_relaxed);
    if (!made || (made < budget && !new_texture_over_budget())) return false;
    st_create_later.fetch_add(1, std::memory_order_relaxed);
    frame_owed.fetch_add(1, std::memory_order_relaxed);   // (the frame drew the dummy in a texture's place: held, and its looks stale)
    return true;
}
/// The textures made so far and their time, and those not made yet for the budget, for the stall line
void texture_create_stats(int64_t* made, int64_t* made_ns, int64_t* later) {
    *made = st_created.load(std::memory_order_relaxed), *made_ns = st_created_ns.load(std::memory_order_relaxed);
    *later = st_create_later.load(std::memory_order_relaxed);
}
std::atomic<int64_t> st_split_parts{0}, st_split_textures{0};
int64_t texture_split_parts() { return st_split_parts.load(std::memory_order_relaxed); }   // (the stall line's)
bool split_pending(const Texture& tex) {
    return !tex.uploaded && tex.astc && tex.layers > 1 && !tex.owed.whole && !tex.owed.ranges.empty() && !tex.owed.failed;
}
bool split_take(const Texture& tex, upload_owed::Ranges& part, upload_owed::Ranges& rest) {
    static const uint64_t per = layer_split::budget(getenv("RX_VK_UPLOAD_SPLIT_MB"));
    if (!per || tex.uploaded || !tex.astc || tex.layers < 2 || tex.bc1 || tex.sparse || tex.type != VK_IMAGE_TYPE_2D ||
        !tex.L.layer_size)
        return false;
    const bool going = split_pending(tex);
    const uint32_t n = layer_split::parts(layer_split::texel_bytes(tex.width, tex.height, tex.levels, tex.layers), tex.layers, per);
    const bool may = upload_defer_ok && hold_mode.load(std::memory_order_relaxed) == present_hold::DEFERS;
    if (!going && (n < 2 || !may)) return false;
    const upload_owed::Ranges remaining = going ? tex.owed.ranges : upload_owed::Ranges{{0, (uint64_t)tex.layers * tex.L.layer_size}};
    if (!may) {
        part = remaining, rest.clear();
        return true;
    }
    layer_split::take(remaining, tex.L.layer_size, layer_split::per_frame(tex.layers, std::max(n, 2u)), part, rest);
    return true;
}
namespace {
/// (hookshot) A new ASTC texture a draw samples, met after this frame's uploads took their budget: not uploaded now, the draw
/// samples the placeholder, and the next frame's first look at it uploads it. An area's load made 300-800 ASTC uploads in one
/// frame, 200-600 ms of the replay thread (the "a frame's uploads took" lines); now that is spread over several frames, the
/// textures coming in a few frames late. ASTC only: those are the materials' colour, normal and mask textures; uncompressed
/// ones can be data (height maps, look-up tables) that a frame without would get wrong. RX_VK_UPLOAD_BUDGET_MS=<n>: the
/// budget (40: only an area load's burst; at 8 the ground's streamed textures waited a frame in ordinary play and the ground
/// flickered, Discord 2026-11-09), 0: no deferral, as before
bool upload_defer(const Texture& tex) {
    static const int64_t budget_ns = (int64_t)(getenv("RX_VK_UPLOAD_BUDGET_MS") ? atof(getenv("RX_VK_UPLOAD_BUDGET_MS")) : 40.0) * 1000000;
    if (!upload_defer_ok || tex.uploaded || !tex.astc || tex.gpu_written) return false;
    // (Triforce, the hold of the last picture: present_hold.h) While the hold may leave textures for later, a new ASTC texture
    // has the new textures' budget too (new_texture_later: its decode on the GPU is part of the backlog its bytes make), and its
    // frame is held rather than shown with the placeholder; at the hold's bound none is left for later. Otherwise its own rule
    // below, as before, the frame then shown with the placeholder
    const int mode = hold_mode.load(std::memory_order_relaxed);
    if (mode == present_hold::FLUSH) return false;
    if (mode == present_hold::DEFERS && new_texture_over_budget()) {
        st_deferred.fetch_add(1, std::memory_order_relaxed), st_deferred_frame.fetch_add(1, std::memory_order_relaxed);
        frame_owed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (!budget_ns) return false;
    // (Triforce) and the first RX_VK_UPLOAD_MIN_ASTC of a frame (24) are never left for later. The budget counts every upload of
    // the frame, and a frame whose other uploads took it all left every new ASTC texture on the placeholder: on a card at its
    // VRAM allowance the render targets evicted and made again are uploaded from memory on the CPU, 58 to 190 ms a frame for
    // frames on end (an RX 5700 XT's log, 2026-11-11: 8,586 textures left for later in one session), and the materials stayed
    // flat white or blue for as long as that went on. An ASTC upload is decoded on the GPU: its part of such a frame is small
    static const int min_astc = getenv("RX_VK_UPLOAD_MIN_ASTC") ? std::max(0, atoi(getenv("RX_VK_UPLOAD_MIN_ASTC"))) : 24;
    if (frame_astc_admitted.fetch_add(1, std::memory_order_relaxed) < min_astc) return false;
    if (frame_upload_ns.load(std::memory_order_relaxed) < budget_ns) return false;
    st_deferred.fetch_add(1, std::memory_order_relaxed), st_deferred_frame.fetch_add(1, std::memory_order_relaxed);
    frame_owed.fetch_add(1, std::memory_order_relaxed);   // (Triforce: drawn with the placeholder; the hold, where it is on)
    return true;
}
}  // namespace
/// (Triforce) A texture uploaded before whose memory was written again, met by a draw after this frame's uploads took
/// RX_VK_UPLOAD_AGAIN_MS (16): its upload is left for the next frame, and the draws sample its last upload's bytes until then.
/// In the owner's test4 logs most uploads over 15 ms were such (36 of 62): screen-sized images of 8 to 30 MB the game writes
/// again, 19 to 24 ms each, and a frame had up to 120 ms of uploads. New textures are never left so (their image holds nothing
/// yet; new ASTC ones have their own rule, upload_defer), nor the first upload of a frame, nor a dispatch's, nor one left so for
/// upload_owed::kLaterMax frames in a row. RX_VK_UPLOAD_AGAIN_MS=0: every one now, as before
/// (Triforce, 11-16) and not one left so RX_VK_UPLOAD_AGAIN_FRAMES frames in a row (2): its last bytes are drawn a frame or two
/// at most (upload_owed::may_wait_again). With the budget's 16 ms nothing in the lab's runs was ever left (the frames with
/// re-upload bursts took 9.8 ms of uploads); a smaller budget (RX_VK_UPLOAD_AGAIN_MS=3) spreads a burst over the frames
/// after it, its textures a frame or two late meanwhile, which is why it is not the default. RX_VK_UPLOAD_AGAIN_FRAMES=8: as
/// before
std::atomic<int64_t> st_again_later{0}, st_again_later_frame{0};
uint32_t upload_again_frames() {
    static const uint32_t n = getenv("RX_VK_UPLOAD_AGAIN_FRAMES")
                                  ? (uint32_t)std::clamp(atoi(getenv("RX_VK_UPLOAD_AGAIN_FRAMES")), 0, (int)upload_owed::kLaterMax)
                                  : 2u;
    return n;
}
// ---- (Triforce, wp18-flash) a re-upload of storage the CPU was rewriting when its submit was captured (upload_torn.h) ----
bool memory_rewritten_since_capture(uint64_t guest, uint64_t n);   // (vk_memory.cpp)
namespace {
std::mutex torn_mu;   // (the looks come from the segments' threads too)
upload_torn::Recent torn_recent;   // (torn_mu)
std::atomic<int64_t> st_torn_found{0}, st_torn_found_frame{0}, st_torn_later{0}, st_torn_later_frame{0};
bool torn_on() {
    static const bool v = [] {
        const bool on = upload_torn::on(getenv("RX_VK_UPLOAD_TORN"));
        logf("vk: a re-upload whose storage the CPU rewrote since its capture %s (RX_VK_UPLOAD_TORN%s)",
             on ? "keeps its last bytes for that frame, the next frame's capture uploaded" : "is uploaded at once, as before",
             on ? "=0: at once" : "");
        return on;
    }();
    return v;
}
}  // namespace
/// (Triforce, wp18-flash) Whether a draw's re-upload of tex is left for the next frame because the CPU rewrote its storage since
/// the copies the upload would read (upload_torn.h: test29's white frames, a copy taken while the game was still writing the
/// texture). Not one left so (or by the budget) the frame before (upload_torn::kFrames), nor one found so in the kGap presents
/// before (the CPU rewrites it at every frame: uploaded as before). RX_VK_UPLOAD_TORN=0: never (such re-uploads are still found
/// and counted, for the log's line: the lab's measure of how often a copy may be taken mid-write)
bool torn_later(const Texture& tex) {
    if (!upload_torn::wanted(true, tex.gpu, tex.guest_size) || !upload_owed::may_wait_again(tex.owed, upload_torn::kFrames))
        return false;
    if (!memory_rewritten_since_capture(tex.gpu, tex.guest_size)) return false;
    st_torn_found.fetch_add(1, std::memory_order_relaxed), st_torn_found_frame.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lk(torn_mu);
    const bool leave = torn_on() && torn_recent.may(tex.id, frame_no);
    torn_recent.seen(tex.id, frame_no);
    if (leave) st_torn_later.fetch_add(1, std::memory_order_relaxed), st_torn_later_frame.fetch_add(1, std::memory_order_relaxed);
    return leave;
}
bool upload_again_later(const Texture& tex) {
    static const int64_t budget_ns = (int64_t)(getenv("RX_VK_UPLOAD_AGAIN_MS") ? atof(getenv("RX_VK_UPLOAD_AGAIN_MS")) : 16.0) * 1000000;
    if (!upload_defer_ok) return false;
    if (torn_later(tex)) return true;   // ((Triforce, wp18-flash) its storage being rewritten: its last bytes this frame)
    if (!budget_ns || !upload_owed::may_wait_again(tex.owed, upload_again_frames())) return false;
    if (frame_upload_ns.load(std::memory_order_relaxed) < budget_ns) return false;
    st_again_later.fetch_add(1, std::memory_order_relaxed), st_again_later_frame.fetch_add(1, std::memory_order_relaxed);
    return true;
}
int64_t texture_uploads_later() { return st_again_later.load(std::memory_order_relaxed); }
bool texture_frame_left_any() {   // (each counter is set back at the frame's end: texture_frame)
    return frame_owed.load(std::memory_order_relaxed) || st_again_later_frame.load(std::memory_order_relaxed) ||
           st_torn_later_frame.load(std::memory_order_relaxed) || st_deferred_frame.load(std::memory_order_relaxed);
}
// ---- (Triforce, 11-16) the re-uploads made before a parallel replay's segments (reupload_early.h) ----
namespace {
std::mutex reup_mu;   // (the notes come from the segments' threads too, under the recording lock; the early looks before them)
reupload_early::List reup_list;
int64_t st_reup_looks, st_reup_uploads, st_reup_bytes, st_reup_ns, st_reup_cbs;   // (the replay thread's)
/// RX_VK_REUPLOAD_EARLY=0: the textures the CPU keeps rewriting are uploaded again by the segments' looks alone, as before
bool reupload_early_on() {
    static const bool v = !getenv("RX_VK_REUPLOAD_EARLY") || atoi(getenv("RX_VK_REUPLOAD_EARLY")) != 0;
    return v;
}
size_t reup_size() {
    std::lock_guard lk(reup_mu);
    return reup_list.size();
}
void reupload_note(Texture& tex) {
    if (!reupload_early_on()) return;
    std::lock_guard lk(reup_mu);
    reup_list.note(&tex, tex.id, frame_no, gone_record.count());
}
/// (the textures to look at now, in their order: the hot ones the early look wants, reupload_early.h; those gone dropped)
std::vector<Texture*> reupload_early_pick() {
    std::vector<std::pair<reupload_early::Entry*, reupload_early::Seen>> pick;
    std::vector<std::pair<const void*, uint32_t>> gone;
    std::vector<Texture*> look;
    std::lock_guard lk(reup_mu);
    for (reupload_early::Entry* e : reup_list.hot(frame_no)) {
        // (its pointer is good while its number is not among the textures gone since it was last known to be there)
        if (gone_record.gone_since(e->id, e->since) || static_cast<Texture*>(e->tex)->id != e->id) {
            gone.push_back({e->tex, e->id});
            continue;
        }
        e->since = gone_record.count();
        const Texture& t = *static_cast<Texture*>(e->tex);
        reupload_early::Seen s;
        s.uploaded = t.uploaded, s.gpu_written = t.gpu_written, s.released = t.released, s.sparse = t.sparse;
        s.only_cleared = t.only_cleared, s.bytes = t.guest_size, s.last_frame = t.last_frame, s.owed_later = t.owed.later;
        if (reupload_early::wanted(s, frame_no)) pick.push_back({e, s});
    }
    reupload_early::order(pick);
    for (auto& [e, s] : pick) look.push_back(static_cast<Texture*>(e->tex));
    for (const auto& [p, id] : gone) reup_list.forget(p, id);
    return look;
}
}  // namespace
Texture* texture_touch(Texture* tex, bool for_sampling);
/// Before a parallel replay's segments, on the replay thread (vk_replay.cpp replay_handle, after the dry pass, into the uploads
/// of the recording before them): the textures the CPU keeps rewriting looked at once (texture_touch), so that their uploads
/// are not made by a segment under the recording lock (reupload_early.h)
void texture_reupload_early() {
    if (!reupload_early_on() || par_active.load(std::memory_order_relaxed)) return;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<Texture*> look = reupload_early_pick();   // (its lock let go: a look that uploads notes it, reupload_note)
    const int64_t up0 = st_uploads, bytes0 = frame_upload_bytes.load(std::memory_order_relaxed);
    for (Texture* t : look) {   // (no texture goes meanwhile: they go at a frame's end, on this thread)
        // (an early look is no draw's: the texture's age, which the eviction and the next pick go by, stays the draws'. Not its
        // last submission: an upload into its image is recorded in this one)
        const uint64_t last_frame = t->last_frame, let_go = t->let_go;
        const uint32_t back_gap = t->back_gap;   // ((Triforce) nor a take-back: keep taken, texture_touch)
        upload_defer_ok = true;   // (as a draw's look: the frame's budget may leave it for the next frame, upload_again_later)
        texture_touch(t, true);
        upload_defer_ok = false;
        t->last_frame = last_frame, t->let_go = let_go, t->back_gap = back_gap;
    }
    if (!look.empty()) {
        st_reup_cbs++, st_reup_looks += (int64_t)look.size(), st_reup_uploads += st_uploads - up0;
        st_reup_bytes += frame_upload_bytes.load(std::memory_order_relaxed) - bytes0;
        st_reup_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }
    static uint64_t told_at;
    if (st_reup_cbs && frame_no >= told_at + 600) {
        told_at = frame_no;
        logf("vk: textures the CPU keeps rewriting, looked at before the parallel replay's segments (since the last line): %lld "
             "looks in %lld command buffers, %lld uploads (%.1f MB) in %.2f ms, none of it under the recording lock; %zu "
             "textures listed (RX_VK_REUPLOAD_EARLY=0: the segments' looks alone)", (long long)st_reup_looks,
             (long long)st_reup_cbs, (long long)st_reup_uploads, st_reup_bytes / 1048576.0, st_reup_ns / 1e6, reup_size());
        st_reup_looks = st_reup_cbs = st_reup_uploads = st_reup_bytes = st_reup_ns = 0;
    }
}
Texture* texture_touch(Texture* tex, bool for_sampling) {
    tex->last_frame = frame_no;
    // ((Triforce) taken back after the game let go of it: the longest such gap, for texture_sweep's keep taken: tex_pressure.h)
    if (const uint64_t g = tex->let_go) tex->back_gap = tex_pressure::taken_back(tex->back_gap, frame_no - g);
    tex->let_go = 0;   // (in use: texture_sweep)
    tex->last_value = rec_value;
    if (!for_sampling) return tex;
    if (upload_defer(*tex)) return nullptr;
    // (Triforce, the parallel replay: from here to this look's end another thread's look without the lock does not take the
    // texture for up to date, touch_quick.h. Set before checked_epoch below, cleared after upload_value and upload_seg. The
    // fence: the fields below are plain, and a store after an atomic one may otherwise be made, or seen, before it)
    const struct Touching {
        Texture* t;
        explicit Touching(Texture* tex) : t(tex) { t->touching.store(1), std::atomic_thread_fence(std::memory_order_seq_cst); }
        ~Touching() { t->touching.store(0); }
    } touching(tex);
    // contents: from guest memory unless the GPU wrote the image (then it is the content, unless the texture was
    // finalized since and the CPU writes its memory again). What the CPU writes into it after the GPU's write is taken
    // all the same, those ranges alone, the rest staying the GPU's: Eden's texture cache takes a CPU write over the
    // GPU's contents, and MKW's font cache is cleared on the GPU with its glyphs written by the CPU as text appears
    // (the menus were missing every letter added later). Only whole pages of its storage: a write to another variable
    // sharing an edge page once re-uploaded garbage over a drawn LUT (the rainbow sky). Opt-in (RX_VK_CPU_OVER_GPU=1): transient render targets share memory the CPU writes for other things, and took that over their texels, and the menus' letters stayed missing with it.
    if (tex->gpu_written && !tex->released) {
        // (hookshot) and a texture the GPU only cleared: the game's font cache, whose letters the CPU writes as text appears. Its menu text
        // went missing at random (Hookshot issues 40 and 44), whenever the clear reached the image first
        static const bool take = getenv("RX_VK_CPU_OVER_GPU") != nullptr;
        if ((!take && !tex->only_cleared) || tex->checked_epoch == epoch) return tex;
        tex->checked_epoch = epoch;
        static thread_local Ranges cpu, inner;
        if (!texture_dirty(*tex, std::max(tex->upload_gen, tex->gpu_gen), cpu)) return tex;
        const uint64_t first = (0x1000 - (tex->gpu & 0xFFF)) & 0xFFF;
        const uint64_t last = ((tex->gpu + tex->guest_size) & ~0xFFFull) - tex->gpu;
        inner.clear();
        uint64_t bytes = 0;
        for (const auto& r : cpu) {
            const uint64_t a = std::max<uint64_t>(r.first, first), b = std::min<uint64_t>(r.second, last);
            if (a < b) inner.push_back({a, b}), bytes += b - a;
        }
        const WriteGen gen = memory_generation();
        if (!inner.empty() && upload(*tex, &inner)) {
            tex->upload_value = rec_value, tex->upload_seg = tl_seg;
            same_unchecked(SAME_CPU_OVER_GPU, up_route, &inner, tex->guest_size);   // ((Triforce) the line of those not checked)
            static int logged;
            if (logged++ < 20)
                logf("vk: texture %#llx %ux%u x%u NVN %#x: %llu KB the CPU wrote after the GPU taken", (unsigned long long)tex->gpu,
                     tex->width, tex->height, tex->layers, tex->nvn_format, (unsigned long long)(bytes >> 10));
        } else if (!inner.empty() && upload_starved) {
            // (Triforce) no staging memory for it: its generation stays, so that the next submission's look finds what the CPU
            // wrote again. Moved all the same, the letters written into a font cache stayed out of its image for good
            return tex;
        }
        tex->upload_gen = gen;
        return tex;
    }
    {   // (RX_VK_REUPLOAD_DIM=<w>x<h> (debugging): textures that size uploaded whole again every frame: is what a
        // sampled texture misses in guest memory, unseen by the write tracking, or not there at all?)
        static uint32_t rw = 0, rh = 0;
        static const bool parsed = [] {
            if (const char* e = getenv("RX_VK_REUPLOAD_DIM")) sscanf(e, "%ux%u", &rw, &rh);
            return true;
        }();
        (void)parsed;
        static thread_local uint64_t last;
        if (rw && tex->width == rw && tex->height == rh && tex->uploaded && tex->checked_epoch != epoch &&
            last != frame_no) {
            last = frame_no;
            tex->checked_epoch = epoch;
            upload(*tex, nullptr);
            return tex;
        }
    }
    if (tex->uploaded && tex->upload_value == rec_value && tex->upload_seg > tl_seg) {
        // (a parallel replay: uploaded in this submission by a segment after this one, which reads it first in stream
        // order: uploaded again, the same data, in this segment's uploads)
        // ((Triforce, 11-16) RX_VK_TWIN_SAME: only what this submission's uploads into it asked for (the whole storage when one was
        // whole or its image was made again), from a copy that keeps the image's hashes when it holds their bytes: upload_same.h,
        // "the parallel replay's twin". RX_VK_TWIN_SAME=0: the whole storage, the hashes forgotten, as before)
        static thread_local Ranges twin_part;   // (a copy: the upload notes into what twin_ranges points at)
        const Ranges* noted = twin_same_on() ? upload_same::twin_ranges(tex->same.seg, rec_value, same_image_id(*tex)) : nullptr;
        if (noted) twin_part = *noted;
        const Ranges* part = noted ? &twin_part : nullptr;
        const bool looked = part && same_twin_look(*tex, *part);
        const bool up_ok = upload(*tex, part);
        same_twin_done(*tex, part, looked, up_ok);
        if (up_ok) same_unchecked(SAME_TWIN, up_route, part, tex->guest_size);   // ((Triforce) not checked)
        tex->upload_seg = tl_seg;
        return tex;
    }
    if (tex->uploaded && tex->checked_epoch == epoch) return tex;
    tex->checked_epoch = epoch;
    // (Triforce) An upload that found no memory is owed (upload_owed.h; the failed upload's branches at this function's end):
    // not tried again before its wait is over, and nothing is asked before that either, since the questions below note what
    // they saw. (And one whose first upload the budget left for the next frame, met again in this one: the dummy again for a
    // draw while the hold may leave it, new_texture_waits; else, for a dispatch or at the hold's bound, it is uploaded now)
    if (upload_owed::waits(tex->owed, frame_no)) {
        if (!upload_owed::first_later(tex->owed, tex->uploaded)) return tex;
        if (new_texture_waits()) return nullptr;
    }
    const bool owed = tex->owed.any();
    static thread_local Ranges dirty;
    static const bool no_partial = getenv("RX_VK_NO_PARTIAL") != nullptr;   // (A/B: whole re-uploads)
    // (a buffer the GPU wrote over its storage, done since: all of it again; the tiles of MKW's terrain are filled so
    // and stayed the flat placeholder they were made with)
    const uint64_t wrote_seen = tex->gpu_wrote_seen;   // (gpu_wrote_into notes the write as seen: put back when the upload fails)
    const bool gpu_dirty = tex->uploaded && gpu_wrote_into(*tex);
    const bool partial = !no_partial && !gpu_dirty && tex->uploaded && !tex->gpu_written && !tex->released && !tex->owed.whole;
    // (a texture the GPU drew and the game finalized: only what the CPU wrote since then. Its memory never got the
    // GPU's texels, and a write before (another variable in the same page) re-uploaded garbage over a drawn LUT:
    // the rainbow sky)
    const WriteGen since = tex->gpu_written && tex->released ? std::max(tex->upload_gen, tex->release_gen) : tex->upload_gen;
    if (tex->gpu_written && tex->released && frame_no > tex->last_written + 60) {
        // (hookshot: one the game finalized, sampled again well after it was drawn: what the eviction keeps them for)
        static std::unordered_set<uint32_t> told;
        if (told.size() < 200 && told.insert(tex->id).second)
            logf("vk: texture #%u %#llx %ux%u x%u NVN %#x the GPU drew, finalized by the game, sampled again at frame %llu (drawn at "
                 "%llu)", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, tex->nvn_format,
                 (unsigned long long)frame_no, (unsigned long long)tex->last_written);
    }
    if (tex->uploaded && !gpu_dirty && !texture_dirty(*tex, since, dirty, !tex->gpu_written) && !owed) return tex;
    if (owed && partial) upload_owed::add(dirty, tex->owed.ranges);   // (Triforce: with what the failed upload was to bring)
    if (tex->uploaded && upload_again_later(*tex)) {
        // (Triforce) left for the next frame: its image keeps its last upload's bytes until then (upload_owed::later), what this
        // upload was to bring owed with what was owed before, the GPU's write into its storage noted as not seen yet
        upload_owed::later(tex->owed, partial ? &dirty : nullptr, frame_no);
        tex->gpu_wrote_seen = wrote_seen;
        return tex;
    }
    const bool split_going = split_pending(*tex);   // (Triforce, layer_split.h: its parts go on below, budget or not)
    if (!split_going && upload_owed::may_leave_first(tex->owed, tex->uploaded) && new_texture_later(*tex)) {
        // (Triforce, the hold of the last picture: present_hold.h) a new texture after the frame's uploads took their budget:
        // its whole first upload owed to the next frame, the draw given the dummy (its image has no bytes yet), the frame held.
        // Asked before anything below notes what it saw (the eviction's note, a virtual texture's first mapping)
        upload_owed::later(tex->owed, nullptr, frame_no);
        return nullptr;
    }
    const WriteGen gen = memory_generation();
    // (storage render targets took last, which the CPU has not written since: nothing to upload, the memory holds
    // nothing the GPU's images did not replace. The game's transient heaps move with the dynamic resolution, and a
    // render target sampled before it is rendered at its new address (the culling reads last frame's shadow map)
    // was unswizzled from that memory: 55 MB for the 3072 x 3072 x 3 shadow map, frame after frame. Not "storage
    // the write watch never saw written": the game fills memory before it makes a pool of it.)
    static const bool upload_all = getenv("RX_VK_UPLOAD_ALL") != nullptr;   // (A/B: such storage uploaded too)
    // (from Shakoquo/Mako ff479e988d) never for a block-compressed texture (ASTC, BC, ETC): the GPU cannot render into one, so
    // its texels come from memory whatever render targets used that storage before. Skipped, a boss or a new enemy met for
    // the first time (its textures loaded into the transient heaps' memory) was drawn with an empty texture, all blue, for
    // the whole cutscene. RX_VK_SKIP_COMPRESSED_OVER_RT=1: as before
    // (Triforce: this tree has no env_on yet; the switch is on when set to anything but 0)
    static const bool skip_compressed = getenv("RX_VK_SKIP_COMPRESSED_OVER_RT") && atoi(getenv("RX_VK_SKIP_COMPRESSED_OVER_RT")) != 0;
    const bool compressed = tex->astc || tex->bc ||
                            (tex->format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && tex->format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
    WriteGen owned = !tex->uploaded && !upload_all && (skip_compressed || !compressed) ? gpu_owned_since(tex->gpu, tex->guest_size) : WriteGen{};
    if (!tex->uploaded && !upload_all && !skip_compressed && compressed) {
        static std::atomic<int> told{0};
        if (told.load(std::memory_order_relaxed) < 30 && gpu_owned_since(tex->gpu, tex->guest_size) && told++ < 30)
            logf("vk: texture #%u %#llx %ux%u x%u NVN %#x (compressed) over storage render targets took: uploaded from the game's "
                 "memory all the same", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers,
                 tex->nvn_format);
    }
    bool from_memory = false;   // (Triforce: its note below was taken: put back when the upload fails, for the next try)
    if (owned && !remade_skip_old()) {   // (one evicted that came from memory: uploaded from it again. Above)
        std::lock_guard lk(from_memory_mu);
        auto e = evicted_from_memory.find(evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format));
        if (e != evicted_from_memory.end() && e->second == tex->guest_size) {
            evicted_from_memory.erase(e);
            owned = {};
            from_memory = true;
            const uint64_t n = ++st_remade_uploaded;
            if (n <= 20 || n % 100 == 0)
                logf("vk: texture #%u %#llx %ux%u NVN %#x evicted, made again over storage render targets took: uploaded from "
                     "the game's memory (%llu so far)", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height,
                     tex->nvn_format, (unsigned long long)n);
        }
    }
    watch_tex(tex, owned ? "sampled, over rendered storage" : "sampled, uploaded");
    audit_scan(*tex);   // (RX_VK_AUDIT_GPU_ONLY)
    if (owned && !texture_dirty(*tex, owned, dirty)) {
        tex->uploaded = true;
        tex->upload_gen = gen;
        // (Triforce: and an upload still owed from before the render targets took its storage is owed no more: kept, the next
        // look would bring that memory in after all, the upload this branch is there to leave out)
        if (tex->owed.from) upload_owed::paid(tex->owed);
        watch_tex(tex, "NOT uploaded (over storage the GPU rendered)");
        if (audit_on()) {   // (the same-storage case: made over what the GPU drew, sampled with nothing in its image)
            static int told;
            if (told++ < 400)
                logf("vk: audit: texture #%u %#llx %ux%ux%u x%u levels %u NVN %#x, sampled by %016llx at frame %llu, is made over "
                     "storage the GPU drew and not uploaded: its image holds nothing", tex->id, (unsigned long long)tex->gpu,
                     tex->width, tex->height, tex->depth, tex->layers, tex->levels, tex->nvn_format,
                     (unsigned long long)tl_watch_prog, (unsigned long long)frame_no);
        }
        if (remade_log_on()) {   // (made again after its eviction, and sampled empty)
            std::lock_guard lk(evicted_mu);
            auto r = remade.find(tex);
            if (r != remade.end()) {
                static int told;
                if (told++ < 300)
                    logf("vk: texture #%u %#llx %ux%u NVN %#x, made again %llu frames after its eviction, sampled EMPTY "
                         "(storage the GPU rendered, not uploaded) at frame %llu", tex->id, (unsigned long long)tex->gpu,
                         tex->width, tex->height, tex->nvn_format, (unsigned long long)r->second,
                         (unsigned long long)frame_no);
                remade.erase(r);
            }
        }
        return tex;
    }
    if (partial && getenv("RX_VK_LOG_PARTIAL")) {
        static int logged;
        if (logged++ < 200) {
            char line[512];
            int n = snprintf(line, sizeof line, "vk: partial upload %#llx %ux%ux%u x%u levels %u NVN %#x size %llu flags %#x:",
                             (unsigned long long)tex->gpu, tex->width, tex->height, tex->depth, tex->layers, tex->levels,
                             tex->nvn_format, (unsigned long long)tex->guest_size, tex->d.flags);
            for (size_t i = 0; i < dirty.size() && i < 8 && n < 480; i++)
                n += snprintf(line + n, sizeof line - n, " [%#llx,%#llx)", (unsigned long long)dirty[i].first,
                              (unsigned long long)dirty[i].second);
            logf("%s", line);
        }
    }
    if (!tex->uploaded && tex->gpu >= (1ull << 40) && tex->page_map.empty()) {
        // (a sparse texture's mapping as it is uploaded first: its first check after does not take every page for
        // remapped, which uploaded the course maps twice)
        static thread_local Ranges first;
        texture_dirty(*tex, WriteGen{}, first);
    }
    // (a sparse image, first or whole: only the storage ever written, not all of its gigabytes)
    const bool sparse_written = tex->sparse && !partial;
    if (sparse_written) texture_dirty(*tex, WriteGen{}, dirty);
    if (tex->gpu_written) {   // (an image the GPU drew, finalized and sampled again: from guest memory, 400 lines)
        static int logged;
        uint64_t kb = 0;
        for (const auto& r : dirty) kb += (r.second - r.first) >> 10;
        if (logged++ < 400)
            logf("vk: upload over a drawn image: #%u %#llx %ux%ux%u x%u NVN %#x at present %llu (texture frame %llu), %s, "
                 "%llu KB dirty in %zu ranges%s", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->depth,
                 tex->layers, tex->nvn_format, (unsigned long long)(frame_no - 1), (unsigned long long)frame_no,
                 tex->released ? "finalized" : "live", (unsigned long long)kb, dirty.size(), gpu_dirty ? ", a GPU buffer wrote it" : "");
    }
    // (hookshot) a BC3 image whose last upload failed waits before the next try: its upload stages the whole ASTC source (some
    // 200 MB for a terrain array), not to be repeated every submission while the memory it needs is not there
    if (tex->bc && !tex->uploaded && tex->bc_retry_frame > frame_no) return tex;
    // (Triforce, 11-16, upload_same.h) A re-upload from the CPU's writes, checked against the bytes its image holds: the units the
    // writes reached whose bytes are the image's already are left out, and when that is all of them nothing is uploaded (the lab's
    // S2 Market after some tours: the game rewrote a few big BC textures with the bytes they had, 40 to 60 MB every 4 to 5 frames).
    // The upload of the rest reads the copy the check hashed (same_done keeps its hashes). RX_VK_UPLOAD_SAME=0: as before
    const int same = partial ? same_look(*tex, dirty) : 0;
    if (same == 1) {   // (nothing to bring: as an upload that brought these bytes, its image left as it is)
        tex->upload_gen = gen;
        upload_owed::paid(tex->owed);
        if (tex->gpu < (1ull << 40) && tex->guest_size < tex_capture_max()) memory_note_texture_upload(tex->gpu, tex->guest_size);
        if (tex->guest_size >= reupload_early::kMinBytes) reupload_note(*tex);   // (one the CPU keeps rewriting all the same)
        return tex;
    }
    // (Triforce, RX_VK_STAMP_SRC=1, a diagnostic: a texture of 1 MB or more uploaded again: who wrote its first dirty page (the
    // write watch 1, a capture 2, else a caller of memory_stamp, placed by memory_stamp's own address), and whether its memory
    // holds other bytes than at its last upload at all. 400 lines)
    static const bool stamp_src = getenv("RX_VK_STAMP_SRC") != nullptr;
    if (stamp_src && tex->uploaded && tex->guest_size >= (1u << 20)) {
        static int told;
        static std::unordered_map<uint32_t, int> times;   // (RX_VK_STAMP_SRC=<n>: only a texture's n-th such upload and later)
        static const int from_nth = std::max(1, atoi(getenv("RX_VK_STAMP_SRC")));
        const uint64_t h = guest_hash(*tex);
        if (++times[tex->id] >= from_nth && told++ < 400) {
            uint64_t part = 0;
            for (const auto& r : dirty) part += r.second - r.first;
            const uint64_t off = dirty.empty() ? 0 : dirty[0].first;
            const uint64_t guest = tex->gpu < (1ull << 40) ? tex->guest + off : host->gpu_guest(tex->gpu + off);
            WriteGen pg;
            uintptr_t anchor = 0;
            const uintptr_t who = guest ? memory_stamp_who(guest, &pg, &anchor) : 0;
            logf("vk: stamp src: #%u %#llx %ux%u x%u NVN %#x uploaded again (%d) at frame %llu: %llu of %llu KB dirty in %zu ranges%s; bytes %s; "
                 "page %#llx gen %llu (uploaded at %llu) by %s%+lld", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height,
                 tex->layers, tex->nvn_format, times[tex->id], (unsigned long long)frame_no, (unsigned long long)(part >> 10),
                 (unsigned long long)(tex->guest_size >> 10), dirty.size(), gpu_dirty ? " (a GPU buffer wrote it)" : "",
                 !tex->verify_hash ? "not hashed before" : h == tex->verify_hash ? "THE SAME as at its last upload" : "changed",
                 (unsigned long long)guest, (unsigned long long)pg.v, (unsigned long long)since.v, who == 1 ? "the write watch" : who == 2 ? "a capture" : who ? "memory_stamp" : "unknown",
                 who > 2 ? (long long)(who - anchor) : 0ll);
        }
        tex->verify_hash = h;
    } else if (stamp_src && tex->guest_size >= (1u << 20)) {
        tex->verify_hash = guest_hash(*tex);
    }
    // (Triforce, layer_split.h) a new layered ASTC texture's first upload, a part of its layers this frame; what the CPU writes into
    // its storage after the first part is taken by the upload that follows the last (its generation is the first part's).
    // RX_VK_UPLOAD_SPLIT_MB=0: whole, as before
    static thread_local Ranges split_part, split_rest;
    const bool split = !partial && !sparse_written && split_take(*tex, split_part, split_rest);
    const WriteGen split_from = split_going ? tex->upload_gen : gen;
    const auto up0 = std::chrono::steady_clock::now();
    // ((Triforce) a re-upload made without the check, and why, for the line of those: same_unchecked)
    const int unchecked = !tex->uploaded || split || same ? SAME_NONE : partial ? tl_same_why : no_partial ? SAME_NO_PARTIAL
                          : gpu_dirty ? SAME_GPU_DIRTY : tex->gpu_written || tex->released ? SAME_GPU_WRITTEN : SAME_OWED_WHOLE;
    // ((Triforce, 11-17) a parallel replay's worker: into the init command buffer of the earliest segment still recorded, before
    // this one, so that no segment before it uploads it again (the twin above); upload_earliest.h. Not a texture the GPU drew or
    // a sparse one, nor one upload_inner reads on the GPU or from live memory (upload_reads_live): only bytes staged as recorded.
    // RX_VK_UPLOAD_EARLIEST=0: in this segment's own uploads, as before)
    UploadRedirect early(upload_earliest::wanted(upload_earliest_on(), tex->gpu_written, tex->sparse) && tl_seg_index >= 0 &&
                         !upload_reads_live(*tex, split ? &split_part : partial || sparse_written ? &dirty : nullptr));
    const bool up_ok = upload(*tex, split ? &split_part : partial || sparse_written ? &dirty : nullptr);
    const uint64_t up_seg = early.order;   // (the segment it is in: its own, or the earlier one's)
    const bool moved = early.to != nullptr;
    early.done();
    if (moved && up_ok) upload_earliest_note(*tex, split ? &split_part : partial || sparse_written ? &dirty : nullptr);
    if (same == 2) same_done(*tex, up_ok);   // ((Triforce) the checked upload's hashes kept or forgotten: upload_same.h)
    else if (up_ok && unchecked) same_unchecked(unchecked, up_route, partial || sparse_written ? &dirty : nullptr, tex->guest_size);
    if (split && up_ok && !split_rest.empty()) {
        // (not the last part: owed the rest for the next frame, the draw given the dummy, the frame held as for a texture left
        // whole, present_hold.h)
        tex->owed.whole = false, tex->owed.ranges.clear();
        upload_owed::later(tex->owed, &split_rest, frame_no);
        tex->upload_gen = split_from;
        tex->upload_value = rec_value, tex->upload_seg = up_seg;
        if (!split_going) st_split_textures.fetch_add(1, std::memory_order_relaxed);
        st_split_parts.fetch_add(1, std::memory_order_relaxed);
        frame_owed.fetch_add(1, std::memory_order_relaxed);
        static int told;
        if (told++ < 40)
            logf("vk: texture #%u %#llx %ux%u x%u levels %u (ASTC%s): its first upload in parts, %zu ranges of layers left for the "
                 "next frames (RX_VK_UPLOAD_SPLIT_MB=0: whole)", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height,
                 tex->layers, tex->levels, tex->bc ? ", kept as BC3" : "", split_rest.size());
        return nullptr;
    }
    {   // (an upload over 15 ms: what it was, 60 of them)
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - up0).count();
        static int slow_logged;
        if (ms > 15.0 && slow_logged++ < 60) {
            uint64_t part = 0;
            if (partial) for (const auto& r : dirty) part += r.second - r.first;
            logf("vk: slow upload %.1f ms: #%u %#llx %ux%ux%u x%u levels %u NVN %#x%s%s, %llu of %llu KB%s", ms, tex->id,
                 (unsigned long long)tex->gpu, tex->width, tex->height, tex->depth, tex->layers, tex->levels,
                 tex->nvn_format, tex->astc ? " ASTC" : "", tex->bc ? " (BC3)" : "",
                 (unsigned long long)((partial ? part : tex->guest_size) >> 10), (unsigned long long)(tex->guest_size >> 10),
                 tex->uploaded ? " again" : "");
        }
    }
    if (up_ok) {
        tex->upload_value = rec_value, tex->upload_seg = up_seg;
        // (its storage joins the submit-time capture: what the game writes into it after a submit is not taken for it)
        // (any size: the capture copies only the pages the CPU wrote, and the big CPU-streamed textures were read as the
        // game had rewritten them a frame or two later. RX_VK_TEX_CAPTURE_MAX_MB=<n>: only up to that, 8 before)
        if (tex->gpu < (1ull << 40) && tex->guest_size < tex_capture_max()) memory_note_texture_upload(tex->gpu, tex->guest_size);
        if (verify_dim().first && tex->width == verify_dim().first && tex->height == verify_dim().second)
            tex->verify_hash = guest_hash(*tex, true);
        if (tex->uploaded && !gpu_dirty && !tex->gpu_written && !tex->sparse && tex->guest_size >= reupload_early::kMinBytes)
            reupload_note(*tex);   // ((Triforce) the CPU rewrote it: the early looks of the textures it keeps rewriting)
        if (tex->uploaded) {
            static int logged;
            static std::unordered_map<uint64_t, int> big;
            if (logged++ < 20 || (tex->guest_size > (4u << 20) && big[tex->gpu]++ < 3)) {
                uint64_t part = 0, nz = 0, readable = 0;
                for (const auto& r : dirty) part += r.second - r.first;
                if (partial) {   // (what the CPU wrote: how much of it is not zero, and in which levels)
                    std::vector<uint8_t> b;
                    char lv[96] = "";
                    int lvn = 0;
                    for (const auto& r : dirty) {
                        b.resize((size_t)(r.second - r.first));
                        if (!host->gpu_read(tex->gpu + r.first, b.data(), b.size())) continue;
                        readable += b.size();
                        for (uint8_t c : b) nz += c != 0;
                        for (uint32_t l = 0; l < tex->levels && lvn < 90; l++) {
                            const uint64_t lo = host->tex_level_offset(&tex->d, l);
                            const uint64_t hi = l + 1 < tex->levels ? host->tex_level_offset(&tex->d, l + 1) : tex->guest_size;
                            if (r.first < hi && r.second > lo) lvn += snprintf(lv + lvn, sizeof lv - lvn, " L%u", l);
                        }
                    }
                    logf("vk:   (its written part: %llu of %llu KB readable, %llu bytes not zero; levels%s)",
                         (unsigned long long)(readable >> 10), (unsigned long long)(part >> 10), (unsigned long long)nz, lv);
                }
                logf("vk: texture %#llx %ux%ux%u x%u levels %u NVN %#x (%llu KB of storage) re-uploaded (its memory was "
                     "written; flags %#x): %s, %llu KB of it%s%s", (unsigned long long)tex->gpu, tex->width, tex->height,
                     tex->depth, tex->layers, tex->levels, tex->nvn_format, (unsigned long long)(tex->guest_size >> 10),
                     tex->d.flags, gpu_dirty ? "a GPU write into its storage" : partial ? "what the CPU wrote" : "whole",
                     (unsigned long long)((partial || sparse_written ? part : tex->guest_size) >> 10),
                     tex->gpu_written ? ", over what the GPU wrote" : "", tex->released ? " (finalized since)" : "");
            }
        }
        tex->uploaded = true;
        tex->upload_gen = gen;
        tex->gpu_written = false;
        tex->released = false;
        upload_owed::paid(tex->owed);
        if (split && split_going) tex->upload_gen = split_from;   // (Triforce, layer_split.h: the last part; as of the first)
    } else if (tex->bc) {
        // (hookshot) a BC3 image holds nothing until its encode has run. Marked uploaded as the others are, its blocks as the
        // memory had them were drawn as coloured noise, and never made again unless the game wrote its memory: the path, the
        // trees and Saria after an alt-tab on a 4 GB RX 5500M, its VRAM full and the textures evicted and made again. Not
        // uploaded, it is tried again 30 frames on, until the staging and the scratch it needs can be had
        tex->bc_retry_frame = frame_no + 30;
        static std::atomic<int> told{0};
        if (told++ < 40)
            logf("vk: BC3 texture #%u %#llx %ux%u x%u: its upload failed (%s): tried again in 30 frames", tex->id,
                 (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, astc_fail ? astc_fail : "unknown");
    } else if (upload_starved) {
        // (Triforce) and any other texture whose upload found no staging memory (staging: no arena could be made, a card or a PC
        // short of memory). Marked uploaded at this generation, as the branch below does, its image held nothing or what it
        // held before until the game wrote other bytes into its memory, and a page written again with the bytes it had is no
        // write to a texture (RX_VK_TEX_SAME): ground and walls wrong or missing until the area was left or the game started
        // again. Not uploaded and its generation kept, it is tried again once its wait is over (upload_owed.h)
        static std::atomic<int64_t> told{0};
        const int64_t n = ++told;
        if (n <= 40 || n % 1000 == 0)
            logf("vk: texture #%u %#llx %ux%u x%u NVN %#x: no staging memory for its upload: tried again in %llu frames (%lld such "
                 "uploads so far)", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, tex->nvn_format,
                 (unsigned long long)upload_owed::wait(tex->owed.failed + 1), (long long)n);
    } else {
        tex->uploaded = true;   // (not retried every draw; a write to its memory tries again)
        tex->upload_gen = gen;
    }
    if (!up_ok && (tex->bc || upload_starved)) {
        // (Triforce) What that upload was to bring stays owed, for the next try to add to what is written by then: the ranges
        // asked for (a virtual texture's pages mapped anew are told once, texture_dirty, and a BC3 image's failed upload of a
        // part lost them), the GPU's write into its storage, noted as seen above, and the note that it came from memory before
        // its eviction, taken above (without it the next try finds its storage the render targets' and uploads nothing)
        upload_owed::failed(tex->owed, partial || sparse_written ? &dirty : nullptr, frame_no, tex->bc ? 30 : 0);
        tex->gpu_wrote_seen = wrote_seen;
        if (from_memory) {
            std::lock_guard lk(from_memory_mu);
            evicted_from_memory[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = tex->guest_size;
        }
    }
    return tex;
}

/// (Triforce, 2026-11-11) A flat texture sampled by a program whose instruction is a 3D sample: a one-slice 3D image with its
/// texels, and a 3D view of that. Vulkan has no 3D view of a 2D image, so such a binding had no view and read zeros (a null
/// descriptor). The console's GPU samples by the texture's own header whatever the instruction says: a flat texture's texel at
/// (u, v), the third coordinate unused, which a one-slice 3D image gives for any w. OoT's water in Zora's River binds one of
/// the game's 4 x 4 textures so (slot 0x102, 44 lookups in 10 s; its ripples ran in slow motion). Small plain textures only:
/// one layer copied, up to 1024 x 1024, not depth, not kept as BC3. Copied again at a lookup after the texture's contents
/// changed (an upload, or the GPU writing it): the lookups are cached by slot, so one the GPU draws every frame is a frame or
/// more behind here. RX_VK_NO_TWIN3D=1: no such view, as before
VkImageView twin3d_view(Texture* tex, VkFormat f, uint32_t level0, uint32_t nlevels, uint32_t layer, const VkComponentMapping& cm) {
    static const bool off = getenv("RX_VK_NO_TWIN3D") != nullptr;
    if (off || tex->type != VK_IMAGE_TYPE_2D || tex->sparse || tex->bc || tex->samples != VK_SAMPLE_COUNT_1_BIT ||
        tex->aspect != VK_IMAGE_ASPECT_COLOR_BIT || is_bc(tex->format) || is_astc(tex->format) ||
        (uint64_t)tex->width * tex->height > 1024ull * 1024 || !tex->image)
        return VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!tex->twin3d) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_3D;
        ci.format = tex->format;
        ci.extent = {tex->width, tex->height, 1};
        ci.mipLevels = tex->levels;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   // (the view's format may be another of its class: sRGB or not)
        if (!create_image(ci, tex->twin3d, tex->twin3d_alloc, false, false, false)) {
            tex->twin3d = VK_NULL_HANDLE, tex->twin3d_alloc = nullptr;
            return VK_NULL_HANDLE;
        }
        {   // (Triforce: its first layout change in queue order for a parallel replay's worker, as a new image's in create():
            // into the earliest segment's init still being recorded. In the maker's own uploads, an earlier segment that
            // asked for the copy after it was made sampled it before the queue had reached that change)
            InitRedirect redirect;
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = tex->twin3d;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(upload_cmd(), &di);
        }
        tex->twin3d_value = ~0ull, tex->twin3d_written = ~0ull;
        static int told;
        if (told++ < 12)
            logf("vk: texture #%u %#llx %ux%u levels %u NVN %#x is sampled as a 3D texture: a one-slice 3D copy of it made", tex->id,
                 (unsigned long long)tex->gpu, tex->width, tex->height, tex->levels, tex->nvn_format);
    }
    // (Triforce: and copied again by a segment before the one that copied it last in this submission, as an upload is made
    // again, texture_touch: the later segment's copy comes after this one's draws in the queue)
    if (tex->twin3d_value != tex->upload_value || tex->twin3d_written != tex->last_written || tex->twin3d_layer != layer ||
        (tex->twin3d_rec == rec_value && tex->twin3d_seg > tl_seg)) {
        if (!cmd) cmd = upload_cmd();
        full_barrier(cmd, "barrier (a flat texture's 3D copy: before)");
        std::vector<VkImageCopy> rs;
        for (uint32_t l = 0; l < tex->levels; l++) {
            VkImageCopy r{};
            r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1};
            r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
            r.extent = {std::max(1u, tex->width >> l), std::max(1u, tex->height >> l), 1};
            rs.push_back(r);
        }
        vkCmdCopyImage(cmd, tex->image, VK_IMAGE_LAYOUT_GENERAL, tex->twin3d, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)rs.size(), rs.data());
        full_barrier(cmd, "barrier (a flat texture's 3D copy: after)");
        tex->twin3d_value = tex->upload_value, tex->twin3d_written = tex->last_written, tex->twin3d_layer = layer;
        tex->twin3d_rec = rec_value, tex->twin3d_seg = tl_seg;
    }
    uint64_t key = mix((uint64_t)f, (uint64_t)level0 << 32 | nlevels);
    key = mix(key, (uint64_t)cm.r << 24 | (uint64_t)cm.g << 16 | (uint64_t)cm.b << 8 | cm.a);
    const auto it = tex->twin3d_views.find(key);
    if (it != tex->twin3d_views.end()) return it->second;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = tex->twin3d;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = f;
    vi.components = cm;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level0, nlevels, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &vi, nullptr, &view) != VK_SUCCESS) view = VK_NULL_HANDLE;
    tex->twin3d_views[key] = view;
    return view;
}

VkImageView texture_view(Texture* tex, const nvn_tex* t, const nvn_view* v, VkImageViewType type, ViewUse use,
                         VkFormat* format_out, VkFormat force_format) {
    if (!tex) return VK_NULL_HANDLE;
    const int nf = view_nvn_format(t, v);
    VkFormat f = vk_format(nf).format;
    int from = 0;
    f = widened(f, &from);
    const bool depth = is_depth_format(tex->format);
    if (depth || f == VK_FORMAT_UNDEFINED) f = tex->format;
    if (tex->astc) f = (nf >= 0x87 && nf <= 0x94) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (tex->bc) {
        if (use != ViewUse::Sample) return VK_NULL_HANDLE;
        // (Triforce: an image kept as BC1 by its own pair of formats, tex_bc1.h)
        f = (VkFormat)tex_bc1::view_format(tex->bc1, f == VK_FORMAT_R8G8B8A8_SRGB);
    }
    if (use == ViewUse::Storage) f = force_format != VK_FORMAT_UNDEFINED ? force_format : storage_view_format(f);
    if (use == ViewUse::Attachment && (is_astc(f) || is_bc(f))) return VK_NULL_HANDLE;
    const uint32_t vl = view_level(v);
    uint32_t level0 = image_level(tex, vl);
    uint32_t nlevels = v && v->set_levels && v->nlevels ? (vl + v->nlevels > tex->skip ? vl + v->nlevels - tex->skip - level0 : 1)
                                                       : tex->levels - level0;
    nlevels = std::clamp<uint32_t>(nlevels, 1, tex->levels - level0);
    const uint32_t total_layers = tex->type == VK_IMAGE_TYPE_3D ? 1 : tex->layers;
    uint32_t layer0 = std::min(view_layer(v), total_layers - 1);
    uint32_t nlayers = v && v->set_layers && v->nlayers ? v->nlayers : total_layers - layer0;
    nlayers = std::clamp<uint32_t>(nlayers, 1, total_layers - layer0);
    {   // (Evil Hookshot, diagnosis) RX_VK_LOG_VIEW_LAYERS=<n>: the views of textures with at least n layers, 40 lines: the
        // texture's target, the game's view (target, layers), the shader's type (11-08: OoT's 1365 x 1365 ground material arrays
        // are 2D arrays of 90 in its own views, as Hookshot makes them)
        static const uint32_t log_n = getenv("RX_VK_LOG_VIEW_LAYERS") ? (uint32_t)atoi(getenv("RX_VK_LOG_VIEW_LAYERS")) : 0;
        static std::atomic<int> logged{0};
        if (log_n && total_layers >= log_n && logged.fetch_add(1) < 40)
            logf("vk: view of #%u %ux%u x%u layers: texture target %d, view %s target %d layers %u+%u, shader type %d, use %d -> "
                 "layers %u+%u", tex->id, tex->width, tex->height, total_layers, (int)t->d.target, v ? "yes" : "no",
                 v && v->set_target ? (int)v->target : -1, v && v->set_layers ? v->min_layer : 0u,
                 v && v->set_layers ? v->nlayers : 0u, (int)type, (int)use, layer0, nlayers);
    }
    VkImageAspectFlags aspect = tex->aspect;
    if (use != ViewUse::Attachment && aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (use == ViewUse::Attachment) {
        nlevels = 1, nlayers = 1;
        type = tex->type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
        // (a slice of a 3D target: a 2D view of it, 2D_ARRAY_COMPATIBLE, the slice as its layer)
        if (tex->type == VK_IMAGE_TYPE_3D) layer0 = std::min(view_layer(v), std::max(1u, tex->depth >> level0) - 1);
    } else if (use == ViewUse::Storage) {
        nlevels = 1;
        if (tex->type == VK_IMAGE_TYPE_3D) type = VK_IMAGE_VIEW_TYPE_3D, layer0 = 0, nlayers = 1;
        else if (type == VK_IMAGE_VIEW_TYPE_2D && nlayers > 1) nlayers = 1;
        else if (type == VK_IMAGE_VIEW_TYPE_1D && nlayers > 1) nlayers = 1;
    }
    int twin_layer = -1;   // (Triforce: a flat texture wanted as 3D: the layer its 3D copy is of)
    // the shader's type over the image's: what Vulkan allows of them
    switch (type) {
    case VK_IMAGE_VIEW_TYPE_1D: case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
        if (tex->type != VK_IMAGE_TYPE_1D) return VK_NULL_HANDLE;
        if (type == VK_IMAGE_VIEW_TYPE_1D) nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_2D: case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        if (tex->type == VK_IMAGE_TYPE_1D) return VK_NULL_HANDLE;
        if (tex->type == VK_IMAGE_TYPE_3D) {
            // (a 2D sample instruction over a 3D texture, as Eden's recompiler keeps the instruction's type: its first
            // slice, one level (VK_EXT_image_2d_view_of_3d). MKW reads a 64 x 64 x 64 texture so, which was sampled
            // as the dummy)
            if (use == ViewUse::Sample && dev.view_2d_of_3d) {
                type = VK_IMAGE_VIEW_TYPE_2D, nlevels = 1, layer0 = 0, nlayers = 1;
                break;
            }
            if (use != ViewUse::Attachment) return VK_NULL_HANDLE;
        }
        if (type == VK_IMAGE_VIEW_TYPE_2D) nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_3D:
        if (tex->type != VK_IMAGE_TYPE_3D) {
            // (Triforce: a flat texture sampled as a 3D one: its one-slice 3D copy, twin3d_view above)
            if (use != ViewUse::Sample) return VK_NULL_HANDLE;
            twin_layer = (int)layer0;
        }
        layer0 = 0, nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE: case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        if (!tex->cube) return VK_NULL_HANDLE;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE) nlayers = 6;
        else nlayers = std::max(6u, nlayers / 6 * 6);
        if (layer0 + nlayers > tex->layers) return VK_NULL_HANDLE;
        break;
    default: break;
    }
    if (tex->type == VK_IMAGE_TYPE_3D && use == ViewUse::Attachment) type = VK_IMAGE_VIEW_TYPE_2D;
    VkComponentMapping cm{};
    if (use == ViewUse::Sample) {
        const nvn_texdesc& d = t->d;
        unsigned sw[4] = {d.swizzle_r, d.swizzle_g, d.swizzle_b, d.swizzle_a};
        if (v && v->set_swizzle)
            for (int i = 0; i < 4; i++) sw[i] = v->swz[i];
        // (X formats read alpha as one)
        switch (nf) {
        case NVN_FORMAT_RGBX8: case NVN_FORMAT_RGBX8SN: case NVN_FORMAT_RGBX8UI: case NVN_FORMAT_RGBX8I:
        case NVN_FORMAT_RGBX16F: case NVN_FORMAT_RGBX16: case NVN_FORMAT_RGBX16SN: case NVN_FORMAT_RGBX16UI:
        case NVN_FORMAT_RGBX16I: case NVN_FORMAT_RGBX32F: case NVN_FORMAT_RGBX32UI: case NVN_FORMAT_RGBX32I:
        case NVN_FORMAT_RGBX8_SRGB: case NVN_FORMAT_BGRX8: case NVN_FORMAT_BGRX8_SRGB:
            for (unsigned& s : sw)
                if (s == NVN_TEXTURE_SWIZZLE_A) s = NVN_TEXTURE_SWIZZLE_ONE;
            break;
        default: break;
        }
        static const bool swap_9e5 = getenv("RX_VK_9E5_SWAP") != nullptr;   // (hookshot, diagnosis: R and B swapped)
        if (swap_9e5 && nf == NVN_FORMAT_RGB9E5F) std::swap(sw[0], sw[2]);
        cm = {component(sw[0]), component(sw[1]), component(sw[2]), component(sw[3])};
        static const bool log_swizzle = getenv("RX_VK_LOG_SWIZZLE") != nullptr;   // (hookshot: RGB9E5 and R11G11B10 views)
        static int swizzle_logged;
        if (log_swizzle && nf == NVN_FORMAT_RGB9E5F && swizzle_logged++ < 12)
            logf("vk: sampled view of #%u NVN %#x as vk %d: swizzle %u %u %u %u (descriptor %u %u %u %u, view %d)", tex->id,
                 nf, (int)f, sw[0], sw[1], sw[2], sw[3], d.swizzle_r, d.swizzle_g, d.swizzle_b, d.swizzle_a,
                 v && v->set_swizzle ? 1 : 0);
    }
    if (twin_layer >= 0) {
        if (format_out) *format_out = f;
        return twin3d_view(tex, f, level0, nlevels, (uint32_t)twin_layer, cm);
    }
    uint64_t key = mix((uint64_t)use, (uint64_t)type);
    key = mix(key, (uint64_t)f << 32 | aspect);
    key = mix(key, (uint64_t)level0 << 48 | (uint64_t)nlevels << 32 | (uint64_t)layer0 << 16 | nlayers);
    key = mix(key, (uint64_t)cm.r << 24 | (uint64_t)cm.g << 16 | (uint64_t)cm.b << 8 | cm.a);
    if (format_out) *format_out = f;
    auto it = tex->views.find(key);
    if (it != tex->views.end()) return it->second;
    const VkImageUsageFlags usage = use == ViewUse::Sample ? VK_IMAGE_USAGE_SAMPLED_BIT
                                  : use == ViewUse::Storage ? VK_IMAGE_USAGE_STORAGE_BIT
                                  : depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                          : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const VkImageView view = make_view(tex, type, f, aspect, level0, nlevels, layer0, nlayers, cm, usage);
    if (!view) {
        static int logged;
        if (logged++ < 16)
            logf("vk: view of texture %#llx (%ux%u, vk %d, usage %#x) as vk %d type %d usage %#x "
                 "levels %u+%u layers %u+%u not made",
                 (unsigned long long)tex->gpu, tex->width, tex->height, (int)tex->format, tex->usage,
                 (int)f, (int)type, usage, level0, nlevels, layer0, nlayers);
    }
    tex->views[key] = view;
    return view;
}

bool texture_bc1_to_bc3(Texture* tex, const char* why) {
    if (!tex || !tex->bc1) return true;
    const bool had = tex->uploaded;
    if (!bc1_retype(*tex, why)) return false;
    if (!had) return true;
    // (what it held came from the game's memory, all of it: a BC1 image is never written by the GPU. Into the BC3 image
    // again, in this recording's uploads, ahead of the copy that follows)
    tex->uploaded = false;
    if (!upload(*tex, nullptr)) {
        // (not filled: the next draw that samples it uploads it whole, and the copy that asked is left out by its caller)
        static std::atomic<int> told{0};
        if (told++ < 16)
            logf("vk: texture #%u %#llx made BC3 (%s) could not be filled again now (%s)", tex->id, (unsigned long long)tex->gpu,
                 why, astc_fail ? astc_fail : "unknown");
        return false;
    }
    tex->uploaded = true;
    tex->upload_value = rec_value, tex->upload_seg = tl_seg;
    return true;
}

bool texture_copy_into_bc(VkCommandBuffer cmd, Texture* src, uint32_t src_level, uint32_t src_layer, int sx, int sy,
                          Texture* dst, uint32_t dst_level, uint32_t dst_layer, int dx, int dy, uint32_t w, uint32_t h,
                          uint32_t layers) {
    if (!src || !dst || !dst->bc || !src->astc || src->bc || !bc_ready() || w > kBcDim || h > kBcDim || (dx & 3) || (dy & 3))
        return false;
    if (dst->bc1 && !texture_bc1_to_bc3(dst, "a copy on the GPU into it")) return false;   // (Triforce: tex_bc1.h)
    // (hookshot-amd) into a resized BC3 image only whole levels, resized as their upload is
    const uint32_t ow = std::max(1u, dst->width >> dst_level), oh = std::max(1u, dst->height >> dst_level);
    if (dst->bc_resized) {
        const uint32_t gl = dst_level + dst->skip;
        const uint32_t gw = std::max(1u, dst->d.width >> gl), gh = std::max(1u, dst->d.height >> gl);
        if (dx || dy || w != gw || h != gh) {
            static int logged;
            if (logged++ < 8)
                logf("vk: a copy into part of resized BC3 texture %#llx (level %u, %d,%d %ux%u of %ux%u) left out",
                     (unsigned long long)dst->gpu, dst_level, dx, dy, w, h, gw, gh);
            return false;
        }
    }
    if (!bc_scratch_layout(cmd, dst->bc_resized)) return false;
    for (uint32_t l0 = 0; l0 < layers; l0 += kBcLayers) {
        const uint32_t n = std::min(kBcLayers, layers - l0);
        full_barrier(cmd);
        const VkImageCopy cp{{VK_IMAGE_ASPECT_COLOR_BIT, src_level, src_layer + l0, n}, {sx, sy, 0},
                             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, n}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, bc_scratch, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        if (dst->bc_resized)
            bc_encode_resized(cmd, w, h, ow, oh, n, *dst, dst_level, dst_layer + l0);
        else
            bc_encode_into(cmd, w, h, n, *dst, dst_level, dst_layer + l0, dx, dy);
    }
    return true;
}

// ---- what the GPU wrote into textures of virtual pools, back into their memory ----
// MK World draws its course maps' tiles (the terrain's height and material maps, 16384 x 16384 R8, 8192 x 8192 R16
// and RG16, sparse in virtual pools) into small 512 x 512 render targets in virtual pools of their own, finalizes
// those, unmaps their pools and maps the physical pages into the course map: on the console the texels are in that
// memory. Here they were only in the small targets' images, the course maps read zeros, and the terrain they shape
// was missing in places (ground drawn nowhere, the sky through it, snow left floating). Before virtual pages are
// unmapped or remapped (nvnMemoryPoolMapVirtual, nvnMemoryPoolFinalize: host/nvn_native.c), the textures the GPU
// wrote there are read back and written into their memory, block linear: the course maps then upload the tiles as
// they are mapped. The same when the CPU is about to read memory the GPU may have written (nvnMemoryPoolInvalidate-
// MappedRange, nvnBufferInvalidateMappedRange): MK World reads back a 1 x 1 RGBA32F texture its GPU computes (the
// scene's luminance), which was never in memory. RX_VK_NO_VMAP_FLUSH=1: off.
namespace {
constexpr uint64_t kFlushMax = 16ull << 20;   // (the small ones: render targets that size and more are not)
std::mutex vw_mu;
std::map<uint64_t, uint64_t> vw;   // (storage start -> end of the GPU-written textures of virtual pools that may be flushed)
bool flushable(const Texture& t) {
    if (!t.guest_size || t.guest_size > kFlushMax || t.astc || t.bc || t.sparse || t.skip ||
        t.samples != VK_SAMPLE_COUNT_1_BIT || t.aspect != VK_IMAGE_ASPECT_COLOR_BIT || t.bpb <= 0 || !t.image ||
        (nvn_tex_is_linear(&t.d) && (t.levels != 1 || t.type == VK_IMAGE_TYPE_3D)))   // (linear: one level, rows)
        return false;
    int from = 0;
    widened(t.d.format ? vk_format(t.nvn_format).format : t.format, &from);
    return !from;   // (the image's texels as the storage's: no 3-byte formats kept as four)
}
void vw_note(Texture& t) {
    static const bool off = getenv("RX_VK_NO_VMAP_FLUSH") != nullptr;
    if (off || t.vw_listed || !flushable(t)) return;
    std::lock_guard lk(vw_mu);
    uint64_t& end = vw[t.gpu];
    end = std::max(end, t.gpu + t.guest_size);
    t.vw_listed = true;
}
void vw_forget(const Texture& t) {   // (the entry goes; other listed textures at that start stay listed)
    if (!t.vw_listed) return;
    std::lock_guard lk(vw_mu);
    vw.erase(t.gpu);
    auto [a, b] = by_gpu.equal_range(t.gpu);
    for (auto it = a; it != b; ++it)
        if (it->second != &t && it->second->vw_listed) {
            uint64_t& end = vw[t.gpu];
            end = std::max(end, t.gpu + it->second->guest_size);
        }
}
// (a command buffer of its own for the read-backs, submitted and waited for at once)
VkCommandPool rb_pool = VK_NULL_HANDLE;
VkCommandBuffer rb_cmd = VK_NULL_HANDLE;
VkFence rb_fence = VK_NULL_HANDLE;
bool readback_begin() {
    if (!rb_pool) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = dev.queue_family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(dev.device, &pi, nullptr, &rb_pool) != VK_SUCCESS) return rb_pool = VK_NULL_HANDLE, false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = rb_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(dev.device, &ai, &rb_cmd) != VK_SUCCESS ||
            vkCreateFence(dev.device, &fi, nullptr, &rb_fence) != VK_SUCCESS)
            return false;
    }
    vkResetCommandPool(dev.device, rb_pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(rb_cmd, &bi) == VK_SUCCESS;
}
bool readback_submit_wait() {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};   // (the copies' writes, for the host)
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(rb_cmd, &d);
    if (vkEndCommandBuffer(rb_cmd) != VK_SUCCESS) return false;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = rb_cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    vkResetFences(dev.device, 1, &rb_fence);
    {
        std::lock_guard qk(dev.present_mu);   // (one queue: submits and presents take turns)
        queue_flush_locked();
        if (vkQueueSubmit2(dev.queue, 1, &si, rb_fence) != VK_SUCCESS) return false;
    }
    return vkWaitForFences(dev.device, 1, &rb_fence, VK_TRUE, 5000000000ull) == VK_SUCCESS;
}
/// A texture's write-back: its image's levels and layers read back, swizzled as the storage lays them out, and written into
/// its memory within [clip_lo, clip_hi). The GPU half (flush_record) goes into rb_cmd, the CPU half (flush_finish) runs once
/// the read-back completed: flush_textures does a batch of them with one submission and one wait. Each wait drains the
/// queue, and the retirement sweep wrote back hundreds one by one at a scene load (seconds of stall)
struct Flush {
    Texture* t;
    uint64_t clip_lo, clip_hi;
    struct Part { uint32_t level, layer; uint64_t at, n; LevelGeo g; };
    std::vector<Part> parts;
    uint64_t total = 0;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    bool ok = false;
    bool ignore_cpu = false;   // (RX_VK_AUDIT_GPU_ONLY's marks are writes of this process: they do not keep the texels out)
};
// (cached host memory, which the CPU reads: the sequential-write kind is write-combined, or VRAM behind the BAR, and every read
// of it by the unswizzle goes over PCIe uncached. RX_VK_FLUSH_UNCACHED=1: as before)
bool flush_uncached() {
    static const bool uncached = getenv("RX_VK_FLUSH_UNCACHED") != nullptr;
    return uncached;
}
/// The copies of f's image into a staging buffer of its own, recorded into rb_cmd (begun by the caller); false: nothing recorded
bool flush_record(Flush& f, VkCommandBuffer cmd = VK_NULL_HANDLE) {   // (cmd: where the copies go; none: rb_cmd)
    Texture& t = *f.t;
    const uint32_t layers = t.type == VK_IMAGE_TYPE_3D ? 1 : t.layers;
    for (uint32_t level = 0; level < t.levels; level++) {
        const LevelGeo g = level_geo(t, level);
        for (uint32_t layer = 0; layer < layers; layer++) {
            const uint64_t n = (uint64_t)g.tw * g.th * g.td * (uint64_t)t.bpb;
            f.parts.push_back({level, layer, f.total, n, g});
            f.total += (n + 15) & ~15ull;
        }
    }
    if (!f.total) return f.ok = false;
    if (!(flush_uncached() ? create_buffer(f.total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, f.buf, f.alloc, &f.map)
                           : create_buffer_cached(f.total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, f.buf, f.alloc, &f.map)) || !f.map) {
        if (f.buf) destroy_buffer(f.buf, f.alloc);
        f.buf = VK_NULL_HANDLE, f.alloc = nullptr, f.map = nullptr;
        return f.ok = false;
    }
    std::vector<VkBufferImageCopy> c;
    for (const Flush::Part& p : f.parts) {
        VkBufferImageCopy r{};
        r.bufferOffset = p.at;
        r.bufferRowLength = p.g.tw * (uint32_t)t.block_w;
        r.bufferImageHeight = p.g.th * (uint32_t)t.block_h;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, p.level, t.type == VK_IMAGE_TYPE_3D ? 0 : p.layer, 1};
        r.imageExtent = {p.g.w, p.g.h, p.g.d};
        c.push_back(r);
    }
    vkCmdCopyImageToBuffer(cmd ? cmd : rb_cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, f.buf, (uint32_t)c.size(), c.data());
    return f.ok = true;
}
/// The CPU half, once the batch's read-back completed (gpu_ok) or failed: the texels into the texture's memory, the buffer freed
bool flush_finish(Flush& f, bool gpu_ok) {
    using Part = Flush::Part;
    Texture& t = *f.t;
    const uint64_t clip_lo = f.clip_lo, clip_hi = f.clip_hi, total = f.total;
    const std::vector<Part>& parts = f.parts;
    void* map = f.map;
    bool ok = f.ok && gpu_ok;
    if (ok && !flush_uncached()) invalidate_mapped(f.alloc, total);
    if (ok && total <= 64) {   // (RX_VK_LOG_TINY_FLUSH=1: the values of tiny ones, 400 lines: a 1 x 1 luminance)
        static const bool log_tiny = getenv("RX_VK_LOG_TINY_FLUSH") != nullptr;
        static int logged;
        if (log_tiny && logged++ < 400) {
            const float* f = (const float*)map;
            const uint32_t* u = (const uint32_t*)map;
            logf("vk: tiny write-back #%u %#llx NVN %#x: %g %g %g %g (%08x %08x %08x %08x)", t.id, (unsigned long long)t.gpu,
                 t.nvn_format, f[0], f[1], f[2], f[3], u[0], u[1], u[2], u[3]);
        }
    }
    // (hookshot) pages the CPU wrote after the GPU last wrote the texture keep the CPU's bytes: on the console the GPU's
    // writes are in memory as it renders, and a later CPU write is what is there. OoT reuses a render target's memory
    // for scene data and then invalidates a range of it: this wrote the whole texture back over that data, and its
    // SceneLowPriority thread read texels as pointers (an access violation at 0xffffffffffffffff)
    std::vector<std::pair<uint64_t, uint64_t>> cpu_runs;   // (offsets from t.gpu)
    if (ok && !f.ignore_cpu && t.gpu < (1ull << 40)) memory_written_runs(t.gpu, t.guest_size, t.gpu_gen, cpu_runs);
    uint64_t kept_cpu = 0;
    // (and only into the pages being remapped: those are what the texture's content would be lost from. The rest of it stays where
    // the game may have put other things since. The watch above does not catch all of those: OoT's SceneLowPriority crash, walking
    // in Kokiri Forest, was texels landing right beside the remapped pages, on scene data)
    const auto write_back = [&](uint64_t whole_gpu, const uint8_t* whole_src, uint64_t whole_n) {
        const uint64_t from = std::max(whole_gpu, clip_lo), to = std::min(whole_gpu + whole_n, clip_hi);
        if (from >= to) return true;
        const uint64_t gpu = from, n = to - from;
        const uint8_t* src = whole_src + (from - whole_gpu);
        const uint64_t lo = gpu - t.gpu, hi = lo + n;
        bool w = true;
        uint64_t at = lo;
        for (const auto& [a, b] : cpu_runs) {
            if (b <= at) continue;
            if (a >= hi) break;
            if (a > at) w &= guest_cpu_write(t.gpu + at, src + (at - lo), a - at) != 0;
            kept_cpu += std::min(b, hi) - std::max(a, at);
            at = std::max(at, b);
            if (at >= hi) break;
        }
        if (at < hi) w &= guest_cpu_write(t.gpu + at, src + (at - lo), hi - at) != 0;
        return w;
    };
    const auto log_kept = [&] {
        static int logged;
        if (kept_cpu && logged++ < 20)
            logf("vk: write-back of #%u %#llx: %llu KB the CPU wrote since the GPU kept", t.id, (unsigned long long)t.gpu,
                 (unsigned long long)(kept_cpu >> 10));
    };
    if (ok && nvn_tex_is_linear(&t.d)) {   // (a linear texture: its rows at the stride, one level)
        const uint64_t stride = t.d.stride ? t.d.stride : host->tex_linear_stride(&t.d);
        for (const Part& p : parts) {
            const uint64_t row = (uint64_t)p.g.tw * (uint64_t)t.bpb;
            const uint64_t base = (uint64_t)p.layer * t.L.layer_size;
            for (uint32_t y = 0; y < p.g.th; y++) {
                const uint64_t off = base + (uint64_t)y * stride;
                if (off + row > t.guest_size) break;
                if (!write_back(t.gpu + off, (const uint8_t*)map + p.at + (uint64_t)y * row, row)) ok = false;
            }
        }
        log_kept();
        if (f.buf) destroy_buffer(f.buf, f.alloc);
        return ok;
    }
    if (ok) {
        std::vector<uint8_t> sw;
        for (const Part& p : parts) {
            const uint64_t at = t.gpu + p.g.offset + (uint64_t)p.layer * t.L.layer_size;
            if (p.g.offset + (uint64_t)p.layer * t.L.layer_size >= t.guest_size) continue;
            const uint64_t span = std::min<uint64_t>(p.g.swizzled, t.guest_size - p.g.offset - (uint64_t)p.layer * t.L.layer_size);
            sw.assign(std::max<uint64_t>(p.g.swizzled, span), 0);
            host->gpu_read(at, sw.data(), span);   // (what is there: the padding between texels kept)
            Tegra::Texture::SwizzleTexture(std::span<uint8_t>(sw.data(), sw.size()),
                                           std::span<const uint8_t>((const uint8_t*)map + p.at, p.n), (u32)t.bpb, p.g.tw,
                                           p.g.th, p.g.td, p.g.tile_h, p.g.tile_d, 1);
            if (!write_back(at, sw.data(), span)) ok = false;   // (marks the pages written: textures over them upload)
        }
        log_kept();
    }
    if (f.buf) destroy_buffer(f.buf, f.alloc);
    return ok;
}
/// A batch of write-backs: one submission and one wait for all of them
void flush_textures(std::vector<Flush>& fs) {
    bool any = false;
    if (readback_begin()) {
        full_barrier(rb_cmd);   // (after what the submissions before wrote)
        for (Flush& f : fs) any |= flush_record(f);
    } else {
        for (Flush& f : fs) f.ok = false;
    }
    const bool gpu_ok = any && readback_submit_wait();
    if (any && !gpu_ok) {
        // (the read-back did not complete: its 5 s wait, which also covers what queue_flush_locked just submitted, or a submit
        // that failed. The copies may still be writing into the staging buffers, so they are not freed here: they go the way a
        // destroyed image does, once a submission made after this one is done. The batch's write-backs are lost, as a single
        // one's was before)
        static int logged;
        if (logged++ < 8)
            logf("vk: a batch of %zu texture write-backs did not complete in time: not written back, its staging buffers kept "
                 "until the GPU is past them", fs.size());
        for (Flush& f : fs)
            if (f.buf) {
                Dead d{rec_value + 1, VK_NULL_HANDLE, nullptr, {}, {}};
                d.buffer = f.buf, d.buffer_alloc = f.alloc;
                dead.push_back(std::move(d));
                f.buf = VK_NULL_HANDLE, f.alloc = nullptr, f.map = nullptr;
            }
    }
    for (Flush& f : fs) f.ok = flush_finish(f, gpu_ok);
}
bool flush_texture(Texture& t, uint64_t clip_lo, uint64_t clip_hi) {
    std::vector<Flush> one;
    one.push_back(Flush{&t, clip_lo, clip_hi});
    flush_textures(one);
    return one[0].ok;
}
} // namespace

void texture_describe_written(uint64_t lo, uint64_t size, std::string& out) {
    const uint64_t hi = lo + size;
    int n = 0;
    for (auto& kv : textures) {
        const Texture* t = kv.second.get();
        if (t->gpu >= hi || t->gpu + t->guest_size <= lo || !t->gpu_written) continue;
        if (n++ < 6) {
            char b[160];
            snprintf(b, sizeof b, " #%u %#llx %ux%ux%u x%u NVN %#x %llu KB%s%s;", t->id, (unsigned long long)t->gpu, t->width,
                     t->height, t->depth, t->layers, t->nvn_format, (unsigned long long)(t->guest_size >> 10),
                     t->released ? " finalized" : "", flushable(*t) ? "" : " (not flushable)");
            out += b;
        }
    }
    if (n > 6) out += " (" + std::to_string(n) + " in all)";
}

bool texture_gpu_written_in(uint64_t lo, uint64_t size) {
    const uint64_t hi = lo + size;
    std::lock_guard lk(vw_mu);
    for (auto it = vw.lower_bound(lo > kFlushMax ? lo - kFlushMax : 0); it != vw.end() && it->first < hi; ++it)
        if (it->second > lo) return true;
    return false;
}

int texture_flush_to_guest(uint64_t lo, uint64_t size) {
    const uint64_t hi = lo + size;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Texture*> todo;
    for (auto& kv : textures) {
        Texture* t = kv.second.get();
        if (t->gpu >= hi || t->gpu + t->guest_size <= lo) continue;
        t->vw_listed = false;
        if (t->gpu_written && flushable(*t)) todo.push_back(t);
    }
    {
        std::lock_guard lk(vw_mu);
        for (auto it = vw.lower_bound(lo > kFlushMax ? lo - kFlushMax : 0); it != vw.end() && it->first < hi;)
            it = it->second > lo ? vw.erase(it) : std::next(it);
    }
    int done = 0;
    uint64_t bytes = 0;
    for (Texture* t : todo) {
        if (!flush_texture(*t, lo, hi)) continue;
        done++, bytes += t->guest_size;
        if (t->gpu < lo || t->gpu + t->guest_size > hi) continue;   // (a part of it: the rest is still only in the image)
        // (its memory is its content now: CPU writes to it count again, and an image made over it uploads it)
        t->gpu_written = false;
        t->uploaded = true;
        t->upload_gen = memory_generation();
        watch_tex(t, "written back into its memory");
    }
    static int64_t flushes, all_bytes;
    static double all_ms;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    flushes += done, all_bytes += (int64_t)bytes, all_ms += ms;
    static int logged;
    if (done && (logged++ < 12 || logged % 200 == 0))
        logf("vk: virtual pages %#llx..%#llx remapped: %d texture(s) the GPU wrote there written back into their memory "
             "(%llu KB, %.1f ms; %lld in all, %lld MB, %.0f ms)", (unsigned long long)lo, (unsigned long long)hi, done,
             (unsigned long long)(bytes >> 10), ms, (long long)flushes, (long long)(all_bytes >> 20), all_ms);
    return done;
}

void texture_written(Texture* tex, const char* why) {
    if (!tex) return;
    if (tex->upload_value == rec_value && tex->upload_seg > tl_seg) {   // (a parallel replay's order broken: see dry_mark_written)
        static int64_t n;
        if (n++ < 40 || n % 500 == 0)
            logf("vk: parallel: #%u %#llx %ux%u x%u NVN %#x written in segment %llu, uploaded by the later segment %llu (%lld so); "
                 "written by: %s; before this the GPU had %s it%s%s, first seen at frame %llu, now %llu",
                 tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, tex->nvn_format,
                 (unsigned long long)tl_seg, (unsigned long long)tex->upload_seg, (long long)n, why,
                 tex->gpu_written ? "written" : "not written", tex->only_cleared ? " (only cleared)" : "",
                 tex->released ? " (released)" : "", (unsigned long long)tex->first_seen, (unsigned long long)frame_no);
    }
    vw_note(*tex);
    tex->only_cleared = false;
    if (tex->sparse && !tex->gpu_written) {
        static int logged;
        if (logged++ < 20) logf("vk: sparse texture %#llx written by the GPU (#%u)", (unsigned long long)tex->gpu, tex->id);
    }
    watch_tex(tex, "written");
    // (RX_VK_AUDIT_GPU_ONLY: its storage marked as the GPU's when the GPU's contents begin; not one the CPU's texels were in first)
    if (audit_on() && (tex->gpu_written ? tex->released : !tex->uploaded)) audit_mark(*tex);
    // (Triforce: a run of frames the GPU draws into it begins when it was not drawn into in the frame before: its first frame, a
    // texture the game finalized and made again, or one the game draws into again later (a terrain tile's normal map baked a
    // second time into the same texture, 363 presents after the first))
    // (and how many such runs began within 30 presents of the one before: a target the game draws into every other frame, or
    // in turns with others, begins a run every few presents for as long as it lives. Written back at each, those were 20,537
    // write-backs and 19.7 GB in a seven-minute session, 0.7 ms of the GPU's frame and 1.1 ms of the replay's (2026-11-10);
    // texture_end_recording stops at the third in a row. Contents that begin anew, or a rest of more than 30 presents, count
    // from none again: a tile baked a second time is taken whenever it comes)
    if (!tex->gpu_written || tex->released) {
        tex->wb_burst = frame_no, tex->wb_quick = 0;
    } else if (tex->last_written + 1 < frame_no) {
        tex->wb_quick = frame_no - tex->wb_burst <= 30 ? tex->wb_quick + 1 : 0;
        tex->wb_burst = frame_no;
    }
    if (!tex->gpu_written || tex->released) tex->first_written = frame_no;
    // ((Triforce) redrawn.h: the frame it was written in before this one, for the new-texture budget's rule; read before
    // gpu_written and released are set for this write)
    tex->prev_written = redrawn::prev_on_write(tex->prev_written, tex->last_written, tex->gpu_written && !tex->released, frame_no);
    tex->last_written = frame_no;
    tex->gpu_written = true;
    upload_same::changed(tex->same);   // ((Triforce) its image's bytes are the GPU's: no kept hash of a re-upload's is theirs)
    tex->gpu_gen = memory_generation();
    tex->released = false;
    tex->uploaded = true;
    if (tex->owed.from) upload_owed::paid(tex->owed);   // (Triforce: the image is the content now, not what an upload still owed it)
    bake_note_written(tex);   // (Triforce: early in such a run, its texels go into its memory at this recording's end)
}


bool texture_touch_quick(Texture* tex, bool for_sampling) {
    tex->last_frame = frame_no;   // (the same values whichever thread stores them)
    if (const uint64_t g = tex->let_go) tex->back_gap = tex_pressure::taken_back(tex->back_gap, frame_no - g);   // (texture_touch)
    tex->let_go = 0;
    tex->last_value = rec_value;
    if (!for_sampling) return true;
    if (tex->gpu_written && !tex->released && !tex->only_cleared) return true;
    // (Triforce: read in touch_quick.h's order, each load where it stands: a locked look of this texture may be under way on
    // another thread, and what it has noted so far is not yet what it will leave)
    touch_quick::Seen seen;
    seen.uploaded = __atomic_load_n(&tex->uploaded, __ATOMIC_SEQ_CST);
    seen.checked_epoch = __atomic_load_n(&tex->checked_epoch, __ATOMIC_SEQ_CST);
    seen.touching = tex->touching.load();
    seen.upload_value = __atomic_load_n(&tex->upload_value, __ATOMIC_SEQ_CST);
    seen.upload_seg = __atomic_load_n(&tex->upload_seg, __ATOMIC_SEQ_CST);
    return touch_quick::up_to_date(seen, epoch, rec_value, tl_seg);
}
void texture_gpu_wrote(uint64_t gpu, uint64_t size) {
    if (!gpu || !size) return;
    const uint64_t lo = gpu, hi = gpu + size;
    {   // (RX_VK_WATCH_GPU_WRITE=<lo hex>[:<hi hex>]: the GPU buffer writes reaching that range, by program, 60 lines)
        static uint64_t wlo = 0, whi = 0;
        static const bool parsed = [] {
            if (const char* e = getenv("RX_VK_WATCH_GPU_WRITE")) {
                unsigned long long a = 0, b = 0;
                if (sscanf(e, "%llx:%llx", &a, &b) < 2) b = a + 1;
                wlo = a, whi = b;
            }
            return true;
        }();
        (void)parsed;
        static int logged;
        if (wlo < hi && lo < whi && logged++ < 60)
            logf("vk: watch: GPU buffer write [%#llx, +%#llx) into [%#llx, %#llx), frame %llu, program %016llx",
                 (unsigned long long)lo, (unsigned long long)size, (unsigned long long)wlo, (unsigned long long)whi,
                 (unsigned long long)frame_no, (unsigned long long)tl_watch_prog);
    }
    std::lock_guard lk(gpu_wrote_mu);
    auto it = gpu_wrote.find(lo);
    if (it != gpu_wrote.end() && it->second.first >= hi) {
        it->second.second = rec_value;
        return;
    }
    for (auto e = gpu_wrote.lower_bound(lo); e != gpu_wrote.end() && e->first < hi;)   // (ranges it covers)
        e = e->second.first <= hi ? gpu_wrote.erase(e) : std::next(e);
    gpu_wrote[lo] = {hi, rec_value};
    if (gpu_wrote.size() > 65536) gpu_wrote.erase(gpu_wrote.begin());
}
/// A finished GPU write into the texture's storage it was not uploaded after (and notes it as seen)
bool gpu_wrote_into(Texture& tex) {
    std::lock_guard lk(gpu_wrote_mu);
    if (gpu_wrote.empty()) return false;
    const uint64_t lo = tex.gpu, hi = tex.gpu + std::max<uint64_t>(tex.guest_size, 1);
    uint64_t newest = 0;
    auto it = gpu_wrote.upper_bound(lo);
    if (it != gpu_wrote.begin()) --it;
    for (; it != gpu_wrote.end() && it->first < hi; ++it)
        if (it->second.first > lo && it->second.second <= completed_value && it->second.second > tex.gpu_wrote_seen)
            newest = std::max(newest, it->second.second);
    if (!newest) return false;
    tex.gpu_wrote_seen = newest;
    static int logged;
    if (logged++ < 40)
        logf("vk: texture #%u %#llx %ux%u NVN %#x: the GPU wrote its storage (submission %llu): uploaded again", tex.id,
             (unsigned long long)tex.gpu, tex.width, tex.height, tex.nvn_format, (unsigned long long)newest);
    return true;
}
void texture_watch_prog(uint64_t prog) {
    tl_watch_prog = prog;
}
void texture_set_segment(uint64_t seg) {
    tl_seg = seg;
}
uint64_t texture_segment() {
    return tl_seg;
}
void texture_segments_begin(const VkCommandBuffer* init_cmds, size_t n) {
    // (no worker records meanwhile: before the segments' workers start, and after they are all done)
    seg_inits = std::vector<SegInit>(n);
    for (size_t i = 0; i < n; i++) seg_inits[i].cmd = init_cmds[i], seg_inits[i].open.store(true, std::memory_order_release);
}
void texture_segment_worker(int index) {
    tl_seg_index = index;
}
int texture_segment_end(int index) {
    if (index < 0 || index >= (int)seg_inits.size()) return 0;
    SegInit& s = seg_inits[index];
    std::unique_lock lk(s.mu, std::try_to_lock);
    if (!lk.owns_lock()) {   // ((Triforce, 11-17) another worker records into its init command buffer: the wait counted, upload_earliest_report)
        const auto t0 = std::chrono::steady_clock::now();
        st_seg_end_waiting.fetch_add(1);
        lk.lock();
        st_seg_end_waiting.fetch_sub(1);
        st_seg_end_waits.fetch_add(1, std::memory_order_relaxed);
        st_seg_end_wait_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
    }
    s.open.store(false, std::memory_order_release);   // (no new image's change, and no upload, goes to it from here: it is ended)
    if (s.used) full_barrier(s.cmd);   // (before its uploads and its commands)
    if (vkEndCommandBuffer(s.cmd) != VK_SUCCESS) return s.used ? -1 : 0;
    return s.used ? 1 : 0;
}
bool texture_marked(const Texture* tex) {
    return !tex || (tex->gpu_written && !tex->released && tex->uploaded && tex->owned_frame == frame_no &&
                    tex->first_render_frame);
}

namespace {
std::mutex sparse_mu;
std::vector<VkSparseImageMemoryBind> sparse_binds;   // (what the next flush binds, under sparse_mu)
std::vector<VkSparseMemoryBind> sparse_opaque;
VkDeviceSize sparse_added = 0;

/// One sparse block's memory (or n bytes) from the texture's chunks; false when none can be had
bool sparse_take(Texture* tex, VkDeviceSize n, VkDeviceMemory& mem, VkDeviceSize& off) {
    constexpr VkDeviceSize kChunk = 64ull << 20;
    n = (n + tex->sparse_align - 1) / tex->sparse_align * tex->sparse_align;
    if (tex->sparse_chunks.empty() || tex->chunk_used + n > kChunk) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = std::max(kChunk, n);
        ai.memoryTypeIndex = tex->sparse_type;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (vkAllocateMemory(dev.device, &ai, nullptr, &m) != VK_SUCCESS) {
            static int logged;
            if (logged++ < 8) logf("vk: sparse texture %#llx: no device memory for its blocks", (unsigned long long)tex->gpu);
            return false;
        }
        tex->sparse_chunks.push_back(m);
        tex->chunk_used = 0;
        tex->bytes += ai.allocationSize;
        sparse_added += ai.allocationSize;
    }
    mem = tex->sparse_chunks.back();
    off = tex->chunk_used;
    tex->chunk_used += n;
    return true;
}

bool sparse_flush(Texture* tex) {
    if (sparse_binds.empty() && sparse_opaque.empty()) return true;
    VkSparseImageMemoryBindInfo ib{tex->image, (uint32_t)sparse_binds.size(), sparse_binds.data()};
    VkSparseImageOpaqueMemoryBindInfo ob{tex->image, (uint32_t)sparse_opaque.size(), sparse_opaque.data()};
    VkBindSparseInfo bi{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    bi.imageBindCount = sparse_binds.empty() ? 0 : 1;
    bi.pImageBinds = &ib;
    bi.imageOpaqueBindCount = sparse_opaque.empty() ? 0 : 1;
    bi.pImageOpaqueBinds = &ob;
    static VkFence fence = VK_NULL_HANDLE;
    if (!fence) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(dev.device, &fi, nullptr, &fence);
    }
    VkResult res;
    {   // (bound before any work that writes the blocks is submitted: waited for here, on the recording thread. And
        // after the work submitted before is done: a sparse bind is not ordered with command buffers by their
        // barriers, and one that landed while a pass sampled the image changed what the pass read part-way through
        // its tiles. Opt-in, RX_VK_SPARSE_IDLE=1: MKW binds hundreds of blocks a race, and the replay's stalls at
        // each made its other races worse)
        static const bool no_idle = getenv("RX_VK_SPARSE_IDLE") == nullptr;
        static int64_t binds;
        std::lock_guard qk(dev.present_mu);
        queue_flush_locked();
        if (!no_idle) vkQueueWaitIdle(dev.queue);
        res = vkQueueBindSparse(dev.queue, 1, &bi, fence);
        if (++binds % 200 == 0) logf("vk: %lld sparse binds", (long long)binds);
    }
    if (res == VK_SUCCESS) {
        vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev.device, 1, &fence);
    } else {
        logf("vk: sparse texture %#llx: binding failed (%d)", (unsigned long long)tex->gpu, (int)res);
    }
    static int logged;
    static uint64_t next_mb = 0;
    if (logged < 20 || (tex->bytes >> 20) >= next_mb) {
        logged++;
        next_mb = (tex->bytes >> 20) + 256;
        logf("vk: sparse texture %#llx: %zu blocks bound (%llu MB of device memory)", (unsigned long long)tex->gpu,
             sparse_binds.size() + sparse_opaque.size(), (unsigned long long)(tex->bytes >> 20));
    }
    total_bytes += sparse_added;
    sparse_added = 0;
    sparse_binds.clear();
    sparse_opaque.clear();
    return res == VK_SUCCESS;
}

/// Queues the binds of what a rectangle reaches that is not bound yet (sparse_mu held)
bool sparse_queue(Texture* tex, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const VkSparseImageMemoryRequirements& r = tex->sparse_req;
    if (level >= tex->levels || layer >= tex->layers) return true;
    if (level >= r.imageMipTailFirstLod) {   // (the mip tail: bound whole)
        const bool single = r.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT;
        uint8_t& bound = tex->tail_bound[single ? 0 : layer];
        if (bound || !r.imageMipTailSize) return true;
        VkDeviceMemory m;
        VkDeviceSize off;
        if (!sparse_take(tex, r.imageMipTailSize, m, off)) return false;
        sparse_opaque.push_back({r.imageMipTailOffset + (single ? 0 : layer * r.imageMipTailStride), r.imageMipTailSize,
                                 m, off, 0});
        bound = 1;
        return true;
    }
    const VkExtent3D g = r.formatProperties.imageGranularity;
    const uint32_t lw = std::max(1u, tex->width >> level), lh = std::max(1u, tex->height >> level);
    if (x >= lw || y >= lh || !w || !h) return true;
    w = std::min(w, lw - x), h = std::min(h, lh - y);
    const uint32_t ntx = (lw + g.width - 1) / g.width, nty = (lh + g.height - 1) / g.height;
    std::vector<uint8_t>& map = tex->tiles[(size_t)layer * tex->levels + level];
    if (map.empty()) map.assign((size_t)ntx * nty, 0);
    bool ok = true;
    for (uint32_t ty = y / g.height; ty <= (y + h - 1) / g.height; ty++)
        for (uint32_t tx = x / g.width; tx <= (x + w - 1) / g.width; tx++) {
            uint8_t& bound = map[(size_t)ty * ntx + tx];
            if (bound) continue;
            VkDeviceMemory m;
            VkDeviceSize off;
            if (!sparse_take(tex, tex->sparse_align, m, off)) {
                ok = false;
                continue;
            }
            VkSparseImageMemoryBind b{};
            b.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer};
            b.offset = {(int32_t)(tx * g.width), (int32_t)(ty * g.height), 0};
            b.extent = {std::min(g.width, lw - tx * g.width), std::min(g.height, lh - ty * g.height), 1};
            b.memory = m;
            b.memoryOffset = off;
            sparse_binds.push_back(b);
            bound = 1;
        }
    return ok;
}
} // namespace

/// (the upload's: queued, and bound together before its copies are recorded)
void texture_resident_queue(Texture* tex, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    std::lock_guard lk(sparse_mu);
    sparse_queue(tex, level, layer, x, y, w, h);
}
void texture_resident_flush(Texture* tex) {
    if (!tex || !tex->sparse) return;
    std::lock_guard lk(sparse_mu);
    sparse_flush(tex);
}

bool texture_resident(Texture* tex, uint32_t level, uint32_t first, uint32_t count, uint32_t x, uint32_t y, uint32_t w,
                      uint32_t h) {
    if (!tex || !tex->sparse) return true;
    std::lock_guard lk(sparse_mu);
    bool ok = true;
    for (uint32_t layer = first; layer < std::min(tex->layers, first + std::max(1u, count)); layer++)
        ok &= sparse_queue(tex, level, layer, x, y, w, h);
    return sparse_flush(tex) && ok;
}

bool texture_resident(Texture* tex, uint32_t first, uint32_t count) {
    if (!tex || !tex->sparse) return true;
    std::lock_guard lk(sparse_mu);
    bool ok = true;
    for (uint32_t layer = first; layer < std::min(tex->layers, first + std::max(1u, count)); layer++)
        for (uint32_t level = 0; level < tex->levels; level++)
            ok &= sparse_queue(tex, level, layer, 0, 0, ~0u, ~0u);
    return sparse_flush(tex) && ok;
}

void texture_rendered(Texture* tex) {
    if (!tex) return;
    if (tex->sparse && !tex->first_render_frame) texture_resident(tex, 0, tex->layers);   // (rendered into: all of it)
    if (!tex->first_render_frame) tex->first_render_frame = frame_no;
    // (its storage's range as the GPU's, once a frame: render targets only. Copies and storage writes registered it
    // too, and a texture made later over such storage (MKW's terrain tiles reuse the slots of a pool) was not uploaded
    // and kept the placeholder it was made with)
    if (tex->owned_frame != frame_no) {
        tex->owned_frame = frame_no;
        const uint64_t lo = tex->gpu, hi = tex->gpu + std::max<uint64_t>(tex->guest_size, 1);
        for (auto it = gpu_owned.lower_bound(lo); it != gpu_owned.end() && it->first < hi;)
            it = it->second.first <= hi ? gpu_owned.erase(it) : std::next(it);
        gpu_owned[lo] = {hi, memory_generation()};
        if (gpu_owned.size() > 65536) gpu_owned.erase(gpu_owned.begin());   // (bounded; the oldest addresses go)
    }
}
uint64_t texture_frame_no() {
    return frame_no;
}

VkBufferView texel_buffer_view(const nvn_tex* t, const nvn_view* v, bool storage) {
    if (!t || !t->gpu) return VK_NULL_HANDLE;
    const int nf = view_nvn_format(t, v);
    VkFormat f = vk_format(nf).format;
    if (f == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(nf, &bw, &bh);
    if (bpb <= 0) return VK_NULL_HANDLE;
    const uint64_t size = (uint64_t)std::max(1u, t->d.width) * bpb;
    // (a formatted store into it: textures over that memory are uploaded again once the GPU is done)
    if (storage) texture_gpu_wrote(t->gpu, size);
    static std::unordered_map<uint64_t, VkBufferView> views;
    const uint64_t key = mix(mix(t->gpu, (uint64_t)f << 1 | storage), size);
    auto it = views.find(key);
    if (it != views.end()) return it->second;
    BufferRef b = gpu_buffer(t->gpu, size);
    VkBufferView bv = VK_NULL_HANDLE;
    if (b && b.offset % std::max<uint64_t>(dev.props.limits.minTexelBufferOffsetAlignment, 1) == 0) {
        VkBufferViewCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
        ci.buffer = b.buffer;
        ci.format = f;
        ci.offset = b.offset;
        ci.range = std::min<uint64_t>(size, (uint64_t)dev.props.limits.maxTexelBufferElements * bpb);
        vkCreateBufferView(dev.device, &ci, nullptr, &bv);
    }
    views[key] = bv;
    return bv;
}

// ---- (hookshot) VRAM compacted (on by default since the hold lets go of the dead render targets first and of long-unused
// textures only; RX_VK_COMPACT=0: off, as before) ----
// An evicted texture's memory stays inside its 128 MB block, and the driver counts the block whole: after some play 0.6 to
// 1.6 GB of what the driver counts is such free space (3 to 4 GB after the cache was emptied), and evicting more gives
// none of it back. Compacted, the textures that are not render targets have a pool of their own (vk_vma.cpp; the targets have theirs) and, when RX_VK_COMPACT_MB (512)
// or more of it is free space, VMA's defragmentation moves textures out of the sparsely used blocks, a few a pass: a new
// image at the new place, the texels copied on the GPU in the present's upload commands (every draw of the frame is in an
// earlier recording), the texture's views made again when next asked for, and the old image destroyed once the GPU is past
// the copy; then VMA frees the blocks that emptied. While a pass is open nothing of the pool is freed (the images the GPU
// is done with wait a frame or two): VMA holds the allocations it listed.
struct Compact {
    bool in_pass = false;
    uint64_t value = 0;                    // (the recording the open pass's copies are in)
    std::vector<VkImage> old_images;       // (of the open pass: destroyed when the GPU is past them)
    std::vector<VkImageView> old_views;
    uint64_t next_check = 0;               // (the present from which another compaction may start)
    uint64_t moved = 0, moved_bytes = 0, ignored = 0, passes = 0;   // (of the compaction under way)
    uint64_t runs = 0, all_moved = 0, all_freed = 0;
    // (the driver's count over 90% of what it allows, texture_frame's look every 10 presents: vram_compact.h)
    bool pressed = false;
} compact;
const Texture* present_source = nullptr;   // (what the present shows this frame: do_present holds its image and view)
void compact_step(uint64_t completed) {
    if (!compact_on()) return;
    if (compact.in_pass) {
        if (compact.value > completed) return;   // (the GPU has not run the copies yet)
        for (VkImageView v : compact.old_views) vkDestroyImageView(dev.device, v, nullptr);
        for (VkImage i : compact.old_images) vkDestroyImage(dev.device, i, nullptr);   // (their memory is VMA's: the pass's end frees it)
        compact.old_views.clear(), compact.old_images.clear();
        compact.in_pass = false;
        uint64_t freed = 0;
        uint32_t blocks = 0;
        if (!image_compact_pass_end(&freed, &blocks)) {
            compact.all_moved += compact.moved, compact.all_freed += freed;
            logf("vk: VRAM compacted: %llu textures moved (%llu MB) in %llu passes, %llu left where they were; %u blocks (%llu MB) "
                 "given back to the driver; %llu MB in all so far", (unsigned long long)compact.moved,
                 (unsigned long long)(compact.moved_bytes >> 20), (unsigned long long)compact.passes,
                 (unsigned long long)compact.ignored, blocks, (unsigned long long)(freed >> 20),
                 (unsigned long long)(compact.all_freed >> 20));
            compact.next_check = frame_no + vram_compact::rest(compact.pressed);
        }
        return;   // (the images the GPU is done with are freed now, before the next pass opens)
    }
    if (!image_compact_active()) {
        if (frame_no < compact.next_check) return;
        compact.next_check = frame_no + 120;
        static const uint64_t min_free = (uint64_t)(getenv("RX_VK_COMPACT_MB") ? std::max(1, atoi(getenv("RX_VK_COMPACT_MB"))) : 512) << 20;
        uint64_t block_bytes = 0, free_bytes = 0;
        if (!textures_pool_usage(&block_bytes, &free_bytes) ||
            free_bytes < vram_compact::run_from(min_free, compact.pressed))
            return;
        if (!image_compact_begin(64ull << 20, 12)) return;
        compact.runs++, compact.moved = compact.moved_bytes = compact.ignored = compact.passes = 0;
        logf("vk: VRAM compaction %llu begins: the textures' blocks hold %llu MB, %llu MB of it free space",
             (unsigned long long)compact.runs, (unsigned long long)(block_bytes >> 20), (unsigned long long)(free_bytes >> 20));
    }
    static thread_local std::vector<ImageMove> moves;
    uint64_t freed_now = 0;
    uint32_t blocks_now = 0;
    if (!image_compact_pass(moves, &freed_now, &blocks_now)) {   // (nothing left to move)
        compact.all_moved += compact.moved, compact.all_freed += freed_now;
        logf("vk: VRAM compaction %llu over: %llu textures moved (%llu MB) in %llu passes, %llu left where they were; %u blocks "
             "(%llu MB) given back to the driver, %llu MB in all so far", (unsigned long long)compact.runs,
             (unsigned long long)compact.moved, (unsigned long long)(compact.moved_bytes >> 20), (unsigned long long)compact.passes,
             (unsigned long long)compact.ignored, blocks_now, (unsigned long long)(freed_now >> 20),
             (unsigned long long)(compact.all_freed >> 20));
        compact.next_check = frame_no + vram_compact::rest(compact.pressed);
        return;
    }
    std::unordered_map<VmaAllocation, Texture*> by_allocation;
    by_allocation.reserve(textures.size());
    for (auto& kv : textures)
        if (kv.second->allocation) by_allocation[kv.second->allocation] = kv.second.get();
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    for (size_t i = 0; i < moves.size(); i++) {
        const auto f = by_allocation.find(moves[i].src);
        Texture* t = f == by_allocation.end() ? nullptr : f->second;   // (none: an image the GPU is not done with)
        const VkImageUsageFlags both = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        bool ok = t && t->image && !t->sparse && t != present_source && t->samples == VK_SAMPLE_COUNT_1_BIT &&
                  (t->usage & both) == both;
        VkImage image = VK_NULL_HANDLE;
        if (ok) {
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (as create() made it)
            ci.flags = t->create_flags;
            ci.imageType = t->type;
            ci.format = t->format;
            ci.extent = {t->width, t->height, t->depth};
            ci.mipLevels = t->levels;
            ci.arrayLayers = t->layers;
            ci.samples = t->samples;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = t->usage;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ok = vkCreateImage(dev.device, &ci, nullptr, &image) == VK_SUCCESS;
            if (ok && !image_compact_bind(i, image)) {
                vkDestroyImage(dev.device, image, nullptr);
                ok = false;
            }
        }
        if (!ok) {
            image_compact_ignore(i);
            compact.ignored++;
            continue;
        }
        if (!cmd) {
            cmd = upload_cmd();
            full_barrier(cmd, "compaction: before the copies");
        }
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {t->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
        static thread_local std::vector<VkImageCopy> regions;
        regions.clear();
        for (uint32_t l = 0; l < t->levels; l++) {
            VkImageCopy r{};
            r.srcSubresource = r.dstSubresource = {t->aspect, l, 0, t->layers};
            r.extent = {std::max(1u, t->width >> l), std::max(1u, t->height >> l), std::max(1u, t->depth >> l)};
            regions.push_back(r);
        }
        vkCmdCopyImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regions.size(), regions.data());
        compact.old_images.push_back(t->image);
        for (auto& kv : t->views)
            if (kv.second) compact.old_views.push_back(kv.second);
        t->views.clear();   // (made again when next asked for)
        t->image = image;
        views_gone.went(t->id);   // ((Triforce) its views went; the generation moves after the loop (view_keep.h))
        t->last_value = std::max(t->last_value, rec_value);   // (its end, if it goes now, waits for the copy too)
        compact.moved++, compact.moved_bytes += t->bytes + t->pad;
    }
    compact.passes++;
    if (!cmd) {   // (nothing of this pass could be moved: ended at once)
        uint64_t freed = 0;
        uint32_t blocks = 0;
        if (!image_compact_pass_end(&freed, &blocks)) {
            compact.all_moved += compact.moved, compact.all_freed += freed;
            logf("vk: VRAM compacted: %llu textures moved (%llu MB) in %llu passes, %llu left where they were; %u blocks (%llu MB) "
                 "given back to the driver; %llu MB in all so far", (unsigned long long)compact.moved,
                 (unsigned long long)(compact.moved_bytes >> 20), (unsigned long long)compact.passes,
                 (unsigned long long)compact.ignored, blocks, (unsigned long long)(freed >> 20),
                 (unsigned long long)(compact.all_freed >> 20));
            compact.next_check = frame_no + vram_compact::rest(compact.pressed);
        }
        return;
    }
    full_barrier(cmd, "compaction: after the copies");
    destroy_gen++;   // (what was resolved to the moved textures' views is stale)
    compact.in_pass = true, compact.value = rec_value;
}


// ---- (Triforce) what the GPU baked, in the game's memory too ----
// On the console a render target's texels are in its storage, and OoT's terrain relies on that. Each tile's 260 x 260 RG8
// normal map (NVN 0xd, 9 levels) is drawn once from the tile's height map (program aa9a8316); the game's files hold no
// normal maps (TerrainArc: .hght, .mate, .bake_ao, .bake_gi). When a tile is wanted again the game does not draw it: it
// makes a texture over the bytes it kept, at the same storage or wherever its tile cache has moved them since, and samples
// that. Here the texels were only in the image, so that texture was uploaded from memory they never reached: the ground's
// normals empty, the dirt stretched into streaks over all of Hyrule Field once tiles came back from the cache (2026-11-10,
// a five-minute run with RX_VK_WATCH_DIM=260x260: 61 of 146 normal maps were sampled without ever being drawn, all at
// storage no texture had been drawn at; reloading the save, which bakes them again, cleared it). Upstream's "finalized
// textures kept" covers only a texture made again over the same storage while its image is still there.
// So a texture the GPU drew in one frame, that the game still has and still samples, gets its texels written into its
// memory a few presents later: the copy to a staging buffer goes into the present's upload commands (no wait), and the
// texels are swizzled into the memory once the GPU is past it. Pages the CPU wrote since the draw keep the CPU's bytes
// (flush_finish), and one finalized or drawn again in between is left alone. RX_VK_BAKE_WRITEBACK=0: off, as before.
struct BakeItem {
    Flush f;
    uint32_t id;
    uint64_t gpu, written;   // (its storage, and the frame it was drawn in, as recorded)
};
struct BakeBatch {
    uint64_t value = 0;   // (the recording the copies are in)
    std::vector<BakeItem> items;
};
std::deque<BakeBatch> bake_batches;
uint64_t st_bake_wb, st_bake_wb_bytes, st_bake_wb_lost, st_bake_wb_logged;
uint64_t st_bake_gpu, st_bake_gpu_bytes, swizzle_map_bytes, swizzle_map_n;   // (texture_end_recording's, below)
struct BakeKind { uint32_t w, h, levels; int format; uint64_t n, bytes; };
std::unordered_map<uint64_t, BakeKind> bake_kinds;   // (what it wrote back, by size and format: the log's largest)
bool bake_writeback_on() {
    static const bool on = !getenv("RX_VK_BAKE_WRITEBACK") || atoi(getenv("RX_VK_BAKE_WRITEBACK")) != 0;
    return on;
}
void bake_writeback_step(uint64_t completed) {
    if (!bake_writeback_on()) return;
    while (!bake_batches.empty() && bake_batches.front().value <= completed) {   // (the CPU half of what the GPU is past)
        for (BakeItem& it : bake_batches.front().items) {
            Texture* t = nullptr;   // (still there: the pointer may be another texture's by now, the id tells)
            auto [a, e] = by_gpu.equal_range(it.gpu);
            for (auto k = a; k != e; ++k)
                if (k->second == it.f.t && k->second->id == it.id) t = k->second;
            if (t && t->gpu_written && !t->released && t->last_written == it.written) {
                if (flush_finish(it.f, true)) {
                    t->wb_done_frame = frame_no;
                    st_bake_wb++, st_bake_wb_bytes += t->guest_size;
                    watch_tex(t, "baked: its texels written into its memory");
                } else {
                    t->wb_written = 0, st_bake_wb_lost++;
                }
            } else {   // (gone, finalized or drawn again since: its memory is left as it is; drawn again, it is listed again)
                if (it.f.buf) destroy_buffer(it.f.buf, it.f.alloc);
                if (t && t->wb_written == it.written) t->wb_written = 0;
                st_bake_wb_lost++;
            }
        }
        bake_batches.pop_front();
    }
    if (frame_no % 600 == 0 && st_bake_wb + st_bake_wb_lost + st_bake_gpu != st_bake_wb_logged) {
        st_bake_wb_logged = st_bake_wb + st_bake_wb_lost + st_bake_gpu;
        logf("vk: what the GPU drew into small textures, in their memory too: %llu by the recordings that drew them (%llu KB, "
             "%zu layouts' maps of %llu KB); some presents later, by the CPU: %llu (%llu KB), %llu left out (finalized, gone or "
             "drawn again first)", (unsigned long long)st_bake_gpu, (unsigned long long)(st_bake_gpu_bytes >> 10), (size_t)swizzle_map_n,
             (unsigned long long)(swizzle_map_bytes >> 10), (unsigned long long)st_bake_wb, (unsigned long long)(st_bake_wb_bytes >> 10),
             (unsigned long long)st_bake_wb_lost);
        std::vector<const BakeKind*> top;   // (the kinds that take the most of it)
        for (const auto& kv : bake_kinds) top.push_back(&kv.second);
        std::sort(top.begin(), top.end(), [](const BakeKind* a, const BakeKind* b) { return a->bytes > b->bytes; });
        char line[400];
        int n = 0;
        for (size_t i = 0; i < top.size() && i < 5 && n < 330; i++)
            n += snprintf(line + n, sizeof line - n, " %ux%u levels %u NVN %#x: %llu (%llu KB);", top[i]->w, top[i]->h, top[i]->levels,
                          top[i]->format, (unsigned long long)top[i]->n, (unsigned long long)(top[i]->bytes >> 10));
        if (n) logf("vk:   by the recordings, the largest of %zu kinds:%s", bake_kinds.size(), line);
    }
    if (frame_no % 4) return;
    // (drawn in one frame, four presents or more ago; still the game's, and sampled in the last four presents: the game's own
    // draws say its storage is still this texture's. The small ones the eviction calls baked; the guest window's, which the
    // game's code can read)
    BakeBatch nb;
    uint64_t bytes = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    for (auto& kv : textures) {
        Texture* t = kv.second.get();
        if (!t->gpu_written || t->released || t->only_cleared || !t->first_written || t->first_written != t->last_written ||
            t->wb_written == t->last_written || frame_no < t->last_written + 4 || frame_no - t->last_frame > 4 ||
            t->bytes > (4ull << 20) || t->gpu >= (1ull << 39) || !t->image || !flushable(*t))
            continue;
        if (nb.items.size() >= 24 || bytes + t->guest_size > (8ull << 20)) break;   // (the rest four presents on)
        if (!cmd) {
            cmd = upload_cmd();
            full_barrier(cmd, "bake write-back: before the copies");
        }
        BakeItem it{Flush{t, t->gpu, t->gpu + t->guest_size}, t->id, t->gpu, t->last_written};
        it.f.ignore_cpu = audit_on();
        if (!flush_record(it.f, cmd)) continue;   // (no staging buffer: tried again four presents on)
        t->wb_written = t->last_written, t->wb_done_frame = 0;
        t->last_value = std::max(t->last_value, rec_value);   // (its end, if it goes now, waits for the copy)
        bytes += t->guest_size;
        nb.items.push_back(std::move(it));
    }
    if (nb.items.empty()) return;
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};   // (the copies' writes, for the host)
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    nb.value = rec_value;
    bake_batches.push_back(std::move(nb));
}
// ---- (Triforce) and at once: on the GPU, by the recording that drew it ----
// The write-back above reaches the memory some presents after the draw, and the game can be there first: it waits for the
// GPU to finish the frame and then moves a freshly baked tile into its cache (2026-11-10, with the write-back above alone:
// 268 of 275 tiles made again from memory had their texels, 7 were empty, each one baked 9 presents or fewer before). On
// the console the texels are in the storage when the GPU has run the draw. So a recording that draws into a small texture
// in the texture's first frame ends with commands of its own that put the image's texels into the storage: the levels
// copied to a buffer row by row, a compute pass that lays them out as the storage does (shaders/swizzle_back.comp, from a
// map made once per geometry with the CPU's swizzle), and a copy into the imported pool. When the submission is done, so
// is the memory. RX_VK_BAKE_GPU=0: only the write-back above.
struct SwizzleMap {
    VkBuffer buf = VK_NULL_HANDLE;   // (none: this geometry is not written back on the GPU)
    VmaAllocation alloc = nullptr;
    uint64_t lin_bytes = 0;                    // (the levels and layers, row by row: the linear copy's size)
    std::vector<VkBufferImageCopy> regions;    // (the image into it)
    uint32_t unit_shift = 0;                   // (log2 of a block's bytes)
};
std::unordered_map<uint64_t, SwizzleMap> swizzle_maps;   // (by the storage's geometry)
VkDescriptorSetLayout swz_set = VK_NULL_HANDLE;
VkPipelineLayout swz_layout = VK_NULL_HANDLE;
VkPipeline swz_pipe = VK_NULL_HANDLE;
bool swizzle_back_ready() {
    static int state = 0;
    if (state) return state > 0;
    state = -1;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    if (getenv("RX_VK_BAKE_GPU") && !atoi(getenv("RX_VK_BAKE_GPU"))) return false;
    const VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 3;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &swz_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &swz_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &swz_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof SWIZZLE_BACK_COMP_SPV;
    mi.pCode = SWIZZLE_BACK_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = swz_layout;
    const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &swz_pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) return false;
    state = 1;
    logf("vk: what the GPU draws into a small texture in its first frame goes into the texture's memory with the recording "
         "that drew it (RX_VK_BAKE_GPU=0: some presents later, by the CPU)");
    return true;
}
/// The map of t's geometry (made when first asked for), or null when it has none: where in the linear copy each block of the
/// storage comes from. Found by swizzling, with the swizzle the write-backs use, a level whose blocks hold their own numbers
const SwizzleMap* swizzle_map_of(const Texture& t) {
    const uint32_t layers = t.type == VK_IMAGE_TYPE_3D ? 1 : t.layers;
    uint64_t key = mix(mix(mix(0x53575Aull, (uint64_t)t.bpb), t.guest_size), mix(t.levels, mix(layers, t.L.layer_size)));
    for (uint32_t level = 0; level < t.levels; level++) {
        const LevelGeo g = level_geo(t, level);
        key = mix(mix(mix(key, g.offset), mix(g.tw, g.th)), mix(mix(g.td, g.tile_h), mix(g.tile_d, mix(g.w, mix(g.h, g.d)))));
    }
    if (const auto it = swizzle_maps.find(key); it != swizzle_maps.end()) return it->second.buf ? &it->second : nullptr;
    SwizzleMap& m = swizzle_maps[key];   // (kept without a buffer when it cannot be made: not tried again)
    const uint64_t bpb = (uint64_t)t.bpb, units = (t.guest_size + bpb - 1) / bpb;
    if (swizzle_maps.size() > 256 || swizzle_map_bytes + units * 4 > (96ull << 20)) return nullptr;
    m.unit_shift = (uint32_t)std::countr_zero((uint32_t)t.bpb);
    std::vector<uint32_t> map(units, 0xffffffffu);
    std::vector<uint8_t> src, dst;
    std::vector<uint32_t> number;
    for (uint32_t level = 0; level < t.levels; level++) {
        const LevelGeo g = level_geo(t, level);
        const uint64_t n = (uint64_t)g.tw * g.th * g.td * bpb, blocks = n / bpb;
        for (uint32_t layer = 0; layer < layers; layer++) {
            const uint64_t at = m.lin_bytes;
            m.lin_bytes += (n + 15) & ~15ull;
            VkBufferImageCopy r{};
            r.bufferOffset = at;
            r.bufferRowLength = g.tw * (uint32_t)t.block_w;
            r.bufferImageHeight = g.th * (uint32_t)t.block_h;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, t.type == VK_IMAGE_TYPE_3D ? 0 : layer, 1};
            r.imageExtent = {g.w, g.h, g.d};
            m.regions.push_back(r);
            const uint64_t so = g.offset + (uint64_t)layer * t.L.layer_size;
            if (so >= t.guest_size || !blocks) continue;
            if (so % bpb || m.lin_bytes > 0xfffffff0ull) return m.regions.clear(), nullptr;
            const uint64_t span = std::min<uint64_t>(g.swizzled, t.guest_size - so);
            number.assign((size_t)(span / bpb), 0);
            for (uint32_t k = 0; k < 4 && ((blocks + 1) >> (8 * k)) != 0; k++) {   // (a block's number + 1, a byte of it a pass)
                src.assign((size_t)n, 0);
                for (uint64_t i = 0; i < blocks; i++) src[(size_t)(i * bpb)] = (uint8_t)((i + 1) >> (8 * k));
                dst.assign((size_t)std::max<uint64_t>(g.swizzled, span), 0);
                Tegra::Texture::SwizzleTexture(std::span<uint8_t>(dst.data(), dst.size()), std::span<const uint8_t>(src.data(), src.size()),
                                               (u32)t.bpb, g.tw, g.th, g.td, g.tile_h, g.tile_d, 1);
                for (size_t j = 0; j < number.size(); j++) number[j] |= (uint32_t)dst[j * bpb] << (8 * k);
            }
            for (size_t j = 0; j < number.size(); j++)
                if (number[j] && number[j] <= blocks) map[(size_t)(so / bpb) + j] = (uint32_t)(at + (uint64_t)(number[j] - 1) * bpb);
        }
    }
    void* mapped = nullptr;
    if (!m.lin_bytes || !create_buffer(units * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, m.buf, m.alloc, &mapped) || !mapped) {
        if (m.buf) destroy_buffer(m.buf, m.alloc);
        m.buf = VK_NULL_HANDLE, m.alloc = nullptr;
        return m.regions.clear(), nullptr;
    }
    std::memcpy(mapped, map.data(), (size_t)units * 4);
    flush_mapped(m.alloc, units * 4);
    swizzle_map_bytes += units * 4, swizzle_map_n++;
    return &m;
}
// (The textures drawn into early in a run of frames (the run's first frame and the next: a bake finished a frame later is
// taken too; a target drawn every frame is written back twice when it is made and no more), by the submission whose recording
// drew them: recordings of several submissions can be open at once (a queue stopped at an event wait while another's goes
// on), and the texels must be copied by the recording that holds the draws, after them)
struct BakeFresh { Texture* t; uint32_t id; uint64_t gpu; };
std::mutex bake_fresh_mu;
std::unordered_map<uint64_t, std::vector<BakeFresh>> bake_fresh;   // (by rec_value)
uint64_t bake_rec_no = 1;           // (counts the recordings ended: a texture is listed once in each)
void bake_note_written(Texture* t) {   // (texture_written)
    if (frame_no - t->wb_burst > 1 || t->wb_quick > 2 || t->wb_rec == bake_rec_no || !bake_writeback_on()) return;
    t->wb_rec = bake_rec_no;
    std::lock_guard lk(bake_fresh_mu);
    bake_fresh[rec_value].push_back({t, t->id, t->gpu});
}
void texture_end_recording(VkCommandBuffer cmd) {
    std::vector<BakeFresh> list;
    {
        std::lock_guard lk(bake_fresh_mu);
        bake_rec_no++;
        if (const auto it = bake_fresh.find(rec_value); it != bake_fresh.end()) list.swap(it->second), bake_fresh.erase(it);
        if (bake_fresh.size() > 64) bake_fresh.clear();   // (recordings that never ended here: a bound)
    }
    if (list.empty() || par_active.load(std::memory_order_relaxed) || !swizzle_back_ready()) return;
    bool any = false;
    std::unordered_set<uint32_t> seen;
    for (const BakeFresh& f : list) {
        Texture* t = nullptr;   // (still there: a present may have come between the draw and this end)
        auto [a, e] = by_gpu.equal_range(f.gpu);
        for (auto k = a; k != e; ++k)
            if (k->second == f.t && k->second->id == f.id) t = k->second;
        if (!t || !seen.insert(f.id).second) continue;
        const VkImageUsageFlags need = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!t->image || t->released || t->only_cleared || t->bytes > (4ull << 20) ||
            t->gpu >= (1ull << 39) || (t->usage & need) != need || !flushable(*t) || nvn_tex_is_linear(&t->d) || t->bpb > 16 ||
            (t->bpb & (t->bpb - 1)))
            continue;
        const SwizzleMap* m = swizzle_map_of(*t);
        if (!m) continue;
        const BufferRef pool = gpu_buffer(t->gpu, t->guest_size);
        if (!pool) continue;
        const uint64_t words = (t->guest_size + 3) / 4;
        VkBuffer lin = VK_NULL_HANDLE, sto = VK_NULL_HANDLE;
        VmaAllocation lin_a = nullptr, sto_a = nullptr;
        if (!create_device_buffer(m->lin_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, lin, lin_a))
            continue;
        if (!create_device_buffer(words * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sto, sto_a)) {
            destroy_buffer(lin, lin_a);
            continue;
        }
        full_barrier(cmd, "bake write-back: before the copies");
        vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, lin, (uint32_t)m->regions.size(), m->regions.data());
        if (audit_on()) {   // (RX_VK_AUDIT_GPU_ONLY: no mark left between the texels)
            vkCmdFillBuffer(cmd, sto, 0, VK_WHOLE_SIZE, 0);
        } else {
            const VkBufferCopy in{pool.offset, 0, t->guest_size};   // (the storage as it is: what is no texel stays)
            vkCmdCopyBuffer(cmd, pool.buffer, sto, 1, &in);
        }
        full_barrier(cmd, "bake write-back: before the swizzle");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, swz_pipe);
        const VkDescriptorBufferInfo bi[3] = {{m->buf, 0, VK_WHOLE_SIZE}, {lin, 0, VK_WHOLE_SIZE}, {sto, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[3]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        for (uint32_t i = 0; i < 3; i++)
            w[i].dstBinding = i, w[i].descriptorCount = 1, w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[i].pBufferInfo = &bi[i];
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, swz_layout, 0, 3, w);
        const uint32_t pc[2] = {(uint32_t)words, m->unit_shift};
        vkCmdPushConstants(cmd, swz_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cmd, (uint32_t)((words + 63) / 64), 1, 1);
        full_barrier(cmd, "bake write-back: before the copy out");
        const VkBufferCopy out{0, pool.offset, t->guest_size};
        vkCmdCopyBuffer(cmd, sto, pool.buffer, 1, &out);
        for (const auto& [buf, alloc] : {std::pair{lin, lin_a}, std::pair{sto, sto_a}}) {   // (given back once the GPU is past them)
            Dead d{rec_value, VK_NULL_HANDLE, nullptr, {}, {}};
            d.buffer = buf, d.buffer_alloc = alloc;
            dead.push_back(std::move(d));
        }
        // (the pages count as written from here on: a texture made over them is uploaded from them, a mirror takes them again)
        memory_stamp_gpu(host->gpu_guest(t->gpu), t->guest_size);
        t->wb_written = t->last_written, t->wb_value = rec_value, t->wb_done_frame = frame_no;
        t->last_value = std::max(t->last_value, rec_value);
        st_bake_gpu++, st_bake_gpu_bytes += t->guest_size;
        {
            BakeKind& k = bake_kinds[mix(mix(t->width, t->height), mix(t->levels, (uint64_t)t->nvn_format))];
            k.w = t->width, k.h = t->height, k.levels = t->levels, k.format = t->nvn_format, k.n++, k.bytes += t->guest_size;
        }
        watch_tex(t, "baked: its texels into its memory by this recording");
        any = true;
    }
    if (!any) return;
    full_barrier(cmd, "bake write-back: after");
    descriptors_disturbed();
}
// (lab: RX_VK_BAKE_REUPLOAD=1) What the game does to a tile's normal map when the tile comes back from its cache, done here to
// every bake so that it can be checked at any save, standing still: 24 presents after it was drawn, its image is filled from
// its memory. With the write-back above the picture stays as it was (the texels made the round trip through the memory, in
// the storage's layout); with RX_VK_BAKE_WRITEBACK=0 the image gets what the memory held without it: the streaks.
void bake_reupload_step(uint64_t completed) {
    static unsigned only_w = 0, only_h = 0;   // (RX_VK_BAKE_REUPLOAD=<w>x<h>: the bakes of that size alone; 1: all of them)
    static const bool on = [] {
        const char* e = getenv("RX_VK_BAKE_REUPLOAD");
        if (!e) return false;
        if (sscanf(e, "%ux%u", &only_w, &only_h) != 2) only_w = only_h = 0;
        return only_w || atoi(e) != 0;
    }();
    if (!on || frame_no % 4) return;
    static std::unordered_set<uint32_t> done;
    for (auto& kv : textures) {
        Texture* t = kv.second.get();
        if (only_w && (t->width != only_w || t->height != only_h)) continue;
        if (!t->gpu_written || t->released || t->only_cleared || !t->first_written || t->first_written != t->last_written ||
            frame_no < t->last_written + 24 || frame_no - t->last_frame > 4 || t->bytes > (4ull << 20) ||
            t->gpu >= (1ull << 39) || !t->image || !flushable(*t) || done.count(t->id))
            continue;
        // (with the write-back: only once its texels are in the memory, two presents after they got there)
        if (bake_writeback_on() && (t->wb_written != t->last_written || !t->wb_done_frame || frame_no < t->wb_done_frame + 2 ||
                                    t->wb_value > completed))
            continue;
        done.insert(t->id);
        const bool ok = upload(*t, nullptr);
        if (done.size() <= 60)
            logf("vk: lab: #%u %#llx %ux%u x%u levels %u NVN %#x, drawn at frame %llu: its image filled from its memory again%s",
                 t->id, (unsigned long long)t->gpu, t->width, t->height, t->layers, t->levels, t->nvn_format,
                 (unsigned long long)t->last_written, ok ? "" : " (the upload failed)");
    }
}

Texture* texture_for_present(const nvn_tex* t) {
    Texture* tex = texture_get(t, false);
    if (tex && tex->gpu_written) return present_source = tex, tex;
    Texture* best = nullptr;   // (one rendered at its storage in another description)
    auto [a, b] = by_gpu.equal_range(t ? t->gpu : 0);
    for (auto it = a; it != b; ++it)
        if (it->second->gpu_written && (it->second->aspect & VK_IMAGE_ASPECT_COLOR_BIT) &&
            (!best || it->second->last_frame > best->last_frame))
            best = it->second;
    present_source = best ? best : tex;
    return best ? best : tex;
}

/// (hookshot) The texture budget a quality preset sets, or the card's own (tests/test_preset_texture_budget.py). The preset's
/// replaced `own` outright, and `own` is where RX_VK_VRAM_MB and an integrated GPU's cap are applied: with a preset on, the
/// documented RX_VK_VRAM_MB did nothing (the mirrors' RX_VK_MIRROR_MB wins over the preset), and an integrated GPU on Low or
/// Balanced got 3.6 or 4.8 GB of textures past the 2.5 GB that keeps a 16 GB laptop from running out of RAM. Now
/// RX_VK_VRAM_MB wins over a preset, and on an integrated GPU the preset's budget is at most `own`
/// (Triforce, 11-16) and on every card: a preset lowers the budget, never raises it past the card's own. Balanced (8GB) set
/// 4,800 MB of textures on an 8 GB card the driver allows ~7,300 MB of, where the card's own is 45% of that (3,285 MB): the
/// game's other VRAM is 3.5-3.8 GB, so the whole went past the card and Windows paged it (the 14 fps of the comment at
/// `own` in texture_frame). High (12GB) asked 7,500 MB where the card's own stops at 6,144 MB for the same reason.
/// RX_VK_PRESET_BUDGET_RAISES=1: as before
uint64_t preset_texture_budget(uint64_t preset_mb, bool preset_applies, bool vram_env, bool integrated, uint64_t own) {
    if (!preset_mb || !preset_applies || vram_env) return own;
    const uint64_t b = preset_mb << 20;
    static const bool raises = getenv("RX_VK_PRESET_BUDGET_RAISES") && atoi(getenv("RX_VK_PRESET_BUDGET_RAISES"));
    return integrated || !raises ? std::min(b, own) : b;
}

uint64_t dead_sweep_next = 0;   // (the present of the next pass for the dead targets between texture_frame's looks; 0: none)
// ((Triforce) the texture budget by live VRAM use, tex_pressure.h: what texture_frame's look says to texture_sweep's pass)
struct Press {
    bool live = false;   // (this look's budget is the line's, not a size: a pass goes down to the budget itself)
    bool acting = false; // (the line acts: past it, or still past 2% under it since it was; tex_pressure::acts)
    bool held = false;   // (and the line stands aside: what it let go came back, tex_pressure::ComeBack)
    size_t step = 0;     // (live textures a pass over the budget lets go of at most; 0: no step)
    uint64_t next = 0;   // (the present of the next look while a pass stopped at its step; 0: the next tenth)
} press;
/// (Triforce) What the system may still commit and its limit: Windows charges a discrete card's memory to the commit too
/// (tex_pressure.h commit_low). False where it is not known (elsewhere device memory is not charged so)
bool commit_now(uint64_t* avail, uint64_t* limit) {
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (!GlobalMemoryStatusEx(&ms)) return false;
    *avail = ms.ullAvailPageFile, *limit = ms.ullTotalPageFile;
    return true;
#else
    (void)avail, (void)limit;
    return false;
#endif
}
void texture_sweep(uint64_t completed, bool sweep, uint64_t budget, uint64_t evict_to, bool known, uint64_t used, uint64_t allowed,
                   bool dead_only, uint64_t hold_to, uint64_t rule_to);
void texture_frame(uint64_t completed) {
    upload_frame_report();
    upload_earliest_report();   // ((Triforce, 11-17) its line every 600 frames)
    frame_upload_ns.store(0, std::memory_order_relaxed);
    frame_upload_bytes.store(0, std::memory_order_relaxed);
    frame_create_ns.store(0, std::memory_order_relaxed);   // (Triforce: create_later)
    frame_astc_admitted.store(0, std::memory_order_relaxed);
    if (frame_owed.exchange(0, std::memory_order_relaxed)) {
        // (Triforce: the hold's) what the draws of this frame resolved to the dummy or the placeholder in a texture's place goes
        // stale, as when textures go: the next frame looks those textures up again, and uploads them. (Triforce: only those,
        // texture_dummy_generation)
        if (dummy_gen_on()) dummy_gen++;
        else destroy_gen++;
    }
    if (const int64_t d = st_new_later_frame.exchange(0, std::memory_order_relaxed)) {   // (Triforce: new_texture_later)
        static int told;
        if (told++ < 40 || st_new_later.load(std::memory_order_relaxed) % 2000 < d)
            logf("vk: %lld new textures left for the next frame, the dummy drawn in their place and the frame held (the frame's "
                 "uploads took their budget; %lld so far; RX_VK_HOLD_MS=0: none)", (long long)d,
                 (long long)st_new_later.load(std::memory_order_relaxed));
    }
    if (const int64_t d = st_again_later_frame.exchange(0, std::memory_order_relaxed)) {   // (Triforce: upload_again_later)
        static int told;
        if (told++ < 40 || st_again_later.load(std::memory_order_relaxed) % 2000 < d)
            logf("vk: %lld uploads of textures held before left for the next frame, their last bytes drawn meanwhile (the frame's "
                 "uploads took their budget; %lld so far; RX_VK_UPLOAD_AGAIN_MS=0: none)", (long long)d,
                 (long long)st_again_later.load(std::memory_order_relaxed));
    }
    {   // ((Triforce, wp18-flash) torn_later: the re-uploads whose storage the CPU rewrote since its capture, left or not)
        const int64_t found = st_torn_found_frame.exchange(0, std::memory_order_relaxed);
        const int64_t left = st_torn_later_frame.exchange(0, std::memory_order_relaxed);
        static int told;
        if (found && (told++ < 40 || st_torn_found.load(std::memory_order_relaxed) % 2000 < found))
            logf("vk: %lld re-uploads of storage the CPU rewrote since its capture, %lld of them left for the next frame, their "
                 "last bytes drawn meanwhile (%lld and %lld so far; RX_VK_UPLOAD_TORN=0: none left)", (long long)found,
                 (long long)left, (long long)st_torn_found.load(std::memory_order_relaxed),
                 (long long)st_torn_later.load(std::memory_order_relaxed));
    }
    if (const int64_t d = st_deferred_frame.exchange(0, std::memory_order_relaxed)) {
        // (the views and descriptor sets resolved to the placeholder this frame go stale, as when textures go: the next frame
        // looks those textures up again, and uploads them. (Triforce: only those, texture_dummy_generation)
        if (dummy_gen_on()) dummy_gen++;
        else destroy_gen++;
        static int told;
        if (told++ < 40 || st_deferred.load(std::memory_order_relaxed) % 2000 < d)
            logf("vk: %lld new ASTC textures left for a later frame (this frame's uploads took their budget; %lld so far)", (long long)d,
                 (long long)st_deferred.load(std::memory_order_relaxed));
    }
    completed_value = completed;
    frame_no++;
    {   // (Triforce) The presents a second, for texture_sweep's waits (present_rate.h): they count presents, set at 60 a second,
        // and below 60 an age counts for what it is at 60, so that a card short of memory, the one that runs slow, does not
        // hold its textures twice and four times as long. RX_VK_TEX_RATE_LOW=0: presents below 60, as before. ((Triforce, after
        // NX2navi's M19) and above 60 too, up to 480 a second: "60 presents unused" was 0.64 s at the Market's 94.
        // RX_VK_TEX_RATE_HIGH=0: presents above 60, as before)
        static const bool low = !(getenv("RX_VK_TEX_RATE_LOW") && !atoi(getenv("RX_VK_TEX_RATE_LOW")));
        static const bool high = !(getenv("RX_VK_TEX_RATE_HIGH") && !atoi(getenv("RX_VK_TEX_RATE_HIGH")));
        const uint32_t was = tex_rate.rate;
        const uint64_t now_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
        // (said at each crossing of 56 a second, 16 times a run: a limit of 60 measures 59 now and then)
        if (tex_rate.tick(frame_no, now_ns, low, high)) {
            static int said;
            if ((tex_rate.rate < 240) != (was < 240) && said++ < 16) {
                if (tex_rate.rate < 240)
                    logf("vk: the game shows %u frames a second, fewer than 60, for %d seconds: the textures' waits count their "
                         "seconds in these (60 presents unused are %llu now; RX_VK_TEX_RATE_LOW=0: presents below 60)",
                         tex_rate.rate * 60 >> 8, present_rate::kSeconds, (unsigned long long)(60ull * tex_rate.rate >> 8));
                else
                    logf("vk: the game shows 60 frames a second or more again: the textures' waits count %s",
                         high ? "their seconds, at up to 480 presents a second (RX_VK_TEX_RATE_HIGH=0: presents above 60)" : "presents");
            }
            static bool said_high;   // (and once a run, the first time it is above 68 a second: the waits longer in presents)
            if (high && !said_high && tex_rate.rate > 290) {
                said_high = true;
                logf("vk: the game shows %u frames a second: the textures' waits count their seconds, not presents (60 presents "
                     "unused are %llu now; RX_VK_TEX_RATE_HIGH=0: presents above 60)", tex_rate.rate * 60 >> 8,
                     (unsigned long long)(60ull * tex_rate.rate >> 8));
            }
        }
    }
    {   // (staging arenas the GPU is done with, beyond 256 MB of them, and any over 64 MB: freed. A scene change stages
        // hundreds of MB to over a GB (the course maps, the terrain's ASTC arrays), and that host memory was kept)
        // (hookshot: 64 on a card of 6 GB or less. On one with resizable BAR the arenas are VRAM: VMA puts a host-visible buffer the GPU
        // reads (STORAGE: the ASTC decoder) in DEVICE_LOCAL | HOST_VISIBLE. The RTX 4050 Laptop held 4 idle ones, 256 MB of 5.7 GB, through
        // all of Kokiri gameplay, which stages ~nothing. A scene change makes the ones it needs again. RX_VK_STAGING_KEEP_MB=256: as before)
        static const uint64_t keep = []() -> uint64_t {
            if (const char* e = getenv("RX_VK_STAGING_KEEP_MB")) return (uint64_t)atoi(e) << 20;
            uint64_t used = 0, allowed = 0;
            return (vram_usage(&used, &allowed) && allowed <= (6ull << 30) + (512ull << 20) ? 64ull : 256ull) << 20;
        }();
        uint64_t kept = 0;
        for (auto it = arenas.begin(); it != arenas.end();) {
            const bool idle = it->value <= completed && it->value != rec_value;
            // (and one nothing has staged into for 30 s: Kokiri Forest's gameplay stages next to nothing, and the 64 MB it kept are VRAM
            // behind the BAR. A load makes one again. RX_VK_STAGING_IDLE_S=<n>: that long, 0: kept)
            static const int64_t idle_s = getenv("RX_VK_STAGING_IDLE_S") ? atoll(getenv("RX_VK_STAGING_IDLE_S")) : 30;
            const bool stale = idle_s && std::chrono::steady_clock::now() - it->used_at > std::chrono::seconds(idle_s);
            if (idle && (it->size > kArena || kept + it->size > keep || stale)) {
                destroy_buffer(it->buffer, it->allocation);
                it = arenas.erase(it);
                continue;
            }
            kept += it->size;
            ++it;
        }
        if (frame_no % 600 == 0) {   // (the process's memory now and then: what the host side holds)
            PROCESS_MEMORY_COUNTERS_EX pm{};
            pm.cb = sizeof pm;
            if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm))
                logf("vk: memory: working set %llu MB (peak %llu), private %llu MB, staging %llu MB in %zu arenas",
                     (unsigned long long)(pm.WorkingSetSize >> 20), (unsigned long long)(pm.PeakWorkingSetSize >> 20),
                     (unsigned long long)(pm.PrivateUsage >> 20), (unsigned long long)(kept >> 20), arenas.size());
#if defined(__linux__) && defined(__GLIBC__)
            // (glibc's heap: what its arenas hold free. The shader translation and the loads' decoding leave their garbage spread through
            // the threads' arenas (26 of them, 1.2 GB resident in Kokiri Forest, the heap's growth in a scene change never going back),
            // and a free chunk's pages are the process's till malloc_trim gives them to the system. Asked on a worker, when 192 MB or
            // more are free: RX_VK_MALLOC_TRIM_MB=<n>: that many, 0: never; the "vk: malloc:" line says what the arenas hold)
            // (every 1800 presents: mallinfo2 walks every free chunk of every arena with its lock held)
            static std::atomic<bool> busy{false};
            if (frame_no % 1800 == 0 && !busy.exchange(true))
                jobs_push([] {
                    static const uint64_t trim_mb = getenv("RX_VK_MALLOC_TRIM_MB") ? (uint64_t)atoll(getenv("RX_VK_MALLOC_TRIM_MB")) : 192;
                    const struct mallinfo2 a = mallinfo2();
                    PROCESS_MEMORY_COUNTERS_EX pm0{};
                    pm0.cb = sizeof pm0;
                    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm0, sizeof pm0);
                    logf("vk: malloc: %llu MB in arenas (%llu MB in use, %llu MB free, %llu MB of it at the tops), %llu MB mapped",
                         (unsigned long long)(a.arena >> 20), (unsigned long long)(a.uordblks >> 20), (unsigned long long)(a.fordblks >> 20),
                         (unsigned long long)(a.keepcost >> 20), (unsigned long long)(a.hblkhd >> 20));
                    if (trim_mb && (a.fordblks >> 20) >= trim_mb) {
                        const auto t0 = std::chrono::steady_clock::now();
                        malloc_trim(0);
                        PROCESS_MEMORY_COUNTERS_EX pm1{};
                        pm1.cb = sizeof pm1;
                        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm1, sizeof pm1);
                        logf("vk: malloc_trim: %llu MB free in the arenas now, the process's private memory %llu -> %llu MB (%.0f ms)",
                             (unsigned long long)(mallinfo2().fordblks >> 20), (unsigned long long)(pm0.PrivateUsage >> 20),
                             (unsigned long long)(pm1.PrivateUsage >> 20),
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                    }
                    busy = false;
                });
#endif
        }
    }
    bc_scratch_idle();
    bake_writeback_step(completed);   // (Triforce: what the GPU baked, into the game's memory too)
    bake_reupload_step(completed);    // (lab: RX_VK_BAKE_REUPLOAD)
    compact_step(completed);   // (hookshot, RX_VK_COMPACT: while its pass is open nothing of the textures' pool is freed)
    for (auto it = dead.begin(); it != dead.end();) {
        if (compact.in_pass) break;   // (hookshot, RX_VK_COMPACT: nothing is freed while a pass is open; a frame or two)
        if (it->value > completed) {
            ++it;
            continue;
        }
        if (it->pool_key) {   // (a render target's image: kept for the next of its shape, above)
            pool_put(*it);
            it = dead.erase(it);
            continue;
        }
        for (VkImageView v : it->views) vkDestroyImageView(dev.device, v, nullptr);
        destroy_image(it->image, it->allocation);
        if (it->buffer) destroy_buffer(it->buffer, it->buffer_alloc);
        for (VkDeviceMemory m : it->memory) vkFreeMemory(dev.device, m, nullptr);
        it = dead.erase(it);
    }
    pool_trim();
    // A budget: RX_VK_VRAM_MB of textures (default 4096), and less when the driver says the process is near what it
    // may have (other programs use the GPU too). The least recently used go first, down to 7/8 of it; nothing the
    // last 8 presents used, nothing recorded work may still read.
    static uint64_t own = 0;
    static bool last_known;   // (the driver's numbers of the last full look, for the passes between)
    static uint64_t last_used, last_allowed;
    {   // (and a present in which a target's image was made new, not taken from the pool, asks for a pass three presents on,
        // when what it replaced can first be found dead (RX_VK_DEAD_MIN_UNUSED): by the passes' own findings alone the first
        // one of a run of new targets came at the next look, and in those ten presents the targets' blocks went 11 -> 22)
        static uint64_t made_seen;
        static const bool dead_quick = !(getenv("RX_VK_DEAD_QUICK") && !atoi(getenv("RX_VK_DEAD_QUICK")));
        if (st_pool_made != made_seen) {
            made_seen = st_pool_made;
            if (dead_quick && (!dead_sweep_next || dead_sweep_next > frame_no + 3)) dead_sweep_next = frame_no + 3;
        }
    }
    // ((Triforce) and a look two presents on while the live line's pass stopped at its step: press, tex_pressure.h)
    if (frame_no % 10 && !(press.next && frame_no >= press.next)) {
        // (Triforce, 11-11) between the looks, a pass for the dead transient targets alone while the game makes them fast
        // (dead_sweep_next, set by texture_sweep): see there
        if (dead_sweep_next && frame_no >= dead_sweep_next)
            texture_sweep(completed, true, ~0ull, 0, last_known, last_used, last_allowed, true, 0, UINT64_MAX);   // (no hold in this pass)
        return;
    }
    press.next = 0;
    vma_blocks_ahead_check();   // (Triforce: the blocks made ahead go back while VRAM is short, vk_vma.cpp)
    uint64_t used = 0, allowed = 0, block_free = 0;
    const bool known = vram_usage(&used, &allowed, &block_free);
    last_known = known, last_used = used, last_allowed = allowed;
    if (!own)   // (RX_VK_VRAM_MB, else 70% of what the driver allows the process at the first look)
        own = getenv("RX_VK_VRAM_MB") ? (uint64_t)atoi(getenv("RX_VK_VRAM_MB")) << 20
              : known                ? tex_pressure::fixed(allowed)   // (45% at 8.5 GB or less, at most 6 GB)
                                     : 4096ull << 20;
    // (hookshot: at most 6 GB: a 16 GB card's 10.8 GB filled up, read as a leak: 105, 106. On a card of 8 GB or less, 45% of what the
    // driver allows, not 70%: the game's other VRAM (render targets, the pools' mirrors and imports, buffers, pipelines) is ~3.5-3.8 GB
    // in Hyrule Field, so 70% (5.1 GB of an 8 GB card's 7.3) put the whole past what the card holds. The driver paged VRAM to system
    // memory and the GPU time went from ~13 to 55 ms a frame for good: an RTX 3060 Ti at 14 fps ten seconds into play, #1400's
    // RTX 4050 at 5 fps. RX_VK_VRAM_MB=<MB>: the old size, or any)
    {   // (hookshot) an integrated GPU's "VRAM" is the system's RAM, which the game's own ~10 GB also needs: 70% of what the driver allows
        // was 5.3 GB of textures on a 16 GB laptop with a Radeon 780M, and the PC ran out of memory in Hyrule Field within minutes
        // (Discord, 2026-11-09). At most 2.5 GB there unless RX_VK_VRAM_MB says otherwise
        static bool told;
        if (!getenv("RX_VK_VRAM_MB") && dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && own > (2560ull << 20)) {
            own = 2560ull << 20;
            if (!told) told = true, logf("vk: an integrated GPU shares the system's RAM: textures at most 2560 MB (RX_VK_VRAM_MB=<MB>: other)");
        }
    }
    // (a smaller preset on a bigger card: the card's own; RX_VK_VRAM_MB and an integrated GPU's cap: preset_texture_budget)
    const bool vram_env = getenv("RX_VK_VRAM_MB") != nullptr;
    const bool preset_budget = opts.vram_mb > 0 && !vram_env && preset_caps_memory();
    uint64_t budget = preset_texture_budget((uint64_t)std::max(opts.vram_mb, 0), preset_budget, vram_env,
                                            dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, own);
    if (preset_budget && known) {
        // Safety clamp: do not exceed 75% of driver-allowed memory on constrained VRAM (prevents VK_ERROR_DEVICE_LOST)
        budget = std::min(budget, (allowed / 4) * 3);
    }
    // ((Triforce, after NX2navi's TEX_VRAM_PRESSURE: tex_pressure.h) On a card the driver allows more than 8.5 GB the budget
    // follows the live use, not a size: textures go once the use less the free space the textures can take back is past 80%
    // of what the driver allows now, 64 a pass, down to 2% under that line. The fixed 6 GB was what a 32 GB card's textures ran
    // into: seven cuts of 134-645 textures in the Market's first 25 s after a tour, each made again within seconds. Decided at
    // the first look that knows the allowance; RX_VK_VRAM_MB, an integrated GPU, a card of 8.5 GB or less, one without
    // VK_EXT_memory_budget keep the size; so does a quality preset whose budget applies. RX_VK_TEX_PRESSURE=0: the fixed size,
    // as before; =<pct>: that line (50 to 95))
    static int live_card = -1;
    static uint64_t press_pct = 0;
    static const char* const step_env = getenv("RX_VK_TEX_STEP");   // (=<n>: that many a pass, on any card; 0: no step)
    if (live_card < 0 && known) {
        const bool integrated = dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        press_pct = tex_pressure::pct_of(getenv("RX_VK_TEX_PRESSURE"));
        live_card = tex_pressure::live(press_pct, dev.memory_budget, allowed, integrated, vram_env);
        if (live_card)
            logf("vk: texture budget by live VRAM use: textures go once the use, less the free space in the blocks they can take "
                 "back, is past %llu%% of what the driver allows (%llu of %llu MB now), the least recently used first, unused 2 s "
                 "or more, %zu a pass, down to 2%% under it; no fixed size (RX_VK_TEX_PRESSURE=0: the fixed %llu MB, as before)",
                 (unsigned long long)press_pct, (unsigned long long)(tex_pressure::line(allowed, press_pct) >> 20),
                 (unsigned long long)(allowed >> 20), tex_pressure::step_of(step_env, true), (unsigned long long)(own >> 20));
        else
            logf("vk: texture budget: a fixed %llu MB (%s)", (unsigned long long)(own >> 20),
                 !press_pct ? "RX_VK_TEX_PRESSURE=0" : vram_env ? "RX_VK_VRAM_MB" : integrated ? "an integrated GPU"
                 : !dev.memory_budget ? "the driver gives no VK_EXT_memory_budget" : "8.5 GB allowed or less: the rules for such cards");
    }
    press.live = live_card > 0 && !preset_budget && known;
    press.step = tex_pressure::step_of(step_env, press.live);
    // (and the fixed size while the system's commit is low: device memory is charged to it, and an allocation that fails there
    // ends the game. RX_VK_TEX_COMMIT_FREE_MB=<MB>: low under that much free, 0: never; NX2navi's default: 4 GB or a tenth)
    bool commit_low = false;
    if (press.live) {
        static const int64_t want_mb = getenv("RX_VK_TEX_COMMIT_FREE_MB") ? atoll(getenv("RX_VK_TEX_COMMIT_FREE_MB")) : -1;
        uint64_t avail = 0, limit = 0;
        commit_low = want_mb && commit_now(&avail, &limit) && tex_pressure::commit_low(avail, limit, want_mb);
        static bool was_low;
        static int told;
        if (commit_low != was_low && told++ < 16)
            logf("vk: the system's commit %s (%llu of %llu MB free): textures %s (RX_VK_TEX_COMMIT_FREE_MB=0: never)",
                 commit_low ? "is low" : "is no longer low", (unsigned long long)(avail >> 20), (unsigned long long)(limit >> 20),
                 commit_low ? "held to the fixed size meanwhile, a few a pass" : "by live VRAM use again");
        was_low = commit_low;
    }
    // ((Triforce, after NX2navi's) and the line stands aside while half or more of what it let go is made again within 600
    // presents: tex_pressure.h ComeBack. The 90% and 97% rules and the compaction's lines still hold meanwhile)
    press.held = false;
    if (press.live && come_back_on()) {
        bool began = false;
        uint64_t hold = 0, drops = 0, backs = 0;
        {
            std::lock_guard lk(come_back_mu);
            press.held = come_back.held(frame_no, tex_rate.rate, &began);
            hold = come_back.hold, drops = come_back.last_drops, backs = come_back.last_backs;
        }
        static int told;
        if (began && told++ < 12)
            logf("vk: of the %llu textures the live VRAM line let go in the last %llu presents, %llu were made again within %llu: "
                 "it stands aside for %llu presents at 60 a second (the 90%% and 97%% rules still hold; RX_VK_TEX_COME_BACK=0: never)",
                 (unsigned long long)drops, (unsigned long long)tex_pressure::ComeBack::kPeriod, (unsigned long long)backs,
                 (unsigned long long)tex_pressure::ComeBack::kWithin, (unsigned long long)hold);
    }
    // (near what the driver allows, the budget shrinks by the excess. The excess of what the process holds less the
    // free space in VMA's blocks, which new images take before VMA makes a block: measured by the blocks alone, the
    // textures evicted freed space inside them that the driver still counted, so 10 frames later the budget shrank
    // again. A race kept ~3 GB free in the blocks and evicted and made again ~100 textures every 10 frames (1300 at
    // once), mostly the same ones: their uploads and decodes, and images made empty in the frame that drew them.
    // The driver's own count near its limit (97%) still squeezes, whatever the blocks hold free: then it pages VRAM
    // out. RX_VK_BUDGET_BY_BLOCKS=1: as before)
    static const bool by_blocks = getenv("RX_VK_BUDGET_BY_BLOCKS") != nullptr;
    uint64_t evict_to = 0;   // (hookshot, RX_VK_COMPACT: what an eviction goes down to when the compaction's hold set the budget)
    uint64_t hold_to = 0;    // (and what the hold's own line asks for, when the reserve's lower line set evict_to)
    // (hookshot, RX_VK_COMPACT) compacted, only the textures' pool's free space comes off the count: vram_compact.h
    uint64_t reclaim = block_free;
    if (compact_on()) {
        uint64_t pool_blocks = 0, pool_free = 0;
        if (textures_pool_usage(&pool_blocks, &pool_free)) reclaim = vram_compact::reclaimable(block_free, pool_free);
    }
    const uint64_t pressure = by_blocks ? used : used - std::min(used, reclaim);
    pool_pressed = known && pressure > allowed / 10 * 9;   // (the target pool given back meanwhile)
    compact.pressed = known && vram_compact::pressed(used, allowed);   // (hookshot, RX_VK_COMPACT: by the driver's own count)
    // ((Triforce) the live line in the size's place (above, tex_pressure.h): none while under it, and once past it the textures
    // less the excess over 2% under it, at each look until the use is there; the fixed size while the commit is low. Before the
    // 90% rule, which then asks for less)
    press.acting = press.live && !press.held && tex_pressure::acts(pressure, allowed, press_pct, press.acting);
    if (press.live) budget = tex_pressure::budget(own, total_bytes, pressure, allowed, press_pct, commit_low, press.acting);
    if (known && pressure > allowed / 10 * 9) {
        const uint64_t over = pressure - allowed / 10 * 9;
        budget = std::min(budget, total_bytes > over ? total_bytes - over : 0);
    }
    // (hookshot, RX_VK_COMPACT: what the budget's size and the 90% rule ask on their own, before the hold's lines; the 97%
    // rule's joins it below. The hold's pass lets a texture unused for the eviction's own time go for that part, as a pass
    // does with the compaction off: vram_compact::hold_target)
    uint64_t rule_budget = budget;
    // (hookshot, RX_VK_COMPACT) and the count is held at RX_VK_COMPACT_PCT% of the allowance (85; 50 to 90), as it will be once a
    // run has given the textures' free space back: within ~0.8 GB of what the driver allows the GPU's frame went from ~10 ms
    // to 13-20 ms with the shadows in place, the driver itself short. At 85 a five-minute walk across Hyrule Field at DLAA
    // 2560 x 1440 had 11-12 frames over 33 ms against 34-49 uncompacted, and a 1% low of 37 fps against 26-29
    if (compact_on() && known && !by_blocks) {
        static const uint64_t pct = getenv("RX_VK_COMPACT_PCT") ? (uint64_t)std::clamp(atoi(getenv("RX_VK_COMPACT_PCT")), 50, 90) : 85;
        const uint64_t held = vram_compact::held_budget(budget, total_bytes, used, reclaim, allowed, pct);
        // (and what goes is the excess alone. Over its budget the cache is cut to 7/8 of it, below: right for a budget that
        // is a size, but this one is "what the textures hold less the excess", crossed again whenever anything else grows.
        // A recorder started beside the game took 0.8 GB of the allowance as play came back from 14 s in the menu: the line
        // was crossed on five passes in a row and the cache went from 4,589 to 2,312 MB (0.875^5) in two seconds, the
        // textures made again at 65-90 ms a frame)
        if (held < budget) budget = evict_to = held;
        hold_to = evict_to;
        // (and a reserve under that line, for the textures long unused: vram_compact.h. The pass below takes the others only
        // for the hold's own line)
        const uint64_t reserve = vram_compact::held_budget(budget, total_bytes, used, reclaim, allowed, vram_compact::reserve_pct(pct));
        if (reserve < budget) budget = evict_to = reserve;
    }
    // (hookshot) not while VMA's blocks hold an eighth of the allowance free: new images take that space, the driver's count does
    // not grow, and what this evicts frees space inside the blocks that the count keeps. Ten frames later it squeezed again: a
    // ratchet. On a 4 GB RX 5500M (3337 MB used of 3337, 1224 MB free in the blocks) a paused game, whose scene draws nothing,
    // lost 1700 MB of textures in seconds, made again on unpausing (the long wait), and the HUD's drawn textures went with them.
    // RX_VK_SQUEEZE_ALWAYS=1: as before
    static const bool squeeze_always = getenv("RX_VK_SQUEEZE_ALWAYS") != nullptr;
    const bool room_in_blocks = !squeeze_always && block_free >= allowed / 8;
    if (known && !by_blocks && used > allowed / 100 * 97 && !room_in_blocks) {
        const uint64_t over = used - allowed / 100 * 97;
        budget = std::min(budget, total_bytes > over ? total_bytes - over : 0);
        rule_budget = std::min(rule_budget, total_bytes > over ? total_bytes - over : 0);
    }
    const uint64_t rule_to = vram_compact::rule_target(total_bytes, rule_budget);
    // (hookshot, RX_VK_COMPACT: the driver's limit asks for more than the reserve does: a pass over the budget, to 7/8 of it, as
    // before. Rare once the reserve is set: its line is 17 points under the limit's and counts the textures' free space out, so
    // this takes more than that share of the allowance free in their pool. Otherwise the pass is the hold's, and what the
    // limit and the 90% rule ask for goes by rule_to)
    if (budget < evict_to) evict_to = hold_to = 0;
    if (frame_no % 600 == 0) {
        memory_trial_tick();   // (Triforce: the memory handling on trial on this card, vk_memory.cpp)
        vma_log_heaps();
        vma_log_image_blocks();
        {
            std::lock_guard lk(pool_mu);
            logf("vk: render target images: %llu reused from the pool, %llu made new, %llu destroyed from it; it holds %llu "
                 "(%llu MB)%s", (unsigned long long)st_pool_reused, (unsigned long long)st_pool_made,
                 (unsigned long long)st_pool_destroyed, (unsigned long long)pool_n, (unsigned long long)(pool_bytes >> 20),
                 pool_cap() ? (pool_pressed ? ", VRAM short: given back" : "") : " (RX_VK_TARGET_POOL_MB=0)");
        }
        // ((Triforce) by live use: the budget the line sets, or while it sets none what the textures may hold before they reach
        // it (tex_pressure::shown), and the line; the lab's scenario_report reads the line up to the textures' count)
        char live_note[160] = "";
        if (press.live)
            snprintf(live_note, sizeof live_note, ", by live use: past %llu MB, %llu%% of what the driver allows%s",
                     (unsigned long long)(tex_pressure::line(allowed, press_pct) >> 20), (unsigned long long)press_pct,
                     commit_low ? " (the system's commit low: the fixed size)"
                     : press.held ? " (standing aside: what it let go came back)" : "");
        logf("vk: VRAM: %llu MB used of %llu MB allowed (%llu MB of it free in the blocks); textures %llu MB (budget %llu "
             "MB), %zu textures%s%s", (unsigned long long)(used >> 20), (unsigned long long)(allowed >> 20),
             (unsigned long long)(block_free >> 20), (unsigned long long)(total_bytes >> 20),
             (unsigned long long)((press.live ? tex_pressure::shown(budget, total_bytes, pressure, allowed, press_pct) : budget) >> 20),
             textures.size(), live_note,
             known && used > allowed / 100 * 97 && room_in_blocks ? " (at the driver's limit, room in the blocks: not squeezed)" : "");
        // (what takes it: decoded ASTC, the GPU's images, the rest; the largest)
        uint64_t astc_b = 0, rt_b = 0, other_b = 0, recent_b = 0, baked_b = 0;
        size_t baked_n = 0;
        std::vector<const Texture*> big, baked;
        size_t bucket_n[4]{}, bucket_rel[4]{};
        uint64_t bucket_b[4]{};
        for (auto& kv : textures) {
            const Texture* t = kv.second.get();
            (t->astc ? astc_b : t->gpu_written ? rt_b : other_b) += t->bytes;
            if (frame_no - t->last_frame < 8) recent_b += t->bytes;
            if (t->gpu_written && t->first_written && t->first_written == t->last_written && t->bytes <= (4ull << 20)) {
                if (!t->released) baked_n++, baked_b += t->bytes;
                const int k = t->bytes <= (64u << 10) ? 0 : t->bytes <= (256u << 10) ? 1 : t->bytes <= (1u << 20) ? 2 : 3;
                bucket_n[k]++, bucket_b[k] += t->bytes, bucket_rel[k] += t->released;
                baked.push_back(t);
            }
            big.push_back(t);
        }
        if (getenv("RX_VK_LOG_BAKED")) {   // (what the textures drawn in one frame are: by size, finalized or not)
            logf("vk: drawn in one frame: <=64 KB %zu (%llu KB, %zu finalized), <=256 KB %zu (%llu KB, %zu), <=1 MB %zu "
                 "(%llu KB, %zu), <=4 MB %zu (%llu KB, %zu)", bucket_n[0], (unsigned long long)(bucket_b[0] >> 10),
                 bucket_rel[0], bucket_n[1], (unsigned long long)(bucket_b[1] >> 10), bucket_rel[1], bucket_n[2],
                 (unsigned long long)(bucket_b[2] >> 10), bucket_rel[2], bucket_n[3], (unsigned long long)(bucket_b[3] >> 10),
                 bucket_rel[3]);
            std::sort(baked.begin(), baked.end(), [](const Texture* a, const Texture* b) { return a->bytes > b->bytes; });
            for (size_t i = 0; i < baked.size() && i < 10; i++) {
                const Texture* t = baked[i];
                logf("vk:   one frame: #%u %#llx %ux%ux%u x%u levels %u NVN %#x %llu KB, written at %llu, made at %llu, last "
                     "used %llu%s%s", t->id, (unsigned long long)t->gpu, t->width, t->height, t->depth, t->layers, t->levels,
                     t->nvn_format, (unsigned long long)(t->bytes >> 10), (unsigned long long)t->first_written,
                     (unsigned long long)t->first_seen, (unsigned long long)t->last_frame, t->released ? " finalized" : "",
                     t->first_render_frame ? " rendered" : " copied or stored");
            }
        }
        std::sort(big.begin(), big.end(), [](const Texture* a, const Texture* b) { return a->bytes > b->bytes; });
        logf("vk: textures: decoded ASTC %llu MB, rendered %llu MB (%zu drawn in one frame, kept: %llu KB), other %llu MB; "
             "used in the last 8 presents %llu MB", (unsigned long long)(astc_b >> 20), (unsigned long long)(rt_b >> 20),
             baked_n, (unsigned long long)(baked_b >> 10), (unsigned long long)(other_b >> 20),
             (unsigned long long)(recent_b >> 20));
        for (size_t i = 0; i < big.size() && i < 8; i++)
            logf("vk:   %llu MB: %#llx %ux%ux%u x%u levels %u NVN %#x%s%s, last used %llu presents ago",
                 (unsigned long long)(big[i]->bytes >> 20), (unsigned long long)big[i]->gpu, big[i]->width, big[i]->height,
                 big[i]->depth, big[i]->layers, big[i]->levels, big[i]->nvn_format, big[i]->astc ? " ASTC" : "",
                 big[i]->gpu_written ? " rendered" : "", (unsigned long long)(frame_no - big[i]->last_frame));
    }
    if (verify_dim().first && frame_no % 300 == 0) {
        int checked = 0, changed = 0, missed = 0;
        for (auto& kv : textures) {
            Texture* t = kv.second.get();
            if (t->width != verify_dim().first || t->height != verify_dim().second || !t->verify_hash || t->gpu_written) continue;
            checked++;
            const uint64_t h = guest_hash(*t);
            if (h == t->verify_hash) continue;
            changed++;
            static thread_local Ranges d;
            if (!texture_dirty(*t, t->upload_gen, d)) {
                missed++;
                static int logged;
                if (logged++ < 40) {
                    logf("vk: MISSED WRITE: texture #%u %#llx %ux%u NVN %#x: its memory changed since its upload, the write "
                         "tracking saw nothing", t->id, (unsigned long long)t->gpu, t->width, t->height, t->nvn_format);
                    verify_report(*t);
                }
            }
        }
        logf("vk: verify %ux%u: %d textures, %d changed since uploaded, %d of them unnoticed", verify_dim().first,
             verify_dim().second, checked, changed, missed);
    }
    warm_known = known, warm_used = used, warm_allowed = allowed, warm_budget = budget;   // ((Triforce) tex_warm.h: the room there is)
    texture_sweep(completed, total_bytes <= budget, budget, evict_to, known, used, allowed, false, hold_to, rule_to);
}

/// (texture_frame) What goes: over the budget the least recently used, and the render targets drawn over since.
/// (Triforce, 11-11) dead_only: a pass between texture_frame's looks (every 10 presents) for the dead transient targets alone.
/// While the camera moves the game makes them at new sizes all the time: 1.2-3 new images a present running across Hyrule
/// Field in the lab, up to 11 a present between areas in a player's session (6733 in 600 presents). A dead one waited for the
/// next look, 10 presents plus the 3 it must go unused: at ~20 MB each that is ~650 MB of dead images in the lab's run and
/// 2.6 GB in that session, on top of what is drawn, their pool's blocks 18 -> 31, and at the driver's limit each new block
/// took 110-170 ms (ten frames of 115-180 ms in a row). So when a pass finds them coming at one a present and more, the next
/// pass is the next present, at one in three presents three presents on, else the next look as before: the dead wait 4-6
/// presents, not 13. RX_VK_DEAD_QUICK=0: only at the looks, as before
void texture_sweep(uint64_t completed, bool sweep, uint64_t budget, uint64_t evict_to, bool known, uint64_t used, uint64_t allowed,
                   bool dead_only, uint64_t hold_to, uint64_t rule_to) {
    static const bool no_evict = getenv("RX_VK_NO_EVICT") != nullptr;   // (an experiment: nothing evicted)
    // (Triforce) An age in presents as what it is at 60 presents a second (present_rate.h): the waits below are the same seconds
    // at 30 as at 60, and what they hold on a card short of memory no more. At 60 and above an age is its presents. Not the
    // dead transient targets' wait (need_dead): that one counts the frames a target's storage needs to be seen drawn over
    const auto age60 = [](uint64_t presents) { return present_rate::at60(presents, tex_rate.rate); };
    // (hookshot) under the budget, every 600 presents: the render targets drawn over since (below), unused for 120
    // (hookshot-amd) and every 60 presents for the render targets drawn over by one at another start (below): OoT makes a frame's
    // transient targets (768 x 432 RGBA16F, 1536 x 864) in a scratch heap whose offsets shift with what came before them in the
    // frame, so turning the camera made new ones each frame ~1.2 MB from the last, each a new image. Left for the 600-present
    // sweep or the budget, ~200 dead ones (1.2 GB) filled an 8 GB card's VRAM in Hyrule Field, the driver paged it out and the
    // GPU's frame went from 15 to 35 ms. RX_VK_DEAD_TARGETS=0: as before
    static const bool dead_targets = !getenv("RX_VK_DEAD_TARGETS") || atoi(getenv("RX_VK_DEAD_TARGETS")) != 0;
    // (hookshot) every 10 presents (RX_VK_DEAD_EVERY; 60 before): the target pool gets the dead ones as the game makes new ones,
    // not 300-700 at once every second, of which the new ones of the next second missed most. The choice is cheap: 1930 textures
    // ordered in 0.3 ms, 251 chosen in 0.2 (RTX 5070, Ryzen 7 9800X3D)
    static const uint64_t dead_every =
        getenv("RX_VK_DEAD_EVERY") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_DEAD_EVERY"))) : 10;
    if (no_evict || (sweep && !dead_only && frame_no % (dead_targets ? dead_every : 600))) return;
    static const bool dead_quick = !(getenv("RX_VK_DEAD_QUICK") && !atoi(getenv("RX_VK_DEAD_QUICK")));
    static uint64_t dead_pass_last, quick_passes, quick_ns, quick_logged_at;
    const auto pass_t0 = std::chrono::steady_clock::now();
    const uint64_t pass_presents = std::max<uint64_t>(1, frame_no - dead_pass_last);
    dead_pass_last = frame_no;
    dead_sweep_next = 0;   // (this pass is the one asked for; the next: below, or a new image's, texture_frame)
    size_t shifted_now = 0;
    const size_t textures_before = textures.size();
    // ((Triforce) after NX2navi's texture table walked by its entries (0.8.0; rewritten, the tables differ) A pass under the
    // budget (sweep: every 10 presents, and the dead targets' passes between the looks, up to every present while the camera
    // moves) takes only a texture the GPU drew and something drew over since, or one the game let go (gone_early): the loop
    // below passes every other one over (`if (sweep && !overwritten && !gone_early) continue;`; drawn_over and
    // drawn_over_shifted are false for a texture the GPU never drew, gone_early for one the game did not let go), and so do
    // the finalized ones' sums (keep_finalized: drawn ones only) and the let-go mass (let go, not drawn). Only those are
    // ordered then: the drawn ones are about a third of the cache, and a pass ordered all of it (3,200 textures in the owner's
    // test18 session; "passes for the dead render targets between the looks: ... 0.17 ms each"). RX_VK_SWEEP_ALL=1: every
    // texture ordered for every pass, as before
    static const bool sweep_all = getenv("RX_VK_SWEEP_ALL") && atoi(getenv("RX_VK_SWEEP_ALL")) != 0;
    std::vector<std::pair<uint64_t, Texture*>> order;
    order.reserve(textures.size());
    for (auto& kv : textures)
        if (!sweep || sweep_all || kv.second->gpu_written || kv.second->let_go) order.push_back({kv.second->last_frame, kv.second.get()});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // (the rendered textures by where their storage starts, for drawn_over_shifted)
    std::vector<const Texture*> by_start;
    if (dead_targets) {
        for (auto& [last, tex] : order)
            if (tex->gpu_written && tex->first_render_frame && tex->guest_size) by_start.push_back(tex);
        std::sort(by_start.begin(), by_start.end(), [](const Texture* a, const Texture* b) { return a->gpu < b->gpu; });
    }
    uint64_t longest = 0;
    for (const Texture* t : by_start) longest = std::max(longest, t->guest_size);
    std::vector<Texture*> gone;
    // (over the budget, what goes has gone unused for RX_VK_EVICT_MIN_UNUSED frames, 60 by default; it was 8. On an
    // 8 GB laptop card the VRAM sits at the driver's limit, the budget shrinks and this runs every 10 frames: with 8,
    // ~300 textures in a few minutes of Kokiri Forest were evicted and made again, all of them within 60 frames (200
    // within 10), uploaded and decoded again each time; and a 260 x 260 one the GPU renders and the game samples every
    // 40 frames came back empty each time (22 times; the lit ground black for a frame, a tiled pattern over the ground).
    // With 60: 2. The textures that are really done with still go, 52 frames later)
    static const uint64_t min_unused =
        getenv("RX_VK_EVICT_MIN_UNUSED") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_EVICT_MIN_UNUSED"))) : 60;
    // (hookshot) a render target another image has since been drawn over (same storage, another size or format:
    // OoT's dynamic resolution makes a new set at each size in the same pool memory) holds nothing any more: it
    // goes like any other. Kept, they were 3.6 GB of 4.5 GB rendered, VRAM near the driver's limit, and the
    // driver's paging made submissions of seconds (5-9 fps for a minute)
    const auto drawn_over = [&](const Texture* tex) {
        if (!tex->gpu_written) return false;
        auto [a, e] = by_gpu.equal_range(tex->gpu);
        for (auto o = a; o != e; ++o)
            if (o->second != tex && o->second->last_written > tex->last_frame) return true;
        return false;
    };
    // (hookshot-amd) a render target of one level and layer whose storage another render target drew over from another start
    // after its last use (target_overlap.h): the transient targets above. Its texels are gone from that memory on the console too
    const auto drawn_over_shifted = [&](const Texture* tex) {
        if (!dead_targets || !tex->gpu_written || !tex->first_render_frame || !tex->guest_size || tex->levels != 1 ||
            tex->layers != 1 || tex->depth > 1)
            return false;
        const uint64_t lo = tex->gpu, hi = tex->gpu + tex->guest_size;
        auto it = std::lower_bound(by_start.begin(), by_start.end(), lo > longest ? lo - longest : 0,
                                   [](const Texture* t, uint64_t g) { return t->gpu < g; });
        for (; it != by_start.end() && (*it)->gpu < hi; ++it)
            if (*it != tex && target_overlap::shifted_over(lo, hi, tex->last_frame, (*it)->gpu, (*it)->gpu + (*it)->guest_size,
                                                           (*it)->last_written))
                return true;
        return false;
    };
    // (the transient targets: a frame's own, not sampled again, their storage drawn over by another since. 16 frames left
    // ~500 MB of them in Hyrule Castle Town's market at 20 fps (59 768 x 432 RGBA16F, 135 384 x 384, 41 1536 x 864 at a
    // time), VRAM at the driver's limit; the GPU's own use is checked below (last_value). RX_VK_DEAD_MIN_UNUSED=<frames>, 3)
    static const uint64_t dead_min_unused =
        getenv("RX_VK_DEAD_MIN_UNUSED") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_DEAD_MIN_UNUSED"))) : 3;
    // (hookshot) a small texture the GPU drew in one frame that the game has finalized, with nothing drawn over its storage
    // since: the game may make a texture over that storage again and sample it without drawing it. OoT's terrain does that with its
    // tiles' 260 x 260 RG8 normal maps: drawn when a chunk comes in, finalized when it goes out of range, made again and sampled as
    // they are when it comes back. On the console the texels are still in that memory; here they were only in the image, and one
    // evicted came back empty (made again over storage the GPU rendered, not uploaded): streaks over the ground in square patches
    // after a while of play (issues 107 and 386). Kept, up to RX_VK_KEEP_FINALIZED_MB (512 by default; 0:
    // evicted as before), the least recently used beyond that going as before. (Not written back into the memory instead: the game
    // may have put other things there since, and a write-back over scene data once crashed it)
    static const uint64_t keep_finalized_max =
        (uint64_t)(getenv("RX_VK_KEEP_FINALIZED_MB") ? std::max(0, atoi(getenv("RX_VK_KEEP_FINALIZED_MB"))) : 512) << 20;
    const auto keep_finalized = [&](const Texture* tex) {
        if (!keep_finalized_max || !tex->gpu_written || !tex->released || !tex->first_written ||
            tex->first_written != tex->last_written || tex->bytes > (4ull << 20))
            return false;
        const auto g = gpu_owned.find(tex->gpu);   // (its storage's range still the one the GPU drew it in)
        if (g == gpu_owned.end() || g->second.first != tex->gpu + std::max<uint64_t>(tex->guest_size, 1)) return false;
        return !drawn_over(tex) && !drawn_over_shifted(tex);
    };
    uint64_t finalized_kept = 0;
    size_t finalized_kept_n = 0;
    if (keep_finalized_max)
        for (auto& [last, tex] : order)
            if (keep_finalized(tex)) finalized_kept += tex->bytes, finalized_kept_n++;
    if (finalized_kept && frame_no % 600 == 0) {
        std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, std::pair<size_t, uint64_t>> kinds;   // (w, h, layers, NVN format)
        for (auto& [last, tex] : order)
            if (keep_finalized(tex)) {
                auto& k = kinds[{tex->width, tex->height, tex->layers, tex->nvn_format}];
                k.first++, k.second += tex->bytes;
            }
        char line[512];
        int n = 0;
        for (const auto& [k, v] : kinds)
            if (n < 440)
                n += snprintf(line + n, sizeof line - n, " %ux%ux%u NVN %#x: %zu (%llu KB);", std::get<0>(k), std::get<1>(k),
                              std::get<2>(k), std::get<3>(k), v.first, (unsigned long long)(v.second >> 10));
        logf("vk: finalized textures the GPU drew once, kept for being made again: %zu (%llu KB):%s", finalized_kept_n,
             (unsigned long long)(finalized_kept >> 10), n ? line : "");
    }
    // ((Triforce) by live use (press, tex_pressure.h) a pass over the budget too takes only what went unused for 120: two
    // seconds, NX2navi's STALE_FRAMES before an idle texture may go. A size has to be met now; the line has its margin)
    const uint64_t need_unused = sweep || press.live ? std::max<uint64_t>(120, min_unused) : min_unused;
    const uint64_t need_dead = dead_targets ? std::min(need_unused, dead_min_unused) : need_unused;
    // (Triforce: too young for anything of it to go. The dead targets' wait counts frames; with RX_VK_DEAD_TARGETS=0 it is the
    // eviction's own, which counts what an age is at 60 presents a second)
    const auto too_young = [&](uint64_t last) { return (dead_targets ? frame_no - last : age60(frame_no - last)) < need_dead; };
    static uint64_t n_dead, b_dead, dead_logged_at;
    // (hookshot) the textures to go are chosen first and destroyed after; the write-backs among them go to the GPU in a few
    // batches (flush_textures), not one submission and one wait each: a sweep of 500 at a scene load stalled for seconds
    struct Evict { Texture* tex; bool flush, shifted; };
    std::vector<Evict> evict;
    uint64_t pending_bytes = 0;   // (chosen below, their bytes not yet off total_bytes)
    size_t live_chosen = 0;       // ((Triforce) and of them the live ones, not render targets drawn over since: press.step)
    std::vector<uint64_t> went_keys;   // ((Triforce) and those the live line let go: the come-back pause)
    // (hookshot, RX_VK_COMPACT) a pass the compaction's hold asked for (evict_to) lets go of the dead render targets as a sweep
    // does, all of them, and of live textures only for the excess left after those, and only of ones long unused: vram_compact.h's
    // hold_met and hold_target. So it does not stop at the first texture past the excess: what the dead targets hold is counted first
    const bool by_hold = evict_to != 0 && !sweep;
    uint64_t hold_dead = 0, hold_live = 0;   // (what the dead targets hold; the live textures chosen so far)
    if (by_hold)
        for (auto& [last, tex] : order) {
            if (too_young(last)) break;
            if (tex->last_value > completed) continue;
            const bool shifted = drawn_over_shifted(tex);
            if (age60(frame_no - last) < need_unused && !shifted) continue;
            if (shifted || drawn_over(tex)) hold_dead += tex->bytes + tex->pad;
        }
    // (Triforce, 11-12) What the game has let go goes first while VRAM is short. At an area's arrival the textures of the area
    // left stayed until a rule asked for room, which was after the new area's had come in: those took new blocks, the old ones
    // then left holes in theirs, and for seconds the driver counted both. A lab arrival in Hyrule Field from Kakariko: VRAM
    // 10,993 to 11,249 MB of 10,468 to 10,663 allowed with 1.3 GB free inside the blocks, and each image made meanwhile took
    // 93 to 124 ms (three frames of 97 to 134 ms). A texture uploaded from memory whose texture the game finalized
    // RX_VK_LET_GO_AFTER presents ago (30) and that nothing has used since goes at the next pass while VRAM is over
    // RX_VK_LET_GO_PCT% of what the driver allows (75; 0: off, as before), whatever its age: its room is there when the next
    // area's textures come. One the game makes again is uploaded from memory again, as after any eviction.
    // Only when the game has let go of much at once, RX_VK_LET_GO_MB and more (256: an area left behind): at the Market the
    // game finalizes and makes again a few backdrop textures all the time, and with every one of those going at once the
    // uploads there went from 1.4-1.7 GB to 3.6 GB in 95 seconds. (Not "or whenever VRAM is over 92%" either: a card that
    // sits there for good, 5.6 GB allowed, had more frames over 40 ms with that than without, 559 against 369 in a tour.)
    // And only where the driver allows 9 GB and more, unless RX_VK_LET_GO_PCT is set: at 5.6 GB allowed three tours with this
    // and the blocks made ahead had 559-896 frames over 40 ms against 369-614 without (vk_vma.cpp)
    static const bool let_go_set = getenv("RX_VK_LET_GO_PCT") != nullptr;
    static const uint64_t let_go_pct = let_go_set ? (uint64_t)std::clamp(atoi(getenv("RX_VK_LET_GO_PCT")), 0, 100) : 75;
    static const uint64_t let_go_after = getenv("RX_VK_LET_GO_AFTER") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_LET_GO_AFTER"))) : 30;
    static const uint64_t let_go_mass_min = (uint64_t)(getenv("RX_VK_LET_GO_MB") ? std::max(0, atoi(getenv("RX_VK_LET_GO_MB"))) : 256) << 20;
    bool let_go_short = let_go_pct && known && used > allowed / 100 * let_go_pct && (let_go_set || allowed >= (9ull << 30));
    // ((Triforce, after NX2navi's TEX_KEEP_TAKEN) and one the game has taken back before waits 8 times the longest it stayed let
    // go before a take-back (back_gap, texture_touch), when that is longer: tex_pressure.h. RX_VK_TEX_KEEP_TAKEN=0: as before)
    static const bool keep_taken = !(getenv("RX_VK_TEX_KEEP_TAKEN") && !atoi(getenv("RX_VK_TEX_KEEP_TAKEN")));
    const auto let_go_due = [&](const Texture* tex) {
        return age60(frame_no - tex->let_go) >= tex_pressure::let_go_wait(let_go_after, age60(tex->back_gap), keep_taken);
    };
    if (let_go_short) {
        uint64_t mass = 0;
        for (auto& [last, tex] : order)
            if (tex->let_go && !tex->gpu_written && let_go_due(tex)) mass += tex->bytes + tex->pad;
        let_go_short = mass >= let_go_mass_min;
    }
    static uint64_t let_go_n, let_go_bytes, let_go_logged_at;
    for (auto& [last, tex] : order) {
        // (total_bytes can have drifted below the textures' sum (destroy_later clamps it): the chosen ones' bytes past it mean the
        // budget is met, not a wrapped difference that never is: tex_pressure::met)
        // ((Triforce) by live use down to the budget itself, its line's margin in it, not 7/8 of it: tex_pressure.h)
        if ((!sweep && !by_hold && tex_pressure::met(total_bytes, pending_bytes, budget, press.live)) || too_young(last))
            break;
        const bool shifted = drawn_over_shifted(tex);
        const bool gone_early = let_go_short && tex->let_go && !tex->gpu_written && let_go_due(tex);
        if (age60(frame_no - last) < need_unused && !shifted && !gone_early) continue;   // (younger: only a dead transient target goes)
        if (tex->last_value > completed) continue;
        // (nor what the GPU wrote and the game still has: the image is its only copy, and one made again over that
        // storage started empty or from stale memory: a sky or course texture drawn once, garbage from then on.
        // RX_VK_EVICT_RENDERED=1: as before)
        static const bool evict_rendered = getenv("RX_VK_EVICT_RENDERED") != nullptr;
        // (one unused for 3600 presents (a minute) goes: a course left behind; the game draws it again when it comes
        // back to that course. Without it a course change kept 1.4 GB arrays and VRAM ran over)
        // (600 presents near what the driver allows, at the budget's 97%: past it the driver pages VRAM to system memory.
        // AMD's driver reports the use at most at what it allows, so "over it" never held there: keep_rendered.h.
        // RX_VK_KEEP_RENDERED_OVER=1: only past what it allows, as before)
        static const bool keep_over_only = getenv("RX_VK_KEEP_RENDERED_OVER") != nullptr;
        const uint64_t keep_rendered = keep_rendered::presents(known, used, allowed, keep_over_only);
        const bool overwritten = shifted || drawn_over(tex);   // (above)
        // ((Triforce) a pass over the budget lets go of press.step live textures at most (64; by live use, tex_pressure.h), the
        // rest at the next look, two presents on: at the Market's arrival after a tour one pass took 134 to 645 at once, each made
        // again within seconds. Not the render targets drawn over since: nothing comes back for them)
        if (!sweep && !overwritten && tex_pressure::stepped(press.step, live_chosen)) continue;
        // (hookshot, RX_VK_COMPACT: the hold's pass, above. A live texture stays once the excess over its line is covered: the
        // reserve's line for one long unused, the hold's own for one unused ten seconds, and what the other rules ask (the 90%
        // rule, the budget's size, the driver's limit) for one unused as long as an eviction asks with the compaction off)
        if (by_hold && !overwritten && !gone_early) {
            const uint64_t to = vram_compact::hold_target(age60(frame_no - last), need_unused, rule_to, hold_to, evict_to);
            if (to == UINT64_MAX || vram_compact::hold_met(total_bytes, hold_live, hold_dead, to)) continue;
        }
        if (finalized_kept && keep_finalized(tex)) {   // (above. Oldest first: beyond the cap the least recently used go)
            if (finalized_kept <= keep_finalized_max) continue;
            finalized_kept -= tex->bytes;
            static int logged;
            if (logged++ < 100)
                logf("vk: kept finalized textures over %llu MB: #%u %#llx %ux%u NVN %#x goes (unused for %llu frames)",
                     (unsigned long long)(keep_finalized_max >> 20), tex->id, (unsigned long long)tex->gpu, tex->width,
                     tex->height, tex->nvn_format, (unsigned long long)(frame_no - last));
        }
        if (!overwritten && tex->gpu_written && !tex->released && !evict_rendered && age60(frame_no - last) < keep_rendered) continue;
        if (sweep && !overwritten && !gone_early) continue;
        if (gone_early) let_go_n++, let_go_bytes += tex->bytes + tex->pad;
        // (hookshot) nor a live texture the GPU drew whose texels cannot be written back into its memory (ASTC kept as BC3, sparse,
        // past the write-back's size, ...): its image is the only copy, and one made again over that storage is not uploaded (the
        // memory counts as the GPU's), so it showed whatever its new memory held: the rainbow/garbled textures that spread over play
        // time on 4-8 GB cards (issue 993)? Up to 256 MB of such textures each stay; bigger ones (a whole area left behind) go as
        // before. On by default since 2026-11-09 (8 GB RTX 3070s still showed rainbow textures once VRAM filled, Discord): the VRAM it
        // costs is bounded (256 MB a texture) and a wrong texture is worse than a fuller budget. RX_VK_EVICT_UNFLUSHABLE=1: evicted, as before
        static const bool evict_unflushable = getenv("RX_VK_EVICT_UNFLUSHABLE") && atoi(getenv("RX_VK_EVICT_UNFLUSHABLE")) != 0;
        if (!overwritten && tex->gpu_written && !tex->released && !evict_unflushable && tex->bytes < (256ull << 20) && !flushable(*tex)) {
            static std::atomic<int> kept_logged{0};
            if (kept_logged.fetch_add(1) < 40)
                logf("vk: texture #%u %#llx %ux%u x%u NVN %#x the GPU drew kept, not evicted: its texels cannot be written back",
                     tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, tex->nvn_format);
            continue;
        }
        // (and a small one never: its texels exist nowhere else, and it is made once and kept by the game. The sky's
        // 256 x 256 lookup table, unused while a menu was up, went and came back from memory as garbage: the rainbow
        // sky. The big ones that may go are a course left behind)
        // (a render target, drawn in the frames it was used in, goes: the dynamic resolution makes a new set of ~20 at
        // each size, and the old ones are not used again)
        const bool drawn_once = tex->last_written + 8 < tex->last_frame;
        if (!overwritten && tex->gpu_written && !tex->released && !evict_rendered && tex->bytes < (64ull << 20) && drawn_once) continue;
        // (nor a small one the GPU wrote in one frame alone: a table the game bakes once and samples now and then. The
        // pause screen's colour grading, 8 x 8 x 8, its slices drawn at boot and sampled next when the game is paused,
        // went after a minute unused, and the one made again over storage only the GPU had written stayed empty: the
        // pause blur black. Not a finalized one: the transient targets the game makes and finalizes every frame are
        // each "written in one frame", and kept so they added up, ~20 MB a minute)
        const bool baked = tex->first_written && tex->first_written == tex->last_written && tex->bytes <= (4ull << 20);
        if (!overwritten && tex->gpu_written && !tex->released && !evict_rendered && baked) continue;
        // (a texture the GPU drew that still holds the game's content goes into the game's memory first: made again later, it is
        // uploaded from there with what the GPU drew. Evicted without it, it came back empty or stale: a terrain chunk dark, a
        // tiled pattern over the Deku Tree's planks and Hyrule Field's path, with a 16 GB card's textures at the 6 GB budget
        // (issue 386). RX_VK_EVICT_NO_FLUSH=1: not written back, as before)
        static const bool no_flush = getenv("RX_VK_EVICT_NO_FLUSH") != nullptr;
        evict.push_back({tex, !no_flush && !overwritten && tex->gpu_written && !tex->released && flushable(*tex), shifted});
        pending_bytes += tex->bytes + tex->pad;
        if (by_hold && !overwritten) hold_live += tex->bytes + tex->pad;
        if (!overwritten) live_chosen++;
        if (!sweep && !overwritten && press.live && !press.held && come_back_on())   // ((Triforce) for the come-back pause, above)
            went_keys.push_back(evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format));
    }
    // ((Triforce) a pass over the budget that stopped at its step: the next look two presents on, the GPU done with these by then
    // and their memory back in the blocks (texture_frame), not ten)
    if (!sweep && tex_pressure::stepped(press.step, live_chosen)) press.next = frame_no + tex_pressure::kNextLook;
    if (!went_keys.empty()) {   // ((Triforce) what the live line let go, for the come-back pause: create() finds the ones made again)
        std::lock_guard lk(come_back_mu);
        for (uint64_t k : went_keys) come_back.went(k, frame_no);
        come_back_noted.store(true, std::memory_order_relaxed);
    }
    {   // (the write-backs: batches of up to 64 textures or ~512 MB of staging memory, each one submission and one wait)
        std::vector<Flush> batch;
        uint64_t batch_bytes = 0;
        const auto run = [&] {
            if (batch.empty()) return;
            flush_textures(batch);
            for (const Flush& f : batch)
                if (f.ok) {
                    static std::atomic<int64_t> flushed{0};
                    if (++flushed % 50 == 1) logf("vk: textures the GPU drew, written back into their memory before eviction: %lld so far", (long long)flushed.load());
                }
            batch.clear();
            batch_bytes = 0;
        };
        for (const Evict& e : evict) {
            if (!e.flush) continue;
            batch.push_back(Flush{e.tex, e.tex->gpu, e.tex->gpu + e.tex->guest_size});
            batch_bytes += e.tex->guest_size;   // (what its staging buffer holds: the texels as the storage lays them out)
            if (batch.size() >= 64 || batch_bytes >= (512ull << 20)) run();
        }
        run();
    }
    // (Triforce, texture detail: the ones a shader reads by texel position go now, whatever their age, and their keys are
    // made whole from here on: the next draw that binds one makes it again from the game's memory, every level. A pointer
    // is only compared here: its texture may have gone since it was asked)
    if (!whole_pending.empty()) {
        static uint64_t n_whole, logged_at;
        for (Texture* want : whole_pending)
            for (auto& kv : textures)
                if (kv.second.get() == want && want->detail_skip && want->whole_asked) {
                    whole_keys.insert(kv.first);
                    if (std::none_of(evict.begin(), evict.end(), [&](const Evict& e) { return e.tex == want; }))
                        evict.push_back({want, false, false});
                    n_whole++;
                    break;
                }
        whole_pending.clear();
        if (n_whole >= logged_at * 2 + 1) {
            logged_at = n_whole;
            logf("vk: texture detail: %llu textures kept whole so far: a shader reads them by texel position or asks "
                 "their size", (unsigned long long)n_whole);
        }
    }
    for (const Evict& e : evict) {   // (and each one's bookkeeping and its end, as before)
        Texture* tex = e.tex;
        if (e.shifted) n_dead++, b_dead += tex->bytes, shifted_now++;
        watch_tex(tex, "evicted");
        if (!tex->gpu_written && tex->uploaded && !remade_skip_old()) {   // (above: made again, it is uploaded from memory)
            std::lock_guard lk(from_memory_mu);
            if (evicted_from_memory.size() > 50000) evicted_from_memory.clear();   // (a bound)
            evicted_from_memory[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = tex->guest_size;
        }
        if (remade_log_on()) {
            std::lock_guard lk(evicted_mu);
            if (evicted_at.size() > 20000) evicted_at.clear();   // (a bound: only the recent ones matter)
            evicted_at[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = {frame_no, tex->gpu_written};
            remade.erase(tex);
        }
        destroy_later(*tex);
        forget_gpu(tex);
        gone.push_back(tex);
    }
    for (auto it = textures.begin(); it != textures.end() && !gone.empty();) {
        auto g = std::find(gone.begin(), gone.end(), it->second.get());
        if (g != gone.end()) {
            gone.erase(g);
            it = textures.erase(it);
        } else {
            ++it;
        }
    }
    if (n_dead && frame_no >= dead_logged_at + 600) {   // (the dead transient targets let go so far, every 600 presents)
        dead_logged_at = frame_no;
        logf("vk: render targets drawn over from another start, let go: %llu so far (%llu MB)", (unsigned long long)n_dead,
             (unsigned long long)(b_dead >> 20));
    }
    const size_t n = textures_before - textures.size();   // ((Triforce) not order's size: a pass under the budget orders only some)
    if (n && !dead_only) logf("vk: %zu textures retired (%zu left, %llu MB)", n, textures.size(), (unsigned long long)(total_bytes >> 20));
    if (let_go_n / 256 != let_go_logged_at) {
        let_go_logged_at = let_go_n / 256;
        logf("vk: textures the game had let go, gone before their age while VRAM is over %llu%% of what the driver allows: %llu so "
             "far (%llu MB; RX_VK_LET_GO_PCT=0: off)", (unsigned long long)let_go_pct, (unsigned long long)let_go_n,
             (unsigned long long)(let_go_bytes >> 20));
    }
    // (the next pass for the dead targets: by how fast this one found them coming)
    if (dead_quick && dead_targets) {
        if (shifted_now >= pass_presents) dead_sweep_next = frame_no + 1;
        else if (shifted_now * 3 >= pass_presents) dead_sweep_next = frame_no + 3;
    }
    if (dead_only) {
        quick_passes++;
        quick_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pass_t0).count();
    }
    if (quick_passes && frame_no >= quick_logged_at + 600) {
        quick_logged_at = frame_no;
        logf("vk: passes for the dead render targets between the looks: %llu so far, %.2f ms each", (unsigned long long)quick_passes,
             quick_ns / 1e6 / (double)quick_passes);
    }
}

void texture_log_guest(const Texture* t) {
    if (!t || !t->guest_size) return;
    std::vector<uint8_t> b(65536);
    char lv[512] = "";
    int k = 0;
    uint64_t all_nz = 0, all_unmapped = 0;
    for (uint32_t l = 0; l < std::max(1u, t->d.levels) && l < 16; l++) {
        const uint64_t lo = host->tex_level_offset(&t->d, l);
        const uint64_t hi = l + 1 < std::max(1u, t->d.levels) ? host->tex_level_offset(&t->d, l + 1) : t->guest_size;
        uint64_t nz = 0, unmapped = 0, pages = 0, empty_pages = 0;
        for (uint64_t off = lo; off < hi; off += 65536) {
            const uint64_t n = std::min<uint64_t>(65536, hi - off);
            if (!host->gpu_read(t->gpu + off, b.data(), n)) { unmapped += n; continue; }
            uint64_t page_nz = 0;
            for (uint64_t i = 0; i < n; i++) page_nz += b[i] != 0;
            nz += page_nz, pages++, empty_pages += !page_nz;
        }
        all_nz += nz, all_unmapped += unmapped;
        if (k < 480)
            k += snprintf(lv + k, sizeof lv - k, " L%u: %llu KB, %llu not zero, %llu of %llu mapped pages empty%s;", l,
                          (unsigned long long)((hi - lo) >> 10), (unsigned long long)nz, (unsigned long long)empty_pages,
                          (unsigned long long)pages, unmapped ? " (part unmapped)" : "");
    }
    const uint64_t g0 = host->gpu_guest(t->gpu);
    logf("vk: dump guest memory of #%u %#llx %ux%u NVN %#x (guest %#llx%s): %llu bytes not zero, %llu KB unmapped;%s", t->id,
         (unsigned long long)t->gpu, t->width, t->height, t->nvn_format, (unsigned long long)g0,
         t->gpu >= (1ull << 40) ? ", a virtual pool's" : "", (unsigned long long)all_nz,
         (unsigned long long)(all_unmapped >> 10), lv);
}

void texture_released(const nvn_tex* t) {
    if (!t) return;
    auto [a, b] = by_gpu.equal_range(t->gpu);
    for (auto it = a; it != b; ++it) it->second->let_go = std::max<uint64_t>(frame_no, 1);   // (texture_sweep: first to go while VRAM is short)
    // (Triforce, a lab switch, off: RX_VK_RELEASE_OWN=1 finalizes only the image the released texture has, NX2navi's rule
    // (there a release that finalized every image at the storage left terrain height maps drawn at height zero). Not
    // the default: this renderer keeps an image for each size and format of a storage, and in the lab's first minute of
    // play 32,767 images were spared this way, most of them at the start of one pool, render targets of other sizes
    // that only such a release ever marks as let go. What that does to memory and to the write-backs before eviction
    // has not been measured (the one run with it on was slow for another reason). The count of the others spared is
    // logged as it doubles)
    static const bool own_only = getenv("RX_VK_RELEASE_OWN") && atoi(getenv("RX_VK_RELEASE_OWN")) != 0;
    const Texture* own = nullptr;
    bool own_known = false;
    if (own_only) {
        const VkFormatInfo fi = vk_format(t->d.format);
        int bw = 1, bh = 1;
        const int bpb = fi.format != VK_FORMAT_UNDEFINED ? nvn_block_size(t->d.format, &bw, &bh) : 0;
        if (bpb > 0) {
            const auto it = textures.find(key_of(t, fi, bpb));
            own_known = true, own = it == textures.end() ? nullptr : it->second.get();
        }
    }
    for (auto it = a; it != b; ++it)
        if (it->second->gpu_written && !it->second->released) {
            if (own_known && it->second != own) {
                static uint64_t spared, told_at;
                if (++spared >= told_at * 2 + 1) {
                    told_at = spared;
                    logf("vk: a texture let go at %#llx (%ux%u NVN %#x): another texture's image there, #%u %ux%u NVN %#x, "
                         "which the GPU drew, stays as it is (%llu such so far)", (unsigned long long)t->gpu, t->d.width,
                         t->d.height, t->d.format, it->second->id, it->second->width, it->second->height,
                         it->second->nvn_format, (unsigned long long)spared);
                }
                continue;
            }
            it->second->released = true, it->second->release_gen = memory_generation();
            watch_tex(it->second, "finalized");
            static int logged;   // (a big one: the course maps' kind)
            if (it->second->bytes >= (32ull << 20) && logged++ < 40)
                logf("vk: texture #%u %#llx %ux%u x%u NVN %#x (%llu MB, rendered or copied into) finalized by the game",
                     it->second->id, (unsigned long long)it->second->gpu, it->second->width, it->second->height,
                     it->second->layers, it->second->nvn_format, (unsigned long long)(it->second->bytes >> 20));
        }
}

uint64_t texture_made() {
    return st_made;
}
void texture_unswizzle_stats(int64_t* bytes, int64_t* ns) {
    *bytes = st_unswizzle_bytes, *ns = st_unswizzle_ns;
}
size_t texture_count() {
    return textures.size();
}
uint64_t texture_bytes() {
    return total_bytes;
}
void texture_stats(int64_t* uploads, int64_t* astc_gpu, int64_t* bytes_uploaded) {
    *uploads = st_uploads, *astc_gpu = st_astc, *bytes_uploaded = st_bytes;
}

bool texels_fit(const nvn_tex* t, const NVNcopyRegion* r, uint64_t row, uint64_t avail) {
    if (!t || !r) return false;
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(t->d.format, &bw, &bh);
    if (bpb <= 0) return true;   // (texture_read_texels copies nothing)
    const uint64_t w = (uint64_t)((std::max(0, r->width) + bw - 1) / bw), h = (uint64_t)((std::max(0, r->height) + bh - 1) / bh);
    const uint64_t dd = (uint64_t)std::max(1, r->depth);
    if (!w || !h) return true;
    const uint64_t pitch = row ? row : w * bpb;
    uint64_t image = 0, a = 0, b = 0, n = 0;
    return !__builtin_mul_overflow(pitch, h, &image) && !__builtin_mul_overflow(dd - 1, image, &a) &&
           !__builtin_mul_overflow(h - 1, pitch, &b) && !__builtin_add_overflow(a, b, &n) &&
           !__builtin_add_overflow(n, w * bpb, &n) && n <= avail;
}

/// ReadTexels / WriteTexels: the CPU's copy of a region in guest memory (block linear, the Switch 2 GOB swizzle).
/// A write also stamps the pages, so the image is uploaded again; a texture the GPU wrote is read from guest memory
/// as it is (its newest texels are in the image: not yet read back).
void texture_read_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* data,
                         uint64_t row, uint64_t img, bool write) {
    if (!t || !t->gpu || !r) return;
    {   // (RX_VK_WATCH_RANGE: ReadTexels / WriteTexels of textures overlapping it, 400 lines)
        static const std::pair<uint64_t, uint64_t> range = [] {
            unsigned long long a = 0, b = 0;
            if (const char* e = getenv("RX_VK_WATCH_RANGE")) sscanf(e, "%llx:%llx", &a, &b);
            return std::pair<uint64_t, uint64_t>{a, b};
        }();
        static int logged;
        if (range.second && t->gpu < range.second && t->gpu + std::max<uint64_t>(t->L.size, 1) > range.first && logged++ < 400)
            logf("vk: watch range %s %#llx %ux%u NVN %#x level %u layer %u region (%d,%d %dx%d) mapped %s", write ? "WriteTexels" : "ReadTexels",
                 (unsigned long long)t->gpu, t->d.width, t->d.height, t->d.format, level, layer0, r->xoffset, r->yoffset,
                 r->width, r->height, host->gpu_guest(t->gpu + host->tex_level_offset(&t->d, level)) ? "(level start yes)" : "(level start no)");
    }
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(t->d.format, &bw, &bh);
    if (bpb <= 0) return;
    const nvn_texdesc& d = t->d;
    const uint32_t lw = (std::max(1u, d.width >> level) + bw - 1) / bw;
    const uint32_t lh = d.target == NVN_TEXTURE_TARGET_1D || d.target == NVN_TEXTURE_TARGET_1D_ARRAY
                            ? 1 : (std::max(1u, d.height >> level) + bh - 1) / bh;
    const bool vol = d.target == NVN_TEXTURE_TARGET_3D;
    const uint64_t base = t->gpu + host->tex_level_offset(&d, level);
    unsigned th = 0, td = 0;
    host->tex_level_tiles(&d, level, &th, &td);
    const uint32_t x0 = (uint32_t)std::max(0, r->xoffset) / bw, y0 = (uint32_t)std::max(0, r->yoffset) / bh;
    const uint32_t w = (uint32_t)(std::max(0, r->width) + bw - 1) / bw, h = (uint32_t)(std::max(0, r->height) + bh - 1) / bh;
    const uint32_t z0 = (uint32_t)std::max(0, r->zoffset), dd = (uint32_t)std::max(1, r->depth);
    const uint64_t pitch = row ? row : (uint64_t)w * bpb, image = img ? img : pitch * h;
    const bool linear = nvn_tex_is_linear(&d);
    const uint32_t stride = linear ? (d.stride ? d.stride : host->tex_linear_stride(&d)) : 0;
    const uint64_t gobs_x = ((uint64_t)lw * bpb + 63) >> 6;
    const uint64_t rows_y = (lh + (8u << th) - 1) >> (3 + th);
    const uint64_t block = 512ull << (th + td);
    for (uint32_t k = 0; k < dd; k++) {
        const uint32_t z = vol ? z0 + k : 0;
        const uint64_t layer_base = base + (vol ? 0 : (uint64_t)(layer0 + z0 + k) * t->L.layer_size);
        for (uint32_t y = 0; y < h && y0 + y < lh; y++) {
            uint8_t* line = data + k * image + y * pitch;
            const uint32_t yy = y0 + y;
            for (uint32_t xb = 0; xb < (uint64_t)w * bpb;) {
                const uint32_t xbyte = x0 * bpb + xb;
                uint64_t off;
                uint32_t run;
                if (linear) {
                    off = (uint64_t)yy * stride + xbyte;
                    run = w * bpb - xb;
                } else {   // (16 bytes of a GOB row are contiguous)
                    const uint64_t gob = ((xbyte & 63) >> 5) << 8 | ((yy & 7) >> 2) << 7 | ((xbyte & 31) >> 4) << 6 |
                                         ((yy >> 1) & 1) << 5 | (yy & 1) << 4 | (xbyte & 15);
                    off = (uint64_t)(z >> td) * gobs_x * rows_y * block + (uint64_t)(yy >> (3 + th)) * gobs_x * block +
                          (uint64_t)(xbyte >> 6) * block + ((uint64_t)(z & ((1u << td) - 1)) << (9 + th)) +
                          ((uint64_t)((yy >> 3) & ((1u << th) - 1)) << 9) + gob;
                    run = std::min<uint32_t>(16 - (xbyte & 15), w * bpb - xb);
                }
                if (write) host->gpu_write(layer_base + off, line + xb, run);   // (the shadows hear of the whole: guest_cpu_wrote below)
                else host->gpu_read(layer_base + off, line + xb, run);
                xb += run;
            }
        }
    }
    if (write) {
        guest_cpu_wrote(t->gpu, t->L.size);   // (the write-back diff's shadows: one report for the 16-byte runs above)
        const uint64_t guest = host->gpu_guest(t->gpu);
        if (guest) memory_stamp(guest, t->L.size);
        auto [a, b] = by_gpu.equal_range(t->gpu);
        for (auto it = a; it != b; ++it) it->second->gpu_written = false, it->second->checked_epoch = 0;
    }
}

} // namespace mkwvk
