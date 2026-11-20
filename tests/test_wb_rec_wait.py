"""A wait for a write-back still on the GPU, by its writer's recording (gpu/vk/wb_rec_wait.h, vk_replay.cpp vk_timeline_wait; the
handoff's §12.9). No game, no GPU.

The waits for write-backs (a capture's late pages on the game's submitting thread, a read or a binding of a page put off) waited
for the submissions' timeline, which finish_recording signals only as far as no older submission is stopped at an event wait:
20 to 46 ms at the lab's steady Market with the GPU 4 submissions behind. The recording the submission was finished in is waited
for first, on the recordings' timeline; the rest of the wait is counted as the timeline held, and RX_VK_WB_WAIT_REC=1 ends the
wait there (off by default).

Compiles and runs the header's rules (the switch, the finished submissions' table, the budget, the held part, the stall line's
words), and reads the source for what needs the game:
- submit_recording notes in fin_rec only the submissions finished in the recording (rec_open_values), never the recording's own
  value: a submission still being recorded in parts is not known, and its wait is the timeline's as before;
- (Triforce) and only at the submit that ends the recording: a part's (a split, the early tail's start, a parallel replay's
  setup during a burst) keeps them for it, as their write-backs are recorded at the recording's end (a burst in parts, below);
- vk_timeline_wait looks the value up under rec_map_mu, waits for the recording on rec_timeline first, waits for the timeline
  unless the switch ends the wait there, with what is left of the one bound, and counts the held part for the stall line;
- status_tick gives the held part to the stall line, and the switch is in the development guide.

    python -I -B tests/test_wb_rec_wait.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def check(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


def function(text, head):
    at = text.index(head)
    return text[at:text.index("\n}\n", at)]


def check_source():
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    submit = function(replay, "void submit_recording(")
    check("fin_rec.submitted(rec_open_values, seq, !part);" in submit,
          "submit_recording does not note the submissions finished in the recording at its end")
    check("fin_rec.note(" not in submit, "submit_recording notes in fin_rec past submitted (a part's, or a recording's own value?)")
    check("void submit_recording(Recording& r, bool signal_value = true, uint64_t signal_at = 0, bool part = false) {" in replay,
          "submit_recording has no part argument")
    check("::mkwvk::wb_rec_wait::Finished fin_rec;" in replay, "the finished submissions' table is missing")
    # (Triforce) the submits of a part of a recording that goes on: a split, the early tail's start, a parallel replay's setup.
    # Their write-backs are recorded at the recording's end; the submits that end it (finish_recording, a present's) finish them
    split = function(replay, "void maybe_split(VkCommandBuffer& cmd) {")
    check("submit_recording(*rec_cur, false, 0, true);" in split, "maybe_split's submit is not a part's")
    tail = function(replay, "void tail_begin(VkCommandBuffer& cmd) {")
    check("submit_recording(*rec_cur, false, 0, true);" in tail, "tail_begin's submit is not a part's")
    par = replay[replay.index("memory_sync_mirrors_before_segments();"):]
    check(par[:par.index("\n", par.index("submit_recording("))].count("submit_recording(r, false, 0, true);") == 1,
          "a parallel replay's setup submit is not a part's")
    check(sum(", 0, true)" in line for line in replay.splitlines() if "submit_recording(" in line) == 3,
          "a submit other than the three parts' is a part's")
    finish = function(replay, "void finish_recording(uint64_t value) {")
    check("submit_recording(r, signal, at);" in finish, "finish_recording's submit does not end the recording")
    check("submit_recording(r);   // (nothing is stopped at a present: drain)" in replay, "a present's submit does not end its recording")

    wait = function(replay, "void vk_timeline_wait(uint64_t v) {")
    for needle, why in (
            ("std::lock_guard mk(rec_map_mu);", "the table is read without its lock"),
            ("seq = fin_rec.of(v);", "the writer's recording is not looked up"),
            ("ri.pSemaphores = &rec_timeline;", "the recording is not waited for on the recordings' timeline"),
            ("if (!wr::ends_at_recording(wb_wait_rec_on(), seq != 0, rec_done)) {", "the switch does not decide the timeline's wait"),
            ("wr::budget_left(wr::kBoundNs, rec_ns)", "the timeline's wait does not keep to what is left of the bound"),
            ("wr::held_ns(seq != 0, rec_done, rec_ns, waited_ns)", "the held part is not counted"),
            ("stall_ct.wb_held_ns.fetch_add(", "the held part does not reach the stall line's counts"),
            ("wb_held_last_ns = held;", "the held part does not reach the capture's late line")):
        check(needle in wait, "vk_replay.cpp vk_timeline_wait: " + why)
    # (the recording first, then the timeline: the timeline cannot reach v before the recording that finished it)
    check(wait.index("ri.pSemaphores = &rec_timeline;") < wait.index("wi.pSemaphores = &timeline;"),
          "vk_timeline_wait waits for the timeline before the writer's recording")
    # (the flush of held submits before either wait: a batched submit holds both timelines' signals)
    check(wait.index("queue_flush_for(FR_VALUE);") < wait.index("ri.pSemaphores = &rec_timeline;"),
          "vk_timeline_wait waits for the recording before the held submits are flushed")
    check("wb_held_last_ns = 0;" in wait[:wait.index("if (!timeline) return;")],
          "the thread's last held part is not reset at a wait's start")
    on = function(replay, "static bool wb_wait_rec_on() {")
    check('::mkwvk::wb_rec_wait::on(getenv("RX_VK_WB_WAIT_REC"))' in on, "RX_VK_WB_WAIT_REC is not read")

    tick = function(replay, "void status_tick() {")
    for needle in ("stall_ct.wb_held.load(", "stall_ct.wb_held_ns.load(",
                   "f.wb_held = wb_held - s_wb_held, f.wb_held_ms = (wb_held_ns - s_wb_held_ns) / 1e6;",
                   "s_wb_held = wb_held, s_wb_held_ns = wb_held_ns;"):
        check(needle in tick, "vk_replay.cpp status_tick: the stall line's held part is not this frame's (%s)" % needle)

    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("`RX_VK_WB_WAIT_REC=1`" in dev, "docs/development.md: RX_VK_WB_WAIT_REC is not in the switches table")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="wb-rec-wait-") as tmp:
        exe = Path(tmp) / "wb-rec-wait"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_wb_rec_wait.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: a wait for a write-back by its writer's recording, the held part counted")


if __name__ == "__main__":
    main()
