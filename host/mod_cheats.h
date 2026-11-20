/* (Triforce) The mods' cheat files: the reader and the interpreter (host/mod_cheats.c has the files, the game's memory
   and the rows; tests/test_mod_cheats.py compiles this alone against a stand-in memory).

   A cheat file is the text file Atmosphere and the emulators take, mods/<name>/cheats/<build id>.txt, the build id the
   first 8 bytes of main's in hex: a cheat is a name in square brackets followed by its code, words of 8 hex digits; a
   name in braces is a master code, which always runs. The code is a program for a small machine over the game's
   memory: 16 registers, 16 saved values, static registers, and the opcodes below. It is run once a game frame, here by
   an interpreter on the game's presenting thread: nothing of it is compiled into the game, so a cheat file needs no
   build. (NX2navi compiled in the cheats that write the game's instructions, as patch sets its translator could switch.
   This translator has none: such a cheat is refused here, and what it changes can be made a code patch, nx2/code_mods.py.)

   The opcodes, by the first word's first digit (T a width of 1, 2, 4 or 8 bytes; M a region, 0 main, 1 the heap, 2 the
   alias region, 3 the address space; A an offset of 40 bits; V a value, two words when T is 8):
     0TMR00AA AAAAAAAA V       store V at M + A + register R
     1TMC00AA AAAAAAAA V       if the value at M + A <C> V (C: 1 >, 2 >=, 3 <, 4 <=, 5 ==, 6 !=) ... 20000000
     20000000 / 21000000       the end of an if / its else
     300R0000 VVVVVVVV         a loop begins, register R its count; 310R0000 ends it (R one less, again while not 0)
     400R0000 VVVVVVVV VVVVVVVV  register R = V
     5TMR00AA AAAAAAAA         register R = the value at M + A;  5T0R10AA AAAAAAAA  at R + A
     6T0RIor0 VVVVVVVV VVVVVVVV  store V at register R (+ register r when o is 1); I: R goes on by T after
     7T0RC000 VVVVVVVV         R = R <C> V (C: 0 +, 1 -, 2 *, 3 <<, 4 >>)
     8kkkkkkk                  if the buttons k are held ... 20000000
     9TCRS0s0 / 9TCRS100 V     R = S <C> s, or V (C: 0 to 4 as above, 5 and, 6 or, 7 not, 8 xor, 9 the value of S itself)
     ATSRIOxa (aaaaaaaa)       store register S at an address by O: 0 R, 1 R + register x, 2 R + a, 3 region x + R,
                               4 region x + a, 5 region x + R + a; I: R goes on by T after
     C0TcSX.. (..)             if register S <c> an operand by X: 0 the value at region + a, 1 at region + register,
                               2 at register + a, 3 at register + register, 4 a value V, 5 another register
     C10D0Sx0                  x 0: register D = saved S; 1: saved D = register S; 2: saved D = 0; 3: register D = 0
     C2x0XXXX                  the same for every register whose bit is set in XXXX
     C3000XXx                  static register XX: below 0x80 read into register x, from 0x80 written from it
   Every other opcode (the 1 with an offset register, the 5 with one, the float operations of 9, C4 on, FF0, FF1 and
   FFF) is refused when the file is read, the whole cheat with a line that names the word: never passed over, which
   would run what follows it as something else.

   As Atmosphere's machine does: registers and saved values are 0 at the start of a frame and are shared by the cheats
   of one file in that frame (a master code can set up a register for the others); the static registers stay; a value
   narrower than 8 bytes loaded with opcode 5 leaves the register's upper bytes as they were. Not as it does: a cheat's
   ifs and loops end with the cheat (one cheat's missing end does not swallow the next); a read outside the game's
   memory ends that cheat for the frame (a pointer that is not set yet: what follows would work on garbage), a write
   outside it or into the game's code is dropped; a cheat that runs past MOD_CHEAT_STEPS opcodes in a frame (a loop that
   never ends) is stopped for that frame.

   The game's memory for a cheat is its program image and its heap (ModCheatLayout): both are committed whole
   (host/hos_mem.c), so the check is a few compares and no access can fault. The code of the modules is read but never
   written: it is compiled ahead of time, a write there would change nothing but what the game reads of itself. */
