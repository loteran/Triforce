// (Triforce) The barrier a draw pass starts with (vk_texture.cpp pass_start_barrier), apart from Vulkan so that
// tests/test_pass_barrier.py can run its rule.
//
// Before every draw pass the replay records a barrier: what the work before it wrote is made visible to it. A full one (all
// commands, all memory) makes the GPU drain: the new pass's vertex, task and mesh work waits until the pass before has drawn its
// last fragment. The game mode (RX_VK_PASS_BARRIER=game) keeps that wait for the new pass's fragment stages only (its fragment
// shaders, depth and stencil tests and colour output), where a draw pass comes right after another with nothing recorded
// between into its command buffer: no barrier of the game's (nvnCommandBufferBarrier is a full barrier and ends the pass), no copy,
// clear, uniform update or dispatch. ((Triforce, P4) by the command buffer's own count, cmd_work.h: what other command buffers get
// meanwhile is not between the two passes on the queue; a recording's uploads go before its commands, behind full barriers of their
// own.) The new pass's vertex, task and mesh work may then overlap the earlier pass's fragments, as on the
// console's GPU, where NVN asks for a barrier before rendering results are read. What changes order is only a vertex, task or
// mesh stage that reads or writes what the pass before touched with no barrier from the game between; every fragment-side access
// stays ordered after all earlier work, the writes and the reads (so a clear or a draw that writes a target an earlier pass
// sampled waits for that pass).
//
// Not where the replay itself ended the pass to keep a draw or a clear apart from its own pass's work (vk_draw.cpp draw_hazards,
// clear_after_draws, note_pass_split): those passes start with a full barrier, as before, since the draw that follows may read the
// pass's targets in its vertex or mesh stage. Not with RX_VK_BARRIER_EVERY_DRAW (an experiment that ends the pass at each draw for a
// full barrier there). ((Triforce, P4) With the parallel replay too: a segment is a command buffer of its own, begun with a full
// barrier, so its first pass start is a full one; RX_VK_BARRIER_CONTEXT=0, the one count of every thread's commands: not while the
// helper threads record, as before.)
//
// On by default with NVIDIA's driver only, where NX2navi turned its own game-only barriers on (its barrier_game, "about a tenth off
// the graphics card's time for a frame", 34 screenshots the same on an RTX 5090); nothing in Triforce's notes records a fault of
// this one, and none of the owner's runs had it on. NX2navi's rule is broader: between two passes it orders the attachments alone
// (no shader reads), and between two dispatches nothing. An attachment-only barrier leaves a target's write unordered with the
// shader reads of it before: across two passes, the fault Triforce's notes record within one (vk_draw.cpp clear_after_draws: a
// clear while draws before it still sampled its target, a band of the picture for a frame). So it is not taken; the dispatches'
// barriers are vk_draw.cpp dispatch's, not this rule's.
#pragma once

#include <cstdint>
#include <cstring>

namespace mkwvk::pass_barrier {

/// RX_VK_PASS_BARRIER: "game" the game mode on any card; "full" (or any other value) a full barrier at every pass start, as
/// before; unset or empty: the game mode with NVIDIA's driver (vendor 0x10de), full on every other card. every_draw:
/// RX_VK_BARRIER_EVERY_DRAW is set, which keeps every pass start full
inline bool game(const char* env, uint32_t vendor, bool every_draw) {
    if (every_draw) return false;
    if (env && *env) return std::strcmp(env, "game") == 0;
    return vendor == 0x10DE;
}

/// Whether a pass start gets the fragment-stage barrier: the game mode, and a draw pass the last thing on the command buffer (the
/// pass ended on this command buffer, its count as it was then, and not a pass the replay split). ((Triforce, P4) no parallel
/// replay's argument: vk_texture.cpp folds RX_VK_BARRIER_CONTEXT=0's standing aside into game)
inline bool relaxed(bool game, bool after_pass) {
    return game && after_pass;
}

} // namespace mkwvk::pass_barrier
