// (Triforce, after NX2navi e10ef60's library warm check, runtime/vk_pipeline.c lw_probe and lw_main) Whether the stage libraries
// the preparation made are still in the pipeline cache, at a start whose record says they were made (vk_draw.cpp lib_check_cold,
// called by prepare_run). Apart from Vulkan so that tests/test_lib_check.py runs its rules alone.
//
// Why. triforce_prepared.bin's second stamp (vk_draw.cpp warmed_stamp) says the driver's part of the shaders was made for these
// programs, this driver and kWarmRecipe, and a start with that stamp goes straight to the precompile. Nothing looked at whether
// the libraries were still there. They are kept in our pipeline cache (gpl_libs_cached: asked for as a draw's link asks, the
// driver answers from the cache it is handed and not from its own on disk), so they are cold again when mkw_vk_pipelines.bin is
// lost or refused (deleted, another device's header, a short read), or when what a library is built with changed without
// kWarmRecipe counted up. Cold, no new pipeline is linked: each waits for a whole build, or its draws are left out for the frames
// that takes. NX2navi measured it on an RX 7900 XT (26.10.1) with all 42,142 of its prepared libraries cold, one entry each in the
// driver's cache after a change of their dynamic states: 93 ms a vertex and 242 ms a fragment library, a walk through a village
// skipped 11,180 draws and made 94 draws wait 5.5 s in all, four of them 1.0 to 2.0 s; with them warm again 0.3 and 0.7 ms.
//
// What. A sample of kSample kept program variants, spread over the program cache (from another place each start), each one's two
// shader libraries made as the driver's part makes them, timed, and made once more at once. Where the device can (gpl_ask_only)
// they are asked of the caches only, with the flag a draw's link asks with (FAIL_ON_PIPELINE_COMPILE_REQUIRED), and the answer
// is the driver's own on every vendor: cold is "a compile is required", the very answer that leaves a draw without its link.
// Without the flag (RX_VK_GPL_SYNC) the times say it, against a reference made on the spot: the second making comes from the
// cache by construction, the warm time of this driver on this PC, so no fixed time is needed (a library from the cache is 0.3 to
// 3 ms on one driver or another, one compiled 30 to 300 ms); cold is a first making over kRatio times the second and kSlackMs
// more. The sample is cold when more than half of its variants are, its median; under half a sample (a program cache with fewer
// variants) says nothing.
//
// Cold, the driver's part is made again at once, before the game, as after a driver update: every processor the preparation may
// have (prepare_threads.h, which counts the RAM free), the start-up screen's "Compiling shaders", its journal and skip list
// (gpl_rules.h), and the cache saved after it. Not on idle threads in play: Triforce's builder behind the game measured 55-64 fps
// against 41-47 with it compiling (vk_draw.cpp, before predict_background), and that is what a cold list of tens of thousands of
// libraries would do for most of a session. Warm (the start's usual case), nothing more happens than the sample: twelve program
// reads and some fifty library lookups.
//
// It is not made again for ever: a making again holds the start for minutes. <prepared>.check records, for the stamp, that the
// check made the part again, and how many starts in a row found the libraries cold again right after (strikes). Past kStrikes the
// driver or the PC does not keep them (a cache that does not stay, a driver that answers cold for what it has), and the libraries
// are left to the draws that need them, as before this check: with kStrikes 0 the part is made again once, and a start that finds
// it cold again right after stops there. NX2navi tries three times, but it warms in play; here each try is a start held. A making
// again whose cache could not be written (over the bound, or its save failed) or that warm_begin left out (gpl_rules.h kTries starts that died in it)
// is recorded the same way. A start that finds them warm removes the file, and a later loss is made again. RX_VK_LIB_CHECK=0: no
// check, as before.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lib_check {

inline constexpr int kSample = 12;               // (variants sampled, NX2navi's LW_SAMPLE)
inline constexpr int kLook = 2 * kSample;        // (programs looked at for them: one may have no variant with libraries)
inline constexpr double kRatio = 4.0;            // (first making against the second: cold above)
inline constexpr double kSlackMs = 3.0;          // (and this much more: lookups of a few milliseconds say nothing)
inline constexpr int kStrikes = 0;               // (made again once, and cold again right after: left to the draws)

