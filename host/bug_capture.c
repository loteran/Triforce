/* (triforce) A bug capture: F8 in the game's window (host/display.c), or the System tab's Bug capture row, writes one folder a
   player can send with a problem report, captures/<date>-<time> in the game folder:

     frame.png              the frame the window was shown next, without the overlays (gpu/vk/vk_bugshot.cpp)
     triforce_options.ini   the settings
     Triforce-save-....zip  the save, as the System tab's Export save writes it (host/save_share.c)
     log-tail.txt           the last 400 lines of triforce_log.txt
     capture.txt            written last, so a folder that has it is whole: the build, the GPU and its driver as the log's
                            first lines name them, the game's frames shown so far, the frame rate before the press, and
                            the map and Link's place when he is in play

   The behaviour is NX2navi's F8 (its docs/bug-capture.md). Its code is another author's and none of it is here; this is
   written for Triforce from what that page says a capture holds.

   Nothing of the file work is done where the key is pressed: the press only notes the time and the frame count, asks the
   renderer for the next frame's picture (rx_option_set HOS_OPT_BUG_SHOT) and wakes the save rows' thread, which writes
   the folder (hos_bug_capture_work) while the game goes on. One capture at a time: a press while one is being written is
   skipped, and said in the log.

   The map and Link's place come from what the game's own thread says when asked "where" (host/hos_main.c
   hos_oot_live_crawl, the lab's command: Sequence_CurrentBanc and the player's position, read there on the thread that
   updates him). It answers in the log within a frame while Link is in play, and the capture reads that line back: the
   first one after the line the press itself logged, which is in the log before the question is asked. At the title
   screen or in a loading screen there is no answer and capture.txt says so. The camera is not in a capture: nothing of
   the host reads it outside the debug tools' free camera.

   The frame rate is counted from the game's own presents (hos_present_count), which the save rows' thread notes ten times a
   second (hos_bug_capture_tick). */
#include "hos_paths.h"   /* (first: hos_game_path_a, and windows.h on Windows) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../gpu/vk/vk_api.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif
#define CAPTURE_PATH 1024
#define TAIL_LINES 400
#define TAIL_BYTES (512u << 10)   /* (400 of the log's lines are well under this) */
#define HEAD_BYTES (64u << 10)    /* (the build, the GPUs and the driver are in the log's first hundred lines) */
#define SHOT_WAIT_MS 3000         /* (the picture is there three presents after the press; a game that shows no frame has none) */
#define TICKS 128                 /* (a tenth of a second each: the twelve seconds before a press) */

void hos_log(const char *fmt, ...);
void hos_log_flush(void);
int rx_option(int id, int otherwise);
void rx_option_set(int id, int value);
const char *hos_version(void);                  /* hos_main.c */
void hos_oot_live_crawl(const char *line);      /* hos_main.c: "!where" */
int hos_binding(int set, int button, int slot); /* nn_hid.c */
extern volatile LONG64 hos_present_count;       /* display.c, posix/display_sdl.c: the game's presents */
/* save_share.c */
int hos_share_game_dir(char *out, size_t cap);
int hos_save_export(const char *dir, int bug_report, char *path, size_t cap, int *files);
void hos_save_share_start(void);
void hos_save_share_wake(void);
void hos_save_share_say(int row, const char *text);

/* ---- the press ---- */
static volatile LONG capture_state;   /* 0 none, 1 asked: the thread has it to write, 2 being written, 3 a press is noting itself */
static struct {
    int number;   /* (the run's first capture is 1: what its lines in the log are told apart by) */
    char trigger[40];
    LONG64 presents;
    time_t when;
    ULONGLONG tick;
} asked;

/* A capture is asked for (any thread; trigger: what asked, for the log). 1: it will be written; 0: one is under way */
int hos_bug_capture_ask(const char *trigger) {
    if (InterlockedCompareExchange(&capture_state, 3, 0) != 0) {
        hos_log("capture: %s: skipped, the capture before it is still being written", trigger);
        return 0;
    }
    static int count;   /* (one press at a time gets here: capture_state) */
    asked.number = ++count;
    snprintf(asked.trigger, sizeof asked.trigger, "%s", trigger);
    asked.presents = InterlockedCompareExchange64(&hos_present_count, 0, 0);
    asked.when = time(NULL);
    asked.tick = GetTickCount64();
    /* (in the log before the game's thread is asked where Link is: its answer is the first one after this line) */
    hos_log("capture: bug capture %d asked for (%s) at present %lld", asked.number, trigger, (long long)asked.presents);
    rx_option_set(HOS_OPT_BUG_SHOT, 1);   /* (the frame shown next) */
#if defined(_WIN32) || defined(__linux__)   /* (the debug tools' side of hos_main.c: as there) */
    hos_oot_live_crawl("!where");   /* (the game's thread says the map and Link's place in the log, while he is in play) */
#endif
    InterlockedExchange(&capture_state, 1);
    hos_save_share_start();
    hos_save_share_wake();
    return 1;
}

