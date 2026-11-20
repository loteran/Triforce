/* (Triforce, after NX2navi's runtime/ov_zstd.c) The game's zstd decoder on the host's libzstd (third_party/zstd).

   Most of the game's files are zstd frames (.zs), many with a dictionary from Pack/ZsDic.pack.zs, and the game decodes
   them with zstd linked into its main module: translated Arm code here. Two of its functions are entered through the
   host (nx2/oot_patches.py patch_host_zstd puts a call at the head of each; host/hos_zstd.c is the glue and the switch,
   HOS_HOST_ZSTD, off unless set). Game 1.1.1, build e0f3ccfa, main at 0x80020000:

   f_8010b380 (main+0xeb380), zstd's ZSTD_decompressMultiFrame(dctx, dst, dstCapacity, src, srcSize, dict, dictSize,
   ddict): one call decodes a whole buffer. Its three callers look at the result as zstd does: an error is a value
   above (size_t)-120, of which they tell -70 (the output is too small) from the rest. hz_oneshot decodes the same
   input with libzstd and answers only with a decoded size: whatever libzstd refuses, and whatever is not plainly
   safe to take (input inside the output, a dictionary that cannot be taken, under 5 bytes of input), goes to the
   game's own function, which then gives its own answer and its own error code, as today.

   f_80115c80 (main+0xf5c80), the game's stream feed, which decodes most of what the loading threads read. Its decode
   job (f_80115bf0) calls feed(S, src, int size, ddict) for each chunk it read and looks only at the result: 0 the
   content is out, bit 31 set an error, anything else go on. The feed drives zstd's bufferless decoder over a state S:
     S+0x00 u8   phase: 1 begin, 2 running, 3 failed, 4 done
     S+0x08 u64  where the next output goes        S+0x10 i32  output room left      S+0x14 u32  content left
     S+0x18 u64  the DCtx workspace                S+0x20 u64, +0x28 i32, +0x2c i32  carry buffer, its size, its fill
     S+0x30 u64  the DCtx (set at begin)
   What the translated code does, and hz_stream does for the streams it takes:
   - phase 1: reads the frame header from the chunk (ZSTD_getFrameHeader); a header not whole in it, an error, or a
     frame without its content size: phase 3 and 0xffffffff. Else +0x14 = the content size, the DCtx made in the
     workspace, +0x30 = it, phase 2, and the chunk is decoded as in phase 2.
   - phase 2: decodes what the input completes, moves +0x08 on and +0x10 and +0x14 down by the bytes written, and
     returns +0x14. When the frame ends (with its checksum, when it has one) phase 4. Input after the frame's end:
     phase 3 and 0xfffffffd. A decode error: phase 3 and 0xfffffffe when it is zstd's "output too small" (70),
     0xffffffff for any other.
   - phase 3: 0xffffffff. Phase 4: 0xfffffffd. A size of 0 or less in phase 2: +0x14 as it is.
   hz_stream owns a stream from its begin or not at all: the game's DCtx never sees the blocks a host stream decoded,
   so there is no going back half way. It takes a stream whose first chunk holds the whole header of a plain frame with
   its content size, whose output has room for all of that content and does not overlap the chunk, whose workspace is
   one the game would make its DCtx in, whose dictionary (if any) the host can take, and for which a slot is free. Every other stream, and every feed of a stream it did not
   begin, is the game's. That leaves the cases whose result depends on the game's decoder itself (an output too small:
   0xfffffffe part way, with what fits written) to the game. For its own streams it decodes with ZSTD_decompressStream
   straight into the game's output, one buffer for the whole frame (ZSTD_d_stableOutBuffer: no window of its own); a
   feed that finds the output anywhere but where the last one left it fails the stream (0xffffffff).
   The job stops feeding at a result of 0, which a frame gives once its content is out even when its checksum is still
   to come: such a stream keeps its slot but is marked spent, and a new stream takes a spent slot when none is free.

   Known differences for a damaged frame (never for a sound one): which feed reports the error, and the bytes written
   before it. The verdict is the same: phase 3 and a result with bit 31 set.

   Check mode (HOS_HOST_ZSTD=2): the game's own code decodes everything as before and its results stand; the host
   decodes the same input into memory of its own and the two are compared, every difference logged ("zstd check").
   The way to try the host decoder on a PC before trusting it with =1.

   The includer says how guest memory is reached (host/hos_zstd.c; tests/test_host_zstd.py with a buffer of its own):
     HZ_PTR(a)          host pointer of guest address a
     HZ_OK(a, n)        whether the n guest bytes at a lie in guest memory
     HZ_WRITTEN(a, n)   host code wrote those guest bytes (the write tracking the GPU's uploads read)
     HZ_LOG(...)        a log line
     HZ_LOCK() / HZ_UNLOCK()   one lock for the tables here
     HZ_AWAY() / HZ_BACK(x)    the game's virtual core given up for a long decode, and taken again
     HZ_NOW_NS()        a clock, for the statistics
   The stream feed is ported from NX2navi's (the owner's lines there); the one-shot path and the dictionary table are
   written here from their description (another contributor's in NX2navi). */
