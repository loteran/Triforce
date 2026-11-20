"""Start next frame early: the early tail (gpu/vk/tail_early.h, gpu/vk/vk_replay.cpp "the early tail"; no game, no GPU).

The replay cuts a frame's recording at the gameplay upscale's dispatch, holds the event signals of the rest (the tail: the
upscaler and the interface), and at the recording's end writes their words ahead of the tail's commands when the tail did
nothing the game could see (verdict: no written binding, no guest memory bound, no program past its bindings, no texture written
back, every word in a pool), else at its end as before. The Debug tab's Developer row, tail_early=, on by default since
options_version 6 (the owner's choice); RX_VK_TAIL_EARLY over it.

1. The header's rules compiled and run: each rule refuses on its own, the first failing one is named, a signal of mode ADD adds to
   the last value held for its word.
2. The model (tests/tail_early_test.cpp): the production functions cut out of vk_replay.cpp (the tail's state, tail_hold, tail_seen,
   tail_begin, tail_at_dispatch, tail_words, tail_words_record, tail_end, tail_early_record, finish_recording, gpu_write) run
   against stand-ins that record the command buffers and a queue that runs them in order. In each scenario (a clean tail, one
   recorded after a parallel replay's segments, one per rule that refuses it, one submitted before its end, and no tail at all)
   the recorded stream must
   hold: an early tail's words after every write-back of the work before it and after its uploads, before its commands; nothing
   written to guest memory after them but the tail's uniform updates' write-backs, and nothing read there; every word with the
   console's value, in guest memory and in the mirror; a refused tail's words after its commands; a full barrier at each held
   signal; the uniform updates in their own recordings.
3. Each rule taken away in turn (a mutation of the header or of the cut code) and the scenario it guards must then fail.
4. The source: the steps the model copies (the signal's hold, the dispatch's look, no split and no parallel replay in a tail, the
   parallel replay's segments ending at the upscale's dispatch, the words between the uploads and the commands in submit_recording,
   finish_recording's order), the counts in vk_memory.cpp and vk_draw.cpp, the row, its key, its default and its documentation.

    python -I -B tests/test_tail_early.py [--cxx g++] --headers <Vulkan headers' include folder>
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def function(text, start):
    at = text.find(start)
    check(at >= 0, "%s is missing" % start.strip())
    if text[at:text.index("\n", at)].rstrip().endswith("}"):   # (a one-line function)
        return text[at:text.index("\n", at) + 1]
    return text[at:text.index("\n}\n", at) + 3]


def in_order(text, steps, what):
    at = -1
    for s in steps:
        k = text.find(s, at + 1)
        check(k >= 0, "%s: %r is missing or out of its order" % (what, s))
        at = k


CUT = ["bool tail_on() {", "bool tail_last(uint64_t gpu, uint32_t* v) {", "bool tail_hold(VkCommandBuffer cmd, uint64_t gpu, uint32_t v) {",
       "tail_early::Seen tail_seen() {", "void tail_begin(VkCommandBuffer& cmd) {",
       "void tail_at_dispatch(VkCommandBuffer& cmd, NvnState& s) {", "void tail_stats() {", "bool tail_words() {",
       "void tail_words_record(VkCommandBuffer cb) {", "void tail_end(Recording& r, const tail_early::Seen& seen, bool baked) {",
       "bool tail_early_record(Recording& r) {", "void finish_recording(uint64_t value) {",
       "void gpu_write(VkCommandBuffer cmd, uint64_t gpu, const void* data, uint64_t n) {"]


def cut(replay):
    start = replay.index("struct TailState {")
    end = replay.index("\n", replay.index("thread_local bool tail_here = false;", start)) + 1
    return replay[start:end] + "".join(function(replay, name) for name in CUT)


# (each rule taken away, and the scenario that must then fail: where, the text, what replaces it, the scenario, the check it fails)
MUTATIONS = [
    ("header", "    if (s.written) return WRITES;\n", "", "written", "late"),
    ("header", "    if (s.guest) return GUEST;\n", "", "guest", "late"),
    ("header", "    if (s.programs) return PROGRAMS;\n", "", "program", "late"),
    ("header", "    if (s.bake) return BAKE;\n", "", "bake", "late"),
    ("header", "            if (list_[i].gpu == gpu) return *v = list_[i].v, true;\n",
     "            if (false && list_[i].gpu == gpu) return *v = list_[i].v, true;\n", "clean", "values"),
    ("cut", "    if (why == tail_early::EARLY && !tail_words()) why = tail_early::EVENT;\n",
     "    if (why == tail_early::EARLY) tail_words();\n", "outside", "values"),
    ("cut", "    memory_end_submission(cmd);\n    full_barrier(cmd, \"barrier (recording end)\");\n    submit_recording(*rec_cur, false, 0, true);",
     "    full_barrier(cmd, \"barrier (recording end)\");\n    submit_recording(*rec_cur, false, 0, true);", "clean", "values"),
    ("cut", "    tail_words_record(r.upload);", "    tail_words_record(r.cmd);", "clean", "order"),
    ("cut", "    full_barrier(cmd, \"barrier (an event signal in the early tail: its word held)\");\n", "", "clean", "barrier"),
    ("cut", "            tl.words.push_back({mirror.buffer, mirror.offset, g.v});\n", "", "clean", "values"),
    ("cut", "        gpu_write(r.cmd, g.gpu, &g.v, 4);\n", "        (void)g;\n", "written", "values"),
    ("cut", "    flush_ub_updates(cmd, ub_here);", "    flush_ub_updates(cmd);", "worker", "ub"),
    ("cut", "    const tail_early::Seen seen = tail ? tail_seen() : tail_early::Seen{};\n    flush_ub_updates(r.cmd);",
     "    flush_ub_updates(r.cmd);\n    const tail_early::Seen seen = tail ? tail_seen() : tail_early::Seen{};", "clean", "verdict"),
]
SCENARIOS = ["clean", "worker", "written", "guest", "program", "bake", "outside", "cut", "none"]

HEADER_TEST = r'''#include <cstdio>
#include "tail_early.h"
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    using namespace tail_early;
    Seen s;
    CHECK(verdict(s) == EARLY);
    Seen w = s; w.written = 1; CHECK(verdict(w) == WRITES);
    Seen g = s; g.guest = 2; CHECK(verdict(g) == GUEST);
    Seen p = s; p.programs = 1; CHECK(verdict(p) == PROGRAMS);
    Seen b = s; b.bake = true; CHECK(verdict(b) == BAKE);
    Seen e = s; e.event_outside = true; CHECK(verdict(e) == EVENT);
    Seen c = s; c.cut = true; c.written = 3; CHECK(verdict(c) == CUT);   /* (the first rule it fails is named) */
    Seen all = s; all.guest = 1; all.programs = 1; all.bake = true; CHECK(verdict(all) == GUEST);
    for (int k = 0; k < WHY_N; k++) CHECK(why_name((Why)k)[0] != '?');
    Held h;
    uint32_t v = 0;
    CHECK(!h.last(0x10, &v) && h.empty());
    h.add(0x10, 1), h.add(0x20, 7), h.add(0x10, 3);
    CHECK(h.last(0x10, &v) && v == 3 && h.last(0x20, &v) && v == 7 && !h.last(0x30, &v) && h.all().size() == 3);
    h.clear();
    CHECK(h.empty() && !h.last(0x10, &v));
    Stats t;
    t.judged(EARLY, 2), t.judged(WRITES, 3);
    CHECK(t.tails == 2 && t.signals == 5 && t.moved == 2 && t.why[EARLY] == 1 && t.why[WRITES] == 1);
    std::printf("ok: the header's rules\n");
    return 0;
}
'''


def check_source():
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    memory = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    menu_h = (VK / "vk_menu.h").read_text(encoding="utf-8")
    sig = replay[replay.index("        case NOP_SIGNAL_EVENT: {"):]
    sig = sig[:sig.index("        case NOP_WAIT_EVENT:")]
    in_order(sig, ["if (dry) break;", "if (held) old = held->v;", "else if (!tail_last(e.gpu, &old)) host->gpu_read(e.gpu, &old, 4);",
                   "if (tail_hold(cmd, e.gpu, v)) break;", "BufferRef word{};", "gpu_write(cmd, e.gpu, &v, 4);",
                   'if (early) guest_cpu_write(e.gpu, &v, 4);'], "vk_replay.cpp NOP_SIGNAL_EVENT")
    for op in ("NOP_DISPATCH: {", "NOP_DISPATCH_INDIRECT: {"):
        case = replay[replay.index("        case " + op):]
        case = case[:case.index("break;")]
        in_order(case, ["if (s.render_enabled && !dry) tail_at_dispatch(cmd, s);", "if (s.render_enabled && !dry) dispatch(cmd, s,"],
                 "vk_replay.cpp " + op)
    loop = function(replay, "void replay(VkCommandBuffer& cmd, NvnState& s, const uint8_t* p, const uint8_t* end = nullptr) {")
    in_order(loop, ["if (dry && dry_tail_find) dry_tail_check(p, h, a, avail, s);", "if (dry && dry_up_find) dry_up_check(p, h, s);",
                    "switch (h.op) {"], "vk_replay.cpp replay()")
    up = function(replay, "void dry_up_check(const uint8_t* p, const nvn_op_hdr& h, NvnState& s) {")
    check("!upscale_dispatch(s)" in up and "par_up.p = p, par_up.s = s, par_up.ub = ub_updates.size()" in up,
          "dry_up_check no longer keeps the state before the upscale's dispatch")
    split = function(replay, "void maybe_split(VkCommandBuffer& cmd) {")
    in_order(split, ["if (tail_on()) return;", "++part_work < every"], "vk_replay.cpp maybe_split: no split in a tail")
    handle = function(replay, "const uint8_t* replay_handle(VkCommandBuffer& cmd, const uint8_t* h, bool may_stop = false) {")
    check("if (threads < 2 || !rec_cur || debug || tail_on() ||" in handle, "replay_handle: a tail's command buffers are not alone")
    in_order(handle, ["dry_up_find = tail_wanted(), par_up_at = false;", "rmode = Mode::Dry;", "dry_up_find = false;",
                      "const bool up = par_up_at;", "const bool tail = par_tail_at && !(up && par_tail.p == par_up.p);",
                      "const int64_t until = tail ? par_tail.work : up ? par_up.work : total;",
                      "seg.back().end = up ? par_up.p : stop;"], "vk_replay.cpp replay_handle")
    check(handle.count("if (up) tail_rest(cmd, cb, stop);") == 3 and handle.count("return stop;") == 3,
          "replay_handle: the rest after the upscale's dispatch is not recorded on every way out")
    check(len(re.findall(r"if \(up\) tail_rest\(cmd, cb, stop\);[^\n]*\n\s*return stop;", handle)) == 3,
          "replay_handle: tail_rest is not right before each return")
    rest = function(replay, "void tail_rest(VkCommandBuffer& cmd, uint64_t cb, const uint8_t* stop) {")
    in_order(rest, ["Segment g{par_up.p, stop, &par_up.s, par_up.ub, par_up.cond, par_up.work, cb};", "tail_here = true;",
                    "record_segment(cmd, g);", "tail_here = false;"], "vk_replay.cpp tail_rest")
    submit = function(replay, "void submit_recording(Recording& r, bool signal_value = true, uint64_t signal_at = 0, bool part = false) {")
    in_order(submit, ["bool uploads = texture_uploads_recorded();", "if (tail_early_record(r)) uploads = true;", "gpu_time_end(r, uploads);",
                      "vkEndCommandBuffer(r.upload);", "vkEndCommandBuffer(r.cmd);", "if (uploads) cb[n++].commandBuffer = r.upload;",
                      "cb[n++].commandBuffer = r.cmd;", "std::lock_guard lk(dev.present_mu);"], "vk_replay.cpp submit_recording")
    finish = function(replay, "void finish_recording(uint64_t value) {")
    in_order(finish, ["end_pass(r.cmd);", 'event_writes_flush(r.cmd, "barrier (events\' held writes, before the recording\'s end)");',
                      "const bool tail = tail_on() && tl.rec == &r;", "const tail_early::Seen seen = tail ? tail_seen() : tail_early::Seen{};",
                      "flush_ub_updates(r.cmd);", "const uint64_t baked = work_between();",
                      "texture_end_recording(r.cmd);", "if (tail) tail_end(r, seen, work_between() != baked);",
                      "memory_end_submission(r.cmd);", 'full_barrier(r.cmd, "barrier (recording end)");', "submit_recording(r, signal, at);"],
             "vk_replay.cpp finish_recording")
    wanted = function(replay, "bool tail_wanted() {")
    check('menu_row_at_start("RX_VK_TAIL_EARLY", "tail_early", true, 6)' in wanted and 'getenv("RX_VK_BENCH")' in wanted,
          "tail_wanted: not the row tail_early= (on by default since options_version 6) with RX_VK_TAIL_EARLY over it, or off "
          "with the bench")
    # (the counts the verdict reads)
    bind = function(memory, "BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t size, bool written) {")
    check(bind.startswith("BufferRef gpu_buffer_bind(") and
          "if (written) texture_gpu_wrote(gpu, size), st_binds_written.fetch_add(1, std::memory_order_relaxed);" in bind,
          "vk_memory.cpp gpu_buffer_bind does not count every written binding")
    lam = bind[bind.index("auto in_guest = [&](BufferRef ref) {"):]
    lam = lam[:lam.index("};")]
    check("st_binds_guest.fetch_add(1, std::memory_order_relaxed);" in lam, "vk_memory.cpp: a binding given guest memory is not counted")
    guest_returns = re.findall(r"return [^;]*\{p->buffer, off, size, 0\}[^;]*;", bind)
    check(guest_returns and all("in_guest(" in r for r in guest_returns),
          "vk_memory.cpp gpu_buffer_bind gives guest memory past in_guest (the early tail would not see it)")
    push = function(draw, "void push_constants(VkCommandBuffer cmd, const Program& p, const Targets* at, uint32_t fallback_first = 0) {")
    in_order(push, ["checkpoint(cmd, p, at ? 'd' : 'c');", "if (tail_watch_on.load(std::memory_order_relaxed)) tail_program(p);",
                    "if (!p.uses_push) return;"], "vk_draw.cpp push_constants")
    prog = function(draw, "void tail_program(const Program& p) {")
    check("uses_global_memory" in prog and "texture_buffer_descriptors" in prog and "image_buffer_descriptors" in prog,
          "vk_draw.cpp tail_program: not the page table's programs and the texel buffers'")
    disp = function(draw, "bool upscale_dispatch(NvnState& s) {")
    check("gameplay_network(cp->key)" in disp and "uses_cooperative_matrix" in disp and "jobs_wait" not in disp,
          "vk_draw.cpp upscale_dispatch: not the gameplay networks as the stand-ins find them, or it waits")
    # (the row, its key and default, and where it is documented)
    rows = re.findall(r'\{TAB_DEBUG, "Start next frame early", &opts\.tail_early, 0, 1, 1, tx_onoff,[^}]*nullptr, true, true\}', menu)
    check(len(rows) == 1, "vk_menu.cpp: the Debug tab's Start next frame early row (On/Off, applies after a restart) is missing")
    in_order(menu, ['{TAB_DEBUG, "Developer", nullptr', '{TAB_DEBUG, "Replay helper threads"', '"Start next frame early"',
                    '{TAB_SYSTEM, "Restart"'], "vk_menu.cpp: the row among the Developer rows (after Replay helper threads)")
    check('"Start next frame early (experimental)"' not in menu, "vk_menu.cpp: the old Graphics row is still there")
    init = function(menu, "void menu_init() {")
    check("if (version < 6) opts.tail_early = 1;" in init, "vk_menu.cpp menu_init: an options file before version 6 keeps its old off")
    check('out += "options_version=7\\n";' in menu, "vk_menu.cpp: the options file is not written as version 7 (6 or later)")
    check("if (version < 7) opts.pace = 1;" in init, "vk_menu.cpp menu_init: an options file before version 7 keeps Frame pacing off")
    check("int pace = 1;" in (ROOT / "gpu" / "vk" / "vk_menu.h").read_text(encoding="utf-8"),
          "vk_menu.h: Frame pacing is not on by default")
    key = function(menu, "const char* ini_key(const Row& r) {")
    check('if (!strcmp(r.name, "Start next frame early")) return "tail_early";' in key,
          "vk_menu.cpp ini_key: the row is not kept as tail_early=")
    check(re.search(r"\n    int tail_early = 1; ", menu_h), "vk_menu.h: opts.tail_early is not on by default")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    dev = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
    check("| Start next frame early | Off, On (the Debug tab; on by default" in readme, "readme.md: the row is not in the settings table")
    check("| `RX_VK_TAIL_EARLY=1` or `=0` |" in dev, "docs/development.md: RX_VK_TAIL_EARLY is not in the switches table")
    return replay


def build(cxx, headers, tmp, name, header, cut_code):
    d = tmp / name
    d.mkdir()
    (d / "tail_early.h").write_text(header, encoding="utf-8")
    (d / "tail_early_cut.inc").write_text(cut_code, encoding="utf-8")
    exe = d / ("model.exe")
    subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
                    "-Wno-missing-field-initializers", "-I" + str(d), "-I" + str(headers), str(ROOT / "tests" / "tail_early_test.cpp"),
                    "-o", str(exe)], check=True)
    return exe


def run(exe, scenario):
    return subprocess.run([str(exe), scenario], capture_output=True, text=True, timeout=60)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--headers", type=Path, required=True)
    a = p.parse_args()
    try:
        replay = check_source()
    except ValueError as e:
        raise SystemExit("FAIL: a text this test reads moved (%s)" % e)
    print("PASS: the source has the steps the model copies, the counts, the row, its key, default and documentation")
    header = (VK / "tail_early.h").read_text(encoding="utf-8")
    code = cut(replay)
    with tempfile.TemporaryDirectory(prefix="tail-early-") as tmp:
        tmp = Path(tmp)
        (tmp / "h").mkdir()
        (tmp / "h" / "header.cpp").write_text(HEADER_TEST, encoding="utf-8")
        exe = tmp / "h" / "header.exe"
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(VK), str(tmp / "h" / "header.cpp"), "-o", str(exe)],
                       check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        check(out.returncode == 0, "the header's rules: " + out.stdout.strip())
        print("PASS: " + out.stdout.strip()[4:])
        model = build(a.cxx, a.headers, tmp, "model", header, code)
        for s in SCENARIOS:
            out = run(model, s)
            check(out.returncode == 0, "scenario %s: %s" % (s, (out.stdout + out.stderr).strip()))
            print("PASS: " + out.stdout.strip()[3:])
        for k, (where, old, new, scenario, fails) in enumerate(MUTATIONS):
            src = header if where == "header" else code
            check(src.count(old) == 1, "mutation %d: its text is not in the %s once: %r" % (k, where, old[:70]))
            mutated = src.replace(old, new)
            exe = build(a.cxx, a.headers, tmp, "m%d" % k, mutated if where == "header" else header, mutated if where == "cut" else code)
            out = run(exe, scenario)
            check(out.returncode == 1 and ("FAIL " + fails) in out.stdout,
                  "mutation %d (%s %r) left scenario %s passing its %s check: %s" % (k, where, old.strip()[:60], scenario, fails,
                                                                                    out.stdout.strip()[:300]))
        print("PASS: each of the %d mutations (a rule taken away) fails the scenario it guards" % len(MUTATIONS))


if __name__ == "__main__":
    main()
