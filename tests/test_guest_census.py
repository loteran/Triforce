"""The guest memory census (host/guest_census.h, host/hos_mem.c hos_mem_note, census_release, hos_mem_commit_begin and _end,
discard_pages and hos_region_free, host/mkw_glue.c; no game).

Triforce's log said only the guest window's committed total (8.7 -> 14.6 GB over an owner's session, the heap 6.9 GB all
through) and nothing of which part held it. The census keeps a bit a 4 KB page of what the runtime handed out (held), of
what the game holds as its own pages and physical maps (game), and of what it let go that stays committed (kept: zeroed
under an import, or discarded), and every few seconds a "mem: census" line counts them by kind. The give-back
(HOS_MEM_GIVE_BACK) decommits the kept pages once no import holds them.

Compiles guest_census.h with hos_mem.c's hos_mem_note and census_release cut out of the source, against stand-ins, and runs:
the bit operations against a model of a page each (random ranges across words and the 1 GB chunks), the runs the give-back
walks, the counts by kind of an authored layout, the give-back's pass (runs split, refused, the bound, the cursor), the notes
of each place that commits or lets go, and census_release's rule: a run is decommitted only while every page of it is kept
and none was handed out again. (Triforce) Also: a walk steps over the chunks never made (the census's line walked the whole
512 GB window, ~10 million words a pass); hos_commit's pages held before its commit and kept no longer only once it is made,
put back as they were when it fails; a freed address region discarded, and noted, while still listed, so that a region asked
for meanwhile is not given its pages; the large pages a discard leaves alone, and Linux's discards noted only as what they
did. All twice: as built for Windows and for Linux (_WIN32 taken away). Then the same with mutations of the production code
and of guest_census.h, each of which must fail. Reads the source for the rest: each place notes what it did, and in the right
order; the line is on and the give-back off by default.

    python -I -B tests/test_guest_census.py [--cxx clang++]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker, what):
    if text.count(marker) != 1:
        raise SystemExit("FAIL: not found once in %s: %s" % (what, marker))
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


FIXTURE = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "guest_census.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ---- stand-ins for hos_mem.c's world */
@@ENUM@@
static GcMap census;
static uint64_t census_touched, census_zeroed, census_discarded;
static int locks, unlocks;
#define CENSUS_LOCK() (locks++)
#define CENSUS_UNLOCK() (unlocks++)
int hos_mem_census_on(void) { return 1; }
uint64_t hos_large_lo, hos_large_hi;
/* (guest addresses: window_lo is at scratch, for the one place that writes there, Linux's zeroing under an import) */
static unsigned char scratch[1 << 20];
static uint64_t window_lo;
#define GP(a) ((void *)((uintptr_t)scratch + (uintptr_t)((a) - window_lo)))
static uint64_t guest_of(const void *p) { return (uint64_t)((uintptr_t)p - (uintptr_t)scratch) + window_lo; }
typedef size_t SIZE_T;
#define MEM_DECOMMIT 0x4000
static int decommits;
static uint64_t dec_lo, dec_hi;
static int VirtualFree(void *p, SIZE_T n, unsigned type) {
    decommits++, dec_lo = guest_of(p), dec_hi = dec_lo + n;
    return type == MEM_DECOMMIT;
}
int hos_posix_guest_discard(void *p, size_t n) { return VirtualFree(p, n, MEM_DECOMMIT) ? 0 : -1; }

/* (the regions' list and lock: hos_region_free's) */
@@RANGE@@
static Range *regions;
static int nregions, lock_depth;
#define LOCK() (lock_depth++)
#define UNLOCK() (lock_depth--)
static int hos_guest_range(uint64_t a, uint64_t n) { (void)a, (void)n; return 1; }
static int hos_guest_io_begin(const void *p, uint64_t n) { (void)p, (void)n; return 0; }
static void hos_guest_io_end(int io) { (void)io; }
/* A discard as it happens: whether a listed region still holds its pages (so that hos_region_alloc would not hand them out
   meanwhile), and whether the regions' lock is held */
static int seen, seen_listed, seen_locked;
static void discard_seen(uint64_t lo, uint64_t hi) {
    seen++, seen_listed = 0, seen_locked = lock_depth;
    for (int i = 0; i < nregions; i++)
        if (regions[i].addr < hi && lo < regions[i].addr + regions[i].size) seen_listed = 1;
}
/* (Windows: DiscardVirtualMemory, or MEM_RESET) */
typedef unsigned long DWORD;
typedef void *PVOID;
#define WINAPI
#define ERROR_SUCCESS 0
#define MEM_RESET 0x80000
#define PAGE_READWRITE 4
static DWORD fake_discard(PVOID p, SIZE_T n) { discard_seen(guest_of(p), guest_of(p) + n); return ERROR_SUCCESS; }
static void *GetModuleHandleA(const char *name) { (void)name; return (void *)1; }
static void *GetProcAddress(void *module, const char *name) { (void)module, (void)name; return (void *)fake_discard; }
static void *VirtualAlloc(void *p, SIZE_T n, unsigned type, unsigned protect) {
    (void)type, (void)protect;
    discard_seen(guest_of(p), guest_of(p) + n);
    return p;
}
/* (Linux: asked first whether an import holds the pages) */
static int imported_now;
int rx_guest_imported(uint64_t lo, uint64_t hi) { discard_seen(lo, hi); return imported_now; }

#ifdef FIXTURE_POSIX   /* (as built for Linux) */
#undef _WIN32
#endif
@@PRODUCTION@@

#define MB (1ULL << 20)
#define GB (1ULL << 30)

/* ---- the bit operations against a model: a byte a page over the first 3 GB */
static uint64_t rnd_state = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void) { rnd_state ^= rnd_state << 13, rnd_state ^= rnd_state >> 7, rnd_state ^= rnd_state << 17; return rnd_state; }
enum { MODEL_PAGES = (int)((3 * GB) >> 12) };
static unsigned char model[GC_MAPS][MODEL_PAGES];
static uint64_t model_count(int map, uint64_t lo, uint64_t hi) {
    uint64_t n = 0;
    for (uint64_t p = (lo + 4095) >> 12; p < (hi >> 12); p++) n += model[map][p];
    return n << 12;
}
static void test_model(void) {
    GcMap m;
    memset(&m, 0, sizeof m);
    for (int i = 0; i < 4000; i++) {
        /* (ranges near the chunk edges and word edges, of a page to a few MB, some not page aligned) */
        const uint64_t edge = (1 + rnd() % 2) * GB;
        uint64_t lo = (rnd() % 3 == 0) ? edge - (rnd() % (4 * MB)) : rnd() % (3 * GB - 8 * MB);
        uint64_t hi = lo + 1 + rnd() % (rnd() % 4 == 0 ? 6 * MB : 300000);
        if (rnd() % 2) lo &= ~0xFFFULL, hi &= ~0xFFFULL;
        const int map = (int)(rnd() % GC_MAPS), op = (int)(rnd() % 3);
        uint64_t want = 0;
        const uint64_t plo = (lo + 4095) >> 12, phi = hi >> 12;
        if (op == 2) {
            const int to = (map + 1) % GC_MAPS;
            for (uint64_t p = plo; p < phi; p++)
                if (model[map][p]) want += 4096, model[map][p] = 0, model[to][p] = 1;
            const uint64_t got = gc_move(&m, map, to, lo, hi);
            CHECK(got == want, "gc_move %d->%d [%#llx, %#llx): %llu bytes, the model %llu", map, to, (unsigned long long)lo,
                  (unsigned long long)hi, (unsigned long long)got, (unsigned long long)want);
        } else {
            for (uint64_t p = plo; p < phi; p++)
                if (model[map][p] != op) want += 4096, model[map][p] = (unsigned char)op;
            const uint64_t got = gc_change(&m, map, lo, hi, op);
            CHECK(got == want, "gc_change map %d [%#llx, %#llx) to %d: %llu bytes, the model %llu", map, (unsigned long long)lo,
                  (unsigned long long)hi, op, (unsigned long long)got, (unsigned long long)want);
        }
        const uint64_t qlo = i % 500 == 0 ? 0 : rnd() % (3 * GB), qhi = i % 500 == 0 ? 3 * GB : qlo + rnd() % (16 * MB);
        const int qm = (int)(rnd() % GC_MAPS);
        const uint64_t c = gc_count(&m, qm, qlo, qhi > 3 * GB ? 3 * GB : qhi), w = model_count(qm, qlo, qhi > 3 * GB ? 3 * GB : qhi);
        CHECK(c == w, "gc_count map %d [%#llx, %#llx): %llu, the model %llu", qm, (unsigned long long)qlo, (unsigned long long)qhi,
              (unsigned long long)c, (unsigned long long)w);
        if (fails) return;
    }
    /* every run of every map, against the model's runs, with and without a bound on a run's length */
    for (int map = 0; map < GC_MAPS; map++)
        for (int bounded = 0; bounded < 2; bounded++) {
            const uint64_t max = bounded ? 64 * 4096 + 4096 * 3 : 0;
            uint64_t at = 0, lo, hi, p = 0, runs = 0;
            while (gc_next_run(&m, map, at, 3 * GB, max, &lo, &hi)) {
                while (p < MODEL_PAGES && !model[map][p]) p++;
                uint64_t e = p;
                while (e < MODEL_PAGES && model[map][e] && (!max || ((e - p) << 12) < max)) e++;
                CHECK(lo == p << 12 && hi == e << 12, "map %d run %llu: [%#llx, %#llx), the model's [%#llx, %#llx)", map,
                      (unsigned long long)runs, (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)(p << 12),
                      (unsigned long long)(e << 12));
                if (fails) return;
                p = e, at = hi, runs++;
            }
            while (p < MODEL_PAGES && !model[map][p]) p++;
            CHECK(p == MODEL_PAGES, "map %d: the runs ended at %#llx, the model has a page set at %#llx", map,
                  (unsigned long long)(at), (unsigned long long)(p << 12));
        }
}

/* ---- the counts by kind of an authored layout */
static void test_counts(void) {
    GcMap m;
    memset(&m, 0, sizeof m);
    const uint64_t heap = 0x1000000000ULL;
    gc_change(&m, GC_HELD, 0x01000000, 0x07000000, 1);                 /* the system region, 96 MB */
    gc_change(&m, GC_HELD, 0x80000000ULL, 0x80000000ULL + 96 * MB, 1);  /* the program */
    gc_change(&m, GC_HELD, 0x10000000, 0x10000000 + 2 * MB, 1);        /* a region in the hole below the heap */
    gc_change(&m, GC_HELD, heap, heap + 64 * MB, 1);                    /* the heap */
    const uint64_t r1 = 0x7F00000000ULL, r2 = 0x7E00000000ULL;
    gc_change(&m, GC_HELD, r1, r1 + 32 * MB, 1);                        /* a region, committed whole */
    gc_change(&m, GC_GAME, r1, r1 + 8 * MB, 1);                         /* its pages the game took */
    gc_change(&m, GC_HELD, r2, r2 + 16 * MB, 1);
    gc_change(&m, GC_HELD, r2 + 16 * MB, r2 + 20 * MB, 0);
    gc_change(&m, GC_HELD, 0x3000000000ULL, 0x3000000000ULL + 10 * MB, 1);   /* physical maps */
    gc_change(&m, GC_GAME, 0x3000000000ULL, 0x3000000000ULL + 10 * MB, 1);
    gc_change(&m, GC_HELD, 0x7D00000000ULL, 0x7D00000000ULL + 0x10000, 1);   /* a first touch */
    gc_change(&m, GC_KEPT, 0x7C00000000ULL, 0x7C00000000ULL + 3 * MB, 1);
    const uint64_t regions[3][2] = {{r1, 32 * MB}, {r2, 20 * MB}, {0x10000000, 2 * MB}};
    GcCensus c;
    memset(&c, 0, sizeof c);
    gc_census_count(&m, regions, 3, heap, 64 * MB, &c);
    CHECK(c.low == 192 * MB, "the program and system: %llu MB, want 192", (unsigned long long)(c.low >> 20));
    CHECK(c.heap == 64 * MB && c.heap_size == 64 * MB, "the heap: %llu MB", (unsigned long long)(c.heap >> 20));
    CHECK(c.regions == 50 * MB && c.region_span == 54 * MB && c.nregions == 3, "regions: %llu MB of %llu in %lld",
          (unsigned long long)(c.regions >> 20), (unsigned long long)(c.region_span >> 20), (long long)c.nregions);
    CHECK(c.region_game == 8 * MB, "the game's pages in the regions: %llu MB", (unsigned long long)(c.region_game >> 20));
    CHECK(c.outside == 10 * MB + 0x10000, "outside the regions: %llu bytes", (unsigned long long)c.outside);
    CHECK(c.outside_game == 10 * MB, "the game's outside the regions: %llu MB", (unsigned long long)(c.outside_game >> 20));
    CHECK(c.kept == 3 * MB, "kept: %llu MB", (unsigned long long)(c.kept >> 20));
    CHECK(c.held == c.low + c.heap + c.regions + c.outside, "the parts (%llu) are not the whole (%llu)",
          (unsigned long long)(c.low + c.heap + c.regions + c.outside), (unsigned long long)c.held);
    c.budget = 9809 * MB, c.program = 96 * MB;
    c.proc_commit = c.proc_peak = c.working_set = c.sys_commit = c.sys_limit = c.ram_avail = c.ram_total = 0x7FFFFFFFFFFFFFFFLL;
    c.touched = c.given_runs = c.refused = ~0ULL;
    c.kept_import = c.kept_discard = c.given_back = 1ULL << 50;
    char line[1024];
    const int n = gc_line(line, sizeof line, &c);
    CHECK(n > 0 && n < 1000, "the line is %d characters with the largest numbers (hos_log's line holds 1024)", n);
    CHECK(strstr(line, "census (MB): handed out ") == line && strstr(line, "heap 64 of 64") && strstr(line, "the game's own count 178 of its 9809"),
          "the line: %s", line);
    c.sys_commit = c.sys_limit = c.ram_avail = c.ram_total = c.proc_commit = c.proc_peak = c.working_set = -1;
    gc_line(line, sizeof line, &c);
    CHECK(strstr(line, "the PC's commit ?, RAM -1 of -1 available") != NULL, "not known: %s", line);
}

/* ---- the give-back's pass */
static GcMap gm;
static uint64_t held_lo = 0, held_hi = 0, offered[64][2];
static int noffered;
static int fake_release(void *ctx, uint64_t lo, uint64_t hi) {
    (void)ctx;
    if (noffered < 64) offered[noffered][0] = lo, offered[noffered][1] = hi;
    noffered++;
    if (lo < held_hi && held_lo < hi) return 0;   /* (an import holds it) */
    gc_change(&gm, GC_KEPT, lo, hi, 0);
    return 1;
}
static void test_give_back(void) {
    memset(&gm, 0, sizeof gm);
    const uint64_t a = 0x7F00000000ULL;
    gc_change(&gm, GC_KEPT, a, a + 40 * MB, 1);                 /* 40 MB: three runs of 16, 16, 8 */
    gc_change(&gm, GC_KEPT, a + 64 * MB, a + 65 * MB, 1);       /* held by an import */
    gc_change(&gm, GC_KEPT, 0x3000000000ULL, 0x3000000000ULL + 4 * MB, 1);
    held_lo = a + 64 * MB, held_hi = a + 65 * MB;
    uint64_t cursor = 0x7000000000ULL, runs = 0, refused = 0;
    uint64_t given = gc_give_back(&gm, 0, &cursor, 16 * MB, 20 * MB, 64, fake_release, NULL, &runs, &refused);
    CHECK(given == 32 * MB && runs == 2 && refused == 0 && noffered == 2, "the bound: %llu MB in %llu runs, %llu refused, %d offered",
          (unsigned long long)(given >> 20), (unsigned long long)runs, (unsigned long long)refused, noffered);
    CHECK(offered[0][0] == a && offered[0][1] == a + 16 * MB && offered[1][1] == a + 32 * MB, "runs not split in 16 MB pieces");
    CHECK(cursor == a + 32 * MB, "the cursor is at %#llx", (unsigned long long)cursor);
    given = gc_give_back(&gm, 0, &cursor, 16 * MB, 256 * MB, 64, fake_release, NULL, &runs, &refused);
    CHECK(given == 12 * MB && runs == 4 && refused == 1, "the next pass: %llu MB, %llu runs, %llu refused (wrapping round to the "
          "window's start)", (unsigned long long)(given >> 20), (unsigned long long)runs, (unsigned long long)refused);
    CHECK(gc_count(&gm, GC_KEPT, 0, GC_SPACE) == MB, "what is left kept: %llu bytes (the import's MB)",
          (unsigned long long)gc_count(&gm, GC_KEPT, 0, GC_SPACE));
    noffered = 0;
    given = gc_give_back(&gm, 0, &cursor, 16 * MB, 256 * MB, 64, fake_release, NULL, &runs, &refused);
    CHECK(given == 0 && noffered == 1, "a pass over what an import holds: %llu given, %d offered", (unsigned long long)given, noffered);
    noffered = 0;
    gc_change(&gm, GC_KEPT, a, a + 200 * 4096 * 2, 1);
    for (uint64_t p = 0; p < 200; p++) gc_change(&gm, GC_KEPT, a + (2 * p + 1) * 4096, a + (2 * p + 2) * 4096, 0);   /* 200 runs */
    held_lo = held_hi = 0;
    given = gc_give_back(&gm, 0, &cursor, 16 * MB, 256 * MB, 64, fake_release, NULL, &runs, &refused);
    CHECK(noffered == 64, "the bound on the runs offered a pass: %d", noffered);
    /* the floor: runs below it are never offered, and the cursor wraps to it */
    memset(&gm, 0, sizeof gm);
    gc_change(&gm, GC_KEPT, 0x10000000, 0x10000000 + MB, 1);
    gc_change(&gm, GC_KEPT, 0x3000000000ULL, 0x3000000000ULL + MB, 1);
    gc_change(&gm, GC_KEPT, 0x7000000000ULL, 0x7000000000ULL + MB, 1);
    noffered = 0, runs = refused = 0, cursor = 0x5000000000ULL;
    given = gc_give_back(&gm, 4 * GB, &cursor, 16 * MB, 256 * MB, 64, fake_release, NULL, &runs, &refused);
    CHECK(given == 2 * MB && noffered == 2 && offered[0][0] == 0x7000000000ULL && offered[1][0] == 0x3000000000ULL,
          "the floor: %llu MB given in %d runs, the first at %#llx", (unsigned long long)(given >> 20), noffered,
          (unsigned long long)offered[0][0]);
    CHECK(gc_count(&gm, GC_KEPT, 0, 4 * GB) == MB, "a run below the floor was given back");
    cursor = 0;
    given = gc_give_back(&gm, 4 * GB, &cursor, 16 * MB, 256 * MB, 64, fake_release, NULL, &runs, &refused);
    CHECK(given == 0 && noffered == 2 && cursor >= 4 * GB, "a pass from below the floor: %llu given, cursor %#llx", (unsigned long long)given,
          (unsigned long long)cursor);
}

/* ---- the production notes and census_release */
static void test_production(void) {
    memset(&census, 0, sizeof census);
    const uint64_t a = 5 * GB - 0x10000;   /* (across a chunk's end) */
    hos_mem_note(HOS_MEM_COMMITTED, a, 0x30000);
    CHECK(gc_count(&census, GC_HELD, 0, GC_SPACE) == 0x30000, "committed: %llu held", (unsigned long long)gc_count(&census, GC_HELD, 0, GC_SPACE));
    CHECK(locks == unlocks && locks > 0, "a commit's note is not made under the census's lock");
    hos_mem_note(HOS_MEM_PAGES, a, 0x10000);
    CHECK(gc_count(&census, GC_GAME, 0, GC_SPACE) == 0x10000, "the game's pages not noted");
    hos_mem_note(HOS_MEM_ZEROED, a, 0x20000);
    CHECK(gc_count(&census, GC_KEPT, a, a + 0x20000) == 0x20000 && !gc_count(&census, GC_HELD, a, a + 0x20000) &&
          !gc_count(&census, GC_GAME, a, a + 0x20000) && census_zeroed == 0x20000, "zeroed under an import: not kept, or still held");
    decommits = 0;
    CHECK(census_release(a, a + 0x20000) == 1 && decommits == 1 && dec_lo == a && dec_hi == a + 0x20000, "a kept run not given back "
          "(%d decommits, [%#llx, %#llx))", decommits, (unsigned long long)dec_lo, (unsigned long long)dec_hi);
    CHECK(!gc_count(&census, GC_KEPT, a, a + 0x20000), "given back and still kept");
    /* a page of a kept run handed out again (AllocateMemoryPages, MapPhysicalMemory) before the give-back reaches it */
    hos_mem_note(HOS_MEM_COMMITTED, a, 0x20000);
    hos_mem_note(HOS_MEM_ZEROED, a, 0x20000);
    hos_mem_note(HOS_MEM_COMMITTED, a + 0x10000, 0x1000);
    CHECK(gc_count(&census, GC_KEPT, a, a + 0x20000) == 0x1F000, "a commit leaves its pages kept");
    decommits = 0;
    CHECK(census_release(a, a + 0x20000) == 0 && decommits == 0, "a run with a page handed out again was decommitted");
    decommits = 0;
    CHECK(census_release(a, a + 0x10000) == 1 && decommits == 1, "the kept part alone not given back");
    /* a run with pages nobody let go (decommitted already, never kept) */
    const uint64_t b = 0x7D80000000ULL;
    hos_mem_note(HOS_MEM_COMMITTED, b, 0x20000);
    hos_mem_note(HOS_MEM_ZEROED, b, 0x10000);
    hos_mem_note(HOS_MEM_DECOMMITTED, b + 0x10000, 0x10000);
    decommits = 0;
    CHECK(census_release(b, b + 0x20000) == 0 && decommits == 0, "a run with pages that were never kept was decommitted");
    /* a kept page held again at this moment: the first touch's handler sets it held before it clears it kept, with no lock */
    gc_change(&census, GC_HELD, b, b + 0x1000, 1);
    CHECK(census_release(b, b + 0x10000) == 0 && decommits == 0, "a kept run with a page held again was decommitted");
    gc_change(&census, GC_HELD, b, b + 0x1000, 0);
    CHECK(census_release(b, b + 0x10000) == 1 && decommits == 1, "a kept run not given back");
    /* discarded (a freed address region): only what was committed stays so */
    const uint64_t r = 0x7F00000000ULL;
    hos_mem_note(HOS_MEM_COMMITTED, r, 0x40000);
    hos_mem_note(HOS_MEM_DECOMMITTED, r + 0x20000, 0x20000);   /* (half given back by FreeMemoryPages before) */
    hos_mem_note(HOS_MEM_DISCARDED, r, 0x40000);
    CHECK(gc_count(&census, GC_KEPT, r, r + 0x40000) == 0x20000 && census_discarded == 0x20000 && !gc_count(&census, GC_HELD, r, r + 0x40000),
          "a discarded region: %llu kept, %llu counted", (unsigned long long)gc_count(&census, GC_KEPT, r, r + 0x40000),
          (unsigned long long)census_discarded);
    hos_mem_note(HOS_MEM_DECOMMITTED, r, 0x40000);
    CHECK(!gc_count(&census, GC_KEPT, r, r + 0x40000), "decommitted pages still kept");
    /* the heap's large pages are never decommitted */
    hos_large_lo = 0x1000000000ULL, hos_large_hi = hos_large_lo + 64 * MB;
    hos_mem_note(HOS_MEM_COMMITTED, hos_large_lo, 0x10000);
    hos_mem_note(HOS_MEM_ZEROED, hos_large_lo, 0x10000);
    decommits = 0;
    CHECK(census_release(hos_large_lo, hos_large_lo + 0x10000) == 0 && decommits == 0, "large pages decommitted");
    hos_large_lo = hos_large_hi = 0;
    /* below 4 GB AllocateMemoryPages hands pages out without a commit: never decommitted there */
    hos_mem_note(HOS_MEM_COMMITTED, 0x20000000, 0x10000);
    hos_mem_note(HOS_MEM_ZEROED, 0x20000000, 0x10000);
    decommits = 0;
    CHECK(census_release(0x20000000, 0x20010000) == 0 && decommits == 0, "pages below 4 GB decommitted");
    const uint64_t t0 = census_touched;
    hos_mem_note(HOS_MEM_TOUCHED, 0x7E00000000ULL, 0x10000);
    CHECK(census_touched == t0 + 1 && gc_count(&census, GC_HELD, 0x7E00000000ULL, 0x7E00010000ULL) == 0x10000, "a first touch not noted");
    hos_mem_note(HOS_MEM_COMMITTED, GC_SPACE - 0x1000, 0x2000);   /* (past the window: nothing) */
}

/* ---- (Triforce) a walk steps over the chunks never made, a partial first and last chunk walked as before */
static void test_walk_skips(void) {
    static GcMap m;
    memset(&m, 0, sizeof m);
    gc_change(&m, GC_HELD, GB / 2, GB / 2 + 8 * MB, 1);            /* (chunk 0) */
    gc_change(&m, GC_HELD, 2 * GB + 4 * MB, 2 * GB + 6 * MB, 1);   /* (chunk 2) */
    gc_change(&m, GC_HELD, GB + MB, GB + 2 * MB, 0);               /* (chunk 1: a clear makes none) */
    uint64_t visits = 0;
    GC_WALK(&m, 0, GC_SPACE, 0, { visits++; (void)mask; });
    CHECK(visits == 2 * GC_CHUNK_WORDS, "a walk of the window with 2 chunks made: %llu words visited, want %u (the 512 GB "
          "window has %llu)", (unsigned long long)visits, 2 * GC_CHUNK_WORDS, (unsigned long long)(GC_SPACE >> 18));
    /* (from 3 pages and 5 bytes into chunk 0's second half, to 100 bytes past chunk 2's half: chunk 0's pages from its 4th
       page of that half, 2048 words; chunk 2's first half, 2048) */
    const uint64_t lo = GB / 2 + 3 * 4096 + 5, hi = 2 * GB + GB / 2 + 100;
    visits = 0;
    GC_WALK(&m, lo, hi, 0, { visits++; (void)mask; });
    CHECK(visits == 4096, "a walk of a partial first and last chunk: %llu words visited, want 4096", (unsigned long long)visits);
    CHECK(gc_count(&m, GC_HELD, lo, hi) == 10 * MB - 4 * 4096, "counted over chunks made and not: %llu bytes",
          (unsigned long long)gc_count(&m, GC_HELD, lo, hi));
    CHECK(gc_move(&m, GC_HELD, GC_KEPT, GB / 2 + 4 * MB, 2 * GB + 5 * MB) == 5 * MB && gc_count(&m, GC_KEPT, 0, GC_SPACE) == 5 * MB &&
          gc_count(&m, GC_HELD, 0, GC_SPACE) == 5 * MB, "moved over a chunk not made");
    uint64_t rlo = 0, rhi = 0;
    CHECK(gc_next_run(&m, GC_HELD, GB / 2 + 8 * MB, GC_SPACE, 0, &rlo, &rhi) && rlo == 2 * GB + 5 * MB && rhi == 2 * GB + 6 * MB,
          "the next run past a chunk not made: [%#llx, %#llx)", (unsigned long long)rlo, (unsigned long long)rhi);
}

/* ---- (Triforce) hos_commit's notes around its commit (mkw_glue.c) */
static void test_commit(void) {
    memset(&census, 0, sizeof census);
    const uint64_t c = 5 * GB - 0x20000;   /* (across a chunk's end) */
    hos_mem_note(HOS_MEM_COMMITTED, c, 0x10000);                    /* (handed out already) */
    hos_mem_note(HOS_MEM_COMMITTED, c + 0x10000, 0x10000);
    hos_mem_note(HOS_MEM_ZEROED, c + 0x10000, 0x10000);             /* (let go, kept under an import) */
    void *had = hos_mem_commit_begin(c, 0x40000);                   /* (and two 64 KB pieces never committed) */
    CHECK(gc_count(&census, GC_HELD, c, c + 0x40000) == 0x40000 && gc_count(&census, GC_KEPT, c, c + 0x40000) == 0x10000,
          "before the commit: %llu held (want all), %llu kept (want the kept piece still)",
          (unsigned long long)gc_count(&census, GC_HELD, c, c + 0x40000), (unsigned long long)gc_count(&census, GC_KEPT, c, c + 0x40000));
    decommits = 0;
    CHECK(census_release(c + 0x10000, c + 0x20000) == 0 && decommits == 0, "a page being committed given back");
    hos_mem_commit_end(had, c, 0x40000, 0);   /* (the commit failed) */
    CHECK(gc_count(&census, GC_HELD, c, c + 0x40000) == 0x10000 && gc_count(&census, GC_HELD, c, c + 0x10000) == 0x10000 &&
          gc_count(&census, GC_KEPT, c, c + 0x40000) == 0x10000 && gc_count(&census, GC_KEPT, c + 0x10000, c + 0x20000) == 0x10000,
          "a failed commit: %llu held (want the piece held before), %llu kept (want the kept piece)",
          (unsigned long long)gc_count(&census, GC_HELD, c, c + 0x40000), (unsigned long long)gc_count(&census, GC_KEPT, c, c + 0x40000));
    had = hos_mem_commit_begin(c, 0x40000);
    hos_mem_commit_end(had, c, 0x40000, 1);
    CHECK(gc_count(&census, GC_HELD, c, c + 0x40000) == 0x40000 && !gc_count(&census, GC_KEPT, c, c + 0x40000),
          "a commit made: %llu held, %llu kept", (unsigned long long)gc_count(&census, GC_HELD, c, c + 0x40000),
          (unsigned long long)gc_count(&census, GC_KEPT, c, c + 0x40000));
    hos_mem_commit_end(hos_mem_commit_begin(GC_SPACE - 0x10000, 0x20000), GC_SPACE - 0x10000, 0x20000, 0);   /* (past the window) */
}

/* ---- (Triforce) a freed address region (hos_region_free, discard_pages) */
static void test_region_free(void) {
    memset(&census, 0, sizeof census);
    static Range list[4];
    memset(list, 0, sizeof list);
    const uint64_t r = 0x7E00000000ULL, other = 0x7E80000000ULL;
    list[0].addr = other, list[0].size = 0x20000, list[1].addr = r, list[1].size = 0x40000;
    regions = list, nregions = 2;
    hos_mem_note(HOS_MEM_COMMITTED, r, 0x40000);
    const uint64_t discarded = census_discarded, zeroed = census_zeroed;
    seen = 0, lock_depth = 0;
    hos_region_free(r);
    CHECK(seen == 1 && seen_listed && !seen_locked, "a freed region's discard: %d seen, %s, %s", seen,
          seen_listed ? "listed" : "no longer listed (a region asked for meanwhile could be given its pages)",
          seen_locked ? "under the regions' lock" : "not under the lock");
    CHECK(nregions == 1 && regions[0].addr == other && lock_depth == 0, "the freed region still listed (%d listed)", nregions);
#ifdef FIXTURE_POSIX
    CHECK(!gc_count(&census, GC_KEPT, r, r + 0x40000) && !gc_count(&census, GC_HELD, r, r + 0x40000) && census_discarded == discarded,
          "Linux's discard noted as discarded too: %llu kept, %llu counted discarded",
          (unsigned long long)gc_count(&census, GC_KEPT, r, r + 0x40000), (unsigned long long)(census_discarded - discarded));
    /* (pages an import holds: zeroed in place, kept as zeroed) */
    memset(&list[1], 0, sizeof list[1]);
    list[1].addr = r, list[1].size = 0x40000, nregions = 2, imported_now = 1, window_lo = r;
    hos_mem_note(HOS_MEM_COMMITTED, r, 0x40000);
    hos_region_free(r);
    CHECK(gc_count(&census, GC_KEPT, r, r + 0x40000) == 0x40000 && census_zeroed == zeroed + 0x40000 && census_discarded == discarded,
          "Linux's zeroing under an import: %llu kept, %llu counted zeroed, %llu discarded",
          (unsigned long long)gc_count(&census, GC_KEPT, r, r + 0x40000), (unsigned long long)(census_zeroed - zeroed),
          (unsigned long long)(census_discarded - discarded));
    imported_now = 0, window_lo = 0;
#else
    (void)zeroed;
    CHECK(gc_count(&census, GC_KEPT, r, r + 0x40000) == 0x40000 && census_discarded == discarded + 0x40000 &&
          !gc_count(&census, GC_HELD, r, r + 0x40000), "a discarded region not kept");
    /* (the heap's large pages are not discarded: left held, not noted) */
    hos_large_lo = r, hos_large_hi = r + 64 * MB;
    memset(&list[1], 0, sizeof list[1]);
    list[1].addr = r, list[1].size = 0x40000, nregions = 2;
    hos_mem_note(HOS_MEM_COMMITTED, r, 0x40000);
    seen = 0;
    hos_region_free(r);
    CHECK(seen == 0 && nregions == 1 && gc_count(&census, GC_HELD, r, r + 0x40000) == 0x40000 && !gc_count(&census, GC_KEPT, r, r + 0x40000) &&
          census_discarded == discarded + 0x40000, "large pages left as they were noted discarded: %llu kept",
          (unsigned long long)gc_count(&census, GC_KEPT, r, r + 0x40000));
    hos_large_lo = hos_large_hi = 0;
#endif
    seen = 0;
    hos_region_free(r);   /* (freed already: nothing) */
    CHECK(seen == 0 && nregions == 1, "a region freed twice discarded twice");
    regions = NULL, nregions = 0;
}

int main(void) {
    test_model();
    test_counts();
    test_give_back();
    test_production();
    test_walk_skips();
    test_commit();
    test_region_free();
    if (fails) return 1;
    printf("PASS: the census's maps, counts, line, give-back pass, notes and release\n");
    return 0;
}
"""

