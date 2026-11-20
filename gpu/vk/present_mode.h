/* The swapchain's present mode (vk_main.cpp create_swapchain), as plain code: which of the modes a surface lists is asked
   for, by the Insert menu's VSync row and the two switches. No Vulkan: tests/test_vsync_row.py checks it.

   VSync on, the default, is the choice from before the row. Mailbox where the driver has it: the newest finished frame at
   each refresh of the display, no tearing, and nothing held back (the game paces itself, host/nn_vi.c). Without mailbox,
   immediate on Windows (AMD's driver has no mailbox, and FIFO held anything under 60 fps at 30) and FIFO elsewhere
   (immediate on NVIDIA's X11 path turns vblank off for every output, so other monitors and programs tear).

   VSync off asks for immediate wherever the surface lists it: a frame is on screen the moment it is presented, up to a
   refresh sooner, and torn where that falls inside a refresh. It lifts no limit: the frame rate is the game's own cap
   either way. Where On is immediate already (Windows without mailbox) Off changes nothing, and where the surface lists
   no immediate Off is On.

   RX_VK_FIFO (set) forces FIFO, and RX_VK_IMMEDIATE (set) asks for immediate, over the row, as before it. */
#pragma once

namespace mkwvk::present_mode {

enum Mode { kFifo = 0, kMailbox = 1, kImmediate = 2 };

/// The mode to ask for. mailbox, immediate: the surface lists them (FIFO always is there). vsync: the VSync row.
/// windows: a Windows build. force_fifo, want_immediate: RX_VK_FIFO and RX_VK_IMMEDIATE are set
inline Mode choose(bool mailbox, bool immediate, bool vsync, bool windows, bool force_fifo, bool want_immediate) {
    if (force_fifo) return kFifo;
    const Mode waits = mailbox ? kMailbox : kFifo;
    const bool at_once = want_immediate || !vsync || (windows && waits == kFifo);
    return at_once && immediate ? kImmediate : waits;
}

inline const char* name(int mode) { return mode == kMailbox ? "mailbox" : mode == kImmediate ? "immediate" : "fifo"; }

} // namespace mkwvk::present_mode
