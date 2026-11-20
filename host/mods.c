/* Mods, in the layout emulators use, so RomFS mods made for them work as they are:
     <mods>/<name>/romfs/...          files that replace (or add to) the game's RomFS
     <mods>/<name>/exefs/...          .ips and .pchtxt patches of the game's code: applied when the game is built
     <mods>/<name>/cheats/<id>.txt    a cheat file for the build of the game with that id: run by host/mod_cheats.c
   <mods> is the "mods" folder beside the executable, or HOS_MODS=<dir>. A mod may sit deeper, as emulators keep them
   (<mods>/contents/<title id>/<name>/romfs): any folder holding a romfs, exefs or cheats folder is a mod. Folders whose
   names start with '-' or '_' are left out (off). When two mods have the same file, the one whose path sorts last wins
   (name them 01-..., 02-... to order them).
   Every mod file is indexed once at the start (no disk lookups per open); nn_fs.c asks hos_mods_romfs for each rom:
   path and lists a folder's mod-only entries with the game's own (hos_mods_dir_extras). HOS_FS_OVERLAY (nn_fs.c) is
   looked at after this, so a test overlay wins over mods.
   (Triforce) exefs patches cannot be applied here: the code is compiled ahead of time. A build applies them to the code it
   translates (nx2/code_mods.py walks the mods folder as find_mods below does) and writes what it did into the game
   folder; the start holds that against the mods folder as it is now (mod_code.h) and says in the log which patch files
   are built in and which wait for a build: one added or edited since, and one that is built in and has been taken away
   or switched off, which the game has until it is built again.
   From Skybound's loader (NotRexed/SkyboundTotk a64-kit/hos/mods.c, MIT): its RomFS part. */
#include "hos.h"
#include "hos_paths.h"
#include "mod_code.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct { char *key; char *host; uint64_t size; int is_dir, logged; volatile uint64_t need; } Entry;   /* key: romfs path, lower case, '/'; need: hos_mods_table_size's */
static Entry *table;
static size_t table_cap, table_n;

static uint64_t hash_str(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}
static void lower_slash(char *s) {
    for (; *s; s++) *s = *s == '\\' ? '/' : (char)tolower((unsigned char)*s);
}
static const char *last_sep(const char *s) {   /* (a host path: '\' on Windows, '/' from host/posix) */
    const char *b = strrchr(s, '\\'), *f = strrchr(s, '/');
    return b > f ? b : f;
}
static Entry *find(const char *key) {
    if (!table_cap) return NULL;
    for (size_t i = hash_str(key) & (table_cap - 1);; i = (i + 1) & (table_cap - 1)) {
        if (!table[i].key) return NULL;
        if (!strcmp(table[i].key, key)) return &table[i];
    }
}
static void put(const char *key, const char *host, uint64_t size, int is_dir) {
    if ((table_n + 1) * 2 > table_cap) {   /* (grow: rehash) */
        Entry *old = table;
        const size_t old_cap = table_cap;
        table_cap = table_cap ? table_cap * 2 : 1024;
        table = (Entry *)calloc(table_cap, sizeof *table);
        table_n = 0;
        for (size_t i = 0; i < old_cap; i++)
            if (old[i].key) {
                size_t j = hash_str(old[i].key) & (table_cap - 1);
                while (table[j].key) j = (j + 1) & (table_cap - 1);
                table[j] = old[i];
                table_n++;
            }
        free(old);
    }
    size_t i = hash_str(key) & (table_cap - 1);
    while (table[i].key && strcmp(table[i].key, key)) i = (i + 1) & (table_cap - 1);
    if (!table[i].key) table[i].key = _strdup(key), table_n++;
    else free(table[i].host);
    table[i].host = _strdup(host);
    table[i].size = size;
    table[i].is_dir = is_dir;
}

