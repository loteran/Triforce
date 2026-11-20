/* (Triforce) tests/test_menu_sound.py: host/menu_sound.c as it is, with stand-ins for the host around it (the log, the
   clock, the options' side of rx_option, the game's files) and, in render_tail.inc, the end of host/nn_audio.c's
   render_frame as it is. No sound device, no game file: every BWAV here is written by bwav() below from samples made
   up here, its DSP-ADPCM by encode_adpcm() or from frames written out by hand.

   One scenario a run (argv[1]): menu_sound.c's state is the process's. */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "../gpu/vk/vk_api.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static int nearly(float a, float b) { return fabsf(a - b) <= 1e-6f; }

/* ---- the host, as menu_sound.c sees it ---- */
static char logged[4096];
static void hos_log(const char *fmt, ...) {
    const size_t at = strlen(logged);
    va_list args;
    va_start(args, fmt);
    vsnprintf(logged + at, sizeof logged - at, fmt, args);
    va_end(args);
    strncat(logged, "\n", sizeof logged - strlen(logged) - 1);
}
static int64_t now_ns = 5000000000ll;
static int64_t hos_now_ns(void) { return now_ns; }
/* (the renderer's answers: the Menu sounds row, -1 while there is no renderer, and the sounds the options asked for) */
static int row_volume = -1, asked[64], nasked, asked_at, volume_reads;
static int rx_option(int id, int otherwise) {
    if (id == HOS_OPT_MENU_SOUND_VOLUME) return volume_reads++, row_volume < 0 ? otherwise : row_volume;
    if (id == HOS_OPT_MENU_SOUND) return row_volume < 0 ? otherwise : asked_at < nasked ? asked[asked_at++] : 0;
    return otherwise;
}
static void ask(int id) { asked[nasked++] = id; }
/* (the game's files: what the test put there) */
static struct { const char *rel; const uint8_t *data; uint64_t size; } rom[16];
static int nrom, rom_reads;
static uint8_t *hos_fs_rom_file(const char *rel, uint64_t most, uint64_t *size) {
    rom_reads++;
    for (int i = 0; i < nrom; i++)
        if (!strcmp(rom[i].rel, rel) && rom[i].size && rom[i].size <= most) {
            uint8_t *copy = (uint8_t *)malloc((size_t)rom[i].size);
            memcpy(copy, rom[i].data, (size_t)rom[i].size);
            *size = rom[i].size;
            return copy;
        }
    return NULL;
}
static int load_requests;
static void load_start(void) { load_requests++; }   /* (the test reads the files when it says: load_all) */
static void (*between)(int at);
#define MENU_SOUND_BETWEEN(at) do { if (between) between(at); } while (0)

#define MENU_SOUND_TEST
#include "../host/menu_sound.c"

/* ---- memory that ends where nothing can be read: a read past a file's or a sound's end stops the run ---- */
static size_t page_size(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    return (size_t)sysconf(_SC_PAGESIZE);
#endif
}
static uint8_t *guard_end(size_t span) {   /* the end of `span` bytes (whole pages) with a page after them that cannot be read */
    const size_t page = page_size();
#ifdef _WIN32
    uint8_t *p = (uint8_t *)VirtualAlloc(NULL, span + page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    if (!p || !VirtualProtect(p + span, page, PAGE_NOACCESS, &old)) exit(3);
#else
    uint8_t *p = (uint8_t *)mmap(NULL, span + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED || mprotect(p + span, page, PROT_NONE)) exit(3);
#endif
    return p + span;
}
static void *guarded(size_t n) {
    const size_t page = page_size();
    return guard_end((n + page - 1) / page * page) - n;
}
#define ARENA (1u << 16)
static const uint8_t *at_guard(const uint8_t *bytes, size_t n) {   /* a copy of bytes[0..n) that ends at the guard (one arena) */
    static uint8_t *end;
    if (!end) end = guard_end(ARENA);
    if (n > ARENA) exit(3);
    memcpy(end - n, bytes, n);
    return end - n;
}

/* ---- a BWAV writer ---- */
typedef struct {
    int codec, pan;
    uint32_t rate, count;
    const int16_t *pcm;       /* PCM16: the samples; DSP-ADPCM: what is encoded */
    int16_t coef[16];
    const uint8_t *frames;    /* DSP-ADPCM: frames written out by hand, in place of an encoding */
} Chan;
static void p16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v) { p16(p, v & 0xffff), p16(p + 2, v >> 16); }
static int64_t div_round(int64_t a, int64_t b) { return (a >= 0 ? a + b / 2 : a - b / 2) / b; }
/* DSP-ADPCM, by the format: a frame of 14 samples has one coefficient pair and one scale; a sample is the pair times
   the two samples before it (in 2048ths), plus its four bits times the scale. Every pair and scale is tried for each
   frame and the one with the least error kept. Returns the error summed: 0 for samples the format can hold exactly */
