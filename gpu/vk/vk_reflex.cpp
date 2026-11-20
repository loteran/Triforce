// (hookshot) NVIDIA Reflex through VK_NV_low_latency2, without NVIDIA's Streamline (vk_reflex.h).
//
// While the Insert menu's NVIDIA Reflex row (Off / On / On + Boost; RX_VK_REFLEX=0 / 1 / 2 over it) is on, the swapchain is
// made with latency mode on (VkSwapchainLatencyCreateInfoNV) and its sleep mode set from the row; turning the row on or off
// makes the swapchain again (reflex_swapchain_stale, at the next present). Off is the game as without Reflex: no latency
// mode, no markers, no sleep, no present ids, nothing logged. The device's extensions are enabled whenever it has them, so
// the row needs no restart: enabled alone they do nothing, nothing of them is used until the row is on. Each frame the driver gets markers with the
// present's id: the game's simulation start (and input) once the sleep returns, its end and the rendering's submission start
// at the game's present (do_present), the submission's end before the present's submit, and the present's start and end
// around vkQueuePresentKHR. vkLatencySleepNV after the present waits until the driver wants the next frame to start: with
// sleep mode off it returns at once.
//
// What the markers stand for is approximate: OoT simulates on its main thread and renders on its presentation thread, and the
// replay records the GPU work as the game submits it; the sleep holds the presentation thread, which the main thread waits for.
// With frame generation on, presents come from the display thread: Reflex is not used then.
//
// RX_VK_REFLEX_LOG=1: every 300 frames with Reflex on, the driver's own timings (vkGetLatencyTimingsNV) averaged in the log:
// simulation start to present end, and to the GPU's end of rendering.
//
// (Triforce, after NX2navi's runtime/vk_lowlat.c, chrg6's: the driver holds the frame limit) RX_VK_REFLEX_LIMIT=1: with
// Reflex on and no frame generation, its sleep also holds the Game frame rate cap (the sleep mode's minimum interval) and the
// game's own wait (host/nn_vi.c hos_vi_native_acquire) steps aside, as the Reflex frame limit row does with DLSS frame
// generation (vk_dlssg.cpp pace): one wait a frame, at its start, timed by the driver, which knows the GPU's time and the
// last present. As there, the sleep that holds a frame has to be where the frame starts, so while Reflex holds the cap it is
// taken on the game's thread at the acquire (reflex_pace, asked by the host through HOS_OPT_PACE_START: vk_pace.cpp) and
// the one after the present is left out. Who holds a frame is dlssg_pace.h's rule with one frame shown for each (NX2navi's
// vk_lowlat_pace): Reflex only while the driver has the very interval asked for, a present went out within 200 ms, no
// movie plays, RivaTuner does not hold the game, and the frames have not shown that the sleep does not hold them (over half
// of 60 sooner than three quarters of the period: the game's own wait for the rest of the run); the game's own wait in every
// other case, from that frame on. Unset: no interval, the sleep after the present, the game's own wait, as before.
#include "vk_reflex.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "dlssg_pace.h"   // ((Triforce) who holds the cap, Reflex or the game's own wait: reflex_pace)
#include "vk_dlssg.h"
#include "lab_ab.h"   // (Triforce, lab: RX_VK_LAB_AB "noreflex")
#include "vk_framegen.h"
#include "vk_menu.h"
#include "vk_work.h"   // (last: RX_VK_RECORDER=defer drains before the latency-sleep wait; =thread does not need to, the driver signals it)

