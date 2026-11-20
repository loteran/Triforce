/* tests/test_save_zip.py's program: host/save_zip.c and host/save_pack.c as the game has them, driven from the command line
   so that Python's zipfile and zlib can be put against them. Names and file lists come in UTF-8 text files, never as
   arguments (a Windows program's arguments are in the system's code page). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "save_pack.h"
#include "save_zip.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size + 1);
    *n = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[*n] = 0;
    return data;
}

static int spill(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const size_t put = fwrite(data, 1, n, f);
    return !fclose(f) && put == n;
}

/* a pattern with long repeats and stretches without any: what deflate has to cope with */
static void fill(uint8_t *p, size_t n, uint32_t seed) {
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        p[i] = (i / 97) % 3 == 0 ? (uint8_t)(seed >> 24) : (uint8_t)("triforce "[i % 9]);
    }
}

static void round_trip(size_t n, uint32_t seed) {
    uint8_t *src = malloc(n + 1), *back = malloc(n + 1);
    fill(src, n, seed);
    size_t packed_n = 0;
    uint8_t *packed = sz_deflate(src, n, &packed_n);
    CHECK(packed != NULL);
    CHECK(packed && sz_inflate(packed, packed_n, back, n) == 0 && !memcmp(src, back, n));
    CHECK(packed && sz_inflate(packed, packed_n, back, n + 1) != 0);        /* (not the size it holds) */
    CHECK(!packed || !n || sz_inflate(packed, packed_n, back, n - 1) != 0);
    CHECK(!packed || packed_n <= 4 || sz_inflate(packed, packed_n - 3, back, n) != 0);   /* (cut short) */
    free(packed), free(src), free(back);
}

