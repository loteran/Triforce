"""The host's zstd decoder for the game's files (host/host_zstd.h, host/hos_zstd.c, nx2/oot_patches.py patch_host_zstd).

No game: host_zstd.h is compiled with zstd's decoder (third_party/zstd, the files nx2/build.py compiles for the runtime)
and a buffer standing in for the game's memory, and run on frames made here with the same zstd (the zstandard package).

What the game's two functions answer was read from their translated code and is written out in host_zstd.h. The checks:

One call, one buffer (the game's ZSTD_decompressMultiFrame, hz_oneshot): a frame, two frames, a skippable frame and a
frame, frames with a dictionary (raw bytes, a guest ZSTD_DDict, a trained dictionary with its id, two frames with it in
one call) decode to the same bytes and the decoded size, and the pages written are told to the write tracking. Left to
the game's own function, with nothing answered here (so its own answer stands: a size, or zstd's error code, which its
callers turn into 0xfffffffe for "output too small" and 0xffffffff for the rest): a truncated input, an output smaller
than the frame (or than two frames), input inside the output, under 5 bytes, damaged input, a wrong checksum alone,
bytes after a frame that are no frame, pointers outside the game's memory, a dictionary that cannot be taken, a frame
that names a dictionary given none.

The stream feed (hz_stream), over a state laid out as the game's: a frame fed in one piece, in pieces of 64 KiB, of 4099
bytes and of 977 bytes gives after every feed the content left as the result, the output position and room moved by the
bytes written, phase 2 until the feed that ends the frame and phase 4 at it, and the game's bytes. A frame with a
checksum gives 0 once its content is out and stays in phase 2 until the checksum came. A truncated input stays in phase
2 with content left: the stream waits, as the game's does. Input after the frame's end: phase 3 and 0xfffffffd. A damaged
frame: phase 3 and 0xffffffff or 0xfffffffe. A wrong checksum: the content out (0), then at the checksum's feed phase 3
and 0xffffffff. An output that moved between feeds: phase 3 and 0xffffffff. Left to the game's own feed: a first chunk
without the whole header, a frame without its content size, an output smaller than the content (the game gives
0xfffffffe part way), an output that overlaps the input, a workspace that is not aligned, a dictionary that cannot be
taken, a seventeenth stream at once, and every feed of a stream in phase 3 or 4 (the game answers 0xffffffff and
0xfffffffd itself). A host stream's begin sets the game's DCtx's "expected" field to 0, which makes the game's own feed
answer "input after the frame's end" should it ever be handed such a stream.

Check mode: with a stand-in for the game's function the game's result always stands; equal outputs count as compared,
a spoiled byte or another result as a difference.

The patch: the two calls go in at the heads of the two functions, once, only where the function's code is the
decoder's, and a second run changes nothing. The functions are written out below as the lift has them before the
patches (the lines the patch looks for in the lifter's spelling, code made up around them), and the lifter's own pass of
page marks (a64_lift.mark_groups, run after the patches) makes them as the lift on disk has them: both calls are found in
either, and patching then marking gives what marking then patching gives. (The stream feed's begin stores are such a
group: the patch once looked for their marked spelling, which the text it runs over never has, and no build had the
stream feed's call.) A function at that address with any one of those lines changed, or twice, gets no call, before the
marks or after; nor does another function with the same lines, nor a function with the call anywhere but at its head.
The source: the switch is off unless HOS_HOST_ZSTD is 1 or 2, the build compiles zstd's decoder for the runtime, and
the switch is in docs/development.md.

    python -I -B tests/test_host_zstd.py [--cc gcc]
"""
import argparse
import ast
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import zstandard

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_MODULES = ["zstandard"]

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "nx2"), str(ROOT / "third_party/a64-kit/lift")]
import a64_lift  # noqa: E402
import oot_patches  # noqa: E402

