/* (triforce) Sharing a save from the System tab, after NX2navi's menu rows (its docs/launcher.md, "Sharing a save"): Export save
   writes the save as a zip into the player's Downloads folder, Export save for a bug report adds the logs, the settings and
   the freeze report to it, and Import a save takes such a zip (Triforce's or NX2navi's) from the game folder's save-import
   folder and leaves its save to wait for the next start, where it becomes the save with the one it replaces kept first
   (host/save_tx_share.h, host/nn_fs.c). No game file goes into a zip.

   The rows are the renderer's (gpu/vk/vk_menu.cpp), the work is done here, on one thread of this file's own, so the game
   and its picture go on meanwhile: the thread asks the menu ten times a second what was chosen (rx_option
   HOS_OPT_SHARE_ASK), does it, and tells the row what to show under its help (HOS_OPT_SHARE_TEXT a byte at a time, then
   HOS_OPT_SHARE_SAID). It starts with the game's first save mount (nn_fs.c). The bug capture (host/bug_capture.c) is
   written on the same thread, and its save is this file's export.

   The save in a zip is the one the next start would load, read with no Commit under way (hos_fs_save_read): the files of
   one commit, never the working copy the game is writing. With HOS_SAVE_TX=0 the game writes its save in place and
   nothing is exported: a copy could catch a file half written.

   HOS_SAVE_EXPORT_DIR=<folder>: where the zips go, in place of the Downloads folder (a test's, or a player's own). */
#include "hos_paths.h"   /* (first: hos_game_path_a, and windows.h on Windows) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../gpu/vk/vk_api.h"
#include "save_pack.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif
#define SHARE_PATH 1024
#define LOG_MAX (16u << 20)   /* (of a log in a bug report: more than that is a trace run's, cut in its middle) */

void hos_log(const char *fmt, ...);
void hos_log_flush(void);
int rx_option(int id, int otherwise);
void rx_option_set(int id, int value);
const char *hos_version(void);   /* hos_main.c */
/* nn_fs.c */
int hos_fs_save_read(int (*fn)(void *arg, const char *rel, const void *data, size_t n), void *arg);
int hos_fs_save_import_stage(int n, const char *const *rels, const uint8_t *const *data, const size_t *sizes);
int hos_fs_save_import_waits(void);
int hos_fs_save_import_cancel(void);
int hos_fs_save_import_note(char *out, size_t cap);
/* bug_capture.c */
int hos_bug_capture_ask(const char *trigger);
void hos_bug_capture_key(void);
void hos_bug_capture_tick(void);
void hos_bug_capture_work(void);

/* The game folder (the log, the settings and the saves are in it: hos_paths.h), with no separator at its end. 0: unknown */
int hos_share_game_dir(char *out, size_t cap) {
    char exe[MAX_PATH];
    DWORD n = hos_game_path_a(exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    while (n && exe[n - 1] != '\\' && exe[n - 1] != '/') n--;
    if (n < 2) return 0;
    exe[n - 1] = 0;
    return snprintf(out, cap, "%s", exe) < (int)cap;
}

static int is_dir(const char *path) {
    const DWORD at = GetFileAttributesA(path);
    return at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
}

/* The player's Downloads folder. 0: not known, or not there */
static int downloads_dir(char *out, size_t cap) {
    const char *forced = getenv("HOS_SAVE_EXPORT_DIR");
    if (forced && *forced) return snprintf(out, cap, "%s", forced) < (int)cap;
#ifdef _WIN32
    /* (Windows' known folder: a player may have moved Downloads to another drive. shell32 and ole32 are looked up here, so
       the game links neither) */
    static const struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } downloads = {   /* FOLDERID_Downloads */
        0x374DE290, 0x123F, 0x4565, {0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B}};
    typedef HRESULT (WINAPI *known_fn)(const void *, DWORD, HANDLE, wchar_t **);
    typedef void (WINAPI *free_fn)(void *);
    HMODULE shell = LoadLibraryExA("shell32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE ole = LoadLibraryExA("ole32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    known_fn known = shell ? (known_fn)(void *)GetProcAddress(shell, "SHGetKnownFolderPath") : NULL;
    free_fn co_free = ole ? (free_fn)(void *)GetProcAddress(ole, "CoTaskMemFree") : NULL;
    wchar_t *wide = NULL;
    int ok = 0;
    if (known && co_free && known(&downloads, 0, NULL, &wide) >= 0 && wide)
        ok = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL) > 0;
    if (wide && co_free) co_free(wide);
    return ok && is_dir(out);
#else
    const char *home = getenv("HOME");
    return home && home[0] == '/' && snprintf(out, cap, "%s/Downloads", home) < (int)cap && is_dir(out);
#endif
}

