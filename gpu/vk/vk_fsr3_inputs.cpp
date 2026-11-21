// (hookshot) AMD FSR 3.1's inputs: the scene's camera and depth, made into colour, depth and motion at the render size by
// one pass (shaders/fsr3_prep.comp); see vk_fsr3_inputs.h for what comes from #841 and what does not yet.
//
// The camera is taken exactly as vk_temporal.cpp's temporal_note_scene_draw takes it, from the draws into the scene's
// colour target: their vertex stage's uniform buffer 3 holds the target's size at 0x120, this frame's view-projection at 0x30 (row major,
// clip = M * position) and the projection at 0x70, its w row (0, 0, -1, 0) at 0xa0. The jitter the host put on the
// projection (nx2/oot_jitter.py: P[0][2] and P[1][2]) is read back from it, given to FSR in render pixels and taken off the
// motion, with dlss_math.h's functions: the same jitter, read the same way, as DLSS's.
#include "vk_fsr3_inputs.h"
#include "vk_dlssg.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "dlss_checks.h"     // ((Triforce) scene_size_fits: the scenes the gameplay upscale's output can come from)
#include "dlss_math.h"
#include "framegen_rows.h"   // ((Triforce) pose_of, camera_cut: a camera cut starts a new history)
#include "vk_draw.h"   // (descriptors_disturbed)
#include "vk_fsr3.h"
#include "vk_fsr3fi.h"
#include "vk_ngxfg.h"
#include "vk_ffx_fg.h"   // (AMD's frame generation keeps the scene's inputs too)
#include "vk_fsr4.h"   // (Triforce: fsr4fg, AMD FSR 4's frame generation takes these inputs too)
#include "vk_texture.h"
#include "vk_temporal.h"   // ((Triforce) temporal_scene_format: the run's scene format, B10G11R11 or RenoDX's RGBA16F)

#include "shaders/fsr3_prep_comp_spv.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
bool present_held_last();   // ((Triforce) vk_replay.cpp: the last present held its frame, the hold of the last picture)
}

