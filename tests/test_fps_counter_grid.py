"""The FPS counter over play, the title and the pause menu on the game's grid, at its 16:9 place among the game's header items
in every window and picture shape (vk_menu.cpp draw_fps, gpu/vk/game_grid.h column_of; no game).

(Triforce, 11-20) The lab's check 3 of 26670c0, a 1680 x 1050 window with a 16:10 picture (fps_counter=1): "on the title
and the pause menu the counter ends at x about 1445, about 235 px in from the right edge; on the pause menu it sits on the
game's header between RB and the rupee counter" (16x10_02_title_down_mods.png, 16x10_31_sys_down_mods.png). Between the
pause menu's R and its rupees is the counter's place at 16:9 (draw_fps: its right end 500 units left of the screen's right
edge, 60 units down, measured on the game's own 1920 x 1080 screens). Over play the counter was laid out on the window's
16:9 column of its whole height: at 16:10 the game's layouts are squeezed across (x 0.9, the grid the whole picture) and the
counter was not, so it ended 500 window units (243 px) in from the edge where the game's items had moved 10% toward the
edge, toward the middle of the gap; in a window with bars above and below the picture it was at the window's top, in the bar.

1. game_grid.h alone: the counter's right end and middle as draw_fps places them on column_of(..., true): its right end
   1420 units right of the grid's middle (the game's units: 3840 across the grid) and its middle 60 units under the grid's
   top in every shape, so the same distance from the pause menu's R (its right end 1044 units right of the middle,
   MenuOverlay_00's T_BtnR_00) and its rupees as at 16:9; unchanged where the picture fills a 16:9 or wider window's
   height; inside the picture with bars above and below. The old place at 16:10 is checked to be off by the lab's amount.
2. The source: draw_fps over play on the grid (on_game unless under our header), put there through put_on_grid.
3. ((11-20, a review of ca33bee) Under our header the counter is on the column draw_menu lays the page and its header on,
   on every page, the restart question (page 5, under our "Restart Needed" banner) too: draw_fps's and draw_menu's own
   on_game expressions, compiled and run for each page over play and inside the game's Options screen. ca33bee had put
   page 5 on the grid always: over play (F1) in a 1600 x 900 window with a 21:9 picture the counter left the banner it
   sat over at (1392, 25) for (1244, 131), over the dimmed page.

    python -I -B tests/test_fps_counter_grid.py [--cxx clang++]
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
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include "game_grid.h"
using namespace mkwvk;
static int fails = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%d %s\n", __LINE__, #x); fails++; } } while (0)
static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
struct Spot { float x1, my, kx, units_x1, units_my; game_grid::Place g; };
// (as draw_fps over play: the column (pages_column(w, h, true)), on it the HUD's 16:9 width (the column's), the right end
// lu(500) left of it and the middle lu(60) down, lu = h / 2160; put through the map. In the game's units on the grid too)
static Spot counter(uint32_t pw, uint32_t ph, uint32_t ww, uint32_t wh, double r) {
    Spot s;
    s.g = game_grid::of(pw, ph, ww, wh, r);
    const game_grid::Column c = game_grid::column_of(s.g, (float)ww, (float)wh, true);
    const float lu = c.h / 2160.0f, hud_w = std::min(c.w, std::floor(c.h * 16.0f / 9.0f + 0.5f)), hud_x1 = (c.w + hud_w) * 0.5f;
    s.x1 = c.map.x(hud_x1 - 500 * lu), s.my = c.map.y(60 * lu), s.kx = c.map.kx;
    s.units_x1 = (s.x1 - (s.g.x + s.g.w * 0.5f)) * 3840 / s.g.w, s.units_my = (s.my - s.g.y) * 2160 / s.g.h;
    return s;
}
// (as it was over play: the window's own context, the HUD the window's middle 16:9 of its whole height)
static float old_x1(float ww, float wh) {
    const float lu = wh / 2160.0f, hud_w = std::min(ww, std::floor(wh * 16.0f / 9.0f + 0.5f));
    return (ww + hud_w) * 0.5f - 500 * lu;
}
int main() {
    const double r21 = 64.0 / 27.0, r1610 = 16.0 / 10.0, r32 = 32.0 / 9.0;
    const struct { uint32_t pw, ph, ww, wh; double r; const char* what; } cases[] = {
        {2560, 1440, 1920, 1080, 0, "16:9"},
        {2304, 1440, 1680, 1050, r1610, "the lab's 1680 x 1050 window, 16:10 picture"},
        {3416, 1440, 1600, 900, r21, "1600 x 900 window, 21:9 picture between bars"},
        {3416, 1440, 1720, 720, r21, "1720 x 720 window, 21:9"},
        {3416, 1440, 3440, 1440, r21, "fullscreen 3440 x 1440, 21:9"},
        {5120, 1440, 5120, 1440, r32, "32:9"},
        {2560, 1440, 1600, 1200, 0, "4:3 window, 16:9 picture between bars"},
        {1600, 1200, 1600, 1200, 0, "4:3 window and picture"},
    };
    for (const auto& k : cases) {
        const Spot s = counter(k.pw, k.ph, k.ww, k.wh, k.r);
        std::printf("%-46s right end %7.1f, middle %6.1f (units: %6.1f right of the middle, %5.1f down), x %.3f\n", k.what, s.x1, s.my,
                    s.units_x1, s.units_my, s.kx);
        // (its 16:9 place on the game's grid: 1420 units right of the middle (500 in from the grid's edge), 60 down)
        CHECK(near(s.units_x1, 1420, 2) && near(s.units_my, 60, 2));   // (the column on whole pixels: half a pixel)
        // (inside the picture, right of the pause menu's R (to 1044 units) by as much as at 16:9)
        CHECK(s.my > s.g.pic_y && s.x1 < s.g.pic_x + s.g.pic_w && s.units_x1 - 1044 > 300);
    }
    // where the picture fills a 16:9 or wider window's height: where it was
    {
        CHECK(near(counter(2560, 1440, 1920, 1080, 0).x1, old_x1(1920, 1080), 0.5f));
        CHECK(near(counter(3416, 1440, 1720, 720, r21).x1, old_x1(1720, 720), 0.5f));
        CHECK(near(counter(3416, 1440, 3440, 1440, r21).x1, old_x1(3440, 1440), 0.5f));
        CHECK(near(counter(2560, 1440, 1920, 1080, 0).my, 30, 0.01f));
    }
    // the lab's 16:10 window: it was 243 px in from the right edge (1437; the lab saw it end at about 1445), 1365 units right
    // of the middle on the squeezed grid where the game's items are at their 16:9 units; now 1420 units, 218 px in (500
    // units squeezed x 0.9), squeezed across with the game's header
    {
        const Spot s = counter(2304, 1440, 1680, 1050, r1610);
        const float was = old_x1(1680, 1050), was_units = (was - 840) * 3840 / 1680;
        CHECK(near(was, 1437, 1) && near(was_units, 1365, 2));
        CHECK(near(s.x1, 1461.5f, 1) && near(1680 - s.x1, 500 * 1050 / 2160.0f * 0.9f, 1) && near(s.kx, 0.9f, 0.001f));
    }
    // bars above and below: it was at the window's top, in the bar (25 px down where the picture starts at 113)
    {
        const Spot s = counter(3416, 1440, 1600, 900, r21);
        CHECK(60 * 900 / 2160.0f < s.g.pic_y && s.my > s.g.pic_y);
    }
    if (!fails) std::puts("the FPS counter at its 16:9 place on the game's grid in every shape");
    return fails ? 1 : 0;
}
"""


