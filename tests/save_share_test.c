/* tests/test_save_share.py's and tests/test_bug_capture.py's program (Windows): the save's sharing and the bug capture as the
   game has them. nn_fs_cut.inc is host/nn_fs.c's own text, cut by the test: its long-path file functions, then everything
   from the transaction's include to the save's mount (the lock, the Commit's thread, tx_mount, the import at the first
   mount, hos_fs_save_read and the import's calls); host/save_share.c and host/bug_capture.c are included whole after it.
   What they call of the rest of the game are the stand-ins here. The program stands in a folder that plays the game
   folder, and each run of it is a start of the game. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gpu/vk/vk_api.h"

#define UID "5a3c9e1f0b7d24680000000000000001"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- what nn_fs.c's part calls of the host ---- */
const char *hos_user_dir;
int hos_trace;
static FILE *game_log;   /* (triforce_log.txt in the game folder, as the game's own log: the capture's phases open it) */
void hos_log(const char *fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("log: %s\n", line);
    if (game_log) fprintf(game_log, "[hos host] %s\n", line), fflush(game_log);
}
static int hos_vcore_away(void) { return 0; }
static void hos_vcore_back(void) {}

#include "nn_fs_cut.inc"

/* (host/save_share.c is its own file in the game: the plain file functions, not nn_fs.c's) */
#undef CreateFileA
#undef GetFileAttributesA
#undef CreateDirectoryA
#undef DeleteFileA
#undef RemoveDirectoryA
#undef MoveFileExA
#undef FindFirstFileA
#undef FindNextFileA

/* ---- what save_share.c calls of the renderer and the host: the menu's side, kept for the test to print ---- */
static char said_in[4096], said[8][4096];
static volatile LONG said_n[8];
static int import_flag = -1;
static volatile LONG ask_next;   /* (what a row asked: the worker takes it) */
/* the renderer's side of a capture's picture: what the test chose. 0 it writes one after two more questions, 1 it has none,
   2 no frame is ever shown */
static int shot_mode, shot_asked, shot_polls, shot_dropped;
static char shot_file[MAX_PATH + 64];
int rx_option(int id, int otherwise) {
    if (id == HOS_OPT_SHARE_ASK) return (int)InterlockedExchange(&ask_next, 0);
    if (id == HOS_OPT_BUG_SHOT) {
        if (!shot_asked) return 0;
        if (shot_mode == 1) return 3;
        if (shot_mode == 2 || ++shot_polls < 3) return 1;
        FILE *f = fopen(shot_file, "wb");   /* (three presents on: the picture is there) */
        if (f) fputs("the frame's picture", f), fclose(f);
        return 2;
    }
    return id == 1 ? 0 : otherwise;
}
void rx_option_set(int id, int value) {
    if (id == HOS_OPT_BUG_SHOT) {
        if (value) shot_asked++, shot_polls = 0;
        else shot_dropped++;
        printf("shot %s\n", value ? "asked" : "asked no longer");
    }
    const size_t n = strlen(said_in);
    if (id == HOS_OPT_SHARE_TEXT && n + 1 < sizeof said_in) said_in[n] = (char)value, said_in[n + 1] = 0;
    if (id == HOS_OPT_SHARE_SAID && value > 0 && value < 8) {
        snprintf(said[value], sizeof said[value], "%s", said_in);
        said_in[0] = 0;
        printf("said %d: %s\n", value, said[value]);
        InterlockedIncrement(&said_n[value]);
    }
    if (id == HOS_OPT_SHARE_IMPORT) import_flag = value, printf("import flag %d\n", value);
}
static int flushed;
void hos_log_flush(void) { flushed++; }
const char *hos_version(void) { return "v0.4.0-test"; }

#include "../host/save_share.c"

/* ---- what bug_capture.c calls of the host: the game's presents, the controls, and the game's thread saying where Link is ---- */
volatile LONG64 hos_present_count;
static int f8_bound, link_in_play = 1, where_asked;
int hos_binding(int set, int button, int slot) { return f8_bound && set == HOS_BIND_KEY && button == HOS_SW_ZR && slot == 1 ? VK_F8 : 0; }
void hos_oot_live_crawl(const char *line) {
    where_asked += !strcmp(line, "!where");
    if (link_in_play) hos_log("cheats: crawl: where: map \"KokiriForest_Child\" loading 0 event 0 child 1 pos -12.5 3.0 480.2");
}

#include "../host/bug_capture.c"