#ifndef HOST_ZSTD_H
#define HOST_ZSTD_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ZSTD_STATIC_LINKING_ONLY
#include "../third_party/zstd/lib/zstd.h"
#include "../third_party/zstd/lib/zstd_errors.h"

#define HZ_AWAY_BYTES (64u << 10)   /* inputs from this size give the virtual core up while they decode */
#define HZ_MAX_DICT (64u << 20)
#define HZ_NDICT 32
#define HZ_NSTREAM 16
#define HZ_NDCTX 8
/* The game's DCtx's "bytes expected next" (zstd's dctx->expected, at +28776 of the DCtx in this build): the first thing
   the game's feed reads of it in phase 2. For a host stream the game's DCtx is never made, and a feed of such a stream
   must never reach the game's own code; one could only if the stream's slot here were gone while the game still fed it
   (a spent stream's slot taken by another, and the game feeding on after a result of 0, which its job does not do).
   So that even that ends in the game's own words and not on a context that was never made, the host's begin sets this
   field to 0: the game's feed then says "input after the frame's end" (phase 3, 0xfffffffd) for any bytes, and
   answers the content left for none */
#define HZ_DCTX_EXPECTED 28776

/* The game's function, run by the includer for check mode: its result (x0) */
typedef uint64_t (*HzGame)(void *user);

/* (Triforce, after NX2navi's ov_zstd.c, whose check lines name the caller) The game's return address of the call being
   answered, set by the includer before it calls in (0: not known): a check's "MISMATCH" line says which caller it was */
static _Thread_local uint64_t hz_caller;

static struct {
    uint64_t calls, in, out, ns, game, inplace, away, checked, differ;   /* one-shot */
    uint64_t streams, feeds, s_in, s_out, s_ns, s_game, s_checked, s_differ;
} hz_stats;

static inline uint32_t hz_ld32(uint64_t a) { uint32_t v; memcpy(&v, HZ_PTR(a), 4); return v; }
static inline uint64_t hz_ld64(uint64_t a) { uint64_t v; memcpy(&v, HZ_PTR(a), 8); return v; }
static inline void hz_st32(uint64_t a, uint32_t v) { memcpy(HZ_PTR(a), &v, 4); }
static inline void hz_st64(uint64_t a, uint64_t v) { memcpy(HZ_PTR(a), &v, 8); }
static inline void hz_add(uint64_t *p, uint64_t v) { __atomic_add_fetch(p, v, __ATOMIC_RELAXED); }
/* zstd's own test of a result, for the game's results too (its error codes are the same numbers) */
static inline int hz_is_error(uint64_t r) { return r > (uint64_t)0 - 120; }

/* ---- dictionaries: a guest ZSTD_DDict (zstd's struct: dictBuffer, dictContent, dictSize first; the game's own code
   reads the content and its size at +8 and +16) or raw dictionary bytes become a host ZSTD_DDict made from a copy of
   the content, kept for the run under the guest's addresses, the size and a print of the content. Entries are never
   freed (another thread may be decoding with one); with the table full further dictionaries decode in the game ---- */
typedef struct { uint64_t guest, content, size, print; ZSTD_DDict *dd; } HzDict;
static HzDict hz_dicts[HZ_NDICT];
static int hz_ndicts;

/* (the size, and 256 bytes from the start, the middle and the end: a new dictionary at an old address differs there,
   a zstd dictionary's id is in its first 8 bytes) */
