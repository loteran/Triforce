/* The mouse in the game's own menus: the title and its lists, the pause menu, the panel of
   Key Items, Threads of Time and the Map, the options, the save files' dialogs. The mouse frees itself over them
   (display.c), points at their buttons (the focus as the pad moves it), clicks them (A), backs out (the right button:
   B) and scrolls their lists. nx2/oot_patches.py has two of the game's functions call here first (the supported build
   only):
   - f_802ed660 (in the pause menu's vtable at 0x84c0bc18), the pause menu's handler of a pane getting the focus: it
     matches the pane's name against the pages (PaItemMenu_00, PaEquipmentMenu_00, PaCollectMenu_00, PaSystemMenu_00)
     and hands the pane to the page (f_802ebd20 on the Items page: the info panel). The pause menu's page, for its tabs.
   - f_802f1380, every menu screen's update (each frame, before its UI controller at +56 runs the buttons' requests):
     the screens are tracked, and the mouse is for the one in front (hos_oot_menu_tick).
   The focus setter is f_8088ffc0(screen, button, flag), as the game's screens call it (f_80795090): it finds the button's
   entry in the screen's list (+136 the head, +144 the first, +156 the entry's offset from its node; entry+24 the button)
   and hands it to the focus manager (*(screen+32)+96, its vtable +48), when the manager takes the player's input.
   HOS_MENU_TRACE=1: the focus changes, the screens and what the mouse does logged. */
#include "hos.h"
#include "guest_bounds.h"   /* ((Triforce) hos_range_ok: set_item_key's buffer) */
/* (Triforce) The game's own Options screen (the layout SystemOption_00; the title's Options and the pause menu's are the one
   screen), by its vtable (HOS_MENU_TRACE in a headless run, 2026-11-10: the screen of 15 entries that starts to run when
   Options is chosen, from the title and from the pause menu alike, and stops with Back; L and R do nothing on it). While
   it runs the renderer is told each frame (rx_option_set HOS_OPT_GAME_OPTIONS): the menu draws its tabs over this
   screen's header, the game's own list the first of them (gpu/vk/vk_menu.cpp), and nn_hid.c passes Back on when one of
   our tabs is left with it. HOS_OPTIONS_TABS=0 keeps the renderer untold.
   ((Triforce, 11-20) Told as 2, not 1, when it was chosen from the pause menu: the pause menu's screen (its vtable,
   PAUSE_SCREEN_VTABLE) ran within half a second before this run of the Options screen began. One screen, but its footer's
   guides band may be another overlay's there (MenuOverlay_00's ends at 2110 units right of the middle, SystemOverlay_00's
   at 2160: the renderer's guides plate ends with it, game_grid.h guide_band_x1) */
#define OPTIONS_SCREEN_VTABLE 0x84c0f108ULL
#define PAUSE_SCREEN_VTABLE 0x84c0baa8ULL
#define OPTIONS_FROM_PAUSE_NS 500000000LL   /* (the pause menu's last update, at most this long before the Options' first) */
void rx_option_set(int id, int value);   /* rx_backend.c */
#if defined(_WIN32) && !defined(HOS_OOT_MENU_NO_MOUSE)   /* (the macro: tests/test_oot_menu_mouse.py builds the other half on Windows) */
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gpu/vk/vk_api.h"   /* (HOS_MOUSE_*) */
#include "hos_bench.h"   /* ((Triforce, 11-19) benchmark mode) */

/* A guest read, of the pointers the game's objects hold: inside the guest's space (below 1 MB: a null pointer's), it is
   read as the lifted code reads it; a page there not committed yet is committed on the touch (mkw_glue.c
   commit_on_touch), so a stray pointer costs a page, not a fault. Not ReadProcessMemory: a call into the kernel each,
   hundreds a frame */
static int peek(uint64_t a, void *dst, size_t n) {
    if (a < 0x100000 || a >= HOS_GUEST_SPACE || n > HOS_GUEST_SPACE - a) return 0;
    memcpy(dst, GP(a), n);
    return 1;
}
static uint64_t peek64(uint64_t a) {
    uint64_t v = 0;
    return peek(a, &v, 8) ? v : 0;
}
/* a printable C string at a (else "-") */
static const char *text_at(uint64_t a, char *buf, size_t cap) {
    if (!peek(a, buf, cap - 1)) return "-";
    buf[cap - 1] = 0;
    for (size_t i = 0; buf[i]; i++)
        if ((unsigned char)buf[i] < 32 || (unsigned char)buf[i] > 126) return i > 2 ? (buf[i] = 0, buf) : "-";
    return buf[0] ? buf : "-";
}

static int trace = -1;
/* (the screen f_802ed660 ran for: written under mouse_lock, read by every screen's update under seen_lock or none,
   so read and written whole) */
static volatile LONG64 pause_screen;
static uint64_t pause_screen_now(void) { return (uint64_t)InterlockedCompareExchange64(&pause_screen, 0, 0); }
static uint64_t focus_page;     /* (the focused pane's page object: a change is a page switch) */
static volatile LONG ticks;     /* (the pause menu's updates since its focus came: for the trace only) */
static int tab_goal = -1, tab_pages, tab_from = -9;   /* (a clicked tab: the page to step to, and how many the menu has) */
static int64_t goal_ns;
static uint64_t focused_button;   /* (the last pane f_802ed660 was handed) */
static uint64_t asked;   /* (the button the mouse asked the focus for, until it has it) */
static int page_now = -1;   /* (the shown page: 0 Collection, 1 Equipment, 2 Items, 3 System; L and R step through them) */

/* the screen's list of entries: fn(entry, button) for each, at most max */
static int each_entry(uint64_t screen, int max, void (*fn)(int i, uint64_t entry, uint64_t button, void *arg), void *arg) {
    const uint64_t head = screen + 136;
    int32_t off = 0;
    peek(screen + 156, &off, 4);
    uint64_t node = peek64(screen + 144);
    int i = 0;
    for (; node && node != head && i < max; i++) {
        const uint64_t entry = node - (uint64_t)(int64_t)off;
        if (fn) fn(i, entry, peek64(entry + 24), arg);
        node = peek64(node + 8);
    }
    return i;
}

/* the game's focus setter on a button (on the game's thread, at a screen's update) */
static int set_focus(Ctx *c, uint64_t screen, uint64_t button) {
    A64Fn fn = a64_lookup(0x8088ffc0ULL);
    if (!fn) return -1;
    const Ctx saved = *c;
    c->x[0] = screen, c->x[1] = button, c->x[2] = 0;
    c->x[30] = 0;
    fn(c);
    const int r = (int)(c->x[0] & 1);
    *c = saved;
    return r;
}

/* A call of a guest function from the host, on the game's thread (x0, x1 in; x0 out, s0 and s1 too: a position) */
static uint64_t guest_call(Ctx *c, uint64_t fn_addr, uint64_t a0, uint64_t a1, float *s0, float *s1) {
    A64Fn fn = a64_lookup(fn_addr);
    if (!fn) return 0;
    const Ctx saved = *c;
    c->x[0] = a0, c->x[1] = a1, c->x[30] = 0;
    fn(c);
    const uint64_t r = c->x[0];
    if (s0) memcpy(s0, &c->vreg[0][0], 4);
    if (s1) memcpy(s1, &c->vreg[1][0], 4);
    *c = saved;
    return r;
}

/* Key Items' items where the page shows them, as the page finds them to place its cursor (f_833fdaa0): the item
   database (**0x84c9e0d0), its lists 4 and 3 (f_80a8f690: +8 the count, +16 the entries); an entry's key (f_8002a520,
   f_80d666a0(.., 0), f_8002a4c0: a string, its text first) and its pane's name (f_80a963c0: its text first); the pane in
   the screen's layout (f_8078e2b0(*(screen+40), name)) and its position (f_8078fcb0). fn(key, x, y) for each */
static int item_panes(Ctx *c, uint64_t screen, void (*fn)(const char *key, float x, float y, void *arg), void *arg) {
    const uint64_t db = peek64(peek64(0x84c9e0d0ULL)), layout = peek64(screen + 40);
    if (!db || !layout) return 0;
    int n = 0;
    for (int which = 4; which >= 3; which--) {
        const uint64_t list = guest_call(c, 0x80a8f690ULL, db, (uint64_t)which, NULL, NULL);
        int32_t count = 0;
        peek(list + 8, &count, 4);
        const uint64_t entries = peek64(list + 16);
        for (int i = 0; i < count && i < 32; i++) {
            const uint64_t e = peek64(entries + 8ull * (unsigned)i);
            if (!e) continue;
            uint64_t k = guest_call(c, 0x8002a520ULL, e, 0, NULL, NULL);
            k = guest_call(c, 0x80d666a0ULL, k, 0, NULL, NULL);
            k = guest_call(c, 0x8002a4c0ULL, k, 0, NULL, NULL);
            char key[40];
            if (!k || !peek(peek64(k), key, sizeof key - 1)) continue;
            key[sizeof key - 1] = 0;
            const uint64_t named = guest_call(c, 0x80a963c0ULL, e, 0, NULL, NULL);
            const uint64_t pane = named ? guest_call(c, 0x8078e2b0ULL, layout, peek64(named), NULL, NULL) : 0;
            float x = 0, y = 0;
            if (!pane) continue;
            guest_call(c, 0x8078fcb0ULL, pane, 0, &x, &y);
            if (x == x && y == y) fn(key, x, y, arg), n++;
        }
    }
    return n;
}

/* Key Items' setter: the item's key into the page's key buffer (*(screen+760), its room at +768) and the page's refresh
   f_833fda00(screen, screen+760) (the cursor's key and the item's name and description; the page's update places the
   cursor from the key next frame), as the page does when it opens. Only while the page browses (+840: 0; 1 its
   details shown) */
static int set_item_key(Ctx *c, uint64_t screen, const char *key) {
    const uint64_t buf = peek64(screen + 760);
    int32_t room = 0, mode = 1;
    peek(screen + 768, &room, 4);
    peek(screen + 840, &mode, 4);
    const size_t n = strlen(key);
    A64Fn fn = a64_lookup(0x833fda00ULL);
    /* ((Triforce) the buffer's end without buf + n + 1, which wrapped past 2^64 for a pointer read wrong) */
    if (!fn || !buf || !hos_range_ok(buf, n + 1, HOS_GUEST_SPACE) || room <= (int32_t)n || mode != 0) return 0;
    memcpy(GP(buf), key, n + 1);
    const Ctx saved = *c;
    c->x[0] = screen, c->x[1] = screen + 760, c->x[30] = 0;
    fn(c);
    *c = saved;
    return 1;
}

