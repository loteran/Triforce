"""The replay's priority while it records (gpu/vk/replay_prio.h, vk_replay.cpp ReplayPin; no game, no GPU).

After NX2navi's recorder_prio: the thread that replays the game's frames is raised to the highest priority for as long as it
replays (RX_VK_REPLAY_PRIO: unset or 2 highest, 1 above normal, 0 as before) and given its own priority back after. Compiles and
runs the header's rule over the switch's values; then the production ReplayPin cut out of vk_replay.cpp against stand-ins for
Windows' thread calls: the outermost pin raises the thread once, a pin inside it changes nothing, the last one out gives the thread
the priority it had, a thread already as high or higher (or one whose priority Windows could not read, or a raise Windows refused)
is left alone and nothing is given back for it, nothing at all with the switch at 0 or with the replay on its own thread
(RX_VK_ASYNC=1), and the affinity pin goes on as before. Each of those guards taken out in turn (a mutation of the cut code) must
make a check fail. Reads the source for the rest: the replays and the present take the pin, the replay thread of RX_VK_ASYNC=1 and
the recorder thread take the level for good, and the switch is in the development guide.

    python -I -B tests/test_replay_prio.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

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


def cut_pin(replay):
    """ReplayPin with its thread-locals, from the first of them to the struct's end"""
    start = replay.index("thread_local int replay_pin_depth = 0;")
    struct = block(replay, "struct ReplayPin {")
    end = replay.index(struct) + len(struct)
    text = replay[start:end] + ";\n"
    for need in ("thread_local int replay_prio_prev = kPrioNone;", "replay_prio_level()", "replay_prio::raise(want, now)"):
        if need not in text:
            raise SystemExit("vk_replay.cpp ReplayPin: %r is missing (update the test with the code)" % need)
    return text


def check_source(replay, dev):
    run = block(replay, "bool run_submit(")
    if "ReplayPin pin;" not in run[:run.index("PROF(PROF_SUBMIT);")]:
        raise SystemExit("vk_replay.cpp run_submit: the replay does not take the pin before its work")
    for name in ("void submit_q(", "void present(const nvn_tex* t, const int crop[4]) {"):
        body = block(replay, name)
        sync = body[:body.index("std::lock_guard order(sync_order_mu);") + 200]
        if "ReplayPin pin;" not in sync:
            raise SystemExit("vk_replay.cpp %s: the submitting thread's replay does not take the pin" % name.split("(")[0])
    thread = block(replay, "void replay_thread_main() {")
    if "SetThreadPriority(GetCurrentThread(), replay_prio::own_thread(replay_prio_level()));" not in thread:
        raise SystemExit("vk_replay.cpp replay_thread_main: the replay thread does not take the replay's level")
    hook = replay[replay.index("const bool rec_thread_hook ="):]
    hook = hook[:hook.index("}, true);")]
    if "SetThreadPriority(GetCurrentThread(), replay_prio::own_thread(replay_prio_level()));" not in hook:
        raise SystemExit("vk_replay.cpp rec_thread_hook: the recorder thread does not take the replay's level")
    level = block(replay, "int replay_prio_level() {")
    if 'replay_prio::level(getenv("RX_VK_REPLAY_PRIO"))' not in level or "static const int v" not in level:
        raise SystemExit("vk_replay.cpp replay_prio_level: not the header's rule over RX_VK_REPLAY_PRIO, read once")
    if '#include "replay_prio.h"' not in replay:
        raise SystemExit("vk_replay.cpp does not include replay_prio.h")
    if "| `RX_VK_REPLAY_PRIO=0` or `1` |" not in dev:
        raise SystemExit("docs/development.md: RX_VK_REPLAY_PRIO is not in the switches table")


HEADER_TEST = r'''
#include <cstdio>
#include <cstdlib>
#include "replay_prio.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::replay_prio;
int main() {
    CHECK(level(nullptr) == kHighest && level("") == kHighest);   // (unset: the highest, as NX2navi's default)
    CHECK(level("2") == kHighest && level("7") == kHighest);
    CHECK(level("1") == kAbove);
    CHECK(level("0") == 0 && level("-1") == 0 && level("no") == 0);   // (as before)
    CHECK(raise(kHighest, 0) && raise(kHighest, kAbove) && raise(kAbove, 0) && raise(kHighest, -2));
    CHECK(!raise(kHighest, kHighest) && !raise(kAbove, kAbove) && !raise(kAbove, kHighest));   // (only ever up)
    CHECK(!raise(kHighest, 15));            // (time critical)
    CHECK(!raise(kHighest, 0x7fffffff));    // (THREAD_PRIORITY_ERROR_RETURN)
    CHECK(!raise(0, -2));                   // (off)
    CHECK(own_thread(0) == kAbove && own_thread(kAbove) == kAbove && own_thread(kHighest) == kHighest);
    std::printf("PASS: the rule over RX_VK_REPLAY_PRIO\n");
}
'''

