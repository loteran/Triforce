/* (hookshot) The button prompts of the controller in use: the game's button-icon font (Font2/Scalable/External.*.bfotf,
   every icon in a text: dialogue, the control hints, menus) served from prompts/<face>/External.bfotf in the game folder,
   which nx2/prompt_assets.py draws per controller face at build time, with each Switch button's icon pointed at the icon of
   the pad input bound to it (host/nn_hid.c's controls, the menu's Layout preset and Controls page). So the icons say which
   button to press on this pad, custom controls too.

   The face: triforce_options.ini's button_prompts (the menu's Button prompts row): 0 Auto, from the first controller's name
   (SDL), else 1 Switch, 2 Xbox, 3 PlayStation 4, 4 PlayStation 5, 5 Steam Deck, 6 Keyboard & mouse. The game reads the font
   once, at its start: a new face or new controls show at the next start.

   (Triforce) The keyboard and mouse face (NX2navi's prompts = pc; nx2/prompt_keys.py draws it): its font has an icon per
   key that can be bound, at U+F700 + the key's code, and each button's icon is that of the first key bound to it
   (bound_input). The left stick's and the d-pad's icons there are those keys side by side (bind_keys); the right stick's
   are the mouse. Auto chooses it when the game starts with no controller (auto_face).

   The font as built: an OpenType font behind an 8-byte header, XORed per little-endian word with the key the header's magic
   (0x18029a7f) gives. Its cmap has one icon per pad input at U+F700 + HOS_PAD_*, and each Switch button's code point in a
   format 4 segment of its own, by idDelta: pointing a button at another icon is that segment's idDelta. The result goes out
   as a zstd frame of raw blocks (the file name says .zs; the game's decoder takes a frame made without its dictionary). */
#include "hos.h"
#include "hos_paths.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../gpu/vk/vk_api.h"   /* (HOS_SW_*, HOS_PAD_*, HOS_BIND_PAD) */

int hos_binding(int set, int button, int slot);   /* nn_hid.c */
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]);   /* pad_sdl.c */
void hos_sdl_pad_first_name(char *out, size_t cap);
int hos_xinput_connected(void);   /* nn_hid.c */
int hos_mods_has(const char *prefix, const char *suffix);   /* mods.c */

#define FONT_PATH "font2/scalable/external.nin_ounce_nvn2.bfotf.zs"   /* (as hos_mods_romfs's keys: lower case) */
#define TEXTURES_PATH "ui/layout2archive/common.product.100.nin_ounce_nvn2.bntx.zs"
#define LAYOUTS_PATH "ui/layout2archive/common.product.100.nin_ounce_nvn2.blarc.zs"
#define SIZES_PATH "system/resource/resourcesizetable."   /* (then the version, ".rsizetable.zs") */
#define CAMERA_PATH "pack/actor/playercamera.pack.zs"   /* (Triforce: the Camera speed row, build_camera) */
#define MAGIC 0x18029a7fu
#define PAD_BASE 0xF700
static const char *const faces[HOS_PROMPT_N] = {"switch", "xbox", "ps4", "ps5", "steamdeck", "keyboard"};
/* the Switch buttons' code points in the font, in HOS_SW_* order (nx2/prompt_assets.py SWITCH) */
static const uint16_t switch_code[HOS_SW_N] = {0xE040, 0xE041, 0xE042, 0xE043, 0xE056, 0xE057, 0xE044, 0xE045,
                                               0xE046, 0xE047, 0xE04B, 0xE04C, 0xE062, 0xE060, 0xE063, 0xE061};

static int game_dir(char *out, size_t cap) {   /* the game folder, with its separator */
    char path[MAX_PATH];
    const DWORD len = hos_game_path_a(path, MAX_PATH);
    char *slash = len ? (strrchr(path, 92) ? strrchr(path, 92) : strrchr(path, 47)) : NULL;   /* (linux: '/') */
    if (!slash) return 0;
    slash[1] = 0;
    snprintf(out, cap, "%s", path);
    return 1;
}

static int contains(const char *text, const char *word) {   /* (any case) */
    for (; *text; text++) {
        size_t i = 0;
        while (word[i] && tolower((unsigned char)text[i]) == word[i]) i++;
        if (!word[i]) return 1;
    }
    return 0;
}

/* (Triforce) Auto's face: the first controller's, from its name (SDL's; "": it has none). With none the keyboard and mouse
   are what the player has: their keys' icons, where the Switch's were shown before. xinput: XInput has a controller that
   SDL does not name (HOS_NO_SDL=1, or no SDL beside the game): the game's own icons, as before.
   The game reads its icons once, at its start, so Auto is chosen once, then: it cannot follow the device used last during
   play, as NX2navi's prompts setting could not (a fixed choice, after a restart). A controller that is there at the start
   is the face even while the keys are used with it, so the two never take turns */
