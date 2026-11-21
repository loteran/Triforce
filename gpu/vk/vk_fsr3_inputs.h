// (hookshot) AMD FSR 3.1's inputs (vk_fsr3_inputs.cpp): everything FSR 3.1's upscaler (vk_fsr3.h) and its frame
// interpolation (vk_fsr3fi.h) are given besides the picture, in one place, so they can move onto the renderer's shared
// temporal inputs (#1008) by changing this file alone.
//
// What comes from #841 (NVIDIA DLSS's own inputs, now vk_temporal.cpp: the frame chain DLSS and FSR 4 share) and what does
// not yet:
//  - the camera jitter: #841's, the one jitter in the game (nx2/oot_jitter.py, host/rx_backend.c hos_dlss_jitter_clip),
//    switched on for FSR by jitter_size() as vk_temporal.cpp's temporal_jitter_size switches it on for DLSS and FSR 4, and
//    read back here from the camera's uniform buffer as vk_temporal.cpp reads it;
//  - the camera: the same uniform buffer and the same checks as vk_temporal.cpp's temporal_note_scene_draw; the near plane and the
//    field of view are read from its projection (fsr3_math.h), so nothing in the game is hooked for them;
//  - the motion: the camera's, from the scene depth with dlss_prep.comp's math (shaders/fsr3_prep.comp); then, while FSR 3.1
//    upscales, ((Triforce) FSR3-03) what moves by itself drawn over it with its own motion by the chain's redraws
//    (vk_temporal.cpp temporal_fsr3_motion: every plain draw from the positions its vertex stage stored, or the skinned
//    draws from their bones), into this file's motion image, from the camera in Frame::own. Its frame interpolation and
//    the other frame generations that take this motion get it with it.
#pragma once

#include <cstdint>

#include "dlssg_math.h"
#include "fsr3_math.h"
#include "vk_internal.h"

