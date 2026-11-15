// The game's picture without its HUD, for the engines that make frames (vk_hudless.h). Moved out of vk_ngxfg.cpp (hudless_note), which
// is NGX-only code: AMD's frame generation (vk_ffx_fg.cpp) needs the same copy, and a build without the NGX SDK must have it too.
#include "vk_hudless.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <vector>

#include "vk_ffx_fg.h"
#include "vk_ngxfg.h"
#include "vk_texture.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk::hudless {

namespace {

struct Img {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};

struct State {
    std::mutex mu;
    Img srgb, snap;               // (the upscaler's picture blitted into sRGB, and those bytes copied as UNORM: the picture we give out)
    uint64_t snap_frame = 0;      // (texture_frame_no when taken; 0: none)
    bool blit_checked = false, blit_ok = false;
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

}  // namespace

bool wanted() { return ngxfg::inputs_wanted() || ffxfg::hudless_wanted(); }

void note(VkCommandBuffer cmd, Texture* picture, uint32_t level) {
    if (!picture || !picture->image || !wanted()) return;
    State& s = st();
    std::lock_guard lk(s.mu);
    free_dead();
    if (!s.blit_checked) {
        // (the picture's format is blitted from and the sRGB image blitted to, or there is no picture without the HUD)
        s.blit_checked = true;
        VkFormatProperties from{}, to{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, picture->format, &from);
        vkGetPhysicalDeviceFormatProperties(dev.physical, VK_FORMAT_R8G8B8A8_SRGB, &to);
        s.blit_ok = (from.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (to.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
        if (!s.blit_ok) logf("vk: hudless: no blit from the upscaler's picture (format %d) into sRGB here: no picture without the HUD", (int)picture->format);
    }
    if (!s.blit_ok || !(picture->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || level >= picture->levels) return;
    const uint32_t w = std::max(1u, picture->width >> level), h = std::max(1u, picture->height >> level);
    const VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!make(cmd, s.srgb, w, h, VK_FORMAT_R8G8B8A8_SRGB, u) ||
        !make(cmd, s.snap, w, h, VK_FORMAT_R8G8B8A8_UNORM, u | VK_IMAGE_USAGE_STORAGE_BIT)) {
        s.snap_frame = 0;
        return;
    }
    full_barrier(cmd);   // (the upscaler's picture, written before)
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {(int)w, (int)h, 1};
    b.dstOffsets[1] = {(int)w, (int)h, 1};
    // (the blit encodes linear light as sRGB, as the game's last pass encodes its picture; the bytes are then copied as they
    // are into the UNORM image the present's copy has the same layout of)
    vkCmdBlitImage(cmd, picture->image, VK_IMAGE_LAYOUT_GENERAL, s.srgb.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    full_barrier(cmd);
    const VkImageCopy cp{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImage(cmd, s.srgb.image, VK_IMAGE_LAYOUT_GENERAL, s.snap.image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    full_barrier(cmd);
    s.snap_frame = texture_frame_no();
}

Picture current() {
    State& s = st();
    std::lock_guard lk(s.mu);
    Picture p;
    if (s.snap.image && s.snap_frame && texture_frame_no() - s.snap_frame <= 1)
        p.image = s.snap.image, p.view = s.snap.view, p.width = s.snap.w, p.height = s.snap.h;
    return p;
}

void release() {
    State& s = st();
    std::lock_guard lk(s.mu);
    drop(s.srgb), drop(s.snap);
    s.snap_frame = 0;
    free_dead(true);   // (the caller has the device idle: nothing is in use)
}

void idle() {
    State& s = st();
    std::unique_lock lk(s.mu, std::try_to_lock);
    if (!lk.owns_lock() || !s.snap.image) return;
    if (texture_frame_no() - s.snap_frame < 300) return;   // (used within the last five seconds' frames)
    drop(s.srgb), drop(s.snap);
    s.snap_frame = 0;
    free_dead();
}

}  // namespace mkwvk::hudless
