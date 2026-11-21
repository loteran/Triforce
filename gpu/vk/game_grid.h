// (Triforce, 11-19) Where the game's 2D layouts are in the window: the 3840 x 2160 grid its menus and lists are laid out on
// (the units host/oot_menu.c tells the Mods entry's list in), as the window shows it. The lab's "Title in a 16:9 window (1600x900,
// 21:9 picture letterboxed): Mods drawn below the picture, larger, far from the list": the entry was drawn on a 16:9 column of
// the window's whole height, which is the grid only when the picture fills the window's height.
//   - The picture: in the window between black bars, as the present puts it (vk_present_rect.h present_rect, with the
//     picture's shape: Aspect ratio's 16:10, 21:9 or 32:9, else its pixels').
//   - The grid in it: the game's layouts keep their 16:9 shape in the picture's middle on a wider picture (host/oot_wide_hud.h:
//     every layout but the kept backgrounds and fades is squeezed by 16:9 over the picture's ratio, host/hos_main.c
//     hos_wide_squeeze), its height the picture's; on a narrower one (16:10) the grid is the whole picture, narrower than 16:9
//     (the layouts squeezed across, not cut). HUD position moves panes of the HUD's own layouts only (oot_hud_out), not
//     the lists'.
// No ImGui and no Vulkan: tests/test_mods_grid.py compiles it alone.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "vk_present_rect.h"

