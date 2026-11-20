"""A submission that begins with a wait for the CPU's signal waits for it at the submit (gpu/vk/gate_wait.h; no game, no GPU).

After NX2navi's gate_wait_ms, off unless RX_VK_GATE_WAIT_MS is set: the submit call waits (at most the bound) for the CPU's signal
of the event its first command buffer begins with a wait for, before its capture and its replay, where the replay would stop at that
wait. Compiles and runs the header's rules (the bound's switch, the look for the first wait past state only, when it goes off for the
run); then vk_replay.cpp's gate_kind and gate_wait_signal cut out of the source and run against stand-ins (the order lock, the
replay's view of the wait, the events the CPU signalled, the event's word in the game's memory and a clock, both counted by calls,
so nothing depends on this PC's speed): a signal that comes, one that does not (the bound), a draw before the wait, state before it,
a wait met already, an event the CPU never signalled, the order lock held by another thread, the switch unset, the replay on its own
thread, and the gate going off after its waits ran out. Each rule taken out in turn (a mutation of the cut code) must make a case
fail. Reads the source for where the gate is called (the submitting thread's submit, before the order lock and the capture) and the
switch's row in the development guide.

    python -I -B tests/test_gate_wait.py [--cxx g++]
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


HEADER_TEST = r'''
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "gate_wait.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::gate_wait;
enum { STATE = 1, WAIT = 2, DRAW = 3 };
std::vector<uint8_t> s;
size_t put(uint32_t op, uint32_t size = 16) { size_t at = s.size(); s.resize(at + size); uint32_t h[2] = {op, size}; std::memcpy(&s[at], h, 8); return at; }
int kind(uint32_t op) { return op == STATE ? 1 : op == WAIT ? 2 : 0; }
int main() {
    CHECK(ms(nullptr) == 0 && ms("") == 0 && ms("0") == 0 && ms("x") == 0 && ms("-5") == 0);   // (off unless set)
    CHECK(ms("10") == 10 && ms("1") == 1 && ms("100") == 100 && ms("500") == 100);
    put(STATE); put(STATE, 32); const size_t w = put(WAIT); put(DRAW);
    CHECK(first_wait(s.data(), kind) == &s[w]);              // (state, then the wait)
    s.clear();
    const size_t w0 = put(WAIT);
    CHECK(first_wait(s.data(), kind) == &s[w0]);             // (the first op)
    s.clear();
    put(STATE); put(DRAW); put(WAIT);
    CHECK(!first_wait(s.data(), kind));                      // (a draw first: no)
    s.clear();
    for (int i = 0; i < kMaxOps; i++) put(STATE);
    put(WAIT);
    CHECK(!first_wait(s.data(), kind));                      // (past kMaxOps ops)
    s.clear();
    put(STATE); const size_t bad = put(STATE); put(WAIT);
    uint32_t h[2] = {STATE, 12};
    std::memcpy(&s[bad], h, 8);
    CHECK(!first_wait(s.data(), kind));                      // (a record of a bad size)
    CHECK(!off_after(3, 3) && off_after(4, 4) && off_after(39, 4) && !off_after(40, 4) && off_after(50, 6) && !off_after(100, 9));
    std::printf("PASS: the bound, the first wait past state, off for the run\n");
}
'''

MODEL_TEST = r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include "rx_nvn.h"
#include "gate_wait.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d (%s) %s\n", __LINE__, g_case.c_str(), #x); std::exit(1); } } while (0)
std::string g_case;
namespace mkwvk {
const char* getenv(const char* n) { return !std::strcmp(n, "RX_VK_GATE_WAIT_MS") && g_case != "unset" ? "10" : nullptr; }
int logged = 0;
void logf(const char*, ...) { logged++; }
bool sync_on = true, met_now = false;
bool sync_waits_on() { return sync_on; }
std::mutex sync_order_mu, ev_cpu_mu;
std::unordered_set<uint64_t> cpu_events;
// (the event's word in the game's memory: the CPU's signal writes it after `signal_after` reads; the clock moves 50 us a look)
uint32_t word = 0, signalled = 0;
int64_t reads = 0, signal_after = -1, clock_ns = 0;
int game_read(uint64_t, void* dst, uint64_t n) {
    if (++reads > 2000000) { std::fprintf(stderr, "FAIL (%s): the gate never ended\n", g_case.c_str()); std::exit(1); }
    if (signal_after >= 0 && reads > signal_after) word = signalled;
    std::memcpy(dst, &word, (size_t)n);
    return 1;
}
struct Host { int (*gpu_read)(uint64_t, void*, uint64_t); } host_s{game_read};
const Host* host = &host_s;
bool wait_met_at(const uint8_t*) { return met_now; }
int64_t gate_clock_ns() { return clock_ns += 50000; }
/* CUT */
} // namespace mkwvk
using namespace mkwvk;
std::vector<uint8_t> st;
void rec(uint32_t op, const void* pl, uint32_t bytes) {
    const uint32_t size = (8 + bytes + 7) & ~7u;
    const size_t at = st.size();
    st.resize(at + size);
    const nvn_op_hdr h{op, size};
    std::memcpy(&st[at], &h, 8);
    if (bytes) std::memcpy(&st[at + 8], pl, bytes);
}
const uint64_t EV = 0x1006e38004ull;
void stream(bool draw_first) {
    st.clear();
    rec(NOP_BEGIN, nullptr, 0);
    nvn_p_bind_program bp{};
    rec(NOP_BIND_PROGRAM, &bp, sizeof bp);
    if (draw_first) { nvn_p_draw d{}; rec(NOP_DRAW, &d, sizeof d); }
    nvn_p_wait_event w{EV, 1, 5};   // (>= 5)
    rec(NOP_WAIT_EVENT, &w, sizeof w);
    nvn_p_draw d{};
    rec(NOP_DRAW, &d, sizeof d);
    rec(NOP_END, nullptr, 0);
}
void gate() {
    const uint64_t h = (uint64_t)(uintptr_t)st.data();
    gate_wait_signal(&h, 1);
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    g_case = argv[1];
    cpu_events.insert(EV);
    word = 4, signalled = 5;
    stream(false);
    if (g_case == "signal") {   // (the CPU signals after 100 looks: the gate ends then, met)
        signal_after = 100;
        gate();
        CHECK(reads == 101 && gate_all == 1 && gate_all_outs == 0 && clock_ns < 10000000);
    } else if (g_case == "runout") {   // (no signal: the bound, then as before)
        gate();
        CHECK(gate_all == 1 && gate_all_outs == 1 && clock_ns >= 10000000 && clock_ns < 11000000 && !gate_off);
    } else if (g_case == "draw-first") {   // (a draw before the wait: the replay would not stop at it first)
        stream(true);
        gate();
        CHECK(reads == 0 && gate_all == 0);
    } else if (g_case == "met") {   // (met in the replay's order already)
        met_now = true;
        gate();
        CHECK(reads == 0);
    } else if (g_case == "not-cpu") {   // (an event the CPU never signalled: another queue's work signals it)
        cpu_events.clear();
        gate();
        CHECK(reads == 0);
    } else if (g_case == "locked") {   // (a replay under way on another thread: as before, no wait)
        std::mutex go;
        go.lock();
        std::atomic<bool> held{false};
        std::thread t([&] { std::lock_guard l(sync_order_mu); held = true; std::lock_guard g(go); });
        while (!held) std::this_thread::yield();
        gate();
        go.unlock();
        t.join();
        CHECK(reads == 0);
    } else if (g_case == "unset") {   // (RX_VK_GATE_WAIT_MS unset: off)
        signal_after = 0;
        gate();
        CHECK(reads == 0 && gate_all == 0);
    } else if (g_case == "sync-off") {   // (RX_VK_SYNC_WAITS=0, or the replay on its own thread: no stop to spare)
        sync_on = false;
        gate();
        CHECK(reads == 0);
    } else if (g_case == "goes-off") {   // (4 that ran out, more than a tenth: off for the run, logged once)
        for (int i = 0; i < 4; i++) gate();
        CHECK(gate_off && gate_all == 4 && gate_all_outs == 4);
        const int64_t r = reads, l = logged;
        gate();
        CHECK(reads == r && logged == l && gate_all == 4);
    } else if (g_case == "mixed") {   // (signals that come keep it on: 3 of 40 ran out)
        for (int i = 0; i < 40; i++) {
            word = 4, reads = 0, signal_after = i % 13 == 0 ? -1 : 10;
            gate();
        }
        CHECK(!gate_off && gate_all == 40 && gate_all_outs == 4);
    } else {
        std::fprintf(stderr, "no case %s\n", g_case.c_str());
        return 2;
    }
    std::printf("PASS %s\n", g_case.c_str());
    return 0;
}
'''

CASES = ["signal", "runout", "draw-first", "met", "not-cpu", "locked", "unset", "sync-off", "goes-off", "mixed"]

# (each: what is taken out, the text in the cut code, what replaces it, the case that must then fail)
MUTATIONS = [
    ("the order lock's try", "if (!lk || wait_met_at(w)) return;", "if (wait_met_at(w)) return;", "locked"),
    ("the wait met already", "if (!lk || wait_met_at(w)) return;", "if (!lk) return;", "met"),
    ("an event the CPU signals", "if (!cpu_events.count(ev.gpu)) return;", "", "not-cpu"),
    ("the bound", "gate_clock_ns() - t0 >= cap_ns", "false", "runout"),
    ("state only before the wait", "    case NOP_WAIT_EVENT: return 2;", "    case NOP_WAIT_EVENT: return 2;\n    case NOP_DRAW: return 1;", "draw-first"),
    ("off for the run", "gate_wait::off_after(all, all_outs)", "false", "goes-off"),
    ("the switch", "if (!cap_ns || n < 1", "if (n < 1", "unset"),
]


def cut(replay):
    payload = replay[replay.index("template <class T>\nT payload(const uint8_t* a, uint32_t bytes) {"):]
    payload = payload[:payload.index("\n}\n") + 3]
    atomics = replay[replay.index("std::atomic<int64_t> gate_n{0}"):]
    atomics = atomics[:atomics.index("\n/// The submitting thread, before the order lock")]
    parts = [payload, block(replay, "bool event_wait_met(uint32_t have, uint32_t mode, uint32_t want) {"),
             block(replay, "nvn_p_wait_event wait_at(const uint8_t* p) {"), block(replay, "int gate_kind(uint32_t op) {"), atomics,
             block(replay, "void gate_wait_signal(const uint64_t* handles, int n) {")]
    return "\n".join(parts)


def check_source(replay, dev):
    sub = block(replay, "void submit_q(const uint64_t* handles, int n, uint64_t queue) {")
    sync = sub[:sub.index("std::lock_guard lk(submit_order_mu);")]
    at = sync.find("gate_wait_signal(handles, n);")
    if at < 0 or at > sync.index("std::lock_guard order(sync_order_mu);") or at > sync.index("memory_capture_submit(true);"):
        raise SystemExit("vk_replay.cpp submit_q: the gate is not called before the order lock and the capture on the submitting thread")
    if "gate_wait_signal(" in sub[len(sync):]:
        raise SystemExit("vk_replay.cpp submit_q: the gate is called with the replay on its own thread")
    if '#include "gate_wait.h"' not in replay:
        raise SystemExit("vk_replay.cpp does not include gate_wait.h")
    if "| `RX_VK_GATE_WAIT_MS=<1..100>` |" not in dev:
        raise SystemExit("docs/development.md: RX_VK_GATE_WAIT_MS is not in the switches table")


def build(cxx, d, name, src, extra):
    cpp, exe = d / (name + ".cpp"), d / (name + ".exe")
    cpp.write_text(src, encoding="utf-8")
    r = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-missing-field-initializers",
                        "-pthread", "-I" + str(VK), *extra, str(cpp), "-o", str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True)
    if r.returncode:
        raise SystemExit("%s: the build failed:\n%s" % (name, r.stdout))
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    a = ap.parse_args()
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    check_source(replay, (ROOT / "docs" / "development.md").read_text(encoding="utf-8"))
    print("PASS sources: the gate on the submitting thread, before the order lock and the capture")
    code = cut(replay)
    render = ["-I" + str(ROOT / "gpu" / "render")]
    with tempfile.TemporaryDirectory(prefix="gate-wait-") as tmp:
        d = Path(tmp)
        exe = build(a.cxx, d, "header", HEADER_TEST, [])
        r = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        if r.returncode:
            raise SystemExit("the header test failed:\n" + r.stdout)
        print(r.stdout.strip())
        exe = build(a.cxx, d, "model", MODEL_TEST.replace("/* CUT */", code), render)
        for case in CASES:
            r = subprocess.run([str(exe), case], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
            if r.returncode:
                raise SystemExit("model case %s failed:\n%s" % (case, r.stdout))
        print("PASS model: %d cases" % len(CASES))
        for i, (what, old, new, case) in enumerate(MUTATIONS):
            if code.count(old) != 1:
                raise SystemExit("mutation %r: its text is not once in the cut code (update the test with the code)" % what)
            exe = build(a.cxx, d, "mutant%d" % i, MODEL_TEST.replace("/* CUT */", code.replace(old, new)), render)
            r = subprocess.run([str(exe), case], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
            if r.returncode == 0:
                raise SystemExit("mutation %r: case %s still passed without it" % (what, case))
            last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "exit %d" % r.returncode
            print("PASS mutation: without %s, case %s fails (%s)" % (what, case, last))
    print("ALL PASS")


if __name__ == "__main__":
    main()
