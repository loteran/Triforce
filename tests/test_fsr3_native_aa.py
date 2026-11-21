"""AMD FSR 3.1 Native AA on the Upscaler row: FSR 3.1's own passes with the 3D drawn at the output's full size, as NVIDIA DLAA
and AMD FSR 4 Native AA are for their engines. The row's rules are gpu/vk/upscaler_rows.h's (tests/test_upscaler_rows.cpp,
compiled and run here: the value 6 after the ones saved before, shown after FSR 3.1, the Render resolution row fixed on
Native); the rest is read from the source: the FSR 3.1 path takes the row's two values, the game is told to draw the 3D at
full scale, and that path takes a scene as large as its output.

    python -I -B tests/test_fsr3_native_aa.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def function(source, start):
    """A function's body from its first line to its closing brace at the start of a line"""
    at = source.find(start)
    check(at >= 0, "%s is missing" % start.rstrip(" {("))
    body = source[at:]
    return body[:body.index("\n}\n")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    vk = ROOT / "gpu" / "vk"
    rows = (vk / "upscaler_rows.h").read_text(encoding="utf-8")
    check(re.search(r"enum \{ GAME = 0, DLSS = 1, DLAA = 2, FSR3 = 3, FSR4 = 4, FSR4_AA = 5, FSR3_AA = 6, kUpscalers = 7 \};", rows),
          "upscaler_rows.h: the saved values moved, or FSR 3.1's native AA is not the one after them")

    # FSR 3.1 runs for either of its values, and as its Native AA for the second (RX_VK_FSR=2 for a run)
    fsr3 = (vk / "vk_fsr3.cpp").read_text(encoding="utf-8")
    # (linux fork: the row's values are read in fsr3_requested, which AMD's library (vk_ffx3.cpp) and these passes share;
    # fsr3_wanted is the passes' own share of it)
    requested = function(fsr3, "bool fsr3_requested() {")
    wanted = function(fsr3, "bool fsr3_wanted() {")
    check("upscaler_rows::is_fsr3(opts.dlss) && !temporal_env_chosen()" in requested and "fsr3_requested()" in wanted and
          "fsr3::supported()" in wanted,
          "fsr3_wanted does not take both of the row's FSR 3.1 values, or drops RX_VK_DLSS / RX_VK_FSR4 over them")
    native = function(fsr3, "bool fsr3_native() {")
    check("fsr3_wanted()" in native and "fsr3_requested() && ffx3_usable()" in native and "opts.dlss == upscaler_rows::FSR3_AA" in native and "env >= 2" in native,
          "fsr3_native is not the row's Native AA (or RX_VK_FSR=2) where FSR 3.1 runs")
    # the game draws the 3D at full scale then, and its dynamic resolution is off: the two answers the host reads at the start
    temporal = (vk / "vk_temporal.cpp").read_text(encoding="utf-8")
    check(re.search(r"bool temporal_full_scale\(\) \{ return ur::full_scale\(temporal_mode\(\)\) \|\| fsr3_native\(\); \}", temporal),
          "temporal_full_scale does not hold the 3D at full scale for FSR 3.1's Native AA")
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    check("id == 2 ? resolution_auto() && !temporal_full_scale()" in replay and "id == 3 ? (temporal_full_scale() ? 10000 : resolution_scale())" in replay,
          "the renderer no longer answers the host's scale (options 2 and 3) with the full scale of a native AA")
    # at full scale the game's upscale is its full-size pass, one of the two the stand-ins know by key: FSR 3.1 is offered it,
    # and takes an input as large as the output (it refuses only a smaller output)
    draw = (vk / "vk_draw.cpp").read_text(encoding="utf-8")
    check("bool gameplay_network(uint64_t key) { return key == 0xfd029a317a9a4b94ull || key == 0x9dc98871eb7fc101ull; }" in draw,
          "vk_draw.cpp: the full-size pass is no longer one of the gameplay networks")
    stand_in = function(draw, "bool fsr3_stand_in(")
    check("fsr3_upscale(cmd, p.key, gameplay_network(p.key)," in stand_in, "fsr3_stand_in no longer says which dispatches are the known ones")
    upscale = function(fsr3, "bool fsr3_upscale(")
    known = upscale.index("if (!known && (")
    check("(out->width >> out_level) <= in->width" in upscale[known:upscale.index("return false;", known)],
          "fsr3_upscale: the same-size refusal is no longer for unknown networks only")
    check("ow < f.width || oh < f.height) return fail(\"unexpected output\")" in upscale and "ow <= f.width" not in upscale,
          "fsr3_upscale refuses an output of the scene's own size")
    check("ai.upscale_width = ow, ai.upscale_height = oh;" in upscale and "ai.render_width = f.width, ai.render_height = f.height;" in upscale,
          "fsr3_upscale no longer gives FSR the scene's size and the output's")
    pre = function(fsr3, "bool fsr3_pre(")
    check("if (ow < f.width || oh < f.height) return fail(\"unexpected output\");" in pre, "fsr3_pre refuses an output of the scene's own size")
    pre_chain = function(temporal, "bool pre_fsr3(")
    check("const bool scaled = ow != rw || oh != rh;" in pre_chain, "pre_fsr3 no longer tells a picture of the scene's size from a larger one")

    # the menu: what runs is shown, both of FSR 3.1's are offered where it runs, and the Render resolution row says Native
    menu = (vk / "vk_menu.cpp").read_text(encoding="utf-8")
    check("int upscaler() { return fsr3_wanted() ? (int)(fsr3_native() ? ur::FSR3_AA : ur::FSR3) : temporal_mode(); }" in menu,
          "the menu does not show FSR 3.1's Native AA when it runs")
    check("ur::fsr3_bits(fsr3::supported() || ffx3_usable())" in function(menu, "unsigned upscalers_available() {"),   # (linux fork: or AMD's library)
          "the Upscaler row does not offer both of FSR 3.1's where it runs")
    # ((Triforce, options_version 7) the Upscaler row names the engine of each of the seven values; a native AA is the Render
    # resolution row's Native (100%) step, upscaler_rows.h step_resolution)
    names = function(menu, "const char* tx_upscaler(int) {")
    table = re.search(r"static const char\* t\[ur::kUpscalers\] = \{(.*?)\};", names, re.S)
    check(table and [n.strip().strip('"') for n in table.group(1).replace("\n", " ").split(",")] ==
          ["Game", "NVIDIA DLSS", "NVIDIA DLSS", "AMD FSR 3.1", "AMD FSR 4", "AMD FSR 4", "AMD FSR 3.1"],
          "tx_upscaler's names are not the seven values' engines in their saved order")
    # ((Triforce, FSR4-7) FSR 4 by the version AMD's library runs, fsr4_name, the engine's name too)
    check("const int up = ur::engine(std::clamp(upscaler(), 0, ur::kUpscalers - 1));" in names and "return t[up];" in names,
          "tx_upscaler does not name what runs by its engine")
    hints = function(menu, "const char* hn_upscaler(int v, int variant) {")
    check("v = ur::engine(v);" in hints, "hn_upscaler does not say each value's engine")
    check('if (ur::full_scale(up)) return "Native (100%)";' in function(menu, "const char* tx_resolution(int v) {"),
          "the Render resolution row does not say Native (100%) with a native AA")
    check("if (ur::full_scale(up))" in function(menu, "const char* hn_resolution(int v, int variant) {"),
          "the Render resolution row's help has no line for a native AA")
    check("ur::step_resolution(upscaler(), *r.value, dir)" in function(menu, "void set_row("),
          "the Render resolution row does not step through Native (100%)")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    check("| Upscaler | Game, NVIDIA DLSS, AMD FSR 3.1, AMD FSR 4" in readme and
          "Native (100%: DLAA, or FSR's native anti-aliasing)" in readme,
          "readme.md's settings table has no Upscaler row, or no Native (100%) on the Render resolution row")
    guide = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("6 AMD FSR 3.1 Native AA" in guide and "`=2`: FSR 3.1 as its Native AA" in guide,
          "docs/development.md does not say the Upscaler's value 6, or RX_VK_FSR=2")

    with tempfile.TemporaryDirectory(prefix="fsr3-native-") as tmp:
        exe = Path(tmp) / "upscaler-rows"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests" / "test_upscaler_rows.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: AMD FSR 3.1 Native AA on the Upscaler row")


if __name__ == "__main__":
    main()
