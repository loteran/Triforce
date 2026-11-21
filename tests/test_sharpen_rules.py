"""(Triforce) The Sharpness row before the HUD, the window pass's filter, the game's SMAA under a temporal upscaler and the
texture LOD bias (gpu/vk/sharpen_rules.h; vk_fsr3.cpp, vk_fsr4.cpp, vk_temporal.cpp, vk_framegen.cpp, vk_draw.cpp). No game,
no GPU.

1. Compiled and run: sharpen_rules.h. The slider as the upscalers' sharpness; FSR 3.1's boost below 3/4 of the output's
   width (NX2navi 385734d: none at 3/4, all at 1/2; 0 stays 0); RCAS's stops as AMD's FSR 2 / 3.1 hosts remap them (1 is 0
   stops, 0 is 2) and cbRCAS as ffx_fsr1.h FsrRcasCon writes it; the window pass sharpens nothing when the upscaler did;
   AMD FSR 1 only where it enlarges (else Catmull-Rom, or the plain copy at 1:1); the window pass's three shaders with their
   rgba8 storage images declared rgba16f for HDR Native (the real SPIR-V headers, one image each, nothing else changed);
   the texture LOD bias (the jitter's share -0.5: DLAA -0.5, Quality -1.08 or -1 in quarters, Performance -1.5, Ultra
   Performance -2.08; in quarters, moved only a fifth of a level away; back to 0 at once), the samplers that take it, the
   draws that are the scene (their smallest colour target at its level, or a depth-only draw's depth target, at the
   last upscale's render size or at this frame's scene camera's, so that a step of the dynamic resolution is the scene at
   once) and RX_VK_UPSCALE_LOD_BIAS (unset, 0, a negative number; a fixed one as it is asked).
2. The SPIR-V of FSR 3.1's RCAS pass and of its sharpening accumulate pass: the bindings vk_fsr3.cpp pushes, and RCAS's
   64 x 1 groups of 16 x 16 pixels.
3. The source: FSR 3.1 runs accumulate_sharpen and RCAS at its own sharpness (both placements); FSR 4's sharpening is the
   row's; DLSS's picture goes through RCAS before the HUD, into the output and the picture before the bloom; the window
   pass's Sharpness is none when the upscaler sharpened, FSR 1 skips RCAS at 0 and runs only where it enlarges and with no
   ultrawide sides, and HDR Native's picture goes through RGBA16F images; the bias is in the sampler's key and follows the
   upscaler each present, only in the draws into the scene (the flag from the draw's targets through build_descriptors,
   kept_texture's key and sampler_for's table to sampler_make; never a dispatch); the Sharpness row's text and default.
   The old lines fail.

    python -I -B tests/test_sharpen_rules.py [--cxx g++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

CHECK = r'''
#include <cstdio>
#include <cstring>
#include <vector>
#include "sharpen_rules.h"
#include "shaders/fg_scale_comp_spv.h"
#include "shaders/fsr1_easu_comp_spv.h"
#include "shaders/fsr1_rcas_comp_spv.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

using namespace mkwvk::sharpen_rules;

static bool close_to(float a, float b) { return a - b < 1e-3f && b - a < 1e-3f; }
static float as_float(uint32_t u) { float f; std::memcpy(&f, &u, sizeof f); return f; }

/* the OpTypeImage formats of a module's storage images (Sampled 2) */
static std::vector<uint32_t> storage_formats(const std::vector<uint32_t>& w) {
    std::vector<uint32_t> out;
    for (size_t i = 5; i < w.size();) {
        const uint32_t op = w[i] & 0xffffu, n = w[i] >> 16;
        if (op == 25 && n >= 9 && w[i + 7] == 2) out.push_back(w[i + 8]);
        i += n ? n : 1;
    }
    return out;
}
static int patched_ok(const uint32_t* code, size_t bytes) {
    std::vector<uint32_t> w(code, code + bytes / 4), before = w;
    if (storage_formats(w) != std::vector<uint32_t>{kSpvRgba8}) return 0;
    if (storage_format_patch(w, kSpvRgba8, kSpvRgba16f) != 1) return 0;
    if (storage_formats(w) != std::vector<uint32_t>{kSpvRgba16f}) return 0;
    size_t changed = 0;
    for (size_t i = 0; i < w.size(); i++) changed += w[i] != before[i];
    return changed == 1 && w.size() == before.size();
}

int main() {
    // the slider as the upscalers' sharpness
    CHECK(amount(0) == 0.0f && amount(50) == 0.5f && amount(100) == 1.0f && amount(-5) == 0.0f && amount(250) == 1.0f);

    // FSR 3.1's boost (NX2navi 385734d): none at 3/4 of the width and above, all of it at 1/2 and below
    CHECK(fsr3_boost(2560, 2560) == 0.0f && fsr3_boost(1920, 2560) == 0.0f && fsr3_boost(2200, 2560) == 0.0f);
    CHECK(fsr3_boost(1280, 2560) == 1.0f && fsr3_boost(853, 2560) == 1.0f && fsr3_boost(0, 2560) == 0.0f);
    CHECK(close_to(fsr3_boost(1600, 2560), 0.5f));   // (5/8: halfway)
    // Balanced (1/1.7 of the width): about 0.65 of the way
    CHECK(fsr3_boost(1506, 2560) > 0.6f && fsr3_boost(1506, 2560) < 0.7f);
    CHECK(fsr3_sharpness(0.0f, 853, 2560) == 0.0f);                       // (0 stays off)
    CHECK(close_to(fsr3_boost(1707, 2560), 0.333f) && close_to(fsr3_sharpness(0.5f, 1707, 2560), 0.667f));   // (Quality)
    CHECK(close_to(fsr3_sharpness(0.5f, 1280, 2560), 1.0f) && close_to(fsr3_sharpness(0.5f, 853, 2560), 1.0f));
    CHECK(close_to(fsr3_sharpness(0.5f, 1600, 2560), 0.75f));
    CHECK(close_to(fsr3_sharpness(0.5f, 853, 2560, false), 0.5f));          // (RX_VK_FSR3_BOOST=0)
    CHECK(close_to(fsr3_sharpness(0.5f, 2560, 2560), 0.5f));                  // (Native AA)

    // RCAS: 1 the most (0 stops), 0 two stops below it, as the SDK's hosts remap (-2 s + 2); FsrRcasCon's words
    CHECK(rcas_stops(1.0f) == 0.0f && rcas_stops(0.0f) == 2.0f && close_to(rcas_stops(0.5f), 1.0f) && rcas_stops(2.0f) == 0.0f);
    uint32_t con[4];
    rcas_config(1.0f, con);
    CHECK(as_float(con[0]) == 1.0f && con[1] == 0x3c003c00u && con[2] == 0 && con[3] == 0);
    rcas_config(0.0f, con);
    CHECK(as_float(con[0]) == 0.25f && con[1] == 0x34003400u);
    rcas_config(0.5f, con);
    CHECK(as_float(con[0]) == 0.5f && con[1] == 0x38003800u);
    CHECK(half_bits(0.75f) == 0x3a00u && half_bits(0.0f) == 0);

    // the window pass: nothing when the upscaler sharpened the 3D before the HUD
    CHECK(window_sharpness(50, true) == 0 && window_sharpness(50, false) == 50 && window_sharpness(0, false) == 0);
    CHECK(window_sharpness(120, false) == 100);
    // ((Triforce) AMD's RCAS alone over DLSS's picture: the tone mapped one only, never linear HDR light (before the bloom,
    // RenoDX's RGBA16F scene), whose values RCAS's limiter, made for a peak of 1, rings or leaves soft
    CHECK(lone_rcas(false, false) && !lone_rcas(true, false) && !lone_rcas(false, true) && !lone_rcas(true, true));
    for (int pre = 0; pre < 2; pre++)   // (and then the window pass sharpens, as for the game's own upscaler)
        for (int hdr = 0; hdr < 2; hdr++)
            CHECK(window_sharpness(50, lone_rcas(pre != 0, hdr != 0)) == (pre || hdr ? 50 : 0));

    // AMD FSR 1 where it enlarges on both axes; else Catmull-Rom, or the plain copy at 1:1
    CHECK(fsr1_enlarges(1920, 1080, 2560, 1440) && fsr1_enlarges(2560, 1080, 2560, 1440));
    CHECK(!fsr1_enlarges(2560, 1440, 2560, 1440) && !fsr1_enlarges(2560, 1440, 1920, 1080));
    CHECK(!fsr1_enlarges(2560, 1440, 3440, 1080));   // (wider but shorter: a shrink on one axis)
    CHECK(window_mode(3, true, 1920, 1080, 2560, 1440) == 3);
    CHECK(window_mode(3, false, 2560, 1440, 2560, 1440) == 0 && window_mode(3, false, 2560, 1440, 1920, 1080) == 1);
    CHECK(window_mode(2, false, 1920, 1080, 2560, 1440) == 2 && window_mode(0, false, 1, 1, 2, 2) == 0);

    // HDR Native: the window pass's three shaders, their one storage image rgba16f, nothing else changed
    CHECK(patched_ok(FG_SCALE_COMP_SPV, sizeof FG_SCALE_COMP_SPV));
    CHECK(patched_ok(FSR1_EASU_COMP_SPV, sizeof FSR1_EASU_COMP_SPV));
    CHECK(patched_ok(FSR1_RCAS_COMP_SPV, sizeof FSR1_RCAS_COMP_SPV));
    std::vector<uint32_t> junk{1, 2, 3, 4, 5, 6};
    CHECK(storage_format_patch(junk, kSpvRgba8, kSpvRgba16f) == 0);
    std::vector<uint32_t> cut(FG_SCALE_COMP_SPV, FG_SCALE_COMP_SPV + 40);   // (cut short: an instruction past the end)
    cut[5] = (200u << 16) | 25u;
    CHECK(storage_format_patch(cut, kSpvRgba8, kSpvRgba16f) == 0);

    // the texture LOD bias: log2(render / output) and -0.5 for the jitter (half the guides' -1: the lab's shimmer)
    CHECK(kLodBiasJitter == -0.5f);
    CHECK(close_to(lod_bias(2560, 2560, true), -0.5f) && close_to(lod_bias(1707, 2560, true), -1.085f));
    CHECK(close_to(lod_bias(1280, 2560, true), -1.5f) && close_to(lod_bias(853, 2560, true), -2.085f));
    CHECK(close_to(lod_bias(1708, 3416, true), -1.5f));   // (the player's FSR 4 Performance: -2.00 before)
    CHECK(close_to(lod_bias(1280, 2560, false), -1.0f) && lod_bias(2560, 2560, false) == 0.0f && lod_bias(0, 2560, true) == 0.0f);
    CHECK(close_to(lod_bias(1707, 2560, false), -0.585f));   // (no jitter: the ratio's alone)
    CHECK(lod_bias(3000, 2560, false) == 0.0f);   // (never above 0)
    // after the rounding: Performance 2x -1.5, Quality 1.5x -1, native with jitter -0.5, Ultra Performance 3x -2
    CHECK(lod_bias_next(0.0f, lod_bias(1280, 2560, true)) == -1.5f && lod_bias_next(0.0f, lod_bias(1707, 2560, true)) == -1.0f);
    CHECK(lod_bias_next(0.0f, lod_bias(2560, 2560, true)) == -0.5f && lod_bias_next(0.0f, lod_bias(853, 2560, true)) == -2.0f);
    CHECK(lod_bias_next(0.0f, lod_bias(1280, 2560, false)) == -1.0f && lod_bias_next(0.0f, lod_bias(2560, 2560, false)) == 0.0f);
    // in quarters, and moved only a fifth of a level away: a dynamic resolution to and fro across a step makes nothing anew
    CHECK(lod_bias_next(0.0f, -1.585f) == -1.5f && lod_bias_next(0.0f, -2.585f) == -2.5f && lod_bias_next(0.0f, -1.0f) == -1.0f);
    CHECK(lod_bias_next(-1.5f, -1.63f) == -1.5f && lod_bias_next(-1.5f, -1.38f) == -1.5f);
    CHECK(lod_bias_next(-1.5f, -1.75f) == -1.75f && lod_bias_next(-1.5f, -2.0f) == -2.0f);
    CHECK(lod_bias_next(-2.5f, 0.0f) == 0.0f && lod_bias_next(0.0f, -0.1f) == 0.0f && lod_bias_next(0.0f, 0.0f) == 0.0f);
    float b = 0.0f;
    for (int i = 0; i < 100; i++) b = lod_bias_next(b, lod_bias(i & 1 ? 1689 : 1631, 2560, true));
    CHECK(b == -1.25f);   // (-1.15 and -1.10: either side of a step, the first one kept)
    // the scene: the targets at the render size, width and height; none without a render size
    CHECK(lod_scene(1708, 960, 1708, 960) && !lod_scene(1600, 1600, 1708, 960) && !lod_scene(854, 480, 1708, 960));
    CHECK(!lod_scene(3416, 1920, 1708, 960) && !lod_scene(1708, 1708, 1708, 960) && !lod_scene(1708, 960, 0, 0));
    CHECK(!lod_scene(0, 0, 0, 0) && !lod_scene(1708, 960, 1708, 0));
    // the size a draw is judged by: the smallest colour target at its level, else the depth target's at its level
    uint32_t sw = 7, sh = 7;
    const LodTarget none{};
    const LodTarget mrt[2]{{1708, 960, 0}, {854, 480, 0}};   // (one smaller: the smallest decides)
    lod_scene_size(mrt, 2, LodTarget{1708, 960, 0}, &sw, &sh);
    CHECK(sw == 854 && sh == 480 && !lod_scene(sw, sh, 1708, 960));
    const LodTarget scene_rt[1]{{1708, 960, 0}};
    lod_scene_size(scene_rt, 1, LodTarget{1708, 960, 0}, &sw, &sh);
    CHECK(sw == 1708 && sh == 960 && lod_scene(sw, sh, 1708, 960));
    lod_scene_size(nullptr, 0, LodTarget{1600, 1600, 0}, &sw, &sh);   // (the sun's cascade, depth only)
    CHECK(sw == 1600 && sh == 1600 && !lod_scene(sw, sh, 1708, 960));
    lod_scene_size(nullptr, 0, LodTarget{1708, 960, 0}, &sw, &sh);    // (a depth or alpha prepass of the scene)
    CHECK(sw == 1708 && sh == 960 && lod_scene(sw, sh, 1708, 960));
    const LodTarget mip1[1]{{1708, 960, 1}};   // (level 1 of a render-size texture: half the size, not the scene)
    lod_scene_size(mip1, 1, none, &sw, &sh);
    CHECK(sw == 854 && sh == 480 && !lod_scene(sw, sh, 1708, 960));
    const LodTarget empty[2]{{0, 0, 0}, {1708, 960, 0}};   // (an unbound slot is no target)
    lod_scene_size(empty, 2, LodTarget{1600, 1600, 0}, &sw, &sh);
    CHECK(sw == 1708 && sh == 960);
    lod_scene_size(nullptr, 0, none, &sw, &sh);
    CHECK(sw == 0 && sh == 0 && !lod_scene(sw, sh, 1708, 960));
    const LodTarget deep[1]{{1708, 960, 40}};   // (a level past the size: 1 x 1, no shift past the width)
    lod_scene_size(deep, 1, none, &sw, &sh);
    CHECK(sw == 1 && sh == 1);
    // this frame's scene: the last upscale's size (the frame before's) or the size the scene camera was noted at this frame
    // or the one before. The game's dynamic resolution makes new targets at each size: a step is the scene at once
    CHECK(lod_scene_now(1708, 960, 1708, 960, 0, 100));                                  // (steady: the last upscale's)
    CHECK(!lod_scene_now(1600, 960, 1708, 960, 0, 100));                                 // (a step, before the camera's note)
    CHECK(lod_scene_now(1600, 900, 1708, 960, lod_scene_pack(1600, 900, 100), 100));     // (after it: the scene)
    CHECK(lod_scene_now(1600, 900, 1708, 960, lod_scene_pack(1600, 900, 99), 100));      // (noted the frame before)
    CHECK(!lod_scene_now(1600, 900, 1708, 960, lod_scene_pack(1600, 900, 97), 100));     // (an old note)
    CHECK(!lod_scene_now(1600, 1600, 1708, 960, lod_scene_pack(1600, 900, 100), 100));   // (a cascade, either way)
    CHECK(!lod_scene_now(800, 450, 1708, 960, lod_scene_pack(1600, 900, 100), 100));     // (half size, either way)
    CHECK(!lod_scene_now(1600, 900, 0, 0, 0, 0) && !lod_scene_now(0, 0, 0, 0, lod_scene_pack(0, 0, 5), 5));
    CHECK(lod_scene_now(1600, 900, 0, 0, lod_scene_pack(1600, 900, 0x100000005ull), 0x100000005ull));   // (the frame's low bits)
    // to and fro every frame between two sizes: the upscale's note is always the other one; the camera's is this frame's
    uint32_t last_w = 0, last_h = 0;
    int biased = 0;
    for (uint64_t f = 1; f <= 20; f++) {
        const uint32_t w = f & 1 ? 1708 : 1600, h = f & 1 ? 960 : 900;
        const uint64_t noted = lod_scene_pack(w, h, f);   // (the scene's camera, in stream order, before its draws record)
        biased += lod_scene_now(w, h, last_w, last_h, noted, f);
        CHECK(!lod_scene(w, h, last_w, last_h));   // (by the last upscale alone: never)
        last_w = w, last_h = h;   // (the upscale's note at the frame's end)
    }
    CHECK(biased == 20);
    // the bias a frame wants and the next one: the computed one in quarters, a fixed one as asked once it jitters, off none
    CHECK(lod_bias_want(LodBiasSwitch::Computed, 0.0f, 1280, 2560, true) == lod_bias(1280, 2560, true));
    CHECK(lod_bias_want(LodBiasSwitch::Off, 0.0f, 1280, 2560, true) == 0.0f);
    CHECK(lod_bias_want(LodBiasSwitch::Fixed, -1.3f, 1280, 2560, true) == -1.3f);
    CHECK(lod_bias_want(LodBiasSwitch::Fixed, -1.3f, 1280, 2560, false) == 0.0f);   // (no jitter yet: none)
    CHECK(lod_bias_want(LodBiasSwitch::Fixed, -1.3f, 0, 2560, true) == 0.0f);       // (no upscale yet: none)
    CHECK(lod_bias_step(LodBiasSwitch::Fixed, 0.0f, -1.3f) == -1.3f);               // (not rounded, no hysteresis)
    CHECK(lod_bias_step(LodBiasSwitch::Fixed, -1.25f, -1.3f) == -1.3f);
    CHECK(lod_bias_step(LodBiasSwitch::Computed, 0.0f, -1.3f) == -1.25f && lod_bias_step(LodBiasSwitch::Computed, -1.25f, -1.3f) == -1.25f);
    // RX_VK_UPSCALE_LOD_BIAS: unset computed; 0 or no number off; a negative number that bias, -3 at most
    float fx = 1.0f;
    CHECK(lod_bias_switch(nullptr, &fx) == LodBiasSwitch::Computed && fx == 0.0f);
    CHECK(lod_bias_switch("0", &fx) == LodBiasSwitch::Off && lod_bias_switch("off", &fx) == LodBiasSwitch::Off);
    CHECK(lod_bias_switch("", &fx) == LodBiasSwitch::Off && lod_bias_switch("-0", &fx) == LodBiasSwitch::Off);
    CHECK(lod_bias_switch("1", &fx) == LodBiasSwitch::Computed);   // (as before: not 0, on)
    CHECK(lod_bias_switch("-1", &fx) == LodBiasSwitch::Fixed && fx == -1.0f);
    CHECK(lod_bias_switch("-1.5", &fx) == LodBiasSwitch::Fixed && fx == -1.5f);
    CHECK(lod_bias_switch("-0.25", &fx) == LodBiasSwitch::Fixed && fx == -0.25f);
    CHECK(lod_bias_switch("-7", &fx) == LodBiasSwitch::Fixed && fx == -3.0f);   // (clamped)
    const float held = b;
    for (int i = 0; i < 100; i++) CHECK((b = lod_bias_next(b, lod_bias(i & 1 ? 1689 : 1631, 2560, true))) == held);
    // which samplers: linear within and between levels, wrapping on one axis, no compare, no reduction
    CHECK(lod_biased(true, true, false, false, false) && lod_biased(true, false, true, false, false));
    CHECK(!lod_biased(false, true, true, false, false) && !lod_biased(true, false, false, false, false));
    CHECK(!lod_biased(true, true, true, true, false) && !lod_biased(true, true, true, false, true));
    std::puts("ok");
    return 0;
}
'''


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def function(source, head, last=False):
    """A definition from its first line to its closing brace at the start of a line"""
    start = source.rindex(head) if last else source.index(head)
    return source[start:source.index("\n}\n", start) + 3]


def spv_words(path):
    text = path.read_text(encoding="utf-8")
    body = text[text.index("{") + 1:text.rindex("}")]
    return [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", body)]


def spv_bindings(words):
    """binding -> 'sampled' / 'storage' / 'uniform' / 'sampler', and the local size"""
    names, binding, var_type, ptr, images, blocks, size = {}, {}, {}, {}, {}, set(), None
    i = 5
    while i < len(words):
        op, n = words[i] & 0xffff, words[i] >> 16
        a = words[i + 1:i + n]
        if op == 71 and a[1] == 33:
            binding[a[0]] = a[2]
        elif op == 71 and a[1] == 2:   # Block
            blocks.add(a[0])
        elif op == 25:
            images[a[0]] = a[6]   # (Sampled)
        elif op == 26:
            images[a[0]] = "sampler"
        elif op == 32:
            ptr[a[0]] = a[2]
        elif op == 59:
            var_type[a[1]] = a[0]
        elif op == 16 and a[1] == 17:
            size = tuple(a[2:5])
        i += max(n, 1)
    out = {}
    for var, b in binding.items():
        t = ptr.get(var_type.get(var))
        kind = images.get(t)
        out[b] = "uniform" if t in blocks else "sampler" if kind == "sampler" else "sampled" if kind == 1 else \
            "storage" if kind == 2 else "?"
    return out, size


def checks(t):
    fsr3, fsr4, temporal, framegen, draw, menu, header = (t[k] for k in ("fsr3", "fsr4", "temporal", "framegen", "draw",
                                                                         "menu", "header"))
    # FSR 3.1's own sharpening, as the SDK runs it
    check('PASS("accumulate_sharpen", FSR3_ACCUMULATE_SHARPEN_COMP_SPV, ACCUMULATE)' in fsr3 and
          'PASS("rcas", FSR3_RCAS_COMP_SPV, RCAS)' in fsr3 and "constexpr Kind RCAS[]{T, T, I, U, U};" in fsr3,
          "vk_fsr3.cpp has no accumulate_sharpen and RCAS passes, or RCAS's bindings are not exposure, input, output, two blocks")
    up = function(fsr3, "bool upscale(VkCommandBuffer cmd, const Inputs& in) {")
    check("const bool sharpen = g.sharpen && in.sharpness > 0.0f;" in up and "sharpen ? P_ACCUMULATE_SHARPEN : P_ACCUMULATE" in up,
          "FSR 3.1 does not take accumulate_sharpen while it sharpens")
    check("run(P_RCAS, {exposure, tex(g.internal_upscaled[q]), tex(g.output), cb_fsr, cb_rcas}, groups(uw, 16), groups(uh, 16));"
          in up and "sharpen_rules::rcas_config(in.sharpness, con);" in up,
          "FSR 3.1's RCAS does not sharpen this frame's history into its output with AMD's constants")
    check(fsr3.count("ai.sharpness = own_sharpness(f.width, ow);") == 2, "FSR 3.1 is not given its sharpness in both placements")
    own = function(fsr3, "float own_sharpness(uint32_t rw, uint32_t ow) {")
    check("sharpen_rules::fsr3_sharpness(temporal_sharpness(), rw, ow, boost)" in own and "RX_VK_FSR3_BOOST" in own,
          "FSR 3.1's sharpness is not the row's with the boost below 3/4")
    check(fsr3.count("upscale_lod_note(f.width, f.height, ow);") == 2, "FSR 3.1 does not note its sizes for the texture LOD bias")
    rcas = function(fsr3, "bool rcas(VkCommandBuffer cmd, VkImageView in, VkImageView out, uint32_t w, uint32_t h, float sharpness) {")
    check("record(cmd, PASSES[P_RCAS], l.pipe, l.point, l.linear, res, 5, (w + 15) / 16, (h + 15) / 16);" in rcas and
          "one.float32[0] = 1.0f;" in rcas, "the RCAS pass alone does not run AMD's pass with an exposure of 1")
    # FSR 4's own sharpening is the row's
    ev = function(fsr4, "bool evaluate(VkCommandBuffer cmd, const TemporalFrame& f) {")
    check("d.enableSharpening = sharpness > 0.0f;" in ev and "d.sharpness = sharpness;" in ev and
          ": temporal_sharpness();" in ev and   # (linux fork: the FSR sharpening row over it when set)
           "enableSharpening = false" not in ev,
          "FSR 4's own sharpening is not the Sharpness row's")
    # DLSS: RCAS over its picture before the HUD, into the output and the picture before the bloom
    chain = function(temporal, "bool chain(VkCommandBuffer cmd, int mode, Engine& engine, const Camera& cam,", last=True)
    check("fsr3::rcas(cmd, s.output.view, s.sharp.view, s.output.width, s.output.height, temporal_sharpness())" in chain and
          "if (ur::is_dlss(mode) && temporal_sharpness() > 0.0f && sharpen_rules::lone_rcas(pre != nullptr, scene_rgba16f) &&"
          in chain and "const bool scene_rgba16f = in_format == VK_FORMAT_R16G16B16A16_SFLOAT;" in chain,
          "DLSS's picture is not sharpened before the HUD, or RCAS alone runs over linear HDR light")
    check("chain_rgba16f.store(scene_rgba16f ? 1 : 0" in chain, "the chain does not say whether its colour was RGBA16F")
    check("s.linear : s.point, picture, VK_IMAGE_LAYOUT_GENERAL}" in chain and "s.pre_picture = {picture," in chain,
          "the sharpened picture is not what goes into the output or before the bloom")
    check("upscale_lod_note(rw, rh, ow);" in chain, "the chain does not note its sizes for the texture LOD bias")
    sharpens = function(temporal, "bool temporal_sharpens_3d() {")
    check("if (fsr3_wanted()) return fsr3::sharpens();" in sharpens and "if (ur::is_fsr4(mode)) return true;" in sharpens and
          "if (ur::is_dlss(mode)) return fsr3::rcas_ok() && sharpen_rules::lone_rcas(pre_wanted(mode), chain_hdr_scene());"
          in sharpens and sharpens.rstrip().endswith("return false;\n}"),
          "temporal_sharpens_3d does not say which upscalers sharpen before the HUD")
    check('getenv("RX_VK_SHARPEN_3D")' in function(temporal, "float temporal_sharpness() {"), "no RX_VK_SHARPEN_3D")
    # the window pass
    scale = function(framegen, "VkImage framegen_scale(")
    check("const int sharp = sharpen_rules::window_sharpness(opts.sharpness, temporal_sharpens_3d());" in scale and
          "opts.sharpness / 100.0f" not in scale and "if (!opts.upscale && !sharp && !wide.side) return VK_NULL_HANDLE;" in scale,
          "the window pass sharpens what the upscaler sharpened before the HUD")
    check("opts.upscale == 3 && scale.easu_img.view && !wide.side && sharpen_rules::fsr1_enlarges(iw, ih, dw, dh)" in scale,
          "AMD FSR 1 runs at 1:1, to a smaller window, or beside ultrawide's sides")
    check("{sharp ? scale.easu_img.view : scale.scaled.view, VK_NULL_HANDLE}" in scale and "if (sharp) {" in scale,
          "AMD FSR 1 still sharpens at Sharpness 0")
    check("sharpen_rules::window_mode(opts.upscale, fsr1, iw, ih, dw, dh)" in scale, "FSR 1's place is not the filter's")
    check("if (pq && !init_scale_pq()) return VK_NULL_HANDLE;" in scale and
          "pq ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM" in scale and
          "VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE" not in scale, "HDR Native's picture still goes through RGBA8 images")
    pq = function(framegen, "bool init_scale_pq() {")
    check("sharpen_rules::storage_format_patch(w, sharpen_rules::kSpvRgba8, sharpen_rules::kSpvRgba16f)" in pq,
          "the window pass's rgba16f shaders are not the rgba8 ones with their storage format changed")
    # the texture LOD bias: in the key, from the upscaler, each present
    make = function(draw, "VkSampler sampler_make(int id, bool compare, bool scene) {", last=True)
    check("p.lod_bias += bias;" in make and make.index("p.lod_bias += bias;") < make.index("uint64_t key = mix(") and
          "sharpen_rules::lod_biased(p.minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR" in make,
          "the texture LOD bias is not added to the material samplers before their key")
    # ((Triforce, S8) only in the draws into the jittered scene: the scene flag from the draw's targets to sampler_make
    check("bias = upscale_lod_bias.load(std::memory_order_relaxed); scene && bias != 0.0f) {" in make,
          "the texture LOD bias is added without the scene flag (shadow cascades and captures biased too)")
    samp = function(draw, "VkSampler sampler_for(int id, bool compare, bool scene) {")
    check("static thread_local Entry table[2][2][4096];" in samp and "Entry& e = table[scene ? 1 : 0][compare ? 1 : 0][id];" in samp
          and samp.count("sampler_make(id, compare, scene)") == 2,
          "sampler_for's table does not keep the scene's samplers apart, or does not pass the flag on")
    kept = function(draw, "VkSampler kept_texture(")
    check("bool compute, bool scene," in kept and "(uint64_t)compare << 40 | (uint64_t)scene << 41;" in kept and
          "const VkSampler smp = sampler_for((int)(handle >> 20), compare, scene);" in kept,
          "kept_texture's key has no scene bit, or its sampler is not the scene's")
    build = function(draw, "void build_descriptors(")
    check("bool keep = false, bool scene = false) {" in build and
          "kept_texture(kp, jb + k, handle, vt, d.is_depth, st.vk_stage == VK_SHADER_STAGE_COMPUTE_BIT,\n"
          "                                                   scene, &view, &tex);" in build,
          "build_descriptors does not carry the scene flag to its textures")
    draw_fn = function(draw, "void draw(VkCommandBuffer")
    check("const bool scene = draw_in_scene(at);" in draw_fn and "build_descriptors(s, p, true, scene);" in draw_fn and
          "if (last_desc_scene != scene) last_desc_valid = false;" in draw_fn and "last_desc_scene = scene;" in draw_fn and
          draw_fn.index("if (last_desc_scene != scene) last_desc_valid = false;") < draw_fn.index("const bool same_gens ="),
          "a draw does not decide the scene from its targets, or keeps descriptors built for the other kind of pass")
    disp = function(draw, "void dispatch(VkCommandBuffer")
    check("build_descriptors(s, p);" in disp and "draw_in_scene" not in disp, "a compute dispatch is biased")
    scene = function(draw, "bool draw_in_scene(const Targets& at) {")
    check("sharpen_rules::lod_scene_now(w, h, upscale_lod_render.load(std::memory_order_relaxed)," in scene and
          "upscale_lod_render_h.load(std::memory_order_relaxed)," in scene and
          "upscale_lod_scene_now.load(std::memory_order_relaxed), texture_frame_no());" in scene and
          "sharpen_rules::lod_scene_size(color, n, depth, &w, &h);" in scene and
          "color[i] = {t->width, t->height, at.color_level[i]};" in scene and
          "depth = {at.depth->width, at.depth->height, at.depth_level};" in scene and "at.width" not in scene,
          "the scene is not the colour targets', or a depth-only draw's depth target's, render size or this frame's")
    note = function(draw, "void upscale_lod_note(uint32_t render_w, uint32_t render_h, uint32_t output_w) {")
    check("upscale_lod_render_h.store(render_h" in note, "the upscaler's render height is not kept")
    now = function(draw, "void upscale_lod_scene(uint32_t width, uint32_t height) {")
    check("upscale_lod_scene_now.store(sharpen_rules::lod_scene_pack(width, height, texture_frame_no())" in now,
          "this frame's scene size is not kept with its frame")
    for k, head in (("temporal", "void temporal_note_scene_draw(const Texture* color0,"),
                    ("inputs", "void note_scene_draw(const Texture* color0,")):
        body = function(t[k], head)
        check("upscale_lod_scene(width, height);" in body and
              body.index("k.valid = true;") < body.index("upscale_lod_scene(width, height);"),
              "%s's scene camera does not tell the texture LOD bias this frame's scene size" % k)
    tick = function(draw, "void upscale_lod_tick() {")
    check("sharpen_rules::lod_bias_step(mode, now, want)" in tick and "sampler_bias_changed();" in tick and
          'sharpen_rules::lod_bias_switch(getenv("RX_VK_UPSCALE_LOD_BIAS"), &fixed)' in tick and
          "want = sharpen_rules::lod_bias_want(mode, fixed, upscale_lod_render.load(" in tick,
          "the bias does not follow the upscaler or RX_VK_UPSCALE_LOD_BIAS, or the samplers are not made anew")
    check("upscale_lod_tick();" in t["replay"], "the bias is not taken at each present")
    check("void sampler_bias_changed() { samp_gen.fetch_add(1, std::memory_order_acq_rel); }" in t["bank"],
          "a change of the bias does not move the samplers' generation")
    # the menu: the row's help and default, no 80% advice left
    row = menu[menu.index('{TAB_GRAPHICS, "Sharpness", &opts.sharpness'):]
    row = row[:row.index("hn_sharpness},")]
    check("before the HUD" in row and "0 is off" in row and "with any filter" not in row, "the Sharpness row's help is the old one")
    hint = function(menu, "const char* hn_sharpness(int v, int variant) {")
    check("temporal_sharpens_3d()" in hint and "80%" not in hint, "the Sharpness hint does not say where it sharpens")
    check("Sharpness to around 80%" not in menu, "the menu still tells DLSS to take Sharpness 80% at the window")
    # ((Triforce) 0 by default: the default Upscaler is the game's own, where any other value sharpens the HUD at the window
    # and costs that pass every present; the launcher's default the same
    check(re.search(r"\bint sharpness = 0;", header), "vk_menu.h: Sharpness is not 0 by default")
    launcher = (ROOT / "recomplauncher.json").read_text(encoding="utf-8")
    entry = launcher[launcher.index('"id": "sharpness"'):]
    entry = entry[:entry.index('"defaults"') + 80]
    check(re.search(r'"defaults": \{\s*"sharpness": "0"\s*\}', entry), "recomplauncher.json: Sharpness's default is not 0")


MUTATIONS = [
    ("no FSR 3.1 sharpening", "fsr3", "sharpen ? P_ACCUMULATE_SHARPEN : P_ACCUMULATE", "P_ACCUMULATE"),
    ("FSR 4 off", "fsr4", "d.enableSharpening = sharpness > 0.0f;", "d.enableSharpening = false;"),
    ("twice", "framegen", "const int sharp = sharpen_rules::window_sharpness(opts.sharpness, temporal_sharpens_3d());",
     "const int sharp = opts.sharpness;"),
    ("FSR 1 anywhere", "framegen", " && !wide.side && sharpen_rules::fsr1_enlarges(iw, ih, dw, dh)", ""),
    ("no bias", "draw", "            p.lod_bias += bias;", "            (void)bias;"),
    ("bias in every pass", "draw", "std::memory_order_relaxed); scene && bias != 0.0f) {", "std::memory_order_relaxed); bias != 0.0f) {"),
    ("one table", "draw", "Entry& e = table[scene ? 1 : 0][compare ? 1 : 0][id];", "Entry& e = table[0][compare ? 1 : 0][id];"),
    ("key without scene", "draw", "(uint64_t)compare << 40 | (uint64_t)scene << 41;", "(uint64_t)compare << 40;"),
    ("scene from width", "draw", "upscale_lod_render_h.load(std::memory_order_relaxed),", "h,"),
    ("scene from the last upscale alone", "draw", "upscale_lod_scene_now.load(std::memory_order_relaxed), texture_frame_no());",
     "0, 0);"),
    ("no frame's scene from FSR 3.1's camera", "inputs",
     "    k.valid = true;\n    upscale_lod_scene(width, height);", "    k.valid = true;"),
    ("scene from the viewport", "draw", "color[i] = {t->width, t->height, at.color_level[i]};",
     "color[i] = {at.width, at.height, 0};"),
    ("descriptors across passes", "draw", "    if (last_desc_scene != scene) last_desc_valid = false;\n", ""),
    ("dispatch biased", "draw", "    build_descriptors(s, p);\n", "    build_descriptors(s, p, false, draw_in_scene(at));\n"),
    ("DLSS unsharpened", "temporal", "s.pre_picture = {picture,", "s.pre_picture = {s.output.view,"),
    ("RCAS over HDR", "temporal", "temporal_sharpness() > 0.0f && sharpen_rules::lone_rcas(pre != nullptr, scene_rgba16f) &&\n",
     "temporal_sharpness() > 0.0f &&\n"),
    ("window off over HDR", "temporal", "return fsr3::rcas_ok() && sharpen_rules::lone_rcas(pre_wanted(mode), chain_hdr_scene());",
     "return fsr3::rcas_ok();"),
]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    texts = {"fsr3": "vk_fsr3.cpp", "fsr4": "vk_fsr4.cpp", "temporal": "vk_temporal.cpp", "framegen": "vk_framegen.cpp",
             "draw": "vk_draw.cpp", "menu": "vk_menu.cpp", "header": "vk_menu.h", "replay": "vk_replay.cpp",
             "bank": "vk_bank.cpp", "inputs": "vk_fsr3_inputs.cpp"}
    texts = {k: (VK / v).read_text(encoding="utf-8") for k, v in texts.items()}
    checks(texts)
    for name, which, new, old in MUTATIONS:
        check(new in texts[which], "the mutation '%s' has nothing to change" % name)
        t = dict(texts)
        t[which] = t[which].replace(new, old)
        try:
            checks(t)
        except SystemExit:
            continue
        raise SystemExit("FAIL: the mutation '%s' passed" % name)

    # FSR 3.1's RCAS and sharpening accumulate passes: the bindings vk_fsr3.cpp pushes them with
    rcas, size = spv_bindings(spv_words(VK / "shaders" / "fsr3_rcas_comp_spv.h"))
    check(rcas == {0: "sampled", 1: "sampled", 2: "storage", 3: "uniform", 4: "uniform", 1000: "sampler", 1001: "sampler"},
          "fsr3_rcas's bindings are not exposure, input, output, cbFSR3Upscaler, cbRCAS and the two samplers: %r" % rcas)
    check(size == (64, 1, 1), "fsr3_rcas's groups are not 64 x 1 (a group 16 x 16 pixels): %r" % (size,))
    plain, _ = spv_bindings(spv_words(VK / "shaders" / "fsr3_accumulate_comp_spv.h"))
    sharp, _ = spv_bindings(spv_words(VK / "shaders" / "fsr3_accumulate_sharpen_comp_spv.h"))
    check(plain == sharp, "accumulate_sharpen's bindings are not accumulate's")
    # AMD's meaning of RCAS's constant, which rcas_stops follows: 0 the most, stops below it
    fsr1 = (ROOT / "third_party/fidelityfx-sdk/include/FidelityFX/gpu/fsr1/ffx_fsr1.h").read_text(encoding="utf-8")
    check("sharpness = exp2(-sharpness);" in fsr1 and "0.0 := maximum" in fsr1, "ffx_fsr1.h's FsrRcasCon is not as rcas_config writes it")

    with tempfile.TemporaryDirectory() as tmp:
        cpp, exe = Path(tmp) / "sharpen_rules.cpp", Path(tmp) / ("sharpen_rules" + (".exe" if os.name == "nt" else ""))
        cpp.write_text(CHECK, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(VK), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)
    print("PASS: Sharpness before the HUD (FSR 3.1's RCAS with its boost, FSR 4's own, RCAS over DLSS), none at the window "
          "then; FSR 1 only where it enlarges and off at 0; HDR Native through RGBA16F; the texture LOD bias under DLSS and FSR")


if __name__ == "__main__":
    main()