/* F8 in the game's window. Not when the player bound F8 to one of the controls (none is by default): the key is the
   game's then, and the System tab's row still takes a capture */
void hos_bug_capture_key(void) {
    for (int button = 0; button < HOS_KEY_N; button++)
        for (int slot = 0; slot < 2; slot++)
            if (hos_binding(HOS_BIND_KEY, button, slot) == VK_F8) {
                static volatile LONG said;
                if (!InterlockedExchange(&said, 1))
                    hos_log("capture: F8 is bound to one of the controls: no capture from the key (the System tab's Bug capture row takes one)");
                return;
            }
    hos_bug_capture_ask("F8");
}

/* ---- the frames shown, a tenth of a second at a time (the save rows' thread, each time round) ---- */
static struct {
    ULONGLONG ms;
    LONG64 presents;
} ticks[TICKS];
static unsigned tick_n;

void hos_bug_capture_tick(void) {
    const ULONGLONG now = GetTickCount64();
    if (tick_n && now - ticks[(tick_n - 1) % TICKS].ms < 50) return;   /* (woken early: not a tenth of a second yet) */
    ticks[tick_n % TICKS].ms = now;
    ticks[tick_n % TICKS].presents = InterlockedCompareExchange64(&hos_present_count, 0, 0);
    tick_n++;
}

/* The frame rate over the span_ms before the press (span_ms > 0): from the newest tick that far back to the press itself,
   whose own count and time the press noted; -1 when the ticks do not reach that far back */
static double rate_before(unsigned span_ms) {
    const unsigned kept = tick_n < TICKS ? tick_n : TICKS;
    for (unsigned k = 0; k < kept; k++) {   /* (newest first) */
        const unsigned i = (tick_n - 1 - k) % TICKS;
        if (ticks[i].ms + span_ms > asked.tick || ticks[i].presents > asked.presents) continue;
        return (double)(asked.presents - ticks[i].presents) * 1000.0 / (double)(asked.tick - ticks[i].ms);
    }
    return -1;
}

/* ---- files ---- */
/* Up to max bytes of a file, from its start or its end: malloc'd with a NUL after them, NULL when it is not there */
static char *read_part(const char *path, size_t max, int from_end, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    _fseeki64(f, 0, SEEK_END);
    const long long size = _ftelli64(f);
    const size_t want = size < 0 ? 0 : (unsigned long long)size > max ? max : (size_t)size;
    char *data = (char *)malloc(want + 1);
    _fseeki64(f, from_end ? size - (long long)want : 0, SEEK_SET);
    *n = data ? fread(data, 1, want, f) : 0;
    fclose(f);
    if (data) data[*n] = 0;
    return data;
}

static int write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const size_t put = fwrite(data, 1, n, f);
    return !fclose(f) && put == n;
}

/* Where the last `lines` lines of text[0..n) begin */
static const char *last_lines(const char *text, size_t n, int lines) {
    const char *p = text + n;
    if (p > text && p[-1] == '\n') p--;   /* (the last line's own end) */
    while (p > text) {
        if (p[-1] == '\n' && --lines == 0) break;
        p--;
    }
    return p;
}

/* A line of the log without its "[hos <thread>] " and its end, into out. Returns the line after it */
static const char *log_line(const char *line, char *out, size_t cap) {
    const char *end = strchr(line, '\n');
    if (!end) end = line + strlen(line);
    const char *from = line;
    if (!strncmp(line, "[hos ", 5)) {
        const char *close = strstr(line, "] ");
        if (close && close < end) from = close + 2;
    }
    size_t len = (size_t)(end - from);
    while (len && (from[len - 1] == '\r' || from[len - 1] == ' ')) len--;
    snprintf(out, cap, "%.*s", (int)len, from);
    return *end ? end + 1 : end;
}

