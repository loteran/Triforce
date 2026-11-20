/* (Triforce) The cheats of the mods' cheat files, after NX2navi's Cheats tab: read at the start, switched on the System
   tab, run once a game frame.

   host/mods.c hands over each mod's file for this build of the game, mods\<name>\cheats\<build id>.txt (the first 8
   bytes of main's build id, as Atmosphere and the emulators name it), in the mods' order. host/mod_cheats.h reads it and
   has the interpreter and what the format is; this file has what is the game's: its memory, the buttons, the rows and
   which cheats are on.

   - The memory a cheat reads and writes is the game's program image and its heap, each access checked against them
     (mod_cheat_readable); a write into a module's code is dropped, as the code is compiled ahead of time. A cheat that
     stores into the code outright (opcode 0 into main's .text) is not run at all and says so on its row: NX2navi built
     such cheats into the game, which this translator cannot do (a code patch can: nx2/code_mods.py).
     The regions a cheat names are where the game is told they are (host/hos_svc.c GetInfo): main at the module main's
     base (gen/hos_image.c), the heap and the alias region at HOS_HEAP_BASE, the address space at 0x08000000.
   - A cheat is off until its row switches it on, and one that was switched on stays on at the next start, as NX2navi
     kept them: triforce_cheats.txt in the game folder has a line for each cheat that is on, the FNV-1a 32 of
     "<mod>/<name>" (a name's second cheat in a file: "<name>#2") and that text. A master code ({name}) always runs,
     before the file's other cheats.
   - The rows are the menu's (gpu/vk/mod_cheat_rows.h): at the first frame the renderer answers, the cheats are named to
     it (HOS_OPT_MODCHEAT_TEXT, _ADD, _COUNT), and every frame it is asked what the player switched (HOS_OPT_MODCHEAT_ASK).
     Without that renderer the cheats that are on still run.
   - hos_mod_cheats_frame is called at each present of the game's (host/nvn_native.c), on the thread that presents: the
     game's other threads run meanwhile, as they do under a cheat on the console. */
#include "hos.h"
#include "hos_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gpu/vk/vk_api.h"
#include "mod_cheats.h"

int rx_option(int id, int otherwise);
void rx_option_set(int id, int value);
extern volatile uint64_t hos_npad_buttons;   /* nn_hid.c: the buttons the game was last given */

enum { MAX_CHEATS = 512, MAX_FILES = 32, MAX_ROWS = 96 /* (mod_cheat_rows.h kSlots) */, FILE_MOST = 1 << 20,
       LONG_NAME = 28 /* (bytes of a name a row surely shows beside its value) */ };
typedef enum { KIND_HEADING, KIND_SWITCH, KIND_MASTER, KIND_REFUSED } Kind;
typedef struct {
    ModCheat c;
    Kind kind;
    int file, on, row;                        /* row: its row on the System tab, -1 none */
    int same;                                 /* 1, or which of the file's cheats of this name it is */
    uint32_t key;                             /* FNV-1a 32 of "<mod>/<name>", "<mod>/<name>#<same>" from the second on */
    int said_read, said_drop, said_steps;     /* (each said once in the log) */
} Cheat;
typedef struct {
    char mod[160];
    ModCheatState state;
    int first, n;
} File;
static Cheat cheats[MAX_CHEATS];
static File files[MAX_FILES];
static int ncheats, nfiles, nrows, running;   /* running: cheats that run each frame (masters and the ones on) */
static int row_cheat[MAX_ROWS];

static const char *last_sep(const char *s) {
    const char *b = strrchr(s, '\\'), *f = strrchr(s, '/');
    return b > f ? b : f;
}
/* a file of the game folder */
static int game_file(char *out, size_t cap, const char *name) {
    char exe[MAX_PATH];
    const DWORD n = hos_game_path_a(exe, sizeof exe);
    const char *slash = n && n < sizeof exe ? last_sep(exe) : NULL;
    if (!slash) return 0;
    return snprintf(out, cap, "%.*s%s", (int)(slash + 1 - exe), exe, name) < (int)cap;
}

