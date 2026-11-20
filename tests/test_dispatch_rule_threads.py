"""RX_VK_DISPATCH_BARRIER=game beside the parallel replay's helper threads (gpu/vk/cmd_work.h, barrier_rules.h; no GPU).

The rule leaves out the full barrier of a dispatch right after the one before it on the same command buffer, nothing recorded
between. "Nothing recorded between" was the global count of commands, which every helper thread's work moves too, so the rule stood
aside while they recorded (most frames at the Market). (wp14) Then it was the recording thread's own count; (Triforce, P4) now it
is the command buffer's own (cmd_work.h: each command counted in the state of the command buffer it goes into, the marks there
too), so another thread's work into its own command buffer leaves this one's alone, and two threads taking turns at one command
buffer both count in it.

Runs cmd_work.h as it is with barrier_rules.h's rule and vk_draw.cpp dispatch's lines for it (the test reads that they are there):
1. two threads, each recording into its own command buffer, in lock step, one with dispatches and a copy between, the other with
   work of its own between each of the first one's steps: the first thread's dispatches get the barriers they get alone (the second
   right after the first: none; the one after the copy: one; the next: none), and its command buffer begun again starts afresh;
2. two threads taking turns at one command buffer: a dispatch, the other thread's copy into it, a dispatch again: the second
   dispatch gets its barrier.
Then: with RX_VK_BARRIER_CONTEXT=0 (the one count of every thread's commands, as before) the second thread's work gives every
dispatch of 1 a barrier, so 1 fails; with the count the thread's own instead of the command buffer's (package D's way), 2 leaves
the second dispatch without its barrier, so 2 fails.

    python -I -B tests/test_dispatch_rule_threads.py [--cxx g++] --headers <Vulkan headers' include folder>
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


STATE = "CmdWork& cw = cmd_work(cmd);"
RIGHT_AFTER = "const bool right_after = game && work_right_after(cw, cw.dispatch_at)"
LAST = "cw.dispatch_at = work_at(cw);"
OWN_COUNT = "return work_global() ? work_epoch.load(std::memory_order_relaxed) : w.n;"

TEST = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include "barrier_rules.h"
#include "cmd_work.h"
namespace mkwvk {
std::atomic<uint64_t> work_epoch{0};
/// (vk_draw.cpp dispatch's steps for the rule: whether it records its barrier; the barrier itself is work, as vk_work.h counts it)
bool dispatch(VkCommandBuffer cmd) {
    const bool game = true;
    /* STATE */
    /* RIGHT_AFTER */;
    const bool barrier = barrier_rules::dispatch_barrier(true, false, game, right_after, false);
    if (barrier) note_work(cmd);
    note_work(cmd);   // (the dispatch)
    /* LAST */
    return barrier;
}
} // namespace mkwvk
using namespace mkwvk;
std::mutex mu;
std::condition_variable cv;
int turn = 0;
void step(int me, int k) {   // (the two threads in lock step: thread 0 takes the even turns, thread 1 the odd ones)
    std::unique_lock lk(mu);
    cv.wait(lk, [&] { return turn == 2 * k + me; });
}
void done() { std::lock_guard lk(mu); turn++; cv.notify_all(); }
int main() {
    int fails = 0;
    {   // 1. each thread its own command buffer
        std::vector<int> got;
        VkCommandBuffer a = (VkCommandBuffer)(uintptr_t)0x10, b = (VkCommandBuffer)(uintptr_t)0x20;
        work_begun(a), work_begun(b);
        turn = 0;
        std::thread t0([&] {   // (a segment: dispatch, dispatch, a copy, dispatch, dispatch; its command buffer begun again, dispatch)
            const char* plan = "DDCDDRD";
            for (int k = 0; plan[k]; k++) {
                step(0, k);
                if (plan[k] == 'D') got.push_back(dispatch(a));
                else if (plan[k] == 'C') note_work(a);
                else work_begun(a);
                done();
            }
        });
        std::thread t1([&] {   // (another segment's thread: work of its own between each step of the first)
            for (int k = 0; k < 7; k++) {
                step(1, k);
                note_work(b), note_work(b);
                done();
            }
        });
        t0.join(), t1.join();
        const std::vector<int> want = {1, 0, 1, 0, 1};
        std::printf("own command buffers: ");
        for (int g : got) std::printf("%d", g);
        std::printf("\n");
        if (got != want) std::printf("FAIL 1: beside another thread's work the dispatches got other barriers than alone (want 10101)\n"), fails++;
    }
    {   // 2. two threads taking turns at one command buffer
        std::vector<int> got;
        VkCommandBuffer c = (VkCommandBuffer)(uintptr_t)0x30;
        work_begun(c);
        turn = 0;
        std::thread t0([&] {
            step(0, 0), got.push_back(dispatch(c)), done();
            step(0, 1), got.push_back(dispatch(c)), done();
        });
        std::thread t1([&] {
            step(1, 0), note_work(c), done();   // (a copy into it between the two dispatches)
        });
        t0.join(), t1.join();
        std::printf("one command buffer, two threads: %d%d\n", got[0], got[1]);
        if (got != std::vector<int>{1, 1}) std::printf("FAIL 2: the dispatch after the other thread's copy into its command buffer had no barrier\n"), fails++;
    }
    if (!fails) std::printf("PASS: the dispatches get the barriers their command buffers' own work gives them, beside another thread's\n");
    return fails ? 1 : 0;
}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    ap.add_argument("--headers", type=Path, required=True)
    a = ap.parse_args()
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    disp = block(draw, "void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {")
    for line in (STATE, RIGHT_AFTER, LAST):
        if line not in disp:
            raise SystemExit("vk_draw.cpp dispatch: %r is not there (update the test with the code)" % line)
    state = (VK / "cmd_work.h").read_text(encoding="utf-8")
    if state.count(OWN_COUNT) != 1:
        raise SystemExit("cmd_work.h work_at: %r is not there once (update the test with the code)" % OWN_COUNT)
    src = TEST.replace("/* STATE */", STATE).replace("/* RIGHT_AFTER */", RIGHT_AFTER).replace("/* LAST */", LAST)
    with tempfile.TemporaryDirectory(prefix="dispatch-threads-") as tmp:
        d = Path(tmp)

        def run(name, state_text, env=None):
            sub = d / name
            sub.mkdir()
            (sub / "cmd_work.h").write_text(state_text, encoding="utf-8")
            cpp, exe = sub / "t.cpp", sub / "t.exe"
            cpp.write_text(src, encoding="utf-8")
            r = subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread", "-I" + str(sub), "-I" + str(VK),
                                "-I" + str(a.headers), str(cpp), "-o", str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if r.returncode:
                raise SystemExit("%s: the build failed:\n%s" % (name, r.stdout))
            e = {k: v for k, v in os.environ.items() if k != "RX_VK_BARRIER_CONTEXT"}
            e.update(env or {})
            return subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60, env=e)

        r = run("own", state)
        if r.returncode:
            raise SystemExit("the threads check failed:\n" + r.stdout)
        print(r.stdout.strip().splitlines()[-1])
        r = run("global", state, {"RX_VK_BARRIER_CONTEXT": "0"})
        if r.returncode == 0 or "FAIL 1" not in r.stdout:
            raise SystemExit("with RX_VK_BARRIER_CONTEXT=0 (the global count) check 1 still passed: it does not tell the two apart:\n" + r.stdout)
        print("PASS mutation: with the global count another thread's work gives every dispatch its barrier (%s)" %
              [line for line in r.stdout.splitlines() if line.startswith("FAIL 1")][0])
        r = run("thread", state.replace(OWN_COUNT, OWN_COUNT.replace(": w.n;", ": work_mine + 0 * w.n;")))
        if r.returncode == 0 or "FAIL 2" not in r.stdout:
            raise SystemExit("with the count the thread's own check 2 still passed: it does not tell the two apart:\n" + r.stdout)
        print("PASS mutation: with the count the thread's own, a dispatch after another thread's copy into its command buffer loses its "
              "barrier (%s)" % [line for line in r.stdout.splitlines() if line.startswith("FAIL 2")][0])
    print("ALL PASS")


if __name__ == "__main__":
    main()
