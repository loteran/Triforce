/* (Triforce) The options' sounds: the game's own interface sounds (Sound/Resource/Stream/SE_Sys_*.bwav of the player's
   game files: the cursor, decide, cancel, the Plus menu's open and close, the tab moves), read once, decoded and mixed
   over the game's output. After NX2navi's menu sounds (its runtime/menu_sound.c, the owner's): the decoder, the voices
   and the mix are that file's. Nothing is shipped: game files without the sounds stay silent, and a mod's file is
   played where a mod replaces one (hos_fs_rom_file, nn_fs.c).

   BWAV: "BWAV", byte order mark, version, CRC32, a prefetch flag (u16) and the channel count (u16), then 0x4C bytes per
   channel: codec (u16, 0 PCM16, 1 DSP-ADPCM), pan (u16, 0 left, 1 right, 2 middle), sample rate, sample count (u32),
   the count again, 16 DSP coefficients (s16), the absolute offset of the samples (u32), ... The interface sounds are
   48 kHz DSP-ADPCM, one channel in the middle or two, left and right: 8-byte frames of a header (coefficient pair
   index, scale exponent) and 14 four-bit samples. A file of another rate (a mod's) is brought to 48 kHz when it is
   read, in a straight line between its samples: the mixer's rate is the output's (nn_audio.c RATE), always.

   Which sound: the options say (gpu/vk/vk_menu.cpp, gpu/vk/menu_sound_events.h), and the sound thread asks them at each
   block it mixes, 5 ms (hos_menu_sound_frame: rx_option HOS_OPT_MENU_SOUND). So a sound is in the block after its
   press, and what is heard later than that is the output's own delay, the game's sounds' too. The files are read on a
   thread of their own, when the sound thread first finds the Menu sounds row above 0; a sound asked for before they
   are read is not played.

   hos_menu_sound_play starts a voice (a free one, else the oldest), hos_menu_sound_mix adds the voices to a block. A
   voice is free while its place is negative; play sets its sound first and its place last (release), the mix reads
   them in the other order (acquire) and moves the place on only if it is still the one it read, so the two may run on
   different threads without a lock. The place is the frame with the number of the voice's start above it: NX2navi's
   was the frame alone, and a voice started again while its first block was mixed had the frame it had before, 0, so
   the mix moved the new sound on by a block it never played (or ended one shorter than a block unheard). Here both
   run on the sound thread, one after the other.

   The level: the Audio tab's Menu sounds row (menu_sounds= in triforce_options.ini, 0 to 100, 80 unless set; 0
   silent), read at each block, and the Master volume over it as over the game's own sound (the sounds are added before
   render_frame applies it). They are added after the game's limiter, and the block is clipped as it is made 16-bit. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "menu_sound.h"
#include "../gpu/vk/vk_api.h"   /* (HOS_MENU_SOUND_*, HOS_OPT_MENU_SOUND, HOS_OPT_MENU_SOUND_VOLUME) */

