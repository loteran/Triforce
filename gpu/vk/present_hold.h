// (Triforce) Hold the last picture while new textures come in (vk_replay.cpp do_present), apart from Vulkan so that
// tests/test_present_hold.py can run it.
//
// At an area change one frame took every new texture's upload at once: 523 ms for 119 uploads of 363 MB in the owner's test5
// session, 107 and 164 ms for some 400 in others. New textures now have a budget a frame too (vk_texture.cpp
// new_texture_later): once a frame's uploads took it, a new texture a draw samples is left for the next frame, and the draw
// samples the dummy in its place. Asked what should show meanwhile, the owner chose "hold the last picture": a frame drawn
// with any texture still owed its first bytes is not shown, the window gets the last complete picture again, and the game
// goes on stepping underneath. This is the rule of when a frame is held and when the hold ends:
//
//   - a frame drawn with every texture it sampled complete is shown, and ends a hold;
//   - a frame drawn with a texture owed its first bytes is held, while the hold is younger than its bound (RX_VK_HOLD_MS,
//     250 ms by default);
//   - once a hold reaches its bound no texture is left for later any more (the flush) and the frame after that is shown
//     whatever it holds, which ends the hold: a picture is never held for longer than the bound and the frame after it;
//   - after a hold that reached its bound no hold begins, and nothing is left for later, for a rest of 120 presents (2 s at
//     60), twice as long after each such hold that follows (at most 1920), back to 120 once a hold ends by itself or the
//     presents since the rest's end are more than 600: a card that cannot keep its textures (one at its video memory
//     allowance, whose render targets are made again from memory frame after frame) gets its frames as before, not a frozen
//     picture every few seconds.
#pragma once

#include <algorithm>
#include <cstdint>

namespace present_hold {

/// What the draws of the next frame may do with a new texture once the frame's uploads took their budget (vk_texture.cpp)
enum Mode {
    AS_BEFORE = 0,   // no hold: as before the hold (a new ASTC texture's own rule, upload_defer, draws the placeholder)
    DEFERS = 1,      // left for the next frame (the frame is then held)
    FLUSH = 2,       // the hold's bound: none left, every upload now
};

inline constexpr uint64_t kRest = 120, kRestMax = 1920, kQuiet = 600;

struct State {
    bool holding = false;
    int64_t since_ns = 0;          // (the first held present's time)
    int64_t frames = 0;            // (this hold's frames so far)
    bool flush = false;            // (this hold reached its bound)
    uint64_t rest_until = 0;       // (no hold before this present)
    uint64_t rest = kRest;
    // (counts, for the log: all of the session)
    int64_t holds = 0, held_frames = 0, held_ns = 0, longest_ns = 0, bounded = 0;
    // (the hold that ended at the last decide, for its log line: frames, time, and whether at the bound; 0 frames: none)
    int64_t ended_frames = 0, ended_ns = 0;
    bool ended_bounded = false;
};

/// What the draws of the frame after present `present` may do (bound_ns 0: the hold is off)
inline Mode mode(const State& s, int64_t bound_ns, uint64_t present) {
    if (bound_ns <= 0) return AS_BEFORE;
    if (s.flush) return FLUSH;
    return present >= s.rest_until ? DEFERS : AS_BEFORE;
}

/// The frame at present `present` (counted from 1), at `now_ns`: drawn with a texture owed its first bytes (owed) or with
/// every one complete. true: it is held (the window keeps the picture before it), false: it is shown
inline bool decide(State& s, int64_t bound_ns, bool owed, int64_t now_ns, uint64_t present) {
    s.ended_frames = 0, s.ended_ns = 0, s.ended_bounded = false;
    if (bound_ns <= 0 && !s.holding) return false;
    if (s.holding) {
        const int64_t held = std::max<int64_t>(0, now_ns - s.since_ns);
        if (!owed || s.flush || bound_ns <= 0) {   // (complete, or the frame after the bound: shown, the hold over)
            s.holding = false;
            s.held_ns += held, s.longest_ns = std::max(s.longest_ns, held);
            s.ended_frames = s.frames, s.ended_ns = held, s.ended_bounded = s.flush;
            if (s.flush) {
                s.bounded++;
                s.rest_until = present + s.rest;
                s.rest = std::min(s.rest * 2, kRestMax);
            } else {
                s.rest = kRest;
            }
            s.flush = false;
            return false;
        }
        if (held >= bound_ns) s.flush = true;   // (this one still held; the next drawn with every upload made)
        s.frames++, s.held_frames++;
        return true;
    }
    if (!owed || present < s.rest_until) return false;   // (complete, or resting: shown as it is)
    if (present >= s.rest_until + kQuiet) s.rest = kRest;
    s.holding = true, s.since_ns = now_ns, s.frames = 1, s.flush = false;
    s.holds++, s.held_frames++;
    return true;
}

}   // namespace present_hold
