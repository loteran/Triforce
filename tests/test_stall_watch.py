"""The stalls in the log (gpu/vk/stall_watch.h, vk_replay.cpp status_tick; no game, no GPU).

A benchmark's 0.1 % low of 6 fps means frames of over 150 ms, which the log's averages over 120 and 600 presents do not
show. Every 600 of the game's frames a line of their times (present to present, and the replay's own), and a line of where
the time of a frame over 50 ms went, at most three a second.

Compiles and runs the header's logic (the counts over 33, 50, 100 and 250 ms, the bound on the lines, the stall line's
text), and reads the source for what needs the game: that status_tick keeps both lines unless RX_VK_STALL_LOG=0, that the
counts the stall line reads are made where they are spent (the waits for write-backs in vk_timeline_wait, the presents'
blits on the queue, the game's fence waits, the present's own time), and that the switch is in the development guide.

    python -I -B tests/test_stall_watch.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def function(text, head):
    at = text.index(head)
    return text[at:text.index("\n}\n", at)]


def check_source():
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    tick = function(replay, "void status_tick() {")
    for needle, why in (
            ('getenv("RX_VK_STALL_LOG")', "the stall lines' switch is not read"),
            ("h_present.add(", "the present side's frame times are not counted"),
            ("h_replay.add(", "the replay side's frame times are not counted"),
            ("bound.take(", "the stall lines are not bounded"),
            ("sw::stall_line(", "no stall line"),
            ("sw::kEvery", "no 600-frame line of frame times"),
            ("memsync_part_ns[", "the stall line lacks memory sync's parts"),
            ("draw_stats3(", "the stall line lacks the waits for pipelines"),
            ("texture_stats(", "the stall line lacks the uploads"),
            ("memory_import_stats(&imp, &imp_ns);", "the stall line lacks the pools imported where bound"),
            ("program_cache_stats(&pr, &pr_ns, &pa);", "the stall line lacks the programs read by the draws"),
            # ((Triforce) the draws' reads with the shaders' lock let go: the waits for another draw's read, and the lock taken
            # again counted with the picks' waits for it)
            ("program_read_stats(&once_n, &once_ns, &relock_ns);", "the stall line lacks the draws' waits for another draw's read"),
            ("(pk_lock - s_pk_lock + relock_ns - s_relock_ns)", "the picks' waits to take the shaders' lock again are not counted"),
            # ((Triforce) why the draws read what they read, and the programs taken in the sessions' order)
            ("program_order_stats(&why_un, &why_go, &why_ahead, &by_order);", "the stall line lacks why the draws read programs"),
            # ((Triforce, wp18) the pipelines new to the draws and those linked ahead, this frame's)
            ("pipeline_ahead_stats(&pf, &pah, &por, &pln, &plb);", "the stall line lacks the pipelines new to the draws"),
            ("f.pipes_first = pf - s_pf, f.pipes_ahead = pah - s_pah, f.pipes_order = por - s_por;",
             "the stall line's pipelines are not this frame's"),
            ("s_pf = pf, s_pah = pah, s_por = por, s_pln = pln, s_plb = plb;", "the stall line's pipelines are not this frame's"),
            ("texture_uploads_later();", "the stall line lacks the uploads left for the next frame"),
            ("memory_import_churn(&given, &ahead_n);", "the stall line lacks the imports given back and sent ahead"),
            ("memory_capture_stats(&caps);", "the stall line lacks the submit-time captures on the game's threads"),
            ("f.captures = (cap_ns - s_cap_ns) / 1e6;", "the stall line's captures are not this frame's"),
            ("s_cap_ns = cap_ns;", "the stall line's captures are not this frame's"),
            ("stall_ct.qs_bg.load(", "the stall line lacks the queue submits beside a background import")):
        if needle not in tick:
            raise SystemExit("vk_replay.cpp status_tick: " + why)
    # ((Triforce, test9) a queue submit made while the background thread imports is counted, with its time)
    submit = function(replay, "void submit_recording(")
    at = submit.index("vkQueueSubmit2(dev.queue, 1, &si, r.fence);")
    if not (submit.index("const bool bg_import = memory_bg_importing() > 0;") < at < submit.index("stall_ct.qs_bg.fetch_add(1")):
        raise SystemExit("vk_replay.cpp submit_recording: a queue submit beside a background import is not counted around it")
    wait = function(replay, "void vk_timeline_wait(uint64_t v) {")
    if "stall_ct.wb_wait_ns" not in wait or "stall_ct.wb_behind_present" not in wait:
        raise SystemExit("vk_timeline_wait does not count its waits for the stall line")
    # (Triforce) whether the value waited for was on the queue when the wait began (the queue's top read before the flush)
    if "stall_ct.wb_unsent" not in wait or wait.index("const bool unsent = v > stall_ct.queue_top") > wait.index("queue_flush_for(FR_VALUE);"):
        raise SystemExit("vk_timeline_wait does not tell a wait for a submission not on the queue yet")
    # (and the uploads' command buffers timed, read only when one went to the queue)
    if "gpu_time_end(r, uploads);" not in function(replay, "void submit_recording(") or \
            "ts_up_pending[r.slot] = uploads;" not in function(replay, "void gpu_time_end(Recording& r, bool uploads) {"):
        raise SystemExit("vk_replay.cpp: the uploads' command buffers are not timed, or read when they were not submitted")
    present = function(replay, "void present_image(")
    if "stall_ct.blit_after[" not in present or "stall_ct.present_ns" not in present:
        raise SystemExit("present_image does not note its blit's place on the queue or its own time")
    if "t.nvn_wait = wait_counted;" not in replay:
        raise SystemExit("the game's fence waits are not counted (t.nvn_wait)")
    flush = function(replay, "void queue_flush_locked() {")
    submit = function(replay, "void submit_recording(")
    if "note_queue_top(" not in flush or "note_queue_top(" not in submit:
        raise SystemExit("the queue's highest timeline value is not noted at every submit")
    memory = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    at = memory.index("ok = import_pool(lo, hi, p);")
    if "st_imports_inline.fetch_add(" not in memory[at:at + 1200] or "st_import_ns.fetch_add(" not in memory[at:at + 1200]:
        raise SystemExit("vk_memory.cpp: the imports made where a pool is bound are not counted for the stall line")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "RX_VK_STALL_LOG" not in dev:
        raise SystemExit("docs/development.md: RX_VK_STALL_LOG is not in the switches table")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="stall-watch-") as tmp:
        exe = Path(tmp) / "stall-watch"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_stall_watch.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the frame times and the stall lines")


if __name__ == "__main__":
    main()
