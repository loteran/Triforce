"""The Debug tab drawn as every other tab of the options (gpu/vk/vk_menu.cpp debug_list, draw_options; no game).

The lab's playtest (11-17) showed the Debug tab styled apart from the rest: its headings ("Player", "Story & amiibo") a
plain gold line instead of the game's separator with its mark, a blank row's room between its groups, its own column
geometry (names further left, value cells wider than on every other tab), and its buttons orange without an arrow where
the System tab's were the dark panel with one. Before that it had its own layout (debug_tab_layout.h, gone now) for two
columns that fitted at no window size.

Now the tab is a list as the others are: draw_options draws it with the same rows, cells, headings and scroll, in the
order debug_list gives (the Debug tools switch, each group of tools under its heading, the Developer heading and its
rows). This compiles debug_list and debug_step from the source with a made-up table of rows and checks: no blank entry,
a group with no row shown has no heading, every heading has a row under it, the tools gone with the switch off leave the
switch and the Developer rows, and Up and Down walk the rows over the headings. The rest is read in the source: one
drawing loop for every tab, the headings ui::heading's, one action-button style (action_cell: the orange plate with its
arrow) on every tab, and a still drawn settled.

    python -I -B tests/test_debug_tab_layout.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


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


FIXTURE = r'''#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string_view>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
enum MenuTab { TAB_DISPLAY = 0, TAB_DEBUG = 5 };
struct { int debug_tools, reuse, robust; } opts;
struct Row { MenuTab tab; const char* name; int* value; signed char cheat; bool heading; };
// (the tab's rows in the table's order, as vk_menu.cpp has them: the switch, the tools by their number, Developer)
Row rows[] = {{TAB_DISPLAY, "Elsewhere", &opts.reuse, -1, false}, {TAB_DEBUG, "Debug tools", &opts.debug_tools, -1, false},
              {TAB_DEBUG, "Heart protection", nullptr, 0, false}, {TAB_DEBUG, "No-clip flight", nullptr, 1, false},
              {TAB_DEBUG, "Mark position", nullptr, 2, false}, {TAB_DEBUG, "Return to mark", nullptr, 3, false},
              {TAB_DEBUG, "Set time", nullptr, 4, false}, {TAB_DEBUG, "Inventory item", nullptr, 5, false},
              {TAB_DEBUG, "Change quantity", nullptr, 6, false}, {TAB_DEBUG, "Free camera", nullptr, 7, false},
              {TAB_DEBUG, "Teleport", nullptr, 8, false}, {TAB_DEBUG, "Switch age", nullptr, 9, false},
              {TAB_DEBUG, "Inventory", nullptr, 10, false}, {TAB_DEBUG, "Game flags", nullptr, 11, false},
              {TAB_DEBUG, "Cutscenes", nullptr, 12, false}, {TAB_DEBUG, "Scan amiibo", nullptr, 13, false},
              {TAB_DEBUG, "Developer", nullptr, -1, true}, {TAB_DEBUG, "Descriptor reuse", &opts.reuse, -1, false},
              {TAB_DEBUG, "Robust buffers", &opts.robust, -1, false}};
constexpr int kRows = sizeof rows / sizeof rows[0];
static bool hidden[kRows];
static bool row_shown(int i) { return !hidden[i]; }
int tab_row_count(int t) {
    int c = 0;
    for (int i = 0; i < kRows; i++) if (rows[i].tab == (MenuTab)t && row_shown(i)) c++;
    return c;
}
int tab_row_to_global(int t, int local_idx) {
    int count = 0;
    for (int i = 0; i < kRows; i++)
        if (rows[i].tab == (MenuTab)t && row_shown(i)) {
            if (count == local_idx) return i;
            count++;
        }
    return 0;
}
@DEBUG@
static std::vector<const char*> names() {   // (the list as drawn: a heading's title or a row's name)
    int list[kDebugList];
    const int n = debug_list(list);
    std::vector<const char*> out;
    for (int j = 0; j < n; j++) out.push_back(list[j] < 0 ? debug_groups[-1 - list[j]].title : rows[tab_row_to_global(TAB_DEBUG, list[j])].name);
    return out;
}
static bool same(const std::vector<const char*>& got, std::initializer_list<const char*> want) {
    if (got.size() != want.size()) return false;
    size_t i = 0;
    for (const char* w : want) {
        if (std::string_view(got[i]) != w) return false;
        i++;
    }
    return true;
}
static void show(const std::vector<const char*>& v) { for (const char* s : v) std::fprintf(stderr, "  %s\n", s); }
static int local(const char* name) {   // (a row's number on the tab)
    const int n = tab_row_count(TAB_DEBUG);
    for (int k = 0; k < n; k++) if (std::string_view(rows[tab_row_to_global(TAB_DEBUG, k)].name) == name) return k;
    return -1;
}
int main() {
    hidden[7] = hidden[8] = true;   // (Inventory item and Change quantity: never on the tab)
    std::vector<const char*> v = names();
    if (!same(v, {"Debug tools", "Player", "Heart protection", "No-clip flight", "Switch age", "Inventory", "Story & amiibo",
                  "Game flags", "Cutscenes", "Scan amiibo", "Travel", "Teleport", "Mark position", "Return to mark",
                  "Camera & world", "Free camera", "Set time", "Developer", "Descriptor reuse", "Robust buffers"})) {
        show(v);
        CHECK(!"the tab's list with every tool");
    }
    // (the tools off: they are not shown, nor their headings)
    for (int i = 2; i <= 15; i++) hidden[i] = true;
    v = names();
    if (!same(v, {"Debug tools", "Developer", "Descriptor reuse", "Robust buffers"})) {
        show(v);
        CHECK(!"the tab's list with the tools off");
    }
    // (a group none of whose rows is shown: no heading, no room)
    for (int i = 2; i <= 15; i++) hidden[i] = i == 7 || i == 8 || i == 4 || i == 5 || i == 10;
    v = names();
    if (!same(v, {"Debug tools", "Player", "Heart protection", "No-clip flight", "Switch age", "Inventory", "Story & amiibo",
                  "Game flags", "Cutscenes", "Scan amiibo", "Camera & world", "Free camera", "Set time", "Developer",
                  "Descriptor reuse", "Robust buffers"})) {
        show(v);
        CHECK(!"the tab's list without the Travel rows");
    }
    // Up and Down: the rows in the list's order, over the headings, round at the ends
    CHECK(debug_step(local("Debug tools"), 1) == local("Heart protection"));
    CHECK(debug_step(local("Inventory"), 1) == local("Game flags"));
    CHECK(debug_step(local("Set time"), 1) == local("Descriptor reuse"));
    CHECK(debug_step(local("Robust buffers"), 1) == local("Debug tools"));
    CHECK(debug_step(local("Debug tools"), -1) == local("Robust buffers"));
    CHECK(debug_step(local("Game flags"), -1) == local("Inventory"));
    std::puts("ok");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    ui = (ROOT / "gpu/vk/vk_ui.h").read_text(encoding="utf-8")
    check("draw_debug_rows" not in menu and not (ROOT / "gpu/vk/debug_tab_layout.h").exists(),
          "the Debug tab still has a layout of its own")
    draw = function(menu, "void draw_options(ui::Ctx& c) {")
    # one list, one loop, for every tab: the Debug tab's entries are debug_list's
    check("if (cur_tab == TAB_DEBUG) listed = debug_list(list);" in draw and "return;" not in draw[:draw.index("PushClipRect")],
          "the Debug tab is not drawn by the tabs' own loop")
    check("scroll_rows(cur_tab, listed, sel_at, top, bottom, pitch, &shown)" in draw, "the Debug tab's list does not scroll as the others")
    # its headings the game's separator and title, as the others'
    check("ui::heading(c, ImVec2(rx0, y), ImVec2(rx1, y + rh), rx0 + ui::lu(c, 72), i < 0 ? debug_groups[-1 - i].title : r.name);" in draw
          and "AddLine(" not in draw, "a Debug group's heading is not the game's separator and title")
    check("separator(c, " in function(ui, "inline void heading("), "ui::heading has no separator")
    # its rows the same cells: the tools' toggles, Link's age, choices and buttons
    check("} else if (r.cheat >= 0) {" in draw and 'halves_cell(c, va, vb, "On", "Off", cheat_value(r.cheat) != "On", selected, false);' in draw
          and 'halves_cell(c, va, vb, "Kid", "Adult", link_age.load(std::memory_order_relaxed) == 1, selected, false);' in draw,
          "the debug tools' toggles are not the tabs' halves")
    # one action-button style: the orange plate with its arrow, on the System tab's rows as on the Debug tab's
    action = function(menu, "void action_cell(")
    check("choice_cell(c, a, b, word, sel, false, !fixed, fixed);" in action, "action_cell is not the orange plate with its arrow")
    check("else action_cell(c, va, vb, v.empty() ? buttons[r.cheat] : v, selected, r.cheat == 9);" in draw
          and 'else action_cell(c, va, vb, ends ? r.name : share_row(r) ? share_label(share_row(r)) : "Open", selected);' in draw,
          "the Debug tab's buttons and the page, save, Restart and Quit rows are not one style")
    check(", false, true, false, true)" not in draw, "a row still draws its button as the dark panel")
    # the rows clipped as the other tabs': none under the header
    check("if (y < clip0 || y > clip1) continue;" in draw, "a row is drawn with its top under the header")
    check("hits.add(rx0, std::max(y, clip0), rx1, std::min(y + rh, clip1), menu_hit::kRow, i, va.x, vb.x, t0, t1, 0, vm);" in draw,
          "the mouse's rows are not what shows of them")
    # Up and Down on the tab walk debug_list's order
    tick = function(menu, "void menu_tick() {")
    check("selected = debug_step(selected, press[2] ? 1 : -1);" in tick, "Up and Down do not walk the Debug tab's order")
    # a still is drawn settled: only menu_record, a window's frames, steps the easing
    image = function(menu, "void menu_record_image(")
    record = function(menu, "void menu_record(VkCommandBuffer cmd, uint32_t index")
    check("if (!ease_live) ease_step = 1.0f;" in image and image.index("if (!ease_live) ease_step = 1.0f;") < image.index("draw_menu(c"),
          "a still does not draw the lists where the selection asks")
    check("ease_live = true;" in record and "ease_live = true" not in image, "the window's frames do not keep their easing")

    start = menu.index("struct DebugGroup {")
    debug = menu[start:menu.index("/// (Triforce) A row that does something or opens a page")]
    fixture = FIXTURE.replace("@DEBUG@", debug)
    with tempfile.TemporaryDirectory(prefix="debug-tab-") as tmp:
        cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if run.returncode or run.stdout.strip() != "ok":
            raise SystemExit("FAIL: the Debug tab's list (exit %d)\n%s%s" % (run.returncode, run.stdout, run.stderr))
    print("PASS: the Debug tab is a list as the other tabs are: their rows, headings, buttons and scroll, no blank rows")


if __name__ == "__main__":
    main()
