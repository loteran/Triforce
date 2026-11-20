// prepare_threads::count (gpu/vk/prepare_threads.h): the workers of the shader preparation before the title.
// tests/test_prepare_threads.py
#include "../gpu/vk/prepare_threads.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

int main() {
    const uint64_t GB = 1ull << 30;
    using prepare_threads::count;
    using prepare_threads::half;
    // (what it was: half the processors, 1 to 32)
    expect(half(1) == 1 && half(2) == 1 && half(12) == 6 && half(24) == 12 && half(64) == 32 && half(128) == 32, "half, as before");
    // (a player's PC: normal priority, RAM free at the start)
    expect(count(12, 24 * GB, false, 0) == 12, "12 processors, 24 GB free: all 12");
    expect(count(24, 26 * GB, false, 0) == 24, "24 processors, 26 GB free: all 24");
    expect(count(1, 8 * GB, false, 0) == 1 && count(2, 8 * GB, false, 0) == 2, "one and two processors");
    expect(count(128, 256 * GB, false, 0) == 64, "64 at most");
    // (1 GB free a worker past half)
    expect(count(24, 16 * GB, false, 0) == 16, "24 processors, 16 GB free: 16");
    expect(count(32, 11 * GB, false, 0) == 16, "32 processors, 11 GB free: half, 16, not fewer");
    expect(count(16, 9 * GB + GB / 2, false, 0) == 9, "16 processors, 9.5 GB free: 9");
    expect(count(16, 3 * GB, false, 0) == 8 && count(16, GB / 2, false, 0) == 8, "little RAM free: half, as before, never fewer");
    for (unsigned cpus = 1; cpus <= 256; cpus++)
        for (uint64_t ram = 0; ram <= 80 * GB; ram += GB / 2) {
            const unsigned n = count(cpus, ram, false, 0);
            if (n < half(cpus) || n > cpus || n > 64 || (n > half(cpus) && (uint64_t)n * GB > ram)) {
                std::printf("FAIL: %u processors, %llu MB free: %u workers\n", cpus, (unsigned long long)(ram >> 20), n);
                failures++;
            }
        }
    // (the RAM not known: as before)
    expect(count(24, 0, false, 0) == 12, "RAM not known: half");
    // (a process started above normal priority, a lab run at High: half, as before, whatever the RAM)
    expect(count(24, 64 * GB, true, 0) == 12 && count(8, 64 * GB, true, 0) == 4, "above normal priority: half");
    // (RX_VK_PREPARE_THREADS)
    expect(count(24, 2 * GB, true, 20) == 20, "the switch decides, whatever the RAM and the priority");
    expect(count(4, 64 * GB, false, 1) == 1, "the switch: one");
    expect(count(8, 64 * GB, false, 1000) == 64, "the switch: 64 at most");
    expect(count(24, 26 * GB, false, -3) == 24 && count(24, 26 * GB, false, 0) == 24, "the switch not set, or not a count: the rule");
    if (failures) return EXIT_FAILURE;
    std::printf("prepare threads: all checks passed\n");
    return EXIT_SUCCESS;
}
