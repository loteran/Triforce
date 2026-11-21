"""Our dialogs' dim leaves the black bars beside or above the game's picture black, the Full panel bright over it
(gpu/vk/game_grid.h dim_parts, vk_menu.cpp dim_window, dim_page, draw_quit_ask, draw_menu; no game).

(Triforce, 11-20) The lab's check 3 of 26670c0: under our dialogs (Quit, and the restart question's and the Mods dialogs'
dim through dim_page, which dim the whole window since 26670c0) "the black bars turn a dark brown": luminance 0 before,
8.6 to 8.9 settled (the dim rgba(8, 5, 2, a) is lighter than black). Over the picture the dim's look is to stay as it is.
And the stats panel: the game's own dialogs never dim our overlay, and over Quit's dialog the panel stayed bright, but the
restart question's, the Mods dialogs' and the keyboard's dim went over it (26670c0): our dialogs dimmed it partly. Now it
is over the dim under all of ours (test_options_tabs_grid.py checks the draw list's order).

1. game_grid.h alone: the dim's parts for the lab's shapes and others: the picture first (the dim's colour), then the
   bars (black), none overlapping, together exactly the window; with no picture known, the window in one part.
2. The blend: a black bar pixel under each dim (alpha 110, 120, 220) stays 0 in the bars' part; over the picture the
   result is the same as with the old one rectangle.
3. The source: dim_window draws the bars black at the dim's alpha, the picture in its colour; dim_page and Quit's dialog
   dim through it; draw_ask, mods_dialog and mods_keyboard through dim_page. ((11-20, a review of 408e2dc) dim_page cuts
   the bars black only inside the game's Options screen; over play our own background is under the bars, one rectangle.

    python -I -B tests/test_dialog_dim.py [--cxx clang++]
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
static float area(const game_grid::Rect& r) { return (r.x1 - r.x0) * (r.y1 - r.y0); }
static bool overlap(const game_grid::Rect& a, const game_grid::Rect& b) {
    return std::fmin(a.x1, b.x1) > std::fmax(a.x0, b.x0) && std::fmin(a.y1, b.y1) > std::fmax(a.y0, b.y0);
}
// (the source-over blend of one 8-bit channel, as the overlay's pipeline: dst (1 - a) + src a)
static float over(float dst, float src, float a) { return dst * (1 - a) + src * a; }
int main() {
    const double r21 = 64.0 / 27.0, r1610 = 16.0 / 10.0, r32 = 32.0 / 9.0;
    const struct { uint32_t pw, ph, ww, wh; double r; int bars; const char* what; } cases[] = {
        {3416, 1440, 1600, 900, r21, 2, "1600 x 900, 21:9 picture (bars above and below: the lab's)"},
        {3416, 1440, 3440, 1440, r21, 2, "fullscreen 3440 x 1440, 21:9 (64:27 a little narrower: bars at the sides)"},
        {3416, 1440, 1720, 720, r21, 2, "1720 x 720, 21:9"},
        {2304, 1440, 1680, 1050, r1610, 0, "1680 x 1050, 16:10 (the picture the whole window)"},
        {2560, 1440, 1920, 1080, 0, 0, "16:9"},
        {5120, 1440, 1920, 1080, r32, 2, "32:9 picture in a 16:9 window"},
        {1600, 1200, 1920, 1080, 0, 2, "4:3 picture in a 16:9 window"},
        {2560, 1440, 1601, 901, 0, -1, "an odd 16:9 window"},
    };
    for (const auto& k : cases) {
        const game_grid::Place g = game_grid::of(k.pw, k.ph, k.ww, k.wh, k.r);
        game_grid::Rect r[5];
        const int n = game_grid::dim_parts(g, (float)k.ww, (float)k.wh, r);
        float sum = 0;
        for (int i = 0; i < n; i++) {
            sum += area(r[i]);
            CHECK(area(r[i]) > 0);
            for (int j = 0; j < i; j++) CHECK(!overlap(r[i], r[j]));
        }
        std::printf("%-70s picture %g,%g %gx%g, %d bars\n", k.what, g.pic_x, g.pic_y, g.pic_w, g.pic_h, n - 1);
        CHECK(std::fabs(sum - (float)k.ww * k.wh) < 0.5f);   // (the window exactly)
        CHECK(r[0].x0 == g.pic_x && r[0].y0 == g.pic_y && r[0].x1 == g.pic_x + g.pic_w && r[0].y1 == g.pic_y + g.pic_h);
        if (k.bars >= 0) CHECK(n - 1 == k.bars);
    }
    // the lab's window: the picture 1600 x 675 from 112 down, the bars above (112) and below (113) it
    {
        game_grid::Rect r[5];
        CHECK(game_grid::dim_parts(game_grid::of(3416, 1440, 1600, 900, r21), 1600, 900, r) == 3);
        CHECK(r[0].x0 == 0 && r[0].y0 == 112 && r[0].x1 == 1600 && r[0].y1 == 787);
        CHECK(r[1].y0 == 0 && r[1].y1 == 112 && r[1].x0 == 0 && r[1].x1 == 1600);
        CHECK(r[2].y0 == 787 && r[2].y1 == 900 && r[2].x0 == 0 && r[2].x1 == 1600);
    }
    // no picture known yet: the whole window, one part
    {
        game_grid::Rect r[5];
        CHECK(game_grid::dim_parts(game_grid::of(0, 0, 1600, 900, 0), 1600, 900, r) == 1 && area(r[0]) == 1600.0f * 900);
    }
    // the blend: a bar's black under each of our dims, before (rgba(8, 5, 2, a): lifted) and now (black at a: still 0); over
    // the picture the dim's colour as it was, the same result
    for (int a8 : {110, 120, 220}) {
        const float a = a8 / 255.0f;
        CHECK(over(0, 8, a) > 3);                 // (was: the bar's red channel lifted, 3.5 to 6.9 of 255)
        CHECK(over(0, 0, a) == 0);                // (now: black)
        for (float p : {0.0f, 20.0f, 140.0f, 255.0f}) CHECK(over(p, 0, a) <= p);   // (black never lightens)
    }
    if (!fails) std::puts("the dim's parts: the picture in its colour, the bars black, the window exactly");
    return fails ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")

    dw = block(menu, "void dim_window(ImDrawList* d, const game_grid::Map& m, const game_grid::Place& g, float w, float h, ImU32 col) {")
    check("const int n = game_grid::dim_parts(g, w, h, r);" in dw and "i ? (col & IM_COL32_A_MASK) : col" in dw,
          "dim_window: the bars are not black under the dim, or the picture not in its colour")
    check("m.back_x(r[i].x0), m.back_y(r[i].y0)" in dw, "dim_window: the parts are not in the drawing's pixels")
    dp = block(menu, "void dim_page(ui::Ctx& c, ImU32 col) {")
    check("dim_window(c.d, menu_map, game_black ? menu_grid(w, h) : game_grid::Place{}, w, h, col);" in dp and "AddRectFilled" not in dp,
          "dim_page: its dim is one rectangle over the whole window inside the game's Options screen, the bars lifted")
    # ((11-20, a review of 408e2dc) the bars black only where the game's own black is under the dim: inside its Options screen
    # (and the Mods entry's row); over play draw_menu covered them with our own background, one rectangle there)
    check("const bool game_black = embed_open || page == kModsRowPage;" in dp,
          "dim_page: over play (our own background over the bars) the dim is cut black at the picture's edges")
    dm = block(menu, "void draw_menu(ui::Ctx& c, bool perf_under) {")
    check("if (!embed_open && !row && !ui::system_bg(c)) c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), ui::rgba(8, 5, 2, ui::kDimAlpha));" in dm,
          "draw_menu: over play our background no longer covers the whole window (dim_page's one rectangle there is for it)")
    ask = block(menu, "void draw_quit_ask(ui::Ctx& c) {")
    check("dim_window(c.d, menu_map, menu_grid(menu_window_w, menu_window_h), menu_window_w, menu_window_h, ui::rgba(8, 5, 2, 220));" in ask and
          "ui::rgba(8, 5, 2, 220));" not in ask.replace("menu_window_h, ui::rgba(8, 5, 2, 220));", ""),
          "Quit's dialog: its dim is one rectangle over the whole window, the bars lifted")
    for head, dim in (("void draw_ask(ui::Ctx& c) {", "dim_page(c, ui::rgba(8, 5, 2, 110));"),
                      ("void mods_dialog(ui::Ctx& c, const char* title,", "dim_page(c, ui::rgba(8, 5, 2, 120));"),
                      ("void mods_keyboard(ui::Ctx& c) {", "dim_page(c, ui::rgba(8, 5, 2, 120));")):
        check(dim in block(menu, head), head + " its dim is not dim_page's")
    # (the Full panel over our dialogs' dim: waiting on every page but the Mods entry's, drawn after the dim)
    check(dp.index("panel_merge(c.d);") < dp.index("dim_window(") < dp.index("panel_over(c.d);"),
          "dim_page: the Full panel is under our dialog's dim (the game's own dialogs never dim the overlay)")
    check("const bool perf_between = perf_under && !row;" in block(menu, "void draw_menu(ui::Ctx& c, bool perf_under) {"),
          "draw_menu: the Full panel is drawn under the restart question's dim, or under the page over play")

    with tempfile.TemporaryDirectory(prefix="dialog-dim-") as tmp:
        tmp = Path(tmp)
        src, exe = tmp / "grid.cpp", tmp / "grid.exe"
        src.write_text(GRID, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(VK), str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout.strip())
        check(r.returncode == 0, "the dim's parts:\n" + r.stdout)
    print("PASS: our dialogs' dim leaves the bars black, the Full panel bright over it")


if __name__ == "__main__":
    main()
