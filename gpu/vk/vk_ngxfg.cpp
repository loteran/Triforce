// (hookshot) NVIDIA DLSS Frame Generation ("DLSS-G") through NGX itself (vk_ngxfg.h), as vk_framegen.cpp's engine on RTX 40 and 50
// cards. Ported from NX2navi's runtime (vk_dlaa.c vk_ngxfg_evaluate and gfx_present.c, which run this game's DLSS-G): the same NGX
// parameters, matrices and reset rules, on the renderer's own inputs (vk_fsr3_inputs.cpp: the scene's depth, motion and camera).
//
// What DLSS-G is given, per game frame, to make `count` frames between the last frame and this one:
//  - the backbuffer: the game's picture with its HUD, the window texture's crop as vk_framegen.cpp copied it (RGBA8);
//  - the depth (1 at the near plane) and the motion vectors (render pixels toward the frame before) of the scene, which has the
//    render size, and the camera: projection and its inverse, this frame's clip space to the last's and back, the position and
//    basis, the planes, the field of view and the aspect (dlssg_math.h; the jitter taken off them and given on its own);
//  - the picture without the HUD (vk_hudless.h), when an upscaler drew this frame: DLSS-G tells the HUD apart from the scene by
//    comparing the two, and keeps the HUD still while the scene's pixels move. The upscaler's output is encoded as the game's
//    last pass encodes its picture (linear light to sRGB, which a blit into an sRGB image does), so a pixel of it is the
//    backbuffer's pixel. Without an upscaler there is none: the HUD is then DLSS-G's own to find (it is optional).
// Made frame i of count is written for MultiFrameIndex i + 1 of MultiFrameCount count; the caller shows them in order, then
// the game's own.
#include "vk_ngxfg.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <vector>

#include "vk_dlss.h"
#include "vk_framegen.h"
#include "vk_hudless.h"
#include "vk_menu.h"
#include "vk_texture.h"

#ifdef HOOKSHOT_NGX
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_helpers_dlssg_vk.h"
#endif
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk::ngxfg {

Vendor vendor() {
    static const Vendor v = [] {
        const char* e = getenv("RX_VK_FRAMEGEN_VENDOR");
        bool known = true;
        const Vendor parsed = fgengine::parse_vendor(e, &known);
        if (!known) logf("vk: RX_VK_FRAMEGEN_VENDOR=%s is not auto, nvidia, amd or triforce: auto", e);
        return parsed;
    }();
    return v;
}

bool dlssg_allowed() { return vendor() == VENDOR_AUTO || vendor() == VENDOR_NVIDIA; }

#ifdef HOOKSHOT_NGX

namespace {

struct Img {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};

struct State {
    std::mutex mu;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    uint32_t fw = 0, fh = 0;      // (the picture size the feature was made for)
    std::atomic<bool> failed{false};   // (its creation or an evaluation failed for good: the renderer's own frames; read by wanted_for without the lock)
    bool reset_next = true;       // (the next evaluation starts a new history)
    uint64_t evals = 0, last_eval = 0, made_total = 0;
    // the picture without the HUD (vk_hudless.h) scaled to the backbuffer's when the two sizes differ
    Img scaled;
    struct Dead { uint64_t frame; std::function<void()> free; };
    std::vector<Dead> dead;       // (what the GPU may still use: let go a few frames on)
};
State& st() {
    static State s;
    return s;
}

void later(std::function<void()> f) { st().dead.push_back({texture_frame_no(), std::move(f)}); }
void free_dead(bool all = false) {
    State& s = st();
    const uint64_t now = texture_frame_no();
    for (size_t i = 0; i < s.dead.size();) {
        if (all || now - s.dead[i].frame > 4) {
            s.dead[i].free();
            s.dead[i] = std::move(s.dead.back());
            s.dead.pop_back();
        } else {
            i++;
        }
    }
}
void drop(Img& im) {
    if (!im.image) return;
    const Img old = im;
    later([old] {
        vkDestroyImageView(dev.device, old.view, nullptr);
        destroy_image(old.image, old.alloc);
    });
    im = {};
}

/// A 2D image of this format and size, UNDEFINED to GENERAL at once (cmd); the usage is what its users need
bool make(VkCommandBuffer cmd, Img& im, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage) {
    if (im.image && im.w == w && im.h == h) return true;
    drop(im);
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D, ic.format = format, ic.extent = {w, h, 1}, ic.mipLevels = 1, ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL, ic.usage = usage;
    Img made;
    if (!create_image(ic, made.image, made.alloc)) return false;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = made.image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device, &vc, nullptr, &made.view) != VK_SUCCESS) {
        destroy_image(made.image, made.alloc);
        return false;
    }
    made.w = w, made.h = h;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = made.image, b.subresourceRange = vc.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    im = made;
    return true;
}

