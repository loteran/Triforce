// (Triforce) What a draw does whose pipeline is not ready yet (vk_draw.cpp draw, graphics_pipeline), apart from Vulkan so that
// tests/test_pipe_wait.py runs it. NX2navi's pipeline-miss rules (runtime/vk_exec_draw.c prepare_draw_ex, frame_wait,
// last_wait, link_budget; runtime/vk_target_frames.h tfr_pass_recent; runtime/vk_pipeline.c lib_worker; by chrg6 and
// epicmeme546, at 62cc3b13) brought to Triforce's own design: the rules and their numbers are NX2navi's, the code is
// written for this renderer.
//
// What a draw did before, and still does by default: one whose pipeline a worker still builds was left out when its targets
// held a picture from an earlier frame (vk_draw.cpp targets_persistent) and its shaders write no memory, so that the next
// frame draws it; any other waited for its pipeline however long that took (RX_VK_PIPE_SKIP=0: every one waited for).
// Triforce links a pipeline from its program's stage libraries on the draw's own thread at its first miss (gpl_rules.h), so
// a draw whose libraries the driver has is never left out; what is not ready after that is a compile on a worker.
//
// The rules added, each with its switch:
//  - kept(): a pass with a colour target drawn for the first time counts as one drawn once, whatever its depth target is
//    (NX2navi's NX2NAVI_PIPE_SKIP=any). The game's item icon capture draws into a new colour target with the per-frame depth
//    buffer, once: a draw left out there stayed missing from the icon until the item was captured again. Its draws wait, as
//    for any target drawn once. RX_VK_PIPE_SKIP_NEW=0: the depth target alone keeps the pass, as before.
//  - choose(), a full-screen draw (at most 6 vertices over half the pass or more: a lighting, fog or tone mapping pass) into
//    kept targets: left out, its frame lacks the pass, a black or unlit picture with the interface intact. Where the hold of
//    the last picture can take that frame (present_hold.h; the draws left out for a pipeline count as owed, vk_replay.cpp
//    hold_step) it is left out and the frame is not shown, as before the hold existed for pipelines. Where it cannot (the hold
//    at its bound, resting after one, or off) the draw waits for its pipeline from the frame's wait pool (NX2navi's
//    NX2NAVI_PIPE_FRAME_WAIT_MS, kFrameWaitMs) and then from kLastWaitMs (NX2navi's FULLSCREEN_LAST_MS), and is left out only
//    after both. NX2navi waits from the pool first and holds after; here the order is the other way: Triforce's link was tried
//    at the miss already, so what the draw would wait for is a compile, and a held frame costs the game's thread nothing.
//    RX_VK_PIPE_FRAME_WAIT_MS=<ms>: the pool; 0: left out, as before.
//  - choose(), a draw into targets drawn once or a draw whose shaders write memory: waited for as before, unless
//    RX_VK_PIPE_ONESHOT_BUDGET=1 (NX2navi's NX2NAVI_ONESHOT_BUDGET, on there): from the frame's wait pool, and left out once it
//    is spent. Off here: a draw left out of a target the game draws once (a volume, a lookup table) may leave it wrong until
//    the game draws it again, and a slow frame is better than a wrong one.
//  - LinkBudget: the synchronous links of a frame (graphics_pipeline) take at most RX_VK_PIPE_LINK_MS (NX2navi's
//    NX2NAVI_PIPE_LINK_MS, 6 there); past it a new pipeline is linked by a worker instead, its draws left out or waited for
//    meanwhile. 0 (the default): every link on the draw's thread, as before. NX2navi's other half of that rule, the 3 ms a
//    draw waits for one pipeline's link (NX2NAVI_PIPE_LINK_DRAW_MS), has no counterpart: its links are made by worker threads
//    and the draw waits for them, while here the link is made in the draw.
//  - libs_first(), a pipeline whose stage libraries were cold (or whose link the budget left to a worker): the worker builds
//    those libraries first and links the pipeline from them, the entry ready with that link, and the whole optimised build
//    follows as a job of its own (NX2navi's NX2NAVI_PIPE_FAST_WORKERS). Before, the whole build came first and the cold
//    libraries after it, so the draws of such a pipeline waited for the whole build. The same objects are made either way.
//    A draw that waits runs the first job itself (vk_jobs.cpp jobs_wait): the libraries and the link, not the whole build.
//    RX_VK_PIPE_FAST_WORKERS=0: the whole build first, as before.
// A wait from a budget is vk_jobs.cpp jobs_wait_for: the pipeline's job goes to the front of the queue, and is not run on the
// draw's thread (a compile there would outlast the bound).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace pipe_wait {

