"""The replay's barriers where the game left its work unordered (gpu/vk/barrier_rules.h; no game, no GPU).

RX_VK_SPLIT_BARRIER: a pass open at a part's split goes on in the next part behind a barrier of its attachments alone.
RX_VK_EVENT_BARRIER: an event signal's word is written right after the next full barrier on its command buffer, and a wait met for
it while it is held gets a full barrier. RX_VK_DISPATCH_BARRIER=game (off unless set): no barrier for a dispatch right after another.
RX_VK_BATCH=parts (off unless set): whole submissions held while a game's submit is replayed, parts at once.

Compiles the header's rules over the switches' values and vendors, and the held submits' order. Then the model
(tests/barrier_rules_test.cpp): the production barrier functions cut out of vk_texture.cpp (full_barrier, the pass starts, the pass
begun again after a split, the held event writes) with a recording vkCmdPipelineBarrier2, driven the way vk_replay.cpp and
vk_draw.cpp drive them, over random and named streams of the game's ops; a checker walks every pair of accesses to one resource
of which one writes and fails where the game's ordering asks for an order the recorded barriers do not give (the rules R1 to R8 in
the model's head). In every configuration: NVIDIA's defaults (the rules on), AMD's (as before), the rules forced off and on, the
dispatch rule. With parts, every pair that both the stream replayed whole and its parts the old way order is ordered across the
parts too, and the rules record fewer full barriers than the old way on the same streams. Then each rule is taken away in turn (a
mutation of the header, of the cut functions or of the model's flow) and a named stream must then leave a hazard open: each rule
is needed. Reads the source for what the model copies: maybe_split's steps and their order, the signal's and the wait's, the pass
begun again, the dispatch's barrier, the held submits, the switches in the development guide.

(Triforce, P4) The rules' counts and marks are each command buffer's own (gpu/vk/cmd_work.h, compiled as it is into the model):
the model's recordings use a few command buffers again and again, as the replay's slots do, and another submitter's work is
recorded on a thread of its own. Then a parallel replay (the model's threads mode): the replay thread and a helper record a
segment each at once, op by op in turn, and each gets the barriers it gets alone (the rules act in each command buffer only), no
hazard is left open in either or across the two; with RX_VK_BARRIER_CONTEXT=0 the rules stand aside beside the helper, as before.
Two threads taking turns at one command buffer count both threads' commands in it. Mutations of cmd_work.h too: a begin that
keeps the marks, what crosses command buffers without the queue's submits, the count the thread's own instead of the command
buffer's; and of the model: no full barrier at a segment's edges.

    python -I -B tests/test_barrier_rules.py [--cxx g++] --headers <Vulkan headers' include folder>
"""
import argparse
import os
import re
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


def in_order(text, steps, what):
    at = -1
    for s in steps:
        k = text.find(s, at + 1)
        if k < 0:
            raise SystemExit("%s: %r is missing or out of its order" % (what, s))
        at = k


