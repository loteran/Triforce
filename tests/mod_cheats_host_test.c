/* The mods' cheats on the host's side: host/mod_cheats.c's own text (mod_cheats_actual.inc, cut by tests/test_mod_cheats.py)
   against a stand-in game, a stand-in menu and a game folder of the test's. Run twice on the same folder: "first" switches
   cheats on and off, "second" is the next start. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- what host/mod_cheats.c takes from hos.h, hos_paths.h and windows.h ---- */
#define MAX_PATH 260
typedef unsigned long DWORD;
#define MOVEFILE_REPLACE_EXISTING 1
static char game_dir[400];
static DWORD hos_game_path_a(char *buf, DWORD size) { return (DWORD)snprintf(buf, size, "%s/Triforce.exe", game_dir); }
static int MoveFileExA(const char *from, const char *to, int flags) {
    (void)flags;
    remove(to);
    return rename(from, to) == 0;
}
#define IB 0x80000000ULL
#define MB 0x80002000ULL
#define HOS_HEAP_BASE 0x1000000000ULL
static uint8_t image[0x4000], heap[0x2000];
uint64_t hos_heap_size = sizeof heap;
const uint64_t hos_image_base = IB, hos_image_end = IB + sizeof image, hos_main_base = MB;
const uint64_t hos_code[][2] = {{IB, IB + 0x1000}, {MB, MB + 0x800}};
const int hos_code_count = 2;
static void *gp(uint64_t a) { return a >= HOS_HEAP_BASE ? (void *)(heap + (a - HOS_HEAP_BASE)) : (void *)(image + (a - IB)); }
#define GP(a) gp(a)
static uint64_t marked;   /* (the bytes told to the renderer's write watch) */
#define HOS_WRITTEN(p, n) ((void)(p), marked += (n))
static char logged[1 << 16];
static void hos_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const size_t at = strlen(logged);
    vsnprintf(logged + at, sizeof logged - at - 2, fmt, ap);
    va_end(ap);
    strcat(logged, "\n");
}
static int said(const char *text) {   /* how often the log has it */
    int n = 0;
    for (const char *p = logged; (p = strstr(p, text)) != NULL; p++) n++;
    return n;
}
volatile uint64_t hos_npad_buttons;

/* ---- the menu's side of HOS_OPT_MODCHEAT_* (gpu/vk/mod_cheat_rows.h is tested by itself) ---- */
#include "mod_cheats_constants.inc"
static int renderer;   /* 0: no renderer yet (rx_option answers what it is given) */
static char row_name[128][64], row_line[128][300], incoming[400];
static int row_bits[128], rows_added, rows_shown = -1, incoming_n;
static int asks[16], asks_n;
int rx_option(int id, int otherwise) {
    if (!renderer) return otherwise;
    if (id != HOS_OPT_MODCHEAT_ASK || !asks_n) return 0;
    const int ask = asks[0];
    memmove(asks, asks + 1, sizeof asks[0] * (size_t)--asks_n);
    return ask;
}
void rx_option_set(int id, int value) {
    CHECK(renderer);
    if (id == HOS_OPT_MODCHEAT_TEXT) {
        CHECK(value > 0 && value < 256 && incoming_n + 1 < (int)sizeof incoming);
        incoming[incoming_n++] = (char)value;
    } else if (id == HOS_OPT_MODCHEAT_ADD) {
        incoming[incoming_n] = 0, incoming_n = 0;
        char *feed = strchr(incoming, '\n');
        CHECK(feed && rows_shown < 0 && rows_added < 128);
        if (feed) *feed = 0, snprintf(row_line[rows_added], sizeof row_line[0], "%s", feed + 1);
        snprintf(row_name[rows_added], sizeof row_name[0], "%s", incoming);
        row_bits[rows_added++] = value;
    } else if (id == HOS_OPT_MODCHEAT_COUNT) {
        CHECK(rows_shown < 0);
        rows_shown = value;
    } else CHECK(!"an option of another feature set");
}
static void ask(int row, int on) { asks[asks_n++] = (row + 1) * 2 + on; }

#include "mod_cheats.h"
#include "mod_cheats_actual.inc"

