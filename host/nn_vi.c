/* nn::vi for MKW (TOTKPort's nn_vi.c is bound to its renderer and TOTK's frame pacing): the default display, one
   layer, and the display's vsync event, signaled at 60 Hz by a host thread. HDR and VRR are reported unsupported,
   so the game takes its SDR path. Frames are paced at the native NVN layer's window acquire (the end of this file). */
#include "hos.h"
#include "pace_wait.h"   /* ((Triforce) the acquire's wait to the frame's time: pace_sleep) */
#include "step_steady.h"   /* ((Triforce) HOS_STEP_STEADY=1: realtime_step) */
#include "pace_steady.h"   /* ((Triforce) the Steady frame rate row: steady_period) */
#include "../gpu/vk/vk_api.h"   /* (HOS_OPT_HDR_*: the Insert menu's HDR, answered by the renderer) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hos_system_event_init(uint64_t key, int autoclear);   /* nn_os_sync.c */
void hos_system_event_signal(uint64_t key);

static uint64_t display, layer;
static uint64_t vsync_events[8];
static volatile LONG nvsync;
static HANDLE vsync_thread;

static int fps_max(void);   /* (below: HOS_FPS_MAX) */
static DWORD WINAPI vsync_main(void *p) {
    (void)p;
    int64_t next = hos_now_ns() + 1000000000LL / fps_max();
    for (;;) {
        /* (asked each time: with the cap on Monitor's rate it is 60 until the renderer says the display's, fps_max) */
        const int64_t period = 1000000000LL / fps_max();
        const int64_t now = hos_now_ns();
        if (next > now) hos_sleep_ns(next - now);
        next += period;
        if (next < hos_now_ns()) next = hos_now_ns() + period;   /* (fell behind: no burst of vsyncs) */
        for (LONG i = 0; i < nvsync; i++) hos_system_event_signal(vsync_events[i]);
    }
    return 0;
}

/* nn::vi::Initialize() */
HLE(_ZN2nn2vi10InitializeEv) {}

/* nn::vi::OpenDefaultDisplay(nn::vi::Display**) -> Result */
HLE(_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE) {
    if (!display) display = hos_sys_alloc(0x100, 16);
    G64(ARG(0)) = display;
    RET(0);
}

/* nn::vi::CreateLayer(nn::vi::Layer**, nn::vi::Display*) -> Result */
HLE(_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE) {
    if (!layer) layer = hos_sys_alloc(0x100, 16);
    G64(ARG(0)) = layer;
    RET(0);
}

/* nn::vi::GetDisplayVsyncEvent(nn::os::SystemEventType*, nn::vi::Display*) -> Result */
HLE(_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE) {
    hos_system_event_init(ARG(0), 1);
    {   /* (hookshot) under a lock: two threads checked the count at once and wrote past the table */
        static SRWLOCK vsync_lock = SRWLOCK_INIT;
        AcquireSRWLockExclusive(&vsync_lock);
        if (nvsync < (LONG)(sizeof vsync_events / sizeof vsync_events[0])) {
            vsync_events[nvsync] = ARG(0);
            InterlockedIncrement(&nvsync);
        }
        ReleaseSRWLockExclusive(&vsync_lock);
    }
    if (!vsync_thread) vsync_thread = CreateThread(NULL, 0, vsync_main, NULL, 0, NULL);
    RET(0);
}

/* nn::vi::QueryDisplayHdrStatus(bool* on, HdrOutputFormat*, unsigned short* max nits, Display const*) -> Result */
/* (hookshot) the Insert menu's HDR set to Native on an HDR10 swapchain: an HDR10 display (format 1, which makes the game present PQ
   codes in 10 bits) of the menu's peak; otherwise SDR, as a desktop window is */
HLE(_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE) {
    const int native = rx_option(HOS_OPT_HDR_NATIVE, 0);
    *(uint8_t *)GP(ARG(0)) = (uint8_t)native;
    if (ARG(1)) G32(ARG(1)) = native ? 1 : 0;
    if (ARG(2)) *(uint16_t *)GP(ARG(2)) = native ? (uint16_t)rx_option(HOS_OPT_HDR_PEAK, 1000) : 0;
    static int logged = -1;
    if (logged != native) logged = native, hos_log("vi: QueryDisplayHdrStatus -> %s", native ? "HDR10 (the game's native HDR)" : "SDR");
    RET(0);
}
/* nn::vi::QueryDisplayHdrPaperWhite(unsigned short*, Display const*) -> Result */
HLE(_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE) {
    *(uint16_t *)GP(ARG(0)) = rx_option(HOS_OPT_HDR_NATIVE, 0) ? (uint16_t)rx_option(HOS_OPT_HDR_WHITE, 203) : 203;
    RET(0);
}
/* nn::vi::QueryDisplayVrrSupported(bool*, Display const*) -> Result */
HLE(_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE) {
    *(uint8_t *)GP(ARG(0)) = 0;
    RET(0);
}
HLE(_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb) { RET(0); }
HLE(_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii) { RET(0); }
HLE(_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE) { RET(0); }

/* nn::vi::GetNativeWindow(void**, nn::vi::Layer*) -> Result: a window object (NVN builds its window on it; the
   native NVN layer only keeps the pointer) */
HLE(_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE) {
    static uint64_t window;
    if (!window) window = hos_sys_alloc(0x100, 16);
    G64(ARG(0)) = window;
    hos_log("vi: GetNativeWindow -> %#llx", (unsigned long long)window);
    RET(0);
}

/* ---- frame pacing for the native NVN layer's window (nvn_native.c) ----
   The game acquires a window texture before each frame and presents it after; on the console the acquire blocks
   while every texture waits for the display. Here the acquire waits for the frame's display time: the present
   interval's vsyncs (1: 60 fps, 2: 30) after the last present, so the game runs at the display's rate.
   HOS_NO_PACING=1: as fast as it can. */
static volatile int present_interval = 1;
/* (hookshot) HOS_FPS_MAX=<61-240>: the frame rate cap past the console's 60 (opt-in, a test): the display pacing's interval, the game's
   own limiter (+0xfc) and the vsync event go up to it, and the game's time step is the frames' own time (realtime_step), so the game is
   not sped up. Unset or 60: 60, as on the console */
int hos_ini_option(const char *key, int otherwise);   /* (nn_applet.c) */
static volatile LONG64 presents;
/* The cap for the Insert menu's Frame rate cap (row: frame_rate_cap=, 0 the console's 60, then 90, 120, 144, 165, 240), HOS_FPS_MAX
   (env) over it. */
/* (Triforce, after NX2navi's limits under 60: -1 45, -2 40, -3 30, for a PC or a handheld's display that holds one
   of them steadily where 60 swings. The pacing follows the cap and the game's time step is real time (realtime_step),
   as when a PC cannot hold 60: the game keeps its speed) */
/* (Triforce, after NX2navi's fps_limit = display: 6, Monitor's rate, is the refresh rate of the display the window is on. display:
   what the renderer says of it (HOS_OPT_DISPLAY_CAP: rounded, 30 to 240), 0 when the rate cannot be read (60 then), under 0
   while there is no renderer to ask: the answer is then 0, not settled yet. NX2navi's "unlimited" is not offered: the time step
   here (realtime_step) stops at 1/240 s, and past it the game would run faster than the clock) */
