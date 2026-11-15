// AMD FSR 3.1 through AMD's own FidelityFX library, one of the temporal upscaler's engines (vk_ffx3.h). The frame chain it draws
// with is vk_temporal.cpp's, the same as NVIDIA DLSS's and AMD FSR 4's: the colour as the game's network samples it (or the
// scene's linear HDR light before the bloom), the depth, the motion (the camera's and the skinned draws' own) and the camera
// jitter. Here are the library, its upscale contexts and the dispatch.
//
// The library is libamd_fidelityfx_vk.so (Windows: amd_fidelityfx_vk.dll), which exports the FidelityFX API's five entry
// points (ffxCreateContext, ffxDestroyContext, ffxConfigure, ffxQuery, ffxDispatch) of SDK 1.1.4, FSR 3.1.4's. It is looked
// for in the same folders as FSR 4's (vk_fsr4.cpp): RX_VK_FSR_DIR=<folder> when set, the executable's folder, the game folder.
// The host side follows NX2navi's runtime/vk_ffx.cpp, which runs the same game on this library: a Vulkan backend description
// chained to the upscale context description, inputs in SHADER_READ_ONLY_OPTIMAL, the output written in GENERAL.
//
// Sharpening is off here: Triforce sharpens at the window with its Sharpness row, after the HUD. (FSR's own RCAS comes later.)
//
// Only this file sees the 1.1.4 headers (third_party/ffx_api_114): FSR 4's 2.4 ones name the same types with other layouts.
#include "vk_ffx3.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "vk_draw.h"
#include "vk_internal.h"
#include "vk_menu.h"
#include "vk_paths.h"
#include "vk_temporal.h"
#include "vk_texture.h"

// (AMD's header marks its entry points __declspec(dllexport), which only Windows compilers know: they are only declarations here,
// the library is loaded at run time)
#if !defined(_WIN32) && !defined(__declspec)
#define __declspec(x)
#endif
#include "ffx_api.h"
#include "ffx_api_types.h"
#include "ffx_upscale.h"
#include "vk/ffx_api_vk.h"

#include "ffx3_policy.h"
#include "vk_work.h"   // (after the others: the barriers here count as work for full_barrier)

