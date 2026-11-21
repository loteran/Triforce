"""(Triforce) AMD FSR 3.1 draws in an RGBA16F run (RenoDX's HDR Native: host/nn_vi.c hos_scene_rgba16f), with Upscale before
bloom off and on. It never did: at the upscale's dispatch gpu/vk/vk_fsr3.cpp fsr3_upscale returned what the before-bloom
dispatch said (false with the row off) for an RGBA16F network and refused any output but B10G11R11, and before the bloom
vk_temporal.cpp temporal_pre_tonemap turned an RGBA16F scene away for FSR 3.1, whose pass there wrote B10G11R11 alone. The
game's own upscale ran each frame while the menu said AMD FSR 3.1.

What must hold, read from the source (no game, no GPU):
- fsr3_upscale: no early return for an RGBA16F network; its inputs made with the colour unclamped (hdr: the network samples
  the square root of HDR colour, over 1 where the scene is brighter); an RGBA16F output taken for an RGBA16F input, written
  through dlss_out.comp's -DHDR layout (dlss_out_hdr_comp_spv.h) squared (linear 2), the B10G11R11 one as before (0);
- the shaders it relies on: dlss_out.comp's -DHDR layout declares an rgba16f output and squares at linear 2, fsr3_prep.comp
  keeps the colour unclamped with flag 4;
- before the bloom: temporal_pre_tonemap takes an RGBA16F scene for FSR 3.1, and pre_fsr3 writes FSR's picture back
  through the -DHDR layout for it, its copy in the scene's own format.
The old lines fail.

    python -I -B tests/test_fsr3_hdr_scene.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def function(source, head):
    start = source.index(head)
    return source[start:source.index("\n}\n", start) + 3]


def checks(fsr3, temporal, out_comp, prep_comp):
    up = function(fsr3, "bool fsr3_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view, Texture* out,")
    check("return temporal_pre_dispatch(cmd, in, in_view, out, out_view, out_level, true);" not in up,
          "fsr3_upscale still leaves an RGBA16F network to the before-bloom dispatch's answer")
    check("const bool hdr = in->format == VK_FORMAT_R16G16B16A16_SFLOAT;" in up and
          "fsr3_inputs::prepare(cmd, in, in_view, f, hdr)" in up, "an RGBA16F network's colour is clamped at 1 for FSR")
    check("const bool out_ok = hdr ? out->format == VK_FORMAT_R16G16B16A16_SFLOAT && temporal_scene_format(in->format)" in up and
          ": out->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;" in up, "fsr3_upscale does not take an RGBA16F output")
    check("out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||" not in up, "fsr3_upscale still refuses any output but B10G11R11")
    check("make_out_pipeline(hdr)" in up and "hdr ? o.pipe_hdr : o.pipe" in up and "hdr ? 2 : 0" in up,
          "FSR's picture is not written through the -DHDR output pass, squared, into an RGBA16F output")
    check(up.index("const bool hdr") < up.index("fsr3_inputs::prepare(cmd, in, in_view, f, hdr)") < up.index("make_out_pipeline(hdr)"),
          "fsr3_upscale's HDR steps are out of order")
    made = function(fsr3, "bool make_out_pipeline(bool hdr) {")
    check("DLSS_OUT_HDR_COMP_SPV" in made and "o.pipe_hdr" in made, "the -DHDR output pass is never made")
    check('#include "shaders/dlss_out_hdr_comp_spv.h"' in fsr3, "vk_fsr3.cpp does not include the -DHDR output pass")
    pre = function(fsr3, "bool fsr3_pre(")
    check("fsr3_inputs::prepare(cmd, scene, scene_view, f, true)" in pre, "before the bloom FSR's colour is not the scene's own")
    # the shaders: -DHDR writes rgba16f and squares at 2; fsr3_prep's flag 4 does not clamp at 1
    hdr_part = out_comp[out_comp.index("#ifdef HDR"):]
    check("layout(binding = 1, rgba16f) uniform writeonly image2D dst;" in hdr_part, "dlss_out.comp's -DHDR layout writes no rgba16f")
    check("else if (pc.linear == 2) c *= c;" in out_comp, "dlss_out.comp's -DHDR layout no longer squares at linear 2")
    check("(pc.flags & 4) != 0 ? min(max(c, vec3(0.0)), vec3(64000.0)) : clamp(c, 0.0, 1.0)" in prep_comp,
          "fsr3_prep.comp no longer keeps the colour unclamped with flag 4")
    # before the bloom
    tone = function(temporal, "Texture* temporal_pre_tonemap(VkCommandBuffer cmd) {")
    check("if (f3 && scene->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32) return nullptr;" not in tone,
          "temporal_pre_tonemap still turns an RGBA16F scene away for FSR 3.1")
    pre3 = function(temporal, "bool pre_fsr3(")
    check("const bool wide = scene->format == VK_FORMAT_R16G16B16A16_SFLOAT;" in pre3 and "make_pipelines(wide)" in pre3 and
          "make_image(cmd, s.base, scene->format, rw, rh)" in pre3 and "wide ? s.out_hdr : s.out" in pre3,
          "pre_fsr3 does not write FSR's picture into an RGBA16F scene through the -DHDR layout")
    check("make_pipelines(false)" not in pre3 and "VK_FORMAT_B10G11R11_UFLOAT_PACK32, rw, rh" not in pre3,
          "pre_fsr3 still makes the B10G11R11 pass and copy alone")


MUTATIONS = [
    ("the old early return", "fsr3", "    // ((Triforce) an RGBA16F run's network (RenoDX's HDR Native)",
     "    if (in->format == VK_FORMAT_R16G16B16A16_SFLOAT) return temporal_pre_dispatch(cmd, in, in_view, out, out_view, out_level, true);\n"
     "    // ((Triforce) an RGBA16F run's network (RenoDX's HDR Native)"),
    ("the old clamped colour", "fsr3", "fsr3_inputs::prepare(cmd, in, in_view, f, hdr)", "fsr3_inputs::prepare(cmd, in, in_view, f)"),
    ("the plain output pass alone", "fsr3", "hdr ? o.pipe_hdr : o.pipe", "o.pipe"),
    ("the old pre guard", "temporal", "    // AMD FSR 3.1's too (pre_fsr3))\n",
     "    // AMD FSR 3.1's too (pre_fsr3))\n    if (f3 && scene->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32) return nullptr;\n"),
    ("the old pre pass", "temporal", "wide ? s.out_hdr : s.out);\n    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out_layout, 0, 2, w);\n    vkCmdPushConstants(cmd, s.out_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof opc, opc);\n    vkCmdDispatch(cmd, (rw + 7)",
     "s.out);\n    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out_layout, 0, 2, w);\n    vkCmdPushConstants(cmd, s.out_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof opc, opc);\n    vkCmdDispatch(cmd, (rw + 7)"),
]


def main():
    texts = {"fsr3": (VK / "vk_fsr3.cpp").read_text(encoding="utf-8"),
             "temporal": (VK / "vk_temporal.cpp").read_text(encoding="utf-8"),
             "out": (VK / "shaders" / "dlss_out.comp").read_text(encoding="utf-8"),
             "prep": (VK / "shaders" / "fsr3_prep.comp").read_text(encoding="utf-8")}
    checks(texts["fsr3"], texts["temporal"], texts["out"], texts["prep"])
    for name, which, new, old in MUTATIONS:
        check(new in texts[which], "the mutation '%s' has nothing to change" % name)
        t = dict(texts)
        t[which] = t[which].replace(new, old, 1)
        try:
            checks(t["fsr3"], t["temporal"], t["out"], t["prep"])
        except SystemExit:
            continue
        raise SystemExit("FAIL: the mutation '%s' passed" % name)
    print("PASS: AMD FSR 3.1 draws in an RGBA16F run, at the upscale's dispatch and before the bloom; the old lines fail")


if __name__ == "__main__":
    main()
