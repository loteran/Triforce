// (Triforce) The Insert menu's Anti-aliasing row at work: the passes that stand in for the game's SMAA composition, and what
// the frame's dispatches are told about it; see vk_aa.h and aa_rows.h. After NX2navi's runtime/vk_aa.c.
//
// Everything here runs on the threads that record the replay. The choice is taken once a frame, by the first dispatch of the
// frame that asks: the game's edge and weight passes and its composition of one frame then agree, whatever the menu does
// meanwhile. With the parallel replay several threads ask at once: the choice and what the passes share are under one lock,
// which a frame's later askers do not take (they read the frame's number and its choice).
//
// tests/test_aa_passes.py compiles this file whole against stand-ins for the rest of the renderer: what the dispatches are
// told, in the suite, and with --gpu the passes themselves on the PC's own graphics card, from authored pictures.
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "aa_rows.h"
#include "upscaler_rows.h"
#include "vk_aa.h"
#include "vk_fsr3.h"       // (fsr3_wanted: AMD FSR 3.1 upscales, with its own anti-aliasing)
#include "vk_menu.h"
#include "vk_temporal.h"   // (temporal_mode: NVIDIA DLSS or AMD FSR 4 does)
#include "vk_texture.h"    // (texture_frame_no)

#include "../../third_party/smaa/AreaTex.h"     // (SMAA's lookup textures, MIT: third_party/smaa/PROVENANCE.md)
#include "../../third_party/smaa/SearchTex.h"
#include "shaders/aa_post_comp_spv.h"
#include "shaders/smaa_blend_comp_spv.h"
#include "shaders/smaa_edge_comp_spv.h"
#include "shaders/smaa_luma_comp_spv.h"
#include "shaders/smaa_weight_comp_spv.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
VkResult shared_cache_compute(const VkComputePipelineCreateInfo* ci, VkPipeline* out);   // (vk_draw.cpp: with the pipeline cache kept across runs)

namespace {

namespace ar = aa_rows;

/// RX_VK_AA=<0..3>: the choice for the run, -1 unset (anything else than a number: unset)
int env_choice() {
    static const int v = [] {
        const char* e = getenv("RX_VK_AA");
        return e && *e >= '0' && *e <= '9' ? atoi(e) : -1;
    }();
    return v;
}
/// RX_VK_AA_KEEP=1: the game's edge and weight passes always run
bool keep_passes() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_AA_KEEP");
        return e && *e && strcmp(e, "0") != 0;
    }();
    return on;
}
/// What upscales now, as the menu's Upscaler row shows it (vk_menu.cpp upscaler)
int upscaler_now() {
    if (fsr3_wanted()) return fsr3_native() ? upscaler_rows::FSR3_AA : upscaler_rows::FSR3;
    return temporal_mode();
}
/// ((Triforce, FSR3-06, FSR4-5) An upscaler draws from the tone mapped picture, so the game's SMAA is left out with the game's
/// own chosen too (aa_rows.h effective): the dispatches ask then. Taken once a frame, as every dispatch asks aa_asked
bool upscaler_asks() {
    static std::atomic<uint64_t> frame{~0ull};
    static std::atomic<bool> asks{false};
    const uint64_t now = texture_frame_no();
    if (frame.load(std::memory_order_acquire) != now) {
        asks.store(ar::effective(ar::GAME, upscaler_now(), temporal_before_bloom(), env_choice() >= 0) != ar::GAME,
                   std::memory_order_relaxed);
        frame.store(now, std::memory_order_release);
    }
    return asks.load(std::memory_order_relaxed);
}

// ---- the frame's choice, and what became of the game's passes ----

std::atomic<uint64_t> frame_read{~0ull};   // (the frame the choice was last taken for)
std::atomic<int> mode_now{ar::GAME};       // (that frame's)
std::atomic<bool> engaged{false};          // (something of this file is at work or left to free: the dispatches ask)
std::atomic<bool> last_replaced{false};    // (the last composition seen was replaced: the passes before the next may go)
std::atomic<int> state_now{ar::QUIET};