NVSDK_NGX_Resource_VK resource(VkImageView view, VkImage image, VkFormat format, uint32_t w, uint32_t h, bool write) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(view, image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, format, w, h, write);
}

void set_ui(NVSDK_NGX_Parameter* p, const char* k, unsigned v) { NVSDK_NGX_Parameter_SetUI(p, k, v); }
void set_f(NVSDK_NGX_Parameter* p, const char* k, float v) { NVSDK_NGX_Parameter_SetF(p, k, v); }
void set_p(NVSDK_NGX_Parameter* p, const char* k, void* v) { NVSDK_NGX_Parameter_SetVoidPointer(p, k, v); }

/// The feature and the parameters let go once the GPU is past them
void release_feature(State& s) {
    if (!s.feature) return;
    NVSDK_NGX_Handle* old = s.feature;
    later([old] { NVSDK_NGX_VULKAN_ReleaseFeature(old); });
    s.feature = nullptr;
    s.fw = s.fh = 0;
}

}  // namespace

bool built() { return true; }

int max_generated() {
    if (!dlssg_allowed()) return 0;
    return dlss_fg_max();
}

bool wanted_for(int framegen) {
    if (framegen < 1 || !dlssg_allowed() || dev.hdr || opts.hdr == 2) return false;
    // (a feature that failed is not tried again for the run: asked of every scene draw, so no lock: a stale read costs a frame)
    if (st().failed.load(std::memory_order_relaxed)) return false;
    const int most = max_generated();
    return most > 0 && (framegen == 1 || framegen <= most + 1);
}

bool inputs_wanted() { return opts.framegen >= 1 && wanted_for(opts.framegen) && framegen_on(); }

