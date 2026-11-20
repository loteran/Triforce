// tests/test_replay_threads_tuner.py compiles and runs this: gpu/vk/replay_threads.h's tuner and rule (vk_replay.cpp
// par_tune_present calls Tuner::step at each present). Cases:
// "same": with the rule and the back-off off, Tuner decides frame for frame what the tuner inline in par_tune_present decided
//   before (copied below as it was), over random runs of frames with loads and stretches where both ways are the replay
//   thread alone.
// "prev": with the back-off off (RX_VK_PARALLEL_BACKOFF=0), Tuner steps frame for frame as the one before the back-off did
//   (PrevTuner below, a copy of it as it was at cce65bd), the rule on and off, over random scenes that change.
// "rule": the rule's verdicts on the owner's own scenes (their numbers from the logs, as replay_threads.h says), and the
//   tuner on long runs of each: on without a trial where the replay is plainly the frame's limit, no trial where the GPU or
//   the game's threads are, trials in between, nothing at the title; the hold after a trial took them back off; loads and
//   DLSS / FSR kept in order (order_only); Part's means.
// "backoff": the back-off (Tuner::backoff). Trials per 10,000 presents and frames held the other way, with it and without,
//   on a steady scene like the lab's Market (f24-S1) and on scenes that change every 8,000 presents; the way in use the
//   better one as often as without it, and as soon after a change; a short trial never changes the way, one that finds the
//   other way shorter is followed at once by a full one; a change the scene's means cannot see is found by a backed-off
//   trial; the rule's no-trial verdicts hold; a load is no scene change.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "replay_threads.h"