enum { P_POST, P_LUMA, P_EDGE, P_WEIGHT, P_BLEND, P_N };
constexpr int kPostFirst = P_POST, kPostEnd = P_LUMA, kSmaaFirst = P_LUMA, kSmaaEnd = P_N;
constexpr VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
struct PassInfo {
    const uint32_t* code;
    size_t size;
    int n;   // (its bindings, 0 to n - 1: the sampled pictures, then the image it stores into)
    VkDescriptorType type[4];
    const char* name;
};
const PassInfo kPasses[P_N] = {
    {AA_POST_COMP_SPV, sizeof AA_POST_COMP_SPV, 2, {S, I}, "aa_post"},
    {SMAA_LUMA_COMP_SPV, sizeof SMAA_LUMA_COMP_SPV, 2, {S, I}, "smaa_luma"},
    {SMAA_EDGE_COMP_SPV, sizeof SMAA_EDGE_COMP_SPV, 2, {S, I}, "smaa_edge"},
    {SMAA_WEIGHT_COMP_SPV, sizeof SMAA_WEIGHT_COMP_SPV, 4, {S, S, S, I}, "smaa_weight"},
    {SMAA_BLEND_COMP_SPV, sizeof SMAA_BLEND_COMP_SPV, 3, {S, S, I}, "smaa_blend"},
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;
};

/// SMAA's work images stay unused once the row leaves SMAA Ultra, three pictures of the 3D's size (10 bytes a pixel): they go
/// kWorkIdle presents after the last frame that used them, and are made again by the next that does. A minute, not less: the
/// passes run only where the game runs its own SMAA, so a long pause or a loading screen counts as unused too
constexpr uint64_t kWorkIdle = 3600;
/// An image let go is destroyed this many presents later: work recorded with it may still be in flight
constexpr uint64_t kDeadFrames = 120;

struct State {
    std::mutex mu;
    // (the pipelines: 0 not tried, 1 made, -1 could not be)
    int post = 0, smaa = 0;
    VkDescriptorSetLayout set[P_N]{};
    VkPipelineLayout layout[P_N]{};
    VkPipeline pipe[P_N]{};
    VkSampler linear = VK_NULL_HANDLE, nearest = VK_NULL_HANDLE;
    // (SMAA's lookup textures: 0 not made, 1 ready, -1 could not be)
    int tables = 0;
    Image area, search;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation staging_memory = nullptr;
    // (its work images, at the picture's size)
    Image luma, edges, weights;
    uint32_t w = 0, h = 0;
    uint64_t smaa_frame = 0;   // (the last frame that recorded SMAA's passes)
    uint64_t work_failed = 0;  // (the frame they could not be made in, 0: none: not tried again at once)
    std::vector<std::pair<uint64_t, Image>> dead;
    ar::Track track;   // (what became of the game's passes, frame to frame: the atomics above are its copies)
    // (what was said in the log)
    int told_mode = ar::GAME;
    const char* told_why = nullptr;   // (one of the callers' string constants, told by its address)
    int told = 0, told_missing = 0;
    bool told_smaa_off = false;
};
State& st() {
    static State s;
    return s;
}

void drop(State& s, Image& m) {
    if (m.image) s.dead.push_back({texture_frame_no(), m});
    m = Image{};
}
void free_dead(State& s, uint64_t frame) {
    for (size_t i = 0; i < s.dead.size();) {
        if (frame - s.dead[i].first < kDeadFrames) {
            i++;
            continue;
        }
        if (s.dead[i].second.view) vkDestroyImageView(dev.device, s.dead[i].second.view, nullptr);
        destroy_image(s.dead[i].second.image, s.dead[i].second.memory);
        s.dead[i] = s.dead.back();
        s.dead.pop_back();
    }
}