#ifndef MENU_SOUND_TEST   /* (tests/menu_sound_test.c has its own of these, and reads the files when it says) */
#include "hos.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int rx_option(int id, int otherwise);                                              /* rx_backend.c */
uint8_t *hos_fs_rom_file(const char *rel, uint64_t most, uint64_t *size);   /* nn_fs.c */
static void load_all(void);
static DWORD WINAPI load_main(void *arg) {
    (void)arg;
    load_all();
    return 0;
}
static void load_start(void) {   /* (the files are read off the sound thread: a disk may take its time) */
    const HANDLE t = CreateThread(NULL, 0, load_main, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else hos_log("menu sounds: no thread to read the game's interface sounds: the options stay silent");
}
#endif

#define MIX_RATE 48000   /* (nn_audio.c RATE: the output's) */
#define SOUNDS HOS_MENU_SOUND_N   /* (by HOS_MENU_SOUND_*: 0 is no sound) */
#define FILE_MOST (4u << 20)      /* (the longest of the game's seven is 138 KB) */

typedef struct {
    float *pcm;          /* interleaved stereo at 48 kHz */
    uint32_t frames;
} Sound;

static const char *const files[SOUNDS] = {
    [HOS_MENU_SOUND_OPEN] = "Sound/Resource/Stream/SE_Sys_PlusMenu_Open00.bwav",
    [HOS_MENU_SOUND_CLOSE] = "Sound/Resource/Stream/SE_Sys_PlusMenu_Close00.bwav",
    [HOS_MENU_SOUND_CURSOR] = "Sound/Resource/Stream/SE_Sys_Cursor00.bwav",
    [HOS_MENU_SOUND_DECIDE] = "Sound/Resource/Stream/SE_Sys_Do00.bwav",
    [HOS_MENU_SOUND_CANCEL] = "Sound/Resource/Stream/SE_Sys_Cancel00.bwav",
    [HOS_MENU_SOUND_TAB_LEFT] = "Sound/Resource/Stream/SE_Sys_MinusMenu_TabMove_L00.bwav",
    [HOS_MENU_SOUND_TAB_RIGHT] = "Sound/Resource/Stream/SE_Sys_MinusMenu_TabMove_R00.bwav",
    /* ((Triforce) the Mods screen's install done: the game's own item-get fanfare, its short da-na-na-naaa (4 s; _Intro is the
       whole chest's opening before it)) */
    [HOS_MENU_SOUND_FANFARE] = "Sound/Resource/Stream/BGM_ItemGet_Medium.bwav",
    /* ((Triforce) 11-17) the Mods entry under the title's list and the System page: the sounds those lists make (the lab's
       file opens: the title's cursor, its Continue and the Load list's choice) */
    [HOS_MENU_SOUND_SYS_CURSOR] = "Sound/Resource/Stream/SE_Sys_SysMenu_Cursor00.bwav",
    [HOS_MENU_SOUND_SYS_DECIDE] = "Sound/Resource/Stream/SE_Sys_SysMenu_DecideL00.bwav",
};

static Sound sounds[SOUNDS];
static atomic_int loaded;   /* 0 not yet, 1 being read (sounds[] is written before the release store of 2), 2 ready */

#define VOICES 8
static struct {
    _Atomic int64_t at;   /* the number of its start << 32 | the frame; -1 free */
    atomic_int sound;
    uint64_t started;
} voices[VOICES];
static uint64_t started_n;
#define AT_FRAME(at) ((uint32_t)(at))
#define AT_START(at) ((at) & ~(int64_t)0xffffffff)

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int16_t clamp16(int64_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* One channel into dst[2 * i] and dst[2 * i + 1] by its pan (0 the left alone, 1 the right alone, else both). 0 when
   the samples do not lie inside the file. */
static int decode_channel(const uint8_t *b, uint64_t size, const uint8_t *info, float *dst, uint32_t frames) {
    const uint16_t codec = u16(info), pan = u16(info + 2);
    uint32_t count = u32(info + 8);
    const uint32_t off = u32(info + 0x30);
    if (count > frames) count = frames;
    int16_t coef[16];
    for (int i = 0; i < 16; i++) coef[i] = (int16_t)u16(info + 0x10 + 2 * i);
    const float scale = 1.0f / 32768.0f;
    if (codec == 0) {   /* PCM16 */
        if ((uint64_t)off + (uint64_t)count * 2 > size) return 0;
        for (uint32_t i = 0; i < count; i++) {
            const float v = (int16_t)u16(b + off + 2 * (uint64_t)i) * scale;
            if (pan != 1) dst[2 * (size_t)i] += v;
            if (pan != 0) dst[2 * (size_t)i + 1] += v;
        }
        return 1;
    }
    if (codec != 1) return 0;
    const uint64_t need = ((uint64_t)count + 13) / 14 * 8;
    if ((uint64_t)off + need > size) return 0;
    int32_t h1 = 0, h2 = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *fr = b + off + (uint64_t)(i / 14) * 8;
        const uint32_t k = i % 14;
        const int idx = fr[0] >> 4 & 7, sh = fr[0] & 15;
        int nib = fr[1 + k / 2] >> (k & 1 ? 0 : 4) & 15;
        if (nib >= 8) nib -= 16;
        /* (64 bits: coefficients a file made up could carry the sum past 32) */
        const int64_t s = (int64_t)nib * (1 << sh) * 2048 + (int64_t)coef[2 * idx] * h1 + (int64_t)coef[2 * idx + 1] * h2;
        const int16_t v = clamp16((s + 1024) >> 11);
        h2 = h1, h1 = v;
        if (pan != 1) dst[2 * (size_t)i] += v * scale;
        if (pan != 0) dst[2 * (size_t)i + 1] += v * scale;
    }
    return 1;
}

/* pcm (*frames stereo frames at `rate`) at 48 kHz, in a straight line between its samples; pcm is freed */
static float *to_mix_rate(float *pcm, uint32_t *frames, uint32_t rate) {
    const uint64_t n = ((uint64_t)*frames * MIX_RATE + rate - 1) / rate;
    float *out = (float *)malloc((size_t)n * 2 * sizeof *out);
    for (uint64_t i = 0; out && i < n; i++) {
        const uint64_t at = i * rate;   /* (in 48000ths of a frame of pcm) */
        const uint32_t k = (uint32_t)(at / MIX_RATE), k1 = k + 1 < *frames ? k + 1 : k;
        const float t = (float)(at % MIX_RATE) / MIX_RATE;
        out[2 * i] = pcm[2 * (size_t)k] + (pcm[2 * (size_t)k1] - pcm[2 * (size_t)k]) * t;
        out[2 * i + 1] = pcm[2 * (size_t)k + 1] + (pcm[2 * (size_t)k1 + 1] - pcm[2 * (size_t)k + 1]) * t;
    }
    free(pcm);
    if (out) *frames = (uint32_t)n;
    return out;
}

float *hos_menu_sound_decode(const uint8_t *b, uint64_t size, uint32_t *frames_out) {
    if (size < 0x10 || memcmp(b, "BWAV", 4)) return NULL;
    const uint16_t nch = u16(b + 0x0e);
    if (!nch || nch > 2 || 0x10 + (uint64_t)nch * 0x4c > size) return NULL;
    const uint32_t rate = u32(b + 0x10 + 4);
    uint32_t frames = u32(b + 0x10 + 8);
    /* (the interface sounds are 48 kHz and short: at most ten seconds of a rate a sound file has) */
    if (rate < 8000 || rate > 192000 || !frames || frames > rate * 10) return NULL;
    float *pcm = (float *)calloc((size_t)frames * 2, sizeof *pcm);
    int ok = pcm != NULL;
    for (uint16_t c = 0; ok && c < nch; c++) ok = decode_channel(b, size, b + 0x10 + c * 0x4c, pcm, frames);
    if (!ok) {
        free(pcm);
        return NULL;
    }
    if (nch == 1 && u16(b + 0x10 + 2) > 1)   /* a middle-panned mono sound in both */
        for (size_t i = 0; i < (size_t)frames * 2; i++) pcm[i] *= 0.7071f;
    if (rate != MIX_RATE && !(pcm = to_mix_rate(pcm, &frames, rate))) return NULL;
    *frames_out = frames;
    return pcm;
}

static void load_all(void) {   /* (once: hos_menu_sound_frame's store of 1 into loaded starts it) */
    int n = 0;
    for (int id = 1; id < SOUNDS; id++) {
        uint64_t size = 0;
        uint8_t *b = hos_fs_rom_file(files[id], FILE_MOST, &size);
        if (!b) continue;
        uint32_t frames = 0;
        float *pcm = hos_menu_sound_decode(b, size, &frames);
        free(b);
        if (!pcm) {
            hos_log("menu sounds: %s is not a BWAV file the options play (PCM16 or DSP-ADPCM, one or two channels, at most 10 s): "
                    "left out", files[id]);
            continue;
        }
        sounds[id] = (Sound){pcm, frames};
        n++;
    }
    for (int i = 0; i < VOICES; i++) atomic_store(&voices[i].at, -1);
    hos_log("menu sounds: %d of %d interface sounds from the game's files", n, SOUNDS - 1);
    atomic_store_explicit(&loaded, 2, memory_order_release);
}

float hos_menu_sound_gain(int percent) { return percent <= 0 ? 0.0f : percent >= 100 ? 1.0f : (float)percent / 100.0f; }

/* The least time between a cursor tick and the sound before it. Moving down a list or changing a value asks for a tick
   at every step, up to 30 a second while a direction is held, and the focus moving with a press or a tab change is
   part of that sound: at most five ticks a second are heard. */
#define CURSOR_GAP_NS 180000000ll
#define CURSOR_GAIN 0.6f   /* the tick under the sounds of a decision */
static _Atomic int64_t last_start_ns = -CURSOR_GAP_NS;

void hos_menu_sound_play(int id, int64_t now_ns) {
    if (id <= 0 || id >= SOUNDS) return;
    if (id == HOS_MENU_SOUND_CURSOR && now_ns - atomic_load(&last_start_ns) < CURSOR_GAP_NS) return;
    atomic_store(&last_start_ns, now_ns);
    if (atomic_load_explicit(&loaded, memory_order_acquire) != 2 || !sounds[id].pcm) return;
    int pick = 0;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < VOICES; i++) {
        if (atomic_load_explicit(&voices[i].at, memory_order_acquire) < 0) { pick = i; break; }
        if (voices[i].started < oldest) oldest = voices[i].started, pick = i;
    }
    /* A sound that replaces itself restarts instead of stacking up, and a decision replaces the tick of the same press
       (a row changes its value in the tick the press is seen). */
    for (int i = 0; i < VOICES; i++) {
        const int64_t at = atomic_load_explicit(&voices[i].at, memory_order_acquire);
        if (at >= 0 && (atomic_load(&voices[i].sound) == id || atomic_load(&voices[i].sound) == HOS_MENU_SOUND_CURSOR) &&
            AT_FRAME(at) < 2400) {   /* started under 50 ms ago */
            pick = i;
            break;
        }
    }
    atomic_store_explicit(&voices[pick].at, -1, memory_order_release);
    atomic_store_explicit(&voices[pick].sound, id, memory_order_relaxed);
    voices[pick].started = __atomic_add_fetch(&started_n, 1, __ATOMIC_RELAXED);
    atomic_store_explicit(&voices[pick].at, (int64_t)(voices[pick].started & 0x7fffffff) << 32, memory_order_release);
}