#ifndef MOD_CHEATS_H
#define MOD_CHEATS_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { MOD_CHEAT_NAME = 64, MOD_CHEAT_WHY = 120, MOD_CHEAT_STEPS = 1 << 16 };
enum { MOD_CHEAT_MAIN, MOD_CHEAT_HEAP, MOD_CHEAT_ALIAS, MOD_CHEAT_ASLR, MOD_CHEAT_REGIONS };

/* ---- the game's memory ---- */
typedef struct ModCheatLayout {
    uint64_t image_base, image_end;   /* the program image: every module, code and data */
    uint64_t heap_base, heap_size;
    const uint64_t (*code)[2];        /* the modules' code, [from, to) each */
    int ncode;
} ModCheatLayout;

/* 1 when the n bytes at a are the game's: all of them in its heap, or all in its image. No sum that could wrap */
static inline int mod_cheat_readable(const ModCheatLayout *l, uint64_t a, uint64_t n) {
    if (a >= l->heap_base && a - l->heap_base < l->heap_size) return n <= l->heap_size - (a - l->heap_base);
    if (a >= l->image_base && a < l->image_end) return n <= l->image_end - a;
    return 0;
}
/* 1 when the n bytes at a (readable) touch a module's code */
static inline int mod_cheat_in_code(const ModCheatLayout *l, uint64_t a, uint64_t n) {
    for (int i = 0; i < l->ncode; i++)
        if (a < l->code[i][1] && n > 0 && a + (n - 1) >= l->code[i][0]) return 1;
    return 0;
}
static inline int mod_cheat_writable(const ModCheatLayout *l, uint64_t a, uint64_t n) {
    return mod_cheat_readable(l, a, n) && !mod_cheat_in_code(l, a, n);
}

/* what the interpreter reads and writes through: 1 when done, 0 when the bytes are not the game's (nothing was read or
   written). base: where each region begins */
typedef struct ModCheatMem {
    void *ctx;
    int (*read)(void *ctx, uint64_t addr, unsigned n, void *out);
    int (*write)(void *ctx, uint64_t addr, unsigned n, const void *in);
    uint64_t base[MOD_CHEAT_REGIONS];
} ModCheatMem;

/* ---- the opcodes ---- */
static inline int mod_cheat_width_ok(unsigned w) { return w == 1 || w == 2 || w == 4 || w == 8; }
/* an opcode that begins an if: 1, 8, C0 */
static inline int mod_cheat_begins_if(uint32_t f) { return f >> 28 == 1 || f >> 28 == 8 || f >> 24 == 0xC0; }

/* The words of the opcode at w (left of them remain); 0 when it is cut short, not supported or has a bad width, with
   the reason in why (when given) */
