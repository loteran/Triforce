"""The pipeline cache's file carried over a stamp change at an update's preparation, capped at 1.25 x its base
(gpu/vk/pcache_carry.h; vk_draw.cpp pcache_init, pipeline_cache_save, pcache_carry_rebuild; vk_main.cpp; no game/GPU).

The header is compiled and run as it is: the cap, the history beside the file (its side file only when written for the
file's own stamp, else one fresh set of the file's size), when a file for other shaders is carried (an update's
preparation, the switch on, within the bound, at most the cap), the session's history (fresh, the file's own, one carry
more), which saves a carried session writes, how the base follows the saves of a file never carried into and stays put
once carried into (so the cap is cumulative over the carries, not a quarter more each time), and the side file's bytes.
What a carried cache may hold (the cap, or the file's bound where lower) and the refusal kept for these shaders
(<file>.nocarry). Then the source: pcache_init decides the carry before the bounded read and still checks the device's
header after it, pipeline_cache_save asks the carry's limit before the bound and before it fetches the data and writes
the history with the .for, the preparation looks at a carried cache's size while it builds and stops building into it
past its limit, and builds again into a fresh cache: the carried run's compute pipelines from the program cache, then the
stage libraries, nothing translated again (vk_draw.cpp's carry part itself is run by tests/test_pipeline_cache_save.py,
cases carry-cap, carry-bound, carry-check and carry-early). The early drop: the projection of a carried cache's growth over a
sample of the stage libraries' whole order (every 32nd program, built first), and when it drops the carry before it gets past
(the sample, the margin), with the RX 7900 XT's update of 11-18 (dropped), a carry whose misses are all in the order's head,
the notes' programs (not: a projection from the head would have dropped it), and a mostly-hit carry (not); warm_libraries
builds the sample first, the others waiting for its projection, before the look past the limit, and a cache dropped so is
not written.
python -I -B tests/test_pcache_carry.py --cxx clang++
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cstdio>
#include <cstring>
#include <vector>
#include "pcache_carry.h"
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); return 1; } } while (0)
using namespace pcache_carry;
int main() {
    const uint64_t MB = 1ull << 20;
    // (the switch: on unless 0)
    CHECK(on(nullptr) && on("1") && !on("0"));
    // (the cap: 1.25 x the base, never wrapping)
    CHECK(cap(1000) == 1250 && cap(2048 * MB) == 2560 * MB && cap(0) == 0 && cap(~0ull) == ~0ull);
    CHECK(fits(1250, 1000) && !fits(1251, 1000) && fits(0, 0) && !fits(1, 0));

    const char* A = "aaaaaaaaaaaaaaaa";
    const char* B = "bbbbbbbbbbbbbbbb";
    Side side;
    std::memcpy(side.stamp, A, 16);
    side.base = 1000, side.carried = 2;
    // (the history: the side file only when it was written for the file's own stamp; else one fresh set of the file's size)
    Side h = history(true, side, A, 1200);
    CHECK(h.base == 1000 && h.carried == 2);
    h = history(true, side, B, 1200);
    CHECK(h.base == 1200 && h.carried == 0 && !std::memcmp(h.stamp, B, 16));
    h = history(false, side, A, 1200);
    CHECK(h.base == 1200 && h.carried == 0);
    h = history(true, side, nullptr, 1200);   // (no .for beside the file)
    CHECK(h.base == 1200 && h.carried == 0);
    Side zero = side;
    zero.base = 0;
    CHECK(history(true, zero, A, 700).base == 700);

    // (a file for other shaders is carried only at an update's preparation, with the switch on, within the bound, at most the
    // cap of its history; nothing of a header's size or less)
    CHECK(may_carry(true, true, false, true, 1250, side));
    CHECK(!may_carry(true, true, false, true, 1251, side));
    CHECK(!may_carry(false, true, false, true, 1100, side));
    CHECK(!may_carry(true, false, false, true, 1100, side));
    CHECK(!may_carry(true, true, false, false, 1100, side));
    CHECK(!may_carry(true, true, false, true, 32, history(false, side, A, 32)));
    CHECK(may_carry(true, true, false, true, 1200, history(false, side, A, 1200)));   // (a file of before this rule: one set)
    CHECK(!may_carry(true, true, false, true, 1100, zero));
    // (refused for these shaders already, <file>.nocarry: a preparation begun again after a kill starts fresh)
    CHECK(!may_carry(true, true, true, true, 1100, side));
    // ((11-19) v0.5.2's carry outside the preparation: asked for a plain start only; not when .nocarry names these shaders
    // and the file has no .for (warm_libraries dropped it: read again it loops, over the bound and never written), nor for a
    // carried save cut short, nor at the preparation, nor for a file read as carried or resumed already)
    CHECK(plain_carry(false, false, false, false, false));
    CHECK(!plain_carry(false, false, false, true, false));
    CHECK(!plain_carry(false, false, false, false, true));
    CHECK(!plain_carry(true, false, false, false, false));
    CHECK(!plain_carry(false, true, false, false, false) && !plain_carry(false, false, true, false, false));

    // (the session: fresh is nothing; the file's own keeps its history; carried counts one carry more)
    CHECK(session(Start::Fresh, side).base == 0 && session(Start::Fresh, side).carried == 0);
    CHECK(session(Start::Same, side).base == 1000 && session(Start::Same, side).carried == 2);
    CHECK(session(Start::Carried, side).base == 1000 && session(Start::Carried, side).carried == 3);

    // (a carried session's saves only up to the cap; any other session's as before)
    const Side carried = session(Start::Carried, side);
    CHECK(save_ok(true, 1250, carried, 0) && !save_ok(true, 1251, carried, 0));
    CHECK(save_ok(false, 5000, carried, 0) && save_ok(false, 5000, Side{}, 0));

    // (what a carried cache may hold: the cap, or the file's bound where that is lower; 0 is no bound)
    CHECK(limit(1000, 0) == 1250 && limit(1000, 1100) == 1100 && limit(1000, 2000) == 1250 && limit(1000, 1250) == 1250);
    // (over the bound and under the cap is past what it may hold: never left over the bound unwritten, with no rebuild)
    CHECK(over(true, 1101, carried, 1100) && !over(true, 1100, carried, 1100) && !save_ok(true, 1101, carried, 1100));
    CHECK(over(true, 1251, carried, 4000) && !over(true, 1250, carried, 4000));
    // (a session not carried: the bound's own rule as before, never over here)
    CHECK(!over(false, 1u << 30, carried, 1100) && save_ok(false, 1u << 30, carried, 1100));
    // (2 GB bound, a 1.8 GB file carried: past 2 GB is past, under its 2.25 GB cap)
    Side big;
    big.base = 1843 * MB;
    CHECK(fits(1843 * MB, big.base) && over(true, 2049 * MB, big, 2048 * MB) && !over(true, 2048 * MB, big, 2048 * MB));
    CHECK(kCheckMs > 0 && kCheckMs <= 60000);

    // (the base follows the saves of a file never carried into, and stays put once carried into)
    Side s = saved(Side{}, 900, B);
    CHECK(s.base == 900 && s.carried == 0 && !std::memcmp(s.stamp, B, 16));
    s = saved(s, 1000, B);
    CHECK(s.base == 1000);
    s = saved(s, 800, B);
    CHECK(s.base == 1000);
    Side c = saved(carried, 1200, B);
    CHECK(c.base == 1000 && c.carried == 3 && !std::memcmp(c.stamp, B, 16));

    // (cumulative: a set of 1000 carried at 1100, then at 1250, never past 1250 whatever the updates; refused, a fresh set
    // starts again with its own base)
    Side f = saved(Side{}, 1000, A);
    Side one = saved(session(Start::Carried, history(true, f, A, 1000)), 1100, B);
    CHECK(one.base == 1000 && one.carried == 1);
    CHECK(may_carry(true, true, false, true, 1100, history(true, one, B, 1100)));
    Side two = saved(session(Start::Carried, history(true, one, B, 1100)), 1250, A);
    CHECK(two.base == 1000 && two.carried == 2);
    CHECK(may_carry(true, true, false, true, 1250, history(true, two, A, 1250)));
    CHECK(!save_ok(true, 1300, session(Start::Carried, history(true, two, A, 1250)), 0));
    const Side play = saved(session(Start::Same, history(true, two, A, 1250)), 1300, A);   // (play's growth on a carried file)
    CHECK(play.base == 1000 && !may_carry(true, true, false, true, 1300, history(true, play, A, 1300)));
    const Side fresh = saved(session(Start::Fresh, two), 1200, B);
    CHECK(fresh.base == 1200 && fresh.carried == 0);

    // (open: a carried session's history until its preparation goes through to its end; the file's own keeps it, its saves
    // too, and a fresh one is not)
    CHECK(session(Start::Carried, side).open == 1 && session(Start::Fresh, two).open == 0);
    CHECK(two.open == 1 && session(Start::Same, two).open == 1 && saved(session(Start::Same, two), 1250, A).open == 1);
    CHECK(saved(session(Start::Fresh, two), 1200, B).open == 0);
    CHECK(history(true, two, A, 1250).open == 1 && history(true, two, B, 1250).open == 0 && history(false, two, A, 1250).open == 0);
    // (resumed: a file for these shaders whose history is open, at an update's preparation with the switch on, is read as
    // carried; stopped at 2.9 GB of a 2.6 GB base, it is capped at 3.25 GB as the carried run was)
    CHECK(resumed(true, true, true, two) && !resumed(false, true, true, two) && !resumed(true, false, true, two));
    CHECK(!resumed(true, true, false, two) && !resumed(true, true, true, fresh));
    const Side stopped = saved(session(Start::Carried, history(true, saved(Side{}, 2600 * MB, A), A, 2600 * MB)), 2900 * MB, B);
    CHECK(stopped.open == 1 && stopped.base == 2600 * MB && stopped.carried == 1);
    const Side again = session(Start::Same, history(true, stopped, B, 2900 * MB));
    CHECK(resumed(true, true, true, history(true, stopped, B, 2900 * MB)) && again.carried == 1 && again.open == 1);
    CHECK(!over(true, 3250 * MB, again, 4096 * MB) && over(true, 3251 * MB, again, 4096 * MB));
    Side done = again;
    done.open = 0;
    CHECK(!resumed(true, true, true, done));
    // (begun: a carried save cut before its .for, the side file written first with these shaders' stamp and open. The .for
    // names the old stamp, or there is none: that session's history, read as resumed. Not when the .for names these shaders
    // (the history's own rule), the side file is shut, another stamp's, or has no base)
    CHECK(begun(true, stopped, B, A) && begun(true, stopped, B, nullptr) && !begun(true, stopped, B, B));
    CHECK(!begun(false, stopped, B, A) && !begun(true, done, B, A) && !begun(true, stopped, A, B));
    Side nobase = stopped;
    nobase.base = 0;
    CHECK(!begun(true, nobase, B, A) && resumed(true, true, true, stopped) && history(true, stopped, A, 2600 * MB).open == 0);

    // (the side file's bytes: as written, read back; another magic or a short file is none)
    uint8_t raw[kSideBytes];
    put(two, raw);
    Side back;
    CHECK(get(raw, sizeof raw, &back) && back.base == two.base && back.carried == two.carried && !std::memcmp(back.stamp, A, 16));
    CHECK(back.open == two.open);
    Side shut = two;
    shut.open = 0;
    put(shut, raw);
    CHECK(get(raw, sizeof raw, &back) && back.open == 0);
    put(two, raw);
    CHECK(!get(raw, sizeof raw - 1, &back));
    raw[0] ^= 1;
    CHECK(!get(raw, sizeof raw, &back));

    // (the early drop: the switch, on unless 0)
    CHECK(project_on(nullptr) && project_on("1") && !project_on("0"));
    CHECK(kProjectLibs == 1500 && kSampleStride == 32 && kProjectNum == 2 * kProjectDen);
    // (the sample's order: every 32nd program of the warm's order first, in their order, then the others in theirs; every
    // program once)
    CHECK(sample_count(0) == 0 && sample_count(1) == 1 && sample_count(32) == 1 && sample_count(33) == 2);
    CHECK(sample_count(42532) == 1330);
    const uint64_t totals[] = {1, 2, 31, 32, 33, 63, 64, 65, 1000, 42532};
    for (uint64_t t : totals) {
        std::vector<char> seen(t, 0);
        const uint64_t m = sample_count(t);
        uint64_t prev = 0;
        for (uint64_t c = 0; c < t; c++) {
            const uint64_t i = sample_order(c, t);
            CHECK(i < t && !seen[i]);
            seen[i] = 1;
            CHECK((c < m) == (i % kSampleStride == 0));
            CHECK(!c || c == m || i > prev);
            prev = i;
        }
    }
    CHECK(sample_order(0, 42532) == 0 && sample_order(1329, 42532) == 42528 && sample_order(1330, 42532) == 1);
    CHECK(sample_order(1330 + 30, 42532) == 31 && sample_order(1330 + 31, 42532) == 33 && sample_order(42531, 42532) == 42531);
    // (the projection: the growth per program done times the programs left, on top of what it holds; nothing grown, nothing
    // done or nothing left: what it holds)
    CHECK(projected(1000, 900, 10, 100) == 1900 && projected(1000, 900, 50, 100) == 1100);
    CHECK(projected(900, 900, 10, 100) == 900 && projected(800, 900, 10, 100) == 800);
    CHECK(projected(1000, 900, 0, 100) == 1000 && projected(1000, 900, 100, 100) == 1000);
    CHECK(projected(~0ull - 1, 0, 1, ~0ull) == ~0ull);   // (never wrapping)
    // (the margin: the projected growth at least twice the room at the start, 100 from 900 under 1000)
    CHECK(drop_early(950, 900, 1, 4, 1500, 1000) && !drop_early(949, 900, 1, 4, 1500, 1000));
    // (the sample: 1500 stage libraries at least)
    CHECK(!drop_early(950, 900, 1, 4, 1499, 1000));
    // (no room at the start, no growth, nothing left: never on the projection; over says it when past)
    CHECK(!drop_early(1100, 1000, 1, 4, 1500, 1000) && !drop_early(900, 900, 1, 4, 1500, 1000));
    CHECK(!drop_early(1000, 900, 4, 4, 1500, 1000));

    // (the warm as warm_libraries takes it with a carried cache: its sample built first, in sample_order, with libs_each
    // stage libraries a program, then its projection, once; and where the whole warm ends. grow(i): what program i of the
    // warm's order adds to the cache)
    const auto sampled = [](uint64_t from, uint64_t total, uint64_t lim_, uint64_t libs_each, auto grow) {
        const uint64_t m = sample_count(total);
        uint64_t now = from;
        for (uint64_t c = 0; c < m; c++) now += grow(sample_order(c, total));
        return drop_early(now, from, m, total, m * libs_each, lim_);
    };
    const auto ends = [](uint64_t from, uint64_t total, auto grow) {
        uint64_t now = from;
        for (uint64_t i = 0; i < total; i++) now += grow(i);
        return now;
    };

    // (the RX 7900 XT's update of 11-18: base 2021349 KB (1973 MB), its limit 2467 MB under the file's bound of 4086 MB; the
    // cache at 2469 MB after 36,906 of the 91,632 stage libraries, 16,018 of 42,532 programs: 496 MB for those, nearly every
    // library a miss. Past, and over its margin at that rate)
    const uint64_t KB = 1024;
    Side rx;
    rx.base = 2021349 * KB;
    const uint64_t lim = limit(rx.base, 4086 * MB);
    CHECK(lim >> 20 == 2467);
    CHECK(over(true, 2469 * MB, rx, 4086 * MB));
    CHECK(drop_early(2469 * MB, rx.base, 16018, 42532, 36906, lim));
    CHECK(projected(2469 * MB, rx.base, 16018, 42532) >> 20 > 3250);   // (some 3290 MB at that rate)
    const uint64_t T = 42532, named = 9420;   // (its programs, and those a state list loaded or the notes name, first)
    const uint64_t miss = 496 * MB / 16018;   // (that run's growth per program)
    const auto all = [&](uint64_t) -> uint64_t { return miss; };
    // (every program a miss: past its limit by the warm's end, and dropped once its sample is built, 1,330 programs, some
    // 2,900 libraries: about four minutes at that run's 12-15 a second, rather than after 34)
    CHECK(ends(rx.base, T, all) > lim && sampled(rx.base, T, lim, 2, all));
    // (the head alone a miss, the rest hits: it would end at some 2266 MB, under its limit, and is not dropped. A projection
    // from the head, as the warm takes it, would have dropped it at the first look past 1500 libraries, 709 programs at 120 s
    // in that run: 4.5 times its growth, and the ~70,000 libraries that hit compiled cold into the fresh cache)
    const auto head = [&](uint64_t i) -> uint64_t { return i < named ? miss : 0; };
    CHECK(ends(rx.base, T, head) <= lim && !sampled(rx.base, T, lim, 2, head));
    CHECK(drop_early(rx.base + 709 * miss, rx.base, 709, T, 1517, lim));
    // (the rest a miss, the head hits: past its limit by the end, dropped)
    const auto tail = [&](uint64_t i) -> uint64_t { return i < named ? 0 : miss; };
    CHECK(ends(rx.base, T, tail) > lim && sampled(rx.base, T, lim, 2, tail));
    // (a stretch of hits as that run had, the rest misses: 6,000 programs from 6,000, dropped. 12,000 from 6,000 ends past
    // its limit too, but its growth is under the margin: left to the look past the limit, as before the early drop)
    const auto stretch = [&](uint64_t i) -> uint64_t { return i >= 6000 && i < 12000 ? 0 : miss; };
    CHECK(ends(rx.base, T, stretch) > lim && sampled(rx.base, T, lim, 2, stretch));
    const auto longer = [&](uint64_t i) -> uint64_t { return i >= 6000 && i < 18000 ? 0 : miss; };
    CHECK(ends(rx.base, T, longer) > lim && !sampled(rx.base, T, lim, 2, longer));
    // (a mostly-hit carry: 5% of its base over the whole warm, 99 MB. Not dropped, at an even rate, nor with all of it in the
    // head)
    const uint64_t whole = rx.base / 20;
    CHECK(!sampled(rx.base, T, lim, 2, [&](uint64_t) -> uint64_t { return whole / T; }));
    CHECK(!sampled(rx.base, T, lim, 2, [&](uint64_t i) -> uint64_t { return i < named ? whole / named : 0; }));
    // (a later carry with little room: 60 MB, and 45 MB of growth all in the head. It fits, and is not dropped; from the head it
    // would have been)
    Side later;
    later.base = 2000 * MB;
    const uint64_t lim2 = limit(later.base, 0), from2 = lim2 - 60 * MB;
    const auto small = [&](uint64_t i) -> uint64_t { return i < named ? 45 * MB / named : 0; };
    CHECK(ends(from2, T, small) <= lim2 && !sampled(from2, T, lim2, 2, small));
    CHECK(drop_early(from2 + 709 * (45 * MB / named), from2, 709, T, 1517, lim2));
    // (a warm too small for its sample to hold 1500 libraries: only past its limit)
    CHECK(!sampled(rx.base, 20000, lim, 2, all));
    return 0;
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def need(where, text, parts):
    for p in parts:
        if p not in text:
            raise SystemExit(f"{where}: missing {p!r}")


def in_order(where, text, parts):
    at = -1
    for p in parts:
        i = text.find(p, at + 1)
        if i < 0:
            raise SystemExit(f"{where}: {p!r} missing or out of order")
        at = i


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    init = between(draw, "void pcache_init() {", "\n}\n")
    # (decided before the bounded read, by the header's rule, at Setup's preparation only, with the switch)
    in_order("vk_draw.cpp pcache_init", init, [
        'fopen((pcache_path + ".base").c_str(), "rb")',
        "pcache_bound_bytes = pcache_bound::bytes(",
        # (a carried save cut before its .for: that session's history, pcache_carry::begun)
        "pcache_carry::begun(side_read, side_had, pcache_for, for_read ? had_for : nullptr);",
        "history = begun ? side_had",
        "pcache_carry::history(",
        "pcache_carried = !pcache_same && !begun && pcache_carry::may_carry(pcache_carry::on(getenv(\"RX_VK_PCACHE_CARRY\"))",
        'getenv("HOOKSHOT_PREPARE_ONLY") != nullptr',
        "pcache_bound::within((uint64_t)sz, pcache_bound_bytes)",
        # (these shaders' file whose carried preparation was stopped: carried still, capped; refused by .nocarry, not read)
        "pcache_carry::resumed(pcache_carry::on(getenv(\"RX_VK_PCACHE_CARRY\")), getenv(\"HOOKSHOT_PREPARE_ONLY\") != nullptr,",
        "pcache_same || begun, history);",
        "if (resumed && !carry_refused) pcache_carried = true;",
        # ((Triforce) v0.5.2's carry by the file's size and the RAM (pcache_bound.h carried), outside Setup's preparation;
        # (11-19) not over .nocarry (a file warm_libraries dropped) nor for a carried save cut short
        "carried = pcache_carry::plain_carry(getenv(\"HOOKSHOT_PREPARE_ONLY\") != nullptr, pcache_carried, resumed, carry_refused, begun) &&",
        "pcache_bound::carried(pcache_same,",
        "if (resumed && carry_refused) {",
        "} else if (!pcache_same && !pcache_carried && !carried) {",
        "pcache_bound::read(pcache_path.c_str(), 32, pcache_bound_bytes, data, &len)",
        # (the device's header is checked on a carried file as on any: another device's or driver's data is dropped)
        "dev.props.pipelineCacheUUID",
        "data.clear();",
        "vkCreatePipelineCache(dev.device, &ci, nullptr, &pcache)",
        "if (!with_data) pcache_carried = false;",
        "pcache_carried && !resumed ? Start::Carried : Start::Same;",
        "pcache_side = pcache_carry::session(how, history);",
        "if (with_data && carried) pcache_side.carried++;",
        'logf("vk: pipeline cache carried over a stamp change (%zu MB, base %llu MB)',
        "pcache_play_open(pcache_same);",
    ])
    # (the play file keeps its own rule: for one stamp)
    if "pcache_play_open(pcache_same || pcache_carried)" in init:
        raise SystemExit("the play file is carried too")
    need("vk_draw.cpp pcache_init", init, ['fopen((pcache_path + ".nocarry").c_str(), "rb")',
                                            'getenv("HOOKSHOT_PREPARE_ONLY") != nullptr, carry_refused,',
                                            # (a carried file's first save writes it, with the new stamp, even with nothing added)
                                            "pcache_saved_size = pcache_carried ? 0 : held;"])
    save = between(draw, "void pipeline_cache_save(bool force) {", "\n}\n")
    # (the carry's limit before the bound's own refusal: over the bound a carried cache asks for the rebuild, not the old file)
    in_order("vk_draw.cpp pipeline_cache_save", save, [
        "if (!pcache_carry_save_ok(n)) return;",
        "if (!pcache_bound::within(n, pcache_bound_bytes)) {",
        "std::vector<uint8_t> data(n);",
        "if (ok && !keep_old) ok = pcache_carry_saving(n, want);",
        "MoveFileExA(tmp.c_str(), pcache_path.c_str(),",
        "if (!keep_old) {",
        'file_writers::write_whole(pcache_path + ".for", want, 16);',
        "pcache_carry_saved(n, want);",
    ])
    past = between(draw, "bool pcache_carry_past(size_t n) {", "\n}\n")
    in_order("vk_draw.cpp pcache_carry_past", past, [
        "pcache_carry::over(pcache_carried, n, pcache_side, pcache_bound_bytes)",
        "pcache_carry_over.exchange(true)",
        "pcache_carry_refused();",
    ])
    refused = between(draw, "void pcache_carry_refused() {", "\n}\n")
    need("vk_draw.cpp pcache_carry_refused", refused, ["program_cache_stamp()",
                                                       'file_writers::write_whole(pcache_path + ".nocarry", stamp, 16)'])
    # (and a cache dropped early, under its limit, is not written either)
    need("vk_draw.cpp pcache_carry_save_ok", draw, [
        "bool pcache_carry_save_ok(size_t n) { return !pcache_carry_over.load() && !pcache_carry_past(n); }"])
    # (the early drop: a carried cache only, its limit, not on Intel while workers build, the rule of the header, then as past it)
    early = between(draw, "bool pcache_carry_early(size_t from, size_t done, size_t total, size_t libs) {", "\n}\n")
    in_order("vk_draw.cpp pcache_carry_early", early, [
        'pcache_carry::project_on(getenv("RX_VK_PCACHE_CARRY_PROJECT"))',
        "if (!pcache_carried) return false;",
        "lim = pcache_carry::limit(pcache_side.base, pcache_bound_bytes);",
        "if (dev.props.vendorID == 0x8086) return false;",
        "vkGetPipelineCacheData(dev.device, use.cache, &n, nullptr)",
        "if (n > lim || !pcache_carry::drop_early(n, from, done, total, libs, lim)) return false;",
        "if (pcache_carry_over.exchange(true)) return true;",
        'logf("vk: the pipeline cache carried over a stamp change would grow to about %llu MB at this rate',
        "RX_VK_PCACHE_CARRY_PROJECT=0: only when it is past",
        "pcache_carry_refused();",
        "return true;",
    ])
    held = between(draw, "bool pcache_carry_held(size_t* n) {", "\n}\n")
    # (and so the warm's own order unless the projection is made: switched off, not carried, Intel)
    in_order("vk_draw.cpp pcache_carry_held", held, ["*n = 0;",
                                                     'pcache_carry::project_on(getenv("RX_VK_PCACHE_CARRY_PROJECT"))',
                                                     "if (!pcache_carried) return false;",
                                                     "if (dev.props.vendorID == 0x8086) return false;",
                                                     "vkGetPipelineCacheData(dev.device, use.cache, n, nullptr) == VK_SUCCESS"])
    side = between(draw, "void pcache_carry_saved(size_t n, const char* stamp) {", "\n}\n")
    need("vk_draw.cpp pcache_carry_saved", side, ["pcache_carry::saved(pcache_side, n, stamp)",
                                                   'file_writers::write_whole(pcache_path + ".base", out, sizeof out)'])
    # (the libraries' pass: nothing into a carried cache past what it may hold; looked at while it builds; and the carry's
    # look before the bound's branch, which would count the part as done with the old file on the disk)
    warm = between(draw, "bool warm_libraries(void (*progress)(size_t done, size_t total)) {", "\n}\n")
    in_order("vk_draw.cpp warm_libraries", warm, [
        "pcache_init();",
        "if (pcache_carry_check(false)) {",
        "return false;",
        # (the early drop: the carried cache's size before the workers begin; its sample, every 32nd of the order, built first
        # and the others waiting for its projection, so that the growth is the sample's alone; the projection once the sample is
        # done, before the look past its limit)
        "const bool carry_projected = pcache_carry_held(&carry_from);",
        "const size_t carry_sample = carry_projected ? (size_t)pcache_carry::sample_count(total) : 0;",
        "std::atomic<bool> carry_sampled{!carry_projected};",
        "if (prepare_stopped() || pcache_carry_over.load(std::memory_order_relaxed)) return;",
        "const size_t claim = next++;",
        "if (claim >= total) return;",
        "const size_t i = carry_projected ? (size_t)pcache_carry::sample_order(claim, total) : claim;",
        "if (claim >= carry_sample && !carry_sampled.load()) {",
        "while (!carry_sampled.load() && !prepare_stopped() && !pcache_carry_over.load(std::memory_order_relaxed))",
        "if (prepare_stopped() || pcache_carry_over.load(std::memory_order_relaxed)) return;",
        "if (i < loaded.size()) warm(*loaded[i], slot);",
        "program_cache_borrow(keys[i - loaded.size()]",
        "threads.emplace_back(worker, (int)i);",
        "while (done.load() < total && !prepare_stopped() && !pcache_carry_over.load()) {",
        "if (!carry_sampled.load() && done.load() >= carry_sample) {",
        "pcache_carry_early(carry_from, done.load(), total, libs.load());",
        "carry_sampled = true;",
        "std::chrono::milliseconds(pcache_carry::kCheckMs)",
        "pcache_carry_check(true);",
        "for (std::thread& t : threads) t.join();",
        "if (prepare_stopped()) {",
        "if (pcache_carry_check(false)) {",
        "return false;",
        "if (gpl_libs_cached()) {",
        "!pcache_bound::within(held, pcache_bound_bytes)",
        "warm_unsaved = true;",
        "const bool saved = pcache_saved_now();",
    ])
    run = between(draw, "bool prepare_run(void (*progress)(size_t done, size_t total), bool background) {", "\n}\n")
    # (the run into the fresh cache: the compute pipelines first, from the program cache, then the driver's part of the
    # translated shaders, nothing translated again; the carried run's warm names its compute programs, builds nothing into a
    # cache past what it may hold, and is looked at while it builds and once its workers are joined)
    in_order("vk_draw.cpp prepare_run", run, [
        "const PrepareWhy why = prepare_check(",
        "if (!background && prepare_cs_again.exchange(false) && !warm_kept_computes(progress)) return false;",
        "if (why == PrepareWhy::None && !getenv(\"RX_VK_PREPARE_AGAIN\")) {",
        # (recorded as made, a carried cache found warm is written with the new stamp: no record says made over the old file)
        "lib_check_cold(path, warmed);",
        "if (!cold && !background && pcache_carried && prepared_for.warmed == warmed) pcache_saved_now();",
        "if (!cold && (prepared_for.warmed == warmed || background)) return false;",
        "if (cold && !prepare_stopped() && !warm_reset && !pcache_carry_over.load()) lib_check_made(path, warmed, false);",
        "const auto warm_compute = [&](Program& p) {",
        "prepare_cs_keys.insert(p.key);",
        "if (pcache_carry_over.load(std::memory_order_relaxed)) return;",
        "if (prepare_compute_pipeline(p)) libs++;",
        "pcache_carry_check(true);",
        "for (std::thread& t : threads) t.join();",
        "if (!background) pcache_carry_check(false);",
        "prepared_for.warmed = 0;",
        "prepared_write(path, prepared_for);",
        "if (!warm_libraries(progress)) {",
        "return false;",
    ])
    kept = between(draw, "bool warm_kept_computes(void (*progress)(size_t done, size_t total)) {", "\n}\n")
    in_order("vk_draw.cpp warm_kept_computes", kept, [
        "keys.assign(prepare_cs_keys.begin(), prepare_cs_keys.end());",
        "program_cache_borrow(keys[i], [&](Program& p) {",
        "p.compute",
        "prepare_compute_pipeline(p)",
    ])
    if "seed_compute" in kept or "seed_program" in kept or "translate" in kept.replace("nothing translated", ""):
        raise SystemExit("warm_kept_computes translates again")
    build = between(draw, "bool prepare_compute_pipeline(Program& p) {", "\n}\n")
    need("vk_draw.cpp prepare_compute_pipeline", build, ["pin_subgroup(ci.stage, p.workgroup[0]);",
                                                          "guarded_compute(pcache, &ci, &pl)",
                                                          'if (getenv("RX_VK_NOOPT")) ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;'])
    rebuild = between(draw, "bool pcache_carry_rebuild() {", "\n}\n")
    in_order("vk_draw.cpp pcache_carry_rebuild", rebuild, [
        "if (!pcache_carry_over.load() || !pcache || pcache_door.closed() || prepare_stopped()) return false;",
        "vkCreatePipelineCache(dev.device, &ci, nullptr, &fresh)",   # (a fresh one first: never left without a cache)
        "while (pcache_door.inside()) Sleep(1);",
        "pcache = fresh;",
        "vkDestroyPipelineCache(dev.device, merged, nullptr);",
        "pcache_saved_size = 0;",
        "pcache_carried = false;",
        "pcache_carry::session(pcache_carry::Start::Fresh, pcache_side)",
        "pcache_carry_over = false;",
        "prepare_cs_again = !prepare_cs_keys.empty();",
        # (the stamp kept: the next run goes to the driver's part alone, and translates nothing again)
        "had.warmed = 0;",
        "prepared_write(path, had);",
        "return true;",
    ])
    if "had.stamp = 0" in rebuild:
        raise SystemExit("the rebuild sends the next run down the whole path (translation again, no compute pipelines)")
    main = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    prep = between(main, "if (prepare_only()) {", "return true;")
    in_order("vk_main.cpp the preparation", prep, [
        "bool prepared = prepare_run(progress, false);",
        "if (pcache_carry_rebuild()) {",
        "fflush(stdout);",
        "prepared = prepare_run(progress, false);",
        "if (prepared && !prepare_stopped()) predict_run(progress);",
        "precompile_run(progress);",
        "pipeline_cache_save(true);",
    ])
    # (the carried history is no longer open only once the preparation went through to its end and saved: not after a stop)
    end = main[main.index("if (prepare_only()) {"):]
    end = end[:end.index("remove(skipped_path().c_str());")]
    in_order("vk_main.cpp the preparation's end", end, ["pipeline_cache_save(true);", "if (prepare_stopped()) {", "return true;",
                                                       "pcache_carry_done();"])
    done = between(draw, "void pcache_carry_close() {", "\n}\n")
    in_order("vk_draw.cpp pcache_carry_close", done, [
        "if (pcache_carry_over.load()) return;",
        "if (!pcache_carried || !pcache_side.open) return;",
        "pcache_side.open = 0;",
        "if (std::memcmp(pcache_side.stamp, stamp, 16)) return;",
        'file_writers::write_whole(pcache_path + ".base", out, sizeof out);',
    ])
    need("vk_draw.cpp pcache_carry_done", draw, ["void pcache_carry_done() { pcache_carry_close(); }"])
    need("vk_internal.h", (ROOT / "gpu/vk/vk_internal.h").read_text(encoding="utf-8"), ["bool pcache_carry_rebuild();",
                                                                                     "void pcache_carry_done();"])
    dev_md = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "`RX_VK_PCACHE_CARRY=0`" not in dev_md:
        raise SystemExit("RX_VK_PCACHE_CARRY is not in docs/development.md's switches")
    # (the early drop's switch, in the row after the carry's)
    if "| `RX_VK_PCACHE_CARRY_PROJECT=0` |" not in dev_md or \
            dev_md.index("| `RX_VK_PCACHE_CARRY_PROJECT=0` |") < dev_md.index("`RX_VK_PCACHE_CARRY=0`"):
        raise SystemExit("RX_VK_PCACHE_CARRY_PROJECT is not in docs/development.md's switches after RX_VK_PCACHE_CARRY")
    # (the compute pass and the precompile keep the look past the limit alone, pcache_carry.h)
    for fn in ("bool prepare_run(void (*progress)(size_t done, size_t total), bool background) {",
               "void precompile_run(void (*progress)(size_t done, size_t total)) {"):
        if "pcache_carry_early(" in between(draw, fn, "\n}\n"):
            raise SystemExit(f"vk_draw.cpp {fn.split('(')[0]}: projected on programs done that build nothing into the cache")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="pcache-carry-") as tmp:
        tmp = Path(tmp)
        (tmp / "t.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / "pcache-carry"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu/vk"), str(tmp / "t.cpp"),
                        "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"pcache_carry.h: {r.stdout.strip()}")
    print("PASS: the pipeline cache is carried over a stamp change at an update's preparation up to 1.25 x its base, "
          "cumulatively, and built again into a fresh cache past it")


if __name__ == "__main__":
    main()
