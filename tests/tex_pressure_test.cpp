// gpu/vk/tex_pressure.h, the texture budget by live VRAM use, against a fake heap that fills and empties: an arrival of 600
// textures after a tour, a tour of ten areas, another program taking and giving back VRAM, the system's commit low.
// tests/test_tex_pressure.py compiles and runs it, and runs it again against copies of the header with one rule broken.
//
// The model is vk_texture.cpp's texture_frame and texture_sweep cut down to what the budget decides: a look every 10
// presents (or at press.next), the budget (the line by live use, or the fixed size, then the 90% rule), a pass over the
// budget that takes the least recently used first, nothing used in the last `need` presents (120 by live use, 60 for a
// size), the render targets drawn over since whatever their age, tex_pressure::met and tex_pressure::stepped as
// texture_sweep calls them. What went is freed once the GPU is done with it (the next present), as free space in the
// blocks the driver still counts until a new texture takes it or the compaction gives it back. The come-back pause as
// texture_frame judges it at each look, texture_sweep notes what the line let go and create() what is made again.
// Presents at 60 a second throughout.
#include "tex_pressure.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static constexpr uint64_t MB = 1ull << 20;

/// The device-local heap as the driver counts it
struct Heap {
    uint64_t allowed = 0;    // what the driver allows the game now (VK_EXT_memory_budget)
    uint64_t other = 0;      // the game's other VRAM: render targets, buffers, mirrors
    uint64_t textures = 0;   // the textures' images
    uint64_t freeing = 0;    // textures gone that the GPU may still read: counted, not free yet
    uint64_t holes = 0;      // free space inside the blocks: counted by the driver, taken first by a new texture
    uint64_t used() const { return other + textures + freeing + holes; }
    uint64_t pressure() const { return used() - holes; }   // (texture_frame's pressure: the count less the free space)
    void take(uint64_t b) {
        holes -= std::min(holes, b);
        textures += b;
    }
};

struct Tex {
    uint64_t bytes = 0, last = 0;
    int area = -1;
    bool alive = false;
    bool dead = false;   // (a render target drawn over since: nothing comes back for it)
};

struct Model {
    bool live;
    size_t step;
    uint64_t pct = tex_pressure::kPct;
    bool commit_low = false;
    bool acting = false;   // (texture_frame's press.acting: the line acts at this look)
    bool come_back = true, held = false;   // (RX_VK_TEX_COME_BACK; the line standing aside this look)
    std::unique_ptr<tex_pressure::ComeBack> cb = std::make_unique<tex_pressure::ComeBack>();
    size_t pauses = 0, held_looks = 0;
    uint64_t frame = 100000, next = 0, total = 0;
    std::vector<Tex> tex;
    size_t passes = 0, evicted = 0, max_pass = 0, remade = 0, young = 0;
    int area_now = -1;
    size_t evicted_current = 0;   // (textures of the area being played that went)

    explicit Model(bool live_, const char* step_env = nullptr) : live(live_), step(tex_pressure::step_of(step_env, live_)) {}