static inline int mod_cheat_op_len(const uint32_t *w, int left, char *why, size_t cap) {
    const uint32_t f = w[0];
    const unsigned t = f >> 28, width = (f >> 24) & 0xF, d5 = (f >> 20) & 0xF, d4 = (f >> 16) & 0xF, d3 = (f >> 12) & 0xF,
                   d2 = (f >> 8) & 0xF, d1 = (f >> 4) & 0xF;
    const char *bad = NULL;   /* (not supported: what about it) */
    int n = 1, sized = 0;     /* (sized: the opcode has a width that must be 1, 2, 4 or 8) */
    switch (t) {
    case 0: n = width == 8 ? 4 : 3, sized = 1; if (d5 >= MOD_CHEAT_REGIONS) bad = "its memory region"; break;
    case 1:
        n = width == 8 ? 4 : 3, sized = 1;
        if (d5 >= MOD_CHEAT_REGIONS) bad = "its memory region";
        else if (d4 < 1 || d4 > 6) bad = "its comparison";
        else if (d3 || d2) bad = "the form with an offset register";
        break;
    case 2: if (width > 1) bad = "this form of opcode 2"; break;
    case 3: n = width == 0 ? 2 : 1; if (width > 1) bad = "this form of opcode 3"; break;
    case 4: n = 3; break;
    case 5:
        n = 2, sized = 1;
        if (d3 > 1) bad = "the form with a second register";
        else if (!d3 && d5 >= MOD_CHEAT_REGIONS) bad = "its memory region";
        break;
    case 6: n = 3, sized = 1; break;
    case 7: n = 2, sized = 1; if (d3 > 4) bad = "its operation"; break;
    case 8: break;
    case 9: n = d2 ? (width == 8 ? 3 : 2) : 1, sized = 1; if (d5 > 9) bad = "its operation (the float ones are not supported)"; break;
    case 0xA:
        sized = 1;
        n = d2 == 2 || d2 == 4 || d2 == 5 ? 2 : 1;
        if (d2 > 5) bad = "its address form";
        else if (d2 >= 3 && d1 >= MOD_CHEAT_REGIONS) bad = "its memory region";
        break;
    case 0xC:
        switch (width) {   /* (the second digit: which of the C opcodes) */
        case 0:   /* C0TcSX..: the width is the third digit */
            if (!mod_cheat_width_ok(d5)) {
                if (why) snprintf(why, cap, "opcode %08X has a bad width", f);
                return 0;
            }
            n = d2 == 0 || d2 == 2 ? 2 : d2 == 4 ? (d5 == 8 ? 3 : 2) : 1;
            if (d4 < 1 || d4 > 6) bad = "its comparison";
            else if (d2 > 5) bad = "its operand";
            else if (d2 <= 1 && d1 >= MOD_CHEAT_REGIONS) bad = "its memory region";
            break;
        case 1: if (d1 > 3) bad = "its operation"; break;
        case 2: if (d5 > 3) bad = "its operation"; break;
        case 3: break;
        default: bad = "this opcode"; break;
        }
        break;
    default: bad = "this opcode"; break;
    }
    if (bad) {
        if (why) snprintf(why, cap, "opcode %08X is not supported (%s)", f, bad);
        return 0;
    }
    if (sized && !mod_cheat_width_ok(width)) {
        if (why) snprintf(why, cap, "opcode %08X has a bad width", f);
        return 0;
    }
    if (n > left) {
        if (why) snprintf(why, cap, "opcode %08X is cut short (%d of its %d words)", f, left, n);
        return 0;
    }
    return n;
}

/* 1 when every opcode of a cheat is one the interpreter runs, else 0 with the reason in why */
static inline int mod_cheat_check(const uint32_t *w, int n, char *why, size_t cap) {
    for (int i = 0; i < n;) {
        const int len = mod_cheat_op_len(w + i, n - i, why, cap);
        if (!len) return 0;
        i += len;
    }
    return 1;
}

/* 1 when the cheat has an opcode 0 that stores into a module's code (region main, the offset register left out of the
   sum: it is 0 unless a cheat before it set it): *at is that address. Such a cheat is a patch of the game's code */
static inline int mod_cheat_stores_code(const uint32_t *w, int n, const ModCheatLayout *l, uint64_t main_base, uint64_t *at) {
    for (int i = 0; i < n;) {
        const int len = mod_cheat_op_len(w + i, n - i, NULL, 0);
        if (!len) return 0;
        if (w[i] >> 28 == 0 && ((w[i] >> 20) & 0xF) == MOD_CHEAT_MAIN) {
            const uint64_t a = main_base + (((uint64_t)(w[i] & 0xFF) << 32) | w[i + 1]);
            if (mod_cheat_in_code(l, a, (w[i] >> 24) & 0xF)) {
                if (at) *at = a;
                return 1;
            }
        }
        i += len;
    }
    return 0;
}

/* ---- the interpreter ---- */
typedef struct ModCheatState {   /* one file's */
    uint64_t reg[16], saved[16];   /* 0 at the start of a frame (mod_cheat_frame) */
    uint64_t stat[0x100];          /* the static registers: they stay */
} ModCheatState;
static inline void mod_cheat_frame(ModCheatState *s) {
    memset(s->reg, 0, sizeof s->reg);
    memset(s->saved, 0, sizeof s->saved);
}

enum {
    MOD_CHEAT_RAN,          /* to its end */
    MOD_CHEAT_STOP_READ,    /* stopped at a read outside the game's memory (ModCheatRun.addr) */
    MOD_CHEAT_STOP_STEPS,   /* stopped after MOD_CHEAT_STEPS opcodes */
    MOD_CHEAT_STOP_BAD      /* an opcode the check refuses (the caller runs checked cheats only) */
};
typedef struct ModCheatRun {
    int steps, writes, dropped;   /* opcodes run; stores done; stores the memory refused */
    uint64_t addr;                /* the read that stopped it, or the last store refused */
} ModCheatRun;

