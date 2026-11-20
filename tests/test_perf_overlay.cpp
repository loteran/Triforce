// (Triforce) gpu/vk/perf_stats.h, the performance overlay's frame times: the ring, the newest frames for the graph, the
// percentiles and the 1% and 0.1% lows, on authored frame times, and the figures per frame (per_frame: the first sample's and
// a fresh one's "-", the base that moves on, a sample with no frame, pipelines and translations apart). And
// gpu/vk/perf_machine.h/.cpp, its machine numbers: the CPU's load from two readings, the GPU's from Windows' engine counters,
// the card's memory from its adapter counters and from Vulkan's device-local heaps, a number's text or its "-", the vendor
// libraries' name match, and the sampler itself run for a GPU of no vendor's name (the numbers only NVIDIA's or AMD's library
// gives stay -1, as on an Intel card): one thread however often it is started, ended when stopped. No GPU, no game. Built and
// run by tests/test_perf_overlay.py.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "perf_machine.h"
#include "perf_stats.h"

using namespace mkwvk::perf_stats;
namespace pm = mkwvk::perf_machine;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

constexpr int64_t MS = 1000000;

int main() {
    auto ring = std::make_unique<Ring>();
    Ring& r = *ring;
    float out[4096];
    int64_t t = 5000 * MS;

    // nothing yet
    Summary s = summarize(r);
    CHECK(s.frames == 0 && s.low1_fps < 0 && s.fps == 0 && held(r) == 0 && newest(r, out, 240) == 0, "an empty ring says nothing");
    // the first frame only starts the first interval
    note(r, t);
    CHECK(held(r) == 0, "one frame is no frame time");
    // 300 frames at 60 a second
    for (int i = 0; i < 300; i++) note(r, t += 16666667);
    s = summarize(r);
    CHECK(s.frames == 300 && near(s.avg_ms, 16.667f) && near(s.p50_ms, 16.667f) && near(s.p99_ms, 16.667f) && near(s.max_ms, 16.667f),
          "60 fps: %d frames, mean %.3f, median %.3f, 99%% %.3f, longest %.3f", s.frames, s.avg_ms, s.p50_ms, s.p99_ms, s.max_ms);
    CHECK(near(s.fps, 60.0f, 0.05f) && near(s.low1_fps, 60.0f, 0.05f), "60 fps and a 1%% low of 60: %.2f, %.2f", s.fps, s.low1_fps);
    // the summary's window: the frames of the last ten seconds, by their own times
    r = Ring{};
    note(r, t += 3000 * MS);
    for (int i = 0; i < 250; i++) note(r, t += 100 * MS);
    s = summarize(r);
    CHECK(s.frames == 100 && near(s.avg_ms, 100.0f) && near(s.fps, 10.0f), "ten seconds of 100 ms frames are 100 of the 250 (%d)", s.frames);
    CHECK(summarize(r, 250 * MS).frames == 2 && summarize(r, 1).frames == 1, "a shorter window, and at least one frame");

    // percentiles by the nearest rank
    {
        const uint32_t v[] = {10, 20, 30, 40};
        CHECK(percentile(v, 4, 0.5) == 20 && percentile(v, 4, 0.51) == 30 && percentile(v, 4, 0.75) == 30 && percentile(v, 4, 0.99) == 40 &&
              percentile(v, 4, 1.0) == 40 && percentile(v, 4, 0.0) == 10 && percentile(v, 1, 0.99) == 10 && percentile(v, 0, 0.5) == 0,
              "the nearest rank");
    }
    // one slow frame in a hundred is not the 1%% low's (99 of 100 took no longer than the fast ones); two are
    r = Ring{};
    note(r, t += 3000 * MS);
    for (int i = 0; i < 100; i++) note(r, t += (i == 40 ? 50 : 10) * MS);
    s = summarize(r);
    CHECK(s.frames == 100 && near(s.p99_ms, 10.0f) && near(s.low1_fps, 100.0f, 0.05f) && near(s.max_ms, 50.0f) && near(s.p50_ms, 10.0f),
          "one slow frame of 100: 99%% %.2f, low %.2f, longest %.2f", s.p99_ms, s.low1_fps, s.max_ms);
    CHECK(near(s.avg_ms, 10.4f) && near(s.fps, 96.15f, 0.05f), "the mean counts it: %.3f ms, %.2f fps", s.avg_ms, s.fps);
    r = Ring{};
    note(r, t += 3000 * MS);
    for (int i = 0; i < 100; i++) note(r, t += (i == 40 || i == 70 ? 50 : 10) * MS);
    s = summarize(r);
    CHECK(near(s.p99_ms, 50.0f) && near(s.low1_fps, 20.0f, 0.05f), "two slow frames of 100: 99%% %.2f, low %.2f", s.p99_ms, s.low1_fps);
    // under a hundred frames there is no 1%% low yet
    r = Ring{};
    note(r, t += 3000 * MS);
    for (int i = 0; i < 99; i++) note(r, t += 10 * MS);
    s = summarize(r);
    CHECK(s.frames == 99 && s.low1_fps < 0 && near(s.p99_ms, 10.0f), "99 frames: no low (%.2f)", s.low1_fps);
    note(r, t += 10 * MS);
    CHECK(summarize(r).low1_fps > 99, "the hundredth gives it");

    // a load or a pause is no frame time, nor a clock that stands still or goes back; the frames after count again
    r = Ring{};
    note(r, t += 3000 * MS);
    note(r, t += 10 * MS);
    note(r, t += 1001 * MS);   // (held over a second)
    CHECK(held(r) == 1, "a gap is not written");
    note(r, t += 1000 * MS);   // (a second exactly is still a frame)
    CHECK(held(r) == 2 && near(summarize(r).max_ms, 1000.0f), "a frame of a second");
    note(r, t);
    note(r, t - 5 * MS);
    CHECK(held(r) == 2, "a clock that did not move on");
    note(r, t += 5 * MS);
    CHECK(held(r) == 3 && newest(r, out, 1) == 1 && near(out[0], 10.0f), "the next interval starts at the newest time given (%.2f)", out[0]);

    // the graph: the newest n, the oldest of them first, across the ring's end
    r = Ring{};
    note(r, t += 3000 * MS);
    for (int i = 1; i <= 5000; i++) note(r, t += (100 + i) * 1000);   // (frame i takes 100 microseconds and i more)
    CHECK(held(r) == kFrames && r.count == 5000, "the ring holds its %d newest", kFrames);
    int m = newest(r, out, 240);
    bool ok = m == 240;
    for (int i = 0; i < m; i++) ok = ok && near(out[i], (100 + 5000 - 239 + i) / 1000.0f, 0.0005f);
    CHECK(ok, "the newest 240, in order, the last one the newest (%.3f to %.3f)", out[0], out[239]);
    m = newest(r, out, 4096);
    CHECK(m == kFrames && near(out[0], (100 + 5000 - kFrames + 1) / 1000.0f, 0.0005f) && near(out[m - 1], 5.1f, 0.0005f), "no more than it holds");
    CHECK(newest(r, out, 0) == 0 && newest(r, out, -3) == 0, "none asked for");
    s = summarize(r);
    CHECK(s.frames == kFrames && near(s.max_ms, 5.1f, 0.0005f), "all of it inside ten seconds (%d)", s.frames);
    // the count's own end, far off as it is (2^32 frames): the ring's places go on in order
    r = Ring{};
    r.count = 0xfffffff0u;
    note(r, t += 3000 * MS);
    for (int i = 1; i <= 32; i++) note(r, t += i * MS);
    m = newest(r, out, 16);
    ok = m == 16;
    for (int i = 0; i < m; i++) ok = ok && near(out[i], (float)(17 + i));
    CHECK(ok && r.count == 16, "past the count's end the newest are still the newest");

    // ((Triforce: from NX2navi's perfmon.c) the 0.1% low: the frame rate of the 99.9th percentile, from 100 frames on as the 1%
    // low; under 1000 frames that is the longest)
    r = Ring{};
    note(r, t += 3000 * MS);
    for (int i = 0; i < 300; i++) note(r, t += 16666667);
    s = summarize(r);
    CHECK(near(s.low01_fps, 60.0f, 0.05f), "60 fps: a 0.1%% low of 60 (%.2f)", s.low01_fps);
    r = Ring{};
    note(r, t += 3000 * MS);
    for (int i = 0; i < 99; i++) note(r, t += (i == 40 ? 50 : 10) * MS);
    CHECK(summarize(r).low01_fps < 0, "99 frames: no 0.1%% low");
    note(r, t += 10 * MS);
    s = summarize(r);
    CHECK(near(s.low01_fps, 20.0f, 0.05f) && near(s.low1_fps, 100.0f, 0.05f), "one slow frame of 100 is the 0.1%% low's, not the 1%%'s: %.2f, %.2f",
          s.low01_fps, s.low1_fps);
    r = Ring{};
    note(r, t += 3000 * MS);
    for (int i = 0; i < 1500; i++) note(r, t += (i == 700 ? 50 : 5) * MS);
    s = summarize(r);
    CHECK(s.frames == 1500 && near(s.low01_fps, 200.0f, 0.1f), "one slow frame of 1500 is not the 0.1%% low's: %.2f", s.low01_fps);
    r = Ring{};
    note(r, t += 3000 * MS);
    for (int i = 0; i < 1500; i++) note(r, t += (i == 700 || i == 900 ? 50 : 5) * MS);
    CHECK(near(summarize(r).low01_fps, 20.0f, 0.05f), "two of 1500 are (%.2f)", summarize(r).low01_fps);

    // ---- (Triforce) the figures per frame: perf_stats::per_frame, one sample's step ----
    {
        Base b;
        PerFrame f;
        Counts c;
        c.frames = 1000, c.prep_ns = 50 * MS, c.gpu_ns = 80 * MS, c.uploads = 40, c.pipe_us = 3000, c.shader_us = 9000;
        // the first sample: nothing to go from, every figure "-"; its counts are the next one's base
        f.prep_ms = 123;
        CHECK(per_frame(b, c, false, &f) == STEP_FIRST && f.prep_ms < 0 && f.gpu_ms < 0 && f.pipe_ms < 0 && f.shader_ms < 0 && f.uploads < 0,
              "the first sample: all \"-\"");
        CHECK(b.have && b.at.frames == 1000 && b.at.shader_us == 9000, "the first sample is the base");
        // 30 frames later: each count's change over 30, pipelines and translations apart
        c.frames += 30, c.prep_ns += 60 * MS, c.gpu_ns += 300 * MS, c.uploads += 15, c.pipe_us += 4500, c.shader_us += 1500;
        CHECK(per_frame(b, c, false, &f) == STEP_NEW, "frames since: new figures");
        CHECK(near((float)f.prep_ms, 2.0f) && near((float)f.gpu_ms, 10.0f) && near((float)f.uploads, 0.5f),
              "per frame: prep %.3f ms, GPU %.3f ms, uploads %.3f", f.prep_ms, f.gpu_ms, f.uploads);
        CHECK(near((float)f.pipe_ms, 0.15f, 0.0005f) && near((float)f.shader_ms, 0.05f, 0.0005f),
              "pipeline waits are their own, the translations' theirs: %.4f, %.4f ms", f.pipe_ms, f.shader_ms);
        // the next sample counts from this one, not from the first
        c.frames += 10, c.prep_ns += 10 * MS, c.gpu_ns += 20 * MS, c.uploads += 0, c.pipe_us += 0, c.shader_us += 2000;
        CHECK(per_frame(b, c, false, &f) == STEP_NEW && near((float)f.prep_ms, 1.0f) && near((float)f.gpu_ms, 2.0f) && near((float)f.uploads, 0.0f) &&
                  near((float)f.pipe_ms, 0.0f) && near((float)f.shader_ms, 0.2f, 0.0005f),
              "from the sample before (the base moves on): prep %.3f, GPU %.3f, uploads %.3f, pipe %.4f, shader %.4f", f.prep_ms, f.gpu_ms,
              f.uploads, f.pipe_ms, f.shader_ms);
        // no game frame since (a pause, a load): the figures as they were, the counts of the time between not kept for later
        const PerFrame kept = f;
        c.prep_ns += 500 * MS, c.uploads += 900, c.pipe_us += 90000;
        CHECK(per_frame(b, c, false, &f) == STEP_KEEP && f.prep_ms == kept.prep_ms && f.uploads == kept.uploads && f.pipe_ms == kept.pipe_ms,
              "no frame since: as they were");
        c.frames += 10, c.prep_ns += 30 * MS;
        CHECK(per_frame(b, c, false, &f) == STEP_NEW && near((float)f.prep_ms, 3.0f) && near((float)f.uploads, 0.0f) && near((float)f.pipe_ms, 0.0f),
              "what came while no frame did is not put on the next frames: prep %.3f, uploads %.3f", f.prep_ms, f.uploads);
        // a count that went back is 0; the GPU's time that did not move on (no timestamps) "-"
        c.frames += 5, c.uploads -= 100, c.prep_ns += 5 * MS;
        CHECK(per_frame(b, c, false, &f) == STEP_NEW && near((float)f.uploads, 0.0f) && f.gpu_ms < 0 && near((float)f.prep_ms, 1.0f),
              "a count gone back: 0 (%.2f); the GPU's time standing still: \"-\" (%.2f)", f.uploads, f.gpu_ms);
        // Full turned on again: the counts from before are of another time, all "-" again, then from this sample
        c.frames += 100, c.prep_ns += 9000 * MS;
        CHECK(per_frame(b, c, true, &f) == STEP_FIRST && f.prep_ms < 0 && f.uploads < 0, "fresh: all \"-\"");
        c.frames += 4, c.prep_ns += 8 * MS;
        CHECK(per_frame(b, c, false, &f) == STEP_NEW && near((float)f.prep_ms, 2.0f), "after fresh, from its counts (%.3f)", f.prep_ms);
    }

    // ---- perf_machine.h ----
    {   // the CPU's load: the busy share of the time that went by, and this process's share; none from a first reading
        pm::CpuTimes a, b;
        float total = 0, game = 0;
        b.idle = 500, b.kernel = 1000, b.user = 1000, b.proc = 300;
        pm::cpu_load(a, b, &total, &game);
        CHECK(total < 0 && game < 0, "a first reading has no load (%.1f, %.1f)", total, game);
        a = b;
        b.idle += 500, b.kernel += 600, b.user += 400, b.proc += 250;   // (1000 went by, 500 of it idle, 250 of it the game's)
        pm::cpu_load(a, b, &total, &game);
        CHECK(near(total, 50.0f) && near(game, 25.0f), "half busy, a quarter the game's: %.1f, %.1f", total, game);
        pm::cpu_load(b, b, &total, &game);
        CHECK(total < 0 && game < 0, "no time went by: none");
        pm::cpu_load(b, a, &total, &game);
        CHECK(total < 0 && game < 0, "a reading from before: none");
    }
    {   // the counters' adapter names, and the GPU's load as Task Manager adds it up: per engine type, the busiest, 100 at most
        const uint8_t luid[8] = {0x34, 0x12, 0, 0, 0x01, 0, 0, 0};   // (LowPart 0x1234, HighPart 1)
        char name[32];
        pm::luid_name(luid, name, sizeof name);
        CHECK(!strcmp(name, "luid_0x00000001_0x00001234"), "the LUID's name: %s", name);
        const char* names[] = {"pid_1_luid_0x00000001_0x00001234_phys_0_eng_0_engtype_3D",
                               "pid_2_luid_0x00000001_0x00001234_phys_0_eng_0_engtype_3D",
                               "pid_2_luid_0x00000001_0x00001234_phys_0_eng_3_engtype_Copy",
                               "pid_3_LUID_0x00000002_0x00005678_phys_0_eng_0_engtype_3D"};
        const double values[] = {30, 25, 40, 90};
        CHECK(near(pm::engine_load(names, values, 4, name), 55.0f), "the adapter's 3D engine, its processes added (%.1f)",
              pm::engine_load(names, values, 4, name));
        CHECK(near(pm::engine_load(names, values, 4, ""), 90.0f), "any adapter: the busiest");
        const double many[] = {80, 70, 0, 0};
        CHECK(near(pm::engine_load(names, many, 4, name), 100.0f), "100 at most");
        CHECK(pm::engine_load(names, values, 4, "luid_0x00000009_0x00000009") < 0 && pm::engine_load(names, values, 0, "") < 0,
              "no instance of the adapter: none");
        const char* mem_names[] = {"luid_0x00000001_0x00001234_phys_0", "luid_0x00000002_0x00005678_phys_0"};
        const double bytes[] = {3.0 * 1048576 * 1024, 5.0 * 1048576 * 1024};
        CHECK(near(pm::adapter_memory_mb(mem_names, bytes, 2, name), 3072.0f) && near(pm::adapter_memory_mb(mem_names, bytes, 2, ""), 5120.0f),
              "the adapter's dedicated memory, or the one that uses the most");
        CHECK(pm::adapter_memory_mb(mem_names, bytes, 2, "luid_0x00000009_0x00000009") < 0 && pm::adapter_memory_mb(mem_names, bytes, 0, "") < 0,
              "none: -1");
    }
    {   // a number's text, or "-" (dimmed) where there is none
        char c[32];
        CHECK(!pm::cell_text(c, sizeof c, "%.0f W", 186.4) && !strcmp(c, "186 W"), "a number: %s", c);
        CHECK(!pm::cell_text(c, sizeof c, "game %.1f GB", 0.0) && !strcmp(c, "game 0.0 GB"), "0 is a number: %s", c);
        CHECK(pm::cell_text(c, sizeof c, "%.0f \xC2\xB0" "C", -1) && !strcmp(c, "-"), "none: %s", c);
        // the card's memory from Vulkan's heaps: the device-local ones added up, not the system memory it can reach
        struct Heap { uint64_t size; uint32_t flags; };
        const uint32_t kLocal = 1;   // (VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        const Heap heaps[] = {{8ull << 30, kLocal}, {16ull << 30, 0}, {256ull << 20, kLocal | 2}, {4ull << 30, 2}};
        CHECK(near(pm::local_heap_mb(heaps, 4, kLocal), 8192.0f + 256.0f), "the device-local heaps alone: %.0f MB", pm::local_heap_mb(heaps, 4, kLocal));
        CHECK(pm::local_heap_mb(heaps + 1, 1, kLocal) < 0 && pm::local_heap_mb(heaps, 0, kLocal) < 0, "no device-local heap: -1");
        CHECK(pm::name_match("NVIDIA GeForce RTX 5090", "NVIDIA GeForce RTX 5090") && pm::name_match("AMD Radeon RX 7900 XTX", "Radeon RX 7900 XTX") &&
              !pm::name_match("Intel(R) Arc(TM) A770 Graphics", "NVIDIA GeForce RTX 4070") && !pm::name_match("", "") && !pm::name_match("x", ""),
              "the vendor libraries' devices by name");
    }
    {   // the sampler, run: for a GPU no vendor library has, the numbers only those give stay -1 (the panel's "-"); the system's
        // own are there; it stops, and starts again
        pm::Gpu g;
        snprintf(g.name, sizeof g.name, "Test Adapter With No Vendor Library");
        g.vram_total_mb = 1024;
        const auto wait_for = [](int samples) {
            pm::Machine m;
            for (int i = 0; i < 100 && (m = pm::latest()).samples < samples; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return m;
        };
        // (the sampler's thread has ended: live_samplers 0, waited for up to five seconds, no fixed sleep)
        const auto ended = [] {
            for (int i = 0; i < 500 && pm::live_samplers() > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return pm::live_samplers() == 0;
        };
        CHECK(pm::latest().samples == 0 && pm::latest().gpu_temp_c < 0 && pm::live_samplers() == 0, "nothing before it starts");
        pm::start(g, nullptr);
        pm::start(g, nullptr);   // (the row set to Full twice over: the one thread)
        CHECK(pm::live_samplers() == 1, "a second start while it runs makes no second thread (%d)", pm::live_samplers());
        pm::Machine m = wait_for(2);
        CHECK(pm::live_samplers() == 1, "still the one thread (%d)", pm::live_samplers());
        CHECK(m.samples >= 2, "two samples in five seconds (%d)", m.samples);
        CHECK(m.gpu_temp_c < 0 && m.gpu_clock_mhz < 0, "no vendor library has this GPU: no temperature or clock "
              "(%.0f, %.0f)", m.gpu_temp_c, m.gpu_clock_mhz);
        CHECK(near(m.vram_total_mb, 1024.0f), "the card's memory as Vulkan gave it (%.0f)", m.vram_total_mb);
        CHECK(!strcmp(m.gpu_name, g.name), "the GPU's name as given");
        CHECK(m.ram_total_mb > 0 && m.ram_used_mb > 0 && m.ram_game_mb > 0 && m.ram_used_mb <= m.ram_total_mb, "the memory: %.0f of %.0f, %.0f",
              m.ram_used_mb, m.ram_total_mb, m.ram_game_mb);
        CHECK(m.cpu_pct >= 0 && m.cpu_pct <= 100 && m.cpu_game_pct >= 0 && m.cpu_game_pct <= 100, "the CPU's load from the second sample: %.1f, %.1f",
              m.cpu_pct, m.cpu_game_pct);
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
        CHECK(m.cpu_name[0] && m.cpu_name[0] != ' ' && m.cpu_name[strlen(m.cpu_name) - 1] != ' ', "the CPU's name, trimmed: '%s'", m.cpu_name);
#endif
        pm::stop();
        CHECK(ended(), "stopped: its thread ends (%d alive)", pm::live_samplers());
        const int stopped = pm::latest().samples;
        std::this_thread::sleep_for(std::chrono::milliseconds(700));   // (longer than its period: a sampler left would have sampled)
        CHECK(pm::latest().samples == stopped, "stopped: no sample more (%d, %d)", stopped, pm::latest().samples);
        pm::start(g, nullptr);
        CHECK(pm::latest().samples == 0, "a new start shows \"-\" until its first sample, not the numbers of before");
        m = wait_for(1);
        CHECK(m.samples >= 1 && pm::live_samplers() == 1, "started again, one thread");
        pm::stop();
        CHECK(ended(), "stopped again");
    }

    printf(fails ? "%d checks failed\n" : "all checks passed\n", fails);
    return fails != 0;
}
