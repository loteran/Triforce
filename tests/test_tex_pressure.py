"""The texture budget follows the live VRAM use on cards over 8.5 GB, and a pass over it goes a step at a time
(gpu/vk/tex_pressure.h, gpu/vk/vk_texture.cpp texture_frame and texture_sweep; no game or GPU).

The budget was a size, min(70% of what the driver allows, 6144 MB) on a card the driver allows more than 8.5 GB. On the
owner's 32 GB card the Market after a tour holds 5.4-6.1 GB of textures, and a pass over the budget cut the cache to 7/8 of
it at once: batches of 134 to 645 textures every ~4 s for the first ~25 s, each made and uploaded again within seconds
(triforce-frame-stability.md; the third audit's P3). NX2navi (runtime/vk_texture.c TEX_VRAM_PRESSURE, sweep_pressure,
evict_idle, by the owner) has no size: idle textures go once the use less the free space in the blocks is past 80% of the
driver's budget, 64 a check, down to 2% under it.

What must hold, compiled and run (tests/tex_pressure_test.cpp, a fake heap): the rules one by one; the Market's arrival
after a tour on a 32 GB card takes nothing by live use where the fixed 6 GB cut hundreds at once; an arrival of 600 on a
12 GB card, another program filling the card and emptying it, the allowance falling, and a tour of ten areas on 12, 16 and
32 GB cards keep the use under the line (never past what the driver allows), at most 64 live textures a pass, nothing of
the area being played, and the only textures made again are those that went and are used again; the system's commit low
brings the fixed size back, in steps. Each rule of the header is then broken in turn in a copy and the checks must fail.

Read from the source: texture_frame decides the line once with the switches, sets it per look (not while a preset's budget
applies), checks the commit, puts the line before the 90% rule, looks again two presents on while a pass stopped at its
step, and the VRAM line keeps the lab's shape; texture_sweep stops at the budget itself by live use, skips live textures
past the step before anything else of the pass is done with them, counts only live ones, asks for the next look, waits two
seconds by live use, and every texture it takes still goes through the same end (its memory noted, so that one made again
is uploaded from the game's memory). Each of those is broken in turn in a copy of the source and the check must fail.

Keep taken (NX2navi's TEX_KEEP_TAKEN): a texture the game lets go and takes back waits 8 times its longest gap before the
let-go rule takes it early (Kakariko's haze, let go and taken back every 300 presents, goes once in 20 take-backs, not 20
times); texture_touch and texture_touch_quick note the gap, an early look before the parallel replay's segments does not.

The come-back pause (NX2navi's, for its commit drops): while half or more of 64 or more textures the line let go in 240
presents are made again within 600, the line stands aside 600 presents, doubled up to 7,200; in a thrash on a 12 GB card
(two sets that do not fit under the line together, wanted in turn) fewer than half as many are made again, the use under
90% of what the driver allows.

python -I -B tests/test_tex_pressure.py [--cxx g++]
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


def in_order(where, text, needles):
    at = [text.find(n) for n in needles]
    if -1 in at or at != sorted(at):
        raise SystemExit(where + ": not in this order: " + " / ".join(needles))


def check_source(tex, dev):
    need("vk_texture.cpp", tex, ['#include "tex_pressure.h"', "struct Press {", "bool commit_now(uint64_t* avail, uint64_t* limit) {",
                                 "*avail = ms.ullAvailPageFile, *limit = ms.ullTotalPageFile;"])
    frame = block(tex, "void texture_frame(uint64_t completed) {")
    need("vk_texture.cpp texture_frame", frame, [
        # (a look two presents on while a pass stopped at its step)
        "if (frame_no % 10 && !(press.next && frame_no >= press.next)) {", "    press.next = 0;",
        # (the size before, the line decided once with the switches, set per look)
        ": known                ? tex_pressure::fixed(allowed)",
        'press_pct = tex_pressure::pct_of(getenv("RX_VK_TEX_PRESSURE"));',
        "live_card = tex_pressure::live(press_pct, dev.memory_budget, allowed, integrated, vram_env);",
        "press.live = live_card > 0 && !preset_budget && known;",
        "press.step = tex_pressure::step_of(step_env, press.live);",
        "commit_low = want_mb && commit_now(&avail, &limit) && tex_pressure::commit_low(avail, limit, want_mb);",
        "press.acting = press.live && !press.held && tex_pressure::acts(pressure, allowed, press_pct, press.acting);",
        "if (press.live) budget = tex_pressure::budget(own, total_bytes, pressure, allowed, press_pct, commit_low, press.acting);",
        # (the VRAM line: the lab's scenario_report reads "textures N MB (budget N MB), N textures")
        'textures %llu MB (budget %llu "', '"MB), %zu textures%s%s"',
        "(press.live ? tex_pressure::shown(budget, total_bytes, pressure, allowed, press_pct) : budget) >> 20"])
    in_order("vk_texture.cpp texture_frame (the line after the preset's budget and the pressure, before the 90% rule and the pass)",
             frame, ["uint64_t budget = preset_texture_budget(", "const uint64_t pressure = ",
                     "if (press.live) budget = tex_pressure::budget(", "if (known && pressure > allowed / 10 * 9) {",
                     "texture_sweep(completed, total_bytes <= budget, budget,"])
    sweep = block(tex[tex.index("void texture_frame(uint64_t completed)"):], "void texture_sweep(uint64_t completed")
    need("vk_texture.cpp texture_sweep", sweep, [
        "const uint64_t need_unused = sweep || press.live ? std::max<uint64_t>(120, min_unused) : min_unused;",
        "if ((!sweep && !by_hold && tex_pressure::met(total_bytes, pending_bytes, budget, press.live)) || too_young(last))",
        "if (!sweep && !overwritten && tex_pressure::stepped(press.step, live_chosen)) continue;",
        "        if (!overwritten) live_chosen++;",
        "if (!sweep && tex_pressure::stepped(press.step, live_chosen)) press.next = frame_no + tex_pressure::kNextLook;"])
    if "budget / 8 * 7" in sweep:
        raise SystemExit("vk_texture.cpp texture_sweep cuts to 7/8 of the budget itself again (tex_pressure::cut_to decides)")
    # (the step is judged once it is known whether a render target drew over the texture, and before anything else of the
    # pass is done with it: the finalized ones' cap, the let-go count, the choice)
    in_order("vk_texture.cpp texture_sweep (the step)", sweep,
             ["const bool overwritten = shifted || drawn_over(tex);",
              "if (!sweep && !overwritten && tex_pressure::stepped(press.step, live_chosen)) continue;",
              "if (finalized_kept && keep_finalized(tex)) {", "if (gone_early) let_go_n++", "evict.push_back({tex,",
              "        if (!overwritten) live_chosen++;", "press.next = frame_no + tex_pressure::kNextLook;",
              "for (const Evict& e : evict) {   // (and each one's bookkeeping and its end, as before)"])
    # (every texture taken goes through the same end: one uploaded from memory is noted, so that made again it is uploaded)
    end = block(sweep, "for (const Evict& e : evict) {   // (and each one's bookkeeping and its end, as before)")
    need("vk_texture.cpp texture_sweep (each texture's end)", end,
         ["evicted_from_memory[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = tex->guest_size;",
          "destroy_later(*tex);", "forget_gpu(tex);", "gone.push_back(tex);"])
    need("docs/development.md", dev, ["`RX_VK_TEX_PRESSURE=0` or `=<pct>`", "`RX_VK_TEX_STEP=<n>`", "`RX_VK_TEX_COMMIT_FREE_MB=<MB>`"])
    # (keep taken: each look that takes a let-go texture back notes the gap, before it forgets the let-go; an early look
    # before the parallel replay's segments is no take-back; the let-go rule waits by it in both its places)
    taken = "if (const uint64_t g = tex->let_go) tex->back_gap = tex_pressure::taken_back(tex->back_gap, frame_no - g);"
    for name, head in (("texture_touch", "Texture* texture_touch(Texture* tex, bool for_sampling) {"),
                       ("texture_touch_quick", "bool texture_touch_quick(Texture* tex, bool for_sampling) {")):
        in_order("vk_texture.cpp " + name, block(tex, head), [taken, "tex->let_go = 0;"])
    need("vk_texture.cpp texture_reupload_early", block(tex, "void texture_reupload_early() {"),
         ["const uint32_t back_gap = t->back_gap;", "t->last_frame = last_frame, t->let_go = let_go, t->back_gap = back_gap;"])
    need("vk_texture.cpp texture_sweep (keep taken)", sweep, [
        'static const bool keep_taken = !(getenv("RX_VK_TEX_KEEP_TAKEN") && !atoi(getenv("RX_VK_TEX_KEEP_TAKEN")));',
        "return age60(frame_no - tex->let_go) >= tex_pressure::let_go_wait(let_go_after, age60(tex->back_gap), keep_taken);",
        "if (tex->let_go && !tex->gpu_written && let_go_due(tex)) mass += tex->bytes + tex->pad;",
        "const bool gone_early = let_go_short && tex->let_go && !tex->gpu_written && let_go_due(tex);"])
    need("gpu/vk/vk_texture.h", (ROOT / "gpu/vk/vk_texture.h").read_text(encoding="utf-8"), ["    uint32_t back_gap = 0;"])
    need("docs/development.md", dev, ["`RX_VK_TEX_KEEP_TAKEN=0`"])
    # (the come-back pause: what the line let go noted by the pass, not while it stands aside; a texture made found; the
    # judgement at each look, before the budget it sets aside)
    need("vk_texture.cpp", tex, ["tex_pressure::ComeBack come_back;", 'getenv("RX_VK_TEX_COME_BACK")'])
    need("vk_texture.cpp create", block(tex, "Texture* create(const Key& key"),
         ["if (come_back_noted.load(std::memory_order_relaxed)) {",
          "come_back.made(evicted_key(p->gpu, p->width, p->height, p->nvn_format), frame_no, tex_rate.rate);"])
    need("vk_texture.cpp texture_sweep (the come-back pause)", sweep, [
        "if (!sweep && !overwritten && press.live && !press.held && come_back_on())",
        "went_keys.push_back(evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format));",
        "for (uint64_t k : went_keys) come_back.went(k, frame_no);", "come_back_noted.store(true, std::memory_order_relaxed);"])
    in_order("vk_texture.cpp texture_frame (the come-back pause)", frame,
             ["press.held = false;", "press.held = come_back.held(frame_no, tex_rate.rate, &began);",
              "press.acting = press.live && !press.held && tex_pressure::acts(", "if (press.live) budget = tex_pressure::budget("])
    need("docs/development.md", dev, ["`RX_VK_TEX_COME_BACK=0`"])


SOURCE_MUTATIONS = [
    ("the line never stands aside", "press.acting = press.live && !press.held && tex_pressure::acts(",
     "press.acting = press.live && tex_pressure::acts("),
    ("what the line let go noted while it stands aside", "press.live && !press.held && come_back_on()", "press.live && come_back_on()"),
    ("a texture made again is not found",
     "        come_back.made(evicted_key(p->gpu, p->width, p->height, p->nvn_format), frame_no, tex_rate.rate);\n", ""),
    ("a take-back is not noted", "    if (const uint64_t g = tex->let_go) tex->back_gap = tex_pressure::taken_back(tex->back_gap, frame_no - g);"
     "\n    tex->let_go = 0;   // (in use: texture_sweep)", "    tex->let_go = 0;   // (in use: texture_sweep)"),
    ("a quick look's take-back is not noted",
     "    if (const uint64_t g = tex->let_go) tex->back_gap = tex_pressure::taken_back(tex->back_gap, frame_no - g);   // (texture_touch)\n", ""),
    ("an early look counts as a take-back", "t->let_go = let_go, t->back_gap = back_gap;", "t->let_go = let_go;"),
    ("the let-go rule waits its own 30 alone", "const bool gone_early = let_go_short && tex->let_go && !tex->gpu_written && let_go_due(tex);",
     "const bool gone_early = let_go_short && tex->let_go && !tex->gpu_written && age60(frame_no - tex->let_go) >= let_go_after;"),
    ("the line does not go on to 2% under it", "tex_pressure::acts(pressure, allowed, press_pct, press.acting);",
     "tex_pressure::acts(pressure, allowed, press_pct, false);"),
    ("the line is never applied", "if (press.live) budget = tex_pressure::budget(", "if (false) budget = tex_pressure::budget("),
    ("a pass by live use cuts to 7/8 again", "tex_pressure::met(total_bytes, pending_bytes, budget, press.live)",
     "(pending_bytes >= total_bytes || total_bytes - pending_bytes <= budget / 8 * 7)"),
    ("no step", "if (!sweep && !overwritten && tex_pressure::stepped(press.step, live_chosen)) continue;", ""),
    ("the dead targets count for the step", "        if (!overwritten) live_chosen++;", "        live_chosen++;"),
    ("no next look asked for", "press.next = frame_no + tex_pressure::kNextLook;", "(void)0;"),
    ("the next look is not taken", "if (frame_no % 10 && !(press.next && frame_no >= press.next)) {", "if (frame_no % 10) {"),
    ("the line over a preset's budget", "press.live = live_card > 0 && !preset_budget && known;", "press.live = live_card > 0 && known;"),
    ("a pass by live use takes what went unused a second", "sweep || press.live ? std::max<uint64_t>(120, min_unused)",
     "sweep ? std::max<uint64_t>(120, min_unused)"),
    ("the commit is not asked", "commit_low = want_mb && commit_now(", "commit_low = false && commit_now("),
    ("the lab's VRAM line changes shape", '"MB), %zu textures%s%s"', '"MB) for %zu textures%s%s"'),
    ("a texture taken is not noted as uploaded from memory", "evicted_from_memory[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = tex->guest_size;\n        }\n        if (remade_log_on()) {",
     "(void)0;\n        }\n        if (remade_log_on()) {"),
]

HEADER_MUTATIONS = [
    ("no 2% under the line", "return at - std::min(at, allowed / 50);", "return at;"),
    ("the line acts only past it", "(pressure > line(allowed, pct) || (acted && pressure > under(allowed, pct)))",
     "(pressure > line(allowed, pct))"),
    ("the budget whether the line acts or not", "if (acting && pressure > to) {", "if (pressure > to) {"),
    ("small cards by live use too", "return pct && driver_budget && allowed > kSmallCard && !integrated && !size_set;",
     "return pct && driver_budget && !integrated && !size_set;"),
    ("no driver budget needed", "return pct && driver_budget && allowed > kSmallCard && !integrated && !size_set;",
     "return pct && allowed > kSmallCard && !integrated && !size_set;"),
    ("a pass by live use cuts to 7/8", "return live ? budget : budget / 8 * 7;", "return budget / 8 * 7;"),
    ("never a step", "return step && live_chosen >= step;", "return false;"),
    ("no step by live use", "return live ? kStep : 0;", "return 0;"),
    ("a step of 256", "inline constexpr size_t kStep = 64;", "inline constexpr size_t kStep = 256;"),
    ("the next look ten presents on", "inline constexpr uint64_t kNextLook = 2;", "inline constexpr uint64_t kNextLook = 10;"),
    ("the commit low the other way round", "return avail < want;", "return avail >= want;"),
    ("the commit ignored", "uint64_t b = commit_low ? size : UINT64_MAX;", "uint64_t b = UINT64_MAX;"),
    ("the line ten points higher", "inline uint64_t line(uint64_t allowed, uint64_t pct) { return allowed / 100 * pct; }",
     "inline uint64_t line(uint64_t allowed, uint64_t pct) { return allowed / 100 * (pct + 10); }"),
    ("met wraps past the total", "return pending >= total || total - pending <= cut_to(budget, live);",
     "return total - pending <= cut_to(budget, live);"),
    ("the fixed size at 8 GB", "allowed <= kSmallCard ? 45 : 70), 6144ull << 20);", "allowed <= kSmallCard ? 45 : 70), 8192ull << 20);"),
    ("0 is not off", "return v <= 0 ? 0 :", "return v <= 0 ? kPct :"),
    ("keep taken waits the rule's own alone", "return on ? std::max(after, gap60 * kGapTimes) : after;", "return after;"),
    ("keep taken waits the gap once", "inline constexpr uint64_t kGapTimes = 8;", "inline constexpr uint64_t kGapTimes = 1;"),
    ("half back is not enough", "if (drops >= kMinDrops && backs * 2 >= drops) {", "if (drops >= kMinDrops && backs * 4 >= drops * 3) {"),
    ("the pause never doubles", "hold = hold ? std::min(hold * 2, kHoldMax) : kHoldMin;", "hold = kHoldMin;"),
    ("come back within 60", "kWithin = 600, kPeriod", "kWithin = 60, kPeriod"),
    ("the key is not compared", "if (s.key != key || !s.frame || now < s.frame", "if (!s.frame || now < s.frame"),
    ("the oldest noted wins the slot", "s.key = key, s.frame = now | 1;", "if (!s.frame) s.key = key, s.frame = now | 1;"),
    ("the pause never ends", "        if (holding && present_rate::at60(now - held_from, rate) >= hold) holding = false;\n", ""),
    ("the doubling never ends", "            hold = 0;\n", ""),
    ("the last gap, not the longest", "return (uint32_t)std::max<uint64_t>(gap, std::min<uint64_t>(presents, 1u << 24));",
     "return (uint32_t)std::min<uint64_t>(presents, 1u << 24);"),
]


def run(cxx, include, tmp, name):
    exe = Path(tmp) / name
    built = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(include), "-I", str(ROOT / "gpu/vk"),
                            str(ROOT / "tests/tex_pressure_test.cpp"), "-o", str(exe)], capture_output=True, text=True)
    if built.returncode:
        return False, built.stdout + built.stderr
    ran = subprocess.run([str(exe)], capture_output=True, text=True)
    return ran.returncode == 0, ran.stdout + ran.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check_source(tex, dev)
    for what, old, new in SOURCE_MUTATIONS:
        if tex.count(old) != 1:
            raise SystemExit("source mutation %r: its text is not once in vk_texture.cpp" % what)
        try:
            check_source(tex.replace(old, new), dev)
        except SystemExit:
            print("PASS: the source check fails when %s" % what)
            continue
        raise SystemExit("the source check passes when %s" % what)
    header = (ROOT / "gpu/vk/tex_pressure.h").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="tex-pressure-") as tmp:
        ok, out = run(args.cxx, ROOT / "gpu/vk", tmp, "tex-pressure")
        print(out, end="")
        if not ok:
            raise SystemExit("tex_pressure.h: the checks fail")
        for i, (what, old, new) in enumerate(HEADER_MUTATIONS):
            if header.count(old) != 1:
                raise SystemExit("header mutation %r: its text is not once in tex_pressure.h" % what)
            inc = Path(tmp) / ("m%d" % i)
            inc.mkdir()
            (inc / "tex_pressure.h").write_text(header.replace(old, new), encoding="utf-8")
            ok, out = run(args.cxx, inc, tmp, "tex-pressure-m%d" % i)
            if ok:
                raise SystemExit("the checks pass when %s" % what)
            print("PASS: the checks fail when %s" % what)
    print("PASS: the texture budget by live VRAM use")


if __name__ == "__main__":
    main()
