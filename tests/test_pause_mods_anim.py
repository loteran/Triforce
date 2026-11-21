"""The Mods and Quit entries under the pause menu's System list, and Quit's dialog, as the game's own buttons and dialogs
(gpu/vk/focus_anim.h, vk_ui.h cursor and button, vk_menu.cpp draw_mods_row, draw_quit_ask and menu_tick, host/nn_hid.c,
host/menu_sound.c; no game).

(Triforce, 11-19) His "In the pause menu, the mods and quit tabs do not have any sound effects when selecting them, have no
animations either. And when scrolling with the controller, the mods tab gets skipped over and goes straight to quit ... The
quit menu submenu also has no animations and sound effects". The System list's buttons are the part PaCommonBtnThin_00 of
the Common archive's layout, and the game's dialogs SystemWindow_00 with three of them. This compiles focus_anim.h alone
and checks:

- the parts' curves as read from the layouts: the cursor's alpha from 0 to 1 over Select's first 4 frames, from 1 to 0
  over UnSelect's first 8; the window's alpha from 0 to 1 over In's 15 frames (slow to start), its rise from 38 units
  under its place to it over 20 (18 under at frame 5), its Out a straight fade over 2;
- a button's cursor from drawing to drawing: there at once when settled (a still), faded in and out over the frames, a
  change back half way from where it is; the window takes input once its In has played.

And reads the source for what only the game shows: the cursor drawn with its amount (the line's and the glow's alpha), the
button with it; Mods and Quit stepping their cursor every drawing, also while the menu is shut (their fade after the
cursor went back to the game's list); the dialog's question and buttons risen and everything faded from its window's
state, drawn on through its Out; no input while its In plays; both ways of opening it (A on Quit, a click on Quit) starting
the In; B and Cancel playing the Out; ((11-19) a click is A on the dialog's own buttons only, Mods and Quit under its dim
are not there for the mouse while it is open or closing; the mouse moved onto Quit gives it the cursor with the system
menu's tick, as onto Mods, and moved up off the entries onto the game's buttons gives the game its cursor back); the
controller's Down that brought the cursor onto Mods not taken again as the
entry's own Down (on to Quit), and the host's stick Down at the menu's own half travel; the system menu's tick replaced by
the decide of the same press (the sound's side is tests/menu_sound_test.c's), no cursor sound after Quit Game's decide.

    python -I -B tests/test_pause_mods_anim.py [--cxx clang++]
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
#include <cmath>
#include <cstdio>
#include "focus_anim.h"
using namespace focus_anim;
static int fails = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%d %s\n", __LINE__, #x); fails++; } } while (0)
static bool near(float a, float b, float tol = 0.01f) { return std::fabs(a - b) <= tol; }
int main() {
    // the layouts' curves
    CHECK(near(curve(kSelect, 0), 0) && near(curve(kSelect, 2), 0.5f) && near(curve(kSelect, 4), 1) && near(curve(kSelect, 8), 1));
    CHECK(near(curve(kUnSelect, 0), 1) && near(curve(kUnSelect, 4), 0.5f) && near(curve(kUnSelect, 8), 0) && near(curve(kUnSelect, 14), 0));
    CHECK(curve(kUnSelect, 1) > 0.9f);   // (not gone after a frame: the fade the game's own buttons show)
    CHECK(kSelectNs == 8 * kFrameNs && kUnSelectNs == 14 * kFrameNs && kInNs == 20 * kFrameNs && kOutNs == 2 * kFrameNs);
    // the window: In and Out
    Window w = window_at(0);
    CHECK(near(w.alpha, 0) && near(w.drop, 38));
    w = window_at(5 * kFrameNs);
    CHECK(near(w.drop, 18) && w.alpha > 0 && w.alpha < 0.3f);   // (slow to start: its slopes 0.02 and 0.14)
    w = window_at(15 * kFrameNs);
    CHECK(near(w.alpha, 1) && w.drop > 0 && w.drop < 3);
    w = window_at(20 * kFrameNs);
    CHECK(near(w.alpha, 1) && near(w.drop, 0));
    w = window_at(60 * kFrameNs);
    CHECK(near(w.alpha, 1) && near(w.drop, 0));
    CHECK(near(window_at(0, true).alpha, 1) && near(window_at(kFrameNs, true).alpha, 0.5f) && near(window_at(2 * kFrameNs, true).alpha, 0));
    CHECK(near(window_at(0, true).drop, 0));
    CHECK(!window_ready(0) && !window_ready(19 * kFrameNs) && window_ready(20 * kFrameNs));
    // a button's cursor from drawing to drawing
    Focus f;
    int64_t t = 1000000000;
    f.step(t, false);
    CHECK(near(f.shown, 0));
    f.step(t + kFrameNs, true);   // (the cursor onto it: Select)
    CHECK(near(f.shown, 0));
    f.step(t + 3 * kFrameNs, true);
    CHECK(near(f.shown, 0.5f));
    f.step(t + 5 * kFrameNs, true);
    CHECK(near(f.shown, 1));
    f.step(t + 6 * kFrameNs, false);   // (and off it: UnSelect)
    f.step(t + 7 * kFrameNs, false);
    CHECK(f.shown > 0.9f && f.shown < 1);
    f.step(t + 10 * kFrameNs, false);
    CHECK(near(f.shown, 0.5f));
    f.step(t + 11 * kFrameNs, true);   // (back half way: from where it is, no jump down or up first)
    CHECK(f.shown >= 0.49f);
    f.step(t + 15 * kFrameNs, true);
    CHECK(near(f.shown, 1));
    f.step(t + 16 * kFrameNs, false);
    f.step(t + 24 * kFrameNs, false);
    CHECK(near(f.shown, 0));
    f.step(t + 25 * kFrameNs, true, true);   // (settled: a still, a dialog just opened)
    CHECK(near(f.shown, 1));
    f.step(t + 25 * kFrameNs, true);
    CHECK(near(f.shown, 1));
    f.step(t + 26 * kFrameNs, false, true);
    CHECK(near(f.shown, 0));
    Focus first;   // (its first drawing: as it stands, not faded in from nothing)
    first.step(t, true);
    CHECK(near(first.shown, 1));
    if (!fails) std::puts("the System list's buttons' and the dialog's curves and state");
    return fails ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    ui = (VK / "vk_ui.h").read_text(encoding="utf-8")
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    sound = (ROOT / "host" / "menu_sound.c").read_text(encoding="utf-8")
    events = (VK / "menu_sound_events.h").read_text(encoding="utf-8")

    # the cursor and the button with an amount
    cursor = function(ui, "inline void cursor(Ctx& c, ImVec2 a, ImVec2 b, float k = 1) {")
    check("if (k <= 0) return;" in cursor and "(0.50f + 0.35f * c.pulse) * k" in cursor and "alpha(kCursor, k)" in cursor,
          "the cursor's line and glow are not drawn with its amount")
    button = function(ui, "inline void button(Ctx& c, ImVec2 a0, ImVec2 b0, const char* label, float focus) {")
    check("if (focus > 0) cursor(c, a0 + ImVec2(lu(c, 8), lu(c, 8)), b0 - ImVec2(lu(c, 8), lu(c, 8)), focus);" in button,
          "the button does not draw its cursor with its amount")
    check("inline void button(Ctx& c, ImVec2 a0, ImVec2 b0, const char* label, bool sel) { button(c, a0, b0, label, sel ? 1.0f : 0.0f); }" in ui,
          "the other buttons (draw_ask's) no longer draw their cursor whole")

    # Mods and Quit: their cursor stepped every drawing, also while the menu is shut
    check('#include "focus_anim.h"' in menu, "vk_menu.cpp does not use focus_anim.h")
    row = function(menu, "void draw_mods_row(ui::Ctx& c, bool sel) {")
    pause = row[:row.index("draw_title_mods(")]
    check("mods_focus[i].step(now, sel && mods_row_sel == i, !ease_live || still);" in pause and
          "ui::button(c, a, b, names[i], mods_focus[i].shown);" in pause,
          "Mods and Quit do not fade their cursor in and out (it pops)")
    check("draw_mods_row(k, false);" in function(menu, "void draw_mods_row_shut(ui::Ctx& c) {"),
          "the entries are not drawn while the menu is shut (no fade after the cursor went back to the game's list)")

    # the dialog: In and Out, its buttons' cursor
    ask = function(menu, "void draw_quit_ask(ui::Ctx& c) {")
    check("quit_ask ? focus_anim::window_at(now - quit_open_ns) : focus_anim::window_at(now - quit_shut_ns, true)" in ask,
          "the dialog does not play the window's In and Out")
    check(ask.index("const int vtx_in = c.d->VtxBuffer.Size;") < ask.index("\"Quit the game?\""),
          "the dialog's question and buttons are not the part that rises (N_In_00)")
    # ((11-19) the lab's "the Quit banner text shows on top of the game's tab strip before the banner plate": SystemWindow_00 has
    # no banner (N_In_00 alone, its In that pane's alpha and height); the dim over the whole window, on the same alpha)
    check("ui::banner(" not in ask, "Quit's dialog draws a banner of its own over the pause menu's header (SystemWindow_00 has none)")
    check("const int vtx0 = c.d->VtxBuffer.Size;" in ask and
          ask.index("const int vtx0 = c.d->VtxBuffer.Size;") < ask.index("dim_window(c.d, menu_map, menu_grid(menu_window_w, menu_window_h), menu_window_w, menu_window_h, ui::rgba(8, 5, 2, 220));") <
          ask.index("const int vtx_in = c.d->VtxBuffer.Size;"), "the dim is not the window's, faded with the dialog")
    check("c.d->VtxBuffer[i].pos.y += drop;" in ask and "c.d->VtxBuffer[i].col = ui::alpha(c.d->VtxBuffer[i].col, w.alpha);" in ask,
          "the dialog is not risen and faded as drawn")
    check("quit_focus[i].step(now, i == quit_sel, settle || quit_fresh);" in ask and "ui::button(c, a, b, labels[i], quit_focus[i].shown);" in ask,
          "the dialog's buttons do not fade their cursor as the System list's")
    dm = function(menu, "void draw_menu(ui::Ctx& c, bool perf_under) {")
    check("if (row) {   // (the entries: drawn above; Quit's dialog over them, and its Out; its dim the window's)" in dm and
          "        if (quit_ask || quit_closing())" in dm and
          "draw_quit_ask(k);" in dm and dm.index("menu_window_w = c.w, menu_window_h = c.h;") < dm.index("draw_quit_ask(k);"),
          "the dialog is not drawn through its Out, or its dim is not the whole window's")
    # ((11-19) the performance panel over the dialog's dim, as over the game's own dialogs: drawn after the menu on Mods' page)
    for fn in ("void menu_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) {",
               "void menu_record_image(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h) {"):
        rec = function(menu, fn)
        check("const bool over_row = open && page == kModsRowPage;" in rec and "if (open) draw_menu(c, full && !over_row);" in rec and
              rec.index("if (open) draw_menu(c, full && !over_row);") < rec.index("if (full && over_row) draw_perf(c);"),
              "the performance panel is under Quit's dim (" + fn.split("(")[0] + ")")
    # ((11-19) the mouse over the dialog: its buttons only, Mods and Quit under the dim gone from the hit map; none on its Out)
    check("hits.clear(0, 0, c.w, c.h);" in ask and ask.index("hits.clear(0, 0, c.w, c.h);") < ask.index("for (int i = 0; i < 3; i++) {"),
          "Mods and Quit under the dialog's dim still take clicks (a second click on Quit's place is A on Quit Game)")
    check("if (quit_ask) hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, 10 + i);" in ask, "the dialog's buttons take clicks while its Out plays")

    # the input
    tick = function(menu, "void menu_tick() {")
    enter = tick[tick.index("bool entered_by_down = false;"):tick.index("if (!open_now.load() || page != kModsPage) {")]
    check("entered_by_down = enter == 1 || enter == 5;" in enter and "if (enter == 3) quit_open();" in enter,
          "the enter word does not tell of a Down, or a click on Quit does not open its dialog with its In")
    check("mods_row_sel = enter == 3 || enter == 4 ? 1 : 0," in enter and "const bool clicked = enter == 2 || enter == 3;" in enter and
          "mods_sound(clicked ? HOS_MENU_SOUND_SYS_DECIDE : HOS_MENU_SOUND_SYS_CURSOR);" in enter,
          "the mouse moved onto Quit (the host's 4) does not give it the cursor with the system menu's tick")
    clear = tick.index("if (entered_by_down) press[2] = false;")
    feeds = [tick.index("press[1] |= pad(HOS_PAD_UP) || pad(HOS_PAD_LS_UP), press[2] |= pad(HOS_PAD_DOWN) || pad(HOS_PAD_LS_DOWN);"),
             tick.index("press[1] |= (mp & HOS_MODS_PRESS_UP) != 0, press[2] |= (mp & HOS_MODS_PRESS_DOWN) != 0;")]
    on_to_quit = tick.index("} else if (press[2] && quit_here && mods_row_sel == 0) {   // (Down from Mods: Quit)")
    check(max(feeds) < clear < on_to_quit, "the Down that brought the cursor onto Mods is taken again as the entry's own (on to Quit)")
    check(not re.search(r"press\[2\] \|= |press\[2\] = true", tick[clear + 40:on_to_quit]), "a Down is put back after the enter's was taken away")
    rows = tick[tick.index("} else if (open_now.load() && page == kModsRowPage) {"):tick.index("} else if (open_now.load() && page == kModsPage) {")]
    check("const bool quit_ready = quit_ask && focus_anim::window_ready(prof_now() - quit_open_ns);" in rows and
          "if (quit_ready && hit.index >= 10" in rows, "the dialog takes the mouse while its In plays")
    check("if ((ptr.pressed & HOS_MOUSE_LEFT) && (!quit_ask || hit.index >= 10)) press[7] = true;" in rows,
          "with the dialog open a click on Mods or Quit is A on the dialog")
    check("} else if (moved && !quit_ask && !quit_closing()) {" in rows and
          "my < e[1] && my > e[1] - (quit_here ? 6 : 4) * e[4]) mods_row_sel = 0, press[1] = true;" in rows,
          "the mouse moved up off Mods or Quit onto the System page's buttons does not give the game its cursor back")
    check("if (i == 0) mods_entry_px[0] = a.x, mods_entry_px[1] = a.y, mods_entry_px[2] = b.x, mods_entry_px[3] = b.y;" in pause and
          "mods_entry_px[4] = ui::lu(c, pitch);" in pause, "the System page's Mods box is not kept for the mouse")
    wait = rows[rows.index("} else if (quit_ask && !quit_ready) {"):rows.index("} else if (quit_ask) {")]
    check("press[7] = false" in wait and "press[5]" in wait and "press[2]" in wait, "the dialog takes presses while its In plays")
    check("if (mods_row_sel == 1) quit_open(), mods_sound(HOS_MENU_SOUND_SYS_DECIDE), changed = true;" in rows,
          "A on Quit does not open its dialog with its In and the system menu's decide")
    check("if (quit_sel == 2) quit_cancel();" in rows and "quit_cancel(), mods_sound(HOS_MENU_SOUND_CANCEL)" in rows,
          "Cancel and B do not close the dialog with its Out")
    check("quit_ask = true" not in tick and "quit_ask = false" not in tick, "the dialog is opened or closed past its In and Out")
    check("else if (!after.open && !quit_ended) mods_sound(HOS_MENU_SOUND_SYS_CURSOR);" in tick and "quit_ended = true" in rows,
          "Quit Game's decide is followed by the cursor's sound")
    # (the sounds reach the host: the queue takes the system menu's)
    check("constexpr int kFanfare = 8, kSysCursor = 9, kSysDecide = 10, kLast = kSysDecide;" in events and "sound > kLast" in events,
          "the sounds' queue drops the system menu's cursor and decide")

    # the host: the stick's Down onto Mods where the menu's bits have it; the system tick replaced by its decide
    check("const int down = (int)(buttons >> B_DOWN & 1) || stick[1] < -16384;" in hid and "if (s[1] < -16384) menu |= 1ULL << HOS_PAD_LS_DOWN;" in hid,
          "the stick's Down onto Mods is not where the menu's pad bits have it (a later read is a Down of the entry's)")
    play = function(sound, "void hos_menu_sound_play(int id, int64_t now_ns) {")
    check("was == HOS_MENU_SOUND_SYS_CURSOR" in play, "the system menu's tick is not replaced by the decide of the same press")
    check("if (id == HOS_MENU_SOUND_CURSOR && now_ns - atomic_load(&last_start_ns) < CURSOR_GAP_NS) return;" in play,
          "the options' tick gap changed")

    with tempfile.TemporaryDirectory(prefix="pause-mods-anim-") as tmp:
        src, exe = Path(tmp) / "anim.cpp", Path(tmp) / "anim.exe"
        src.write_text(PROGRAM, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(VK), str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        check(r.returncode == 0, "the buttons' and the dialog's animations as made:\n" + r.stdout)
    print("PASS: Mods and Quit under the System list, Quit's dialog: the game's animations, the controller's Down, the sounds")


if __name__ == "__main__":
    main()
