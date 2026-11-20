"""Samples-passed reports from occlusion queries (RX_VK_OCC_QUERY=1, default off; gpu/vk/occ_answer.h, vk_replay.cpp
"occlusion queries", vk_draw.cpp).

The game resets NVN's samples-passed counter, draws an object's proxy and reports the count; a count of 0 is an object it does
not draw. The replay writes every report as it passes it with 4096 (everything visible), and still does with the switch on.
With RX_VK_OCC_QUERY=1 the draws from a reset to its report are counted by Vulkan occlusion queries, and once the GPU is done
with the recording that holds them the count is written into the report's record over the 4096 (as NX2navi writes it at
completion), only while that report is still the newest at its address (the game's records rotate), only when the count
is whole, and only over the value the replay left in the word (a compare-and-write: the game's CPU re-arms its records). The
render conditions on those words are decided as NX2navi decides them: a count still to come renders, a written one is read
from memory (a 0 skips). What must hold, without the game or a GPU:
- occ_answer.h (compiled and run, tests/occ_answer_test.cpp): the skip list, the lab knob's name, a block's sums (none when a
  query is not available), the counting from a reset to its report (a segment without its reset, a draw left out, a draw in
  another recording, the same slot begun again (by serial, not command buffer), a full block, a report twice without a reset
  and the cumulative count after it), Writers (the newest report per address in stream order; superseded, not placed,
  pruned, a count not to be written; where a count stands for a condition: pending, written, settled, too late; a report of
  another type, a small clear over records), the conditions' answers (cond_decide: pending renders, written from memory,
  the knob's off phase as before, RX_VK_OCC_COND=0 renders), complete (whole counts written by compare-and-write, 0 too;
  lost, late, superseded, unknown, fixed and a re-armed record left alone and settled; the latency; never sent settles), the
  line's numbers and the first 64 sites;
- the source (read): off unless RX_VK_OCC_QUERY or the lab knob occq (occ_init), every hook behind occ_live (or occ_pending,
  0 with it off); the report's record is still written by the replay as it passes it, in stream order, with the fixed count,
  the placeholder's write and its Writers entry under occ_mu; the dry pass places (at the dry pass's command buffer and
  draws) and measures nothing, a worker measures (at its stream place) without writing; a count is written only from a
  sealed slot's read (occ_read_slot -> occ::complete -> occ_write: a compare-and-write on the guest word, then the shadows
  told and memory_cpu_owned, the way the replay writes a report); a slot is sealed after dev.present_mu is let go, by its
  recordings' timeline number, the replay's recordings in submit_recording and a parallel replay's segments in replay_handle
  (which signal the recordings' timeline while the queries run); the done slots are
  found with one timeline read (no fence poll unless there is no timeline, and then on the replay thread only), at a game's
  fence wait that ended, a recording's begin, a present, and the slot's reuse; no read waits (no WAIT bit); a render
  condition (not a worker's) is decided by occ_cond from the answer from memory, and the draws and clears it skips are
  counted; a report of another type and a small clear rewrite the records they reach (occ_rewritten, under occ_mu with
  their write); the old guard (a condition's words kept at 4096) is gone; the replay's counting is per queue's state under
  replay_mu and a segment's the thread's own
  (none matched by command buffer handle), given up before a dry pass; a query is begun only in an open pass and ended before
  the pass ends, only in the recording it was begun in; a segment starts and ends its own counting; a slot takes a new
  serial at each begin; its block is reset on the host once read (hostQueryReset) or else as its command buffer begins;
  Writers are pruned by age regularly; the fingerprint and the per-address answer table are gone; the switches are in the
  development guide.

    python -I -B tests/test_occ_answer.py [--cxx clang++]
"""
import argparse
import re
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


def in_order(where, text, needles):
    at = [text.index(n) if n in text else -1 for n in needles]
    if -1 in at or at != sorted(at):
        raise SystemExit(where + ": not in this order: " + " / ".join(needles))