static uint64_t h(unsigned off, unsigned n) { uint64_t v = 0; memcpy(&v, heap + off, n); return v; }
static void frames(int n) { while (n-- > 0) hos_mod_cheats_frame(); }
static char on_file[2000];
static const char *on_text(void) {   /* triforce_cheats.txt as it is now ("" when there is none) */
    char path[600];
    snprintf(path, sizeof path, "%s/triforce_cheats.txt", game_dir);
    FILE *f = fopen(path, "rb");
    const size_t n = f ? fread(on_file, 1, sizeof on_file - 1, f) : 0;
    if (f) fclose(f);
    on_file[n] = 0;
    return on_file;
}
static int image_untouched(void) {
    for (unsigned i = 0; i < sizeof image; i++)
        if (image[i] != 0x11) return 0;
    return 1;
}

/* rows, in the files' order, headings left out: 0 Master, 1 Rupees, 2 Hold ZL, 3 Code patch, 4 Pause, 5 Wild, 6 Null chain,
   7 Rupees (the name's second in A), 8 B's Rupees, 9 B's cheat with a long name */
enum { MASTER, RUPEES, HOLD_ZL, CODE_PATCH, PAUSE, WILD, NULL_CHAIN, RUPEES_2, B_RUPEES, B_LONG, ROWS };
#define LONG_CHEAT "A cheat with a name longer than a row shows whole"

static void read_files(const char *a, const char *b) {
    memset(image, 0x11, sizeof image);
    hos_mod_cheats_frame();   /* (no cheat file: nothing, and nothing asked of the renderer) */
    hos_mod_cheats_add("A", a);
    hos_mod_cheats_add("B", b);
    hos_mod_cheats_add("C", "no such file.txt");
    hos_mod_cheats_ready();
    CHECK(said("cheats: A: \"Code patch\" (line 9) cannot run: it writes the game's code (main+0x100), which is compiled ahead of time") == 1);
    CHECK(said("cheats: A: \"Pause\" (line 11) cannot run: opcode FF000000 is not supported") == 1);
    CHECK(said("cheats: A: 9 cheats in ") == 1 && said(": 5 to switch, 1 always on, 2 that cannot run, 1 headings") == 1);
    CHECK(said("cheats: B: 2 cheats in ") == 1 && said("no such file.txt: cannot be read") == 1);
    CHECK(said("cheats: 10 rows on the System tab") == 1);
}
static void named(int on_mask) {
    CHECK(rows_shown == ROWS && rows_added == ROWS);
    static const char *const names[ROWS] = {"Master", "Rupees", "Hold ZL", "Code patch", "Pause", "Wild", "Null chain", "Rupees", "Rupees", LONG_CHEAT};
    for (int r = 0; r < ROWS; r++) {
        CHECK(!strcmp(row_name[r], names[r]));
        const int want = r == MASTER ? HOS_MODCHEAT_ALWAYS : r == CODE_PATCH || r == PAUSE ? HOS_MODCHEAT_REFUSED : (on_mask >> r & 1) ? HOS_MODCHEAT_ON : 0;
        if (row_bits[r] != want) printf("FAIL: row %d (%s) has the bits %d, not %d\n", r, names[r], row_bits[r], want), fails++;
    }
    CHECK(!strcmp(row_line[RUPEES], "From the mod A.") && !strcmp(row_line[B_RUPEES], "From the mod B."));
    CHECK(!strcmp(row_line[B_LONG], "\"" LONG_CHEAT "\". From the mod B."));   /* (a long name whole, where there is room for it) */
    CHECK(!strcmp(row_line[MASTER], "Always on: the master code of the mod A."));
    CHECK(!strcmp(row_line[CODE_PATCH], "Not supported: it writes the game's code (main+0x100), which is compiled ahead of time. From the mod A."));
    CHECK(strstr(row_line[PAUSE], "Not supported: opcode FF000000 is not supported") && strstr(row_line[PAUSE], "From the mod A."));
}

