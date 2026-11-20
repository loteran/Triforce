/* (triforce) The save as a transaction, with backups. Included by nn_fs.c after its file helpers (its CreateFileA and the rest
   are the long-path ones there); tests/test_save_tx.py compiles it alone.

   On the console the game writes its save files and then calls nn::fs::Commit: until then the old save is what a power cut
   leaves. Here the "save:" mount was the save folder itself and Commit did nothing, so a crash, a kill or a power cut in the
   middle of a save left a half-written one, and nothing kept an older save. NX2navi commits its save as a transaction and
   keeps backups (its runtime/fs_host.c, "save transactions": public domain); this is that design for Triforce's folders:

     <user>/save/<uid>/              the committed save: where it always was (Setup's Convert saves, the lab's save copies)
     <user>/save-tx/<uid>/work/      the working copy, the save mount's root; made from the committed save at each first mount
     <user>/save-tx/<uid>/new/       Commit's staging copy of the working copy
     <user>/save-tx/<uid>/ready-<n>/ a finished commit not swapped in yet: the newest save
     <user>/save-tx/<uid>/old/       the committed save a Commit replaced, on its way to the backups
     <user>/save-backup/<uid>/<UTC date-time>/   the saves Commits replaced: the newest SAVE_TX_BACKUPS of them

   Commit: (1) copy work to new, every file written out to the disk (FlushFileBuffers); (2) rename new to ready-<n>, the commit
   point, and delete the ready-<n> it supersedes; (3) rename save/<uid> to old; (4) rename ready-<n> to save/<uid>; (5) old
   becomes a backup when the run has made none yet or the last one is SAVE_TX_BACKUP_GAP_S old, else it is deleted. Windows
   refuses (3) while another program holds a file of the save open (a virus scanner, a sync tool): ready-<n> then stays the
   newest save, and the next Commit and the next start try (3) to (5) again. A step that fails before the commit point falls
   back to what was done before: the working copy's files copied over the save's.

   The first mount of a run (save_tx_mount) puts the disk right after a run that died: new (an unfinished copy) is deleted; the
   highest ready-<n> is swapped in; a save folder that is gone with old still there gets old back; then the working copy is
   made again from the committed save, so changes no Commit took are gone, as on the console.

   HOS_SAVE_TX=0: the save folder itself is the mount again and Commit does nothing, as before. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SAVE_TX_BACKUPS 5
#define SAVE_TX_BACKUP_GAP_S 300
#define SAVE_TX_MOVE_TRIES 10   /* (20 ms apart: 0.2 s at most on the game's thread) */

typedef struct {
    char save[HOS_FS_PATH], tx[HOS_FS_PATH], work[HOS_FS_PATH], staging[HOS_FS_PATH], old[HOS_FS_PATH], backups[HOS_FS_PATH];
} SaveTxPaths;

/* (tests: a Commit or a recovery stops before its save_tx_crash_at-th step, from 1, and leaves the disk as a process killed
   there would; save_tx_test_now stands for the clock) */
static int save_tx_crash_at, save_tx_step, save_tx_dead;
static time_t save_tx_test_now;
static time_t save_tx_last_backup;   /* (this run's last backup: 0 before the first) */

static int tx_crashed(void) {
    if (save_tx_crash_at && ++save_tx_step == save_tx_crash_at) save_tx_dead = 1;
    return save_tx_dead;
}
static time_t tx_now(void) { return save_tx_test_now ? save_tx_test_now : time(NULL); }
static int tx_exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static int tx_is_dir(const char *p) {
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

static void tx_make_dirs(const char *path) {   /* path and every parent of it */
    char p[HOS_FS_PATH];
    snprintf(p, sizeof p, "%s", path);
    for (char *q = p + 1; *q; q++)
        if ((*q == '\\' || *q == '/') && q[-1] != ':') {
            const char keep = *q;
            *q = 0;
            CreateDirectoryA(p, NULL);
            *q = keep;
        }
    CreateDirectoryA(p, NULL);
}

static void tx_delete_tree(const char *dir) {
    char pat[HOS_FS_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char sub[HOS_FS_PATH];
            snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
            /* (a link goes itself: never what it points to) */
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { if (!DeleteFileA(sub)) RemoveDirectoryA(sub); }
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) tx_delete_tree(sub);
            else DeleteFileA(sub);
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryA(dir);
}

static int tx_empty(const char *dir) {   /* 1: a folder with nothing in it */
    char pat[HOS_FS_PATH];
    WIN32_FIND_DATAA fd;
    int any = 0;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 1;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) any = 1;
    } while (!any && FindNextFileA(f, &fd));
    FindClose(f);
    return !any;
}

