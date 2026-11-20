"""The overlays (the start-up screen, the menu, the FPS counter) over the window's image: menu_record draws them with Dear ImGui
in one frame and one recording, the start-up screen under the menu and the counter over both; nothing at all (no ImGui frame,
no render pass, only ui::idle letting old textures go) while none shows (but one frame that bakes the glyphs after a resize), and no "0 FPS" over the start-up screen.

(Triforce) The FPS counter row's Full (opts.show_fps == 2) draws the performance overlay's panel in the counter's place, as
NX2navi's Full draws its panel alone: no counter then; with the options open the panel is drawn by draw_menu itself, over its
dim and under its pages (NX2navi keeps its panel under its menu's window). 0 and 1 are the row's Off and On, as before. A bug
capture's picture (bugshot_record) is taken before any of it is drawn, and not over the start-up screen. menu_record_image
(a still: a headless shot, tests/ui_preview.cpp's pictures) draws the same in the same order, with no bake, no capture and no
start-up screen.

python -I -B tests/test_fps_overlay_order.py [--cxx g++] [--source path/to/vk_menu.cpp]
Executes the real menu_record and menu_record_image with recording stubs; no GPU or game data.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_menu.cpp")
    a = p.parse_args()
    src = a.source.read_text(encoding="utf-8")
    fn = block(src, "void menu_record(") + "\n" + block(src, "void menu_record_image(")
    fixture = r'''#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using VkCommandBuffer = int;
using VkImage = int;
using VkImageView = int;
struct { int show_fps = 0; } opts;
std::atomic<bool> open_now{false}, startup_on{false};
std::string calls;
// (what menu_record looks at besides: a notice at the foot of the screen and the game's own Options screen, neither here)
std::atomic<int64_t> game_options_ns{0};
int64_t embed_since = 0, notice_until = 0;
char notice_text[64] = "";
bool embed_allowed() { return true; }
bool game_options_shown() { return game_options_ns.load() != 0; }   // (menu_record_image's look at the game's Options screen)
void bugshot_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) { CHECK(cmd == 1 && index == 2 && w == 640 && h == 360); calls += "B"; }
bool can_draw = true;
float ease_step = 0;
bool ease_live = false;   // (menu_record's frames step the easing; a still, menu_record_image, is drawn settled)
int64_t prof_now() { return 1000; }
namespace ui {
struct Ctx {};
bool begin(uint32_t, uint32_t) { calls += "b"; return can_draw; }
void idle() { calls += "i"; }
void record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) { CHECK(cmd == 1 && index == 2 && w == 640 && h == 360); calls += "r"; }
void record_to(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h) {   // (a still's: "R")
    CHECK(cmd == 1 && image == 3 && view == 4 && w == 640 && h == 360); calls += "R";
}
}
namespace mkwvk::rec {   // (RX_VK_RECORDER: the boundary before each Dear ImGui recording, logged as "d")
enum Bound { B_DIRECT };
void boundary(Bound) { calls += "d"; }
}
ui::Ctx ui_ctx(uint32_t, uint32_t) { return {}; }
ui::Ctx menu_ctx(uint32_t, uint32_t) { return {}; }
void prebake(const ui::Ctx&) { calls += "p"; }
bool resized = false;
bool prebake_needed(uint32_t, uint32_t) { return resized; }
void draw_startup(ui::Ctx&) { calls += "s"; }
void draw_menu(ui::Ctx&, bool perf_under) { calls += perf_under ? "M" : "m"; }   // ("M": with the Full panel under its pages)
void draw_fps(ui::Ctx&, bool under_banner) { calls += under_banner ? "F" : "f"; }
void draw_perf(ui::Ctx&) { calls += "P"; }
void draw_game_tab(ui::Ctx&) { calls += "g"; }
bool mods_list_fresh() { return false; }   // ((Triforce) no game list with the Mods entry under it here)
int page = 0;                              // ((Triforce) the menu's page: not the Mods entry's (kModsRowPage) here
constexpr int kModsRowPage = 9;
void draw_mods_row_shut(ui::Ctx&) { calls += "M"; }
void draw_notice(ui::Ctx&) { calls += "n"; }
''' + fn + r'''
int main() {
    for (bool startup : {false, true}) for (int fps : {0, 1, 2}) for (bool menu : {false, true}) for (bool can : {true, false}) {
        opts.show_fps = fps;
        open_now = menu;
        startup_on = startup;
        can_draw = can;
        calls.clear();
        menu_record(1, 2, 640, 360);
        // (a bug capture's picture first, before any overlay is drawn; none of the start-up screen)
        const std::string shot = startup ? "" : "B";
        if (!startup && !fps && !menu) {   // (nothing shows: no ImGui frame at all, only the reaping; after a resize, one bake)
            CHECK(calls == shot + "i");
            calls.clear(), resized = true;
            menu_record(1, 2, 640, 360);
            CHECK(calls == shot + (can ? "bpdr" : "bi"));
            resized = false;
            continue;
        }
        if (!can) { CHECK(calls == shot + "b"); continue; }                    // (no ImGui here: nothing recorded)
        std::string want = shot + "bp";
        if (startup) want += "s";
        const bool full = fps == 2 && !startup;
        if (menu) want += full ? "M" : "m";   // (Full with the options open: the panel under them, drawn by draw_menu)
        else if (full) want += "P";           // (Full: the panel alone)
        if (fps && !startup && !full) want += menu ? "F" : "f";   // (the counter on On only: Full draws none)
        want += "dr";   // (the recorder boundary right before the recording)
        CHECK(calls == want);
    }
    // ((Triforce) the game's own Options screen with our header's tabs over it: the Full panel under them, the counter of On
    // under the header)
    game_options_ns = 999, embed_since = 1;
    for (int fps : {0, 1, 2}) {
        opts.show_fps = fps, open_now = false, startup_on = false, can_draw = true;
        calls.clear();
        menu_record(1, 2, 640, 360);
        CHECK(calls == std::string("Bbp") + (fps == 2 ? "Pg" : fps == 1 ? "gF" : "g") + "dr");
    }
    game_options_ns = 0, embed_since = 0;
    // ((Triforce) a still, menu_record_image: the same overlays in the same order (Full's panel in the counter's place, under
    // an open menu), no bake, no bug capture, no start-up screen; nothing at all while none shows)
    for (int fps : {0, 1, 2}) for (bool menu : {false, true}) for (bool can : {true, false}) for (bool tab : {false, true}) {
        opts.show_fps = fps, open_now = menu, startup_on = false, can_draw = can;
        game_options_ns = tab ? 999 : 0, embed_since = tab;
        calls.clear();
        menu_record_image(1, 3, 4, 640, 360);
        const bool game_tab = tab && !menu, full = fps == 2;
        if (!fps && !menu && !game_tab) { CHECK(calls.empty()); continue; }
        if (!can) { CHECK(calls == "b"); continue; }
        std::string want = "b";
        if (menu) want += full ? "M" : "m";
        else {
            if (full) want += "P";
            if (game_tab) want += "g";
        }
        if (fps && !full) want += menu || game_tab ? "F" : "f";
        CHECK(calls == want + "dR");
    }
    game_options_ns = 0, embed_since = 0;
    std::printf("PASS: one ImGui frame and recording for whatever shows, in order, nothing while nothing shows (a still's too)\n");
}
'''
    with tempfile.TemporaryDirectory(prefix="fps-overlay-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
