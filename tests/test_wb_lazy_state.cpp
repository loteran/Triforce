// The lazy write-back's page states (gpu/vk/wb_lazy_state.h, the code vk_memory.cpp runs) against a model of memory and of
// the GPU's copies: tests/test_wb_lazy_state.py. No game, no GPU.
//   rules:  the states one step at a time, protection compared with state after every step; the payers' slots
//   stress: a recorder, CPU readers and writers, host calls and GPU reads on threads of their own for <seconds>, with
//           <payers> payments copying at once (wb_lazy_payers.h; 4 unless given, 1 as the renderer had it first)
#include "wb_lazy_state.h"
#include "wb_lazy_payers.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace wb_lazy;

namespace {

int failures = 0;
std::mutex fail_mu;
void fail(const char* what, uint64_t a = 0, uint64_t b = 0) {
    std::lock_guard lk(fail_mu);
    if (failures++ < 20) std::printf("FAIL: %s (%llu, %llu)\n", what, (unsigned long long)a, (unsigned long long)b);
}
void expect(bool ok, const char* what) {
    if (!ok) fail(what);
}

constexpr uint64_t kN = 512;   // (the model's pages, from kBase)
constexpr uint64_t kBase = 0x40000;

/// Memory as the table sees it: each page's access, changed and looked at under the page's own lock (as the MMU checks an
/// access and makes it in one step)
struct Model {
    std::mutex page_mu[kN];
    uint8_t acc[kN] = {};
    std::atomic<uint64_t> sets{0};
    bool refuse = false;
    bool set(uint64_t lo, uint64_t hi, Access a) {
        if (refuse) return false;
        if (lo < kBase || hi > kBase + kN) { fail("protection outside the model", lo, hi); return false; }
        for (uint64_t p = lo; p < hi; p++) {
            std::lock_guard lk(page_mu[p - kBase]);
            acc[p - kBase] = a;
        }
        sets++;
        return true;
    }
    bool accessible(uint64_t page, bool write) {
        if (page < kBase || page >= kBase + kN) return false;
        std::lock_guard lk(page_mu[page - kBase]);
        return acc[page - kBase] == RW || (acc[page - kBase] == RO && !write);
    }
};

/// Protection and state never disagree: no access is OWED or PAYING, read-only is WATCH, the rest can be read and written
bool agree(Table<Model>& t, Model& m, const char* where) {
    for (uint64_t p = kBase; p < kBase + kN; p++) {
        const int s = t.state_of(p);
        const Access want = s == WATCH ? RO : s == OWED || s == PAYING ? NONE : RW;
        if (m.acc[p - kBase] != want) {
            std::printf("FAIL: %s: page %llu is in state %d with access %d\n", where, (unsigned long long)(p - kBase), s, m.acc[p - kBase]);
            failures++;
            return false;
        }
    }
    return true;
}

using Skip = std::vector<std::pair<uint64_t, uint64_t>>;
const Src kSrc{11, 22, kBase << kPageShift};

void rules() {
    Model m;
    Table<Model> t(m);
    Epoch e;
    Skip skip;
    std::vector<Job> jobs;
    uint64_t now = 100000;
    const uint64_t a = kBase + 32, b = kBase + 96;   // (a run of 64 whole pages, four 64 KB blocks)

    // -- a page is watched before anything is owed, and owed only once the collections have caught up
    expect(!t.any() && !t.tracked(a), "rules: a new table tracks nothing");
    e.collects_begun = 1;   // (a collection is under way: its sweep may have passed the page already)
    t.advance(a, b, kSrc, 1, e, now, true, skip);
    expect(skip.empty() && t.state_of(a) == WATCH && t.state_of(b - 1) == WATCH && t.state_of(b) == 0, "rules: the first write-back starts a watch and skips nothing");
    expect(t.tracked(a) && t.any(), "rules: a watched page is found by a fault");
    agree(t, m, "watch");
    t.advance(a, b, kSrc, 2, e, now, true, skip);
    expect(skip.empty() && t.state_of(a) == WATCH, "rules: no collection since the watch began: still watched, still copied");
    e.collects = 1;
    t.advance(a, b, kSrc, 2, e, now, true, skip);
    expect(skip.empty() && t.state_of(a) == WATCH, "rules: the collection that was under way when the watch began does not count");
    e.collects_begun = 2, e.collects = 2;
    e.captures = true, e.caps_made = 5;   // (captures are made, and none numbered after the watch began was named yet)
    t.advance(a, b, kSrc, 3, e, now, true, skip);
    expect(skip.empty() && t.state_of(a) == WATCH, "rules: a collection but no capture since the watch began: still watched");
    e.caps_taken = 1;
    t.advance(a, b, kSrc, 4, e, now, true, skip);
    expect(skip.empty() && t.state_of(a) == OWED && t.state_of(b - 1) == OWED, "rules: both have come: owed from now, and copied this once more");
    agree(t, m, "owed");
    t.advance(a, b, kSrc, 5, e, now, true, skip);
    expect(skip == Skip{{a, b}}, "rules: an owed run's copy is left out");
    skip.clear();

    // -- later writes join the one debt; a run over part of it splits it, in order
    t.advance(a, a + 16, kSrc, 6, e, now, true, skip);
    t.advance(a + 8, a + 24, kSrc, 7, e, now, true, skip);
    expect(skip == Skip{{a, a + 16}, {a + 8, a + 24}}, "rules: each run's owed pages are skipped");
    skip.clear();
    t.settle(a, b, BY_GPU, now, jobs);
    expect(jobs.size() == 4 && jobs[0].lo == a && jobs[0].hi == a + 8 && jobs[0].seq == 6 && jobs[1].hi == a + 16 && jobs[1].seq == 7 &&
               jobs[2].hi == a + 24 && jobs[2].seq == 7 && jobs[3].hi == b && jobs[3].seq == 5,
           "rules: one copy pays for every write that joined, each piece with its newest");
    for (const Job& j : jobs) expect(j.src == kSrc, "rules: a job names its buffers");
    expect(t.state_of(a) == PAYING && t.state_of(b - 1) == PAYING, "rules: pages being paid are PAYING");
    agree(t, m, "paying");
    t.settle(a, b, BY_GPU, now, jobs);
    expect(jobs.size() == 4, "rules: a settle by the payer itself takes nothing twice");
    t.mark_rest(jobs, 7);
    t.end_pay(jobs, BY_GPU, now);
    expect(t.state_of(a) == 0 && !t.any(), "rules: paid, the pages are untracked");
    agree(t, m, "paid");
    jobs.clear();
    const Counters c0 = t.counters();
    expect(c0.paid_pages[BY_GPU] == 64 && c0.owed_pages == 0 && c0.armed == 64 && c0.skipped_pages == 64 + 32, "rules: the counters");

    // -- a touch holds its blocks: not watched again within the hold, watched after it, and the hold doubles
    t.advance(a, b, kSrc, 8, e, now + kHoldMs - 1, true, skip);
    expect(t.state_of(a) == 0 && t.counters().refused_hot == 4, "rules: touched blocks are held for a second");
    now += kHoldMs + 1;
    t.advance(a, b + 16, kSrc, 9, e, now, true, skip);
    expect(t.state_of(a) == WATCH && t.state_of(b + 15) == WATCH, "rules: watched again after the hold");
    agree(t, m, "watch again");

    // -- a CPU write to a watched page ends the watch of its block, with no copy; the rest stays watched
    expect(t.fault(a + 3, false, 255, now, jobs) == RETRY && jobs.empty() && t.state_of(a + 3) == WATCH,
           "rules: a read fault on a watched page is from before the watch: run again, the watch stays");
    expect(t.fault(a + 3, true, 255, now, jobs) == RETRY && jobs.empty(), "rules: a write to a watched page ends the watch without a copy");
    expect(t.state_of(a) == 0 && t.state_of(a + 15) == 0 && t.state_of(a + 16) == WATCH, "rules: of its 64 KB block alone");
    expect(t.fault(a + 3, true, 255, now, jobs) == RETRY, "rules: and a second fault there finds the page writable: run again");
    agree(t, m, "watch ended by a write");
    // -- a write the collection names later ends the watch too (a page written before the watch began)
    expect(!t.cpu_wrote(a + 20, now) && t.state_of(a + 20) == 0 && t.state_of(a + 19) == WATCH && t.state_of(a + 21) == WATCH,
           "rules: a collected page's watch ends, the page alone");
    e.collects_begun++, e.collects++, e.caps_made++, e.caps_taken = e.caps_made + 1;
    t.advance(a, b + 16, kSrc, 10, e, now, true, skip);
    expect(t.state_of(a + 19) == OWED && t.state_of(a + 20) == 0 && t.state_of(a + 3) == 0,
           "rules: the pages the CPU wrote are not owed: held, and copied at every submission as ever");
    agree(t, m, "owed around the CPU's pages");

    // -- every reader pays first. A fault: the owed pages around it, PAYING until the copy is done
    m.acc[5] = RO;   // (someone else's protection, a debugging page watch: not this table's to answer)
    expect(t.fault(kBase + 5, true, 255, now, jobs) == NOT_OURS && jobs.empty(), "rules: a fault elsewhere is not ours");
    m.acc[5] = RW;
    expect(t.fault(kBase + 5, true, 255, now, jobs) == RETRY, "rules: a fault on a page that has its access back by now runs again");
    expect(t.fault(a + 40, false, 15, now, jobs) == PAY && jobs.size() == 1 && jobs[0].lo == a + 32 && jobs[0].hi == a + 48,
           "rules: a read of an owed page takes its block to pay");
    expect(t.state_of(a + 40) == PAYING && t.state_of(a + 48) == OWED, "rules: the rest stays owed");
    agree(t, m, "fault paying");
    // (a write to an owed page pays too, and the debt is not dropped)
    std::vector<Job> wj;
    expect(t.fault(a + 50, true, 0, now, wj) == PAY && wj.size() == 1 && wj[0].lo == a + 50 && wj[0].hi == a + 51 && wj[0].seq == 10,
           "rules: a write to an owed page pays first: its debt is a job, not dropped");
    // (another thread touching a page in PAYING waits for the copy, then runs again)
    std::atomic<int> got{-1};
    std::thread other([&] {
        std::vector<Job> none;
        got = t.fault(a + 41, false, 15, now, none);
        if (!none.empty()) fail("a second payer for a page being paid");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect(got == -1, "rules: a touch of a page being paid waits");
    // (the copy came between a recording's end and its submit: what that recording wrote is still owed)
    t.mark_rest(jobs, 9);
    std::vector<Job> early;
    t.take_pending(10, early);
    expect(early.size() == 1 && early[0].lo == a + 32, "rules: that recording's submit finds the rest while the payment still runs");
    t.end_pay(jobs, BY_FAULT, now);
    other.join();
    expect(got == RETRY, "rules: and runs again once the copy is done");
    expect(t.state_of(a + 40) == PENDING, "rules: paid before its newest recording was submitted: PENDING, access back");
    agree(t, m, "pending");
    t.mark_rest(wj, 10);
    t.end_pay(wj, BY_FAULT, now);
    expect(t.state_of(a + 50) == 0, "rules: paid after its newest recording was submitted: nothing left");
    jobs.clear();
    t.take_pending(9, jobs);
    expect(jobs.empty(), "rules: nothing to finish before that recording is submitted");
    t.take_pending(10, jobs);
    expect(jobs.size() == 1 && jobs[0].lo == a + 32 && jobs[0].hi == a + 48, "rules: the rest is copied right after its submit");
    t.advance(a + 32, a + 48, kSrc, 11, e, now + 5 * kHoldMaxMs, true, skip);
    expect(t.state_of(a + 40) == PENDING, "rules: a PENDING page is not watched or skipped meanwhile");
    t.end_pending(jobs);
    expect(t.state_of(a + 40) == 0, "rules: then it is untracked");
    agree(t, m, "pending paid");
    jobs.clear();

    // -- host calls: nothing gets protected while one has memory in the kernel's hands; its range is settled first
    t.io_begin();
    t.settle(a, b + 16, BY_HOST, now, jobs);
    expect(!jobs.empty(), "rules: a host call's range: the owed pages are jobs");
    t.end_pay(jobs, BY_HOST, now);
    jobs.clear();
    for (uint64_t p = a; p < b + 16; p++)
        if (t.state_of(p) != 0) fail("a page of a host call's range is still tracked", p - kBase, (uint64_t)t.state_of(p));
    agree(t, m, "host call");
    const uint64_t far = kBase + 300;
    t.advance(far, far + 32, kSrc, 12, e, now, true, skip);
    expect(t.state_of(far) == 0 && t.counters().refused_io == 1, "rules: no watch starts while a host call is in the kernel");
    t.io_end();
    t.advance(far, far + 32, kSrc, 13, e, now, true, skip);
    expect(t.state_of(far) == WATCH, "rules: and it does afterwards");
    // (a GPU read leaves a watch alone: the page is whole and readable; a host call ends it and holds it)
    t.settle(far, far + 32, BY_GPU, now, jobs);
    expect(jobs.empty() && t.state_of(far) == WATCH, "rules: a GPU read does not end a watch");
    t.settle(far, far + 16, BY_HOST, now, jobs);
    expect(jobs.empty() && t.state_of(far) == 0 && t.state_of(far + 16) == WATCH, "rules: a host call ends the watch of its range");
    t.advance(far, far + 16, kSrc, 14, e, now, true, skip);
    expect(t.state_of(far) == 0, "rules: and its blocks are held");
    agree(t, m, "host call over a watch");

    // -- another mirror's pages are never skipped, and code that gives a mirror up settles it first
    e.collects_begun++, e.collects++, e.caps_made++, e.caps_taken = e.caps_made + 1;
    t.advance(far + 16, far + 32, kSrc, 15, e, now, true, skip);
    expect(t.state_of(far + 16) == OWED, "rules: owed again");
    skip.clear();
    const Src other_src{33, 22, kBase << kPageShift};
    t.advance(far + 16, far + 32, other_src, 16, e, now, true, skip);
    expect(skip.empty() && t.counters().foreign == 1, "rules: a page owed from another mirror is copied, not skipped");
    t.settle(kBase, kBase + kN, BY_GIVEUP, now, jobs);
    expect(jobs.size() == 1 && jobs[0].src == kSrc && jobs[0].seq == 15, "rules: giving the mirror up pays what it alone holds");
    t.end_pay(jobs, BY_GIVEUP, now);
    expect(!t.any(), "rules: nothing is left protected");
    agree(t, m, "given up");

    // -- pages that cannot be protected are left as they are
    m.refuse = true;
    t.advance(kBase, kBase + 16, kSrc, 17, e, now + 10 * kHoldMaxMs, true, skip);
    expect(t.state_of(kBase) == 0 && t.counters().protect_failed == 1, "rules: a page whose protection cannot be changed is not watched");
    // (and may_start false: nothing new is watched)
    m.refuse = false;
    t.advance(kBase, kBase + 16, kSrc, 18, e, now + 10 * kHoldMaxMs, false, skip);
    expect(t.state_of(kBase) == 0, "rules: no new watch where the caller says so");
}

// ---- stress: threads against a stand-in for the GPU ----
struct World {
    Model m;
    Table<Model> t{m};
    Src src{100, 200, kBase << kPageShift};
    // (guest memory and the mirror, a page each: the GPU's word (the recording that wrote it last) and the CPU's word)
    std::atomic<uint64_t> guest_gpu[kN], guest_cpu[kN], mirror_cpu[kN];
    std::atomic<uint64_t> mirror_exec[kN];   // (the mirror as of the submitted recordings: what a copy reads)
    std::atomic<uint64_t> visible[kN];       // (what guest memory has to show a reader by now, at least)
    std::atomic<uint8_t> dirty[kN], copying[kN];
    std::mutex queue_mu;
    uint64_t submitted = 0;                  // (queue_mu)
    std::atomic<uint64_t> now{1000}, cpu_seq{1};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> pays{0}, faults{0}, kernel{0}, reads{0};
    // (the payers' slots, as vk_memory.cpp lazy_pay holds them: each slot's command buffer used by one payment at a time)
    Payers payers;
    std::atomic<uint8_t> slot_busy[kMaxPayers];
    World(int n) {
        for (uint64_t i = 0; i < kN; i++) guest_gpu[i] = 0, guest_cpu[i] = 0, mirror_cpu[i] = 0, mirror_exec[i] = 0, visible[i] = 0, dirty[i] = 0, copying[i] = 0;
        for (auto& b : slot_busy) b = 0;
        payers.init(n);
    }
    /// The GPU's copy of jobs, mirror to guest memory, after everything submitted (the copy is made under the queue's lock: its
    /// place on the queue); then the payment waits for it, with its slot, while other payments go onto the queue and wait for
    /// theirs, and the table hears of it
    void pay(std::vector<Job>& jobs, Why why) {
        {
            const Payers::Hold slot(payers);
            if (slot.slot < 0 || slot.slot >= payers.size()) fail("a payer's slot out of range", (uint64_t)slot.slot);
            else if (slot_busy[slot.slot].exchange(1)) fail("a payer's slot used by two payments at once", (uint64_t)slot.slot);
            {
                std::lock_guard q(queue_mu);
                t.mark_rest(jobs, submitted);
                for (const Job& j : jobs) copy(j);
            }
            // (the wait for the copy: a submit and other payments may come meanwhile; now and then a longer one, as a GPU's)
            if (pays % 4 == 0) std::this_thread::sleep_for(std::chrono::microseconds(100));
            else std::this_thread::yield();
            if (slot.slot >= 0 && slot.slot < kMaxPayers) slot_busy[slot.slot] = 0;
        }
        t.end_pay(jobs, why, now);
        pays++;
    }
    void copy(const Job& j) {
        if (!(j.src == src)) fail("a copy from a mirror that was given up", j.lo - kBase);
        for (uint64_t p = j.lo; p < j.hi; p++) {
            const uint64_t i = p - kBase;
            if (copying[i].exchange(1)) fail("two copies of one page at once", i);
            std::lock_guard lk(m.page_mu[i]);
            // (the copy is of the whole page: it must not put older bytes over what the CPU wrote)
            if (guest_cpu[i] != mirror_cpu[i]) fail("a payment would lose a CPU write", i, guest_cpu[i]);
            guest_gpu[i] = mirror_exec[i].load();
            guest_cpu[i] = mirror_cpu[i].load();
            copying[i] = 0;
        }
    }
    /// A CPU access, as the MMU and the fault handler make it
    void access(uint64_t i, bool write, std::mt19937_64& rng) {
        const uint64_t must = visible[i];
        for (int tries = 0; tries < 1000000; tries++) {
            {
                std::lock_guard lk(m.page_mu[i]);
                if (m.acc[i] == RW || (m.acc[i] == RO && !write)) {
                    if (write) guest_cpu[i] = cpu_seq++, dirty[i] = 1;
                    else if (guest_gpu[i] < must) fail("a reader saw guest memory older than a submitted write", i, must);
                    reads++;
                    return;
                }
            }
            std::vector<Job> jobs;
            const Fault f = t.fault(kBase + i, write, rng() % 2 ? 15 : 0, now, jobs);
            faults++;
            if (f == NOT_OURS) fail("a fault on a protected page was not recognised", i);
            if (f == PAY) pay(jobs, BY_FAULT);
        }
        fail("an access never got through", i);
    }
};

void stress(double seconds, int n) {
    World w(n);
    std::vector<std::thread> th;
    // the recorder: collect, record, end (the lazy decisions), a window, submit
    th.emplace_back([&] {
        std::mt19937_64 rng(1);
        Epoch e;
        e.captures = true;
        std::vector<uint64_t> capture;   // (the pages of the capture made a recording ago: the game runs ahead of the replay)
        uint64_t capture_no = 0;
        for (uint64_t rec = 1; !w.stop; rec++) {
            // (that capture is taken now: every page of it is named as written by the CPU, and synced into the mirror)
            for (uint64_t i : capture) {
                if (w.t.cpu_wrote(kBase + i, w.now)) fail("a CPU write to an owed page was found by a capture", i);
                std::lock_guard lk(w.m.page_mu[i]);
                w.mirror_cpu[i] = w.guest_cpu[i].load();
            }
            e.caps_taken = capture_no;
            // (and the next one is made: numbered first, then the write watch's marks collected)
            capture_no = ++e.caps_made;
            capture.clear();
            for (uint64_t i = 0; i < kN; i++)
                if (w.dirty[i].exchange(0)) capture.push_back(i);
            e.collects_begun++, e.collects++;
            // (now and then the mirror is given up and made again: settled first)
            if (rng() % 97 == 0) {
                std::vector<Job> jobs;
                w.t.settle(kBase, kBase + kN, BY_GIVEUP, w.now, jobs);
                if (!jobs.empty()) w.pay(jobs, BY_GIVEUP);
                for (uint64_t i = 0; i < kN; i++) {
                    const int s = w.t.state_of(kBase + i);
                    if (s != 0 && s != PENDING) fail("a page is still tracked after its mirror was given up", i, (uint64_t)s);
                }
                std::lock_guard q(w.queue_mu);
                w.src.mirror++;
            }
            // (a GPU read through the import: settled first, and then guest memory is the mirror's)
            if (rng() % 5 == 0) {
                const uint64_t a = rng() % (kN - 40), n = 1 + rng() % 40;
                std::vector<Job> jobs;
                w.t.settle(kBase + a, kBase + a + n, BY_GPU, w.now, jobs);
                if (!jobs.empty()) w.pay(jobs, BY_GPU);
                for (uint64_t i = a; i < a + n; i++)
                    if (w.guest_gpu[i] < w.visible[i]) fail("a GPU read saw guest memory older than a submitted write", i);
            }
            // (this recording's runs: mostly the same ranges, as a game's buffers are)
            struct Run { uint64_t a, b; Skip skip; };
            std::vector<Run> runs;
            for (int k = 0; k < 6; k++) {
                const uint64_t a = k < 4 ? 40 + k * 100 : rng() % (kN - 64), n = k < 4 ? 64 : 1 + rng() % 64;
                runs.push_back({a, a + n, {}});
            }
            for (Run& r : runs) w.t.advance(kBase + r.a, kBase + r.b, w.src, rec, e, w.now, true, r.skip);
            // (the window between the recording's end and its submit: the recorder itself may touch a page here)
            if (rng() % 3 == 0) w.access(rng() % kN, rng() % 4 == 0, rng);
            if (rng() % 2) std::this_thread::yield();
            {   // (the submit: the GPU writes the mirror, the copies that were not left out, and what was paid in the window)
                std::lock_guard q(w.queue_mu);
                for (const Run& r : runs)
                    for (uint64_t i = r.a; i < r.b; i++) w.mirror_exec[i] = rec;
                for (const Run& r : runs)
                    for (uint64_t i = r.a; i < r.b; i++) {
                        bool skipped = false;
                        for (const auto& s : r.skip) skipped |= kBase + i >= s.first && kBase + i < s.second;
                        if (!skipped) {
                            std::lock_guard lk(w.m.page_mu[i]);
                            w.guest_gpu[i] = rec;
                        }
                    }
                w.submitted = rec;
                // (what was paid in the window: the rest of it now, as this recording's own write-back would have landed. The
                // page had its access back meanwhile, so the CPU may have written it: the GPU's word alone is checked)
                std::vector<Job> pend;
                w.t.take_pending(rec, pend);
                for (const Job& j : pend)
                    for (uint64_t p = j.lo; p < j.hi; p++) {
                        std::lock_guard lk(w.m.page_mu[p - kBase]);
                        w.guest_gpu[p - kBase] = w.mirror_exec[p - kBase].load();
                    }
                w.t.end_pending(pend);
                for (const Run& r : runs)
                    for (uint64_t i = r.a; i < r.b; i++) w.visible[i] = rec;
            }
            w.now += 40;
        }
    });
    // CPU readers and writers
    for (int k = 0; k < 3; k++)
        th.emplace_back([&, k] {
            std::mt19937_64 rng(100 + k);
            while (!w.stop) {
                // (mostly a few pages, so that most runs stay untouched and become owed; writers keep to pages of their own)
                const uint64_t i = rng() % 8 ? 40 + (rng() % 4) * 100 + rng() % 8 : rng() % kN;
                const bool write = k == 0 && rng() % 3 == 0;
                w.access(write ? i / 3 * 3 : i, write, rng);
                if (rng() % 64 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    // host calls that hand a range to the kernel
    th.emplace_back([&] {
        std::mt19937_64 rng(7);
        while (!w.stop) {
            const uint64_t a = rng() % (kN - 24), n = 1 + rng() % 24;
            w.t.io_begin();
            std::vector<Job> jobs;
            w.t.settle(kBase + a, kBase + a + n, BY_HOST, w.now, jobs);
            if (!jobs.empty()) w.pay(jobs, BY_HOST);
            for (int pass = 0; pass < 2; pass++) {   // (the kernel's access: no fault, it fails on a page without access)
                for (uint64_t i = a; i < a + n; i++) {
                    std::lock_guard lk(w.m.page_mu[i]);
                    if (w.m.acc[i] != RW) fail("the kernel was handed a protected page", i, w.m.acc[i]);
                }
                std::this_thread::yield();
            }
            w.kernel++;
            w.t.io_end();
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    w.stop = true;
    for (auto& x : th) x.join();
    std::vector<Job> jobs;
    w.t.settle(kBase, kBase + kN, BY_GIVEUP, w.now, jobs);
    if (!jobs.empty()) w.pay(jobs, BY_GIVEUP);
    agree(w.t, w.m, "the stress test's end");
    const Counters c = w.t.counters();
    std::printf("stress: %llu accesses, %llu faults, %llu payments, %llu host calls; pages watched %llu, owed %llu, skipped %llu, "
                "paid by fault %llu, host %llu, GPU %llu, give-up %llu, submit %llu; waited %llu; payers %d, %d at once at most, "
                "%llu waits for a slot\n",
                (unsigned long long)w.reads.load(), (unsigned long long)w.faults.load(), (unsigned long long)w.pays.load(),
                (unsigned long long)w.kernel.load(), (unsigned long long)c.watched, (unsigned long long)c.armed,
                (unsigned long long)c.skipped_pages, (unsigned long long)c.paid_pages[BY_FAULT], (unsigned long long)c.paid_pages[BY_HOST],
                (unsigned long long)c.paid_pages[BY_GPU], (unsigned long long)c.paid_pages[BY_GIVEUP], (unsigned long long)c.paid_pages[BY_SUBMIT],
                (unsigned long long)c.waited, w.payers.size(), w.payers.most_at_once(), (unsigned long long)w.payers.waited());
    // (a run that exercised nothing proves nothing: with several payers, two payments have to have been in flight at once)
    if (!c.armed || !c.skipped_pages || !c.paid_pages[BY_FAULT] || !(c.paid_pages[BY_HOST] + c.paid_pages[BY_GPU]) ||
        (n > 1 && w.payers.most_at_once() < 2))
        fail("the stress test did not reach its paths: see the line above");
    if (w.payers.most_at_once() > n) fail("more payments copied at once than there are slots", (uint64_t)w.payers.most_at_once(), (uint64_t)n);
}

/// The payers' slots (wb_lazy_payers.h): RX_VK_LAZY_PAYERS read as 1 to 4, 4 when not set; n slots, each held by one payment
/// at a time, a payment past n waits for one to be given back; one slot is one payment at a time, as the renderer had it first
void payer_slots() {
    struct Env { const char* v; int want; };
    for (const Env& k : {Env{nullptr, 4}, Env{"", 4}, Env{"1", 1}, Env{"2", 2}, Env{"4", 4}, Env{"0", 1}, Env{"-3", 1}, Env{"9", 4},
                         Env{"x", 4}}) {
        char what[96];
        std::snprintf(what, sizeof what, "payers: RX_VK_LAZY_PAYERS=%s is %d at once", k.v ? k.v : "(not set)", k.want);
        expect(payers_from(k.v) == k.want, what);
    }
    {   // (four slots: four payments hold four different ones without waiting; a fifth waits until one is given back)
        Payers p;
        p.init(4);
        expect(p.size() == 4, "payers: four slots");
        int held[4];
        unsigned seen = 0;
        for (int& h : held) h = p.take(), seen |= 1u << h;
        expect(seen == 0xF && p.waited() == 0 && p.most_at_once() == 4, "payers: four payments hold four different slots, none waits");
        std::atomic<int> fifth{-1};
        std::thread t([&] { fifth = p.take(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        expect(fifth == -1, "payers: a fifth payment waits while all four slots are held");
        p.give(held[2]);
        t.join();
        expect(fifth == held[2] && p.waited() == 1, "payers: the fifth gets the slot given back");
        p.give(fifth), p.give(held[0]), p.give(held[1]), p.give(held[3]);
        expect(p.most_at_once() == 4, "payers: never more than four at once");
    }
    {   // (one slot: one payment at a time, the second waits for the first)
        Payers p;
        p.init(1);
        const int a = p.take();
        std::atomic<bool> got{false};
        std::thread t([&] { Payers::Hold h(p); got = true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        expect(a == 0 && !got, "payers: with one slot a second payment waits for the first");
        p.give(a);
        t.join();
        expect(got && p.most_at_once() == 1, "payers: with one slot, one payment at a time");
    }
    {   // (more than four asked: four)
        Payers p;
        p.init(9);
        expect(p.size() == kMaxPayers, "payers: at most four slots");
    }
}

/// Whether it runs (vk_memory.cpp lazy_decide): off unless the row or the switch says on; the switch goes over the row both
/// ways; never outside Windows, nor without mirrors; on AMD's driver the row alone does not turn it on, the switch does
void choice() {
    struct Case { const char* env; bool row, windows, amd, mirrors; int mode; const char* what; };
    const Case cases[] = {
        {nullptr, false, true, false, true, 0, "choice: off by default"},
        {"", false, true, false, true, 0, "choice: an empty switch is no switch"},
        {nullptr, true, true, false, true, 1, "choice: the row turns it on"},
        {"0", true, true, false, true, 0, "choice: RX_VK_LAZY_WB=0 over the row"},
        {"1", false, true, false, true, 1, "choice: RX_VK_LAZY_WB=1 over the row"},
        {"2", false, true, false, true, 2, "choice: RX_VK_LAZY_WB=2, the self-check"},
        {"7", false, true, false, true, 2, "choice: a larger number is the self-check"},
        {nullptr, true, true, true, true, 0, "choice: the row alone leaves it off on AMD's driver"},
        {"1", false, true, true, true, 1, "choice: RX_VK_LAZY_WB=1 turns it on on AMD's driver"},
        {"2", false, true, true, true, 2, "choice: RX_VK_LAZY_WB=2 on AMD's driver"},
        {nullptr, true, false, false, true, 0, "choice: off outside Windows, the row on"},
        {"1", false, false, false, true, 0, "choice: off outside Windows, the switch on"},
        {nullptr, true, true, false, false, 0, "choice: off without mirrors"},
        {"1", false, true, false, false, 0, "choice: off without mirrors, the switch on"},
    };
    for (const Case& k : cases) {
        const Choice c = choose(k.env, k.row, k.windows, k.amd, k.mirrors);
        expect(c.mode == k.mode && c.why && *c.why, k.what);
    }
    expect(std::strstr(choose(nullptr, true, true, true, true).why, "RX_VK_LAZY_WB=1") != nullptr,
           "choice: the AMD line names the switch that turns it on");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "stress")) stress(argc > 2 ? std::atof(argv[2]) : 2.0, argc > 3 ? std::atoi(argv[3]) : kMaxPayers);
    else rules(), choice(), payer_slots();
    if (failures) return std::printf("%d check(s) failed\n", failures), 1;
    std::printf("PASS: %s\n", argc > 1 ? argv[1] : "rules");
    return 0;
}
