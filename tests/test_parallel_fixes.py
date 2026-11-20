"""The parallel replay's holes that an audit found, fixed before the Replay helper threads row became Automatic by default
(gpu/vk/replay_threads.h lists them). No game, no GPU; this toolchain has no thread sanitizer for its Windows target
(clang: unsupported option '-fsanitize=thread' for target 'x86_64-w64-windows-gnu'), so the threaded cases check an
invariant of their own.

1. The texture cache under the recording lock, from every dispatch a worker records: vk_draw.cpp's upscale_stub,
   temporal_rejected_network and taau_stand_in are read scope by scope: each texture_get, texture_view, texture_written
   and typed_upscale call lies in a scope that holds ParLock, and no such scope reaches the temporal upscaler's own calls
   (which take their lock and then this one: the other order would deadlock).
2. A copy's and a texture clear's destination marked in the dry pass: vk_xfer.cpp's own dry_copy_b2t, dry_copy_t2t,
   dry_clear_tex and b2t_on_cpu, cut out and run against a model of the texture cache and the queue
   (tests/parallel_dry_copies_test.cpp): with the mark a later segment samples what the earlier segment's copy left, and
   the same stream without it samples the game's memory (the hole, shown); what the copies leave out marks nothing; a
   BC1 destination is BC3 first; the CPU's copy is made in the dry pass once. And vk_replay.cpp calls them in the dry pass.
3. The page table's changes in the order they were made: vk_memory.cpp's own table code cut out and run on real threads
   (tests/parallel_table_order_test.cpp): a later segment's import and an earlier segment's retire of the same pages end
   on the retired pool when the queue has run the segments, and on the live one after memory_table_segments_end.
4. A temporal upscaler, or DLSS's or FSR's frame generation, on: with RX_VK_PARALLEL_TEMPORAL=0 the frame is recorded on the
   replay thread alone (vk_replay.cpp par_stream_order_only, asked by replay_handle and by the tuner); by default their
   hooks are told in stream order by the dry pass (tests/test_stream_order.py), and a command buffer that starts with the
   engine's work pending is recorded on the replay thread alone.
5. The lock-free look of a texture (gpu/vk/touch_quick.h, tests/parallel_touch_quick_test.cpp): step by step through a
   locked look with an upload, never "up to date" for an earlier segment, where the look as it was says so mid-upload;
   then 20,000 submissions on two threads. And vk_texture.cpp writes and reads the fields in that order.
6. A flat texture's one-slice 3D copy: its first layout change through InitRedirect, and copied again by an earlier segment.

    python -I -B tests/test_parallel_fixes.py [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def check(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


def function(text, head):
    start = text.find(head)
    check(start >= 0, "%s is missing" % head.rstrip(" {("))
    return text[start:text.index("\n}\n", start) + 3]


def code_only(text):
    """Without comments and string literals, so that a brace or a name inside one is not taken for code"""
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    text = re.sub(r"'(?:\\.|[^'\\\n])+'", "' '", text)
    return re.sub(r"//[^\n]*", "", text)


def lock_scopes(body, under_lock, never_under_lock, name):
    """Every call of under_lock lies in a scope where a ParLock was declared before it (or in one around it); no call of
    never_under_lock does. Returns how many of each were seen"""
    code = code_only(body)
    token = re.compile(r"[{}]|\bParLock\s+\w+\s*[;(]|\b(?:%s)\(" % "|".join(map(re.escape, under_lock + never_under_lock)))
    scopes, seen = [False], {}
    for m in token.finditer(code):
        t = m.group(0)
        if t == "{":
            scopes.append(False)
        elif t == "}":
            scopes.pop()
            check(scopes, "%s: its braces do not pair" % name)
        elif t.startswith("ParLock"):
            scopes[-1] = True
        else:
            call = t[:-1]
            seen[call] = seen.get(call, 0) + 1
            if call in under_lock:
                check(any(scopes), "%s calls %s without the recording lock (ParLock)" % (name, call))
            else:
                check(not any(scopes), "%s calls %s with the recording lock held: the upscaler takes its own lock and "
                      "then this one" % (name, call))
    return seen


def check_stand_ins():
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    cache = ["texture_get", "texture_view", "texture_written", "typed_upscale"]
    engine = ["temporal_rejected_network", "temporal_upscale"]
    stub = lock_scopes(function(draw, "bool upscale_stub(VkCommandBuffer cmd"), cache, engine, "upscale_stub")
    check(stub.get("texture_get") == 2 and stub.get("texture_written", 0) >= 3 and stub.get("typed_upscale") == 1 and
          stub.get("temporal_rejected_network") == 1, "upscale_stub no longer has the calls this test reads: update it")
    rejected = lock_scopes(function(draw, "static bool temporal_rejected_network(VkCommandBuffer cmd, uint64_t key, Texture* src, const nvn_tex* in, Texture* dst,\n"
                                          "                                      const nvn_tex* out) {"),
                           cache, engine, "temporal_rejected_network")
    check(rejected.get("texture_view") == 2 and rejected.get("texture_written") == 1 and rejected.get("temporal_upscale") == 1,
          "temporal_rejected_network no longer has the calls this test reads: update it")
    taau = lock_scopes(function(draw, "bool taau_stand_in(VkCommandBuffer cmd"), cache, engine, "taau_stand_in")
    check(taau.get("typed_upscale") == 1, "taau_stand_in no longer calls typed_upscale: update this test")
    # the dispatch itself calls the stub before it takes any lock: the stub's own are the only ones
    dispatch = function(draw, "void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {")
    check(dispatch.index("upscale_stub(cmd, s, compute_code_key(s))") < dispatch.index("ParLock"),
          "dispatch calls upscale_stub under a lock of its own now: read both again")
    # the anti-aliasing stand-in keeps its state under its own lock and does not touch the texture cache's tables
    aa = code_only((VK / "vk_aa.cpp").read_text(encoding="utf-8"))
    check(not re.search(r"\btexture_(get|view|written|touch)\(", aa) and "ParLock" not in aa,
          "vk_aa.cpp uses the texture cache now: it needs the recording lock, and a look at its own lock's order")
    for fn in ("bool aa_record(", "void aa_composed("):
        check(re.search(re.escape(fn) + r"[^{]*\{(?:(?!\n\}\n).)*std::lock_guard lk\(s\.mu\);", aa, re.S),
              "vk_aa.cpp: %s no longer takes its own lock" % fn.rstrip("("))


def check_replay():
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    loop = function(replay, "void replay(VkCommandBuffer& cmd, NvnState& s, const uint8_t* p, const uint8_t* end = nullptr) {")
    for op, dry_call, work in (
            ("case NOP_COPY_B2T:", "if (dry && dry_copies_on()) dry_copy_b2t(s, payload<nvn_p_copy_bt>(a, avail));",
             "copy_b2t(cmd, s, payload<nvn_p_copy_bt>(a, avail), true, worker && dry_copies_on());"),
            ("case NOP_COPY_T2T:", "if (dry && dry_copies_on()) dry_copy_t2t(payload<nvn_p_copy_tt>(a, avail));",
             "copy_t2t(cmd, s, payload<nvn_p_copy_tt>(a, avail));"),
            ("case NOP_CLEAR_TEX:", "if (dry && dry_copies_on()) dry_clear_tex(payload<nvn_p_clear_tex>(a, avail));",
             "clear_tex(cmd, s, payload<nvn_p_clear_tex>(a, avail));")):
        part = loop[loop.index(op):]
        part = part[:part.index("break;")]
        check(dry_call in part, "vk_replay.cpp: the dry pass no longer marks what %s writes" % op[5:-1])
        check("if (!dry) { ParLock lk; " + work in part, "vk_replay.cpp: %s is no longer recorded under the lock, outside the dry pass" % op[5:-1])
    # a temporal upscaler or a frame generation: on the replay thread alone with RX_VK_PARALLEL_TEMPORAL=0, and while the
    # engine's work is pending; else their hooks in stream order (tests/test_stream_order.py)
    check("bool par_temporal_hooks() { return temporal_wanted() || fsr3_inputs::wanted(); }" in replay,
          "par_temporal_hooks no longer asks both")
    only = function(replay, "bool par_stream_order_only() {")
    check("const bool hooks = par_temporal_hooks();" in only and "const bool only = hooks && !par_temporal_on();" in only,
          "par_stream_order_only no longer keeps such a frame on the replay thread alone with RX_VK_PARALLEL_TEMPORAL=0")
    handle = function(replay, "const uint8_t* replay_handle(VkCommandBuffer& cmd, const uint8_t* h, bool may_stop = false) {")
    serial = handle[:handle.index("// the dry pass")]
    check("|| par_stream_order_only() || (hooks && temporal_pre_pending())) {" in serial and "replay(cmd, state, h);" in serial,
          "replay_handle records a frame on workers with RX_VK_PARALLEL_TEMPORAL=0, or with the engine's work pending")
    # (the tuner is replay_threads.h's: par_tune_present tells it, and it starts no trial and judges no window then;
    # tests/test_replay_threads_tuner.py runs it)
    tuner = (VK / "replay_threads.h").read_text(encoding="utf-8")
    check("tuner.step(fr, par_auto_on, par_stream_order_only());" in function(replay, "void par_tune_present() {") and
          "if (order_only) return s;" in tuner and "if (win_.n >= kWindow * 3 / 4 && !order_only) {" in tuner,
          "the tuner starts a trial while both ways are the replay thread alone")
    # the page table's changes once more, after the segments, in the recording that follows them
    after = handle[handle.index("Recording& nr = begin_recording(rec_cur_value);"):]
    check(0 < after.index("memory_table_segments_end();") < after.index("full_barrier(nr.cmd);"),
          "replay_handle no longer writes the segments' page table changes once more after them")
    memory = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
    setter = function(memory, "void guest_pages_set(uint64_t lo, uint64_t hi, uint64_t address) {")
    check(setter.index("par_table_log.push_back({first, count, at});") < setter.index("if (table_defer && !now) {"),
          "guest_pages_set keeps only the changes it holds back, not the ones written at once")


def check_texture():
    tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    touch = function(tex, "Texture* texture_touch(Texture* tex, bool for_sampling) {")
    check(0 < touch.index("} touching(tex);") < touch.index("tex->checked_epoch = epoch;") and
          "explicit Touching(Texture* tex) : t(tex) { t->touching.store(1), std::atomic_thread_fence(std::memory_order_seq_cst); }" in touch
          and "~Touching() { t->touching.store(0); }" in touch,
          "texture_touch no longer says a locked look is under way, with a fence, before it notes that it looked")
    quick = function(tex, "bool texture_touch_quick(Texture* tex, bool for_sampling) {")
    order = [quick.index(s) for s in ("&tex->uploaded,", "&tex->checked_epoch,", "tex->touching.load()", "&tex->upload_value,",
                                      "&tex->upload_seg,", "touch_quick::up_to_date(seen, epoch, rec_value, tl_seg)")]
    check(order == sorted(order), "texture_touch_quick no longer reads the fields in touch_quick.h's order")
    twin = function(tex, "VkImageView twin3d_view(")
    made = twin[twin.index("if (!tex->twin3d) {"):twin.index("tex->twin3d_value = ~0ull, tex->twin3d_written = ~0ull;")]
    check(0 < made.index("InitRedirect redirect;") < made.index("vkCmdPipelineBarrier2(upload_cmd(), &di);"),
          "twin3d_view's first layout change no longer goes through InitRedirect")
    check("(tex->twin3d_rec == rec_value && tex->twin3d_seg > tl_seg)) {" in twin and
          "tex->twin3d_rec = rec_value, tex->twin3d_seg = tl_seg;" in twin,
          "twin3d_view's copy is no longer made again by a segment before the one that made it last")


def build_and_run(cxx, tmp, name, source, cases, extra=()):
    src, exe = Path(tmp) / (name + ".cpp"), Path(tmp) / (name + ".exe")
    src.write_text(source, encoding="utf-8")
    built = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", *extra, str(src), "-o", str(exe)], capture_output=True, text=True)
    check(built.returncode == 0, "%s did not build:\n%s" % (name, built.stderr))
    env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_")}
    out = []
    for case in cases:
        r = subprocess.run([str(exe)] + ([case] if case else []), env=env, capture_output=True, text=True, timeout=120)
        check(r.returncode == 0, "%s %s:\n%s" % (name, case or "", (r.stdout + r.stderr).strip()))
        out += r.stdout.strip().splitlines()
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    for part in (check_stand_ins, check_replay, check_texture):
        try:
            part()
        except ValueError as e:   # (a text one of them looks for is gone)
            raise SystemExit("FAIL: %s: the source it reads lost a fix, or moved (%s)" % (part.__name__, e))
    xfer =(VK / "vk_xfer.cpp").read_text(encoding="utf-8")
    memory = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
    start = xfer.index("bool b2t_gpu_on() {")
    cpu = xfer[start:xfer.index("\n} // namespace", start)]
    start = xfer.index("void dry_copy_b2t(NvnState& st, const nvn_p_copy_bt& p) {")
    dry = xfer[start:xfer.index("\nvoid copy_b2t(VkCommandBuffer cmd", start)]
    check("dry_clear_tex" in dry and "dry_copy_t2t" in dry, "vk_xfer.cpp's dry-pass functions moved: update this test")
    start = memory.index("struct TableRun {")
    flush = memory.index("bool memory_table_flush() {", start)
    table = memory[start:memory.index("\n}\n", flush) + 3]
    check("void memory_table_segments_end() {" in table and "void memory_table_begin() {" in table,
          "vk_memory.cpp's page table code moved: update this test")
    with tempfile.TemporaryDirectory(prefix="parallel-fixes-") as tmp:
        fixture = (ROOT / "tests/parallel_dry_copies_test.cpp").read_text(encoding="utf-8").replace("/* ACTUAL */", cpu + "\n" + dry)
        build_and_run(args.cxx, tmp, "dry_copies", fixture,
                      ["t2t", "t2t-unmarked", "clear", "clear-unmarked", "clear-drawn", "b2t", "b2t-unmarked", "skips", "bc1", "b2t-cpu"])
        fixture = (ROOT / "tests/parallel_table_order_test.cpp").read_text(encoding="utf-8").replace("/* ACTUAL */", table)
        build_and_run(args.cxx, tmp, "table_order", fixture, ["segments", "segments-unreconciled", "serial", "direct"])
        lines = build_and_run(args.cxx, tmp, "touch_quick", (ROOT / "tests/parallel_touch_quick_test.cpp").read_text(encoding="utf-8"),
                              [None], ("-Werror", "-I", str(VK)))
    for line in lines[:-1]:
        print(line)
    print("PASS: the parallel replay: the texture cache under the lock from every dispatch, copies and clears marked in the dry "
          "pass, the page table's changes in the order they were made, upscalers on the replay thread alone, the lock-free "
          "look, the 3D copy's first layout change")


if __name__ == "__main__":
    main()
