// (Triforce) The Anti-aliasing row's FXAA (gpu/vk/vk_aa.cpp; aa_post.comp): a fast approximate smoothing of the tone mapped
// picture's edges, in place of the game's SMAA. After NX2navi's shaders/aa_post.comp.
//
// It works on the square-rooted luma (aa_common.glsl aa_encode: the space the picture is stored in). Where the local contrast
// is high enough, it finds whether the edge runs across or along the pixel, walks along the edge both ways until the luma
// pair it follows changes, and moves the sample point toward the neighbour across the edge by how far the pixel sits from
// the nearer end of the edge. A blur by the 3 x 3 neighbourhood's contrast handles features thinner than a pixel. The sample
// at the moved point is bilinear, so the move blends the two sides.
//
// The file that includes it says what the picture is: vec3 aa_at(vec2 uv), the picture's encoded colour at uv (0 to 1,
// bilinear between the texels' centres, the edge's texels past the picture's edge). Written in what GLSL and C++ share:
// tests/test_aa_shaders.py compiles this file as C++ and runs it on authored pixels.

const float AA_EDGE_MIN = 0.0312;   // (no smoothing below this contrast: dark areas)
const float AA_EDGE_REL = 0.125;    // (nor below this share of the brightest neighbour)
const float AA_SUBPIXEL = 0.75;     // (how much a thin feature is blurred)
const int AA_STEPS = 12;
// (how far each step of the walk along the edge goes, in texels: 1 x 5, 1.5, 2 x 4, 4, 8)
float aa_step_len(int i) { return i < 5 ? 1.0 : i == 5 ? 1.5 : i < 10 ? 2.0 : i == 10 ? 4.0 : 8.0; }

float aa_luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float aa_luma_at(vec2 uv) { return aa_luma(aa_at(uv)); }

// The picture at uv, smoothed. d: one texel (1 / the picture's size)
vec3 aa_fxaa(vec2 uv, vec2 d) {
    vec3 cM = aa_at(uv);
    float lM = aa_luma(cM);
    float lN = aa_luma_at(uv + vec2(0.0, -d.y)), lS = aa_luma_at(uv + vec2(0.0, d.y));
    float lW = aa_luma_at(uv + vec2(-d.x, 0.0)), lE = aa_luma_at(uv + vec2(d.x, 0.0));
    float lo = min(lM, min(min(lN, lS), min(lW, lE)));
    float hi = max(lM, max(max(lN, lS), max(lW, lE)));
    float range = hi - lo;
    if (range < max(AA_EDGE_MIN, hi * AA_EDGE_REL)) return cM;
    float lNW = aa_luma_at(uv + vec2(-d.x, -d.y)), lNE = aa_luma_at(uv + vec2(d.x, -d.y));
    float lSW = aa_luma_at(uv + vec2(-d.x, d.y)), lSE = aa_luma_at(uv + vec2(d.x, d.y));

    // thin features: how far the centre stands out from the neighbourhood's weighted mean
    float mean = (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0;
    float sub = clamp(abs(mean - lM) / range, 0.0, 1.0);
    sub = sub * sub * (3.0 - 2.0 * sub);   // (smoothstep(0, 1, sub))
    sub = sub * sub * AA_SUBPIXEL;

    // the edge's direction: horizontal when the luma changes more from row to row than from column to column
    float across_rows = abs(0.25 * lNW - 0.5 * lN + 0.25 * lNE) + abs(0.5 * lW - lM + 0.5 * lE) +
                        abs(0.25 * lSW - 0.5 * lS + 0.25 * lSE);
    float across_cols = abs(0.25 * lNW - 0.5 * lW + 0.25 * lSW) + abs(0.5 * lN - lM + 0.5 * lS) +
                        abs(0.25 * lNE - 0.5 * lE + 0.25 * lSE);
    bool horizontal = across_cols >= across_rows;

    // which side of the pixel the edge is on: the neighbour across it with the larger step
    float l1 = horizontal ? lN : lW, l2 = horizontal ? lS : lE;
    float g1 = abs(l1 - lM), g2 = abs(l2 - lM);
    bool first = g1 >= g2;
    float grad = 0.25 * max(g1, g2);
    float step_len = horizontal ? d.y : d.x;
    float pair = 0.5 * (l2 + lM);   // (the mean luma of the pixel and its neighbour across the edge, which the walk follows)
    if (first) {
        step_len = -step_len;
        pair = 0.5 * (l1 + lM);
    }
    vec2 edge = uv;
    if (horizontal) edge.y += 0.5 * step_len;
    else edge.x += 0.5 * step_len;

    // walk along the edge both ways until the pair's luma changes by more than a quarter of the step across it
    vec2 dir = horizontal ? vec2(d.x, 0.0) : vec2(0.0, d.y);
    vec2 uv1 = edge - dir * aa_step_len(0), uv2 = edge + dir * aa_step_len(0);
    float e1 = aa_luma_at(uv1) - pair, e2 = aa_luma_at(uv2) - pair;
    bool done1 = abs(e1) >= grad, done2 = abs(e2) >= grad;
    for (int i = 1; i < AA_STEPS && !(done1 && done2); i++) {
        if (!done1) {
            uv1 = uv1 - dir * aa_step_len(i);
            e1 = aa_luma_at(uv1) - pair;
            done1 = abs(e1) >= grad;
        }
        if (!done2) {
            uv2 = uv2 + dir * aa_step_len(i);
            e2 = aa_luma_at(uv2) - pair;
            done2 = abs(e2) >= grad;
        }
    }
    float dist1 = horizontal ? uv.x - uv1.x : uv.y - uv1.y;
    float dist2 = horizontal ? uv2.x - uv.x : uv2.y - uv.y;
    bool nearer1 = dist1 < dist2;
    float offset = 0.5 - min(dist1, dist2) / (dist1 + dist2);
    // only when the luma at the nearer end moves the way the centre's does: else the pixel is outside the edge's span
    if (((nearer1 ? e1 : e2) < 0.0) == (lM < pair)) offset = 0.0;
    offset = max(offset, sub);
    vec2 moved = uv;
    if (horizontal) moved.y += offset * step_len;
    else moved.x += offset * step_len;
    return aa_at(moved);
}
