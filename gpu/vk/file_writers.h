// (Triforce, handoff §8) The renderer's file writes made on threads of their own, and the process's end. The window's close ends
// the game through ExitProcess (host/hos_relaunch.c), which stops every other thread wherever it is: a thread in the middle of
// a file (a bug capture's picture, the pipeline cache's save in play, the order of the pipelines, a hang report, the prepared
// record, the predicted pipelines' list, a debug picture or the frame log) left part of it. Now:
//   Writing   one write, from its open to its close or rename, as a scope: counted while it runs, and refused once the end has
//             begun (on any thread but the one ending the process: its own last saves go on)
//   stop      the process is about to end (HOS_OPT_EXITING, vk_replay.cpp option_set): no write begins from here on, and the
//             ones under way are waited for, up to a bound (2 s; RX_VK_WRITERS_WAIT_MS=<ms>, 0: no wait, as before). A write
//             that is still going then is cut by the exit as before; each such file is one write_whole makes, or one
//             whose readers take its whole records only, so it is the old file or the new one, or a list one record short
//   write_whole  (file_whole.h) a whole file written as <path>.part and put in the old one's place: the old file or the new
//             one, never part of one, whenever the process ends
// The threads themselves are not joined: they are detached and keep no state that outlives the process; what is waited for
// is their write, the only part of them an end can damage. The standard library alone (file_whole.h has the
// Windows part): tests/test_file_writers.py compiles it alone
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace file_writers {
inline std::atomic<bool> g_stopping{false};
inline std::atomic<int> g_busy{0};       // (writes under way on other threads than the ending one)
inline std::atomic<int> g_refused{0};    // (writes not begun because the end had)
inline thread_local bool t_ending = false;   // (the thread that called stop: its writes are the exit's own)
inline thread_local int t_inside = 0;        // (Writings this thread holds: one inside another is part of it, never refused)

/// One write: `file_writers::Writing w; if (!w) return;` before the file is opened, and the scope ends after its close (and
/// rename). The busy count goes up before the stop flag is read and stop sets the flag before it reads the count (both
/// sequentially consistent): a write either sees the stop and does not begin, or stop sees it and waits for it. One begun
/// inside another on the same thread (a save's own small files) goes on: the outer one is waited for whole
class Writing {
public:
    Writing() {
        if (t_ending || t_inside > 0) {
            on_ = true;
            t_inside++, nested_ = true;
            return;
        }
        g_busy.fetch_add(1);
        if (g_stopping.load()) {
            g_busy.fetch_sub(1);
            g_refused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        on_ = counted_ = true;
        t_inside++;
    }
    ~Writing() {
        if (counted_ || nested_) t_inside--;
        if (counted_) g_busy.fetch_sub(1);
    }
    Writing(const Writing&) = delete;
    Writing& operator=(const Writing&) = delete;
    explicit operator bool() const { return on_; }

private:
    bool on_ = false, counted_ = false, nested_ = false;
};

/// The end has begun: a loop that writes ends at its next turn
inline bool stopping() { return g_stopping.load(std::memory_order_relaxed); }

/// The bound of stop's wait: 2 s, or RX_VK_WRITERS_WAIT_MS (0: no wait at all, no write refused: as before this)
inline int exit_wait_ms() {
    const char* e = getenv("RX_VK_WRITERS_WAIT_MS");
    return e && *e ? std::max(0, atoi(e)) : 2000;
}

struct Stopped {
    int left = 0;          // (writes still under way when the wait ended: cut by the exit)
    double waited_ms = 0;  // (how long the wait took)
    int refused = 0;       // (writes not begun since the end began, so far)
};

/// The process is about to end, on this thread: no write begins on another thread from here on, and the ones under way are
/// waited for, up to ms. ms 0: nothing is stopped (the old behaviour)
inline Stopped stop(int ms) {
    Stopped s;
    if (ms <= 0) return s;
    t_ending = true;
    g_stopping.store(true);
    const auto t0 = std::chrono::steady_clock::now();
    const auto until = t0 + std::chrono::milliseconds(ms);
    while (g_busy.load() > 0 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    s.left = g_busy.load();
    s.waited_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    s.refused = g_refused.load();
    return s;
}

}  // namespace file_writers
