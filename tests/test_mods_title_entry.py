"""The Mods entry under the title's list drawn and animated as the game's own entries (gpu/vk/mods_entry_anim.h,
vk_game_tex.h make_title_band and make_title_skin, vk_menu.cpp draw_title_mods and menu_tick, vk_imgui.cpp; no game).

(Triforce, 11-19) His "make it the same as the other options, with the green filigree, the same animations: when you
select it, it just pops in". Each entry of the title's list is the part PaCommonBtnTitle_00 of the Title archive's layout,
which plays its own Select, UnSelect, LoopRandom and Decide. This compiles mods_entry_anim.h and vk_game_tex.h's
make_title_band with made-up textures and checks:

- the layout's curves as read from the part: Select over 1 frame, UnSelect over 8 (half way at 4, not gone after 1),
  LoopRandom 0.6, 0.5, 0.6 over 40 frames, Decide's filigree from 1 to 0.85 between frames 8 and 16 and its sparkle from
  frame 2 to 16; a step key (two at one frame) is a step;
- the entry's state from drawing to drawing: chosen in a frame, let go over 8, a change back half way starts from where it
  is (no jump), the entry rising with the list over 15 frames and going over 8 once the list has not been seen for 150 ms
  (mods_list_fresh's); a game frame late (a 70 ms gap in the host's reports: the 30 fps cap, a hitch) leaves it as it is,
  and the list back after it went goes on from where the entry was, not from nothing;
- the band: (7,4,4) at 0.96 where its shape is whole, thinner (0.58) where the gradient is, nothing outside the shape; the
  sparkle the noise inside the shape only.
- ((Triforce, 11-19) the lab's "much weaker dark shadow than the game's entries ... its glow is fainter than the game's
  Options glow") the dark edge (T_BtnSh_00) and the glow (P_BtnGlow_00) as text_spread.h has them, fitted to the game's
  own entries: out from a stroke the edge dark at 6 units and gone by 15, the glow strong at 9 and still there at 21.
- ((Triforce, 11-20) the lab's second check: not chosen, "paler and greyer than New Game ... its dark edge weaker" in the
  1720x720 and 1600x900 windows) at 1440, 900, 720 and 674 picture lines, on a model glyph drawn as draw_title_mods draws
  it (the copies at their offsets from the text's whole pixel, the text kTextWeight times, the shots' HDR capture): the
  edge beside the strokes near black (0.9 and over, the game's 0.93-0.96), the strokes no thinner in units than at 1440,
  the edge gone by 15 units; and the entry as it was (140e05e's rings, snapped, the text once) misses at every size.
- ((Triforce, 11-20, a review of cb644b1) the hard shadow (kShadowCore) fades with the text, its 16 copies together the
  fade's amount of their full darkness (not 0.98 black under text at 0.5); the chosen text drawn kTextWeight times too, its
  strokes as solid at 720 and 674 lines as at 1440.
- ((Triforce, 11-20, lab check 3, shots052c) not chosen, per shape and output: on the model glyph (its strokes 4 to 5.5
  units) in the five shapes the lab ran (3440x1440, 1680x1050 with a 16:10 picture squeezed across by 0.9, 1720x720,
  1600x900 with a 21:9 picture of 674 lines, all HDR, and 1720x720 with HDR off) against the game's New Game, Options and
  Continue measured in the same shots: in HDR the strokes at least half as much bolder than 26670c0's as the game's are
  (26% at 1440, 31% at 674) and no more than 0.15 of a pixel past the game's, no wider in SDR; the hard shadow softer 3
  units out in the windows and the 16:10 picture and as it was in the fullscreen; with HDR off the edge as dark as the HDR
  picture's (each black copy darker: text_spread.h kLookSdr), where 26670c0's was a third as dark. The text's copies fade
  together (the fade's amount where the text is whole) at every size; the copies a frame counted (159 black, 4 of the text).

And reads the source for what only the game shows: the title textures read after the menu's own (one missing leaves the
skin as it was); the entry drawn with the band, the filigree mirrored on the left and breathing with LoopRandom, the text at
65 x 0.8 units in the layout's cream (white when chosen, over its blue glow), no pulsing copies; A or a click on it plays
the system menu's "decide" at once and opens the Mods screen after Decide's 16 frames, not twice; the entry drawn on while
it fades after its list; the mouse moving up off it gives the cursor back.

    python -I -B tests/test_mods_title_entry.py [--cxx clang++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def function(text, head):
    start = text.index(head)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PROGRAM = r"""
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "mods_entry_anim.h"
#include "vk_game_tex.h"
#include "text_spread.h"
using namespace mods_entry;
// ((Triforce, 11-19) the dark edge's or the glow's alpha at d units out from the edge of a stroke (a half plane of text,
// x under 0): each copy a ring's radius r round, at its angle, covers the place when r cos(angle) > d; the copies over it)
// ((Triforce, 11-19) as it is drawn (text_spread::ring_drawn: whole steps of 255, the copies as many as keep the coverage),
// or with fit set the fitted rings' own float alpha, the entry's fade amount over them)
static float spread_at(const text_spread::Spread& s, float d, float amount = 1, bool fit = false) {
    float keep = d < 0 ? 1 - (fit ? s.centre * amount : (float)text_spread::centre_drawn(s, amount) / 255) : 1;
    for (int i = 0; i < s.rings; i++) {
        const text_spread::Drawn dr = text_spread::ring_drawn(s, i, amount);
        const float r = text_spread::ring_radius(s, i);
        const float a = fit ? text_spread::ring_alpha(s, i) * amount : (float)dr.alpha / 255;
        const int n = fit ? text_spread::ring_copies(s, i) : dr.copies;
        for (int j = 0; j < n; j++)
            if (r * std::cos(6.2831853f * (float)j / (float)n) > d) keep *= 1 - a;
    }
    return 1 - keep;
}
// (over a pixel of the lab's shots, 3 units: the half plane's edge at a single d is sharper than any pixel, and a ring of
// a few copies more or fewer moves which of them pass it)
static float spread_px(const text_spread::Spread& s, float d, float amount, bool fit) {
    float sum = 0;
    for (int i = 0; i <= 12; i++) sum += spread_at(s, d - 1.5f + 0.25f * (float)i, amount, fit);
    return sum / 13;
}
// ((Triforce, 11-20, lab check 3) the entry not chosen as drawn in one of the lab's shapes, on a model glyph: two stems of
// the Cetra Display "Normal" text at the entry's size (sw units wide: ImGui draws its strokes 4 to 5.5, 40 high, 12 apart,
// as in "M" or "n"), each pixel's coverage the stems' area in it. lines: the picture's (2160 / lines units a pixel down); kx: the grid's
// squeeze across (0.9 on a 16:10 picture: the stems and every copy's offset across with it, as put_on_grid moves the
// drawing); hdr: the shots' HDR capture (the overlay in PQ code values: a black copy of alpha a darkens 1 - (1 - a)^2) or
// SDR (8-bit code values: a). The black copies (the core and the soft edge) and the text's own (its weight: in place or
// out across, each at its alpha) at their offsets from the text's whole pixel, the entry at a part of a pixel (fx, fy),
// over a background of 110 (the wheat and grass round the list in the shots)
struct Copy { float dx, dy; int a; };
struct Shape {
    float solid;     // (pixels across a stem where the text shows solid: 0.72 of its brightness and over)
    float darkest;   // (the darkest of the background within 15 units of the strokes, of 1)
    float at3, at6, at9;   // (the background's darkness 3, 6 and 9 units out from a stem's side, along its middle)
};
static float overlap(float a0, float a1, float b0, float b1) { return std::max(0.0f, std::min(a1, b1) - std::max(a0, b0)); }
static Shape shape_model(float lines, float kx, bool hdr, const std::vector<Copy>& black, const std::vector<Copy>& text, float sw, float fx, float fy) {
    const float u = 2160.0f / lines, ux = u / kx;   // (units a pixel down and across)
    const float sh = 40.0f, gap = 12.0f, bg = 110;
    const int W = (int)std::ceil(90 / ux), H = (int)std::ceil(90 / u);
    const float tx = std::floor(W * 0.5f - (sw + gap * 0.5f) / ux) + fx, ty = std::floor(H * 0.5f - sh * 0.5f / u) + fy;
    auto cover = [&](int x, int y, float ox, float oy) {   // (of pixel (x, y): the stems with their top left at (ox, oy))
        float c = 0;
        for (int s = 0; s < 2; s++) {
            const float l = ox + s * (sw + gap) / ux;
            c += overlap((float)x, x + 1.0f, l, l + sw / ux) * overlap((float)y, y + 1.0f, oy, oy + sh / u);
        }
        return std::min(1.0f, c);
    };
    std::vector<float> keep((size_t)W * H, 1.0f), through((size_t)W * H, 1.0f);
    for (const Copy& k : black)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) keep[(size_t)y * W + x] *= 1 - (float)k.a / 255.0f * cover(x, y, tx + k.dx / ux, ty + k.dy / u);
    for (const Copy& k : text)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) through[(size_t)y * W + x] *= 1 - (float)k.a / 255.0f * cover(x, y, tx + k.dx / ux, ty + k.dy / u);
    auto lum = [&](int x, int y) {
        const float k = keep[(size_t)y * W + x], c = 1 - through[(size_t)y * W + x];
        return bg * (hdr ? k * k : k) * (1 - c) + 255 * c;
    };
    Shape e{0, 0, 0, 0, 0};
    const int mid = (int)(ty + sh * 0.5f / u);
    int runs = 0, run = 0;
    float len = 0;
    for (int x = 0; x < W; x++) {
        if (lum(x, mid) >= 0.72f * 255) run++;
        else if (run) runs++, len += (float)run, run = 0;
    }
    e.solid = runs ? len / (float)runs : 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            if (cover(x, y, tx, ty) > 0) continue;
            const float px = x + 0.5f, py = y + 0.5f;
            float d = 1e9f;   // (units to the nearest stem)
            for (int s = 0; s < 2; s++) {
                const float l = tx + s * (sw + gap) / ux;
                const float ddx = std::max({0.0f, l - px, px - (l + sw / ux)}) * ux, ddy = std::max({0.0f, ty - py, py - (ty + sh / u)}) * u;
                d = std::min(d, std::hypot(ddx, ddy));
            }
            if (d <= 15) e.darkest = std::max(e.darkest, 1 - lum(x, y) / bg);
        }
    // (out from the left stem's left side along its middle, between pixels as the picture's filter reads it)
    auto row_at = [&](float units) {
        const float x = tx - units / ux - 0.5f;
        const int x0 = (int)std::floor(x);
        const float t = x - x0;
        return 1 - ((1 - t) * lum(x0, mid) + t * lum(x0 + 1, mid)) / bg;
    };
    e.at3 = row_at(3), e.at6 = row_at(6), e.at9 = row_at(9);
    return e;
}
// (over the entry's places in a pixel, where the window puts the list, and the strokes' widths of "Mods")
static Shape shape_avg(float lines, float kx, bool hdr, const std::vector<Copy>& black, const std::vector<Copy>& text) {
    Shape s{0, 0, 0, 0, 0};
    const float fs[4] = {0.0f, 0.3f, 0.55f, 0.8f}, sws[4] = {4.0f, 4.5f, 5.0f, 5.5f};
    for (float sw : sws)
        for (float fx : fs)
            for (float fy : fs) {
                const Shape e = shape_model(lines, kx, hdr, black, text, sw, fx, fy);
                s.solid += e.solid / 64, s.darkest += e.darkest / 64, s.at3 += e.at3 / 64, s.at6 += e.at6 / 64, s.at9 += e.at9 / 64;
            }
    return s;
}
// ((Triforce, 11-20) the model glyph's stems drawn weight times over themselves on their whole pixel, at grid_h lines: their
// width in units where the text shows solid (0.72 and over, as Edge's weight), over the entry's places in a pixel; the
// background's own brightness left out (the chosen text is over the blue glow, not the wheat))
static float solid_units(float grid_h, int weight) {
    const float u = 2160.0f / grid_h, sw = 4.5f, gap = 12.0f;
    float sum = 0;
    for (int i = 0; i < 16; i++) {
        const float l0 = 4 + (float)i / 16;   // (pixels: the left stem's left side)
        for (int x = 0; x < (int)std::ceil(80 / u); x++) {
            float c = 0;
            for (int s = 0; s < 2; s++) {
                const float l = l0 + s * (sw + gap) / u;
                c += overlap((float)x, x + 1.0f, l, l + sw / u);
            }
            if (1 - std::pow(1 - std::min(1.0f, c), (float)weight) >= 0.72f) sum += u * 0.5f / 16;   // (two stems)
        }
    }
    return sum;
}
static std::vector<Copy> copies_of(std::initializer_list<text_spread::Spread> spreads, float amount = 1) {
    std::vector<Copy> v;
    for (const text_spread::Spread& s : spreads) text_spread::each_copy(s, amount, [&](float dx, float dy, int a) { v.push_back({dx, dy, a}); });
    return v;
}
static std::vector<Copy> text_of(const text_spread::Weight& w, float lines, float amount = 1) {
    std::vector<Copy> v;
    text_spread::each_weight_copy(w, lines, amount, [&](float dx, float dy, int a) { v.push_back({dx, dy, a}); });
    return v;
}
// (where the text is whole: how opaque its copies together are, of 1 (those in place and those out across all over it))
static float text_whole(const std::vector<Copy>& text) {
    float through = 1;
    for (const Copy& k : text) through *= 1 - (float)k.a / 255.0f;
    return 1 - through;
}
static int fails = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%d %s\n", __LINE__, #x); fails++; } } while (0)
static bool near(float a, float b, float tol = 0.01f) { return std::fabs(a - b) <= tol; }
int main() {
    // the layout's curves
    CHECK(near(curve(kSelect, 0), 0) && near(curve(kSelect, 1), 1) && near(curve(kSelect, 5), 1));
    CHECK(near(curve(kUnSelect, 0), 1) && near(curve(kUnSelect, 4), 0.5f) && near(curve(kUnSelect, 8), 0));
    CHECK(curve(kUnSelect, 1) > 0.9f);   // (not gone after a frame: the fade the game's own entries show)
    CHECK(near(curve(kLoop, 0), 0.6f) && near(curve(kLoop, 20), 0.5f) && near(curve(kLoop, 40), 0.6f));
    CHECK(near(curve(kDecideDeco, 4), 1) && near(curve(kDecideDeco, 8), 1) && near(curve(kDecideDeco, 16), 0.85f));
    CHECK(curve(kDecideDeco, 12) < 1 && curve(kDecideDeco, 12) > 0.85f);
    CHECK(near(curve(kDecideGlow, 16), 0.95f) && near(curve(kDecideFlash, 2), 0) && near(curve(kDecideFlash, 16), 1));
    CHECK(near(curve(kListIn, 0), 0) && near(curve(kListIn, 15), 1) && near(curve(kListIn, 7.5f), 0.275f));   // (slow to start: its slopes)
    const Key step[] = {{0, 0, 0}, {5, 0, 0}, {5, 1, 0}, {9, 1, 0}};
    CHECK(near(curve(step, 4, 5), 0) && near(curve(step, 4, 5.01f), 1));
    CHECK(kDecideNs == 16 * kFrameNs && kListGoneNs == 150000000 && kListShownNs == kListGoneNs + 8 * kFrameNs);
    CHECK(in_at(0) == 0 && near(curve(kListIn, frames(in_at(0.275f))), 0.275f) && near(curve(kListIn, frames(in_at(1))), 1));
    // the entry from drawing to drawing (now in ns; the list told each frame unless said)
    Anim a;
    int64_t t = 1000000000;
    a.step(t, false, 0);
    CHECK(near(a.focus, 0) && near(a.shown, 0));   // (the list just came: the entry rises with it)
    a.step(t + 15 * kFrameNs, false, 0);
    CHECK(near(a.shown, 1));
    t += 20 * kFrameNs;
    a.step(t, true, 0);   // (the cursor onto Mods)
    a.step(t + kFrameNs, true, 0);
    CHECK(near(a.focus, 1));
    a.step(t + 2 * kFrameNs, false, 0);   // (and off it: UnSelect)
    a.step(t + 3 * kFrameNs, false, 0);
    CHECK(a.focus > 0.9f && a.focus < 1);
    a.step(t + 6 * kFrameNs, false, 0);
    CHECK(near(a.focus, 0.5f));
    a.step(t + 7 * kFrameNs, true, 0);   // (back half way: from where it is, no jump down or up first)
    CHECK(a.focus >= 0.49f);
    a.step(t + 8 * kFrameNs, true, 0);
    CHECK(near(a.focus, 1));
    a.step(t + 9 * kFrameNs, false, 0);
    a.step(t + 17 * kFrameNs, false, 0);
    CHECK(near(a.focus, 0));
    CHECK(near(a.loop(a.since_ns + 20 * kFrameNs), 0.5f) && near(a.loop(a.since_ns + 40 * kFrameNs), 0.6f));
    // ((11-19) a game frame late: 70 ms with no report, then reports again: the entry stays, no blink)
    a.step(t + 18 * kFrameNs, false, 70000000);
    CHECK(near(a.shown, 1));
    a.step(t + 19 * kFrameNs, false, 0);
    CHECK(near(a.shown, 1));
    // the list gone: shown on for 150 ms, then down over 8 frames
    a.step(t + 20 * kFrameNs, false, kListGoneNs - kFrameNs);
    CHECK(near(a.shown, 1));
    a.step(t + 21 * kFrameNs, false, kListGoneNs + 4 * kFrameNs);
    CHECK(near(a.shown, 0.5f));
    a.step(t + 22 * kFrameNs, false, kListShownNs);
    CHECK(near(a.shown, 0));
    a.step(t + 23 * kFrameNs, false, 0);   // (and back: rises again, from nothing as it is)
    CHECK(near(a.shown, 0));
    a.step(t + 30 * kFrameNs, false, 0);
    CHECK(a.shown > 0.1f && a.shown < 0.9f);
    {   // (gone half way and back: on from where it was, not from nothing)
        Anim b;
        b.step(t, false, 0), b.step(t + 20 * kFrameNs, false, 0);
        b.step(t + 21 * kFrameNs, false, kListGoneNs + 4 * kFrameNs);
        CHECK(near(b.shown, 0.5f));
        b.step(t + 22 * kFrameNs, false, 0);
        CHECK(near(b.shown, 0.5f, 0.03f));
        b.step(t + 40 * kFrameNs, false, 0);
        CHECK(near(b.shown, 1));
    }
    // Decide
    const Decide none = decide_at(-1), mid = decide_at(12 * kFrameNs), end = decide_at(30 * kFrameNs);
    CHECK(near(none.deco, 1) && near(none.glow, 1) && near(none.flash, 0));
    CHECK(mid.deco < 1 && mid.flash > 0 && near(end.deco, 0.85f) && near(end.glow, 0.95f) && near(end.flash, 1));
    // the band and the sparkle from made-up masks: the shape whole, then whole with the gradient, then none
    using namespace mkwvk::game_tex;
    auto mask = [](std::initializer_list<int> v) {
        Image im;
        im.w = (uint32_t)v.size(), im.h = 1, im.mask = true;
        for (int x : v) im.rgba.insert(im.rgba.end(), {255, 255, 255, (unsigned char)x});
        return im;
    };
    Image band, sparkle;
    make_title_band(mask({255, 255, 0}), mask({0, 255, 255}), mask({200, 100, 50, 25}), &band, &sparkle);
    CHECK(band.w == 3 && band.h == 1 && !band.mask && sparkle.mask);
    CHECK(band.rgba[0] == 7 && band.rgba[1] == 4 && band.rgba[2] == 4 && band.rgba[3] == 245);   // (0.96)
    CHECK(band.rgba[7] == 147);   // (0.96 x 0.6 where the gradient is whole)
    CHECK(band.rgba[11] == 0);    // (outside the shape)
    CHECK(sparkle.rgba[3] == 200 && sparkle.rgba[7] == 25 && sparkle.rgba[11] == 0);   // (the noise 3 texels a texel along, in the shape)
    // ((Triforce, 11-19) the lab's "much weaker dark shadow than the game's entries ... its glow is fainter than the game's
    // Options glow": the dark edge and the glow as fitted to the game's own entries (text_spread.h). Out from a stroke's
    // edge, in units (3 a pixel of the lab's 720-line shots): the edge dark at 6 units, gone by 15; the glow strong at 9,
    // still there at 21, gone by 33. Before: one shadow copy (no edge at the sides at all), the glow gone at 15)
    using text_spread::kGlow;
    using text_spread::kLookHdr;
    using text_spread::kLookSdr;
    const text_spread::Spread& kShadow = kLookHdr.shadow;
    CHECK(spread_at(kGlow, 3) > 0.85f && spread_at(kGlow, 9) > 0.6f && spread_at(kGlow, 21) > 0.1f && spread_at(kGlow, 33) < 0.01f);
    // ((11-19, a review of 3d1a417) what is drawn is the fit: each copy's alpha cut to a whole step of 255 drew the shadow
    // at 0.313 for 0.362 at 6 units and the glow at 0.115 for 0.177 at 21, nothing at 27; and as the entry fades in)
    for (float amount : {1.0f, 0.6f, 0.25f})
        for (float d = 0; d <= 30; d += 3) {
            const float gl = spread_px(kGlow, d, amount, true);
            for (const text_spread::Spread* sp : {&kLookHdr.shadow, &kLookSdr.shadow}) {
                const float sh = spread_px(*sp, d, amount, true);
                CHECK(near(spread_px(*sp, d, amount, false), sh, 0.01f + 0.04f * sh));
            }
            CHECK(near(spread_px(kGlow, d, amount, false), gl, 0.01f + 0.04f * gl));
        }
    CHECK(spread_at(kGlow, 27) > 0.02f && text_spread::ring_drawn(kGlow, 9, 1).alpha == 1 && text_spread::ring_drawn(kGlow, 9, 1).copies == 48 &&
          text_spread::ring_drawn(kGlow, 8, 1).copies == 77);   // (the outer rings at 1 of 255, as many copies as their 0.77 and 1.36)
    CHECK(text_spread::centre_drawn(kGlow, 1) == 255 && text_spread::ring_drawn(kGlow, 0, 0).alpha == 0);
    CHECK(text_spread::ring_copies(kGlow, 9) == 63 && text_spread::ring_copies(kShadow, 0) == 8);   // (a copy about every 3 units)
    CHECK(near(text_spread::ring_alpha(kGlow, 0), 18.01f / 255, 0.001f) && near(kGlow.down, 0) && near(kGlow.centre, 1));
    // (the chosen text: kTextWeight times over itself, each at weight_alpha)
    CHECK(text_spread::kTextWeight == 2 && near(text_spread::weight_alpha(1), 1, 1e-6f) && near(text_spread::weight_alpha(0), 0, 1e-6f));
    for (float a : {0.1f, 0.5f, 0.9f})   // (fading: the text whole where it is whole, at the fade's alpha)
        CHECK(near(1 - std::pow(1 - text_spread::weight_alpha(a), (float)text_spread::kTextWeight), a, 1e-4f));
    // ((Triforce, 11-20, lab check 3) the text not chosen per output: HDR (the shots' PQ capture) and SDR (8-bit code values)
    // have their own black copies, the SDR ones darker each (about 1 - (1 - a)^2 of HDR's: the lab's "edge much too weak ...
    // Mods reads pale" with HDR off); the same places (the core 2.55 units down, 14 times; the soft edge's rings)
    CHECK(&text_spread::look_of(true) == &kLookHdr && &text_spread::look_of(false) == &kLookSdr);
    CHECK(kLookHdr.core.repeat == kLookSdr.core.repeat && near(kLookHdr.core.down, kLookSdr.core.down) && kLookHdr.shadow.rings == kLookSdr.shadow.rings &&
          near(kLookHdr.shadow.down, kLookSdr.shadow.down) && near(kLookHdr.shadow.sigma, kLookSdr.shadow.sigma));
    CHECK(text_spread::centre_drawn(kLookSdr.core, 1) > text_spread::centre_drawn(kLookHdr.core, 1) + 40 &&
          text_spread::ring_drawn(kLookSdr.shadow, 0, 1).alpha > text_spread::ring_drawn(kLookHdr.shadow, 0, 1).alpha);
    // ((Triforce, 11-20, a review of cb644b1) the hard shadow fades with the text: where the text is solid its copies
    // together are the fade's amount of their full darkness, as the text's alpha is; each at amount x 0.45 they stacked to
    // 0.98 black at 0.5 and 0.86 at 0.25 (the list rising, going, UnSelect over the blue glow). Each copy a whole step of 255.
    // ((11-20, lab check 3) and the text's own copies, in place and out across, together the fade's amount where it is whole
    // (each_weight_copy): no brighter step through the fades, at every size and in both outputs)
    for (const text_spread::Look* look : {&kLookHdr, &kLookSdr}) {
        const text_spread::Spread& core = look->core;
        const int n = core.repeat;
        auto core_dark = [&](float amount) { return 1 - std::pow(1 - (float)text_spread::centre_drawn(core, amount) / 255, (float)n); };
        const float full = core_dark(1);
        CHECK(full > 0.995f);
        for (float a : {0.1f, 0.25f, 0.5f, 0.6f, 0.9f}) {
            if (getenv("TITLE_ENTRY_DEBUG")) std::printf("core at %.2f: %d of 255 a copy, %.3f dark\n", a, text_spread::centre_drawn(core, a), core_dark(a));
            CHECK(near(core_dark(a), a * full, 0.025f));
            for (float lines : {1440.0f, 1050.0f, 720.0f, 674.0f}) {
                CHECK(near(text_whole(text_of(look->weight, lines, a)), a, 0.012f));   // (the text's)
                CHECK(near(core_dark(a), text_whole(text_of(look->weight, lines, a)), 0.03f));
            }
            int same = 0;
            text_spread::each_copy(core, a, [&](float dx, float dy, int q) { same += dx == 0 && near(dy, core.down) && q == text_spread::centre_drawn(core, a); });
            CHECK(same == n);
        }
        CHECK(text_spread::centre_drawn(core, 0) == 0 && text_of(look->weight, 720, 0).empty());
        for (float lines : {1440.0f, 720.0f}) CHECK(near(text_whole(text_of(look->weight, lines, 1)), 1, 1e-6f));
    }
    CHECK(text_spread::centre_drawn(kGlow, 0.5f) == 128 && text_spread::centre_drawn(kGlow, 0.25f) == 64);   // (one copy: as before)
    // ((Triforce, 11-20, a review of cb644b1) the text chosen at the game's weight too (the same font and size as the unchosen
    // text, T_Btn_02): the strokes as solid in units at 720 and 674 lines as at 1440; drawn once, 1 whole pixel at 720 lines)
    {
        const float full = solid_units(1440, text_spread::kTextWeight);
        for (float h : {720.0f, 674.0f}) {
            if (getenv("TITLE_ENTRY_DEBUG"))
                std::printf("%4.0f lines, chosen: solid %.2f units drawn %d times, %.2f once (1440: %.2f)\n", h, solid_units(h, text_spread::kTextWeight),
                            text_spread::kTextWeight, solid_units(h, 1), full);
            CHECK(solid_units(h, text_spread::kTextWeight) >= full - 0.05f);
            CHECK(solid_units(h, 1) < full - 1);
        }
    }
    {   // ((Triforce, 11-20, lab check 3) the cost: copies of "Mods" a frame, not chosen (the chosen glow's are as before)
        for (const text_spread::Look* look : {&kLookHdr, &kLookSdr}) {
            int n = 0;
            for (const text_spread::Spread* sp : {&look->core, &look->shadow}) text_spread::each_copy(*sp, 1, [&](float, float, int) { n++; });
            const int text = (int)text_of(look->weight, 1440, 1).size();
            if (getenv("TITLE_ENTRY_DEBUG")) std::printf("%s: %d black copies and %d of the text a frame\n", look == &kLookHdr ? "HDR" : "SDR", n, text);
            CHECK(n <= 165 && text <= 4);
            int gone = 0;
            text_spread::each_copy(look->shadow, 0, [&](float, float, int) { gone++; });   // (gone with the entry)
            CHECK(gone == 0);
        }
    }
    {   // ((Triforce, 11-20, lab check 3) the shapes the lab ran (shots052c), each against the game's own New Game, Options
        // and Continue in the same shots: fullscreen 3440x1440 (1440 lines), the 1680x1050 window with a 16:10 picture
        // (1050 lines, squeezed across by 0.9), the 1720x720 window (720), the 1600x900 window with the 21:9 picture (674),
        // all HDR, and the 1720x720 window with HDR off. 26670c0's look: the text twice in place, a hard shadow of 16 copies of
        // 115 2.5 units down and a soft edge of 9.49 (HDR's fit, in SDR too)
        const text_spread::Spread core0{0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0, 115.0f / 255.0f, 2.5f, 16};
        const text_spread::Spread shadow0{9.49f / 255.0f, 10.9f, 1.2f, 1.5f, 2.3f, 7, 0.0f, 1.75f};
        const std::vector<Copy> black0 = copies_of({core0, shadow0}), text0 = {{0, 0, 255}, {0, 0, 255}};
        // (the lab's numbers: across a stroke, the mean of the runs of pixels at 0.72 of the text's brightness and over, the
        // game's three entries' (low, high) and 26670c0's Mods in the same shots)
        struct Cond { const char* name; float lines, kx; bool hdr; float game_lo, game_hi, mods; };
        const Cond conds[] = {{"3440x1440", 1440, 1, true, 4.28f, 4.61f, 3.51f}, {"1680x1050 16:10", 1050, 1680.0f / 1867.0f, true, 2.60f, 2.79f, 2.44f},
                              {"1720x720", 720, 1, true, 2.03f, 2.08f, 1.82f}, {"1600x900 21:9", 674, 1, true, 2.00f, 2.06f, 1.54f},
                              {"1720x720 SDR", 720, 1, false, 1.68f, 1.80f, 1.84f}};
        Shape now[5], before[5];
        for (int i = 0; i < 5; i++) {
            const Cond& c = conds[i];
            const text_spread::Look& look = text_spread::look_of(c.hdr);
            now[i] = shape_avg(c.lines, c.kx, c.hdr, copies_of({look.core, look.shadow}), text_of(look.weight, c.lines));
            before[i] = shape_avg(c.lines, c.kx, c.hdr, black0, text0);
            if (getenv("TITLE_ENTRY_DEBUG"))
                std::printf("%-16s now: solid %.2f px, darkest %.3f, %.3f %.3f %.3f at 3, 6, 9 units; 26670c0: %.2f, %.3f, %.3f %.3f %.3f\n", c.name,
                            now[i].solid, now[i].darkest, now[i].at3, now[i].at6, now[i].at9, before[i].solid, before[i].darkest, before[i].at3,
                            before[i].at6, before[i].at9);
            // (the strokes: as the lab would measure them, 26670c0's Mods times the model's gain over 26670c0. In HDR the game's
            // are bolder than 26670c0's in every shape (by 26% at 1440 lines, 31% at 674): at least half of that, and no more
            // than 0.15 of a pixel past the game's. With HDR off the game's are 1.74 for 26670c0's 1.84: no wider)
            const float gain = now[i].solid / before[i].solid, as_shot = c.mods * gain;
            if (c.hdr) {
                const float short_by = (c.game_lo + c.game_hi) * 0.5f / c.mods;
                if (getenv("TITLE_ENTRY_DEBUG")) std::printf("  strokes %.2f as shot (the game's %.2f-%.2f), %.0f%% bolder for the game's %.0f%%\n", as_shot, c.game_lo, c.game_hi, 100 * (gain - 1), 100 * (short_by - 1));
                CHECK(gain - 1 >= 0.5f * (short_by - 1));
                CHECK(as_shot <= c.game_hi + 0.15f && as_shot >= c.game_lo - 0.25f);
            } else
                CHECK(gain <= 1.0f);
        }
        // (the edge, as the lab saw it. The 1720x720 window: "a hard, near-black drop shadow under the strokes, darker and
        // crisper than New Game's soft halo", 3 units out 0.37-0.53 dark for New Game's 0.20-0.42 (the 1600x900 window and
        // 16:10 alike): softer there now, still near black next to the strokes. Fullscreen: "the edge is OK": as it was)
        for (int i : {1, 2, 3}) CHECK(now[i].at3 <= before[i].at3 - 0.03f && now[i].darkest >= 0.8f);
        CHECK(std::fabs(now[0].at3 - before[0].at3) <= 0.03f && std::fabs(now[0].at6 - before[0].at6) <= 0.03f && now[0].darkest >= 0.99f);
        for (int i : {0, 1, 2, 3}) CHECK(std::fabs(now[i].at6 - before[i].at6) <= 0.03f && now[i].at9 <= 0.34f);
        // (HDR off: "the edge much too weak (6 units 0.09-0.31 dark for New Game's 0.41-0.53; 3 units -0.11-0.30 for 0.08-0.40),
        // Mods reads pale": in 8-bit code values the black copies darken a third as much as the HDR shots show them. Now as the
        // HDR picture's edge at 720 lines, near black next to the strokes)
        CHECK(now[4].at6 >= before[4].at6 + 0.15f && now[4].at3 >= before[4].at3 + 0.15f && now[4].darkest >= 0.88f && before[4].darkest < 0.8f);
        CHECK(std::fabs(now[4].at6 - now[2].at6) <= 0.05f && std::fabs(now[4].at9 - now[2].at9) <= 0.05f);
    }
    if (!fails) std::puts("the title entry's curves, state and band");
    return fails ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    tex = (VK / "vk_game_tex.h").read_text(encoding="utf-8")
    imgui = (VK / "vk_imgui.cpp").read_text(encoding="utf-8")
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    host = (ROOT / "host" / "oot_menu.c").read_text(encoding="utf-8")

    # the textures: the title archive's, after the menu's own; one missing keeps the skin
    art = function(imgui, "void art_thread() {")
    check('"UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.bntx.zs"' in art and "game_tex::make_title_skin(std::move(bytes), skin_)" in art,
          "the title list's textures are not read")
    check(art.index("make_skin(") < art.index("make_title_skin(") and "art_state.store(ok || title ? 2 : 4" in art,
          "the title's textures are not read after the menu's, or one set missing throws the other away")
    skin = function(tex, "inline bool make_title_skin(")
    for name in ("CtrlGuideBG_00", "CtrlGuideBGGrd_00", "DecideNoise_01", "TitleBtnDeco_00", "TitleBtnDecoGlow_00"):
        check('"%s"' % name in skin, "make_title_skin does not read " + name)
    check(skin.index("return false") < skin.index("skin.images.push_back"), "make_title_skin changes the skin before it has every texture")

    # the drawing: the part's look, the layout's colours, no pulse
    draw = function(menu, "void draw_title_mods(")
    for part in ('ui::art("title_band")', 'ui::art("title_decide")', 'ui::art("TitleBtnDeco_00")', 'ui::art("TitleBtnDecoGlow_00")',
                 "title_anim.step(now, sel,", "title_anim.loop(now)", "side < 0);", "mods_entry::decide_at(",
                 "kTitleText * ui::kType * k"):
        check(part in draw, "draw_title_mods does not draw " + part)
    check("c.pulse" not in draw, "the title's entry still pulses with the options' cursor, not the game's LoopRandom")
    # ((11-19) the dark edge and the glow: the text's own shape spread, text_spread.h's rings, under the text)
    check('#include "text_spread.h"' in menu and "spread(look.shadow, ui::rgba(0, 0, 0), a);" in draw and
          "spread(text_spread::kGlow, kTitleGlow, f * show);" in draw, "the title entry's shadow and glow are not the game's")
    # ((Triforce, 11-20, lab check 3) not chosen: the look of the window's output (HDR10 or SDR), its core and soft edge, then
    # the text's weight copies; the chosen text over its glow as before)
    unsel_text = "text_spread::each_weight_copy(look.weight, ui::lu(c, 2160 * k), a,"
    check("const text_spread::Look& look = text_spread::look_of(dev.hdr);" in draw,
          "the title entry not chosen is drawn with the HDR fit whatever the output (with HDR off its edge a third as dark)")
    check(draw.index("spread(look.core, ui::rgba(0, 0, 0), a);") < draw.index("spread(look.shadow, ui::rgba(0, 0, 0), a);") < draw.index(unsel_text) and
          draw.index("spread(text_spread::kGlow, kTitleGlow, f * show);") <
          draw.index("for (int i = 0; i < text_spread::kTextWeight; i++) c.d->AddText(c.f.body, px, at, ink_sel, label);"),
          "the title entry's text is not over its shadow and glow")
    # ((11-20, a review of cb644b1) the chosen text at the game's weight too: drawn kTextWeight times, each at weight_alpha)
    check("const ImU32 ink_sel = ui::alpha(kTitleInkSel, text_spread::weight_alpha(f * show));" in draw and
          "ui::alpha(kTitleInkSel, f * show), label);" not in draw,
          "the chosen title entry's text is drawn once, thinner than the game's chosen Continue and Options (and than Mods not chosen)")
    check("text_spread::each_copy(sp, amount * own," in draw and "ui::lu(c, dx * k)" in draw and "ui::lu(c, dy * k)" in draw,
          "the copies are not drawn as text_spread.h has them (whole steps of 255, the copies keeping the coverage)")
    # ((11-20) the lab's "paler and greyer ... its dark edge weaker" in the windows: the text at the game's weight over the
    # hard shadow, each copy at its own offset from the text's whole pixel (not snapped: an offset under a pixel went to 0 or 1))
    # ((Triforce, 11-20, lab check 3) the weight by the text's size, its copies across at their offsets (a part of a unit,
    # not snapped), in the layout's cream at each copy's alpha
    weight = draw[draw.index(unsel_text):]
    weight = weight[:weight.index("});") + 3]
    check("c.d->AddText(c.f.body, px, ImVec2(at.x + ui::lu(c, dx * k), at.y + ui::lu(c, dy * k)), ink_rgb | (ImU32)q << IM_COL32_A_SHIFT, label);" in weight and
          "const ImU32 ink_rgb = kTitleInk & ~IM_COL32_A_MASK;" in draw and
          draw.rindex("c.d->Flags |= ImDrawListFlags_TextNoPixelSnap;", 0, draw.index(unsel_text)) > draw.index("spread(look.shadow, ui::rgba(0, 0, 0), a);"),
          "the title entry's text is not drawn at the game's weight (text_spread::each_weight_copy, its copies across not snapped)")
    check("const ImVec2 at(std::floor(m.x - tw * 0.5f), std::floor(ui::mid_top(c.f.body, px, ty)));" in draw and
          "c.d->Flags |= ImDrawListFlags_TextNoPixelSnap;" in draw and "c.d->Flags = flags;" in draw,
          "the shadow's and glow's copies are still snapped to whole pixels each, not put at their offsets from the text")
    check("ui::alpha(col, text_spread::ring_alpha" not in draw, "a ring's alpha is still cut to a whole step of 255 (ui::alpha)")
    check("constexpr float kTitleText = 65 * 0.8f," in menu, "the title entry's text is not the layout's 65 x 0.8 units")
    check("kTitleInk = ui::rgba(255, 254, 228)" in menu and "kTitleDecoInk = ui::rgba(131, 115, 22)" in menu,
          "the title entry's colours are not the layout's materials' (T_Btn_01, P_Deco_*_00)")
    # ((Triforce, 11-20, lab check 3: "in 16:10 Mods is not squeezed across") the entry, its copies with it, put on the game's
    # grid as the game's panes: squeezed across by the grid's kx (0.9 at 16:10) wherever it is drawn, the menu shut or open
    shut = function(menu, "void draw_mods_row_shut(ui::Ctx& c) {")
    check("ui::Ctx k = grid_ctx(c, g);" in shut and "put_on_grid(c.d, vtx0, cmd0, game_grid::map_of(g), window_clip);" in shut,
          "the title entry with the menu shut is not put on the game's grid (squeezed across on a 16:10 picture)")
    dm = function(menu, "void draw_menu(ui::Ctx& c, bool perf_under) {")
    check("k = grid_ctx(c, g), k.over_game = false;" in dm and "menu_map = game_grid::map_of(g);" in dm and
          "put_on_grid(c.d, vtx0, cmd0, menu_map, window_clip," in dm,
          "the title entry chosen is not drawn on the game's grid")
    grid = (VK / "game_grid.h").read_text(encoding="utf-8")
    check("m.kx = cw > 0 ? g.w / cw : 1.0f;" in grid and "float x(float px) const { return ox + px * kx; }" in grid,
          "the game's grid narrower than 16:9 is not squeezed across")
    row = function(menu, "void draw_mods_row(ui::Ctx& c, bool sel) {")
    check("draw_title_mods(c, sel, cx, cy, k, pitch);" in row and "(x1 - x0) / 60.0f" in row,
          "draw_mods_row does not draw the title's entry as the part, at the list's scale")

    # the input: Decide before the Mods screen, its sound once at the press
    tick = function(menu, "void menu_tick() {")
    check("else if (mods_title_list()) mods_decide_ns = prof_now(), mods_sound(HOS_MENU_SOUND_SYS_DECIDE), changed = true;" in tick,
          "A on the title's entry does not play Decide with the system menu's sound")
    check("const bool decide = enter == 2 && mods_title_list();" in tick and "if (decide) mods_decide_ns = prof_now();" in tick,
          "a click on the title's entry does not play Decide")
    decide = tick[tick.index("page == kModsRowPage && mods_decide_ns) {"):]
    decide = decide[:decide.index("} else if (open_now.load() && page == kModsRowPage) {")]
    check("prof_now() - mods_decide_ns >= mods_entry::kDecideNs" in decide and "page = kModsPage" in decide and
          "mods_decided = true" in decide and "press[" not in decide, "the Mods screen does not open after Decide, or input is taken meanwhile")
    check("if (!mods_decided) mods_sound(HOS_MENU_SOUND_SYS_DECIDE);" in tick and "mods_decided = false;" in tick,
          "the decide sound plays again when the Mods screen opens after Decide")
    check("my < e[1] && my > e[1] - (quit_here ? 6 : 4) * e[4]) mods_row_sel = 0, press[1] = true;" in tick,
          "the mouse moving up off the title's entry does not give the cursor back")
    check(tick.count("(mods_list_fresh() || mods_title_fading())") == 0 and
          menu.count("mods_row = !open && !start && (mods_list_fresh() || mods_title_fading());") == 1 and
          menu.count("mods_row = !open && (mods_list_fresh() || mods_title_fading());") == 1,
          "the title's entry is not drawn on while it fades after its list (menu_record, menu_record_image)")

    # the host: the entry taken across the line; the mouse onto it takes the cursor
    check("mods_entry = (Rect){mx - 550, my - pitch / 2, mx + 550, my + pitch / 2};" in host,
          "the title's Mods entry is not taken across the line as the game's rows are")
    check("const int onto = mods_entry_hit(hit.x, hit.y) ? 1 : quit_entry_hit(hit.x, hit.y) ? 4 : 0;" in
          function(host, "static void mouse_tick_locked(Ctx *c, uint64_t screen) {"),
          "the mouse onto the title's Mods entry does not take the cursor")

    with tempfile.TemporaryDirectory(prefix="mods-title-entry-") as tmp:
        src, exe = Path(tmp) / "entry.cpp", Path(tmp) / "entry.exe"
        src.write_text(PROGRAM, encoding="utf-8")
        inc = [VK, ROOT / "gpu" / "third_party" / "suyu", VK / "compat", ROOT / "gpu" / "compat"]
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-I" + str(VK), *("-isystem" + str(p) for p in inc[1:]),
                        str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        check(r.returncode == 0, "the title entry as made:\n" + r.stdout)
    print("PASS: the Mods entry under the title's list: the game's look, its animations, Decide, the mouse")


if __name__ == "__main__":
    main()
