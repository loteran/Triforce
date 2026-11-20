/* (Triforce) The sprint button's rules, after NX2navi's runtime/sprint.c (public domain additions over an MIT base):
   Link sprints while his input flags hold the dash input (bit 4 of the word at *(Link +0x50)); the game itself sets it
   only out of a roll. With a sprint button chosen, the lifted code asks hos_oot_sprint_link / hos_oot_sprint_stance
   right after it loads those flags (nx2/oot_patches.py patch_sprint), and they put the bit in while the button's sprint
   is on: the game's own sprint then runs, with its speed, animation, camera and every rule of its own that ends it.

   The button: the left stick's click or B, the bottom face button of the Nintendo layout.
   Toggle: a press starts the sprint, the next press or Link's state ends it. Hold: sprint while it is held. During
   gameplay the button is kept from the game, so it does not also do its own action; B only for a press that begins
   while Link moves, and then until it is let go, so B puts the weapon away as usual otherwise and the game never sees
   half a press. ((Triforce, 11-18) The stick's click only for a press that begins while Link is in a state a sprint can
   be in (OOT_SPRINT_STOP is not): the game does use it, to hide the text box in a conversation, where Link's update goes
   on, and with the click kept from it there that did nothing (#bug-tracker 11-18).) A press made in a menu, a cutscene or a load (no update of Link within OOT_SPRINT_POLLS polls of the
   controller) starts nothing. With the game's Swap Jump Controls B is the jump: the stick's click is the one to use.

   Here: the logic alone, for tests/test_sprint.py. host/oot_sprint.c reads Link and the settings. */
#ifndef OOT_SPRINT_H
#define OOT_SPRINT_H
#include <stdint.h>

enum { OOT_SPRINT_OFF = 0, OOT_SPRINT_LS = 1, OOT_SPRINT_B = 2 };          /* the Sprint button row's values */
enum { OOT_SPRINT_MOVE, OOT_SPRINT_KEEP, OOT_SPRINT_WAIT, OOT_SPRINT_STOP };   /* what Link's state does to a sprint */
#define OOT_SPRINT_DASH 0x10u       /* the dash input in Link's flags */
#define OOT_SPRINT_BIT_B (1u << 1)  /* HidNpadButton: B, the left stick's click, ZL (the game's targeting) */
#define OOT_SPRINT_BIT_LS (1u << 4)
#define OOT_SPRINT_BIT_ZL (1u << 8)
#define OOT_SPRINT_POLLS 8          /* the button is kept from the game while Link's update ran within this many polls */
/* Standing still this long ends a toggled sprint that has moved, or this long after a press made while standing, which
   has not moved yet. In 60 Hz frames, not updates: above 60 fps Link is updated once a rendered frame, and four updates
   are 17 ms at 240 fps: any short stop would end the sprint */
#define OOT_SPRINT_WAIT_STOP 4.0
#define OOT_SPRINT_WAIT_START 90.0

/* Link's state id (host/oot_sprint.c reads it) -> its kind. A state not listed stops the sprint. */
static inline int oot_sprint_kind(int32_t id) {
    switch (id) {
    case 1:     /* Walk (running too) */
    case 13:    /* Jump */
    case 32:    /* Land */
    case 33:    /* Fall */
    case 68:    /* Clamber */
        return OOT_SPRINT_MOVE;
    case 253:   /* StepOn */
    case 35:    /* Slide */
    case 36:    /* SlideLand */
        return OOT_SPRINT_KEEP;
    case 40:    /* Wait */
        return OOT_SPRINT_WAIT;
    default:
        return OOT_SPRINT_STOP;
    }
}

static inline uint64_t oot_sprint_bit(int button) {
    return button == OOT_SPRINT_LS ? OOT_SPRINT_BIT_LS : button == OOT_SPRINT_B ? OOT_SPRINT_BIT_B : 0;
}

/* The controller's polls come from the game's input reads, Link's updates from one of its workers: the fields both
   touch are read and written whole (__atomic), each side's own are plain */
typedef struct {
    uint64_t pressed;   /* the button's presses since Link's last update (pad -> step) */
    int held;           /* the button held at the last poll */
    int zl_held;        /* ZL held at the last poll */
    int polls;          /* polls since Link's last update */
    int moving;         /* Link's state was a MOVE one at his last update */
    int active;         /* the sprint is on: the dash input goes in */
    int latched;        /* the press now held began in gameplay (B: while Link moved): kept from the game */
    uint64_t last;      /* pad only: the buttons at the last poll */
    int moved;          /* step only: the sprint has moved since it started */
    double wait_frames; /* step only: 60 Hz frames stood still */
    int last_button;    /* step only: the setting at the last update */
    int sprintable;     /* Link's state at his last update was one a sprint can be in (not OOT_SPRINT_STOP) */
} OotSprint;
#define OOT_SPRINT_INIT {0, 0, 0, OOT_SPRINT_POLLS, 0, 0, 0, 0, 0, 0.0, -1, 0}