def check_source():
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    texture = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    split = block(replay, "void maybe_split(VkCommandBuffer& cmd) {")
    in_order(split, ["barrier_rules::split_in_pass(", "const bool open = in_pass && pass_carry_take();", "end_pass(cmd);",
                     "const bool held = event_writes_held(cmd);",
                     'event_writes_flush(cmd, "barrier (events\' held writes, before the recording\'s end)");',
                     "const uint64_t ended = work_between();", "flush_ub_updates(cmd);", "texture_end_recording(cmd);",
                     "memory_end_submission(cmd);", "const bool quiet = work_between() == ended;",
                     "barrier_rules::split_carry(in_pass, open, quiet, held);",
                     'if (!carry) full_barrier(cmd, "barrier (recording end)");', "submit_recording(*rec_cur, false, 0, true);",
                     "const uint64_t sent = work_between();", "Recording& r = begin_recording(rec_cur_value);",
                     "memory_begin_submission();", "barrier_rules::split_start_full(carry, work_between() != sent)",
                     'full_barrier(r.cmd, "barrier (recording start)");', "cmd = r.cmd;", "pass_carry_resume(cmd, carry);"],
             "vk_replay.cpp maybe_split")
    for name in ("void finish_recording(uint64_t value) {", "void bench_replay("):
        end = block(replay, name)
        in_order(end, ["end_pass(r.cmd);", 'event_writes_flush(r.cmd, "barrier (events\' held writes, before the recording\'s end)");',
                       "flush_ub_updates(r.cmd);", "memory_end_submission(r.cmd);"], "vk_replay.cpp " + name.split("(")[0])
    sig = replay[replay.index("        case NOP_SIGNAL_EVENT: {"):]
    sig = sig[:sig.index("        case NOP_WAIT_EVENT:")]
    in_order(sig, ["if (dry) break;", "held_write(e.gpu)", "if (held) old = held->v;", "(!held_n || event_writes_held(cmd))",
                   "barrier_rules::event_hold(event_late_on(), par_active.load(", "memory_bind_event_write(true);",
                   "word = gpu_buffer_bind(e.gpu, 4, true);", "memory_bind_event_write(false);",
                   "held_writes[held_n++] = {e.gpu, v, word.buffer, word.offset};", "event_writes_hold(cmd);", "guest_cpu_write(e.gpu, &v, 4);",
                   "} else {", "end_pass(cmd);", "full_barrier(cmd);", "gpu_write(cmd, e.gpu, &v, 4);"], "vk_replay.cpp NOP_SIGNAL_EVENT")
    wait = replay[replay.index("        case NOP_WAIT_EVENT:"):]
    wait = wait[:wait.index("        case NOP_REPORT_COUNTER:")]
    in_order(wait, ["if (worker) break;", "if (event_wait_met(have, w.mode, w.value)) {",
                    "barrier_rules::wait_barrier(held_n && held_write(w.gpu) && event_writes_held(cmd))", "end_pass(cmd);",
                    'full_barrier(cmd, "barrier (a wait whose signal\'s write was held)");', "} else if (suspend_ok) {"],
             "vk_replay.cpp NOP_WAIT_EVENT")
    if "if (!dry && barrier_rules::wait_barrier(" not in wait:
        raise SystemExit("vk_replay.cpp NOP_WAIT_EVENT: the wait's barrier is recorded in the parallel replay's dry pass")
    record = block(replay, "void event_writes_record(VkCommandBuffer cmd) {")
    in_order(record, ["if (i) {", "b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;",
                      "b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;", "vkCmdPipelineBarrier2(cmd, &d);",
                      "vkCmdUpdateBuffer(cmd, held_writes[i].buffer, held_writes[i].offset, 4, &held_writes[i].v);", "held_n = 0;"],
             "vk_replay.cpp event_writes_record")
    for lock in ("gpu_write(", "gpu_buffer_bind(", "lock_guard", "unique_lock", "memory_bind_event_write("):
        if lock in record:
            raise SystemExit("vk_replay.cpp event_writes_record binds or locks (%s): full_barrier may run it under the pools' lock" % lock)
    submit = block(replay, "void submit_recording(Recording& r, bool signal_value = true, uint64_t signal_at = 0, bool part = false) {")
    in_order(submit, ["if (event_writes_held(r.cmd)) {", "end_pass(r.cmd), full_barrier(r.cmd,", "draw_flush_recording(r.slot);",
                      "if (batch_on()) {", "barrier_rules::batch_hold(way, !signal_value, submit_depth > 0);",
                      "if (hold || !pend.cbs.empty()) {", "pend.recs++;", "barrier_rules::batch_flush(way, hold,", "return;",
                      "vkQueueSubmit2(dev.queue, 1, &si, r.fence);"], "vk_replay.cpp submit_recording")
    job = block(replay, "void submit_job(std::vector<uint64_t>& handles, uint64_t value, uint64_t queue) {")
    in_order(job, ["submit_depth++;", "if (!--submit_depth && batch_way() == barrier_rules::Batch::Parts) queue_flush_for(FR_OTHER);",
                   "run_submit("], "vk_replay.cpp submit_job")
    flush = block(replay, "void queue_flush_locked() {")
    if "pend.recs = 0;" not in flush:
        raise SystemExit("vk_replay.cpp queue_flush_locked: the held recordings' count is not cleared with them")
    begin = block(draw, "void begin_pass(")
    in_order(begin, ["end_pass(cmd);", "if (pass_resuming) pass_resume_barrier(cmd);", "else pass_start_barrier(cmd);",
                     "work_since_barrier = true;", "pass_at = at;", "pass().sampled.clear(), pass().sampled_targets = 0, pass().written = 0;",
                     "vkCmdBeginRendering(cmd, &ri);"], "vk_draw.cpp begin_pass")
    take = block(draw, "bool pass_carry_take() {")
    if "pass_carry_on = pass().open && !par_active.load(std::memory_order_relaxed);" not in take or \
            "pass_carry() = pass(), pass_carry_at = pass_at;" not in take:
        raise SystemExit("vk_draw.cpp pass_carry_take does not keep the open pass and its targets")
    resume = block(draw, "void pass_carry_resume(VkCommandBuffer cmd, bool go) {")
    in_order(resume, ["if (!go) return;", "pass_resuming = true;", "begin_pass(cmd, pass_carry_at);", "pass_resuming = false;",
                      "p.sampled = k.sampled, p.sampled_targets = k.sampled_targets, p.written = k.written;"],
             "vk_draw.cpp pass_carry_resume")
    disp = block(draw, "void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {")
    in_order(disp, ["end_pass(cmd);", "barrier_rules::dispatch_game(getenv(\"RX_VK_DISPATCH_BARRIER\"))",
                    "CmdWork& cw = cmd_work(cmd);", "const bool right_after = game && work_right_after(cw, cw.dispatch_at)",
                    "barrier_rules::dispatch_barrier(work_since_barrier, no_order, game, right_after, indirect != 0))",
                    'full_barrier(cmd, "barrier (dispatch)");', "work_since_barrier = true;", "vkCmdDispatchIndirect(cmd, args.buffer, args.offset);",
                    "vkCmdDispatch(cmd, x, y, z);", "cw.dispatch_at = work_at(cw);"], "vk_draw.cpp dispatch")
    # ((Triforce, P4) the rules' count is each command buffer's own (cmd_work.h, which the model includes as it is): every command
    # vk_work.h counts goes into the state of the command buffer it names, a begin starts that state afresh, a submit is counted)
    work = (VK / "vk_work.h").read_text(encoding="utf-8")
    if '#include "cmd_work.h"' not in work:
        raise SystemExit("vk_work.h does not include cmd_work.h")
    counted = re.findall(r"#define (vkCmd\w+)\(\.\.\.\) \(::mkwvk::rec::rx_(vkCmd\w+)\(RX_WORK_ON\(__VA_ARGS__\)\)\)", work)
    if len(counted) < 27 or any(a != b for a, b in counted):
        raise SystemExit("vk_work.h: the commands that do GPU work are not each counted in their command buffer (RX_WORK_ON): %d" % len(counted))
    if "#define RX_WORK_ON(cmd, ...) ::mkwvk::work_on(cmd) __VA_OPT__(, ) __VA_ARGS__" not in work:
        raise SystemExit("vk_work.h: RX_WORK_ON does not count the first argument, the command buffer")
    if "::vkx::vkBeginCommandBuffer(RX_WORK_BEGUN(__VA_ARGS__))" not in work or "#define RX_WORK_BEGUN(cmd, ...) ::mkwvk::work_begun(cmd)," not in work:
        raise SystemExit("vk_work.h: a command buffer's begin does not start its state afresh (work_begun)")
    if "::vkx::vkQueueSubmit2(RX_WORK_SUBMIT(__VA_ARGS__))" not in work:
        raise SystemExit("vk_work.h: a queue submit is not counted (work_submitted)")
    state = (VK / "cmd_work.h").read_text(encoding="utf-8")
    begun = block(state, "inline VkCommandBuffer work_begun(VkCommandBuffer cmd) {")
    if "w.n++;" not in begun or "w.barrier_at = w.pass_end_at = w.dispatch_at = UINT64_MAX;" not in begun:
        raise SystemExit("cmd_work.h work_begun: a begun command buffer's marks are not cleared")
    note = block(state, "inline void note_work(VkCommandBuffer cmd) {")
    if "else cmd_work(cmd).n++;" not in note or "work_epoch.fetch_add(1" not in note or "work_mine++;" not in note:
        raise SystemExit("cmd_work.h note_work: a command is not counted in its command buffer (or RX_VK_BARRIER_CONTEXT=0's count)")
    dbegin = block(draw, "void draw_begin_recording(")
    if "dispatch_last_cmd" in dbegin:
        raise SystemExit("vk_draw.cpp draw_begin_recording: the dispatch rule's thread-local mark is back (cmd_work.h dispatch_at is its own)")
    tex_begin = block(texture, "void texture_begin_recording(")
    if "late_cmd" in tex_begin:
        raise SystemExit("vk_texture.cpp texture_begin_recording drops a held event write")
    barriers = texture[texture.index("std::atomic<uint64_t> work_epoch{0};"):texture.index("void texture_begin_recording(")]
    for stale in ("last_barrier_cmd", "last_pass_end_cmd", "par_active.load(std::memory_order_relaxed),"):
        if stale in barriers:
            raise SystemExit("vk_texture.cpp's barriers: %r is back (the marks are the command buffer's own, cmd_work.h)" % stale)
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    for row in ("`RX_VK_SPLIT_BARRIER=pass` or `full`", "`RX_VK_EVENT_BARRIER=late` or `full`", "`RX_VK_DISPATCH_BARRIER=game`",
                "`RX_VK_BATCH=parts`", "`RX_VK_BARRIER_CONTEXT=0`"):
        if row not in dev:
            raise SystemExit("docs/development.md: %s is not in the switches table" % row)


