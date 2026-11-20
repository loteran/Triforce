// (Triforce) Where the replay records less than a full barrier between work the game left unordered: a part's split inside a
// pass, an event's signal, a dispatch right after another; and when held submits go to the queue (RX_VK_BATCH). Apart from
// Vulkan so that tests/test_barrier_rules.py can run the rules over recorded command streams and check every hazard the game's
// ordering asks for against what the recorded barriers order (and, a rule dropped at a time, that a hazard is then left open).
//
// What the game's ordering asks for (NVN's, as the replay keeps it): work after one of the game's barriers after the work before
// it; a draw after a dispatch and a dispatch after a draw (the console's 3D and compute work share the engine and drain at the
// switch); copies, clears and uniform updates after and before what they touch; the colour, depth and stencil outputs of draws
// in the order they were drawn, from pass to pass too; a draw that samples a target its pass wrote, or writes one its pass
// sampled, after the draws before (the replay ends the pass there itself: vk_draw.cpp draw_hazards); an event's word written
// after the work before its signal, and a wait's work after the work before the signal it waits for. A signal is no barrier on
// the console: the work after it in the same queue is not ordered after the work before it.
//
// 1. A part's split (RX_VK_SPLIT_BARRIER; vk_replay.cpp maybe_split). A long submission goes to the GPU in parts of 120 draws and
//    dispatches (RX_VK_CHUNK) so that the GPU starts early. Each part ended its pass with a full barrier, and the next began with
//    another: the GPU drained between two draws of one of the game's passes, about 27 times a frame. With the rule the open pass
//    goes on in the next part: the part ends it, and the next begins it again with the same targets behind a barrier of the
//    attachments' outputs alone (colour output and the depth and stencil tests, their writes before their reads and writes), and
//    keeps what the pass's draws sampled and wrote, so that draw_hazards ends the pass where it would have without the split. That
//    orders all that the same draws in one pass ordered (RX_VK_CHUNK=0): within a pass Vulkan orders the attachments' accesses
//    alone. Only where nothing else was recorded at the split (the part's uniform updates, its write-backs, an event's write held:
//    then the full barriers as before), and a pass open. A part's uploads keep their own full barriers (vk_texture.cpp upload_cmd,
//    texture_uploads_recorded), and anything recorded or submitted between the part's submit and the pass's start again (another
//    submitter's work on the queue) gives the next part a full barrier first. Unset: on with NVIDIA's driver; =pass: on any card;
//    =full (or any other value): as before. ((Triforce, P4) A parallel replay's segments are never split: they are the parts there,
//    each begun and ended with a full barrier (vk_replay.cpp record_segment), so nothing goes on across a segment's edge; the rule
//    acts in the serial replay's parts, as before. "Recorded between" is the replaying thread's own commands and every submit to the
//    queue (cmd_work.h work_between); RX_VK_BARRIER_CONTEXT=0: every thread's commands, as before)
// 2. An event's signal (RX_VK_EVENT_BARRIER; vk_replay.cpp NOP_SIGNAL_EVENT). Each signal ended the pass and drained the GPU before
//    its word's write. With the rule nothing is recorded at the signal: its word's buffer is bound there (so its page is among the
//    recording's write-backs, as before), and the write waits for the next full barrier on its command buffer (the game's next
//    barrier, the next dispatch's or pass start's) and goes right after it, so it still follows all the work before the signal;
//    a recording's end writes what is still held behind a full barrier first, before its write-backs copy the words to the game's
//    memory (that barrier is recorded under the pools' lock, so the writes take none: their buffers were bound at the signals). The
//    writes count as work for the next barrier's look, so the write-backs' barrier is never left out after them. The writes held
//    for one barrier go in the order of their signals, a barrier of the transfers between each two, so the game sees them done in
//    that order (two signals of one word too). A wait met in the replay's order whose event's write is still held gets a full
//    barrier first: the waiting queue's work is then after the signalling queue's work before the signal, as the drain at the
//    signal made it. The game sees the word later (at that barrier, not at the signal). NX2navi records nothing at a signal at all
//    (its CPU writes the word once the submission is done). Unset: with the pass barrier's game mode (NVIDIA's driver,
//    pass_barrier.h); =late: on any card; =full (or any other value): as before. Not with the parallel replay.
// 3. A dispatch right after another (RX_VK_DISPATCH_BARRIER; vk_draw.cpp dispatch). Off unless asked for: =game leaves out the
//    full barrier of a dispatch whose command buffer's last command was the dispatch before it (no barrier of the game's, no copy,
//    no draw between; not an indirect one, whose grid another dispatch may have written), as NX2navi's barrier_game does. The game
//    orders its dependent dispatches with its own barriers on the console; whether it always does, only the game can show (a
//    one-frame band or flicker where it does not), so it is a switch for the lab. (Triforce, wp14, P4) Beside the parallel replay too:
//    "nothing between" is the command buffer's own count of its commands (cmd_work.h), and a segment is a command buffer of its own;
//    with the global count the rule stood aside while the helper threads recorded, which at the Market is most frames
//    (RX_VK_BARRIER_CONTEXT=0: so again). The event rule above still does (event_hold).
// 4. Held submits (RX_VK_BATCH; vk_replay.cpp submit_recording, submit_job). =1 (on by default with Mesa's RADV) holds every submit
//    until something needs the GPU's results or the queue's order. =parts holds a whole submission's recording only while the game's
//    submit is being replayed: a part (split off to start the GPU early) goes to the queue at once with what is held, so does the
//    held when there are RX_VK_BATCH_MAX (4 here) of them, and what is held goes when the replay of the game's submit returns.
//    Holding changes neither the command buffers nor their order on the queue, only how many vkQueueSubmit2 calls carry them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mkwvk::barrier_rules {

