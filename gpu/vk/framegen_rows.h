/* (Triforce) The Insert menu's Frame generation row (vk_menu.cpp, opts.framegen) with its last choice, AMD FSR 4, and what
   AMD FSR 4's frame generation (vk_fsr4.cpp, fsr4fg) decides without a GPU: where the choice is offered, which of the
   versions AMD's library lists is taken, when a frame is a camera cut, when a context still fits and when it is let go,
   and the order it is taken down in. Plain functions, no Vulkan, no menu: tests/test_framegen_fsr4.py checks them. */
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace mkwvk::framegen_rows {

/// opts.framegen (saved as frame_generation): 0 Off, 1 Auto, 2 to 8 the multiplier, as before (an options file from before
/// keeps its meaning), then 9: AMD FSR frame generation, one frame made by AMD's library between each two of the game's
/// ((Triforce, FG-1) whichever version AMD's library has for the card: FSR 4's on an RX 9000, FSR 3.1's elsewhere. The
/// name FSR4 stays: it is the value's, saved in options files as 9)
enum { OFF = 0, AUTO = 1, kMaxMult = 8, FSR4 = 9, kRowMax = FSR4 };

/// ((Triforce, FG-9) RX_VK_FRAMEGEN_MULT=<0..9>, over the options file for a lab run: the row's whole range, 9 AMD's frame
/// generation among it (it ran 2 to 8 alone before, and a 9 became the renderer's own 8x). Not a number: 2, as before
inline int env_row(const char* e) {
    if (!e || !*e) return 2;
    int v = 0;
    if (std::sscanf(e, "%d", &v) != 1) return 2;
    return v < OFF ? (int)OFF : v > kRowMax ? (int)kRowMax : v;
}

/// ((Triforce, FG-1) The row's value 9 by the version AMD's library makes the frames with: "AMD FSR (3.1.6)", "AMD FSR
/// (4.0.2)"; "AMD FSR" before it is known. out holds n bytes
inline const char* row_label(char* out, size_t n, const char* version) {
    if (!out || !n) return "";
    unsigned v[3] = {0, 0, 0};
    const bool known = version && std::sscanf(version, "%u.%u.%u", &v[0], &v[1], &v[2]) == 3;
    if (known) std::snprintf(out, n, "AMD FSR (%u.%u.%u)", v[0], v[1], v[2]);
    else std::snprintf(out, n, "AMD FSR");
    return out;
}

/// Why AMD FSR 4 cannot make the frames here (the row says it); FG_OK when it can. FG_NOT_YET: not asked, the device is not
/// made yet
enum Why { FG_OK, FG_NOT_BUILT, FG_ENV_OFF, FG_NOT_YET, FG_NO_LIBRARY, FG_NO_FILE, FG_NO_MODEL, FG_FAILED, FG_WHY_COUNT };

/// ((Triforce) Why, in words, for the log line a run with the row at AMD FSR gets when AMD's library cannot make its frames)
constexpr const char* why_name(Why w) {
    return w == FG_OK ? "it can" : w == FG_NOT_BUILT ? "not in this build (HOOKSHOT_FSR, Windows)"
         : w == FG_ENV_OFF ? "switched off (RX_VK_FSR4_FG=0 or RX_VK_FSR4=0)" : w == FG_NOT_YET ? "not asked yet (no device)"
         : w == FG_NO_LIBRARY ? "AMD's loader is not beside the game" : w == FG_NO_FILE ? "amd_fidelityfx_framegeneration_vk.dll is not beside it"
         : w == FG_NO_MODEL ? "AMD's library lists no frame generation it can take for this GPU"
         : w == FG_FAILED ? "it failed earlier in this run" : "?";
}

/// The row's last value on this PC: AMD FSR 4 where it is offered, 8x elsewhere (the row as it was)
constexpr int row_hi(bool fsr4_offered) { return fsr4_offered ? (int)FSR4 : (int)kMaxMult; }
/// What the row shows, and what runs, for a value: itself, or 2x for a saved AMD FSR 4 on a PC where it is not offered
/// (one made frame a frame either way, there by the renderer's own interpolation). The saved value stays for a PC where it is
constexpr int shown(int v, bool fsr4_offered) { return v == FSR4 && !fsr4_offered ? 2 : v; }
/// The frames shown for each of the game's: 1 at Off, auto_most with Auto (the display's refresh over 30 fps), the
/// multiplier, and 2 with AMD FSR 4 (its library makes the one frame between two)
constexpr int multiplier(int v, int auto_most) {
    return v == FSR4 ? 2 : v == AUTO ? auto_most : v < 1 ? 1 : v > kMaxMult ? (int)kMaxMult : v;
}
/// Whether AMD's library is asked for the made frame: the row at AMD FSR 4, where it can, and not beside NVIDIA DLSS frame
/// generation, whose frames go in at the present itself (dlssg: vk_dlssg.cpp says it runs in this session)
constexpr bool fsr4_asked(int v, Why why, bool dlssg) { return v == FSR4 && why == FG_OK && !dlssg; }

/// A version as AMD's library names it ("4.0.2"; one the graphics driver brings carries a star, "4.0.2*"): false when the
/// name does not start with three numbers
inline bool parse_version(const char* name, unsigned v[3]) {
    v[0] = v[1] = v[2] = 0;
    return name && std::sscanf(name, "%u.%u.%u", &v[0], &v[1], &v[2]) == 3;
}
/// Of the frame generation versions AMD's library lists for this GPU, the one contexts are made with: the newest, the first
/// of equal ones, so FSR 4's (4.x) wherever it is listed (RX 9000), else FSR 3.1's (3.1.x: RX 6000 and 7000, whose driver
/// lists 3.1.6 alone, NX2navi 5bdf583 on the owner's RX 7900 XT); -1 when it lists none it can read. ((Triforce, FG-1)
/// Before, a list of FSR 3.1 alone gave -1 and the row never offered AMD's frame generation on those cards. The row names
/// the version it runs, row_label.) only_fsr4 (RX_VK_FSR4_FG=4): 4.x or nothing, as it was
inline int pick_version(const char* const* names, int n, bool only_fsr4) {
    int best = -1;
    unsigned bv[3] = {0, 0, 0};
    for (int i = 0; i < n; i++) {
        unsigned v[3];
        if (!parse_version(names[i], v) || (only_fsr4 && v[0] != 4)) continue;
        const bool newer = v[0] != bv[0] ? v[0] > bv[0] : v[1] != bv[1] ? v[1] > bv[1] : v[2] > bv[2];
        if (best < 0 || newer) best = i, bv[0] = v[0], bv[1] = v[1], bv[2] = v[2];
    }
    return best;
}

/// The camera in the world, from the view matrix's three rows as the game has them (row major, view = R * world + t: the
/// rows are its right, up and back axes, the last column the translation). The position is -R^T t
struct Pose {
    float pos[3]{}, right[3]{}, up[3]{}, fwd[3]{};
};
inline Pose pose_of(const float v[12]) {
    Pose p;
    for (int i = 0; i < 3; i++) {
        p.right[i] = v[i], p.up[i] = v[4 + i], p.fwd[i] = -v[8 + i];
        p.pos[i] = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);
    }
    return p;
}
/// What counts as a camera cut: the turn (with a move) in degrees for each 60th of a second, and the least the time between
/// the two frames is scaled to (in 60ths of a second; four at the most)
struct CutRule {
    float turn_degrees;
    float min_scale;
};
/// Frame generation's (vk_fsr4.cpp fsr4fg): NX2navi's first thresholds (6e87c92), 10 degrees, never less than one 60th
constexpr CutRule kFrameGenCut{10.0f, 1.0f};
/// The upscalers' (FSR 3.1: vk_fsr3_inputs.cpp; DLSS and FSR 4: vk_temporal.cpp's chain): NX2navi's 2495391, 30 degrees.
/// A quick swing of the camera around Link turns it 11 to 18 degrees and moves it 0.9 to 1.4 m a frame: at 10 degrees each
/// frame of the swing started a new history, the scene at its render size for those frames, blocky at the smaller modes.
/// ((Triforce) never less than one 60th, as frame generation's: NX2navi scaled from a quarter of one, but the time here is
/// the CPU's between two recordings of the upscale, not the game's step, and the replay records frames back to back
/// after a stall (a shader compile, an area load): a few ms apart, which at a quarter took a 1.25 m move, or a 7.5 degree
/// turn with 0.125 m, for a cut, an ordinary swing among them. Above 60 fps a cut is the same move as at 60: an area load
/// (5 to 180 m in a frame) or a jump of the camera still is one)
constexpr CutRule kUpscalerCut{30.0f, 1.0f};

