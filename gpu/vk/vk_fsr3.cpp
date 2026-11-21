// (hookshot) AMD FSR 3.1's upscaler as our own dispatches over AMD's vendored passes, and its place in the gameplay upscale's
// dispatch (fsr3_upscale, at the end); see vk_fsr3.h.
//
// The resource table and the per-pass binding lists are the SDK's, not ours: ffx_fsr3upscaler.cpp decides the formats and
// sizes, and each pass file numbers its own bindings (FSR3UPSCALER_BIND_*). Getting one wrong is a validation error at best
// and a wrong picture at worst, so they are written out here in the SDK's order rather than derived.
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "vk_fsr3.h"
#include "vk_fsr3_inputs.h"
#include "vk_ffx3.h"      // (ffx3_usable: AMD's library draws FSR 3.1 in place of these passes)
#include "vk_fsr3fi.h"   // (frames_wanted: not before the bloom with frame interpolation on)
#include "vk_draw.h"      // (descriptors_disturbed)
#include "vk_gprof.h"     // (RX_VK_PASS_TIMING: a span per pass, so the report names them)
#include "vk_menu.h"
#include "vk_hudless.h"
#include "vk_ngxfg.h"
#include "vk_temporal.h"   // (temporal_env_chosen: RX_VK_DLSS / RX_VK_FSR4 over the row's FSR 3.1)
#include "vk_texture.h"
#include "sharpen_rules.h"   // ((Triforce, S1, S2) the Sharpness row as FSR's own: RCAS's constants, the boost below 3/4)
#include "upscaler_rows.h"

#include "shaders/dlss_out_comp_spv.h"   // (the network's output from a picture: its encoding squared back, as for DLSS)
#include "shaders/dlss_out_hdr_comp_spv.h"   // ((Triforce) the same into an RGBA16F output: RenoDX's HDR run)
#include "shaders/fsr3_accumulate_comp_spv.h"
#include "shaders/fsr3_accumulate_sharpen_comp_spv.h"   // ((Triforce, S1) FSR's own sharpening: the history alone, then RCAS)
#include "shaders/fsr3_rcas_comp_spv.h"
#include "shaders/fsr3_luma_instability_comp_spv.h"
#include "shaders/fsr3_luma_pyramid_comp_spv.h"
#include "shaders/fsr3_luma_pyramid_nowave_comp_spv.h"
#include "shaders/fsr3_prepare_inputs_comp_spv.h"
#include "shaders/fsr3_prepare_reactivity_comp_spv.h"
#include "shaders/fsr3_shading_change_comp_spv.h"
#include "shaders/fsr3_shading_change_pyramid_comp_spv.h"
#include "shaders/fsr3_shading_change_pyramid_nowave_comp_spv.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
VkResult shared_cache_compute(const VkComputePipelineCreateInfo* ci, VkPipeline* out);   // (vk_draw.cpp: with the pipeline cache kept across runs)
}

