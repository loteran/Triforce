"""A stage's SPIR-V kept packed, and its module made again for each pipeline (gpu/vk/spv_store.h; vk_shader.cpp
program_pack and StageModule, on unless RX_VK_SPV_PACK=0; no game/GPU).

The header is compiled and run as it is, on authored words only:
- the packing gives every block back word for word: blocks shaped like SPIR-V, random ones, zeros, runs long enough for
  every count byte, and each length from nothing to a few hundred words;
- unpack refuses every cut of a packed copy and whatever random bytes it is given, and writes nothing outside its block
  (the block stands between guard bytes);
- a store is filled only by words that come back, hands one module to the builders that hold it at once, makes it by
  the words it was filled with, and destroys it when the last of them is done, never under another;
- as the real readers run: several threads, each making pipelines of the same few stages over and over (the workers of
  vk_jobs.h and the replay thread), with a stand-in driver that knows which modules are alive and what each was made of.
Then the source: programs are packed before they are ready and only while the switch is on, compute programs keep their
modules, those let go soon after they are made (the prepare's, the borrowed ones) and those a draw reads itself get no
store, a module made with no store takes away any store an earlier module of its handle left, every program module is let
go through stage_module_destroy, and no pipeline is made from a stage's module field where a packed stage can arrive.
python -I -B tests/test_spv_store.py --cxx clang++
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#define private public   // (the test changes a store's packed bytes behind its back: what memory gone bad would do)
#include "spv_store.h"
#undef private
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); return 1; } } while (0)

// Words shaped like a module's: a header, then instructions (length and opcode in one word, ids that count up, small operands).
static std::vector<uint32_t> shaped(std::mt19937& rng, size_t words) {
    std::vector<uint32_t> w{0x07230203, 0x00010600, 0x000B0000, 0, 0};
    uint32_t id = 1;
    while (w.size() < words) {
        const uint32_t n = 2 + rng() % 5, op = 20 + rng() % 12 * 7;
        w.push_back(n << 16 | op);
        w.push_back(id++);
        for (uint32_t i = 2; i < n; i++) w.push_back(rng() % 20 ? id - rng() % (id < 64 ? id : 64) : rng());
    }
    w.resize(words);
    w[3] = id;
    return w;
}
static bool round_trip(const std::vector<uint32_t>& w, size_t* packed = nullptr) {
    std::vector<uint8_t> z;
    std::vector<uint32_t> back{1, 2, 3};
    spv_store::pack(w.data(), w.size(), z);
    if (packed) *packed = z.size();
    return spv_store::unpack(z.data(), z.size(), w.size(), back) && back == w;
}

// The stand-in driver: which modules are alive, and the hash of the words each was made of.
static std::mutex driver_mu;
static std::map<int, uint64_t> alive;
static std::atomic<int> next_module{1}, made{0}, dropped{0}, bad{0};
static int make(const uint32_t* w, size_t n) {
    const int m = next_module++;
    std::lock_guard lk(driver_mu);
    alive[m] = spv_store::hash(w, n);
    made++;
    return m;
}
static void drop(int m) {
    std::lock_guard lk(driver_mu);
    if (!alive.erase(m)) bad++;   // (destroyed twice, or never made)
    dropped++;
}
static bool is_alive_as(int m, uint64_t h) {
    std::lock_guard lk(driver_mu);
    const auto it = alive.find(m);
    return it != alive.end() && it->second == h;
}

int main() {
    std::mt19937 rng(20261115);
    // ---- the packing
    for (size_t n = 0; n < 300; n++) CHECK(round_trip(shaped(rng, n)));
    size_t plain = 0, packed = 0;
    for (int i = 0; i < 40; i++) {
        const auto w = shaped(rng, 2000 + rng() % 30000);
        size_t z = 0;
        CHECK(round_trip(w, &z));
        plain += w.size() * 4, packed += z;
    }
    CHECK(packed * 3 < plain * 2);   // (words of that shape pack to under two thirds; the game's own to 0.37, spv_store.h)
    for (int i = 0; i < 20; i++) {   // (nothing to find in them: all literals, long counts)
        std::vector<uint32_t> w(1 + rng() % 5000);
        for (uint32_t& v : w) v = rng();
        CHECK(round_trip(w));
    }
    for (size_t n : {1u, 2u, 3u, 4u, 5u, 67u, 68u, 69u, 100000u}) {   // (one long match, its length in many count bytes)
        CHECK(round_trip(std::vector<uint32_t>(n, 0)));
        CHECK(round_trip(std::vector<uint32_t>(n, 0x01020304)));
    }
    {   // (matches at the greatest distance and past it, and one that runs into itself)
        std::vector<uint32_t> w;
        for (uint32_t i = 0; i < 40000; i++) w.push_back(i * 2654435761u);
        for (uint32_t i = 0; i < 40000; i++) w.push_back(i * 2654435761u);
        for (uint32_t i = 0; i < 9; i++) w.push_back(i % 3);
        CHECK(round_trip(w));
    }
    // ---- what is not a packed copy is refused, and nothing is written outside the block
    {
        const auto w = shaped(rng, 400);
        std::vector<uint8_t> z;
        spv_store::pack(w.data(), w.size(), z);
        std::vector<uint32_t> back;
        for (size_t cut = 0; cut < z.size(); cut++) CHECK(!spv_store::unpack(z.data(), cut, w.size(), back) && back.empty());
        CHECK(!spv_store::unpack(z.data(), z.size(), w.size() - 1, back) && !spv_store::unpack(z.data(), z.size(), w.size() + 1, back));
        const size_t n = w.size() * 4, guard = 64;
        std::vector<uint8_t> block(n + 2 * guard);
        for (int i = 0; i < 20000; i++) {
            std::vector<uint8_t> junk = z;
            if (i % 2) {   // (a packed copy with some bytes changed; else random bytes)
                for (int k = 0; k < 1 + i % 5; k++) junk[rng() % junk.size()] = (uint8_t)rng();
            } else {
                junk.resize(1 + rng() % 600);
                for (uint8_t& b : junk) b = (uint8_t)(rng() % 3 ? rng() : rng() % 2 ? 0xFF : 0x0F);
            }
            std::fill(block.begin(), block.end(), 0xA5);
            spv_store::detail::lz_unpack(junk.data(), junk.size(), block.data() + guard, n);   // (yes or no: neither may reach out)
            for (size_t g = 0; g < guard; g++) CHECK(block[g] == 0xA5 && block[guard + n + g] == 0xA5);
        }
        // (a match that would begin before the block: refused, not read)
        const uint8_t before[] = {0x00, 0x01, 0x00, 0x00}, inside[] = {0x10, 0x07, 0x01, 0x00, 0x00};
        CHECK(!spv_store::detail::lz_unpack(before, sizeof before, block.data() + guard, 4));
        CHECK(spv_store::detail::lz_unpack(inside, sizeof inside, block.data() + guard, 5) && block[guard + 4] == 7);
        const uint8_t zero_distance[] = {0x10, 0x07, 0x00, 0x00, 0x00};
        CHECK(!spv_store::detail::lz_unpack(zero_distance, sizeof zero_distance, block.data() + guard, 5));
    }
    // ---- a store whose packed bytes changed after it was filled makes no module (the hash taken when it was filled)
    {
        const auto w = shaped(rng, 3000);
        spv_store::Code<int> code;
        CHECK(code.fill(w.data(), w.size()));
        std::vector<uint32_t> back;
        size_t at = 0;
        for (; at < code.z_.size(); at++) {   // (a byte whose change still unpacks, to other words: a literal)
            code.z_[at] ^= 0x40;
            if (spv_store::unpack(code.z_.data(), code.z_.size(), w.size(), back) && back != w) break;
            code.z_[at] ^= 0x40;
        }
        CHECK(at < code.z_.size());
        int asked = 0;
        CHECK(code.pin([&](const uint32_t*, size_t) { return asked++, 7; }) == 0 && !asked && code.pins() == 0);
        code.z_[at] ^= 0x40;   // (as it was: a module again)
        CHECK(code.pin([&](const uint32_t*, size_t) { return asked++, 7; }) == 7 && asked == 1);
        code.unpin([](int) {});
    }
    // ---- one store
    {
        const auto w = shaped(rng, 5000);
        const uint64_t h = spv_store::hash(w.data(), w.size());
        spv_store::Code<int> code;
        CHECK(!code.filled() && code.pin(make) == 0 && code.pins() == 0);   // (not filled: no module, nothing owed)
        code.unpin(drop);
        CHECK(dropped == 0);
        CHECK(!code.fill(w.data(), 0) && !code.filled());
        CHECK(code.fill(w.data(), w.size()) && code.filled() && code.words() == w.size() && code.packed_bytes() < w.size() * 4);
        const int a = code.pin(make);
        CHECK(a && made == 1 && is_alive_as(a, h) && code.pins() == 1);
        const int b = code.pin(make);   // (a second builder while the first holds it: the same module)
        CHECK(b == a && made == 1 && code.pins() == 2);
        code.unpin(drop);
        CHECK(dropped == 0 && is_alive_as(a, h));   // (not under the other)
        code.unpin(drop);
        CHECK(dropped == 1 && !is_alive_as(a, h) && code.pins() == 0);
        code.unpin(drop);   // (one too many: nothing)
        CHECK(dropped == 1 && bad == 0);
        const int c = code.pin(make);   // (the next pipeline: made again, of the same words)
        CHECK(c && c != a && made == 2 && is_alive_as(c, h) && code.made() == 2);
        code.unpin(drop);
        CHECK(code.pin([](const uint32_t*, size_t) { return 0; }) == 0 && code.pins() == 0);   // (the driver makes none)
        const int d = code.pin(make);
        CHECK(d && is_alive_as(d, h));
        code.unpin(drop);
        CHECK(made == 3 && dropped == 3 && bad == 0);
    }
    // ---- as the readers run: threads that make pipelines of the same stages, over and over
    {
        made = dropped = 0;
        constexpr int kStages = 6, kThreads = 8, kRounds = 4000;
        std::vector<std::vector<uint32_t>> words;
        std::vector<std::shared_ptr<spv_store::Code<int>>> stages;
        std::vector<uint64_t> hashes;
        for (int i = 0; i < kStages; i++) {
            words.push_back(shaped(rng, 300 + 400 * i));
            hashes.push_back(spv_store::hash(words.back().data(), words.back().size()));
            stages.push_back(std::make_shared<spv_store::Code<int>>());
            CHECK(stages.back()->fill(words.back().data(), words.back().size()));
        }
        std::atomic<int> wrong{0}, built{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; t++)
            threads.emplace_back([&, t] {
                std::mt19937 r(1000 + t);
                for (int i = 0; i < kRounds; i++) {
                    // (a pipeline of two stages, as a vertex and a fragment stage: both held for the creation)
                    const int s0 = r() % kStages, s1 = r() % kStages;
                    const int m0 = stages[s0]->pin(make), m1 = stages[s1]->pin(make);
                    if (!m0 || !m1 || !is_alive_as(m0, hashes[s0]) || !is_alive_as(m1, hashes[s1])) wrong++;
                    if (i % 16 == 0) std::this_thread::yield();   // (the driver at work)
                    if (!is_alive_as(m0, hashes[s0]) || !is_alive_as(m1, hashes[s1])) wrong++;   // (still, at its end)
                    stages[s1]->unpin(drop), stages[s0]->unpin(drop);
                    built++;
                }
            });
        for (std::thread& t : threads) t.join();
        CHECK(wrong == 0 && bad == 0 && built == kThreads * kRounds);
        CHECK(made == dropped && made > 0 && made <= 2 * kThreads * kRounds);
        for (const auto& s : stages) CHECK(s->pins() == 0);
        std::lock_guard lk(driver_mu);
        CHECK(alive.empty());   // (nothing left alive once no pipeline is being made)
    }
    return 0;
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def check_source():
    shader = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    head = (ROOT / "gpu/vk/vk_shader.h").read_text(encoding="utf-8")
    # (on unless RX_VK_SPV_PACK=0, and the switch read in one place)
    if 'static const bool on = !(getenv("RX_VK_SPV_PACK") && atoi(getenv("RX_VK_SPV_PACK")) == 0);' not in \
            between(shader, "bool spv_pack_on() {", "\n}\n"):
        raise SystemExit("RX_VK_SPV_PACK is not on by default, or =0 does not turn it off")
    if shader.count('getenv("RX_VK_SPV_PACK")') != 2 or 'getenv("RX_VK_SPV_PACK")' in draw:
        raise SystemExit("RX_VK_SPV_PACK is read outside spv_pack_on")
    note = between(shader, "void pack_note(", "\n}\n")
    if "if (!m || !spv_pack_on()) return;" not in note or "code->fill(words, n)" not in note or "pack_made.erase(m)" not in note:
        raise SystemExit("pack_note keeps a store without the switch, or one whose words did not come back")
    # (a module made with no store takes away the store an earlier module of its handle left: program_pack would give that
    # one, another stage's code, to this stage)
    at = note.index("if (pack_skip) {")
    if "pack_made.erase(m);" not in note[at:note.index("return;", at)] or at > note.index("code->fill(words, n)"):
        raise SystemExit("pack_note leaves, for a module it makes no store for, the store an earlier module of its handle left")
    # (no store for the programs let go soon after they are made, for compute programs, nor for a draw's own read)
    if "uint64_t* key_out) {\n    const PackSkip skip;   // ((Triforce) its callers let it go" not in shader:
        raise SystemExit("the prepare's programs, which it lets go, are packed")
    load = between(shader, "void program_cache_load(uint64_t key,", "\n}\n")
    if "const PackSkip skip;\n        program_cache_read(disk_in, &disk_mu, key, recs, variants);" not in load:
        raise SystemExit("a draw's own read (or a borrow) packs its program on the thread replaying")
    read = between(shader, "void program_cache_read(FILE* in,", "\n}\n")
    if "if (p->compute) compute_skip.emplace();" not in read or read.index("compute_skip.emplace()") > read.index("make_module("):
        raise SystemExit("a compute program read from the cache is packed")
    if "const PackSkip skip;   // ((Triforce) a compute program keeps its module: no store for it)\n" \
       "                st.module = make_module(spirv);" not in shader:
        raise SystemExit("a compute program translated is packed")
    # (every program module let go goes through stage_module_destroy, the memory line's count: program_pack's own aside)
    lets = [m.start() for m in re.finditer(r"vkDestroyShaderModule\(dev\.device, st\.module", shader)]
    if len(lets) != 1 or not shader.index("void program_pack(Program& p) {") < lets[0] < shader.index("VkShaderModule make_module("):
        raise SystemExit("vk_shader.cpp lets a program's module go past stage_module_destroy")
    if "vkDestroyShaderModule(dev.device, st.module" in draw or draw.count("stage_module_destroy(st.module)") != 2:
        raise SystemExit("vk_draw.cpp lets a program's module go past stage_module_destroy")
    pack = between(shader, "void program_pack(Program& p) {", "\n}\n")
    if "if (!spv_pack_on()) return;" not in pack or "if (p.failed || p.compute) continue;" not in pack:
        raise SystemExit("program_pack packs without the switch, or a compute program (whose makers read the module's field)")
    if pack.index("if (p.failed || p.compute) continue;") > pack.index("vkDestroyShaderModule("):
        raise SystemExit("program_pack destroys a module before it knows the program is one to pack")
    # (every place a program becomes ready packs it first: there are three)
    ready = [m.start() for m in re.finditer(r"ready\.store\(true, std::memory_order_release\);", shader)]
    if len(ready) != 3:
        raise SystemExit("vk_shader.cpp makes programs ready in %d places, not the three this test knows" % len(ready))
    for at in ready:
        before = shader[shader.rindex("\n", 0, shader.rindex("\n", 0, at)):at]
        if "program_pack(" not in before:
            raise SystemExit("a program becomes ready without program_pack before it")
    # (the module made again is made of the store's words and destroyed by the store's last holder)
    mod = between(shader, "StageModule::StageModule(const Stage& st) {", "StageModule::~StageModule() {")
    if "if (!st.code) {\n        m_ = st.module;\n        return;\n    }" not in mod or "m_ = st.code->pin(" not in mod:
        raise SystemExit("StageModule does not give an unpacked stage its own module, or a packed one a pinned one")
    if "code_->unpin(" not in between(shader, "StageModule::~StageModule() {", "\n}\n"):
        raise SystemExit("StageModule does not let its pin go")
    if "std::shared_ptr<spv_store::Code<VkShaderModule>> code;" not in between(head, "struct Stage {", "\n};\n"):
        raise SystemExit("Stage has no store")
    # (no pipeline from the field of a stage that may be packed: build_graphics and the fallback's compute pipelines take
    # StageModule; what is left reads stages[0] of a compute program, which is never packed)
    if "si.module = st.module;" in draw or "shader_reads_exact(st.module" in draw:
        raise SystemExit("vk_draw.cpp reads a stage's module field where a packed stage can arrive")
    build = between(draw, "VkPrimitiveTopology topo, int part, bool* cold) {", "\n}\n")
    if "modules.emplace_back(st);" not in build or "si.module = modules.back().get();" not in build or \
            "if (!si.module) return VK_NULL_HANDLE;" not in build:
        raise SystemExit("build_graphics does not take its stages' modules through StageModule, or builds without one")
    if build.index("std::vector<StageModule> modules;") > build.index("cached_graphics(cache, &ci, &pl)"):
        raise SystemExit("build_graphics's modules do not live as long as the creation")
    fb = between(draw, "bool fallback_compute_pipelines(", "\n}\n") if "bool fallback_compute_pipelines(" in draw else ""
    if fb and ("const StageModule module(st);" not in fb or "si.module ? guarded_compute(" not in fb):
        raise SystemExit("the fallback's compute pipelines are made from a module field, or without a module")
    fields = [m.start() for m in re.finditer(r"stages\[0\]\.module", draw)]
    for at in fields:
        line = draw[draw.rindex("\n", 0, at):draw.index("\n", at)]
        if "ci.stage.module = " not in line:
            raise SystemExit("vk_draw.cpp reads stages[0].module for something other than a compute pipeline:" + line)
    if "shader_reads_exact(stage_exact_key(st), w.dstBinding)" not in draw:
        raise SystemExit("the texel-position answer is not asked by the key that stands for a packed stage's module")
    dev_md = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "`RX_VK_SPV_PACK=0`" not in dev_md:
        raise SystemExit("RX_VK_SPV_PACK is not in docs/development.md's switches")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="spv-store-") as tmp:
        tmp = Path(tmp)
        (tmp / "t.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / "spv-store"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread", "-I" + str(ROOT / "gpu/vk"),
                        str(tmp / "t.cpp"), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
        if r.returncode:
            raise SystemExit(f"spv_store.h: {r.stdout.strip() or r.returncode}")
    print("PASS: packed SPIR-V comes back word for word, what is not a packed copy is refused, and a module is shared by its "
          "builders and destroyed after the last")


if __name__ == "__main__":
    main()