/// The frame's first asker, once the choice is known (the lock held): what is idle goes, and what is kept about the game's
/// passes is brought up to date
void frame_begun(State& s, uint64_t frame, int want) {
    if (s.luma.image && frame - s.smaa_frame >= kWorkIdle) {
        logf("vk: aa: SMAA's work images unused for %llu frames: freed (%ux%u)", (unsigned long long)kWorkIdle, s.w, s.h);
        drop(s, s.luma), drop(s, s.edges), drop(s, s.weights);
        s.w = s.h = 0;
    }
    free_dead(s, frame);
    // (another game version, or frames without the passes aa_rows.h knows: nothing is replaced. A few times a run)
    if (s.track.begin(frame, want) && s.told_missing++ < 4)
        logf("vk: aa: %s replaced none of the game's SMAA compositions in %llu frames: the picture stays the game's",
             ar::name(want), (unsigned long long)ar::kMissingFrames);
    if (want == ar::GAME) s.told_mode = ar::GAME, s.told_why = nullptr;
    last_replaced.store(s.track.last_replaced, std::memory_order_release);
    state_now.store(s.track.state, std::memory_order_relaxed);
    engaged.store(want != ar::GAME || s.luma.image || !s.dead.empty(), std::memory_order_relaxed);
}

// ---- the passes ----

bool make_samplers(State& s) {
    if (s.linear && s.nearest) return true;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (!s.linear && vkCreateSampler(dev.device, &si, nullptr, &s.linear) != VK_SUCCESS) return s.linear = VK_NULL_HANDLE, false;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (!s.nearest && vkCreateSampler(dev.device, &si, nullptr, &s.nearest) != VK_SUCCESS) return s.nearest = VK_NULL_HANDLE, false;
    return true;
}

bool storage_and_linear(VkFormat f) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, f, &props);
    const VkFormatFeatureFlags want = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (props.optimalTilingFeatures & want) == want;
}

/// Passes [first, end) as pipelines: each with a set of push descriptors and 32 bytes of push constants. false: one could not be
bool make_pipelines(State& s, int first, int end) {
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR || dev.max_push_descriptors < 4) {
        logf("vk: aa: no push descriptors on this GPU (%u)", dev.max_push_descriptors);
        return false;
    }
    if (!make_samplers(s)) return false;
    for (int p = first; p < end; p++) {
        const PassInfo& k = kPasses[p];
        VkDescriptorSetLayoutBinding b[4]{};
        for (int i = 0; i < k.n; i++) b[i] = {(uint32_t)i, k.type[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        li.bindingCount = (uint32_t)k.n, li.pBindings = b;
        const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 1, pi.pSetLayouts = &s.set[p], pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = k.size, mi.pCode = k.code;
        VkShaderModule m = VK_NULL_HANDLE;
        bool ok = vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &s.set[p]) == VK_SUCCESS &&
                  vkCreatePipelineLayout(dev.device, &pi, nullptr, &s.layout[p]) == VK_SUCCESS &&
                  vkCreateShaderModule(dev.device, &mi, nullptr, &m) == VK_SUCCESS;
        if (ok) {
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
            ci.layout = s.layout[p];
            // (through the pipeline cache kept across runs, as vk_fsr3.cpp's: not compiled again inside a frame of each run)
            ok = shared_cache_compute(&ci, &s.pipe[p]) == VK_SUCCESS;
            if (!ok) s.pipe[p] = VK_NULL_HANDLE;
        }
        if (m) vkDestroyShaderModule(dev.device, m, nullptr);
        if (!ok) {
            logf("vk: aa: the %s pipeline could not be made", k.name);
            return false;
        }
    }
    return true;
}

/// An image of the passes' own, GENERAL from cmd on
bool make_image(VkCommandBuffer cmd, Image& m, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = format;
    ci.extent = {w, h, 1}, ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL, ci.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!create_image(ci, m.image, m.memory)) return m = Image{}, false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = m.image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device, &vi, nullptr, &m.view) != VK_SUCCESS) {
        destroy_image(m.image, m.memory);
        return m = Image{}, false;
    }
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};   // (UNDEFINED -> GENERAL, once)
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.srcAccessMask = VK_ACCESS_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                      VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m.image, b.subresourceRange = vi.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    return true;
}

