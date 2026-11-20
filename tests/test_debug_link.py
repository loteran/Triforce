"""Game flags by name and events by name: the Debug tab's two pages and the game's side (no game, no game data).

A flag is a Bool of the game's GameData, known to the game by the MurmurHash3 of its name; an event is started by its
file's name. The pages (gpu/vk/vk_menu.cpp) read both lists from the player's own game files and ask the game's side
(host/hos_main.c) in lines (gpu/vk/oot_debug_link.h).

Compiles and runs tests/debug_link_test.c with the game side's functions cut out of host/hos_main.c as they are
(oot_singleton, oot_gd_lookup, oot_gd_bool, oot_gd_set, oot_link_say, oot_flag_state, oot_flag_set, oot_start_event,
oot_link_tick) against a stand-in for the game's memory and functions: the hash on the names and numbers the existing
tools use; the lines made and read back, and malformed ones refused; the watched flags' states (set, not set, not in
the save data, no save loaded) twice a second; a flag set and cleared through the game's own setter with its three
arguments, and nothing else called or written; nothing called or written for a flag the save data does not have or
while no save is loaded; an event asked of the event manager once by its name with Link as its starter, and not while
no save is loaded, another event plays, a map loads or a teleport is under way; the game's registers as they were
after each; and nothing read of the game while no line came and no flag is watched.

Reads the source for what cannot be run: the lines are read at Link's update on the game's thread, and the two files
carry no list of the game's flags or events.

    python -I -B tests/test_debug_link.py [--cc gcc]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
CUT = ("static uint64_t oot_singleton(", "static int64_t oot_gd_lookup(", "static uint64_t oot_gd_bool(", "static int oot_gd_set(",
       "static void oot_link_say(", "static char oot_flag_state(", "static int oot_flag_set(", "static int oot_start_event(",
       "static void oot_link_tick(")


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


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
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    host = (ROOT / "host/hos_main.c").read_text(encoding="utf-8")
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    link = (ROOT / "gpu/vk/oot_debug_link.h").read_text(encoding="utf-8")

    # at Link's update, on the game's thread, with the other tools' actions
    update = block(host, "static void oot_update_player_cheats(")
    check("oot_link_tick(c, actor, now);" in update and update.index("oot_inventory_refresh(c, now);") < update.index("oot_link_tick(c, actor, now);"),
          "host/hos_main.c does not read the pages' lines at Link's update")
    check(host.count("oot_link_tick(") == 2, "the pages' lines are read from somewhere else too")
    tick = block(host, "static void oot_link_tick(")
    check("if (!watching || now < next) return;" in tick and tick.index("oot_ask_parse(line, &ask)") < tick.index("oot_pouch_count("),
          "a line is acted on before it is read whole, or the game asked before a line came")
    # a flag: read where the game's own getter reads it, changed by the game's own setter and by nothing else
    setter = block(host, "static int oot_flag_set(")
    check("if (!mgr || !hash || !oot_gd_bool(c, mgr, hash)) return 0;" in setter and "const uint64_t mgr = loaded ? oot_singleton(c, OOT_GAMEDATA_SLOT) : 0;" in setter
          and "recomp_store" not in setter and "A64_PTR" not in setter, "a flag is written without the game's setter, or without the guards")
    check("recomp_store" not in block(host, "static char oot_flag_state("), "reading a flag's state writes")
    # no list of the game's names in the repository: the two files name a flag or an event only where the older tools did
    own = host[host.index("/* ---- (Triforce) Game flags by name and events by name"):host.index("static uint64_t oot_gameplay_view(")]
    quoted = re.findall(r'"([A-Za-z]+_[A-Za-z0-9_.]+)"', own + link)
    check(not quoted, "a flag's or an event's name is written into the source: %s" % quoted[:3])

    constants = "enum { HOS_OPT_DEBUG_ASK = 115, HOS_OPT_DEBUG_TEXT, HOS_OPT_DEBUG_SAID, HOS_OPT_AMIIBO_ASK, HOS_OPT_AMIIBO_SAID };\n"
    check(constants.strip() in api, "gpu/vk/vk_api.h: the option ids 115 to 119")
    for name in ("OOT_GAMEDATA_SLOT", "OOT_EVENTMGR_SLOT", "OOT_EVENT_ARG_SIZE"):
        found = re.findall(r"^#define %s\s+0x[0-9a-fA-F]+ULL" % name, host, re.M)
        check(len(found) == 1, "host/hos_main.c: " + name)
        constants += found[0] + "\n"
    functions = "\n".join(block(host, marker) for marker in CUT)
    source = (ROOT / "tests/debug_link_test.c").read_text(encoding="utf-8")
    check(source.count("\nHOST_CONSTANTS\n") == 1 and source.count("\nHOST_FUNCTIONS\n") == 1, "tests/debug_link_test.c: the two places for the host's code")
    source = source.replace("\nHOST_CONSTANTS\n", "\n" + constants).replace("\nHOST_FUNCTIONS\n", "\n" + functions + "\n")
    source = source.replace('#include "../gpu/vk/oot_debug_link.h"', '#include "%s"' % (ROOT / "gpu/vk/oot_debug_link.h").as_posix())
    with tempfile.TemporaryDirectory() as tmp:
        c_file, exe = Path(tmp) / "debug_link_test.c", Path(tmp) / "debug_link_test.exe"
        c_file.write_text(source, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-format-security",
                        "-Wno-format-nonliteral", "-Wno-unused-but-set-variable", str(c_file), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if run.returncode or run.stdout.strip() != "ok":
            raise SystemExit("FAIL: tests/debug_link_test.c (exit %d)\n%s%s" % (run.returncode, run.stdout, run.stderr))
    print("PASS: the pages' lines, the flags' states, a flag set through the game's setter and an event asked for, and the guards")


if __name__ == "__main__":
    main()
