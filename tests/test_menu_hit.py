"""The Insert menu's mouse: the hit-test (gpu/vk/vk_menu_hit.h) compiled and run against positions over the controls as drawn
(tests/test_menu_hit.cpp: the options, the Controls page, a list, the restart question), and the sources checked for the rest of
the path: the window lets the mouse go while the menu is open and takes it back after (WM_APP + 8, host/display.c and
host/posix/display_sdl.c), the free mouse reaches the menu (MkwVkHost.pointer, host/rx_backend.c), and the menu records each
control it draws and tests the mouse against that record, so its picture and its hit-test cannot drift apart.
(Triforce) A toggle's halves are split where they are drawn to meet, and a click on one picks that value (not a flip).
Inside the game's own Options screen the tabs over its header are recorded and read too (draw_game_tab, before
any of ours is open): the window hands their clicks to the menu while that screen is in front (host/display.c
game_menu_press), a tab clicked is shown, and a click from before the menu read the mouse is not taken.

    python -I -B tests/test_menu_hit.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8")


def sources():
    menu, api, backend = read("gpu", "vk", "vk_menu.cpp"), read("gpu", "vk", "vk_api.h"), read("host", "rx_backend.c")
    win, sdl = read("host", "display.c"), read("host", "posix", "display_sdl.c")
    if '#include "vk_menu_hit.h"' not in menu or "hits.at(" not in menu or "hits.clear(" not in menu or "hits.drag(" not in menu:
        raise SystemExit("the menu does not hit-test with vk_menu_hit.h's record of what it drew")
    for part in ("kTabPrev", "kTabNext", "kTab", "kRow", "kCtlRow", "kCtlCell", "kListLine", "kButton"):
        if not re.search(r"hits\.add\([^;]*menu_hit::%s\b" % part, menu):
            raise SystemExit("the menu draws a control it does not record (menu_hit::%s): the mouse cannot reach it" % part)
    if "PostMessageA(app_window(), WM_APP + 8" not in menu:
        raise SystemExit("the menu does not tell the window it opened or closed")
    if "int (*pointer)(MkwVkPointer *out);" not in api or "hos_mouse_pointer," not in backend:
        raise SystemExit("the free mouse does not reach the renderer")
    for name, src in (("display.c", win), ("display_sdl.c", sdl)):
        if "int hos_mouse_pointer(MkwVkPointer *out)" not in src:
            raise SystemExit(name + ": no hos_mouse_pointer")
        if "WM_APP + 8" not in src or "captured_before_menu" not in src:
            raise SystemExit(name + ": the menu does not release the mouse and give the capture back")
    # (a click with the menu open is the menu's: it never captures the mouse for the game)
    down = sdl[sdl.index("case HOS_SDL_EV_MOUSE_DOWN:"):]
    down = down[:down.index("break;")]
    if "if (menu_open)" not in down or "else if (renderer && !mouse_captured) mouse_capture(1);" not in down:
        raise SystemExit("display_sdl.c: a click with the menu open captures the mouse")
    down = win[win.index("case WM_LBUTTONDOWN:"):]
    down = down[:down.index("return 0;")]
    if "if (menu_open)" not in down or "else if (renderer && !mouse_captured) mouse_capture(w, 1);" not in down:
        raise SystemExit("display.c: a click with the menu open captures the mouse")
    # ((Triforce) a toggle's halves: split where halves_cell draws them meeting, on the tabs' rows (the Debug tab's among
    # them since 11-17: one loop draws every tab); a half picks)
    if "float halves_meet(ImVec2 a, ImVec2 b)" not in menu or "const float xm = halves_meet(a, b);" not in menu:
        raise SystemExit("the menu's toggles are not split for the mouse where they are drawn to meet")
    if len(re.findall(r"hits\.add\([^;]*menu_hit::kRow[^;]*, vm\);", menu)) != 1:
        raise SystemExit("a page of rows records its toggles without where their halves meet")
    if "if (*d.value != (hit.side < 0 ? d.hi : d.lo)) press[4] = true;" not in menu:
        raise SystemExit("a click on a toggle's half does not pick that value")
    # ((Triforce) the tabs over the game's own Options screen: recorded as drawn, read on the game's own tab, a click opens one)
    game_tab = menu[menu.index("void draw_game_tab(ui::Ctx& c) {"):]
    game_tab = game_tab[:game_tab.index("\n}\n")]
    if "hits.clear(0, 0, k.w, k.h);" not in game_tab or "draw_tabs(k, -1, true, true);" not in game_tab or "hits_game_tab = true" not in game_tab:
        raise SystemExit("draw_game_tab does not record the tabs it draws over the game's header")
    embed = menu[menu.index("if (in_game_options && !open_now.load()) {"):]
    embed = embed[:embed.index("tab_left = tab_right = false;")]
    for need in ("host->pointer(&gp)", "hits.at(game_tab_map.back_x((float)ix), game_tab_map.back_y((float)iy))", "if (!game_tab_read) gp.pressed = 0",
                 "clicked_tab = hover_tab", "cur_tab = clicked_tab >= 0 ? clicked_tab"):
        if need not in embed:
            raise SystemExit("the game's own tab does not take the mouse on our tabs: " + need)
    if "if (!mouse_read) ptr.pressed = 0;" not in menu:
        raise SystemExit("the open menu takes a click from before it read the mouse")
    press = win[win.index("static void game_menu_press(LPARAM lp, LONG bit) {"):]
    press = press[:press.index("\n}\n")]
    if "if (bit == HOS_MOUSE_LEFT && hos_oot_options_front()) InterlockedOr(&ptr_pressed, bit);" not in press:
        raise SystemExit("display.c: the game's Options screen's clicks do not reach the tabs over its header")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    sources()
    with tempfile.TemporaryDirectory(prefix="menu-hit-") as tmp:
        exe = Path(tmp) / "menu-hit"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_menu_hit.cpp"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the menu's mouse")


if __name__ == "__main__":
    main()
