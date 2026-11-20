"""A texture the temporal upscaler keeps from one draw to a later one is given up only when that texture itself was destroyed.

With the engine (NVIDIA DLSS, AMD FSR 4) before the game's bloom and scaling up, the tone mapping is drawn once more at the
output's size from the engine's picture, into the upscale's output as the last frame's dispatch had it. That output, and the
scene's colour and depth kept from the scene's draws, were checked with texture_destroy_generation, which changes whenever
any texture is destroyed. On a card whose video memory is full the cache lets textures go in most frames (a tester's 8 GB
card at 2560x1440: 154 times in some 2300 frames), and each of those frames was shown from the scene's size scaled up in
place of the engine's picture: a soft frame, reported as a blur that comes and goes.

What must hold: gpu/vk/texture_gone.h tells one texture's end from another's (compiled and run here, no game or GPU),
vk_texture.cpp records every texture that goes there, and vk_temporal.cpp and vk_fsr3_inputs.cpp (AMD FSR 3.1's scene depth)
ask it, not the generation, for the textures they keep (the generation stays for the views' caches, which any texture's end
may make stale).

python -I -B tests/test_texture_gone.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


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


def check_source():
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    temporal = (ROOT / "gpu/vk/vk_temporal.cpp").read_text(encoding="utf-8")
    header = (ROOT / "gpu/vk/vk_texture.h").read_text(encoding="utf-8")
    # (every texture's end is recorded, with the generation: destroy_later is where a texture goes)
    need("vk_texture.cpp: destroy_later()", block(tex, "void destroy_later(Texture& tex)"), ["destroy_gen++;", "gone_record.went(tex.id);"])
    need("vk_texture.cpp", tex, ['#include "texture_gone.h"', "texture_gone::Record gone_record;"])
    need("vk_texture.cpp: texture_destroyed_count()", block(tex, "uint64_t texture_destroyed_count()"), ["return gone_record.count();"])
    need("vk_texture.cpp: texture_destroyed_since()", block(tex, "bool texture_destroyed_since(uint32_t id, uint64_t since)"),
         ["return gone_record.gone_since(id, since);"])
    need("vk_texture.h", header, ["uint64_t texture_destroyed_count();", "bool texture_destroyed_since(uint32_t id, uint64_t since);"])
    # (every texture has a number: one without would always count as gone, and the engine would never draw before the bloom)
    need("vk_texture.cpp: create()", block(tex, "Texture* create(const Key& key"), ["p->id = ++ids;"])
    if tex.count("std::make_unique<Texture>()") != 1:
        raise SystemExit("vk_texture.cpp: textures are made in another place than create(), where they get their number")

    # (the upscaler: the textures it keeps are asked about themselves)
    need("vk_temporal.cpp: there_still()", temporal,
         ["bool there_still(uint32_t id, uint64_t noted) { return !texture_destroyed_since(id, noted); }",
          "bool there_still(const Camera& cam) { return there_still(cam.color_id, cam.noted) && there_still(cam.depth_id, cam.noted); }"])
    need("vk_temporal.cpp: temporal_note_scene_draw()", block(temporal, "void temporal_note_scene_draw(const Texture* color0"),
         ["k.color_id = color0->id, k.depth_id = depth->id, k.noted = texture_destroyed_count();"])
    need("vk_temporal.cpp: pre_dispatch()", block(temporal, "bool pre_dispatch(VkCommandBuffer cmd, int mode"),
         ["s.pre_out = out, s.pre_out_id = out->id, s.pre_out_noted = texture_destroyed_count();"])
    need("vk_temporal.cpp: temporal_pre_composite()", block(temporal, "bool temporal_pre_composite(const Texture* color0"),
         ["!there_still(s.pre_out_id, s.pre_out_noted)"])
    need("vk_temporal.cpp: temporal_pre_tonemap()", block(temporal, "Texture* temporal_pre_tonemap(VkCommandBuffer cmd)"), ["!there_still(cam)"])
    # (and no texture it keeps is given up for another texture's end: the generation is left to the views' cache alone)
    uses = [line.strip() for line in temporal.splitlines() if "texture_destroy_generation()" in line and not line.strip().startswith("//")]
    if uses != ["const uint64_t gen = texture_destroy_generation();"]:
        raise SystemExit("vk_temporal.cpp asks texture_destroy_generation for more than its views' cache: " + "; ".join(uses))
    need("vk_temporal.cpp: depth_view()", block(temporal, "VkImageView depth_view(Texture* t)"),
         ["const uint64_t gen = texture_destroy_generation();", "if (gen != s.depth_views_gen)"])

    # (AMD FSR 3.1's own inputs keep the scene's depth the same way: a frame is not left to the game's upscale for another
    # texture's end)
    fsr3 = (ROOT / "gpu/vk/vk_fsr3_inputs.cpp").read_text(encoding="utf-8")
    need("vk_fsr3_inputs.cpp", fsr3,
         ["k.depth = depth, k.depth_id = depth->id, k.noted = texture_destroyed_count(), k.frame = now;",
          'if (texture_destroyed_since(cam.depth_id, cam.noted)) return fail("the scene depth is gone");'])
    uses = [line.strip() for line in fsr3.splitlines() if "texture_destroy_generation()" in line and not line.strip().startswith("//")]
    if uses != ["const uint64_t gen = texture_destroy_generation();"]:
        raise SystemExit("vk_fsr3_inputs.cpp asks texture_destroy_generation for more than its views' cache: " + "; ".join(uses))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="texture-gone-") as tmp:
        exe = Path(tmp) / "texture-gone"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_texture_gone.cpp"), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
