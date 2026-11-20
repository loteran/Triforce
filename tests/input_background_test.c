/* Authored memory, clock and API spies; input_actual.inc contains the unmodified production path. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
#include "input_constants.inc"
/* API spies must not collide with Windows SDK/import-library names. */
#define GetCurrentProcessId fixture_GetCurrentProcessId
#define InterlockedCompareExchange64 fixture_InterlockedCompareExchange64

typedef void *HWND;
typedef uint32_t DWORD;
typedef intptr_t LONG_PTR;
typedef struct { uint64_t x[32]; } Ctx;
enum { GWL_EXSTYLE = -20, WS_EX_TOOLWINDOW = 0x80, WS_EX_NOACTIVATE = 0x8000000,
       VK_MENU = 0x12, VK_RETURN = 0x0d, VK_ESCAPE = 0x1b, VK_LEFT = 0x25, VK_UP, VK_RIGHT, VK_DOWN,
       VK_LBUTTON = 1, VK_RBUTTON = 2, VK_MBUTTON = 4, VK_XBUTTON1 = 5, VK_XBUTTON2 = 6,
       VK_TAB = 0x09, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_CAPITAL = 0x14 };   /* (the Walk key row's keys) */
static int front, menu_background, bootstrap_polls, pad_merges, key_queries, mouse_queries, focus_queries, commands;
static int no_window;   /* (no window in front at all: GetForegroundWindow answers NULL) */
typedef int64_t LONG64;
volatile LONG64 hos_present_count = 1000;
static int frame_reads;
LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 value, LONG64 expected) {
    const LONG64 old = *p;
    frame_reads++;
    if (old == expected) *p = value;
    return old;
}
static int64_t now_ns = 10000000000LL, sampling;
static int key_bind[HOS_KEY_N][2], pad_raw_now;   /* (the keys-only controls after the Switch's buttons) */
static unsigned char *memory;
static char last_log[256];
volatile int64_t hos_hid_last_read;
volatile int hos_nvn_sync_log_on;
static void *at(uint64_t address) { CHECK(address <= 512 - 8); return memory + address; }
#define GP(a) at(a)
#define G32(a) (*(uint32_t *)at(a))
#define G64(a) (*(uint64_t *)at(a))
#define ARG(n) (c->x[n])
#define RET(value) (c->x[0] = (uint64_t)(value))

/* (the Insert menu's Walk key row and Walk scale, 0 the scale's default, and whether Caps Lock, the row's first key, is held) */
static int walk_row, walk_scale, walk_held;
/* ((Triforce) the "options-back" case: our options open (rx_option 1) over the game's Options screen, the mouse's right
   button held, and the game's Back tapped for it) */
static int options_open, options_front, right_held;
static char tapped[64];
int rx_option(int id, int otherwise) {
    if (id == HOS_OPT_WALK_KEY) return walk_row;
    if (id == 1 && options_open) return 1;
    if (id == HOS_OPT_WALK_SCALE && walk_scale) return walk_scale;
    return id == 4 ? menu_background : id == 1 ? 0 : otherwise;
}
int64_t hos_now_ns(void) { return now_ns; }
void hos_log(const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(last_log, sizeof last_log, format, args); va_end(args);
}
HWND GetForegroundWindow(void) { focus_queries++; return no_window ? (HWND)0 : (HWND)(uintptr_t)(front ? 1 : 2); }
/* (the program in front, by its file name: production asks Windows) */
static void front_program(DWORD pid, char *name, int size) { (void)pid; snprintf(name, (size_t)size, "fixture.exe"); }
DWORD GetWindowThreadProcessId(HWND window, DWORD *pid) { *pid = window == (HWND)(uintptr_t)1 ? 10 : 20; return 1; }
DWORD GetCurrentProcessId(void) { return 10; }
LONG_PTR GetWindowLongPtrA(HWND window, int index) { (void)window; (void)index; return 0; }
int GetClassNameA(HWND window, char *text, int size) { (void)window; return snprintf(text, (size_t)size, "fixture"); }
short GetAsyncKeyState(int key) {
    key_queries++;
    return key == 'P' || key == 'D' || (walk_held && key == VK_CAPITAL) || (right_held && key == VK_RBUTTON) ? (short)-32768 : 0;
}
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]) {
    bootstrap_polls++; *buttons = 1ULL << B_Y; stick[0] = -31000; return 1;
}
void pads_read(uint64_t *buttons, int32_t stick[4]) {
    /* The physical backend boundary is a spy; actual routing merges its result with the script below. */
    pad_merges++; *buttons |= 1ULL << B_B; stick[0] = -12000; stick[2] = 8000;
}
void binds_load(void) {
    key_bind[B_X][0] = 'P';
    /* (the left stick's and the camera's keys are bindings too: the defaults, WASD and the arrows) */
    key_bind[HOS_KEY_MOVE_UP][0] = 'W', key_bind[HOS_KEY_MOVE_DOWN][0] = 'S', key_bind[HOS_KEY_MOVE_LEFT][0] = 'A';
    key_bind[HOS_KEY_MOVE_RIGHT][0] = 'D', key_bind[HOS_KEY_CAM_UP][0] = VK_UP, key_bind[HOS_KEY_CAM_DOWN][0] = VK_DOWN;
    key_bind[HOS_KEY_CAM_LEFT][0] = VK_LEFT, key_bind[HOS_KEY_CAM_RIGHT][0] = VK_RIGHT;
}
/* the native mouse look (no camera found: the motion goes to mouse_stick) and the mouse's sensitivity, which
   read_input calls */
