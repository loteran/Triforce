/* (Triforce) The 2D interface on a picture wider than 16:9: host/oot_wide_hud.h has the rules and why. Here: the call the
   lifted code makes when a layout's full-screen projection is set (nx2/oot_wide.py HUD, the 2D canvas setup f_80794330
   at 0x80794368), and the Ultrawide HUD row's setting.

   The Insert menu's "Ultrawide HUD" row (ultrawide_hud= in the options file): 0 the middle, as before; 1 the screen's
   edges. HOS_WIDE_HUD=<0|1> over it. Read once, at the start: the hearts', the item slot's and the small map's panes
   are moved in the layouts as the game loads them (host/prompts.c build_layouts asks hos_wide_hud_out).
   Lab switches: HOS_WIDE_LOG=1 names each screen class the first time its projection is set (its address and where it
   was put); HOS_WIDE_SHIFT=<class in hex>=<l|r|m>,... moves whole layouts to a side, to find out what a class draws. */
#include "hos.h"
#include "oot_wide_hud.h"
#include <stdlib.h>
#include <string.h>

int hos_ini_option(const char *key, int otherwise);
extern volatile float hos_wide_squeeze;
extern volatile uint32_t hos_wide_keep_mask;

#define SEEN_MAX 96
static uint64_t seen[SEEN_MAX];
static int nseen;
static struct { uint64_t cls; int side; } forced[32];
static int nforced = -1;

static int edges_on(void) {
    static volatile int v = -1;
    if (v < 0) {
        const char *e = getenv("HOS_WIDE_HUD");
        v = e ? atoi(e) != 0 : hos_ini_option("ultrawide_hud", 0) != 0;
    }
    return v;
}

/* host/prompts.c build_layouts: how far out the side panes of the layouts go (oot_hud_out), 0 when they stay */
float hos_wide_hud_out(void) {
    return edges_on() ? oot_hud_out(hos_wide_squeeze) : 0.0f;
}

static void read_forced(void) {
    nforced = 0;
    const char *e = getenv("HOS_WIDE_SHIFT");
    while (e && *e && nforced < 32) {
        char *end;
        const uint64_t cls = strtoull(e, &end, 16);
        if (end == e || *end != '=') break;
        const char s = end[1];
        forced[nforced].cls = cls, forced[nforced].side = s == 'l' ? OOT_HUD_LEFT : s == 'r' ? OOT_HUD_RIGHT : OOT_HUD_MIDDLE;
        nforced++;
        e = strchr(end, ',');
        if (e) e++;
    }
}

static int side_of(uint64_t cls) {
    if (nforced < 0) read_forced();
    for (int i = 0; i < nforced; i++)
        if (forced[i].cls == cls) return forced[i].side;
    return OOT_HUD_MIDDLE;
}

/* A printable text at a, inside the program's image, for the log: "" when there is none */
static void text_at(uint64_t a, char *out, size_t cap) {
    out[0] = 0;
    if (a < hos_image_base || a + cap >= hos_image_end) return;
    const char *s = (const char *)GP(a);
    size_t n = 0;
    while (n + 1 < cap && s[n] >= 0x20 && s[n] < 0x7f) n++;
    if (n < 3 || s[n]) return;
    memcpy(out, s, n), out[n] = 0;
}

static void say_once(uint64_t cls, int keep, int side, const float *row, float move) {
    static int on = -1;
    if (on < 0) on = getenv("HOS_WIDE_LOG") != NULL;
    if (!on) return;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == cls) return;
    if (nseen >= SEEN_MAX) return;
    seen[nseen++] = cls;
    /* (what the class may name itself by: a text its table points at, at a few places the game's classes keep one) */
    char names[4][48];
    const int at[4] = {0x378, -8, 0x10, 0x18};
    for (int i = 0; i < 4; i++) {
        names[i][0] = 0;
        const uint64_t slot = cls + (uint64_t)(int64_t)at[i];
        if (slot < hos_image_base || slot + 8 >= hos_image_end) continue;
        uint64_t p = G64(slot);
        text_at(p, names[i], sizeof names[i]);
        if (!names[i][0] && p >= hos_image_base && p + 16 < hos_image_end) text_at(G64(p + 8), names[i], sizeof names[i]);
    }
    hos_log("wide hud: screen class %#llx: %s (m00 %.6f, m30 %.4f, m03 %.4f) names [%s] [%s] [%s] [%s]", (unsigned long long)cls,
            keep ? "kept whole" : side < 0 ? "left edge" : side > 0 ? "right edge" : "middle", (double)row[0], (double)row[3],
            (double)move, names[0], names[1], names[2], names[3]);
}

/* nx2/oot_wide.py HUD: screen is the canvas' screen object; row the first column of the layout's full-screen projection
   and move the first number of its fourth column (oot_wide_hud.h oot_hud_row) */
void hos_wide_hud_row(Ctx *c, uint64_t screen, float *row, float *move) {
    (void)c;
    const float q = hos_wide_squeeze;
    if (!(q > 0.0f)) return;
    const uint64_t cls = hos_guest_range(screen, 8) ? G64(screen) : 0;   /* (the first word of the screen object: its class) */
    const int keep = (oot_hud_keep_bit(cls) & hos_wide_keep_mask) != 0;
    const int side = keep ? OOT_HUD_MIDDLE : side_of(cls);
    say_once(cls, keep, side, row, *move);
    oot_hud_row(row, move, q, keep, side);
}
