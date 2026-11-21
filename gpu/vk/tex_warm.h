// (Triforce, 11-19) Textures made and uploaded ahead, while the frames are light (vk_texture.cpp texture_warm_step,
// RX_VK_TEX_WARM=1; off unless set), apart from Vulkan so that tests/test_tex_warm.py can run the rules.
//
// A texture is made and its bytes uploaded where a draw first samples it, on the thread that replays, and a frame's uploads
// are held to a budget (12 ms, 64 MB) with the picture held meanwhile. That budget applies only to draws into targets the
// game drew in the frame before (redrawn.h: a text box composed once must not keep the dummy). At an area's first frame the
// scene's targets are new, so nothing is left for later, and that one frame takes every texture of the area: in six
// as-shipped tours of ten areas (lab rounds 52 to 65) the 74 frames over 80 ms were 54% texture looks and uploads, with
// 350 to 800 uploads of 250 to 390 MB in the worst of them (100 to 180 ms each).
//
// The game registers an area's textures while its loading screen shows, and those frames are nearly empty (a few draws, 4 ms
// each). Here every registered slot is noted, and at the start of a submission whose frame before was light the noted
// textures are made and uploaded, oldest first, up to a budget a frame. The area's first frame then finds them made. A
// texture whose bytes the game writes after that is uploaded again where the CPU wrote, by the rules there were (the write
// marks), so what is drawn is what it was; a texture registered and never drawn is let go by the sweep as any other.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace tex_warm {

/// RX_VK_TEX_WARM: on when set to something other than 0
inline bool on_of(const char* env) { return env && *env && std::atoi(env) != 0; }

inline constexpr int64_t kLightWork = 400;        // (the draws and dispatches of the frame before: under it, a loading screen or a menu)
inline constexpr int64_t kBudgetNs = 6'000'000;   // (a light frame's uploads ahead, and the time looked for them)
inline constexpr uint64_t kAgeFrames = 8;         // (a slot is taken this many frames after it was registered: its bytes may still be coming)
inline constexpr size_t kMaxPending = 16384;      // (noted slots; more are not noted, and are made at their first draw as before)
inline constexpr uint64_t kRoomPct = 40;          // (made ahead only while the card's memory in use, or the textures' share of their
                                                  // budget, is under this: the game registers far more than a view draws, and a
                                                  // stand in the lab made 6,200 textures ahead, 9.5 GB for the 4 GB it draws from)

/// Whether a submission's start may make and upload noted textures: the switch, a light frame before, and this frame's
/// uploads and its looks ahead so far (a frame has several submissions: one budget for them all) under the budget
inline bool may_warm(bool on, int64_t last_frame_work, int64_t frame_upload_ns, int64_t frame_spent_ns) {
    return on && last_frame_work < kLightWork && frame_upload_ns < kBudgetNs && frame_spent_ns < kBudgetNs;
}
/// Whether there is room to make textures nothing has drawn yet. known: the card says what is in use (used of allowed);
/// else the textures' own bytes of their budget (0 or all ones: none is set, no room is known)
inline bool has_room(bool known, uint64_t used, uint64_t allowed, uint64_t texture_bytes, uint64_t budget) {
    if (known && allowed) return used / 100 * 100 < allowed / 100 * kRoomPct;
    if (!budget || budget == ~0ull) return false;
    return texture_bytes < budget / 100 * kRoomPct;
}
/// Whether a slot noted in `noted` is old enough in `frame`
inline bool ripe(uint64_t noted, uint64_t frame) { return frame >= noted + kAgeFrames; }
/// Whether the look goes on after a texture: neither the time spent nor the frame's uploads took the budget
inline bool goes_on(int64_t spent_ns, int64_t frame_upload_ns) { return spent_ns < kBudgetNs && frame_upload_ns < kBudgetNs; }

} // namespace tex_warm