namespace mkwvk::fsr3 {
namespace {

/// A descriptor's kind, in the binding order each pass file declares. Every pass also takes the two
/// samplers at bindings 1000 (point clamp) and 1001 (linear clamp).
enum Kind : uint8_t { T, I, U };   // sampled texture, storage image, uniform buffer

struct PassInfo {
    const char* name;
    const uint32_t* spv;
    size_t spv_bytes;
    const Kind* bindings;
    size_t binding_count;
};

constexpr Kind PREPARE_INPUTS[]{T, T, T, I, I, I, I, I, U};
constexpr Kind LUMA_PYRAMID[]{T, T, I, I, I, I, I, I, I, I, I, U, U};
constexpr Kind SHADING_CHANGE_PYRAMID[]{T, T, T, T, I, I, I, I, I, I, I, U, U};
constexpr Kind SHADING_CHANGE[]{T, I, U};
constexpr Kind PREPARE_REACTIVITY[]{T, T, T, T, T, T, T, T, T, I, I, I, U};
constexpr Kind LUMA_INSTABILITY[]{T, T, T, T, T, T, T, I, I, U};
constexpr Kind ACCUMULATE[]{T, T, T, T, T, T, T, T, T, I, I, I, U};
constexpr Kind RCAS[]{T, T, I, U, U};   // (exposure, its input, the output; cbFSR3Upscaler, cbRCAS)

#define PASS(n, spv, b) PassInfo{n, spv, sizeof spv, b, sizeof b / sizeof b[0]}

/// The passes, in the order PassId numbers them. The two _nowave pyramids are alternatives to the two before them, for
/// devices without the subgroup operations the single-pass downsampler wants. ((Triforce, S1) FSR's own sharpening, as the
/// SDK runs it with sharpening on: accumulate_sharpen in accumulate's place (the same bindings; it writes the history alone),
/// then RCAS from that history into the output, at the Sharpness row's value. Before, they were left out and the row
/// sharpened the finished picture at the window, the HUD with it)
const PassInfo PASSES[]{
    PASS("prepare_inputs", FSR3_PREPARE_INPUTS_COMP_SPV, PREPARE_INPUTS),
    PASS("luma_pyramid", FSR3_LUMA_PYRAMID_COMP_SPV, LUMA_PYRAMID),
    PASS("luma_pyramid_nowave", FSR3_LUMA_PYRAMID_NOWAVE_COMP_SPV, LUMA_PYRAMID),
    PASS("shading_change_pyramid", FSR3_SHADING_CHANGE_PYRAMID_COMP_SPV, SHADING_CHANGE_PYRAMID),
    PASS("shading_change_pyramid_nowave", FSR3_SHADING_CHANGE_PYRAMID_NOWAVE_COMP_SPV,
         SHADING_CHANGE_PYRAMID),
    PASS("shading_change", FSR3_SHADING_CHANGE_COMP_SPV, SHADING_CHANGE),
    PASS("prepare_reactivity", FSR3_PREPARE_REACTIVITY_COMP_SPV, PREPARE_REACTIVITY),
    PASS("luma_instability", FSR3_LUMA_INSTABILITY_COMP_SPV, LUMA_INSTABILITY),
    PASS("accumulate", FSR3_ACCUMULATE_COMP_SPV, ACCUMULATE),
    PASS("accumulate_sharpen", FSR3_ACCUMULATE_SHARPEN_COMP_SPV, ACCUMULATE),
    PASS("rcas", FSR3_RCAS_COMP_SPV, RCAS),
};
#undef PASS

enum PassId : size_t {
    P_PREPARE_INPUTS,
    P_LUMA_PYRAMID,
    P_LUMA_PYRAMID_NOWAVE,
    P_SHADING_CHANGE_PYRAMID,
    P_SHADING_CHANGE_PYRAMID_NOWAVE,
    P_SHADING_CHANGE,
    P_PREPARE_REACTIVITY,
    P_LUMA_INSTABILITY,
    P_ACCUMULATE,
    P_ACCUMULATE_SHARPEN,
    P_RCAS,
    kPassCount
};
/// The two passes of FSR's own sharpening: made apart from the others, and FSR 3.1 draws without them when they cannot be
bool sharpen_pass(size_t i) { return i == P_ACCUMULATE_SHARPEN || i == P_RCAS; }
static_assert(sizeof PASSES / sizeof PASSES[0] == kPassCount, "PassId must name every pass");
constexpr uint32_t kSpdStoredMips = 6;

struct Pipeline {
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;          ///< every level
    std::vector<VkImageView> levels;            ///< one per level, for the mipped ones
    uint32_t width = 0, height = 0, level_count = 1;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

/// cbFSR3Upscaler, exactly as ffx_fsr3upscaler.cpp lays it out. The size is asserted because every
/// pass reads it by offset: a field out of place is a wrong picture, not a compile error.
struct Constants {
    int32_t render_size[2];
    int32_t previous_render_size[2];
    int32_t upscale_size[2];
    int32_t previous_upscale_size[2];
    int32_t max_render_size[2];
    int32_t max_upscale_size[2];
    float device_to_view_depth[4];
    float jitter[2];
    float previous_jitter[2];
    float motion_vector_scale[2];
    float downscale_factor[2];
    float motion_vector_jitter_cancellation[2];
    float tan_half_fov;
    float jitter_phase_count;
    float delta_time;
    float delta_pre_exposure;
    float view_space_to_meters;
    float frame_index;
    float velocity_factor;
    float reactiveness_scale;
    float shading_change_scale;
    float accumulation_added_per_frame;
    float min_disocclusion_accumulation;
};
static_assert(sizeof(Constants) == 148, "cbFSR3Upscaler must match the SDK's layout");

struct SpdConstants {
    uint32_t mips;
    uint32_t num_work_groups;
    uint32_t work_group_offset[2];
    uint32_t render_size[2];
};

/// One descriptor of a dispatch: an image view, or a range of the uniform buffer.
struct Resource {
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

/// A memory dependency over the whole queue, in the synchronization2 form this renderer loads.
void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage, b.srcAccessMask = src;
    b.dstStageMask = dst_stage, b.dstAccessMask = dst;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

constexpr VkImageSubresourceRange ALL_LEVELS{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};

struct State {
    bool tried = false;
    bool ok = false;
    bool wave = true;   ///< the pyramids with subgroup operations (else their _nowave variants)
    bool sharpen = false;   ///< ((Triforce, S1) FSR's own sharpening's two passes were made)
    float told_sharpness = -1.0f;   ///< (the sharpness the log last said)
    Pipeline passes[kPassCount];
    VkSampler point = VK_NULL_HANDLE;
    VkSampler linear = VK_NULL_HANDLE;
    // The SDK's internal images. The pairs ping-pong on frame parity, as its resource tables do.
    Image accumulation[2], luma[2], luma_history[2], internal_upscaled[2];
    Image farthest_depth, shading_change, new_locks, spd_mips, farthest_depth_mip1;
    Image dilated_motion_vectors, dilated_depth, previous_nearest_depth, dilated_reactive_masks;
    Image frame_info, spd_atomic, lanczos_lut, default_reactive, output;
    // The render-size images' size (the SDK's maxRenderSize: fsr3_math.h plan_images) and the output's. Each frame's own
    // render size is the dispatch's (upscale): smaller frames of the game's dynamic resolution keep the images and the history
    uint32_t render_width = 0, render_height = 0, upscale_width = 0, upscale_height = 0;
    uint64_t used_frame = 0;   ///< texture_frame_no of the last evaluation (release_idle)
    // The uniform buffer the constant blocks are written into, one set of slots per frame in flight so a frame being read
    // is not overwritten.
    VkBuffer uniforms = VK_NULL_HANDLE;
    VmaAllocation uniforms_memory = nullptr;
    void* uniforms_mapped = nullptr;
    VkDeviceSize slot_fsr = 0, slot_spd = 0, slot_lut = 0, frame_stride = 0;   // (and cbRCAS after the Lanczos table)
    uint32_t uniform_frame = 0;
    // History, as fsr3upscalerDispatch carries it.
    int32_t previous_render_size[2]{};
    int32_t previous_upscale_size[2]{};
    float previous_jitter[2]{};
    float frame_index = 0.0f;
    uint32_t parity = 0;
    bool first_evaluation = true;   ///< the images are new: their first layout, cleared
    bool lut_uploaded = false;
    // Images of an earlier size, freed once the frames that may still read them are done (texture_frame_no)
    std::vector<std::pair<uint64_t, Image>> dead;
};

constexpr uint32_t kUniformFrames = 4;   // (more than the swapchain's images in flight)

State g;

void destroy_now(Image& t) {
    for (VkImageView v : t.levels) vkDestroyImageView(dev.device, v, nullptr);
    if (t.view) vkDestroyImageView(dev.device, t.view, nullptr);
    if (t.image) mkwvk::destroy_image(t.image, t.memory);
    t = Image{};
}

/// An image let go: destroyed a few presents on, as work recorded with it may still be in flight
void drop_image(Image& t) {
    if (t.image) g.dead.push_back({texture_frame_no(), std::move(t)});
    t = Image{};
}

void free_dead() {
    const uint64_t now = texture_frame_no();
    for (size_t i = 0; i < g.dead.size();) {
        if (now - g.dead[i].first > 4) {
            destroy_now(g.dead[i].second);
            g.dead[i] = std::move(g.dead.back());
            g.dead.pop_back();
        } else {
            i++;
        }
    }
}

/// One of the SDK's internal images: storage and sampled, GENERAL throughout, with a view per level when it has more than
/// one (the pyramids bind their levels separately).
bool make_image(Image& t, VkFormat format, uint32_t width, uint32_t height, uint32_t levels = 1) {
    drop_image(t);
    t.format = format, t.width = std::max(1u, width), t.height = std::max(1u, height);
    t.level_count = std::max(1u, levels);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {t.width, t.height, 1};
    ci.mipLevels = t.level_count;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!create_image(ci, t.image, t.memory, false)) {
        logf("vk: fsr3: a %ux%u image (format %d) was not made", t.width, t.height, (int)format);
        t.image = VK_NULL_HANDLE;
        return false;
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.level_count, 0, 1};
    if (vkCreateImageView(dev.device, &vi, nullptr, &t.view) != VK_SUCCESS) return t.view = VK_NULL_HANDLE, false;
    if (t.level_count > 1) {
        for (uint32_t level = 0; level < t.level_count; level++) {
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
            VkImageView v = VK_NULL_HANDLE;
            if (vkCreateImageView(dev.device, &vi, nullptr, &v) != VK_SUCCESS) return false;
            t.levels.push_back(v);
        }
    }
    return true;
}

bool make_pipeline(Pipeline& p, const PassInfo& info) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(info.binding_count + 2);
    for (size_t i = 0; i < info.binding_count; i++) {
        const VkDescriptorType type = info.bindings[i] == T   ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                      : info.bindings[i] == I ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                              : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings.push_back({(uint32_t)i, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }
    // (the two samplers every pass declares, at the bindings the SDK's GLSL fixes them to)
    bindings.push_back({1000, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    bindings.push_back({1001, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = (uint32_t)bindings.size();
    li.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &p.set) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &p.set;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &p.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = info.spv_bytes;
    mi.pCode = info.spv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main",
                nullptr};
    ci.layout = p.layout;
    // (hookshot, a stutter: through the pipeline cache kept across runs. Made with none, FSR 3.1's dozen passes were compiled
    // again at every start, inside the first upscaled frame: a 2.7 s first frame of play against 1.0 s without FSR, 2026-11-10)
    const VkResult made = shared_cache_compute(&ci, &p.pipeline);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (made != VK_SUCCESS) {
        logf("vk: fsr3: the %s pass pipeline was not made (%d)", info.name, (int)made);
        p.pipeline = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool make_samplers() {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.0f, si.maxLod = VK_LOD_CLAMP_NONE;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(dev.device, &si, nullptr, &g.point) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    return vkCreateSampler(dev.device, &si, nullptr, &g.linear) == VK_SUCCESS;
}

/// Every storage format the internal images need. A device missing one cannot run the passes, and finding that out here is
/// better than a failed image allocation in the middle of a frame.
bool formats_supported() {
    static const VkFormat needed[]{
        VK_FORMAT_R8_UNORM,      VK_FORMAT_R16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM,      VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R16_SNORM,
    };
    for (VkFormat f : needed) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, f, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
            !(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            logf("vk: fsr3: not on this GPU: format %d cannot be a storage and sampled image here", (int)f);
            return false;
        }
    }
    return true;
}

/// The uniform buffer the two constant blocks (and the Lanczos table, once) are written into: host visible and mapped, with
/// a set of slots per frame in flight so writing one frame cannot disturb a frame still being read.
bool make_uniforms() {
    const VkDeviceSize align = std::max<VkDeviceSize>(dev.props.limits.minUniformBufferOffsetAlignment, 16);
    const auto round_up = [align](VkDeviceSize n) { return (n + align - 1) / align * align; };
    g.slot_fsr = round_up(sizeof(Constants));
    g.slot_spd = round_up(sizeof(SpdConstants));
    g.slot_lut = round_up(128 * sizeof(int16_t));
    g.frame_stride = g.slot_fsr + g.slot_spd + g.slot_lut + round_up(4 * sizeof(uint32_t));   // (cbRCAS: rcasConfig)
    return create_buffer(g.frame_stride * kUniformFrames, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         true, g.uniforms, g.uniforms_memory, &g.uniforms_mapped);
}

/// Records one pass: its descriptors pushed in the order the pass file numbers them, then the two samplers it declares at
/// 1000 and 1001, then the dispatch, and a barrier after it (every pass reads what the one before it wrote).
void record(VkCommandBuffer cmd, const PassInfo& info, const Pipeline& p, VkSampler point_sampler, VkSampler linear_sampler,
            const Resource* res, size_t count, uint32_t groups_x, uint32_t groups_y) {
    std::vector<VkDescriptorImageInfo> images(count);
    std::vector<VkDescriptorBufferInfo> buffers(count);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(count + 2);
    for (size_t i = 0; i < count; i++) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = (uint32_t)i;
        w.descriptorCount = 1;
        if (info.bindings[i] == U) {
            buffers[i] = {res[i].buffer, res[i].offset, res[i].size};
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo = &buffers[i];
        } else {
            images[i] = {VK_NULL_HANDLE, res[i].view, VK_IMAGE_LAYOUT_GENERAL};
            w.descriptorType = info.bindings[i] == T ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w.pImageInfo = &images[i];
        }
        writes.push_back(w);
    }
    const VkDescriptorImageInfo point{point_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    const VkDescriptorImageInfo linear{linear_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    for (const auto& [binding, info_ptr] : {std::pair<uint32_t, const VkDescriptorImageInfo*>{1000, &point},
                                           std::pair<uint32_t, const VkDescriptorImageInfo*>{1001, &linear}}) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = info_ptr;
        writes.push_back(w);
    }
    const bool timed = gprof_begin(cmd);   // (RX_VK_PASS_TIMING=1: GPU ms by pass name)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, (uint32_t)writes.size(), writes.data());
    vkCmdDispatch(cmd, std::max(1u, groups_x), std::max(1u, groups_y), 1);
    if (timed) gprof_end(cmd, std::string("fsr3 ") + info.name);
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}
void record_pass(VkCommandBuffer cmd, size_t pass, const Resource* res, size_t count, uint32_t groups_x, uint32_t groups_y) {
    record(cmd, PASSES[pass], g.passes[pass], g.point, g.linear, res, count, groups_x, groups_y);
}

std::vector<Image*> all_images() {
    std::vector<Image*> all{&g.farthest_depth, &g.shading_change, &g.new_locks, &g.spd_mips, &g.farthest_depth_mip1,
                            &g.dilated_motion_vectors, &g.dilated_depth, &g.previous_nearest_depth,
                            &g.dilated_reactive_masks, &g.frame_info, &g.spd_atomic, &g.lanczos_lut, &g.default_reactive,
                            &g.output};
    for (size_t i = 0; i < 2; i++) {
        all.push_back(&g.accumulation[i]);
        all.push_back(&g.luma[i]);
        all.push_back(&g.luma_history[i]);
        all.push_back(&g.internal_upscaled[i]);
    }
    return all;
}

/// The pipelines and the internal images for an evaluation at these sizes: made again only for another output, or a frame
/// larger than the images (fsr3_math.h plan_images; dynamic: the game's dynamic resolution is on, the images at the output's
/// size from the start)
bool prepare(uint32_t render_width, uint32_t render_height, uint32_t upscale_width, uint32_t upscale_height, bool dynamic) {
    if (!supported()) return false;
    if (!g.tried) {
        g.tried = true;
        g.ok = make_samplers() && make_uniforms();
        for (size_t i = 0; i < kPassCount && g.ok; i++) {
            const bool needed = g.wave ? (i != P_LUMA_PYRAMID_NOWAVE && i != P_SHADING_CHANGE_PYRAMID_NOWAVE)
                                       : (i != P_LUMA_PYRAMID && i != P_SHADING_CHANGE_PYRAMID);
            if (needed && !sharpen_pass(i)) g.ok = make_pipeline(g.passes[i], PASSES[i]);
        }
        // ((Triforce, S1) FSR's own sharpening: without its two passes FSR 3.1 still draws, and the Sharpness row sharpens
        // at the window as with the game's own upscaler (fsr3::sharpens))
        g.sharpen = g.ok && make_pipeline(g.passes[P_ACCUMULATE_SHARPEN], PASSES[P_ACCUMULATE_SHARPEN]) &&
                    make_pipeline(g.passes[P_RCAS], PASSES[P_RCAS]);
        if (g.ok) logf("vk: fsr3: its passes are ready (%s subgroup operations%s)", g.wave ? "with" : "without",
                       g.sharpen ? ", its own sharpening too" : "; its own sharpening was not made: Sharpness at the window");
    }
    if (!g.ok) return false;
    const fsr3_math::ImagePlan plan =
        fsr3_math::plan_images(g.output.image ? g.render_width : 0, g.output.image ? g.render_height : 0, g.upscale_width,
                               g.upscale_height, render_width, render_height, upscale_width, upscale_height, dynamic);
    if (!plan.remake) return true;

    const uint32_t rw = std::max(1u, plan.width), rh = std::max(1u, plan.height);
    const uint32_t uw = std::max(1u, upscale_width), uh = std::max(1u, upscale_height);
    bool made = true;
    for (size_t i = 0; i < 2 && made; i++) {
        made = made && make_image(g.accumulation[i], VK_FORMAT_R8_UNORM, rw, rh);
        made = made && make_image(g.luma[i], VK_FORMAT_R16_SFLOAT, rw, rh);
        // (the pass files declare the luma history as rgba8; the SDK allocates RGBA16F, which a storage format qualifier
        // does not match, so this allocates what the shaders store)
        made = made && make_image(g.luma_history[i], VK_FORMAT_R8G8B8A8_UNORM, rw, rh);
        made = made && make_image(g.internal_upscaled[i], VK_FORMAT_R16G16B16A16_SFLOAT, uw, uh);
    }
    const uint32_t half_w = std::max(1u, rw / 2), half_h = std::max(1u, rh / 2);
    const uint32_t spd_levels = (uint32_t)std::floor(std::log2((float)std::max(half_w, half_h))) + 1;
    made = made && make_image(g.farthest_depth, VK_FORMAT_R16_SFLOAT, rw, rh);
    made = made && make_image(g.shading_change, VK_FORMAT_R8_UNORM, half_w, half_h);
    made = made && make_image(g.new_locks, VK_FORMAT_R8_UNORM, uw, uh);
    made = made && make_image(g.spd_mips, VK_FORMAT_R16G16_SFLOAT, half_w, half_h, std::max(spd_levels, kSpdStoredMips));
    made = made && make_image(g.farthest_depth_mip1, VK_FORMAT_R16_SFLOAT, half_w, half_h);
    made = made && make_image(g.dilated_motion_vectors, VK_FORMAT_R16G16_SFLOAT, rw, rh);
    made = made && make_image(g.dilated_depth, VK_FORMAT_R32_SFLOAT, rw, rh);
    made = made && make_image(g.previous_nearest_depth, VK_FORMAT_R32_UINT, rw, rh);
    made = made && make_image(g.dilated_reactive_masks, VK_FORMAT_R8G8B8A8_UNORM, rw, rh);
    made = made && make_image(g.frame_info, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1);
    made = made && make_image(g.spd_atomic, VK_FORMAT_R32_UINT, 1, 1);
    made = made && make_image(g.lanczos_lut, VK_FORMAT_R16_SNORM, 128, 1);
    made = made && make_image(g.default_reactive, VK_FORMAT_R8_UNORM, 1, 1);
    made = made && make_image(g.output, VK_FORMAT_R16G16B16A16_SFLOAT, uw, uh);
    g.first_evaluation = true;   // (new images: their first layout and cleared, the history from nothing)
    g.lut_uploaded = false;
    g.parity = 0;
    if (!made) {
        for (Image* t : all_images()) drop_image(*t);
        g.render_width = g.render_height = g.upscale_width = g.upscale_height = 0;
        return false;
    }
    g.render_width = rw, g.render_height = rh, g.upscale_width = uw, g.upscale_height = uh;
    logf("vk: fsr3: %ux%u -> %ux%u (its images for frames up to %ux%u%s)", render_width, render_height, uw, uh, rw, rh,
         dynamic ? ", the game's dynamic resolution" : "");
    return true;
}

} // namespace

void release() {
    if (!g.output.image && g.dead.empty()) return;
    if (g.output.image)
        logf("vk: fsr3: not used for a while (the last at frame %llu): its images let go; the next upscale makes them again",
             (unsigned long long)g.used_frame);
    for (Image* t : all_images()) drop_image(*t);
    g.render_width = g.render_height = g.upscale_width = g.upscale_height = 0;
    g.first_evaluation = true, g.lut_uploaded = false, g.parity = 0;
    free_dead();
}

bool holding() { return g.output.image || !g.dead.empty(); }
void free_unused() { free_dead(); }
uint64_t last_used() { return g.used_frame; }

bool supported() {
    if (!dev.device) return false;   // (asked before the device is made, by the menu: not an answer to keep)
    static const bool answer = [] {
        // (the widest pass pushes thirteen descriptors and the two samplers)
        if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR || dev.max_push_descriptors < 15) {
            logf("vk: fsr3: not on this GPU: no push descriptors, or fewer than 15 (%u)", dev.max_push_descriptors);
            return false;
        }
        if (!dev.features.shaderStorageImageExtendedFormats || !dev.features.shaderStorageImageWriteWithoutFormat) {
            logf("vk: fsr3: not on this GPU: no extended storage image formats, or no storage writes without a format");
            return false;
        }
        if (!formats_supported()) return false;
        VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &sg;
        vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        const VkSubgroupFeatureFlags want = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
        g.wave = (sg.supportedOperations & want) == want && (sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT);
        logf("vk: fsr3: AMD FSR 3.1 can run on this GPU");
        return true;
    }();
    return answer;
}

bool upscale(VkCommandBuffer cmd, const Inputs& in) {
    free_dead();
    if (!in.color || !in.depth || !in.motion) return false;
    if (!prepare(in.render_width, in.render_height, in.upscale_width, in.upscale_height, in.dynamic)) return false;

    const bool first = g.first_evaluation;
    const bool reset = in.reset || first;
    g.first_evaluation = false;
    g.used_frame = texture_frame_no();
    // (this frame's render size, which the game's dynamic resolution changes: the images are made for the largest, max)
    const int32_t rw = (int32_t)std::clamp(in.render_width, 1u, g.render_width);
    const int32_t rh = (int32_t)std::clamp(in.render_height, 1u, g.render_height);
    const int32_t uw = (int32_t)g.upscale_width, uh = (int32_t)g.upscale_height;

    Constants k{};
    k.render_size[0] = rw, k.render_size[1] = rh;
    k.upscale_size[0] = uw, k.upscale_size[1] = uh;
    k.max_render_size[0] = (int32_t)g.render_width, k.max_render_size[1] = (int32_t)g.render_height;
    k.max_upscale_size[0] = uw, k.max_upscale_size[1] = uh;
    for (int i = 0; i < 2; i++) {
        k.previous_render_size[i] = first ? k.render_size[i] : g.previous_render_size[i];
        k.previous_upscale_size[i] = first ? k.upscale_size[i] : g.previous_upscale_size[i];
        k.jitter[i] = in.jitter[i];
        k.previous_jitter[i] = first ? in.jitter[i] : g.previous_jitter[i];
    }
    memcpy(k.device_to_view_depth, in.camera.device_to_view_depth, sizeof k.device_to_view_depth);
    k.tan_half_fov = in.camera.tan_half_fov_x;   // (not read by the 3.1.4 upscaler's passes; set as the SDK sets it)
    k.motion_vector_scale[0] = in.motion_scale[0] / (float)std::max(1, rw);
    k.motion_vector_scale[1] = in.motion_scale[1] / (float)std::max(1, rh);
    k.downscale_factor[0] = (float)rw / (float)std::max(1, uw);
    k.downscale_factor[1] = (float)rh / (float)std::max(1, uh);
    k.jitter_phase_count = (float)std::max(1u, in.jitter_phases);
    k.delta_time = std::clamp(in.frame_time_ms / 1000.0f, 0.0f, 1.0f);
    k.delta_pre_exposure = 1.0f;
    k.view_space_to_meters = 1.0f;
    g.frame_index = reset ? 0.0f : g.frame_index + 1.0f;
    k.frame_index = g.frame_index;
    k.velocity_factor = 1.0f;   // (the SDK's FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_* defaults)
    k.reactiveness_scale = 1.0f;
    k.shading_change_scale = 1.0f;
    k.accumulation_added_per_frame = 1.0f / 3.0f;
    k.min_disocclusion_accumulation = -1.0f / 3.0f;
    for (int i = 0; i < 2; i++) {
        g.previous_render_size[i] = k.render_size[i];
        g.previous_upscale_size[i] = k.upscale_size[i];
        g.previous_jitter[i] = in.jitter[i];
    }

    // (the single-pass downsampler over this frame's render size; its pyramid's levels were made for the images' size)
    const uint32_t spd_x = ((uint32_t)rw - 1) / 64 + 1;
    const uint32_t spd_y = ((uint32_t)rh - 1) / 64 + 1;
    SpdConstants spd{};
    spd.mips = (uint32_t)std::min(std::floor(std::log2((float)std::max(rw, rh))), 12.0f);
    spd.num_work_groups = spd_x * spd_y;
    spd.render_size[0] = (uint32_t)rw, spd.render_size[1] = (uint32_t)rh;

    // This frame's slots, written through the mapped buffer.
    const VkDeviceSize base = (VkDeviceSize)(g.uniform_frame % kUniformFrames) * g.frame_stride;
    g.uniform_frame++;
    uint8_t* const mapped = (uint8_t*)g.uniforms_mapped + base;
    memcpy(mapped, &k, sizeof k);
    memcpy(mapped + g.slot_fsr, &spd, sizeof spd);
    const VkDeviceSize lut_at = g.slot_fsr + g.slot_spd;
    if (!g.lut_uploaded) {   // (the accumulate pass's Lanczos weights, 128 taps as the SDK builds them)
        int16_t weights[128];
        for (size_t i = 0; i < 128; i++) {
            const float x = 2.0f * (float)i / 127.0f;
            const float pi_x = 3.14159265358979f * x;
            const float y = std::abs(x) < FLT_EPSILON ? 1.0f : (std::sin(pi_x) / pi_x) * (std::sin(0.5f * pi_x) / (0.5f * pi_x));
            weights[i] = (int16_t)std::lround(y * 32767.0f);
        }
        memcpy(mapped + lut_at, weights, sizeof weights);
    }
    // ((Triforce, S1) FSR's own sharpening at in.sharpness, AMD's RCAS constants for it: sharpen_rules.h rcas_config)
    const bool sharpen = g.sharpen && in.sharpness > 0.0f;
    const VkDeviceSize rcas_at = lut_at + g.slot_lut;
    if (sharpen) {
        uint32_t con[4];
        sharpen_rules::rcas_config(in.sharpness, con);
        memcpy(mapped + rcas_at, con, sizeof con);
    }
    if (sharpen ? in.sharpness != g.told_sharpness : g.told_sharpness != 0.0f) {
        g.told_sharpness = sharpen ? in.sharpness : 0.0f;
        if (sharpen)
            logf("vk: fsr3: its own sharpening (RCAS) at %.2f, %.2f stops below its most, before the HUD", in.sharpness,
                 sharpen_rules::rcas_stops(in.sharpness));
        else
            logf("vk: fsr3: its own sharpening off%s", g.sharpen ? "" : " (its passes were not made)");
    }
    flush_mapped(g.uniforms_memory, g.frame_stride * kUniformFrames);

    const uint32_t p = g.parity, q = p ^ 1u;

    // What was written before (the inputs: vk_fsr3_inputs.cpp) has to be visible, and new images need their first layout.
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const std::vector<Image*> all = all_images();
    if (first) {
        std::vector<VkImageMemoryBarrier2> to_general;
        to_general.reserve(all.size());
        for (const Image* t : all) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = t->image;
            b.subresourceRange = ALL_LEVELS;
            to_general.push_back(b);
        }
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = (uint32_t)to_general.size();
        di.pImageMemoryBarriers = to_general.data();
        vkCmdPipelineBarrier2(cmd, &di);
        const VkClearColorValue zero{};
        for (const Image* t : all) vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ALL_LEVELS);
    }
    if (!g.lut_uploaded) {
        g.lut_uploaded = true;
        VkBufferImageCopy copy{};
        copy.bufferOffset = base + lut_at;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {128, 1, 1};
        vkCmdCopyBufferToImage(cmd, g.uniforms, g.lanczos_lut.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    }
    // Per frame: the nearest-depth scratch, the SPD counter and its mips; the history on a reset.
    const VkClearColorValue zero{};
    for (const Image* t : {&g.previous_nearest_depth, &g.spd_atomic, &g.spd_mips})
        vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ALL_LEVELS);
    if (reset) {
        vkCmdClearColorImage(cmd, g.accumulation[p].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ALL_LEVELS);
        // (the previous log-luma at 1e8, as the SDK's clearValuesExposure: the luma pyramid takes the first frame's own
        // exposure when the one before is at least resetAutoExposureAverageSmoothing, 1e4. At 1 it eased in from there over
        // about a second after every reset)
        VkClearColorValue info{};
        info.float32[0] = -1.0f, info.float32[1] = 1e8f;
        vkCmdClearColorImage(cmd, g.frame_info.image, VK_IMAGE_LAYOUT_GENERAL, &info, 1, &ALL_LEVELS);
    }
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    const auto tex = [](const Image& t) { return Resource{t.view}; };
    const auto mip = [](uint32_t level) {
        const size_t n = g.spd_mips.levels.size();
        return Resource{g.spd_mips.levels[std::min<size_t>(level, n ? n - 1 : 0)]};
    };
    const Resource color{in.color}, depth{in.depth}, motion{in.motion};
    const Resource exposure = tex(g.frame_info);   // (FSR computes its own exposure)
    const Resource current_luma = tex(g.luma[p]);
    const Resource cb_fsr{VK_NULL_HANDLE, g.uniforms, base, g.slot_fsr};
    const Resource cb_spd{VK_NULL_HANDLE, g.uniforms, base + g.slot_fsr, g.slot_spd};
    const auto groups = [](int32_t size, uint32_t tile) { return ((uint32_t)std::max(1, size) + tile - 1) / tile; };
    const auto run = [&](size_t pass, std::initializer_list<Resource> res, uint32_t gx, uint32_t gy) {
        record_pass(cmd, pass, res.begin(), res.size(), gx, gy);
    };

    run(P_PREPARE_INPUTS, {motion, depth, color, tex(g.dilated_motion_vectors), tex(g.dilated_depth),
                           tex(g.previous_nearest_depth), tex(g.farthest_depth), current_luma, cb_fsr},
        groups(rw, 8), groups(rh, 8));
    run(g.wave ? P_LUMA_PYRAMID : P_LUMA_PYRAMID_NOWAVE,
        {current_luma, tex(g.farthest_depth), tex(g.spd_atomic), tex(g.frame_info), mip(0), mip(1), mip(2), mip(3), mip(4),
         mip(5), tex(g.farthest_depth_mip1), cb_fsr, cb_spd},
        spd_x, spd_y);
    // The two pyramid passes share one atomic counter, and the single-pass downsampler uses it to elect the workgroup that
    // finishes the chain: it counts up to num_work_groups and the last one through does the tail. Cleared only once a frame,
    // the second pass would start at the total, elect no workgroup, and the ones waiting on the tail would spin
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(cmd, g.spd_atomic.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ALL_LEVELS);
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    run(g.wave ? P_SHADING_CHANGE_PYRAMID : P_SHADING_CHANGE_PYRAMID_NOWAVE,
        {current_luma, tex(g.luma[q]), tex(g.dilated_motion_vectors), exposure, tex(g.spd_atomic), mip(0), mip(1), mip(2),
         mip(3), mip(4), mip(5), cb_fsr, cb_spd},
        spd_x, spd_y);
    run(P_SHADING_CHANGE, {tex(g.spd_mips), tex(g.shading_change), cb_fsr}, groups(rw / 2, 8), groups(rh / 2, 8));
    run(P_PREPARE_REACTIVITY,
        {tex(g.previous_nearest_depth), tex(g.dilated_motion_vectors), tex(g.dilated_depth), tex(g.default_reactive),
         tex(g.default_reactive), tex(g.accumulation[p]), tex(g.shading_change), current_luma, exposure,
         tex(g.dilated_reactive_masks), tex(g.new_locks), tex(g.accumulation[q]), cb_fsr},
        groups(rw, 8), groups(rh, 8));
    run(P_LUMA_INSTABILITY,
        {exposure, tex(g.dilated_reactive_masks), tex(g.dilated_motion_vectors), tex(g.frame_info), tex(g.luma_history[p]),
         tex(g.farthest_depth_mip1), current_luma, tex(g.luma_history[q]), tex(g.farthest_depth), cb_fsr},
        groups(rw, 8), groups(rh, 8));
    // ((Triforce, S1) with sharpening, as the SDK: accumulate_sharpen writes the history alone (internal_upscaled[q], the
    // output's binding left unwritten), and RCAS sharpens that into the output, 16 x 16 pixels a group of 64)
    run(sharpen ? P_ACCUMULATE_SHARPEN : P_ACCUMULATE,
        {exposure, tex(g.dilated_reactive_masks), tex(g.dilated_motion_vectors), tex(g.internal_upscaled[p]),
         tex(g.lanczos_lut), tex(g.farthest_depth_mip1), current_luma, tex(g.farthest_depth), color,
         tex(g.internal_upscaled[q]), tex(g.output), tex(g.new_locks), cb_fsr},
        groups(uw, 8), groups(uh, 8));
    if (sharpen) {
        const Resource cb_rcas{VK_NULL_HANDLE, g.uniforms, base + rcas_at, 4 * sizeof(uint32_t)};
        run(P_RCAS, {exposure, tex(g.internal_upscaled[q]), tex(g.output), cb_fsr, cb_rcas}, groups(uw, 16), groups(uh, 16));
    }
    g.parity = q;
    return true;
}

VkImage output_image() { return g.output.image; }
VkImageView output_view() { return g.output.view; }

bool sharpens() { return supported() && (!g.tried || (g.ok && g.sharpen)); }

namespace {
/// ((Triforce, S1) RCAS alone, over another upscaler's picture (fsr3::rcas): its pipeline, samplers, a 1 x 1 exposure of 1
/// and a ring of the two constant blocks it declares. Made at its first use; -1 when it could not be, for the run
struct Lone {
    std::mutex mu;
    std::atomic<int> ok{0};   // (read without the lock by rcas_ok, on the present's thread)
    Pipeline pipe;
    VkSampler point = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
    Image exposure;
    bool exposure_set = false;
    VkBuffer uniforms = VK_NULL_HANDLE;
    VmaAllocation uniforms_memory = nullptr;
    void* mapped = nullptr;
    VkDeviceSize slot_fsr = 0, stride = 0;
    uint32_t at = 0;
};
Lone& lone() {
    static Lone& l = *new Lone;   // (never destroyed, as the chain's: the device outlives the exit)
    return l;
}
bool lone_make(Lone& l) {
    if (!supported()) return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(dev.device, &si, nullptr, &l.point) != VK_SUCCESS) return l.point = VK_NULL_HANDLE, false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(dev.device, &si, nullptr, &l.linear) != VK_SUCCESS) return l.linear = VK_NULL_HANDLE, false;
    const VkDeviceSize align = std::max<VkDeviceSize>(dev.props.limits.minUniformBufferOffsetAlignment, 16);
    const auto round_up = [align](VkDeviceSize n) { return (n + align - 1) / align * align; };
    l.slot_fsr = round_up(sizeof(Constants));   // (cbFSR3Upscaler, declared by the pass and not read by RCAS: zeros)
    l.stride = l.slot_fsr + round_up(4 * sizeof(uint32_t));
    if (!create_buffer(l.stride * kUniformFrames, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, l.uniforms, l.uniforms_memory,
                       &l.mapped))
        return false;
    memset(l.mapped, 0, (size_t)(l.stride * kUniformFrames));
    if (!make_image(l.exposure, VK_FORMAT_R32_SFLOAT, 1, 1)) return false;
    return make_pipeline(l.pipe, PASSES[P_RCAS]);
}
}  // namespace

bool rcas_ok() { return supported() && lone().ok >= 0; }

bool rcas(VkCommandBuffer cmd, VkImageView in, VkImageView out, uint32_t w, uint32_t h, float sharpness) {
    if (!in || !out || !w || !h || sharpness <= 0.0f) return false;
    Lone& l = lone();
    std::lock_guard lk(l.mu);
    if (!l.ok) {
        l.ok = lone_make(l) ? 1 : -1;
        logf("vk: fsr3: AMD's RCAS for another upscaler's picture %s", l.ok > 0 ? "is ready" : "could not be made: Sharpness at "
                                                                                         "the window");
    }
    if (l.ok < 0) return false;
    if (!l.exposure_set) {   // (its first layout, and 1: RCAS multiplies the colour by it and divides it out again)
        l.exposure_set = true;
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = l.exposure.image, b.subresourceRange = ALL_LEVELS;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
        VkClearColorValue one{};
        one.float32[0] = 1.0f;
        vkCmdClearColorImage(cmd, l.exposure.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &ALL_LEVELS);
    }
    const VkDeviceSize base = (VkDeviceSize)(l.at++ % kUniformFrames) * l.stride;
    uint32_t con[4];
    sharpen_rules::rcas_config(sharpness, con);
    memcpy((uint8_t*)l.mapped + base + l.slot_fsr, con, sizeof con);
    flush_mapped(l.uniforms_memory, l.stride * kUniformFrames);
    // (what was written before, the picture and the exposure's clear, visible to the pass)
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    const Resource res[5] = {Resource{l.exposure.view}, Resource{in}, Resource{out},
                             Resource{VK_NULL_HANDLE, l.uniforms, base, sizeof(Constants)},
                             Resource{VK_NULL_HANDLE, l.uniforms, base + l.slot_fsr, 4 * sizeof(uint32_t)}};
    record(cmd, PASSES[P_RCAS], l.pipe, l.point, l.linear, res, 5, (w + 15) / 16, (h + 15) / 16);
    return true;
}

} // namespace mkwvk::fsr3

