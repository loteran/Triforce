// (Triforce, wp20-onepct) The per-frame trace's split of a frame (RX_VK_FRAME_TRACE=1: triforce_frames.csv beside the game, one
// line a frame, vk_replay.cpp status_tick). Its first columns say what the replay did (the draws' parts, sampled and summed over
// every thread that records: the parallel replay's helpers too); they could not say what made a slow frame that was not the
// draws': the lab's Market frames over the 99th percentile were 7-21% explained by them (r37, r38, r41). These columns, after
// the first ones, split every frame the way the stall line splits a frame of 50 ms or more (stall_watch.h): the replay's own
// time (its submits and the present phase, on every thread that replays: on the game's threads with the synchronous replay),
// the present's own, the dispatches and the queue submits within the replay, the replay thread's waits for its helpers'
// segments, and what is left: outside the replay and the present, the game's own update. Of that, the game's threads spend
// some in our work: their fence waits and the captures at their submits; and the waits for write-backs still on the GPU (the
// captures' late pages, the reads and bindings put off), with the part of them after the writer's recording was done on the
// GPU (the submissions' timeline held back by work stopped at an event wait: vk_replay.cpp vk_timeline_wait), and the queue
// submits made while a background import ran. Plain functions: tests/test_frame_trace.py
#pragma once
#include <cstddef>
#include <cstdio>

namespace mkwvk::frame_trace {

/// One frame's split, in milliseconds
struct Split {
    double interval = 0;   // (present to present)
    double replay = 0;     // (the submits and the present phase, every thread that replays: the stall line's "replay")
    double present = 0;    // (the present's own: the swapchain's image, its fence, the present call)
    double dispatch = 0, qsubmit = 0;   // (in replay: the dispatches, the queue submits)
    double par_wait = 0;   // (in replay: the replay thread's waits for the helper threads' segments)
    double game_wait = 0;  // (the game's fence waits, every thread)
    double captures = 0;   // (the submit-time captures on the game's threads)
    double wb_wait = 0;    // (the waits for write-backs still on the GPU, wherever they were: memory sync's capture, a read)
    double wb_held = 0;    // (of them, the part after the writer's recording was done: the timeline held, not the GPU)
    double qs_bg = 0;      // (the queue submits made while a background import ran)
};

/// Outside the replay and the present: the game's own update, its waits for the replay and the GPU among it. As the stall line's
inline double outside(const Split& s) { return s.interval - s.replay - s.present; }

/// The columns, after the trace's first ones (their names; the line's values in this order: line)
constexpr const char* kColumns =
    "replay_ms,present_ms,dispatch_ms,qsubmit_ms,par_wait_ms,outside_ms,game_wait_ms,captures_ms,wb_wait_ms,wb_held_ms,qs_bg_ms";

/// The values for kColumns, comma-separated, no line end: snprintf's count
inline int line(const Split& s, char* out, size_t cap) {
    return std::snprintf(out, cap, "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f", s.replay, s.present, s.dispatch,
                         s.qsubmit, s.par_wait, outside(s), s.game_wait, s.captures, s.wb_wait, s.wb_held, s.qs_bg);
}

} // namespace mkwvk::frame_trace
