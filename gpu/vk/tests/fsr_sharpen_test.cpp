// Game-free regression for the FSR sharpening row's strength (gpu/vk/fsr_sharpen.h): off at 0, the row's percent as FSR's sharpness, and
// raised towards 1 below three quarters of the output's width.
#include <cmath>
#include <cstdio>

#include "../fsr_sharpen.h"

namespace {
int failures = 0;
void check(const char* name, bool ok) {
    if (ok) return;
    std::fprintf(stderr, "FAIL %s\n", name);
    failures++;
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
namespace fs = mkwvk::fsr_sharpen;
}  // namespace

int main() {
    check("0 is off", !fs::enabled(0) && !fs::enabled(-5));
    check("1 and up is on", fs::enabled(1) && fs::enabled(100));
    check("off gives 0 at any size", fs::strength(0, 960, 1920) == 0.0f && fs::strength(-3, 640, 1920) == 0.0f);
    // the boost: none from 3/4 up, all from 1/2 down, linear between
    check("no boost at full size", fs::boost(1920, 1920) == 0.0f);
    check("a third of it at Quality (67%)", near(fs::boost(1280, 1920), 1.0f / 3.0f));
    check("none at 75% (the old 1.33x mode)", fs::boost(1440, 1920) == 0.0f);
    check("no boost at exactly 3/4", fs::boost(1440, 1920) == 0.0f);
    check("half way at 5/8", near(fs::boost(1200, 1920), 0.5f));
    check("all of it at Performance", fs::boost(960, 1920) == 1.0f);
    check("all of it below Performance", fs::boost(640, 1920) == 1.0f);
    check("a render wider than the output has none", fs::boost(2560, 1920) == 0.0f);
    check("an unknown output has none", fs::boost(960, 0) == 0.0f);
    // the strength
    check("the percent is FSR's sharpness at full size", near(fs::strength(40, 1920, 1920), 0.4f));
    check("Quality raises it a third of the way to 1", near(fs::strength(40, 1280, 1920), 0.6f));
    check("Performance raises it to 1", near(fs::strength(40, 960, 1920), 1.0f));
    check("half way raises it half way to 1", near(fs::strength(40, 1200, 1920), 0.7f));
    check("100 stays 1", near(fs::strength(100, 1920, 1920), 1.0f) && near(fs::strength(100, 960, 1920), 1.0f));
    check("past 100 is clamped", near(fs::strength(250, 1920, 1920), 1.0f));
    check("never past 1", fs::strength(100, 100, 1920) <= 1.0f);
    check("a little is a little", near(fs::strength(5, 1920, 1920), 0.05f));
    if (failures) return 1;
    std::printf("fsr_sharpen_test: all passed\n");
    return 0;
}
