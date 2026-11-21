// (hookshot) AMD FSR 4, one of the temporal upscaler's engines (vk_fsr4.h). The frame chain it draws with is
// vk_temporal.cpp's, the same as NVIDIA DLSS's: the colour as the game's network samples it, the depth, the motion (the
// camera's and the skinned draws' own) and the camera jitter. Here are AMD's library, the upscale context and its dispatch.
// The host side follows the API guide of FidelityFX SDK 2.4 (Kits/FidelityFX/docs/getting-started/ffx-api.md): a Vulkan
// backend description chained to the upscale context description, inputs read in SHADER_READ_ONLY_OPTIMAL, the output
// written in GENERAL. First written for Lichii's OoT branch (madebyjordan, vk_fsr4_upscaler.cpp), GPL-3.0-or-later; its Linux
// path (a Vulkan FSR 4 library through dlopen) by N3oTraX.
//
// AMD's library is looked for in RX_VK_FSR_DIR=<folder> when set, the executable's folder (bin\ beside the launcher), then
// the game folder: amd_fidelityfx_loader_vk.dll, with amd_fidelityfx_upscaler_vk.dll beside it (Linux: a Vulkan FSR 4
// library, amd_fidelityfx_upscaler_vk.so). Of the upscalers it lists for this GPU the newest FSR 4 is taken, never an older
// one in its place. RX_VK_FSR4=0: FSR 4 out for this run.
//
// (Triforce) AMD FSR 4's frame generation, the Frame generation row's AMD FSR 4, is at the end of this file (fsr4fg): the same
// loader, with amd_fidelityfx_framegeneration_vk.dll beside it.
#include "vk_fsr4.h"

#include <atomic>
#include <cstdlib>

#include "fsr_sharpen.h"
#include "vk_dlssg.h"   // (Triforce: frame generation is not asked of AMD's library beside NVIDIA's)
#include "vk_internal.h"
#include "vk_menu.h"    // (Triforce: the Frame generation row, opts.framegen)
#include "vk_temporal.h"

#ifdef HOOKSHOT_FSR
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "temporal_rules.h"   // ((Triforce, FSR4-9, FSR4-10) plan_context, frame_ms)
#include "vk_paths.h"
#include "vk_texture.h"

#include "api/include/ffx_api.h"
#include "api/include/ffx_api_types.h"
#include "api/include/vk/ffx_api_vk.h"
#include "upscalers/include/ffx_upscale.h"
#ifdef _WIN32
#include "framegeneration/include/ffx_framegeneration.h"   // (Triforce: fsr4fg, at the end of this file)
#endif
#endif
#include "vk_work.h"   // (after the others: the barriers here count as work for full_barrier)