#define OOT_SPRINT_GET(f) __atomic_load_n(&(f), __ATOMIC_SEQ_CST)
#define OOT_SPRINT_SET(f, v) __atomic_store_n(&(f), (v), __ATOMIC_SEQ_CST)

static inline void oot_sprint_reset(OotSprint *s) {
    OOT_SPRINT_SET(s->active, 0), OOT_SPRINT_SET(s->pressed, (uint64_t)0), OOT_SPRINT_SET(s->latched, 0);
}

/* One poll of the controller, the buttons as the game is about to get them: the sprint button's bit is taken out of
   them while its press is the sprint's. */
static inline void oot_sprint_pad(OotSprint *s, int button, uint64_t *buttons) {
    if (button == OOT_SPRINT_OFF) {   /* nothing may stay on for when a button is chosen again */
        oot_sprint_reset(s);
        s->last = *buttons;
        return;
    }
    const uint64_t bit = oot_sprint_bit(button), now = *buttons;
    const int n = OOT_SPRINT_GET(s->polls), gameplay = n < OOT_SPRINT_POLLS;
    if (n <= OOT_SPRINT_POLLS) OOT_SPRINT_SET(s->polls, n + 1);
    /* A press is decided at its start, for its whole length: kept from the game, and the sprint's, when it begins in
       gameplay (B: while Link moves); else the game's alone, and no sprint's */
    if (now & ~s->last & bit) {
        const int mine = gameplay && (button == OOT_SPRINT_B ? OOT_SPRINT_GET(s->moving) : OOT_SPRINT_GET(s->sprintable));
        OOT_SPRINT_SET(s->latched, mine);
        if (mine) __atomic_fetch_or(&s->pressed, bit, __ATOMIC_SEQ_CST);
    } else if (!(now & bit)) {
        OOT_SPRINT_SET(s->latched, 0);
    }
    s->last = now;
    OOT_SPRINT_SET(s->held, (now & bit) != 0);
    OOT_SPRINT_SET(s->zl_held, (now & OOT_SPRINT_BIT_ZL) != 0);
    if (OOT_SPRINT_GET(s->latched) && gameplay) *buttons &= ~bit;
}

/* One update of Link: his state's kind, the settings, and the 60 Hz frames this update covers (1 at 60 fps, 0.5 at
   120). 1 when the sprint is on for it. */
static inline int oot_sprint_step(OotSprint *s, int kind, int button, int hold, double frames) {
    OOT_SPRINT_SET(s->polls, 0);
    if (button != s->last_button) s->last_button = button, OOT_SPRINT_SET(s->active, 0), OOT_SPRINT_SET(s->pressed, (uint64_t)0);
    const int press = (__atomic_exchange_n(&s->pressed, (uint64_t)0, __ATOMIC_SEQ_CST) & oot_sprint_bit(button)) != 0;
    int on = OOT_SPRINT_GET(s->active);
    if (OOT_SPRINT_GET(s->zl_held)) kind = OOT_SPRINT_STOP;   /* targeting: the game's own stance */
    OOT_SPRINT_SET(s->moving, kind == OOT_SPRINT_MOVE);
    OOT_SPRINT_SET(s->sprintable, kind != OOT_SPRINT_STOP);
    if (button == OOT_SPRINT_OFF) {
        on = 0;
    } else if (hold) {
        on = OOT_SPRINT_GET(s->held) && OOT_SPRINT_GET(s->latched) && kind != OOT_SPRINT_STOP;
    } else if (press) {
        if (on) on = 0;
        else if (kind != OOT_SPRINT_STOP && (button != OOT_SPRINT_B || kind == OOT_SPRINT_MOVE))
            on = 1, s->moved = kind == OOT_SPRINT_MOVE, s->wait_frames = 0;
    } else if (on) {
        if (kind == OOT_SPRINT_STOP) {
            on = 0;
        } else if (kind == OOT_SPRINT_WAIT) {
            s->wait_frames += frames;
            if (s->wait_frames > (s->moved ? OOT_SPRINT_WAIT_STOP : OOT_SPRINT_WAIT_START)) on = 0;
        } else {
            s->wait_frames = 0;
            s->moved |= kind == OOT_SPRINT_MOVE;
        }
    }
    OOT_SPRINT_SET(s->active, on);
    return on;
}

#endif