namespace mkwvk {

namespace {

int env_switch(const char* name) {   // (-1: unset)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}

/// ((Triforce, S1, S2) FSR's own sharpness for a frame of render width rw on an output of ow: the Sharpness row's
/// (temporal_sharpness: 0 with RX_VK_SHARPEN_3D=0), that share of the way to the most below 3/4 of the output's width, NX2navi
/// 385734d's (RX_VK_FSR3_BOOST=0: the row's alone)
float own_sharpness(uint32_t rw, uint32_t ow) {
    static const bool boost = env_switch("RX_VK_FSR3_BOOST") != 0;
    return sharpen_rules::fsr3_sharpness(temporal_sharpness(), rw, ow, boost);
}

/// The network's output from FSR's picture: dlss_out.comp, the same pass DLSS's picture goes through (the colour FSR was
/// given is the network's perceptual encoding, which this squares back to linear colour, as the network's own output does)
struct Out {
    std::mutex mu;
    bool tried = false, tried_hdr = false;
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    /// ((Triforce) RenoDX's HDR, an RGBA16F run) dlss_out.comp's -DHDR layout (dlss_out_hdr_comp_spv.h), the RGBA16F output
    VkPipeline pipe_hdr = VK_NULL_HANDLE;
    VkSampler point = VK_NULL_HANDLE;
    const char* reason = nullptr;
    int reason_repeats = 0;
    bool told_first = false;
    uint64_t drawn = 0;
    // ((Triforce, FSR3-03) the draws drawn again with their own motion (vk_temporal.cpp temporal_fsr3_motion), since the last
    // 600-frame line, and whether the first frame that had any was said)
    uint64_t own_draws = 0, own_frames = 0;
    bool told_own = false;
};
Out& out_state() {
    static Out o;
    return o;
}

/// The output pass for an RGBA16F output (hdr) or the network's B10G11R11 one: the same layout, another shader
bool make_out_pipeline(bool hdr) {
    Out& o = out_state();
    if (hdr) {
        if (o.pipe_hdr) return true;
        if (o.tried_hdr || !make_out_pipeline(false)) return false;
        o.tried_hdr = true;
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof DLSS_OUT_HDR_COMP_SPV, mi.pCode = DLSS_OUT_HDR_COMP_SPV;
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main",
                    nullptr};
        ci.layout = o.layout;
        const VkResult r = shared_cache_compute(&ci, &o.pipe_hdr);
        vkDestroyShaderModule(dev.device, m, nullptr);
        if (r != VK_SUCCESS) return o.pipe_hdr = VK_NULL_HANDLE, false;
        return true;
    }
    if (o.pipe) return true;
    if (o.tried) return false;
    o.tried = true;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &o.set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};   // (dlss_out.comp's size and linear)
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &o.set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &o.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof DLSS_OUT_COMP_SPV, mi.pCode = DLSS_OUT_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main",
                nullptr};
    ci.layout = o.layout;
    const VkResult r = shared_cache_compute(&ci, &o.pipe);   // (kept across runs, above)
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) return o.pipe = VK_NULL_HANDLE, false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev.device, &si, nullptr, &o.point) != VK_SUCCESS) {
        vkDestroyPipeline(dev.device, o.pipe, nullptr);
        return o.pipe = VK_NULL_HANDLE, false;
    }
    return true;
}