static int64_t encode_adpcm(const int16_t *pcm, uint32_t count, const int16_t *coef, uint8_t *out) {
    int h1 = 0, h2 = 0;
    int64_t total = 0;
    for (uint32_t f = 0; f * 14 < count; f++) {
        const uint32_t n = count - f * 14 < 14 ? count - f * 14 : 14;
        int64_t best = -1;
        uint8_t best_frame[8] = {0};
        int best_h1 = 0, best_h2 = 0;
        for (int idx = 0; idx < 8; idx++)
            for (int sh = 0; sh <= 12; sh++) {
                uint8_t frame[8] = {(uint8_t)(idx << 4 | sh)};
                int a1 = h1, a2 = h2;
                int64_t err = 0;
                for (uint32_t k = 0; k < n; k++) {
                    const int64_t pred = (int64_t)coef[2 * idx] * a1 + (int64_t)coef[2 * idx + 1] * a2;
                    int64_t nib = div_round((int64_t)pcm[f * 14 + k] * 2048 - pred, (int64_t)2048 << sh);
                    nib = nib < -8 ? -8 : nib > 7 ? 7 : nib;
                    int64_t v = (nib * ((int64_t)2048 << sh) + pred + 1024) / 2048;
                    if ((nib * ((int64_t)2048 << sh) + pred + 1024) % 2048 < 0) v--;   /* (rounded down, as a shift does) */
                    v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
                    err += llabs(v - pcm[f * 14 + k]);
                    frame[1 + k / 2] |= (uint8_t)((nib & 15) << (k & 1 ? 0 : 4));
                    a2 = a1, a1 = (int)v;
                }
                if (best < 0 || err < best) best = err, memcpy(best_frame, frame, 8), best_h1 = a1, best_h2 = a2;
            }
        memcpy(out + f * 8, best_frame, 8);
        h1 = best_h1, h2 = best_h2;
        total += best;
    }
    return total;
}
static int64_t encode_error;   /* (of the last bwav()) */
static uint8_t *bwav(const Chan *ch, int nch, uint64_t *size) {
    uint64_t offs[2] = {0, 0}, lens[2] = {0, 0}, at = (0x10 + 0x4c * (uint64_t)nch + 0x3f) & ~(uint64_t)0x3f;
    for (int c = 0; c < nch; c++) {
        lens[c] = ch[c].codec == 1 ? ((uint64_t)ch[c].count + 13) / 14 * 8 : (uint64_t)ch[c].count * 2;
        offs[c] = at;
        at = c + 1 < nch ? (at + lens[c] + 0x3f) & ~(uint64_t)0x3f : at + lens[c];   /* (the file ends with its last sample) */
    }
    uint8_t *b = (uint8_t *)calloc(1, (size_t)at + 1);
    memcpy(b, "BWAV", 4);
    p16(b + 4, 0xfeff), p16(b + 6, 1), p16(b + 0x0e, (unsigned)nch);
    encode_error = 0;
    for (int c = 0; c < nch; c++) {
        uint8_t *info = b + 0x10 + c * 0x4c;
        p16(info, (unsigned)ch[c].codec), p16(info + 2, (unsigned)ch[c].pan);
        p32(info + 4, ch[c].rate), p32(info + 8, ch[c].count), p32(info + 12, ch[c].count);
        for (int i = 0; i < 16; i++) p16(info + 0x10 + 2 * i, (uint16_t)ch[c].coef[i]);
        p32(info + 0x30, (uint32_t)offs[c]), p32(info + 0x34, (uint32_t)offs[c]), p32(info + 0x38, 1), p32(info + 0x3c, 0xffffffffu);
        if (ch[c].codec == 1 && ch[c].frames) memcpy(b + offs[c], ch[c].frames, (size_t)lens[c]);
        else if (ch[c].codec == 1) encode_error += encode_adpcm(ch[c].pcm, ch[c].count, ch[c].coef, b + offs[c]);
        else
            for (uint32_t i = 0; i < ch[c].count; i++) p16(b + offs[c] + 2 * (uint64_t)i, (uint16_t)ch[c].pcm[i]);
    }
    *size = at;
    return b;
}
/* (the pairs the made-up samples need: none; the sample before; a straight line through the two before; twice the one before) */
static const int16_t kCoef[16] = {0, 0, 2048, 0, 4096, -2048, 4096, 0, 32767, 32767};
/* Samples DSP-ADPCM holds exactly with kCoef, a frame of each kind in turn: steps of 256 (no prediction); a walk in steps
   of 64 from the sample before; a line through the two before, bent twice by 32s */
static void exact_signal(int16_t *pcm, uint32_t n, int seed) {
    for (uint32_t i = 0; i < n; i++) {
        const int frame = (int)(i / 14), k = (int)(i % 14);
        int v;
        if (frame % 3 == 0) v = 256 * ((k * 5 + frame + seed) % 16 - 8);
        else if (frame % 3 == 1) v = pcm[i - 1] + 64 * ((k * 3 + seed) % 15 - 7);
        else v = 2 * pcm[i - 1] - pcm[i - 2] + 32 * (k == 0 ? -3 - seed % 3 : k == 5 ? 2 : 0);
        if (v < -32768 || v > 32767) exit(3);
        pcm[i] = (int16_t)v;
    }
}
static Chan chan(int codec, int pan, uint32_t rate, uint32_t count, const int16_t *pcm) {
    Chan c = {codec, pan, rate, count, pcm, {0}, NULL};
    memcpy(c.coef, kCoef, sizeof c.coef);
    return c;
}
static float f16(int16_t v) { return (float)v * (1.0f / 32768.0f); }

