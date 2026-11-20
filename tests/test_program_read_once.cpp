// (Triforce) The draws' own reads of the program cache with the shaders' lock let go (gpu/vk/program_read_once.h). The runner,
// tests/test_program_read_once.py, cuts vk_shader.cpp's readahead_take, readahead_choose_locked and program_cache_load out as
// they are into read_once_functions.inc; this file gives them a program cache whose reads wait on the test (a slow disk), the
// shaders' lock as a mutex that knows its owner, and the picks' lock-taking as program_pick and mesh_program_for have it
// (shader_mu held around program_cache_load). Each scenario is a process of its own: test_program_read_once <scenario>.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "program_readahead.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::fflush(stderr); std::_Exit(1); } } while (0)
#define PROF_EXACT()

using Clock = std::chrono::steady_clock;

namespace mkwvk {

/// shader_mu: a mutex that knows which thread holds it
struct Owned {
    std::mutex m;
    std::atomic<std::thread::id> owner{};
    void lock() {
        m.lock();
        owner = std::this_thread::get_id();
    }
    void unlock() {
        owner = std::thread::id();
        m.unlock();
    }
    bool try_lock() {
        if (!m.try_lock()) return false;
        owner = std::this_thread::get_id();
        return true;
    }
    bool mine() const { return owner.load() == std::this_thread::get_id(); }
};
Owned shader_mu;
program_read_once::Reads<Owned> draw_once(shader_mu);

struct Program {
    uint64_t key = 0;
    std::atomic<bool> ready{false};
};
#include "read_once_types.inc"   // (vk_shader.cpp's DiskRecord)

// (the program cache's list and file, as vk_shader.cpp has them)
bool disk_on = true;
FILE* disk_in = nullptr;
std::mutex disk_mu;
std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_records;
void disk_init() {}
bool probe_names(uint64_t) { return false; }
void logf(const char*, ...) {}
struct PackSkip {
    PackSkip() {}
};

// (the read-ahead's state; its condition variable waits as long as the test needs, not program_readahead::kWaitMs: the worker
// here is the test, which takes its time on purpose)
struct SlowCv {
    std::condition_variable cv;
    template <class Lock, class Duration, class Pred> bool wait_for(Lock& lk, Duration, Pred pred) {
        return cv.wait_for(lk, std::chrono::seconds(5), pred);
    }
    template <class Lock, class Pred> void wait(Lock& lk, Pred pred) { cv.wait(lk, pred); }
    void notify_all() { cv.notify_all(); }
};
std::mutex ra_mu;
SlowCv ra_cv;
std::deque<int64_t> ra_asks, ra_note_asks;
std::unordered_map<uint64_t, std::vector<std::unique_ptr<Program>>> ra_parked;
std::unordered_set<uint64_t> ra_reading;
std::deque<std::pair<uint64_t, std::vector<DiskRecord>>> ra_pending;
std::vector<std::pair<int64_t, uint64_t>> ra_order, ra_note_order;
std::atomic<int64_t> st_ra_read{0}, st_ra_taken{0}, st_ra_waits{0}, st_ra_wait_ns{0}, st_ra_dropped{0}, st_ra_let_go{0};
std::atomic<int64_t> st_ra_by_notes{0}, st_ra_read_ns{0}, st_draw_reads{0}, st_draw_read_ns{0};
std::atomic<int64_t> st_once_waits{0}, st_once_wait_ns{0}, st_once_relock_ns{0}, st_once_unlocked{0};
int readahead_count() { return 32; }
bool readahead_notes() { return false; }
bool read_unlocked() {
    static const bool on = program_read_once::on(getenv("RX_VK_PROGRAM_READ_UNLOCKED"));
    return on;
}
// (the sessions' order, program_order.h: off here; tests/test_program_order.py has it)
program_order::Ahead ra_ahead;
std::atomic<int64_t> st_order_taken{0}, st_order_read{0}, st_order_let_go{0};
bool order_on() { return false; }
void order_note(uint64_t) {}
void ahead_met_locked(uint64_t) {}
void readahead_why(uint64_t) {}
size_t shelf_locked() { return ra_parked.size(); }
std::atomic<int> asks{0}, dropped{0};
void readahead_ask(uint64_t, int64_t) { asks++; }
void readahead_drop(std::vector<std::unique_ptr<Program>>& variants) {
    dropped += (int)variants.size();
    variants.clear();
}

// (the slow disk: a key's first read runs the scenario's step for it (during), which takes its time; reads[key]: the reads of
// it; held[key]: whether shader_mu was the reader's then)
std::mutex test_mu;
std::map<uint64_t, int> reads;
std::map<uint64_t, bool> held;
std::map<uint64_t, std::function<void()>> during;   // (run in a key's first read, the lock as the reader has it)
void program_cache_read(FILE*, std::mutex* mu, uint64_t key, const std::vector<DiskRecord>& recs,
                        std::vector<std::unique_ptr<Program>>& variants) {
    CHECK(mu == &disk_mu);
    std::function<void()> fn;
    {
        std::lock_guard lk(test_mu);
        reads[key]++;
        held[key] = shader_mu.mine();
        if (auto it = during.find(key); it != during.end()) fn = std::move(it->second), during.erase(it);
    }
    if (fn) fn();
    for (size_t i = 0; i < recs.size(); i++) {
        auto p = std::make_unique<Program>();
        p->key = key;
        p->ready = true;
        variants.push_back(std::move(p));
    }
}

#include "read_once_functions.inc"   // (vk_shader.cpp's readahead_choose_locked, readahead_take and program_cache_load)

} // namespace mkwvk

