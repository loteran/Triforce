"""The System tab's rows for the mods' cheats (gpu/vk/mod_cheat_rows.h, gpu/vk/vk_menu.cpp, gpu/vk/vk_api.h; no game).

The host names its cheats to the menu a byte a call, each cheat gets a row with the file's own name, On | Off, and the
host asks every frame what the player switched (tests/test_mod_cheats.py has the host's side of the same calls).
Compiled and run from vk_menu.cpp's own text (the list, the option_set branches, menu_take_mod_cheat) with the header:

- nothing is shown until the host says the list is whole, then as many rows as it named, in its order, each with its
  name and the line under its help, UTF-8 as sent; a name cut at 63 bytes, never inside a letter;
- a cheat that has the name of a row that acts by its name (Export save ...) is shown with " (cheat)" after it;
- a row is off unless the host said it was left on; Left switches it on, Right off, confirm flips it, and the host is
  told each change once, in the rows' order, and nothing when a switch is back where the host knows it;
- a master code's row and a refused cheat's cannot be changed and are never reported;
- more cheats than rows, a list sent twice, bytes that are none, rows past the list: nothing breaks, nothing is shown
  that was not named.

Read in the sources: the rows stand on the System tab after Bug capture (F8) with a row for every slot, are not rows of
the options file, are drawn and pressed before the rows that act by their name or open a page, the ids are the
feature's own (130 to 134), and the readme has them.

    python -I -B tests/test_mod_cheat_rows.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


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
#include <cstdio>
#include <string>
#include "mod_cheat_rows.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static bool menu_refresh;
#include "cut.inc"
// the host's calls, as host/mod_cheats.c makes them
static void name(const char* cheat, const char* line, int bits) {
    for (const char* p = cheat; *p; p++) option_set(HOS_OPT_MODCHEAT_TEXT, (unsigned char)*p);
    option_set(HOS_OPT_MODCHEAT_TEXT, '\n');
    for (const char* p = line; *p; p++) option_set(HOS_OPT_MODCHEAT_TEXT, (unsigned char)*p);
    option_set(HOS_OPT_MODCHEAT_ADD, bits);
}
static bool press(int slot, char key) { return mod_cheats.press(slot, key == 'L', key == 'R', key == 'A'); }
int main() {
    CHECK(mod_cheats.shown() == 0 && menu_take_mod_cheat() == 0 && !press(0, 'A') && !mod_cheats.is_on(0));
    // (the rows' names are the list's own: that is how a row is told from every other)
    CHECK(mod_cheat_slot(mod_cheats.names[0]) == 0 && mod_cheat_slot(mod_cheats.names[95]) == 95 && mod_cheat_slot(mod_cheats.names[7] + 3) == 7);
    static const char other[] = "Export save";
    CHECK(mod_cheat_slot(other) == -1 && mod_cheat_slot(kModCheatHeading) == -1 && mod_cheat_slot(mod_cheats.lines[0]) == -1);
    CHECK(strstr(kModCheatHelp, "Off at first") && strstr(kModCheatHelp, "stays on at the next start"));   // (what every such row says of itself)

    name("Infinite Hearts", "From the mod Zo\xc3\xab's cheats.", 0);
    name("Master", "Always on: the master code of the mod A.", HOS_MODCHEAT_ALWAYS);
    name("Rupees", "From the mod A.", HOS_MODCHEAT_ON);
    name("Code patch", "Not supported: it writes the game's code. From the mod A.", HOS_MODCHEAT_REFUSED);
    name("Export save", "From the mod B.", 0);
    name("", "From the mod B.", 0);
    name("0123456789012345678901234567890123456789012345678901234567890\xc3\xa9tail", "x", 0);   // (61 bytes, a letter of two, more)
    name("01234567890123456789012345678901234567890123456789012345678901\xc3\xa9tail", "x", HOS_MODCHEAT_ON | HOS_MODCHEAT_REFUSED);
    CHECK(mod_cheats.shown() == 0 && !menu_refresh);   // (not shown while the list comes)
    CHECK(menu_take_mod_cheat() == 0 && !press(0, 'A'));
    option_set(HOS_OPT_MODCHEAT_COUNT, 8);
    CHECK(mod_cheats.shown() == 8 && menu_refresh);
    CHECK(!strcmp(mod_cheats.names[0], "Infinite Hearts") && !strcmp(mod_cheats.line(0), "From the mod Zo\xc3\xab's cheats."));
    CHECK(!strcmp(mod_cheats.names[4], "Export save (cheat)") && !strcmp(mod_cheats.names[5], "(no name)") && !strcmp(mod_cheats.line(5), "From the mod B."));
    CHECK(strlen(mod_cheats.names[6]) == 63 && !strcmp(mod_cheats.names[6] + 61, "\xc3\xa9"));
    CHECK(strlen(mod_cheats.names[7]) == 62 && mod_cheats.names[7][61] == '1');
    CHECK(!mod_cheats.is_on(0) && mod_cheats.is_on(1) && mod_cheats.is_on(2) && !mod_cheats.is_on(3) && !mod_cheats.is_on(7));
    CHECK(!mod_cheats.fixed(0) && mod_cheats.fixed(1) && !mod_cheats.fixed(2) && mod_cheats.fixed(3) && mod_cheats.fixed(7));
    CHECK(!strcmp(mod_cheats.fixed_text(1), "Always on") && !strcmp(mod_cheats.fixed_text(3), "Not supported"));
    CHECK(menu_take_mod_cheat() == 0);   // (what the host said is what it knows: nothing to tell it)

    // Left is On, Right is Off, confirm flips; the host is told each change once
    CHECK(press(0, 'L') && mod_cheats.is_on(0) && !press(0, 'L'));
    CHECK(menu_take_mod_cheat() == (0 + 1) * 2 + 1 && menu_take_mod_cheat() == 0);
    CHECK(press(0, 'R') && !mod_cheats.is_on(0) && !press(0, 'R') && menu_take_mod_cheat() == (0 + 1) * 2 + 0 && menu_take_mod_cheat() == 0);
    CHECK(press(0, 'A') && mod_cheats.is_on(0) && press(0, 'A') && !mod_cheats.is_on(0));
    CHECK(menu_take_mod_cheat() == 0);   // (on and off again before the host asked: as it knows it)
    CHECK(press(2, 'A') && !mod_cheats.is_on(2) && press(6, 'L') && press(4, 'A'));   // (several rows: in the rows' order, one a read)
    CHECK(menu_take_mod_cheat() == (2 + 1) * 2 + 0 && menu_take_mod_cheat() == (4 + 1) * 2 + 1 && menu_take_mod_cheat() == (6 + 1) * 2 + 1 && menu_take_mod_cheat() == 0);
    CHECK(!press(0, 'x'));   // (no key of the three)
    // the rows that cannot be changed, and rows that are none
    CHECK(!press(1, 'A') && !press(1, 'R') && mod_cheats.is_on(1) && !press(3, 'A') && !press(3, 'L') && !mod_cheats.is_on(3) && !press(7, 'A'));
    CHECK(!press(8, 'A') && !press(95, 'A') && !press(96, 'A') && !press(-1, 'A') && !mod_cheats.is_on(96) && !mod_cheats.is_on(-1));
    CHECK(mod_cheats.fixed(96) && mod_cheats.fixed(-1) && !mod_cheats.line(96)[0] && menu_take_mod_cheat() == 0);

    // a list sent again, bytes that are none: the rows stay as they are
    option_set(HOS_OPT_MODCHEAT_TEXT, 0), option_set(HOS_OPT_MODCHEAT_TEXT, -1), option_set(HOS_OPT_MODCHEAT_TEXT, 4096);
    name("Another", "x", HOS_MODCHEAT_ON);
    option_set(HOS_OPT_MODCHEAT_COUNT, 9);
    CHECK(mod_cheats.shown() == 8 && !mod_cheats.names[8][0] && !strcmp(mod_cheats.names[0], "Infinite Hearts") && menu_take_mod_cheat() == 0);
    if (!fails) printf("PASS\n");
    return fails != 0;
}
'''

# (a list of its own for the limits: more cheats than rows, a count that is not the list's, text without end)
LIMITS = r'''
#include <cstdio>
#include <string>
#include "mod_cheat_rows.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    using namespace mod_cheat_rows;
    static List many;
    for (int i = 0; i < kSlots + 30; i++) {
        const std::string n = "cheat " + std::to_string(i);
        for (char ch : n) many.text((unsigned char)ch);
        many.text('\n');
        for (int k = 0; k < 5000; k++) many.text('y');   // (a line without end: cut, and not the next row's name)
        many.add(0);
    }
    many.done(kSlots + 30);
    CHECK(many.shown() == kSlots && !strcmp(many.names[kSlots - 1], ("cheat " + std::to_string(kSlots - 1)).c_str()) && !strcmp(many.names[0], "cheat 0"));
    CHECK(strlen(many.line(0)) == kLine - 1 && strlen(many.line(kSlots - 1)) == kLine - 1 && many.press(kSlots - 1, false, false, true));
    CHECK(many.take() == kSlots * 2 + 1 && many.take() == 0);
    static List fewer;   // (the host counts fewer than it named, or none, or nonsense)
    fewer.add(0), fewer.add(0), fewer.add(0);
    fewer.done(2);
    CHECK(fewer.shown() == 2 && !fewer.press(2, false, false, true) && fewer.press(1, false, false, true) && fewer.take() == 5);
    static List none;
    none.add(0);
    none.done(-3);
    CHECK(none.shown() == 0 && !none.press(0, false, false, true) && none.take() == 0);
    static List more;
    more.add(0);
    more.done(50);
    CHECK(more.shown() == 1);
    static List plain;   // (a name with no line after it; a line feed in the line)
    for (char ch : std::string("Only a name")) plain.text((unsigned char)ch);
    plain.add(kOn);
    for (char ch : std::string("Two\nlines\nhere")) plain.text((unsigned char)ch);
    plain.add(0);
    plain.done(2);
    CHECK(!strcmp(plain.names[0], "Only a name") && !plain.line(0)[0] && plain.is_on(0) && !strcmp(plain.names[1], "Two") && !strcmp(plain.line(1), "lines here"));
    if (!fails) printf("PASS\n");
    return fails != 0;
}
'''


def check_sources(menu, header, api, replay, readme):
    table = menu[menu.index("Row rows[] = {"):menu.index("constexpr int kQuitRow")]
    # a row for every slot, after Bug capture (F8) and its heading, before the Controls tab's rows
    slots = int(re.search(r"constexpr int kSlots = (\d+);", (ROOT / "gpu/vk/mod_cheat_rows.h").read_text(encoding="utf-8")).group(1))
    starts = [int(n) for n in re.findall(r"MOD_CHEAT_ROWS8\((\d+)\)", table)]
    check(starts == list(range(0, slots, 8)) and slots % 8 == 0, "the table has no row for every slot: %r" % starts)
    check(menu.count("MOD_CHEAT_ROW(i + ") == 7 and "MOD_CHEAT_ROW(i) MOD_CHEAT_ROW(i + 1)" in menu, "MOD_CHEAT_ROWS8 is not eight rows")
    check("#define MOD_CHEAT_ROW(i) {TAB_SYSTEM, mod_cheats.names[i], nullptr, 0, 0, 0, nullptr, kModCheatHelp, nullptr, false, true}," in menu,
          "a cheat's row is not a row of the System tab without a value of the options file")
    at = table.index('{TAB_SYSTEM, "Bug capture (F8)", nullptr,')
    heading = table.index('{TAB_SYSTEM, "Mod cheats", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},')
    check(at < heading < table.index("MOD_CHEAT_ROWS8(0)") < table.index("// Controls ("), "the cheats' rows do not stand after Bug capture (F8) on the System tab")
    check('constexpr char kModCheatHeading[] = "Mod cheats";' in menu, "the heading's name in the table is not the one row_shown tells it by")
    check(re.search(r"static_assert\(mod_cheat_rows::kSlots == %d," % slots, menu), "nothing holds the table to the list's slots")
    check('bool ini_row(const Row& r) { return r.value && r.value != &opts.language; }' in menu, "a row without a value may be a row of the options file now")
    # shown only once named
    shown = block(menu, "bool row_shown(int i) {")
    check("(!rows[i].heading || strcmp(rows[i].name, kModCheatHeading) || mod_cheats.shown() > 0)" in shown
          and "(mod_cheat_slot(rows[i].name) < 0 || mod_cheat_slot(rows[i].name) < mod_cheats.shown())" in shown,
          "the cheats' rows and their heading are shown before the host named them")
    # drawn and pressed as cheats before anything that goes by a row's name
    draw = block(menu, "void draw_options(ui::Ctx& c) {")
    cell = draw.index("if (const int cheat = mod_cheat_slot(r.name); cheat >= 0) {")
    check(cell < draw.index("} else if (!r.value) {") and 'halves_cell(c, va, vb, "On", "Off", !mod_cheats.is_on(cheat), selected, false);' in draw
          and "choice_cell(c, va, vb, mod_cheats.fixed_text(cheat), selected, false, false, true);" in draw,
          "a cheat's row is not drawn On | Off, or as fixed when it cannot be changed")
    check(draw.index("if (!share_hint.empty()) hint = share_hint.c_str();") < draw.index("if (sel_cheat >= 0 && mod_cheats.line(sel_cheat)[0]) hint = mod_cheats.line(sel_cheat);"),
          "the row's line (its mod, or why not) is not shown under its help")
    check(draw.index("if (share_row(selr)) prompt_list[0]") < draw.index('if (sel_cheat >= 0) prompt_list[0] = {ui::kLeftRight, "Change"};'),
          "the guides do not say Change on a cheat's row")
    tick = block(menu, "void menu_tick() {")
    press = tick.index("else if (mod_cheat_slot(r.name) >= 0) changed |= mod_cheats.press(mod_cheat_slot(r.name), press[3], press[4], press[7]);")
    check(tick.index("} else if (r.cheat >= 0) cheat_press(") < press < tick.index("else if (press[7] && share_row(r))")
          < tick.index("else if (press[7] && !r.value && can_bind()) page = 1"), "a press on a cheat's row is not taken before the rows that act by name or open a page")
    # the host's side of it
    ids = block(api, "enum { HOS_OPT_MODCHEAT_TEXT")
    names = re.findall(r"\bHOS_OPT_\w+", ids)
    check(names == ["HOS_OPT_MODCHEAT_TEXT", "HOS_OPT_MODCHEAT_ADD", "HOS_OPT_MODCHEAT_COUNT", "HOS_OPT_MODCHEAT_ASK"] and "HOS_OPT_MODCHEAT_TEXT = 130" in ids,
          "the option ids are not 130 on")
    bits = block(api, "enum { HOS_MODCHEAT_ON")
    check("HOS_MODCHEAT_ON = 1," in bits and "HOS_MODCHEAT_ALWAYS = 2," in bits and "HOS_MODCHEAT_REFUSED = 4" in bits
          and "constexpr int kOn = 1, kAlways = 2, kRefused = 4;" in header, "the rows' bits differ between vk_api.h and mod_cheat_rows.h")
    option = replay[replay.index("t.option = "):]
    option = option[:option.index("};") + 2]
    check("if (id == HOS_OPT_MODCHEAT_ASK) return menu_take_mod_cheat();" in option, "the host cannot ask what was switched (vk_replay.cpp's option)")
    check("int menu_take_mod_cheat();" in (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8"), "vk_menu.h does not declare menu_take_mod_cheat")
    check("| Mod cheats |" in readme and "triforce_cheats.txt" in readme, "readme.md does not have the cheats' rows")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    header = (ROOT / "gpu/vk/mod_cheat_rows.h").read_text(encoding="utf-8")
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    check_sources(menu, header, api, replay, (ROOT / "readme.md").read_text(encoding="utf-8"))
    # (the menu's own text: the list and how a row is told, the option_set branches, the host's read)
    state = menu[menu.index("mod_cheat_rows::List mod_cheats;"):menu.index("#define MOD_CHEAT_ROW(i)")]
    branches = menu[menu.index("    if (id == HOS_OPT_MODCHEAT_TEXT) return mod_cheats.text(value);"):menu.index("    if (id == HOS_OPT_SHARE_TEXT) {")]
    take = re.search(r"^int menu_take_mod_cheat\(\) \{[^\n]*\}", menu, re.M).group(0)
    cut = "\n".join((block(api, "enum { HOS_OPT_MODCHEAT_TEXT") + ";", block(api, "enum { HOS_MODCHEAT_ON") + ";", state,
                     "void option_set(int id, int value) {\n" + branches + "}", take))
    with tempfile.TemporaryDirectory(prefix="triforce-mod-cheat-rows-") as tmp:
        tmp = Path(tmp)
        (tmp / "cut.inc").write_text(cut, encoding="utf-8")
        for name, source in (("rows", FIXTURE), ("limits", LIMITS)):
            (tmp / (name + ".cpp")).write_text(source, encoding="utf-8")
            exe = tmp / (name + ".exe")
            build = subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(tmp), "-I", str(ROOT / "gpu" / "vk"), str(tmp / (name + ".cpp")),
                                    "-o", str(exe)], capture_output=True, text=True)
            check(build.returncode == 0, "the rows' code does not build alone (%s):\n%s" % (name, build.stderr))
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            check(run.returncode == 0 and "PASS" in run.stdout, "the rows' code (%s):\n%s%s" % (name, run.stdout, run.stderr))
    print("PASS: the System tab's rows for the mods' cheats: what the host names, what a press does, what the host is told, where they stand")


if __name__ == "__main__":
    main()
