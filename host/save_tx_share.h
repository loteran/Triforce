/* (triforce) Sharing a save, the part on the save's own folders: reading the newest save whole for an export or a bug capture,
   and an imported save that waits for the next start. Included by nn_fs.c after save_tx.h (its helpers and its long-path file
   functions are used here); tests/test_save_share.py compiles the two alone.

   The newest save is what the next start would load: the highest ready-<n> (a finished commit not in place yet), else the
   committed save. nn_fs.c reads it with no Commit under way (its tx_lock held, the last Commit's thread ended), so every
   file comes from one commit: never the working copy the game is writing.

   An import from the System tab is not put in place while the game runs: the game keeps its progress in memory and writes
   it back at its next save, over files changed under it. Its files are written to <tx>/import-new and that folder renamed
   to <tx>/import when it is whole; Commits leave both alone. The first mount of the next run, after save_tx_mount put the
   disk right, copies the save about to be replaced to <backups>/import-<UTC date-time> (a name tx_backups_prune does not
   take: it stays until the player deletes it), renames import to the next ready-<n>, and mounts again: the import is then
   a finished commit, put in place as one, and the committed save it replaces becomes one of the usual backups as well. A
   start that dies in between leaves either the import still waiting (it is applied again, with a second backup) or a
   finished commit. Triforce Setup's Convert saves deletes <tx> after it replaced the save, and a waiting import with it:
   what was converted last is the save. */
#pragma once

#define TX_SHARE_FILE_MAX (64u << 20)   /* (a save file is some 100 KB: more than this is not one) */

static void tx_share_import_paths(const SaveTxPaths *p, char *waiting, char *writing, size_t cap) {
    snprintf(waiting, cap, "%s\\import", p->tx);
    snprintf(writing, cap, "%s\\import-new", p->tx);
}

/* 1: the folder holds a file only this game's save has (account.sav, option.sav, slot_00/progress.sav: what Triforce
   Setup takes for a save too) */
static int tx_share_is_save(const char *dir) {
    static const char *const files[] = {"account.sav", "option.sav", "slot_00\\progress.sav"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char path[HOS_FS_PATH];
        if (snprintf(path, sizeof path, "%s\\%s", dir, files[i]) < (int)sizeof path && tx_exists(path) && !tx_is_dir(path)) return 1;
    }
    return 0;
}

/* The save the next start loads, in out. 0: there is none yet */
static int tx_share_newest(const SaveTxPaths *p, char *out, size_t cap) {
    if (tx_ready_highest(p, out, cap) && tx_is_dir(out) && !tx_empty(out)) return 1;
    snprintf(out, cap, "%s", p->save);
    return tx_is_dir(out) && !tx_empty(out);
}

/* A file whole: malloc'd, *n its size. NULL when it cannot be read or is no save file's size */
static void *tx_share_read_file(const char *path, size_t *n) {
    HANDLE in = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (in == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    size.QuadPart = 0;
    char *data = NULL;
    if (GetFileSizeEx(in, &size) && size.QuadPart >= 0 && size.QuadPart <= (LONGLONG)TX_SHARE_FILE_MAX)
        data = (char *)malloc((size_t)size.QuadPart + 1);
    size_t done = 0;
    while (data && done < (size_t)size.QuadPart) {
        DWORD got = 0;
        const size_t left = (size_t)size.QuadPart - done;
        if (!ReadFile(in, data + done, (DWORD)(left > (1u << 20) ? (1u << 20) : left), &got, NULL) || !got) break;
        done += got;
    }
    CloseHandle(in);
    if (data && done != (size_t)size.QuadPart) free(data), data = NULL;   /* (shorter than it said: not read) */
    if (data) *n = done;
    return data;
}

/* Every file below dir given to fn whole, with its path below dir (rel: "" at the top; / between the parts). Links are
   left out, as tx_copy_tree leaves them. The files given, or -1: one could not be read, or fn answered 0 */
static int tx_share_read(const char *dir, const char *rel, int depth, int (*fn)(void *arg, const char *rel, const void *data, size_t n),
                         void *arg) {
    char pat[HOS_FS_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return tx_is_dir(dir) ? 0 : -1;
    int files = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..") || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        char sub[HOS_FS_PATH], name[HOS_FS_PATH];
        if (snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName) >= (int)sizeof sub ||
            snprintf(name, sizeof name, "%s%s%s", rel, rel[0] ? "/" : "", fd.cFileName) >= (int)sizeof name) {
            files = -1;
            break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            const int below = depth < 4 ? tx_share_read(sub, name, depth + 1, fn, arg) : -1;
            files = below < 0 ? -1 : files + below;
        } else {
            size_t n = 0;
            void *data = tx_share_read_file(sub, &n);
            const int taken = data && fn(arg, name, data, n);
            free(data);
            files = taken ? files + 1 : -1;
        }
    } while (files >= 0 && FindNextFileA(f, &fd));
    FindClose(f);
    return files;
}

