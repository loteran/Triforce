"""A draw's descriptors through the per-binding reuse: each one takes what the program's draw before left it only while every input
and stamp it was resolved from is the same.

1. gpu/vk/desc_keep.h compiled alone (case header): for each kind (a sampled texture, a user uniform buffer, a stage's constants, a
   read-only storage buffer) every input and every stamp it stands on stops a take when it moves, and no other kind's result is
   taken; another program or count in a slot empties it.
2. vk_draw.cpp's block before build_descriptors (kept_uniform, kept_texture, buf_stamps, buf_keep, keep_check, desc_keep_mode) cut
   out of the source and run against stand-ins for texture_view_of, sampler_for, uniform_range and the generations
   (tests/desc_keep_test.cpp), which count their calls: a texture is looked up again when its slot is registered anew, a sampler
   registration changed, a texture was destroyed or made again (issue 1126), a frame left a texture for later, or the submission
   is a new one, or the draw is the jittered scene where the last was not (its own, biased sampler); a uniform buffer when an
   inline update came, a pool moved or a mirror was made (gpu_bind_stamp), or the recording is another; c1 within its recording; never a failed one, a copy in the recording's stream, or a driver bank. A generation that
   moves while its lookup runs makes the next one look again. RX_VK_DESC_KEEP=check resolves every time and counts and logs one
   that comes out otherwise; =0, RX_VK_NO_DESC_REUSE, RX_VK_SAMPLER_CACHE=0 and RX_VK_LATE_DEFER turn it off.
3. build_descriptors itself (source): only a draw's build keeps (not a dispatch's, a dump's or the temporal upscaler's second
   recording of one); the textures go through kept_texture and the uniform buffers through kept_uniform; a storage buffer is kept
   only when single and read-only and not of a program that picks among pointers, its stamps read before its binding and its
   result kept after; every descriptor kind moves the place counter, so each kept result is its own descriptor's; a new
   recording moves the recording's stamp.
python -I -B tests/test_desc_keep.py --headers <Vulkan include folder> [--cxx g++] [--source <vk_draw.cpp>]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
CASES = ["header", "on", "off", "no-reuse", "sampler-cache-off", "late-defer", "texture", "compute", "texture-moved", "texture-dummy", "texture-dummy-redrawn",
         "texture-scene", "texture-no-sampler", "uniform", "uniform-stream", "uniform-moved", "c1", "storage", "check", "check-bad"]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, why):
    if not cond:
        raise SystemExit(why)


def check_build(source):
    build = block(source, "void build_descriptors(")
    need("desc_keep::Prog* const kp = keep && desc_keep_mode() && !dump_on() && !pre_retarget_on" in build,
         "build_descriptors keeps descriptors outside a draw's own build")
    need("build_descriptors(s, p, true, scene);" in block(source, "void draw(VkCommandBuffer"), "a draw does not keep its descriptors")
    need(source.count("build_descriptors(s, p, true") == 1, "something other than a draw keeps descriptors")
    need(build.count("jn += d.count;") == 5, "a descriptor kind does not move the place counter: kept results would be another's")
    need("kept_uniform(kp, jb + k, s, st, d.index + k)" in build, "the uniform buffers do not go through kept_uniform")
    need("kept_texture(kp, jb + k, handle, vt, d.is_depth" in build, "the sampled textures do not go through kept_texture")
    need("st.vk_stage == VK_SHADER_STAGE_COMPUTE_BIT,\n                                                   scene, &view, &tex);" in build,
         "the sampled textures are not given the draw's scene flag (the upscaler's texture LOD bias)")
    need("sampler_for(" not in build and "texture_view_of(handle, vt, ViewUse::Sample" not in build,
         "a sampled texture is looked up past kept_texture")
    ssbo = build[build.index("for (const auto& d : info.storage_buffers_descriptors)"):build.index("auto texel_views")]
    need("kp && d.count == 1 && !d.is_written && !info.native_global_bounds" in ssbo,
         "a storage buffer other than a single read-only one is kept")
    at = ssbo.index("buf_stamps(desc_keep::kSsbo)")
    need(at < ssbo.index("gpu_buffer_bind("), "a storage buffer's stamps are read after it is bound")
    need(ssbo.index("dw().bufs.push_back(b ?") < ssbo.index("buf_keep(*kp, jb + k, *ke, desc_keep::kSsbo, addr, size, kn, kept_took, (bool)b"),
         "a storage buffer is kept before its result, or without saying whether it was bound")
    begin = block(source, "void draw_begin_recording(")
    need("desc_rec++" in begin, "a new recording keeps the last one's buffers")
    disp = block(source, "void dispatch(VkCommandBuffer")
    need("build_descriptors(s, p);" in disp, "a dispatch's build is not the plain one")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--source", type=Path, default=ROOT / "gpu" / "vk" / "vk_draw.cpp", help="another vk_draw.cpp (a mutation)")
    a = p.parse_args()
    source = a.source.read_text(encoding="utf-8")
    check_build(source)
    keep = source[source.index("// ---- (Triforce) Per-binding descriptor reuse (desc_keep.h)"):source.index("void build_descriptors(")]
    fixture = (ROOT / "tests" / "desc_keep_test.cpp").read_text(encoding="utf-8").replace("/* KEEP */", keep)
    with tempfile.TemporaryDirectory(prefix="desc-keep-") as tmp:
        cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", "-pthread",
                        "-I" + str(a.headers), "-I" + str(ROOT / "gpu" / "vk"), str(cpp), "-o", str(exe)], check=True)
        for case in CASES:
            subprocess.run([str(exe), case], check=True, timeout=20)
    print("PASS: %d cases: a kept descriptor is taken only while what it was resolved from stands" % len(CASES))


if __name__ == "__main__":
    main()