    size_t add(Heap& h, uint64_t bytes, int area, bool dead = false) {
        tex.push_back({bytes, frame, area, true, dead});
        h.take(bytes), total += bytes;
        return tex.size() - 1;
    }
    /// The game samples texture i this present: one gone is made and uploaded again from the game's memory
    void use(Heap& h, size_t i) {
        Tex& t = tex[i];
        if (!t.alive) {
            t.alive = true, h.take(t.bytes), total += t.bytes, remade++;
            if (come_back) cb->made(i, frame, 256);   // (create(): one the line let go, back)
        }
        t.last = frame;
    }
    /// A present (texture_frame): the GPU done with what went, the compaction giving back some free space, a look now and then
    void present(Heap& h) {
        frame++;
        h.holes += h.freeing, h.freeing = 0;
        h.holes -= std::min<uint64_t>(h.holes, 64 * MB);
        if (frame % 10 && !(next && frame >= next)) return;
        next = 0;
        look(h);
    }
    void look(Heap& h) {
        const uint64_t allowed = h.allowed, pressure = h.pressure();
        held = false;
        if (live && come_back) {
            bool began = false;
            held = cb->held(frame, 256, &began);
            pauses += began, held_looks += held;
        }
        acting = live && !held && tex_pressure::acts(pressure, allowed, pct, acting);
        uint64_t budget = live ? tex_pressure::budget(tex_pressure::fixed(allowed), total, pressure, allowed, pct, commit_low, acting)
                               : tex_pressure::fixed(allowed);
        if (pressure > allowed / 10 * 9) {   // (the 90% rule, as it stays)
            const uint64_t over = pressure - allowed / 10 * 9;
            budget = std::min(budget, total > over ? total - over : 0);
        }
        if (total <= budget) return;   // (a pass under the budget: only what was drawn over goes; none of that here)
        passes++;
        std::vector<size_t> order;
        for (size_t i = 0; i < tex.size(); i++)
            if (tex[i].alive) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return tex[a].last < tex[b].last; });
        const uint64_t need = live ? 120 : 60;   // (texture_sweep's need_unused over the budget)
        uint64_t pending = 0;
        size_t chosen = 0;
        for (size_t i : order) {
            Tex& t = tex[i];
            if (tex_pressure::met(total, pending, budget, live) || frame - t.last < 3) break;   // (too_young: the dead targets' 3)
            if (frame - t.last < need && !t.dead) continue;
            if (!t.dead && tex_pressure::stepped(step, chosen)) continue;
            if (frame - t.last < need) young++;
            t.alive = false, pending += t.bytes, evicted++;
            if (!t.dead) chosen++;
            if (!t.dead && t.area == area_now) evicted_current++;
            if (live && !held && !t.dead && come_back) cb->went(i, frame);   // (texture_sweep: what the line let go)
        }
        if (tex_pressure::stepped(step, chosen)) next = frame + tex_pressure::kNextLook;
        max_pass = std::max(max_pass, chosen);
        total -= pending, h.textures -= pending, h.freeing += pending;
    }
};

static uint64_t rnd_state = 12345;
static uint64_t rnd(uint64_t n) {
    rnd_state = rnd_state * 6364136223846793005ull + 1442695040888963407ull;
    return (rnd_state >> 33) % n;
}

