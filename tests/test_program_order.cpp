// (Triforce) The programs read ahead in the order the last sessions first needed them (gpu/vk/program_order.h): the file, the
// merge of a session's order with the ones before, the session's record, and the read-ahead in that order (what it reads, what
// it keeps, what it lets go). Run by tests/test_program_order.py.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "program_order.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

using namespace mkwvk::program_order;

static std::vector<uint64_t> keys_from(uint64_t first, size_t n) {
    std::vector<uint64_t> out;
    for (size_t i = 0; i < n; i++) out.push_back(first + i);
    return out;
}

static void file() {
    const std::vector<uint64_t> keys = {0x1111, 0xdeadbeefcafe, 7};
    const auto bytes = write(keys);
    CHECK(bytes.size() == 16 + 3 * 8 + 8);
    CHECK(read(bytes.data(), bytes.size()) == keys);
    CHECK(read(write({}).data(), write({}).size()).empty());
    auto cut = bytes;
    cut.pop_back();
    CHECK(read(cut.data(), cut.size()).empty());   // (cut short: none)
    auto longer = bytes;
    longer.push_back(0);
    CHECK(read(longer.data(), longer.size()).empty());   // (more than it says: none)
    auto bad = bytes;
    bad[20] ^= 1;
    CHECK(read(bad.data(), bad.size()).empty());   // (a key changed: the check word says so)
    auto other = bytes;
    other[0] ^= 1;
    CHECK(read(other.data(), other.size()).empty());   // (not this file)
    auto version = bytes;
    version[4] = 2;
    CHECK(read(version.data(), version.size()).empty());
    CHECK(read(nullptr, 0).empty());
    // (too many keys for the file: refused, whatever the bytes)
    std::vector<uint8_t> many(16 + (kKeysMax + 1) * 8 + 8);
    const uint32_t head[4] = {kMagic, kVersion, (uint32_t)(kKeysMax + 1), 0};
    std::memcpy(many.data(), head, 16);
    CHECK(read(many.data(), many.size()).empty());
}

static void merged() {
    // (this session's first, then the ones before it has not met, each once, no zero key, max at most)
    CHECK(merge({3, 1, 2}, {9, 1, 8, 3, 7}) == (std::vector<uint64_t>{3, 1, 2, 9, 8, 7}));
    CHECK(merge({}, {5, 6}) == (std::vector<uint64_t>{5, 6}));
    CHECK(merge({4, 0, 4}, {}) == (std::vector<uint64_t>{4}));
    CHECK(merge({1, 2, 3}, {4, 5}, 4) == (std::vector<uint64_t>{1, 2, 3, 4}));
    const auto big = merge(keys_from(1, kKeysMax), keys_from(100000, 10));
    CHECK(big.size() == kKeysMax && big.back() == kKeysMax);
}

static void record() {
    Record r;
    CHECK(r.add(5) && r.add(9) && !r.add(5) && !r.add(0));
    CHECK(r.keys == (std::vector<uint64_t>{5, 9}));
    CHECK(!r.due(kSaveEveryMs - 1) && r.due(kSaveEveryMs));
    r.saved = r.keys.size();
    CHECK(!r.due(kSaveEveryMs * 10));   // (nothing new since the last write)
    r.add(11);
    CHECK(r.due(kSaveEveryMs));
    Record full;
    for (uint64_t k = 1; k <= kKeysMax + 10; k++) full.add(k);
    CHECK(full.keys.size() == kKeysMax);
}

