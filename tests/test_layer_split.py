"""A new layered ASTC texture's first upload in parts, and the looks that resolved to the dummy made stale alone (gpu/vk/layer_split.h,
vk_texture.cpp texture_touch and texture_frame, vk_draw.cpp texture_view_of; no game, no GPU).

In the owner's test7 session one texture, 1365 x 1365 x 90 layers of ASTC kept as BC3, was decoded and encoded in one frame's
upload command buffer: 114.8 ms of GPU time, and the next frame's capture waited 117.2 ms for it. Now such a texture's first
upload goes a part of its layers a frame while the hold of the last picture may leave textures for later (its draws sample the
dummy, its frames are held), and what is left goes at once for a dispatch or at the hold's bound. And a frame that left textures
for later made every texture view of every draw stale (destroy_gen); now only the looks that resolved to the dummy are.

Compiles and runs the header (the switch, the texels, the parts, the ranges of a part), and reads the source for what needs the
game: split_take's rules, the part before the upload and the rest owed, the last part's generation (test_upload_owed.py runs the
cut parts), the dummy's generation at the frame's end and in texture_view_of's cache, the counts in the stall line, the switches
in the development guide.

    python -I -B tests/test_layer_split.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def function(text, head):
    at = text.index(head)
    return text[at:text.index("\n}\n", at)]


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit(where + " lacks: " + needle)


def check_source():
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    need("vk_texture.cpp", tex, ['#include "layer_split.h"'])
    take = function(tex, "bool split_take(const Texture& tex, upload_owed::Ranges& part, upload_owed::Ranges& rest) {")
    need("vk_texture.cpp split_take", take, [
        "tex.uploaded || !tex.astc || tex.layers < 2",   # (a new layered ASTC texture only)
        "upload_defer_ok && hold_mode.load(std::memory_order_relaxed) == present_hold::DEFERS",   # (parts only while the hold may)
        "if (!going && (n < 2 || !may)) return false;",   # (not begun and not to be split: whole, as before)
        "part = remaining, rest.clear();"])   # (a dispatch or the hold's flush: what is left at once)
    pending = function(tex, "bool split_pending(const Texture& tex) {")
    need("vk_texture.cpp split_pending", pending, ["!tex.uploaded", "!tex.owed.whole", "!tex.owed.failed"])
    frame = function(tex, "void texture_frame(uint64_t completed) {")
    if frame.count("if (dummy_gen_on()) dummy_gen++;") != 2 or frame.count("else destroy_gen++;") != 2:
        raise SystemExit("vk_texture.cpp texture_frame: a frame that left textures for later does not make the dummy's looks stale alone")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    view = function(draw, "VkImageView texture_view_of(")
    need("vk_draw.cpp texture_view_of", view, [
        "it->second.tex_epoch == texture_dummy_generation()",   # (the hit: a dummy look stands while its generation does)
        "!view && !tex && known ? texture_dummy_generation() : texture_epoch()",   # (and is noted with it)
        "st_view_lookups.fetch_add("])
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    need("vk_replay.cpp status_tick", function(replay, "void status_tick() {"),
         ["texture_split_parts()", "draw_view_lookups()", "program_pick_stats(&pk_lock, &pk_codes, &pk_n, &pk_ns);",
          "texture_create_stats(&mk, &mk_ns, &mk_later);", "vma_image_stats(&im, &im_ns, &rt, &rt_ns);"])
    # ((Triforce, test8) a new sampled texture is not made once the frame's budget is taken: asked before it is made, only for a
    # draw's look while the hold may leave textures, never the frame's first make, the frame counted owed (held), the frame's
    # makes counted and set back at its end)
    get = function(tex, "Texture* texture_get(const nvn_tex* t, bool for_sampling) {")
    if not (get.index("if (create_later(for_sampling)) return nullptr;") < get.index("tex = create(k, t, fi.format") <
            get.index("create_timed(")):
        raise SystemExit("vk_texture.cpp texture_get: the budget is not asked before the texture is made, or the make not timed")
    later = function(tex, "bool create_later(bool for_sampling) {")
    need("vk_texture.cpp create_later", later, [
        "!for_sampling || !upload_defer_ok || hold_mode.load(std::memory_order_relaxed) != present_hold::DEFERS",
        "if (!made || (made < budget && !new_texture_over_budget())) return false;",
        "frame_owed.fetch_add(1"])
    need("vk_texture.cpp texture_frame", frame, ["frame_create_ns.store(0"])
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    need("docs/development.md", dev, ["RX_VK_UPLOAD_SPLIT_MB", "RX_VK_DUMMY_GEN", "RX_VK_CREATE_NEW_MS"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="layer-split-") as tmp:
        exe = Path(tmp) / "layer-split"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_layer_split.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: a layered texture's first upload in parts, the dummy's looks stale alone")


if __name__ == "__main__":
    main()
