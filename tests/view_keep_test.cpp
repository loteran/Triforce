// tests/test_view_keep.py: gpu/vk/view_keep.h with gpu/vk/texture_gone.h, compiled and run against a model of a recording
// thread's texture looks (no game, no GPU). The rule's promise: a look the thread takes again gives what resolving it anew would
// give, and a look of a texture whose views did not go is not resolved anew for another texture's end.
#include "texture_gone.h"
#include "view_keep.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>

static int fails;
#define CHECK(c, ...)                                                                                                   \
    do {                                                                                                                \
        if (!(c)) {                                                                                                     \
            fprintf(stderr, "FAIL: " __VA_ARGS__);                                                                      \
            fprintf(stderr, "\n");                                                                                      \
            fails++;                                                                                                    \
        }                                                                                                               \
    } while (0)

// The renderer as the thread sees it: textures by creation number with what their view is now (0: gone), the slots and the
// texture each names, the destroy generation, the record of textures whose views went (vk_texture.cpp views_gone)
struct World {
    texture_gone::Record rec;
    uint64_t dgen = 1;
    uint32_t next_id = 1;
    std::map<uint32_t, uint64_t> view_of;   // (texture -> its view now; 0: gone)
    std::map<int, uint32_t> slot_tex;       // (slot -> texture it resolves to now)
    std::map<int, bool> slot_copy;          // (a slot sampled as a flat texture's 3D copy: copied again at each look)
    std::map<int, bool> slot_none;          // (a slot that resolves to no view: one may be made since)
    uint64_t made = 0;                      // (what a look of no view resolves to now: changes at any texture's end)
    uint32_t make() {
        const uint32_t id = next_id++;
        view_of[id] = 1000ull * id;
        return id;
    }
    // A texture's views go: destroyed (its slots then resolve to a new texture), or made again in another format, or moved.
    // Noted before the generation moves, as vk_texture.cpp does
    void views_go(uint32_t id, bool destroyed) {
        if (destroyed) {
            view_of[id] = 0;
            for (auto& [s, t] : slot_tex)
                if (t == id) t = make();
        } else {
            view_of[id] += 7;
        }
        rec.went(id);
        made++;
        dgen++;
    }
    void bump_alone() { made++, dgen++; }   // (the generation moved with no texture's views going: RX_VK_DUMMY_GEN=0's frames)
    // What resolving slot s now gives: the view, the texture, whether that look is stable (view_keep.h)
    uint64_t truth(int s) const {
        if (slot_none.count(s)) return made;
        const uint32_t t = slot_tex.at(s);
        return view_of.at(t) + (slot_copy.count(s) ? made : 0);
    }
};

struct Look {
    uint64_t gen = 0;
    uint32_t id = 0;
    bool stable = false;
    uint64_t view = 0;
};

struct Thread {
    view_keep::Pass p;
    std::vector<uint32_t> gone;
    std::map<int, Look> looks;
    uint64_t resolves = 0;
    bool on = true;
    uint64_t look(World& w, int s) {
        const uint64_t gen = w.dgen;   // (read first, the record inside the pass: vk_draw.cpp texture_view_of)
        if (on)
            view_keep::pass(p, gen, [&](auto&& fn) { for (auto& kv : looks) fn(kv.second.gen, kv.second.id, kv.second.stable); },
                            [&](uint64_t since, std::vector<uint32_t>& out, uint64_t* now) { return w.rec.went_since(since, out, now); },
                            gone);
        auto it = looks.find(s);
        if (it != looks.end() && it->second.gen == gen) return it->second.view;
        resolves++;
        Look l;
        l.gen = gen;
        l.view = w.truth(s);
        l.id = w.slot_none.count(s) ? 0 : w.slot_tex.at(s);
        l.stable = !w.slot_none.count(s) && !w.slot_copy.count(s);
        looks[s] = l;
        return l.view;
    }
};