/// The network runs: why, when it changes and every 600th time. real: FSR 3.1 was to draw this frame and could not (not its
/// inputs', which say so themselves: no scene camera is no failure), so the host's jitter stops soon (fsr3_inputs::missed)
bool fail(const char* why, bool real = true) {
    if (real) fsr3_inputs::missed();
    Out& o = out_state();
    if (why != o.reason || ++o.reason_repeats % 600 == 0) logf("vk: fsr3: the game's upscale runs (%s)", why);
    if (why != o.reason) o.reason = why, o.reason_repeats = 0;
    return false;
}

/// ((Triforce, FSR3-03) A frame's motion of what moves by itself: n draws drawn again over the camera's (vk_temporal.cpp
/// temporal_fsr3_motion), counted for the 600-frame line; the first frame that had any said. With o.mu held
void own_noted(Out& o, int n, uint64_t frame) {
    if (n <= 0) return;
    o.own_draws += (uint64_t)n, o.own_frames++;
    if (!o.told_own) {
        o.told_own = true;
        logf("vk: fsr3: what moves by itself has motion of its own from frame %llu: %d draws drawn again over the camera's",
             (unsigned long long)frame, n);
    }
}
/// The 600-frame line's part about it, and its counts started again
std::string own_said(Out& o) {
    char b[96];
    snprintf(b, sizeof b, "; own motion in %llu of them, %.0f draws a frame", (unsigned long long)o.own_frames,
             o.own_frames ? (double)o.own_draws / (double)o.own_frames : 0.0);
    o.own_draws = o.own_frames = 0;
    return b;
}

} // namespace

