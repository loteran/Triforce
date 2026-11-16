"""A mesh fallback compile the driver never ends costs its program's draws, not the game.

RADV (Mesa 26.2.6) on an RX 580, a GPU without mesh shaders, never finished the compute pipelines of two mesh fallback
programs: its compiler's spiller (aco::spill, under fallback_compute_pipelines in gpu/vk/vk_draw.cpp) ran on, taking
about 1 GB a second, until the system killed the game, at every start, in the precompile or the intro movie after it.
The driver caches nothing it did not finish, so each start did it again. Now:

- fallback_compute_pipelines watches its compiles (slow_begin before guarded_compute), and a program the record
  (triforce_pipelines.bin.refused) names is not compiled: it is remembered as failed and its draws are left out;
- slow_watch records the programs in a long compile only when memory has run short, and writes the record before the
  game starts again (start_game_again), so the next start leaves them out; a compile that ends after all takes its
  program off the record (slow_end);
- start_game_again marks every file this process opened close-on-exec before it becomes the game again on Linux: the
  guest memory's memfd kept open across the execv would keep the gigabytes it holds;
- a light precompile that goes on without its stragglers ("nothing finished for 30 s") still writes the record of
  what it built (built_write), or every start built everything again;
- the light precompile's rule (under 20 GB of RAM) reads /proc/meminfo on Linux: it was Windows' alone.

The memory test itself (memory_left, memory_short) is compiled and run where a C++ compiler is found.

python -I -B tests/test_fallback_compile_watch.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRAW = ROOT / "gpu" / "vk" / "vk_draw.cpp"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, what):
    if not cond:
        raise SystemExit(what)


def structure(draw):
    body = block(draw, "bool fallback_compute_pipelines(Program& p) {")
    begin = body.find("if (slow_begin(p, watched)) {")
    create = body.index("guarded_compute(")
    need(0 <= begin < create, "fallback_compute_pipelines does not watch its compile before guarded_compute")
    refused = block(body, "if (slow_begin(p, watched)) {")
    need("failed.insert({p.key, p.variant});" in refused and "return false;" in refused,
         "a program the record names is compiled anyway, or not remembered as failed")
    need("~Watched() { slow_end(at); }" in body, "the watch does not end with the compile")

    watch = block(draw, "void slow_watch() {")
    short = watch.index("memory_short(free_mb, total_mb)")
    write = watch.index("slow_write_locked();")
    again = watch.index("start_game_again(n)")
    need(short < write < again, "slow_watch must record only when memory is short, and before the game starts again")
    need("_exit(1);" in watch[again:], "with no restart left, slow_watch does not end the game before the system does")

    end = block(draw, "void slow_end(std::list<SlowCompile>::iterator at) {")
    need("slow_refused.erase({at->key, at->variant})" in end, "a compile that ended after all stays on the record")

    begin_fn = block(draw, "bool slow_begin(const Program& p, std::list<SlowCompile>::iterator& at) {")
    need("slow_read_locked();" in begin_fn and begin_fn.index("slow_read_locked();") < begin_fn.index("slow_refused.count"),
         "slow_begin asks the record before reading it")

    again_fn = block(draw, "bool start_game_again(int n) {")
    linux = again_fn[again_fn.index("#else"):]
    need("FD_CLOEXEC" in linux and linux.index("FD_CLOEXEC") < linux.index("execv("),
         "start_game_again keeps this process's files open across execv (the guest memory with them)")
    need("TRIFORCE_FAULT_RESTARTS" in linux, "start_game_again does not count its restarts on Linux")
    fault = block(draw, "__declspec(noinline) void driver_fault_restart() {")
    need("start_game_again(n)" in fault, "driver_fault_restart does not start the game again through start_game_again")

    pre = block(draw, "void precompile_run(void (*progress)(size_t done, size_t total)) {")
    gone = block(pre, "if (gone_on) {")
    need("built_write(built_stamp, built);" in gone and "pre->graphics_done" in gone,
         "a light precompile that goes on without its stragglers writes no record of what it built")

    keeps = block(draw, "bool precompile_keeps_all() {")
    need("#else" in keeps and "/proc/meminfo" in keeps[keeps.index("#else"):],
         "precompile_keeps_all has no Linux rule: a 16 GB Linux PC keeps every pipeline")


FIXTURE = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
%(left)s
%(short)s
int main() {
    if (memory_short(1535, 8192) != true || memory_short(1536, 8192) != false) return 2;     // (1.5 GB on a small PC)
    if (memory_short(3000, 65536) != true || memory_short(4096, 65536) != false) return 3;   // (a sixteenth on a large one)
    uint64_t free_mb = 0, total_mb = 0;
    if (!memory_left(free_mb, total_mb) || !total_mb || free_mb > total_mb) return 4;
    std::printf("%%llu of %%llu MB\n", (unsigned long long)free_mb, (unsigned long long)total_mb);
    return 0;
}
'''


def compiled(draw):
    cxx = shutil.which("clang++") or shutil.which("g++")
    if not cxx or not sys.platform.startswith("linux"):
        print("memory test not compiled here (needs Linux and a C++ compiler)")
        return
    left = block(draw, "bool memory_left(uint64_t& free_mb, uint64_t& total_mb) {")
    short = block(draw, "bool memory_short(uint64_t free_mb, uint64_t total_mb) {")
    with tempfile.TemporaryDirectory() as d:
        src, exe = os.path.join(d, "fixture.cpp"), os.path.join(d, "fixture")
        Path(src).write_text(FIXTURE % {"left": left, "short": short}, encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-Wall", "-Werror", src, "-o", exe], check=True)
        r = subprocess.run([exe], capture_output=True, text=True)
        need(r.returncode == 0, "memory_left or memory_short gave a wrong answer (exit %d)" % r.returncode)
        print("memory free now: " + r.stdout.strip())


def main():
    draw = DRAW.read_text(encoding="utf-8")
    structure(draw)
    compiled(draw)
    print("a mesh fallback compile that never ends is recorded, and the game starts again without it: ok")


if __name__ == "__main__":
    main()
