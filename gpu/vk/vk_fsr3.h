// (hookshot) AMD FidelityFX Super Resolution 3.1 (the FSR 3.1.4 upscaler of FidelityFX SDK v1.1.4) in place of OoT's
// gameplay upscale network, the same place NVIDIA DLSS takes (vk_dlss.h). Built from AMD's open shader sources
// (third_party/fidelityfx-sdk, compiled into gpu/vk/shaders by scripts/fsr3_shaders.py): every pass is a dispatch of ours,
// and nothing of AMD's is linked or loaded at run time, so it is in every build and runs on any GPU with what supported()
// asks for.
//
// The passes (namespace fsr3) take colour, depth and motion at the render size and write the picture at the output size.
// Those inputs are made in vk_fsr3_inputs.cpp; fsr3_upscale below is the dispatch's side: the network's input in, FSR's
// picture squared back into the network's output, as vk_dlss.cpp's dlss_out.comp does for DLSS.
//
// RX_VK_FSR=0: never (the row stays), =1: FSR 3.1 over the Insert menu's Upscaler row, =2: its Native AA over the row.
//
// Native AA (the row's AMD FSR 3.1 Native AA, upscaler_rows.h FSR3_AA): the same passes with the 3D drawn at the
// output's full size (vk_temporal.h temporal_full_scale tells the game so, as for DLAA and FSR 4's native AA), FSR's
// own anti-aliasing mode at a scale of 1. The game's dispatch is then its full-size pass, the render size the
// output's; nothing else differs.
#pragma once

#include <cstdint>
#include <functional>

#include "fsr3_math.h"
#include "vk_internal.h"