HEADER_TEST = r'''
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "barrier_rules.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::barrier_rules;
int main() {
    CHECK(!split_in_pass(nullptr, 0x10DE, false) && !split_in_pass("", 0x10DE, false));   // (off unless asked since 11-16)
    CHECK(!split_in_pass(nullptr, 0x1002, false) && !split_in_pass(nullptr, 0x8086, false) && !split_in_pass(nullptr, 0, false));
    CHECK(split_in_pass("pass", 0x1002, false) && !split_in_pass("full", 0x10DE, false) && !split_in_pass("1", 0x10DE, false));
    CHECK(!split_in_pass(nullptr, 0x10DE, true) && !split_in_pass("pass", 0x10DE, true));   // (RX_VK_BARRIER_EVERY_DRAW)
    CHECK(split_carry(true, true, true, false));
    CHECK(!split_carry(false, true, true, false));
    CHECK(!split_carry(true, false, true, false) && !split_carry(true, true, false, false));
    CHECK(!split_carry(true, true, true, true));
    CHECK(split_start_full(false, false) && split_start_full(true, true) && !split_start_full(true, false));
    CHECK(!event_late(nullptr, true) && !event_late("", true) && !event_late(nullptr, false));   // (off unless asked since 11-16)
    CHECK(event_late("late", false) && !event_late("full", true) && !event_late("game", true));
    CHECK(event_hold(true, false, 0, 16) && event_hold(true, false, 15, 16) && !event_hold(true, false, 16, 16));
    CHECK(!event_hold(false, false, 0, 16) && !event_hold(true, true, 0, 16));
    CHECK(wait_barrier(true) && !wait_barrier(false));
    CHECK(dispatch_game("game") && !dispatch_game(nullptr) && !dispatch_game("") && !dispatch_game("full"));
    CHECK(dispatch_barrier(true, false, false, true, false));          // (as before: after any work)
    CHECK(!dispatch_barrier(false, false, true, false, false));        // (nothing before it on the command buffer)
    CHECK(!dispatch_barrier(true, true, false, false, false));         // (RX_VK_NO_DISPATCH_ORDER)
    CHECK(!dispatch_barrier(true, false, true, true, false));          // (the game mode, right after a dispatch; the parallel replay too)
    CHECK(dispatch_barrier(true, false, true, false, false));          // (not right after one)
    CHECK(dispatch_barrier(true, false, true, true, true));            // (an indirect one)
    CHECK(batch_mode(nullptr, true) == Batch::Hold && batch_mode(nullptr, false) == Batch::Off);
    CHECK(batch_mode("parts", false) == Batch::Parts && batch_mode("parts", true) == Batch::Parts);
    CHECK(batch_mode("1", false) == Batch::Hold && batch_mode("0", true) == Batch::Off && batch_mode("", true) == Batch::Off);
    CHECK(batch_hold(Batch::Hold, true, false) && batch_hold(Batch::Hold, false, false));
    CHECK(!batch_hold(Batch::Off, false, true));
    CHECK(batch_hold(Batch::Parts, false, true) && !batch_hold(Batch::Parts, true, true) && !batch_hold(Batch::Parts, false, false));
    CHECK(batch_flush(Batch::Off, false, 0, 32));
    CHECK(!batch_flush(Batch::Hold, true, 31, 32) && batch_flush(Batch::Hold, true, 32, 32));
    CHECK(batch_flush(Batch::Parts, false, 1, 4) && !batch_flush(Batch::Parts, true, 3, 4) && batch_flush(Batch::Parts, true, 4, 4));
    // (submit_recording's way with RX_VK_BATCH=parts, over a game's submits: the command buffers reach the queue in the order they
    // were submitted, a part never waits, at most 4 wait, and none is left once the submit's replay returns)
    struct Sub { int id; bool part; };
    const std::vector<std::vector<Sub>> jobs = {{{1, false}, {2, false}, {3, true}, {4, false}},
                                                {{5, false}, {6, false}, {7, false}, {8, false}, {9, false}, {10, true}},
                                                {{11, true}, {12, false}}, {{13, false}}};
    std::vector<int> pend, queue;
    int calls = 0;
    for (const auto& job : jobs) {
        for (const Sub& s : job) {
            const bool hold = batch_hold(Batch::Parts, s.part, true);
            if (hold || !pend.empty()) {
                pend.push_back(s.id);
                if (batch_flush(Batch::Parts, hold, pend.size(), 4)) queue.insert(queue.end(), pend.begin(), pend.end()), pend.clear(), calls++;
            } else {
                queue.push_back(s.id), calls++;
            }
            if (s.part) CHECK(pend.empty());
            CHECK(pend.size() < 4);
        }
        if (!pend.empty()) queue.insert(queue.end(), pend.begin(), pend.end()), pend.clear(), calls++;   // (submit_job's return)
    }
    for (int i = 0; i < (int)queue.size(); i++) CHECK(queue[i] == i + 1);
    CHECK(calls == 7);   // (13 submits in 7 queue calls)
    std::printf("PASS: barrier_rules.h's switches and rules; RX_VK_BATCH=parts keeps the order (13 submits, %d queue calls)\n", calls);
}
'''