static void units() {
    using namespace tex_pressure;
    // (the switch: 80 unset, 0 off, else 50 to 95)
    CHECK(pct_of(nullptr) == 80 && pct_of("") == 80, "unset: 80");
    CHECK(pct_of("0") == 0 && pct_of("off") == 0 && pct_of("-5") == 0, "0: the fixed size");
    CHECK(pct_of("70") == 70 && pct_of("10") == 50 && pct_of("99") == 95 && pct_of("1") == 50, "a line: 50 to 95");
    // (where the line is live: more than 8.5 GB, the driver's budget, not integrated, no RX_VK_VRAM_MB, the switch on)
    CHECK(live(80, true, 30515 * MB, false, false), "a 32 GB card");
    CHECK(live(80, true, 8705 * MB, false, false) && !live(80, true, 8704 * MB, false, false), "8.5 GB: the rules for small cards");
    CHECK(!live(80, false, 30515 * MB, false, false), "no VK_EXT_memory_budget");
    CHECK(!live(80, true, 30515 * MB, true, false), "an integrated GPU");
    CHECK(!live(80, true, 30515 * MB, false, true), "RX_VK_VRAM_MB");
    CHECK(!live(0, true, 30515 * MB, false, false), "RX_VK_TEX_PRESSURE=0");
    // (the fixed size, the rule before)
    CHECK(fixed(30515 * MB) == 6144 * MB && fixed(7300 * MB) == 7300 * MB / 100 * 45 && fixed(9000 * MB) == 6144 * MB &&
          fixed(8704 * MB) == 8704 * MB / 100 * 45, "the fixed size: 45%% at 8.5 GB or less, else 70%%, at most 6 GB");
    CHECK(line(30515 * MB, 80) == 30515 * MB / 100 * 80, "the line");
    // (the budget: none under the line; past it the textures less the excess and 2% of the allowance)
    // (as a look finds it, the line not acting at the look before)
    const auto bud = [](uint64_t size, uint64_t t, uint64_t p, uint64_t a, uint64_t pct, bool low) {
        return budget(size, t, p, a, pct, low, acts(p, a, pct, false));
    };
    const uint64_t A = 10000 * MB;
    CHECK(bud(6144 * MB, 5000 * MB, 7999 * MB, A, 80, false) == UINT64_MAX, "under the line: no budget");
    CHECK(bud(6144 * MB, 5000 * MB, 8000 * MB, A, 80, false) == UINT64_MAX, "at the line: no budget");
    CHECK(bud(6144 * MB, 5000 * MB, 8500 * MB, A, 80, false) == 5000 * MB - (500 * MB + A / 50), "past it: %llu MB",
          (unsigned long long)(bud(6144 * MB, 5000 * MB, 8500 * MB, A, 80, false) / MB));
    CHECK(bud(6144 * MB, 300 * MB, 9900 * MB, A, 80, false) == 0, "more over than the textures hold: 0");
    CHECK(bud(6144 * MB, 5000 * MB, 7000 * MB, A, 80, true) == 6144 * MB, "the commit low: the fixed size");
    CHECK(bud(6144 * MB, 7000 * MB, 9000 * MB, A, 80, true) == 7000 * MB - (1000 * MB + A / 50), "the commit low past the line: the lower");
    CHECK(bud(6144 * MB, 5000 * MB, 9000 * MB, 0, 80, false) == UINT64_MAX, "nothing allowed known: none");
    // (once it acts, on until the use is 2% under the line: 7,800 MB of 10,000)
    CHECK(under(A, 80) == 7800 * MB && under(100 * MB, 1) == 0, "2%% under the line");
    CHECK(acts(8001 * MB, A, 80, false) && !acts(8000 * MB, A, 80, false) && !acts(7900 * MB, A, 80, false), "past the line");
    CHECK(acts(7900 * MB, A, 80, true) && acts(7801 * MB, A, 80, true) && !acts(7800 * MB, A, 80, true), "acting: on to 2%% under it");
    CHECK(!acts(9000 * MB, 0, 80, true) && !acts(9000 * MB, A, 0, true), "nothing allowed known, or off: never");
    CHECK(budget(6144 * MB, 5000 * MB, 7900 * MB, A, 80, false, true) == 4900 * MB &&
          budget(6144 * MB, 5000 * MB, 7900 * MB, A, 80, false, false) == UINT64_MAX &&
          budget(6144 * MB, 5000 * MB, 7900 * MB, A, 80, true, false) == 6144 * MB, "acting under the line, or not");
    // (a pass: down to the budget by live use, 7/8 of a size; what it chose past what the textures hold is met)
    CHECK(cut_to(8000 * MB, true) == 8000 * MB && cut_to(8000 * MB, false) == 7000 * MB, "cut_to");
    CHECK(met(9000 * MB, 1000 * MB, 8000 * MB, true) && !met(9000 * MB, 999 * MB, 8000 * MB, true), "met by live use");
    CHECK(!met(9000 * MB, 1000 * MB, 8000 * MB, false) && met(9000 * MB, 2000 * MB, 8000 * MB, false), "met for a size: 7/8");
    CHECK(met(100 * MB, 200 * MB, 8000 * MB, true) && met(100 * MB, 200 * MB, 0, false), "chosen past the total: met, not wrapped");
    CHECK(!met(100 * MB, 0, 0, true), "a budget of 0: not met while anything is left");
    // (the step)
    CHECK(step_of(nullptr, true) == 64 && step_of(nullptr, false) == 0 && step_of("", true) == 64, "64 by live use, none for a size");
    CHECK(step_of("16", false) == 16 && step_of("16", true) == 16 && step_of("0", true) == 0 && step_of("x", true) == 0,
          "RX_VK_TEX_STEP on any card; 0: none");
    CHECK(!stepped(64, 63) && stepped(64, 64) && stepped(64, 65) && !stepped(0, 100000), "stepped");
    // (the commit: NX2navi's default, the larger of 4 GB and a tenth of the limit)
    const uint64_t G = 1024 * MB;
    CHECK(commit_low(3 * G, 40 * G, -1) && !commit_low(5 * G, 40 * G, -1), "a 40 GB limit: 4 GB");
    CHECK(commit_low(9 * G, 100 * G, -1) && !commit_low(11 * G, 100 * G, -1), "a 100 GB limit: a tenth");
    CHECK(!commit_low(0, 40 * G, 0) && !commit_low(0, 0, -1), "0: never; no limit known: never");
    CHECK(commit_low(1 * G, 40 * G, 2048) && !commit_low(3 * G, 40 * G, 2048), "RX_VK_TEX_COMMIT_FREE_MB=2048");
    // (the budget the log shows)
    CHECK(shown(UINT64_MAX, 5000 * MB, 7000 * MB, A, 80) == 6000 * MB && shown(4321 * MB, 5000 * MB, 9000 * MB, A, 80) == 4321 * MB &&
          shown(UINT64_MAX, 5000 * MB, 9000 * MB, A, 80) == 5000 * MB, "shown");
}

