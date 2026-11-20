// (Triforce) Who holds a frame of the game to the Game frame rate cap, after NX2navi's one limiter a frame (its
// runtime/frame_rate.c frame_step_cap_ns, vk_dlssg.c vk_dlssg_pace and rtss_limit.c).
//
// The game's frames are paced by the host: where the game takes a window texture, the frame's start, it waits for the
// frame's time under the cap (host/nn_vi.c hos_vi_native_acquire). A second limiter that holds the same frames makes a
// frame wait twice. So the host asks here first, once a frame, on the game's presenting thread (HOS_OPT_PACE_PERIOD,
// HOS_OPT_PACE_MOVIE, then HOS_OPT_PACE_START), and its own wait steps aside for a frame another limiter holds. One of
// three for every frame, never two and never none (pace_rule.h pace_owner):
//
//   RivaTuner Statistics Server, where it is installed and running: by the owner's word it is the game's limiter
//   whenever it can hold the game, with no row to turn it on. The game's limit goes into RTSS's profile for the game
//   (rtss_limit.cpp, on a thread of its own that looks twice a second), and once RTSS is seen holding the game to it
//   this frame is RTSS's. What it is asked to hold is the limit on presented frames: the cap, times the frames shown
//   for each of the game's with the Frame generation row's fixed multipliers (pace_rule.h says why Auto and DLSS frame
//   generation keep RTSS out). A movie, a window that shows nothing, an RTSS that is not there or has not hooked the
//   game yet, a profile that cannot be written (unless a limit of the player's own in RTSS holds the game) and an RTSS
//   that is seen not to hold the game each leave the frame to the game's own wait. HOS_RTSS=0 keeps RTSS out entirely:
//   nothing of it is read or loaded.
//
//   NVIDIA Reflex with DLSS frame generation, with the Display tab's Reflex frame limit row: its sleep is taken inside
//   the call (vk_dlssg.cpp pace, dlssg_pace.h). Only where RivaTuner does not hold the frame: with RivaTuner as the
//   limiter Reflex is given no frame limit, and its sleep, without one, stays where it was.
//   (Triforce, after NX2navi's vk_lowlat.c) With RX_VK_REFLEX_LIMIT=1, without DLSS frame generation, the renderer's own
//   Reflex (vk_reflex.cpp reflex_pace) the same way, its sleep taken inside the call; a movie is the game's own wait's.
//
//   The game's own wait, in every other case.
#include "vk_pace.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "pace_rule.h"
#include "rtss_limit.h"
#include "vk_dlssg.h"
#include "vk_reflex.h"
#include "vk_framegen.h"
#include "vk_internal.h"