/// The read-ahead in the order, by the rules before (RX_VK_PROGRAM_ORDER_KEEP=0): reads in it, kept apart, up to the bound; a
/// draw that takes one frees its room; one far behind the draws goes, and nothing is read there
static void ahead() {
    Ahead a;
    a.keep_all = false;
    CHECK(!a.more());
    a.load(keys_from(1, 3000));
    std::unordered_set<uint64_t> on_disk;
    for (uint64_t k = 1; k <= 3000; k++)
        if (k % 10) on_disk.insert(k);   // (every tenth no longer kept in the cache: skipped)
    const auto wanted = [&](uint64_t k) { return on_disk.count(k) != 0; };
    // (the workers read while no draw asks: in the order, up to kKeptMax at a time)
    std::vector<uint64_t> read;
    while (uint64_t k = a.choose(wanted)) {
        CHECK(a.is_chosen(k));
        a.read_done(k, true);
        read.push_back(k);
    }
    CHECK(read.size() == kKeptMax && a.kept.size() == kKeptMax);
    CHECK(read.front() == 1 && read[8] == 9 && read[9] == 11);   // (in the order, 10 skipped)
    CHECK(!a.more());
    // (the title's draws take the first ones: room again, the read goes on from where it was. Where the draws are: the middle
    // place of the last kRecent, places 84 to 99 here)
    std::vector<uint64_t> gone;
    for (uint64_t k = 1; k <= 100; k++) a.met(k, gone);
    CHECK(gone.empty() && a.kept.size() == kKeptMax - 90 && a.furthest == 92);
    CHECK(a.choose(wanted) == read.back() + 1);
    // (a read that made nothing, or one a draw took meanwhile: not kept)
    a.read_done(read.back() + 1, false);
    CHECK(!a.kept.count(read.back() + 1) && !a.is_chosen(read.back() + 1));
    // (one program met out of the order's turn (a session that first drew it later): nothing goes)
    const size_t kept_before = a.kept.size();
    a.met(2000, gone);
    CHECK(gone.empty() && a.kept.size() == kept_before && a.furthest < 100);
    // (the draws go elsewhere in the order (that session went to another area): those far behind them go, and are not read)
    for (uint64_t k = 2001; k <= 2000 + kRecent; k++) a.met(k, gone);
    CHECK(a.furthest > 2000 && a.kept.empty());
    CHECK(gone.size() == kept_before);
    for (const uint64_t k : gone) CHECK(k - 1 < (uint64_t)(a.furthest - kBehind));
    const uint64_t next = a.choose(wanted);
    CHECK(next == (uint64_t)(a.furthest - kBehind) + 1 || next == (uint64_t)(a.furthest - kBehind) + 2);   // (10s skipped)
    // (a draw meeting one far behind: no move back)
    const int64_t at = a.furthest;
    a.met(50, gone);
    CHECK(a.furthest == at);
    // (a key the order does not name: nothing moves)
    a.met(999999, gone);
    CHECK(a.furthest == at);
}

/// A session as the lab's S1, the path the order's own: the title and the file select need the first 300 programs, the save's
/// load the next 400 in a burst. Read ahead in the order during the menus, the load finds every one of them kept (the asks'
/// shelf aside), by either rules
static void session(bool keep_all) {
    Ahead a;
    a.keep_all = keep_all;
    a.load(keys_from(1, 4000));
    const auto wanted = [](uint64_t) { return true; };
    size_t found = 0, missed = 0;
    // (the menus: a draw now and then, the workers read between)
    for (uint64_t k = 1; k <= 300; k++) {
        for (int i = 0; i < 8; i++)
            if (uint64_t c = a.choose(wanted)) a.read_done(c, true);
        (a.kept.count(k) ? found : missed)++;
        std::vector<uint64_t> gone;
        a.met(k, gone);
        if (k == 150) a.met(3500, gone);   // (one met out of turn: nothing goes)
        CHECK(gone.empty());
    }
    // (the load: 400 programs at once, no read between)
    for (uint64_t k = 301; k <= 700; k++) {
        (a.kept.count(k) ? found : missed)++;
        std::vector<uint64_t> gone;
        a.met(k, gone);
    }
    CHECK(missed <= 1 && found >= 699);   // (the very first one is the read-ahead's start)
}

