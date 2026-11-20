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
/// keeps its meaning), then 9: AMD FSR 4, one frame made by AMD's library between each two of the game's
enum { OFF = 0, AUTO = 1, kMaxMult = 8, FSR4 = 9, kRowMax = FSR4 };

/// Why AMD FSR 4 cannot make the frames here (the row says it); FG_OK when it can. FG_NOT_YET: not asked, the device is not
/// made yet
enum Why { FG_OK, FG_NOT_BUILT, FG_ENV_OFF, FG_NOT_YET, FG_NO_LIBRARY, FG_NO_FILE, FG_NO_MODEL, FG_FAILED, FG_WHY_COUNT };

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
/// Of the frame generation versions AMD's library lists for this GPU, the one contexts are made with: the newest FSR 4, the
/// first of equal ones; -1 when it lists none. An FSR 3.1 frame generation alone is not FSR 4: it never stands in under
/// FSR 4's name, as vk_fsr4.cpp's upscaler takes no FSR 3.1 either. any (RX_VK_FSR4_FG=1, for trying the path on a card
/// without FSR 4): the newest of whatever is listed
inline int pick_version(const char* const* names, int n, bool any) {
    int best = -1;
    unsigned bv[3] = {0, 0, 0};
    for (int i = 0; i < n; i++) {
        unsigned v[3];
        if (!parse_version(names[i], v) || (!any && v[0] != 4)) continue;
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
/// A camera cut between two frames frame_ms apart: the camera moved over 5 units, or turned over 10 degrees while it
/// moved over half a unit, for each 60th of a second (a slower frame may move in proportion, up to four times; the game's
/// world is in metres, its near plane at 0.1). A made frame between the two pictures of a cut would blend two views that
/// have nothing to do with each other: such a frame starts a new history and none is made. The thresholds are NX2navi's,
/// from its camera traces of the same game (a median of 0.05 m and 0.08 degrees a frame in play, area loads of 5 to
/// 180 m in one frame)
inline bool camera_cut(const Pose& before, const Pose& now, float frame_ms) {
    const float scale = !(frame_ms > 0.0f) ? 1.0f : frame_ms < 16.667f ? 1.0f : frame_ms > 66.667f ? 4.0f : frame_ms / 16.667f;
    float move2 = 0.0f, dot = 0.0f, la = 0.0f, lb = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float d = now.pos[i] - before.pos[i];
        move2 += d * d, dot += before.fwd[i] * now.fwd[i], la += before.fwd[i] * before.fwd[i], lb += now.fwd[i] * now.fwd[i];
    }
    if (!std::isfinite(move2) || !std::isfinite(dot) || !(la > 0.0f) || !(lb > 0.0f)) return true;   // (no camera to compare)
    const float move = std::sqrt(move2);
    const float cosine = dot / std::sqrt(la * lb);
    const float turn = std::acos(cosine < -1.0f ? -1.0f : cosine > 1.0f ? 1.0f : cosine);   // (radians)
    constexpr float kTenDegrees = 0.17453293f;
    return move > 5.0f * scale || (turn > kTenDegrees * scale && move > 0.5f * scale);
}

/// What a present with the row at AMD FSR 4 shows between the game's last frame and this one
enum Made {
    MADE,   ///< AMD's library made the frame
    OWN,    ///< it did not take this present (no scene's depth and motion: a menu, a movie; not available; failed): the
            ///< renderer's own interpolation makes the frame, as at 2x
    SKIP    ///< a new history (the first frame, a gap, a camera cut): AMD's library was given the frame and no frame is made
};

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
