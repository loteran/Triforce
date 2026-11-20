"""The frame trace's split of each frame (gpu/vk/frame_trace.h, vk_replay.cpp status_tick; RX_VK_FRAME_TRACE=1,
triforce_frames.csv). No game, no GPU.

The trace's first columns are the draws' parts; at the lab's Market they explained 7-21% of the frames over the 99th
percentile. The columns after them split every frame as the stall line splits a frame of 50 ms or more: the replay's own time,
the present's, the dispatches, the queue submits, the replay thread's waits for its helpers, what is outside them (the game's
own update), the game's fence waits, the captures on its threads, the waits for write-backs and their held part, the queue
submits beside a background import.

Compiles and runs the header (as many values as columns, in order; outside as the stall line's), and reads the source: the
trace's header line ends with the first columns and then frame_trace::kColumns; each line has the split after the first
values; the split is this frame's (every counter's last value kept); the lab's tools read the columns by name (the first
ones keep their names and order).

    python -I -B tests/test_frame_trace.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIRST = ("frame,ms,pipe_waits,pipe_wait_ms,left_out,translate_ms,uploads,upload_mb,textures,memsync_ms,pipelines_ms,"
         "uploads_ms,programs_ms,slot_wait_ms,gpu_behind_ms,draw_ms,desc_ms,cmds_ms,tex_lookup_ms,textures_gone,dummies,draws")


def check(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


def function(text, head):
    at = text.index(head)
    return text[at:text.index("\n}\n", at)]


def check_source():
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    tick = function(replay, "void status_tick() {")
    trace = tick[tick.index("static FILE* trace = [] {"):tick.index("// (Triforce, stall_watch.h) every frame's time into the 600-frame")]
    header = "".join(part for part in trace[trace.index('fprintf(f, "'):trace.index("frame_trace::kColumns);")].split('"')[1::2])
    check(header.replace("\\n", "") == FIRST + ",%s", "the trace's first columns moved or the split is not after them: " + header)
    for needle, why in (
            ("frame_trace::Split sp;", "no split"),
            ("sp.replay = dp(PROF_SUBMIT) + dp(PROF_PRESENT)", "the replay is not the stall line's (submit and present)"),
            ("sp.dispatch = dp(PROF_DISPATCH), sp.qsubmit = dp(PROF_QSUBMIT)", "the dispatches or the queue submits are missing"),
            ("sp.present = (present_ns - l_present_ns) / 1e6", "the present's own time is not this frame's"),
            ("sp.game_wait = (game_ns - l_game_ns) / 1e6", "the game's fence waits are not this frame's"),
            ("sp.captures = (cap_ns - l_cap_ns) / 1e6", "the captures are not this frame's"),
            ("sp.wb_wait = (wb_ns - l_wb_ns) / 1e6, sp.wb_held = (wb_held_ns - l_wb_held_ns) / 1e6",
             "the write-back waits or their held part are not this frame's"),
            ("sp.qs_bg = (qsbg_ns - l_qsbg_ns) / 1e6", "the queue submits beside a background import are not this frame's"),
            ("sp.par_wait = (par_wait - l_par_wait) / 1e6", "the replay thread's waits for its helpers are not this frame's"),
            ("const int64_t par_wait = par_st.wait_ns.load() - par_st.own_ns.load() - par_st.submit_ns.load();",
             "the replay thread's wait is not the parallel replay line's (its part less its own recording and the submits)"),
            ("frame_trace::line(sp, split, sizeof split);", "the split is not written"),
            ("l_present_ns = present_ns, l_game_ns = game_ns, l_cap_ns = cap_ns, l_wb_ns = wb_ns, l_wb_held_ns = wb_held_ns;",
             "the split's counters are not kept for the next frame"),
            ("l_qsbg_ns = qsbg_ns, l_par_wait = par_wait;", "the split's counters are not kept for the next frame")):
        check(needle in trace, "vk_replay.cpp status_tick's trace: " + why)
    # (the split read before the profile's last values are kept: dp() is this frame's)
    check(trace.index("frame_trace::line(sp, split, sizeof split);") < trace.index("std::memcpy(l_prof, prof_ns, sizeof l_prof);"),
          "the split's replay is read after the profile's values were kept for the next frame")
    # (the values: the first ones, then the split)
    check('%llu,%lld,%lld,%s\\n"' in trace, "a trace line does not end with the split")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("`RX_VK_FRAME_TRACE=1`" in dev and "`outside_ms`" in dev, "docs/development.md: RX_VK_FRAME_TRACE's columns are not described")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="frame-trace-") as tmp:
        exe = Path(tmp) / "frame-trace"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_frame_trace.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the frame trace's split of each frame")


if __name__ == "__main__":
    main()
