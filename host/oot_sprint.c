/* (Triforce) The sprint button: host/oot_sprint.h has the rules and why. Here: the settings (the Insert menu's Sprint
   button and Sprint mode rows, HOS_SPRINT=ls|b and HOS_SPRINT_HOLD=1 over them), Link's state read from the game's
   memory, and the two calls the lifted code makes (nx2/oot_patches.py patch_sprint). After NX2navi's runtime/sprint.c.

   Game 1.1.1's addresses (main at 0x80020000): Link's movement update f_804caa80 loads his input flags at 0x804ce798
   and tests the dash input twice from them (to start the sprint, to keep it); the update of his camera and stance
   f_80c225d0 loads their low byte at 0x80c27f3c. x19 is Link at both.
   Link's state: the id at [[[[Link +0x18] +0x270] +0x188] +0x1c0] +0x28 (+0x4c when that reads 0x11a, a state that
   wraps another), the field the game compares with 1 (Walk) before it starts a sprint itself.
   HOS_SPRINT_LOG (set): the first calls and each change, in the log. */
#include "hos.h"
#include "oot_sprint.h"
#include "../gpu/vk/vk_api.h"
#include <stdlib.h>
#include <string.h>

int rx_option(int id, int otherwise);
int hos_ini_option(const char *key, int otherwise);
extern volatile uint32_t hos_frame_seconds_bits;   /* (host/nn_vi.c: the frame's seconds, 0 at a cap of 60) */

#define LINK_CLASS 0x84b6dfc0ULL      /* what Link's first word reads */
#define ACTOR_MANAGER_AT 0x84c9e0d0ULL
#define ACTOR_MANAGER_CLASS 0x84bffe68ULL
#define CONTROLLED_CLASS 0x84b69698ULL

static OotSprint sprint = OOT_SPRINT_INIT;

static int log_on(void) {
    static int on = -1;
    int v = __atomic_load_n(&on, __ATOMIC_RELAXED);
    if (v < 0) v = getenv("HOS_SPRINT_LOG") != NULL, __atomic_store_n(&on, v, __ATOMIC_RELAXED);
    return v;
}

/* The button and the mode: the menu's rows once the renderer answers, the options file's before, the test switches
   over both */
static int setting_button(void) {
    static int env = -2, file = -1;
    if (env == -2) {
        const char *e = getenv("HOS_SPRINT");
        env = !e ? -1 : !strcmp(e, "ls") || !strcmp(e, "l3") ? OOT_SPRINT_LS : !strcmp(e, "b") ? OOT_SPRINT_B : OOT_SPRINT_OFF;
    }
    if (env >= 0) return env;
    if (file < 0) file = hos_ini_option("sprint_button", 0);
    const int v = rx_option(HOS_OPT_SPRINT, file);
    return v == OOT_SPRINT_LS || v == OOT_SPRINT_B ? v : OOT_SPRINT_OFF;
}
static int setting_hold(void) {
    static int env = -2, file = -1;
    if (env == -2) env = getenv("HOS_SPRINT_HOLD") ? atoi(getenv("HOS_SPRINT_HOLD")) != 0 : -1;
    if (env >= 0) return env;
    if (file < 0) file = hos_ini_option("sprint_mode", 0);
    return rx_option(HOS_OPT_SPRINT_HOLD, file) != 0;
}

/* Reads through Link's pointers: one that is not a pointer into the game's heap or its program image (stale, or a
   number) reads 0 instead of faulting. Both ranges are committed whole (host/hos_mem.c), so the check is two compares.
   (Not VirtualQuery: with a dozen calls of it in each update of Link the lab ran 2.8 fps, 350 ms a frame, where the
   same run without them made 48 to 66, 11-14. The game's memory is gigabytes of pages in few regions.) */
static int readable(uint64_t a, uint64_t n) {
    if (a >= HOS_HEAP_BASE && a - HOS_HEAP_BASE < hos_heap_size) return n <= hos_heap_size - (a - HOS_HEAP_BASE);
    if (a >= hos_image_base && a < hos_image_end) return n <= hos_image_end - a;
    return 0;
}
static uint64_t ld64(uint64_t a) { return readable(a, 8) ? *(const uint64_t *)GP(a) : 0; }
static int32_t ld32(uint64_t a) { return readable(a, 4) ? *(const int32_t *)GP(a) : 0; }

/* 1 when the object is the Link the player controls. His class has one more instance the player does not control: the
   game's actor manager names the controlled one at +0x470, whose +0x18 must be Link's own +0x18. */
static int is_link(uint64_t link) {
    if (!link || ld64(link) != LINK_CLASS) return 0;
    const uint64_t slot = ld64(ACTOR_MANAGER_AT), mgr = slot ? ld64(slot) : 0;
    if (!mgr || ld64(mgr) != ACTOR_MANAGER_CLASS) return 1;
    const uint64_t ctl = ld64(mgr + 0x470);
    if (!ctl || ld64(ctl) != CONTROLLED_CLASS) return 1;
    return ld64(ctl + 0x18) == ld64(link + 0x18);
}

static int32_t state_id(uint64_t link) {
    uint64_t p = ld64(link + 0x18);
    if (p) p = ld64(p + 0x270);
    if (p) p = ld64(p + 0x188);
    if (p) p = ld64(p + 0x1c0);
    if (!p) return -1;
    int32_t id = ld32(p + 0x28);
    if (id == 0x11a) id = ld32(p + 0x4c);
    return id;
}

/* The controller as the game is about to read it (host/nn_hid.c npad_states) */
void hos_oot_sprint_pad(uint64_t *buttons) {
    oot_sprint_pad(&sprint, setting_button(), buttons);
}

/* f_804caa80 at 0x804ce798, after the load of Link's input flags: the flags, with the dash input while the sprint is on */
uint64_t hos_oot_sprint_link(Ctx *c, uint64_t link, uint64_t flags) {
    (void)c;
    const int button = setting_button();
    if (button == OOT_SPRINT_OFF && !OOT_SPRINT_GET(sprint.active)) return flags;   /* (off: one load and out) */
    static int told_class;
    if (log_on() && !told_class) {
        told_class = 1;
        hos_log("sprint: the movement update's object %#llx reads %#llx (Link's class is %#llx), state %d",
                (unsigned long long)link, (unsigned long long)ld64(link), (unsigned long long)LINK_CLASS, (int)state_id(link));
    }
    if (!is_link(link)) return flags;
    const uint32_t bits = __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED);
    float seconds = 0.0f;
    memcpy(&seconds, &bits, 4);
    const double frames = bits && seconds > 0.0f && seconds < 1.0f ? (double)seconds * 60.0 : 1.0;
    const int32_t id = state_id(link);
    const int was = OOT_SPRINT_GET(sprint.active);
    const int on = oot_sprint_step(&sprint, oot_sprint_kind(id), button, setting_hold(), frames);
    if (log_on() && on != was) hos_log("sprint: %s (Link's state %d, the game's own dash input %d)", on ? "on" : "off", (int)id, (int)(flags >> 4 & 1));
    return on ? flags | OOT_SPRINT_DASH : flags;
}

/* f_80c225d0 at 0x80c27f3c, after the load of the same flags' low byte for the stance and the camera */
uint64_t hos_oot_sprint_stance(Ctx *c, uint64_t link, uint64_t flags) {
    (void)c;
    return OOT_SPRINT_GET(sprint.active) && is_link(link) ? flags | OOT_SPRINT_DASH : flags;
}
