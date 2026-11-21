"""Float selects of a NaN constant made selects of the bits on AMD's older shader compiler (gpu/vk/spv_nan_select.h, after
NX2navi's e38f42a, issue #156; no game, no GPU).

- spv_nan_select::rewrite, compiled and run on authored SPIR-V (tests/test_spv_nan_select.cpp): 32-bit selects of the mask
  on either side, against a constant or a value, in a module with or without a 32-bit unsigned type; 16-bit ones where the
  module has a 16-bit unsigned type, left where it has none; what comes out read back (each id defined once and below the
  bound, types and constants before the first function, operands defined before their use) and evaluated: each select's
  result keeps its float type and gives the same bits for either condition, and no float select of a NaN constant is left;
  rewritten once; modules with nothing to change (numbers, infinity, a vector select) and modules it does not read left
  alone; RX_VK_NAN_SELECTS. With spirv-val on the PATH, every module made, before and after, passes it too;
- vk_shader.cpp: make_module rewrites the module it hands the driver when dev.fixes.nan_bits is set, after the fog's patch
  and never a RenoDX replacement, spv_nan_select.h is included in a part marked as outside the translation stamp and the
  stamp's text (nx2/build.py translation_text) holds nothing of it, so the program cache is carried over this change as it
  was and no other card sees a change; the translator (Eden) does not name it; the switch is documented.

    python -I -B tests/test_spv_nan_select.py [--cxx g++] [--spirv-val spirv-val]
"""
import argparse
import ast
import hashlib
import os
import re
import shutil
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
        elif s == '#include "spv_nan_select.h"':
            if not inside:
                raise SystemExit("vk_shader.cpp includes spv_nan_select.h in the translation stamp: every program would be "
                                 "translated again, on every card")
            included = True
    if not included:
        raise SystemExit("vk_shader.cpp no longer includes spv_nan_select.h")
    stamped = translation_text("vk_shader.cpp", shader_bytes).decode("utf-8")
    if "spv_nan_select" in stamped or "RX_VK_NAN_SELECTS" in stamped:
        raise SystemExit("the NaN selects' rewrite is in the translation stamp's text: every program would be translated again")
    eden = ROOT / "gpu/vk/eden"
    for path in eden.rglob("*"):
        if path.suffix in (".cpp", ".h") and "spv_nan_select" in path.read_text(encoding="utf-8", errors="replace"):
            raise SystemExit("%s names spv_nan_select: the translator's own files are in the stamp" % path.relative_to(ROOT))
    m = re.search(r"\nVkShaderModule make_module\(const std::vector<uint32_t>& spirv,.*?\n}\n", shader, re.S)
    if not m:
        raise SystemExit("vk_shader.cpp: make_module not found")
    body = m.group(0)
    for needle in ("if (!other && dev.fixes.nan_bits) {", 'spv_nan_select::on(getenv("RX_VK_NAN_SELECTS"))',
                   "const std::vector<uint32_t>& in = !fog.empty() ? fog : base;",
                   "if (selects) nan_sel = spv_nan_select::rewrite(in.data(), in.size(), &found);",
                   "const std::vector<uint32_t>& code = other ? *other : !nan_sel.empty() ? nan_sel : !fog.empty() ? fog : base;",
                   "std::vector<uint32_t> salted = !nan_sel.empty() ? nan_sel : !fog.empty() ? fog : base;",
                   "vk: NaN selects: module %08X"):
        if needle not in body:
            raise SystemExit("make_module no longer has: " + needle)
    # (after the fog's patch, before the module is made; the record's SPIR-V, written from the translation, is not changed)
    if not body.find("spv_fog::keep_froxels") < body.find("spv_nan_select::rewrite") < body.find("vkCreateShaderModule"):
        raise SystemExit("make_module rewrites the selects before the fog's patch or after the module is made")
    vendor = (ROOT / "gpu/vk/vk_vendor.h").read_text(encoding="utf-8")
    if "x.nan_bits = amd_older_compiler(f);" not in vendor or 'over("RX_VK_NAN_BITS", x.nan_bits);' not in vendor:
        raise SystemExit("vk_vendor.h: nan_bits is no longer AMD's older compiler's, or RX_VK_NAN_BITS no longer decides it")
    docs = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "`RX_VK_NAN_SELECTS=0`" not in docs:
        raise SystemExit("docs/development.md does not describe RX_VK_NAN_SELECTS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--spirv-val", default=shutil.which("spirv-val"))
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="spv-nan-select-") as tmp:
        exe = Path(tmp) / "spv-nan-select"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_spv_nan_select.cpp"), "-o", str(exe)], check=True)
        if not args.spirv_val:
            subprocess.run([str(exe)], check=True)
            print("spirv-val not found: the modules were not validated")
            return
        out = Path(tmp) / "modules"
        out.mkdir()
        subprocess.run([str(exe), str(out)], check=True)
        modules = sorted(out.glob("*.spv"))
        if not modules:
            raise SystemExit("no module was written for spirv-val")
        for spv in modules:
            result = subprocess.run([args.spirv_val, "--target-env", "vulkan1.2", str(spv)], capture_output=True, text=True)
            if result.returncode:
                raise SystemExit("spirv-val refuses %s:\n%s%s" % (spv.name, result.stdout, result.stderr))
        print("spirv-val: %d modules valid" % len(modules))


if __name__ == "__main__":
    main()
