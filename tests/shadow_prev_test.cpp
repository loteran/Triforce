// tests/test_shadow_prev.py's fixture: take_capture's texture shadow loop (vk_memory.cpp) modelled over the steps of
// gpu/vk/shadow_prev.h, which the renderer calls (the test checks the calls' places in vk_memory.cpp). No game, no GPU.
//
// A take is a list of texture pages a capture copied, each with its bytes (a page may come twice, with other bytes). The
// model does what take_capture does: tex_same's look (the copy compared with the capture's, shadow_prev::look), then the
// shadow's loop (act; SAME and BACK left, CHANGED keep_old, every other page copied into its slot as before, its hash let
// go unless tex_same found it the same; a page copied twice loses its hash; a Prev not given back aged at its page's take and
// at the sweep every kSweep takes), with the copies made after the loop. Between
// the two (shadow_mu let go) and between takes, other threads: a check hashing a copy (memory_hash_as_submitted keeps
// content_hash of it), a GPU write dropping copies (memory_drop_shadow), and, for the act's own guard, a slot given other
// buffers. What must hold after every take:
//   - every page the take copied holds the bytes of its last copy in the take (what the old loop leaves);
//   - a hash is content_hash of the bytes it is kept with, or 0 (the copy's and the Prev's);
//   - shadow_prev::alive() counts the Prevs with a buffer, never more than the cap, and none outlives kStale takes without
//     being given back by more than the sweep's period;
//   - with RX_VK_SHADOW_KEEP=0 and RX_VK_SHADOW_PREV=0 (the env, as the renderer reads it) the copies, the bytes and the hashes
//     are the old loop's, step for step, and nothing is kept.
// And what is gained: a page the game rewrites A, B, A, B is copied twice, then given back its copy with its hash every
// change; one written A, A, B, B is not copied when it comes again the same; one that never comes back lets its copy go;
// one written A, B, A, C, A, D gets A back every time; one changed once and then taken the same (or not taken) lets its copy
// go after kStale takes (at the sweep), so the cap does not stay full of copies that never come back.
#include "shadow_prev.h"
#include "upload_same.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(c, ...)                                         \
    do {                                                      \
        if (!(c)) {                                           \
            if (failures++ < 20) {                            \
                std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                std::printf(__VA_ARGS__);                     \
                std::printf("\n");                            \
            }                                                 \
        }                                                     \
    } while (0)

using Page = std::array<uint8_t, 4096>;

struct Slot {   // (vk_memory.cpp ShadowPage's members, by the same names)
    std::unique_ptr<uint8_t[]> b;
    uint64_t seq = 0;
    shadow_prev::Vers prev;
    uint64_t hash = 0;
};
const shadow_prev::Vers kNoVers;   // (a page with no slot: what act compares)

/// A page's bytes for value v: 0 all zero; v >= 0x100 the bytes of v & 0xff but the last (which a compare of less than the
/// whole page does not see)
Page bytes_of(uint32_t page, int v) {
    Page b{};
    if ((v & 0xff) == 0) return b;
    uint64_t x = page * 0x9E3779B97F4A7C15ull + (uint64_t)(v & 0xff) * 0xBF58476D1CE4E5B9ull + 1;
    for (auto& c : b) x ^= x << 13, x ^= x >> 7, x ^= x << 17, c = (uint8_t)x;
    if (v >= 0x100) b[4095] ^= 0x5a;
    return b;
}
uint64_t chash(const uint8_t* b) { return upload_same::content_hash(b, 4096); }

struct Copy {
    uint32_t page;
    int v;
};
struct Opts {
    bool keep = true, prev = true;
    int vers = shadow_prev::kVersDefault;   // (RX_VK_SHADOW_VERS: the copies a page keeps)
    int64_t cap = shadow_prev::kCapPages;
    std::mt19937* rng = nullptr;   // (other threads between the look and the loop)
    int between = 0;               // (out of 100: how often)
};
struct Stats {
    int64_t copies = 0, kept = 0, back = 0, old = 0, looked = 0, stale = 0;
};

struct Model {
    std::map<uint32_t, Slot> shadow;
    uint64_t seq = 0;

