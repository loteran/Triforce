// (Triforce) The replay thread's rules (RX_VK_ASYNC=1, off by default): what it took to make the thread a place frames can be
// replayed on without the faults the lab's runs of it showed. Nothing here releases the game early: the game still presents only
// once the GPU has run its frame (its own event poll), so the picture and its order are the submitting thread's replay's.
//
// What the runs showed (rounds 22 and 23, vk_replay.cpp's own counters): the thread took the replay of the game's main thread's
// two queues off the part of the frame both of the game's chains wait for (about 1.3 ms at the Market), and the frame got only 0.4
// to 0.8 ms shorter, because
//   1. a capture was taken at every signal of the CPU's while the thread had any work (26,654 in 26,400 frames, 0.25 ms each on
//      the game's thread), where the submitting thread's replay needs one only when a queue really goes on from a wait for that
//      event (126 to 1,028 a run);
//   2. the submit's capture made every copy on the game's thread, where the submitting thread's replay leaves them to the copy
//      helpers;
//   3. the game's submit gave its virtual core up and took it again around a hand-over of a quarter millisecond;
//   4. the game's present poll slept in steps of the system's timer, about 0.6 ms late on average;
//   5. every present made the stopped queues go past their waits (5,649 to 15,166 waits gone past a run against 0), which is
//      also the first suspect of the flicker after a tour.
// The rules below are 1 and 5, apart from Vulkan and from the replay's state so that tests/test_async_rules.py can run them; 2 to
// 4 are in vk_replay.cpp submit_q, host/nvn_native.c nvnQueueSubmitCommands and host/hos_thread.c present_poll. Each has a switch
// of its own, and all of it is behind RX_VK_ASYNC=1.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <vector>

namespace mkwvk::async_rules {

/// A switch that is on unless set to 0 (RX_VK_ASYNC_EVENT_NOTES, RX_VK_ASYNC_CAPTURE_BEHIND)
inline bool on_unless_zero(const char* env) {
    return !env || !*env || std::atoi(env) != 0;
}

// ---- 1. the capture at a signal of the CPU's ----
// The game writes what a queue's work reads after it submitted that work, up to its signal of the event the work waits for; the
// console's GPU reads memory after its wait. With the replay on its own thread that capture has to be taken at the signal, on the
// game's thread (later the game has written on). It is needed only when a submission handed over before the signal holds a wait
// for that event that the replay has not passed: every submission handed over after the signal has its own capture, taken after
// the game's writes. The host tells the events a submission waits for (its walk at the submit sees the wait ops:
// host/nvn_native.c resolve_deferred); a submission is noted from its hand-over until it is replayed whole.

constexpr uint64_t kAnyEvent = ~0ull;   // (a submission whose waits the host did not tell: it may wait for any event)
constexpr size_t kMaxNotes = 256;       // (more than this pending: every signal takes its capture, as before, until those past it are done)

struct Notes {
    struct Note {
        uint64_t value;                 // (the submission's timeline value: its hand-over's)
        std::vector<uint64_t> events;   // (the events it waits for; kAnyEvent: not told)
    };
    std::deque<Note> pending;           // (in hand-over order)
    bool over = false;                  // (a submission past kMaxNotes is not replayed whole yet: waits_for answers yes)
    // ((Triforce) the submissions handed over past the bound, by value: their events are not known, so every event counts until
    // each of them is done. The flag fell once `pending` was empty, which could be while one past the bound was still stopped
    // at its wait, and its signal then took no capture)
    std::vector<uint64_t> unnoted;

