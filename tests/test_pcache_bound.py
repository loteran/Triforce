"""The pipeline cache's file has a bound: one over it is not read, data over it is not written (gpu/vk/pcache_bound.h,
vk_draw.cpp pcache_init and pipeline_cache_save; no game/GPU).

The header is compiled and run as it is: the bound from RX_VK_PCACHE_MAX_MB or the PC's RAM, and the read of authored
files in a temporary folder: under the bound byte for byte as written, at the bound still read, over it not a byte, a
file no longer than a header no cache, a missing one a first start. Then the source: pcache_init sets the bound before
it reads and reads through the header, and pipeline_cache_save asks the bound before it fetches the data (the save's
own behaviour is run by tests/test_pipeline_cache_save.py, case over-bound).
python -I -B tests/test_pcache_bound.py --cxx clang++
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
#include <string>
#include "pcache_bound.h"
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); return 1; } } while (0)
using namespace pcache_bound;
static bool put(const std::string& path, size_t n) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = true;
    for (size_t i = 0; i < n && ok; i++) ok = fputc((int)(i * 7 + 3) & 0xFF, f) != EOF;
    return fclose(f) == 0 && ok;
}
static bool same(const std::vector<uint8_t>& d, size_t n) {
    if (d.size() != n) return false;
    for (size_t i = 0; i < n; i++)
        if (d[i] != (uint8_t)(i * 7 + 3)) return false;
    return true;
}
int main(int, char** argv) {
    const uint64_t GB = 1ull << 30, MB = 1ull << 20;
    // (unset: an eighth of the RAM, 2048 MB at least, 8192 MB at most; RAM not known: the least)
    CHECK(bytes(nullptr, 0) == 2048 * MB && bytes(nullptr, 8 * GB) == 2048 * MB && bytes(nullptr, 16 * GB) == 2048 * MB);
    CHECK(bytes(nullptr, 32 * GB) == 4096 * MB && bytes(nullptr, 48 * GB) == 6144 * MB);
    CHECK(bytes(nullptr, 64 * GB) == 8192 * MB && bytes(nullptr, 256 * GB) == 8192 * MB);
    // (set: that many MB, below the least too; 0 lifts it; what is no number is as unset, never "no bound")
    CHECK(bytes("0", 32 * GB) == 0 && bytes("512", 32 * GB) == 512 * MB && bytes("20000", 8 * GB) == 20000 * MB);
    CHECK(bytes("", 32 * GB) == 4096 * MB && bytes("off", 32 * GB) == 4096 * MB && bytes("-1", 32 * GB) == 4096 * MB);
    CHECK(bytes("12x", 32 * GB) == 4096 * MB && bytes("99999999999999999999999", 32 * GB) == 0);
    CHECK(within(0, 0) && within(~0ull, 0) && within(80, 80) && !within(81, 80) && within(1, 80));

    const std::string dir = argv[1];
    std::vector<uint8_t> d{1, 2, 3};
    uint64_t len = 99;
    // (no file: a first start, and nothing left in the block)
    CHECK(read((dir + "/none.bin").c_str(), 32, 1000, d, &len) == Read::None && d.empty() && len == 0);
    // (a header and no more is no cache)
    CHECK(put(dir + "/short.bin", 32));
    CHECK(read((dir + "/short.bin").c_str(), 32, 1000, d, &len) == Read::Short && d.empty() && len == 32);
    // (under the bound: whole, as written, however the read is cut into pieces)
    const size_t big = 300000;
    CHECK(put(dir + "/under.bin", big));
    CHECK(read((dir + "/under.bin").c_str(), 32, big + 1, d, &len) == Read::Ok && same(d, big) && len == big);
    // (at the bound: read. No bound: read)
    CHECK(read((dir + "/under.bin").c_str(), 32, big, d, &len) == Read::Ok && same(d, big));
    CHECK(read((dir + "/under.bin").c_str(), 32, 0, d, &len) == Read::Ok && same(d, big));
    // (a byte over: nothing of it, and its length is still told for the log)
    CHECK(read((dir + "/under.bin").c_str(), 32, big - 1, d, &len) == Read::OverBound && d.empty() && len == big);
    // (the answers the log names have words; the three that need no line have none)
    CHECK(*why(Read::OverBound) && *why(Read::Cut) && *why(Read::NoMemory) && *why(Read::Unsized) && *why(Read::Short));
    CHECK(!*why(Read::Ok) && !*why(Read::None));
    // ((Triforce, 11-18) the carry: a file for other shaders read all the same up to the limit (1536 MB unset, 0: never), and only
    // when it leaves room under the bound for as much again; one for these shaders is never "carried")
    CHECK(carry_limit(nullptr) == 1536 * MB && carry_limit("") == 1536 * MB && carry_limit("x") == 1536 * MB && carry_limit("0") == 0);
    CHECK(carry_limit("800") == 800 * MB);
    const uint64_t R32 = 32 * GB - 300 * MB, R16 = 16 * GB - 200 * MB;   // (what such PCs report)
    CHECK(carried(false, 860 * MB, 1536 * MB, 4096 * MB, R32) && carried(false, 1536 * MB, 1536 * MB, 4096 * MB, R32));
    CHECK(!carried(false, 1537 * MB, 1536 * MB, 4096 * MB, R32));          // (over the limit: a file carried and saved again)
    CHECK(!carried(false, 1100 * MB, 1536 * MB, 2048 * MB, R32) && carried(false, 1024 * MB, 1536 * MB, 2048 * MB, R32));   // (room for as much again)
    CHECK(carried(false, 1400 * MB, 1536 * MB, 0, R32));                    // (no bound)
    CHECK(!carried(false, 860 * MB, 1536 * MB, 4096 * MB, R16) && !carried(false, 860 * MB, 1536 * MB, 4096 * MB, 0));   // (16 GB, or RAM not known: never)
    CHECK(carried(false, 860 * MB, 1536 * MB, 8192 * MB, 64 * GB));
    CHECK(!carried(true, 860 * MB, 1536 * MB, 4096 * MB, R32) && !carried(false, 860 * MB, 0, 4096 * MB, R32) && !carried(false, 32, 1536 * MB, 4096 * MB, R32));
    return 0;
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    init = between(draw, "void pcache_init() {", "\n}\n")
    setb = 'pcache_bound_bytes = pcache_bound::bytes(getenv("RX_VK_PCACHE_MAX_MB"), ram);'
    readb = "pcache_bound::read(pcache_path.c_str(), 32, pcache_bound_bytes, data, &len)"
    if setb not in init or readb not in init or init.index(setb) > init.index(readb):
        raise SystemExit("pcache_init does not read the file through the bound, or reads before the bound is set")
    if "fread(" in init[init.index(setb):]:
        raise SystemExit("pcache_init reads the cache's file past the bounded read")
    if "ci.initialDataSize = data.size();" not in init:
        raise SystemExit("the cache is not made from what the bounded read gave")
    # ((Triforce, 11-18) what is for other shaders is read only when carried: over the stamp change at an update's preparation
    # (pcache_carry.h, tests/test_pcache_carry.py), or outside it by v0.5.2's size and RAM rule (pcache_bound::carried, with the
    # bound, where pcache_carry::plain_carry lets it: not over the preparation's own carry); else not at all
    carry = "pcache_bound::carried(pcache_same, (uint64_t)std::max<int64_t>(sz, 0), carry, pcache_bound_bytes, ram);"
    if carry not in init or init.index(setb) > init.index(carry):
        raise SystemExit("pcache_init does not decide the carry with the bound set, or not by pcache_bound::carried")
    if init.index("if (!pcache_same && !pcache_carried && !carried) {", init.index(carry)) > init.index(readb):
        raise SystemExit("a file for other shaders that is not carried is read")
    save = between(draw, "void pipeline_cache_save(bool force) {", "\n}\n")
    ask = "if (!pcache_bound::within(n, pcache_bound_bytes)) {"
    if ask not in save or save.index(ask) > save.index("std::vector<uint8_t> data(n);"):
        raise SystemExit("pipeline_cache_save fetches the cache's data before it asks the bound")
    if save.index(ask) < save.index("n != pcache_saved_size"):
        raise SystemExit("the bound is asked before the size is known")
    dev_md = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "`RX_VK_PCACHE_MAX_MB=<MB>`" not in dev_md:
        raise SystemExit("RX_VK_PCACHE_MAX_MB is not in docs/development.md's switches")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="pcache-bound-") as tmp:
        tmp = Path(tmp)
        (tmp / "t.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / "pcache-bound"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu/vk"), str(tmp / "t.cpp"),
                        "-o", str(exe)], check=True)
        data = tmp / "data"
        data.mkdir()
        r = subprocess.run([str(exe), str(data)], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"pcache_bound.h: {r.stdout.strip()}")
    print("PASS: a pipeline cache file over the bound is not read, at or under it it is read whole, and data over it is not written")


if __name__ == "__main__":
    main()
