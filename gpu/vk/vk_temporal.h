// A temporal upscaler in place of OoT's gameplay upscale network (vk_temporal.cpp): the frame chain any such upscaler
// needs, and the engines that draw with it. The chain finds the scene's camera and depth, makes the inputs at the render
// size (the colour as the network samples it, the depth, motion vectors with the skinned draws' own), keeps the camera
// jitter's state for the host (nx2/oot_jitter.py) and writes the engine's picture into the network's output. An engine
// (NVIDIA DLSS: vk_dlss.cpp; AMD FSR 4: vk_fsr4.cpp) only turns those inputs into the picture.
#pragma once

#include <cstdint>
#include <functional>

#include "vk_internal.h"

namespace mkwvk {

struct Texture;

/// What the Upscaler row runs now on this chain (upscaler_rows.h: GAME, DLSS, DLAA, FSR4, FSR4_AA): the row's choice
/// (opts.dlss), or RX_VK_DLSS=1 / 2 or RX_VK_FSR4=1 / 2 over it; GAME when the chosen engine cannot run here, and while
/// AMD FSR 3.1 (vk_fsr3.cpp, its own inputs) upscales: RX_VK_FSR=1, or the row's FSR 3.1 with neither switch above set.
/// (Triforce, FSR4-1) GAME too while the chosen engine is off after failing again and again: a creation or an evaluation
/// that fails is tried again 2 s later, and the fourth failure in a row turns it off until the Upscaler row is set again
int temporal_mode();
/// (Triforce, FSR4-1) The engine chosen (DLSS, DLAA, FSR4, FSR4_AA) when it is off after failures and the game's upscale
/// stands in for it (the Upscaler row's help says so); GAME when nothing stands in
int temporal_stood_in();
/// RX_VK_DLSS=1 / 2 or RX_VK_FSR4=1 / 2 set for this run, and that engine runs here: over the row, its FSR 3.1 too
/// (vk_fsr3.cpp fsr3_wanted). Where it cannot run, a saved FSR 3.1 stays
bool temporal_env_chosen();
bool temporal_wanted();       // (temporal_mode() != GAME)
/// DLAA or FSR's native anti-aliasing (FSR 4's on this chain, FSR 3.1's with its own passes: vk_fsr3.h fsr3_native): the 3D
/// drawn at full scale, whatever the Render resolution row says
bool temporal_full_scale();
/// ((Triforce, FSR3-06, FSR4-5) The engine (or AMD FSR 3.1) draws before the game's bloom and tone mapping this run (the
/// Upscale before bloom row at the start, RX_VK_DLSS_PRE over it): it takes the scene, which the game's SMAA has not been over
bool temporal_before_bloom();

/// ((Triforce, S1) The Sharpness row as the upscalers' own sharpening, 0 to 1 (sharpen_rules.h amount); 0 at 0, and with
/// RX_VK_SHARPEN_3D=0 (the row then sharpens the finished picture at the window for every upscaler, as before)
float temporal_sharpness();
/// ((Triforce, S1) Whether what upscales now sharpens the 3D before the HUD at the row's value: AMD FSR 3.1 (its own RCAS),
/// AMD FSR 4 (its own sharpening), NVIDIA DLSS and DLAA (AMD's RCAS over their picture, vk_fsr3.h fsr3::rcas, only where it is
/// tone mapped: not before the bloom nor RenoDX's RGBA16F scene, sharpen_rules.h lone_rcas). The window pass then sharpens
/// nothing (vk_framegen.cpp framegen_scale): never twice, and not the HUD. The game's own upscaler: false
bool temporal_sharpens_3d();

/// A draw into a scene-sized target (colour target 0 and the depth target), with the vertex stage's uniform buffer 3 at
/// `camera` (its size): the camera of the frame, when that buffer holds one
void temporal_note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,
                              uint64_t camera_size);