namespace mkwvk {

namespace {
int env_switch(const char* name) {   // (-1: unset)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}
// (what is known of FSR on this device: -1 not yet, else an FsrWhy. Set when AMD's library has been asked)
std::atomic<int> g_why{-1};
// ((Triforce, FSR4-7) the names the menu shows, by the version AMD's library runs here: "AMD FSR 4.1" and "AMD FSR 4.1
// Native AA". Written once by load() before g_named is set, read on any thread without a lock (the menu never waits)
char g_name[2][40] = {"AMD FSR 4", "AMD FSR 4 Native AA"};
std::atomic<bool> g_named{false};
}  // namespace

const char* fsr4_name(bool native_aa) {
    static const char* const plain[2] = {"AMD FSR 4", "AMD FSR 4 Native AA"};
    return g_named.load(std::memory_order_acquire) ? g_name[native_aa ? 1 : 0] : plain[native_aa ? 1 : 0];
}

FsrWhy fsr4_why() {
    if (!fsr4_built()) return FSR_NOT_BUILT;
    static const bool env_off = env_switch("RX_VK_FSR4") == 0;
    if (env_off) return FSR_ENV_OFF;
    const int w = g_why.load(std::memory_order_relaxed);
    return w < 0 ? FSR_OK : (FsrWhy)w;
}

#ifdef HOOKSHOT_FSR

namespace {

#ifdef _WIN32
constexpr const char* kLoader = "amd_fidelityfx_loader_vk.dll";
#else
// (linux) no AMD loader exists there: the upscaler library itself, which exports the ffx entry points (a Vulkan FSR 4 build)
constexpr const char* kLoader = "amd_fidelityfx_upscaler_vk.so";
#endif

/// AMD's library, loaded once (the menu's thread may ask first: fsr4_probe), and the upscaler it has for this device
struct Library {
    std::mutex mu;
    bool tried = false, ok = false;
    // (Triforce) the loader and its entry points alone, for frame generation too (fsr4fg): there or not whatever upscaler
    // it lists. dir: the folder it was found in, where its other libraries lie
    bool lib_tried = false, lib_ok = false;
    std::string dir;
    HMODULE lib = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;   // (frame generation's alone: an upscaler library without it still upscales)
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    uint64_t version_id = 0;
    VkDeviceContext device_context{};
};
// (lib() and st() are never destroyed: Hookshot leaves through ExitProcess / exit() with the device alive, and a destructor
// then would call into AMD's library after its DLL_PROCESS_DETACH, or under a render thread still recording with it)
Library& lib() {
    static Library& l = *new Library;
    return l;
}

/// An upscale context and the descriptions it was made from: it keeps pointers into them until it is destroyed, which
/// happens a few frames after it is let go (temporal_later), so each context has its own
struct Context {
    ffxCreateBackendVKDesc backend{};
    ffxOverrideVersion version{};
    // ((Triforce, FSR4-4) the API version this program was built with, which the SDK asks every upscale context to say
    // (ffx_upscale.h: "must be set to FFX_UPSCALER_VERSION"), as NX2navi's does)
    ffxCreateContextDescUpscaleVersion built{};
#ifndef _WIN32
    ffxCreateBackendVKDesc_DEPRECATED legacy_backend{};   // (linux) the backend description a Vulkan FSR 4 build like fsr4-vk reads
#endif
    ffxCreateContextDescUpscale upscale{};
    ffxContext context = nullptr;
    uint32_t width = 0, height = 0;   // (its output)
    // ((Triforce, FSR4-9) the render size it was made for: with drs any size up to the output's (the game's dynamic
    // resolution), else that one alone, as AMD's sample makes them)
    uint32_t render_w = 0, render_h = 0;
    bool drs = false;
    bool hdr = false;                 // (made for linear HDR colour: the other kind of colour makes it again)
    uint64_t memory = 0;              // (what AMD's library says it holds)
    ~Context() {
        if (context) lib().destroy(&context, nullptr);
    }
};

/// The engine's state: used with the temporal upscaler's lock held
struct State {
    std::shared_ptr<Context> context;
    uint32_t in_w = 0, in_h = 0;   // (the inputs' size under this context: the largest render size it has had)
    // ((Triforce, FSR4-9) the render size changed under an output: the game's dynamic resolution, one context for every size
    // from then on)
    bool dynamic = false;
    std::chrono::steady_clock::time_point last{};
    uint64_t evals = 0;
    int told_resets = 0;
    float told_sharpness = -1.0f;   // ((Triforce, S1) the sharpness the log last said)
};
State& st() {
    static State& s = *new State;   // (never destroyed: lib()'s note)
    return s;
}

void on_message(uint32_t type, const wchar_t* message) {
    char text[512] = {};
    if (message) WideCharToMultiByte(CP_UTF8, 0, message, -1, text, (int)sizeof text - 1, nullptr, nullptr);
    logf("vk: AMD FSR 4: AMD's %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

/// The folders the library is looked for in: RX_VK_FSR_DIR, the executable's (bin\ beside the launcher), the game folder
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

/// (Triforce) AMD's loader and its entry points, once (l.mu held): false when it is not there, which the log says. The
/// upscaler (load) and frame generation (fsr4fg) both start here
bool load_library(Library& l) {
    if (l.lib_tried) return l.lib_ok;
    l.lib_tried = true;
    std::string where;
    for (const std::string& dir : library_dirs()) {
        where += (where.empty() ? "" : ", ") + dir;
#ifdef _WIN32
        const std::string path = dir + "\\" + kLoader;
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        // (the upscaler DLL the loader picks lies beside it: the loader's folder first in the search)
        l.lib = LoadLibraryExA(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        // (there but not loaded: a DLL it needs missing, a bad signature, or a relative RX_VK_FSR_DIR, which the search
        // flags refuse)
        if (!l.lib) logf("vk: AMD FSR 4: %s not loaded (Windows error %lu)", path.c_str(), (unsigned long)GetLastError());
#else
        const std::string path = dir + "/" + kLoader;
        if (access(path.c_str(), R_OK) != 0) continue;
        l.lib = (HMODULE)dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!l.lib) logf("vk: AMD FSR 4: %s not loaded: %s", path.c_str(), dlerror());
#endif
        if (l.lib) {
            logf("vk: AMD FSR 4: %s", path.c_str());
            l.dir = dir;
            break;
        }
    }
    if (!l.lib) {
        logf("vk: AMD FSR 4: no %s in %s (the readme says where AMD's files come from)", kLoader, where.c_str());
        return false;
    }
    l.create = (PfnFfxCreateContext)symbol("ffxCreateContext");
    l.destroy = (PfnFfxDestroyContext)symbol("ffxDestroyContext");
    l.configure = (PfnFfxConfigure)symbol("ffxConfigure");
    l.query = (PfnFfxQuery)symbol("ffxQuery");
    l.dispatch = (PfnFfxDispatch)symbol("ffxDispatch");
    if (!l.create || !l.destroy || !l.query || !l.dispatch) {
        logf("vk: AMD FSR 4: %s lacks the FSR API's entry points", kLoader);
        return false;
    }
    l.device_context = VkDeviceContext{dev.device, dev.physical, vkGetDeviceProcAddr};
    return l.lib_ok = true;
}

/// AMD's library on this device, once: false when FSR 4 cannot run here (g_why says why, the log too)
bool load() {
    Library& l = lib();
    std::lock_guard lk(l.mu);
    if (l.tried) return l.ok;
    l.tried = true;
    const auto no = [](FsrWhy why) {
        g_why = why;
        return false;
    };
    if (!load_library(l)) return no(FSR_NO_LIBRARY);
    // (the library lists every upscaler this device can run: the newest 4.x is taken explicitly, so an FSR 3.1 provider never
    // stands in silently)
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = &l.device_context;
    uint64_t count = 0;
    versions.outputCount = &count;
    if (l.query(nullptr, &versions.header) != FFX_API_RETURN_OK || !count) {
        logf("vk: AMD FSR 4: AMD's library lists no upscaler for this GPU");
        return no(FSR_NO_MODEL);
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    if (l.query(nullptr, &versions.header) != FFX_API_RETURN_OK) {
        logf("vk: AMD FSR 4: the upscalers could not be listed");
        return no(FSR_NO_MODEL);
    }
    std::string listed;
    unsigned best[3] = {0, 0, 0};
    for (uint64_t i = 0; i < count; i++) {
        const char* name = names[i] ? names[i] : "";
        listed += (listed.empty() ? "" : ", ") + std::string(name);
        unsigned v[3] = {0, 0, 0};
        sscanf(name, "%u.%u.%u", &v[0], &v[1], &v[2]);
        // (the first of equal versions: a Vulkan FSR 4 build lists its Auto, which picks the GPU's model, before INT8 and FP8)
        if (v[0] == 4 && (!l.version_id || v[1] > best[1] || (v[1] == best[1] && v[2] > best[2]))) {
            std::memcpy(best, v, sizeof best);
            l.version_id = ids[i];
        }
    }
    if (!l.version_id) {
        // ((Triforce, FSR4-7) on a Radeon RX 6000 or 7000 the driver decides: AMD's 26.10.1 or newer brings FSR 4 there)
        logf("vk: AMD FSR 4: no FSR 4 for this GPU and driver (AMD's library has: %s; FSR 4 needs a Radeon RX 6000, 7000 or "
             "9000 and AMD's driver 26.10.1 or newer)", listed.c_str());
        return no(FSR_NO_MODEL);
    }
    logf("vk: AMD FSR 4: upscalers %s; using %u.%u.%u", listed.c_str(), best[0], best[1], best[2]);
    // ((Triforce, FSR4-7) the menu's name for it: its major and minor, as NX2navi's bd7d526 shows it)
    snprintf(g_name[0], sizeof g_name[0], "AMD FSR %u.%u", best[0], best[1]);
    snprintf(g_name[1], sizeof g_name[1], "AMD FSR %u.%u Native AA", best[0], best[1]);
    g_named.store(true, std::memory_order_release);
    l.ok = true;
    g_why = FSR_OK;
    return true;
}

/// (Triforce) RX_VK_FSR4_AUTO_EXPOSURE=0: an HDR context without FFX_UPSCALE_ENABLE_AUTO_EXPOSURE (FSR takes the colour's
/// exposure as 1), to compare with the default where the colour is the tone mapped picture (FSR4-3)
bool hdr_auto_exposure() {
    static const bool on = env_switch("RX_VK_FSR4_AUTO_EXPOSURE") != 0;
    return on;
}

/// A context for outputs of ow x oh. drs: any render size up to the output's (the game's dynamic resolution); else the
/// render size rw x rh alone, as AMD's sample makes them ((Triforce, FSR4-9): NX2navi's 7e938ec saw AMD's driver fail in the
/// dispatch on an RX 7900 XT when the render size changed under one context, and moved to one context a render size)
std::shared_ptr<Context> make_context(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool drs, bool hdr) {
    Library& l = lib();
    auto c = std::make_shared<Context>();
    c->backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    c->backend.deviceContext = l.device_context;
    c->version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    c->version.versionId = l.version_id;
    c->backend.header.pNext = &c->version.header;
    c->built.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    c->built.version = FFX_UPSCALER_VERSION;
    c->version.header.pNext = &c->built.header;
#ifndef _WIN32
    // (linux) fsr4-vk looks for the SDK's older backend description, not ffxCreateBackendVKDesc: without it, it finds no device
    // and creation returns FFX_API_RETURN_ERROR_PARAMETER. Both stay chained
    c->legacy_backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED;
    c->legacy_backend.vkDevice = l.device_context.vkDevice;
    c->legacy_backend.vkPhysicalDevice = l.device_context.vkPhysicalDevice;
    c->legacy_backend.vkDeviceProcAddr = l.device_context.vkDeviceProcAddr;
    c->built.header.pNext = &c->legacy_backend.header;
#endif
    c->upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    c->upscale.header.pNext = &c->backend.header;
    // (the chain's colour is tonemapped and square-root encoded already: LDR, perceptual, no auto exposure; depth not
    // inverted)
    c->upscale.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    // (hdr, linear HDR colour: the chain before the game's bloom and tone mapping, the scene's light before the game's
    // exposure, so FSR finds the exposure itself; or (Triforce, FSR4-3) an RGBA16F picture's, RenoDX's HDR Native)
    if (hdr) c->upscale.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | (hdr_auto_exposure() ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0);
    if (drs) c->upscale.flags |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    c->upscale.maxRenderSize = drs ? FfxApiDimensions2D{ow, oh} : FfxApiDimensions2D{rw, rh};
    c->upscale.maxUpscaleSize = {ow, oh};
    c->upscale.fpMessage = &on_message;
    const auto t0 = std::chrono::steady_clock::now();
    const ffxReturnCode_t r = l.create(&c->context, &c->upscale.header, nullptr);
    const double made_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != FFX_API_RETURN_OK || !c->context) {
        logf("vk: AMD FSR 4: its upscaler was not made (%d) for %ux%u -> %ux%u%s", (int)r, rw, rh, ow, oh,
             drs ? " (any render size)" : "");
        c->context = nullptr;
        return nullptr;
    }
    c->width = ow, c->height = oh, c->render_w = drs ? ow : rw, c->render_h = drs ? oh : rh, c->drs = drs, c->hdr = hdr;
    FfxApiEffectMemoryUsage usage{};
    ffxQueryDescUpscaleGetGPUMemoryUsage mem{};
    mem.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    mem.gpuMemoryUsageUpscaler = &usage;
    if (l.query(&c->context, &mem.header) == FFX_API_RETURN_OK) c->memory = usage.totalUsageInBytes;
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const bool known = l.query(&c->context, &provider.header) == FFX_API_RETURN_OK && provider.versionName;
    logf("vk: AMD FSR 4: FSR %s made for %s%ux%u -> %ux%u, %s colour%s (frame %llu, %.1f ms of CPU; its own %llu MB)",
         known ? provider.versionName : "?", drs ? "render sizes up to " : "", c->render_w, c->render_h, ow, oh,
         hdr ? "HDR" : "LDR", hdr && !hdr_auto_exposure() ? " without auto exposure" : "", (unsigned long long)texture_frame_no(),
         made_ms, (unsigned long long)(c->memory >> 20));
    // ((Triforce, FSR4-8) the provider asked for the version it runs: anything but a 4.x means the loader gave another upscaler
    // in FSR 4's place, which is never shown under its name (NX2navi's vk_ffx4.cpp does the same). The context goes with c)
    if (known && atoi(provider.versionName) < 4) {
        logf("vk: AMD FSR 4: the context runs FSR %s, not FSR 4: FSR 4 off for this run", provider.versionName);
        {
            std::lock_guard lk(l.mu);
            l.ok = false;
        }
        g_why = FSR_NO_MODEL;
        return nullptr;
    }
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

#ifndef _WIN32
/// (linux) a Vulkan FSR 4 build takes no frame without an exposure, and the chain's colour is tonemapped already: a 1x1 R32F of 1
bool unit_exposure(VkCommandBuffer cmd, TemporalImage& out) {
    static VkImage image = VK_NULL_HANDLE;
    static VmaAllocation allocation{};
    const bool made = image != VK_NULL_HANDLE;
    if (!made) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R32_SFLOAT, ci.extent = {1, 1, 1}, ci.mipLevels = 1, ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!create_image(ci, image, allocation)) return image = VK_NULL_HANDLE, false;
    }
    out = TemporalImage{image, VK_NULL_HANDLE, VK_FORMAT_R32_SFLOAT, 1, 1};
    if (!made) {
        const VkImageMemoryBarrier2 to_clear = barrier(image, 0, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        barriers(cmd, &to_clear, 1);
        const VkClearColorValue one{{1.0f, 0.0f, 0.0f, 0.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
        const VkImageMemoryBarrier2 to_read = barrier(image, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barriers(cmd, &to_read, 1);
    }
    return true;
}
#endif

// ---- the engine (vk_temporal.h TemporalEngine) ----

bool start() { return load(); }

void release() {
    State& s = st();
    if (!s.context) return;
    std::shared_ptr<Context> old = std::move(s.context);
    temporal_later([old] {});   // (destroyed with the last copy, a few frames on: recorded dispatches may still use it)
    s.context.reset();
}

int prepare(VkCommandBuffer, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool hdr,
            uint32_t* in_w, uint32_t* in_h, const char** why) {
    State& s = st();
    int made = 1;
    // ((Triforce, FSR4-9) a fixed share has a context of its own size; the first time the render size changes under an output
    // (the game's dynamic resolution: Auto), one context for any size up to the output's from then on. RX_VK_FSR4_DRS=1:
    // that one from the start, as before)
    static const bool drs_env = env_switch("RX_VK_FSR4_DRS") > 0;
    const Context* const c = s.context.get();
    const temporal_rules::ContextPlan plan =
        c ? temporal_rules::plan_context(true, c->width, c->height, c->render_w, c->render_h, c->drs, c->hdr, rw, rh, ow, oh,
                                         hdr, s.dynamic, drs_env)
          : temporal_rules::plan_context(false, 0, 0, 0, 0, false, false, rw, rh, ow, oh, hdr, s.dynamic, drs_env);
    if (plan.went_dynamic)
        logf("vk: AMD FSR 4: the render size went from %ux%u to %ux%u: one context for every render size from now on (the "
             "game's dynamic resolution)", c->render_w, c->render_h, rw, rh);
    s.dynamic = plan.dynamic;
    if (plan.remake) {
        release();
        s.context = make_context(rw, rh, ow, oh, plan.drs, hdr);
        if (!s.context) return *why = "its upscaler was not made", -1;
        s.in_w = s.in_h = 0;
        made = 2;
    }
    // (the inputs as large as the largest render size this context has had, never past the output: a fixed share keeps
    // images of its own size, and the game's dynamic resolution only makes them again when it goes past them)
    s.in_w = std::min(std::max(s.in_w, rw), ow), s.in_h = std::min(std::max(s.in_h, rh), oh);
    *in_w = s.in_w, *in_h = s.in_h;
    return made;
}

bool evaluate(VkCommandBuffer cmd, const TemporalFrame& f) {
    State& s = st();
    Library& l = lib();
#ifndef _WIN32
    TemporalImage exposure{};
    if (!unit_exposure(cmd, exposure)) return false;
#endif
    const auto now = std::chrono::steady_clock::now();
    // ((Triforce, FSR4-10) under 100 ms, else a 60th, as the frame generation's below: a single long hitch, or the first
    // frame, is no rate for FSR's history to follow)
    const float frame_ms = temporal_rules::frame_ms(!s.evals, std::chrono::duration<float, std::milli>(now - s.last).count());
    s.last = now;

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
#ifndef _WIN32
    d.exposure = resource(exposure, VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
#endif
    d.jitterOffset = {f.jitter[0], f.jitter[1]};   // (render pixels, as the projection was offset: the same as DLSS's)
    d.motionVectorScale = {1.0f, 1.0f};             // (render pixels already, towards the frame before)
    d.renderSize = {f.render_w, f.render_h};
    d.upscaleSize = {f.output.width, f.output.height};
    // ((Triforce, S1) the Sharpness row as FSR 4's own sharpening, in AMD's meaning (0 none, 1 the most), on the 3D before
    // the HUD: the window pass then sharpens nothing (vk_temporal.h temporal_sharpens_3d). It was off here, the row sharpening
    // the finished picture at the window, the HUD and text with it, and FSR 4's picture stayed as soft as it came)
    // (linux fork: the FSR sharpening row, when set, is AMD's RCAS strength over the Sharpness row's, raised below a 75% render
    // share: fsr_sharpen.h)
    const float sharpness = fsr_sharpen::enabled(opts.fsr_sharpness)
                                ? fsr_sharpen::strength(opts.fsr_sharpness, f.render_w, f.output.width)
                                : temporal_sharpness();
    d.enableSharpening = sharpness > 0.0f;
    d.sharpness = sharpness;
    if (sharpness != s.told_sharpness) {
        s.told_sharpness = sharpness;
        if (sharpness > 0.0f) logf("vk: AMD FSR 4: its own sharpening at %.2f, before the HUD", sharpness);
        else logf("vk: AMD FSR 4: its own sharpening off");
    }
    d.frameTimeDelta = frame_ms;
    d.preExposure = 1.0f;
    d.reset = f.reset;
    d.cameraNear = f.near_z, d.cameraFar = f.far_z, d.cameraFovAngleVertical = f.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = s.context->hdr ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;   // (HDR: linear light as it is)
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (ffxDispatch records into cmd itself: what this thread recorded through vk_work.h reaches cmd first)
    const ffxReturnCode_t r = l.dispatch(&s.context->context, &d.header);
    note_work(cmd);   // (AMD's dispatches: full_barrier must not take the next barrier for a repeat of the one before them)

    const VkImageMemoryBarrier2 out[4] = {barrier(f.color.image, VK_ACCESS_2_SHADER_READ_BIT, any, R, G),
                                          barrier(f.depth.image, VK_ACCESS_2_SHADER_READ_BIT, any, R, G),
                                          barrier(f.motion.image, VK_ACCESS_2_SHADER_READ_BIT, any, R, G),
                                          barrier(f.output.image, any, any, G, G)};
    barriers(cmd, out, 4);
    if (r != FFX_API_RETURN_OK) {
        logf("vk: AMD FSR 4: its dispatch failed (%d)", (int)r);
        return false;
    }
    // (the first evaluation, and the first resets: a camera cut each)
    if (s.evals++ == 0 || (f.reset && s.told_resets < 40))
        s.told_resets++, logf("vk: AMD FSR 4: %ux%u -> %ux%u, jitter %+.3f %+.3f px%s", f.render_w, f.render_h, f.output.width,
                              f.output.height, f.jitter[0], f.jitter[1], f.reset ? " (history reset)" : "");
    return true;
}

uint64_t memory() {
    State& s = st();
    return s.context ? s.context->memory : 0;
}

const TemporalEngine kEngine{"AMD FSR 4", "FSR", start, prepare, evaluate, release, memory};

}  // namespace

bool fsr4_built() { return true; }
const TemporalEngine* fsr4_engine() { return &kEngine; }

void fsr4_probe() {
    if (fsr4_why() != FSR_OK || g_why.load(std::memory_order_relaxed) >= 0 || !dev.device) return;
    load();
}

bool fsr4_wants_descriptor_buffer() {
#ifdef _WIN32
    return false;
#else
    if (fsr4_why() == FSR_ENV_OFF) return false;
    for (const std::string& dir : library_dirs())
        if (access((dir + "/" + kLoader).c_str(), R_OK) == 0) return true;
    return false;
#endif
}

// ---- (Triforce) AMD FSR 4's frame generation (vk_fsr4.h fsr4fg) ----
//
// AMD's frame generation context, of the same FidelityFX SDK 2.4 as the upscaler. Its loader picks the provider for the
// card: the machine-learned FSR Frame Generation 4.x on the Radeon cards AMD lists it for (RX 9000), FSR Frame Generation
// 3.1.x on every other. ((Triforce, FG-1) Both are taken now, 4.x first where listed (framegen_rows.h pick_version), and the
// row names the version: "AMD FSR (3.1.6)" on an RX 7900 XT. Before, 3.1.x was refused and the row never offered AMD's
// frame generation on RX 6000 and 7000.)
//
// How it is driven. AMD's API has two ways to get the made frame on screen. One is a swapchain of AMD's in the window's
// place (ffxCreateContextDescFrameGenerationSwapChainVK), which presents and paces the frames on queues and threads of its
// own; NX2navi runs AMD's library that way (its runtime/vk_fg4.cpp). The other is to ask the context for the frame:
// ffxConfigure with FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY ("the context should only run frame interpolation
// and not modify the swapchain", ffx_framegeneration.h), the preparation dispatch with the scene's depth and motion, then
// ffxDispatchDescFrameGeneration with the presented picture and an image of ours for the made frame: the description AMD's
// swapchain itself fills in for its callback. That is the way taken here, because this renderer already presents made
// frames itself: vk_framegen.cpp's display thread shows them between the game's, paced on its own line, through the one
// present path (the window scaling, HDR, the overlays). So nothing of the swapchain, the queues or the presents changes
// with this choice, AMD's library has no thread of its own to stop at the exit, and a present it does not take gets the
// renderer's own made frame.
//
// ((Triforce, FG-3) Why the swapchain-less way is kept. AMD's header describes the flag as a supported mode, "the context
// should only run frame interpolation and not modify the swapchain"; the generation's description
// (ffxDispatchDescFrameGeneration: a command list, the present colour, up to four outputs, the frame id) is a public
// dispatch type, and the very one AMD's swapchain hands its frameGenerationCallback, whose default in AMD's samples is
// ffxDispatch of it on the same context. Taking AMD's swapchain instead (NX2navi's runtime/vk_fg4.cpp) would put AMD's present
// and acquire queues and threads in place of this renderer's window, HDR, scaling, overlays and display thread, with the queue
// family work NX2navi 293731d needed on AMD's driver: a far larger change, for the same three calls. Not yet seen on a card:
// every way the library refuses is logged (fail, and the first frame's "the first made frame" line), and a refusal leaves
// the renderer's own interpolation.)
//
// The HUD ((Triforce, FG-2)): the picture given is the game's finished one, HUD included, and with it, as
// ffxConfigureDescFrameGeneration::HUDLessColor, the same picture without the HUD: the gameplay upscale's output before the
// HUD is drawn into it, encoded as the game's last pass encodes it (vk_framegen.cpp hudless_for). AMD's library moves the
// scene by that one and keeps what differs between the two, the HUD, where the game drew it. No frame is made without it
// (GAME: the game's frame alone, NX2navi 5bdf583's rule): FSR 3.1's frame interpolation here (vk_fsr3fi.cpp), without
// one, moved the HUD with the scene (#1635). Nor in HDR Native, whose picture without the HUD the game alone could encode,
// nor in a run with RenoDX's RGBA16F scene: those hold for every present (held()), and there the renderer's own
// interpolation makes the one frame, as at 2x, as a saved 9 ran before AMD's FSR 3.1 frame generation was offered.
#ifdef _WIN32
namespace fsr4fg {
namespace {
namespace fgr = framegen_rows;

constexpr const char* kFgFile = "amd_fidelityfx_framegeneration_vk.dll";
// (the images' uses, for AMD's descriptions of them: the made frame's image is ours, written as a storage image or as a
// colour attachment, whichever AMD's last pass does; the picture is vk_framegen.cpp's copy of the frame, the depth and the
// motion vk_fsr3_inputs.cpp's)
constexpr VkImageUsageFlags kOutUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageUsageFlags kColorUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
constexpr VkImageUsageFlags kInUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

struct Fg {
    std::mutex mu;   // (every call into AMD's library for frame generation, and all of this)
    uint64_t version_id = 0;
    char version[24] = "";
    ffxContext context = nullptr;
    // (what the context was made from: the API asks that it stays until the context is destroyed)
    ffxCreateContextDescFrameGeneration desc{};
    ffxCreateContextDescFrameGenerationHudless hudless{};   // ((Triforce, FG-2) the picture without the HUD's format)
    ffxCreateContextDescFrameGenerationVersion api{};
    ffxCreateBackendVKDesc backend{};
    ffxOverrideVersion pin{};
    fgr::ContextKey key{};
    VkImage out = VK_NULL_HANDLE;   // (the made frame: R8G8B8A8_UNORM at the picture's size, GENERAL)
    VmaAllocation out_memory{};
    bool out_new = false;           // (made, and not in GENERAL yet)
    bool made = false;              // (the last generate answered MADE)
    uint64_t memory = 0;            // (what AMD's library says the context holds)
    uint64_t frame_id = 0;          // (AMD's: one more for each frame handed over)
    uint64_t sequence = 0;          // (Inputs::sequence of the last frame handed over)
    fgr::Pose pose{};               // (its camera, for the cuts)
    bool have_pose = false;
    uint64_t presents = 0, used_at = 0;   // (present_tick's count, and its value at the last frame handed over)
    uint64_t n_made = 0, n_skipped = 0, n_cuts = 0;
    // ((Triforce) each way a present is refused said once: no depth and motion, no picture without the HUD, HDR Native)
    bool told_bare = false, told_held[fgr::HELD_COUNT] = {};
    uint64_t n_held[fgr::HELD_COUNT] = {};
};
Fg& fg() {
    static Fg& f = *new Fg;   // (never destroyed: lib()'s note)
    return f;
}
std::atomic<int> g_fg_why{-1};          // (-1: AMD's library not asked yet; else a framegen_rows.h Why)
std::atomic<bool> g_fg_failed{false};   // (it failed in this run: off until the next start)
std::atomic<bool> g_fg_exiting{false};  // (HOS_OPT_EXITING: nothing of AMD's is called any more)
std::atomic<bool> g_fg_live{false};     // (a context exists: present_tick has something to look after)

int fg_env() {   // (RX_VK_FSR4_FG: 0 never; 4 FSR 4's alone, as before (FG-1); 1 or unset (-1): the newest AMD's library lists)
    static const int e = env_switch("RX_VK_FSR4_FG");
    return e;
}

/// AMD's library asked once (s.mu held, the device made): whether it has an FSR 4 frame generation for this card and driver
fgr::Why probe(Fg& s) {
    Library& l = lib();
    {
        std::lock_guard lk(l.mu);
        if (!load_library(l)) return fgr::FG_NO_LIBRARY;
    }
    if (!l.configure) {
        logf("vk: AMD FSR 4 frame generation: %s has no ffxConfigure", kLoader);
        return fgr::FG_NO_LIBRARY;
    }
    const std::string file = l.dir + "\\" + kFgFile;
    if (GetFileAttributesA(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        logf("vk: AMD FSR 4 frame generation: no %s beside AMD's loader in %s", kFgFile, l.dir.c_str());
        return fgr::FG_NO_FILE;
    }
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    versions.device = &l.device_context;
    uint64_t count = 0;
    versions.outputCount = &count;
    if (l.query(nullptr, &versions.header) != FFX_API_RETURN_OK || !count) {
        logf("vk: AMD FSR 4 frame generation: AMD's library lists no frame generation for this GPU");
        return fgr::FG_NO_MODEL;
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    if (l.query(nullptr, &versions.header) != FFX_API_RETURN_OK || !count || count > ids.size()) {
        logf("vk: AMD FSR 4 frame generation: the frame generations could not be listed");
        return fgr::FG_NO_MODEL;
    }
    std::string listed;
    for (uint64_t i = 0; i < count; i++) listed += (listed.empty() ? "" : ", ") + std::string(names[i] ? names[i] : "?");
    const bool only4 = fg_env() == 4;
    const int at = fgr::pick_version(names.data(), (int)count, only4);
    if (at < 0) {
        logf("vk: AMD FSR 4 frame generation: no frame generation it can take for this GPU (AMD's library has: %s%s)",
             listed.c_str(), only4 ? "; RX_VK_FSR4_FG=4: FSR 4's alone" : "");
        return fgr::FG_NO_MODEL;
    }
    s.version_id = ids[at];
    snprintf(s.version, sizeof s.version, "%s", names[at]);
    unsigned v[3];
    const bool fsr31 = fgr::parse_version(s.version, v) && v[0] < 4;
    logf("vk: AMD FSR 4 frame generation: AMD's library has %s; using %s%s (the Frame generation row's AMD FSR)", listed.c_str(),
         s.version, fsr31 ? ", FSR 3.1's frame generation: AMD lists FSR 4's for RX 9000 alone" : "");
    return fgr::FG_OK;
}

FfxApiResource image_resource(VkImage image, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, uint32_t more) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = format, ci.extent = {w, h, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage, ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // (GENERAL, which AMD's Vulkan backend takes for the unordered-access state, and leaves the image in: NX2navi hands it
    // the depth, the motion and its pictures the same way)
    return ffxApiGetResourceVK((void*)image, ffxApiGetImageResourceDescriptionVK(image, ci, more), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

/// framegen_rows.h teardown's steps in play (s.mu held by the caller: no call into AMD's library is in progress)
struct Down {
    Fg& s;
    void stop() { s.made = false; }
    void wait_gpu() {
        std::lock_guard qk(dev.present_mu);   // (every queue idle: the present queue's too, as vk_framegen.cpp's ensure_size)
        device_wait_idle_locked();
    }
    void destroy_context() {
        if (s.context) lib().destroy(&s.context, nullptr);
        s.context = nullptr, s.memory = 0;
    }
    void destroy_images() {
        if (s.out) destroy_image(s.out, s.out_memory);
        s.out = VK_NULL_HANDLE, s.out_memory = {}, s.out_new = false;
    }
};
/// ...and at the process's exit: the flag first, so that no call starts, then a call in progress is waited for (not for
/// ever: an exit never hangs on it)
struct AtExit {
    Fg& s;
    void stop() {
        g_fg_exiting.store(true);
        for (int i = 0; i < 200; i++) {
            if (s.mu.try_lock()) {
                s.made = false;
                s.mu.unlock();
                return;
            }
            Sleep(10);
        }
        logf("vk: AMD FSR 4 frame generation: a call into AMD's library did not return in 2 s at the exit");
    }
    void wait_gpu() {}
    void destroy_context() {}
    void destroy_images() {}
};

void release(Fg& s, const char* why) {
    if (!s.context && !s.out) return;
    logf("vk: AMD FSR 4 frame generation: its context is let go (%s; %llu frames made, %llu not made at a new history, %llu "
         "camera cuts among them; the game's frame alone %llu times without a picture without the HUD; %llu HDR Native "
         "pictures to the renderer's own)",
         why, (unsigned long long)s.n_made, (unsigned long long)s.n_skipped, (unsigned long long)s.n_cuts,
         (unsigned long long)s.n_held[fgr::HELD_NO_HUDLESS], (unsigned long long)s.n_held[fgr::HELD_HDR_NATIVE]);
    Down d{s};
    fgr::teardown(d, false);
    s.key = {}, s.have_pose = false, s.sequence = 0;
    g_fg_live.store(false);
}

void fail(Fg& s, const char* what, ffxReturnCode_t r) {
    // (the context stays until present_tick takes it down, two presents on: this frame's commands are recorded with it)
    logf("vk: AMD FSR 4 frame generation: %s failed (%d): off for this run, the renderer's own interpolation makes the frames",
         what, (int)r);
    s.made = false;
    g_fg_failed.store(true);
}

/// The context and the made frame's image for such frames. Each is made locally and published only when both exist
/// (docs/framegen-resource-failure.md): a failure leaves nothing behind and frame generation by AMD's library off for the run
bool make(Fg& s, const fgr::ContextKey& key) {
    Library& l = lib();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R8G8B8A8_UNORM, ci.extent = {key.width, key.height, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = kOutUsage;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory{};
    if (!create_image(ci, image, memory)) {
        fail(s, "the made frame's image", 0);
        return false;
    }
    s.backend = {};
    s.backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    s.backend.deviceContext = l.device_context;
    // (the version asked of the library by its id, so an FSR 3.1 frame generation never stands in silently)
    s.pin = {};
    s.pin.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    s.pin.versionId = s.version_id;
    s.backend.header.pNext = &s.pin.header;
    // (the version of the API this was built against, which the SDK asks for with every context)
    s.api = {};
    s.api.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    s.api.version = FFX_FRAMEGENERATION_VERSION;
    s.api.header.pNext = &s.backend.header;
    // ((Triforce, FG-2) the picture without the HUD, given each frame as HUDLessColor: 8 bits of sRGB as the picture is)
    s.hudless = {};
    s.hudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    s.hudless.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(VK_FORMAT_R8G8B8A8_UNORM);
    s.hudless.header.pNext = &s.api.header;
    s.desc = {};
    s.desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    s.desc.header.pNext = &s.hudless.header;
    // (the depth it is given is reversed, 1 at the near plane: vk_fsr3_inputs.h. The picture is the presented one, 8 bits
    // of sRGB or of HDR10 codes: not the linear HDR that flag is for)
    s.desc.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | (key.infinite_depth ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0);
    s.desc.displaySize = {key.width, key.height};
    s.desc.maxRenderSize = {key.max_render_width, key.max_render_height};
    s.desc.backBufferFormat = ffxApiGetSurfaceFormatVK(VK_FORMAT_R8G8B8A8_UNORM);
    ffxContext context = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    const ffxReturnCode_t r = l.create(&context, &s.desc.header, nullptr);
    const double made_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != FFX_API_RETURN_OK || !context) {
        destroy_image(image, memory);
        fail(s, "its context", r);
        return false;
    }
    s.context = context, s.out = image, s.out_memory = memory, s.out_new = true, s.key = key;
    s.used_at = s.presents;
    g_fg_live.store(true);
    FfxApiEffectMemoryUsage usage{};
    ffxQueryDescFrameGenerationGetGPUMemoryUsage mem{};
    mem.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
    mem.gpuMemoryUsageFrameGeneration = &usage;
    s.memory = l.query(&s.context, &mem.header) == FFX_API_RETURN_OK ? usage.totalUsageInBytes : 0;
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const bool known = l.query(&s.context, &provider.header) == FFX_API_RETURN_OK && provider.versionName;
    logf("vk: AMD FSR 4 frame generation: FSR frame generation %s made for pictures of %ux%u, scenes up to %ux%u, the far "
         "plane %s, with a picture without the HUD, no swapchain of AMD's (%.1f ms of CPU; its own %llu MB)",
         known ? provider.versionName : "?", key.width, key.height,
         key.max_render_width, key.max_render_height, key.infinite_depth ? "at infinity" : "finite", made_ms,
         (unsigned long long)(s.memory >> 20));
    return true;
}

}  // namespace

framegen_rows::Why why() {
    static const bool fsr_off = env_switch("RX_VK_FSR4") == 0;   // (FSR 4 out for this run: AMD's library is not loaded at all)
    if (fsr_off || fg_env() == 0) return fgr::FG_ENV_OFF;
    if (g_fg_failed.load(std::memory_order_relaxed)) return fgr::FG_FAILED;
    int w = g_fg_why.load(std::memory_order_relaxed);
    if (w >= 0) return (fgr::Why)w;
    if (!dev.device || dev.offscreen || g_fg_exiting.load()) return fgr::FG_NOT_YET;
    Fg& s = fg();
    std::lock_guard lk(s.mu);
    w = g_fg_why.load();
    if (w < 0 && g_fg_exiting.load()) return fgr::FG_NOT_YET;
    if (w < 0) w = (int)probe(s), g_fg_why.store(w);
    return (fgr::Why)w;
}

framegen_rows::Why why_known() {
    static const bool fsr_off = env_switch("RX_VK_FSR4") == 0;
    if (fsr_off || fg_env() == 0) return fgr::FG_ENV_OFF;
    if (g_fg_failed.load(std::memory_order_relaxed)) return fgr::FG_FAILED;
    const int w = g_fg_why.load(std::memory_order_relaxed);
    return w >= 0 ? (fgr::Why)w : fgr::FG_NOT_YET;
}

bool frames_wanted() {
    // (the row first: AMD's library is not asked anything, and nothing is logged, while the row is elsewhere)
    // ((Triforce) nor while HDR Native or an RGBA16F scene holds it from every present: no depth, motion or picture without
    // the HUD is made for frames AMD's library cannot be given, and the renderer's own makes the frame, as at 2x)
    return opts.framegen == fgr::FSR4 && fgr::fsr4_asked(opts.framegen, why(), dlssg::active()) &&
           !fgr::held_whole_run(held());
}

const char* version() { return g_fg_why.load(std::memory_order_relaxed) == fgr::FG_OK ? fg().version : ""; }

framegen_rows::Made generate(VkCommandBuffer cmd, const Inputs& in) {
    if (!cmd || !frames_wanted()) return fgr::OWN;
    Fg& s = fg();
    if (!in.color || !in.depth || !in.motion || !in.width || !in.height || !in.render_width || !in.render_height) {
        std::lock_guard lk(s.mu);
        if (!s.told_bare)   // ((Triforce) the lab's fgprobe1 run had no line for this, the reason AMD made no frame there)
            s.told_bare = true, logf("vk: AMD FSR 4 frame generation: a present without the scene's depth and motion (a menu, a movie, "
                                     "or no upscale's inputs made): the renderer's own interpolation makes its frame");
        return fgr::OWN;
    }
    std::lock_guard lk(s.mu);
    if (g_fg_exiting.load() || g_fg_failed.load()) return fgr::OWN;
    // ((Triforce, FG-2, FG-5) never a made frame with the HUD in the picture it moves. Without this frame's picture without
    // the HUD, the game's frame alone (GAME). In HDR Native (the game's own HDR encoding, which no picture without the HUD can
    // follow; frames_wanted already says no while present_native_pq is on, in.pq is this present's own word for it), the
    // renderer's own makes the frame (OWN, held_made), as a saved 9 ran on a card without AMD's frame generation
    const fgr::Held held = fgr::held_for(in.pq, false, in.hudless != VK_NULL_HANDLE);
    if (held != fgr::HELD_NONE) {
        s.n_held[held]++;
        if (!s.told_held[held]) {
            s.told_held[held] = true;
            logf("vk: AMD FSR 4 frame generation: %s (first at present %llu)",
                 held == fgr::HELD_HDR_NATIVE
                     ? "paused with HDR Native (the game encodes its HDR picture itself, and AMD's frame generation needs that "
                       "picture without the HUD, which only the game could encode): the renderer's own makes the frame, as at 2x"
                     : "no picture without the HUD this frame (it is taken at the gameplay upscale's output, before the HUD): "
                       "no made frame, the game's frame alone",
                 (unsigned long long)s.presents);
        }
        return fgr::held_made(held);
    }
    Library& l = lib();
    s.made = false;
    const bool infinite = !(in.far_plane < 1e8f);
    if (s.context && !fgr::context_fits(s.key, in.width, in.height, in.render_width, in.render_height, infinite))
        release(s, "another size or depth range");
    // (a new history: the caller's, a context made now, or a frame that does not follow the last one handed over)
    bool reset = in.reset || !s.context || in.sequence != s.sequence + 1;
    if (!s.context && !make(s, fgr::context_for(in.width, in.height, in.render_width, in.render_height, infinite))) return fgr::OWN;
    const fgr::Pose pose = fgr::pose_of(in.view);
    if (!reset && s.have_pose && fgr::camera_cut(s.pose, pose, in.frame_ms, fgr::kFrameGenCut)) {
        reset = true;
        if (s.n_cuts++ < 24)
            logf("vk: AMD FSR 4 frame generation: a camera cut (from %.1f %.1f %.1f to %.1f %.1f %.1f in %.1f ms): no made frame, a "
                 "new history", s.pose.pos[0], s.pose.pos[1], s.pose.pos[2], pose.pos[0], pose.pos[1], pose.pos[2], in.frame_ms);
    }
    s.pose = pose, s.have_pose = true;
    s.sequence = in.sequence, s.used_at = s.presents;
    s.frame_id++;

    if (s.out_new) {   // (the made frame's image starts UNDEFINED and stays in GENERAL)
        s.out_new = false;
        const VkImageMemoryBarrier2 b = barrier(s.out, 0, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        barriers(cmd, &b, 1);
    }
    full_barrier(cmd);   // (after what wrote the picture, the depth and the motion in this recording and before it)

    // 1. the configuration: generation on for this frame id, without a swapchain of AMD's and without its callbacks (the
    // frame is asked for below, not by a present)
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.frameGenerationEnabled = true;
    c.allowAsyncWorkloads = false;
    c.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    c.generationRect = {0, 0, (int32_t)in.width, (int32_t)in.height};
    c.frameID = s.frame_id;
    // ((Triforce, FG-2) the same picture without the HUD: what differs from it is kept where the game drew it, not moved)
    c.HUDLessColor = image_resource(in.hudless, VK_FORMAT_R8G8B8A8_UNORM, in.width, in.height, kInUsage, 0);
    ffxReturnCode_t r = l.configure(&s.context, &c.header);
    if (r != FFX_API_RETURN_OK) return fail(s, "the configuration", r), fgr::OWN;

    // 2. the preparation, from this frame's depth and motion and its camera, which FSR Frame Generation 4 needs
    ffxDispatchDescFrameGenerationPrepareV2 p{};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = s.frame_id;
    p.commandList = cmd;
    p.renderSize = {in.render_width, in.render_height};
    p.jitterOffset = {in.jitter[0], in.jitter[1]};
    p.motionVectorScale = {1.0f, 1.0f};   // (render pixels already, towards the frame before)
    p.frameTimeDelta = in.frame_ms > 0.0f && in.frame_ms < 100.0f ? in.frame_ms : 16.667f;
    p.reset = reset;
    p.cameraNear = in.near_plane;
    p.cameraFar = infinite ? 1e8f : in.far_plane;   // (read only for a finite far plane)
    p.cameraFovAngleVertical = in.fov_y;
    // (the game's world is in metres. NX2navi's note on this library: with 0.01 FSR frame generation 3.1.6 drew grey shapes
    // into the sky of its frames in a camera sweep, with 1 they were clean)
    p.viewSpaceToMetersFactor = 1.0f;
    p.depth = image_resource(in.depth, VK_FORMAT_R32_SFLOAT, in.render_width, in.render_height, kInUsage, 0);
    p.motionVectors = image_resource(in.motion, VK_FORMAT_R16G16_SFLOAT, in.render_width, in.render_height, kInUsage, 0);
    std::memcpy(p.cameraPosition, pose.pos, sizeof p.cameraPosition), std::memcpy(p.cameraUp, pose.up, sizeof p.cameraUp);
    std::memcpy(p.cameraRight, pose.right, sizeof p.cameraRight), std::memcpy(p.cameraForward, pose.fwd, sizeof p.cameraForward);
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (ffxDispatch records into cmd itself: what this thread recorded through vk_work.h reaches cmd first)
    r = l.dispatch(&s.context, &p.header);
    note_work(cmd);   // (AMD's dispatches: full_barrier must not take the next barrier for a repeat of the one before them)
    if (r != FFX_API_RETURN_OK) return fail(s, "the preparation", r), fgr::OWN;

    // 3. the generation: the one frame between the picture before and this one, into the image of ours. Given every frame,
    // a reset one too: the library keeps this picture as the next frame's start
    ffxDispatchDescFrameGeneration g{};
    g.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    g.commandList = cmd;
    g.presentColor = image_resource(in.color, VK_FORMAT_R8G8B8A8_UNORM, in.width, in.height, kColorUsage, 0);
    g.outputs[0] = image_resource(s.out, VK_FORMAT_R8G8B8A8_UNORM, in.width, in.height, kOutUsage, FFX_API_RESOURCE_USAGE_RENDERTARGET);
    g.numGeneratedFrames = 1;
    g.reset = reset;
    g.backbufferTransferFunction = in.pq ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    g.minMaxLuminance[0] = 0.005f, g.minMaxLuminance[1] = in.pq ? in.peak_nits : 100.0f;   // (nits: read for an HDR picture)
    g.generationRect = c.generationRect;
    g.frameID = s.frame_id;
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);
    r = l.dispatch(&s.context, &g.header);
    note_work(cmd);
    if (r != FFX_API_RETURN_OK) return fail(s, "the generation", r), fgr::OWN;
    full_barrier(cmd);   // (before the made frame is copied)

    if (reset) {
        s.n_skipped++;
        return fgr::SKIP;
    }
    s.made = true;
    if (s.n_made++ == 0)
        logf("vk: AMD FSR 4 frame generation: the first made frame, %ux%u from depth and motion of %ux%u (near %.4f, far %.6g, fov "
             "%.2f deg, jitter %+.3f %+.3f px)", in.width, in.height, in.render_width, in.render_height, in.near_plane,
             infinite ? INFINITY : in.far_plane, in.fov_y * 57.2957795f, in.jitter[0], in.jitter[1]);
    return fgr::MADE;
}

VkImage made_image() {
    Fg& s = fg();
    std::lock_guard lk(s.mu);
    return s.made ? s.out : VK_NULL_HANDLE;
}

void present_tick() {
    if (!g_fg_live.load(std::memory_order_relaxed)) return;   // (no context: one atomic load a present)
    Fg& s = fg();
    std::lock_guard lk(s.mu);
    if (g_fg_exiting.load()) return;
    s.presents++;
    if (fgr::release_due(s.context != nullptr || s.out != VK_NULL_HANDLE, g_fg_failed.load(), s.presents, s.used_at))
        release(s, g_fg_failed.load() ? "after its failure" : "the Frame generation row left AMD FSR 4");
}

void exiting() {
    AtExit e{fg()};
    fgr::teardown(e, true);
}

}  // namespace fsr4fg
#else   // (not Windows: AMD publishes no frame generation library elsewhere)
namespace fsr4fg {
framegen_rows::Why why() { return framegen_rows::FG_NOT_BUILT; }
framegen_rows::Why why_known() { return framegen_rows::FG_NOT_BUILT; }
bool frames_wanted() { return false; }
const char* version() { return ""; }
framegen_rows::Made generate(VkCommandBuffer, const Inputs&) { return framegen_rows::OWN; }
VkImage made_image() { return VK_NULL_HANDLE; }
void present_tick() {}
void exiting() {}
}  // namespace fsr4fg
#endif

#else  // (no HOOKSHOT_FSR in this build: FSR is not there)

bool fsr4_built() { return false; }
const TemporalEngine* fsr4_engine() { return nullptr; }
void fsr4_probe() {}
bool fsr4_wants_descriptor_buffer() { return false; }
namespace fsr4fg {
framegen_rows::Why why() { return framegen_rows::FG_NOT_BUILT; }
framegen_rows::Why why_known() { return framegen_rows::FG_NOT_BUILT; }
bool frames_wanted() { return false; }
const char* version() { return ""; }
framegen_rows::Made generate(VkCommandBuffer, const Inputs&) { return framegen_rows::OWN; }
VkImage made_image() { return VK_NULL_HANDLE; }
void present_tick() {}
void exiting() {}
}  // namespace fsr4fg

#endif

// ((Triforce) in every build: the menu's Frame generation line reads it, and frames_wanted above)
extern std::atomic<bool> present_native_pq;   // (vk_replay.cpp: the game's picture is HDR10 codes, HDR Native)
framegen_rows::Held fsr4fg::held() {
    if (present_native_pq.load(std::memory_order_relaxed)) return framegen_rows::HELD_HDR_NATIVE;
    // (the run's scene is RGBA16F: decided at the start and kept for the run, whatever the HDR row says now)
    if (temporal_scene_format(VK_FORMAT_R16G16B16A16_SFLOAT)) return framegen_rows::HELD_HDR_SCENE;
    return framegen_rows::HELD_NONE;
}

}  // namespace mkwvk
