"""The textures' waits count seconds, not presents, below 60 presents a second and above it (gpu/vk/present_rate.h; no game or GPU).

The eviction (gpu/vk/vk_texture.cpp texture_sweep) lets a texture go by how many presents it has gone unused: 60 over the
budget, 30 after the game let go of it, 600 or 3600 for one the GPU drew (keep_rendered.h), 600 and 1800 for the
compaction's hold and reserve (vram_compact.h). Those were set at 60 presents a second. At 30 they last twice as long and
at 15 four times, and the card short of video memory is the one that runs slow: what the waits hold grew as the frame
rate fell. NX2navi measured the same in its cache (442 MiB held at 60 presents a second, 2,105 at 21) and scales its
waits by the presents a second (runtime/vk_tex_release.h, by the owner); this is that rule below 60. And above 60
(NX2navi's M19, vk_texture.c 177-181): at the Market's 94 a second "60 presents unused" was 0.64 s, and NX2navi found its
textures going three times as readily as meant at 85 to 240 a second.

What must hold: the rule (compiled and run here): the rate is the highest of the last 8 seconds, 15 to 480, rounded up, so
that a wait is the same seconds at any rate as at 60 and never shorter, a slow second or a long gap shortens nothing;
RX_VK_TEX_RATE_HIGH=0 keeps 60 and above at the presents they were, RX_VK_TEX_RATE_LOW=0 below. And vk_texture.cpp:
texture_frame ticks the clock at each present with both switches, texture_sweep takes every one of those ages through it,
and the dead transient targets' wait stays a count of frames. Each part of the rule is then broken in turn in a copy of the
header and the compiled checks must fail.

python -I -B tests/test_present_rate.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

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
    need("vk_texture.cpp", tex, ['#include "present_rate.h"', "present_rate::Clock tex_rate;"])
    frame = block(tex, "void texture_frame(uint64_t completed)")
    need("vk_texture.cpp: texture_frame()", frame,
         ['getenv("RX_VK_TEX_RATE_LOW")', 'getenv("RX_VK_TEX_RATE_HIGH")', "tex_rate.tick(frame_no, now_ns, low, high)",
          "std::chrono::steady_clock::now()"])
    # (ticked once a present, where the present is counted, before the pass that asks)
    if not frame.index("    frame_no++;") < frame.index("tex_rate.tick(") < frame.index("texture_sweep("):
        raise SystemExit("vk_texture.cpp: texture_frame() does not tick the presents' clock between the present's count and the pass")
    sweep = block(tex[tex.index("void texture_frame(uint64_t completed)"):], "void texture_sweep(uint64_t completed")
    need("vk_texture.cpp: texture_sweep()", sweep,
         ["const auto age60 = [](uint64_t presents) { return present_rate::at60(presents, tex_rate.rate); };",
          # (the eviction's own wait, in the hold's count of the dead targets and in the pass)
          "if (age60(frame_no - last) < need_unused && !shifted) continue;",
          "if (age60(frame_no - last) < need_unused && !shifted && !gone_early) continue;",
          # (what the game let go: in the mass that turns the rule on, and for each texture; the longest gap before a
          # take-back, keep taken's, too)
          "return age60(frame_no - tex->let_go) >= tex_pressure::let_go_wait(let_go_after, age60(tex->back_gap), keep_taken);",
          "if (tex->let_go && !tex->gpu_written && let_go_due(tex)) mass += tex->bytes + tex->pad;",
          "const bool gone_early = let_go_short && tex->let_go && !tex->gpu_written && let_go_due(tex);",
          # (the compaction's hold and reserve, and what the GPU drew)
          "vram_compact::hold_target(age60(frame_no - last), need_unused, rule_to, hold_to, evict_to);",
          "age60(frame_no - last) < keep_rendered) continue;",
          # (the dead transient targets' wait counts frames: the pass stops at the first texture too young for it)
          "const auto too_young = [&](uint64_t last) { return (dead_targets ? frame_no - last : age60(frame_no - last)) < need_dead; };",
          "if (too_young(last)) break;", "|| too_young(last))\n            break;"])
    # (no wait of the pass is left counting presents: the only ages not taken through the clock are the log's and too_young's)
    raw = [line.strip() for line in sweep.splitlines()
           if ("frame_no - last" in line or "frame_no - tex->let_go" in line) and "age60(" not in line]
    if len(raw) != 1 or "(unsigned long long)(frame_no - last));" not in raw[0]:
        raise SystemExit("vk_texture.cpp: texture_sweep() compares an age in presents again: " + "; ".join(raw))


# (each breaks one part of the rule in a copy of present_rate.h: the compiled checks must fail)
MUTATIONS = [
    ("above 60 counts as 60 with high on", "most = high ? kMost : k60;", "most = k60;"),
    ("below 60 counts as 60 with low on", "least = low ? kLeast : k60,", "least = k60,"),
    ("the rate is rounded down", "+ ns * 60 - 1) / (ns * 60);", ") / (ns * 60);"),
    ("the rate is the last second's, not the highest", "m = std::max(m, ring[i] ? ring[i] : k60);", "m = ring[i] ? ring[i] : k60;"),
    ("an age above 60 is its presents", "return rate && rate != k60 ? n * k60 / rate : n;", "return rate && rate < k60 ? n * k60 / rate : n;"),
    ("the clock leaves high out", "sample(presents - f0, now_ns - t0, low, high);", "sample(presents - f0, now_ns - t0, low, false);"),
    ("no cap at 480", "inline constexpr uint32_t k60 = 256, kLeast = 64, kMost = 2048;", "inline constexpr uint32_t k60 = 256, kLeast = 64, kMost = 1u << 30;"),
]


def run(cxx, include, tmp, name):
    exe = Path(tmp) / name
    built = subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(include), "-I", str(ROOT / "gpu/vk"),
                            str(ROOT / "tests/test_present_rate.cpp"), "-o", str(exe)], capture_output=True, text=True)
    if built.returncode:
        return False, built.stdout + built.stderr
    ran = subprocess.run([str(exe)], capture_output=True, text=True)
    return ran.returncode == 0, ran.stdout + ran.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    header = (ROOT / "gpu/vk/present_rate.h").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="present-rate-") as tmp:
        ok, out = run(args.cxx, ROOT / "gpu/vk", tmp, "present-rate")
        print(out, end="")
        if not ok:
            raise SystemExit("present_rate.h: the checks fail")
        for i, (what, old, new) in enumerate(MUTATIONS):
            if header.count(old) != 1:
                raise SystemExit("mutation %r: its text is not once in present_rate.h" % what)
            inc = Path(tmp) / ("m%d" % i)
            inc.mkdir()
            (inc / "present_rate.h").write_text(header.replace(old, new), encoding="utf-8")
            ok, out = run(args.cxx, inc, tmp, "present-rate-m%d" % i)
            if ok:
                raise SystemExit("the checks pass when %s" % what)
            print("PASS: the checks fail when %s" % what)


if __name__ == "__main__":
    main()
