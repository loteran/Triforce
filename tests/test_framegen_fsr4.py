"""(Triforce) The Frame generation row's AMD FSR 4 and AMD's frame generation library behind it. No game, no GPU, none
of AMD's files: AMD's library is a stand-in that notes every call it gets.

1. gpu/vk/framegen_rows.h, compiled and run: the row's last value by what a build and a card support, the values from before
   (0 Off, 1 Auto, 2 to 8) meaning what they did, the version taken from AMD's list (the newest FSR 4, never an FSR 3.1
   under its name), the camera cut, when a context still fits and when it is let go, and the order it is taken down in.
2. gpu/vk/vk_menu.cpp's own tx_framegen and hn_framegen_fsr4, cut out and run: the names an options file's values show,
   a saved AMD FSR 4 where it cannot run, and that the row's table, its end and the menu's opening are wired to them.
3. gpu/vk/vk_fsr4.cpp's frame generation section (fsr4fg), cut out and run against the stand-in library (Windows, with
   the Vulkan headers): where it is offered (AMD's file beside the loader, an FSR 4 frame generation in AMD's list, the
   switches, not beside NVIDIA's), the context's descriptions (the API's version, the device, the version asked for by
   its id), each frame's calls in AMD's order for a frame id that goes up by one, the first frame, a gap and a camera cut
   as a new history without a made frame, another size, a failure, the row left for a while, and the exit: what is
   destroyed, in which order, and that nothing of AMD's is called after the exit or a failure.
4. The source: vk_framegen.cpp hands the frame over and falls back, the inputs are gathered for it, the present's tick and
   the exit reach it, the build has the stubs, the rows are documented.

python -I -B tests/test_framegen_fsr4.py [--cxx g++] [--headers path/to/Vulkan-Headers/include]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def function(source, start):
    """A definition from its first line to its closing brace at the start of a line"""
    at = source.find(start)
    if at < 0:
        raise SystemExit("%s is missing" % start.rstrip(" {("))
    body = source[at:]
    return body[:body.index("\n}\n") + 3]


def between(source, start, end):
    a = source.find(start)
    b = source.find(end, a + 1)
    if a < 0 or b < 0:
        raise SystemExit("not found: %r ... %r" % (start[:50], end[:50]))
    return source[a:b]


def build_and_run(cxx, tmp, name, text, flags=(), runs=((),), env=None):
    cpp, exe = Path(tmp) / (name + ".cpp"), Path(tmp) / (name + (".exe" if os.name == "nt" else ""))
    cpp.write_text(text, encoding="utf-8")
    subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", *flags, str(cpp),
                    "-o", str(exe)], check=True)
    for args in runs:
        subprocess.run([str(exe), *args], check=True, timeout=60, env=env)


CHECK = r'''
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
'''

ROWS = CHECK + r'''
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "framegen_rows.h"
using namespace mkwvk::framegen_rows;

struct Ops {
    std::vector<std::string> did;
    void stop() { did.push_back("stop"); }
    void wait_gpu() { did.push_back("wait_gpu"); }
    void destroy_context() { did.push_back("destroy_context"); }
    void destroy_images() { did.push_back("destroy_images"); }
};

int main() {
    // the row: AMD FSR 4 is its last value where it is offered, 8x where it is not (the row as it was)
    CHECK(FSR4 == 9 && kRowMax == 9 && OFF == 0 && AUTO == 1 && kMaxMult == 8);
    CHECK(row_hi(true) == 9 && row_hi(false) == 8);
    // an options file from before: 0 to 8 show and run as they did, whatever the card
    for (int v = 0; v <= 8; v++)
        for (int ok = 0; ok < 2; ok++) {
            CHECK(shown(v, ok != 0) == v);
            for (int cap = 2; cap <= 4; cap++) CHECK(multiplier(v, cap) == (v == 1 ? cap : std::clamp(v, 1, 8)));   // (vk_framegen.cpp's rule from before)
            for (int w = 0; w < FG_WHY_COUNT; w++) CHECK(!fsr4_asked(v, (Why)w, false));
        }
    // the new value: itself where AMD's library can make the frames, 2x by the renderer's own where it cannot
    CHECK(shown(9, true) == 9 && shown(9, false) == 2);
    CHECK(multiplier(9, 2) == 2 && multiplier(9, 4) == 2);
    CHECK(fsr4_asked(9, FG_OK, false));
    CHECK(!fsr4_asked(9, FG_OK, true));   // (not beside NVIDIA DLSS frame generation)
    for (int w = 1; w < FG_WHY_COUNT; w++) CHECK(!fsr4_asked(9, (Why)w, false));
    CHECK(multiplier(-3, 2) == 1 && multiplier(12, 2) == 8);   // (a value outside the row)

    // AMD's list: the newest FSR 4, the first of equal ones; never an FSR 3.1 under FSR 4's name
    unsigned v[3];
    CHECK(parse_version("4.0.2", v) && v[0] == 4 && v[1] == 0 && v[2] == 2);
    CHECK(parse_version("4.1.0*", v) && v[1] == 1);   // (a version the graphics driver brings)
    CHECK(!parse_version("FSR", v) && !parse_version(nullptr, v) && !parse_version("4.0", v));
    { const char* n[] = {"4.0.2", "3.1.6"}; CHECK(pick_version(n, 2, false) == 0); }
    { const char* n[] = {"3.1.6", "4.0.0", "4.0.2*", "4.0.2", "4.0.1"}; CHECK(pick_version(n, 5, false) == 2); }
    { const char* n[] = {"3.1.6", "3.1.4"}; CHECK(pick_version(n, 2, false) == -1); CHECK(pick_version(n, 2, true) == 0); }
    { const char* n[] = {"3.1.6", "4.0.2"}; CHECK(pick_version(n, 2, true) == 1); }   // (any: still the newest)
    { const char* n[] = {nullptr, "?", "5.0.0"}; CHECK(pick_version(n, 3, false) == -1); CHECK(pick_version(n, 3, true) == 2); }
    CHECK(pick_version(nullptr, 0, false) == -1);

    // the camera: the view matrix's rows are its axes, the position -R^T t
    const float still[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    Pose a = pose_of(still);
    CHECK(a.pos[0] == 0 && a.pos[1] == 0 && a.pos[2] == 0 && a.fwd[2] == -1 && a.up[1] == 1 && a.right[0] == 1);
    const float moved[12] = {1, 0, 0, -3, 0, 1, 0, -4, 0, 0, 1, 12};   // (the camera at 3, 4, -12)
    Pose b = pose_of(moved);
    CHECK(b.pos[0] == 3 && b.pos[1] == 4 && b.pos[2] == -12);
    const float turned[12] = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0};   // (a quarter turn about the up axis)
    CHECK(std::fabs(pose_of(turned).fwd[0] + 1.0f) < 1e-6f);
    // a cut: over 5 units, or over 10 degrees with over half a unit, for each 60th of a second
    auto at = [](float x, float deg) {
        Pose p;
        p.pos[0] = x;
        p.fwd[0] = std::sin(deg * 0.01745329252f), p.fwd[2] = -std::cos(deg * 0.01745329252f);
        return p;
    };
    CHECK(!camera_cut(at(0, 0), at(0.05f, 0.08f), 16.667f));   // (play: NX2navi's medians)
    CHECK(!camera_cut(at(0, 0), at(4.9f, 0), 16.667f));
    CHECK(camera_cut(at(0, 0), at(5.1f, 0), 16.667f));          // (an area load)
    CHECK(!camera_cut(at(0, 0), at(0.4f, 45), 16.667f));        // (a fast turn in place is no cut)
    CHECK(camera_cut(at(0, 0), at(0.6f, 12), 16.667f));
    CHECK(!camera_cut(at(0, 0), at(0.6f, 8), 16.667f));
    CHECK(!camera_cut(at(0, 0), at(9.0f, 0), 33.333f));         // (a 30 fps frame may move twice as far)
    CHECK(camera_cut(at(0, 0), at(11.0f, 0), 33.333f));
    CHECK(camera_cut(at(0, 0), at(21.0f, 0), 500.0f) && !camera_cut(at(0, 0), at(19.0f, 0), 500.0f));   // (four times at the most)
    CHECK(camera_cut(at(0, 0), at(5.1f, 0), 4.0f) && camera_cut(at(0, 0), at(5.1f, 0), 0.0f));          // (never less than one 60th's)
    Pose none;   // (no camera: nothing to compare, a new history)
    CHECK(camera_cut(none, at(0, 0), 16.667f) && camera_cut(at(0, 0), at(NAN, 0), 16.667f));

    // a context: for one picture size and one kind of depth, any scene up to the picture's size
    const ContextKey k = context_for(2560, 1440, 1920, 1080, true);
    CHECK(k.width == 2560 && k.height == 1440 && k.max_render_width == 2560 && k.max_render_height == 1440 && k.infinite_depth);
    CHECK(context_fits(k, 2560, 1440, 1920, 1080, true) && context_fits(k, 2560, 1440, 2560, 1440, true));
    CHECK(context_fits(k, 2560, 1440, 1280, 720, true));   // (the game's dynamic resolution)
    CHECK(!context_fits(k, 1920, 1080, 1920, 1080, true) && !context_fits(k, 2560, 1440, 1920, 1080, false));
    CHECK(!context_fits(k, 2560, 1440, 2561, 1440, true));
    const ContextKey big = context_for(1280, 720, 1920, 1080, false);   // (a scene larger than the picture still gets one)
    CHECK(big.max_render_width == 1920 && big.max_render_height == 1080 && context_fits(big, 1280, 720, 1920, 1080, false));

    // let go: 120 presents after the row left it, 2 after a failure, never at the present of its last frame
    CHECK(!release_due(false, false, 500, 0) && !release_due(false, true, 500, 0));
    CHECK(!release_due(true, false, 100, 100) && !release_due(true, false, 219, 100) && release_due(true, false, 220, 100));
    CHECK(!release_due(true, true, 100, 100) && !release_due(true, true, 101, 100) && release_due(true, true, 102, 100));
    CHECK(!release_due(true, true, 99, 100));

    // taken down in play: no new call, the GPU past its work, AMD's context, then the image it wrote; at the exit only the first
    Ops play, exit;
    teardown(play, false);
    CHECK((play.did == std::vector<std::string>{"stop", "wait_gpu", "destroy_context", "destroy_images"}));
    teardown(exit, true);
    CHECK((exit.did == std::vector<std::string>{"stop"}));
    std::puts("PASS: the Frame generation row's AMD FSR 4, AMD's version list, camera cuts, context life and teardown order");
}
'''

MENU = CHECK + r'''
#include <algorithm>
#include <cstring>
#include "framegen_rows.h"
namespace framegen_rows = mkwvk::framegen_rows;
namespace fsr4fg {
framegen_rows::Why now = framegen_rows::FG_OK;
framegen_rows::Why why() { return now; }
bool offered() { return why() == framegen_rows::FG_OK; }
framegen_rows::Why why_known() { return now; }   // ((Triforce) the menu's rows read the answer as known)
bool offered_known() { return why_known() == framegen_rows::FG_OK; }
}
/* TX */
/* HN */
int main() {
    static const char* const before[] = {"Off", "Auto", "2x", "3x", "4x", "5x", "6x", "7x", "8x"};
    for (int w = 0; w < framegen_rows::FG_WHY_COUNT; w++) {
        fsr4fg::now = (framegen_rows::Why)w;
        for (int v = 0; v <= 8; v++) CHECK(!std::strcmp(tx_framegen(v), before[v]));   // (an options file from before)
        CHECK(!std::strcmp(tx_framegen(9), w == framegen_rows::FG_OK ? "AMD FSR 4" : "2x"));   // (a saved choice shows what runs)
        const char* line = hn_framegen_fsr4(-1);
        CHECK(line && *line);
        CHECK((std::strstr(line, "AMD's FSR 4 library") != nullptr) == (w == framegen_rows::FG_OK));
        if (w != framegen_rows::FG_OK && w != framegen_rows::FG_NOT_YET) CHECK(std::strstr(line, "the renderer's own makes"));
        bool listed = false;   // (the menu measures every line a value can show: the one shown now is among them)
        int k = 0;
        for (; const char* t = hn_framegen_fsr4(k); k++) listed |= t == line;
        CHECK(listed && k == framegen_rows::FG_WHY_COUNT);
    }
    CHECK(std::strstr(hn_framegen_fsr4(0), "HUD"));   // (said to the player: the HUD is in the picture it works from)
    std::puts("PASS: the row's names for an options file's values, and what a saved AMD FSR 4 shows where it cannot run");
}
'''

FG = CHECK + r'''
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "api/include/ffx_api.h"
#include "api/include/ffx_api_types.h"
#include "api/include/vk/ffx_api_vk.h"
#include "framegeneration/include/ffx_framegeneration.h"
#include "framegen_rows.h"

typedef struct VmaAllocation_T* VmaAllocation;

// ---- what the section takes from the renderer and from the rest of vk_fsr4.cpp: stand-ins that note what they are asked ----
static std::vector<std::string> events;
static std::vector<std::string> logged;
static void ev(const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    events.push_back(b);
}
static bool has_log(const char* part) {
    for (const std::string& l : logged)
        if (l.find(part) != std::string::npos) return true;
    return false;
}
namespace mkwvk {
void logf(const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    logged.push_back(b);
}
static std::map<std::string, int> env;
int env_switch(const char* name) {
    const auto it = env.find(name);
    return it == env.end() ? -1 : it->second;
}
struct Device {
    VkDevice device = (VkDevice)(uintptr_t)0x11;
    bool offscreen = false;
    std::mutex present_mu;
} dev;
static int idle_waits;
void device_wait_idle_locked() { idle_waits++, events.push_back("wait_gpu"); }
static bool image_fails;
static int images_live;
static VkImage last_image;
static VkImageUsageFlags last_image_usage;
bool create_image(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool = false, bool = false, bool = false) {
    if (image_fails) return false;
    static uintptr_t next = 0x1000;
    image = last_image = (VkImage)(next += 0x10), allocation = (VmaAllocation)(next + 1);
    last_image_usage = ci.usage;
    CHECK(ci.format == VK_FORMAT_R8G8B8A8_UNORM);
    images_live++, ev("create_image %ux%u", ci.extent.width, ci.extent.height);
    return true;
}
void destroy_image(VkImage image, VmaAllocation) {
    CHECK(image != VK_NULL_HANDLE);
    images_live--, events.push_back("destroy_image");
}
void full_barrier(VkCommandBuffer, const char* = "") {}
void note_work(VkCommandBuffer) {}   // ((Triforce, P4) cmd_work.h's: counted in the command buffer)
namespace rec {
enum { B_DIRECT };
void boundary(int) {}
}
struct {
    int framegen = 9;
} opts;
namespace dlssg {
static bool on;
bool active() { return on; }
}
VkImageMemoryBarrier2 barrier(VkImage image, VkAccessFlags2, VkAccessFlags2, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.image = image, b.oldLayout = from, b.newLayout = to;
    return b;
}
void barriers(VkCommandBuffer, const VkImageMemoryBarrier2* b, uint32_t n) {
    CHECK(n == 1 && b->image == last_image && b->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && b->newLayout == VK_IMAGE_LAYOUT_GENERAL);
    events.push_back("image to GENERAL");
}
constexpr const char* kLoader = "amd_fidelityfx_loader_vk.dll";
struct Library {
    std::mutex mu;
    std::string dir;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    VkDeviceContext device_context{};
};
Library& lib() {
    static Library l;
    return l;
}
static bool library_there = true;
static int library_loads;
bool load_library(Library&) { return library_loads++, library_there; }

namespace fsr4fg {
/* INPUTS */
framegen_rows::Why why();
inline bool offered() { return why() == framegen_rows::FG_OK; }   // (as vk_fsr4.h has it)
framegen_rows::Why why_known();
inline bool offered_known() { return why_known() == framegen_rows::FG_OK; }
}
/* SECTION */
} // namespace mkwvk

using namespace mkwvk;
namespace fgr = mkwvk::framegen_rows;

// ---- AMD's library: a stand-in that checks what it is given and notes the calls ----
static std::vector<const char*> listed = {"4.0.2", "3.1.6"};
static const uint64_t kIds[] = {0x402, 0x316, 0x999};
static int amd_calls, contexts_live, contexts_made;
static bool create_fails;
static int fail_dispatch;   // (0 none, 1 the preparation, 2 the generation)
static bool fail_configure;
static uint64_t chosen_id;
static VkCommandBuffer const kCmd = (VkCommandBuffer)(uintptr_t)0x77;
static VkImage const kColor = (VkImage)(uintptr_t)0xC0, kDepth = (VkImage)(uintptr_t)0xD0, kMotion = (VkImage)(uintptr_t)0xE0;
static uint64_t last_id;
static uint32_t made_w, made_h, made_rw, made_rh, made_flags;

static ffxReturnCode_t amd_query(ffxContext* context, ffxQueryDescHeader* d) {
    amd_calls++;
    if (d->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS) {
        auto* q = (ffxQueryDescGetVersions*)d;
        CHECK(!context && q->createDescType == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);   // (frame generation's list, not the upscaler's)
        CHECK(q->device == &lib().device_context && q->outputCount);
        if (!q->versionIds && !q->versionNames) return *q->outputCount = listed.size(), FFX_API_RETURN_OK;
        CHECK(*q->outputCount >= listed.size());
        for (size_t i = 0; i < listed.size(); i++) q->versionIds[i] = kIds[i], q->versionNames[i] = listed[i];
        return *q->outputCount = listed.size(), FFX_API_RETURN_OK;
    }
    CHECK(context && *context);
    if (d->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE)
        return ((ffxQueryDescFrameGenerationGetGPUMemoryUsage*)d)->gpuMemoryUsageFrameGeneration->totalUsageInBytes = 300ull << 20, FFX_API_RETURN_OK;
    if (d->type == FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION) return ((ffxQueryGetProviderVersion*)d)->versionName = "4.0.2", FFX_API_RETURN_OK;
    return 1;
}
static ffxReturnCode_t amd_create(ffxContext* context, ffxCreateContextDescHeader* d, const ffxAllocationCallbacks*) {
    amd_calls++;
    CHECK(d->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION);
    const auto* c = (const ffxCreateContextDescFrameGeneration*)d;
    bool api = false, backend = false, pin = false;
    for (const ffxApiHeader* h = d->pNext; h; h = h->pNext) {
        if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION)
            api = ((const ffxCreateContextDescFrameGenerationVersion*)h)->version == FFX_FRAMEGENERATION_VERSION;
        if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK)
            backend = ((const ffxCreateBackendVKDesc*)h)->deviceContext.vkDevice == lib().device_context.vkDevice;
        if (h->type == FFX_API_DESC_TYPE_OVERRIDE_VERSION) pin = ((const ffxOverrideVersion*)h)->versionId == chosen_id;
    }
    CHECK(api && backend && pin);   // (the API's version, the device, and the version asked for by its id)
    CHECK(c->backBufferFormat == ffxApiGetSurfaceFormatVK(VK_FORMAT_R8G8B8A8_UNORM));
    CHECK(c->flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED);
    CHECK(!(c->flags & (FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT | FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)));
    if (create_fails) return ev("create fails"), 2;
    made_w = c->displaySize.width, made_h = c->displaySize.height, made_rw = c->maxRenderSize.width, made_rh = c->maxRenderSize.height;
    made_flags = c->flags;
    *context = (ffxContext)(uintptr_t)(0x5000 + ++contexts_made);
    contexts_live++, ev("create_context %ux%u", made_w, made_h);
    return FFX_API_RETURN_OK;
}
static ffxReturnCode_t amd_destroy(ffxContext* context, const ffxAllocationCallbacks*) {
    amd_calls++;
    CHECK(context && *context && contexts_live == 1);
    CHECK(images_live == 1);   // (AMD's context goes before the image it wrote into)
    contexts_live--, events.push_back("destroy_context");
    return FFX_API_RETURN_OK;
}
static ffxReturnCode_t amd_configure(ffxContext* context, const ffxConfigureDescHeader* d) {
    amd_calls++;
    CHECK(context && *context && d->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION);
    const auto* c = (const ffxConfigureDescFrameGeneration*)d;
    // (no swapchain of AMD's and none of its callbacks: the frame is asked for, and presented by the renderer)
    CHECK((c->flags & FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY) && !c->swapChain && !c->presentCallback && !c->frameGenerationCallback);
    CHECK(c->frameGenerationEnabled && !c->allowAsyncWorkloads && !c->onlyPresentGenerated && !c->HUDLessColor.resource);
    CHECK(c->generationRect.left == 0 && c->generationRect.top == 0 && (uint32_t)c->generationRect.width == made_w);
    CHECK(c->frameID == last_id + 1);   // (one more for each frame handed over)
    last_id = c->frameID;
    ev("configure %llu", (unsigned long long)c->frameID);
    return fail_configure ? 3 : FFX_API_RETURN_OK;
}
static ffxReturnCode_t amd_dispatch(ffxContext* context, const ffxDispatchDescHeader* d) {
    amd_calls++;
    CHECK(context && *context);
    if (d->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2) {
        const auto* p = (const ffxDispatchDescFrameGenerationPrepareV2*)d;
        CHECK(p->frameID == last_id && p->commandList == kCmd);
        CHECK(p->depth.resource == kDepth && p->motionVectors.resource == kMotion);
        CHECK(p->depth.state == FFX_API_RESOURCE_STATE_UNORDERED_ACCESS && p->depth.description.width == p->renderSize.width);
        CHECK(p->renderSize.width <= made_rw && p->renderSize.height <= made_rh);
        CHECK(p->motionVectorScale.x == 1.0f && p->motionVectorScale.y == 1.0f && p->viewSpaceToMetersFactor == 1.0f);
        CHECK(p->cameraNear == 0.1f && p->cameraFovAngleVertical == 1.0f && p->frameTimeDelta > 0.0f);
        CHECK(p->cameraForward[2] == -1.0f && p->cameraUp[1] == 1.0f && p->cameraRight[0] == 1.0f);
        ev("prepare %llu%s at %g", (unsigned long long)p->frameID, p->reset ? " reset" : "", p->cameraPosition[0] + 0.0f);   // (+ 0: no "-0")
        return fail_dispatch == 1 ? 4 : FFX_API_RETURN_OK;
    }
    CHECK(d->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION);
    const auto* g = (const ffxDispatchDescFrameGeneration*)d;
    CHECK(g->frameID == last_id && g->commandList == kCmd && g->numGeneratedFrames == 1);
    CHECK(g->presentColor.resource == kColor && g->outputs[0].resource == last_image && !g->outputs[1].resource);
    CHECK(g->outputs[0].description.width == made_w && (g->outputs[0].description.usage & FFX_API_RESOURCE_USAGE_RENDERTARGET));
    CHECK((g->outputs[0].description.usage & FFX_API_RESOURCE_USAGE_UAV) && g->outputs[0].state == FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(g->backbufferTransferFunction == FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB);
    CHECK((uint32_t)g->generationRect.width == made_w && (uint32_t)g->generationRect.height == made_h);
    ev("generate %llu%s", (unsigned long long)g->frameID, g->reset ? " reset" : "");
    return fail_dispatch == 2 ? 5 : FFX_API_RETURN_OK;
}

static fsr4fg::Inputs frame(uint64_t sequence, float x = 0.0f, uint32_t w = 2560, uint32_t h = 1440, uint32_t rw = 1920, uint32_t rh = 1080) {
    fsr4fg::Inputs in;
    in.color = kColor, in.width = w, in.height = h;
    in.depth = kDepth, in.motion = kMotion, in.render_width = rw, in.render_height = rh;
    in.near_plane = 0.1f, in.far_plane = FLT_MAX, in.fov_y = 1.0f;
    const float view[12] = {1, 0, 0, -x, 0, 1, 0, 0, 0, 0, 1, 0};   // (the camera at x, looking down -z)
    std::memcpy(in.view, view, sizeof view);
    in.sequence = sequence;
    return in;
}
static std::vector<std::string> take() {
    std::vector<std::string> e;
    e.swap(events);
    return e;
}
static bool is(const std::vector<std::string>& got, std::vector<std::string> want) {
    if (got == want) return true;
    for (const std::string& e : got) std::fprintf(stderr, "  got: %s\n", e.c_str());
    return false;
}

int main(int argc, char** argv) {
    const std::string scene = argc > 1 ? argv[1] : "play";
    const std::string folder = argc > 2 ? argv[2] : ".";
    Library& l = lib();
    l.dir = folder;
    l.create = amd_create, l.destroy = amd_destroy, l.configure = amd_configure, l.query = amd_query, l.dispatch = amd_dispatch;
    l.device_context = VkDeviceContext{dev.device, (VkPhysicalDevice)(uintptr_t)0x22, nullptr};
    chosen_id = kIds[0];

    // ---- where it is offered ----
    if (scene == "no-device") {   // (asked before the device is made: no answer to keep, AMD's library not touched)
        dev.device = VK_NULL_HANDLE;
        CHECK(fsr4fg::why() == fgr::FG_NOT_YET && !fsr4fg::frames_wanted() && !library_loads && !amd_calls);
        dev.device = (VkDevice)(uintptr_t)0x11;
        // ((Triforce) as known, before it is asked: not yet, and AMD's library not touched; the menu's rows read this)
        CHECK(fsr4fg::why_known() == fgr::FG_NOT_YET && !fsr4fg::offered_known() && !library_loads && !amd_calls);
        CHECK(fsr4fg::why() == fgr::FG_OK && fsr4fg::frames_wanted());
        CHECK(fsr4fg::why_known() == fgr::FG_OK && fsr4fg::offered_known());   // (and the answer once asked)
        CHECK(!std::strcmp(fsr4fg::version(), "4.0.2"));
        const int calls = amd_calls;
        CHECK(fsr4fg::why() == fgr::FG_OK && amd_calls == calls && library_loads == 1);   // (asked once)
        return std::puts("PASS no-device"), 0;
    }
    if (scene == "no-library" || scene == "no-configure") {
        if (scene == "no-library") library_there = false;
        else l.configure = nullptr;   // (a loader without ffxConfigure upscales, and has no frame generation)
        CHECK(fsr4fg::why() == fgr::FG_NO_LIBRARY && !fsr4fg::offered() && !fsr4fg::frames_wanted() && !amd_calls);
        CHECK(fsr4fg::generate(kCmd, frame(1)) == fgr::OWN && !amd_calls && events.empty());
        return std::puts("PASS no-library"), 0;
    }
    if (scene == "no-file") {   // (AMD's frame generation library is not beside the loader: nothing asked of the loader)
        CHECK(fsr4fg::why() == fgr::FG_NO_FILE && !amd_calls && has_log("amd_fidelityfx_framegeneration_vk.dll"));
        CHECK(fsr4fg::generate(kCmd, frame(1)) == fgr::OWN && events.empty());
        return std::puts("PASS no-file"), 0;
    }
    if (scene == "no-model" || scene == "any-version" || scene == "none-listed") {
        listed = {"3.1.6"};   // (a card without FSR 4's frame generation: AMD's library lists FSR 3.1's alone)
        if (scene == "none-listed") listed.clear();
        if (scene == "any-version") env["RX_VK_FSR4_FG"] = 1, chosen_id = kIds[0];
        if (scene == "any-version") {
            CHECK(fsr4fg::why() == fgr::FG_OK && !std::strcmp(fsr4fg::version(), "3.1.6"));
            CHECK(fsr4fg::generate(kCmd, frame(1)) == fgr::SKIP && fsr4fg::generate(kCmd, frame(2)) == fgr::MADE);
        } else {
            CHECK(fsr4fg::why() == fgr::FG_NO_MODEL && !fsr4fg::offered());
            CHECK(fsr4fg::generate(kCmd, frame(1)) == fgr::OWN && events.empty() && !contexts_made);
            CHECK(scene == "none-listed" || has_log("3.1.6"));   // (the log names what AMD's library has)
        }
        return std::puts("PASS versions"), 0;
    }
    if (scene == "env-off" || scene == "fsr4-off") {
        env[scene == "env-off" ? "RX_VK_FSR4_FG" : "RX_VK_FSR4"] = 0;
        CHECK(fsr4fg::why() == fgr::FG_ENV_OFF && !fsr4fg::frames_wanted() && !library_loads && !amd_calls);
        return std::puts("PASS env"), 0;
    }
    if (scene == "row") {   // (asked only with the row at AMD FSR 4, and not beside NVIDIA DLSS frame generation)
        for (int v = 0; v <= 8; v++) {
            opts.framegen = v;
            CHECK(!fsr4fg::frames_wanted() && fsr4fg::generate(kCmd, frame(1)) == fgr::OWN);
        }
        CHECK(!library_loads && !amd_calls);   // (AMD's library is not even looked for while the row is elsewhere)
        opts.framegen = 9;
        dlssg::on = true;
        CHECK(!fsr4fg::frames_wanted() && fsr4fg::generate(kCmd, frame(1)) == fgr::OWN && !contexts_made);
        dlssg::on = false;
        CHECK(fsr4fg::frames_wanted());
        return std::puts("PASS row"), 0;
    }

    CHECK(fsr4fg::why() == fgr::FG_OK && fsr4fg::offered() && fsr4fg::frames_wanted());
    take();
    if (scene == "create-fails" || scene == "image-fails") {   // (nothing published, nothing left, off for the run)
        (scene == "create-fails" ? create_fails : image_fails) = true;
        CHECK(fsr4fg::generate(kCmd, frame(1)) == fgr::OWN);
        CHECK(is(take(), scene == "create-fails" ? std::vector<std::string>{"create_image 2560x1440", "create fails", "destroy_image"}
                                                 : std::vector<std::string>{}));
        CHECK(!images_live && !contexts_live && fsr4fg::made_image() == VK_NULL_HANDLE);
        CHECK(fsr4fg::why() == fgr::FG_FAILED && !fsr4fg::offered() && !fsr4fg::frames_wanted());
        const int calls = amd_calls;
        create_fails = image_fails = false;
        CHECK(fsr4fg::generate(kCmd, frame(2)) == fgr::OWN && amd_calls == calls);   // (not tried again in this run)
        for (int i = 0; i < 300; i++) fsr4fg::present_tick();
        CHECK(take().empty() && amd_calls == calls);
        return std::puts("PASS create-fails"), 0;
    }
    if (scene == "dispatch-fails" || scene == "configure-fails") {
        CHECK(fsr4fg::generate(kCmd, frame(1)) == fgr::SKIP && fsr4fg::generate(kCmd, frame(2)) == fgr::MADE);
        fsr4fg::present_tick();
        take();
        if (scene == "configure-fails") fail_configure = true;
        else fail_dispatch = 2;
        CHECK(fsr4fg::generate(kCmd, frame(3)) == fgr::OWN);   // (the renderer's own makes this frame)
        CHECK(fsr4fg::made_image() == VK_NULL_HANDLE && fsr4fg::why() == fgr::FG_FAILED && has_log("off for this run"));
        take();
        const int calls = amd_calls;
        // (the context stays while the failed frame's commands may be recorded and not on the queue: this present's tick
        // leaves it, the next takes it down, the GPU idle first, AMD's context before the image)
        fsr4fg::present_tick();
        CHECK(take().empty() && contexts_live == 1);
        CHECK(fsr4fg::generate(kCmd, frame(4)) == fgr::OWN && amd_calls == calls);
        fsr4fg::present_tick();
        CHECK(is(take(), {"wait_gpu", "destroy_context", "destroy_image"}) && !contexts_live && !images_live);
        for (int i = 0; i < 300; i++) fsr4fg::present_tick();
        CHECK(fsr4fg::generate(kCmd, frame(5)) == fgr::OWN && take().empty() && amd_calls == calls + 1);   // (+1: the destroy)
        return std::puts("PASS dispatch-fails"), 0;
    }
    if (scene == "exit") {
        CHECK(fsr4fg::generate(kCmd, frame(1)) == fgr::SKIP && fsr4fg::generate(kCmd, frame(2)) == fgr::MADE);
        take();
        const int calls = amd_calls;
        fsr4fg::exiting();
        // (nothing of AMD's is called at the exit or after it: no destroy under ExitProcess, no frame, no tick's release)
        CHECK(fsr4fg::generate(kCmd, frame(3)) == fgr::OWN && fsr4fg::made_image() == VK_NULL_HANDLE);
        for (int i = 0; i < 300; i++) fsr4fg::present_tick();
        CHECK(take().empty() && amd_calls == calls && contexts_live == 1 && images_live == 1 && !idle_waits);
        fsr4fg::exiting();   // (twice is once)
        CHECK(amd_calls == calls);
        return std::puts("PASS exit"), 0;
    }

    // ---- play: each frame's calls, the histories, another size, the row left ----
    CHECK(scene == "play");
    // the first frame: the image and the context made (the image first, published together), then AMD's order for frame 1,
    // as a new history, and no made frame
    CHECK(fsr4fg::generate(kCmd, frame(1)) == fgr::SKIP && fsr4fg::made_image() == VK_NULL_HANDLE);
    CHECK(is(take(), {"create_image 2560x1440", "create_context 2560x1440", "image to GENERAL", "configure 1", "prepare 1 reset at 0",
                      "generate 1 reset"}));
    CHECK(made_rw == 2560 && made_rh == 1440 && (made_flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE));
    CHECK((last_image_usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) ==
          (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    CHECK(has_log("FSR frame generation 4.0.2 made"));
    fsr4fg::present_tick();
    // the frames after it: configuration, preparation, generation, the id one more each; the made frame is the image of ours
    CHECK(fsr4fg::generate(kCmd, frame(2, 0.05f)) == fgr::MADE && fsr4fg::made_image() == last_image);
    CHECK(is(take(), {"configure 2", "prepare 2 at 0.05", "generate 2"}));
    fsr4fg::present_tick();
    CHECK(fsr4fg::generate(kCmd, frame(3, 0.1f, 2560, 1440, 1280, 720)) == fgr::MADE);   // (the game's dynamic resolution: the same context)
    CHECK(is(take(), {"configure 3", "prepare 3 at 0.1", "generate 3"}));
    // a camera cut: handed over as a new history, and no frame made between the two views
    CHECK(fsr4fg::generate(kCmd, frame(4, 40.0f)) == fgr::SKIP && fsr4fg::made_image() == VK_NULL_HANDLE);
    CHECK(is(take(), {"configure 4", "prepare 4 reset at 40", "generate 4 reset"}) && has_log("a camera cut"));
    CHECK(fsr4fg::generate(kCmd, frame(5, 40.05f)) == fgr::MADE);   // (and on from there)
    take();
    // the caller's reset (its first frame, a hitch) and a present it did not hand over (a menu: no scene): a new history each
    fsr4fg::Inputs hitch = frame(6, 40.1f);
    hitch.reset = true;
    CHECK(fsr4fg::generate(kCmd, hitch) == fgr::SKIP);
    CHECK(is(take(), {"configure 6", "prepare 6 reset at 40.1", "generate 6 reset"}));
    CHECK(fsr4fg::generate(kCmd, frame(7, 40.1f)) == fgr::MADE);
    CHECK(fsr4fg::generate(kCmd, frame(9, 40.1f)) == fgr::SKIP);   // (8 was not handed over)
    CHECK(is(take(), {"configure 7", "prepare 7 at 40.1", "generate 7", "configure 8", "prepare 8 reset at 40.1", "generate 8 reset"}));
    CHECK(fsr4fg::generate(kCmd, frame(10, 40.1f)) == fgr::MADE);
    take();
    // a present without its inputs is not handed over at all
    fsr4fg::Inputs bare = frame(11, 40.1f);
    bare.depth = VK_NULL_HANDLE;
    const int calls = amd_calls;
    CHECK(fsr4fg::generate(kCmd, bare) == fgr::OWN && amd_calls == calls && take().empty());
    // another picture size: the GPU idle, AMD's context destroyed, then the image; then both made again, a new history
    CHECK(fsr4fg::generate(kCmd, frame(12, 40.1f, 1920, 1080, 1920, 1080)) == fgr::SKIP);
    CHECK(is(take(), {"wait_gpu", "destroy_context", "destroy_image", "create_image 1920x1080", "create_context 1920x1080", "image to GENERAL",
                      "configure 10", "prepare 10 reset at 40.1", "generate 10 reset"}));
    CHECK(contexts_live == 1 && images_live == 1);
    CHECK(fsr4fg::generate(kCmd, frame(13, 40.1f, 1920, 1080, 1920, 1080)) == fgr::MADE);
    take();
    // the row leaves AMD FSR 4: nothing for 119 presents, then the context and the image go, in that order
    opts.framegen = 2;
    CHECK(fsr4fg::generate(kCmd, frame(14, 40.1f, 1920, 1080, 1920, 1080)) == fgr::OWN && take().empty());
    for (int i = 0; i < 119; i++) fsr4fg::present_tick();
    CHECK(take().empty() && contexts_live == 1);
    fsr4fg::present_tick();
    CHECK(is(take(), {"wait_gpu", "destroy_context", "destroy_image"}) && !contexts_live && !images_live && has_log("row left AMD FSR 4"));
    const int after = amd_calls;
    for (int i = 0; i < 300; i++) fsr4fg::present_tick();
    CHECK(take().empty() && amd_calls == after);
    // and comes back: made again, a new history; AMD's library is not asked what it has a second time
    opts.framegen = 9;
    CHECK(fsr4fg::generate(kCmd, frame(15, 40.1f, 1920, 1080, 1920, 1080)) == fgr::SKIP);
    CHECK(is(take(), {"create_image 1920x1080", "create_context 1920x1080", "image to GENERAL", "configure 12", "prepare 12 reset at 40.1",
                      "generate 12 reset"}));
    CHECK(library_loads == 1 && contexts_made == 3);
    return std::puts("PASS: AMD FSR 4's frame generation against a stand-in for AMD's library: each frame's calls, histories, sizes, release"), 0;
}
'''

def source_checks():
    fsr4 = (VK / "vk_fsr4.cpp").read_text(encoding="utf-8")
    framegen = (VK / "vk_framegen.cpp").read_text(encoding="utf-8")
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (VK / "vk_menu.h").read_text(encoding="utf-8")
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    inputs = (VK / "vk_fsr3_inputs.cpp").read_text(encoding="utf-8")

    def need(text, what, where):
        if what not in text:
            raise SystemExit("%s: missing %r" % (where, what))

    # the default stays Off, and the row's table takes an options file's AMD FSR 4 before the graphics card is known
    need(header, "int framegen = 0;", "vk_menu.h (the Frame generation row's default)")
    need(menu, '{TAB_GRAPHICS, "Frame generation", &opts.framegen, 0, 9, 1, tx_framegen,', "vk_menu.cpp rows[]")
    sync = function(menu, "void framegen_row_sync() {")
    need(sync, "r.hi = framegen_rows::row_hi(fsr4fg::offered_known())", "framegen_row_sync")   # ((Triforce) as known: menu_ahead)
    need(sync, "framegen_rows::kRowMax == 9", "framegen_row_sync")
    if menu.count("framegen_row_sync();") < 2:
        raise SystemExit("vk_menu.cpp: the Frame generation row's end is not set at both places the menu opens")
    need(function(menu, "const char* hn_framegen(int v, int variant) {"), "if (v == framegen_rows::FSR4) return hn_framegen_fsr4(variant);",
         "hn_framegen")
    # the made frame: asked of AMD's library first, at the row's AMD FSR 4 only; what it does not make falls to the renderer's own,
    # and a new history gets no made frame at all
    need(function(framegen, "int multiplier() {"), "if (opts.framegen == framegen_rows::FSR4) return 2;", "vk_framegen.cpp multiplier")
    record = function(framegen, "bool framegen_record(")
    for what in ("if (opts.framegen == framegen_rows::FSR4) {", "fsr4_frame(cmd, *set, cur, uw, uh, sequence, blend)",
                 "none = by_fsr4 == framegen_rows::SKIP;", "if (set->made == 0 && !none && blend && m > 1) {"):
        need(record, what, "framegen_record")
    # ((Triforce) the renderer's own vectors are make_vectors')
    if not record.index("fsr4_frame(") < record.index("fsr3fi_frame(") < record.index("make_vectors(cmd", record.index("fsr4_frame(")):
        raise SystemExit("framegen_record: AMD FSR 4 is not asked before the renderer's own interpolation")
    frame = function(framegen, "framegen_rows::Made fsr4_frame(")
    for what in ("if (!fsr4fg::frames_wanted()) return framegen_rows::OWN;", "fsr3_inputs::take(f)", "in.reset = f.reset || !blend;",
                 "in.sequence = sequence;", "fsr4fg::generate(cmd, in)", "set.frame[0].image"):
        need(frame, what, "fsr4_frame")
    # its inputs are gathered while it is asked, its context looked after each present, and the exit reaches it
    need(inputs, "|| fsr4fg::frames_wanted()", "vk_fsr3_inputs.cpp wanted()")
    need(framegen, "void framegen_present_tick() { fsr4fg::present_tick(); }", "vk_framegen.cpp")
    need(framegen, "void framegen_exiting() { fsr4fg::exiting(); }", "vk_framegen.cpp")
    need(replay, "framegen_present_tick();", "vk_replay.cpp (each present)")
    at = replay.find("if (id == HOS_OPT_EXITING) framegen_exiting();")
    if at < 0 or not at < replay.find("if (id == HOS_OPT_EXITING) return dlssg::shutdown();"):
        raise SystemExit("vk_replay.cpp: HOS_OPT_EXITING does not reach frame generation (before the return for DLSS's)")
    # the real steps of the teardown are framegen_rows.h's, in its order, and the exit's are the exit's
    need(function(fsr4, "void release(Fg& s, const char* why) {"), "fgr::teardown(d, false);", "vk_fsr4.cpp release")
    need(function(fsr4, "void exiting() {"), "fgr::teardown(e, true);", "vk_fsr4.cpp exiting")
    down = between(fsr4, "struct Down {", "struct AtExit {")
    for what in ("device_wait_idle_locked();", "lib().destroy(&s.context, nullptr)", "destroy_image(s.out, s.out_memory)"):
        need(down, what, "vk_fsr4.cpp Down")
    at_exit = between(fsr4, "struct AtExit {", "void release(Fg& s")
    if "lib()" in at_exit or "destroy_image(" in at_exit or "device_wait_idle" in at_exit:
        raise SystemExit("vk_fsr4.cpp AtExit: something of AMD's or of the GPU is still called at the exit")
    need(at_exit, "g_fg_exiting.store(true);", "vk_fsr4.cpp AtExit")
    # a build without FSR, and one that is not for Windows, have the same functions answering "not here"
    stubs = fsr4[fsr4.index("#else   // (not Windows"):]
    if stubs.count("framegen_rows::Why why() { return framegen_rows::FG_NOT_BUILT; }") != 2 or stubs.count("void exiting() {}") != 2:
        raise SystemExit("vk_fsr4.cpp: the frame generation stubs of a build without FSR, or not for Windows, are missing")
    # the rows are documented
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    row = [line for line in readme.splitlines() if line.startswith("| Frame generation |")]
    if len(row) != 1 or "AMD FSR 4" not in row[0] or "2x up to 8x" not in row[0]:
        raise SystemExit("readme.md: the Frame generation row does not list its choices with AMD FSR 4")
    dev = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
    key = [line for line in dev.splitlines() if line.startswith("| `frame_generation` |")]
    if len(key) != 1 or "9 AMD FSR 4" not in key[0] or "2 to 8 the multiplier" not in key[0]:
        raise SystemExit("docs/development.md: frame_generation's values do not list 9, AMD FSR 4")
    need(dev, "`RX_VK_FSR4_FG=0/1`", "docs/development.md (the switches)")
    print("PASS: the row, the made frame, the present's tick, the exit and the documents are wired to AMD FSR 4's frame generation")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--headers", type=Path)
    a = p.parse_args()
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    fsr4 = (VK / "vk_fsr4.cpp").read_text(encoding="utf-8")
    fsr4_h = (VK / "vk_fsr4.h").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="framegen-fsr4-") as tmp:
        build_and_run(a.cxx, tmp, "rows", ROWS, ["-I" + str(VK)])
        text = MENU.replace("/* TX */", function(menu, "const char* tx_framegen(int v) {"))
        text = text.replace("/* HN */", function(menu, "const char* hn_framegen_fsr4(int variant) {"))
        build_and_run(a.cxx, tmp, "menu", text, ["-I" + str(VK)])
        if os.name != "nt":
            print("skipped: AMD's frame generation section is the Windows build's (AMD publishes no library elsewhere)")
        elif not a.headers or not (a.headers / "vulkan" / "vulkan.h").is_file():
            raise SystemExit("--headers <Vulkan-Headers/include> is needed for AMD's frame generation section")
        else:
            section = between(fsr4, "namespace fsr4fg {\nnamespace {\nnamespace fgr = framegen_rows;", "#else   // (not Windows")
            text = FG.replace("/* INPUTS */", between(fsr4_h, "struct Inputs {", "/// Why AMD's library cannot make the frames here"))
            text = text.replace("/* SECTION */", section)
            there, bare = Path(tmp) / "with", Path(tmp) / "without"
            there.mkdir(), bare.mkdir()
            (there / "amd_fidelityfx_framegeneration_vk.dll").write_bytes(b"a stand-in: only looked for by its name")
            scenes = ["play", "no-device", "no-library", "no-configure", "no-model", "none-listed", "any-version", "env-off", "fsr4-off",
                      "row", "create-fails", "image-fails", "dispatch-fails", "configure-fails", "exit"]
            build_and_run(a.cxx, tmp, "fg", text,
                          ["-I" + str(a.headers), "-I" + str(ROOT / "third_party" / "fidelityfx"), "-I" + str(VK), "-Wno-unknown-pragmas",
                           "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-const-variable", "-Wno-unused-parameter",
                           "-Wno-deprecated-declarations"],
                          runs=[(s, str(there)) for s in scenes] + [("no-file", str(bare))])
    source_checks()


if __name__ == "__main__":
    main()
