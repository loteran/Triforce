// jobs_wait_for (gpu/vk/vk_jobs.cpp): a wait with a bound. The job waited for, still queued, goes to the front for the next
// worker and is never run on the waiting thread; the wait ends at its bound, or when the flag is set. Built against stand-ins
// for the renderer's headers with one worker (tests/test_jobs_wait_for.py).
#include "vk_jobs.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace mkwvk;

namespace {

int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) std::printf("FAIL: %s\n", what), failures++;
}

std::mutex gate_mu;
std::condition_variable gate_cv;
bool gate_open = false;
std::atomic<int> held{0};
std::mutex order_mu;
std::string order;
void note(char c) {
    std::lock_guard lk(order_mu);
    order += c;
}

}   // namespace

int main() {
#ifdef _WIN32
    _putenv("RX_VK_WORKERS=1");
#else
    setenv("RX_VK_WORKERS", "1", 1);
#endif
    // (the one worker held, so that what is pushed after stays queued)
    std::atomic<bool> hold_done{false};
    jobs_push([&] {
        held++;
        std::unique_lock lk(gate_mu);
        gate_cv.wait(lk, [] { return gate_open; });
        hold_done.store(true);
    }, false, &hold_done);
    const auto t0 = std::chrono::steady_clock::now();
    while (!held.load() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) std::this_thread::yield();
    expect(held.load() == 1, "the worker took the holding job");

    // (set already: at once)
    std::atomic<bool> done_before{true};
    expect(jobs_wait_for(done_before, 1000), "a flag set already is had at once");

    std::atomic<bool> a{false}, b{false}, f{false};
    std::thread::id ran_on{};
    jobs_push([&] { note('A'), a.store(true); });
    jobs_push([&] { note('B'), b.store(true); });
    jobs_push([&] { note('F'), ran_on = std::this_thread::get_id(), f.store(true, std::memory_order_release); }, false, &f);
    const auto w0 = std::chrono::steady_clock::now();
    const bool got = jobs_wait_for(f, 60);
    const auto waited = std::chrono::steady_clock::now() - w0;
    expect(!got && !f.load(), "the worker held: the wait ends at its bound without the job");
    expect(waited >= std::chrono::milliseconds(50), "it waited out its bound");
    expect(waited < std::chrono::seconds(3), "and not much longer");
    expect(ran_on == std::thread::id{}, "the job was not run on the waiting thread");

    {   // (let the worker go: the job waited for comes first, then the ones queued before it, in their order)
        std::lock_guard lk(gate_mu);
        gate_open = true;
    }
    gate_cv.notify_all();
    expect(jobs_wait_for(f, 5000), "the job ran once a worker was free");
    expect(ran_on != std::this_thread::get_id(), "on the worker");
    jobs_wait_for(b, 5000);
    const auto t1 = std::chrono::steady_clock::now();
    while (!(a.load() && b.load()) && std::chrono::steady_clock::now() - t1 < std::chrono::seconds(5)) std::this_thread::yield();
    {
        std::lock_guard lk(order_mu);
        expect(order == "FAB", ("the job waited for went to the front (order " + order + ")").c_str());
    }

    if (failures) return EXIT_FAILURE;
    std::printf("PASS: jobs_wait_for keeps its bound, moves its job to the front and never runs it on the waiting thread\n");
    std::fflush(stdout);
    std::_Exit(EXIT_SUCCESS);   // (the worker is detached and waits forever)
}