static int auto_face(const char *name, int xinput) {
    if (!name[0]) return xinput ? HOS_PROMPT_SWITCH : HOS_PROMPT_KEYBOARD;
    if (contains(name, "dualsense") || contains(name, "ps5")) return HOS_PROMPT_PS5;
    if (contains(name, "dualshock") || contains(name, "ps4") || contains(name, "playstation")) return HOS_PROMPT_PS4;
    if (contains(name, "steam deck")) return HOS_PROMPT_STEAMDECK;
    if (contains(name, "nintendo") || contains(name, "switch") || contains(name, "joy-con")) return HOS_PROMPT_SWITCH;
    return HOS_PROMPT_XBOX;   /* (Xbox pads, and the many that copy their layout) */
}

/* HOS_PROMPT_* (faces[]) */
static int choose_face(const char *dir) {
    char path[MAX_PATH], line[80];
    int choice = 0;
    snprintf(path, sizeof path, "%striforce_options.ini", dir);
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "button_prompts=", 15)) choice = atoi(line + 15);
        fclose(f);
    }
    if (choice >= 1 && choice <= HOS_PROMPT_N) return choice - 1;
    /* Auto: the first controller SDL opens (it scans every second: up to 1.5 s for one, at the game's start) */
    char name[128] = "";
    uint64_t b = 0;
    int32_t s[4] = {0};
    for (int i = 0; i < 30 && !name[0]; i++) {
        hos_sdl_pads(&b, s);
        hos_sdl_pad_first_name(name, sizeof name);
        if (!name[0]) Sleep(50);
    }
    const int xinput = !name[0] && hos_xinput_connected();
    const int face = auto_face(name, xinput);
    hos_log("prompts: auto: controller \"%s\": %s", name[0] ? name : xinput ? "(XInput's, no name)" : "(none)", faces[face]);
    return face;
}

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static unsigned rd16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wr16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }

/* the glyph of a code point in a format 4 subtable (st: its start, n its bytes), 0 none */
static unsigned cmap4_glyph(const uint8_t *st, size_t n, unsigned code) {
    const unsigned segs = rd16(st + 6) / 2;
    if (14 + 8 * (size_t)segs + 2 > n) return 0;
    const uint8_t *ends = st + 14, *starts = ends + 2 * segs + 2, *deltas = starts + 2 * segs, *ranges = deltas + 2 * segs;
    for (unsigned i = 0; i < segs; i++) {
        if (rd16(ends + 2 * i) < code) continue;
        if (rd16(starts + 2 * i) > code) return 0;
        const unsigned ro = rd16(ranges + 2 * i);
        if (!ro) return (code + rd16(deltas + 2 * i)) & 0xFFFF;
        const uint8_t *g = ranges + 2 * i + ro + 2 * (code - rd16(starts + 2 * i));
        if (g + 2 > st + n) return 0;
        const unsigned gid = rd16(g);
        return gid ? (gid + rd16(deltas + 2 * i)) & 0xFFFF : 0;
    }
    return 0;
}

/* (Triforce) What a control is on (control: a Switch button, HOS_SW_*, or a movement key's place, HOS_KEY_*), as the
   face's icons are numbered: a controller's input (HOS_PAD_*; -1 none, and for what is no button), or on the keyboard and
   mouse face the first of the control's keys (its key code; 0 none: the blank cap), as NX2navi's showed the first key */
static int bound_input(int face, int control) {
    if (face != HOS_PROMPT_KEYBOARD) return hos_binding(HOS_BIND_PAD, control, 0);
    const int first = hos_binding(HOS_BIND_KEY, control, 0);
    return first > 0 ? first : hos_binding(HOS_BIND_KEY, control, 1);
}

/* each Switch button's segment pointed at its input's icon (bound_input); how many. (Triforce) On the keyboard and mouse
   face the left stick's four directions too, at their movement keys' icons (nx2/prompt_keys.py MOVES), and a key that has
   no icon (one a hand put into the options file) shows the blank cap */
static int cmap4_bind(uint8_t *st, size_t n, int face) {
    const unsigned segs = rd16(st + 6) / 2;
    if (14 + 8 * (size_t)segs + 2 > n) return 0;   /* (as cmap4_glyph: the ranges' last entry too) */
    uint8_t *ends = st + 14, *starts = ends + 2 * segs + 2, *deltas = starts + 2 * segs, *ranges = deltas + 2 * segs;
    static const struct { uint16_t code; uint8_t control; } moves[4] = {
        {0xE052, HOS_KEY_MOVE_UP}, {0xE050, HOS_KEY_MOVE_DOWN}, {0xE05A, HOS_KEY_MOVE_LEFT}, {0xE058, HOS_KEY_MOVE_RIGHT}};
    const int keys = face == HOS_PROMPT_KEYBOARD, inputs = keys ? 256 : HOS_PAD_N;
    int done = 0;
    for (int b = 0; b < HOS_SW_N + (keys ? 4 : 0); b++) {
        const unsigned code = b < HOS_SW_N ? switch_code[b] : moves[b - HOS_SW_N].code;
        const int pad = bound_input(face, b < HOS_SW_N ? b : moves[b - HOS_SW_N].control);
        if (pad < 0 || pad >= inputs) continue;   /* (unbound: the default icon stays) */
        unsigned gid = cmap4_glyph(st, n, PAD_BASE + (unsigned)pad);
        if (!gid && keys) gid = cmap4_glyph(st, n, PAD_BASE);
        if (!gid) continue;
        for (unsigned i = 0; i < segs; i++)
            if (rd16(starts + 2 * i) == code && rd16(ends + 2 * i) == code && !rd16(ranges + 2 * i)) {
                wr16(deltas + 2 * i, (gid - code) & 0xFFFF);
                done++;
                break;
            }
    }
    return done;
}