static uint64_t hz_print(const uint8_t *p, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ull ^ n;
    const uint64_t at[3] = {0, n / 2, n > 256 ? n - 256 : 0};
    for (int k = 0; k < 3; k++)
        for (uint64_t i = at[k]; i < at[k] + 256 && i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static ZSTD_DDict *hz_dict_find(uint64_t guest, uint64_t content, uint64_t size, uint64_t print) {
    for (int i = 0; i < hz_ndicts; i++)
        if (hz_dicts[i].guest == guest && hz_dicts[i].content == content && hz_dicts[i].size == size && hz_dicts[i].print == print)
            return hz_dicts[i].dd;
    return NULL;
}

/* The host dictionary for the content at guest address `content` (guest: the ZSTD_DDict it came from, 0 for raw
   bytes); NULL when it cannot be taken */
static ZSTD_DDict *hz_dict_for(uint64_t guest, uint64_t content, uint64_t size) {
    if (!content || !size || size > HZ_MAX_DICT || !HZ_OK(content, size)) return NULL;
    const uint64_t print = hz_print((const uint8_t *)HZ_PTR(content), size);
    HZ_LOCK();
    ZSTD_DDict *dd = hz_dict_find(guest, content, size, print);
    const int full = !dd && hz_ndicts >= HZ_NDICT;
    HZ_UNLOCK();
    if (dd || full) return dd;
    ZSTD_DDict *made = ZSTD_createDDict(HZ_PTR(content), (size_t)size);   /* (a copy: the game may free or reuse its buffer) */
    if (!made) return NULL;
    int slot = -1;
    HZ_LOCK();
    dd = hz_dict_find(guest, content, size, print);   /* (another thread may have made it meanwhile) */
    if (!dd && hz_ndicts < HZ_NDICT) {
        slot = hz_ndicts;
        hz_dicts[hz_ndicts].guest = guest, hz_dicts[hz_ndicts].content = content, hz_dicts[hz_ndicts].size = size;
        hz_dicts[hz_ndicts].print = print, hz_dicts[hz_ndicts].dd = made;
        hz_ndicts++;
        dd = made;
    }
    HZ_UNLOCK();
    if (dd != made) ZSTD_freeDDict(made);
    if (!dd) HZ_LOG("zstd: more than %d dictionaries: further ones decode in the game", HZ_NDICT);
    else if (slot >= 0)
        HZ_LOG("zstd: host dictionary %d for %s %#llx: %llu bytes at %#llx, id %u", slot, guest ? "the DDict" : "raw bytes at",
               (unsigned long long)(guest ? guest : content), (unsigned long long)size, (unsigned long long)content,
               ZSTD_getDictID_fromDDict(dd));
    return dd;
}

/* (the dictionary a call names: a ZSTD_DDict, else raw bytes, else none. 0 when it names one the host cannot take) */
static int hz_dict_of(uint64_t dict, uint64_t dict_size, uint64_t ddict, ZSTD_DDict **dd) {
    *dd = NULL;
    if (ddict) {
        if (!HZ_OK(ddict, 24)) return 0;
        return (*dd = hz_dict_for(ddict, hz_ld64(ddict + 8), hz_ld64(ddict + 16))) != NULL;
    }
    if (dict && dict_size) return (*dd = hz_dict_for(0, dict, dict_size)) != NULL;
    return 1;
}

/* ---- one call, one buffer (ZSTD_decompressMultiFrame) ---- */
static ZSTD_DCtx *hz_dctx[HZ_NDCTX];
static int hz_dctx_busy[HZ_NDCTX];

/* (a decoder context for one call: one of a few kept, else one made for the call. *slot: which, -1 the call's own) */
static ZSTD_DCtx *hz_dctx_take(int *slot) {
    *slot = -1;
    HZ_LOCK();
    for (int i = 0; i < HZ_NDCTX && *slot < 0; i++)
        if (!hz_dctx_busy[i]) hz_dctx_busy[i] = 1, *slot = i;
    HZ_UNLOCK();
    if (*slot < 0) return ZSTD_createDCtx();
    if (!hz_dctx[*slot]) hz_dctx[*slot] = ZSTD_createDCtx();
    if (!hz_dctx[*slot]) {
        HZ_LOCK();
        hz_dctx_busy[*slot] = 0;
        HZ_UNLOCK();
        *slot = -1;
        return NULL;
    }
    return hz_dctx[*slot];
}
static void hz_dctx_give(ZSTD_DCtx *d, int slot) {
    if (slot < 0) {
        ZSTD_freeDCtx(d);
        return;
    }
    HZ_LOCK();
    hz_dctx_busy[slot] = 0;
    HZ_UNLOCK();
}

/* Decodes src into out (host memory: the game's output, or check mode's own). libzstd's result; *ok 0 when the inputs
   cannot be taken (then nothing was written) */
static size_t hz_decode(void *out, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict, uint64_t dict_size,
                        uint64_t ddict, int *ok) {
    ZSTD_DDict *dd = NULL;
    *ok = 0;
    if (size < 5 || !src || !HZ_OK(src, size) || !hz_dict_of(dict, dict_size, ddict, &dd)) return 0;
    int slot;
    ZSTD_DCtx *d = hz_dctx_take(&slot);
    if (!d) return 0;
    const int away = size >= HZ_AWAY_BYTES ? HZ_AWAY() : 0;
    if (away) hz_add(&hz_stats.away, 1);
    *ok = 1;
    const size_t r = dd ? ZSTD_decompress_usingDDict(d, out, (size_t)cap, HZ_PTR(src), (size_t)size, dd)
                        : ZSTD_decompressDCtx(d, out, (size_t)cap, HZ_PTR(src), (size_t)size);
    HZ_BACK(away);
    hz_dctx_give(d, slot);
    return r;
}

static void hz_count(uint64_t in, uint64_t out, uint64_t ns) {
    const uint64_t n = __atomic_add_fetch(&hz_stats.calls, 1, __ATOMIC_RELAXED);
    hz_add(&hz_stats.in, in), hz_add(&hz_stats.out, out), hz_add(&hz_stats.ns, ns);
    if (n == 1 || !(n & 0x3fff))
        HZ_LOG("zstd: %llu calls decoded on the host, %.1f MB in, %.1f MB out, %.0f ms; %llu left to the game's decoder (%llu of "
               "them in place), %llu with the core given up; check: %llu compared, %llu differ", (unsigned long long)n,
               hz_stats.in / 1048576.0, hz_stats.out / 1048576.0, hz_stats.ns / 1e6, (unsigned long long)hz_stats.game,
               (unsigned long long)hz_stats.inplace, (unsigned long long)hz_stats.away, (unsigned long long)hz_stats.checked,
               (unsigned long long)hz_stats.differ);
}

/* (the input inside the output: the result depends on how the decoder's writes overtake its reads, which libzstd does
   not promise to do as the game's version does, and a host decode that failed half way would leave the game's decoder
   a damaged input) */
static int hz_in_place(uint64_t dst, uint64_t cap, uint64_t src, uint64_t size) {
    return cap && src < dst + cap && dst < src + size;
}

/* A call of the game's ZSTD_decompressMultiFrame. 1: decoded here, *ret its result (the decoded size). 0: the game's
   own function is to run, with its arguments as they are */
static int hz_oneshot(uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict, uint64_t dict_size,
                      uint64_t ddict, uint64_t *ret) {
    if (hz_in_place(dst, cap, src, size)) {
        hz_add(&hz_stats.inplace, 1), hz_add(&hz_stats.game, 1);
        return 0;
    }
    int ok = 0;
    const uint64_t t0 = (uint64_t)HZ_NOW_NS();
    const size_t r = (cap && !dst) || !HZ_OK(dst, cap) ? 0 : hz_decode(HZ_PTR(dst), cap, src, size, dict, dict_size, ddict, &ok);
    if (!ok || ZSTD_isError(r)) {   /* (the game's decoder decides, and gives its own error code) */
        static int logged;
        if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 8)
            HZ_LOG("zstd: %llu bytes at %#llx go to the game's decoder: %s", (unsigned long long)size, (unsigned long long)src,
                   !ok ? "inputs not taken" : ZSTD_getErrorName(r));
        hz_add(&hz_stats.game, 1);
        return 0;
    }
    if (r) HZ_WRITTEN(dst, r);
    *ret = r;
    hz_count(size, r, (uint64_t)HZ_NOW_NS() - t0);
    return 1;
}

/* The same call in check mode: the game's function runs and its result stands; the host decodes into memory of its
   own first, and the two are compared */
static uint64_t hz_oneshot_check(uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict, uint64_t dict_size,
                                 uint64_t ddict, HzGame game, void *user) {
    if (hz_in_place(dst, cap, src, size)) {
        hz_add(&hz_stats.inplace, 1), hz_add(&hz_stats.game, 1);
        return game(user);
    }
    uint8_t *mine = cap ? (uint8_t *)malloc((size_t)cap) : NULL;
    int ok = 0;
    const uint64_t t0 = (uint64_t)HZ_NOW_NS();
    const size_t r = cap && !mine ? 0 : hz_decode(mine, cap, src, size, dict, dict_size, ddict, &ok);
    const uint64_t ns = (uint64_t)HZ_NOW_NS() - t0;
    const uint64_t g = game(user);
    const int host_ok = ok && !ZSTD_isError(r);
    if (!host_ok) {   /* (outside check mode this call would have gone to the game: nothing to compare) */
        hz_add(&hz_stats.game, 1);
        static int logged;
        if (!hz_is_error(g) && __atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 8)
            HZ_LOG("zstd check: %llu bytes at %#llx the game decoded (%llu bytes) and the host would have left to it: %s",
                   (unsigned long long)size, (unsigned long long)src, (unsigned long long)g,
                   !ok ? "inputs not taken" : ZSTD_getErrorName(r));
    } else {
        hz_add(&hz_stats.checked, 1);
        if (g != r || (r && (!HZ_OK(dst, r) || memcmp(mine, HZ_PTR(dst), r)))) {
            hz_add(&hz_stats.differ, 1);
            static int logged;
            if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 12) {
                /* (Triforce) and what the input's first frame says of itself: its size in the input and its content's
                   (-1: not in its header, -2: no frame there), which tell one frame from several and a short output
                   from other bytes */
                const size_t first = ZSTD_findFrameCompressedSize(HZ_PTR(src), (size_t)size);
                const unsigned long long content = ZSTD_getFrameContentSize(HZ_PTR(src), (size_t)size);
                HZ_LOG("zstd check: MISMATCH: %llu bytes at %#llx (room %llu, dict %#llx/%llu, ddict %#llx; its first frame %lld "
                       "bytes, content %lld; called from %#llx): the host decoded %llu bytes, the game answered %#llx%s",
                       (unsigned long long)size, (unsigned long long)src, (unsigned long long)cap, (unsigned long long)dict,
                       (unsigned long long)dict_size, (unsigned long long)ddict, ZSTD_isError(first) ? -1ll : (long long)first,
                       (long long)content, (unsigned long long)hz_caller, (unsigned long long)r, (unsigned long long)g,
                       g == r ? " (the same size, other bytes)" : "");
            }
        }
    }
    free(mine);
    hz_count(size, host_ok ? r : 0, ns);
    return g;
}

