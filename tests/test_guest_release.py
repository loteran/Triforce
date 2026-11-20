"""The census's give-back asks the Vulkan back end before it decommits (gpu/vk/vk_memory.cpp guest_release_unimported, its
export mkwvk_guest_release; host/hos_mem.c census_offer; no game, no GPU).

The game's freed pages are zeroed in place, not decommitted, while an import holds them (host/hos_mem.c FreeMemoryPages,
host/hos_svc.c UnmapPhysicalMemory: "decommitted pages under an import faulted the device"), and nothing gave them back
after: the census's give-back (HOS_MEM_GIVE_BACK) does, once no import holds them. The question and the decommit must be
one step: an import made between them would lock pages the host then takes away (the GPU on the old pages, the game on
new ones; the owner's test11 and test12 lost the device that way, tests/test_bg_import_held.py). So the back end runs the
host's decommit itself, with pools_mu and bg_mu held, only where nothing of its own holds the pages.

Cuts guest_release_unimported (and DeadImport) out of vk_memory.cpp and runs it against stand-ins: an import in the table,
a retired pool still imported (and one whose import is gone), an import given back whose memory is not freed yet (an evicted
pool, or a retired pool's import that retired_imports_give_back gave back, until reap_mirrors frees it), a range under way on
the background thread, and pages nobody imports. The decommit must run only for the last, with both locks held, over the range
asked; and with each check of the function taken out in turn, a case must fail.

    python -I -B tests/test_guest_release.py [--cxx g++]
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
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
typedef void* VkDeviceMemory;
typedef void* VkBuffer;
struct Pool {
    uint64_t lo = 0, hi = 0;
    VkDeviceMemory memory = nullptr;
};
struct Lock {   // (pools_mu's and bg_mu's stand-in: says whether it is held)
    bool held = false;
    void lock() { held = true; }
    void unlock() { held = false; }
};
Lock pools_mu, bg_mu;
std::map<uint64_t, Pool> imported;
std::deque<std::map<uint64_t, Pool>::node_type> retired_pools, evicted_pools;
std::vector<std::pair<uint64_t, uint64_t>> bg_held;
@@DEAD_IMPORT@@
std::deque<DeadImport> dead_imports;

@@PRODUCTION@@

int calls, fails;
uint64_t got_lo, got_hi;
bool locks_held;
int release(uint64_t lo, uint64_t hi) {
    calls++, got_lo = lo, got_hi = hi, locks_held = pools_mu.held && bg_mu.held;
    return 1;
}
int refuse(uint64_t, uint64_t) { calls++; return 0; }
#define CHECK(c, what) do { if (!(c)) { std::printf("FAIL: %s\n", what); fails++; } } while (0)
constexpr uint64_t MB = 1ull << 20;
const auto mem = (VkDeviceMemory)(uintptr_t)1;

void clear() {
    imported.clear(), retired_pools.clear(), evicted_pools.clear(), bg_held.clear(), dead_imports.clear();
}
std::map<uint64_t, Pool>::node_type node(uint64_t lo, uint64_t hi, bool memory) {
    std::map<uint64_t, Pool> m;
    m[lo] = Pool{lo, hi, memory ? mem : nullptr};
    return m.extract(lo);
}
// (the range [lo, hi) asked about, against an import of [a, b) of the given kind; overlapping: the decommit must not run)
void one(const char* kind, uint64_t a, uint64_t b, uint64_t lo, uint64_t hi, bool overlapping) {
    clear();
    std::string k(kind);
    if (k == "table") imported[a] = Pool{a, b, mem};
    else if (k == "retired") retired_pools.push_back(node(a, b, true));
    else if (k == "retired, its import gone") retired_pools.push_back(node(a, b, false));
    else if (k == "given back, not freed yet") evicted_pools.push_back(node(a, b, true));
    else if (k == "background") bg_held.push_back({a, b});
    else if (k == "retired, given back, not freed yet") {   // (as retired_imports_give_back pushes it)
        DeadImport d{};
        d.memory = mem, d.bytes = b - a, d.lo = a, d.hi = b;
        dead_imports.push_back(std::move(d));
    }
    calls = 0;
    const int r = guest_release_unimported(lo, hi, release);
    char what[256];
    std::snprintf(what, sizeof what, "%s [%#llx, %#llx) against [%#llx, %#llx): answered %d, %d decommits", kind, (unsigned long long)a,
                  (unsigned long long)b, (unsigned long long)lo, (unsigned long long)hi, r, calls);
    if (overlapping) CHECK(r == -1 && calls == 0, what);
    else CHECK(r == 1 && calls == 1 && got_lo == lo && got_hi == hi && locks_held, what);
    CHECK(!pools_mu.held && !bg_mu.held, "a lock left held");
}

int main() {
    const uint64_t a = 0x7F00000000ull;
    for (const char* kind : {"table", "retired", "given back, not freed yet", "retired, given back, not freed yet", "background"}) {
        one(kind, a, a + 16 * MB, a + 4 * MB, a + 5 * MB, true);        // (inside)
        one(kind, a, a + 16 * MB, a - MB, a + 4096, true);              // (its first page)
        one(kind, a, a + 16 * MB, a + 16 * MB - 4096, a + 17 * MB, true);   // (its last page)
        one(kind, a, a + 16 * MB, a - 2 * MB, a, false);                // (just below)
        one(kind, a, a + 16 * MB, a + 16 * MB, a + 17 * MB, false);     // (just above)
    }
    one("retired, its import gone", a, a + 16 * MB, a + 4 * MB, a + 5 * MB, false);
    one("none", 0, 0, a, a + MB, false);
    {   // (the table's walk: imports far below, and one 1 GB below that reaches nothing)
        clear();
        imported[a - (4ull << 30)] = Pool{a - (4ull << 30), a - (3ull << 30), mem};
        imported[a + 64 * MB] = Pool{a + 64 * MB, a + 80 * MB, mem};
        calls = 0;
        CHECK(guest_release_unimported(a, a + MB, release) == 1 && calls == 1, "pages between imports not given back");
        imported[a - 512 * MB] = Pool{a - 512 * MB, a + 4096, mem};   // (a large import from below that reaches it)
        calls = 0;
        CHECK(guest_release_unimported(a, a + MB, release) == -1 && calls == 0, "pages a large import from below holds given back");
    }
    clear();
    calls = 0;
    CHECK(guest_release_unimported(a, a + MB, refuse) == 0 && calls == 1, "the host's refusal is not passed on");
    if (fails) return 1;
    std::printf("PASS: the give-back's decommit runs only where nothing imports the pages, with both locks held\n");
    return 0;
}
"""