/// A camera cut between two frames frame_ms apart: the camera moved over 5 units, or turned over the rule's degrees while
/// it moved over half a unit, for each 60th of a second (a slower frame may move in proportion, up to four times, a faster
/// one down to the rule's least; the game's world is in metres, its near plane at 0.1). A made frame between the two
/// pictures of a cut would blend two views that have nothing to do with each other: such a frame starts a new history and
/// none is made; an upscaler's history from before a cut is no use after it. The thresholds are NX2navi's, from its camera
/// traces of the same game (a median of 0.05 m and 0.08 degrees a frame in play, area loads of 5 to 180 m in one frame)
inline bool camera_cut(const Pose& before, const Pose& now, float frame_ms, const CutRule& rule = kFrameGenCut) {
    const float least = rule.min_scale > 0.0f ? rule.min_scale : 1.0f;
    const float k = frame_ms / 16.667f;
    const float scale = !(frame_ms > 0.0f) ? 1.0f : k < least ? least : k > 4.0f ? 4.0f : k;
    float move2 = 0.0f, dot = 0.0f, la = 0.0f, lb = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float d = now.pos[i] - before.pos[i];
        move2 += d * d, dot += before.fwd[i] * now.fwd[i], la += before.fwd[i] * before.fwd[i], lb += now.fwd[i] * now.fwd[i];
    }
    if (!std::isfinite(move2) || !std::isfinite(dot) || !(la > 0.0f) || !(lb > 0.0f)) return true;   // (no camera to compare)
    const float move = std::sqrt(move2);
    const float cosine = dot / std::sqrt(la * lb);
    const float turn = std::acos(cosine < -1.0f ? -1.0f : cosine > 1.0f ? 1.0f : cosine);   // (radians)
    const float turn_limit = rule.turn_degrees * 0.017453293f;   // (radians)
    return move > 5.0f * scale || (turn > turn_limit * scale && move > 0.5f * scale);
}

