// (Triforce) The lazy write-back's page states (vk_memory.cpp "lazy write-back", docs/lazy-writeback.md), in a header of
// their own so that tests run them without the game or a GPU (tests/test_wb_lazy_state.py).
//
// What a shader writes into a mirrored pool is copied back to guest memory at the end of every submission, in case the CPU
// reads it. With the lazy write-back a whole page that keeps being written and that the CPU leaves alone is no longer
// copied: the copy is owed, the page gets no access, and whoever touches it pays first (one copy, mirror to guest memory,
// of everything written since). A page goes through:
//
//   none     guest memory has all the GPU wrote; read and write.        Written back at each submission's end, as ever.
//   WATCH    the same, and read-only: a CPU write faults and ends the watch (the page is the CPU's too: not for us).
//            It is still written back at each submission's end. Once every CPU write from before the watch is known
//            (a collection and a capture made after it began have come and named no write to it), the next write makes it
//   OWED     no access. Later submissions leave their writes in the mirror (seq: the newest recording that did).
//   PAYING   no access; one thread copies mirror to guest memory. Every other toucher waits for that copy, then goes on.
//   PENDING  access back, after a payment that came between a recording's end and its submit: what that recording
//            wrote is still owed, and is copied right after it is submitted (as its own write-back would have been).
//
// The rules the tests hold it to: protection and state never disagree (no access: OWED or PAYING; read-only: WATCH);
// every reader of an owed page gets the copy first; a CPU write to a watched page ends the watch without a copy, and one
// to an owed page pays first and never drops the debt; later writes join the one debt and one copy pays them all; one
// payer at a time for a page; nothing gets protected while a host call has guest memory in the kernel's hands (io).
//
// Locking: one mutex, held for the table's steps and the protection calls, never across a copy or a wait for the GPU.
// A touch of a page in PAYING waits on the condition, which lets the mutex go.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace wb_lazy {

constexpr uint64_t kPageShift = 12;
constexpr uint64_t kRegionPages = 512, kPages = 1ull << 27;   // (2 MB regions over the guest's 512 GB, as write_gen_table.h)
constexpr uint64_t kHotShift = 4;                              // (a touch holds its 64 KB block: 16 pages)
constexpr uint64_t kHoldMs = 1000, kHoldMaxMs = 32000;         // (the hold after a touch, doubling while it keeps being touched)
constexpr size_t kMaxSegs = 1u << 16;

enum State : uint8_t { WATCH = 1, OWED, PAYING, PENDING };
enum Access : uint8_t { RW = 0, RO, NONE };
/// Who asks for a payment: a fault (the game's or the host's code), a host call about to hand the range to the kernel, a GPU
/// read through the import, code that gives the mirror up, the recording's own submit (PENDING)
enum Why : int { BY_FAULT = 0, BY_HOST, BY_GPU, BY_GIVEUP, BY_SUBMIT, WHY_N };

struct Src {   // (the copy's two buffers and the pool's start: a page's offset in both is its guest address - base)
    uint64_t mirror = 0, import = 0, base = 0;
    bool operator==(const Src&) const = default;
};
struct Job {   // (pages [lo, hi) to copy from src.mirror to src.import)
    uint64_t lo = 0, hi = 0;
    Src src;
    uint64_t seq = 0;
};
/// What the collections have seen so far (vk_memory.cpp): a watch may end in OWED only once one of each, begun after the
/// watch did, has come: by then every CPU write from before the watch has been named
struct Epoch {
    uint64_t collects_begun = 0;   // (collections of the write watch numbered: one's sweep starts after it has its number)
    uint64_t collects = 0;     // (the newest collection whose pages were named)
    uint64_t caps_made = 0;    // (captures numbered: a capture's collection starts after it has its number)
    uint64_t caps_taken = 0;   // (the newest capture whose pages were named)
    bool captures = false;     // (captures are made at all)
};
struct Counters {
    uint64_t watched = 0, armed = 0, skipped_pages = 0, watch_ended_by_write = 0, watch_ended_by_host = 0;
    uint64_t paid[WHY_N] = {}, paid_pages[WHY_N] = {};
    uint64_t waited = 0, pending = 0, refused_hot = 0, refused_io = 0, refused_full = 0, foreign = 0, protect_failed = 0;
    uint64_t owed_pages = 0, watch_pages = 0;   // (now)
};
enum Fault { NOT_OURS, RETRY, PAY };

