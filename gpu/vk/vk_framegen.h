// Frame interpolation (vk_framegen.cpp): made frames shown between the game's (in a window; RX_VK_FRAMEGEN=0: off).
#pragma once

#include "framegen_engine.h"
#include "vk_internal.h"

namespace mkwvk {

bool framegen_on();
/// (hookshot) Who makes the made frames for a Frame generation row value, from what is live now: DLSS Frame Generation, AMD's library at 2x,
/// or the renderer's own (framegen_engine.h). No side effects
fgengine::Choice framegen_choice(int framegen);
/// (hookshot) Each present, first in do_present (the replay's thread): the choice, said in the log when it changes, and AMD's library's
/// swapchain switched on or off with the display thread drained
void framegen_engine_tick();
/// Records the frame's processing into the present's recording (src: the window texture's view, sw x sh, the crop
/// x, y, w, h); false when interpolation cannot run (the present goes to the window as without it)
bool framegen_record(VkCommandBuffer cmd, VkImageView src, uint32_t sw, uint32_t sh, const int crop[4]);
/// The first frame the last framegen_record made (null when it made none), w x h, GENERAL
VkImage framegen_made_image(uint32_t* w, uint32_t* h);
/// After that recording's submit (value): the display thread shows its frames
void framegen_queue(uint64_t value);
/// The window's present of an image (GENERAL, w x h; crop as above or null), vk_replay.cpp: the display thread's
void present_image(VkImage src, uint32_t w, uint32_t h, const int crop[4], VkImageView view = VK_NULL_HANDLE);
/// The Insert menu's upscaling of a frame to the window (dw x dh), recorded: the image to show, or null when it is off
VkImage framegen_scale(VkCommandBuffer cmd, VkImageView src, VkImage image, uint32_t sw, uint32_t sh, const int crop[4], uint32_t dw, uint32_t dh);
VkImageView framegen_scaled_view();   // (hookshot) the view of framegen_scale's last image
/// (hookshot) The HDR10 picture of src's crop at dw x dh (the Insert menu's HDR), GENERAL; null: none (the SDR picture then)
VkImage framegen_hdr(VkCommandBuffer cmd, VkImageView src, uint32_t sw, uint32_t sh, const int crop[4], uint32_t dw, uint32_t dh);
/// Each present: ultrawide's menus (the Insert menu's setting to the game's side, the sides blurred or black while a menu shows)
void wide_sides_tick();
/// Whether ultrawide's menu sides can show at all: Ultrawide with a margin and the Insert menu's setting on
bool wide_sides_possible();

} // namespace mkwvk
