/* (Triforce, 11-19) The game's texture data compaction: counted, and for the lab its cycles held to a pace.

   What it is. The game keeps its textures' data in a heap of its own and has a thread, "aglTextureDataMgr::Compaction", that
   moves a texture's data to another place in that heap when the free room is in pieces (its planner, f_807da590, weighs the
   largest free pieces against each other and the heap's size, and takes turns between the heap's two ends). A cycle is the
   plan, two copies of the data and an end, one step a message, about 18 frames; it moves one texture, some megabytes.

   Why it matters here. Standing in a fresh Market the planner finds nothing to move. After a tour of ten areas it finds work
   at every ask (lab round 62: the thread idle in the fresh Market's sample, copying in the tour's), and each move is a
   texture at a new address to this renderer: its pages written by the CPU, so captured and compared at every submit, its
   storage looked up and hashed again at the draws. That is the "re-upload loop" of the lab's tours: 79 to 88 fps at the
   Market for 97, a 1% low of 54 to 61 for 75.

   What is here. The thread's handler (f_807da3f0) calls the compaction's step (f_807da480) through hos_oot_tex_compact
   (nx2/oot_patches.py patch_tex_compact). The step runs as the game has it, and the cycles it starts are counted and logged
   every 10 s while there are any. With HOS_OOT_TEX_COMPACT_MS set, a cycle is started only that many milliseconds after the
   one before: an ask in between is answered as the planner answers when it finds nothing (0, the state left idle), and the
   game asks again. A cycle in flight is never held, and the step's other callers (the game's own waits for a compaction)
   are not touched. Nothing is held unless the switch is set: whether the game can do with fewer cycles (its heap is the
   one that fills, host/oot_heap_retry.c) is for the lab to say first.

   HOS_OOT_TEX_COMPACT_MS=<n>: at most one cycle started every n ms. -1 (a test): none started at all. Unset or 0: as shipped. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>

/* (the supported build's) the compaction's record at the manager's +7992, its state at the record's +8: 3 idle (the next
   step plans), 0 to 2 a cycle's steps */
enum { COMPACT = 7992, STATE = 8, IDLE = 3 };

static int pace_ms(void) {
    static int v = -2;
    if (v == -2) {
        const char *e = getenv("HOS_OOT_TEX_COMPACT_MS");
        v = e && *e ? atoi(e) : 0;
        if (v < -1) v = -1;
        if (v) hos_log("oot: texture compaction: %s (HOS_OOT_TEX_COMPACT_MS=%d)", v < 0 ? "no cycle is started" : "its cycles held to a pace", v);
    }
    return v;
}
static int readable(uint64_t a) { return a >= 0x80000000ULL && a < (1ULL << 39) - 0x100; }

/* What an ask with the compaction idle does. now, next: the clock and the earliest start of the next cycle (ns); ms: the
   switch. 1: the game's step runs (it plans, and starts a cycle or finds nothing); 0: held, answered as nothing to move */
int hos_oot_tex_compact_runs(int ms, int64_t now, int64_t next) {
    if (!ms) return 1;
    if (ms < 0) return 0;
    return now >= next;
}

void hos_oot_tex_compact(Ctx *c, A64Fn game) {
    static int64_t next, said;          /* (the game's compaction thread alone comes here) */
    static long started, held, steps;
    const uint64_t mgr = c->x[0];
    int idle = 0;
    if (readable(mgr)) {
        const uint64_t rec = recomp_load64(c, mgr + COMPACT);
        idle = readable(rec) && (uint8_t)recomp_load8(c, rec + STATE) == IDLE;
    }
    const int ms = pace_ms();
    const int64_t now = hos_now_ns();
    if (idle && !hos_oot_tex_compact_runs(ms, now, next)) {
        held++;
        c->x[0] = 0;   /* (as the planner's "nothing to move": the handler takes the ask back, and the game asks again) */
    } else {
        game(c);
        steps += !idle;
        if (idle && (c->x[0] & 1)) {
            started++;
            if (ms > 0) next = now + (int64_t)ms * 1000000;
        }
    }
    if (!said) said = now;
    if (now - said >= 10000000000LL) {
        if (started || held || steps)
            hos_log("oot: texture compaction in %.0f s: %ld cycles started, %ld steps of them, %ld asks held (HOS_OOT_TEX_COMPACT_MS=%d)",
                    (now - said) / 1e9, started, steps, held, ms);
        said = now, started = held = steps = 0;
    }
}
