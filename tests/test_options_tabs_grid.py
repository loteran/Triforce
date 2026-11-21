"""Our options' tabs inside the game's own Options screen on the game's grid, over the game's panes, in every window shape
(gpu/vk/game_grid.h column_of, vk_menu.cpp draw_menu, draw_game_tab, draw_fps, prebake and the game tab's mouse; no game).

(Triforce, 11-19) The lab's check of 662cfef, a 1600 x 900 window showing a letterboxed 21:9 picture: on every Triforce tab
inside the game's Options page "the game's own Default Settings (X) / Back (A) footer and the left edge of its help box show
through mid-screen beside our panel"; not in the 1720 x 720 window or fullscreen. Our tabs' page (its opaque column, its
help box, the dark band behind the guides that hides the game's) was drawn on a 16:9 column of the window's whole height
(1600 x 900 from the top), while the game draws its Options panes on its 3840 x 2160 grid in the picture (1200 x 675, 112
down). Where the picture fills the window's height (1720 x 720, 3440 x 1440) the two are the same, which is why only the
letterboxed window showed it. Also "our help box draws over part of the stats panel" (fullscreen): the Full panel was drawn
under the whole page; on the game's own tab it is over the game's panes and under our header, and now on ours too.

1. game_grid.h alone: column_of over the game's panes puts a place of the game's layout (its footer's guides, its help
   box's left edge, its header's foot, the grid's corners) where the game's grid has it, within a pixel, for a 16:9 window
   with a 21:9 picture, 21:9 windows, fullscreen 3440 x 1440, 32:9 in 16:9 and 32:9 windows, a 16:10 picture (squeezed
   across), a 4:3 window with a 16:9 picture (bars above and below) and with a 4:3 picture (squeezed); over play, and with
   no picture known yet, the window's column as before. The old column (the window's, its whole height) is checked to miss
   the game's footer in the lab's window by far, as the shots show.
2. The draw list as draw_menu leaves it, with Dear ImGui itself and the functions taken from vk_menu.cpp (clip_now,
   push_window_clip, put_on_grid, header_layer): the page put on the grid, the window's clip rectangle kept, a page's own
   rectangles moved with it and not cut on a squeezed grid wider than the window, the Full panel's vertices left in the
   window's pixels, and the order drawn: the page, the Full panel, our header. ((11-20, a review of c3d2d32) A dialog
   over the page (dim_page, panel_between): its dim over the whole window, over the header too; and the FPS counter's
   text under our header on a 16:10 grid, with Dear ImGui's own font: every glyph dropped with the window's clip as it
   was, all there with push_window_clip. (11-20, lab check 3) The Full panel over a dialog's dim and under its box,
   inside the game's Options screen and over play (bright, as over the game's own dialogs and Quit's), under the pages
   over play with no dialog; inside the game's Options screen the dim black over the bars above and below the picture,
   over play (our own background over the bars) one rectangle over the whole window, a review of 408e2dc.)
3. The source: draw_menu, draw_game_tab, draw_fps, prebake and the game tab's mouse on that column and map; the restart
   question's, the Mods dialogs' and the keyboard's dim through dim_page.

    python -I -B tests/test_options_tabs_grid.py [--cxx clang++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
IMGUI = ROOT / "third_party" / "imgui"
RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def block(text, head):
    start = text.index(head)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


GRID = r"""
#include <cmath>
#include <cstdio>
#include <cstdint>
#include "game_grid.h"
using namespace mkwvk;
static int fails = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%d %s\n", __LINE__, #x); fails++; } } while (0)
static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
// (where the game draws a place of its layout (units from the middle, y up): its grid, 3840 x 2160 units over the place)
static void game_at(const game_grid::Place& g, float ux, float uy, float* x, float* y) {
    *x = g.x + g.w * 0.5f + ux * g.w / 3840, *y = g.y + g.h * 0.5f - uy * g.h / 2160;
}
// (where our page draws it: ui::at on the column (unit h / 720, 3 layout units a point), then put through the map)
static void our_at(const game_grid::Column& c, float ux, float uy, float* x, float* y) {
    const float unit = c.h / 720.0f;
    *x = c.map.x(c.w * 0.5f + ux * unit / 3), *y = c.map.y(c.h * 0.5f - uy * unit / 3);
}
int main() {
    const double r21 = 64.0 / 27.0, r32 = 32.0 / 9.0, r1610 = 16.0 / 10.0;
    // (the places our plates must cover: the game's guides (Default Settings, Back) at the foot's right, 62 up; its help
    // box's plate from 440 right of the middle; the header's foot at 880 up; the grid's corners)
    const float places[][2] = {{1000, -1018}, {1500, -1018}, {1840, -1018}, {440, -90}, {456, 300}, {0, 880}, {-1920, 1080},
                               {1920, -1080}, {-730, 0}};
    const struct { uint32_t pw, ph, ww, wh; double r; const char* what; } cases[] = {
        {3416, 1440, 1600, 900, r21, "16:9 window, 21:9 picture between bars above and below (the lab's)"},
        {3416, 1440, 1720, 720, r21, "1720 x 720 window, 21:9 picture"},
        {3416, 1440, 3440, 1440, r21, "fullscreen 3440 x 1440, 21:9"},
        {3416, 1440, 2560, 1080, r21, "21:9 window"},
        {5120, 1440, 1920, 1080, r32, "32:9 picture in a 16:9 window"},
        {5120, 1440, 5120, 1440, r32, "32:9 window"},
        {2304, 1440, 1920, 1200, r1610, "16:10 picture in a 16:10 window (squeezed across)"},
        {2560, 1440, 1600, 1200, 0, "4:3 window, 16:9 picture between bars"},
        {1600, 1200, 1600, 1200, 0, "4:3 window, its own 4:3 picture (squeezed across)"},
        {2560, 1440, 1601, 901, 0, "an odd 16:9 window"},
    };
    for (const auto& k : cases) {
        const game_grid::Place g = game_grid::of(k.pw, k.ph, k.ww, k.wh, k.r);
        const game_grid::Column c = game_grid::column_of(g, (float)k.ww, (float)k.wh, true);
        for (const auto& p : places) {
            float gx, gy, ox, oy;
            game_at(g, p[0], p[1], &gx, &gy);
            our_at(c, p[0], p[1], &ox, &oy);
            if (!near(gx, ox, 1.0f) || !near(gy, oy, 1.0f)) {
                std::printf("FAIL: %s: the game's (%g, %g) at (%.1f, %.1f), ours at (%.1f, %.1f)\n", k.what, p[0], p[1], gx, gy, ox, oy);
                fails++;
            }
        }
        // (the mouse: back through the same map onto the column's pixels)
        CHECK(near(c.map.back_x(c.map.x(321)), 321, 0.01f) && near(c.map.back_y(c.map.y(123)), 123, 0.01f));
        // (over play: the window's column of its whole height, as before)
        const game_grid::Column play = game_grid::column_of(g, (float)k.ww, (float)k.wh, false);
        const float w = std::fmin((float)k.ww, std::floor(k.wh * 16.0f / 9.0f + 0.5f));
        CHECK(play.w == w && play.h == (float)k.wh && play.map.ox == std::floor((k.ww - w) * 0.5f) && play.map.oy == 0 && play.map.kx == 1);
    }
    // no picture known yet: the window's column, as before, not squeezed in a narrow window
    {
        const game_grid::Place g = game_grid::of(0, 0, 1600, 1200, 0);
        const game_grid::Column c = game_grid::column_of(g, 1600, 1200, true);
        CHECK(!g.picture && c.w == 1600 && c.h == 1200 && c.map.ox == 0 && c.map.oy == 0 && c.map.kx == 1);
        const game_grid::Column w = game_grid::column_of(game_grid::of(0, 0, 2560, 1080, 0), 2560, 1080, true);
        CHECK(w.w == 1920 && w.h == 1080 && w.map.ox == 320 && w.map.kx == 1);
    }
    // the old column in the lab's window: the window's 16:9 of its whole height from its top. The game's guides at its foot
    // (1500, -1018) are 112 + 675 * (1080 + 1018) / 2160 = 768 down; our band behind the guides (prompt_bar: from 160
    // units over the foot) began at 900 - 160 * 900 / 2160 = 833: the guides were above it, over the game's picture, beside
    // our panel. Now the band's top is at the game's 160 units over its foot, 112 + 675 - 50 = 737, over the guides
    {
        const game_grid::Place g = game_grid::of(3416, 1440, 1600, 900, r21);
        float gx, gy;
        game_at(g, 1500, -1018, &gx, &gy);
        const float old_band = 900 - 160 * 900.0f / 2160;
        CHECK(near(gy, 768.3f, 1.0f) && gy < old_band - 50);
        float ox, oy;
        our_at(game_grid::column_of(g, 1600, 900, true), 0, -1080 + 160, &ox, &oy);
        CHECK(near(oy, 112 + 675 - 50, 1.0f) && oy < gy);
    }
    if (!fails) std::puts("our tabs' column on the game's grid in every window shape");
    return fails ? 1 : 0;
}
"""

DRAW = r"""
#include <cmath>
#include <cstdio>
#include <vector>
#include "imgui.h"
#include "imgui_internal.h"
#include "game_grid.h"
namespace ui { struct Ctx { ImDrawList* d; float w = 0, h = 0; }; }
namespace mkwvk {
game_grid::Map menu_map;
float menu_window_w = 0, menu_window_h = 0;
game_grid::Place grid_now;   // (the picture the dims are cut at: menu_grid's, from the present)
bool embed_open = false;     // (inside the game's Options screen: the game's picture and its black bars under our page)
[[maybe_unused]] constexpr int kModsRowPage = 9;
int page = 0;
game_grid::Place menu_grid(float, float) { return grid_now; }
// (the Full panel as draw_perf draws it: at the window's top right, in its pixels; tagged PERF, 6)
void draw_perf(ui::Ctx& c) { c.d->AddRectFilled(ImVec2(c.w - 390, 12), ImVec2(c.w - 12, 700), IM_COL32(6, 0, 0, 255)); }
%FUNCS%
}
using namespace mkwvk;
static int fails = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%d %s\n", __LINE__, #x); fails++; } } while (0)
static bool near(float a, float b, float tol = 0.02f) { return std::fabs(a - b) <= tol; }
static bool same(const ImVec4& a, const ImVec4& b, float tol = 0.02f) {
    return near(a.x, b.x, tol) && near(a.y, b.y, tol) && near(a.z, b.z, tol) && near(a.w, b.w, tol);
}
// (each rectangle in its own red: the order the GPU draws them in, by the commands and their indices)
enum { BAR = 0, PRE = 1, BODY, BODY2, BODY3, HEAD, PERF, AFTER, DIM, BOX, TEXT };   // (BAR: black, a dim over the bars)
static ImU32 tag(int t) { return IM_COL32(t, 0, 0, 255); }
static std::vector<int> order(const ImDrawList& d) {
    std::vector<int> out;
    for (const ImDrawCmd& c : d.CmdBuffer)
        for (unsigned i = 0; i < c.ElemCount; i++) {
            const int t = (int)(d.VtxBuffer[(int)(c.VtxOffset + d.IdxBuffer[(int)(c.IdxOffset + i)])].col & 0xff);
            if (out.empty() || out.back() != t) out.push_back(t);
        }
    return out;
}
static const ImDrawCmd* cmd_of(const ImDrawList& d, int t) {
    for (const ImDrawCmd& c : d.CmdBuffer)
        for (unsigned i = 0; i < c.ElemCount; i++)
            if ((int)(d.VtxBuffer[(int)(c.VtxOffset + d.IdxBuffer[(int)(c.IdxOffset + i)])].col & 0xff) == t) return &c;
    return nullptr;
}
static ImVec2 min_of(const ImDrawList& d, int t) {
    ImVec2 m(1e9f, 1e9f);
    for (const ImDrawVert& v : d.VtxBuffer)
        if ((int)(v.col & 0xff) == t) m.x = std::fmin(m.x, v.pos.x), m.y = std::fmin(m.y, v.pos.y);
    return m;
}
static ImVec2 max_of(const ImDrawList& d, int t) {
    ImVec2 m(-1e9f, -1e9f);
    for (const ImDrawVert& v : d.VtxBuffer)
        if ((int)(v.col & 0xff) == t) m.x = std::fmax(m.x, v.pos.x), m.y = std::fmax(m.y, v.pos.y);
    return m;
}
// (a page as draw_menu draws it over the game's Options screen, the Full panel between the page and its header when perf)
static void draw_page(ImDrawList& d, const game_grid::Column& col, float W, float H, bool perf, int* p0, int* p1) {
    ui::Ctx c{&d};
    d.AddRectFilled(ImVec2(5, 5), ImVec2(20, 20), tag(PRE));   // (drawn before the page: the dim, the window's)
    const int vtx0 = d.VtxBuffer.Size, cmd0 = d.CmdBuffer.Size;
    const ImVec4 window_clip = clip_now(&d);
    if (perf) d.ChannelsSplit(2), header_split = &d;
    push_window_clip(&d, col.map, W, H);
    d.AddRectFilled(ImVec2(0, 0), ImVec2(10, 10), tag(BODY));   // (the column's corner)
    d.PushClipRect(ImVec2(col.w * 0.5f, 50), ImVec2(col.w, col.h), true);   // (the help box's own clip: to the column's right edge)
    d.AddRectFilled(ImVec2(col.w * 0.5f, 50), ImVec2(col.w + 40, 60), tag(BODY2));
    d.PopClipRect();
    header_layer(c, true);
    d.AddRectFilled(ImVec2(0, 0), ImVec2(col.w, col.h * 200 / 2160), tag(HEAD));   // (the header, 200 units)
    header_layer(c, false);
    d.AddRectFilled(ImVec2(col.w - 300, col.h - 40), ImVec2(col.w, col.h), tag(BODY3));   // (the guides' band)
    d.PopClipRect();
    *p0 = *p1 = 0;
    if (perf) {
        d.ChannelsSetCurrent(0);
        *p0 = d.VtxBuffer.Size;
        d.AddRectFilled(ImVec2(W - 390, 12), ImVec2(W - 12, 700), tag(PERF));   // (the Full panel: the window's top right)
        *p1 = d.VtxBuffer.Size;
        d.ChannelsMerge();
        header_split = nullptr;
    }
    put_on_grid(&d, vtx0, cmd0, col.map, window_clip, *p0, *p1);
    d.AddRectFilled(ImVec2(30, 30), ImVec2(40, 40), tag(AFTER));   // (drawn after: the counter, a notice)
}
// ((Triforce, 11-20, a review of c3d2d32) a page with a dialog over it as draw_menu and draw_mods (mods_dialog,
// mods_keyboard) or draw_ask draw it: the header, then the dialog's dim (dim_page) and its box, then the guides. perf: the
// Full panel waits (panel_wait) in a list split in two: inside the game's Options screen (inside) the page in channel 0
// and the header in 1; over play the pages in 1, channel 0 left for the panel under them. ((11-20) dialog false: none)
static void page_dialog(ImDrawList& d, const game_grid::Column& col, float W, float H, bool inside, bool dialog, int* p0, int* p1) {
    ui::Ctx win{&d, W, H}, c{&d, col.w, col.h};
    menu_map = col.map, menu_window_w = W, menu_window_h = H;
    d.AddRectFilled(ImVec2(5, 5), ImVec2(20, 20), tag(PRE));
    const int vtx0 = d.VtxBuffer.Size, cmd0 = d.CmdBuffer.Size;
    const ImVec4 window_clip = clip_now(&d);
    d.ChannelsSplit(2), panel_split = &d, panel_wait = {&win, window_clip, 0, 0};
    if (inside) header_split = &d;
    else d.ChannelsSetCurrent(1);
    push_window_clip(&d, col.map, W, H);
    d.AddRectFilled(ImVec2(0, 0), ImVec2(10, 10), tag(BODY));
    header_layer(c, true);
    d.AddRectFilled(ImVec2(0, 0), ImVec2(col.w, col.h * 200 / 2160), tag(HEAD));
    header_layer(c, false);
    if (dialog) dim_page(c, tag(DIM));
    d.AddRectFilled(ImVec2(col.w * 0.25f, col.h * 0.3f), ImVec2(col.w * 0.75f, col.h * 0.7f), tag(BOX));
    d.AddRectFilled(ImVec2(col.w - 300, col.h - 40), ImVec2(col.w, col.h), tag(BODY3));
    d.PopClipRect();
    panel_between(&d), *p0 = panel_wait.v0, *p1 = panel_wait.v1;
    put_on_grid(&d, vtx0, cmd0, col.map, window_clip, *p0, *p1);
}
static bool covers(const ImDrawList& d, int over, int under) {
    const ImVec2 a = min_of(d, over), b = max_of(d, over), p = min_of(d, under), q = max_of(d, under);
    return a.x <= p.x + 0.02f && a.y <= p.y + 0.02f && b.x >= q.x - 0.02f && b.y >= q.y - 0.02f;
}
static int index_of(const std::vector<int>& v, int t) {
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] == t) return (int)i;
    return -1;
}
int main() {
    ImDrawListSharedData shared;
    const double r21 = 64.0 / 27.0, r1610 = 16.0 / 10.0;
    // the lab's window: 1600 x 900, a 21:9 picture between bars; the grid 1200 x 675 at (200, 112)
    {
        const float W = 1600, H = 900;
        const game_grid::Column col = game_grid::column_of(game_grid::of(3416, 1440, 1600, 900, r21), W, H, true);
        const float oy = col.map.oy;
        CHECK(col.map.ox == 200 && (oy == 112 || oy == 113) && col.map.kx == 1 && col.w == 1200 && near(col.h, 675, 0.5f));
        ImDrawList d(&shared);
        d._ResetForNewFrame();
        d.PushClipRect(ImVec2(0, 0), ImVec2(W, H));
        int p0, p1;
        draw_page(d, col, W, H, true, &p0, &p1);
        const std::vector<int> want = {PRE, BODY, BODY2, BODY3, PERF, HEAD, AFTER};
        CHECK(order(d) == want);   // (the page, then the Full panel over it, then our header over the panel)
        CHECK(near(min_of(d, BODY).x, 200) && near(min_of(d, BODY).y, oy));   // (the column's corner on the grid's)
        CHECK(near(min_of(d, HEAD).y, oy) && near(max_of(d, HEAD).x, 1400));    // (the header over the game's, on the picture)
        CHECK(near(max_of(d, BODY3).y, oy + col.h) && near(max_of(d, BODY3).x, 1400));   // (the guides' band at the picture's foot)
        CHECK(near(min_of(d, PERF).x, W - 390) && near(min_of(d, PERF).y, 12));   // (the panel where the window has it)
        CHECK(near(min_of(d, PRE).x, 5) && near(min_of(d, AFTER).x, 30));          // (before and after: not moved)
        const ImVec4 win(0, 0, W, H);
        CHECK(same(cmd_of(d, PRE)->ClipRect, win) && same(cmd_of(d, PERF)->ClipRect, win) && same(cmd_of(d, AFTER)->ClipRect, win));
        CHECK(same(cmd_of(d, BODY)->ClipRect, win) && same(cmd_of(d, HEAD)->ClipRect, win));   // (the window's, put back)
        CHECK(same(cmd_of(d, BODY2)->ClipRect, ImVec4(200 + 600, oy + 50, 1400, oy + col.h)));   // (the help box's, moved)
    }
    // a 16:10 picture: the grid the whole window, the column (2133 wide) squeezed into it. A page's clip rectangle to the
    // column's right edge reaches the window's (the window's rectangle as it is, 1920 in the column's pixels, would have cut
    // it at 1920 x 0.9 = 1728)
    {
        const float W = 1920, H = 1200;
        const game_grid::Column col = game_grid::column_of(game_grid::of(2304, 1440, 1920, 1200, r1610), W, H, true);
        CHECK(col.w == 2133 && col.map.kx < 0.95f);
        ImDrawList d(&shared);
        d._ResetForNewFrame();
        d.PushClipRect(ImVec2(0, 0), ImVec2(W, H));
        int p0, p1;
        draw_page(d, col, W, H, false, &p0, &p1);
        const std::vector<int> want = {PRE, BODY, BODY2, HEAD, BODY3, AFTER};   // (no panel: drawn in order, nothing split)
        CHECK(order(d) == want);
        CHECK(near(cmd_of(d, BODY2)->ClipRect.z, W, 0.5f) && near(cmd_of(d, BODY2)->ClipRect.x, W * 0.5f, 0.5f));
        CHECK(near(max_of(d, BODY3).x, W, 0.5f) && near(max_of(d, HEAD).x, W, 0.5f));
    }
    // a 16:9 picture in a 16:10 window: bars above and below; the column as wide as the window, lower than its top. Its own
    // rectangles move down with it (the old rule kept every rectangle from the window's left edge to its right one)
    {
        const float W = 1920, H = 1200;
        const game_grid::Column col = game_grid::column_of(game_grid::of(2560, 1440, 1920, 1200, 0), W, H, true);
        CHECK(col.w == 1920 && col.map.oy == 60 && col.map.ox == 0);
        ImDrawList d(&shared);
        d._ResetForNewFrame();
        d.PushClipRect(ImVec2(0, 0), ImVec2(W, H));
        const int vtx0 = d.VtxBuffer.Size, cmd0 = d.CmdBuffer.Size;
        const ImVec4 window_clip = clip_now(&d);
        push_window_clip(&d, col.map, W, H);
        d.PushClipRect(ImVec2(0, 100), ImVec2(col.w, 200), true);
        d.AddRectFilled(ImVec2(0, 100), ImVec2(col.w, 200), tag(BODY));
        d.PopClipRect();
        d.PopClipRect();
        put_on_grid(&d, vtx0, cmd0, col.map, window_clip);
        CHECK(same(cmd_of(d, BODY)->ClipRect, ImVec4(0, 160, W, 260)));
    }
    // ((Triforce, 11-20, a review of c3d2d32) a dialog over the Mods page with the Full panel on, inside the game's Options
    // screen and over play (the lab's 1600 x 900 window, 21:9 picture): the dim over the page and the header alike, across
    // the whole window (before: the dim in channel 0 under the header in channel 1). ((11-20) The panel then over the dim and
    // under the box, bright as over the game's own dialogs and Quit's (26670c0 dimmed it under the restart question and the
    // Mods dialogs); the dim's colour over the picture only, black over the bars above and below it (lab check 3: "the black
    // bars turn dark brown"). With no dialog: the panel between the page and its header inside, under the pages over play.
    // ((11-20, a review of 408e2dc) Over play the bars are not the game's black: draw_menu covered the whole window with our
    // own background and our page and header are on the window's column, over the bars too. There the dim is one rectangle
    // in its colour over the whole window, as on 26670c0; cut black at the picture's edges it left a straight step of the
    // tint across our background and header at y 112 and 787)
    for (bool inside : {true, false})
        for (bool dialog : {true, false}) {
            const float W = 1600, H = 900;
            const game_grid::Place g = game_grid::of(3416, 1440, 1600, 900, r21);
            const game_grid::Column col = game_grid::column_of(g, W, H, inside);
            grid_now = g, embed_open = inside, mkwvk::page = 0;
            ImDrawList d(&shared);
            d._ResetForNewFrame();
            d.PushClipRect(ImVec2(0, 0), ImVec2(W, H));
            int p0, p1;
            page_dialog(d, col, W, H, inside, dialog, &p0, &p1);
            const std::vector<int> o = order(d);
            const std::vector<int> want = dialog ? (inside ? std::vector<int>{PRE, BODY, HEAD, DIM, BAR, PERF, BOX, BODY3}
                                                           : std::vector<int>{PRE, BODY, HEAD, DIM, PERF, BOX, BODY3})
                                          : inside ? std::vector<int>{PRE, BODY, BOX, BODY3, PERF, HEAD}
                                                   : std::vector<int>{PRE, PERF, BODY, HEAD, BOX, BODY3};
            CHECK(o == want);
            if (o != want) {
                for (int t : o) std::printf(" %d", t);
                std::printf("  (inside %d, dialog %d)\n", (int)inside, (int)dialog);
            }
            CHECK(header_split == nullptr && panel_split == nullptr && panel_wait.window == nullptr);
            CHECK(!dialog || (index_of(o, HEAD) < index_of(o, DIM) && index_of(o, DIM) < index_of(o, PERF) && index_of(o, PERF) < index_of(o, BOX)));
            CHECK(near(min_of(d, PERF).x, W - 390) && near(min_of(d, PERF).y, 12));   // (the panel where the window has it)
            CHECK(same(cmd_of(d, PERF)->ClipRect, ImVec4(0, 0, W, H)));
            if (!dialog) continue;
            // (inside: the dim's colour over the picture, 1600 x 675 from 112 down; black at its alpha over the bars, to the
            // window's edges. Over play: the dim's colour over the whole window, our header in the top bar under it, no black)
            CHECK(g.pic_h == 675 && g.pic_y == 112);
            if (inside) {
                CHECK(near(min_of(d, DIM).x, 0) && near(min_of(d, DIM).y, g.pic_y) && near(max_of(d, DIM).x, W) && near(max_of(d, DIM).y, g.pic_y + g.pic_h));
                CHECK(near(min_of(d, BAR).y, 0) && near(max_of(d, BAR).y, H) && near(min_of(d, BAR).x, 0) && near(max_of(d, BAR).x, W));
            } else {
                CHECK(near(min_of(d, DIM).x, 0) && near(min_of(d, DIM).y, 0) && near(max_of(d, DIM).x, W) && near(max_of(d, DIM).y, H));
                CHECK(index_of(o, BAR) < 0);
            }
            CHECK(covers(d, DIM, HEAD) && same(cmd_of(d, DIM)->ClipRect, ImVec4(0, 0, W, H)));
            CHECK(near(min_of(d, BOX).x, col.map.ox + col.w * 0.25f, 0.5f));          // (the box on the column)
        }
    // a squeezed grid (16:10): the picture the whole window, no bars; the dim the whole window, the panel over it
    {
        const float W = 1920, H = 1200;
        const game_grid::Place g = game_grid::of(2304, 1440, 1920, 1200, r1610);
        const game_grid::Column col = game_grid::column_of(g, W, H, true);
        grid_now = g, embed_open = true, mkwvk::page = 0;
        ImDrawList d(&shared);
        d._ResetForNewFrame();
        d.PushClipRect(ImVec2(0, 0), ImVec2(W, H));
        int p0, p1;
        page_dialog(d, col, W, H, true, true, &p0, &p1);
        const std::vector<int> want = {PRE, BODY, HEAD, DIM, PERF, BOX, BODY3};
        CHECK(order(d) == want);
        CHECK(near(min_of(d, DIM).x, 0, 0.5f) && near(max_of(d, DIM).x, W, 0.5f) && near(min_of(d, DIM).y, 0, 0.5f) && near(max_of(d, DIM).y, H, 0.5f));
    }
    // ((Triforce, 11-20, a review of c3d2d32) the FPS counter under our header (draw_fps) on a squeezed grid: a 1680 x 1050
    // window, a 16:10 picture; the column 1867 wide, x 0.9. The counter at the column's right, about 1753 to 1828 in its
    // pixels, past the window's 1680: with Dear ImGui's own text, a glyph past the clip's right edge is dropped as it is
    // added (ImFont::RenderText), so with the window's clip as it is no glyph is left; with the window's clip in the
    // column's pixels (push_window_clip, as draw_fps has it now) they are all there, inside the window after the squeeze
    {
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr, io.LogFilename = nullptr;   // (no files of its own where the test runs)
        io.DisplaySize = ImVec2(1680, 1050);
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImFont* font = io.Fonts->AddFontDefault();
        ImGui::NewFrame();
        const float W = 1680, H = 1050;
        const game_grid::Column col = game_grid::column_of(game_grid::of(2304, 1440, 1680, 1050, r1610), W, H, true);
        CHECK(col.w == 1867 && near(col.map.kx, 0.9f, 0.001f));
        auto counter = [&](bool push, ImVec2* lo, ImVec2* hi) {
            ImDrawList d(ImGui::GetDrawListSharedData());
            d._ResetForNewFrame();
            d.PushTexture(io.Fonts->TexRef);
            d.PushClipRect(ImVec2(0, 0), ImVec2(W, H));
            const int vtx0 = d.VtxBuffer.Size, cmd0 = d.CmdBuffer.Size;
            const ImVec4 window_clip = clip_now(&d);
            if (push) push_window_clip(&d, col.map, W, H);
            d.AddText(font, 24, ImVec2(1753, 80), tag(TEXT), "60 FPS");
            if (push) d.PopClipRect();
            put_on_grid(&d, vtx0, cmd0, col.map, window_clip);
            int n = 0;
            for (const ImDrawVert& v : d.VtxBuffer) n += (int)(v.col & 0xff) == TEXT;
            *lo = min_of(d, TEXT), *hi = max_of(d, TEXT);
            CHECK(!n || same(cmd_of(d, TEXT)->ClipRect, window_clip));
            return n;
        };
        ImVec2 lo, hi;
        CHECK(counter(false, &lo, &hi) == 0);   // (the window's clip as it was: every glyph dropped, no counter)
        const int n = counter(true, &lo, &hi);
        CHECK(n >= 4 * 5);                      // (its glyphs, "60 FPS": five at least)
        CHECK(lo.x >= 1753 * 0.9f - 1 && hi.x <= W && hi.x > W - 200);   // (inside the window, at its right)
        ImGui::EndFrame();
        ImGui::DestroyContext(ctx);
    }
    if (!fails) std::puts("the page on the grid, the window's clip kept, the panel between the page and our header");
    return fails ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")

    # 3. the source
    dm = block(menu, "void draw_menu(ui::Ctx& c, bool perf_under) {")
    check("const bool on_game = !row && pages_on_game();" in dm and "const game_grid::Column col = pages_column(c.w, c.h, on_game);" in dm and
          "ui::Ctx k = column_ctx(c, col);" in dm and "menu_map = col.map;" in dm,
          "draw_menu: our tabs over the game's Options screen are not on the game's grid (the window's column of its whole height)")
    check("std::floor(c.h * 16.0f / 9.0f" not in dm, "draw_menu still lays its pages on the window's height")
    check("if (on_game) push_window_clip(c.d, menu_map, c.w, c.h);" in dm and "if (on_game) c.d->PopClipRect();" in dm,
          "draw_menu: a squeezed grid's page is cut at the window's width in the column's pixels")
    # ((11-20) the panel waits on every page but the Mods entry's, over play too, so that a dialog's dim can go under it)
    check("const bool perf_between = perf_under && !row;" in dm and "draw_perf(c);" not in dm and
          "c.d->ChannelsSplit(2), panel_split = c.d, panel_wait = {&c, window_clip, 0, 0};" in dm and
          "if (embed_open) header_split = c.d;" in dm and "else c.d->ChannelsSetCurrent(1);" in dm,
          "draw_menu: the Full panel does not wait for the page's end or a dialog's dim (still drawn under the page first)")
    between = dm[dm.index("int perf0 = 0, perf1 = 0;"):]
    check(between.index("panel_between(c.d);") < between.index("perf0 = panel_wait.v0, perf1 = panel_wait.v1;"),
          "draw_menu: the Full panel is not drawn between the page and its header")
    pb = block(menu, "void panel_between(ImDrawList* d) {")
    check(pb.index("d->ChannelsSetCurrent(0);") < pb.index("panel_draw(d);") < pb.index("d->ChannelsMerge();") <
          pb.index("panel_split = header_split = nullptr;"), "panel_between: the Full panel is not drawn between the page and its header")
    pd = block(menu, "void panel_draw(ImDrawList* d) {")
    check(pd.index("panel_wait.v0 = d->VtxBuffer.Size;") < pd.index("draw_perf(*panel_wait.window);") < pd.index("panel_wait.v1 = d->VtxBuffer.Size;"),
          "panel_draw: the Full panel's vertices are not kept out of the grid's move")
    check("put_on_grid(c.d, vtx0, cmd0, menu_map, window_clip, perf0, perf1);" in dm,
          "draw_menu: the Full panel's vertices are moved with the page")
    check(dm.index("menu_window_w = c.w, menu_window_h = c.h;") < dm.index("else if (page == 5) draw_ask(k);"),
          "draw_menu: the dialogs' dim does not know the window")
    # ((11-20, a review of c3d2d32) a dialog's dim over the whole window, over the page's header and the Full panel too: the
    # restart question, the Mods page's dialogs and its keyboard)
    dp = block(menu, "void dim_page(ui::Ctx& c, ImU32 col) {")
    check(dp.index("panel_merge(c.d);") < dp.index("dim_window(c.d, menu_map, game_black ? menu_grid(w, h) : game_grid::Place{}, w, h, col);") <
          dp.index("panel_over(c.d);"),
          "dim_page: the dim is not the whole window's over the page and its header, under the Full panel")
    for head, dim in (("void draw_ask(ui::Ctx& c) {", "dim_page(c, ui::rgba(8, 5, 2, 110));"),
                      ("void mods_dialog(ui::Ctx& c, const char* title,", "dim_page(c, ui::rgba(8, 5, 2, 120));"),
                      ("void mods_keyboard(ui::Ctx& c) {", "dim_page(c, ui::rgba(8, 5, 2, 120));")):
        body = block(menu, head)
        check(dim in body and "AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h)" not in body,
              head + " its dim is the column's alone, under the header and the Full panel inside the game's Options screen")
    opts = block(menu, "void draw_options(ui::Ctx& c) {")
    check(opts.index("header_layer(c, true);") < opts.index("ui::banner(c, hh);") < opts.index("draw_tabs(c, cur_tab") <
          opts.index("header_layer(c, false);") < opts.index("caption(c, hx0, hx1"), "draw_options: the header is not drawn over the Full panel")
    tb = block(menu, "float title_banner(ui::Ctx& c, const char* title, const char* right) {")
    check(tb.index("header_layer(c, true);") < tb.index("ui::banner(c, 0, title);") < tb.index("header_layer(c, false);"),
          "title_banner: the header is not drawn over the Full panel")
    pog = block(menu, "bool pages_on_game() {")
    check("embed_open ||" in pog and "game_options_shown()" in pog, "pages_on_game does not follow the game's Options screen")
    tab = block(menu, "void draw_game_tab(ui::Ctx& c) {")
    check("const game_grid::Column col = pages_column(c.w, c.h, true);" in tab and "ui::Ctx k = column_ctx(c, col);" in tab and
          "game_tab_map = col.map, hits_game_tab = true;" in tab and "put_on_grid(c.d, vtx0, cmd0, col.map, window_clip);" in tab,
          "draw_game_tab: the header over the game's own tab is not on the game's grid")
    check("std::floor(c.h * 16.0f / 9.0f" not in tab, "draw_game_tab still lays the header on the window's height")
    tick = block(menu, "void menu_tick() {")
    check("hits.at(game_tab_map.back_x((float)ix), game_tab_map.back_y((float)iy))" in tick,
          "the game's own tab does not take the mouse back through the map its tabs were drawn with")
    fps = block(menu, "void draw_fps(ui::Ctx& window, bool under_banner) {")
    check("const bool on_game = under_banner ? pages_on_game() : true;" in fps and
          "if (on_game) c.d->PopClipRect(), put_on_grid(c.d, vtx0, cmd0, col.map, window_clip);" in fps,
          "draw_fps: the counter under our header is not on the grid with the header")
    check(fps.index("const ImVec4 window_clip = clip_now(c.d);") < fps.index("if (on_game) push_window_clip(c.d, col.map, window.w, window.h);") <
          fps.index("edged(digits, big, x, my, fps_text, ui::kInk)"),
          "draw_fps: the counter is cut at the window's width in the column's pixels (gone on a 16:10 or 4:3 picture)")
    check("const float ph = pages_h(c.w, c.h);" in block(menu, "void prebake(const ui::Ctx& c) {") and
          "const float ph = pages_h((float)w, (float)h);" in block(menu, "bool prebake_needed(uint32_t w, uint32_t h) {"),
          "prebake: the pages' glyphs are not baked at the grid's size they open at inside the game's Options screen")

    funcs = "\n".join([
        "ImDrawList* header_split = nullptr;",
        block(menu, "void header_layer(const ui::Ctx& c, bool on) {"),
        block(menu, "ImVec4 clip_now(const ImDrawList* d) {"),
        block(menu, "void put_on_grid(ImDrawList* d, int vtx0, int cmd0, const game_grid::Map& m, const ImVec4& keep, int skip0 = 0, int skip1 = 0) {"),
        block(menu, "void push_window_clip(ImDrawList* d, const game_grid::Map& m, float window_w, float window_h) {"),
        block(menu, "struct PanelWait {") + ";",
        "PanelWait panel_wait;",
        "ImDrawList* panel_split = nullptr;",
        block(menu, "void panel_draw(ImDrawList* d) {"),
        block(menu, "void panel_between(ImDrawList* d) {"),
        block(menu, "void panel_merge(ImDrawList* d) {"),
        block(menu, "void panel_over(ImDrawList* d) {"),
        block(menu, "void dim_window(ImDrawList* d, const game_grid::Map& m, const game_grid::Place& g, float w, float h, ImU32 col) {"),
        block(menu, "void dim_page(ui::Ctx& c, ImU32 col) {"),
    ])
    with tempfile.TemporaryDirectory(prefix="options-tabs-grid-") as tmp:
        tmp = Path(tmp)
        # 1. the grid alone
        src, exe = tmp / "grid.cpp", tmp / "grid.exe"
        src.write_text(GRID, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(VK), str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        check(r.returncode == 0, "the column:\n" + r.stdout)
        print(r.stdout.strip())
        # 2. the draw list, with Dear ImGui
        src, obj, exe = tmp / "draw.cpp", tmp / "draw.o", tmp / "draw.exe"
        src.write_text(DRAW.replace("%FUNCS%", funcs), encoding="utf-8")
        inc = ["-I" + str(VK), "-I" + str(IMGUI)]
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", *inc,
                        "-c", str(src), "-o", str(obj)], check=True)
        objs = [str(obj)]
        for name in ("imgui.cpp", "imgui_draw.cpp", "imgui_widgets.cpp", "imgui_tables.cpp"):
            o = tmp / (name + ".o")
            subprocess.run([args.cxx, "-std=c++20", "-O0", *inc, "-c", str(IMGUI / name), "-o", str(o)], check=True)
            objs.append(str(o))
        subprocess.run([args.cxx, *objs, "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        check(r.returncode == 0, "the draw list:\n" + r.stdout + r.stderr)
        print(r.stdout.strip())
    print("PASS: our tabs inside the game's Options screen on the game's grid, the Full panel between the page and our header")


if __name__ == "__main__":
    main()
