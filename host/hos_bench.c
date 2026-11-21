/* (Triforce, 11-19) Benchmark mode, for the launcher's benchmark: HOS_BENCHMARK=<results.json>. Nothing changes without it.

   The owner's words on the first, launcher-driven try: "It needs to be faster, there is a ton of just standing around. It
   needs to skip the main menu. All I saw was the game open, how am I supposed to know what it's doing". So the game drives
   itself here, reacting to what it shows, with no timed waits, and says what it does in a panel the renderer draws
   (gpu/vk/vk_menu.cpp draw_bench, through MkwVkHost.bench):

   1. Boot: as each of the start's three screens gets a focused button (host/oot_menu.c: the title 0x84c106e8, the file select
      0x84c0eba0, the file's box 0x84c0f840), A is pressed at once. The file select opens on the newest save's file (the
      game's own choice: the file it saved last); the file loaded and the newest by its caption's date are both logged.
      Another screen with a focused button (a first-start dialog) is logged and pressed too; after a few, the run fails with
      a clear error in the results.
   2. The tour, once Link is in play: per scene (HOS_BENCH_SCENES="1001:Hyrule Field,..." = the teleport actions of
      host/hos_main.c and their names; HOS_BENCH_SECONDS each, default 15) a teleport, its arrival (the map loaded and no event
      playing), 1.5 s settling (not counted), then every present's interval is measured while the camera turns steadily
      (the right stick at 35%, Link standing). The next teleport follows the moment a scene ends.
   3. The results (version 1): average fps, 1% and 0.1% lows (nearest rank, as gpu/vk/perf_stats.h), frames, seconds and the
      frame times in ~120 points per scene, the GPU and the options that shape the frame rate, written atomically (.new, then
      renamed); then the game quits as its window's close does (host/hos_relaunch.c: a save being committed finished, the
      renderer told), so the caches are written. Esc (the window in front) or the window closed by the launcher: written
      with "stopped": true.
   4. No save is written while it runs (host/nn_fs.c Commit), the hearts are protected (host/hos_main.c), and the player's
      devices do not count (host/nn_hid.c): the benchmark's presses count with the window behind other windows. */
#include "hos.h"
#include "hos_paths.h"
#include "hos_bench.h"
#include "../gpu/vk/vk_api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

void hos_hid_tap(const char *press);                                /* host/nn_hid.c */
void hos_hid_stick(int lx, int ly, int rx, int ry, int hold_ms);
const char *rx_describe(void);                                      /* host/rx_backend.c */
int rx_option(int id, int otherwise);
void hos_window_close(void);                                        /* host/display.c, host/posix/display_sdl.c */
extern const char *hos_user_dir;                                    /* host/nn_fs.c */

#define TITLE_VT 0x84c106e8ULL      /* the title's list (Continue, ...) */
#define FILES_VT 0x84c0eba0ULL      /* the file select: six files */
#define FILE_BOX_VT 0x84c0f840ULL   /* the chosen file's box: Load first */
#define MAX_SCENES 12
#define GRAPH_POINTS 120
#define SETTLE_NS 1500000000LL
#define PAN 11469                   /* (the right stick at 35%) */
#define HIST_BINS 2000              /* (the scene's frame times so far, 0.1 ms each up to 200 ms: the panel's 1% low) */

typedef struct { int action; char name[48]; } Scene;
typedef struct {
    int frames;
    double seconds, avg, low1, low01, max_ms;
    float points[GRAPH_POINTS];
    int npoints;
    int scene;   /* ((Triforce, 11-19) the scene it measured: a scene skipped before it shifts no name */
} Result;
typedef struct { uint64_t screen, vt; int stage, presses, focus_asks; int64_t press_ns, ask_ns; } Screen;