/* (Triforce) The keyboard and mouse face's icons of several keys (the left stick: the movement keys; the d-pad: its four
   keys). Each such glyph calls a subroutine per key, that key's small cap, and prompts/keyboard/keys.bin (t; nx2/
   prompt_keys.py: "HPKB", u32 count; each: u32 offset in the font, u8 control (HOS_KEY_*), 3 bytes; then u32 keys, and for
   each key code the two bytes of its cap's call, 0 0 none) says where those calls are: each gets the call of the first key
   bound to its control (none bound, or a key with no cap: the blank cap's, key code 0's). A call is two bytes whatever the
   key (a charstring's number from 108 to 1131: its first byte 247 to 250), so nothing else in the font moves; a place or a
   call that is not such a number is left alone. How many were written */
static int bind_keys(uint8_t *otf, size_t n, const uint8_t *t, size_t tn) {
    if (tn < 12) return 0;
    const uint32_t count = rd32le(t + 4);
    if (count > (tn - 12) / 8) return 0;
    const uint8_t *calls = t + 12 + 8 * (size_t)count;
    const uint32_t keys = rd32le(calls - 4);
    if (!keys || keys > (tn - (size_t)(calls - t)) / 2) return 0;
    int done = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = t + 8 + 8 * (size_t)i;
        const uint32_t off = rd32le(e);
        const int key = bound_input(HOS_PROMPT_KEYBOARD, e[4]);
        const uint8_t *call = calls + (key > 0 && (uint32_t)key < keys && calls[2 * (size_t)key] ? 2 * (size_t)key : 0);
        if (off > n || n - off < 2 || otf[off] < 247 || otf[off] > 250 || call[0] < 247 || call[0] > 250) continue;
        otf[off] = call[0], otf[off + 1] = call[1];
        done++;
    }
    return done;
}

/* the controls into the decrypted font's format 4 subtables; how many buttons */
static int bind_font(uint8_t *otf, size_t n, int face) {
    if (n < 12) return 0;
    const unsigned tables = rd16(otf + 4);
    for (unsigned t = 0; t < tables && 12 + 16 * (size_t)(t + 1) <= n; t++) {
        const uint8_t *rec = otf + 12 + 16 * t;
        if (memcmp(rec, "cmap", 4)) continue;
        const uint32_t off = rd32(rec + 8), len = rd32(rec + 12);
        if (off > n || len > n - off || len < 4) return 0;
        uint8_t *cmap = otf + off;
        const unsigned subs = rd16(cmap + 2);
        uint32_t seen[8];
        int nseen = 0, done = 0;
        for (unsigned s = 0; s < subs && 4 + 8 * (size_t)(s + 1) <= len; s++) {
            const uint32_t so = rd32(cmap + 4 + 8 * s + 4);
            if (so > len || len - so < 14 || rd16(cmap + so) != 4) continue;   /* (so + 14 wrapped past 4 GB) */
            int dup = 0;   /* (platforms 0 and 3 share one subtable) */
            for (int k = 0; k < nseen; k++) dup |= seen[k] == so;
            if (dup) continue;
            if (nseen < 8) seen[nseen++] = so;
            const unsigned sl = rd16(cmap + so + 2);
            done = cmap4_bind(cmap + so, sl <= len - so ? sl : len - so, face);
        }
        return done;
    }
    return 0;
}

/* a zstd frame's header for raw blocks of 128 KB: the 4-byte content size, and a window of 128 KB (raw blocks never look back:
   a single-segment frame's window, the whole content, is more than decoders take: zstd's own refuses 244 MB) */
static void zstd_head(uint8_t head[10], size_t n) {
    wr32le(head, 0xFD2FB528u);
    head[4] = 0x80;   /* (FCS 4 bytes, not single segment, no checksum, no dictionary) */
    head[5] = 7 << 3;   /* (window 2^(10+7)) */
    wr32le(head + 6, (uint32_t)n);
}

/* one raw block of a frame (zstd_head): k bytes (at most 128 KB), last for the frame's last */
static int zstd_block(FILE *o, const uint8_t *data, size_t k, int last) {
    const uint32_t bh = (uint32_t)(k << 3) | (last ? 1u : 0u);
    const uint8_t b3[3] = {(uint8_t)bh, (uint8_t)(bh >> 8), (uint8_t)(bh >> 16)};
    return fwrite(b3, 1, 3, o) == 3 && fwrite(data, 1, k, o) == k;
}

