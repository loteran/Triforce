// tests/test_layer_split.py: gpu/vk/layer_split.h, a new layered ASTC texture's first upload in parts
#include "../gpu/vk/layer_split.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

namespace ls = mkwvk::layer_split;

int main() {
    // the switch
    CHECK("unset: 128 MB", ls::budget(nullptr) == (128ull << 20) && ls::budget("") == (128ull << 20));
    CHECK("0: off", ls::budget("0") == 0);
    CHECK("a number", ls::budget("64") == (64ull << 20));
    CHECK("no number", ls::budget("lots") == (128ull << 20) && ls::budget("-5") == (128ull << 20));

    // the texture of the owner's test7 log: 1365 x 1365 x 90, 11 levels: some 890 MB of texels, 6 parts of 15 layers
    const uint64_t t = ls::texel_bytes(1365, 1365, 11, 90);
    std::printf("1365x1365 x90, 11 levels: %.0f MB of texels\n", t / 1048576.0);
    CHECK("its texels", t > (850ull << 20) && t < (900ull << 20));
    const uint32_t n = ls::parts(t, 90, ls::budget(nullptr));
    CHECK("six parts at most", n == ls::kMaxParts);
    CHECK("15 layers a part", ls::per_frame(90, n) == 15);
    CHECK("a small one is whole", ls::parts(ls::texel_bytes(512, 512, 10, 8), 8, ls::budget(nullptr)) == 1);
    CHECK("one layer is whole", ls::parts(1ull << 40, 1, ls::budget(nullptr)) == 1);
    CHECK("no budget: whole", ls::parts(t, 90, 0) == 1);
    CHECK("never more parts than layers", ls::parts(1ull << 40, 3, 1) == 3);
    CHECK("two parts", ls::parts(200ull << 20, 10, 128ull << 20) == 2);

    // the parts, whole layers of the storage
    const uint64_t L = 0x10000;
    ls::Ranges part, rest;
    ls::take(ls::Ranges{{0, 90 * L}}, L, 15, part, rest);
    CHECK("the first part", part == ls::Ranges({{0, 15 * L}}) && rest == ls::Ranges({{15 * L, 90 * L}}));
    ls::Ranges remaining = rest;
    int frames = 1;
    while (!rest.empty()) {
        remaining = rest;
        ls::take(remaining, L, 15, part, rest);
        CHECK("each part is 15 layers or the last", part.size() == 1 && (part[0].second - part[0].first == 15 * L || rest.empty()));
        frames++;
    }
    CHECK("six frames in all", frames == 6 && part == ls::Ranges({{75 * L, 90 * L}}));
    ls::take(ls::Ranges{{0, 2 * L}, {5 * L, 9 * L}}, L, 3, part, rest);
    CHECK("across ranges", part == ls::Ranges({{0, 2 * L}, {5 * L, 6 * L}}) && rest == ls::Ranges({{6 * L, 9 * L}}));
    ls::take(ls::Ranges{{0, 4 * L}}, L, 9, part, rest);
    CHECK("the rest in one", part == ls::Ranges({{0, 4 * L}}) && rest.empty());
    ls::take(ls::Ranges{{0, 4 * L}}, 0, 9, part, rest);
    CHECK("no layer size: nothing", part.empty() && rest.empty());
    std::puts("layer split: ok");
    return 0;
}
