// (Triforce, the third performance audit's P6, after NX2navi's replay workers: runtime/vk_exec.c rw_main's idle spin and the
// submitting thread's spin for its chunks, epicmeme546, 3010a637, 7534193b, 7eee4817, ffa599ef) A bounded spin before a sleep
// on a condition variable, for the parallel replay (vk_replay.cpp): a helper thread with no segment to record spins for the
// next before it sleeps (par_worker_main), and the replay thread spins for a segment a helper records before it sleeps for it
// (replay_handle). What each waits for is published where the spin can read it without the lock (ParPool::queued, Job::done),
// and the condition variable's wait with its predicate follows the spin as before: the spin only makes the wait shorter when
// what it waits for comes within it, it never decides anything. The audit's trace: the helpers began a segment 0.262 ms after
// they were given it, and the replay thread waited 1.74 ms a frame for them (r22-trace). tests/test_helper_spin.py
//
//   RX_VK_HELPER_SPIN_US=<us>: the spin's bound (100, NX2navi's; at most 1000000); 0: no spin, straight to the wait as before
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdlib>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace helper_spin {

/// The bound in nanoseconds that RX_VK_HELPER_SPIN_US's text gives (null: unset)
inline int64_t bound_ns(const char* text) {
    if (!text || !*text) return 100000;
    const long long us = std::atoll(text);
    return us <= 0 ? 0 : us >= 1000000 ? 1000000000LL : us * 1000;
}
/// RX_VK_HELPER_SPIN_US, read once
inline int64_t spin_ns() {
    static const int64_t v = bound_ns(std::getenv("RX_VK_HELPER_SPIN_US"));
    return v;
}
inline void pause() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause();
#endif
}
/// Spins until done() holds or ns have passed; whether done() held. ns <= 0: done() once, no spin. *spent (when given): the
/// time spun
template <class Done>
bool until(Done&& done, int64_t ns, int64_t* spent = nullptr) {
    if (spent) *spent = 0;
    if (done()) return true;
    if (ns <= 0) return false;
    const auto t0 = std::chrono::steady_clock::now();
    const auto end = t0 + std::chrono::nanoseconds(ns);
    bool held = false;
    for (unsigned i = 1;; i++) {
        pause();
        if ((held = done())) break;
        if (!(i & 7) && std::chrono::steady_clock::now() >= end) {
            held = done();
            break;
        }
    }
    if (spent) *spent = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    return held;
}

}  // namespace helper_spin
