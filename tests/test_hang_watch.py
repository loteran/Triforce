"""The hang watch (gpu/vk/vk_hangdump.cpp, gpu/vk/hang_watch.h; no game, no GPU).

A game that froze left a log that just stopped. The watch is on in every run: when no frame is shown for 20 seconds with
the game's window in front, where every thread was goes into the log and into triforce_hang.txt beside the game.

Compiles and runs the judgement (when a report is due), and reads the source for what a test cannot run without the
game: that the watch is on unless RX_VK_HANG_DUMP=0, starts at the first present, judges by the window in front unless a
file was named, writes into the game folder, and writes the log out after a report.

    python -I -B tests/test_hang_watch.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    dump = (ROOT / "gpu/vk/vk_hangdump.cpp").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    for needle, why in (
            ('(named && !strcmp(named, "0")) || started.exchange(true)', "the watch is not on by default, off only with RX_VK_HANG_DUMP=0"),
            ("hangwatch::step(watch, presents, by_name || window_in_front(), sec)", "a second no longer counts by the window in front"),
            ('"triforce_hang.txt"', "the stacks no longer go to triforce_hang.txt"),
            ("game_module_path(exe, MAX_PATH)", "the file is no longer in the game folder"),
            ("host->log_flush()", "the log is not written out after a report"),
            ("pid == GetCurrentProcessId() && !IsIconic(w)", "a minimized window, or another program's, counts as in front")):
        if needle not in dump:
            raise SystemExit("gpu/vk/vk_hangdump.cpp: " + why)
    # ((Triforce) the stacks asked for from outside: looked for every second before the stall's judgement, the file taken away
    # before the report, the report as a stall's, the log written out, a few a run)
    loop = dump[dump.index("hangwatch::Watch watch;"):dump.index("}).detach();")]
    at = 0
    for part in ("int asked = 0;", "Sleep(1000);", "const std::string ask_path = path + hangwatch::kAskSuffix;", "if (!ask_stuck && game_path_there(ask_path)) {",
                 "if (!ask_take(ask_path)) {", "ask_stuck = true;", "} else if (hangwatch::ask(asked)) {", "dump(path.c_str(), true)", "host->log_flush();",
                 "hangwatch::step(watch, presents, by_name || window_in_front(), sec)"):
        found = loop.find(part, at)
        if found < 0:
            raise SystemExit("gpu/vk/vk_hangdump.cpp: the asked-for stacks: %r is missing or out of order" % part)
        at = found + len(part)
    take = dump[dump.index("bool ask_take(const std::string& path) {"):dump.index("/// The game's own window")]
    if "DeleteFileW(" not in take or "MultiByteToWideChar(CP_UTF8" not in take or "remove(" in take:
        raise SystemExit("gpu/vk/vk_hangdump.cpp: the ask's file is not taken away by its UTF-8 path with DeleteFileW")
    header = (ROOT / "gpu/vk/hang_watch.h").read_text(encoding="utf-8")
    if 'constexpr const char* kAskSuffix = ".now";' not in header:
        raise SystemExit("gpu/vk/hang_watch.h: the ask's file is no longer the report's name with .now")
    if "if (presents == 1) hang_watch_start();" not in replay:
        raise SystemExit("gpu/vk/vk_replay.cpp no longer starts the hang watch at the first present")
    # (nothing is logged or allocated while a thread is suspended: the log's lines come after every ResumeThread)
    body = dump[dump.index("bool dump("):dump.index("/// The game's own window")]
    if body.index("logf(") < body.rindex("ResumeThread(") or body.index("fopen(") < body.rindex("ResumeThread("):
        raise SystemExit("gpu/vk/vk_hangdump.cpp logs or opens the file before every thread runs again")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="hang-watch-") as tmp:
        exe = Path(tmp) / "hang-watch"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_hang_watch.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