/// RX_VK_LIB_CHECK: on unless "0"
inline bool on(const char* env) { return !env || strcmp(env, "0") != 0; }

/// One variant sampled: its two libraries' first making and the second (ms, both libraries together). asked: made asking the
/// caches only; answered_cold: they said a compile is required; made: both were made both times (a library the driver refuses
/// says nothing of the cache). Asked, the answer alone counts: a driver that made the library without a compile has it, however
/// long its first lookup took. Not asked, the times say it
struct Probe {
    bool asked = false, answered_cold = false, made = false;
    double first_ms = 0, again_ms = 0;
};
inline bool counts(const Probe& p) { return p.answered_cold || p.made; }
inline bool cold(const Probe& p) {
    return p.answered_cold || (!p.asked && p.made && p.first_ms > kRatio * p.again_ms + kSlackMs);
}

/// What a sample comes to: the variants that count, the cold ones, and the median times (for the log)
struct Tally {
    int got = 0, cold = 0, answered = 0;
    double first_median = 0, again_median = 0;
};
inline double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
inline Tally tally(const std::vector<Probe>& probes) {
    Tally t;
    std::vector<double> first, again;
    for (const Probe& p : probes) {
        if (!counts(p)) continue;
        t.got++, t.cold += cold(p), t.answered += p.answered_cold;
        first.push_back(p.first_ms);
        if (p.made) again.push_back(p.again_ms);
    }
    t.first_median = median(first), t.again_median = median(again);
    return t;
}

enum class Verdict { Unknown, Warm, Cold };
inline Verdict verdict(const Tally& t) {
    if (t.got * 2 < kSample) return Verdict::Unknown;
    return t.cold * 2 > t.got ? Verdict::Cold : Verdict::Warm;
}

/// k places spread over n, the first at seed's place in the first step: each start samples other programs
inline std::vector<size_t> spots(size_t n, size_t k, uint64_t seed) {
    std::vector<size_t> at;
    if (!n || !k) return at;
    const size_t step = std::max<size_t>(1, n / k);
    for (size_t i = (size_t)(seed % step); i < n && at.size() < k; i += step) at.push_back(i);
    return at;
}

// ---- <prepared>.check: "lib-check <stamp> <strikes>"

struct State {
    bool found = false;
    uint64_t stamp = 0;
    int strikes = 0;
};
inline State state_read(const std::string& path) {
    State s;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return s;
    char line[96]{};
    const size_t n = fread(line, 1, sizeof line - 1, f);
    fclose(f);
    line[n] = 0;
    unsigned long long stamp = 0;
    int strikes = 0;
    if (sscanf(line, "lib-check %16llx %d", &stamp, &strikes) == 2 && strikes >= 0)
        s.found = true, s.stamp = stamp, s.strikes = strikes;
    return s;
}
/// Written whole beside the old one, then put in place (a start that ends in the middle leaves the old file)
inline bool state_write(const std::string& path, uint64_t stamp, int strikes) {
    char line[96];
    const int n = snprintf(line, sizeof line, "lib-check %016llx %d\n", (unsigned long long)stamp, strikes);
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(line, 1, (size_t)n, f) == (size_t)n;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(path.c_str());   // (rename does not replace on Windows)
        ok = rename(tmp.c_str(), path.c_str()) == 0;
    }
    if (!ok) remove(tmp.c_str());
    return ok;
}

enum class Do { Nothing, Again, Stop };
/// What a start does with a sample's verdict, the record had of the last starts and the stamp now. *next: the strikes to record
/// once the part is made again (Again only)
inline Do decide(Verdict v, const State& had, uint64_t stamp, int* next) {
    if (v != Verdict::Cold) return Do::Nothing;
    const bool same = had.found && had.stamp == stamp;   // (made again for this stamp, and cold again since)
    if (same && had.strikes >= kStrikes) return Do::Stop;
    *next = same ? had.strikes + 1 : 0;
    return Do::Again;
}

}   // namespace lib_check
