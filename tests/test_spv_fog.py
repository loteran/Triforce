"""The volumetric fog's froxels behind last frame's depth (gpu/vk/spv_fog.h, after NX2navi's 0379b00; no game, no GPU).

- spv_fog::find and keep_froxels, compiled and run on authored SPIR-V (tests/test_spv_fog.cpp): the fog's first pass found by
  its shape, as NX2navi's translator wrote it and as Triforce's may (its exit on either side of the branch, through a block that
  only branches on, its colours bitcast or not, and Triforce's: the comparison carried to the branch by a bool OpPhi whose
  other value, from the block that went round it, is known false on its edge from that block's branch, through an
  OpLogicalOr or an OpLogicalAnd), and patched in one word (distance > distance); near misses left alone (another stage, other
  colours, four or six writes, a less-than, a comparison that does not decide the branch, two comparisons, no selection merge,
  two such branches, two of which one has its comparison read elsewhere too, a phi whose other value is unknown or true, a
  phi on a back edge, a phi or its comparison read elsewhere, a module cut short);
  RX_VK_FOG_CULL;
- vk_shader.cpp: make_module patches the module it hands the driver, spv_fog.h is included in a part marked as outside the
  translation stamp, and the stamp's text (nx2/build.py translation_text) holds nothing of it, so the program cache is carried
  over this change as it was and only the fog's compute pipeline is new to the driver; the switch is documented;
- vk_draw.cpp, the light precompile (RX_VK_PRECOMPILE_KEEP off): its record's key for the compute programs has kFogPatch mixed
  in while the patch is on (times the matcher's version, spv_fog::kMatcher: c209c48's key, written while the matcher patched
  none of Triforce's fog modules, is not it), read as make_module reads RX_VK_FOG_CULL, so a record written without it, or by
  an earlier matcher, builds the listed compute
  programs once more before the game rather than leaving the fog's to its first dispatch; with RX_VK_FOG_CULL=1 the key is
  kBuiltComputes as it was (compiled and run); the record's stamp, the stamps' words, the predicted record and the notes' keys
  hold nothing of it. Its compute pipelines are made through cached_compute and the caches saved before built_write: its line
  and cached_create, compiled and run with a stand-in driver, put the fog's patched pipeline (in no cache file yet) in the
  play cache, which that save writes and the next start's dispatch finds (or, without a play cache, count it for the main
  file); made straight in the main cache, it was on no file and compiled in play all the same.

    python -I -B tests/test_spv_fog.py [--cxx g++]
"""
import argparse
import ast
import hashlib
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
OUT_MARK = "// ---- (Triforce) Outside the translation stamp"
IN_MARK = "// ---- (Triforce) Inside the translation stamp again"


def translation_text(rel, data):
    """nx2/build.py's translation_text, run here"""
    build = ROOT / "nx2" / "build.py"
    tree = ast.parse(build.read_text(encoding="utf-8"), filename=str(build))
    functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ("strip_comments", "translation_text")]
    ns = {"os": os, "hashlib": hashlib}
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(build), "exec"), ns)
    return ns["translation_text"](rel, data)


