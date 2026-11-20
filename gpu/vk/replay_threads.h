// (Triforce) The Debug tab's Replay helper threads row: what its Automatic gives a PC, and what the row is when the options
// file does not say (vk_replay.cpp par_threads_max, vk_menu.cpp). tests/test_replay_threads_row.py
//
// The row's values: 0 Off, 1 to 7 the parallel replay tried on 2 to 8 threads (what RX_VK_PARALLEL_AUTO=<n> does), and 8
// Automatic: the thread count below, tried the same way.
//
// Automatic's rule, after NX2navi's replay_workers = auto (its vk_exec_replay_workers_auto: half the physical cores less one,
// none for four cores or fewer, none on AMD's Windows driver for RX 6000 and older, where players' games lost the device with
// workers on by default and the cause was never found), fitted to what this renderer measured (three threads at the Castle
// Town save, 67.1 fps against 58.8; kMaxBatch: more than three recording threads only took slots and submits):
// - four cores or fewer: off. The game's threads, the replay and the captures already share them (docs/design.md, "The CPU
//   saturates first").
// - five to seven cores: 2 threads. Eight or more: 3.
// - AMD's driver for RDNA 1 and 2 (vk_vendor.h amd_older_compiler): off.
// The cores are taken as half the logical processors: a processor without SMT gets the smaller count.
//
// What the row is when the file has no replay_helper_threads: Automatic, as NX2navi has its workers and as the owner asked
// (2026-11-15). A file that says 0 means Off, as it did: one written since the row exists has the line, so only a new
// options file, or one from before the row, takes the new default. Off on the Debug tab (after a restart) or
// RX_VK_PARALLEL=0 is the replay thread alone, always.
//
// Automatic only says how many threads; the tuner (Tuner below, called by vk_replay.cpp par_tune_present) decides when: the
// game starts on the replay thread alone, and every 900 presents or so the other way is tried for about a hundred frames and
// kept when its frames are at least 4% shorter than before and after. So on a PC or in a scene where the threads do not help,
// they record only those trial frames. (Triforce, 11-15) And its rule (rule below): where the replay is plainly the frame's
// limit they go on without a trial, and where they cannot pay no trial is made. (Triforce, 11-16) And once a way has held,
// trials come ever rarer (up to some 14,400 presents apart) and shorter, and early only when the scene clearly changes (the
// back-off, at Tuner).
//
// The parallel replay was audited for this by reading (nothing could be run). What was found and is fixed, each with a
// check in tests/test_parallel_fixes.py:
// - a data race: vk_draw.cpp upscale_stub, temporal_rejected_network and typed_upscale (through taau_stand_in) used the
//   texture cache and their own pipelines and history images without the recording lock, from a dispatch a segment's worker
//   records. They take ParLock around those now, and never across the temporal upscaler's own calls.
// - an ordering hole: the dry pass marked what draws and clears of a pass write, not what a texture copy, a
//   buffer-to-texture copy or a texture clear writes. A later segment's upload of the game's memory landed over the
//   earlier segment's copy: one-frame black or stale textures, for good when the copy was made once. vk_xfer.cpp
//   dry_copy_b2t, dry_copy_t2t and dry_clear_tex mark them, and the copies the CPU makes are made in the dry pass.
// - the page table's changes reached the queue in the segments' order, not in the order they were made (vk_memory.cpp
//   table_pending is each thread's): a pool imported by a later segment and retired by an earlier one ended with its
//   pages pointing at the retired pool. They are written once more, in order, after the last segment
//   (memory_table_segments_end).
// - a temporal upscaler's hooks in the draws, and those of a frame generation that takes the scene's camera (AMD FSR's,
//   NVIDIA DLSS's), are told things in the order the draws are recorded
//   ("the first draw after the scene's last", the scene's camera). While one was on, the frame was recorded on the replay
//   thread alone. (Triforce, 11-15) Now those hooks are told in the dry pass, in stream order, up to the first draw or
//   dispatch that takes what they noted, and the command buffer from there is recorded in order after the segments before
//   it; the skinned draws are named again in stream order where they are taken (stream_order.h, tests/test_stream_order.py).
//   RX_VK_PARALLEL_TEMPORAL=0: the replay thread alone with them on, as before (vk_replay.cpp par_stream_order_only).
// - a texture's look without the lock could pass while another worker was still uploading it: touch_quick.h.
// - a flat texture's one-slice 3D copy got its first layout change in the maker's own uploads: through InitRedirect now,
//   and copied again by a segment before the maker's.
//
// What is left, each a frame's flaw at most, none seen or ruled out in the game; RX_VK_PARALLEL=0 or the row's Off avoids
// them all:
// - a pool first bound, or retired, inside a frame recorded in segments: a segment earlier in the queue than the one that
//   made the change does not have it until the frame's end, and a pool first bound there is read from the game's memory
//   for that frame, its mirror made at the next (vk_memory.cpp gpu_buffer_bind, "wanted"). A frame of missing or wrong
//   geometry, foliage or shadow where shaders follow pointers into that pool, at the moment the pool appears.
// - the dry pass does not mark a storage image a draw's program writes (only a dispatch's), nor what upscale_stub writes
//   for a program that did not translate: such a texture can still be uploaded over by a later segment, as above.
// - a texture the GPU only cleared (the game's font cache) whose new letters a later segment uploaded: an earlier
//   segment's draws of that frame do not have them yet. A letter a frame late.
// - the CPU's buffer-to-texture copies are made in the dry pass: a draw before such a copy in the same command buffer
//   samples the texels after it.
// - the segments' page table changes, written once more in the recording after them (memory_table_segments_end), would
//   undo a change of the same pages that a thread not recording wrote at once in the meantime (until those pages change
//   again), as the replay thread's own deferred changes can in a serial replay. The threads that record defer theirs; whether
//   another writes the table (under the pools' lock, pool_of) while a frame is recorded was not traced to the end.
// - the tuner's trials are short, and a scene's frame time moves by more than what they measure (vk_replay.cpp, above
//   par_threads_max): it may change over, or back, for no gain. The rule's thresholds are fitted to four of the owner's
//   sessions on one PC, not measured on others.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace replay_threads {