/// Whether a target of width x height can be the scene the gameplay upscale takes (1 to 3 times smaller than its output, as
/// last seen; dlss_checks.h scene_size_fits): the camera, the actor motion and RX_VK_DLSS_TRACE take only those.
/// ((Triforce, FSR3-03) while AMD FSR 3.1 upscales, also the sizes its own inputs take as the scene, vk_fsr3_inputs.h
/// scene_size: its upscale's output is noted there, and the draws of its scene are to store their positions)
bool temporal_scene_size(uint32_t width, uint32_t height);
/// A skinned draw into a scene-sized target (vk_draw.cpp finds them by their vertex layout): what the motion needs to draw
/// it again with this frame's bones and the frame before's (actor motion: Link, the others, what moves by itself)
struct SkinnedDraw {
    const Texture* depth = nullptr;   // (its depth target: only the scene's are taken)
    uint64_t vbuf = 0, vsize = 0;     // (vertex stream 0)
    uint32_t stride = 0;
    uint32_t pos = 0;                 // (byte offsets in a vertex: the position, RGB32F or RGBA16F)
    uint32_t joints[2]{}, weights[2]{};   // (RGBA16UI or RGBA8UI joints and RGBA8 weights, four a set; sets: 1 or 2)
    int sets = 0;
    uint32_t formats = 0;             // (dlss_actor::Push::formats)
    uint64_t ibuf = 0;
    int itype = 0, count = 0, base_vertex = 0;
    uint64_t bones = 0, bones_size = 0;   // (the palette: 3 x 4 row-major matrices, model space to the camera's world)
    uint64_t object = 0, object_size = 0; // (the object's buffer: its origin at 0x40)
    uint64_t camera = 0, camera_size = 0; // (the camera it was drawn with, the vertex stage's buffer 3: its view at 0, projection
                                          // at 0x70. Characters need not be drawn with the scene's: theirs is used for them)
    uint64_t program = 0;
    uint64_t stream_pos = 0;   // (Triforce: its place in the stream, vk_replay.cpp replay_stream_pos: the n-th of a model is named by it)
};
/// nullptr: taken; else why not (vk_draw.cpp's RX_VK_DLSS_TRACE says it)
const char* temporal_note_skinned_draw(const SkinnedDraw& k);

/// A draw into a scene-sized target whose vertex stage stores its positions (vk_shader.cpp's capture_position): what the
/// motion needs to draw it again from the positions it stored this frame and the frame before (vk_temporal.cpp
/// capture_motion: everything the game moves in its vertex shaders, bones or not)
struct CaptureDraw {
    const Texture* depth = nullptr;   // (its depth target: only the scene's are drawn again)
    uint64_t vbuf = 0;                // (vertex stream 0's address: the draw's name across frames, with the rest)
    uint64_t ibuf = 0;                // (its indices; itype dlss_actor::kNoIndices: none, vertex i is base_vertex + i)
    int itype = 0, count = 0, base_vertex = 0;
    uint64_t program = 0;
    uint64_t camera = 0, camera_size = 0;   // (the vertex stage's uniform buffer 3: the camera it is drawn with, its jitter)
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool restart = false;             // (primitive restart, as the draw has it)
    uint64_t stream_pos = 0;          // (Triforce: its place in the stream, as SkinnedDraw's)
};
/// Where such a draw's vertex stage stores its positions: CAPTURE_PUSH_OFFSET's push constants (address 0: nowhere)
struct CaptureSlot {
    uint64_t address = 0;
    uint32_t first = 0, count = 0;   // (the vertex index of the first slot, the slots)
};
/// The slot for a draw about to be recorded, when the chain wants its positions (*out left zero when not); the draw must be
/// recorded after it, as it is described
void temporal_capture_draw(const CaptureDraw& k, CaptureSlot* out);
/// Whether draws are to store their positions at all now (an engine wanted, or AMD FSR 3.1 upscaling: temporal_motion_wanted;
/// RX_VK_DLSS_ACTORS not 0 or bones): a draw that is not asks nothing more
bool temporal_capture_wanted();
/// ((Triforce, FSR3-03) Whether what moves by itself is to get its own motion now: an engine of this chain draws, or AMD
/// FSR 3.1 upscales (vk_fsr3.cpp fsr3_wanted), whose inputs the chain's redraws draw into (temporal_fsr3_motion). The draws
/// store their positions, and the skinned ones are noted, only then
bool temporal_motion_wanted();

namespace fsr3_inputs {
struct Frame;
}
/// ((Triforce, FSR3-03) AMD FSR 3.1's upscale in place of the upscale's dispatch (vk_fsr3.cpp fsr3_upscale), its inputs just
/// made (f): this frame's draws that stored their positions (or the skinned ones, RX_VK_DLSS_ACTORS=bones) drawn again into
/// f's motion over the camera's, as the chain does for its engines, from f's camera (fsr3_inputs::Frame::own). Takes this
/// frame's draws, as an engine's upscale does: f null when FSR 3.1 was to draw and its inputs were not made, the draws only
/// let go. Returns the draws drawn again. Before the bloom the chain does the same itself (temporal_pre_tonemap)
int temporal_fsr3_motion(VkCommandBuffer cmd, const fsr3_inputs::Frame* f);