static int fps_cap_of(int row, const char *env, int display) {
    static const int caps[] = {60, 90, 120, 144, 165, 240}, low[] = {45, 40, 30};
    int want = env ? atoi(env) : row >= 0 && row < 6 ? caps[row] : row < 0 && row >= -3 ? low[-row - 1] : 60;
    if (!env && row == 6) {
        if (display < 0) return 0;
        want = display < 1 ? 60 : display < 30 ? 30 : display > 240 ? 240 : display;
    }
    return want > 60 ? (want > 240 ? 240 : want) : want >= 20 && want < 60 ? want : 60;
}
static int fps_max(void) {
    static int v = -1;
    if (v < 0) {
        /* (the options file once: with Monitor's rate this is asked again until the renderer is up. Until then the cap is 60 and
           nothing is settled; the game presents nothing before the renderer is up (hos_renderer_wait), so the pacing, the game's
           limiter and its time step all start with the settled cap. A renderer that never answers (the WebGPU one) leaves 60
           at the first frame presented) */
        static int row = -100;
        if (row == -100) row = hos_ini_option("frame_rate_cap", 0);
        const char *e = getenv("HOS_FPS_MAX");
        const int monitor = !e && row == 6;
        int rx_option(int id, int otherwise);
        int display = monitor ? rx_option(HOS_OPT_DISPLAY_CAP, -1) : 0;
        if (display < 0 && presents) display = 0;
        const int cap = fps_cap_of(row, e, display);
        if (!cap) return 60;
        v = cap;
        if (monitor && display > 0) hos_log("vi: frame rate cap: the monitor's rate, %d", v);
        if (monitor && display <= 0) hos_log("vi: frame rate cap: the monitor's rate is not known: %d", v);
        if (v > 60) hos_log("vi: frame rate cap %d: pacing, the game's limiter and its time step follow it", v);
        if (v < 60) hos_log("vi: frame rate cap %d: pacing follows it, the game keeps real time", v);
    }
    return v;
}
static int64_t next_frame_at, window_start, frames_in_window;
int hos_vi_native_interval(Ctx *c, int requested) {
    (void)c;
    /* (hookshot) OoT's frame-rate controller asks for 2, 3 or 4 vsyncs a frame whenever one runs long (480 changes a minute
       in Kokiri): paced to them, frames snapped to 33, 50 and 67 ms, the stutter, and the frame rate sat far under what the
       PC managed. With game time following the clock (the Insert menu's Real time, realtime_step), a frame is shown as soon
       as it is ready, 60 at most; the game still reads back its own interval. HOS_GAME_INTERVAL=1, or Game speed Per frame:
       paced to the game's interval, as before */
    static int game_interval = -1;
    if (game_interval < 0) game_interval = getenv("HOS_GAME_INTERVAL") && atoi(getenv("HOS_GAME_INTERVAL")) != 0;
    int rx_option(int id, int otherwise);
    const int clamped = requested < 1 ? 1 : requested > 4 ? 4 : requested;
    /* (a cap under 60 paces by itself: with the game's 2 vsyncs on top a cap of 30 would show 15) */
    present_interval = (game_interval || !rx_option(0, 1)) && fps_max() >= 60 ? clamped : 1;
    return requested;
}
/* (hookshot) The window's textures as the console's display queue has them: each present is shown at its display
   time, the later of its present and one interval after the frame shown before it, and a texture is free again once
   the frame presented after it is shown. The acquire of a frame waits for its texture: with `active` textures, the
   texture of the frame `active` back, freed when the frame after that one is shown. With three (OoT) a frame slower
   than the display waits for nothing and a fast one is held to the display's rate, two frames queued at most;
   the acquire waited for one interval after the last present before (two textures' behaviour), which held every
   frame over 16.7 ms to the next vsync step (gameplay at 27 ms frames, 5 ms of them in this sleep). HOS_VI_DOUBLE=1:
   that behaviour */
static int64_t shown_at[16];
static volatile LONG64 shown_count;
/* (Triforce, after NX2navi's one limiter a frame: its runtime/frame_rate.c frame_step_cap_ns, vk_dlssg.c vk_dlssg_pace and
   rtss_limit.c) Another limiter may hold this frame in the acquire's wait's place: RivaTuner Statistics Server, which is
   the game's limiter wherever it is installed, runs and is seen holding the game (gpu/vk/vk_pace.cpp, rtss_limit.cpp), or
   NVIDIA Reflex's sleep, with DLSS frame generation and the Display tab's Reflex frame limit row (gpu/vk/vk_dlssg.cpp
   pace). The renderer is asked where the frame starts, with the period this file paces by and whether a movie plays, once
   a frame (the game may acquire again before it presents: the frame's answer stays). 1: that limiter holds this frame,
   RivaTuner's wait seen to be taken at the frame's presents and Reflex's taken inside the call, and the wait below steps
   aside for this frame alone. 0: the wait below, as before; that is the answer without a renderer to ask, without
   RivaTuner, and while a movie plays. One or the other for every frame, never both and never neither.
   HOS_PACE_HANDOFF=0: never asked (and RivaTuner then gets no limit from the game). */
static int64_t pace_period_ns;   /* (the period hos_vi_native_presented last paced by) */
/* ((Triforce) for the Steady frame rate row, steady_period below: how long the acquire's own wait took since the last
   present, on the presenting thread and on any other (OoT acquires on its main thread and presents on its Presentation
   Thread), and whether another limiter held a frame since then. The presenting thread is the one steady_period runs on) */
static int64_t frame_waited_own_ns;
static volatile LONG64 frame_waited_other_ns;
static volatile LONG frame_handed;
static _Thread_local int presenting_thread;
/* (Triforce) When a movie player last updated (hos_movie_frames, below, which the game's movie players call at each of
   their updates): within a quarter second of one, a movie plays. NX2navi's pacer holds a movie itself while RivaTuner keeps
   the play limit, and so does this one */
static volatile LONG64 movie_seen_ns;
static void movie_seen(void) { __atomic_store_n(&movie_seen_ns, (LONG64)hos_now_ns(), __ATOMIC_RELAXED); }
static int movie_plays(void) {
    const int64_t seen = (int64_t)__atomic_load_n(&movie_seen_ns, __ATOMIC_RELAXED);
    return seen && hos_now_ns() - seen < 250000000LL;
}
static int pace_handed(void) {
    static int on = -1, held;
    static LONG64 asked = -1;
    int rx_option(int id, int otherwise);
    void rx_option_set(int id, int value);
    if (on < 0) on = !(getenv("HOS_PACE_HANDOFF") && !atoi(getenv("HOS_PACE_HANDOFF")));
    if (!on) return 0;
    const LONG64 frame = presents;
    if (frame == asked) return held;
    asked = frame;
    rx_option_set(HOS_OPT_PACE_PERIOD, (int)pace_period_ns);
    rx_option_set(HOS_OPT_PACE_MOVIE, movie_plays());
    held = rx_option(HOS_OPT_PACE_START, 0) == 1;
    if (held) __atomic_store_n(&frame_handed, 1, __ATOMIC_RELAXED);   /* ((Triforce) steady_period: not this file's wait's frame) */
    return held;
}
/* (Triforce, after NX2navi's sleep_until: pace_wait.h) The acquire's wait to the frame's time: the timer up to a margin
   before it and the rest yielded away, so that the frame starts on its time and not up to half a millisecond after it.
   HOS_PACE_SPIN=<microseconds>: another margin; =0: the timer for the whole wait, as before. Every 5 s in which frames
   waited, how long after their time the waits ended (vi: frame waits), for comparing the two */