static int selftest(void) {
    CHECK(sz_crc32(0, "123456789", 9) == 0xCBF43926u);   /* (the CRC-32's check value) */
    CHECK(sz_crc32(sz_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
    CHECK(sz_crc32(0, "", 0) == 0);
    static const size_t sizes[] = {0, 1, 2, 3, 63, 64, 65, 258, 259, 32768, 32769, 70000, 300000};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) round_trip(sizes[i], (uint32_t)(i * 7919 + 1));
    {   /* a run longer than a match and its window: a megabyte of one byte */
        const size_t n = 1 << 20;
        uint8_t *src = calloc(1, n), *back = malloc(n);
        size_t packed_n = 0;
        uint8_t *packed = sz_deflate(src, n, &packed_n);
        CHECK(packed && packed_n < n / 50 && sz_inflate(packed, packed_n, back, n) == 0 && !memcmp(src, back, n));
        free(packed), free(src), free(back);
    }
    {   /* the writer and the reader, a member of each kind */
        SzWriter w;
        sz_writer_init(&w, 2026, 11, 15, 14, 22, 34);
        uint8_t *big = malloc(100000);
        fill(big, 100000, 5);
        sz_writer_add(&w, "a.txt", "hello", 5);                 /* (short: stored) */
        sz_writer_add(&w, "empty", "", 0);
        sz_writer_add(&w, "dir/big.bin", big, 100000);          /* (deflated) */
        sz_writer_add(&w, "na\xc3\xafve/\xe3\x82\xbb\xe3\x83\xbc\xe3\x83\x96.sav", "utf8", 4);
        size_t zn = 0;
        uint8_t *zip = sz_writer_finish(&w, &zn);
        CHECK(zip != NULL);
        SzEntry *e = NULL;
        int count = 0, err = 0;
        CHECK(zip && sz_read_dir(zip, zn, &e, &count) == SZ_OK && count == 4);
        if (zip && e && count == 4) {
            CHECK(!strcmp(e[0].name, "a.txt") && e[0].method == 0 && e[0].usize == 5);
            CHECK(e[1].usize == 0 && e[1].csize == 0);
            CHECK(e[2].method == 8 && e[2].csize < e[2].usize && e[2].crc == sz_crc32(0, big, 100000));
            CHECK(!strcmp(e[3].name, "na\xc3\xafve/\xe3\x82\xbb\xe3\x83\xbc\xe3\x83\x96.sav") && !e[3].is_dir && !e[3].is_link);
            uint8_t *got = sz_extract(zip, zn, &e[2], &err);
            CHECK(got && err == SZ_OK && !memcmp(got, big, 100000) && got[100000] == 0);
            free(got);
            /* a byte of a member's data changed: its CRC-32 catches it; the others still come out */
            zip[e[0].data] ^= 0x20;
            CHECK(!sz_extract(zip, zn, &e[0], &err) && err == SZ_E_DAMAGED);
            got = sz_extract(zip, zn, &e[3], &err);
            CHECK(got && !memcmp(got, "utf8", 4));
            free(got);
            zip[e[0].data] ^= 0x20;
            zip[e[2].data + 40] ^= 0x01;   /* (and inside deflated data) */
            CHECK(!sz_extract(zip, zn, &e[2], &err) && err == SZ_E_DAMAGED);
        }
        free(e);
        /* cut short anywhere: never read as a whole zip */
        for (size_t cut = 0; zip && cut < zn; cut += zn / 37 + 1) CHECK(sz_read_dir(zip, cut, &e, &count) != SZ_OK);
        CHECK(sz_read_dir((const uint8_t *)"PK\x05\x06", 4, &e, &count) == SZ_E_NOT_ZIP);
        free(zip), free(big);
    }
    {   /* the manifest's JSON */
        const char *j = "{ \"format\": \"triforce-save\", \"n\": 12, \"neg\": -3, \"deep\": {\"format\": \"no\", \"a\": [1, \"}\"]},\n"
                        " \"esc\": \"a\\\"b\\\\c\\u0041\\u00e9\", \"f\": 1.5, \"t\": true }";
        char s[64];
        long long v = 0;
        CHECK(sz_json_str(j, "format", s, sizeof s) && !strcmp(s, "triforce-save"));
        CHECK(sz_json_int(j, "n", &v) && v == 12 && sz_json_int(j, "neg", &v) && v == -3);
        CHECK(sz_json_str(j, "esc", s, sizeof s) && !strcmp(s, "a\"b\\cA?"));
        CHECK(!sz_json_str(j, "a", s, sizeof s) && !sz_json_int(j, "f", &v) && !sz_json_int(j, "format", &v));
        CHECK(!sz_json_str(j, "missing", s, sizeof s) && !sz_json_str("[1]", "format", s, sizeof s) && !sz_json_str("{\"format\": \"x", "format", s, sizeof s));
        CHECK(!strcmp(sz_json_escape("v0.4.0 \"dev\"\\\n", s, sizeof s), "v0.4.0 \\\"dev\\\"\\\\\\u000a"));
    }
    {   /* which paths a save's files may have */
        static const char *const good[] = {"account.sav", "option.sav", "slot_00/progress.sav", "slot_05/caption.sav", "a/b/c/d.sav", "x-1_2.sav"};
        static const char *const bad[] = {"", "/account.sav", "../account.sav", "slot_00/../../x", "slot_00//x", "slot_00/", "a\\b", "c:x", "C:/x",
                                          ".hidden", "x.", "a/./b", "nul", "NUL.sav", "slot_00/con", "com1", "LPT9.txt", "aux.sav", "a b", "a/b/c/d/e",
                                          "caf\xc3\xa9.sav", "a:b", "x\ty", "prn"};
        for (size_t i = 0; i < sizeof good / sizeof *good; i++)
            if (!save_pack_safe_rel(good[i])) printf("FAIL: \"%s\" is refused\n", good[i]), fails++;
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
            if (save_pack_safe_rel(bad[i])) printf("FAIL: \"%s\" is taken\n", bad[i]), fails++;
        char longer[140];
        memset(longer, 'a', sizeof longer - 1), longer[sizeof longer - 1] = 0;
        CHECK(!save_pack_safe_rel(longer));
        CHECK(save_pack_save_file("account.sav") && save_pack_save_file("option.sav") && save_pack_save_file("slot_03/progress.sav"));
        CHECK(!save_pack_save_file("slot_03/caption.sav") && !save_pack_save_file("slot_3/progress.sav") && !save_pack_save_file("progress.sav"));
    }
    {   /* an export opened again: the same files, and what is not a save's is left out or refused */
        SavePack p;
        time_t now = 1794752554;
        struct tm t = *gmtime(&now);
        uint8_t *slot = malloc(120556);
        fill(slot, 120556, 9);
        save_pack_begin(&p, &t);
        CHECK(save_pack_add_save(&p, "account.sav", "acc", 3) && save_pack_add_save(&p, "slot_00/progress.sav", slot, 120556));
        CHECK(save_pack_add_save(&p, "prod.keys", "secret", 6) && p.files == 2);   /* (a key file: never in a zip) */
        CHECK(!save_pack_add_save(&p, "../x.sav", "x", 1) && p.files == 2);
        save_pack_add_report(&p, "triforce_log.txt", "a log", 5);
        size_t zn = 0;
        uint8_t *zip = save_pack_end(&p, "v0.4.0 (source abc)", &t, 1, &zn);
        SavePackSave s;
        CHECK(zip && save_pack_open(zip, zn, &s) == SAVE_PACK_OK && s.count == 2);
        if (zip && s.count == 2) {
            CHECK(!strcmp(s.rels[0], "account.sav") && s.sizes[0] == 3 && !memcmp(s.data[0], "acc", 3));
            CHECK(!strcmp(s.rels[1], "slot_00/progress.sav") && s.sizes[1] == 120556 && !memcmp(s.data[1], slot, 120556));
            CHECK(!strcmp(s.made_by, "Triforce v0.4.0 (source abc)") && !strncmp(s.exported, "20", 2) && strlen(s.exported) == 10);
        }
        save_pack_free(&s);
        CHECK(s.count == 0 && !s.rels && !s.data);
        if (zip) {   /* (every byte of it damaged in turn: opened as it was, or refused; never something else) */
            int taken = 0;
            for (size_t at = 0; at < zn; at += 211) {
                zip[at] ^= 0x5a;
                const SavePackResult r = save_pack_open(zip, zn, &s);
                if (r == SAVE_PACK_OK) {
                    taken++;
                    CHECK(s.count == 2 && !strcmp(s.rels[0], "account.sav") && !strcmp(s.rels[1], "slot_00/progress.sav"));
                    CHECK(s.count == 2 && s.sizes[1] == 120556 && !memcmp(s.data[1], slot, 120556) && !memcmp(s.data[0], "acc", 3));
                }
                save_pack_free(&s);
                zip[at] ^= 0x5a;
            }
            printf("damaged in %zu places: %d still opened whole (a byte outside the save's members and the manifest's keys)\n", (zn + 210) / 211, taken);
        }
        free(zip), free(slot);
        CHECK(save_pack_open((const uint8_t *)"not a zip at all, just text", 27, &s) == SAVE_PACK_NOT_A_SAVE && s.count == 0);
        CHECK(save_pack_open((const uint8_t *)"", 0, &s) == SAVE_PACK_NOT_A_SAVE);
    }
    if (!fails) printf("PASS\n");
    return fails != 0;
}

/* a list file: one "<member name>\t<data file>" a line */
static int each_line(const char *list, int (*fn)(void *arg, const char *name, const uint8_t *data, size_t n), void *arg) {
    size_t ln = 0;
    char *text = (char *)slurp(list, &ln);
    if (!text) return 0;
    int ok = 1;
    for (char *line = strtok(text, "\r\n"); line && ok; line = strtok(NULL, "\r\n")) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        size_t n = 0;
        uint8_t *data = slurp(tab + 1, &n);
        ok = data && fn(arg, line, data, n);
        free(data);
    }
    free(text);
    return ok;
}