FIXTURE = r'''
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the game's memory: one buffer at a made-up guest address */
#define BASE 0x20000000ull
#define ARENA (32u << 20)
static uint8_t *arena;
static uint8_t marks[ARENA >> 12];
static int logs, mismatches, aways, backs, verbose;
static char last_log[1024];
__attribute__((format(printf, 1, 2))) static void log_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
    logs++;
    if (strstr(last_log, "MISMATCH")) mismatches++;
    if (verbose) printf("  log: %s\n", last_log);
}
static void mark(uint64_t a, uint64_t n) {
    for (uint64_t p = (a - BASE) >> 12; n && p <= (a + n - 1 - BASE) >> 12 && p < (ARENA >> 12); p++) marks[p] = 1;
}
#define HZ_PTR(a) ((void *)(arena + ((uint64_t)(a) - BASE)))
#define HZ_OK(a, n) ((uint64_t)(a) >= BASE && (uint64_t)(a) - BASE <= ARENA && (uint64_t)(n) <= ARENA - ((uint64_t)(a) - BASE))
#define HZ_WRITTEN(a, n) mark((a), (n))
#define HZ_LOG(...) log_line(__VA_ARGS__)
#define HZ_LOCK() ((void)0)
#define HZ_UNLOCK() ((void)0)
#define HZ_AWAY() (aways++, 1)
#define HZ_BACK(x) do { if (x) backs++; } while (0)
#define HZ_NOW_NS() 0
#include "host_zstd.h"

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int marked(uint64_t a, uint64_t n) {
    for (uint64_t p = (a - BASE) >> 12; p <= (a + n - 1 - BASE) >> 12; p++)
        if (!marks[p]) return 0;
    return 1;
}
static int any_mark(void) {
    for (size_t p = 0; p < sizeof marks; p++)
        if (marks[p]) return 1;
    return 0;
}
static void clear_marks(void) { memset(marks, 0, sizeof marks); }

static uint8_t *load(const char *dir, const char *name, size_t *n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = (uint8_t *)malloc(*n + 1);
    if (fread(p, 1, *n, f) != *n) { printf("short read of %s\n", path); exit(2); }
    fclose(f);
    return p;
}

/* the places in the game's memory */
enum { MB = 1 << 20 };
#define COMPA (BASE + 1 * MB)     /* compressed input, up to 6 MB */
#define OUTA (BASE + 8 * MB)      /* output, up to 6 MB */
#define SA (BASE + 16 * MB)       /* stream states, 0x40 apart */
#define WSA (SA + 0x10000)        /* a DCtx workspace (only its address counts here) */
#define DICTA (BASE + 20 * MB)    /* a dictionary's bytes */
#define DDICTA (BASE + 22 * MB)   /* a guest ZSTD_DDict: dictBuffer, dictContent, dictSize */
static void put(uint64_t a, const uint8_t *p, size_t n) { memcpy(HZ_PTR(a), p, n); }
static void guest_ddict(uint64_t at, uint64_t content, uint64_t size) {
    hz_st64(at, content), hz_st64(at + 8, content), hz_st64(at + 16, size);
}

/* ---- one call ---- */
static uint64_t once(uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict, uint64_t dn, uint64_t ddict) {
    uint64_t r = 0xdeadbeefull;
    return hz_oneshot(dst, cap, src, size, dict, dn, ddict, &r) ? r : (r == 0xdeadbeefull ? (1ull << 62) : (1ull << 63));
}
#define GAME1 (1ull << 62)   /* left to the game's function, nothing answered */

/* a stand-in for the game's function in check mode: decodes with a context of its own, may spoil its output or answer
   something else */
typedef struct { uint64_t dst, cap, src, size; const uint8_t *dict; size_t dn; int spoil; uint64_t answer; int calls; } Fake1;
static uint64_t fake_once(void *user) {
    Fake1 *f = (Fake1 *)user;
    f->calls++;
    ZSTD_DCtx *d = ZSTD_createDCtx();
    size_t r = ZSTD_decompress_usingDict(d, HZ_PTR(f->dst), (size_t)f->cap, HZ_PTR(f->src), (size_t)f->size, f->dict, f->dn);
    ZSTD_freeDCtx(d);
    if (f->spoil && !ZSTD_isError(r) && r) ((uint8_t *)HZ_PTR(f->dst))[r / 2] ^= 0x40;
    return f->answer ? f->answer : (uint64_t)r;
}

/* ---- the stream feed ---- */
static void state(uint64_t s, uint64_t dst, uint32_t cap, uint64_t ws) {
    memset(HZ_PTR(s), 0, 0x38);
    *(uint8_t *)HZ_PTR(s) = 1;
    hz_st64(s + 8, dst), hz_st32(s + 0x10, cap), hz_st64(s + 0x18, ws);
}
static int phase(uint64_t s) { return *(const uint8_t *)HZ_PTR(s); }
#define GAME (1ull << 32)   /* the feed is the game's */
static uint64_t feed(uint64_t s, uint64_t src, int32_t size, uint64_t ddict) {
    uint64_t r = 0;
    return hz_stream(s, src, size, ddict, 0, NULL, NULL, &r) ? r : GAME;
}

/* Feeds frame[0, n) in pieces of `chunk` bytes after a first one of `first`, and checks the state after every feed */
static void whole(const char *what, uint64_t s, uint64_t frame, size_t n, uint64_t out, const uint8_t *raw, size_t rawn,
                  size_t first, size_t chunk, uint64_t ddict) {
    const uint32_t cap = (uint32_t)rawn + 4096;
    state(s, out, cap, WSA);
    memset(HZ_PTR(out), 0xA5, rawn);
    clear_marks();
    size_t off = 0, feeds = 0;
    uint64_t r = 1;
    while (off < n) {
        size_t k = off ? chunk : first;
        if (k > n - off) k = n - off;
        r = feed(s, frame + off, (int32_t)k, ddict);
        off += k, feeds++;
        const uint64_t wrote = hz_ld64(s + 8) - out;
        const int ph = phase(s);
        if (r >> 32 || wrote > rawn || hz_ld32(s + 0x10) != cap - wrote || hz_ld32(s + 0x14) != rawn - wrote ||
            r != rawn - wrote || (ph != 2 && ph != 4) || (ph == 4) != (off == n)) {
            CHECK(0, "%s: the feed ending at %zu of %zu gave %#llx in phase %d with %llu bytes out, room %u, left %u", what, off, n,
                  (unsigned long long)r, ph, (unsigned long long)wrote, hz_ld32(s + 0x10), hz_ld32(s + 0x14));
            hz_stream_drop(hz_stream_find(s));
            return;
        }
    }
    CHECK(r == 0 && phase(s) == 4, "%s: ended with %#llx in phase %d", what, (unsigned long long)r, phase(s));
    CHECK(hz_ld64(s + 0x30) == WSA, "%s: the state's DCtx field is not its workspace", what);
    CHECK(!memcmp(HZ_PTR(out), raw, rawn), "%s: the output differs from the source", what);
    CHECK(!hz_stream_find(s), "%s: the slot is still taken after the frame's end", what);
    CHECK(marked(out, rawn) && marked(s, 0x38), "%s: pages written were not told to the write tracking", what);
    if (verbose) printf("stream: %s: %zu bytes in %zu feeds gave %zu\n", what, n, feeds, rawn);
}

/* a stand-in for the game's feed in check mode: zstd's stream decoder with a window of its own, the state's fields
   kept as the game keeps them */
typedef struct { uint64_t s, src; int32_t size; ZSTD_DCtx *d; int spoil, stop_at_content; } Fake2;
static uint64_t fake_feed(void *user) {
    Fake2 *f = (Fake2 *)user;
    const uint64_t s = f->s;
    if (phase(s) == 3) return 0xffffffffu;
    if (phase(s) == 4) return 0xfffffffdu;
    if (phase(s) == 1) {
        ZSTD_FrameHeader h;
        if (ZSTD_getFrameHeader(&h, HZ_PTR(f->src), (size_t)f->size) != 0 || h.frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN) {
            *(uint8_t *)HZ_PTR(s) = 3;
            return 0xffffffffu;
        }
        hz_st32(s + 0x14, (uint32_t)h.frameContentSize);
        hz_st64(s + 0x30, hz_ld64(s + 0x18));
        *(uint8_t *)HZ_PTR(s) = 2;
        ZSTD_DCtx_reset(f->d, ZSTD_reset_session_only);
    }
    ZSTD_inBuffer in = {HZ_PTR(f->src), (size_t)f->size, 0};
    size_t r = 1;
    while (in.pos < in.size) {
        ZSTD_outBuffer out = {HZ_PTR(hz_ld64(s + 8)), hz_ld32(s + 0x10), 0};
        const size_t i0 = in.pos;
        r = ZSTD_decompressStream(f->d, &out, &in);
        hz_st64(s + 8, hz_ld64(s + 8) + out.pos);
        hz_st32(s + 0x10, hz_ld32(s + 0x10) - (uint32_t)out.pos);
        hz_st32(s + 0x14, hz_ld32(s + 0x14) - (uint32_t)out.pos);
        if (ZSTD_isError(r)) {
            *(uint8_t *)HZ_PTR(s) = 3;
            return ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall ? 0xfffffffeu : 0xffffffffu;
        }
        if (!r || (in.pos == i0 && !out.pos)) break;
    }
    if (!r && in.pos < in.size) { *(uint8_t *)HZ_PTR(s) = 3; return 0xfffffffdu; }
    if (!r) *(uint8_t *)HZ_PTR(s) = 4;
    if (f->spoil && !hz_ld32(s + 0x14)) ((uint8_t *)HZ_PTR(hz_ld64(s + 8)))[-7] ^= 0x10, f->spoil = 0;
    return hz_ld32(s + 0x14);
}
/* the whole frame through check mode in pieces of `chunk`: the last result */
static uint64_t checked(Fake2 *f, uint64_t s, uint64_t frame, size_t n, uint64_t out, uint32_t cap, size_t chunk) {
    state(s, out, cap, WSA);
    uint64_t r = 1;
    for (size_t off = 0; off < n; off += chunk) {
        f->s = s, f->src = frame + off, f->size = (int32_t)(n - off < chunk ? n - off : chunk);
        uint64_t got = 77;
        const int handled = hz_stream(s, f->src, f->size, 0, 1, fake_feed, f, &got);
        CHECK(handled == 1, "check mode did not answer a feed");
        r = got;
        if (r >> 31 || (!r && f->stop_at_content)) break;
    }
    return r;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    verbose = argc > 2;
    arena = (uint8_t *)calloc(ARENA, 1);
    size_t RAWN, AN, SMALLN, SN, NOSIZEN, TWON, SKIPN, DICTN, DN, TDICTN, TN, TRAWN, D2N, S2N;
    uint8_t *raw = load(argv[1], "raw.bin", &RAWN), *A = load(argv[1], "a.zst", &AN);
    uint8_t *small = load(argv[1], "small.bin", &SMALLN), *S = load(argv[1], "small.zst", &SN);
    uint8_t *nosize = load(argv[1], "nosize.zst", &NOSIZEN), *two = load(argv[1], "two.zst", &TWON);
    uint8_t *skip = load(argv[1], "skip.zst", &SKIPN), *dict = load(argv[1], "dict.bin", &DICTN);
    uint8_t *D = load(argv[1], "dicted.zst", &DN), *tdict = load(argv[1], "tdict.bin", &TDICTN);
    uint8_t *T = load(argv[1], "tdicted.zst", &TN), *traw = load(argv[1], "traw.bin", &TRAWN);
    uint8_t *D2 = load(argv[1], "dicted2.zst", &D2N), *small2 = load(argv[1], "small2.bin", &S2N);
    if (RAWN != 3 * MB || AN < HZ_AWAY_BYTES || AN > 5 * MB || SN < 2048 || SMALLN != 100000 || S2N != SMALLN) {
        printf("the frames are not as the checks expect (%zu, %zu, %zu)\n", RAWN, AN, SN);
        return 2;
    }
    uint64_t r;

    /* ================= one call, one buffer ================= */
    put(COMPA, A, AN);
    memset(HZ_PTR(OUTA), 0xA5, RAWN + 4096);
    clear_marks();
    r = once(OUTA, RAWN + 4096, COMPA, AN, 0, 0, 0);
    CHECK(r == RAWN, "a frame: answered %#llx, not its size", (unsigned long long)r);
    CHECK(!memcmp(HZ_PTR(OUTA), raw, RAWN), "a frame: other bytes");
    CHECK(((uint8_t *)HZ_PTR(OUTA))[RAWN + 4095] == 0xA5, "a frame: written past the room given");
    CHECK(marked(OUTA, RAWN), "a frame: its pages were not told to the write tracking");
    CHECK(aways == 1 && backs == 1, "a large input did not give the core up once and take it again (%d, %d)", aways, backs);
    r = once(OUTA, RAWN, COMPA, AN, 0, 0, 0);
    CHECK(r == RAWN, "room for exactly the content: %#llx", (unsigned long long)r);

    put(COMPA, two, TWON);
    memset(HZ_PTR(OUTA), 0, RAWN);
    r = once(OUTA, RAWN, COMPA, TWON, 0, 0, 0);
    CHECK(r == 2 * MB && !memcmp(HZ_PTR(OUTA), raw, 2 * MB), "two frames in one call: %#llx", (unsigned long long)r);

    put(COMPA, skip, SKIPN);
    r = once(OUTA, RAWN, COMPA, SKIPN, 0, 0, 0);
    CHECK(r == SMALLN && !memcmp(HZ_PTR(OUTA), small, SMALLN), "a skippable frame and a frame: %#llx", (unsigned long long)r);

    put(COMPA, S, SN);
    aways = backs = 0;
    r = once(OUTA, SMALLN, COMPA, SN, 0, 0, 0);
    CHECK(r == SMALLN && !memcmp(HZ_PTR(OUTA), small, SMALLN), "a frame with a checksum: %#llx", (unsigned long long)r);
    CHECK(SN >= HZ_AWAY_BYTES || (!aways && !backs), "a small input gave the core up");

    /* left to the game's own function: nothing answered, the game's memory as it was (but for bytes libzstd wrote before
       it gave up, in the output the game's decoder then writes over) */
    put(COMPA, A, AN);
    memset(HZ_PTR(OUTA), 0xA5, RAWN + 4096);
    clear_marks();
    CHECK(once(OUTA, RAWN + 4096, COMPA, AN - 7, 0, 0, 0) == GAME1, "a truncated input was answered here");
    CHECK(once(OUTA, RAWN - 1, COMPA, AN, 0, 0, 0) == GAME1, "an output smaller than the frame was answered here");
    CHECK(!any_mark(), "a call left to the game told the write tracking of pages");
    memset(HZ_PTR(OUTA), 0xA5, RAWN + 4096);
    put(OUTA + 4096, A, AN);
    const uint64_t inplace0 = hz_stats.inplace;
    CHECK(once(OUTA, RAWN + 4096, OUTA + 4096, AN, 0, 0, 0) == GAME1 && hz_stats.inplace == inplace0 + 1, "input inside the output was answered here");
    CHECK(!memcmp(HZ_PTR(OUTA + 4096), A, AN) && ((uint8_t *)HZ_PTR(OUTA))[0] == 0xA5, "input inside the output: something was written");
    CHECK(once(OUTA, RAWN, COMPA, 4, 0, 0, 0) == GAME1, "4 bytes of input were answered here");
    CHECK(once(OUTA, RAWN, COMPA, 0, 0, 0, 0) == GAME1, "no input was answered here");
    CHECK(once(OUTA, RAWN, 0, AN, 0, 0, 0) == GAME1, "a null input was answered here");
    CHECK(once(OUTA, RAWN, BASE + ARENA - 16, AN, 0, 0, 0) == GAME1, "an input past the game's memory was answered here");
    CHECK(once(BASE + ARENA - 16, RAWN, COMPA, AN, 0, 0, 0) == GAME1, "an output past the game's memory was answered here");
    CHECK(once(0, RAWN, COMPA, AN, 0, 0, 0) == GAME1, "a null output was answered here");
    put(COMPA, A, AN);
    ((uint8_t *)HZ_PTR(COMPA))[AN / 2] ^= 0x55, ((uint8_t *)HZ_PTR(COMPA))[AN / 2 + 1] ^= 0xAA;
    r = once(OUTA, RAWN + 4096, COMPA, AN, 0, 0, 0);
    CHECK(r == GAME1 || (r == RAWN && memcmp(HZ_PTR(OUTA), raw, RAWN)), "a damaged frame: %#llx", (unsigned long long)r);
    memset(HZ_PTR(COMPA), 0x11, 4096);
    CHECK(once(OUTA, RAWN, COMPA, 4096, 0, 0, 0) == GAME1, "bytes that are no frame were answered here");
    /* (Triforce) a frame whose checksum alone is wrong: its content decodes, but the verdict is the game's decoder's */
    put(COMPA, S, SN);
    ((uint8_t *)HZ_PTR(COMPA))[SN - 1] ^= 0x01;
    CHECK(once(OUTA, SMALLN, COMPA, SN, 0, 0, 0) == GAME1, "a frame with a wrong checksum was answered here");
    /* (Triforce) a frame and bytes after it that are no frame: zstd's "input not all used", the game's to give */
    put(COMPA, S, SN);
    memset(HZ_PTR(COMPA + SN), 0x11, 16);
    CHECK(once(OUTA, SMALLN, COMPA, SN + 16, 0, 0, 0) == GAME1, "a frame with other bytes after it was answered here");

    /* dictionaries */
    put(DICTA, dict, DICTN);
    put(COMPA, D, DN);
    memset(HZ_PTR(OUTA), 0, SMALLN);
    r = once(OUTA, SMALLN, COMPA, DN, DICTA, DICTN, 0);
    CHECK(r == SMALLN && !memcmp(HZ_PTR(OUTA), small, SMALLN), "a dictionary as raw bytes: %#llx", (unsigned long long)r);
    CHECK(hz_ndicts == 1, "one dictionary, %d host ones", hz_ndicts);
    guest_ddict(DDICTA, DICTA, DICTN);
    memset(HZ_PTR(OUTA), 0, SMALLN);
    r = once(OUTA, SMALLN, COMPA, DN, 0, 0, DDICTA);
    CHECK(r == SMALLN && !memcmp(HZ_PTR(OUTA), small, SMALLN), "a dictionary as the game's ZSTD_DDict: %#llx", (unsigned long long)r);
    CHECK(once(OUTA, SMALLN, COMPA, DN, 0, 0, DDICTA) == SMALLN && hz_ndicts == 2, "the same DDict again made another host dictionary (%d)", hz_ndicts);
    r = once(OUTA, SMALLN, COMPA, DN, 0, 0, 0);
    CHECK(r == GAME1 || memcmp(HZ_PTR(OUTA), small, SMALLN), "the frame decoded right without its dictionary");
    guest_ddict(DDICTA + 64, BASE + ARENA - 8, DICTN);
    CHECK(once(OUTA, SMALLN, COMPA, DN, 0, 0, DDICTA + 64) == GAME1, "a DDict whose bytes are past the game's memory was taken");
    guest_ddict(DDICTA + 64, DICTA, 0);
    CHECK(once(OUTA, SMALLN, COMPA, DN, 0, 0, DDICTA + 64) == GAME1, "a DDict of no bytes was taken");
    CHECK(once(OUTA, SMALLN, COMPA, DN, 0, 0, BASE + ARENA - 8) == GAME1, "a DDict past the game's memory was taken");
    /* another dictionary at the same address and of the same size: a new host dictionary, not the old one */
    for (size_t i = 0; i < DICTN; i++) ((uint8_t *)HZ_PTR(DICTA))[i] = (uint8_t)(dict[DICTN - 1 - i] ^ 0x5a);
    put(COMPA, D2, D2N);
    memset(HZ_PTR(OUTA), 0, SMALLN);
    r = once(OUTA, SMALLN, COMPA, D2N, 0, 0, DDICTA);
    CHECK(r == SMALLN && !memcmp(HZ_PTR(OUTA), small2, SMALLN) && hz_ndicts == 3, "new bytes at a dictionary's address: %#llx, %d host ones",
          (unsigned long long)r, hz_ndicts);
    r = once(OUTA, SMALLN, COMPA, D2N, 0, 0, 0);
    CHECK(r == GAME1 || memcmp(HZ_PTR(OUTA), small2, SMALLN), "that frame decoded right without its dictionary");
    /* a trained dictionary: the frame names its id */
    put(DICTA, tdict, TDICTN);
    guest_ddict(DDICTA, DICTA, TDICTN);
    put(COMPA, T, TN);
    memset(HZ_PTR(OUTA), 0, TRAWN);
    r = once(OUTA, TRAWN, COMPA, TN, 0, 0, DDICTA);
    CHECK(r == TRAWN && !memcmp(HZ_PTR(OUTA), traw, TRAWN), "a trained dictionary: %#llx", (unsigned long long)r);
    CHECK(once(OUTA, TRAWN, COMPA, TN, 0, 0, 0) == GAME1, "a frame that names a dictionary was answered here without one");
    /* (Triforce) two such frames in one call: the game's ZSTD_decompressMultiFrame begins each frame with the DDict */
    put(COMPA + TN, T, TN);
    memset(HZ_PTR(OUTA), 0, 2 * TRAWN);
    r = once(OUTA, 2 * TRAWN, COMPA, 2 * TN, 0, 0, DDICTA);
    CHECK(r == 2 * TRAWN && !memcmp(HZ_PTR(OUTA), traw, TRAWN) && !memcmp(HZ_PTR(OUTA + TRAWN), traw, TRAWN),
          "two frames with a dictionary in one call: %#llx", (unsigned long long)r);
    CHECK(once(OUTA, 2 * TRAWN - 1, COMPA, 2 * TN, 0, 0, DDICTA) == GAME1, "two frames with room for all but a byte were answered here");

    /* check mode: the game's result stands */
    put(COMPA, A, AN);
    Fake1 f1 = {OUTA, RAWN + 4096, COMPA, AN, NULL, 0, 0, 0, 0};
    uint64_t c0 = hz_stats.checked, d0 = hz_stats.differ;
    r = hz_oneshot_check(OUTA, RAWN + 4096, COMPA, AN, 0, 0, 0, fake_once, &f1);
    CHECK(r == RAWN && f1.calls == 1 && hz_stats.checked == c0 + 1 && hz_stats.differ == d0, "check: the same output counted as %llu compared, %llu different",
          (unsigned long long)(hz_stats.checked - c0), (unsigned long long)(hz_stats.differ - d0));
    f1.spoil = 1;
    r = hz_oneshot_check(OUTA, RAWN + 4096, COMPA, AN, 0, 0, 0, fake_once, &f1);
    CHECK(r == RAWN && hz_stats.differ == d0 + 1, "check: a spoiled byte was not a difference");
    CHECK(mismatches == 1, "check: the difference was not logged once (%d)", mismatches);
    f1.spoil = 0, f1.answer = (uint64_t)0 - 70;
    r = hz_oneshot_check(OUTA, RAWN + 4096, COMPA, AN, 0, 0, 0, fake_once, &f1);
    CHECK(r == (uint64_t)0 - 70 && hz_stats.differ == d0 + 2, "check: the game's error against a host decode: %#llx", (unsigned long long)r);
    f1.answer = 0, f1.size = AN - 7, c0 = hz_stats.checked, d0 = hz_stats.differ;
    r = hz_oneshot_check(OUTA, RAWN + 4096, COMPA, AN - 7, 0, 0, 0, fake_once, &f1);
    CHECK(hz_is_error(r) && hz_stats.checked == c0 && hz_stats.differ == d0, "check: a call the host leaves to the game was compared");
    f1.size = AN, f1.src = OUTA + 4096, f1.calls = 0;
    put(OUTA + 4096, A, AN);
    hz_oneshot_check(OUTA, RAWN + 4096, OUTA + 4096, AN, 0, 0, 0, fake_once, &f1);
    CHECK(f1.calls == 1 && hz_stats.checked == c0, "check: input inside the output was compared, or the game not run");

    /* ================= the stream feed ================= */
    put(COMPA, A, AN);
    whole("a 3 MB frame in pieces of 64 KiB", SA, COMPA, AN, OUTA, raw, RAWN, 65536, 65536, 0);
    whole("the same in pieces of 4099 bytes", SA, COMPA, AN, OUTA, raw, RAWN, 64, 4099, 0);
    whole("the same in one feed", SA, COMPA, AN, OUTA, raw, RAWN, AN, AN, 0);
    put(COMPA, S, SN);
    whole("a frame with a checksum in pieces of 977 bytes", SA, COMPA, SN, OUTA, small, SMALLN, 32, 977, 0);
    whole("the same with the checksum in a feed of its own", SA, COMPA, SN, OUTA, small, SMALLN, SN - 4, 4, 0);
    put(DICTA, tdict, TDICTN);
    guest_ddict(DDICTA, DICTA, TDICTN);
    put(COMPA, T, TN);
    whole("a frame with a dictionary", SA, COMPA, TN, OUTA, traw, TRAWN, 100, 300, DDICTA);

    /* left to the game's own feed */
    put(COMPA, A, AN);
    state(SA, OUTA, RAWN + 4096, WSA);
    clear_marks();
    CHECK(feed(SA, COMPA, 3, 0) == GAME && phase(SA) == 1, "a first chunk without the whole header was taken");
    state(SA, OUTA, RAWN - 1, WSA);
    CHECK(feed(SA, COMPA, 65536, 0) == GAME && phase(SA) == 1, "an output smaller than the content was taken");
    state(SA, COMPA + 4096, RAWN + 4096, WSA);
    CHECK(feed(SA, COMPA, 65536, 0) == GAME && phase(SA) == 1, "an output that overlaps its input was taken");
    state(SA, OUTA, RAWN + 4096, WSA + 4);
    CHECK(feed(SA, COMPA, 65536, 0) == GAME && phase(SA) == 1, "a workspace not aligned to 8 was taken");
    state(SA, OUTA, RAWN + 4096, 0);
    CHECK(feed(SA, COMPA, 65536, 0) == GAME && phase(SA) == 1, "a state without a workspace was taken");
    state(SA, OUTA, RAWN + 4096, BASE + ARENA - 4096);
    CHECK(feed(SA, COMPA, 65536, 0) == GAME && phase(SA) == 1, "a workspace with no room for a DCtx in the game's memory was taken");
    state(SA, OUTA, RAWN + 4096, WSA);
    CHECK(feed(SA, COMPA, 65536, BASE + ARENA - 8) == GAME && phase(SA) == 1, "a dictionary past the game's memory was taken");
    CHECK(feed(SA, COMPA, 0, 0) == GAME && feed(SA, COMPA, -5, 0) == GAME && phase(SA) == 1, "a first feed of no bytes was taken");
    CHECK(feed(SA, 0, 65536, 0) == GAME && feed(SA, BASE + ARENA - 16, 65536, 0) == GAME, "a first chunk outside the game's memory was taken");
    put(COMPA, nosize, NOSIZEN);
    CHECK(feed(SA, COMPA, (int32_t)NOSIZEN, 0) == GAME && phase(SA) == 1, "a frame without its content size was taken");
    put(COMPA, skip, SKIPN);
    CHECK(feed(SA, COMPA, (int32_t)SKIPN, 0) == GAME && phase(SA) == 1, "a skippable frame was taken");
    put(COMPA, A, AN);
    *(uint8_t *)HZ_PTR(SA) = 3;
    CHECK(feed(SA, COMPA, 65536, 0) == GAME && phase(SA) == 3, "a feed in phase 3 was answered here");
    *(uint8_t *)HZ_PTR(SA) = 4;
    CHECK(feed(SA, COMPA, 65536, 0) == GAME && phase(SA) == 4, "a feed in phase 4 was answered here");
    *(uint8_t *)HZ_PTR(SA) = 2;
    CHECK(feed(SA, COMPA, 65536, 0) == GAME && phase(SA) == 2, "a feed of a stream the game began was answered here");
    CHECK(feed(0, COMPA, 65536, 0) == GAME && feed(BASE + ARENA - 8, COMPA, 65536, 0) == GAME, "a state outside the game's memory was taken");
    CHECK(!any_mark(), "a feed left to the game told the write tracking of pages");

    /* a truncated input: the stream waits in phase 2 with content left, as the game's */
    state(SA, OUTA, RAWN + 4096, WSA);
    hz_st64(WSA + HZ_DCTX_EXPECTED, ~0ull);
    clear_marks();
    r = feed(SA, COMPA, (int32_t)(AN - 1000), 0);
    /* (the game's DCtx is never made for a host stream: its "expected" field says so to the game's own feed) */
    CHECK(hz_ld64(WSA + HZ_DCTX_EXPECTED) == 0 && marked(WSA + HZ_DCTX_EXPECTED, 8), "a host stream's begin left the game's DCtx as it found it");
    CHECK(r > 0 && r < RAWN && r == hz_ld32(SA + 0x14) && phase(SA) == 2 && hz_stream_find(SA), "a truncated input gave %#llx in phase %d",
          (unsigned long long)r, phase(SA));
    CHECK(feed(SA, COMPA, 0, 0) == r && feed(SA, COMPA, -1, 0) == r && phase(SA) == 2, "a feed of no bytes did not answer the content left");
    CHECK(feed(SA, COMPA + AN - 1000, 1000, 0) == 0 && phase(SA) == 4, "the rest of the input did not end the frame");
    CHECK(!memcmp(HZ_PTR(OUTA), raw, RAWN), "the frame fed in two parts: other bytes");

    /* (Triforce) a wrong checksum: the content comes out (the job could stop there, at 0), and the checksum's own feed fails
       the stream as the game's feed fails on zstd's checksum error: phase 3 and 0xffffffff */
    put(COMPA, S, SN);
    ((uint8_t *)HZ_PTR(COMPA))[SN - 1] ^= 0x01;
    state(SA, OUTA, SMALLN + 4096, WSA);
    r = feed(SA, COMPA, (int32_t)SN - 4, 0);
    CHECK(r == 0 && phase(SA) == 2 && !memcmp(HZ_PTR(OUTA), small, SMALLN), "a wrong checksum: the content gave %#llx in phase %d",
          (unsigned long long)r, phase(SA));
    r = feed(SA, COMPA + SN - 4, 4, 0);
    CHECK(r == 0xffffffffu && phase(SA) == 3 && !hz_stream_find(SA), "a wrong checksum gave %#llx in phase %d", (unsigned long long)r, phase(SA));

    /* input after the frame's end */
    put(COMPA, S, SN);
    memset(HZ_PTR(COMPA + SN), 0, 8);
    state(SA, OUTA, SMALLN + 4096, WSA);
    r = feed(SA, COMPA, (int32_t)SN + 8, 0);
    CHECK(r == 0xfffffffdu && phase(SA) == 3 && !hz_stream_find(SA), "input after the frame's end gave %#llx in phase %d", (unsigned long long)r, phase(SA));

    /* a damaged frame */
    put(COMPA, S, SN);
    ((uint8_t *)HZ_PTR(COMPA))[SN / 2] ^= 0x55;
    state(SA, OUTA, SMALLN + 4096, WSA);
    r = 0;
    for (size_t off = 0; off < SN && r < 0x80000000u; off += 977)
        r = feed(SA, COMPA + off, (int32_t)(SN - off < 977 ? SN - off : 977), 0);
    CHECK((r == 0xffffffffu || r == 0xfffffffeu) && phase(SA) == 3 && !hz_stream_find(SA), "a damaged frame gave %#llx in phase %d",
          (unsigned long long)r, phase(SA));

    /* the output moved between feeds */
    put(COMPA, S, SN);
    state(SA, OUTA, SMALLN + 4096, WSA);
    r = feed(SA, COMPA, 977, 0);
    hz_st64(SA + 8, hz_ld64(SA + 8) + 16);
    r = feed(SA, COMPA + 977, 977, 0);
    CHECK(r == 0xffffffffu && phase(SA) == 3 && !hz_stream_find(SA), "a feed with the output moved gave %#llx in phase %d", (unsigned long long)r, phase(SA));

    /* a stream with its content out and its checksum unread is spent: the next stream takes its slot when none is free */
    state(SA, OUTA, SMALLN + 4096, WSA);
    r = feed(SA, COMPA, (int32_t)SN - 4, 0);
    CHECK(r == 0 && phase(SA) == 2 && hz_stream_find(SA), "content out before the checksum gave %#llx in phase %d", (unsigned long long)r, phase(SA));
    int took = 0;
    for (int i = 1; i <= HZ_NSTREAM; i++) {
        state(SA + 0x40 * i, OUTA, SMALLN + 4096, WSA);
        took += feed(SA + 0x40 * i, COMPA, 64, 0) != GAME;
    }
    CHECK(took == HZ_NSTREAM && !hz_stream_find(SA), "%d of %d streams begun beside a spent one", took, HZ_NSTREAM);
    state(SA + 0x40 * (HZ_NSTREAM + 1), OUTA, SMALLN + 4096, WSA);
    CHECK(feed(SA + 0x40 * (HZ_NSTREAM + 1), COMPA, 64, 0) == GAME, "a stream was taken with every slot in use");
    /* the game begins a stream again in a state a host stream still had: the old one is dropped, not fed on */
    state(SA + 0x40, OUTA, SMALLN + 4096, WSA);
    whole("a state begun again over an unfinished stream", SA + 0x40, COMPA, SN, OUTA, small, SMALLN, 500, 500, 0);
    for (int i = 1; i <= HZ_NSTREAM; i++) hz_stream_drop(hz_stream_find(SA + 0x40 * i));

    /* check mode */
    Fake2 f2 = {0, 0, 0, ZSTD_createDCtx(), 0, 0};
    uint64_t sc0 = hz_stats.s_checked, sd0 = hz_stats.s_differ;
    put(COMPA, A, AN);
    memset(HZ_PTR(OUTA), 0, RAWN);
    r = checked(&f2, SA, COMPA, AN, OUTA, RAWN + 4096, 50000);
    CHECK(r == 0 && phase(SA) == 4 && !memcmp(HZ_PTR(OUTA), raw, RAWN), "check: the game's stream did not run to its end (%#llx)", (unsigned long long)r);
    CHECK(hz_stats.s_checked == sc0 + 1 && hz_stats.s_differ == sd0 && !hz_stream_find(SA), "check: equal streams counted as %llu compared, %llu different",
          (unsigned long long)(hz_stats.s_checked - sc0), (unsigned long long)(hz_stats.s_differ - sd0));
    f2.spoil = 1;
    r = checked(&f2, SA, COMPA, AN, OUTA, RAWN + 4096, 50000);
    CHECK(r == 0 && hz_stats.s_checked == sc0 + 2 && hz_stats.s_differ == sd0 + 1, "check: a spoiled byte in a stream was not a difference");
    CHECK(mismatches == 3, "check: the stream's difference was not logged (%d lines so far)", mismatches);
    /* the job stops at a result of 0, the checksum unread: compared then, and the slot is spent */
    put(COMPA, S, SN);
    f2.stop_at_content = 1;
    state(SA, OUTA, SMALLN + 4096, WSA);
    f2.s = SA, f2.src = COMPA, f2.size = (int32_t)SN - 4;
    uint64_t got = 77;
    hz_stream(SA, COMPA, (int32_t)SN - 4, 0, 1, fake_feed, &f2, &got);
    CHECK(got == 0 && phase(SA) == 2 && hz_stats.s_checked == sc0 + 3 && hz_stats.s_differ == sd0 + 1 && hz_stream_find(SA) && hz_stream_find(SA)->spent,
          "check: content out before the checksum: %#llx, %llu compared", (unsigned long long)got, (unsigned long long)(hz_stats.s_checked - sc0));
    hz_stream_drop(hz_stream_find(SA));
    /* a damaged frame: the game fails, and so does the host: no difference */
    f2.stop_at_content = 0;
    ((uint8_t *)HZ_PTR(COMPA))[SN / 2] ^= 0x55;
    r = checked(&f2, SA, COMPA, SN, OUTA, SMALLN + 4096, 977);
    CHECK(r >> 31 && phase(SA) == 3 && hz_stats.s_differ == sd0 + 1, "check: a damaged frame: %#llx, %llu different", (unsigned long long)r,
          (unsigned long long)(hz_stats.s_differ - sd0));
    /* a stream the host does not take: the game's result alone, nothing compared */
    put(COMPA, nosize, NOSIZEN);
    sc0 = hz_stats.s_checked;
    r = checked(&f2, SA, COMPA, NOSIZEN, OUTA, RAWN + 4096, NOSIZEN);
    CHECK(r == 0xffffffffu && hz_stats.s_checked == sc0, "check: a stream left to the game was compared (%#llx)", (unsigned long long)r);
    ZSTD_freeDCtx(f2.d);

    CHECK(logs > 0, "nothing was logged");
    if (fails) printf("host zstd: FAILED (%d of %d checks)\n", fails, checks);
    else printf("host zstd: all %d checks passed\n", checks);
    return fails != 0;
}
'''

