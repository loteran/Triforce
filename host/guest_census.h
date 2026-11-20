/* (Triforce) The guest memory census (hos_mem.c, mkw_glue.c): which pages of the guest window the runtime has handed out,
   which of them the game holds as pages or physical maps, and which the game let go that stay committed, and the line that
   says so every few seconds. Apart from Windows and the runtime so that tests/test_guest_census.py runs it alone.

   Why: NX2navi's guest region above the heap grew 6.6 -> 13.3 -> 16.1 GB over 6,000 frames while the heap stayed flat,
   until the PC's commit ran out (MapPhysicalMemory ... LimitReached, then a lost device). Triforce's log had only the
   window's committed total (the renderer's "vk: memory, the parts" line, the guest window walked every eighth line): 8.7
   -> 9.2 -> 12.4 -> 14.6 GB over an owner's session of 134,000 presents, the heap 6.9 GB throughout, and nothing said
   which part held it. The game is told 9809 MB in all.

   What is kept, a bit a 4 KB page, in 1 GB chunks made as the window's gigabytes are first used (96 KB a chunk, 512 at
   most):
   - GC_HELD: the runtime handed the page out and committed it (hos_commit: the program, the system region, the heap, the
     address regions whole, AllocateMemoryPages, MapPhysicalMemory; and a first touch of a page nothing handed out)
   - GC_GAME: of those, the pages the game holds in Horizon's own terms, as pages (AllocateMemoryPages) or physical maps
     (MapPhysicalMemory): what its 9809 MB pays for besides the heap and the program
   - GC_KEPT: pages the game let go that stay committed: zeroed instead of decommitted because a Vulkan import held them
     (FreeMemoryPages, UnmapPhysicalMemory: "decommitted pages under an import faulted the device"), and the pages of a
     freed address region (discarded: out of RAM, but still the process's commit). Nothing gave them back before: an
     import given back later left its zeroed pages committed for the rest of the session. The give-back (mkw_glue.c,
     HOS_MEM_GIVE_BACK) decommits them once no import holds them, under the renderer's import locks.

   The bits are changed with atomic operations (the first touch's handler takes no lock); a chunk is made with GC_ALLOC,
   which the runtime points at VirtualAlloc (no heap lock in an exception handler). */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef GC_ALLOC
#include <stdlib.h>
#define GC_ALLOC(n) calloc(1, (n))
#define GC_FREE(p) free(p)
#endif

enum { GC_HELD = 0, GC_GAME = 1, GC_KEPT = 2, GC_MAPS = 3 };
#define GC_PAGE_SHIFT 12
#define GC_CHUNK_SHIFT 30
#define GC_CHUNK_WORDS ((1u << (GC_CHUNK_SHIFT - GC_PAGE_SHIFT)) / 64)
#define GC_CHUNKS 512u   /* (the 512 GB window, HOS_GUEST_SPACE) */
#define GC_SPACE ((uint64_t)GC_CHUNKS << GC_CHUNK_SHIFT)

typedef struct {
    uint64_t bits[GC_MAPS][GC_CHUNK_WORDS];
} GcChunk;
typedef struct {
    GcChunk *chunk[GC_CHUNKS];
} GcMap;

