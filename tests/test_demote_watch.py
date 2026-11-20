"""A pool's trial in guest memory far over its baseline ends early (gpu/vk/demote_watch.h, vk_memory.cpp demote_trials; no
game, no GPU).

In the owner's logs a pool the CPU and the GPU both write took the GPU 106 ms a frame while its trial had it in guest memory,
against 7.4 before; the game's thread waited for that GPU in every capture (memory sync 99 ms a submit), for the trial's 10
settling and 10 measured frames, twice. Compiles and runs the header's rule against the trials of those logs (the four costly
ones end, the others are measured to the end as before) and frames whose GPU time comes in late, and reads the source for what
needs the game: that demote_trials asks the rule while the trial settles and while it is measured, that an early end takes the
slower trial's way (mirrored again and measured once more), that the trial under way reaches the stall line, and that the
switch is in the development guide.

    python -I -B tests/test_demote_watch.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def function(text, head):
    at = text.index(head)
    return text[at:text.index("\n}\n", at)]


def check_source():
    memory = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    if '#include "demote_watch.h"' not in memory:
        raise SystemExit("vk_memory.cpp does not include demote_watch.h")
    trials = function(memory, "void demote_trials() {")
    for needle, why in (
            ('demote_watch::quick_on(getenv("RX_VK_DEMOTE_QUICK"))', "the switch is not read"),
            ("(t.phase == 1 || t.phase == 2) && f > t.f &&", "the rule is not asked while the trial settles and is measured"),
            ("demote_watch::gross(t.base_ms,", "the rule is not asked against the trial's baseline"),
            ("t.phase = 2;", "an early end does not become the trial's measure"),
            ("if (t.phase == 2 && (gross ||", "an early end does not take the trial's verdict"),
            ("demote_trial_phase.store(t.phase", "the trial under way is not told to the stall line")):
        if needle not in trials:
            raise SystemExit("vk_memory.cpp demote_trials: " + why)
    # (the early end before the settling frames' own step to phase 2, which would start a fresh measure)
    if trials.index("demote_watch::gross(") > trials.index("if ((t.phase == 1 || t.phase == 3) && f >= t.f + 10)"):
        raise SystemExit("vk_memory.cpp demote_trials: the rule is asked after the settling frames' step")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    if "f.trial_phase = memory_demote_trial(&f.trial_pool);" not in function(replay, "void status_tick() {"):
        raise SystemExit("vk_replay.cpp status_tick: the stall line does not say which trial is under way")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "RX_VK_DEMOTE_QUICK" not in dev:
        raise SystemExit("docs/development.md: RX_VK_DEMOTE_QUICK is not in the switches table")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="demote-watch-") as tmp:
        exe = Path(tmp) / "demote-watch"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_demote_watch.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: a trial far over its baseline ends early")


if __name__ == "__main__":
    main()