/* ---- the decoder ---- */
static void decode_pcm16(void) {
    int16_t a[100], b2[100];
    for (int i = 0; i < 100; i++) a[i] = (int16_t)(i * 300 - 15000), b2[i] = (int16_t)(32767 - i * 650);
    a[0] = -32768, a[1] = 32767;
    for (int pan = 0; pan <= 2; pan++) {   /* one channel: the left alone, the right alone, the middle in both, 3 dB down */
        const Chan c = chan(0, pan, 48000, 100, a);
        uint64_t size;
        uint8_t *file = bwav(&c, 1, &size);
        uint32_t frames = 0;
        float *pcm = hos_menu_sound_decode(at_guard(file, (size_t)size), size, &frames);
        CHECK(pcm && frames == 100);
        for (int i = 0; pcm && i < 100; i++) {
            const float l = pan == 1 ? 0.0f : pan == 0 ? f16(a[i]) : f16(a[i]) * 0.7071f;
            const float r = pan == 0 ? 0.0f : pan == 1 ? f16(a[i]) : f16(a[i]) * 0.7071f;
            if (pcm[2 * i] != l || pcm[2 * i + 1] != r) { CHECK(!"a mono PCM16 sample by its pan"); break; }
        }
        free(pcm), free(file);
    }
    {   /* two channels, left and right: each its own, as they are */
        const Chan c[2] = {chan(0, 0, 48000, 100, a), chan(0, 1, 48000, 100, b2)};
        uint64_t size;
        uint8_t *file = bwav(c, 2, &size);
        uint32_t frames = 0;
        float *pcm = hos_menu_sound_decode(at_guard(file, (size_t)size), size, &frames);
        CHECK(pcm && frames == 100);
        for (int i = 0; pcm && i < 100; i++)
            if (pcm[2 * i] != f16(a[i]) || pcm[2 * i + 1] != f16(b2[i])) { CHECK(!"a stereo PCM16 sample"); break; }
        free(pcm), free(file);
    }
    {   /* a second channel shorter than the first: silence after its end, nothing read past it */
        Chan c[2] = {chan(0, 0, 48000, 100, a), chan(0, 1, 48000, 40, b2)};
        uint64_t size;
        uint8_t *file = bwav(c, 2, &size);
        uint32_t frames = 0;
        float *pcm = hos_menu_sound_decode(at_guard(file, (size_t)size), size, &frames);
        CHECK(pcm && frames == 100 && pcm[2 * 39 + 1] == f16(b2[39]) && pcm[2 * 40 + 1] == 0.0f && pcm[2 * 99] == f16(a[99]));
        free(pcm), free(file);
    }
}
static void decode_adpcm(void) {
    enum { N = 14 * 9 + 3 };   /* (the last frame is a part of one) */
    int16_t a[N], b2[N];
    exact_signal(a, N, 0), exact_signal(b2, N, 4);
    for (int pan = 0; pan <= 2; pan++) {
        const Chan c = chan(1, pan, 48000, N, a);
        uint64_t size;
        uint8_t *file = bwav(&c, 1, &size);
        CHECK(encode_error == 0);   /* (the samples are held exactly: what comes back is what went in) */
        CHECK(size == 0x80 + (N + 13) / 14 * 8);
        uint32_t frames = 0;
        float *pcm = hos_menu_sound_decode(at_guard(file, (size_t)size), size, &frames);
        CHECK(pcm && frames == N);
        for (int i = 0; pcm && i < N; i++) {
            const float l = pan == 1 ? 0.0f : pan == 0 ? f16(a[i]) : f16(a[i]) * 0.7071f;
            const float r = pan == 0 ? 0.0f : pan == 1 ? f16(a[i]) : f16(a[i]) * 0.7071f;
            if (pcm[2 * i] != l || pcm[2 * i + 1] != r) { printf("sample %d pan %d\n", i, pan); CHECK(!"a mono DSP-ADPCM sample by its pan"); break; }
        }
        free(pcm), free(file);
    }
    {
        const Chan c[2] = {chan(1, 0, 48000, N, a), chan(1, 1, 48000, N, b2)};
        uint64_t size;
        uint8_t *file = bwav(c, 2, &size);
        CHECK(encode_error == 0);
        uint32_t frames = 0;
        float *pcm = hos_menu_sound_decode(at_guard(file, (size_t)size), size, &frames);
        CHECK(pcm && frames == N);
        for (int i = 0; pcm && i < N; i++)
            if (pcm[2 * i] != f16(a[i]) || pcm[2 * i + 1] != f16(b2[i])) { printf("sample %d\n", i); CHECK(!"a stereo DSP-ADPCM sample"); break; }
        free(pcm), free(file);
    }
    {   /* frames by hand. Pair 3 doubles the sample before: 7 << 12, then past the top and held there; -8 << 12, then past
           the bottom. Pair 4 is as large as coefficients get: the sum is past 32 bits and still the top, not wrapped */
        const uint8_t hand[24] = {0x3c, 0x70, 0x00, 0x00, 0, 0, 0, 0,     /* pair 3, scale 12: 28672, 32767 (57344), 32767 ... */
                                  0x0c, 0x80, 0, 0, 0, 0, 0, 0,           /* pair 0: -32768, then 0s */
                                  0x4c, 0x77, 0x77, 0, 0, 0, 0, 0};       /* pair 4, four 7s: 28672, then far past the top */
        Chan c = chan(1, 0, 48000, 42, NULL);
        c.frames = hand;
        uint64_t size;
        uint8_t *file = bwav(&c, 1, &size);
        uint32_t frames = 0;
        float *pcm = hos_menu_sound_decode(at_guard(file, (size_t)size), size, &frames);
        CHECK(pcm && frames == 42);
        if (pcm) {
            CHECK(pcm[0] == f16(28672) && pcm[2] == f16(32767) && pcm[4] == f16(32767) && pcm[26] == f16(32767));
            CHECK(pcm[28] == f16(-32768) && pcm[30] == 0.0f && pcm[54] == 0.0f);
            /* frame 2: 28672; then 32767 times the samples before, in 2048ths: the fourth's sum is 2,206,072,834, more than
               32 bits hold (wrapped, it would be the bottom) */
            CHECK(pcm[56] == f16(28672) && pcm[58] == f16(32767) && pcm[60] == f16(32767) && pcm[62] == f16(32767));
            for (int i = 0; i < 42; i++) CHECK(pcm[2 * i + 1] == 0.0f);
        }
        free(pcm), free(file);
    }
}
static void decode_rates(void) {
    int16_t a[50], b2[50];
    for (int i = 0; i < 50; i++) a[i] = (int16_t)(i * 512), b2[i] = (int16_t)(-i * 256);
    {   /* 24 kHz: twice the samples, each new one halfway between its neighbours, the last held */
        const Chan c[2] = {chan(0, 0, 24000, 50, a), chan(0, 1, 24000, 50, b2)};
        uint64_t size;
        uint8_t *file = bwav(c, 2, &size);
        uint32_t frames = 0;
        float *pcm = hos_menu_sound_decode(file, size, &frames);
        CHECK(pcm && frames == 100);
        for (int i = 0; pcm && i < 50; i++) {
            const int j = i + 1 < 50 ? i + 1 : i;
            CHECK(nearly(pcm[4 * i], f16(a[i])) && nearly(pcm[4 * i + 1], f16(b2[i])));
            CHECK(nearly(pcm[4 * i + 2], (f16(a[i]) + f16(a[j])) / 2) && nearly(pcm[4 * i + 3], (f16(b2[i]) + f16(b2[j])) / 2));
        }
        free(pcm), free(file);
    }
    {   /* 96 kHz: every other sample */
        const Chan c = chan(0, 0, 96000, 50, a);
        uint64_t size;
        uint8_t *file = bwav(&c, 1, &size);
        uint32_t frames = 0;
        float *pcm = hos_menu_sound_decode(file, size, &frames);
        CHECK(pcm && frames == 25);
        for (int i = 0; pcm && i < 25; i++) CHECK(nearly(pcm[2 * i], f16(a[2 * i])) && pcm[2 * i + 1] == 0.0f);
        free(pcm), free(file);
    }
    {   /* 44.1 kHz: as long as the sound, to the sample; the first sample the first */
        const Chan c = chan(0, 0, 44100, 50, a);
        uint64_t size;
        uint8_t *file = bwav(&c, 1, &size);
        uint32_t frames = 0;
        float *pcm = hos_menu_sound_decode(file, size, &frames);
        CHECK(pcm && frames == (50 * 48000 + 44099) / 44100 && pcm[0] == f16(a[0]));
        for (uint32_t i = 1; pcm && i < frames; i++) CHECK(pcm[2 * i] >= pcm[2 * i - 2] && pcm[2 * i] <= f16(a[49]));   /* (a rising line stays one) */
        free(pcm), free(file);
    }
}
/* file with one change: a field of its header or of a channel's */
static int refused_with(const uint8_t *file, uint64_t size, uint64_t at, int bytes, uint32_t value) {
    uint8_t *copy = (uint8_t *)malloc((size_t)size);
    memcpy(copy, file, (size_t)size);
    if (bytes == 2) p16(copy + at, value);
    else p32(copy + at, value);
    uint32_t frames = 0;
    float *pcm = hos_menu_sound_decode(at_guard(copy, (size_t)size), size, &frames);
    const int refused = pcm == NULL;
    free(pcm), free(copy);
    return refused;
}
static void decode_refusals(void) {
    enum { N = 14 * 6 };
    int16_t a[N], b2[N];
    exact_signal(a, N, 1), exact_signal(b2, N, 2);
    for (int codec = 0; codec <= 1; codec++) {
        const Chan c[2] = {chan(codec, 0, 48000, N, a), chan(codec, 1, 48000, N, b2)};
        uint64_t size;
        uint8_t *file = bwav(c, 2, &size);
        const uint32_t off0 = u32(file + 0x10 + 0x30), off1 = u32(file + 0x10 + 0x4c + 0x30);
        const uint32_t len = codec ? N / 14 * 8 : N * 2;
        CHECK(off1 + len == size);
        CHECK(!refused_with(file, size, 0x0c, 2, 0));          /* (the file as it is plays) */
        CHECK(refused_with(file, size, 0, 4, 0x42574156));      /* "BWAV" backwards */
        CHECK(refused_with(file, size, 0x0e, 2, 0));            /* no channel */
        CHECK(refused_with(file, size, 0x0e, 2, 3));            /* three */
        CHECK(refused_with(file, size, 0x0e, 2, 0xffff));
        CHECK(refused_with(file, size, 0x10, 2, 2));            /* a codec that is neither */
        CHECK(refused_with(file, size, 0x10 + 0x4c, 2, 7));     /* ...of the second channel */
        CHECK(refused_with(file, size, 0x14, 4, 0));            /* no rate */
        CHECK(refused_with(file, size, 0x14, 4, 7999));
        CHECK(refused_with(file, size, 0x14, 4, 192001));
        CHECK(refused_with(file, size, 0x18, 4, 0));            /* no samples */
        CHECK(refused_with(file, size, 0x18, 4, 480001));       /* more than ten seconds */
        CHECK(refused_with(file, size, 0x18, 4, 0xffffffffu));
        {   /* one sample more than the file has, in both channels (the second's are the file's last: two bytes, or a frame) */
            uint8_t *more = (uint8_t *)malloc((size_t)size);
            memcpy(more, file, (size_t)size);
            p32(more + 0x18, N + 1);
            CHECK(!refused_with(more, size, 0x0c, 2, 0));   /* (the first channel's go on into what follows them; the second has N) */
            CHECK(refused_with(more, size, 0x10 + 0x4c + 8, 4, N + 1));
            free(more);
        }
        CHECK(refused_with(file, size, 0x10 + 0x30, 4, (uint32_t)size));            /* the samples start at the file's end */
        CHECK(refused_with(file, size, 0x10 + 0x30, 4, (uint32_t)size - len + 1));  /* ...one byte too late */
        CHECK(!refused_with(file, size, 0x10 + 0x30, 4, (uint32_t)size - len));     /* ...as late as they fit */
        CHECK(refused_with(file, size, 0x10 + 0x30, 4, 0xffffffffu));               /* past 4 GB with their length */
        CHECK(refused_with(file, size, 0x10 + 0x30, 4, 0xfffffff0u));
        CHECK(refused_with(file, size, 0x10 + 0x4c + 0x30, 4, off1 + 1));           /* the second channel's, one byte too late */
        CHECK(refused_with(file, size, 0x10 + 0x4c + 0x30, 4, 0x80000000u));
        CHECK(!refused_with(file, size, 0x10 + 0x4c + 0x30, 4, off0));              /* (inside the file: played, whatever is there) */
        /* cut short anywhere: refused, and nothing past the cut is read (the copy ends at memory that cannot be read) */
        for (uint64_t cut = 0; cut < size; cut++) {
            uint32_t frames = 0;
            float *pcm = hos_menu_sound_decode(at_guard(file, (size_t)cut), cut, &frames);
            if (pcm) { printf("cut at %u of %u\n", (unsigned)cut, (unsigned)size); CHECK(!"a file cut short is refused"); free(pcm); break; }
        }
        /* bytes changed at random, the header's above all, and cut at random: whatever comes back is whole and finite */
        uint32_t seed = 12345 + (uint32_t)codec;
        int played = 0;
        for (int round = 0; round < 40000; round++) {
            uint8_t *copy = (uint8_t *)malloc((size_t)size);
            memcpy(copy, file, (size_t)size);
            const int changes = 1 + (int)((seed = seed * 1664525u + 1013904223u) >> 30);
            for (int k = 0; k < changes; k++) {
                seed = seed * 1664525u + 1013904223u;
                const uint64_t where = (seed >> 8 & 3) ? (seed >> 12) % 0xa8 : (seed >> 12) % size;
                seed = seed * 1664525u + 1013904223u;
                copy[where] = (seed >> 20 & 7) == 0 ? 0xff : (seed >> 20 & 7) == 1 ? 0 : (uint8_t)(seed >> 9);
            }
            seed = seed * 1664525u + 1013904223u;
            const uint64_t cut = (seed >> 28) ? size : (seed >> 4) % (size + 1);
            uint32_t frames = 0;
            float *pcm = hos_menu_sound_decode(at_guard(copy, (size_t)cut), cut, &frames);
            if (pcm) {
                played++;
                CHECK(frames > 0 && frames <= 480000);
                for (uint32_t i = 0; i < frames * 2; i++)
                    if (!(pcm[i] >= -2.0f && pcm[i] <= 2.0f)) { CHECK(!"a decoded sample out of range"); break; }
            }
            free(pcm), free(copy);
        }
        CHECK(played > 100);   /* (many of them still play: the bytes changed were samples, or fields that do not matter) */
        free(file);
    }
    uint32_t frames = 0;
    CHECK(!hos_menu_sound_decode(at_guard((const uint8_t *)"BWAV", 4), 4, &frames));
    CHECK(!hos_menu_sound_decode(at_guard((const uint8_t *)"", 0), 0, &frames));
}