inline constexpr int64_t kMs = 1000000;
inline constexpr int64_t kFrameWaitMs = 50;   // (NX2navi's NX2NAVI_PIPE_FRAME_WAIT_MS default)
inline constexpr int64_t kLastWaitMs = 40;    // (NX2navi's FULLSCREEN_LAST_MS)
inline constexpr int64_t kLinkMs = 6;         // (NX2navi's NX2NAVI_PIPE_LINK_MS default; here 0 unless asked)
inline constexpr int64_t kMinWaitNs = kMs;    // (a wait shorter than this is none: NX2navi counts its waits in whole ms)
inline constexpr int kFullVertices = 6;       // (a full-screen triangle or quad, or a tile: NX2navi's count <= 6)

// ---- the pass: kept from an earlier frame, or drawn once

/// A target of the pass as kept() sees it: there, drawn in an earlier frame, or not drawn before this frame (drawn first in
/// this frame, or never)
struct Target {
    bool there = false;
    bool earlier = false;
};
inline Target target(bool there, uint64_t first_render_frame, uint64_t now) {
    return {there, there && first_render_frame && first_render_frame < now};
}

/// Whether a draw into this pass may be left out while its pipeline builds: the game draws its targets frame after frame.
/// n colour targets c, the depth target d. new_colour_rule (RX_VK_PIPE_SKIP_NEW): a colour target not drawn before this
/// frame makes a pass whose depth target alone was drawn before one drawn once. Without it: any target drawn before keeps
/// the pass (targets_persistent, as before)
inline bool kept(const Target* c, int n, Target d, bool new_colour_rule) {
    bool colour_earlier = false, colour_new = false;
    for (int i = 0; i < n; i++) {
        if (!c[i].there) continue;
        colour_earlier |= c[i].earlier;
        colour_new |= !c[i].earlier;
    }
    if (colour_earlier) return true;
    return d.earlier && !(new_colour_rule && colour_new);
}

/// A full-screen draw: direct, at most kFullVertices vertices in one instance, over half the pass or more (the scissor's
/// area as the draw has it, the whole target when it has none)
inline bool full_screen(bool mesh, bool indirect, int count, int instances, uint64_t area, uint64_t pass_area) {
    return !mesh && !indirect && count > 0 && count <= kFullVertices && instances <= 1 && pass_area &&
           area * 2 >= pass_area;
}

// ---- a presented frame's budgets

/// What the draws of one presented frame may still spend (any thread; a new frame number fills it again). The draws of the
/// parallel replay's helper threads share it: a frame's total is approximate where two of them charge at once
class Pool {
public:
    explicit Pool(int64_t per_frame_ns = 0) : per_frame_(per_frame_ns) {}
    void per_frame(int64_t ns) { per_frame_ = ns; }
    int64_t per_frame() const { return per_frame_; }
    int64_t left(uint64_t frame) {
        fill(frame);
        return left_.load(std::memory_order_relaxed);
    }
    void charge(uint64_t frame, int64_t ns) {
        fill(frame);
        int64_t was = left_.load(std::memory_order_relaxed);
        while (!left_.compare_exchange_weak(was, std::max<int64_t>(0, was - std::max<int64_t>(0, ns)), std::memory_order_relaxed)) {
        }
    }

private:
    void fill(uint64_t frame) {
        uint64_t was = frame_.load(std::memory_order_relaxed);
        if (was != frame && frame_.compare_exchange_strong(was, frame, std::memory_order_relaxed))
            left_.store(per_frame_, std::memory_order_relaxed);
    }
    int64_t per_frame_ = 0;
    std::atomic<uint64_t> frame_{~0ull};
    std::atomic<int64_t> left_{0};
};