namespace mkwvk::fsr3_inputs {
namespace {

int env_switch(const char* name) {   // (-1: unset)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}

/// ((Triforce) the frames FSR 3.1 may fail to draw in a row before the host's jitter stops: jitter_size)
constexpr uint64_t kMissedFrames = 3;

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageUsageFlags usage = 0;
};

struct Camera {
    float view[12]{};         // (its view, 3 x 4: the buffer's first 0x30 bytes; the camera's position and basis for frame generation)
    float current[16]{};      // (its view-projection, jittered)
    float projection[16]{};   // (its projection, jittered: the near plane and the field of view come from it)
    float jitter[2]{};        // (the projection's offset of the picture's centre, in clip space: nx2/oot_jitter.py)
    uint32_t width = 0, height = 0;
    Texture* depth = nullptr;
    // (the depth texture's creation number and texture_destroyed_count when taken: destroyed since, it is not used. It was
    // texture_destroy_generation, which any texture's end changes: with the video memory full the cache lets textures go
    // in most frames, and each of those frames was left to the game's own upscale)
    uint32_t depth_id = 0;
    uint64_t noted = 0;
    uint64_t frame = 0;       // (texture_frame_no when taken)
    bool valid = false;
};

struct State {
    std::mutex mu;
    Camera cam;               // (this frame's, since the last prepare)
    Image color, depth, motion;
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkSampler point = VK_NULL_HANDLE;
    bool tried = false;
    std::unordered_map<VkImage, VkImageView> depth_views;
    uint64_t depth_views_gen = 0;
    float last_vp[16]{};      // (the frame before's view-projection, and its jitter)
    float last_jitter[2]{};
    float last_pos[3]{}, last_fwd[3]{};   // (the frame before's camera position and direction: dlssg_math::is_cut)
    bool last_cam_ok = false;
    uint64_t last_frame = 0;  // (the frame prepare last ran, 0: none)
    std::chrono::steady_clock::time_point last_time{};
    Frame frame;              // (the last one prepared: take())
    bool have_frame = false;
    std::atomic<uint64_t> upscaled_frame{0};   // (the frame FSR's upscaler last drew: jitter_size, on the game's thread)
    std::atomic<uint32_t> jitter_w{0}, jitter_h{0}, jitter_phases{8};
    // ((Triforce) the first frame of a run of FSR 3.1's failures since it last drew (missed), 0: none; jitter_size)
    std::atomic<uint64_t> missed_from{0};
    // ((Triforce) the gameplay upscale's output size, as last seen (note_output): which scenes keep a camera)
    std::atomic<uint32_t> out_w{0}, out_h{0};
    // ((Triforce) camera cuts: the pose of the frame before (framegen_rows.h pose_of), and how many were taken)
    framegen_rows::Pose last_pose;
    bool have_pose = false;
    uint64_t cuts = 0;
    uint64_t used_frame = 0;   // (the frame prepare last ran or wanted() last held: release_idle)
    struct Dead { uint64_t frame; std::function<void()> free; };
    std::vector<Dead> dead;   // (what work in flight may still use: freed a few frames on)
    const char* reason = nullptr;
    int reason_repeats = 0;
    bool told = false;
    bool told_chain = false;   // ((Triforce, FG-4) chain_motion's first frame said)
};
State& st() {
    static State s;
    return s;
}

constexpr const char* kNoCamera = "no scene camera this frame";

/// Why, when it changes and every 600th time. Anything but a frame without the scene's camera (a menu, the pause screen: no
/// scene drawn) is FSR 3.1 failing to draw a frame it was to draw: the host's jitter stops soon (missed)
bool fail(const char* why) {
    State& s = st();
    if (why != kNoCamera) missed();
    if (why != s.reason || ++s.reason_repeats % 600 == 0) logf("vk: fsr3 inputs: not made this frame (%s)", why);
    if (why != s.reason) s.reason = why, s.reason_repeats = 0;
    return false;
}

void later(std::function<void()> f) { st().dead.push_back({texture_frame_no(), std::move(f)}); }
void free_dead() {
    State& s = st();
    const uint64_t now = texture_frame_no();
    for (size_t i = 0; i < s.dead.size();) {
        if (now - s.dead[i].frame > 4) {
            s.dead[i].free();
            s.dead[i] = std::move(s.dead.back());
            s.dead.pop_back();
        } else {
            i++;
        }
    }
}

/// extra: usage besides storage and sampling ((Triforce) the motion's: a colour attachment, which the chain's redraws of
/// what moves by itself draw into, vk_temporal.cpp temporal_fsr3_motion)
bool make_image(VkCommandBuffer cmd, Image& im, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags extra = 0) {
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra;
    if (im.image && im.format == format && im.width == w && im.height == h && im.usage == usage) return true;
    if (im.image) {
        const Image old = im;
        later([old] {
            vkDestroyImageView(dev.device, old.view, nullptr);
            destroy_image(old.image, old.memory);
        });
        im = {};
    }
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D, ic.format = format, ic.extent = {w, h, 1}, ic.mipLevels = 1, ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = usage;
    if (!create_image(ic, im.image, im.memory)) return im = {}, false;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = im.image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device, &vc, nullptr, &im.view) != VK_SUCCESS) {
        destroy_image(im.image, im.memory);
        return im = {}, false;
    }
    im.format = format, im.width = w, im.height = h, im.usage = usage;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};   // (UNDEFINED -> GENERAL, once)
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image, b.subresourceRange = vc.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    return true;
}

/// fsr3_prep.comp's pipeline, once: two sampled inputs, three storage outputs, dlss_prep.comp's push constants
bool make_pipeline() {
    State& s = st();
    if (s.pipe) return true;
    if (s.tried || !dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    s.tried = true;
    VkDescriptorSetLayoutBinding b[5]{};
    for (uint32_t i = 0; i < 5; i++)
        b[i] = {i, i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 5, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &s.set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 84};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &s.set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &s.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof FSR3_PREP_COMP_SPV, mi.pCode = FSR3_PREP_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main",
                nullptr};
    ci.layout = s.layout;
    const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &s.pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) {
        logf("vk: fsr3 inputs: their pipeline was not made (%d)", (int)r);
        return s.pipe = VK_NULL_HANDLE, false;
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev.device, &si, nullptr, &s.point) != VK_SUCCESS) {
        vkDestroyPipeline(dev.device, s.pipe, nullptr);
        return s.pipe = VK_NULL_HANDLE, false;
    }
    return true;
}

