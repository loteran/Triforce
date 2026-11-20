"""The shader preparation before the title runs on every processor where that is safe, not on half of them.

prepare_run, warm_libraries and state_list_prepare (gpu/vk/vk_draw.cpp) each started clamp(cpus / 2, 1, 32) workers, at
below normal priority: "on all of them at High the driver's compiles froze the whole PC". Nothing of the game runs
during that preparation, and a worker below normal in a process of normal priority gives way to every other program, so
on a player's PC half the processors stood idle through a wait of minutes. NX2navi prepares on every processor.

What must hold: gpu/vk/prepare_threads.h's rule (compiled and run here, no game or GPU): every processor, 64 at most; a
worker past half only for each GB of RAM free at the start; half in a process started above normal priority and when the
RAM is not known; never fewer than half; RX_VK_PREPARE_THREADS=<n> decides outright. And vk_draw.cpp: the three
functions take their count from it, the preparation behind the game keeps its own (a quarter, 2 to 4), and predict_run
and precompile_run are as they were.

python -I -B tests/test_prepare_threads.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit(where + " no longer has: " + needle)


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    need("vk_draw.cpp", draw, ['#include "prepare_threads.h"'])
    workers = block(draw, "unsigned prepare_workers() {")
    need("vk_draw.cpp: prepare_workers()", workers,
         ['getenv("RX_VK_PREPARE_THREADS")', "GlobalMemoryStatusEx(&ms)", "ram_free = ms.ullAvailPhys;",
          "GetPriorityClass(GetCurrentProcess())", "cls == ABOVE_NORMAL_PRIORITY_CLASS || cls == HIGH_PRIORITY_CLASS",
          "prepare_threads::count(cpus, ram_free, above_normal, e ? atoi(e) : 0)", "MemAvailable"])
    # (the preparation before the title: the count is the rule's, and the workers stay below normal priority)
    for name, marker in [("state_list_prepare", "void state_list_prepare(void (*progress)(size_t done, size_t total)) {"),
                         ("warm_libraries", "bool warm_libraries(void (*progress)(size_t done, size_t total)) {")]:
        body = block(draw, marker)
        need("vk_draw.cpp: %s()" % name, body,
             ["const unsigned workers = prepare_workers();", "for (unsigned i = 0; i < workers; i++) threads.emplace_back(",
              "SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);"])
        if "cpus / 2" in body or "hardware_concurrency" in body:
            raise SystemExit("vk_draw.cpp: %s() counts its workers itself again" % name)
    prepare = block(draw, "bool prepare_run(void (*progress)(size_t done, size_t total), bool background) {")
    need("vk_draw.cpp: prepare_run()", prepare,
         # (behind the game: its own count and idle priority, as before)
         ["const unsigned workers = background ? std::clamp(cpus / 4, 2u, 4u) : prepare_workers();",
          "SetThreadPriority(GetCurrentThread(), background ? THREAD_PRIORITY_IDLE : THREAD_PRIORITY_BELOW_NORMAL);",
          "for (unsigned i = 0; i < workers; i++) threads.emplace_back(worker);"])
    if "cpus / 2" in prepare:
        raise SystemExit("vk_draw.cpp: prepare_run() starts half the processors' workers again")
    # (not asked to change: the predicted pipelines' pass and the precompile)
    need("vk_draw.cpp: predict_run()", block(draw, "void predict_run(void (*progress)(size_t done, size_t total)) {"),
         ["for (unsigned i = 0; i < std::clamp(cpus / 2, 1u, 32u); i++) threads.emplace_back(worker);"])
    need("vk_draw.cpp: precompile_run()", block(draw, "void precompile_run(void (*progress)(size_t done, size_t total)) {"),
         ["for (unsigned i = 0; i < std::min(cpus, 64u); i++) threads.emplace_back(worker);"])
    docs = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    need("docs/development.md", docs, ["`RX_VK_PREPARE_THREADS=<n>`"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="prepare-threads-") as tmp:
        exe = Path(tmp) / "prepare-threads"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_prepare_threads.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