    void others(std::mt19937& rng, int pct, bool replace) {   // (a check's hash, a GPU write, a slot given other buffers)
        for (auto it = shadow.begin(); it != shadow.end();) {
            const int r = (int)(rng() % 100);
            if (r < pct) {
                it->second.hash = chash(it->second.b.get());
            } else if (r < pct + 3) {
                it = shadow.erase(it);
                continue;
            } else if (replace && r < pct + 5) {
                Slot s;
                s.b.reset(new uint8_t[4096]);
                const Page o = bytes_of(it->first, (int)(rng() % 5));
                std::memcpy(s.b.get(), o.data(), 4096);
                it->second = std::move(s);
            } else if (replace && r < pct + 7) {   // (its versions let go alone: act's guard of the buffer it compared)
                shadow_prev::drop_all(it->second.prev);
            }
            ++it;
        }
    }

    /// One take of the copies cap (as take_capture: the look, then the loop, then the copies)
    void take(const std::vector<Copy>& cap, const Opts& o, Stats& st) {
        std::vector<Page> capb(cap.size());
        for (size_t i = 0; i < cap.size(); i++) capb[i] = bytes_of(cap[i].page, cap[i].v);
        // (tex_same: the looks sized at the first page with a copy, any counting them)
        std::vector<shadow_prev::Look> looks;
        std::vector<uint32_t> same_set;
        const bool on = o.keep || o.prev;
        size_t any = 0;
        for (size_t i = 0; i < cap.size(); i++) {
            auto s = shadow.find(cap[i].page);
            if (s == shadow.end() || !s->second.b) continue;
            if (on && !any++) looks.assign(cap.size(), shadow_prev::Look{});
            if (std::memcmp(s->second.b.get(), capb[i].data(), 4096) != 0) {
                if (on) looks[i] = shadow_prev::look(false, s->second.b.get(), s->second.prev, capb[i].data(), o.prev, o.vers);
                continue;
            }
            same_set.push_back(cap[i].page);
            if (on) looks[i] = shadow_prev::look(true, s->second.b.get(), s->second.prev, nullptr, o.prev, o.vers);
        }
        if (o.rng && o.between) others(*o.rng, o.between, true);
        // (the shadow's loop, after the sweep that ages every kept copy every kSweep takes)
        seq++;
        if (seq % shadow_prev::kSweep == 0 && shadow_prev::alive().load() > 0)
            for (auto& [p, s] : shadow) st.stale += shadow_prev::age(s.prev, seq);
        std::sort(same_set.begin(), same_set.end());
        std::vector<uint32_t> pages, hashed_kept;
        if (any) {
            for (const auto& c : cap) pages.push_back(c.page);
            std::sort(pages.begin(), pages.end());
        }
        std::vector<std::pair<uint8_t*, const uint8_t*>> pairs;
        for (size_t i = 0; i < cap.size(); i++) {
            const uint32_t p = cap[i].page;
            uint8_t act = shadow_prev::NONE;
            if (any && looks[i].v != shadow_prev::NONE) {
                st.looked++;
                auto s = shadow.find(p);
                Slot* const sp = s == shadow.end() ? nullptr : &s->second;
                act = shadow_prev::act(looks[i], o.keep, o.prev, shadow_prev::twice(pages, p), sp ? sp->b.get() : nullptr,
                                       sp ? sp->prev : kNoVers);
                if (act == shadow_prev::SAME || act == shadow_prev::BACK) {
                    if (act == shadow_prev::BACK) shadow_prev::back(sp->b, sp->hash, sp->prev, looks[i].depth, seq), st.back++;
                    else st.kept++, st.stale += shadow_prev::age(sp->prev, seq);
                    sp->seq = seq;
                    continue;
                }
            }
            Slot& slot = shadow[p];
            if (!slot.b) slot.b.reset(new uint8_t[4096]);
            slot.seq = seq;
            if (act == shadow_prev::CHANGED && shadow_prev::keep_old(slot.b, slot.hash, slot.prev, o.cap, seq, o.vers)) st.old++;
            else if (any) st.stale += shadow_prev::age(slot.prev, seq);
            if (slot.hash) {
                if (std::binary_search(same_set.begin(), same_set.end(), p)) hashed_kept.push_back(p);
                else slot.hash = 0;
            }
            pairs.push_back({slot.b.get(), capb[i].data()});
        }
        for (const auto& [d, s] : pairs) std::memcpy(d, s, 4096), st.copies++;
        std::sort(hashed_kept.begin(), hashed_kept.end());
        for (size_t k = 1; k < hashed_kept.size(); k++)
            if (hashed_kept[k] == hashed_kept[k - 1]) shadow[hashed_kept[k]].hash = 0;
        // (what must hold)
        std::map<uint32_t, const Page*> last;
        for (size_t i = 0; i < cap.size(); i++) last[cap[i].page] = &capb[i];
        for (const auto& [p, b] : last) {
            auto s = shadow.find(p);
            CHECK(s != shadow.end() && s->second.b && !std::memcmp(s->second.b.get(), b->data(), 4096),
                  "page %u does not hold its last copy's bytes after the take", p);
        }
        holds(o.keep || o.prev);
        CHECK(shadow_prev::alive().load() <= o.cap, "%lld copies kept past the cap %lld", (long long)shadow_prev::alive().load(),
              (long long)o.cap);
    }
    void holds(bool counted) {   // (counted: the only model with copies kept; another one keeps none)
        int64_t with = 0;
        for (const auto& [p, s] : shadow) {
            CHECK(!s.hash || s.hash == chash(s.b.get()), "page %u: its hash is not of its bytes", p);
            const auto chash2 = [](const uint8_t* b, uint64_t) { return chash(b); };   // (holds_all's shape)
            CHECK(shadow_prev::holds_all(s.prev, chash2), "page %u: a kept version's hash is not of its bytes", p);
            for (int k = 0; k < (int)s.prev.n; k++) {   // (none kept past kStale takes without coming back, but until the sweep)
                CHECK(s.prev.p[k].b && seq - s.prev.p[k].at <= shadow_prev::kStale + shadow_prev::kSweep, "page %u: a version kept %llu "
                      "takes", p, (unsigned long long)(seq - s.prev.p[k].at));
                with++;
            }
        }
        if (counted) CHECK(with == shadow_prev::alive().load(), "alive() is %lld, %lld kept", (long long)shadow_prev::alive().load(), (long long)with);
        else CHECK(with == 0, "%lld copies kept with both switches off", (long long)with);
    }
};

