"""The game's freed pages under an import still being made on the background thread (gpu/vk/vk_memory.cpp
memory_guest_imported, bg_send, bg_let_go, bg_drain; no game, no GPU).

The host asks the Vulkan back end, before it gives the game's freed pages back (host/hos_mem.c FreeMemoryPages,
host/hos_svc.c UnmapPhysicalMemory), whether an import holds them: if one does they are zeroed and stay, otherwise they are
decommitted ("decommitted pages under an import faulted the device"). The answer looked at the pool table alone, and an
import the background thread makes is in the table only once bg_drain has run: from the thread's page locking until then the
host decommitted pages the import had locked, the GPU kept the old pages and the game got new ones. The imports ahead import
ranges of up to 1 GB early and whole, at the title while the game still frees and maps pages there: the owner's test11 and
test12 lost the device at the title, a lab copy of test12 in 6 of 12 runs with them and in none of 10 without.

Cuts memory_guest_imported, bg_send, bg_let_go and bg_drain out of vk_memory.cpp and runs them against stand-ins through a
range's whole way: sent (as import_ahead_look, prefetch_chunks, import_in_background and memory_pool_created send theirs),
imported and waiting in bg_ready, drained into the table, an import made redundant or dropped as overlapping, a failed one.
At each step the host's question about the range's pages must say "imported" while anything may hold them, and "not" once
nothing does. Reads the source for the rest: every range goes to the thread through bg_send, and bg_main lets a failed one go.
Without the fix the host is told "not imported" while the range is under way, and the test fails; with
RX_VK_BG_IMPORT_HOLD=0 (the lab's A/B) it is told so again.

    python -I -B tests/test_bg_import_held.py [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    if text.count(marker) != 1:
        raise SystemExit("FAIL: not found once in vk_memory.cpp: " + marker)
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


FIXTURE = r"""
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <utility>
#include <vector>
typedef void* VkBuffer;
typedef void* VkDeviceMemory;
#define VK_NULL_HANDLE nullptr
struct Pool {
    uint64_t lo = 0, hi = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> pieces;
    uint64_t address = 0, imported_value = 0;
    bool prefetched = false;
};
struct { void* device = nullptr; } dev;
std::mutex pools_mu, bg_mu;
std::map<uint64_t, Pool> imported;
std::deque<std::map<uint64_t, Pool>::node_type> retired_pools;
std::deque<std::pair<uint64_t, uint64_t>> bg_queue;
std::vector<uint64_t> bg_inflight;
std::vector<std::pair<uint64_t, Pool>> bg_ready;
int64_t st_prefetch_dropped = 0;
uint64_t mem_rec_value = 7;
int freed = 0;
void vkDestroyBuffer(void*, VkBuffer, void*) {}
void vkFreeMemory(void*, VkDeviceMemory, void*) { freed++; }
void logf(const char*, ...) {}
void guest_pages_set(uint64_t, uint64_t, uint64_t) {}
void retire_pool(std::map<uint64_t, Pool>::iterator it) { retired_pools.push_back(imported.extract(it)); }
std::vector<std::pair<uint64_t, uint64_t>> bg_held;
@@SEND@@
@@LET_GO@@
@@DRAIN@@
@@IMPORTED@@
int fails = 0;
#define CHECK(c, what) do { if (!(c)) { std::printf("FAIL: %s\n", what); fails++; } } while (0)
constexpr uint64_t M = 1ull << 20;
// (bg_main's end, as it is: the import done, or not)
void thread_done(bool ok, uint64_t lo, uint64_t hi) {
    std::lock_guard lk(bg_mu);
    std::pair<uint64_t, uint64_t> r{lo, hi};
    bg_queue.erase(std::find(bg_queue.begin(), bg_queue.end(), r));
    Pool p;
    p.lo = lo, p.hi = hi, p.memory = (VkDeviceMemory)(uintptr_t)lo, p.buffer = (VkBuffer)(uintptr_t)hi, p.prefetched = true;
    if (ok) bg_ready.push_back({lo, p});
    else bg_let_go(lo, hi);
    std::erase(bg_inflight, lo);
}
void drain() {
    std::lock_guard lk(pools_mu);
    bg_drain();
}
int main(int argc, char**) {
    const uint64_t A = 0x1072000000, B = A + 80 * M;   // (the test11 / test12 pool: imported ahead whole)
    CHECK(!memory_guest_imported(A + M, A + 2 * M), "nothing sent, nothing imported: not imported");
    {
        std::lock_guard lk(bg_mu);
        bg_send(A, B);
    }
    if (argc > 1) {   // (run with RX_VK_BG_IMPORT_HOLD=0: the lab's A/B, the answer as before the fix)
        CHECK(!memory_guest_imported(A + 16 * M, A + 17 * M), "RX_VK_BG_IMPORT_HOLD=0: under way and not in the table, not imported");
        if (!fails) std::puts("background imports: RX_VK_BG_IMPORT_HOLD=0 answers as before");
        return fails ? 1 : 0;
    }
    CHECK(memory_guest_imported(A + 16 * M, A + 17 * M), "sent to the thread, its pages may be locked any moment: imported");
    CHECK(memory_guest_imported(A - M, A + 4096), "a freed range reaching into it: imported");
    CHECK(!memory_guest_imported(B, B + M), "the pages after it: not imported");
    thread_done(true, A, B);
    CHECK(memory_guest_imported(A + 16 * M, A + 17 * M), "imported, waiting for bg_drain: imported");
    drain();
    CHECK(imported.count(A) == 1 && bg_held.empty(), "drained: in the table, no longer held");
    CHECK(memory_guest_imported(A + 16 * M, A + 17 * M), "in the table: imported");
    // (a second import of a range the table already covers: given back at the drain, the table's pool still holds the pages)
    {
        std::lock_guard lk(bg_mu);
        bg_send(A, A + 16 * M);
    }
    thread_done(true, A, A + 16 * M);
    const int freed0 = freed;
    drain();
    CHECK(freed == freed0 + 1 && bg_held.empty(), "a redundant import: given back and let go");
    CHECK(memory_guest_imported(A, A + M), "its pages are still the table pool's: imported");
    // (one overlapping a pool in part: dropped, let go)
    const uint64_t C = B - 16 * M, D = B + 32 * M;
    {
        std::lock_guard lk(bg_mu);
        bg_send(C, D);
    }
    CHECK(memory_guest_imported(B + 16 * M, B + 17 * M), "an overlapping import under way: its own pages too");
    thread_done(true, C, D);
    drain();
    CHECK(bg_held.empty() && st_prefetch_dropped == 1, "an overlapping import: dropped and let go");
    CHECK(!memory_guest_imported(B + 16 * M, B + 17 * M), "dropped: its pages past the pool are no import's");
    // (a failed import: nothing holds its pages)
    const uint64_t E = 0x1080000000, F = E + 16 * M;
    {
        std::lock_guard lk(bg_mu);
        bg_send(E, F);
    }
    CHECK(memory_guest_imported(E, E + M), "under way: imported");
    thread_done(false, E, F);
    CHECK(!memory_guest_imported(E, E + M) && bg_held.empty(), "failed: not imported, let go");
    if (!fails) std::printf("background imports: the host is told of every range from its sending until it is in the table\n");
    return fails ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    memory = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    fails = []
    pushes = [m.start() for m in re.finditer(r"\bbg_queue\.push_back\(", memory)]
    send = block(memory, "void bg_send(uint64_t lo, uint64_t hi) {")
    if len(pushes) != 1 or "bg_queue.push_back(" not in send or "bg_held.push_back(" not in send:
        fails.append("a range goes to the background thread other than through bg_send (bg_queue.push_back outside it)")
    bg_main = block(memory, "void bg_main() {")
    if "else bg_let_go(r.first, r.second);" not in bg_main:
        fails.append("bg_main does not let a failed import's range go")
    if "bg_held" not in block(memory, "int memory_guest_imported(uint64_t lo, uint64_t hi) {"):
        fails.append("memory_guest_imported does not look at the ranges the background thread holds (bg_held)")
    for f in fails:
        print("FAIL: " + f)
    fixture = (FIXTURE.replace("@@SEND@@", send)
               .replace("@@LET_GO@@", block(memory, "void bg_let_go(uint64_t lo, uint64_t hi) {"))
               .replace("@@DRAIN@@", block(memory, "void bg_drain() {"))
               .replace("@@IMPORTED@@", block(memory, "int memory_guest_imported(uint64_t lo, uint64_t hi) {")))
    with tempfile.TemporaryDirectory(prefix="bg-import-held-") as tmp:
        src, exe = Path(tmp) / "held.cpp", Path(tmp) / "held.exe"
        src.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-unused-variable",
                        str(src), "-o", str(exe)], check=True)
        env = dict(os.environ)
        env.pop("RX_VK_BG_IMPORT_HOLD", None)
        ran = subprocess.run([str(exe)], env=env).returncode
        env["RX_VK_BG_IMPORT_HOLD"] = "0"   # (the switch: as before)
        ran = ran or subprocess.run([str(exe), "off"], env=env).returncode
    if fails or ran:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