/* ---- the stream feed ---- */
typedef struct {
    uint64_t s;              /* the game's stream state's address; 0: free */
    ZSTD_DCtx *dctx;         /* stays with the slot from stream to stream */
    uint8_t *shadow;         /* check mode: the host's output */
    uint64_t content, pos;   /* the frame's size; bytes the host wrote */
    uint64_t dst0, cap0;     /* the game's output at the begin: every feed writes on into this one buffer */
    uint64_t in;             /* compressed bytes fed */
    uint64_t ddict;          /* the game's ZSTD_DDict the stream began with (0: none), for the check's lines */
    int spent;               /* all the content is out: the game feeds no more, and the slot may be taken */
    int done, bad, moved;    /* check mode: the host reached the frame's end / failed; the game's output moved */
} HzStream;
static HzStream hz_streams[HZ_NSTREAM];

static HzStream *hz_stream_find(uint64_t s) {
    HzStream *st = NULL;
    HZ_LOCK();
    for (int i = 0; i < HZ_NSTREAM && !st; i++)
        if (hz_streams[i].s == s) st = &hz_streams[i];
    HZ_UNLOCK();
    return st;
}

/* The slot of s, or a free one for it, or one whose stream is spent; NULL when there is none (that stream decodes in
   the game) */
static HzStream *hz_stream_take(uint64_t s) {
    HzStream *st = NULL;
    HZ_LOCK();
    for (int i = 0; i < HZ_NSTREAM && !st; i++)
        if (hz_streams[i].s == s) st = &hz_streams[i];
    for (int i = 0; i < HZ_NSTREAM && !st; i++)
        if (!hz_streams[i].s) st = &hz_streams[i];
    for (int i = 0; i < HZ_NSTREAM && !st; i++)
        if (hz_streams[i].spent) st = &hz_streams[i];
    if (st) st->s = s, st->spent = 0;
    HZ_UNLOCK();
    return st;
}