/// The row's value for Automatic, and the value the row has when the options file does not name it (see above)
inline constexpr int kRowAutomatic = 8;
inline constexpr int kDefaultRow = kRowAutomatic;

/// The threads Automatic records a frame's large command buffers on: 0, none (the replay thread alone), on every PC.
/// (Triforce, 11-17) It was 2 or 3 with five cores or more. The owner's playtests on a 12-core PC (24 logical, RTX, 2560x1440):
/// with helper threads on (Automatic's 3, or the row's 3 or 8) moving and jumping brought stretches of 10-25 s at 39-48 fps,
/// the game's own frame time going from about 4 to 10-13 ms and the game slowing then catching up, on the merged build and
/// on the build before it; with the row's None, twice (both builds), none of it, at the same frame rate otherwise (52-74).
/// The helper threads take the processors the game's own threads need while it streams and makes textures. The row's
/// counts are still there for a tester. logical, amd_older: as before, for the log's line (why)
constexpr int automatic(unsigned logical, bool amd_older) {
    (void)logical, (void)amd_older;
    return 0;
}

/// Why, for the log's line at the start
constexpr const char* why(unsigned logical, bool amd_older) {
    (void)logical, (void)amd_older;
    return "none: the helper threads slowed the game's own threads when moving (11-17); the Debug tab's row still sets a count";
}

/// The threads a value of the row stands for: 0 for Off and for anything the row does not have, one more than the value
/// for 1 to 7 (there is no "1 thread": that is the replay thread alone), and automatic_threads for Automatic
constexpr int of_row(int row, int automatic_threads) {
    if (row == kRowAutomatic) return automatic_threads;
    return row >= 1 && row <= 7 ? row + 1 : 0;
}

/// (Triforce, 11-15) One frame as the tuner sees it: vk_replay.cpp par_tune_present takes it from the replay's counters at each
/// present. Times in ms. What a trial's line says of each of its three parts, so that a session's log tells why the helper
/// threads' frames were longer or shorter (in the owner's test7 every trial kept the replay thread alone, and the log had only
/// the three means)
struct Frame {
    double frame = 0;      // present to present
    double replay = 0;     // the replay thread's own time in the frame's submissions (the replay time line's submit)
    double gpu = 0;        // the GPU's: the recordings' command buffers and their uploads, the segments' too (read as their slots
                           // come round again: a frame or two late)
    double barriers = 0;   // full barriers recorded
    double draws = 0;      // draws and dispatches
    double cut = 0;        // of them in command buffers large enough for two segments or more
    double op = 0;         // the draw path's time (draw() and dispatch(); the helper threads' too while they record)
    // (the parallel replay's parts, vk_replay.cpp ParStats: zero on the replay thread alone)
    double segmented = 0;  // command buffers recorded in segments
    double dry = 0, setup = 0, rec = 0, own = 0, waited = 0, submit = 0, first = 0;
    bool parallel = false; // (recorded with the helper threads in use)
};