static inline uint64_t mod_cheat_mask(unsigned width, uint64_t v) { return width >= 8 ? v : v & ((1ULL << (8 * width)) - 1); }
static inline int mod_cheat_compare(unsigned c, uint64_t a, uint64_t b) {
    switch (c) {
    case 1: return a > b;
    case 2: return a >= b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a == b;
    case 6: return a != b;
    default: return 0;
    }
}
static inline uint64_t mod_cheat_math(unsigned op, uint64_t a, uint64_t b) {
    switch (op) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return b < 64 ? a << b : 0;
    case 4: return b < 64 ? a >> b : 0;
    case 5: return a & b;
    case 6: return a | b;
    case 7: return ~a;
    case 8: return a ^ b;
    default: return a;   /* (9: the value itself) */
    }
}
/* From i, inside an if that is being skipped: the place after its end, or (to_else) after its else */
static inline int mod_cheat_skip(const uint32_t *w, int n, int i, int to_else) {
    int depth = 0;
    while (i < n) {
        const int len = mod_cheat_op_len(w + i, n - i, NULL, 0);
        if (!len) return n;
        const uint32_t f = w[i];
        i += len;
        if (mod_cheat_begins_if(f)) depth++;
        else if (f >> 28 == 2 && ((f >> 24) & 0xF) == 0) {
            if (!depth) return i;
            depth--;
        } else if (f >> 28 == 2 && !depth && to_else) return i;
    }
    return n;
}
static inline void mod_cheat_store(const ModCheatMem *m, ModCheatRun *r, uint64_t a, unsigned width, uint64_t v) {
    if (m->write(m->ctx, a, width, &v)) r->writes++;   /* (the value's low bytes: the host is little-endian, as the game) */
    else r->dropped++, r->addr = a;
}

/* One cheat's code run once: w its n words, s its file's state, keys the buttons held (nn::hid's bits, as the format's
   masks). Returns MOD_CHEAT_*; *r says what it did */
