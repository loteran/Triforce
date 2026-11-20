// tests/test_tex_bc1.py compiles and runs this: gpu/vk/tex_bc1.h and gpu/vk/astc_opaque.h, no game and no GPU.
// With TEX_BC1_CPU_DECODER it is linked with the repository's CPU ASTC decoder and checks astc_opaque against it too.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include "astc_opaque.h"
#include "tex_bc1.h"

#ifdef TEX_BC1_CPU_DECODER
namespace Tegra::Texture::ASTC {
void DecompressSerial(std::span<const uint8_t> data, uint32_t width, uint32_t height, uint32_t depth, uint32_t block_width,
                      uint32_t block_height, std::span<uint8_t> output);
}
#endif

static int failures;
#define CHECK(x)                                                        \
    do {                                                                \
        if (!(x)) {                                                     \
            std::printf("FAIL:%d %s\n", __LINE__, #x);                  \
            failures++;                                                 \
        }                                                               \
    } while (0)

// ---- BC1 and BC3's colour half, decoded as the Khronos Data Format Specification says (the same expansion of 5:6:5
// and the same thirds in both: what differs between the two is only which colours a block's eight bytes select) ----
struct Rgba {
    uint8_t r, g, b, a;
    bool operator==(const Rgba& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};
static Rgba expand565(uint16_t c) {
    const unsigned r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    return {(uint8_t)(r << 3 | r >> 2), (uint8_t)(g << 2 | g >> 4), (uint8_t)(b << 3 | b >> 2), 255};
}
static Rgba mix(Rgba a, Rgba b, unsigned wa, unsigned wb) {   // (wa a + wb b) / (wa + wb), rounded to nearest
    const unsigned d = wa + wb;
    return {(uint8_t)((wa * a.r + wb * b.r + d / 2) / d), (uint8_t)((wa * a.g + wb * b.g + d / 2) / d),
            (uint8_t)((wa * a.b + wb * b.b + d / 2) / d), 255};
}
/// The colour half of a BC3 block (or a BC2 block): always four colours. Alpha comes from the other half
static void bc3_colour(const uint8_t b[8], Rgba out[16]) {
    const uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    const Rgba p[4] = {expand565(c0), expand565(c1), mix(expand565(c0), expand565(c1), 2, 1), mix(expand565(c0), expand565(c1), 1, 2)};
    const uint32_t idx = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) out[i] = p[(idx >> (2 * i)) & 3];
}
/// A BC1 block. rgba: the RGBA format, whose three-colour mode's index 3 is transparent; the RGB format gives alpha 255
/// always and black there
static void bc1(const uint8_t b[8], bool rgba, Rgba out[16]) {
    const uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    Rgba p[4] = {expand565(c0), expand565(c1), {}, {}};
    if (c0 > c1) {
        p[2] = mix(p[0], p[1], 2, 1), p[3] = mix(p[0], p[1], 1, 2);
    } else {
        p[2] = mix(p[0], p[1], 1, 1);
        p[3] = {0, 0, 0, (uint8_t)(rgba ? 0 : 255)};
    }
    const uint32_t idx = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) out[i] = p[(idx >> (2 * i)) & 3];
}
/// BC3's alpha half, decoded
static void bc3_alpha(const uint8_t b[8], uint8_t out[16]) {
    unsigned a[8] = {b[0], b[1]};
    if (a[0] > a[1])
        for (unsigned i = 1; i < 7; i++) a[1 + i] = ((7 - i) * a[0] + i * a[1] + 3) / 7;
    else {
        for (unsigned i = 1; i < 5; i++) a[1 + i] = ((5 - i) * a[0] + i * a[1] + 2) / 5;
        a[6] = 0, a[7] = 255;
    }
    uint64_t idx = 0;
    for (int i = 0; i < 6; i++) idx |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)a[(idx >> (3 * i)) & 7];
}
static void put(uint8_t b[8], uint16_t c0, uint16_t c1, uint32_t idx) {
    b[0] = (uint8_t)c0, b[1] = (uint8_t)(c0 >> 8), b[2] = (uint8_t)c1, b[3] = (uint8_t)(c1 >> 8);
    b[4] = (uint8_t)idx, b[5] = (uint8_t)(idx >> 8), b[6] = (uint8_t)(idx >> 16), b[7] = (uint8_t)(idx >> 24);
}
/// Whether the eight bytes are the same sixteen texels as a BC3 colour half (alpha 255) and as a BC1 block of either format
static bool same_picture(uint16_t c0, uint16_t c1, uint32_t idx) {
    uint8_t b[8];
    put(b, c0, c1, idx);
    Rgba three[16], rgb[16], rgba[16];
    bc3_colour(b, three), bc1(b, false, rgb), bc1(b, true, rgba);
    for (int i = 0; i < 16; i++)
        if (!(three[i] == rgb[i]) || !(three[i] == rgba[i])) return false;
    return true;
}
/// The end of the encoder's EncodeColor(texels, false), from its SPIR-V (tests/test_tex_bc1.py checks the header is the one
/// read): the two packed colours ordered c0 >= c1, and every index 0 when they are equal
static void encoder_order(uint16_t& q0, uint16_t& q1, uint32_t& idx) {
    if (q0 < q1) {
        const uint16_t t = q0;
        q0 = q1, q1 = t;
    }
    if (q0 == q1) idx = 0;
}