static void stands_cases() {
    const std::vector<uint32_t> gone = {3, 9, 12};
    CHECK(view_keep::stands(5, 4, true, 5, gone, true), "a stable look of a texture not gone, at the last pass's generation, stands");
    CHECK(!view_keep::stands(5, 9, true, 5, gone, true), "a look of a texture whose views went stands");
    CHECK(!view_keep::stands(5, 4, false, 5, gone, true), "a look of no view, or of a 3D copy, stands");
    CHECK(!view_keep::stands(5, 0, true, 5, gone, true), "a look of no texture stands");
    CHECK(!view_keep::stands(4, 4, true, 5, gone, true), "a look already stale before the last pass stands");
    CHECK(!view_keep::stands(5, 4, true, 5, gone, false), "a look stands when the textures gone could not all be told");
    CHECK(view_keep::wanted(nullptr) && view_keep::wanted("1") && !view_keep::wanted("0"), "RX_VK_VIEW_KEEP: on unless 0");
}

static void record_cases() {
    texture_gone::Record r;
    std::vector<uint32_t> out;
    uint64_t now = 99;
    CHECK(r.went_since(0, out, &now) && out.empty() && now == 0, "an empty record tells nothing went");
    for (uint32_t i = 1; i <= 5; i++) r.went(i);
    CHECK(r.went_since(2, out, &now) && out == std::vector<uint32_t>({3, 4, 5}) && now == 5, "the record tells those from 2 on");
    out.clear();
    CHECK(!r.went_since(7, out, &now) && out.empty() && now == 5, "a count past the record's is told");
    for (uint32_t i = 0; i < texture_gone::Record::kKept; i++) r.went(100 + i);
    CHECK(!r.went_since(2, out, &now) && out.empty(), "more gone than kept is told");
    CHECK(r.went_since(now - 3, out, &now) && out.size() == 3, "the last few are told");
}

static void directed() {
    World w;
    for (int s = 0; s < 4; s++) w.slot_tex[s] = w.make();
    Thread t;
    for (int s = 0; s < 4; s++) t.look(w, s);
    const uint64_t r0 = t.resolves;
    // (another texture goes: the looks stand)
    const uint32_t other = w.make();
    w.views_go(other, true);
    for (int s = 0; s < 4; s++) CHECK(t.look(w, s) == w.truth(s), "slot %d after another texture's end", s);
    CHECK(t.resolves == r0, "looks of textures not gone were resolved anew for another texture's end (%llu)",
          (unsigned long long)(t.resolves - r0));
    // (slot 1's texture goes: it alone is resolved anew)
    w.views_go(w.slot_tex[1], true);
    for (int s = 0; s < 4; s++) CHECK(t.look(w, s) == w.truth(s), "slot %d after slot 1's texture went", s);
    CHECK(t.resolves == r0 + 1, "slot 1 alone resolved anew (%llu)", (unsigned long long)(t.resolves - r0));
    // (slot 2's texture made again in another format: resolved anew, the same texture's new view)
    w.views_go(w.slot_tex[2], false);
    CHECK(t.look(w, 2) == w.truth(2), "slot 2 after its texture's views went");
    // (a look left stale by one pass, then another texture goes: still stale. Slot 3's texture's views go, the thread passes
    // while looking at slot 0, then another texture goes and the thread passes again: slot 3 resolved anew all the same)
    w.views_go(w.slot_tex[3], false);
    t.look(w, 0);
    w.views_go(w.make(), true);
    CHECK(t.look(w, 3) == w.truth(3), "a look stale since an earlier pass was taken again at a later one");
    // (two gone in one pass, in an order the search must sort)
    for (int s = 0; s < 4; s++) t.look(w, s);
    const uint32_t a = w.slot_tex[2], b = w.slot_tex[0];
    w.views_go(a > b ? a : b, false);
    w.views_go(a > b ? b : a, false);
    for (int s = 0; s < 4; s++) CHECK(t.look(w, s) == w.truth(s), "slot %d after two went in one pass", s);
    // (more gone since the last pass than the record keeps: every look resolved anew)
    for (int s = 0; s < 4; s++) t.look(w, s);
    const uint64_t r1 = t.resolves;
    w.views_go(w.slot_tex[1], false);
    for (uint64_t i = 0; i < texture_gone::Record::kKept; i++) w.views_go(w.make(), true);
    for (int s = 0; s < 4; s++) CHECK(t.look(w, s) == w.truth(s), "slot %d after more went than are kept", s);
    CHECK(t.resolves == r1 + 4, "after more went than are kept every look was resolved anew (%llu)", (unsigned long long)(t.resolves - r1));
    // (a look of no view, and a flat texture's 3D copy: resolved anew at any end)
    w.slot_none[10] = true;
    w.slot_tex[11] = w.make(), w.slot_copy[11] = true;
    t.look(w, 10), t.look(w, 11);
    w.views_go(w.make(), true);
    CHECK(t.look(w, 10) == w.truth(10), "a look of no view kept through a texture's end");
    CHECK(t.look(w, 11) == w.truth(11), "a 3D copy's look kept through a texture's end");
    w.bump_alone();
    CHECK(t.look(w, 10) == w.truth(10), "a look of no view kept through the generation's move alone");
    // (the switch off: every look resolved anew, as before)
    Thread off;
    off.on = false;
    for (int s = 0; s < 4; s++) off.look(w, s);
    const uint64_t r2 = off.resolves;
    w.views_go(w.make(), true);
    for (int s = 0; s < 4; s++) CHECK(off.look(w, s) == w.truth(s), "slot %d with the rule off", s);
    CHECK(off.resolves == r2 + 4, "with the rule off a look stood through a texture's end");
}

