// AMD FSR 3.1 frame generation through AMD's own FidelityFX library (vk_ffx_fg.h). Ported from NX2navi's runtime/vk_fg.cpp and
// vk_ffx.cpp, which run this game on the same library (SDK 1.1.4, FSR 3.1.4), onto Triforce's own swapchain, queues and inputs.
//
// Who does what:
//  - the swapchain: AMD's. vk_main.cpp's create_swapchain asks create_swapchain() below for it (the first time it makes the
//    FidelityFX swapchain context, which replaces vkCreateSwapchainKHR, vkGetSwapchainImagesKHR, vkAcquireNextImageKHR and
//    vkQueuePresentKHR with its own); present_image (vk_replay.cpp) acquires, draws the picture into the image it gets (the menu,
//    ultrawide's sides and the HDR pass included) and presents, once for each frame of the game, through the functions here. AMD's
//    threads present the generated frame between that present and the one before, on the present queue;
//  - the generation: AMD's frame generation context (one for each display size and format). Every frame frame() configures it: on
//    or off, the picture's rectangle, the picture without the HUD, the frame id; with it on, a preparation (the frame's depth and
//    motion vectors, camera and jitter) is recorded into the present's command buffer. The generation callback, on AMD's thread,
//    dispatches the generation itself, with the camera cut noted for that frame id (vk_fg_reset.h);
//  - the queues: the swapchain wants four distinct ones: the game's (dev.queue, where the present is called, and where AMD submits
//    through submit_game, which takes dev.present_mu as every other submit on it does), an async compute one (none: allowAsync is
//    off), a present queue and an image acquire queue (create_device made them). The library refuses a queue named twice, so a device
//    that cannot give three distinct queues has no AMD frame generation here (said in the log);
//  - the generation is off, and AMD's swapchain presents the game's frames alone, while the menu is open, for a frame without the
//    scene's inputs (a load, the pause screen), while the context is being made, and after a failure.
//
// The picture without the HUD: the one vk_hudless.h kept where the upscaler drew, placed into an image of the swapchain's format and
// size at the same rectangle the picture has, so a pixel of it is the swapchain image's pixel without the menu's overlays and the
// HUD. Given only when the picture reaches the swapchain image by the plain blit of its crop; with the window filter (Upscaling,
// Sharpness), ultrawide's sides or the HDR pass the picture in the swapchain image has been through a pass the HUD-less one has
// not, and none is given (AMD then finds the HUD itself, less well).
//
// Only this file and vk_ffx3.cpp see the SDK 1.1.4 headers (third_party/ffx_api_114).
#include "vk_ffx_fg.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "vk_ffx3.h"
#include "vk_fg_reset.h"
#include "vk_fsr3_inputs.h"
#include "vk_hudless.h"
#include "vk_menu.h"
#include "vk_ngxfg.h"
#include "vk_texture.h"

// (AMD's header marks its entry points __declspec(dllexport), which only Windows compilers know: they are only declarations here,
// the library is loaded at run time)
#if !defined(_WIN32) && !defined(__declspec)
#define __declspec(x)
#endif
#include "ffx_api.h"
#include "ffx_api_types.h"
#include "ffx_framegeneration.h"
#include "vk/ffx_api_vk.h"

#include "vk_work.h"   // (after the others: the barriers here count as work for full_barrier)