/* a mod's romfs folder, recursively: rel is the path inside the RomFS so far ("" at its root) */
static int index_romfs(const char *host_dir, const char *rel, int depth) {
    if (depth > 32) return 0;   /* (hookshot: no RomFS is this deep) */
    if (!depth) {   /* (hookshot) the romfs folder itself a link (to / in a mod's archive): the walk covered the whole disk */
        const DWORD at = GetFileAttributesA(host_dir);
        if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_REPARSE_POINT)) {
            hos_log("mods: %s is a link to a folder: left out", host_dir);
            return 0;
        }
    }
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", host_dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    int files = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char host[MAX_PATH], key[MAX_PATH];
        snprintf(host, sizeof host, "%s\\%s", host_dir, fd.cFileName);
        /* (hookshot) a link to a folder is not followed (two links back up made the walk endless); links to files are
           (mod managers place files as links) */
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            hos_log("mods: %s is a link to a folder: left out", host);
            continue;
        }
        snprintf(key, sizeof key, "%s%s%s", rel, *rel ? "/" : "", fd.cFileName);
        lower_slash(key);
        const int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        put(key, host, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, is_dir);
        if (is_dir) {
            char sub_rel[MAX_PATH];
            snprintf(sub_rel, sizeof sub_rel, "%s%s%s", rel, *rel ? "/" : "", fd.cFileName);
            files += index_romfs(host, sub_rel, depth + 1);
        } else {
            files++;
        }
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    return files;
}

/* the mod folders under root, sorted (a mod: a folder holding romfs, exefs or cheats) */
typedef struct { char path[MAX_PATH]; char name[MAX_PATH]; } Mod;
static Mod *mods;
static int nmods, cap_mods;
static void find_mods(const char *dir, const char *name, int depth) {
    char probe[MAX_PATH];
    snprintf(probe, sizeof probe, "%s\\romfs", dir);
    const int has_romfs = GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES;
    snprintf(probe, sizeof probe, "%s\\exefs", dir);
    const int has_exefs = GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES;
    snprintf(probe, sizeof probe, "%s\\cheats", dir);   /* (Triforce: a mod of cheats alone) */
    const int has_cheats = GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES;
    if ((has_romfs || has_exefs || has_cheats) && depth > 0) {
        if (nmods == cap_mods) mods = (Mod *)realloc(mods, sizeof *mods * (size_t)(cap_mods = cap_mods ? cap_mods * 2 : 16));
        snprintf(mods[nmods].path, MAX_PATH, "%s", dir);
        snprintf(mods[nmods].name, MAX_PATH, "%s", name);
        nmods++;
        return;
    }
    if (depth >= 4) return;
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        if (fd.cFileName[0] == '-' || fd.cFileName[0] == '_') {
            hos_log("mods: %s\\%s is off (its name starts with %c)", dir, fd.cFileName, fd.cFileName[0]);
            continue;
        }
        char sub[MAX_PATH], sub_name[MAX_PATH];
        snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
        snprintf(sub_name, sizeof sub_name, "%s%s%s", name, *name ? "/" : "", fd.cFileName);
        find_mods(sub, sub_name, depth + 1);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}
static int mod_cmp(const void *a, const void *b) { return _stricmp(((const Mod *)a)->name, ((const Mod *)b)->name); }

/* (Triforce) a file's bytes, at most `most` of them (a larger file: NULL), with a 0 after them; freed by the caller */
static char *read_whole(const char *path, size_t most, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *data = NULL;
    size_t n = 0, cap = 0;
    for (;;) {   /* (in pieces: no room taken for more than the file has) */
        if (n == cap) {
            cap = cap ? cap * 2 : 4096;
            char *more = cap <= most + 4096 ? (char *)realloc(data, cap + 1) : NULL;
            if (!more) {
                free(data);
                fclose(f);
                return NULL;
            }
            data = more;
        }
        const size_t got = fread(data + n, 1, cap - n, f);
        if (!got) break;
        n += got;
    }
    fclose(f);
    if (n > most) {
        free(data);
        return NULL;
    }
    data[n] = 0;
    *len = n;
    return data;
}

/* (Triforce) The mods' code patches against what the game was built with (mod_code.h, nx2/code_mods.py): the build's
   record from the game folder, a line in the log for each patch file of a mod, and how many wait for a build */
