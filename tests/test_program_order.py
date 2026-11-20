"""The programs read ahead in the order the last sessions first needed them (gpu/vk/program_order.h, vk_shader.cpp
program_cache_load, readahead_main, readahead_take; no game, no GPU).

The save's load-in frame read 96 to 108 programs from the program cache on the draws' threads (73 ms in the lab), though the
read-ahead had the pipeline notes' order: it begins with the load's first program and its shelf lets its oldest go. Now each
session writes the order it first needed its programs in beside the cache, and the next one's workers read in that order while
no draw asks them for anything, keeping those programs apart from the shelf until a draw takes them.

Compiles and runs tests/test_program_order.cpp (the header: the file whole or not at all, the merge with the sessions before,
the session's record and when it is written, the read-ahead in the order with its bound, the draws taking from it and what goes
far behind them, and a session as the lab's S1 whose load finds its programs kept), and reads the source for what needs the
game: that a draw's first need of a program goes into the order, that the workers read the order only when nothing is asked and
keep its programs apart from the shelf (which counts and lets go only its own), that a take moves the order's place and counts
for the stall line, that the order is read as the workers start and written by them through a file of its own, and that the
switch is in the development guide (tests/test_stall_watch.py: that the counts reach the stall line).

(Triforce, wp19) Kept until needed: the owner's test37 session took another path than the order's sessions (SDR against HDR
Native) and its load-in frame read 115 programs itself, "64 read ahead and let go before, 51 not read ahead yet". The test runs
that session from its numbers by the rules before (they drop those 64 and leave the 51 unread) and by the new ones (the load
finds all 115), and each part of the new rules undone in the header (MUTATIONS) must fail it. The source: the switch read before
the order is loaded, an ask's read the order names kept with it, the RAM short letting them go, the whole order said read once.

(Triforce) Passed by: an order recorded in HDR replayed in SDR leaves some 300 of its first 512 programs never drawn, and they
held the pipelines' warm's window for the session. The test takes none of the first 300 and the rest in order: the window
ahead of the draws must be the whole kKeptMax again once they are kBehind past them (212 before). The RAM short keeps the cold
marks of those still being read, and the cold are kept by place (next_warm without a look at each); each has its mutations.

    python -I -B tests/test_program_order.py [--cxx g++]
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
    shader = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    if '#include "program_order.h"' not in (ROOT / "gpu/vk/program_readahead.h").read_text(encoding="utf-8"):
        raise SystemExit("program_readahead.h does not include program_order.h (vk_shader.cpp's includes are in the stamp)")
    load = function(shader, "void program_cache_load(uint64_t key, std::vector<std::unique_ptr<Program>>& variants, bool keep")
    note = load.find("if (!keep && variants.empty() && order_on()) order_note(key);")
    if not 0 <= note < load.index("readahead_take(key, variants)"):
        raise SystemExit("vk_shader.cpp program_cache_load: a draw's first need of a program does not go into the session's order")
    if "if (!keep) readahead_why(key);" not in load:
        raise SystemExit("vk_shader.cpp program_cache_load: a draw's own read does not say why the read-ahead had not read it")
    worker = function(shader, "void readahead_main() {")
    for needle, why in (
            ("if (ra_pending.empty() && (!ra_asks.empty() || !ra_note_asks.empty())) readahead_choose_locked();",
             "the asks are not served first, or the choice runs with no ask"),
            ("if (!rewarm && ra_pending.empty()) ahead_choose_locked();", "the sessions' order is not read when nothing is asked"),
            ("if (order_on()) ra_cv.wait_for(lk, std::chrono::seconds(1), work);", "the workers do not wake to write the order"),
            ("if (order_on()) order_write_maybe();", "the session's order is not written"),
            ("ra_parked[k] = std::move(made), ra_ahead.read_done(k, true), st_order_read++;",
             "a program of the sessions' order is not kept apart from the shelf"),
            ("program_readahead::to_let_go(shelf_locked(), 1)", "the shelf counts the programs kept for the sessions' order")):
        if needle not in worker:
            raise SystemExit("vk_shader.cpp readahead_main: " + why)
    if worker.index("if (ra_pending.empty() && (!ra_asks.empty()") > worker.index("if (!rewarm && ra_pending.empty()) ahead_choose_locked();"):
        raise SystemExit("vk_shader.cpp readahead_main: the sessions' order before the asks")
    choose = function(shader, "void readahead_choose_locked() {")
    if "shelf_locked() >= program_readahead::kParkedMax" not in choose or "std::min(shelf_locked()," not in choose:
        raise SystemExit("vk_shader.cpp readahead_choose_locked: the shelf's room counts the programs kept for the sessions' order")
    ahead = function(shader, "void ahead_choose_locked() {")
    if "if (!ahead_more_locked()) return;" not in ahead or "ra_ahead.choose(wanted)" not in ahead or \
            "draw_once.reading(k)" not in ahead:
        raise SystemExit("vk_shader.cpp ahead_choose_locked: not bounded, or it chooses a program a draw is reading")
    more = function(shader, "bool ahead_more_locked() {")
    if "memory_ram_low()" not in more or "memory_ram_short()" not in more:
        raise SystemExit("vk_shader.cpp ahead_more_locked: the sessions' order is read with the RAM short")
    take = function(shader, "bool readahead_take(uint64_t key")
    if take.index("if (variants.empty()) ahead_met_locked(key);") > take.index("auto it = ra_parked.find(key);") or \
            "st_order_taken" not in take or "ra_parked.erase(it), ra_ahead.taken(key);" not in take:
        raise SystemExit("vk_shader.cpp readahead_take: a draw's first need does not move the sessions' order on, or a take is "
                         "not counted or stays kept")
    ask = function(shader, "void readahead_ask(uint64_t key, int64_t at) {")
    if ask.index("if (order_on()) order_load_locked();") > ask.index("std::thread(readahead_main).detach();"):
        raise SystemExit("vk_shader.cpp readahead_ask: the order is not read before the workers start")
    write = function(shader, "void order_write_maybe() {")
    for needle in ("program_order::merge(order_rec.keys, order_before)", 'path + ".part"', "MoveFileExA(part.c_str(), path.c_str(), "
                   "MOVEFILE_REPLACE_EXISTING)", "std::unique_lock fk(order_file_mu, std::try_to_lock);"):
        if needle not in write:
            raise SystemExit("vk_shader.cpp order_write_maybe lacks: " + needle)
    if '"triforce_program_order.bin"' not in function(shader, "std::string order_path() {"):
        raise SystemExit("vk_shader.cpp order_path: not triforce_program_order.bin beside the program cache")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "RX_VK_PROGRAM_ORDER" not in dev:
        raise SystemExit("docs/development.md: RX_VK_PROGRAM_ORDER is not in the switches table")
    # ((Triforce, wp19) kept until needed)
    load = function(shader, "void order_load_locked() {")
    if load.find("ra_ahead.keep_all = order_keep_all();") > load.find("ra_ahead.load(std::move(keys));") or \
            'program_order::on(getenv("RX_VK_PROGRAM_ORDER_KEEP"))' not in function(shader, "bool order_keep_all() {"):
        raise SystemExit("vk_shader.cpp order_load_locked: the order's rules are not set from RX_VK_PROGRAM_ORDER_KEEP before it loads")
    for needle, why in (
            ("} else if (listed && !made.empty() && order_on() && !memory_ram_short() && ra_ahead.keep(k)) {",
             "an ask's read of a program the order names is not kept with the order's"),
            ("if (order_on()) ahead_ram_let_go_locked();", "the RAM short does not let the order's kept programs go"),
            ("cold = ra_ahead.is_cold(k);", "a read beyond the pipelines' warm's window is not told cold"),
            ("if (!cold) pipelines_warm(made, at);", "a program read cold is warmed at its read"),
            ("if (const uint64_t w = ra_ahead.next_warm())", "a program read cold is never warmed"),
            ("k = w, made = std::move(it->second), ra_parked.erase(it), ra_reading.insert(k), rewarm = true;",
             "a program warmed later is not in hand as a read is (a draw could take it during its warm)"),
            ("if (listed && !made.empty() && ra_ahead.kept.count(k)) {", "a program warmed later is parked again though taken")):
        if needle not in worker:
            raise SystemExit("vk_shader.cpp readahead_main: " + why)
    if worker.index("ra_ahead.keep(k)") > worker.index("ra_park_order.push_back(k)"):
        raise SystemExit("vk_shader.cpp readahead_main: an ask's read the order names goes to the shelf first")
    short = function(shader, "void ahead_ram_let_go_locked() {")
    if "memory_ram_short()" not in short or "ra_ahead.let_go_all(gone);" not in short or "readahead_drop(" not in short:
        raise SystemExit("vk_shader.cpp ahead_ram_let_go_locked: the kept programs do not go when the RAM is short")
    if "RX_VK_PROGRAM_ORDER_KEEP" not in dev:
        raise SystemExit("docs/development.md: RX_VK_PROGRAM_ORDER_KEEP is not in the switches table")


# ((Triforce, wp19) each part of keeping until needed undone in program_order.h: test37's session, or the cold reads' window,
# must fail then. ((Triforce) where the draws are moves on again with keep_all, for the warm's count: the places behind the
# draws passed by is one now, and so is each part of that count, the RAM short's cold marks and the cold by place)
MUTATIONS = [
    ("the draws' place lets the kept go again", "        if (keep_all) {   // ((Triforce) nothing let go:",
     "        if (false) {   // ((Triforce) nothing let go:"),
    ("the bound of 512 again", "size_t max_kept() const { return keep_all ? kKeepAllMax : kKeptMax; }",
     "size_t max_kept() const { return kKeptMax; }"),
    ("an ask's read the order names not kept", "if (!keep_all || it == place.end() ||", "if (true || it == place.end() ||"),
    ("every read with the pipelines' warm (its window lost)", "if (keep_all && warmed() > kKeptMax) make_cold(k);",
     "if (false) make_cold(k);"),
    ("a program read cold never warmed", "        if (cold_at.empty() || warmed() >= kKeptMax) return 0;\n        for",
     "        if (true) return 0;\n        for"),
    ("the places behind the draws passed by again", "if (!keep_all && at < furthest - kBehind) continue;",
     "if (at < furthest - kBehind) continue;"),
    # ((Triforce) the warmed the draws went by hold the warm's window for the session (an HDR order replayed in SDR)
    ("the passed counted in the warm's window", "return all - std::min(cold.size() + passed.size(), all);",
     "return all - std::min(cold.size(), all);"),
    ("the kept behind the draws not passed", "at < furthest - kBehind; at++) pass(order[(size_t)at]);",
     "at < furthest - kBehind; at++) (void)order[(size_t)at];"),
    ("one read while the draws went by not passed", "if (keep_all && at < furthest - kBehind) pass(key);",
     "if (false) pass(key);"),
    ("an ask's read behind the draws not passed", "if (it->second < furthest - kBehind) pass(key);",
     "if (false) pass(key);"),
    # ((Triforce) the RAM short takes the cold marks of those chosen and being read: kept as warmed, with no pipelines made
    ("the RAM short lets every cold mark go", "        kept.clear(), passed.clear();\n",
     "        kept.clear(), passed.clear(), cold.clear(), cold_at.clear();\n"),
    # ((Triforce) the cold by place: one made cold not in it, or one no longer cold left in it
    ("one made cold not by place", "if (cold.insert(key).second && it != place.end()) cold_at",
     "if (cold.insert(key).second && it != place.end() && false) cold_at"),
    ("one no longer cold left by place", "it != place.end()) cold_at.erase({it->second, key});",
     "it != place.end()) (void)it;"),
]


def build(cxx, d, header):
    d.mkdir()
    (d / "program_order.h").write_text(header, encoding="utf-8")
    exe = d / "program-order.exe"
    # (d first: the test's quoted include takes the header there)
    subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(d), "-I" + str(ROOT / "gpu/vk"),
                    str(ROOT / "tests/test_program_order.cpp"), "-o", str(exe)], check=True)
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    header = (ROOT / "gpu/vk/program_order.h").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="program-order-") as tmp:
        exe = build(args.cxx, Path(tmp) / "as-is", header)
        subprocess.run([str(exe)], check=True)
        for i, (what, old, new) in enumerate(MUTATIONS):
            if header.count(old) != 1:
                raise SystemExit("the mutation's text is not in program_order.h once: " + what)
            r = subprocess.run([str(build(args.cxx, Path(tmp) / ("mutant%d" % i), header.replace(old, new)))],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
            if r.returncode == 0:
                raise SystemExit("FAIL: the mutation survived (%s)" % what)
            print("PASS mutation caught: %s (%s)" % (what, (r.stdout.strip().splitlines() or ["?"])[-1]))
    print("PASS: the programs read ahead in the sessions' order")


if __name__ == "__main__":
    main()
