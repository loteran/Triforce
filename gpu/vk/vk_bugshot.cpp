// (Triforce) The picture of a bug capture: the frame the window is shown, written as captures/frame-pending.png in the game
// folder, from where the host's capture takes it into its own folder (host/bug_capture.c; after NX2navi's F8, whose code is
// another author's: none of it is here).
//
// The cheapest way to one frame that the back end has is the one its frame log takes (vk_replay.cpp present_image): a copy
// out of an image into a mapped buffer, recorded into the present's own command buffer and read once that frame's fence was
// waited for. Here the image is the window's own, at the last step before the overlays (menu_record calls bugshot_record
// first), so the picture is the frame as it goes to the screen, scaled and with its bars, without the menu or the counter
// over it. A present has three frame slots and waits for a slot's fence before it uses the slot again: three presents
// after the copy was recorded its bytes are there. They are then handed to a thread of their own, which turns them into
// RGB (bugshot_pixels.h: BGRA, or HDR10 brought to sRGB) and writes the PNG, so the presenting thread does no more than
// make one buffer and record four commands. Without a window, or where the driver lets a window's images only be written
// (create_swapchain asks for TRANSFER_SRC where it is offered), there is no picture and the log says why.
#include <algorithm>
#include <atomic>
#include <direct.h>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "bugshot_pixels.h"
#include "vk_bugshot.h"
#include "vk_dump.h"    // (write_png)
#include "vk_menu.h"    // (opts: the HDR paper white)
#include "vk_paths.h"
#include "vk_work.h"   // (after the others: the commands recorded here go the recorder's way, in order with the present's)

namespace mkwvk {
namespace {

static_assert(VK_FORMAT_R8G8B8A8_UNORM == 37 && VK_FORMAT_R8G8B8A8_SRGB == 43 && VK_FORMAT_B8G8R8A8_UNORM == 44 &&
              VK_FORMAT_B8G8R8A8_SRGB == 50 && VK_FORMAT_A2R10G10B10_UNORM_PACK32 == 58 && VK_FORMAT_A2B10G10R10_UNORM_PACK32 == 64,
              "bugshot_pixels.h names the window's formats by these numbers");

std::atomic<int> state{0};          // (bugshot_state)
std::atomic<bool> wanted{false};    // (asked for, and no present has taken it yet)
// the presenting thread's own, between the present that records the copy and the one that hands its bytes on
VkBuffer buf = VK_NULL_HANDLE;
VmaAllocation alloc = nullptr;
void* map = nullptr;
uint32_t buf_w = 0, buf_h = 0;
int presents_left = 0;
bugshot::Kind kind = bugshot::NONE;
bool hdr10 = false;
double white_nits = 200;

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {   // (as vk_replay.cpp's image_barrier)
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

/// The picture's file: frame-pending.png in the game folder's captures folder, which is made when missing
std::string pending_path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    const std::string dir(exe, n);
    const std::string captures = dir.substr(0, dir.find_last_of("\\/") + 1) + "captures";
    _mkdir(captures.c_str());
    return captures + "/frame-pending.png";
}

/// A copy's bytes to their file. On a thread of its own, which the buffer belongs to from here on
void write_out(VkBuffer b, VmaAllocation a, const void* bytes, uint32_t w, uint32_t h, bugshot::Kind k, bool hdr, double white) {
    bool ok = false;
    try {
        const size_t n = (size_t)w * h;
        invalidate_mapped(a, (uint64_t)n * 4);   // (the GPU wrote it: the CPU is to read that)
        std::vector<uint8_t> px((const uint8_t*)bytes, (const uint8_t*)bytes + n * 4);
        destroy_buffer(b, a);
        b = VK_NULL_HANDLE;
        const std::string path = pending_path();
        ok = bugshot::to_rgba8(px.data(), n, k, hdr, white) && write_png(path.c_str(), px.data(), w, h);
        if (ok) logf("vk: bug capture: the frame shown, %ux%u%s, written to %s", w, h, hdr ? " (HDR10 brought to sRGB)" : "", path.c_str());
        else logf("vk: bug capture: no picture: %s could not be written", path.c_str());
    } catch (const std::exception&) {
        if (b) destroy_buffer(b, a);
        logf("vk: bug capture: no picture: no memory for its %ux%u pixels", w, h);
    }
    state.store(ok ? 2 : 3, std::memory_order_release);
}

} // namespace

void bugshot_request(bool want) {
    if (!want) {   // (the host waited long enough: a request no present took is dropped; a copy under way ends as it ends)
        if (wanted.exchange(false, std::memory_order_acq_rel)) state.store(3, std::memory_order_release);
        return;
    }
    for (int s = state.load(std::memory_order_acquire);;) {
        if (s == 1) return;
        if (state.compare_exchange_weak(s, 1, std::memory_order_acq_rel)) break;
    }
    wanted.store(true, std::memory_order_release);
}

int bugshot_state() { return state.load(std::memory_order_acquire); }

void bugshot_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) {
    if (buf) {
        if (--presents_left > 0) return;
        const VkBuffer b = buf;
        const VmaAllocation a = alloc;
        const void* const bytes = map;
        const uint32_t bw = buf_w, bh = buf_h;
        const bugshot::Kind k = kind;
        const bool hdr = hdr10;
        const double white = white_nits;
        buf = VK_NULL_HANDLE, alloc = nullptr, map = nullptr;
        try {
            std::thread([=] {
                SetThreadDescription(GetCurrentThread(), L"mkw_vk bug capture");
                write_out(b, a, bytes, bw, bh, k, hdr, white);
            }).detach();
        } catch (const std::exception&) {   // (no thread: written here, a longer frame this once)
            write_out(b, a, bytes, bw, bh, k, hdr, white);
        }
        return;
    }
    if (!wanted.load(std::memory_order_relaxed) || !wanted.exchange(false, std::memory_order_acq_rel)) return;
    const char* why = nullptr;
    VkSurfaceCapabilitiesKHR caps{};
    kind = bugshot::kind_of((int)dev.swap_format);
    if (dev.offscreen || index >= dev.swap_images.size() || !w || !h) why = "there is no window image to read";
    else if (kind == bugshot::NONE) why = "the window's format is not one the capture reads";
    else if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.physical, dev.surface, &caps) != VK_SUCCESS ||
             !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        why = "this driver's window images can only be written, not read back";
    else if (!create_buffer_cached((uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, buf, alloc, &map) || !map)
        why = "no memory for the read-back";
    if (why) {
        if (buf) destroy_buffer(buf, alloc);   // (made, but not mapped)
        buf = VK_NULL_HANDLE, alloc = nullptr, map = nullptr;
        logf("vk: bug capture: no picture: %s (format %d, %ux%u)", why, (int)dev.swap_format, w, h);
        state.store(3, std::memory_order_release);
        return;
    }
    const VkImage image = dev.swap_images[index];
    barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    {   // (the copy's bytes are for the CPU: made available to the host, as the frame log's)
        VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = buf;
        b.size = (VkDeviceSize)w * h * 4;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.bufferMemoryBarrierCount = 1;
        d.pBufferMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    buf_w = w, buf_h = h, hdr10 = dev.hdr, white_nits = (double)std::clamp(opts.hdr_white, 80, 500);
    presents_left = 3;
}

} // namespace mkwvk
