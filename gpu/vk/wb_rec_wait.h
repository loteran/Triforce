// (Triforce, wp20-onepct; the handoff's §12.9: a read waits for its writer's part, not its whole submission) A wait for a
// write-back still on the GPU (vk_replay.cpp vk_timeline_wait: a capture's late pages on the game's submitting thread, a read or
// a binding of a page put off) waited for the submissions' timeline to reach the writer's value. That timeline is signalled only
// as far as no older submission is stopped at an event wait (vk_replay.cpp finish_recording): with another queue's work stopped,
// the wait went on after the GPU had done the write-back, until that work was let on and recorded. The lab's steady Market had a
// late-page wait of 20 to 46 ms about once a minute with the GPU 4 submissions behind at 5.5 ms of GPU time a frame (r37, r38,
// r41: 186 waits of 8 ms or more in 15 runs, the median 11.9 ms), each a stall of the game's thread.
//
// The recordings' timeline (vk_replay.cpp rec_timeline) is signalled at the end of every recording, in the queue's order, and a
// signal covers all that was submitted before it; so the recording a submission was finished in, done, means all that submission
// recorded is done, its write-backs too. The write-backs a recording makes are noted under its value (vk_memory.cpp
// wb_pages_note, mem_rec_value), and only a submission finished in a recording is known here (Finished), never one still being
// recorded or stopped at a wait: those are waited for on the timeline as before. The timeline cannot reach a value before the
// recording that finished it is done, so waiting for the recording first never makes the wait longer: the part after it is the
// timeline held (held_ns), counted always (the stall line, the frame trace, the capture's late line). RX_VK_WB_WAIT_REC=1 ends the
// wait there. Off by default: the captures' late-page wait orders something the game's frame depends on that is not known yet
// (vk_memory.cpp take_capture, RX_VK_LATE_NEW), and what the stopped work writes back lands after the copy taken without its
// wait. Tried with the lab's flicker count (scenario.ps1 -Flick) before it is turned on. tests/test_wb_rec_wait.py
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <vector>

namespace mkwvk::wb_rec_wait {

/// The longest wait (ns): vk_timeline_wait's bound, both waits together
constexpr uint64_t kBoundNs = 250000000ull;
/// A held part shorter than this is the two signals' own latency, not the timeline held (ns)
constexpr int64_t kHeldMinNs = 100000;

/// RX_VK_WB_WAIT_REC's text: on with a number other than 0
inline bool on(const char* text) { return text && *text && std::atoi(text) != 0; }

/// What is left of the bound for the timeline's wait after the recording's took spent_ns
inline uint64_t budget_left(uint64_t bound_ns, int64_t spent_ns) {
    if (spent_ns <= 0) return bound_ns;
    return (uint64_t)spent_ns >= bound_ns ? 0 : bound_ns - (uint64_t)spent_ns;
}

/// Whether the wait ends with the recording's: the switch on, the recording known, and done within the bound
inline bool ends_at_recording(bool switch_on, bool known, bool done) { return switch_on && known && done; }

/// The part of a wait of total_ns after its writer's recording was done (rec_ns into it): 0 when the recording is not known, or
/// was not done within the bound, or the rest is the signals' latency alone
inline int64_t held_ns(bool known, bool done, int64_t rec_ns, int64_t total_ns) {
    if (!known || !done || total_ns <= rec_ns) return 0;
    const int64_t h = total_ns - rec_ns;
    return h >= kHeldMinNs ? h : 0;
}

/// The finished submissions: value -> the recording (its number on the recordings' timeline) it was finished in; the oldest go
/// past cap. Noted at the submit that ends the recording they were finished in (submitted), never for one still being recorded
struct Finished {
    std::map<uint64_t, uint64_t> by_value;
    size_t cap = 8192;
    /// ((Triforce) the submissions finished in a recording that goes on in another part. A part's submit (a split, the early
    /// tail's start, a parallel replay's setup before its segments) is not the end of their write-backs: a burst's are noted
    /// under its value, and the last of them are recorded at the burst's end (vk_replay.cpp finish_recording,
    /// memory_end_submission), a later part than the piece that finished them; a parallel replay's setup records none. Noted at
    /// the part's number, a wait ended there (RX_VK_WB_WAIT_REC=1) before its writer's part had run)
    std::vector<uint64_t> pending;
    /// A recording's submit, its number rec: values, the submissions finished in it since its submit before. ends: the recording
    /// ends with it (finish_recording, a present's); else a part of it, and they are noted at the submit that ends the recording
    void submitted(const std::vector<uint64_t>& values, uint64_t rec, bool ends) {
        pending.insert(pending.end(), values.begin(), values.end());
        if (!ends) return;
        for (uint64_t v : pending) note(v, rec);
        pending.clear();
    }
    void note(uint64_t value, uint64_t rec) {
        if (!value || !rec) return;
        by_value[value] = rec;
        while (by_value.size() > cap) by_value.erase(by_value.begin());
    }
    /// The recording value was finished in; 0: not known (not finished yet, or too old)
    uint64_t of(uint64_t value) const {
        const auto it = by_value.find(value);
        return it == by_value.end() ? 0 : it->second;
    }
};

} // namespace mkwvk::wb_rec_wait