static inline int mod_cheat_run(const uint32_t *w, int n, ModCheatState *s, const ModCheatMem *m, uint64_t keys, ModCheatRun *r) {
    int loop_top[16] = {0};
    uint64_t *reg = s->reg;
    memset(r, 0, sizeof *r);
    for (int i = 0; i < n;) {
        if (++r->steps > MOD_CHEAT_STEPS) return MOD_CHEAT_STOP_STEPS;
        const int len = mod_cheat_op_len(w + i, n - i, NULL, 0);
        if (!len) return MOD_CHEAT_STOP_BAD;
        const uint32_t f = w[i];
        const unsigned t = f >> 28, width = (f >> 24) & 0xF, d5 = (f >> 20) & 0xF, d4 = (f >> 16) & 0xF, d3 = (f >> 12) & 0xF,
                       d2 = (f >> 8) & 0xF, d1 = (f >> 4) & 0xF, d0 = f & 0xF;
        const uint64_t rel = ((uint64_t)(f & 0xFF) << 32) | (len > 1 ? w[i + 1] : 0);   /* (the 40-bit offset of 0, 1 and 5) */
        const uint64_t small = ((uint64_t)d0 << 32) | (len > 1 ? w[i + 1] : 0);         /* (the 36-bit one of A and C0) */
        uint64_t v = 0;
        int next = i + len;
        switch (t) {
        case 0:
            mod_cheat_store(m, r, m->base[d5] + rel + reg[d4], width, width == 8 ? (uint64_t)w[i + 2] << 32 | w[i + 3] : w[i + 2]);
            break;
        case 1:
            if (!m->read(m->ctx, m->base[d5] + rel, width, &v) ||
                !mod_cheat_compare(d4, v, mod_cheat_mask(width, width == 8 ? (uint64_t)w[i + 2] << 32 | w[i + 3] : w[i + 2])))
                next = mod_cheat_skip(w, n, next, 1);
            break;
        case 2:   /* (an end: nothing to do. An else reached by running the if's own part: on to its end) */
            if (width == 1) next = mod_cheat_skip(w, n, next, 0);
            break;
        case 3:
            if (width == 0) {
                reg[d4] = w[i + 1];
                loop_top[d4] = next;
            } else if (--reg[d4]) {
                next = loop_top[d4];
            }
            break;
        case 4:
            reg[d4] = (uint64_t)w[i + 1] << 32 | w[i + 2];
            break;
        case 5: {
            const uint64_t a = d3 ? reg[d4] + rel : m->base[d5] + rel;
            if (!m->read(m->ctx, a, width, &v)) {
                r->addr = a;
                return MOD_CHEAT_STOP_READ;
            }
            memcpy(&reg[d4], &v, width);   /* (the low bytes: the rest of the register stays, as Atmosphere's) */
            break;
        }
        case 6:
            mod_cheat_store(m, r, reg[d4] + (d2 ? reg[d1] : 0), width, (uint64_t)w[i + 1] << 32 | w[i + 2]);
            if (d3) reg[d4] += width;
            break;
        case 7:
            reg[d4] = mod_cheat_mask(width, mod_cheat_math(d3, reg[d4], w[i + 1]));
            break;
        case 8:
            if ((keys & (f & 0x0FFFFFFF)) != (f & 0x0FFFFFFF)) next = mod_cheat_skip(w, n, next, 1);
            break;
        case 9: {
            const uint64_t b = d2 ? mod_cheat_mask(width, width == 8 ? (uint64_t)w[i + 1] << 32 | w[i + 2] : w[i + 1]) : reg[d1];
            reg[d4] = mod_cheat_mask(width, mod_cheat_math(d5, reg[d3], b));
            break;
        }
        case 0xA: {
            const uint64_t a = d2 == 0 ? reg[d4] : d2 == 1 ? reg[d4] + reg[d1] : d2 == 2 ? reg[d4] + small
                             : d2 == 3 ? m->base[d1] + reg[d4] : d2 == 4 ? m->base[d1] + small : m->base[d1] + reg[d4] + small;
            mod_cheat_store(m, r, a, width, reg[d5]);
            if (d3) reg[d4] += width;
            break;
        }
        default:   /* 0xC: the check let nothing else through */
            if (width == 0) {   /* C0TcSX..: T is d5, c d4, S d3, X d2 */
                const uint64_t mine = mod_cheat_mask(d5, reg[d3]);
                uint64_t other = 0;
                int have = 1;
                if (d2 == 4) other = mod_cheat_mask(d5, d5 == 8 ? (uint64_t)w[i + 1] << 32 | w[i + 2] : w[i + 1]);
                else if (d2 == 5) other = mod_cheat_mask(d5, reg[d1]);
                else {
                    const uint64_t a = d2 == 0 ? m->base[d1] + small : d2 == 1 ? m->base[d1] + reg[d0]
                                     : d2 == 2 ? reg[d1] + small : reg[d1] + reg[d0];
                    have = m->read(m->ctx, a, d5, &other);
                }
                if (!have || !mod_cheat_compare(d4, mine, other)) next = mod_cheat_skip(w, n, next, 1);
            } else if (width == 1) {   /* C10D0Sx0: D is d4, S d2, x d1 */
                if (d1 == 0) reg[d4] = s->saved[d2];
                else if (d1 == 1) s->saved[d4] = reg[d2];
                else if (d1 == 2) s->saved[d4] = 0;
                else reg[d4] = 0;
            } else if (width == 2) {   /* C2x0XXXX: x is d5 */
                for (unsigned k = 0; k < 16; k++) {
                    if (!(f >> k & 1)) continue;
                    if (d5 == 0) reg[k] = s->saved[k];
                    else if (d5 == 1) s->saved[k] = reg[k];
                    else if (d5 == 2) s->saved[k] = 0;
                    else reg[k] = 0;
                }
            } else {   /* C3000XXx */
                const unsigned at = (f >> 4) & 0xFF;
                if (at < 0x80) reg[d0] = s->stat[at];
                else s->stat[at] = reg[d0];
            }
            break;
        }
        i = next;
    }
    return MOD_CHEAT_RAN;
}

/* ---- the file ---- */
typedef struct ModCheat {
    char name[MOD_CHEAT_NAME];   /* as the file has it, UTF-8, at most 63 bytes of it */
    int master;                  /* {name}: always runs */
    int line;                    /* the line its name is on */
    const uint32_t *words;
    int n;
    char why[MOD_CHEAT_WHY];     /* why it cannot run ("" when it can): a word that is none, an opcode */
} ModCheat;

