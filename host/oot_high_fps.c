/* (hookshot) The sound manager's job at 60 Hz, the physics solver's steps rounded up, the floors' carry, the height-field
   surfaces' fixed steps and the ocarina recorder's clock, under a Frame rate cap above 60: the lifted code calls these
   (nx2/oot_patches.py); host/oot_high_fps.h has why and the logic. HOS_FPS_LOG (set): what they do, every 600 calls. */
#include "hos.h"
#include "oot_high_fps.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

extern volatile uint32_t hos_frame_seconds_bits;   /* (host/nn_vi.c: the frame's seconds, 0 at a cap of 60) */

static int timed(void) { return __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED) != 0; }
static int fps_log(void) {
    static int on = -1;   /* (read by the game's threads) */
    int v = __atomic_load_n(&on, __ATOMIC_RELAXED);
    if (v < 0) v = getenv("HOS_FPS_LOG") != NULL, __atomic_store_n(&on, v, __ATOMIC_RELAXED);
    return v;
}

static OotSoundGate sound_gate = OOT_SOUND_GATE_INIT;
static long long sound_calls, sound_runs;   /* (HOS_FPS_LOG's counts, under sound_lock) */
static SRWLOCK sound_lock = SRWLOCK_INIT;   /* (the job's phases may come from different threads) */

/* f_80435300's head: phase w2, slot the guest address of the pointer to the object whose +0x10 is the game step (the job's own
   load). 0: the job returns at once */
int hos_oot_sound_gate(Ctx *c, uint32_t phase, uint64_t slot) {
    (void)c;
    float step = 1.0f;
    const int t = timed();
    if (t && phase == 0) {
        const uint64_t object = G64(slot);
        const uint64_t time = object ? G64(object) : 0;
        if (time) step = oot_bits_float(*(const uint32_t *)GP(time + 0x10));
    }
    long long ran = -1;
    AcquireSRWLockExclusive(&sound_lock);
    const int run = oot_sound_gate(&sound_gate, phase, step, t);
    if (phase == 0 && fps_log()) {
        sound_runs += run;
        if (!(++sound_calls % 600)) ran = sound_runs, sound_runs = 0;
    }
    ReleaseSRWLockExclusive(&sound_lock);
    if (ran >= 0)
        hos_log("oot: sound job ran %lld of the last 600 frames (step %.3f%s)", ran, step,
                oot_sound_partial(step, t) ? ", held to 60 Hz" : "");
    return run;
}

/* The step f_80435300's phase 0 hands to the sound engine (its float's bits) */
uint32_t hos_oot_sound_step(uint32_t step_bits) {
    return oot_sound_step(step_bits, timed());
}

/* The solver steps of f_838ceb80 / f_838cfc10: game, the game's count; scaled_bits, the float it cut; per_frame_bits, the steps
   of a 60 fps frame (a float) */
uint32_t hos_oot_solver_steps(uint32_t game, uint32_t scaled_bits, uint32_t per_frame_bits) {
    const int32_t steps = oot_solver_steps((int32_t)game, scaled_bits, per_frame_bits, timed());
    if (fps_log()) {
        static volatile LONG64 calls;
        const LONG64 n = InterlockedIncrement64(&calls);
        if (n % 600 == 1)
            hos_log("oot: physics solver steps %d (the game's %d: frame step %.3f x %.0f steps a 60 fps frame)", steps, (int32_t)game,
                    (oot_bits_float(scaled_bits) - 0.001f) / oot_bits_float(per_frame_bits), oot_bits_float(per_frame_bits));
    }
    return (uint32_t)steps;
}

/* The follow-surface updates' compare (f_8015d490, f_80159dc0, f_838baa40): what the physics' inverse step is compared with */
uint32_t hos_oot_floor_cut(void) {
    const uint32_t cut = oot_floor_cut(__atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED));
    if (fps_log()) {
        static volatile LONG64 calls;
        const LONG64 n = InterlockedIncrement64(&calls);
        if (n % 600 == 1)
            hos_log("oot: a floor's carry is cut to a tenth above an inverse step of %.1f (the game's 100)", oot_bits_float(cut));
    }
    return cut;
}

static FrameWhole sim_whole;   /* (a table of the surfaces' own: sharing hos_frame_whole's, they and the waters' ripples would
                                  take each other's slots in turn, each starting again without its remainder) */
static long long sim_calls, sim_given;   /* (HOS_FPS_LOG's counts, under sim_lock) */
static SRWLOCK sim_lock = SRWLOCK_INIT;   /* (the physics step's jobs) */

/* The fixed steps of f_83895f20, f_838a2660 and f_80652cb0: object, the manager or floor (its x19); quotient_bits, the update's
   seconds over 1/60 s. The float the game's cut then takes */
uint32_t hos_oot_sim_steps(uint64_t object, uint32_t quotient_bits) {
    const int t = timed();
    long long given = -1;
    AcquireSRWLockExclusive(&sim_lock);
    const uint32_t bits = oot_sim_steps(&sim_whole, object, quotient_bits, t);
    if (fps_log()) {
        const float steps = oot_bits_float(bits);
        if (steps > 0.0f && steps < 1000.0f) sim_given += (long long)steps;
        if (!(++sim_calls % 600)) given = sim_given, sim_given = 0;
    }
    ReleaseSRWLockExclusive(&sim_lock);
    if (given >= 0)
        hos_log("oot: the water and floor surfaces got %lld fixed steps in the last 600 updates (quotient %.3f%s)", given,
                oot_bits_float(quotient_bits), t ? ", fractions carried" : "");
    return bits;
}

static float record_rem;
static SRWLOCK record_lock = SRWLOCK_INIT;   /* (the ocarina manager's update alone runs the four places: a lock all the same) */

/* The ocarina recorder's clock (f_832b2690, f_832b32b0, f_832b6840): the frames it adds for the frame step */
uint32_t hos_oot_record_frames(uint32_t step_bits) {
    AcquireSRWLockExclusive(&record_lock);
    const uint32_t frames = oot_record_frames(&record_rem, step_bits, timed());
    ReleaseSRWLockExclusive(&record_lock);
    return frames;
}