/* ---- the voices and the mix, on sounds put there as they would be after the files are read ---- */
enum { OPEN = HOS_MENU_SOUND_OPEN, CLOSE = HOS_MENU_SOUND_CLOSE, CURSOR = HOS_MENU_SOUND_CURSOR, DECIDE = HOS_MENU_SOUND_DECIDE,
       CANCEL = HOS_MENU_SOUND_CANCEL, TAB_LEFT = HOS_MENU_SOUND_TAB_LEFT, TAB_RIGHT = HOS_MENU_SOUND_TAB_RIGHT, FANFARE = HOS_MENU_SOUND_FANFARE,
       SYS_CURSOR = HOS_MENU_SOUND_SYS_CURSOR, SYS_DECIDE = HOS_MENU_SOUND_SYS_DECIDE };
#define BLOCK 240
static float bl[BLOCK], br[BLOCK];
static void install(int id, uint32_t frames, float l, float r) {   /* (its samples end where nothing can be read) */
    float *pcm = (float *)guarded((size_t)frames * 2 * sizeof *pcm);
    for (uint32_t i = 0; i < frames; i++) pcm[2 * i] = l, pcm[2 * i + 1] = r;
    sounds[id] = (Sound){pcm, frames};
}
static void quiet(void) {   /* every voice free, as after the files were read */
    for (int i = 0; i < VOICES; i++) atomic_store(&voices[i].at, -1);
    atomic_store(&loaded, 2);
    between = NULL;
}
static int voice_pos(int i) {   /* the frame a voice is at, -1 free */
    const int64_t at = atomic_load(&voices[i].at);
    return at < 0 ? -1 : (int)AT_FRAME(at);
}
static int active(void) {
    int n = 0;
    for (int i = 0; i < VOICES; i++) n += voice_pos(i) >= 0;
    return n;
}
static int voice_with(int id) {
    for (int i = 0; i < VOICES; i++)
        if (voice_pos(i) >= 0 && atomic_load(&voices[i].sound) == id) return i;
    return -1;
}
static void block(float gain, float l, float r) {   /* one block mixed over l and r */
    for (int i = 0; i < BLOCK; i++) bl[i] = l, br[i] = r;
    hos_menu_sound_mix(bl, br, BLOCK, gain);
}
static int block_is(int from, int to, float l, float r) {
    for (int i = from; i < to; i++)
        if (!nearly(bl[i], l) || !nearly(br[i], r)) return 0;
    return 1;
}
static int64_t t0 = 1000000000ll;
static const int64_t kMs = 1000000;
static int restart_with, restart_at;
static void restart(int at) {   /* (a play that lands inside the mix, once) */
    if (at != restart_at) return;
    between = NULL;
    hos_menu_sound_play(restart_with, t0);
}
static void voices_and_mix(void) {
    install(OPEN, 40000, 0.25f, -0.5f), install(CLOSE, 3000, 0.125f, 0.125f), install(CURSOR, 30000, 0.5f, 0.5f);
    install(DECIDE, 100, 1.0f, -1.0f), install(CANCEL, 300, 0.25f, 0.25f), install(TAB_RIGHT, 5000, 0.0f, 0.0625f);
    /* (TAB_LEFT: the game's files did not have it) */
    {   /* before the files are read: nothing starts, nothing is mixed, nothing is touched */
        hos_menu_sound_play(OPEN, t0);
        block(1.0f, 0.5f, 0.5f);
        CHECK(block_is(0, BLOCK, 0.5f, 0.5f));
    }
    quiet();
    {   /* none of the seven, or one the game's files did not have: nothing */
        hos_menu_sound_play(0, t0), hos_menu_sound_play(-1, t0), hos_menu_sound_play(HOS_MENU_SOUND_N, t0), hos_menu_sound_play(1000, t0);
        hos_menu_sound_play(TAB_LEFT, t0);
        CHECK(active() == 0);
    }
    {   /* a sound is added to what the block holds, at the gain, left and right; the cursor's tick at 0.6 of it */
        hos_menu_sound_play(OPEN, t0);
        CHECK(active() == 1 && voice_with(OPEN) == 0);
        block(0.8f, 0.1f, -0.1f);
        CHECK(block_is(0, BLOCK, 0.1f + 0.25f * 0.8f, -0.1f - 0.5f * 0.8f));
        CHECK(voice_pos(0) == BLOCK);
        block(0.0f, 0.1f, -0.1f);   /* at gain 0 nothing is added, and the sound goes on */
        CHECK(block_is(0, BLOCK, 0.1f, -0.1f) && voice_pos(0) == 2 * BLOCK);
        quiet();
        hos_menu_sound_play(CURSOR, t0 + 500 * kMs);   /* (not within 180 ms of the sound before: see below) */
        block(0.5f, 0.0f, 0.0f);
        CHECK(block_is(0, BLOCK, 0.5f * 0.5f * 0.6f, 0.5f * 0.5f * 0.6f));
        quiet();
    }
    {   /* a free voice each, while there is one; then the oldest's */
        int64_t t = t0;
        for (int i = 0; i < VOICES; i++, t += 1000 * kMs) {
            hos_menu_sound_play(OPEN, t);
            CHECK(active() == i + 1 && voice_pos(i) == 0 && atomic_load(&voices[i].sound) == OPEN);
            for (int k = 0; k < 10; k++) block(1.0f, 0.0f, 0.0f);   /* (50 ms on: the next one is not a restart of this) */
            CHECK(block_is(0, BLOCK, 0.25f * (float)(i + 1), -0.5f * (float)(i + 1)));   /* (all of them are heard) */
        }
        CHECK(active() == VOICES);
        hos_menu_sound_play(CLOSE, t);   /* a ninth: the first one's voice */
        CHECK(active() == VOICES && atomic_load(&voices[0].sound) == CLOSE && voice_pos(0) == 0);
        CHECK(atomic_load(&voices[1].sound) == OPEN && voice_pos(1) == 2400 * 7);
        hos_menu_sound_play(CANCEL, t);   /* a tenth: the second's, the oldest now */
        CHECK(atomic_load(&voices[1].sound) == CANCEL && voice_pos(1) == 0);
        CHECK(atomic_load(&voices[0].sound) == CLOSE && atomic_load(&voices[2].sound) == OPEN);
        quiet();
    }
    {   /* a sound ends at its last sample: the rest of the block is as it was, the voice free again and the first taken */
        hos_menu_sound_play(OPEN, t0), hos_menu_sound_play(CANCEL, t0);
        CHECK(voice_with(OPEN) == 0 && voice_with(CANCEL) == 1);
        block(1.0f, 0.0f, 0.0f);
        CHECK(block_is(0, BLOCK, 0.5f, -0.25f));
        block(1.0f, 0.0f, 0.0f);   /* CANCEL has 300 samples: 60 of them here */
        CHECK(block_is(0, 60, 0.5f, -0.25f) && block_is(60, BLOCK, 0.25f, -0.5f));
        CHECK(active() == 1 && voice_pos(1) == -1);
        block(1.0f, 0.0f, 0.0f);
        CHECK(block_is(0, BLOCK, 0.25f, -0.5f));
        hos_menu_sound_play(CLOSE, t0);
        CHECK(voice_with(CLOSE) == 1);
        quiet();
        hos_menu_sound_play(DECIDE, t0);   /* 100 samples, less than a block */
        block(0.5f, 0.0f, 0.0f);
        CHECK(block_is(0, 100, 0.5f, -0.5f) && block_is(100, BLOCK, 0.0f, 0.0f) && active() == 0);
        quiet();
    }
    {   /* the same sound again within 50 ms starts over on its voice; later it is a second one */
        hos_menu_sound_play(OPEN, t0);
        block(1.0f, 0.0f, 0.0f);
        hos_menu_sound_play(OPEN, t0 + 5 * kMs);
        CHECK(active() == 1 && voice_pos(0) == 0);
        for (int k = 0; k < 10; k++) block(1.0f, 0.0f, 0.0f);
        hos_menu_sound_play(OPEN, t0 + 60 * kMs);
        CHECK(active() == 2);
        quiet();
    }
    {   /* the cursor's tick: not within 180 ms of the sound before it, whatever that was; a decision within 50 ms of a
           tick takes the tick's voice */
        hos_menu_sound_play(CURSOR, t0 + 1000 * kMs);
        CHECK(active() == 1 && voice_with(CURSOR) == 0);
        block(1.0f, 0.0f, 0.0f);
        hos_menu_sound_play(CURSOR, t0 + 1100 * kMs);
        hos_menu_sound_play(CURSOR, t0 + 1179 * kMs);
        CHECK(active() == 1 && voice_pos(0) == BLOCK);
        hos_menu_sound_play(DECIDE, t0 + 1179 * kMs);
        CHECK(active() == 1 && voice_with(DECIDE) == 0 && voice_with(CURSOR) < 0 && voice_pos(0) == 0);
        hos_menu_sound_play(CURSOR, t0 + 1300 * kMs);   /* (121 ms after the decision) */
        CHECK(voice_with(CURSOR) < 0);
        hos_menu_sound_play(CURSOR, t0 + 1359 * kMs);
        CHECK(voice_with(CURSOR) >= 0);
        quiet();
        hos_menu_sound_play(CURSOR, t0 + 3000 * kMs);
        for (int k = 0; k < 10; k++) block(1.0f, 0.0f, 0.0f);   /* (50 ms on) */
        hos_menu_sound_play(DECIDE, t0 + 3050 * kMs);
        CHECK(active() == 2 && voice_with(CURSOR) == 0 && voice_with(DECIDE) == 1);
        quiet();
    }
    {   /* a restart inside the mix, between its read of the voice's place and of its sound: the tick is 240 samples in when
           the decision, 100 samples long, takes its voice. The place is the tick's, the sound the decision's: nothing of
           it is mixed in this block (240 is past the decision's end, and its samples end where nothing can be read),
           and it plays from its start in the next */
        t0 += 10000 * kMs;
        hos_menu_sound_play(CURSOR, t0);
        block(1.0f, 0.0f, 0.0f);
        restart_with = DECIDE, restart_at = 0, between = restart;
        block(1.0f, 0.125f, 0.125f);
        CHECK(between == NULL);   /* (the play did land) */
        CHECK(block_is(0, BLOCK, 0.125f, 0.125f));
        CHECK(active() == 1 && voice_with(DECIDE) == 0 && voice_pos(0) == 0);
        block(1.0f, 0.0f, 0.0f);
        CHECK(block_is(0, 100, 1.0f, -1.0f) && block_is(100, BLOCK, 0.0f, 0.0f) && active() == 0);
        quiet();
    }
    {   /* ...and with a longer sound in its place: the old place is not taken into the new sound (none of it in this
           block), it plays from its start in the next */
        t0 += 10000 * kMs;
        hos_menu_sound_play(CURSOR, t0);
        block(1.0f, 0.0f, 0.0f);
        restart_with = OPEN, restart_at = 0, between = restart;
        block(1.0f, 0.125f, 0.125f);
        CHECK(between == NULL && block_is(0, BLOCK, 0.125f, 0.125f));
        CHECK(active() == 1 && voice_with(OPEN) == 0 && voice_pos(0) == 0);
        block(1.0f, 0.0f, 0.0f);
        CHECK(block_is(0, BLOCK, 0.25f, -0.5f) && voice_pos(0) == BLOCK);
        quiet();
    }
    {   /* a restart after the mix added its block and before it moved the voice on: the new sound's start is kept */
        t0 += 10000 * kMs;
        hos_menu_sound_play(CURSOR, t0);
        block(1.0f, 0.0f, 0.0f);
        restart_with = DECIDE, restart_at = 1, between = restart;
        block(1.0f, 0.0f, 0.0f);
        CHECK(between == NULL && block_is(0, BLOCK, 0.3f, 0.3f));   /* (the tick's block was already added) */
        CHECK(active() == 1 && voice_with(DECIDE) == 0 && voice_pos(0) == 0);
        block(1.0f, 0.0f, 0.0f);
        CHECK(block_is(0, 100, 1.0f, -1.0f) && block_is(100, BLOCK, 0.0f, 0.0f) && active() == 0);
        quiet();
    }
    {   /* ...and the same with a sound that restarts itself in its last block: it is not cut off as ended */
        t0 += 10000 * kMs;
        hos_menu_sound_play(DECIDE, t0);
        restart_with = DECIDE, restart_at = 1, between = restart;
        block(1.0f, 0.0f, 0.0f);
        CHECK(between == NULL && active() == 1 && voice_pos(0) == 0);
        block(1.0f, 0.0f, 0.0f);
        CHECK(block_is(0, 100, 1.0f, -1.0f) && active() == 0);
        quiet();
    }
    {   /* a voice whose place is past its sound's end, should that ever be: nothing read, and the voice is free again */
        atomic_store(&voices[3].sound, DECIDE), atomic_store(&voices[3].at, ((int64_t)9 << 32) | 500);
        block(1.0f, 0.25f, 0.25f);
        CHECK(block_is(0, BLOCK, 0.25f, 0.25f) && active() == 0);
        quiet();
    }
    CHECK(nearly(hos_menu_sound_gain(0), 0.0f) && nearly(hos_menu_sound_gain(-5), 0.0f) && nearly(hos_menu_sound_gain(80), 0.8f));
    CHECK(nearly(hos_menu_sound_gain(5), 0.05f) && nearly(hos_menu_sound_gain(100), 1.0f) && nearly(hos_menu_sound_gain(250), 1.0f));
}