static void random_runs() {
    for (unsigned seed = 1; seed <= 200; seed++) {
        std::mt19937 rng(seed);
        World w;
        const int slots = 24;
        for (int s = 0; s < slots; s++) {
            const unsigned k = rng() % 10;
            if (k == 0) w.slot_none[s] = true;
            else w.slot_tex[s] = w.make();
            if (k == 1) w.slot_copy[s] = true;
        }
        Thread th[3];
        for (int step = 0; step < 4000 && !fails; step++) {
            const unsigned e = rng() % 100;
            if (e < 70) {
                Thread& t = th[rng() % 3];
                const int s = (int)(rng() % slots);
                const uint64_t got = t.look(w, s), want = w.truth(s);
                CHECK(got == want, "seed %u step %d: slot %d gave %llu, resolving anew gives %llu", seed, step, s,
                      (unsigned long long)got, (unsigned long long)want);
            } else if (e < 85) {
                // (a texture a slot names, or one nobody names, goes or has its views made anew)
                const bool named = rng() % 2;
                uint32_t id = 0;
                if (named) {
                    auto it = w.slot_tex.begin();
                    std::advance(it, rng() % w.slot_tex.size());
                    id = it->second;
                } else {
                    id = w.make();
                }
                w.views_go(id, rng() % 3 != 0);
            } else if (e < 88) {
                w.bump_alone();
            } else if (e < 89) {
                // (a burst past what the record keeps)
                const unsigned n = (unsigned)texture_gone::Record::kKept + rng() % 64;
                for (unsigned i = 0; i < n; i++) w.views_go(rng() % 4 ? w.make() : w.slot_tex.begin()->second, true);
            } else {
                // (a burst of a few)
                const unsigned n = 2 + rng() % 6;
                for (unsigned i = 0; i < n; i++) {
                    auto it = w.slot_tex.begin();
                    std::advance(it, rng() % w.slot_tex.size());
                    w.views_go(rng() % 2 ? it->second : w.make(), rng() % 2);
                }
            }
        }
    }
}

int main() {
    stands_cases();
    record_cases();
    directed();
    random_runs();
    if (fails) {
        fprintf(stderr, "FAIL: %d checks\n", fails);
        return 1;
    }
    printf("view_keep: every look taken again gives what resolving it anew gives; others' ends leave looks standing\n");
    return 0;
}