static void pace_sleep(int64_t until, int64_t now) {
    static int64_t margin = -1, waits, late_sum, late_max, since;
    if (margin < 0) {
        const char *e = getenv("HOS_PACE_SPIN");
        margin = e && *e ? atoll(e) * 1000 : PACE_WAIT_MARGIN_NS;
        if (margin < 0) margin = 0;
    }
    if (margin) hos_sleep_until_ns(until, margin);
    else hos_sleep_ns(until - now);
    const int64_t after = hos_now_ns(), late = after > until ? after - until : 0;
    /* ((Triforce) steady_period: not what the frame needed) */
    if (presenting_thread) frame_waited_own_ns += after - now;
    else __atomic_fetch_add(&frame_waited_other_ns, (LONG64)(after - now), __ATOMIC_RELAXED);
    waits++, late_sum += late;
    if (late > late_max) late_max = late;
    if (!since) since = after;
    if (after - since >= 5000000000LL) {
        char how[64];
        if (margin) snprintf(how, sizeof how, "the timer to %.2f ms before, the rest yielded", margin / 1e6);
        else snprintf(how, sizeof how, "the timer for the whole wait: HOS_PACE_SPIN=0");
        hos_log("vi: frame waits: %lld in %.1f s ended %.0f us after their time on average, %.0f us at most (%s)",
                (long long)waits, (after - since) / 1e9, late_sum / 1e3 / waits, late_max / 1e3, how);
        waits = late_sum = late_max = 0, since = after;
    }
}
void hos_vi_native_acquire(int active) {
    static int no_pacing = -1, double_buffered = -1;
    if (no_pacing < 0) no_pacing = getenv("HOS_NO_PACING") != NULL;
    if (double_buffered < 0) double_buffered = getenv("HOS_VI_DOUBLE") != NULL;
    if (no_pacing || !next_frame_at) return;
    {   /* (Triforce: another limiter holds this frame: its wait was taken in the asking, or is at the frame's present) */
        static int handed_before;
        if (pace_handed()) { handed_before = 1; return; }
        if (handed_before) {
            /* (the first frame back from another limiter. Its frames' times were not this file's: where they came sooner than
               the period, a limiter that let go, each present still put the display queue a whole period on, so the queue's times
               ran ahead of the clock by all the time not waited, and the wait below would be that long, once, a freeze. The
               queue is moved back, its frames as far apart as they were, to where this file's own pacing has it at most: its
               newest frame shown no later than a period from now for each texture the game may be ahead by) */
            const int64_t now = hos_now_ns(), n = shown_count;
            const int ahead = !double_buffered && active >= 2 ? active - 1 : 1;
            const int64_t over = (n ? shown_at[(n - 1) & 15] : 0) - (now + pace_period_ns * ahead);
            handed_before = 0;
            if (over > 0) for (int i = 0; i < 16; i++) shown_at[i] -= over;
            if (next_frame_at > now + pace_period_ns) next_frame_at = now + pace_period_ns;
        }
    }
    int64_t until = next_frame_at;
    if (!double_buffered && active >= 2) {
        const int64_t n = shown_count, frame = n - active + 1;   /* (the frame whose showing frees this texture) */
        until = frame >= 0 && frame < n ? shown_at[frame & 15] : 0;
    }
    const int64_t now = hos_now_ns();
    if (until > now) pace_sleep(until, now);
}
/* (mkw-recomp) MKW's frame pacer (0x806bcbb0, x19 when it sets the interval: nvn_native.c): its measured frame time
   (+0x128, ticks), its running excess (+0x218) and interval (+0x214), logged with the frame rate */
volatile uint64_t hos_mkw_pacer;
/* HOS_SCAN_DT=<file prefix> (diagnostic): writes every float in guest memory between ~0.015 and ~0.04, with its
   address, four times 300 presents apart: the game's frame step is either one exact 1/60 or follows the frames */
static void scan_dt(long long presents) {
    static long long first; static const char *prefix; static int scans, read_env;
    if (!read_env) read_env = 1, prefix = getenv("HOS_SCAN_DT");
    if (!prefix) return;
    if (!first) {   /* (once <prefix>go exists) */
        if (presents % 60) return;
        char go[512];
        snprintf(go, sizeof go, "%sgo", prefix);
        FILE *g = fopen(go, "r");
        if (!g) return;
        fclose(g);
        first = presents;
    }
    if (presents < first + scans * 300 || scans >= 4) return;
    char name[512];
    snprintf(name, sizeof name, "%s%d.txt", prefix, scans++);
    FILE *f = fopen(name, "w");
    if (!f) return;
    size_t hits = 0;
    for (uint64_t a = 0x80000000ULL; a < 0x2000000000ULL && hits < 4000000;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(GP(a), &m, sizeof m)) break;
        const uint64_t size = m.RegionSize;
        if (m.State == MEM_COMMIT && (m.Protect & PAGE_READWRITE)) {
            const uint32_t *w = (const uint32_t *)m.BaseAddress;
            for (size_t i = 0; i < size / 4; i++)
                if (w[i] >= 0x3c800000 && w[i] <= 0x3d230000) {
                    fprintf(f, "%llx %08x\n", (unsigned long long)((uint8_t *)&w[i] - (uint8_t *)GP(0)), w[i]);
                    if (++hits >= 4000000) break;
                }
        }
        a = (uint64_t)((uint8_t *)m.BaseAddress - (uint8_t *)GP(0)) + size;
    }
    fclose(f);
    hos_log("vi: scan %s: %zu floats", name, hits);
}
/* (hookshot) Game time follows the clock. The engine steps every frame by ratio = 60 / <the graphics module's nominal fps,
   an int at +0x2c8> (main+0x6e6cc4, f_83b6e300 makes seconds = ratio / 60 and the rest), so at 50 fps the game ran at 83%
   speed: walking the same 8 s at 52 and 33 fps went as far as those rates. Each present writes the rate of the last 5
   display intervals there, the rounding error carried to the next frame so that no time is lost; a step is at most
   1/15 s, as TotK Recomp's frame_rate.c (slower frames slow the game down, as on the console), at least 1/60 s (the game's
   own limit holds it at 60). HOS_REALTIME_STEP=0: the shipping whole steps. */
extern volatile uint64_t hos_gfx_module;
/* (local, RenoDX port) The scene's colour target as RGBA16F in place of the game's R11G11B10F (nx2/oot_patches.py: the format's
   choice at main+0x65234c asks here; host/nn_applet.c gives the render targets' pool room for it): with the Insert menu's HDR
   Native at the start, where RenoDX's shaders map the scene (gpu/vk/vk_renodx.cpp; RX_VK_RENODX=0 turns them off, and this with
   them). Decided once, at the first ask, so the pool and the target agree. HOS_SCENE_RGBA16F=0 or 1 over it. */
