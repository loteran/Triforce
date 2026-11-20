// (Triforce) The priority of the thread that replays the game's frames while it records them, after NX2navi's recorder_prio
// (runtime/vk_exec.c rec_claim, chrg6's lines). NX2navi keeps the thread that records at the highest scheduling priority, so that the
// scheduler prefers it over the game's other threads: preempted in the middle of a part, the submission came late for its frame
// (Kokiri Forest: 274 of 2,737 frames had a late submit). Here the replay runs on the game's own submitting thread (RX_VK_ASYNC off),
// at the game's priority (normal), pinned while it replays to the core kept for it (vk_replay.cpp ReplayPin, hos_reserved_cpus). That
// core is kept from the game's threads only: the renderer's helper threads and the capture's copy helpers (above normal), the shader
// workers and the driver's threads may all be put there, and the first two outrank a replay at normal priority. So a thread is raised
// for as long as it replays (the outermost ReplayPin: a submit's capture and replay, a present, a stopped queue let on by the order
// thread), and given its own priority back after: the game's code it runs between two replays keeps the game's priority. The replay
// thread of RX_VK_ASYNC=1 and the recorder thread of RX_VK_RECORDER=thread, which do nothing else, take the level for good (above
// normal at least, as they had).
//   RX_VK_REPLAY_PRIO unset (or 2): highest (THREAD_PRIORITY_HIGHEST); 1: above normal; 0: the thread's priority left alone, as before
// Apart from Windows so that tests/test_replay_prio.py can run the rule and vk_replay.cpp's ReplayPin over stand-ins.
#pragma once

#include <cstdlib>

namespace mkwvk::replay_prio {

constexpr int kAbove = 1, kHighest = 2;   // (Windows' THREAD_PRIORITY_ABOVE_NORMAL and THREAD_PRIORITY_HIGHEST)

/// The level a replay raises its thread to, from RX_VK_REPLAY_PRIO: 0 none (as before), kAbove, kHighest
inline int level(const char* env) {
    if (!env || !*env) return kHighest;
    const int v = std::atoi(env);
    return v <= 0 ? 0 : v == 1 ? kAbove : kHighest;
}
/// Whether a replay raises its thread, whose priority is `now`, to `want`: only ever up (a thread above it, a time-critical one, or
/// one Windows could not tell, THREAD_PRIORITY_ERROR_RETURN, is left alone; nothing is given back for it then)
inline bool raise(int want, int now) {
    return want > 0 && now < want;
}
/// The level of a thread of the renderer's own that only records (the replay thread of RX_VK_ASYNC=1, the recorder thread): `want`,
/// and above normal at least, as before
inline int own_thread(int want) {
    return want > kAbove ? want : kAbove;
}

} // namespace mkwvk::replay_prio
