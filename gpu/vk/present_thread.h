// (Triforce) The present thread (vk_replay.cpp present_image, RX_VK_PRESENT_THREAD): the last step of a present of the
// game's handed to a thread of its own, apart from Vulkan so that tests/test_present_thread.py can run it.
//
// A frame of OoT here is one chain on the game's presenting thread: its build, the replay, its poll for the frame's GPU
// event, then the present. present_image's last step, the pacing's wait for an early picture (present_smooth.h, up to 8 ms)
// and vkQueuePresentKHR (where the driver, or an overlay's frame limiter, may block), held that thread and with it the start
// of the next frame. Now the presenting thread records the blit and submits it as before (the frame fence, the swapchain's
// image, the overlays, the submit: on the queue in the same order, so the same picture), hands the rest over and goes on;
// whoever presents next first waits until the one handed over is out (settle), so one present is in hand at most and the
// presents go out in their order. NX2navi does the same with a thread of its own (runtime/vk_backend.c present_main, most of
// whose lines are not chrg6's: its behaviour only, none of its code); there the blit is the thread's as well, and every later
// submission waits for it. Here the blit stays where it was, so nothing else of the replay has to wait for the thread.
//
// Not for frame generation's display thread nor NVIDIA DLSS frame generation (presents of their own), nor with AMD
// Anti-Lag 2 (its present stage would come after the next frame's input stage); NVIDIA Reflex carries the frame's id with
// its submit's end (vk_reflex.cpp shown_id), so its present's markers are that frame's on either thread.
#pragma once

#include <stdint.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

/// Whether a present goes out from the present thread. on: RX_VK_PRESENT_THREAD (on unless =0); games: one of the game's
/// own pictures (do_present, smooth_mine), not the start-up screen's nor frame generation's display thread's; framegen,
/// dlssg: frame generation or NVIDIA DLSS frame generation runs; antilag: AMD Anti-Lag 2 gives its stages
static inline int present_thread_use(int on, int games, int framegen, int dlssg, int antilag) {
    return on && games && !framegen && !dlssg && !antilag;
}

namespace present_thread {

/// One present in hand at most. The presenting thread hands one over (hand) and goes on; the present thread takes it
/// (take), presents and says so (done); whoever presents next waits first until it is out (settle)
template <class Job>
struct Handoff {
    std::mutex mu;
    std::condition_variable cv;
    Job job{};
    bool pending = false;   // (handed over, not taken yet)
    bool busy = false;      // (taken, not out yet)
    int64_t handed = 0, taken = 0;

    /// Before a present: until the one handed over before it is out. The time waited in ns; -1 when none was in hand
    int64_t settle() {
        std::unique_lock<std::mutex> lk(mu);
        if (!pending && !busy) return -1;
        const auto t0 = std::chrono::steady_clock::now();
        cv.wait(lk, [this] { return !pending && !busy; });
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }
    /// A present for the thread; returns at once unless one is still in hand (the caller settled first: never)
    void hand(const Job& j) {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return !pending && !busy; });
        job = j, pending = true, handed++;
        cv.notify_all();
    }
    /// The present thread's: the next one handed over
    Job take() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return pending; });
        pending = false, busy = true, taken++;
        return job;
    }
    /// The present thread's: the one taken is out
    void done() {
        std::lock_guard<std::mutex> lk(mu);
        busy = false;
        cv.notify_all();
    }
};

} // namespace present_thread