/* data as a zstd frame of raw blocks (zstd_head) */
static int write_zstd_raw(const char *path, const uint8_t *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint8_t head[10];
    zstd_head(head, n);
    int ok = fwrite(head, 1, 10, f) == 10;
    size_t at = 0;
    do {
        const size_t k = n - at < 131072 ? n - at : 131072;
        ok &= zstd_block(f, data + at, k, at + k == n);
        at += k;
    } while (at < n);
    ok &= fclose(f) == 0;
    return ok;
}

static uint8_t *read_all(const char *path, const char *magic, long *n) {   /* (the whole file, if it starts with magic: NULL any) */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *t = *n > 8 ? (uint8_t *)malloc((size_t)*n) : NULL;
    const int got = t && fread(t, 1, (size_t)*n, f) == (size_t)*n;
    fclose(f);
    if (!got || (magic && memcmp(t, magic, 4))) {
        free(t);
        return NULL;
    }
    return t;
}

static volatile LONG setup_state;   /* setup's: 0 not looked, 1 looking, 2 prompts, 3 none */

/* the game folder and the face, found once; 0 when the folder has no prompts/ (built before them: the game's own icons) */
static int setup(char *dir, size_t cap, int *face) {
    static char d[MAX_PATH];
    static int f;
    if (InterlockedCompareExchange(&setup_state, 1, 0) == 0) {
        char path[MAX_PATH];
        int ok = game_dir(d, sizeof d);
        if (ok) {
            snprintf(path, sizeof path, "%sprompts", d);
            ok = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        }
        if (ok) f = choose_face(d);
        /* (v0.3.5 shipped tools whose Python lacked nx2/prompt_requirements.txt: no folder was made, the Button prompts
           setting did nothing, and the log said nothing of it) */
        else hos_log("prompts: no prompts folder beside the game: the game's own icons, whatever Button prompts is set to "
                     "(Setup's Update makes them)");
        InterlockedExchange(&setup_state, ok ? 2 : 3);
    }
    while (setup_state == 1) Sleep(1);
    snprintf(dir, cap, "%s", d);
    *face = f;
    return setup_state == 2;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 0x100000001b3ull;
    return h;
}

typedef struct { uint32_t off, len, idx; const uint8_t *data; } Run;   /* (idx: its place in textures.bin) */
static int run_cmp(const void *a, const void *b) {
    const Run *x = (const Run *)a, *y = (const Run *)b;
    return x->off != y->off ? (x->off < y->off ? -1 : 1) : (x->idx > y->idx) - (x->idx < y->idx);
}
static int run_order(const void *a, const void *b) {
    const uint32_t x = (*(const Run *const *)a)->idx, y = (*(const Run *const *)b)->idx;
    return (x > y) - (x < y);
}

static void add_run(Run **runs, size_t *nruns, size_t *cap, uint32_t off, uint32_t len, const uint8_t *data) {
    if (*nruns == *cap) *runs = (Run *)realloc(*runs, (*cap *= 2) * sizeof **runs);
    (*runs)[*nruns] = (Run){off, len, (uint32_t)*nruns, data};
    (*nruns)++;
}

/* prompts/Common.bntx.zs: the game's textures (prompts/Common.bntx) with the blocks of prompts/<face>/textures.bin that this
   pad's controls choose (nx2/prompt_assets.py: "HPTX", u32 count; each: u8 Switch button (0xff: always), u8 pad input, u16,
   u32 runs; each run: u32 offset, u32 length, the bytes; a later entry over an earlier). Written again only when the face, the
   controls or the blocks changed (prompts/Common.key) */
/* out: the game's file (game) with the runs written over it (a later one over an earlier where they meet), as raw zstd blocks;
   written again only when its key (keypath: the hash h of what chose the runs) changed */