# (the switches, the vendor) -> the configurations the model runs in
CONFIGS = [
    ({}, "10de", "NVIDIA's defaults: the split and event rules off (since 11-16, the lab)"),
    ({"RX_VK_SPLIT_BARRIER": "pass", "RX_VK_EVENT_BARRIER": "late"}, "10de", "the rules asked for on NVIDIA"),
    ({}, "1002", "AMD's defaults: as before"),
    ({"RX_VK_SPLIT_BARRIER": "full", "RX_VK_EVENT_BARRIER": "full"}, "10de", "the rules off on NVIDIA"),
    ({"RX_VK_SPLIT_BARRIER": "pass", "RX_VK_EVENT_BARRIER": "late"}, "1002", "the rules on with full pass barriers"),
    ({"RX_VK_PASS_BARRIER": "full"}, "10de", "the pass barriers full: the event rule follows them off"),
    ({"RX_VK_DISPATCH_BARRIER": "game"}, "10de", "the dispatch rule on"),
]
NAMED = ["carry", "carry-sample", "carry-writeback", "carry-between", "carry-signal", "dispatch-chain", "wait", "rewrite", "two-signals",
         "signal-again", "signal-writeback", "signal", "dispatch-indirect", "dispatch-draw", "hazard-split"]

FULL_BARRIER_END = "    vkCmdPipelineBarrier2(cmd, &d);\n    w.barrier_at = work_at(w);\n    if (late) late_writes_after(cmd);\n"
# ((Triforce, P4) the parallel replay: (the switches, the vendor) the threads mode runs in. Not with the event rule asked for: it stands
# aside beside the helper threads by design (event_hold), so a segment's barriers there differ from its own alone)
THREAD_CONFIGS = [({}, "10de"), ({"RX_VK_DISPATCH_BARRIER": "game"}, "10de"), ({}, "1002"), ({"RX_VK_PASS_BARRIER": "game"}, "1002"),
                  ({"RX_VK_BARRIER_CONTEXT": "0"}, "10de"), ({"RX_VK_BARRIER_CONTEXT": "0", "RX_VK_DISPATCH_BARRIER": "game"}, "10de")]
