// tests/test_import_ahead.py: gpu/vk/import_ahead.h, the ranges of the game's memory imported ahead of their first binding
#include "../gpu/vk/import_ahead.h"

#include <cstdio>
#include <string>
#include <vector>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

namespace ia = mkwvk::import_ahead;
using R = ia::Range;
struct Pool { uint64_t lo, hi; };

int main() {
    const uint64_t C = 16ull << 20;
    // the switch: NVIDIA's driver unless set
    CHECK("unset: NVIDIA (on again after the race fix's A/B)", ia::on(nullptr, 0x10DE) && !ia::on(nullptr, 0x1002) && !ia::on(nullptr, 0x8086));
    CHECK("1: any", ia::on("1", 0x1002) && ia::on("1", 0x8086));
    CHECK("0: none", !ia::on("0", 0x10DE));
    CHECK("anything else: as unset", ia::on("yes", 0x10DE) && !ia::on("yes", 0x1002));

    // the file: ranges ("lo hi"), and chunks alone as the first version wrote them; what is not one is left out; none inside
    // another, in order
    const auto list = ia::parse("1063000000\n1059000000 105d000000\r\nzzz\n105b000000\n7fb0000000 7fb4000000\n7fb0000010 7fb4000000\n"
                                "7fc0000000 7fb0000000\n\n0\n", C);
    CHECK("parsed", list == std::vector<R>({{0x1059000000, 0x105d000000}, {0x1063000000, 0x1064000000}, {0x7fb0000000, 0x7fb4000000}}));
    CHECK("no chunk size: nothing", ia::parse("1059000000\n", 0).empty() && ia::parse(nullptr, C).empty());
    CHECK("not over 1 GB", ia::parse("1000000000 1080000000\n", C).empty());

    // a note: one inside a kept one changes nothing; one over kept ones takes their place
    std::vector<R> l = list;
    CHECK("inside: nothing", !ia::add(l, 0x105a000000, 0x105c000000) && l.size() == 3);
    CHECK("the same: nothing", !ia::add(l, 0x7fb0000000, 0x7fb4000000) && l.size() == 3);
    CHECK("over two: one", ia::add(l, 0x1059000000, 0x1064000000) &&
                               l == std::vector<R>({{0x1059000000, 0x1064000000}, {0x7fb0000000, 0x7fb4000000}}));
    CHECK("a new one in its place", ia::add(l, 0x7fbc000000, 0x7fc0000000) && l.size() == 3 && l[2] == (R{0x7fbc000000, 0x7fc0000000}));
    CHECK("empty: nothing", !ia::add(l, 0x2000000000, 0x2000000000));
    std::vector<R> full;
    for (uint64_t i = 0; i < ia::kMax; i++) ia::add(full, (0x2000 + 2 * i) * C, (0x2001 + 2 * i) * C);
    CHECK("at most kMax", full.size() == ia::kMax && !ia::add(full, 0x100 * C, 0x101 * C));
    CHECK("full, but over kept ones: in their place", ia::add(full, 0x2000 * C, 0x2004 * C) && full.size() == ia::kMax - 1);

    // the file's text, read back
    const std::vector<char> t = ia::text(l);
    const std::string s(t.begin(), t.end());
    CHECK("written", s == "1059000000 1064000000\n7fb0000000 7fb4000000\n7fbc000000 7fc0000000\n");
    CHECK("read back", ia::parse(s.c_str(), C) == l);

    // the ranges the game's pools reach now, not imported, a few at a time
    const std::vector<Pool> pools = {{0x1058800000, 0x1059400000}, {0x7fb3000000, 0x7fb3100000}};
    auto wanted = [&](const R& r) { return r.lo != 0x7fbc000000; };
    auto due = ia::due(l, pools, 8, wanted);
    CHECK("reached", due == std::vector<R>({{0x1059000000, 0x1064000000}, {0x7fb0000000, 0x7fb4000000}}));
    CHECK("a pool ending at the range's start does not reach it",
          ia::due(std::vector<R>{{0x1059400000, 0x105a000000}}, pools, 8, wanted).empty());
    CHECK("a pool starting at its end does not either", ia::due(std::vector<R>{{0x7fb2000000, 0x7fb3000000}}, pools, 8, wanted).empty());
    CHECK("n at most", ia::due(l, pools, 1, wanted).size() == 1);
    CHECK("not wanted: none", ia::due(l, pools, 8, [](const R&) { return false; }).empty());
    std::puts("import ahead: ok");
    return 0;
}
