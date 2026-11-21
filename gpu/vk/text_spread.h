// (Triforce, 11-19) The title list's text as the game draws it round its entries (PaCommonBtnTitle_00, the Title archive's
// layout): not chosen, T_BtnSh_00, the text in black (the layout's colour 3) 2 units under it, spread soft into a dark edge;
// chosen, P_BtnGlow_00, the capture C_Btn_00 of the text (T_Btn_00) blurred, under the white text (T_Btn_02) in blue. Their
// materials are the font's effect (an extension of 28 bytes where the plain text's has 24: T_BtnSh_00's 70 and 30,
// T_Btn_00's 40), which the overlay cannot read as the game does; ImGui has no blur either. So the text's own shape is
// drawn again round it, ring after ring, each copy a little seen (the copies' alpha falls off as a Gaussian's), and the rings
// were fitted to the game's own entries on the lab's shots (runs/port052, 720 picture lines: 3 units a pixel) with the
// text's mask taken from the game's own "Continue": the darkness of New Game's and Options' edges at 1 to 8 pixels from their
// strokes (0.47 at 2, 0.27 at 3, 0.13 at 4, 0.05 at 5) and the blue of the chosen Continue's and Options' glow (0.93 at
// 1, 0.67 at 3, 0.40 at 5, 0.18 at 7, 0.01 at 9). The entry had one shadow copy at 150 (no edge to be seen) and four
// rings out to 15 units (gone at 2 pixels): the lab's "much weaker dark shadow ... its glow is fainter".
// ((Triforce, 11-20) The text not chosen fitted again. The lab's second check (shots052b): "paler and greyer than New Game
// over the bright wheat, its dark edge weaker" in the 1720x720 and 1600x900 windows (the darkest 2% of pixels round it 34-41
// of 255, New Game's 19-21), close in the 3440x1440 fullscreen. The fit above took a stroke for a half plane, where a ring
// copy far out still covers the place; beside a thin stroke it covers it only where the stroke is, so the edge drawn was
// weaker near the strokes and broader than fitted. And the game's text is bolder than ImGui's Cetra Display: its strokes 6
// units wide in the fullscreen and both windows, ours 4.5; at 720 lines that is 1 whole pixel with grey ones beside it where
// the game's have 2. It sits on a near black shadow tight along the strokes (the darkest pixels beside New Game 0.93-0.96
// dark in the shots, ours 0.75-0.80 at every size). At 1440 lines the 1.5 units are a pixel of 3 and the text passed; at 720
// or 674 they are half of its whole pixels: the greyer text. Fitted now with the game's own font drawn by ImGui as the game
// draws it, in a model of the drawing (the copies, their 8-bit alpha, and the shots' HDR capture, where a black copy of alpha
// a darkens 1-(1-a)^2, read from the old Mods in the same shots), to New Game's and Options' edges in the fullscreen's and
// both windows' shots at once: the text drawn twice over itself (kTextWeight), a hard shadow (the core: the text in black
// 2.5 units down, 16 copies of 0.45, so that its soft edges fill out as the game's outlined T_BtnSh_00's) and the soft edge
// round both (the shadow). The distance from New Game's edge at 1440, 720 and 674 lines: 0.28, 0.27, 0.37 before; 0.06, 0.03,
// 0.07 now. The copies are put at their offsets from the text's whole-pixel place, not each snapped to a pixel (vk_menu.cpp
// draw_title_mods): the core's 2.5 units are 0.83 of a pixel at 720 lines, cut to 0 or 1 by where the entry happened to be.
// ((Triforce, 11-20, lab check 3, shots052c) Fitted again, per output, in the five shapes the lab ran: fullscreen 3440x1440,
// the 1720x720 window, the 1600x900 window (a 21:9 picture of 674 lines), the 1680x1050 window with a 16:10 picture (the
// layouts squeezed across by 0.9) and the 1720x720 window with HDR off. What the lab saw: the strokes thinner than New
// Game's (mean 3.5 pixels across for 4.3-4.6 at 1440 lines, 1.5 for 2.0 at 674), the hard shadow too dark and crisp at 720
// (its darkest 2% 0.0-0.9 of 255, New Game's 1.2-16.6), and with HDR off the edge a third of the game's (0.09-0.31 dark 6
// units out for 0.41-0.53). The text drawn twice in place has 2 copies' coverage at every size; the game's text, drawn at
// 1440 lines and scaled to the window, is bolder in units at 1440 than at 720. So the weight is two copies in place and
// two more either side across (Weight: their offset and alpha by the text's size in pixels: 0.87 units out at 0.88 of
// alpha at 1440 lines, 0.76 at 0.43 at 720, 0.41 of alpha at 674), the hard shadow softer at 720, and with HDR off
// (kLookSdr) every black copy as dark in 8-bit code values as the HDR picture shows it (the overlay blends there in PQ
// code values, where a black copy of alpha a darkens about 1 - (1 - a)^2; in SDR about a), the text no wider.
// No ImGui: tests/test_mods_title_entry.py compiles it alone.
#pragma once
#include <cmath>

