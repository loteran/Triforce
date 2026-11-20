"""The Debug tab's name pages in the menu: Game flags, Cutscenes and Scan amiibo (gpu/vk/vk_menu.cpp; source read, no game).

What the pages keep and how the names are found is run by tests/test_debug_names.py, the lines by
tests/test_debug_link.py, the figure by tests/test_amiibo.py. This reads what is left, which only a run in the game
shows otherwise: the three rows are tools of the hidden Debug tab and of no other; confirm on one opens page 6, which is
drawn, takes the keys and the mouse, and goes back to the tab; the tab's layout has a place for every tool once; the
host's reads of the pages' lines and its answers reach the menu (the option ids); nothing is read from the game's files,
listed or sent before a page is opened, and no file is written; the wheel moves no choice; the readme has the rows.

    python -I -B tests/test_debug_pages.py
"""
import re
from pathlib import Path

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


def main():
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    header = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    imgui = (ROOT / "gpu/vk/vk_imgui.cpp").read_text(encoding="utf-8")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")

    # the rows: tools of the Debug tab (shown with Debug tools on), with no value of the options file
    rows = menu[menu.index("Row rows[] = {") if "Row rows[] = {" in menu else menu.index("{TAB_DISPLAY,"):menu.index("constexpr int kQuitRow")]
    for name, tool in (("Game flags", 11), ("Cutscenes", 12), ("Scan amiibo", 13)):
        found = re.findall(r'\{(TAB_\w+), "%s", nullptr, 0, 0, 0, nullptr,\s*(?:"[^\n]*"\s*)+, nullptr, false, true, (\d+)\},' % name, rows)
        check(found == [("TAB_DEBUG", str(tool))], "the row %s is not one tool (%d) of the Debug tab: %s" % (name, tool, found))
    check("constexpr int kCheatRows = 14;" in menu, "kCheatRows does not count the three rows")
    for table in re.findall(r"says\[kCheatRows\] = \{([^}]*)\}|buttons\[kCheatRows\] = \{([^}]*)\}", menu):
        entries = re.findall(r'"[^"]*"|nullptr', table[0] or table[1])
        check(len(entries) == 14, "a table of the tools' words has %d entries, not 14" % len(entries))
    # the tab's layout: every tool but the two the Inventory page replaced, once, each in one group
    order = [int(n) for n in re.search(r"constexpr int debug_tools_order\[\] = \{([^}]*)\};", menu).group(1).split(",")]
    check(sorted(order) == [0, 1, 2, 3, 4, 7, 8, 9, 10, 11, 12, 13], "the Debug tab's order does not have every tool once: %s" % order)
    groups = re.findall(r'\{"([^"]+)", (\d+), (\d+)\}', re.search(r"constexpr DebugGroup debug_groups\[\] = \{(.*?)\};", menu).group(1))
    at = 0
    for title, start, end in groups:
        check(int(start) == at and int(end) > at, "the group %s does not follow the one before" % title)
        at = int(end)
    check(at == len(order), "the groups do not cover the tools")
    story = [g for g in groups if g[0] == "Story & amiibo"]
    check(story and order[int(story[0][1]):int(story[0][2])] == [11, 12, 13], "the three rows are not one group of the tab")
    second = int(re.search(r"constexpr int kDebugSecondColumn = (\d+);", menu).group(1))
    check(0 < second < len(groups) and "g >= kDebugSecondColumn ? 1 : 0" in menu and "debug_groups[kDebugSecondColumn].from ? 1 : 0" in menu,
          "the two columns of the tab are not told apart by the group")

    # confirm opens the page; it is drawn, takes the keys and the mouse, and goes back to the tab
    cheats = menu[menu.index("void cheat_press(int i,"):menu.index("void menu_tick() {")]
    check("if (press[7] && i >= 11 && i <= 13) names_open(i - 11), changed = true;" in cheats, "confirm on a row does not open its page")
    check("names_kind = kind, names_edit = false, page = 6;" in function(menu, "void names_open("), "names_open does not show page 6")
    check("else if (page == 6) draw_names(k);" in function(menu, "void draw_menu(ui::Ctx& c, bool perf_under) {"), "page 6 is not drawn")
    tick = menu[menu.index("void menu_tick() {"):]
    branch = tick[tick.index("} else if (open_now.load() && page == 6) {"):]
    check("if (mouse) names_mouse(ptr, mx, my, press, changed);" in branch[:400] and "names_press(press, tab_left, tab_right, changed);" in branch[:400],
          "page 6 does not take the keys and the mouse")
    press = function(menu, "void names_press(")
    check(press.index("if (press[5] && names_edit) names_edit = false") < press.index("else if (press[5]) page = 0, changed = true;") < press.index("p.empty()"),
          "back does not leave the page (an empty one too), or not a flag's On and Off first")
    check("names_tick();" in tick and tick.index("names_tick();") > tick.index("} else if (open_now.load() && page == 6) {"), "names_tick does not run each tick")
    # a flag: Left sets and Right clears, as its halves are drawn (On at the left)
    draw = function(menu, "void draw_names(")
    check("names_send_flag(press[3])" in press and 'halves_cell(c, av, bv, "On", "Off", state == \'0\', true, false);' in draw,
          "Left and Right are not On and Off as the halves are drawn")
    # (the audit's findings, what a picture showed or the code said)
    # a held Left or Right repeats the key: the same request is not queued behind itself
    send = function(menu, "void names_send_flag(")
    check("if (!names_ask.repeats(line)) names_ask.push(line);" in send, "a held key queues the same request again and again")
    # what the game's side said shows under the name it was said of, not under whichever is chosen
    check("names_status_of = p.list.names[p.choice], names_status_kind = kNamesFlags;" in send
          and "names_status_of = name, names_status_kind = kNamesEvents;" in function(menu, "void names_confirm("),
          "the last answer is not kept with the name it is about, and whether that is a flag's or an event's")
    check("names_kind == names_status_kind && names_status_of == p.list.names[p.choice]) status = names_status;" in draw
          and "amiibo_chosen == p.list.names[p.choice])" in draw, "the last answer is shown under any name, or on the other page")
    # a flag the save has not: its On and Off dim, as a row that cannot change (not drawn as one whose state is not known)
    check("const ImU32 ink = state == '-' ? ui::kDim : ui::kInk;" in draw and draw.count(", ink, \"O") == 2,
          "a flag the save has not is drawn as one that can be set")
    # a figure's card is lit while it has a tag (waiting, on the reader, read), not for a file the game refused
    check("ui::plate(c, a, b, flags ? st == '1' : tag != nullptr);" in draw, "a refused figure's card is lit as the one in use")
    # the page's title as its row says it, and the range as the Inventory's
    check('{"Game flags", "Cutscenes", "Scan amiibo"}' in draw and '"%d\\xe2\\x80\\x93%d of %d", first - p.begin() + 1' in draw,
          "the name pages' title or range is not as the rows' and the Inventory's")
    # the Teleport's help names no key: the guides do, a controller's too
    check("then Travel (or Enter)" not in function(menu, "void draw_teleport("), "the Teleport's help names Enter beside a controller's guides")
    # a long answer takes lines, and is not cut at the caption's edge
    check('if (!status.empty() && !one_line) about += "\\n" + status;' in draw and "one_line ? status.c_str() : nullptr" in draw, "a long answer is cut off")
    # the guides: no Move for Up and Down while they do nothing, no Group with one group
    check('if (!names_edit) prompts[prompts_n++] = {ui::kUpDown, "Move"};' in draw and 'if (groups > 1) prompts[prompts_n++] = {ui::kTabs, "Group"};' in draw
          and "ui::prompt_bar(c, prompts, prompts_n);" in draw, "a name page's guides offer keys that do nothing")
    inventory = function(menu, "void draw_inventory(")
    check('if (!inventory_amount) prompts[prompts_n++] = {ui::kUpDown, "Move"};' in inventory, "the Inventory's guides offer Up and Down while an amount is changed")
    # the cards: the name without its group's beginning, cut in its middle; "not" lines that can be read
    check("dn::elide_middle(dn::card_label(p.list, p.group, i)," in draw, "a card's name is cut at its end, where a group's names differ")
    check("st == '1' ? ui::kAccent : st == '0' ? ui::kSoft : kCardOff" in draw and "owned ? ui::kAccent : kCardOff" in inventory and "ui::kDim, amount" not in inventory,
          "a card's second line is drawn in the plate's own tone")
    # the two texts whose dash and dot were encoded twice (they showed as three wrong characters)
    check("â\u0080\u0093" not in menu and "Â·" not in menu and '"%d\\xe2\\x80\\x93%d of %d"' in menu and '"%s \\xc2\\xb7 %s"' in menu,
          "the Inventory's range or the Teleport's note is mis-encoded")
    # the Debug tools turned off: what was asked and not yet taken by the game's side is dropped
    check("if (!cheats_on() && names_asked.exchange(false, std::memory_order_relaxed)) {" in function(menu, "void names_tick(")
          and "names_ask.drop_but('W');" in function(menu, "void names_tick("), "lines asked with the Debug tools on are sent after they were turned off")
    check("return cheats_on() ? action : 0;" in function(menu, "int menu_take_cheat_action("), "a tool's action is taken after the Debug tools were turned off")
    check("names_edit = false;   // (another list under the choice" in function(menu, "void names_tick("), "a flag's On and Off stay open over a new list")
    # the wheel turns the page shown and moves no choice
    mouse = function(menu, "void names_mouse(")
    wheel = [line for line in mouse.splitlines() if "ptr.wheel" in line]
    check(wheel == ["    if (ptr.wheel) p.turn_shown(-ptr.wheel), changed = true;"], "the wheel on a name page does something else than turn the page shown")

    # nothing before a page is opened: the game's files are read, the folder listed and lines made from the pages alone
    code = re.sub(r"//[^\n]*", "", menu)
    check(code.count("std::thread(names_scan_thread)") == 1 and "std::thread(names_scan_thread).detach();" in function(menu, "void names_open("),
          "the game's files are read from somewhere else than a page's opening")
    check(code.count("ui::folder_files(") == 1 and "ui::folder_files(" in function(menu, "void names_open("), "the amiibo folder is listed before its page is opened")
    check(code.count("ui::game_files(") == 2 and function(menu, "void names_scan_thread(").count("ui::game_files(") == 2, "the game's files are read outside the scan")
    pushes = {name: function(menu, "void %s(" % name).count("_ask.push(") for name in ("names_send_flag", "names_confirm", "names_tick")}
    check(pushes == {"names_send_flag": 1, "names_confirm": 2, "names_tick": 1} and code.count("_ask.push(") == 4, "a line is made outside the pages: %s" % pushes)
    names_tick = function(menu, "void names_tick(")
    check("if (shows_flags || names_watching) {" in names_tick and "shows_flags = open && page == 6 && names_kind == kNamesFlags" in names_tick,
          "flags are asked for while the Game flags page is not shown")
    # in memory only: neither the scan nor the listing writes a file
    for text, where in ((function(menu, "void names_scan_thread("), "names_scan_thread"), (function(imgui, "bool game_files("), "game_files"),
                        (function(imgui, "std::vector<std::string> folder_files("), "folder_files")):
        check(not re.search(r"fopen|fwrite|ofstream|CreateFile|WriteFile", text), where + " opens or writes a file itself")
    check("game_font::Reader reader;" in function(imgui, "bool game_files("), "game_files shares the presenting thread's reader")

    # the lines either way: the host reads the pages', and the menu takes the host's
    option = replay[replay.index("t.option = "):]
    option = option[:option.index("t.option_set = ")]
    check("if (id == HOS_OPT_DEBUG_ASK) return menu_take_debug_ask();" in option and "if (id == HOS_OPT_AMIIBO_ASK) return menu_take_amiibo_ask();" in option,
          "the host cannot read the pages' lines (options 115 and 118)")
    check("int menu_take_debug_ask();" in header and "int menu_take_amiibo_ask();" in header, "gpu/vk/vk_menu.h does not declare the two reads")
    check("return names_ask.take();" in function(menu, "int menu_take_debug_ask(") and "return amiibo_ask.take();" in function(menu, "int menu_take_amiibo_ask("),
          "the two reads do not take the lines' bytes")
    option_set = function(menu, "void menu_option_set(int id, int value) {")
    for needle, why in (("if (id == HOS_OPT_DEBUG_TEXT) {", "the game side's line is not taken"),
                        ("if (value == OOT_SAID_FLAGS) names_states.take(names_in.c_str());", "the flags' states are not taken"),
                        ("else if (value == OOT_SAID_STATUS) names_status = names_in", "what became of a flag or an event is not kept"),
                        ("if (id == HOS_OPT_AMIIBO_SAID) {", "what became of the figure is not kept")):
        check(needle in option_set, "menu_option_set: " + why)
    # the readme's settings table
    for row in ("| Scan amiibo |", "| Game flags |", "| Cutscenes |"):
        check(row in readme, "readme.md's settings table has no row " + row)
    check("no key is needed" in readme and "`amiibo`" in readme, "readme.md does not say where the dumps go, or that no key is needed")
    print("PASS: the three rows open page 6 from the Debug tab, and nothing is read, listed or sent before")


if __name__ == "__main__":
    main()