/// (local, RenoDX port) Whether f is the scene's colour format: the game's B10G11R11, or the RGBA16F the host chose for the
/// run once an upscale's input was RGBA16F
bool temporal_scene_format(VkFormat f);

/// A gameplay upscale's dispatch (program `key`; vk_draw.cpp's temporal_stand_in finds them: the 4/3 network fd029a317a9a4b94
/// at the game's usual 3/4, the same-size pass 9dc98871eb7fc101 at full size, and one network of its own for each other
/// share): its input as it samples it (in, in_view) and its output's storage view (out at level 0). known: one of the
/// gameplay networks by its key; else a network of that shape (the fixed shares' own), taken only when its input is this
/// frame's scene and its output the picture, and left to run without a word otherwise. true: the engine wrote the output
/// (the dispatch is left out); false: the network runs, the reason logged
bool temporal_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view, Texture* out,
                      VkImageView out_view, uint32_t out_level);

/// NVIDIA DLAA before the game's bloom and tone mapping (RX_VK_DLSS_PRE=0: not). OoT's frame after the scene's last draw:
/// the bloom's first pass samples the scene's colour target, the bloom's pyramid, one draw that adds the bloom and tone
/// maps into another target, and the upscale's dispatch. Each draw (vk_draw.cpp) is asked here with its colour target 0
/// and its program: true at the first draw after the scene's last (found by its program, as the frame before had it),
/// where temporal_pre_tonemap is then to run, outside a pass. *late: a draw into the scene after the engine drew there, in
/// a frame worth a dump (the first few such frames)
bool temporal_pre_point(const Texture* color0, uint64_t program, bool* late = nullptr);
/// (Triforce) The same, asked by the parallel replay's dry pass in stream order for each draw (vk_draw.cpp
/// dry_temporal_draw), so that the segments' threads need not ask it: program(ctx) gives the draw's program, asked only when
/// the answer depends on it (stream_order.h pre_point_step). True at the point: the dry pass's segments end there and the
/// rest is recorded in order, where temporal_pre_point says true again for the same draw
bool temporal_pre_point_dry(const Texture* color0, uint64_t (*program)(void* ctx), void* ctx);
/// (Triforce) Whether the engine drew before the bloom and the upscale's dispatch is still to come (or a frame before left it
/// so): a command buffer that starts so is recorded on the replay thread alone (vk_replay.cpp replay_handle)
bool temporal_pre_pending();
/// The engine's frame there: the scene's linear HDR colour, the camera's depth and motion in, its picture back into the
/// scene's colour target (returned: it was written), so the bloom and the tone mapping work from the anti-aliased scene.
/// The upscale's dispatch of that frame then only copies (temporal_upscale). nullptr: nothing drawn, the dispatch draws
Texture* temporal_pre_tonemap(VkCommandBuffer cmd);
/// After it, each draw with one colour target (its size): true at the game's tone mapping draw (the first into a target
/// of the scene's size and format that is not the scene: it samples the scene and the bloom) when the engine scaled up.
/// The draw is then to be recorded once more before its own: into k's target at its size, sampling k->to_view (the
/// engine's picture) wherever it samples k->from (the scene), so the tone mapped picture is made at the output's size.
/// Its own recording still follows, for what the game reads of its target (its edge anti-aliasing, its light meter).
/// false: nothing more to do (DLAA: the draw's own target is the picture, taken by the upscale's dispatch)
struct TemporalRetarget {
    Texture* target = nullptr;
    VkImageView target_view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    const Texture* from = nullptr;
    VkImageView to_view = VK_NULL_HANDLE;
};
bool temporal_pre_composite(const Texture* color0, uint32_t width, uint32_t height, TemporalRetarget* k);
/// The upscale's dispatch as AMD FSR 3.1 has it (vk_fsr3.cpp fsr3_upscale; the chain's own engines are asked inside
/// temporal_upscale): true when the output is written, because the picture was drawn before the bloom this frame. In a
/// frame it was not, the point is learnt for the next (learn: this dispatch is known to be the scene's upscale), and false
bool temporal_pre_dispatch(VkCommandBuffer cmd, Texture* in, VkImageView in_view, Texture* out, VkImageView out_view,
                           uint32_t out_level, bool learn);