/* A file whole: malloc'd, *n its size, NULL when it is not there. One longer than max keeps its first sixteenth and its
   end, with a line between them that says what was left out (a log: its start names the build and the GPU, its end has
   what happened) */
static uint8_t *read_file(const char *path, size_t max, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    _fseeki64(f, 0, SEEK_END);
    const long long size = _ftelli64(f);
    uint8_t *data = size >= 0 ? (uint8_t *)malloc((size_t)(size > (long long)max ? max : size) + 96) : NULL;
    size_t got = 0;
    if (data && size <= (long long)max) {
        _fseeki64(f, 0, SEEK_SET);
        got = fread(data, 1, (size_t)size, f);
    } else if (data) {
        const size_t head = max / 16, tail = max - head;
        _fseeki64(f, 0, SEEK_SET);
        got = fread(data, 1, head, f);
        got += (size_t)snprintf((char *)data + got, 96, "\n[... %lld bytes left out here ...]\n", size - (long long)max);
        _fseeki64(f, size - (long long)tail, SEEK_SET);
        got += fread(data + got, 1, tail, f);
    }
    fclose(f);
    *n = got;
    return data;
}

/* ---- the save's files, kept while nn_fs.c holds its lock and packed after ---- */
typedef struct {
    char rel[104];
    uint8_t *data;
    size_t n;
} SaveFile;
typedef struct {
    SaveFile *f;
    int count, cap, left_out;
} SaveFiles;

static int keep_file(void *arg, const char *rel, const void *data, size_t n) {
    SaveFiles *s = (SaveFiles *)arg;
    if (!save_pack_safe_rel(rel)) {   /* (not a name a save's file has: something of the player's own in the folder) */
        s->left_out++;
        return 1;
    }
    if (s->count == s->cap) {
        const int cap = s->cap ? s->cap * 2 : 32;
        SaveFile *f = (SaveFile *)realloc(s->f, (size_t)cap * sizeof *f);
        if (!f) return 0;
        s->f = f, s->cap = cap;
    }
    SaveFile *f = &s->f[s->count];
    f->data = (uint8_t *)malloc(n ? n : 1);
    if (!f->data) return 0;
    memcpy(f->data, data, n);
    f->n = n;
    snprintf(f->rel, sizeof f->rel, "%s", rel);
    s->count++;
    return 1;
}

static void files_free(SaveFiles *s) {
    for (int i = 0; i < s->count; i++) free(s->f[i].data);
    free(s->f);
    memset(s, 0, sizeof *s);
}