static SRWLOCK mouse_lock;
static volatile DWORD mouse_owner;
/* A focus change the hook could not hand over at once (mouse_lock busy: mouse_tick holds it on another of the game's
   threads, across its calls into the game): kept, the latest only, for mouse_tick to take. The hook never waits for
   mouse_lock, as that thread's game code might wait on this one. pending_lock is held only to copy these words */
static SRWLOCK pending_lock = SRWLOCK_INIT;
static struct { uint64_t screen, pane, lr; int set; } pending;
static void focus_locked(uint64_t screen, uint64_t pane, uint64_t lr);
void hos_oot_menu_focus(Ctx *c, uint64_t screen, uint64_t pane, uint64_t lr) {
    (void)c;
    if (trace < 0) trace = getenv("HOS_MENU_TRACE") != NULL;
    if (mouse_owner == GetCurrentThreadId()) {   /* (inside mouse_tick's setter, on its thread: the lock is this thread's) */
        focus_locked(screen, pane, lr);
        return;
    }
    const int mine = TryAcquireSRWLockExclusive(&mouse_lock);
    AcquireSRWLockExclusive(&pending_lock);
    pending.screen = screen, pending.pane = pane, pending.lr = lr, pending.set = !mine;   /* (a newer one than any kept) */
    ReleaseSRWLockExclusive(&pending_lock);
    if (!mine) return;
    focus_locked(screen, pane, lr);
    ReleaseSRWLockExclusive(&mouse_lock);
}
/* (mouse_tick, under mouse_lock) the focus change kept for it, if any */
static void focus_pending(void) {
    AcquireSRWLockExclusive(&pending_lock);
    const int set = pending.set;
    const uint64_t screen = pending.screen, pane = pending.pane, lr = pending.lr;
    pending.set = 0;
    ReleaseSRWLockExclusive(&pending_lock);
    if (set) focus_locked(screen, pane, lr);
}
static void focus_locked(uint64_t screen, uint64_t pane, uint64_t lr) {
    if (screen != pause_screen_now()) InterlockedExchange64(&pause_screen, (LONG64)screen), InterlockedExchange(&ticks, 0);
    uint8_t flags = 0;
    peek(pane + 65, &flags, 1);
    const uint64_t at = (flags & 0x20) ? peek64(pane + 32) + 48 : pane + 32;
    const uint64_t obj = peek64(at);
    focus_page = obj;
    focused_button = pane, asked = 0;
    /* (f_807923a0's name: obj+128 -> +248 -> +24) */
    char a[96], b[64];
    {
        static const char *const pages[] = {"PaCollectMenu_00", "PaEquipmentMenu_00", "PaItemMenu_00", "PaSystemMenu_00"};
        const char *page = text_at(peek64(peek64(peek64(obj + 128) + 248) + 24), a, sizeof a);
        for (int i = 0; i < 4; i++)
            if (!strcmp(page, pages[i])) page_now = i;
    }
    if (!trace) return;
    hos_log("menu: focus (caller %#llx, tick %u) screen %#llx pane %#llx %s page %#llx %s; owner screen %#llx",
            (unsigned long long)lr, (unsigned)ticks, (unsigned long long)screen, (unsigned long long)pane,
            text_at(peek64(pane + 0x18), b, sizeof b), (unsigned long long)obj,
            text_at(peek64(peek64(peek64(obj + 128) + 248) + 24), a, sizeof a),
            (unsigned long long)peek64(peek64(pane + 32) + 40));
}

/* ---- the mouse. The layout's units: 3840 x 2160, the origin at the screen's centre, y up; the window shows it fit
   (the larger of the two scales) and centred */
void hos_mouse_game_menu(void);   /* (display.c) */
void hos_mouse_game_menu_gone(void);
int hos_mouse_game_pointer(int *x, int *y, int *w, int *h, uint32_t *pressed, int *wheel);
void hos_hid_tap(const char *press);   /* (nn_hid.c) */
void hos_hid_stick(int lx, int ly, int rx, int ry, int hold_ms);
void hos_hid_drag(int dx, int dy, int right);   /* (nn_hid.c: as the camera's mouse; the left stick or the right) */
/* The panel of Key Items, Threads of Time and Map (Minus opens it; L to the left, R to the right; pages 0, 1, 2), as
   the screens told apart by vtable: Key Items' screen 0x84c07798 and Threads of Time's
   0x84c04a18 (its list of threads: N_SlideList_00, the framework's buttons) each run only while their page is shown;
   the Map page has no screen of its own: the panel's frame 0x84c08aa0 runs alone (it runs under the pause menu too).
   The mouse is the page's while it is shown (no focused button on Key Items or the Map: the stones, the floors and the
   world map are not the framework's buttons). 0x84c08300, its 30 Pa_MelodyIcon_00, is the game's HUD of songs */
#define PANEL_FRAME_VTABLE 0x84c08aa0ULL
static volatile int map_page;   /* (hos_oot_menu_tick: the frame runs, and neither other page nor the pause menu) */
static int panel_page_vt(uint64_t vt) {
    return vt == 0x84c07798ULL ? 0 : vt == 0x84c04a18ULL ? 1 : vt == PANEL_FRAME_VTABLE && map_page ? 2 : -1;
}
static int panel_page(uint64_t screen) { return panel_page_vt(peek64(screen)); }
/* The game's HUD screen (0x84c08300, the songs' icons; it runs through the play) holds the map page's floors (a
   search of memory, in Lord Jabu-Jabu's Belly): +0x1368 the floor chosen (2F 1, 1F 0, B1 -1), +0x136c how
   many floors, +0x1370 the lowest; the top is the lowest + the count - 1. Kept as the screens are seen */
#define HUD_SCREEN_VTABLE 0x84c08300ULL
static volatile LONG64 options_seen_ns;
int hos_oot_options_front(void) {
    const LONG64 t = InterlockedCompareExchange64(&options_seen_ns, 0, 0);
    return t && (LONG64)hos_now_ns() - t < 200000000LL;
}
/* (Triforce) the renderer is told of the Options screen (its tabs over the header): HOS_OPTIONS_TABS=0 keeps it untold */
static int options_tabs_on(void) {
    static int tabs = -1;
    if (tabs < 0) {
        const char *e = getenv("HOS_OPTIONS_TABS");
        tabs = !(e && *e == '0');
    }
    return tabs;
}
static volatile uint64_t hud_screen;
static int map_floors(int *cur, int *top, int *bottom) {
    int32_t f[3];
    if (!hud_screen || !peek(hud_screen + 0x1368, f, sizeof f) || f[1] < 1 || f[1] > 12) return 0;
    *cur = f[0], *bottom = f[2], *top = f[2] + f[1] - 1;
    return *cur >= *bottom && *cur <= *top;
}

typedef struct { float x0, y0, x1, y1; } Rect;
/* (Key Items' items, in their grid's order: item_found puts the page's own positions of them in) */
static const char *const item_keys[9] = {"Item_Medal_Shadow", "Item_Medal_Light", "Item_Medal_Forest", "Item_Stone_Forest",
                                         "Item_Stone_Fire", "Item_Stone_Water", "Item_Medal_Spirit", "Item_Medal_Water",
                                         "Item_Medal_Fire"};
static void item_found(const char *key, float x, float y, void *arg) {
    struct { int *found; float *x, *y; } *a = arg;
    for (int i = 0; i < 9; i++) {
        /* (the game's strings hold a length: the text need not end in a NUL, so the key's end is where a name's can't go) */
        const size_t n = strlen(item_keys[i]);
        const char after = key[n];
        if (!strncmp(key, item_keys[i], n) && !isalnum((unsigned char)after) && after != '_')
            *a->found |= 1 << i, a->x[i] = x, a->y[i] = y;
    }
}
/* a button's rectangle in the layout's units (0 when it has none or is hidden) */
static int button_rect(uint64_t button, Rect *r);

typedef struct { float x, y; uint64_t hit; float area; int n; uint64_t button[128]; Rect rect[128]; } Hit;
static void shown_entry(int i, uint64_t entry, uint64_t button, void *arg) {
    (void)i, (void)entry;
    Hit *h = arg;
    if (h->n < 128 && button_rect(button, &h->rect[h->n])) h->button[h->n++] = button;
}

/* The boxes as the mouse takes them, from the game's: a box under 100 x 100 (the title's: 60 x 60, a cursor's mark at the
   middle of a line of text) made 1100 wide; then boxes one above the other (centres less than 60 apart across) grown to
   meet halfway across a gap under 240, and the column's ends by as much as the gap they have (no dead lines between
   the rows of a list, and its first and last rows as tall as the others) */
static void list_boxes(Hit *h) {
    for (int i = 0; i < h->n; i++) {
        Rect *r = &h->rect[i];
        if (r->x1 - r->x0 < 100 && r->y1 - r->y0 < 100) {
            const float cx = (r->x0 + r->x1) / 2;
            r->x0 = cx - 550, r->x1 = cx + 550;
        }
    }
    Rect out[128];
    for (int i = 0; i < h->n; i++) {
        const Rect *r = &h->rect[i];
        const float cx = (r->x0 + r->x1) / 2;
        float above = -1, below = -1;   /* (the gaps to the nearest box above and below in the column) */
        for (int j = 0; j < h->n; j++) {
            const Rect *o = &h->rect[j];
            const float ox = (o->x0 + o->x1) / 2;
            if (j == i || ox - cx > 60 || cx - ox > 60) continue;
            const float up = o->y0 - r->y1, down = r->y0 - o->y1;
            if (up >= 0 && up < 240 && (above < 0 || up < above)) above = up;
            if (down >= 0 && down < 240 && (below < 0 || down < below)) below = down;
        }
        out[i] = *r;
        if (above >= 0) out[i].y1 += above / 2;
        if (below >= 0) out[i].y0 -= below / 2;
        if (above >= 0 && below < 0) out[i].y0 -= above / 2;   /* (an end of the column) */
        if (below >= 0 && above < 0) out[i].y1 += below / 2;
    }
    memcpy(h->rect, out, (size_t)h->n * sizeof *out);
}