/* One file of an import written out to the disk, its folders made. 1: written */
static int tx_share_write_file(const char *dir, const char *rel, const void *data, size_t n) {
    char path[HOS_FS_PATH];
    if (snprintf(path, sizeof path, "%s\\%s", dir, rel) >= (int)sizeof path) return 0;
    char *last = NULL;
    for (char *q = path + strlen(dir); *q; q++)
        if (*q == '/' || *q == '\\') *q = '\\', last = q;
    if (last && last > path + strlen(dir)) {   /* (a slot's folder) */
        *last = 0;
        tx_make_dirs(path);
        *last = '\\';
    }
    HANDLE out = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) return 0;
    int ok = 1;
    for (size_t done = 0; ok && done < n;) {
        DWORD put = 0;
        const size_t left = n - done;
        if (!WriteFile(out, (const char *)data + done, (DWORD)(left > (1u << 20) ? (1u << 20) : left), &put, NULL) || !put) ok = 0;
        done += put;
    }
    if (ok && !FlushFileBuffers(out)) ok = 0;
    CloseHandle(out);
    return ok;
}

static int tx_share_import_waits(const SaveTxPaths *p) {
    char waiting[HOS_FS_PATH], writing[HOS_FS_PATH];
    tx_share_import_paths(p, waiting, writing, sizeof waiting);
    return tx_is_dir(waiting);
}

/* Drops a waiting import. 1: none waits any more */
static int tx_share_import_cancel(const SaveTxPaths *p) {
    char waiting[HOS_FS_PATH], writing[HOS_FS_PATH];
    tx_share_import_paths(p, waiting, writing, sizeof waiting);
    if (tx_exists(writing)) tx_delete_tree(writing);
    if (tx_exists(waiting)) tx_delete_tree(waiting);
    return !tx_exists(waiting);
}

/* The n files of a checked import (rels: their paths below the save, / between the parts) written to wait for the next
   start, in place of one that waited. 1: it waits whole; 0: it could not be written, and what waited before still does */
static int tx_share_import_stage(const SaveTxPaths *p, int n, const char *const *rels, const uint8_t *const *data, const size_t *sizes) {
    char waiting[HOS_FS_PATH], writing[HOS_FS_PATH];
    tx_share_import_paths(p, waiting, writing, sizeof waiting);
    if (tx_exists(writing)) tx_delete_tree(writing);
    tx_make_dirs(writing);
    int ok = tx_is_dir(writing) && tx_empty(writing);
    for (int i = 0; ok && i < n; i++) ok = tx_share_write_file(writing, rels[i], data[i], sizes[i]);
    ok = ok && tx_share_is_save(writing);
    if (ok && tx_exists(waiting)) {   /* (the import before it: replaced by this one) */
        tx_delete_tree(waiting);
        ok = !tx_exists(waiting);
    }
    if (ok && !tx_move(writing, waiting, 3)) ok = 0;
    if (!ok) tx_delete_tree(writing);
    return ok;
}

/* The first mount of a run, after save_tx_mount: a waiting import made the newest save, the save it replaces copied to the
   backups first (its folder in backup when there was one to keep). 1: done, the import is the highest ready-<n> and
   save_tx_mount is to run again; 0: none waited; -1: the save could not be backed up or the import not renamed, it waits
   on and nothing changed; -2: what waited was no save and was dropped */
static int tx_share_import_apply(const SaveTxPaths *p, char *backup, size_t cap) {
    char waiting[HOS_FS_PATH], writing[HOS_FS_PATH], newest[HOS_FS_PATH];
    if (backup && cap) backup[0] = 0;
    tx_share_import_paths(p, waiting, writing, sizeof waiting);
    if (tx_exists(writing)) tx_delete_tree(writing);   /* (an import the run before did not finish writing: never whole) */
    if (!tx_exists(waiting)) return 0;
    if (!tx_is_dir(waiting) || !tx_share_is_save(waiting)) {
        tx_delete_tree(waiting);
        return -2;
    }
    if (tx_share_newest(p, newest, sizeof newest)) {
        struct tm t;
        const time_t now = tx_now();
#ifdef _WIN32
        gmtime_s(&t, &now);
#else
        gmtime_r(&now, &t);
#endif
        char stamp[64], part[HOS_FS_PATH], to[HOS_FS_PATH];
        strftime(stamp, sizeof stamp, "import-%Y%m%d-%H%M%S", &t);
        tx_make_dirs(p->backups);
        snprintf(part, sizeof part, "%s\\import-new", p->backups);
        snprintf(to, sizeof to, "%s\\%s", p->backups, stamp);
        for (int k = 2; tx_exists(to) && k < 100; k++) snprintf(to, sizeof to, "%s\\%s-%d", p->backups, stamp, k);
        if (tx_exists(part)) tx_delete_tree(part);   /* (a backup a start that died did not finish) */
        if (tx_exists(part) || tx_exists(to) || !tx_copy_tree(newest, part, 1) || tx_crashed() || !tx_move(part, to, 3)) {
            if (!save_tx_dead) tx_delete_tree(part);
            return -1;
        }
        if (backup) snprintf(backup, cap, "%s", to);
    }
    char ready[HOS_FS_PATH];
    snprintf(ready, sizeof ready, "%s\\ready-%u", p->tx, tx_ready_highest(p, NULL, 0) + 1);
    if (tx_crashed() || !tx_move(waiting, ready, 3)) return -1;
    return 1;
}
