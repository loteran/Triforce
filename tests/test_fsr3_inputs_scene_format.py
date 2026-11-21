"""(Triforce, 11-16) Frame generation's inputs in a run whose scene is RGBA16F (HDR Native with RenoDX: host/nn_vi.c
hos_scene_rgba16f). The owner's DLSS frame generation never made a frame: "frame generation paused", "1 frame(s) shown for each
present", and no "fsr3 inputs" line, because gpu/vk/vk_fsr3_inputs.cpp note_scene_draw kept the scene's camera only for a
B10G11R11 target and gpu/vk/vk_fsr3.cpp fsr3_upscale left an RGBA16F network (in and out "/97") before making the inputs. Both
now take the run's scene format as the DLSS upscaler does (vk_temporal.cpp temporal_scene_format). Source checks, with the old
lines as the mutations they must reject.

    python -I -B tests/test_fsr3_inputs_scene_format.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def function(source, head):
    start = source.index(head)
    return source[start:source.index("\n}\n", start) + 3]


def checks(inputs, fsr3):
    note = function(inputs, "void note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,")
    check("if (!temporal_scene_format(color0->format) || !wanted()) return;" in note,
          "vk_fsr3_inputs.cpp note_scene_draw does not keep the camera of the run's scene format (RGBA16F with RenoDX's HDR)")
    check("color0->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32" not in note, "note_scene_draw still takes B10G11R11 alone")
    check('#include "vk_temporal.h"' in inputs, "vk_fsr3_inputs.cpp does not include vk_temporal.h for temporal_scene_format")
    up = function(fsr3, "bool fsr3_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view, Texture* out,")
    check("if (!known && (!temporal_scene_format(in->format) || out->format != in->format ||" in up,
          "vk_fsr3.cpp fsr3_upscale does not take a network of the run's scene format (RGBA16F in and out) for the inputs")
    check("in->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32" not in up,
          "fsr3_upscale still leaves an RGBA16F network before the inputs")
    # (the inputs are made before the network's own upscale is left to DLSS: prepare comes after the format check and before
    # the return for frame interpolation's inputs alone)
    # ((Triforce) with hdr: an RGBA16F network's colour unclamped, tests/test_fsr3_hdr_scene.py)
    check(up.index("!temporal_scene_format(in->format)") < up.index("fsr3_inputs::prepare(cmd, in, in_view, f, hdr)") <
          up.index("if (!upscaling) return false;"), "fsr3_upscale: the inputs are not made where DLSS upscales")


def main():
    inputs = (ROOT / "gpu/vk/vk_fsr3_inputs.cpp").read_text(encoding="utf-8")
    fsr3 = (ROOT / "gpu/vk/vk_fsr3.cpp").read_text(encoding="utf-8")
    checks(inputs, fsr3)
    # (the old lines must fail)
    old_note = inputs.replace("if (!temporal_scene_format(color0->format) || !wanted()) return;",
                              "if (color0->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || !wanted()) return;")
    old_up = fsr3.replace("if (!known && (!temporal_scene_format(in->format) || out->format != in->format ||",
                          "if (!known && (in->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||")
    for name, a, b in (("the old note", old_note, fsr3), ("the old upscale check", inputs, old_up)):
        try:
            checks(a, b)
        except SystemExit:
            continue
        raise SystemExit("FAIL: the mutation '%s' passed" % name)
    print("PASS: frame generation's inputs are made in an RGBA16F run (the camera kept, the network taken); the old lines fail")


if __name__ == "__main__":
    main()
