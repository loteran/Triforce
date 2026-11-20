// (Triforce) The pipelines of a load made behind its draws (vk_draw.cpp pipelines_warm, called by the program read-ahead's
// workers: vk_shader.cpp readahead_main). Apart from Vulkan so that tests/test_pipe_warm.py runs it alone.
//
// Why: v0.4.0-test16's load at Castle Town spent 59.6, 79.5 and 37.3 ms a frame in "pipelines" on the thread replaying
// (frames 8769, 8789 and 8793), with 267, 203 and 99 programs new to the draws and no waits for a pipeline. That is the fast
// link at each new pipeline's first draw: its two stage libraries made from the caches (the driver had all 1654 the session
// asked for, none cold) and their link (1.35 ms for 6.01 links a frame over that session, some 0.2 ms a link). That PC's
// precompile keeps no pipelines (the RAM spared: precompile_keeps_all), so every pipeline of a load is new at its first
// draw. The program read-ahead reads a load's programs on four workers before their draws, in the notes' order
// (program_readahead.h), and the notes hold each pipeline's whole state. So the worker that has just read a program, before
// any draw can have it, makes the stage libraries of the pipelines the notes name for it, asking the caches only, and links
// them. The draw finds the link made and goes on; the worker's whole build follows it as before.
//
// The rules, here and in vk_draw.cpp:
//  - level (RX_VK_PIPELINE_WARM): 0 none, as before; 1 the stage libraries only; 2 (the default) the libraries and the links
//  - only where the precompile keeps no pipelines, with the fast link on and asking the caches only (gpl_ask_only). A
//    library the driver would have to compile is left to its draw, which hands it to a worker as before. Nothing is made
//    while the RAM is low or short (may_warm)
//  - at most kNotesPerVariant notes a program variant (the first in the notes' order), each variant once a session
//  - what it holds is bounded: at most kLinksMax links made for draws that have not come (the oldest let go beyond: Held),
//    at most kLibsMax stage libraries made that no draw has had (none made beyond). A library or link a draw has had is the
//    draw's from then on, as if the draw had made it
//  - while the RAM is short, what it holds goes back to the driver (vk_draw.cpp warm_let_go, from pipelines_release_tick):
//    none of it was ever in a recording, and no warm has one of its libraries in hand then
//  - ((Triforce, wp18) with the last sessions' order of pipelines (pipe_order.h, RX_VK_PIPELINE_ORDER), the pipelines that
//    order names for a variant read and no other, their links held by their place in it with its own bounds; the notes'
//    others are not made then, and the warm does not follow the draws)
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pipe_warm {

constexpr size_t kNotesPerVariant = 32;   // (notes warmed a program variant, at most)
constexpr size_t kLinksMax = 512;         // (links held for draws that have not come: about one load's, as the shelf's 512 programs)
constexpr size_t kLibsMax = 1024;         // (stage libraries made that no draw has had: two a program for the shelf's 512)

/// RX_VK_PIPELINE_WARM: unset 2; 0, 1 or 2 as given; below 0 is 0, above 2 is 2
inline int level(const char* env) {
    if (!env || !*env) return 2;
    const int v = atoi(env);
    return v < 0 ? 0 : v > 2 ? 2 : v;
}

/// Whether a worker makes anything now: a level, the fast link on, asking the caches only, the RAM neither low nor short
inline bool may_warm(int level, bool fast_link, bool ask_only, bool ram_low, bool ram_short) {
    return level > 0 && fast_link && ask_only && !ram_low && !ram_short;
}

/// The notes' records by program variant (the caller's key for it), at most per a variant, in the order they are added
inline void index_add(std::unordered_map<uint64_t, std::vector<uint32_t>>& index, uint64_t variant, uint32_t rec,
                      size_t per = kNotesPerVariant) {
    std::vector<uint32_t>& recs = index[variant];
    if (recs.size() < per) recs.push_back(rec);
}

/// Whether the warm may make one more stage library, with made of its own that no draw has had
inline bool lib_room(size_t made, size_t max = kLibsMax) { return made < max; }

/// The links held for draws that have not come, by pipeline key, each with the keys of the libraries it was linked from.
/// At most max: beyond, the oldest goes. Not thread-safe: the caller's lock
template <class Link>
class Held {
public:
    struct One {
        uint64_t key = 0;
        Link link{};
        uint64_t libs[4]{};
        uint32_t n = 0;
    };
    explicit Held(size_t max = kLinksMax) : max_(max) {}
    /// link held for key, linked from n libraries; what goes into gone: the new one when key is held already, the oldest
    /// beyond max. The caller destroys what goes (no draw has had it)
    void put(uint64_t key, Link link, const uint64_t* libs, uint32_t n, std::vector<Link>& gone) {
        if (by_key_.count(key)) {
            gone.push_back(link);
            return;
        }
        One o;
        o.key = key, o.link = link, o.n = n > 4 ? 4 : n;
        for (uint32_t i = 0; i < o.n; i++) o.libs[i] = libs[i];
        order_.push_back(o);
        by_key_[key] = std::prev(order_.end());
        while (order_.size() > max_) {
            gone.push_back(order_.front().link);
            by_key_.erase(order_.front().key);
            order_.pop_front();
        }
    }
    /// The link held for key, taken out (a draw's from now on): false when none is
    bool take(uint64_t key, One& out) {
        const auto it = by_key_.find(key);
        if (it == by_key_.end()) return false;
        out = *it->second;
        order_.erase(it->second);
        by_key_.erase(it);
        return true;
    }
    /// ((Triforce, test18) the links held that were linked from library lib, taken out into gone: it is let go)
    void drop_using(uint64_t lib, std::vector<Link>& gone) {
        for (auto it = order_.begin(); it != order_.end();) {
            bool uses = false;
            for (uint32_t i = 0; i < it->n; i++) uses |= it->libs[i] == lib;
            if (!uses) {
                ++it;
                continue;
            }
            gone.push_back(it->link);
            by_key_.erase(it->key);
            it = order_.erase(it);
        }
    }
    /// ((Triforce, wp18) whether a link held was linked from library lib: pipe_order.h let_go_libs)
    bool uses(uint64_t lib) const {
        for (const One& o : order_)
            for (uint32_t i = 0; i < o.n; i++)
                if (o.libs[i] == lib) return true;
        return false;
    }
    /// Every link held, taken out into out, the oldest first
    void all(std::vector<Link>& out) {
        for (const One& o : order_) out.push_back(o.link);
        order_.clear();
        by_key_.clear();
    }
    size_t size() const { return order_.size(); }

private:
    size_t max_;
    std::list<One> order_;   // (the oldest first)
    std::unordered_map<uint64_t, typename std::list<One>::iterator> by_key_;
};

// ---- (Triforce, test18) Following the draws. In the owner's test18 session the warm made 2266 links and 2462 libraries and
// its draws took 276 of the links, of the 1659 pipelines the session drew (all but 15 of them noted). The read-ahead reads 32
// programs after each one the draws take, far more than a load takes, and the warm made everything it read: by the end of the
// load (the line at frame ~3540) the libraries no draw had had were at their bound of 1024, so nothing read after that had a
// library made or a link (the warm makes no link without all its libraries), and 1207 links had been let go as the oldest
// beyond 512, among them ones the load came to later. With RX_VK_PIPELINE_WARM_FOLLOW=1 (off by default since test19, below):
//  - where the draws are (Front): the last kPoints places in the read-ahead's order (the notes', then the compute programs')
//    the draws consumed (took a program read ahead, or read one themselves), each with its time;
//  - a program read within kLinkAhead places after one of those consumed in the last kRecentMs has its libraries made and
//    linked; one within kLibAhead its libraries only (cheap, and shared by its other pipelines: its draw links them itself, a
//    fraction of a millisecond); one farther, or with no recent place before it (the draws stopped taking), nothing (want);
//  - the libraries no draw has had are kept up to kLibsKeep, the oldest let go beyond (Made; the held links made from one
//    go with it, Held::drop_using), so the bound never stops the warm as kLibsMax did;
// Off (the default): every program read is warmed whole and the libraries stop at kLibsMax, as before.
// tests/pipe_warm_sim.cpp runs both ways through a synthetic load.
//
// (Triforce, test18) The compute pipelines too (vk_draw.cpp warm_compute). test18's load spent 67.6 ms of its first frame in
// "dispatch": that PC's precompile builds no compute pipelines either (built once for the caches, then left to play), so the
// first dispatch of each compute program made its pipeline on the thread replaying, asked of the caches (a hit, but a
// creation all the same), or queued it and waited up to 50 ms. The read-ahead reads the compute programs play dispatched
// (triforce_computes.bin, after the notes' programs); the worker that has read one makes its pipeline the same way, of the
// caches only, and the dispatch that takes the program has it. One a program on the read-ahead's shelf (512 at most, and the
// game lists 286), destroyed when the shelf lets the program go, given back from the shelf while the RAM is short.
// RX_VK_COMPUTE_WARM=1 to have it; off by default since test19 (below): none, as before.
constexpr size_t kPoints = 16;          // (the draws' latest places kept)
constexpr int64_t kLinkAhead = 64;      // (places after a recent one: libraries and links)
constexpr int64_t kLibAhead = 192;      // (places after a recent one: libraries only)
constexpr uint64_t kRecentMs = 2000;    // (a place older than this is not where the draws are)
constexpr size_t kLibsKeep = 2048;      // (following: the libraries no draw has had, kept; the oldest let go beyond)

/// On unless "0"
inline bool on(const char* env) { return !(env && env[0] == '0' && !env[1]); }
/// ((Triforce, test19) RX_VK_PIPELINE_WARM_FOLLOW and RX_VK_COMPUTE_WARM: off unless "1" (opt_in))
inline bool opt_in(const char* env) { return env && env[0] == '1' && !env[1]; }

// ---- (Triforce, test19) What the warm may cost. The lab's test19 tour (w19-new-1, the one run of four whose precompile kept
// no pipelines, so the one where the warm ran at all; the other three kept every pipeline, and there the warm and its switches
// do nothing) had the warm spend 2538 ms on the read-ahead's four workers, 6862 libraries and 3662 links made for 556 links
// taken, 3143 libraries let go as the oldest, and 2833 compute pipelines made for the 378 compute programs the game lists: the
// read-ahead's shelf let 7647 programs go and read many again, and the warm made them again. A worker warming is a worker not
// reading: that run's draws read 500 programs themselves, and waited 28 times (69 ms) for one being read. So:
//  - at most kBudgetMsPerSecond of the workers' time a second goes to the warm, kBudgetBurstMs saved up at most (Budget): past
//    it, a program read is parked as it is, its draws link it as before;
//  - nothing is made twice in a session: a library the warm let go as the oldest is not made again by the warm (a draw that
//    needs it makes it), a compute pipeline is made once a program variant (Once)
constexpr double kBudgetMsPerSecond = 250;   // (a quarter of one worker)
constexpr double kBudgetBurstMs = 500;

/// The workers' time the warm may spend: per_second ms a second, burst saved up at most. Any thread
class Budget {
public:
    explicit Budget(double per_second = kBudgetMsPerSecond, double burst = kBudgetBurstMs)
        : per_second_(per_second), burst_(burst), tokens_(burst) {}
    /// Whether the warm may begin something now (anything left of the budget)
    bool allows(uint64_t now_ms) {
        std::lock_guard lk(mu_);
        refill(now_ms);
        return tokens_ > 0;
    }
    /// ms of the workers' time spent (the budget may go below nothing: what is spent is spent)
    void spent(double ms, uint64_t now_ms) {
        std::lock_guard lk(mu_);
        refill(now_ms);
        tokens_ -= ms;
    }

private:
    void refill(uint64_t now_ms) {
        if (last_ && now_ms > last_) tokens_ = std::min(burst_, tokens_ + (double)(now_ms - last_) * per_second_ / 1000.0);
        last_ = now_ms;
    }
    std::mutex mu_;
    double per_second_, burst_, tokens_;
    uint64_t last_ = 0;
};

/// Keys done once a session: first(key) is true the first time only. Not thread-safe: the caller's lock
class Once {
public:
    bool first(uint64_t key) { return done_.insert(key).second; }
    bool done(uint64_t key) const { return done_.count(key) != 0; }
    size_t size() const { return done_.size(); }

private:
    std::unordered_set<uint64_t> done_;
};

enum class Want { none, libraries, links };

/// What the warm makes of a program that far (Front::ahead) after the nearest place the draws consumed lately
inline Want want(int64_t ahead, int64_t link_ahead = kLinkAhead, int64_t lib_ahead = kLibAhead) {
    if (ahead < 0 || ahead > lib_ahead) return Want::none;
    return ahead <= link_ahead ? Want::links : Want::libraries;
}

/// Where the draws are: their latest places in the read-ahead's order, with when. Any thread
class Front {
public:
    void consumed(int64_t at, uint64_t ms) {
        if (at < 0) return;
        std::lock_guard lk(mu_);
        points_[next_++ % kPoints] = {at, ms, true};
    }
    /// How far at lies after the nearest place at or before it consumed in the last recent_ms (0: that place); -1: none
    int64_t ahead(int64_t at, uint64_t now_ms, uint64_t recent_ms = kRecentMs) const {
        if (at < 0) return -1;
        std::lock_guard lk(mu_);
        int64_t best = -1;
        for (const Point& p : points_) {
            if (!p.set || p.at > at || (now_ms > p.ms && now_ms - p.ms > recent_ms)) continue;
            if (best < 0 || at - p.at < best) best = at - p.at;
        }
        return best;
    }

private:
    struct Point {
        int64_t at = 0;
        uint64_t ms = 0;
        bool set = false;
    };
    mutable std::mutex mu_;
    Point points_[kPoints];
    size_t next_ = 0;
};

/// The libraries the warm made, in the order it made them: the oldest still the warm's (still(key)) is the one let go.
/// Not thread-safe: the caller's lock
class Made {
public:
    void add(uint64_t key) { q_.push_back(key); }
    /// The oldest key still(key) says is the warm's, taken out into key; false when there is none. Keys a draw had since
    /// are passed over and dropped
    template <class Still>
    bool oldest(uint64_t& key, Still still) {
        while (!q_.empty()) {
            key = q_.front();
            q_.pop_front();
            if (still(key)) return true;
        }
        return false;
    }
    /// The keys no longer the warm's dropped once the list is four times max (a draw had them)
    template <class Still>
    void trim(size_t max, Still still) {
        if (q_.size() <= 4 * max) return;
        std::deque<uint64_t> keep;
        for (const uint64_t k : q_)
            if (still(k)) keep.push_back(k);
        q_.swap(keep);
    }
    size_t size() const { return q_.size(); }

private:
    std::deque<uint64_t> q_;
};

}   // namespace pipe_warm
