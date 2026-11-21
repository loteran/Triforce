"""(Triforce, 11-18) EstebanGGM's RX 580 fixes, ported from NX2navi, as this branch takes them: the source's rules.

- vk_main.cpp: alpha to coverage leaves the dynamic state only on AMD's older compiler where the driver has no fast-linking
  pipeline libraries (the RX 580's), so fast pipeline link (gpl_wanted, which needs the dynamic state) is never turned off
  by it; RX_VK_DYN_A2C decides outright.
- vk_texture.cpp: on AMD only a cubemap's image is made cube compatible (RX_VK_CUBE_ONLY decides outright).
- vk_shader.cpp: the water/shadow PCF rewrite is outside the translation stamp, on by default only on the same cards.
usage: python -I -B tests/test_amd_older_fixes.py"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def main():
    main_cpp = (VK / "vk_main.cpp").read_text(encoding="utf-8")
    a2c = between(main_cpp, "const bool eds3_a2c = dev.dynamic_state3", "dev.vk12 = f12;")
    if "bool want = !(amd_older && !dev.graphics_pipeline_library);" not in a2c:
        raise SystemExit("alpha to coverage leaves the dynamic state on more than AMD's older compiler without pipeline libraries")
    if 'getenv("RX_VK_DYN_A2C")' not in a2c or "dev.eds3_a2c = eds3_a2c && want;" not in a2c:
        raise SystemExit("RX_VK_DYN_A2C does not decide the dynamic alpha to coverage")
    gpl_at = main_cpp.index("dev.graphics_pipeline_library = dev.graphics_pipeline_library && gpl.graphicsPipelineLibrary;")
    if gpl_at > main_cpp.index("bool want = !(amd_older && !dev.graphics_pipeline_library);"):
        raise SystemExit("the alpha to coverage rule reads the pipeline libraries before they are known")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    if "dev.eds3_a2c &&" not in between(draw, "bool gpl_wanted() {", "\n}\n"):
        raise SystemExit("fast pipeline link no longer needs the dynamic alpha to coverage: revisit the rule above")

    tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    cube = between(tex, 'static const int cube_env = getenv("RX_VK_CUBE_ONLY")', "flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, x.cube = true;")
    if "cube_env >= 0 ? cube_env != 0 : dev.props.vendorID == 0x1002" not in cube:
        raise SystemExit("the cube-only rule is not AMD's by default")
    if "(!cube_only || cube_target)" not in cube or "NVN_TEXTURE_TARGET_CUBEMAP_ARRAY" not in cube:
        raise SystemExit("a non-cube 2D array is still made cube compatible where the rule applies")
    if "if (!tex->cube) return VK_NULL_HANDLE;" not in tex:
        raise SystemExit("a cube view of an image without the flag is no longer refused")

    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    out_at = shader.index("// ---- (Triforce) Outside the translation stamp, to the next mark")
    simplify_at = shader.index("bool spirv_water_simplify(")
    back_in = shader.index("// ---- (Triforce) Inside the translation stamp again", out_at)
    if not out_at < simplify_at < back_in:
        raise SystemExit("the water rewrite is inside the translation stamp: every player's caches would start again")
    if "return dev.fixes.nan_bits && !dev.graphics_pipeline_library ? 1 : 0;" not in between(shader, "int water_quality_mode() {", "\n}\n"):
        raise SystemExit("the water rewrite is on by default on more cards than the alpha to coverage rule")
    mm = between(shader, "VkShaderModule make_module(", "\n}\n")
    if not re.search(r"spirv_water_simplify\(spirv, water_quality_mode\(\), simplified\)", mm):
        raise SystemExit("make_module does not ask the water rewrite")
    print("PASS: the RX 580 fixes' rules (alpha to coverage, cube images, water PCF) as this branch takes them")


if __name__ == "__main__":
    sys.exit(main())
