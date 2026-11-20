/* tests/test_fpx_hot.py's driver: the hot functions' float helpers (a64_fpx_hot_*: the fast path inline, the exact path
   called without the context) and the four-lane helpers lifted code calls (a64_vfpx_add32 / sub32 / mul32, a64_vfpx_fma32)
   against the exact bodies (recomp_fpe_*), lane by lane in order: the value, FPSR and the rest of the context.
   Built against the real a64.h with the a64_suyu.h / a64_suyu.c / a64_fpx.h the generators write, at the build's FP flags,
   at -march=x86-64-v3 (the binary32 FMA by the host's instruction) and -march=x86-64-v2 (its binary64 form; the four-lane
   multiply-add lane by lane).

   fpx_hot_test <n> <seed>     n random operand sets for every helper over random FPCR / FPSR states
   Exit 0: no mismatch. 1: a mismatch (the first few are printed). 3: the CPU lacks this build's -march. */
#include "a64.h"
#include <stdio.h>
#include <stdlib.h>
#include <cpuid.h>

static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint32_t r32(void) { return (uint32_t)(rnd() >> 16); }
static uint32_t rbelow(uint32_t n) { return (uint32_t)(((rnd() >> 32) * n) >> 32); }
static uint32_t mkf(uint32_t sign, int e, uint32_t frac) { return (sign << 31) | ((uint32_t)e << 23) | (frac & 0x7fffff); }
static uint64_t mkd(uint64_t sign, int e, uint64_t frac) { return (sign << 63) | ((uint64_t)e << 52) | (frac & UINT64_C(0xfffffffffffff)); }
static float F(uint32_t x) { float f; memcpy(&f, &x, 4); return f; }
static uint32_t B(float f) { uint32_t x; memcpy(&x, &f, 4); return x; }
static double D(uint64_t x) { double f; memcpy(&f, &x, 8); return f; }
static uint64_t BD(double f) { uint64_t x; memcpy(&x, &f, 8); return x; }