namespace mkwvk {

namespace {

namespace pol = ffx3_policy;

#ifdef _WIN32
constexpr const char* kLibrary = "amd_fidelityfx_vk.dll";
#else
constexpr const char* kLibrary = "libamd_fidelityfx_vk.so";
#endif

int env_switch(const char* name) {   // (-1: unset)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}

/// The library, loaded once. Any thread may ask first (the menu's, the chain's)
struct Library {
    std::mutex mu;
    bool tried = false, ok = false;
    HMODULE lib = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    std::string path;
};
// (never destroyed: Hookshot leaves through ExitProcess / exit() with the device alive, and a destructor then would call into
// the library after it is gone, or under a render thread still recording with it)
Library& lib() {
    static Library& l = *new Library;
    return l;
}
// (FSR 3.1 through the library failed for good in this run: Triforce's own takes over, vk_fsr3.cpp)
std::atomic<bool> g_failed{false};

/// The folders the library is looked for in, as vk_fsr4.cpp's: RX_VK_FSR_DIR, the executable's, the game folder
std::vector<std::string> library_dirs() {
    std::vector<std::string> d;
    if (const char* e = getenv("RX_VK_FSR_DIR"); e && *e) d.emplace_back(e);
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        std::string s(exe, n);
        d.push_back(s.substr(0, s.find_last_of("\\/")));
    }
    n = game_module_path(exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        std::string s(exe, n);
        s = s.substr(0, s.find_last_of("\\/"));
        if (d.empty() || d.back() != s) d.push_back(s);
    }
    return d;
}

void* symbol(const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress(lib().lib, name);
#else
    return dlsym(lib().lib, name);
#endif
}

/// Opens the library and resolves its entry points, once. false: Triforce's own FSR 3.1 (the log says why)
bool load() {
    Library& l = lib();
    std::lock_guard lk(l.mu);
    if (l.tried) return l.ok;
    l.tried = true;
    if (env_switch("RX_VK_FSR3_LIB") == 0) {
        logf("vk: FSR 3.1: Triforce's own (RX_VK_FSR3_LIB=0)");
        return false;
    }
    std::string where;
    for (const std::string& dir : library_dirs()) {
        where += (where.empty() ? "" : ", ") + dir;
#ifdef _WIN32
        const std::string path = dir + "\\" + kLibrary;
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        l.lib = LoadLibraryExA(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!l.lib) logf("vk: FSR 3.1: %s not loaded (Windows error %lu)", path.c_str(), (unsigned long)GetLastError());
#else
        const std::string path = dir + "/" + kLibrary;
        if (access(path.c_str(), R_OK) != 0) continue;
        l.lib = (HMODULE)dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!l.lib) logf("vk: FSR 3.1: %s not loaded: %s", path.c_str(), dlerror());
#endif
        if (l.lib) {
            l.path = path;
            break;
        }
    }
    if (!l.lib) {
        logf("vk: FSR 3.1: Triforce's own: no %s in %s", kLibrary, where.c_str());
        return false;
    }
    l.create = (PfnFfxCreateContext)symbol("ffxCreateContext");
    l.destroy = (PfnFfxDestroyContext)symbol("ffxDestroyContext");
    l.configure = (PfnFfxConfigure)symbol("ffxConfigure");
    l.query = (PfnFfxQuery)symbol("ffxQuery");
    l.dispatch = (PfnFfxDispatch)symbol("ffxDispatch");
    if (!l.create || !l.destroy || !l.configure || !l.query || !l.dispatch) {
        logf("vk: FSR 3.1: Triforce's own: %s lacks the FidelityFX API's entry points", l.path.c_str());
        return false;
    }
    logf("vk: FSR 3.1: AMD's library %s (FidelityFX API 1.1.4), in place of Triforce's own", l.path.c_str());
    l.ok = true;
    return true;
}

void on_message(uint32_t type, const wchar_t* message) {
    char text[512];
    size_t n = 0;
    for (; message && message[n] && n < sizeof text - 1; n++) text[n] = message[n] < 128 ? (char)message[n] : '?';
    text[n] = 0;
    static std::atomic<int> told{0};
    if (told++ < 40) logf("vk: FSR 3.1: AMD's %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

/// An upscale context and the description it was made from (the library keeps no pointer into them after the call, but the
/// context is shared with the list of what is let go of, below)
struct Context {
    ffxContext context = nullptr;
    uint64_t memory = 0;   // (what AMD's library says it holds)
    ~Context() {
        if (context) lib().destroy(&context, nullptr);
    }
};

/// The engine's state: used with the temporal chain's lock held
struct State {
    pol::Slot slot[pol::kSlots];
    std::shared_ptr<Context> context[pol::kSlots];
    uint32_t in_w[pol::kSlots]{}, in_h[pol::kSlots]{};   // (the inputs' size under each: the largest render size it has had)
    uint64_t tick = 0;
    int cur = -1;                // (the context the last prepare chose; evaluate draws with it)
    int last_drawn = -1;         // (the one that drew last: another one has no history of its own)
    std::chrono::steady_clock::time_point last{};
    uint64_t evals = 0;
    int told_resets = 0;
    bool told_first = false;
};
State& st() {
    static State& s = *new State;   // (never destroyed: lib()'s note)
    return s;
}

/// Contexts let go of, destroyed once the GPU is past their last recording (the timeline: vk_timeline_completed), without a wait
/// for the device. A creation the library refuses while some are still in flight waits for those and tries once more
struct Dead {
    std::shared_ptr<Context> context;
    uint64_t fence;
};
std::vector<Dead>& dead() {
    static std::vector<Dead>& d = *new std::vector<Dead>;
    return d;
}
int reap(bool wait) {
    auto& d = dead();
    int n = 0;
    const uint64_t done = vk_timeline_completed();
    for (size_t i = 0; i < d.size();) {
        if (!pol::gpu_past(d[i].fence, done) && wait) vk_timeline_wait(d[i].fence);
        if (pol::gpu_past(d[i].fence, vk_timeline_completed())) {
            d[i] = std::move(d.back());
            d.pop_back();
            n++;
        } else {
            i++;
        }
    }
    return n;
}
uint64_t dead_fence() {
    uint64_t f = 0;
    for (const Dead& x : dead()) f = std::max(f, x.fence);
    return f;
}

void retire(int i, const char* why) {
    State& s = st();
    const pol::Slot& k = s.slot[i];
    logf("vk: FSR 3.1: the AMD context for %ux%u (render sizes up to %ux%u) let go of (destroyed once the GPU is past its last "
         "upscale): %s", k.out_w, k.out_h, k.max_w, k.max_h, why);
    if (s.context[i]) dead().push_back({std::move(s.context[i]), k.fence});
    s.slot[i] = pol::Slot{};
    s.in_w[i] = s.in_h[i] = 0;
    if (s.cur == i) s.cur = -1;
    if (s.last_drawn == i) s.last_drawn = -1;
    // (and a few frames on, when no upscale comes to reap them: the chain frees these with its own deferred frees)
    temporal_later([] { reap(true); });
}

std::shared_ptr<Context> make_context(const pol::Ask& a, uint32_t mw, uint32_t mh) {
    Library& l = lib();
    auto c = std::make_shared<Context>();
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = dev.device;
    backend.vkPhysicalDevice = dev.physical;
    backend.vkDeviceProcAddr = vkGetDeviceProcAddr;
    ffxCreateContextDescUpscale d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    d.header.pNext = &backend.header;
    // (the chain's colour after the game's tone mapping is perceptual (square-root encoded) LDR: no auto exposure. Before the
    // game's bloom and tone mapping, or an RGBA16F scene, it is the scene's linear HDR light, before the game's exposure, so
    // FSR finds the exposure itself. The depth is not inverted: 0 near, 1 far, with the planes of each dispatch; the planes are
    // finite, so no infinite-depth flag)
    d.flags = a.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
                    : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    if (a.drs) d.flags |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    d.maxRenderSize = {mw, mh};
    d.maxUpscaleSize = {a.ow, a.oh};
    d.fpMessage = &on_message;
    const auto t0 = std::chrono::steady_clock::now();
    ffxReturnCode_t r = l.create(&c->context, &d.header, nullptr);
    if (r != FFX_API_RETURN_OK && !dead().empty()) {
        // Refused while contexts let go of are still in flight (most often their video memory): the GPU finishes with them,
        // they go, and the creation is tried once more, rather than the library failing for the run
        c->context = nullptr;
        const uint64_t w0 = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
        vk_timeline_wait(dead_fence());
        const int gone = reap(false);
        const uint64_t w1 = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
        const ffxReturnCode_t r0 = r;
        if (gone) r = l.create(&c->context, &d.header, nullptr);
        logf("vk: FSR 3.1: context creation refused (%u) with contexts let go of still in flight: waited %.1f ms for the GPU, "
             "%d destroyed, %s", (unsigned)r0, (double)(w1 - w0) / 1000.0, gone,
             !gone ? "not tried again" : r == FFX_API_RETURN_OK ? "made at the second try" : "refused again");
    }
    const double made_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != FFX_API_RETURN_OK || !c->context) {
        logf("vk: FSR 3.1: AMD's upscaler was not made (%d) for %ux%u, render sizes up to %ux%u", (int)r, a.ow, a.oh, mw, mh);
        c->context = nullptr;
        return nullptr;
    }
    FfxApiEffectMemoryUsage usage{};
    ffxQueryDescUpscaleGetGPUMemoryUsage mem{};
    mem.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    mem.gpuMemoryUsageUpscaler = &usage;
    if (l.query(&c->context, &mem.header) == FFX_API_RETURN_OK) c->memory = usage.totalUsageInBytes;
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const bool known = l.query(&c->context, &provider.header) == FFX_API_RETURN_OK && provider.versionName;
    logf("vk: FSR 3.1: AMD's FSR %s made for outputs of %ux%u, render sizes %s %ux%u, %s colour (frame %llu, %.1f ms of CPU; its "
         "own %llu MB)", known ? provider.versionName : "?", a.ow, a.oh, a.drs ? "up to" : "of", mw, mh, a.hdr ? "HDR" : "LDR",
         (unsigned long long)texture_frame_no(), made_ms, (unsigned long long)(c->memory >> 20));
    return c;
}

FfxApiResource resource(const TemporalImage& im, VkImageUsageFlags usage, uint32_t state) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = im.format, ci.extent = {im.width, im.height, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage, ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return ffxApiGetResourceVK((void*)im.image, ffxApiGetImageResourceDescriptionVK(im.image, ci, 0), state);
}

VkImageMemoryBarrier2 barrier(VkImage image, VkAccessFlags2 src, VkAccessFlags2 dst, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = src, b.dstAccessMask = dst, b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image, b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}
void barriers(VkCommandBuffer cmd, const VkImageMemoryBarrier2* b, uint32_t n) {
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = n, di.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd, &di);
}

// ---- the engine (vk_temporal.h TemporalEngine) ----

bool start() {
    if (!ffx3_usable()) return false;
    return true;
}

void release() {
    State& s = st();
    for (int i = 0; i < pol::kSlots; i++)
        if (s.slot[i].live) retire(i, "the upscaler is let go of");
    s.cur = s.last_drawn = -1;
}

int prepare(VkCommandBuffer, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool hdr, uint32_t* in_w, uint32_t* in_h,
            const char** why) {
    State& s = st();
    reap(false);
    s.tick++;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t now_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    if (const unsigned idle = pol::idle_mask(s.slot, now_ns))
        for (int i = 0; i < pol::kSlots; i++)
            if (idle & (1u << i)) retire(i, "no upscale used it for 90 s");
    pol::Ask a;
    a.w = rw, a.h = rh, a.ow = ow, a.oh = oh, a.hdr = hdr;
    a.drs = opts.resolution == 0;   // (Render resolution Auto: the game's dynamic resolution, whose render size changes every so often)
    int idx = pol::find(s.slot, a);
    if (const unsigned stale = pol::stale_for_size(s.slot, a, idx, s.tick))
        for (int i = 0; i < pol::kSlots; i++)
            if (stale & (1u << i)) retire(i, "the render size changed");
    bool made = false;
    if (idx < 0) {
        bool evict = false;
        idx = pol::pick_slot(s.slot, s.tick, &evict);
        if (idx < 0) return *why = "all its contexts are in recent use", 0;
        if (evict) retire(idx, "its slot went to another output size");
        uint32_t mw = 0, mh = 0;
        pol::made_for(a, &mw, &mh);
        s.context[idx] = make_context(a, mw, mh);
        if (!s.context[idx]) {
            g_failed = true;   // (for good: Triforce's own FSR 3.1 from the next frame, vk_fsr3.cpp)
            logf("vk: FSR 3.1: AMD's library failed to make its upscaler: Triforce's own FSR 3.1 from now on");
            return *why = "AMD's upscaler was not made", -1;
        }
        pol::Slot& k = s.slot[idx];
        k = pol::Slot{};
        k.live = true, k.max_w = mw, k.max_h = mh, k.out_w = ow, k.out_h = oh, k.drs = a.drs, k.hdr = hdr;
        s.in_w[idx] = s.in_h[idx] = 0;
        made = true;
    }
    pol::Slot& k = s.slot[idx];
    k.used = s.tick, k.used_ns = now_ns;
    k.fence = std::max(k.fence, draw_recording_value());
    // (the inputs as large as the largest render size this context has had, never past what it takes: a fixed share keeps images
    // of its own size, and the game's dynamic resolution only makes them again when it goes past them)
    s.in_w[idx] = std::min(std::max(s.in_w[idx], rw), k.max_w), s.in_h[idx] = std::min(std::max(s.in_h[idx], rh), k.max_h);
    *in_w = s.in_w[idx], *in_h = s.in_h[idx];
    // (another context than the one that drew last has no history: a new one for the chain too)
    const bool other = s.last_drawn != idx;
    s.cur = idx;
    return made || other ? 2 : 1;
}

bool evaluate(VkCommandBuffer cmd, const TemporalFrame& f) {
    State& s = st();
    Library& l = lib();
    if (s.cur < 0 || !s.context[s.cur]) return false;
    const auto now = std::chrono::steady_clock::now();
    const float frame_ms = s.evals ? std::chrono::duration<float, std::milli>(now - s.last).count() : 1000.0f / 30.0f;
    s.last = now;
    const bool hdr = s.slot[s.cur].hdr;

    constexpr VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, R = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    constexpr VkAccessFlags2 any = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    const VkImageMemoryBarrier2 in[4] = {barrier(f.color.image, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT, G, R),
                                         barrier(f.depth.image, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT, G, R),
                                         barrier(f.motion.image, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT, G, R),
                                         barrier(f.output.image, any, any, G, G)};
    barriers(cmd, in, 4);

    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmd;
    d.color = resource(f.color, VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = resource(f.depth, VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = resource(f.motion, VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = resource(f.output, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {f.jitter[0], f.jitter[1]};   // (render pixels, as the projection was offset: the same as DLSS's)
    d.motionVectorScale = {1.0f, 1.0f};             // (render pixels already, towards the frame before)
    d.renderSize = {f.render_w, f.render_h};
    d.upscaleSize = {f.output.width, f.output.height};
    d.enableSharpening = false;   // (the Sharpness row sharpens at the window, after the HUD)
    d.sharpness = 0.0f;
    d.frameTimeDelta = frame_ms;
    d.preExposure = 1.0f;
    d.reset = f.reset || s.last_drawn != s.cur;
    // (the scene's projection's planes and field of view, as the chain read them; none known: AMD's defaults as NX2navi gives)
    d.cameraNear = f.near_z > 0 ? f.near_z : 0.1f;
    d.cameraFar = f.far_z > 0 ? f.far_z : 1e8f;
    d.cameraFovAngleVertical = f.fov_y > 0 ? f.fov_y : 1.0f;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = hdr ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;   // (HDR: linear light as it is)
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (ffxDispatch records into cmd itself: what this thread recorded through vk_work.h reaches cmd first)
    const ffxReturnCode_t r = l.dispatch(&s.context[s.cur]->context, &d.header);
    note_work();   // (AMD's dispatches: full_barrier must not take the next barrier for a repeat of the one before them)
    s.slot[s.cur].fence = std::max(s.slot[s.cur].fence, draw_recording_value());   // (recorded into cmd, a refusal too, for all this side knows)

    const VkImageMemoryBarrier2 out[4] = {barrier(f.color.image, VK_ACCESS_2_SHADER_READ_BIT, any, R, G),
                                          barrier(f.depth.image, VK_ACCESS_2_SHADER_READ_BIT, any, R, G),
                                          barrier(f.motion.image, VK_ACCESS_2_SHADER_READ_BIT, any, R, G),
                                          barrier(f.output.image, any, any, G, G)};
    barriers(cmd, out, 4);
    if (r != FFX_API_RETURN_OK) {
        // (for good: Triforce's own FSR 3.1 from the next frame)
        g_failed = true;
        logf("vk: FSR 3.1: AMD's dispatch failed (%d): Triforce's own FSR 3.1 from now on", (int)r);
        return false;
    }
    s.last_drawn = s.cur;
    if (!s.told_first)
        s.told_first = true, logf("vk: FSR 3.1: drawn by AMD's library %s (not Triforce's own passes)", l.path.c_str());
    // (the first evaluation, and the first resets: a camera cut each)
    if (s.evals++ == 0 || (f.reset && s.told_resets < 40))
        s.told_resets++, logf("vk: FSR 3.1: %ux%u -> %ux%u, jitter %+.3f %+.3f px%s", f.render_w, f.render_h, f.output.width,
                              f.output.height, f.jitter[0], f.jitter[1], f.reset ? " (history reset)" : "");
    return true;
}

uint64_t memory() {
    State& s = st();
    uint64_t m = 0;
    for (const auto& c : s.context)
        if (c) m += c->memory;
    return m;
}

const TemporalEngine kEngine{"AMD FSR 3.1", "FSR", start, prepare, evaluate, release, memory};

}  // namespace

bool ffx3_usable() {
    if (g_failed.load(std::memory_order_relaxed)) return false;
    // (asked every frame: the library's lock only for the first answer)
    static std::atomic<int> loaded{-1};
    int v = loaded.load(std::memory_order_acquire);
    if (v < 0) loaded.store(v = load() ? 1 : 0, std::memory_order_release);
    return v == 1;
}

const TemporalEngine* ffx3_engine() { return &kEngine; }

}  // namespace mkwvk