static struct {
    SRWLOCK lock;
    int on;
    char path[1024];
    double seconds;
    Scene scenes[MAX_SCENES];
    int nscenes;
    int phase, scene;   /* MKWVK_BENCH_*; the scene gone to or measured, 0-based */
    int64_t t_start, t_play, t_phase;
    /* boot */
    Screen screens[16];
    int nscreens, unknown, stage_max, slot, newest, slot_looked;   /* (slot: the file the game loaded; newest: by date) */
    /* travel */
    int want, tries, resends, saw_load;
    int64_t sent_ns, retry_ns, last_link_ns;
    /* measure */
    int64_t last_present, measure_t0;
    int64_t *iv;   /* (the scene's present intervals, ns) */
    int n, cap;
    unsigned hist[HIST_BINS + 1];
    /* the panel's graph and live rate: the newest intervals of any phase */
    float ring[MKWVK_BENCH_GRAPH];
    uint32_t ring_n;
    Result results[MAX_SCENES];
    int nresults;
    int skipped[MAX_SCENES], nskipped;   /* ((Triforce, 11-19) the scenes left out: skip_scene */
    char error[160];
    int written, closing;
    int64_t end_ns;
} B = {SRWLOCK_INIT};

int hos_bench_on(void) { return B.on; }
int hos_bench_input_only(void) { return B.on; }
int hos_bench_blocks_save(void) {
    if (B.on) hos_log("benchmark: the game's save commit left out (benchmark mode writes no save)");
    return B.on;
}

static void parse_scenes(const char *s) {
    B.nscenes = 0;
    while (s && *s && B.nscenes < MAX_SCENES) {
        while (*s == ' ' || *s == ',') s++;
        char *end;
        const long action = strtol(s, &end, 10);
        if (end == s) break;
        s = end;
        Scene *sc = &B.scenes[B.nscenes];
        sc->action = (int)action;
        snprintf(sc->name, sizeof sc->name, "Place %ld", action - 1000);
        if (*s == ':') {
            s++;
            size_t n = strcspn(s, ",");
            while (n && s[n - 1] == ' ') n--;
            if (n >= sizeof sc->name) n = sizeof sc->name - 1;
            memcpy(sc->name, s, n);
            sc->name[n] = 0;
            s += strcspn(s, ",");
        }
        if (action >= 1000 && action < 1100) B.nscenes++;
    }
}

void hos_bench_init(void) {
    const char *p = getenv("HOS_BENCHMARK");
    if (!p || !*p) return;
    snprintf(B.path, sizeof B.path, "%s", p);
    const char *sec = getenv("HOS_BENCH_SECONDS");
    B.seconds = sec && atof(sec) >= 1 ? atof(sec) : 15;
    const char *scenes = getenv("HOS_BENCH_SCENES");
    parse_scenes(scenes && *scenes ? scenes : "1001:Hyrule Field,1003:Castle Town Market,1000:Kokiri Forest,1007:Lake Hylia");
    if (!B.nscenes) parse_scenes("1001:Hyrule Field");
    B.slot = B.newest = -1;
    B.t_start = hos_now_ns();
    B.phase = MKWVK_BENCH_BOOT;
    B.on = 1;
}

/* ---- the newest save: <user>/save/<user id>/slot_0N/caption.sav. Its Int values (the save files' format: a 0x01020304 magic,
   (hash, value) pairs from 0x20 to the data area, a pair of hash 0 opening the next type's section, Int = type 2) hold the
   date it was saved: year 0x9811a3f7, month 0xdfd840d3, day 0xbd46f485, hour 0x23f3d75e, minute 0x27853bf7, second
   0x5ee50bed (read from the lab's saves, 11-19: a file saved at 08:28:27 on 11-19 says 2026, 11, 19, 8, 28, 27 there). The
   file's own time is no help: every commit writes all six files. For the log and the results only: the game's file select
   is a carousel whose entries are places, not files (entry 2 focused loaded file 6 in a lab run), so the newest file is not
   focused by its entry; the game itself opens on the newest file (its last saved), which the benchmark then loads (checked
   by the file it opens: hos_bench_file_opened) */