def check_source(src):
    replay = (src / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
    for gone in ("occ_table", "occ_answer(", "occ_fp(", "occ_first", "draw_key", "fingerprint", "RX_VK_OCC_QUERY_FP"):
        if gone in replay:
            raise SystemExit("vk_replay.cpp: the old answer-at-replay design is back: " + gone)
    init = block(replay, "bool occ_init() {")
    in_order("vk_replay.cpp occ_init (off unless asked for)", init,
             ['getenv("RX_VK_OCC_QUERY")', 'occ::knob_names(lab_ab::knobs[k].name, "occq")',
              "if (!occ_env_real && !knob) return false;", "qi.queryType = VK_QUERY_TYPE_OCCLUSION;",
              "dev.features.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0",
              "occ_host_reset = dev.vk12.hostQueryReset != VK_FALSE;",
              "if (occ_host_reset) vkResetQueryPool(dev.device, occ_pool, 0, qi.queryCount);", 'getenv("RX_VK_OCC_QUERY_SKIP")'])
    begin = block(replay, "Recording& begin_recording(uint64_t value) {")
    in_order("vk_replay.cpp begin_recording", begin,
             ["static const bool occ_on = (occ_live = occ_init());", "recording_wait(r);", "if (occ_live) occ_before_begin(r);",
              "vkResetCommandPool(dev.device, r.pool, 0);", "vkBeginCommandBuffer(r.cmd, &bi);", "if (occ_live) occ_slot_begin(r);"])
    seg = block(replay, "Recording& begin_segment_recording(uint64_t value) {")
    in_order("vk_replay.cpp begin_segment_recording", seg,
             ["recording_wait(r);", "if (occ_live) occ_before_begin(r);", "vkBeginCommandBuffer(r.cmd, &bi);",
              "if (occ_live) occ_slot_begin(r);"])
    if "VK_QUERY_RESULT_WAIT_BIT" in replay:
        raise SystemExit("vk_replay.cpp: an occlusion query read waits (VK_QUERY_RESULT_WAIT_BIT)")
    read = block(replay, "void occ_read_slot(int i, occ::Stats::At at) {")
    in_order("vk_replay.cpp occ_read_slot (no WAIT bit; whole counts written through occ::complete; reset on the host once read)",
             read, ["VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT", "occ::complete(sl.reps, v.data(), sl.used,",
                    "occ_writers,", "occ_write(addr, expect, count)", "sl.reps.clear();",
                    "if (occ_host_reset && sl.used) vkResetQueryPool(dev.device, occ_pool, (uint32_t)i * kOccPerSlot, sl.used), sl.used = 0;",
                    "sl.sealed = false;"])
    in_order("vk_replay.cpp occ_write (a compare-and-write of the count word over the replay's value, then as the replay "
             "writes a report: the page marked written, shadows told, kept out of write-backs)",
             block(replay, "bool occ_write(uint64_t addr, uint64_t expect, uint64_t count) {"),
             ["host->gpu_ptr(addr, &avail)", "__atomic_compare_exchange_n(reinterpret_cast<uint64_t*>(p), &expect, count,",
              "host->mark_written(p, 8);", "now == expect && host->gpu_write(addr, &count, 8);", "if (!ok) return false;",
              "guest_cpu_wrote(addr, 8);", "memory_cpu_owned(host->gpu_guest(addr), 16);", "return true;"])
    if replay.count("occ_write(") != 2:   # (its definition and occ_read_slot's call: no other count write)
        raise SystemExit("vk_replay.cpp: a count is written somewhere other than occ_read_slot")
    harvest = block(replay, "void occ_harvest_done(occ::Stats::At at) {")
    in_order("vk_replay.cpp occ_harvest_done (one timeline read under occ_mu; fences only without it, never on a game's thread)",
             harvest, ["if (!occ_pending.load(std::memory_order_acquire)) return;",
                       "if (!rec_timeline && at == occ::Stats::kAtWait) return;", "std::lock_guard lk(occ_mu);",
                       "if (rec_timeline) vkGetSemaphoreCounterValue(dev.device, rec_timeline, &done);",
                       "if (!sl.sealed || sl.reps.empty()) continue;", "is_done = sl.seq <= done;",
                       "if (at == occ::Stats::kAtWait) continue;", "vkWaitForFences(dev.device, 1, &o.fence, VK_TRUE, 0) == VK_SUCCESS",
                       "if (is_done) occ_read_slot(i, at);"])
    before = block(replay, "void occ_before_begin(Recording& r) {")
    in_order("vk_replay.cpp occ_before_begin (read if sealed, else dropped; reset either way; then the done ones)", before,
             ["std::lock_guard lk(occ_mu);", "if (sl.sealed) {", "occ_read_slot(r.slot, occ::Stats::kAtReuse);",
              "occ::dropped(sl.reps, occ_writers, occ_st);", "vkResetQueryPool(", "sl.armed = false, sl.sealed = false, sl.seq = 0;",
              "occ_harvest_done(occ::Stats::kAtBegin);"])
    in_order("vk_replay.cpp occ_seal (sealed once, by its timeline number; counted pending if it has reports)",
             block(replay, "void occ_seal(Recording& r, uint64_t seq) {"),
             ["std::lock_guard lk(occ_mu);", "if (!sl.armed || sl.sealed) return;", "sl.sealed = true, sl.seq = seq;",
              "occ_pending.fetch_add(1, std::memory_order_release);"])
    in_order("vk_replay.cpp occ_slot_begin (reset in the command buffer only without hostQueryReset; a new serial)",
             block(replay, "void occ_slot_begin(Recording& r) {"),
             ["if (!occ_host_reset) vkCmdResetQueryPool(r.cmd, occ_pool, (uint32_t)r.slot * kOccPerSlot, kOccPerSlot), sl.used = 0;",
              "sl.serial = occ_serial_next.fetch_add(1, std::memory_order_relaxed) + 1;", "sl.armed = true;"])
    submit = block(replay, "void submit_recording(Recording& r, bool signal_value = true, uint64_t signal_at = 0) {")
    in_order("vk_replay.cpp submit_recording (sealed after dev.present_mu is let go: the seal is declared before the lock)",
             submit, ["seq = ++rec_seq;", "struct OccSeal {", "if (occ_live && rec.submitted) occ_seal(rec, at);",
                      "} occ_sealed{r, seq};", "std::lock_guard lk(dev.present_mu);   // (one queue"])
    waitc = block(replay, "int wait_counted(uint64_t f, uint64_t timeout_ns) {")
    in_order("vk_replay.cpp wait_counted (a game's wait that ended: the done counts written first)", waitc,
             ["if (completed() >= f) {", "if (occ_pending.load(std::memory_order_relaxed)) occ_harvest_done(occ::Stats::kAtWait);",
              "return 1;", "const int ok = wait(f, timeout_ns);",
              "if (ok && occ_pending.load(std::memory_order_relaxed)) occ_harvest_done(occ::Stats::kAtWait);"])
    # M1: no counting state per thread for the replay's stream, none matched by the command buffer's handle
    if re.search(r"thread_local occ::Counter occ_tc\b", replay) or "c.cmd != cmd" in replay:
        raise SystemExit("vk_replay.cpp: the replay's counting is per thread or matched by command buffer handle again")
    ctr = block(replay, "occ::Counter& occ_ctr() {")
    in_order("vk_replay.cpp occ_ctr (a segment's: the thread's; the replay's: its queue's state's, under replay_mu)", ctr,
             ["if (rmode == Mode::Worker) return occ_seg;", "c = &occ_mains[cur_state];"])
    need("vk_replay.cpp occ_seg", replay, ["thread_local occ::Counter occ_seg;",
                                           "std::unordered_map<const NvnState*, occ::Counter> occ_mains;"])
    need("vk_replay.cpp occ_close (only in the recording it was begun in)", block(replay, "void occ_close(occ::Counter& c, VkCommandBuffer cmd) {"),
         ["occ_serial_of(cmd, &slot) == c.serial && slot == c.slot", "vkCmdEndQuery(cmd, occ_pool,", "c.lost = true"])
    measure = block(replay, "void occ_report_measure(VkCommandBuffer cmd, uint64_t addr, uint64_t pos, bool writable) {")
    in_order("vk_replay.cpp occ_report_measure (matched by serial; only a whole, writable count goes with the recording)", measure,
             ["occ_close(c, cmd);", "const uint64_t serial = occ_serial_of(cmd, &slot);", "c.report(occ_keep.load(std::memory_order_relaxed), serial,",
              "if (o == occ::Counter::kMeasured && writable) {", "rep.pos = pos", "occ_slots[c.slot].reps.push_back(rep);"])
    placed = block(replay, "void occ_placed(uint64_t addr, uint64_t pos, bool dry, bool flag, uint64_t value) {")
    in_order("vk_replay.cpp occ_placed (a clear flag, the skip list, then the knob: whether the count is to be written)", placed,
             ["bool write = !flag;", "occ::listed(occ_skip, addr)", '!occ_env_real && !lab_ab::on("occq")',
              "occ_writers.placed(addr, pos, occ_presents.load(std::memory_order_relaxed), write, value);"])
    for gone in ("cond_read(", "kept_cond", "conds_full", "kMostConds"):
        if gone in replay:
            raise SystemExit("vk_replay.cpp: the guard that kept a condition's words at 4096 is back: " + gone)
    in_order("vk_replay.cpp occ_cond (NX2navi's cond_enabled over the answer from memory; the knob's off phase as before; "
             "RX_VK_OCC_COND; the skipping state; one in 499 logged)",
             block(replay, "void occ_cond(const NvnState& s, uint64_t addr, int mode, uint64_t& x, uint64_t& y, bool* enabled) {"),
             ["occ_cond_off = nullptr;", "std::lock_guard lk(occ_mu);",
              "occ_writers.word(addr, now), sy = occ_writers.word(addr + 16, now);",
              "if (sx == occ::Writers::kNone && sy == occ::Writers::kNone) return;",
              'const bool active = occ_env_real || lab_ab::on("occq");',
              "if (active && (sx == occ::Writers::kWrittenSrc || sy == occ::Writers::kWrittenSrc)) {",
              "host->gpu_read(addr, &x, 8);", "*enabled = mode == 0 ? x == y : x != y;",
              "occ::cond_decide(*enabled, sx, sy, active, occ_cond_on);", "occ_st.cond(c.kind);",
              "*enabled = c.enabled;", "if (c.kind == occ::Cond::kWrittenOff) occ_cond_off = &s;",
              "if (occ_log_cond && seen++ % 499 == 0 && logged < 300) {"])
    in_order("vk_replay.cpp occ_rewritten (another counter type's report, or a small clear: the records leave Writers)",
             block(replay, "void occ_rewritten(uint64_t addr, uint64_t size, bool report) {"),
             ["occ_writers.erase(addr)", "occ_writers.cleared(addr, addr + size)"])
    need("vk_replay.cpp occ_skip_note", block(replay, "void occ_skip_note(const NvnState& s, bool clear) {"),
         ["if (s.render_enabled || occ_cond_off != &s) return;", "(clear ? occ_skips_clear : occ_skips_draw).fetch_add(1"])
    in_order("vk_replay.cpp occ_init (the condition switches)", init,
             ['occ_cond_on = !getenv("RX_VK_OCC_COND") || atoi(getenv("RX_VK_OCC_COND")) != 0;',
              'occ_log_cond = getenv("RX_VK_OCC_LOG_COND") && atoi(getenv("RX_VK_OCC_LOG_COND")) != 0;'])
    need("vk_replay.cpp occ_pos (the dry pass's place is the segment thread's)", replay,
         ["uint64_t occ_pos(bool dry) { return dry ? occ_dry_cb << 32 | (uint64_t)(dry_work & 0xffffffff) : replay_stream_pos(); }"])
    in_order("vk_replay.cpp occ_present_tick (the done ones read; Writers pruned by age every 60 presents, the line every 600)",
             block(replay, "void occ_present_tick() {"),
             ["occ_presents.store(presents_now, std::memory_order_relaxed);", "occ_harvest_done(occ::Stats::kAtPresent);",
              "if (++n % 60) return;", "occ_writers.prune(presents_now, occ::kKeepAge);", "if (n % 600) return;", "occ_st.line("])
    replay_fn = block(replay, "void replay(VkCommandBuffer& cmd, NvnState& s, const uint8_t* p, const uint8_t* end = nullptr) {")
    report = replay_fn[replay_fn.index("case NOP_REPORT_COUNTER: {"):replay_fn.index("case NOP_RESET_COUNTER: {")]
    in_order("vk_replay.cpp NOP_REPORT_COUNTER (the worker measures; the replay writes the fixed count in stream order, placed "
             "under occ_mu with its write; the dry pass measures nothing)", report,
             ["if (worker) {", "if (occ_live) {",
              "occ_report_measure(cmd, c.addr, replay_stream_pos(), (c.type & NVN_COUNTER_WRITTEN) == 0);", "break;",
              "const uint64_t count = samples ? (occ_fixed >= 0 ? (uint64_t)occ_fixed : s.samples_passed) : 0;",
              "const uint64_t rec[2] = {count, ns * 12 / 625};", "const auto write_record = [&] {",
              "else guest_cpu_write(c.addr, rec, sizeof rec);", "memory_cpu_owned(host->gpu_guest(c.addr), sizeof rec);",
              "if (samples && occ_live) {", "const uint64_t pos = occ_pos(dry);", "std::lock_guard lk(occ_mu);",
              "occ_placed(c.addr, pos, dry, flag_written, count);", "write_record();",
              "if (!dry) occ_report_measure(cmd, c.addr, pos, !flag_written);", "} else if (occ_live) {\n                std::lock_guard lk(occ_mu);\n                occ_rewritten(c.addr, 16, true);\n                write_record();",
              "} else {\n                write_record();\n            }\n            break;"])
    reset = replay_fn[replay_fn.index("case NOP_RESET_COUNTER: {"):]
    in_order("vk_replay.cpp NOP_RESET_COUNTER", reset[:400], ["s.samples_passed = 0;", "if (occ_live && !dry) occ_reset(cmd);"])
    cond = replay_fn[replay_fn.index("case NOP_RENDER_COND: {"):]
    in_order("vk_replay.cpp NOP_RENDER_COND (not the worker's: the replay's and the dry pass's decide, the segments take it)",
             cond[:2400],
             ["if (worker) {", "par_conds[cond_at] != 0", "break;", "s.render_enabled = r.mode == 0 ? x == y : x != y;",
              "if (occ_live) {", "if (r.snapped) occ_cond_off = nullptr;",
              "else occ_cond(s, r.addr, (int)r.mode, x, y, &s.render_enabled);", "if (always) s.render_enabled = true;",
              "if (dry) par_conds.push_back(s.render_enabled ? 1 : 0);"])
    enable = replay_fn[replay_fn.index("case NOP_RENDER_ENABLE:"):replay_fn.index("case NOP_RENDER_COND: {")]
    need("vk_replay.cpp NOP_RENDER_ENABLE (a condition's skipping ends)", enable,
         ["if (occ_live && !worker && occ_cond_off == &s) occ_cond_off = nullptr;"])
    clear_buf = replay_fn[replay_fn.index("case NOP_CLEAR_BUF: {"):]
    in_order("vk_replay.cpp NOP_CLEAR_BUF (a small clear in stream order: the records it covers leave Writers with its write)",
             clear_buf[:1600], ["if (in_stream && !worker) {", "if (occ_live) {", "std::lock_guard lk(occ_mu);",
                                "occ_rewritten(c0.dst, std::min<uint64_t>(c0.size, sizeof words), false);",
                                "guest_cpu_write(c0.dst, words, std::min<uint64_t>(c0.size, sizeof words));", "} else {",
                                "guest_cpu_write(c0.dst, words, std::min<uint64_t>(c0.size, sizeof words));   // (hookshot"])
    for op in ("case NOP_CLEAR_COLOR:", "case NOP_CLEAR_DS:"):
        at = replay_fn[replay_fn.index(op):]
        in_order("vk_replay.cpp " + op + " (a skipped clear counted, not a worker's)", at[:at.index("break;")],
                 ["if (occ_live && !worker) occ_skip_note(s, true);", "if (s.render_enabled && !dry) {"])
    in_order("vk_replay.cpp replay() (a skipped draw or dispatch counted, not a worker's)", replay_fn,
             ["h.op == NOP_DISPATCH || h.op == NOP_DISPATCH_INDIRECT) {", "if (occ_live && !worker) occ_skip_note(s, false);",
              "if (dry) {", "if (++dry_work % 64 == 0)"])
    handle = block(replay, "const uint8_t* replay_handle(VkCommandBuffer& cmd, const uint8_t* h, bool may_stop = false) {")
    in_order("vk_replay.cpp replay_handle (the replay's counting given up before a dry pass; its reports placed at cb's)", handle,
             ["const uint64_t cb = ++stream_cbs;", "if (occ_live) occ_serial_give_up(cmd), occ_dry_cb = cb;", "rmode = Mode::Dry;"])
    # (the never-sent bug, 11-17: a segment's recording is submitted here; it is sealed by its timeline number after the lock)
    in_order("vk_replay.cpp replay_handle (a segment's recording signals the recordings' timeline and is sealed after "
             "dev.present_mu is let go; off: the submit as before)", handle,
             ["draw_flush_recording(j->r->slot);", "if (occ_live && rec_timeline) {", "occ_seq = ++rec_seq;",
              "occ_sig.semaphore = rec_timeline, occ_sig.value = occ_seq,",
              "si.signalSemaphoreInfoCount = 1, si.pSignalSemaphoreInfos = &occ_sig;",
              "struct OccSeal {", "if (occ_live && rec.submitted) occ_seal(rec, at);", "} occ_sealed{*j->r, occ_seq};",
              "std::lock_guard lk(dev.present_mu);", "queue_flush_locked();", "vkQueueSubmit2(dev.queue, 1, &si, j->r->fence);",
              "j->r->submitted = res == VK_SUCCESS;"])
    rs = block(replay, "int64_t record_segment(VkCommandBuffer& cmd, const Segment& g) {")
    in_order("vk_replay.cpp record_segment (a segment counts on its own; its place is the dry pass's)", rs,
             ["rmode = Mode::Worker;", "stream_cb = g.cb, stream_work = g.work;", "if (occ_live) occ_segment_edge(cmd);",
              "replay(cmd, s, g.p, g.end);", "end_pass(cmd);", "if (occ_live) occ_segment_edge(cmd);   // (what this thread counted",
              "rmode = Mode::Serial;"])
    need("vk_replay.cpp occ_segment_edge", block(replay, "void occ_segment_edge(VkCommandBuffer cmd) {"),
         ["occ::Counter& c = occ_ctr();", "occ_close(c, cmd);", "c.segment();"])
    need("vk_replay.cpp occ_serial_give_up", block(replay, "void occ_serial_give_up(VkCommandBuffer cmd) {"),
         ["occ::Counter& c = occ_ctr();", "occ_close(c, cmd);", "c.segment();"])
    in_order("vk_replay.cpp occ_draw_begin", block(replay, "void occ_draw_begin(VkCommandBuffer cmd) {"),
             ["if (!c.may_begin(keep)) return;", "if (!c.begin(keep, occ_serial_of(cmd, &slot))) return;",
              "if (sl.used >= kOccPerSlot) return c.full();", "vkCmdBeginQuery(cmd, occ_pool,", "occ_open_here = true;"])
    need("vk_replay.cpp occ_pass_end (only this thread's query)", block(replay, "void occ_pass_end(VkCommandBuffer cmd) {"),
         ["if (occ_open_here) occ_close(occ_ctr(), cmd);"])
    need("vk_replay.cpp do_present", replay, ["if (occ_live) occ_present_tick();"])
    # every Vulkan call on the occlusion pool is in an occ_ function (behind occ_live or occ_pending): none in the off path
    for call in ("vkCmdBeginQuery(", "vkCmdEndQuery(", "vkGetQueryPoolResults(", "vkCmdResetQueryPool(", "vkResetQueryPool(",
                 "vkCreateQueryPool("):
        for m in re.finditer(re.escape(call), replay):
            if "occ_pool" not in replay[m.start():replay.index(";", m.start())]:
                continue   # (another pool's: the GPU timestamps')
            fn = replay.rfind("\nvoid occ_", 0, m.start())
            other = max(replay.rfind("\nvoid ", 0, m.start()), replay.rfind("\nbool ", 0, m.start()),
                        replay.rfind("\nuint64_t ", 0, m.start()), replay.rfind("\nint ", 0, m.start()))
            init_fn = replay.rfind("\nbool occ_init() {", 0, m.start())
            if max(fn, init_fn) < other:
                raise SystemExit("vk_replay.cpp: " + call + " outside the occ_ functions")

    state = (src / "gpu/vk/vk_state.h").read_text(encoding="utf-8")
    if "occ_first" in state:
        raise SystemExit("vk_state.h: the fingerprint's occ_first is back")

    draw = (src / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
    in_order("vk_draw.cpp end_pass (the query ends in its pass)", block(draw, "void end_pass(VkCommandBuffer cmd) {"),
             ["if (!pass().open) return;", "if (occ_live) occ_pass_end(cmd);", "vkCmdEndRendering(cmd);"])
    d = block(draw, "void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {")
    in_order("vk_draw.cpp draw", d, ["if (occ_live) occ_draw_enter();", "begin_pass(cmd, at);",
                                     "if (occ_live && pass().open) occ_draw_begin(cmd);"])
    issued = d.count("st_draws++, pass().draws++;")
    if issued != d.count("st_draws++, pass().draws++;\n") or issued != d.count("if (occ_live) occ_draw_issued();") or not issued:
        raise SystemExit("vk_draw.cpp draw: every draw it records (st_draws++) says so to the count (occ_draw_issued)")

    dev = (src / "docs/development.md").read_text(encoding="utf-8")
    for switch in ("RX_VK_OCC_QUERY=1", "RX_VK_OCC_QUERY_SKIP", "RX_VK_OCC_COND=0", "RX_VK_OCC_LOG_COND=1"):
        if "`" + switch not in dev:
            raise SystemExit("docs/development.md: " + switch + " is not in the switches table")
    if "RX_VK_OCC_QUERY_FP" in dev:
        raise SystemExit("docs/development.md: RX_VK_OCC_QUERY_FP is gone")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--cxx", default="clang++")
    a = p.parse_args()
    check_source(a.source)
    with tempfile.TemporaryDirectory(prefix="occ-answer-") as temp:
        exe = Path(temp) / "occ_answer_test.exe"
        flags = ["-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-I" + str(a.source / "gpu/vk")]
        subprocess.run([a.cxx, *flags, str(ROOT / "tests/occ_answer_test.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("occ_answer: ok")


if __name__ == "__main__":
    main()