/// A view of a depth texture's depth alone, for sampling (made once per image: vk_temporal.cpp's depth_view)
VkImageView depth_view(Texture* t) {
    State& s = st();
    const uint64_t gen = texture_destroy_generation();
    if (gen != s.depth_views_gen) {   // (a texture was destroyed: its image's view may be stale)
        auto old = std::move(s.depth_views);
        s.depth_views.clear();
        later([old] {
            for (const auto& [image, view] : old) vkDestroyImageView(dev.device, view, nullptr);
        });
        s.depth_views_gen = gen;
    }
    if (auto it = s.depth_views.find(t->image); it != s.depth_views.end()) return it->second;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = t->image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = t->format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &vc, nullptr, &v) != VK_SUCCESS) v = VK_NULL_HANDLE;
    s.depth_views[t->image] = v;
    return v;
}

} // namespace

// (Triforce: or NVIDIA DLSS frame generation, through NGX here (vk_ngxfg.h) or Streamline on Windows (vk_dlssg.cpp), which take the
// same depth and motion; AMD's frame generation through its library (vk_ffx_fg.h); or AMD FSR 4's frame generation, Windows only: vk_fsr4.h fsr4fg)
bool wanted() {
    return fsr3_wanted() || fsr3fi::frames_wanted() != 0 || ngxfg::inputs_wanted() || ffxfg::inputs_wanted() || dlssg::active() ||
           fsr4fg::frames_wanted();
}

void note_output(uint32_t width, uint32_t height) {
    State& s = st();
    s.out_w.store(width, std::memory_order_relaxed), s.out_h.store(height, std::memory_order_relaxed);
}

bool scene_size(uint32_t width, uint32_t height) {
    // ((Triforce) 640x360 at least, as before, or the chain's rule (vk_temporal.cpp temporal_scene_size) once the output is
    // seen: 1 to 3 times smaller than it, so FSR's Ultra Performance in the handheld size (512x288 for 1536x864) has its
    // camera. Either: an output whose height is no multiple of 3 (1600) draws 533 lines at a third, which the chain's rule
    // alone would turn away. A smaller target with a camera (the pause screen's model views, a reflection) is still not the
    // scene: the largest of the frame is, and the upscale's input must be it)
    if (width >= 640 && height >= 360) return true;
    const State& s = st();
    const uint32_t ow = s.out_w.load(std::memory_order_relaxed), oh = s.out_h.load(std::memory_order_relaxed);
    return dlss_checks::scene_size_fits(width, height, ow, oh);
}

void note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,
                     uint64_t camera_size) {
    if (!color0 || !depth || !camera || camera_size < 0x230 || !scene_size(width, height)) return;
    // ((Triforce, 11-16: DLSS frame generation never made a frame for the owner, HDR Native with RenoDX) the scene's target in
    // the format this run draws it in, as vk_temporal.cpp's note takes it: B10G11R11, or RGBA16F where the run's scene is
    // (RenoDX's HDR, host/nn_vi.c hos_scene_rgba16f). With B10G11R11 alone no camera was kept in an RGBA16F run, so frame
    // generation's inputs were never made and it stayed paused)
    if (!temporal_scene_format(color0->format) || !wanted()) return;
    State& s = st();
    std::lock_guard lk(s.mu);
    const uint64_t now = texture_frame_no();
    if (s.cam.valid && s.cam.frame == now && (uint64_t)width * height <= (uint64_t)s.cam.width * s.cam.height)
        return;   // (the largest one of the frame wins)
    float c[0x230 / 4];
    if (!memory_read_as_submitted(camera, c, sizeof c)) return;
    // (the target's size at 0x120; the projection at 0x70, its w row (0, 0, -1, 0) at 0xa0)
    if (c[0x120 / 4] != (float)width || c[0x124 / 4] != (float)height || c[0xa0 / 4] != 0.0f || c[0xa4 / 4] != 0.0f ||
        c[0xa8 / 4] != -1.0f || c[0xac / 4] != 0.0f)
        return;
    Camera& k = s.cam;
    std::memcpy(k.view, c, sizeof k.view);
    std::memcpy(k.current, c + 0x30 / 4, sizeof k.current);
    std::memcpy(k.projection, c + 0x70 / 4, sizeof k.projection);
    // (the offset sead's projection gave the frustum, in its third column: 2 * offset, which moves the picture by -that)
    k.jitter[0] = -c[0x78 / 4], k.jitter[1] = -c[0x88 / 4];
    k.width = width, k.height = height, k.depth = depth, k.depth_id = depth->id, k.noted = texture_destroyed_count(), k.frame = now;
    k.valid = true;
    upscale_lod_scene(width, height);   // ((Triforce, S8) this frame's scene size for the texture LOD bias: vk_draw.cpp)
}