PAGES = r"""
#include <cstdio>
#include <initializer_list>
static int fails = 0;
static int page = 0;
static bool inside = false;   // (pages_on_game: our tabs inside the game's Options screen)
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%d %s (page %d, inside %d)\n", __LINE__, #x, page, (int)inside); fails++; } } while (0)
static bool pages_on_game() { return inside; }
static bool fps_on_game(bool under_banner) { return %FPS%; }
static bool menu_on_game(bool row) { return %MENU%; }
int main() {
    for (bool in : {false, true})
        for (int p : {0, 1, 3, 4, 5, 6, 8}) {   // (8: the Mods page, kModsPage)
            inside = in, page = p;
            CHECK(fps_on_game(true) == menu_on_game(false));   // (under our header: on the page's column, its banner's)
            CHECK(fps_on_game(true) == in);
            CHECK(fps_on_game(false));                         // (over play, the title, the pause menu: the game's grid)
        }
    if (!fails) std::puts("the counter under our header on the page's own column, the restart question's too");
    return fails ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")

    fps = block(menu, "void draw_fps(ui::Ctx& window, bool under_banner) {")
    check("const bool on_game = under_banner ? pages_on_game() : true;" in fps,
          "draw_fps: over play, the title and the pause menu the counter is on the window's column, not the game's grid")
    dm = block(menu, "void draw_menu(ui::Ctx& c, bool perf_under) {")

    def on_game_of(body, what):
        lines = [l.strip() for l in body.splitlines() if l.strip().startswith("const bool on_game = ")]
        check(len(lines) == 1, what + ": not one on_game")
        return lines[0][len("const bool on_game = "):].rstrip(";")
    check("const game_grid::Column col = pages_column(window.w, window.h, on_game);" in fps and
          "ui::Ctx c = on_game ? column_ctx(window, col) : window;" in fps and
          "if (on_game) c.d->PopClipRect(), put_on_grid(c.d, vtx0, cmd0, col.map, window_clip);" in fps,
          "draw_fps: the counter is not put on the grid it is laid out on")
    check("const float x1 = below ? hud_x1 - ui::lu(c, 80) : hud_x1 - ui::lu(c, 500);" in fps and
          "const float my = below ? banner_h(c) + ui::lu(c, 50) : ui::lu(c, 60);" in fps,
          "draw_fps: the counter's place on its column changed (the test's model is draw_fps's)")

    # 3. the counter's column against the page's: draw_fps's on_game (under_banner: our options open, not the Mods entry's
    # row) and draw_menu's (row: the Mods entry's row), for each page, over play and inside the game's Options screen
    pages = PAGES.replace("%FPS%", on_game_of(fps, "draw_fps")).replace("%MENU%", on_game_of(dm, "draw_menu"))

    with tempfile.TemporaryDirectory(prefix="fps-counter-grid-") as tmp:
        tmp = Path(tmp)
        src, exe = tmp / "grid.cpp", tmp / "grid.exe"
        src.write_text(GRID, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(VK), str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout.strip())
        check(r.returncode == 0, "the counter's place:\n" + r.stdout)
        src, exe = tmp / "pages.cpp", tmp / "pages.exe"
        src.write_text(pages, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout.strip())
        check(r.returncode == 0, "the counter's column under our header:\n" + r.stdout)
    print("PASS: the FPS counter over play on the game's grid at its 16:9 place in every shape")


if __name__ == "__main__":
    main()