/* One file; the source may be open for writing (a save file the game has not closed). 1: copied */
static int tx_copy_file(const char *src, const char *dst, int flush) {
    HANDLE in = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (in == INVALID_HANDLE_VALUE) return 0;
    HANDLE out = CreateFileA(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        CloseHandle(in);
        return 0;
    }
    static const DWORD chunk = 1u << 16;
    char *buf = (char *)malloc(chunk);
    int ok = buf != NULL;
    while (ok) {
        DWORD got = 0, put = 0;
        if (!ReadFile(in, buf, chunk, &got, NULL)) ok = 0;
        else if (!got) break;
        else if (!WriteFile(out, buf, got, &put, NULL) || put != got) ok = 0;
    }
    free(buf);
    if (ok && flush && !FlushFileBuffers(out)) ok = 0;
    CloseHandle(out);
    CloseHandle(in);
    return ok;
}

/* A folder with everything in it, links left out; dst is made. 1: every file copied */
static int tx_copy_tree(const char *src, const char *dst, int flush) {
    tx_make_dirs(dst);
    if (!tx_is_dir(dst)) return 0;
    char pat[HOS_FS_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", src);
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return tx_is_dir(src);   /* (an empty folder has "." at least; a missing one is no copy) */
    int ok = 1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        char a[HOS_FS_PATH], b[HOS_FS_PATH];
        if (snprintf(a, sizeof a, "%s\\%s", src, fd.cFileName) >= (int)sizeof a ||
            snprintf(b, sizeof b, "%s\\%s", dst, fd.cFileName) >= (int)sizeof b) { ok = 0; continue; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok &= tx_copy_tree(a, b, flush);
        else ok &= tx_copy_file(a, b, flush);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    return ok;
}

/* Rename a folder; tried again while another program holds a file in it. 1: moved */
static int tx_move(const char *from, const char *to, int tries) {
    for (int i = 0; i < tries; i++) {
        if (MoveFileExA(from, to, 0)) return 1;
        if (i + 1 < tries) Sleep(20);
    }
    return 0;
}

/* The highest ready-<n> under tx (0: none), its path in out */
static unsigned tx_ready_highest(const SaveTxPaths *p, char *out, size_t cap) {
    char pat[HOS_FS_PATH];
    WIN32_FIND_DATAA fd;
    unsigned best = 0;
    snprintf(pat, sizeof pat, "%s\\ready-*", p->tx);
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        const char *digits = fd.cFileName + 6;
        if (strncmp(fd.cFileName, "ready-", 6) || !*digits || strlen(digits) > 9 || strspn(digits, "0123456789") != strlen(digits)) continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const unsigned n = (unsigned)strtoul(digits, NULL, 10);
        if (n > best) best = n;
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    if (best && out) snprintf(out, cap, "%s\\ready-%u", p->tx, best);
    return best;
}

static void tx_ready_delete_below(const SaveTxPaths *p, unsigned n) {
    char pat[HOS_FS_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\ready-*", p->tx);
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        const char *digits = fd.cFileName + 6;
        if (strncmp(fd.cFileName, "ready-", 6) || !*digits || strlen(digits) > 9 || strspn(digits, "0123456789") != strlen(digits)) continue;
        if ((unsigned)strtoul(digits, NULL, 10) >= n) continue;
        char sub[HOS_FS_PATH];
        snprintf(sub, sizeof sub, "%s\\%s", p->tx, fd.cFileName);
        tx_delete_tree(sub);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

static int tx_backup_name(const char *name) {   /* YYYYMMDD-HHMMSS, or that with -<k> after it: the names made here */
    if (strlen(name) < 15 || strspn(name, "0123456789") != 8 || name[8] != '-' || strspn(name + 9, "0123456789") != 6) return 0;
    return !name[15] || (name[15] == '-' && name[16] && strspn(name + 16, "0123456789") == strlen(name + 16));
}

/* Deletes the oldest backups past SAVE_TX_BACKUPS; only folders with the names made here */
static void tx_backups_prune(const SaveTxPaths *p) {
    for (;;) {
        char pat[HOS_FS_PATH], oldest[64] = "";
        WIN32_FIND_DATAA fd;
        int count = 0;
        snprintf(pat, sizeof pat, "%s\\*", p->backups);
        HANDLE f = FindFirstFileA(pat, &fd);
        if (f == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (!tx_backup_name(fd.cFileName) || strlen(fd.cFileName) >= sizeof oldest) continue;
            count++;
            /* (by the 15 characters of the time, then shorter first: "x" was made before "x-2") */
            const int c = strncmp(fd.cFileName, oldest, 15);
            if (!oldest[0] || c < 0 || (!c && (strlen(fd.cFileName) < strlen(oldest) ||
                                               (strlen(fd.cFileName) == strlen(oldest) && strcmp(fd.cFileName, oldest) < 0))))
                snprintf(oldest, sizeof oldest, "%s", fd.cFileName);
        } while (FindNextFileA(f, &fd));
        FindClose(f);
        if (count <= SAVE_TX_BACKUPS) return;
        char sub[HOS_FS_PATH];
        snprintf(sub, sizeof sub, "%s\\%s", p->backups, oldest);
        tx_delete_tree(sub);
        if (tx_exists(sub)) return;   /* (it cannot be deleted: no loop over it) */
    }
}

/* old, the save a Commit replaced: a backup when wanted (always: a recovery's), else deleted. The name of the backup in made */
static void tx_old_away(const SaveTxPaths *p, int always, char *made, size_t cap) {
    if (made && cap) made[0] = 0;
    if (!tx_exists(p->old)) return;
    const time_t now = tx_now();
    if (tx_empty(p->old)) {   /* (the folder a first mount made, before any save: nothing to keep) */
        tx_delete_tree(p->old);
        return;
    }
    if (always || !save_tx_last_backup || now - save_tx_last_backup >= SAVE_TX_BACKUP_GAP_S) {
        struct tm t;
        const time_t n = now;
#ifdef _WIN32
        gmtime_s(&t, &n);
#else
        gmtime_r(&n, &t);
#endif
        char name[64], to[HOS_FS_PATH];
        strftime(name, sizeof name, "%Y%m%d-%H%M%S", &t);
        tx_make_dirs(p->backups);
        snprintf(to, sizeof to, "%s\\%s", p->backups, name);
        for (int k = 2; tx_exists(to) && k < 100; k++) snprintf(to, sizeof to, "%s\\%s-%d", p->backups, name, k);
        if (tx_move(p->old, to, 3)) {
            save_tx_last_backup = now;
            if (made) snprintf(made, cap, "%s", to);
            tx_backups_prune(p);
            return;
        }
    }
    tx_delete_tree(p->old);
}

/* Steps (3) and (4): the finished commit at ready becomes the committed save. 1: swapped in */
static int tx_swap(const SaveTxPaths *p, const char *ready) {
    if (tx_exists(p->old)) tx_delete_tree(p->old);   /* (left by a swap that died before its backup step: already replaced) */
    if (tx_exists(p->save)) {
        if (tx_crashed()) return 0;
        if (!tx_move(p->save, p->old, SAVE_TX_MOVE_TRIES)) return 0;   /* (held open elsewhere: ready stays the newest save) */
    }
    if (tx_crashed()) return 0;
    if (!tx_move(ready, p->save, SAVE_TX_MOVE_TRIES)) {
        if (!tx_exists(p->save)) tx_move(p->old, p->save, SAVE_TX_MOVE_TRIES);   /* (the save back where it was) */
        return 0;
    }
    return 1;
}

static void save_tx_paths(SaveTxPaths *p, const char *user, const char *uid) {
    snprintf(p->save, sizeof p->save, "%s\\save\\%s", user, uid);
    snprintf(p->tx, sizeof p->tx, "%s\\save-tx\\%s", user, uid);
    snprintf(p->work, sizeof p->work, "%s\\save-tx\\%s\\work", user, uid);
    snprintf(p->staging, sizeof p->staging, "%s\\save-tx\\%s\\new", user, uid);
    snprintf(p->old, sizeof p->old, "%s\\save-tx\\%s\\old", user, uid);
    snprintf(p->backups, sizeof p->backups, "%s\\save-backup\\%s", user, uid);
}

static int save_tx_enabled(void) {
    const char *e = getenv("HOS_SAVE_TX");
    return !(e && e[0] == '0');
}

/* (Triforce) The save's lock between the game and Setup: <user>\save.lock, opened without sharing at the run's first save
   mount and held until the process ends (Windows lets go of it however the game ends). Setup's copy of a save (setup/Guard.cs
   SaveLock, setup/Saves.cs Copy) holds the same file across the whole copy: it found a running game by its process alone, so a
   game started after that look and before the copy's end mounted half a save and wrote it back over the copy when it closed.
   A game started while Setup copies waits here for the lock, up to wait_ms. 1: held; 0: another program held it all that time
   (the save is mounted anyway, as before, and the log says so); -1: it could not be made (nothing is locked: as before).
   HOS_SAVE_LOCK=0: none taken (2). */
#ifndef ERROR_SHARING_VIOLATION   /* (host/posix/windows.h has neither: no such refusal there, the lock is always had) */
#define ERROR_SHARING_VIOLATION 32L
#endif
#ifndef ERROR_LOCK_VIOLATION
#define ERROR_LOCK_VIOLATION 33L
#endif
static HANDLE save_tx_lock_handle = INVALID_HANDLE_VALUE;
static int save_tx_lock(const char *user, int wait_ms) {
    const char *e = getenv("HOS_SAVE_LOCK");
    if (e && e[0] == '0') return 2;
    if (save_tx_lock_handle != INVALID_HANDLE_VALUE) return 1;
    char p[HOS_FS_PATH];
    if (!user || snprintf(p, sizeof p, "%s\\save.lock", user) >= (int)sizeof p) return -1;
    tx_make_dirs(user);
    for (int waited = 0;; waited += 50) {
        const HANDLE h = CreateFileA(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            save_tx_lock_handle = h;
            return 1;
        }
        const DWORD why = GetLastError();
        if (why != ERROR_SHARING_VIOLATION && why != ERROR_LOCK_VIOLATION) return -1;
        if (waited >= wait_ms) return 0;
        Sleep(50);
    }
}

/* The first mount of the save in a run: the disk put right after a run that died, then the working copy made from the
   committed save. 1: mount p->work; 0: the working copy could not be made, mount p->save as before. note (may be NULL) gets
   a line for the log when the recovery found something to do */
static int save_tx_mount(const SaveTxPaths *p, char *note, size_t cap) {
    if (note && cap) note[0] = 0;
    tx_make_dirs(p->tx);
    if (tx_exists(p->staging)) {   /* (an unfinished copy: the commit never happened) */
        tx_delete_tree(p->staging);
        if (note) snprintf(note, cap, "an unfinished save was dropped (the run before ended in the middle of saving)");
    }
    char ready[HOS_FS_PATH];
    const unsigned n = tx_ready_highest(p, ready, sizeof ready);
    if (n) {   /* (a finished commit: the newest save) */
        const int in = tx_swap(p, ready);
        if (save_tx_dead) return 0;
        if (in) tx_ready_delete_below(p, n + 1);
        if (note) snprintf(note, cap, in ? "a finished save that was not in place yet was put in place" :
                                           "a finished save could not be put in place (another program holds the save folder): it is used from where it is");
    } else if (!tx_exists(p->save) && tx_exists(p->old)) {   /* (died between steps (3) and (4) of a swap whose ready is gone) */
        tx_move(p->old, p->save, SAVE_TX_MOVE_TRIES);
        if (note) snprintf(note, cap, "the save was put back in place");
    }
    if (tx_crashed()) return 0;
    tx_old_away(p, 1, NULL, 0);
    tx_delete_tree(p->work);
    if (tx_exists(p->work)) return 0;   /* (a file of it is held open: no second run of the game beside this one's files) */
    /* (the newest save: a finished commit still waiting, else the committed one) */
    const unsigned left = tx_ready_highest(p, ready, sizeof ready);
    const char *from = left ? ready : p->save;
    if (tx_is_dir(from)) {
        if (!tx_copy_tree(from, p->work, 0)) {
            tx_delete_tree(p->work);
            return 0;
        }
    } else tx_make_dirs(p->work);
    tx_make_dirs(p->save);   /* (the save "exists" from its first mount on, as before: IsSaveDataExisting) */
    return tx_is_dir(p->work);
}

/* Every file of a folder written out to the disk (FlushFileBuffers). 1: all of them */
static int tx_flush_tree(const char *dir) {
    char pat[HOS_FS_PATH];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return tx_is_dir(dir);   /* (an empty folder, where the listing has no "." entry) */
    int ok = 1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..") || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        char sub[HOS_FS_PATH];
        snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ok &= tx_flush_tree(sub); continue; }
        HANDLE h = CreateFileA(sub, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { ok = 0; continue; }
        if (!FlushFileBuffers(h)) ok = 0;
        CloseHandle(h);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    return ok;
}

/* nn::fs::Commit of the save mount, in two parts so that the game's thread pays only the first (5 ms for a real save, against
   30 with the second: nn_fs.c runs that one on a thread of its own).

   save_tx_stage, on the game's thread: the working copy as it is at this moment, copied to the staging folder. What the game
   writes after its Commit returned is not in it. 1: staged; 0: the staging copy could not be made and the working copy's files
   were copied over the save's, as before this file (nothing left to finish); -1: that failed too */
static int save_tx_stage(const SaveTxPaths *p) {
    if (tx_exists(p->staging)) tx_delete_tree(p->staging);
    if (tx_crashed()) return -1;
    if (!tx_exists(p->staging) && tx_copy_tree(p->work, p->staging, 0)) return 1;
    tx_delete_tree(p->staging);   /* (the disk is full, or something sits where the folder goes) */
    return tx_copy_tree(p->work, p->save, 0) ? 0 : -1;
}

/* save_tx_finish, after a save_tx_stage that returned 1: the staged copy written out, made the newest save and put in place.
   1: committed (swapped in, or waiting as ready-<n>); 0: a step before the commit point failed and the staged files were
   copied over the save's; -1: that failed too. backup (may be NULL) gets the backup's folder when one was made */
static int save_tx_finish(const SaveTxPaths *p, char *backup, size_t cap) {
    if (backup && cap) backup[0] = 0;
    const int staged = tx_flush_tree(p->staging);
    char ready[HOS_FS_PATH];
    const unsigned n = tx_ready_highest(p, NULL, 0) + 1;
    snprintf(ready, sizeof ready, "%s\\ready-%u", p->tx, n);
    if (tx_crashed()) return -1;
    if (!staged || !tx_move(p->staging, ready, 3)) {   /* (a file could not be written out, or the folder cannot be renamed) */
        const int copied = tx_copy_tree(p->staging, p->save, 0);
        tx_delete_tree(p->staging);
        return copied ? 0 : -1;
    }
    /* (committed: from here a death leaves ready-<n>, which the next start puts in place) */
    if (tx_crashed()) return 1;
    tx_ready_delete_below(p, n);
    if (tx_swap(p, ready)) {
        if (tx_crashed()) return 1;
        tx_old_away(p, 0, backup, cap);
    }
    return 1;
}

/* Both parts one after the other (the tests, and a build without threads) */
__attribute__((unused)) static int save_tx_commit(const SaveTxPaths *p, char *backup, size_t cap) {
    if (backup && cap) backup[0] = 0;
    const int staged = save_tx_stage(p);
    if (staged != 1 || save_tx_dead) return save_tx_dead ? -1 : staged;
    return save_tx_finish(p, backup, cap);
}
