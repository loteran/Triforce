"""The replay thread's rules (gpu/vk/async_rules.h, RX_VK_ASYNC=1; no game, no GPU).

With the replay on a thread of its own a signal of the CPU's takes its capture on the game's thread only when a submission handed
over before it may still wait for that event; a present forces nothing but what was already stopped at the present before; the
submit's capture shares its copies with the copy helpers and has them made before the hand-over; the game's submit keeps its
virtual core; the present poll waits for the replay thread asleep. Compiles and runs the header's rules (the notes of the
submissions handed over: told and not told, done, the bound; the capture's rule; the present's drain from its switch; the look
before the thread sleeps). Each rule of the header taken out in turn (a mutation) must make the header's test fail. Then reads
the sources for where the rules are used: the notes made at the hand-over and taken away where a submission is replayed whole, the
capture's rule at the signal, the copies done before the hand-over, the host's wait ops told before its submit, the two calls
found by name, and the switches' row in the development guide.

    python -I -B tests/test_async_rules.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
HOST = ROOT / "host"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def in_order(text, parts, where):
    at = 0
    for part in parts:
        found = text.find(part, at)
        if found < 0:
            raise SystemExit("%s: %r is missing, or not after the part before it (update the test with the code)" % (where, part))
        at = found + len(part)


HEADER_TEST = r'''
#include <cstdio>
#include <cstdlib>
#include "async_rules.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::async_rules;
int main() {
    // (the switches that are on unless 0)
    CHECK(on_unless_zero(nullptr) && on_unless_zero("") && on_unless_zero("1") && on_unless_zero("2") && !on_unless_zero("0"));
    const uint64_t A = 0x1000, B = 0x2000, C = 0x3000;
    {   // (told its waits: noted for those events only, until it is replayed whole)
        Notes n;
        const uint64_t ev[] = {A, B, A};
        n.add(7, true, ev, 3);
        CHECK(n.pending.size() == 1 && n.pending[0].events.size() == 2);   // (A once)
        CHECK(n.waits_for(A) && n.waits_for(B) && !n.waits_for(C));
        CHECK(capture_at_signal(true, true, n, A) && !capture_at_signal(true, true, n, C));
        CHECK(!capture_at_signal(false, true, n, A));        // (the thread has nothing: the next submit's capture has it all)
        CHECK(capture_at_signal(true, false, n, C));         // (the notes off: every signal with the thread behind, as before)
        CHECK(!capture_at_signal(false, false, n, C));
        n.done(8);                                           // (another submission's: this one stays)
        CHECK(n.waits_for(A));
        n.done(7);
        CHECK(n.pending.empty() && !n.waits_for(A) && !n.waits_for(B));
        CHECK(!capture_at_signal(true, true, n, A));
    }
    {   // (told that it waits for nothing: not noted)
        Notes n;
        n.add(1, true, nullptr, 0);
        CHECK(n.pending.empty() && !n.waits_for(A));
    }
    {   // (not told: it may wait for any event)
        Notes n;
        n.add(3, false, nullptr, 0);
        CHECK(n.pending.size() == 1 && n.waits_for(A) && n.waits_for(C));
        n.done(3);
        CHECK(!n.waits_for(A));
    }
    {   // (two submissions wait for the same event: the second still does when the first is done)
        Notes n;
        n.add(1, true, &A, 1);
        n.add(2, true, &A, 1);
        n.add(3, true, &B, 1);
        n.done(1);
        CHECK(n.waits_for(A) && n.waits_for(B));
        n.done(2);
        CHECK(!n.waits_for(A) && n.waits_for(B));
        n.done(3);
        CHECK(n.pending.empty());
    }
    {   // (the bound: past it every signal takes its capture, until none is pending)
        Notes n;
        for (uint64_t v = 1; v <= kMaxNotes; v++) n.add(v, true, &A, 1);
        CHECK(n.pending.size() == kMaxNotes && !n.over && !n.waits_for(C));
        n.add(kMaxNotes + 1, true, &B, 1);                   // (not noted: its event must not be missed all the same)
        CHECK(n.pending.size() == kMaxNotes && n.over);
        CHECK(n.waits_for(B) && n.waits_for(C));
        for (uint64_t v = 1; v < kMaxNotes; v++) n.done(v);
        CHECK(n.over && n.waits_for(C));                     // (one is still pending)
        n.done(kMaxNotes);
        CHECK(n.over && n.pending.empty() && n.waits_for(B));   // (the one past the bound is not done: its event still counts)
        n.done(kMaxNotes + 1);
        CHECK(!n.over && n.pending.empty() && !n.waits_for(B));
    }
    // (the present's drain: the stale way unless the switch says otherwise)
    CHECK(present_drain(nullptr) == PresentDrain::Stale && present_drain("") == PresentDrain::Stale);
    CHECK(present_drain("2") == PresentDrain::Stale && present_drain("1") == PresentDrain::Force);
    CHECK(present_drain("0") == PresentDrain::Keep);
    // (the look before the thread sleeps)
    CHECK(spin_ns(nullptr) == 100000 && spin_ns("") == 100000 && spin_ns("0") == 0 && spin_ns("50") == 50000);
    CHECK(spin_ns("5000") == 1000000 && spin_ns("-3") == 0);
    std::puts("PASS header: the notes of the submissions handed over, the capture at a signal, the present's drain, the look");
    return 0;
}
'''

# (what, the header's text, its replacement): each must make the header's test fail
MUTATIONS = [
    ("a submission told to wait for nothing is left out", "        if (told && n <= 0) return;\n", ""),
    ("a submission not told may wait for any event", "        if (!told) note.events.push_back(kAnyEvent);\n        else\n",
     "        if (told)\n"),
    ("an event is noted once", "if (std::find(note.events.begin(), note.events.end(), events[i]) == note.events.end()) ", ""),
    ("the bound raises its flag", "            over = true;\n            unnoted.push_back(value);", "            unnoted.push_back(value);"),
    ("a submission past the bound is remembered", "            unnoted.push_back(value);\n", ""),
    ("a submission replayed whole leaves the notes", "                pending.erase(it);\n", ""),
    ("only the submission of that value leaves", "            if (it->value == value) {", "            if (it->value <= value) {"),
    ("the flag falls when every one past the bound is done", "        if (unnoted.empty()) over = false;\n", ""),
    ("the flag stays while one past the bound is not done", "        if (unnoted.empty()) over = false;", "        if (pending.empty()) over = false;"),
    ("past the bound every event counts", "        if (over) return true;\n", ""),
    ("any event stands for every event", "if (e == event || e == kAnyEvent) return true;", "if (e == event) return true;"),
    ("no capture when the thread has nothing", "    if (!behind) return false;\n", ""),
    ("the notes decide when they are on", "    return !notes_on || notes.waits_for(event);", "    return true;"),
    ("the stale way unless set", "    if (!env || !*env) return PresentDrain::Stale;", "    if (!env || !*env) return PresentDrain::Force;"),
    ("0 keeps, 1 forces", "    return v == 0 ? PresentDrain::Keep : v == 1 ? PresentDrain::Force : PresentDrain::Stale;",
     "    return v == 0 ? PresentDrain::Keep : PresentDrain::Stale;"),
    ("the look's bound", "std::clamp(std::atoi(env), 0, 1000)", "std::atoi(env)"),
]


def check_source(replay, native, backend, thread, dev):
    if '#include "async_rules.h"' not in replay:
        raise SystemExit("vk_replay.cpp does not include async_rules.h")
    # (the notes: made at the hand-over, under the queue's lock, with the value the hand-over gave)
    enq = block(replay, "void enqueue(Job&& j) {")
    in_order(enq, ["std::unique_lock lk(q_mu);", "j.value = ++q_values;", "if (j.kind == JOB_SUBMIT && wait_notes_on()) {",
                   "wait_notes.add(j.value, j.waits_told, j.waits.data(), (int)j.waits.size());", "q_jobs.push_back(std::move(j));",
                   "q_count.fetch_add(1"], "vk_replay.cpp enqueue")
    # (taken away only where a submission is replayed whole: run_submit returned true)
    job = block(replay, "void submit_job(std::vector<uint64_t>& handles, uint64_t value, uint64_t queue) {")
    if "if (run_submit(handles.data(), (int)handles.size(), value, queue, true, 0, nullptr, may_stop)) async_wait_done(value);" not in job:
        raise SystemExit("vk_replay.cpp submit_job: a submission replayed whole does not leave the notes, or one that stopped does")
    res = block(replay, "void resume_queue(uint64_t queue, bool forced) {")
    in_order(res, ["const uint64_t resumed = q.value;",
                   "if (!run_submit(handles.data(), (int)handles.size(), resumed, queue, false, q.cb, from, true)) return;",
                   "async_wait_done(resumed);",
                   "if (!run_submit(s.handles.data(), (int)s.handles.size(), s.value, queue, false, 0, nullptr, true)) return;",
                   "async_wait_done(s.value);"], "vk_replay.cpp resume_queue")
    if replay.count("async_wait_done(") != 5:   # (its declaration, its definition, and the three places above)
        raise SystemExit("vk_replay.cpp: async_wait_done is called in a place this test does not know")
    done = block(replay, "void async_wait_done(uint64_t value) {")
    in_order(done, ["if (!wait_notes_n.load(", "std::lock_guard lk(q_mu);", "wait_notes.done(value);", "wait_notes_n.store("],
             "vk_replay.cpp async_wait_done")
    # (the capture at a signal: the rule's, on the game's thread, and in order with the jobs)
    sig = block(replay, "void event_signal_queued(uint64_t gpu, uint32_t value) {")
    in_order(sig, ["if (capture && async_unforced()) {", "std::lock_guard order(submit_order_mu);", "const bool behind = replay_behind();",
                   "if (behind && wait_notes_on()) {", "std::lock_guard nk(q_mu);",
                   "take = async_rules::capture_at_signal(true, true, wait_notes, gpu);", "if (take) {",
                   "j.ev_capture = memory_capture_submit();", "enqueue(std::move(j));", "return;"],
             "vk_replay.cpp event_signal_queued")
    # ((Triforce, 11-20) the look at the notes under the hand-overs' lock: a submission between its capture and its enqueue is
    # seen, and the signal's job is queued in order with it; one lock_guard of it, taken before the look)
    if sig.count("(submit_order_mu)") != 1 or sig.index("std::lock_guard order(submit_order_mu);") > sig.index("const bool behind = replay_behind();"):
        raise SystemExit("vk_replay.cpp event_signal_queued: the look at the notes is not under submit_order_mu")
    # (the submit's capture: its copies shared, and all made before the hand-over)
    sub = block(replay, "void submit_q(const uint64_t* handles, int n, uint64_t queue) {")
    own = sub[sub.index("std::lock_guard lk(submit_order_mu);"):]
    in_order(own, ['async_rules::on_unless_zero(getenv("RX_VK_ASYNC_CAPTURE_BEHIND"))', "memory_capture_submit(true);",
                   "memory_capture_copies_done();", "} else {", "memory_capture_submit();", "if (tl_submit_waits_told) {",
                   "j.waits_told = true;", "tl_submit_waits_told = false;", "enqueue(std::move(j));"], "vk_replay.cpp submit_q")
    # (the present: the rule from its switch; the thread's look)
    run = block(replay, "void run_job(Job& j) {")
    in_order(run, ['async_rules::present_drain(getenv("RX_VK_PRESENT_DRAIN"))', "way == async_rules::PresentDrain::Keep",
                   "way == async_rules::PresentDrain::Stale", "drain_stale();", "drain(0);", "do_present("], "vk_replay.cpp run_job")
    main_ = block(replay, "void replay_thread_main() {")
    in_order(main_, ['async_rules::spin_ns(getenv("RX_VK_ASYNC_SPIN_US"))', "while (!q_count.load(", "std::unique_lock lk(q_mu);",
                     "q_jobs.pop_front(), have = true, q_count.fetch_sub(1"], "vk_replay.cpp replay_thread_main")
    if replay.count("q_jobs.pop_front()") != replay.count("q_count.fetch_sub(1"):
        raise SystemExit("vk_replay.cpp: a job is taken from the queue without its count")
    # (the two diagnostics for the black frames after a tour stay behind their switches)
    for name in ('getenv("RX_VK_LOG_READBACK_ZERO")', 'getenv("RX_VK_REGION_TRAP")'):
        if replay.count(name) != 1:
            raise SystemExit("vk_replay.cpp: the diagnostic %s is not read in one place" % name)
    idle = block(replay, "bool replay_idle_wait(uint64_t timeout_ns) {")
    in_order(idle, ["if (!async_on()) return true;", "std::unique_lock lk(q_mu);", "q_done.wait_for(lk,",
                    "return q_jobs.empty() && !q_busy;"], "vk_replay.cpp replay_idle_wait")
    for name in ("void mkwvk_submit_waits(const uint64_t* events, int n) {", "int mkwvk_replay_idle_wait(uint64_t timeout_ns) {"):
        if name not in replay:
            raise SystemExit("vk_replay.cpp: %s is not exported" % name.split("(")[0])
    # (the host: the wait ops of a submit's recordings, told before its hand-over; the core kept; the poll's wait)
    res_d = block(native, "static void resolve_deferred(uint8_t *p, uint64_t *waits, int *nwaits) {")
    in_order(res_d, ["if (h.op == NOP_END) return;", "if (h.op == NOP_WAIT_EVENT && waits && *nwaits >= 0) {",
                     "if (*nwaits == SUBMIT_WAITS_MAX) *nwaits = -1;", "waits[(*nwaits)++] = ((const nvn_p_wait_event *)(p + 8))->gpu;",
                     "p += h.size;"], "nvn_native.c resolve_deferred")
    q = block(native, "NV(nvnQueueSubmitCommands) {")
    in_order(q, ["const int async = rx_async_replay();", "resolve_deferred((uint8_t *)(uintptr_t)h[i], async ? waits : NULL, &nwaits);",
                 'getenv("HOS_ASYNC_KEEP_CORE")', "const int away = keep_core ? 0 : hos_vcore_away();",
                 "if (async && nwaits >= 0) rx_nvn_submit_waits(waits, nwaits);", "rx_nvn_submit_q(h, n, (uint64_t)(uintptr_t)q);",
                 "if (away) hos_vcore_back();"], "nvn_native.c nvnQueueSubmitCommands")
    for name in ('GetProcAddress(vk_module, "mkwvk_submit_waits")', 'GetProcAddress(vk_module, "mkwvk_replay_idle_wait")',
                 "int rx_async_replay(void) {", "return fn ? fn(timeout_ns) : 1;"):
        if name not in backend:
            raise SystemExit("rx_backend.c: %r is missing" % name)
    poll = block(thread, "static int present_poll(Ctx *c, int64_t ns) {")
    in_order(poll, ["async = rx_async_replay();", "on = e ? atoi(e) != 0 : async;", "const int away = hos_vcore_away();",
                    "if (async && v != 1) {", "rx_nvn_replay_idle_wait((uint64_t)ns);", "hos_nvn_event_value(ev, &v);",
                    "while (v != 1 && hos_now_ns() < end) {"], "hos_thread.c present_poll")
    for name in ("RX_VK_ASYNC_EVENT_NOTES", "RX_VK_ASYNC_CAPTURE_BEHIND", "HOS_ASYNC_KEEP_CORE", "HOS_PRESENT_POLL", "RX_VK_PRESENT_DRAIN",
                 "RX_VK_ASYNC_SPIN_US", "RX_VK_LOG_READBACK_ZERO", "RX_VK_REGION_TRAP"):
        if name not in dev:
            raise SystemExit("docs/development.md: %s is not in the switches table" % name)


def build(cxx, d, name, src, include):
    cpp, exe = d / (name + ".cpp"), d / (name + ".exe")
    cpp.write_text(src, encoding="utf-8")
    r = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I" + str(include), str(cpp),
                        "-o", str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        return None, r.stdout
    return exe, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    a = ap.parse_args()
    check_source((VK / "vk_replay.cpp").read_text(encoding="utf-8"), (HOST / "nvn_native.c").read_text(encoding="utf-8"),
                 (HOST / "rx_backend.c").read_text(encoding="utf-8"), (HOST / "hos_thread.c").read_text(encoding="utf-8"),
                 (ROOT / "docs" / "development.md").read_text(encoding="utf-8"))
    print("PASS sources: the notes at the hand-over and where a submission is replayed whole, the capture at a signal, the copies "
          "before the hand-over, the host's waits and calls, the switches' row")
    header = (VK / "async_rules.h").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="async-rules-") as tmp:
        d = Path(tmp)
        exe, out = build(a.cxx, d, "header", HEADER_TEST, VK)
        if not exe:
            raise SystemExit("header: the build failed:\n" + out)
        r = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        if r.returncode:
            raise SystemExit("the header test failed:\n" + r.stdout)
        print(r.stdout.strip())
        for i, (what, old, new) in enumerate(MUTATIONS):
            if header.count(old) != 1:
                raise SystemExit("mutation %r: its text is not once in async_rules.h (update the test with the header)" % what)
            inc = d / ("mutant%d" % i)
            inc.mkdir()
            (inc / "async_rules.h").write_text(header.replace(old, new), encoding="utf-8")
            exe, out = build(a.cxx, d, "mutant%d" % i, HEADER_TEST, inc)
            if exe:   # (a mutant that does not build has failed too: an unused name under -Werror)
                r = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
                if r.returncode == 0:
                    raise SystemExit("mutation %r: the header test still passed without it" % what)
                out = r.stdout
            last = out.strip().splitlines()[-1] if out.strip() else "it did not pass"
            print("PASS mutation: without %s the test fails (%s)" % (what, last[:120]))
    print("ALL PASS")


if __name__ == "__main__":
    main()