/* (Triforce) The game's own Options screen (OPTIONS_SCREEN_VTABLE): its rows as the mouse takes them. The screen keeps a
   widget for each row in one of three lists by the row's kind, as its focus handler (f_83423f00, the vtable's +0x170) and
   its setup (f_83424450) walk them by the kind in the row's entry (+4: 0, 1, 2): +712 the count and +720 the widgets of
   kind 0, +728 and +736 of kind 1, +696 and +704 of kind 2. A widget's button is f_80df5410(*(*(widget+24)+56),
   *(widget+32)), as f_833eff40 and the focus handler find it: the one the focus handler is handed, the button of the
   screen's list (its focus at +56). What a kind is, from their setups: kind 1's (f_833ef590) reads the row's count of values
   (entry+16) and kind 0's (f_833eff50) does not: the two-choice toggles (On | Off) and the choices of more (< value >);
   kind 2, the third look the screen has, the plates (amiibo, ZELDA NOTES; the screen's query f_83424580 asks whether the
   focus is on one). (Read from the lifted code, not traced: HOS_MENU_TRACE logs the kind each click finds.) The value's
   panel, in the layout's units (from the screen as shown, as gpu/vk/vk_menu.cpp draws its own rows like it: the list's
   middle at x -730, the panel 804 wide with its middle 500 right of that): x -632 to 172, a toggle's halves meeting at
   0.632 of it (x -124), a choice's arrows at its ends. The rows are shown from y 760 down to -960 (the header above, 200
   units: our tabs over it; the guides below): a row laid out past that (scrolled away) is not pointed at */
enum { OPT_KIND_NONE = -1, OPT_TOGGLE = 0, OPT_CHOICE = 1, OPT_PLATE = 2 };
#define OPT_VALUE_X0 (-632.0f)
#define OPT_VALUE_X1 172.0f
#define OPT_TOGGLE_MEET (-124.0f)
#define OPT_ROWS_TOP 760.0f
#define OPT_ROWS_BOTTOM (-960.0f)
#define OPT_HEADER_Y 880.0f
static int options_screen(uint64_t screen) { return peek64(screen) == OPTIONS_SCREEN_VTABLE; }
static int options_row_kind(Ctx *c, uint64_t screen, uint64_t button) {
    static const struct { int at, kind; } lists[] = {{712, OPT_TOGGLE}, {728, OPT_CHOICE}, {696, OPT_PLATE}};
    for (int l = 0; l < 3; l++) {
        int32_t n = 0;
        peek(screen + (uint64_t)lists[l].at, &n, 4);
        const uint64_t widgets = peek64(screen + (uint64_t)lists[l].at + 8);
        for (int i = 0; i < n && i < 64; i++) {
            const uint64_t w = peek64(widgets + 8ull * (unsigned)i), holder = peek64(w + 24), pane = peek64(holder + 56);
            if (w && pane && guest_call(c, 0x80df5410ULL, pane, peek64(w + 32), NULL, NULL) == button) return lists[l].kind;
        }
    }
    return OPT_KIND_NONE;
}
/* the pad's press a click on an Options row is, at x (the layout's units): on a toggle's or a choice's value the left part
   Left and the right part Right (a toggle's half is chosen so, a choice steps down or up; at an end the game does nothing);
   on a plate, a value's name, or a row of no kind found, A */
static const char *options_click(int kind, float x) {
    if ((kind != OPT_TOGGLE && kind != OPT_CHOICE) || x < OPT_VALUE_X0 || x > OPT_VALUE_X1) return "A";
    return x < (kind == OPT_TOGGLE ? OPT_TOGGLE_MEET : (OPT_VALUE_X0 + OPT_VALUE_X1) / 2) ? "LEFT" : "RIGHT";
}

static void hit_test(uint64_t screen, Hit *h) {
    h->n = 0, h->hit = 0;
    each_entry(screen, 400, shown_entry, h);
    list_boxes(h);
    if (h->y > -1080 && h->y < -960 && h->x >= 1600 && h->x < 1900) return;   /* (the "Back" prompt's: no button's) */
    const int options = options_screen(screen);
    for (int i = 0; i < h->n; i++) {
        const Rect *r = &h->rect[i];
        if (h->x < r->x0 || h->x > r->x1 || h->y < r->y0 || h->y > r->y1) continue;
        if (options && ((r->y0 + r->y1) / 2 > OPT_ROWS_TOP || (r->y0 + r->y1) / 2 < OPT_ROWS_BOTTOM)) continue;   /* (scrolled away) */
        const float area = (r->x1 - r->x0) * (r->y1 - r->y0);
        if (!h->hit || area < h->area) h->hit = h->button[i], h->area = area;   /* (the smallest: a slot inside a panel) */
    }
}

/* The tab bar across the top (no buttons of the screen's list: the pages' names and the L and R signs), in the layout's
   units, measured from the menu as shown (the Items page): the names centred at about -696, -228, 240, 708 (468 apart),
   L at -1002 and R at 1014; the boxes meet halfway between */
enum { TAB_L = 4, TAB_R = 5 };
static int tab_at(float x, float y) {
    if (y < 900 || y > 1080) return -1;
    if (x >= -1100 && x < -926) return TAB_L;
    if (x >= -926 && x < 942) return x < -462 ? 0 : x < 6 ? 1 : x < 474 ? 2 : 3;
    if (x >= 942 && x <= 1100) return TAB_R;
    return -1;
}

static void focus_entry(int i, uint64_t entry, uint64_t button, void *arg);   /* (below: a focused button shown) */

/* The mouse's state below is one, and the screens' updates run on several of the game's threads: one at a time (a
   frame the lock is busy is passed, not waited for); the focus hook's writes take it too */
static SRWLOCK mouse_lock = SRWLOCK_INIT;
static void mouse_tick_locked(Ctx *c, uint64_t screen);
static int mods_entry_hit(float x, float y);   /* ((Triforce) below: the Mods entry under a list's last button) */
static int quit_entry_hit(float x, float y);   /* ((Triforce, 11-19) and Quit under it, the System page's only */
int rx_option(int id, int otherwise);   /* rx_backend.c */
/* (mouse_owner: the thread in mouse_tick: the game's focus handler may run inside its setter) */
static void mouse_tick(Ctx *c, uint64_t screen) {
    if (!TryAcquireSRWLockExclusive(&mouse_lock)) return;
    mouse_owner = GetCurrentThreadId();
    focus_pending();
    mouse_tick_locked(c, screen);
    mouse_owner = 0;
    ReleaseSRWLockExclusive(&mouse_lock);
}
/* The R button's item choice (0x84c0d430, N_SlideList_00: a carousel of the items in a row, shown while R is held in
   play, turned by the D-pad's or the left stick's left and right, not wrapping; its items move from one end to the
   other as it turns, so no row stays the next). The mouse stays the camera's (the menu does not free it: no cursor, no
   pointing); the wheel turns it (hos_mouse_wheel_take, captured or not): the D-pad's right (the wheel down) or left, a
   notch each. Each press after the last one queued has been let go (60 ms down, 60 up: a notch the next frame would
   otherwise press while the last is held, and the game sees one press), at most 0.72 s ahead, so the row stops soon
   after the wheel */
#define ITEM_CHOICE_VTABLE 0x84c0d430ULL
int hos_mouse_wheel_take(void);   /* (display.c) */
static void item_choice_tick(void) {
    static int64_t dpad_free_ns;   /* (when the last press queued here has been let go) */
    const int wheel = hos_mouse_wheel_take();
    if (!wheel) return;
    const int64_t now = hos_now_ns();
    const int k = wheel < 0 ? -wheel : wheel;
    char taps[128];
    int n = 0;
    if (dpad_free_ns < now) dpad_free_ns = now;
    for (int i = 0; i < k && i < 6 && dpad_free_ns - now < 720000000; i++) {
        n += snprintf(taps + n, sizeof taps - (size_t)n, "%s%.3f:%s/60", n ? "," : "", (double)(dpad_free_ns - now) / 1e9,
                      wheel < 0 ? "RIGHT" : "LEFT");
        dpad_free_ns += 120000000;
    }
    if (n) hos_hid_tap(taps);
    if (trace) hos_log("menu: wheel %d: the item choice turned %s", wheel, wheel < 0 ? "right" : "left");
}

