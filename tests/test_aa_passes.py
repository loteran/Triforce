"""The Anti-aliasing row's own file, gpu/vk/vk_aa.cpp, compiled whole against the renderer's headers and the Vulkan headers and
run with stand-ins for what it asks of the rest of the renderer (the device, the frame's number, the options, what upscales,
the log, images and buffers).

Without a GPU (the suite's run): what a frame's dispatches are told. With the game's own SMAA chosen nothing is asked and
nothing left out; another choice's first frame still runs the game's edge and weight passes, and they go from the frame after
a composition was replaced, until one is left to the game; the choice is taken once a frame, whatever the row does meanwhile;
an upscaler with its own anti-aliasing puts the game's own back; none replaced for a minute is said once; each reason a
composition was left to the game is logged once. Run again with RX_VK_AA and RX_VK_AA_KEEP set: the run's choice over the row,
and the two passes kept.

--gpu (not in the suite: it needs a graphics card and its Vulkan driver, and is Windows only as written): the passes
themselves, recorded by vk_aa.cpp's aa_record on this PC's first Vulkan 1.3 device, from an authored picture (an RGBA16F
image) into a B10G11R11 image through an R32_UINT storage view, as the game's target is written, and read back.
 - Off: every pixel is the picture's square root packed as RGB9E5, over the whole range the format holds (the GPU's own log2
   and exp2);
 - FXAA: every pixel is what aa_fxaa.glsl compiled as C++ gives for it (tests/test_aa_shaders.py's fixture), so the SPIR-V
   and the logic run in the suite are the same thing;
 - SMAA Ultra: a flat picture and what is away from an edge come out as Off's, a stair-stepped edge's pixels come out
   between the two sides, inside their range; the lookup textures' upload and the four passes run on a real driver.

    python -I -B tests/test_aa_passes.py --headers <Vulkan-Headers/include> [--cxx g++] [--gpu]
"""
import argparse
import importlib.util
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]
RUN_ALL_SKIP = {"linux": "compiles vk_aa.cpp with the renderer's Windows loader header: checked on Windows"}

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

