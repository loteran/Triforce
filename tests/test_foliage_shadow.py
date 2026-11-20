"""The foliage in the sun's far shadow cascades drawn with one face culled (gpu/vk/foliage_shadow.h, vk_draw.cpp draw_cull;
RX_VK_FOLIAGE_SHADOW_CULL, off unless set; no game, no GPU).

The game draws its foliage cards double-sided with mesh programs, into the shadow cascades too, where the stacked cards seen
from the sun cover the same texels many times over. With the switch a mesh program's draw into a cascade from the given layer
on that culls neither face is drawn with its back (or front) faces culled. Compiles and runs the header's rule (the switch's
values; which draws it takes: mesh programs only, no colour target, a depth target square and wider than 768, the layer, the
game's own cull mode kept where it has one), then draw_cull cut out of vk_draw.cpp against stand-ins, in a process each for
the switch's values (native mesh programs and the compute fallback's, the log). Reads the source for what needs the game:
that the face goes through the packed dynamic state, so a draw into another layer with the same state is not taken for the
same, that dynamic_state sets the cull mode from it, that the cull mode is dynamic state (no other pipeline), and that the
switch is in the development guide.

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


def check_source(draw):
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
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "`RX_VK_FOLIAGE_SHADOW_CULL=<layer>[f]`" not in dev:
        raise SystemExit("docs/development.md: RX_VK_FOLIAGE_SHADOW_CULL is not in the switches table")


HEADER_TEST = r'''
#include <cstdio>
#include <cstdlib>
#include "foliage_shadow.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::foliage_shadow;
int main() {
    CHECK(parse(nullptr).from == -1);
    CHECK(parse("").from == -1);
    CHECK(parse("off").from == -1);
    CHECK(parse("-1").from == -1);
    CHECK(parse("f").from == -1);
    CHECK(parse("1").from == 1 && !parse("1").front);
    CHECK(parse("1f").from == 1 && parse("1f").front);
    CHECK(parse("0").from == 0 && !parse("0").front);
    CHECK(parse("2f").from == 2 && parse("2f").front);
    CHECK(parse("3").from == 3);
    const Rule off = parse(nullptr), one = parse("1"), one_front = parse("1f"), zero = parse("0"), three = parse("3");
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
    std::printf("PASS: the rule: the switch's values, the draws it takes and the ones it leaves\n");
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
#include "foliage_shadow.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
enum { NVN_FACE_NONE = 0, NVN_FACE_FRONT = 1, NVN_FACE_BACK = 2, NVN_FACE_FRONT_AND_BACK = 3 };
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
struct Targets { int n = 0; Texture* depth = nullptr; uint32_t width = 0, height = 0, depth_layer = 0; };
struct Program { bool mesh = false, fallback = false; uint64_t key = 0; };
thread_local const Program* cur_program = nullptr;
/* FUNCTION */
} // namespace mkwvk
using namespace mkwvk;
int main(int argc, char** argv) {
    CHECK(argc == 3);
    const int want = std::atoi(argv[1]);   // (the face the far cascades' foliage gets: 0 the game's none, 1 front, 2 back)
    const int near_face = std::atoi(argv[2]);   // (and the near cascade's, layer 0)
    Texture cascades{1};
    Program native{true, false, 0x1111}, fallback{false, true, 0x2222}, vertex{false, false, 0x3333};
    NvnState s;
    s.poly.cull = NVN_FACE_NONE;
    Targets at;
    at.depth = &cascades, at.width = at.height = 1600, at.depth_layer = 2;
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
    CHECK((logged.find("vk: RX_VK_FOLIAGE_SHADOW_CULL=") != std::string::npos) == on);
    CHECK((logged.find("vk: foliage shadow cull: program 0000000000001111") != std::string::npos) == on);
    CHECK((logged.find("vk: foliage shadow cull: program 0000000000002222") != std::string::npos) == on);
    CHECK(logged.find("0000000000003333") == std::string::npos);
    std::printf("PASS: RX_VK_FOLIAGE_SHADOW_CULL=%s: the far cascades' foliage culls %s\n", std::getenv("RX_VK_FOLIAGE_SHADOW_CULL") ?
                std::getenv("RX_VK_FOLIAGE_SHADOW_CULL") : "(unset)", want == 2 ? "its back faces" : want == 1 ? "its front faces" : "as the game asks");
}
'''

# the switch -> the face the far cascades' foliage gets, and the near cascade's (NVN's: 0 none, 1 front, 2 back)
RUNS = [(None, 0, 0), ("1", 2, 0), ("1f", 1, 0), ("0", 2, 2), ("off", 0, 0)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    check_source(draw)
    function = block(draw, "uint32_t draw_cull(")
    vk = str(ROOT / "gpu" / "vk")
    with tempfile.TemporaryDirectory(prefix="foliage-shadow-") as tmp:
        tmp = Path(tmp)
        for name, text in (("header", HEADER_TEST), ("function", FUNCTION_TEST.replace("/* FUNCTION */", function))):
            cpp, exe = tmp / (name + ".cpp"), tmp / (name + ".exe")
            cpp.write_text(text, encoding="utf-8")
            subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + vk, str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(tmp / "header.exe")], check=True)
        for switch, want, near in RUNS:
            env = {k: v for k, v in os.environ.items() if k != "RX_VK_FOLIAGE_SHADOW_CULL"}
            if switch is not None:
                env["RX_VK_FOLIAGE_SHADOW_CULL"] = switch
            subprocess.run([str(tmp / "function.exe"), str(want), str(near)], env=env, check=True, timeout=30)
    print("PASS: the foliage in the far shadow cascades")


if __name__ == "__main__":
    main()
