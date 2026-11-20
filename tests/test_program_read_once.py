"""The draws' own reads of the program cache with the shaders' lock let go (gpu/vk/program_read_once.h, vk_shader.cpp
program_cache_load and readahead_take; no game, no GPU).

A draw that met a program first and found it neither in memory nor read ahead read it from triforce_programs.bin holding
shader_mu, and waited for the read-ahead's worker holding it: the replay's helper threads queued behind it ("the program picks
waited 169.0 ms for the shaders' lock" at the Market's arrival, lab run r33). Now the read and the wait let the lock go.

Cuts vk_shader.cpp's readahead_choose_locked, readahead_take and program_cache_load out as they are and runs them in
tests/test_program_read_once.cpp with threads, against a program cache whose reads wait on the test and the shaders' lock as a
mutex that knows its owner, each scenario a process of its own:
- read: while a draw reads a program, another draw's pick goes on (it could not with the lock held), the reader does not hold
  the lock, and the record stays listed until the read is done;
- second: a second draw of the program being read waits for the first's result with the lock let go, finds its variants, and
  the record is read once;
- worker: a draw's wait for the read-ahead's worker lets the lock go, and it takes what the worker parked;
- release: a program's release (program_released_after_reads) waits until no pick has the lock let go; under its gate a draw
  reads under the lock, after it with the lock let go again;
- choose: the read-ahead does not choose a program a draw is reading;
- taken: a read whose records the precompile took meanwhile adds nothing the list has.
Then each fails with the change undone, one part at a time (the mutations below): RX_VK_PROGRAM_READ_UNLOCKED=0, the read under
the lock, no claim, the records taken off the list at the read's start, the worker's wait under the lock, the choice without
the check, the release's gate open. And reads the source for what needs the game: that program_pick and mesh_program_for hold
shader_mu whole around program_cache_load (the lock is let go only where the pick holds it), that a borrow does not take it,
that readahead_main parks nothing a draw is reading, that do_program_released goes through the gate, and that the switch is in
the development guide.

    python -I -B tests/test_program_read_once.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
SCENARIOS = ["read", "second", "worker", "release", "choose", "taken"]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def replaced(text, old, new):
    if text.count(old) != 1:
        raise SystemExit("the mutation's text is not there once: %r" % old)
    return text.replace(old, new)


# (what undoes the change, one part at a time: (what, the scenario that must fail, the part of vk_shader.cpp's functions or of
# the header changed, the text and its replacement; or an environment))
MUTATIONS = [
    ("RX_VK_PROGRAM_READ_UNLOCKED=0: the read under the lock, as before", "read", "env", "RX_VK_PROGRAM_READ_UNLOCKED", "0"),
    ("the read under the lock", "read", "functions", "draw_once.unlocked(read, &st_once_relock_ns);", "read();"),
    ("no claim: the second draw reads the program again", "second", "functions",
     "if (once && !draw_once.claim(key, &st_once_wait_ns)) {", "if (false) {"),
    ("the records taken off the list at the read's start", "read", "functions", "if (keep || once) {", "if (keep) {"),
    ("the wait for the worker under the lock", "worker", "functions", "if (read_unlocked() && draw_once.may_let_go()) {",
     "if (false) {"),
    ("the read-ahead chooses a program a draw is reading", "choose", "functions", " || draw_once.reading(k)) return false;",
     ") return false;"),
    ("the release's gate open", "release", "header", "gate_.wait(lk, [&] { return open_ == 0; });", "(void)lk;"),
    ("a release does not keep reads under the lock", "release", "header", "bool may_let_go() const { return closed_ == 0; }",
     "bool may_let_go() const { return true; }"),
    ("a read whose records were taken meanwhile keeps its variants", "taken", "functions", "if (listed || variants.empty()) {",
     "if (true) {"),
]


def function(text, head):
    at = text.index(head)
    return text[at:text.index("\n}\n", at)]


def check_source(shader):
    # (program_pick takes the lock and leaves the choice to program_pick_locked, which may start again with it held: the stages'
    # code read lazily, tests/test_stage_code_lazy.py)
    pick = function(shader, "const Program* program_pick(const NvnState& s, ProgramKind kind, const uint8_t color_types[8]) {")
    lock, call = pick.find("std::lock_guard lk(shader_mu);"), pick.find("return program_pick_locked(s, kind, color_types, cs);")
    locked = function(shader, "const Program* program_pick_locked(")
    if not 0 <= lock < call or "program_cache_load(key, variants);" not in locked or "shader_mu" in locked:
        raise SystemExit("vk_shader.cpp program_pick does not hold shader_mu whole around program_cache_load (program_pick_locked)")
    mesh = function(shader, "const Program* mesh_program_for(const NvnState& s, const uint8_t color_types[8]) {")
    if "std::lock_guard lk(shader_mu);" not in mesh or "program_cache_load(key, variants);" not in \
            function(shader, "const Program* mesh_program_locked("):
        raise SystemExit("vk_shader.cpp mesh_program_for does not hold shader_mu whole around program_cache_load")
    calls = shader.count("program_cache_load(key, variants")
    if calls != 3:   # (program_pick, mesh_program_locked, program_cache_borrow's keep)
        raise SystemExit("vk_shader.cpp: program_cache_load has a caller this test does not know (%d calls)" % calls)
    borrow = function(shader, "void program_cache_borrow(uint64_t key")
    if "shader_mu" in borrow or "program_cache_load(key, variants, true);" not in borrow:
        raise SystemExit("vk_shader.cpp program_cache_borrow: a borrow takes shader_mu or is not a keep")
    load_fn = block(shader, "void program_cache_load(")
    if "const bool once = !keep && read_unlocked();" not in load_fn:
        raise SystemExit("vk_shader.cpp program_cache_load: a borrow (keep) would let a lock go it does not hold")
    worker = function(shader, "void readahead_main() {")
    if "listed = disk_records.count(k) != 0 && !draw_once.reading(k);" not in worker:
        raise SystemExit("vk_shader.cpp readahead_main parks a program a draw is reading")
    after = function(shader, "void program_released_after_reads(const nvn_prog* p) {")
    if "program_read_once::Closed closed(draw_once);" not in after or "program_released(p);" not in after:
        raise SystemExit("vk_shader.cpp program_released_after_reads: the release does not go through the gate")
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    released = function(replay, "void do_program_released(const nvn_prog* p) {")
    if "program_released_after_reads(p);" not in released or "program_released(p);" in released.replace("_after_reads(p)", ""):
        raise SystemExit("vk_replay.cpp do_program_released does not wait for the picks' reads")
    if '#include "program_read_once.h"' not in (VK / "program_readahead.h").read_text(encoding="utf-8"):
        raise SystemExit("program_readahead.h does not include program_read_once.h (vk_shader.cpp's includes are in the stamp)")
    if "RX_VK_PROGRAM_READ_UNLOCKED" not in (ROOT / "docs/development.md").read_text(encoding="utf-8"):
        raise SystemExit("docs/development.md: RX_VK_PROGRAM_READ_UNLOCKED is not in the switches table")


def build(cxx, tmp, name, functions, header=None):
    d = Path(tmp) / name
    d.mkdir()
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    (d / "read_once_types.inc").write_text(block(shader, "struct DiskRecord {") + ";\n", encoding="utf-8")
    (d / "read_once_functions.inc").write_text(functions, encoding="utf-8")
    # (the read-ahead's headers beside the test's files, so that program_readahead.h's quoted include takes the copy here: a
    # mutated program_read_once.h when given)
    for name in ("program_readahead.h", "program_order.h", "program_read_once.h"):
        if (VK / name).exists():
            (d / name).write_text((VK / name).read_text(encoding="utf-8"), encoding="utf-8")
    if header is not None:
        (d / "program_read_once.h").write_text(header, encoding="utf-8")
    exe = d / "read-once.exe"
    r = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread", "-I" + str(d), "-I" + str(VK),
                        str(ROOT / "tests/test_program_read_once.cpp"), "-o", str(exe)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise SystemExit("the test did not compile (%s):\n%s" % (name, r.stdout))
    return exe


def run(exe, scenario, env=None):
    e = dict(os.environ)
    e.pop("RX_VK_PROGRAM_READ_UNLOCKED", None)
    e.update(env or {})
    return subprocess.run([str(exe), scenario], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120, env=e)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    check_source(shader)
    functions = "\n".join(block(shader, head) for head in ("void readahead_choose_locked() {", "bool readahead_take(",
                                                              "void program_cache_load("))
    header = (VK / "program_read_once.h").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="program-read-once-") as tmp:
        exe = build(args.cxx, tmp, "as-is", functions)
        for s in SCENARIOS:
            r = run(exe, s)
            if r.returncode:
                raise SystemExit("FAIL %s:\n%s" % (s, r.stdout))
            print("PASS " + r.stdout.strip().splitlines()[-1])
        for i, (what, scenario, part, old, new) in enumerate(MUTATIONS):
            if part == "env":
                r = run(exe, scenario, {old: new})
            else:
                f = replaced(functions, old, new) if part == "functions" else functions
                h = replaced(header, old, new) if part == "header" else None
                r = run(build(args.cxx, tmp, "mutant%d" % i, f, h), scenario)
            if r.returncode == 0:
                raise SystemExit("FAIL: the mutation survived (%s): scenario %s passed" % (what, scenario))
            print("PASS mutation caught: %s (%s: %s)" % (what, scenario, (r.stdout.strip().splitlines() or ["?"])[-1]))
    print("PASS: the draws' own reads let the shaders' lock go")


if __name__ == "__main__":
    main()