namespace mkwvk {

namespace {

struct Reflex {
    bool have_ext = false, ready = false;
    bool latency_next = false;           // (the swapchain being made has latency mode on: reflex_swapchain_info)
    bool latency = false;                // (the current one has)
    VkPhysicalDevicePresentIdFeaturesKHR present_id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkSwapchainLatencyCreateInfoNV latency_info{VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV};
    PFN_vkSetLatencySleepModeNV set_mode = nullptr;
    PFN_vkLatencySleepNV sleep = nullptr;
    PFN_vkSetLatencyMarkerNV marker = nullptr;
    PFN_vkGetLatencyTimingsNV timings = nullptr;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint64_t swapchain_generation = 0;   // (a sleep may finish after this chain was replaced)
    VkSemaphore wake = VK_NULL_HANDLE;   // (timeline: the sleep's end)
    uint64_t wake_value = 0;
    uint64_t id = 1;                     // (the frame's present id)
    int mode = -1;                       // (as set on the swapchain)
    bool frame_marked = false;           // (this frame's start markers given: its other markers count)
    // ((Triforce) the frame whose present comes next, as its submit ended (reflex_submit_ends): its present's id and markers.
    // With the present thread (vk_replay.cpp, RX_VK_PRESENT_THREAD) the present goes out after the game's thread has slept into
    // its next frame, whose id the sleep counted on)
    uint64_t shown_id = 0;
    bool shown_marked = false;
    VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    std::mutex mu;
    // ((Triforce) RX_VK_REFLEX_LIMIT: reflex_pace)
    uint32_t interval_us = 0;                 // (mu: the minimum interval as set on the swapchain with the mode)
    bool interval_refused = false;            // (mu: the driver refused a mode with one: none is asked again)
    std::atomic<uint32_t> want_us{0};         // (the interval the game's thread asked for, set at the next sleep)
    std::atomic<bool> game_sleeps{false};     // (the game's thread takes the sleep where its frame starts)
    std::atomic<bool> slept_after{false};     // (the sleep after a present was taken since the last frame's start)
    std::atomic<int64_t> hosted_ns{0};        // (the host's clock after the newest present with Reflex on, 0 before)
    DlssgPace pace{};                         // (the game's thread's)
    int pace_said = -1;                       // (the game's thread's: who held the cap, as last logged)
};
Reflex& rx() {
    static Reflex r;
    return r;
}

int wanted_mode() {
    static const char* e = getenv("RX_VK_REFLEX");
    // (Triforce: with NVIDIA DLSS frame generation Reflex is Streamline's, vk_dlssg.cpp: this one stays off)
    if (dlssg::early()) return 0;
    return std::clamp(e && *e ? atoi(e) : opts.reflex, 0, 2);
}

/// With r.mu held: the row on, the extension and its feature, a swapchain made with latency mode, and no frame generation (its
/// own display thread)
bool on() {
    Reflex& r = rx();
    return r.ready && r.swapchain && r.latency && wanted_mode() > 0 && !framegen_on();
}

void mark(VkLatencyMarkerNV m) {
    Reflex& r = rx();
    VkSetLatencyMarkerInfoNV i{VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV};
    i.presentID = r.id, i.marker = m;
    r.marker(dev.device, r.swapchain, &i);
}
/// ((Triforce) a marker of the frame whose present comes next: shown_id)
void mark_shown(VkLatencyMarkerNV m) {
    Reflex& r = rx();
    VkSetLatencyMarkerInfoNV i{VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV};
    i.presentID = r.shown_id, i.marker = m;
    r.marker(dev.device, r.swapchain, &i);
}

void apply_mode() {
    Reflex& r = rx();
    const int want = wanted_mode();
    // ((Triforce) and the frame limit the game's thread asked for, the minimum interval: reflex_pace; none unless asked)
    uint32_t us = want > 0 && !r.interval_refused ? r.want_us.load(std::memory_order_acquire) : 0;
    if (want == r.mode && us == r.interval_us) return;
    VkLatencySleepModeInfoNV m{VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
    m.lowLatencyMode = want >= 1, m.lowLatencyBoost = want == 2, m.minimumIntervalUs = us;
    VkResult res = r.set_mode(dev.device, r.swapchain, &m);
    if (res != VK_SUCCESS && us) {   // ((Triforce) the interval refused: the mode as before, and the game's own wait holds the cap)
        r.interval_refused = true, us = 0, m.minimumIntervalUs = 0;
        logf("vk: NVIDIA Reflex: the driver refused a frame limit in the sleep mode (%d): the game's own wait holds the cap", (int)res);
        res = r.set_mode(dev.device, r.swapchain, &m);
    }
    if (want != r.mode)
        logf("vk: NVIDIA Reflex: %s%s", want == 0 ? "off" : want == 1 ? "on" : "on + boost",
             res == VK_SUCCESS ? "" : " (the driver refused the sleep mode)");
    r.mode = want;
    r.interval_us = res == VK_SUCCESS ? us : 0;
}

void log_timings() {
    static const bool wanted = getenv("RX_VK_REFLEX_LOG") && atoi(getenv("RX_VK_REFLEX_LOG")) > 0;
    Reflex& r = rx();
    if (!wanted || !r.timings || r.id % 300) return;
    VkGetLatencyMarkerInfoNV g{VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV};
    r.timings(dev.device, r.swapchain, &g);
    if (!g.timingCount) return;
    std::vector<VkLatencyTimingsFrameReportNV> t(g.timingCount, {VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV});
    g.pTimings = t.data();
    r.timings(dev.device, r.swapchain, &g);
    double sim_present = 0, gpu_end = 0, sim = 0;
    int n = 0;
    for (uint32_t i = 0; i < g.timingCount; i++) {
        const auto& f = t[i];
        if (!f.simStartTimeUs || !f.presentEndTimeUs || f.presentEndTimeUs < f.simStartTimeUs) continue;
        sim_present += (double)(f.presentEndTimeUs - f.simStartTimeUs);
        sim += f.simEndTimeUs > f.simStartTimeUs ? (double)(f.simEndTimeUs - f.simStartTimeUs) : 0.0;
        gpu_end += f.gpuRenderEndTimeUs > f.simStartTimeUs ? (double)(f.gpuRenderEndTimeUs - f.simStartTimeUs) : 0.0;
        n++;
    }
    if (n)
        logf("vk: NVIDIA Reflex (%s): over %d frames, simulation start to present end %.1f ms, to the GPU's end %.1f ms, "
             "simulation %.1f ms", r.mode == 0 ? "off" : r.mode == 1 ? "on" : "on + boost", n, sim_present / n / 1000.0,
             gpu_end / n / 1000.0, sim / n / 1000.0);
}

}  // namespace

void reflex_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    static const char* e = getenv("RX_VK_REFLEX");
    if (e && !strcmp(e, "-")) return;   // (RX_VK_REFLEX=-: the extensions not even enabled)
    bool ll = false, pid = false;
    for (const auto& x : all) {
        ll |= !strcmp(x.extensionName, VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
        pid |= !strcmp(x.extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME);
    }
    if (!ll || !pid) return;
    for (const char* n : {VK_NV_LOW_LATENCY_2_EXTENSION_NAME, VK_KHR_PRESENT_ID_EXTENSION_NAME}) {
        bool listed = false;
        for (const char* w : want) listed |= !strcmp(w, n);
        if (!listed) want.push_back(n);
    }
    rx().have_ext = true;
}

void reflex_device_features(void**& tail) {
    Reflex& r = rx();
    if (!r.have_ext) return;
    *tail = &r.present_id, tail = &r.present_id.pNext;
}

void reflex_device_created() {
    Reflex& r = rx();
    if (!r.have_ext) return;
    if (!r.present_id.presentId) {
        logf("vk: NVIDIA Reflex: the device has no present ids: off");
        return;
    }
    r.set_mode = (PFN_vkSetLatencySleepModeNV)vkGetDeviceProcAddr(dev.device, "vkSetLatencySleepModeNV");
    r.sleep = (PFN_vkLatencySleepNV)vkGetDeviceProcAddr(dev.device, "vkLatencySleepNV");
    r.marker = (PFN_vkSetLatencyMarkerNV)vkGetDeviceProcAddr(dev.device, "vkSetLatencyMarkerNV");
    r.timings = (PFN_vkGetLatencyTimingsNV)vkGetDeviceProcAddr(dev.device, "vkGetLatencyTimingsNV");
    VkSemaphoreTypeCreateInfo tc{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tc.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sc.pNext = &tc;
    r.ready = r.set_mode && r.sleep && r.marker && vkCreateSemaphore(dev.device, &sc, nullptr, &r.wake) == VK_SUCCESS;
    logf("vk: NVIDIA Reflex: %s", r.ready ? "available (VK_NV_low_latency2)" : "its functions are missing: off");
}

bool reflex_available() { return rx().ready; }

void reflex_swapchain_info(VkSwapchainCreateInfoKHR& ci) {
    Reflex& r = rx();
    r.latency_next = r.ready && wanted_mode() > 0;
    if (!r.latency_next) return;   // (Off: the swapchain as without Reflex)
    r.latency_info.pNext = ci.pNext;
    r.latency_info.latencyModeEnable = VK_TRUE;
    ci.pNext = &r.latency_info;
}

bool reflex_swapchain_stale() {
    Reflex& r = rx();
    std::lock_guard lk(r.mu);
    return r.ready && r.swapchain && r.latency != (wanted_mode() > 0);
}

void reflex_swapchain_created(VkSwapchainKHR swapchain) {
    Reflex& r = rx();
    if (!r.ready) return;
    std::lock_guard lk(r.mu);
    const bool was = r.latency;
    ++r.swapchain_generation;
    r.swapchain = swapchain, r.mode = -1, r.frame_marked = false, r.shown_marked = false, r.latency = r.latency_next;
    r.interval_us = 0;   // ((Triforce) a new swapchain has no frame limit yet: apply_mode sets the one asked for)
    if (r.latency) apply_mode();
    else if (was) logf("vk: NVIDIA Reflex: off");
}

void reflex_swapchain_destroyed() {
    Reflex& r = rx();
    std::lock_guard lk(r.mu);
    ++r.swapchain_generation;
    r.swapchain = VK_NULL_HANDLE;
    r.latency = false;
    r.frame_marked = false, r.shown_marked = false;
    r.mode = -1;
}

void reflex_present_begins() {
    Reflex& r = rx();
    std::lock_guard lk(r.mu);
    if (!on() || !r.frame_marked) return;
    mark(VK_LATENCY_MARKER_SIMULATION_END_NV);
    mark(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
}

void reflex_submit_ends() {
    Reflex& r = rx();
    std::lock_guard lk(r.mu);
    r.shown_marked = false;
    if (!on()) return;
    r.shown_id = r.id, r.shown_marked = r.frame_marked;   // ((Triforce) the present's, whichever thread gives it)
    if (r.frame_marked) mark(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV);
}

const void* reflex_present_id(const void* next) {
    Reflex& r = rx();
    std::lock_guard lk(r.mu);
    if (!on() || !r.shown_marked) return next;
    mark_shown(VK_LATENCY_MARKER_PRESENT_START_NV);
    r.pid.pNext = next, r.pid.swapchainCount = 1, r.pid.pPresentIds = &r.shown_id;
    return &r.pid;
}

void reflex_presented() {
    Reflex& r = rx();
    std::lock_guard lk(r.mu);
    if (!on()) return;
    if (r.shown_marked) mark_shown(VK_LATENCY_MARKER_PRESENT_END_NV);
    r.hosted_ns.store(host->now_ns(), std::memory_order_release);   // ((Triforce) for reflex_pace: a present went out lately)
}

namespace {
/// The driver's sleep until the next frame should start, then that frame's start markers: after the present
/// (reflex_sleep), or where the frame starts while Reflex holds the cap (reflex_pace). False when no sleep was taken
bool sleep_now() {
    Reflex& r = rx();
    uint64_t generation, value;
    {
        std::lock_guard lk(r.mu);
        if (!on()) return false;
        generation = r.swapchain_generation;
        apply_mode();   // (the menu's row may have changed)
        if (r.frame_marked) log_timings(), r.id++;
        VkLatencySleepInfoNV s{VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV};
        s.signalSemaphore = r.wake, s.value = ++r.wake_value;
        value = s.value;
        if (r.sleep(dev.device, r.swapchain, &s) != VK_SUCCESS) return false;
    }
    VkSemaphoreWaitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    w.semaphoreCount = 1, w.pSemaphores = &r.wake, w.pValues = &value;
    vkWaitSemaphores(dev.device, &w, 100000000ull);   // (100 ms at most: a driver that never wakes it does not stop the game)
    std::lock_guard lk(r.mu);
    if (!on() || generation != r.swapchain_generation || value != r.wake_value) return true;
    mark(VK_LATENCY_MARKER_SIMULATION_START_NV);
    mark(VK_LATENCY_MARKER_INPUT_SAMPLE_NV);
    r.frame_marked = true;
    return true;
}

/// (Triforce) Who holds the cap, as a log line when it or the reason changes (the first 40). why: the reason the game's own
/// wait holds it
void pace_say(int reflex, uint32_t us, const char* why) {
    Reflex& r = rx();
    static int lines;
    static const char* said_why;
    if ((r.pace_said == reflex && (reflex || why == said_why)) || lines >= 40) return;
    r.pace_said = reflex, said_why = why, lines++;
    if (reflex)
        logf("vk: NVIDIA Reflex: frame limit: its sleep holds the frame rate cap where each frame starts (%.3f ms, "
             "RX_VK_REFLEX_LIMIT); the game's own wait steps aside", us / 1000.0);
    else logf("vk: NVIDIA Reflex: frame limit: the game's own wait holds it (%s)", why);
}
} // namespace

void reflex_sleep() {
    // ((Triforce) while Reflex holds the Game frame rate cap the game's thread takes the sleep where its frame starts; a sleep
    // taken here is noted for the frame that starts next, reflex_pace)
    Reflex& r = rx();
    if (r.game_sleeps.load(std::memory_order_acquire)) return;
    if (lab_ab::on("noreflex")) return;   // (lab: the sleep after the present left out in these phases: what it costs the frame)
    if (sleep_now()) r.slept_after.store(true, std::memory_order_release);
}

bool reflex_pace(int64_t period_ns) {
    static const bool wanted = [] {
        const char* e = getenv("RX_VK_REFLEX_LIMIT");
        return e && atoi(e) != 0;
    }();
    if (!wanted) return false;
    Reflex& r = rx();
    bool live, refused;
    uint32_t applied;
    {
        std::lock_guard lk(r.mu);
        live = on(), refused = r.interval_refused;
        applied = r.mode > 0 ? r.interval_us : 0;
    }
    const int64_t hosted = r.hosted_ns.load(std::memory_order_acquire), now = host->now_ns();   // (in this order: now is not before hosted)
    uint32_t want = 0;
    int found = DLSSG_PACE_SAME;
    // (dlssg_pace.h's rule with one frame shown for each of the game's: Reflex while the driver has the very interval asked
    // for, a present went out lately and its frames have not shown that it does not hold them)
    bool held = dlssg_pace_step(&r.pace, (uint64_t)now, period_ns > 0 ? (uint64_t)period_ns : 0, 1, (uint64_t)std::max<int64_t>(hosted, 0),
                                live && !refused, applied, &want, &found) != 0;
    r.want_us.store(want, std::memory_order_release);   // (set with the sleep mode at the next sleep: apply_mode)
    if (found == DLSSG_PACE_FAST)
        logf("vk: NVIDIA Reflex: its sleep does not hold the frame rate cap (over half of %d frames came sooner than three "
             "quarters of %.2f ms): the game's own wait holds it for the rest of this run", DLSSG_PACE_WINDOW, period_ns / 1e6);
    // (the frame's sleep here, where it starts, in the game's own wait's place; but where the sleep after the present before
    // it was taken, which is so for the first frame Reflex holds, that one, with the interval set, held this frame)
    const bool after = r.slept_after.exchange(false, std::memory_order_acq_rel);
    if (held && !after && !sleep_now()) held = false;   // (none taken: the game's own wait)
    r.game_sleeps.store(held, std::memory_order_release);
    if (held) pace_say(1, want, nullptr);
    else if (!want)   // (an interval the driver has yet to take is no change: it is set at the next sleep)
        pace_say(0, 0, !live ? "Reflex is off, or frame generation presents the frames"
                       : refused ? "the driver refused the frame limit"
                       : r.pace.bad ? "Reflex's sleep did not hold the frames"
                       : period_ns <= 0 ? "a movie plays, or RivaTuner holds the game"
                       : hosted ? "no present for 200 ms"
                                : "no present yet");
    return held;
}

} // namespace mkwvk
