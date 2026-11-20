/* (triforce) What a save export holds and what an import takes (save_pack.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "save_pack.h"

#define GAME "The Legend of Zelda: Ocarina of Time"
#define MANIFEST_MAX 65536

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int same_text(const char *a, const char *b) {   /* ASCII, any case */
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) a++, b++;
    return !*a && !*b;
}

static int ends_with(const char *s, const char *tail) {
    const size_t n = strlen(s), k = strlen(tail);
    return n >= k && same_text(s + n - k, tail);
}

/* Key files never go into a zip or out of one, wherever a stray copy sits in a save folder. */
static int key_file(const char *rel) { return ends_with(rel, ".keys") || ends_with(rel, ".keys.txt"); }

int save_pack_save_file(const char *rel) {
    if (!strcmp(rel, "account.sav") || !strcmp(rel, "option.sav")) return 1;
    return !strncmp(rel, "slot_", 5) && rel[5] >= '0' && rel[5] <= '9' && rel[6] >= '0' && rel[6] <= '9' &&
           !strcmp(rel + 7, "/progress.sav");
}

/* (a part's name up to its first dot is what Windows takes for a device: "NUL", "nul.sav", "COM1.txt") */
static int device_name(const char *part, size_t n) {
    static const char *const devices[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$"};
    size_t stem = 0;
    while (stem < n && part[stem] != '.') stem++;
    for (size_t d = 0; d < sizeof devices / sizeof *devices; d++) {
        size_t k = 0;
        while (k < stem && devices[d][k] && lower((unsigned char)part[k]) == lower((unsigned char)devices[d][k])) k++;
        if (k == stem && !devices[d][k]) return 1;
    }
    if (stem == 4 && part[3] >= '0' && part[3] <= '9') {
        const int c0 = lower((unsigned char)part[0]), c1 = lower((unsigned char)part[1]), c2 = lower((unsigned char)part[2]);
        if ((c0 == 'c' && c1 == 'o' && c2 == 'm') || (c0 == 'l' && c1 == 'p' && c2 == 't')) return 1;
    }
    return 0;
}

int save_pack_safe_rel(const char *rel) {
    const size_t len = strlen(rel);
    if (!len || len > 100) return 0;
    int parts = 0;
    for (const char *p = rel;;) {
        const char *slash = strchr(p, '/');
        const size_t k = slash ? (size_t)(slash - p) : strlen(p);
        if (!k || ++parts > 4 || p[0] == '.' || p[k - 1] == '.' || device_name(p, k)) return 0;
        for (size_t i = 0; i < k; i++) {
            const char c = p[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                return 0;
        }
        if (!slash) return 1;
        p = slash + 1;
    }
}

/* ---- an export ---- */
void save_pack_begin(SavePack *p, const struct tm *local) {
    memset(p, 0, sizeof *p);
    sz_writer_init(&p->w, local->tm_year + 1900, local->tm_mon + 1, local->tm_mday, local->tm_hour, local->tm_min, local->tm_sec);
}

int save_pack_add_save(SavePack *p, const char *rel, const void *data, size_t n) {
    if (key_file(rel)) return 1;
    char name[128];
    if (!save_pack_safe_rel(rel) || snprintf(name, sizeof name, "save/%s", rel) >= (int)sizeof name) return 0;
    sz_writer_add(&p->w, name, data, n);
    p->files++;
    return !p->w.failed;
}

void save_pack_add_report(SavePack *p, const char *name, const void *data, size_t n) {
    char member[160];
    if (snprintf(member, sizeof member, "bug-report/%s", name) < (int)sizeof member) sz_writer_add(&p->w, member, data, n);
}

uint8_t *save_pack_end(SavePack *p, const char *version, const struct tm *utc, int bug_report, size_t *n) {
    char ver[96], manifest[1024];
    sz_json_escape(version ? version : "unknown", ver, sizeof ver);
    const int mn = snprintf(manifest, sizeof manifest,
                            "{\n \"format\": \"" SAVE_PACK_FORMAT "\",\n \"format_version\": %d,\n \"game\": \"" GAME "\",\n"
                            " \"title_id\": \"" SAVE_PACK_TITLE_ID "\",\n \"triforce\": \"%s\",\n"
                            " \"exported\": \"%04d-%02d-%02dT%02d:%02d:%02dZ\",\n \"exported_by\": \"game\",\n"
                            " \"files\": %d,\n \"bug_report\": %s\n}\n",
                            SAVE_PACK_VERSION, ver, utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday, utc->tm_hour, utc->tm_min,
                            utc->tm_sec, p->files, bug_report ? "true" : "false");
    sz_writer_add(&p->w, "manifest.json", manifest, (size_t)mn);
    return sz_writer_finish(&p->w, n);
}

/* ---- an import ---- */
static const SzEntry *find_member(const SzEntry *e, int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (!strcmp(e[i].name, name)) return &e[i];
    return NULL;
}

/* The manifest's verdict on the zip; out gets who made it and when, *files how many files of the save it counts (-1: it
   does not say). */
static SavePackResult check_manifest(const uint8_t *zip, size_t n, const SzEntry *e, int count, SavePackSave *out, long long *files) {
    const SzEntry *m = find_member(e, count, "manifest.json");
    if (!m || m->usize > MANIFEST_MAX) return SAVE_PACK_NOT_A_SAVE;
    int err;
    char *json = (char *)sz_extract(zip, n, m, &err);
    if (!json) return err == SZ_E_MEMORY ? SAVE_PACK_NO_MEMORY : SAVE_PACK_DAMAGED;
    char format[32] = "", title[32] = "", ver[64] = "", when[32] = "";
    long long version = 0;
    SavePackResult r = SAVE_PACK_OK;
    if (!sz_json_int(json, "files", files)) *files = -1;
    const int has_format = sz_json_str(json, "format", format, sizeof format);
    const int ours = has_format && !strcmp(format, SAVE_PACK_FORMAT), navi = has_format && !strcmp(format, SAVE_PACK_FORMAT_NX2NAVI);
    if ((!ours && !navi) || !sz_json_int(json, "format_version", &version) || version < 1) r = SAVE_PACK_NOT_A_SAVE;
    else if (version > SAVE_PACK_VERSION) r = SAVE_PACK_NEWER;
    else if (!sz_json_str(json, "title_id", title, sizeof title) || !same_text(title, SAVE_PACK_TITLE_ID)) r = SAVE_PACK_OTHER_GAME;
    if (ours || navi) {
        sz_json_str(json, ours ? "triforce" : "nx2navi", ver, sizeof ver);
        sz_json_str(json, "exported", when, sizeof when);
        snprintf(out->made_by, sizeof out->made_by, "%s %s", ours ? "Triforce" : "NX2navi", ver[0] ? ver : "?");
        snprintf(out->exported, sizeof out->exported, "%.10s", when[0] ? when : "?");
    }
    free(json);
    return r;
}

void save_pack_free(SavePackSave *s) {
    for (int i = 0; i < s->count; i++) free(s->rels ? s->rels[i] : NULL), free(s->data ? s->data[i] : NULL);
    free(s->rels), free(s->data), free(s->sizes);
    s->rels = NULL, s->data = NULL, s->sizes = NULL, s->count = 0;
}

SavePackResult save_pack_open(const uint8_t *zip, size_t n, SavePackSave *out) {
    memset(out, 0, sizeof *out);
    if (n > SAVE_PACK_MAX_ZIP) return SAVE_PACK_UNSAFE;
    SzEntry *e = NULL;
    int count = 0;
    long long said = -1;
    const int zr = sz_read_dir(zip, n, &e, &count);
    SavePackResult r = zr == SZ_OK ? check_manifest(zip, n, e, count, out, &said)
                       : zr == SZ_E_NOT_ZIP ? SAVE_PACK_NOT_A_SAVE
                       : zr == SZ_E_MEMORY  ? SAVE_PACK_NO_MEMORY
                                            : SAVE_PACK_DAMAGED;
    /* the save's members: all of them checked before any is unpacked */
    int files = 0, has_save = 0;
    uint64_t bytes = 0;
    for (int i = 0; r == SAVE_PACK_OK && i < count; i++) {
        if (strncmp(e[i].name, "save/", 5) || e[i].is_dir) continue;
        const char *rel = e[i].name + 5;
        if (e[i].is_link || !save_pack_safe_rel(rel)) r = SAVE_PACK_UNSAFE;
        else if (key_file(rel)) continue;
        for (int k = 0; r == SAVE_PACK_OK && k < i; k++)   /* (a name twice, in any case: one file on Windows) */
            if (!strncmp(e[k].name, "save/", 5) && same_text(e[k].name, e[i].name)) r = SAVE_PACK_UNSAFE;
        bytes += e[i].usize;
        if (++files > SAVE_PACK_MAX_FILES || bytes > SAVE_PACK_MAX_BYTES) r = SAVE_PACK_UNSAFE;
        has_save |= save_pack_save_file(rel);
    }
    if (r == SAVE_PACK_OK && !has_save) r = SAVE_PACK_NOT_A_SAVE;
    /* (the manifest counts the save's files: a member gone from the directory, or one too many, is damage) */
    if (r == SAVE_PACK_OK && said >= 0 && said != files) r = SAVE_PACK_DAMAGED;
    if (r == SAVE_PACK_OK) {
        out->rels = calloc((size_t)files, sizeof *out->rels);
        out->data = calloc((size_t)files, sizeof *out->data);
        out->sizes = calloc((size_t)files, sizeof *out->sizes);
        if (!out->rels || !out->data || !out->sizes) r = SAVE_PACK_NO_MEMORY;
    }
    for (int i = 0; r == SAVE_PACK_OK && i < count; i++) {
        if (strncmp(e[i].name, "save/", 5) || e[i].is_dir || key_file(e[i].name + 5)) continue;
        int err;
        const int k = out->count;
        out->data[k] = sz_extract(zip, n, &e[i], &err);
        const size_t len = strlen(e[i].name + 5) + 1;
        out->rels[k] = out->data[k] ? malloc(len) : NULL;
        if (!out->data[k]) r = err == SZ_E_MEMORY ? SAVE_PACK_NO_MEMORY : SAVE_PACK_DAMAGED;
        else if (!out->rels[k]) r = SAVE_PACK_NO_MEMORY;
        else memcpy(out->rels[k], e[i].name + 5, len);
        out->sizes[k] = (size_t)e[i].usize;
        out->count++;   /* (counted either way: save_pack_free frees what this one got) */
    }
    free(e);
    if (r != SAVE_PACK_OK) save_pack_free(out);
    return r;
}

const char *save_pack_why(SavePackResult r) {
    static const char *const why[] = {
        "It is a save export.",
        "Not enough memory to read it.",
        "It is not a save export of Triforce or NX2navi.",
        "It is a save of another game.",
        "It was made by a newer version: update Triforce first.",
        "It is damaged.",
        "It holds paths outside the save, links, or far more than a save.",
    };
    return (unsigned)r < sizeof why / sizeof *why ? why[r] : "It could not be read.";
}
