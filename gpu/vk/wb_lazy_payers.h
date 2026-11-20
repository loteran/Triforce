// (Triforce) How many of the lazy write-back's payments copy at once (vk_memory.cpp lazy_pay; docs/lazy-writeback.md), in a
// header of its own so that tests run it without the game or a GPU (tests/test_wb_lazy_state.py).
//
// A payment's copy goes onto the queue after everything submitted so far, and its thread waits for it: 0.5 to 3 ms a payment
// in the owner's sessions, up to 21 ms at an area's load. As first built, one payer at a time held the payments' one command
// buffer for its copy and its wait (lz_gpu_mu): a second thread that touched another owed page, or the replay giving a mirror
// up, waited for the first one's copy to come back before its own could go onto the queue. NX2navi lets several run at once
// (wb_lazy_unlocked = 1, its default: four slots, each a command pool, its command buffer and a fence; a fetch past four waits
// for a slot). The page states need nothing for it: a page is PAYING for one payer only, a second toucher of it waits for
// that copy and makes none (wb_lazy_state.h), so the copies of different payers are of different pages. Each still goes onto
// the one queue under its lock and after everything before it; only the waits for them overlap.
//
// RX_VK_LAZY_PAYERS=<1..4>: how many payments copy at once; 1 is one at a time, as before. Unset: 4, as NX2navi.
#pragma once

#include <bit>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>

namespace wb_lazy {

constexpr int kMaxPayers = 4;

/// How many payments copy at once, from RX_VK_LAZY_PAYERS (env: its value; nullptr or "": not set): kMaxPayers when not set or
/// not a number, else the number within 1 to kMaxPayers
inline int payers_from(const char* env) {
    if (!env || !*env) return kMaxPayers;
    char* end = nullptr;
    const long v = strtol(env, &end, 10);
    if (end == env) return kMaxPayers;
    return v < 1 ? 1 : v > kMaxPayers ? kMaxPayers : (int)v;
}

/// The payers' slots: a payment takes one (its own command buffer and fence) for its copy and gives it back once it has
/// waited for that copy. With every slot in use, a payment waits for one to be given back (the condition lets the mutex go)
class Payers {
  public:
    /// n slots, 1 to kMaxPayers (as many as could be made); before the first payment, from one thread
    void init(int n) {
        n = n < 1 ? 1 : n > kMaxPayers ? kMaxPayers : n;
        std::lock_guard lk(mu);
        count = n, free_bits = (1u << n) - 1;
    }
    int size() {
        std::lock_guard lk(mu);
        return count;
    }
    /// A free slot, 0 to size() - 1, waited for while every one is in use
    int take() {
        std::unique_lock lk(mu);
        if (!free_bits) {
            waits++;
            cv.wait(lk, [this] { return free_bits != 0; });
        }
        const int i = std::countr_zero(free_bits);
        free_bits &= ~(1u << i);
        if (++busy > most) most = busy;
        return i;
    }
    void give(int i) {
        {
            std::lock_guard lk(mu);
            free_bits |= 1u << i;
            busy--;
        }
        cv.notify_one();
    }
    /// Payments that found every slot in use and waited for one, so far
    uint64_t waited() {
        std::lock_guard lk(mu);
        return waits;
    }
    /// The most slots in use at once, so far
    int most_at_once() {
        std::lock_guard lk(mu);
        return most;
    }

    /// A slot held for a scope
    struct Hold {
        explicit Hold(Payers& p) : payers(p), slot(p.take()) {}
        ~Hold() { payers.give(slot); }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        Payers& payers;
        const int slot;
    };

  private:
    std::mutex mu;
    std::condition_variable cv;
    uint32_t free_bits = 1;   // (bit i: slot i is free; one slot until init)
    int count = 1, busy = 0, most = 0;
    uint64_t waits = 0;
};

} // namespace wb_lazy