using namespace mkwvk;

std::mutex programs_mu;   // (the programs' lists: vk_shader.cpp's are under shader_mu, as here; this guards the map's nodes for the checks)
std::unordered_map<uint64_t, std::vector<std::unique_ptr<Program>>> programs;
std::vector<std::unique_ptr<Program>>& list_of(uint64_t key) {
    std::lock_guard lk(programs_mu);
    return programs[key];
}
/// A draw's pick, as program_pick and mesh_program_for take the lock: shader_mu held whole; the variants' count after the load
size_t pick(uint64_t key) {
    std::lock_guard lk(shader_mu);
    auto& variants = list_of(key);
    program_cache_load(key, variants);
    CHECK(shader_mu.mine());   // (held again on the way out)
    return variants.size();
}
void listed(uint64_t key, size_t records) {
    std::lock_guard lk(disk_mu);
    disk_records[key].resize(records);
    for (size_t i = 0; i < records; i++) disk_records[key][i] = {(int64_t)(key * 1000 + i * 100), 64, 0};
}
bool is_listed(uint64_t key) {
    std::lock_guard lk(disk_mu);
    return disk_records.count(key) != 0;
}
/// Waits up to ms for pred (polled): whether it came
template <class Pred> bool until(Pred pred, int ms = 3000) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (Clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
void on_read(uint64_t key, std::function<void()> fn) {
    std::lock_guard lk(test_mu);
    during[key] = std::move(fn);
}

/// A draw reads program 1 itself; while it reads, another draw picks program 2 (no record): that pick ends before the read
/// does, which it cannot while the reader holds shader_mu. The record stays listed until the read is done
int read_scenario() {
    listed(1, 1);
    std::atomic<bool> other_done{false};
    on_read(1, [&] {
        CHECK(is_listed(1) && draw_once.reading(1));   // (listed while read: every other reader of the list sees it kept)
        std::thread other([&] { pick(2), other_done = true; });
        CHECK(until([&] { return other_done.load(); }));   // (shader_mu let go during the read)
        other.join();
    });
    const size_t n = pick(1);
    CHECK(!held.at(1));
    CHECK(n == 1 && reads.at(1) == 1);
    CHECK(!is_listed(1) && !draw_once.reading(1));
    CHECK(st_draw_reads == 1 && st_once_unlocked == 1 && asks == 1);
    std::puts("read: shader_mu let go during the draw's read, the record listed until it was done");
    return 0;
}

/// Two draws of program 1: the second comes while the first reads, waits for the first's result (with shader_mu let go: a third
/// draw's pick goes on meanwhile), and finds the variants in the list; the record is read once
int second_scenario() {
    listed(1, 2);   // (two variants)
    size_t second = 0;
    std::atomic<bool> third_done{false};
    std::thread t2;
    on_read(1, [&] {
        t2 = std::thread([&] { second = pick(1); });
        CHECK(until([&] { return draw_once.open() >= 2; }));   // (the second waits for this read with the lock let go)
        std::thread t3([&] { pick(2), third_done = true; });
        CHECK(until([&] { return third_done.load(); }));        // (and a third pick goes on meanwhile)
        t3.join();
    });
    const size_t first = pick(1);
    t2.join();
    CHECK(first == 2 && second == 2);   // (the second found the first's variants: it did not go on with an empty list)
    CHECK(reads.at(1) == 1);            // (read once)
    CHECK(st_once_waits == 1 && dropped == 0);
    CHECK(!is_listed(1));
    std::puts("second: the second draw waited for the first's read and took its result");
    return 0;
}

/// A draw comes to program 3 while the read-ahead's worker reads it: it waits for the worker with shader_mu let go (another
/// draw's pick goes on), then takes what the worker parked; nothing is read from the disk for it
int worker_scenario() {
    listed(3, 1);
    {
        std::lock_guard lk(ra_mu);
        ra_reading.insert(3);
    }
    std::atomic<bool> other_done{false};
    std::thread worker([&] {
        CHECK(until([&] { return draw_once.open() >= 1; }));   // (the draw waits for the worker, the lock let go)
        std::thread other([&] { pick(2), other_done = true; });
        CHECK(until([&] { return other_done.load(); }));
        other.join();
        std::lock_guard lk(ra_mu);
        auto p = std::make_unique<Program>();
        p->key = 3, p->ready = true;
        ra_parked[3].push_back(std::move(p));
        ra_reading.erase(3);
        ra_cv.notify_all();
    });
    const size_t n = pick(3);
    worker.join();
    CHECK(n == 1 && st_ra_taken == 1 && st_ra_waits == 1);
    CHECK(!reads.count(3));       // (taken, not read)
    CHECK(!is_listed(3) && asks == 1);
    std::puts("worker: the wait for the read-ahead's worker let shader_mu go");
    return 0;
}

/// A program's release (vk_shader.cpp program_released_after_reads) while a draw reads program 1 with the lock let go: it waits
/// until the read is done and the lock taken again; while it holds the gate a draw reads under the lock, as before; after it,
/// with the lock let go again
int release_scenario() {
    listed(1, 1), listed(4, 1), listed(5, 1);
    std::atomic<bool> closing{false}, closed{false};
    on_read(1, [&] {
        CHECK(until([&] { return closing.load(); }));
        CHECK(!until([&] { return closed.load(); }, 500));   // (the release must not get past the gate meanwhile)
    });
    std::thread reader([&] { pick(1); });
    CHECK(until([&] { return draw_once.reading(1); }));
    std::thread release([&] {
        closing = true;
        const program_read_once::Closed gate(draw_once);
        closed = true;
        {
            std::lock_guard lk(shader_mu);   // (where program_released erases the program's stage codes)
            CHECK(draw_once.open() == 0);
            CHECK(!draw_once.reading(1));
        }
        pick(4);   // (a draw while the gate is closed: read under the lock)
    });
    release.join();
    reader.join();
    pick(5);
    CHECK(!held.at(1) && held.at(4) && !held.at(5));
    CHECK(st_once_unlocked == 2);
    std::puts("release: the release waited for the read with the lock let go; under its gate the read held the lock");
    return 0;
}

/// The read-ahead's choice leaves a program a draw is reading alone (vk_shader.cpp readahead_choose_locked)
int choose_scenario() {
    listed(1, 1), listed(6, 1);
    on_read(1, [&] {
        std::lock_guard lk(ra_mu);
        ra_asks.push_back(0);
        readahead_choose_locked();
        CHECK(ra_pending.size() == 1 && ra_pending.front().first == 6);
    });
    pick(1);
    std::puts("choose: the read-ahead does not choose a program a draw is reading");
    return 0;
}

/// The precompile's load took program 1's records while a draw read them: when it already put its variants in the list, the
/// draw's go; when not yet, the draw keeps its own (it would translate the program again otherwise)
int taken_scenario() {
    listed(1, 1), listed(7, 1);
    on_read(1, [&] {
        {
            std::lock_guard lk(disk_mu);
            disk_records.erase(1);
        }
        std::lock_guard lk(shader_mu);   // (program_cache_load_some puts its variants in under shader_mu)
        auto p = std::make_unique<Program>();
        p->key = 1, p->ready = true;
        list_of(1).push_back(std::move(p));
    });
    on_read(7, [&] {
        std::lock_guard lk(disk_mu);
        disk_records.erase(7);
    });
    CHECK(pick(1) == 1 && dropped == 1);
    CHECK(pick(7) == 1 && dropped == 1);
    std::puts("taken: a draw's read whose records were taken meanwhile adds nothing the list has");
    return 0;
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string s = argv[1];
    if (s == "read") return read_scenario();
    if (s == "second") return second_scenario();
    if (s == "worker") return worker_scenario();
    if (s == "release") return release_scenario();
    if (s == "choose") return choose_scenario();
    if (s == "taken") return taken_scenario();
    std::fprintf(stderr, "no scenario %s\n", s.c_str());
    return 2;
}
