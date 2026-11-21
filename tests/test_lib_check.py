"""Whether the stage libraries the preparation made are still in the pipeline cache, at a start whose record says they were made
(gpu/vk/lib_check.h; vk_draw.cpp lib_check_sample, lib_check_cold, lib_check_made, prepare_run; after NX2navi e10ef60; no game/GPU).

The header is compiled and run as it is:
- RX_VK_LIB_CHECK: on unless 0;
- a variant asked of the caches only is cold by their answer alone (a slow first lookup they answered is not cold); one made
  without asking is cold by its time against its own second making (the warm reference), never by a fixed time;
- a sample is cold when more than half of the variants that count are, says nothing under half a sample, and a library the
  driver refused does not count;
- the places sampled are spread over the whole program cache, distinct, and move with the start;
- the record of the starts that found them cold again: written whole, read back, another stamp's or a broken one is no record;
- starts in a row against a cache that loses the libraries every time: made again once, then left to the draws; a start that
  finds them warm begins the count anew; another stamp begins it anew; a part left out, or one whose cache was not saved,
  stops the same way.
Then the source: the sample makes the very libraries warm_libraries makes (same state, targets, topology, the same variants
left out, the shading rate's libraries of a native mesh program in both), asks with the flag only where the device can, the
check runs only before the game from prepare_run, at a start recorded as made, and makes the part again through warm_begin
and warm_libraries, recorded on every way out but a stopped one; a cache over its bound is not made again.
python -I -B tests/test_lib_check.py --cxx clang++
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cstdio>
#include <set>
#include "lib_check.h"
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); return 1; } } while (0)
using namespace lib_check;
static Probe asked(bool cold_answer, double first = 0.4, double again = 0.4) {
    Probe p;
    p.asked = true, p.answered_cold = cold_answer, p.made = !cold_answer, p.first_ms = first, p.again_ms = cold_answer ? 0 : again;
    return p;
}
static Probe timed(double first, double again) {
    Probe p;
    p.made = true, p.first_ms = first, p.again_ms = again;
    return p;
}
static std::vector<Probe> sample(int cold, int warm, int refused = 0) {
    std::vector<Probe> v;
    for (int i = 0; i < cold; i++) v.push_back(asked(true));
    for (int i = 0; i < warm; i++) v.push_back(asked(false));
    for (int i = 0; i < refused; i++) v.push_back(Probe{});   // (not made, not answered cold: refused by the driver)
    return v;
}
// A start as prepare_run makes it: the sample's verdict, the record, and the part made again (made: it was begun and finished,
// left: warm_begin left it out). Returns what the start did
static Do start(const std::string& rec, uint64_t stamp, Verdict v, bool left = false) {
    int next = -1;
    const Do d = decide(v, state_read(rec), stamp, &next);
    if (v == Verdict::Warm && state_read(rec).found) remove(rec.c_str());
    if (d == Do::Again && !state_write(rec, stamp, left ? kStrikes : next)) return Do::Nothing;
    return d;
}

int main(int, char** argv) {
    const std::string dir = argv[1];
    // ---- the switch
    CHECK(on(nullptr) && on("") && on("1") && !on("0"));
    // ---- a variant
    CHECK(cold(asked(true)) && !cold(asked(false)));
    CHECK(!cold(asked(false, 40, 0.3)));   // (asked and answered: made without a compile, whatever its first lookup took)
    CHECK(cold(timed(120, 0.6)) && cold(timed(30, 2)));   // (compiled, against the warm second making)
    CHECK(!cold(timed(0.7, 0.6)) && !cold(timed(4, 0.4)) && !cold(timed(2.5, 0.1)));   // (lookups: a few ms say nothing)
    CHECK(!cold(timed(9, 2)) && cold(timed(12, 2)));   // (relative: kRatio times the reference and kSlackMs more)
    CHECK(!cold(timed(300, 80)) && cold(timed(400, 80)));   // (a driver slow at lookups too: still by its own reference)
    CHECK(!counts(Probe{}) && counts(asked(true)) && counts(timed(1, 1)));
    // ---- a sample
    CHECK(verdict(tally(sample(0, 12))) == Verdict::Warm);
    CHECK(verdict(tally(sample(12, 0))) == Verdict::Cold);
    CHECK(verdict(tally(sample(7, 5))) == Verdict::Cold);
    CHECK(verdict(tally(sample(6, 6))) == Verdict::Warm);   // (more than half: the median)
    CHECK(verdict(tally(sample(1, 5, 6))) == Verdict::Warm && tally(sample(1, 5, 6)).got == 6);   // (refused: not counted)
    CHECK(verdict(tally(sample(4, 1, 7))) == Verdict::Unknown);   // (under half a sample says nothing)
    CHECK(verdict(tally({})) == Verdict::Unknown);
    {
        const Tally t = tally({timed(100, 1), timed(1, 1), timed(200, 2), asked(true, 0.01)});
        CHECK(t.got == 4 && t.cold == 3 && t.answered == 1);
        CHECK(t.first_median == 100 && t.again_median == 1);   // (the median: of all first makings, of the second ones made)
    }
    // ---- the places sampled
    {
        CHECK(spots(0, 24, 7).empty() && spots(10, 0, 7).empty());
        CHECK((spots(5, 24, 99) == std::vector<size_t>{0, 1, 2, 3, 4}));   // (fewer programs than places: all of them)
        const std::vector<size_t> a = spots(46600, kLook, 0), b = spots(46600, kLook, 1234567);
        CHECK((int)a.size() == kLook && (int)b.size() == kLook && a != b);
        CHECK(a.front() == 0 && b.front() == 1234567 % (46600 / kLook) && b.back() < 46600);
        CHECK(a.back() >= 46600 - 2 * (46600 / kLook));   // (over the whole cache, not its first part)
        CHECK(std::set<size_t>(b.begin(), b.end()).size() == b.size());
    }
    // ---- the record
    const uint64_t stamp = 0x0123456789ABCDEFull;
    {
        const std::string rec = dir + "/a.check";
        CHECK(!state_read(rec).found);
        CHECK(state_write(rec, stamp, 1));
        State s = state_read(rec);
        CHECK(s.found && s.stamp == stamp && s.strikes == 1);
        CHECK(state_write(rec, stamp + 1, 0) && state_read(rec).stamp == stamp + 1 && state_read(rec).strikes == 0);   // (replaced)
        FILE* f = fopen(rec.c_str(), "wb");
        CHECK(f && fputs("lib check 12 x\n", f) >= 0 && fclose(f) == 0);
        CHECK(!state_read(rec).found);   // (broken: no record)
        CHECK(!state_write(dir + "/no-such-folder/a.check", stamp, 0));
    }
    // ---- what a start does
    {
        int next = -1;
        CHECK(decide(Verdict::Warm, State{}, stamp, &next) == Do::Nothing && next == -1);
        CHECK(decide(Verdict::Unknown, State{true, stamp, 0}, stamp, &next) == Do::Nothing);
        CHECK(decide(Verdict::Cold, State{}, stamp, &next) == Do::Again && next == 0);
        CHECK(decide(Verdict::Cold, State{true, stamp, kStrikes}, stamp, &next) == Do::Stop);
        CHECK(decide(Verdict::Cold, State{true, stamp, kStrikes + 3}, stamp, &next) == Do::Stop);
        CHECK(decide(Verdict::Cold, State{true, stamp + 1, 9}, stamp, &next) == Do::Again && next == 0);   // (another driver's)
        CHECK(kStrikes == 0);   // (each try holds a start for minutes: made again once, not again right after)
    }
    {
        // (a cache that loses the libraries at every start: made again once, then left to the draws, for good)
        const std::string rec = dir + "/b.check";
        CHECK(start(rec, stamp, Verdict::Cold) == Do::Again && state_read(rec).found);
        CHECK(start(rec, stamp, Verdict::Cold) == Do::Stop);
        CHECK(start(rec, stamp, Verdict::Unknown) == Do::Nothing && state_read(rec).found);   // (nothing said: the record stays)
        CHECK(start(rec, stamp, Verdict::Cold) == Do::Stop);
        // (a driver update: a stamp of its own, the count begins anew)
        CHECK(start(rec, stamp + 1, Verdict::Cold) == Do::Again);
        // (kept this time: the record goes, and a later loss is made again)
        CHECK(start(rec, stamp + 1, Verdict::Warm) == Do::Nothing && !state_read(rec).found);
        CHECK(start(rec, stamp + 1, Verdict::Cold) == Do::Again && state_read(rec).strikes == 0);
        // (the usual start: warm, no record, nothing written)
        const std::string none = dir + "/c.check";
        for (int i = 0; i < 3; i++) CHECK(start(none, stamp, Verdict::Warm) == Do::Nothing && !state_read(none).found);
        // (left out by warm_begin, or made into a cache it could not write: no second try)
        const std::string left = dir + "/d.check";
        CHECK(start(left, stamp, Verdict::Cold, true) == Do::Again && start(left, stamp, Verdict::Cold) == Do::Stop);
        // (a cache whose save fails every time: made again once, then left to the draws, not held at every start)
        const std::string unsaved = dir + "/e.check";
        CHECK(start(unsaved, stamp, Verdict::Cold, true) == Do::Again);
        for (int i = 0; i < 3; i++) CHECK(start(unsaved, stamp, Verdict::Cold) == Do::Stop);
    }
    return 0;
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    sample = between(draw, "lib_check::Tally lib_check_sample(", "\n}\n")
    warm = between(draw, "bool warm_libraries(", "\n}\n")
    # ---- the sample makes the very libraries the driver's part made
    lib = "build_graphics(p, *blank, at, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, part"
    for text, who in ((sample, "lib_check_sample"), (warm, "warm_libraries")):
        for need in ("blank->nvattr = 16;", "Targets at{};", lib, "for (int part = 1; part <= 2; part++)"):
            if need not in text:
                raise SystemExit(who + ": " + need + " is missing (the sample would ask for other libraries than the part made)")
    # ((Triforce) and a native mesh program's shading rate libraries (pipeline_rate, keyed apart in gpl_keys) in both: the
    # warm made none of them once, and the check could not see it)
    for text, who in ((sample, "lib_check_sample"), (warm, "warm_libraries")):
        steps = ("for (int rate = 0; rate < 2", "if (rate) {", "at = rate_targets();", "if (!pipeline_rate(p, at)) break;", lib)
        if any(s not in text for s in steps):
            raise SystemExit(who + ": the shading rate's libraries of a native mesh program are not made with the others")
        at = [text.index(s) for s in steps]
        if at != sorted(at):
            raise SystemExit(who + ": the rate's targets are not set before its libraries are made (the loop's second round)")
    targets = between(draw, "Targets rate_targets() {", "\n}\n")
    if "t.depth_format = VK_FORMAT_D32_SFLOAT;" not in targets or "t.n" in targets:
        raise SystemExit("rate_targets: wanted no colour target and a depth format, so that pipeline_rate holds")
    if "p.failed || p.compute || p.fallback || p.stages.empty()" not in warm or \
            "if (taken || p.failed || p.compute || p.fallback || p.stages.empty() || gpl_skipped(p)) return;" not in sample:
        raise SystemExit("lib_check_sample samples other variants than warm_libraries makes libraries of")
    if lib + ", ask ? &cold : nullptr);" not in sample or "const bool ask = gpl_ask_only();" not in sample or \
            "pr.asked = ask, pr.made = true;" not in sample:
        raise SystemExit("lib_check_sample does not ask the caches only where the device can, as a draw's link does")
    for need in ("program_cache_keys(&loaded)", "lib_check::spots(keys.size(), lib_check::kLook, seed)",
                 "program_cache_borrow(keys[i],", "vkDestroyPipeline(dev.device, lib, nullptr);", "prepare_stopped()",
                 "return lib_check::tally(probes);"):
        if need not in sample:
            raise SystemExit("lib_check_sample: " + need + " is missing")
    if "program_cache_each(" in sample or "SetThreadPriority" in sample:
        raise SystemExit("lib_check_sample reads every record or runs on threads of its own")
    # ---- the check
    check = between(draw, "bool lib_check_cold(", "\n}\n")
    for need in ('lib_check::on(getenv("RX_VK_LIB_CHECK"))', "gpl_libs_cached()", "pcache_init();",
                 "!pcache_bound::within((uint64_t)size, pcache_bound_bytes)) return false;",
                 "lib_check::decide(v, had, warmed, &lib_check_next)", "if (had.found) remove(state.c_str());",
                 "return d == lib_check::Do::Again;", 'prepared + ".check"'):
        if need not in check:
            raise SystemExit("lib_check_cold: " + need + " is missing")
    if check.index("!pcache_bound::within(") > check.index("lib_check_sample(&ms)"):
        raise SystemExit("lib_check_cold samples a cache over its bound, which a making again could not write")
    made = between(draw, "void lib_check_made(", "\n}\n")
    if "!made || warm_unsaved ? lib_check::kStrikes : lib_check_next" not in made:
        raise SystemExit("lib_check_made: a part left out or made into a cache it could not write is tried again")
    over = warm.index("!pcache_bound::within(held, pcache_bound_bytes)) {")
    if warm.index("warm_unsaved = true;", over) > warm.index("return true;", over):
        raise SystemExit("warm_libraries does not say that its cache over the bound was not written")
    # ---- before the game only, from prepare_run, at a start recorded as made
    if draw.count("lib_check_cold(") != 2 or draw.count("lib_check_made(") != 3 or draw.count("lib_check_sample(") != 2:
        raise SystemExit("the check is called past prepare_run")
    prep = between(draw, "bool prepare_run(", "\n}\n")
    for need in ("const bool cold = !background && libraries && prepared_for.warmed == warmed && lib_check_cold(path, warmed);",
                 "if (!cold && (prepared_for.warmed == warmed || background)) return false;",
                 "const bool begun = warm_begin(warming, warmed);", "if (cold) lib_check_made(path, warmed, begun);"):
        if need not in prep:
            raise SystemExit("prepare_run: " + need + " is missing")
    alone = prep[:prep.index("seed_open();\n    if (!programs)")]
    a = alone.index("const bool cold = ")
    b = alone.index("if (!warm_libraries(progress)) {", a)
    c = alone.index("if (cold) lib_check_made(path, warmed, begun);", b)
    if not (alone.index("(prepared_for.warmed == warmed || background)) return false;", a) < b < c < alone.index("warm_end(warming);", c)):
        raise SystemExit("prepare_run: the part made again is not the warm_begin / warm_libraries path, or recorded before it ends")
    if alone.index("return false;", b) > c:
        raise SystemExit("prepare_run records the part made again when its pass was stopped")
    # ((Triforce) every way out after the making again records it, but for a stopped pass: one whose cache could not be saved
    # too, or every start would make the part again for minutes, for good. But for warm_reset: a cache over the bound only by
    # what an older file held, made again once into a new file, which is not read from that file: once at most; and for a
    # carried cache past what it may hold, built again into a fresh cache: pcache_carry.h)
    unsaved = "if (cold && !prepare_stopped() && !warm_reset && !pcache_carry_over.load()) lib_check_made(path, warmed, false);"
    if unsaved not in alone or not b < alone.index(unsaved) < alone.index("return false;", b):
        raise SystemExit("prepare_run: a making again whose cache was not saved is not recorded before it returns")
    # ---- documented
    if "| `RX_VK_LIB_CHECK=0` |" not in (ROOT / "docs/development.md").read_text(encoding="utf-8"):
        raise SystemExit("RX_VK_LIB_CHECK is not in docs/development.md's switches")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="lib-check-") as tmp:
        tmp = Path(tmp)
        (tmp / "t.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / "lib-check"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu/vk"),
                        str(tmp / "t.cpp"), "-o", str(exe)], check=True)
        data = tmp / "data"
        data.mkdir()
        r = subprocess.run([str(exe), str(data)], capture_output=True, text=True, timeout=120)
        if r.returncode:
            raise SystemExit(f"lib_check.h: {r.stdout.strip() or r.returncode}")
    print("PASS: a start recorded as prepared samples the driver's part as it was made, makes it again before the game when the "
          "cache lost it, and stops after a making again that did not stay")


if __name__ == "__main__":
    main()