static void mouse_tick_locked(Ctx *c, uint64_t screen) {
    if (peek64(screen) == ITEM_CHOICE_VTABLE) {
        item_choice_tick();
        return;
    }
    hos_mouse_game_menu();
    int px, py, w, h, wheel;
    uint32_t pressed;
    const int got = hos_mouse_game_pointer(&px, &py, &w, &h, &pressed, &wheel);
    if (!got) return;
    const int hidden = got == 2;   /* (the cursor hidden while the pad is in use: the wheel only, no pointing) */
    const int pause = screen == pause_screen_now();
    const float scale = 3840.0f / (float)w > 2160.0f / (float)h ? 3840.0f / (float)w : 2160.0f / (float)h;
    static Hit hit;
    hit.x = (float)(px - w / 2) * scale, hit.y = (float)(h / 2 - py) * scale, hit.hit = 0;
    static int last_x = -1, last_y = -1;
    int moved = !hidden && (px != last_x || py != last_y);
    if (!hidden) last_x = px, last_y = py;
    /* The panel's pages: the left button dragged pans the map and the tapestry (the left stick, against the drag: the
       picture follows the mouse) and turns Key Items' stone (the right stick, with the drag), as the camera's mouse
       turns it (hos_hid_drag); pressed and let go without a drag it is a click, at its release. The middle button is Y
       on the map (the world map and back). The wheel off a list zooms (the right stick up or down, 0.12 s a notch:
       the world map's two steps) */
    const int page = panel_page(screen), map = page == 2, keys = page == 0;
    /* The pause menu's Items page: the chosen item's model at the right, turned left and right by the right stick (as
       Key Items' stones are): a drag there turns it, off the buttons; the page's slots at the left (to x about 100),
       the tabs above (y 900), the prompts below (-960) */
    const int item_model = pause && page_now == 2, pan = page >= 0 || item_model;
    static int ldown, ldrag, lx0, ly0, llx, lly;
    if (pan) {
        const int held = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const int was_down = ldown;
        int grab = !item_model;
        if (item_model && (pressed & HOS_MOUSE_LEFT) && !ldown && hit.x > 300 && hit.x < 1900 && hit.y > -900 && hit.y < 880) {
            hit_test(screen, &hit);
            grab = !hit.hit;
        }
        if ((pressed & HOS_MOUSE_LEFT) && !ldown && grab) ldown = 1, ldrag = 0, lx0 = llx = px, ly0 = lly = py;
        if (ldown) pressed &= ~(uint32_t)HOS_MOUSE_LEFT;   /* (a click is at the release) */
        if (ldown && held) {
            if (!ldrag && (px - lx0 > 6 || lx0 - px > 6 || py - ly0 > 6 || ly0 - py > 6)) {
                ldrag = 1;
                if (trace) hos_log("menu: drag from (%d, %d)", lx0, ly0);
            }
            if (ldrag && (px != llx || py != lly)) {   /* (against the drag: the picture follows the mouse) */
                if (keys) hos_hid_drag(px - llx, -(py - lly), 1);
                else if (item_model) hos_hid_drag(px - llx, 0, 1);   /* (left and right only) */
                else hos_hid_drag(-(px - llx), py - lly, 0);
                static int64_t logged;
                if (trace && hos_now_ns() - logged > 250000000)
                    logged = hos_now_ns(), hos_log("menu: drag: %d, %d pixels", px - llx, py - lly);
            }
            llx = px, lly = py;
        } else if (ldown && was_down) {
            if (!ldrag) {   /* (a click: where it was pressed) */
                pressed |= HOS_MOUSE_LEFT;
                px = lx0, py = ly0;
                hit.x = (float)(px - w / 2) * scale, hit.y = (float)(h / 2 - py) * scale;
            }
            if (trace) hos_log("menu: left button up: %s", ldrag ? "the drag's end" : "a click");
            ldown = 0;
        }
        if (map && (pressed & HOS_MOUSE_MIDDLE)) hos_hid_tap("0:Y/80");
        if (ldown) moved = 0;   /* (no hover while dragging) */
    }
    /* the wheel: the pause menu's pages (L, R). Elsewhere it scrolls a list: the focus to the row shown last that way
       (the lowest for down), then the pad's down (up) a notch each, which moves the list on by a row as at its end;
       then the focus back to the row the mouse is over */
    static int64_t refocus_ns;
    static uint64_t scroll_row;   /* (the row the wheel gave the focus, out of view) */
    static float scroll_lo, scroll_hi;
    const int64_t now = hos_now_ns();
    if (wheel) {
        char taps[128];
        int n = 0;
        const int k = wheel < 0 ? -wheel : wheel;
        float at = 0;
        int list = !keys, pan_off = keys;   /* (the wheel scrolls a list, or turns the pages; nothing on Key Items) */
        /* The Map page, anywhere: a dungeon's floors (the pad's up and down, a notch each) and the world map's zoom (the
           right stick, below): each view takes its own and lets the other be (the world map has nothing on the pad's up
           and down). The view shown is not known */
        if (map) {
            const char *dir = wheel > 0 ? "UP" : "DOWN";
            for (int i = 0; i < k && i < 4; i++)
                n += snprintf(taps + n, sizeof taps - (size_t)n, "%s%.2f:%s/75", n ? "," : "", i * 0.25, dir);
            if (trace) hos_log("menu: map wheel %d: the floors (%s) and the zoom", wheel, dir);
            list = 0;
        }
        if ((map || page == 1) && !pan_off) {   /* (off the list, or no row of it focused: the world map's, the tapestry's: the zoom) */
            int focused = 0;
            each_entry(screen, 400, focus_entry, &focused);
            /* (Threads of Time: its list focused, the wheel scrolls it wherever the mouse is; the tapestry shown (the
               list without a focused row) zooms. The map: the list's rows under the mouse scroll it, else the zoom) */
            if (focused && page != 1) hit_test(screen, &hit);
            if (!focused || (page != 1 && !hit.hit)) {
                hos_hid_stick(0, 0, 0, wheel > 0 ? 32767 : -32767, 120 * (k < 6 ? k : 6)), list = 0;
                if (trace) hos_log("menu: map wheel %d: the zoom (right stick %s for %d ms)", wheel, wheel > 0 ? "up" : "down", 120 * (k < 6 ? k : 6));
            }
            hit.hit = 0;
        }
        if (list && !pause) {
            /* A list's rows in their order down the screen. The wheel's target is the row the pad's down would bring
               in from the lowest row shown (up: the highest), a notch each: the first rows off the screen that way (a
               scrolled list lays out all its items, or keeps a spare row for the next). Given the focus, the list
               scrolls it in, without the pad's sound; the focus goes back to the row under the mouse once the list has
               moved. With no row off the screen that way (a short list: a dungeon's floors, or a list's end) the focus
               steps from the focused row to the next, and stays */
            hit_test(screen, &hit);
            int order[128], n_rows = 0, at_focus = -1;
            for (int i = 0; i < hit.n; i++) {
                int j = n_rows++;
                const float cy = (hit.rect[i].y0 + hit.rect[i].y1) / 2;
                while (j > 0 && (hit.rect[order[j - 1]].y0 + hit.rect[order[j - 1]].y1) / 2 < cy) order[j] = order[j - 1], j--;
                order[j] = i;
            }
            for (int j = 0; j < n_rows; j++) {
                uint8_t st = 0;
                peek(hit.button[order[j]] + 56, &st, 1);
                if (st && at_focus < 0) at_focus = j;
            }
            const int step = wheel < 0 ? 1 : -1, want = (k < 3 ? k : 3) * step;
            /* (the rows in view: the screen's height, or Threads of Time's list frame, y -756 to 594 (from the page as
               shown): the game takes a row as shown once its centre is in it, and scrolls a focused row only that far) */
            const int threads = peek64(screen) == 0x84c04a18ULL, options = options_screen(screen);   /* ((Triforce) and the Options' list) */
            const float view_lo = threads ? -760.0f : options ? OPT_ROWS_BOTTOM : -1080.0f;
            const float view_hi = threads ? 598.0f : options ? OPT_ROWS_TOP : 1080.0f;
            int first = -1, last = -1;   /* (the rows shown: their first and last in the order) */
            for (int j = 0; j < n_rows; j++) {
                const float cy = (hit.rect[order[j]].y0 + hit.rect[order[j]].y1) / 2;
                if (cy >= view_lo && cy <= view_hi) last = j, first = first < 0 ? j : first;
            }
            int to = -1;
            if (first >= 0 && step > 0 && last + 1 < n_rows) to = last + want < n_rows ? last + want : n_rows - 1;
            else if (first >= 0 && step < 0 && first > 0) to = first + want >= 0 ? first + want : 0;
            else if (at_focus >= 0) {
                to = at_focus + want;
                if (to >= n_rows) to = n_rows - 1;
                if (to < 0) to = 0;
            }
            if (to >= 0 && to != at_focus) {
                const Rect *r = &hit.rect[order[to]];
                const float cy = (r->y0 + r->y1) / 2;
                set_focus(c, screen, hit.button[order[to]]);
                if (trace) hos_log("menu: wheel %d: the focus from row %d to %d of %d (y %.0f)%s", wheel, at_focus, to, n_rows, cy,
                                   cy < view_lo || cy > view_hi ? ", out of view: the list moves" : "");
                if (cy < view_lo || cy > view_hi)   /* (then the row under the mouse, once the list has brought it into view) */
                    refocus_ns = now + 1200000000, scroll_row = hit.button[order[to]], scroll_lo = view_lo, scroll_hi = view_hi, asked = 0;
                else asked = hit.button[order[to]];   /* (the hover leaves it be until the mouse moves on) */
            }
            hit.hit = 0;
            list = 0;   /* (no pad presses) */
        }
        for (int i = 0; list && i < k && i < 6; i++)
            n += snprintf(taps + n, sizeof taps - (size_t)n, "%s%.2f:%s/60", n ? "," : "", at + i * 0.12,
                          pause ? (wheel > 0 ? "L" : "R") : (wheel > 0 ? "UP" : "DOWN"));
        if (n) hos_hid_tap(taps);
    }
    /* (the list has moved under the mouse: the row the wheel gave the focus is in view, as the list scrolls at its own
       pace; at most 1.2 s on. Taken back sooner, the scroll stopped short of it) */
    if (refocus_ns && scroll_row) {
        Rect r;
        const float cy = button_rect(scroll_row, &r) ? (r.y0 + r.y1) / 2 : 0;
        if (cy >= scroll_lo && cy <= scroll_hi) refocus_ns = now;
    }
    if (refocus_ns && now >= refocus_ns) refocus_ns = 0, scroll_row = 0, moved = 1, asked = 0;
    /* ((Triforce) the Options screen's header is our tabs' (gpu/vk/vk_menu.cpp draw_game_tab: the renderer reads the same
       clicks through hos_mouse_pointer, display.c): neither pointed at nor clicked here) */
    if (options_screen(screen) && options_tabs_on() && hit.y >= OPT_HEADER_Y) moved = 0, pressed &= ~(uint32_t)HOS_MOUSE_LEFT;
    if (moved || (pressed & HOS_MOUSE_LEFT)) hit_test(screen, &hit);
    else hit.hit = 0;
    /* ((Triforce, 11-19) the Mods entry under the mouse as the game's own entries are, the owner: "make it the same as the other
       options": the mouse moved onto it takes the cursor (the renderer's word 1, as the pad's Down, with its sound); on the
       System page Quit under it as well (4: Quit has the cursor), as the game's buttons above take it on hover. Off them
       again the mouse is the renderer's (the menu is open while they have the cursor): vk_menu.cpp gives the cursor back
       when it moves up onto the game's list */
    if (moved && !hit.hit && !rx_option(HOS_OPT_MODS_SELECTED, 0)) {
        const int onto = mods_entry_hit(hit.x, hit.y) ? 1 : quit_entry_hit(hit.x, hit.y) ? 4 : 0;
        if (onto) rx_option_set(HOS_OPT_MODS_ENTER, onto);
        if (onto && trace) hos_log("menu: mouse (%.0f, %.0f): onto the %s entry", hit.x, hit.y, onto == 1 ? "Mods" : "Quit");
    }
    /* hover: the focus to the button under the mouse, only as it moves (the pad keeps the focus it moved) */
    int just = 0;
    uint8_t state = 0;
    if (hit.hit) peek(hit.hit + 56, &state, 1);   /* (0: not focused) */
    if (hit.hit && hit.hit != focused_button && hit.hit != asked && !state) {
        asked = hit.hit, just = 1;
        const int r = set_focus(c, screen, hit.hit);
        if (trace) {
            char name[64];
            hos_log("menu: mouse (%.0f, %.0f): the focus to %s (%d) on screen %#llx", hit.x, hit.y,
                    text_at(peek64(hit.hit + 0x18), name, sizeof name), r, (unsigned long long)screen);
        }
    }
    /* a click on the tabs: L or R pressed as many times as the pages between (one at a time, as the pages slide) */
    if (pause && (pressed & HOS_MOUSE_LEFT) && !hit.hit) {
        const int tab = tab_at(hit.x, hit.y);
        if (tab == TAB_L || tab == TAB_R) hos_hid_tap(tab == TAB_L ? "0:L/120" : "0:R/120");
        else if (tab >= 0 && page_now >= 0 && tab != page_now) tab_goal = tab, tab_pages = 4, goal_ns = now, tab_from = -9;
        if (trace && tab != -1) hos_log("menu: mouse (%.0f, %.0f): tab %d from page %d", hit.x, hit.y, tab, page_now);
    }
    /* A tab clicked (the pause menu's, the panel's): L or R one at a time toward it, the next once the page has come and
       settled (0.25 s: a press while a page slides in is lost); given up after 4 s */
    {
        static int last_cur = -9;
        static int64_t arrive_ns;
        const int cur = pause ? page_now : page;
        if (cur != last_cur) last_cur = cur, arrive_ns = now;
        if (tab_goal >= 0 && (cur < 0 || now - goal_ns > 4000000000LL)) tab_goal = -1;
        if (tab_goal >= 0) {
            if (cur == tab_goal) tab_goal = -1;
            else if (cur != tab_from && now - arrive_ns > 250000000) {
                const int d = tab_goal - cur;
                if (tab_pages == 4) hos_hid_tap(d < 0 ? "0:L/120" : "0:R/120");
                else hos_hid_tap(d < 0 ? "0:L/75" : "0:R/75");   /* (the panel: 75 ms, not into the next page) */
                if (trace) hos_log("menu: tab: page %d toward %d: %s", cur, tab_goal, d < 0 ? "L" : "R");
                tab_from = cur;
            }
        }
    }
    /* Key Items: the stones and the medallions (not the framework's buttons). The item chosen is the page's own key of
       it, a string at *(screen+760) (Item_Medal_Fire, Item_Stone_Water, ...; an empty slot has its key too). The page
       moves its choice with the left stick (A and D on the keyboard) on a 3 x 3 grid:
           Item_Medal_Shadow   Item_Medal_Light   Item_Medal_Forest
           Item_Stone_Forest   Item_Stone_Fire    Item_Stone_Water
           Item_Medal_Spirit   Item_Medal_Water   Item_Medal_Fire
       and a press off the grid's edge leaves the page (to the pause menu). With the medallions (an adult's page) the
       items sit in a ring round the stones; without (a child's) only the stones are shown, further apart. The key is
       the page's whole choice: each frame its update places the cursor from it, and its stick only works out the next
       key and copies it in. So the mouse moved onto an item sets that item's key and calls the page's own refresh
       (set_item_key: as the page does when it opens), silently; a click on an item is Details (A) for it, once it is
       the one chosen (given up after 2 s). Positions: the item's centre in the layout's units, from the pages as
       shown (1280 x 720 screenshots) */
    if (keys) {
        static const struct { const char *key; int row, col; float x, y, cx; } items[] = {
            {"Item_Medal_Shadow", 0, 0, -750, 294, 0}, {"Item_Medal_Light", 0, 1, 0, 660, 0}, {"Item_Medal_Forest", 0, 2, 744, 294, 0},
            {"Item_Stone_Forest", 1, 0, -429, -24, -640}, {"Item_Stone_Fire", 1, 1, 0, -15, 0}, {"Item_Stone_Water", 1, 2, 429, -6, 650},
            {"Item_Medal_Spirit", 2, 0, -756, -354, 0}, {"Item_Medal_Water", 2, 1, 0, -714, 0}, {"Item_Medal_Fire", 2, 2, 759, -354, 0}};
        enum { N_ITEMS = (int)(sizeof items / sizeof *items) };
        static int adult;   /* (the ring is shown; worked out again when the page has been away) */
        static int64_t keys_ns;
        static float px_[N_ITEMS], py_[N_ITEMS];
        static int found;   /* (the items' panes the game found) */
        if (now - keys_ns > 1000000000) {
            /* The page shown anew: the layout from the items' panes, found as the page finds them to place its glow;
               the Emerald's x tells them apart (a child's wider row: about -680, the ring's -430). The panes' positions
               are their parent's, about 200 below the items as drawn: the hit boxes stay those measured above */
            adult = 0, found = 0;
            struct { int *found; float *x, *y; } at_ = {&found, px_, py_};
            item_panes(c, screen, item_found, &at_);
            if (found & (1 << 3)) adult = px_[3] > -540.0f;
            if (trace) {
                char line[600];
                int n = snprintf(line, sizeof line, "menu: Key Items' panes (%s):", adult ? "the ring" : "the stones' row");
                for (int i = 0; i < N_ITEMS && n < (int)sizeof line - 40; i++)
                    n += found & (1 << i) ? snprintf(line + n, sizeof line - (size_t)n, " %s %.0f,%.0f", items[i].key + 5, px_[i], py_[i])
                                          : snprintf(line + n, sizeof line - (size_t)n, " %s -", items[i].key + 5);
                hos_log("%s", line);
            }
        }
        keys_ns = now;
        static int64_t click_ns;
        static int click_item = -1;
        char key[40] = "";
        int at = -1, want = -1;
        {
            const uint64_t p = peek64(screen + 760);
            if (p && peek(p, key, sizeof key - 1)) key[sizeof key - 1] = 0;
            for (int i = 0; i < N_ITEMS; i++)
                if (!strcmp(key, items[i].key)) at = i;
            if (at >= 0 && items[at].row != 1) adult = 1;
        }
        float best = 0;
        for (int i = 0; i < N_ITEMS; i++) {
            if (!adult && items[i].row != 1) continue;   /* (a child's page: the stones alone) */
            const float ix = adult || !items[i].cx ? items[i].x : items[i].cx, iy = items[i].y, r = adult ? 190.0f : 240.0f;
            const float dx = hit.x - ix, dy = hit.y - iy, d = dx * dx + dy * dy;
            if (d < r * r && (want < 0 || d < best)) want = i, best = d;
        }
        if ((pressed & HOS_MOUSE_LEFT) && want >= 0) {
            click_item = want, click_ns = now;
            pressed &= ~(uint32_t)HOS_MOUSE_LEFT;   /* (a click on an item is this, not the tabs' or the prompts') */
        }
        if (click_item >= 0 && now - click_ns > 2000000000LL) click_item = -1;
        if (click_item >= 0 && at == click_item) {
            hos_hid_tap("0:A/75");
            if (trace) hos_log("menu: Key Items: %s clicked: A", key);
            click_item = -1;
        }
        /* (the mouse moved onto an item, or clicked one: the stick may move the choice away while the mouse rests) */
        const int to = click_item >= 0 ? click_item : moved && !ldown ? want : -1;
        if (to >= 0 && at >= 0 && to != at) {
            const int r = set_item_key(c, screen, items[to].key);
            if (trace) hos_log("menu: Key Items: %s chosen, the mouse over %s: set %s", key, items[to].key, r ? "" : "(not now)");
        }
    }
    /* The map's page (no buttons of the screen's list for these; in the layout's units, from its screens as shown,
       measured): the prompts at the bottom right ("World Map" / "Current Location" with Y, "Back" with B) and the tab
       bar of its panel (Key Items, Threads of Time, Map; L and R): the map's screen has the mouse while the Map page
       is shown, so a tab is that many pages from it */
    if (page >= 0 && (pressed & HOS_MOUSE_LEFT) && !hit.hit) {
        char tap[256] = "";
        if (hit.y > -1080 && hit.y < -960) {   /* (the Y prompt: World Map / Current Location, See Tapestry) */
            if (page >= 1 && hit.x > 1040 && hit.x < 1570) strcpy(tap, "0:Y/75");
        } else if (map && hit.x > 1400 && hit.x < 1880 && hit.y > -960 && hit.y < -560) {
            strcpy(tap, "0:Y/75");   /* (an area's map, not a dungeon's: its World Map picture and prompt above Back) */
        } else if (map && hit.x > -1850 && hit.x < -1200 && hit.y < 700 && hit.y > -1000) {
            /* A dungeon's floors (not the framework's buttons): the rows 142 apart, the list centred at y -119 (from the
               screens as shown: Lord Jabu-Jabu's 3 floors' top row at y 23; the Water Temple's 4 sit half a row higher):
               the floor of the row clicked, and the pad's up or down as many times as it is from the floor chosen
               (map_floors). A row past the last floor is the last */
            int cur, top, bottom;
            if (map_floors(&cur, &top, &bottom)) {
                const float top_y = -119.0f + (float)(top - bottom) * 71.0f;
                int row = (int)((top_y - hit.y) / 142.0f + 0.5f);
                if (row < 0) row = 0;
                int want = top - row;
                if (want < bottom) want = bottom;
                const int steps = want - cur;
                for (int i = 0, n = 0; i < (steps < 0 ? -steps : steps) && n < (int)sizeof tap - 16; i++)
                    n += snprintf(tap + n, sizeof tap - (size_t)n, "%s%.2f:%s/60", n ? "," : "", i * 0.15, steps > 0 ? "UP" : "DOWN");
                if (trace) hos_log("menu: map floors %d..%d, floor %d; row %d clicked: floor %d", bottom, top, cur, row, want);
            }
        } else if (hit.y > 900 && hit.y < 1080) {   /* (the tabs: Key Items, Threads of Time, Map; L and R) */
            if (hit.x >= -1100 && hit.x < -850) strcpy(tap, "0:L/75");
            else if (hit.x >= 850 && hit.x < 1100) strcpy(tap, "0:R/75");
            else {   /* (L to the left, R to the right: stepped toward it, above) */
                const int to = hit.x >= -850 && hit.x < -300 ? 0 : hit.x >= -300 && hit.x < 345 ? 1 : hit.x >= 345 && hit.x < 850 ? 2 : page;
                if (to != page) tab_goal = to, tab_pages = 3, goal_ns = now, tab_from = -9;
            }
        }
        if (tap[0]) hos_hid_tap(tap);
        if (trace) hos_log("menu: panel page %d click (%.0f, %.0f): %s", page, hit.x, hit.y, tap[0] ? tap : "-");
    }
    /* ((Triforce) a click on the Mods entry under a list's last button: the renderer opens Mods; the game is not given it) */
    if ((pressed & HOS_MOUSE_LEFT) && !hit.hit && mods_entry_hit(hit.x, hit.y)) {
        rx_option_set(HOS_OPT_MODS_ENTER, 2);
        pressed &= ~(uint32_t)HOS_MOUSE_LEFT;
        if (trace) hos_log("menu: the Mods entry clicked (%.0f, %.0f)", hit.x, hit.y);
    }
    /* ((Triforce, 11-19) a click on Quit under Mods: the renderer opens its dialog (3) */
    if ((pressed & HOS_MOUSE_LEFT) && !hit.hit && quit_entry_hit(hit.x, hit.y)) {
        rx_option_set(HOS_OPT_MODS_ENTER, 3);
        pressed &= ~(uint32_t)HOS_MOUSE_LEFT;
        if (trace) hos_log("menu: the Quit entry clicked (%.0f, %.0f)", hit.x, hit.y);
    }
    /* The "Back" prompt at the bottom right (B), on every screen with one (the title's load list, the pause menu, the
       map; in the layout's units, measured from their screens): a click there off the buttons */
    if ((pressed & HOS_MOUSE_LEFT) && !hit.hit && hit.y > -1080 && hit.y < -960 && hit.x >= 1600 && hit.x < 1900) {
        hos_hid_tap("0:B/80");
        if (trace) hos_log("menu: the Back prompt clicked (%.0f, %.0f)", hit.x, hit.y);
    }
    /* a click: the button under it decided (A: after the focus has landed, at the next update, when it moved it), the
       right button back (B) */
    /* ((Triforce) on the Options screen the press is by the row's kind and where on it: options_click) */
    if ((pressed & HOS_MOUSE_LEFT) && hit.hit) {
        const int kind = options_screen(screen) ? options_row_kind(c, screen, hit.hit) : OPT_KIND_NONE;
        const char *press = options_click(kind, hit.x);
        char tap[32];
        snprintf(tap, sizeof tap, "%s:%s/80", just || hit.hit == asked ? "0.05" : "0", press);
        hos_hid_tap(tap);
        if (trace && options_screen(screen))
            hos_log("menu: Options: a click (%.0f, %.0f) on a row of kind %d (0 toggle, 1 choice, 2 plate, -1 none found): %s",
                    hit.x, hit.y, kind, tap);
    }
    if (pressed & HOS_MOUSE_RIGHT) hos_hid_tap("0:B/80");
}

