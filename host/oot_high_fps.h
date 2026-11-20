/* (hookshot) Two places in the game that need whole 60 fps frames, under a Frame rate cap above 60, where the frame step is the
   frame's own time (host/nn_vi.c, hos_frame_seconds_bits: 0.5 at 120 fps). nx2/oot_patches.py hooks them; host/oot_high_fps.c
   holds the state. At a cap of 60 (no frame time given: timed 0) both return what the game computed, so nothing changes.

   The sound manager's job (f_80435300, its phase in w2): phase 0 hands the game step to the sound engine, whose fade and delay
   timers and parameter ramps count updates of at least one whole step. Fed less, some sounds are not released when an area
   unloads, the voices in use climb until the game's voice budget is full, and new sounds (sword, voices), then the area music,
   stop until a restart. Above 60 fps the job runs at 60 Hz: phase 0 adds the step to a remainder and runs once a whole step has
   built up, with a step of 1 (sound_step), keeping the rest; the job's other phases follow that frame's choice. Sound keeps the
   clock on average. A step of 1 or more runs as the game has it and keeps the remainder: around 60 fps (a cap above 60 with
   the game near 60, or VRR) steps of 0.98 and 1.02 alternate, and the 0.98 that did not run comes back as a whole step later
   (dropped, the sound clock ran at half the real one). A step of 0 or less (or NaN), and every phase at a cap of 60, run as the
   game has them, and clear the remainder.
   (After zortos293/Lichii PR #1289 by auz466, GPL-3.0: the same gate as a code patch for the emulator.)

   The physics world's solver steps: the game sets them each update to (int)(frame step * the steps of a 60 fps frame + 0.001)
   (f_838ceb80 and f_838cfc10, into the solver's +712; 8 a 60 fps frame in the places seen). A step under 1 cuts that short,
   and as the measured step wanders around its mean the count changes from frame to frame (8 x 0.49 is 3, 8 x 0.5 is 4 at 120
   fps); 4 steps a 60 fps frame would be 0 above 240. A moving platform (the stone one under the spikes in the Deku Tree)
   then slides out from under Link (issue 1798). While the step is under 1 the count is rounded up instead, so a solver step
   never covers more time than at 60 fps, give or take 10% (the frame time's jitter, so a steady rate keeps a steady count),
   and it is at least 1 and never under the game's. A step of 1 or more (a frame slower than 60 fps) keeps the game's count.

   Three more places, as NX2navi found and fixed them for this build (its recomp/patches-v111.txt, the sets ofollow, owater and
   orecord: public domain / MIT), here as hooks in the lifted C:

   Floors that carry a character: the character controller's three follow-surface updates (f_8015d490, f_80159dc0,
   f_838baa40) add the velocity of what the character stands on to its move, but keep only a tenth of it when the physics'
   inverse step (one over the frame's seconds times the time scale) is over 100.0. At the console's 60 and 30 fps that is a
   slow motion under 0.6 of the game's speed; with the frames' own time as the step it is every update above 100 fps, and
   Link slides off a moving platform that carries him at 60. The compare takes 100 / (the frame's seconds x 60) while a frame
   is shorter than a 60 fps one, so the cut comes where it came at 60 fps; 100 otherwise.

   The fixed steps of the physics' height-field surfaces: the two water managers (f_83895f20, f_838a2660) and the interactive
   floors (f_80652cb0: the Deku Tree's ground webs) simulate in steps of 1/60 s. Each update divides its seconds by 1/60 and cuts
   the quotient to whole steps, dropping the fraction: every update shorter than 1/60 s ran none, so above 60 fps those surfaces
   stood still whatever pushed into them. The quotient becomes the whole steps in it plus a remainder kept for the object
   (oot_sim_steps), as a float the game's own cut and clamp then take: 60 steps a second at any rate.

   The ocarina recorder's clock: the ocarina manager adds (unsigned)frame step to the length of the current note or rest
   (in 60 fps frames) at four places (f_832b2690, f_832b32b0, twice in f_832b6840). Cut to 0 above 60 fps, the clock stood
   still: a recorded song lost its rests and the clock's limits never came. One remainder for the four (oot_record_frames):
   the clock counts 60 a second at any rate. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "frame_whole.h"

typedef struct {
    float part;   /* (the steps added up since the job last ran, under 1) */
    int run;      /* (phase 0's choice, for the other phases of the same frame) */
} OotSoundGate;
#define OOT_SOUND_GATE_INIT {0.0f, 1}   /* (every phase runs until a phase 0 has chosen) */