static void first(void) {
    frames(3);   /* no renderer yet: the master code runs, nothing is named, nothing asked for is lost */
    CHECK(rows_shown < 0 && !marked && !on_text()[0]);
    renderer = 1;
    frames(1);
    named(0);
    frames(2);
    CHECK(rows_added == ROWS && h(0x600, 4) == 0 && !marked);   /* (named once; every cheat is off) */

    ask(RUPEES, 1);   /* its store goes through the register the master code set */
    frames(1);
    CHECK(h(0x600, 4) == 999 && marked == 4 && said("cheats: A: \"Rupees\" on") == 1);
    CHECK(strstr(on_text(), " A/Rupees\n") && !strstr(on_text(), "Rupees#2") && !strstr(on_text(), "B/Rupees"));
    CHECK(!strstr(on_text(), ".new") && on_text()[0] == '#');
    frames(5);
    CHECK(marked == 4);   /* (the same value again: not written, not marked) */
    heap[0x600] = 1;      /* (the game changes it: the cheat puts it back) */
    frames(1);
    CHECK(h(0x600, 4) == 999 && marked == 8);

    ask(HOLD_ZL, 1);   /* while a button is held */
    frames(2);
    CHECK(heap[0x700] == 0);
    hos_npad_buttons = 0x100;
    frames(1);
    CHECK(heap[0x700] == 1);
    hos_npad_buttons = 0;

    ask(CODE_PATCH, 1), ask(PAUSE, 1), ask(MASTER, 0), ask(ROWS, 1), ask(5000, 1);   /* rows that cannot be changed, and no rows */
    asks[asks_n++] = 1, asks[asks_n++] = -7;
    frames(8);
    CHECK(image_untouched() && !strstr(on_text(), "Code patch") && !strstr(on_text(), "Pause") && !strstr(on_text(), "Master"));
    heap[0x600] = 2;
    frames(1);
    CHECK(h(0x600, 4) == 999);   /* (the master code still runs) */

    ask(WILD, 1);   /* a store outside the game's memory: dropped, said once */
    const uint64_t before = marked;
    frames(4);
    CHECK(marked == before && said("cheats: A: \"Wild\" writes 0x1000100000, which is not the game's memory: nothing is written there") == 1);

    ask(NULL_CHAIN, 1);   /* a pointer that is not set: the cheat waits, said once, and goes on once it is */
    frames(4);
    CHECK(heap[0x710] == 0 && said("cheats: A: \"Null chain\" reads 0, which is not the game's memory") == 1);
    const uint64_t pointer = HOS_HEAP_BASE + 0x800;
    memcpy(heap + 0xF00, &pointer, 8);
    frames(1);
    CHECK(heap[0x710] == 1);

    ask(RUPEES_2, 1), ask(B_RUPEES, 1);   /* three cheats of one name, each its own switch */
    frames(2);
    CHECK(heap[0x720] == 2 && heap[0x730] == 3);
    CHECK(strstr(on_text(), " A/Rupees\n") && strstr(on_text(), " A/Rupees#2\n") && strstr(on_text(), " B/Rupees\n"));
    ask(RUPEES, 0);
    frames(1);
    heap[0x600] = 0, heap[0x601] = 0;
    frames(2);
    CHECK(h(0x600, 4) == 0 && said("cheats: A: \"Rupees\" off") == 1);
    /* what is kept: a line for each cheat that is on, and only those */
    const char *kept = on_text();
    int lines = 0;
    for (const char *p = kept; *p; p++) lines += *p == '\n';
    CHECK(kept[0] == '#' && lines == 1 + 5 && strstr(kept, " A/Hold ZL\n") && strstr(kept, " A/Wild\n") && strstr(kept, " A/Null chain\n"));
    CHECK(strstr(kept, " B/Rupees\n") && strstr(kept, " A/Rupees#2\n") && !strstr(kept, " A/Rupees\n"));
    CHECK(image_untouched());
}

static void second(void) {   /* the next start: what was on is on, before any row is touched */
    CHECK(said("5 cheats left on from before (triforce_cheats.txt)") == 1);
    const uint64_t pointer = HOS_HEAP_BASE + 0x800;
    memcpy(heap + 0xF00, &pointer, 8);
    hos_npad_buttons = 0x100;
    frames(1);   /* (no renderer yet: they run all the same) */
    CHECK(heap[0x700] == 1 && heap[0x710] == 1 && heap[0x720] == 2 && heap[0x730] == 3 && h(0x600, 4) == 0);
    renderer = 1;
    frames(1);
    named(1 << HOLD_ZL | 1 << WILD | 1 << NULL_CHAIN | 1 << RUPEES_2 | 1 << B_RUPEES);
    ask(RUPEES_2, 0);
    frames(1);
    CHECK(!strstr(on_text(), "Rupees#") && strstr(on_text(), " B/Rupees\n"));
    int lines = 0;
    for (const char *p = on_text(); *p; p++) lines += *p == '\n';
    CHECK(lines == 1 + 4);
}

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    snprintf(game_dir, sizeof game_dir, "%s", argv[1]);
    read_files(argv[2], argv[3]);
    if (!strcmp(argv[4], "first")) first();
    else second();
    if (fails) printf("the log:\n%s", logged);
    else printf("PASS\n");
    return fails != 0;
}
