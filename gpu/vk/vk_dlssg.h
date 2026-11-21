// (Triforce) NVIDIA DLSS Frame Generation through NVIDIA's Streamline (vk_dlssg.cpp), after NX2navi's runtime/vk_dlssg.c
// and runtime/dlssg/nx2_sl.cpp. In a build without HOOKSHOT_STREAMLINE_SDK every function here answers "off".
#pragma once

#include <cstdint>
#include <vector>

#include "vk_internal.h"

namespace mkwvk::dlssg {

/// (Triforce) This build has DLSS frame generation (made with NVIDIA's Streamline SDK): the Insert menu's Frame generation row
/// offers NVIDIA DLSS 2x to 4x only then
bool built();
/// The frames shown for each of the game's as asked at the start, 2 to 4; 0: off. The Insert menu's DLSS frame
/// generation row as the options file has it (dlss_frame_generation=), RX_VK_DLSSG=<0|2|3|4> over it. Read once: the
/// Vulkan instance and device are made through Streamline, so a change takes a restart.
int asked();

/// Before the Vulkan instance is made: Streamline loaded and started from the game's folder. Its vkGetInstanceProcAddr,
/// through which the instance, the device and every function of both are then fetched; nullptr when it is not asked
/// for, not built in, its files are missing or it did not start (the loader's own is used, as without it).
PFN_vkGetInstanceProcAddr early();

/// The extensions Vulkan 1.3 made core, asked for by name as well where the device lists them: Streamline's plugins
/// look their functions up under the extensions' names, which resolve only with the extension enabled.
void device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all);

/// The device was not made through Streamline (another vendor's card, or Streamline refused it): off for this run.
void gave_up(const char* why);

/// The device is made (through Streamline): whether frame generation runs on this card and driver, said in the log.
void device_ready(VkPhysicalDevice physical, VkDevice device);

/// Frame generation runs in this session (the swapchain's presents go through Streamline). The game's frames then
/// reach the display from the replay's thread alone, and Reflex is Streamline's.
bool active();
/// (Triforce) Reflex runs through Streamline in this session (frame generation runs and Streamline has Reflex): the
/// renderer's own Reflex (vk_reflex.cpp) stands aside, and the Insert menu's NVIDIA Reflex row shows Streamline's
bool reflex_on();

/// A swapchain was made: Reflex's mode is the swapchain's and is set again.
void swapchain_created();

/// Where a frame of the game starts (after the present of the one before): Reflex's sleep, then the frame's first
/// latency markers. Frames are numbered from here.
void frame_start();
/// (Triforce) Where a frame of the game starts, on the game's presenting thread (the host's window acquire, asked
/// through vk_pace.cpp), period_ns the period of the Game frame rate cap in effect: who holds this frame to it
/// (dlssg_pace.h). true: Reflex does, and its sleep was taken inside this call: the host's own wait steps aside.
/// false: the host waits as before, and frame_start keeps Reflex's sleep. Always false with the Reflex frame limit
/// row off, while no frame is generated, and in a build without Streamline.
bool pace(int64_t period_ns);
/// The frame's submissions went out.
void submit_end();
/// Before the present's command buffer ends: this frame's camera, depth and motion for Streamline, and generation on
/// or off (off in the menu, and while a frame has no scene). cmd: the present's command buffer.
void frame(VkCommandBuffer cmd, uint32_t out_width, uint32_t out_height);
/// Around vkQueuePresentKHR.
void present_marker(bool after);

/// The frames the display gets for each of the game's right now: 1 while generation is off or paused.
int multiplier();
/// One line for the menu about what runs, or why it does not.
const char* status();

void shutdown();

} // namespace mkwvk::dlssg
