"""Programs read ahead of their first draw from the program cache (gpu/vk/program_readahead.h, vk_shader.cpp
program_cache_load; no game, no GPU).

A program a draw meets first in a session was read from triforce_programs.bin on the draw's thread: the "programs" phase of
the owner's test4 slow submits, up to 53 ms. Now a worker reads the next records in the file's order after each such read and
keeps them made; the draw that comes to one takes it. Compiles and runs the header's choice of records and its asks, and reads
the source for what needs the game: that program_cache_load takes a record read ahead before it reads one and asks the worker
to read on after a read of its own and after a take, that the worker never takes shader_mu and takes ra_mu before disk_mu,
that a record read ahead stays listed until a draw takes it, and that the switch is in the development guide
(tests/test_stall_watch.py: that the counts reach the stall line).

    python -I -B tests/test_program_readahead.py [--cxx g++]
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
    if '#include "program_readahead.h"' not in shader:
        raise SystemExit("vk_shader.cpp does not include program_readahead.h")
    load = function(shader, "void program_cache_load(uint64_t key, std::vector<std::unique_ptr<Program>>& variants, bool keep")
    take, read = load.index("readahead_take(key, variants)"), load.index("program_cache_read(disk_in, &disk_mu, key, recs, variants);")
    if not take < read:
        raise SystemExit("vk_shader.cpp program_cache_load: a record read ahead is not taken before the draw reads one")
    if load.count("readahead_ask(") != 2:
        raise SystemExit("vk_shader.cpp program_cache_load: the worker is not asked to read on after a take and after a read")
    if "if (!keep && readahead_take(" not in load:
        raise SystemExit("vk_shader.cpp program_cache_load: a borrow (keep) takes a program read ahead")
    worker = function(shader, "void readahead_main() {")
    if "shader_mu" in worker:
        raise SystemExit("vk_shader.cpp readahead_main takes shader_mu: the draw that waits for it holds that lock")
    if "disk_records.erase" in worker:
        raise SystemExit("vk_shader.cpp readahead_main takes records off the list: the other readers would miss them")
    if worker.index("std::unique_lock lk(ra_mu);") > worker.index("std::lock_guard dk(disk_mu);"):
        raise SystemExit("vk_shader.cpp readahead_main: disk_mu before ra_mu")
    if "program_cache_read(in, nullptr," not in worker:
        raise SystemExit("vk_shader.cpp readahead_main does not read with a handle of its own")
    take_fn = function(shader, "bool readahead_take(uint64_t key")
    if "wait_for(" not in take_fn or "kWaitMs" not in take_fn:
        raise SystemExit("vk_shader.cpp readahead_take: a draw's wait for a record being read is not bounded")
    # ((Triforce, test16) the notes' order: an ask after a program the notes name goes by them; the choice reads the notes' order
    # first; a full shelf lets its oldest go (not with the notes off: as before); several workers; the precompile hands the order)
    ask = function(shader, "void readahead_ask(uint64_t key, int64_t at) {")
    for needle in ("program_readahead::index_of(ra_note_index, key)", "program_readahead::ask(ra_note_asks, note);",
                   "program_readahead::ask(ra_asks, at);", "program_readahead::kWorkers"):
        if needle not in ask:
            raise SystemExit("vk_shader.cpp readahead_ask lacks: " + needle)
    # ((Triforce) the shelf: the programs parked but those kept for the sessions' order, which it neither counts nor lets go:
    # vk_shader.cpp shelf_locked, tests/test_program_order.py)
    choose = function(shader, "void readahead_choose_locked() {")
    if "by_notes ? ra_note_order : ra_order" not in choose or \
            "if (!readahead_notes() && shelf_locked() >= program_readahead::kParkedMax) return;" not in choose:
        raise SystemExit("vk_shader.cpp readahead_choose_locked: not the notes' order first, or the old full-shelf rule lost")
    if "program_readahead::to_let_go(shelf_locked(), 1)" not in worker or "ra_park_order.push_back(k)" not in worker:
        raise SystemExit("vk_shader.cpp readahead_main: a full shelf does not let its oldest go")
    shelf = function(shader, "size_t shelf_locked() {")
    if "ra_parked.size() - std::min(ra_parked.size(), ra_ahead.kept.size())" not in shelf:
        raise SystemExit("vk_shader.cpp shelf_locked: not the parked programs but those kept for the sessions' order")
    notes_fn = function(shader, "void program_readahead_notes(const std::vector<uint64_t>& note_programs) {")
    if "program_readahead::notes_order(note_programs)" not in notes_fn:
        raise SystemExit("vk_shader.cpp program_readahead_notes does not make the notes' order")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    pre = function(draw, "void precompile_run(void (*progress)(size_t done, size_t total)) {")
    at_notes = pre.index("program_readahead_notes(progs);")
    if not (pre.index("std::vector<std::unique_ptr<PipeNote>> notes = pipe_notes_read(&note_at);") < at_notes <
            pre.index("std::erase_if(notes,")):
        raise SystemExit("vk_draw.cpp precompile_run: the notes' order is not handed over whole, before the built ones are taken out")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "RX_VK_PROGRAM_READAHEAD" not in dev:
        raise SystemExit("docs/development.md: RX_VK_PROGRAM_READAHEAD is not in the switches table")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="program-readahead-") as tmp:
        exe = Path(tmp) / "program-readahead"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_program_readahead.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: programs read ahead of their first draw")


if __name__ == "__main__":
    main()
