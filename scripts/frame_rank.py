"""What made the slowest frames, from the per-frame trace (RX_VK_FRAME_TRACE=1: triforce_frames.csv; the lab's runs keep it as
frames.csv beside log.txt). Standard library only.

    python -I scripts/frame_rank.py <run folder or csv> [...] [--skip-s 6] [--last-s 0]

For each window the frame times (median, 99th percentile, the overlay's 1% low: 1000 / p99, and the mean of the slowest 1% as a
rate), then the frames over the 99th percentile and those over 1.5x the median: their time over the median frame, split by what
each part took over its own median. A run folder with log.txt is cut where the cheats' teleports came (the lab's scenarios):
"market" the last stay, "tour areas" the stays before it, each after its first --skip-s seconds, and "loads" those first
seconds; a csv alone (or a log without teleports) is one window, "run", after its first --skip-s seconds, or its last --last-s.
Several runs: their windows summed by name.

The split, with a trace that has frame_trace.h's columns (gpu/vk/frame_trace.h): the frame is the replay (the submits and the
present phase, every thread that replays), the present's own time, and what is outside them: the game's own update. Within the
replay: memory sync, the dispatches, the queue submits, the waits for a recording slot and for the GPU, the replay thread's waits
for its helpers' segments, and the draws (their parts sampled and summed over every thread that records: with the helper threads
on, more than their share of the replay's time). Within outside: the game's fence waits and the captures at its submits. Across
both: the waits for write-backs still on the GPU, their part after the writer's recording was done (the timeline held), the
queue submits beside a background import. An older trace: the draws' parts, memory sync, the slot and GPU waits, and the rest.
"""
import argparse
import csv
import os
import re
import statistics
import sys

# (name, column, the part it is in) - the wall split first: replay, present and outside add up to the frame
SPLIT = [("replay", "replay_ms", None), ("present", "present_ms", None), ("outside (the game's update)", "outside_ms", None),
         ("memory sync", "memsync_ms", "replay"), ("dispatch", "dispatch_ms", "replay"), ("queue submit", "qsubmit_ms", "replay"),
         ("slot wait", "slot_wait_ms", "replay"), ("the GPU behind", "gpu_behind_ms", "replay"),
         ("helpers' segments waited for", "par_wait_ms", "replay"), ("draws (all threads)", "draw_ms", "replay"),
         ("game's fence waits", "game_wait_ms", "outside (the game's update)"),
         ("captures at its submits", "captures_ms", "outside (the game's update)"),
         ("write-back waits", "wb_wait_ms", "across"), ("of them the timeline held", "wb_held_ms", "across"),
         ("queue submits beside an import", "qs_bg_ms", "across")]
OLD = [("draws (all threads)", "draw_ms", None), ("memory sync", "memsync_ms", None), ("slot wait", "slot_wait_ms", None),
       ("the GPU behind", "gpu_behind_ms", None)]
DRAW = [("programs", "programs_ms"), ("pipelines", "pipelines_ms"), ("descriptors", "desc_ms"), ("texture lookups", "tex_lookup_ms"),
        ("uploads", "uploads_ms"), ("commands", "cmds_ms")]


def read_rows(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                rows.append({k: float(v) for k, v in r.items() if k})
            except (TypeError, ValueError):
                pass   # (a line cut at the end of a run)
    return rows


def teleports(log):
    """The presents at which the cheats' teleports were asked, in order (the lab's scenario_report.ps1 takes them so)"""
    marks, last = [], 0
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r"vk: (\d+) presents,", line)
            if m:
                last = int(m.group(1))
                continue
            if re.search(r"cheats: teleport (?:child|adult) to \S+", line):
                marks.append(last)
    return marks


def windows(rows, marks, skip_s, last_s):
    out = {}
    if not rows:
        return out
    if not marks:
        if last_s > 0:
            t, k = 0.0, len(rows)
            while k > 0 and t < last_s * 1000.0:
                k -= 1
                t += rows[k]["ms"]
            out["run"] = rows[k:]
        else:
            t, w = 0.0, []
            for r in rows:
                t += r["ms"]
                if t >= skip_s * 1000.0:
                    w.append(r)
            out["run"] = w
        return out
    f0 = int(rows[0]["frame"])
    for i, pres in enumerate(marks):
        a = max(0, pres + 120 - f0)   # (the trace's frame is the present; scenario_report.ps1's 120)
        b = min(len(rows), marks[i + 1] + 120 - f0) if i + 1 < len(marks) else len(rows)
        t = 0.0
        for j in range(a, b):
            t += rows[j]["ms"]
            name = "loads" if t < skip_s * 1000.0 else "market" if i + 1 == len(marks) else "tour areas"
            out.setdefault(name, []).append(rows[j])
    return out