# (mutations of the production code that the fixture must catch: what each would break)
MUTATIONS = [
    ("hos_mem_commit_end", "else if (had) gc_unset_had(&census, GC_HELD, lo, hi, (const uint64_t *)had);", "",
     "a failed commit leaves its pages held: never given back, counted as handed out"),
    ("hos_mem_commit_end", "if (committed) gc_change(&census, GC_KEPT", "if (committed || 1) gc_change(&census, GC_KEPT",
     "a failed commit takes its kept pages out of the give-back's reach"),
    ("hos_mem_commit_begin", "if (had) gc_set_keeping(", "if (0) gc_set_keeping(",
     "a failed commit cannot say which pages it set held"),
    ("hos_region_free", "regions[i].freeing = 1;",
     "regions[i].freeing = 1, memmove(&regions[i], &regions[i + 1], (nregions - i - 1) * sizeof(Range)), nregions--;",
     "a freed region unlisted before its discard: a region asked for meanwhile may be given the pages it then takes away"),
    ("discard_pages", "    if (hi <= lo || keep) return;\n", "    if (hi <= lo || keep) return;\n    hos_mem_note(HOS_MEM_DISCARDED, lo, hi - lo);\n",
     "the discard noted before the large pages are left alone, and before Linux's own note"),
    ("hos_mem_note", "gc_change(&census, GC_KEPT, lo, hi, 0);\n        CENSUS_UNLOCK();", "CENSUS_UNLOCK();",
     "a commit leaves its pages kept: the give-back may take them"),
    ("census_release", " && !gc_count(&census, GC_HELD, lo, hi)", "",
     "a run with a page handed out again is decommitted"),
    ("census_release", "gc_count(&census, GC_KEPT, lo, hi) == hi - lo", "gc_count(&census, GC_KEPT, lo, hi) != 0",
     "a run with a page not kept is decommitted"),
    ("census_release", "if (hos_large_hi && lo < hos_large_hi && hi > hos_large_lo) return 0;", "",
     "the heap's large pages are decommitted"),
    ("census_release", "if (lo < (1ULL << 32)) return 0;", "", "pages below 4 GB, handed out without a commit, are decommitted"),
    ("hos_mem_note", "gc_move(&census, GC_HELD, GC_KEPT, lo, hi)", "gc_change(&census, GC_KEPT, lo, hi, 1)",
     "a discarded region's pages never committed count as kept"),
    ("hos_mem_note", "gc_change(&census, GC_HELD, lo, hi, 0);\n        gc_change(&census, GC_GAME, lo, hi, 0);\n        __atomic_add_fetch(&census_zeroed",
     "gc_change(&census, GC_GAME, lo, hi, 0);\n        __atomic_add_fetch(&census_zeroed",
     "zeroed pages stay held: never given back"),
]
# (and of guest_census.h)
HEADER_MUTATIONS = [
    ("if (!gc_k) gc_p = gc_stop;", "",
     "a walk goes through every word of chunks never made: the census's line through the whole 512 GB window"),
    ("if (chunk && set) __atomic_fetch_and", "if (0 && set) __atomic_fetch_and", "a failed commit's held pages are not put back"),
]