static inline float oot_bits_float(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
static inline uint32_t oot_float_bits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    return bits;
}

/* (whether the step is a part of a frame: the only case the gate and the feed change) */
static inline int oot_sound_partial(float step, int timed) {
    return timed && step > 0.0f && step < 1.0f;
}

/* 1: run the job now, 0: return from it at once */
static inline int oot_sound_gate(OotSoundGate *g, uint32_t phase, float step, int timed) {
    if (!timed) {   /* (a cap of 60: every phase of every frame, whatever came before) */
        g->part = 0.0f;
        g->run = 1;
        return 1;
    }
    if (phase != 0) return g->run;
    if (!(step > 0.0f)) {   /* (0, below 0, NaN: as the game has it) */
        g->part = 0.0f;
        g->run = 1;
        return 1;
    }
    if (step >= 1.0f) {   /* (a whole step or more: runs with it, the remainder kept for the next partial ones) */
        g->run = 1;
        return 1;
    }
    g->part += step;
    g->run = g->part >= 1.0f;
    if (g->run) g->part -= 1.0f;
    return g->run;
}

/* The step phase 0 hands to the sound engine: a whole one while the gate holds the job to whole steps */
static inline uint32_t oot_sound_step(uint32_t step_bits, int timed) {
    return oot_sound_partial(oot_bits_float(step_bits), timed) ? 0x3f800000u : step_bits;
}

/* The solver's steps: game (the game's own count), scaled_bits (frame step * steps a 60 fps frame + 0.001, the float it cut),
   per_frame_bits (the steps of a 60 fps frame, a float) */
static inline int32_t oot_solver_steps(int32_t game, uint32_t scaled_bits, uint32_t per_frame_bits, int timed) {
    const float scaled = oot_bits_float(scaled_bits) - 0.001f, per_frame = oot_bits_float(per_frame_bits);
    if (!timed || !(scaled > 0.0f) || !(per_frame > 0.0f) || !(scaled < per_frame) || per_frame > 1000.0f) return game;
    int32_t steps = (int32_t)ceilf(scaled - 0.1f);
    if (steps < 1) steps = 1;
    return steps > game ? steps : game;
}

/* The number the follow-surface updates compare the physics' inverse step with (a float's bits): seconds_bits, the frame's
   seconds as the step hook has them (host/nn_vi.c hos_frame_seconds_bits; 0: the game's own step) */
static inline uint32_t oot_floor_cut(uint32_t seconds_bits) {
    const float frames = oot_bits_float(seconds_bits) * 60.0f;   /* (the frame in 60 fps frames) */
    if (!seconds_bits || !(frames > 0.0f) || !(frames < 1.0f)) return 0x42c80000u;   /* (100.0, the game's) */
    return oot_float_bits(100.0f / frames);
}

/* A height-field surface's fixed steps for this update: quotient_bits, its seconds over 1/60 s (a float's bits). With the frames'
   own time as the step the whole steps in it plus the object's remainder, as a float; else, and for a quotient that is no
   count (0, below 0, NaN, huge), the quotient as it is, for the game's own cut. The caller holds the table's lock. */
static inline uint32_t oot_sim_steps(FrameWhole *w, uint64_t object, uint32_t quotient_bits, int timed) {
    const float q = oot_bits_float(quotient_bits);
    if (!timed || !(q > 0.0f) || q >= 2147483648.0f) return quotient_bits;
    return oot_float_bits((float)frame_whole(w, object, quotient_bits, 1));
}

/* The frames the ocarina recorder's clock takes for a frame step (a float's bits): as the game cuts it (fcvtzu: toward zero,
   below 0 and NaN 0), with the fraction carried in *rem while the frames' own time is the step */
static inline uint32_t oot_record_frames(float *rem, uint32_t step_bits, int timed) {
    const float step = oot_bits_float(step_bits);
    if (!timed || !(step > 0.0f) || step >= 2147483648.0f) {
        *rem = 0.0f;
        return !(step > 0.0f) ? 0 : step >= 4294967296.0f ? 0xffffffffu : (uint32_t)step;
    }
    const float sum = *rem + step;
    const uint32_t whole = (uint32_t)sum;
    *rem = sum - (float)whole;
    return whole;
}
