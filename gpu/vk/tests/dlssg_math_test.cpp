// Game-free regression for DLSS Frame Generation's camera (gpu/vk/dlssg_math.h): the matrices NGX is given, their order, the
// camera's basis and position, and the cut test.
#include <cmath>
#include <cstdio>

#include "../dlss_math.h"
#include "../dlssg_math.h"

namespace {
int failures = 0;

void check(const char* name, bool ok) {
    if (ok) return;
    std::fprintf(stderr, "FAIL %s\n", name);
    failures++;
}
bool near(double a, double b, double tol = 1e-4) { return std::fabs(a - b) <= tol * (1.0 + std::fabs(b)); }

// a perspective projection as the game fills it (row major, clip = p * view position, w row 0 0 -1 0), reversed depth
void perspective(float p[16], double fovy, double aspect, double n) {
    for (int i = 0; i < 16; i++) p[i] = 0.0f;
    const double c = 1.0 / std::tan(fovy / 2);
    p[0] = (float)(c / aspect), p[5] = (float)c, p[14] = -1.0f;
    // infinite far, depth 1 at the near plane (ndc z = 1 there, -1 at infinity): -p22 + p23 / n = 1, -p22 = -1
    p[10] = 1.0f, p[11] = (float)(2.0 * n);
}
// a view, 3 x 4 as stored: a camera at `pos` turned `yaw` radians about +y (looking down -z at yaw 0)
void view(float v[12], double yaw, const double pos[3]) {
    const double c = std::cos(yaw), s = std::sin(yaw);
    const double r[3][3] = {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};   // rows: right, up, back
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) v[i * 4 + k] = (float)r[i][k];
        v[i * 4 + 3] = (float)-(r[i][0] * pos[0] + r[i][1] * pos[1] + r[i][2] * pos[2]);
    }
}
// view-projection = projection * view (the view's fourth row 0 0 0 1)
void view_projection(const float p[16], const float v[12], float out[16]) {
    const double vv[16] = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], 0, 0, 0, 1};
    double pp[16], o[16];
    for (int i = 0; i < 16; i++) pp[i] = p[i];
    mkwvk::dlssg_math::multiply(pp, vv, o);
    for (int i = 0; i < 16; i++) out[i] = (float)o[i];
}
// position (row vector) * m (NGX's order)
void apply_row(const float m[16], const double in[4], double out[4]) {
    for (int c = 0; c < 4; c++) {
        out[c] = 0;
        for (int k = 0; k < 4; k++) out[c] += in[k] * m[k * 4 + c];
    }
}
}  // namespace

