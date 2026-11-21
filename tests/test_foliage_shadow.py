"""The foliage in the sun's far shadow cascades made cheaper (gpu/vk/foliage_shadow.h; vk_draw.cpp draw_cull, pipeline_rate,
draw_rate; after NX2navi's 754ded9; no game, no GPU).

The game draws its foliage cards double-sided with mesh programs, into the shadow cascades too, where the stacked cards seen
from the sun cover the same texels many times over. RX_VK_FOLIAGE_SHADOW_CULL (on from layer 1 unless off): a mesh program's
draw into a cascade from that layer on that culls neither face is drawn with its back (or front) faces culled.
RX_VK_FOLIAGE_SHADOW_VRS (on from layer 2 unless off, where the device has pipelineFragmentShadingRate): the same draws of
native mesh programs from that layer on shade one fragment per 2 x 2 pixels, the rate dynamic state in those pipelines alone.

Compiles and runs the header's rules (the switches' values and defaults; which draws they take: mesh programs only, no colour
target, a depth target square and wider than 768, the layer, the game's own cull mode kept where it has one; which pipelines
have the rate), then draw_cull, pipeline_rate and draw_rate cut out of vk_draw.cpp against stand-ins, in a process each for the
switches' values (native mesh programs and the compute fallback's, the log, when the rate is set again). Reads the source for
what needs the game: that the face goes through the packed dynamic state, that the cull mode is dynamic state (no other
pipeline), that the rate is dynamic state in pipeline_rate's pipelines alone (whole or their two shader libraries, those keyed
apart) and set right after their bind, that the device asks for the extension only where it can be used, and that the switches
are in the development guide.

    python -I -B tests/test_foliage_shadow.py [--cxx g++]
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
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_source(draw, main):
    if '#include "foliage_shadow.h"' not in draw:
        raise SystemExit("vk_draw.cpp does not include foliage_shadow.h")
    if "constexpr int kDynCull = 23;" not in draw:
        raise SystemExit("vk_draw.cpp: no kDynCull, the packed word of the face")
    pack = block(draw, "int dyn_pack(")
    if "up[kDynCull] = draw_cull(s, at)" not in pack or re.search(r"up\[23\]", pack):
        raise SystemExit("vk_draw.cpp dyn_pack: the face packed is not draw_cull's (a draw into another cascade layer would keep the last one's)")
    if "s.poly.cull" in pack:
        raise SystemExit("vk_draw.cpp dyn_pack: the game's face is packed beside draw_cull's")
    state = block(draw, "void dynamic_state(VkCommandBuffer cmd")
    if "const uint32_t cull = du[kDynCull];" not in state or "s.poly.cull" in state:
        raise SystemExit("vk_draw.cpp dynamic_state: the cull mode is not set from draw_cull's face")
    if not state.index("dyn_pack(s, at, topo, mesh, df, du, db)") < state.index("const uint32_t cull = du[kDynCull];"):
        raise SystemExit("vk_draw.cpp dynamic_state: the face is read before it is packed")
    for needle in ("cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT", "cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT",
                   "if (DYN_CHANGED(cull)) vkCmdSetCullMode(cmd, d.cull);"):
        if needle not in state:
            raise SystemExit("vk_draw.cpp dynamic_state lacks %r" % needle)
    pipeline = draw[draw.index("std::vector<VkDynamicState> dyn = {"):]
    if "VK_DYNAMIC_STATE_CULL_MODE" not in pipeline[:pipeline.index("};")]:
        raise SystemExit("vk_draw.cpp: the cull mode is no longer dynamic state: the rule would need other pipelines")
    if draw.index("uint32_t draw_cull(") > draw.index("int dyn_pack("):
        raise SystemExit("vk_draw.cpp: draw_cull is not before dyn_pack")
    # (the shading rate: dynamic state in pipeline_rate's pipelines alone, whole or their two shader libraries)
    rate_line = ("if (pipeline_rate(p, at) && (part < 0 || part == 1 || part == 2)) "
                 "dyn.push_back(VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR);")
    build = block(draw, "VkPipeline build_graphics(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, int part, bool* cold)")
    if rate_line not in build:
        raise SystemExit("vk_draw.cpp build_graphics: the shading rate is not dynamic state in pipeline_rate's pipelines alone")
    if draw.count("VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR") != 1:
        raise SystemExit("vk_draw.cpp: the shading rate's dynamic state is named outside build_graphics's one line (every pipeline's key?)")
    if build.index(rate_line) > build.index("dy.dynamicStateCount = (uint32_t)dyn.size();"):
        raise SystemExit("vk_draw.cpp build_graphics: the shading rate is pushed after the dynamic state's count was taken")
    keys = block(draw, "void gpl_keys(")
    if "pipeline_rate(p, at) ? mix(program, kRateLibrary) : program" not in keys or \
            "keys[1] = shaders, keys[2] = mix(shaders, (uint64_t)at.samples)" not in keys:
        raise SystemExit("vk_draw.cpp gpl_keys: the shader libraries of pipelines with the rate are not keyed apart (or the others changed)")
    # (the light precompile: a depth-only note is built once more for the rate (built_key), but once its program is read one
    # whose program has no rate is left as the earlier start built it, before the workers take the notes)
    pre = draw[draw.index("const uint64_t built_stamp = warmed_stamp(gpl_wanted());"):]
    drop = "return p && !pipeline_rate(*p, note_targets(*n));"
    if "if (light && dev.foliage_rate) {" not in pre or drop not in pre or \
            not pre.index("program_cache_load_some(wanted,") < pre.index(drop) < pre.index("pre->notes = std::move(notes)"):
        raise SystemExit("vk_draw.cpp precompile_run: the depth-only notes of programs without the rate are built again (or "
                         "dropped before their programs are read)")
    if "if (built_key(*n) == n->key || !built.count(n->key)) return false;" not in pre:
        raise SystemExit("vk_draw.cpp precompile_run: a note not built before, or with no rate key, is dropped by the program test")
    if "rate" in block(draw, "uint64_t pipeline_key(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo) {"):
        raise SystemExit("vk_draw.cpp pipeline_key: the rate changed the pipelines' keys (the notes and the precompile would not find them)")
    bind = draw.index("vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl), last_pipeline = pl;")
    call = draw.index("if (pipeline_rate(p, at)) draw_rate(cmd, s, at, bound);")
    if not bind < call < draw.index("if (!bind_descriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p)) SKIP_HERE();", bind):
        raise SystemExit("vk_draw.cpp draw: the rate is not set right after the pipeline's bind (a draw left out between would leave it unset)")
    if "const bool bound = pl != last_pipeline;" not in draw[bind - 400:bind]:
        raise SystemExit("vk_draw.cpp draw: draw_rate is not told whether the pipeline was bound for this draw")
    # (the device: the extension only where it is of use, the rate per draw only)
    for needle in ("if (dev.mesh_shader && rate_from >= 0) optional(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, rate_ext);",
                   "rate.attachmentFragmentShadingRate = rate.primitiveFragmentShadingRate = VK_FALSE;",
                   "dev.foliage_rate = rate_ext && rate.pipelineFragmentShadingRate && dev.mesh_shader;",
                   "*rate_at = rate.pNext;"):
        if needle not in main:
            raise SystemExit("vk_main.cpp create_device lacks %r" % needle)
    if not main.index("dev.mesh_shader = dev.mesh_shader && mesh.meshShader && mesh.taskShader;") < \
            main.index("dev.foliage_rate = rate_ext") < main.index("VkResult made = vkCreateDevice("):
        raise SystemExit("vk_main.cpp create_device: dev.foliage_rate is not decided between the mesh shaders' and the device's creation")
    rec = (ROOT / "gpu/vk/vk_recorder.h").read_text(encoding="utf-8")
    if "A(vkCmdSetFragmentShadingRateKHR)" not in rec or "pk_vkCmdSetFragmentShadingRateKHR" not in rec:
        raise SystemExit("vk_recorder.h does not carry vkCmdSetFragmentShadingRateKHR (RX_VK_RECORDER would make it out of order)")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    for switch in ("`RX_VK_FOLIAGE_SHADOW_CULL=<layer>[f]`", "`RX_VK_FOLIAGE_SHADOW_VRS=<layer>`"):
        if switch not in dev:
            raise SystemExit("docs/development.md: %s is not in the switches table" % switch)


HEADER_TEST = r'''
#include <cstdio>
#include <cstdlib>
#include "foliage_shadow.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::foliage_shadow;
int main() {
    // (the cull: on from layer 1 unless off, NX2navi's)
    CHECK(parse(nullptr).from == 1 && !parse(nullptr).front);
    CHECK(parse("").from == 1);
    CHECK(parse("off").from == -1);
    CHECK(parse("-1").from == -1);
    CHECK(parse("f").from == -1);
    CHECK(parse("1").from == 1 && !parse("1").front);
    CHECK(parse("1f").from == 1 && parse("1f").front);
    CHECK(parse("0").from == 0 && !parse("0").front);
    CHECK(parse("2f").from == 2 && parse("2f").front);
    CHECK(parse("3").from == 3);
    const Rule off = parse("off"), one = parse(nullptr), one_front = parse("1f"), zero = parse("0"), three = parse("3");
    // (off: the game's face, whatever the draw)
    for (int game = 0; game < 4; game++) CHECK(cull(off, game, true, 0, true, 1600, 1600, 2) == game);
    // (the far cascades of the sun's three, 1600 x 1600: from layer 1 on)
    CHECK(cull(one, kNone, true, 0, true, 1600, 1600, 1) == kBack);
    CHECK(cull(one, kNone, true, 0, true, 1600, 1600, 2) == kBack);
    CHECK(cull(one, kNone, true, 0, true, 1600, 1600, 0) == kNone);       // (the near cascade: as the game draws it)
    CHECK(cull(one_front, kNone, true, 0, true, 1600, 1600, 2) == kFront);
    CHECK(cull(zero, kNone, true, 0, true, 1600, 1600, 0) == kBack);
    CHECK(cull(three, kNone, true, 0, true, 1600, 1600, 2) == kNone);     // (past the game's layers: none)
    CHECK(cull(one, kNone, true, 0, true, 2048, 2048, 1) == kBack);
    CHECK(cull(one, kNone, true, 0, true, 769, 769, 1) == kBack);
    // (what culls a face already keeps it)
    CHECK(cull(one, kFront, true, 0, true, 1600, 1600, 2) == kFront);
    CHECK(cull(one_front, kBack, true, 0, true, 1600, 1600, 2) == kBack);
    CHECK(cull(one, 3, true, 0, true, 1600, 1600, 2) == 3);
    // (not a mesh program's draw, a colour target, no depth target: none)
    CHECK(cull(one, kNone, false, 0, true, 1600, 1600, 2) == kNone);
    CHECK(cull(one, kNone, true, 1, true, 1600, 1600, 2) == kNone);
    CHECK(cull(one, kNone, true, 0, false, 1600, 1600, 2) == kNone);
    // (not a cascade: the characters' own shadow maps, 768 and less, and targets not square)
    CHECK(cull(one, kNone, true, 0, true, 768, 768, 1) == kNone);
    CHECK(cull(one, kNone, true, 0, true, 260, 260, 5) == kNone);
    CHECK(cull(one, kNone, true, 0, true, 1600, 900, 1) == kNone);
    CHECK(cull(one, kNone, true, 0, true, 1600, 1601, 1) == kNone);
    // (the rate: on from layer 2 unless off, NX2navi's)
    CHECK(parse_rate(nullptr) == 2 && parse_rate("") == 2);
    CHECK(parse_rate("off") == -1 && parse_rate("-1") == -1 && parse_rate("x") == -1);
    CHECK(parse_rate("1") == 1 && parse_rate("0") == 0 && parse_rate("3") == 3);
    const int r2 = parse_rate(nullptr), r_off = parse_rate("off"), r3 = parse_rate("3");
    CHECK(rate(r2, kNone, true, 0, true, 1600, 1600, 2) == 2);       // (the far cascade's foliage: 2 x 2)
    CHECK(rate(r2, kNone, true, 0, true, 1600, 1600, 1) == 1);       // (the middle and near ones: as the game draws them)
    CHECK(rate(r2, kNone, true, 0, true, 1600, 1600, 0) == 1);
    CHECK(rate(parse_rate("1"), kNone, true, 0, true, 1600, 1600, 1) == 2);
    CHECK(rate(r_off, kNone, true, 0, true, 1600, 1600, 2) == 1);
    CHECK(rate(r3, kNone, true, 0, true, 1600, 1600, 2) == 1);
    CHECK(rate(r2, kBack, true, 0, true, 1600, 1600, 2) == 1);       // (single-sided as the game draws it: not the cards)
    CHECK(rate(r2, kFront, true, 0, true, 1600, 1600, 2) == 1);
    CHECK(rate(r2, kNone, false, 0, true, 1600, 1600, 2) == 1);
    CHECK(rate(r2, kNone, true, 1, true, 1600, 1600, 2) == 1);
    CHECK(rate(r2, kNone, true, 0, false, 1600, 1600, 2) == 1);
    CHECK(rate(r2, kNone, true, 0, true, 768, 768, 2) == 1);         // (a character's own shadow map)
    CHECK(rate(r2, kNone, true, 0, true, 1600, 900, 2) == 1);
    // (the pipelines with the rate dynamic: native mesh programs, no colour target, a depth one, the rate on the device)
    CHECK(pipeline_rate(true, true, 0, true));
    CHECK(!pipeline_rate(false, true, 0, true));
    CHECK(!pipeline_rate(true, false, 0, true));
    CHECK(!pipeline_rate(true, true, 1, true));
    CHECK(!pipeline_rate(true, true, 0, false));
    std::printf("PASS: the rules: the switches' values, the draws they take and the ones they leave, the pipelines with the rate\n");
}
'''

FUNCTION_TEST = r'''
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "foliage_shadow.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
enum { NVN_FACE_NONE = 0, NVN_FACE_FRONT = 1, NVN_FACE_BACK = 2, NVN_FACE_FRONT_AND_BACK = 3 };
typedef struct Cmd_T* VkCommandBuffer;
#define VK_NULL_HANDLE nullptr
enum VkFormat { VK_FORMAT_UNDEFINED = 0, VK_FORMAT_D32_SFLOAT = 126, VK_FORMAT_S8_UINT = 127 };
struct VkExtent2D { uint32_t width, height; };
enum VkFragmentShadingRateCombinerOpKHR { VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR = 0, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR = 3 };
struct RateCall { VkCommandBuffer cmd; uint32_t w, h; int op0, op1; };
std::vector<RateCall> rate_calls;
void vkCmdSetFragmentShadingRateKHR(VkCommandBuffer cmd, const VkExtent2D* e, const VkFragmentShadingRateCombinerOpKHR ops[2]) {
    rate_calls.push_back({cmd, e->width, e->height, (int)ops[0], (int)ops[1]});
}
namespace mkwvk {
std::string logged;
void logf(const char* fmt, ...) {
    char line[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    logged += line;
    logged += "\n";
}
struct NvnState { struct { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; } poly{}; };
struct Texture { int id; };
struct Targets { int n = 0; Texture* depth = nullptr; uint32_t width = 0, height = 0, depth_layer = 0; VkFormat depth_format = VK_FORMAT_UNDEFINED; };
struct Program { bool mesh = false, fallback = false; uint64_t key = 0; };
struct Device { bool foliage_rate = false; } dev;
bool depth_aspect(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT; }
thread_local const Program* cur_program = nullptr;
thread_local VkCommandBuffer rate_cmd = VK_NULL_HANDLE;
thread_local uint32_t rate_size = 0;
/* FUNCTIONS */
} // namespace mkwvk
using namespace mkwvk;
int main(int argc, char** argv) {
    CHECK(argc == 4);
    const int want = std::atoi(argv[1]);        // (the face the far cascades' foliage gets: 0 the game's none, 1 front, 2 back)
    const int near_face = std::atoi(argv[2]);   // (and the near cascade's, layer 0)
    const int rate_from = std::atoi(argv[3]);   // (the first layer at 2 x 2; -1 none)
    Texture cascades{1};
    Program native{true, false, 0x1111}, fallback{false, true, 0x2222}, vertex{false, false, 0x3333};
    NvnState s;
    s.poly.cull = NVN_FACE_NONE;
    Targets at;
    at.depth = &cascades, at.width = at.height = 1600, at.depth_layer = 2, at.depth_format = VK_FORMAT_D32_SFLOAT;
    cur_program = &native;
    CHECK((int)draw_cull(s, at) == want);
    cur_program = &fallback;   // (the compute fallback's draw of a mesh program: the same)
    CHECK((int)draw_cull(s, at) == want);
    cur_program = &vertex;     // (a vertex program's draw: the game's)
    CHECK(draw_cull(s, at) == NVN_FACE_NONE);
    cur_program = nullptr;
    CHECK(draw_cull(s, at) == NVN_FACE_NONE);
    cur_program = &native;
    at.depth_layer = 0;        // (the near cascade: as the game draws it, but at RX_VK_FOLIAGE_SHADOW_CULL=0)
    CHECK((int)draw_cull(s, at) == near_face);
    at.depth_layer = 1;
    CHECK((int)draw_cull(s, at) == want);
    s.poly.cull = NVN_FACE_FRONT;   // (the game's own face kept)
    CHECK(draw_cull(s, at) == NVN_FACE_FRONT);
    s.poly.cull = NVN_FACE_NONE;
    at.n = 1;                  // (a colour target: no shadow map)
    CHECK(draw_cull(s, at) == NVN_FACE_NONE);
    at.n = 0, at.width = at.height = 768;   // (a character's own shadow map)
    CHECK(draw_cull(s, at) == NVN_FACE_NONE);
    const bool on = want != 0;
    CHECK((logged.find("faces culled (=off: as the game draws them)") != std::string::npos) == on);
    CHECK((logged.find("cull as the game asks") != std::string::npos) == !on);
    CHECK((logged.find("vk: foliage shadow cull: program 0000000000001111") != std::string::npos) == on);
    CHECK((logged.find("vk: foliage shadow cull: program 0000000000002222") != std::string::npos) == on);
    CHECK(logged.find("0000000000003333") == std::string::npos);

    // the pipelines with the rate dynamic: the device's flag, native mesh programs, no colour target, a depth format
    Targets cascade;
    cascade.depth = &cascades, cascade.width = cascade.height = 1600, cascade.depth_format = VK_FORMAT_D32_SFLOAT;
    CHECK(!pipeline_rate(native, cascade));   // (no device flag: none, whatever else)
    dev.foliage_rate = true;
    CHECK(pipeline_rate(native, cascade));
    CHECK(!pipeline_rate(fallback, cascade) && !pipeline_rate(vertex, cascade));
    Targets colour = cascade;
    colour.n = 1;
    CHECK(!pipeline_rate(native, colour));
    Targets stencil = cascade;
    stencil.depth_format = VK_FORMAT_S8_UINT;
    CHECK(!pipeline_rate(native, stencil));
    Targets none = cascade;
    none.depth_format = VK_FORMAT_UNDEFINED;   // (by the depth format, the pipeline's key, not by the depth texture)
    CHECK(!pipeline_rate(native, none));

    // the draws through them: 2 x 2 into the far cascades from the switch's layer, 1 x 1 else; set at each bind, once a size
    const uint32_t far_size = rate_from >= 0 && rate_from <= 2 ? 2u : 1u;
    VkCommandBuffer a = (VkCommandBuffer)(uintptr_t)0x10, b = (VkCommandBuffer)(uintptr_t)0x20;
    cur_program = &native;
    cascade.depth_layer = 2;
    draw_rate(a, s, cascade, true);
    CHECK(rate_calls.size() == 1 && rate_calls[0].cmd == a && rate_calls[0].w == far_size && rate_calls[0].h == far_size);
    CHECK(rate_calls[0].op0 == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR && rate_calls[0].op1 == VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR);
    draw_rate(a, s, cascade, false);   // (the same pipeline, the same size: the command buffer has it)
    CHECK(rate_calls.size() == 1);
    draw_rate(a, s, cascade, true);    // (bound again: another pipeline was bound between, the state undefined)
    CHECK(rate_calls.size() == 2);
    draw_rate(b, s, cascade, false);   // (another command buffer)
    CHECK(rate_calls.size() == 3 && rate_calls[2].cmd == b);
    cascade.depth_layer = 0;           // (the near cascade: 1 x 1, set when it differs)
    draw_rate(b, s, cascade, false);
    CHECK(rate_calls.size() == (far_size == 2 ? 4u : 3u) && rate_calls.back().w == 1 && rate_calls.back().h == 1);
    cascade.depth_layer = 2;
    s.poly.cull = NVN_FACE_BACK;       // (drawn single-sided by the game: not the cards, 1 x 1)
    draw_rate(b, s, cascade, true);
    CHECK(rate_calls.back().w == 1);
    s.poly.cull = NVN_FACE_NONE;
    Targets small = cascade;
    small.width = small.height = 768;  // (a character's own shadow map)
    draw_rate(b, s, small, true);
    CHECK(rate_calls.back().w == 1);
    CHECK((logged.find("vk: foliage shadow 2 x 2: program 0000000000001111's draw into the cascade layer 2 (1600x1600)") !=
           std::string::npos) == (far_size == 2));
    std::printf("PASS: RX_VK_FOLIAGE_SHADOW_CULL=%s: the far cascades' foliage culls %s; RX_VK_FOLIAGE_SHADOW_VRS=%s: %s\n",
                std::getenv("RX_VK_FOLIAGE_SHADOW_CULL") ? std::getenv("RX_VK_FOLIAGE_SHADOW_CULL") : "(unset)",
                want == 2 ? "its back faces" : want == 1 ? "its front faces" : "as the game asks",
                std::getenv("RX_VK_FOLIAGE_SHADOW_VRS") ? std::getenv("RX_VK_FOLIAGE_SHADOW_VRS") : "(unset)",
                far_size == 2 ? "the far cascade at 2 x 2" : "1 x 1 everywhere");
}
'''

# (cull switch, vrs switch) -> the face the far cascades' foliage gets, the near cascade's (NVN's: 0 none, 1 front, 2 back),
# and the first layer at 2 x 2 (-1 none)
RUNS = [(None, None, 2, 0, 2), ("1", "2", 2, 0, 2), ("1f", None, 1, 0, 2), ("0", "1", 2, 2, 1), ("off", "off", 0, 0, -1),
        ("", "", 2, 0, 2), (None, "3", 2, 0, 3)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    main_cpp = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    check_source(draw, main_cpp)
    functions = "\n".join(block(draw, m) for m in ("bool pipeline_rate(const Program& p, const Targets& at)", "uint32_t draw_cull(",
                                                    "void draw_rate("))
    vk = str(ROOT / "gpu" / "vk")
    with tempfile.TemporaryDirectory(prefix="foliage-shadow-") as tmp:
        tmp = Path(tmp)
        for name, text in (("header", HEADER_TEST), ("function", FUNCTION_TEST.replace("/* FUNCTIONS */", functions))):
            cpp, exe = tmp / (name + ".cpp"), tmp / (name + ".exe")
            cpp.write_text(text, encoding="utf-8")
            subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + vk, str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(tmp / "header.exe")], check=True)
        for cull, vrs, want, near, rate_from in RUNS:
            env = {k: v for k, v in os.environ.items() if k not in ("RX_VK_FOLIAGE_SHADOW_CULL", "RX_VK_FOLIAGE_SHADOW_VRS")}
            if cull is not None:
                env["RX_VK_FOLIAGE_SHADOW_CULL"] = cull
            if vrs is not None:
                env["RX_VK_FOLIAGE_SHADOW_VRS"] = vrs
            subprocess.run([str(tmp / "function.exe"), str(want), str(near), str(rate_from)], env=env, check=True, timeout=30)
    print("PASS: the foliage in the far shadow cascades")


if __name__ == "__main__":
    main()