/* ---- a save of three files, one in a slot folder, all written with one tag (as tests/test_save_tx.py's) ---- */
static void put(const char *dir, const char *name, const char *text) {
    char path[HOS_FS_PATH];
    snprintf(path, sizeof path, "%s\\%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL: cannot write %s\n", path); fails++; return; }
    for (int i = 0; i < 3000; i++) fputs(text, f);
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
static void write_save(const char *dir, const char *tag) {
    char slot[HOS_FS_PATH];
    tx_make_dirs(dir);
    snprintf(slot, sizeof slot, "%s\\slot_00", dir);
    tx_make_dirs(slot);
    put(dir, "account.sav", tag);
    put(dir, "option.sav", tag);
    put(slot, "progress.sav", tag);
}
static int is_save(const char *dir, const char *tag) {
    char slot[HOS_FS_PATH];
    snprintf(slot, sizeof slot, "%s\\slot_00", dir);
    return has(dir, "account.sav", tag) && has(dir, "option.sav", tag) && has(slot, "progress.sav", tag);
}
/* the folders of dir whose name starts with prefix: how many, and how many of them are the save written with tag */
static int count_saves(const char *dir, const char *prefix, const char *tag, int *whole) {
    char pat[HOS_FS_PATH], sub[HOS_FS_PATH];
    WIN32_FIND_DATAA fd;
    int n = 0;
    *whole = 0;
    snprintf(pat, sizeof pat, "%s\\%s*", dir, prefix);
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..") || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
        n++, *whole += is_save(sub, tag);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    return n;
}

/* ---- the game's side: a mount as MountSaveData's, a save as nn::fs::Commit's (nn_fs.c's HLEs, by hand: they read guest
   registers), then the game's end ---- */
static SaveTxPaths *game_mount(void) {
    char root[HOS_FS_PATH] = "";
    if (!tx_mount("save", UID, root, sizeof root)) return NULL;
    CHECK(ntx == 1 && !strcmp(root, tx_mounts[0].p.work));
    return &tx_mounts[0].p;
}
static void game_commit(void) {
    AcquireSRWLockExclusive(&tx_lock);
    tx_wait_locked();
    const int staged = save_tx_stage(&tx_mounts[0].p);
    CHECK(staged == 1);
    if (staged == 1) {
        tx_finish_paths = tx_mounts[0].p;
        tx_thread = CreateThread(NULL, 0, tx_finish_main, NULL, 0, NULL);
        if (!tx_thread) tx_finish_main(NULL);
    }
    ReleaseSRWLockExclusive(&tx_lock);
}
static void game_save(SaveTxPaths *p, const char *tag) {
    write_save(p->work, tag);
    game_commit();
    hos_fs_save_finish();
}

