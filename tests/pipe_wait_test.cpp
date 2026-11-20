// gpu/vk/pipe_wait.h, compiled and run (tests/test_pipe_wait.py): what a draw does whose pipeline is not ready, the frame's
// budgets, and the worker's order for a pipeline whose libraries were cold. One case a run, named on the command line.
#include "pipe_wait.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace pipe_wait;

namespace {

int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

constexpr int64_t MS = kMs;

// ---- kept(): the pass
void case_kept() {
    const uint64_t now = 100;
    // (target(): drawn in an earlier frame only when first drawn before now)
    CHECK(!target(false, 50, now).there && !target(false, 50, now).earlier);
    CHECK(target(true, 0, now).there && !target(true, 0, now).earlier);       // (never drawn)
    CHECK(!target(true, now, now).earlier);                                   // (first drawn this frame)
    CHECK(target(true, now - 1, now).earlier);
    const Target none{}, earlier = target(true, 10, now), fresh = target(true, 0, now), thisframe = target(true, now, now);
    for (int rule = 0; rule < 2; rule++) {
        const bool r = rule != 0;
        // (nothing there: drawn once)
        CHECK(!kept(nullptr, 0, none, r));
        // (a colour target drawn before keeps the pass, a new one beside it too)
        Target c1[2] = {earlier, fresh};
        CHECK(kept(c1, 2, none, r));
        CHECK(kept(c1, 2, earlier, r));
        // (only new colour targets, no depth: drawn once)
        Target c2[2] = {fresh, thisframe};
        CHECK(!kept(c2, 2, none, r));
        // (depth alone, drawn before: kept)
        CHECK(kept(nullptr, 0, earlier, r));
        // (an empty colour slot beside a kept depth target is no new colour target)
        Target c3[2] = {none, none};
        CHECK(kept(c3, 2, earlier, r));
    }
    // (the rule: a new colour target with the per-frame depth target is drawn once; without it, kept as before)
    Target icon[1] = {fresh};
    CHECK(!kept(icon, 1, earlier, true));
    CHECK(kept(icon, 1, earlier, false));
    Target icon2[1] = {thisframe};
    CHECK(!kept(icon2, 1, earlier, true));
    CHECK(kept(icon2, 1, earlier, false));
    // (a depth target new too: drawn once either way)
    CHECK(!kept(icon, 1, fresh, true) && !kept(icon, 1, fresh, false));
}

// ---- full_screen()
void case_full() {
    const uint64_t pass = 1920ull * 1080;
    CHECK(full_screen(false, false, 3, 1, pass, pass));
    CHECK(full_screen(false, false, 4, 1, pass, pass));
    CHECK(full_screen(false, false, 6, 1, pass, pass));
    CHECK(full_screen(false, false, 6, 0, pass, pass));   // (no instance count given: one)
    CHECK(!full_screen(false, false, 7, 1, pass, pass));
    CHECK(!full_screen(false, false, 0, 1, pass, pass));
    CHECK(!full_screen(true, false, 3, 1, pass, pass));   // (a mesh draw: its count is not vertices)
    CHECK(!full_screen(false, true, 3, 1, pass, pass));   // (indirect: not known)
    CHECK(!full_screen(false, false, 3, 2, pass, pass));  // (instanced: a tile set, not one full-screen pass)
    CHECK(full_screen(false, false, 3, 1, pass / 2, pass));
    CHECK(!full_screen(false, false, 3, 1, pass / 2 - 1, pass));
    CHECK(!full_screen(false, false, 3, 1, 0, 0));
}

// ---- Pool and LinkBudget
void case_pool() {
    Pool p(50 * MS);
    CHECK(p.left(1) == 50 * MS);
    p.charge(1, 30 * MS);
    CHECK(p.left(1) == 20 * MS);
    p.charge(1, 30 * MS);   // (never below none)
    CHECK(p.left(1) == 0);
    p.charge(1, -5 * MS);   // (a negative charge gives nothing back)
    CHECK(p.left(1) == 0);
    CHECK(p.left(2) == 50 * MS);   // (the next frame: full again)
    p.charge(2, 10 * MS);
    CHECK(p.left(2) == 40 * MS);
    CHECK(p.left(3) == 50 * MS);
    Pool off(0);
    CHECK(off.left(1) == 0);

    LinkBudget none(0);
    CHECK(!none.on());
    none.spent(1, 1000 * MS);
    CHECK(none.links(1));   // (no bound: every link on the draw's thread, as before)
    LinkBudget six(kLinkMs * MS);
    CHECK(six.on() && six.links(1));
    six.spent(1, 4 * MS);
    CHECK(six.links(1));
    six.spent(1, 2 * MS);
    CHECK(!six.links(1));   // (spent: the next new pipeline is linked by a worker)
    CHECK(six.links(2));    // (a new frame)
}

// ---- choose()
bool is(const Choice& c, Act a, int64_t ns = -1, Kind k = K_NONE) {
    return c.act == a && (ns < 0 || c.ns == ns) && (a != Act::Wait || c.kind == k);
}
void case_choose() {
    const Rules def{};   // (the defaults: no one-shot budget, the full-screen pool on)
    const int64_t pool = 50 * MS, last = 40 * MS;
    // (RX_VK_PIPE_SKIP=0: waited for, whatever the draw)
    Rules all = def;
    all.always_wait = true;
    CHECK(is(choose(all, true, false, true, true, pool, last), Act::WaitAll));
    CHECK(is(choose(all, true, false, false, false, pool, last), Act::WaitAll));
    // (targets drawn once, or shaders that write memory: waited for, as before)
    CHECK(is(choose(def, false, false, false, false, pool, last), Act::WaitAll));
    CHECK(is(choose(def, true, true, false, false, pool, last), Act::WaitAll));
    CHECK(is(choose(def, false, false, true, true, 0, 0), Act::WaitAll));
    // (kept, not full-screen: left out, as before)
    CHECK(is(choose(def, true, false, false, false, pool, last), Act::Skip));
    CHECK(is(choose(def, true, false, false, true, pool, last), Act::Skip));
    // (kept, full-screen, the hold takes the frame: left out, the frame held)
    CHECK(is(choose(def, true, false, true, true, pool, last), Act::Skip));
    // (kept, full-screen, no hold: from the pool, then the last wait, then left out)
    CHECK(is(choose(def, true, false, true, false, pool, last), Act::Wait, pool, K_FULL));
    CHECK(is(choose(def, true, false, true, false, 0, last), Act::Wait, last, K_LAST));
    CHECK(is(choose(def, true, false, true, false, MS - 1, last), Act::Wait, last, K_LAST));   // (under a ms: none)
    CHECK(is(choose(def, true, false, true, false, 0, MS - 1), Act::Skip));
    CHECK(is(choose(def, true, false, true, false, 0, 0), Act::Skip));
    // (RX_VK_PIPE_FRAME_WAIT_MS=0: left out, as before)
    Rules nofull = def;
    nofull.full_wait = false;
    CHECK(is(choose(nofull, true, false, true, false, pool, last), Act::Skip));
    // (RX_VK_PIPE_ONESHOT_BUDGET=1: drawn once or writing memory, from the pool, then left out; not from the last wait)
    Rules one = def;
    one.oneshot_budget = true;
    CHECK(is(choose(one, false, false, false, false, pool, last), Act::Wait, pool, K_ONESHOT));
    CHECK(is(choose(one, true, true, false, false, 20 * MS, last), Act::Wait, 20 * MS, K_ONESHOT));
    CHECK(is(choose(one, false, false, false, false, 0, last), Act::Skip));
    CHECK(is(choose(one, false, false, true, true, 0, last), Act::Skip));
    // (the defaults and none of the new rules are the rule before: kept and writing nothing, left out; else waited for)
    for (int k = 0; k < 2; k++)
        for (int w = 0; w < 2; w++)
            for (int h = 0; h < 2; h++) {
                const Choice c = choose(nofull, k, w, false, h, pool, last);
                CHECK((k && !w) ? c.act == Act::Skip : c.act == Act::WaitAll);
            }
}

// ---- one frame's draws: the pool shared, in order
void case_frame() {
    Pool pool(kFrameWaitMs * MS), lastp(kLastWaitMs * MS);
    const Rules def{};
    const uint64_t f = 7;
    // (five full-screen draws whose pipelines take 30 ms each to come, none held: the first has its pipeline after 30 ms of
    // the pool, the second waits the 20 left and is left out, the third has its pipeline within the last wait's 40, the fourth
    // waits the 10 left of that and is left out, the fifth is left out at once)
    int waited = 0, left = 0;
    std::vector<int64_t> waits;
    for (int i = 0; i < 5; i++) {
        const Choice c = choose(def, true, false, true, false, pool.left(f), lastp.left(f));
        if (c.act == Act::Skip) {
            left++;
            continue;
        }
        CHECK(c.act == Act::Wait);
        const int64_t spent = std::min<int64_t>(c.ns, 30 * MS);
        (c.kind == K_LAST ? lastp : pool).charge(f, spent);
        waited++, waits.push_back(c.ns / MS);
        left += c.ns < 30 * MS;
    }
    CHECK(waited == 4 && left == 3);
    CHECK((waits == std::vector<int64_t>{50, 20, 40, 10}));
    CHECK(pool.left(f) == 0 && lastp.left(f) == 0);   // (the frame waited 90 ms for its pipelines in all, and no longer)
    // (the next frame: the pool full again)
    CHECK(is(choose(def, true, false, true, false, pool.left(f + 1), lastp.left(f + 1)), Act::Wait, kFrameWaitMs * MS, K_FULL));
}

// ---- libs_first(): the worker's order
struct Log {
    std::vector<std::string> ev;
    int libs_cold[2] = {0, 0};   // (libraries still cold: cleared by a build that works)
    bool build_fails = false, link_refused = false, entry_ready = false;
    int whole_later = 0, whole_now = 0;
};
bool run_libs_first(Log& L, uint64_t c0, uint64_t c1, bool later) {
    using P = int;
    return libs_first<P>(
        c0, c1, later,
        [&](uint64_t c[2]) {
            L.ev.push_back("link");
            for (int i = 0; i < 2; i++)
                if (L.libs_cold[i]) c[i] = 100 + i;
            if (L.libs_cold[0] || L.libs_cold[1] || L.link_refused) return 0;
            return 7;
        },
        [&](int i, uint64_t k) {
            L.ev.push_back("build" + std::to_string(i) + ":" + std::to_string(k));
            if (!L.build_fails) L.libs_cold[i] = 0;
        },
        [&](int l) {
            L.ev.push_back("linked");
            L.entry_ready = l != 0;
            return l != 0;
        },
        [&] { L.ev.push_back("whole-later"), L.whole_later++; },
        [&] { L.ev.push_back("whole-now"), L.whole_now++, L.entry_ready = true; });
}
std::string seq(const Log& L) {
    std::string s;
    for (const auto& e : L.ev) s += (s.empty() ? "" : " ") + e;
    return s;
}
void case_libs_first() {
    {   // (two cold libraries: both built, then the link, the entry ready with it, the whole build queued after)
        Log L;
        L.libs_cold[0] = L.libs_cold[1] = 1;
        CHECK(run_libs_first(L, 11, 12, false));
        CHECK(seq(L) == "build0:11 build1:12 link linked whole-later");
        CHECK(L.entry_ready && L.whole_later == 1 && L.whole_now == 0);
    }
    {   // (one cold)
        Log L;
        L.libs_cold[1] = 1;
        CHECK(run_libs_first(L, 0, 12, false));
        CHECK(seq(L) == "build1:12 link linked whole-later");
    }
    {   // (a cold library the worker cannot build: no link, the whole build here, the entry ready with it)
        Log L;
        L.libs_cold[0] = 1, L.build_fails = true;
        CHECK(!run_libs_first(L, 11, 0, false));
        CHECK(seq(L) == "build0:11 link whole-now");
        CHECK(L.entry_ready && L.whole_later == 0 && L.whole_now == 1);
    }
    {   // (the link refused: the whole build here)
        Log L;
        L.libs_cold[0] = 1, L.link_refused = true;
        CHECK(!run_libs_first(L, 11, 0, false));
        CHECK(seq(L) == "build0:11 link whole-now");
    }
    {   // (left to the worker by the frame's link budget, libraries warm: linked at once)
        Log L;
        CHECK(run_libs_first(L, 0, 0, true));
        CHECK(seq(L) == "link linked whole-later");
    }
    {   // (left to the worker, a library cold that only its link finds: built, then linked)
        Log L;
        L.libs_cold[1] = 1;
        CHECK(run_libs_first(L, 0, 0, true));
        CHECK(seq(L) == "link build1:101 link linked whole-later");
    }
    {   // (left to the worker, refused with nothing cold: the whole build here)
        Log L;
        L.link_refused = true;
        CHECK(!run_libs_first(L, 0, 0, true));
        CHECK(seq(L) == "link whole-now");
    }
    {   // (nothing cold and not left to it: the whole build, nothing else)
        Log L;
        CHECK(!run_libs_first(L, 0, 0, false));
        CHECK(seq(L) == "whole-now");
    }
}

}   // namespace

int main(int argc, char** argv) {
    const char* which = argc > 1 ? argv[1] : "";
    struct Case { const char* name; void (*fn)(); } cases[] = {
        {"kept", case_kept}, {"full", case_full}, {"pool", case_pool}, {"choose", case_choose}, {"frame", case_frame},
        {"libs-first", case_libs_first},
    };
    bool ran = false;
    for (const Case& c : cases)
        if (!*which || !std::strcmp(which, c.name)) c.fn(), ran = true;
    if (!ran) return std::printf("no case %s\n", which), 2;
    if (failures) return 1;
    std::printf("PASS %s\n", *which ? which : "all");
    return 0;
}
