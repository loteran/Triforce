"""(Triforce) AMD FSR 3.1's history and jitter (gpu/vk/vk_fsr3.cpp, vk_fsr3_inputs.cpp) and the camera cut the upscalers share
(gpu/vk/framegen_rows.h, vk_temporal.cpp's chain). No game, no GPU.

1. Compiled and run: framegen_rows.h camera_cut with the upscalers' rule (NX2navi's 2495391: a move over 5 m, or a turn over
   30 degrees with a move over 0.5 m, per 60th of a second, the time scaled 0.25 to 4 times) and frame generation's (10
   degrees, 1 to 4 times, as before); fsr3_math.h jitter_phases (AMD's ffxFsr3GetJitterPhaseCount: 8 x the width's scale
   squared, truncated, no cap) and plan_images (one set of images for the game's dynamic resolution: kept for any frame
   that fits, made again for another output or a larger frame, at the output's size then and with Auto).
2. The source: FSR 3.1's inputs start a new history after a held present and at a camera cut, not at another render size;
   the chain (DLSS, FSR 4) at a cut too; FSR 3.1's passes are given this frame's render size and the images' as the max,
   the downsampler's dispatch from the frame's size; the exposure snaps after a reset (the previous log-luma at 1e8); the
   jitter is AMD's length and stops after a few failed frames; the images are let go once FSR 3.1 has not been used for a
   while (the chain's temporal_idle calls it); the scenes FSR 3.1's inputs keep a camera of follow the output. The old
   lines fail.

    python -I -B tests/test_fsr3_history.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

CHECK = r'''
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "framegen_rows.h"
#include "fsr3_math.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

using namespace mkwvk;
namespace fgr = mkwvk::framegen_rows;

// a camera at x metres along +x, turned by deg degrees from looking down -z (view rows: right, up, back; t = -R pos)
static fgr::Pose at(float x, float deg) {
    const float a = deg * 0.017453293f, c = std::cos(a), s = std::sin(a);
    const float right[3] = {c, 0, -s}, up[3] = {0, 1, 0}, back[3] = {s, 0, c};
    const float pos[3] = {x, 0, 0};
    float v[12];
    const float* rows[3] = {right, up, back};
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 3; i++) v[r * 4 + i] = rows[r][i];
        v[r * 4 + 3] = -(rows[r][0] * pos[0] + rows[r][1] * pos[1] + rows[r][2] * pos[2]);
    }
    return fgr::pose_of(v);
}

int main() {
    const fgr::CutRule up = fgr::kUpscalerCut, fg = fgr::kFrameGenCut;
    CHECK(up.turn_degrees == 30.0f && up.min_scale == 1.0f && fg.turn_degrees == 10.0f && fg.min_scale == 1.0f);
    // (a quick swing of the orbiting camera: 11 to 18 degrees and 0.9 to 1.4 m a frame. A cut for frame generation as before,
    // none for the upscalers, whose history it would throw away at every frame of the swing)
    CHECK(fgr::camera_cut(at(0, 0), at(1.2f, 15), 16.667f, fg) && fgr::camera_cut(at(0, 0), at(1.2f, 15), 16.667f));
    CHECK(!fgr::camera_cut(at(0, 0), at(1.2f, 15), 16.667f, up));
    CHECK(!fgr::camera_cut(at(0, 0), at(1.4f, 18), 16.667f, up));
    // (a turn over 30 degrees with a move is one; a turn in place is none; an area load is one)
    CHECK(fgr::camera_cut(at(0, 0), at(0.6f, 35), 16.667f, up) && !fgr::camera_cut(at(0, 0), at(0.4f, 90), 16.667f, up));
    CHECK(fgr::camera_cut(at(0, 0), at(5.1f, 0), 16.667f, up) && !fgr::camera_cut(at(0, 0), at(4.9f, 0), 16.667f, up));
    CHECK(!fgr::camera_cut(at(0, 0), at(0.05f, 0.08f), 16.667f, up));   // (play: NX2navi's medians)
    // (the time: a 30 fps frame may move twice as far, four times at the most; never less than one 60th, as frame
    // generation's rule: the time is the CPU's between two recordings, and frames recorded back to back after a stall are
    // a few ms apart)
    CHECK(!fgr::camera_cut(at(0, 0), at(9.0f, 0), 33.333f, up) && fgr::camera_cut(at(0, 0), at(11.0f, 0), 33.333f, up));
    CHECK(fgr::camera_cut(at(0, 0), at(21.0f, 0), 500.0f, up) && !fgr::camera_cut(at(0, 0), at(19.0f, 0), 500.0f, up));
    static const float burst[] = {0.0f, 2.0f, 4.1667f, 8.0f, 16.667f};
    for (float ms : burst) {
        // ((Triforce) a swing recorded 2 ms after the frame before (the replay catching up) is still a swing, no cut; nor a
        // 1 m move, nor a 7.5 degree turn with 0.125 m; an area load still is one
        CHECK(!fgr::camera_cut(at(0, 0), at(1.2f, 15), ms, up) && !fgr::camera_cut(at(0, 0), at(1.4f, 18), ms, up));
        CHECK(!fgr::camera_cut(at(0, 0), at(1.0f, 0), ms, up) && !fgr::camera_cut(at(0, 0), at(2.0f, 0), ms, up));
        CHECK(!fgr::camera_cut(at(0, 0), at(0.2f, 8), ms, up));
        CHECK(fgr::camera_cut(at(0, 0), at(5.1f, 0), ms, up) && fgr::camera_cut(at(0, 0), at(0.6f, 35), ms, up));
    }
    fgr::Pose none;
    CHECK(fgr::camera_cut(none, at(0, 0), 16.667f, up) && fgr::camera_cut(at(0, 0), at(NAN, 0), 16.667f, up));

    // AMD's jitter length: by the width, truncated, no cap
    CHECK(fsr3_math::jitter_phases(1280, 1920) == 18);    // (Quality, 1.5x)
    CHECK(fsr3_math::jitter_phases(960, 1920) == 32);     // (Performance)
    CHECK(fsr3_math::jitter_phases(640, 1920) == 72);     // (Ultra Performance, 3x: NVIDIA's rule stops at 64)
    CHECK(fsr3_math::jitter_phases(683, 2048) == 71);     // (the game's 4/12 of 2048)
    CHECK(fsr3_math::jitter_phases(1920, 1920) == 8);     // (Native AA)
    CHECK(fsr3_math::jitter_phases(0, 1920) == 8 && fsr3_math::jitter_phases(1920, 0) == 8);

    // one set of images for the dynamic resolution
    using fsr3_math::plan_images;
    auto p = plan_images(0, 0, 0, 0, 1280, 720, 1920, 1080, false);   // (a fixed share: its own size)
    CHECK(p.remake && p.width == 1280 && p.height == 720);
    p = plan_images(1280, 720, 1920, 1080, 1280, 720, 1920, 1080, false);
    CHECK(!p.remake && p.width == 1280 && p.height == 720);
    p = plan_images(1280, 720, 1920, 1080, 1104, 621, 1920, 1080, false);   // (a step down: kept, the history goes on)
    CHECK(!p.remake && p.width == 1280 && p.height == 720);
    p = plan_images(1280, 720, 1920, 1080, 1360, 765, 1920, 1080, false);   // (a step up: once, at the output's size)
    CHECK(p.remake && p.width == 1920 && p.height == 1080);
    p = plan_images(1920, 1080, 1920, 1080, 1700, 956, 1920, 1080, false);
    CHECK(!p.remake);
    p = plan_images(0, 0, 0, 0, 1280, 720, 1920, 1080, true);   // (Auto: at the output's size from the start)
    CHECK(p.remake && p.width == 1920 && p.height == 1080);
    p = plan_images(1920, 1080, 1920, 1080, 853, 480, 2560, 1440, false);   // (another output: made again)
    CHECK(p.remake && p.width == 853 && p.height == 480);
    p = plan_images(0, 0, 0, 0, 1920, 1080, 1920, 1080, true);   // (Native AA)
    CHECK(p.remake && p.width == 1920 && p.height == 1080);
    p = plan_images(0, 0, 0, 0, 2000, 1100, 1920, 1080, true);   // (never smaller than the frame)
    CHECK(p.remake && p.width == 2000 && p.height == 1100);
    std::puts("ok");
    return 0;
}
'''


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def function(source, head, last=False):
    """A definition from its first line to its closing brace at the start of a line (last: the last one so named, past a
    declaration ahead of it)"""
    start = source.rindex(head) if last else source.index(head)
    return source[start:source.index("\n}\n", start) + 3]


def checks(fsr3, inputs, temporal):
    prep = function(inputs, "bool prepare(VkCommandBuffer cmd, Texture* in, VkImageView in_view, Frame& out, bool linear) {")
    # FSR3-04: after a held present and at a camera cut (the upscalers' rule), and no more at another render size (FSR3-02)
    check("const bool held = !gap && present_held_last();" in prep, "FSR 3.1's inputs start no new history after a held present")
    check("framegen_rows::camera_cut(s.last_pose, pose, dt, framegen_rows::kUpscalerCut)" in prep,
          "FSR 3.1's inputs have no camera cut, or not with the upscalers' 30-degree rule")
    check("const bool reset = gap || held || cut;" in prep, "FSR 3.1's inputs' reset is not the gap, the hold and the cut")
    check("s.frame.width != rw" not in prep, "FSR 3.1's inputs still start a new history at another render size")
    check(prep.index("const bool reset = gap || held || cut;") < prep.index("const bool motion = !reset &&"),
          "the reset is decided after the motion it turns off")
    chain = function(temporal, "bool chain(VkCommandBuffer cmd, int mode, Engine& engine, const Camera& cam,", last=True)
    check("framegen_rows::camera_cut(s.last_pose, pose, dt, framegen_rows::kUpscalerCut)" in chain and
          "const bool reset = !last || now - last > 2 || present_held_last() || cut;" in chain,
          "the chain (DLSS, FSR 4) starts no new history at a camera cut")
    # FSR3-02: the frame's render size into the constants and the downsampler, the images' size as the max
    up = function(fsr3, "bool upscale(VkCommandBuffer cmd, const Inputs& in) {")
    check("k.max_render_size[0] = (int32_t)g.render_width" in up and "std::clamp(in.render_width, 1u, g.render_width)" in up,
          "FSR 3.1 is not given the frame's render size within the images' (maxRenderSize)")
    check("k.max_render_size[0] = rw" not in up, "FSR 3.1's max render size is still the frame's")
    check("const uint32_t spd_x = ((uint32_t)rw - 1) / 64 + 1;" in up and "spd.render_size[0] = (uint32_t)rw" in up,
          "the downsampler is not dispatched over the frame's render size")
    pre = function(fsr3, "bool prepare(uint32_t render_width, uint32_t render_height, uint32_t upscale_width, uint32_t upscale_height, bool dynamic) {")
    check("fsr3_math::plan_images(" in pre and "if (!plan.remake) return true;" in pre,
          "FSR 3.1's images are not kept across the dynamic resolution's steps (plan_images)")
    # FSR3-08: the exposure snaps after a reset
    check("info.float32[0] = -1.0f, info.float32[1] = 1e8f;" in up, "FSR 3.1's exposure eases in after a reset (log-luma 1)")
    # FSR3-11 and FSR3-10: AMD's length, and no jitter once FSR 3.1 failed for a few frames
    done = function(inputs, "void upscaled(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh) {")
    check("fsr3_math::jitter_phases(rw, dw)" in done and "dlss_jitter_phases" not in done,
          "FSR 3.1's jitter length is not AMD's")
    check("s.missed_from.store(0" in done, "a frame FSR 3.1 drew does not end a run of failures")
    jitter = function(inputs, "uint32_t jitter_size(int axis) {")
    check("now - miss > kMissedFrames) return 0;" in jitter, "the jitter goes on while FSR 3.1 fails to draw")
    fail = function(fsr3, "bool fail(const char* why, bool real = true) {")
    check("if (real) fsr3_inputs::missed();" in fail, "FSR 3.1's failures do not stop the jitter")
    check('fail("its inputs could not be made", false)' in fsr3, "a frame with no scene camera counts as a failure")
    in_fail = function(inputs, "bool fail(const char* why) {")
    check("if (why != kNoCamera) missed();" in in_fail, "the inputs' failures (but no camera) do not stop the jitter")
    # FSR3-09: let go once not used for a while, from the present's idle tick
    idle = function(fsr3, "bool fsr3_idle() {")
    check("if (!fsr3_wanted() && now - last > 120) {" in idle and "fsr3::release();" in idle and
          "fsr3_inputs::release_idle();" in idle, "FSR 3.1's images are never let go")
    check("if (fsr3_idle()) {" in function(temporal, "void temporal_idle() {"), "temporal_idle does not let FSR 3.1's images go")
    rel = function(inputs, "void release_idle() {")
    check("if (now - s.used_frame <= 120) return;" in rel and "later([old] {" in rel, "FSR 3.1's inputs' images are never let go")
    # FSR3-14: the scene's camera for a scene 1 to 3 times smaller than the output, 640x360 as before
    note = function(inputs, "void note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,")
    check("!scene_size(width, height)" in note and "width < 640" not in note,
          "FSR 3.1's inputs still keep no camera for a scene under 640x360")
    size = function(inputs, "bool scene_size(uint32_t width, uint32_t height) {")
    check("if (width >= 640 && height >= 360) return true;" in size and "dlss_checks::scene_size_fits(width, height, ow, oh)" in size,
          "the scene size rule is not 640x360 or the chain's")
    check("fsr3_inputs::note_output(" in function(fsr3, "bool fsr3_upscale("), "the output's size is never noted for the inputs")


MUTATIONS = [
    ("the old reset", "inputs", "const bool reset = gap || held || cut;", "const bool reset = gap || cut;"),
    ("the old cut rule", "inputs", "framegen_rows::camera_cut(s.last_pose, pose, dt, framegen_rows::kUpscalerCut)",
     "framegen_rows::camera_cut(s.last_pose, pose, dt)"),
    ("the old chain reset", "temporal", "const bool reset = !last || now - last > 2 || present_held_last() || cut;",
     "const bool reset = !last || now - last > 2 || present_held_last();"),
    ("the old max render size", "fsr3", "k.max_render_size[0] = (int32_t)g.render_width", "k.max_render_size[0] = rw"),
    ("the old exposure clear", "fsr3", "info.float32[0] = -1.0f, info.float32[1] = 1e8f;",
     "info.float32[0] = -1.0f, info.float32[1] = 1.0f;"),
    ("the old jitter length", "inputs", "fsr3_math::jitter_phases(rw, dw)", "(uint32_t)dlss_jitter_phases(rw, rh, dw, dh)"),
    ("the old floor", "inputs", "!scene_size(width, height)", "width < 640 || height < 360"),
    ("no release", "fsr3", "fsr3::release();", "(void)0;"),
]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    texts = {"fsr3": (VK / "vk_fsr3.cpp").read_text(encoding="utf-8"),
             "inputs": (VK / "vk_fsr3_inputs.cpp").read_text(encoding="utf-8"),
             "temporal": (VK / "vk_temporal.cpp").read_text(encoding="utf-8")}
    checks(texts["fsr3"], texts["inputs"], texts["temporal"])
    for name, which, new, old in MUTATIONS:
        check(new in texts[which], "the mutation '%s' has nothing to change" % name)
        t = dict(texts)
        t[which] = t[which].replace(new, old)
        try:
            checks(t["fsr3"], t["inputs"], t["temporal"])
        except SystemExit:
            continue
        raise SystemExit("FAIL: the mutation '%s' passed" % name)
    with tempfile.TemporaryDirectory() as tmp:
        cpp, exe = Path(tmp) / "fsr3_history.cpp", Path(tmp) / ("fsr3_history" + (".exe" if os.name == "nt" else ""))
        cpp.write_text(CHECK, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(VK), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)
    print("PASS: FSR 3.1's history (held presents, camera cuts, the dynamic resolution, the exposure), its jitter and its "
          "images' release; the upscalers' camera cut; the old lines fail")


if __name__ == "__main__":
    main()