MUTATIONS = [
    ("if (it->second.hi > lo) return -1;", "if (false) return -1;", "an import in the table is not asked about"),
    ("retired_pools)\n        if (nh", "retired_pools)\n        if (false && nh", "a retired pool still imported is not asked about"),
    ("imported until then)\n        if (nh", "imported until then)\n        if (false && nh",
     "an import given back and not freed yet is not asked about"),
    ("if (d.lo < hi && lo < d.hi) return -1;", "if (false && d.lo < hi && lo < d.hi) return -1;",
     "a retired pool's import given back and not freed yet (dead_imports) is not asked about"),
    ("if (a < hi && lo < b) return -1;", "if (false && a < hi && lo < b) return -1;",
     "a range under way on the background thread is not asked about"),
    ("    std::lock_guard bl(bg_mu);\n", "", "the decommit runs without bg_mu: the background thread may start an import meanwhile"),
]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    text = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    fn = block(text, "int guest_release_unimported(uint64_t lo, uint64_t hi, int (*release)(uint64_t, uint64_t))")
    dead = re.findall(r"^struct DeadImport \{[^\n]*\};$", text, re.M)
    if len(dead) != 1:
        raise SystemExit("FAIL: vk_memory.cpp's struct DeadImport not found once")
    # (the import's guest range filled where retired_imports_give_back gives a retired pool's import back)
    give_back = block(text, "void retired_imports_give_back()")
    if not re.search(r"dead_imports\.push_back\(\{p\.buffer, p\.memory, [^;]*, std::move\(p\.pieces\), p\.lo, p\.hi\}\);", give_back):
        raise SystemExit("FAIL: retired_imports_give_back no longer gives a dead import its pool's guest range (lo, hi)")
    if "int mkwvk_guest_release(uint64_t lo, uint64_t hi, int (*release)(uint64_t, uint64_t))" not in text or \
            "mkwvk::guest_release_unimported(lo, hi, release)" not in text:
        raise SystemExit("FAIL: vk_memory.cpp's mkwvk_guest_release no longer runs guest_release_unimported")
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0

    def run(tmp, name, production):
        src, exe = Path(tmp) / (name + ".cpp"), Path(tmp) / (name + ".exe")
        src.write_text(FIXTURE.replace("@@DEAD_IMPORT@@", dead[0]).replace("@@PRODUCTION@@", production), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)], capture_output=True, text=True, timeout=120, creationflags=flags)

    with tempfile.TemporaryDirectory(prefix="guest-release-") as tmp:
        r = run(tmp, "release", fn)
        print(r.stdout, end="")
        if r.returncode != 0:
            raise SystemExit("FAIL: the give-back's question failed (exit code %d)" % r.returncode)
        for i, (old, new, what) in enumerate(MUTATIONS):
            if fn.count(old) != 1:
                raise SystemExit("FAIL: a mutation no longer matches guest_release_unimported once: %r" % old)
            m = run(tmp, "mutant%d" % i, fn.replace(old, new))
            if m.returncode == 0:
                raise SystemExit("FAIL: the fixture passes with a mutation: " + what)
        print("PASS: %d mutations of guest_release_unimported each fail the fixture" % len(MUTATIONS))


if __name__ == "__main__":
    main()