/* The zip's bytes written as <path>.part and renamed when whole: nothing of that name is half a zip. 1: written */
static int write_whole(const char *path, const uint8_t *data, size_t n) {
    char part[SHARE_PATH + 8];
    snprintf(part, sizeof part, "%s.part", path);
    FILE *f = fopen(part, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    if (fflush(f)) ok = 0;
    if (fclose(f)) ok = 0;
    if (ok && !MoveFileExA(part, path, 0)) ok = 0;
    if (!ok) DeleteFileA(part);
    return ok;
}

/* The save the next start would load as Triforce-save-<date>-<time>.zip in dir (made when missing), with the logs, the
   settings and the freeze report in a bug report's. 1: written, its path in path; 0: nothing written. *files: the save's
   files in it, or why it has none: 0 the game has not saved yet, -1 one could not be read, -2 the save is written in
   place in this run (hos_fs_save_read). A bug report is written without a save too; a plain export is not */
int hos_save_export(const char *dir, int bug_report, char *path, size_t cap, int *files) {
    path[0] = 0;
    SaveFiles kept;
    memset(&kept, 0, sizeof kept);
    *files = hos_fs_save_read(keep_file, &kept);
    if (*files > 0) *files = kept.count;
    if (*files <= 0 && !bug_report) {
        files_free(&kept);
        return 0;
    }
    const time_t now = time(NULL);
    struct tm local, utc;
    localtime_s(&local, &now);
    gmtime_s(&utc, &now);
    SavePack pack;
    save_pack_begin(&pack, &local);
    for (int i = 0; *files > 0 && i < kept.count; i++) save_pack_add_save(&pack, kept.f[i].rel, kept.f[i].data, kept.f[i].n);
    const int left_out = kept.left_out;
    files_free(&kept);
    char game[MAX_PATH], file[MAX_PATH + 40];
    if (bug_report && hos_share_game_dir(game, sizeof game)) {
        static const char *const names[] = {"triforce_log.txt", "triforce_log.1.txt", "triforce_options.ini", "triforce_hang.txt"};
        hos_log_flush();   /* (the lines queued so far: the log in the zip ends where the export began) */
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
            size_t n = 0;
            snprintf(file, sizeof file, "%s" SEP "%s", game, names[i]);
            uint8_t *data = read_file(file, LOG_MAX, &n);
            if (data) save_pack_add_report(&pack, names[i], data, n);
            free(data);
        }
    }
    size_t zn = 0;
    uint8_t *zip = save_pack_end(&pack, hos_version(), &utc, bug_report, &zn);
    if (!zip) {
        hos_log("save: export: no memory for the zip");
        return 0;
    }
    CreateDirectoryA(dir, NULL);
    char stem[64];
    snprintf(stem, sizeof stem, "Triforce-save-%04d%02d%02d-%02d%02d%02d%s", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec, bug_report ? "-bug-report" : "");
    int ok = 0;
    for (int k = 1; k < 100 && !ok; k++) {   /* (two exports in one second: "(2)") */
        const int len = k == 1 ? snprintf(path, cap, "%s" SEP "%s.zip", dir, stem) : snprintf(path, cap, "%s" SEP "%s (%d).zip", dir, stem, k);
        if (len < 0 || (size_t)len >= cap || len >= SHARE_PATH) break;
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) continue;
        ok = write_whole(path, zip, zn);
        if (!ok) break;
    }
    free(zip);
    if (!ok) {
        hos_log("save: export: the zip could not be written into %s", dir);
        path[0] = 0;
        return 0;
    }
    hos_log("save: exported %d save files%s to %s (%zu bytes%s)", *files > 0 ? *files : 0, bug_report ? " with the logs and the settings" : "",
            path, zn, left_out ? "; files that are not a save's were left out" : "");
    return 1;
}

/* ---- what the rows show: a line under the row's help ---- */
static void say(int row, const char *text) {
    for (const char *p = text; *p; p++) rx_option_set(HOS_OPT_SHARE_TEXT, (unsigned char)*p);
    rx_option_set(HOS_OPT_SHARE_SAID, row);
}

void hos_save_share_say(int row, const char *text) { say(row, text); }   /* (bug_capture.c's row, and its notice) */

