/* (Triforce, after NX2navi) The host's zstd decoder in place of the game's own, for the files it loads: host/host_zstd.h
   has what is replaced, the game's functions as their translated code reads, and the rules. Here: the switch and the two
   calls the lifted code makes at the head of those functions (nx2/oot_patches.py patch_host_zstd).

   HOS_HOST_ZSTD, read once at the first call:
     unset or 0   the game's own decoder, exactly as before (the calls answer 0 and its code runs; after the first call
                  the lifted code no longer makes them)
     1            the host decodes what host_zstd.h takes, the game's code the rest
     2            check: the game's code decodes everything and its results stand; the host decodes beside it and every
                  difference is logged ("zstd check: ... MISMATCH"). The log's "zstd:" lines count what was compared
   Off by default: nobody has run it in Triforce's game. A wrong answer here damages every file the game loads. */
#include "hos.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

static SRWLOCK hz_lock = SRWLOCK_INIT;
#define HZ_PTR(a) GP(a)
#define HZ_OK(a, n) (hos_guest_range((a), (n)) != NULL)
#define HZ_WRITTEN(a, n) HOS_WRITTEN(GP(a), (size_t)(n))
#define HZ_LOG(...) hos_log(__VA_ARGS__)
#define HZ_LOCK() AcquireSRWLockExclusive(&hz_lock)
#define HZ_UNLOCK() ReleaseSRWLockExclusive(&hz_lock)
#define HZ_AWAY() hos_vcore_away()
#define HZ_BACK(x) do { if (x) hos_vcore_back(); } while (0)
#define HZ_NOW_NS() hos_now_ns()
#include "host_zstd.h"

#define GAME_MULTIFRAME 0x8010b380ULL   /* main+0xeb380: ZSTD_decompressMultiFrame */
#define GAME_STREAM 0x80115c80ULL       /* main+0xf5c80: the stream feed */

/* What the lifted code tests before it calls in: not 0 until the switch is read, then 0 with the switch off */
volatile int hos_host_zstd = -1;

static int mode(void) {
    static volatile LONG m = -1;
    LONG v = m;
    if (v < 0) {
        const char *e = getenv("HOS_HOST_ZSTD");
        v = e && *e ? atoi(e) : 0;
        v = v == 1 || v == 2 ? v : 0;
        /* (only the build the addresses are for: nx2/oot_patches.py puts the calls into no other, and this says so twice) */
        if (v && strncmp(hos_main_build_id, "e0f3ccfa", 8)) {
            hos_log("zstd: HOS_HOST_ZSTD is for game build e0f3ccfa, this is %.8s: the game's own decoder", hos_main_build_id);
            v = 0;
        }
        if (v && (!a64_lookup(GAME_MULTIFRAME) || !a64_lookup(GAME_STREAM))) {
            hos_log("zstd: the game's decoder is not where HOS_HOST_ZSTD expects it: the game's own decoder");
            v = 0;
        }
        if (InterlockedCompareExchange(&m, v, -1) == -1 && v)
            hos_log(v == 2 ? "zstd: check mode (HOS_HOST_ZSTD=2): the game's decoder decodes, the host's beside it, differences are logged"
                           : "zstd: the game's files are decoded by the host's zstd %s (HOS_HOST_ZSTD=1; =2 checks it against the game's)",
                    ZSTD_versionString());
        v = m;
        hos_host_zstd = (int)v;
    }
    return (int)v;
}

/* The game's function itself, entered again from its own head: the call in below answers 0 inside it */
typedef struct { Ctx *c; uint64_t fn; } GameCall;
static _Thread_local int in_game;
static uint64_t game_call(void *user) {
    const GameCall *g = (const GameCall *)user;
    in_game++;
    a64_lookup(g->fn)(g->c);
    in_game--;
    return g->c->x[0];
}

/* f_8010b380's head: 1 when the call is answered (x0 the result), 0 for the game's code to run */
int hos_zstd_multiframe(Ctx *c) {
    const int m = in_game ? 0 : mode();
    if (!m) return 0;
    hz_caller = c->x[30];   /* (the check's lines name it) */
    /* (Triforce: the output and the input, paid and left alone by the lazy write-back first, rx_backend.c. The decoder's
       accesses are the CPU's and would fault and pay one by one; this is one payment, before the work) */
    const int io_dst = HZ_OK(c->x[1], c->x[2]) ? hos_guest_io_begin(GP(c->x[1]), c->x[2]) : 0;
    const int io_src = HZ_OK(c->x[3], c->x[4]) ? hos_guest_io_begin(GP(c->x[3]), c->x[4]) : 0;
    int answered = 1;
    if (m == 2) {
        GameCall g = {c, GAME_MULTIFRAME};
        const uint64_t dst = c->x[1], cap = c->x[2], src = c->x[3], size = c->x[4], dict = c->x[5], dict_size = c->x[6], ddict = c->x[7];
        c->x[0] = hz_oneshot_check(dst, cap, src, size, dict, dict_size, ddict, game_call, &g);
    } else {
        uint64_t r = 0;
        if (hz_oneshot(c->x[1], c->x[2], c->x[3], c->x[4], c->x[5], c->x[6], c->x[7], &r)) c->x[0] = r;
        else answered = 0;
    }
    hos_guest_io_end(io_src);
    hos_guest_io_end(io_dst);
    return answered;
}

/* f_80115c80's head: the same */
int hos_zstd_stream(Ctx *c) {
    const int m = in_game ? 0 : mode();
    if (!m) return 0;
    hz_caller = c->x[30];
    GameCall g = {c, GAME_STREAM};
    uint64_t r = 0;
    if (!hz_stream(c->x[0], c->x[1], (int32_t)c->x[2], c->x[3], m == 2, game_call, &g, &r)) return 0;
    c->x[0] = r;
    return 1;
}