Opts env_opts() {
    Opts o;
    o.keep = shadow_prev::env_on("RX_VK_SHADOW_KEEP");
    o.prev = shadow_prev::env_on("RX_VK_SHADOW_PREV");
    o.vers = shadow_prev::env_vers();
    return o;
}

/// The loop's gains, page by page
void gains() {
    const Opts o = env_opts();
    if (!o.keep || !o.prev) { std::printf("gains: skipped with a switch off\n"); return; }
    {   // (A, B, A, B, ...: two copies, then each change back to the other with its hash)
        Model m;
        Stats st;
        for (int k = 0; k < 20; k++) {
            m.take({{7, k % 2 ? 2 : 1}}, o, st);
            if (k < 2) m.shadow[7].hash = chash(m.shadow[7].b.get());   // (a check hashes A, then B)
            if (k >= 2) CHECK(m.shadow[7].hash == chash(m.shadow[7].b.get()), "ABAB take %d: the hash was not given back", k);
        }
        CHECK(st.copies == 2 && st.back == 18 && st.old == 1, "ABAB: %lld copies, %lld back, %lld kept (2, 18, 1)",
              (long long)st.copies, (long long)st.back, (long long)st.old);
        std::printf("ABAB: 20 takes, %lld copies, %lld given back\n", (long long)st.copies, (long long)st.back);
    }
    {   // (A, B, A, B past two sweeps: given back at every take, never aged out: back() makes its Prev's age start again)
        Model m;
        Stats st;
        const int n = 2 * (int)shadow_prev::kSweep + 10;
        for (int k = 0; k < n; k++) m.take({{8, k % 2 ? 2 : 1}}, o, st);
        CHECK(st.copies == 2 && st.old == 1 && st.back == n - 2 && st.stale == 0, "ABAB long: %lld copies, %lld kept, %lld back, "
              "%lld aged out (2, 1, %d, 0)", (long long)st.copies, (long long)st.old, (long long)st.back, (long long)st.stale, n - 2);
    }
    {   // (A, A, B, B, ...: the same pages left, the changes given back)
        Model m;
        Stats st;
        for (int k = 0; k < 20; k++) m.take({{3, (k / 2) % 2 ? 4 : 0}}, o, st);
        CHECK(st.copies == 2 && st.kept == 10 && st.back == 8, "AABB: %lld copies, %lld left, %lld back (2, 10, 8)",
              (long long)st.copies, (long long)st.kept, (long long)st.back);
    }
    {   // (A, A', A: bytes that differ in the last byte only are other bytes)
        Model m;
        Stats st;
        m.take({{5, 1}}, o, st);
        m.take({{5, 0x101}}, o, st);
        m.take({{5, 0x101}}, o, st);
        m.take({{5, 1}}, o, st);
        CHECK(st.copies == 2 && st.kept == 1 && st.back == 1, "near bytes: %lld copies, %lld left, %lld back (2, 1, 1)",
              (long long)st.copies, (long long)st.kept, (long long)st.back);
    }
    {   // (never back: the kept copy goes after kMisses changes, none kept for kRetry, then one again)
        Model m;
        Stats st;
        int v = 1;
        m.take({{9, v}}, o, st);
        int first_drop = -1, again = -1;
        for (int k = 0; k < 80; k++) {
            m.take({{9, (++v % 250) + 1}}, o, st);
            if (first_drop < 0 && !shadow_prev::newest(m.shadow[9].prev) && k > 0) first_drop = k;
            if (first_drop >= 0 && again < 0 && shadow_prev::newest(m.shadow[9].prev)) again = k;
        }
        CHECK(first_drop == shadow_prev::kMisses, "the copy that never came back went after %d changes (%d)", first_drop,
              (int)shadow_prev::kMisses);
        CHECK(again == first_drop + shadow_prev::kRetry + 1, "a copy kept again after %d changes (%d)", again,
              first_drop + shadow_prev::kRetry + 1);
        CHECK(st.back == 0, "streaming bytes given back");
    }
    {   // (the cap: three kept at most, the others copied as before)
        Model m;
        Stats st;
        Opts c = o;
        c.cap = 3;
        for (int k = 0; k < 10; k++) {
            std::vector<Copy> t;
            for (uint32_t p = 0; p < 8; p++) t.push_back({p, k % 2 ? 2 : 1});
            m.take(t, c, st);
        }
        CHECK(shadow_prev::alive().load() <= 3 && st.back == 3 * 8, "the cap: %lld kept, %lld back (3, 24)",
              (long long)shadow_prev::alive().load(), (long long)st.back);
    }
    {   // (A, B, A, C, A, D, ...: A given back at every take after the first, its Prev never let go: back() starts its misses
        // again, else the changes in between would count up to kMisses and let it go)
        Model m;
        Stats st;
        const int n = 6 * shadow_prev::kMisses;
        int a_takes = 0, a_back = 0, dropped = 0;
        for (int k = 0; k < n; k++) {
            const int64_t b0 = st.back;
            m.take({{11, k % 2 ? 2 + k : 1}}, o, st);
            if (k % 2 == 0 && k > 0) a_takes++, a_back += st.back == b0 + 1;
            if (k > 0 && !shadow_prev::newest(m.shadow[11].prev)) dropped++;
        }
        CHECK(a_takes == n / 2 - 1 && a_back == a_takes && st.back == a_takes, "ABAC: %d of %d A takes given back (%lld in all)",
              a_back, a_takes, (long long)st.back);
        CHECK(dropped == 0 && st.copies == n / 2 + 1, "ABAC: its Prev let go %d times, %lld copies (%d)", dropped, (long long)st.copies,
              n / 2 + 1);
    }
    {   // (changed once, then the same: its Prev goes after kStale takes, at a take of the page; the cap not held by such copies)
        Model m;
        Stats st;
        Opts c = o;
        c.cap = 3;
        auto all = [](uint32_t p0, int v) {
            std::vector<Copy> t;
            for (uint32_t p = p0; p < p0 + 8; p++) t.push_back({p, v});
            return t;
        };
        m.take(all(0, 1), c, st);
        m.take(all(0, 2), c, st);   // (changed: three kept, the cap)
        CHECK(shadow_prev::alive().load() == 3 && st.old == 3, "changed once: %lld kept (3)", (long long)shadow_prev::alive().load());
        for (uint64_t k = 0; k < shadow_prev::kStale; k++) m.take(all(0, 2), c, st);
        CHECK(shadow_prev::alive().load() == 3 && st.stale == 0, "after %llu takes the same: %lld kept (3)",
              (unsigned long long)shadow_prev::kStale, (long long)shadow_prev::alive().load());
        m.take(all(0, 2), c, st);
        CHECK(shadow_prev::alive().load() == 0 && st.stale == 3, "after %llu takes the same: %lld kept, %lld let go (0, 3)",
              (unsigned long long)shadow_prev::kStale + 1, (long long)shadow_prev::alive().load(), (long long)st.stale);
        CHECK(st.copies == 16 && st.kept == 8 * ((int64_t)shadow_prev::kStale + 1), "changed once: %lld copies, %lld left",
              (long long)st.copies, (long long)st.kept);
        m.take(all(8, 1), c, st);   // (other pages that change: copies kept for them again)
        const int64_t old0 = st.old;
        m.take(all(8, 2), c, st);
        CHECK(shadow_prev::alive().load() == 3 && st.old - old0 == 3, "other pages after: %lld kept (3)",
              (long long)shadow_prev::alive().load());
        m.take(all(8, 1), c, st);
        CHECK(st.back == 3, "other pages after: %lld given back (3)", (long long)st.back);
    }
    {   // (changed once, then not taken at all: its Prev goes at the sweep, every kSweep takes)
        Model m;
        Stats st;
        m.take({{20, 1}}, o, st);
        m.take({{20, 2}}, o, st);
        CHECK(shadow_prev::alive().load() == 1, "changed once: %lld kept (1)", (long long)shadow_prev::alive().load());
        int went = -1;
        for (int k = 0; k < (int)shadow_prev::kSweep + 8; k++) {
            m.take({{21, 1 + k % 2}}, o, st);   // (another page only)
            if (went < 0 && !shadow_prev::newest(m.shadow[20].prev)) went = (int)m.seq;
        }
        CHECK(went == (int)shadow_prev::kSweep, "a page not taken again let its copy go at take %d (%d)", went, (int)shadow_prev::kSweep);
        CHECK(m.shadow[20].b && m.shadow[20].hash == 0 && !std::memcmp(m.shadow[20].b.get(), bytes_of(20, 2).data(), 4096),
              "the page's own copy changed by the sweep");
    }
    {   // (A, B, C, A: with one version kept A is copied again; with two, A is the one before B (depth 1) and given back)
        for (int vers = 1; vers <= 2; vers++) {
            Model m;
            Stats st;
            Opts c = o;
            c.vers = vers;
            for (const int v : {1, 2, 3, 1}) m.take({{12, v}}, c, st);
            CHECK(st.copies == (vers == 1 ? 4 : 3) && st.back == (vers == 1 ? 0 : 1), "ABC A with %d kept: %lld copies, %lld back "
                  "(%d, %d)", vers, (long long)st.copies, (long long)st.back, vers == 1 ? 4 : 3, vers == 1 ? 0 : 1);
        }
    }
    {   // (A, B, C, D, A: given back only with three kept, the version two before)
        for (int vers = 1; vers <= 3; vers++) {
            Model m;
            Stats st;
            Opts c = o;
            c.vers = vers;
            for (const int v : {1, 2, 3, 4, 1}) m.take({{13, v}}, c, st);
            CHECK(st.copies == (vers == 3 ? 4 : 5) && st.back == (vers == 3 ? 1 : 0), "ABC D A with %d kept: %lld copies, %lld back "
                  "(%d, %d)", vers, (long long)st.copies, (long long)st.back, vers == 3 ? 4 : 5, vers == 3 ? 1 : 0);
        }
    }
    {   // (A, B, C, B, A at 3 kept: each version taken back once it is in the ring, its own copy made once)
        Model m;
        Stats st;
        Opts c = o;
        c.vers = 3;
        for (const int v : {1, 2, 3, 2, 1}) m.take({{14, v}}, c, st);
        CHECK(st.copies == 3 && st.back == 2, "ABC B A at 3: %lld copies, %lld back (3, 2)", (long long)st.copies, (long long)st.back);
        CHECK(m.shadow[14].b && !std::memcmp(m.shadow[14].b.get(), bytes_of(14, 1).data(), 4096), "ABC B A at 3: not A's bytes");
    }
    {   // (A, B, C, D, E, A at 4 kept: the version four back, depth 3, given back; the ring's oldest rotation)
        Model m;
        Stats st;
        Opts c = o;
        c.vers = 4;
        for (const int v : {1, 2, 3, 4, 5, 1}) m.take({{15, v}}, c, st);
        CHECK(st.copies == 5 && st.back == 1, "ABCDE A at 4: %lld copies, %lld back (5, 1)", (long long)st.copies, (long long)st.back);
        CHECK(m.shadow[15].b && !std::memcmp(m.shadow[15].b.get(), bytes_of(15, 1).data(), 4096), "ABCDE A at 4: not A's bytes");
    }
    {   // (a page the capture took twice: copied as before, the second copy's bytes)
        Model m;
        Stats st;
        m.take({{4, 1}}, o, st);
        m.take({{4, 2}}, o, st);
        const int64_t c0 = st.copies;
        m.take({{4, 2}, {4, 1}}, o, st);
        CHECK(st.copies - c0 == 2, "a page taken twice: %lld copies (2)", (long long)(st.copies - c0));
    }
    CHECK(shadow_prev::alive().load() == 0, "copies kept after every model went: %lld", (long long)shadow_prev::alive().load());
}

