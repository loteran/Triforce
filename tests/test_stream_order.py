"""The parallel replay with a temporal upscaler or a frame generation on (NVIDIA DLSS, AMD FSR 4 and 3.1, their frame
generations): gpu/vk/stream_order.h. Their hooks in the draws decide "first" by the order they are told in; the dry pass
tells them in stream order, up to the first draw or dispatch that takes what they noted (the tail), which is recorded in
order after the segments before it, and the skinned draws are named again in stream order where they are taken. No game,
no GPU; this toolchain has no thread sanitizer for its Windows target, so the threaded cases check their own invariant.

1. Production code cut out and run (tests/stream_order_test.cpp): vk_temporal.cpp's temporal_pre_point,
   temporal_pre_point_dry and temporal_pre_pending, vk_replay.cpp's segment state and record_segment, replay_hooks_told and
   replay_stream_pos, stream_order.h whole. Random frames of 1 to 3 command buffers, recorded on the replay thread alone and
   in segments on 3 threads with random delays: the same camera, the same point before the bloom, the same point learnt, the
   same names for the skinned draws the engine takes ("order"); without the dry pass's telling they differ ("hole"); the
   header's steps on their own ("steps").
2. The sources read: vk_draw.cpp's draw() asks the hooks in the order the fixture's model has them, and not where the dry
   pass told them; dry_temporal_draw leaves out what draw() leaves out before them; dry_temporal_dispatch ends the told part
   at every dispatch that may reach the upscale's stand-ins; vk_temporal.cpp renumbers where the noted draws are taken;
   vk_replay.cpp's replay() and replay_handle have the steps the fixture copies.

    python -I -B tests/test_stream_order.py [--cxx g++]
"""
import argparse
import os
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


def in_order(text, steps, what):
    at = 0
    for s in steps:
        i = text.find(s, at)
        check(i >= 0, "%s: %r is missing or out of order" % (what, s))
        at = i + len(s)


def check_draw(draw_src):
    body = function(draw_src, "void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {")
    in_order(body, ["if (!d.mesh && !topology(d.prim, topo)) {", "Targets at = bare_targets(s, targets(s));",
                    "if (!at.width || !at.height) SKIP_HERE();",
                    "const bool told = replay_hooks_told();", "if (!told && at.n && at.color[0] && at.depth) {",
                    "temporal_note_scene_draw(", "fsr3_inputs::note_scene_draw(", "cp = program_for(s,",
                    "const bool pre_now = !told && at.n && at.color[0] && temporal_pre_point(at.color[0], cp->key, &pre_late);",
                    "if (!told && !pre_retarget_on && at.n == 1 && at.color[0]) {", "temporal_pre_composite(",
                    "const char* skin = skin_check(s, p, d, at);"], "draw()")
    check(body.count("temporal_pre_point(") == 1 and body.count("temporal_note_scene_draw(") == 1,
          "draw() asks the hooks in more places now: read it again")
    skin = function(draw_src, "static const char* skin_check(const NvnState& s, const Program& p, const DrawArgs& d, const Targets& at) {")
    in_order(skin, ["k.stream_pos = replay_stream_pos();", "return temporal_note_skinned_draw(k);"], "skin_check")
    cap = function(draw_src, "static void push_capture(")
    in_order(cap, ["k.stream_pos = replay_stream_pos();", "temporal_capture_draw(k, &slot);"], "push_capture")
    dry = function(draw_src, "bool dry_temporal_draw(NvnState& s, int prim, bool mesh) {")
    in_order(dry, ["if (!mesh && !topology(prim, topo)) return false;", "const Targets at = bare_targets(s, targets(s));",
                   "if (!at.width || !at.height) return false;", "if (at.n && at.color[0] && at.depth) {",
                   "temporal_note_scene_draw(at.color[0], at.width, at.height, at.depth, s.ubo[0][0].addr, s.ubo[0][0].size);",
                   "fsr3_inputs::note_scene_draw(at.color[0], at.width, at.height, at.depth, s.ubo[0][0].addr, s.ubo[0][0].size);",
                   "if (!at.n || !at.color[0]) return false;", "return temporal_pre_point_dry(at.color[0], dry_draw_program, &q);"],
             "dry_temporal_draw")
    check("target_types(*q.at, types);" in draw_src and "target_types(at, types);" in body,
          "the dry pass and draw() no longer find the program from the same types")
    disp = function(draw_src, "bool dry_temporal_dispatch(NvnState& s) {")
    check("if (!cp || !cp->ready.load(std::memory_order_acquire) || cp->failed || cp->stages.empty()) return true;" in disp and
          "return cp->stages[0].info.uses_cooperative_matrix || gameplay_network(cp->key);" in disp,
          "dry_temporal_dispatch no longer ends the told part at every network and every program that did not translate")
    # what the upscale's stand-ins take: a network or a gameplay key; the stub: a program missing, failed, or a slow network
    for head in ("bool fsr3_stand_in(VkCommandBuffer cmd, const NvnState& s, const Program& p) {",
                 "bool temporal_stand_in(VkCommandBuffer cmd, const NvnState& s, const Program& p) {"):
        check("if (!gameplay_network(p.key) && !(st.info.uses_cooperative_matrix &&" in function(draw_src, head),
              "%s takes other dispatches now: dry_temporal_dispatch has to end the told part there too" % head[5:head.index("(")])
    dispatch = function(draw_src, "void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {")
    check("if ((!cp || cp->failed || slow_network) && upscale_stub(cmd, s, compute_code_key(s)))" in dispatch and
          "const bool slow_network = cp && !cp->failed && !cp->stages.empty() &&" in dispatch and
          dispatch.count("uses_cooperative_matrix") >= 2,
          "dispatch() reaches upscale_stub in other ways now: read dry_temporal_dispatch again")