int hos_mouse_native(void) { return -1; }
void hos_mouse_native_free(int free) { (void)free; }
void hos_mouse_native_stick(int held) { (void)held; }
static void mouse_setup(void) {}
static void mouse_sens_keys(void) {}
int mouse_stick(int32_t *x, int32_t *y, int held) { (void)x; (void)y; (void)held; mouse_queries++; return 0; }
/* (the options tabs inside the game's own Options screen, which read_input looks at: never open here, so the Back it
   would tap for the game is never asked for) */
int hos_binding(int set, int button, int slot) { (void)set; (void)button; (void)slot; return -1; }
int hos_oot_options_front(void) { return options_front; }
/* (the sprint button, which takes its press out of the buttons while Link plays: off here; and the pipeline crawl's
   lines of a live script, which no case writes) */
void hos_oot_sprint_pad(uint64_t *buttons) { (void)buttons; }
void hos_oot_live_crawl(const char *line) { (void)line; CHECK(!"unexpected crawl line"); }
void hos_hid_tap(const char *press) { CHECK(options_front && "unexpected tap of the game's Back"); snprintf(tapped, sizeof tapped, "%s", press); }
void rx_request_frame_dump(void) { commands++; }
void rx_set_screenshot_ms(int64_t ms) { (void)ms; commands++; }
void rx_request_rdoc_capture(int frames) { (void)frames; commands++; }
void rx_flash_dark_arm(void) { commands++; }

#include "input_actual.inc"

static void packet(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx) {
    static int64_t expected_sampling;
    Ctx context = {{0}};
    context.x[0] = 64; context.x[1] = 2; context.x[2] = 16; context.x[30] = 0x1234;
    G32(16) = NPAD_NO1;
    memset(memory + 64, 0xa5, 2 * STATE_SIZE + 8);
    npad_states(&context);
    CHECK(context.x[0] == 2);
    expected_sampling++;
    for (int i = 0; i < 2; i++) {
        const uint64_t address = 64 + (uint64_t)i * STATE_SIZE;
        CHECK(G64(address) == (uint64_t)(expected_sampling - i));
        CHECK(G64(address + 8) == buttons);
        CHECK((int32_t)G32(address + 16) == lx && (int32_t)G32(address + 20) == ly);
        CHECK((int32_t)G32(address + 24) == rx && G32(address + 28) == 0);
        CHECK(G32(address + 32) == ATTR_CONNECTED && G32(address + 36) == 0);
    }
    for (int i = 0; i < 8; i++) CHECK(memory[64 + 2 * STATE_SIZE + i] == 0xa5);
    CHECK(commands == 0);
    CHECK(hos_npad_buttons == buttons);   /* (the mods' cheats see the buttons the game was given: host/mod_cheats.c) */
}

static void append_live(const char *text) {
    const char *path = getenv("HOS_INPUT_LIVE"); CHECK(path);
    FILE *file = fopen(path, "ab"); CHECK(file);
    CHECK(fputs(text, file) >= 0 && fclose(file) == 0);
}