/// ((Triforce, wp19) Kept until needed: what is read stays until a draw takes it, whatever the draws meet meanwhile; nothing is
/// passed by; the bound is the whole order; an ask's read the order names is kept with its own; the RAM short lets all go
static void kept_until_needed() {
    Ahead a;
    CHECK(a.keep_all && a.max_kept() == kKeepAllMax && kKeepAllMax >= kKeysMax);
    a.load(keys_from(1, 3000));
    std::unordered_set<uint64_t> on_disk;
    for (uint64_t k = 1; k <= 3000; k++)
        if (k % 10) on_disk.insert(k);
    const auto wanted = [&](uint64_t k) { return on_disk.count(k) != 0; };
    // (the first 100 read; then the draws meet programs far ahead in the order, out of its turn: nothing goes, nothing is passed)
    std::vector<uint64_t> gone;
    for (int i = 0; i < 100; i++) {
        const uint64_t k = a.choose(wanted);
        CHECK(k);
        a.read_done(k, true);
    }
    const size_t kept = a.kept.size();
    for (uint64_t k = 2001; k <= 2000 + 4 * kRecent; k++) a.met(k, gone);
    CHECK(gone.empty() && a.kept.size() == kept);
    const uint64_t next = a.choose(wanted);
    CHECK(next == 112);   // (on from where it was: 100 read of 1..111, the tens skipped)
    a.read_done(next, true);
    // (an ask read one the order names before the order came to it: kept with the order's, once; not one it does not name)
    CHECK(a.keep(2500) && a.kept.count(2500) && !a.keep(2500) && !a.keep(999999));
    a.chosen.insert(2600);
    CHECK(!a.keep(2600));   // (chosen here: the order's own read keeps it)
    a.chosen.erase(2600);
    // (the whole order read: up to the bound, which is all of it)
    while (const uint64_t k = a.choose(wanted)) a.read_done(k, true);
    CHECK(!a.more() && a.next == 3000 && a.kept.size() == 2701);   // (the 2700 not of the tens, and 2500 the ask kept)
    // (a draw takes one: it leaves the kept, nothing else does)
    a.met(5, gone);
    CHECK(gone.empty() && a.kept.size() == 2700 && !a.kept.count(5));
    // (the RAM short: all go)
    a.let_go_all(gone);
    CHECK(gone.size() == 2700 && a.kept.empty());
    // (the pipelines' warm keeps its window: the first kKeptMax kept read with it, those beyond cold; as draws take warmed ones,
    // the nearest cold one's warm is due)
    Ahead c;
    c.load(keys_from(1, 1000));
    std::vector<uint64_t> order_read;
    while (const uint64_t k = c.choose([](uint64_t) { return true; })) {
        CHECK(c.is_cold(k) == (order_read.size() >= kKeptMax));
        c.read_done(k, true);
        order_read.push_back(k);
    }
    CHECK(order_read.size() == 1000 && c.warmed() == kKeptMax && c.cold.size() == 1000 - kKeptMax);
    CHECK(!c.warm_due() && !c.next_warm());
    c.met(1, gone), c.met(2, gone);   // (the draws take two warmed ones)
    CHECK(c.warm_due() && c.next_warm() == kKeptMax + 1 && c.next_warm() == kKeptMax + 2 && !c.next_warm());
    CHECK(!c.is_cold(kKeptMax + 1) && c.warmed() == kKeptMax);
    c.met(800, gone);   // (one taken cold: no room for a warm)
    CHECK(!c.warm_due() && c.cold.size() == 1000 - kKeptMax - 3);
    CHECK(c.cold_at.size() == c.cold.size());   // ((Triforce) the cold by place: the same ones)
    // ((Triforce) the RAM short while one chosen cold is being read: only the kept's cold marks go; that one is kept cold once
    // read and warmed when the window reaches it (not kept as warmed with no pipelines made)
    Ahead r;
    r.load(keys_from(1, 1000));
    for (int i = 0; i < 600; i++) r.read_done(r.choose([](uint64_t) { return true; }), true);
    const uint64_t reading = r.choose([](uint64_t) { return true; });
    CHECK(reading == 601 && r.is_cold(reading));
    r.let_go_all(gone);
    CHECK(r.kept.empty() && r.is_cold(reading) && r.cold.size() == 1 && r.cold_at.size() == 1);
    r.read_done(reading, true);
    CHECK(r.warmed() == 0 && r.warm_due() && r.next_warm() == reading && !r.is_cold(reading) && r.warmed() == 1);
    // (the rules before: an ask's read is the shelf's)
    Ahead b;
    b.keep_all = false;
    b.load(keys_from(1, 10));
    CHECK(!b.keep(3) && b.max_kept() == kKeptMax);
}

/// ((Triforce) Passed by, out of the warm's window: an order recorded in HDR replayed in SDR, whose first 300 programs this
/// session never draws. The whole order read (the first kKeptMax warmed, the rest cold), the draws take the rest in order, the
/// workers warming whenever next_warm gives one. Kept until needed, the 300 stay kept; but once the draws are kBehind past them
/// they no longer hold the window, which refills: kKeptMax warmed ahead of the draws, not 212 for the session
static void passed_by() {
    Ahead a;
    a.load(keys_from(1, 2000));
    while (const uint64_t k = a.choose([](uint64_t) { return true; })) a.read_done(k, true);
    CHECK(a.kept.size() == 2000 && a.warmed() == kKeptMax && a.cold.size() == 2000 - kKeptMax);
    std::vector<uint64_t> gone;
    size_t warms = 0, least = SIZE_MAX;
    for (uint64_t k = 301; k <= 2000; k++) {
        while (a.next_warm()) warms++;
        if (k >= 301 + 2 * kBehind && k <= 2000 - kKeptMax) {   // (the draws past the 300, and the order not near its end)
            size_t ahead = 0;   // (warmed ahead of the draws: kept, not cold, not behind them)
            for (const auto& [key, at] : a.kept) ahead += key >= k && !a.is_cold(key);
            least = std::min(least, ahead);
        }
        a.met(k, gone);
        CHECK(gone.empty() && a.cold_at.size() == a.cold.size());
    }
    std::printf("passed by: the warm's window ahead of the draws %zu at least (of %zu), %zu warmed later\n", least, kKeptMax, warms);
    CHECK(least == kKeptMax);
    CHECK(warms == 2000 - kKeptMax);   // (every cold one warmed before the draws took it)
    // (the 300 still kept, passed: a draw may still take one, which leaves the passed too)
    CHECK(a.kept.size() == 300 && a.passed.size() == 300 && a.warmed() == 0 && !a.warm_due());
    a.met(5, gone);
    CHECK(gone.empty() && !a.kept.count(5) && a.passed.size() == 299);
    a.taken(6);
    CHECK(!a.kept.count(6) && a.passed.size() == 298);
    // (one an ask read behind the draws, kept with the order's: passed at once)
    Ahead b;
    b.load(keys_from(1, 2000));
    for (uint64_t k = 1001; k <= 1000 + kRecent; k++) b.met(k, gone);
    CHECK(b.furthest > 1000 && b.keep(3) && b.passed.count(3) && b.warmed() == 0);
    // (one the order read while the draws went by: passed once read)
    b.next = 9;
    const uint64_t late = b.choose([](uint64_t) { return true; });
    CHECK(late == 10 && !b.passed.count(late));
    b.read_done(late, true);
    CHECK(b.passed.count(late) && b.warmed() == 0);
    // (the RAM short, or the order loaded again: none passed)
    b.let_go_all(gone);
    CHECK(b.passed.empty());
    a.load(keys_from(1, 10));
    CHECK(a.passed.empty() && a.cold.empty() && a.cold_at.empty());
}