bool evaluate(VkCommandBuffer cmd, const Inputs& in, const Output* out, int count) {
    State& s = st();
    std::lock_guard lk(s.mu);
    free_dead();
    if (s.failed || count < 1 || count > max_generated() || !in.color || !in.depth || !in.motion || !in.width || !in.height ||
        !in.render_width || !in.render_height)
        return false;
    if (!s.params) {
        if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&s.params)) || !s.params) {
            s.params = nullptr, s.failed = true;
            logf("vk: ngxfg: NGX gave no parameters for DLSS Frame Generation: the renderer's own frames");
            return false;
        }
    }
    if (s.feature && (s.fw != in.width || s.fh != in.height)) {
        release_feature(s);
        s.reset_next = true;
    }
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (NGX records into cmd itself: what this thread recorded through vk_work.h reaches cmd first)
    if (!s.feature) {
        NVSDK_NGX_DLSSG_Create_Params cp{};
        cp.Width = in.width, cp.Height = in.height, cp.NativeBackbufferFormat = (unsigned)VK_FORMAT_R8G8B8A8_UNORM;
        cp.RenderWidth = in.render_width, cp.RenderHeight = in.render_height, cp.DynamicResolutionScaling = false;
        const NVSDK_NGX_Result r = NGX_VK_CREATE_DLSSG(cmd, 1, 1, &s.feature, s.params, &cp);
        note_work();
        if (NVSDK_NGX_FAILED(r) || !s.feature) {
            s.feature = nullptr, s.failed = true;
            logf("vk: ngxfg: DLSS Frame Generation's feature was not made (%#x) at %ux%u: the renderer's own frames", (unsigned)r,
                 in.width, in.height);
            return false;
        }
        s.fw = in.width, s.fh = in.height, s.reset_next = true;
        logf("vk: ngxfg: DLSS Frame Generation made at %ux%u (RGBA8 backbuffer; depth and motion %ux%u), up to %d made frames "
             "a game frame", in.width, in.height, in.render_width, in.render_height, max_generated());
    }
    NVSDK_NGX_Parameter* p = s.params;
    NVSDK_NGX_Resource_VK rb = resource(in.color, in.color_image, VK_FORMAT_R8G8B8A8_UNORM, in.width, in.height, false);
    NVSDK_NGX_Resource_VK rz = resource(in.depth, in.depth_image, VK_FORMAT_R32_SFLOAT, in.render_width, in.render_height, false);
    NVSDK_NGX_Resource_VK rm = resource(in.motion, in.motion_image, VK_FORMAT_R16G16_SFLOAT, in.render_width, in.render_height, false);
    // (the picture without the HUD, taken this frame by an upscaler: its size the backbuffer's, or scaled to it when the two
    // differ in size only; otherwise none, which DLSS-G allows)
    NVSDK_NGX_Resource_VK rh{};
    bool hudless = false;
    if (const hudless::Picture snap = hudless::current(); snap.valid()) {
        if (snap.width == in.width && snap.height == in.height) {
            rh = resource(snap.view, snap.image, VK_FORMAT_R8G8B8A8_UNORM, in.width, in.height, false), hudless = true;
        } else if (std::fabs((double)snap.width / snap.height - (double)in.width / in.height) < 0.02 &&
                   make(cmd, s.scaled, in.width, in.height, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            // (the upscaler's picture can be smaller than the presented one, as at the handheld profile: the same picture, scaled)
            full_barrier(cmd);
            VkImageBlit b{};
            b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.srcOffsets[1] = {(int)snap.width, (int)snap.height, 1};
            b.dstOffsets[1] = {(int)in.width, (int)in.height, 1};
            vkCmdBlitImage(cmd, snap.image, VK_IMAGE_LAYOUT_GENERAL, s.scaled.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
            full_barrier(cmd);
            rh = resource(s.scaled.view, s.scaled.image, VK_FORMAT_R8G8B8A8_UNORM, in.width, in.height, false), hudless = true;
        }
    }
    static int told_hudless = -1;   // (said when it changes: a picture without the HUD this present, or none)
    if (told_hudless != (hudless ? 1 : 0)) {
        told_hudless = hudless ? 1 : 0;
        logf("vk: ngxfg: %s", hudless ? "the picture without the HUD is given (an upscaler drew this frame)"
                                      : "no picture without the HUD this frame (no upscaler drew, or its size does not fit): DLSS-G finds the HUD itself");
    }
    set_p(p, NVSDK_NGX_DLSSG_Parameter_Backbuffer, &rb);
    set_p(p, NVSDK_NGX_DLSSG_Parameter_Depth, &rz);
    set_p(p, NVSDK_NGX_DLSSG_Parameter_MVecs, &rm);
    set_p(p, NVSDK_NGX_DLSSG_Parameter_HUDLess, hudless ? &rh : nullptr);
    for (const char* k : {NVSDK_NGX_DLSSG_Parameter_UI, NVSDK_NGX_DLSSG_Parameter_UIAlpha,
                          NVSDK_NGX_DLSSG_Parameter_BidirectionalDistortionField, NVSDK_NGX_DLSSG_Parameter_OutputReal,
                          NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation, NVSDK_NGX_DLSSG_Parameter_ClipToLensClip})
        set_p(p, k, nullptr);
    const dlssg_math::Camera& c = in.camera;
    set_p(p, NVSDK_NGX_DLSSG_Parameter_CameraViewToClip, (void*)c.view_to_clip);
    set_p(p, NVSDK_NGX_DLSSG_Parameter_ClipToCameraView, (void*)c.clip_to_view);
    set_p(p, NVSDK_NGX_DLSSG_Parameter_ClipToPrevClip, (void*)c.clip_to_prev_clip);
    set_p(p, NVSDK_NGX_DLSSG_Parameter_PrevClipToClip, (void*)c.prev_clip_to_clip);
    set_f(p, NVSDK_NGX_DLSSG_Parameter_JitterOffsetX, in.jitter[0]);
    set_f(p, NVSDK_NGX_DLSSG_Parameter_JitterOffsetY, in.jitter[1]);
    set_f(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleX, 1.0f / (float)in.render_width);   // (the vectors are in render pixels)
    set_f(p, NVSDK_NGX_DLSSG_Parameter_MvecScaleY, 1.0f / (float)in.render_height);
    set_f(p, NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetX, 0.0f);
    set_f(p, NVSDK_NGX_DLSSG_Parameter_CameraPinholeOffsetY, 0.0f);
    static const char* const pos[] = {NVSDK_NGX_DLSSG_Parameter_CameraPosX, NVSDK_NGX_DLSSG_Parameter_CameraPosY, NVSDK_NGX_DLSSG_Parameter_CameraPosZ};
    static const char* const up[] = {NVSDK_NGX_DLSSG_Parameter_CameraUpX, NVSDK_NGX_DLSSG_Parameter_CameraUpY, NVSDK_NGX_DLSSG_Parameter_CameraUpZ};
    static const char* const rt[] = {NVSDK_NGX_DLSSG_Parameter_CameraRightX, NVSDK_NGX_DLSSG_Parameter_CameraRightY, NVSDK_NGX_DLSSG_Parameter_CameraRightZ};
    static const char* const fw[] = {NVSDK_NGX_DLSSG_Parameter_CameraFwdX, NVSDK_NGX_DLSSG_Parameter_CameraFwdY, NVSDK_NGX_DLSSG_Parameter_CameraFwdZ};
    for (int i = 0; i < 3; i++)
        set_f(p, pos[i], c.pos[i]), set_f(p, up[i], c.up[i]), set_f(p, rt[i], c.right[i]), set_f(p, fw[i], c.fwd[i]);
    set_f(p, NVSDK_NGX_DLSSG_Parameter_CameraNear, c.near_plane);
    set_f(p, NVSDK_NGX_DLSSG_Parameter_CameraFar, c.far_plane);
    set_f(p, NVSDK_NGX_DLSSG_Parameter_CameraFOV, c.fov_y);
    set_f(p, NVSDK_NGX_DLSSG_Parameter_CameraAspectRatio, c.aspect);
    set_ui(p, NVSDK_NGX_DLSSG_Parameter_ColorBuffersHDR, 0);
    set_ui(p, NVSDK_NGX_DLSSG_Parameter_DepthInverted, 1);   // (vk_fsr3_inputs.cpp's depth: 1 at the near plane, whatever the game's is)
    set_ui(p, NVSDK_NGX_DLSSG_Parameter_CameraMotionIncluded, 1);   // (the motion is the camera's, from the depth, and the actors')
    set_ui(p, NVSDK_NGX_DLSSG_Parameter_Reset, (in.reset || s.reset_next) ? 1 : 0);
    set_ui(p, NVSDK_NGX_DLSSG_Parameter_AutomodeOverrideReset, 0);
    set_ui(p, NVSDK_NGX_DLSSG_Parameter_NotRenderingGameFrames, 0);
    set_ui(p, NVSDK_NGX_DLSSG_Parameter_OrthoProjection, 0);
    set_ui(p, NVSDK_NGX_DLSSG_Parameter_MvecDilated, 0);
    set_ui(p, NVSDK_NGX_DLSSG_Parameter_MenuDetectionEnabled, 0);
    struct Rect { const char *x, *y, *w, *h; unsigned width, height; };
    const Rect rects[] = {
        {NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseX, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectBaseY,
         NVSDK_NGX_DLSSG_Parameter_MVecsSubrectWidth, NVSDK_NGX_DLSSG_Parameter_MVecsSubrectHeight, in.render_width, in.render_height},
        {NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseX, NVSDK_NGX_DLSSG_Parameter_DepthSubrectBaseY,
         NVSDK_NGX_DLSSG_Parameter_DepthSubrectWidth, NVSDK_NGX_DLSSG_Parameter_DepthSubrectHeight, in.render_width, in.render_height},
        {NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectBaseX, NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectBaseY,
         NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectWidth, NVSDK_NGX_DLSSG_Parameter_HUDLessSubrectHeight, in.width, in.height},
        {NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectBaseX, NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectBaseY,
         NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectWidth, NVSDK_NGX_DLSSG_Parameter_InputBackbufferSubrectHeight, in.width, in.height},
        {NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseX, NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectBaseY,
         NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectWidth, NVSDK_NGX_DLSSG_Parameter_OutputInterpolatedSubrectHeight, in.width, in.height},
        {NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectBaseX, NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectBaseY,
         NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectWidth, NVSDK_NGX_DLSSG_Parameter_OutputRealSubrectHeight, in.width, in.height}};
    for (const Rect& r : rects) set_ui(p, r.x, 0), set_ui(p, r.y, 0), set_ui(p, r.w, r.width), set_ui(p, r.h, r.height);
    set_ui(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, (unsigned)count);
    int made = 0;
    for (int i = 0; i < count; i++) {
        NVSDK_NGX_Resource_VK ro = resource(out[i].view, out[i].image, VK_FORMAT_R8G8B8A8_UNORM, in.width, in.height, true);
        set_p(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, &ro);
        set_ui(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, (unsigned)(i + 1));
        const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_EvaluateFeature_C(cmd, s.feature, p, nullptr);
        note_work();   // (NGX's own commands, which vk_work.h does not see: the next full_barrier is not left out)
        if (NVSDK_NGX_FAILED(r)) {
            static int told = 0;
            if (told++ < 5) logf("vk: ngxfg: DLSS Frame Generation's evaluation %d of %d failed (%#x)", i + 1, count, (unsigned)r);
            break;
        }
        made++;
    }
    // (the pointers set above are of this call's locals: none left in the parameters for another call to find)
    set_p(p, NVSDK_NGX_DLSSG_Parameter_Backbuffer, nullptr), set_p(p, NVSDK_NGX_DLSSG_Parameter_Depth, nullptr);
    set_p(p, NVSDK_NGX_DLSSG_Parameter_MVecs, nullptr), set_p(p, NVSDK_NGX_DLSSG_Parameter_HUDLess, nullptr);
    set_p(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, nullptr);
    full_barrier(cmd);
    if (made < count) {
        s.reset_next = true;   // (the made frames are not all there: the renderer's own are, and DLSS-G has no clean history)
        return false;
    }
    s.reset_next = false;
    s.last_eval = texture_frame_no();
    s.made_total += (uint64_t)made;
    if (++s.evals % 3600 == 0)
        logf("vk: ngxfg: %llu game frames with DLSS Frame Generation, %llu made frames (%d a frame now)",
             (unsigned long long)s.evals, (unsigned long long)s.made_total, count);
    return true;
}

void reset() { st().reset_next = true; }

void release() {
    State& s = st();
    std::lock_guard lk(s.mu);
    release_feature(s);
    drop(s.scaled);
    s.reset_next = true;
    free_dead(true);   // (the caller has the device idle: nothing is in use)
    if (s.params) NVSDK_NGX_VULKAN_DestroyParameters(s.params), s.params = nullptr;
}

void idle() {
    State& s = st();
    std::unique_lock lk(s.mu, std::try_to_lock);
    if (!lk.owns_lock() || (!s.feature && !s.scaled.image)) return;
    const uint64_t now = texture_frame_no();
    if (now - s.last_eval < 300) return;   // (used within the last five seconds' frames)
    // (not used for a while: its video memory goes back, made again when the row asks for it)
    release_feature(s);
    drop(s.scaled);
    s.reset_next = true;
    free_dead();
}

#else  // (no NGX SDK in this build: DLSS Frame Generation is not there)

bool built() { return false; }
int max_generated() { return 0; }
bool wanted_for(int) { return false; }
bool inputs_wanted() { return false; }
bool evaluate(VkCommandBuffer, const Inputs&, const Output*, int) { return false; }
void reset() {}
void release() {}
void idle() {}

#endif

} // namespace mkwvk::ngxfg