# The two functions as the lift writes them, before the patches and before the page marks a64_lift.mark_groups makes after
# them (nx2/chunks.py edit_c): the lines the patch looks for in the lifter's spelling, the code around them made up here.
# Written out, not made from ZSTD_SITES: a fixture made from the patch's own lines cannot tell that the text the patch runs
# over spells them otherwise, and the stream feed's call was missing from every build that way. The begin's two stores
# through x19 (+48, +0), and the stores through x19 and x21 nearby, are groups of that pass: plain here, recomp_store*_sp
# with an A64_MARK after them in the lift on disk (check_patch makes that with the pass itself).
LIFTED = (
    '#include "a64.h"\n'
    "void f_8010b380(Ctx *__restrict A64_NOESC c) {\n"
    "    /* nop/hint */\n"
    "    { uint64_t _a=c->x[31], _b=112ULL; uint64_t _r=_a-_b; c->x[31]=_r; }\n"
    "    { uint64_t _b=c->x[31]; int64_t _o=96; recomp_stp64_sp(c,(_b+_o),c->x[29],c->x[30]); }\n"
    "    recomp_store64_sp(c,c->x[31] + 104,c->x[7]);\n"
    "if (c->x[7] != 0) { goto L_8010fd78; }\n"
    "L_8010b3b8:;\n"
    "    recomp_store64(c,c->x[19] + 8,c->x[1]);\n"
    "    recomp_store64(c,c->x[19] + 16,c->x[2]);\n"
    "    { uint64_t _b=c->x[31]; int64_t _o=96; { uint64_t _p0,_p1; recomp_ldp64(c,(_b+_o),&_p0,&_p1); c->x[29]=_p0; c->x[30]=_p1; } }\n"
    "    { uint64_t _a=c->x[31], _b=112ULL; uint64_t _r=_a+_b; c->x[31]=_r; }\n"
    "return;\n"
    "L_8010fd78:;\n"
    "    c->x[8]=recomp_load64(c,c->x[31] + 104);\n"
    "    { uint64_t _b=c->x[8]; int64_t _o=8; { uint64_t _p0,_p1; recomp_ldp64(c,(_b+_o),&_p0,&_p1); c->x[5]=_p0; c->x[6]=_p1; } }\n"
    "{ A64_POLL(c); goto L_8010b3b8; }\n"
    "}\n"
    "void f_80115c80(Ctx *__restrict A64_NOESC c) {\n"
    "    /* nop/hint */\n"
    "    { uint64_t _a=c->x[31], _b=64ULL; uint64_t _r=_a-_b; c->x[31]=_r; }\n"
    "    c->x[8]=recomp_load8(c,c->x[0] + 0);\n"
    "    { uint64_t _r = ((uint64_t)0 | ((c->x[0]) << 0)); c->x[19] = _r; }\n"
    "if ((uint32_t)c->x[8] == 1) { goto L_801160a4; }\n"
    "    c->x[21]=recomp_load64(c,c->x[19] + 48);\n"
    "L_80115d00:;\n"
    "    c->x[22]=recomp_load64(c,c->x[21] + 28776);\n"
    "if (c->x[22] == 0) { goto L_8011627c; }\n"
    "    c->x[23]=recomp_load64(c,c->x[19] + 8);\n"
    "    { uint64_t _r = (uint64_t)(int64_t)(int32_t)recomp_load32(c,c->x[19] + 16); c->x[2] = _r; }\n"
    "    recomp_store64(c,c->x[21] + 28776,c->x[2]);\n"
    "    recomp_store64(c,c->x[21] + 28744,c->x[23]);\n"
    "L_80116080:;\n"
    "    c->x[0]=recomp_load32(c,c->x[19] + 20);\n"
    "    { uint64_t _a=c->x[31], _b=64ULL; uint64_t _r=_a+_b; c->x[31]=_r; }\n"
    "return;\n"
    "L_801160a4:;\n"
    "c->x[30] = 0x801160bcULL; f_80109790(c);\n"
    "    c->x[21]=recomp_load64(c,c->x[19] + 24);\n"
    "    recomp_store32(c,c->x[19] + 20,c->x[8]);\n"
    "if (c->x[21] & 7) { goto L_8011627c; }\n"
    "    c->x[8] = 0x2ULL;\n"
    "    c->x[8] &= 0xFFFFFFFFULL;\n"
    "    recomp_store64(c,c->x[19] + 48,c->x[21]);\n"
    "    recomp_store8(c,c->x[19] + 0,c->x[8]);\n"
    "{ A64_POLL(c); goto L_80115d00; }\n"
    "L_8011627c:;\n"
    "    c->x[8] = 0x3ULL;\n"
    "    recomp_store8(c,c->x[19] + 0,c->x[8]);\n"
    "    c->x[0] = 0xfffffffdULL;\n"
    "c->x[30] = 0x8011629cULL; f_80d5ba00(c);\n"
    "{ A64_POLL(c); goto L_80116080; }\n"
    "}\n"
)
# (another function with all of the stream feed's lines, at another address: no call goes in)
LIFTED += LIFTED[LIFTED.index("void f_80115c80("):].replace("void f_80115c80(", "void f_80116490(", 1)
GROUPED = ("    recomp_store64_sp(c,c->x[19] + 48,c->x[21]);\n    recomp_store8_sp(c,c->x[19] + 0,c->x[8]);\n"
           "    A64_MARK((c->x[19] + (uint64_t)(int64_t)(0)), 56);\n")


