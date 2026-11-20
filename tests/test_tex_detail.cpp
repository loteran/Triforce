// tex_detail (gpu/vk/tex_detail.h): the detail level a card gets and the levels a texture loses at it.
// tests/test_tex_detail.py
#include "../gpu/vk/tex_detail.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

int main() {
    const uint64_t MB = 1ull << 20;
    using namespace tex_detail;
    // ---- the allowances in players' logs
    expect(by_vram(11311 * MB, false) == kFull, "a 12 GB card (11,311 MB allowed): full");
    expect(by_vram(11247 * MB, false) == kFull, "the same card with something else open");
    expect(by_vram(15569 * MB, false) == kFull, "a 16 GB card");
    expect(by_vram(31419 * MB, false) == kFull, "a 32 GB card");
    expect(by_vram(9300 * MB, false) == kReduced, "a 10 GB card");
    expect(by_vram(7365 * MB, false) == kReduced, "an 8 GB RX 7600");
    expect(by_vram(7133 * MB, false) == kReduced, "an 8 GB RX 7700S");
    expect(by_vram(4730 * MB, false) == kLow, "a 6 GB RTX 3060 Laptop");
    expect(by_vram(3379 * MB, false) == kLow, "a 4 GB laptop card");
    // ---- the two lines
    expect(by_vram(10240 * MB, false) == kFull, "full starts at 10,240 MB");
    expect(by_vram(10239 * MB, false) == kReduced, "one MB under it");
    expect(by_vram(6144 * MB, false) == kReduced, "reduced starts at 6,144 MB");
    expect(by_vram(6143 * MB, false) == kLow, "one MB under it");
    // ---- no allowance reported: whole, as before
    expect(by_vram(0, false) == kFull, "nothing reported");
    expect(by_vram(0, true) == kFull, "nothing reported, integrated");
    // ---- an integrated GPU never gets full, and follows the allowance below that
    expect(by_vram(10361 * MB, true) == kReduced, "a handheld that reported 10,361 MB");
    expect(by_vram(16000 * MB, true) == kReduced, "integrated with 16 GB allowed");
    expect(by_vram(8034 * MB, true) == kReduced, "an Iris Xe (8,034 MB)");
    expect(by_vram(5000 * MB, true) == kLow, "integrated with 5 GB allowed");

    // ---- the switch
    expect(parse(nullptr) == -1 && parse("auto") == -1 && parse("") == -1 && parse("1") == -1, "auto, unset, anything else");
    expect(parse("full") == kFull && parse("reduced") == kReduced && parse("low") == kLow, "the three names");

    // ---- the Texture detail row: Automatic, then the same three levels; the switch over it, the card under it
    expect(from_row(0) == -1 && from_row(1) == kFull && from_row(2) == kReduced && from_row(3) == kLow, "the row's four values");
    expect(from_row(-1) == -1 && from_row(4) == -1 && from_row(99) == -1, "a value past the row: Automatic");
    for (int card = kFull; card <= kLow; card++) {
        expect(choose(-1, from_row(0), card) == card, "Automatic, no switch: the card's own level");
        for (int row = 1; row <= 3; row++) {
            expect(choose(-1, from_row(row), card) == row - 1, "the row over the card's own");
            for (int asked = kFull; asked <= kLow; asked++)
                expect(choose(asked, from_row(row), card) == asked, "the switch over the row");
        }
        for (int asked = kFull; asked <= kLow; asked++)
            expect(choose(asked, from_row(0), card) == asked, "the switch over Automatic");
    }
    expect(choose(parse("auto"), from_row(2), kFull) == kReduced, "RX_VK_TEX_DETAIL=auto leaves the row");

    // ---- what a texture loses
    expect(skip(kFull, 2048, 2048, 12) == 0, "full: nothing");
    expect(skip(kReduced, 2048, 2048, 12) == 1, "reduced: the largest level");
    expect(skip(kLow, 2048, 2048, 12) == 2, "low: the two largest of a 2048");
    expect(skip(kLow, 1024, 1024, 11) == 2, "low: two of a 1024");
    expect(skip(kLow, 1023, 1024, 10) == 1, "low: one where a side is under 1024");
    expect(skip(kLow, 512, 512, 10) == 1, "low: one of a 512");
    expect(skip(kLow, 1024, 1024, 3) == 1, "low: one where two would leave a single level");
    expect(skip(kReduced, 256, 256, 9) == 1, "reduced: a 256 still loses one");
    expect(skip(kReduced, 255, 512, 9) == 0 && skip(kLow, 512, 128, 9) == 0, "a side under 256: whole");
    expect(skip(kReduced, 1024, 1024, 1) == 0 && skip(kLow, 2048, 2048, 1) == 0, "no mip levels: whole");
    expect(skip(kReduced, 1365, 1365, 11) == 1, "a terrain material array's 1365");
    if (failures) return EXIT_FAILURE;
    std::printf("tex detail: all checks passed\n");
    return EXIT_SUCCESS;
}
