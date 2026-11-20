// (Triforce, wp18) The pipelines made ahead in the order the last sessions' draws first needed them (gpu/vk/pipe_order.h): the
// file, the index by program variant, where the draws are, the links held by place with their bound, the libraries let go with
// them, and a session as the lab's S1 run through the program read-ahead's own rules (program_order.h), beside the notes' warm
// as it was (pipe_warm.h Held, the oldest let go). Run by tests/test_pipe_order.py.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pipe_order.h"
#include "pipe_warm.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

using namespace mkwvk;
namespace po = mkwvk::pipe_order;

static void file() {
    const std::vector<uint64_t> keys = {0x1111, 0xdeadbeefcafe, 7};
    const auto bytes = po::write(keys);
    CHECK(bytes.size() == 16 + 3 * 8 + 8);
    CHECK(po::read(bytes.data(), bytes.size()) == keys);
    // (a program order is not a pipeline order, and the other way: each file its own magic)
    const auto programs = program_order::write(keys);
    CHECK(po::read(programs.data(), programs.size()).empty());
    CHECK(program_order::read(bytes.data(), bytes.size()).empty());
    CHECK(program_order::read(programs.data(), programs.size()) == keys);
    auto cut = bytes;
    cut.pop_back();
    CHECK(po::read(cut.data(), cut.size()).empty());
    auto bad = bytes;
    bad[20] ^= 1;
    CHECK(po::read(bad.data(), bad.size()).empty());
    CHECK(po::merge({3, 1}, {9, 1, 8}) == (std::vector<uint64_t>{3, 1, 9, 8}));
    po::Record r;
    CHECK(r.add(5) && !r.add(5) && !r.add(0) && r.keys.size() == 1);
}

static void indexed() {
    // (the order: pipelines 10.. of variants by key / 100; 0 and a key met twice are not places; one without a note neither
    // indexed nor counted, but placed)
    const std::vector<uint64_t> order = {101, 0, 102, 201, 101, 999, 103, 202};
    po::Index ix;
    const auto note = [](uint64_t key, uint64_t* variant, uint32_t* rec) {
        if (key == 999) return false;
        *variant = key / 100, *rec = (uint32_t)(key * 2);
        return true;
    };
    ix.build(order, note, 2);   // (two a variant at most)
    CHECK(ix.named == 4 && ix.variants == 2);
    CHECK(ix.at(101) == 0 && ix.at(102) == 2 && ix.at(201) == 3 && ix.at(999) == 5 && ix.at(103) == 6 && ix.at(202) == 7);
    CHECK(ix.at(0) == -1 && ix.at(555) == -1);
    const auto one = ix.take(1);
    CHECK(one.size() == 2 && one[0].place == 0 && one[0].rec == 202 && one[1].place == 2 && one[1].rec == 204);   // (103 past 2)
    CHECK(ix.take(1).empty());   // (once a session)
    const auto two = ix.take(2);
    CHECK(two.size() == 2 && two[0].place == 3 && two[1].place == 7);
    CHECK(ix.take(9).empty());
    ix.build({}, note);
    CHECK(ix.named == 0 && ix.place.empty());
}

static void front() {
    po::Front f;
    CHECK(!f.passed(0) && !f.met(-1) && f.furthest == -1);
    for (int64_t q = 0; q < 100; q++) f.met(q);
    CHECK(f.furthest == 92);   // (the middle place of the last 16: 84 to 99)
    f.met(5000);
    CHECK(f.furthest < 100);   // (one met out of the order's turn moves it no further than the next in turn would)
    const int64_t was = f.furthest;
    CHECK(!f.met(10) && f.furthest == was);   // (nor back)
    for (int64_t q = 2000; q < 2000 + (int64_t)po::kRecent; q++) f.met(q);
    CHECK(f.furthest >= 2000 && f.from() == f.furthest - po::kBehind);
    CHECK(f.passed(f.from() - 1) && !f.passed(f.from()) && !f.passed(f.furthest + 10));
}

