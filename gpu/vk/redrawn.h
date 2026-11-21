// (Triforce, 11-18) Whether a render target is drawn again frame after frame, for the new-texture budget (vk_texture.cpp
// create_later, new_texture_later, new_texture_waits, the layered ASTC split, upload_defer, upload_again_later). The budget
// leaves a frame's textures for the next frame, the draw samples the dummy (or a texture's last bytes) in their place and the
// present hold hides that frame: right for a target the game draws again in the next frame (the scene), wrong for one it draws
// once and keeps. Ocarina of Time composes each text box once (its CPU-written R8 line textures, 256x46 and 640x46, drawn into a
// 262x104 .. 656x113 R8 target whose result it samples for as long as the text stands): a compose in a frame over the budget kept
// the dummy for the whole session, the title's buttons, the options' values and dialogue boxes empty or partial (#bug-tracker
// "missing text from main menu and settings", "Menu buttons don't appear unless selected", "Title Screen invisible buttons",
// "Text is invisible sometimes"; found and tested by Kaefergeneral). So a draw may leave a texture for later only when every
// target it writes was written in the frame before too and its program writes no memory (vk_draw.cpp draw: draw_may_defer).
//
// A target's frames: last_written, the frame the GPU last wrote it, and prev_written, the frame it was written in before the
// frame of last_written (0: none, or its contents began anew then). begin_pass marks a pass's targets written before the second
// and later draws into them build their descriptors, so last_written alone would make a box composed for the first time look
// drawn before from its second draw on: prev_written tells them apart.
#pragma once
#include <cstdint>

namespace redrawn {
/// prev_written after a write in `frame` (live: the target held the GPU's contents before this write, not released since)
inline uint64_t prev_on_write(uint64_t prev, uint64_t last, bool live, uint64_t frame) {
    if (last == frame) return prev;   // (written already in this frame: the frame before stays what it was)
    return live ? last : 0;
}
/// ((Triforce, 11-19) How many frames back a write still counts as the target being drawn again. 1, v0.5.2's rule, made every
/// target the game draws every second or third frame (reflections, shadow cascades, the water's own passes) make and upload its
/// textures at once and skip the descriptors' reuse, every time: in a lab A/B with the benchmark Lake Hylia's average fell from
/// 59.6 to 44.6 fps and its 1 % low from 43.3 to 28.3, Castle Town's from 45.5 to 40.5, and the first visits had 250 and 900 ms
/// frames (#bug-tracker "Full Stuttering with last Update 0.5.2", "1% low 17.6 fps", "Stuttering & Flickering 0.5.2"). A text box
/// is composed once and left: its first compose is never drawn before, whatever the window. RX_VK_REDRAWN_FRAMES=<n>: n (1: as
/// v0.5.2)
inline constexpr uint64_t kWindow = 4;

/// Whether a target was written in the `window` frames before `frame` (live: as above, now)
inline bool drawn_before(uint64_t prev, uint64_t last, bool live, uint64_t frame, uint64_t window = kWindow) {
    if (!live || frame == 0) return false;
    if (window < 1) window = 1;
    if (last == frame) return prev != 0 && prev + window >= frame;   // (already marked in this frame: the run before it)
    return last < frame && last + window >= frame;
}
}  // namespace redrawn
