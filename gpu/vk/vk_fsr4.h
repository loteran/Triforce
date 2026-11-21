// AMD FSR 4, one of the temporal upscaler's engines (vk_temporal.h; vk_fsr4.cpp), through the AMD FSR API. Only the API's MIT
// headers are built in (third_party/fidelityfx), and only in a build with HOOKSHOT_FSR (make_game.py --fsr, which also puts
// the camera jitter into the game: nx2/oot_jitter.py). AMD's signed amd_fidelityfx_loader_vk.dll and
// amd_fidelityfx_upscaler_vk.dll are loaded from the user's PC at run time; none ship with Hookshot. Linux: a Vulkan FSR 4
// library (such as fsr4-vk's amd_fidelityfx_upscaler_vk.so) in their place.
// (Triforce) And AMD FSR 4's frame generation, the Frame generation row's last choice (fsr4fg, below): the same loader with
// amd_fidelityfx_framegeneration_vk.dll beside it. Windows only: AMD publishes no library for it elsewhere.
#pragma once

#include <cstdint>

#include "framegen_rows.h"
#include "vk_loader.h"

namespace mkwvk {

struct TemporalEngine;

/// Whether this build has FSR in it (HOOKSHOT_FSR)
bool fsr4_built();
/// Why FSR 4 cannot be chosen here (the Upscaler row says it), FSR_OK when it can or is not known yet
enum FsrWhy { FSR_OK, FSR_NOT_BUILT, FSR_ENV_OFF, FSR_NO_LIBRARY, FSR_NO_MODEL, FSR_WHY_COUNT };
FsrWhy fsr4_why();
/// (Triforce, FSR4-7) The Upscaler row's name for FSR 4 (native_aa: its Native AA), by the version AMD's library runs here
/// once it has been asked: "AMD FSR 4.1", "AMD FSR 4.1 Native AA"; "AMD FSR 4" before. Never waits for the library
const char* fsr4_name(bool native_aa);
/// Loads AMD's library once and asks it for an FSR 4 upscaler for this device, so fsr4_why knows before the first upscale
/// (the menu, when it opens). Nothing in a build without FSR, or before the device is made
void fsr4_probe();
/// (linux) a Vulkan FSR 4 library is there and records with descriptor buffers: the device has to enable them
bool fsr4_wants_descriptor_buffer();
/// FSR 4 as the temporal upscaler's engine (FSR 4 and its native AA on the Upscaler row); nullptr in a build without it
const TemporalEngine* fsr4_engine();

/// (Triforce) AMD FSR 4's frame generation: the made frame of the Frame generation row's AMD FSR 4 (framegen_rows.h FSR4),
/// by AMD's library from the two pictures and the scene's depth, motion and camera (vk_fsr3_inputs.h gathers them). The
/// renderer keeps its own swapchain, presents and pacing: AMD's frame generation context is asked for the one frame in
/// the present's recording (vk_framegen.cpp framegen_record) and the display thread shows it, as it shows the renderer's
/// own made frames. Every function answers "not here" in a build without HOOKSHOT_FSR and outside Windows.
namespace fsr4fg {

/// One present's frame, everything in GENERAL
struct Inputs {
    VkImage color = VK_NULL_HANDLE;   ///< the picture the game has just finished, as it is presented: R8G8B8A8_UNORM, with the HUD
    /// ((Triforce, FG-2) the same picture without the HUD: the gameplay upscale's output, encoded as the game's last pass
    /// encodes it (sRGB, 8 bits) and cut as the picture is (vk_framegen.cpp hudless_for). Null: no frame is made (GAME)
    VkImage hudless = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    bool pq = false;                  ///< its bytes are HDR10 codes (HDR Native), not sRGB: the renderer's own makes the frame (OWN)
    float peak_nits = 1000.0f;        ///< the HDR peak, for a PQ picture
    VkImage depth = VK_NULL_HANDLE;   ///< R32F, reversed (1 at the near plane), render_width x render_height
    VkImage motion = VK_NULL_HANDLE;  ///< RG16F, render pixels toward the frame before, x right and y down, no jitter
    uint32_t render_width = 0, render_height = 0;
    float jitter[2]{};                ///< render pixels
    float near_plane = 0.1f, far_plane = 1e8f, fov_y = 1.0f;   ///< far_plane: FLT_MAX for a far plane at infinity
    float view[12]{};                 ///< the view matrix's three rows (framegen_rows.h pose_of)
    float frame_ms = 16.667f;         ///< since the frame before
    bool reset = false;               ///< no frame before this one to make a frame from (the first, a gap, a new size)
    uint64_t sequence = 0;            ///< the caller's count of its presents: a frame that does not follow the last one given is a reset
};

/// Why AMD's library cannot make the frames here, FG_OK when it can. The first call after the device is made asks AMD's
/// library (the loader and amd_fidelityfx_framegeneration_vk.dll beside the game, and whether they list a frame generation
/// for this card and driver: ((Triforce, FG-1) FSR 4's where listed, else FSR 3.1's, framegen_rows.h pick_version);
/// RX_VK_FSR4_FG=0: never, =4: FSR 4's alone (as before), 1 or unset: the newest listed
framegen_rows::Why why();
inline bool offered() { return why() == framegen_rows::FG_OK; }
/// (Triforce) why() as it is known now, never asking AMD's library (nor waiting for another thread asking it): FG_NOT_YET until
/// it has been asked. The menu's rows read this, so that drawing them never waits for the library (vk_menu.cpp menu_ahead asks
/// it off the menu's thread)
framegen_rows::Why why_known();
inline bool offered_known() { return why_known() == framegen_rows::FG_OK; }
/// (Triforce) What keeps AMD's library from every present while it lasts (framegen_rows.h held_whole_run): HELD_HDR_NATIVE
/// while the game's picture is HDR Native's PQ codes (vk_replay.cpp present_native_pq), HELD_HDR_SCENE in a run whose scene
/// is RenoDX's RGBA16F (vk_temporal.cpp temporal_scene_format); HELD_NONE otherwise. In every build, never asks AMD's library
framegen_rows::Held held();
/// Whether a made frame is asked of it now: the row at AMD FSR 4, where it can, not beside NVIDIA DLSS frame generation,
/// and nothing held for the whole run (held(): the renderer's own makes the frame then, and no input is made for AMD's).
/// Read every frame; costs nothing while the row is elsewhere
bool frames_wanted();
/// The version AMD's library makes the frames with ("4.0.2"), "" before it is known
const char* version();

/// Hands AMD's library this present's frame and records its work into cmd. MADE: the made frame is in made_image(). SKIP: it
/// was given the frame as a new history and no frame is to be made. OWN: it did not take the present (the renderer's own
/// interpolation makes the frame; HDR Native's picture too). GAME ((Triforce, FG-2)): not given, no picture without the HUD
/// this frame: the game's frame alone. Each way it refuses or fails is logged, the first time
framegen_rows::Made generate(VkCommandBuffer cmd, const Inputs& in);
/// The made frame of the last generate that answered MADE: R8G8B8A8_UNORM at the picture's size, GENERAL; else null
VkImage made_image();

/// Once a present, on the thread that calls generate, outside its recording: a context that failed, or that the row has left
/// for a while, is taken down (framegen_rows.h release_due and teardown)
void present_tick();
/// The process is about to exit (HOS_OPT_EXITING): no call into AMD's library after this returns
void exiting();

} // namespace fsr4fg

} // namespace mkwvk