namespace mkwvk {

struct Texture;
namespace fsr3_inputs {
struct Frame;   // (vk_fsr3_inputs.h)
}

namespace fsr3 {

/// One evaluation's inputs, in the SDK's meanings: the colour at the render size, the depth reversed, the motion toward the
/// frame before in render pixels without the jitter, and the jitter in render pixels (input texel k is sampled at
/// k + 0.5 - jitter)
struct Inputs {
    VkImageView color = VK_NULL_HANDLE;
    VkImageView depth = VK_NULL_HANDLE;
    VkImageView motion = VK_NULL_HANDLE;
    uint32_t render_width = 0, render_height = 0;
    uint32_t upscale_width = 0, upscale_height = 0;
    float jitter[2]{};
    uint32_t jitter_phases = 8;   ///< the length of the jitter's sequence (fsr3_math.h jitter_phases: the host's own)
    /// The game's dynamic resolution is on (the Render resolution row's Auto): the render size changes from frame to frame,
    /// and the internal images are made for the output's size at once (fsr3_math.h plan_images)
    bool dynamic = false;
    /// Turns a motion vector into render pixels (1 for vectors already in them)
    float motion_scale[2]{1.0f, 1.0f};
    fsr3_math::Camera camera;     ///< device_to_view_depth (fsr3_math.h)
    float frame_time_ms = 16.667f;
    bool reset = false;
    /// ((Triforce, S1) FSR's own sharpening, 0 (off) to 1 (the most): accumulate_sharpen and RCAS, as the SDK runs them with
    /// its sharpening on (sharpen_rules.h fsr3_sharpness: the Sharpness row, more below 3/4 of the output's width)
    float sharpness = 0.0f;
};

/// Whether this device can run the passes: push descriptors enough for the widest pass, storage images in every format the
/// SDK's internal images use, the extended storage formats and storage writes without a format (the accumulate and RCAS
/// passes' output). Without subgroup quad operations in compute the two pyramids run as their wave-less variants. Asked
/// once; the reason it cannot is logged once
[[nodiscard]] bool supported();

/// Records one evaluation into output_image(), making the pipelines and the internal images first (again for another
/// output, or a frame larger than they are: a smaller frame of the game's dynamic resolution keeps them and the history).
/// False when something could not be made or the inputs are not usable
[[nodiscard]] bool upscale(VkCommandBuffer cmd, const Inputs& in);

/// The image an evaluation resolves into: RGBA16F at the upscale size, or null before the first
[[nodiscard]] VkImage output_image();
[[nodiscard]] VkImageView output_view();

/// The internal images let go (a few frames on, when no work in flight can read them), the pipelines kept: the next
/// upscale makes them again with a new history. Not while a frame may still use output_view (fsr3_idle decides)
void release();
/// Images held, or let go and not yet destroyed
[[nodiscard]] bool holding();
/// Images let go earlier destroyed, once the frames that may read them are done
void free_unused();
/// texture_frame_no of the last evaluation, 0: none
[[nodiscard]] uint64_t last_used();

/// ((Triforce, S1) FSR's own sharpening can run (its two passes were made, or have not been tried yet): the Sharpness row
/// then sharpens FSR 3.1's 3D before the HUD, and nothing at the window
[[nodiscard]] bool sharpens();
/// ((Triforce, S1) FSR 3.1's RCAS pass alone, over another upscaler's picture before the HUD (NVIDIA DLSS's, which has no
/// sharpening of its own: vk_temporal.cpp): in (sampled) into out (storage, any float format), w x h, at sharpness 0..1
/// (sharpen_rules.h rcas_config). Exposure 1. False when it cannot run here (made once; the reason logged): the caller
/// keeps its picture as it is
[[nodiscard]] bool rcas(VkCommandBuffer cmd, VkImageView in, VkImageView out, uint32_t w, uint32_t h, float sharpness);
/// rcas can run here: this device runs FSR 3.1's passes, and it was made or not tried yet
[[nodiscard]] bool rcas_ok();

} // namespace fsr3

/// Once a frame (vk_temporal.cpp temporal_idle, at the present): FSR 3.1's internal images let go once the Upscaler row has
/// left FSR 3.1 and they were not used for 120 frames (2 s: a row stepped through keeps them), and its inputs' images once
/// nothing asks for them (vk_fsr3_inputs.h release_idle), as vk_temporal.cpp lets the chain's go. Nothing to do costs a
/// few loads. true: FSR 3.1's picture was let go now (the chain forgets the one it kept of it from before the bloom)
bool fsr3_idle();

/// FSR 3.1 upscales: the Insert menu's Upscaler row says FSR 3.1 or its Native AA (upscaler_rows.h FSR3, FSR3_AA) and
/// neither RX_VK_DLSS nor RX_VK_FSR4 is set over it (vk_temporal.h temporal_env_chosen), or RX_VK_FSR=1 or 2; and
/// Triforce's own passes run here, AMD's library (vk_ffx3.h) not drawing it in their place
bool fsr3_wanted();
/// FSR 3.1 asked (the row's, or RX_VK_FSR=1 or 2), whichever draws it: AMD's own library through the temporal chain (vk_ffx3.cpp,
/// when it loads: vk_temporal.cpp's temporal_mode then says FSR3) or Triforce's own passes here (fsr3_wanted)
bool fsr3_requested();
/// ...as its Native AA, the 3D at full size: the row's AMD FSR 3.1 Native AA, or RX_VK_FSR=2 (1: the Render resolution
/// row's mode, whatever the row says), whichever draws it
bool fsr3_native();

/// A gameplay upscale's dispatch, as vk_temporal.h's temporal_upscale takes it (vk_draw.cpp's fsr3_stand_in finds them the same
/// way). With FSR 3.1's frame interpolation on, the dispatch's input and this frame's camera become its inputs here
/// (vk_fsr3_inputs.h) whichever upscaler runs. true: FSR 3.1 wrote the output (the dispatch is left out); false: the network
/// (or DLSS) runs, the reason logged when it changes. (Triforce) In an RGBA16F run (RenoDX's HDR Native) as well: the
/// network's RGBA16F input, the square root of HDR colour, is FSR's colour unclamped, and its picture goes into the RGBA16F
/// output squared (dlss_out.comp's -DHDR layout)
/// FSR 3.1 before the game's bloom and tone mapping (vk_temporal.h temporal_pre_tonemap, which calls this at the bloom's
/// first pass): its inputs from the scene's colour target (linear HDR light) and its passes, at ow x oh. *picture: its
/// picture, linear HDR at that size, which the chain writes back into the scene and the tone mapping draws from. Not
/// with FSR's frame interpolation on (fsr3_pre_wanted), whose inputs are made at the upscale's dispatch.
/// ((Triforce, FSR3-03) own_motion: called once its inputs are made, before its passes, with the chain's lock held: the
/// chain draws what moves by itself into their motion, from this frame's draws it took; it returns the draws drawn again)
bool fsr3_pre_wanted();
bool fsr3_pre(VkCommandBuffer cmd, Texture* scene, VkImageView scene_view, uint32_t ow, uint32_t oh, VkImageView* picture,
              const std::function<int(const fsr3_inputs::Frame&)>& own_motion);

bool fsr3_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view, Texture* out,
                  VkImageView out_view, uint32_t out_level);

} // namespace mkwvk
