// tests/test_parallel_fixes.py puts vk_memory.cpp's own page table code (TableRun to memory_table_flush, with
// guest_pages_set and memory_table_segments_end) in the place of ACTUAL and runs it on real threads: two segments' workers
// change the same pages in one order and the queue runs their uploads in the other. No game, no GPU.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fixture {
void require(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        std::exit(1);
    }
}
// (the table as the GPU has it once the queue has run, and as the CPU maps it: one buffer in the renderer, two arrays
// here so that a change written at once and one that went through a command buffer can be told apart)
constexpr uint64_t kPages = 3ull << 20;
std::vector<uint64_t> gpu_table(kPages, 0), mapped_table(kPages, 0);
uint64_t* guest_pages = mapped_table.data();
using VkBuffer = int;
using VkCommandBuffer = int;
VkBuffer guest_pages_buffer = 7;
std::atomic<bool> par_active{false};
int updates = 0;
void vkCmdUpdateBuffer(VkCommandBuffer, VkBuffer b, uint64_t offset, uint64_t bytes, const void* data) {
    require(b == guest_pages_buffer && offset % 8 == 0 && bytes % 8 == 0, "an update of the table, in whole entries");
    std::copy_n((const uint64_t*)data, bytes / 8, gpu_table.begin() + (ptrdiff_t)(offset / 8));
    updates++;
}
VkCommandBuffer upload_command_buffer() { return 1; }
struct GprofSpan {
    int items = 0;
    uint64_t bytes = 0;
    GprofSpan(VkCommandBuffer, const char*) {}
};

/* ACTUAL */

/// A recording thread that does what it is told, one step at a time (its changes and its flush are its own thread's)
struct Worker {
    std::mutex mu;
    std::condition_variable cv;
    std::function<void()> job;
    bool quit = false, busy = false;
    std::thread thread{[this] {
        for (;;) {
            std::unique_lock lk(mu);
            cv.wait(lk, [this] { return quit || job; });
            if (quit) return;
            job();
            job = nullptr, busy = false;
            cv.notify_all();
        }
    }};
    void run(std::function<void()> f) {
        std::unique_lock lk(mu);
        job = std::move(f), busy = true;
        cv.notify_all();
        cv.wait(lk, [this] { return !busy; });
    }
    ~Worker() {
        { std::lock_guard lk(mu); quit = true; }
        cv.notify_all();
        thread.join();
    }
};
// (a pool at guest 0x1000000000: its pages' entries are the addresses of what holds them)
constexpr uint64_t kLo = 0x1000000000ull, kHi = kLo + (64ull << 12);
constexpr uint64_t P = 0xAAAA0000000ull, P2 = 0xBBBB0000000ull;
bool all_at(const std::vector<uint64_t>& t, uint64_t address) {
    for (uint64_t i = 0; i < 64; i++)
        if (t[i] != (address ? address + (i << 12) : 0)) return false;
    return true;
}

int run(int argc, char** argv) {
    require(argc == 2, "a case");
    const std::string test = argv[1];
    if (test == "segments" || test == "segments-unreconciled") {
        // segment 3's worker imports the pool (its pages -> P); then segment 1's worker retires it into another (-> 0,
        // then -> P2). The queue runs segment 1's uploads, then segment 3's.
        Worker seg1, seg3;
        par_active = true;
        seg1.run([] { memory_table_begin(); });
        seg3.run([] { memory_table_begin(); });
        seg3.run([] { guest_pages_set(kLo, kHi, P); });
        seg1.run([] { guest_pages_set(kLo, kHi, 0), guest_pages_set(kLo, kHi, P2); });
        require(all_at(mapped_table, 0) && all_at(gpu_table, 0), "nothing is written while the segments are recorded");
        seg1.run([] { require(memory_table_flush(), "segment 1 has changes"); });
        require(all_at(gpu_table, P2), "segment 1's uploads: the pool it retired into");
        seg3.run([] { require(memory_table_flush(), "segment 3 has changes"); });
        require(all_at(gpu_table, P), "segment 3's uploads come after in the queue: the retired pool's address is back");
        par_active = false;
        // the recording that follows the segments, on the replay thread (this one)
        memory_table_begin();
        if (test == "segments") {
            memory_table_segments_end();
            require(memory_table_flush(), "the changes of the segments, once more");
            require(all_at(gpu_table, P2), "the table ends as the changes left it: the pool that is there");
            memory_table_begin(), memory_table_segments_end();
            require(!memory_table_flush(), "and only once");
        } else {
            // (the hole, shown so that the case above means something)
            require(!memory_table_flush() && all_at(gpu_table, P), "without it the table ends on the retired pool");
        }
    } else if (test == "serial") {
        // no segments: nothing is kept for later, a recording's changes are flushed as before and a change outside a
        // recording is written at once
        memory_table_begin();
        guest_pages_set(kLo, kHi, P);
        require(all_at(mapped_table, 0), "held back while the recording is open");
        require(memory_table_flush() && all_at(gpu_table, P) && updates == 1, "flushed with the recording's uploads");
        guest_pages_set(kLo, kHi, P2);
        require(all_at(mapped_table, P2), "outside a recording: written at once");
        memory_table_begin(), memory_table_segments_end();
        require(!memory_table_flush(), "nothing was kept for after segments");
    } else if (test == "direct") {
        // a change written at once by a thread that is not recording while segments are (a pool the game makes then) is
        // kept too: a segment's older change that the queue runs later does not have the last word
        Worker seg1, other;
        par_active = true;
        seg1.run([] { memory_table_begin(), guest_pages_set(kLo, kHi, P); });
        other.run([] { guest_pages_set(kLo, kHi, P2); });
        require(all_at(mapped_table, P2), "written at once");
        seg1.run([] { memory_table_flush(); });
        require(all_at(gpu_table, P), "the segment's older change, later in the queue");
        par_active = false;
        memory_table_begin(), memory_table_segments_end();
        require(memory_table_flush() && all_at(gpu_table, P2), "the later change has the last word");
    } else {
        require(false, "a known case");
    }
    return 0;
}
} // namespace fixture
int main(int argc, char** argv) { return fixture::run(argc, argv); }