THREAD_CASES = ["1", "2", "3", "carry", "dispatch-chain", "hazard-split", "dispatch-draw", "shared"]
# (name, where, old text, new text, extra compile flag, env, vendor, part size (or "threads"), stream, extra arguments): each rule
# taken away; the stream must then leave a hazard open
MUTATIONS = [
    ("the split goes on in the pass with work recorded at the part's end", "header",
     "return on && pass_open && quiet && !writes_held;", "return on && pass_open && !writes_held;", None, {},
     "10de", 4, "carry-writeback"),
    ("the split goes on with no pass open", "header",
     "return on && pass_open && quiet && !writes_held;", "return on && quiet && !writes_held;", None, {},
     "10de", 2, "dispatch-chain"),
    ("no full barrier at the next part's start after another submit between", "header",
     "return !carry || recorded_between;", "return !carry;", None, {}, "10de", 4, "carry-between"),
    ("the pass begun again with no barrier", "functions",
     "    st_pass_resumed.fetch_add(1, std::memory_order_relaxed);\n", "    return;\n", None, {}, "10de", 4, "carry"),
    ("the pass begun again with the colour's barrier alone", "functions",
     "    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;\n",
     "    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;\n", None, {}, "10de", 4, "carry"),
    ("the pass begun again without what its draws sampled and wrote", "model", None, None, "-DMUT_NO_CARRY_STATE", {}, "10de", 5,
     "carry-sample"),
    ("no barrier at a wait for a held signal", "model", None, None, "-DMUT_NO_WAIT_BARRIER", {}, "10de", 0, "wait"),
    ("the held writes of one barrier in no order", "model", None, None, "-DMUT_EVENT_WRITES_UNORDERED", {}, "10de", 0, "two-signals"),
    ("no write of the held events before a recording's end", "model", None, None, "-DMUT_NO_END_FLUSH", {}, "10de", 0, "signal"),
    ("the held writes taken for no work by the next barrier's look", "functions",
     "    event_writes_record(cmd);\n}",
     "    event_writes_record(cmd);\n    CmdWork& m = cmd_work(cmd);\n    m.barrier_at = work_at(m);\n}",
     None, {}, "10de", 0, "signal-writeback"),
    ("the held writes recorded before their barrier", "functions", FULL_BARRIER_END,
     "    if (late) late_writes_after(cmd);\n    vkCmdPipelineBarrier2(cmd, &d);\n    w.barrier_at = work_at(w);\n", None, {}, "10de", 0,
     "signal"),
    ("the dispatch rule leaves out an indirect dispatch's barrier", "header",
     "return !(game && right_after && !indirect);", "return !(game && right_after);", None,
     {"RX_VK_DISPATCH_BARRIER": "game"}, "10de", 0, "dispatch-indirect"),
    ("the dispatch rule leaves out the barrier after a draw", "header",
     "return !(game && right_after && !indirect);", "return !(game && !indirect);", None,
     {"RX_VK_DISPATCH_BARRIER": "game"}, "10de", 0, "dispatch-draw"),
    # ((Triforce, P4) the state per command buffer)
    ("a pass the replay split for a hazard starts with the fragment-stage barrier", "functions",
     "    cmd_work(cmd).pass_end_at = UINT64_MAX;   // ((Triforce) the next pass start is a full barrier: pass_barrier.h)\n",
     "    (void)cmd;\n", None, {}, "10de", 0, "hazard-split"),
    ("a begin keeps the command buffer's marks (its handle used again)", "cmd_work",
     "    w.n++;\n    w.barrier_at = w.pass_end_at = w.dispatch_at = UINT64_MAX;\n", "    (void)w;\n", None,
     {"RX_VK_SPLIT_BARRIER": "full"}, "10de", 4, "carry-between", ["slots=1"]),
    ("what crosses command buffers leaves out the queue's submits (another thread's work between two parts)", "cmd_work",
     "work_mine + work_submits.load(std::memory_order_relaxed)", "work_mine", None, {}, "10de", 4, "carry-between"),
    ("the count the thread's own, not the command buffer's (package D's way, for every rule)", "cmd_work",
     "return work_global() ? work_epoch.load(std::memory_order_relaxed) : w.n;",
     "return work_global() ? work_epoch.load(std::memory_order_relaxed) : work_mine + 0 * w.n;", None, {}, "10de", "threads",
     "shared"),
    ("no full barrier at a segment's start and end", "model", None, None, "-DMUT_NO_SEGMENT_EDGES", {}, "10de", "threads",
     "dispatch-chain"),
]


