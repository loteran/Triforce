// tests/test_demote_watch.py: gpu/vk/demote_watch.h, when a pool's trial in guest memory ends early
#include "../gpu/vk/demote_watch.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

namespace dw = mkwvk::demote_watch;

/// A trial's frames as demote_trials sees them: each frame costs ms_trial of GPU time from its start, and the GPU's time comes
/// in `late` frames after the frame (submissions finish later than they are recorded). The frame at which gross() ends it, 0:
/// never within `frames`
static int ends_at(double base_ms, double ms_trial, int late, int frames) {
    for (int f = 1; f <= frames; f++) {
        const int counted = f - late > 0 ? f - late : 0;
        const double so_far = counted * ms_trial / f;
        if (dw::gross(base_ms, so_far, (uint64_t)f)) return f;
    }
    return 0;
}

int main() {
    // the four costly trials of the owner's logs (after, before): ended
    CHECK("106 against 7.37", dw::gross(7.37, 106.24, 10));
    CHECK("104.5 against 5.49", dw::gross(5.49, 104.52, 10));
    CHECK("39.4 against 7.70", dw::gross(7.70, 39.39, 10));
    CHECK("36.6 against 7.73", dw::gross(7.73, 36.58, 10));
    // the slowest of the others, measured to the end as before: 20.67 against 11.53, 12.48 against 5.45, and the one at 3.6
    // times whose baseline was off (10.82 mirrored again too: it stayed in guest memory)
    CHECK("20.7 against 11.5 is not", !dw::gross(11.53, 20.67, 10));
    CHECK("12.5 against 5.45 is not", !dw::gross(5.45, 12.48, 10));
    CHECK("10.8 against 3.02 is not", !dw::gross(3.02, 10.82, 10));
    CHECK("a trial as fast as before is not", !dw::gross(7.0, 7.1, 50));
    // not before kFrames frames, and never without a baseline
    CHECK("two frames are too few", !dw::gross(7.0, 200.0, 2));
    CHECK("three are enough", dw::gross(7.0, 200.0, 3));
    CHECK("no baseline", !dw::gross(0.0, 200.0, 10) && !dw::gross(-1.0, 200.0, 10));
    // the line itself: over 3 times and 5 ms more
    CHECK("just over", dw::gross(10.0, 35.01, 3));
    CHECK("at it", !dw::gross(10.0, 35.0, 3));

    // as the frames come in: the 106 ms pool ends within a few frames, not after 10 settling and 10 measured
    const int a = ends_at(7.37, 106.24, 2, 20);
    std::printf("106 ms a frame against 7.37, the GPU's time 2 frames late: ends at frame %d\n", a);
    CHECK("the 106 ms trial ends by frame 4", a >= 3 && a <= 4);
    const int b = ends_at(7.70, 39.39, 2, 20);
    std::printf("39.4 ms a frame against 7.70, 2 frames late: ends at frame %d\n", b);
    CHECK("the 39 ms trial ends by frame 8", b >= 3 && b <= 8);
    CHECK("the 20.7 ms trial runs on", ends_at(11.53, 20.67, 0, 90) == 0);
    CHECK("one slow first frame of 30 ms against 7 does not end a trial", [] {
        for (int f = 1; f <= 20; f++)
            if (dw::gross(7.0, (30.0 + 7.0 * (f - 1)) / f, (uint64_t)f)) return false;
        return true;
    }());

    // RX_VK_DEMOTE_QUICK: on unless 0
    CHECK("unset: on", dw::quick_on(nullptr) && dw::quick_on(""));
    CHECK("1: on", dw::quick_on("1"));
    CHECK("0: off", !dw::quick_on("0"));
    CHECK("a word is not 0", dw::quick_on("off?"));
    std::puts("demote watch: ok");
    return 0;
}