int hos_scene_rgba16f(void) {
    static volatile LONG known = -1;
    if (known < 0) {
        const char *e = getenv("HOS_SCENE_RGBA16F"), *r = getenv("RX_VK_RENODX");
        /* (the menu's saved choice, hdr=2: the first ask comes before the renderer has its swapchain, so it cannot be asked
           whether the display took HDR10. On a display that did not, the game stays in SDR with the wider target) */
        extern int hos_ini_option(const char *key, int otherwise);
#ifdef HOOKSHOT_RENODX
        /* (Triforce, 11-13: and the menu's RenoDX HDR row on, renodx_hdr=1, which is off unless chosen; RX_VK_RENODX=0 or 1
           over the row, as in gpu/vk/vk_menu.cpp renodx_chosen) */
        const LONG v = e ? atoi(e) != 0 : hos_ini_option("hdr", 0) == 2 && (r && *r ? atoi(r) != 0 : hos_ini_option("renodx_hdr", 0) == 1);
#else   /* (a build without RenoDX's shaders, nx2/build.py HOOKSHOT_RENODX: nothing asks for the wider target) */
        const LONG v = e ? atoi(e) != 0 : 0;
        (void)r;
#endif
        if (InterlockedCompareExchange(&known, v, -1) == -1)
            hos_log("vi: the scene's colour target: %s", v ? "RGBA16F (HDR Native at the start: for RenoDX's tone mapping)" : "the game's R11G11B10F");
    }
    return (int)known;
}
volatile int hos_fixed_resolution = 0;   /* (the lifted code reads these: nx2/oot_patches.py) */
volatile uint32_t hos_fixed_scale_bits = 0x3f800000;   /* (HOS_RES_SCALE: the fixed path's scale) */
volatile int hos_dynres_runs = 0;   /* (the dynamic path's frames: it fills fields the 3D reads, +8 +16 +168) */
volatile float hos_text_scale = 1.0f;   /* (the letters' clock of a text pane, f_80b1e140: nx2/oot_patches.py) */
/* The Insert menu's Text speed (0 Normal, 1 2x, 2 4x, 3 Instant), HOS_TEXT_SPEED=<0..3> over it: what the time of a letter
   animation's step is multiplied by. Instant: the page at once, as the game's own skip on A */
static float text_scale(const char *env, int menu) {
    const int v = env && *env ? atoi(env) : menu;
    return v == 1 ? 2.0f : v == 2 ? 4.0f : v >= 3 ? 1e6f : 1.0f;
}
volatile uint64_t hos_step_probe = 0;   /* (f_80706be0's object, set by the lifted code when the frame-time hook is in this build; its
                                            +0xd58 is the game clock in 1/60 s: nx2/oot_patches.py) */
volatile uint32_t hos_frame_seconds_bits = 0;   /* (the frame's seconds as a float's bits for that hook, 0: the game's own step) */
/* A movie's frame count while the frames' time is given (a Frame rate cap above 60): how many 60ths of a second this update of
   that movie player is worth, the rest carried to its next one, so the count the cutscene's cues are placed by keeps the clock
   as the movie's picture does (issue 1742: the narration ran ahead above 60 fps). 1, the game's own step, otherwise, and with
   HOS_FPS_MOVIE=0 (for comparisons). Called by the lifted f_83b7c710 (nx2/oot_patches.py). */
uint32_t hos_movie_frames(uint64_t player) {
    enum { PLACES = 16 };
    movie_seen();   /* (Triforce: a movie plays, for the frame pacing: pace_handed) */
    const uint32_t bits = __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED);   /* (realtime_step writes it at each present) */
    if (!bits) return 1;
    static SRWLOCK lock = SRWLOCK_INIT;
    static int timed = -1;   /* (these four under the lock) */
    static struct { uint64_t player, used; double part; } carry[PLACES];
    static uint64_t calls;
    static unsigned made;
    float secs;
    uint32_t n = 1;
    memcpy(&secs, &bits, 4);
    AcquireSRWLockExclusive(&lock);
    if (timed < 0) timed = !(getenv("HOS_FPS_MOVIE") && !atoi(getenv("HOS_FPS_MOVIE")));
    if (timed) {
        int i = 0;
        while (i < PLACES && carry[i].player != player) i++;
        if (i == PLACES) {
            /* (a player not seen yet takes the place used longest ago, so a movie that plays keeps its own while others come
               and go: its count is exact while no more than PLACES players, itself among them, update in turn. It starts at a part
               of a 60th drawn from a hash of how many places were made: with that many players at once every update is a first
               one, and each count then still grows by the frames' time on average, whatever the number of players, instead of
               standing still. A sequence with a fixed step, the golden ratio's multiples, stood still for one player in 233) */
            uint32_t x = made++;
            x ^= x >> 16, x *= 0x85ebca6bu, x ^= x >> 13, x *= 0xc2b2ae35u, x ^= x >> 16;   /* (MurmurHash3's last step) */
            i = 0;
            for (int k = 1; k < PLACES; k++)
                if (carry[k].used < carry[i].used) i = k;
            carry[i].player = player, carry[i].part = x / 4294967296.0;
        }
        carry[i].used = ++calls;
        carry[i].part += (double)secs * 60.0;
        n = (uint32_t)carry[i].part;
        carry[i].part -= n;
    }
    ReleaseSRWLockExclusive(&lock);
    return n;
}
/* (nx2/oot_patches.py patch_whole_frames) the whole frames a game counter at `at` takes for an amount of the frame step (its
   float's bits): with the frames' own time as the step, their fractions carried per counter; else cut as the game cuts it */
#include "frame_whole.h"
static FrameWhole frame_whole_state;
static SRWLOCK frame_whole_lock = SRWLOCK_INIT;   /* (the game's update threads) */
uint32_t hos_frame_whole(uint64_t at, uint32_t amount_bits) {
    AcquireSRWLockExclusive(&frame_whole_lock);
    const uint32_t whole = frame_whole(&frame_whole_state, at, amount_bits, __atomic_load_n(&hos_frame_seconds_bits, __ATOMIC_RELAXED) != 0);
    ReleaseSRWLockExclusive(&frame_whole_lock);
    return whole;
}
volatile uint64_t hos_res_module = 0;   /* (f_831f3200's object, whose +0xe5c is the 3D's scale: nx2/oot_patches.py) */
/* Quantize the time step to the engine's integer nominal rate. Keep debt from a short frame even when the minimum
   step is 1/60: discarding it made alternating short/long frames advance more game time than elapsed wall time.
   Keep at most four nominal frames of debt so unpaced fast presents cannot leave a long recovery. Steps beyond
   1/15 s are still dropped, rather than caught up after a long stall. */