PIN_TEST = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "replay_prio.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
typedef uintptr_t DWORD_PTR;
typedef void* HANDLE;
namespace mkwvk {
// (Windows' calls, standing in: one thread, its priority and affinity)
int prio = 0, sets = 0, aff_sets = 0;
bool refuse = false;
uint64_t mask = 0;
int level_now = 2;
bool async = false;
DWORD_PTR affinity = 0xffff;
HANDLE GetCurrentThread() { return (HANDLE)1; }
int GetThreadPriority(HANDLE) { return prio; }
int SetThreadPriority(HANDLE, int p) { sets++; if (refuse) return 0; prio = p; return 1; }
DWORD_PTR SetThreadAffinityMask(HANDLE, DWORD_PTR m) { aff_sets++; DWORD_PTR was = affinity; affinity = m; return was; }
bool async_on() { return async; }
uint64_t replay_pin_mask() { return mask; }
int replay_prio_level() { return level_now; }
/* PIN */
} // namespace mkwvk
using namespace mkwvk;
void reset(int p, int level, bool a = false) { prio = p, sets = 0, aff_sets = 0, refuse = false, level_now = level, async = a; }
int main() {
    reset(0, 2);   // (normal, the default level: raised once, nested pins change nothing, its own back after the last)
    {
        ReplayPin outer;
        CHECK(prio == 2 && sets == 1);
        {
            ReplayPin inner;
            CHECK(prio == 2 && sets == 1);
        }
        CHECK(prio == 2 && sets == 1);   // (still replaying: the outer one holds it)
    }
    CHECK(prio == 0 && sets == 2);
    reset(-2, 1);   // (below normal, the switch at 1: above normal while it replays, below normal again after)
    { ReplayPin p; CHECK(prio == 1); }
    CHECK(prio == -2 && sets == 2);
    reset(2, 2);   // (as high already: left alone, nothing given back)
    { ReplayPin p; CHECK(prio == 2); }
    CHECK(prio == 2 && sets == 0);
    reset(15, 2);   // (time critical: left alone)
    { ReplayPin p; }
    CHECK(prio == 15 && sets == 0);
    reset(0x7fffffff, 2);   // (Windows could not tell: left alone)
    { ReplayPin p; }
    CHECK(prio == 0x7fffffff && sets == 0);
    reset(0, 2), refuse = true;   // (Windows refused the raise: nothing to give back)
    { ReplayPin p; }
    CHECK(prio == 0 && sets == 1);
    reset(0, 0);   // (RX_VK_REPLAY_PRIO=0: as before)
    { ReplayPin p; CHECK(prio == 0); }
    CHECK(sets == 0);
    reset(0, 2, true);   // (RX_VK_ASYNC=1: the replay thread has its own level; the pin does nothing)
    { ReplayPin p; CHECK(prio == 0); }
    CHECK(sets == 0 && aff_sets == 0);
    reset(0, 2), mask = 0xc000, affinity = 0xffff;   // (the affinity pin as before, beside it)
    {
        ReplayPin p;
        CHECK(affinity == 0xc000 && prio == 2);
        { ReplayPin q; CHECK(aff_sets == 1); }
    }
    CHECK(affinity == 0xffff && prio == 0 && aff_sets == 2);
    reset(0, 2), mask = 0;
    for (int i = 0; i < 3; i++) { ReplayPin p; }   // (pin after pin: raised and given back each time)
    CHECK(prio == 0 && sets == 6);
    std::printf("PASS: ReplayPin raises a replaying thread once and gives it its own priority back\n");
}
'''

# (each: what is taken out, the text in the cut ReplayPin, what replaces it)
MUTATIONS = [
    ("the priority given back", "if (replay_prio_prev != kPrioNone) SetThreadPriority(GetCurrentThread(), replay_prio_prev), replay_prio_prev = kPrioNone;", ""),
    ("only the outermost pin", "if (replay_pin_depth++) return;", "replay_pin_depth++;"),
    ("only ever up", "replay_prio::raise(want, now) &&", "want > 0 &&"),
    ("nothing given back for a refused raise", "&& SetThreadPriority(GetCurrentThread(), want)) replay_prio_prev = now;",
     ") SetThreadPriority(GetCurrentThread(), want), replay_prio_prev = now;"),
    ("the switch's 0", "if (const int want = replay_prio_level()) {", "if (const int want = 2) {"),
]


def build_run(cxx, d, name, src, expect_ok=True):
    path = d / (name + ".cpp")
    path.write_text(src, encoding="utf-8")
    exe = d / (name + ".exe")
    r = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I" + str(VK),
                        str(path), "-o", str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise SystemExit("%s: the build failed:\n%s" % (name, r.stdout))
    r = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
    if expect_ok and r.returncode:
        raise SystemExit("%s failed:\n%s" % (name, r.stdout))
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = ap.parse_args()
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    check_source(replay, (ROOT / "docs" / "development.md").read_text(encoding="utf-8"))
    print("PASS sources: the replays and the present take the pin; the async replay thread and the recorder take the level")
    pin = cut_pin(replay)
    with tempfile.TemporaryDirectory(prefix="replay-prio-") as tmp:
        d = Path(tmp)
        print(build_run(args.cxx, d, "header", HEADER_TEST).stdout.strip())
        print(build_run(args.cxx, d, "pin", PIN_TEST.replace("/* PIN */", pin)).stdout.strip())
        for i, (what, old, new) in enumerate(MUTATIONS):
            if old not in pin:
                raise SystemExit("mutation %r: %r is not in the cut ReplayPin (update the test with the code)" % (what, old))
            r = build_run(args.cxx, d, "mutant%d" % i, PIN_TEST.replace("/* PIN */", pin.replace(old, new, 1)), expect_ok=False)
            if r.returncode == 0:
                raise SystemExit("mutation %r: the checks still passed without it" % what)
            print("PASS mutation: without %s a check fails (%s)" % (what, r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "exit %d" % r.returncode))
    print("ALL PASS")


if __name__ == "__main__":
    main()
