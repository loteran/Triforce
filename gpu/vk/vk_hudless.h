// (hookshot) The game's picture without its HUD, kept for the engines that make frames between the game's: NVIDIA DLSS Frame Generation
// (vk_ngxfg.cpp) and AMD FSR 3.1 frame generation through AMD's library (vk_ffx_fg.cpp) both tell the HUD apart from the scene by
// comparing the presented picture with it, and keep the HUD out of the made frames.
//
// Taken once, here, where an upscaler (NVIDIA DLSS, AMD FSR 4 or 3.1) has just written the game's gameplay upscale and the HUD is
// not yet drawn into it; both engines read the same copy. The upscaler's output is encoded as the game's last pass encodes its
// picture (linear light to sRGB, which a blit into an sRGB image does), so a pixel of it is the presented picture's pixel.
// Nothing when no upscaler runs (the game's own network's picture is not taken: none is better than a made-up one).
#pragma once

#include <cstdint>

#include "vk_internal.h"

namespace mkwvk {

struct Texture;

namespace hudless {

/// An engine wants the picture without the HUD this frame (the scene's inputs are being kept for one that makes frames)
bool wanted();

/// Called where an upscaler has just written the game's gameplay upscale into `picture` (its level `level`), before the HUD is
/// drawn into it. Recorded into cmd (GENERAL images). No-op when no engine wants it
void note(VkCommandBuffer cmd, Texture* picture, uint32_t level);

/// The copy taken this frame (or the frame before: the present follows the upscale), RGBA8 UNORM, GENERAL, at the upscaler's own
/// size: valid() false when there is none (no upscaler drew, a size that did not fit, the copy failed)
struct Picture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    bool valid() const { return image != VK_NULL_HANDLE; }
};
Picture current();

/// Let go of its images: the device is idle (the engines' release), or after some seconds of no use (idle)
void release();
void idle();

}  // namespace hudless
}  // namespace mkwvk