/// RX_VK_SPLIT_BARRIER: "pass" the rule on any card; "full" (or any other value) a full barrier at each split, as before; unset or
/// empty: on with NVIDIA's driver (vendor 0x10de). every_draw: RX_VK_BARRIER_EVERY_DRAW (an experiment that ends the pass at each
/// draw) keeps the old way
inline bool split_in_pass(const char* env, uint32_t vendor, bool every_draw) {
    if (every_draw) return false;
    if (env && *env) return std::strcmp(env, "pass") == 0;
    (void)vendor;   // ((Triforce, 11-16) off unless asked: the lab's Market had the same ~500 full barriers a frame with it on (the
    return false;   //  helper threads record most frames there, and the rule stands aside for them) and no gain (90.6 / 89.2 fps
                    //  against 92.8 / 90.3 with the old barriers)
}
/// Whether a part's split goes on in the same pass: the rule on, a pass open at the split, nothing recorded at the part's end (quiet:
/// the work count as it was after the pass's end) and no event's write held then (written behind a full barrier before the part's
/// write-backs, which copy its word to the game's memory: the GPU drained there all the same). ((Triforce, P4) no parallel replay's
/// argument: its segments are never split, maybe_split runs in the serial replay only)
inline bool split_carry(bool on, bool pass_open, bool quiet, bool writes_held) {
    return on && pass_open && quiet && !writes_held;
}
/// Whether the next part starts with a full barrier: always when the pass is not carried, and when it is but something was recorded
/// between the part's submit and the pass's start again (into its command buffer, by this thread into another, or submitted to the
/// queue by any thread: cmd_work.h work_between)
inline bool split_start_full(bool carry, bool recorded_between) {
    return !carry || recorded_between;
}

/// RX_VK_EVENT_BARRIER: "late" the rule on any card; "full" (or any other value) as before; unset or empty: with the pass barrier's
/// game mode (pass_barrier::game: NVIDIA's driver unless RX_VK_PASS_BARRIER says otherwise)
inline bool event_late(const char* env, bool pass_game) {
    if (env && *env) return std::strcmp(env, "late") == 0;
    (void)pass_game;   // ((Triforce, 11-16) off unless asked, as the split rule above: no gain measured)
    return false;
}
/// Whether a signal's write is held for the next full barrier: the rule on, no parallel replay (its segments' threads share the
/// command buffers), and room for it (most: the held writes a command buffer may have)
inline bool event_hold(bool on, bool parallel, int held, int most) {
    return on && !parallel && held < most;
}
/// A wait the replay finds met: a full barrier first when the write of its event's signal is still held
inline bool wait_barrier(bool its_write_held) {
    return its_write_held;
}

/// RX_VK_DISPATCH_BARRIER: "game" the barrier of a dispatch right after another left out; anything else, or unset: as before
inline bool dispatch_game(const char* env) {
    return env && std::strcmp(env, "game") == 0;
}
/// Whether a dispatch records its full barrier. work_since: GPU work recorded on the command buffer since its start (vk_draw.cpp
/// work_since_barrier); no_order: RX_VK_NO_DISPATCH_ORDER; right_after: the command buffer's last command was a dispatch's (by the
/// command buffer's own count, cmd_work.h: with the parallel replay too)
inline bool dispatch_barrier(bool work_since, bool no_order, bool game, bool right_after, bool indirect) {
    if (!work_since || no_order) return false;
    return !(game && right_after && !indirect);
}

/// RX_VK_BATCH
enum class Batch { Off, Hold, Parts };
/// "parts": Parts; otherwise set: Hold where its number is not 0, Off where it is (as RX_VK_BATCH was read); unset: Hold with RADV,
/// Off elsewhere
inline Batch batch_mode(const char* env, bool radv) {
    if (env) {
        if (std::strcmp(env, "parts") == 0) return Batch::Parts;
        return std::atoi(env) != 0 ? Batch::Hold : Batch::Off;
    }
    return radv ? Batch::Hold : Batch::Off;
}
/// Whether a recording is held rather than handed to the queue: Hold always; Parts only a whole submission's (not a part's), and
/// only inside the replay of a game's submit (in_submit: submit_job's)
inline bool batch_hold(Batch m, bool part, bool in_submit) {
    if (m == Batch::Hold) return true;
    return m == Batch::Parts && !part && in_submit;
}
/// Whether what is held goes to the queue now, after a submit was added to it (held: recordings held now, the added one too): Hold at
/// cap (RX_VK_BATCH_MAX, 32) as before; Parts at most (RX_VK_BATCH_MAX, 4) or when the added one was not held (a part: it goes with
/// those before it, in order)
inline bool batch_flush(Batch m, bool added_held, size_t held, size_t cap) {
    if (m == Batch::Off) return true;
    if (m == Batch::Parts && !added_held) return true;
    return held >= cap;
}

} // namespace mkwvk::barrier_rules