void compute_after(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage, b.srcAccessMask = src_access;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

/// SMAA's area and search textures, made once and filled by cmd from a buffer that stays (the copy runs when cmd does)
bool make_tables(State& s, VkCommandBuffer cmd) {
    if (s.tables) return s.tables > 0;
    s.tables = -1;
    static_assert(sizeof areaTexBytes == AREATEX_SIZE && sizeof searchTexBytes == SEARCHTEX_SIZE);
    void* mapped = nullptr;
    const uint64_t bytes = AREATEX_SIZE + SEARCHTEX_SIZE;
    if (!create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, s.staging, s.staging_memory, &mapped) || !mapped) return false;
    memcpy(mapped, areaTexBytes, AREATEX_SIZE);
    memcpy((uint8_t*)mapped + AREATEX_SIZE, searchTexBytes, SEARCHTEX_SIZE);
    flush_mapped(s.staging_memory, bytes);
    if (!make_image(cmd, s.area, VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !make_image(cmd, s.search, VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        drop(s, s.area), drop(s, s.search);
        destroy_buffer(s.staging, s.staging_memory);   // (nothing was recorded with it)
        s.staging = VK_NULL_HANDLE, s.staging_memory = nullptr;
        return false;
    }
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.bufferOffset = 0, r.imageExtent = {AREATEX_WIDTH, AREATEX_HEIGHT, 1};
    vkCmdCopyBufferToImage(cmd, s.staging, s.area.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    r.bufferOffset = AREATEX_SIZE, r.imageExtent = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1};
    vkCmdCopyBufferToImage(cmd, s.staging, s.search.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    compute_after(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    s.tables = 1;
    return true;
}

/// SMAA's work images at the picture's size, made again when it changes
bool make_work(State& s, VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    if (s.luma.image && s.w == w && s.h == h) return true;
    // (no room for them a moment ago: asked again ten seconds on, not every frame)
    const uint64_t frame = texture_frame_no();
    if (s.work_failed && frame - s.work_failed < 600) return false;
    s.work_failed = 0;
    drop(s, s.luma), drop(s, s.edges), drop(s, s.weights);
    s.w = s.h = 0;
    if (!make_image(cmd, s.luma, VK_FORMAT_R16_SFLOAT, w, h, VK_IMAGE_USAGE_STORAGE_BIT) ||
        !make_image(cmd, s.edges, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_STORAGE_BIT) ||
        !make_image(cmd, s.weights, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_STORAGE_BIT)) {
        drop(s, s.luma), drop(s, s.edges), drop(s, s.weights);
        s.work_failed = frame ? frame : 1;
        return false;
    }
    s.w = w, s.h = h;
    return true;
}

/// One pass: its pipeline, its bindings (views in the pass's order; a sampler for each sampled one), its push constants,
/// and a barrier after it: the next reads what it stored
void pass(State& s, VkCommandBuffer cmd, int p, const VkImageView* views, const VkSampler* samplers, const void* push, uint32_t push_bytes,
          uint32_t w, uint32_t h) {
    const PassInfo& k = kPasses[p];
    VkDescriptorImageInfo info[4]{};
    VkWriteDescriptorSet wr[4]{};
    for (int i = 0; i < k.n; i++) {
        info[i] = {k.type[i] == S ? samplers[i] : VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstBinding = (uint32_t)i, wr[i].descriptorCount = 1, wr[i].descriptorType = k.type[i], wr[i].pImageInfo = &info[i];
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipe[p]);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout[p], 0, (uint32_t)k.n, wr);
    vkCmdPushConstants(cmd, s.layout[p], VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes, push);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    compute_after(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
}

} // namespace

int aa_mode() {
    const uint64_t frame = texture_frame_no();
    if (frame_read.load(std::memory_order_acquire) == frame) return mode_now.load(std::memory_order_acquire);
    // (the frame's first asker takes the choice; one that asks meanwhile waits here for it, and gets the same)
    State& s = st();
    std::lock_guard lk(s.mu);
    if (frame_read.load(std::memory_order_relaxed) != frame) {
        const int choice = ar::chosen(opts.aa, env_choice());
        // ((Triforce, FSR3-06, FSR4-5) with an upscaler after the bloom, none: aa_rows.h effective)
        const int want = ar::effective(choice, upscaler_now(), temporal_before_bloom(), env_choice() >= 0);
        frame_begun(s, frame, want);
        mode_now.store(want, std::memory_order_release);
        frame_read.store(frame, std::memory_order_release);
    }
    return mode_now.load(std::memory_order_relaxed);
}

int aa_env_choice() { return env_choice(); }

bool aa_asked() { return ar::chosen(opts.aa, env_choice()) != ar::GAME || engaged.load(std::memory_order_relaxed) || upscaler_asks(); }

bool aa_leave_out(uint64_t key) {
    if (!aa_asked()) return false;
    const int mode = aa_mode();
    return ar::leave_out(ar::role(key), mode, last_replaced.load(std::memory_order_acquire), keep_passes());
}

bool aa_record(VkCommandBuffer cmd, int mode, VkImageView src, VkImageView dst, uint32_t w, uint32_t h) {
    if (mode == ar::GAME || !src || !dst || !w || !h) return false;
    State& s = st();
    std::lock_guard lk(s.mu);
    if (!s.post) s.post = make_pipelines(s, kPostFirst, kPostEnd) ? 1 : -1;
    if (s.post < 0) return false;
    if (mode == ar::SMAA) {
        if (!s.smaa)
            s.smaa = dev.features.shaderStorageImageExtendedFormats && storage_and_linear(VK_FORMAT_R16_SFLOAT) &&
                             storage_and_linear(VK_FORMAT_R8G8B8A8_UNORM) && make_pipelines(s, kSmaaFirst, kSmaaEnd)
                         ? 1 : -1;
        if (s.smaa < 0 || !make_tables(s, cmd) || !make_work(s, cmd, w, h)) {
            if (!s.told_smaa_off) logf("vk: aa: SMAA Ultra cannot run here (its pipelines or images, %ux%u): it runs as Off", w, h);
            s.told_smaa_off = true;
            mode = ar::OFF;
        }
    }
    if (mode != ar::SMAA) {
        struct {
            float inv[2];
            int32_t size[2];
            int32_t fxaa;
        } pc = {{1.0f / (float)w, 1.0f / (float)h}, {(int32_t)w, (int32_t)h}, mode == ar::FXAA ? 1 : 0};
        const VkImageView views[2] = {src, dst};
        const VkSampler samplers[2] = {s.linear, VK_NULL_HANDLE};
        pass(s, cmd, P_POST, views, samplers, &pc, sizeof pc, w, h);
        return true;
    }
    s.smaa_frame = texture_frame_no();
    const float metrics[4] = {1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h};
    const VkSampler linear[4] = {s.linear, s.linear, s.linear, s.linear};
    const VkSampler weight_samplers[4] = {s.linear, s.linear, s.nearest, VK_NULL_HANDLE};
    const VkImageView luma[2] = {src, s.luma.view};
    const VkImageView edge[2] = {s.luma.view, s.edges.view};
    const VkImageView weight[4] = {s.edges.view, s.area.view, s.search.view, s.weights.view};
    const VkImageView blend[3] = {src, s.weights.view, dst};
    pass(s, cmd, P_LUMA, luma, linear, metrics, sizeof metrics, w, h);
    pass(s, cmd, P_EDGE, edge, linear, metrics, sizeof metrics, w, h);
    pass(s, cmd, P_WEIGHT, weight, weight_samplers, metrics, sizeof metrics, w, h);
    pass(s, cmd, P_BLEND, blend, linear, metrics, sizeof metrics, w, h);
    return true;
}

void aa_composed(bool replaced, const char* why) {
    State& s = st();
    std::lock_guard lk(s.mu);
    const int mode = mode_now.load(std::memory_order_acquire);
    s.track.composed(texture_frame_no(), replaced);
    if (replaced) {
        if (s.told_mode != mode) logf("vk: aa: the game's SMAA composition replaced by %s", ar::name(mode));
        s.told_mode = mode, s.told_why = nullptr;
    } else if (why && why != s.told_why && s.told < 16) {
        // (left to the game, with its own edge and weight passes from the next frame on. Each reason once as it changes)
        s.told++, s.told_why = why, s.told_mode = ar::GAME;
        logf("vk: aa: a composition of the game's SMAA left to the game (%s): %s", ar::name(mode), why);
    }
    last_replaced.store(s.track.last_replaced, std::memory_order_release);
    state_now.store(s.track.state, std::memory_order_relaxed);
}

int aa_state() { return state_now.load(std::memory_order_relaxed); }

} // namespace mkwvk
