"""What the process holds in RAM, in parts, and the rules that react to the PC's RAM running short (gpu/vk/mem_parts.h;
vk_memory.cpp memory_ram_watch and the "vk: memory, the parts" line; no game/GPU).

The header is compiled and run as it is, on authored address spaces only:
- the committed runs of a window are found from a stand-in VirtualQuery: regions that touch are joined, the window's
  edges cut them, uncommitted and reserved ones are left out, a query that fails or a region of no size ends the walk,
  and the number of regions asked is bounded;
- the sample of pages: every page asked when they are few (the answer exact), one in so many when they are many (never
  more than the bound plus one a run), each standing for the bytes up to the next, so that the estimate of the pages in
  RAM is within a stride of the truth for runs resident in part, and exact for runs resident whole or not at all;
- the rest of the address space by kind: the guest window stepped over without a query (and a region that runs into
  it cut at its start), runs of one kind joined and of two kinds not, what is not counted left out, the queries
  counted and a walk stopped by its bound told; the samples' pages summed by kind;
- the line gives each part and says "?" for what is not known, and "let go" for a pipeline cache let go, the rest's
  split by kind once it was sampled and the pipelines drawn and given back once those are known;
- the RAM's rules: short under 1.5 GB, low under 3 GB or a tenth of a PC of more than 30 GB, a copy fits with 2 GB to
  spare (and never past what is available), and the precompile keeps its pipelines only where 22 GB are available or
  held, on a PC of 20 GB or more.
Then the source: the RAM is looked at from the presents, the line is written on a thread of its own (not the thread
presenting, nor the serial job thread whose queue builds compute pipelines), the guest window walked every eighth line,
the pipeline cache let go only at the second short look in a row, the save in play and the builder behind the game ask
the RAM, the precompile's rule asks the RAM available, and each switch is in docs/development.md.
python -I -B tests/test_mem_parts.py --cxx clang++
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "mem_parts.h"
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); return 1; } } while (0)
using namespace mem_parts;
// A stand-in address space: regions in order, each committed or not, that tile [0, top)
struct Region { uint64_t base, size; bool committed; };
struct Space {
    std::vector<Region> regions;
    size_t asked = 0;
    bool query(uint64_t at, uint64_t* base, uint64_t* size, bool* committed) {
        asked++;
        for (const Region& r : regions)
            if (at >= r.base && at < r.base + r.size) {
                *base = r.base, *size = r.base + r.size - r.base, *committed = r.committed;
                return true;
            }
        return false;
    }
};
static std::vector<Span> walk(Space& s, uint64_t lo, uint64_t hi, size_t max_queries = 1000) {
    return committed_spans(lo, hi, [&](uint64_t a, uint64_t* b, uint64_t* n, bool* c) { return s.query(a, b, n, c); }, max_queries);
}
int main() {
    const uint64_t P = 4096, MB = 1ull << 20, GB = 1ull << 30;
    {   // (runs: joined where they touch, cut by the window, the uncommitted left out)
        Space s{{{0, 8 * P, false}, {8 * P, 4 * P, true}, {12 * P, 4 * P, true}, {16 * P, 2 * P, false}, {18 * P, 6 * P, true},
                 {24 * P, 100 * P, false}}};
        std::vector<Span> v = walk(s, 0, 124 * P);
        CHECK(v.size() == 2 && v[0].lo == 8 * P && v[0].hi == 16 * P && v[1].lo == 18 * P && v[1].hi == 24 * P);
        v = walk(s, 10 * P, 20 * P);   // (a window that starts and ends inside runs)
        CHECK(v.size() == 2 && v[0].lo == 10 * P && v[0].hi == 16 * P && v[1].lo == 18 * P && v[1].hi == 20 * P);
        v = walk(s, 0, 124 * P, 3);   // (three regions asked at most)
        CHECK(s.asked && v.size() == 1 && v[0].lo == 8 * P && v[0].hi == 16 * P);
        Space gap{{{0, 4 * P, true}}};   // (a query that fails past the known regions ends the walk)
        v = walk(gap, 0, 100 * P);
        CHECK(v.size() == 1 && v[0].hi == 4 * P);
        Space none{{{0, 0, true}}};   // (a region of no size: no endless walk)
        CHECK(walk(none, 0, 100 * P).empty());
    }
    {   // (few pages: each asked, the answer exact)
        const std::vector<Span> spans{{0, 10 * P}, {20 * P, 23 * P}};
        const Plan p = sample_plan(spans, P, 1000);
        CHECK(p.committed == 13 * P && p.stride == P && p.at.size() == 13);
        std::vector<bool> in(p.at.size());
        uint64_t truth = 0;
        for (size_t i = 0; i < in.size(); i++) in[i] = i % 3 == 0, truth += in[i] ? P : 0;
        CHECK(resident_bytes(p, in) == truth);
    }
    {   // (many pages: a stride of whole pages, at most the bound plus one a run, and every byte stood for once)
        std::mt19937_64 rng(7);
        for (int round = 0; round < 200; round++) {
            std::vector<Span> spans;
            uint64_t at = 0;
            const int runs = 1 + (int)(rng() % 40);
            for (int r = 0; r < runs; r++) {
                at += (rng() % 64) * P;
                const uint64_t n = (1 + rng() % 5000) * P;
                spans.push_back({at, at + n}), at += n;
            }
            const size_t bound = 1 + rng() % 3000;
            const Plan p = sample_plan(spans, P, bound);
            CHECK(p.stride % P == 0 && p.stride >= P && p.at.size() <= bound + spans.size());
            uint64_t sum = 0;
            for (uint64_t b : p.bytes) sum += b;
            CHECK(sum == p.committed);
            // (resident: each run's first part, at random lengths: the estimate is within a stride a run of the truth)
            std::vector<uint64_t> cut;
            uint64_t truth = 0;
            for (const Span& s : spans) {
                const uint64_t c = s.lo + (rng() % ((s.hi - s.lo) / P + 1)) * P;
                cut.push_back(c), truth += c - s.lo;
            }
            std::vector<bool> in(p.at.size());
            for (size_t i = 0; i < p.at.size(); i++)
                for (size_t r = 0; r < spans.size(); r++)
                    if (p.at[i] >= spans[r].lo && p.at[i] < spans[r].hi) in[i] = p.at[i] < cut[r];
            const uint64_t est = resident_bytes(p, in);
            const uint64_t err = est > truth ? est - truth : truth - est;
            CHECK(err <= p.stride * spans.size());
            // (a run resident whole or not at all is counted exactly)
            std::vector<bool> half(p.at.size());
            uint64_t whole = 0;
            for (size_t r = 0; r < spans.size(); r += 2) whole += spans[r].hi - spans[r].lo;
            for (size_t i = 0; i < p.at.size(); i++)
                for (size_t r = 0; r < spans.size(); r += 2)
                    if (p.at[i] >= spans[r].lo && p.at[i] < spans[r].hi) half[i] = true;
            CHECK(resident_bytes(p, half) == whole);
        }
        // (the window's 9.8 GB committed, as the game's: about 32,000 pages asked)
        const Plan p = sample_plan({{0x1000000000ull, 0x1000000000ull + 9809 * MB}}, P, 32768);
        CHECK(p.at.size() <= 32769 && p.at.size() > 30000 && p.committed == 9809 * MB);
        CHECK(resident_bytes(p, std::vector<bool>(p.at.size(), true)) == 9809 * MB);
        CHECK(resident_bytes(p, {}) == 0);
        CHECK(sample_plan({}, P, 100).at.empty() && sample_plan({}, P, 100).committed == 0);
    }
    {   // (the line)
        Parts p;
        p.working_set = 20887 * MB, p.private_bytes = 39435 * MB, p.guest_committed = 9809 * MB, p.guest_resident = 7000 * MB;
        p.modules = 12000, p.module_bytes = 1500 * MB, p.packed_stages = 20000, p.packed_bytes = 520 * MB;
        p.pcache_bytes = 2618 * MB, p.pipelines = 15525, p.libraries = 3, p.system_blocks = 744 * MB;
        p.ram_avail = 547 * MB, p.ram_total = 32069 * MB;
        std::string s = line(p);
        CHECK(s.find("working set 20887 (private 39435)") != std::string::npos);
        CHECK(s.find("the game's own memory 9809 committed, 7000 of it in RAM") != std::string::npos);
        CHECK(s.find("the rest of the working set 13887: shader modules 1500 (12000 modules; 20000 stages packed in 520)") !=
              std::string::npos);
        CHECK(s.find(", the pipeline cache 2618, 15525 pipelines and 3 stage libraries kept") != std::string::npos);
        CHECK(s.find("system memory 744; RAM 547 of 32069 available") != std::string::npos);
        p.pcache_bytes = -1, p.guest_resident = -1, p.guest_committed = -1, p.system_blocks = -1;
        s = line(p);
        CHECK(s.find("the pipeline cache let go") != std::string::npos && s.find("? committed, ? of it in RAM") != std::string::npos);
        CHECK(s.find("the rest of the working set") == std::string::npos && s.find("system memory ?") != std::string::npos);
    }
    {   // (the rest of the address space, by kind: the guest window stepped over)
        struct KSpace {
            std::vector<std::pair<Region, uint8_t>> regions;
            size_t asked = 0;
            bool query(uint64_t at, uint64_t* base, uint64_t* size, uint8_t* kind) {
                asked++;
                for (const auto& [r, k] : regions)
                    if (at >= r.base && at < r.base + r.size) {
                        *base = r.base, *size = r.size, *kind = r.committed ? k : (uint8_t)kNone;
                        return true;
                    }
                return false;
            }
        };
        KSpace s{{{{0, 4 * P, true}, kExe}, {{4 * P, 4 * P, true}, kExe}, {{8 * P, 4 * P, true}, kPrivate},
                  {{12 * P, 8 * P, true}, kPrivate}, {{20 * P, 1000 * P, false}, kNone}, {{1020 * P, 4 * P, true}, kMapped},
                  {{1024 * P, 4 * P, true}, kGpuVisible}}};
        size_t queries = 0;
        bool whole = false;
        auto q = [&](uint64_t a, uint64_t* b, uint64_t* n, uint8_t* k) { return s.query(a, b, n, k); };
        std::vector<Span> v = kind_spans(0, 1028 * P, q, 100, 0, 0, &queries, &whole);
        CHECK(v.size() == 4 && whole && queries == 7);
        CHECK(v[0].lo == 0 && v[0].hi == 8 * P && v[0].kind == kExe && v[1].lo == 8 * P && v[1].hi == 20 * P && v[1].kind == kPrivate);
        CHECK(v[2].kind == kMapped && v[3].kind == kGpuVisible && v[3].hi == 1028 * P);
        // (the window [14P, 1022P) stepped over: the private run cut at its start, the mapped one begun at its end)
        s.asked = 0;
        v = kind_spans(0, 1028 * P, q, 100, 14 * P, 1022 * P, &queries, &whole);
        CHECK(whole && v.size() == 4 && v[1].hi == 14 * P && v[2].lo == 1022 * P && v[2].kind == kMapped && s.asked == queries);
        for (const Span& sp : v) CHECK(sp.hi <= 14 * P || sp.lo >= 1022 * P);
        v = kind_spans(0, 1028 * P, q, 2, 0, 0, &queries, &whole);   // (stopped by its bound: told)
        CHECK(!whole && queries == 2 && v.size() == 1);
        // (the samples summed by kind)
        const Plan pl = sample_plan({{0, 8 * P, kExe}, {8 * P, 20 * P, kPrivate}, {1020 * P, 1024 * P, kMapped}}, P, 1000);
        std::vector<bool> in(pl.at.size(), true);
        uint64_t by[kKinds]{};
        resident_by_kind(pl, in, by);
        CHECK(by[kExe] == 8 * P && by[kPrivate] == 12 * P && by[kMapped] == 4 * P && by[kGuest] == 0 && by[kGpuVisible] == 0);
        in[0] = false;
        uint64_t by2[kKinds]{};
        resident_by_kind(pl, in, by2);
        CHECK(by2[kExe] == 7 * P);
    }
    {   // (the line's split and the pipelines once known)
        Parts p;
        p.working_set = 19281 * MB, p.guest_resident = 9421 * MB;
        for (int k = 0; k < kKinds; k++) p.by_kind[k] = (int64_t)k * 100 * MB;
        p.pipelines = 14535, p.drawn = 3000, p.released = 512;
        const std::string s = line(p);
        CHECK(s.find("the rest of the working set 9860 (sampled: the program's code 200, the driver's code 300, other code 400, "
                     "mapped files and views 500, write-combined or uncached memory 600, the heaps and other private memory 700)") !=
              std::string::npos);
        CHECK(s.find("14535 pipelines and 0 stage libraries kept (3000 of the pipelines drawn; 512 no draw used given back)") !=
              std::string::npos);
        Parts q;
        CHECK(line(q).find("sampled") == std::string::npos && line(q).find("drawn") == std::string::npos);
    }
    // (the precompile's keep: 22 GB available or held, on a PC of 20 GB or more)
    CHECK(keep_all_pipelines(32 * GB, 20 * GB, 2 * GB) && !keep_all_pipelines(32 * GB, 13 * GB, 5 * GB));
    CHECK(!keep_all_pipelines(16 * GB, 30 * GB, 0) && keep_all_pipelines(64 * GB, 40 * GB, 1 * GB));
    CHECK(!keep_all_pipelines(32 * GB, 18455 * MB, 1 * GB));   // (v0.4.0-test9's start)
    // (the RAM's rules)
    CHECK(short_now(1535 * MB) && !short_now(1536 * MB) && short_now(0));
    CHECK(low_now(3 * GB - 1, 16 * GB) && !low_now(3 * GB, 16 * GB) && !low_now(3 * GB, 30 * GB));
    CHECK(low_now(3200 * MB, 32 * GB) && !low_now(3300 * MB, 32 * GB) && low_now(6 * GB, 64 * GB) && !low_now(7 * GB, 64 * GB));
    CHECK(copy_fits(5 * GB, 2 * GB) && !copy_fits(4 * GB, 2 * GB) && !copy_fits(1 * GB, 2 * GB) && !copy_fits(0, 1));
    CHECK(copy_fits(2 * GB + 1, 0) && !copy_fits(2 * GB, 0));
    return 0;
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def check_source():
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    if "memory_ram_watch((uint64_t)presents);" not in between(replay, "void log_frame_stats() {", "\n}\n"):
        raise SystemExit("the RAM is not looked at from the presents")
    watch = between(mem, "void memory_ram_watch(uint64_t presents) {", "\n}\n")
    if "if (presents % 60) return;" not in watch or "if (presents % 600 == 0) memory_line_ask();" not in watch or \
            "memory_parts_line" in watch or "jobs_push" in watch:
        raise SystemExit("the RAM is looked at every present, or the memory line is written on the thread presenting or "
                         "on a job thread")
    ask = between(mem, "void memory_line_ask() {", "\n}\n")
    if "std::thread(" not in ask or "memory_parts_line();" not in ask or "line_cv.wait(" not in ask or \
            ask.index("std::thread(") > ask.index("memory_parts_line();"):
        raise SystemExit("the memory line has no thread of its own")
    if "jobs_push_serial" in mem:
        raise SystemExit("vk_memory.cpp puts work on the serial job thread, whose queue builds compute pipelines")
    if "ram_short_looks.store(short_looks, std::memory_order_relaxed);\n    if (short_looks >= 2) pcache_let_go(avail);" \
            not in watch or "mem_parts::short_now(avail) ? ram_short_looks.load(std::memory_order_relaxed) + 1 : 0;" not in watch:
        raise SystemExit("the pipeline cache is let go at a single short look, or never")
    line = between(mem, "void memory_parts_line() {", "\n}\n")
    for call in ("sample_parts(&p, &c);", "program_module_stats(", "pipeline_parts(", "vma_system_blocks()",
                 'getenv("RX_VK_MEM_PARTS")'):
        if call not in line:
            raise SystemExit("the memory line leaves out " + call)
    parts = between(mem, "bool sample_parts(mem_parts::Parts* p, LineCost* c) {", "\n}\n")
    if "if (guest.empty() || lines++ % 8 == 0) {" not in parts or "glo, ghi, &c->other_regions" not in parts:
        raise SystemExit("the guest window is walked at every line, or the rest's walk does not step over it")
    keep = between(draw, "bool precompile_keeps_all() {", "\n}\n")
    if "mem_parts::keep_all_pipelines(ms.ullTotalPhys, ms.ullAvailPhys, ws)" not in keep:
        raise SystemExit("the precompile's keep goes by the PC's total RAM, not the RAM the game can have")
    save = between(draw, "void pipeline_cache_save(bool force) {", "\n}\n")
    if "if (!force && !memory_ram_fits(n)) {" not in save or save.index("memory_ram_fits(n)") > save.index("std::vector<uint8_t> data(n);"):
        raise SystemExit("the save in play fetches the cache before it asks whether a copy fits in the RAM")
    bg = between(draw, "void predict_background() {", "\n}\n")
    if "while (!prepare_stopped() && memory_ram_low()) Sleep(500);" not in bg:
        raise SystemExit("the builder behind the game does not rest while the RAM is low")
    dev_md = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    for switch in ("`RX_VK_MEM_PARTS=0`", "`RX_VK_PCACHE_RAM=1`", "`RX_VK_PRECOMPILE_KEEP=1` or `0`"):
        if switch not in dev_md:
            raise SystemExit(switch + " is not in docs/development.md's switches")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="mem-parts-") as tmp:
        tmp = Path(tmp)
        (tmp / "t.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / "mem-parts"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu/vk"),
                        str(tmp / "t.cpp"), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        if r.returncode:
            raise SystemExit(f"mem_parts.h: {r.stdout.strip() or r.returncode}")
    print("PASS: the guest window's committed runs are found and sampled within a stride of the truth, the memory line gives "
          "each part, and the RAM's rules react as they say")


if __name__ == "__main__":
    main()
