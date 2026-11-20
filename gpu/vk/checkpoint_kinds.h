/* (Triforce) The kinds of RX_VK_CHECKPOINTS markers (vk_draw.cpp checkpoint, checkpoint_mark) and what lies after each, for the
   device lost's report. The owner's test11 and test12 and two lab runs of five lost the device at the title the same way: the
   top and the bottom of the pipe both at "write-back of pool ...", the next marker the next frame's first dispatch, never
   reached. Between those two markers there was a pool's write-back copies, the write-back diff pass, the end of that recording,
   the whole present (its recording, the blit to the window's image after its acquire, the menu) and the next recording's start
   and uploads: none of them had a marker, and the report could not say which the GPU stopped in. Now each of those has one, and
   the report says what the work after the last marker reached is (gap). Plain functions: tests/test_checkpoint_kinds.py. */
#pragma once

#include <cstdint>

namespace mkwvk::checkpoint_kinds {

/// A marker's kind as the report names it; its key follows the name in the report (a program, a pool, a texture, a recording's
/// timeline value, a swapchain image's index or a chunk count, as the name says)
inline const char* name(char c) {
    switch (c) {
    case 'd': return "draw of program";
    case 'c': return "dispatch of program";
    case 'i': return "INDIRECT dispatch of program";
    case 'm': return "mirror sync of pool";
    case 'b': return "write-back of pool";
    case 'u': return "upload of texture";
    case 'g': return "GPU unswizzle of texture";
    case 'w': return "write-back diff pass, chunks";
    case 'r': return "recording begun, value";
    case 'v': return "uploads of the recording, value";
    case 'z': return "recording ended, value";
    case 'p': return "present blit begun, image";
    case 'M': return "menu drawn over the present, image";
    case 'P': return "present blit ended, image";
    default: return "?";
    }
}

/// What the GPU had to do after a marker of this kind, up to the next marker recorded: where a device lost whose last marker
/// reached at the top of the pipe is this one stopped
inline const char* gap(char c) {
    switch (c) {
    case 'd': case 'c': case 'i':
        return "that draw or dispatch itself (its marker is set just before it), or what its command buffer records after it before the "
               "next marker: barriers, copies, clears, queries";
    case 'm': return "that mirror sync's copies (guest memory -> the pool's VRAM mirror), in the recording's uploads";
    case 'b':
        return "that pool's write-back copies (its mirror -> guest memory, and the shadow's fill), then the write-back diff pass if "
               "one follows (w): with no w after it, the end of the recording (z)";
    case 'u': case 'g': return "that texture's upload or unswizzle itself";
    case 'w':
        return "the write-back diff pass (one compute dispatch over every pool's chunks, reading mirrors and shadows and writing guest "
               "memory by device address), then its barrier and the end of the recording (z)";
    case 'r': return "the recording's first commands: its barrier, and the replay's work before its first marked draw or dispatch";
    case 'v': return "the recording's uploads: mirror pieces filled, mirror syncs, the page table's changes, texture uploads";
    case 'z':
        return "the next submission: its waits on the queue (the mirrors' new pieces bound, a swapchain image acquired) or its first "
               "commands (r, v, or the present's p next)";
    case 'p': return "the present's blit: the game's picture scaled into the window's image, the HDR pass and the bars, before the menu (M)";
    case 'M': return "the menu's drawing (Dear ImGui) over the present, then its last barrier (P)";
    case 'P': return "the present of that image, then the next submission's waits and first commands";
    default: return "not known (a marker of no named kind)";
    }
}

/// The two stages' last markers (sequence numbers; 0: none reached): one sentence of what they mean together
inline const char* reading(uint32_t top, uint32_t bottom) {
    if (!top) return "no marker was reached at the top of the pipe";
    if (bottom == top)
        return "every command up to the last marker reached is done and nothing after it began: the GPU stopped in what follows that "
               "marker (below), or in a wait before the next one";
    if (bottom < top)
        return "the work between the bottom's last marker and the top's was running: the GPU stopped in it (a draw or dispatch that "
               "did not end, or a fault)";
    return "the bottom of the pipe is past the top: the markers are not in order (several queues or threads), read them with care";
}
/// (Triforce, P4) The two stages' last markers are of two threads' rings (vk_draw.cpp checkpoint_next: each recording thread
/// numbers its own): their numbers say nothing of each other's order
inline const char* apart() {
    return "the two are of two threads' recordings (each numbers its own markers: the replay thread's, a helper thread's, the "
           "present's): their numbers say nothing of their order, read each with the markers around it (below)";
}

}  // namespace mkwvk::checkpoint_kinds
