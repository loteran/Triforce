"""A wait for a background job with a bound (gpu/vk/vk_jobs.cpp jobs_wait_for; no game/GPU).

A draw that may wait for its pipeline only a while (gpu/vk/pipe_wait.h: a frame's wait budget) must not run the pipeline's
build itself, as jobs_wait does with a job still queued: a compile on the draw's thread could outlast any bound. Its job goes
to the front of the queue instead, for the next worker that is free. vk_jobs.cpp is compiled beside stand-ins for
vk_internal.h and vk_draw.h with one worker, held in a job (tests/jobs_wait_for_test.cpp): the wait must end at its bound
without the job, the job must not run on the waiting thread, and once the worker is free the job waited for runs before the
ones queued before it.

    python -I -B tests/test_jobs_wait_for.py [--cxx g++]
"""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

STUB_INTERNAL = """#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
using HANDLE = void*;
constexpr int THREAD_PRIORITY_BELOW_NORMAL = -1;
inline HANDLE GetCurrentThread() { return nullptr; }
inline int SetThreadPriority(HANDLE, int) { return 1; }
inline long SetThreadDescription(HANDLE, const wchar_t*) { return 0; }
namespace mkwvk {
inline void logf(const char*, ...) {}
}
"""

STUB_DRAW = """#pragma once
namespace mkwvk {
inline void ub_thread_off() {}
}
"""


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_source():
    jobs = (ROOT / "gpu/vk/vk_jobs.cpp").read_text(encoding="utf-8")
    wf = block(jobs, "bool jobs_wait_for(const std::atomic<bool>& flag, int ms) {")
    for needle in ("queue.push_front(std::move(e));", "done_cv.wait_for(lk, std::chrono::milliseconds("):
        if needle not in wf:
            raise SystemExit("vk_jobs.cpp jobs_wait_for no longer has: " + needle)
    if "run_guarded" in wf or "jobs_wait(" in wf:
        raise SystemExit("vk_jobs.cpp jobs_wait_for runs the job on the waiting thread: its bound would not hold")
    if "bool jobs_wait_for(const std::atomic<bool>& flag, int ms);" not in (ROOT / "gpu/vk/vk_jobs.h").read_text(encoding="utf-8"):
        raise SystemExit("vk_jobs.h does not declare jobs_wait_for")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="jobs-wait-for-") as tmp:
        tmp = Path(tmp)
        for name in ("vk_jobs.cpp", "vk_jobs.h"):
            shutil.copy(ROOT / "gpu/vk" / name, tmp / name)
        (tmp / "vk_internal.h").write_text(STUB_INTERNAL, encoding="utf-8")
        (tmp / "vk_draw.h").write_text(STUB_DRAW, encoding="utf-8")
        exe = tmp / "jobs-wait-for"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-pthread", "-I", str(tmp), str(tmp / "vk_jobs.cpp"),
                        str(ROOT / "tests/jobs_wait_for_test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == "__main__":
    main()
