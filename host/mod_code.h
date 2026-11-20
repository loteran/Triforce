/* (Triforce) The mods' code patches (.ips, .pchtxt) and what the game was built with.

   The game's code is compiled ahead of time, so a patch of it is applied when the game is built (nx2/code_mods.py), and
   the build writes what it did into the game folder, triforce_code_mods.txt: a line per patch file, tab separated,

       applied <FNV-1a 64 of the file, 16 hex digits> <its size> <its path under the mods folder> <module> <bytes changed>
       refused <the same three>                                                              <why>

   (lines that start with # are notes). At its start the game holds that against the mods folder as it is now
   (host/mods.c): a patch file the build did not have, or had with other contents, waits for the next build, and so does
   one that is built in and is gone from the folder or switched off since. Nothing here changes the game: it is what the
   log says, so that a mod that seems to do nothing can be told from one that waits for a build.

   No Windows calls and no host state: tests/test_mod_code.py compiles it alone. */
#ifndef MOD_CODE_H
#define MOD_CODE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MOD_CODE_RECORD "triforce_code_mods.txt"
enum { MOD_CODE_PATH = 520, MOD_CODE_WHY = 200 };

typedef struct ModCodeEntry {
    int applied;                 /* 0: the build refused it (why) */
    uint64_t fnv, size;
    char path[MOD_CODE_PATH];    /* "<mod>/exefs/<file>", as the build wrote it */
    char why[MOD_CODE_WHY];
    int seen;                    /* the mods folder has the file now (mod_code_state) */
} ModCodeEntry;

/* what a patch file of the mods folder is to the game as built */
enum {
    MOD_CODE_BUILT,      /* built in, as it is now */
    MOD_CODE_CHANGED,    /* built in with other contents: the next build has it as it is now */
    MOD_CODE_REFUSED,    /* the build refused it as it is now (why) */
    MOD_CODE_WAITS       /* the build did not have it, or refused other contents: the next build looks at it */
};

static inline uint64_t mod_code_fnv(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static inline int mod_code_is_patch(const char *name) {   /* (a file name: .ips or .pchtxt, any case) */
    const size_t n = strlen(name);
    static const char *const ends[] = {".ips", ".pchtxt"};
    for (int k = 0; k < 2; k++) {
        const size_t m = strlen(ends[k]);
        if (n <= m) continue;
        size_t i = 0;
        while (i < m && (name[n - m + i] | 0x20) == ends[k][i]) i++;
        if (i == m) return 1;
    }
    return 0;
}

/* The record's entries from its text (changed in place: a line's fields are cut apart), at most cap of them. A line
   that is not one of the two kinds, or whose numbers cannot be read, is passed over. Returns how many */
static inline int mod_code_parse(char *text, ModCodeEntry *out, int cap) {
    int n = 0;
    for (char *line = text; line && *line && n < cap;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (end) *end = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;
        char *field[6] = {0};
        int nf = 0;
        if (*line != '#')
            for (char *p = line; nf < 6; nf++) {
                field[nf] = p;
                char *tab = strchr(p, '\t');
                if (!tab) { nf++; break; }
                *tab = 0;
                p = tab + 1;
            }
        const int applied = nf >= 5 && !strcmp(field[0], "applied"), refused = nf >= 5 && !strcmp(field[0], "refused");
        if (applied || refused) {
            char *e1 = NULL, *e2 = NULL;
            ModCodeEntry *e = &out[n];
            memset(e, 0, sizeof *e);
            e->applied = applied;
            e->fnv = strtoull(field[1], &e1, 16);
            e->size = strtoull(field[2], &e2, 10);
            if (*field[1] && !*e1 && *field[2] && !*e2 && *field[3] && strlen(field[3]) < sizeof e->path) {
                memcpy(e->path, field[3], strlen(field[3]) + 1);
                if (refused) {
                    size_t wn = strlen(field[4]);
                    if (wn > sizeof e->why - 1) wn = sizeof e->why - 1;
                    memcpy(e->why, field[4], wn);
                    e->why[wn] = 0;
                }
                n++;
            }
        }
        line = next;
    }
    return n;
}

/* A patch file that is in the mods folder now (path as the build names it, its contents' number and size) against the
   record: MOD_CODE_*, and for MOD_CODE_REFUSED the build's reason in *why. The entry is marked seen */
static inline int mod_code_state(ModCodeEntry *e, int n, const char *path, uint64_t fnv, uint64_t size, const char **why) {
    if (why) *why = "";
    for (int i = 0; i < n; i++) {
        if (strcmp(e[i].path, path)) continue;
        e[i].seen = 1;
        const int same = e[i].fnv == fnv && e[i].size == size;
        if (e[i].applied) return same ? MOD_CODE_BUILT : MOD_CODE_CHANGED;
        if (!same) return MOD_CODE_WAITS;
        if (why) *why = e[i].why;
        return MOD_CODE_REFUSED;
    }
    return MOD_CODE_WAITS;
}

/* The next entry from *at on that is built into the game and that mod_code_state was not asked about: its file is gone
   from the mods folder, or its mod is switched off. NULL when there is no more */
static inline const ModCodeEntry *mod_code_gone(const ModCodeEntry *e, int n, int *at) {
    while (*at < n) {
        const ModCodeEntry *x = &e[(*at)++];
        if (x->applied && !x->seen) return x;
    }
    return NULL;
}
#endif