namespace mkwvk::ffxfg {

namespace {

constexpr int kHudlessRing = 4;       // (the picture without the HUD, one image for each frame AMD's threads may still be reading)
constexpr int kContextTries = 3;      // (a context that is not made is tried again this often, then AMD's engine is off for the run)

struct HudlessImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
};

struct State {
    // the device
    VkQueue present_q = VK_NULL_HANDLE, acquire_q = VK_NULL_HANDLE;
    uint32_t present_family = 0, acquire_family = 0;
    bool queues_ok = false;            // (the device made them, and the present queue's family presents to the window)
    std::atomic<bool> failed{false};
    char failed_what[160] = "";
    std::atomic<bool> target{false};
    // the swapchain's context and its functions
    ffxContext sc = nullptr;
    ffxQueryDescSwapchainReplacementFunctionsVK fn{};
    // the frame generation context
    ffxContext fg = nullptr;
    VkExtent2D fg_ext{}, fg_max{};
    VkFormat fg_fmt = VK_FORMAT_UNDEFINED;
    bool fg_hdr = false;
    int ctx_fails = 0;
    uint64_t retry_at = 0;
    uint64_t vram = 0;
    // each frame
    uint64_t frame = 0;                // (the frame id: 1 more for each configured frame)
    uint64_t calls = 0;
    bool prepared = false;             // (the frame before was prepared: generation was on)
    std::atomic<bool> on{false};
    fg_reset::Ring cuts;
    std::atomic<uint64_t> generated{0};
    uint64_t paused = 0, no_inputs = 0, no_hudless = 0, hudless_given = 0, cut_frames = 0;
    // the picture without the HUD at the swapchain's size and format
    HudlessImage hudless[kHudlessRing];
    int hudless_next = 0;
    VkExtent2D hudless_ext{};
    VkFormat hudless_fmt = VK_FORMAT_UNDEFINED;
    bool hudless_unsupported = false;
    // (the HDR metadata: Triforce sets none; AMD's replacement of vkSetHdrMetadataEXT is not used)
};
State& st() {
    static State& s = *new State;   // (never destroyed: Hookshot leaves through ExitProcess with the device alive, and AMD's threads may call back)
    return s;
}

// A thread that holds dev.present_mu while it calls into AMD's swapchain (create, destroy) must not be asked for it again by the
// submit callback, which AMD may call from that same call
thread_local bool t_lock_held = false;
struct LockHeld {
    LockHeld() { t_lock_held = true; }
    ~LockHeld() { t_lock_held = false; }
};

const FfxLibrary* L() { return ffx_library(); }
ffxReturnCode_t create_ctx(ffxContext* c, ffxCreateContextDescHeader* d) {
    return ((PfnFfxCreateContext)L()->create)(c, d, nullptr);
}
ffxReturnCode_t destroy_ctx(ffxContext* c) { return ((PfnFfxDestroyContext)L()->destroy)(c, nullptr); }
ffxReturnCode_t configure_ctx(ffxContext* c, const ffxConfigureDescHeader* d) { return ((PfnFfxConfigure)L()->configure)(c, d); }
ffxReturnCode_t query_ctx(ffxContext* c, ffxQueryDescHeader* d) { return ((PfnFfxQuery)L()->query)(c, d); }
ffxReturnCode_t dispatch_ctx(ffxContext* c, const ffxDispatchDescHeader* d) { return ((PfnFfxDispatch)L()->dispatch)(c, d); }

/// The submissions AMD's swapchain makes on the game's queue go through here, under the lock every submit on it takes
VkResult submit_game(uint32_t n, const VkSubmitInfo* si, VkFence fence) {
    static const PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)vkGetDeviceProcAddr(dev.device, "vkQueueSubmit");
    std::unique_lock lk(dev.present_mu, std::defer_lock);
    if (!t_lock_held) lk.lock();
    queue_flush_locked();   // (the submits held out for batching came before this one: AMD's work reads what they wrote)
    return submit(dev.queue, n, si, fence);
}

/// AMD's generation callback, on its thread: this frame's cut, and the HDR10 colour description, then the generation itself
ffxReturnCode_t generate(ffxDispatchDescFrameGeneration* d, void* user) {
    State& s = st();
    s.generated.fetch_add(1, std::memory_order_relaxed);
    // (the swapchain's own reset, a frame id that did not follow the last, stays; the frame's own cut is added)
    if (fg_reset::is_cut(s.cuts, d->frameID)) d->reset = true;
    if (s.fg_hdr) {   // (Triforce's HDR10 swapchain: PQ in Rec. 2020, the peak the Insert menu says)
        d->backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
        d->minMaxLuminance[0] = 0.0f;
        d->minMaxLuminance[1] = (float)std::clamp(opts.hdr_peak, 400, 10000);
    }
    return dispatch_ctx((ffxContext*)user, &d->header);
}