static int write_patched(const char *game, const char *out, const char *keypath, uint64_t h, Run *runs, size_t nruns) {
    qsort(runs, nruns, sizeof *runs, run_cmp);
    char keyline[32] = "", want[32];
    FILE *f;
    if ((f = fopen(keypath, "r"))) {
        if (!fgets(keyline, sizeof keyline, f)) keyline[0] = 0;
        fclose(f);
    }
    snprintf(want, sizeof want, "%016llx", (unsigned long long)h);
    int ok = !strcmp(keyline, want) && GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
    if (!ok) {   /* the game's file through, the chosen runs written over it, out as raw zstd blocks */
        FILE *in = fopen(game, "rb"), *o = fopen(out, "wb");
        uint8_t *chunk = (uint8_t *)malloc(131072);
        ok = in && o && chunk;
        if (ok) {
            fseek(in, 0, SEEK_END);
            const uint64_t n = (uint64_t)ftell(in);
            fseek(in, 0, SEEK_SET);
            uint8_t head[10];
            zstd_head(head, n);
            ok = n < 0xFFFFFFFFull && fwrite(head, 1, 10, o) == 10;
            size_t first = 0;   /* (the first run that may reach this chunk) */
            const Run **here = (const Run **)malloc((nruns ? nruns : 1) * sizeof *here);
            ok = ok && here;
            for (uint64_t pos = 0; ok && pos < n;) {
                const size_t k = n - pos < 131072 ? (size_t)(n - pos) : 131072;
                ok = fread(chunk, 1, k, in) == k;
                while (first < nruns && runs[first].off + runs[first].len <= pos) first++;
                size_t nh = 0;   /* (the runs in this chunk, written in their order: a later one over an earlier) */
                for (size_t r = first; r < nruns && runs[r].off < pos + k; r++)
                    if (runs[r].off + runs[r].len > pos) here[nh++] = &runs[r];
                qsort(here, nh, sizeof *here, run_order);
                for (size_t i = 0; i < nh; i++) {
                    const Run *r = here[i];
                    const uint64_t a = r->off > pos ? r->off : pos;
                    const uint64_t z = r->off + r->len < pos + k ? r->off + r->len : pos + k;
                    memcpy(chunk + (a - pos), r->data + (a - r->off), (size_t)(z - a));
                }
                ok = ok && zstd_block(o, chunk, k, pos + k == n);
                pos += k;
            }
            free(here);
        }
        if (in) fclose(in);
        if (o) ok &= fclose(o) == 0;
        free(chunk);
        if (ok && (f = fopen(keypath, "w"))) {
            fputs(want, f);
            fclose(f);
        }
    }
    return ok;
}

static int build_textures(char *out, size_t cap) {
    char dir[MAX_PATH], path[MAX_PATH];
    int face;
    if (!setup(dir, sizeof dir, &face)) return 0;
    snprintf(path, sizeof path, "%sprompts/%s/textures.bin", dir, faces[face]);
    long tn;
    uint8_t *t = read_all(path, "HPTX", &tn);
    if (!t) return 0;   /* (built without the textures: the game's) */
    int bind[HOS_SW_N];   /* ((Triforce) on the keyboard and mouse face the entries' inputs are key codes: bound_input) */
    for (int b = 0; b < HOS_SW_N; b++) bind[b] = bound_input(face, b);
    uint64_t h = fnv(fnv(fnv(0xcbf29ce484222325ull, &face, sizeof face), bind, sizeof bind), t, (size_t)tn);
    /* the runs these controls choose */
    const uint32_t count = rd32le(t + 4);
    size_t at = 8, nruns = 0, cap_runs = 256;
    Run *runs = (Run *)malloc(cap_runs * sizeof *runs);
    for (uint32_t e = 0; e < count && at + 8 <= (size_t)tn; e++) {
        const int sw = t[at], pad = t[at + 1];
        const uint32_t nr = rd32le(t + at + 4);
        const int on = sw == 0xff || (sw < HOS_SW_N && bind[sw] == pad);
        at += 8;
        for (uint32_t r = 0; r < nr && at + 8 <= (size_t)tn; r++) {
            const uint32_t off = rd32le(t + at), len = rd32le(t + at + 4);
            if (len > (size_t)tn - at - 8) break;
            if (on) add_run(&runs, &nruns, &cap_runs, off, len, t + at + 8);
            at += 8 + len;
        }
    }
    snprintf(out, cap, "%sprompts/Common.bntx.zs", dir);
    snprintf(path, sizeof path, "%sprompts/Common.bntx", dir);
    char keypath[MAX_PATH];
    snprintf(keypath, sizeof keypath, "%sprompts/Common.key", dir);
    const int ok = write_patched(path, out, keypath, h, runs, nruns);
    hos_log("prompts: %s textures, %zu runs for these controls%s", faces[face], nruns,
            ok ? "" : ": could not be written");
    free(runs);
    free(t);
    return ok;
}

/* the face the game was given (faces[]), -1 none: the Insert menu names the pad inputs as that controller does. -1 too
   until the game has asked for a prompt file: the menu asks from the present thread, and choose_face can wait for SDL */
int hos_prompts_face(void) {
    char dir[MAX_PATH];
    int face;
    return setup_state >= 2 && setup(dir, sizeof dir, &face) ? face : -1;
}

/* the Talk ring's place for each of A B X Y (0 east, 1 south, 2 north, 3 west: nx2/prompt_assets.py RING_PLACES): where its pad
   input is among the face buttons, else its own place on the Switch when free, else the first one free */
static void ring_places(const int *bind, int place[4]) {
    static const int of_pad[4] = {1, 0, 3, 2};   /* (HOS_PAD_SOUTH, EAST, WEST, NORTH) */
    int taken[4] = {0};
    for (int s = 0; s < 4; s++) {
        const int p = bind[s] >= 0 && bind[s] < 4 ? of_pad[bind[s]] : -1;
        place[s] = p >= 0 && !taken[p] ? p : -1;
        if (place[s] >= 0) taken[place[s]] = 1;
    }
    for (int s = 0; s < 4; s++) {
        if (place[s] >= 0) continue;
        int p = s;   /* (A B X Y's own places are 0 1 2 3) */
        if (taken[p])
            for (p = 0; taken[p]; p++) {}
        place[s] = p;
        taken[p] = 1;
    }
}

