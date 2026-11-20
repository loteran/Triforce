// tests/test_program_readahead.py: gpu/vk/program_readahead.h, which programs the worker reads ahead of their first draw
#include "../gpu/vk/program_readahead.h"

#include <cstdio>
#include <deque>
#include <set>
#include <vector>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

namespace ra = mkwvk::program_readahead;

int main() {
    // the switch
    CHECK("unset", ra::count(nullptr) == ra::kDefault && ra::count("") == ra::kDefault);
    CHECK("0: none", ra::count("0") == 0);
    CHECK("a number", ra::count("8") == 8);
    CHECK("at most 256", ra::count("100000") == 256 && ra::count("-3") == 0);
    CHECK("no number", ra::count("lots") == ra::kDefault && ra::count("8x") == ra::kDefault);

    // the records after an offset, in the file's order, those still wanted
    const std::vector<std::pair<int64_t, uint64_t>> order = {{16, 0xa}, {400, 0xb}, {900, 0xc}, {1500, 0xd}, {2000, 0xe}, {2600, 0xf}};
    std::set<uint64_t> gone = {0xd};   // (read already, or read ahead)
    auto wanted = [&](uint64_t k) { return !gone.count(k); };
    auto got = ra::next_keys(order, 400, 3, wanted);
    CHECK("after b: c, e, f (d gone)", got == std::vector<uint64_t>({0xc, 0xe, 0xf}));
    CHECK("not the record itself", ra::next_keys(order, 16, 1, wanted) == std::vector<uint64_t>({0xb}));
    CHECK("from before the first", ra::next_keys(order, 0, 2, wanted) == std::vector<uint64_t>({0xa, 0xb}));
    CHECK("past the last: none", ra::next_keys(order, 2600, 5, wanted).empty());
    CHECK("n of them at most", ra::next_keys(order, 16, 2, wanted).size() == 2);
    CHECK("none wanted", ra::next_keys(order, 16, 9, [](uint64_t) { return false; }).empty());
    CHECK("between two offsets", ra::next_keys(order, 1000, 9, wanted) == std::vector<uint64_t>({0xe, 0xf}));

    // the asks: the newest kAsksMax, each place once
    std::deque<int64_t> asks;
    for (int64_t at : {10, 20, 20, 30, 40, 50}) ra::ask(asks, at);
    CHECK("the newest four", asks == std::deque<int64_t>({20, 30, 40, 50}));
    ra::ask(asks, 40);
    CHECK("one asked for again is not added", asks.size() == 4 && asks.back() == 50);

    // ((Triforce, test16) the notes' order: each program once, at its first note; a load's programs after the one a draw read)
    CHECK("the switch", ra::notes_on(nullptr) && ra::notes_on("1") && !ra::notes_on("0"));
    // (notes in the order sessions first built them: the menus' programs, then an area's, with programs met again)
    const std::vector<uint64_t> notes = {0x10, 0x11, 0x10, 0x12, 0xa0, 0xa1, 0x11, 0xa2, 0xa0, 0xa3, 0, 0xa4};
    const auto no = ra::notes_order(notes);
    const std::vector<std::pair<int64_t, uint64_t>> want_order = {{0, 0x10}, {1, 0x11}, {3, 0x12}, {4, 0xa0}, {5, 0xa1}, {7, 0xa2},
                                                                  {9, 0xa3}, {11, 0xa4}};
    CHECK("each program at its first note", no == want_order);
    const auto idx = ra::by_key(no);
    CHECK("a program's index", ra::index_of(idx, 0xa0) == 4 && ra::index_of(idx, 0x11) == 1 && ra::index_of(idx, 0x12) == 3);
    CHECK("one no note names", ra::index_of(idx, 0xbeef) == -1 && ra::index_of(std::vector<std::pair<uint64_t, int64_t>>{}, 1) == -1);
    std::set<uint64_t> taken2 = {0xa1};   // (read already)
    CHECK("after the area's first program: the area's next ones",
          ra::next_keys(no, ra::index_of(idx, 0xa0), 3, [&](uint64_t k) { return !taken2.count(k); }) ==
              std::vector<uint64_t>({0xa2, 0xa3, 0xa4}));
    // (the shelf: how many of its oldest go to take more)
    CHECK("room: none go", ra::to_let_go(10, 1, 512) == 0 && ra::to_let_go(511, 1, 512) == 0);
    CHECK("full: the oldest go", ra::to_let_go(512, 1, 512) == 1 && ra::to_let_go(512, 4, 512) == 4 && ra::to_let_go(600, 1, 512) == 89);
    CHECK("workers", ra::kWorkers >= 2);
    std::puts("program read-ahead: ok");
    return 0;
}
