/* (Triforce) Where the game's 2D interface goes on a picture wider than 16:9: the rules alone, for
   tests/test_wide_hud.py. host/oot_wide_hud.c reads the game's memory and the settings; nx2/oot_wide.py has the hook.

   The game lays its interface out on a 16:9 grid and draws each layout (a "screen": the hearts, the action buttons, the
   map, a menu, a fade) through one full-screen projection. It sets a layout's projection once, when the layout is made,
   not each frame; the hook hands it over then. Two things are done with it on a wider picture:
     - kept: as it is, the layout stretched over the whole picture (the item wheel's and the menus' backgrounds, which
       redraw a capture of the whole picture, and the fades, which are one colour);
     - middle: times the squeeze (16:9 over the picture's ratio), so the layout keeps its shape in the middle 16:9 of the
       picture. Everything else, as NX2navi's patch does it.
   (oot_hud_row can also move a whole layout out to one side, which the lab's HOS_WIDE_SHIFT uses to find out what a
   screen draws. No layout of the game's is placed that way: see below.)

   The Insert menu's "Ultrawide HUD" row on Screen edges puts the hearts and the item slot in the picture's top corners
   and the small map at its right edge. Those are moved inside their layouts, by oot_hud_out, because a projection
   cannot do it: PlayerStatus_00 holds the hearts (on a pane at the grid's top left corner) and the item slot (on one at
   its top right) together, and MapMenu_00 is the small map and the pause menu's whole-screen map in one layout, whose
   projection is not set again as the menu opens (tried: the menu's map sat off to the right). nx2/prompt_assets.py
   HUD_SIDE_PANES lists the panes, host/prompts.c build_layouts writes them as the game loads its layouts, so the row is
   read once, at the start. Layouts that follow something in the 3D world (the targeting pointer, an enemy's bar, a
   speech balloon) and the action buttons stay in the middle. */
#ifndef OOT_WIDE_HUD_H
#define OOT_WIDE_HUD_H
#include <stdint.h>

enum { OOT_HUD_MIDDLE = 0, OOT_HUD_LEFT = -1, OOT_HUD_RIGHT = 1 };

/* The screens that keep the whole picture, by their class (the first word of the screen object), a bit each: bit k of
   the keep mask keeps screen k whole (the Insert menu's Ultrawide menus row clears the menus' bit 1; the two fades
   are never squeezed: one colour in the middle alone leaves the picture showing at the sides). */
#define OOT_HUD_KEEP_N 4
static const uint64_t oot_hud_keep_classes[OOT_HUD_KEEP_N] = {
    0x84c0d098ULL,   /* the item wheel's background */
    0x84c04fb0ULL,   /* the pause and system menus' background */
    0x84c061c8ULL,   /* the event fade */
    0x84c07b30ULL,   /* the loading fade */
};

static inline uint32_t oot_hud_keep_bit(uint64_t screen_class) {
    for (int k = 0; k < OOT_HUD_KEEP_N; k++)
        if (oot_hud_keep_classes[k] == screen_class) return 1u << k;
    return 0;
}

#define OOT_HUD_HALF 1920.0f   /* (half the 16:9 grid's width, in the layouts' units) */

/* How far out, in the layouts' units, a pane at a side of the 16:9 grid goes to sit at that side of a picture whose
   squeeze is q: the same distance oot_hud_row moves a whole layout (1 - q of the clip space's half, q per unit). */
static inline float oot_hud_out(float q) {
    return q > 0.0f && q < 1.0f ? OOT_HUD_HALF * (1.0f / q - 1.0f) : 0.0f;
}

/* A layout's projection for a picture whose squeeze is q (0 < q <= 1). The game keeps the matrix by columns: `first` is
   its first column (m00 m10 m20 m30: what x is multiplied by) and `move` the first number of its fourth column (m03,
   what is added to clip x). keep: the layout keeps the whole picture. side: where it goes when it is not kept.
   (The fourth number of `first` is m30, not the move: a move put there bends the picture, and the layouts it was first
   tried on left the screen altogether.) */
static inline void oot_hud_row(float first[4], float *move, float q, int keep, int side) {
    if (keep || !(q > 0.0f) || q >= 1.0f) return;
    for (int k = 0; k < 4; k++) first[k] *= q;
    /* (clip space is 2 wide: the 16:9 middle leaves 1 - q free on each side) */
    *move = *move * q + (float)side * (1.0f - q);
}

#endif