namespace mkwvk {

struct Texture;

namespace fsr3_inputs {

/// A frame's inputs, at the render size, in images of this file's own (written by prepare)
struct Frame {
    VkImageView color = VK_NULL_HANDLE;    ///< RGBA16F: the network's input as it samples it, 0..1
    VkImageView depth = VK_NULL_HANDLE;    ///< R32F, reversed (fsr3_math.h Camera::flip_depth)
    /// RG16F, render pixels toward the frame before, x right and y down, no jitter. ((Triforce) a colour attachment too: the
    /// chain's redraws of what moves by itself draw into it, vk_temporal.cpp temporal_fsr3_motion)
    VkImageView motion = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    float jitter[2]{};                      ///< this frame's, in render pixels as FSR takes it (x right, y down)
    fsr3_math::Camera camera;
    /// No history: the first frame, a gap of more than two frames, the frame after a present that held its frame, or a camera
    /// cut (framegen_rows.h kUpscalerCut). Not another render size: the game's dynamic resolution keeps FSR's history
    /// The whole camera for NVIDIA DLSS Frame Generation (dlssg_math.h: matrices without the jitter, position and basis), made
    /// only while it is wanted (vk_ngxfg.h inputs_wanted); `dlssg_ok` when the scene's view and projection made one
    dlssg_math::Camera dlssg;
    bool dlssg_ok = false;
    bool cut = false;                       ///< the camera jumped (a teleport, a cut scene's shot, an area): reset DLSS-G's history
    bool reset = false;
    float frame_ms = 16.667f;               ///< since the frame before's
    uint64_t frame = 0;                     ///< texture_frame_no() when prepared
    // (Triforce, for NVIDIA DLSS frame generation (vk_dlssg.cpp, and NGX's vk_ngxfg.cpp here): the depth's and the motion's images
    // themselves, and the camera's matrices as the game has them, row major, clip = M * position, without the jitter)
    VkImage depth_image = VK_NULL_HANDLE, motion_image = VK_NULL_HANDLE;
    float projection[16]{};                 ///< view space to clip space
    float clip_to_prev[16]{};               ///< this frame's clip space to the frame before's; the identity on a reset
    float view[12]{};                       ///< the view matrix's three rows: the camera's axes and its translation
    /// ((Triforce, FSR3-03) what the chain's redraws of what moves by itself take, vk_temporal.cpp temporal_fsr3_motion: the
    /// scene's camera as the game drew with it, the jitter in it, the frame before's, the reprojection fsr3_prep.comp drew the
    /// camera's motion with, and the depth texture the scene was drawn with (the draws that are the scene's are told by it))
    struct Own {
        float current[16]{};      ///< this frame's view-projection, jittered (row major, clip = M * position)
        float projection[16]{};   ///< its projection, jittered
        float jitter[2]{};        ///< its jitter in clip space (nx2/oot_jitter.py)
        float before[16]{};       ///< the frame before's view-projection without its jitter
        float jitter_before[2]{}; ///< the frame before's jitter in clip space
        float reproject[16]{};    ///< this frame's jittered clip space to the frame before's without its jitter
        Texture* depth = nullptr; ///< the scene's depth texture (checked there still when prepared)
        uint64_t last = 0;        ///< the frame these inputs were made before this one (0: none): the history's
        bool history = false;     ///< the motion is from the frame before (no reset): the redraws have something to go by
    } own;
    /// ((Triforce, FG-4) the motion is the upscaler chain's (DLSS, FSR 4), what moves by itself drawn in it: chain_motion
    bool chain_motion = false;
};

/// FSR 3.1 is asked for at all: its upscaler (the Upscaler row) or its frame interpolation (the Frame generation row), or NVIDIA
/// DLSS Frame Generation (vk_ngxfg.h) is given the scene's inputs. The scene's camera is kept only then
bool wanted();

/// A draw into a scene-sized target with the vertex stage's uniform buffer 3 at `camera`: what vk_temporal.cpp's
/// temporal_note_scene_draw is given, read the same way (the largest such target of the frame is the scene)
void note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,
                     uint64_t camera_size);

/// The gameplay upscale's output size, as vk_fsr3.cpp's fsr3_upscale sees its dispatch (scene_size)
void note_output(uint32_t width, uint32_t height);
/// Whether a target of width x height can be the scene: 640x360 or more, or 1 to 3 times smaller than the output as last
/// noted (dlss_checks.h scene_size_fits, vk_temporal.cpp's temporal_scene_size rule)
bool scene_size(uint32_t width, uint32_t height);

/// The texture is this frame's scene colour (the size of the scene's camera this frame): for a network of the gameplay
/// upscale's shape that is not one of the known ones, as vk_temporal.cpp's temporal_upscale tells them apart
bool is_scene(const Texture* in);

/// At the gameplay upscale's dispatch: its input as the network samples it (in, in_view), this frame's camera and depth into
/// FSR's inputs (shaders/fsr3_prep.comp). False when they cannot be made (no camera this frame, another size, ...): the
/// reason is logged when it changes
/// linear: in is the scene's colour target itself, linear HDR light (FSR before the game's bloom and tone mapping)
bool prepare(VkCommandBuffer cmd, Texture* in, VkImageView in_view, Frame& out, bool linear = false);

/// The frame prepare made last, for frame interpolation at the present, once: false when none was made since the last take
/// (or it is older than the frame before), so a present never interpolates with another frame's depth and motion
bool take(Frame& out);

/// ((Triforce, FG-4) prepare made this frame's inputs already (texture_frame_no), not yet taken: the upscaler chain made them
/// before the bloom, and the gameplay upscale's dispatch after it makes none again (vk_fsr3.cpp fsr3_upscale)
bool made_this_frame();
/// ((Triforce, FG-4) While NVIDIA DLSS or AMD FSR 4 upscale (vk_temporal.cpp's chain), frame generation's motion is the
/// chain's own: the camera's with what moves by itself drawn over it (the redraws from the stored positions, or the bones),
/// width x height of motion (RG16F, GENERAL, the same convention as this file's: render pixels toward the frame before, x
/// right and y down, no jitter: shaders/dlss_prep.comp and fsr3_prep.comp share the math), copied over this frame's inputs'
/// motion. Before, frame generation had the camera's alone whatever the upscaler, and Link and what moves by itself were
/// made as if they stood still. False when this frame's inputs are not there or not of that size, or RX_VK_FG_OWN_MOTION=0
bool chain_motion(VkCommandBuffer cmd, VkImage motion, uint32_t width, uint32_t height);

/// FSR 3.1's upscaler drew this frame, rw x rh into dw x dh: the host's jitter follows FSR (jitter_size)
void upscaled(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh);

/// FSR 3.1's upscaler was to draw this frame and did not (vk_fsr3.cpp fail; the inputs' own failures but a frame without the
/// scene's camera): once that went on for more than 3 frames, jitter_size is 0 until it draws again
void missed();

/// The scene's width (axis 0) or height (1) for the host's camera jitter while FSR 3.1's upscaler draws, 0 otherwise, with
/// RX_VK_FSR_JITTER=0, 30 frames after it last drew, or after more than 3 frames it failed to draw (missed); axis 2: the
/// jitter sequence's length (fsr3_math.h jitter_phases, AMD's rule). vk_replay.cpp's rx_option gives the host this or
/// vk_temporal.cpp's temporal_jitter_size, whichever upscaler draws
uint32_t jitter_size(int axis);

/// Once a frame (vk_fsr3.cpp fsr3_idle): the colour, depth and motion images and the depth views let go once nothing has
/// asked for these inputs (wanted) for 120 frames, and what was let go earlier destroyed when no frame in flight can use it
void release_idle();

} // namespace fsr3_inputs
} // namespace mkwvk