/// Keep taken: a texture the game lets go and takes back waits 8 times its longest gap before it may go early
static void keep_taken() {
    using namespace tex_pressure;
    CHECK(taken_back(0, 50) == 50 && taken_back(80, 50) == 80 && taken_back(50, 80) == 80, "the longest gap");
    CHECK(taken_back(0, 1ull << 40) == 1u << 24 && (uint64_t)taken_back(0, 1ull << 40) * kGapTimes < (1ull << 32), "capped");
    CHECK(let_go_wait(30, 0, true) == 30 && let_go_wait(30, 3, true) == 30 && let_go_wait(30, 10, true) == 80 &&
          let_go_wait(30, 300, true) == 2400, "8 times the gap when longer than the rule's own 30");
    CHECK(let_go_wait(30, 300, false) == 30, "RX_VK_TEX_KEEP_TAKEN=0: the rule's own wait");
    // (Kakariko's haze: let go and taken back every 300 presents, VRAM short all the while. The let-go rule looks every 10
    // presents; the texture goes early once its wait is up and is made again at the take-back)
    for (bool on : {false, true}) {
        uint32_t gap = 0;
        size_t gone = 0;
        for (int cycle = 0; cycle < 20; cycle++) {
            const uint64_t let_go = 1000 + (uint64_t)cycle * 300;   // (the take-back comes 300 presents after each let-go)
            bool went = false;
            for (uint64_t f = let_go; f < let_go + 300; f += 10)
                if (!went && f - let_go >= let_go_wait(30, gap, on)) went = true;
            gone += went;
            gap = taken_back(gap, 300);
        }
        if (on) CHECK(gone == 1, "keep taken: the haze went %zu times in 20 take-backs (once: before the first take-back)", gone);
        else CHECK(gone == 20, "as before: the haze went %zu times in 20 take-backs", gone);
    }
}

