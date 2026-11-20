// (Triforce) A submission whose command buffer begins with a wait for an event the game signals from the CPU: the submit waits for
// the signal (RX_VK_GATE_WAIT_MS at most) before its capture and its replay, after NX2navi's gate_wait_ms (runtime/vk_exec.c
// vk_exec_gate_wait and gate_first_wait).
//
// OoT's queues order some of their work with events the CPU signals once it has written what the work reads. A submission that
// meets such a wait unmet is taken (its capture of the game's memory at the submit) and replayed up to the wait, where it stops; once
// the CPU signals, the order thread (vk_replay.cpp sync_watch, a millisecond at a time) or the next submit or present lets it go on,
// with a second capture (resume_queue, RX_VK_RESUME_CAPTURE). NX2navi found the signal 1 to 2 ms after the submit call in about
// every other frame in Hyrule Field, and made the submit call wait for it, before its copies, as the console's GPU waits at the
// wait itself: the capture is then taken after the game has written the frame's data, and the frame is replayed in the submit call
// and on the submitting thread (54.5 fps instead of 53 there, the longest frame 33 ms instead of 46). Triforce meets it far less
// often (the "captures at a CPU event's wait" of the order line: 100 to 170 in a lab run of 26,000 frames at the Market), and a gate
// that runs out holds the submitting thread for the whole bound, so it is off unless asked for, for the lab to measure.
//   The gate waits only where the replay would stop: the first op of the submission's first command buffer that is not state the
//   replay only stores (bound programs, buffers, textures, targets, viewports...) is a wait for an event, the wait is not met in the
//   replay's order now, and the CPU has signalled that event before (vk_replay.cpp cpu_events). It takes no lock while it waits (the
//   thread that will signal may need the order lock first), and looks at the event's word in the game's memory, which the CPU's signal
//   writes. It ends at the signal, or at the bound, and the submission then goes as before. After kOuts waits that ran out, once they
//   are more than a tenth of the waits, it is off for the run (the signal is then one that comes only after the submitting thread goes
//   on, and every wait would be a stall).
//   RX_VK_GATE_WAIT_MS unset or 0: off, as before; 1 to 100: the bound in milliseconds (NX2navi's default is 10)
// Apart from Vulkan and the stream's op numbers so that tests/test_gate_wait.py can run the rules and the gate over authored streams.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace mkwvk::gate_wait {

constexpr int kMaxOps = 256;   // (the ops looked at before the wait, at most: NX2navi's)
constexpr int64_t kOuts = 4;   // (waits that ran out before the gate may go off for the run)

/// RX_VK_GATE_WAIT_MS: the bound in milliseconds, 0 (off) to 100; unset or not a number: 0
inline int ms(const char* env) {
    if (!env || !*env) return 0;
    const int v = std::atoi(env);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

/// The command buffer's first op that is not state (records {uint32 op, uint32 size}, sizes 8-aligned, from p): the wait it begins
/// with, or null. kind(op): 1 state the replay only stores (looked past), 2 a wait for an event, 0 anything else (a draw, a copy, a
/// signal, a barrier, the end: the gate does not wait)
template <class K>
const uint8_t* first_wait(const uint8_t* p, K kind) {
    for (int k = 0; k < kMaxOps; k++) {
        uint32_t h[2];
        std::memcpy(h, p, sizeof h);
        if (h[1] < 8 || (h[1] & 7)) return nullptr;
        const int what = kind(h[0]);
        if (what == 2) return p;
        if (what != 1) return nullptr;
        p += h[1];
    }
    return nullptr;
}

/// Whether the gate goes off for the run, after `gates` waits of which `outs` ran out
inline bool off_after(int64_t gates, int64_t outs) {
    return outs >= kOuts && outs * 10 > gates;
}

/// A pause of the host core while it looks (the waiting thread holds no virtual core: the signaller runs)
inline void pause() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause();
#endif
}

} // namespace mkwvk::gate_wait