int main() {
    namespace dg = mkwvk::dlssg_math;
    float p[16], v0[12], v1[12], vp0[16], vp1[16];
    perspective(p, 1.0, 16.0 / 9.0, 0.1);
    const double pos0[3] = {10, 2, -30}, pos1[3] = {10.5, 2, -30.2};
    view(v0, 0.30, pos0), view(v1, 0.32, pos1);
    view_projection(p, v0, vp0), view_projection(p, v1, vp1);

    dg::Camera c;
    check("a perspective camera builds", dg::camera(v1, p, vp1, vp0, true, c));
    check("the camera's position is the view's inverse translation",
          near(c.pos[0], pos1[0]) && near(c.pos[1], pos1[1]) && near(c.pos[2], pos1[2]));
    // yaw 0.32: right = (cos, 0, -sin)... the rows of the rotation as stored; forward is minus the back row
    check("right is the view's first row", near(c.right[0], std::cos(0.32)) && near(c.right[2], std::sin(0.32)));
    check("up is the view's second row", near(c.up[1], 1.0) && near(c.up[0], 0.0));
    check("forward is minus the back row", near(c.fwd[0], -(-std::sin(0.32))) && near(c.fwd[2], -std::cos(0.32)));
    check("the planes and the field of view come from the projection", near(c.near_plane, 0.1, 1e-3) && near(c.fov_y, 1.0) &&
              near(c.aspect, 16.0 / 9.0) && c.far_plane == dg::kFarInfinite);
    check("an infinite projection is reversed depth", c.depth_reversed);

    // view_to_clip takes a view-space point to clip space in NGX's row-vector order, as the game's projection does in its own
    const double q[4] = {0.7, -0.4, -9.0, 1.0};   // a point in front of the camera
    double clip[4], expect[4];
    apply_row(c.view_to_clip, q, clip);
    for (int r = 0; r < 4; r++) {
        expect[r] = 0;
        for (int k = 0; k < 4; k++) expect[r] += (double)p[r * 4 + k] * q[k];
    }
    check("view_to_clip is the projection's transpose", near(clip[0], expect[0]) && near(clip[1], expect[1]) &&
              near(clip[2], expect[2]) && near(clip[3], expect[3]));
    double back[4];
    apply_row(c.clip_to_view, clip, back);
    check("clip_to_view undoes it", near(back[0], q[0]) && near(back[1], q[1]) && near(back[2], q[2]) && near(back[3], q[3]));

    // a world point's clip position this frame and last: clip_to_prev_clip takes the first to the second
    const double world[4] = {12.0, 3.0, -41.0, 1.0};
    double now_clip[4], prev_clip[4], moved[4], again[4];
    for (int r = 0; r < 4; r++) {
        now_clip[r] = prev_clip[r] = 0;
        for (int k = 0; k < 4; k++) now_clip[r] += (double)vp1[r * 4 + k] * world[k], prev_clip[r] += (double)vp0[r * 4 + k] * world[k];
    }
    apply_row(c.clip_to_prev_clip, now_clip, moved);
    check("clip_to_prev_clip takes this frame's clip position to the frame before's",
          near(moved[0], prev_clip[0], 1e-3) && near(moved[1], prev_clip[1], 1e-3) && near(moved[2], prev_clip[2], 1e-3) &&
              near(moved[3], prev_clip[3], 1e-3));
    apply_row(c.prev_clip_to_clip, moved, again);
    check("prev_clip_to_clip is its inverse", near(again[0], now_clip[0], 1e-3) && near(again[1], now_clip[1], 1e-3) &&
              near(again[2], now_clip[2], 1e-3) && near(again[3], now_clip[3], 1e-3));

    // no frame before: the identity pair
    dg::Camera first;
    check("a first frame builds", dg::camera(v1, p, vp1, vp1, false, first));
    bool identity = true;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) identity &= first.clip_to_prev_clip[r * 4 + k] == (r == k ? 1.0f : 0.0f);
    check("without a frame before the clip-to-clip pair is the identity", identity);

    // jitter off the matrices: the host's jitter moves clip x and y by jitter * w; unjittered, the camera of a still camera is
    // the identity pair again, whatever each frame's jitter was
    float vpj0[16], vpj1[16];
    const float j0[2] = {0.0007f, -0.0004f}, j1[2] = {-0.0005f, 0.0009f};
    view_projection(p, v1, vpj1), view_projection(p, v1, vpj0);
    for (int c4 = 0; c4 < 4; c4++) {
        vpj1[c4] += j1[0] * vpj1[12 + c4], vpj1[4 + c4] += j1[1] * vpj1[12 + c4];
        vpj0[c4] += j0[0] * vpj0[12 + c4], vpj0[4 + c4] += j0[1] * vpj0[12 + c4];
    }
    mkwvk::dlss_math::unjitter(vpj1, j1), mkwvk::dlss_math::unjitter(vpj0, j0);
    dg::Camera still;
    check("a still camera builds", dg::camera(v1, p, vpj1, vpj0, true, still));
    bool ident2 = true;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) ident2 &= near(still.clip_to_prev_clip[r * 4 + k], r == k ? 1.0 : 0.0, 1e-4);
    check("a still camera's clip-to-prev-clip is the identity once the jitter is off", ident2);

    // things that are not a camera
    float flat[16] = {};
    check("a zero projection is not a camera", !dg::camera(v1, flat, vp1, vp0, true, c));
    float bad_view[12];
    for (int i = 0; i < 12; i++) bad_view[i] = v1[i];
    bad_view[5] = NAN;
    check("a view with a NaN is not a camera", !dg::camera(bad_view, p, vp1, vp0, true, c));
    float singular[16] = {};
    check("a singular view-projection is not a camera", !dg::camera(v1, p, singular, vp0, true, c));

    // the cut test
    const float a_pos[3] = {0, 0, 0}, a_fwd[3] = {0, 0, -1};
    const float near_pos[3] = {0.2f, 0, 0}, turned_fwd[3] = {(float)std::sin(0.2), 0, (float)-std::cos(0.2)};
    const float far_pos[3] = {40, 0, 0}, side_fwd[3] = {1, 0, 0};
    check("a step is not a cut", !dg::is_cut(a_pos, a_fwd, near_pos, turned_fwd, 1.0f));
    check("a jump of 40 units is a cut", dg::is_cut(a_pos, a_fwd, far_pos, a_fwd, 1.0f));
    const float one_pos[3] = {1.0f, 0, 0};
    check("a quarter turn while moving is a cut", dg::is_cut(a_pos, a_fwd, one_pos, side_fwd, 1.0f));
    check("a quarter turn on the spot is not (a camera swinging round Link)", !dg::is_cut(a_pos, a_fwd, a_pos, side_fwd, 1.0f));
    const float mid_pos[3] = {15, 0, 0};
    check("15 units in one frame is a cut", dg::is_cut(a_pos, a_fwd, mid_pos, a_fwd, 1.0f));
    check("15 units over four frames is not (the allowance grows with the gap)", !dg::is_cut(a_pos, a_fwd, mid_pos, a_fwd, 4.0f));
    return failures ? 1 : 0;
}