bool fsr3_requested() {
    static const int env = env_switch("RX_VK_FSR");
    if (env == 0) return false;
    // (the Upscaler row's FSR 3.1 or its Native AA, upscaler_rows.h, read each time: the row changes it while the game runs.
    // RX_VK_DLSS=1 / 2 or RX_VK_FSR4=1 / 2 goes over the row, its FSR 3.1 too: vk_temporal.cpp's chain draws then)
    return env > 0 || (upscaler_rows::is_fsr3(opts.dlss) && !temporal_env_chosen());
}

bool fsr3_wanted() {
    // (AMD's own library draws FSR 3.1 through the temporal chain when it is there and works (vk_ffx3.cpp); these passes are the
    // fallback, and what runs without it)
    return fsr3_requested() && !ffx3_usable() && fsr3::supported();
}

bool fsr3_native() {
    static const int env = env_switch("RX_VK_FSR");
    // (the switch decides the size too when it turns FSR 3.1 on: 1 scaled, 2 and up its Native AA, as RX_VK_DLSS and
    // RX_VK_FSR4 count. (linux fork) Whichever draws it: AMD's library through the temporal chain, or the passes here)
    return (fsr3_wanted() || (fsr3_requested() && ffx3_usable())) && (env > 0 ? env >= 2 : opts.dlss == upscaler_rows::FSR3_AA);
}

