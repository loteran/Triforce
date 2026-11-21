"""The options menu drawn as the game's own Options screen looks (gpu/vk/vk_game_tex.h, vk_ui.h, vk_menu.cpp; no game).

The colours are the game's layouts' own (UI/Layout2Archive/Common.*.blarc: each pane's material has a white and a black
colour, linear, and its texture picks between them), written into vk_game_tex.h and vk_ui.h as numbers. This compiles
vk_game_tex.h's makers with made-up textures and checks what they give against the game's Options screen as measured:

- the chosen value's orange plate is 212,145,37 where its texture is white (measured 211,144,37), darker where the
  texture is grey, as the game's: not the texture times one colour, which made its middle 168,115,29;
- the value's dark panel, the chosen tab's plate and a mask (white) take their white colour where the texture is white
  and their black colour where it is black, the texture's alpha kept;
- a row's plate is darker at its rim (98,74,37 measured) than in its middle (111,84,44), where the grain texture is
  white and dark: the browns ran the other way before;
- the standalone background's shade is shrunk without changing its mean.

And reads the source for what only the game shows: inside the game's own Options screen nothing dims the game's own
background and only the opaque middle of the column is drawn over the game's identical one (its soft edges stay single);
over play the System screens' shade and noise, or the flat dim without them; the two textures that needs are optional,
so a romfs without them keeps the rest of the game's look; the plates, the tab and the separator use the layouts'
colours.

    python -I -B tests/test_menu_vanilla_look.py [--cxx clang++]
"""
import argparse
import re
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
#include <cstdio>
#include <cstdlib>
#include "vk_game_tex.h"
using namespace mkwvk::game_tex;
static int fails = 0;
static void near(const unsigned char* px, int r, int g, int b, int a, int tol, const char* what) {
    if (std::abs(px[0] - r) > tol || std::abs(px[1] - g) > tol || std::abs(px[2] - b) > tol || px[3] != a) {
        std::printf("FAIL: %s: %d,%d,%d,%d, not %d,%d,%d,%d\n", what, px[0], px[1], px[2], px[3], r, g, b, a);
        fails++;
    }
}
static Image grey(std::initializer_list<int> values, bool srgb, int alpha = 255) {
    Image im;
    im.w = (uint32_t)values.size(), im.h = 1, im.srgb = srgb;
    for (int v : values) im.rgba.insert(im.rgba.end(), {(unsigned char)v, (unsigned char)v, (unsigned char)v, (unsigned char)alpha});
    return im;
}
int main() {
    // the chosen value's plate (W_CheckL_00): white texel the white colour, the measured orange; grey darker, as the game's
    const Image el = grey({255, 203, 0}, true);
    const Image on = make_tinted(el, kValueOnBlack, kValueOnWhite);
    near(&on.rgba[0], 211, 144, 37, 255, 1, "the orange plate where its texture is white");
    near(&on.rgba[4], 191, 126, 31, 255, 2, "the orange plate where its texture is 203 (its middle)");
    near(&on.rgba[8], 153, 89, 19, 255, 1, "the orange plate where its texture is black");
    // the value's dark panel (W_Base_01) and the tab (P_PlusMenuBg_00)
    const Image panel = make_tinted(el, kValueBlack, kValueWhite);
    near(&panel.rgba[0], 15, 14, 11, 255, 1, "the value's panel where its texture is white");
    near(&panel.rgba[8], 51, 45, 27, 255, 1, "the value's panel where its texture is black");
    const Image tab = make_tinted(grey({255, 0}, true, 128), kTabBlack, kTabWhite);
    near(&tab.rgba[0], 146, 113, 66, 128, 1, "the tab's plate where its texture is white (its alpha kept)");
    near(&tab.rgba[4], 50, 13, 0, 128, 1, "the tab's plate where its texture is black");
    // a mask (BC4: white, its strength in alpha) is its white colour
    Image mask = grey({255}, false, 77);
    near(&make_tinted(mask, kValueOnBlack, kValueOnWhite).rgba[0], 212, 145, 37, 77, 1, "a mask tinted");
    // a row's plate: the grain white at the rim, dark in the middle; the line over it where its mask is
    Image shape = grey({255, 255, 255}, true), grain = grey({250, 80, 0}, true), line = grey({255, 255, 255}, false, 0);
    line.rgba[2 * 4 + 3] = 255;
    const Image plate = make_plate(shape, grain, &line, kPlateBlack, kPlateWhite, kPlateLine);
    near(&plate.rgba[0], 98, 74, 37, 255, 2, "a row's plate at its rim (the grain white)");
    near(&plate.rgba[4], 111, 84, 44, 255, 2, "a row's plate in its middle (the grain dark)");
    near(&plate.rgba[8], 124, 104, 62, 255, 1, "a row's plate under its line");
    if (plate.rgba[0] >= plate.rgba[4]) std::printf("FAIL: the plate's rim is not darker than its middle\n"), fails++;
    // the shade shrunk: each texel its block's mean
    Image big;
    big.w = 8, big.h = 4, big.mask = true;
    big.rgba.assign(8 * 4 * 4, 255);
    for (int i = 0; i < 32; i++) big.rgba[i * 4 + 3] = (unsigned char)((i % 8) < 4 ? 0 : 200);
    const Image small = shrink_mask(big, 4);
    if (small.w != 2 || small.h != 1 || small.rgba[3] != 0 || small.rgba[7] != 200 || !small.mask)
        std::printf("FAIL: shrink_mask: %ux%u %d %d\n", small.w, small.h, small.rgba[3], small.rgba[7]), fails++;
    return fails ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    tex = (VK / "vk_game_tex.h").read_text(encoding="utf-8")
    ui = (VK / "vk_ui.h").read_text(encoding="utf-8")
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")

    # the background: inside the game's Options screen, its own; over play, the System screens' shade and noise
    # (v0.5.1: draw_menu takes perf_under, and over the Mods entry (row) dims nothing)
    draw = function(menu, "void draw_menu(ui::Ctx& c, bool perf_under) {")
    check("if (!embed_open && !row && !ui::system_bg(c)) c.d->AddRectFilled(" in draw,
          "draw_menu dims the game's own Options screen, or not the System screens' way over play")
    check(draw.count("AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h)") == 1, "draw_menu dims the window more than once")
    check("ui::column_bg(k, cx0, cx1, k.over_game);" in draw and "k.over_game = embed_open;" in draw,
          "over the game's Options screen the column is not only its opaque middle")
    column = function(ui, "inline void column_bg(")
    check("60.0f / 1440" in column and "1380.0f / 1440" in column and "PushClipRect(clip0, clip1, true)" in column,
          "column_bg's inner part is not the texture's opaque middle")
    check("at(c, 0, 1240 + 2500)" in column and "at(c, 0, 1240 - 2500)" in column and "lu(c, 20)" in column,
          "column_bg is not P_OptionBg_00's place (2240 x 5000 units, 1240 up, seen through S_Scissor_00)")
    bg = function(ui, "inline bool system_bg(")
    check('art("SystemBgSh_00")' in bg and 'art("DecoNoise_00")' in bg and "return false;" in bg, "system_bg does not fall back")
    check("kNoiseAlpha = 102, kShadeAlpha = 179;" in ui, "the shade and the noise are not the layout's 0.7 and 0.4")
    check("kBgNoise = rgba(28, 13, 13)" in ui and "kSeparator = rgba(75, 57, 34)" in ui and "kInk = rgba(255, 254, 228)" in ui,
          "vk_ui.h's colours are not the layouts' own")
    # (v0.5.1: the colour is a parameter, the performance overlay asks for its own; kSeparator is its default)
    check("ImU32 col = kSeparator, ImU32 line_col = 0) {" in ui, "the separator is not the layout's colour")

    # the skin: the two background textures optional; the baked plates made and used
    skin = function(tex, "inline bool make_skin(")
    names = re.search(r"names\[\] = \{(.*?)\};", skin, re.S).group(1)
    optional = re.search(r"optional\[\] = \{(.*?)\};", skin, re.S).group(1)
    check("SystemBgSh_00" not in names and "DecoNoise_00" not in names, "a background texture is a required one")
    check('"SystemBgSh_00", "DecoNoise_00"' in optional, "the background textures are not the optional ones")
    loop = skin[skin.index("for (const char* n : optional)"):]
    loop = loop[:loop.index("\n    }\n") + 6]
    check("continue;" in loop and "return false" not in loop, "a missing optional texture throws the skin away")
    for made in ('"value_plate", make_tinted(*element, kValueBlack, kValueWhite)',
                 '"value_plate_on", make_tinted(*element, kValueOnBlack, kValueOnWhite)',
                 '"tab_select", make_tinted(*tab, kTabBlack, kTabWhite)',
                 '"plate_row", make_plate(*shape, *grain, skin.get("BtnThinBase_04"), kPlateBlack, kPlateWhite, kPlateLine)'):
        check(made in skin, "make_skin does not make " + made.split(",")[0])
    plate = function(ui, "inline void value_plate(")
    check('art(on ? "value_plate_on" : "value_plate")' in plate and "alpha(IM_COL32_WHITE, k)" in plate,
          "value_plate does not draw the baked plates untinted")
    check('ui::art("tab_select")) ui::image(c, t, a, b);' in function(menu, "void draw_tabs("), "the chosen tab is not the baked plate")

    with tempfile.TemporaryDirectory(prefix="vanilla-look-") as tmp:
        src, exe = Path(tmp) / "look.cpp", Path(tmp) / "look.exe"
        src.write_text(PROGRAM, encoding="utf-8")
        inc = [VK, ROOT / "gpu" / "third_party" / "suyu", VK / "compat", ROOT / "gpu" / "compat"]
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-w", *("-I" + str(p) for p in inc), str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        check(r.returncode == 0, "the colours as made:\n" + r.stdout)
    print("PASS: the menu's plates, tab, separator and background in the game's layouts' colours")


if __name__ == "__main__":
    main()