static void hz_stream_drop(HzStream *st) {
    if (!st) return;
    free(st->shadow);
    st->shadow = NULL;
    HZ_LOCK();
    st->s = 0, st->spent = 0;
    HZ_UNLOCK();
}

/* Drops the slot of stream state s, if it has one. Found and cleared under one hold of the lock: between a find and a
   drop another thread can take a spent slot for its own stream (hz_stream_take), and the drop then cleared that
   thread's slot, which a third thread took as free and reset the decoder context in use */
static void hz_stream_drop_of(uint64_t s) {
    HZ_LOCK();
    for (int i = 0; i < HZ_NSTREAM; i++)
        if (hz_streams[i].s == s) {
            free(hz_streams[i].shadow);
            hz_streams[i].shadow = NULL;
            hz_streams[i].s = 0, hz_streams[i].spent = 0;
        }
    HZ_UNLOCK();
}

static void hz_stream_spent(HzStream *st) {
    HZ_LOCK();
    st->spent = 1;
    HZ_UNLOCK();
}

/* A context for streams: the output is one buffer for the whole frame, so the decoder allocates no window (a window
   of its own would be as large as the frame's, and a frame may name one as large as its content), and it takes any
   window size, as the game's decoder does */
static ZSTD_DCtx *hz_stream_dctx(void) {
    ZSTD_DCtx *d = ZSTD_createDCtx();
    if (d && (ZSTD_isError(ZSTD_DCtx_setParameter(d, ZSTD_d_stableOutBuffer, 1)) ||
              ZSTD_isError(ZSTD_DCtx_setParameter(d, ZSTD_d_windowLogMax, ZSTD_WINDOWLOG_MAX)))) {
        ZSTD_freeDCtx(d);
        d = NULL;
    }
    return d;
}