static void frame_clock_case(const char *name) {
    const int hold = !strcmp(name, "frame-hold");
    const int live = !strcmp(name, "live-frame");
    const int reuse = !strcmp(name, "reuse-frame");
    const int wall = !strcmp(name, "wall") || !strcmp(name, "zero") || !strcmp(name, "empty");
    CHECK(hold || live || reuse || wall);
    front = hold || wall;
    packet(hold || wall ? 1ULL << B_A : 0, 0, hold || wall ? 32767 : 0, 0);
    if (hold) {
        now_ns += 7000000000LL; /* no native present returns during a compile */
        packet(1ULL << B_A, 0, 32767, 0);
        CHECK(hos_hid_last_read == now_ns); /* HID diagnostics stay wall-clock */
        hos_present_count = 1011;
        packet(1ULL << B_A, 0, 32767, 0);
        hos_present_count = 1012; /* exactly 200 virtual ms, relative to initial frame 1000 */
        packet(0, 0, 0, 0);
        CHECK(pad_merges == 0 && key_queries == 0 && mouse_queries == 0);
        hos_present_count = 1311;
        packet(0, 0, 0, 0); /* exclusive margin still held at 5183 ms */
        hos_present_count = 1312;
        packet((1ULL << B_B) | (1ULL << B_X), 32767, 0, 8000);
        CHECK(pad_merges > 0 && frame_reads > 0);
    } else if (wall) {
        now_ns += 7000000000LL;
        packet((1ULL << B_B) | (1ULL << B_X), 32767, 0, 8000);
        CHECK(pad_merges > 0 && frame_reads == 0);
    } else if (live) {
        append_live("0:A+LUP/200"); now_ns += 30000000;
        packet(0, 0, 0, 0); /* incomplete live line remains unconsumed */
        hos_present_count = 1060;
        append_live("\n"); now_ns += 30000000;
        packet(1ULL << B_A, 0, 32767, 0);
        CHECK(presses[0].start_ms == 1000 && presses[0].end_ms == 1200);
        append_live("0:B/200\n"); now_ns += 5000000;
        packet(1ULL << B_A, 0, 32767, 0); /* file polling stays on the 30 ms wall clock */
        now_ns += 30000000;
        packet((1ULL << B_A) | (1ULL << B_B), 0, 32767, 0);
        CHECK(presses[1].start_ms == 1000);
        now_ns += 7000000000LL;
        packet((1ULL << B_A) | (1ULL << B_B), 0, 32767, 0);
        hos_present_count = 1072;
        packet(0, 0, 0, 0);
    } else {
        for (int i = 0; i < MAX_PRESSES; i++) CHECK(parse_presses("0:A/200", 0, 1) == 1);
        now_ns += 7000000000LL;
        CHECK(parse_presses("0:B/200", 0, 1) == 0);
        hos_present_count = 1012;
        CHECK(parse_presses("0:B/200", 200, 1) == 0); /* original strict reuse boundary */
        hos_present_count = 1013;
        CHECK(parse_presses("0:B/200", 216, 1) == 1);
        packet(1ULL << B_B, 0, 0, 0);
        CHECK(npresses == MAX_PRESSES);
    }
    CHECK(bootstrap_polls == 1);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    CHECK(!pads_in_background(NULL, 0));   /* out of the box: the window in front only */
    CHECK(pads_in_background(NULL, 1));    /* the menu's row */
    CHECK(pads_in_background("1", 0));     /* HOS_INPUT_BACKGROUND=1 without the menu */
    CHECK(!pads_in_background("0", 1));    /* HOS_INPUT_BACKGROUND=0 wins over the menu */
    memory = calloc(512, 1); CHECK(memory);
    const char *name = argv[1];
    if (!strcmp(name, "frame-hold") || !strcmp(name, "live-frame") || !strcmp(name, "reuse-frame") ||
        !strcmp(name, "wall") || !strcmp(name, "zero") || !strcmp(name, "empty")) {
        frame_clock_case(name);
        printf("PASS: actual input frame clock %s\n", name);
        free(memory);
        return 0;
    }
    if (!strcmp(name, "no-window")) {
        /* No window is in front for a moment, with the game's in front before it: the pads and the keys still count. For
           longer than a quarter of a second nothing counts, and everything counts again once the game's window is back */
        const uint64_t held = (1ULL << B_B) | (1ULL << B_X);
        front = 1;
        packet((1ULL << B_A) | held, 32767, 32767, 8000);
        now_ns += 201000000;
        no_window = 1;
        packet(held, 32767, 0, 8000);
        const int merges = pad_merges, keys = key_queries;
        now_ns += 251000000;
        packet(0, 0, 0, 0);
        CHECK(pad_merges == merges && key_queries == keys);
        CHECK(strstr(last_log, "game window not in front"));
        no_window = 0;
        now_ns += 3000000;
        packet(held, 32767, 0, 8000);
        CHECK(strstr(last_log, "was not in front for"));
        CHECK(bootstrap_polls == 1);
        printf("PASS: no window in front for a moment keeps the input %s\n", name);
        free(memory);
        return 0;
    }
    if (!strcmp(name, "options-back")) {
        /* (Triforce) Our tabs open inside the game's own Options screen, left: with the mouse's right button held (the menu's
           Back, as Esc is) the game's Back is tapped for it, as for the pad's B and Esc; left with nothing held (L or R, a
           click on the game's own tab), no tap */
        front = 1, options_front = 1;
        Ctx context = {{0}};
        for (int step = 0; step < 6; step++) {
            options_open = step == 1 || step == 4, right_held = step >= 4;
            context.x[0] = 64, context.x[1] = 2, context.x[2] = 16, context.x[30] = 0x1234;
            G32(16) = NPAD_NO1;
            npad_states(&context);
            now_ns += 16666667;
            if (step == 2) CHECK(tapped[0] == 0);
        }
        CHECK(!strcmp(tapped, "0:B/80"));
        printf("PASS: our tabs left with the right button are the game's Back too %s\n", name);
        free(memory);
        return 0;
    }
    if (!strcmp(name, "walk")) {
        /* (Triforce) The Walk key row. The script holds A and the left stick up; the pad holds B and its stick 12000 to
           the left; the keys hold P (X) and D (move right). Off, D pushes the stick all the way. Caps Lock chosen and not
           held: the same. Held: D pushes it 0.51 of the way, the row's default, and the script's push up is as it was;
           0.90 the same way; at 0.20 the pad's 12000 is the further of the two and is what the game gets, as of any two
           sticks. Off again with the key still held: all the way */
        const uint64_t all = (1ULL << B_A) | (1ULL << B_B) | (1ULL << B_X);
        front = 1;
        packet(all, 32767, 32767, 8000);
        walk_row = 1;
        packet(all, 32767, 32767, 8000);
        walk_held = 1;
        packet(all, 16711, 32767, 8000);
        walk_scale = 90;
        packet(all, 29490, 32767, 8000);
        walk_scale = 20;
        packet(all, -12000, 32767, 8000);
        walk_row = 0;
        packet(all, 32767, 32767, 8000);
        walk_row = 2, walk_scale = 0;   /* (Shift is the walk key, and it is Caps Lock that is held: no walk) */
        packet(all, 32767, 32767, 8000);
        CHECK(bootstrap_polls == 1);
        printf("PASS: the walk key scales the movement keys' stick alone %s\n", name);
        free(memory);
        return 0;
    }
    front = !strcmp(name, "foreground") || !strcmp(name, "exclusive-foreground");
    menu_background = !strcmp(name, "pads-menu") || !strcmp(name, "pads-off");
    const int exclusive = !strncmp(name, "exclusive-", 10);
    const int pads = !exclusive && (front || !strcmp(name, "pads-env") || !strcmp(name, "pads-menu"));
    const uint64_t physical = pads ? (1ULL << B_B) | (front ? 1ULL << B_X : 0) : 0;
    if (!strcmp(name, "live-background")) {
        packet(0, 0, 0, 0);
        append_live("0:A+LUP/200"); now_ns += 30000000;
        packet(0, 0, 0, 0); /* incomplete authored line is not consumed */
        append_live("\n"); now_ns += 30000000;
        packet(1ULL << B_A, 0, 32767, 0);
        now_ns += 200000000;
        packet(0, 0, 0, 0); /* consumed once, then released at the actual hold boundary */
    } else if (!strcmp(name, "no-script-background")) {
        packet(0, 0, 0, 0);
    } else {
        packet((1ULL << B_A) | physical, pads ? (front ? 32767 : -12000) : 0, 32767, pads ? 8000 : 0);
        now_ns += 201000000;
        packet(physical, pads ? (front ? 32767 : -12000) : 0, 0, pads ? 8000 : 0);
        if (exclusive) {
            CHECK(pad_merges == 0 && key_queries == 0 && mouse_queries == 0);
            now_ns += 5100000000LL;
            packet(front ? (1ULL << B_B) | (1ULL << B_X) : 0, front ? 32767 : 0, 0, front ? 8000 : 0);
        }
    }
    CHECK(bootstrap_polls == 1); /* production startup poll is allowed, but its Y/stick result is discarded */
    if (!front) {
        CHECK(key_queries == 0 && mouse_queries == 0);
        if (!pads) CHECK(pad_merges == 0);
    }
    if (!strcmp(name, "script-background")) CHECK(strstr(last_log, "game window not in front"));
    if (front) CHECK(pad_merges > 0 && key_queries > 0 && mouse_queries > 0);
    CHECK(focus_queries > 0);
    printf("PASS: actual scripted parse/merge/Npad route %s\n", name);
    free(memory);
    return 0;
}