def unhooked(text):
    """text without the patch's lines"""
    for _, name, _ in oot_patches.ZSTD_SITES:
        text = text.replace(oot_patches.ZSTD_HOOK % name, "")
    return text.replace(oot_patches.ZSTD_DECL, "")


def check_patch():
    patch, marks = oot_patches.patch_host_zstd, a64_lift.mark_groups
    marked = marks(LIFTED)
    # (the fixture has what kept the stream feed's call out: its begin's stores spelled one way before the marks, another
    # after them)
    assert "    recomp_store64(c,c->x[19] + 48,c->x[21]);\n    recomp_store8(c,c->x[19] + 0,c->x[8]);\n" in LIFTED
    assert GROUPED not in LIFTED and marked.count(GROUPED) == 2, "the page marks did not group the begin's stores"
    for what, text in (("before the marks", LIFTED), ("after the marks", marked)):
        out, n = patch(text)
        assert n == 2, (what, n)
        for head, name, _ in oot_patches.ZSTD_SITES:
            hook = oot_patches.ZSTD_HOOK % name
            assert out.count(hook) == 1 and out.count(head + hook) == 1, (what, name)   # (the first line of its function, once)
            assert "if (hos_host_zstd && %s(c)) return;" % name in hook   # (a switch the lifted code reads: no call while off)
        assert out.count(oot_patches.ZSTD_DECL) == 1 and out.index(oot_patches.ZSTD_DECL) < out.index("void f_8010b380("), what
        assert unhooked(out) == text, what                    # (nothing else changed)
        assert "hos_zstd" not in out[out.index("void f_80116490("):], what   # (the same lines at another address)
        assert patch(out) == (out, 2), what                   # (a second run)
    # (the order of the two: the marks made over the patched lift are what the patch makes of the marked one)
    assert marks(patch(LIFTED)[0]) == patch(marked)[0]
    # (a function at that address whose code is not the decoder's, as in another game version or a changed lift: left as it
    # is, whichever of its lines differs or comes twice, before the marks and after them. The lines are written as the lift
    # is before the marks: that is the text the patch sees)
    for head, name, shape in oot_patches.ZSTD_SITES:
        hook = oot_patches.ZSTD_HOOK % name
        start = LIFTED.index(head)
        end = LIFTED.index("\nvoid f_", start)
        for line in shape:
            assert LIFTED.count(line, start, end) == 1, (name, "not in the lifter's own spelling", line)
            changed = LIFTED[:start] + LIFTED[start:end].replace(line, line.replace("c->x[", "c->x[1", 1)) + LIFTED[end:]
            twice = LIFTED[:start] + LIFTED[start:end].replace(line, line + line) + LIFTED[end:]
            for text in (changed, marks(changed), twice, marks(twice)):
                out, n = patch(text)
                assert n == 1 and hook not in out and unhooked(out) == text, (name, line)
    # (the call already there, but not at the head: not made again)
    out = patch(LIFTED)[0]
    head, hook = oot_patches.ZSTD_SITES[1][0], oot_patches.ZSTD_HOOK % "hos_zstd_stream"
    moved = out.replace(head + hook, head, 1).replace("L_80115d00:;\n", "L_80115d00:;\n" + hook, 1)
    assert patch(moved) == (moved, 1) and moved.count(hook) == 1
    assert patch('#include "a64.h"\nvoid f_80000000(Ctx *__restrict A64_NOESC c) {\n}\n')[1] == 0
    # (only the supported build's lift gets them: found by address)
    source = (ROOT / "nx2/oot_patches.py").read_text(encoding="utf-8")
    assert "        if protect_hearts:   # (found by address: the supported build's functions)\n            zstd, found = patch_host_zstd(out)" in source