    /// A submission handed over. told: the host gave its waits (events, n; n 0: it waits for none, and nothing is noted)
    void add(uint64_t value, bool told, const uint64_t* events, int n) {
        if (told && n <= 0) return;
        if (pending.size() >= kMaxNotes) {
            over = true;
            unnoted.push_back(value);
            return;
        }
        Note note{value, {}};
        if (!told) note.events.push_back(kAnyEvent);
        else
            for (int i = 0; i < n; i++)
                if (std::find(note.events.begin(), note.events.end(), events[i]) == note.events.end()) note.events.push_back(events[i]);
        pending.push_back(std::move(note));
    }
    /// The submission of that value replayed whole (none of it stopped at a wait any more)
    void done(uint64_t value) {
        for (auto it = pending.begin(); it != pending.end(); ++it)
            if (it->value == value) {
                pending.erase(it);
                break;
            }
        for (auto it = unnoted.begin(); it != unnoted.end(); ++it)
            if (*it == value) {
                unnoted.erase(it);
                break;
            }
        if (unnoted.empty()) over = false;
    }
    /// Whether a submission handed over and not replayed whole may hold a wait for the event
    bool waits_for(uint64_t event) const {
        if (over) return true;
        for (const Note& note : pending)
            for (const uint64_t e : note.events)
                if (e == event || e == kAnyEvent) return true;
        return false;
    }
};

/// Whether a signal of the CPU's takes a capture on the game's thread. behind: the replay thread has work handed over and not
/// recorded. notes_on: RX_VK_ASYNC_EVENT_NOTES (off: every signal with the thread behind, as before)
inline bool capture_at_signal(bool behind, bool notes_on, const Notes& notes, uint64_t event) {
    if (!behind) return false;
    return !notes_on || notes.waits_for(event);
}

// ---- 5. what a present on the replay thread does with the queues stopped at a wait ----
// Force: every stopped queue past its wait (as it was by default: the waits gone past). Keep: nothing forced (a load stood for
// good with it: only a present makes a load's stopped work go). Stale: the jobs behind the present are run first, a few
// milliseconds at most, until the frame's stopped work has gone on, and then only what was already stopped at the present before
// is made to go (vk_replay.cpp drain_stale). RX_VK_PRESENT_DRAIN: unset or 2 stale, 1 force, 0 keep
enum class PresentDrain { Keep = 0, Force = 1, Stale = 2 };
inline PresentDrain present_drain(const char* env) {
    if (!env || !*env) return PresentDrain::Stale;
    const int v = std::atoi(env);
    return v == 0 ? PresentDrain::Keep : v == 1 ? PresentDrain::Force : PresentDrain::Stale;
}

// ---- what is still open (2026-11-19, lab rounds 52 to 60; the thread stays off by default) ----
// Standing at the Market the thread gave 104.3 fps for 97.5 and a 1% low of 81.7 for 75.2, with no flicker (3 runs each). After
// a ten-area tour the player's character is drawn black for one frame, 0 to 3 times a minute at the Market (the default: none in
// 2 tours); with the presents forcing again (RX_VK_PRESENT_DRAIN=1) 37 to 111 times, so rule 5 is what keeps it rare. Every pass
// and dispatch of such a frame is there with its draws: the character is lit from wrong inputs, in a frame after one whose replay
// ran late. Ruled out: these rules (the thread without them does it too), texture uploads (none in three of four such frames),
// a readback handing the game a word it had set to 0 (none at 43 such frames: RX_VK_LOG_READBACK_ZERO), the conditions on
// occlusion counts and the occlusion answers (RX_VK_OCC_COND=0, RX_VK_OCC_QUERY=0: still there), and the game's request flags
// (all read set around eight such frames). Not settled: whether the lab's frame log, whose readback and disk writes are on this
// thread, is what makes the replay late there (one tour each: 3 such frames with its writes, none with RX_VK_FRAMELOG_DROP=1).
// RX_VK_REGION_TRAP=1 with RX_VK_PASS_TRACE=1 and the frame log logs the passes and conditions around the first eight.

// ---- the replay thread's look for its next job ----
/// RX_VK_ASYNC_SPIN_US: how long the thread looks for a job before it sleeps, in nanoseconds: 100 us unset, 0 none, 1000 at most.
/// The thread is on the core kept from the game's threads, and a wake of a sleeping thread is tens of microseconds for each of a
/// frame's hand-overs
inline int64_t spin_ns(const char* env) {
    if (!env || !*env) return 100 * 1000;
    return (int64_t)std::clamp(std::atoi(env), 0, 1000) * 1000;
}

} // namespace mkwvk::async_rules
