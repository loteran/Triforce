"""A load's pipelines made behind its draws (gpu/vk/pipe_warm.h; vk_draw.cpp pipelines_warm, warm_variant, warm_part,
warm_hold, warm_link_take, warm_let_go, gpl_part, gpl_link, graphics_pipeline, precompile_run, pipelines_release_tick;
vk_shader.cpp readahead_main; no game/GPU).

First the header alone: the level RX_VK_PIPELINE_WARM gives, when a worker may make anything, the notes kept a program
variant, the libraries' bound, and the links held (a key once, the oldest let go beyond the bound, each taken once, all
taken out), the last against a plain list at random. Then vk_draw.cpp's own code for the stage libraries' map and the
warm's links, cut out and compiled beside a stand-in driver: a library the warm made is the draw's once a draw finds it or
takes a link made from it, the warm's and the draw's made at once leave one, the warm's; one the driver would have had to
compile is not kept (the draw asks for it as before); the warm makes none beyond its bound; the oldest link goes beyond
its bound; while the RAM is short what no draw has had goes back to the driver, nothing while a warm has libraries in
hand, nothing a draw has; and with four warms, a draw and the RAM's give-back at once, no library or link a draw has, or
a warm has in hand, is destroyed, and none twice. Then the source: the worker warms what it read before it parks it (no
draw can have it), the warm's checks and lock, the draw takes the warm's link for a new entry before it links its own, the
link's keys are the ones gpl_link finds, the precompile keeps the notes' records before it takes the built ones out, the
give-back is asked at the presents, and the switch is in docs/development.md.

(test18) Following the draws: the header's Front (where the draws are, the places too old forgotten), want (links near,
libraries farther, nothing beyond), Made (the oldest library still the warm's) and Held::drop_using; the cut code, run with
RX_VK_PIPELINE_WARM_FOLLOW=1 and =0, lets the oldest library go at kLibsKeep with the links made from it, destroys them only
at the presents with no warm in hand, and holds no link made from a library let go meanwhile. tests/pipe_warm_sim.cpp, a
synthetic load through the read-ahead's real rules (program_readahead.h), shows the counts of both ways: following the
draws, more of the links made are taken, more of the load's pipelines have one, and none the load needed was let go. The
compute pipelines (vk_draw.cpp warm_compute, vk_shader.cpp readahead_drop, readahead_computes_let_go, readahead_ask) by
their source: made as the dispatch makes them, asked of the caches only, for the programs play dispatched, destroyed with
a program the shelf lets go, given back from the shelf with the RAM short.
python -I -B tests/test_pipe_warm.py --cxx clang++
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "pipe_warm.h"
#include "pipe_order.h"
namespace pipe_order = mkwvk::pipe_order;   // (vk_draw.cpp's code is inside mkwvk)
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); return 1; } } while (0)
using VkPipeline = uint64_t;
using VkPrimitiveTopology = int;
#define VK_NULL_HANDLE 0
#define THREAD_PRIORITY_BELOW_NORMAL 0
inline int GetCurrentThread() { return 0; }
inline void SetThreadPriority(int, int) {}
struct Program { uint64_t key; };
struct NvnState {};
struct Targets {};
struct { int device; } dev;
std::atomic<int64_t> st_lib_hits, st_lib_cold;
inline uint64_t mix(uint64_t a, uint64_t b) { return (a * 1000003 + b + 1) * 0x9E3779B97F4A7C15ull; }
inline void logf(const char*, ...) {}
inline void warm_line(const char*) {}
std::atomic<bool> ram_short{false};
inline bool memory_ram_short() { return ram_short.load(); }
inline bool gpl_ask_only() { return true; }
bool readahead_computes_let_go(std::vector<uint64_t>&) { return true; }   // (vk_shader.cpp's: none on the shelf here)
// (the stand-in driver: the handles alive, and the programs whose shader libraries its caches have)
std::mutex live_mu;
std::set<VkPipeline> live;
std::atomic<int> destroyed{0}, bad_destroys{0}, builds{0};
std::set<uint64_t> driver_has;
std::function<void()> during_build;   // (set only while one thread runs)
VkPipeline next_handle = 100;
inline void vkDestroyPipeline(int, VkPipeline h, void*) {
    std::lock_guard lk(live_mu);
    if (!live.erase(h)) bad_destroys++;
    destroyed++;
}
inline bool alive(VkPipeline h) {
    std::lock_guard lk(live_mu);
    return live.count(h) != 0;
}
VkPipeline build_graphics(const Program& p, const NvnState&, const Targets&, VkPrimitiveTopology, int, bool* cold = nullptr) {
    builds++;
    if (during_build) during_build();
    if (cold && !driver_has.count(p.key)) return *cold = true, VK_NULL_HANDLE;
    std::lock_guard lk(live_mu);
    const VkPipeline h = next_handle++;
    live.insert(h);
    return h;
}
@PART@
@WARM@
void order_line(const char*) {}   // (vk_draw.cpp's logs the order's counts)
@LETGO@
int settle() {   // (the give-back destroys on a thread of its own: until nothing more is destroyed)
    int last = -1;
    while (last != destroyed.load()) {
        last = destroyed.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return last;
}
int main() {
    using namespace pipe_warm;
    // ---- the header
    CHECK(level(nullptr) == 2 && level("") == 2 && level("0") == 0 && level("1") == 1 && level("2") == 2);
    CHECK(level("7") == 2 && level("-3") == 0 && level("off") == 0);
    CHECK(may_warm(2, true, true, false, false) && may_warm(1, true, true, false, false));
    CHECK(!may_warm(0, true, true, false, false) && !may_warm(2, false, true, false, false) && !may_warm(2, true, false, false, false));
    CHECK(!may_warm(2, true, true, true, false) && !may_warm(2, true, true, false, true));
    {
        std::unordered_map<uint64_t, std::vector<uint32_t>> index;
        for (uint32_t i = 0; i < 40; i++) index_add(index, 7, i);
        index_add(index, 8, 99);
        CHECK(index[7].size() == kNotesPerVariant && index[7].front() == 0 && index[7].back() == kNotesPerVariant - 1);
        CHECK(index[8].size() == 1 && index[8][0] == 99 && index.size() == 2);
    }
    CHECK(lib_room(0) && lib_room(kLibsMax - 1) && !lib_room(kLibsMax) && kLinksMax > 0 && kNotesPerVariant > 0);
    const uint64_t four[4] = {1, 2, 3, 4};
    {
        Held<int> h(3);
        std::vector<int> gone;
        h.put(10, 100, four, 2, gone);
        h.put(10, 101, four, 2, gone);   // (held already: the new one goes)
        CHECK(gone.size() == 1 && gone[0] == 101 && h.size() == 1);
        h.put(11, 110, four, 4, gone), h.put(12, 120, four, 4, gone), h.put(13, 130, four, 4, gone);   // (beyond 3: the oldest)
        CHECK(gone.size() == 2 && gone[1] == 100 && h.size() == 3);
        Held<int>::One one;
        CHECK(!h.take(10, one) && h.take(12, one) && one.link == 120 && one.key == 12 && one.n == 4 && one.libs[3] == 4);
        CHECK(!h.take(12, one));
        h.put(14, 140, four, 9, gone);   // (more than four libraries: four)
        CHECK(h.take(14, one) && one.n == 4);
        std::vector<int> all;
        h.all(all);
        CHECK(all.size() == 2 && all[0] == 110 && all[1] == 130 && h.size() == 0 && !h.take(11, one));
    }
    {   // (at random, against a plain list)
        std::mt19937 rng(11);
        Held<int> m(16);
        std::vector<std::pair<uint64_t, int>> ref;
        int made = 0;
        for (int i = 0; i < 20000; i++) {
            const uint64_t k = rng() % 40;
            const auto it = std::find_if(ref.begin(), ref.end(), [&](const auto& e) { return e.first == k; });
            if (rng() % 3) {
                std::vector<int> gone, want;
                const int l = ++made;
                if (it != ref.end()) {
                    want.push_back(l);
                } else {
                    ref.push_back({k, l});
                    while (ref.size() > 16) want.push_back(ref.front().second), ref.erase(ref.begin());
                }
                m.put(k, l, four, 1, gone);
                CHECK(gone == want);
            } else {
                Held<int>::One o;
                const bool got = m.take(k, o);
                CHECK(got == (it != ref.end()));
                if (got) {
                    CHECK(o.link == it->second);
                    ref.erase(it);
                }
            }
            CHECK(m.size() == ref.size());
        }
    }
    // ---- (test18) following the draws
    CHECK(on(nullptr) && on("") && on("1") && !on("0") && on("00"));
    CHECK(!opt_in(nullptr) && !opt_in("") && opt_in("1") && !opt_in("0") && !opt_in("11"));   // ((test19) off unless 1)
    {   // ((test19) the warm's time: what it spent comes back at per_second, burst at most)
        Budget b(250, 500);
        CHECK(b.allows(1000));
        b.spent(600, 1000);
        CHECK(!b.allows(1000) && !b.allows(1300) && b.allows(1500));   // (-100, +75 by 1300, +125 by 1500)
        b.spent(10000, 1500);
        CHECK(!b.allows(5000) && !b.allows(41000) && b.allows(42000));   // (25 - 10000 back at 250 a second)
        b.spent(0, 1000000);
        b.spent(499, 1000000);
        CHECK(b.allows(1000000));   // (saved up to 500 at most: 1 left)
        Once o;
        CHECK(o.first(7) && !o.first(7) && o.done(7) && !o.done(8) && o.first(8) && o.size() == 2);
    }
    CHECK(kLibsKeep > kLibsMax && kLibsKeep > 2 * kLinksMax && kLinkAhead < kLibAhead);   // (the libraries kept longer than links)
    CHECK(want(-1) == Want::none && want(0) == Want::links && want(kLinkAhead) == Want::links);
    CHECK(want(kLinkAhead + 1) == Want::libraries && want(kLibAhead) == Want::libraries && want(kLibAhead + 1) == Want::none);
    {
        Front f;
        CHECK(f.ahead(100, 1000) == -1);   // (the draws took nothing yet)
        f.consumed(-1, 1000);              // (a program the order does not name: no place)
        CHECK(f.ahead(100, 1000) == -1);
        f.consumed(90, 1000), f.consumed(400, 1500);
        CHECK(f.ahead(100, 1600) == 10 && f.ahead(90, 1600) == 0 && f.ahead(89, 1600) == -1 && f.ahead(450, 1600) == 50);
        CHECK(f.ahead(100, 1000 + kRecentMs + 1) == -1 && f.ahead(450, 1000 + kRecentMs + 1) == 50);   // (90 is too old now)
        CHECK(f.ahead(450, 1500 + kRecentMs + 1) == -1);   // (the draws stopped taking: nothing)
        for (int i = 0; i < (int)kPoints; i++) f.consumed(5000 + i, 9000);   // (the oldest places give way to the newest)
        CHECK(f.ahead(401, 9000) == -1 && f.ahead(5100, 9000) == 5100 - (5000 + (int)kPoints - 1));
    }
    {
        Made m;
        std::set<uint64_t> mine = {1, 2, 3, 4};
        for (uint64_t k = 1; k <= 4; k++) m.add(k);
        mine.erase(1);   // (a draw had 1: passed over)
        uint64_t k = 0;
        const auto still = [&](uint64_t key) { return mine.count(key) != 0; };
        CHECK(m.oldest(k, still) && k == 2 && m.oldest(k, still) && k == 3);
        mine.erase(4);
        CHECK(!m.oldest(k, still) && m.size() == 0);
        for (uint64_t i = 0; i < 100; i++) m.add(i);
        m.trim(10, [](uint64_t key) { return key % 2 == 0; });
        CHECK(m.size() == 50);   // (over four times 10: only those still the warm's kept)
        m.trim(100, [](uint64_t) { return false; });
        CHECK(m.size() == 50);   // (under: left as it is)
    }
    {
        Held<int> h(8);
        std::vector<int> gone;
        const uint64_t ab[2] = {5, 6}, bc[2] = {6, 7}, cd[2] = {7, 8};
        h.put(1, 10, ab, 2, gone), h.put(2, 20, bc, 2, gone), h.put(3, 30, cd, 2, gone);
        h.drop_using(6, gone);
        Held<int>::One one;
        CHECK(gone.size() == 2 && gone[0] == 10 && gone[1] == 20 && h.size() == 1 && h.take(3, one) && !h.take(1, one));
    }
    // ---- vk_draw.cpp's map of stage libraries, with the warm's
    NvnState s;
    Targets at;
    const Program a{1}, b{2};
    driver_has = {1};
    uint64_t cold = 0;
    {
        const std::shared_lock wl(warm_mu);
        // (made from the caches: kept, the warm's own until a draw has it)
        const uint64_t ka = mix(11, 1);
        const VkPipeline la = warm_part(a, s, at, 0, 1, ka);
        CHECK(la && alive(la) && gpl_libs.at(ka) == la && gpl_warm_only.count(ka) && warm_holds.load() && st_warm_libs == 1);
        CHECK(warm_part(a, s, at, 0, 1, ka) == la && st_warm_libs == 1);   // (there: found, not made again)
        // (the driver would have had to compile it: nothing kept, the draw asks for it as before)
        const uint64_t kb = mix(21, 1);
        CHECK(!warm_part(b, s, at, 0, 1, kb) && st_warm_cold == 1 && !gpl_libs.count(kb));
        CHECK(warm_part(b, s, at, 0, 0, mix(20, 0)));   // (one without shaders: made without asking)
    }
    // (the draw finds the warm's: the draw's from then on)
    CHECK(gpl_part(a, s, at, 0, 1, 11, &cold) == gpl_libs.at(mix(11, 1)) && !cold && !gpl_warm_only.count(mix(11, 1)));
    // (one the warm left out: cold in the draw, as without the warm)
    CHECK(!gpl_part(b, s, at, 0, 1, 21, &cold) && cold == mix(21, 1));
    {   // (the warm makes one while the draw makes the same: the warm's stays, and it is the draw's)
        const uint64_t k = mix(12, 2);
        VkPipeline w = 0;
        bool once = true;
        during_build = [&] {
            if (!once) return;
            once = false;
            const std::shared_lock wl(warm_mu);
            w = warm_part(a, s, at, 0, 2, k);
        };
        const int d0 = destroyed;
        const VkPipeline d = gpl_part(a, s, at, 0, 2, 12, &cold);
        during_build = nullptr;
        CHECK(w && d == w && alive(w) && !gpl_warm_only.count(k) && destroyed == d0 + 1);
    }
    // (a link the warm made: taken once by its draw, and its libraries are the draw's with it)
    const uint64_t l1 = mix(30, 1), l2 = mix(30, 2);
    VkPipeline taken = 0;
    {
        const std::shared_lock wl(warm_mu);
        CHECK(warm_part(a, s, at, 0, 1, l1) && warm_part(a, s, at, 0, 2, l2));
        const uint64_t libs[2] = {l1, l2};
        warm_hold(777, build_graphics(a, s, at, 0, -1), libs, 2);
        CHECK(st_warm_links == 1 && gpl_warm_only.count(l1) && gpl_warm_only.count(l2));
    }
    taken = warm_link_take(777);
    CHECK(taken && alive(taken) && !warm_link_take(777) && !warm_link_take(778) && st_warm_taken == 1);
    CHECK(!gpl_warm_only.count(l1) && !gpl_warm_only.count(l2));
    {   // (beyond kLinksMax the oldest goes, destroyed: no draw had it)
        const int d0 = destroyed;
        std::vector<VkPipeline> first;
        for (size_t i = 0; i < kLinksMax + 5; i++) {
            const VkPipeline l = build_graphics(a, s, at, 0, -1);
            if (i < 5) first.push_back(l);
            warm_hold(1000 + i, l, nullptr, 0);
        }
        CHECK(destroyed == d0 + 5 && st_warm_dropped == 5 && warm_links.size() == kLinksMax);
        for (const VkPipeline l : first) CHECK(!alive(l));
        CHECK(!warm_link_take(1000) && warm_link_take(1005));
    }
    if (!warm_follow()) {   // (RX_VK_PIPELINE_WARM_FOLLOW=0: no library beyond kLibsMax that no draw has had)
        const std::shared_lock wl(warm_mu);
        for (uint64_t i = 0; gpl_warm_only.size() < kLibsMax; i++) CHECK(warm_part(a, s, at, 0, 0, mix(5000, i)));
        const int b0 = builds;
        CHECK(!warm_part(a, s, at, 0, 0, mix(9999, 0)) && builds == b0 && !gpl_libs.count(mix(9999, 0)));
    } else {   // (following the draws: at kLibsKeep the oldest no draw has had goes, with the links made from it, and is destroyed
               // only by the presents' warm_let_go, when no warm has libraries in hand)
        const uint64_t oldest = mix(20, 0);   // (b's vertex input: the oldest the warm made that no draw has had)
        const VkPipeline old_lib = gpl_libs.at(oldest), old_link = build_graphics(a, s, at, 0, -1);
        warm_hold(4242, old_link, &oldest, 1, &old_lib);
        {
            const std::shared_lock wl(warm_mu);
            for (uint64_t i = 0; gpl_warm_only.size() < kLibsKeep; i++) CHECK(warm_part(a, s, at, 0, 0, mix(5000, i)));
            CHECK(gpl_libs.count(oldest) && warm_dead.empty());
            CHECK(warm_part(a, s, at, 0, 0, mix(9999, 0)) && gpl_warm_only.size() == kLibsKeep);
            CHECK(!gpl_libs.count(oldest) && !gpl_warm_only.count(oldest) && st_warm_lib_oldest == 1 && warm_dead.size() == 2);
            CHECK(alive(old_lib) && alive(old_link) && !warm_link_take(4242));
            const int b0 = builds;   // ((test19) let go once: not made again by the warm; a draw makes its own)
            CHECK(!warm_part(b, s, at, 0, 0, oldest) && builds == b0 && st_warm_not_again == 1 && !gpl_libs.count(oldest));
            warm_let_go();   // (a warm has libraries in hand: nothing destroyed)
            CHECK(warm_dead.size() == 2);
        }
        // (a link made from a library let go meanwhile is not held: it goes with it)
        const uint64_t gone_key = mix(77777, 0);
        const VkPipeline stale = 31337;
        warm_hold(4343, build_graphics(a, s, at, 0, -1), &gone_key, 1, &stale);
        CHECK(!warm_link_take(4343) && warm_dead.size() == 3);
        const int d0 = destroyed;
        warm_let_go();
        CHECK(warm_dead.empty() && settle() == d0 + 3 && !alive(old_lib) && !alive(old_link) && !bad_destroys);
    }
    {   // (the RAM's give-back: not while the RAM is not short, not while a warm has libraries in hand; then every link held
        // and every library no draw has had, none a draw has)
        const size_t links = warm_links.size(), libs = gpl_warm_only.size();
        const std::vector<VkPipeline> drawn = {gpl_libs.at(mix(11, 1)), gpl_libs.at(mix(12, 2)), gpl_libs.at(l1), taken};
        warm_let_go();
        CHECK(warm_links.size() == links && gpl_warm_only.size() == libs);
        ram_short = true;
        {
            const std::shared_lock wl(warm_mu);
            warm_let_go();
            CHECK(warm_links.size() == links && gpl_warm_only.size() == libs);
        }
        const int d0 = destroyed;
        warm_let_go();
        CHECK(warm_links.size() == 0 && gpl_warm_only.empty() && !warm_holds.load() && gpl_libs.count(mix(11, 1)) && gpl_libs.count(l1));
        CHECK(settle() == d0 + (int)(links + libs) && !bad_destroys && st_warm_freed == (int64_t)(links + libs));
        for (const VkPipeline h : drawn) CHECK(alive(h));
        CHECK(!warm_link_take(1006));
        ram_short = false;
    }
    {   // (four warms, a draw and the presents at once: nothing a draw has or a warm has in hand is destroyed, nothing twice)
        std::atomic<bool> stop{false};
        std::atomic<int> in_hand_dead{0};
        std::mutex drawn_mu;
        std::unordered_set<VkPipeline> drawn;
        std::vector<std::thread> threads;
        const int64_t freed0 = st_warm_freed;
        for (int t = 0; t < 4; t++)
            threads.emplace_back([&, t] {
                std::mt19937 rng(100 + t);
                while (!stop) {
                    if (memory_ram_short()) {   // (warm_variant: nothing made while the RAM is short)
                        std::this_thread::yield();
                        continue;
                    }
                    const uint64_t keys[2] = {mix(rng() % 300 + 100000, 1), mix(rng() % 300 + 100000, 2)};
                    const std::shared_lock wl(warm_mu);
                    VkPipeline libs[2];
                    bool all = true;
                    for (int i = 0; i < 2 && all; i++) all = (libs[i] = warm_part(a, s, at, 0, 1 + i, keys[i])) != VK_NULL_HANDLE;
                    if (!all) continue;
                    for (const VkPipeline l : libs)
                        if (!alive(l)) in_hand_dead++;
                    warm_hold(rng() % 600 + 100000, build_graphics(a, s, at, 0, -1), keys, 2);
                }
            });
        threads.emplace_back([&] {   // (the draw)
            std::mt19937 rng(7);
            while (!stop) {
                uint64_t c = 0;
                if (const VkPipeline l = gpl_part(a, s, at, 0, 1 + (int)(rng() % 2), rng() % 300 + 100000, &c)) {
                    std::lock_guard lk(drawn_mu);
                    drawn.insert(l);
                }
                if (const VkPipeline l = warm_link_take(rng() % 600 + 100000)) {
                    std::lock_guard lk(drawn_mu);
                    drawn.insert(l);
                }
            }
        });
        threads.emplace_back([&] {   // (the presents, the RAM short now and then)
            std::mt19937 rng(9);
            while (!stop) {
                ram_short = rng() % 4 == 0;
                warm_let_go();
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        });
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500) ||
               (st_warm_freed == freed0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)))
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop = true;
        for (auto& t : threads) t.join();
        ram_short = false;
        settle();
        CHECK(st_warm_freed > freed0 && !bad_destroys && !in_hand_dead && !drawn.empty());
        for (const VkPipeline l : drawn) CHECK(alive(l));
        for (const auto& [k, l] : gpl_libs) CHECK(!l || alive(l));
    }
    // ---- ((wp18) the last sessions' order of pipelines, pipe_order.h: with one, the warm does not follow and has the order's
    // bound of libraries; a link held by its place is taken once by its draw, its libraries with it; beyond the bound the
    // farthest ahead goes, and none is made farther than the farthest held; the draws gone far past a link let it go with the
    // libraries only it held (destroyed at the presents, nothing a draw or another link has); the RAM's give-back takes the
    // order's links too
    pipe_order_used = true;
    CHECK(!warm_follow() && warm_libs_most() == pipe_order::kLibsMax);
    const Program c{3};
    driver_has.insert(3);
    {
        const uint64_t o1 = mix(40, 1), o2 = mix(40, 2);
        {
            const std::shared_lock wl(warm_mu);
            const VkPipeline libs[2] = {warm_part(c, s, at, 0, 1, o1), warm_part(c, s, at, 0, 2, o2)};
            CHECK(libs[0] && libs[1] && order_wanted(5));
            const uint64_t keys[2] = {o1, o2};
            warm_hold(5005, build_graphics(c, s, at, 0, -1), keys, 2, libs, 5);
            CHECK(gpl_warm_only.count(o1) && gpl_warm_only.count(o2) && pipe_order_links.size() == 1 && warm_holds.load());
        }
        const int64_t t0 = st_order_taken, w0 = st_warm_taken;
        const VkPipeline got = warm_link_take(5005);
        CHECK(got && alive(got) && st_order_taken == t0 + 1 && st_warm_taken == w0 + 1);
        CHECK(!gpl_warm_only.count(o1) && !gpl_warm_only.count(o2) && !warm_link_take(5005) && pipe_order_links.size() == 0);
    }
    std::vector<VkPipeline> made;   // (links held by their places; the last three beyond the bound)
    {
        const int64_t b0 = st_order_beyond;
        for (size_t i = 0; i < pipe_order::kLinksMax + 3; i++) {
            made.push_back(build_graphics(c, s, at, 0, -1));
            warm_hold(20000 + i, made.back(), nullptr, 0, nullptr, 10000 + (int64_t)i);
        }
        CHECK(pipe_order_links.size() == pipe_order::kLinksMax && st_order_beyond == b0 + 3);   // (the farthest: the last three)
        CHECK(!order_wanted(10000 + (int64_t)pipe_order::kLinksMax) && order_wanted(9999));
        CHECK(st_order_beyond == b0 + 4);   // (one not made beyond the bound: counted with those let go)
        const VkPipeline nearer = build_graphics(c, s, at, 0, -1);
        warm_hold(19999, nearer, nullptr, 0, nullptr, 9999);   // (nearer than the farthest held: that one goes)
        CHECK(st_order_beyond == b0 + 5 && warm_link_take(19999) == nearer);
        CHECK(!warm_link_take(20000 + pipe_order::kLinksMax - 1) && warm_link_take(20000) == made[0]);
    }
    {
        const uint64_t a1 = mix(50, 1), a2 = mix(50, 2), shared = mix(51, 3), b1 = mix(60, 1);
        VkPipeline la1 = 0, la2 = 0, lsh = 0, lb1 = 0, l1 = 0, l2 = 0, l3 = 0;
        {
            const std::shared_lock wl(warm_mu);
            la1 = warm_part(c, s, at, 0, 1, a1), la2 = warm_part(c, s, at, 0, 2, a2), lsh = warm_part(c, s, at, 0, 3, shared);
            lb1 = warm_part(c, s, at, 0, 1, b1);
            const uint64_t k1[2] = {a1, shared}, k2[2] = {a2, shared};
            const VkPipeline h1[2] = {la1, lsh}, h2[2] = {la2, lsh};
            warm_hold(30001, l1 = build_graphics(c, s, at, 0, -1), k1, 2, h1, 3);
            warm_hold(30002, l2 = build_graphics(c, s, at, 0, -1), k2, 2, h2, 4000);
            warm_hold(30003, l3 = build_graphics(c, s, at, 0, -1), &b1, 1, &lb1, 6);
        }
        uint64_t cold3 = 0;   // (a draw links its own from b1 meanwhile: b1 is the draw's from then on)
        CHECK(la1 && la2 && lsh && lb1 && order_wanted(100) && gpl_part(c, s, at, 0, 1, 60, &cold3) == lb1 && !gpl_warm_only.count(b1));
        // (the draws need 16 pipelines of the order's places 3000 on for the first time: where they are moves there)
        const int64_t behind0 = st_order_behind, libs0 = st_order_libs_gone;
        for (uint64_t q = 3000; q < 3000 + pipe_order::kRecent; q++) {
            pipe_order_index.place[70000 + q] = (int64_t)q;
            order_met(70000 + q);
        }
        CHECK(pipe_order_front.furthest >= 3000 && pipe_order_rec.keys.size() >= pipe_order::kRecent);
        CHECK(!warm_link_take(30001) && !warm_link_take(30003) && st_order_behind == behind0 + 2 && st_order_libs_gone == libs0 + 1);
        CHECK(!gpl_libs.count(a1) && !gpl_warm_only.count(a1) && gpl_libs.count(shared) && gpl_warm_only.count(shared) && gpl_libs.count(a2));
        CHECK(gpl_libs.count(b1) && gpl_libs.at(b1) == lb1);   // (the draw's stays)
        CHECK(!order_wanted(100) && order_wanted(3000));   // (behind the draws: not made)
        CHECK(alive(la1) && alive(l1));   // (destroyed at the presents, once no warm has libraries in hand)
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        {
            const std::shared_lock wl(warm_mu);
            warm_let_go();
            CHECK(alive(la1) && alive(l1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        warm_let_go();
        settle();
        CHECK(!alive(la1) && !alive(l1) && !alive(l3) && alive(lsh) && alive(la2) && alive(l2) && alive(lb1) && !bad_destroys);
        for (size_t i = pipe_order::kLinksMax - 1; i < made.size(); i++) CHECK(!alive(made[i]));   // (those let go beyond the bound)
        CHECK(alive(made[0]) && alive(made[1]));   // (a draw's, and one still held)
        // (the RAM short: the order's links held go back too, with every library no draw has had)
        const size_t held = pipe_order_links.size();
        ram_short = true;
        warm_let_go();
        settle();
        ram_short = false;
        CHECK(held > 1 && pipe_order_links.size() == 0 && gpl_warm_only.empty() && !alive(l2) && !alive(lsh) && alive(lb1) && !bad_destroys);
        CHECK(!warm_link_take(30002));
    }
    return 0;
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def check_source(draw, shader):
    """Where the safety is: the worker has the variants alone when it warms them, and the draw takes what the warm made"""
    worker = between(shader, "void readahead_main() {", "\n}\n")
    read = worker.index("program_cache_read(in, nullptr, k, recs, made);")
    warm = worker.index("pipelines_warm(made, at);")
    lock = worker.index("std::lock_guard lk(ra_mu);", read)
    park = worker.index("ra_parked[k] = std::move(made)")
    if not read < warm < lock < park:
        raise SystemExit("vk_shader.cpp readahead_main: the variants are not warmed after the read and before they are parked "
                         "(a draw could have them meanwhile), or under the read-ahead's lock")
    header = (ROOT / "gpu/vk/vk_shader.h").read_text(encoding="utf-8")
    for need in ("void pipelines_warm(std::vector<std::unique_ptr<Program>>& variants, int64_t at);",
                 "void pipelines_warm_consumed(int64_t at);", "bool readahead_computes_let_go(std::vector<VkPipeline>& out);"):
        if need not in header:
            raise SystemExit("vk_shader.h does not declare: " + need)
    if "at = program_readahead::index_of(ra_note_index, k);" not in worker[:read]:
        raise SystemExit("vk_shader.cpp readahead_main does not hand the warm the program's place in the order")
    ask = between(shader, "void readahead_ask(uint64_t key, int64_t at) {", "\n}\n")
    if "pipelines_warm_consumed(note);" not in ask:
        raise SystemExit("vk_shader.cpp readahead_ask does not tell the warm where the draws are")
    drop = between(shader, "void readahead_drop(", "\n}\n")
    if "if (p->compute_pipeline) vkDestroyPipeline(dev.device, p->compute_pipeline, nullptr)" not in drop:
        raise SystemExit("vk_shader.cpp readahead_drop keeps the warm's compute pipeline of a program let go")
    shelf = between(shader, "bool readahead_computes_let_go(std::vector<VkPipeline>& out) {", "\n}\n")
    for need in ("std::unique_lock lk(ra_mu, std::try_to_lock);", "for (auto& [k, variants] : ra_parked)",
                 "p->compute_pipeline = VK_NULL_HANDLE;", "p->compute_state.store(0, std::memory_order_release);"):
        if need not in shelf:
            raise SystemExit("vk_shader.cpp readahead_computes_let_go lacks: " + need)
    if 'getenv("RX_VK_PIPELINE_WARM")' not in between(draw, "int warm_level() {", "\n}\n"):
        raise SystemExit("vk_draw.cpp warm_level does not read RX_VK_PIPELINE_WARM")
    pw = between(draw, "void pipelines_warm(std::vector<std::unique_ptr<Program>>& variants, int64_t at) {", "\n}\n")
    for need in ("if (!level || memory_ram_low() || memory_ram_short()) return;",
                 "warm_follow() ? pipe_warm::want(warm_front.ahead(at, warm_ms())) : pipe_warm::Want::links",
                 "pipe_warm::may_warm(level, gpl_wanted(), gpl_ask_only(), false, false)",
                 "const bool links = level >= 2 && want == pipe_warm::Want::links;", "warm_compute(*p);",
                 "warm_index.erase(it);", 'fopen(pipe_notes_path().c_str(), "rb")', "ub_thread_off();",
                 # ((wp18) the order's places with its records: tests/test_pipe_order.py)
                 "warm_variant(*p, in, recs, links, places.empty() ? nullptr : places.data());"):
        if need not in pw:
            raise SystemExit("vk_draw.cpp pipelines_warm lacks: " + need)
    var = between(draw, "void warm_variant(Program& p, FILE* in, const std::vector<uint32_t>& recs, bool links, "
                        "const int64_t* places = nullptr) {", "\n}\n")
    loop = var.index("for (size_t r = 0; r < recs.size(); r++) {")
    for need in ("p.failed || p.compute || p.fallback || p.stages.empty() || gpl_skipped(p)",
                 "if (prepare_stopped() || memory_ram_low() || memory_ram_short()) return;",
                 "n->prog != p.key || n->variant != p.variant", "gpl_keys(p, n->s, at, topo, keys);", "ensure_layout(p);",
                 "if (!all || !links) continue;", "pipeline_key(p, n->s, at, topo)", "gpl_link_libs(p, libs, m)",
                 "warm_hold(key, link, lib_keys, m, libs, place);"):
        if need not in var:
            raise SystemExit("vk_draw.cpp warm_variant lacks: " + need)
    if var.index("memory_ram_short()) return;") < loop or \
            not var.index("const std::shared_lock wl(warm_mu);") < var.index("warm_part(") < var.index("gpl_link_libs("):
        raise SystemExit("vk_draw.cpp warm_variant: the RAM not asked at each note, or libraries in hand outside warm_mu")
    link = between(draw, "VkPipeline gpl_link(", "\n}\n")
    if "gpl_keys(p, s, at, topo, keys);" not in link or "return gpl_link_libs(p, libs, n);" not in link:
        raise SystemExit("vk_draw.cpp gpl_link does not take its keys and its link where the warm does")
    gp = between(draw, "PipeEntry* graphics_pipeline(", "\n}\n")
    new = gp.index("PipeEntry* e = pipelines.emplace(key,")
    take = gp.index("VkPipeline linked = warm_link_take(key);")
    # ((Triforce) or left to a worker by the frame's link budget: tests/test_pipe_wait.py)
    own = gp.index("if (!linked && !link_later) linked = gpl_link(p, s, at, topo, cold), st_links++;")
    if not new < take < own < gp.index("gpl_rules::linked(*e, linked);"):
        raise SystemExit("vk_draw.cpp graphics_pipeline: the warm's link is not taken for a new entry before one of its own")
    tick = between(draw, "void pipelines_release_tick(uint64_t presents) {", "\n}\n")
    if tick.index("warm_let_go();") > tick.index("if (!on ||"):
        raise SystemExit("vk_draw.cpp pipelines_release_tick: the warm's give-back is not asked at each present")
    let_go = between(draw, "void warm_let_go() {", "\n}\n")
    for need in ("memory_ram_short()", "std::unique_lock wl(warm_mu, std::try_to_lock);", "std::thread([gone] {"):
        if need not in let_go:
            raise SystemExit("vk_draw.cpp warm_let_go lacks: " + need)
    pre = between(draw, "void precompile_run(void (*progress)(size_t done, size_t total)) {", "\n}\n")
    if not pre.index("std::vector<std::unique_ptr<PipeNote>> notes = pipe_notes_read(&note_at);") < \
            pre.index("if (light) warm_index_set(notes, note_at, computes_listed);") < pre.index("std::erase_if(notes,"):
        raise SystemExit("vk_draw.cpp precompile_run: the notes' records are not kept for the warm before the built ones go")
    notes = between(draw, "std::vector<std::unique_ptr<PipeNote>> pipe_notes_read(", "\n}\n")
    if "for (uint32_t i = 0;; i++) {" not in notes or "if (at) at->push_back(i);" not in notes:
        raise SystemExit("vk_draw.cpp pipe_notes_read: a note's record is not its place in the file")
    at = between(draw, "bool pipe_note_at(FILE* f, uint32_t rec, PipeNote& n) {", "\n}\n")
    if "16 + (int64_t)rec * (int64_t)sizeof(PipeNote)" not in at or "note_sane(n)" not in at:
        raise SystemExit("vk_draw.cpp pipe_note_at does not read the record after the head, or takes one not sane")
    index = between(draw, "void warm_index_set(", "\n}\n")
    if "pipe_warm::index_add(warm_index, mix(notes[i]->prog, notes[i]->variant), at[i]);" not in index or \
            "gpl_ask_only()" not in index:
        raise SystemExit("vk_draw.cpp warm_index_set: not by program variant, or kept where the warm never runs")
    # ((test18) the compute pipelines: as the dispatch makes them, of the caches only, the listed ones, kept on the program)
    comp = between(draw, "void warm_compute(Program& p) {", "\n}\n")
    for need in ('pipe_warm::opt_in(getenv("RX_VK_COMPUTE_WARM"))', "dev.vk13.pipelineCreationCacheControl", "taau_key(p.key)",
                 "!warm_computes.count(p.key) || !warm_computes_made.first(mix(p.key, p.variant))", "VkComputePipelineCreateInfo ci = compute_create_info(p);",
                 "ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;",
                 "guarded_compute(pcache_use().cache, &ci, &made)", "p.compute_pipeline = made;",
                 "p.compute_state.store(2, std::memory_order_release);"):
        if need not in comp:
            raise SystemExit("vk_draw.cpp warm_compute lacks: " + need)
    disp = between(draw, "void dispatch(VkCommandBuffer cmd, NvnState& s,", "\n}\n")
    if "const auto info = [](const Program& q) { return compute_create_info(q); };" not in disp or \
            disp.index("if (!compute_wait && !p.compute_pipeline") > disp.index("compute_create_info("):
        raise SystemExit("vk_draw.cpp dispatch does not make its compute pipeline as the warm does, or makes one it has")
    if 'if (pipe_warm::opt_in(getenv("RX_VK_COMPUTE_WARM"))) warm_computes.insert(computes.begin(), computes.end());' not in index:
        raise SystemExit("vk_draw.cpp warm_index_set does not keep the compute programs play dispatched")
    for need in ("readahead_computes_let_go(*gone)", "gone->swap(warm_dead);", "if (!give_back && warm_ms() < reaped + 500) return;"):
        if need not in let_go:
            raise SystemExit("vk_draw.cpp warm_let_go lacks: " + need)
    part = between(draw, "VkPipeline warm_part(", "\n}\n")
    if "(!warm_follow() || !warm_oldest_go_locked())" not in part or "warm_made.add(key);" not in part:
        raise SystemExit("vk_draw.cpp warm_part: following the draws, the oldest library does not give way")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    # ((test19) off by default, opt-in; the budget, following the draws only; a library let go not made again by the warm)
    if 'pipe_warm::opt_in(getenv("RX_VK_PIPELINE_WARM_FOLLOW"))' not in between(draw, "bool warm_follow() {", "\n}\n"):
        raise SystemExit("vk_draw.cpp warm_follow is not off by default")
    if "if (warm_follow() && !warm_budget.allows(warm_ms())) {" not in pw or pw.index("warm_budget.allows") > pw.index("warm_compute(*p);"):
        raise SystemExit("vk_draw.cpp pipelines_warm: the warm's time is not bounded before it makes anything")
    if "if (warm_gone.count(key)) return st_warm_not_again++, VK_NULL_HANDLE;" not in part:
        raise SystemExit("vk_draw.cpp warm_part makes again a library it let go")
    # ((test19) the stall line splits the dispatches' time and counts the waits for the read-ahead)
    sw = (ROOT / "gpu/vk/stall_watch.h").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    if "the dispatches' program picks %.1f ms, their first layouts and pipelines %.1f ms" not in sw or \
            "waits for programs being read ahead" not in sw or "dispatch_split_stats(&dp_pick, &dp_pipe, &dp_ready, &dp_made);" not in replay \
            or "readahead_wait_stats(&raw_n, &raw_ns);" not in replay:
        raise SystemExit("the stall line does not split the dispatches' time, or lacks the waits for the read-ahead")
    disp_pick = disp.index("const Program* cp = program_for(s, ProgramKind::Compute, types);")
    if not disp.index("const auto t_pick = std::chrono::steady_clock::now();") < disp_pick < disp.index("st_disp_pick_ns +=") or \
            not disp.index("pipe_clock{") < disp.index("ensure_layout(p);") < disp.index("pipe_clock.stop();") < disp.index("end_pass(cmd);"):
        raise SystemExit("vk_draw.cpp dispatch: the program pick or the first layout and pipeline are not what is timed")
    for need in ("`RX_VK_PIPELINE_WARM=0` or `1`", "`RX_VK_PIPELINE_WARM_FOLLOW=1`", "`RX_VK_COMPUTE_WARM=1`"):
        if need not in dev:
            raise SystemExit("docs/development.md's switches lack " + need)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    shader = (ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8")
    check_source(draw, shader)
    part = between(draw, "std::mutex gpl_mu;\n", "/// (a worker's: a library gpl_part found cold")
    warm = between(draw, "std::shared_mutex warm_mu;\n", "/// (a read-ahead worker's, with p its own")
    let_go = between(draw, "void warm_let_go() {", "\n}\n") + "\n}\n"
    code = FIXTURE.replace("@PART@", part).replace("@WARM@", warm).replace("@LETGO@", let_go)
    with tempfile.TemporaryDirectory(prefix="pipe-warm-") as tmp:
        tmp = Path(tmp)
        (tmp / "t.cpp").write_text(code, encoding="utf-8")
        exe = tmp / "pipe-warm"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-pthread", "-I" + str(ROOT / "gpu/vk"),
                        str(tmp / "t.cpp"), "-o", str(exe)], check=True)
        for follow in ("1", "0"):   # (following the draws, the default; and RX_VK_PIPELINE_WARM_FOLLOW=0, as before)
            r = subprocess.run([str(exe)], capture_output=True, text=True, env=dict(os.environ, RX_VK_PIPELINE_WARM_FOLLOW=follow))
            if r.returncode:
                raise SystemExit("pipe_warm (RX_VK_PIPELINE_WARM_FOLLOW=%s): %s" % (follow, r.stdout.strip() or "exit %d" % r.returncode))
        # ((test18) the synthetic load: following the draws, the links made are taken, and none the load needs is let go)
        sim = tmp / "pipe-warm-sim"
        subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-I" + str(ROOT / "gpu/vk"),
                        str(ROOT / "tests/pipe_warm_sim.cpp"), "-o", str(sim)], check=True)
        for seed in ("18", "1", "2"):
            old, new = ([int(v) for v in subprocess.run([str(sim), follow, seed], capture_output=True, text=True,
                                                         check=True).stdout.split()] for follow in ("0", "1"))
            print("synthetic load, seed %s (links made, taken, needed after they were let go; libraries made, needed after they "
                  "were let go; pipelines drawn): every program read %s, following the draws %s" % (seed, old, new))
            (o_made, o_taken, o_lost, _, _, drawn), (n_made, n_taken, n_lost, _, n_lib_lost, _) = old, new
            if not (o_lost > 0 and n_lost == 0 and n_lib_lost == 0):
                raise SystemExit("synthetic load: following the draws let go a link or a library the load needed")
            if not (n_taken > 4 * o_taken and n_taken * 10 > 3 * n_made and n_taken * 10 > 3 * drawn):
                raise SystemExit("synthetic load: following the draws does not raise the links taken")
    print("PASS: a load's pipelines made behind its draws, bounded, given back while the RAM is short")


if __name__ == "__main__":
    main()