enum { BUILT_MOST = 512 };
static ModCodeEntry *built;
static int nbuilt, code_waits;
static void code_record_load(void) {
    char path[MAX_PATH + 32];
    hos_game_path_a(path, MAX_PATH);
    char *slash = (char *)last_sep(path);
    if (!slash) return;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "%s", MOD_CODE_RECORD);
    size_t len = 0;
    char *text = read_whole(path, 1 << 20, &len);
    if (!text) return;   /* (no record: a game built without code patches) */
    built = (ModCodeEntry *)calloc(BUILT_MOST, sizeof *built);
    if (built) nbuilt = mod_code_parse(text, built, BUILT_MOST);
    free(text);
}
/* (returns how many of this mod's patches wait for a build: new since it, or changed) */
static int code_patches(const Mod *mod) {
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\exefs\\*", mod->path);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    int waits = 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !mod_code_is_patch(fd.cFileName)) continue;
        char path[MAX_PATH], rel[MOD_CODE_PATH];
        snprintf(path, sizeof path, "%s\\exefs\\%s", mod->path, fd.cFileName);
        snprintf(rel, sizeof rel, "%s/exefs/%s", mod->name, fd.cFileName);
        size_t len = 0;
        char *data = read_whole(path, 16 << 20, &len);
        const char *why = "";
        const int state = data ? mod_code_state(built, nbuilt, rel, mod_code_fnv(data, len), len, &why) : MOD_CODE_WAITS;
        free(data);
        if (state == MOD_CODE_BUILT) hos_log("mods: %s is built into the game", rel);
        else if (state == MOD_CODE_REFUSED) hos_log("mods: %s was not applied when the game was built: %s", rel, why);
        else if (state == MOD_CODE_CHANGED) hos_log("mods: %s changed since the game was built: the game has it as it was then", rel);
        else hos_log("mods: %s is not built into the game", rel);
        waits += state == MOD_CODE_CHANGED || state == MOD_CODE_WAITS;
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    code_waits += waits;
    return waits;
}
/* (and the patches the game has that the mods folder no longer does, with or without a mods folder) */
static void code_patches_gone(void) {
    int at = 0;
    for (const ModCodeEntry *e; (e = mod_code_gone(built, nbuilt, &at)) != NULL; code_waits++)
        hos_log("mods: %s is built into the game, and is gone from the mods folder or switched off: the game has it until it is built again",
                e->path);
    if (code_waits)
        hos_log("mods: %d code patches wait for a build: they are applied when the game is built. Build it again for the mods folder as "
                "it is now (Triforce Setup, or nx2/make_game.py --update with the game folder)", code_waits);
}

/* (Triforce) a mod's cheat file for this build of the game, cheats\<the first 16 digits of main's build id>.txt (the
   name's case does not matter), to host/mod_cheats.c; one named after another build is said and left */
static void cheat_files(const Mod *mod) {
    void hos_mod_cheats_add(const char *mod, const char *path);
    char pat[MAX_PATH], id[17];
    snprintf(id, sizeof id, "%.16s", hos_main_build_id);
    snprintf(pat, sizeof pat, "%s\\cheats\\*", mod->path);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        const size_t n = strlen(fd.cFileName);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n != 20 || _stricmp(fd.cFileName + 16, ".txt")) continue;
        if (strspn(fd.cFileName, "0123456789abcdefABCDEF") != 16) continue;   /* (not named after a build: not a cheat file) */
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\cheats\\%s", mod->path, fd.cFileName);
        if (strlen(id) == 16 && !_strnicmp(fd.cFileName, id, 16)) hos_mod_cheats_add(mod->name, path);
        else hos_log("mods: %s/cheats/%s is for another build of the game (this one is %s): not read", mod->name, fd.cFileName, id);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

static void mods_scan(void) {
    char root[MAX_PATH];
    if (getenv("HOS_MODS")) {
        snprintf(root, sizeof root, "%s", getenv("HOS_MODS"));
    } else {
        hos_game_path_a(root, sizeof root);
        char *slash = (char *)last_sep(root);
        if (!slash) return;
        snprintf(slash + 1, sizeof root - (size_t)(slash + 1 - root), "mods");
    }
    if (GetFileAttributesA(root) == INVALID_FILE_ATTRIBUTES) return;
    find_mods(root, "", 0);
    if (!nmods) {
        hos_log("mods: none in %s (a mod: <name>\\romfs)", root);
        return;
    }
    qsort(mods, (size_t)nmods, sizeof *mods, mod_cmp);
    for (int i = 0; i < nmods; i++) {
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%s\\romfs", mods[i].path);
        /* (Triforce: its exefs patches, built in or waiting, first. (11-18) A mod whose code waits for a build keeps its RomFS
           files waiting too: the game crashed at its start with a mod's new files over its old code (Classic Item Slots
           put in by the Mods menu, #bug-tracker "Crashing on startup"; a build put them in step). They come with the code,
           at the next build. HOS_MODS_SPLIT=1: the files now anyway, as before) */
        const int waits = code_patches(&mods[i]);
        static int split = -1;
        if (split < 0) split = getenv("HOS_MODS_SPLIT") && atoi(getenv("HOS_MODS_SPLIT")) == 1;
        const int hold = waits > 0 && !split;
        const int files = hold ? 0 : index_romfs(dir, "", 0);
        cheat_files(&mods[i]);    /* (and its cheat file) */
        if (hold)
            hos_log("mods: %s: its RomFS files wait for the build too, with its code (%d patch(es) not built in yet)", mods[i].name, waits);
        else
            hos_log("mods: %s: %d RomFS files", mods[i].name, files);
    }
    hos_log("mods: %d mods from %s: %zu RomFS entries", nmods, root, table_n);
}