static void do_export(int bug_report) {
    const int row = bug_report ? HOS_SHARE_EXPORT_BUG : HOS_SHARE_EXPORT;
    char dir[SHARE_PATH], path[SHARE_PATH], line[SHARE_PATH + 200];
    int files = 0;
    const int downloads = downloads_dir(dir, sizeof dir);
    int ok = (downloads || hos_share_game_dir(dir, sizeof dir)) && hos_save_export(dir, bug_report, path, sizeof path, &files);
    if (!ok && downloads && (files > 0 || bug_report) && hos_share_game_dir(dir, sizeof dir))   /* (Downloads cannot be written: beside the game) */
        ok = hos_save_export(dir, bug_report, path, sizeof path, &files);
    if (ok && files > 0) snprintf(line, sizeof line, "Saved as %s", path);
    else if (ok) snprintf(line, sizeof line, "Saved as %s: the logs and the settings, without a save (%s).", path,
                          files == 0 ? "the game has not saved yet" : files == -1 ? "it could not be read" : "it is written in place in this run");
    else if (files == 0) snprintf(line, sizeof line, "There is no save yet: save in the game first.");
    else if (files == -1) snprintf(line, sizeof line, "A file of the save could not be read. Nothing was written.");
    else if (files == -2)
        snprintf(line, sizeof line, "Not in this run: the save is written in place (HOS_SAVE_TX=0), and a copy could catch a file half written.");
    else snprintf(line, sizeof line, "The zip could not be written into %s", dir);
    say(row, line);
}

