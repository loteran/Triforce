// (hookshot) AMD FSR 3.1 frame generation through AMD's own FidelityFX library (vk_ffx_fg.cpp): the engine that makes the 2x frames when
// NVIDIA DLSS Frame Generation cannot run (framegen_engine.h: DLSS-G, then this, then Triforce's own passes). The library is the one
// FSR 3.1's upscaler loads (vk_ffx3.h): libamd_fidelityfx_vk.so, FidelityFX SDK 1.1.4, built by the CI and placed beside the game.
//
// How it differs from every other engine: AMD's frame generation is a swapchain. Its replacement functions create, acquire from and
// present on a swapchain of its own, and between each two presents of the game it presents a generated frame, on its own threads,
// paced by its own thread. So in this mode the renderer presents once for each frame of the game, as without frame generation
// (vk_replay.cpp present_image), through these functions, and vk_framegen.cpp's display thread makes and shows nothing.
//
// Each frame, before the present's command buffer is submitted (frame()): the context is configured for this frame (generation on
// or off, the picture's rectangle, the picture without the HUD, the frame's id), and when generation is on, the frame's depth and
// motion vectors (vk_fsr3_inputs.h) are handed over in a preparation recorded into that command buffer. AMD's library then calls
// back, on its thread, to record the generation of the frame between this one and the one before.
//
// This header names nothing of AMD's: vk_ffx_fg.cpp and vk_ffx3.cpp alone see the SDK 1.1.4 headers.
#pragma once

#include <cstdint>

#include "vk_internal.h"

namespace mkwvk::ffxfg {

// ---- the device (vk_main.cpp create_device, start) ----

/// AMD's swapchain may be the engine here at all: its library is beside the game, RX_VK_FRAMEGEN_VENDOR is auto or amd, and
/// RX_VK_FFX_FG is not 0. Asked before the device exists: the queues it needs are made only then
bool queues_wanted();
/// The two queues it presents and acquires on, made beside the game's (any other queue of the swapchain's four: the game's own and
/// the async compute one, which is none here). Both a queue of their own: the library refuses a queue used twice. VK_NULL_HANDLE:
/// the device could not give them (said in the log), and AMD's frame generation is not an engine here
void set_queues(VkQueue present, uint32_t present_family, VkQueue acquire, uint32_t acquire_family);
/// The window's surface is made: the present queue's family must present to it, or the engine is off
void surface_made(VkSurfaceKHR surface);

// ---- the choice (vk_framegen.cpp) ----

/// The library is loaded with its entry points, the device gave it its queues, and it has not failed in this run. The first ask
/// loads the library (any thread: the menu asks)
bool usable();
/// Why it is not usable, for the log and the hint ("" when it is)
const char* why_not();
/// Remember for the run that it failed (a swapchain or a context that was not made, a refused configuration): usable() is false from
/// now on, and the engine choice goes to Triforce's own
void note_failed(const char* what, int code);
/// The engine the choice made is this one: the renderer's frame generation makes its swapchain this way (set by do_present, the
/// replay's thread, once the display thread has nothing in flight; present_image and create_swapchain follow it)
void set_target(bool on);
bool target();

// ---- the swapchain (vk_main.cpp create_swapchain, vk_replay.cpp present_image) ----

/// AMD's swapchain context exists: the window's swapchain handle is AMD's object, and every swapchain call goes through its
/// functions below (until create_swapchain has replaced it)
bool active();
/// The swapchain is not the kind the target wants: present_image makes it again. false while the target is on and the engine failed
/// (create_swapchain then makes a plain one)
bool swapchain_stale();
/// The scene's depth, motion and camera are kept, and the picture without the HUD (vk_hudless.h): the swapchain is AMD's
bool inputs_wanted();
bool hudless_wanted();

/// First call makes the swapchain context and its swapchain from ci (its oldSwapchain must be null, and the old one destroyed with
/// destroy_swapchain first; the sharing mode exclusive: the library moves the images between its queues itself); later ones make the
/// swapchain again. dev.present_mu held by the caller. On failure the context is let go of when it was this call's
VkResult create_swapchain(const VkSwapchainCreateInfoKHR& ci, VkSwapchainKHR* out);
/// The swapchain goes, the context stays (release() ends it). dev.present_mu held, the device idle
void destroy_swapchain(VkSwapchainKHR sc);
VkResult get_images(VkSwapchainKHR sc, uint32_t* count, VkImage* images);
VkResult acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore semaphore, uint32_t* index);
/// On the game's queue (dev.queue), with dev.present_mu NOT held: AMD's swapchain submits through our callback, which takes it
VkResult present(const VkPresentInfoKHR* info);
/// Waits for AMD's threads to be done with what they were handed (its queued presents and generations): before the device is idled
/// and its swapchain or context destroyed, with no lock held. Nothing when it is not active
void quiesce();
/// Its contexts go (the frame generation context, then the swapchain context): the swapchain was destroyed, the device is idle
void release();

// ---- each game frame (vk_replay.cpp present_image) ----

struct FrameArgs {
    VkCommandBuffer cmd = VK_NULL_HANDLE;   ///< the present's recording, before the picture is drawn over: after the game's work
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    int rect[4]{};         ///< where the game's picture is in the swapchain's image {x, y, w, h}: generation stays inside it
    bool game_frame = false;   ///< a frame of the game (not the start-up screen)
    bool plain = false;    ///< the picture reaches the swapchain image by the plain blit of its crop (no window filter, no HDR pass, no
                           ///< ultrawide sides): the picture without the HUD can be made to match it, else it is not given
};
/// Configures generation for this frame, makes the picture without the HUD, and records the preparation into cmd. Nothing for a
/// frame that is not the game's
void frame(const FrameArgs& args);
/// The last frame configured had generation on: AMD's swapchain presents twice for it
bool generating();
/// Frames the swapchain presented, generated ones included; 0 when not active
uint64_t presents(VkSwapchainKHR sc);

}  // namespace mkwvk::ffxfg
