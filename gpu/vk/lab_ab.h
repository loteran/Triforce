/* (Triforce, lab) In-run A/B. Two runs of one build differ by more than most changes are worth (the other programs on the GPU,
   which cores the threads landed on, the scene's state), so a change's effect is measured inside ONE run: the frames go in
   phases of RX_VK_LAB_AB_FRAMES presents (300): none, knob 1, none, knob 2, ... and around again, and the frame trace
   (RX_VK_FRAME_TRACE, vk_replay.cpp status_tick) carries the phase's knob in its last column (0: none, k: the k-th knob;
   negative for a phase's first RX_VK_LAB_AB_SKIP frames (30), which a reader leaves out). perf\lowend\lab_ab.py compares each
   knob's phases with the none-phases on both sides of them.

   RX_VK_LAB_AB=<name>[:<value>],<name>[:<value>],...   (at most 16)
   Knobs joined by '+' are one knob that turns each of them on ("reclast+tlrec+rbsettle").
   A knob named d_<site> is a delay: a busy wait of <value> microseconds at that site, at most once a frame (delay(), below):
   what the frame's time answers to a part of it taking longer, which is whether that part is on the frame's critical path
   (1 ms in for 1 ms of delay) or has slack (nothing). Any other name is a switch the code reads with on("<name>").
   Not for players: nothing reads these without the variable. */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mkwvk::lab_ab {

struct Knob {
    char name[64];
    int64_t value;
};
inline Knob knobs[16];
inline int n_knobs = -1;                    // (-1: the variable not read yet)
inline std::atomic<int> active{0};          // (0: none; k: knobs[k - 1])
inline std::atomic<uint64_t> frame_no{0};   // (presents counted here)
inline std::atomic<uint64_t> fired[16];     // (the frame each delay last waited in)
inline std::atomic<uint32_t> pres_thread{0};   // (the thread that presents: the game's Presentation thread, in the plain mode)
inline int64_t phase_frames = 300, skip_frames = 30;

inline void init() {
    if (n_knobs >= 0) return;
    int n = 0;
    const char* e = getenv("RX_VK_LAB_AB");
    while (e && *e && n < 16) {
        const char* end = strchr(e, ',');
        const size_t len = end ? (size_t)(end - e) : strlen(e);
        char item[96] = {};
        memcpy(item, e, len < sizeof item - 1 ? len : sizeof item - 1);
        char* colon = strchr(item, ':');
        knobs[n].value = colon ? atoll(colon + 1) : 1;
        if (colon) *colon = 0;
        strncpy(knobs[n].name, item, sizeof knobs[n].name - 1);
        if (knobs[n].name[0]) n++;
        e = end ? end + 1 : nullptr;
    }
    if (const char* f = getenv("RX_VK_LAB_AB_FRAMES")) phase_frames = atoll(f) > 0 ? atoll(f) : 300;
    if (const char* s = getenv("RX_VK_LAB_AB_SKIP")) skip_frames = atoll(s);
    n_knobs = n;
}
/// Once a present, on the presenting thread: the phase this frame belongs to (the trace's column)
inline int phase_tick() {
    init();
    if (!n_knobs) return 0;
    const uint64_t f = frame_no.fetch_add(1) + 1;
    const uint64_t phase = f / (uint64_t)phase_frames;        // (0 none, 1 knob 1, 2 none, 3 knob 2, ...)
    const int k = (phase & 1) ? (int)((phase / 2) % (uint64_t)n_knobs) + 1 : 0;
    active.store(k, std::memory_order_relaxed);
    const bool settling = (int64_t)(f % (uint64_t)phase_frames) < skip_frames;
    return settling ? -(k + 1) : k;   // (-1: none, settling; -(k + 1): knob k, settling)
}
/// Is this switch on in this phase: the phase's knob is it, or names it among several joined by '+' ("reclast+tlrec":
/// both on in that knob's phases, so a combination is one knob)
inline bool on(const char* name) {
    if (n_knobs <= 0) return false;
    const int k = active.load(std::memory_order_relaxed);
    if (k <= 0) return false;
    const size_t len = strlen(name);
    for (const char* p = knobs[k - 1].name; p && *p;) {
        const char* plus = strchr(p, '+');
        const size_t n = plus ? (size_t)(plus - p) : strlen(p);
        if (n == len && !strncmp(p, name, len)) return true;
        p = plus ? plus + 1 : nullptr;
    }
    return false;
}
inline int64_t value_of(const char* name, int64_t otherwise) {
    if (n_knobs <= 0) return otherwise;
    const int k = active.load(std::memory_order_relaxed);
    return k > 0 && !strcmp(knobs[k - 1].name, name) ? knobs[k - 1].value : otherwise;
}
/// The delay site <site> (knob "d_<site>"): a busy wait of its microseconds, once a frame
inline void delay(const char* site) {
    if (n_knobs <= 0) return;
    const int k = active.load(std::memory_order_relaxed);
    if (k <= 0 || knobs[k - 1].name[0] != 'd' || knobs[k - 1].name[1] != '_' || strcmp(knobs[k - 1].name + 2, site)) return;
    const uint64_t f = frame_no.load(std::memory_order_relaxed);
    if (fired[k - 1].exchange(f, std::memory_order_relaxed) == f) return;
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(knobs[k - 1].value);
    while (std::chrono::steady_clock::now() < until) {
    }
}

}   // namespace mkwvk::lab_ab
