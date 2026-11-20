// (Triforce, test18) A synthetic load for the warm's two ways (tests/test_pipe_warm.py; gpu/vk/pipe_warm.h, program_readahead.h).
// The read-ahead as vk_shader.cpp has it (four workers, the newest four asks, 32 programs read after each one the draws consume,
// a shelf of 512 that lets its oldest go), the warm as vk_draw.cpp has it (made at the read, before the shelf), and draws that
// consume a game's way: some menus, then a load of three areas at once (each a run of the notes' order, three in five of its
// programs drawn, a little out of order, a few programs from anywhere), then play in a fourth area. Each policy's counts are
// printed: links made, taken by their draws, and the ones a draw came for after the warm had let them go.
// pipe_warm_sim <follow 0|1> [seed]: one line, "made taken needed_let_go libs_made lib_needed_let_go drawn"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pipe_warm.h"
#include "program_readahead.h"

namespace ra = mkwvk::program_readahead;

struct Prog {
    int64_t at = 0;   // (its place: its first note's)
    int notes = 1;
};

struct Sim {
    bool follow = true;
    std::vector<Prog> progs;
    std::vector<std::pair<int64_t, uint64_t>> order;   // (place, program)
    std::unordered_set<uint64_t> listed;               // (on disk: no draw consumed it)
    std::deque<int64_t> asks;
    std::deque<uint64_t> pending;
    std::unordered_map<uint64_t, bool> parked;         // (program -> parked)
    std::deque<uint64_t> park_order;
    std::unordered_set<uint64_t> indexed;              // (variants the warm has not linked yet: warm_index)
    // the warm
    pipe_warm::Front front;
    pipe_warm::Held<int> held{pipe_warm::kLinksMax};
    pipe_warm::Made made_order;
    std::unordered_set<uint64_t> libs;       // (gpl_libs)
    std::unordered_set<uint64_t> warm_only;  // (gpl_warm_only)
    std::unordered_set<uint64_t> links_let_go, libs_let_go;
    std::unordered_set<uint64_t> links_ever;
    long made = 0, taken = 0, needed_let_go = 0, libs_made = 0, lib_needed_let_go = 0, drawn = 0;
    struct Worker {
        double free_at = 0;
        uint64_t reading = 0;
        bool busy = false;
    } workers[ra::kWorkers];

    static uint64_t lib_key(uint64_t prog, int i) { return prog * 4 + (uint64_t)i; }
    static uint64_t pipe_key(uint64_t prog, int j) { return prog * 64 + (uint64_t)j; }