def pct(sorted_values, q):
    return sorted_values[min(len(sorted_values) - 1, int(q * len(sorted_values)))]


def split_of(window, slow, parts):
    """Each part's time over its median in the slow frames, and the slow frames' time over the median frame"""
    med = statistics.median(r["ms"] for r in window)
    meds = {col: statistics.median(r.get(col, 0.0) for r in window) for _, col, *_ in parts}
    excess = sum(r["ms"] - med for r in slow)
    return excess, {col: sum(max(0.0, r.get(col, 0.0) - meds[col]) for r in slow) for _, col, *_ in parts}


def report(name, windows_by_run, out):
    frames = [r for w in windows_by_run for r in w]
    if len(frames) < 100:
        return
    has_split = all("outside_ms" in w[0] for w in windows_by_run if w)
    parts = SPLIT if has_split else OLD
    ms = sorted(r["ms"] for r in frames)
    med, p99 = statistics.median(ms), pct(ms, 0.99)
    slowest = ms[-max(1, len(ms) // 100):]
    out.write("=== %s: %d frames, %.1f fps; median %.2f ms, p99 %.2f ms; 1%% low %.0f fps (1000 / p99), the slowest 1%% at %.0f fps\n"
              % (name, len(ms), 1000.0 * len(ms) / sum(ms), med, p99, 1000.0 / p99, 1000.0 / statistics.mean(slowest)))
    for sel in ("p99", "1.5x"):
        excess, total = 0.0, {}
        n = 0
        for w in windows_by_run:   # (each run's own medians and thresholds: runs differ by up to 10%)
            if len(w) < 100:
                continue
            wm = sorted(r["ms"] for r in w)
            thr = pct(wm, 0.99) if sel == "p99" else 1.5 * statistics.median(wm)
            slow = [r for r in w if r["ms"] > thr]
            n += len(slow)
            e, s = split_of(w, slow, parts + [(None, c) for _, c in DRAW])
            excess += e
            for k, v in s.items():
                total[k] = total.get(k, 0.0) + v
        out.write("  frames over %s: %d, %.0f ms over the median frame in all\n" % ("the 99th percentile" if sel == "p99" else
                                                                                    "1.5x the median", n, excess))
        if excess <= 0:
            continue
        share = lambda col: 100.0 * total.get(col, 0.0) / excess
        if not has_split:
            rest = excess - sum(total[c] for _, c, _ in parts)
        for label, col, within in parts:
            indent = "    " if within is None else "      "
            pre = "" if within is None else ("across: " if within == "across" else "in %s: " % within.split(" (")[0])
            out.write("%s%-44s %7.0f ms %5.1f%%\n" % (indent, pre + label, total.get(col, 0.0), share(col)))
            if col == "draw_ms":
                for dl, dc in DRAW:
                    out.write("        %-42s %7.0f ms %5.1f%%\n" % ("in draws: " + dl, total.get(dc, 0.0), share(dc)))
        if not has_split:
            out.write("    %-44s %7.0f ms %5.1f%%\n" % ("the rest (the game's update, submits, present)", rest, 100.0 * rest / excess))


def main(argv=None, out=sys.stdout):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("paths", nargs="+", help="run folders (frames.csv, log.txt) or trace files")
    p.add_argument("--skip-s", type=float, default=6.0, help="seconds at each stay's start (or the run's) left out: the load")
    p.add_argument("--last-s", type=float, default=0.0, help="a run with no teleports: its last seconds only")
    a = p.parse_args(argv)
    by_name = {}
    for path in a.paths:
        csv_path, log = path, None
        if os.path.isdir(path):
            csv_path = next((os.path.join(path, n) for n in ("frames.csv", "triforce_frames.csv")
                             if os.path.isfile(os.path.join(path, n))), None)
            log = os.path.join(path, "log.txt") if os.path.isfile(os.path.join(path, "log.txt")) else None
            if not csv_path:
                out.write("(%s: no frame trace)\n" % path)
                continue
        rows = read_rows(csv_path)
        for name, w in windows(rows, teleports(log) if log else [], a.skip_s, a.last_s).items():
            by_name.setdefault(name, []).append(w)
    for name in ("market", "tour areas", "loads", "run"):
        if name in by_name:
            report(name, by_name[name], out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
