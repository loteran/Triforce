/* The mods' cheat files (host/mod_cheats.h): the reader and the interpreter against a stand-in memory, opcode by opcode.
   tests/test_mod_cheats.py compiles and runs it. */
#include "mod_cheats.h"
#include <stdlib.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* the stand-in game: an image of two modules (a loader's code, then main: code and data) and a heap */
#define IB 0x80000000ULL           /* the image */
#define MB 0x80002000ULL           /* main: its code to MB + 0x800, its data after */
#define IMAGE_SIZE 0x4000
#define HB 0x1000000000ULL         /* the heap */
#define HEAP_SIZE 0x2000
static uint8_t image[IMAGE_SIZE], heap[HEAP_SIZE];
static const uint64_t code[2][2] = {{IB, IB + 0x1000}, {MB, MB + 0x800}};
static ModCheatLayout layout = {IB, IB + IMAGE_SIZE, HB, HEAP_SIZE, code, 2};
static int reads_refused, writes_refused;

static uint8_t *at(uint64_t a) { return a >= HB ? heap + (a - HB) : image + (a - IB); }
static int mem_read(void *ctx, uint64_t a, unsigned n, void *out) {
    if (!mod_cheat_readable((const ModCheatLayout *)ctx, a, n)) return reads_refused++, 0;
    memcpy(out, at(a), n);
    return 1;
}
static int mem_write(void *ctx, uint64_t a, unsigned n, const void *in) {
    if (!mod_cheat_writable((const ModCheatLayout *)ctx, a, n)) return writes_refused++, 0;
    memcpy(at(a), in, n);
    return 1;
}
static const ModCheatMem mem = {&layout, mem_read, mem_write, {MB, HB, HB, 0x08000000ULL}};
static ModCheatState st;
static ModCheatRun last;

static uint64_t h(unsigned off, unsigned n) { uint64_t v = 0; memcpy(&v, heap + off, n); return v; }
static void hset(unsigned off, uint64_t v, unsigned n) { memcpy(heap + off, &v, n); }
static void fresh(void) {
    memset(image, 0x11, sizeof image), memset(heap, 0, sizeof heap), memset(&st, 0, sizeof st);
    reads_refused = writes_refused = 0;
}
#define W(...) (const uint32_t[]){__VA_ARGS__}, (int)(sizeof((const uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t))
static int go_keys(uint64_t keys, const uint32_t *w, int n) {
    char why[MOD_CHEAT_WHY] = "";
    if (!mod_cheat_check(w, n, why, sizeof why)) printf("FAIL: a cheat of this test is refused: %s\n", why), fails++;
    return mod_cheat_run(w, n, &st, &mem, keys, &last);
}
static int go(const uint32_t *w, int n) { return go_keys(0, w, n); }

static void store_static(void) {   /* 0TMR00AA AAAAAAAA V */
    fresh();
    CHECK(go(W(0x04100000, 0x00000010, 0x12345678)) == MOD_CHEAT_RAN);
    CHECK(h(0x10, 4) == 0x12345678 && h(0x0C, 4) == 0 && h(0x14, 4) == 0 && last.writes == 1 && last.dropped == 0 && last.steps == 1);
    go(W(0x01100000, 0x00000020, 0x000000AB));
    go(W(0x02100000, 0x00000022, 0x0000BEEF));
    go(W(0x08100000, 0x00000028, 0x11223344, 0x55667788));
    CHECK(h(0x20, 8) == 0xBEEF00ABull && h(0x28, 8) == 0x1122334455667788ull);
    go(W(0x01100000, 0x00000030, 0xFFFFFF01));   /* (a value wider than its width: the low byte) */
    CHECK(h(0x30, 4) == 0x01);
    go(W(0x04000000, 0x00001000, 0xCAFEF00D));   /* region 0: main's base, into its data */
    CHECK(!memcmp(image + 0x3000, "\x0D\xF0\xFE\xCA", 4) && image[0x2FFF] == 0x11 && image[0x3004] == 0x11);
    go(W(0x40030000, 0, 0x40, 0x04130000, 0x00000010, 0x0000000A));   /* + register 3 */
    CHECK(h(0x50, 4) == 0x0A);
    go(W(0x04200000, 0x00000104, 0x000000DD));   /* region 2, the alias region: where the heap is */
    CHECK(h(0x104, 4) == 0xDD);
    go(W(0x0430000F, 0xF8000100, 0x000000EE));   /* region 3 from 0x08000000, an offset of more than 32 bits */
    CHECK(h(0x100, 4) == 0xEE);
    go(W(0x04300000, 0x78003004, 0x000000CC));   /* and into the image through it */
    CHECK(image[0x3004] == 0xCC);
    CHECK(writes_refused == 0);
}