/// Whether it runs (vk_memory.cpp lazy_decide): mode 0 off, 1 on, 2 the self-check, and why, for the log line. env: what
/// RX_VK_LAZY_WB is set to (nullptr or "": not set), which goes over the row; row: the Debug tab's Lazy write-back row;
/// windows: a Windows build (the fault handler pays on Windows only); amd: AMD's driver, where the row alone does not turn
/// it on (NX2navi turned its own off there after reports of the game ending in play); mirrors: there are VRAM mirrors
struct Choice {
    int mode;
    const char* why;
};
inline Choice choose(const char* env, bool row, bool windows, bool amd, bool mirrors) {
    const bool set = env && *env;
    const int asked = set ? std::clamp(atoi(env), 0, 2) : row ? 1 : 0;
    if (!asked) return {0, set ? "RX_VK_LAZY_WB=0 is set" : "the Debug tab's row is off"};
    if (!windows) return {0, "it runs on Windows only (its fault handler has not been made for signals)"};
    if (!mirrors) return {0, "there are no VRAM mirrors to leave anything in"};
    if (set) return {asked, asked == 2 ? "RX_VK_LAZY_WB=2 is set" : "RX_VK_LAZY_WB=1 is set"};
    if (amd)
        return {0, "the Debug tab's row is on, but it stays off on AMD's driver, where NX2navi's ended games in play "
                   "(RX_VK_LAZY_WB=1 turns it on there)"};
    return {1, "the Debug tab's row is on"};
}

/// Mem: bool set(uint64_t page_lo, uint64_t page_hi, Access a) (false: the pages could not be changed), and
/// bool accessible(uint64_t page, bool write) (whether the system lets that access through now)
template <class Mem>
class Table {
  public:
    explicit Table(Mem& m) : mem(m), regions(new std::atomic<uint32_t>[kPages / kRegionPages]) {
        for (uint64_t i = 0; i < kPages / kRegionPages; i++) regions[i].store(0, std::memory_order_relaxed);
    }

    /// Whether a fault at that page can be ours at all (no lock: the fault handler's first question)
    bool tracked(uint64_t page) const {
        return page < kPages && regions[page / kRegionPages].load(std::memory_order_acquire) != 0;
    }
    bool any() const { return protected_pages.load(std::memory_order_acquire) != 0; }

    /// A submission's end, for the whole pages [lo, hi) of a run it is about to write back (seq: this recording). skip gets
    /// the pages that are OWED: their copy is left out, and the debt is this recording's now. The others are copied as ever;
    /// of them, the untracked become WATCH and the watched whose time has come OWED (from the next write on they are skipped)
    void advance(uint64_t lo, uint64_t hi, const Src& src, uint64_t seq, const Epoch& e, uint64_t now_ms, bool may_start,
                 std::vector<std::pair<uint64_t, uint64_t>>& skip) {
        if (hi <= lo || hi > kPages) return;
        std::lock_guard lk(mu);
        split(lo), split(hi);
        uint64_t at = lo;
        auto it = segs.lower_bound(lo);
        while (at < hi) {
            const uint64_t next = it != segs.end() && it->first < hi ? it->first : hi;
            if (at < next) {   // (untracked pages [at, next))
                if (may_start) start_watch(at, next, src, e, now_ms);
                it = segs.lower_bound(next);   // (start_watch added segments before it)
                at = next;
                continue;
            }
            Seg& s = it->second;
            if (!(s.src == src)) {
                c.foreign++;   // (another mirror's: never skipped. The code that gave that mirror up should have paid)
            } else if (s.state == OWED) {
                s.seq = seq;
                if (!skip.empty() && skip.back().second == s.lo) skip.back().second = s.hi;
                else skip.push_back({s.lo, s.hi});
                c.skipped_pages += s.hi - s.lo;
            } else if (s.state == WATCH && e.collects > s.collects_at && (!e.captures || e.caps_taken > s.caps_at)) {
                if (mem.set(s.lo, s.hi, NONE)) {
                    s.state = OWED, s.seq = seq;
                    c.armed += s.hi - s.lo, c.owed_pages += s.hi - s.lo, c.watch_pages -= s.hi - s.lo;
                } else {
                    c.protect_failed++;
                }
            }
            at = s.hi;
            ++it;
        }
    }