def zstd_sources():
    """HOST_ZSTD_SOURCES of nx2/build.py: the files the runtime's decoder is compiled from"""
    tree = ast.parse((ROOT / "nx2/build.py").read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "HOST_ZSTD_SOURCES" for t in node.targets):
            return ast.literal_eval(node.value)
    raise SystemExit("nx2/build.py has no HOST_ZSTD_SOURCES")


def check_source():
    glue = (ROOT / "host/hos_zstd.c").read_text(encoding="utf-8")
    build = (ROOT / "nx2/build.py").read_text(encoding="utf-8")
    docs = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    for text, needle, why in (
            (glue, 'getenv("HOS_HOST_ZSTD")', "host/hos_zstd.c does not read HOS_HOST_ZSTD"),
            (glue, "v = e && *e ? atoi(e) : 0;\n        v = v == 1 || v == 2 ? v : 0;", "the decoder is not off unless HOS_HOST_ZSTD is 1 or 2"),
            (glue, 'strncmp(hos_main_build_id, "e0f3ccfa", 8)', "the game build the addresses are for is not checked"),
            (glue, "const int m = in_game ? 0 : mode();\n    if (!m) return 0;", "a call does not answer 0 with the switch off, or inside the game's own function"),
            (glue, "#define GAME_MULTIFRAME 0x8010b380ULL", "the one-call function's address changed"),
            (glue, "#define GAME_STREAM 0x80115c80ULL", "the stream feed's address changed"),
            (glue, "#define HZ_WRITTEN(a, n) HOS_WRITTEN(GP(a), (size_t)(n))", "the decoder's writes are not told to the write tracking"),
            (glue, "#define HZ_OK(a, n) (hos_guest_range((a), (n)) != NULL)", "guest ranges are not checked against the game's memory"),
            (build, 'for f in HOST_ZSTD_SOURCES:\n            jobs.append((cc, os.path.join(hzstd, *f.split("/")), os.path.join(obj_dir, "hostzstd_"',
             "nx2/build.py does not compile zstd's decoder for the runtime"),
            (docs, "`HOS_HOST_ZSTD=1` or `2`", "docs/development.md does not name HOS_HOST_ZSTD")):
        if needle not in text:
            raise SystemExit(why)
    for f in zstd_sources():
        if not (ROOT / "third_party/zstd/lib" / f).is_file():
            raise SystemExit("third_party/zstd/lib/%s is not there" % f)