/* ---- the transaction's part alone, on folders of its own: what a start does with a waiting import, killed at each step ---- */
static void new_run(void) { save_tx_crash_at = save_tx_step = save_tx_dead = 0, save_tx_last_backup = 0, tx_import_looked = 0, tx_import_note[0] = 0; }
static int start(const SaveTxPaths *p) { return tx_import_at_mount(p, save_tx_mount(p, NULL, 0)); }   /* (as tx_mount does) */
typedef struct { int n; char rel[8][64]; char first[8]; size_t size[8]; } Seen;
static int see(void *arg, const char *rel, const void *data, size_t n) {
    Seen *s = arg;
    if (s->n < 8) snprintf(s->rel[s->n], sizeof s->rel[s->n], "%s", rel), s->first[s->n] = n ? *(const char *)data : 0, s->size[s->n] = n, s->n++;
    return 1;
}
static void stage_tag(const SaveTxPaths *p, const char *tag, int want) {
    char *body = malloc(3000 * strlen(tag) + 1);
    body[0] = 0;
    for (int i = 0; i < 3000; i++) strcat(body, tag);
    const char *rels[3] = {"account.sav", "option.sav", "slot_00/progress.sav"};
    const uint8_t *data[3] = {(const uint8_t *)body, (const uint8_t *)body, (const uint8_t *)body};
    const size_t sizes[3] = {strlen(body), strlen(body), strlen(body)};
    CHECK(tx_share_import_stage(p, 3, rels, data, sizes) == want);
    free(body);
}
static int unit(const char *base) {
    char user[HOS_FS_PATH], path[HOS_FS_PATH], kept[HOS_FS_PATH], newest[HOS_FS_PATH];
    SaveTxPaths p;
    int whole = 0;
    save_tx_test_now = 1800000000;

    /* the newest save, read whole; never the working copy */
    snprintf(user, sizeof user, "%s\\a", base);
    save_tx_paths(&p, user, UID);
    new_run();
    CHECK(save_tx_mount(&p, NULL, 0) == 1);
    CHECK(!tx_share_newest(&p, newest, sizeof newest));   /* (no save yet: an empty folder) */
    write_save(p.work, "v1");
    CHECK(save_tx_commit(&p, NULL, 0) == 1);
    write_save(p.work, "zz");   /* (what the game is writing: not a save yet) */
    CHECK(tx_share_newest(&p, newest, sizeof newest) && !strcmp(newest, p.save));
    Seen seen;
    memset(&seen, 0, sizeof seen);
    CHECK(tx_share_read(newest, "", 0, see, &seen) == 3 && seen.n == 3);
    for (int i = 0; i < seen.n; i++) {
        CHECK(seen.first[i] == 'v' && seen.size[i] == 6000);
        CHECK(!strcmp(seen.rel[i], "account.sav") || !strcmp(seen.rel[i], "option.sav") || !strcmp(seen.rel[i], "slot_00/progress.sav"));
    }
    snprintf(path, sizeof path, "%s\\ready-7", p.tx);   /* (a finished commit that is not in place yet: the newest) */
    write_save(path, "r7");
    CHECK(tx_share_newest(&p, newest, sizeof newest) && !strcmp(newest, path));
    memset(&seen, 0, sizeof seen);
    CHECK(tx_share_read(newest, "", 0, see, &seen) == 3 && seen.first[0] == 'r' && seen.first[2] == 'r');
    CHECK(tx_share_read("Z:\\no\\such\\folder", "", 0, see, &seen) == -1);

    /* an import waits; Commits leave it alone; the next one replaces it; cancelled, it is gone */
    snprintf(user, sizeof user, "%s\\b", base);
    save_tx_paths(&p, user, UID);
    new_run();
    CHECK(start(&p) == 1 && !tx_import_note[0]);
    write_save(p.work, "v1");
    CHECK(save_tx_commit(&p, NULL, 0) == 1);
    CHECK(!tx_share_import_waits(&p));
    stage_tag(&p, "i1", 1);
    snprintf(path, sizeof path, "%s\\import", p.tx);
    CHECK(tx_share_import_waits(&p) && is_save(path, "i1") && is_save(p.save, "v1"));
    write_save(p.work, "v2");
    CHECK(save_tx_commit(&p, NULL, 0) == 1 && is_save(p.save, "v2"));   /* (the game saves on) */
    CHECK(tx_share_import_waits(&p) && is_save(path, "i1"));
    stage_tag(&p, "i2", 1);
    CHECK(is_save(path, "i2"));
    snprintf(kept, sizeof kept, "%s\\import-new", p.tx);
    CHECK(!tx_exists(kept));
    CHECK(tx_share_import_cancel(&p) && !tx_share_import_waits(&p) && is_save(p.save, "v2"));
    new_run();
    CHECK(start(&p) == 1 && !tx_import_note[0] && is_save(p.save, "v2") && is_save(p.work, "v2"));   /* (cancelled: the save stays) */

    /* the next start puts a waiting import in place, the save it replaces kept first */
    stage_tag(&p, "i3", 1);
    write_save(p.work, "v3");
    CHECK(save_tx_commit(&p, NULL, 0) == 1);
    new_run();
    CHECK(start(&p) == 1 && strstr(tx_import_note, "was put in place") && strstr(tx_import_note, "import-20"));
    CHECK(is_save(p.save, "i3") && is_save(p.work, "i3") && !tx_share_import_waits(&p));
    CHECK(!tx_exists(p.staging) && !tx_exists(p.old) && !tx_ready_highest(&p, NULL, 0));
    CHECK(count_saves(p.backups, "import-", "v3", &whole) == 1 && whole == 1);   /* (the save it replaced, whole) */
    write_save(p.work, "v4");
    CHECK(save_tx_commit(&p, NULL, 0) == 1 && is_save(p.save, "v4"));   /* (and the game saves on from the import) */
    new_run();
    CHECK(start(&p) == 1 && !tx_import_note[0] && is_save(p.save, "v4"));   /* (once: the start after has nothing to do) */
    for (int i = 0; i < 12; i++) {   /* (many saves later the import's backup is still there: it is not one of the five) */
        save_tx_test_now += SAVE_TX_BACKUP_GAP_S;
        write_save(p.work, i & 1 ? "wa" : "wb");
        CHECK(save_tx_commit(&p, NULL, 0) == 1);
    }
    CHECK(count_saves(p.backups, "import-", "v3", &whole) == 1 && whole == 1);
    CHECK(count_saves(p.backups, "2", "", &whole) == SAVE_TX_BACKUPS);

    /* a start killed before each of its steps, then the start after: the import is the save, and the save it replaced is kept */
    int reached_end = 0;
    for (int at = 1; at <= 24 && !reached_end; at++) {
        snprintf(user, sizeof user, "%s\\k%d", base, at);
        save_tx_paths(&p, user, UID);
        new_run();
        CHECK(start(&p) == 1);
        write_save(p.work, "o1");
        CHECK(save_tx_commit(&p, NULL, 0) == 1);
        stage_tag(&p, "im", 1);
        new_run();
        save_tx_crash_at = at;
        start(&p);
        if (!save_tx_dead) reached_end = 1;
        else CHECK(is_save(p.save, "o1") || is_save(p.save, "im") || !tx_exists(p.save));   /* (never a mix; gone only between a swap's two renames) */
        new_run();
        CHECK(start(&p) == 1);
        if (!is_save(p.save, "im") || !is_save(p.work, "im")) printf("FAIL: killed before step %d, the next start does not play the import\n", at), fails++;
        if (count_saves(p.backups, "import-2", "o1", &whole) < 1 || whole < 1) printf("FAIL: killed before step %d, the replaced save is not kept whole\n", at), fails++;
        snprintf(path, sizeof path, "%s\\import-new", p.backups);
        CHECK(!tx_exists(path) && !tx_share_import_waits(&p));
        CHECK(!tx_exists(p.staging) && !tx_exists(p.old) && !tx_ready_highest(&p, NULL, 0));
    }
    CHECK(reached_end);

    /* a first start with an import and no save of its own yet: nothing to keep */
    snprintf(user, sizeof user, "%s\\n", base);
    save_tx_paths(&p, user, UID);
    new_run();
    CHECK(start(&p) == 1);
    stage_tag(&p, "nw", 1);
    new_run();
    CHECK(start(&p) == 1 && strstr(tx_import_note, "was put in place") && !strstr(tx_import_note, "kept in"));
    CHECK(is_save(p.save, "nw") && is_save(p.work, "nw") && count_saves(p.backups, "", "", &whole) == 0);

    /* what waits is no save (a folder someone emptied): dropped, the save stays */
    snprintf(user, sizeof user, "%s\\d", base);
    save_tx_paths(&p, user, UID);
    new_run();
    CHECK(start(&p) == 1);
    write_save(p.work, "s1");
    CHECK(save_tx_commit(&p, NULL, 0) == 1);
    snprintf(path, sizeof path, "%s\\import", p.tx);
    tx_make_dirs(path);
    put(path, "notes.txt", "x");
    new_run();
    CHECK(start(&p) == 1 && strstr(tx_import_note, "dropped") && is_save(p.save, "s1") && is_save(p.work, "s1") && !tx_exists(path));

    /* the save cannot be kept (something sits where its backups go): the import waits on and nothing changes */
    snprintf(user, sizeof user, "%s\\e", base);
    save_tx_paths(&p, user, UID);
    new_run();
    CHECK(start(&p) == 1);
    write_save(p.work, "s1");
    CHECK(save_tx_commit(&p, NULL, 0) == 1);
    stage_tag(&p, "e2", 1);
    snprintf(path, sizeof path, "%s\\save-backup", user);
    tx_make_dirs(path);
    { FILE *f = fopen(p.backups, "wb"); if (f) fclose(f); }   /* (a file where the folder of backups goes) */
    new_run();
    CHECK(start(&p) == 1 && strstr(tx_import_note, "waits for the next start"));
    CHECK(is_save(p.save, "s1") && is_save(p.work, "s1") && tx_share_import_waits(&p));
    DeleteFileA(p.backups);
    new_run();
    CHECK(start(&p) == 1 && is_save(p.save, "e2") && count_saves(p.backups, "import-", "s1", &whole) == 1 && whole == 1);

    /* a finished commit that is not in place (another program holds the save folder): it is the save that is kept, the
       import waits as the newest commit, and once the folder is free the start puts it in place */
    snprintf(user, sizeof user, "%s\\h", base);
    save_tx_paths(&p, user, UID);
    new_run();
    CHECK(start(&p) == 1);
    write_save(p.work, "h1");
    CHECK(save_tx_commit(&p, NULL, 0) == 1 && is_save(p.save, "h1"));
    snprintf(path, sizeof path, "%s\\option.sav", p.save);
    HANDLE held = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
    write_save(p.work, "h2");
    CHECK(save_tx_commit(&p, NULL, 0) == 1 && is_save(p.save, "h1") && tx_ready_highest(&p, NULL, 0) == 1);
    stage_tag(&p, "hi", 1);
    new_run();
    CHECK(start(&p) == 1 && strstr(tx_import_note, "was put in place"));
    CHECK(is_save(p.work, "hi") && is_save(p.save, "h1"));   /* (played from where it waits) */
    CHECK(count_saves(p.backups, "import-", "h2", &whole) == 1 && whole == 1);   /* (the newest save was h2, not the folder's h1) */
    CloseHandle(held);
    new_run();
    CHECK(start(&p) == 1 && is_save(p.save, "hi") && is_save(p.work, "hi") && !tx_ready_highest(&p, NULL, 0));
    CHECK(count_saves(p.backups, "2", "h1", &whole) == 1 && whole == 1);   /* (and the folder's old save became a usual backup) */

    if (!fails) printf("PASS\n");
    return fails != 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char game[MAX_PATH];
    static char user[MAX_PATH + 8];
    if (argc < 2 || !hos_share_game_dir(game, sizeof game)) return 2;
    snprintf(user, sizeof user, "%s\\user", game);
    hos_user_dir = user;
    CreateDirectoryA(user, NULL);
    const char *phase = argv[1];
    if (!strcmp(phase, "unit")) {
        char base[MAX_PATH + 8];
        snprintf(base, sizeof base, "%s\\unit", game);
        return unit(base);
    }
    if (!strcmp(phase, "downloads")) {   /* (the player's own Downloads folder, only named: nothing is written there) */
        char dir[SHARE_PATH] = "";
        const int found = downloads_dir(dir, sizeof dir);
        printf("downloads %d %s\n", found, dir);
        return 0;
    }
    if (!strcmp(phase, "early")) {   /* (before the game mounted its save: the rows find the save's folders by themselves) */
        printf("waits %d\n", hos_fs_save_import_waits());
        do_export(0);
        return fails != 0;
    }
    if (!strcmp(phase, "thread") && argc == 3) {   /* thread <what a row asks>: the worker itself, started as the save's mount starts it */
        const int ask = atoi(argv[2]), row = ask == HOS_SHARE_CAPTURE_KEY ? HOS_SHARE_CAPTURE : ask;
        snprintf(shot_file, sizeof shot_file, "%s\\captures\\frame-pending.png", game);
        CHECK(game_mount() != NULL);
        hos_save_share_start();
        hos_save_share_start();   /* (once: the second call starts nothing) */
        Sleep(350);               /* (a few rounds with nothing asked: nothing is done) */
        CHECK(!said_n[row] && !shot_asked);
        InterlockedExchange(&ask_next, ask);
        for (int waited = 0; waited < 10000 && !said_n[row]; waited += 20) Sleep(20);
        CHECK(said_n[row] == 1);
        Sleep(250);
        CHECK(said_n[row] == 1 && tick_n >= 4);   /* (asked once, done once; and the frames were counted meanwhile) */
        return fails != 0;
    }
    if (!strcmp(phase, "capture") && argc == 3) {   /* capture <picture|nopicture|noframes|bound|title|twice> */
        char log_path[MAX_PATH + 32];
        const char *mode = argv[2];
        snprintf(log_path, sizeof log_path, "%s\\triforce_log.txt", game);
        game_log = fopen(log_path, "a");   /* (text, as the game's own log: its lines end as Windows ends them) */
        snprintf(shot_file, sizeof shot_file, "%s\\captures\\frame-pending.png", game);
        shot_mode = !strcmp(mode, "nopicture") ? 1 : !strcmp(mode, "noframes") ? 2 : 0;
        f8_bound = !strcmp(mode, "bound");
        link_in_play = strcmp(mode, "title") != 0;
        CHECK(game_mount() != NULL);
        for (int i = 0; i < 14; i++) {   /* (1.4 s of play at 60 frames a second, as the worker counts it) */
            InterlockedExchangeAdd64(&hos_present_count, 6);
            hos_bug_capture_tick();
            Sleep(100);
        }
        const LONG64 at_press = hos_present_count;
        hos_bug_capture_key();
        printf("presents %lld where asked %d state %ld\n", (long long)at_press, where_asked, (long)capture_state);
        if (f8_bound) {
            hos_bug_capture_work();
            CHECK(!shot_asked && !where_asked && !capture_state && !said_n[HOS_SHARE_CAPTURE]);
            return fails != 0;
        }
        CHECK(shot_asked == 1 && where_asked == 1 && capture_state != 0);
        CHECK(!hos_bug_capture_ask("F8 again") && shot_asked == 1);   /* (one at a time: a press while one is written is skipped) */
        InterlockedExchangeAdd64(&hos_present_count, 600);   /* (the game goes on while the folder is written, on the rows' thread) */
        const ULONGLONG t0 = GetTickCount64();
        for (int waited = 0; waited < 15000 && (capture_state || !said_n[HOS_SHARE_NOTICE]); waited += 20) Sleep(20);
        printf("written in %llu ms\n", (unsigned long long)(GetTickCount64() - t0));
        CHECK(!capture_state && said_n[HOS_SHARE_CAPTURE] == 1 && said_n[HOS_SHARE_NOTICE] == 1 && strlen(said[HOS_SHARE_NOTICE]) < 64);
        CHECK(shot_dropped == (shot_mode == 2));   /* (no frame in three seconds: the picture is asked for no longer) */
        if (!strcmp(mode, "twice")) {   /* (and the next press, perhaps in the same second: a folder of its own) */
            CHECK(hos_bug_capture_ask("the System tab's row") == 1);
            for (int waited = 0; waited < 15000 && (capture_state || said_n[HOS_SHARE_NOTICE] < 2); waited += 20) Sleep(20);
            CHECK(!capture_state && said_n[HOS_SHARE_CAPTURE] == 2 && shot_asked == 2);
        }
        return fails != 0;
    }
    SaveTxPaths *p = game_mount();
    if (!strcmp(phase, "plain")) {   /* (HOS_SAVE_TX=0: no working copy, the save written in place) */
        CHECK(!p);
        do_export(0), do_export(1), do_import();
        return fails != 0;
    }
    if (!p) return 3;
    char note[SHARE_PATH + 200];
    printf("note %d %s\n", hos_fs_save_import_note(note, sizeof note), note);
    printf("waits %d\n", hos_fs_save_import_waits());
    if (!strcmp(phase, "play") && argc == 3) {   /* play <tag>: the game saves, then goes on writing */
        game_save(p, argv[2]);
        write_save(p->work, "zz");
    } else if (!strcmp(phase, "export") && argc == 3) {   /* export <0|1> */
        write_save(p->work, "zz");
        do_export(atoi(argv[2]));
        CHECK(flushed == (atoi(argv[2]) ? 1 : 0));   /* (a bug report's log is written out first) */
    } else if (!strcmp(phase, "import")) {   /* the row, then the game saves again before it ends */
        do_import();
        if (argc == 3) game_save(p, argv[2]);
        printf("waits %d\n", hos_fs_save_import_waits());
    } else if (!strcmp(phase, "import-cancel")) {   /* the row, and the row again while it waits */
        char waiting[HOS_FS_PATH];
        do_import();
        snprintf(waiting, sizeof waiting, "%s\\import", p->tx);
        printf("staged %s\n", is_save(waiting, "n2") ? "n2" : "?");
        do_import_cancel();
        printf("waits %d\n", hos_fs_save_import_waits());
    } else if (!strcmp(phase, "busy") && argc == 3) {   /* busy <tag>: an export while a Commit is still on its thread */
        char path[HOS_FS_PATH], zip[SHARE_PATH], dir[SHARE_PATH];
        int files = 0;
        snprintf(path, sizeof path, "%s\\option.sav", p->save);
        HANDLE held = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(held != INVALID_HANDLE_VALUE);
        write_save(p->work, argv[2]);
        game_commit();   /* (its thread now tries for 0.2 s to move the held folder away) */
        const ULONGLONG t0 = GetTickCount64();
        CHECK(downloads_dir(dir, sizeof dir) && hos_save_export(dir, 0, zip, sizeof zip, &files) == 1 && files == 3);
        printf("busy export after %llu ms: %s\n", (unsigned long long)(GetTickCount64() - t0), zip);
        CHECK(!tx_thread);   /* (the export waited for the Commit's thread, as the next Commit would) */
        CloseHandle(held);
        hos_fs_save_finish();
    } else if (strcmp(phase, "start")) {
        return 2;
    }
    return fails != 0;
}