#ifndef MENU_SOUND_BETWEEN   /* (tests/menu_sound_test.c: a play that lands between the mix's reads, 0, or its adds and its store, 1) */
#define MENU_SOUND_BETWEEN(where) ((void)0)
#endif

void hos_menu_sound_mix(float *left, float *right, uint32_t frames, float gain) {
    if (atomic_load_explicit(&loaded, memory_order_acquire) != 2) return;
    for (int v = 0; v < VOICES; v++) {
        int64_t at = atomic_load_explicit(&voices[v].at, memory_order_acquire);
        if (at < 0) continue;
        MENU_SOUND_BETWEEN(0);
        const int which = atomic_load_explicit(&voices[v].sound, memory_order_acquire);
        const Sound *s = &sounds[which];
        /* hos_menu_sound_play may restart the voice between the two loads: the frame would then belong to the old sound,
           past the end of a shorter new one; such a voice is mixed from the next call on */
        if (atomic_load_explicit(&voices[v].at, memory_order_acquire) != at) continue;
        const uint32_t pos = AT_FRAME(at);
        if (pos >= s->frames) {   /* (past its sound's end, should that ever be: free again, not held for good) */
            atomic_compare_exchange_strong(&voices[v].at, &at, -1);
            continue;
        }
        const float vg = which == HOS_MENU_SOUND_CURSOR ? gain * CURSOR_GAIN : gain;
        const uint32_t n = s->frames - pos < frames ? s->frames - pos : frames;
        const float *src = s->pcm + (size_t)pos * 2;
        for (uint32_t i = 0; vg != 0.0f && i < n; i++) {
            left[i] += src[2 * (size_t)i] * vg;
            right[i] += src[2 * (size_t)i + 1] * vg;
        }
        MENU_SOUND_BETWEEN(1);
        /* Only advance if the voice was not restarted meanwhile. */
        atomic_compare_exchange_strong(&voices[v].at, &at, pos + n >= s->frames ? -1 : AT_START(at) | (int64_t)(pos + n));
    }
}

void hos_menu_sound_frame(float *left, float *right, uint32_t frames) {
    const int volume = rx_option(HOS_OPT_MENU_SOUND_VOLUME, -1);
    if (volume < 0) return;   /* (no renderer with the options: nothing asks for a sound) */
    int none = 0;
    if (volume > 0 && atomic_compare_exchange_strong(&loaded, &none, 1)) load_start();
    const int64_t now = hos_now_ns();
    /* (what was asked for since the last block: a press or two at most. With the row at 0 the options ask for none,
       and one asked for as it went to 0 is taken and not played) */
    for (int k = 0, id; k < 8 && (id = rx_option(HOS_OPT_MENU_SOUND, 0)) > 0; k++)
        if (volume > 0) hos_menu_sound_play(id, now);
    hos_menu_sound_mix(left, right, frames, hos_menu_sound_gain(volume));
}