static uint32_t rng_state = 0x1234567u;
static uint32_t rng() {
    rng_state ^= rng_state << 13, rng_state ^= rng_state >> 17, rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_colour_half() {
    // authored: four colours in order; the order BC1 reads as three colours; equal colours with and without index 3
    CHECK(tex_bc1::colour_half_same(0xF800, 0x001F, 0xE4E4E4E4u) && same_picture(0xF800, 0x001F, 0xE4E4E4E4u));
    CHECK(!tex_bc1::colour_half_same(0x001F, 0xF800, 0xE4E4E4E4u) && !same_picture(0x001F, 0xF800, 0xE4E4E4E4u));
    CHECK(!same_picture(0x001F, 0xF800, 0xAAAAAAAAu));   // (index 2 alone: a mean, not a third)
    CHECK(!same_picture(0x001F, 0xF800, 0xFFFFFFFFu));   // (index 3 alone: black, not a third)
    CHECK(tex_bc1::colour_half_same(0x8410, 0x8410, 0) && same_picture(0x8410, 0x8410, 0));
    CHECK(tex_bc1::colour_half_same(0x8410, 0x8410, 0xAAAAAAAAu) && same_picture(0x8410, 0x8410, 0xAAAAAAAAu));
    CHECK(!tex_bc1::colour_half_same(0x8410, 0x8410, 0x000000C0u) && !same_picture(0x8410, 0x8410, 0x000000C0u));
    CHECK(tex_bc1::colour_half_same(1, 0, 0xFFFFFFFFu) && same_picture(1, 0, 0xFFFFFFFFu));
    CHECK(tex_bc1::colour_half_same(0xFFFF, 0, 0x1B1B1B1Bu) && same_picture(0xFFFF, 0, 0x1B1B1B1Bu));
    // every block the rule passes is the same picture: 400,000 at random, and every pair of colours a step apart
    for (int i = 0; i < 400000; i++) {
        const uint16_t c0 = (uint16_t)rng(), c1 = (uint16_t)(i % 5 == 0 ? c0 : rng());
        const uint32_t idx = i % 7 == 0 ? 0 : rng();
        if (tex_bc1::colour_half_same(c0, c1, idx)) CHECK(same_picture(c0, c1, idx));
    }
    for (uint32_t c = 0; c < 0xFFFF; c++) {
        CHECK(tex_bc1::colour_half_same((uint16_t)(c + 1), (uint16_t)c, 0xE4E4E4E4u) && same_picture((uint16_t)(c + 1), (uint16_t)c, 0xE4E4E4E4u));
        CHECK(!tex_bc1::colour_half_same((uint16_t)c, (uint16_t)(c + 1), 0xE4E4E4E4u));
    }
    // what the encoder writes, for any two colours and any indices it chose: passes the rule, and is the same picture
    for (int i = 0; i < 400000; i++) {
        uint16_t q0 = (uint16_t)rng(), q1 = (uint16_t)(i % 4 == 0 ? q0 : rng());
        uint32_t idx = rng();
        encoder_order(q0, q1, idx);
        CHECK(tex_bc1::colour_half_same(q0, q1, idx) && same_picture(q0, q1, idx));
    }
    // an opaque block's alpha half as the encoder's EncodeAlpha writes it (both ends 255, the indices 0): 255 in every
    // texel, what the BC1 RGB format gives
    const uint8_t alpha[8] = {255, 255, 0, 0, 0, 0, 0, 0};
    uint8_t a[16];
    bc3_alpha(alpha, a);
    for (int i = 0; i < 16; i++) CHECK(a[i] == 255);
    uint8_t b[8];
    put(b, 0xF800, 0x001F, 0x1B1B1B1Bu);
    Rgba rgb[16];
    bc1(b, false, rgb);
    for (int i = 0; i < 16; i++) CHECK(rgb[i].a == 255);
}

static void test_policy() {
    CHECK(!tex_bc1::wanted(nullptr, 0) && !tex_bc1::wanted("", 0));     // (off unless set: as before the row)
    CHECK(tex_bc1::wanted(nullptr, 1) && tex_bc1::wanted("", 1));       // (the row)
    CHECK(!tex_bc1::wanted(nullptr, 2) && !tex_bc1::wanted(nullptr, -1));
    CHECK(tex_bc1::wanted("1", 0) && !tex_bc1::wanted("0", 1));         // (the switch over the row, both ways)
    // the image's bytes: half of BC3's sixteen a block, level by level and layer by layer
    CHECK(tex_bc1::bytes(4, 4, 1, 1) == 8 && tex_bc1::bytes(5, 4, 1, 1) == 16 && tex_bc1::bytes(1, 1, 1, 1) == 8);
    CHECK(tex_bc1::bytes(8, 8, 4, 1) == (4 + 1 + 1 + 1) * 8);
    CHECK(tex_bc1::bytes(1024, 1024, 1, 90) == 1024ull * 1024 / 2 * 90);
    CHECK(tex_bc1::bytes(1365, 1365, 3, 2) == (342ull * 342 + 171ull * 171 + 86ull * 86) * 8 * 2);
    // the formats: BC1's RGB pair for a BC1 image, BC3's for the others, sRGB for the game's sRGB format
    CHECK(tex_bc1::image_format(true) == 131 && tex_bc1::image_format(false) == 137);
    CHECK(tex_bc1::view_format(true, false) == 131 && tex_bc1::view_format(true, true) == 132);
    CHECK(tex_bc1::view_format(false, false) == 137 && tex_bc1::view_format(false, true) == 138);
    CHECK(tex_bc1::block_copy_ok(true, true) && tex_bc1::block_copy_ok(false, false));
    CHECK(!tex_bc1::block_copy_ok(true, false) && !tex_bc1::block_copy_ok(false, true));
}

// ---- ASTC blocks, authored bit by bit ----
struct Block {
    uint8_t b[16] = {};
    Block& set(unsigned at, unsigned n, uint32_t v) {
        for (unsigned i = 0; i < n; i++) {
            const unsigned bit = at + i;
            b[bit / 8] = (uint8_t)((b[bit / 8] & ~(1u << (bit % 8))) | (((v >> i) & 1u) << (bit % 8)));
        }
        return *this;
    }
};
/// One partition, a 4 x 4 weight grid of 2-bit weights (32 bits of weights), one plane: block mode 0x42
static Block plain(unsigned cem) {
    Block k;
    k.set(0, 11, 0x42).set(11, 2, 0).set(13, 4, cem);
    for (unsigned i = 17; i < 128; i++) k.set(i, 1, (i * 7 + cem) % 3 == 0);
    return k;
}
static bool is_opaque_mode(unsigned cem) { return cem == 0 || cem == 1 || cem == 6 || cem == 8 || cem == 9; }

static void test_astc_authored() {
    CHECK(astc_opaque::bits(plain(9).b, 0, 11) == 0x42 && astc_opaque::bits(plain(9).b, 13, 4) == 9);
    CHECK(astc_opaque::weight_bits(16, 2) == 32 && astc_opaque::weight_bits(64, 0) == 64 && astc_opaque::weight_bits(64, 2) == 128);
    CHECK(astc_opaque::weight_bits(5, 1) == 8 && astc_opaque::weight_bits(3, 3) == 7 && astc_opaque::weight_bits(10, 4) == 26);
    // the endpoint modes: without alpha and low dynamic range, yes; with alpha or HDR, no
    for (unsigned cem = 0; cem < 16; cem++)
        for (unsigned bw = 4; bw <= 12; bw += 2) CHECK(astc_opaque::block(plain(cem).b, bw, bw) == is_opaque_mode(cem));
    CHECK(!astc_opaque::block(plain(8).b, 3, 4) && !astc_opaque::block(plain(8).b, 4, 13));   // (no such block size)
    // the decoder's errors, one by one, on a block that is opaque but for them
    Block z;
    CHECK(!astc_opaque::block(z.b, 8, 8));                                // (all zero: a reserved block mode)
    CHECK(!astc_opaque::block(plain(8).set(0, 4, 0).b, 8, 8));            // (bits 0 to 3 zero)
    CHECK(!astc_opaque::block(plain(8).set(0, 11, 0x1C4).b, 8, 8));       // (bits 0 and 1 zero with bits 6 to 8 set)
    CHECK(!astc_opaque::block(plain(8).set(7, 2, 3).b, 6, 6));            // (a weight grid 7 wide in a block of 6)
    CHECK(astc_opaque::block(plain(8).set(7, 2, 3).b, 8, 8));             // (...and in a block of 8: 7 x 4 weights, 56 bits)
    CHECK(astc_opaque::block(plain(8).set(4, 1, 1).b, 8, 8));             // (5 levels: 16 weights in 38 bits, still fine)
    CHECK(!astc_opaque::block(plain(8).set(0, 2, 1).b, 8, 8));            // (2 levels: 16 bits of weights, under 24)
    CHECK(astc_opaque::block(plain(8).set(0, 11, 0x1D2).b, 12, 12));      // (7 x 4 weights of 5 levels: 66 bits)
    CHECK(!astc_opaque::block(plain(8).set(0, 11, 0x77).b, 12, 12));      // (8 x 5 weights of 3 bits: 120 bits, over 96)
    CHECK(astc_opaque::block(plain(8).set(0, 11, 0x66).b, 12, 12));       // (8 x 5 weights of 2 bits: 80 bits)
    CHECK(!astc_opaque::block(plain(8).set(0, 11, 0x466).b, 12, 12));     // (two planes of 8 x 5: 80 weights, over 64)
    CHECK(!astc_opaque::block(plain(8).set(10, 1, 1).set(11, 2, 3).b, 8, 8));   // (two planes and four partitions)
    // several partitions: one shared mode (bits 23 and 24 zero, the mode in bits 25 to 28), or a mode each (not looked for)
    for (unsigned cem = 0; cem < 16; cem++)
        for (unsigned parts = 1; parts < 4; parts++) {
            Block k = plain(15);
            k.set(11, 2, parts).set(23, 2, 0).set(25, 4, cem);
            CHECK(astc_opaque::block(k.b, 8, 8) == is_opaque_mode(cem));
            k.set(23, 2, 1 + cem % 3);
            CHECK(!astc_opaque::block(k.b, 8, 8));
        }
    // a void extent: the constant's alpha, and the decoder's two errors
    Block v;
    v.set(0, 9, 0x1FC).set(9, 1, 0).set(10, 2, 3);
    for (unsigned i = 12; i < 64; i++) v.set(i, 1, 1);
    v.set(64, 16, 0x1234).set(80, 16, 0x8000).set(96, 16, 0xFFFF).set(112, 16, 0xFFFF);
    CHECK(astc_opaque::block(v.b, 8, 8) && astc_opaque::block(v.b, 4, 4));
    { Block k = v; CHECK(!astc_opaque::block(k.set(112, 16, 0xFFFE).b, 8, 8)); }   // (alpha one step under)
    { Block k = v; CHECK(!astc_opaque::block(k.set(112, 16, 0x00FF).b, 8, 8)); }
    { Block k = v; CHECK(!astc_opaque::block(k.set(9, 1, 1).b, 8, 8)); }           // (the HDR kind)
    { Block k = v; CHECK(!astc_opaque::block(k.set(10, 1, 0).b, 8, 8)); }          // (bits 10 and 11 not both set)
    { Block k = v; CHECK(!astc_opaque::block(k.set(11, 1, 0).b, 8, 8)); }
    { Block k = v; CHECK(astc_opaque::block(k.set(12, 26, 0).set(38, 26, 0).set(12, 13, 5).b, 8, 8)); }   // (its coordinates are not looked at)
    // every block of a run, and one bad block among them
    std::vector<uint8_t> run;
    for (int i = 0; i < 100; i++) {
        const Block k = plain(i % 2 ? 8 : 0);
        run.insert(run.end(), k.b, k.b + 16);
    }
    CHECK(astc_opaque::all(run.data(), 100, 8, 8) && astc_opaque::all(run.data(), 0, 8, 8));
    std::memcpy(run.data() + 16 * 63, plain(12).b, 16);
    CHECK(!astc_opaque::all(run.data(), 100, 8, 8) && astc_opaque::all(run.data(), 63, 8, 8));
}

#ifdef TEX_BC1_CPU_DECODER
/// Against the repository's CPU ASTC decoder (suyu's, of the same family as the GPU one): a block astc_opaque passes
/// decodes with alpha 255 in every texel, over blocks made at random around legal ones, for every block size the game uses
static void test_astc_cpu_decoder() {
    static const unsigned sizes[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8},
                                        {10, 10}, {12, 10}, {12, 12}};
    long passed = 0, with_alpha = 0, total = 0;
    std::vector<uint8_t> out(12 * 12 * 4);
    for (const auto& sz : sizes)
        for (int i = 0; i < 60000; i++, total++) {
            Block k;
            for (int j = 0; j < 16; j++) k.b[j] = (uint8_t)rng();
            if (i % 3) k.set(0, 11, 0x42 | (rng() & 0x7B0));           // (mostly modes near the legal one above)
            if (i % 5 == 0) k.set(0, 13, 0x1FC | (rng() & 0x1E00));    // (void extents)
            if (i % 2) k.set(11, 2, 0);
            const bool opaque = astc_opaque::block(k.b, sz[0], sz[1]);
            std::memset(out.data(), 0x5A, out.size());
            Tegra::Texture::ASTC::DecompressSerial(std::span<const uint8_t>(k.b, 16), sz[0], sz[1], 1, sz[0], sz[1],
                                                   std::span<uint8_t>(out.data(), (size_t)sz[0] * sz[1] * 4));
            bool all255 = true;
            for (unsigned t = 0; t < sz[0] * sz[1]; t++) all255 = all255 && out[t * 4 + 3] == 255;
            passed += opaque, with_alpha += !all255;
            if (opaque && !all255) {
                std::printf("FAIL: a block astc_opaque passes decodes with alpha under 255 (%ux%u):", sz[0], sz[1]);
                for (int j = 0; j < 16; j++) std::printf(" %02x", k.b[j]);
                std::printf("\n");
                failures++;
                if (failures > 20) return;
            }
        }
    // (the run means something only when both kinds of block came up in number)
    CHECK(passed > total / 50 && with_alpha > total / 50);
    std::printf("astc_opaque against the CPU decoder: %ld blocks, %ld passed as opaque, %ld decode with alpha\n", total, passed, with_alpha);
}
#endif

int main() {
    test_colour_half();
    test_policy();
    test_astc_authored();
#ifdef TEX_BC1_CPU_DECODER
    test_astc_cpu_decoder();
#endif
    if (failures) return 1;
    std::puts("tex_bc1: all checks passed");
    return 0;
}