/* Feeds in to the stream's context until the input is used up, the frame ends (0), an error, or nothing moves. With
   the virtual core given up for large chunks, as hz_decode */
static size_t hz_stream_run(HzStream *st, ZSTD_outBuffer *out, ZSTD_inBuffer *in) {
    const int away = in->size >= HZ_AWAY_BYTES ? HZ_AWAY() : 0;
    if (away) hz_add(&hz_stats.away, 1);
    size_t r = 1;
    while (in->pos < in->size) {
        const size_t i0 = in->pos, o0 = out->pos;
        r = ZSTD_decompressStream(st->dctx, out, in);
        if (ZSTD_isError(r) || !r || (in->pos == i0 && out->pos == o0)) break;
    }
    HZ_BACK(away);
    return r;
}

static void hz_stream_count(const HzStream *st, uint64_t out) {
    const uint64_t n = __atomic_add_fetch(&hz_stats.streams, 1, __ATOMIC_RELAXED);
    hz_add(&hz_stats.s_in, st->in), hz_add(&hz_stats.s_out, out);
    if (n == 1 || !(n & 0x3ff))
        HZ_LOG("zstd: %llu streams decoded on the host in %llu feeds, %.1f MB in, %.1f MB out, %.0f ms; %llu streams left to the "
               "game's decoder; check: %llu compared, %llu differ", (unsigned long long)n, (unsigned long long)hz_stats.feeds,
               hz_stats.s_in / 1048576.0, hz_stats.s_out / 1048576.0, hz_stats.s_ns / 1e6, (unsigned long long)hz_stats.s_game,
               (unsigned long long)hz_stats.s_checked, (unsigned long long)hz_stats.s_differ);
}

/* A stream at its begin (phase 1): its slot, the host context ready for the frame, or NULL when the game's feed is to
   decode it (see the top). check: the game's state is left alone, and the host's output is memory of its own */
static HzStream *hz_stream_begin(uint64_t s, uint64_t src, int32_t size, uint64_t ddict, int check) {
    if (size <= 0 || !src || !HZ_OK(src, (uint64_t)size)) return NULL;
    ZSTD_FrameHeader h;
    if (ZSTD_getFrameHeader(&h, HZ_PTR(src), (size_t)size) != 0 || h.frameType != ZSTD_frame || !h.frameContentSize ||
        h.frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN || h.frameContentSize > 0x7fffffffull)
        return NULL;
    const uint64_t dst = hz_ld64(s + 8), ws = hz_ld64(s + 0x18);
    const int32_t cap = (int32_t)hz_ld32(s + 0x10);
    /* an output without room for the content fails part way in the game, with what fits written: left to the game */
    if (cap < 0 || (uint64_t)cap < h.frameContentSize || !dst || !HZ_OK(dst, (uint64_t)cap)) return NULL;
    if (src < dst + (uint64_t)cap && dst < src + (uint64_t)size) return NULL;   /* in place: the game's decoder */
    /* (the game makes its DCtx there, and none in a workspace not aligned to 8) */
    if (!ws || (ws & 7) || !HZ_OK(ws, HZ_DCTX_EXPECTED + 8)) return NULL;
    ZSTD_DDict *dd = NULL;
    if (!hz_dict_of(0, 0, ddict, &dd)) return NULL;
    HzStream *st = hz_stream_take(s);
    if (!st) return NULL;
    free(st->shadow);
    st->shadow = NULL, st->content = h.frameContentSize, st->pos = 0, st->dst0 = dst, st->cap0 = (uint64_t)cap;
    st->in = 0, st->ddict = ddict, st->done = st->bad = st->moved = 0;
    if ((!st->dctx && !(st->dctx = hz_stream_dctx())) ||
        ZSTD_isError(ZSTD_DCtx_reset(st->dctx, ZSTD_reset_session_only)) ||
        ZSTD_isError(ZSTD_DCtx_refDDict(st->dctx, dd)) ||
        (check && !(st->shadow = (uint8_t *)malloc((size_t)st->content)))) {
        hz_stream_drop(st);
        return NULL;
    }
    if (!check) {   /* what the game's begin leaves in its state */
        hz_st32(s + 0x14, (uint32_t)st->content);
        hz_st64(s + 0x30, ws);
        *(uint8_t *)HZ_PTR(s) = 2;
        HZ_WRITTEN(s, 0x38);
        hz_st64(ws + HZ_DCTX_EXPECTED, 0);   /* (see HZ_DCTX_EXPECTED) */
        HZ_WRITTEN(ws + HZ_DCTX_EXPECTED, 8);
    }
    return st;
}