/// A part of a trial: the frames counted in it, summed
struct Part {
    Frame sum;
    int n = 0;
    void add(const Frame& f) {
        sum.frame += f.frame, sum.replay += f.replay, sum.gpu += f.gpu, sum.barriers += f.barriers, sum.draws += f.draws;
        sum.cut += f.cut, sum.op += f.op, sum.segmented += f.segmented, sum.dry += f.dry, sum.setup += f.setup;
        sum.rec += f.rec, sum.own += f.own, sum.waited += f.waited, sum.submit += f.submit, sum.first += f.first;
        sum.parallel = sum.parallel || f.parallel;
        n++;
    }
    Frame mean() const {
        Frame m = sum;
        const double k = n ? 1.0 / n : 0.0;
        m.frame *= k, m.replay *= k, m.gpu *= k, m.barriers *= k, m.draws *= k, m.cut *= k, m.op *= k, m.segmented *= k;
        m.dry *= k, m.setup *= k, m.rec *= k, m.own *= k, m.waited *= k, m.submit *= k;
        m.first = sum.segmented > 0 ? sum.first / sum.segmented : 0.0;   // (each command buffer's, not a frame's)
        return m;
    }
};

// (Triforce, 11-15) The rule: where the helper threads plainly pay, on without a trial; where they cannot, no trial; trials for
// the rest. From the owner's own logs (test4, test5, test7, test8; NX2navi's figures are another engine's):
// - A frame of OoT here is a chain: the game's Presentation thread replays it and then waits for its GPU work to end (its
//   event, vk_replay.cpp RX_VK_EVENT_EARLY) before the next frame is built. So the helper threads shorten the frame by the
//   time they take off the replay thread, but never below the GPU's time: in test8 they took 1.65 to 1.8 ms off the replay
//   thread (7.63 to 5.84-5.98 ms a frame) and the frame did not get shorter, the GPU's time being 6.4 + 0.7 ms (its command
//   buffers and uploads) of a 9.3 ms frame. The gain is at most the replay's time over the GPU's, less what the helpers add
//   to the GPU and to its start (kGpuExtra, a guess the trials' parts lines will measure: 70 more full barriers a frame in
//   test8, the first segment's own recording before anything reaches the GPU).
// - Where the replay is not most of the frame the game's own threads are its limit, and three more threads at above-normal
//   priority take processor time from them: test7's trial 4, 11.08 ms frames with 6.3 ms of replay and 5.6 of GPU, was 12.31
//   ms with them. kShareOn, kShareOff.
// - What the helpers take off the replay thread: the draws and dispatches in command buffers large enough to cut, at what
//   one costs on the replay thread alone (its draw() and dispatch() time over their count), less what the replay thread still
//   records of them (one share of `threads`), less the dry pass (kDryPerOp a draw: 0.44 ms a frame for ~2,300 in test8).
// It is cautious both ways: "on" counts on the gain less kGpuExtra, "no trial" only where even without it nothing is to be won.
// Against test4's heaviest scene (9.71 ms of replay, 6.4 of GPU, a 12.0 ms frame, 2,870 draws and dispatches) it says on: the
// trial there kept the helpers, and frames went from 75 to 86-90 fps with more draws. Against test7's trial 4 and the title it
// says no trial; test8's play (7.63 of 7.1, 9.34: a trial there kept them, for about 2%) and test7's busy scenes (9.6 of 8.4,
// 12.0) are trials.
// RX_VK_PARALLEL_RULE=0 (vk_replay.cpp): trials only, as before.
struct Verdict {
    enum Kind { kTrial, kOn, kOff };
    Kind kind = kTrial;
    double saved = 0;   // what the helper threads would take off the replay thread a frame (ms)
    double gain = 0;    // what the frame would get shorter by, at most (ms)
    double share = 0;   // the replay thread's share of the frame
    const char* why = "";
};
inline constexpr double kGpuExtra = 0.5, kDryPerOp = 0.0002, kOnMs = 1.0, kOnPart = 0.10, kOffPart = 0.02;
// (the replay thread's share of the frame: 0.78 to 0.85 in the logs' scenes where the replay was the limit, 0.47 to 0.56 in
// test7's trial 4)
inline constexpr double kShareOn = 0.70, kShareOff = 0.60;

