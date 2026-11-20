/* (triforce) What a save export holds and what an import takes (save_pack.c). The zip, as NX2navi's Export save writes its own
   (its docs/launcher.md, "Sharing a save"), so one reader takes both:

     manifest.json   {"format": "triforce-save", "format_version": 1, "game", "title_id", "triforce" (the version),
                      "exported" (UTC), "exported_by": "game", "files", "bug_report"}
     save/...        the save's files as the game keeps them (account.sav, option.sav, slot_00/progress.sav, ...)
     bug-report/...  with a bug report: the logs, triforce_options.ini and triforce_hang.txt; an import leaves them out

   An import takes a zip whose manifest names this format or NX2navi's ("nx2navi-save"), a version up to SAVE_PACK_VERSION
   and this game's title id; whose save/ paths are plain names that stay inside the save (save_pack_safe_rel), no links, no
   name twice, at most SAVE_PACK_MAX_FILES files and SAVE_PACK_MAX_BYTES, as many as the manifest counts; and that holds a
   file of this game's save. Every member's CRC-32 is checked as it is unpacked, all of them before the caller writes
   anything. Never a key file, either way.
   Plain C on the C library and save_zip.c: tests/test_save_zip.py builds the two alone. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "save_zip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAVE_PACK_FORMAT "triforce-save"
#define SAVE_PACK_FORMAT_NX2NAVI "nx2navi-save"
#define SAVE_PACK_VERSION 1
#define SAVE_PACK_TITLE_ID "0400486024dec000"   /* (the game's title id, as the manifests write it) */
#define SAVE_PACK_MAX_FILES 256                 /* (a save is 14 files) */
#define SAVE_PACK_MAX_BYTES (64u << 20)         /* (and about 1 MB) */
#define SAVE_PACK_MAX_ZIP (96u << 20)

/* ---- an export ---- */
typedef struct {
    SzWriter w;
    int files;   /* (the save's files added so far) */
} SavePack;
/* local: the time the members are stamped with */
void save_pack_begin(SavePack *p, const struct tm *local);
/* A file of the save, rel its path below the save folder with / between its parts. 1: added (or a key file, left out);
   0: the path is not one a save has, or no memory */
int save_pack_add_save(SavePack *p, const char *rel, const void *data, size_t n);
/* A file for a bug report, as bug-report/<name> */
void save_pack_add_report(SavePack *p, const char *name, const void *data, size_t n);
/* The manifest written and the zip finished: *n bytes the caller frees, NULL when something could not be added. version:
   the build's; utc: the export's time */
uint8_t *save_pack_end(SavePack *p, const char *version, const struct tm *utc, int bug_report, size_t *n);

/* ---- an import ---- */
typedef enum {
    SAVE_PACK_OK,
    SAVE_PACK_NO_MEMORY,
    SAVE_PACK_NOT_A_SAVE,   /* not a zip, no manifest, another format, or no file of this game's save in it */
    SAVE_PACK_OTHER_GAME,   /* the manifest names another game */
    SAVE_PACK_NEWER,        /* a format version this build does not know */
    SAVE_PACK_DAMAGED,      /* the zip's structure, a member's data or its CRC-32 is wrong */
    SAVE_PACK_UNSAFE,       /* a path outside the save, a link, a name twice, or far more than a save */
} SavePackResult;
typedef struct {
    int count;
    char **rels;         /* each file's path below the save folder, / between its parts */
    uint8_t **data;
    size_t *sizes;
    char made_by[96];    /* "Triforce v0.4.0" or "NX2navi 0.12.0", from the manifest ("?" for what it lacks) */
    char exported[32];   /* the export's day, as the manifest has it: 2026-11-15 */
} SavePackSave;
/* The save in the zip zip[0..n), checked whole. On SAVE_PACK_OK out holds its files (save_pack_free lets them go); otherwise
   out holds none, with made_by and exported filled when the manifest could be read */
SavePackResult save_pack_open(const uint8_t *zip, size_t n, SavePackSave *out);
void save_pack_free(SavePackSave *s);
/* Why an import was refused, as a sentence for the menu and the log */
const char *save_pack_why(SavePackResult r);

/* 1: rel is a path a save's file can have: one to four parts of ASCII letters, digits, '.', '_' and '-' with / between
   them, none starting or ending with a dot, none a Windows device name (NUL, COM1, "aux.sav"), 100 bytes at most */
int save_pack_safe_rel(const char *rel);
/* 1: a file only this game's save has (account.sav, option.sav, slot_NN/progress.sav) */
int save_pack_save_file(const char *rel);

#ifdef __cplusplus
}
#endif
