/* (Triforce, 11-16, the owner: "remove monitors shape as a option keep 16:9 16:10 21:9 32:9") The Ultrawide row's shapes, for
   host/hos_main.c (the picture's shape at the start) and gpu/vk/vk_menu.cpp (the row): 0 the game's own 16:9, 1 a fixed 16:10,
   2 a fixed 21:9 (64:27, the shape of a 2560 x 1080 or a 5120 x 2160 screen), 3 a fixed 32:9. A value past the row is its nearest
   end. Before options_version 6 the row's 1 was "Monitor's shape" (the main monitor's own ratio): such a file's 1 is read as the
   shape nearest the main monitor's, ultrawide_row_nearest, so its picture stays as it was on the common screens (16:9, 16:10,
   21:9 and 32:9 ones; a 4:3 one gets 16:10). Plain C, no includes: the host's C and the renderer's C++ both take it. */
#ifndef ULTRAWIDE_ROWS_H
#define ULTRAWIDE_ROWS_H

/* The picture's shape for the row, width over height; 0 for the game's own 16:9 */
static inline float ultrawide_row_ratio(int row) {
    if (row <= 0) return 0.0f;
    if (row == 1) return 1.6f;
    return row == 2 ? (float)(64.0 / 27.0) : (float)(32.0 / 9.0);
}

/* Whether the row's shape is wider than the game's 16:9: 21:9 and 32:9 (16:10 is narrower, 1.6). The HUD position and the
   menu sides act only on such a picture's sides (gpu/vk/vk_menu.cpp row_shown) */
static inline int ultrawide_row_wider(int row) { return ultrawide_row_ratio(row) > (float)(16.0 / 9.0); }

/* The row whose shape is nearest a screen's (by ratio: the larger over the smaller); 0 (16:9) while the screen is not known */
static inline int ultrawide_row_nearest(int screen_w, int screen_h) {
    if (screen_w <= 0 || screen_h <= 0) return 0;
    const double r = (double)screen_w / (double)screen_h;
    const double shapes[4] = {16.0 / 9.0, 1.6, 64.0 / 27.0, 32.0 / 9.0};
    int best = 0;
    double best_d = 0;
    for (int i = 0; i < 4; i++) {
        const double d = r > shapes[i] ? r / shapes[i] : shapes[i] / r;
        if (i == 0 || d < best_d) best = i, best_d = d;
    }
    return best;
}

/* The row as an options file holds it: before options_version 6 its 1 was the monitor's shape (see above) */
static inline int ultrawide_row_read(int row, int version, int screen_w, int screen_h) {
    return version < 6 && row == 1 ? ultrawide_row_nearest(screen_w, screen_h) : row;
}

#endif
