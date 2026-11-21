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
/// (Triforce) The frames the display thread shows for each of the game's now: 0 frame generation is off (the game's own
/// present), -1 Auto (a frame for each refresh of the screen, whatever the game's rate), else the row's multiplier, 2 to 8
/// (AMD FSR 3.1's 2x and AMD FSR 4's frame are 2). For vk_pace.cpp: what a limit on presented frames holds
int framegen_multiplier();
/// Records the frame's processing into the present's recording (src: the window texture's view, sw x sh, the crop
/// x, y, w, h); false when interpolation cannot run (the present goes to the window as without it)
bool framegen_record(VkCommandBuffer cmd, VkImageView src, uint32_t sw, uint32_t sh, const int crop[4]);
/// The first frame the last framegen_record made (null when it made none), w x h, GENERAL
VkImage framegen_made_image(uint32_t* w, uint32_t* h);
/// After that recording's submit (value): the display thread shows its frames
void framegen_queue(uint64_t value);
/// (Triforce) The hold of the last picture (present_hold.h, vk_replay.cpp do_present). A frame held is not given to
/// framegen_record: the display thread shows the held picture (image GENERAL, w x h, crop as above or null; view: for the
/// window's passes, or null) once, after the frames it has still to show, and nothing is made from it
void framegen_queue_held(VkImage image, VkImageView view, uint32_t w, uint32_t h, const int crop[4]);
/// The first frame shown after a hold: no frame is made between it and the one before the hold (the in-house interpolation
/// shows it alone, AMD FSR 4 and FSR 3.1 start their history at it), as after a loading hitch
void framegen_new_history();
/// The last present held its frame: the frame drawn next starts the temporal upscalers' history anew (vk_temporal.cpp)
bool present_held_last();
/// The present under way shows the held picture: DLSS frame generation takes it as a frame with no scene (vk_dlssg.cpp)
bool present_hold_now();
/// The present under way is the first shown after a hold: DLSS frame generation starts anew at it
bool present_after_hold();
/// The window's present of an image (GENERAL, w x h; crop as above or null), vk_replay.cpp: the display thread's
void present_image(VkImage src, uint32_t w, uint32_t h, const int crop[4], VkImageView view = VK_NULL_HANDLE);
/// The Insert menu's upscaling of a frame to the window (dw x dh), recorded: the image to show, or null when it is off
VkImage framegen_scale(VkCommandBuffer cmd, VkImageView src, VkImage image, uint32_t sw, uint32_t sh, const int crop[4], uint32_t dw, uint32_t dh);
VkImageView framegen_scaled_view();   // (hookshot) the view of framegen_scale's last image
/// (hookshot) The HDR10 picture of src's crop at dw x dh (the Insert menu's HDR), GENERAL; null: none (the SDR picture then)
VkImage framegen_hdr(VkCommandBuffer cmd, VkImageView src, uint32_t sw, uint32_t sh, const int crop[4], uint32_t dw, uint32_t dh);
/// (Triforce) Each present, outside any recording of framegen_record's: frame generation's unused resources are let go
void framegen_present_tick();
struct Texture;
/// ((Triforce, FG-2) The gameplay upscale's output (out, its level), just written by the upscale's dispatch or the upscaler in
/// its place, before the HUD is drawn into it: taken as frame generation's picture without the HUD for this frame (AMD's
/// library makes no frame without it). Nothing unless the Frame generation row is at AMD FSR and AMD's library can make the
/// frames; recorded into cmd, on whichever thread records the dispatch (vk_draw.cpp)
void framegen_hudless_note(VkCommandBuffer cmd, Texture* out, uint32_t level);
/// (Triforce) The process is about to exit: nothing of frame generation calls into a vendor's library after this
void framegen_exiting();
/// Each present: ultrawide's menus (the Insert menu's setting to the game's side, the sides blurred or black while a menu shows)
void wide_sides_tick();
/// Whether ultrawide's menu sides can show at all: Ultrawide with a margin and the Insert menu's setting on
bool wide_sides_possible();

} // namespace mkwvk