def check_source():
    def read(rel):
        return (ROOT / rel).read_text(encoding="utf-8")

    glue, mem, svc, backend, vk = read("host/mkw_glue.c"), read("host/hos_mem.c"), read("host/hos_svc.c"), \
        read("host/rx_backend.c"), read("gpu/vk/vk_memory.cpp")
    problems = []

    def need(text, needle, why):
        if needle not in text:
            problems.append(why)

    commit = block(glue, "int hos_commit(", "host/mkw_glue.c")
    begin, end = "= hos_mem_commit_begin(lo, hi - lo);", "hos_mem_commit_end(census_had, lo, hi - lo, committed);"
    if not (begin in commit and end in commit and commit.index(begin) < commit.index("VirtualAlloc(") < commit.index(end)):
        problems.append("hos_commit does not hold its pages in the census before it commits them (the give-back's check could "
                        "miss a page handed out again), or does not say after it whether the commit was made (a failed one left "
                        "its pages held, a kept page no longer kept)")
    touch = block(glue, "static LONG CALLBACK commit_on_touch(", "host/mkw_glue.c")
    need(touch, "hos_mem_note(HOS_MEM_TOUCHED", "a first touch is not noted")
    free_pages = block(mem, "HLE(_ZN2nn2os15FreeMemoryPagesEmm)", "host/hos_mem.c")
    need(free_pages, "memset(GP(lo), 0, (size_t)(hi - lo)), hos_mem_note(HOS_MEM_ZEROED, lo, hi - lo)",
         "FreeMemoryPages does not note its pages zeroed under an import as kept")
    need(free_pages, "MEM_DECOMMIT), hos_mem_note(HOS_MEM_DECOMMITTED, lo, hi - lo)", "FreeMemoryPages does not note its decommit")
    need(block(mem, "HLE(_ZN2nn2os19AllocateMemoryPagesEmm)", "host/hos_mem.c"), "hos_mem_note(HOS_MEM_PAGES, a, n)",
         "AllocateMemoryPages does not note the game's pages")
    need(block(mem, "static void discard_pages(", "host/hos_mem.c"), "hos_mem_note(HOS_MEM_DISCARDED, lo, hi - lo)",
         "a discard (a freed address region) is not noted")
    need(svc, "hos_mem_note(HOS_MEM_MAPPED, c->x[0], c->x[1])", "MapPhysicalMemory does not note the game's physical maps")
    need(svc, "hos_mem_note(HOS_MEM_ZEROED, c->x[0], c->x[1])", "UnmapPhysicalMemory does not note its pages zeroed under an import")
    need(svc, "c->x[0] = 0x10801;", "MapPhysicalMemory no longer tells the game LimitReached when the commit failed")
    offer = block(mem, "static int census_offer(", "host/hos_mem.c")
    if not (offer.index("hos_guest_io_begin(") < offer.index("= rx_guest_release(") < offer.index("hos_guest_io_end(")):
        problems.append("census_offer does not pay the lazy write-back's pages before the renderer runs the decommit")
    need(backend, 'GetProcAddress(vk_module, "mkwvk_guest_release")', "rx_backend.c no longer finds mkwvk_guest_release")
    need(backend, 'GetProcAddress(vk_module, "mkwvk_memory_holders")', "rx_backend.c no longer finds mkwvk_memory_holders")
    need(vk, "int mkwvk_guest_release(uint64_t lo, uint64_t hi, int (*release)(uint64_t, uint64_t))",
         "vk_memory.cpp no longer exports mkwvk_guest_release")
    need(vk, "int mkwvk_memory_holders(char* out, size_t n)", "vk_memory.cpp no longer exports mkwvk_memory_holders")
    start = block(glue, "void hos_mem_census_start(void)", "host/mkw_glue.c")
    need(start, "census_line = !(e && atoi(e) <= 0);", "the census line is no longer on by default (HOS_MEM_CENSUS=0 turns it off)")
    need(start, "census_give_back = g ? atoi(g) : 0;", "the give-back is no longer off by default")
    need(mem, "hos_mem_census_start();", "hos_mem_init no longer starts the census")
    if problems:
        raise SystemExit("\n".join("FAIL: " + p for p in problems))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    check_source()
    mem = (ROOT / "host/hos_mem.c").read_text(encoding="utf-8")
    hos_h = (ROOT / "host/hos.h").read_text(encoding="utf-8")
    enum = re.search(r"^enum \{ HOS_MEM_COMMITTED[^}]*\};", hos_h, re.M)
    if not enum:
        raise SystemExit("FAIL: host/hos.h has no HOS_MEM_* enum")
    # (in the order the fixture needs them: each before its callers)
    parts = {"hos_mem_note": block(mem, "void hos_mem_note(int what, uint64_t addr, uint64_t size)", "host/hos_mem.c"),
             "census_release": block(mem, "static int census_release(uint64_t lo, uint64_t hi)", "host/hos_mem.c"),
             "hos_mem_commit_begin": block(mem, "void *hos_mem_commit_begin(uint64_t addr, uint64_t size)", "host/hos_mem.c"),
             "hos_mem_commit_end": block(mem, "void hos_mem_commit_end(void *had, uint64_t addr, uint64_t size, int committed)",
                                         "host/hos_mem.c"),
             "pages_mark_used_locked": block(mem, "static void pages_mark_used_locked(", "host/hos_mem.c"),
             "discard_pages": block(mem, "static void discard_pages(", "host/hos_mem.c"),
             "hos_region_free": block(mem, "void hos_region_free(uint64_t addr)", "host/hos_mem.c")}
    range_t = re.findall(r"^typedef struct \{[^}]*\} Range;$", mem, re.M)
    pages_used = re.findall(r"^static uint64_t pages_used\[[^\n]*\];$", mem, re.M)
    if len(range_t) != 1 or len(pages_used) != 1:
        raise SystemExit("FAIL: host/hos_mem.c's Range or pages_used not found once")
    header = (ROOT / "host/guest_census.h").read_text(encoding="utf-8")

    def build_and_run(tmp, name, chosen, census_h=header):
        """chosen: the parts; census_h: guest_census.h as given. Built and run as for Windows, then as for Linux: the first
        failure's return code (0: none), and the output"""
        inc = Path(tmp) / (name + "-inc")
        inc.mkdir(exist_ok=True)
        (inc / "guest_census.h").write_text(census_h, encoding="utf-8")
        src = Path(tmp) / (name + ".c")
        src.write_text(FIXTURE.replace("@@ENUM@@", enum.group(0)).replace("@@RANGE@@", range_t[0] + "\n" + pages_used[0])
                       .replace("@@PRODUCTION@@", "\n".join(chosen.values())), encoding="utf-8")
        flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
        out = ""
        for variant, defines in (("windows", []), ("linux", ["-DFIXTURE_POSIX"])):
            exe = Path(tmp) / ("%s-%s.exe" % (name, variant))
            subprocess.run([args.cxx, "-x", "c", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", *defines,
                            "-I", str(inc), str(src), "-o", str(exe)], check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300, creationflags=flags)
            out += r.stdout.replace("PASS: ", "PASS (as for %s): " % variant)
            if r.returncode != 0:
                return r.returncode, out
        return 0, out

    with tempfile.TemporaryDirectory(prefix="guest-census-") as tmp:
        code, out = build_and_run(tmp, "census", parts)
        print(out, end="")
        if code != 0:
            raise SystemExit("FAIL: the census's fixture failed (exit code %d)" % code)
        for i, (fn, old, new, what) in enumerate(MUTATIONS):
            if parts[fn].count(old) != 1:
                raise SystemExit("FAIL: a mutation no longer matches %s once: %r" % (fn, old))
            mutated = dict(parts)
            mutated[fn] = parts[fn].replace(old, new)
            if build_and_run(tmp, "mutant%d" % i, mutated)[0] == 0:
                raise SystemExit("FAIL: the fixture passes with a mutation of %s: %s" % (fn, what))
        for i, (old, new, what) in enumerate(HEADER_MUTATIONS):
            if header.count(old) != 1:
                raise SystemExit("FAIL: a mutation no longer matches guest_census.h once: %r" % old)
            if build_and_run(tmp, "header-mutant%d" % i, parts, header.replace(old, new))[0] == 0:
                raise SystemExit("FAIL: the fixture passes with a mutation of guest_census.h: %s" % what)
        print("PASS: %d mutations of the production code and %d of guest_census.h each fail the fixture" %
              (len(MUTATIONS), len(HEADER_MUTATIONS)))

if __name__ == "__main__":
    main()