/* prompts/Common.blarc.zs: the game's layouts (prompts/Common.blarc) with
   - the Talk ring's A B X Y where their pad inputs are (prompts/ring.bin, nx2/prompt_assets.py: "HPRG", u32 count; each: u8
     group, u8 Switch button placed, u8 place, u8, u32 runs, the runs);
   - the ocarina hint's icons moved apart for the wide ones bound (prompts/<face>/hint.bin, nx2/prompt_assets.py HINTS: "HPHN",
     u32 count of pad inputs, each one's icon's extra width (f32), u32 panes; each: u32 offset, u8 slot, 3 bytes, f32 x): each
     icon the one before's extra further on, the last one where it is;
   - the hearts, the item slot and the small map out at a wide picture's sides (prompts/wide.bin, nx2/prompt_assets.py HUD_SIDE_PANES: "HPWD", u32
     count; each: u32 offset, i8 side, 3 bytes, f32 x), with the Insert menu's Ultrawide HUD row on Screen edges
     (host/oot_wide_hud.h);
   none when all are as the game has them (the game's own file) */
static int build_layouts(char *out, size_t cap) {
    char dir[MAX_PATH], path[MAX_PATH];
    int face;
    if (!setup(dir, sizeof dir, &face)) return 0;
    int bind[HOS_SW_N], place[4] = {0, 1, 2, 3};
    for (int b = 0; b < HOS_SW_N; b++) bind[b] = bound_input(face, b);
    /* ((Triforce) keys have no places among four face buttons: the keyboard and mouse face keeps the Switch's) */
    if (face != HOS_PROMPT_KEYBOARD) ring_places(bind, place);
    const int ring_moved = !(place[0] == 0 && place[1] == 1 && place[2] == 2 && place[3] == 3);
    size_t nruns = 0, cap_runs = 64;
    Run *runs = (Run *)malloc(cap_runs * sizeof *runs);
    uint64_t h = fnv(0xcbf29ce484222325ull, place, sizeof place);
    long tn = 0, hn = 0;
    snprintf(path, sizeof path, "%sprompts/ring.bin", dir);
    uint8_t *t = ring_moved ? read_all(path, "HPRG", &tn) : NULL;
    if (t) {
        const uint32_t count = rd32le(t + 4);
        size_t at = 8;
        for (uint32_t e = 0; e < count && at + 8 <= (size_t)tn; e++) {
            const int placed = t[at + 1], where = t[at + 2];
            const uint32_t nr = rd32le(t + at + 4);
            const int on = placed < 4 && place[placed] == where;
            at += 8;
            for (uint32_t r = 0; r < nr && at + 8 <= (size_t)tn; r++) {
                const uint32_t off = rd32le(t + at), len = rd32le(t + at + 4);
                if (len > (size_t)tn - at - 8) break;
                if (on) add_run(&runs, &nruns, &cap_runs, off, len, t + at + 8);
                at += 8 + len;
            }
        }
        h = fnv(h, t, (size_t)tn);
    }
    /* the hint's notes, in its order (Switch L R Y X A: up, down, left, right, A) */
    static const int notes[5] = {HOS_SW_L, HOS_SW_R, HOS_SW_Y, HOS_SW_X, HOS_SW_A};
    snprintf(path, sizeof path, "%sprompts/%s/hint.bin", dir, faces[face]);
    uint8_t *hint = read_all(path, "HPHN", &hn);
    float *xs = NULL;
    int spread = 0;
    if (hint) {
        const uint32_t npads = rd32le(hint + 4);
        const size_t panes_at = 8 + 4 * (size_t)npads;
        float extra[5], shift[6] = {0};
        for (int i = 0; i < 5; i++) {
            const int p = bind[notes[i]];
            extra[i] = 0;
            if (p >= 0 && (uint32_t)p < npads && panes_at <= (size_t)hn) memcpy(&extra[i], hint + 8 + 4 * p, 4);
        }
        for (int i = 3; i >= 0; i--) shift[i] = shift[i + 1] - extra[i];   /* (an icon is drawn from its pane's left) */
        shift[5] = shift[0];   /* (the text with the first icon) */
        spread = shift[0] != 0;
        const uint32_t count = panes_at + 4 <= (size_t)hn ? rd32le(hint + panes_at) : 0;
        if (spread && count <= ((size_t)hn - panes_at - 4) / 12) {
            xs = (float *)malloc(count * sizeof *xs);
            for (uint32_t k = 0; k < count; k++) {
                const uint8_t *e = hint + panes_at + 4 + 12 * k;
                memcpy(&xs[k], e + 8, 4);
                xs[k] += shift[e[4] < 6 ? e[4] : 5];
                add_run(&runs, &nruns, &cap_runs, rd32le(e), 4, (const uint8_t *)&xs[k]);
            }
            h = fnv(fnv(h, shift, sizeof shift), hint, (size_t)hn);
        }
    }
    float hos_wide_hud_out(void);
    const float wide_out = hos_wide_hud_out();
    long wn = 0;
    snprintf(path, sizeof path, "%sprompts/wide.bin", dir);
    uint8_t *wide = wide_out > 0.0f ? read_all(path, "HPWD", &wn) : NULL;
    float *wxs = NULL;
    if (wide) {
        const uint32_t count = rd32le(wide + 4);
        if (count && count <= ((size_t)wn - 8) / 12) {
            wxs = (float *)malloc(count * sizeof *wxs);
            for (uint32_t k = 0; k < count; k++) {
                const uint8_t *e = wide + 8 + 12 * k;
                memcpy(&wxs[k], e + 8, 4);
                wxs[k] += (float)(int8_t)e[4] * wide_out;
                add_run(&runs, &nruns, &cap_runs, rd32le(e), 4, (const uint8_t *)&wxs[k]);
            }
            h = fnv(fnv(h, &wide_out, sizeof wide_out), wide, (size_t)wn);
        }
    } else if (wide_out > 0.0f) {
        hos_log("prompts: no prompts/wide.bin: the Ultrawide HUD stays in the middle (Setup's Update makes it)");
    }
    int ok = 0;
    if (nruns) {
        char keypath[MAX_PATH];
        snprintf(out, cap, "%sprompts/Common.blarc.zs", dir);
        snprintf(path, sizeof path, "%sprompts/Common.blarc", dir);
        snprintf(keypath, sizeof keypath, "%sprompts/Common.blarc.key", dir);
        ok = write_patched(path, out, keypath, h, runs, nruns);
        hos_log("prompts: the Talk ring's A B X Y at places %d %d %d %d, the ocarina hint %s, the hearts, item slot and map %s%s",
                place[0], place[1], place[2], place[3], xs ? "spread" : "as is", wxs ? "at the picture's sides" : "as is",
                ok ? "" : ": could not be written");
    }
    free(wxs);
    free(wide);
    free(xs);
    free(hint);
    free(runs);
    free(t);
    return ok;
}

