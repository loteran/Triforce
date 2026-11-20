"""Compile and run the check for texture bindings read by texel position (gpu/vk/spv_exact.h; no game/GPU)."""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """The shader modules are still scanned, and a draw still asks for a texture whole at such a binding"""
    shader = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    texture = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    for src, name, needles in ((shader, "vk_shader.cpp", ('#include "spv_exact.h"', "spv_exact::scan(")),
                               (draw, "vk_draw.cpp", ("texture_want_whole(tex)",)),
                               (texture, "vk_texture.cpp", ("void texture_want_whole(Texture* tex)", "whole_keys.count(key)"))):
        for needle in needles:
            if needle not in src:
                raise SystemExit(name + " no longer has: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="spv-exact-") as tmp:
        exe = Path(tmp) / "spv-exact"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_spv_exact.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