static int realtime_rate(double ratio, int maximum, double *carry) {
    ratio += *carry;
    if (ratio > 4.0) ratio = 4.0;
    const double least = 60.0 / maximum;
    const double bounded = ratio < least ? least : ratio;
    int fps = (int)(60.0 / bounded + 0.5);
    fps = fps < 15 ? 15 : fps > maximum ? maximum : fps;
    *carry = ratio - 60.0 / fps;
    if (*carry < -4.0) *carry = -4.0;
    return fps;
}
static void realtime_step(int64_t now) {
    int rx_option(int id, int otherwise);
    /* (Resolution 100% from the start once came out black (the dynamic block's fields unset), so the dynamic path ran 30
       frames first. Those frames left the 3D at the controller's size, 60% of 1440p (1536x864, then the game's network to
       2048x1152), whatever the fixed scale said. 11-08: from the first frame, as EvilOOT's Max Resolution patch, the 3D
       draws at the full 2560x1440, no network pass, not black, same fps; tree edges 23.4 -> 36.1 at the rock) */
    {   /* (Evil Hookshot, HOS_FIXED_FROM=<n>: the dynamic path's frames before the fixed one, 0 by default = EvilOOT's
           built-in patch, the fixed path from the first frame) */
        static int from = -1;
        if (from < 0) from = getenv("HOS_FIXED_FROM") ? atoi(getenv("HOS_FIXED_FROM")) : 0;
        /* (HOS_RES_PULSE=<frame>: Auto for 240 frames from that frame, then back: the menu's Auto -> 100% in a lab run) */
        static long long frame, pulse = -2;
        if (pulse == -2) pulse = getenv("HOS_RES_PULSE") ? atoll(getenv("HOS_RES_PULSE")) : -1;
        frame++;
        const int pulsed = pulse >= 0 && frame >= pulse && frame < pulse + 240;
        if (pulse >= 0 && (frame == pulse || frame == pulse + 240)) hos_log("vi: resolution pulse: %s at frame %lld", pulsed ? "Auto" : "back to fixed", frame);
        /* (the menu's Resolution is read once, at the start: switching to Auto in game ran the dynamic path, whose size the
           game keeps elsewhere, and 100% after it stayed at that size; putting the resolution object back did not undo it.
           Read when the renderer answers: before, rx_option gives its fallback) */
        static int dyn_at_start = -1;
        if (dyn_at_start < 0) dyn_at_start = rx_option(2, -1);
        hos_fixed_resolution = !pulsed && dyn_at_start == 0 && hos_dynres_runs >= from;
    }
    {   /* (Evil Hookshot, HOS_RES_LOG=1: the resolution switch's state every 600 frames) */
        static int on = -1;
        static long long n;
        if (on < 0) on = getenv("HOS_RES_LOG") != NULL;
        if (on && !(n++ % 600))
            hos_log("vi: resolution: menu dynamic %d, scale %d/10000, fixed %d, dynamic path runs %d, scale bits %08x", rx_option(2, -1),
                    rx_option(3, -1), hos_fixed_resolution, hos_dynres_runs, (unsigned)hos_fixed_scale_bits);
    }
    {
        static const char *env;
        static int looked;
        if (!looked) looked = 1, env = getenv("HOS_TEXT_SPEED");
        hos_text_scale = text_scale(env, rx_option(5, 0));
    }
    {   /* (the Insert menu's Resolution: the fixed path's scale in 1/10000, read once at the start as the mode above;
           HOS_RES_SCALE below stays over it) */
        static int scale = -1;
        if (scale < 0) scale = rx_option(3, -1);
        if (scale > 0 && !getenv("HOS_RES_SCALE")) {
            const float v = scale / 10000.0f;
            memcpy((void *)&hos_fixed_scale_bits, &v, 4);
        }
    }
    {
        static int read_scale;
        if (!read_scale) {
            read_scale = 1;
            if (getenv("HOS_RES_SCALE")) { float v = (float)atof(getenv("HOS_RES_SCALE")); memcpy((void *)&hos_fixed_scale_bits, &v, 4); }
        }
    }
    {   /* HOS_RES_LOG=1: the 3D's scale every 300 presents. +0xe20: flags (bit 3: 0.75, bit 4: 0.5 in place of the scale),
           +0xe2c bit 3: recompute asked, +0xe50/+0xe54/+0xe58: the sizes the scale is taken against, +0xe5c: the scale */
        static int on = -1;
        static long long n;
        if (on < 0) on = getenv("HOS_RES_LOG") != NULL;
        const uint64_t m = hos_res_module;
        if (on && m && !(n++ % 300)) {
            float f[4], s;
            memcpy(f, GP(m + 0xe50), sizeof f);
            memcpy(&s, (const void *)&hos_fixed_scale_bits, 4);
            hos_log("vi: res: fixed %d (scale %.3f), +0xe20 %llx, +0xe2c %x, +0xe50 %.0f %.0f %.0f, 3D scale %.3f", hos_fixed_resolution, s,
                    (unsigned long long)*(uint64_t *)GP(m + 0xe20), *(uint32_t *)GP(m + 0xe2c), f[0], f[1], f[2], f[3]);
        }
    }
    const uint64_t mod = hos_gfx_module;
    if (!mod) return;
    {   /* (hookshot) The frame limiter (f_8042cd20 sleeps to 1 / +0xfc). The engine's adaptive controller writes +0xfc when
           variable refresh is on (+0x231, f_804fc0f0), any rate from 40 up. HOS_FPS_TARGET=<fps>: that rate kept, 0: left alone;
           HOS_FPS_LOG=1: what the game asks for, every 300 presents */
        static int target = -1, log_on = -1;
        static long long n;
        if (target < 0) target = getenv("HOS_FPS_TARGET") ? atoi(getenv("HOS_FPS_TARGET")) : 0, log_on = getenv("HOS_FPS_LOG") != NULL;
        int *limit = (int *)GP(mod + 0xfc);
        if (log_on && !(n++ % 300))
            hos_log("vi: frame limiter %d, vrr %d %d, nominal %d, controller +0x58.. %d %d %d %d", *limit, *(uint8_t *)GP(mod + 0x231),
                    *(uint8_t *)GP(mod + 0x232), *(int *)GP(mod + 0x2c8), *(int *)GP(mod + 0x270 + 0x58), *(int *)GP(mod + 0x270 + 0x28),
                    *(int *)GP(mod + 0x270 + 0x2c), *(int *)GP(mod + 0x270 + 0x30));
        if (log_on && !((n - 1) % 300) && hos_step_probe) {   /* (game speed: the game clock's 1/60 s units a second of wall time, 60.0 at speed 1) */
            static double clock_before;
            static int64_t at_before;
            double game_clock;
            memcpy(&game_clock, GP(hos_step_probe + 0xd58), 8);
            if (at_before && now > at_before) {
                const double rate = (game_clock - clock_before) * 1e9 / (double)(now - at_before);
                hos_log("vi: game clock %.1f: %.2f a second of wall time (speed %.3f) at %.1f fps", game_clock, rate, rate / 60.0,
                        300e9 / (double)(now - at_before));
            }
            clock_before = game_clock, at_before = now;
        }
        if (target && *limit > 0 && *limit < target) *limit = target;
        if (fps_max() > 60 && *limit > 0 && *limit < fps_max()) *limit = fps_max();
    }
    /* (the Insert menu's Game speed "per frame": a frame is a 60th of a second of game time. Not past 60 fps: the game would run
       faster than real time, so a cap above 60 keeps the real-time step whatever that row says) */
    /* (nor under 60: a cap of 30 would be the game at half speed for good) */
    if (!rx_option(0, 1) && fps_max() == 60) {
        int *slot = (int *)GP(mod + 0x2c8);
        if (*slot >= 15 && *slot <= 60) *slot = 60;
        return;
    }
    static int64_t last;
    static double samples[5], carry;
    static unsigned count, next;
    static long long steps;
    static StepSteady steady;   /* ((Triforce) HOS_STEP_STEADY=1, below) */
    const double dt = (now - last) / 1e9;
    last = now;
    if (dt <= 0 || dt > 0.25) { count = next = 0; carry = 0; step_steady_reset(&steady); return; }   /* (a load: the average starts again) */
    /* (HOS_REALTIME_WINDOW=<1-5>: frames averaged for the step; 1: each step the last frame's own time) */
    static int window = -1;
    if (window < 0) {
        window = getenv("HOS_REALTIME_WINDOW") ? atoi(getenv("HOS_REALTIME_WINDOW")) : 5;
        window = window < 1 ? 1 : window > 5 ? 5 : window;
    }
    samples[next++ % window] = dt;
    if (count < (unsigned)window) count++;
    double sum = 0;
    for (unsigned i = 0; i < count; i++) sum += samples[i];
    /* (hookshot) A cap above 60: the engine's own rate is 60 / <whole display intervals>, and its controller puts 60 back over a
       higher +0x2c8 before the step is made from it, so every frame still stepped 1/60 s and the game ran fps / 60 times too fast.
       The step is then the frames' own time at the two places the engine makes it (nx2/oot_patches.py, hos_frame_seconds_bits):
       this same average, at most 1/15 s as below and at least 1/240 s, and the rate under it stays the engine's, 60 at most.
       HOS_FPS_STEP=0, or a lift without the hook (hos_step_probe unset): the rate up to the cap, as before */
    static int timed_step = -1;
    if (timed_step < 0) timed_step = !(getenv("HOS_FPS_STEP") && !atoi(getenv("HOS_FPS_STEP")));
    const int timed = fps_max() > 60 && timed_step && hos_step_probe;
    const int top = timed ? 60 : fps_max();
    /* (Triforce, after NX2navi's steady step: host/step_steady.h) HOS_STEP_STEADY (on unless =0): the step is not the mean of the last five
       intervals but that of the last 12 without their longest and shortest, the paced period where it is within 3% of it, and
       what the display showed and the game was not given repaid at a 64th a frame (10% of the step at most), within the bounds
       each path below gives it: a hitch is paid back over most of a second, not in the next five frames, and a cap the game
       holds steps it by the period exactly. =0: the mean of the last five, as before */
    double secs_avg = sum / count;
    {
        static int steady_on = -1;
        if (steady_on < 0) {
            /* ((Triforce, 11-15) On by default, as NX2navi's STEP_STEADY: the lab's A/B cost nothing at the Market; its gain is
               at hitches (tests/test_step_steady.py). HOS_STEP_STEADY=0: the mean of the last five, as before) */
            steady_on = !(getenv("HOS_STEP_STEADY") && *getenv("HOS_STEP_STEADY") && atoi(getenv("HOS_STEP_STEADY")) == 0);
            if (steady_on) hos_log("vi: the steady step: 12 intervals without their longest and shortest, the paced period within 3%%, a hitch repaid slowly (HOS_STEP_STEADY=0: off)");
        }
        if (steady_on) secs_avg = step_steady(&steady, dt, (double)pace_period_ns / 1e9, timed ? 1.0 / 240.0 : 1.0 / top, 4.0 / 60.0);
    }
    if (timed) {
        const double secs = secs_avg;
        const float f = (float)(secs > 4.0 / 60.0 ? 4.0 / 60.0 : secs < 1.0 / 240.0 ? 1.0 / 240.0 : secs);
        uint32_t bits;
        memcpy(&bits, &f, 4);
        __atomic_store_n(&hos_frame_seconds_bits, bits, __ATOMIC_RELAXED);   /* (read by the game's threads) */
    } else {
        __atomic_store_n(&hos_frame_seconds_bits, 0, __ATOMIC_RELAXED);
    }
    const double ratio = secs_avg * 60.0;
    int *slot = (int *)GP(mod + 0x2c8);
    if (*slot < 15 || *slot > (fps_max() > 60 ? fps_max() : 60)) {   /* (under a cap below 60 it starts at the game's 60) */
        static int logged;
        if (!logged++) hos_log("vi: the graphics module's frame rate reads %d: no real-time step", *slot);
        return;
    }
    /* The timed hook already supplies the actual frame time. Its nominal rate tracks this frame without debt. */
    double frame_carry = timed ? 0 : carry;
    const int fps = realtime_rate(ratio, top, &frame_carry);
    carry = timed ? 0 : frame_carry;
    *slot = fps;
    if (!(steps++ % 3000)) hos_log("vi: real-time step on: ratio %.3f, game's frame rate %d", ratio, fps);
}
/* (Triforce) The Steady frame rate row (steady_frame_rate=, HOS_FPS_STEADY=<0-2> over it; read at the start): 0 Off, the frames
   paced by the cap as before. 1 (any rate: a display with variable refresh, or VSync off) and 2 (steps of the display's refresh,
   for VSync without variable refresh): the period the frames are paced by is one this PC holds in the scene, lowered quickly when
   frames run late and raised slowly when there is room, never shorter than the cap's (base; host/pace_steady.h has the rule and
   its reasons), and the renderer holds the game's pictures to it (HOS_OPT_PACE_STEADY, gpu/vk/vk_pace.cpp steady_floor_ns,
   present_smooth.h present_smooth_paced_at): the acquire's wait evens the frames' starts and with them the game's time step
   (realtime_step's steady step snaps to the period), the present's hold their pictures.
   What a frame took and needed (pace_steady_measure): its present interval, and the pacing's waits in it, the acquire's
   (pace_sleep, on this thread or another) and the present's hold (the renderer says how long). A frame another limiter held
   (pace_handed) is not measured, and the cap's period is back until the game's own wait holds the frames again. A line at
   each change, and every 5 s what the rule was given (steady_line) */