bool fsr3_pre_wanted() { return fsr3_wanted() && fsr3fi::frames_wanted() == 0; }

bool fsr3_pre(VkCommandBuffer cmd, Texture* scene, VkImageView scene_view, uint32_t ow, uint32_t oh, VkImageView* picture,
              const std::function<int(const fsr3_inputs::Frame&)>& own_motion) {
    if (!fsr3_pre_wanted() || !scene || !scene_view) return false;
    fsr3_inputs::Frame f;
    if (!fsr3_inputs::prepare(cmd, scene, scene_view, f, true)) return fail("its inputs could not be made", false);
    // ((Triforce, FSR3-03) what moves by itself into the motion, over the camera's: the chain's, with this frame's draws)
    const int own = own_motion ? own_motion(f) : 0;
    Out& o = out_state();
    std::lock_guard lk(o.mu);
    own_noted(o, own, f.frame);
    if (ow < f.width || oh < f.height) return fail("unexpected output");
    fsr3::Inputs ai;
    ai.color = f.color, ai.depth = f.depth, ai.motion = f.motion;
    ai.render_width = f.width, ai.render_height = f.height;
    ai.upscale_width = ow, ai.upscale_height = oh;
    ai.jitter[0] = f.jitter[0], ai.jitter[1] = f.jitter[1];
    ai.jitter_phases = fsr3_inputs::jitter_size(2) ? fsr3_inputs::jitter_size(2) : fsr3_math::jitter_phases(f.width, ow);
    ai.dynamic = resolution_auto() && !fsr3_native();
    ai.camera = f.camera;
    ai.frame_time_ms = f.frame_ms;
    ai.reset = f.reset;
    ai.sharpness = own_sharpness(f.width, ow);   // ((Triforce, S1) before the bloom too: the scene's own, the HUD not in it)
    if (!fsr3::upscale(cmd, ai)) return fail("its passes could not be recorded");
    upscale_lod_note(f.width, f.height, ow);   // ((Triforce, S8) the texture LOD bias for the next frames' scene: vk_draw.cpp)
    fsr3_inputs::upscaled(f.width, f.height, ow, oh);
    if (++o.drawn % 600 == 0)
        logf("vk: fsr3: %llu frames drawn, %ux%u -> %ux%u before the bloom, this one's jitter %+.3f %+.3f px of %u phases%s",
             (unsigned long long)o.drawn, f.width, f.height, ow, oh, f.jitter[0], f.jitter[1], ai.jitter_phases,
             own_said(o).c_str());
    if (o.reason) logf("vk: fsr3: FSR 3.1 draws the upscale again (frame %llu)", (unsigned long long)f.frame);
    o.reason = nullptr;
    static bool told;
    if (!told)
        told = true, logf("vk: fsr3 %ux%u -> %ux%u before the game's bloom and tone mapping, from the scene's linear HDR colour "
                          "(jitter %+.3f %+.3f px, %u phases)", f.width, f.height, ow, oh, f.jitter[0], f.jitter[1],
                          ai.jitter_phases);
    *picture = fsr3::output_view();
    return true;
}