    /// A page the CPU is known to have written (a collection's or a capture's): a watch on it ends, and it is held like a
    /// touched one. True when the page is OWED or PAYING, which the watch is there to rule out (the caller says so in the log)
    bool cpu_wrote(uint64_t page, uint64_t now_ms) {
        if (!tracked(page)) return false;
        std::lock_guard lk(mu);
        auto it = find(page);
        if (it == segs.end()) return false;
        if (it->second.state != WATCH) return it->second.state == OWED || it->second.state == PAYING;
        split(page), split(page + 1);
        end_watch(segs.find(page));
        hold(page, now_ms);
        c.watch_ended_by_write++;
        return false;
    }

    /// A fault at page. NOT_OURS: nothing of this table's. RETRY: the access can run again (a watch ended by this write, a
    /// payment by another thread waited for, or the page has its access back by now). PAY: jobs holds what this thread
    /// has to copy (the owed pages around the address, grow more at most, in PAYING now), then end_pay
    Fault fault(uint64_t page, bool write, uint64_t grow, uint64_t now_ms, std::vector<Job>& jobs) {
        if (page >= kPages) return NOT_OURS;
        std::unique_lock lk(mu);
        for (;;) {
            auto it = find(page);
            // (not this table's, or no longer: the access runs again when the page lets it through by now. Another thread's
            // payment may have given the access back between the fault and this look)
            if (it == segs.end() || it->second.state == PENDING) return mem.accessible(page, write) ? RETRY : NOT_OURS;
            Seg& s = it->second;
            if (s.state == WATCH) {
                if (!write) return RETRY;   // (a watched page can be read: the fault is from before the watch, when it was owed)
                split(page & ~((1ull << kHotShift) - 1)), split((page | ((1ull << kHotShift) - 1)) + 1);   // (its 64 KB block alone)
                end_watch(find(page));
                hold(page, now_ms);
                c.watch_ended_by_write++;
                return RETRY;
            }
            if (s.state == PAYING) {
                c.waited++;
                cv.wait(lk);
                continue;   // (whatever it is now)
            }
            const uint64_t a = grow ? page / (grow + 1) * (grow + 1) : page;
            take(std::max(a, s.lo), std::min(a + grow + 1, s.hi), jobs, &jobs);
            return PAY;
        }
    }

    /// Everything in [lo, hi) settled before a reader that cannot fault: the owed pages go into jobs (PAYING; the caller
    /// copies, then end_pay), payments by other threads are waited for, and a watch ends unless the reader only reads through
    /// the GPU (BY_GPU: a watched page is whole and readable)
    void settle(uint64_t lo, uint64_t hi, Why why, uint64_t now_ms, std::vector<Job>& jobs) {
        if (hi <= lo) return;
        hi = std::min(hi, kPages);
        std::unique_lock lk(mu);
        for (bool again = true; again;) {
            again = false;
            split(lo), split(hi);
            for (auto it = segs.lower_bound(lo); it != segs.end() && it->first < hi;) {
                Seg& s = it->second;
                if (s.state == PAYING && s.payer != &jobs) {   // (another thread's copy: waited for, then looked at again)
                    c.waited++;
                    cv.wait(lk);
                    again = true;   // (the table changed while the mutex was away)
                    break;
                }
                if (s.state == WATCH && why != BY_GPU) {
                    if (why == BY_HOST) {
                        for (uint64_t b = s.lo >> kHotShift; b <= (s.hi - 1) >> kHotShift; b++) hold(b << kHotShift, now_ms);
                        c.watch_ended_by_host++;
                    }
                    it = end_watch(it);
                    continue;
                }
                if (s.state == OWED) {
                    const uint64_t a = s.lo, b = s.hi;
                    take(a, b, jobs, &jobs);
                    it = segs.upper_bound(a);
                    continue;
                }
                ++it;   // (PENDING: its access is back, and what it lacks no reader could have yet)
            }
        }
    }

    /// With the queue's lock held, as the copy of jobs goes onto the queue. submitted: the newest recording that is on the
    /// queue before it. A job whose debt is newer keeps the rest of it: that recording's submit, which takes the same lock,
    /// finds it (take_pending), whether this payment has ended by then or not
    void mark_rest(const std::vector<Job>& jobs, uint64_t submitted) {
        std::lock_guard lk(mu);
        for (const Job& j : jobs)
            for (auto it = segs.lower_bound(j.lo); it != segs.end() && it->first < j.hi; ++it) {
                Seg& s = it->second;
                if (s.state != PAYING || s.rest || s.seq <= submitted) continue;
                s.rest = true;
                pendings.fetch_add(1, std::memory_order_release);
                c.pending++;
            }
    }

