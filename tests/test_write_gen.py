"""Page write generations are 64 bits and order across 2^32 (gpu/vk/write_gen.h, write_gen_table.h, vk_memory.cpp; no game, no GPU).

The renderer numbers every write to guest memory, and a texture or a mirror asks "written since my upload?" by comparing
numbers. They were 32 bits: after days of play the counter started again at 0, and from then on a stamp made before
compared newer than every later check and one made after it older, so textures and mirrors kept old contents until the
next start (NX2navi's counter of the same kind ran out after 15 to 66 hours).

What is checked:
- tests/test_write_gen.cpp, compiled against the tables as they are: the answers below 2^32 are the ones they were, and a
  write past 2^32 is newer than an upload before it (pages, whole 2 MB regions, the textures' same-bytes stamps).
- the same file against a copy of the headers with the generation narrowed to 32 bits: its WRAP checks fail. That is the
  proof that they would have caught the 32-bit counter, and it is why the checks are not vacuous.
- the type refuses to be narrowed: `uint32_t g = WriteGen{}` and `WriteGen g = 5` do not compile.
- vk_memory.cpp's gen_seed and next_gen, cut out and run: RX_VK_GEN_SEED starts the generations where it says (so that a
  play test crosses 2^32 in seconds), a bad value is 1, and the log gets its two lines once each.
- the source: the tables and every place a generation is kept hold a WriteGen, the 32-bit warning is gone, and no
  `uint32_t ...gen` is left but the three that count something else.

    python -I -B tests/test_write_gen.py [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
FLAGS = ["-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS"]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def swap(text, old, new, what):
    if text.count(old) != 1:
        raise SystemExit("gpu/vk/write_gen.h no longer has %s: the 32-bit copy of it cannot be made" % what)
    return text.replace(old, new)


def check_source():
    mem = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
    tex_h = (VK / "vk_texture.h").read_text(encoding="utf-8")
    tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    internal = (VK / "vk_internal.h").read_text(encoding="utf-8")
    for text, name, needles in (
            (mem, "vk_memory.cpp", (
                '#include "write_gen_table.h"', "write_gen::Table gens = [] {",
                # (the questions are the header's, the ones the test runs)
                "return gens.written_since(guest, size, gen);", "gens.written_runs(guest, size, gen, out, tex);",
                "gens.stamp_range(guest, size, g);", "inline WriteGen page_gen(uint64_t p) { return gens.page(p); }",
                "inline void stamp_page(uint64_t p, WriteGen g) {\n    gens.stamp_page(p, g);\n",
                # (Triforce: and the CPU's writes alone, the lazy write-back's rule 1: the same tables, the same generations)
                "write_gen::Table* cpu_gens = nullptr;", "if (cpu_gens) cpu_gens->stamp_page(p, g);",
                # (every place one is kept)
                "WriteGen synced_gen;", "struct CapEntry { WriteGen gen; uint32_t slot = 0; };", "std::map<WriteGen, CapBuf> cap_bufs;",
                "WriteGen cur_cap_gen;", "std::unordered_map<uint32_t, WriteGen> late_skipped_gen;",
                "std::unordered_map<uint32_t, WriteGen> gpu_pages;", "std::unordered_map<uint32_t, WriteGen> prot_pages;",
                "WriteGen pgen;", "WriteGen memory_page_stamp(uint64_t gpu) {", "WriteGen memory_generation() {",
                "for (Pool* p : mirror_synced) p->synced_gen = gens.now;")),
            (internal, "vk_internal.h", (
                '#include "write_gen.h"', "WriteGen memory_generation();", "WriteGen memory_page_stamp(uint64_t gpu);",
                "bool memory_written_since(uint64_t guest, uint64_t size, WriteGen gen);",
                "void memory_written_runs(uint64_t guest, uint64_t size, WriteGen gen,")),
            (tex_h, "vk_texture.h", ("WriteGen upload_gen;", "WriteGen gpu_gen;", "WriteGen release_gen;")),
            (tex, "vk_texture.cpp", (
                "std::map<uint64_t, std::pair<uint64_t, WriteGen>> gpu_owned;", "WriteGen gpu_owned_since(uint64_t gpu, uint64_t n) {",
                "bool texture_dirty(Texture& tex, WriteGen gen, Ranges& out, bool same_ok = false) {")),
            (replay, "vk_replay.cpp", ("WriteGen page;", "const WriteGen page = n == 4 ? memory_page_stamp(addr) : WriteGen{};"))):
        for needle in needles:
            if needle not in text:
                raise SystemExit("gpu/vk/%s no longer has: %s" % (name, needle))
    # (the 32-bit counter, its accessors and the line that warned of its end)
    for gone in ("uint32_t gen_now", "gen_now", "load_u32", "store_u32", "0xF0000000u", "close the game and start it again soon"):
        if gone in mem:
            raise SystemExit("gpu/vk/vk_memory.cpp still has the 32-bit generations' " + gone)
    # (what is still a uint32_t and named a generation counts something else: the host's pool table (compared for being the
    # same, never for order) and the mappings the texture's runs were made at)
    other = {"generation", "pool_cap_gen", "runs_maps_gen", "maps_gen"}
    for text, name in ((mem, "vk_memory.cpp"), (tex_h, "vk_texture.h"), (tex, "vk_texture.cpp"), (internal, "vk_internal.h")):
        for m in re.finditer(r"\buint32_t\b[ *&]*(\w*gen\w*)\s*(?:=|;|,|\)|\{)", text):
            if m.group(1) not in other:
                raise SystemExit("gpu/vk/%s keeps a generation in 32 bits: %s" % (name, m.group(0)))
    # (a generation handed to the log goes as its 64 bits)
    for text, name in ((mem, "vk_memory.cpp"), (tex, "vk_texture.cpp")):
        for m in re.finditer(r"gen(?:eration)?(?: now| at)? %u", text):
            raise SystemExit("gpu/vk/%s logs a generation with %%u: %s" % (name, m.group(0)))
    return mem


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    mem = check_source()
    with tempfile.TemporaryDirectory(prefix="write-gen-") as tmp:
        tmp = Path(tmp)
        # 1. the tables as they are
        exe = tmp / "write-gen"
        subprocess.run([args.cxx, *FLAGS, "-I", str(VK), str(ROOT / "tests/test_write_gen.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

        # 2. the same checks against 32-bit generations: the ones marked WRAP fail, and only those
        narrow = tmp / "narrow"
        narrow.mkdir()
        gen_h = (VK / "write_gen.h").read_text(encoding="utf-8")
        gen_h = swap(gen_h, "    uint64_t v = 0;\n", "    uint32_t v = 0;\n", "the generation's 64-bit value")
        gen_h = swap(gen_h, "constexpr explicit WriteGen(uint64_t x) : v(x) {}", "constexpr explicit WriteGen(uint64_t x) : v((uint32_t)x) {}",
                     "the constructor from a number")
        gen_h = swap(gen_h, "static_assert(sizeof(WriteGen) == 8 && alignof(WriteGen) == 8);\n", "", "the check of its size")
        (narrow / "write_gen.h").write_text(gen_h, encoding="utf-8")
        (narrow / "write_gen_table.h").write_text((VK / "write_gen_table.h").read_text(encoding="utf-8"), encoding="utf-8")
        exe32 = tmp / "write-gen-32"
        built = run([args.cxx, *FLAGS, "-I", str(narrow), str(ROOT / "tests/test_write_gen.cpp"), "-o", str(exe32)])
        if built.returncode:
            raise SystemExit("the 32-bit copy of the tables does not compile, so nothing shows the checks catch it:\n" + built.stdout)
        ran = run([str(exe32)])
        failed = [line for line in ran.stdout.splitlines() if line.startswith("FAIL: ")]
        if ran.returncode != 1 or not failed:
            raise SystemExit("with 32-bit generations the checks still pass: they do not test the width\n" + ran.stdout)
        if any(not line.startswith("FAIL: WRAP: ") for line in failed):
            raise SystemExit("with 32-bit generations a check below 2^32 fails too:\n" + ran.stdout)
        for must in ("a generation past 2^32 orders after one before it", "a page written after 2^32 is written since an upload before it",
                     "a page written before 2^32 is not written since a sync after it", "no generation is 0",
                     "a region marked whole after 2^32 is written since an upload before it",
                     "a same-bytes stamp past 2^32 tells a texture the generation before it"):
            if "FAIL: WRAP: " + must not in ran.stdout:
                raise SystemExit("with 32-bit generations this check passes: " + must + "\n" + ran.stdout)

        # 3. the type refuses the ways a generation gets narrowed or a plain number taken for one
        for body, what in (("uint32_t g = mkwvk::WriteGen{}; (void)g;", "a generation put into a uint32_t"),
                           ("mkwvk::WriteGen g = 5; (void)g;", "a plain number taken for a generation"),
                           ("mkwvk::WriteGen g; unsigned u = (unsigned)g; (void)u;", "a generation cast to 32 bits"),
                           ("mkwvk::WriteGen g; (void)(g > 3);", "a generation compared with a plain number")):
            src = tmp / "refuse.cpp"
            src.write_text('#include "write_gen.h"\nint main() { %s return 0; }\n' % body, encoding="utf-8")
            if run([args.cxx, *FLAGS, "-fsyntax-only", "-I", str(VK), str(src)]).returncode == 0:
                raise SystemExit("gpu/vk/write_gen.h: this compiles: " + what)
        src.write_text('#include "write_gen.h"\nint main() { mkwvk::WriteGen a, b{7}; return a < b && b && !a ? 0 : 1; }\n', encoding="utf-8")
        subprocess.run([args.cxx, *FLAGS, "-fsyntax-only", "-I", str(VK), str(src)], check=True)

        # 4. the seed and the log's two lines: vk_memory.cpp's own functions
        seed = block(mem, "uint64_t gen_seed() {")
        table = mem[mem.index("write_gen::Table gens = [] {"):]
        table = table[:table.index("}();") + 4]
        next_gen = block(mem, "inline WriteGen next_gen() {")
        fixture = tmp / "seed.cpp"
        fixture.write_text('''#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "write_gen_table.h"
namespace mkwvk {
static std::vector<std::string> lines;
static void logf(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    lines.push_back(b);
}
const std::chrono::steady_clock::time_point gens_made_at = std::chrono::steady_clock::now();
%s
%s
%s
}
using namespace mkwvk;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%%d %%s\\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char** argv) {
    const unsigned long long want = strtoull(argc > 1 ? argv[1] : "1", nullptr, 0);
    CHECK(gen_seed() == want && gens.now == WriteGen{want});
    WriteGen last = gens.now;
    int seed_lines = 0, wrap_lines = 0;
    for (int i = 0; i < 64; i++) {
        const size_t had = lines.size();
        const WriteGen g = next_gen();
        CHECK(g.v == last.v + 1 && g > last && gens.now == g);
        last = g;
        for (size_t k = had; k < lines.size(); k++) {
            const bool seeded = lines[k].find("RX_VK_GEN_SEED") != std::string::npos;
            const bool wrapped = lines[k].find("4294967296 write generations") != std::string::npos;
            CHECK(seeded != wrapped);
            if (seeded) CHECK(i == 0 && lines[k].find(std::to_string(want)) != std::string::npos);
            if (wrapped) CHECK(g.v == 1ull << 32 && lines[k].find("hours") != std::string::npos);
            seed_lines += seeded, wrap_lines += wrapped;
        }
    }
    CHECK(seed_lines == (want != 1));
    CHECK(wrap_lines == (want < (1ull << 32) && want + 64 >= (1ull << 32)));
    return 0;
}
''' % (seed, table, next_gen), encoding="utf-8")
        exe_seed = tmp / "seed"
        subprocess.run([args.cxx, *FLAGS, "-I", str(VK), str(fixture), "-o", str(exe_seed)], check=True)
        env = {k: v for k, v in os.environ.items() if k != "RX_VK_GEN_SEED"}
        for value, want in ((None, 1), ("", 1), ("0", 1), ("1", 1), ("junk", 1), ("77", 77), ("0xFFFFFFF0", 0xFFFFFFF0),
                            ("4294967295", 0xFFFFFFFF), ("0x100000000", 1 << 32), ("0x7fffffffffffffff", 1 << 62),
                            ("99999999999999999999999999", 1 << 62)):
            e = dict(env) if value is None else dict(env, RX_VK_GEN_SEED=value)
            r = run([str(exe_seed), str(want)], env=e)
            if r.returncode:
                raise SystemExit("RX_VK_GEN_SEED=%r: the generations do not start at %d, or the log's lines are wrong\n%s" % (value, want, r.stdout))
    print("page write generations: the tables order across 2^32, a 32-bit copy of them fails the same checks, the seed works")


if __name__ == "__main__":
    main()
