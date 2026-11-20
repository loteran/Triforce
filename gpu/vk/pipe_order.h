/* (Triforce, wp18) The pipelines made ahead in the order the last sessions' draws first needed them (vk_draw.cpp
   graphics_pipeline, order_met, warm_index_set, pipelines_warm, warm_variant, warm_hold, warm_link_take; the file
   triforce_pipeline_order.bin beside the pipeline notes).

   With the programs read ahead in the sessions' order (program_order.h) the save's load-in frame reads none of its programs
   any more (lab round 37: "0 programs read from the cache by the draws, 298 taken read ahead"), but it still makes its
   pipelines: "pipelines 86.5" ms of its 249 to 270 (86 to 92 in every run), and 178 ms summed over the helpers in the Market's
   arrival frame. Where the precompile keeps no pipelines (the RAM spared), each pipeline of a load is new at its first draw,
   which links it on its own thread from its stage libraries, those made from the driver's caches there too. The pipeline warm
   (pipe_warm.h) makes them ahead for a program the read-ahead has read, from the pipeline notes: up to 32 a program variant,
   every pipeline any session ever built with it. In round 37 it made 1964 links and the draws took 54: its bounds (512 links,
   the oldest let go beyond; 1024 libraries no draw has had, none made beyond) were full well before the load, of pipelines the
   load did not draw, and of the program order's read-ahead the newest, the farthest from the draws, were what it kept.

   So each session keeps the pipelines its draws first needed (a new entry, or one the precompile made that no draw had), in
   that order (Record: the first kKeysMax), and writes them beside the notes now and then: this session's first, then those of
   the sessions before it has not met (program_order.h merge; the same form of file, its own magic). The next session finds
   each one's note (Index) and from the title on, when the read-ahead has read a program variant, the warm makes the pipelines
   the order names for it, and with an order only those (kPerVariant at most): their stage libraries, asked of the caches only,
   and their links, held for the draws (Held). The draw that needs one takes its link as it takes the warm's now.
   - At most kLinksMax links held: beyond, the one farthest ahead in the order goes, not the oldest (the nearest are the next
     frames'). At most kLibsMax libraries the warm made that no draw has had: none made beyond.
   - Where the draws are: the middle place of the last kRecent pipelines they needed first (Front, program_order.h's rule: one
     met out of the order's turn moves nothing). A link held kBehind places behind it goes, and none is made there: that
     session went elsewhere.
   - A link that goes takes with it the libraries the warm made for it that no draw has had and no other link held uses
     (let_go_libs), so that the libraries' bound does not fill with what the draws did not come to.
   Nothing changes what a draw gets: the same pipeline from the same libraries, linked a moment earlier on another thread.

   Plain functions: tests/test_pipe_order.cpp checks them. RX_VK_PIPELINE_ORDER=0: neither read nor written, the warm from the
   notes as before. */
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "program_order.h"

