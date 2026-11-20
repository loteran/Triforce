// tests/test_stream_order.py compiles and runs this: the parallel replay with a temporal upscaler's hooks in the draws
// (gpu/vk/stream_order.h). The production pieces are cut out of the sources by the test and put in at the markers below:
// vk_temporal.cpp's temporal_pre_point, temporal_pre_point_dry and temporal_pre_pending (with its State::PrePoint), and
// vk_replay.cpp's segment state and record_segment, with replay_hooks_told and replay_stream_pos. What they call is stood in
// for here: the stream is a list of draws and dispatches, the hooks (the scene's camera, the skinned draws, the upscale's
// dispatch) are small models of vk_temporal.cpp's, and draw() is a model of vk_draw.cpp's order (the test reads that order
// from vk_draw.cpp). replay_handle's way with a command buffer (the dry pass, its tail, stream_order::cuts, the jobs on a
// pool of threads taken in order) is done here as vk_replay.cpp does it (the test reads its steps there too).
//
// Cases: "order": random frames of several command buffers, recorded once on the replay thread alone and once in segments
// on 3 threads with random delays: every "first" (the camera, the point before the bloom, the point learnt for the next
// frame) and every skinned draw's name the upscale takes are the same. "hole": the same frames in segments without the dry
// pass's telling (as the parallel replay was before par_stream_order_only): they differ, which is what the order rules are
// for. "steps": pre_point_step against the rule as temporal_pre_point had it inline; renumber; cuts; InOrder under load.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "stream_order.h"