def check_source():
    shader_bytes = (ROOT / "gpu/vk/vk_shader.cpp").read_bytes()
    shader = shader_bytes.decode("utf-8").replace("\r\n", "\n")
    # (the include, in a marked part)
    inside, included = False, False
    for line in shader.split("\n"):
        s = line.strip()
        if s.startswith(OUT_MARK):
            inside = True
        elif s.startswith(IN_MARK):
            inside = False
        elif s == '#include "spv_fog.h"':
            if not inside:
                raise SystemExit("vk_shader.cpp includes spv_fog.h in the translation stamp: every program would be translated again")
            included = True
    if not included:
        raise SystemExit("vk_shader.cpp no longer includes spv_fog.h")
    stamped = translation_text("vk_shader.cpp", shader_bytes).decode("utf-8")
    if "spv_fog" in stamped or "RX_VK_FOG_CULL" in stamped:
        raise SystemExit("the fog's patch is in the translation stamp's text: every program would be translated again")
    m = re.search(r"\nVkShaderModule make_module\(const std::vector<uint32_t>& spirv,.*?\n}\n", shader, re.S)
    if not m:
        raise SystemExit("vk_shader.cpp: make_module not found")
    body = m.group(0)
    for needle in ('spv_fog::cull_kept(getenv("RX_VK_FOG_CULL"))', "spv_fog::keep_froxels(base.data(), base.size(), &found)",
                   # ((Triforce) on v0.5.2's water rewrite when it made one: spirv_water_simplify)
                   "const std::vector<uint32_t>& base = use_simple ? simplified : spirv;",
                   "const std::vector<uint32_t>& code = other ? *other : !nan_sel.empty() ? nan_sel : !fog.empty() ? fog : base;",
                   "std::vector<uint32_t> salted = !nan_sel.empty() ? nan_sel : !fog.empty() ? fog : base;", "vk: fog: module %08X"):
        if needle not in body:
            raise SystemExit("make_module no longer has: " + needle)
    # (the module made from what was patched; the record's SPIR-V, written from the translation, is not)
    if body.find("spv_fog::") > body.find("vkCreateShaderModule"):
        raise SystemExit("make_module patches after the module is made")
    docs = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "`RX_VK_FOG_CULL=1`" not in docs:
        raise SystemExit("docs/development.md does not describe RX_VK_FOG_CULL")
    row = next((line for line in docs.split("\n") if line.startswith("| `RX_VK_FOG_CULL=1`")), "")
    if "kFogPatch" not in row or "RX_VK_PRECOMPILE_KEEP" not in row:
        raise SystemExit("docs/development.md: the RX_VK_FOG_CULL row does not say what the light precompile builds once more")


def block(text, marker):
    start = text.index(marker)
    at = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[at] == "{") - (text[at] == "}")
        at += 1
    return text[start:at]


def code_only(text):
    """text without its // comments (none of the lines read here has a // in a string)"""
    return "\n".join(line.split("//")[0] for line in text.split("\n"))


def check_light_record():
    """The light precompile's record: the fog's patched compute module is built once more before the game. Returns the
    source of the key, for run_light_key"""
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
    if '#include "spv_fog.h"' not in draw:
        raise SystemExit("vk_draw.cpp does not include spv_fog.h: it cannot tell whether make_module patches the fog")
    key = block(draw, "uint64_t built_computes_key() {")
    for needle in ('static const bool kept = spv_fog::cull_kept(getenv("RX_VK_FOG_CULL"));',
                   "return kept ? kBuiltComputes : kBuiltComputes ^ (kFogPatch * spv_fog::kMatcher);"):
        if needle not in key:
            raise SystemExit("vk_draw.cpp built_computes_key no longer has: " + needle)
    code = code_only(block(draw, "void precompile_run(void (*progress)(size_t done, size_t total)) {"))
    for needle in ("const uint64_t built_stamp = warmed_stamp(gpl_wanted());",
                   "const uint64_t computes_key = built_computes_key();",
                   "computes_built = built.count(computes_key) != 0;", "built.insert(computes_key);",
                   "built_write(built_stamp, built);", "built = built_read(built_stamp);"):
        if needle not in code:
            raise SystemExit("vk_draw.cpp precompile_run no longer has: " + needle)
    if "kBuiltComputes" in code or "kFogPatch" in code:
        raise SystemExit("vk_draw.cpp precompile_run reads or writes the computes' key otherwise than by built_computes_key")
    if not code.index("const uint64_t computes_key") < code.index("built = built_read(built_stamp);"):
        raise SystemExit("vk_draw.cpp precompile_run: the computes' key is taken after the record is read")
    # (the compute programs made as the dispatch makes them, through the play cache: one compiled into the main cache straight
    # was counted nowhere, the save before the record wrote the play file alone, and the next start compiled it in play)
    if CREATE not in code or "guarded_compute(" in code:
        raise SystemExit("vk_draw.cpp precompile_run: its compute pipelines are not made through cached_compute")
    if not code.index("pipeline_cache_save(true);") < code.index("built_write(built_stamp, built);"):
        raise SystemExit("vk_draw.cpp precompile_run: the record says built before the caches are saved")
    # (where nothing changed nothing is built again: the fog is in no stamp, no other record and no pipeline's key)
    plain = code_only(draw)
    if plain.count("kFogPatch") != 2 or plain.count("built_computes_key()") != 2 or plain.count("spv_fog::") != 2:
        raise SystemExit("vk_draw.cpp: kFogPatch, built_computes_key or spv_fog is used outside the light precompile's computes key")
    for marker in ("uint64_t warmed_stamp_of(", "uint64_t warmed_stamp(", "uint64_t libraries_stamp(", "uint32_t module_patches(",
                   "void predict_run(", "uint64_t pipeline_key(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo) {"):
        part = code_only(block(draw, marker))
        if "fog" in part.lower() or "computes_key" in part:
            raise SystemExit("vk_draw.cpp %s reads the fog's patch: other records and stamps would change" % marker.split("(")[0])
    start = draw.index("constexpr uint64_t kBuiltComputes = ")
    key_source = draw[start:draw.index("\n}\n", draw.index("uint64_t built_computes_key() {")) + 3]
    line = next(s.strip() for s in code.split("\n") if CREATE in s)
    create_source = ("template <class Info, class Guarded>\n"
                     + block(draw, "VkResult cached_create(VkPipelineCache cache, const Info* ci, VkPipeline* pl, Guarded guarded) {")
                     + "\n" + block(draw, "VkResult cached_compute(VkPipelineCache cache, const VkComputePipelineCreateInfo* ci, VkPipeline* pl) {")
                     + "\nvoid precompile_compute(VkComputePipelineCreateInfo ci, VkPipeline& made) {\n    " + line + "\n}\n")
    return key_source, create_source


