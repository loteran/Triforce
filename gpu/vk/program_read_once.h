/* (Triforce) A program a draw reads from the program cache itself, read with the shaders' lock let go (vk_shader.cpp
   program_cache_load, readahead_take).

   A draw that meets a program for the first time in a session and finds it neither in memory nor read ahead reads it from
   triforce_programs.bin there and then: the record off the disk and its stages' shader modules made, 0.6 to 1.3 ms a program
   in the stall lines of the owner's test29 ("108 programs read from the cache by the draws (63.3 ms)"). The program pick
   (vk_shader.cpp program_pick, mesh_program_for) held shader_mu through it, and through a draw's wait (up to 50 ms) for a
   program the read-ahead's worker was reading, so the replay's helper threads, each picking its own programs, queued behind
   it: "the program picks waited 169.0 ms for the shaders' lock" at the Market's arrival (lab run r33-t28-s1-1, frame 17617),
   58.4 ms at an area change in the owner's session 2.

   Now the draw marks the program's key as being read, lets shader_mu go, reads into a list of its own, takes the lock again and
   only then puts the variants in the program's list: a program is in that list only once whole and ready, as before. A second
   draw of the same program waits for the first's result, not for the lock and not to read it again (claim). The record stays
   listed in the cache's list until the read is done, so that every other reader of that list (the prepare in play, the
   borrows, the read-ahead's choice) sees it kept, as before the read began. The same records are read the same way: nothing
   changes what a draw gets.

   The lock is let go only where the pick holds it (program_cache_load's draws: program_pick and mesh_program_for hold it whole)
   and only for the read, and what the pick holds on to across it is not let go meanwhile: its program's list (the programs'
   lists are never erased) and its stages' code, which only the program's release lets go (vk_shader.cpp program_released). A
   release closes the gate (close): no read lets the lock go from then on, and the release waits until none has it let go.
   Releases are rare (the game finalizes programs at area changes) and already wait for the replay (vk_replay.cpp
   do_program_released holds replay_mu), so this costs nothing in play.

   Locks: the caller's (shader_mu), then this one's own (mu_), never the other way: a reader waiting for another's read waits
   on mu_ with the caller's let go, and takes the caller's again after letting mu_ go. Plain code, the caller's mutex a template
   parameter: tests/test_program_read_once.py runs vk_shader.cpp's program_cache_load and readahead_take with threads against
   a program cache whose reads are slow. RX_VK_PROGRAM_READ_UNLOCKED=0: read under the lock, as before. */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace mkwvk::program_read_once {

/// RX_VK_PROGRAM_READ_UNLOCKED: on unless "0"
inline bool on(const char* env) { return !(env && env[0] == '0' && !env[1]); }

/// The keys being read by the draws themselves, and the windows in which a pick has the caller's lock (outer) let go
template <class Mutex> class Reads {
public:
    explicit Reads(Mutex& outer) : outer_(outer) {}

    /// (outer held) Whether a read may let outer go: none while a release waits (close)
    bool may_let_go() const { return closed_ == 0; }

    /// (outer held) key to be read by this caller: true, and it is marked as being read (done ends that). false: another
    /// caller reads it now; it was waited for with outer let go, and outer is held again (its variants are in their list by
    /// then, put there before done). *wait_ns: the wait, the lock's taking again included. (Outer is let go for this wait even
    /// while a release waits: the read waited for needs outer to end, and the release waits for both)
    bool claim(uint64_t key, std::atomic<int64_t>* wait_ns = nullptr) {
        std::unique_lock lk(mu_);
        if (keys_.insert(key).second) return true;
        const auto t0 = std::chrono::steady_clock::now();
        open_++;   // (outer held: close() reads it under outer)
        outer_.unlock();
        cv_.wait(lk, [&] { return !keys_.count(key); });
        lk.unlock();
        outer_.lock();
        leave();
        if (wait_ns) *wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        return false;
    }

    /// (outer held) fn run with outer let go (a read, a wait for the read-ahead's worker), outer taken again after it, the
    /// wait to take it counted in *relock_ns. Only where may_let_go(); fn must not take outer
    template <class Fn> void unlocked(Fn&& fn, std::atomic<int64_t>* relock_ns = nullptr) {
        open_++;
        outer_.unlock();
        struct Relock {
            Reads& r;
            std::atomic<int64_t>* ns;
            ~Relock() {
                const auto t0 = std::chrono::steady_clock::now();
                r.outer_.lock();
                if (ns) *ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
                r.leave();
            }
        } relock{*this, relock_ns};
        fn();
    }

    /// (outer held) key's read done (what it read is in its list by now): the callers waiting for it go on
    void done(uint64_t key) {
        {
            std::lock_guard lk(mu_);
            keys_.erase(key);
        }
        cv_.notify_all();
    }

    /// Whether key is being read by a caller now (the read-ahead's worker leaves it alone)
    bool reading(uint64_t key) const {
        std::lock_guard lk(mu_);
        return keys_.count(key) != 0;
    }

    /// (outer not held) A release: from now on no read lets outer go, and this returns once none has it let go (outer not held
    /// then). reopen() ends it
    void close() {
        std::unique_lock lk(outer_);
        closed_++;
        gate_.wait(lk, [&] { return open_ == 0; });
    }
    void reopen() {
        std::lock_guard lk(outer_);
        closed_--;
    }

    /// The picks with outer let go now (for the tests: read without outer)
    int open() const { return open_.load(); }

private:
    void leave() {   // (outer held again)
        if (--open_ == 0) gate_.notify_all();
    }
    Mutex& outer_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_set<uint64_t> keys_;   // (being read; mu_)
    std::condition_variable_any gate_;    // (open_ back to 0; waited on with outer)
    std::atomic<int> open_{0};            // (picks with outer let go; changed under outer)
    int closed_ = 0;                      // (releases waiting or under way; outer)
};

/// A release's close() for a scope
template <class R> struct Closed {
    R& r;
    explicit Closed(R& reads) : r(reads) { r.close(); }
    ~Closed() { r.reopen(); }
    Closed(const Closed&) = delete;
    Closed& operator=(const Closed&) = delete;
};

} // namespace mkwvk::program_read_once
