"""The names on the Debug tab's Game flags, Cutscenes and Scan amiibo pages (gpu/vk/debug_names.h; no game, no game data).

The game knows a flag by the hash of its name alone, and an event by its file's name. Neither list is in the
repository: the pages make them in memory from the player's own game files (the Bools' hashes of the GameDataList, the
texts of the event flows whose hash is one of them, the event flows that hold a script).

Compiles and runs tests/debug_names_test.cpp on files of the test's own making: the Bools' hashes read from a BYML file
of either byte order (and not the Ints'), every shorter file and 20,000 bent ones refused or read without leaving the
file; which event flows are events; the flags' names found among an event flow's texts (not a text that is too short,
too long, not ended by a 0, cut by the file's end or no Bool's); an event's name from its file's; the list (every name
once, sorted, the groups one after another with at least six names each but Other, each name in the group it begins
with, each hash its name's); a page's choice (kept in its group by the arrows, the pages, Home, End, a click and the
wheel; kept by its name when the list is made again); the flags asked for (those shown, 64 at most); the states said
for another list refused; and the lines waiting to be sent (a byte a read, the newest alone of those that say what is
shown, 32 at most).

Reads the source: the header names no flag and no event of the game.

    python -I -B tests/test_debug_names.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    header = (ROOT / "gpu/vk/debug_names.h").read_text(encoding="utf-8")
    code = re.sub(r"//[^\n]*|^#include[^\n]*", "", header, flags=re.M)
    quoted = [q for q in re.findall(r'"([^"\n]*)"', code) if re.fullmatch(r"[A-Za-z]+_[A-Za-z0-9_.]+", q)]
    if quoted:
        raise SystemExit("FAIL: gpu/vk/debug_names.h names a flag or an event: %s" % quoted[:3])
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "debug_names_test.exe"
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/debug_names_test.cpp"), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if run.returncode or run.stdout.strip() != "ok":
            raise SystemExit("FAIL: tests/debug_names_test.cpp (exit %d)\n%s%s" % (run.returncode, run.stdout, run.stderr))
    print("PASS: the Bools' hashes, the flags' and events' names, the list in groups, a page's choice, the states and the lines")


if __name__ == "__main__":
    main()
