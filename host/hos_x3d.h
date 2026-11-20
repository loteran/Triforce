/* (Triforce, after NX2navi's cache placement) The logical processors that share the largest L3 cache, on a processor whose
   L3 caches are not all the same size: AMD's two-die X3D parts (a Ryzen 9 7950X3D or 9950X3D), where one die of eight
   cores has the stacked 3D V-Cache (96 MB) and the other has not (32 MB). The game's threads and the replay's core are
   kept on that die (host/hos_thread.c), so that what one of them wrote is in the cache the next one reads it from, and
   none of them crosses dies. Windows' own placement of games on that die (Game Mode with AMD's chipset driver) depends
   on its taking the program for a game.

   Nothing is done:
   - with every L3 cache the same size: one die (a 7800X3D), two dies alike (a 7950X, a part with the cache on both), an
     Intel processor of two kinds of core, whose cores differ in their L2 caches and share one L3;
   - where the largest L3 serves fewer than 8 cores: a 7900X3D's six, or the four large cores of a mobile part whose
     small cores share a smaller L3. The game runs six cores' worth of threads beside the replay's: held to fewer cores
     it would lose more than the cache gives (NX2navi's rule, measured there);
   - with more than one processor group (over 64 logical processors): the masks here are one group's;
   - when the process is already held to that die (an affinity given at its start), or to so little of it that fewer
     than 8 of its cores are left.

   Plain code over the caches and cores the system lists, with no system calls: tests/test_x3d_placement.py runs it on
   authored topologies. */
#ifndef HOS_X3D_H
#define HOS_X3D_H
#include <stdint.h>

/* A cache as the system lists it: its level (1 to 3), the logical processors that share it, its size in bytes */
typedef struct { int level; uint64_t mask, bytes; } HosCache;

enum {
    HOS_X3D_NONE,     /* no L3 cache listed, or a list that contradicts itself */
    HOS_X3D_GROUPS,   /* more than one processor group */
    HOS_X3D_SAME,     /* every L3 the same size */
    HOS_X3D_FEW,      /* the largest L3 serves fewer than HOS_X3D_MIN_CORES of the cores the process may use */
    HOS_X3D_ALL,      /* the process may use no processor outside it: nothing to do */
    HOS_X3D_USED      /* the die: the processors to keep the game's threads on */
};
#define HOS_X3D_MIN_CORES 8

typedef struct {
    int why;               /* HOS_X3D_* */
    uint64_t die;          /* the allowed logical processors under the largest L3 (FEW, ALL, USED) */
    uint64_t big, small;   /* the largest and the smallest L3, in bytes */
    int cores;             /* the cores in die */
} HosX3d;

/* caches: every cache listed (the L3 ones are looked at); cores: each core's logical processors; allowed: the
   process's affinity; groups: the active processor groups. The die's mask when the game's threads are to be kept on it
   (HOS_X3D_USED), else 0; *out says which and why */
static inline uint64_t hos_x3d_pick(const HosCache *caches, int ncaches, const uint64_t *cores, int ncores,
                                    uint64_t allowed, int groups, HosX3d *out) {
    HosX3d x = {HOS_X3D_NONE, 0, 0, 0, 0};
    uint64_t die = 0, rest = 0;
    int n = 0;
    *out = x;
    if (groups != 1) {
        out->why = HOS_X3D_GROUPS;
        return 0;
    }
    for (int i = 0; i < ncaches; i++) {
        if (caches[i].level != 3 || !caches[i].mask || !caches[i].bytes) continue;
        if (!n || caches[i].bytes > x.big) x.big = caches[i].bytes;
        if (!n || caches[i].bytes < x.small) x.small = caches[i].bytes;
        n++;
    }
    if (!n) return 0;
    for (int i = 0; i < ncaches; i++) {
        if (caches[i].level != 3 || !caches[i].mask || !caches[i].bytes) continue;
        if (caches[i].bytes == x.big) die |= caches[i].mask;
        else rest |= caches[i].mask;
    }
    out->big = x.big, out->small = x.small;
    if (x.big == x.small) {
        out->why = HOS_X3D_SAME;
        return 0;
    }
    if (die & rest) return 0;   /* (a processor under two L3 caches: not a list to act on) */
    die &= allowed;
    for (int i = 0; i < ncores; i++)
        if (cores[i] & die) x.cores++;
    out->die = die, out->cores = x.cores;
    if (x.cores < HOS_X3D_MIN_CORES) {
        out->why = HOS_X3D_FEW;
        return 0;
    }
    if (!(allowed & ~die)) {
        out->why = HOS_X3D_ALL;
        return 0;
    }
    out->why = HOS_X3D_USED;
    return die;
}

/* The affinity a game thread gets (hos_thread_pin), 0 for none set: the processors the process may use, on the die
   when there is one (hos_x3d_pick's, 0: none), without the core kept for the replay (hos_reserved_cpus, 0: none).
   With no die this is what it was before: the process's processors but the reserved core's, where any are left */
static inline uint64_t hos_x3d_thread_mask(uint64_t process, uint64_t die, uint64_t reserved) {
    const uint64_t on = die && (process & die) ? process & die : process;
    if (on & ~reserved) return reserved || on != process ? on & ~reserved : 0;
    return on != process ? on : 0;
}
#endif