    void build(std::mt19937& rng, int n) {
        int64_t at = 0;
        for (int i = 0; i < n; i++) {
            Prog p;
            p.at = at;
            const unsigned r = rng() % 10;
            p.notes = r < 6 ? 1 : r < 9 ? 2 : 3;
            at += p.notes;
            progs.push_back(p);
            order.push_back({p.at, (uint64_t)i});
            listed.insert((uint64_t)i);
            indexed.insert((uint64_t)i);
        }
    }
    pipe_warm::Budget budget;   // ((test19) following: the workers' time a second)
    long over_budget = 0;
    bool lib_made(uint64_t k) {   // (the warm's library: made, or there; false at the old bound)
        if (libs.count(k)) return true;
        if (follow && libs_let_go.count(k)) return false;   // ((test19) let go once: not made again by the warm)
        if (!pipe_warm::lib_room(warm_only.size(), follow ? pipe_warm::kLibsKeep : pipe_warm::kLibsMax)) {
            if (!follow) return false;
            uint64_t old = 0;
            if (!made_order.oldest(old, [&](uint64_t key) { return warm_only.count(key) != 0; })) return false;
            warm_only.erase(old), libs.erase(old), libs_let_go.insert(old);
            std::vector<int> gone;
            held.drop_using(old, gone);
            for (int g : gone) links_let_go.insert((uint64_t)g);
        }
        libs.insert(k), warm_only.insert(k), libs_made++;
        if (follow) made_order.add(k);
        return true;
    }
    void warm(uint64_t prog, uint64_t now) {
        const Prog& p = progs[prog];
        const pipe_warm::Want w = follow ? pipe_warm::want(front.ahead(p.at, now)) : pipe_warm::Want::links;
        if (w == pipe_warm::Want::none || !indexed.count(prog)) return;
        if (follow && !budget.allows(now)) return void(over_budget++);
        if (follow) budget.spent(0.15 * 2 + 0.25 * p.notes, now);   // (two libraries and a link a note: their tenths of a ms)
        const bool links = w == pipe_warm::Want::links;
        if (links) indexed.erase(prog);   // (once linked: warm_index)
        for (int j = 0; j < p.notes; j++) {
            const uint64_t l[2] = {lib_key(prog, 0), lib_key(prog, 1 + j % 2)};
            if (!lib_made(l[0]) || !lib_made(l[1]) || !links) continue;
            std::vector<int> gone;
            held.put(pipe_key(prog, j), (int)pipe_key(prog, j), l, 2, gone);
            made++, links_ever.insert(pipe_key(prog, j));
            for (int g : gone) links_let_go.insert((uint64_t)g);
        }
    }
    void park(uint64_t prog) {
        for (size_t n = ra::to_let_go(parked.size(), 1); n && !park_order.empty(); park_order.pop_front()) {
            if (parked.erase(park_order.front())) n--;
        }
        parked[prog] = true, park_order.push_back(prog);
    }
    void finish(Worker& w, uint64_t now) {
        w.busy = false;
        if (!listed.count(w.reading)) return;   // (a draw read it meanwhile)
        warm(w.reading, now);
        park(w.reading);
    }
    void workers_run(double t) {
        for (Worker& w : workers) {
            if (w.busy && w.free_at <= t) finish(w, (uint64_t)t);
            if (w.busy) continue;
            if (pending.empty() && !asks.empty()) {
                const int64_t at = asks.front();
                asks.pop_front();
                const auto wanted = [&](uint64_t k) {
                    if (!listed.count(k) || parked.count(k)) return false;
                    if (std::find(pending.begin(), pending.end(), k) != pending.end()) return false;
                    for (const Worker& o : workers)
                        if (o.busy && o.reading == k) return false;
                    return true;
                };
                for (uint64_t k : ra::next_keys(order, at, (size_t)ra::kDefault, wanted)) pending.push_back(k);
            }
            if (pending.empty()) continue;
            w.reading = pending.front(), pending.pop_front(), w.busy = true;
            w.free_at = t + 1.2 + 0.4 * progs[w.reading].notes;   // (a read about a millisecond, the warm a few tenths a note)
        }
    }
    void consume(uint64_t prog, double t) {
        if (!listed.count(prog)) return;
        const uint64_t now = (uint64_t)t;
        front.consumed(progs[prog].at, now);
        for (Worker& w : workers)   // (one being read: the draw waits for it)
            if (w.busy && w.reading == prog) finish(w, now);
        parked.erase(prog);
        listed.erase(prog);
        for (int j = 0; j < progs[prog].notes; j++) {
            drawn++;
            const uint64_t key = pipe_key(prog, j);
            pipe_warm::Held<int>::One one;
            if (held.take(key, one)) {
                taken++;
                for (uint32_t i = 0; i < one.n; i++) warm_only.erase(one.libs[i]);
                continue;
            }
            if (links_ever.count(key) && links_let_go.count(key)) needed_let_go++;
            for (const uint64_t l : {lib_key(prog, 0), lib_key(prog, 1 + j % 2)}) {
                if (libs.count(l)) warm_only.erase(l);
                else if (libs_let_go.count(l)) lib_needed_let_go++, libs.insert(l);
                else libs.insert(l);
            }
        }
        ra::ask(asks, progs[prog].at);
    }
};

int main(int argc, char** argv) {
    Sim sim;
    sim.follow = argc > 1 && argv[1][0] == '1';
    std::mt19937 rng(argc > 2 ? (unsigned)atoi(argv[2]) : 18);
    const int n = 6000;
    sim.build(rng, n);
    // the draws: (time in ms, program)
    std::vector<std::pair<double, uint64_t>> draws;
    double t = 0;
    for (int i = 0; i < 60; i++)   // (the menus: their programs at the start of the notes, slowly)
        if (rng() % 10 < 7) draws.push_back({t += 16, (uint64_t)i});
    t += 5000;
    const int areas[3] = {900, 2600, 4100};   // (a load: three areas at once, 350 programs each)
    std::vector<std::vector<uint64_t>> runs(3);
    for (int a = 0; a < 3; a++)
        for (int i = 0; i < 350; i++)
            if (rng() % 5 < 3) runs[a].push_back((uint64_t)(areas[a] + i));
    for (auto& r : runs)   // (a little out of order)
        for (size_t i = 0; i + 1 < r.size(); i++)
            if (rng() % 4 == 0) std::swap(r[i], r[std::min(r.size() - 1, i + 1 + rng() % 5)]);
    size_t at[3] = {0, 0, 0};
    for (bool more = true; more;) {
        more = false;
        for (int a = 0; a < 3; a++) {
            for (int k = 0; k < 15 && at[a] < runs[a].size(); k++) draws.push_back({t += 0.5, runs[a][at[a]++]});
            more |= at[a] < runs[a].size();
            if (rng() % 3 == 0) draws.push_back({t += 0.5, (uint64_t)(rng() % n)});   // (one from anywhere)
        }
    }
    t += 3000;
    for (int i = 0; i < 300; i++)   // (play: a fourth area, a frame at a time)
        if (rng() % 2) draws.push_back({t += 50, (uint64_t)(5300 + i)});
    double now = 0;
    for (const auto& [when, prog] : draws) {
        for (; now < when; now += 0.1) sim.workers_run(now);
        sim.consume(prog, when);
    }
    for (int i = 0; i < 100000; i++) sim.workers_run(now += 0.1);
    std::printf("%ld %ld %ld %ld %ld %ld\n", sim.made, sim.taken, sim.needed_let_go, sim.libs_made, sim.lib_needed_let_go,
                sim.drawn);
    return 0;
}
