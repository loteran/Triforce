// (Triforce) Start next frame early: the early tail (vk_replay.cpp "the early tail"; the Graphics tab's row, RX_VK_TAIL_EARLY
// over it; after NX2navi's tail_early, NX2NAVI_TAIL_EARLY, by epicmeme546). Its rules apart from Vulkan, so that
// tests/test_tail_early.py runs them, and the production functions that use them, over recorded command streams.
//
// OoT's Presentation thread polls its frame's event (nvnEventGetValue, host/hos_thread.c present_poll) before it presents and
// lets the next frame be built. The event's word reaches the game's memory when the GPU has run the signal and, where its page
// has a VRAM mirror, the write-back at the end of the signal's recording (vk_memory.cpp memory_end_submission): after
// everything the GPU runs of that recording, the frame's tail too. The tail is what follows the scene: the gameplay upscale's
// dispatch (the game's network, or DLSS, DLAA, FSR 3.1 or FSR 4 in its place), the interface drawn over the picture, the
// recording's end. Nothing of it is what the game reads next: it writes images (the picture, the upscalers' own), which the
// game never reads back, and the present's blit is a recording of its own after it.
//
// With the row on, the replay cuts its recording at the gameplay upscale's dispatch (a part's end as maybe_split makes it: the
// uniform updates before it, the write-backs, a full barrier, the part to the queue) and records the rest into one command
// buffer, without further splits (the tail). Its event signals are held, not written in it. At the recording's end the tail
// is judged by what its commands did (verdict, below). One that passes has its held words written in its recording's upload
// command buffer, after the tail's uploads and before the tail's commands: on the GPU after every write-back of the work
// before the tail (the part before it is ahead in the queue and ends with a full barrier; the words' writes begin with one),
// and before any of the tail's commands. The game's wait ends there, while the GPU runs the tail. One that fails has them
// written at its end, before its write-backs, as their words always reached the game's memory (the game's wait ends where it
// did before).
//
// What a tail must not have done to pass, each a rule (verdict), counted from its first command to its recording's end:
//   WRITES   a binding written by the GPU (into a mirror, which a write-back copies to the game's memory after the tail, or into
//            guest memory itself). The tail's inline uniform updates are not counted: they are the stream's own writes of
//            uniform blocks, written at its recording's end as ever (flush_ub_updates), and the game does not read them.
//   GUEST    a binding given guest memory itself (a pool with no mirror, a mirror piece left in guest memory): the GPU would read
//            the game's memory while the game writes its next frame there.
//   PROGRAMS a program of the game's that reaches guest memory past its bindings: through the page table (global memory) or a
//            texel buffer (a view of a pool's import), read or written.
//   BAKE     a texture's write-back into the game's memory at its recording's end (vk_texture.cpp texture_end_recording).
//   EVENT    an event's word outside the pools, or not 4-aligned: the early write has nowhere to go.
//   CUT      the tail did not stay one command buffer to its recording's end (cut short by something that submits it).
// What it reads otherwise is the replay's: mirrors (synced in its uploads, ahead of the words), its own stream buffers (written
// by the CPU at the replay), images. Counter reports are the CPU's at the replay already (vk_replay.cpp NOP_REPORT_COUNTER),
// and the waits between the game's queues are ordered on the CPU from the signals' values (ev_values), which the replay keeps
// as before.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tail_early {

/// What a tail's commands did, from its first to its recording's end (vk_replay.cpp tail_seen)
struct Seen {
    uint64_t written = 0;    // bindings the GPU writes (WRITES)
    uint64_t guest = 0;      // bindings given guest memory itself (GUEST)
    uint64_t programs = 0;   // the game's programs that reach guest memory past their bindings (PROGRAMS)
    bool bake = false;       // a texture written back into guest memory at the recording's end (BAKE)
    bool event_outside = false;   // a held event's word with no buffer for the early write (EVENT)
    bool cut = false;        // not one command buffer to its recording's end (CUT)
};
enum Why { EARLY = 0, CUT, WRITES, GUEST, PROGRAMS, BAKE, EVENT, WHY_N };
inline const char* why_name(Why w) {
    static const char* const n[WHY_N] = {"early", "cut short", "a written binding", "guest memory bound", "a program past its bindings",
                                         "a texture's write-back", "an event outside the pools"};
    return w >= 0 && w < WHY_N ? n[w] : "?";
}
/// EARLY: the tail goes after its held words; else the first rule it fails, and the words are written at its end
inline Why verdict(const Seen& s) {
    if (s.cut) return CUT;
    if (s.written) return WRITES;
    if (s.guest) return GUEST;
    if (s.programs) return PROGRAMS;
    if (s.bake) return BAKE;
    if (s.event_outside) return EVENT;
    return EARLY;
}

/// The tail's event signals, held in their order. A signal of mode ADD adds to the last value held for its word, else to
/// what the game's memory has (vk_replay.cpp NOP_SIGNAL_EVENT)
struct Signal {
    uint64_t gpu;
    uint32_t v;
};
class Held {
  public:
    void clear() { list_.clear(); }
    void add(uint64_t gpu, uint32_t v) { list_.push_back({gpu, v}); }
    bool last(uint64_t gpu, uint32_t* v) const {
        for (size_t i = list_.size(); i--;)
            if (list_[i].gpu == gpu) return *v = list_[i].v, true;
        return false;
    }
    const std::vector<Signal>& all() const { return list_; }
    bool empty() const { return list_.empty(); }

  private:
    std::vector<Signal> list_;
};

/// The counts of what the tails did, for the log's line (vk_replay.cpp tail_stats)
struct Stats {
    uint64_t tails = 0, signals = 0, moved = 0;   // (tails judged; their signals held; those written before their tail)
    uint64_t why[WHY_N] = {};
    void judged(Why w, size_t held) {
        tails++, why[w]++, signals += held;
        if (w == EARLY) moved += held;
    }
};

}  // namespace tail_early