/* The zip to import: the newest .zip of dir (by its time, then by its name: an export's name has its date). 1: its name in name */
static int find_zip(const char *dir, char *name, size_t cap) {
    char pat[SHARE_PATH];
    WIN32_FIND_DATAA fd;
    uint64_t best = 0;
    name[0] = 0;
    snprintf(pat, sizeof pat, "%s" SEP "*.zip", dir);
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const uint64_t when = (uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
        if (name[0] && (when < best || (when == best && strcmp(fd.cFileName, name) <= 0))) continue;
        if (strlen(fd.cFileName) >= cap) continue;
        best = when;
        snprintf(name, cap, "%s", fd.cFileName);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    return name[0] != 0;
}

static void do_import(void) {
    char game[MAX_PATH], dir[MAX_PATH + 16], name[260], path[SHARE_PATH], line[SHARE_PATH + 400];
    if (!hos_share_game_dir(game, sizeof game)) {
        say(HOS_SHARE_IMPORT, "The game folder is not known: nothing was imported.");
        return;
    }
    snprintf(dir, sizeof dir, "%s" SEP "save-import", game);
    CreateDirectoryA(dir, NULL);   /* (there for the player to find) */
    if (!find_zip(dir, name, sizeof name) || snprintf(path, sizeof path, "%s" SEP "%s", dir, name) >= (int)sizeof path) {
        snprintf(line, sizeof line, "Put a save export of Triforce or NX2navi (a .zip) into %s first, then choose this row again.", dir);
        say(HOS_SHARE_IMPORT, line);
        return;
    }
    size_t n = 0;
    uint8_t *zip = read_file(path, SAVE_PACK_MAX_ZIP + 1, &n);   /* (a byte past the most a save's zip may have: refused as too large) */
    const int was_read = zip != NULL;
    SavePackSave save;
    memset(&save, 0, sizeof save);
    const SavePackResult r = was_read ? save_pack_open(zip, n, &save) : SAVE_PACK_DAMAGED;
    free(zip);
    if (r != SAVE_PACK_OK) {   /* (refused whole: nothing of it was written anywhere) */
        hos_log("save: import of %s refused: %s", path, was_read ? save_pack_why(r) : "It could not be read.");
        snprintf(line, sizeof line, "%s was not imported. %s", name, was_read ? save_pack_why(r) : "It could not be read.");
        say(HOS_SHARE_IMPORT, line);
        return;
    }
    const int staged = hos_fs_save_import_stage(save.count, (const char *const *)save.rels, (const uint8_t *const *)save.data, save.sizes);
    const int count = save.count;
    char made[sizeof save.made_by], when[sizeof save.exported];
    memcpy(made, save.made_by, sizeof made), memcpy(when, save.exported, sizeof when);
    save_pack_free(&save);
    if (staged != 1) {
        hos_log("save: import of %s: %s", path, staged == -2 ? "no save folder to put it under in this run" : "its files could not be written");
        snprintf(line, sizeof line,
                 staged == -2 ? "%s was not imported: the game has no save folder yet, or writes its save in place in this run (HOS_SAVE_TX=0)."
                              : "%s was not imported: its files could not be written to wait for the next start.", name);
        say(HOS_SHARE_IMPORT, line);
        return;
    }
    /* (the zip out of the way, so the folder holds only what is still to import; kept, for the player) */
    char done_dir[MAX_PATH + 32], done[SHARE_PATH];
    snprintf(done_dir, sizeof done_dir, "%s" SEP "imported", dir);
    CreateDirectoryA(done_dir, NULL);
    snprintf(done, sizeof done, "%s" SEP "%s", done_dir, name);
    const int moved = MoveFileExA(path, done, MOVEFILE_REPLACE_EXISTING) != 0;
    rx_option_set(HOS_OPT_SHARE_IMPORT, 1);
    hos_log("save: import: %d files of %s (%s, exported %s) wait for the next start%s", count, path, made, when,
            moved ? "; the zip went to the imported folder" : "");
    snprintf(line, sizeof line, "%s (%s, exported %s) becomes your save at the next start: restart the game. The save it replaces is "
             "kept first, in the user folder's save-backup. Choose this row again to cancel.", name, made, when);
    say(HOS_SHARE_IMPORT, line);
}

static void do_import_cancel(void) {
    const int gone = hos_fs_save_import_cancel();
    rx_option_set(HOS_OPT_SHARE_IMPORT, gone ? 0 : 1);
    hos_log("save: import: %s", gone ? "cancelled, the save stays as it is" : "could not be cancelled (its folder is in use)");
    say(HOS_SHARE_IMPORT, gone ? "The import was cancelled: your save stays as it is. The zip is in the save-import folder's imported folder."
                               : "The import could not be cancelled: its folder is in use. Try again.");
}

/* ---- the thread ---- */
static HANDLE share_wake;   /* (set to look at once, without waiting for the next tenth of a second) */
static volatile LONG share_started;

static DWORD WINAPI share_main(LPVOID arg) {
    (void)arg;
    int told = 0;
    for (;;) {
        WaitForSingleObject(share_wake, 100);
        if (!told && rx_option(1, -1) >= 0) {   /* (the menu is there now: what the Import row shows from the start) */
            told = 1;
            char note[SHARE_PATH + 200];
            const int waits = hos_fs_save_import_waits();
            rx_option_set(HOS_OPT_SHARE_IMPORT, waits);
            if (hos_fs_save_import_note(note, sizeof note)) say(HOS_SHARE_IMPORT, note);
            else if (waits) say(HOS_SHARE_IMPORT, "An imported save waits for the next start. Choose this row to cancel it.");
        }
        hos_bug_capture_tick();   /* (the frames shown so far: a capture's frame rate) */
        switch (rx_option(HOS_OPT_SHARE_ASK, 0)) {
        case HOS_SHARE_EXPORT: do_export(0); break;
        case HOS_SHARE_EXPORT_BUG: do_export(1); break;
        case HOS_SHARE_IMPORT: do_import(); break;
        case HOS_SHARE_IMPORT_CANCEL: do_import_cancel(); break;
        case HOS_SHARE_CAPTURE:
            if (!hos_bug_capture_ask("the System tab's row")) say(HOS_SHARE_CAPTURE, "A capture is being written already.");
            break;
        case HOS_SHARE_CAPTURE_KEY: hos_bug_capture_key(); break;
        }
        hos_bug_capture_work();   /* (a capture asked for, by F8 or by its row: written here) */
    }
    return 0;
}

/* (bug_capture.c: F8 was pressed, not a tenth of a second later) */
void hos_save_share_wake(void) {
    if (share_wake) SetEvent(share_wake);
}

/* nn_fs.c, at the game's first save mount, and a bug capture before it: the rows answer from now on. Once */
void hos_save_share_start(void) {
    if (InterlockedCompareExchange(&share_started, 1, 0)) return;
    share_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, share_main, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else hos_log("save: the save rows' thread could not be started: Export and Import do nothing in this run");
}