/* The button's boxes (button+0x58, 0xb0 apart). The first is its hit box: its global matrix at +0x28 (3 x 4, by column:
   the translation at +0x4c, +0x50; the layout's units, moving with a page as it slides; all 0 while its page is not shown)
   and its rectangle at +0x84 (x, y, w, h, about the box's origin). The second is centred on the slot's picture (its
   translation at +0xfc, +0x100; its own rectangle a fixed 700 x 700). A few slots' hit boxes cover only a side of the
   picture (Pa_ItemIcon_05: 100 wide on the right of a slot 260 wide, its centre 80 to the right of the picture's): such
   a box is mirrored about the picture's centre and both taken */
static int button_rect(uint64_t button, Rect *r) {
    float m[12], b[4], c[2];
    const uint64_t box = peek64(button + 0x58);
    if (!peek(box + 0x28, m, sizeof m) || !peek(box + 0x84, b, sizeof b)) return 0;
    const float sx = m[0], sy = m[4], tx = m[9], ty = m[10];
    if (!(sx > 1e-3f) || !(sy > 1e-3f) || !(b[2] > 0) || !(b[3] > 0)) return 0;
    r->x0 = tx + sx * b[0], r->x1 = tx + sx * (b[0] + b[2]);
    r->y0 = ty + sy * b[1], r->y1 = ty + sy * (b[1] + b[3]);
    float s2;
    if (peek(box + 0xd8, &s2, 4) && s2 > 1e-3f && peek(box + 0xfc, c, sizeof c)) {
        const float w = r->x1 - r->x0, h = r->y1 - r->y0, cx = (r->x0 + r->x1) / 2, cy = (r->y0 + r->y1) / 2;
        if (c[0] - cx < w && cx - c[0] < w && c[1] - cy < h && cy - c[1] < h) {   /* (the picture's centre near the box) */
            const float x0 = 2 * c[0] - r->x1, x1 = 2 * c[0] - r->x0, y0 = 2 * c[1] - r->y1, y1 = 2 * c[1] - r->y0;
            if (x0 < r->x0) r->x0 = x0;
            if (x1 > r->x1) r->x1 = x1;
            if (y0 < r->y0) r->y0 = y0;
            if (y1 > r->y1) r->y1 = y1;
        }
    }
    return 1;
}