/* ---- which cheats are on: triforce_cheats.txt ---- */
#define ON_FILE "triforce_cheats.txt"
static void on_load(void) {
    char path[MAX_PATH + 32], line[512];
    if (!game_file(path, sizeof path, ON_FILE)) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *end = NULL;
        const unsigned long key = strtoul(line, &end, 16);
        if (line[0] == '#' || end != line + 8) continue;
        for (int i = 0; i < ncheats; i++)
            if (cheats[i].kind == KIND_SWITCH && cheats[i].key == (uint32_t)key) cheats[i].on = 1;
    }
    fclose(f);
}
static void on_save(void) {
    char path[MAX_PATH + 32], fresh[MAX_PATH + 40];
    if (!game_file(path, sizeof path, ON_FILE)) return;
    snprintf(fresh, sizeof fresh, "%s.new", path);
    FILE *f = fopen(fresh, "wb");   /* (its lines end the same on Windows and Linux) */
    if (!f) {
        hos_log("cheats: %s could not be written: the switches are as they are until the game ends", path);
        return;
    }
    fputs("# The mods' cheats that are switched on (the System tab's rows), a line each: kept for the next start.\n", f);
    for (int i = 0; i < ncheats; i++) {
        const Cheat *c = &cheats[i];
        if (c->kind != KIND_SWITCH || !c->on) continue;
        if (c->same > 1) fprintf(f, "%08x %s/%s#%d\n", c->key, files[c->file].mod, c->c.name, c->same);
        else fprintf(f, "%08x %s/%s\n", c->key, files[c->file].mod, c->c.name);
    }
    const int ok = !ferror(f);
    if (fclose(f) || !ok || !MoveFileExA(fresh, path, MOVEFILE_REPLACE_EXISTING)) {
        remove(fresh);
        hos_log("cheats: %s could not be written: the switches are as they are until the game ends", path);
    }
}

static void count_running(void) {
    running = 0;
    for (int i = 0; i < ncheats; i++) running += cheats[i].kind == KIND_MASTER || (cheats[i].kind == KIND_SWITCH && cheats[i].on);
}

/* ---- the files (host/mods.c, at the start, before the game's threads) ---- */
static ModCheatLayout game_layout(void) {
    extern const uint64_t hos_code[][2];   /* gen/hos_image.c: each module's .text */
    extern const int hos_code_count;
    const ModCheatLayout l = {hos_image_base, hos_image_end, HOS_HEAP_BASE, hos_heap_size, hos_code, hos_code_count};
    return l;
}

/* One mod's cheat file for this build (mod: the mod's name as host/mods.c has it) */
void hos_mod_cheats_add(const char *mod, const char *path) {
    extern const uint64_t hos_main_base;   /* gen/hos_image.c */
    if (nfiles == MAX_FILES || ncheats == MAX_CHEATS) {
        hos_log("cheats: %s: not read (%d cheat files and %d cheats are the most)", path, MAX_FILES, MAX_CHEATS);
        return;
    }
    FILE *f = fopen(path, "rb");
    char *text = f ? (char *)malloc(FILE_MOST + 1) : NULL;
    const size_t len = text ? fread(text, 1, FILE_MOST + 1, f) : 0;
    if (f) fclose(f);
    if (!text || !len || len > FILE_MOST) {
        hos_log("cheats: %s: %s", path, !f ? "cannot be read" : !text ? "no memory" : len ? "larger than 1 MB: not a cheat file" : "empty");
        free(text);
        return;
    }
    /* (a word is 8 digits and what parts it from the next: the file holds no more than len / 8 of them) */
    uint32_t *pool = (uint32_t *)malloc((len / 8 + 1) * sizeof *pool);   /* (kept: the cheats' code) */
    ModCheat *parsed = (ModCheat *)malloc(sizeof *parsed * (size_t)(MAX_CHEATS - ncheats));
    if (!pool || !parsed) {
        free(text), free(pool), free(parsed);
        return;
    }
    int cut = 0;
    const int n = mod_cheat_parse(text, len, parsed, MAX_CHEATS - ncheats, pool, (int)(len / 8 + 1), &cut);
    free(text);
    File *file = &files[nfiles];
    memset(file, 0, sizeof *file);
    snprintf(file->mod, sizeof file->mod, "%s", mod);
    file->first = ncheats, file->n = n;
    const ModCheatLayout l = game_layout();
    int counts[KIND_REFUSED + 1] = {0};
    for (int i = 0; i < n; i++) {
        Cheat *c = &cheats[ncheats + i];
        memset(c, 0, sizeof *c);
        c->c = parsed[i];
        c->file = nfiles, c->row = -1;
        uint64_t at = 0;
        if (c->c.why[0]) c->kind = KIND_REFUSED;
        else if (mod_cheat_is_heading(&c->c)) c->kind = KIND_HEADING;
        else if (mod_cheat_stores_code(c->c.words, c->c.n, &l, hos_main_base, &at)) {
            c->kind = KIND_REFUSED;
            snprintf(c->c.why, sizeof c->c.why, "it writes the game's code (main+%#llx), which is compiled ahead of time",
                     (unsigned long long)(at - hos_main_base));
        } else c->kind = c->c.master ? KIND_MASTER : KIND_SWITCH;
        /* (its switch's number: the name's second cheat in the file is "<name>#2", so each keeps its own) */
        char named[400];
        c->same = 1;
        for (int k = 0; k < i; k++) c->same += !strcmp(parsed[k].name, parsed[i].name);
        if (c->same > 1) snprintf(named, sizeof named, "%s/%s#%d", mod, c->c.name, c->same);
        else snprintf(named, sizeof named, "%s/%s", mod, c->c.name);
        c->key = mod_cheat_hash(named);
        counts[c->kind]++;
        if (c->kind == KIND_REFUSED && counts[KIND_REFUSED] <= 24)
            hos_log("cheats: %s: \"%s\" (line %d) cannot run: %s", mod, c->c.name, c->c.line, c->c.why);
    }
    free(parsed);
    ncheats += n, nfiles++;
    hos_log("cheats: %s: %d cheats in %s: %d to switch, %d always on, %d that cannot run, %d headings%s", mod, n, path, counts[KIND_SWITCH],
            counts[KIND_MASTER], counts[KIND_REFUSED], counts[KIND_HEADING], cut ? " (the file has more: the rest is left out)" : "");
}