static void held() {
    const uint64_t libs[4] = {1, 2, 3, 4};
    po::Held<int> h(3);
    std::vector<po::Held<int>::One> gone;
    h.put(10, 50, 100, libs, 2, gone);
    h.put(10, 50, 101, libs, 2, gone);   // (held already: the new one goes)
    CHECK(gone.size() == 1 && gone[0].link == 101 && h.size() == 1);
    gone.clear();
    h.put(11, 10, 110, libs, 4, gone), h.put(12, 90, 120, libs, 4, gone);
    CHECK(gone.empty() && h.size() == 3 && h.room(80) && !h.room(90) && !h.room(95));   // (full: only nearer than 90 stays)
    h.put(13, 30, 130, libs, 4, gone);   // (beyond 3: the farthest ahead goes, not the oldest)
    CHECK(gone.size() == 1 && gone[0].key == 12 && gone[0].link == 120 && h.size() == 3);
    gone.clear();
    h.put(14, 200, 140, libs, 4, gone);   // (the new one is the farthest: it goes at once)
    CHECK(gone.size() == 1 && gone[0].key == 14 && h.size() == 3);
    po::Held<int>::One o;
    CHECK(h.take(13, o) && o.link == 130 && o.place == 30 && o.n == 4 && o.libs[3] == 4 && !h.take(13, o) && h.size() == 2);
    gone.clear();
    h.put(15, 20, 150, libs, 9, gone);   // (more than four libraries: four)
    CHECK(h.take(15, o) && o.n == 4);
    gone.clear();
    h.behind(50, gone);   // (those placed before 50: 11 at 10)
    CHECK(gone.size() == 1 && gone[0].key == 11 && h.size() == 1 && !h.take(11, o));
    const uint64_t ab[2] = {5, 6}, cd[2] = {7, 8};
    h.put(16, 60, 160, ab, 2, gone), h.put(17, 70, 170, cd, 2, gone);
    CHECK(h.uses(6) && h.uses(8) && !h.uses(9));
    gone.clear();
    h.drop_using(6, gone);
    CHECK(gone.size() == 1 && gone[0].key == 16 && !h.uses(6) && h.size() == 2);
    std::vector<po::Held<int>::One> all;
    h.all(all);
    CHECK(all.size() == 2 && all[0].key == 10 && all[1].key == 17 && h.size() == 0 && h.room(1000000));   // (the nearest first)
    po::Held<int> none(0);
    CHECK(!none.room(0));
    {   // (at random, against a plain list: the farthest goes beyond the bound, each taken once)
        std::mt19937 rng(18);
        po::Held<int> m(16);
        std::map<uint64_t, std::pair<int64_t, int>> ref;   // (key -> place, link)
        int made = 0;
        for (int i = 0; i < 20000; i++) {
            const uint64_t k = rng() % 60 + 1;
            const int64_t place = (int64_t)(k * 7 % 61);   // (a key's place is its own)
            const int r = (int)(rng() % 4);
            if (r < 2) {
                std::vector<po::Held<int>::One> g;
                const int l = ++made;
                m.put(k, place, l, libs, 1, g);
                std::vector<int> want;
                if (ref.count(k)) {
                    want.push_back(l);
                } else {
                    ref[k] = {place, l};
                    while (ref.size() > 16) {
                        auto far = ref.begin();
                        for (auto it = ref.begin(); it != ref.end(); ++it)
                            if (it->second.first > far->second.first) far = it;
                        want.push_back(far->second.second);
                        ref.erase(far);
                    }
                }
                CHECK(g.size() == want.size());
                for (size_t j = 0; j < g.size(); j++) CHECK(g[j].link == want[j]);
            } else if (r == 2) {
                po::Held<int>::One t;
                const bool got = m.take(k, t);
                CHECK(got == (ref.count(k) != 0));
                if (got) {
                    CHECK(t.link == ref[k].second);
                    ref.erase(k);
                }
            } else if (i % 50 == 0) {
                std::vector<po::Held<int>::One> g;
                const int64_t at = (int64_t)(rng() % 30);
                m.behind(at, g);
                size_t n = 0;
                for (auto it = ref.begin(); it != ref.end();)
                    if (it->second.first < at) it = ref.erase(it), n++;
                    else ++it;
                CHECK(g.size() == n);
            }
            CHECK(m.size() == ref.size());
        }
    }
}