/// m: a window's mean frame on the replay thread alone; threads: the threads that would record segments (the helper threads,
/// and the replay thread when it records too)
inline Verdict rule(const Frame& m, int threads) {
    Verdict v;
    const auto is = [&v](Verdict::Kind kind, const char* why) {
        v.kind = kind, v.why = why;
        return v;
    };
    if (m.frame <= 0 || threads < 2) return is(Verdict::kOff, "no frames, or no helper threads");
    v.share = m.replay / m.frame;
    const double per_op = m.draws > 0 ? m.op / m.draws : 0.0;
    v.saved = std::max(0.0, m.cut * per_op * (1.0 - 1.0 / threads) - m.cut * kDryPerOp);
    // (cautious both ways: "on" counts on the gain less kGpuExtra; "no trial" only where even without it there is nothing to win)
    const double over_gpu = m.replay - m.gpu;
    v.gain = std::max(0.0, std::min(v.saved, over_gpu - kGpuExtra));
    if (m.cut <= 0) return is(Verdict::kOff, "no command buffer large enough to cut");
    if (v.share < kShareOff)
        return is(Verdict::kOff, "the replay is not the frame's limit: the game's threads are, and the helpers would take from them");
    if (over_gpu <= kOffPart * m.frame)
        return is(Verdict::kOff, "the GPU's time is about as long as the replay's: the frame waits for it either way");
    if (std::min(v.saved, over_gpu) <= kOffPart * m.frame) return is(Verdict::kOff, "less to win than a trial could tell");
    if (v.share >= kShareOn && v.gain >= std::max(kOnMs, kOnPart * m.frame))
        return is(Verdict::kOn, "the replay is the frame's limit and the helpers plainly take enough off it");
    return is(Verdict::kTrial, "not plain either way: the trials decide");
}