/* Once every mod's file is read: which cheats were left on, and their rows (a heading has none) */
void hos_mod_cheats_ready(void) {
    if (!ncheats) return;
    on_load();
    int on = 0, unlisted = 0;
    for (int i = 0; i < ncheats; i++) {
        if (cheats[i].kind == KIND_HEADING) continue;
        if (nrows < MAX_ROWS) row_cheat[nrows] = i, cheats[i].row = nrows++;
        else unlisted++;
        on += cheats[i].kind == KIND_SWITCH && cheats[i].on;
    }
    count_running();
    hos_log("cheats: %d rows on the System tab, %d cheats left on from before (" ON_FILE ")%s", nrows, on,
            unlisted ? "; more cheats than rows: the last ones have none and stay as they are" : "");
}

/* ---- the rows ---- */
static void name_rows(void) {
    for (int r = 0; r < nrows; r++) {
        const Cheat *c = &cheats[row_cheat[r]];
        char line[400];
        if (c->kind == KIND_REFUSED) snprintf(line, sizeof line, "Not supported: %s. From the mod %s.", c->c.why, files[c->file].mod);
        else if (c->kind == KIND_MASTER) snprintf(line, sizeof line, "Always on: the master code of the mod %s.", files[c->file].mod);
        else snprintf(line, sizeof line, "From the mod %s.", files[c->file].mod);
        for (const char *p = c->c.name; *p; p++) rx_option_set(HOS_OPT_MODCHEAT_TEXT, (unsigned char)*p);
        rx_option_set(HOS_OPT_MODCHEAT_TEXT, '\n');
        if (strlen(c->c.name) > LONG_NAME) {   /* (a row has room for so much of a name: the whole of it is in its line) */
            rx_option_set(HOS_OPT_MODCHEAT_TEXT, '"');
            for (const char *p = c->c.name; *p; p++) rx_option_set(HOS_OPT_MODCHEAT_TEXT, (unsigned char)*p);
            for (const char *p = "\". "; *p; p++) rx_option_set(HOS_OPT_MODCHEAT_TEXT, (unsigned char)*p);
        }
        for (const char *p = line; *p; p++) rx_option_set(HOS_OPT_MODCHEAT_TEXT, (unsigned char)*p);
        rx_option_set(HOS_OPT_MODCHEAT_ADD, c->kind == KIND_REFUSED ? HOS_MODCHEAT_REFUSED : c->kind == KIND_MASTER ? HOS_MODCHEAT_ALWAYS
                                                                                           : c->on ? HOS_MODCHEAT_ON : 0);
    }
    rx_option_set(HOS_OPT_MODCHEAT_COUNT, nrows);
}
static void switched(int row, int on) {
    if (row < 0 || row >= nrows) return;
    Cheat *c = &cheats[row_cheat[row]];
    if (c->kind != KIND_SWITCH || c->on == on) return;
    c->on = on;
    c->said_read = c->said_drop = c->said_steps = 0;
    count_running();
    on_save();
    hos_log("cheats: %s: \"%s\" %s", files[c->file].mod, c->c.name, on ? "on" : "off");
}