static int cond_runs(unsigned c, unsigned width, uint64_t in_memory, uint64_t value) {
    fresh();
    hset(0x200, in_memory, 8);
    int end;
    if (width == 8)
        end = go(W(0x18100000 | c << 16, 0x200, (uint32_t)(value >> 32), (uint32_t)value, 0x01100000, 0x300, 1, 0x20000000));
    else
        end = go(W((width << 24) | 0x10100000 | c << 16, 0x200, (uint32_t)value, 0x01100000, 0x300, 1, 0x20000000));
    CHECK(end == MOD_CHEAT_RAN);
    return heap[0x300];
}
static void conditions(void) {   /* 1TMC00AA AAAAAAAA V ... 20000000, and 21000000 */
    static const int want[7][3] = {{0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {0, 1, 1}};   /* [c]: 5?5, 5?6, 6?5 */
    for (unsigned c = 1; c <= 6; c++) {
        CHECK(cond_runs(c, 4, 5, 5) == want[c][0] && cond_runs(c, 4, 5, 6) == want[c][1] && cond_runs(c, 4, 6, 5) == want[c][2]);
        CHECK(cond_runs(c, 8, 0x500000000ull, 0x500000000ull) == want[c][0] && cond_runs(c, 8, 0x500000000ull, 0x600000000ull) == want[c][1]);
    }
    CHECK(cond_runs(5, 1, 0x1FF, 0xFF) == 1 && cond_runs(5, 2, 0x1FF, 0xFF) == 0 && cond_runs(5, 1, 0x1FF, 0x2FF) == 1);
    CHECK(cond_runs(1, 4, 0xFFFFFFFF, 1) == 1);   /* (unsigned) */
    for (int truth = 0; truth < 2; truth++) {   /* an else */
        fresh();
        hset(0x200, truth, 4);
        CHECK(go(W(0x14150000, 0x200, 1, 0x01100000, 0x300, 0xA, 0x21000000, 0x01100000, 0x301, 0xB, 0x20000000, 0x01100000, 0x302, 0xC)) == MOD_CHEAT_RAN);
        CHECK(heap[0x300] == (truth ? 0xA : 0) && heap[0x301] == (truth ? 0 : 0xB) && heap[0x302] == 0xC);
    }
    for (int outer = 0; outer < 2; outer++)   /* an if in an if, each with an else */
        for (int inner = 0; inner < 2; inner++) {
            fresh();
            hset(0x200, outer, 4), hset(0x204, inner, 4);
            go(W(0x14150000, 0x200, 1,
                     0x14150000, 0x204, 1, 0x01100000, 0x300, 1, 0x21000000, 0x01100000, 0x300, 2, 0x20000000,
                 0x21000000,
                     0x14150000, 0x204, 1, 0x01100000, 0x300, 3, 0x21000000, 0x01100000, 0x300, 4, 0x20000000,
                 0x20000000, 0x01100000, 0x301, 9));
            CHECK(heap[0x300] == (outer ? (inner ? 1 : 2) : (inner ? 3 : 4)) && heap[0x301] == 9);
        }
    fresh();   /* a read outside the game's memory: the if is not taken, the cheat goes on */
    CHECK(go(W(0x14150000, 0x00010000, 0, 0x01100000, 0x300, 1, 0x20000000, 0x01100000, 0x301, 2)) == MOD_CHEAT_RAN);
    CHECK(heap[0x300] == 0 && heap[0x301] == 2 && reads_refused == 1);
    fresh();   /* an if without its end ends with the cheat; an end without an if is nothing */
    CHECK(go(W(0x14150000, 0x200, 7, 0x01100000, 0x300, 1)) == MOD_CHEAT_RAN && heap[0x300] == 0);
    CHECK(go(W(0x20000000, 0x20000000, 0x01100000, 0x300, 1)) == MOD_CHEAT_RAN && heap[0x300] == 1);
}

static void loops(void) {   /* 300R0000 VVVVVVVV ... 310R0000 */
    fresh();
    CHECK(go(W(0x30050000, 3, 0x74060000, 1, 0x31050000)) == MOD_CHEAT_RAN);
    CHECK(st.reg[6] == 3 && st.reg[5] == 0 && last.steps == 1 + 3 * 2);
    fresh();   /* a loop in a loop, each on its own register; stores with the address going on */
    go(W(0x40010000, 0x10, 0x00000600, 0x30050000, 2, 0x30040000, 3, 0x61011000, 0, 0x5A, 0x31040000, 0x31050000));
    CHECK(h(0x600, 8) == 0x5A5A5A5A5A5Aull && st.reg[1] == HB + 0x606);
    fresh();   /* a loop's end whose register is 0: it would run 2^64 times. Stopped after MOD_CHEAT_STEPS opcodes */
    CHECK(go(W(0x74060000, 1, 0x31050000)) == MOD_CHEAT_STOP_STEPS && last.steps == MOD_CHEAT_STEPS + 1 && st.reg[6] > 1000);
}

static void registers(void) {
    fresh();   /* 400R0000 V V */
    go(W(0x40070000, 0x01234567, 0x89ABCDEF, 0x400F0000, 0xFFFFFFFF, 0xFFFFFFFF));
    CHECK(st.reg[7] == 0x0123456789ABCDEFull && st.reg[15] == ~0ull && st.reg[0] == 0);
    /* 5TMR00AA AAAAAAAA and 5T0R10AA AAAAAAAA */
    hset(0x400, HB + 0x500, 8), hset(0x504, 0xAABBCCDD, 4);
    CHECK(go(W(0x58170000, 0x400)) == MOD_CHEAT_RAN && st.reg[7] == HB + 0x500);
    CHECK(go(W(0x54171000, 0x4)) == MOD_CHEAT_RAN);
    CHECK(st.reg[7] == 0x00000010AABBCCDDull);   /* (4 bytes loaded: the register's upper bytes stay, as Atmosphere's) */
    go(W(0x40020000, 0, 0, 0x51120000, 0x504, 0x40030000, 0, 0, 0x52130000, 0x504));
    CHECK(st.reg[2] == 0xDD && st.reg[3] == 0xCCDD);
    image[0x3000] = 0x77;
    go(W(0x40040000, 0, 0, 0x51040000, 0x1000, 0x40050000, 0, 0, 0x51350000, 0x78003000, 0x40060000, 0, 0, 0x51260000, 0x504));
    CHECK(st.reg[4] == 0x77 && st.reg[5] == 0x77 && st.reg[6] == 0xDD);   /* (main, the address space, the alias region) */
    /* a read outside the game's memory ends the cheat for the frame: what follows it would work on garbage */
    fresh();
    CHECK(go(W(0x58170000, 0x00010000, 0x01100000, 0x300, 1)) == MOD_CHEAT_STOP_READ);
    CHECK(last.addr == HB + 0x10000 && heap[0x300] == 0 && last.steps == 1 && reads_refused == 1);
    CHECK(go(W(0x58171000, 0x8, 0x01100000, 0x300, 1)) == MOD_CHEAT_STOP_READ && last.addr == 8 && heap[0x300] == 0);   /* (a null pointer) */
}

static void store_register_address(void) {   /* 6T0RIor0 V V */
    fresh();
    go(W(0x40010000, 0x10, 0x00000600, 0x64010000, 0, 0x2A));
    CHECK(h(0x600, 4) == 0x2A && st.reg[1] == HB + 0x600);
    go(W(0x62011000, 0, 0x1234, 0x62011000, 0, 0x5678));
    CHECK(h(0x600, 4) == 0x56781234 && st.reg[1] == HB + 0x604);
    go(W(0x40020000, 0, 0x10, 0x61010120, 0, 0x77));
    CHECK(heap[0x614] == 0x77 && st.reg[1] == HB + 0x604);
    go(W(0x68011000, 0xAABBCCDD, 0x11223344));
    CHECK(h(0x604, 8) == 0xAABBCCDD11223344ull && st.reg[1] == HB + 0x60C);
}

static uint64_t legacy(unsigned width, unsigned op, uint64_t a, uint32_t b) {
    fresh();
    go(W(0x40010000, (uint32_t)(a >> 32), (uint32_t)a, 0x70010000 | width << 24 | op << 12, b));
    return st.reg[1];
}
static uint64_t math(unsigned width, unsigned op, uint64_t a, uint64_t b, int immediate) {
    fresh();
    go(W(0x40010000, (uint32_t)(a >> 32), (uint32_t)a, 0x40020000, (uint32_t)(b >> 32), (uint32_t)b));
    if (!immediate) go(W(0x90031020 | width << 24 | op << 20));
    else if (width == 8) go(W(0x90031100 | width << 24 | op << 20, (uint32_t)(b >> 32), (uint32_t)b));
    else go(W(0x90031100 | width << 24 | op << 20, (uint32_t)b));
    return st.reg[3];
}
static void arithmetic(void) {
    /* 7T0RC000 VVVVVVVV */
    CHECK(legacy(4, 0, 10, 5) == 15 && legacy(4, 1, 10, 5) == 5 && legacy(4, 2, 10, 5) == 50 && legacy(4, 3, 10, 5) == 320 && legacy(4, 4, 320, 5) == 10);
    CHECK(legacy(1, 0, 0xFF, 1) == 0 && legacy(2, 0, 0xFFFF, 2) == 1 && legacy(4, 1, 0, 1) == 0xFFFFFFFFull && legacy(8, 1, 0, 1) == ~0ull);
    CHECK(legacy(8, 0, 0x100000000ull, 1) == 0x100000001ull && legacy(4, 0, 0x100000000ull, 1) == 1);
    /* 9TCRS0s0 and 9TCRS100 V */
    for (int imm = 0; imm < 2; imm++) {
        CHECK(math(8, 0, 0xF0F0, 4, imm) == 0xF0F4 && math(8, 1, 0xF0F0, 4, imm) == 0xF0EC && math(8, 2, 0xF0F0, 4, imm) == 0x3C3C0);
        CHECK(math(8, 3, 0xF0F0, 4, imm) == 0xF0F00 && math(8, 4, 0xF0F0, 4, imm) == 0xF0F && math(8, 5, 0xF0F0, 0xFF, imm) == 0xF0);
        CHECK(math(8, 6, 0xF0F0, 0xF, imm) == 0xF0FF && math(8, 7, 0xF0F0, 4, imm) == ~0xF0F0ull && math(8, 8, 0xF0F0, 0xFF, imm) == 0xF00F);
        CHECK(math(8, 9, 0xF0F0, 4, imm) == 0xF0F0);
        CHECK(math(4, 7, 0xF0F0, 4, imm) == 0xFFFF0F0Full && math(1, 0, 0xFF, 2, imm) == 1 && math(2, 2, 0x1000, 0x10, imm) == 0);
        CHECK(math(8, 3, 1, 64, imm) == 0 && math(8, 4, ~0ull, 64, imm) == 0 && math(8, 3, 1, 63, imm) == 1ull << 63);   /* (no shift by 64) */
        CHECK(math(8, 0, 0x8000000000000000ull, 0x8000000000000001ull, imm) == 1);
    }
}

static void keys(void) {   /* 8kkkkkkk ... 20000000 */
    static const uint64_t held[] = {0, 1, 2, 3, 0x8000003};
    for (unsigned i = 0; i < 5; i++) {
        fresh();
        CHECK(go_keys(held[i], W(0x80000001, 0x01100000, 0x300, 1, 0x20000000, 0x80000003, 0x01100000, 0x301, 1, 0x21000000, 0x01100000, 0x302, 1,
                                 0x20000000, 0x88000000, 0x01100000, 0x303, 1, 0x20000000)) == MOD_CHEAT_RAN);
        CHECK(heap[0x300] == (held[i] & 1) && heap[0x301] == ((held[i] & 3) == 3) && heap[0x302] == ((held[i] & 3) != 3));
        CHECK(heap[0x303] == (held[i] >> 27 & 1));
    }
}

static void store_register(void) {   /* ATSRIOxa (aaaaaaaa) */
    fresh();
    go(W(0x40050000, 0xDEADBEEF, 0x11223344, 0x40010000, 0x10, 0x00000700, 0x40020000, 0, 0x20));
    go(W(0xA4510000));
    CHECK(h(0x700, 8) == 0x11223344);
    go(W(0xA8511000));
    CHECK(h(0x700, 8) == 0xDEADBEEF11223344ull && st.reg[1] == HB + 0x708);
    go(W(0xA2510120));
    CHECK(h(0x728, 4) == 0x3344);
    go(W(0xA1510200, 0x00000040));
    CHECK(h(0x748, 4) == 0x44);
    go(W(0x40010000, 0, 0x780, 0xA4510310, 0xA4510410, 0x00000790, 0xA4510510, 0x00000020));
    CHECK(h(0x780, 4) == 0x11223344 && h(0x790, 4) == 0x11223344 && h(0x7A0, 4) == 0x11223344);
    go(W(0x40010000, 0, 0x1000, 0xA1510300, 0xA1510401, 0x00000001));   /* region 0, and an offset's high digit: outside */
    CHECK(image[0x3000] == 0x44 && last.dropped == 1 && last.addr == MB + 0x100000001ull);
    go(W(0x40010000, 0, 0x7B0, 0xA4511310));   /* (the address register goes on by the width) */
    CHECK(h(0x7B0, 4) == 0x11223344 && st.reg[1] == 0x7B4);
}

static int reg_cond(uint32_t first, int extra, uint32_t e1, uint32_t e2) {
    heap[0x300] = 0;
    uint32_t all[8];
    int n = 0;
    all[n++] = first;
    if (extra > 0) all[n++] = e1;
    if (extra > 1) all[n++] = e2;
    all[n++] = 0x01100000, all[n++] = 0x300, all[n++] = 1, all[n++] = 0x20000000;
    CHECK(go(all, n) == MOD_CHEAT_RAN);
    return heap[0x300];
}
static void register_conditions(void) {   /* C0TcSX.. */
    static const int want[7][3] = {{0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {0, 1, 1}};   /* [c]: 5?5, 5?6, 6?5 */
    fresh();
    hset(0x200, 5, 8), hset(0x208, 6, 8), hset(0x210, 0x1FF, 8);
    for (unsigned c = 1; c <= 6; c++) {
        const uint32_t base = 0xC0400000 | c << 16 | 3 << 12;   /* (4 bytes, register 3) */
        /* (register 3 is 5, 4 is 6; 1 points at the 5 in memory, the 6 is 8 after it; 8 and 6 are their offsets in the heap.
           Not register 0: the stores of this test's ifs add it, as every opcode 0 adds its offset register) */
        go(W(0x40030000, 0, 5, 0x40040000, 0, 6, 0x40010000, 0x10, 0x200, 0x40020000, 0, 8, 0x40080000, 0, 0x200, 0x40060000, 0, 0x208,
             0x40070000, 0, 0));
        CHECK(reg_cond(base | 0x400, 1, 5, 0) == want[c][0] && reg_cond(base | 0x400, 1, 6, 0) == want[c][1]);   /* a value */
        CHECK(reg_cond(base | 0x530, 0, 0, 0) == want[c][0] && reg_cond(base | 0x540, 0, 0, 0) == want[c][1]);   /* another register */
        CHECK(reg_cond(base | 0x010, 1, 0x200, 0) == want[c][0] && reg_cond(base | 0x010, 1, 0x208, 0) == want[c][1]);   /* region + offset */
        CHECK(reg_cond(base | 0x118, 0, 0, 0) == want[c][0] && reg_cond(base | 0x116, 0, 0, 0) == want[c][1]);   /* region + register */
        CHECK(reg_cond(base | 0x210, 1, 0, 0) == want[c][0] && reg_cond(base | 0x210, 1, 8, 0) == want[c][1]);   /* register + offset */
        CHECK(reg_cond(base | 0x317, 0, 0, 0) == want[c][0] && reg_cond(base | 0x312, 0, 0, 0) == want[c][1]);   /* register + register */
        go(W(0x40030000, 0, 6));
        CHECK(reg_cond(base | 0x400, 1, 5, 0) == want[c][2] && reg_cond(base | 0x010, 1, 0x200, 0) == want[c][2]);
    }
    go(W(0x40030000, 0xAAAAAAAA, 0x000001FF));   /* the width: of the register, the value and the memory alike */
    CHECK(reg_cond(0xC0153400, 1, 0xFF, 0) == 1 && reg_cond(0xC0253400, 1, 0xFF, 0) == 0 && reg_cond(0xC0253400, 1, 0x1FF, 0) == 1);
    CHECK(reg_cond(0xC0853400, 2, 0xAAAAAAAA, 0x1FF) == 1 && reg_cond(0xC0853400, 2, 0, 0x1FF) == 0 && reg_cond(0xC0153010, 1, 0x210, 0) == 1);
    reads_refused = 0;   /* (a read outside: not taken) */
    CHECK(reg_cond(0xC0453010, 1, 0x00010000, 0) == 0 && reads_refused == 1);
}

static void saved_and_static(void) {
    fresh();   /* C10D0Sx0 */
    go(W(0x40030000, 0, 0x33, 0xC1020310));
    CHECK(st.saved[2] == 0x33 && st.reg[2] == 0);
    go(W(0xC1040200));
    CHECK(st.reg[4] == 0x33);
    go(W(0xC1020020, 0xC1040030));
    CHECK(st.saved[2] == 0 && st.reg[4] == 0 && st.reg[3] == 0x33);
    fresh();   /* C2x0XXXX */
    go(W(0x40010000, 0, 0x11, 0x40020000, 0, 0x22, 0x400F0000, 0, 0xFF, 0xC2108006));
    CHECK(st.saved[1] == 0x11 && st.saved[2] == 0x22 && st.saved[15] == 0xFF && st.saved[0] == 0 && st.saved[3] == 0);
    go(W(0xC2300006));
    CHECK(st.reg[1] == 0 && st.reg[2] == 0 && st.reg[15] == 0xFF);
    go(W(0xC2000004));
    CHECK(st.reg[1] == 0 && st.reg[2] == 0x22);
    go(W(0xC2208002));
    CHECK(st.saved[1] == 0 && st.saved[15] == 0 && st.saved[2] == 0x22);
    fresh();   /* C3000XXx: the static registers, read below 0x80 and written from 0x80 */
    st.stat[5] = 0x1234, st.stat[0x7F] = 0x4321;
    go(W(0x40010000, 0, 0x99, 0xC3000052, 0xC30007F4, 0xC3000801, 0xC3000FF1));
    CHECK(st.reg[2] == 0x1234 && st.reg[4] == 0x4321 && st.stat[0x7F] == 0x4321 && st.stat[5] == 0x1234);
    CHECK(st.stat[0x80] == 0x99 && st.stat[0xFF] == 0x99 && st.reg[1] == 0x99);
    mod_cheat_frame(&st);   /* (a new frame: the registers and the saved values are 0 again, the static ones stay) */
    CHECK(st.reg[1] == 0 && st.reg[2] == 0 && st.stat[0x80] == 0x99 && st.stat[5] == 0x1234);
}

/* one frame of a file: registers shared by its cheats, cleared between frames */
static void shared_state(void) {
    fresh();
    mod_cheat_frame(&st);
    CHECK(go(W(0x40010000, 0x10, 0x00000600)) == MOD_CHEAT_RAN);                     /* a master code sets a register */
    CHECK(go(W(0x64010000, 0, 0x55)) == MOD_CHEAT_RAN && h(0x600, 4) == 0x55);       /* a cheat after it uses it */
    mod_cheat_frame(&st);
    hset(0x600, 0, 4);
    CHECK(go(W(0x64010000, 0, 0x55)) == MOD_CHEAT_RAN && h(0x600, 4) == 0 && last.dropped == 1 && last.addr == 0);
    /* a cheat's loop does not go on into the next cheat: its top is the cheat's own */
    fresh();
    go(W(0x30050000, 2));
    CHECK(go(W(0x74060000, 1, 0x31050000)) == MOD_CHEAT_RAN && st.reg[6] == 2 && st.reg[5] == 0);
}

static void refused(const uint32_t *w, int n, const char *what) {
    char why[MOD_CHEAT_WHY] = "", word[16];
    snprintf(word, sizeof word, "%08X", w[0]);
    const int ok = mod_cheat_check(w, n, why, sizeof why);
    if (ok || !strstr(why, what) || !strstr(why, word)) printf("FAIL: %s: refused %d, said \"%s\" (wanted \"%s\")\n", word, !ok, why, what), fails++;
    /* (and run all the same, it stops there: nothing of it or after it is done) */
    uint32_t then[8];
    memcpy(then, w, sizeof *w * (size_t)n);
    then[n] = 0x01100000, then[n + 1] = 0x300, then[n + 2] = 1;
    fresh();
    CHECK(mod_cheat_run(then, n + 3, &st, &mem, ~0ull, &last) == MOD_CHEAT_STOP_BAD && heap[0x300] == 0 && last.writes == 0);
}
static void unsupported(void) {
    refused(W(0x03100000, 0x10, 1), "bad width");
    refused(W(0x00100000, 0x10, 1), "bad width");
    refused(W(0x0F100000, 0x10, 1), "bad width");
    refused(W(0x04400000, 0x10, 1), "not supported (its memory region)");
    refused(W(0x14100000, 0x10, 1), "not supported (its comparison)");
    refused(W(0x14170000, 0x10, 1), "not supported (its comparison)");
    refused(W(0x14151000, 0x10, 1), "not supported (the form with an offset register)");
    refused(W(0x14150300, 0x10, 1), "not supported (the form with an offset register)");
    refused(W(0x16150000, 0x10, 1), "bad width");
    refused(W(0x22000000), "not supported");
    refused(W(0x32000000), "not supported");
    refused(W(0x54102000, 0x10), "not supported (the form with a second register)");
    refused(W(0x54503000, 0x10), "not supported");
    refused(W(0x54400000, 0x10), "not supported (its memory region)");
    refused(W(0x53100000, 0x10), "bad width");
    refused(W(0x63010000, 0, 1), "bad width");
    refused(W(0x74015000, 1), "not supported (its operation)");
    refused(W(0x70010000, 1), "bad width");
    refused(W(0x94A31020), "not supported (its operation");   /* (a float add) */
    refused(W(0x94F31020), "not supported (its operation");
    refused(W(0x95031020), "bad width");
    refused(W(0xA4510600), "not supported (its address form)");
    refused(W(0xA4510340), "not supported (its memory region)");
    refused(W(0xA3510000), "bad width");
    refused(W(0xB0000000), "not supported");
    refused(W(0xC0403400, 1), "not supported (its comparison)");
    refused(W(0xC0473400, 1), "not supported (its comparison)");
    refused(W(0xC0453600), "not supported (its operand)");
    refused(W(0xC0453040, 0x10), "not supported (its memory region)");
    refused(W(0xC0353400, 1), "bad width");
    refused(W(0xC1020340), "not supported (its operation)");
    refused(W(0xC2400006), "not supported (its operation)");
    refused(W(0xC4000000, 0, 1), "not supported");   /* (the newer opcodes: an if on 64 bits of buttons ...) */
    refused(W(0xC5000000), "not supported");
    refused(W(0xD0000000), "not supported");
    refused(W(0xE0000000), "not supported");
    refused(W(0xFF000000), "not supported");   /* (pause the game) */
    refused(W(0xFF100000), "not supported");   /* (and let it go on) */
    refused(W(0xFFF00000, 0), "not supported");   /* (write to a log) */
    /* an opcode without all its words */
    static const uint32_t shorts[][4] = {{0x04100000, 0x10}, {0x08100000, 0x10, 1}, {0x14150000}, {0x30050000}, {0x40010000, 0}, {0x58170000},
                                         {0x64010000, 0}, {0x74010000}, {0x98031100, 0}, {0xA4510200}, {0xC0453400}, {0xC0853400, 0}};
    static const int have[] = {2, 3, 1, 1, 2, 1, 2, 1, 2, 1, 1, 2};
    for (unsigned i = 0; i < sizeof have / sizeof have[0]; i++) {
        char why[MOD_CHEAT_WHY] = "";
        CHECK(!mod_cheat_check(shorts[i], have[i], why, sizeof why) && strstr(why, "cut short"));
        CHECK(mod_cheat_run(shorts[i], have[i], &st, &mem, 0, &last) == MOD_CHEAT_STOP_BAD);
    }
    /* and every opcode's length when it is whole */
    static const uint32_t firsts[] = {0x04100000, 0x08100000, 0x14150000, 0x18150000, 0x20000000, 0x21000000, 0x30050000, 0x31050000, 0x40010000,
                                      0x58170000, 0x54171000, 0x64010000, 0x74010000, 0x80000001, 0x98031020, 0x94031100, 0x98031100, 0xA4510000,
                                      0xA4510120, 0xA4510200, 0xA4510310, 0xA4510410, 0xA4510510, 0xC0453010, 0xC0453110, 0xC0453210, 0xC0453310,
                                      0xC0453400, 0xC0853400, 0xC0453540, 0xC1020310, 0xC2100006, 0xC3000801};
    static const int lens[] = {3, 4, 3, 4, 1, 1, 2, 1, 3, 2, 2, 3, 2, 1, 1, 2, 3, 1, 1, 2, 1, 2, 2, 2, 1, 2, 1, 2, 3, 1, 1, 1, 1};
    for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        const uint32_t w[4] = {firsts[i], 0, 0, 0};
        if (mod_cheat_op_len(w, 4, NULL, 0) != lens[i]) printf("FAIL: %08X is %d words, not %d\n", firsts[i], mod_cheat_op_len(w, 4, NULL, 0), lens[i]), fails++;
    }
}

static void bounds(void) {
    const ModCheatLayout *l = &layout;
    CHECK(mod_cheat_readable(l, HB, 1) && mod_cheat_readable(l, HB + HEAP_SIZE - 1, 1) && mod_cheat_readable(l, HB, HEAP_SIZE));
    CHECK(mod_cheat_readable(l, HB + HEAP_SIZE - 8, 8) && !mod_cheat_readable(l, HB + HEAP_SIZE - 7, 8) && !mod_cheat_readable(l, HB + HEAP_SIZE, 1));
    CHECK(!mod_cheat_readable(l, HB - 1, 1) && !mod_cheat_readable(l, HB - 1, 2) && !mod_cheat_readable(l, HB, HEAP_SIZE + 1));
    CHECK(!mod_cheat_readable(l, 0, 8) && !mod_cheat_readable(l, 8, 8) && !mod_cheat_readable(l, ~0ull, 1) && !mod_cheat_readable(l, ~0ull - 3, 8));
    CHECK(!mod_cheat_readable(l, HB + 1, ~0ull) && !mod_cheat_readable(l, IB + 1, ~0ull));   /* (no sum that wraps) */
    CHECK(mod_cheat_readable(l, IB, 1) && mod_cheat_readable(l, IB + IMAGE_SIZE - 4, 4) && !mod_cheat_readable(l, IB + IMAGE_SIZE - 3, 4));
    CHECK(!mod_cheat_readable(l, IB - 1, 1) && !mod_cheat_readable(l, IB + IMAGE_SIZE, 1) && !mod_cheat_readable(l, 0x08000000ull, 4));
    /* the code is read, never written: neither module's, nor a store that only reaches into it */
    CHECK(mod_cheat_readable(l, IB, 4) && !mod_cheat_writable(l, IB, 4) && !mod_cheat_writable(l, IB + 0xFFC, 4) && !mod_cheat_writable(l, IB + 0xFFF, 2));
    CHECK(mod_cheat_writable(l, IB + 0x1000, 4) && mod_cheat_writable(l, MB - 4, 4) && !mod_cheat_writable(l, MB - 3, 4) && !mod_cheat_writable(l, MB, 1));
    CHECK(!mod_cheat_writable(l, MB + 0x7FF, 1) && !mod_cheat_writable(l, MB + 0x7FE, 4) && mod_cheat_writable(l, MB + 0x800, 8));
    CHECK(mod_cheat_writable(l, HB, 8) && !mod_cheat_writable(l, HB + HEAP_SIZE - 1, 2) && !mod_cheat_writable(l, 0, 1));
    ModCheatLayout none = layout;   /* (before the game has a heap) */
    none.heap_size = 0;
    CHECK(!mod_cheat_readable(&none, HB, 1) && mod_cheat_readable(&none, IB, 1));
    /* through the interpreter: every kind of store dropped outside, and the cheat goes on */
    fresh();
    CHECK(go(W(0x04100000, 0x00001FFD, 0xAAAAAAAA,    /* the heap's last three bytes and one more */
               0x04100000, 0x00002000, 0xBBBBBBBB,    /* past it */
               0x0410007F, 0xFFFFFFFF, 0xCCCCCCCC,    /* far */
               0x04000000, 0x00000100, 0xDDDDDDDD,    /* main's code */
               0x08000000, 0x000007FC, 0xEEEEEEEE, 0xEEEEEEEE,   /* its last word and the data after */
               0x40010000, 0xFFFFFFFF, 0xFFFFFFFC, 0x68010000, 0x12121212, 0x12121212,   /* an address that wraps */
               0x40010000, 0, 0x80000010, 0x64010000, 0, 0x34343434,                     /* the loader's code by a register */
               0x40050000, 0, 0x56, 0xA1510000, 0xA1510411, 0x00002000,                  /* and by the other stores */
               0x01100000, 0x00001FFF, 0x00000099)) == MOD_CHEAT_RAN);
    CHECK(last.dropped == 9 && last.writes == 1 && writes_refused == 9 && heap[0x1FFF] == 0x99 && heap[0x1FFD] == 0 && last.addr == HB + 0x100002000ull);
    for (unsigned i = 0; i < IMAGE_SIZE; i++)
        if (image[i] != 0x11) { printf("FAIL: the image was written at %#x\n", i), fails++; break; }
    for (unsigned i = 0; i < HEAP_SIZE - 1; i++)
        if (heap[i]) { printf("FAIL: the heap was written at %#x\n", i), fails++; break; }
    /* a cheat that stores into main's code outright is told from one that stores into its data */
    uint64_t where = 0;
    CHECK(mod_cheat_stores_code(W(0x04100000, 0x100, 1, 0x04000000, 0x000007FC, 0xD503201F), l, MB, &where) && where == MB + 0x7FC);
    CHECK(!mod_cheat_stores_code(W(0x04000000, 0x00000800, 1, 0x04100000, 0x0, 1, 0x04300000, 0x78002000, 1), l, MB, &where));
    CHECK(mod_cheat_stores_code(W(0x08000000, 0x000007FC, 1, 2), l, MB, NULL) && mod_cheat_stores_code(W(0x01000000, 0x000007FF, 1), l, MB, NULL));
    CHECK(!mod_cheat_stores_code(W(0x04000000, 0x00000100), l, MB, NULL));   /* (cut short: not looked at; the check refuses it) */
}

static void file(void) {
    static const char text[] =
        "\xEF\xBB\xBF" "Ocarina cheats v1.0 by somebody 04100000\r\n"
        "[ Infinite Hearts ]\r\n04100000 00000010 0000000A\r\n\r\n"
        "{Master Code}\n40010000 00000000 00000001\n"
        "[lower case]\n0410000a 0000001c deadbeef\n\t08100000 00000020 11111111 22222222\n"
        "[Heading]\n"
        "[--- Section ---]\n20000000\n"
        "[bad word]\n04100000 0000001 0000000A\n"
        "[not hex]\n04100000 0000001G 0000000A\n"
        "[pause]\n04100000 00000010 0000000A\nFF000000\n"
        "[]\n"
        "04100000 00000030 0000000B [same line] 20000000 20000000\n"
        "[cut]\n04100000 00000010\n";
    static ModCheat c[16];
    static uint32_t pool[64];
    int cut = -1;
    const int n = mod_cheat_parse(text, sizeof text - 1, c, 16, pool, 64, &cut);
    CHECK(n == 11 && cut == 0);
    CHECK(!strcmp(c[0].name, "Infinite Hearts") && !c[0].master && c[0].n == 3 && c[0].words[0] == 0x04100000 && c[0].words[2] == 0xA && !c[0].why[0] && c[0].line == 2);
    CHECK(!strcmp(c[1].name, "Master Code") && c[1].master && c[1].n == 3 && !c[1].why[0] && c[1].line == 5);
    CHECK(!strcmp(c[2].name, "lower case") && c[2].n == 7 && c[2].words[0] == 0x0410000A && c[2].words[2] == 0xDEADBEEF && c[2].words[6] == 0x22222222);
    CHECK(!strcmp(c[3].name, "Heading") && c[3].n == 0 && mod_cheat_is_heading(&c[3]) && !mod_cheat_is_heading(&c[0]));
    CHECK(!strcmp(c[4].name, "--- Section ---") && c[4].n == 1 && mod_cheat_is_heading(&c[4]));
    CHECK(strstr(c[5].why, "line 14") && strstr(c[5].why, "\"0000001\" is not a code word") && !mod_cheat_is_heading(&c[5]));
    CHECK(strstr(c[6].why, "line 16") && strstr(c[6].why, "\"0000001G\" is not a code word"));
    CHECK(strstr(c[7].why, "opcode FF000000 is not supported") && c[7].n == 4);
    CHECK(!strcmp(c[8].name, "") && c[8].n == 3 && c[8].words[1] == 0x30 && !c[8].why[0]);   /* (its code is on the next line) */
    CHECK(!strcmp(c[9].name, "same line") && c[9].n == 2 && !c[9].why[0] && !mod_cheat_is_heading(&c[9]));
    CHECK(!strcmp(c[10].name, "cut") && strstr(c[10].why, "opcode 04100000 is cut short"));
    /* the first cheat as read, run */
    fresh();
    CHECK(mod_cheat_run(c[0].words, c[0].n, &st, &mem, 0, &last) == MOD_CHEAT_RAN && h(0x10, 4) == 0xA);

    /* a name is at most 63 bytes, never cut inside a letter */
    char longer[200];
    snprintf(longer, sizeof longer, "[%s\xC3\xA9%s]\n20000000\n", "0123456789012345678901234567890123456789012345678901234567890", "tail");   /* (61, then a 2-byte letter at 62 and 63) */
    CHECK(mod_cheat_parse(longer, strlen(longer), c, 16, pool, 64, &cut) == 1 && strlen(c[0].name) == 63 && !memcmp(c[0].name + 61, "\xC3\xA9", 2));
    snprintf(longer, sizeof longer, "[%s\xC3\xA9%s]\n20000000\n", "01234567890123456789012345678901234567890123456789012345678901", "tail");   /* (62: the letter would be cut) */
    CHECK(mod_cheat_parse(longer, strlen(longer), c, 16, pool, 64, &cut) == 1 && strlen(c[0].name) == 62 && c[0].name[61] == '1');

    /* a name without its closing bracket is no name: the cheat before it would get its code, and is refused */
    static const char open[] = "[A]\n04100000 00000010 0000000A\n[B\n04100000 00000020 0000000B\n[C]\n04100000 00000030 0000000C\n";
    CHECK(mod_cheat_parse(open, sizeof open - 1, c, 16, pool, 64, &cut) == 2 && !strcmp(c[0].name, "A") && strstr(c[0].why, "line 3: a name without its closing ]"));
    CHECK(!strcmp(c[1].name, "C") && !c[1].why[0] && c[1].n == 3 && c[1].words[1] == 0x30);

    /* more cheats, or more code, than there is room for: said, and the cheat that is not whole is refused */
    static const char three[] = "[1]\n04100000 00000010 00000001\n[2]\n04100000 00000010 00000002\n[3]\n04100000 00000010 00000003\n";
    CHECK(mod_cheat_parse(three, sizeof three - 1, c, 2, pool, 64, &cut) == 2 && cut == 1 && !c[1].why[0] && c[1].words[2] == 2);
    CHECK(mod_cheat_parse(three, sizeof three - 1, c, 16, pool, 5, &cut) == 3 && cut == 1 && !c[0].why[0] && strstr(c[1].why, "too many code words") && c[2].why[0]);
    CHECK(mod_cheat_parse("", 0, c, 16, pool, 64, &cut) == 0 && mod_cheat_parse("no cheats here\n04100000\n", 24, c, 16, pool, 64, &cut) == 0);

    CHECK(mod_cheat_hash("") == 0x811C9DC5u && mod_cheat_hash("a") == 0xE40C292Cu && mod_cheat_hash("foobar") == 0xBF9CF968u);
}

int main(void) {
    store_static();
    conditions();
    loops();
    registers();
    store_register_address();
    arithmetic();
    keys();
    store_register();
    register_conditions();
    saved_and_static();
    shared_state();
    unsupported();
    bounds();
    file();
    if (!fails) printf("PASS\n");
    return fails != 0;
}