static inline int mod_cheat_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v'; }

/* The cheats of a file's text (len bytes; a UTF-8 mark at its start is passed over), at most cap of them, their words
   in pool (pool_cap of them: a file with more loses its last cheats, *cut_short says so). What stands before the first
   name is passed over; in a cheat, anything but words of 8 hex digits refuses that cheat (why), and so does an opcode
   the interpreter does not run. Returns how many */
static inline int mod_cheat_parse(const char *text, size_t len, ModCheat *out, int cap, uint32_t *pool, int pool_cap, int *cut_short) {
    int n = 0, used = 0, line = 1;
    size_t i = len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3) ? 3 : 0;
    if (cut_short) *cut_short = 0;
    while (i < len) {
        const char ch = text[i];
        if (ch == '\n') line++;
        if (mod_cheat_space(ch) || !ch) {
            i++;
            continue;
        }
        if (ch == '[' || ch == '{') {   /* a name, up to its closing bracket on the same line */
            const char close = ch == '[' ? ']' : '}';
            size_t e = i + 1;
            while (e < len && text[e] != '\n' && text[e] != close) e++;
            if (e >= len || text[e] != close) {   /* (no closing bracket: not a name. The line is passed over) */
                if (n && !out[n - 1].why[0])
                    snprintf(out[n - 1].why, sizeof out[n - 1].why, "line %d: a name without its closing %c", line, close);
                i = e;
                continue;
            }
            if (n == cap) {
                if (cut_short) *cut_short = 1;
                break;
            }
            ModCheat *c = &out[n++];
            memset(c, 0, sizeof *c);
            size_t a = i + 1, b = e;
            while (a < b && mod_cheat_space(text[a])) a++;
            while (b > a && mod_cheat_space(text[b - 1])) b--;
            size_t k = b - a < sizeof c->name - 1 ? b - a : sizeof c->name - 1;
            while (k && k < b - a && ((unsigned char)text[a + k] & 0xC0) == 0x80) k--;   /* (not in the middle of a letter) */
            memcpy(c->name, text + a, k);
            c->master = ch == '{';
            c->line = line;
            c->words = pool + used;
            i = e + 1;
            continue;
        }
        size_t e = i;   /* a word */
        while (e < len && !mod_cheat_space(text[e]) && text[e] != '[' && text[e] != '{') e++;
        if (e == i) e++;
        if (n) {
            ModCheat *c = &out[n - 1];
            uint32_t v = 0;
            int hex = e - i == 8;
            for (size_t k = i; hex && k < e; k++) {
                const char d = text[k];
                const int digit = d >= '0' && d <= '9' ? d - '0' : d >= 'a' && d <= 'f' ? d - 'a' + 10 : d >= 'A' && d <= 'F' ? d - 'A' + 10 : -1;
                if (digit < 0) hex = 0;
                else v = v << 4 | (uint32_t)digit;
            }
            if (!hex) {
                if (!c->why[0]) {   /* (the first such word is the one named) */
                    char word[25];
                    const size_t wn = e - i < 24 ? e - i : 24;
                    memcpy(word, text + i, wn);
                    word[wn] = 0;
                    snprintf(c->why, sizeof c->why, "line %d: \"%s\" is not a code word (8 hex digits)", line, word);
                }
            } else if (used == pool_cap) {
                if (cut_short) *cut_short = 1;
                if (!c->why[0]) snprintf(c->why, sizeof c->why, "the file has too many code words");
            } else {
                pool[used++] = v;
                c->n++;
            }
        }
        i = e;
    }
    for (int k = 0; k < n; k++)
        if (!out[k].why[0]) mod_cheat_check(out[k].words, out[k].n, out[k].why, sizeof out[k].why);
    return n;
}

/* A cheat that does nothing: no code, or the lone end (20000000) cheat files give their headings */
static inline int mod_cheat_is_heading(const ModCheat *c) {
    return !c->why[0] && (c->n == 0 || (c->n == 1 && c->words[0] == 0x20000000u));
}

/* FNV-1a 32 of a string: a cheat's switch is kept by the number of "<mod>/<name>" (mod_cheats.c) */
static inline uint32_t mod_cheat_hash(const char *s) {
    uint32_t h = 0x811C9DC5u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x01000193u;
    return h;
}
#endif