/// ((Triforce, wp19) The owner's test37 session, from its numbers: an order of 2411 programs from sessions that took another
/// path (HDR Native with RenoDX, where this one is SDR). The title needs the order's first 200 programs, the workers reading
/// four between two draws; the file select then needs 12 programs the order has far later (another output path's); it stands
/// still while the workers read on (as long as they have something to read); the save's load needs 115 programs at once, every
/// 8th place from 201 on. The rules before drop 64 of the load's 115 as behind where the draws are and pass the other 51 by:
/// test37's frame 8064 ("64 read ahead and let go before, 51 not read ahead yet"). Kept until needed, the load finds all 115
struct Load {
    size_t found = 0, let_go = 0, not_read = 0, kept = 0;
};
static Load test37(bool keep_all) {
    Ahead a;
    a.keep_all = keep_all;
    a.load(keys_from(1, 2411));
    std::unordered_set<uint64_t> taken, let_go;
    const auto wanted = [&](uint64_t k) { return !taken.count(k); };
    const auto need = [&](uint64_t k) {   // (a draw's first need: taken if kept)
        std::vector<uint64_t> gone;
        a.met(k, gone);
        let_go.insert(gone.begin(), gone.end());
        taken.insert(k);
    };
    std::vector<uint64_t> menus = keys_from(1, 200), file_select = keys_from(1501, 12);
    menus.insert(menus.end(), file_select.begin(), file_select.end());
    for (const uint64_t k : menus) {
        for (int i = 0; i < 4; i++)
            if (const uint64_t c = a.choose(wanted)) a.read_done(c, true);
        need(k);
    }
    for (int i = 0; i < 5000; i++) {   // (the file select stands still: the workers read while nothing is asked)
        const uint64_t c = a.choose(wanted);
        if (!c) break;
        a.read_done(c, true);
    }
    Load l;
    for (uint64_t k = 201; k < 201 + 8 * 115; k += 8) {
        if (a.kept.count(k)) l.found++;
        else (let_go.count(k) ? l.let_go : l.not_read)++;
        need(k);
    }
    l.kept = a.kept.size();
    return l;
}

int main() {
    CHECK(on(nullptr) && on("1") && !on("0") && on("00"));
    file();
    merged();
    record();
    ahead();
    session(false);
    session(true);
    kept_until_needed();
    passed_by();
    // ((Triforce, wp19) test37's session: the rules before drop what that frame read itself; kept until needed, nothing)
    const Load before = test37(false), now = test37(true);
    std::printf("test37's load of 115 programs: the rules before found %zu, let go %zu, left %zu unread; kept until needed found %zu "
                "(%zu kept after it)\n", before.found, before.let_go, before.not_read, now.found, now.kept);
    CHECK(before.found + before.let_go + before.not_read == 115 && before.let_go == 64 && before.not_read == 51);
    CHECK(now.found == 115 && now.let_go == 0 && now.not_read == 0);
    CHECK(now.kept <= kKeepAllMax && now.kept == 2411 - 212 - 115);   // (the whole order kept but what the draws took)
    std::puts("PASS: the sessions' order: file, merge, record, the read-ahead in it, kept until needed");
    return 0;
}
