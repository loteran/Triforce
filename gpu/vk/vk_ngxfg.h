// (hookshot) NVIDIA DLSS Frame Generation ("DLSS-G") through NGX itself, as the Frame generation row's engine on RTX 40 and 50
// cards (vk_ngxfg.cpp): the frames vk_framegen.cpp otherwise makes from optical flow. Built in only with NVIDIA's NGX SDK
// (HOOKSHOT_NGX, scripts/ngx_sdk.py: Linux and Windows, as vk_dlss.cpp); without it these say it is not there and the
// renderer's own frames are made.
//
// RX_VK_FRAMEGEN_VENDOR=auto|nvidia|amd|triforce chooses who makes the frames (framegen_engine.h): auto (the default) DLSS-G where it
// can run, then AMD's library at 2x, then the renderer's own; nvidia only DLSS-G (the renderer's own never replaces it: with it
// unusable the row makes no frames at the multipliers DLSS-G does); amd and triforce never DLSS-G (NGX is not even asked for its
// frame generation's extensions).
#pragma once

#include <cstdint>

#include "dlssg_math.h"
#include "framegen_engine.h"
#include "vk_internal.h"

namespace mkwvk {

struct Texture;

namespace ngxfg {

using Vendor = fgengine::Vendor;
constexpr Vendor VENDOR_AUTO = Vendor::Auto, VENDOR_NVIDIA = Vendor::Nvidia, VENDOR_AMD = Vendor::Amd, VENDOR_TRIFORCE = Vendor::Triforce;
/// RX_VK_FRAMEGEN_VENDOR, read once; *set: it names a vendor (the Frame generation engine row is then fixed). Unset, empty or unknown: Auto, not set
Vendor env_vendor(bool* set = nullptr);
bool vendor_env_set();
/// Who may make the frames now: RX_VK_FRAMEGEN_VENDOR when it names one, else the Frame generation engine row (opts.fg_engine), live: a
/// change of the row is taken at the next present (framegen_engine_tick)
Vendor vendor();
/// DLSS Frame Generation can run on this GPU, whatever the vendor says (NGX's answers: an RTX 40 or 50, its runtime). The row's NVIDIA asks it
bool dlssg_capable();
/// DLSS Frame Generation may be the engine at all now: the vendor is auto or nvidia (amd and triforce never ask NGX for it)
bool dlssg_allowed();
/// The same for RX_VK_FRAMEGEN_VENDOR alone: what NGX's start reads (the capabilities and extensions are asked once, whichever the row says
/// then, so a row changed in play finds them)
bool dlssg_env_allowed();

/// Whether this build has DLSS-G in it (HOOKSHOT_NGX)
bool built();
/// How many frames DLSS-G can make between two of the game's on this GPU (NGX's DLSSG.MultiFrameCountMax, clamped as NX2navi
/// clamps it): 0 when it cannot run here (not an RTX 40 or 50, no libnvidia-ngx-dlssg runtime, NGX not started, the vendor
/// is triforce). Starts NGX when nothing did, so only from the device's threads once it exists
int max_generated();
/// Whether a Frame generation row value (opts.framegen: 1 Auto, 2 to 8 times) is made by DLSS-G: it can run here, the value is
/// within what it makes (2 to max_generated() + 1; Auto always, up to that many a frame), and the picture is SDR (DLSS-G here is
/// SDR, as in NX2navi). The menu's hint and the scene's inputs ask this; framegen_record asks it every present
bool wanted_for(int framegen);
/// The scene's camera, depth and motion are kept for DLSS-G (vk_fsr3_inputs.cpp wanted): the row is on and wanted_for it. Cheap:
/// asked at every scene draw
bool inputs_wanted();

/// What DLSS-G is given of a frame besides the picture (vk_fsr3_inputs.h made them, vk_framegen.cpp hands them over)
struct Inputs {
    VkImage color_image = VK_NULL_HANDLE;   ///< the game's picture with its HUD, RGBA8 UNORM, GENERAL: the backbuffer DLSS-G reads
    VkImageView color = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;         ///< its size (the cropped window picture)
    VkImage depth_image = VK_NULL_HANDLE;   ///< R32F, 1 at the near plane
    VkImageView depth = VK_NULL_HANDLE;
    VkImage motion_image = VK_NULL_HANDLE;  ///< RG16F, render pixels toward the frame before, x right and y down, no jitter
    VkImageView motion = VK_NULL_HANDLE;
    uint32_t render_width = 0, render_height = 0;   ///< the depth's and the motion's size
    float jitter[2]{};                      ///< render pixels, x right and y down
    dlssg_math::Camera camera;
    bool reset = false;                     ///< no history: a new size, a gap, a camera cut, the first frame
};
/// One made frame's image: the output DLSS-G writes (RGBA8 UNORM, GENERAL, the picture's size)
struct Output {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

/// Records DLSS-G's `count` made frames between the last present's picture and this one's into out[0..count) (frame i at
/// i + 1 of count + 1 of the way), into cmd, GENERAL images. The picture without the HUD goes with it when this frame has one
/// (hudless_note). False when it could not (no feature, an error: the caller makes the frames itself, and the next call starts
/// a new history). count over max_generated() is not taken
bool evaluate(VkCommandBuffer cmd, const Inputs& in, const Output* out, int count);
/// The next evaluation starts a new history (a present DLSS-G did not see: the renderer's own made the frames, a cut, a
/// loading hitch, a resize)
void reset();
/// The feature, its parameters and the images let go (the device idle)
void release();
/// Each present: what DLSS-G holds in video memory let go once it has not been used for a while (the row turned off, another
/// multiplier, no scene drawn), as vk_temporal.cpp's temporal_idle does for the upscalers
void idle();

} // namespace ngxfg
} // namespace mkwvk
