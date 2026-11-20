// gpu/vk/helper_spin.h, and gpu/vk/vk_replay.cpp's ParPool and par_worker_main included unchanged (par_pool.inc, which
// tests/test_helper_spin.py cuts from the source), run on real threads: jobs are handed to the helper threads as
// replay_handle hands them (under the pool's lock, the count stored for the spinners, then the condition variable's wake).
// Cases (a process each: RX_VK_HELPER_SPIN_US is read once):
// - until: the spin's bound from the switch's text, and until(): at once when done, no spin with no bound, the bound kept,
//   an end seen within it;
// - spin (RX_VK_HELPER_SPIN_US=1000000): a helper with nothing to do spins, and a job handed over 20 ms later, with no wake
//   at all, is begun at once (not after the 1 s bound), twice; the counts say so;
// - off (0): as before, every wait for work goes to the condition variable, nothing is spun;
// - many, many-off: four helpers, thousands of hand-overs of 1 to 7 jobs: each job runs once, every batch ends, the count the
//   spinners read is the queue's at the end.
#include "helper_spin.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

static std::atomic<int> failures{0};
#define CHECK(x)                                                                                                           \
    do {                                                                                                                   \
        if (!(x)) {                                                                                                        \
            std::printf("FAIL:%d %s\n", __LINE__, #x);                                                                     \
            std::fflush(stdout);                                                                                           \
            failures++;                                                                                                    \
        }                                                                                                                  \
    } while (0)

// (Windows' thread naming and priority: nothing here)
#define THREAD_PRIORITY_ABOVE_NORMAL 1
static void* GetCurrentThread() { return nullptr; }
static void SetThreadDescription(void*, const wchar_t*) {}
static void SetThreadPriority(void*, int) {}

#include "par_pool.inc"

using Clock = std::chrono::steady_clock;
static int64_t ms_since(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}
// jobs handed over as replay_handle hands them: under the lock, pending counted, the count stored for the spinners; then the
// wake (wake = false: none, so that only the spin can see them)
static void hand_over(std::vector<std::function<void()>> fns, bool wake) {
    {
        std::lock_guard lk(par_pool.mu);
        for (auto& f : fns) {
            par_pool.jobs.push_back(std::move(f));
            par_pool.pending++;
        }
        par_pool.queued.store((int)par_pool.jobs.size(), std::memory_order_release);
    }
    if (wake) par_pool.cv.notify_all();
}
static bool all_done(int seconds) {
    std::unique_lock lk(par_pool.mu);
    return par_pool.done.wait_for(lk, std::chrono::seconds(seconds), [] { return par_pool.pending == 0; });
}

static void case_until() {
    CHECK(helper_spin::bound_ns(nullptr) == 100000);
    CHECK(helper_spin::bound_ns("") == 100000);
    CHECK(helper_spin::bound_ns("0") == 0);
    CHECK(helper_spin::bound_ns("-5") == 0);
    CHECK(helper_spin::bound_ns("250") == 250000);
    CHECK(helper_spin::bound_ns("5000000") == 1000000000LL);
    int64_t spent = -1;
    CHECK(helper_spin::until([] { return true; }, 1000000000LL, &spent) && spent == 0);
    auto t0 = Clock::now();
    CHECK(!helper_spin::until([] { return false; }, 0, &spent) && spent == 0);
    CHECK(ms_since(t0) < 50);
    t0 = Clock::now();
    CHECK(!helper_spin::until([] { return false; }, 2000000, &spent));
    CHECK(spent >= 2000000 && ms_since(t0) < 500);
    std::atomic<bool> flag{false};
    std::thread setter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        flag = true;
    });
    t0 = Clock::now();
    CHECK(helper_spin::until([&] { return flag.load(); }, 1000000000LL, &spent));
    CHECK(spent >= 15000000 && spent < 900000000 && ms_since(t0) < 900);
    setter.join();
}

// one helper; two jobs, each handed over 20 ms after it went idle
static void case_one(bool spin) {
    std::thread(par_worker_main).detach();
    for (int k = 0; k < 2; k++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::atomic<int64_t> began{-1};
        const auto t0 = Clock::now();
        hand_over({[&] { began = ms_since(t0); }}, !spin);
        CHECK(all_done(5));
        std::printf("job %d begun %lld ms after it was handed over\n", k, (long long)began.load());
        if (spin) CHECK(began >= 0 && began < 500);   // (seen in the spin, with no wake; not after the 1 s bound)
    }
    std::lock_guard lk(par_pool.mu);
    std::printf("spun %lld, slept %lld, %.3f ms spun\n", (long long)par_pool.spun.load(), (long long)par_pool.slept.load(),
                par_pool.spin_ns.load() / 1e6);
    if (spin) CHECK(par_pool.spun == 2 && par_pool.slept == 0 && par_pool.spin_ns >= 30000000);
    else   // (and the wait after the last job, counted once it has begun)
        CHECK(par_pool.spun == 0 && (par_pool.slept == 2 || par_pool.slept == 3) && par_pool.spin_ns == 0);
    CHECK(par_pool.queued == 0 && par_pool.jobs.empty() && par_pool.pending == 0);
}

// four helpers, many hand-overs
static void case_many() {
    for (int i = 0; i < 4; i++) std::thread(par_worker_main).detach();
    const int batches = 3000;
    std::vector<std::atomic<int>> ran(batches * 7);
    unsigned seed = 77;
    int handed = 0;
    for (int b = 0; b < batches; b++) {
        seed = seed * 1103515245u + 12345u;
        const int n = 1 + (int)((seed >> 8) % 7);
        std::vector<std::function<void()>> fns;
        for (int k = 0; k < n; k++) {
            const int id = handed++;
            fns.push_back([&ran, id] {
                ran[id]++;
                for (int i = 0; i < 200; i++) helper_spin::pause();
            });
        }
        hand_over(std::move(fns), true);
        if (!all_done(10)) {
            std::printf("FAIL: batch %d never ended\n", b);
            failures++;
            return;
        }
        if (seed & 0x100) std::this_thread::sleep_for(std::chrono::microseconds((seed >> 12) % 300));
    }
    for (int id = 0; id < handed; id++)
        if (ran[id] != 1) {
            std::printf("FAIL: job %d ran %d times\n", id, ran[id].load());
            failures++;
            break;
        }
    std::lock_guard lk(par_pool.mu);
    std::printf("%d jobs: spun %lld, slept %lld, %.3f ms spun\n", handed, (long long)par_pool.spun.load(),
                (long long)par_pool.slept.load(), par_pool.spin_ns.load() / 1e6);
    CHECK(par_pool.queued == 0 && par_pool.jobs.empty() && par_pool.pending == 0);
    CHECK(par_pool.spun + par_pool.slept > 0);
}

int main(int argc, char** argv) {
    const char* c = argc > 1 ? argv[1] : "";
    if (!std::strcmp(c, "until")) case_until();
    else if (!std::strcmp(c, "spin")) case_one(true);
    else if (!std::strcmp(c, "off")) case_one(false);
    else if (!std::strcmp(c, "many") || !std::strcmp(c, "many-off")) case_many();
    else std::printf("FAIL: unknown case %s\n", c), failures++;
    if (!failures) std::printf("PASS %s\n", c);
    std::fflush(stdout);
    std::_Exit(failures ? 1 : 0);   // (the helpers wait on for good: no destructors under them)
}
