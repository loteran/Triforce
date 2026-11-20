"""The Replay helper threads' tuner and its rule (gpu/vk/replay_threads.h Tuner and rule, called by gpu/vk/vk_replay.cpp
par_tune_present at each present). No game, no GPU.

1. The header compiled and run (tests/replay_threads_tuner_test.cpp):
   "same": with the rule and the back-off off the tuner decides, frame for frame, what the tuner inline in par_tune_present
   decided before (a copy of it as it was is in the fixture), over random runs with loads and stretches where both ways are
   the replay thread alone, the helper threads faster or slower.
   "prev": with the back-off off (RX_VK_PARALLEL_BACKOFF=0) the tuner steps, frame for frame, as the one before the back-off
   (a copy of it), the rule on and off, over random scenes that change.
   "rule": its verdicts on the owner's own scenes (their numbers from the test4, test7 and test8 logs): on without a trial in
   test4's heaviest scene, no trial in test8's (the GPU's time is the frame's limit) or in test7's trial 4 (the game's
   threads are), trials in test7's busy scenes, nothing at the title; the tuner over 20,000 presents of each; the hold after a
   trial took the helpers back off; loads, DLSS or FSR kept in order, the helpers in use: no verdict.
   "backoff": the back-off. On a steady scene like the lab's Market (f24-S1) a trial every ~2,090 presents without it, under
   one in 10,000 with it, and an eighth of the frames the other way or fewer, the helper threads kept; on scenes that change
   every 8,000 presents fewer trials, the better way in use as often and as soon after a change; a short trial never changes
   the way and one that finds the other way shorter is followed at once by a full one; a change the scene's means cannot see
   found by a backed-off trial; loads are no scene change; the rule's no-trial verdicts hold through scene changes.
2. The source read: par_tune_present returns at once unless the tuner decides (the Debug row's Off, RX_VK_PARALLEL), hands
   each frame to Tuner::step with the way in use, and applies its change; the rule is RX_VK_PARALLEL_RULE's to turn off, and
   counts the replay thread among the threads that record segments only where it does (RX_VK_PARALLEL_OWN); the back-off is
   RX_VK_PARALLEL_BACKOFF's to turn off.

    python -I -B tests/test_replay_threads_tuner.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def check(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


def function(text, head):
    start = text.find(head)
    check(start >= 0, "%s is missing" % head.rstrip(" {("))
    return text[start:text.index("\n}\n", start) + 3]


def in_order(text, steps, what):
    at = 0
    for s in steps:
        i = text.find(s, at)
        check(i >= 0, "%s: %r is missing or out of order" % (what, s))
        at = i + len(s)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    tune = function(replay, "void par_tune_present() {")
    in_order(tune, ["if (!par_auto()) return;", "const replay_threads::Frame fr = par_frame_now(ms);",
                    "t.use_rule = par_rule_on();", "t.backoff = par_backoff_on();",
                    "t.threads = par_threads_max() + (par_own_on() ? 1 : 0);",
                    "tuner.step(fr, par_auto_on, par_stream_order_only());", "if (s.flip) par_auto_on = !par_auto_on;"],
             "par_tune_present")
    check("phase" not in tune and "kEvery" not in tune, "par_tune_present decides on its own again: the tuner is replay_threads.h's")
    rule_on = function(replay, "bool par_rule_on() {")
    check('getenv("RX_VK_PARALLEL_RULE")' in rule_on and "atoi(getenv(\"RX_VK_PARALLEL_RULE\")) != 0" in rule_on,
          "par_rule_on no longer reads RX_VK_PARALLEL_RULE (on unless 0)")
    backoff = function(replay, "bool par_backoff_on() {")
    check('getenv("RX_VK_PARALLEL_BACKOFF")' in backoff and 'atoi(getenv("RX_VK_PARALLEL_BACKOFF")) != 0' in backoff,
          "par_backoff_on no longer reads RX_VK_PARALLEL_BACKOFF (on unless 0)")
    own = function(replay, "bool par_own_on() {")
    check('getenv("RX_VK_PARALLEL_OWN")' in own, "par_own_on no longer reads RX_VK_PARALLEL_OWN")
    check("bool par_auto() {\n    static const bool v = !getenv(\"RX_VK_PARALLEL\") && par_threads_max() >= 2;" in replay,
          "par_auto changed: the tuner (and its rule) must stay off with RX_VK_PARALLEL and with the row's Off")
    guide = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
    check("`RX_VK_PARALLEL_RULE=0`" in guide, "docs/development.md does not say what RX_VK_PARALLEL_RULE=0 does")
    check("`RX_VK_PARALLEL_BACKOFF=0`" in guide, "docs/development.md does not say what RX_VK_PARALLEL_BACKOFF=0 does")
    with tempfile.TemporaryDirectory(prefix="replay-tuner-") as tmp:
        exe = Path(tmp) / "tuner.exe"
        built = subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(VK),
                                str(ROOT / "tests" / "replay_threads_tuner_test.cpp"), "-o", str(exe)], capture_output=True, text=True)
        check(built.returncode == 0, "the fixture did not build:\n" + built.stderr[-4000:])
        env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_")}
        for case in ("same", "prev", "rule", "backoff"):
            r = subprocess.run([str(exe), case], env=env, capture_output=True, text=True, timeout=300)
            print((r.stdout + r.stderr).strip())
            check(r.returncode == 0, "replay_threads_tuner_test %s" % case)
    print("PASS: the helper threads' tuner decides as it did inline with the rule off; with it, on without a trial where the "
          "replay is plainly the frame's limit, no trial where the GPU or the game's threads are, trials in between; the "
          "back-off makes them rarer and shorter once the way holds, early where the scene changes, off as before")


if __name__ == "__main__":
    main()