CREATE = "cached_compute(pcache_use().cache, &ci, &made)"

SAVE_HEAD = r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <set>
#include "pcache_play.h"
using VkResult = int;
using VkPipeline = uintptr_t;
using VkPipelineCache = uintptr_t;
constexpr VkResult VK_SUCCESS = 0, VK_PIPELINE_COMPILE_REQUIRED = 1000297000;
constexpr uint32_t VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT = 0x100;
constexpr VkPipeline VK_NULL_HANDLE = 0;
struct VkComputePipelineCreateInfo { uint32_t flags; int key; };
VkPipelineCache pcache = 10, pcache_play_cache = 0;
std::atomic<int> pcache_preparing{0};
std::atomic<int64_t> pcache_main_built{0}, st_play_made{0};
std::set<int> main_has, play_has;
int compiles = 0;
// (a stand-in driver: a cache answers what it holds, compiles the rest into itself, or under the flag says it must compile)
VkResult guarded_compute(VkPipelineCache cache, const VkComputePipelineCreateInfo* ci, VkPipeline* pl) {
    std::set<int>& has = cache == pcache ? main_has : play_has;
    if (has.count(ci->key)) return *pl = cache * 100 + ci->key, VK_SUCCESS;
    if (ci->flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) return VK_PIPELINE_COMPILE_REQUIRED;
    compiles++, has.insert(ci->key);
    return *pl = cache * 100 + ci->key, VK_SUCCESS;
}
struct UseFx { VkPipelineCache cache; };
UseFx pcache_use() { return {pcache}; }
'''

SAVE_MAIN = r'''
static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}
static bool saved(bool play_cache) {   // (what pipeline_cache_save(true) writes, from the counts: pcache_play_save, save_what)
    const pcache_play::SaveWhat w = pcache_play::save_what(play_cache, play_has.size(), 0, pcache_main_built.load());
    return w.play || w.main;
}
int main() {
    {   // a play cache (pipelineCreationCacheControl): the main file has every compute program's but the fog's patched one (2)
        pcache_play_cache = 20, main_has = {1}, play_has = {}, compiles = 0, pcache_main_built = 0, st_play_made = 0;
        VkPipeline made = VK_NULL_HANDLE;
        precompile_compute({0, 1}, made);
        expect(made == 1001 && compiles == 0 && st_play_made == 0, "one the main cache has: answered by it, nothing compiled");
        precompile_compute({0, 2}, made);
        expect(made == 2002 && compiles == 1 && play_has.count(2) && st_play_made == 1,
               "the fog's: compiled into the play cache, which the save before the record writes");
        expect(saved(true), "the save before the record writes a file that holds the fog's pipeline");
        // (the next start: the files read back, the record says built; the dispatch asks the caches only and finds it)
        VkComputePipelineCreateInfo asked{VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT, 2};
        made = VK_NULL_HANDLE;
        expect(cached_compute(pcache, &asked, &made) == VK_SUCCESS && made == 2002 && compiles == 1,
               "the next start's first dispatch finds the fog's pipeline without a compile");
    }
    {   // no play cache: compiled into the main cache, counted for its file
        pcache_play_cache = 0, main_has = {1}, play_has = {}, compiles = 0, pcache_main_built = 0, st_play_made = 0;
        VkPipeline made = VK_NULL_HANDLE;
        precompile_compute({0, 2}, made);
        expect(made == 1002 && main_has.count(2) && pcache_main_built == 1, "no play cache: in the main cache, counted for its save");
        expect(saved(false), "no play cache: the main file written");
    }
    if (failures) return 1;
    std::printf("spv_fog: the light precompile's compute pipelines reach a saved cache: ok\n");
    return 0;
}
'''


def run_light_save(cxx, tmp, source):
    """the precompile's compute creation, as it is in the source, with a stand-in driver: the fog's patched pipeline, which no
    cache file has, is in one that the save before built_write writes (with a play cache or without)"""
    src = Path(tmp) / "fog_save.cpp"
    src.write_text(SAVE_HEAD + source + SAVE_MAIN, encoding="utf-8")
    exe = Path(tmp) / "fog-save"
    subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), str(src), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)


KEY_MAIN = r'''
int main() {
    std::printf("%016llx %016llx\n", (unsigned long long)built_computes_key(), (unsigned long long)kBuiltComputes);
    return 0;
}
'''


def run_light_key(cxx, tmp, source):
    """the key compiled as it is in the source: RX_VK_FOG_CULL=1 gives kBuiltComputes bit for bit, unset or 0 another key"""
    src = Path(tmp) / "fog_key.cpp"
    src.write_text("#include <cstdint>\n#include <cstdio>\n#include <cstdlib>\n#include \"spv_fog.h\"\n" + source + KEY_MAIN,
                   encoding="utf-8")
    exe = Path(tmp) / "fog-key"
    subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), str(src), "-o", str(exe)],
                   check=True)
    got = {}
    for value in (None, "0", "1"):
        env = dict(os.environ)
        env.pop("RX_VK_FOG_CULL", None)
        if value is not None:
            env["RX_VK_FOG_CULL"] = value
        got[value] = subprocess.run([str(exe)], check=True, env=env, capture_output=True, text=True).stdout.split()
    kept, patched, zero = got["1"], got[None], got["0"]
    if kept[0] != kept[1] or kept[1] != "c04d50555445d001":
        raise SystemExit("built_computes_key with RX_VK_FOG_CULL=1 is not kBuiltComputes as it was: " + " ".join(kept))
    if patched[0] == patched[1] or zero[0] != patched[0]:
        raise SystemExit("built_computes_key with the fog's patch on is kBuiltComputes, or unset and 0 differ: "
                         + " ".join(patched + zero))
    # (the matcher's version in it: the key c209c48's matcher wrote, which patched none of Triforce's fog modules, builds the
    # computes once more; each version its own key)
    fog = (ROOT / "gpu/vk/spv_fog.h").read_text(encoding="utf-8")
    m = re.search(r"\nconstexpr uint64_t kMatcher = (\d+);", fog)
    if not m or int(m.group(1)) < 2:
        raise SystemExit("spv_fog.h: kMatcher missing, or below 2 (the version after the matcher that patched nothing)")
    want = 0xC04D50555445D001 ^ ((0xF06F205E0F7A1101 * int(m.group(1))) % (1 << 64))
    if patched[0] != "%016x" % want or patched[0] == "3022700b5b3fc100":
        raise SystemExit("built_computes_key with the fog's patch on is not kBuiltComputes ^ (kFogPatch * kMatcher), or is "
                         "the key the matcher that patched nothing wrote: " + patched[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    key_source, create_source = check_light_record()
    with tempfile.TemporaryDirectory(prefix="spv-fog-") as tmp:
        run_light_key(args.cxx, tmp, key_source)
        run_light_save(args.cxx, tmp, create_source)
        exe = Path(tmp) / "spv-fog"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_spv_fog.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