typedef struct {   /* (what steady_line says: the frames since its last line) */
    int64_t interval[2048], took[2048], need[2048], own, other, held, since;
    int n, frames;
    int64_t hitches, loads, unmeasured, kept, settling;
} SteadyWindow;
/* (the 5 s line's last part: the probes' measure, or the wait before a raise without them) */
static const char *steady_line_rule(const PaceSteady *s) {
    static char text[160];
    if (s->probe)
        snprintf(text, sizeof text, "%lld probes (%lld found a new scene), the scene's last took %.2f / %.2f ms unpaced "
                 "(p50 / p90)", (long long)s->probes, (long long)s->scenes, s->probe_p50 / 1e6, s->probe_p90 / 1e6);
    else snprintf(text, sizeof text, "a raise waits for %.0f s since the last change (HOS_FPS_STEADY_PROBE=0)", s->hold_ns / 1e9);
    return text;
}
static void steady_line(SteadyWindow *w, PaceSteady *s, int64_t now) {
    if (!w->since) w->since = now;
    if (now - w->since < 5000000000LL) return;
    unsigned late = 0;
    if (s->count >= PACE_STEADY_SHORT)
        for (unsigned i = 0; i < PACE_STEADY_SHORT; i++) late += s->took[i] > pace_steady_held(s);
    const int n = w->n, f = w->frames ? w->frames : 1;
#define STEADY_Q(a, q) (n ? pace_steady_select(a, n, (int)(n * (q)) < n ? (int)(n * (q)) : n - 1) / 1e6 : 0.0)
    const double iv50 = STEADY_Q(w->interval, 0.5), took50 = STEADY_Q(w->took, 0.5), took90 = STEADY_Q(w->took, 0.9);
    const double need50 = STEADY_Q(w->need, 0.5), need90 = STEADY_Q(w->need, 0.9);
#undef STEADY_Q
    hos_log("vi: steady frame rate: %.1f fps held; the last %.1f s, %d frames: took %.2f / %.2f ms (p50 / p90), needed %.2f / "
            "%.2f, present interval p50 %.2f; a frame's waits: the acquire's %.2f ms on the presenting thread, %.2f on others, the "
            "present's hold %.2f; %lld kept, left out %lld hitches, %lld loads, %lld unmeasured, %lld settling after a load; %u of "
            "the last %d kept late; %lld raised, %lld lowered so far, %s",
            1e9 / (double)pace_steady_held(s), (now - w->since) / 1e9, w->frames, took50, took90, need50, need90, iv50,
            w->own / 1e6 / f, w->other / 1e6 / f, w->held / 1e6 / f, (long long)(s->kept - w->kept),
            (long long)(s->hitches - w->hitches), (long long)(s->loads - w->loads), (long long)(s->unmeasured - w->unmeasured),
            (long long)(s->settling - w->settling), late, PACE_STEADY_SHORT, (long long)s->raises, (long long)s->lowerings,
            steady_line_rule(s));
    w->n = w->frames = 0, w->own = w->other = w->held = 0, w->since = now;
    w->hitches = s->hitches, w->loads = s->loads, w->unmeasured = s->unmeasured, w->kept = s->kept;
    w->settling = s->settling;
}
static int64_t steady_period(int64_t base, int64_t now) {
    static int mode = -1, cap;
    static PaceSteady steady;
    static SteadyWindow window;
    static int64_t last;
    static long long frames;
    int rx_option(int id, int otherwise);
    void rx_option_set(int id, int value);
    presenting_thread = 1;   /* (pace_sleep: the acquire's waits on this thread add to the present's hold) */
    const int64_t own = frame_waited_own_ns, interval = last ? now - last : 0;
    const int64_t other = (int64_t)__atomic_exchange_n(&frame_waited_other_ns, 0, __ATOMIC_RELAXED);
    const int handed = __atomic_exchange_n(&frame_handed, 0, __ATOMIC_RELAXED) != 0;
    frame_waited_own_ns = 0, last = now;
    if (mode < 0) {
        const char *e = getenv("HOS_FPS_STEADY");
        mode = e && *e ? atoi(e) : hos_ini_option("steady_frame_rate", PACE_STEADY_OFF);
        if (mode < PACE_STEADY_OFF || mode > PACE_STEADY_REFRESH) mode = PACE_STEADY_OFF;
    }
    if (mode == PACE_STEADY_OFF) return base;
    frames++;
    if (cap != fps_max()) {
        /* (the set, once the cap is settled: on Monitor's rate fps_max is 60 until the renderer answers. Steps of the refresh
           wait for the renderer to say the display's rate, half a minute of frames at most: then the set of any rate) */
        int refresh = mode == PACE_STEADY_REFRESH ? rx_option(HOS_OPT_DISPLAY_CAP, -1) : 0;
        if (refresh < 0 && frames < 1800) return base;
        pace_steady_init(&steady, mode, fps_max(), refresh);
        cap = fps_max();
        /* ((Triforce, test31) HOS_FPS_STEADY_PROBE=0: the rate by the frames' own measure as they run, as before the probes) */
        const char *pe = getenv("HOS_FPS_STEADY_PROBE");
        steady.probe = !(pe && *pe && atoi(pe) == 0);
        hos_log("vi: steady frame rate: %s, %d rates from the cap's %d fps to %.1f, %s (%s)",
                mode == PACE_STEADY_REFRESH && refresh > 0 ? "steps of the display's refresh" : "any rate", steady.n,
                cap, 1e9 / (double)steady.periods[steady.n - 1],
                steady.probe ? "chosen by unpaced probes" : "chosen by the frames as they run (HOS_FPS_STEADY_PROBE=0)",
                mode == PACE_STEADY_REFRESH && refresh <= 0 ? "the display's refresh rate is not known" : "HOS_FPS_STEADY=0: off");
    }
    const int64_t held = (int64_t)rx_option(HOS_OPT_PACE_STEADY, 0) * 1000;   /* (the present's hold, since the last present) */
    if (handed) {
        if (steady.at || steady.count) pace_steady_reset(&steady), hos_log("vi: steady frame rate: another limiter holds the frames");
        rx_option_set(HOS_OPT_PACE_STEADY, 0);
        window.frames++;
        steady_line(&window, &steady, now);
        return base;
    }
    const int64_t before = pace_steady_held(&steady);
    int64_t took, need;
    pace_steady_measure(interval, own + held, other, &took, &need);
    if (other > 0) steady.one_step = 1;   /* (the acquire on another thread: what a frame needs is not always known) */
    if (interval > 0 && interval <= PACE_STEADY_LOAD_NS && window.n < 2048) {
        window.interval[window.n] = interval, window.took[window.n] = took, window.need[window.n] = need;
        window.n++;
    }
    window.frames++, window.own += own, window.other += other, window.held += held;
    if (pace_steady_feed(&steady, took, need, interval)) {
        const int64_t p = pace_steady_held(&steady);
        if (steady.probe)
            hos_log("vi: steady frame rate: %.1f fps (was %.1f): the last three probes' %u frames took %.2f ms unpaced at the 90th "
                    "percentile, %.2f at the median (%lld probes, %lld raised, %lld lowered)", 1e9 / (double)p, 1e9 / (double)before,
                    steady.probe_count, steady.probe_p90 / 1e6, steady.probe_p50 / 1e6, (long long)steady.probes,
                    (long long)steady.raises, (long long)steady.lowerings);
        else
            hos_log("vi: steady frame rate: %.1f fps (was %.1f): the last %u frames needed %.2f ms at the 90th percentile, %.2f at "
                    "the 95th (%lld raised, %lld lowered, %lld hitches left out; the next raise waits %.0f s at least)", 1e9 / (double)p,
                    1e9 / (double)before, steady.count, pace_steady_quantile(&steady, PACE_STEADY_RING, 0.90) / 1e6,
                    pace_steady_quantile(&steady, PACE_STEADY_RING, 0.95) / 1e6, (long long)steady.raises,
                    (long long)steady.lowerings, (long long)steady.hitches, steady.hold_ns / 1e9);
    }
    steady_line(&window, &steady, now);
    const int64_t p = pace_steady_period(&steady) > base ? pace_steady_period(&steady) : base;
    rx_option_set(HOS_OPT_PACE_PERIOD, (int)p);
    rx_option_set(HOS_OPT_PACE_STEADY, 1);
    return p;
}
/* HOS_PRESENT_TRACE=<csv>: native game present-call returns, not GPU completion or display times.
   Uses the existing frame ID/clock; a forced stop can lose the last second of buffered present records. */