static int failures;
#define CHECK(x)                                              \
    do {                                                      \
        if (!(x)) {                                           \
            std::printf("FAIL:%d %s\n", __LINE__, #x);        \
            failures++;                                       \
        }                                                     \
    } while (0)

namespace mkwvk {

// ---- vk_temporal.cpp's pre point, cut out, with what it uses ----
enum VkFormat { VK_FORMAT_UNDEFINED = 0, FMT_SCENE = 122, FMT_LDR = 37 };
struct Texture {
    VkFormat format;
    uint32_t width, height;
    bool depth_and_camera;   // (draws into it note the camera: the scene and scene-sized look-alikes)
};
std::atomic<uint64_t> frame_now{1};
uint64_t texture_frame_no() { return frame_now.load(); }
void logf(const char*, ...) {}
bool pre_on = true;
bool pre_wanted(int) { return pre_on; }
int temporal_mode() { return 1; }
struct State {
    std::mutex mu;
    struct Camera {
        bool valid = false;
        const Texture* color = nullptr;
        uint64_t frame = 0;
        uint64_t area = 0;
        size_t op = 0;   // (the fixture's: which draw gave it)
    } cam;
/* PREPOINT_STRUCT */
    PrePoint pre_key, pre_candidate;
    bool pre_pending = false;
    uint64_t pre_frame = 0;
    const Texture* pre_scene = nullptr;
    bool pre_touched = false;
    int pre_late = 0, pre_late_dumps = 0;
    uint64_t pre_late_frame = 0;
};
State the_state;
State& st() { return the_state; }

/* PRE_POINT */

// ---- vk_replay.cpp's segments, cut out, with what they use ----
struct Rec {   // (a command buffer: what was recorded into it)
    std::vector<size_t> ops;
};
using VkCommandBuffer = Rec*;
struct NvnState {
    int unused = 0;
};
enum class Mode { Serial, Dry, Worker };
thread_local Mode rmode = Mode::Serial;
thread_local size_t ub_limit = SIZE_MAX, cond_at = 0;
thread_local uint64_t ub_gen = 0;
thread_local bool hooks_told = false;
thread_local uint64_t stream_cb = 0;
thread_local int64_t stream_work = 0;
void full_barrier(VkCommandBuffer) {}
void end_pass(VkCommandBuffer) {}
bool occ_live = false;                       // (vk_replay.cpp RX_VK_OCC_QUERY: off here)
void occ_segment_edge(VkCommandBuffer) {}
void replay(VkCommandBuffer& cmd, NvnState& s, const uint8_t* p, const uint8_t* end);

/* SEGMENT */

/* HOOKS */

// ---- the stream and the hooks' models ----
enum Kind { DRAW, UPSCALE, DISPATCH, END };
struct Op {
    Kind kind = DRAW;
    int target = 0;           // (an index into targets)
    uint64_t program = 0;
    uint64_t model = 0;       // (a skinned draw's model: 0 none)
};
std::vector<Texture> targets;
std::vector<Op> ops;          // (the frame's ops, the command buffers' one after another, each ending in END)
std::vector<std::thread::id> recorded_on;   // (each op: the thread that recorded it)
std::mutex events_mu;
std::vector<std::string> events;   // (what the hooks decided, in the order the upscale and the point saw it)
struct Skin {
    uint64_t pos;
    uint64_t key;
};
std::vector<Skin> skins;   // (vk_temporal.cpp State::skins)
bool told_in_dry = true;   // ("hole": false, the parallel replay as it was without the dry pass's telling)

/// temporal_note_scene_draw's rule: the largest target with a camera this frame, the first of equal ones
void note_scene_draw(const Op& o, size_t at) {
    const Texture& t = targets[o.target];
    if (!t.depth_and_camera) return;
    State& s = st();
    std::lock_guard lk(s.mu);
    const uint64_t area = (uint64_t)t.width * t.height;
    if (s.cam.valid && s.cam.frame == texture_frame_no() && area <= s.cam.area) return;
    s.cam.valid = true, s.cam.color = &t, s.cam.frame = texture_frame_no(), s.cam.area = area, s.cam.op = at;
}
/// temporal_note_skinned_draw's naming: the n-th draw of its model this frame
void note_skinned_draw(const Op& o) {
    State& s = st();
    std::lock_guard lk(s.mu);
    int nth = 0;
    for (const Skin& k : skins) nth += k.key >> 8 == o.model;
    skins.push_back({replay_stream_pos(), o.model << 8 | (uint64_t)std::min(nth, 255)});
}
/// What the engine takes (actor_motion): the skinned draws, put in stream order and named again there
std::string take_skins() {
    stream_order::renumber(skins, [](const Skin& k) { return k.pos; }, [](Skin& k) -> uint64_t& { return k.key; });
    std::string out;
    for (const Skin& k : skins) out += " " + std::to_string(k.key);
    skins.clear();
    return out;
}
/// The engine's point (temporal_pre_tonemap): what it takes, then pending until the upscale
void engine_at_point(size_t at) {
    State& s = st();
    std::lock_guard lk(s.mu);
    std::lock_guard ek(events_mu);
    events.push_back("point at op " + std::to_string(at) + ", camera from op " + std::to_string(s.cam.op) + ", skins" + take_skins());
    s.cam.valid = false;
    s.pre_pending = true, s.pre_frame = texture_frame_no(), s.pre_scene = s.cam.color;
}
/// The upscale's dispatch (pre_dispatch): after the point, done; else the point learnt for the next frame
void upscale(size_t at) {
    State& s = st();
    std::lock_guard lk(s.mu);
    std::lock_guard ek(events_mu);
    std::string e = "upscale at op " + std::to_string(at);
    if (s.pre_pending) {
        s.pre_pending = false, s.pre_candidate = {};
        events.push_back(e + " after the point");
        return;
    }
    if (s.pre_candidate.program && !(s.pre_candidate == s.pre_key)) s.pre_key = s.pre_candidate;
    s.pre_candidate = {};
    events.push_back(e + ": camera from op " + std::to_string(s.cam.valid ? s.cam.op : 9999) + ", learnt " +
                     std::to_string(s.pre_key.program) + ", skins" + take_skins());
    s.cam.valid = false;
}

/// vk_draw.cpp draw()'s order (the test checks it there): the camera unless told, the program, the point unless told, the
/// skinned draw, the draw
void draw(const Op& o, size_t at) {
    const bool told = replay_hooks_told();
    if (!told) note_scene_draw(o, at);
    const bool pre_now = !told && temporal_pre_point(&targets[o.target], o.program, nullptr);
    if (pre_now) engine_at_point(at);
    if (o.model) note_skinned_draw(o);
}
void dispatch(const Op& o, size_t at) {
    if (o.kind == UPSCALE) upscale(at);
}
size_t index_of(const uint8_t* p) { return (size_t)(reinterpret_cast<const Op*>(p) - ops.data()); }
void replay(VkCommandBuffer& cmd, NvnState&, const uint8_t* p, const uint8_t* end) {
    for (;; p += sizeof(Op)) {
        if (p == end) return;
        const size_t at = index_of(p);
        const Op& o = ops[at];
        if (o.kind == END) return;
        recorded_on[at] = std::this_thread::get_id();
        cmd->ops.push_back(at);
        if (o.kind == DRAW) draw(o, at);
        else dispatch(o, at);
        stream_work++;
    }
}

// ---- replay_handle's way with a command buffer (vk_replay.cpp) ----
struct ParPoint {
    const uint8_t* p;
    NvnState s;
    size_t ub, cond;
    int64_t work;
};
uint64_t stream_cbs = 0;
int segmented = 0, segmented_tails = 0;   // (command buffers recorded on the pool's threads, and those with a tail)
bool own_on = true;      // (RX_VK_PARALLEL_OWN: the replay thread records the next segment to submit no pool thread began)
int own_recorded = 0;    // (segments it recorded)
struct Pool {   // (par_pool: jobs taken in order by the threads)
    std::mutex mu;
    std::condition_variable cv, done;
    std::deque<std::function<void()>> jobs;
    int pending = 0;
    bool stop = false;
    std::vector<std::thread> threads;
    void start(int n) {
        for (int i = 0; i < n; i++)
            threads.emplace_back([this] {
                for (;;) {
                    std::function<void()> fn;
                    {
                        std::unique_lock lk(mu);
                        cv.wait(lk, [&] { return stop || !jobs.empty(); });
                        if (stop && jobs.empty()) return;
                        fn = std::move(jobs.front());
                        jobs.pop_front();
                    }
                    fn();
                    std::lock_guard lk(mu);
                    if (--pending == 0) done.notify_all();
                }
            });
    }
    void end() {
        {
            std::lock_guard lk(mu);
            stop = true;
        }
        cv.notify_all();
        for (auto& t : threads) t.join();
        threads.clear();
    }
} pool;
std::mt19937 rng_delay(7);
std::mutex rng_mu;
int delay_us() {
    std::lock_guard lk(rng_mu);
    return (int)(rng_delay() % 300);
}

/// One command buffer from h: on the replay thread alone (parallel false, or the engine pending), or as replay_handle does
void replay_command_buffer(const uint8_t* h, bool parallel, int64_t per) {
    NvnState state;
    Rec cmd_rec;
    VkCommandBuffer cmd = &cmd_rec;
    const uint64_t cb = ++stream_cbs;
    const bool hooks = told_in_dry;
    if (!parallel || (hooks && temporal_pre_pending())) {
        stream_cb = cb, stream_work = 0;
        replay(cmd, state, h, nullptr);
        return;
    }
    // the dry pass (dry_tail_check, dry_temporal_draw, dry_temporal_dispatch): the hooks told up to the first draw or
    // dispatch that takes what they noted, the points every 64 draws and dispatches
    std::vector<ParPoint> par_points;
    ParPoint par_tail{};
    bool find = hooks, tail = false;
    int64_t dry_work = 0;
    for (const uint8_t* p = h;; p += sizeof(Op)) {
        const size_t at = index_of(p);
        const Op& o = ops[at];
        if (o.kind == END) break;
        if (find) {
            bool here = false;
            if (o.kind == DRAW) {
                note_scene_draw(o, at);
                here = temporal_pre_point_dry(&targets[o.target], [](void* c) { return static_cast<const Op*>(c)->program; },
                                              const_cast<Op*>(&o));
            } else {
                here = o.kind == UPSCALE;
            }
            if (here) find = false, tail = true, par_tail = {p, state, 0, 0, dry_work};
        }
        if (++dry_work % 64 == 0) par_points.push_back({p + sizeof(Op), state, 0, 0, dry_work});
    }
    const int64_t total = dry_work;
    const int64_t until = tail ? par_tail.work : total;
    const int most = per < 100 ? 12 : 6;   // (vk_replay.cpp par_seg_most: RX_VK_PARALLEL_SEGS, 6 unless set)
    std::vector<Segment> seg;
    seg.push_back({h, nullptr, &state, 0, 0, 0, cb, hooks});
    for (const size_t i : stream_order::cuts(par_points, until, per, most - (tail ? 1 : 0),
                                             [](const ParPoint& q) { return q.work; })) {
        const ParPoint& at = par_points[i];
        seg.back().end = at.p;
        seg.push_back({at.p, nullptr, &at.s, at.ub, at.cond, at.work, cb, hooks});
    }
    if (tail) {
        seg.back().end = par_tail.p;
        seg.push_back({par_tail.p, nullptr, &par_tail.s, par_tail.ub, par_tail.cond, par_tail.work, cb, false, true});
    }
    seg.back().end = nullptr;
    if (seg.size() == 1 || (tail && seg.size() == 2)) {
        for (const Segment& g : seg) record_segment(cmd, g);
        return;
    }
    segmented++, segmented_tails += tail;
    stream_order::InOrder in_order(seg.size());
    if (tail)
        for (size_t k = 0; k < seg.size(); k++) seg[k].order = &in_order, seg[k].index = k;
    std::vector<Rec> recs(seg.size());
    // (Triforce: a segment is recorded by whichever thread takes it first: a pool thread, or the replay thread, in order, the
    // next one to submit when no pool thread has begun it, as replay_handle's Job::taken)
    std::vector<std::atomic<bool>> taken(seg.size()), done(seg.size());
    std::mutex done_mu;
    std::condition_variable done_cv;
    const auto run = [&](size_t k) {
        VkCommandBuffer c = &recs[k];
        record_segment(c, seg[k]);
        {
            std::lock_guard l(done_mu);
            done[k] = true;
        }
        done_cv.notify_all();
    };
    {
        std::lock_guard lk(pool.mu);
        for (size_t k = 0; k < seg.size(); k++) {
            pool.jobs.push_back([k, &taken, &run] {
                std::this_thread::sleep_for(std::chrono::microseconds(delay_us()));   // (a helper thread's wake-up, at random)
                if (taken[k].exchange(true)) return;
                run(k);
            });
            pool.pending++;
        }
    }
    pool.cv.notify_all();
    for (size_t k = 0; k < seg.size(); k++) {
        if (own_on && !taken[k].exchange(true)) run(k), own_recorded++;
        std::unique_lock l(done_mu);
        done_cv.wait(l, [&] { return done[k].load(); });
    }
    std::unique_lock lk(pool.mu);
    pool.done.wait(lk, [] { return pool.pending == 0; });
}

// ---- random frames ----
std::mt19937 rng(1234);
int pick(int n) { return (int)(rng() % (uint32_t)n); }

/// A frame like OoT's: draws into small targets (shadows, a look-alike of the scene's size), the scene with its camera
/// and skinned draws, the bloom's first draw, more post draws, the upscale's dispatch, the menus; in 1 to 3 command buffers
std::vector<std::vector<Op>> make_frame(int frame) {
    std::vector<Op> f;
    const uint64_t bloom = 0xB100, other = 0x0700;
    const int pre = pick(300);
    for (int i = 0; i < pre; i++) {
        Op o;
        // (a shadow map, a reflection with a camera; now and then a look-alike of the scene's size, which is the camera then:
        // the first of equal ones)
        o.target = pick(60) == 0 ? 4 : 2 + pick(2);
        o.program = 0x100 + pick(20);
        o.model = pick(6) == 0 ? 1 + pick(4) : 0;
        f.push_back(o);
        if (pick(40) == 0) f.push_back({DISPATCH, 0, 0x900, 0});
    }
    const int scene = 200 + pick(1200);
    for (int i = 0; i < scene; i++) {
        Op o;
        o.target = 0;
        o.program = 0x200 + pick(50);
        o.model = pick(4) == 0 ? 1 + pick(8) : 0;
        f.push_back(o);
        if (pick(15) == 0) f.push_back({DRAW, 1, 0x300 + (uint64_t)pick(3), 0});   // (a draw elsewhere amid the scene's)
        if (pick(60) == 0) f.push_back({DISPATCH, 0, 0x901, 0});
    }
    // (the bloom's first pass; in some frames something else first, so that the point is learnt again)
    f.push_back({DRAW, 1, frame % 7 == 3 ? other : bloom, 0});
    const int post = 5 + pick(40);
    for (int i = 0; i < post; i++) {
        f.push_back({DRAW, 1 + pick(2) * 4, 0x400 + (uint64_t)pick(5), 0});
        if (pick(8) == 0) f.push_back({DRAW, 0, 0x250, 0});   // (into the scene after it: the lens flare)
    }
    f.push_back({UPSCALE, 0, 0xfd02, 0});
    const int ui = pick(200);
    for (int i = 0; i < ui; i++) f.push_back({DRAW, 6, 0x500 + (uint64_t)pick(9), 0});
    std::vector<std::vector<Op>> cbs;
    const int n = 1 + pick(3);
    size_t from = 0;
    for (int k = 1; k <= n; k++) {
        const size_t to = k == n ? f.size() : from + (f.size() - from) * (size_t)(1 + pick(3)) / 5;
        cbs.emplace_back(f.begin() + (long)from, f.begin() + (long)to);
        from = to;
    }
    return cbs;
}

void reset_state() {
    State& s = st();
    s.cam = {};
    s.pre_key = {}, s.pre_candidate = {}, s.pre_pending = false, s.pre_frame = 0, s.pre_scene = nullptr;
    skins.clear();
    stream_cbs = 0;
}

/// The run's events: frames one after another, each frame's command buffers in turn
std::vector<std::string> run(uint32_t seed, int frames, bool parallel, int64_t per) {
    rng.seed(seed);
    reset_state();
    events.clear();
    for (int f = 0; f < frames; f++) {
        frame_now = 100 + (uint64_t)f;
        const auto cbs = make_frame(f);
        ops.clear();
        std::vector<size_t> starts;
        for (const auto& c : cbs) {
            starts.push_back(ops.size());
            ops.insert(ops.end(), c.begin(), c.end());
            ops.push_back({END, 0, 0, 0});
        }
        recorded_on.assign(ops.size(), std::thread::id());
        for (size_t s : starts) replay_command_buffer(reinterpret_cast<const uint8_t*>(&ops[s]), parallel, per);
        std::lock_guard ek(events_mu);
        events.push_back("frame " + std::to_string(f) + " ends: learnt " + std::to_string(st().pre_key.program));
    }
    return events;
}

int threads_seen() {   // (how many threads recorded ops of the last frame)
    std::vector<std::thread::id> ids;
    for (auto id : recorded_on)   // (the command buffers' ends are not recorded: no thread)
        if (id != std::thread::id() && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    return (int)ids.size();
}

void order_case() {
    targets = {{FMT_SCENE, 1920, 1080, true}, {FMT_SCENE, 960, 540, false}, {FMT_LDR, 1024, 1024, false},
               {FMT_SCENE, 640, 360, true},   {FMT_SCENE, 1920, 1080, true}, {FMT_LDR, 480, 270, false},
               {FMT_LDR, 3840, 2160, false}};
    pool.start(3);
    int differing = 0, runs = 0, most_threads = 0, points = 0;
    segmented = segmented_tails = 0, own_recorded = 0;
    for (uint32_t seed = 1; seed <= 40; seed++)
        for (int64_t per : {40, 160}) {
            told_in_dry = true;
            own_on = seed % 4 != 0;   // (a quarter of the runs with RX_VK_PARALLEL_OWN=0: the pool's threads alone)
            const auto serial = run(seed, 6, false, per);
            const auto par = run(seed, 6, true, per);
            for (const auto& e : serial) points += e.rfind("point at", 0) == 0;
            most_threads = std::max(most_threads, threads_seen());
            runs++;
            if (serial != par) {
                differing++;
                if (differing <= 2) {
                    for (size_t i = 0; i < std::max(serial.size(), par.size()); i++) {
                        const std::string a = i < serial.size() ? serial[i] : "-", b = i < par.size() ? par[i] : "-";
                        if (a != b) {
                            std::printf("  seed %u per %lld: replay thread alone: %s\n                 in segments: %s\n", seed,
                                        (long long)per, a.substr(0, 160).c_str(), b.substr(0, 160).c_str());
                            break;
                        }
                    }
                }
            }
        }
    pool.end();
    own_on = true;
    CHECK(differing == 0);
    CHECK(most_threads >= 2);   // (the segments were recorded on the pool's threads)
    CHECK(points > 0 && segmented_tails > 0 && segmented > segmented_tails);   // (frames with the point, and tails, were seen)
    CHECK(own_recorded > 0);    // (and some by the replay thread, each the next to submit)
    std::printf("order: %d runs of 6 frames, %d differ from the replay thread alone; %d command buffers in segments, %d of them "
                "with a tail, %d segments recorded by the replay thread; %d points before the bloom; up to %d threads recorded a "
                "frame\n", runs, differing, segmented, segmented_tails, own_recorded, points, most_threads);
}

void hole_case() {   // (without the dry pass's telling: the hooks in the order the threads came)
    targets = {{FMT_SCENE, 1920, 1080, true}, {FMT_SCENE, 960, 540, false}, {FMT_LDR, 1024, 1024, false},
               {FMT_SCENE, 640, 360, true},   {FMT_SCENE, 1920, 1080, true}, {FMT_LDR, 480, 270, false},
               {FMT_LDR, 3840, 2160, false}};
    pool.start(3);
    int differing = 0, runs = 0;
    own_on = false;   // (the pool's threads alone: the replay thread taking the next segment in order hides the hole at times)
    for (uint32_t seed = 1; seed <= 20; seed++) {
        told_in_dry = true;
        const auto serial = run(seed, 6, false, 40);
        told_in_dry = false;
        const auto par = run(seed, 6, true, 40);
        runs++;
        differing += serial != par;
    }
    told_in_dry = true;
    pool.end();
    CHECK(differing > 0);
    std::printf("hole: %d of %d runs differ from the replay thread alone without the dry pass's telling\n", differing, runs);
}

void steps_case() {
    // pre_point_step against the rule as temporal_pre_point had it inline, over random runs of draws
    struct P {
        uint64_t program = 0;
        int format = 0;
        uint32_t w = 0, h = 0;
        bool operator==(const P& o) const { return program == o.program && format == o.format && w == o.w && h == o.h; }
    };
    std::mt19937 r(99);
    int asked = 0, steps = 0;
    for (int run = 0; run < 2000; run++) {
        P cand_a, cand_b, learnt{0x10 + r() % 3, 1, 8, 8};
        for (int i = 0; i < 60; i++) {
            const bool into = r() % 3 == 0;
            const P here{0x10 + r() % 4, (int)(r() % 2), 8, 8};
            bool want;   // (the inline rule)
            if (into) cand_a = {}, want = false;
            else {
                if (!cand_a.program) cand_a = here;
                want = here == learnt && cand_a == here;
            }
            bool was_asked = false;
            const bool got = stream_order::pre_point_step(cand_b, learnt, into, [&] { was_asked = true; return here; });
            CHECK(got == want);
            CHECK(cand_a == cand_b);
            asked += was_asked, steps++;
        }
    }
    CHECK(asked < steps);   // (the program is not asked of every draw)
    // renumber: entries in another order come back in stream order, named by their place among their model's
    std::vector<Skin> v = {{5, 2 << 8 | 0}, {1, 2 << 8 | 1}, {3, 7 << 8 | 0}, {2, 2 << 8 | 2}};
    stream_order::renumber(v, [](const Skin& k) { return k.pos; }, [](Skin& k) -> uint64_t& { return k.key; });
    CHECK(v[0].pos == 1 && v[0].key == (2 << 8 | 0) && v[1].pos == 2 && v[1].key == (2 << 8 | 1));
    CHECK(v[2].pos == 3 && v[2].key == (7 << 8 | 0) && v[3].pos == 5 && v[3].key == (2 << 8 | 2));
    std::vector<Skin> in_order = {{1, 4 << 8 | 9}, {2, 4 << 8 | 3}};   // (already in stream order: left as they are)
    stream_order::renumber(in_order, [](const Skin& k) { return k.pos; }, [](Skin& k) -> uint64_t& { return k.key; });
    CHECK(in_order[0].key == (4 << 8 | 9) && in_order[1].key == (4 << 8 | 3));
    // cuts: increasing, all before until, at most most - 1 of them, about even
    struct Q {
        int64_t work;
    };
    std::vector<Q> pts;
    for (int64_t w = 64; w <= 6400; w += 64) pts.push_back({w});
    const auto work = [](const Q& q) { return q.work; };
    const auto c = stream_order::cuts(pts, 4000, 160, 6, work);
    CHECK(c.size() == 5);
    for (size_t i = 0; i < c.size(); i++) {
        CHECK(pts[c[i]].work < 4000);
        CHECK(i == 0 || c[i] > c[i - 1]);
        CHECK(std::abs(pts[c[i]].work - 4000 * (int64_t)(i + 1) / 6) < 64);
    }
    CHECK(stream_order::cuts(pts, 100, 160, 6, work).empty());   // (less than a segment's worth: one)
    CHECK(stream_order::cuts(pts, 64, 16, 6, work).empty());     // (no point before until)
    CHECK(stream_order::cuts(pts, 6400, 160, 3, work).size() == 2);
    // InOrder under load: the last never goes before the others are done
    for (int round = 0; round < 300; round++) {
        const size_t n = 2 + round % 5;
        stream_order::InOrder o(n);
        std::atomic<int> done{0};
        std::atomic<bool> early{false};
        std::vector<std::thread> ts;
        for (size_t k = 0; k < n; k++)
            ts.emplace_back([&, k] {
                if (k + 1 == n) {
                    o.wait_before(k);
                    if (done.load() != (int)k) early = true;
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds((k * 37 + round) % 200));
                    done++;
                }
                o.done(k);
            });
        for (auto& t : ts) t.join();
        CHECK(!early.load());
    }
    std::printf("steps: pre_point_step as the inline rule (%d of %d draws asked their program), renumber, cuts, InOrder\n", asked,
                steps);
}

} // namespace mkwvk

int main(int argc, char** argv) {
    const std::string c = argc > 1 ? argv[1] : "";
    if (c == "order") mkwvk::order_case();
    else if (c == "hole") mkwvk::hole_case();
    else if (c == "steps") mkwvk::steps_case();
    else return std::printf("which case?\n"), 2;
    return failures ? 1 : 0;
}