/// The come-back pause: the rules, then a thrash on a 12 GB card (two sets of textures that do not fit under the line together,
/// each wanted for 5 s in turn): the line stands aside while what it let go comes back, the use stays under 90%
static void come_back() {
    using T = tex_pressure::ComeBack;
    auto c = std::make_unique<T>();
    bool began = false;
    CHECK(!c->held(1000, 256, &began) && !began, "nothing let go: no pause");
    c->went(42, 1000);
    CHECK(!c->made(43, 1100, 256) && c->made(42, 1100, 256) && !c->made(42, 1101, 256), "made again: by its key, once");
    // (another key in the same slot: not the one noted; and the newest noted wins the slot)
    uint64_t other = 43;
    while (T::at(other) != T::at(42)) other++;
    c->went(42, 1500);
    CHECK(!c->made(other, 1600, 256) && c->made(42, 1600, 256), "a key in the same slot is another texture");
    c->went(42, 1700), c->went(other, 1700);
    CHECK(!c->made(42, 1800, 256) && c->made(other, 1800, 256), "the newest noted wins the slot");
    c->went(7, 2000);
    CHECK(!c->made(7, 2001 + 601, 256) && c->made(7, 2001 + 600, 256), "after 600 presents: not come back (%llu)",
          (unsigned long long)c->backs_all);
    c->went(9, 3000);
    CHECK(c->made(9, 3000 + 1100, 512), "at 120 a second 600 presents at 60 are 1,200");
    // (a period: 64 went, 32 back: a pause of 600; doubled at the next such period, at most 7,200; a period with 64 and fewer
    // back ends the doubling)
    auto period = [&](uint64_t now, int went, int back) {
        for (int i = 0; i < went; i++) c->went(now * 1000 + (uint64_t)i, now);
        for (int i = 0; i < back; i++) c->made(now * 1000 + (uint64_t)i, now + 5, 256);
        return c->held(now + T::kPeriod, 256, &began);
    };
    c = std::make_unique<T>();
    c->held(10000, 256, &began);
    CHECK(period(10000, 64, 32) && began && c->hold == 600, "half back: a pause of 600 (%llu)", (unsigned long long)c->hold);
    CHECK(c->held(10000 + 240 + 599, 256, &began) && !c->held(10000 + 240 + 600, 256, &began), "the pause ends after 600");
    CHECK(period(10840, 64, 40) && c->hold == 1200, "again: 1,200 (%llu)", (unsigned long long)c->hold);
    uint64_t now = 12280;
    for (int i = 0; i < 6; i++) now += 7200 + T::kPeriod, period(now, 100, 90);
    CHECK(c->hold == 7200, "at most 7,200 (%llu)", (unsigned long long)c->hold);
    now += 7200 + T::kPeriod;
    CHECK(!period(now, 64, 31) && c->hold == 0, "fewer than half back: no pause, the doubling ended (%llu)", (unsigned long long)c->hold);
    now += T::kPeriod;
    CHECK(period(now, 63, 63) == false, "fewer than 64 went: no pause");
    now += T::kPeriod;
    CHECK(period(now, 64, 32) && c->hold == 600, "the next pause: 600 again (%llu)", (unsigned long long)c->hold);

    // (the thrash, with the pause and without)
    size_t remade[2] = {}, evicted[2] = {}, pauses = 0;
    for (int on = 0; on < 2; on++) {
        Heap h;
        h.allowed = 11000 * MB, h.other = 4500 * MB;
        Model m(true);
        m.come_back = on;
        std::vector<size_t> hot, a, b;
        for (int i = 0; i < 1000; i++) hot.push_back(m.add(h, 2 * MB, 0));
        for (int i = 0; i < 600; i++) a.push_back(m.add(h, 2 * MB, 1));
        for (int i = 0; i < 600; i++) b.push_back(m.add(h, 2 * MB, 2));
        for (int p = 0; p < 18000; p++) {
            for (size_t i : hot) m.use(h, i);
            for (size_t i : (p / 300) % 2 ? b : a) m.use(h, i);
            m.present(h);
            CHECK(h.pressure() <= h.allowed / 10 * 9 + 128 * MB, "thrash (%s): past 90%% of what the driver allows at %d (%llu MB)",
                  on ? "paused" : "no pause", p, (unsigned long long)(h.pressure() / MB));
            CHECK(h.used() <= h.allowed, "thrash: the driver's count past what it allows at %d", p);
        }
        remade[on] = m.remade, evicted[on] = m.evicted;
        if (on) pauses = m.pauses;
        CHECK(m.max_pass <= 64, "thrash: the largest pass %zu", m.max_pass);
    }
    // (half as many or fewer: the 240-present periods judge drops and come-backs that fall in different periods now and then,
    // which ends the doubling, as NX2navi's rule does)
    CHECK(pauses >= 3 && remade[1] * 2 < remade[0], "thrash: %zu pauses, %zu made again with them against %zu without", pauses,
          remade[1], remade[0]);
    std::printf("thrash, 12 GB: without the pause %zu went and %zu were made again; with it %zu and %zu (%zu pauses)\n", evicted[0],
                remade[0], evicted[1], remade[1], pauses);
}

