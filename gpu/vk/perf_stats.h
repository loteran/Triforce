// (Triforce) The performance overlay's frame times (vk_menu.cpp, the FPS counter row on Full): a ring of the
// game's frame times, and what the overlay says of them: the mean, the longest, the median, the 99th percentile and
// the 1% and 0.1% lows over the last ten seconds, and the newest of them for the graph. After what NX2navi's overlay shows
// (its runtime/perfmon.c; none of its code is here: its 0.1% low and its rule of 100 frames for both lows are, computed
// here by the same nearest rank as the 1% low). Plain functions with no clock and no lock of their own, so that
// tests/test_perf_overlay.py runs them (tests/test_perf_overlay.cpp); the caller gives the times and keeps one thread in
// at a time.
#pragma once

#include <algorithm>
#include <cstdint>

namespace mkwvk::perf_stats {

constexpr int kFrames = 2048;                       // the frame times kept (a power of two)
constexpr int64_t kWindowNs = 10000000000ll;        // the summary counts the frames of the last ten seconds
constexpr int64_t kGapNs = 1000000000ll;            // held longer than this, a frame is a load or a pause: no frame time
constexpr int kLowFrames = 100;                     // under this many frames there is no 1% low yet, nor a 0.1% low

struct Ring {
    uint32_t us[kFrames] = {};   // frame times in microseconds
    uint32_t count = 0;          // frame times written since the start: the newest is us[(count - 1) % kFrames]
    int64_t last_ns = 0;         // when the newest frame came; 0 before the first
};

/// A frame of the game came at now_ns: the time since the one before goes into the ring. The first frame, a frame
/// after a gap (kGapNs) and a clock that did not move on only start the next interval.
inline void note(Ring& r, int64_t now_ns) {
    const int64_t d = r.last_ns ? now_ns - r.last_ns : 0;
    r.last_ns = now_ns;
    if (d <= 0 || d > kGapNs) return;
    r.us[r.count++ % kFrames] = (uint32_t)((d + 500) / 1000);
}

/// How many frame times the ring holds
inline int held(const Ring& r) { return (int)std::min<uint32_t>(r.count, (uint32_t)kFrames); }

/// The newest n frame times (as many as there are), the oldest of them first, in milliseconds: the graph. Returns how
/// many were written.
inline int newest(const Ring& r, float* out, int n) {
    const int m = std::min(std::max(n, 0), held(r));
    for (int i = 0; i < m; i++) out[i] = (float)r.us[(r.count - (uint32_t)m + (uint32_t)i) % kFrames] / 1000.0f;
    return m;
}

struct Summary {
    int frames = 0;       // the frame times counted
    float avg_ms = 0;     // their mean
    float max_ms = 0;     // the longest
    float p50_ms = 0;     // the median
    float p99_ms = 0;     // the 99th percentile: 99 of 100 frames took no longer
    float fps = 0;        // the frame rate of the mean
    float low1_fps = -1;  // the 1% low: the frame rate of the 99th percentile; -1 with fewer than kLowFrames frames
    float low01_fps = -1; // (Triforce: from NX2navi's perfmon.c lows_summary) the 0.1% low: the frame rate of the 99.9th
                          // percentile (under 1000 frames, the longest); -1 with fewer than kLowFrames frames
};

/// Of n values sorted upwards, the one at percentile q (0 to 1), by the nearest rank: the smallest value that at least
/// that share of them does not exceed.
inline uint32_t percentile(const uint32_t* sorted, int n, double q) {
    if (n <= 0) return 0;
    int rank = (int)(q * n);
    if ((double)rank < q * n) rank++;   // (the ceiling)
    return sorted[std::min(std::max(rank, 1), n) - 1];
}

/// What the overlay says of the newest frames: those whose times add up to window_ns, at least one.
inline Summary summarize(const Ring& r, int64_t window_ns = kWindowNs) {
    Summary s;
    static thread_local uint32_t v[kFrames];
    const int have = held(r);
    int n = 0;
    uint64_t sum = 0;
    for (int i = 0; i < have; i++) {   // (the newest first)
        const uint32_t us = r.us[(r.count - 1u - (uint32_t)i) % kFrames];
        if (n && (int64_t)(sum + us) * 1000 > window_ns) break;
        v[n++] = us, sum += us;
    }
    if (!n) return s;
    std::sort(v, v + n);
    s.frames = n;
    s.avg_ms = (float)((double)sum / n / 1000.0);
    s.max_ms = (float)v[n - 1] / 1000.0f;
    s.p50_ms = (float)percentile(v, n, 0.50) / 1000.0f;
    s.p99_ms = (float)percentile(v, n, 0.99) / 1000.0f;
    s.fps = sum ? (float)(1e6 * n / (double)sum) : 0;
    if (n >= kLowFrames && s.p99_ms > 0) s.low1_fps = 1000.0f / s.p99_ms;
    if (n >= kLowFrames) {
        const uint32_t p999 = percentile(v, n, 0.999);
        if (p999) s.low01_fps = 1000000.0f / (float)p999;
    }
    return s;
}

// ---- (Triforce) the overlay's figures per frame (NX2navi's waits block: GPU frame prep, GPU busy per frame, Pipeline waits,
// Shader waits, Texture uploads), from running counts read twice a second (vk_menu.cpp perf_sample) ----

/// The running counts the figures come from, as of one sample: the game's frames, the replay's time to record them and the
/// GPU's in the recordings (ns; the GPU's stays 0 without timestamps), the textures uploaded, and the replay's waits for
/// pipelines and for translations (us, vk_draw.cpp draw_wait_split)
struct Counts {
    int64_t frames = 0, prep_ns = 0, gpu_ns = 0, uploads = 0, pipe_us = 0, shader_us = 0;
};
/// The counts of the sample before (have: false until there is one)
struct Base {
    bool have = false;
    Counts at;
};
/// The figures per game frame since the sample before, ms or uploads a frame; -1 where there is none (the panel's "-")
struct PerFrame {
    double prep_ms = -1, gpu_ms = -1, pipe_ms = -1, shader_ms = -1, uploads = -1;
};
enum Step {
    STEP_FIRST,   ///< no counts to go from (the first sample, or fresh): every figure "-"
    STEP_NEW,     ///< out has the figures since the sample before
    STEP_KEEP,    ///< no game frame since: the panel keeps what it shows (out untouched)
};
/// One sample's step. fresh: the panel was turned on now, so the counts of the sample before are of another time. Each figure
/// is its count's change over the frames' change; a count that went back is 0, and the GPU's time "-" when it did not move
/// on (no timestamps on this device). now becomes the next sample's base in every case
inline Step per_frame(Base& b, const Counts& now, bool fresh, PerFrame* out) {
    const Counts was = b.at;
    const bool had = b.have && !fresh;
    b.have = true, b.at = now;
    if (!had) {
        *out = PerFrame{};
        return STEP_FIRST;
    }
    const int64_t n = now.frames - was.frames;
    if (n <= 0) return STEP_KEEP;
    const auto per = [n](int64_t d, double unit) { return (double)(d > 0 ? d : 0) / unit / (double)n; };
    out->prep_ms = per(now.prep_ns - was.prep_ns, 1e6);
    out->gpu_ms = now.gpu_ns > was.gpu_ns ? per(now.gpu_ns - was.gpu_ns, 1e6) : -1;
    out->pipe_ms = per(now.pipe_us - was.pipe_us, 1e3);
    out->shader_ms = per(now.shader_us - was.shader_us, 1e3);
    out->uploads = per(now.uploads - was.uploads, 1);
    return STEP_NEW;
}

} // namespace mkwvk::perf_stats
