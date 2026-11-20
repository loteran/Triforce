"""The game's memory imported ahead of its first binding (gpu/vk/import_ahead.h, vk_memory.cpp import_ahead_*; no game, no
GPU).

An import of a chunk of the game's memory is the driver's page locking, 8 to 17 ms on the owner's RTX 5090, made on the
replay's thread when a binding the GPU writes, or a texture's storage the GPU unswizzles, needs it: seven in one dispatch made
a 79 ms submit in the owner's test4 log. The chunks a run imports so are kept in triforce_imports_ahead.txt, and the next
runs import them on the background import thread as soon as the game's pools reach them. Compiles and runs the header (the
switch, the file, the chunks due), and reads the source for what needs the game: that the imports made on a binding's thread
are noted, that the pool table's refresh sends the chunks due to the background thread through the same in-flight list the
lookups wait on, at most 16 under way, and that the switch is in the development guide.

    python -I -B tests/test_import_ahead.py [--cxx g++]
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
    memory = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    if '#include "import_ahead.h"' not in memory:
        raise SystemExit("vk_memory.cpp does not include import_ahead.h")
    sync = function(memory, "void memory_sync() {")
    if sync.index("import_ahead_look();") < sync.index("guest_pools.swap(next);"):
        raise SystemExit("vk_memory.cpp memory_sync: the chunks are looked for before the new pool table is in")
    look = function(memory, "void import_ahead_look() {")
    for needle, why in (("std::lock_guard lk(bg_mu);", "the in-flight list is not locked"),
                        # ((Triforce, 11-15) through bg_send: in the list the lookups wait on, queued whole, and held for the
                        # host's question until it is in the table, tests/test_bg_import_held.py)
                        ("bg_send(r.lo, r.hi);", "no range is sent to the background thread through bg_send, or not whole"),
                        ("bg_ahead_ranges.push_back({r.lo, r.hi});", "a range sent is not known whole to the lookups' wait"),
                        ("if (bg_inflight.size() >= 16) break;", "more than 16 imports may be under way"),
                        ("imported_over(r.lo, r.hi)", "a range imported already is sent again"),
                        # ((Triforce, test9) once a session, and none while the RAM is low: the RAM's rule gives idle imports back)
                        ("if (ahead_sent_once.count(r.lo) ||", "a range given back for the RAM is sent again"),
                        ("ahead_sent_once.insert(r.lo);", "a range sent is not remembered as sent"),
                        ("if (memory_ram_low()) {", "ranges are sent while the PC's RAM is low")):
        if needle not in look:
            raise SystemExit("vk_memory.cpp import_ahead_look: " + why)
    if look.index("if (memory_ram_low()) {") > look.index("std::lock_guard lk(bg_mu);"):
        raise SystemExit("vk_memory.cpp import_ahead_look: the RAM is asked about after the ranges are looked at")
    send = function(memory, "void bg_send(uint64_t lo, uint64_t hi) {")
    if "bg_inflight.push_back(lo);" not in send or "bg_queue.push_back({lo, hi});" not in send:
        raise SystemExit("vk_memory.cpp bg_send: a range sent is not in the list the lookups wait on, or not queued whole")
    bg = function(memory, "void bg_main() {")
    if bg.index("bg_importing.fetch_add(1") > bg.index("import_pool(r.first, r.second, p);") or \
            bg.index("bg_importing.fetch_sub(1") < bg.index("import_pool(r.first, r.second, p);"):
        raise SystemExit("vk_memory.cpp bg_main: the import under way is not counted around it (the stall line's)")
    # ((Triforce, test8) a lookup waits for a range imported ahead that reaches into what it wants, not only one starting there,
    # and the background thread forgets it when it is in)
    if "for (const auto& [a, b] : bg_ahead_ranges)" not in function(memory, "Pool* pool_lookup(uint64_t guest, uint64_t size) {"):
        raise SystemExit("vk_memory.cpp pool_lookup: a binding inside a range imported ahead does not wait for it")
    if "std::erase_if(bg_ahead_ranges," not in function(memory, "void bg_main() {"):
        raise SystemExit("vk_memory.cpp bg_main: a range imported ahead stays under way")
    note = function(memory, "void import_ahead_note(uint64_t lo, uint64_t hi) {")
    if "import_ahead::add(import_ahead_list(), lo / chunk * chunk, (hi + chunk - 1) / chunk * chunk)" not in note:
        raise SystemExit("vk_memory.cpp import_ahead_note: the import's range is not noted whole")
    at = memory.index("ok = import_pool(lo, hi, p);")
    if "if (ok) import_ahead_note(lo, hi);" not in memory[at:at + 1500]:
        raise SystemExit("vk_memory.cpp pool_lookup: an import on the binding's own thread is not noted for the next runs")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "RX_VK_IMPORT_AHEAD" not in dev:
        raise SystemExit("docs/development.md: RX_VK_IMPORT_AHEAD is not in the switches table")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="import-ahead-") as tmp:
        exe = Path(tmp) / "import-ahead"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_import_ahead.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: chunks imported ahead of their first binding")


if __name__ == "__main__":
    main()