/// The Market after a tour on the owner's card (RTX 5090, ~30.5 GB allowed): the tour's leftovers held, the Market's own
/// textures (some used every frame, the backdrops now and then), 600 new ones arriving two a present
static void market_arrival(bool live, Model& m, Heap& h) {
    h.allowed = 30515 * MB, h.other = 7000 * MB;
    std::vector<size_t> hot, backdrop;
    m.frame = 100000;
    for (int i = 0; i < 1450; i++) m.add(h, (1 + rnd(2)) * MB, 0), m.tex.back().last = 99000 - rnd(3000);   // (the tour's: 2.2 GB)
    for (int i = 0; i < 900; i++) hot.push_back(m.add(h, (1 + rnd(4)) * MB, 1));                         // (2.3 GB)
    for (int i = 0; i < 850; i++) backdrop.push_back(m.add(h, (1 + rnd(3)) * MB, 1));                    // (1.7 GB)
    std::vector<size_t> fresh;
    m.area_now = 1;
    for (int p = 0; p < 1800; p++) {
        if (fresh.size() < 600) fresh.push_back(m.add(h, (uint64_t)(512 + rnd(1536)) * 1024, 1)), fresh.push_back(m.add(h, (uint64_t)(512 + rnd(1536)) * 1024, 1));
        for (size_t i : hot) m.use(h, i);
        for (size_t i : fresh) m.use(h, i);
        for (size_t k = 0; k < backdrop.size(); k++)
            if ((m.frame + k * 7) % (90 + k % 211) == 0) m.use(h, backdrop[k]);
        m.present(h);
        CHECK(h.pressure() <= h.allowed, "market (%s): the use past what the driver allows", live ? "live" : "fixed");
        if (live) CHECK(h.pressure() <= tex_pressure::line(h.allowed, 80), "market by live use: past the line (%llu MB)",
                        (unsigned long long)(h.pressure() / MB));
    }
}

/// The same arrival on a 12 GB card (~11 GB allowed): the line is reached, the tour's leftovers go a step at a time
static void arrival_12gb(Model& m, Heap& h) {
    h.allowed = 11000 * MB, h.other = 4000 * MB;
    std::vector<size_t> hot;
    m.frame = 100000;
    for (int i = 0; i < 800; i++) m.add(h, 2 * MB, 0), m.tex.back().last = 99000 - rnd(3000);   // (the tour's: 1.6 GB)
    for (int i = 0; i < 1300; i++) hot.push_back(m.add(h, 2 * MB, 1));                         // (the Market's: 2.6 GB)
    m.area_now = 1;
    size_t first_over = 0;
    for (int p = 0; p < 1800; p++) {
        if (hot.size() < 1900) hot.push_back(m.add(h, (uint64_t)(1024 + rnd(1024)) * 1024, 1)), hot.push_back(m.add(h, (uint64_t)(1024 + rnd(1024)) * 1024, 1));
        for (size_t i : hot) m.use(h, i);
        m.present(h);
        CHECK(h.pressure() <= h.allowed && h.used() <= h.allowed, "12 GB arrival: past what the driver allows (%llu, %llu MB)",
              (unsigned long long)(h.pressure() / MB), (unsigned long long)(h.used() / MB));
        if (h.pressure() > tex_pressure::line(h.allowed, 80) && !first_over) first_over = (size_t)p;
        // (two seconds after the last of the arrival came in, the use is back under the line)
        if (p > 300 + 120) CHECK(h.pressure() <= tex_pressure::line(h.allowed, 80), "12 GB arrival: still past the line at %d (%llu MB)",
                                 p, (unsigned long long)(h.pressure() / MB));
    }
    CHECK(first_over > 0, "12 GB arrival: the line was never reached (the scenario tests nothing)");
    CHECK(h.pressure() <= tex_pressure::under(h.allowed, 80), "12 GB arrival: not 2%% under the line at the end (%llu MB)",
          (unsigned long long)(h.pressure() / MB));
}