/// The tuner (vk_replay.cpp par_tune_present calls step at each present, the replay thread's; it was inline there). Every
/// kEvery presents a trial: kTake frames as they are, kTake the other way (after kSettle to settle) and kTake as they were
/// again, and a change when the other way's mean frame was at least 4% shorter than both, so that a scene that changed under the
/// trial does not pass for a gain; frames over 60 ms (a load) left out; the next trial kEvery presents after a change, twice
/// that after none. With the rule (use_rule): between trials, on the replay thread alone, every kWindow presents the window's
/// mean frame is judged (rule()): on, the helper threads at once (a trial follows kEvery presents later, as after a change);
/// no trial, the next one put off while that holds; else nothing changes. A trial that takes the helpers back off holds the
/// rule's "on" for kHold presents: the measurement goes over the model
/// (Triforce, 11-16) The back-off (backoff; vk_replay.cpp par_backoff_on, RX_VK_PARALLEL_BACKOFF=0: off, every step as before).
/// In the owner's steady Market (lab runs f24-S1-1, -2, r25-base-1, -2) every trial kept the helper threads, the other way 13
/// to 19% longer, and each trial held the frame ~1.9 ms longer for its ~100 frames the other way, one every ~2,090 presents:
/// 16 to 31% of the frame time's variance there. So, once a way has held:
/// - each trial that keeps the way in use, its other way longer than both of the way's own parts, puts the next one off twice
///   as long as the one before (2 x kEvery after the first, as before, then 4 and 8 x kEvery, up to kBackoffMax presents:
///   ~2.7 minutes at 90 fps), and one whose other way was clearly longer (by kClear or more) counts twice. One too close to
///   tell (the other way between the two) puts the next off 2 x kEvery, as before, and the first after a change of scene or
///   way (fresh_) kEvery: right after a change the first trial is the one most often wrong;
/// - with the way held through two such steps (held_ 2 or more) a trial's other way is kTakeShort frames after its settling,
///   not kTake. A short trial that finds the other way shorter changes nothing itself: a full trial follows at once, and only
///   a full one changes the way (the shorter mean is the noisier one: it may ask for a trial, not decide one);
/// - the next trial comes early when the scene clearly changes, not on a clock: every kWindow presents between trials the
///   window's mean draws and dispatches, replay thread's time and GPU time are held against those of the first window after
///   the way was last set or tried (the times differ with the way in use); two windows in a row off by more than kSceneDraws
///   of the draws (and kSceneDrawsMin) or kScenePart of either time (and kScenePartMs), and the back-off starts again, the
///   next trial kWindow presents on but never sooner than kEvery after the last one ended, nor while the rule says no trial
///   (its no-trial verdicts hold as before). In the lab runs' frames.csv the draw counts alone moved so at every area's load
///   and arrival, and never in the 8,700 presents of the steady Market after its arrival (f24-S1-1, -2, r25-base-1, -2). A
///   trial whose first and last parts (the same way) moved so had the scene change under it: the windows after it are the
///   new scene's reference, so the back-off starts again there too, the next trial kEvery on.
/// A change of way, by a trial or by the rule, starts the back-off again. tests/test_replay_threads_tuner.py "backoff"
class Tuner {
public:
    static constexpr int kEvery = 900, kTake = 80, kSettle = 24, kWindow = 120, kHold = 4 * kEvery;
    static constexpr int kTakeShort = 40, kBackoffMax = 16 * kEvery;
    static constexpr double kClear = 1.10, kSceneDraws = 0.25, kSceneDrawsMin = 200, kScenePart = 0.30, kScenePartMs = 1.0;
    bool use_rule = true;
    bool backoff = true;   // (the back-off above; false: every step as before it)
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
        // (the back-off's)
        bool short_trial = false;  // (the trial that ended had kTakeShort frames the other way)
        bool recheck = false;      // (...and found the other way shorter: nothing changed, a full trial begins at once)
        bool scene = false;        // (the scene changed: the back-off starts again, the next trial brought forward)
        bool moved = false;        // (the scene changed under the trial that ended: the back-off starts again)
        bool sure = false;         // (the trial kept the way, the other way longer than both its parts: the back-off grows)
        int64_t next = 0;          // (the present the next trial may begin at: set at a trial's end and at a scene change)
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
                            if (backoff) held_ = 0, fresh_ = true, scene_restart();
                        } else if (s.verdict.kind == Verdict::kOff) {
                            next_trial_ = std::max(next_trial_, frames_ + kEvery);
                            no_trial_until_ = frames_ + kEvery;
                        }
                    }
                    win_ = Part{}, win_frames_ = 0;
                    if (s.flip) return s;
                }
            } else {
                win_ = Part{}, win_frames_ = 0;
            }
            if (backoff) scene_look(f, s);
            if (frames_ < next_trial_) return s;
            if (order_only) return s;
            phase_ = 1, n_ = 0;
            short_ = backoff && held_ >= 2;
            win_ = Part{}, win_frames_ = 0;   // (the rule's next window begins after the trial)
            sum_[0] = sum_[1] = sum_[2] = 0, cnt_[0] = cnt_[1] = cnt_[2] = 0;
            part_[0] = part_[1] = part_[2] = Part{};
            return s;
        }
        const int k = phase_ - 1;
        const int take = phase_ == 2 && short_ ? kTakeShort : kTake;   // (only the other way is shorter: the rest costs nothing)
        n_++;
        if (n_ > (phase_ == 1 ? 0 : kSettle) && f.frame <= 60.0) sum_[k] += f.frame, cnt_[k]++, part_[k].add(f);
        if (n_ < (phase_ == 1 ? take : kSettle + take)) return s;
        n_ = 0;
        if (phase_ == 1) {
            s.flip = true, phase_ = 2;
        } else if (phase_ == 2) {
            s.flip = true, phase_ = 3;
        } else {
            phase_ = 0, trials_++;
            const int took = short_ ? kTakeShort : kTake;
            s.ok = cnt_[0] >= kTake * 3 / 4 && cnt_[1] >= took * 3 / 4 && cnt_[2] >= kTake * 3 / 4;
            s.a = cnt_[0] ? sum_[0] / cnt_[0] : 0, s.b = cnt_[1] ? sum_[1] / cnt_[1] : 0, s.c = cnt_[2] ? sum_[2] / cnt_[2] : 0;
            s.better = s.ok && s.b < std::min(s.a, s.c) * 0.96;
            s.short_trial = short_;
            if (s.better && short_) s.better = false, s.recheck = true;   // (a full trial decides: at once, below)
            if (s.better) {
                s.flip = true, changes_++;
                if (on) hold_until_ = frames_ + kHold;   // (the trial took them off: the rule's "on" waits)
            }
            if (!backoff) {
                next_trial_ = frames_ + (s.better ? kEvery : kEvery * 2);
            } else {
                // (a scene that changed under the trial: its first and last parts, the same way, describe two scenes. The
                // windows after it are the new scene's reference, so the change is not seen again: the next trial kEvery on,
                // as after a change, not put off)
                // (and only a trial whose other way was longer than both of the way's parts holds it: one in between, too close
                // to tell, puts the next off 2 x kEvery as before, so that a wrong "as it was" right after a change is not
                // put off ever longer; such a trial costs little, its two ways' frames being about as long. The first trial
                // after a change of scene or way that is too close to tell: kEvery, a second look soon. The trials' means
                // drift with the scene, and right after a change the first is the one most often wrong)
                s.moved = cnt_[0] && cnt_[2] && scene_moved(part_[0].mean(), part_[2].mean());
                const double top = std::max(s.a, s.c);
                s.sure = s.ok && !s.better && !s.recheck && !s.moved && s.b > top;
                const bool again = !s.sure && fresh_;
                fresh_ = s.better || s.moved || s.recheck;
                if (s.better || s.recheck || s.moved) held_ = 0;
                else if (s.sure) held_ = std::min(held_ + (s.b >= top * kClear ? 2 : 1), 30);
                next_trial_ = frames_ + (s.recheck ? 1 : s.better || s.moved || again ? kEvery : s.sure ? put_off(held_) : 2 * kEvery);
                last_end_ = frames_;
                scene_restart();
            }
            s.next = next_trial_;
            s.trial = true, s.parts = part_;
        }
        s.trials = trials_, s.changes = changes_;
        return s;
    }
    int64_t frames() const { return frames_; }
    /// (the back-off's interval after trials that kept the way, held_ steps of them: 2 x kEvery for none or one, doubling with
    /// each step, up to kBackoffMax)
    static int64_t put_off(int held) {
        const int64_t base = 2 * (int64_t)kEvery;
        return held <= 1 ? base : std::min<int64_t>(base << std::min(held - 1, 16), kBackoffMax);
    }
    /// (whether the scene a window's means describe moved from the reference's: the back-off's early trial)
    static bool scene_moved(const Frame& ref, const Frame& now) {
        const auto off = [](double was, double is, double part, double least) { return std::fabs(is - was) > std::max(part * was, least); };
        return off(ref.draws, now.draws, kSceneDraws, kSceneDrawsMin) || off(ref.replay, now.replay, kScenePart, kScenePartMs) ||
               off(ref.gpu, now.gpu, kScenePart, kScenePartMs);
    }