/* ---- the game's memory ---- */
static int mem_read(void *ctx, uint64_t a, unsigned n, void *out) {
    if (!mod_cheat_readable((const ModCheatLayout *)ctx, a, n)) return 0;
    memcpy(out, GP(a), n);
    return 1;
}
static int mem_write(void *ctx, uint64_t a, unsigned n, const void *in) {
    if (!mod_cheat_writable((const ModCheatLayout *)ctx, a, n)) return 0;
    if (memcmp(GP(a), in, n)) {   /* (unchanged: not written, and not marked for the renderer) */
        memcpy(GP(a), in, n);
        HOS_WRITTEN(GP(a), n);
    }
    return 1;
}

static void run_cheat(Cheat *c, File *file, const ModCheatLayout *l, const ModCheatMem *m, uint64_t keys) {
    ModCheatRun r;
    const int end = mod_cheat_run(c->c.words, c->c.n, &file->state, m, keys, &r);
    if (end == MOD_CHEAT_STOP_READ && !c->said_read) {
        c->said_read = 1;
        hos_log("cheats: %s: \"%s\" reads %#llx, which is not the game's memory (a pointer that is not set yet?): it goes on once it is",
                file->mod, c->c.name, (unsigned long long)r.addr);
    }
    if (end == MOD_CHEAT_STOP_STEPS && !c->said_steps) {
        c->said_steps = 1;
        hos_log("cheats: %s: \"%s\" ran %d opcodes in one frame (a loop without end?): stopped for the frame, each frame", file->mod,
                c->c.name, MOD_CHEAT_STEPS);
    }
    if (r.dropped && !c->said_drop) {
        c->said_drop = 1;
        hos_log("cheats: %s: \"%s\" writes %#llx, which is %s: nothing is written there", file->mod, c->c.name, (unsigned long long)r.addr,
                mod_cheat_readable(l, r.addr, 1) ? "the game's code (compiled ahead of time)" : "not the game's memory");
    }
}

/* host/nvn_native.c, at each present of the game's */
void hos_mod_cheats_frame(void) {
    extern const uint64_t hos_main_base;   /* gen/hos_image.c */
    static int named;
    if (!ncheats) return;
    if (!named && rx_option(HOS_OPT_MODCHEAT_ASK, -1) >= 0) named = 1, name_rows();   /* (the renderer is there now) */
    for (int k = 0, ask; named && k < 8 && (ask = rx_option(HOS_OPT_MODCHEAT_ASK, 0)) > 1; k++) switched(ask / 2 - 1, ask & 1);
    if (!running) return;
    const ModCheatLayout l = game_layout();
    /* (the regions: as host/hos_svc.c's GetInfo tells the game) */
    const ModCheatMem m = {(void *)&l, mem_read, mem_write, {hos_main_base, HOS_HEAP_BASE, HOS_HEAP_BASE, 0x08000000ULL}};
    const uint64_t keys = __atomic_load_n(&hos_npad_buttons, __ATOMIC_RELAXED);
    for (int f = 0; f < nfiles; f++) {
        File *file = &files[f];
        mod_cheat_frame(&file->state);
        for (int pass = 0; pass < 2; pass++)   /* (the master codes first, then the cheats that are on, in the file's order) */
            for (int i = file->first; i < file->first + file->n; i++) {
                Cheat *c = &cheats[i];
                if (pass ? c->kind == KIND_SWITCH && c->on : c->kind == KIND_MASTER) run_cheat(c, file, &l, &m, keys);
            }
    }
}