namespace mkwvk::pace {

static_assert((int)GAME == PACE_GAME && (int)REFLEX == PACE_REFLEX && (int)RTSS == PACE_RTSS, "vk_pace.h's owners are pace_rule.h's");

namespace {

std::atomic<int> period_ns{0};        // the host's period, as last told
std::atomic<int> movie_on{0};         // a movie plays, as last told
std::atomic<int> owner_now{GAME};
std::atomic<int> yields{0};           // the display thread's own pacing stands aside (display_yields)
std::atomic<int> fg_now{PACE_FG_OFF}, mult_now{1};   // where the presents come from, as the newest frame start found it
std::atomic<uint64_t> hosted{0};      // the host's clock after the newest present
RtssWatch watch;                      // the game's presenting thread's (frame_start)
uint64_t start_last, start_before;    // and the starts of its last two frames (rtss_frame_holds)
std::atomic<int> begun{0};            // the first frame with a period started the worker (or said HOS_RTSS=0)
std::atomic<int> mode_known{-2}, dlssg_try_known{-1};   // rtss_mode's and dlssg_try's, read once
// ((Triforce) the Steady frame rate row: whether the period told is its own, and how long the present held the game's
// presenting thread for it since the host last asked; that thread is the one that asks, and says where its frame starts)
std::atomic<int> steady_on{0};
std::atomic<int64_t> steady_held_ns{0};
thread_local bool presenting = false;

/// RivaTuner's part in this run: 1 the game's limiter where it can be, 0 kept out entirely (HOS_RTSS=0, from
/// triforce_env.txt too), -1 a system without it
int rtss_mode() {
    int mode = mode_known.load(std::memory_order_relaxed);
    if (mode == -2) {
        const char* e = getenv("HOS_RTSS");
        mode = !rtss::real_sys() ? -1 : e && *e && atoi(e) == 0 ? 0 : 1;
        mode_known.store(mode, std::memory_order_relaxed);
    }
    return mode;
}
/// HOS_RTSS_DLSSG=1: RivaTuner tries to hold the frames NVIDIA's Streamline shows too (pace_rule.h)
bool dlssg_try() {
    int on = dlssg_try_known.load(std::memory_order_relaxed);
    if (on < 0) {
        const char* e = getenv("HOS_RTSS_DLSSG");
        on = e && atoi(e) != 0;
        dlssg_try_known.store(on, std::memory_order_relaxed);
    }
    return on != 0;
}

/// Where the presents come from now, and how many for each of the game's frames
void presents(int* fg, int* mult) {
    if (dlssg::active()) {
        *fg = PACE_FG_DLSSG, *mult = dlssg::multiplier();
        return;
    }
    const int m = framegen_multiplier();   // (0 off, -1 Auto, else the frames shown for each of the game's)
    *fg = m == 0 ? PACE_FG_OFF : m < 0 ? PACE_FG_AUTO : PACE_FG_FIXED;
    *mult = m > 0 ? m : 1;
}

/// The worker's thread: rtss_limit.cpp's poll every half second, until it says there is nothing to do in this run
void rtss_worker() {
    while (rtss::poll()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

/// Whether RivaTuner holds this frame (the game's presenting thread, where the frame starts). Tells the worker what is
/// wanted, which costs two atomic stores; everything of RTSS's is the worker's
bool rtss_frame(int64_t period, int fg, int mult, bool movie) {
    const int mode = rtss_mode();
    if (mode < 0) return false;
    if (mode == 0) {   // (kept out entirely: a line a run that did not close left in the game's own state file is said, once)
        if (!begun.load(std::memory_order_relaxed) && !begun.exchange(1)) {
            logf("vk: pace: HOS_RTSS=0: RivaTuner Statistics Server is kept out; the game's own wait limits the game");
            rtss::start(rtss::real_sys());
            std::thread(rtss::report_left).detach();
        }
        return false;
    }
    const int64_t want = pace_rtss_want((uint64_t)period, fg, mult, dlssg_try());
    if (want == RTSS_UNSET) return false;   // (no frame paced yet)
    if (!begun.load(std::memory_order_relaxed) && !begun.exchange(1)) {
        rtss::start(rtss::real_sys());
        std::thread(rtss_worker).detach();
    }
    rtss::want(want);
    rtss::movie(movie && want > 0 ? want : 0);
    const uint64_t shown = hosted.load(std::memory_order_acquire), now = (uint64_t)host->now_ns();   // (in this order)
    // (a movie is the game's own wait's while RTSS holds the game's limit, as NX2navi's pacer holds its movies; a limit of
    // the player's own holds the movie as it holds play, whatever the game does)
    bool holds = want > 0 && (!movie || rtss::own()) && rtss_frame_holds(rtss::active(), now, shown, start_before) != 0;
    start_before = start_last, start_last = now;
    if (rtss_watch_step(&watch, now, holds, (uint64_t)period)) {
        // (nothing held the frames RTSS was to hold: this one is the game's own wait's, and the worker gives RTSS up
        // until it is tried again)
        rtss::fast();
        holds = false;
    }
    return holds;
}

} // namespace

void set_period_ns(int ns) { period_ns.store(ns > 0 ? ns : 0, std::memory_order_relaxed); }
void set_movie(int playing) { movie_on.store(playing != 0, std::memory_order_relaxed); }
void set_steady(int on) { steady_on.store(on == 1, std::memory_order_relaxed); }
int steady_held_us() {
    presenting = true;
    return (int)(steady_held_ns.exchange(0, std::memory_order_relaxed) / 1000);
}
int64_t steady_floor_ns() {
    // (only while the game's own wait paces the frames: RivaTuner and Reflex hold theirs to the cap, and the host's steady
    // frame rate stands aside for them)
    if (!steady_on.load(std::memory_order_relaxed) || owner() != GAME) return 0;
    return period_ns.load(std::memory_order_relaxed);
}
void steady_held(int64_t ns) {
    if (presenting && ns > 0) steady_held_ns.fetch_add(ns, std::memory_order_relaxed);
}

int frame_start() {
    presenting = true;   // ((Triforce) steady_held: the game's presenting thread)
    const int64_t period = period_ns.load(std::memory_order_relaxed);
    int fg, mult;
    presents(&fg, &mult);
    const bool rtss = rtss_frame(period, fg, mult, movie_on.load(std::memory_order_relaxed) != 0);
    // (with RivaTuner as the limiter Reflex is asked with no period: it holds no limit, and takes back one it held)
    // ((Triforce) RX_VK_REFLEX_LIMIT=1: without DLSS frame generation, the renderer's own Reflex, a movie left to the
    // game's own wait as NX2navi leaves it to its pacer)
    const bool movie = movie_on.load(std::memory_order_relaxed) != 0;
    const bool reflex = dlssg::pace(rtss ? 0 : period) || (!dlssg::active() && reflex_pace(rtss || movie ? 0 : period));
    const int who = pace_owner(rtss, reflex);
    owner_now.store(who, std::memory_order_relaxed);
    yields.store(pace_display_yields(who, fg), std::memory_order_relaxed);
    fg_now.store(fg, std::memory_order_relaxed), mult_now.store(mult, std::memory_order_relaxed);
    return who != PACE_GAME;
}

void presented() {
    if (rtss_mode() > 0) hosted.store((uint64_t)host->now_ns(), std::memory_order_release);
}
uint64_t presented_ns() { return hosted.load(std::memory_order_acquire); }

void exiting() {
    // (an exit before the first paced frame starts no worker after it either; restore() closes the profile to every later
    // want, and touches nothing of RTSS's where the worker never found it)
    begun.store(1);
    if (rtss_mode() > 0) rtss::restore();
}

int owner() { return owner_now.load(std::memory_order_relaxed); }
bool display_yields() { return yields.load(std::memory_order_relaxed) != 0; }

void limiter_text(char* out, size_t n) {
    const int who = owner(), mode = rtss_mode();
    if (who == REFLEX) {
        snprintf(out, n, "NVIDIA Reflex");
        return;
    }
    const rtss::View v = mode > 0 ? rtss::view() : rtss::View{};
    if (who == RTSS) {   // (RivaTuner counts the frames shown: with frame generation the cap times the frames shown for each)
        const int mult = mult_now.load(std::memory_order_relaxed);
        char shown[48] = "";
        if (mult > 1) snprintf(shown, sizeof shown, " shown (%.0f x %d)", v.want / 1000.0 / mult, mult);
        if (v.limiter == rtss::RTSS_OWN_LIMIT)
            snprintf(out, n, "RivaTuner, at your own limit of %.0f%s", v.mhz / 1000.0, mult > 1 ? " shown" : "");
        else snprintf(out, n, "RivaTuner, at %.0f%s", v.want / 1000.0, shown);
        return;
    }
    const char* why = mode == 0                               ? " (HOS_RTSS=0)"
                      : mode < 0 || !v.looked                 ? ""
                      : !v.installed                          ? " (RivaTuner is not installed)"
                      : v.want == RTSS_OUT_AUTO               ? " (frame generation on Auto)"
                      : v.want == RTSS_OUT_DLSSG              ? " (DLSS frame generation)"
                      : movie_on.load()                       ? " (a movie plays)"
                      : !v.running                            ? " (RivaTuner is not running: start it and it limits the game)"
                      : v.fail == RTSS_FAIL_READONLY          ? " (run as administrator, or set the limit in RivaTuner)"
                      : v.fail == RTSS_FAIL_LIBRARY           ? " (RivaTuner's library did not load)"
                      : rtss_fail_retried(v.fail)             ? " (RivaTuner did not hold the frames: tried again later)"
                      : !v.listed                             ? " (RivaTuner has not hooked the game yet)"
                      : v.why == RTSS_WHY_IDLE                ? " (no picture is shown)"
                                                              : " (RivaTuner is taking over)";
    const int64_t steady = steady_floor_ns();   // ((Triforce) the Steady frame rate row: the rate it holds now)
    if (steady > 0) snprintf(out, n, "the game's own wait, steady at %.0f fps%s", 1e9 / (double)steady, why);
    else snprintf(out, n, "the game's own wait%s", why);
}

} // namespace mkwvk::pace