/* ---- the game's files, as the test wrote them: all seven two channels of PCM16 that never change, so a block's sum
   says which sounds are in it ---- */
static int16_t held[HOS_MENU_SOUND_N - 1][2][4000];   /* (a sound's samples each: the seven and the Mods screen's fanfare) */
static void rom_put(const char *rel, const uint8_t *data, uint64_t size) { rom[nrom].rel = rel, rom[nrom].data = data, rom[nrom].size = size, nrom++; }
static void rom_sound(int id, uint32_t frames, int l, int r) {
    for (uint32_t i = 0; i < frames; i++) held[id - 1][0][i] = (int16_t)l, held[id - 1][1][i] = (int16_t)r;
    const Chan c[2] = {chan(0, 0, 48000, frames, held[id - 1][0]), chan(0, 1, 48000, frames, held[id - 1][1])};
    uint64_t size;
    const uint8_t *file = bwav(c, 2, &size);
    rom_put(files[id], file, size);
}
static void rom_seven(void) {
    rom_sound(OPEN, 2000, 8192, 8192), rom_sound(CLOSE, 1000, 4096, 4096), rom_sound(CURSOR, 4000, 16384, 16384);
    rom_sound(DECIDE, 1000, 8192, -8192), rom_sound(CANCEL, 500, 2048, 2048), rom_sound(TAB_LEFT, 500, 4096, 0);
    rom_sound(TAB_RIGHT, 500, 0, 4096), rom_sound(FANFARE, 1500, 8192, 8192);   /* ((Triforce) the Mods screen's install done) */
    rom_sound(SYS_CURSOR, 600, 2048, 4096), rom_sound(SYS_DECIDE, 700, 4096, 2048);   /* ((Triforce, 11-17) the Mods entry's */
}
static void frame(float l, float r) {
    for (int i = 0; i < BLOCK; i++) bl[i] = l, br[i] = r;
    hos_menu_sound_frame(bl, br, BLOCK);
}
static void loading(void) {
    /* five of the seven: one is not in the game's files, one is not a sound; and one is a mod's at another rate */
    static int16_t ramp[60];
    for (int i = 0; i < 60; i++) ramp[i] = (int16_t)(i * 64);
    const Chan mono = chan(1, 2, 24000, 60, ramp);
    uint64_t size;
    const uint8_t *file = bwav(&mono, 1, &size);
    CHECK(encode_error == 0);
    rom_sound(OPEN, 2000, 8192, 8192), rom_sound(CURSOR, 4000, 16384, 16384), rom_sound(DECIDE, 1000, 8192, -8192);
    rom_sound(TAB_LEFT, 500, 4096, 0);
    rom_put(files[CLOSE], file, size);
    rom_put(files[CANCEL], (const uint8_t *)"not a sound at all, whatever its name says", 42);
    static uint8_t huge[FILE_MOST + 1] = "BWAV";
    rom_put(files[TAB_RIGHT], huge, sizeof huge);   /* (more than a sound's file ever has: not taken) */
    CHECK(atomic_load(&loaded) == 0);
    load_all();
    CHECK(atomic_load(&loaded) == 2 && rom_reads == 10);
    CHECK(strstr(logged, "menu sounds: 5 of 10 interface sounds from the game's files"));
    CHECK(strstr(logged, "SE_Sys_Cancel00.bwav is not a BWAV file the options play") && strstr(logged, "left out"));
    CHECK(sounds[OPEN].pcm && sounds[OPEN].frames == 2000 && sounds[OPEN].pcm[0] == 0.25f && sounds[OPEN].pcm[3999] == 0.25f);
    CHECK(sounds[DECIDE].pcm && sounds[DECIDE].pcm[1] == -0.25f && sounds[TAB_LEFT].pcm && sounds[TAB_LEFT].pcm[1] == 0.0f);
    CHECK(!sounds[CANCEL].pcm && !sounds[TAB_RIGHT].pcm && !sounds[0].pcm);
    CHECK(sounds[CLOSE].pcm && sounds[CLOSE].frames == 120);   /* 60 samples at 24 kHz are 120 at 48 */
    CHECK(nearly(sounds[CLOSE].pcm[2 * 20], f16(ramp[10]) * 0.7071f) && nearly(sounds[CLOSE].pcm[2 * 21 + 1], (f16(ramp[10]) + f16(ramp[11])) / 2 * 0.7071f));
    /* the ones that are not there stay silent; the others play */
    row_volume = 100;
    ask(CANCEL), ask(TAB_RIGHT);
    frame(0.0f, 0.0f);
    CHECK(block_is(0, BLOCK, 0.0f, 0.0f) && active() == 0 && asked_at == 2);
    ask(TAB_LEFT);
    frame(0.0f, 0.0f);
    CHECK(block_is(0, BLOCK, 0.125f, 0.0f));
    CHECK(load_requests == 0);   /* (they were read already) */
}
static void frames_and_events(void) {
    rom_seven();
    /* no renderer with the options: nothing is asked of it but the row, nothing read, nothing added */
    ask(DECIDE);
    frame(0.5f, 0.5f);
    CHECK(block_is(0, BLOCK, 0.5f, 0.5f) && load_requests == 0 && asked_at == 0 && atomic_load(&loaded) == 0);
    /* the row at 0: the files are not read; what the options still asked for is taken and not played */
    row_volume = 0;
    frame(0.5f, 0.5f);
    CHECK(block_is(0, BLOCK, 0.5f, 0.5f) && load_requests == 0 && asked_at == 1 && rom_reads == 0);
    /* the row above 0: the files are asked for, once, on their own thread (here: when the test says); a sound asked for
       before they are read is taken and never played */
    row_volume = 80;
    ask(OPEN);
    frame(0.0f, 0.0f);
    CHECK(load_requests == 1 && asked_at == 2 && rom_reads == 0 && block_is(0, BLOCK, 0.0f, 0.0f));
    frame(0.0f, 0.0f), frame(0.0f, 0.0f);
    CHECK(load_requests == 1);
    load_all();
    CHECK(strstr(logged, "menu sounds: 10 of 10 interface sounds from the game's files") && rom_reads == 10);
    frame(0.0f, 0.0f);
    CHECK(load_requests == 1 && block_is(0, BLOCK, 0.0f, 0.0f) && active() == 0);
    /* a sound asked for is in the very next block, at the row's level, over what the block holds */
    ask(DECIDE);
    frame(0.1f, 0.1f);
    CHECK(asked_at == 3 && block_is(0, BLOCK, 0.1f + 0.25f * 0.8f, 0.1f - 0.25f * 0.8f));
    /* two asked for since the last block: both, in the order asked */
    now_ns += 1000 * kMs;
    for (int k = 0; k < 5; k++) frame(0.0f, 0.0f);   /* (DECIDE ends: 1000 samples) */
    CHECK(active() == 0);
    ask(TAB_LEFT), ask(CANCEL);
    frame(0.0f, 0.0f);
    CHECK(block_is(0, BLOCK, (0.125f + 0.0625f) * 0.8f, 0.0625f * 0.8f));
    CHECK(voice_with(TAB_LEFT) == 0 && voice_with(CANCEL) == 1 && voices[0].started < voices[1].started);
    /* the row is read at each block: 100, then 0 (silent at once, the sounds go on unheard), then 50 */
    row_volume = 100;
    frame(0.0f, 0.0f);
    CHECK(block_is(0, BLOCK, 0.125f + 0.0625f, 0.0625f) && voice_pos(0) == 2 * BLOCK);
    row_volume = 0;
    frame(0.0f, 0.0f);   /* (both are 500 samples: their last 20 in this block, unheard, and they end) */
    CHECK(block_is(0, BLOCK, 0.0f, 0.0f) && active() == 0);
    ask(OPEN);   /* (asked as the row went to 0: taken, not played, and not played later) */
    frame(0.0f, 0.0f);
    CHECK(asked_at == nasked && voice_with(OPEN) < 0);
    CHECK(active() == 0);
    row_volume = 50;
    frame(0.0f, 0.0f);
    CHECK(block_is(0, BLOCK, 0.0f, 0.0f));
    /* the cursor's tick asked for at every step of a held direction: five a second are heard */
    now_ns += 1000 * kMs;
    int ticks = 0;
    for (int k = 0; k < 200; k++, now_ns += 5 * kMs) {   /* one second of blocks, a tick asked for at every third */
        if (k % 3 == 0) ask(CURSOR);
        frame(0.0f, 0.0f);
        ticks += voice_with(CURSOR) >= 0 && voice_pos(voice_with(CURSOR)) == BLOCK;
        CHECK(asked_at == nasked);
        if (nasked > 48) nasked = asked_at = 0;
    }
    CHECK(ticks == 6);   /* (at 0, 180, 360, 540, 720 and 900 ms) */
    now_ns += 1000 * kMs;
    for (int k = 0; k < 40; k++) frame(0.0f, 0.0f);
    CHECK(active() == 0);
    /* many asked for at once (the sound thread was held up): eight a block, in order, none lost */
    nasked = asked_at = 0;
    for (int k = 0; k < 5; k++) ask(DECIDE), ask(CANCEL);
    frame(0.0f, 0.0f);
    CHECK(asked_at == 8);
    frame(0.0f, 0.0f);
    CHECK(asked_at == 10);
    CHECK(volume_reads > 200 && rom_reads == 10);   /* (the files were read once) */
}