/* A feed of a host stream: the game's fields and the result as the game's feed leaves them */
static uint64_t hz_stream_feed(HzStream *st, uint64_t s, uint64_t src, int32_t size) {
    if (size <= 0) return hz_ld32(s + 0x14);   /* nothing to feed: the content left, as the game */
    const uint64_t t0 = (uint64_t)HZ_NOW_NS(), dst = hz_ld64(s + 8);
    const uint32_t cap = hz_ld32(s + 0x10);
    uint32_t ret;
    if (!src || !HZ_OK(src, (uint64_t)size) || dst != st->dst0 + st->pos || cap != st->cap0 - st->pos) {
        static int logged;
        if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 8)
            HZ_LOG("zstd: a host stream cannot go on after %llu bytes out: input %#llx + %d, output at %#llx with room for %u "
                   "where %#llx with %llu was left", (unsigned long long)st->pos, (unsigned long long)src, size,
                   (unsigned long long)dst, cap, (unsigned long long)(st->dst0 + st->pos), (unsigned long long)(st->cap0 - st->pos));
        *(uint8_t *)HZ_PTR(s) = 3;
        HZ_WRITTEN(s, 1);
        hz_stream_drop(st);
        return 0xffffffffu;
    }
    ZSTD_inBuffer in = {HZ_PTR(src), (size_t)size, 0};
    ZSTD_outBuffer out = {HZ_PTR(st->dst0), (size_t)st->cap0, (size_t)st->pos};
    const size_t r = hz_stream_run(st, &out, &in);
    const uint32_t wrote = (uint32_t)(out.pos - st->pos);
    st->pos = out.pos, st->in += in.pos;
    hz_st64(s + 8, dst + wrote);
    hz_st32(s + 0x10, cap - wrote);
    hz_st32(s + 0x14, hz_ld32(s + 0x14) - wrote);
    if (wrote) HZ_WRITTEN(dst, wrote);
    hz_add(&hz_stats.feeds, 1);
    hz_add(&hz_stats.s_ns, (uint64_t)HZ_NOW_NS() - t0);
    int end = 1;
    if (ZSTD_isError(r)) {
        *(uint8_t *)HZ_PTR(s) = 3;
        ret = ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall ? 0xfffffffeu : 0xffffffffu;
        static int logged;
        if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 8)
            HZ_LOG("zstd: a host stream failed after %llu bytes in: %s", (unsigned long long)st->in, ZSTD_getErrorName(r));
    } else if (!r && in.pos < in.size) {   /* input after the frame's end */
        *(uint8_t *)HZ_PTR(s) = 3;
        ret = 0xfffffffdu;
    } else if (!r) {
        *(uint8_t *)HZ_PTR(s) = 4;
        ret = hz_ld32(s + 0x14);
    } else if (in.pos < in.size) {   /* nothing moved with input left */
        *(uint8_t *)HZ_PTR(s) = 3;
        ret = 0xfffffffeu;
    } else {
        ret = hz_ld32(s + 0x14);
        end = 0;
        if (!ret) hz_stream_spent(st);
    }
    HZ_WRITTEN(s, 0x38);
    if (end) {
        hz_stream_count(st, st->pos);
        hz_stream_drop(st);
    }
    return ret;
}

/* Check mode: the host decodes the chunk into its own buffer, the game's feed runs as always and its result stands;
   when the game ends the stream, or has all its content out, the two outputs are compared */
