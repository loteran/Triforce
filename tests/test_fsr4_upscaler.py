"""(Triforce) AMD FSR 4 on the frame chain (gpu/vk/vk_fsr4.cpp, vk_temporal.cpp; gpu/vk/temporal_rules.h). No game, no GPU.

1. Compiled and run: temporal_rules.h's Retry (a failure leaves the game's upscale for 2 s, then the engine is tried again;
   the fourth in a row turns it off until the Upscaler row is set again; a good evaluation counts from 0 again), native_at
   (a native AA anti-aliases at the render size only while the 3D is drawn at the output's size: one chosen in play scales
   the smaller 3D up), plan_context (a fixed share gets a context of its own render size without the dynamic-resolution
   flag, the game's dynamic resolution one with it for every size, once the size changes) and frame_ms (under 100 ms, else
   a 60th).
2. The source: a creation or evaluation failure goes through fail_again, not the old latch for good; temporal_mode says the
   game's own once the engine is off and a fresh Upscaler choice tries it anew; the chain's native AA and the one before
   the bloom follow native_at and the upscale's output; an RGBA16F (RenoDX HDR Native) picture is no longer refused to
   FSR 4; FSR 4's context chains ffxCreateContextDescUpscaleVersion, refuses a provider under 4, takes the frame time from
   frame_ms and the dynamic-resolution flag only with plan_context's drs; FSR 4's jitter is AMD's length; the menu names
   FSR 4 by the version AMD's library runs and says when the game's own stands in. The old lines fail.

    python -I -B tests/test_fsr4_upscaler.py [--cxx g++]
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
#include <cstdio>
#include "temporal_rules.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

using namespace mkwvk::temporal_rules;

int main() {
    // a failure: the game's upscale for 2 s, then tried again; the fourth in a row: off
    Retry r;
    const int64_t s = 1'000'000'000;
    CHECK(!r.waiting(0) && !r.is_off());
    CHECK(!r.failed(10 * s) && r.count() == 1 && r.waiting(10 * s) && r.waiting(11 * s) && !r.waiting(12 * s));
    CHECK(!r.failed(12 * s) && !r.failed(14 * s) && !r.is_off() && r.count() == 3);
    CHECK(r.failed(16 * s) && r.is_off());
    // the Upscaler row set again: tried anew, from 0
    CHECK(r.rearm() && !r.is_off() && r.count() == 0 && !r.waiting(16 * s));
    CHECK(!r.rearm());
    // a good evaluation between failures: they are not added up over a session
    CHECK(!r.failed(20 * s) && !r.failed(22 * s));
    r.succeeded();
    CHECK(r.count() == 0 && !r.waiting(22 * s));
    CHECK(!r.failed(30 * s) && !r.failed(32 * s) && !r.failed(34 * s) && !r.is_off());

    // native AA: only at the output's size. Chosen in play over a 67% 3D: scaled up as FSR 4's upscale
    CHECK(native_at(true, 2560, 1440, 2560, 1440));
    CHECK(!native_at(true, 1707, 960, 2560, 1440) && !native_at(true, 2560, 960, 2560, 1440));
    CHECK(!native_at(false, 2560, 1440, 2560, 1440) && !native_at(false, 854, 480, 2560, 1440));

    // FSR 4's context: the first, a fixed share's own size, no dynamic-resolution flag
    ContextPlan p = plan_context(false, 0, 0, 0, 0, false, false, 854, 480, 2560, 1440, false, false, false);
    CHECK(p.remake && !p.drs && !p.dynamic && !p.went_dynamic);
    p = plan_context(true, 2560, 1440, 854, 480, false, false, 854, 480, 2560, 1440, false, false, false);
    CHECK(!p.remake);
    // (HDR colour now, another output: made again, of its own size still)
    p = plan_context(true, 2560, 1440, 854, 480, false, false, 854, 480, 2560, 1440, true, false, false);
    CHECK(p.remake && !p.drs);
    p = plan_context(true, 2560, 1440, 854, 480, false, false, 1280, 720, 3840, 2160, false, false, false);
    CHECK(p.remake && !p.drs && !p.dynamic);
    // the render size changes under the output (the game's dynamic resolution): one context for every size from then on
    p = plan_context(true, 2560, 1440, 1707, 960, false, false, 1600, 900, 2560, 1440, false, false, false);
    CHECK(p.remake && p.drs && p.dynamic && p.went_dynamic);
    p = plan_context(true, 2560, 1440, 2560, 1440, true, false, 1500, 844, 2560, 1440, false, true, false);
    CHECK(!p.remake && p.dynamic && !p.went_dynamic);
    p = plan_context(true, 2560, 1440, 2560, 1440, true, false, 2560, 1440, 2560, 1440, false, true, false);
    CHECK(!p.remake);   // (up to 100%: the same context, NX2navi's crash was a context without the flag)
    // RX_VK_FSR4_DRS=1: the flag from the start, as before
    p = plan_context(false, 0, 0, 0, 0, false, false, 854, 480, 2560, 1440, false, false, true);
    CHECK(p.remake && p.drs);
    p = plan_context(true, 2560, 1440, 854, 480, false, false, 854, 480, 2560, 1440, false, false, true);
    CHECK(p.remake && p.drs);

    // the frame time: under 100 ms, else a 60th
    CHECK(frame_ms(false, 8.3f) == 8.3f && frame_ms(false, 33.3f) == 33.3f);
    CHECK(frame_ms(true, 8.3f) == 16.667f && frame_ms(false, 1500.0f) == 16.667f && frame_ms(false, 0.0f) == 16.667f);
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


def checks(temporal, fsr4, menu):
    # FSR4-1: a failure is tried again, off after four in a row, the row then says the game's own; a new choice re-arms
    chain = function(temporal, "bool chain(VkCommandBuffer cmd, int mode, Engine& engine, const Camera& cam,", last=True)
    check('if (ready < 0) return fail_again(engine, why ? why : "it could not be made");' in chain,
          "a failed creation still turns the engine off for good")
    check('return fail_again(engine, "its evaluation failed");' in chain and "engine.retry.succeeded();" in chain,
          "a failed evaluation still turns the engine off for good, or a good one does not count from 0 again")
    check(chain.count("engine.failed = true;") == 2, "only a missing engine and its shaders are failures for good")
    again = function(temporal, "bool fail_again(Engine& k, const char* why) {")
    check("k.e->release();" in again and "drop_image(*im);" in again and "k.retry.failed(now_ns())" in again and
          "s.last_frame = 0;" in again, "fail_again does not let go what the engine and the chain hold, or count the failure")
    mode = function(temporal, "int temporal_mode() {")
    check("rearm_on_choice(v);" in mode and "if (stood_in(v)) return ur::GAME;" in mode,
          "temporal_mode does not say the game's own for an engine that is off, or a new choice does not try it anew")
    check(mode.index("rearm_on_choice(v);") < mode.index("if (dlss > 0)"), "the choice is read after the switches")
    check("if (waiting(engine)) return fail(" in function(temporal, "bool temporal_upscale(VkCommandBuffer cmd,") and
          "waiting(*engine)" in function(temporal, "Texture* temporal_pre_tonemap(VkCommandBuffer cmd) {"),
          "the chain does not wait before it tries a failed engine again")
    check("k.retry.is_off()" in function(temporal, "bool stood_in(int mode) {"), "stood_in does not read the retry state")
    check("retry.rearm()" in function(temporal, "void rearm_on_choice(int choice) {"), "a new choice does not re-arm")
    hint = function(menu, "const char* hn_upscaler(int v, int variant) {")
    check("temporal_stood_in()" in hint and "The game's own stands in: %s failed four times in a row" in hint,
          "the Upscaler's help does not say when the game's own stands in for FSR 4")
    # (the FSR 4 it names is the Upscaler row's, by its version: "AMD FSR 4.1")
    stood = hint[hint.index("static char fsr4_stood_in["):hint.index("const char* const stood_in[2] = {")]
    check("fsr4_name(false)" in stood, "the stand-in line names FSR 4 without its version")
    check("else if (variant < DLSS_WHY_COUNT + FSR_WHY_COUNT + 1) return stood_in[" in hint,
          "the stand-in lines are not among the help's variants (the menu's height)")
    # FSR4-2: native AA only at the output's size, before the bloom too
    check("const bool native = temporal_rules::native_at(ur::full_scale(mode), rw, rh, ow, oh);" in chain,
          "the chain's native AA anti-aliases a smaller 3D and stretches it")
    pre = function(temporal, "Texture* temporal_pre_tonemap(VkCommandBuffer cmd) {")
    check("if (!ur::full_scale(mode) || (seen_w && seen_h && (seen_w > rw || seen_h > rh))) {" in pre,
          "before the bloom a native AA chosen in play does not scale the smaller 3D up to the output")
    # ((Triforce, options_version 7) an engine's native AA is the Render resolution's Native (100%): that line says it, for
    # DLAA, FSR 4's and FSR 3.1's alike (ur::full_scale)
    resolution = function(menu, "const char* hn_resolution(int v, int variant) {")
    native = resolution[resolution.index("if (ur::full_scale(up))"):resolution.index("const int p = ur::percent(v);")]
    check("takes its full size at the next start." in native,
          "the native AA's help (the Render resolution's Native) does not say the 3D takes its full size at the next start")
    # FSR4-3: an RGBA16F picture goes to FSR 4
    check("only DLSS takes it" not in chain, "the chain still refuses FSR 4 an RGBA16F (HDR Native) picture")
    check("const bool hdr = pre != nullptr || in_format == VK_FORMAT_R16G16B16A16_SFLOAT;" in chain,
          "the chain no longer gives an RGBA16F picture as HDR colour")
    # FSR4-4, FSR4-8, FSR4-9, FSR4-10: the context and the dispatch
    make = function(fsr4, "std::shared_ptr<Context> make_context(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool drs, bool hdr) {")
    check("c->built.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;" in make and
          "c->built.version = FFX_UPSCALER_VERSION;" in make and "c->version.header.pNext = &c->built.header;" in make,
          "the upscale context is made without ffxCreateContextDescUpscaleVersion")
    check("c->built.header.pNext = &c->legacy_backend.header;" in make, "(linux) the older backend description left the chain")
    check("if (known && atoi(provider.versionName) < 4) {" in make and "g_why = FSR_NO_MODEL;" in make and
          "return nullptr;" in make[make.index("atoi(provider.versionName) < 4"):], "a provider under 4 runs under FSR 4's name")
    check("if (drs) c->upscale.flags |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;" in make and
          make.count("FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION") == 1, "the dynamic-resolution flag is set for a fixed share")
    check("c->upscale.maxRenderSize = drs ? FfxApiDimensions2D{ow, oh} : FfxApiDimensions2D{rw, rh};" in make,
          "a fixed share's context is not made for its own render size")
    prep = function(fsr4, "int prepare(VkCommandBuffer, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool hdr,")
    check("temporal_rules::plan_context(" in prep and "make_context(rw, rh, ow, oh, plan.drs, hdr)" in prep,
          "FSR 4's prepare does not follow plan_context")
    ev = function(fsr4, "bool evaluate(VkCommandBuffer cmd, const TemporalFrame& f) {")
    check("temporal_rules::frame_ms(!s.evals," in ev and "d.frameTimeDelta = frame_ms;" in ev, "FSR 4's frame time is not clamped")
    check("s.jitter_phases = ur::is_fsr4(mode) ? fsr3_math::jitter_phases(rw, dw) : (uint32_t)dlss_jitter_phases(rw, rh, dw, dh);"
          in chain, "FSR 4's jitter is not AMD's length")
    # FSR4-7: the name by the version, and the driver in the help
    load = function(fsr4, "bool load() {")
    check('snprintf(g_name[0], sizeof g_name[0], "AMD FSR %u.%u", best[0], best[1]);' in load and
          "g_named.store(true, std::memory_order_release);" in load, "FSR 4's name is not taken from the version AMD's library runs")
    name = function(fsr4, "const char* fsr4_name(bool native_aa) {")
    check("g_named.load(std::memory_order_acquire)" in name and "lock" not in name, "fsr4_name waits for the library")
    tx = function(menu, "const char* tx_upscaler(int) {")
    check("if (ur::is_fsr4(up)) return fsr4_name(up == ur::FSR4_AA);" in tx, "the Upscaler row always says AMD FSR 4")
    # ((Triforce, options_version 7) the row names the engine alone: FSR 4's Native AA is the Render resolution's Native)
    check("const int up = ur::engine(std::clamp(upscaler(), 0, ur::kUpscalers - 1));" in tx,
          "the Upscaler row names an engine's native AA, which the Render resolution's Native (100%) says")
    check("driver 26.10.1 or newer" in hint,"the help for no FSR 4 here does not name the driver it needs")


MUTATIONS = [
    ("the old latch", "temporal", 'if (ready < 0) return fail_again(engine, why ? why : "it could not be made");',
     'if (ready < 0) { engine.failed = true; return fail(why); }'),
    ("no stand-in", "temporal", "if (stood_in(v)) return ur::GAME;", ""),
    ("the old native", "temporal", "const bool native = temporal_rules::native_at(ur::full_scale(mode), rw, rh, ow, oh);",
     "const bool native = ur::full_scale(mode);"),
    ("the old HDR refusal", "temporal", "    if (!engine.started) engine.started = true, engine.ok = engine.e->start();",
     '    if (hdr && !pre && !ur::is_dlss(mode)) return fail("an HDR scene: only DLSS takes it");\n'
     "    if (!engine.started) engine.started = true, engine.ok = engine.e->start();"),
    ("no version struct", "fsr4", "c->version.header.pNext = &c->built.header;", ""),
    ("no provider check", "fsr4", "if (known && atoi(provider.versionName) < 4) {", "if (false) {"),
    ("the old frame time", "fsr4", "temporal_rules::frame_ms(!s.evals,", "(float)(!s.evals) + ("),
    ("the old name", "menu", "if (ur::is_fsr4(up)) return fsr4_name(up == ur::FSR4_AA);", ""),
    ("the stand-in's old name", "menu", "it anew.\", fsr4_name(false));", "it anew.\", \"AMD FSR 4\");"),
]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    texts = {"temporal": (VK / "vk_temporal.cpp").read_text(encoding="utf-8"),
             "fsr4": (VK / "vk_fsr4.cpp").read_text(encoding="utf-8"),
             "menu": (VK / "vk_menu.cpp").read_text(encoding="utf-8")}
    checks(texts["temporal"], texts["fsr4"], texts["menu"])
    for name, which, new, old in MUTATIONS:
        check(new in texts[which], "the mutation '%s' has nothing to change" % name)
        t = dict(texts)
        t[which] = t[which].replace(new, old)
        try:
            checks(t["temporal"], t["fsr4"], t["menu"])
        except SystemExit:
            continue
        raise SystemExit("FAIL: the mutation '%s' passed" % name)
    with tempfile.TemporaryDirectory() as tmp:
        cpp, exe = Path(tmp) / "fsr4_upscaler.cpp", Path(tmp) / ("fsr4_upscaler" + (".exe" if os.name == "nt" else ""))
        cpp.write_text(CHECK, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(VK), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)
    print("PASS: AMD FSR 4's failures tried again and the stand-in said, native AA in play, RGBA16F colour, its context "
          "(the version struct, the provider, a fixed share's own size), its frame time, jitter and name; the old lines fail")


if __name__ == "__main__":
    main()