/* ---- operand classes and states (as tests/fpx_sticky_test.c's) ---- */
static uint32_t sparse_frac(void) {
    switch (rbelow(4)) {
    case 0: return 0;
    case 1: return (uint32_t)1 << rbelow(23);
    case 2: return r32() & 0x7ff000;
    default: return r32() & 0x7fffff;
    }
}
static uint32_t any32(void) {
    const uint32_t s = r32() & 1;
    switch (rbelow(16)) {
    case 0: return s << 31;
    case 1: return mkf(s, 0, r32() | 1);
    case 2: return mkf(s, 0, rbelow(8) + 1);
    case 3: return mkf(s, 0, 0x7fffff - rbelow(8));
    case 4: return mkf(s, 1 + rbelow(3), sparse_frac());
    case 5: return mkf(s, 254 - rbelow(3), sparse_frac());
    case 6: return mkf(s, 255, 0);
    case 7: return mkf(s, 255, (r32() | 1) & 0x7fffff);
    case 8: return mkf(s, 255, 0x400000 | (r32() & 0x3fffff));
    case 9: case 10: return mkf(s, 120 + rbelow(15), sparse_frac());
    default: return mkf(s, 1 + rbelow(254), r32());
    }
}
static uint64_t any64(void) {
    const uint64_t s = r32() & 1;
    const uint64_t fr = rnd() & UINT64_C(0xfffffffffffff), sp = rbelow(3) ? fr : (rbelow(2) ? 0 : UINT64_C(1) << rbelow(52));
    switch (rbelow(16)) {
    case 0: return s << 63;
    case 1: return mkd(s, 0, fr | 1);
    case 2: return mkd(s, 0, rbelow(8) + 1);
    case 3: return mkd(s, 0, UINT64_C(0xfffffffffffff) - rbelow(8));
    case 4: return mkd(s, 1 + (int)rbelow(3), sp);
    case 5: return mkd(s, 2046 - (int)rbelow(3), sp);
    case 6: return mkd(s, 2047, 0);
    case 7: return mkd(s, 2047, fr | 1);
    case 8: return mkd(s, 2047, UINT64_C(0x8000000000000) | fr);
    case 9: case 10: return mkd(s, 1016 + (int)rbelow(15), sp);
    default: return mkd(s, 1 + (int)rbelow(2046), fr);
    }
}
static void edge_pair32(uint32_t *a, uint32_t *b) {   /* a product near the bottom of the normal range or the overflow edge */
    const int target = (rbelow(8) == 0) ? 255 - (int)rbelow(3) : (int)rbelow(60) - 30;
    int ea = 1 + (int)rbelow(253);
    int eb = target - ea + 127;
    if (eb < 1 || eb > 254) { eb = 1 + (int)rbelow(254); ea = target - eb + 127; }
    if (ea < 1) ea = rbelow(4) ? 1 : 0;
    if (ea > 254) ea = 254;
    *a = mkf(r32() & 1, ea, rbelow(2) ? sparse_frac() : r32());
    *b = mkf(r32() & 1, eb, rbelow(2) ? sparse_frac() : r32());
}
static uint32_t addend32(uint32_t a, uint32_t b) {
    const double p = (double)F(a) * (double)F(b);
    switch (rbelow(10)) {
    case 0: return any32();
    case 1: return B((float)-p);
    case 2: return B((float)-p) + (uint32_t)((int)rbelow(9) - 4);
    case 3: return r32() & 0x80000000u;
    case 4: return B((float)p) ^ (rbelow(2) ? 0x80000000u : 0);
    case 5: return mkf(r32() & 1, 0, r32());
    case 6: return mkf(r32() & 1, 1 + rbelow(4), r32());
    default: return mkf(r32() & 1, 1 + rbelow(254), r32());
    }
}
static uint64_t addend64(uint64_t a, uint64_t b) {
    const double p = D(a) * D(b);
    switch (rbelow(8)) {
    case 0: return any64();
    case 1: return BD(-p);
    case 2: return BD(-p) + (uint64_t)((int)rbelow(9) - 4);
    case 3: return (uint64_t)(r32() & 1) << 63;
    default: return mkd(r32() & 1, 1 + (int)rbelow(2046), rnd());
    }
}
static void gen_state(Ctx *c) {
    uint64_t cr = 0, sr = 0;
    if (rbelow(2)) cr |= 1u << 24;                        /* FZ */
    if (rbelow(10) == 0) cr |= 1u << 25;                  /* DN (gate closed) */
    if (rbelow(10) == 0) cr |= (uint64_t)rbelow(4) << 22; /* RMode */
    if (rbelow(40) == 0) cr |= r32() & 0x07ff9f07u;       /* anything else */
    if (rbelow(50) == 0) cr |= UINT64_C(1) << 32;         /* the host's kill switch */
    switch (rbelow(6)) {
    case 0: sr = 0x9f; break;                             /* every cumulative flag */
    case 1: sr = 0x10; break;                             /* IXC only */
    case 2: sr = 0; break;
    default: sr = (r32() & 0x9f) | (rbelow(4) ? 0x10 : 0); break;
    }
    memset(c, 0, sizeof *c);
    c->fpcr = cr;
    c->fpsr = sr;
    c->x[5] = 0x1234;   /* (a helper must leave the rest alone) */
    c->vreg[7][1] = 0x5678;
}

static unsigned long long checks, bad, per_bad[16], per_n[16];
static const char *const NAMES[16] = {"add32", "sub32", "div32", "sqrt32", "mul32", "fma32", "recps32", "rsqrts32", "add64", "sub64",
                                      "div64", "sqrt64", "mul64", "fma64", "vector add/sub/mul", "vector mul/fma by element"};
static void report(int k, const Ctx *c0, const Ctx *got, const Ctx *want, const uint64_t *g, const uint64_t *w, int n, const uint64_t *in, int nin) {
    int diff = memcmp(got, want, sizeof *got) != 0;
    for (int i = 0; i < n; i++) diff |= g[i] != w[i];
    checks++, per_n[k]++;
    if (!diff) return;
    bad++, per_bad[k]++;
    if (bad <= 12) {
        printf("MISMATCH %s: fpcr %#llx fpsr %#llx; in", NAMES[k], (unsigned long long)c0->fpcr, (unsigned long long)c0->fpsr);
        for (int i = 0; i < nin; i++) printf(" %#llx", (unsigned long long)in[i]);
        printf("; got");
        for (int i = 0; i < n; i++) printf(" %#llx", (unsigned long long)g[i]);
        printf(" fpsr %#llx; want", (unsigned long long)got->fpsr);
        for (int i = 0; i < n; i++) printf(" %#llx", (unsigned long long)w[i]);
        printf(" fpsr %#llx\n", (unsigned long long)want->fpsr);
    }
}
#define SCALAR(k, hot_call, exact_call, ...) do { \
    Ctx c0, c1, c2; gen_state(&c0); c1 = c0; c2 = c0; \
    const uint64_t in_[] = {__VA_ARGS__}; \
    uint64_t g_ = hot_call, w_ = exact_call; \
    report(k, &c0, &c1, &c2, &g_, &w_, 1, in_, (int)(sizeof in_ / sizeof in_[0])); } while (0)

