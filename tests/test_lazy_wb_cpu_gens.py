"""The lazy write-back's rule 1 asks the CPU's writes alone (gpu/vk/vk_memory.cpp cpu_gens, lazy_page_takes; no game, no GPU).

A page of a write-back may be left in its mirror only while the mirror has every CPU write to it: the CPU's newest write
generation for the page no newer than its pool's synced_gen. The renderer's one table of write generations (gens) holds
the GPU's writes too: every storage buffer a draw or a dispatch writes is stamped while it is recorded
(memory_stamp_gpu), and synced_gen is raised only at the submission's end, so every page being written back looked newer
than its sync and none was ever owed (the owner's first session with the row on: 0 pages watched, 0 owed). cpu_gens holds
the CPU's stamps alone, by the same generations.

vk_memory.cpp's own cpu_gens, stamp_page, memory_stamp_gpu, memory_stamp and lazy_page_takes are cut out and run on
write_gen_table.h with stand-ins for what they call:
- a page the GPU wrote after the pool's sync, and the CPU did not, is taken; the same for a whole 2 MB region;
- a page the CPU wrote after the sync is not, whether a collection or a capture stamped it (stamp_page) or the replay's
  own write (memory_stamp); after the next sync it is;
- a page with an event word is not;
- with the lazy write-back off (no cpu_gens) nothing is taken, and gens is the same either way: the same generations,
  the same answer for every page, so nothing else that reads it changes meaning.
Then the same against a copy whose lazy_page_takes asks gens, as before: it has to fail, by the GPU-written page.

    python -I -B tests/test_lazy_wb_cpu_gens.py [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def cut(text, start, end_marker="\n}\n"):
    at = text.index(start)
    return text[at:text.index(end_marker, at) + len(end_marker)]


FIXTURE = r'''#include "write_gen_table.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>
namespace mkwvk {
using write_gen::kPageShift;
write_gen::Table gens;
inline WriteGen next_gen() { return gens.next(); }
inline WriteGen page_gen(uint64_t p) { return gens.page(p); }
thread_local uintptr_t tl_stamp_who;
void stamp_src_note(WriteGen, uintptr_t) {}
void memory_drop_shadow(uint64_t, uint64_t) {}
void cap_view_bump() {}
void logf(const char*, ...) {}
bool gpu_pages_on() { return false; }
std::mutex gpu_pages_mu;
std::unordered_map<uint32_t, WriteGen> gpu_pages;
void memory_stamp(uint64_t guest, uint64_t size);
@CPU_GENS@
@STAMP_GPU@
@STAMP@
@TAKES@
}  // namespace mkwvk
using namespace mkwvk;
int failures = 0;
#define CHECK(x, what) do { if (!(x)) { std::printf("FAIL: %s\n", what); failures++; } } while (0)
const std::vector<std::pair<uint64_t, uint64_t>> none;
constexpr uint64_t kLo = 0x40000000;   // (a pool's start; its pages below)
constexpr uint64_t A = kLo / 4096 + 1, B = A + 1, C = A + 2, E = A + 3, R0 = (kLo + (4ull << 20)) / 4096;
/// The same stamps, with cpu_gens there or not: what a session does with the row on or off
void session(WriteGen& synced, WriteGen& synced2) {
    for (int i = 0; i < 5; i++) stamp_page(A + 40 + i, next_gen());   // (some CPU writes, then the pool's sync)
    synced = gens.now;
    memory_stamp_gpu(A << 12, 4096);          // (a shader writes A)
    memory_stamp_gpu(R0 << 12, 2u << 20);      // (and a whole 2 MB region)
    stamp_page(B, next_gen());                 // (the collection, or a capture's take, finds a CPU write to B)
    memory_stamp(C << 12, 4096);               // (the replay itself writes C on the CPU: a texture written back)
    memory_stamp_gpu(B << 12, 4096);           // (and the GPU writes B and C too, after them)
    memory_stamp_gpu(C << 12, 4096);
    memory_stamp_gpu(E << 12, 4096);
    synced2 = gens.now;                        // (the pool synced again: every CPU write so far is in its mirror)
}
int main() {
    // ---- the row off: no cpu_gens, nothing taken; what gens says, kept to compare
    WriteGen s_off, s2_off;
    session(s_off, s2_off);
    std::vector<WriteGen> off;
    for (uint64_t p = A; p < A + 64; p++) off.push_back(gens.page(p));
    for (uint64_t p = R0; p < R0 + 512; p += 17) off.push_back(gens.page(p));
    const WriteGen now_off = gens.now;
    CHECK(!lazy_page_takes(A, kLo, s2_off, none), "with the lazy write-back off (no cpu_gens) a page is taken");
    // ---- the row on
    gens = write_gen::Table{};
    cpu_gens = new write_gen::Table;
    WriteGen synced, synced2;
    session(synced, synced2);
    CHECK(gens.page(A) > synced, "(the fixture) the GPU's stamp of A is newer than the pool's sync, as in the renderer");
    CHECK(lazy_page_takes(A, kLo, synced, none), "a page the GPU wrote after the sync and the CPU did not is refused");
    CHECK(lazy_page_takes(R0 + 100, kLo, synced, none), "a page of a whole region the GPU wrote is refused");
    CHECK(!lazy_page_takes(B, kLo, synced, none), "a page the CPU wrote (a collection's or a capture's stamp) is taken before its sync");
    CHECK(!lazy_page_takes(C, kLo, synced, none), "a page the replay wrote on the CPU (memory_stamp) is taken before its sync");
    CHECK(lazy_page_takes(B, kLo, synced2, none) && lazy_page_takes(C, kLo, synced2, none),
          "a page the CPU wrote is still refused after the sync that took the write into the mirror");
    CHECK(lazy_page_takes(E, kLo, synced, none), "a page only the GPU wrote, after the CPU's last write elsewhere, is refused");
    const std::vector<std::pair<uint64_t, uint64_t>> ev{{(E << 12) - kLo + 64, (E << 12) - kLo + 68}};
    CHECK(!lazy_page_takes(E, kLo, synced2, ev), "a page with an event word is taken");
    CHECK(lazy_page_takes(A, kLo, synced2, ev), "an event word on another page refuses this one");
    // ---- gens means what it meant
    std::vector<WriteGen> on;
    for (uint64_t p = A; p < A + 64; p++) on.push_back(gens.page(p));
    for (uint64_t p = R0; p < R0 + 512; p += 17) on.push_back(gens.page(p));
    CHECK(on.size() == off.size() && gens.now == now_off && s_off == synced && s2_off == synced2,
          "gens hands out other generations with cpu_gens there");
    bool same = true;
    for (size_t i = 0; i < on.size() && i < off.size(); i++) same = same && on[i] == off[i];
    CHECK(same, "gens answers otherwise for a page with cpu_gens there");
    CHECK(cpu_gens->page(A) == WriteGen{} && cpu_gens->page(R0 + 3) == WriteGen{} && cpu_gens->page(B) <= synced2 &&
          cpu_gens->page(B) > synced, "cpu_gens holds a GPU write, or not the CPU's");
    if (failures) return std::printf("%d check(s) failed\n", failures), 1;
    std::printf("PASS: rule 1 takes what only the GPU wrote and refuses what the CPU wrote since the sync\n");
    return 0;
}
'''


def build(cxx, tmp, memory, name):
    src = FIXTURE
    src = src.replace("@CPU_GENS@", cut(memory, "write_gen::Table* cpu_gens = nullptr;", "\n}\n"))
    src = src.replace("@STAMP_GPU@", cut(memory, "void memory_stamp_gpu(uint64_t guest, uint64_t size) {"))
    src = src.replace("@STAMP@", cut(memory, "void memory_stamp(uint64_t guest, uint64_t size) {"))
    src = src.replace("@TAKES@", cut(memory, "bool lazy_page_takes("))
    cpp = Path(tmp) / (name + ".cpp")
    cpp.write_text(src, encoding="utf-8")
    exe = Path(tmp) / (name + (".exe" if os.name == "nt" else ""))
    subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function",
                    "-D_CRT_SECURE_NO_WARNINGS", "-I", str(ROOT / "gpu" / "vk"), str(cpp), "-o", str(exe)], check=True)
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    memory = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    # (the source: the rule asks cpu_gens alone, lazy_runs goes through it, and only the CPU's stamps feed it)
    takes = cut(memory, "bool lazy_page_takes(")
    if "cpu_gens->page(page) <= synced" not in takes or "gens.page(" in takes.replace("cpu_gens->page(", "") or "page_gen(" in takes:
        raise SystemExit("FAIL: lazy_page_takes does not ask cpu_gens alone")
    runs = cut(memory, "bool lazy_runs(")
    if "lazy_page_takes(page, p.lo, p.synced_gen, events)" not in runs or "page_gen(" in runs:
        raise SystemExit("FAIL: lazy_runs does not take its pages through lazy_page_takes")
    if memory.count("cpu_gens->page(") != 1 or memory.count("cpu_gens = new write_gen::Table") != 1:
        raise SystemExit("FAIL: cpu_gens is read outside lazy_page_takes, or made more than once")
    if "if (cpu_gens && !tl_stamp_gpu) cpu_gens->stamp_range(guest, size, g);" not in cut(memory, "void memory_stamp(uint64_t guest"):
        raise SystemExit("FAIL: memory_stamp does not stamp the CPU's writes into cpu_gens")
    with tempfile.TemporaryDirectory(prefix="lazy-wb-cpu-gens-") as tmp:
        subprocess.run([str(build(args.cxx, tmp, memory, "fixture"))], check=True, timeout=60)
        # (the test's teeth: the rule as it was, asking gens, has to fail, by the page only the GPU wrote)
        old = memory.replace("return cpu_gens && cpu_gens->page(page) <= synced;", "return gens.page(page) <= synced;")
        if old == memory:
            raise SystemExit("FAIL: lazy_page_takes's rule is not where this test changes it")
        r = subprocess.run([str(build(args.cxx, tmp, old, "old"))], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           timeout=60)
        if r.returncode == 0 or "the GPU wrote after the sync and the CPU did not is refused" not in r.stdout:
            raise SystemExit("FAIL: the rule asking gens, as before, passes this test:\n" + r.stdout)
        print("the rule asking gens, as it did, fails here as it must")


if __name__ == "__main__":
    main()