    /// The copy of jobs is done and waited for: the pages get their access back, and a touch by the CPU holds them. A
    /// piece with a rest (mark_rest) is PENDING from here
    void end_pay(const std::vector<Job>& jobs, Why why, uint64_t now_ms) {
        {
            std::lock_guard lk(mu);
            for (const Job& j : jobs) {
                c.paid[why]++;
                if (why == BY_FAULT || why == BY_HOST || why == BY_GPU)
                    for (uint64_t b = j.lo >> kHotShift; b <= (j.hi - 1) >> kHotShift; b++) hold(b << kHotShift, now_ms);
                // (the job's pages, in as many pieces as other steps have cut them into meanwhile: PAYING there is this job's)
                for (auto it = segs.lower_bound(j.lo); it != segs.end() && it->first < j.hi;) {
                    Seg& s = it->second;
                    if (s.state != PAYING) { ++it; continue; }
                    if (!mem.set(s.lo, s.hi, RW)) c.protect_failed++;
                    count(s.lo, s.hi, -1);
                    c.owed_pages -= s.hi - s.lo, c.paid_pages[why] += s.hi - s.lo;
                    if (s.rest) {
                        s.state = PENDING, s.payer = nullptr, s.rest = false;
                        ++it;
                    } else {
                        it = segs.erase(it);
                        nsegs.fetch_sub(1, std::memory_order_release);
                    }
                }
            }
        }
        cv.notify_all();
    }

    /// With the queue's lock held, right after a recording's submit (submitted: its number): the pages with a rest that is
    /// on the queue now (PENDING, or still in a payment that began before the submit), to copy once more; then end_pending
    void take_pending(uint64_t submitted, std::vector<Job>& jobs) {
        if (!pendings.load(std::memory_order_acquire)) return;
        std::lock_guard lk(mu);
        for (auto& [lo, s] : segs)
            if ((s.state == PENDING || (s.state == PAYING && s.rest)) && s.seq <= submitted) jobs.push_back({s.lo, s.hi, s.src, s.seq});
    }
    /// That copy is done and waited for (the queue's lock still held): nothing of those pages is owed any more
    void end_pending(const std::vector<Job>& jobs) {
        std::lock_guard lk(mu);
        for (const Job& j : jobs) {
            c.paid[BY_SUBMIT]++;
            for (auto it = segs.lower_bound(j.lo); it != segs.end() && it->first < j.hi;) {
                Seg& s = it->second;
                if (s.seq > j.seq || !(s.state == PENDING || (s.state == PAYING && s.rest))) { ++it; continue; }
                c.paid_pages[BY_SUBMIT] += s.hi - s.lo;
                pendings.fetch_sub(1, std::memory_order_release);
                if (s.state == PAYING) {   // (its payment is still waiting for its own copy: it ends with nothing left)
                    s.rest = false;
                    ++it;
                } else {
                    it = segs.erase(it);
                    nsegs.fetch_sub(1, std::memory_order_release);
                }
            }
        }
    }

    /// A host call is about to hand guest memory to the kernel, or to change pages' state: from here to io_end nothing
    /// gets protected (the caller settles its own range after this, BY_HOST)
    void io_begin() {
        std::lock_guard lk(mu);
        io++;
    }
    void io_end() {
        std::lock_guard lk(mu);
        if (io) io--;
    }

    Counters counters() {
        std::lock_guard lk(mu);
        return c;
    }
    /// Pages made OWED so far (a caller that keeps answers about which buffer a range is read from asks whether it grew)
    uint64_t armed() {
        std::lock_guard lk(mu);
        return c.armed;
    }
    /// (tests) the state of a page, 0 none
    int state_of(uint64_t page) {
        std::lock_guard lk(mu);
        auto it = find(page);
        return it == segs.end() ? 0 : (int)it->second.state;
    }

  private:
    struct Seg {
        uint64_t lo = 0, hi = 0;
        Src src;
        uint64_t seq = 0;
        uint64_t collects_at = 0, caps_at = 0;   // (WATCH: what the collections had seen when it began)
        const void* payer = nullptr;             // (PAYING: whose jobs it is in)
        bool rest = false;                       // (PAYING: its newest recording was not on the queue yet: mark_rest)
        State state = WATCH;
    };
    struct Hot { uint64_t until = 0, back = 0, at = 0; };