bool fsr3_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view, Texture* out,
                  VkImageView out_view, uint32_t out_level) {
    const bool upscaling = fsr3_wanted();
    if (!upscaling && !fsr3_inputs::wanted()) return false;
    if (!in || !in_view || !out || !out_view) return false;
    // (FSR drew before the game's bloom and tone mapping this frame, into this dispatch's input: the tone mapped picture is in
    // the output, or copied there (vk_temporal.cpp). Asked before the scene's camera is, which that pass took)
    if (upscaling && temporal_pre_dispatch(cmd, in, in_view, out, out_view, out_level, false)) return true;
    // ((Triforce) the output's size, for which scenes FSR's inputs keep a camera of: fsr3_inputs::note_scene_draw. Of a
    // known network, or another in the run's scene format that scales up, as temporal_upscale notes them, before the scene is
    // asked for: a scene of FSR's Ultra Performance in the handheld size (512x288) has a camera only once the output is known)
    if (temporal_scene_format(out->format) &&
        (known || (temporal_scene_format(in->format) && out->format == in->format && (out->width >> out_level) > in->width)))
        fsr3_inputs::note_output(std::max(1u, out->width >> out_level), std::max(1u, out->height >> out_level));
    // (another network of the upscale's shape, as temporal_upscale takes them: only the scene into the picture. The gameplay
    // networks sample the scene's B10G11R11 colour and write the B10G11R11 picture)
    // ((Triforce, 11-16) in the run's scene format: an RGBA16F run's (RenoDX's HDR) networks sample the RGBA16F scene and write an
    // RGBA16F picture, as the owner's log showed ("2208x932x1/97 4416x1864x1/97"); with B10G11R11 alone frame generation's
    // inputs were never made there and DLSS frame generation stayed paused)
    if (!known && (!temporal_scene_format(in->format) || out->format != in->format ||
                   (out->width >> out_level) <= in->width || !fsr3_inputs::is_scene(in)))
        return false;
    // (it did not: the point learnt for the next frame)
    if (upscaling && temporal_pre_dispatch(cmd, in, in_view, out, out_view, out_level, true)) return true;
    // ((Triforce) an RGBA16F run's network (RenoDX's HDR Native): it samples the RGBA16F scene, the square root of linear HDR
    // colour, over 1 where the scene is brighter (DLSS squares it: dlss_prep_hdr), and writes the RGBA16F picture. FSR is
    // given that colour unclamped (its passes are compiled for HDR colour) and its picture goes into the RGBA16F output
    // squared, through the output pass's -DHDR layout. It was left to the game's upscale here before, the menu saying FSR 3.1)
    const bool hdr = in->format == VK_FORMAT_R16G16B16A16_SFLOAT;
    // ((Triforce, FG-4) frame generation's inputs made already this frame: by the upscaler chain before the bloom, from the
    // scene, with what moves by itself (vk_temporal.cpp fg_inputs_from_chain). Not made again here with the camera's alone)
    if (!upscaling && fsr3_inputs::made_this_frame()) return false;
    fsr3_inputs::Frame f;
    if (!fsr3_inputs::prepare(cmd, in, in_view, f, hdr)) {
        if (upscaling) (void)temporal_fsr3_motion(cmd, nullptr);   // ((Triforce) this frame's stored draws let go)
        return upscaling ? fail("its inputs could not be made", false) : false;
    }
    if (!upscaling) return false;   // (frame interpolation's inputs alone: the network or DLSS upscales)
    // ((Triforce, FSR3-03) what moves by itself into the motion, over the camera's: the chain's redraws, with this frame's
    // draws that stored their positions. Before, FSR 3.1 had the camera's motion alone, and Link and the others trailed)
    const int own = temporal_fsr3_motion(cmd, &f);
    Out& o = out_state();
    std::lock_guard lk(o.mu);
    own_noted(o, own, f.frame);
    const uint32_t ow = std::max(1u, out->width >> out_level), oh = std::max(1u, out->height >> out_level);
    const bool out_ok = hdr ? out->format == VK_FORMAT_R16G16B16A16_SFLOAT && temporal_scene_format(in->format)
                            : out->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    if (!out_ok || ow < f.width || oh < f.height) return fail("unexpected output");
    if (!make_out_pipeline(hdr)) return fail("its output pass was not made");
    struct Disturbed {   // (from here commands are recorded: the program's descriptor pushes are pushed again after)
        ~Disturbed() { descriptors_disturbed(); }
    } disturbed;
    fsr3::Inputs ai;
    ai.color = f.color, ai.depth = f.depth, ai.motion = f.motion;
    ai.render_width = f.width, ai.render_height = f.height;
    ai.upscale_width = ow, ai.upscale_height = oh;
    ai.jitter[0] = f.jitter[0], ai.jitter[1] = f.jitter[1];
    ai.jitter_phases = fsr3_inputs::jitter_size(2) ? fsr3_inputs::jitter_size(2) : fsr3_math::jitter_phases(f.width, ow);
    ai.dynamic = resolution_auto() && !fsr3_native();
    ai.camera = f.camera;
    ai.frame_time_ms = f.frame_ms;
    ai.reset = f.reset;
    ai.sharpness = own_sharpness(f.width, ow);   // ((Triforce, S1) into the picture before the HUD is drawn over it)
    if (!fsr3::upscale(cmd, ai)) return fail("its passes could not be recorded");
    upscale_lod_note(f.width, f.height, ow);   // ((Triforce, S8) the texture LOD bias for the next frames' scene: vk_draw.cpp)
    {
        // (linear: 0 the plain pass squares into B10G11R11; 2 the -DHDR one squares into RGBA16F, dlss_out.comp)
        const int32_t pc[3] = {(int32_t)ow, (int32_t)oh, hdr ? 2 : 0};
        const VkDescriptorImageInfo info[2] = {{o.point, fsr3::output_view(), VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, out_view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &info[0];
        w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &info[1];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hdr ? o.pipe_hdr : o.pipe);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, o.layout, 0, 2, w);
        vkCmdPushConstants(cmd, o.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cmd, (ow + 7) / 8, (oh + 7) / 8, 1);
    }
    full_barrier(cmd);
    fsr3_inputs::upscaled(f.width, f.height, ow, oh);
    hudless::note(cmd, out, out_level);   // (the picture without the HUD, for NVIDIA DLSS Frame Generation: vk_ngxfg.h)
    // (every 600 frames: that it still draws, and the jitter it was given, which says the host's jitter is on)
    if (++o.drawn % 600 == 0)
        logf("vk: fsr3: %llu frames drawn, %ux%u -> %ux%u, this one's jitter %+.3f %+.3f px of %u phases%s", (unsigned long long)o.drawn,
             f.width, f.height, ow, oh, f.jitter[0], f.jitter[1], ai.jitter_phases, own_said(o).c_str());
    if (o.reason) logf("vk: fsr3: FSR 3.1 draws the upscale again (frame %llu)", (unsigned long long)f.frame);
    o.reason = nullptr;
    if (!o.told_first) {
        o.told_first = true;
        logf("vk: fsr3 %ux%u -> %ux%u with assembled depth and motion (network %016llx; jitter %+.3f %+.3f px, "
             "%u phases%s)",
             f.width, f.height, ow, oh, (unsigned long long)key, f.jitter[0], f.jitter[1], ai.jitter_phases,
             hdr ? "; an RGBA16F scene, HDR colour" : "");
    }
    return true;
}

bool fsr3_idle() {
    bool released = false;
    {   // (FSR 3.1's own images: its evaluations run under the output pass's lock, as this does)
        Out& o = out_state();
        std::lock_guard lk(o.mu);
        if (fsr3::holding()) {
            const uint64_t now = texture_frame_no(), last = fsr3::last_used();
            // (kept while the row still says FSR 3.1: each area's loading screen has no upscale for 2 s and more, and the images
            // made again at the next area's first frame would be a stall there, as vk_temporal.cpp's temporal_idle keeps DLSS's)
            if (!fsr3_wanted() && now - last > 120) {
                released = fsr3::output_view() != VK_NULL_HANDLE;
                fsr3::release();
            } else {
                fsr3::free_unused();
            }
        }
    }
    fsr3_inputs::release_idle();
    return released;
}

} // namespace mkwvk