/* builds prompts/External.bfotf.zs for this run; 1 when there is one */
static int build(char *out, size_t cap) {
    char dir[MAX_PATH], path[MAX_PATH];
    int face;
    if (!setup(dir, sizeof dir, &face)) return 0;
    /* (a mod's size table is served, not ours: the faces' fonts, larger than the game's, do not fit it. ((Triforce, 11-18)
       unless the lifted code asks host/mods.c's hos_mods_table_size for each file's room, which gives the font its own: so
       only when that hook has not run in this game, an older build of it) */
    extern volatile LONG hos_mods_size_hook_seen;
    if (hos_mods_has(SIZES_PATH, ".rsizetable.zs")) {
        if (!hos_mods_size_hook_seen) {
            hos_log("prompts: a mod has the resource size table: the game's own icons in texts");
            return 0;
        }
        hos_log("prompts: a mod has the resource size table: the font's room comes from the size hook (host/mods.c)");
    }
    snprintf(path, sizeof path, "%sprompts/%s/External.bfotf", dir, faces[face]);
    long n;
    uint8_t *b = read_all(path, NULL, &n);
    if (!b || n < 16 || n % 4) {
        hos_log("prompts: no %s: the game's own icons", path);
        free(b);
        return 0;
    }
    const uint32_t key = rd32le(b) ^ MAGIC;
    for (long i = 8; i < n; i += 4) wr32le(b + i, rd32le(b + i) ^ key);
    const int bound = bind_font(b + 8, (size_t)n - 8, face);
    int several = 0;
    if (face == HOS_PROMPT_KEYBOARD) {   /* ((Triforce) the icons of several keys: bind_keys) */
        long kn = 0;
        snprintf(path, sizeof path, "%sprompts/%s/keys.bin", dir, faces[face]);
        uint8_t *k = read_all(path, "HPKB", &kn);
        if (k) several = bind_keys(b + 8, (size_t)n - 8, k, (size_t)kn);
        free(k);
    }
    for (long i = 8; i < n; i += 4) wr32le(b + i, rd32le(b + i) ^ key);
    snprintf(out, cap, "%sprompts/External.bfotf.zs", dir);
    const int ok = write_zstd_raw(out, b, (size_t)n);
    free(b);
    if (face == HOS_PROMPT_KEYBOARD)
        hos_log("prompts: %s icons, %d buttons and directions on these keys, %d keys in the icons of several%s", faces[face],
                bound, several, ok ? "" : ": could not be written");
    else
        hos_log("prompts: %s icons, %d buttons on this pad's controls%s", faces[face], bound, ok ? "" : ": could not be written");
    return ok;
}

/* (Triforce) The Insert menu's Camera speed row (camera_speed= in the options file, percent; HOS_CAMERA_SPEED=<percent>
   over it): how fast the right stick turns the camera, times the game's own speed at any of the five Camera sensitivity
   settings of its Options. The rates are five floats in the game's camera pack (nx2/prompt_assets.py CAMERA has where and
   why): prompts/PlayerCamera.pack.zs is that pack (prompts/PlayerCamera.pack) with each rate of prompts/camera.bin ("HPCM",
   u32 count; each: u32 offset, f32 the game's rate) times the factor. Read once: the game loads the pack at its start, so
   the row applies after a restart. At 100 the game's own file is served. NX2navi's camera speed setting, without its
   search of the game's memory for a change during play. */