static int add_member(void *arg, const char *name, const uint8_t *data, size_t n) {
    sz_writer_add((SzWriter *)arg, name, data, n);
    return 1;
}

static int add_export(void *arg, const char *name, const uint8_t *data, size_t n) {
    if (!strncmp(name, "report:", 7)) save_pack_add_report((SavePack *)arg, name + 7, data, n);
    else if (!save_pack_add_save((SavePack *)arg, name, data, n)) printf("left out: %s\n", name);
    return 1;
}

static const char *const results[] = {"OK", "NO_MEMORY", "NOT_A_SAVE", "OTHER_GAME", "NEWER", "DAMAGED", "UNSAFE"};

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "selftest")) return selftest();
    if (argc == 4 && !strcmp(argv[1], "write")) {   /* write <list> <out.zip> */
        SzWriter w;
        sz_writer_init(&w, 2026, 11, 15, 14, 22, 34);
        if (!each_line(argv[2], add_member, &w)) return 2;
        size_t n = 0;
        uint8_t *zip = sz_writer_finish(&w, &n);
        return zip && spill(argv[3], zip, n) ? 0 : 3;
    }
    if (argc == 4 && !strcmp(argv[1], "list")) {   /* list <zip> <out folder>: each member's line, its bytes as <index>.bin */
        size_t n = 0;
        uint8_t *zip = slurp(argv[2], &n);
        SzEntry *e = NULL;
        int count = 0;
        const int r = zip ? sz_read_dir(zip, n, &e, &count) : -1;
        printf("dir %d %d\n", r, count);
        for (int i = 0; r == SZ_OK && i < count; i++) {
            int err = 0;
            uint8_t *data = e[i].is_dir ? NULL : sz_extract(zip, n, &e[i], &err);
            char out[1024];
            snprintf(out, sizeof out, "%s/%d.bin", argv[3], i);
            if (data) spill(out, data, (size_t)e[i].usize);
            printf("%d\t%u\t%llu\t%08x\t%d\t%d\t%d\t%s\n", i, e[i].method, (unsigned long long)e[i].usize, e[i].crc, e[i].is_dir, e[i].is_link,
                   e[i].is_dir ? 0 : data ? 1 : -err, e[i].name);
            free(data);
        }
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "deflate")) {   /* deflate <in> <out>: raw deflate */
        size_t n = 0, pn = 0;
        uint8_t *in = slurp(argv[2], &n), *packed = in ? sz_deflate(in, n, &pn) : NULL;
        return packed && spill(argv[3], packed, pn) ? 0 : 2;
    }
    if (argc == 5 && !strcmp(argv[1], "inflate")) {   /* inflate <in> <out> <size> */
        size_t n = 0;
        const size_t size = (size_t)strtoull(argv[4], NULL, 10);
        uint8_t *in = slurp(argv[2], &n), *out = malloc(size + 1);
        if (!in || sz_inflate(in, n, out, size)) return 2;
        return spill(argv[3], out, size) ? 0 : 3;
    }
    if (argc == 5 && !strcmp(argv[1], "export")) {   /* export <list> <out.zip> <bug report 0|1> */
        SavePack p;
        time_t now = 1794752554;   /* 2026-11-15 14:22:34 UTC */
        struct tm t = *gmtime(&now);
        save_pack_begin(&p, &t);
        if (!each_line(argv[2], add_export, &p)) return 2;
        size_t n = 0;
        uint8_t *zip = save_pack_end(&p, "v0.4.0 \"test\"", &t, atoi(argv[4]), &n);
        printf("files %d\n", p.files);
        return zip && spill(argv[3], zip, n) ? 0 : 3;
    }
    if (argc == 4 && !strcmp(argv[1], "open")) {   /* open <zip> <out folder>: the verdict, and the save's files as <index>.bin */
        size_t n = 0;
        uint8_t *zip = slurp(argv[2], &n);
        if (!zip) return 2;
        SavePackSave s;
        const SavePackResult r = save_pack_open(zip, n, &s);
        printf("%s\t%s\t%s\t%d\t%s\n", results[r], s.made_by, s.exported, s.count, save_pack_why(r));
        for (int i = 0; i < s.count; i++) {
            char out[1024];
            snprintf(out, sizeof out, "%s/%d.bin", argv[3], i);
            spill(out, s.data[i], s.sizes[i]);
            printf("%d\t%zu\t%s\n", i, s.sizes[i], s.rels[i]);
        }
        save_pack_free(&s);
        return 0;
    }
    fprintf(stderr, "usage: selftest | write <list> <zip> | list <zip> <dir> | deflate <in> <out> | inflate <in> <out> <size> | "
                    "export <list> <zip> <0|1> | open <zip> <dir>\n");
    return 2;
}