def clean_env(extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_")}
    env.update(extra)
    return env


def build(cxx, headers, tmp, name, header_text, functions, flag=None, state_text=None):
    d = tmp / name
    d.mkdir()
    (d / "barrier_rules.h").write_text(header_text, encoding="utf-8")
    # ((Triforce, P4) cmd_work.h beside it, ahead of gpu/vk's: a mutation may change it)
    (d / "cmd_work.h").write_text(state_text or (VK / "cmd_work.h").read_text(encoding="utf-8"), encoding="utf-8")
    fixture = (ROOT / "tests" / "barrier_rules_test.cpp").read_text(encoding="utf-8").replace("/* FUNCTIONS */", functions)
    cpp, exe = d / "model.cpp", d / "model.exe"
    cpp.write_text(fixture, encoding="utf-8")
    subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", "-Wno-unused-function",
                    "-Wno-unused-parameter",   # (a mutation's rule may leave one of its arguments unread)
                    "-I" + str(d), "-I" + str(VK), "-I" + str(headers), str(cpp), "-o", str(exe)] + ([flag] if flag else []), check=True)
    return exe


def run(exe, env, args, expect_ok=True):
    p = subprocess.run([str(exe)] + [str(a) for a in args], env=clean_env(env), capture_output=True, text=True, timeout=120)
    if expect_ok and p.returncode:
        raise SystemExit("the model %s with %s: %s%s" % (" ".join(map(str, args)), env, p.stdout, p.stderr))
    return p


