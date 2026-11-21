"""The Mods entry where the game's list is, whatever the window's shape (gpu/vk/game_grid.h, vk_menu.cpp draw_mods_row_shut,
draw_menu and menu_tick's mouse; no game).

(Triforce, 11-19) The lab's "Title in a 16:9 window (1600x900, 21:9 picture letterboxed): Mods drawn below the picture,
larger, far from the list": the entry was drawn on a 16:9 column of the window's whole height, which is where the game's
layouts are only when the picture fills the window's height. The game lays its lists out on a 3840 x 2160 grid in the
picture's middle 16:9 (its layouts squeezed into it on a wider picture, host/oot_wide_hud.h), the picture between black bars
as the present puts it (vk_present_rect.h). This compiles game_grid.h alone and checks the grid and where the title's
entry (its list's last button at -800 units, the entry a pitch of 120 under it) lands, in the picture and under the list:

- 16:9 window, 21:9 picture (bars above and below): the grid is the picture's height, its middle 16:9; the entry inside
  the picture (it was 40 px under it in 1600 x 900);
- 21:9 window, 21:9 picture: the window's height, its middle 16:9 (as before);
- 16:9 window, 16:9 picture: the whole window (as before); 32:9 picture in 16:9 and in 32:9 windows; a window taller than
  the picture's 16:9 (bars at the sides and at the top and bottom both);
- 16:10 picture (narrower than 16:9): the whole picture, the layouts squeezed across (kx under 1), not cut;
- no picture known yet: the window's column, as before;
- the map's way back (the mouse) undoes it;
- ((11-19) a review of 0653792) on a 16:9 grid the column on whole pixels and not stretched (the text sharp), in odd
  window sizes too; only a 16:10 grid is squeezed.

And reads the source: the shut entry and the open menu's Mods page (Mods, Quit and its dialog) drawn on the grid from the
last present's picture and shape, the mouse taken back through the same map.

    python -I -B tests/test_mods_grid.py [--cxx clang++]
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


PROGRAM = r"""
#include <cmath>
#include <cstdio>
#include <cstdint>
#include "game_grid.h"
using namespace mkwvk;
static int fails = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%d %s\n", __LINE__, #x); fails++; } } while (0)
static bool near(float a, float b, float tol = 1.0f) { return std::fabs(a - b) <= tol; }
// (a place of the grid's units (x right, y up from its middle) in the window, as ui::at on the grid's column, then put)
static void at(const game_grid::Place& g, float ux, float uy, float* x, float* y) {
    const game_grid::Map m = game_grid::map_of(g);
    const float cw = game_grid::column_w(g), unit = g.h / 720.0f;
    *x = m.x(cw * 0.5f + ux * unit / 3), *y = m.y(g.h * 0.5f - uy * unit / 3);
}
int main() {
    const double r21 = 64.0 / 27.0, r32 = 32.0 / 9.0, r1610 = 16.0 / 10.0;
    float x, y;
    // 16:9 window (1600 x 900), 21:9 picture: bars above and below; the grid the picture's height in its middle
    game_grid::Place g = game_grid::of(3416, 1440, 1600, 900, r21);
    CHECK(near(g.h, 675) && near(g.y, 112) && near(g.w, 1200) && near(g.x, 200));
    at(g, 0, -800 - 120, &x, &y);   // (the title's entry: its list's last button at -800, a pitch under it)
    CHECK(near(x, 800) && y > g.y && y < g.y + g.h && near(y, 112 + 337.5f + 920 * 675.0f / 2160, 1.5f));
    CHECK(y < 112 + 675);   // (inside the picture: it was 40 px under it, at 833)
    float lx, ly;
    at(g, 0, -800, &lx, &ly);
    CHECK(near(y - ly, 120 * 675.0f / 2160, 1.0f));   // (a pitch of the list's own under its last button)
    // 21:9 window, 21:9 picture: the window's height, its middle 16:9
    g = game_grid::of(3416, 1440, 2560, 1080, r21);
    CHECK(near(g.h, 1080) && near(g.y, 0) && near(g.w, 1920) && near(g.x, 320));
    // a 1720 x 720 window (wider than 21:9): pillarboxed picture, the grid its middle 16:9 at the window's height
    g = game_grid::of(3416, 1440, 1720, 720, r21);
    CHECK(near(g.h, 720) && near(g.y, 0) && near(g.w, 1280) && near(g.x, 220));
    // 16:9 picture in a 16:9 window: the whole window
    g = game_grid::of(2560, 1440, 1920, 1080, 0);
    CHECK(near(g.x, 0) && near(g.y, 0) && near(g.w, 1920) && near(g.h, 1080));
    CHECK(near(game_grid::map_of(g).kx, 1, 0.001f));
    // 32:9 picture in a 16:9 window and in a 32:9 one
    g = game_grid::of(5120, 1440, 1920, 1080, r32);
    CHECK(near(g.h, 540) && near(g.y, 270) && near(g.w, 960) && near(g.x, 480));
    g = game_grid::of(5120, 1440, 5120, 1440, r32);
    CHECK(near(g.h, 1440) && near(g.w, 2560) && near(g.x, 1280));
    // a window taller than 16:9 (4:3) with a 16:9 picture: bars above and below, the whole picture
    g = game_grid::of(2560, 1440, 1600, 1200, 0);
    CHECK(near(g.w, 1600) && near(g.h, 900) && near(g.y, 150) && near(g.x, 0));
    // 16:10 picture in a 16:10 window: the whole picture, squeezed across (the layouts keep the picture's height)
    g = game_grid::of(2304, 1440, 1920, 1200, r1610);
    CHECK(near(g.w, 1920) && near(g.h, 1200) && near(g.x, 0) && near(g.y, 0));
    game_grid::Map m = game_grid::map_of(g);
    CHECK(m.kx < 0.95f && near(m.kx, 1920.0f / std::floor(1200 * 16.0f / 9 + 0.5f), 0.001f));
    at(g, 1920, 0, &x, &y);
    CHECK(near(x, 1920));   // (the grid's right edge at the picture's)
    // no picture known yet: the window's column of its whole height, as before
    g = game_grid::of(0, 0, 2560, 1080, 0);
    CHECK(near(g.h, 1080) && near(g.w, 1920) && near(g.x, 320) && near(g.y, 0));
    // ((Triforce, 11-19) a 16:9 grid in a wider picture (or a 16:9 one): the column on whole pixels, not stretched, so the
    // text drawn on it (on whole pixels already) stays sharp; in the grid's middle within half a pixel. 1600 x 900 with a
    // 21:9 picture was 200.889 across and 1.00019 wide, a 1601 x 901 window half a pixel off)
    {
        const struct { uint32_t pw, ph, ww, wh; double r; } cases[] = {
            {3416, 1440, 1600, 900, r21}, {3416, 1440, 1601, 901, r21}, {3416, 1440, 1920, 1080, r21}, {3416, 1440, 1919, 1079, r21},
            {5120, 1440, 1920, 1080, r32}, {5120, 1440, 1601, 901, r32}, {3416, 1440, 2560, 1080, r21}, {3416, 1440, 1720, 720, r21},
            {2560, 1440, 1920, 1080, 0}, {2560, 1440, 1601, 901, 0}, {2560, 1440, 1600, 1200, 0}};
        for (const auto& k : cases) {
            g = game_grid::of(k.pw, k.ph, k.ww, k.wh, k.r);
            const game_grid::Map w = game_grid::map_of(g);
            const float cw = game_grid::column_w(g);
            CHECK(w.kx == 1.0f && w.ox == std::round(w.ox) && w.oy == std::round(w.oy));
            CHECK(near(w.ox + cw * 0.5f, g.x + g.w * 0.5f, 0.5f) && near(w.oy, g.y, 0.5f));
        }
    }
    // the mouse back through the map
    m = game_grid::map_of(game_grid::of(3416, 1440, 1600, 900, r21));
    CHECK(near(m.back_x(m.x(123)), 123, 0.01f) && near(m.back_y(m.y(45)), 45, 0.01f));
    if (!fails) std::puts("the game's grid in the window");
    return fails ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    shut = block(menu, "void draw_mods_row_shut(ui::Ctx& c) {")
    check("const game_grid::Place g = menu_grid(c.w, c.h);" in shut and "ui::Ctx k = grid_ctx(c, g);" in shut and
          "put_on_grid(c.d, vtx0, cmd0, game_grid::map_of(g), window_clip);" in shut,
          "the entry while the menu is shut is not drawn on the game's grid (a 16:9 column of the window's height)")
    check("std::floor(c.h * 16.0f / 9.0f" not in shut, "the shut entry still takes the window's height")
    grid = block(menu, "game_grid::Place menu_grid(float w, float h) {")
    check("present_sizes.load(" in grid and "host->aspect_ratio()" in grid and "game_grid::of(" in grid,
          "the grid is not taken from the last present's picture and its shape")
    ctx = block(menu, "ui::Ctx grid_ctx(const ui::Ctx& c, const game_grid::Place& g) {")
    check("k.h = g.h, k.w = game_grid::column_w(g);" in ctx and "k.s = ui::unit(g.h, 0, false, false, &k.safe);" in ctx,
          "the entry's sizes are not the grid's")
    dm = block(menu, "void draw_menu(ui::Ctx& c, bool perf_under) {")
    check("if (row) {" in dm and "k = grid_ctx(c, g), k.over_game = false;" in dm and "menu_map = game_grid::map_of(g);" in dm and
          "put_on_grid(c.d, vtx0, cmd0, menu_map, window_clip, perf0, perf1);" in dm, "the open menu's Mods page is not drawn on the game's grid")
    tick = block(menu, "void menu_tick() {")
    check("mx = (int)std::floor(menu_map.back_x((float)ix)), my = (int)std::floor(menu_map.back_y((float)iy));" in tick,
          "the mouse is not taken back through the map the menu was drawn with")
    with tempfile.TemporaryDirectory(prefix="mods-grid-") as tmp:
        src, exe = Path(tmp) / "grid.cpp", Path(tmp) / "grid.exe"
        src.write_text(PROGRAM, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(VK), str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        check(r.returncode == 0, "the grid:\n" + r.stdout)
        print(r.stdout.strip())
    print("PASS: the Mods entry on the game's grid in every window shape")


if __name__ == "__main__":
    main()