def check_temporal(temporal):
    point = function(temporal, "bool temporal_pre_point(const Texture* color0, uint64_t program, bool* late) {")
    check("stream_order::pre_point_step(s.pre_candidate, s.pre_key, color0 == s.cam.color," in point,
          "temporal_pre_point no longer decides by stream_order.h's rule")
    motion = function(temporal, "int actor_motion(VkCommandBuffer cmd, const Camera& cam,")
    in_order(motion, ["std::vector<State::Skin> skins = std::move(s.skins);", "stream_order::renumber(skins,",
                      "for (const auto& k : skins) {"], "actor_motion")
    take = function(temporal, "std::vector<State::Captured> take_captures() {")
    check("stream_order::renumber(c," in take, "take_captures no longer puts the stored draws in stream order")


def check_replay(replay, fixture):
    loop = function(replay, "void replay(VkCommandBuffer& cmd, NvnState& s, const uint8_t* p, const uint8_t* end = nullptr) {")
    in_order(loop, ["if (dry && dry_tail_find) dry_tail_check(p, h, a, avail, s);", "switch (h.op) {",
                    "if (++dry_work % 64 == 0) par_points.push_back(", "} else {", "stream_work++;", "if (!worker) maybe_split(cmd);"],
             "replay()")
    tail = function(replay, "void dry_tail_check(")
    for op in ("NOP_DRAW:", "NOP_DRAW_IDX:", "NOP_DRAW_INDIRECT:", "NOP_DRAW_MESH:", "NOP_DISPATCH:", "NOP_DISPATCH_INDIRECT:"):
        check("case " + op in tail, "dry_tail_check does not ask the hooks at %s" % op[:-1])
    check("par_tail.p = p, par_tail.s = s, par_tail.ub = ub_updates.size(), par_tail.cond = par_conds.size(), par_tail.work = dry_work;" in tail,
          "dry_tail_check no longer keeps the state before the tail's first draw or dispatch")
    handle = function(replay, "const uint8_t* replay_handle(VkCommandBuffer& cmd, const uint8_t* h, bool may_stop = false) {")
    steps = ["const uint64_t cb = ++stream_cbs;", "(hooks && temporal_pre_pending())", "stream_cb = cb, stream_work = 0;",
             "dry_tail_find = hooks, par_tail_at = false;", "rmode = Mode::Dry;", "dry_tail_find = false;"]
    shared = ["    for (const size_t i : stream_order::cuts(par_points, until, per, most - (tail ? 1 : 0),\n",
              "        seg.back().end = at.p;\n        seg.push_back({at.p, nullptr, &at.s, at.ub, at.cond, at.work, cb, hooks});\n",
              "    if (tail) {\n        seg.back().end = par_tail.p;\n"
              "        seg.push_back({par_tail.p, nullptr, &par_tail.s, par_tail.ub, par_tail.cond, par_tail.work, cb, false, true});\n",
              "    if (seg.size() == 1 || (tail && seg.size() == 2)) {",
              "        for (size_t k = 0; k < seg.size(); k++) seg[k].order = &in_order, seg[k].index = k;"]
    in_order(handle, steps + [s.strip() for s in shared] + ["record_segment(c, j->g);"], "replay_handle")
    for s in shared:   # (the fixture's copy of replay_handle has them as they are)
        check(s in handle and s in fixture, "replay_handle and tests/stream_order_test.cpp differ at: " + s.strip()[:60])
    # (a segment taken once, by a helper thread or by the replay thread when it is the next to submit, as the fixture does it)
    in_order(handle, ["const auto run_job = [&seg_cv, &seg_mu](Job* j) {", "if (j->taken.exchange(true)) return;",
                      "run_job(j);", "for (auto& jp : jobs) {   // (in order, each as soon as it is done)",
                      "if (!init_lost && par_own_on() && !j->taken.exchange(true)) {", "run_job(j);",
                      "seg_cv.wait(l, [j] { return j->done.load(); });"], "replay_handle's jobs")
    check("if (taken[k].exchange(true)) return;" in fixture and "if (own_on && !taken[k].exchange(true)) run(k), own_recorded++;" in fixture,
          "tests/stream_order_test.cpp no longer takes the segments as replay_handle does")
    only = function(replay, "bool par_stream_order_only() {")
    check("const bool only = hooks && !par_temporal_on();" in only, "par_stream_order_only: RX_VK_PARALLEL_TEMPORAL=0 no longer keeps them off")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    draw_src = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    temporal = (VK / "vk_temporal.cpp").read_text(encoding="utf-8")
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    fixture = (ROOT / "tests" / "stream_order_test.cpp").read_text(encoding="utf-8")
    try:
        check_draw(draw_src)
        check_temporal(temporal)
        check_replay(replay, fixture)
    except ValueError as e:
        raise SystemExit("FAIL: a text this test reads moved (%s)" % e)
    start = temporal.index("    struct PrePoint {")
    prepoint = temporal[start:temporal.index("\n    };\n", start) + 8]
    start = temporal.index("bool temporal_pre_point(const Texture* color0, uint64_t program, bool* late) {")
    pre = temporal[start:temporal.index("\n}\n", temporal.index("bool temporal_pre_pending() {")) + 3]
    start = replay.index("struct ParStats {")
    seg = replay[start:end_of(replay, "int64_t record_segment(VkCommandBuffer& cmd, const Segment& g) {")]
    hooks = "\n".join(line for line in replay.splitlines()
                      if line.startswith("bool replay_hooks_told() {") or line.startswith("uint64_t replay_stream_pos() {"))
    check(hooks.count("\n") == 1, "replay_hooks_told and replay_stream_pos are no longer one line each: update this test")
    source = (fixture.replace("/* PREPOINT_STRUCT */", prepoint).replace("/* PRE_POINT */", pre)
              .replace("/* SEGMENT */", seg).replace("/* HOOKS */", hooks))
    with tempfile.TemporaryDirectory(prefix="stream-order-") as tmp:
        src, exe = Path(tmp) / "stream_order.cpp", Path(tmp) / "stream_order.exe"
        src.write_text(source, encoding="utf-8")
        built = subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter", "-I", str(VK),
                                str(src), "-o", str(exe)], capture_output=True, text=True)
        check(built.returncode == 0, "the fixture did not build:\n" + built.stderr[-4000:])
        env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_")}
        for case in ("steps", "order", "hole"):
            r = subprocess.run([str(exe), case], env=env, capture_output=True, text=True, timeout=600)
            print((r.stdout + r.stderr).strip())
            check(r.returncode == 0, "stream_order_test %s" % case)
    print("PASS: the temporal hooks in stream order with the helper threads: the same firsts and names as on the replay "
          "thread alone, and the sources as the fixture has them")


def end_of(text, head):
    start = text.index(head)
    return text.index("\n}\n", start) + 3


if __name__ == "__main__":
    main()