static int64_t caption_stamp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char *b = (unsigned char *)malloc(65536);
    const size_t n = b ? fread(b, 1, 65536, f) : 0;
    fclose(f);
    int64_t stamp = 0;
    uint32_t magic = 0, data = 0;
    if (n >= 0x20) memcpy(&magic, b, 4), memcpy(&data, b + 8, 4);
    if (magic == 0x01020304u && data <= n) {
        static const uint32_t keys[6] = {0x9811a3f7u, 0xdfd840d3u, 0xbd46f485u, 0x23f3d75eu, 0x27853bf7u, 0x5ee50bedu};
        int v[6] = {-1, -1, -1, -1, -1, -1};
        uint32_t type = 0;
        for (size_t o = 0x20; o + 8 <= data; o += 8) {
            uint32_t h, x;
            memcpy(&h, b + o, 4), memcpy(&x, b + o + 4, 4);
            if (!h) { type = x; continue; }
            if (type != 2) continue;
            for (int k = 0; k < 6; k++)
                if (h == keys[k]) v[k] = (int)x;
        }
        if (v[0] > 2000 && v[1] >= 1 && v[1] <= 12 && v[2] >= 1 && v[2] <= 31 && v[3] >= 0 && v[4] >= 0 && v[5] >= 0)
            stamp = ((((((int64_t)v[0] * 13 + v[1]) * 32 + v[2]) * 24 + v[3]) * 60 + v[4]) * 60 + v[5]);
    }
    free(b);
    return stamp;
}
static int64_t file_time(const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return -1;
    return (int64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
#else
    struct stat st;
    return stat(path, &st) ? -1 : (int64_t)st.st_mtime;
#endif
}
#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif
/* the newest file in one user's folder: *stamp its caption's date (0 none), *time its file's time */
static int newest_in(const char *dir, int64_t *stamp, int64_t *time) {
    int best = -1;
    *stamp = -1, *time = -1;
    for (int i = 0; i < 6; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s" SEP "slot_%02d" SEP "caption.sav", dir, i);
        const int64_t t = file_time(p);
        if (t < 0) continue;
        const int64_t s = caption_stamp(p);
        if (best < 0 || s > *stamp || (s == *stamp && t > *time)) best = i, *stamp = s, *time = t;
    }
    return best;
}
static int newest_slot(void) {
    if (!hos_user_dir) return -1;
    char root[1100];
    snprintf(root, sizeof root, "%s" SEP "save", hos_user_dir);
    int best = -1;
    int64_t best_stamp = -1, best_time = -1;
#ifdef _WIN32
    char pat[1200];
    snprintf(pat, sizeof pat, "%s\\*", root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        const char *name = fd.cFileName;
#else
    DIR *d = opendir(root);
    if (!d) return -1;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        const char *name = e->d_name;
#endif
        char dir[1200];
        snprintf(dir, sizeof dir, "%s" SEP "%s", root, name);
        int64_t s, t;
        const int i = newest_in(dir, &s, &t);
        if (i >= 0 && (best < 0 || s > best_stamp || (s == best_stamp && t > best_time))) best = i, best_stamp = s, best_time = t;
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif
    hos_log("benchmark: the newest save is file %d (%s)", best + 1, best_stamp > 0 ? "by the date in its caption.sav" : "by its file's time");
    return best;
}

/* ---- the results ---- */
static int cmp_i64(const void *a, const void *b) {
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}
/* of n values sorted upwards, the one at q by the nearest rank (gpu/vk/perf_stats.h percentile) */
static int64_t rank_of(const int64_t *v, int n, double q) {
    if (n <= 0) return 0;
    int r = (int)(q * n);
    if ((double)r < q * n) r++;
    if (r < 1) r = 1;
    if (r > n) r = n;
    return v[r - 1];
}
static void scene_done(void) {   /* (under the lock) */
    if (B.nresults >= MAX_SCENES) return;
    Result *r = &B.results[B.nresults++];
    memset(r, 0, sizeof *r);
    r->scene = B.scene;
    r->frames = B.n;
    int64_t sum = 0;
    for (int i = 0; i < B.n; i++) sum += B.iv[i];
    r->seconds = sum / 1e9;
    r->avg = sum ? B.n * 1e9 / (double)sum : 0;
    /* (the graph's points: GRAPH_POINTS runs of frames in order, each its mean) */
    r->npoints = B.n < GRAPH_POINTS ? B.n : GRAPH_POINTS;
    for (int p = 0; p < r->npoints; p++) {
        const int a = (int)((int64_t)p * B.n / r->npoints), z = (int)((int64_t)(p + 1) * B.n / r->npoints);
        int64_t s = 0;
        for (int i = a; i < z; i++) s += B.iv[i];
        r->points[p] = z > a ? (float)(s / 1e6 / (z - a)) : 0;
    }
    if (B.n) {
        qsort(B.iv, (size_t)B.n, sizeof *B.iv, cmp_i64);
        r->max_ms = B.iv[B.n - 1] / 1e6;
        if (B.n >= 100) {   /* (as the performance overlay: no lows under 100 frames) */
            const int64_t p99 = rank_of(B.iv, B.n, 0.99), p999 = rank_of(B.iv, B.n, 0.999);
            r->low1 = p99 ? 1e9 / (double)p99 : 0;
            r->low01 = p999 ? 1e9 / (double)p999 : 0;
        } else {
            r->low1 = r->low01 = -1;
        }
    }
    hos_log("benchmark: scene %d of %d, %s: %.1f fps average, 1%% low %.1f, 0.1%% low %.1f, %d frames in %.2f s", B.nresults,
            B.nscenes, B.scenes[r->scene].name, r->avg, r->low1, r->low01, r->frames, r->seconds);
}
static void json_text(FILE *f, const char *s) {
    fputc('"', f);
    for (; s && *s; s++) {
        const unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
        else if (ch < 32) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}
static double bench_finite(double v) { return v == v && v < 1e12 && v > -1e12 ? v : 0; }
/* the options that shape the frame rate, as the options file has them (beside the game: triforce_options.ini) */
static void json_settings(FILE *out) {
    static const char *const keys[] = {"frame_rate_cap", "vsync", "fullscreen", "output_res", "resolution", "quality_preset",
                                       "upscaling", "dlss", "dlss_model", "frame_generation", "dlss_frame_generation",
                                       "frame_pacing", "steady_frame_rate", "nvidia_reflex", "amd_anti-lag_2", "texture_detail",
                                       "texture_filtering", "anti-aliasing", "sharpness", "hdr", "replay_helper_threads"};
    char path[MAX_PATH];
    const DWORD len = hos_game_path_a(path, MAX_PATH);
    char *slash = len ? (strrchr(path, 92) ? strrchr(path, 92) : strrchr(path, 47)) : NULL;
    FILE *f = NULL;
    if (slash) {
        snprintf(slash + 1, (size_t)(path + MAX_PATH - slash - 1), "triforce_options.ini");
        f = fopen(path, "r");
    }
    fprintf(out, "{");
    int first = 1;
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        for (unsigned k = 0; k < sizeof keys / sizeof *keys; k++) {
            if (strcmp(line, keys[k])) continue;
            char *end;
            strtol(v, &end, 10);
            fprintf(out, "%s", first ? "" : ",");
            first = 0;
            json_text(out, line);
            if (*v && !*end) fprintf(out, ":%s", v);
            else fputc(':', out), json_text(out, v);
        }
    }
    if (f) fclose(f);
    fprintf(out, "}");
}
static void write_results(int stopped) {   /* (under the lock; once) */
    if (B.written || !B.path[0]) return;
    B.written = 1;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.new", B.path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        hos_log("benchmark: the results could not be written to %s", tmp);
        return;
    }
    char gpu[160];
    snprintf(gpu, sizeof gpu, "%s", rx_describe() ? rx_describe() : "");
    char *paren = strstr(gpu, " (Vulkan");
    if (paren) *paren = 0;
    const int64_t now = hos_now_ns();
    fprintf(f, "{\"version\":1,\"stopped\":%s,", stopped ? "true" : "false");
    if (B.error[0]) fprintf(f, "\"error\":"), json_text(f, B.error), fputc(',', f);
    fprintf(f, "\"gpu\":"), json_text(f, gpu);
    fprintf(f, ",\"seconds_per_scene\":%.1f,\"boot_seconds\":%.1f,\"total_seconds\":%.1f,\"file\":%d,\"scenes\":[", B.seconds,
            B.t_play ? (B.t_play - B.t_start) / 1e9 : -1.0, (now - B.t_start) / 1e9, B.slot + 1);
    for (int i = 0; i < B.nresults; i++) {
        const Result *r = &B.results[i];
        fprintf(f, "%s{\"name\":", i ? "," : "");
        json_text(f, B.scenes[r->scene].name);
        fprintf(f, ",\"avg\":%.2f,\"low1\":%.2f,\"low01\":%.2f,\"frames\":%d,\"seconds\":%.3f,\"max_ms\":%.2f,\"frametimes_ms\":[",
                bench_finite(r->avg), bench_finite(r->low1), bench_finite(r->low01), r->frames, bench_finite(r->seconds), bench_finite(r->max_ms));
        for (int p = 0; p < r->npoints; p++) fprintf(f, "%s%.2f", p ? "," : "", bench_finite(r->points[p]));
        fprintf(f, "]}");
    }
    fprintf(f, "],\"skipped\":[");
    for (int i = 0; i < B.nskipped; i++) fprintf(f, "%s", i ? "," : ""), json_text(f, B.scenes[B.skipped[i]].name);
    fprintf(f, "],\"settings\":");
    json_settings(f);
    fprintf(f, "}\n");
    const int ok = fflush(f) == 0;
    fclose(f);
    if (ok && MoveFileExA(tmp, B.path, MOVEFILE_REPLACE_EXISTING))
        hos_log("benchmark: results written to %s (%s, %d of %d scenes, %.1f s since the start)", B.path,
                stopped ? "stopped" : "finished", B.nresults, B.nscenes, (now - B.t_start) / 1e9);
    else
        hos_log("benchmark: the results could not be moved to %s", B.path);
    hos_log_flush();
}
static void fail(int64_t now, const char *why) {   /* (under the lock) */
    if (B.phase >= MKWVK_BENCH_DONE) return;
    snprintf(B.error, sizeof B.error, "%s", why);
    hos_log("benchmark: FAILED: %s", why);
    B.phase = MKWVK_BENCH_FAILED, B.end_ns = now;
    write_results(1);
}
/* ((Triforce, 11-19) #bug-tracker "Game kept refusing to teleport to Lake Hylia while running the benchmark" (RTX 5090): one place
   the game would not take Link to failed the whole run. A place it refuses (an event or a fight keeps the event manager busy)
   or never arrives at is left out now, said in the log and the results' "skipped", and the tour goes on; it fails only
   when nothing at all was measured (under the lock) */
static void go_scene(int64_t now);
static void skip_scene(int64_t now, const char *why) {
    if (B.phase >= MKWVK_BENCH_DONE) return;
    hos_log("benchmark: %s %s: left out, the tour goes on", why, B.scenes[B.scene].name);
    if (B.nskipped < MAX_SCENES) B.skipped[B.nskipped++] = B.scene;
    if (++B.scene < B.nscenes) {
        go_scene(now);
        return;
    }
    if (B.nresults) {
        B.phase = MKWVK_BENCH_DONE, B.end_ns = now;
        write_results(0);
    } else {
        char msg[160];
        snprintf(msg, sizeof msg, "%s %s", why, B.scenes[B.scene - 1].name);
        fail(now, msg);
    }
}
static void stop(int64_t now, const char *why) {   /* (under the lock) */
    if (B.phase >= MKWVK_BENCH_DONE) return;
    hos_log("benchmark: stopped (%s)", why);
    B.phase = MKWVK_BENCH_STOPPED, B.end_ns = now;
    write_results(1);
}

/* host/nn_fs.c, each file the game opens: a save file's progress.sav is the file it loads ("save:/slot_0N/progress.sav") */
void hos_bench_file_opened(const char *guest) {
    if (!B.on || !guest || B.phase != MKWVK_BENCH_BOOT) return;
    const char *p = strstr(guest, "slot_");
    if (!p || p[5] < '0' || p[5] > '9' || p[6] < '0' || p[6] > '9' || strcmp(p + 7, "/progress.sav")) return;
    const int slot = (p[5] - '0') * 10 + (p[6] - '0');
    if (slot == B.slot) return;
    B.slot = slot;
    hos_log("benchmark: the game loads file %d%s", slot + 1,
            B.newest < 0 ? "" : slot == B.newest ? " (the newest by its date)" : " (NOT the newest by its date)");
}

/* ---- the boot: the start's screens ---- */
static const char *stage_name(int stage) {
    return stage == 0 ? "the title" : stage == 1 ? "the file select" : stage == 2 ? "the file's box" : "a screen it does not know";
}
int hos_bench_menu(uint64_t screen, uint64_t vt, int focus, int focused_entry, int entries, int *entry) {
    if (!B.on || !focus || B.phase != MKWVK_BENCH_BOOT) return HOS_BENCH_MENU_NONE;
    const int64_t now = hos_now_ns();
    int act = HOS_BENCH_MENU_NONE;
    AcquireSRWLockExclusive(&B.lock);
    Screen *s = NULL;
    for (int i = 0; i < B.nscreens; i++)
        if (B.screens[i].screen == screen && B.screens[i].vt == vt) s = &B.screens[i];
    if (!s && B.nscreens < (int)(sizeof B.screens / sizeof *B.screens)) {
        s = &B.screens[B.nscreens++];
        memset(s, 0, sizeof *s);
        s->screen = screen, s->vt = vt;
        s->stage = vt == TITLE_VT ? 0 : vt == FILES_VT ? 1 : vt == FILE_BOX_VT ? 2 : 3;
        hos_log("benchmark: %s has a focused button (screen %#llx, vtable %#llx, %d entries, entry %d focused) at %.1f s",
                stage_name(s->stage), (unsigned long long)screen, (unsigned long long)vt, entries, focused_entry, (now - B.t_start) / 1e9);
        if (s->stage == 3 && ++B.unknown > 3) {
            char why[160];
            snprintf(why, sizeof why, "The game showed screens the benchmark does not know at its start (the last: vtable %#llx)",
                     (unsigned long long)vt);
            fail(now, why);
            ReleaseSRWLockExclusive(&B.lock);
            return HOS_BENCH_MENU_NONE;
        }
    }
    /* (a screen of an earlier step still fading out while the next is up: never pressed again, the press would be the next's) */
    if (s && s->stage < 3 && s->stage < B.stage_max) s = NULL;
    if (s && s->stage < 3 && s->stage > B.stage_max) B.stage_max = s->stage;
    if (s && s->stage == 1 && !B.slot_looked) B.slot_looked = 1, B.newest = newest_slot();
    (void)entry;
    if (s) {
        /* (A at once; again if the screen is still there with its focus after a while: a press the screen was not taking yet.
           The box is given longer: Load takes a moment before the box goes) */
        const int64_t again = s->stage == 2 ? 5000000000LL : 700000000LL;   /* (the title drops the first, lab 11-19) */
        if (!s->presses || now - s->press_ns > again) {
            if (s->presses >= 4) {
                char why[160];
                snprintf(why, sizeof why, "The game did not take the benchmark's presses on %s", stage_name(s->stage));
                fail(now, why);
            } else {
                s->presses++, s->press_ns = now;
                act = HOS_BENCH_MENU_PRESS_A;
                hos_log("benchmark: A pressed on %s%s", stage_name(s->stage), s->presses > 1 ? " again" : "");
            }
        }
    }
    ReleaseSRWLockExclusive(&B.lock);
    return act;
}

/* ---- the tour: Link's update ---- */
int hos_bench_link_wants_state(void) {
    const int p = B.on ? B.phase : MKWVK_BENCH_OFF;
    return p == MKWVK_BENCH_BOOT || p == MKWVK_BENCH_TRAVEL;
}
static void go_scene(int64_t now) {   /* (under the lock) the next scene's teleport */
    B.phase = MKWVK_BENCH_TRAVEL, B.t_phase = now;
    B.want = 1, B.tries = 0, B.resends = 0, B.saw_load = 0, B.retry_ns = 0, B.sent_ns = 0;
    hos_log("benchmark: going to %s (scene %d of %d)", B.scenes[B.scene].name, B.scene + 1, B.nscenes);
}
int hos_bench_link(int64_t now, uint64_t actor, const char *map, int loading, int event) {
    (void)actor;
    if (!B.on) return -1;
    int want = -1;
    AcquireSRWLockExclusive(&B.lock);
    const int64_t gap = B.last_link_ns ? now - B.last_link_ns : 0;
    B.last_link_ns = now;
    if (B.phase == MKWVK_BENCH_BOOT && !loading && !event && map && map[0]) {
        B.t_play = now;
        hos_log("benchmark: Link is in play on \"%s\" after %.1f s", map, (now - B.t_start) / 1e9);
        B.scene = 0;
        go_scene(now);
    }
    if (B.phase == MKWVK_BENCH_TRAVEL) {
        if (B.want) {
            if (now >= B.retry_ns && !loading) want = B.scenes[B.scene].action - 1000;
        } else {
            if (loading || gap > 250000000LL) B.saw_load = 1;
            if (B.saw_load && !loading && !event && map && map[0]) {
                hos_log("benchmark: arrived at %s (\"%s\") %.1f s after the teleport", B.scenes[B.scene].name, map, (now - B.sent_ns) / 1e9);
                B.phase = MKWVK_BENCH_SETTLE, B.t_phase = now;
            } else if (!B.saw_load && now - B.sent_ns > 12000000000LL) {
                if (++B.resends > 3) {
                    skip_scene(now, "The benchmark could not reach");
                } else {
                    hos_log("benchmark: no map change 12 s after the teleport to %s: asked again", B.scenes[B.scene].name);
                    B.want = 1, B.retry_ns = 0;
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&B.lock);
    return want;
}
void hos_bench_teleport_sent(int index, int ok) {
    (void)index;
    const int64_t now = hos_now_ns();
    AcquireSRWLockExclusive(&B.lock);
    if (B.phase == MKWVK_BENCH_TRAVEL && B.want) {
        if (ok) {
            B.want = 0, B.sent_ns = now, B.saw_load = 0;
        } else if (++B.tries > 20) {   /* (an event playing refuses it: asked again every half second, 10 s at most, then left out) */
            skip_scene(now, "The game kept refusing the teleport to");
        } else {
            B.retry_ns = now + 500000000LL;
        }
    }
    ReleaseSRWLockExclusive(&B.lock);
}

/* ---- each present: the measurement, the camera, the clock's steps, Esc ---- */
static void ring_put(float ms) { B.ring[B.ring_n++ % MKWVK_BENCH_GRAPH] = ms; }
void hos_bench_present(int64_t now) {
    if (!B.on) return;
    int close = 0;
    AcquireSRWLockExclusive(&B.lock);
    static int64_t last;
    const int64_t d = last ? now - last : 0;
    last = now;
    if (d > 0 && B.phase != MKWVK_BENCH_BOOT) ring_put((float)(d / 1e6));
    static int told;
    if (!told) {
        told = 1;
        hos_log("benchmark: on: %d scenes of %.1f s, results to %s", B.nscenes, B.seconds, B.path);
    }
    switch (B.phase) {
    case MKWVK_BENCH_BOOT: {
        const char *e = getenv("HOS_BENCH_BOOT_S");
        const double limit = e && atof(e) > 0 ? atof(e) : 900;
        if (now - B.t_start > (int64_t)(limit * 1e9)) fail(now, "The game did not reach play from its start in time");
        break;
    }
    case MKWVK_BENCH_TRAVEL:
        if (now - B.t_phase > 120000000000LL) {
            char why[160];
            snprintf(why, sizeof why, "The game did not finish loading %s", B.scenes[B.scene].name);
            fail(now, why);
        }
        break;
    case MKWVK_BENCH_SETTLE:
        hos_hid_stick(0, 0, PAN, 0, 250);
        if (now - B.t_phase >= SETTLE_NS) {
            B.phase = MKWVK_BENCH_MEASURE, B.measure_t0 = now, B.n = 0;
            memset(B.hist, 0, sizeof B.hist);
        }
        break;
    case MKWVK_BENCH_MEASURE:
        hos_hid_stick(0, 0, PAN, 0, 250);
        if (B.measure_t0 != now && d > 0) {
            if (B.n == B.cap) {
                const int cap = B.cap ? B.cap * 2 : 16384;
                int64_t *p = (int64_t *)realloc(B.iv, (size_t)cap * sizeof *p);
                if (p) B.iv = p, B.cap = cap;
            }
            if (B.n < B.cap) B.iv[B.n++] = d;
            const int64_t bin = d / 100000;
            B.hist[bin < HIST_BINS ? bin : HIST_BINS]++;
        }
        if (now - B.measure_t0 >= (int64_t)(B.seconds * 1e9)) {
            scene_done();
            if (++B.scene >= B.nscenes) {
                B.phase = MKWVK_BENCH_DONE, B.end_ns = now;
                write_results(0);
            } else {
                go_scene(now);
            }
        }
        break;
    default:   /* (done, stopped, failed: the panel's last words for a moment, then the game quits as at its window's close) */
        if (!B.closing && now - B.end_ns > (B.phase == MKWVK_BENCH_FAILED ? 4000000000LL : 1200000000LL)) B.closing = 1, close = 1;
        break;
    }
    /* (Esc, the game's window in front and the options closed: the run stops) */
    if (B.phase < MKWVK_BENCH_DONE && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) && !rx_option(1, 0)) {
        DWORD pid = 0;
        HWND w = GetForegroundWindow();
        if (w) GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId()) stop(now, "Esc pressed");
    }
    ReleaseSRWLockExclusive(&B.lock);
    if (close) {
        hos_log("benchmark: the game quits");
        hos_window_close();
    }
}

void hos_bench_closing(void) {
    if (!B.on) return;
    AcquireSRWLockExclusive(&B.lock);
    if (B.phase < MKWVK_BENCH_DONE) stop(hos_now_ns(), "the window was closed");
    ReleaseSRWLockExclusive(&B.lock);
}

/* ---- the panel ---- */
int hos_bench_state(MkwVkBench *out) {
    if (!B.on || !out) return 0;
    AcquireSRWLockExclusive(&B.lock);
    memset(out, 0, sizeof *out);
    out->phase = B.phase;
    out->scenes = B.nscenes;
    const int sc = B.scene < B.nscenes ? B.scene : B.nscenes - 1;
    out->scene = B.phase == MKWVK_BENCH_BOOT ? 0 : sc + 1;
    snprintf(out->name, sizeof out->name, "%s", B.phase == MKWVK_BENCH_BOOT ? "" : B.scenes[sc].name);
    const int64_t now = hos_now_ns();
    double frac = 0;
    if (B.phase == MKWVK_BENCH_MEASURE) frac = (now - B.measure_t0) / (B.seconds * 1e9);
    if (frac > 1) frac = 1;
    out->progress = B.phase >= MKWVK_BENCH_DONE && B.phase != MKWVK_BENCH_DONE ? (float)B.nresults / B.nscenes
                    : B.phase == MKWVK_BENCH_DONE ? 1.0f : B.phase == MKWVK_BENCH_BOOT ? 0.0f : (float)((sc + frac) / B.nscenes);
    if (B.phase == MKWVK_BENCH_FAILED) snprintf(out->line, sizeof out->line, "%s", B.error);
    /* (the newest frame times, oldest first; the rate of the last second of them) */
    const int have = B.ring_n < MKWVK_BENCH_GRAPH ? (int)B.ring_n : MKWVK_BENCH_GRAPH;
    for (int i = 0; i < have; i++) out->graph_ms[i] = B.ring[(B.ring_n - (uint32_t)have + (uint32_t)i) % MKWVK_BENCH_GRAPH];
    out->graph_n = have;
    double sum = 0;
    int n = 0;
    for (int i = have - 1; i >= 0 && sum < 1000; i--) sum += out->graph_ms[i], n++;
    out->fps = sum > 0 ? (float)(n * 1000.0 / sum) : 0;
    /* (the scene's 1% low so far, from its histogram: the bin the slowest 1% reach) */
    out->low1 = -1;
    if (B.phase == MKWVK_BENCH_DONE && B.nresults) {   /* (finished: the whole run's average and its lowest 1% low) */
        int64_t frames = 0;
        double secs = 0, low = 1e9;
        for (int i = 0; i < B.nresults; i++) {
            frames += B.results[i].frames, secs += B.results[i].seconds;
            if (B.results[i].low1 > 0 && B.results[i].low1 < low) low = B.results[i].low1;
        }
        out->fps = secs > 0 ? (float)(frames / secs) : 0;
        out->low1 = low < 1e9 ? (float)low : -1;
    }
    if (B.phase == MKWVK_BENCH_MEASURE && B.n >= 100) {
        const int want = B.n - (int)ceil(0.99 * B.n) + 1;   /* (how many are at or above the 99th percentile's rank) */
        int above = 0;
        for (int b = HIST_BINS; b >= 0; b--) {
            above += (int)B.hist[b];
            if (above >= want) {
                out->low1 = (float)(1e4 / (b + 0.5));
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&B.lock);
    return 1;
}