/// Random takes: what must hold; and with the switches as the env has them, against the old loop step by step when both are 0
void random_takes(bool against_old) {
    const Opts o = env_opts();
    int64_t total_copies = 0, total_left = 0, old_copies = 0;
    for (int round = 0; round < 24; round++) {
        std::mt19937 rng(1234 + round), rng_old(1234 + round);
        Model m, ref;
        Opts on = o, off;
        off.keep = off.prev = false;
        on.cap = round % 3 == 0 ? 4 : shadow_prev::kCapPages;
        on.rng = &rng, on.between = round % 2 ? 6 : 0;
        off.rng = &rng_old, off.between = on.between;
        Stats st, st_old;
        std::mt19937 gen(99 + round);
        for (int t = 0; t < 400; t++) {
            std::vector<Copy> cap;
            const int n = 1 + (int)(gen() % 10);
            for (int k = 0; k < n; k++) {
                const uint32_t p = (uint32_t)(gen() % 12);
                int v = (int)(gen() % 4);   // (values 0 to 3, now and then the near bytes of one)
                if (gen() % 16 == 0) v |= 0x100;
                if (gen() % 3) v = (int)((t / 2 + p) % 2) + 1;   // (mostly A, B, A, B: the loop's pages)
                cap.push_back({p, v});
                if (gen() % 25 == 0) cap.push_back({p, (int)(gen() % 4)});   // (the capture's twice)
            }
            m.take(cap, on, st);
            if (against_old) {
                ref.take(cap, off, st_old);
                for (const auto& [p, s] : ref.shadow) {
                    auto it = m.shadow.find(p);
                    CHECK(it != m.shadow.end() && !std::memcmp(it->second.b.get(), s.b.get(), 4096), "round %d take %d: page %u not the old loop's bytes",
                          round, t, p);
                    // (PREV=0: the hashes the old loop's, a page left with its bytes keeping its hash as the copy over it did)
                    if (!o.prev && it != m.shadow.end())
                        CHECK(it->second.hash == s.hash, "round %d take %d: page %u not the old loop's hash", round, t, p);
                }
                CHECK(m.shadow.size() == ref.shadow.size(), "round %d take %d: %zu pages, the old loop %zu", round, t, m.shadow.size(),
                      ref.shadow.size());
            }
            if (t % 7 == 0) {   // (between takes: checks hash copies, GPU writes drop some)
                std::mt19937& r = rng;
                m.others(r, 30, false);
                if (against_old) ref.others(rng_old, 30, false);
            }
        }
        total_copies += st.copies, total_left += st.kept + st.back, old_copies += st_old.copies;
        if (!o.keep && !o.prev) CHECK(st.copies == st_old.copies && shadow_prev::alive().load() == 0, "off: %lld copies, the old loop %lld",
                                      (long long)st.copies, (long long)st_old.copies);
        if (!o.keep) CHECK(st.kept == 0, "RX_VK_SHADOW_KEEP=0: %lld pages left with their bytes", (long long)st.kept);
        if (!o.prev) CHECK(st.back == 0 && st.old == 0, "RX_VK_SHADOW_PREV=0: %lld given back, %lld copies kept", (long long)st.back,
                           (long long)st.old);
    }
    CHECK(shadow_prev::alive().load() == 0, "copies kept after every model went: %lld", (long long)shadow_prev::alive().load());
    if (o.keep && o.prev) CHECK(total_left * 5 > total_copies + total_left, "random: only %lld left of %lld", (long long)total_left,
                                (long long)(total_copies + total_left));
    std::printf("random%s: %lld copies, %lld pages left uncopied (the old loop: %lld copies)%s%s\n", against_old ? " (old loop beside)" : "",
                (long long)total_copies, (long long)total_left, (long long)old_copies, o.keep ? "" : " RX_VK_SHADOW_KEEP=0",
                o.prev ? "" : " RX_VK_SHADOW_PREV=0");
}

