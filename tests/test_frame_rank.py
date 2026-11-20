"""scripts/frame_rank.py: what made the slowest frames, from a per-frame trace (no game: authored traces).

A run of 10 ms frames with two kinds of slow ones: 10 frames of 25 ms whose time over the median is all outside the replay (the
game's update), and 20 of 16 ms whose time over it is memory sync in the replay, a write-back wait, most of it the timeline held.
What must hold: the cheats' teleport in log.txt cuts the run into the load (its first seconds) and the Market stay; the frames
over 1.5x the median are split by part, each part's share of their time over the median frame; the 99th percentile's frames are
the slowest ones alone; a trace without the split columns (an older build's) is split by the draws' parts and the rest.

    python -I -B tests/test_frame_rank.py
"""
import importlib.util
import io
import os
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

COLS = ("frame,ms,pipe_waits,pipe_wait_ms,left_out,translate_ms,uploads,upload_mb,textures,memsync_ms,pipelines_ms,uploads_ms,"
        "programs_ms,slot_wait_ms,gpu_behind_ms,draw_ms,desc_ms,cmds_ms,tex_lookup_ms,textures_gone,dummies,draws,"
        "replay_ms,present_ms,dispatch_ms,qsubmit_ms,par_wait_ms,outside_ms,game_wait_ms,captures_ms,wb_wait_ms,wb_held_ms,qs_bg_ms")


def check(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


def load():
    spec = importlib.util.spec_from_file_location("frame_rank", ROOT / "scripts" / "frame_rank.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def frame(no, ms, memsync=0.5, outside=3.8, wb=0.0, held=0.0, split=True):
    replay = ms - 0.2 - outside
    first = [no, ms, 0, 0, 0, 0, 0, 0, 100, memsync, 0.1, 0, 0.4, 0, 0, 4.0, 1.0, 1.2, 0.05, 0, 0, 3000]
    rest = [replay, 0.2, 0.3, 0.1, 0.5, outside, 0.2, 1.1, wb, held, 0]
    return ",".join("%g" % v for v in first + (rest if split else []))


def write_run(folder, split=True):
    lines = [COLS if split else COLS[:COLS.index(",replay_ms")]]
    no = 1
    for i in range(2000):
        if 800 <= i < 1600 and i % 80 == 0:      # (10 frames: the game's update)
            lines.append(frame(no, 25.0, outside=18.8, split=split))
        elif 800 <= i < 1600 and i % 40 == 20:   # (20 frames: memory sync, a write-back wait the timeline held)
            lines.append(frame(no, 16.0, memsync=6.5, wb=6.0, held=5.0, split=split))
        else:
            lines.append(frame(no, 10.0, split=split))
        no += 1
    (folder / "frames.csv").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (folder / "log.txt").write_text("[hos host] vk: 0 presents, 60.0 fps; a frame: 3000 draws\n"
                                    "[hos BaseProcExecutor0] cheats: teleport child to Market_Child at Entrance\n", encoding="utf-8")


def share(text, section, label):
    """The share of label's line in the section after the line that starts with section"""
    part = text[text.index(section):]
    for line in part.splitlines()[1:]:
        if line.strip().startswith("frames over"):
            break
        if label in line:
            return float(line.split()[-1].rstrip("%"))
    raise SystemExit("FAIL: no line %r in %r:\n%s" % (label, section, text))


def main():
    fr = load()
    with tempfile.TemporaryDirectory(prefix="frame-rank-") as tmp:
        run = Path(tmp) / "run"
        run.mkdir()
        write_run(run)
        out = io.StringIO()
        fr.main([str(run)], out)
        text = out.getvalue()
        check("=== market:" in text and "=== loads:" in text, "the teleport did not cut the run into the load and the stay:\n" + text)
        check("=== run:" not in text, "a run with a teleport is reported whole")
        # (the load: the frames before 6 s after the teleport's present + 120, at 10 ms a frame: 599, the 600th ends at 6 s)
        check("=== loads: 599 frames" in text, "the load is not the stay's first 6 s:\n" + text)
        sec = "frames over 1.5x the median: 30"
        check(sec in text, "the frames over 1.5x the median are not the 30 slow ones:\n" + text)
        check(abs(share(text, sec, "outside (the game's update)") - 55.6) < 0.1, "the game's update's share is wrong:\n" + text)
        check(abs(share(text, sec, "    replay") - 44.4) < 0.1, "the replay's share is wrong:\n" + text)
        check(abs(share(text, sec, "in replay: memory sync") - 44.4) < 0.1, "memory sync's share is wrong:\n" + text)
        check(abs(share(text, sec, "across: write-back waits") - 44.4) < 0.1, "the write-back waits' share is wrong:\n" + text)
        check(abs(share(text, sec, "across: of them the timeline held") - 37.0) < 0.1, "the held part's share is wrong:\n" + text)
        check(share(text, sec, "in replay: dispatch") == 0.0, "a part that did not move has a share:\n" + text)
        # (the 99th percentile's: of 1282 Market frames the 99th percentile is a 16 ms one, and the ten over it are the game's update)
        p99 = text[text.index("frames over the 99th percentile"):]
        check(p99.startswith("frames over the 99th percentile: 10,"), "the p99 frames are not the slowest 1%:\n" + text)
        check(abs(share(text, "frames over the 99th percentile", "outside (the game's update)") - 100.0) < 0.1,
              "the slowest frames are not the game's update:\n" + text)

        old = Path(tmp) / "old"
        old.mkdir()
        write_run(old, split=False)
        out = io.StringIO()
        fr.main([str(old / "frames.csv"), "--last-s", "10"], out)
        text = out.getvalue()
        check("=== run: " in text, "a trace file alone is not one window")
        check("the rest (the game's update, submits, present)" in text and "outside (the game's update)" not in text,
              "an older trace is not split by its own parts:\n" + text)
        # (the window's frames over its rate: its seconds, the last 10 and the frame that reached them)
        head = text[text.index("=== run: "):].split(";")[0].split()
        n, fps = int(head[2]), float(head[4])
        check(10.0 <= n / fps < 10.03, "--last-s does not keep the run's last seconds:\n" + text)
    print("PASS: frame_rank splits the slowest frames by part, per window")


if __name__ == "__main__":
    main()
