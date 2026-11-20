"""What a draw does whose pipeline is not ready yet (gpu/vk/pipe_wait.h; vk_draw.cpp draw, graphics_pipeline, dispatch;
vk_jobs.cpp jobs_wait_for; vk_replay.cpp hold_step). NX2navi's pipeline-miss rules on Triforce's design; no game or GPU.

What must hold:
- pipe_wait.h, compiled and run (tests/pipe_wait_test.cpp): a pass with a colour target drawn for the first time is drawn
  once even where its depth target was drawn before (RX_VK_PIPE_SKIP_NEW), and kept as before without the rule; a
  full-screen draw is at most 6 vertices, direct, over half the pass; a frame's wait pool is spent across its draws and full
  again at the next frame; the link budget bounds a frame's links only when set; a draw into kept targets that writes no
  memory is left out, a full-screen one waits from the pool and then the last wait unless the hold takes its frame; with the
  defaults and no full-screen pool the choice is the rule before (left out where kept and writing nothing, else waited for);
  RX_VK_PIPE_ONESHOT_BUDGET waits from the pool and leaves out after; the worker builds a pipeline's cold libraries before
  its link and the whole build after, as a job of its own, and builds it whole at once when the link does not come;
- (jobs_wait_for, which a wait from a budget uses, has its own check: tests/test_jobs_wait_for.py)
- the source (read): draw() asks pipe_wait::choose with pass_kept and the frame's budgets, waits for all with jobs_wait,
  for a while with jobs_wait_for, and counts every draw it leaves out for a pipeline in the frame's count; a dispatch left out
  while its compute pipeline compiles is counted too; graphics_pipeline queues the libraries-first job with the entry's ready
  flag and the whole build after it without one, honours RX_VK_PIPE_FAST_WORKERS and RX_VK_PIPE_LINK_MS; hold_step takes the
  frame's count every present before it decides and gives the draws the hold's mode; the switches are in the development
  guide.

    python -I -B tests/test_pipe_wait.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

CASES = ["kept", "full", "pool", "choose", "frame", "libs-first"]


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


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    d = block(draw, "void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {")
    miss = d[d.index("if (!pe->ready.load(std::memory_order_acquire)) {"):d.index("const VkPipeline pl = pe->pipeline;")]
    need("vk_draw.cpp draw (a pipeline not ready)", miss,
         ["pipe_wait::choose(rules, kept, writes, full, held, pipe_pool().left(frame),", "pass_kept(at, pipe_skip_new())",
          "writes_memory(p)", "full_screen_draw(s, at, d)", "hold_takes_frame()", "wait_for(pe->ready, st_pipe_wait_us);",
          "pipe_wait_for(pe->ready, c.ns)", "return left_out_pending(), void(st_pending_skips++);",
          "(c.kind == pipe_wait::K_LAST ? pipe_last() : pipe_pool()).charge(frame, spent);"])
    if miss.count("st_pending_skips++") != 1 or "left_out_pending()" not in miss:
        raise SystemExit("vk_draw.cpp draw: a draw left out for its pipeline is not counted for the hold, or left out elsewhere")
    need("vk_draw.cpp draw (a program not translated, the old skip)", d,
         ["if (legacy_translation_skip && targets_persistent(at)) return left_out_pending(), void(st_pending_skips++);"])
    disp = block(draw, "void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {")
    compiling = disp[disp.index("its compute pipeline is still compiling"):]
    compiling = compiling[:compiling.index("return;")]
    need("vk_draw.cpp dispatch (left out while compiling)", compiling, ["st_pending_skips++;", "left_out_pending();"])
    need("vk_draw.cpp pass_kept", block(draw, "bool pass_kept(const Targets& at, bool new_colour) {"),
         ["pipe_wait::kept(c, std::min(at.n, 8), d, new_colour)", "first_render_frame", "texture_frame_no()"])
    need("vk_draw.cpp full_screen_draw", block(draw, "bool full_screen_draw(const NvnState& s, const Targets& at, const DrawArgs& d) {"),
         ["pipe_wait::full_screen(d.mesh, d.indirect != 0, d.count, d.instances, w * h, (uint64_t)at.width * at.height)",
          "s.scissor[2] > 0 && s.scissor[3] > 0"])
    need("vk_draw.cpp the switches", draw,
         ['getenv("RX_VK_PIPE_SKIP")', '"RX_VK_PIPE_FRAME_WAIT_MS", pipe_wait::kFrameWaitMs, 500',
          '"RX_VK_PIPE_LINK_MS", 0, 100', 'getenv("RX_VK_PIPE_ONESHOT_BUDGET")', 'getenv("RX_VK_PIPE_SKIP_NEW")',
          'getenv("RX_VK_PIPE_FAST_WORKERS")', 'getenv("RX_VK_HOLD_PIPELINES")'])
    need("vk_draw.cpp pipe_wait_for", block(draw, "bool pipe_wait_for(const std::atomic<bool>& ready, int64_t ns) {"),
         ["jobs_wait_for(ready,"])

    gp = block(draw, "PipeEntry* graphics_pipeline(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, "
                     "const uint8_t color_types[8]) {")
    need("vk_draw.cpp graphics_pipeline (the link budget)", gp,
         ["link_later = !linked && !gpl_skipped(p) && pipe_fast_workers() && !pipe_links().links(frame);",
          "if (!linked && !link_later) linked = gpl_link(p, s, at, topo, cold), st_links++;", "pipe_links().spent(frame, us * 1000);"])
    first = gp[gp.index("pipe_wait::libs_first<VkPipeline>("):]
    first = first[:first.index("return remember(e);")]
    need("vk_draw.cpp graphics_pipeline (libraries first)", first,
         ["gpl_build_cold(q, job->s, job->at, job->topo, i + 1, k);", "gpl_rules::linked(*job->e, l);",
          "jobs_push([job] { gpl_retire(gpl_rules::built(*job->e, build_graphics(*job->p, job->s, job->at, job->topo))); });",
          "gpl_retire(gpl_rules::built(*job->e, build_graphics(q, job->s, job->at, job->topo)));", "}, false, &e->ready);"])
    need("vk_draw.cpp graphics_pipeline (when)", gp,
         ["if (!e->ready.load(std::memory_order_relaxed) && pipe_fast_workers() && (link_later || cold[0] || cold[1])) {"])
    tail = gp[gp.index("pipe_wait::libs_first<VkPipeline>("):]
    in_order("vk_draw.cpp graphics_pipeline (libraries first, else the whole build first as before)", tail,
             ["pipe_wait::libs_first<VkPipeline>(", "return remember(e);", "    jobs_push([job] {\n        // (over a linked one"])

    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    step = block(replay, "bool hold_step() {")
    in_order("vk_replay.cpp hold_step (the pipelines' count taken every present, before the decision)", step,
             ["const int64_t left_out = draw_left_out_take();",
              "present_hold::decide(hold, bound, texture_frame_owed() > 0 || left_out > 0, now, present);",
              "draw_hold_mode(present_hold::mode(hold, bound, present + 1));"])
    take = block(draw, "int64_t draw_left_out_take() {")
    need("vk_draw.cpp draw_left_out_take", take, ["frame_left_out.exchange(0", "return hold_pipelines() ? n : 0;"])

    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    need("docs/development.md", dev, ["RX_VK_PIPE_FRAME_WAIT_MS", "RX_VK_PIPE_ONESHOT_BUDGET", "RX_VK_PIPE_SKIP_NEW",
                                      "RX_VK_PIPE_LINK_MS", "RX_VK_PIPE_FAST_WORKERS", "RX_VK_HOLD_PIPELINES"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="pipe-wait-") as tmp:
        tmp = Path(tmp)
        exe = tmp / "pipe-wait"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-pthread", "-I", str(ROOT / "gpu/vk"),
                        str(ROOT / "tests/pipe_wait_test.cpp"), "-o", str(exe)], check=True)
        for case in CASES:
            subprocess.run([str(exe), case], check=True, timeout=60)


if __name__ == "__main__":
    main()