/// The synchronous links of a frame (RX_VK_PIPE_LINK_MS): links() true while the frame's links have time left (always, with
/// no budget)
class LinkBudget {
public:
    explicit LinkBudget(int64_t per_frame_ns = 0) : pool_(per_frame_ns) {}
    bool on() const { return pool_.per_frame() > 0; }
    bool links(uint64_t frame) { return !on() || pool_.left(frame) > 0; }
    void spent(uint64_t frame, int64_t ns) {
        if (on()) pool_.charge(frame, ns);
    }

private:
    Pool pool_;
};

// ---- the draw's choice

enum class Act { Skip, Wait, WaitAll };
enum Kind { K_NONE, K_ONESHOT, K_FULL, K_LAST };

struct Rules {
    bool always_wait = false;      // (RX_VK_PIPE_SKIP=0: every pipeline waited for)
    bool oneshot_budget = false;   // (RX_VK_PIPE_ONESHOT_BUDGET=1)
    bool full_wait = true;         // (RX_VK_PIPE_FRAME_WAIT_MS not 0)
};

struct Choice {
    Act act = Act::Skip;
    int64_t ns = 0;      // (Wait: for at most this long, then left out)
    Kind kind = K_NONE;  // (Wait: which budget the time is charged to)
};

/// A draw whose pipeline is not ready. kept: its pass (kept()); writes: its shaders write memory; full: full_screen(); held:
/// the hold of the last picture would take this frame if it lacks a draw (present_hold.h DEFERS, the pipelines counted);
/// pool_ns, last_ns: what the frame's wait pool and its last wait have left
inline Choice choose(const Rules& r, bool kept, bool writes, bool full, bool held, int64_t pool_ns, int64_t last_ns) {
    if (r.always_wait) return {Act::WaitAll, 0, K_NONE};
    if (!kept || writes) {
        if (!r.oneshot_budget) return {Act::WaitAll, 0, K_NONE};
        if (pool_ns >= kMinWaitNs) return {Act::Wait, pool_ns, K_ONESHOT};
        return {Act::Skip, 0, K_NONE};
    }
    if (!full || held || !r.full_wait) return {Act::Skip, 0, K_NONE};
    if (pool_ns >= kMinWaitNs) return {Act::Wait, pool_ns, K_FULL};
    if (last_ns >= kMinWaitNs) return {Act::Wait, last_ns, K_LAST};
    return {Act::Skip, 0, K_NONE};
}

// ---- a pipeline whose libraries were cold: the worker's order

/// The job a draw may wait for (its entry's ready flag), for a pipeline the draw could not link: cold names the libraries the
/// draw found cold (0: not), link_later says the draw did not try (LinkBudget). link(cold) links from the libraries, naming
/// those still cold in cold (gpl_link); build_cold(i, key) builds one (gpl_build_cold); linked(pipeline) makes the entry
/// ready with a link (gpl_rules::linked); whole_later() queues the whole build over it as a job of its own, whole_now() builds
/// it here and makes the entry ready with it (gpl_rules::built). Returns true when the entry is ready with a link
template <class Pipeline, class Link, class BuildCold, class Linked, class WholeLater, class WholeNow>
bool libs_first(uint64_t cold0, uint64_t cold1, bool link_later, Link link, BuildCold build_cold, Linked linked,
                WholeLater whole_later, WholeNow whole_now) {
    uint64_t cold[2] = {cold0, cold1};
    Pipeline l{};
    if (link_later) l = link(cold);
    if (l == Pipeline{} && (cold[0] || cold[1])) {
        for (int i = 0; i < 2; i++)
            if (cold[i]) build_cold(i, cold[i]);
        uint64_t still[2] = {0, 0};
        l = link(still);
    }
    if (!(l == Pipeline{}) && linked(l)) {
        whole_later();
        return true;
    }
    whole_now();
    return false;
}

}   // namespace pipe_wait
