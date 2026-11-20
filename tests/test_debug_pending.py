"""The Inventory page's change waiting for the game (gpu/vk/debug_pending.h, gpu/vk/vk_menu.cpp; no game).

The game's side takes a debug tool's action at Link's update, which does not run while the game's own Options screen has
the game paused, and one action waits at a time (cheat_action). Each press on the Inventory page used to replace the
waiting one: ten presses of Right on an amount made it one more when the game went on, a second Unlock was lost, and the
card showed the amount as it was, so a press read as one that did nothing.

Compiles and runs tests/debug_pending_test.cpp: the steps on one item's amount add up (and take back), within 0 to 999
and what one action can say; Left at 0 asks nothing; another item's waiting action is replaced as before; a second
Unlock takes the first back; the cards show the change waiting, and nothing for any other action.

Reads vk_menu.cpp for the rest: the amount's Left and Right and the Unlock press go through those functions (a compare
and exchange, as the game's side takes the action on another thread), and the cards and the amount show what they say.

    python -I -B tests/test_debug_pending.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

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
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    tick = function(menu, "void menu_tick() {")
    inventory = tick[tick.index("} else if (open_now.load() && page == 4) {"):tick.index("} else if (open_now.load() && page == 6) {")]
    check("debug_pending::amount_step(was, item_choice, count, press[4] ? 1 : -1)" in inventory
          and "debug_pending::toggle(was, item_choice)" in inventory and "compare_exchange_weak" in inventory,
          "the Inventory's presses do not add to the change waiting")
    check("cheat_action.store(OOT_CHEAT_ITEM_ACTION" not in inventory, "a press on the Inventory still replaces the change waiting")
    draw = function(menu, "void draw_inventory(")
    check("debug_pending::shown(pending, item_choice, item_counts[item_choice]" in draw and "debug_pending::shown(pending, i, item_counts[i]" in draw,
          "the Inventory does not show the change waiting")
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "debug_pending_test.exe"
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/debug_pending_test.cpp"), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if run.returncode or run.stdout.strip() != "ok":
            raise SystemExit("FAIL: tests/debug_pending_test.cpp (exit %d)\n%s%s" % (run.returncode, run.stdout, run.stderr))
    print("PASS: the Inventory's presses add up while the game has not taken them, and the cards show them")


if __name__ == "__main__":
    main()
