"""The System tab's save rows and their way to the host (gpu/vk/vk_menu.cpp, gpu/vk/vk_api.h, gpu/vk/vk_replay.cpp; no game).

Export save, Export save for a bug report, Import a save and Bug capture act on a press: the row leaves what was asked for
the host's worker to take (rx_option HOS_OPT_SHARE_ASK, host/save_share.c) and shows under its help the line the host sends
back, a byte a call. Compiled and run from vk_menu.cpp's own text (share_row to share_press, the option_set branches,
menu_take_share_ask) with a clock of the test's:

- which rows they are, and that no other row is one;
- a press is asked of the host once (the next read answers nothing), shows "Working...", and a second press is not asked
  while the host has not answered; its answer is the row's line, UTF-8 as sent, and frees the next press;
- no answer in ten seconds: the row says so and can be pressed again;
- the Import row cancels while an import waits (its label, and what a press asks);
- the Bug capture row asks for a capture, the host's line for the foot of the screen is kept for the menu's tick, and the
  host's question for a capture's picture reaches the renderer (tests/test_bug_capture.py has the capture itself).

Read in the sources: the rows stand directly after Discord status on the System tab, a press on them is taken before
the rows that open a page, their label is drawn in the value's place and in the guides, the ids are the feature's own
(70 to 79), the host's read is the take, and the readme has the rows.

    python -I -B tests/test_save_rows.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
ROWS = ("Export save", "Export save for a bug report", "Import a save")
ALL_ROWS = ROWS + ("Bug capture (F8)",)   # (as they stand on the tab)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


FIXTURE = r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int64_t fake_now = 1000000000LL;
static int64_t prof_now() { return fake_now; }
static int shot_wanted = -1;
static void bugshot_request(bool want) { shot_wanted = want; }   // (vk_bugshot.cpp's)
#include "cut.inc"
static void say(int row, const char* text) {
    for (const char* p = text; *p; p++) option_set(HOS_OPT_SHARE_TEXT, (unsigned char)*p);
    option_set(HOS_OPT_SHARE_SAID, row);
}
int main() {
    int v = 0;
    const Row exp{TAB_SYSTEM, "Export save"}, bug{TAB_SYSTEM, "Export save for a bug report"}, imp{TAB_SYSTEM, "Import a save"};
    const Row cap{TAB_SYSTEM, "Bug capture (F8)"};
    CHECK(share_row(exp) == HOS_SHARE_EXPORT && share_row(bug) == HOS_SHARE_EXPORT_BUG && share_row(imp) == HOS_SHARE_IMPORT);
    CHECK(share_row(cap) == HOS_SHARE_CAPTURE && !strcmp(share_label(HOS_SHARE_CAPTURE), "Capture"));
    const Row restart{TAB_SYSTEM, "Restart"}, page{TAB_CONTROLS, "Controls"}, elsewhere{TAB_CONTROLS, "Export save"}, valued{TAB_SYSTEM, "Export save", &v};
    CHECK(!share_row(restart) && !share_row(page) && !share_row(elsewhere) && !share_row(valued));
    CHECK(!strcmp(share_label(HOS_SHARE_EXPORT), "Export") && !strcmp(share_label(HOS_SHARE_EXPORT_BUG), "Export") && !strcmp(share_label(HOS_SHARE_IMPORT), "Import"));
    CHECK(share_line(HOS_SHARE_EXPORT).empty() && menu_take_share_ask() == 0);

    share_press(HOS_SHARE_EXPORT);   // (a press: asked once, and shown as under way)
    CHECK(share_line(HOS_SHARE_EXPORT) == "Working...");
    CHECK(menu_take_share_ask() == HOS_SHARE_EXPORT && menu_take_share_ask() == 0);
    fake_now += 3000000000LL;
    share_press(HOS_SHARE_EXPORT_BUG);   // (the host still works: not asked, the other row's line untouched)
    share_press(HOS_SHARE_EXPORT);
    CHECK(menu_take_share_ask() == 0 && share_line(HOS_SHARE_EXPORT_BUG).empty() && share_line(HOS_SHARE_EXPORT) == "Working...");
    say(HOS_SHARE_EXPORT, "Saved as C:\\Users\\Zo\xc3\xab\\Downloads\\Triforce-save-20261115-142234.zip");
    CHECK(share_line(HOS_SHARE_EXPORT) == "Saved as C:\\Users\\Zo\xc3\xab\\Downloads\\Triforce-save-20261115-142234.zip");
    share_press(HOS_SHARE_IMPORT);
    say(HOS_SHARE_EXPORT_BUG, "another row's line");   // (not the answer to the press: the row is still waited for)
    share_press(HOS_SHARE_EXPORT_BUG);
    CHECK(menu_take_share_ask() == HOS_SHARE_IMPORT && menu_take_share_ask() == 0 && share_line(HOS_SHARE_IMPORT) == "Working...");
    CHECK(share_line(HOS_SHARE_EXPORT_BUG) == "another row's line");
    say(HOS_SHARE_IMPORT, "nothing to import");
    share_press(HOS_SHARE_EXPORT_BUG);   // (answered: the next press is asked)
    CHECK(menu_take_share_ask() == HOS_SHARE_EXPORT_BUG && share_line(HOS_SHARE_EXPORT_BUG) == "Working...");

    fake_now += 10500000000LL;   // (no answer in ten seconds: said, and the row can be pressed again)
    CHECK(share_line(HOS_SHARE_EXPORT_BUG) == "The game did not answer. Try again in a moment.");
    CHECK(share_line(HOS_SHARE_EXPORT) == "Saved as C:\\Users\\Zo\xc3\xab\\Downloads\\Triforce-save-20261115-142234.zip");
    share_press(HOS_SHARE_EXPORT_BUG);
    CHECK(menu_take_share_ask() == HOS_SHARE_EXPORT_BUG);
    say(HOS_SHARE_EXPORT_BUG, "Saved as x");
    say(HOS_SHARE_EXPORT_BUG, "Saved as y");   // (each line whole: the bytes of the one before are not in it)
    CHECK(share_line(HOS_SHARE_EXPORT_BUG) == "Saved as y");

    share_press(HOS_SHARE_IMPORT);   // (the Import row: imports, or cancels while one waits)
    CHECK(menu_take_share_ask() == HOS_SHARE_IMPORT);
    option_set(HOS_OPT_SHARE_IMPORT, 1);
    say(HOS_SHARE_IMPORT, "it waits");
    CHECK(!strcmp(share_label(HOS_SHARE_IMPORT), "Cancel the import") && !strcmp(share_label(HOS_SHARE_EXPORT), "Export"));
    share_press(HOS_SHARE_IMPORT);
    CHECK(menu_take_share_ask() == HOS_SHARE_IMPORT_CANCEL);
    option_set(HOS_OPT_SHARE_IMPORT, 0);
    say(HOS_SHARE_IMPORT, "cancelled");
    CHECK(!strcmp(share_label(HOS_SHARE_IMPORT), "Import") && share_line(HOS_SHARE_IMPORT) == "cancelled");
    share_press(HOS_SHARE_IMPORT);
    CHECK(menu_take_share_ask() == HOS_SHARE_IMPORT);

    say(HOS_SHARE_IMPORT, "imported");
    share_press(HOS_SHARE_CAPTURE);   // (the Bug capture row: asked as the others, answered with its folder)
    CHECK(menu_take_share_ask() == HOS_SHARE_CAPTURE && share_line(HOS_SHARE_CAPTURE) == "Working...");
    CHECK(!share_notice_new.load());
    say(HOS_SHARE_NOTICE, "Bug capture saved: captures\\20261115-142234");   // (the line for the foot of the screen: kept for the tick)
    CHECK(share_notice_new.load() && share_lines[HOS_SHARE_NOTICE] == "Bug capture saved: captures\\20261115-142234");
    CHECK(share_line(HOS_SHARE_CAPTURE) == "Working...");   // (the notice is not the row's answer)
    say(HOS_SHARE_CAPTURE, "Saved in C:\\Games\\Triforce\\captures\\20261115-142234");
    CHECK(share_line(HOS_SHARE_CAPTURE) == "Saved in C:\\Games\\Triforce\\captures\\20261115-142234");
    option_set(HOS_OPT_BUG_SHOT, 1);   // (the host's question for a capture's picture, and its taking back)
    CHECK(shot_wanted == 1);
    option_set(HOS_OPT_BUG_SHOT, 0);
    CHECK(shot_wanted == 0);
    share_press(HOS_SHARE_IMPORT);

    say(0, "for no row");   // (a line for a row that is none, or past the rows: dropped, and nothing written past the lines)
    say(99, "for no row");
    say(-4, "for no row");
    option_set(HOS_OPT_SHARE_TEXT, 0), option_set(HOS_OPT_SHARE_TEXT, -1), option_set(HOS_OPT_SHARE_TEXT, 4096);   // (no byte: not text)
    std::string long_line(5000, 'x');
    say(HOS_SHARE_EXPORT, long_line.c_str());   // (a line without end: cut, not kept growing)
    CHECK(share_line(HOS_SHARE_EXPORT).size() == 600 && share_line(HOS_SHARE_IMPORT) == "Working...");
    if (!fails) printf("PASS\n");
    return fails != 0;
}
'''


def check_sources(menu, api, replay, readme):
    table = menu[menu.index("Row rows[] = {"):menu.index("constexpr int kQuitRow")]
    entries = re.findall(r'\{(TAB_\w+), "([^"]+)", ([^,]+),', table)
    # ((Triforce) 11-17) a section's heading (its entry ends "..., -1, true}") is not a row
    headings = set(re.findall(r'\{TAB_\w+, "([^"]+)", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true\}', table))
    entries = [e for e in entries if e[1] not in headings]
    names = [e[1] for e in entries]
    at = names.index("Discord status")
    check(tuple(names[at + 1:at + 1 + len(ALL_ROWS)]) == ALL_ROWS, "the save rows do not stand directly after Discord status: %r" % names[at + 1:at + 5])
    for tab, name, value in entries[at + 1:at + 1 + len(ALL_ROWS)]:
        check(tab == "TAB_SYSTEM" and value == "nullptr", "%s is not a row of the System tab that acts on a press" % name)
    check(names[-2:] == ["Restart", "Quit"], "Restart and Quit are no longer the table's last two")
    tick = block(menu, "void menu_tick() {")
    press = tick.index("else if (press[7] && share_row(r)) share_press(share_row(r)), press[7] = false, changed = true;")
    check(tick.index("} else if (r.cheat >= 0) cheat_press(") < press < tick.index("else if (press[7] && !r.value && can_bind()) page = 1"),
          "a press on a save row is not taken before the rows that open a page")
    draw = block(menu, "void draw_options(ui::Ctx& c) {")
    check('ends ? r.name : share_row(r) ? share_label(share_row(r)) : "Open"' in draw, "a save row's label is not drawn in its value's place")
    check("if (share_row(selr)) prompt_list[0] = {ui::kConfirm, share_label(share_row(selr))};" in draw, "the guides do not say what a press on a save row does")
    check("share_row(selr) ? share_line(share_row(selr)) : std::string()" in draw and "if (!share_hint.empty()) hint = share_hint.c_str();" in draw,
          "the host's line is not shown under the row's help")
    ids = dict((name, int(number)) for name, number in re.findall(r"\b(HOS_OPT_SHARE_ASK) = (\d+)", api))
    enum = block(api, "enum { HOS_OPT_SHARE_ASK")
    count = len(re.findall(r"\bHOS_OPT_\w+", enum))
    check(ids == {"HOS_OPT_SHARE_ASK": 70} and 70 + count - 1 <= 79, "the save rows' option ids are not within 70 to 79")
    option = replay[replay.index("t.option = "):]
    option = option[:option.index("};") + 2]
    check("if (id == HOS_OPT_SHARE_ASK) return menu_take_share_ask();" in option, "the host cannot take what a row asked (vk_replay.cpp's option)")
    check("menu_option_set(id, value);" in replay[replay.index("t.option_set = "):replay.index("t.nvn_register_sampler")], "the host's lines no longer reach the menu")
    for row in ROWS:
        check("| %s |" % row in readme and "| **%s** |" % row in readme, "readme.md does not have the row " + row)
    check("save-import" in readme and "Triforce-save-<date>-<time>.zip" in readme and "import-<date>-<time>" in readme, "readme.md does not say where the zips and the kept save are")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    check_sources(menu, api, replay, (ROOT / "readme.md").read_text(encoding="utf-8"))
    # (the menu's own text: the tabs and a row, the rows' state and what they do, the host's side of it)
    state = menu[menu.index("int share_row(const Row& r) {"):menu.index(block(menu, "void share_press(int row) {")) + len(block(menu, "void share_press(int row) {"))]
    branches = menu[menu.index("    if (id == HOS_OPT_SHARE_TEXT) {"):menu.index("    if (id == OOT_CHEAT_AGE_ID) {")]
    take = re.search(r"^int menu_take_share_ask\(\) \{[^\n]*\}$", menu, re.M).group(0)
    cut = "\n".join((block(api, "enum { HOS_OPT_SHARE_ASK") + ";", block(api, "enum { HOS_SHARE_EXPORT") + ";", block(menu, "enum MenuTab {") + ";",
                     block(menu, "struct Row {") + ";", state, "void option_set(int id, int value) {\n" + branches + "}", take))
    with tempfile.TemporaryDirectory(prefix="triforce-save-rows-") as tmp:
        tmp = Path(tmp)
        (tmp / "cut.inc").write_text(cut, encoding="utf-8")
        (tmp / "t.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / "t.exe"
        build = subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", "-I", str(tmp), str(tmp / "t.cpp"), "-o", str(exe)],
                               capture_output=True, text=True)
        check(build.returncode == 0, "the rows' code does not build alone:\n" + build.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        check(run.returncode == 0 and "PASS" in run.stdout, "the rows' code:\n" + run.stdout + run.stderr)
    print("PASS: the System tab's save rows: what a press asks the host, what the host's answer shows, where they stand")


if __name__ == "__main__":
    main()
