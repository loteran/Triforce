// (hookshot) NVIDIA DLSS Frame Generation's camera, from the scene's own view and projection: plain functions, no Vulkan, no
// NGX. gpu/vk/tests/dlssg_math_test.cpp checks them.
//
// DLSS-G (NGX's FrameGeneration, vk_ngxfg.cpp) wants more of the camera than the upscalers do: the projection and its
// inverse, the matrices that take this frame's clip space to the frame before's and back, the camera's position and basis,
// and its planes, field of view and aspect. The game's vertex stages read all of it from one uniform buffer (the vertex
// stage's buffer 3: vk_temporal.cpp's temporal_note_scene_draw), row major, column vectors (clip = M * position):
//   0x00  the view, 3 x 4: its rows are the camera's right, up and back (the camera looks down -z), its last column the
//         translation. The fourth row (0 0 0 1) is not stored
//   0x30  the view-projection, 4 x 4
//   0x70  the projection, 4 x 4 (its w row 0 0 -1 0)
// all of them with the sub-pixel jitter the host puts on the projection (nx2/oot_jitter.py). DLSS-G wants none in any matrix
// (NVSDK_NGX_DLSSG_Opt_Eval_Params: "All matrices should NOT contain temporal AA jitter offset"), and takes the jitter itself,
// so the callers unjitter first (dlss_math.h unjitter).
//
// NGX's matrices are the ones Streamline names: row vectors (clip = position * M), so each is the transpose of the game's
// (the same hand-over as NX2navi's runtime/vk_dlaa.c fg_camera, which runs this game's DLSS-G on RTX 40 and 50 cards).
#pragma once

#include <cfloat>
#include <cmath>

#include "fsr3_math.h"

namespace mkwvk::dlssg_math {

/// The distance DLSS-G is told for an infinite far plane (a distance no surface reaches)
constexpr float kFarInfinite = 1e8f;

struct Camera {
    float view_to_clip[16]{};        ///< NGX's order (row vectors): the projection's transpose
    float clip_to_view[16]{};
    float clip_to_prev_clip[16]{};   ///< this frame's clip space to the frame before's, jitter free in both
    float prev_clip_to_clip[16]{};
    float pos[3]{}, up[3]{}, right[3]{}, fwd[3]{};   ///< world space
    float near_plane = 0.0f, far_plane = 0.0f;       ///< far: kFarInfinite for an infinite projection
    float fov_y = 0.0f;                              ///< radians
    float aspect = 1.0f;                             ///< view space width over height
    bool depth_reversed = false;                     ///< the device depth runs 1 at the near plane (the projection's, not FSR's copy)
};

/// The inverse of a 4 x 4 (row major), Gauss-Jordan with partial pivoting in double precision (a view-projection of this game
/// has world coordinates in the thousands and a far plane in the tens of thousands: a float inverse is noise); false when
/// singular or not finite
inline bool invert(const double m[16], double out[16]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (!(std::fabs(a[p][c]) > 1e-300)) return false;
        if (p != c)
            for (int k = 0; k < 8; k++) {
                const double t = a[p][k];
                a[p][k] = a[c][k], a[c][k] = t;
            }
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c || a[r][c] == 0.0) continue;
            const double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            out[r * 4 + c] = a[r][c + 4];
            if (!std::isfinite(out[r * 4 + c])) return false;
        }
    return true;
}

/// a * b (row major, column vectors: applies b first)
inline void multiply(const double a[16], const double b[16], double out[16]) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double v = 0.0;
            for (int k = 0; k < 4; k++) v += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = v;
        }
}

/// The matrix for NGX: m's transpose, as floats
inline void to_ngx(const double m[16], float out[16]) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = (float)m[c * 4 + r];
}

/// This frame's clip space to the frame before's, previous * inverse(current) (view-projections, jitter free, row major, column
/// vectors), and its inverse; false when either cannot be inverted
inline bool clip_to_prev_clip(const float current[16], const float previous[16], double out[16], double inverse[16]) {
    double c[16], p[16], ci[16];
    for (int i = 0; i < 16; i++) c[i] = current[i], p[i] = previous[i];
    if (!invert(c, ci)) return false;
    multiply(p, ci, out);
    return invert(out, inverse);
}

/// The camera of a frame. view: the buffer's 3 x 4 view; projection: its projection, jitter free; current and previous: the two
/// frames' view-projections, jitter free (false for `have_previous` when there is no frame before: the two clip-to-clip pairs
/// are then the identity, and the caller resets DLSS-G's history). False when the projection is not a perspective one or the
/// view is not finite
inline bool camera(const float view[12], const float projection[16], const float current[16], const float previous[16],
                   bool have_previous, Camera& out) {
    out = Camera{};
    fsr3_math::Camera planes;
    if (!fsr3_math::camera_from_projection(projection, planes)) return false;
    for (int i = 0; i < 12; i++)
        if (!std::isfinite(view[i])) return false;
    double p[16], pi[16];
    for (int i = 0; i < 16; i++) p[i] = projection[i];
    if (!invert(p, pi)) return false;
    to_ngx(p, out.view_to_clip), to_ngx(pi, out.clip_to_view);
    double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, mi[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (have_previous && !clip_to_prev_clip(current, previous, m, mi)) return false;
    to_ngx(m, out.clip_to_prev_clip), to_ngx(mi, out.prev_clip_to_clip);
    for (int i = 0; i < 3; i++) {
        out.right[i] = view[i], out.up[i] = view[4 + i], out.fwd[i] = -view[8 + i];
        // the position: -R^T t, R the view's 3 x 3 and t its last column
        out.pos[i] = -(view[i] * view[3] + view[4 + i] * view[7] + view[8 + i] * view[11]);
    }
    out.near_plane = planes.near_plane;
    out.far_plane = planes.far_plane >= FLT_MAX ? kFarInfinite : planes.far_plane;
    out.fov_y = planes.fov_y;
    out.aspect = projection[0] != 0.0f ? std::fabs(projection[5] / projection[0]) : 1.777f;
    out.depth_reversed = !planes.flip_depth;
    return true;
}

/// Whether the camera of two frames `frames` of the game's (60 Hz) frames apart is a cut (a teleport, a cut scene's new shot,
/// an area load) rather than motion: DLSS-G's history is then no use. NX2navi's thresholds, measured on this game: further
/// than 5 units (metres) a frame, or turned more than 30 degrees a frame while moving more than half a unit
inline bool is_cut(const float pos_before[3], const float fwd_before[3], const float pos[3], const float fwd[3], float frames) {
    const double k = frames < 0.25f ? 0.25 : frames > 4.0f ? 4.0 : (double)frames;
    double dot = 0.0, move2 = 0.0;
    for (int i = 0; i < 3; i++) {
        dot += (double)fwd[i] * fwd_before[i];
        const double d = (double)pos[i] - pos_before[i];
        move2 += d * d;
    }
    const double turn = std::acos(dot > 1.0 ? 1.0 : dot < -1.0 ? -1.0 : dot) * (180.0 / 3.14159265358979323846);
    const double move = std::sqrt(move2);
    return move > 5.0 * k || (turn > 30.0 * k && move > 0.5 * k);
}

} // namespace mkwvk::dlssg_math
