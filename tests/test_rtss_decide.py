"""RivaTuner Statistics Server as the game's frame limiter: its decision rules (no RTSS, no game, no GPU).

Where RTSS is installed and running the game's frame limit goes into RTSS's profile for the game and RTSS limits the
game instead of the game's own wait (gpu/vk/rtss_limit.cpp, gpu/vk/vk_pace.cpp, after NX2navi's rtss_limit.c). The
rules that make no system call are gpu/vk/rtss_decide.h and gpu/vk/pace_rule.h.

Compiles and runs tests/test_rtss_decide.c, as C (undefined behaviour and unsigned wrap trapping) and as C++: RTSS's
shared memory absent, too small, closed, without the game, with it, cut off, with wild sizes; the worker's polls
(NX2navi's cases: the injection delay, RTSS quitting and coming back, a profile that cannot be written, a refused limit,
RTSS's count past the limit, 30 s without its hold, a minimised window, kept out and let in, a want that flips, movies,
the hooks library); Triforce's own: the retry after a give-back, the hysteresis on a limit that changes in play (a
flicker writes the profile once), the player's own limit where the profile cannot be written; the state file's lines;
RTSS's profile texts; the frames' own check; and what RTSS is asked to hold for every cap and frame generation mode.

    python -I -B tests/test_rtss_decide.py [--cc gcc] [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    # (the rules make no system call: no header but the C library's plain ones)
    for name in ("rtss_decide.h", "pace_rule.h"):
        text = (ROOT / "gpu/vk" / name).read_text(encoding="utf-8")
        check(set(re.findall(r"#include <([^>]+)>", text)) <= {"stddef.h", "stdint.h", "stdio.h", "string.h"},
              name + " includes more than the C library's plain headers")
    src = str(ROOT / "tests/test_rtss_decide.c")
    inc = ["-I", str(ROOT / "gpu/vk")]
    with tempfile.TemporaryDirectory(prefix="rtss-decide-") as tmp:
        version = subprocess.run([args.cc, "--version"], capture_output=True, text=True).stdout
        ub = "undefined,unsigned-integer-overflow"
        trap = ["-fsanitize=" + ub, "-fsanitize-trap=" + ub] if "clang" in version else []
        for compiler, std, name in ((args.cc, "-std=gnu11", "c"), (args.cxx, "-std=c++20", "cxx")):
            exe = Path(tmp) / ("rtss-decide-" + name)
            lang = ["-x", "c++"] if name == "cxx" else []
            subprocess.run([compiler, std, "-O1", "-Wall", "-Wextra", "-Werror", *(trap if name == "c" else []), *inc, *lang, src,
                            "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
    print("PASS: RivaTuner's decisions, the state file's lines and who holds a frame, as gpu/vk/rtss_decide.h and pace_rule.h rule")


if __name__ == "__main__":
    main()