/* ---- (Triforce) The Mods entry under a game list's last button: the pause menu's System page (Save, Load, Options,
   Special Actions, Title Screen: its buttons left of the Basic Controls panel, x under 0) and the title's list (by its
   screen's vtable). Each frame such a list runs the renderer is told its last button's box and whether the game's cursor
   is on it (HOS_OPT_MODS_*, gpu/vk/vk_api.h); nn_hid.c then keeps Down from the game (hos_oot_mods_down) and the renderer
   takes the cursor onto Mods. A click on the entry opens it (mouse_tick_locked) */
#define TITLE_LIST_VTABLE 0x84c106e8ULL
static volatile LONG64 mods_last_ns;   /* (the last frame a list's last button had the game's cursor) */
static volatile LONG64 mods_last_present;   /* ((Triforce, 11-19) ...and the game's presents then: hos_oot_mods_down) */
extern volatile LONG64 hos_present_count;   /* (display.c: the game's own presents) */
static Rect mods_entry;                 /* (the entry's box, as the renderer draws it: under the last button by the pitch) */
static Rect quit_entry;                 /* ((Triforce, 11-19) Quit's, under Mods by the pitch; empty on the title's list */
static volatile LONG64 mods_entry_ns;
typedef struct { Rect r[16]; uint64_t b[16]; int n, pause; } ModsList;
static void mods_list_entry(int i, uint64_t entry, uint64_t button, void *arg) {
    (void)i, (void)entry;
    ModsList *l = arg;
    Rect r;
    if (l->n >= 16 || !button_rect(button, &r)) return;
    if (l->pause) {   /* (the System page's buttons, Pa_Button_*, in its left column; the other pages' slots are in the list too, off the screen) */
        char name[32];
        const float cx = (r.x0 + r.x1) / 2;
        if (cx > 0 || cx < -1920 || strncmp(text_at(peek64(button + 0x18), name, sizeof name), "Pa_Button_", 10)) return;
    }
    l->r[l->n] = r, l->b[l->n++] = button;
}
int rx_option(int id, int otherwise);   /* rx_backend.c */
/* While Mods has the cursor the game's last button is let go as the game itself lets a button go when its cursor moves on
   (11-17, a workflow over the lift, its findings checked by skeptics; then the lab): the focus controller (the manager
   *(*(screen+32)+96), its controllers at +40 and +48: +120 the current entry, +128 a move asked, not made) calls the old
   button's "cursor left" (vtable+336, f_80afe540: an OFF event in the button's own queue at +58..+62, and the button linked
   into its screen's update list, which plays its focus-out: +56 3 -> 2 -> 0) and the new one's "cursor entered" (vtable+328,
   f_80aa9150: an ON event). The controller is left alone: its +120 stays on the last button, so the game's cursor is where it
   was when Mods closes, and "cursor entered" then lights it again (the pause menu's own focus handler, f_802ed660, replays the
   System option's look and help text with it). Raw writes of the state bytes do nothing: the events drive it.
   Decided under seen_lock (mods_list_tick), done after it (hos_oot_menu_tick): ModsCall */
