"""The capture's long waits for late pages say which pages they were for (gpu/vk/vk_memory.cpp take_capture, late_slow_log).
No game, no GPU: the source read.

In the lab's steady Market about one take a minute waited 20 to 37 ms for its late pages on the game's submitting thread. Which
pages, and how far the GPU was behind, only RX_VK_LOG_LATE could say, and only for the first takes after 500. Now a take whose
wait took kLateSlowMs or more logs one line of them (40 lines, then every 100th), by pool kind, texture storage, and what of
each a submission still on the GPU writes back. What must hold:
- the wait is timed where it is made, from the GPU's progress read before it, and only a wait of kLateSlowMs or more logs;
- the log takes each of the locks it reads under (pools_mu, shadow_mu, wb_pages_mu) in a scope of its own, never two at once
  (their order elsewhere is not one this line should have to keep), and is bounded.

    python -I -B tests/test_late_slow_log.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


def block(text, head):
    start = text.find(head)
    check(start >= 0, "%s is missing" % head)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    mem = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    take = block(mem, "bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed) {")
    # ((Triforce) the lab's RX_VK_LAB_AB "nolatewait" knob may stand in the condition after done0's test)
    wait = take[re.search(r"if \(const uint64_t done0 = vk_timeline_completed\(\); v > done0( && !lab_ab::on\(\"nolatewait\"\))?\) \{",
                          take).start():]
    wait = wait[:wait.index("\n        }\n") + 10]
    for needle in ("vk_timeline_wait(v);", "st_tk_late += waited;", "st_tk_late_n++;", "if (waited >= kLateSlowMs)",
                   # ((Triforce, wp20) and the part of it after the writer's recording was done: this thread's wait's, wb_rec_wait.h)
                   "late_slow_log(*c, v, done0, waited, vk_timeline_wait_held_last() / 1e6);"):
        check(needle in wait, "take_capture's late wait no longer has: " + needle)
    check(wait.index("vk_timeline_wait(v);") < wait.index("vk_timeline_wait_held_last()"),
          "take_capture reads the held part of its wait before the wait")
    check(take.count("late_slow_log(") == 1, "take_capture logs a slow late wait in more than one place")
    check(re.search(r"constexpr double kLateSlowMs = [0-9.]+;", mem), "kLateSlowMs is missing")

    log = block(mem, "void late_slow_log(const Capture& c, uint64_t v, uint64_t done0, double ms, double held_ms) {")
    check("ms of the wait after the recording that finished it was done" in log and ", held_ms," in log,
          "late_slow_log's line does not say how much of the wait was the timeline held")
    check("if (nth > 40 && nth % 100) return;" in log, "late_slow_log is not bounded")
    # (each lock in a scope of its own: the scope's braces close before the next lock is taken)
    locks = [m.start() for m in re.finditer(r"std::lock_guard \w+\((pools_mu|shadow_mu|wb_pages_mu)\);", log)]
    check(len(locks) == 3, "late_slow_log reads under other locks than the three it is written for")
    for a, b in zip(locks, locks[1:]):
        between = log[a:b]
        check(between.count("{") < between.count("}") + 1 and "    }\n    {" in between,
              "late_slow_log holds two locks at once")
    check("logf(\"vk: late: a capture's wait for its late pages took" in log, "late_slow_log's line is missing")
    print("PASS: a long wait for late pages logs which pages, bounded, one lock at a time")


if __name__ == "__main__":
    main()
