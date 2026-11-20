/* (Triforce) A submit-time capture's copies behind the game's ordering point (vk_memory.cpp capture_make, RX_VK_CAPTURE_BEHIND).
   With the replay on the submitting thread (RX_VK_ASYNC off, the default) the game's thread was held for the whole capture before
   its replay began: the scan of the write marks, then the copy of every page they named. At the Castle Town Market that was
   ~0.35 ms a submit, six submits a frame: 2.1 ms of a 10.6 ms frame, outside the replay (the frame trace of 2026-11-15: scan 0.10
   ms over ~1,240 ranges, ~2,000 pages copied, 7.8 MB). The scan is the ordering point: the pages written before the submit, their
   marks taken. The copy need not be on the game's thread before the replay: the submitting thread replays the submission before it
   returns to the game, so nothing it writes after its submit can reach the copy, and what reads the copy comes after the scan: the
   replay's reads as submitted, the late pages' second copy and the texture shadow (take_capture), and the GPU's copies into the
   mirrors once a recording is submitted. So the copy is cut in parts of consecutive pages that the capture's copy helpers take as
   soon as the scan is done, while the submitting thread replays. A reader of a page whose part nobody has taken yet copies that part
   itself; one whose part is being copied waits for it. Before a recording goes to the GPU, and before the submitting thread goes
   back to the game, every part is done. Each page is copied once, by whoever claims its part.
   Plain C++, no Vulkan: tests/test_capture_behind.py compiles it with stand-ins. */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace mkwvk::capture_behind {

constexpr int64_t kPartPages = 32;   // (128 KB a part: what a reader that needs one of its pages copies itself, ~10 us)

struct Part {
    int64_t i0 = 0, i1 = 0;          // (the capture's indices [i0, i1): consecutive, none of them copied ahead)
    std::atomic<uint8_t> state{0};   // (0 nobody's yet, 1 being copied, 2 copied)
};

/// A capture's pages left to copy, in parts. An index is the page's place in the capture (its slot in the capture's buffer).
/// copy(i0, i1) is the caller's: it copies indices [i0, i1), and is run once for each part, by the thread that claims it
struct Work {
    std::unique_ptr<Part[]> parts;
    int nparts = 0;
    std::vector<int32_t> part_of;    // (by index: its part; -1: copied ahead, not one of this work's)
    std::atomic<int> left{0};        // (the parts not copied yet)
    std::mutex mu;                   // (the waits for a part another thread copies)
    std::condition_variable cv;

    /// The parts: the indices i < n with ahead[i] == 0 (ahead: null for none), in runs of at most part_pages cut at every index
    /// copied ahead
    void plan(const uint8_t* ahead, int64_t n, int64_t part_pages = kPartPages) {
        if (part_pages < 1) part_pages = 1;
        part_of.assign((size_t)(n > 0 ? n : 0), -1);
        std::vector<std::pair<int64_t, int64_t>> runs;
        for (int64_t i = 0; i < n;) {
            if (ahead && ahead[i]) {
                i++;
                continue;
            }
            int64_t j = i + 1;
            while (j < n && !(ahead && ahead[j]) && j - i < part_pages) j++;
            runs.push_back({i, j});
            i = j;
        }
        nparts = (int)runs.size();
        parts.reset(new Part[(size_t)nparts]);
        for (int k = 0; k < nparts; k++) {
            parts[k].i0 = runs[(size_t)k].first, parts[k].i1 = runs[(size_t)k].second;
            for (int64_t i = parts[k].i0; i < parts[k].i1; i++) part_of[(size_t)i] = k;
        }
        left.store(nparts, std::memory_order_release);
    }
    /// Part k is this thread's to copy (nobody had taken it)
    bool claim(int k) {
        uint8_t free = 0;
        return parts[k].state.compare_exchange_strong(free, 1, std::memory_order_acq_rel);
    }
    /// Part k copied, by the thread that claimed it: its bytes are there for every thread that sees it so
    void copied(int k) {
        {
            std::lock_guard lk(mu);
            parts[k].state.store(2, std::memory_order_release);
            left.fetch_sub(1, std::memory_order_acq_rel);
        }
        cv.notify_all();
    }
    bool done() const { return left.load(std::memory_order_acquire) == 0; }
    /// Index i is copied (or was copied ahead)
    bool ready(int64_t i) const {
        if (i < 0 || (size_t)i >= part_of.size()) return true;
        const int32_t k = part_of[(size_t)i];
        return k < 0 || parts[k].state.load(std::memory_order_acquire) == 2;
    }
    /// Part k copied on return: by this thread when nobody had taken it, else by whoever took it, waited for. 1: this thread
    /// copied it, 2: it waited, 0: it was copied already
    template <class Copy> int make_ready(int k, Copy&& copy) {
        if (parts[k].state.load(std::memory_order_acquire) == 2) return 0;
        if (claim(k)) {
            copy(parts[k].i0, parts[k].i1);
            copied(k);
            return 1;
        }
        std::unique_lock lk(mu);
        cv.wait(lk, [&] { return parts[k].state.load(std::memory_order_acquire) == 2; });
        return 2;
    }
    /// Index i copied on return (make_ready of its part)
    template <class Copy> int ensure(int64_t i, Copy&& copy) {
        if (i < 0 || (size_t)i >= part_of.size()) return 0;
        const int32_t k = part_of[(size_t)i];
        return k < 0 ? 0 : make_ready(k, copy);
    }
    /// Every part copied on return: those nobody had taken by this thread, in order, then the others waited for. The parts this
    /// thread copied
    template <class Copy> int finish(Copy&& copy) {
        int made = 0;
        for (int k = 0; k < nparts; k++)
            if (parts[k].state.load(std::memory_order_acquire) == 0 && claim(k)) {
                copy(parts[k].i0, parts[k].i1);
                copied(k);
                made++;
            }
        std::unique_lock lk(mu);
        cv.wait(lk, [&] { return done(); });
        return made;
    }
};

/// Whether a capture's copies go behind: helpers to take them, enough pages for them (copy_split's own threshold: under it the
/// submitting thread copies all at once, cheaper than waking a helper), and few to copy ahead (the late pages and the texture
/// storage: take_capture reads those at once; many of them, as at a scene's load, and the capture is copied whole as before)
inline bool worth_behind(int helpers, int64_t pages, int64_t ahead, int64_t min_pages = 128, int64_t max_ahead = 512) {
    return helpers > 0 && pages - ahead >= min_pages && ahead <= max_ahead;
}

} // namespace mkwvk::capture_behind