static void vec(int k, const Ctx *c0, Ctx *got, Ctx *want, const uint32_t g[4], const uint32_t w[4], const uint32_t *in, int nin) {
    uint64_t gg[4], ww[4], ii[12];
    for (int i = 0; i < 4; i++) gg[i] = g[i], ww[i] = w[i];
    for (int i = 0; i < nin; i++) ii[i] = in[i];
    report(k, c0, got, want, gg, ww, 4, ii, nin);
}

int main(int argc, char **argv) {
    unsigned a_, b_, c_, d_;
#if defined(__FMA__)
    if (!__get_cpuid(1, &a_, &b_, &c_, &d_) || !(c_ & (1u << 12)) || !(c_ & (1u << 28))) return 3;
    if (!__get_cpuid_count(7, 0, &a_, &b_, &c_, &d_) || !(b_ & (1u << 5))) return 3;
#else
    if (!__get_cpuid(1, &a_, &b_, &c_, &d_) || !(c_ & (1u << 20)) || !(c_ & (1u << 23))) return 3;
#endif
    const unsigned long long n = argc > 1 ? strtoull(argv[1], NULL, 10) : 1000000;
    rs = argc > 2 ? strtoull(argv[2], NULL, 10) : 20261114;
    if (!rs) rs = 1;
    for (unsigned long long it = 0; it < n; it++) {
        uint32_t a = any32(), b = any32();
        if (rbelow(3) == 0) edge_pair32(&a, &b);
        const uint32_t z = addend32(a, b), zs = (a ^ b) & 0x80000000u;
        SCALAR(0, a64_fpx_hot_add32_add32(&c1, a, b), recomp_fpe_add32(&c2, a, b), a, b);
        SCALAR(1, a64_fpx_hot_sub32_sub32(&c1, a, b), recomp_fpe_sub32(&c2, a, b), a, b);
        SCALAR(2, a64_fpx_hot_div32_div32(&c1, a, b), recomp_fpe_div32(&c2, a, b), a, b);
        SCALAR(3, a64_fpx_hot_sqrt32_sqrt32(&c1, a), recomp_fpe_sqrt32(&c2, a), a);
        SCALAR(4, a64_fpx_hot_mul32_fma32(&c1, a, b, zs), recomp_fpe_fma32(&c2, a, b, zs), a, b, zs);
        SCALAR(5, a64_fpx_hot_fma32_fma32(&c1, a, b, z), recomp_fpe_fma32(&c2, a, b, z), a, b, z);
        SCALAR(6, a64_fpx_hot_recps32_recps32(&c1, a, b), recomp_fpe_recps32(&c2, a, b), a, b);
        SCALAR(7, a64_fpx_hot_rsqrts32_rsqrts32(&c1, a, b), recomp_fpe_rsqrts32(&c2, a, b), a, b);
        {
            const uint64_t p = any64(), q = any64(), r = addend64(p, q), rz = (p ^ q) & (UINT64_C(1) << 63);
            SCALAR(8, a64_fpx_hot_add64_add64(&c1, p, q), recomp_fpe_add64(&c2, p, q), p, q);
            SCALAR(9, a64_fpx_hot_sub64_sub64(&c1, p, q), recomp_fpe_sub64(&c2, p, q), p, q);
            SCALAR(10, a64_fpx_hot_div64_div64(&c1, p, q), recomp_fpe_div64(&c2, p, q), p, q);
            SCALAR(11, a64_fpx_hot_sqrt64_sqrt64(&c1, p), recomp_fpe_sqrt64(&c2, p), p);
            SCALAR(12, a64_fpx_hot_mul64_fma64(&c1, p, q, rz), recomp_fpe_fma64(&c2, p, q, rz), p, q, rz);
            SCALAR(13, a64_fpx_hot_fma64_fma64(&c1, p, q, r), recomp_fpe_fma64(&c2, p, q, r), p, q, r);
        }
        /* ---- four lanes: mostly ordinary numbers (so the one-op path is taken), now and then any operand in a lane ---- */
        uint32_t vn[4], vm[4], vz[4];
        for (int i = 0; i < 4; i++) {
            vn[i] = rbelow(6) ? mkf(r32() & 1, 100 + rbelow(60), r32()) : any32();
            vm[i] = rbelow(6) ? mkf(r32() & 1, 100 + rbelow(60), r32()) : any32();
            vz[i] = rbelow(6) ? mkf(r32() & 1, 100 + rbelow(60), r32()) : addend32(vn[i], vm[i]);
        }
        const uint32_t el = rbelow(8) ? mkf(r32() & 1, 100 + rbelow(60), r32()) : any32();
        for (int op = 0; op < 3; op++) {   /* straight add, sub (4 and 2 lanes), mul */
            for (int k = 4; k >= (op == 2 ? 4 : 2); k -= 2) {
                Ctx c0, c1, c2; gen_state(&c0); c1 = c0; c2 = c0;
                uint64_t dn[2], dm[2], dd[2] = {0x1111111111111111ULL, 0x2222222222222222ULL};
                uint32_t g[4], w[4] = {0};
                memcpy(dn, vn, 16); memcpy(dm, vm, 16);
                const int alias = rbelow(4) == 0;   /* (the destination is the first source's register) */
                uint64_t *dst = alias ? dn : dd;
                if (op == 0) a64_vfpx_add32(&c1, dst, dn, dm, k);
                else if (op == 1) a64_vfpx_sub32(&c1, dst, dn, dm, k);
                else a64_vfpx_mul32(&c1, dst, dn, dm, 4);
                memcpy(g, dst, 16);
                for (int j = 0; j < k; j++)
                    w[j] = (uint32_t)(op == 0 ? recomp_fpe_add32(&c2, vn[j], vm[j]) : op == 1 ? recomp_fpe_sub32(&c2, vn[j], vm[j])
                                              : recomp_fpe_fma32(&c2, vn[j], vm[j], (vn[j] ^ vm[j]) & 0x80000000u));
                uint32_t in[8]; memcpy(in, vn, 16); memcpy(in + 4, vm, 16);
                vec(14, &c0, &c1, &c2, g, w, in, 8);
            }
        }
        for (int op = 0; op < 3; op++) {   /* by element, as vectorize_elem writes it: mul, fmla, fmls */
            Ctx c0, c1, c2; gen_state(&c0); c1 = c0; c2 = c0;
            uint64_t dn[2], dd[2], e[2];
            uint32_t g[4], w[4];
            memcpy(dn, vn, 16); memcpy(dd, vz, 16);
            e[0] = e[1] = (uint64_t)el << 32 | el;
            const int alias = op == 0 && rbelow(4) == 0;
            uint64_t *dst = alias ? dn : dd;
            if (op == 0) a64_vfpx_mul32(&c1, dst, dn, e, 4);
            else a64_vfpx_fma32(&c1, dst, dn, e, op == 2);
            memcpy(g, dst, 16);
            for (int j = 0; j < 4; j++) {
                const uint32_t x = op == 2 ? vn[j] ^ 0x80000000u : vn[j];
                w[j] = (uint32_t)recomp_fpe_fma32(&c2, x, el, op == 0 ? (vn[j] ^ el) & 0x80000000u : vz[j]);
            }
            uint32_t in[9]; memcpy(in, vn, 16); memcpy(in + 4, vz, 16); in[8] = el;
            vec(15, &c0, &c1, &c2, g, w, in, 9);
        }
    }
    for (int k = 0; k < 16; k++)
        if (per_bad[k]) printf("  %s: %llu of %llu mismatch\n", NAMES[k], per_bad[k], per_n[k]);
    printf("%llu checks, %llu mismatches\n", checks, bad);
    return bad ? 1 : 0;
}