namespace mkwvk::game_grid {

/// The grid's rectangle in the window's pixels: its left and top, its width and height (2160 units down, 3840 across)
struct Place {
    float x = 0, y = 0, w = 0, h = 0;
    bool picture = false;   // ((11-19) from a picture the present showed; false: the window's column, none known yet)
    /// ((Triforce, 11-20) the picture itself in the window (the present's rectangle, whole pixels), with picture: the game
    /// draws nothing outside it)
    float pic_x = 0, pic_y = 0, pic_w = 0, pic_h = 0;
};

/// The game's picture (picture_w x picture_h, 0 not known yet) shown in a window_w x window_h window, aspect the picture's
/// shape (0: its pixels'). With no picture known, the window's middle 16:9 column of its whole height, as before
inline Place of(uint32_t picture_w, uint32_t picture_h, uint32_t window_w, uint32_t window_h, double aspect) {
    Place g;
    if (!window_w || !window_h) return g;
    if (!picture_w || !picture_h) {
        g.h = (float)window_h;
        g.w = std::min((float)window_w, std::floor((float)window_h * 16.0f / 9.0f + 0.5f));
        g.x = std::floor(((float)window_w - g.w) * 0.5f);
        return g;
    }
    const PresentRect p = present_rect(picture_w, picture_h, nullptr, window_w, window_h, PresentAspect{aspect});
    const double ratio = (double)p.w / (double)p.h, squeeze = std::min(1.0, (16.0 / 9.0) / ratio);
    g.h = (float)p.h, g.y = (float)p.y, g.picture = true;
    g.pic_x = (float)p.x, g.pic_y = (float)p.y, g.pic_w = (float)p.w, g.pic_h = (float)p.h;
    g.w = (float)((double)p.w * squeeze);
    g.x = (float)p.x + ((float)p.w - g.w) * 0.5f;
    return g;
}

/// A drawing made on a 16:9 column of the grid's height (its own pixels, from 0: w = h x 16 / 9 wide, unit h / 720) put on
/// the grid: x across times kx (1 on a 16:9 grid; under 1 where the layouts are squeezed across), then moved by ox, oy
struct Map {
    float ox = 0, oy = 0, kx = 1;
    float x(float px) const { return ox + px * kx; }
    float y(float py) const { return oy + py; }
    /// (the window's pixel back in the drawing's: the mouse)
    float back_x(float wx) const { return (wx - ox) / kx; }
    float back_y(float wy) const { return wy - oy; }
};
inline float column_w(const Place& g) { return std::floor(g.h * 16.0f / 9.0f + 0.5f); }
/// ((Triforce, 11-19) on a 16:9 grid (a wider picture's middle, or a 16:9 one) the column is put on whole pixels, not
/// stretched: the text drawn on it is on whole pixels already (ImGui's glyphs), and a part of a pixel across or a scale a
/// little off 1 (the grid's width from the picture's, the column's rounded) left it between pixels, soft. Only a grid
/// narrower than 16:9 (16:10) is squeezed across, as the game squeezes its layouts there)
inline Map map_of(const Place& g) {
    Map m;
    const float cw = column_w(g);
    if (cw > 0 && g.w >= cw - 1.0f) {
        m.ox = std::round(g.x + (g.w - cw) * 0.5f), m.oy = std::round(g.y), m.kx = 1.0f;
        return m;
    }
    m.ox = std::round(g.x), m.oy = std::round(g.y), m.kx = cw > 0 ? g.w / cw : 1.0f;
    return m;
}

/// A page's 16:9 column (its own pixels from 0: w x h, unit h / 720) and where it goes in the window (map)
struct Column {
    Map map;
    float w = 0, h = 0;
    /// ((Triforce, 11-20) over the game's panes: the picture's right edge in the column's own pixels (past w where the
    /// picture is wider than the grid); 0 otherwise
    float game_x1 = 0;
};
/// ((Triforce, 11-19) the column a page of ours is drawn on in a window_w x window_h window. on_game: over the game's own
/// panes (its Options screen under our tabs and their header, the Mods entry's list): on the game's grid, as those panes
/// are, so that our plates cover them; the lab's "16:9 window, 21:9 picture letterboxed: the game's Default Settings and
/// Back and the left edge of its help box show through beside our panel" was our tabs on the window's column of its whole
/// height while the game's panes were on the picture's grid. Else (our options over play), and with no picture known yet,
/// the window's middle 16:9 column of its whole height, as before (narrower windows: the whole window, not squeezed)
inline Column column_of(const Place& grid, float window_w, float window_h, bool on_game) {
    Column c;
    if (on_game && grid.picture) {
        c.map = map_of(grid), c.w = column_w(grid), c.h = grid.h;
        c.game_x1 = c.map.back_x(grid.pic_x + grid.pic_w);
        return c;
    }
    c.w = std::min(window_w, std::floor(window_h * 16.0f / 9.0f + 0.5f)), c.h = window_h;
    c.map.ox = std::floor((window_w - c.w) * 0.5f);
    return c;
}

/// ((Triforce, 11-20) A rectangle of the window, in its pixels
struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
/// ((Triforce, 11-20) A dim over a whole window_w x window_h window cut where the game's picture ends (g, from of): out[0]
/// the picture, where a dim keeps its colour, then the bars beside, above or below it (up to four, none overlapping, all
/// of them and the picture the window exactly), where a dim must be black. The lab's check 3 of 26670c0: our dialogs' dim
/// (rgba(8, 5, 2, ...), lighter than black) turned the black bars a dark brown, luminance about 9. Black at the same alpha
/// never lightens them; over the picture the dim stays as it was. With no picture known: the whole window, one part.
/// Returns how many
inline int dim_parts(const Place& g, float window_w, float window_h, Rect out[5]) {
    out[0] = {0, 0, window_w, window_h};
    if (!g.picture) return 1;
    const float x0 = std::clamp(g.pic_x, 0.0f, window_w), x1 = std::clamp(g.pic_x + g.pic_w, x0, window_w);
    const float y0 = std::clamp(g.pic_y, 0.0f, window_h), y1 = std::clamp(g.pic_y + g.pic_h, y0, window_h);
    out[0] = {x0, y0, x1, y1};
    int n = 1;
    if (y0 > 0) out[n++] = {0, 0, window_w, y0};                // (above)
    if (y1 < window_h) out[n++] = {0, y1, window_w, window_h};  // (below)
    if (x0 > 0) out[n++] = {0, y0, x0, y1};                     // (left, between those)
    if (x1 < window_w) out[n++] = {x1, y0, window_w, y1};       // (right)
    return n;
}

/// ((Triforce, 11-20) The game's guides band at the foot of its Options screen (SystemOverlay_00's N_Footer_00: W_GuideBg_00
/// and W_GuideBg_01, right-aligned 2110 and 2174 units right of the middle, 160 high from 19 under the foot) goes on past the
/// 16:9 column's right edge (1920): on a picture wider than the grid its end shows beside the column. The lab's check 3 of
/// 26670c0 (the game's own tab: 16x9_10, 21x9_10, fs_10): the band seen to 238 to 240 units past the column's edge (the
/// window pane's last texels clear), from 126 units over the foot, and our guides' plate ending at the column's edge left
/// 75 px of it in the 1600 x 900 window and 155 px in fullscreen 3440 x 1440. Where the band ends, in units right of the middle
constexpr float kGuideBandRight = 2160;
/// ((Triforce, 11-20, a review of 1104352) The Options screen chosen from the pause menu (host/oot_menu.c tells it as 2): its
/// footer may be the pause menu's own (MenuOverlay_00's band panes end 2110 units right of the middle; in the pause menu's
/// fs_30_sys_on_title_screen.png its band fades out by about 2100), not SystemOverlay_00's to 2160. No lab shot of that screen
/// yet: our plate ends at the smaller of the two there, so that it never runs about 50 units (33 px in fullscreen 3440 x 1440)
/// past the game's band as a dark sliver over the bars' black
constexpr float kPauseGuideBandRight = 2110;
/// (where our guides' plate ends over the game's panes on column c, in its own pixels from 0: as far as the game's band
/// (band_right units right of the middle: kGuideBandRight, the title's Options; kPauseGuideBandRight, the pause menu's), up to
/// the picture's right edge (the game draws nothing past it); the column's right edge on a 16:9 or narrower picture (16:10:
/// the grid is the whole picture), over play, or with no picture known)
inline float guide_band_x1(const Column& c, float band_right = kGuideBandRight) {
    const float band = c.w * 0.5f + band_right * c.h / 2160.0f;
    return std::max(c.w, std::min(band, c.game_x1));
}

}   // namespace mkwvk::game_grid