static void libs() {
    struct L { uint32_t n; uint64_t libs[4]; };
    const std::vector<L> gone = {{3, {1, 2, 3}}, {2, {3, 4}}};
    const std::set<uint64_t> mine = {1, 3, 4}, used = {4};   // (2 a draw's; 4 still held by another link)
    const auto out = po::let_go_libs(gone, [&](uint64_t l) { return mine.count(l) != 0; }, [&](uint64_t l) { return used.count(l) != 0; });
    CHECK(out == (std::vector<uint64_t>{1, 3}));   // (each once, 3 met twice)
}

/// A session as the lab's S1: the title and the file select need their pipelines a few at a time, the save's load 800 at once.
/// Programs read ahead in the program order (program_order.h Ahead, its real rules), and each read program variant has its
/// pipelines made: the order's (pipe_order.h) or the notes' as the warm did before (every pipeline any session built with it,
/// 32 at most, links held with the oldest let go beyond 512, libraries none made beyond 1024). Returns the load's pipelines
/// found linked; the libraries no draw has had never pass their bound
struct Sim {
    int found_menus = 0, found_load = 0, load = 0, made = 0, beyond = 0, behind_gone = 0;
    size_t libs_most = 0;
};

static Sim session(bool by_order, uint32_t seed) {
    std::mt19937 rng(seed);
    const int programs = 900;
    // (the order the draws first need their pipelines in: each program's first where the program comes, its others (one to
    // three) a little later, now and then much later (the same material in another pass))
    std::vector<std::pair<int64_t, int>> events;   // (when, program)
    for (int i = 0; i < programs; i++) {
        const int n = 1 + (int)(rng() % 3);
        events.push_back({(int64_t)i * 100, i});
        for (int k = 1; k < n; k++) events.push_back({(int64_t)i * 100 + 1 + (int64_t)(rng() % (rng() % 4 ? 300 : 30000)), i});
    }
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint64_t> order;   // (pipeline keys, as the draws first need them)
    std::vector<int> prog_of;
    for (size_t q = 0; q < events.size(); q++) order.push_back(1000000 + q), prog_of.push_back(events[q].second);
    std::vector<uint64_t> prog_order;   // (program keys in their first need's order)
    {
        std::unordered_set<int> seen;
        for (const int p : prog_of)
            if (seen.insert(p).second) prog_order.push_back((uint64_t)p + 1);
    }
    // (a pipeline's libraries: its program's two with shaders, a vertex input and an output shared by many)
    const auto libs_of = [&](size_t q, uint64_t out[3]) {
        out[0] = (uint64_t)prog_of[q] * 2 + 1, out[1] = (uint64_t)prog_of[q] * 2 + 2, out[2] = 900000 + q % 24;
    };
    // (the notes: every pipeline the order names, and those other sessions built with the same programs, which this one
    // never draws: 0 to 12 a program, in the notes' order (the order they were first built in, by any session))
    std::unordered_map<int, std::vector<int64_t>> notes;   // (program -> pipelines: >= 0 the order's place, < 0 another's)
    for (size_t q = 0; q < order.size(); q++) notes[prog_of[q]].push_back((int64_t)q);
    int64_t other = -1;
    for (auto& [p, list] : notes) {
        for (int k = (int)(rng() % 13); k > 0; k--) list.push_back(other--);
        std::shuffle(list.begin(), list.end(), rng);
        if (list.size() > pipe_warm::kNotesPerVariant) list.resize(pipe_warm::kNotesPerVariant);
    }
    po::Index ix;
    ix.build(order, [&](uint64_t key, uint64_t* v, uint32_t* rec) {
        *v = (uint64_t)prog_of[key - 1000000], *rec = (uint32_t)(key - 1000000);
        return true;
    });
    program_order::Ahead ahead;
    ahead.load(prog_order);
    po::Held<int> held;              // (the order's)
    pipe_warm::Held<int> held_notes;   // (the notes', as before)
    po::Front front;
    std::unordered_set<uint64_t> mine;   // (libraries the warm made that no draw has had)
    std::unordered_set<uint64_t> drawn_libs;
    std::unordered_set<int> warmed;
    Sim sim;
    const auto lib_mine = [&](uint64_t l) { return mine.count(l) != 0; };
    const auto lib_used = [&](uint64_t l) { return held.uses(l); };
    const auto gone_libs = [&](const std::vector<po::Held<int>::One>& gone) {
        for (const uint64_t l : po::let_go_libs(gone, lib_mine, lib_used)) mine.erase(l);
    };
    // (the worker that has read program p: its pipelines made, as vk_draw.cpp warm_variant)
    const auto warm = [&](int p) {
        if (!warmed.insert(p).second) return;
        std::vector<std::pair<int64_t, int64_t>> list;   // (place or other, rec)
        if (by_order) {
            for (const po::Entry& e : ix.take((uint64_t)p)) list.push_back({e.place, e.place});
        } else {
            for (const int64_t q : notes[p]) list.push_back({-1, q});
        }
        for (const auto& [place, q] : list) {
            if (place >= 0 && (front.passed(place) || !held.room(place))) {
                sim.beyond += !front.passed(place);
                continue;
            }
            uint64_t l[3] = {(uint64_t)p * 2 + 1, (uint64_t)p * 2 + 2, 950000 + (uint64_t)(-q) % 24};
            if (q >= 0) libs_of((size_t)q, l);
            bool all = true;
            for (const uint64_t lib : l) {
                if (drawn_libs.count(lib) || mine.count(lib)) continue;
                if (mine.size() >= (by_order ? po::kLibsMax : pipe_warm::kLibsMax)) {
                    all = false;
                    break;
                }
                mine.insert(lib);
            }
            sim.libs_most = std::max(sim.libs_most, mine.size());
            if (!all) continue;
            sim.made++;
            const uint64_t key = q >= 0 ? order[(size_t)q] : (uint64_t)(5000000 - q);
            if (by_order) {
                std::vector<po::Held<int>::One> gone;
                held.put(key, place, 1, l, 3, gone);
                gone_libs(gone);
            } else {
                std::vector<int> gone;
                held_notes.put(key, 1, l, 3, gone);
            }
        }
    };
    const auto read_some = [&](int n) {
        for (int i = 0; i < n; i++) {
            // ((Triforce, wp19) as the workers do: a program the order kept cold is warmed first once the warm has room, and one
            // read beyond the warm's window is read cold (program_order.h Ahead::next_warm, is_cold)
            if (const uint64_t w = ahead.next_warm()) {
                warm((int)w - 1);
                continue;
            }
            if (const uint64_t k = ahead.choose([](uint64_t) { return true; })) {
                const bool cold = ahead.is_cold(k);
                ahead.read_done(k, true);
                if (!cold) warm((int)k - 1);
            }
        }
    };
    // (a draw needs the pipeline at place q for the first time: its link taken when held, or it links its own)
    std::unordered_set<int> programs_met;
    const auto draw = [&](size_t q) -> bool {
        const int p = prog_of[q];
        if (programs_met.insert(p).second) {
            std::vector<uint64_t> let_go;
            ahead.met((uint64_t)p + 1, let_go);   // (a program read by the draw itself is not warmed: no worker read it)
        }
        bool got = false;
        uint64_t l[3];
        libs_of(q, l);
        if (by_order) {
            po::Held<int>::One o;
            got = held.take(order[q], o);
            if (front.met((int64_t)q)) {
                std::vector<po::Held<int>::One> gone;
                held.behind(front.from(), gone);
                sim.behind_gone += (int)gone.size();
                gone_libs(gone);
            }
        } else {
            pipe_warm::Held<int>::One o;
            got = held_notes.take(order[q], o);
        }
        for (const uint64_t lib : l) mine.erase(lib), drawn_libs.insert(lib);
        return got;
    };
    const size_t menus = 350, load_end = std::min<size_t>(1150, order.size());
    for (size_t q = 0; q < menus; q++) {
        read_some(8);
        sim.found_menus += draw(q);
    }
    read_some(4000);   // (the load's seconds: the workers read on, up to the read-ahead's bound)
    for (size_t q = menus; q < load_end; q++) sim.load++, sim.found_load += draw(q);
    return sim;
}