private:
    void scene_restart() { scene_win_ = Part{}, scene_frames_ = 0, ref_set_ = false, strikes_ = 0; }
    /// (between trials: the scene's windows against the reference, the next trial brought forward when two in a row moved)
    void scene_look(const Frame& f, Step& s) {
        if (f.frame <= 60.0) scene_win_.add(f);
        if (++scene_frames_ < kWindow) return;
        const bool counted = scene_win_.n >= kWindow * 3 / 4;
        const Frame m = scene_win_.mean();
        scene_win_ = Part{}, scene_frames_ = 0;
        if (!counted) return;   // (a load: neither a reference nor a change)
        if (!ref_set_) {
            ref_ = m, ref_set_ = true, strikes_ = 0;
            return;
        }
        if (!scene_moved(ref_, m)) {
            strikes_ = 0;
            return;
        }
        if (++strikes_ < 2) return;
        strikes_ = 0, ref_ = m, held_ = 0, fresh_ = true, s.scene = true;
        const int64_t at = std::max({frames_ + kWindow, last_end_ + kEvery, no_trial_until_});
        if (at < next_trial_) next_trial_ = at;
        s.next = next_trial_;
    }
    int phase_ = 0, n_ = 0, cnt_[3] = {0, 0, 0};
    double sum_[3] = {0, 0, 0};
    Part part_[3];
    Part win_;
    int win_frames_ = 0;
    int64_t frames_ = 0, next_trial_ = 600, trials_ = 0, changes_ = 0, hold_until_ = 0;
    Verdict::Kind last_ = Verdict::kTrial;
    // (the back-off's: trials that kept the way since it was last set, in held_ steps; the trial in progress a short one; no
    // trial since the scene or the way changed (fresh_); the last trial's end; the rule's "no trial" until; the scene's window,
    // its reference and the windows in a row that moved)
    int held_ = 0;
    bool short_ = false, fresh_ = true;
    int64_t last_end_ = 0, no_trial_until_ = 0;
    Part scene_win_;
    int scene_frames_ = 0, strikes_ = 0;
    bool ref_set_ = false;
    Frame ref_;
};

} // namespace replay_threads
