// tests/test_present_hold.py: gpu/vk/present_hold.h, the rule of when a frame is held (the last picture shown again) and when
// the hold ends, as it is (no Vulkan, no game)
#include "present_hold.h"

#include <cstdio>
#include <cstring>
#include <string>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s: failed at line %d: %s\n", name.c_str(), __LINE__, #x); return 1; } } while (0)

namespace ph = present_hold;
constexpr int64_t kMs = 1000000;
constexpr int64_t kBound = 250 * kMs;
constexpr int64_t kFrame = 16666667;   // (60 presents a second)

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "";
    ph::State s;
    uint64_t present = 1;
    int64_t now = 1000 * kMs;
    // (one frame at the present after the last: its draws owed a texture its first bytes or not; true: held)
    auto frame = [&](bool owed, int64_t bound = kBound) {
        const bool held = ph::decide(s, bound, owed, now, present);
        present++, now += kFrame;
        return held;
    };
    if (name == "off") {   // RX_VK_HOLD_MS=0: nothing held, nothing left for later
        for (int i = 0; i < 100; i++) CHECK(!frame(i % 2 == 0, 0));
        CHECK(ph::mode(s, 0, present) == ph::AS_BEFORE && s.holds == 0 && s.held_frames == 0);
    } else if (name == "held-until-complete") {
        // a frame drawn with a texture owed its first bytes is held, those after it too, until one is drawn complete
        CHECK(ph::mode(s, kBound, present) == ph::DEFERS);
        CHECK(!frame(false));
        CHECK(frame(true) && s.holding && s.holds == 1);
        CHECK(ph::mode(s, kBound, present) == ph::DEFERS);
        CHECK(frame(true) && frame(true));
        CHECK(!frame(false) && !s.holding);   // (complete: shown, the hold over)
        CHECK(s.ended_frames == 3 && s.ended_ns == 3 * kFrame && !s.ended_bounded);
        CHECK(s.held_frames == 3 && s.held_ns == 3 * kFrame && s.longest_ns == 3 * kFrame && s.bounded == 0);
        CHECK(!frame(false) && s.ended_frames == 0);   // (said once)
        CHECK(ph::mode(s, kBound, present) == ph::DEFERS);   // (no rest after a hold that ended by itself)
        CHECK(frame(true) && s.holds == 2);
    } else if (name == "bound") {
        // the bound: a picture is held at most RX_VK_HOLD_MS and one frame; at the bound nothing is left for later (the flush),
        // and the frame after it is shown whatever it holds
        int held = 0;
        while (frame(true)) {
            held++;
            CHECK(held < 40);
        }
        // (frames from 0 to 250 ms are held: the one at 250 ms too, with the flush; the next is shown)
        CHECK(held == 16 && s.bounded == 1 && s.ended_bounded && s.ended_frames == 16);
        CHECK(s.ended_ns == 16 * kFrame && s.ended_ns < kBound + 2 * kFrame && !s.holding && !s.flush);
        // (then a rest of 120 presents: no hold, and the draws do as before the hold; the frames are shown as they are)
        CHECK(s.rest_until == present - 1 + ph::kRest);
        CHECK(ph::mode(s, kBound, present) == ph::AS_BEFORE);
        for (uint64_t i = 1; i < ph::kRest; i++) CHECK(!frame(true));
        CHECK(ph::mode(s, kBound, present) == ph::DEFERS && frame(true));
    } else if (name == "flush") {
        // the frame that reaches the bound is still held, and the mode of the next one's draws is the flush
        CHECK(frame(true));
        now += kBound;
        CHECK(frame(true) && s.flush && ph::mode(s, kBound, present) == ph::FLUSH);
        CHECK(!frame(false) && s.bounded == 1 && ph::mode(s, kBound, present) == ph::AS_BEFORE);
    } else if (name == "rest-doubles") {
        // a hold at its bound after another: the rest doubles, up to kRestMax; a hold that ends by itself, or a quiet stretch,
        // brings it back to kRest
        uint64_t want = ph::kRest;
        for (int k = 0; k < 7; k++) {
            while (frame(true)) {}
            CHECK(s.rest_until == present - 1 + want);
            want = std::min(want * 2, ph::kRestMax);
            while (present < s.rest_until) CHECK(!frame(true));
        }
        CHECK(s.rest == ph::kRestMax && s.bounded == 7);
        CHECK(frame(true) && !frame(false) && s.rest == ph::kRest);   // (ended by itself)
        while (frame(true)) {}
        CHECK(s.rest == 2 * ph::kRest);
        present = s.rest_until + ph::kQuiet;   // (a quiet stretch: the next bounded hold rests the shortest time)
        while (frame(true)) {}
        CHECK(s.rest_until == present - 1 + ph::kRest);
    } else if (name == "never-frozen") {
        // a card whose every frame owes a texture its first bytes (one at its video memory allowance): over a minute at 60
        // presents a second no picture is held longer than the bound and the frame after it, and nearly every frame is shown
        int64_t run = 0, longest = 0, held = 0;
        for (int i = 0; i < 3600; i++) {
            if (frame(true)) held++, run++;
            else run = 0;
            longest = std::max(longest, run);
        }
        CHECK(longest * kFrame < kBound + 2 * kFrame);
        CHECK(held * 100 < 3600 * 5);   // (under 5% of the frames)
        CHECK(s.longest_ns < kBound + 2 * kFrame);
    } else if (name == "resting-owed") {
        // while resting a frame that still owes a texture (a new ASTC one's own rule, as before the hold) is shown
        while (frame(true)) {}
        CHECK(!frame(true) && !s.holding && s.holds == 1);
    } else if (name == "off-while-holding") {
        // the bound turned off under a hold (it is read once a run, but the rule does not hang on it): the hold ends at once
        CHECK(frame(true));
        CHECK(!frame(true, 0) && !s.holding);
    } else {
        std::fprintf(stderr, "unknown case %s\n", name.c_str());
        return 2;
    }
    return 0;
}