void on_message(uint32_t type, const wchar_t* message) {
    char text[512];
    size_t n = 0;
    for (; message && message[n] && n < sizeof text - 1; n++) text[n] = message[n] < 128 ? (char)message[n] : '?';
    text[n] = 0;
    static std::atomic<int> told{0};
    if (told++ < 40) logf("vk: ffxfg: AMD's %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

FfxApiResource image_resource(VkImage image, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = format, ci.extent = {w, h, 1}, ci.mipLevels = 1, ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.usage = usage;
    // (GENERAL, which the backend reads as the unordered-access state: the images are left there)
    return ffxApiGetResourceVK((void*)image, ffxApiGetImageResourceDescriptionVK(image, ci, 0), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

bool hdr_format(VkFormat f) {
    return f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

void drop_hudless() {   // (the device idle)
    State& s = st();
    for (HudlessImage& h : s.hudless)
        if (h.image) destroy_image(h.image, h.alloc), h = {};
    s.hudless_ext = {};
    s.hudless_fmt = VK_FORMAT_UNDEFINED;
}

/// Generation off for the frame with this id, best effort: a failure leaves the context configured as the last frame left it, which
/// would go on generating from a preparation that never came
void configure_off(VkSwapchainKHR sc, VkExtent2D ext, uint64_t id) {
    State& s = st();
    if (!s.fg || !sc) return;
    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = (void*)sc;
    cfg.frameGenerationCallback = generate;
    cfg.frameGenerationCallbackUserContext = &s.fg;
    cfg.frameGenerationEnabled = false;
    cfg.generationRect = FfxApiRect2D{0, 0, (int32_t)ext.width, (int32_t)ext.height};
    cfg.frameID = id;
    configure_ctx(&s.fg, &cfg.header);
    s.on.store(false, std::memory_order_relaxed);
    s.prepared = false;
}

/// The frame generation context goes, with the swapchain's callbacks reset first: the provider's destroy leaves the swapchain
/// configured with callbacks into the freed context, which a frame presented before another context is configured would call.
/// The device is idle
void drop_context(VkSwapchainKHR sc, VkExtent2D ext) {
    State& s = st();
    if (!s.fg) return;
    if (sc) {
        ffxConfigureDescFrameGeneration cfg{};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = (void*)sc;
        cfg.frameGenerationEnabled = false;
        cfg.generationRect = FfxApiRect2D{0, 0, (int32_t)ext.width, (int32_t)ext.height};
        cfg.frameID = ++s.frame;
        const ffxReturnCode_t r = configure_ctx(&s.fg, &cfg.header);
        if (r != FFX_API_RETURN_OK) logf("vk: ffxfg: resetting the swapchain's frame generation failed (%d)", (int)r);
    }
    destroy_ctx(&s.fg);
    s.fg = nullptr;
    s.prepared = false;
    s.on.store(false, std::memory_order_relaxed);
}

/// Makes the frame generation context for this display size and format (render sizes up to max_render). false: not made (said)
bool make_context(VkFormat fmt, VkExtent2D ext, VkExtent2D max_render) {
    State& s = st();
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = dev.device;
    backend.vkPhysicalDevice = dev.physical;
    backend.vkDeviceProcAddr = vkGetDeviceProcAddr;
    ffxCreateContextDescFrameGeneration d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    d.header.pNext = &backend.header;
    // (the depth vk_fsr3_inputs.h gives is reversed, 1 at the near plane: the inverted flag, with the scene's own finite planes in
    // each preparation. The motion has no jitter in it. The picture without the HUD has the swapchain's own format, so no
    // hudless format description beside the back buffer's)
    d.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | (hdr_format(fmt) ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0u);
    d.displaySize = {ext.width, ext.height};
    d.maxRenderSize = {max_render.width, max_render.height};
    d.backBufferFormat = ffxApiGetSurfaceFormatVK(fmt);
    const auto t0 = std::chrono::steady_clock::now();
    const ffxReturnCode_t r = create_ctx(&s.fg, &d.header);
    s.fg_ext = ext;   // (the size tried: a failure is tried again at once for another one)
    if (r != FFX_API_RETURN_OK || !s.fg) {
        s.fg = nullptr;
        s.on.store(false, std::memory_order_relaxed);
        if (++s.ctx_fails >= kContextTries) {
            char what[96];
            snprintf(what, sizeof what, "the frame generation context for %ux%u was not made (%d), %d tries", ext.width, ext.height,
                     (int)r, s.ctx_fails);
            note_failed(what, (int)r);
        } else {
            // (video memory full, typically: a window back to 4K late in a run. Tried again after 600 frames, 1200, ...)
            s.retry_at = s.calls + (600ull << (s.ctx_fails - 1));
            logf("vk: ffxfg: the frame generation context for %ux%u was not made (%d), try %d of %d: generation paused, tried again in "
                 "%llu frames", ext.width, ext.height, (int)r, s.ctx_fails, kContextTries, (unsigned long long)(s.retry_at - s.calls));
        }
        return false;
    }
    s.fg_fmt = fmt, s.fg_max = max_render, s.fg_hdr = hdr_format(fmt);
    FfxApiEffectMemoryUsage mem{};
    ffxQueryDescFrameGenerationGetGPUMemoryUsage mq{};
    mq.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
    mq.gpuMemoryUsageFrameGeneration = &mem;
    s.vram = query_ctx(&s.fg, &mq.header) == FFX_API_RETURN_OK ? mem.totalUsageInBytes : 0;
    logf("vk: ffxfg: AMD FSR 3.1 frame generation context for %ux%u%s, render sizes up to %ux%u (%.1f ms of CPU, its own %llu MB)",
         ext.width, ext.height, s.fg_hdr ? " (HDR)" : "", max_render.width, max_render.height,
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), (unsigned long long)(s.vram >> 20));
    s.ctx_fails = 0, s.retry_at = 0;
    return true;
}

/// The picture without the HUD, made for this frame: the kept copy (vk_hudless.h) blitted into the next image of the ring, at the
/// rectangle the picture has in the swapchain image (black around it). The image, or VK_NULL_HANDLE (none, and why is counted)
VkImage make_hudless(const FrameArgs& a) {
    State& s = st();
    const hudless::Picture pic = hudless::current();
    if (!pic.valid() || !a.plain) return VK_NULL_HANDLE;
    const int* r = a.rect;
    if (r[2] <= 0 || r[3] <= 0 || pic.width == 0 || pic.height == 0) return VK_NULL_HANDLE;
    // (the same picture as the rectangle's: the upscaler's output is smaller than the presented one at the handheld profile, and
    // the blit scales it; another shape is not the same picture)
    const double ar = (double)pic.width / pic.height, rr = (double)r[2] / r[3];
    if (std::abs(ar - rr) > 0.02 * rr) return VK_NULL_HANDLE;
    if (s.hudless_unsupported) return VK_NULL_HANDLE;
    if (s.hudless[0].image && (s.hudless_ext.width != a.extent.width || s.hudless_ext.height != a.extent.height || s.hudless_fmt != a.format)) {
        // (a new swapchain size or format: the old images may be what AMD's threads are reading; the device idles, as for the context)
        std::lock_guard qk(dev.present_mu);
        device_wait_idle_locked();
        drop_hudless();
    }
    if (!s.hudless[0].image) {
        // (storage: the backend reads these images as unordered access; a format that has no storage image, an sRGB one, has none)
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, a.format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || ffxApiGetSurfaceFormatVK(a.format) == FFX_API_SURFACE_FORMAT_UNKNOWN) {
            s.hudless_unsupported = true;
            logf("vk: ffxfg: the swapchain's format %d cannot be a storage image for AMD's frame generation: no picture without the HUD "
                 "is given (it finds the HUD itself)", (int)a.format);
            return VK_NULL_HANDLE;
        }
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D, ic.format = a.format, ic.extent = {a.extent.width, a.extent.height, 1};
        ic.mipLevels = 1, ic.arrayLayers = 1, ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        bool ok = true;
        for (HudlessImage& h : s.hudless) ok = ok && create_image(ic, h.image, h.alloc);
        if (!ok) {
            drop_hudless();
            s.hudless_unsupported = true;
            logf("vk: ffxfg: the picture without the HUD (%ux%u, 4 images) was not made: it is not given", a.extent.width, a.extent.height);
            return VK_NULL_HANDLE;
        }
        s.hudless_ext = a.extent, s.hudless_fmt = a.format;
        for (HudlessImage& h : s.hudless) {   // (UNDEFINED to GENERAL once; GENERAL from then on)
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = h.image, b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(a.cmd, &di);
        }
        logf("vk: ffxfg: the picture without the HUD is kept at %ux%u (swapchain format %d), %d images", a.extent.width, a.extent.height,
             (int)a.format, kHudlessRing);
    }
    VkImage target = s.hudless[s.hudless_next++ % kHudlessRing].image;
    full_barrier(a.cmd);   // (the kept copy was written earlier in the frame; the last use of this image by AMD's work: earlier submits)
    if (r[0] || r[1] || r[2] != (int)a.extent.width || r[3] != (int)a.extent.height) {
        const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(a.cmd, target, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        full_barrier(a.cmd);
    }
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {(int)pic.width, (int)pic.height, 1};
    b.dstOffsets[0] = {r[0], r[1], 0};
    b.dstOffsets[1] = {r[0] + r[2], r[1] + r[3], 1};
    // (the same filter the present's own blit of the picture uses: vk_replay.cpp blit_crop_to_rect)
    vkCmdBlitImage(a.cmd, pic.image, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
    full_barrier(a.cmd);
    return target;
}

}  // namespace

// ---- the device ----

bool queues_wanted() {
    static const bool wanted = [] {
        const char* e = getenv("RX_VK_FFX_FG");
        if (e && *e && atoi(e) == 0) return false;
        if (ngxfg::vendor() != ngxfg::VENDOR_AUTO && ngxfg::vendor() != ngxfg::VENDOR_AMD) return false;
        return ffx_library_present();
    }();
    return wanted;
}

void set_queues(VkQueue present, uint32_t present_family, VkQueue acquire, uint32_t acquire_family) {
    State& s = st();
    s.present_q = present, s.present_family = present_family, s.acquire_q = acquire, s.acquire_family = acquire_family;
    s.queues_ok = present != VK_NULL_HANDLE && acquire != VK_NULL_HANDLE && present != acquire;
}

void surface_made(VkSurfaceKHR surface) {
    State& s = st();
    if (!s.queues_ok) return;
    VkBool32 ok = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical, s.present_family, surface, &ok) != VK_SUCCESS || !ok) {
        s.queues_ok = false;
        logf("vk: ffxfg: queue family %u (AMD's frame generation presents there) cannot present to the window: AMD's frame "
             "generation is not an engine here", s.present_family);
    }
}

// ---- the choice ----

bool usable() {
    State& s = st();
    if (s.failed.load(std::memory_order_relaxed) || !s.queues_ok) return false;
    return L() != nullptr;
}

const char* why_not() {
    State& s = st();
    static char buf[200];
    if (s.failed.load(std::memory_order_relaxed)) {
        snprintf(buf, sizeof buf, "AMD's library failed in this run (%s)", s.failed_what);
        return buf;
    }
    if (!queues_wanted()) return "no libamd_fidelityfx_vk.so beside the game, or RX_VK_FFX_FG=0 or RX_VK_FRAMEGEN_VENDOR leaves it off";
    if (!s.queues_ok) return "the device has no queues for AMD's swapchain";
    if (!L()) return "AMD's library did not load";
    return "";
}

void note_failed(const char* what, int code) {
    State& s = st();
    if (s.failed.exchange(true)) return;
    snprintf(s.failed_what, sizeof s.failed_what, "%s", what);
    logf("vk: ffxfg: %s (%d): AMD's frame generation is off for this run, Triforce's own takes over", what, code);
}

void set_target(bool on) {
    State& s = st();
    if (s.target.exchange(on) != on) logf("vk: ffxfg: AMD's frame generation swapchain is %s", on ? "wanted" : "no longer wanted");
}

bool target() { return st().target.load(std::memory_order_relaxed); }

// ---- the swapchain ----

bool active() { return st().sc != nullptr; }

bool swapchain_stale() {
    State& s = st();
    const bool want = s.target.load(std::memory_order_relaxed);
    if (want == (s.sc != nullptr)) return false;
    return !(want && s.failed.load(std::memory_order_relaxed));   // (wanted but failed: a plain one is made, nothing to make again)
}

bool inputs_wanted() { return st().sc != nullptr || st().target.load(std::memory_order_relaxed); }
bool hudless_wanted() { return inputs_wanted(); }

VkResult create_swapchain(const VkSwapchainCreateInfoKHR& ci_in, VkSwapchainKHR* out) {
    State& s = st();
    LockHeld held;
    VkSwapchainCreateInfoKHR ci = ci_in;
    ci.oldSwapchain = VK_NULL_HANDLE;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;   // (the library moves the images between its queues' families itself)
    ci.queueFamilyIndexCount = 0, ci.pQueueFamilyIndices = nullptr;
    if (s.sc) return s.fn.pOutCreateSwapchainFFXAPI(dev.device, &ci, nullptr, out, s.sc);
    if (!L() || !s.queues_ok || s.failed) return VK_ERROR_FEATURE_NOT_PRESENT;
    ffxCreateContextDescFrameGenerationSwapChainVK d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    d.physicalDevice = dev.physical;
    d.device = dev.device;
    VkSwapchainKHR none = VK_NULL_HANDLE;
    d.swapchain = &none;
    d.createInfo = ci;
    d.gameQueue = {dev.queue, dev.queue_family, submit_game};
    d.asyncComputeQueue = {VK_NULL_HANDLE, dev.queue_family, nullptr};
    d.presentQueue = {s.present_q, s.present_family, nullptr};
    d.imageAcquireQueue = {s.acquire_q, s.acquire_family, nullptr};
    ffxReturnCode_t r = create_ctx(&s.sc, &d.header);
    if (r != FFX_API_RETURN_OK) {
        s.sc = nullptr;
        logf("vk: ffxfg: AMD's frame generation swapchain was not made (%d)", (int)r);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    s.fn = {};
    s.fn.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    if (query_ctx(&s.sc, &s.fn.header) != FFX_API_RETURN_OK || !s.fn.pOutQueuePresentKHR || !s.fn.pOutAcquireNextImageKHR ||
        !s.fn.pOutGetSwapchainImagesKHR || !s.fn.pOutCreateSwapchainFFXAPI || !s.fn.pOutDestroySwapchainFFXAPI) {
        destroy_ctx(&s.sc);   // (with it goes the swapchain it made: nothing was handed out yet)
        s.sc = nullptr;
        logf("vk: ffxfg: AMD's swapchain functions were not given");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *out = none;
    logf("vk: ffxfg: AMD FSR 3.1 frame generation swapchain %ux%u, %u images, queues: game family %u, present family %u, acquire family %u",
         ci.imageExtent.width, ci.imageExtent.height, ci.minImageCount, dev.queue_family, s.present_family, s.acquire_family);
    return VK_SUCCESS;
}

void destroy_swapchain(VkSwapchainKHR sc) {
    State& s = st();
    if (!s.sc || !sc) return;
    LockHeld held;
    s.fn.pOutDestroySwapchainFFXAPI(dev.device, sc, nullptr, s.sc);
}

VkResult get_images(VkSwapchainKHR sc, uint32_t* count, VkImage* images) {
    return st().fn.pOutGetSwapchainImagesKHR(dev.device, sc, count, images);
}

VkResult acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore semaphore, uint32_t* index) {
    return st().fn.pOutAcquireNextImageKHR(dev.device, sc, timeout, semaphore, VK_NULL_HANDLE, index);
}

VkResult present(const VkPresentInfoKHR* info) {
    State& s = st();
    return s.fn.pOutQueuePresentKHR(dev.queue, info);
}

void quiesce() {
    State& s = st();
    if (!s.sc) return;
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    dispatch_ctx(&s.sc, &d.header);
}

void release() {
    State& s = st();
    if (s.fg) drop_context(VK_NULL_HANDLE, {});   // (the swapchain is gone: no callbacks left to reset)
    drop_hudless();
    s.ctx_fails = 0, s.retry_at = 0, s.prepared = false;
    s.on.store(false, std::memory_order_relaxed);
    if (s.sc) {
        LockHeld held;
        destroy_ctx(&s.sc);
        s.sc = nullptr;
    }
    s.fn = {};
}

bool generating() { return st().on.load(std::memory_order_relaxed); }

uint64_t presents(VkSwapchainKHR sc) {
    State& s = st();
    return s.sc && s.fn.pOutGetLastPresentCountFFXAPI && sc ? s.fn.pOutGetLastPresentCountFFXAPI(sc) : 0;
}

// ---- each game frame ----

void frame(const FrameArgs& a) {
    State& s = st();
    if (!s.sc || !a.game_frame) return;
    if (s.failed) {   // (AMD's engine failed in this run: its swapchain presents the game's frames alone until the plain one replaces it)
        if (s.on.load(std::memory_order_relaxed)) configure_off(a.swapchain, a.extent, ++s.frame);
        return;
    }
    s.calls++;
    fsr3_inputs::Frame f;
    bool have = fsr3_inputs::take(f) && f.depth_image && f.motion_image && f.width && f.height;
    // The context's largest render size covers the depth and motion vectors it is given: a context made for the display's size alone
    // wrote past its own resources (NX2navi: a crash in the library, device faults, striped frames). The scene's inputs here are at
    // the render size, which can pass the window's with the upscaler off or DLAA: the context is made for the larger of the two,
    // and made again when a frame brings a larger one
    VkExtent2D max_render = {std::max(a.extent.width, have ? f.width : 0u), std::max(a.extent.height, have ? f.height : 0u)};
    if (s.fg && (s.fg_ext.width != a.extent.width || s.fg_ext.height != a.extent.height || s.fg_fmt != a.format ||
                 max_render.width > s.fg_max.width || max_render.height > s.fg_max.height)) {
        quiesce();   // (before the lock: AMD's threads submit through it)
        {
            std::lock_guard qk(dev.present_mu);
            device_wait_idle_locked();
        }
        drop_context(a.swapchain, a.extent);
        s.retry_at = 0;
    }
    if (!s.fg && s.ctx_fails && (s.fg_ext.width != a.extent.width || s.fg_ext.height != a.extent.height)) s.retry_at = 0;
    // No context made while the menu is open (generation pauses there anyway): a window resized from it is often resized again
    // before it closes, and each context made for a passing size costs a full set of resources. Nor before a failed creation's
    // retry, nor without this frame's inputs (the context is made for their size). The swapchain then presents the game's frames alone
    const bool menu = menu_is_open();
    if (!s.fg) {
        if (menu || !have || s.calls < s.retry_at) {
            s.on.store(false, std::memory_order_relaxed);
            s.prepared = false;
            return;
        }
        if (!make_context(a.format, a.extent, max_render)) return;
    }
    // Generation is off for the menu, a frame without the scene's inputs, and without the camera the preparation takes
    const bool on = have && !menu;
    s.on.store(on, std::memory_order_relaxed);
    if (!have) s.no_inputs++;
    else if (menu) s.paused++;
    s.frame++;
    // A cut: the scene's history was reset (a load, a size change, a gap), the camera jumped, or the frame before was not prepared
    // (generation was off for the menu or a frame without inputs), so the frame between the two would be made from a stale depth
    const bool cut = on && (f.reset || f.cut || !s.prepared);
    fg_reset::note(s.cuts, s.frame, cut);
    if (cut) s.cut_frames++;
    s.prepared = on;
    const VkImage hl = on ? make_hudless(a) : VK_NULL_HANDLE;
    if (on) (hl ? s.hudless_given : s.no_hudless)++;
    // (every 600 frames: what kept frames from being generated, and how many AMD's swapchain presented, generated ones included)
    if (s.frame == 120 || s.frame % 600 == 0)
        logf("vk: ffxfg: %llu frames, %llu generated (%llu with the HUD kept out), %llu presents by the swapchain; not generated: %llu menu "
             "open, %llu without the scene's inputs; %llu without the HUD-less picture (window filter, ultrawide sides, HDR or no "
             "upscaler); %llu after a camera cut", (unsigned long long)s.frame, (unsigned long long)s.generated.load(),
             (unsigned long long)s.hudless_given, (unsigned long long)presents(a.swapchain), (unsigned long long)s.paused,
             (unsigned long long)s.no_inputs, (unsigned long long)s.no_hudless, (unsigned long long)s.cut_frames);

    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = (void*)a.swapchain;
    cfg.frameGenerationCallback = generate;
    cfg.frameGenerationCallbackUserContext = &s.fg;
    cfg.frameGenerationEnabled = on;
    cfg.allowAsyncWorkloads = false;
    // (the picture's rectangle: with black bars beside or below it (a window of another shape) a generation over the whole swapchain
    // warped the picture's edges into the bars)
    cfg.generationRect = a.rect[2] > 0 && a.rect[3] > 0 ? FfxApiRect2D{a.rect[0], a.rect[1], a.rect[2], a.rect[3]}
                                                        : FfxApiRect2D{0, 0, (int32_t)a.extent.width, (int32_t)a.extent.height};
    if (hl) cfg.HUDLessColor = image_resource(hl, a.format, a.extent.width, a.extent.height, VK_IMAGE_USAGE_STORAGE_BIT);
    cfg.frameID = s.frame;
    ffxReturnCode_t r = configure_ctx(&s.fg, &cfg.header);
    if (r != FFX_API_RETURN_OK) {
        note_failed("AMD's frame generation configuration was refused", (int)r);
        s.on.store(false, std::memory_order_relaxed);
        s.prepared = false;
        return;
    }
    if (!on) return;

    ffxDispatchDescFrameGenerationPrepare p{};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    p.frameID = s.frame;
    p.commandList = a.cmd;
    p.renderSize = {f.width, f.height};
    p.jitterOffset = {f.jitter[0], f.jitter[1]};   // (render pixels, x right and y down: as the upscaler takes it)
    p.motionVectorScale = {1.0f, 1.0f};            // (render pixels already, toward the frame before)
    p.frameTimeDelta = std::clamp(f.frame_ms, 1.0f, 250.0f);
    // (the scene's projection's planes and field of view, as vk_fsr3_inputs.h read them; the far plane is FLT_MAX for an infinite
    // projection, which AMD takes as a finite one a long way off)
    const fsr3_math::Camera& c = f.camera;
    p.cameraNear = c.near_plane > 0 ? c.near_plane : 0.1f;
    p.cameraFar = c.far_plane > 0 ? std::min(c.far_plane, 1e9f) : 1e8f;
    p.cameraFovAngleVertical = c.fov_y > 0 ? c.fov_y : 1.0f;
    p.viewSpaceToMetersFactor = 1.0f;   // (the game's units are not known to be metres: AMD's default)
    // (FSR 3.1.4 and later take the camera's world-space basis for best quality: the whole camera NVIDIA's frame generation is given)
    ffxDispatchDescFrameGenerationPrepareCameraInfo cam{};
    if (f.dlssg_ok) {
        cam.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
        std::memcpy(cam.cameraPosition, f.dlssg.pos, sizeof cam.cameraPosition);
        std::memcpy(cam.cameraUp, f.dlssg.up, sizeof cam.cameraUp);
        std::memcpy(cam.cameraRight, f.dlssg.right, sizeof cam.cameraRight);
        std::memcpy(cam.cameraForward, f.dlssg.fwd, sizeof cam.cameraForward);
        p.header.pNext = &cam.header;
    }
    p.depth = image_resource(f.depth_image, VK_FORMAT_R32_SFLOAT, f.width, f.height, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    p.motionVectors = image_resource(f.motion_image, VK_FORMAT_R16G16_SFLOAT, f.width, f.height, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    full_barrier(a.cmd);   // (the depth and motion were written earlier in the frame)
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (ffxDispatch records into cmd itself: what this thread recorded through vk_work.h reaches cmd first)
    r = dispatch_ctx(&s.fg, &p.header);
    note_work();   // (AMD's dispatches: full_barrier must not take the next barrier for a repeat of the one before them)
    full_barrier(a.cmd);
    if (r != FFX_API_RETURN_OK) {
        note_failed("AMD's frame generation preparation was refused", (int)r);
        configure_off(a.swapchain, a.extent, s.frame);   // (this frame's preparation never came: not generated from)
    }
}

}  // namespace mkwvk::ffxfg