/* hos_main.c, once the image is loaded (image_path: not used; exefs patches are applied when the game is built) */
void hos_mods_init(const char *image_path) {
    void hos_mod_cheats_ready(void);
    (void)image_path;
    code_record_load();
    mods_scan();
    code_patches_gone();
    hos_mod_cheats_ready();   /* (mod_cheats.c: every mod's cheat file is read) */
}

/* nn_fs.c: the mod file for a RomFS path ("UI/x.zs", no mount, no leading slash), or 0 */
int hos_mods_romfs(const char *rel, char *out, size_t cap) {
    char key[MAX_PATH];
    snprintf(key, sizeof key, "%s", rel);
    lower_slash(key);
    int hos_prompts_romfs(const char *key, char *out, size_t cap);   /* prompts.c: the button prompts, where no mod has the file */
    Entry *e = table_n ? find(key) : NULL;
    if (!e || e->is_dir) return hos_prompts_romfs(key, out, cap);
    snprintf(out, cap, "%s", e->host);
    static volatile LONG logged;
    if (!e->logged && InterlockedIncrement(&logged) <= 200) hos_log("mods: %s <- %s", rel, e->host);
    e->logged = 1;
    return 1;
}

/* ((Triforce) 11-17) The size a zstd file holds once unpacked: its frame header's content size (0: not a zstd frame, or
   the size not written) */
static uint64_t zs_content_size(const char *host_path) {
    FILE *f = fopen(host_path, "rb");
    if (!f) return 0;
    uint8_t h[18] = {0};
    const size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < 6 || h[0] != 0x28 || h[1] != 0xb5 || h[2] != 0x2f || h[3] != 0xfd) return 0;
    const int fcs_flag = h[4] >> 6, single = h[4] >> 5 & 1, dict = h[4] & 3;
    const size_t at = 5 + (single ? 0 : 1) + (size_t)(dict == 3 ? 4 : dict);
    const size_t len = fcs_flag == 0 ? (single ? 1 : 0) : fcs_flag == 1 ? 2 : fcs_flag == 2 ? 4 : 8;
    if (!len || at + len > n) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) v |= (uint64_t)h[at + i] << (8 * i);
    return len == 2 ? v + 256 : v;
}

/* ((Triforce) 11-17) The memory the game makes for loading a file is the ResourceSizeTable's figure for its path (the
   lifted f_801b6340 asks the table, f_836e3d30, and this is called with the answer: nx2/oot_patches.py patch_mod_sizes).
   For a file a mod serves, the table's figure (the game's, or the one the mod ships) can be too small: a model with more
   materials and shapes than the one it replaces needs more beside its own bytes, and the game does not check the
   allocations that then fail (the Dark Link mod's Deku Shield and Slingshot crashed loading a child save, a write to
   address 0). So a mod's file gets at least its unpacked size and room beside it: an eighth of it, 128 KB at least, and
   64 KB more (a model needs about 8 KB, and 3 to 4 KB a material and a shape: 99 of the game's own fitted, 11-17). The
   game's own files keep the table's figure; a figure is only ever raised. size: the table's, path: the guest's path */