int hos_ini_option(const char *key, int otherwise);
static volatile double camera_applied = 1.0;   /* the factor in the pack the game was given */
static double camera_wanted(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("HOS_CAMERA_SPEED");
        v = e && atoi(e) > 0 ? atoi(e) : hos_ini_option("camera_speed", 100);
        v = v < 25 ? 25 : v > 300 ? 300 : v;
    }
    return v / 100.0;
}
/* host/nn_hid.c: the mouse's panning reaches the game as the right stick; divided by this it keeps its own sensitivity */
double hos_camera_speed(void) {
    return camera_applied;
}
static int build_camera(char *out, size_t cap) {
    char dir[MAX_PATH], path[MAX_PATH], keypath[MAX_PATH];
    int face;
    const double k = camera_wanted();
    if (k == 1.0 || !setup(dir, sizeof dir, &face)) return 0;
    long tn = 0;
    snprintf(path, sizeof path, "%sprompts/camera.bin", dir);
    uint8_t *t = read_all(path, "HPCM", &tn);
    const uint32_t count = t ? rd32le(t + 4) : 0;
    if (!count || count > 8 || (size_t)tn < 8 + 8 * (size_t)count) {
        hos_log("prompts: no prompts/camera.bin: the camera keeps the game's speed (Setup's Update makes it)");
        free(t);
        return 0;
    }
    float rates[8];
    size_t nruns = 0, cap_runs = 8;
    Run *runs = (Run *)malloc(cap_runs * sizeof *runs);
    for (uint32_t i = 0; i < count; i++) {
        memcpy(&rates[i], t + 8 + 8 * i + 4, 4);
        rates[i] = (float)(rates[i] * k);
        add_run(&runs, &nruns, &cap_runs, rd32le(t + 8 + 8 * i), 4, (const uint8_t *)&rates[i]);
    }
    const uint64_t h = fnv(fnv(0xcbf29ce484222325ull, &k, sizeof k), t, (size_t)tn);
    snprintf(out, cap, "%sprompts/PlayerCamera.pack.zs", dir);
    snprintf(path, sizeof path, "%sprompts/PlayerCamera.pack", dir);
    snprintf(keypath, sizeof keypath, "%sprompts/PlayerCamera.key", dir);
    const int ok = write_patched(path, out, keypath, h, runs, nruns);
    if (ok) camera_applied = k;
    hos_log("prompts: camera speed x%.2f, the first rate %.2f%s", k, (double)rates[0], ok ? "" : ": could not be written");
    free(runs);
    free(t);
    return ok;
}

int hos_prompts_romfs(const char *key, char *out, size_t cap);
/* ((Triforce, 11-18) mods.c's size hook: for the font's key (lower case, '/', packed name), the font built for this run
   (built now if it was not), 1 with one; 0 for any other key or with no prompts. A mod's own font is the mod's: the hook
   finds that one first) */
int hos_prompts_font_file(const char *key, char *out, size_t cap) {
    if (strcmp(key, FONT_PATH)) return 0;
    return hos_prompts_romfs(FONT_PATH, out, cap);
}

/* mods.c: the file for a RomFS path the prompts replace (key: lower case, '/'), 1 with one. Asked only when no mod has
   the file: a mod's own wins */
int hos_prompts_romfs(const char *key, char *out, size_t cap) {
    static volatile LONG state[4];   /* the font, the textures, the layouts, the camera: 0 not built, 1 being built, 2 built, 3 none */
    static char path[4][MAX_PATH];
    const size_t kn = strlen(key);
    if (!strncmp(key, SIZES_PATH, sizeof SIZES_PATH - 1) && kn > 14 && !strcmp(key + kn - 14, ".rsizetable.zs")) {
        char dir[MAX_PATH];   /* (the game's size table with room for the faces' fonts, larger than its: nx2/prompt_assets.py) */
        int face;
        if (!setup(dir, sizeof dir, &face)) return 0;
        snprintf(out, cap, "%sprompts/rsizetable.zs", dir);
        return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
    }
    const int which = !strcmp(key, FONT_PATH) ? 0 : !strcmp(key, TEXTURES_PATH) ? 1 : !strcmp(key, LAYOUTS_PATH) ? 2 :
                      !strcmp(key, CAMERA_PATH) ? 3 : -1;
    if (which < 0) return 0;
    static int (*const builders[4])(char *, size_t) = {build, build_textures, build_layouts, build_camera};
    if (InterlockedCompareExchange(&state[which], 1, 0) == 0)
        InterlockedExchange(&state[which], builders[which](path[which], MAX_PATH) ? 2 : 3);
    while (state[which] == 1) Sleep(1);
    if (state[which] != 2) return 0;
    snprintf(out, cap, "%s", path[which]);
    return 1;
}