def frames(tmp):
    """The frames and what they decode to, as files in tmp"""
    rng = random.Random(20261115)
    raw = bytearray()
    while len(raw) < 3 << 20:   # (random runs and repeats of earlier bytes from up to 2 MiB back: matches that reach far)
        if len(raw) >= 4096 and rng.randrange(4):
            at = len(raw) - 64 - rng.randrange(min(len(raw), 2 << 20) - 63)
            raw += raw[at:at + 64]
        else:
            raw += rng.randbytes(64)
    raw = bytes(raw[:3 << 20])
    small = raw[:100000]
    plain = zstandard.ZstdCompressor(level=3, write_content_size=True, write_checksum=False)
    summed = zstandard.ZstdCompressor(level=3, write_content_size=True, write_checksum=True)
    # (raw bytes as a dictionary: no header, no id. Bytes the data repeats, so that the frame cannot be decoded without them)
    dict_bytes = raw[30000:30000 + 65536]
    # (another dictionary of the same size for the same address, made as the fixture makes it, and data that repeats it)
    dict2 = bytes(b ^ 0x5a for b in reversed(dict_bytes))
    small2 = dict2[1000:51000] + small[:50000]
    samples = [("item %04d: colour=%s weight=%d shelf=%s note=%s\n" % (
        i, ("red", "green", "blue", "amber")[i % 4], i * 37 % 1000, "ABCDEFGH"[i % 8] * 3, "x" * (i % 17))).encode() * 4
        for i in range(4000)]
    trained = zstandard.train_dictionary(16384, samples)
    traw = b"".join(samples[1000:1200])
    files = {
        "raw.bin": raw,
        "small.bin": small,
        "a.zst": plain.compress(raw),
        "small.zst": summed.compress(small),
        "nosize.zst": zstandard.ZstdCompressor(level=3, write_content_size=False).compress(small),
        "two.zst": plain.compress(raw[:1 << 20]) + plain.compress(raw[1 << 20:2 << 20]),
        "skip.zst": struct.pack("<II", 0x184D2A53, 11) + b"hello there" + plain.compress(small),
        "dict.bin": dict_bytes,
        "dicted.zst": zstandard.ZstdCompressor(level=3, dict_data=zstandard.ZstdCompressionDict(
            dict_bytes, dict_type=zstandard.DICT_TYPE_RAWCONTENT)).compress(small),
        "dicted2.zst": zstandard.ZstdCompressor(level=3, dict_data=zstandard.ZstdCompressionDict(
            dict2, dict_type=zstandard.DICT_TYPE_RAWCONTENT)).compress(small2),
        "small2.bin": small2,
        "tdict.bin": trained.as_bytes(),
        "tdicted.zst": zstandard.ZstdCompressor(level=3, dict_data=trained, write_dict_id=True).compress(traw),
        "traw.bin": traw,
    }
    assert zstandard.get_frame_parameters(files["tdicted.zst"]).dict_id == trained.dict_id() != 0
    assert zstandard.get_frame_parameters(files["a.zst"]).content_size == len(raw)
    assert zstandard.get_frame_parameters(files["small.zst"]).has_checksum
    for name, data in files.items():
        (tmp / name).write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    check_patch()
    check_source()
    zstd = ROOT / "third_party/zstd/lib"
    with tempfile.TemporaryDirectory(prefix="host-zstd-") as d:
        tmp = Path(d)
        frames(tmp)
        (tmp / "t.c").write_text(FIXTURE, encoding="utf-8")
        objects = []
        for f in zstd_sources():   # (as nx2/build.py compiles them for the runtime, but for the optimisation level)
            obj = tmp / (Path(f).stem + ".o")
            subprocess.run([args.cc, "-c", "-O1", "-g0", "-DNDEBUG", "-w", "-DZSTD_DISABLE_ASM", "-DZSTD_LEGACY_SUPPORT=0",
                            "-DZSTD_MULTITHREAD=0", str(zstd / f), "-o", str(obj)], check=True)
            objects.append(str(obj))
        exe = tmp / ("t.exe" if sys.platform == "win32" else "t")
        subprocess.run([args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-I", str(ROOT / "host"), str(tmp / "t.c")] +
                       objects + ["-o", str(exe)], check=True)
        run = subprocess.run([str(exe), str(tmp)] + (["-v"] if args.verbose else []), text=True, capture_output=True, timeout=300)
        sys.stdout.write(run.stdout)
        if run.returncode or "checks passed" not in run.stdout:
            raise SystemExit("the host decoder's checks failed" + run.stderr)


if __name__ == "__main__":
    main()