/// What a present with the row at AMD FSR 4 shows between the game's last frame and this one
enum Made {
    MADE,   ///< AMD's library made the frame
    OWN,    ///< it did not take this present (no scene's depth and motion: a menu, a movie; not available; failed): the
            ///< renderer's own interpolation makes the frame, as at 2x
    SKIP,   ///< a new history (the first frame, a gap, a camera cut): AMD's library was given the frame and no frame is made
    /// ((Triforce, FG-2) the scene's depth and motion are there but AMD's library is not given the frame: no picture
    /// without the HUD this frame. The game's frame is shown alone: never a made frame of AMD's with the HUD moved in it
    /// (NX2navi 5bdf583's rule), and not the renderer's own in its place. (HDR Native and an RGBA16F scene, which hold for
    /// every present, are OWN: held_made)
    GAME
};
/// Whether a present's answer shows the game's frame alone, nothing made or blended before it (SKIP, GAME)
constexpr bool game_frame_alone(Made m) { return m == SKIP || m == GAME; }

/// ((Triforce, FG-2) Why a present with the scene's depth and motion was not given to AMD's library, for its log.
/// HELD_HDR_NATIVE: the game's own HDR (present_native_pq), whose picture without the HUD only the game could encode.
/// HELD_HDR_SCENE: a run whose scene is RenoDX's RGBA16F (vk_temporal.cpp temporal_scene_format), whose upscale output is
/// no picture without the HUD this renderer can encode as the presented one is. HELD_NO_HUDLESS: this frame has none
enum Held { HELD_NONE, HELD_NO_HUDLESS, HELD_HDR_NATIVE, HELD_HDR_SCENE, HELD_COUNT };
/// The reason, from what the present has: the game's own HDR first, then the run's RGBA16F scene (neither can have a
/// picture without the HUD), then the picture without the HUD of this frame
constexpr Held held_for(bool hdr_native, bool hdr_scene, bool have_hudless) {
    return hdr_native ? HELD_HDR_NATIVE : hdr_scene ? HELD_HDR_SCENE : !have_hudless ? HELD_NO_HUDLESS : HELD_NONE;
}
/// ((Triforce) Whether a reason holds for every present while it lasts (HDR Native, the RGBA16F scene), not for one frame
/// that is missing its picture without the HUD. Then AMD's library is not asked at all (fsr4fg::frames_wanted: no inputs
/// are made for it) and the renderer's own interpolation makes the one frame, as at 2x (OWN), which is what a saved 9 ran
/// as on an RX 6000 or 7000 before AMD's FSR 3.1 frame generation was offered there. Only a frame missing its picture
/// without the HUD shows the game's frame alone (GAME)
constexpr bool held_whole_run(Held h) { return h == HELD_HDR_NATIVE || h == HELD_HDR_SCENE; }
/// What a held present shows: the renderer's own frame for a reason that lasts, the game's frame alone for a missing picture
constexpr Made held_made(Held h) { return held_whole_run(h) ? OWN : GAME; }