volatile LONG hos_mods_size_hook_seen;   /* ((Triforce, 11-18) the lifted code calls the hook below: prompts.c asks */
int hos_prompts_font_file(const char *key, char *out, size_t cap);   /* prompts.c */
uint64_t hos_mods_table_size(uint64_t size, uint64_t path) {
    if (!hos_mods_size_hook_seen) InterlockedExchange(&hos_mods_size_hook_seen, 1);
    if (!table_n || !path) return size;
    const char *p = (const char *)GP(path);
    char rel[MAX_PATH], key[MAX_PATH];
    size_t i = 0;
    for (; i + 4 < sizeof rel && p[i]; i++) rel[i] = p[i];
    rel[i] = 0;
    snprintf(key, sizeof key, "%s.zs", rel);   /* (the table names the unpacked file; the RomFS has it packed) */
    lower_slash(key);
    Entry *e = find(key);
    int packed = 1;
    if (!e) {
        /* ((Triforce, 11-18) the button prompts' font, larger than the game's: with a mod's own size table served, ours
           (prompts/rsizetable.zs, which has room for it) is not, so its room comes from here instead. Before, prompts.c
           gave the prompts up whenever a mod had a size table (#bug-tracker 11-18: "Button Prompt UI Broken", with the
           Young Link Rides Pony Epona mod) */
        char built[MAX_PATH];
        if (hos_prompts_font_file(key, built, sizeof built)) {
            const uint64_t real = zs_content_size(built);
            const uint64_t need = real ? (real + (64u << 10) + 0xfff) & ~(uint64_t)0xfff : 0;
            static volatile LONG told_font;
            if (need > (uint32_t)size && !InterlockedExchange(&told_font, 1))
                hos_log("mods: the button prompts' font gets %llu bytes to load in (the size table's %llu; the font %llu unpacked)",
                        (unsigned long long)need, (unsigned long long)(uint32_t)size, (unsigned long long)real);
            return need > (uint32_t)size ? need : size;
        }
        snprintf(key, sizeof key, "%s", rel);
        lower_slash(key);
        e = find(key), packed = 0;
    }
    if (!e || e->is_dir) return size;
    uint64_t need = e->need;
    if (!need) {
        const uint64_t real = packed ? zs_content_size(e->host) : e->size;
        if (!real) return size;   /* (a packed file whose header has no size: the table's figure, as the game has it) */
        const uint64_t room = real / 8 > (128u << 10) ? real / 8 : (128u << 10);
        need = (real + room + (64u << 10) + 0xfff) & ~(uint64_t)0xfff;
        if (need > 0xfffff000u) need = 0xfffff000u;
        e->need = need;
        static volatile LONG told;
        if (need > (uint32_t)size && InterlockedIncrement(&told) <= 100)
            hos_log("mods: %s gets %llu bytes to load in (the size table's %llu; the file %llu unpacked)", rel,
                    (unsigned long long)need, (unsigned long long)(uint32_t)size, (unsigned long long)real);
    }
    return need > (uint32_t)size ? need : size;
}

/* prompts.c: 1 when a mod has a file whose key (lower case, '/') starts with prefix and ends with suffix */
int hos_mods_has(const char *prefix, const char *suffix) {
    const size_t pn = strlen(prefix), sn = strlen(suffix);
    for (size_t i = 0; i < table_cap; i++) {
        const char *k = table[i].key;
        const size_t kn = k ? strlen(k) : 0;
        if (k && !table[i].is_dir && kn >= pn + sn && !strncmp(k, prefix, pn) && !strcmp(k + kn - sn, suffix)) return 1;
    }
    return 0;
}

/* nn_fs.c, listing a RomFS folder: fn(arg, name, is_dir, size) for each mod entry directly in it that the game's own
   folder (base_dir, a host path; may not exist) does not have. Returns how many. */
int hos_mods_dir_extras(const char *rel, const char *base_dir, void (*fn)(void *, const char *, int, uint64_t),
                        void *arg) {
    if (!table_n) return 0;
    char prefix[MAX_PATH];
    snprintf(prefix, sizeof prefix, "%s", rel);
    lower_slash(prefix);
    size_t pn = strlen(prefix);
    while (pn && prefix[pn - 1] == '/') prefix[--pn] = 0;
    int n = 0;
    for (size_t i = 0; i < table_cap; i++) {
        const Entry *e = &table[i];
        if (!e->key) continue;
        const char *k = e->key;
        if (pn && (strncmp(k, prefix, pn) || k[pn] != '/')) continue;
        const char *name = pn ? k + pn + 1 : k;
        if (!*name || strchr(name, '/')) continue;   /* (deeper) */
        const char *real = last_sep(e->host);   /* (the name as the mod spells it) */
        real = real ? real + 1 : e->host;
        char base[MAX_PATH];
        snprintf(base, sizeof base, "%s\\%s", base_dir, real);
        if (GetFileAttributesA(base) != INVALID_FILE_ATTRIBUTES) continue;
        fn(arg, real, e->is_dir, e->size);
        n++;
    }
    return n;
}