    typename std::map<uint64_t, Seg>::iterator find(uint64_t page) {
        auto it = segs.upper_bound(page);
        if (it == segs.begin()) return segs.end();
        --it;
        return page < it->second.hi ? it : segs.end();
    }
    /// A segment that reaches over page is cut there (both halves keep its state: no protection changes)
    void split(uint64_t page) {
        auto it = find(page);
        if (it == segs.end() || it->second.lo == page) return;
        Seg b = it->second;
        b.lo = page;
        it->second.hi = page;
        segs.emplace(page, b);
        nsegs.fetch_add(1, std::memory_order_release);
        if (b.state == PENDING || b.rest) pendings.fetch_add(1, std::memory_order_release);
    }
    /// (regions' counts of pages without access: tracked())
    void count(uint64_t lo, uint64_t hi, int d) {
        for (uint64_t r = lo / kRegionPages; r <= (hi - 1) / kRegionPages; r++) {
            const uint64_t n = std::min(hi, (r + 1) * kRegionPages) - std::max(lo, r * kRegionPages);
            if (d > 0) regions[r].fetch_add((uint32_t)n, std::memory_order_release);
            else regions[r].fetch_sub((uint32_t)n, std::memory_order_release);
        }
        if (d > 0) protected_pages.fetch_add(hi - lo, std::memory_order_release);
        else protected_pages.fetch_sub(hi - lo, std::memory_order_release);
    }
    bool hot(uint64_t page, uint64_t now_ms) const {
        const auto it = hots.find(page >> kHotShift);
        return it != hots.end() && now_ms < it->second.until;
    }
    void hold(uint64_t page, uint64_t now_ms) {
        if (hots.size() > (1u << 16)) hots.clear();   // (a bound; the blocks come back at their next touch)
        Hot& h = hots[page >> kHotShift];
        if (h.back && h.at == now_ms) return;   // (the same touch, told once per piece of it)
        h.back = h.back && now_ms < h.until + h.back ? std::min(h.back * 2, kHoldMaxMs) : kHoldMs;
        h.until = now_ms + h.back, h.at = now_ms;
    }
    /// [lo, hi), untracked: read-only and watched from now, block by block, but for the blocks a touch holds; not while
    /// a host call is in the kernel, nor when the table is full
    void start_watch(uint64_t lo, uint64_t hi, const Src& src, const Epoch& e, uint64_t now_ms) {
        if (io) { c.refused_io++; return; }
        for (uint64_t a = lo; a < hi;) {
            const uint64_t block_end = std::min(hi, ((a >> kHotShift) + 1) << kHotShift);
            if (hot(a, now_ms)) { c.refused_hot++; a = block_end; continue; }
            uint64_t b = block_end;
            while (b < hi && !hot(b, now_ms)) b = std::min(hi, ((b >> kHotShift) + 1) << kHotShift);
            if (segs.size() >= kMaxSegs) { c.refused_full++; return; }
            if (mem.set(a, b, RO)) {
                Seg s;
                s.lo = a, s.hi = b, s.src = src, s.state = WATCH, s.collects_at = e.collects_begun, s.caps_at = e.caps_made;
                segs.emplace(a, s);
                nsegs.fetch_add(1, std::memory_order_release);
                // (a write fault on a watched page has to find the table: its region counts it too)
                count(a, b, +1);
                c.watched += b - a, c.watch_pages += b - a;
            } else {
                c.protect_failed++;
            }
            a = b;
        }
    }
    typename std::map<uint64_t, Seg>::iterator end_watch(typename std::map<uint64_t, Seg>::iterator it) {
        Seg& s = it->second;
        if (!mem.set(s.lo, s.hi, RW)) c.protect_failed++;
        count(s.lo, s.hi, -1);
        c.watch_pages -= s.hi - s.lo;
        nsegs.fetch_sub(1, std::memory_order_release);
        return segs.erase(it);
    }
    /// The OWED pages [a, b) of one segment become a job, PAYING
    void take(uint64_t a, uint64_t b, std::vector<Job>& jobs, const void* payer) {
        split(a), split(b);
        auto it = segs.find(a);
        Seg& s = it->second;
        s.state = PAYING, s.payer = payer;
        jobs.push_back({s.lo, s.hi, s.src, s.seq});
    }

    Mem& mem;
    std::mutex mu;
    std::condition_variable cv;
    std::map<uint64_t, Seg> segs;   // by lo, never overlapping
    std::unordered_map<uint64_t, Hot> hots;
    std::unique_ptr<std::atomic<uint32_t>[]> regions;   // (pages of each region that are WATCH, OWED or PAYING)
    std::atomic<uint64_t> protected_pages{0}, nsegs{0}, pendings{0};
    uint32_t io = 0;
    Counters c;
};

} // namespace wb_lazy