namespace text_spread {

struct Spread {
    float a;         // (a copy's alpha at the middle, of 1)
    float sigma;     // (units: the copies' alpha exp(-(r / sigma)^power))
    float power;
    float r0, dr;    // (units: the first ring and the step)
    int rings;
    float centre;    // (a copy under the text itself, its alpha)
    float down;      // (units: the whole of it under the text by this)
    int repeat = 1;  // ((11-20) the copy under the text drawn this many times)
};
/// P_BtnGlow_00's: the glow's blue (kTitleGlow)
constexpr Spread kGlow{18.85f / 255.0f, 16.0f, 1.85f, 3.0f, 3.0f, 10, 1.0f, 0.0f};
/// ((11-20) the chosen text drawn this many times over itself: the game's strokes' weight (ImGui's Cetra Display is 1.5 units
/// thinner; a pixel the text covers c of shows 1 - (1 - c)^2). (Lab check 3: the text not chosen has its own, Look's weight)
constexpr int kTextWeight = 2;
/// (the alpha of each of those draws for the text at alpha a: a where the text is whole)
inline float weight_alpha(float a, int weight = kTextWeight) {
    return a >= 1 ? 1.0f : a <= 0 ? 0.0f : 1.0f - std::pow(1.0f - a, 1.0f / (float)weight);
}

inline float ring_radius(const Spread& s, int i) { return s.r0 + s.dr * (float)i; }
inline float ring_alpha(const Spread& s, int i) { return s.a * std::exp(-std::pow(ring_radius(s, i) / s.sigma, s.power)); }
/// (the copies of a ring: about one every 3 units round it, 8 at least)
inline int ring_copies(const Spread& s, int i) {
    const int n = (int)std::ceil(2.0f * 3.14159265f * ring_radius(s, i) / 3.0f);
    return n < 8 ? 8 : n;
}

/// ((Triforce, 11-19) A ring as it is drawn: its copies' alpha a whole step of 255 (an 8-bit colour's), and as many copies as
/// keep the ring's coverage (copies x alpha) the fit's. Its alpha is only 1 to 19 of 255, so cut to a whole step it lost a
/// large share (the outer glow rings to 0, not drawn: the lab's weak edge and faint glow again), and the more while the
/// entry fades in (amount under 1). amount: the entry's fade times the colour's own alpha (of 1)
struct Drawn {
    int alpha;   // (of 255; 0 none drawn)
    int copies;
};
inline Drawn ring_drawn(const Spread& s, int i, float amount) {
    const float a = ring_alpha(s, i) * amount * 255.0f;
    const int n = ring_copies(s, i);
    if (!(a > 0)) return {0, 0};
    int q = (int)std::lround(a);
    q = q < 1 ? 1 : q > 255 ? 255 : q;
    return {q, (int)std::lround((float)n * a / (float)q)};
}
/// (the copy under the text itself: its alpha rounded to a whole step; drawn s.repeat times)
/// ((Triforce, 11-20) drawn more than once (a Look's core), the copies' darkness together is amount times theirs at full
/// strength, as the text's own alpha is amount (weight_alpha): each copy 1 - (1 - amount C)^(1 / repeat), C = 1 - (1 - a)^repeat
/// with a the full copy's whole step. A review of cb644b1: each at amount x 0.45 stacked 16 times to 1 - (1 - 0.45 amount)^16,
/// 0.98 black under the text at 0.5, 0.86 at 0.25: "Mods" a near black word as the list rose, the core left after the text
/// as it went, and the blue glow darkened through UnSelect; the game's T_BtnSh_00 fades with its pane)
inline int centre_drawn(const Spread& s, float amount) {
    long q = std::lround(s.centre * amount * 255.0f);
    if (s.repeat > 1 && amount > 0 && amount < 1) {
        const float full = (float)std::lround(s.centre * 255.0f) / 255.0f;
        const float dark = 1.0f - std::pow(1.0f - full, (float)s.repeat);
        q = std::lround((1.0f - std::pow(1.0f - amount * dark, 1.0f / (float)s.repeat)) * 255.0f);
    }
    return q < 0 ? 0 : q > 255 ? 255 : (int)q;
}

/// ((11-20) every copy of a spread as draw_title_mods draws it: f(dx, dy, alpha of 255), dx and dy in units from the text's
/// place (the caller takes them to pixels), the copies under the text first)
template <class F>
inline void each_copy(const Spread& s, float amount, F&& f) {
    if (const int a = s.centre > 0 ? centre_drawn(s, amount) : 0)
        for (int n = 0; n < s.repeat; n++) f(0.0f, s.down, a);
    for (int i = 0; i < s.rings; i++) {
        const Drawn dr = ring_drawn(s, i, amount);
        if (!dr.alpha || !dr.copies) continue;
        const float r = ring_radius(s, i);
        for (int j = 0; j < dr.copies; j++) {
            const float ang = 6.2831853f * (float)j / (float)dr.copies;
            f(std::cos(ang) * r, s.down + std::sin(ang) * r, dr.alpha);
        }
    }
}

/// ((Triforce, 11-20, lab check 3) The text not chosen as its weight: `same` copies at its own place, `wide` copies round it
/// (2: either side across) `widen` units out and `wide_a` of alpha, both by the text's size: lines, the picture's lines
/// times the list's scale (2160 units). (widen x (lines / 1080)^widen_pow, wide_a x (lines / 720)^wide_pow, 1 at most)
struct Weight {
    int same;
    int wide;
    float widen, widen_pow;
    float wide_a, wide_pow;
};
inline float weight_widen(const Weight& w, float lines) { return lines > 0 ? w.widen * std::pow(lines / 1080.0f, w.widen_pow) : 0.0f; }
inline float weight_wide_a(const Weight& w, float lines) {
    const float a = lines > 0 ? w.wide_a * std::pow(lines / 720.0f, w.wide_pow) : 0.0f;
    return a < 0 ? 0.0f : a > 1 ? 1.0f : a;
}
/// (every copy of the text as draw_title_mods draws it at amount (its fade, of 1): f(dx, dy, alpha of 255), dx and dy in
/// units. Each copy's alpha the full one's times a scale g, found so that where the text is whole the copies together are
/// amount opaque, as the text once at alpha amount ((1 - g)^same (1 - g wide_a)^wide = 1 - amount): no brighter step
/// through the fades)
template <class F>
inline void each_weight_copy(const Weight& w, float lines, float amount, F&& f) {
    if (!(amount > 0)) return;
    const float wa = w.wide ? weight_wide_a(w, lines) : 0.0f, r = weight_widen(w, lines);
    float g = 1;
    if (amount < 1) {
        float lo = 0, hi = 1;
        for (int i = 0; i < 24; i++) {
            g = (lo + hi) * 0.5f;
            const float through = std::pow(1 - g, (float)w.same) * std::pow(1 - g * wa, (float)w.wide);
            (1 - through < amount ? lo : hi) = g;
        }
        g = (lo + hi) * 0.5f;
    }
    const int same_a = (int)std::lround(g * 255.0f), wide_q = (int)std::lround(g * wa * 255.0f);
    for (int i = 0; i < w.same; i++)
        if (same_a > 0) f(0.0f, 0.0f, same_a > 255 ? 255 : same_a);
    for (int j = 0; j < w.wide && wide_q > 0; j++) {
        const float ang = 6.2831853f * (float)j / (float)w.wide;
        f(std::cos(ang) * r, std::sin(ang) * r, wide_q > 255 ? 255 : wide_q);
    }
}

/// ((Triforce, 11-20, lab check 3) the text not chosen, per output: T_BtnSh_00's hard shadow (core: the text in black under
/// it, repeat times, and a ring of 8 faint copies a third of a unit round that) and its soft edge (shadow), drawn in that order
/// before the text's weight copies
struct Look {
    Spread core;
    Spread shadow;
    Weight weight;
};
/// (HDR10 window images: the overlay in PQ code values. Fitted to New Game, Options and Continue at 1440, 1050 (16:10,
/// squeezed across), 720 and 674 lines at once)
constexpr Look kLookHdr{
    {15.9f / 255.0f, 1000.0f, 1.0f, 0.37f, 0.0f, 1, 107.6f / 255.0f, 2.55f, 14},
    {10.0f / 255.0f, 11.19f, 1.21f, 1.1f, 2.71f, 7, 0.0f, 4.0f},
    {2, 2, 0.82f, 0.2f, 0.434f, 1.02f},
};
/// (SDR window images: 8-bit code values. Fitted at 720 lines; the copies' alpha about 1 - (1 - a)^2 of HDR's, the text no
/// wider across (the game's own strokes are thinner in SDR: 1.74 pixels at 720 lines, 2.06 in HDR))
constexpr Look kLookSdr{
    {0.0f, 1000.0f, 1.0f, 0.37f, 0.0f, 0, 170.5f / 255.0f, 2.55f, 14},
    {21.1f / 255.0f, 11.19f, 1.21f, 1.1f, 2.71f, 7, 0.0f, 4.0f},
    {2, 0, 0.0f, 0.0f, 0.0f, 0.0f},
};
inline const Look& look_of(bool hdr) { return hdr ? kLookHdr : kLookSdr; }

}   // namespace text_spread