/* ---- the capture, on the save rows' thread ---- */
void hos_bug_capture_work(void) {
    if (InterlockedCompareExchange(&capture_state, 2, 1) != 1) return;
    const ULONGLONG began = GetTickCount64();
    char game[MAX_PATH], name[48], dir[CAPTURE_PATH], path[CAPTURE_PATH + 64], other[CAPTURE_PATH + 64], line[CAPTURE_PATH + 200];
    struct tm local, utc;
    localtime_s(&local, &asked.when);
    gmtime_s(&utc, &asked.when);
    int made = 0;
    if (hos_share_game_dir(game, sizeof game)) {
        snprintf(dir, sizeof dir, "%s" SEP "captures", game);
        CreateDirectoryA(dir, NULL);
        for (int k = 1; k < 100 && !made; k++) {   /* (two in one second: "-2") */
            const int len = snprintf(name, sizeof name, k == 1 ? "%04d%02d%02d-%02d%02d%02d" : "%04d%02d%02d-%02d%02d%02d-%d", local.tm_year + 1900,
                                     local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, k);
            if (len < 0 || snprintf(dir, sizeof dir, "%s" SEP "captures" SEP "%s", game, name) >= (int)sizeof dir) break;
            made = CreateDirectoryA(dir, NULL) != 0;
        }
    }
    if (!made) {
        rx_option_set(HOS_OPT_BUG_SHOT, 0);
        hos_log("capture: bug capture %d: no folder could be made under the game folder's captures: nothing was written", asked.number);
        hos_save_share_say(HOS_SHARE_CAPTURE, "The capture's folder could not be made in the game folder: nothing was written.");
        InterlockedExchange(&capture_state, 0);
        return;
    }
    hos_log("capture: bug capture %d goes to %s", asked.number, dir);   /* (the one line that says where) */

    /* the settings, and the save as Export save writes it */
    size_t n = 0;
    snprintf(path, sizeof path, "%s" SEP "triforce_options.ini", game);
    char *ini = read_part(path, 1u << 20, 0, &n);
    snprintf(path, sizeof path, "%s" SEP "triforce_options.ini", dir);
    const int have_ini = ini && write_file(path, ini, n);
    free(ini);
    int files = 0;
    char zip[CAPTURE_PATH + 64];
    const int have_save = hos_save_export(dir, 0, zip, sizeof zip, &files);

    /* the picture: the renderer writes it once the frame it copied is done */
    int shot = 1;
    for (unsigned waited = 0; waited < SHOT_WAIT_MS && (shot = rx_option(HOS_OPT_BUG_SHOT, 3)) == 1; waited += 20) Sleep(20);
    if (shot == 1) rx_option_set(HOS_OPT_BUG_SHOT, 0);   /* (no frame was shown in that time: asked no longer) */
    snprintf(other, sizeof other, "%s" SEP "captures" SEP "frame-pending.png", game);
    snprintf(path, sizeof path, "%s" SEP "frame.png", dir);
    const int have_picture = shot == 2 && MoveFileExA(other, path, MOVEFILE_REPLACE_EXISTING) != 0;

    /* the log: its end as log-tail.txt, and in it what the game's thread said of Link's place since the press */
    while (GetTickCount64() - asked.tick < 300) Sleep(20);   /* (a few frames, for that answer) */
    hos_log_flush();
    char place[400] = "";
    snprintf(path, sizeof path, "%s" SEP "triforce_log.txt", game);
    char *tail = read_part(path, TAIL_BYTES, 1, &n);
    int have_log = 0;
    if (tail) {
        const char *from = last_lines(tail, n, TAIL_LINES);
        snprintf(other, sizeof other, "%s" SEP "log-tail.txt", dir);
        have_log = write_file(other, from, (size_t)(tail + n - from));
        char mark[64];   /* (the press's own line: a capture's number is the run's, and the log is the run's) */
        snprintf(mark, sizeof mark, "capture: bug capture %d asked for (", asked.number);
        const char *mine = NULL;
        for (const char *p = tail; (p = strstr(p, mark)) != NULL; p++) mine = p;
        const char *where = mine ? strstr(mine, "cheats: crawl: where: ") : NULL;
        if (where) log_line(where + strlen("cheats: crawl: where: "), place, sizeof place);
    }

    /* capture.txt, last */
    snprintf(path, sizeof path, "%s" SEP "capture.txt.part", dir);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "Triforce bug capture %s\n\n", name);
        fprintf(f, "time = %04d-%02d-%02d %02d:%02d:%02d (%04d-%02d-%02dT%02d:%02d:%02dZ)\n", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                local.tm_hour, local.tm_min, local.tm_sec, utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
        fprintf(f, "trigger = %s\n", asked.trigger);
        fprintf(f, "version = %s\n", hos_version());
        /* (the build, the GPUs and the driver, as the log's first lines give them) */
        snprintf(other, sizeof other, "%s" SEP "triforce_log.txt", game);
        size_t hn = 0;
        char *head = read_part(other, HEAD_BYTES, 0, &hn);
        int head_lines = 0;
        for (const char *p = head; p && *p && head_lines < 24;) {
            static const char *const says[] = {"] triforce ", "vk: device ", "; mesh shaders ", "vk: vendor fixes", "rx: renderer:",
                                               "oe: output resolution", "vk: swapchain ", "vk: HDR:"};
            const char *end = strchr(p, '\n');
            const size_t len = end ? (size_t)(end - p) : strlen(p);
            for (size_t i = 0; i < sizeof says / sizeof *says; i++) {
                const char *hit = strstr(p, says[i]);
                if (!hit || hit >= p + len) continue;
                log_line(p, line, sizeof line);
                fprintf(f, "log = %s\n", line);
                head_lines++;
                break;
            }
            p = end ? end + 1 : p + len;
        }
        if (!head) fprintf(f, "log = none (triforce_log.txt is not in the game folder: the game was started with its output sent elsewhere)\n");
        free(head);
        fprintf(f, "\npresents = %lld (the game's frames shown up to the press)\n", (long long)asked.presents);
        const double second = rate_before(1000), ten = rate_before(10000);
        if (second >= 0) fprintf(f, "frame rate = %.1f a second over the second before the press\n", second);
        else fprintf(f, "frame rate = not known (the press came in the game's first second)\n");
        if (ten >= 0) fprintf(f, "frame rate, ten seconds = %.1f a second\n", ten);
        {   /* (the last three seconds a tenth at a time, oldest first: a stutter shows as a low count. A span that was not a
               tenth, the thread busy with an export or woken early, is written with its length) */
            const unsigned kept = tick_n < TICKS ? tick_n : TICKS;
            int shown = 0;
            fprintf(f, "frames each tenth of a second before the press =");
            for (unsigned k = kept < 31 ? kept : 31; k >= 2; k--) {
                const unsigned a = (tick_n - k) % TICKS, b = (tick_n - k + 1) % TICKS;
                if (ticks[b].ms >= asked.tick || ticks[b].presents > asked.presents) break;   /* (at the press, or after it) */
                const ULONGLONG span = ticks[b].ms - ticks[a].ms;
                if (span >= 80 && span <= 125) fprintf(f, " %lld", (long long)(ticks[b].presents - ticks[a].presents));
                else fprintf(f, " %lld/%llums", (long long)(ticks[b].presents - ticks[a].presents), (unsigned long long)span);
                shown++;
            }
            fprintf(f, shown ? "\n" : " not known\n");
        }
        if (place[0]) fprintf(f, "\nplace = %s\n", place);
        else fprintf(f, "\nplace = not known (Link is not in play: the title screen, a menu before a save is loaded, or a loading screen)\n");
        fprintf(f, "camera = not in a capture\n\n");
        if (have_picture) fprintf(f, "picture = frame.png (the frame shown after the press, without the overlays)\n");
        else
            fprintf(f, "picture = none (%s)\n", shot == 1 ? "no frame was shown in three seconds"
                                                          : "the renderer could not read the window's picture: the log says why");
        fprintf(f, "settings = %s\n", have_ini ? "triforce_options.ini" : "none (the game folder has no triforce_options.ini yet)");
        if (have_save) {
            const char *base = strrchr(zip, SEP[0]);
            fprintf(f, "save = %s (%d files, the save the next start would load)\n", base ? base + 1 : zip, files);
        } else {
            fprintf(f, "save = none (%s)\n", files == 0 ? "the game has not saved yet" : files == -1 ? "a file of it could not be read"
                                             : files == -2 ? "it is written in place in this run" : "its zip could not be written");
        }
        fprintf(f, "log tail = %s\n", have_log ? "log-tail.txt (the last 400 lines)" : "none (triforce_log.txt is not in the game folder)");
        const int whole = !fclose(f);
        snprintf(other, sizeof other, "%s" SEP "capture.txt", dir);
        if (!whole || !MoveFileExA(path, other, MOVEFILE_REPLACE_EXISTING)) f = NULL;
    }
    free(tail);
    hos_log("capture: bug capture %d written in %llu ms: picture %s, save %s, settings %s%s", asked.number,
            (unsigned long long)(GetTickCount64() - began), have_picture ? "yes" : "no", have_save ? "yes" : "no", have_ini ? "yes" : "no",
            f ? "" : "; capture.txt could not be written");
    snprintf(line, sizeof line, "Saved in %s%s", dir, have_picture ? "" : " (without a picture: the log says why)");
    hos_save_share_say(HOS_SHARE_CAPTURE, line);
    snprintf(line, sizeof line, "Bug capture saved: captures" SEP "%s", name);
    hos_save_share_say(HOS_SHARE_NOTICE, line);
    InterlockedExchange(&capture_state, 0);
}
