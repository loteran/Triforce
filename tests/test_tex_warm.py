"""Textures made and uploaded ahead, while the frames are light (gpu/vk/tex_warm.h, RX_VK_TEX_WARM; no game, no GPU).

The rules of the header, compiled and run: off unless the switch is set; a submission's start warms only after a light frame
and while the frame's uploads are under the budget; a slot is taken only some frames after it was registered; the look stops
when the time spent or the frame's uploads take the budget. Each rule taken out in turn (a mutation) must make the test
fail. Then the sources: a registered slot is noted on both of the replay's registration paths; the frame's work is counted
at each draw and dispatch and handed over at the present; the step runs at the start of a game's submit, in its recording,
not at a resumed one; it looks a texture up as a draw's first look does, under the recording lock; the switch's row is in
the development guide.

    python -I -B tests/test_tex_warm.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def in_order(text, parts, where):
    at = 0
    for part in parts:
        found = text.find(part, at)
        if found < 0:
            raise SystemExit("%s: %r is missing, or not after the part before it (update the test with the code)" % (where, part))
        at = found + len(part)


HEADER_TEST = r'''
#include <cstdio>
#include <cstdlib>
#include "tex_warm.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace tex_warm;
int main() {
    // (off unless set)
    CHECK(!on_of(nullptr) && !on_of("") && !on_of("0") && on_of("1") && on_of("2"));
    // (a submission's start: the switch, a light frame before, the frame's uploads under the budget)
    CHECK(may_warm(true, 0, 0, 0) && may_warm(true, kLightWork - 1, kBudgetNs - 1, kBudgetNs - 1));
    CHECK(!may_warm(false, 0, 0, 0) && !may_warm(true, kLightWork, 0, 0) && !may_warm(true, 2500, 0, 0));
    CHECK(!may_warm(true, 0, kBudgetNs, 0) && !may_warm(true, 0, 2 * kBudgetNs, 0));
    CHECK(!may_warm(true, 0, 0, kBudgetNs));   // (the frame's earlier submissions took the budget)
    // (room: by the card's memory in use when it says, else by the textures' share of their budget; none known, no room)
    const uint64_t GB = 1ull << 30;
    CHECK(has_room(true, 3 * GB, 8 * GB, 0, 0) && !has_room(true, 4 * GB, 8 * GB, 0, 0) && !has_room(true, 7 * GB, 8 * GB, 0, 100 * GB));
    CHECK(has_room(false, 0, 0, 1 * GB, 4 * GB) && !has_room(false, 0, 0, 2 * GB, 4 * GB));
    CHECK(!has_room(false, 0, 0, 0, 0) && !has_room(false, 0, 0, 0, ~0ull) && !has_room(true, 0, 0, 0, 0));
    // (a slot: only some frames after it was registered)
    CHECK(!ripe(100, 100) && !ripe(100, 100 + kAgeFrames - 1) && ripe(100, 100 + kAgeFrames) && ripe(100, 5000));
    // (the look goes on while neither the time nor the frame's uploads took the budget)
    CHECK(goes_on(0, 0) && goes_on(kBudgetNs - 1, kBudgetNs - 1));
    CHECK(!goes_on(kBudgetNs, 0) && !goes_on(0, kBudgetNs));
    CHECK(kLightWork >= 100 && kLightWork <= 1000 && kBudgetNs >= 1000000 && kBudgetNs <= 12000000 && kMaxPending >= 1024);
    std::puts("PASS header: the switch, a submission's start, a slot's age, the look's budget");
    return 0;
}
'''

MUTATIONS = [
    ("off unless set", "return env && *env && std::atoi(env) != 0;", "return !env || !*env || std::atoi(env) != 0;"),
    ("0 is off", "return env && *env && std::atoi(env) != 0;", "return env && *env;"),
    ("only with the switch", "return on && last_frame_work < kLightWork && frame_upload_ns < kBudgetNs && frame_spent_ns < kBudgetNs;",
     "return last_frame_work < kLightWork && frame_upload_ns < kBudgetNs && frame_spent_ns < kBudgetNs + (on ? 0 : 0);"),
    ("only after a light frame", "return on && last_frame_work < kLightWork && frame_upload_ns < kBudgetNs && frame_spent_ns < kBudgetNs;",
     "return on && frame_upload_ns < kBudgetNs && frame_spent_ns < kBudgetNs + 0 * last_frame_work;"),
    ("only under the frame's uploads' budget", "return on && last_frame_work < kLightWork && frame_upload_ns < kBudgetNs && frame_spent_ns < kBudgetNs;",
     "return on && last_frame_work < kLightWork && frame_spent_ns < kBudgetNs + 0 * frame_upload_ns;"),
    ("one budget for a frame's submissions", "return on && last_frame_work < kLightWork && frame_upload_ns < kBudgetNs && frame_spent_ns < kBudgetNs;",
     "return on && last_frame_work < kLightWork && frame_upload_ns < kBudgetNs + 0 * frame_spent_ns;"),
    ("room by the card's memory", "    if (known && allowed) return used / 100 * 100 < allowed / 100 * kRoomPct;\n", ""),
    ("no room when none is known", "    if (!budget || budget == ~0ull) return false;\n", "    if (!budget) return false;\n"),
    ("room by the textures' budget", "    return texture_bytes < budget / 100 * kRoomPct;", "    return texture_bytes < budget;"),
    ("a slot waits its frames", "return frame >= noted + kAgeFrames;", "return frame >= noted;"),
    ("the time spent ends the look", "return spent_ns < kBudgetNs && frame_upload_ns < kBudgetNs;", "return frame_upload_ns < kBudgetNs + 0 * spent_ns;"),
    ("the frame's uploads end the look", "return spent_ns < kBudgetNs && frame_upload_ns < kBudgetNs;", "return spent_ns < kBudgetNs + 0 * frame_upload_ns;"),
]


def check_source(tex, replay, dev):
    if '#include "tex_warm.h"' not in tex:
        raise SystemExit("vk_texture.cpp does not include tex_warm.h")
    note = block(tex, "void texture_warm_note(int slot) {")
    in_order(note, ["if (!texture_warm_on()) return;", "std::lock_guard lk(warm_mu);",
                    "if (warm_pending.size() < tex_warm::kMaxPending && warm_noted.insert(slot).second) warm_pending.push_back({slot, frame_no});"],
             "vk_texture.cpp texture_warm_note")
    step = block(tex, "void texture_warm_step(int64_t last_frame_work) {")
    in_order(step, ["if (warm_frame != frame_no) warm_frame = frame_no, warm_frame_ns = 0;",
                    "if (!tex_warm::may_warm(texture_warm_on(), last_frame_work, frame_upload_ns.load(std::memory_order_relaxed), warm_frame_ns)) return;",
                    "if (!tex_warm::has_room(warm_known, warm_used, warm_allowed, total_bytes, warm_budget)) return;",
                    "if (warm_pending.empty() || !tex_warm::ripe(warm_pending.front().second, frame_no)) break;",
                    "warm_pending.pop_front();", "warm_noted.erase(e.first);",
                    "if (!texture_of(e.first, &t, &v, &has_view) || !t.gpu) continue;", "ParLock lk;",
                    "texture_get(&t, true);", "if (!tex_warm::goes_on(warm_frame_ns + spent, frame_upload_ns.load(",
                    "warm_frame_ns += spent;"], "vk_texture.cpp texture_warm_step")
    in_order(tex, ["warm_known = known, warm_used = used, warm_allowed = allowed, warm_budget = budget;",
                   "texture_sweep(completed, total_bytes <= budget, budget, evict_to, known, used, allowed, false, hold_to, rule_to);"],
             "vk_texture.cpp texture_frame")
    if 'tex_warm::on_of(getenv("RX_VK_TEX_WARM"))' not in tex:
        raise SystemExit("vk_texture.cpp: the switch is not read through tex_warm::on_of")
    # (the replay: the frame's work, the registrations, the step at a game's submit's start)
    in_order(replay, ["static const bool count_work = texture_warm_on();",
                      "if (count_work) frame_work.fetch_add(1, std::memory_order_relaxed);"], "vk_replay.cpp replay: counted only with the switch")
    if replay.count("frame_work.fetch_add(1, std::memory_order_relaxed);") != 1:
        raise SystemExit("vk_replay.cpp: the frame's work is not counted in one place")
    in_order(replay, ["stream_work++;", "frame_work.fetch_add(1, std::memory_order_relaxed);"], "vk_replay.cpp replay")
    in_order(replay, ["texture_frame(completed());",
                      "last_frame_work.store(frame_work.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);"],
             "vk_replay.cpp do_present")
    run = block(replay, "bool run_submit(const uint64_t* handles, int n, uint64_t value, uint64_t queue, bool memory_work, int cb = 0,")
    in_order(run, ["Recording& first = begin_recording(tag);", "memory_begin_submission();",
                   'full_barrier(first.cmd, "barrier (recording start)");',
                   "if (memory_work && !p)",
                   "texture_warm_step(std::max(last_frame_work.load(std::memory_order_relaxed), frame_work.load(std::memory_order_relaxed)));",
                   "VkCommandBuffer cmd = rec_cur->cmd;"], "vk_replay.cpp run_submit")
    if replay.count("texture_warm_step(") != 1:
        raise SystemExit("vk_replay.cpp: the textures are made ahead in a place this test does not know")
    locked = block(replay, "void register_texture_locked(int id, const nvn_tex* t, const nvn_view* v) {")
    in_order(locked, ["register_texture(id, t, v);", "texture_warm_note(id);"], "vk_replay.cpp register_texture_locked")
    in_order(replay, ["case JOB_REGISTER_TEXTURE:", "register_texture_copy(j.id, j.tex, j.has_view ? &j.view : nullptr);",
                      "texture_warm_note(j.id);", "break;"], "vk_replay.cpp run_job")
    if "RX_VK_TEX_WARM" not in dev:
        raise SystemExit("docs/development.md: RX_VK_TEX_WARM is not in the switches table")


def build(cxx, d, name, src, include):
    cpp, exe = d / (name + ".cpp"), d / (name + ".exe")
    cpp.write_text(src, encoding="utf-8")
    r = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I" + str(include), str(cpp),
                        "-o", str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        return None, r.stdout
    return exe, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    a = ap.parse_args()
    check_source((VK / "vk_texture.cpp").read_text(encoding="utf-8"), (VK / "vk_replay.cpp").read_text(encoding="utf-8"),
                 (ROOT / "docs" / "development.md").read_text(encoding="utf-8"))
    print("PASS sources: registrations noted on both paths, the frame's work counted and handed over at the present, the step at "
          "a game's submit's start in its recording, a draw's first look under the lock, the switch's row")
    header = (VK / "tex_warm.h").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="tex-warm-") as tmp:
        d = Path(tmp)
        exe, out = build(a.cxx, d, "header", HEADER_TEST, VK)
        if not exe:
            raise SystemExit("header: the build failed:\n" + out)
        r = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        if r.returncode:
            raise SystemExit("the header test failed:\n" + r.stdout)
        print(r.stdout.strip())
        for i, (what, old, new) in enumerate(MUTATIONS):
            if header.count(old) != 1:
                raise SystemExit("mutation %r: its text is not once in tex_warm.h (update the test with the header)" % what)
            inc = d / ("mutant%d" % i)
            inc.mkdir()
            (inc / "tex_warm.h").write_text(header.replace(old, new), encoding="utf-8")
            exe, out = build(a.cxx, d, "mutant%d" % i, HEADER_TEST, inc)
            if exe:
                r = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
                if r.returncode == 0:
                    raise SystemExit("mutation %r: the header test still passed without it" % what)
        print("PASS mutations: %d rules of tex_warm.h, each taken out, fail the header's test" % len(MUTATIONS))


if __name__ == "__main__":
    main()
