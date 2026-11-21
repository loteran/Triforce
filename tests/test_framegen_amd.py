"""(Triforce) AMD's frame generation made to make frames: what the lab's fgprobe1 run on the owner's RX 7900 XT showed it never
did (AMD's library 3.1.6 taken, no AMD-made frame: the scene's depth and motion were never made where the gameplay network
does not translate and the upscaler draws it in its place), the motion of what moves by itself, the picture without the HUD,
HDR Native's 10 bits and the lab's pin. No game, no GPU.

1. Compiled and run: sharpen_rules.h's SPIR-V patches on the real frame generation shaders (fg_copy, fg_synth,
   fg_fine_synth): their one rgba8 storage image declared Rgb10A2 (A2B10G10R10, HDR Native's) with the capability SPIR-V asks
   for added once, as the first instruction, or rgba16f; nothing else changed. spirv-val checks the modules where it is
   installed.
2. The source (FG-1..FG-9):
   - the upscaler chain (DLSS, FSR 4) makes frame generation's inputs from its own frame, before the bloom and in place of
     the dispatch (the path of a network that does not translate too), and copies its motion, what moves by itself in it,
     over theirs; the dispatch after it makes none again; RX_VK_FG_OWN_MOTION=0 the camera's alone;
   - the picture without the HUD is taken where the gameplay upscale's output is written (FSR 3.1, the chain, a network that
     does not translate, the game's own network and the TAAU pass in the 3/4 one's place), never left from a dispatch skipped;
   - HDR Native's frames are kept in a 10-bit format, the passes that store them made for it, and the present's retyped
     copy is what frame generation reads;
   - FSR 3.1's interpolation only with the row at 2x; RX_VK_FRAMEGEN_MULT takes the row's whole range.
   The old lines fail.

    python -I -B tests/test_framegen_amd.py [--cxx g++]
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

SPV = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "sharpen_rules.h"
#include "shaders/fg_copy_comp_spv.h"
#include "shaders/fg_synth_comp_spv.h"
#include "shaders/fg_fine_synth_comp_spv.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::sharpen_rules;

static size_t images(const std::vector<uint32_t>& w, uint32_t format) {   // (storage images, Sampled 2, of that format)
    size_t n = 0;
    for (size_t i = 5; i < w.size();) {
        const uint32_t op = w[i] & 0xffffu, count = w[i] >> 16;
        if (op == 25 && count >= 9 && w[i + 7] == 2 && w[i + 8] == format) n++;
        i += count;
    }
    return n;
}
static size_t capabilities(const std::vector<uint32_t>& w, uint32_t cap, bool* first) {
    size_t n = 0;
    bool past = false;
    *first = true;
    for (size_t i = 5; i < w.size();) {
        const uint32_t op = w[i] & 0xffffu, count = w[i] >> 16;
        if (op != 17) past = true;
        else if (w[i + 1] == cap) n++, *first = *first && !past;
        i += count;
    }
    return n;
}
static void write(const char* dir, const char* name, const std::vector<uint32_t>& w) {
    if (!dir) return;
    char path[1024];
    std::snprintf(path, sizeof path, "%s/%s.spv", dir, name);
    FILE* f = std::fopen(path, "wb");
    CHECK(f && std::fwrite(w.data(), 4, w.size(), f) == w.size());
    std::fclose(f);
}

static void one(const char* name, const uint32_t* code, size_t bytes, const char* dir) {
    const std::vector<uint32_t> before(code, code + bytes / 4);
    CHECK(images(before, kSpvRgba8) == 1);   // (the frame it stores, alone)
    bool first = false;
    CHECK(capabilities(before, kSpvCapStorageImageExtendedFormats, &first) == 0);
    // HDR Native's A2B10G10R10: Rgb10A2, which needs StorageImageExtendedFormats: added once, among the first instructions
    std::vector<uint32_t> w = before;
    CHECK(storage_format_patch(w, kSpvRgba8, kSpvRgb10A2) == 1);
    CHECK(spv_require_capability(w, kSpvCapStorageImageExtendedFormats));
    CHECK(w.size() == before.size() + 2 && images(w, kSpvRgb10A2) == 1 && images(w, kSpvRgba8) == 0);
    CHECK(capabilities(w, kSpvCapStorageImageExtendedFormats, &first) == 1 && first);
    CHECK(spv_require_capability(w, kSpvCapStorageImageExtendedFormats) && w.size() == before.size() + 2);   // (twice is once)
    CHECK(std::memcmp(w.data(), before.data(), 5 * 4) == 0);   // (the header as it was)
    // (every other word as it was: the two inserted, the one format)
    size_t differ = 0;
    for (size_t i = 5, j = 7; i < before.size(); i++, j++) differ += before[i] != w[j];
    CHECK(differ == 1);
    write(dir, (std::string(name) + "_rgb10a2").c_str(), w);
    // or RGBA16F, where the card stores no A2B10G10R10: no capability asked
    std::vector<uint32_t> h = before;
    CHECK(storage_format_patch(h, kSpvRgba8, kSpvRgba16f) == 1 && h.size() == before.size() && images(h, kSpvRgba16f) == 1);
    write(dir, (std::string(name) + "_rgba16f").c_str(), h);
    // not SPIR-V: nothing
    std::vector<uint32_t> bad(before.begin(), before.begin() + 4);
    CHECK(!spv_require_capability(bad, kSpvCapStorageImageExtendedFormats) && bad.size() == 4);
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : nullptr;
    one("fg_copy", FG_COPY_COMP_SPV, sizeof FG_COPY_COMP_SPV, dir);
    one("fg_synth", FG_SYNTH_COMP_SPV, sizeof FG_SYNTH_COMP_SPV, dir);
    one("fg_fine_synth", FG_FINE_SYNTH_COMP_SPV, sizeof FG_FINE_SYNTH_COMP_SPV, dir);
    std::puts("PASS: the frame generation shaders' storage image as A2B10G10R10 (its capability added once) or RGBA16F");
}
'''


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def function(source, head):
    start = source.index(head)
    return source[start:source.index("\n}\n", start) + 3]


def checks(t):
    temporal, fsr3, inputs, inputs_h, draw, fg, replay, fi, menu = (t["temporal"], t["fsr3"], t["inputs"], t["inputs_h"],
                                                                    t["draw"], t["fg"], t["replay"], t["fi"], t["menu"])
    # FG-4 and the gate: the chain makes frame generation's inputs from its frame, in both placements, with its motion
    helper = function(temporal, "void fg_inputs_from_chain(VkCommandBuffer cmd, Texture* scene, VkImageView view, bool linear,")
    check("if (!fsr3_inputs::wanted() || fsr3_wanted() || !scene || !view) return;" in helper,
          "the chain makes frame generation's inputs while FSR 3.1 upscales, or when nothing asks for them")
    check(helper.index("if (!fsr3_inputs::made_this_frame()) {") < helper.index("fsr3_inputs::prepare(cmd, scene, view, f, linear)") <
          helper.index("fsr3_inputs::chain_motion(cmd, s.motion.image, rw, rh)"),
          "the chain does not make the inputs where none are, then copy its motion over them")
    up = function(temporal, "bool temporal_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view,")
    check("if (!chain(cmd, mode, engine, cam, captures, in_view, rw, rh, ow, oh, out_view, nullptr, in->format, out->format)) return false;"
          in up and "fg_inputs_from_chain(cmd, in, in_view, in->format == VK_FORMAT_R16G16B16A16_SFLOAT, rw, rh);" in up,
          "temporal_upscale (a network's dispatch, or one that does not translate) makes no frame generation inputs")
    pre = function(temporal, "Texture* temporal_pre_tonemap(VkCommandBuffer cmd) {")
    check(pre.index("if (!chain(cmd, mode, *engine, cam, captures, view") < pre.index("fg_inputs_from_chain(cmd, scene, view, true, rw, rh);"),
          "before the bloom the chain makes no frame generation inputs")
    check("VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT" in function(temporal, "VkFormat in_format, VkFormat out_format) {"),
          "the chain's motion cannot be copied from")
    motion = function(inputs, "bool chain_motion(VkCommandBuffer cmd, VkImage motion, uint32_t width, uint32_t height) {")
    for what in ('env_switch("RX_VK_FG_OWN_MOTION") == 0', "s.frame.frame != texture_frame_no()", "s.frame.width != width",
                 "vkCmdCopyImage(cmd, motion, VK_IMAGE_LAYOUT_GENERAL, s.motion.image", "s.frame.chain_motion = true;"):
        check(what in motion, "chain_motion: missing %r" % what)
    check("return s.have_frame && s.frame.frame == texture_frame_no();" in function(inputs, "bool made_this_frame() {"),
          "made_this_frame is not this frame's inputs")
    check("bool chain_motion(VkCommandBuffer cmd, VkImage motion, uint32_t width, uint32_t height);" in inputs_h and
          "bool chain_motion = false;" in inputs_h, "vk_fsr3_inputs.h does not declare the chain's motion")
    up3 = function(fsr3, "bool fsr3_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view,")
    check("if (!upscaling && fsr3_inputs::made_this_frame()) return false;" in up3 and
          up3.index("fsr3_inputs::made_this_frame()") < up3.index("fsr3_inputs::prepare(cmd, in, in_view, f, hdr)"),
          "the dispatch after the bloom makes the inputs again, the camera's motion alone over the chain's")
    # FG-2: the picture without the HUD, wherever the upscale's output is written
    stand3 = function(draw, "bool fsr3_stand_in(VkCommandBuffer cmd, const NvnState& s, const Program& p) {")
    check(stand3.index("hudless_after = {};") < stand3.index("if (p.stages.empty() || !fsr3_inputs::wanted()) return false;"),
          "fsr3_stand_in leaves a skipped dispatch's picture to the next")
    check("if (done && out) framegen_hudless_note(cmd, out, level);" in stand3 and
          "else if (out && gameplay_network(p.key)) hudless_after = {out, level, p.key};" in stand3,
          "fsr3_stand_in takes no picture without the HUD")
    check("framegen_hudless_note(cmd, out, level);" in function(draw, "bool temporal_stand_in(VkCommandBuffer cmd, const NvnState& s, const Program& p) {"),
          "temporal_stand_in takes no picture without the HUD")
    rejected = function(draw, "static bool temporal_rejected_network(VkCommandBuffer cmd, uint64_t key, Texture* src, const nvn_tex* in, Texture* dst,\n                                      const nvn_tex* out) {")
    check("framegen_hudless_note(cmd, dst, 0);" in rejected, "a network that does not translate gives no picture without the HUD")
    check("if (done) hudless_after_dispatch(cmd, p.key);" in function(draw, "bool taau_stand_in(VkCommandBuffer cmd, const NvnState& s, const Program& p) {"),
          "the TAAU pass in the 3/4 network's place gives no picture without the HUD")
    at = draw.index("cw.dispatch_at = work_at(cw);")
    check(draw.index("hudless_after_dispatch(cmd, p.key);", at) - at < 200, "the game's own network gives no picture without the HUD")
    after = function(draw, "void hudless_after_dispatch(VkCommandBuffer cmd, uint64_t key) {")
    check("hudless_after = {};" in after and "h.key == key" in after, "a picture is taken for another dispatch than its own")
    # FG-5: HDR Native's 10 bits through frame generation
    ensure = function(fg, "bool ensure_size(VkCommandBuffer cmd, uint32_t w, uint32_t h) {")
    check("fg.format == format) return true;" in ensure, "a change of HDR mode does not make the frames again")
    check("make_img(fg.frames[i], w, h, format," in ensure and "make_img(set.frame[k], w, h, format," in ensure,
          "the frames are RGBA8 whatever the picture is")
    check("std::swap(fg.copy, fg.alt_copy), std::swap(fg.synth, fg.alt_synth), std::swap(fg.fine_synth, fg.alt_fine_synth);" in ensure,
          "the passes that store the frames are not the frames' format's")
    pipes = function(fg, "bool make_format_pipes(VkFormat f) {")
    check("sharpen_rules::spv_require_capability(w, sharpen_rules::kSpvCapStorageImageExtendedFormats)" in pipes and
          "storage_format_patch(w, sharpen_rules::kSpvRgba8, rgb10 ? sharpen_rules::kSpvRgb10A2 : sharpen_rules::kSpvRgba16f) != 1" in pipes,
          "the 10-bit passes are not the patched shaders")
    check("if (!present_native_pq.load(std::memory_order_relaxed) || fg.hdr_failed) return VK_FORMAT_R8G8B8A8_UNORM;" in
          function(fg, "VkFormat frames_format() {"), "the frames' format does not follow HDR Native")
    check("if (fg.format != VK_FORMAT_R8G8B8A8_UNORM) return 0;" in function(fg, "int fsr3fi_frame("),
          "FSR 3.1's interpolation is given HDR Native's 10-bit frames")
    check(replay.count("retyped_view ? retyped_view : texture_view(tex, t, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample)") == 2,
          "frame generation reads the window texture's own view where the present retyped it")
    check("retyped_view = rt.view;" in replay, "the retyped copy's view is not handed on")
    # FG-6, FG-9
    check("return opts.framegen == 2 && asked() && !dlssg::active() && capable() ? 1u : 0u;" in function(fi, "uint32_t frames_wanted() {"),
          "FSR 3.1's interpolation runs whatever the row says")
    check('if (const char* e = getenv("RX_VK_FRAMEGEN_MULT")) opts.framegen = framegen_rows::env_row(e);' in menu,
          "RX_VK_FRAMEGEN_MULT cannot pin AMD's frame generation")


MUTATIONS = [
    ("no inputs at the chain's dispatch", "temporal",
     "    fg_inputs_from_chain(cmd, in, in_view, in->format == VK_FORMAT_R16G16B16A16_SFLOAT, rw, rh);\n", ""),
    ("no inputs before the bloom", "temporal", "    fg_inputs_from_chain(cmd, scene, view, true, rw, rh);\n", ""),
    ("the camera's motion alone", "temporal", "    if (s.motion.image) (void)fsr3_inputs::chain_motion(cmd, s.motion.image, rw, rh);\n", ""),
    ("made twice", "fsr3", "    if (!upscaling && fsr3_inputs::made_this_frame()) return false;\n", ""),
    ("no picture at a network that does not translate", "draw", "    framegen_hudless_note(cmd, dst, 0);\n", ""),
    ("no picture at the TAAU pass", "draw", "    if (done) hudless_after_dispatch(cmd, p.key);", "    if (done) {}"),
    ("no picture at the game's network", "draw", "    hudless_after_dispatch(cmd, p.key);   // ((Triforce, FG-2)", "    // ("),
    ("a skipped dispatch's picture kept", "draw", "    hudless_after = {};   // ((Triforce, FG-2) a dispatch's own", "    // ("),
    ("8 bits in HDR Native", "fg", "make_img(fg.frames[i], w, h, format,", "make_img(fg.frames[i], w, h, VK_FORMAT_R8G8B8A8_UNORM,"),
    ("the frames not made again", "fg", " && fg.format == format) return true;", ") return true;"),
    ("the window texture's view", "replay", "retyped_view ? retyped_view : texture_view(tex, t", "texture_view(tex, t"),
    ("FSR 3.1 whatever the row", "fi", "return opts.framegen == 2 && asked()", "return asked()"),
    ("the old pin", "menu", "opts.framegen = framegen_rows::env_row(e);", "opts.framegen = std::min(std::max(atoi(e), 2), 8);"),
]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix="framegen-amd-") as tmp:
        cpp, exe = Path(tmp) / "spv.cpp", Path(tmp) / ("spv.exe" if os.name == "nt" else "spv")
        cpp.write_text(SPV, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(VK), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe), tmp], check=True, timeout=60)
        val = shutil.which("spirv-val")
        if val:
            for spv in sorted(Path(tmp).glob("*.spv")):
                subprocess.run([val, "--target-env", "vulkan1.2", str(spv)], check=True, timeout=60)
            print("spirv-val: the patched modules are valid")
        else:
            print("spirv-val is not installed here: the patched modules were not validated")
    texts = {"temporal": (VK / "vk_temporal.cpp").read_text(encoding="utf-8"),
             "fsr3": (VK / "vk_fsr3.cpp").read_text(encoding="utf-8"),
             "inputs": (VK / "vk_fsr3_inputs.cpp").read_text(encoding="utf-8"),
             "inputs_h": (VK / "vk_fsr3_inputs.h").read_text(encoding="utf-8"),
             "draw": (VK / "vk_draw.cpp").read_text(encoding="utf-8"),
             "fg": (VK / "vk_framegen.cpp").read_text(encoding="utf-8"),
             "replay": (VK / "vk_replay.cpp").read_text(encoding="utf-8"),
             "fi": (VK / "vk_fsr3fi.cpp").read_text(encoding="utf-8"),
             "menu": (VK / "vk_menu.cpp").read_text(encoding="utf-8")}
    checks(texts)
    for name, which, new, old in MUTATIONS:
        check(new in texts[which], "the mutation '%s' has nothing to change" % name)
        t = dict(texts)
        t[which] = t[which].replace(new, old, 1)
        try:
            checks(t)
        except (SystemExit, ValueError):
            continue
        raise SystemExit("FAIL: the mutation '%s' passed" % name)
    print("PASS: AMD's frame generation gets the scene's depth and motion where the upscaler draws (what moves by itself in "
          "it), the picture without the HUD, HDR Native's 10 bits, and the lab's pin; the old lines fail")


if __name__ == "__main__":
    main()