def full_count(out):
    # "PASS: ... N full barriers and M others, ..."
    return int(out.split(" full barriers")[0].rsplit(" ", 1)[1])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--headers", type=Path, required=True)
    a = p.parse_args()
    check_source()
    texture = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    start = texture.index("std::atomic<uint64_t> work_epoch{0};")
    functions = texture[start:texture.index("void texture_begin_recording(", start)]
    for name in ("void full_barrier(", "void note_pass_end(", "void note_pass_split(", "void pass_start_barrier(",
                 "void pass_resume_barrier(", "void event_writes_hold(", "bool event_writes_held(", "void event_writes_flush(",
                 "void late_writes_after("):
        if name not in functions:
            raise SystemExit("vk_texture.cpp: %s is not beside the barriers any more" % name.rstrip("("))
    header = (VK / "barrier_rules.h").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="barrier-rules-") as tmp:
        tmp = Path(tmp)
        cpp, exe = tmp / "header.cpp", tmp / "header.exe"
        cpp.write_text(HEADER_TEST, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(VK), str(cpp), "-o", str(exe)], check=True)
        print(subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.strip())
        model = build(a.cxx, a.headers, tmp, "model", header, functions)
        counts = {}
        for env, vendor, what in CONFIGS:
            for every in (0, 5, 13, 120):
                for seed in (1, 2, 3):
                    out = run(model, env, [vendor, every, seed] + (["compare"] if every else [])).stdout
                    counts[(tuple(sorted(env.items())), vendor, every, seed)] = full_count(out)
                for name in NAMED:
                    run(model, env, [vendor, every, name] + (["compare"] if every else []))
            print("PASS: %s: no hazard left open, and with parts every pair the whole stream and the old parts order" % what)
        # (fewer full barriers with the rules than without, on the same streams and parts)
        for every in (5, 13, 120):
            new = sum(counts[((("RX_VK_EVENT_BARRIER", "late"), ("RX_VK_SPLIT_BARRIER", "pass")), "10de", every, s)] for s in (1, 2, 3))
            old = sum(counts[((("RX_VK_EVENT_BARRIER", "full"), ("RX_VK_SPLIT_BARRIER", "full")), "10de", every, s)] for s in (1, 2, 3))
            if not new < old:
                raise SystemExit("parts of %d: %d full barriers with the rules, %d without" % (every, new, old))
            print("PASS: parts of %d: %d full barriers with the rules, %d without" % (every, new, old))
        # ((Triforce, P4) the model's recordings on one pair of command buffers used again and again: the same as on fresh ones)
        for env, vendor, what in CONFIGS:
            for every in (5, 13):
                for seed in (1, 2):
                    out = run(model, env, [vendor, every, seed, "compare", "slots=1"]).stdout
                    if full_count(out) != counts[(tuple(sorted(env.items())), vendor, every, seed)]:
                        raise SystemExit("%s, parts of %d, seed %d: one pair of command buffers used again gives other barriers than "
                                         "several: %s" % (what, every, seed, out))
        print("PASS: the recordings' command buffers used again start afresh: the same barriers as on several")
        # ((Triforce, P4) the parallel replay: a helper's segment and the replay thread's at once)
        for env, vendor in THREAD_CONFIGS:
            for case in THREAD_CASES:
                print(run(model, env, [vendor, "threads", case]).stdout.strip())
        for k, (what, where, old, new, flag, env, vendor, every, stream, *extra) in enumerate(MUTATIONS):
            extra = extra[0] if extra else []
            env = dict({"RX_VK_SPLIT_BARRIER": "pass", "RX_VK_EVENT_BARRIER": "late"}, **env)   # (the rules off by default since 11-16: asked for here)
            if every == "threads":   # (the threads mode with the event rule off, as THREAD_CONFIGS)
                env.pop("RX_VK_EVENT_BARRIER")
            h, f, s = header, functions, (VK / "cmd_work.h").read_text(encoding="utf-8")
            if where == "header":
                if h.count(old) != 1:
                    raise SystemExit("mutation %r: its text is not once in barrier_rules.h" % what)
                h = h.replace(old, new)
            elif where == "functions":
                if f.count(old) != 1:
                    raise SystemExit("mutation %r: its text is not once in vk_texture.cpp's barrier functions" % what)
                f = f.replace(old, new)
            elif where == "cmd_work":
                if s.count(old) != 1:
                    raise SystemExit("mutation %r: its text is not once in cmd_work.h" % what)
                s = s.replace(old, new)
            exe = build(a.cxx, a.headers, tmp, "m%d" % k, h, f, flag, s)
            run(model, env, [vendor, every, stream] + extra)   # (the stream passes with the rule)
            res = run(exe, env, [vendor, every, stream] + extra, expect_ok=False)
            if res.returncode == 0 or "FAIL" not in res.stderr:
                raise SystemExit("mutation %r: stream %s left no hazard open without the rule: %s%s" % (what, stream, res.stdout, res.stderr))
            print("PASS: without the rule (%s), stream %s leaves a hazard open: %s" % (what, stream, res.stderr.strip().splitlines()[0][:160]))
    print("PASS: the replay's barriers where the game left its work unordered")


if __name__ == "__main__":
    main()