/// Another program fills the card and empties it again, and then takes 3 GB of what the driver allows (a 16 GB card)
static void fill_and_empty(Model& m, Heap& h) {
    h.allowed = 15000 * MB, h.other = 4000 * MB;
    std::vector<size_t> hot;
    m.frame = 100000;
    m.area_now = 1;
    for (int i = 0; i < 2000; i++) m.add(h, 2 * MB, 0), m.tex.back().last = 99000 - rnd(3000);
    for (int i = 0; i < 1000; i++) hot.push_back(m.add(h, 2 * MB, 1));
    size_t evicted_at_rest = 0;
    for (int p = 0; p < 3600; p++) {
        if (p < 500) h.other += 10 * MB;                    // (fills: 4 -> 9 GB)
        else if (p >= 1100 && p < 1600) h.other -= 10 * MB;   // (empties again)
        if (p == 1650)   // (an area's 1,500 textures come in, seen once: under the line still)
            for (int i = 0; i < 1500; i++) m.add(h, 2 * MB, 2);
        if (p == 1700) evicted_at_rest = m.evicted;
        if (p == 2300) {   // (nothing went while it was empty; then another program takes 3 GB of what the driver allows)
            CHECK(m.evicted == evicted_at_rest, "emptied: %zu more went", m.evicted - evicted_at_rest);
            h.allowed = 12000 * MB;
        }
        for (size_t i : hot) m.use(h, i);
        m.present(h);
        CHECK(h.pressure() <= h.allowed, "fill: the use past what the driver allows at %d", p);
        const uint64_t at = tex_pressure::line(h.allowed, 80);
        if ((p >= 560 && p < 1100) || p >= 2420) CHECK(h.pressure() <= at, "fill: past the line at %d (%llu of %llu MB)", p,
                                                       (unsigned long long)(h.pressure() / MB), (unsigned long long)(at / MB));
    }
    CHECK(evicted_at_rest > 0 && m.evicted > evicted_at_rest, "fill: nothing went when it filled (%zu) or when the allowance fell (%zu)",
          evicted_at_rest, m.evicted);
}

/// A tour of ten areas and back to the first: each area 600 textures (400 every frame, 200 now and then), 30 s in each
static void tour(Model& m, Heap& h, uint64_t allowed, uint64_t other, size_t* remade_back, const char* what) {
    h.allowed = allowed, h.other = other;
    m.frame = 100000;
    std::vector<std::vector<size_t>> areas(10);
    for (int a = 0; a <= 10; a++) {
        const int area = a % 10;
        m.area_now = area;
        if (a < 10)
            for (int i = 0; i < 600; i++) areas[area].push_back(m.add(h, (1 + rnd(6)) * MB, area));
        const size_t remade0 = m.remade;
        for (int p = 0; p < (a < 10 ? 1800 : 600); p++) {
            for (size_t k = 0; k < areas[area].size(); k++)
                if (k < 400 || (m.frame + k * 13) % (150 + k % 251) == 0) m.use(h, areas[area][k]);
            m.present(h);
            CHECK(h.pressure() <= h.allowed, "%s: past what the driver allows in area %d", what, area);
            if (m.live && p >= 600) CHECK(h.pressure() <= tex_pressure::line(h.allowed, 80), "%s: past the line in area %d at %d (%llu MB)",
                                          what, area, p, (unsigned long long)(h.pressure() / MB));
        }
        if (a == 10) *remade_back = m.remade - remade0;
    }
}