/// ((Triforce, FG-2) The picture without the HUD is taken at the gameplay upscale's output (its size ow x oh), the present
/// is the window texture (sw x sh) cropped to x, y, w, h (vk_present_rect.h present_source_rect). The part of the output that
/// is the present's crop, scaled when the two sizes differ (the handheld size's scaled crop, NX2navi 81cdd67), in whole
/// pixels inside the output; all zero when there is nothing to take
struct SourceRect {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
inline SourceRect hudless_source(uint32_t ow, uint32_t oh, uint32_t sw, uint32_t sh, uint32_t x, uint32_t y, uint32_t w,
                                 uint32_t h) {
    SourceRect r;
    if (!ow || !oh || !sw || !sh || !w || !h || x >= sw || y >= sh) return r;
    const uint64_t rx = x + w > sw ? sw : (uint64_t)x + w, ry = y + h > sh ? sh : (uint64_t)y + h;
    r.x0 = (int32_t)((uint64_t)x * ow / sw), r.y0 = (int32_t)((uint64_t)y * oh / sh);
    r.x1 = (int32_t)(rx * ow / sw), r.y1 = (int32_t)(ry * oh / sh);
    if (r.x1 <= r.x0 || r.y1 <= r.y0) r = SourceRect{};
    return r;
}

/// A frame generation context as it was made: the presented picture's size, the largest scene it takes, and whether the
/// scene's far plane is at infinity (a context is made for one kind of depth)
struct ContextKey {
    uint32_t width = 0, height = 0, max_render_width = 0, max_render_height = 0;
    bool infinite_depth = false;
};
/// Whether a context made as c takes a frame of w x h with a scene of rw x rh: else it is made again
constexpr bool context_fits(const ContextKey& c, uint32_t w, uint32_t h, uint32_t rw, uint32_t rh, bool infinite_depth) {
    return c.width == w && c.height == h && rw <= c.max_render_width && rh <= c.max_render_height &&
           c.infinite_depth == infinite_depth;
}
/// The context for such a frame: the scene is never larger than the picture in play, and a larger one still gets a context
constexpr ContextKey context_for(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh, bool infinite_depth) {
    return ContextKey{w, h, rw > w ? rw : w, rh > h ? rh : h, infinite_depth};
}

/// The presents a context is kept after its last frame: 120 once the row has left AMD FSR 4 (stepping through the row's
/// choices passes it: nothing is made and let go again within two seconds), 2 after a failure. Never at the present of
/// its last frame: that frame's commands are recorded and not yet on the queue
constexpr uint64_t kIdlePresents = 120, kFailedPresents = 2;
constexpr bool release_due(bool have, bool failed, uint64_t presents, uint64_t used_at) {
    return have && presents >= used_at && presents - used_at >= (failed ? kFailedPresents : kIdlePresents);
}

/// Taking AMD's frame generation down, in the order that is safe, as one function the real code (vk_fsr4.cpp) and
/// tests/test_framegen_fsr4.py both run. ops.stop: no call into AMD's library starts after it (the lock a call holds is
/// taken, so one in progress has returned). At the process's exit (HOS_OPT_EXITING, host/hos_relaunch.c) that is all:
/// there is no swapchain or thread of AMD's to stop (the renderer presents the made frames itself), and what the context
/// holds goes with the process, as the upscaler's does; AMD's library is not called again, so nothing of it runs under
/// ExitProcess. In play (the row left AMD FSR 4, another size, a failure): the GPU is past every command buffer that
/// used the context (ops.wait_gpu), then AMD's context goes (ops.destroy_context) before the image it wrote into
/// (ops.destroy_images), never the other way round
template <class Ops> void teardown(Ops& ops, bool exiting) {
    ops.stop();
    if (exiting) return;
    ops.wait_gpu();
    ops.destroy_context();
    ops.destroy_images();
}

} // namespace mkwvk::framegen_rows
