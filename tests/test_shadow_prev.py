"""The texture shadow's pages a capture takes again with bytes it holds already (gpu/vk/shadow_prev.h, vk_memory.cpp take_capture).
No game, no GPU.

After some tours of the lab's S2 the game rewrites the Market's textures every few frames: about half of the texture pages a
capture takes hold the bytes of their shadow copy (RX_VK_TEX_SAME) and the other half other bytes, which the next change takes
back. The shadow's loop copied all of them again, and a page copied with other bytes lost the hash a re-upload check had
kept with it, so the check after the change back hashed it again. Now (RX_VK_SHADOW_KEEP) a page with its copy's bytes keeps
the copy, and (RX_VK_SHADOW_PREV) a changed page keeps its copy before the change, with its hash, and gets it back, hash and
all, when the capture brings those bytes again: neither is copied.

What is checked:
1. tests/shadow_prev_test.cpp over the header: the shadow's loop modelled as take_capture runs it (tex_same's look, then the
   loop with the copies after it; checks hashing copies, GPU writes dropping them and a slot given other buffers between
   the two and between takes):
   - after every take each page holds the bytes of its last copy, each hash is of the bytes it is kept with or 0, the kept
     copies are counted and never more than the cap (random, against_old);
   - against the old loop step by step: the same bytes always, and with both switches 0 the same copies and hashes and nothing
     kept (against_old, with each switch 0 and both);
   - the gains: A, B, A, B copied twice in 20 takes and given its hash back; A, A, B, B; bytes that differ in their last
     byte only are other bytes; a copy that never comes back goes after kMisses changes and none is kept for kRetry; the cap;
     A, B, A, C, A, D given A back every time, its Prev never let go; a page changed once and then taken the same lets its
     copy go after kStale takes and the cap takes others' again; one changed once and not taken again lets it go at the
     sweep (every kSweep takes); a page the capture takes twice copied as before (gains);
   - the switch's rule (unset or not 0: on), a Prev's count and its age (env).
2. vk_memory.cpp: the look in tex_same's loop, after the copy is ready, the looks sized only at the first page with a copy
   (a take with none costs what it did before: the loop's steps gated on sh_any); the sweep that ages the kept copies; the
   loop's steps where the model has them (the pages left before a slot is taken with operator[], the copy kept before the hash
   is let go and the copy queued, a Prev not given back aged); the header's steps the only writers of a Prev; the switches'
   names, and their rows in docs/development.md.

    python -I -B tests/test_shadow_prev.py [--cxx g++]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def check(cond, why):
    if not cond:
        print("FAIL: " + why)
        sys.exit(1)


def block(text, head):
    start = text.find(head)
    check(start >= 0, "%s is missing" % head)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def in_order(text, steps, what):
    at = 0
    for s in steps:
        i = text.find(s, at)
        check(i >= 0, "%s: %r is missing or out of order" % (what, s))
        at = i + len(s)


def check_source():
    mem = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
    check('#include "shadow_prev.h"' in mem, "vk_memory.cpp does not include shadow_prev.h")
    page = block(mem, "struct ShadowPage {", )
    in_order(page, ["std::unique_ptr<uint8_t[]> b;", "shadow_prev::Vers prev;", "uint64_t hash = 0;"], "ShadowPage")
    take = block(mem, "bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed) {")
    in_order(take, [
        # (the switches, the looks of this take only)
        'shadow_prev::env_on("RX_VK_SHADOW_KEEP")', 'shadow_prev::env_on("RX_VK_SHADOW_PREV")',
        "static const bool sh_on = sh_keep || sh_prev;", "shadow_prev::env_vers()", "static const shadow_prev::Vers sh_none{};",
        "sh_looks.clear();", "size_t sh_any = 0;",
        # (tex_same: a look for each page compared, by its index in c->copied, after its copy is ready; the looks sized at the
        # first page with a copy, sh_any counting them)
        "std::lock_guard sk(shadow_mu);",
        "if (!dropped.empty() && dropped[c->slots[i]]) continue;", "if (s == shadow.end() || !s->second.b) continue;",
        "cap_ready(*c, c->slots[i]);",
        "if (sh_on && !sh_any++) sh_looks.assign(c->copied.size(), shadow_prev::Look{});",
        "if (memcmp(s->second.b.get(), (const uint8_t*)c->map + (uint64_t)c->slots[i] * 4096, 4096) != 0) {",
        "if (sh_on)", "sh_looks[i] = shadow_prev::look(false, s->second.b.get(), s->second.prev, cap_page_bytes(*c, c->slots[i]), sh_prev,",
        "sh_vers);", "continue;", "same_pages.push_back({p, gens.page_tex(p)});",
        "if (sh_on) sh_looks[i] = shadow_prev::look(true, s->second.b.get(), s->second.prev, nullptr, sh_prev, sh_vers);",
        # (the sweep: every kSweep captures while any is kept, every kept copy aged, the seq sweep's walk as it was)
        "shadow_seq++;",
        "const bool seq_sweep = !sweep_off && shadow_seq % 256 == 0 && shadow_n.load(std::memory_order_relaxed) > 8192;",
        "const bool prev_sweep = shadow_seq % shadow_prev::kSweep == 0 && shadow_prev::alive().load(std::memory_order_relaxed) > 0;",
        "if (seq_sweep || prev_sweep)", "if (!seq_sweep || it->second.seq + 256 >= shadow_seq) {",
        "if (prev_sweep && shadow_prev::age(it->second.prev, shadow_seq)) st_sh_stale++;", "++it;", "continue;",
        "it = shadow.erase(it);",
        # (the shadow's loop: the pages of this capture, for one taken twice, only when a look was kept)
        "std::sort(same_set.begin(), same_set.end());", "if (sh_any) {", "sh_pages.push_back(p);",
        "std::sort(sh_pages.begin(), sh_pages.end());",
        "for (; i < c->copied.size(); i++) {", "if (!in_tex_merged((uint64_t)p << 12)) continue;",
        "if (shadow_n.load(std::memory_order_relaxed) >= kShadowMax && !shadow.count(p)) {",
        # (left: before the slot is taken, as the slot is now)
        "if (sh_any && sh_looks[i].v != shadow_prev::NONE) {", "const auto s = shadow.find(p);",
        "sh_act = shadow_prev::act(sh_looks[i], sh_keep, sh_prev, shadow_prev::twice(sh_pages, p), sp ? sp->b.get() : nullptr,",
        "sp ? sp->prev : sh_none);",
        "if (sh_act == shadow_prev::SAME || sh_act == shadow_prev::BACK) {",
        "if (sh_act == shadow_prev::BACK)", "shadow_prev::back(sp->b, sp->hash, sp->prev, sh_looks[i].depth, shadow_seq), st_sh_back++,",
        "else st_sh_kept++, st_sh_stale += shadow_prev::age(sp->prev, shadow_seq);",
        "sp->seq = shadow_seq;", "continue;",
        # (copied: the copy kept before its hash is let go and the copy queued into the slot's buffer; else its Prev aged)
        "ShadowPage& slot = shadow[p];", "slot.seq = shadow_seq;",
        "if (sh_act == shadow_prev::CHANGED &&",
        "shadow_prev::keep_old(slot.b, slot.hash, slot.prev, shadow_prev::kCapPages, shadow_seq, sh_vers)) {",
        "} else if (sh_any && shadow_prev::age(slot.prev, shadow_seq)) {",
        "if (slot.hash) {", "else slot.hash = 0;", "pairs.push_back({slot.b.get(), (const uint8_t*)c->stage.get()",
        "copy_pairs(pairs.data(), (int64_t)pairs.size());",
        # (RX_VK_SHADOW_CHECK: after the copies)
        "for (const auto& [sp, slot_i] : sh_left) {", "cap_page_bytes(*c, slot_i)", "if (sh_any) {",
    ], "take_capture")
    # (a take with no look kept, most of them, costs what it did before: the looks sized once, at the first page with a copy,
    # and the shadow's loop asks sh_any before anything of the looks)
    check(mem.count("sh_looks.assign(") == 1, "vk_memory.cpp: the looks sized elsewhere than at the first page with a copy")
    loop = take[take.index("std::sort(same_set.begin(), same_set.end());"):take.index("copy_pairs(pairs.data(), (int64_t)pairs.size());")]
    # (the guard, the act, and BACK's depth twice: behind the same sh_any test)
    check("sh_looks.empty()" not in loop and loop.count("sh_looks[i]") == 4 and "if (sh_any && sh_looks[i].v" in loop,
          "take_capture's shadow loop reads the looks without asking sh_any first")
    left = take[take.index("if (sh_act == shadow_prev::SAME || sh_act == shadow_prev::BACK) {"):]
    left = left[:left.index("ShadowPage& slot = shadow[p];")]
    check(left.count("continue;") == 1 and "pairs.push_back" not in left, "take_capture: a page left is copied all the same")
    # (the header's steps are the only writers of a Prev and of the buffers they swap)
    for needle, n in (("shadow_prev::back(", 1), ("shadow_prev::keep_old(", 1), ("shadow_prev::look(", 2), ("shadow_prev::act(", 1),
                      ("shadow_prev::age(", 3), ("shadow_prev::holds_all(", 1), ("shadow_prev::Vers prev;", 1)):
        check(mem.count(needle) == n, "vk_memory.cpp: %s %d times, not %d" % (needle, mem.count(needle), n))
    for needle in ("prev.b =", "prev.b.reset", "prev.hash =", ".prev = ", "->prev = ", "prev.b.swap", "std::swap(slot.b", "std::swap(sp->b",
                   "prev.p[", "prev.n =", "prev.misses =", "prev.n++", "prev.n--"):
        check(needle not in mem, "vk_memory.cpp: a Prev or a copy's buffer changed outside shadow_prev.h: " + needle)
    # (the page's bytes where the shadow's copy reads them: the staging when it holds the page, else the buffer)
    src = block(mem, "inline const uint8_t* cap_page_bytes(const Capture& c, uint32_t slot) {")
    check("c.sidx.empty() ? -1 : c.sidx[slot]" in src and "c.stage.get() + (uint64_t)sx * 4096" in src and
          "c.map + (uint64_t)slot * 4096" in src, "cap_page_bytes is not where the shadow's copies read the page")
    hdr = (VK / "shadow_prev.h").read_text(encoding="utf-8")
    check("inline constexpr int64_t kCapPages = 16384;" in hdr, "shadow_prev.h: the cap is not 64 MB")
    check("inline constexpr uint64_t kStale = 64;" in hdr and "inline constexpr uint64_t kSweep = 256;" in hdr,
          "shadow_prev.h: a kept copy is not let go 64 takes after it was last given back, or the sweep is not every 256")
    check("inline constexpr int kMaxVers = 4;" in hdr and "inline constexpr int kVersDefault = 1;" in hdr and
          "RX_VK_SHADOW_VERS" in hdr, "shadow_prev.h: the versions' range, their default or their switch is not as designed")
    # (act compares the version by its take too: a buffer that left the ring and came back holds the same pointer)
    check("v_now.p[l.depth].at == l.at" in hdr, "shadow_prev.h: act compares a kept version by its buffer alone")
    check('getenv("RX_VK_SHADOW_CHECK") && atoi(getenv("RX_VK_SHADOW_CHECK")) != 0' in take,
          "take_capture no longer reads RX_VK_SHADOW_CHECK")
    guide = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
    for row in ("`RX_VK_SHADOW_KEEP=0`", "`RX_VK_SHADOW_PREV=0`", "`RX_VK_SHADOW_CHECK=1`", "`RX_VK_SHADOW_VERS`"):
        check(row in guide, "docs/development.md does not say what %s does" % row)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--header", default=str(VK / "shadow_prev.h"), help="(the mutation check: a changed copy of the header)")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="shadow-prev-") as tmp:
        inc = Path(tmp) / "inc"
        inc.mkdir()
        (inc / "shadow_prev.h").write_text(Path(args.header).read_text(encoding="utf-8"), encoding="utf-8")
        exe = Path(tmp) / "fixture.exe"
        built = subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                                "-I", str(inc), "-I", str(VK), str(ROOT / "tests" / "shadow_prev_test.cpp"), "-o", str(exe)],
                               capture_output=True, text=True)
        check(built.returncode == 0, "the fixture did not build:\n" + built.stderr[-4000:])
        env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_")}
        runs = [(c, {}) for c in ("env", "gains", "random", "against_old")]
        runs += [("against_old", {"RX_VK_SHADOW_KEEP": "0", "RX_VK_SHADOW_PREV": "0"}), ("against_old", {"RX_VK_SHADOW_KEEP": "0"}),
                 ("against_old", {"RX_VK_SHADOW_PREV": "0"}), ("random", {"RX_VK_SHADOW_PREV": "2"}),
                 ("random", {"RX_VK_SHADOW_VERS": "2"}), ("random", {"RX_VK_SHADOW_VERS": "4"}),
                 ("against_old", {"RX_VK_SHADOW_VERS": "3"})]
        for case, extra in runs:
            r = subprocess.run([str(exe), case], env=dict(env, **extra), capture_output=True, text=True, timeout=300)
            tag = " ".join("%s=%s" % kv for kv in extra.items())
            print((r.stdout + r.stderr).strip() + (" (%s)" % tag if tag else ""))
            check(r.returncode == 0, "shadow_prev_test %s %s" % (case, tag))
    print("PASS: a texture page a capture takes with its copy's bytes, or with the bytes of one of the copies before the last "
          "changes (RX_VK_SHADOW_VERS), is not copied into the shadow again and keeps its hash; every page holds what the capture "
          "took; RX_VK_SHADOW_KEEP=0 RX_VK_SHADOW_PREV=0 as before")


if __name__ == "__main__":
    main()