void set_env(const char* name, const char* v) {
#ifdef _WIN32
    _putenv_s(name, v);
#else
    setenv(name, v, 1);
#endif
}

void env_rules() {
    CHECK(shadow_prev::env_on("RX_VK_SHADOW_TEST_UNSET_NAME"), "a switch not set is not on");
    set_env("RX_VK_SHADOW_TEST_X", "0");
    CHECK(!shadow_prev::env_on("RX_VK_SHADOW_TEST_X"), "=0 is not off");
    set_env("RX_VK_SHADOW_TEST_X", "2");
    CHECK(shadow_prev::env_on("RX_VK_SHADOW_TEST_X"), "=2 is not on");
    {   // (Vers: moved, its copies go with it; aged and dropped, they are let go)
        shadow_prev::Vers a;
        std::unique_ptr<uint8_t[]> b(new uint8_t[4096]), d(new uint8_t[4096]);
        uint64_t h = 5;
        CHECK(shadow_prev::keep_old(b, h, a, 10, 7, 1) && a.n == 1 && a.p[0].b && a.p[0].hash == 5 && h == 0 && a.p[0].at == 7 &&
              shadow_prev::alive().load() == 1, "keep_old");
        shadow_prev::Vers c(std::move(a));
        CHECK(a.n == 0 && !a.p[0].b && c.n == 1 && c.p[0].b && c.p[0].hash == 5 && c.p[0].at == 7 &&
              shadow_prev::alive().load() == 1, "a Vers moved");
        CHECK(!shadow_prev::age(c, 7 + shadow_prev::kStale) && c.n == 1, "a version aged within kStale takes");
        CHECK(shadow_prev::age(c, 8 + shadow_prev::kStale) && c.n == 0 && c.p[0].hash == 0 && shadow_prev::alive().load() == 0,
              "a version aged");
        CHECK(!shadow_prev::age(c, 1000), "a Vers without a copy aged");
        CHECK(shadow_prev::keep_old(d, h, c, 10, 9, 1) && c.n == 1 && shadow_prev::alive().load() == 1, "keep_old after age");
        shadow_prev::drop_all(c);
        CHECK(c.n == 0 && shadow_prev::alive().load() == 0, "a Vers dropped");
        std::unique_ptr<uint8_t[]> none;
        CHECK(!shadow_prev::keep_old(none, h, c, 10, 10, 1) && shadow_prev::alive().load() == 0, "keep_old of no copy");
        // (the ring: as many as asked, then the oldest's buffer is the new copy's, and no more is allocated)
        shadow_prev::Vers v3;
        std::unique_ptr<uint8_t[]> x(new uint8_t[4096]);
        uint64_t hx = 0;
        for (int k = 0; k < 3; k++)
            CHECK(shadow_prev::keep_old(x, hx, v3, 100, (uint64_t)(k + 1), 3), "keep_old %d with 3 kept", k);
        CHECK(v3.n == 3 && shadow_prev::alive().load() == 3, "3 kept: %d, %lld", (int)v3.n, (long long)shadow_prev::alive().load());
        CHECK(shadow_prev::keep_old(x, hx, v3, 100, 4, 3) && v3.n == 3 && shadow_prev::alive().load() == 3,
              "a full ring: %d kept, %lld", (int)v3.n, (long long)shadow_prev::alive().load());
        CHECK(v3.p[0].at == 4 && v3.p[1].at == 3 && v3.p[2].at == 2, "the ring's order: %llu, %llu, %llu",
              (unsigned long long)v3.p[0].at, (unsigned long long)v3.p[1].at, (unsigned long long)v3.p[2].at);
        shadow_prev::drop_all(v3);
        CHECK(shadow_prev::alive().load() == 0, "3 let go");
    }
    {   // (act's identity guard: a buffer at the same depth but kept at another take is not the version the look compared)
        shadow_prev::Vers v;
        std::unique_ptr<uint8_t[]> b(new uint8_t[4096]);
        std::memcpy(b.get(), bytes_of(30, 1).data(), 4096);
        uint64_t h = 0;
        CHECK(shadow_prev::keep_old(b, h, v, 100, 5, 2) && v.n == 1 && v.p[0].b, "the guard: keep_old");
        shadow_prev::Look l;
        l.v = shadow_prev::BACK, l.b = b.get(), l.depth = 0, l.prev = v.p[0].b.get();
        l.at = v.p[0].at - 1;   // (the look compared a version kept before the one there now)
        CHECK(shadow_prev::act(l, true, true, false, b.get(), v) == shadow_prev::CHANGED, "act took a version kept again as BACK");
        l.at = v.p[0].at;
        CHECK(shadow_prev::act(l, true, true, false, b.get(), v) == shadow_prev::BACK, "act did not take the version it compared");
        CHECK(shadow_prev::act(l, true, true, true, b.get(), v) == shadow_prev::NONE, "act took a page the capture had twice");
        CHECK(shadow_prev::act(l, true, true, false, nullptr, v) == shadow_prev::NONE, "act took a page with no copy");
        shadow_prev::drop_all(v);
        CHECK(shadow_prev::alive().load() == 0, "the guard: let go");
    }
    {   // (RX_VK_SHADOW_VERS's number)
        set_env("RX_VK_SHADOW_VERS", "3");
        CHECK(shadow_prev::env_vers() == 3, "RX_VK_SHADOW_VERS=3 is not 3");
        set_env("RX_VK_SHADOW_VERS", "9");
        CHECK(shadow_prev::env_vers() == shadow_prev::kMaxVers, "RX_VK_SHADOW_VERS=9 is not the most");
        set_env("RX_VK_SHADOW_VERS", "0");
        CHECK(shadow_prev::env_vers() == shadow_prev::kVersDefault, "RX_VK_SHADOW_VERS=0 is not the default");
        set_env("RX_VK_SHADOW_VERS", "");
        CHECK(shadow_prev::env_vers() == shadow_prev::kVersDefault, "RX_VK_SHADOW_VERS= is not the default");
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string c = argc > 1 ? argv[1] : "";
    if (c == "gains") gains();
    else if (c == "random") random_takes(false);
    else if (c == "against_old") random_takes(true);
    else if (c == "env") env_rules();
    else { std::printf("cases: gains random against_old env\n"); return 2; }
    if (failures) std::printf("%s: %d failures\n", c.c_str(), failures);
    else std::printf("%s: ok\n", c.c_str());
    return failures ? 1 : 0;
}