HARNESS = r'''
} // namespace glsl

#include "vk_aa.h"
#include "aa_rows.h"
#include "vk_fsr3.h"
#include "vk_menu.h"
#include "vk_temporal.h"
#include "vk_texture.h"
#include <atomic>
#include <cstdarg>
#include <string>

namespace vkx {
#define VKX_DEFINE(f) PFN_##f f;
VKX_GLOBAL(VKX_DEFINE)
VKX_INSTANCE(VKX_DEFINE)
VKX_DEVICE(VKX_DEFINE)
VKX_DEVICE2(VKX_DEFINE)
VKX_DEVICE3(VKX_DEFINE)
#undef VKX_DEFINE
} // namespace vkx

// ---- what vk_aa.cpp asks of the rest of the renderer ----
namespace mkwvk {
Device dev;
Options opts;
const MkwVkHost* host = nullptr;
std::atomic<bool> par_active{false};
std::atomic<uint64_t> work_epoch{0};
static std::vector<std::string> said;
static bool echo = false;
void logf(const char* fmt, ...) {
    char b[600];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    said.push_back(b);
    if (echo) std::printf("  log: %s\n", b);
}
static int upscaling = 0;   // (upscaler_rows.h: what the temporal chain draws with)
int temporal_mode() { return upscaling; }
static bool before_bloom = false;   // (the Upscale before bloom row: the upscaler takes the scene, before the SMAA)
bool temporal_before_bloom() { return before_bloom; }
bool fsr3_wanted() { return false; }
bool fsr3_native() { return false; }
static uint64_t frame_no = 10;
uint64_t texture_frame_no() { return frame_no; }
VkResult shared_cache_compute(const VkComputePipelineCreateInfo* ci, VkPipeline* out) {
    return vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, ci, nullptr, out);
}

struct Memory {
    VkDeviceMemory memory = VK_NULL_HANDLE;
};
static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < dev.memory.memoryTypeCount; i++)
        if ((bits >> i & 1) && (dev.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
bool create_image(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool, bool, bool) {
    if (vkCreateImage(dev.device, &ci, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev.device, image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size, ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Memory* m = new Memory;
    if (vkAllocateMemory(dev.device, &ai, nullptr, &m->memory) != VK_SUCCESS) return false;
    allocation = reinterpret_cast<VmaAllocation>(m);
    return vkBindImageMemory(dev.device, image, m->memory, 0) == VK_SUCCESS;
}
void destroy_image(VkImage image, VmaAllocation allocation) {
    vkDestroyImage(dev.device, image, nullptr);
    Memory* m = reinterpret_cast<Memory*>(allocation);
    vkFreeMemory(dev.device, m->memory, nullptr);
    delete m;
}
bool create_buffer(uint64_t size, VkBufferUsageFlags usage, bool, VkBuffer& buffer, VmaAllocation& allocation, void** mapped, bool) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size, ci.usage = usage;
    if (vkCreateBuffer(dev.device, &ci, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev.device, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Memory* m = new Memory;
    if (vkAllocateMemory(dev.device, &ai, nullptr, &m->memory) != VK_SUCCESS) return false;
    allocation = reinterpret_cast<VmaAllocation>(m);
    if (vkBindBufferMemory(dev.device, buffer, m->memory, 0) != VK_SUCCESS) return false;
    return !mapped || vkMapMemory(dev.device, m->memory, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS;
}
void destroy_buffer(VkBuffer buffer, VmaAllocation allocation) {
    vkDestroyBuffer(dev.device, buffer, nullptr);
    Memory* m = reinterpret_cast<Memory*>(allocation);
    vkFreeMemory(dev.device, m->memory, nullptr);
    delete m;
}
void flush_mapped(VmaAllocation, uint64_t) {}   // (coherent memory)
} // namespace mkwvk

using namespace mkwvk;
namespace ar = mkwvk::aa_rows;
namespace ur = mkwvk::upscaler_rows;

static bool said_has(const char* what) {
    for (const std::string& s : said)
        if (s.find(what) != std::string::npos) return true;
    return false;
}
constexpr uint64_t kNetwork = 0xfd029a317a9a4b94ull;   // (the gameplay upscale: another program)

// ---- what a frame's dispatches are told (no GPU) ----
static int logic(const char* env) {
    if (!std::strcmp(env, "fxaa")) {   // (RX_VK_AA=2 in the environment: the run's choice, whatever the row holds)
        CHECK(aa_env_choice() == ar::FXAA);
        for (int row = 0; row < ar::kValues; row++) {
            opts.aa = row, frame_no++;
            CHECK(aa_asked() && aa_mode() == ar::FXAA);
        }
        upscaling = ur::DLAA, frame_no++;
        CHECK(aa_mode() == ar::FXAA);   // (the switch over an upscaler too: a lab's A/B of the game's SMAA under it)
        std::puts("aa passes: RX_VK_AA is the run's choice");
        return 0;
    }
    if (!std::strcmp(env, "keep")) {   // (RX_VK_AA_KEEP=1: the game's edge and weight passes always run)
        opts.aa = ar::OFF, frame_no++;
        CHECK(aa_mode() == ar::OFF);
        aa_composed(true, nullptr);
        frame_no++;
        CHECK(!aa_leave_out(ar::kEdgesKey) && !aa_leave_out(ar::kWeightsKey));
        std::puts("aa passes: RX_VK_AA_KEEP keeps the game's edge and weight passes");
        return 0;
    }
    CHECK(aa_env_choice() == -1);
    // the game's own SMAA: nothing asked, nothing left out, nothing said, frame after frame
    for (int i = 0; i < 100; i++, frame_no++) {
        CHECK(!aa_asked());
        CHECK(!aa_leave_out(ar::kEdgesKey) && !aa_leave_out(ar::kWeightsKey) && !aa_leave_out(ar::kComposeKey) && !aa_leave_out(kNetwork));
    }
    CHECK(aa_state() == ar::QUIET && said.empty());
    CHECK(!aa_record(VK_NULL_HANDLE, ar::GAME, VK_NULL_HANDLE, VK_NULL_HANDLE, 16, 16));
    // Off: its first frame still runs the game's edge and weight passes (no composition replaced yet)
    opts.aa = ar::OFF, frame_no++;
    CHECK(aa_asked() && !aa_leave_out(ar::kEdgesKey) && !aa_leave_out(ar::kWeightsKey) && aa_mode() == ar::OFF);
    aa_composed(true, nullptr);
    CHECK(aa_state() == ar::REPLACED && said.size() == 1 && said_has("the game's SMAA composition replaced by Off"));
    // (the choice is the frame's: the row changing in the middle of a frame changes the next one)
    opts.aa = ar::SMAA;
    CHECK(aa_mode() == ar::OFF);
    opts.aa = ar::OFF;
    // from the next frame they are left out; the composition itself and other programs never are
    frame_no++;
    CHECK(aa_leave_out(ar::kEdgesKey) && aa_leave_out(ar::kWeightsKey) && !aa_leave_out(ar::kComposeKey) && !aa_leave_out(kNetwork) &&
          !aa_leave_out(0));
    aa_composed(true, nullptr);
    CHECK(said.size() == 1);   // (said once, not every frame)
    // a composition left to the game: its reason logged once, and the two passes run again from the next frame
    static const char* const why = "it stores into no B10G11R11 target as integer words";
    frame_no++;
    CHECK(aa_leave_out(ar::kEdgesKey));
    aa_composed(false, why);
    CHECK(said.size() == 2 && said_has("left to the game (Off): it stores into no B10G11R11 target"));
    frame_no++;
    CHECK(!aa_leave_out(ar::kEdgesKey) && !aa_leave_out(ar::kWeightsKey));
    aa_composed(false, why);
    CHECK(said.size() == 2);
    frame_no++;
    CHECK(!aa_leave_out(ar::kEdgesKey));
    aa_composed(true, nullptr);
    CHECK(said.size() == 3);   // (replaced again: said again)
    frame_no++;
    CHECK(aa_leave_out(ar::kEdgesKey));
    // DLSS comes on after the bloom: the game's SMAA stays out (Off under it: AMD and NVIDIA want jittered frames no other
    // anti-aliasing has been over); with Upscale before bloom the game's own whole again, at once; and off again: Off's first
    // frame runs the two passes
    aa_composed(true, nullptr);
    upscaling = ur::DLSS, frame_no++;
    CHECK(aa_asked() && aa_mode() == ar::OFF && aa_leave_out(ar::kEdgesKey) && aa_leave_out(ar::kWeightsKey));
    aa_composed(true, nullptr);
    before_bloom = true, frame_no++;
    CHECK(aa_asked() && aa_mode() == ar::GAME && !aa_leave_out(ar::kEdgesKey) && !aa_leave_out(ar::kWeightsKey) && aa_state() == ar::QUIET);
    before_bloom = false;
    upscaling = ur::GAME, frame_no++;
    CHECK(aa_mode() == ar::OFF && !aa_leave_out(ar::kEdgesKey));
    aa_composed(true, nullptr);
    frame_no++;
    CHECK(aa_leave_out(ar::kEdgesKey));
    // back to the game's own: the frame it is taken in still asks, and from then on nothing does
    opts.aa = ar::GAME, frame_no++;
    CHECK(aa_asked() && !aa_leave_out(ar::kEdgesKey) && aa_mode() == ar::GAME);
    CHECK(!aa_asked() && aa_state() == ar::QUIET);
    frame_no++;
    CHECK(!aa_asked() && !aa_leave_out(ar::kWeightsKey));
    // FXAA with none of the game's compositions met for a minute (another game version): said once in the log, and to the row
    opts.aa = ar::FXAA;
    const size_t before = said.size();
    for (uint64_t i = 0; i < 2 * ar::kMissingFrames; i++) {
        frame_no++;
        CHECK(!aa_leave_out(kNetwork));
        CHECK((aa_state() == ar::MISSING) == (i >= ar::kMissingFrames));
    }
    CHECK(said.size() == before + 1 && said_has("FXAA replaced none of the game's SMAA compositions in 3600 frames"));
    frame_no++;
    aa_leave_out(ar::kComposeKey);
    aa_composed(true, nullptr);
    CHECK(aa_state() == ar::REPLACED);
    std::puts("aa passes: what the frame's dispatches are told: all checks passed");
    return 0;
}

// ---- the passes on this PC's GPU ----
static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer cmd;

static bool gpu_start() {
    const HMODULE lib = LoadLibraryExA("vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return false;
    const auto gipa = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(lib, "vkGetInstanceProcAddr");
    if (!gipa) return false;
#define VKX_LOAD(f) vkx::f = (PFN_##f)gipa(nullptr, #f);
    VKX_GLOBAL(VKX_LOAD)
#undef VKX_LOAD
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (!vkCreateInstance || vkCreateInstance(&ici, nullptr, &dev.instance) != VK_SUCCESS) return false;
#define VKX_LOAD(f) vkx::f = (PFN_##f)gipa(dev.instance, #f);
    VKX_INSTANCE(VKX_LOAD)
#undef VKX_LOAD
    uint32_t n = 16;
    VkPhysicalDevice devices[16];
    vkEnumeratePhysicalDevices(dev.instance, &n, devices);
    uint32_t family = 0;
    for (uint32_t i = 0; i < n && !dev.physical; i++) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &ne, exts.data());
        bool push = false;
        for (const VkExtensionProperties& e : exts) push |= !std::strcmp(e.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        uint32_t nq = 16;
        VkQueueFamilyProperties families[16];
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &nq, families);
        for (uint32_t q = 0; q < nq && push && !dev.physical; q++)
            if (families[q].queueFlags & VK_QUEUE_COMPUTE_BIT) dev.physical = devices[i], dev.props = props, family = q;
    }
    if (!dev.physical) return false;
    vkGetPhysicalDeviceMemoryProperties(dev.physical, &dev.memory);
    VkPhysicalDeviceVulkan13Features have13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have.pNext = &have13;
    vkGetPhysicalDeviceFeatures2(dev.physical, &have);
    if (!have13.synchronization2) return false;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    f2.features.shaderStorageImageExtendedFormats = have.features.shaderStorageImageExtendedFormats;
    dev.features = f2.features;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family, qi.queueCount = 1, qi.pQueuePriorities = &priority;
    const char* const ext = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2, dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qi, dci.enabledExtensionCount = 1, dci.ppEnabledExtensionNames = &ext;
    if (vkCreateDevice(dev.physical, &dci, nullptr, &dev.device) != VK_SUCCESS) return false;
#define VKX_LOAD(f) vkx::f = (PFN_##f)vkGetDeviceProcAddr(dev.device, #f);
    VKX_DEVICE(VKX_LOAD)
    VKX_DEVICE2(VKX_LOAD)
    VKX_DEVICE3(VKX_LOAD)
#undef VKX_LOAD
    dev.push_descriptor = true, dev.max_push_descriptors = 32, dev.queue_family = family;
    vkGetDeviceQueue(dev.device, family, 0, &queue);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = family;
    if (vkCreateCommandPool(dev.device, &pi, nullptr, &pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool, ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = 1;
    return vkAllocateCommandBuffers(dev.device, &ai, &cmd) == VK_SUCCESS;
}

static uint16_t to_half(float f) {   // (positive values in a half's normal range, or 0)
    if (!(f > 0.0f)) return 0;
    int e = 0;
    const float m = std::frexp(f, &e);   // (f = m * 2^e, m in [0.5, 1))
    int exponent = e - 1 + 15;
    uint32_t mantissa = (uint32_t)std::lround((m * 2.0f - 1.0f) * 1024.0f);
    if (mantissa == 1024) mantissa = 0, exponent++;
    if (exponent <= 0) return 0;
    if (exponent >= 31) return 0x7bff;
    return (uint16_t)(exponent << 10 | mantissa);
}
static float from_half(uint16_t h) {
    const int exponent = h >> 10 & 31;
    return exponent ? std::ldexp(1.0f + (float)(h & 1023) / 1024.0f, exponent - 15) : 0.0f;
}
static void all_work_done(VkCommandBuffer c) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(c, &di);
}

/// mode's pass over the picture (glsl::picture, W x H; taken as the halves the image holds), as aa_record records it into a
/// B10G11R11 image seen as R32_UINT: the image's words
static std::vector<uint32_t> run(int mode) {
    using namespace glsl;
    const uint32_t w = (uint32_t)W, h = (uint32_t)H;
    VkImage src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE;
    VmaAllocation src_memory = nullptr, dst_memory = nullptr, staging_memory = nullptr, back_memory = nullptr;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R16G16B16A16_SFLOAT, ci.extent = {w, h, 1}, ci.mipLevels = 1, ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CHECK(create_image(ci, src, src_memory, false, false, false));
    // (the target as vk_texture.cpp makes the game's colour targets: any format of its class may be a view of it)
    ci.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    CHECK(create_image(ci, dst, dst_memory, false, false, false));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView src_view = VK_NULL_HANDLE, dst_view = VK_NULL_HANDLE;
    vi.image = src, vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    CHECK(vkCreateImageView(dev.device, &vi, nullptr, &src_view) == VK_SUCCESS);
    VkImageViewUsageCreateInfo uci{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    uci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    vi.pNext = &uci, vi.image = dst, vi.format = VK_FORMAT_R32_UINT;
    CHECK(vkCreateImageView(dev.device, &vi, nullptr, &dst_view) == VK_SUCCESS);
    VkBuffer staging = VK_NULL_HANDLE, back = VK_NULL_HANDLE;
    void* staging_at = nullptr;
    void* back_at = nullptr;
    CHECK(create_buffer((uint64_t)w * h * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging, staging_memory, &staging_at, false));
    CHECK(create_buffer((uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, back, back_memory, &back_at, false));
    for (size_t i = 0; i < picture.size(); i++) {
        const uint16_t t[4] = {to_half(picture[i].r), to_half(picture[i].g), to_half(picture[i].b), to_half(1.0f)};
        std::memcpy((uint8_t*)staging_at + i * 8, t, 8);
        picture[i] = vec3(from_half(t[0]), from_half(t[1]), from_half(t[2]));
    }
    std::memset(back_at, 0xff, (size_t)w * h * 4);
    vkResetCommandPool(dev.device, pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS);
    VkImageMemoryBarrier2 ib[2]{{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};
    for (int i = 0; i < 2; i++) {
        ib[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ib[i].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[i].srcQueueFamilyIndex = ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib[i].image = i ? dst : src, ib[i].subresourceRange = vi.subresourceRange;
    }
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 2, di.pImageMemoryBarriers = ib;
    vkCmdPipelineBarrier2(cmd, &di);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging, src, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    all_work_done(cmd);   // (as the dispatch's own barrier before a stand-in)
    CHECK(aa_record(cmd, mode, src_view, dst_view, w, h));
    all_work_done(cmd);
    vkCmdCopyImageToBuffer(cmd, dst, VK_IMAGE_LAYOUT_GENERAL, back, 1, &region);
    CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
    VkCommandBufferSubmitInfo ci2{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    ci2.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1, si.pCommandBufferInfos = &ci2;
    CHECK(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS);
    CHECK(vkQueueWaitIdle(queue) == VK_SUCCESS);
    std::vector<uint32_t> words((size_t)w * h);
    std::memcpy(words.data(), back_at, words.size() * 4);
    vkDestroyImageView(dev.device, src_view, nullptr), vkDestroyImageView(dev.device, dst_view, nullptr);
    destroy_image(src, src_memory), destroy_image(dst, dst_memory);
    destroy_buffer(staging, staging_memory), destroy_buffer(back, back_memory);
    frame_no++;
    return words;
}

static glsl::vec3 unpack(uint32_t w) {
    const float scale = std::ldexp(1.0f, (int)(w >> 27) - 15 - 9);
    return {(float)(w & 511u) * scale, (float)((w >> 9) & 511u) * scale, (float)((w >> 18) & 511u) * scale};
}
static float step_of(uint32_t w) { return std::ldexp(1.0f, (int)(w >> 27) - 15 - 9); }
static float apart(glsl::vec3 a, glsl::vec3 b) {
    return std::fmax(std::fabs(a.r - b.r), std::fmax(std::fabs(a.g - b.g), std::fabs(a.b - b.b)));
}

static int gpu() {
    using namespace glsl;
    echo = true;
    if (!gpu_start()) {
        std::puts("aa passes: no Vulkan 1.3 device with push descriptors on this PC: the passes were not run");
        return 2;
    }
    std::printf("aa passes: on %s\n", dev.props.deviceName);
    // ---- Off over the format's whole range: zero, powers of two (their square roots exact), the smallest and the largest ----
    W = 64, H = 32, picture.assign((size_t)W * H, vec3(0.0f));
    unsigned seed = 99;
    const auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.0f; };
    for (int i = 0; i < W * H; i++) {
        const float top = std::ldexp(1.0f, i % 28 - 13);   // (2^-13 to 2^14)
        picture[i] = i < 64 ? vec3(std::ldexp(1.0f, 2 * (i % 14) - 12), std::ldexp(1.0f, 2 * (i % 7) - 6), i % 3 ? 1.0f : 0.0f)
                            : vec3(rnd() * top, rnd() * top, rnd() * top);
    }
    picture[70] = vec3(60000.0f, 0.5f, 0.0f), picture[71] = vec3(0.0f);
    {
        const std::vector<uint32_t> words = run(ar::OFF);
        float worst = 0.0f;
        for (int i = 0; i < W * H; i++) {
            const vec3 want = aa_encode(picture[i]);
            const uint32_t cpu = aa_pack_rgb9e5(want);
            // (the GPU's log2 may put a power of two an exponent higher or lower than the CPU's: the same value either way, to
            // within a step of the coarser of the two)
            const float step = std::fmax(step_of(words[i]), step_of(cpu));
            CHECK(apart(unpack(words[i]), want) <= 0.51f * step);
            CHECK(step_of(words[i]) <= 2.0f * step_of(cpu) && step_of(cpu) <= 2.0f * step_of(words[i]));
            worst = std::fmax(worst, apart(unpack(words[i]), want) / step);
        }
        CHECK(words[71] == 0u && (words[70] >> 27) == 23u);   // (sqrt(60000) = 245, in [2^7, 2^8): exponent 7 + 16)
        std::printf("aa passes: Off stores the picture's square root as RGB9E5 (worst %.3f of a step over %d pixels)\n", worst, W * H);
    }
    // ---- the stair-stepped edge of tests/test_aa_shaders.py ----
    W = 96, H = 48, picture.assign((size_t)W * H, vec3(0.0f));
    const vec3 dark(0.02f, 0.03f, 0.05f), bright(0.8f, 0.7f, 0.5f);
    const auto edge_at = [](int x) { return 12 + x / 6; };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) picture[(size_t)y * W + x] = y >= edge_at(x) ? bright : dark;
    const std::vector<uint32_t> off = run(ar::OFF);
    const vec3 lo = aa_encode(picture[0]), hi = aa_encode(picture[(size_t)W * H - 1]);
    for (int i = 0; i < W * H; i++) CHECK(apart(unpack(off[i]), aa_encode(picture[i])) <= 0.5f * step_of(off[i]) + 1e-7f);
    {
        // FXAA: the SPIR-V against the same file compiled as C++
        const std::vector<uint32_t> words = run(ar::FXAA);
        float worst = 0.0f;
        int changed = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const vec3 want = aa_fxaa(vec2((x + 0.5f) / W, (y + 0.5f) / H), vec2(1.0f / W, 1.0f / H));
                const uint32_t word = words[(size_t)y * W + x];
                worst = std::fmax(worst, apart(unpack(word), want));
                changed += word != off[(size_t)y * W + x];
            }
        std::printf("aa passes: FXAA on the GPU and aa_fxaa.glsl as C++ differ by %.5f at most (%d pixels smoothed)\n", worst, changed);
        CHECK(worst <= 0.01f && changed >= 40);
    }
    {
        // SMAA Ultra: what is away from the edge as Off stores it, the edge's pixels between the two sides
        const std::vector<uint32_t> words = run(ar::SMAA);
        CHECK(!said_has("cannot run here"));
        int changed = 0, blended = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const uint32_t word = words[(size_t)y * W + x], was = off[(size_t)y * W + x];
                const vec3 o = unpack(word);
                const int away = y - edge_at(x);
                CHECK(o.r >= lo.r - 0.004f && o.r <= hi.r + 0.004f && o.g >= lo.g - 0.004f && o.g <= hi.g + 0.004f &&
                      o.b >= lo.b - 0.004f && o.b <= hi.b + 0.004f);
                if (away >= 2 || away <= -3) CHECK(apart(o, unpack(was)) <= step_of(was));
                changed += apart(o, unpack(was)) > 2.0f * step_of(was);
                blended += std::fabs(o.g * o.g - unpack(was).g * unpack(was).g) > 0.1f * (bright.g - dark.g) && x >= 12 && x < W - 12;
            }
        std::printf("aa passes: SMAA Ultra changed %d pixels along the edge, %d of them by a tenth of the way or more\n", changed, blended);
        CHECK(changed >= 60 && blended >= 40);
        // again (the lookup textures and the work images are there now), the same picture: the same words
        const std::vector<uint32_t> again = run(ar::SMAA);
        CHECK(again == words);
    }
    // ---- a flat picture: every choice stores the same words ----
    for (vec3& p : picture) p = vec3(0.3f, 0.5f, 0.2f);
    {
        const std::vector<uint32_t> flat = run(ar::OFF), fxaa = run(ar::FXAA), smaa = run(ar::SMAA);
        CHECK(flat == fxaa);
        for (size_t i = 0; i < flat.size(); i++) CHECK(apart(unpack(flat[i]), unpack(smaa[i])) <= step_of(flat[i]));
        CHECK(flat[0] == flat[flat.size() - 1] && apart(unpack(flat[0]), aa_encode(picture[0])) <= 0.5f * step_of(flat[0]) + 1e-7f);
    }
    // another size: SMAA's work images are made again (the old ones let go), and the passes still run
    W = 40, H = 24, picture.assign((size_t)W * H, vec3(0.1f));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) picture[(size_t)y * W + x] = x + y >= 30 ? bright : dark;
    {
        const std::vector<uint32_t> a = run(ar::OFF), b = run(ar::SMAA);
        int changed = 0;
        for (size_t i = 0; i < a.size(); i++) changed += apart(unpack(a[i]), unpack(b[i])) > 2.0f * step_of(a[i]);
        CHECK(changed >= 20);   // (a diagonal: SMAA's diagonal search and its area texture's second half)
        std::printf("aa passes: SMAA Ultra at another size, a diagonal edge: %d pixels smoothed\n", changed);
    }
    vkDeviceWaitIdle(dev.device);
    std::puts("aa passes: Off, FXAA and SMAA Ultra recorded by vk_aa.cpp ran on this GPU: all checks passed");
    return 0;
}

int main(int argc, char** argv) {
    const char* what = argc > 1 ? argv[1] : "";
    return !std::strcmp(what, "gpu") ? gpu() : logic(what);
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--gpu", action="store_true", help="also record and run the passes on this PC's GPU")
    a = p.parse_args()
    spec = importlib.util.spec_from_file_location("test_aa_shaders", ROOT / "tests" / "test_aa_shaders.py")
    shaders = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(shaders)
    glsl = ((VK / "shaders" / "aa_common.glsl").read_text(encoding="utf-8") + shaders.FIXTURE +
            "static vec3 aa_at(vec2 uv) { samples++; return aa_encode(bilinear(uv)); }\n" +
            (VK / "shaders" / "aa_fxaa.glsl").read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="aa-passes-") as tmp:
        work = Path(tmp)
        cpp, exe = work / "harness.cpp", work / "harness.exe"
        cpp.write_text(shaders.SHIM + glsl + HARNESS, encoding="utf-8")
        # (vk_aa.cpp as the build compiles it, nx2/build.py's vflags: what it takes for granted is included for it)
        flags = ["-std=c++20", "-O1", "-g0", "-Wno-everything", "-DVK_NO_PROTOTYPES", "-DMKWVK_BUILDING", "-D_CRT_SECURE_NO_WARNINGS"]
        for h in ("algorithm", "cstdio", "cstdlib", "cstring", "functional", "string", "vector"):
            flags += ["-include", h]
        subprocess.run([a.cxx, *flags, "-I" + str(VK), "-I" + str(a.headers), str(VK / "vk_aa.cpp"), str(cpp), "-o", str(exe)], check=True)
        plain = {k: v for k, v in os.environ.items() if k not in ("RX_VK_AA", "RX_VK_AA_KEEP")}
        subprocess.run([str(exe)], check=True, env=plain)
        subprocess.run([str(exe), "fxaa"], check=True, env=dict(plain, RX_VK_AA="2"))
        subprocess.run([str(exe), "keep"], check=True, env=dict(plain, RX_VK_AA_KEEP="1"))
        if a.gpu:
            r = subprocess.run([str(exe), "gpu"], env=plain)
            if r.returncode == 2:
                raise SystemExit("FAIL: --gpu asked and no device to run on")
            if r.returncode:
                raise SystemExit("FAIL: the passes on the GPU")
    print("PASS: vk_aa.cpp's frame logic%s" % (", and its passes on this PC's GPU" if a.gpu else " (its passes on a GPU: --gpu)"))


if __name__ == "__main__":
    main()