bool is_scene(const Texture* in) {
    State& s = st();
    std::lock_guard lk(s.mu);
    return in && s.cam.valid && texture_frame_no() - s.cam.frame <= 1 && in->width == s.cam.width &&
           in->height == s.cam.height;
}

bool prepare(VkCommandBuffer cmd, Texture* in, VkImageView in_view, Frame& out, bool linear) {
    State& s = st();
    std::lock_guard lk(s.mu);
    free_dead();
    const Camera cam = s.cam;
    s.cam.valid = false;   // (each frame's own camera)
    if (!in || !in_view) return fail("no input view");
    const uint64_t now = texture_frame_no();
    if (!cam.valid || now - cam.frame > 1) return fail(kNoCamera);
    if (texture_destroyed_since(cam.depth_id, cam.noted)) return fail("the scene depth is gone");
    const uint32_t rw = in->width, rh = in->height;
    if (rw != cam.width || rh != cam.height) return fail("the input is not the scene's size");
    if (cam.depth->width != rw || cam.depth->height != rh || !(cam.depth->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        cam.depth->samples != VK_SAMPLE_COUNT_1_BIT || !(cam.depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        return fail("unexpected scene depth");
    // (the projection without this frame's jitter: the field of view and the depth range do not move with it)
    float projection[16];
    std::memcpy(projection, cam.projection, sizeof projection);
    dlss_math::unjitter(projection, cam.jitter);
    fsr3_math::Camera camera;
    if (!fsr3_math::camera_from_projection(projection, camera)) return fail("the scene's projection is not one FSR takes");
    if (!make_pipeline()) return fail("its pipeline was not made");
    const VkImageView dview = depth_view(cam.depth);
    if (!dview) return fail("no view of the scene depth");
    struct Disturbed {   // (from here commands are recorded: the program's descriptor pushes are pushed again after)
        ~Disturbed() { descriptors_disturbed(); }
    } disturbed;
    // ((Triforce, FG-4) the motion a copy's destination too: the chain's, with what moves by itself, copied over it: chain_motion)
    if (!make_image(cmd, s.color, VK_FORMAT_R16G16B16A16_SFLOAT, rw, rh) || !make_image(cmd, s.depth, VK_FORMAT_R32_SFLOAT, rw, rh) ||
        !make_image(cmd, s.motion, VK_FORMAT_R16G16_SFLOAT, rw, rh, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return fail("its images were not made");

    // ((Triforce) a new history also after a present that held its frame, as vk_temporal.cpp's chain starts one: what was drawn
    // while new textures were still owed (the dummy in their place) is not carried into the picture shown after the hold; and
    // at a camera cut (framegen_rows.h kUpscalerCut: NX2navi's 2495391), the time measured since the frame before's camera
    // (the CPU's, never taken under one 60th there: frames recorded back to back after a stall are no cut).
    // Not at another render size: the game's dynamic resolution changes it, and the reprojection is in clip space, which
    // does not depend on it (FSR's own history is kept across it: vk_fsr3.cpp)
    const auto t = std::chrono::steady_clock::now();
    const framegen_rows::Pose pose = framegen_rows::pose_of(cam.view);
    const bool gap = !s.last_frame || now - s.last_frame > 2;
    const bool held = !gap && present_held_last();
    bool cut = false;
    if (!gap && !held && s.have_pose && s.last_time.time_since_epoch().count()) {
        const float dt = (float)std::chrono::duration<double, std::milli>(t - s.last_time).count();
        cut = framegen_rows::camera_cut(s.last_pose, pose, dt, framegen_rows::kUpscalerCut);
        if (cut && s.cuts++ < 24) {
            float d2 = 0.0f;
            for (int i = 0; i < 3; i++) d2 += (pose.pos[i] - s.last_pose.pos[i]) * (pose.pos[i] - s.last_pose.pos[i]);
            logf("vk: fsr3 inputs: frame %llu: a camera cut (moved %.2f m in %.1f ms): a new history (%llu so far)",
                 (unsigned long long)now, std::sqrt(d2), dt, (unsigned long long)s.cuts);
        }
    }
    s.last_pose = pose, s.have_pose = true;
    const bool reset = gap || held || cut;
    struct {
        float reproject[16];
        float jitter[2];
        int32_t size[2];
        int32_t flags;
    } pc{};
    static_assert(sizeof pc == 84, "fsr3_prep.comp's push constants");
    // (the motion without the jitter: from this frame's jittered clip space to the frame before's without its jitter. A jitter
    // moves clip x by jitter * w: taken off the previous matrix's first two rows)
    float previous[16];
    std::memcpy(previous, s.last_vp, sizeof previous);
    dlss_math::unjitter(previous, s.last_jitter);
    const bool motion = !reset && dlss_math::reprojection(cam.current, previous, pc.reproject);
    pc.jitter[0] = cam.jitter[0], pc.jitter[1] = cam.jitter[1];
    pc.size[0] = (int32_t)rw, pc.size[1] = (int32_t)rh;
    pc.flags = (motion ? 1 : 0) | (camera.flip_depth ? 2 : 0) | (linear ? 4 : 0);

    full_barrier(cmd);   // (the scene's colour and depth, written before)
    {
        const VkDescriptorImageInfo info[5] = {{s.point, in_view, VK_IMAGE_LAYOUT_GENERAL},
                                               {s.point, dview, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.color.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.depth.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.motion.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[5]{};
        for (uint32_t i = 0; i < 5; i++) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstBinding = i, w[i].descriptorCount = 1, w[i].pImageInfo = &info[i];
            w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipe);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 5, w);
        vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, 1);
    }
    full_barrier(cmd);

    Frame f;
    f.color = s.color.view, f.depth = s.depth.view, f.motion = s.motion.view;
    f.width = rw, f.height = rh;
    dlss_math::jitter_to_pixels(cam.jitter, rw, rh, f.jitter);   // (in render pixels, x right and y down: FSR's, as DLSS's)
    f.camera = camera;
    f.reset = reset;
    f.depth_image = s.depth.image, f.motion_image = s.motion.image;
    if (ngxfg::inputs_wanted() || ffxfg::inputs_wanted()) {   // (and AMD's frame generation takes the camera's basis as well)
        // (NVIDIA DLSS Frame Generation's camera: its matrices carry no jitter, which it is given on its own: both frames'
        // view-projections unjittered, where the motion above takes this frame's jittered, as the depth it was drawn with is)
        float current[16], before[16];
        std::memcpy(current, cam.current, sizeof current), std::memcpy(before, s.last_vp, sizeof before);
        dlss_math::unjitter(current, cam.jitter), dlss_math::unjitter(before, s.last_jitter);
        f.dlssg_ok = dlssg_math::camera(cam.view, projection, current, before, !reset, f.dlssg);
        if (f.dlssg_ok && s.last_cam_ok && !reset) {
            const float frames = s.last_time.time_since_epoch().count()
                                     ? (float)(std::chrono::duration<double, std::milli>(t - s.last_time).count() / 16.667)
                                     : 1.0f;
            f.cut = dlssg_math::is_cut(s.last_pos, s.last_fwd, f.dlssg.pos, f.dlssg.fwd, frames);
        }
        s.last_cam_ok = f.dlssg_ok;
        if (f.dlssg_ok)
            std::memcpy(s.last_pos, f.dlssg.pos, sizeof s.last_pos), std::memcpy(s.last_fwd, f.dlssg.fwd, sizeof s.last_fwd);
    } else {
        s.last_cam_ok = false;
    }
    {   // (Triforce, NVIDIA DLSS frame generation's: the images, and the matrices with no jitter in them on either side)
        f.depth_image = s.depth.image, f.motion_image = s.motion.image;
        std::memcpy(f.projection, projection, sizeof f.projection);
        std::memcpy(f.view, cam.view, sizeof f.view);
        float current[16];
        std::memcpy(current, cam.current, sizeof current);
        dlss_math::unjitter(current, cam.jitter);
        if (!motion || !dlss_math::reprojection(current, previous, f.clip_to_prev))
            for (int i = 0; i < 16; i++) f.clip_to_prev[i] = i % 5 == 0 ? 1.0f : 0.0f;
    }
    {   // ((Triforce, FSR3-03) the chain's redraws of what moves by itself: the camera as drawn, and the frame before's)
        Frame::Own& o = f.own;
        std::memcpy(o.current, cam.current, sizeof o.current);
        std::memcpy(o.projection, cam.projection, sizeof o.projection);
        o.jitter[0] = cam.jitter[0], o.jitter[1] = cam.jitter[1];
        std::memcpy(o.before, previous, sizeof o.before);
        o.jitter_before[0] = s.last_jitter[0], o.jitter_before[1] = s.last_jitter[1];
        std::memcpy(o.reproject, pc.reproject, sizeof o.reproject);
        o.depth = cam.depth;
        o.last = s.last_frame;   // (before it is this frame's, below)
        o.history = motion;
    }
    f.frame_ms = s.last_time.time_since_epoch().count() && !reset
                     ? (float)std::min(250.0, std::chrono::duration<double, std::milli>(t - s.last_time).count())
                     : 16.667f;
    f.frame = now;
    s.frame = f, s.have_frame = true;
    s.last_time = t;
    s.last_frame = now;
    std::memcpy(s.last_vp, cam.current, sizeof s.last_vp);
    s.last_jitter[0] = cam.jitter[0], s.last_jitter[1] = cam.jitter[1];
    if (s.reason) logf("vk: fsr3 inputs: made again (frame %llu)", (unsigned long long)now);
    s.reason = nullptr;
    if (!s.told) {
        s.told = true;
        // (what the projection says, once: the planes and the field of view FSR's depth factors are made of)
        logf("vk: fsr3 inputs: %ux%u, from the scene's projection: near %.4f far %.6g fov %.2f deg, depth %s (p22 %.6g p23 "
             "%.6g); camera motion from the depth, jitter %+.3f %+.3f px",
             rw, rh, camera.near_plane, camera.far_plane, camera.fov_y * 57.2957795, camera.flip_depth ? "0 at the near plane: "
             "reversed for FSR" : "reversed already", projection[10], projection[11], f.jitter[0], f.jitter[1]);
    }
    out = f;
    return true;
}

bool made_this_frame() {
    State& s = st();
    std::lock_guard lk(s.mu);
    return s.have_frame && s.frame.frame == texture_frame_no();
}

bool chain_motion(VkCommandBuffer cmd, VkImage motion, uint32_t width, uint32_t height) {
    // (RX_VK_FG_OWN_MOTION=0: the camera's motion alone, as before, for comparing)
    static const bool off = env_switch("RX_VK_FG_OWN_MOTION") == 0;
    State& s = st();
    std::lock_guard lk(s.mu);
    if (off || !cmd || !motion || !s.have_frame || s.frame.frame != texture_frame_no() || !s.motion.image ||
        s.frame.width != width || s.frame.height != height || s.motion.width < width || s.motion.height < height)
        return false;
    full_barrier(cmd);   // (the chain's redraws into its motion, and prepare's pass into this one, before the copy)
    const VkImageCopy c{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                        {width, height, 1}};
    vkCmdCopyImage(cmd, motion, VK_IMAGE_LAYOUT_GENERAL, s.motion.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(cmd);
    s.frame.chain_motion = true;
    if (!s.told_chain) {
        s.told_chain = true;
        logf("vk: fsr3 inputs: frame generation's motion is the upscaler chain's from frame %llu, what moves by itself in it "
             "(%ux%u; RX_VK_FG_OWN_MOTION=0: the camera's alone)", (unsigned long long)s.frame.frame, width, height);
    }
    return true;
}

bool take(Frame& out) {
    State& s = st();
    std::lock_guard lk(s.mu);
    const bool have = s.have_frame && texture_frame_no() - s.frame.frame <= 1;
    s.have_frame = false;
    if (have) out = s.frame;
    return have;
}

void upscaled(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh) {
    State& s = st();
    (void)dh;   // ((Triforce) AMD's rule is the width's: fsr3_math.h jitter_phases, 71 or 72 at Ultra Performance, not 64)
    s.jitter_w = rw, s.jitter_h = rh, s.jitter_phases = fsr3_math::jitter_phases(rw, dw);
    s.missed_from.store(0, std::memory_order_relaxed);
    s.upscaled_frame = texture_frame_no();
}

void missed() {
    State& s = st();
    uint64_t none = 0;
    const uint64_t now = texture_frame_no();
    s.missed_from.compare_exchange_strong(none, now ? now : 1, std::memory_order_relaxed);
}

uint32_t jitter_size(int axis) {
    // (RX_VK_FSR_JITTER=0: none. 0 too once FSR has not drawn for a while: the camera as the game has it)
    static const bool off = env_switch("RX_VK_FSR_JITTER") == 0;
    State& s = st();
    const uint64_t last = s.upscaled_frame.load(std::memory_order_relaxed), now = texture_frame_no();
    if (off || !last || now - last > 30 || !fsr3_wanted()) return 0;
    // ((Triforce) and once FSR 3.1 failed to draw the frames it was to draw for more than 3 in a row (not frames with no scene:
    // the pause screen keeps the 30 above): the game's own upscale then takes the frames, which a jitter only makes wobble.
    // A few frames, not one: this is asked on the game's thread, ahead of the replay that draws)
    const uint64_t miss = s.missed_from.load(std::memory_order_relaxed);
    if (miss && now >= miss && now - miss > kMissedFrames) return 0;
    if (axis == 2) return s.jitter_phases.load(std::memory_order_relaxed);
    return axis ? s.jitter_h.load(std::memory_order_relaxed) : s.jitter_w.load(std::memory_order_relaxed);
}

void release_idle() {
    State& s = st();
    const uint64_t now = texture_frame_no();
    if (wanted()) {
        s.used_frame = now;   // (asked for: kept, a frame may come at any time)
        std::unique_lock lk(s.mu, std::try_to_lock);
        if (lk.owns_lock() && !s.dead.empty()) free_dead();
        return;
    }
    std::lock_guard lk(s.mu);
    if (now - s.used_frame <= 120) return;   // (off for under 120 frames (2 s): kept, a row stepped through comes back at once)
    if (s.color.image || s.depth.image || s.motion.image || !s.depth_views.empty()) {
        logf("vk: fsr3 inputs: not asked for 120 frames: their images let go");
        for (Image* im : {&s.color, &s.depth, &s.motion})
            if (im->image) {
                const Image old = *im;
                later([old] {
                    vkDestroyImageView(dev.device, old.view, nullptr);
                    destroy_image(old.image, old.memory);
                });
                *im = {};
            }
        auto old = std::move(s.depth_views);
        s.depth_views.clear();
        later([old] {
            for (const auto& [image, view] : old) vkDestroyImageView(dev.device, view, nullptr);
        });
        s.have_frame = false, s.last_frame = 0, s.have_pose = false, s.cam.valid = false;
    }
    free_dead();
}

} // namespace mkwvk::fsr3_inputs