/* ---- the end of nn_audio.c's render_frame, cut out as it is: the options' sounds, the Master volume, the 16 bits ---- */
#define FRAME 240
static float master = 1.0f;
static float master_volume(void) { return master; }
static void render_tail(float out[6][FRAME], int16_t *pcm, const int nch) {
#include "render_tail.inc"
}
static float out6[6][FRAME];
static int16_t pcm6[FRAME * 6];
static void tail(float front, float others, int nch) {
    for (int ch = 0; ch < 6; ch++)
        for (int i = 0; i < FRAME; i++) out6[ch][i] = ch < 2 ? front : others;
    memset(pcm6, 0x55, sizeof pcm6);
    render_tail(out6, pcm6, nch);
}
static int pcm_is(int nch, int ch, int from, int to, float want) {
    for (int i = from; i < to; i++) {
        const int got = pcm6[i * nch + ch], exact = (int)(want * 32767.0f);
        if (got < exact - 1 || got > exact + 1) { printf("channel %d sample %d: %d, not %d\n", ch, i, got, exact); return 0; }
    }
    return 1;
}
static void master_and_clipping(void) {
    rom_seven();
    row_volume = 100;
    tail(0.0f, 0.0f, 2);   /* (the files are asked for) */
    load_all();
    /* the game's sound alone, stereo and 5.1 */
    tail(0.25f, 0.125f, 2);
    CHECK(pcm_is(2, 0, 0, FRAME, 0.25f) && pcm_is(2, 1, 0, FRAME, 0.25f) && pcm6[FRAME * 2] == 0x5555);
    tail(0.25f, 0.125f, 6);
    for (int ch = 0; ch < 6; ch++) CHECK(pcm_is(6, ch, 0, FRAME, ch < 2 ? 0.25f : 0.125f));
    /* a sound of the options: over the game's, in the front left and right alone */
    ask(DECIDE);   /* 0.25 left, -0.25 right, 1000 samples */
    tail(0.25f, 0.125f, 6);
    CHECK(pcm_is(6, 0, 0, FRAME, 0.5f) && pcm_is(6, 1, 0, FRAME, 0.0f));
    for (int ch = 2; ch < 6; ch++) CHECK(pcm_is(6, ch, 0, FRAME, 0.125f));
    /* the Master volume is over both: at 50% the sum is halved (a change is ramped over one block: the block after it) */
    master = 0.5f;
    tail(0.25f, 0.125f, 2);
    CHECK(pcm6[0] < (int)(0.5f * 32767) && pcm6[(FRAME - 1) * 2] <= (int)(0.25f * 32767) + 1);   /* (on its way down) */
    tail(0.25f, 0.125f, 2);
    CHECK(pcm_is(2, 0, 0, FRAME, 0.25f) && pcm_is(2, 1, 0, FRAME, 0.0f));
    row_volume = 40;   /* ...and the row under it: 0.25 + 0.25 * 0.4, halved */
    tail(0.25f, 0.125f, 2);
    CHECK(pcm_is(2, 0, 0, FRAME, 0.175f) && pcm_is(2, 1, 0, FRAME, 0.075f));
    master = 0.0f;   /* Muted: nothing, the options' sounds neither */
    tail(0.25f, 0.125f, 2);
    tail(0.25f, 0.125f, 2);
    CHECK(pcm_is(2, 0, 0, FRAME, 0.0f) && pcm_is(2, 1, 0, FRAME, 0.0f));
    /* too loud together: held at the 16 bits' ends, not wrapped */
    master = 1.0f, row_volume = 100;
    now_ns += 1000 * kMs;
    for (int k = 0; k < 6; k++) tail(0.0f, 0.0f, 2);   /* (the ramp back up; DECIDE ends) */
    CHECK(active() == 0);
    ask(OPEN), ask(DECIDE), ask(CANCEL);   /* 0.25 + 0.25 + 0.0625 left, 0.25 - 0.25 + 0.0625 right */
    tail(0.95f, 0.0f, 2);
    CHECK(active() == 3);
    CHECK(pcm_is(2, 0, 0, FRAME, 1.0f) && pcm_is(2, 1, 0, FRAME, 1.0f) && pcm6[0] == 32767 && pcm6[1] == 32767);
    tail(-0.9f, 0.0f, 2);
    CHECK(pcm_is(2, 0, 0, FRAME, -0.3375f) && pcm_is(2, 1, 0, FRAME, -0.8375f));
    tail(-1.9f, 0.0f, 2);
    CHECK(pcm6[0] == -32768 && pcm6[1] == -32768 && pcm6[(FRAME - 1) * 2] == -32768);
    tail(NAN, 0.0f, 2);   /* (not a number, from the game's side: silence, as before) */
    CHECK(pcm6[0] == 0 && pcm6[1] == 0);
}

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "";
    if (!strcmp(what, "decode")) decode_pcm16(), decode_adpcm(), decode_rates(), decode_refusals();
    else if (!strcmp(what, "voices")) voices_and_mix();
    else if (!strcmp(what, "load")) loading();
    else if (!strcmp(what, "frames")) frames_and_events();
    else if (!strcmp(what, "master")) master_and_clipping();
    else return printf("no such scenario: %s\n", what), 2;
    if (fails) return printf("%s: %d checks failed\n", what, fails), 1;
    printf("%s: ok\n", what);
    return 0;
}