static void present_trace(int64_t frame, int64_t now) {
    static FILE *file;
    static char buffer[64 * 1024];
    static int looked;
    static int64_t last, flushed;
    if (!looked) {
        const char *path = getenv("HOS_PRESENT_TRACE");
        looked = 1;
        if (path && *path) {
            file = fopen(path, "w");
            if (!file) hos_log("vi: could not open present trace %s", path);
        }
        if (file) {
            setvbuf(file, buffer, _IOFBF, sizeof buffer);
            fputs("frame,return_ns,interval_ns\n", file);
            flushed = now;
        }
    }
    if (!file) return;
    fprintf(file, "%lld,%lld,%lld\n", (long long)frame, (long long)now, (long long)(frame > 1 ? now - last : 0));
    last = now;
    if (now - flushed >= 1000000000LL) {
        fflush(file);
        flushed = now;
    }
}
void hos_vi_native_presented(void) {
    static long long scan_n;
    scan_dt(++scan_n);
    {   /* HOS_FRAME_SLEEP_MS (diagnostic): slower frames, for game speed against frame rate */
        static int ms = -1;
        if (ms < 0) ms = getenv("HOS_FRAME_SLEEP_MS") ? atoi(getenv("HOS_FRAME_SLEEP_MS")) : 0;
        if (ms) hos_sleep_ns(ms * 1000000LL);
    }
    /* (a cap under 60 paces by itself, whatever interval the game asked for while Monitor's rate was not known yet) */
    const int interval = fps_max() < 60 ? 1 : present_interval;
    const int64_t now = hos_now_ns();
    /* ((Triforce) the Steady frame rate row: a period this PC holds in the scene, never shorter than the cap's) */
    const int64_t period = steady_period(1000000000LL / fps_max() * interval, now);
    pace_period_ns = period;   /* (Triforce: what another limiter is given to hold in this file's place, pace_handed) */
    realtime_step(now);
    /* (behind by more than a frame: the next one is paced from now, with no burst to catch up) */
    next_frame_at = next_frame_at && now - next_frame_at < period ? next_frame_at + period : now + period;
    {   /* (the display queue: this frame is shown one interval after the last, or now when the display waits) */
        const int64_t n = shown_count, last = n ? shown_at[(n - 1) & 15] : 0;
        shown_at[n & 15] = last + period > now ? last + period : now;
        shown_count = n + 1;
    }
    const int64_t n = InterlockedIncrement64(&presents);
    present_trace(n, now);
    {   /* HOS_MKW_INTERVAL=<0|1|2>: the interval MKW's pacer is held at, through its own override (+0x210, any
           non-zero value is used as the interval): 1 (the default): a steady 60 fps; 2: 30; 0: the game's choice,
           which with frames between 16.7 and 33 ms swings between the two (25-27 fps, stutters). Its timing counts
           vsyncs, so an interval the renderer cannot keep slows the game down rather than dropping frames */
        static int forced = -1;
        if (forced < 0) forced = getenv("HOS_MKW_INTERVAL") ? atoi(getenv("HOS_MKW_INTERVAL")) : 1;
        if (hos_mkw_pacer && forced >= 0 && forced <= 2) G32(hos_mkw_pacer + 0x210) = (uint32_t)forced;
    }
    if (!window_start) window_start = now;
    frames_in_window++;
    /* (present-to-present intervals of the window, in buckets: which frames miss their vsync) */
    static int64_t last_present;
    static int hist[6];
    /* ((Triforce) and against the period the frames are paced by, which the buckets above, made for 60, do not see at a cap
       of 240 (every interval under 15.5 ms): from under three quarters of it to over twice it, and the intervals' mean
       distance from it, the frame-time spread a change of the pacing shows in) */
    static int rel[6];
    static int64_t off_sum;
    if (last_present) {
        const int64_t d = now - last_present, off = d > period ? d - period : period - d;
        hist[d < 15500000 ? 0 : d < 17800000 ? 1 : d < 22000000 ? 2 : d < 30000000 ? 3 : d < 40000000 ? 4 : 5]++;
        rel[d * 4 < period * 3 ? 0 : d * 20 < period * 19 ? 1 : d * 20 <= period * 21 ? 2 : d * 4 <= period * 5 ? 3 : d <= period * 2 ? 4 : 5]++;
        off_sum += off;
    }
    last_present = now;
    if (now - window_start >= 5000000000LL) {
        hos_log("vi: %lld frames presented (%.1f fps over the last %.1f s, interval %d)", (long long)n,
                frames_in_window * 1e9 / (double)(now - window_start), (now - window_start) / 1e9, interval);
        hos_log("vi: present intervals: <15.5 ms %d, 15.5-17.8 %d, 17.8-22 %d, 22-30 %d, 30-40 %d, over 40 %d", hist[0],
                hist[1], hist[2], hist[3], hist[4], hist[5]);
        const int counted = rel[0] + rel[1] + rel[2] + rel[3] + rel[4] + rel[5];
        hos_log("vi: frame intervals against the paced period of %.3f ms: under 0.75 of it %d, 0.75-0.95 %d, 0.95-1.05 %d, "
                "1.05-1.25 %d, 1.25-2 %d, over 2 %d; %.3f ms from it on average", period / 1e6, rel[0], rel[1], rel[2], rel[3],
                rel[4], rel[5], counted ? off_sum / 1e6 / counted : 0.0);
        memset(hist, 0, sizeof hist);
        memset(rel, 0, sizeof rel), off_sum = 0;
        window_start = now, frames_in_window = 0;
        const uint64_t pacer = hos_mkw_pacer, freq = pacer ? G64(0x837de940) : 0;   /* (MKW only: nvn_native.c) */
        if (pacer && freq)
            hos_log("vi: the game's pacer: frame work %.1f ms, excess %.1f ms, interval %d",
                    (double)(int64_t)G64(pacer + 0x128) * 1e3 / (double)freq,
                    (double)(int64_t)G64(pacer + 0x218) * 1e3 / (double)freq, (int)G32(pacer + 0x214)),
            hos_log("vi: pacer raw: now %llx f0 %llx 128 %llx e8 %llx 50 %x 210 %x 22c %x freq %llu", (unsigned long long)hos_ticks(),
                    (unsigned long long)G64(pacer + 0xf0), (unsigned long long)G64(pacer + 0x128),
                    (unsigned long long)G64(pacer + 0xe8), G32(pacer + 0x50), G32(pacer + 0x210), *(const uint8_t *)GP(pacer + 0x22c),
                    (unsigned long long)freq);
    }
}