static int failures;
#define CHECK(x)                                              \
    do {                                                      \
        if (!(x)) {                                           \
            std::printf("FAIL:%d %s\n", __LINE__, #x);        \
            failures++;                                       \
        }                                                     \
    } while (0)

using replay_threads::Frame;
using replay_threads::Part;
using replay_threads::Tuner;
using replay_threads::Verdict;

/// The tuner as par_tune_present had it inline (before 11-15): its decisions only. Returns whether the way changes
struct OldTuner {
    int phase = 0, n = 0, cnt[3] = {0, 0, 0};
    double sum[3] = {0, 0, 0};
    long long frames = 0, next_trial = 600;
    bool step(double ms, bool order_only) {
        constexpr int kEvery = 900, kTake = 80, kSettle = 24;
        frames++;
        if (phase == 0) {
            if (frames < next_trial) return false;
            if (order_only) return false;
            phase = 1, n = 0;
            sum[0] = sum[1] = sum[2] = 0, cnt[0] = cnt[1] = cnt[2] = 0;
            return false;
        }
        const int k = phase - 1;
        n++;
        if (n > (phase == 1 ? 0 : kSettle) && ms <= 60.0) sum[k] += ms, cnt[k]++;
        if (n < (phase == 1 ? kTake : kSettle + kTake)) return false;
        n = 0;
        if (phase == 1) return phase = 2, true;
        if (phase == 2) return phase = 3, true;
        phase = 0;
        const bool ok = cnt[0] >= kTake * 3 / 4 && cnt[1] >= kTake * 3 / 4 && cnt[2] >= kTake * 3 / 4;
        const double a = cnt[0] ? sum[0] / cnt[0] : 0, b = cnt[1] ? sum[1] / cnt[1] : 0, c = cnt[2] ? sum[2] / cnt[2] : 0;
        const bool better = ok && b < std::min(a, c) * 0.96;
        next_trial = frames + (better ? kEvery : kEvery * 2);
        return better;
    }
};

using replay_threads::rule;
/// (Triforce, 11-16) The tuner as replay_threads.h had it before the back-off (cce65bd), whole: "prev" holds Tuner with
/// backoff = false to it, step for step
class PrevTuner {
public:
    static constexpr int kEvery = 900, kTake = 80, kSettle = 24, kWindow = 120, kHold = 4 * kEvery;
    bool use_rule = true;
    int threads = 4;   // (the threads that record segments: rule()'s)
    struct Step {
        bool flip = false;         // change the way in use
        bool trial = false;        // a trial ended (a, b, c its means; ok: each part had enough frames; better: changed)
        double a = 0, b = 0, c = 0;
        bool ok = false, better = false;
        int64_t trials = 0, changes = 0;
        const Part* parts = nullptr;   // (the trial's three parts)
        bool ruled = false;        // the rule judged a window
        bool rule_new = false;     // ...and its verdict differs from the last one, or it turned the helpers on
        Verdict verdict;
        Frame window;              // (the window's mean frame it judged)
    };
    /// f: the frame just presented; on: the helper threads in use for it; order_only: both ways are the replay thread alone
    Step step(const Frame& f, bool on, bool order_only) {
        Step s;
        frames_++;
        if (phase_ == 0) {
            if (use_rule && !on) {
                if (f.frame <= 60.0) win_.add(f);
                if (++win_frames_ >= kWindow) {
                    if (win_.n >= kWindow * 3 / 4 && !order_only) {
                        s.ruled = true, s.window = win_.mean(), s.verdict = rule(s.window, threads);
                        const bool turn_on = s.verdict.kind == Verdict::kOn && frames_ >= hold_until_;
                        s.rule_new = s.verdict.kind != last_ || turn_on;
                        last_ = s.verdict.kind;
                        if (turn_on) {
                            s.flip = true;
                            next_trial_ = frames_ + kEvery;
                        } else if (s.verdict.kind == Verdict::kOff) {
                            next_trial_ = std::max(next_trial_, frames_ + kEvery);
                        }
                    }
                    win_ = Part{}, win_frames_ = 0;
                    if (s.flip) return s;
                }
            } else {
                win_ = Part{}, win_frames_ = 0;
            }
            if (frames_ < next_trial_) return s;
            if (order_only) return s;
            phase_ = 1, n_ = 0;
            win_ = Part{}, win_frames_ = 0;   // (the rule's next window begins after the trial)
            sum_[0] = sum_[1] = sum_[2] = 0, cnt_[0] = cnt_[1] = cnt_[2] = 0;
            part_[0] = part_[1] = part_[2] = Part{};
            return s;
        }
        const int k = phase_ - 1;
        n_++;
        if (n_ > (phase_ == 1 ? 0 : kSettle) && f.frame <= 60.0) sum_[k] += f.frame, cnt_[k]++, part_[k].add(f);
        if (n_ < (phase_ == 1 ? kTake : kSettle + kTake)) return s;
        n_ = 0;
        if (phase_ == 1) {
            s.flip = true, phase_ = 2;
        } else if (phase_ == 2) {
            s.flip = true, phase_ = 3;
        } else {
            phase_ = 0, trials_++;
            s.ok = cnt_[0] >= kTake * 3 / 4 && cnt_[1] >= kTake * 3 / 4 && cnt_[2] >= kTake * 3 / 4;
            s.a = cnt_[0] ? sum_[0] / cnt_[0] : 0, s.b = cnt_[1] ? sum_[1] / cnt_[1] : 0, s.c = cnt_[2] ? sum_[2] / cnt_[2] : 0;
            s.better = s.ok && s.b < std::min(s.a, s.c) * 0.96;
            if (s.better) {
                s.flip = true, changes_++;
                if (on) hold_until_ = frames_ + kHold;   // (the trial took them off: the rule's "on" waits)
            }
            next_trial_ = frames_ + (s.better ? kEvery : kEvery * 2);
            s.trial = true, s.parts = part_;
        }
        s.trials = trials_, s.changes = changes_;
        return s;
    }
    int64_t frames() const { return frames_; }

private:
    int phase_ = 0, n_ = 0, cnt_[3] = {0, 0, 0};
    double sum_[3] = {0, 0, 0};
    Part part_[3];
    Part win_;
    int win_frames_ = 0;
    int64_t frames_ = 0, next_trial_ = 600, trials_ = 0, changes_ = 0, hold_until_ = 0;
    Verdict::Kind last_ = Verdict::kTrial;
};

/// A scene: its frame on the replay thread alone, and how much shorter (or longer: below 1 is shorter) the helpers make it
struct Scene {
    double frame, replay, gpu, draws, cut, op;
    double par_factor;   // (the frame with the helper threads in use, as a share of the frame without)
};
Frame sample(const Scene& sc, bool on, std::mt19937& r) {
    std::normal_distribution<double> noise(0.0, 0.02);
    Frame f;
    const double k = 1.0 + noise(r);
    f.frame = sc.frame * k * (on ? sc.par_factor : 1.0);
    f.replay = sc.replay * k * (on ? 0.6 : 1.0);
    f.gpu = sc.gpu * k, f.draws = sc.draws, f.cut = sc.cut, f.op = sc.op * k, f.parallel = on;
    if (on) f.segmented = 3, f.dry = 0.4, f.rec = sc.op * 1.3;
    return f;
}

// (the owner's scenes, from the logs replay_threads.h names: on the replay thread alone)
const Scene test4_heavy{12.03, 9.71, 6.4, 2872, 2700, 7.28, 0.88};    // (trial 4 kept them: 75 fps to 86-90 with more draws)
const Scene test8_play{9.34, 7.63, 7.1, 2387, 2250, 4.19, 0.98};      // (the GPU's 6.4 + 0.7 ms: kept by a noisy trial, ~2%)
const Scene gpu_bound{9.6, 7.0, 7.4, 2387, 2250, 4.19, 1.02};         // (test8's with more GPU than replay: made up)
const Scene test7_busy{12.02, 9.63, 8.4, 2930, 2700, 6.5, 0.97};      // (trials there kept the replay thread alone)
const Scene test7_trial4{11.5, 6.3, 5.6, 1020, 900, 3.8, 1.11};       // (the game's threads the limit: 12.31 ms with them)
const Scene title{4.17, 0.6, 0.9, 30, 0, 0.1, 1.0};

Frame mean_of(const Scene& sc) {
    Frame f;
    f.frame = sc.frame, f.replay = sc.replay, f.gpu = sc.gpu, f.draws = sc.draws, f.cut = sc.cut, f.op = sc.op;
    return f;
}

struct Run {
    int rule_on = 0, rule_off = 0, rule_trial = 0, trials = 0, changes = 0, flips = 0, on_frames = 0;
    long long first_on = -1, first_trial = -1;
};
/// frames presents of a scene through the tuner, the way in use applied as par_tune_present applies it
/// (backoff: the tuner's back-off; off for the checks of the trials' schedule as it was)
Run run_scene(const Scene& sc, int frames, bool use_rule, uint32_t seed, Tuner* keep = nullptr, bool backoff = false) {
    Tuner local;
    Tuner& t = keep ? *keep : local;
    t.use_rule = use_rule, t.threads = 4, t.backoff = backoff;
    std::mt19937 r(seed);
    bool on = false;
    Run out;
    for (int i = 0; i < frames; i++) {
        const Frame f = sample(sc, on, r);
        const Tuner::Step s = t.step(f, on, false);
        if (s.ruled) (s.verdict.kind == Verdict::kOn ? out.rule_on : s.verdict.kind == Verdict::kOff ? out.rule_off : out.rule_trial)++;
        if (s.ruled && s.flip && out.first_on < 0) out.first_on = t.frames();
        if (s.trial) {
            out.trials++, out.changes += s.better;
            if (out.first_trial < 0) out.first_trial = t.frames();
        }
        if (s.flip) on = !on, out.flips++;
        out.on_frames += on;
    }
    return out;
}

void same_case() {
    std::mt19937 r(2026);
    int runs = 0, flips = 0, differ = 0;
    for (int run = 0; run < 60; run++) {
        OldTuner old;
        Tuner t;
        t.use_rule = false, t.backoff = false;
        bool on = false, order_only = false;
        const double base = 6.0 + (r() % 900) / 100.0, par = 0.85 + (r() % 30) / 100.0;   // (the helpers 15% faster to 15% slower)
        for (int i = 0; i < 12000; i++) {
            if (r() % 1500 == 0) order_only = !order_only;
            double ms = base * (on ? par : 1.0) * (1.0 + ((int)(r() % 200) - 100) / 2000.0);
            if (r() % 97 == 0) ms = 70.0 + r() % 200;   // (a load)
            Frame f;
            f.frame = ms;
            const bool a = old.step(ms, order_only);
            const Tuner::Step s = t.step(f, on, order_only);
            CHECK(!s.ruled);
            if (a != s.flip) differ++;
            if (s.flip) on = !on, flips++;
        }
        runs++;
    }
    CHECK(differ == 0);
    CHECK(flips > 100);   // (the trials changed ways, and back, many times: the decisions were compared where they matter)
    std::printf("same: %d runs of 12,000 presents with the rule off, %d changes of way, %d frames where the tuner and the inline one "
                "differ\n", runs, flips, differ);
}

void rule_case() {
    // the verdicts on the owner's scenes
    const auto kind = [](const Scene& sc) { return replay_threads::rule(mean_of(sc), 4).kind; };
    CHECK(kind(test4_heavy) == Verdict::kOn);
    CHECK(kind(test8_play) == Verdict::kTrial);   // (0.53 ms over the GPU's 7.1: too little to count on, too much to rule out)
    CHECK(kind(gpu_bound) == Verdict::kOff);      // (the GPU's time over the replay's)
    CHECK(kind(test7_trial4) == Verdict::kOff);   // (the game's threads: the replay 55% of the frame)
    CHECK(kind(test7_busy) == Verdict::kTrial);
    CHECK(kind(title) == Verdict::kOff);
    {
        const Verdict v = replay_threads::rule(mean_of(test4_heavy), 4);
        // (2,700 of them at 2.53 us, three quarters on the helpers, less the dry pass: ~4.6 ms off the replay thread; the frame
        // at most 9.71 - 6.4 - 0.5 = 2.81 ms shorter)
        CHECK(std::fabs(v.saved - (2700 * 7.28 / 2872 * 0.75 - 2700 * 0.0002)) < 1e-9);
        CHECK(std::fabs(v.gain - (9.71 - 6.4 - replay_threads::kGpuExtra)) < 1e-9);
        CHECK(std::strstr(v.why, "plainly"));
        CHECK(std::strstr(replay_threads::rule(mean_of(gpu_bound), 4).why, "GPU"));
        CHECK(std::strstr(replay_threads::rule(mean_of(test8_play), 4).why, "trials decide"));
        CHECK(std::strstr(replay_threads::rule(mean_of(test7_trial4), 4).why, "game's threads"));
        CHECK(std::strstr(replay_threads::rule(mean_of(title), 4).why, "cut"));
    }
    // fewer threads take less off: two recording threads (one helper and the replay thread) give half, never more than four
    CHECK(replay_threads::rule(mean_of(test4_heavy), 2).saved < replay_threads::rule(mean_of(test4_heavy), 4).saved);
    CHECK(replay_threads::rule(mean_of(test4_heavy), 1).kind == Verdict::kOff);
    // a scene where the helpers save less than the replay is over the GPU: the saving bounds the gain
    {
        Frame m = mean_of(test4_heavy);
        m.cut = 400;
        const Verdict v = replay_threads::rule(m, 4);
        CHECK(v.gain == v.saved && v.gain < 1.0 && v.kind != Verdict::kOn);
    }
    // the tuner on each scene, 20,000 presents
    const Run heavy = run_scene(test4_heavy, 20000, true, 1);
    CHECK(heavy.first_on == Tuner::kWindow);             // (on at the first window, without a trial)
    CHECK(heavy.first_trial > heavy.first_on);           // (a trial came after, kEvery later)
    CHECK(heavy.changes == 0 && heavy.trials >= 6);       // (and kept them, 12% shorter frames: no trial took them off)
    CHECK(heavy.on_frames > 20000 - 120 - (heavy.trials + 1) * (Tuner::kSettle + Tuner::kTake));   // (but in the trials' serial parts)
    const Run gpu = run_scene(gpu_bound, 20000, true, 2);
    CHECK(gpu.trials == 0 && gpu.flips == 0 && gpu.rule_off > 100);   // (no trial at all: it cannot pay)
    const Run game = run_scene(test7_trial4, 20000, true, 3);
    CHECK(game.trials == 0 && game.flips == 0);
    const Run busy = run_scene(test7_busy, 20000, true, 4);
    CHECK(busy.trials >= 6 && busy.rule_trial > 0 && busy.rule_on == 0);   // (the trials decide, on their schedule)
    const Run play = run_scene(test8_play, 20000, true, 6);
    CHECK(play.trials >= 6 && play.rule_on == 0 && play.rule_off == 0);
    const Run idle = run_scene(title, 20000, true, 5);
    CHECK(idle.trials == 0 && idle.flips == 0);
    // with the rule off: trials everywhere, as before
    const Run old_gpu = run_scene(gpu_bound, 20000, false, 2);
    CHECK(old_gpu.trials >= 6);
    // (Triforce, 11-16) the same with the back-off on: the rule's verdicts as they were (on without a trial, no trial where it
    // cannot pay, nothing at the title), the trials fewer where they decide, and none changed the way where it held
    {
        const Run h = run_scene(test4_heavy, 20000, true, 1, nullptr, true);
        CHECK(h.first_on == Tuner::kWindow && h.first_trial > h.first_on && h.changes == 0);
        CHECK(h.trials >= 2 && h.trials < heavy.trials);
        CHECK(h.on_frames > heavy.on_frames);   // (fewer frames the other way)
        const Run g = run_scene(gpu_bound, 20000, true, 2, nullptr, true);
        CHECK(g.trials == 0 && g.flips == 0 && g.rule_off == gpu.rule_off);
        const Run gm = run_scene(test7_trial4, 20000, true, 3, nullptr, true);
        CHECK(gm.trials == 0 && gm.flips == 0);
        // (the helpers 2 and 3% faster there: every trial too close to tell, so none holds the way and they come as before. Each
        // costs little: its two ways' frames about as long)
        const Run b = run_scene(test7_busy, 20000, true, 4, nullptr, true);
        CHECK(b.trials >= 3 && b.trials <= busy.trials && b.rule_on == 0 && b.changes == busy.changes);
        const Run p = run_scene(test8_play, 20000, true, 6, nullptr, true);
        CHECK(p.trials >= 3 && p.trials <= play.trials && p.rule_on == 0 && p.rule_off == 0);
        const Run i = run_scene(title, 20000, true, 5, nullptr, true);
        CHECK(i.trials == 0 && i.flips == 0);
        std::printf("rule: with the back-off: test4's heavy scene %d trials in 20,000 presents (%d without), test7's busy scene %d "
                    "(%d), test8's play %d (%d); the GPU-bound scene, test7's trial 4 and the title none\n", h.trials, heavy.trials,
                    b.trials, busy.trials, p.trials, play.trials);
    }
    // the hold: the rule says on, but the frames are longer with the helpers (the model wrong for this scene). The trial takes
    // them back off, and the rule does not turn them on again for kHold presents
    {
        Scene wrong = test4_heavy;
        wrong.par_factor = 1.15;
        Tuner t;
        t.use_rule = true, t.threads = 4;
        std::mt19937 r(9);
        bool on = false;
        long long off_by_trial = -1, on_again = -1;
        for (int i = 0; i < 20000; i++) {
            const Tuner::Step s = t.step(sample(wrong, on, r), on, false);
            if (s.trial && s.better && on && off_by_trial < 0) off_by_trial = t.frames();
            if (s.ruled && s.flip && off_by_trial >= 0 && on_again < 0) on_again = t.frames();
            if (s.flip) on = !on;
        }
        CHECK(off_by_trial > 0);
        CHECK(on_again < 0 || on_again >= off_by_trial + Tuner::kHold);
        std::printf("rule: the hold: a trial took the helpers off at present %lld, the rule turned them on again at %lld\n",
                    off_by_trial, on_again);
    }
    // loads: a window with fewer than three quarters of its frames counted is not judged
    {
        Tuner t;
        std::mt19937 r(11);
        int ruled = 0;
        for (int i = 0; i < 2400; i++) {
            Frame f = sample(test4_heavy, false, r);
            if (i % 3 == 0) f.frame = 80.0;   // (a third of them a load)
            ruled += t.step(f, false, false).ruled;
        }
        CHECK(ruled == 0);
    }
    // both ways the replay thread alone (DLSS or FSR with RX_VK_PARALLEL_TEMPORAL=0): neither the rule nor a trial
    {
        Tuner t;
        std::mt19937 r(12);
        int changes = 0;
        for (int i = 0; i < 6000; i++) changes += t.step(sample(test4_heavy, false, r), false, true).flip;
        CHECK(changes == 0);
    }
    // with the helpers in use the rule does not judge (no frame on the replay thread alone to judge by)
    {
        Tuner t;
        std::mt19937 r(13);
        int ruled = 0;
        for (int i = 0; i < 590; i++) ruled += t.step(sample(test4_heavy, true, r), true, false).ruled;
        CHECK(ruled == 0);
    }
    // Part: sums and means; first is each command buffer's
    {
        Part p;
        Frame a, b;
        a.frame = 10, a.replay = 8, a.segmented = 2, a.first = 0.8, a.parallel = true;
        b.frame = 12, b.replay = 6, b.segmented = 4, b.first = 2.0;
        p.add(a), p.add(b);
        const Frame m = p.mean();
        CHECK(p.n == 2 && m.frame == 11 && m.replay == 7 && m.segmented == 3 && std::fabs(m.first - 2.8 / 6) < 1e-12 && m.parallel);
        CHECK(Part{}.mean().frame == 0);
    }
    std::printf("rule: test4's heavy scene on at present %lld without a trial (first trial at %lld); a GPU-bound scene and test7's "
                "trial 4 no trial in 20,000 presents (%d and %d windows judged); test8's play %d trials, test7's busy scene %d; the "
                "title none\n", heavy.first_on, heavy.first_trial, gpu.rule_off, game.rule_off, play.trials, busy.trials);
}

// ---- (Triforce, 11-16) the back-off ----

/// Tuner with backoff = false against PrevTuner, step for step: the same frames, ways, loads and DLSS / FSR stretches
void prev_case() {
    std::mt19937 r(1116);
    const Scene scenes[] = {test4_heavy, test8_play, gpu_bound, test7_busy, test7_trial4, title};
    long long steps = 0, differ = 0, flips = 0, trials = 0, ruled = 0;
    for (int run = 0; run < 40; run++) {
        PrevTuner prev;
        Tuner t;
        const bool use_rule = run % 2 == 0;
        prev.use_rule = use_rule, t.use_rule = use_rule, t.backoff = false;
        prev.threads = t.threads = 2 + run % 3;
        bool on_p = false, on_t = false, order_only = false;
        Scene sc = scenes[r() % 6];
        sc.par_factor = 0.8 + (r() % 40) / 100.0;
        std::mt19937 noise((uint32_t)run);
        for (int i = 0; i < 20000; i++) {
            if (r() % 3000 == 0) sc = scenes[r() % 6], sc.par_factor = 0.8 + (r() % 40) / 100.0;
            if (r() % 2500 == 0) order_only = !order_only;
            Frame f = sample(sc, on_t, noise);
            if (r() % 97 == 0) f.frame = 70.0 + r() % 200;   // (a load)
            const PrevTuner::Step a = prev.step(f, on_p, order_only);
            const Tuner::Step b = t.step(f, on_t, order_only);
            const bool same = a.flip == b.flip && a.trial == b.trial && a.ok == b.ok && a.better == b.better && a.a == b.a &&
                              a.b == b.b && a.c == b.c && a.trials == b.trials && a.changes == b.changes && a.ruled == b.ruled &&
                              a.rule_new == b.rule_new && (!a.ruled || (a.verdict.kind == b.verdict.kind && a.window.frame == b.window.frame)) &&
                              (!a.trial || (a.parts[1].n == b.parts[1].n && a.parts[2].sum.frame == b.parts[2].sum.frame)) &&
                              !b.scene && !b.moved && !b.recheck && !b.short_trial && prev.frames() == t.frames();
            differ += !same, steps++;
            flips += a.flip, trials += a.trial, ruled += a.ruled;
            if (a.flip) on_p = !on_p;
            if (b.flip) on_t = !on_t;
        }
    }
    CHECK(differ == 0);
    CHECK(flips > 200 && trials > 100 && ruled > 200);   // (the trials and the rule were compared where they act)
    std::printf("prev: %lld presents in 40 runs with the back-off off (the rule on in half): %lld changes of way, %lld trials, %lld "
                "windows judged; %lld steps where the tuner and the one before the back-off differ\n", steps, flips, trials, ruled, differ);
}

/// A frame of a scene with the noise of a real one: 8% from frame to frame and a slow drift of about 4% (a crowd moving), the
/// draw counts moving with half the drift
struct Drift {
    std::mt19937 r;
    double d = 0;
    explicit Drift(uint32_t seed) : r(seed) {}
    Frame next(const Scene& sc, bool on) {
        std::normal_distribution<double> white(0.0, 0.08), step(0.0, 0.04 * std::sqrt(1.0 - 0.995 * 0.995));
        d = 0.995 * d + step(r);
        Frame f;
        f.frame = sc.frame * (1.0 + d + white(r)) * (on ? sc.par_factor : 1.0);
        f.replay = sc.replay * (1.0 + d + 0.5 * white(r)) * (on ? 0.6 : 1.0);
        f.gpu = sc.gpu * (1.0 + 0.8 * d + 0.5 * white(r));
        f.draws = std::round(sc.draws * (1.0 + 0.5 * d)), f.cut = std::round(sc.cut * (1.0 + 0.5 * d));
        f.op = sc.op * (1.0 + d + 0.5 * white(r)), f.parallel = on;
        return f;
    }
};

/// What a run of the tuner did, counted from a present on
struct Tally {
    long long frames = 0, trials = 0, short_trials = 0, rechecks = 0, scenes = 0, changes = 0, other = 0, right = 0;
    long long short_flipped = 0, recheck_not_followed = 0;   // (the rules a short trial keeps: both must stay 0)
    std::vector<long long> latency;   // (per scene of a schedule: presents from its start until the way decided was its best)
};
/// A schedule's scenes, each for `each` presents, through a tuner (backoff on or off), frames counted from `from`. The way
/// decided is the one after the rule's or a trial's change; "other" counts the frames presented the other way (a trial's), and
/// "right" those presented with the scene's better way (the helper threads where they shorten the frame)
Tally run_schedule(const std::vector<Scene>& sched, int each, int rounds, bool backoff, uint32_t seed, long long from,
                   bool use_rule = true, int load_every = 0) {
    Tuner t;
    t.use_rule = use_rule, t.backoff = backoff, t.threads = 4;
    Drift noise(seed);
    bool on = false, decided = false;
    long long recheck_at = -1;
    Tally out;
    const long long total = (long long)each * (long long)sched.size() * rounds;
    long long scene_start = 0, found = -1;
    for (long long i = 0; i < total; i++) {
        const Scene& sc = sched[(size_t)((i / each) % (long long)sched.size())];
        if (i % each == 0) {
            if (i && found < 0) out.latency.push_back(each);
            scene_start = i, found = -1;
        }
        const bool best = sc.par_factor < 1.0;
        if (found < 0 && decided == best) found = i - scene_start, out.latency.push_back(found);
        Frame f = noise.next(sc, on);
        if (load_every && i % load_every < 40) f.frame = 80.0, f.draws = 6, f.cut = 0;   // (a load: 40 frames of 80 ms)
        const Tuner::Step s = t.step(f, on, false);
        const bool counted = i >= from;
        if (counted) {
            out.frames++;
            out.other += on != decided;
            out.right += on == best;
        }
        if (s.trial) {
            if (counted) out.trials++, out.short_trials += s.short_trial, out.rechecks += s.recheck, out.changes += s.better;
            if (s.short_trial && s.flip) out.short_flipped++;
            if (recheck_at >= 0 && (t.frames() != recheck_at + 2 * (Tuner::kSettle + Tuner::kTake) + Tuner::kTake + 1 || s.short_trial))
                out.recheck_not_followed++;
            recheck_at = s.recheck ? t.frames() : -1;
        }
        if ((s.scene || s.moved) && counted) out.scenes++;   // (seen between trials, or under one)
        if (s.flip) {
            on = !on;
            if (s.trial || s.ruled) decided = on;
        }
    }
    return out;
}

void backoff_case() {
    // (the lab's Market, f24-S1-1's trial parts: 12.4 ms frames on the replay thread alone, 10.4 with the helper threads, the
    // replay 9.6 ms of it, the GPU 7.3, ~3,250 draws and dispatches, 3,150 of them in command buffers to cut)
    const Scene market{12.4, 9.6, 7.3, 3250, 3150, 7.7, 0.84};
    CHECK(rule(mean_of(market), 4).kind == Verdict::kOn);   // (as the log has it: on without a trial at the Market's arrival)
    const long long from = 20000;
    const int per = 10000;
    {   // a steady scene where the helpers pay, 200,000 presents measured
        const Tally o = run_schedule({market}, 220000, 1, false, 21, from), n = run_schedule({market}, 220000, 1, true, 21, from);
        const double ot = o.trials * (double)per / o.frames, nt = n.trials * (double)per / n.frames;
        const double oo = o.other * (double)per / o.frames, no = n.other * (double)per / n.frames;
        CHECK(ot > 4.3 && ot < 5.2);   // (the model of the lab: a trial every ~2,090 presents without the back-off)
        CHECK(nt <= 1.0);
        CHECK(no * 8 <= oo);
        CHECK(n.changes == 0 && n.scenes == 0);   // (no false change, no false scene change)
        CHECK(n.right >= o.right && n.right > 0.995 * n.frames);
        CHECK(n.short_trials >= n.trials - 1);   // (all short once held, but maybe the first counted)
        CHECK(n.short_flipped == 0 && n.recheck_not_followed == 0);
        std::printf("backoff: the Market, steady: %.2f trials per 10,000 presents (%.2f without the back-off), %.0f frames the other "
                    "way (%.0f), %lld short, %lld changes (%lld), %lld scene changes; the helper threads in %.2f%% of the frames (%.2f%%)\n",
                    nt, ot, no, oo, n.short_trials, n.changes, o.changes, n.scenes, 100.0 * n.right / n.frames, 100.0 * o.right / o.frames);
    }
    {   // a steady scene where they do not (the trials decide there, the replay thread alone wins by 10%)
        Scene slow = test7_busy;
        slow.par_factor = 1.10;
        CHECK(rule(mean_of(slow), 4).kind == Verdict::kTrial);
        const Tally o = run_schedule({slow}, 220000, 1, false, 22, from), n = run_schedule({slow}, 220000, 1, true, 22, from);
        const double nt = n.trials * (double)per / n.frames;
        CHECK(nt <= 1.0 && n.other * 8 <= o.other && n.changes <= o.changes && n.right >= o.right);
        CHECK(n.short_flipped == 0 && n.recheck_not_followed == 0);
        std::printf("backoff: a steady scene where the helper threads lose: %.2f trials per 10,000 presents (%.2f without), %lld "
                    "frames the other way (%lld)\n", nt, o.trials * (double)per / o.frames, n.other, o.other);
    }
    {   // scenes that change every 8,000 presents: the Market (the rule turns the helpers on), one where they lose by 8% and
        // the trials decide, one where they win by 10% and the trials decide, one where the game's threads are the limit (no
        // trial: the rule). Their draw counts differ as the areas' did in the lab's tour
        const Scene lose{12.0, 9.6, 8.4, 1500, 1400, 3.4, 1.08};
        Scene win = test8_play;
        win.par_factor = 0.90;
        const Scene game = test7_trial4;
        CHECK(rule(mean_of(lose), 4).kind == Verdict::kTrial && rule(mean_of(win), 4).kind == Verdict::kTrial);
        CHECK(rule(mean_of(game), 4).kind == Verdict::kOff);
        const std::vector<Scene> sched{market, lose, win, game};
        long long o_trials = 0, n_trials = 0, o_right = 0, n_right = 0, o_other = 0, n_other = 0, n_max = 0, o_max = 0, frames = 0;
        long long n_scenes = 0, n_slow = 0, o_slow = 0;
        double o_lat = 0, n_lat = 0;
        int lat_n = 0;
        const uint32_t runs = 30;
        for (uint32_t seed = 30; seed < 30 + runs; seed++) {
            const Tally o = run_schedule(sched, 8000, 5, false, seed, 0), n = run_schedule(sched, 8000, 5, true, seed, 0);
            CHECK(n.short_flipped == 0 && n.recheck_not_followed == 0);
            o_trials += o.trials, n_trials += n.trials, o_right += o.right, n_right += n.right, o_other += o.other, n_other += n.other;
            frames += n.frames, n_scenes += n.scenes;
            for (size_t k = 1; k < n.latency.size() && k < o.latency.size(); k++) {   // (not the first scene: the start)
                n_lat += (double)n.latency[k], o_lat += (double)o.latency[k], lat_n++;
                n_max = std::max(n_max, n.latency[k]), o_max = std::max(o_max, o.latency[k]);
                n_slow += n.latency[k] > 4000, o_slow += o.latency[k] > 4000;
            }
        }
        CHECK(n_trials <= o_trials);
        CHECK(n_other < o_other);
        CHECK(n_right >= o_right);
        CHECK(n_lat <= o_lat);       // (as soon after a change, on average)
        CHECK(n_slow <= o_slow);     // (and as seldom late: the trials' noise leaves some changes found late either way)
        CHECK(n_max <= o_max);
        CHECK(n_scenes >= runs * 5 * 3 * 9 / 10);   // (nearly every change seen, between trials or under one; at the Market's
                                                    // arrival the rule's own change comes first)
        std::printf("backoff: scenes changing every 8,000 presents (%u runs of 160,000): %lld trials (%lld without the back-off), %lld "
                    "frames the other way (%lld), the better way in %.2f%% of the frames (%.2f%%); the better way %.0f presents after a "
                    "change on average (%.0f), %lld at most (%lld), %lld of %d changes found after 4,000 presents (%lld); %lld scene "
                    "changes seen\n", runs, n_trials, o_trials, n_other, o_other, 100.0 * n_right / frames, 100.0 * o_right / frames,
                    n_lat / lat_n, o_lat / lat_n, n_max, o_max, n_slow, lat_n, o_slow, n_scenes);
    }
    {   // a change the scene's means cannot see (the frame alone changes): a backed-off short trial finds it, a full one decides
        Scene before = test8_play, after = test8_play;
        before.par_factor = 0.85, after.par_factor = 1.15;
        Tuner t;
        t.use_rule = true, t.threads = 4;
        Drift noise(41);
        bool on = false;
        long long rechecks = 0, changed_at = -1, changed_by_short = 0, recheck_last = -1, changes_before = 0;
        for (long long i = 0; i < 80000; i++) {
            const Scene& sc = i < 40000 ? before : after;
            const Tuner::Step s = t.step(noise.next(sc, on), on, false);
            if (s.trial && s.recheck && i >= 40000) rechecks++, recheck_last = t.frames();
            if (s.trial && s.better) {
                if (i < 40000) changes_before++;
                else if (changed_at < 0) changed_at = i, changed_by_short += s.short_trial;
            }
            if (s.flip) on = !on;
        }
        CHECK(changes_before == 1);   // (the first trial turned them on; the back-off kept them)
        CHECK(rechecks >= 1 && changed_at > 0 && changed_by_short == 0);
        CHECK(changed_at - 40000 <= Tuner::kBackoffMax + 3 * 289);
        CHECK(recheck_last > 0 && changed_at + 1 - recheck_last == 289);   // (the full trial right after the short one)
        CHECK(!on);
        std::printf("backoff: a change only the frame shows: a short trial found it, a full one changed the way %lld presents after "
                    "it (at most %d apart, and two trials)\n", changed_at - 40000, Tuner::kBackoffMax);
    }
    {   // loads are no scene change, and the rule's no-trial verdicts hold through scene changes (two scenes where it cannot pay)
        const Tally l = run_schedule({market}, 120000, 1, true, 51, from, true, 5000);
        CHECK(l.scenes == 0 && l.changes == 0);
        const Tally g = run_schedule({gpu_bound, test7_trial4}, 3000, 20, true, 52, 0);
        CHECK(g.trials == 0 && g.other == 0);
        std::printf("backoff: a load every 5,000 presents: %lld scene changes; two no-trial scenes changing every 3,000 presents: %lld "
                    "trials\n", l.scenes, g.trials);
    }
    // the interval: 2 x kEvery after one trial that kept the way, doubling, never past kBackoffMax; and scene_moved's margins
    CHECK(Tuner::put_off(0) == 1800 && Tuner::put_off(1) == 1800 && Tuner::put_off(2) == 3600 && Tuner::put_off(3) == 7200);
    CHECK(Tuner::put_off(4) == Tuner::kBackoffMax && Tuner::put_off(30) == Tuner::kBackoffMax);
    {
        Frame a = mean_of(market), b = a;
        CHECK(!Tuner::scene_moved(a, b));
        b.draws = a.draws * 1.24;
        CHECK(!Tuner::scene_moved(a, b));
        b.draws = a.draws * 1.26;
        CHECK(Tuner::scene_moved(a, b));
        b = a, b.replay = a.replay * 0.71;
        CHECK(!Tuner::scene_moved(a, b));
        b.replay = a.replay * 0.69;
        CHECK(Tuner::scene_moved(a, b));
        b = a, b.gpu = a.gpu * 1.31;
        CHECK(Tuner::scene_moved(a, b));
        Frame small = mean_of(title), s2 = small;   // (the floors: 200 draws, 1 ms)
        s2.draws = small.draws * 3, s2.replay = small.replay * 2, s2.gpu = small.gpu * 2;
        CHECK(!Tuner::scene_moved(small, s2));
    }
}

int main(int argc, char** argv) {
    const std::string c = argc > 1 ? argv[1] : "";
    if (c == "same") same_case();
    else if (c == "prev") prev_case();
    else if (c == "rule") rule_case();
    else if (c == "backoff") backoff_case();
    else return std::printf("which case?\n"), 2;
    return failures ? 1 : 0;
}
