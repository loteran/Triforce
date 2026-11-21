"""Our guides' plate inside the game's Options screen covers the game's own guides band to its end, past the 16:9 column on
a picture wider than the grid (gpu/vk/game_grid.h guide_band_x1, vk_ui.h prompt_bar, vk_menu.cpp draw_menu; no game).

(Triforce, 11-20) The lab's check 3 of 26670c0: on every Triforce tab inside the game's Options screen "a plain strip of
the game's footer band shows to the right of our guides": about 75 px in the 1600 x 900 window (a letterboxed 21:9
picture, 16x9_z3_footer_game_vs_ours.png) and about 155 px in fullscreen 3440 x 1440 (fs_10_options_game_tab.png against
fs_11_tab1.png, x 3000 to 3155). Our plate ended at the column's right edge (3840 units); the game's band (SystemOverlay_00's
N_Footer_00: W_GuideBg_00 and W_GuideBg_01, right-aligned at 2110 and 2174 units right of the middle) goes on past it.
Measured on the game's own tab in shots052c, the band is seen to 238 to 240 units past the column's edge in fs, the
1720 x 720 window and the 1600 x 900 one (the window pane's last texels clear).

1. game_grid.h alone: where our plate ends, in the window, for the lab's shapes (within a few pixels of where the game's band
   was seen to end), never past the picture's right edge (the game draws nothing in the bars), and at the column's edge
   where the grid is the whole picture's width (16:9, 16:10, 4:3) or over play.
2. The source: prompt_bar's plate and its dark texture to guides_x1, the texture's own right end carried on (not stretched,
   so inside the column it is as it was); draw_menu gives the pages the band's end.

    python -I -B tests/test_guides_band.py [--cxx clang++]
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
static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
// (our plate's right end in the window: guide_band_x1 on the column the pages are drawn on, put through its map)
static float plate_end(uint32_t pw, uint32_t ph, uint32_t ww, uint32_t wh, double r, game_grid::Place* gp = nullptr,
                       float band_right = game_grid::kGuideBandRight) {
    const game_grid::Place g = game_grid::of(pw, ph, ww, wh, r);
    if (gp) *gp = g;
    const game_grid::Column c = game_grid::column_of(g, (float)ww, (float)wh, true);
    return c.map.x(game_grid::guide_band_x1(c, band_right));
}
int main() {
    const double r21 = 64.0 / 27.0, r1610 = 16.0 / 10.0, r32 = 32.0 / 9.0;
    // the lab's shapes, where the game's band was seen to end on its own tab (shots052c *_10_options_game_tab: the last
    // pixel of the band's fade): the old plate ended at the column's edge, 1399 / 1500 / 3000
    {
        game_grid::Place g;
        const float e = plate_end(3416, 1440, 1600, 900, r21, &g);   // (16x9: 674 lines, the column 201 to 1399)
        std::printf("1600 x 900, 21:9 picture: the plate to %.1f (the game's band seen to 1473 to 1478)\n", e);
        CHECK(near(e, 1475, 4) && e <= g.pic_x + g.pic_w);
        const float f = plate_end(3416, 1440, 3440, 1440, r21, &g);   // (fs: the column 440 to 3000)
        std::printf("fullscreen 3440 x 1440: the plate to %.1f (seen to 3150 to 3162)\n", f);
        CHECK(near(f, 3158, 6));
        const float h = plate_end(3416, 1440, 1720, 720, r21, &g);    // (21x9: the column 220 to 1500)
        std::printf("1720 x 720: the plate to %.1f (seen to 1577 to 1583)\n", h);
        CHECK(near(h, 1580, 4));
    }
    // where the grid is the picture's whole width the plate ends at the column's edge, as before
    {
        CHECK(near(plate_end(2560, 1440, 1920, 1080, 0), 1920, 0.5f));          // (16:9)
        CHECK(near(plate_end(2304, 1440, 1680, 1050, r1610), 1680, 0.5f));     // (16:10: squeezed, the grid the whole picture)
        CHECK(near(plate_end(2560, 1440, 1600, 1200, 0), 1600, 0.5f));          // (a 16:9 picture between bars in a 4:3 window)
        game_grid::Place g;
        const float e = plate_end(1600, 1200, 1920, 1080, 0, &g);              // (a 4:3 picture between bars at its sides)
        CHECK(near(e, g.pic_x + g.pic_w, 0.5f) && e < 1920 - 100);              // (not into the bar)
    }
    // a picture only a little wider than 16:9: the band cut at the picture's edge (the game draws nothing past it)
    {
        game_grid::Place g;
        const float e = plate_end(1998, 1080, 1998, 1080, 1998.0 / 1080, &g);
        const float col_edge = g.x + g.w;
        CHECK(e > col_edge + 1 && near(e, g.pic_x + g.pic_w, 0.5f));
    }
    // a 32:9 picture: the whole band, 240 units past the column, far inside the picture
    {
        game_grid::Place g;
        const float e = plate_end(5120, 1440, 5120, 1440, r32, &g);
        CHECK(near(e, g.x + g.w + 240 * g.h / 2160, 0.5f) && e < g.pic_x + g.pic_w);
    }
    // over play (our options over the game, not its Options screen), and with no picture known: the column's edge
    {
        const game_grid::Place g = game_grid::of(3416, 1440, 1600, 900, r21);
        const game_grid::Column play = game_grid::column_of(g, 1600, 900, false);
        CHECK(play.game_x1 == 0 && game_grid::guide_band_x1(play) == play.w);
        const game_grid::Column none = game_grid::column_of(game_grid::of(0, 0, 2560, 1080, 0), 2560, 1080, true);
        CHECK(none.game_x1 == 0 && game_grid::guide_band_x1(none) == none.w);
    }
    // ((Triforce, 11-20, a review of 1104352) the Options screen chosen from the pause menu: its footer may be MenuOverlay_00's,
    // whose band panes end 2110 units right of the middle (its fade seen out by about 3120 px in the pause menu's own
    // fs_30_sys_on_title_screen.png, fullscreen 3440 x 1440: (3120 - 1720) x 1.5 = 2100 units). Our plate ends there, not
    // at the title's 2160 (3160 px: about 33 px of a dark sliver past it over the bars' black)
    {
        CHECK(game_grid::kPauseGuideBandRight == 2110 && game_grid::kGuideBandRight == 2160);
        game_grid::Place g;
        const float f = plate_end(3416, 1440, 3440, 1440, r21, &g, game_grid::kPauseGuideBandRight);
        const float title = plate_end(3416, 1440, 3440, 1440, r21);
        std::printf("fullscreen 3440 x 1440, from the pause menu: the plate to %.1f (the title's %.1f)\n", f, title);
        CHECK(near(f, 1720 + 2110 / 1.5f, 1) && near(title - f, 33.3f, 1));
        const float e = plate_end(3416, 1440, 1600, 900, r21, &g, game_grid::kPauseGuideBandRight);   // (16x9: 674 lines)
        CHECK(near(e, 800 + 2110 * g.h / 2160, 1) && e > 1400 && near(plate_end(3416, 1440, 1600, 900, r21) - e, 16, 1));
        // (over play and on a picture no wider than the grid: the column's edge, whichever band)
        const game_grid::Column play = game_grid::column_of(game_grid::of(3416, 1440, 1600, 900, r21), 1600, 900, false);
        CHECK(game_grid::guide_band_x1(play, game_grid::kPauseGuideBandRight) == play.w);
        CHECK(near(plate_end(2560, 1440, 1920, 1080, 0, nullptr, game_grid::kPauseGuideBandRight), 1920, 0.5f));
    }
    if (!fails) std::puts("our guides' plate to the game's band's end in every shape");
    return fails ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    ui = (VK / "vk_ui.h").read_text(encoding="utf-8")
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")

    # 2. the source
    pb = block(ui, "inline float prompt_bar(Ctx& c, const Prompt* list, int n) {")
    check("const float x1 = c.over_game ? std::max(c.w, c.guides_x1) : c.w, past = c.w + lu(c, 4);" in pb,
          "prompt_bar: the plate still ends at the column's right edge over the game's Options screen")
    check("ImVec2(x1, c.h), rgba(24, 22, 12));" in pb, "prompt_bar: the plate's dark does not reach the game's band's end")
    check("image(c, t, ImVec2(band0, top), ImVec2(past, c.h), rgba(0, 0, 0, 215), true);" in pb,
          "prompt_bar: the band's texture inside the column is not drawn as it was")
    check("if (x1 > past) c.d->AddImage(t->tex->GetTexRef(), ImVec2(past, top), ImVec2(x1, c.h), ImVec2(u, 0), ImVec2(u, 1), rgba(0, 0, 0, 215));" in pb,
          "prompt_bar: past the column the band's texture does not go on as its right end is")
    check("if (x1 > c.w) c.d->AddRectFilled(ImVec2(c.w, top), ImVec2(x1, c.h), kBar);" in pb,
          "prompt_bar: without the game's texture the plain band stops at the column")
    dm = block(menu, "void draw_menu(ui::Ctx& c, bool perf_under) {")
    check("k.guides_x1 = game_grid::guide_band_x1(col, game_options_from_pause.load(std::memory_order_relaxed) ? game_grid::kPauseGuideBandRight" in dm
          and ": game_grid::kGuideBandRight);" in dm, "draw_menu: the pages do not know where the game's band ends (the title's or the pause menu's)")
    # ((11-20, a review of 1104352) the host's word 2: the Options chosen from the pause menu)
    seen = block(menu, "void menu_option_set(int id, int value) {")
    gop = seen[seen.index("if (id == HOS_OPT_GAME_OPTIONS) {"):]
    gop = gop[:gop.index("return;")]
    check("game_options_from_pause.store(value == 2, std::memory_order_relaxed);" in gop,
          "menu_option_set: the Options screen from the pause menu (the host's 2) is not kept")

    with tempfile.TemporaryDirectory(prefix="guides-band-") as tmp:
        tmp = Path(tmp)
        src, exe = tmp / "grid.cpp", tmp / "grid.exe"
        src.write_text(GRID, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(VK), str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout.strip())
        check(r.returncode == 0, "the band's end:\n" + r.stdout)
    print("PASS: our guides' plate covers the game's guides band to its end past the column")


if __name__ == "__main__":
    main()