static uint64_t hz_stream_check(HzStream *st, uint64_t s, uint64_t src, int32_t size, HzGame game, void *user) {
    if (st->spent) {   /* compared already: the game was not expected to feed on after a result of 0 */
        static int logged;
        if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 4)
            HZ_LOG("zstd check: a feed of %d bytes after the content of a stream was out (content %llu)", size,
                   (unsigned long long)st->content);
        const uint64_t g = game(user);
        const uint8_t ph = *(const uint8_t *)HZ_PTR(s);
        if (ph == 3 || ph == 4) hz_stream_drop(st);
        return g;
    }
    if (!st->bad && !st->done && size > 0 && src && HZ_OK(src, (uint64_t)size)) {
        ZSTD_inBuffer in = {HZ_PTR(src), (size_t)size, 0};
        ZSTD_outBuffer out = {st->shadow, (size_t)st->content, (size_t)st->pos};
        const uint64_t t0 = (uint64_t)HZ_NOW_NS();
        const size_t r = hz_stream_run(st, &out, &in);
        hz_add(&hz_stats.feeds, 1);
        hz_add(&hz_stats.s_ns, (uint64_t)HZ_NOW_NS() - t0);
        st->pos = out.pos, st->in += in.pos;
        if (ZSTD_isError(r)) st->bad = 1;
        else if (!r) st->done = in.pos == in.size ? 1 : 2;   /* 2: input after the frame's end */
        else if (in.pos < in.size) st->bad = 1;
    }
    const uint32_t g = (uint32_t)game(user);
    const uint8_t ph = *(const uint8_t *)HZ_PTR(s);
    /* a host stream writes into the buffer the stream began with: the game's output has to advance through it alone */
    if ((ph == 2 || ph == 4) && hz_ld64(s + 8) - st->dst0 != st->content - hz_ld32(s + 0x14)) st->moved = 1;
    const int out_all = ph == 2 && !g;   /* the job feeds no more, with or without the frame's last bytes */
    if (ph != 3 && ph != 4 && !out_all) return g;
    hz_add(&hz_stats.s_checked, 1);
    const int same = ph == 3 ? st->bad || st->done != 1   /* the game failed: the host must not have a clean frame */
                             : !st->bad && !st->moved && st->done != 2 && (ph != 4 || st->done == 1) &&
                                   st->pos == st->content && HZ_OK(st->dst0, st->content) &&
                                   !memcmp(st->shadow, HZ_PTR(st->dst0), (size_t)st->content);
    if (!same) {
        hz_add(&hz_stats.s_differ, 1);
        static int logged;
        if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 12)
            HZ_LOG("zstd check: stream MISMATCH: the game ended in phase %d with %#x after %llu bytes in (content %llu, ddict %#llx, "
                   "called from %#llx)%s; the host wrote %llu bytes, %s", (int)ph, g, (unsigned long long)st->in,
                   (unsigned long long)st->content, (unsigned long long)st->ddict, (unsigned long long)hz_caller,
                   st->moved ? ", its output not advancing through one buffer" : "", (unsigned long long)st->pos,
                   st->bad ? "failed" : st->done == 1 ? "reached the frame's end"
                   : st->done ? "found input after the frame's end" : "had not reached the frame's end");
    }
    hz_stream_count(st, st->pos);
    if (out_all) {   /* keeps the slot in case the game does feed on; nothing more is compared */
        free(st->shadow);
        st->shadow = NULL;
        hz_stream_spent(st);
    } else {
        hz_stream_drop(st);
    }
    return g;
}

/* A call of the game's stream feed. 1: fed here, *ret its result. 0: the game's own feed is to run. check (with the
   game's function to run): always 1, the game's result */
static int hz_stream(uint64_t s, uint64_t src, int32_t size, uint64_t ddict, int check, HzGame game, void *user, uint64_t *ret) {
    HzStream *st = NULL;
    if (s && HZ_OK(s, 0x38)) {
        const uint8_t ph = *(const uint8_t *)HZ_PTR(s);
        if (ph == 1) {
            hz_stream_drop_of(s);   /* a stream the game gave up without ending it */
            st = hz_stream_begin(s, src, size, ddict, check);
            if (!st) hz_add(&hz_stats.s_game, 1);
        } else if (ph == 2) {
            st = hz_stream_find(s);
        }
    }
    if (check) {
        *ret = st ? hz_stream_check(st, s, src, size, game, user) : game(user);
        return 1;
    }
    if (!st) return 0;
    *ret = hz_stream_feed(st, s, src, size);
    return 1;
}
#endif