/* The chunk that holds page bit c's gigabyte (NULL when none was made and make is 0, or none could be made) */
static inline GcChunk *gc_chunk(GcMap *m, uint64_t c, int make) {
    if (c >= GC_CHUNKS) return NULL;
    GcChunk *k = __atomic_load_n(&m->chunk[c], __ATOMIC_ACQUIRE);
    if (k || !make) return k;
    GcChunk *fresh = (GcChunk *)GC_ALLOC(sizeof(GcChunk));
    if (!fresh) return NULL;
    GcChunk *expected = NULL;
    if (__atomic_compare_exchange_n(&m->chunk[c], &expected, fresh, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return fresh;
    GC_FREE(fresh);   /* (another thread made it first) */
    return expected;
}

/* Walks the whole pages of [lo, hi) (lo rounded up, hi down: a part of a page is not the page) a word at a time:
   body for each word's bits (chunk, word_at, mask). A chunk not made (make 0), or that could not be made, is stepped over
   whole (Triforce: the census's walks of the 512 GB window had gone through its 2 million words a map, chunk or none, some
   10 million steps a census; now they cost the gigabytes in use) */
#define GC_WALK(m, lo, hi, make, body)                                                                                      \
    do {                                                                                                                    \
        uint64_t gc_p = ((lo) + 0xFFFULL) >> GC_PAGE_SHIFT, gc_e = (hi) >> GC_PAGE_SHIFT;                                  \
        if (gc_e > (GC_SPACE >> GC_PAGE_SHIFT)) gc_e = GC_SPACE >> GC_PAGE_SHIFT;                                          \
        while (gc_p < gc_e) {                                                                                               \
            const uint64_t gc_c = gc_p >> (GC_CHUNK_SHIFT - GC_PAGE_SHIFT);                                                \
            const uint64_t gc_cend = (gc_c + 1) << (GC_CHUNK_SHIFT - GC_PAGE_SHIFT);                                       \
            const uint64_t gc_stop = gc_e < gc_cend ? gc_e : gc_cend;                                                      \
            GcChunk *gc_k = gc_chunk((m), gc_c, (make));                                                                   \
            if (!gc_k) gc_p = gc_stop;   /* (no bits there: on to the chunk's end, or to hi's page in it) */                \
            while (gc_p < gc_stop) {                                                                                        \
                const uint64_t gc_w = (gc_p & ((1u << (GC_CHUNK_SHIFT - GC_PAGE_SHIFT)) - 1)) >> 6, gc_b = gc_p & 63;      \
                const uint64_t gc_n = gc_stop - gc_p < 64 - gc_b ? gc_stop - gc_p : 64 - gc_b;                             \
                const uint64_t mask = (gc_n == 64 ? ~0ULL : ((1ULL << gc_n) - 1)) << gc_b;                                 \
                const uint64_t word_at = gc_w;                                                                              \
                GcChunk *chunk = gc_k;                                                                                      \
                (void)word_at, (void)chunk;                                                                                 \
                body;                                                                                                       \
                gc_p += gc_n;                                                                                               \
            }                                                                                                               \
        }                                                                                                                   \
    } while (0)

/* Sets (set 1) or clears (set 0) one map's bits for the whole pages of [lo, hi); the bytes whose bit changed */
static inline uint64_t gc_change(GcMap *m, int map, uint64_t lo, uint64_t hi, int set) {
    uint64_t changed = 0;
    if (hi <= lo) return 0;
    GC_WALK(m, lo, hi, set, {
        if (chunk) {
            uint64_t *w = &chunk->bits[map][word_at];
            if (set) changed += (uint64_t)__builtin_popcountll(mask & ~__atomic_fetch_or(w, mask, __ATOMIC_ACQ_REL));
            else changed += (uint64_t)__builtin_popcountll(mask & __atomic_fetch_and(w, ~mask, __ATOMIC_ACQ_REL));
        }
    });
    return changed << GC_PAGE_SHIFT;
}

/* Moves one map's set bits over [lo, hi) into another (cleared in from, set in to); the bytes moved */
static inline uint64_t gc_move(GcMap *m, int from, int to, uint64_t lo, uint64_t hi) {
    uint64_t moved = 0;
    if (hi <= lo) return 0;
    GC_WALK(m, lo, hi, 0, {
        if (chunk) {
            const uint64_t had = mask & __atomic_fetch_and(&chunk->bits[from][word_at], ~mask, __ATOMIC_ACQ_REL);
            if (had) __atomic_fetch_or(&chunk->bits[to][word_at], had, __ATOMIC_ACQ_REL);
            moved += (uint64_t)__builtin_popcountll(had);
        }
    });
    return moved << GC_PAGE_SHIFT;
}

/* (Triforce) hos_commit's held pages (hos_mem.c hos_mem_commit_begin): gc_words(lo, hi) words, one a word of the range's
   pages from lo's on; gc_set_keeping sets one map's bits over [lo, hi) and says in had which were set already (had starts
   all ones: a word it never reaches, its chunk not made, set nothing), and gc_unset_had clears the bits it set, no other */
static inline uint64_t gc_words(uint64_t lo, uint64_t hi) {
    const uint64_t p = (lo + 0xFFFULL) >> GC_PAGE_SHIFT, e = hi >> GC_PAGE_SHIFT;
    return e > p ? ((e - 1) >> 6) - (p >> 6) + 1 : 0;
}
static inline void gc_set_keeping(GcMap *m, int map, uint64_t lo, uint64_t hi, uint64_t *had) {
    const uint64_t first = ((lo + 0xFFFULL) >> GC_PAGE_SHIFT) >> 6;
    if (hi <= lo) return;
    GC_WALK(m, lo, hi, 1, {   /* (gc_p: the walk's page) */
        if (chunk) had[(gc_p >> 6) - first] = mask & __atomic_fetch_or(&chunk->bits[map][word_at], mask, __ATOMIC_ACQ_REL);
    });
}
static inline void gc_unset_had(GcMap *m, int map, uint64_t lo, uint64_t hi, const uint64_t *had) {
    const uint64_t first = ((lo + 0xFFFULL) >> GC_PAGE_SHIFT) >> 6;
    if (hi <= lo) return;
    GC_WALK(m, lo, hi, 0, {
        const uint64_t set = mask & ~had[(gc_p >> 6) - first];
        if (chunk && set) __atomic_fetch_and(&chunk->bits[map][word_at], ~set, __ATOMIC_ACQ_REL);
    });
}

/* The bytes of [lo, hi) whose bit is set in one map */
static inline uint64_t gc_count(GcMap *m, int map, uint64_t lo, uint64_t hi) {
    uint64_t n = 0;
    if (hi <= lo) return 0;
    GC_WALK(m, lo, hi, 0, {
        if (chunk) n += (uint64_t)__builtin_popcountll(mask & __atomic_load_n(&chunk->bits[map][word_at], __ATOMIC_ACQUIRE));
    });
    return n << GC_PAGE_SHIFT;
}

/* The next run of set bits of one map at or after from (and below hi), at most max bytes long: 1 and [*rlo, *rhi), or 0
   when there is none. Chunks never made and empty words are stepped over whole */
static inline int gc_next_run(GcMap *m, int map, uint64_t from, uint64_t hi, uint64_t max, uint64_t *rlo, uint64_t *rhi) {
    uint64_t p = (from + 0xFFFULL) >> GC_PAGE_SHIFT, e = hi >> GC_PAGE_SHIFT;
    const uint64_t per = 1u << (GC_CHUNK_SHIFT - GC_PAGE_SHIFT);
    if (e > (GC_SPACE >> GC_PAGE_SHIFT)) e = GC_SPACE >> GC_PAGE_SHIFT;
    uint64_t start = 0;
    int in = 0;
    while (p < e) {
        GcChunk *k = gc_chunk(m, p / per, 0);
        if (!k) {
            if (in) break;
            p = (p / per + 1) * per;
            continue;
        }
        const uint64_t w = __atomic_load_n(&k->bits[map][(p % per) >> 6], __ATOMIC_ACQUIRE) >> (p & 63);
        if (!in) {
            if (!w) {   /* (the rest of this word is clear) */
                p = (p | 63) + 1;
                continue;
            }
            p += (uint64_t)__builtin_ctzll(w);
            if (p >= e) break;
            start = p, in = 1;
            continue;
        }
        if (!(w & 1)) break;
        const uint64_t ones = ~w ? (uint64_t)__builtin_ctzll(~w) : 64 - (p & 63);   /* (the set bits from p in this word) */
        p += ones;
        if (max && (p - start) << GC_PAGE_SHIFT >= max) {
            p = start + (max >> GC_PAGE_SHIFT);
            break;
        }
    }
    if (!in) return 0;
    if (p > e) p = e;
    *rlo = start << GC_PAGE_SHIFT, *rhi = p << GC_PAGE_SHIFT;
    return 1;
}

/* ---- the census: the counts by kind, and its log line */
typedef struct {
    uint64_t held, low, heap, heap_size, regions, region_span, region_game, outside, outside_game, game, kept;
    int64_t nregions;
    uint64_t touched;                                   /* (first touches so far, 64 KB each) */
    uint64_t kept_import, kept_discard, given_back, given_runs, refused;   /* (so far) */
    uint64_t budget, program;                           /* (what the game is told it has, hos_total_memory; the program) */
    int64_t proc_commit, proc_peak, working_set, sys_commit, sys_limit, ram_avail, ram_total;   /* (-1: not known) */
} GcCensus;

/* The counts from the maps: regions[i] = {address, size} of the live address regions, the heap at heap_lo, heap_size long.
   Whatever is handed out below the heap and not in a region is the program's and the system region's; above it, not in
   the heap or a region, the physical maps', the pages the game took outside its regions', and first touches' */
static inline void gc_census_count(GcMap *m, const uint64_t (*regions)[2], int n, uint64_t heap_lo, uint64_t heap_size,
                                   GcCensus *c) {
    c->held = gc_count(m, GC_HELD, 0, GC_SPACE);
    c->game = gc_count(m, GC_GAME, 0, GC_SPACE);
    c->kept = gc_count(m, GC_KEPT, 0, GC_SPACE);
    const uint64_t low_all = gc_count(m, GC_HELD, 0, heap_lo);
    c->heap = gc_count(m, GC_HELD, heap_lo, heap_lo + heap_size), c->heap_size = heap_size;
    uint64_t regions_low = 0;
    c->regions = c->region_span = c->region_game = 0;
    c->nregions = n;
    for (int i = 0; i < n; i++) {
        const uint64_t lo = regions[i][0], hi = regions[i][0] + regions[i][1];
        const uint64_t h = gc_count(m, GC_HELD, lo, hi);
        c->regions += h, c->region_span += regions[i][1];
        c->region_game += gc_count(m, GC_GAME, lo, hi);
        if (lo < heap_lo) regions_low += h;
    }
    c->low = low_all > regions_low ? low_all - regions_low : 0;
    const uint64_t accounted = low_all + c->heap + (c->regions - regions_low);
    c->outside = c->held > accounted ? c->held - accounted : 0;
    c->outside_game = c->game > c->region_game ? c->game - c->region_game : 0;
}

static inline long long gc_mb(int64_t b) { return b < 0 ? -1 : (long long)(b >> 20); }

/* The line, without its "mem: " head (under a thousand characters: hos_log's line is 1024). The game's own count is what
   it counts against what it is told it has: the heap, the program, and its pages and physical maps */
static inline int gc_line(char *out, size_t size, const GcCensus *c) {
    const uint64_t own = c->heap_size + c->program + c->game;
    char sys[64] = "?";
    if (c->sys_commit >= 0 && c->sys_limit > 0) snprintf(sys, sizeof sys, "%lld of %lld", gc_mb(c->sys_commit), gc_mb(c->sys_limit));
    return snprintf(out, size,
                    "census (MB): handed out %lld: the program and system %lld, heap %lld of %lld, address regions %lld in %lld "
                    "(spanning %lld; the game's pages in them %lld), outside them %lld (the game's pages and physical maps %lld; "
                    "%llu first touches); the game's own count %lld of its %lld; kept committed after the game let go %lld (so far "
                    "%lld zeroed under imports, %lld discarded; given back %lld in %llu runs, %llu runs found under an import); "
                    "process commit %lld (peak %lld), working set %lld; the PC's commit %s, RAM %lld of %lld available",
                    gc_mb((int64_t)c->held), gc_mb((int64_t)c->low), gc_mb((int64_t)c->heap), gc_mb((int64_t)c->heap_size),
                    gc_mb((int64_t)c->regions), (long long)c->nregions, gc_mb((int64_t)c->region_span),
                    gc_mb((int64_t)c->region_game), gc_mb((int64_t)c->outside), gc_mb((int64_t)c->outside_game),
                    (unsigned long long)c->touched, gc_mb((int64_t)own), gc_mb((int64_t)c->budget), gc_mb((int64_t)c->kept),
                    gc_mb((int64_t)c->kept_import), gc_mb((int64_t)c->kept_discard), gc_mb((int64_t)c->given_back),
                    (unsigned long long)c->given_runs, (unsigned long long)c->refused, gc_mb(c->proc_commit), gc_mb(c->proc_peak),
                    gc_mb(c->working_set), sys, gc_mb(c->ram_avail), gc_mb(c->ram_total));
}

/* ---- the give-back: the kept pages, decommitted once no import holds them */
/* release(ctx, lo, hi): 1 when [lo, hi) was given back, 0 when not (an import holds it, or the game took a page back) */
typedef int (*GcRelease)(void *ctx, uint64_t lo, uint64_t hi);

/* One pass over the kept runs at or above floor: from *cursor on (and from floor once the window's end is reached), each at
   most max_run bytes, until max_bytes are given back or max_calls runs were offered; *cursor is where the next pass starts.
   The bytes given back; *runs and *refused count the runs given back and refused. (The runtime's floor is 4 GB: below it
   AllocateMemoryPages hands out pages without committing them, hos_mem.c, so a page decommitted there could reach a host
   call uncommitted) */
static inline uint64_t gc_give_back(GcMap *m, uint64_t floor, uint64_t *cursor, uint64_t max_run, uint64_t max_bytes,
                                    int max_calls, GcRelease release, void *ctx, uint64_t *runs, uint64_t *refused) {
    uint64_t given = 0, at = *cursor < floor ? floor : *cursor, lo = 0, hi = 0;
    int calls = 0, wrapped = 0;
    const uint64_t start = at;
    while (given < max_bytes && calls < max_calls) {
        const uint64_t end = wrapped ? start : GC_SPACE;
        if (!gc_next_run(m, GC_KEPT, at, end, max_run, &lo, &hi)) {
            if (wrapped || start == floor) break;
            wrapped = 1, at = floor;
            continue;
        }
        calls++;
        if (release(ctx, lo, hi)) given += hi - lo, (*runs)++;
        else (*refused)++;
        at = hi;
    }
    *cursor = at >= GC_SPACE ? floor : at;
    return given;
}