/// Before that recording, outside a pass: the view it is to sample in place of k->to_view. The engine's picture, or that
/// with what the game drew into the scene after the engine's pass added (OoT's lens flare is drawn there after the
/// bloom's passes: shaders/dlss_merge.comp), which this records
VkImageView temporal_pre_merged(VkCommandBuffer cmd);
/// Whether that recording drew (not when its pipeline was not ready): the upscale's dispatch then has nothing to copy
void temporal_pre_composited(bool drawn);

/// Once a frame (the game's present): after 120 frames without an upscale by an engine (off, or the upscale not taken) the
/// engine's own state, the chain's images and deferred frees are let go, so their video memory comes back. Nothing to do
/// costs one atomic load
void temporal_idle();

/// For the renderer's status line (vk_replay.cpp status_tick): while an engine draws, its details since the last call into b
/// ("; DLSS: jitter ..., actor motion ..., its images ... MB") and temporal_mode(); else 0 and b left alone
int temporal_status(char* b, size_t n);

/// The scene's width (axis 0) or height (1) for the host's camera jitter (nx2/oot_jitter.py, HOS_OPT_DLSS_JITTER_W and _H):
/// 0 when there is to be none (no engine drawing, or RX_VK_DLSS_JITTER=0). Axis 2: the jitter sequence's length for the
/// engine's scale (dlss_jitter_phases, HOS_OPT_DLSS_JITTER_PHASES)
uint32_t temporal_jitter_size(int axis);

// ---- what an engine is given and does ----

/// One of the chain's images: GENERAL before and after an engine's evaluation
struct TemporalImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};
/// One frame's evaluation. color: the scene as the network samples it (tonemapped, square-root encoded: LDR, RGBA16F);
/// depth: R32F in [0, 1], not reversed; motion: RG16F in render pixels, towards the frame before, the jitter taken off. They
/// are as large as the engine asked (in_w x in_h), the frame's part at the top left. output: RGBA16F, at the output's size
/// (DLAA, FSR's native AA: the render size)
struct TemporalFrame {
    TemporalImage color, depth, motion, output;
    uint32_t render_w = 0, render_h = 0;
    float jitter[2]{};          // (the scene's sub-pixel offset this frame, in render pixels, x right and y down)
    bool reset = false;         // (no history: the first frame, or one after a gap or a camera cut the chain saw)
    float near_z = 0.1f, far_z = 1000.0f, fov_y = 1.0f;   // (the scene's projection's, when it is a plain perspective one)
};
/// An engine: plain functions, each called with the chain's lock held, on the recording thread
struct TemporalEngine {
    const char* name;           // (for the log and the status line: "NVIDIA DLSS", "AMD FSR 4")
    const char* short_name;     // ("DLSS", "FSR")
    /// Once, before its first upscale: false when it cannot run here (it says why in the log)
    bool (*start)();
    /// Ready to scale rw x rh into ow x oh, made again when its sizes or settings changed (cmd: what that records). in_w and
    /// in_h: how large the inputs are to be (the most the render size can be for what it made). 1 ready, 2 ready and made
    /// just now (a new history), 0 not this frame (*why: the reason), -1 failed (*why too: (Triforce, FSR4-1) tried again 2 s
    /// later, off after four in a row until the Upscaler row is set again, vk_temporal.cpp fail_again). hdr: the colour holds
    /// linear HDR values (an RGBA16F scene), not the LDR of TemporalFrame's color
    int (*prepare)(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool hdr, uint32_t* in_w,
                   uint32_t* in_h, const char** why);
    /// Records the frame: false when it failed (the game's upscale for that frame, and as after a failed prepare)
    bool (*evaluate)(VkCommandBuffer cmd, const TemporalFrame& f);
    /// What it made, let go (its feature or context): the chain unused for a while, or another engine chosen. Work in flight
    /// may still use it, so what it frees goes through later
    void (*release)();
    /// Its own video memory for the status line, in bytes (0: not known)
    uint64_t (*memory)();
};
/// Runs f a few frames on, to free what work in flight may still use. Called by the engines with the chain's lock held
void temporal_later(std::function<void()> f);

} // namespace mkwvk