namespace mkwvk::pipe_order {

constexpr size_t kKeysMax = program_order::kKeysMax;   // (pipelines a session's order keeps, and the file)
constexpr size_t kPerVariant = 32;     // (the order's pipelines made for a program variant, at most: as the notes')
constexpr size_t kLinksMax = 1024;     // (links held for draws that have not come: a load's, beside the title's taken)
constexpr size_t kLibsMax = 2048;      // (stage libraries the warm made that no draw has had: two a program variant of 1024)
constexpr int64_t kBehind = 512;       // (a link held this many places behind where the draws are goes, and none is made there)
constexpr size_t kRecent = 16;         // (where the draws are: the middle place of the last this many they needed first)
constexpr uint32_t kMagic = 0x50505446;   // ("FTPP")

/// RX_VK_PIPELINE_ORDER: on unless "0"
inline bool on(const char* env) { return !(env && env[0] == '0' && !env[1]); }

/// The file: program_order.h's form, its own magic
inline std::vector<uint8_t> write(const std::vector<uint64_t>& keys) { return program_order::write(keys, kMagic); }
inline std::vector<uint64_t> read(const uint8_t* data, size_t n) { return program_order::read(data, n, kMagic); }
using program_order::merge;
using Record = program_order::Record;

/// One pipeline of the order: its place, its note's record in the notes file
struct Entry {
    int64_t place = 0;
    uint32_t rec = 0;
};

/// The order as the warm takes it: the pipelines with a note by program variant (the caller's key for it), each list in the
/// order, at most per a variant; and every key's place, for where the draws are
struct Index {
    std::unordered_map<uint64_t, std::vector<Entry>> by_variant;
    std::unordered_map<uint64_t, int64_t> place;
    size_t named = 0, variants = 0;   // (the pipelines with a note, and their variants, as built)
    /// note(key, &variant, &rec): the note of key, false when the notes have none
    template <class Note>
    void build(const std::vector<uint64_t>& order, Note note, size_t per = kPerVariant) {
        by_variant.clear(), place.clear(), named = 0;
        for (size_t i = 0; i < order.size(); i++) {
            const uint64_t k = order[i];
            if (!k || !place.emplace(k, (int64_t)i).second) continue;
            uint64_t variant = 0;
            uint32_t rec = 0;
            if (!note(k, &variant, &rec)) continue;
            std::vector<Entry>& list = by_variant[variant];
            if (list.size() >= per) continue;
            list.push_back({(int64_t)i, rec});
            named++;
        }
        variants = by_variant.size();
    }
    /// The variant's pipelines, taken out (each variant once a session); none when the order names none for it
    std::vector<Entry> take(uint64_t variant) {
        const auto it = by_variant.find(variant);
        if (it == by_variant.end()) return {};
        std::vector<Entry> out = std::move(it->second);
        by_variant.erase(it);
        return out;
    }
    /// key's place in the order; -1: it names none
    int64_t at(uint64_t key) const {
        const auto it = place.find(key);
        return it == place.end() ? -1 : it->second;
    }
};

/// Where the draws are in the order
struct Front {
    std::deque<int64_t> recent;   // (the places of the last kRecent pipelines the draws needed first)
    int64_t furthest = -1;        // (the furthest the middle of recent has been)
    /// A draw needed the pipeline at place first (-1: one the order does not name, nothing moves): true when where the draws
    /// are moved on
    bool met(int64_t place) {
        if (place < 0) return false;
        recent.push_back(place);
        if (recent.size() > kRecent) recent.pop_front();
        std::vector<int64_t> s(recent.begin(), recent.end());
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        if (s[s.size() / 2] <= furthest) return false;
        furthest = s[s.size() / 2];
        return true;
    }
    /// Whether the draws went kBehind places past place (nothing made there, and what is held there goes)
    bool passed(int64_t place) const { return furthest >= 0 && place < furthest - kBehind; }
    /// The first place not passed
    int64_t from() const { return furthest - kBehind; }
};

/// The links held for draws that have not come, by pipeline key, each with its place and the keys of the libraries it was
/// linked from. At most max: beyond, the one farthest ahead goes. Not thread-safe: the caller's lock
template <class Link>
class Held {
public:
    struct One {
        uint64_t key = 0;
        int64_t place = 0;
        Link link{};
        uint64_t libs[4]{};
        uint32_t n = 0;
    };
    explicit Held(size_t max = kLinksMax) : max_(max) {}
    /// link held for key at place, linked from n libraries; into gone: the new one when key is held already, the farthest
    /// ahead beyond max (the new one, when it is that). The caller destroys what goes (no draw has had it)
    void put(uint64_t key, int64_t place, Link link, const uint64_t* libs, uint32_t n, std::vector<One>& gone) {
        One o;
        o.key = key, o.place = place, o.link = link, o.n = n > 4 ? 4 : n;
        for (uint32_t i = 0; i < o.n; i++) o.libs[i] = libs[i];
        if (place_of_.count(key)) {
            gone.push_back(o);
            return;
        }
        by_place_.emplace(std::make_pair(place, key), o);
        place_of_.emplace(key, place);
        while (by_place_.size() > max_) {
            const auto last = std::prev(by_place_.end());
            gone.push_back(last->second);
            place_of_.erase(last->first.second);
            by_place_.erase(last);
        }
    }
    /// The link held for key, taken out (a draw's from now on): false when none is
    bool take(uint64_t key, One& out) {
        const auto it = place_of_.find(key);
        if (it == place_of_.end()) return false;
        const auto at = by_place_.find({it->second, key});
        out = at->second;
        by_place_.erase(at);
        place_of_.erase(it);
        return true;
    }
    /// Those placed before at, taken out into gone
    void behind(int64_t at, std::vector<One>& gone) {
        const auto end = by_place_.lower_bound({at, 0});
        for (auto it = by_place_.begin(); it != end;) {
            gone.push_back(it->second);
            place_of_.erase(it->first.second);
            it = by_place_.erase(it);
        }
    }
    /// Those linked from library lib, taken out into gone (it is let go)
    void drop_using(uint64_t lib, std::vector<One>& gone) {
        for (auto it = by_place_.begin(); it != by_place_.end();) {
            if (!linked_from(it->second, lib)) {
                ++it;
                continue;
            }
            gone.push_back(it->second);
            place_of_.erase(it->first.second);
            it = by_place_.erase(it);
        }
    }
    /// Whether a link at place would stay held: room left, or nearer than the farthest held (made, not let go at once)
    bool room(int64_t place) const {
        return by_place_.size() < max_ || (!by_place_.empty() && place < std::prev(by_place_.end())->first.first);
    }
    /// Whether a link held was linked from library lib
    bool uses(uint64_t lib) const {
        for (const auto& [at, o] : by_place_)
            if (linked_from(o, lib)) return true;
        return false;
    }
    /// Every link held, taken out into out, the nearest first
    void all(std::vector<One>& out) {
        for (const auto& [at, o] : by_place_) out.push_back(o);
        by_place_.clear();
        place_of_.clear();
    }
    size_t size() const { return by_place_.size(); }

private:
    static bool linked_from(const One& o, uint64_t lib) {
        for (uint32_t i = 0; i < o.n; i++)
            if (o.libs[i] == lib) return true;
        return false;
    }
    size_t max_;
    std::map<std::pair<int64_t, uint64_t>, One> by_place_;   // ((place, key): the nearest first)
    std::unordered_map<uint64_t, int64_t> place_of_;
};

/// Of the libraries of the links that went (gone), those to let go with them, each once: the warm's own that no draw has had
/// (mine(lib)) and that no link still held uses (used(lib))
template <class One, class Mine, class Used>
std::vector<uint64_t> let_go_libs(const std::vector<One>& gone, Mine mine, Used used) {
    std::vector<uint64_t> out;
    std::unordered_set<uint64_t> seen;
    for (const One& o : gone)
        for (uint32_t i = 0; i < o.n; i++) {
            const uint64_t lib = o.libs[i];
            if (seen.insert(lib).second && mine(lib) && !used(lib)) out.push_back(lib);
        }
    return out;
}

} // namespace mkwvk::pipe_order
