"""Frame generation's straight lines (gpu/vk/vk_framegen.cpp; gpu/vk/shaders/fg_*.comp): the made frames measured.

The passes' own files -- fg_luma, fg_flow, fg_median and fg_synth (the old way, RX_VK_FRAMEGEN_FINE=0), fg_half,
fg_fine_flow, fg_fine_pick and fg_fine_synth (the fine vectors) -- are turned into C++ here (the layout lines into
globals, the swizzles into calls; tests/framegen_lines_test.cpp has the rest of GLSL they use, and stores into each image
as its format keeps it) and run as make_vectors and make_frame dispatch them, on pictures made by
tests/framegen_lines_test.cpp: straight lines 1.2 to 2.4 pixels wide on a surface with some texture, a fence of thin
lines, a box moving over a still world and a still HUD bar, as a camera pans, pans fast, turns and zooms, at 2x (t 1/2)
and 3x (t 1/3, 2/3). Each made frame is measured against the picture of its own moment: how far its line pixels are from
the true lines (a line bent, broken or doubled), and how many of the true lines' pixels it lost (a thin line not drawn).
The fine vectors must keep both small in every scene and do better than the old way in each; two pictures alike must
give that picture, and a cut (nothing in common) almost nothing that neither picture nor their blend has. The numbers
are printed, with the texel reads each way costs a pixel: they are the evidence the fine vectors are on by default.
FG_LINES_DEBUG=<folder> also writes the pictures (PPM) and each block's vector error there.

    python -I -B tests/test_framegen_lines.py [--cxx g++]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "gpu" / "vk" / "shaders"
PASSES = ["fg_luma", "fg_half", "fg_flow", "fg_median", "fg_synth", "fg_fine_flow", "fg_fine_pick", "fg_fine_synth"]


def fail(what):
    raise SystemExit("FAIL: " + what)


def translate(name):
    """A pass as C++: its bindings, push constants and specialization constants as globals of a namespace of its own"""
    out = []
    for line in (SHADERS / (name + ".comp")).read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if s.startswith("#version") or s.startswith("#extension") or s.startswith("layout(local_size"):
            continue
        m = re.match(r"layout\(binding = \d+\) uniform sampler2D (\w+);", s)
        if m:
            out.append("static Sampler %s;" % m.group(1))
            continue
        m = re.match(r"layout\(binding = \d+, \w+\) uniform writeonly image2D (\w+);", s)
        if m:
            out.append("static Image %s;" % m.group(1))
            continue
        m = re.match(r"layout\(push_constant\) uniform \w+ \{(.*)\} (\w+);", s)
        if m:
            out.append("static struct {%s} %s;" % (m.group(1), m.group(2)))
            continue
        m = re.match(r"layout\(constant_id = \d+\) const (int|float) (\w+) = ([\d.]+);", s)
        if m:
            out.append("static %s %s = %s;" % m.groups())
            continue
        if s.startswith("layout("):
            fail("%s.comp: a layout line this test does not know: %s" % (name, s))
        out.append(line)
    code = "\n".join(out)
    code = re.sub(r"\bvoid main\(\)", "void main_()", code)
    code = re.sub(r"\.(xy|rgb)\b", r".\1()", code)
    # (GLSL's literals are floats: C++'s doubles would make the arithmetic of another precision)
    code = re.sub(r"(?<![\w.])(\d+\.\d*(?:[eE][-+]?\d+)?|\d+[eE][-+]?\d+)(?![\w.])", r"\1f", code)
    return "namespace sh_%s {\n%s\n}  // namespace sh_%s\n" % (name, code, name)


def check_source():
    """vk_framegen.cpp runs the passes as the program here does, and the fine vectors are its default; their coarse bias"""
    source = (ROOT / "gpu" / "vk" / "vk_framegen.cpp").read_text(encoding="utf-8")
    bias = re.search(r"constexpr float kFineBias = ([\d.]+)f;", source)
    if not bias:
        fail("vk_framegen.cpp has no kFineBias")
    gate = source[source.index("bool fine_vectors()"):]
    gate = gate[:gate.index("}")]
    if '!getenv("RX_VK_FRAMEGEN_FINE") || atoi(getenv("RX_VK_FRAMEGEN_FINE")) != 0' not in gate:
        fail("the fine vectors are no longer on unless RX_VK_FRAMEGEN_FINE=0")
    vectors = source[source.index("void make_vectors("):source.index("void make_frame(")]
    order = ["fg.flow,", "fg.median,", "fg.fine_flow,", "fg.fine_pick,"]
    at = [vectors.find(x) for x in order]
    if -1 in at or at != sorted(at) or "for (int i = 0; i < 2; i++)" not in vectors:
        fail("make_vectors no longer runs fg_flow, fg_median, fg_fine_flow and fg_fine_pick twice, in that order")
    if "fine ? kFineBias : 1.0f" not in source:
        fail("fg_flow.comp's BIAS is no longer kFineBias with the fine vectors and 1 without")
    if "pass(cmd, fg.half," not in source[source.index("bool framegen_record("):]:
        fail("framegen_record no longer makes the half-size luma")
    return bias.group(1)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    bias = check_source()
    template = (ROOT / "tests" / "framegen_lines_test.cpp").read_text(encoding="utf-8")
    unit = template.replace("/* SHADERS */", "\n".join(translate(n) for n in PASSES))
    unit = unit.replace("/* FINE_BIAS */", bias + "f")
    with tempfile.TemporaryDirectory(prefix="framegen-lines-") as tmp:
        src, exe = Path(tmp) / "lines.cpp", Path(tmp) / "lines.exe"
        src.write_text(unit, encoding="utf-8")
        r = subprocess.run([a.cxx, "-std=c++20", "-O2", "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-o",
                            str(exe), str(src)], capture_output=True, text=True)
        if r.returncode:
            fail("the passes do not compile as C++:\n" + r.stdout + r.stderr)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
        sys.stdout.write(r.stdout)
        if r.returncode:
            fail(r.stderr or "exit %d" % r.returncode)
    print("PASS: frame generation's straight lines")


if __name__ == "__main__":
    main()