static struct { uint64_t screen, button; int64_t since_ns; } mods_held;   /* (seen_lock) */
typedef struct { int op; uint64_t screen, button; int title; } ModsCall;   /* (op 1 let go, 2 give back) */
static int mods_button_vt(uint64_t vt) {   /* (the framework's button classes: setState f_80ad5310 at +248 in each) */
    static const uint64_t v[] = {0x84c41280ULL, 0x84c416e8ULL, 0x84c42808ULL, 0x84c42990ULL, 0x84c42b00ULL,
                                 0x84c42c90ULL, 0x84c42e08ULL, 0x84c42fb8ULL, 0x84c431c8ULL, 0x84c43338ULL};
    for (int i = 0; i < 10; i++)
        if (vt == v[i]) return 1;
    return 0;
}
/* the button the game's cursor is on for this screen (0: none); *pending: a move asked of the controller and not made yet */
static uint64_t mods_cursor_button(uint64_t screen, int *pending) {
    const uint64_t mgr = peek64(peek64(screen + 32) + 96);
    *pending = 0;
    for (uint64_t k = 0; mgr && k < 2; k++) {
        const uint64_t ctrl = peek64(mgr + 40 + 8 * k), e = ctrl ? peek64(ctrl + 120) : 0;
        if (!e || peek64(e + 32) != screen) continue;
        *pending = peek64(ctrl + 128) != 0;
        return peek64(e + 24);
    }
    return 0;
}
static void mods_call(Ctx *c, const ModsCall *m) {
    const uint64_t b = m->button;
    if (!m->op || !b || !mods_button_vt(peek64(b))) return;
    uint8_t st = 0;
    uint32_t fl = 0;
    peek(b + 56, &st, 1), peek(b + 64, &fl, 4);
    int called = 0;
    if (m->op == 1 && (st == 1 || st == 3) && (fl & 0x2)) {
        guest_call(c, peek64(peek64(b) + 336), b, 0, NULL, NULL), called = 1;
        /* (the System page: its option's own "chosen" look rewound, as the game does to every option not focused, f_833ee800) */
        if (!m->title && peek64(m->screen) == PAUSE_SCREEN_VTABLE) {
            const uint64_t sys = peek64(m->screen + 800);
            for (uint64_t k = 56; sys && k <= 88; k += 8) {
                const uint64_t w = peek64(sys + k);
                if (w && peek64(w + 96) == b && peek64(w + 72)) { guest_call(c, 0x833d90f0ULL, w, 0, NULL, NULL); break; }
            }
        }
    } else if (m->op == 2 && (st == 0 || st == 2) && (fl & 0x11) == 0x11) {
        guest_call(c, peek64(peek64(b) + 328), b, 0, NULL, NULL), called = 1;
    }
    if (trace) {
        uint8_t st2 = 0;
        peek(b + 56, &st2, 1);
        hos_log("menu: mods entry: the game's last button %s (state %u -> %u, flags %#x)%s", m->op == 1 ? "let go" : "given back",
                st, st2, fl, called ? "" : ": not called");
    }
}
static void mods_list_tick(uint64_t screen, int title, ModsCall *call) {
    ModsList l = {0};
    l.pause = !title;
    each_entry(screen, 400, mods_list_entry, &l);
    if (l.n < 2) return;
    int last = 0, above = -1;
    for (int i = 1; i < l.n; i++)
        if ((l.r[i].y0 + l.r[i].y1) < (l.r[last].y0 + l.r[last].y1)) last = i;
    for (int i = 0; i < l.n; i++)
        if (i != last && (above < 0 || (l.r[i].y0 + l.r[i].y1) < (l.r[above].y0 + l.r[above].y1))) above = i;
    const Rect r = l.r[last];
    const float pitch = (l.r[above].y0 + l.r[above].y1) / 2 - (r.y0 + r.y1) / 2;
    if (!(pitch > 20 && pitch < 600)) return;
    /* The button's state byte (+56) is its focus animation's: 0 none, 1 coming, 3 focused, 2 going. While Mods has the
       cursor the game gets no input (nn_hid.c) and its last button is let go (mods_held, above) */
    uint8_t state = 0;
    peek(l.b[last] + 56, &state, 1);
    const int64_t now = hos_now_ns();
    {
        const int sel = rx_option(HOS_OPT_MODS_SELECTED, 0) != 0;
        int pending = 0;
        /* ((Triforce, 11-19) the button the game's cursor is on, whichever: the last one when the pad's Down came from it, but
           the mouse may come onto Mods (or click it) from any; that one is let go and given back */
        const uint64_t cur = mods_cursor_button(screen, &pending);
        uint8_t cur_state = 0;
        if (cur) peek(cur + 56, &cur_state, 1);
        if (mods_held.button && mods_held.screen != screen) memset(&mods_held, 0, sizeof mods_held);
        if (sel && !mods_held.button && cur && (cur_state == 1 || cur_state == 3) && !pending) {
            *call = (ModsCall){1, screen, cur, title};
            mods_held.screen = screen, mods_held.button = cur, mods_held.since_ns = 0;
        } else if (sel && mods_held.button) {
            mods_held.since_ns = 0;
            if ((cur_state == 1 || cur_state == 3) && cur == mods_held.button) *call = (ModsCall){1, screen, cur, title};   /* (lit again: again) */
        } else if (!sel && mods_held.button) {
            if (!mods_held.since_ns) mods_held.since_ns = now;
            if (cur == mods_held.button && !pending) *call = (ModsCall){2, screen, cur, title}, memset(&mods_held, 0, sizeof mods_held);
            else if (cur || now - mods_held.since_ns > 1000000000LL) memset(&mods_held, 0, sizeof mods_held);   /* (moved on) */
        }
    }
    {   /* ((Triforce) 11-17, his picture: Continue opened the Load list over the title, whose list still runs, and "Mods" stayed
           on it) The entry only while the game's cursor is on this list (its focus controller's entry is this screen's), or
           Mods has the cursor */
        int pend = 0;
        if (!mods_cursor_button(screen, &pend) && !mods_held.button) return;
    }
    /* (coming or there; 2 is its focus going to another button. Let go for Mods, it is still the game's cursor's place) */
    const int focused = state == 1 || state == 3 || (mods_held.button && mods_held.button == l.b[last]);
    if (focused) {
        InterlockedExchange64(&mods_last_present, InterlockedCompareExchange64(&hos_present_count, 0, 0));
        InterlockedExchange64(&mods_last_ns, (LONG64)now);
    }
    mods_entry = (Rect){r.x0, r.y0 - pitch, r.x1, r.y1 - pitch};
    if (title) {   /* ((Triforce, 11-19) the title's: its button a 60 x 60 mark, the line's middle: as list_boxes makes the game's
                      boxes there (1100 wide, meeting the row above halfway), so the word itself is the entry's */
        const float mx = (r.x0 + r.x1) / 2, my = (r.y0 + r.y1) / 2 - pitch;
        mods_entry = (Rect){mx - 550, my - pitch / 2, mx + 550, my + pitch / 2};
    }
    quit_entry = title ? (Rect){0, 0, 0, 0} : (Rect){r.x0, r.y0 - 2 * pitch, r.x1, r.y1 - 2 * pitch};
    InterlockedExchange64(&mods_entry_ns, (LONG64)now);
    rx_option_set(HOS_OPT_MODS_X0, (int)r.x0), rx_option_set(HOS_OPT_MODS_X1, (int)r.x1);
    rx_option_set(HOS_OPT_MODS_Y0, (int)r.y0), rx_option_set(HOS_OPT_MODS_Y1, (int)r.y1);
    rx_option_set(HOS_OPT_MODS_PITCH, (int)pitch);
    rx_option_set(HOS_OPT_MODS_LIST, HOS_MODS_LIST_SHOWN | (focused ? HOS_MODS_LIST_LAST_FOCUSED : 0) | (title ? HOS_MODS_LIST_TITLE : 0));
    static unsigned logged;
    if (trace && logged++ % 120 == 0)
        hos_log("menu: mods entry: %s list of %d, last button (%.0f, %.0f)-(%.0f, %.0f), pitch %.0f, focused %d", title ? "the title's" : "System's",
                l.n, r.x0, r.y0, r.x1, r.y1, pitch, focused);
}
/* nn_hid.c: the game's cursor is on a list's last button now (the frame before at most): its Down is the Mods entry's.
   ((Triforce, 11-19) the lab's "one DOWN from Options on the title did nothing and logged nothing": with the game's window
   behind another one its frames came 80 to 235 ms apart, and 80 ms after the list's last frame the Down went to the game,
   which has nothing under its last button. The list's frame is the game's last one (or there has been none since) however
   long ago that was; within 80 ms as before) */
int hos_oot_mods_down(void) {
    const LONG64 t = InterlockedCompareExchange64(&mods_last_ns, 0, 0);
    if (!t) return 0;
    if ((LONG64)hos_now_ns() - t < 80000000LL) return 1;
    const LONG64 presents = InterlockedCompareExchange64(&hos_present_count, 0, 0) - InterlockedCompareExchange64(&mods_last_present, 0, 0);
    return presents >= 0 && presents <= 1;
}
static int quit_entry_hit(float x, float y) {
    const LONG64 t = InterlockedCompareExchange64(&mods_entry_ns, 0, 0);
    return t && (LONG64)hos_now_ns() - t < 1000000000LL && quit_entry.x1 > quit_entry.x0 && x >= quit_entry.x0 && x <= quit_entry.x1 &&
           y >= quit_entry.y0 && y <= quit_entry.y1;
}
static int mods_entry_hit(float x, float y) {
    const LONG64 t = InterlockedCompareExchange64(&mods_entry_ns, 0, 0);
    /* ((Triforce, 11-19) a second, not 100 ms: a click in a slow frame (a hitch at 20 fps) missed the entries; the renderer still
       takes it only while the game's list is fresh) */
    return t && (LONG64)hos_now_ns() - t < 1000000000LL && x >= mods_entry.x0 && x <= mods_entry.x1 && y >= mods_entry.y0 && y <= mods_entry.y1;
}

/* ---- The screens f_802f1380 runs for (the HUD's, the menus', the title's: each frame each). The mouse is for the one
   in front: of those that ran in the last 100 ms with a focused button shown (or one gone for under 0.5 s: the map's
   list loses it for 6 to 24 frames as it moves), the one that came last (a dialog over the menu that opened it). Whether a screen has a focused button is looked at 10 times a second (each look reads its list).
   (Triforce) The game's Options screen has the mouse with no focused button found as well: our tabs over its header take
   the mouse (gpu/vk/vk_menu.cpp), and a screen the mouse is not freed for leaves it the camera's, captured and hidden */
typedef struct { uint64_t screen, vt; int64_t first_ns, seen_ns, lost_ns; int focus, running, options; unsigned n; } Seen;
static Seen seen[24];
static SRWLOCK seen_lock = SRWLOCK_INIT;   /* (the screens' updates run on several of the game's threads at once) */