int main() {
    units();
    keep_taken();
    come_back();
    {   // (the owner's card: by live use nothing goes; the fixed 6 GB cut hundreds at once)
        Heap h;
        Model live(true);
        market_arrival(true, live, h);
        CHECK(live.evicted == 0 && live.remade == 0, "market by live use: %zu went, %zu made again", live.evicted, live.remade);
        Heap h2;
        Model fixed(false);
        market_arrival(false, fixed, h2);
        CHECK(fixed.max_pass > 64, "market, the fixed size: the largest cut %zu (the cuts of hundreds are gone only by live use)", fixed.max_pass);
        std::printf("market arrival, 32 GB: by live use %zu went; the fixed 6 GB: %zu went in %zu passes, the largest %zu, %zu made again\n",
                    live.evicted, fixed.evicted, fixed.passes, fixed.max_pass, fixed.remade);
        Heap h3;   // (and with RX_VK_TEX_STEP=64 the fixed size's cuts are steps too)
        Model fixed_step(false, "64");
        market_arrival(false, fixed_step, h3);
        CHECK(fixed_step.max_pass <= 64 && fixed_step.evicted > 0, "market, the fixed size in steps: the largest %zu", fixed_step.max_pass);
    }
    {
        Heap h;
        Model m(true);
        arrival_12gb(m, h);
        CHECK(m.max_pass <= 64 && m.evicted > 64, "12 GB arrival: the largest pass %zu of %zu", m.max_pass, m.evicted);
        CHECK(m.evicted_current == 0 && m.young == 0, "12 GB arrival: %zu of the Market's went, %zu used in the last 2 s", m.evicted_current, m.young);
        std::printf("arrival of 600, 12 GB: %zu went in %zu passes, at most %zu a pass, none of the Market's\n", m.evicted, m.passes, m.max_pass);
    }
    {
        Heap h;
        Model m(true);
        fill_and_empty(m, h);
        CHECK(m.max_pass <= 64 && m.evicted_current == 0 && m.remade == 0, "fill: the largest pass %zu, %zu in use went, %zu made again",
              m.max_pass, m.evicted_current, m.remade);
        std::printf("fill and empty, 16 GB: %zu went in %zu passes, at most %zu a pass\n", m.evicted, m.passes, m.max_pass);
    }
    struct Card { uint64_t allowed, other; const char* name; };
    static const Card cards[] = {{30515 * MB, 7000 * MB, "tour, 32 GB"}, {11000 * MB, 4000 * MB, "tour, 12 GB"},
                                 {15000 * MB, 5000 * MB, "tour, 16 GB"}};
    for (const Card& c : cards) {
        const char* name = c.name;
        Heap h;
        Model m(true);
        size_t back = 0;
        tour(m, h, c.allowed, c.other, &back, name);
        CHECK(m.max_pass <= 64 && m.evicted > 0, "%s: the largest pass %zu of %zu", name, m.max_pass, m.evicted);
        CHECK(m.evicted_current == 0, "%s: %zu textures of the area played went", name, m.evicted_current);
        CHECK(m.remade == back && back > 0, "%s: %zu made again, %zu of them back in the first area (only those come back)", name, m.remade, back);
        std::printf("%s: %zu went in %zu passes, at most %zu a pass; back in the first area %zu made again\n", name, m.evicted, m.passes,
                    m.max_pass, back);
    }
    {   // (the fixed size on the owner's card in the same tour: cuts of hundreds)
        Heap h;
        Model m(false);
        size_t back = 0;
        tour(m, h, 30515 * MB, 7000 * MB, &back, "tour, 32 GB, the fixed size");
        CHECK(m.max_pass > 64, "tour, the fixed size: the largest cut %zu", m.max_pass);
    }
    {   // (the system's commit low: the fixed size comes back, in steps; then the line again)
        Heap h;
        h.allowed = 30515 * MB, h.other = 7000 * MB;
        Model m(true);
        std::vector<size_t> hot;
        for (int i = 0; i < 3500; i++) m.add(h, 2 * MB, 0), m.tex.back().last = 99000 - rnd(3000);
        for (int i = 0; i < 1000; i++) hot.push_back(m.add(h, 2 * MB, 1));
        m.area_now = 1;
        size_t low_end = 0;
        for (int p = 0; p < 1200; p++) {
            m.commit_low = p < 600;
            if (p == 600) low_end = m.evicted;
            for (size_t i : hot) m.use(h, i);
            m.present(h);
            if (p >= 300 && p < 600) CHECK(m.total <= 6144 * MB, "commit low: the textures %llu MB at %d", (unsigned long long)(m.total / MB), p);
        }
        CHECK(m.max_pass <= 64 && low_end > 64 && m.evicted == low_end && m.evicted_current == 0,
              "commit low: the largest pass %zu, %zu went while low, %zu after, %zu in use", m.max_pass, low_end, m.evicted - low_end,
              m.evicted_current);
    }
    {   // (the render targets drawn over since go whatever the step: 200 of them and 64 live textures in one pass)
        Heap h;
        h.allowed = 11000 * MB, h.other = 7000 * MB;   // (9,400 MB in use: past the line of 8,800 by 600, 820 to go)
        Model m(true);
        m.frame = 100000;
        for (int i = 0; i < 1000; i++) m.add(h, 2 * MB, 0), m.tex.back().last = 99000;
        for (int i = 0; i < 200; i++) m.add(h, 2 * MB, 0, true), m.tex.back().last = 99990;
        m.frame = 100009;
        m.present(h);
        CHECK(m.passes == 1 && m.evicted == 264 && m.max_pass == 64 && m.next == m.frame + 2,
              "dead targets: %zu passes, %zu went, %zu live, next look %llu", m.passes, m.evicted, m.max_pass,
              (unsigned long long)(m.next - m.frame));
    }
    if (failures) {
        std::printf("tex_pressure: %d checks failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("tex_pressure: all checks passed\n");
    return EXIT_SUCCESS;
}
