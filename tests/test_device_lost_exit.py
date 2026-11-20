"""A lost device ends the process once its report is written (gpu/vk/vk_replay.cpp device_lost, issue 1275).

After VK_ERROR_DEVICE_LOST the replay went on: no submission completed again, so no capture or staging arena was let
go, and system memory filled until the window was closed. Every VK_ERROR_DEVICE_LOST in vk_replay.cpp must now go to
device_lost(), and none to checkpoints_report() alone. device_lost() is cut into a fixture with checkpoints_report() and
replay_fatal() as spies, and called from several threads at once: the report must be written exactly once, and the
process must end (exit code 3) only after it is done, whichever thread gets there first.
replay_fatal must write out the host's queued log (MkwVkHost.log_flush, host/rx_backend.c) before it ends the process:
on Windows the report's lines are queued, and _Exit dropped them.

python -I -B tests/test_device_lost_exit.py [--cxx g++]
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    if text.count(marker) != 1:
        raise SystemExit("marker not found once: " + marker)
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


FIXTURE = r"""
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
static std::atomic<int> reports{0};
static std::atomic<bool> report_done{false};
void memory_trial_device_lost() {}   // (vk_memory.cpp: the memory handling on trial is marked failed)
void checkpoints_report() {
    reports++;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // (a slow report: the others must wait for it)
    report_done = true;
}
[[noreturn]] void replay_fatal(const char* why) {
    std::printf("%s: %d report(s), %s\n", why ? "fatal" : "?", reports.load(), report_done ? "done" : "NOT done");
    if (reports != 1 || !report_done) std::printf("FAIL: the process ended before its one report was written\n");
    std::fflush(stdout);
    std::_Exit(3);
}
@@DEVICE_LOST@@
int main() {
    std::vector<std::thread> t;
    for (int i = 0; i < 4; i++) t.emplace_back([] { device_lost(); });
    device_lost();
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    sites = [line.strip() for line in replay.splitlines() if "VK_ERROR_DEVICE_LOST" in line and "if (" in line]
    if len(sites) < 4:
        raise SystemExit("FAIL: fewer VK_ERROR_DEVICE_LOST checks than the four known: update this test")
    stray = [s for s in sites if not re.search(r"== VK_ERROR_DEVICE_LOST\) device_lost\(\);", s)]
    if stray:
        raise SystemExit("FAIL: a device lost that does not end the process: " + "; ".join(stray))
    # (the report is logf lines, which the host queues on Windows: replay_fatal writes the queue out before the end)
    fatal = block(replay, "[[noreturn]] void replay_fatal(")
    if "end_process(3);" not in fatal or "_Exit(" in fatal:
        raise SystemExit("FAIL: replay_fatal does not end the process through end_process")
    if "host->log_flush()" not in fatal or fatal.index("host->log_flush()") > fatal.index("end_process("):
        raise SystemExit("FAIL: replay_fatal ends the process without writing out the host's queued log")
    # (on Windows the end is TerminateProcess: _Exit there is ExitProcess, which runs every library's detach, and one
    # that faults or waits put a second crash after the report or held the process open. The recorder thread's fatal
    # end and the host's hos_fatal end the same way)
    end = block(replay, "[[noreturn]] void end_process(")
    if "TerminateProcess(GetCurrentProcess(), (UINT)code);" not in end or end.index("TerminateProcess(") > end.index("std::_Exit("):
        raise SystemExit("FAIL: end_process does not end the process with TerminateProcess on Windows")
    recorder = (ROOT / "gpu" / "vk" / "vk_recorder.h").read_text(encoding="utf-8")
    rec_fatal = block(recorder, "[[noreturn]] inline void recorder_fatal(")
    if "g_end_process_hook(3)" not in rec_fatal or "g_end_process_hook = " not in replay:
        raise SystemExit("FAIL: the recorder thread's fatal end does not go through end_process")
    host_fatal = block((ROOT / "host" / "hos_thread.c").read_text(encoding="utf-8"), "void hos_fatal(")
    # (the last one: the first is the end of a fatal error inside this thread's own report, which writes nothing more)
    ends_with = "TerminateProcess(GetCurrentProcess(), 3);"
    if ends_with not in host_fatal or host_fatal.index("fflush(stderr)") > host_fatal.rindex(ends_with):
        raise SystemExit("FAIL: hos_fatal does not end the process with TerminateProcess, after its lines are written")
    backend = (ROOT / "host" / "rx_backend.c").read_text(encoding="utf-8")
    # (in MkwVkHost's order, right after gyro_status; later fields may follow it)
    if not re.search(r"hos_sdl_gyro_status,\s*hos_log_flush,", backend):
        raise SystemExit("FAIL: host/rx_backend.c does not give the back end hos_log_flush (MkwVkHost.log_flush)")
    with tempfile.TemporaryDirectory(prefix="device-lost-") as tmp:
        src, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        src.write_text(FIXTURE.replace("@@DEVICE_LOST@@", block(replay, "[[noreturn]] void device_lost() {")), encoding="utf-8")
        command = [args.cxx, "-std=c++17", "-O1", str(src), "-o", str(exe)] + ([] if os.name == "nt" else ["-pthread"])
        r = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("the fixture did not build")
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        print(r.stdout, end="")
        if r.returncode != 3 or "FAIL" in r.stdout or "1 report(s), done" not in r.stdout:
            raise SystemExit("FAIL: device_lost (exit code %d)" % r.returncode)
    print("PASS: every device lost goes to device_lost, which writes its report once and then ends the process")


if __name__ == "__main__":
    main()
