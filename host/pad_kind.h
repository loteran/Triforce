/* ((Triforce) 11-20, ticket "Controls not working properly") Which of SDL's game controllers are left out of the game's input:
   instruments and dance mats, which SDL lists as game controllers with a mapping of their own. A "(Guitar Hero )" guitar beside a
   Switch Pro controller fed its whammy and tilt into player 1 as held triggers: Link was always targeting, his shield came up by
   itself, the camera would not move and the horse would not turn. HOS_SDL_ALL_PADS=1 keeps every controller, as before.
   type: SDL's joystick type (SDL2 SDL_JoystickType and SDL3 SDL_JoystickType share these numbers); name: the controller's name
   (either may be unknown: 0, NULL). Returns why it is left out, or NULL to use it. host/hos_sdl.c hos_sdl_pads_list;
   tests/test_pad_kind.py */
#pragma once

#include <ctype.h>
#include <stddef.h>

enum { PAD_KIND_DANCE_PAD = 5, PAD_KIND_GUITAR = 6, PAD_KIND_DRUM_KIT = 7 };

static inline int pad_kind_has_word(const char *name, const char *word) {
    if (!name) return 0;
    for (const char *p = name; *p; p++) {
        size_t i = 0;
        while (word[i] && p[i] && tolower((unsigned char)p[i]) == word[i]) i++;
        if (!word[i]) return 1;
    }
    return 0;
}

static inline const char *pad_kind_left_out(int type, const char *name) {
    if (type == PAD_KIND_GUITAR) return "a guitar";
    if (type == PAD_KIND_DRUM_KIT) return "a drum kit";
    if (type == PAD_KIND_DANCE_PAD) return "a dance mat";
    /* (by its name where SDL did not say: a guitar or drums seen as an Xbox 360 pad through a generic driver) */
    if (pad_kind_has_word(name, "guitar")) return "a guitar";
    if (pad_kind_has_word(name, "drum")) return "a drum kit";
    if (pad_kind_has_word(name, "rock band")) return "a Rock Band instrument";
    if (pad_kind_has_word(name, "dance pad") || pad_kind_has_word(name, "dance mat")) return "a dance mat";
    return NULL;
}