static void focus_entry(int i, uint64_t entry, uint64_t button, void *arg) {
    (void)i, (void)entry;
    uint8_t state = 0;
    Rect r;
    /* ((Triforce) a button let go while the Mods entry has the cursor still counts: the menu is there, mods_held) */
    if (!*(int *)arg && peek(button + 56, &state, 1) && (state || button == mods_held.button) && button_rect(button, &r)) *(int *)arg = 1;
}
static Seen *screen_seen(uint64_t screen, int64_t now) {
    Seen *free_one = NULL;
    for (int i = 0; i < (int)(sizeof seen / sizeof *seen); i++) {
        /* ((Triforce) a screen's memory is another screen's later (the Load list's was the Options' in a traced run): another
           vtable there is a new screen, which came last. Kept as the one before, it came first, under the screens it is over) */
        if (seen[i].screen == screen && seen[i].vt == peek64(screen)) return &seen[i];
        if (seen[i].screen == screen) { free_one = &seen[i]; break; }
        if (!free_one || seen[i].seen_ns < free_one->seen_ns) free_one = &seen[i];   /* (the one gone longest) */
    }
    memset(free_one, 0, sizeof *free_one);
    free_one->screen = screen, free_one->first_ns = now, free_one->vt = peek64(screen);
    if (free_one->vt == HUD_SCREEN_VTABLE) hud_screen = screen;
    if (trace) {
        const uint64_t mgr = peek64(peek64(screen + 32) + 96);
        hos_log("menu: a screen: %#llx (vtable %#llx) entries %d, focus manager %#llx%s", (unsigned long long)screen,
                (unsigned long long)peek64(screen), each_entry(screen, 400, NULL, NULL), (unsigned long long)mgr,
                screen == pause_screen_now() ? " (the pause menu)" : "");
    }
    return free_one;
}

/* ((Triforce, 11-19) benchmark mode, host/hos_bench.c: the start's screens pressed through as each gets a focused button. The
   file select is left on the file it opens on, the newest (its entries are a carousel's places, not files: focusing one by
   its place loaded another file in a lab run) */
typedef struct { int index, state; } BenchLook;
static void bench_entry(int i, uint64_t entry, uint64_t button, void *arg) {
    (void)entry;
    BenchLook *l = (BenchLook *)arg;
    uint8_t state = 0;
    if (peek(button + 56, &state, 1) && state > l->state) l->index = i, l->state = state;
}
static void bench_menu(Ctx *c, uint64_t screen, uint64_t vt) {
    (void)c;
    BenchLook l = {-1, 0};
    const int n = each_entry(screen, 64, bench_entry, &l);
    int entry = -1;
    if (hos_bench_menu(screen, vt, 1, l.index, n, &entry) == HOS_BENCH_MENU_PRESS_A) hos_hid_tap("0:A/120");
}

void hos_oot_menu_tick(Ctx *c, uint64_t screen) {
    if (!screen) return;
    if (trace < 0) trace = getenv("HOS_MENU_TRACE") != NULL;
    const int64_t now = hos_now_ns();
    AcquireSRWLockExclusive(&seen_lock);
    Seen *me = screen_seen(screen, now);
    if (!me->running) {
        me->running = 1;
        if (trace) hos_log("menu: screen %#llx (vtable %#llx) runs", (unsigned long long)screen, (unsigned long long)peek64(screen));
    }
    me->seen_ns = now;
    /* (Triforce: the game's Options screen runs: the renderer's tabs over its header. Its vtable as it is now, not me->vt:
       a screen's memory is another screen's later, the Load list ran at the Options' address in the traced run) */
    me->options = options_screen(screen);
    /* ((Triforce, 11-20) where this run of the Options screen was chosen from: the pause menu if its screen ran just before
       (kept for the run; a run begins after 200 ms with no Options frame, as hos_oot_options_front's) */
    static int64_t pause_ran_ns;   /* (under seen_lock) */
    static int options_from_pause;
    if (peek64(screen) == PAUSE_SCREEN_VTABLE) pause_ran_ns = now;
    if (me->options) {
        const LONG64 was = InterlockedCompareExchange64(&options_seen_ns, 0, 0);
        if (!was || now - was >= 200000000LL) options_from_pause = pause_ran_ns && now - pause_ran_ns < OPTIONS_FROM_PAUSE_NS;
        InterlockedExchange64(&options_seen_ns, (LONG64)now);
        if (options_tabs_on()) rx_option_set(HOS_OPT_GAME_OPTIONS, options_from_pause ? 2 : 1);
    }
    /* ((Triforce) the Mods entry: the pause menu's System page, the title's list; its call into the game after the lock) */
    ModsCall mods_now = {0};
    if (peek64(screen) == TITLE_LIST_VTABLE) mods_list_tick(screen, 1, &mods_now);
    else if (screen == pause_screen_now() && page_now == 3) mods_list_tick(screen, 0, &mods_now);
    for (int i = 0; i < (int)(sizeof seen / sizeof *seen); i++)   /* (a screen with no update for 200 ms has stopped) */
        if (seen[i].running && now - seen[i].seen_ns > 200000000) {
            seen[i].running = 0;
            if (trace) hos_log("menu: screen %#llx stopped", (unsigned long long)seen[i].screen);
        }
    if (me->n++ % 6 == 0) {
        const int had = me->focus;
        me->focus = 0;
        if (peek64(peek64(screen + 32) + 96)) each_entry(screen, 400, focus_entry, &me->focus);
        if (had && !me->focus) me->lost_ns = now;
        if (trace && me->focus != had)
            hos_log("menu: screen %#llx %s a focused button (%u frames in)", (unsigned long long)screen,
                    me->focus ? "has" : "no longer has", me->n);
    }
    {   /* (the Map page: the panel's frame running alone) */
        int frame = 0, other = 0;
        for (int i = 0; i < (int)(sizeof seen / sizeof *seen); i++) {
            if (!seen[i].screen || !seen[i].running) continue;
            if (seen[i].vt == PANEL_FRAME_VTABLE) frame = 1;
            if (seen[i].vt == 0x84c07798ULL || seen[i].vt == 0x84c04a18ULL || seen[i].screen == pause_screen_now()) other = 1;
        }
        const int was = map_page;
        map_page = frame && !other;
        if (trace && map_page != was) hos_log("menu: the panel's Map page %s", map_page ? "shown" : "gone");
    }
    const Seen *front = NULL;
    for (int i = 0; i < (int)(sizeof seen / sizeof *seen); i++)
        if (seen[i].screen && (seen[i].focus || now - seen[i].lost_ns < 500000000 || panel_page_vt(seen[i].vt) >= 0 || seen[i].options) &&
            seen[i].running && now - seen[i].seen_ns < 100000000 &&
            (!front || seen[i].first_ns > front->first_ns))   /* (its focus gone for under 0.5 s: a list's moment between rows) */
            front = &seen[i];
    static uint64_t front_was;
    if (trace && front && front->screen != front_was) hos_log("menu: the mouse is for screen %#llx", (unsigned long long)front->screen);
    front_was = front ? front->screen : 0;
    const int mine = front == me;
    /* (no screen has held the mouse for 50 ms of frames, the HUD's among them in play: the game's menus have closed,
       and the window gives the camera its mouse back now, not after display.c's 300 ms) */
    static int64_t none_since;
    const int gone = !front && none_since && now - none_since > 50000000;
    none_since = front ? 0 : none_since ? none_since : now;
    const int focus_now = me->focus;
    const uint64_t vt_now = me->vt;
    ReleaseSRWLockExclusive(&seen_lock);
    if (focus_now && hos_bench_on()) bench_menu(c, screen, vt_now);   /* ((Triforce, 11-19) benchmark mode: the start's screens) */
    mods_call(c, &mods_now);
    if (gone) hos_mouse_game_menu_gone();
    if (mine) mouse_tick(c, screen);
    if (screen == pause_screen_now()) InterlockedIncrement(&ticks);
}
#else
/* Not Windows: the mouse in the game's menus is the Windows window's (display.c), not here. The game's own Options
   screen is named all the same, each frame it runs, so the options' tabs sit in it as on Windows: they are the only way
   to our options since the keys that opened them over the game went (gpu/vk/vk_menu.cpp overlay_allowed), and with this
   left out a Linux build had none at all (2026-11-14). The screen is the one whose update the lifted game is running
   (nx2/oot_patches.py patch_menu_tick), so its first word, the vtable, is there to read */
#include <stdlib.h>
#include <string.h>
#include "../gpu/vk/vk_api.h"   /* (HOS_OPT_GAME_OPTIONS) */
static int64_t options_seen_ns;   /* (the screens' updates run on several of the game's threads: read and written whole) */
void hos_oot_menu_focus(Ctx *c, uint64_t screen, uint64_t pane, uint64_t lr) { (void)c, (void)screen, (void)pane, (void)lr; }
void hos_oot_menu_tick(Ctx *c, uint64_t screen) {
    (void)c;
    uint64_t vt = 0;
    if (screen < 0x100000 || screen >= HOS_GUEST_SPACE - 8) return;
    memcpy(&vt, GP(screen), 8);
    static int64_t pause_ran_ns;   /* ((Triforce, 11-20) the pause menu's last update: where the Options were chosen from) */
    static int options_from_pause;
    const int64_t now = hos_now_ns();
    if (vt == PAUSE_SCREEN_VTABLE) __atomic_store_n(&pause_ran_ns, now, __ATOMIC_RELAXED);
    if (vt != OPTIONS_SCREEN_VTABLE) return;
    static int tabs = -1;
    if (tabs < 0) {
        const char *e = getenv("HOS_OPTIONS_TABS");
        tabs = !(e && *e == '0');
    }
    const int64_t was = __atomic_load_n(&options_seen_ns, __ATOMIC_RELAXED), paused = __atomic_load_n(&pause_ran_ns, __ATOMIC_RELAXED);
    if (!was || now - was >= 200000000LL)   /* (a new run of the screen: from the pause menu if it ran just before) */
        __atomic_store_n(&options_from_pause, paused && now - paused < OPTIONS_FROM_PAUSE_NS, __ATOMIC_RELAXED);
    __atomic_store_n(&options_seen_ns, now, __ATOMIC_RELAXED);
    if (tabs) rx_option_set(HOS_OPT_GAME_OPTIONS, __atomic_load_n(&options_from_pause, __ATOMIC_RELAXED) ? 2 : 1);
}
int hos_oot_options_front(void) {
    const int64_t t = __atomic_load_n(&options_seen_ns, __ATOMIC_RELAXED);
    return t && hos_now_ns() - t < 200000000LL;
}
/* ((Triforce, 11-18) the Mods entry under the game's lists is the Windows half's (mods_list_tick): here no list's last button
   has the cursor, so nn_hid.c never keeps a Down from the game. Without it every Linux build of v0.5.0 stopped at the link,
   "undefined reference to hos_oot_mods_down" (#bug-tracker, 11-18). tests/test_oot_menu_linux_half.py) */
int hos_oot_mods_down(void) { return 0; }
#endif
