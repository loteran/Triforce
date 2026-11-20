"""The save as a transaction, with backups (host/save_tx.h), on real folders in a temporary place.

A Commit copies the working copy to a staging folder, renames that to ready-<n> (the commit point) and swaps it in for the
committed save, whose old state becomes a backup at most every five minutes, the newest five kept. Checked here:
- a first save, a second one and its backup, the five-minute gap, the five kept, a player's own folder among them left alone;
- a Commit killed before each of its steps, then the next start: the committed save is the whole old one or the whole new one,
  never a mix, it is the new one once the commit point was passed, and the working copy equals it;
- changes no Commit took are gone at the next start, as on the console;
- a staging folder that cannot be made: the files are copied over the save's as before the transactions (result 0);
- (Windows) a save folder another program holds open: the commit waits as ready-<n>, the next start plays from it, and the
  next Commit with the folder free puts it in place.

python -I -B tests/test_save_tx.py
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define HOS_FS_PATH 1024
#include "save_tx.h"
#include <stdio.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void put(const char *dir, const char *name, const char *text) {
    char path[HOS_FS_PATH];
    snprintf(path, sizeof path, "%s\\%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL: cannot write %s\n", path); fails++; return; }
    for (int i = 0; i < 3000; i++) fputs(text, f);   /* (more than one copy chunk would hold: 3000 x a few bytes) */
    fclose(f);
}
static int has(const char *dir, const char *name, const char *text) {
    char path[HOS_FS_PATH], got[16] = "";
    snprintf(path, sizeof path, "%s\\%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, size - (long)strlen(text), SEEK_SET);
    const size_t n = fread(got, 1, strlen(text), f);
    fclose(f);
    return size == 3000L * (long)strlen(text) && n == strlen(text) && !memcmp(got, text, n);
}
/* (a save of three files, one in a slot folder, all written with one tag) */
static void write_save(const char *dir, const char *tag) {
    char slot[HOS_FS_PATH];
    tx_make_dirs(dir);
    snprintf(slot, sizeof slot, "%s\\slot_00", dir);
    tx_make_dirs(slot);
    put(dir, "account.sav", tag);
    put(dir, "option.sav", tag);
    put(slot, "progress.sav", tag);
}
/* 1: the folder is exactly a save written with tag */
static int is_save(const char *dir, const char *tag) {
    char slot[HOS_FS_PATH];
    snprintf(slot, sizeof slot, "%s\\slot_00", dir);
    return has(dir, "account.sav", tag) && has(dir, "option.sav", tag) && has(slot, "progress.sav", tag);
}
static int count_dirs(const char *dir) {
    char pat[HOS_FS_PATH];
    WIN32_FIND_DATAA fd;
    int n = 0;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..") && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n++;
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    return n;
}
static void new_run(void) { save_tx_crash_at = save_tx_step = save_tx_dead = 0; save_tx_last_backup = 0; }
static int clean_tx(const SaveTxPaths *p) {   /* nothing of a commit left behind */
    return !tx_exists(p->staging) && !tx_exists(p->old) && !tx_ready_highest(p, NULL, 0);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    char user[HOS_FS_PATH], note[256], backup[HOS_FS_PATH];
    SaveTxPaths p;
    save_tx_test_now = 1800000000;

    /* a first save, a second one and its backup, the gap, the five kept */
    snprintf(user, sizeof user, "%s\\a", argv[1]);
    save_tx_paths(&p, user, "0123456789abcdef0000000000000001");
    new_run();
    CHECK(save_tx_mount(&p, note, sizeof note) == 1 && !note[0]);
    CHECK(tx_is_dir(p.work) && tx_is_dir(p.save));
    write_save(p.work, "v1");
    CHECK(save_tx_commit(&p, backup, sizeof backup) == 1 && !backup[0]);
    CHECK(is_save(p.save, "v1") && clean_tx(&p) && count_dirs(p.backups) == 0);
    write_save(p.work, "v2");
    CHECK(save_tx_commit(&p, backup, sizeof backup) == 1 && backup[0] && is_save(backup, "v1"));
    CHECK(is_save(p.save, "v2") && clean_tx(&p) && count_dirs(p.backups) == 1);
    save_tx_test_now += 10;   /* (ten seconds on: no backup) */
    write_save(p.work, "v3");
    CHECK(save_tx_commit(&p, backup, sizeof backup) == 1 && !backup[0] && is_save(p.save, "v3") && count_dirs(p.backups) == 1);
    save_tx_test_now += SAVE_TX_BACKUP_GAP_S;
    write_save(p.work, "v4");
    CHECK(save_tx_commit(&p, backup, sizeof backup) == 1 && is_save(backup, "v3") && count_dirs(p.backups) == 2);
    {   /* (a folder of the player's own among the backups stays) */
        char own[HOS_FS_PATH];
        snprintf(own, sizeof own, "%s\\my old save", p.backups);
        write_save(own, "me");
        for (int i = 0; i < 9; i++) {
            save_tx_test_now += SAVE_TX_BACKUP_GAP_S;
            write_save(p.work, i & 1 ? "wa" : "wb");
            CHECK(save_tx_commit(&p, backup, sizeof backup) == 1 && backup[0]);
        }
        CHECK(count_dirs(p.backups) == SAVE_TX_BACKUPS + 1 && is_save(own, "me"));
        CHECK(is_save(backup, "wa") && is_save(p.save, "wb"));   /* (the last backup is the save before the last) */
    }
    {   /* (two backups in one second get two names) */
        save_tx_last_backup = 0;
        write_save(p.work, "x1");
        CHECK(save_tx_commit(&p, backup, sizeof backup) == 1 && backup[0]);
        save_tx_last_backup = 0;
        char second[HOS_FS_PATH];
        write_save(p.work, "x2");
        CHECK(save_tx_commit(&p, second, sizeof second) == 1 && second[0] && strcmp(second, backup));
        CHECK(is_save(second, "x1") && count_dirs(p.backups) == SAVE_TX_BACKUPS + 1);
    }

    /* changes no Commit took are gone at the next start */
    write_save(p.work, "zz");
    new_run();
    CHECK(save_tx_mount(&p, note, sizeof note) == 1 && !note[0]);
    CHECK(is_save(p.work, "x2") && is_save(p.save, "x2"));

    /* a Commit killed before each of its steps, then the next start */
    int reached_end = 0;
    for (int at = 1; at <= 12 && !reached_end; at++) {
        char sub[32];
        snprintf(sub, sizeof sub, "%s\\k%d", argv[1], at);
        snprintf(user, sizeof user, "%s", sub);
        save_tx_paths(&p, user, "0123456789abcdef0000000000000001");
        new_run();
        CHECK(save_tx_mount(&p, NULL, 0) == 1);
        write_save(p.work, "o1");
        CHECK(save_tx_commit(&p, NULL, 0) == 1 && is_save(p.save, "o1"));
        write_save(p.work, "n2");
        save_tx_crash_at = at, save_tx_step = 0, save_tx_dead = 0;
        const int r = save_tx_commit(&p, NULL, 0);
        const int died = save_tx_dead;
        if (!died) reached_end = 1;   /* (more steps asked for than a Commit has: it ran whole) */
        new_run();
        CHECK(save_tx_mount(&p, note, sizeof note) == 1);
        const int old = is_save(p.save, "o1"), new_ = is_save(p.save, "n2");
        if (!(old ^ new_)) { printf("FAIL: killed before step %d, the save is neither the old nor the new one\n", at); fails++; }
        if (r == 1 && !new_) { printf("FAIL: killed before step %d after the commit point, the save is not the new one\n", at); fails++; }
        if (r != 1 && died && !old) { printf("FAIL: killed before step %d before the commit point, the save is not the old one\n", at); fails++; }
        CHECK(is_save(p.work, new_ ? "n2" : "o1") && clean_tx(&p));
        /* (and the game saves on from there) */
        write_save(p.work, "n3");
        CHECK(save_tx_commit(&p, NULL, 0) == 1 && is_save(p.save, "n3") && clean_tx(&p));
    }
    CHECK(reached_end);

    /* a staging folder that cannot be made: the files copied over the save's, as before the transactions */
    snprintf(user, sizeof user, "%s\\f", argv[1]);
    save_tx_paths(&p, user, "0123456789abcdef0000000000000001");
    new_run();
    CHECK(save_tx_mount(&p, NULL, 0) == 1);
    write_save(p.work, "f1");
    CHECK(save_tx_commit(&p, NULL, 0) == 1);
    { FILE *f = fopen(p.staging, "wb"); if (f) fclose(f); }   /* (a file where the staging folder goes) */
    write_save(p.work, "f2");
    CHECK(save_tx_commit(&p, NULL, 0) == 0 && is_save(p.save, "f2"));
    DeleteFileA(p.staging);
    write_save(p.work, "f3");
    CHECK(save_tx_commit(&p, NULL, 0) == 1 && is_save(p.save, "f3") && clean_tx(&p));

#ifdef _WIN32
    /* a save folder another program holds open */
    snprintf(user, sizeof user, "%s\\h", argv[1]);
    save_tx_paths(&p, user, "0123456789abcdef0000000000000001");
    new_run();
    CHECK(save_tx_mount(&p, NULL, 0) == 1);
    write_save(p.work, "h1");
    CHECK(save_tx_commit(&p, NULL, 0) == 1 && is_save(p.save, "h1"));
    char held[HOS_FS_PATH];
    snprintf(held, sizeof held, "%s\\option.sav", p.save);
    HANDLE h = CreateFileA(held, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    write_save(p.work, "h2");
    CHECK(save_tx_commit(&p, NULL, 0) == 1);
    CHECK(is_save(p.save, "h1") && tx_ready_highest(&p, NULL, 0) == 1);   /* (committed, waiting) */
    new_run();
    CHECK(save_tx_mount(&p, note, sizeof note) == 1 && note[0]);
    CHECK(is_save(p.work, "h2") && is_save(p.save, "h1"));   /* (the next start plays from the newest save) */
    write_save(p.work, "h3");
    CHECK(save_tx_commit(&p, NULL, 0) == 1 && tx_ready_highest(&p, NULL, 0) == 2 && is_save(p.save, "h1"));
    CloseHandle(h);
    write_save(p.work, "h4");
    CHECK(save_tx_commit(&p, NULL, 0) == 1 && is_save(p.save, "h4") && clean_tx(&p));
    /* (and held again, then freed before the next start: that start puts the waiting save in place) */
    h = CreateFileA(held, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    write_save(p.work, "h5");
    CHECK(save_tx_commit(&p, NULL, 0) == 1 && is_save(p.save, "h4"));
    CloseHandle(h);
    new_run();
    CHECK(save_tx_mount(&p, note, sizeof note) == 1 && note[0] && is_save(p.save, "h5") && is_save(p.work, "h5") && clean_tx(&p));
#endif
    if (!fails) printf("PASS\n");
    return fails != 0;
}
"""


class SaveTxTests(unittest.TestCase):
    def test_transactions(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        if sys.platform != "win32":
            self.skipTest("the fixture calls the Windows file functions (the game's Linux build has them from host/posix)")
        with tempfile.TemporaryDirectory(prefix="triforce-save-tx-") as tmp:
            tmp = Path(tmp)
            (tmp / "t.c").write_text(FIXTURE, encoding="utf-8")
            binary = tmp / "t.exe"
            build = subprocess.run([compiler, "-std=gnu11", "-Wall", "-O1", "-I", str(ROOT / "host"), str(tmp / "t.c"),
                                    "-o", str(binary)], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            work = tmp / "saves"
            work.mkdir()
            run = subprocess.run([str(binary), str(work)], text=True, capture_output=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("PASS", run.stdout)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
