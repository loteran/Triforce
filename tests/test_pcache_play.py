"""The pipeline cache's play file (gpu/vk/pcache_play.h; vk_draw.cpp pcache_play_open, cached_create, pcache_play_save,
pcache_main_written, pipeline_cache_save). NX2navi's play cache on Triforce's cache; no game, GPU or driver.

Once the preparation is done, what a run compiles goes into a second cache with a file of its own, so that a save after a few
new pipelines writes that small file instead of the whole 2.6 GB main one (the owner's test18 start wrote all of it after its
precompile, for 116 KB new). What must hold:
- pcache_play.h, compiled and run against a stand-in driver: a creation with the main cache asks it first without compiling
  and goes to the play cache only when that answers "compile required", with the creation's own flags (a pipeline an earlier
  run compiled comes back, one asked of the caches only is answered by the play cache too, the rest is compiled into it); the
  creation's info is left as it was; a fault the guard reports is returned as it is; without the route (no play cache, a
  preparation under way, another cache) the creation is the one before; the fold is due at its size; a save writes the play
  file when its data changed and the main file only without a play cache or after builds into the main cache;
- pcache_play_save and pcache_main_written, cut out of vk_draw.cpp and run with a stand-in driver on a folder of their own:
  the play file and its .for written when the data changed, not when it did not, the main file asked for after builds into
  it; the play file and its .for removed once the main file is written after a fold;
- the source (read): pcache_init opens the play cache; it needs pipelineCreationCacheControl, its own .for, this device's
  header, and folds at its size with vkMergePipelineCaches (in the loader's list); build_graphics and the dispatch's compute
  pipelines go through cached_graphics and cached_compute with the main cache through its gate; prepare_run builds into the
  main cache; pipeline_cache_save saves the play file first, the main file too during a preparation, and counts the main
  file saved after it is in place; the switches are in the development guide.

    python -I -B tests/test_pcache_play.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

RULES = r"""
#include "pcache_play.h"
#include <cstdio>
#include <set>
#include <string>
using namespace pcache_play;
enum Res { OK = 0, COMPILE_REQUIRED = 1000297000, FAULT = -77 };
constexpr uint32_t FAIL = 0x100;
struct Info { uint32_t flags; int key; };
std::set<int> main_has = {1}, play_has = {2};
int calls_main, calls_play, compiles;
uint32_t last_flags_play;
bool fault_main;
Res make(int cache, const Info* ci, int* out) {
    if (cache == 10) {   // (the main cache)
        calls_main++;
        if (fault_main) return FAULT;
        if (main_has.count(ci->key)) return *out = 100 + ci->key, OK;
        if (ci->flags & FAIL) return COMPILE_REQUIRED;
        compiles++, main_has.insert(ci->key);
        return *out = 100 + ci->key, OK;
    }
    calls_play++, last_flags_play = ci->flags;
    if (play_has.count(ci->key)) return *out = 200 + ci->key, OK;
    if (ci->flags & FAIL) return COMPILE_REQUIRED;
    compiles++, play_has.insert(ci->key);
    return *out = 200 + ci->key, OK;
}
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int reset() { return calls_main = calls_play = compiles = 0, last_flags_play = 0, 0; }
int main() {
    // (routes: the main cache, a play cache, no preparation)
    CHECK(routes(true, true, false));
    CHECK(!routes(false, true, false) && !routes(true, false, false) && !routes(true, true, true));
    // (the fold at its size; never at 0)
    CHECK(fold_due(64ull << 20, 64ull << 20) && !fold_due((64ull << 20) - 1, 64ull << 20) && !fold_due(1ull << 40, 0));
    // (a save: the play file when it changed, the main file without a play cache or after builds into it)
    CHECK(save_what(false, 0, 0, 0).main && !save_what(false, 5, 0, 0).play);
    CHECK(save_what(true, 100, 0, 0).play && !save_what(true, 100, 0, 0).main);
    CHECK(!save_what(true, 100, 100, 0).play && !save_what(true, 0, 0, 0).play);
    CHECK(save_what(true, 100, 100, 3).main && !save_what(true, 100, 100, 3).play);
    int out = 0;
    bool into = false;
    // (no route: one creation with the main cache, the caller's flags)
    reset();
    Info c{0, 3};
    CHECK(create(false, 10, 20, &c, &out, FAIL, COMPILE_REQUIRED, make, &into) == OK && out == 103);
    CHECK(calls_main == 1 && calls_play == 0 && compiles == 1 && !into);
    // (routed, the main cache has it: once, without compiling, and nothing of the play cache)
    reset(), into = false;
    Info a{0, 1};
    CHECK(create(true, 10, 20, &a, &out, FAIL, COMPILE_REQUIRED, make, &into) == OK && out == 101);
    CHECK(calls_main == 1 && calls_play == 0 && compiles == 0 && !into && a.flags == 0);
    // (routed, an earlier run's in the play cache: from it, not compiled)
    reset(), into = false;
    Info b{0, 2};
    CHECK(create(true, 10, 20, &b, &out, FAIL, COMPILE_REQUIRED, make, &into) == OK && out == 202);
    CHECK(calls_main == 1 && calls_play == 1 && compiles == 0 && into && last_flags_play == 0);
    // (routed, asked of the caches only and in neither: compile required, from the play cache, nothing compiled)
    reset(), into = false;
    Info cold{FAIL, 5};
    CHECK(create(true, 10, 20, &cold, &out, FAIL, COMPILE_REQUIRED, make, &into) == COMPILE_REQUIRED);
    CHECK(calls_play == 1 && compiles == 0 && last_flags_play == FAIL && cold.flags == FAIL);
    // (routed, new: compiled into the play cache, not the main one, and found there after)
    reset(), into = false;
    Info n{0, 6};
    CHECK(create(true, 10, 20, &n, &out, FAIL, COMPILE_REQUIRED, make, &into) == OK && out == 206 && compiles == 1);
    CHECK(!main_has.count(6) && play_has.count(6) && n.flags == 0);
    reset();
    CHECK(create(true, 10, 20, &n, &out, FAIL, COMPILE_REQUIRED, make, &into) == OK && out == 206 && compiles == 0);
    // (a fault the guard reports with the main cache: returned, the play cache not asked)
    reset(), fault_main = true;
    Info f{0, 7};
    CHECK(create(true, 10, 20, &f, &out, FAIL, COMPILE_REQUIRED, make, &into) == FAULT && calls_play == 0);
    std::printf("PASS rules\n");
    return 0;
}
"""

SAVE = r"""
#include "pcache_play.h"
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
constexpr unsigned MOVEFILE_REPLACE_EXISTING = 1;
int MoveFileExA(const char* from, const char* to, unsigned) { std::remove(to); return std::rename(from, to) == 0; }
#endif
#include "file_whole.h"   // ((Triforce, handoff §8) the .for files whole)
using VkResult = int;
using VkPipelineCache = int;
constexpr VkResult VK_SUCCESS = 0;
struct { int device = 1; } dev;
VkPipelineCache pcache_play_cache = 0;
std::string pcache_play_path;
std::atomic<size_t> pcache_play_saved{0};
std::atomic<int64_t> pcache_main_built{0}, st_play_made{0};
std::atomic<bool> pcache_fold{false};
size_t play_size = 0;
int fetches = 0;
unsigned long long program_cache_stamp() { return 0x0123456789abcdefull; }
void logf(const char*, ...) {}
VkResult vkGetPipelineCacheData(int, VkPipelineCache cache, size_t* n, void* data) {
    if (cache != pcache_play_cache || !cache) return -1;
    if (!data) return *n = play_size, VK_SUCCESS;
    fetches++;
    std::memset(data, 'P', *n);
    return VK_SUCCESS;
}
@CODE@
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
long size_of(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n;
}
std::string text_of(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return "";
    char b[32]{};
    const size_t n = std::fread(b, 1, sizeof b - 1, f);
    std::fclose(f);
    return std::string(b, n);
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    pcache_play_path = std::string(argv[1]) + "/mkw_vk_pipelines_play.bin";
    const std::string fore = pcache_play_path + ".for";
    // (no play cache: the main file, nothing written here)
    CHECK(pcache_play_save(0) && size_of(pcache_play_path) < 0);
    // (a play cache whose data grew: its file and the shaders' stamp; the main file not asked for)
    pcache_play_cache = 7, play_size = 100;
    CHECK(!pcache_play_save(0));
    CHECK(size_of(pcache_play_path) == 100 && text_of(fore) == "0123456789abcdef" && pcache_play_saved == 100 && fetches == 1);
    // (the same size: not fetched nor written again)
    std::remove(pcache_play_path.c_str());
    CHECK(!pcache_play_save(0) && size_of(pcache_play_path) < 0 && fetches == 1);
    // (grown, with builds into the main cache since its save: both)
    play_size = 150;
    CHECK(pcache_play_save(3) && size_of(pcache_play_path) == 150 && pcache_play_saved == 150 && fetches == 2);
    // (the main file in place: those builds counted saved, the others kept; no fold, the play file stays)
    pcache_main_built = 5;
    pcache_main_written(3);
    CHECK(pcache_main_built == 2 && size_of(pcache_play_path) == 150);
    // (after a fold at this start: the play file and its stamp removed, once)
    pcache_fold = true;
    pcache_main_written(2);
    CHECK(pcache_main_built == 0 && !pcache_fold && size_of(pcache_play_path) < 0 && size_of(fore) < 0);
    std::printf("PASS save\n");
    return 0;
}
"""


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit(where + " no longer has: " + needle)


def check_source(draw):
    init = block(draw, "void pcache_init() {")
    need("vk_draw.cpp pcache_init", init, ["pcache_play_open(pcache_same);"])
    opened = block(draw, "void pcache_play_open(bool same) {")
    need("vk_draw.cpp pcache_play_open", opened,
         ['getenv("RX_VK_PCACHE_PLAY_FILE")', "dev.vk13.pipelineCreationCacheControl", '(pcache_play_path + ".for")',
          "program_cache_stamp()", "dev.props.pipelineCacheUUID", "pcache_bound::read(pcache_play_path.c_str()",
          "pcache_play::fold_due(held, fold)", "vkMergePipelineCaches(dev.device, pcache, 1, &made)", "pcache_fold = true;",
          'getenv("RX_VK_PCACHE_FOLD_MB")', "pcache_play_cache = made;", "pcache_play_saved = held;"])
    if opened.index("pcache_fold = true;") > opened.index("pcache_play_cache = made;"):
        raise SystemExit("vk_draw.cpp pcache_play_open keeps a play cache after folding it")
    create = block(draw, "VkResult cached_create(VkPipelineCache cache, const Info* ci, VkPipeline* pl, Guarded guarded) {")
    need("vk_draw.cpp cached_create", create,
         ["pcache_play::routes(cache && cache == pcache, pcache_play_cache != VK_NULL_HANDLE,",
          "pcache_preparing.load(std::memory_order_relaxed) > 0", "VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT",
          "VK_PIPELINE_COMPILE_REQUIRED", "pcache_main_built++"])
    need("vk_draw.cpp cached_graphics / cached_compute", draw,
         ["return cached_create(cache, ci, pl, guarded_graphics);", "return cached_create(cache, ci, pl, guarded_compute);"])
    build = draw[draw.index("VkPipeline build_graphics(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology "
                            "topo, int part, bool* cold) {"):]
    build = build[:build.index("\n}\n")]
    if build.count("cached_graphics(cache, &ci, &pl)") != 2 or "guarded_graphics(" in build:
        raise SystemExit("vk_draw.cpp build_graphics: its creation and its retry are not through the play cache")
    disp = block(draw, "void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {")
    if disp.count("cached_compute(pcache_use().cache,") != 3 or "guarded_compute(" in disp:
        raise SystemExit("vk_draw.cpp dispatch: its compute pipelines are not through the play cache, or not through the gate")
    prep = block(draw, "bool prepare_run(void (*progress)(size_t done, size_t total), bool background) {")
    if not prep.split("\n")[1].strip().startswith("const PcachePreparing preparing;"):
        raise SystemExit("vk_draw.cpp prepare_run does not build into the main cache for its whole length")
    save = block(draw, "void pipeline_cache_save(bool force) {")
    need("vk_draw.cpp pipeline_cache_save", save,
         ["const int64_t main_built = pcache_main_built.load();",
          "if (!pcache_play_save(main_built) && pcache_preparing.load() == 0) return;", "pcache_main_written(main_built);"])
    if not save.index("if (!pcache_play_save(main_built) && pcache_preparing.load() == 0) return;") < \
            save.index("const auto use = pcache_use();") or \
            not save.index("pcache_saved_size = n;\n") < save.index("pcache_main_written(main_built);"):
        raise SystemExit("vk_draw.cpp pipeline_cache_save: the play file not first, or the main file counted saved before it is")
    loader = (ROOT / "gpu/vk/vk_loader.h").read_text(encoding="utf-8")
    need("vk_loader.h", loader, ["X(vkMergePipelineCaches)"])
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    need("docs/development.md", dev, ["RX_VK_PCACHE_PLAY_FILE=0", "RX_VK_PCACHE_FOLD_MB", "mkw_vk_pipelines_play.bin"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    check_source(draw)
    code = block(draw, "bool pcache_play_save(int64_t main_built) {") + "\n" + block(draw, "void pcache_main_written(int64_t main_built) {")
    with tempfile.TemporaryDirectory(prefix="pcache-play-") as tmp:
        tmp = Path(tmp)
        for name, text in (("rules", RULES), ("save", SAVE.replace("@CODE@", code))):
            src, exe = tmp / (name + ".cpp"), tmp / name
            src.write_text(text, encoding="utf-8")
            subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-D_CRT_SECURE_NO_WARNINGS", "-I",
                            str(ROOT / "gpu/vk"), str(src), "-o", str(exe)], check=True)
        subprocess.run([str(tmp / "rules")], check=True, timeout=60)
        data = tmp / "data"
        data.mkdir()
        subprocess.run([str(tmp / "save"), str(data)], check=True, timeout=60)


if __name__ == "__main__":
    main()