/// The session that went elsewhere: what is held far behind the draws goes, with the libraries only it held
static void elsewhere() {
    po::Held<int> held;
    po::Front front;
    std::unordered_set<uint64_t> mine;
    for (int64_t q = 0; q < 600; q++) {
        const uint64_t l[2] = {(uint64_t)q / 2 + 1, 777};   // (777 shared by all)
        std::vector<po::Held<int>::One> gone;
        held.put(100 + q, q, 1, l, 2, gone);
        mine.insert(l[0]), mine.insert(777);
    }
    for (int64_t q = 0; q < 16; q++) front.met(q);
    std::vector<po::Held<int>::One> gone;
    held.behind(front.from(), gone);
    CHECK(gone.empty() && held.size() == 600);   // (the draws at the start: nothing behind)
    for (int64_t q = 3000; q < 3000 + (int64_t)po::kRecent; q++) front.met(q);   // (that session went to another area)
    held.behind(front.from(), gone);
    CHECK(gone.size() == 600 && held.size() == 0);
    const auto out = po::let_go_libs(gone, [&](uint64_t l) { return mine.count(l) != 0; }, [&](uint64_t l) { return held.uses(l); });
    CHECK(out.size() == 301);   // (300 programs' and the shared one, each once)
}

int main() {
    CHECK(po::on(nullptr) && po::on("1") && !po::on("0") && po::on("00"));
    CHECK(po::kMagic != program_order::kMagic && po::kKeysMax == program_order::kKeysMax);
    CHECK(po::kLinksMax >= 2 * pipe_warm::kLinksMax && po::kLibsMax >= 2 * po::kLinksMax && po::kPerVariant > 0);
    file();
    indexed();
    front();
    held();
    libs();
    elsewhere();
    for (const uint32_t seed : {37u, 1u, 2u}) {
        const Sim o = session(true, seed), n = session(false, seed);
        std::printf("synthetic S1, seed %u: the load's %d pipelines found linked: in the order %d (made %d, %d not made beyond the "
                    "bound, %d let go behind; libraries at most %zu), from the notes as before %d (made %d; libraries at most %zu)\n",
                    seed, o.load, o.found_load, o.made, o.beyond, o.behind_gone, o.libs_most, n.found_load, n.made, n.libs_most);
        CHECK(o.load == n.load && o.load >= 790);
        CHECK(o.found_load * 100 >= o.load * 99);   // (the load finds its pipelines linked)
        CHECK(o.found_load > 2 * n.found_load);     // (and the notes' warm did not)
        CHECK(o.libs_most <= po::kLibsMax && n.libs_most <= pipe_warm::kLibsMax);
        CHECK(o.found_menus * 10 >= 9 * 350);
    }
    std::puts("PASS: the pipelines made ahead in the sessions' order: file, index, where the draws are, the links held, a session");
    return 0;
}
