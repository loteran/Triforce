// (Triforce, 11-17) Samples-passed reports answered from occlusion queries (RX_VK_OCC_QUERY=1, vk_replay.cpp "occlusion
// queries"): the bookkeeping, header only so that tests/test_occ_answer.py checks it without a device.
//
// The game resets NVN's samples-passed counter, draws an object's proxy and reports the count (its render job f_80084de0,
// the report from near lr 0x800876dc); its CPU reads the count back once it has waited for the frame, and does not draw an
// object whose count is 0. The replay writes every report as it passes it, in stream order, long before the GPU has run
// the draws it counts, and has always written 4096 there (everything visible). It still does: that placeholder is what
// anything reading the record early sees, as before. With RX_VK_OCC_QUERY=1 the draws between a reset and its report are
// also counted by a Vulkan occlusion query, and once the recording that holds them is done on the GPU the count is
// written into the same record, over the placeholder, as NX2navi writes it at completion. The game's report records
// rotate (a ring of them: an address stands for another object at its next report), so a count is written only into the
// record of the report it counted, and only while no later report has been written there (Writers), and only over the
// value the replay left in the word (a compare-and-write: the game's CPU re-arms its records between frames). A wrong 0
// hides an object the player should see, so a count goes in only when it is whole; otherwise the record keeps its 4096.
// The game also predicates draws on these words (render conditions, NOP_RENDER_COND): as NX2navi decides them
// (runtime/vk_exec_xfer.c cond_enabled), a word whose newest report is a samples-passed one with its count still to come is
// unknown, and the condition renders; a word whose count was written is read from memory, where a real 0 can skip the draw.
//
//   Counter   one counting, from a reset to its report, over the queries its draws began: the replay's (one per NVN
//             queue's state, under replay_mu) or a parallel replay's segment's (the helper thread's: a segment starts with
//             none, what came before it is another thread's). Matched to its recording by the recording's serial (each
//             begin of a slot takes a new one), never by the command buffer's handle, which a slot reuses
//   Writers   per report address, the newest report written there (its place in the stream, the value it left, whether its
//             count is pending, written or not to come): what a count read back may be written over, what a condition reads
//   Cond      a render condition's answer from its two words' sources (cond_decide)
//   complete  a done recording's counts into their records (through the caller's compare-and-write), or why not
//   Stats     what happened, for the line every 600 presents
//   Sites     the first 64 report addresses with their counts, for the lab to find the one RX_VK_OCC_QUERY_SKIP keeps at 4096
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mkwvk::occ {

constexpr uint64_t kVisible = 4096;      // (the placeholder every report gets at replay; it stays unless its whole count comes)
constexpr int64_t kLateAge = 8;          // (presents: a count read back this long after its report is not written)
constexpr int64_t kKeepAge = 16;         // (presents: a report address not reported again for longer leaves Writers)

/// RX_VK_OCC_QUERY_SKIP=<addr>[,<addr>...]: report addresses (hex with 0x, or decimal) that keep the fixed answer.
/// Separated by ',', ';' or spaces; anything else ends the list
inline std::vector<uint64_t> parse_addrs(const char* e) {
    std::vector<uint64_t> v;
    while (e && *e) {
        while (*e == ',' || *e == ';' || *e == ' ') e++;
        if (!*e) break;
        char* end = nullptr;
        const unsigned long long a = strtoull(e, &end, 0);
        if (end == e) break;
        v.push_back((uint64_t)a);
        e = end;
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}
inline bool listed(const std::vector<uint64_t>& sorted, uint64_t addr) {
    return std::binary_search(sorted.begin(), sorted.end(), addr);
}

/// Whether a knob list (RX_VK_LAB_AB's knobs, each maybe several names joined by '+') names this switch anywhere
inline bool knob_names(const char* knob, const char* name) {
    const size_t len = strlen(name);
    for (const char* p = knob; p && *p;) {
        const char* plus = strchr(p, '+');
        const size_t n = plus ? (size_t)(plus - p) : strlen(p);
        if (n == len && !strncmp(p, name, len)) return true;
        p = plus ? plus + 1 : nullptr;
    }
    return false;
}

/// The samples of the queries [first, first + n) of a block read with VK_QUERY_RESULT_64_BIT |
/// VK_QUERY_RESULT_WITH_AVAILABILITY_BIT ({samples, available} per query). False when one is not available
inline bool sum_range(const uint64_t* pairs, uint32_t first, uint32_t n, uint64_t* out) {
    uint64_t sum = 0;
    for (uint32_t i = first; i < first + n; i++) {
        if (!pairs[2 * i + 1]) return false;
        sum += pairs[2 * i];
    }
    *out = sum;
    return true;
}

/// A measured report: its address, its queries in its recording's block, its place in the stream (the same in the dry pass
/// that wrote its placeholder and on the thread that recorded its draws) and the present it was recorded at
struct Rep {
    uint64_t addr = 0;
    uint32_t first = 0, n = 0;
    uint64_t pos = 0;
    int64_t present = 0;
};

/// One samples-passed counting. The replay calls reset() at a SAMPLES_PASSED reset, draw_enter() as a draw goes into draw(),
/// begin() where a query may begin (the draw's pass open; may_begin() first, cheaply), draw_issued() for each draw recorded,
/// report() at the report. A query is begun inside a pass and ended at the pass's end or the report, so the queries of one
/// count follow one another in the block of the recording that holds them (first.. the block's next). The recording is
/// known by its serial (a new one each time a slot is begun): a draw or report in another recording, or in the same slot
/// begun again, loses the count
struct Counter {
    enum State { kUnknown, kCounting, kReported };
    enum Outcome { kMeasured, kNoReset, kIncomplete, kDouble };
    State state = kUnknown;
    int slot = -1;            // (the recording slot whose block the queries are in; -1: none, the count is lost)
    uint64_t serial = 0;      // (that slot's recording: occ_slot_begin's serial; 0: none)
    uint32_t first = 0;       // (the count's first query in the block)
    int open = -1;            // (the query begun and not ended, its index in the block)
    bool lost = false;        // (a draw in another recording, or the block full)
    uint32_t ops = 0, issued = 0;   // (draws that went into draw() while counting, and those it recorded)
    enum Why { kNone, kSkipped, kOtherCmd, kFull };
    Why why = kNone;

    /// A parallel replay's segment starts (or ends), or the replay's counting is given up: nothing is counting
    void segment() { *this = Counter{}; }
    bool counting(bool keep) const { return state == kCounting || (state == kReported && keep); }
    /// A reset: counting from the block's next query of slot's recording serial_ (slot -1 or serial 0: the command buffer
    /// is no recording's, the count is lost)
    void reset(int slot_, uint64_t serial_, uint32_t next) {
        const bool none = slot_ < 0 || !serial_;
        state = kCounting, slot = none ? -1 : slot_, serial = none ? 0 : serial_, first = next, open = -1, lost = none;
        ops = issued = 0;
        why = none ? kOtherCmd : kNone;
    }
    void draw_enter(bool keep) {
        if (counting(keep)) ops++;
    }
    void draw_issued(bool keep) {
        if (counting(keep)) issued++;
    }
    /// Whether a query might begin at a draw (before the recording's serial is looked up)
    bool may_begin(bool keep) const { return counting(keep) && open < 0 && !lost; }
    /// A draw in an open pass of the recording serial_: true when a query is to begin (the caller takes the block's next
    /// index and sets open); a draw in another recording loses the count
    bool begin(bool keep, uint64_t serial_) {
        if (!may_begin(keep)) return false;
        if (serial_ != serial) {
            lost = true, why = kOtherCmd;
            return false;
        }
        return true;
    }
    void full() { lost = true, why = kFull; }
    /// The report, in the recording serial_ (the open query already ended by the caller): what it measured. *keep_now: the
    /// game reported twice without a reset, which NVN counts on from the reset (cumulative): from then on the counting goes
    /// on past a report. A report in another recording than the reset's is incomplete
    Outcome report(bool keep, uint64_t serial_, uint32_t next, Rep* rep, bool* keep_now) {
        *keep_now = false;
        if (state == kUnknown) return kNoReset;
        if (state == kReported && !keep) {   // (the draws since the first report went uncounted: none of this count is whole)
            *keep_now = true;
            lost = true, why = kSkipped;
            return kDouble;
        }
        if (!lost && serial_ != serial) lost = true, why = kOtherCmd;
        const bool complete = !lost && ops == issued;
        if (!complete && !lost) why = kSkipped;
        state = kReported;
        if (!complete) return kIncomplete;
        rep->first = first, rep->n = next - first;
        return kMeasured;
    }
};

/// A map from 64-bit keys, open addressing with linear probes and backward-shift deletion (no tombstones): no allocation per
/// entry. The replay placed one entry for every samples-passed report (~2,600 a frame at the Market) in a std::unordered_map,
/// a node allocated and freed each time: 0.76 ms a frame of the Presentation Thread (all-thread sample, 2026-11-20)
template <class V>
class FlatU64 {
public:
    struct Slot {
        uint64_t key = 0;
        V val{};
        bool used = false;
    };
    V* find(uint64_t k) {
        if (!n_) return nullptr;
        for (size_t i = idx(k);; i = (i + 1) & mask_) {
            Slot& s = s_[i];
            if (!s.used) return nullptr;
            if (s.key == k) return &s.val;
        }
    }
    const V* find(uint64_t k) const { return const_cast<FlatU64*>(this)->find(k); }
    V& operator[](uint64_t k) {
        if (V* v = find(k)) return *v;
        if ((n_ + 1) * 10 > s_.size() * 7) grow();
        size_t i = idx(k);
        while (s_[i].used) i = (i + 1) & mask_;
        s_[i].key = k, s_[i].val = V{}, s_[i].used = true;
        n_++;
        return s_[i].val;
    }
    size_t count(uint64_t k) const { return find(k) ? 1 : 0; }
    size_t erase(uint64_t k) {
        if (!n_) return 0;
        size_t i = idx(k);
        for (;; i = (i + 1) & mask_) {
            if (!s_[i].used) return 0;
            if (s_[i].key == k) break;
        }
        // backward shift: the entries after the hole that would be found from a slot at or before it move into it
        for (size_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
            if (!s_[j].used) break;
            const size_t home = idx(s_[j].key);
            const bool moves = i <= j ? (home <= i || home > j) : (home <= i && home > j);
            if (moves) s_[i] = s_[j], i = j;
        }
        s_[i] = Slot{};
        n_--;
        return 1;
    }
    template <class F>
    void erase_if(F pred) {   // (the keys to go first, then each erased: a shift during the walk would skip or revisit entries)
        std::vector<uint64_t> gone;
        for (const Slot& s : s_)
            if (s.used && pred(s.key, s.val)) gone.push_back(s.key);
        for (uint64_t k : gone) erase(k);
    }
    bool empty() const { return n_ == 0; }
    size_t size() const { return n_; }

private:
    size_t idx(uint64_t k) const { return (size_t)((k * 0x9E3779B97F4A7C15ull) >> shift_) & mask_; }
    void grow() {
        std::vector<Slot> old;
        old.swap(s_);
        const size_t cap = old.empty() ? 1024 : old.size() * 2;
        s_.assign(cap, Slot{});
        mask_ = cap - 1;
        shift_ = 64;
        for (size_t c = cap; c > 1; c >>= 1) shift_--;
        n_ = 0;
        for (const Slot& s : old)
            if (s.used) (*this)[s.key] = s.val;
    }
    std::vector<Slot> s_;
    size_t n_ = 0, mask_ = 0;
    int shift_ = 64;
};

/// Per report address, the newest samples-passed report the replay wrote there (in stream order: placed) and where its
/// count stands: NX2navi's Pending table (runtime/vk_exec_xfer.c: seq, written, unknown, "the last report to addr
/// decides"). A count read back goes into its record only while its report is the newest there (the game's records rotate,
/// and a later report at the same address is another object's, whose placeholder stays), and a render condition reads a
/// word whose newest report's count is still to come as unknown (Src kPendingSrc: renders, NX2navi's cond_enabled "rx ==
/// 2"). A record rewritten in the stream after its report (a small buffer clear the replay applies in stream order, or a
/// report of another counter type) leaves the table: its count is not written there, and a condition reads memory, as before
class Writers {
public:
    enum Verdict { kWrite, kSuperseded, kUnknown, kFixed };
    enum State : uint8_t {
        kPending,   // (its count is to be written and has not been yet)
        kWritten,   // (its count is in the record: completion wrote it)
        kFinal      // (no count comes: skip list, knob off, a clear flag, not counted whole, lost, late, the CPU wrote it)
    };
    enum Src { kNone, kPendingSrc, kWrittenSrc, kFinalSrc };   // (what a render condition reading the word finds)
    struct Placed {
        uint64_t pos = 0;
        int64_t present = 0;
        uint64_t value = 0;   // (what the replay, or a count written since, put in the word: a count is written only over it)
        State state = kFinal;
    };
    /// A report's placeholder `value` written at the replay, at pos in the stream (the newest there from now on). write:
    /// its count is to be written once read back (not on RX_VK_OCC_QUERY_SKIP, the lab knob on, not a clear flag)
    void placed(uint64_t addr, uint64_t pos, int64_t present, bool write, uint64_t value) {
        m_[addr] = Placed{pos, present, value, write ? kPending : kFinal};
        page_[page(addr)] = 1, page_[page(addr + 7)] = 1;
    }
    bool is_report(uint64_t addr) const { return m_.count(addr) != 0; }
    /// What a count read back for the report at (addr, pos) may do. (Two reports at one address with no draw between share
    /// a place; in stream order the last one's count is written last, over the first one's)
    Verdict verdict(uint64_t addr, uint64_t pos) const {
        const auto* it = m_.find(addr);
        if (!it) return kUnknown;            // (not reported for kKeepAge presents, or rewritten in the stream)
        if (it->pos > pos) return kSuperseded;   // (reported again since: another object's record now)
        if (it->pos != pos) return kUnknown;
        return it->state == kFinal ? kFixed : kWrite;
    }
    /// The value the report at addr left in its word (the placeholder, or the count written since)
    uint64_t value(uint64_t addr) const {
        const auto* it = m_.find(addr);
        return !it ? 0 : it->value;
    }
    /// The count of the report at (addr, pos) is in its record now
    void wrote(uint64_t addr, uint64_t pos, uint64_t count) {
        auto* it = m_.find(addr);
        if (it && it->pos == pos) it->state = kWritten, it->value = count;
    }
    /// No count comes for the report at (addr, pos) (not counted whole, lost, late, never sent, the CPU wrote the record):
    /// a condition reads its word from memory from now on, as before
    void settle(uint64_t addr, uint64_t pos) {
        auto* it = m_.find(addr);
        if (it && it->pos == pos && it->state == kPending) it->state = kFinal;
    }
    /// A report of another counter type at addr (a timestamp: base behaviour) is the newest there now
    bool erase(uint64_t addr) { return m_.erase(addr) != 0; }
    /// A small buffer clear of [lo, hi) applied in stream order (NOP_CLEAR_BUF): the records whose count word it reaches leave
    /// the table (NX2navi's clear_small and clr_mask, by whole records: memory holds the clear's value from now on, and the
    /// count of a report before it is not written over it). Report addresses are 4-byte aligned at least. Returns how many
    int cleared(uint64_t lo, uint64_t hi) {
        if (hi <= lo || m_.empty() || (!page_[page(lo)] && !page_[page(hi - 1)])) return 0;
        int n = 0;
        for (uint64_t k = ((lo >= 7 ? lo - 7 : 0) + 3) & ~3ull; k < hi; k += 4) n += (int)m_.erase(k);
        return n;
    }
    /// What a render condition finds in the word at addr: no samples-passed report (memory, as before), its count pending
    /// (unknown: renders), written (memory holds it), or none to come (memory). A count pending longer than kLateAge
    /// presents is not written any more (complete: late): memory
    Src word(uint64_t addr, int64_t now) const {
        const auto* it = m_.find(addr);
        if (!it) return kNone;
        switch (it->state) {
        case kPending: return now - it->present > kLateAge ? kFinalSrc : kPendingSrc;
        case kWritten: return kWrittenSrc;
        case kFinal: break;
        }
        return kFinalSrc;
    }
    /// The addresses not reported for more than `age` presents go (a count still to come for them is not written)
    void prune(int64_t now, int64_t age) {
        m_.erase_if([now, age](uint64_t, const Placed& p) { return now - p.present > age; });
    }
    size_t size() const { return m_.size(); }

private:
    // (the pages a report went into, by a hash of the page, as NX2navi's rep_pages: a clear elsewhere looks nothing up)
    static uint32_t page(uint64_t a) { return (uint32_t)(((a >> 12) * 0x9E3779B97F4A7C15ull) >> 48); }
    FlatU64<Placed> m_;   // (by report address: FlatU64, no allocation per report)
    std::vector<uint8_t> page_ = std::vector<uint8_t>(1u << 16);
};

/// A render condition (NOP_RENDER_COND: mode 0 renders when the counter words at addr and addr + 16 are equal, mode 1 when
/// they differ) over the words' sources (Writers::word), after NX2navi's cond_enabled: a samples-passed count not done yet
/// renders; a written one, as any other word, is read from memory (base_enabled: the replay's answer from memory). active:
/// RX_VK_OCC_QUERY=1 or the lab knob's phase (else the answer is the base one, whatever the table says); cond_on: false
/// with RX_VK_OCC_COND=0, every condition on a samples-passed word renders
struct Cond {
    enum Kind { kNotSamples, kBase, kUnknown, kForced, kWrittenOn, kWrittenOff, kMemory };
    bool enabled;
    Kind kind;
};
inline Cond cond_decide(bool base_enabled, Writers::Src sx, Writers::Src sy, bool active, bool cond_on) {
    if (sx == Writers::kNone && sy == Writers::kNone) return {base_enabled, Cond::kNotSamples};
    if (!active) return {base_enabled, Cond::kBase};
    if (!cond_on) return {true, Cond::kForced};
    if (sx == Writers::kPendingSrc || sy == Writers::kPendingSrc) return {true, Cond::kUnknown};
    if (sx == Writers::kWrittenSrc || sy == Writers::kWrittenSrc)
        return {base_enabled, base_enabled ? Cond::kWrittenOn : Cond::kWrittenOff};
    return {base_enabled, Cond::kMemory};
}
inline const char* src_name(Writers::Src s) {
    return s == Writers::kPendingSrc ? "pending" : s == Writers::kWrittenSrc ? "written" : s == Writers::kFinalSrc ? "memory, no count to come" : "memory";
}

/// What happened since the last line (under the caller's lock)
struct Stats {
    enum At { kAtWait, kAtBegin, kAtPresent, kAtReuse };
    // at the replay
    int64_t reports = 0;      // samples-passed reports written (the placeholder, 4096 as before)
    int64_t in_segments = 0;  //   of them by the dry pass of a parallel replay
    int64_t skipped = 0;      //   on RX_VK_OCC_QUERY_SKIP: no count to be written
    int64_t fixed = 0;        //   the lab knob occq off in this phase: no count to be written
    int64_t flagged = 0;      //   the count word a clear flag (NVN_COUNTER_WRITTEN): left alone
    int64_t rewritten = 0;    // records rewritten in the stream after their report (a small clear, another counter type)
    int64_t measured = 0, no_reset = 0, incomplete = 0, doubles = 0;   // counted whole by queries, and those not
    int64_t inc_skipped = 0, inc_other_cmd = 0, inc_full = 0;          // why not whole
    // render conditions that read a samples-passed report's word (Cond::Kind), and the work they skipped
    int64_t conds = 0, cond_base = 0, cond_unknown = 0, cond_forced = 0, cond_written_on = 0, cond_written_off = 0, cond_memory = 0;
    int64_t draws_skipped = 0, clears_skipped = 0;
    // at completion
    int64_t written = 0, zeros = 0;    // counts written into their records (of them 0: the object hidden)
    int64_t lost = 0;                  // a query not available once its recording was done: 4096 stays
    int64_t dropped = 0;               // their recording never went to the GPU
    int64_t late = 0;                  // read back more than kLateAge presents after the report: not written
    int64_t superseded = 0;            // the record reported again first (the ring came round): not written
    int64_t unknown = 0;               // the record no longer known (pruned, or rewritten in the stream): not written
    int64_t kept_fixed = 0;            // its report placed with no count to be written (skip list, knob off)
    int64_t rearmed = 0;               // the word no longer held the replay's value (the CPU wrote the record since): not written
    int64_t lat_sum = 0, lat_ge1 = 0, lat_ge2 = 0;   // presents from the report to the write, over written
    int64_t at[4] = {};                // recordings read: at a game's fence wait, a recording's begin, a present, the slot's reuse
    int64_t addrs = 0;                 // distinct report addresses since the last line
    void cond(Cond::Kind k) {
        if (k == Cond::kNotSamples) return;
        conds++;
        (k == Cond::kBase ? cond_base : k == Cond::kUnknown ? cond_unknown : k == Cond::kForced ? cond_forced
         : k == Cond::kWrittenOn ? cond_written_on : k == Cond::kWrittenOff ? cond_written_off : cond_memory)++;
    }
    /// The line, into out
    int line(char* out, size_t size, int64_t presents, size_t tracked) const {
        return snprintf(out, size,
                        "vk: occlusion queries (RX_VK_OCC_QUERY), %lld presents: %lld samples-passed reports, 4096 written at "
                        "the replay as before (%lld by a parallel replay's dry pass; %lld skipped (RX_VK_OCC_QUERY_SKIP), %lld "
                        "lab knob occq off, %lld a clear flag; %lld records rewritten in the stream after their report); %lld "
                        "counted whole, %lld without their reset on the recording thread, %lld incomplete (%lld draws left out, "
                        "%lld over two recordings, %lld block full), %lld reported twice without a reset. At completion: %lld "
                        "counts written (%lld of them 0), %lld lost (not available), %lld never sent, %lld late, %lld superseded "
                        "(the record reported again first), %lld no longer known, %lld kept 4096 (skip list or knob off), %lld "
                        "not written: the record no longer held the replay's value; mean latency %.2f presents, %lld written "
                        "after the next present, %lld after two; recordings read at: %lld a game's fence wait, %lld a "
                        "recording's begin, %lld a present, %lld the slot's reuse. Render conditions on samples-passed words: "
                        "%lld (%lld as before: knob occq off; %lld rendered: a count pending (unknown); %lld rendered: "
                        "RX_VK_OCC_COND=0; %lld rendered from a written count; %lld skipped from a written count; %lld from "
                        "memory, no count to come); skipped by them: %lld draws and dispatches, %lld clears; %lld addresses "
                        "this period, %zu tracked",
                        (long long)presents, (long long)reports, (long long)in_segments, (long long)skipped, (long long)fixed,
                        (long long)flagged, (long long)rewritten, (long long)measured, (long long)no_reset,
                        (long long)incomplete, (long long)inc_skipped, (long long)inc_other_cmd, (long long)inc_full,
                        (long long)doubles, (long long)written, (long long)zeros, (long long)lost, (long long)dropped,
                        (long long)late, (long long)superseded, (long long)unknown, (long long)kept_fixed, (long long)rearmed,
                        written ? (double)lat_sum / (double)written : 0.0, (long long)lat_ge1, (long long)lat_ge2,
                        (long long)at[kAtWait], (long long)at[kAtBegin], (long long)at[kAtPresent], (long long)at[kAtReuse],
                        (long long)conds, (long long)cond_base, (long long)cond_unknown, (long long)cond_forced,
                        (long long)cond_written_on, (long long)cond_written_off, (long long)cond_memory,
                        (long long)draws_skipped, (long long)clears_skipped, (long long)addrs, tracked);
    }
};

/// A done recording's reports (in stream order) into their records. pairs: its block read with VK_QUERY_RESULT_64_BIT |
/// VK_QUERY_RESULT_WITH_AVAILABILITY_BIT over the `used` queries it took (readable false: the read failed). A count is
/// written only when every query of it is available, it is read back at most kLateAge presents after its report, and its
/// report is still the newest at its address with a count to be written there (Writers::verdict); then write(addr, expect,
/// count) puts it in the record only while the word still holds expect, what the replay (or an earlier count of the same
/// place) left there, and says whether it did: the game's CPU re-arms its records between frames, and a count read back
/// late must not land over that (NX2navi treats a report as pending until its value is written; here the write itself is
/// a compare-and-write). Anything else leaves the record as it is, and the report's count is settled (no count comes: a
/// condition reads memory). measured(addr, count): each whole count read back (the sites)
template <class Write, class Measured>
void complete(const std::vector<Rep>& reps, const uint64_t* pairs, uint32_t used, bool readable, Writers& w, int64_t now,
              Stats& st, Write&& write, Measured&& measured) {
    for (const Rep& p : reps) {
        uint64_t sum = 0;
        const bool inside = p.n <= used && p.first <= used - p.n;   // (its queries in the block the recording took)
        if (!readable || !inside || !sum_range(pairs, p.first, p.n, &sum)) {
            st.lost++;
            w.settle(p.addr, p.pos);
            continue;
        }
        measured(p.addr, sum);
        if (now - p.present > kLateAge) {
            st.late++;
            w.settle(p.addr, p.pos);
            continue;
        }
        switch (w.verdict(p.addr, p.pos)) {
        case Writers::kSuperseded: st.superseded++; continue;
        case Writers::kUnknown: st.unknown++; continue;
        case Writers::kFixed: st.kept_fixed++; continue;
        case Writers::kWrite: break;
        }
        if (!write(p.addr, w.value(p.addr), sum)) {
            st.rearmed++;
            w.settle(p.addr, p.pos);
            continue;
        }
        w.wrote(p.addr, p.pos, sum);
        const int64_t lat = std::max<int64_t>(0, now - p.present);
        st.written++, st.zeros += sum ? 0 : 1, st.lat_sum += lat, st.lat_ge1 += lat >= 1, st.lat_ge2 += lat >= 2;
    }
}
/// A recording's reports that never went to the GPU: no count comes for them
inline void dropped(const std::vector<Rep>& reps, Writers& w, Stats& st) {
    for (const Rep& p : reps) w.settle(p.addr, p.pos);
    st.dropped += (int64_t)reps.size();
}

/// The first 64 report addresses the replay saw, with how often each was reported, what its queries counted and how many
/// counts went into it
struct Sites {
    struct Site {
        uint64_t addr = 0;
        int64_t reports = 0, measured = 0, written = 0;
        uint64_t last = 0, lo = UINT64_MAX, hi = 0;
    };
    static constexpr int kMost = 64;
    Site s[kMost];
    int n = 0;
    Site* at(uint64_t addr, bool add) {
        for (int i = 0; i < n; i++)
            if (s[i].addr == addr) return &s[i];
        if (!add || n == kMost) return nullptr;
        s[n].addr = addr;
        return &s[n++];
    }
    void reported(uint64_t addr) {
        if (Site* x = at(addr, true)) x->reports++;
    }
    void measured(uint64_t addr, uint64_t count) {
        if (Site* x = at(addr, false)) x->measured++, x->last = count, x->lo = std::min(x->lo, count), x->hi = std::max(x->hi, count);
    }
    void wrote(uint64_t addr) {
        if (Site* x = at(addr, false)) x->written++;
    }
    /// Lines of eight sites each: "addr reports/measured last [lo..hi] written"
    template <class Log>
    void log(Log&& out) const {
        for (int b = 0; b < n; b += 8) {
            char line[1024];
            int k = snprintf(line, sizeof line, "vk: occlusion query sites %d-%d of %d (address: reports/measured, last count "
                             "[lowest..highest], counts written):", b + 1, std::min(n, b + 8), n);
            for (int i = b; i < std::min(n, b + 8) && k < (int)sizeof line - 96; i++) {
                const Site& x = s[i];
                k += snprintf(line + k, sizeof line - k, " %#llx: %lld/%lld %llu [%llu..%llu] %lld;", (unsigned long long)x.addr,
                              (long long)x.reports, (long long)x.measured, (unsigned long long)x.last,
                              (unsigned long long)(x.measured ? x.lo : 0), (unsigned long long)x.hi, (long long)x.written);
            }
            out(line);
        }
    }
};

/// The distinct addresses reported since the last line (bounded)
/// (a fixed open-addressing table of 131072 keys, 0 meaning empty (a report address is never 0), cleared at each line: it
/// was a std::unordered_set, a node allocated for each report's address only to count them for the log, 2,600 a frame)
struct Distinct {
    static constexpr size_t kSlots = 1u << 17, kMost = 65536;
    std::vector<uint64_t> slot = std::vector<uint64_t>(kSlots, 0);
    int64_t n = 0;
    void note(uint64_t addr) {
        if (!addr || n >= (int64_t)kMost) return;
        for (size_t i = (size_t)((addr * 0x9E3779B97F4A7C15ull) >> 47) & (kSlots - 1);; i = (i + 1) & (kSlots - 1)) {
            if (slot[i] == addr) return;
            if (!slot[i]) {
                slot[i] = addr, n++;
                return;
            }
        }
    }
    int64_t take() {
        const int64_t r = n;
        if (n) std::fill(slot.begin(), slot.end(), 0), n = 0;
        return r;
    }
};

} // namespace mkwvk::occ
