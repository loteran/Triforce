// The actual save function, with authored cache bytes and Vulkan/file API spies. No driver is loaded.
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
constexpr int MOVEFILE_REPLACE_EXISTING = 1, MOVEFILE_WRITE_THROUGH = 8;
void Sleep(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif
#include "file_whole.h"   // ((Triforce, handoff §8) the save's Writing, and its .for file written whole)

// Arm only the allocating thread, after the query/copy API or immediately before thread construction.
// Real std::vector, std::string and std::thread allocations still run; nothing models their failure handling.
thread_local bool fail_next_new = false;
std::atomic<int> allocation_failures{0};
void* operator new(std::size_t n) {
    if (fail_next_new) {
        fail_next_new = false;
        ++allocation_failures;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
// Keep the replacement allocator boundary: GCC must not inline free into a caller of operator new.
[[gnu::noinline]] void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

using VkResult = int;
constexpr VkResult VK_SUCCESS = 0, VK_INCOMPLETE = 5, VK_ERROR_UNKNOWN = -13;
// props: the save in play is left out on Intel; the fixture's card is not one.
struct { int device = 1; struct { unsigned vendorID = 0x10DE; } props; } dev;
int pcache = 2;
// The stamp the file is kept for (written beside it as <file>.for), and the file's bound (0: none).
unsigned long long program_cache_stamp() { return 0x1122334455667788ull; }
#include "pcache_bound.h"
std::uint64_t pcache_bound_bytes = 0;
std::string pcache_path;
std::atomic<int64_t> pcache_built_since_save{32};
std::atomic<std::size_t> pcache_saved_size{0};
// The size the driver gives for the cache's data; a case grows it for pipelines taken in between saves.
std::atomic<std::size_t> cache_size{64};
enum Fault { NONE, VECTOR_ALLOC, VECTOR_LENGTH, TMP_ALLOC, QUERY_FAIL, EMPTY, COPY_FAIL, SHORT_WRITE, BLOCK_COPY, REPLACE_FAIL };
std::atomic<Fault> fault{NONE};
std::atomic<int> queries{0}, copies{0}, writes{0}, replacements{0}, save_errors{0}, start_errors{0};
std::atomic<bool> copy_blocked{false}, allow_copy{false};
// ((Triforce) the workers building while a checkpoint fetches (warm_checkpoint.h): the cache grows by this between the size
// asked and each copy, up to grow_until)
std::atomic<std::size_t> grow_on_copy{0}, grow_until{0};
std::atomic<int> incompletes{0};

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

void logf(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    std::fprintf(stderr, "%s\n", line);
    if (std::strstr(line, "pipeline cache save failed:")) ++save_errors;
    if (std::strstr(line, "could not start pipeline cache save:")) ++start_errors;
}

VkResult vkGetPipelineCacheData(int device, int cache, std::size_t* n, void* data) {
    if (device != dev.device || cache != pcache) std::abort();
    if (!data) {
        ++queries;
        if (fault == QUERY_FAIL) return VK_ERROR_UNKNOWN;
        *n = fault == VECTOR_LENGTH ? (std::numeric_limits<std::size_t>::max)() : fault == EMPTY ? 0 : cache_size.load();
        if (fault == VECTOR_ALLOC) fail_next_new = true;
    } else {
        // (the data as the call found it: taken in by a pipeline built after it began, it need not be in this copy. Read
        // before the copy is counted, which a case waits for before it grows the cache)
        if (grow_on_copy && cache_size < grow_until) cache_size = std::min(cache_size + grow_on_copy, grow_until.load());
        const std::size_t has = cache_size.load();
        ++copies;
        if (fault == COPY_FAIL) return VK_ERROR_UNKNOWN;
        if (fault == BLOCK_COPY) {
            copy_blocked = true;
            while (!allow_copy.load()) Sleep(1);
        }
        // (as the Vulkan rule has it: at most *n written, VK_INCOMPLETE when the data is larger, *n what was written)
        const std::size_t put = std::min(*n, has);
        std::memset(data, 'N', put);
        *n = put;
        if (fault == TMP_ALLOC) fail_next_new = true;
        if (put < has) {
            ++incompletes;
            return VK_INCOMPLETE;
        }
    }
    return VK_SUCCESS;
}

std::size_t save_fwrite(const void* p, std::size_t size, std::size_t n, FILE* f) {
    // The stamp written beside a saved file (<file>.for) is not the cache's data: neither counted nor made to fail.
    if (n == 16 && std::memcmp(p, "1122334455667788", 16) == 0) return std::fwrite(p, size, n, f);
    ++writes;
    if (fault == SHORT_WRITE) return 0;
    return std::fwrite(p, size, n, f);
}
std::atomic<int> write_through{0};
int save_replace(const char* from, const char* to, unsigned flags) {
    if ((flags & ~(unsigned)MOVEFILE_WRITE_THROUGH) != (unsigned)MOVEFILE_REPLACE_EXISTING || to != pcache_path) std::abort();
    if (flags & MOVEFILE_WRITE_THROUGH) ++write_through;
    ++replacements;
    if (fault == REPLACE_FAIL) return 0;
#ifdef _WIN32
    return MoveFileExA(from, to, flags);
#else
    return std::rename(from, to) == 0;
#endif
}

// (Triforce) The cache's gate (vk_draw.cpp pcache_use, pcache_gate.h): closed, the cache was let go for the PC's RAM and is
// neither asked nor written again. And the RAM a copy of the data needs in play (vk_memory.cpp memory_ram_fits)
std::atomic<bool> let_go{false}, ram_fits{true};
std::atomic<int> fit_asks{0};
struct UseStub {
    int cache;
};
UseStub pcache_use() { return {let_go ? 0 : pcache}; }
bool memory_ram_fits(std::uint64_t bytes) {
    ++fit_asks;
    return bytes && ram_fits;
}
// ((Triforce) and the same looked at now, for the preparation's checkpoint saves: vk_memory.cpp memory_ram_fits_now)
bool memory_ram_fits_now(std::uint64_t bytes) {
    ++fit_asks;
    return bytes && ram_fits;
}

// ((Triforce) A checkpoint's (vk_draw.cpp, warm_checkpoint.h): the flag on its thread, a stop asked, the file flushed, the disk's
// room. And whether the cache began from the file, and the stamp the file's .own names (pcache_own_mark)
#include "warm_checkpoint.h"
thread_local bool pcache_checkpoint_now = false;
std::atomic<bool> stop_asked{false}, disk_room{true};
std::atomic<int> flushes{0}, disk_asks{0}, stop_asks{0};
// (a stop asked once this many stop asks were made: the first, before the fetch; the next, between the parts of the write)
std::atomic<int> stop_after{-1};
bool prepare_stopped() {
    const int k = ++stop_asks;
    return stop_asked || (stop_after >= 0 && k > stop_after);
}
bool pcache_file_flush(FILE* f) {
    ++flushes;
    return std::fflush(f) == 0;
}
bool pcache_disk_fits(std::uint64_t) {
    ++disk_asks;
    return disk_room;
}
bool pcache_from_file = false;
std::uint64_t pcache_own_stamp() { return 0xabcdef0123456789ull; }

// (Triforce) The play file (vk_draw.cpp pcache_play_save, pcache_play.h; tests/test_pcache_play.py): no play cache here, so the
// main file is saved as before
std::atomic<int64_t> pcache_main_built{0};
std::atomic<int> pcache_preparing{0};
bool pcache_play_save(int64_t) { return true; }
int pcache_play_cache = 0;
std::atomic<std::size_t> pcache_play_saved{0};
void pcache_main_written(int64_t) {}

// (Triforce) The carry over a stamp change (pcache_carry.h, tests/test_pcache_carry.py): vk_draw.cpp's own part, as it is
// (pcache_carry_past, pcache_carry_save_ok, pcache_carry_check, pcache_carry_held, pcache_carry_early, pcache_carry_saved, cut
// out by tests/test_pipeline_cache_save.py):
// a carried session writes only up to its cap or the bound where lower, past it names the stamp in <file>.nocarry, and each
// save in place writes the history beside the file
#include <mutex>
#include "pcache_carry.h"
#include "pcache_carry_part.inc"

#define fwrite save_fwrite
#define MoveFileExA save_replace
#include "pipeline_cache_save.inc"
#undef MoveFileExA
#undef fwrite
// ((Triforce) the preparation's checkpoint save, vk_draw.cpp's as it is (warm_checkpoint.h), through the save above
#include "pcache_checkpoint_save.inc"

bool wait_until(const std::atomic<int>& n, int target) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (n.load() < target && std::chrono::steady_clock::now() < end) Sleep(1);
    return n.load() >= target;
}
bool old_file() {
    FILE* f = std::fopen(pcache_path.c_str(), "rb");
    if (!f) return false;
    char data[4]{};
    const auto n = std::fread(data, 1, sizeof data, f);
    std::fclose(f);
    return n == 3 && std::memcmp(data, "old", 3) == 0;
}
bool side_file(std::uint64_t base, std::uint32_t carried) {
    FILE* f = std::fopen((pcache_path + ".base").c_str(), "rb");
    if (!f) return false;
    std::uint8_t raw[pcache_carry::kSideBytes]{};
    const auto n = std::fread(raw, 1, sizeof raw, f);
    std::fclose(f);
    pcache_carry::Side s;
    return pcache_carry::get(raw, n, &s) && s.base == base && s.carried == carried && !std::memcmp(s.stamp, "1122334455667788", 16);
}
// The side file's open word (pcache_carry.h Side::open), -1 when there is no side file
int side_open() {
    FILE* f = std::fopen((pcache_path + ".base").c_str(), "rb");
    if (!f) return -1;
    std::uint8_t raw[pcache_carry::kSideBytes]{};
    const auto n = std::fread(raw, 1, sizeof raw, f);
    std::fclose(f);
    pcache_carry::Side s;
    return pcache_carry::get(raw, n, &s) ? (int)s.open : -1;
}
// The 16 digits a side file of the pipeline cache's has ("" when there is none)
std::string sixteen(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return "";
    char data[17]{};
    const auto n = std::fread(data, 1, 16, f);
    std::fclose(f);
    return n == 16 ? std::string(data, 16) : "";
}
bool tmp_left() {
    FILE* f = std::fopen((pcache_path + ".tmp").c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}
bool nocarry_file() {
    FILE* f = std::fopen((pcache_path + ".nocarry").c_str(), "rb");
    if (!f) return false;
    char data[17]{};
    const auto n = std::fread(data, 1, sizeof data, f);
    std::fclose(f);
    return n == 16 && !std::memcmp(data, "1122334455667788", 16);
}
bool new_file() {
    FILE* f = std::fopen(pcache_path.c_str(), "rb");
    if (!f) return false;
    char data[257]{};
    const auto n = std::fread(data, 1, sizeof data, f);
    std::fclose(f);
    if (n != cache_size) return false;
    for (std::size_t i = 0; i < n; ++i) if (data[i] != 'N') return false;
    return true;
}

int run(const std::string& name) {
    if (name == "success") {
        pcache_built_since_save = 31;
        pipeline_cache_save(false);
        CHECK(queries == 0 && old_file());
        pcache_built_since_save = 32;
        pipeline_cache_save(false);
        CHECK(wait_until(copies, 1));
        cache_size = 96;  // Pipelines taken in meanwhile: the forced save has new data to write.
        pipeline_cache_save(true);  // Wait for the worker, then preserve the existing forced-save behavior.
        CHECK(queries == 2 && writes == 2 && replacements == 2 && new_file());
        CHECK(save_errors == 0 && start_errors == 0);
        pcache = 0;
        pipeline_cache_save(true);
        CHECK(queries == 2);
    } else if (name == "force-vector" || name == "async-vector" || name == "force-length" ||
               name == "force-string" || name == "async-string") {
        fault = name == "force-length" ? VECTOR_LENGTH : name.find("string") != std::string::npos ? TMP_ALLOC : VECTOR_ALLOC;
        const bool async = name.starts_with("async");
        pipeline_cache_save(!async);
        CHECK(wait_until(save_errors, 1));
        CHECK(old_file() && replacements == 0 && start_errors == 0);
        CHECK(allocation_failures == (fault == VECTOR_LENGTH ? 0 : 1));
        fault = NONE;
        pipeline_cache_save(true);
        CHECK(new_file() && replacements == 1 && queries == 2);  // No stuck busy flag after the failure.
    } else if (name == "thread-start") {
        fail_next_new = true;
        pipeline_cache_save(false);  // The real std::thread's state allocation, before it can run the worker.
        CHECK(!fail_next_new && allocation_failures == 1 && start_errors == 1 && save_errors == 0);
        CHECK(queries == 0 && old_file());
        pipeline_cache_save(true);
        CHECK(new_file() && queries == 1 && replacements == 1);
    } else if (name == "api-results") {
        for (Fault f : {QUERY_FAIL, EMPTY, COPY_FAIL}) {
            fault = f;
            pipeline_cache_save(true);
            CHECK(old_file() && replacements == 0 && save_errors == 0 && start_errors == 0);
        }
        fault = NONE;
        pipeline_cache_save(true);
        CHECK(new_file() && replacements == 1);
    } else if (name == "short-write") {
        fault = SHORT_WRITE;
        pipeline_cache_save(true);
        CHECK(writes == 1 && replacements == 0 && old_file());
        fault = NONE;
        pipeline_cache_save(true);
        CHECK(new_file() && replacements == 1);
    } else if (name == "forced-wait") {
        fault = BLOCK_COPY;
        pipeline_cache_save(false);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!copy_blocked.load() && std::chrono::steady_clock::now() < end) Sleep(1);
        CHECK(copy_blocked);
        std::atomic<bool> started{false}, finished{false};
        std::thread waiter([&] { started = true; pipeline_cache_save(true); finished = true; });
        while (!started.load()) Sleep(1);
        Sleep(20);
        const bool waited = !finished.load() && queries == 1;
        cache_size = 96;  // Taken in while the first save copied: the waiter has new data to write.
        allow_copy = true;
        waiter.join();
        CHECK(waited && finished && queries == 2 && replacements == 2 && new_file());
    } else if (name == "unchanged") {
        pcache_saved_size = 64;  // As pcache_init leaves it: the driver's size for the data read from the file.
        pipeline_cache_save(true);  // At exit, nothing taken in.
        CHECK(queries == 1 && copies == 0 && writes == 0 && old_file());
        pcache_built_since_save = 32;  // In play: the precompile's pipelines, all found in the cache, still count.
        pipeline_cache_save(false);
        CHECK(wait_until(queries, 2));
        pipeline_cache_save(true);  // After the worker.
        CHECK(queries == 3 && copies == 0 && writes == 0 && replacements == 0 && old_file());
        cache_size = 96;
        for (Fault f : {SHORT_WRITE, REPLACE_FAIL}) {  // Not in place: tried again at the next save.
            fault = f;
            pipeline_cache_save(true);
            CHECK(old_file() && pcache_saved_size == 64);
        }
        fault = NONE;
        pipeline_cache_save(true);
        CHECK(queries == 6 && copies == 3 && writes == 3 && replacements == 2 && new_file() && pcache_saved_size == 96);
        pipeline_cache_save(true);  // Written: nothing new since.
        CHECK(queries == 7 && copies == 3 && writes == 3 && replacements == 2 && new_file());
        cache_size = 80;  // A different size of any kind is data the file lacks.
        pipeline_cache_save(true);
        CHECK(copies == 4 && replacements == 3 && new_file());
        CHECK(save_errors == 0 && start_errors == 0);
    } else if (name == "over-bound") {
        pcache_bound_bytes = 80;  // The data is 64 bytes: under the bound, written as ever.
        pipeline_cache_save(true);
        CHECK(new_file() && replacements == 1 && pcache_saved_size == 64);
        cache_size = 80;  // At the bound: still written.
        pipeline_cache_save(true);
        CHECK(new_file() && replacements == 2 && pcache_saved_size == 80);
        cache_size = 96;  // Over it: its size is asked, nothing is fetched or written, and the file keeps its last save.
        pipeline_cache_save(true);
        CHECK(queries == 3 && copies == 2 && writes == 2 && replacements == 2 && pcache_saved_size == 80);
        cache_size = 80;
        CHECK(new_file());
        cache_size = 96;
        pcache_built_since_save = 32;  // In play too, on the worker.
        pipeline_cache_save(false);
        CHECK(wait_until(queries, 4));
        pipeline_cache_save(true);  // After the worker.
        CHECK(queries == 5 && copies == 2 && writes == 2 && replacements == 2 && pcache_saved_size == 80);
        pcache_bound_bytes = 0;  // The bound lifted: the same data is written.
        pipeline_cache_save(true);
        CHECK(new_file() && copies == 3 && replacements == 3 && pcache_saved_size == 96);
        CHECK(save_errors == 0 && start_errors == 0);
    } else if (name == "carry-cap") {
        // (Triforce) Carried over a stamp change with a base of 64 bytes: written up to 80 (1.25 x), its history with it.
        pcache_carried = true;
        pcache_side.base = 64, pcache_side.carried = 1;
        pipeline_cache_save(true);
        CHECK(new_file() && replacements == 1 && side_file(64, 1) && pcache_side.base == 64 && pcache_side.carried == 1);
        CHECK(std::memcmp(pcache_side.stamp, "1122334455667788", 16) == 0);
        cache_size = 80;  // At the cap: still written, the base kept (a carried file's growth is not a new base).
        pipeline_cache_save(true);
        CHECK(new_file() && replacements == 2 && side_file(64, 1) && pcache_saved_size == 80);
        CHECK(!pcache_carry_over && !nocarry_file());
        cache_size = 96;  // Past it: the size asked, nothing fetched or written, the file keeps its last save.
        pipeline_cache_save(true);
        CHECK(queries == 3 && copies == 2 && writes == 2 && replacements == 2 && pcache_saved_size == 80);
        CHECK(pcache_carry_over && nocarry_file());  // Over: the rebuild is due, and a preparation begun again starts fresh.
        cache_size = 80;
        CHECK(new_file() && side_file(64, 1));
        cache_size = 96;  // Asked again: refused again.
        pipeline_cache_save(true);
        CHECK(copies == 2 && replacements == 2);
        // A fresh cache in its place (pcache_carry_rebuild): no carry, its saves written and the base follows them.
        pcache_carried = false;
        pcache_carry_over = false;
        pcache_side = pcache_carry::session(pcache_carry::Start::Fresh, pcache_side);
        pipeline_cache_save(true);
        CHECK(new_file() && replacements == 3 && pcache_side.base == 96 && pcache_side.carried == 0 && side_file(96, 0));
        CHECK(!pcache_carry_over && save_errors == 0 && start_errors == 0);
    } else if (name == "carry-bound") {
        // (Triforce) Carried with a base of 64 bytes (cap 80) and a bound of 72: past the bound, under the cap, is past what it
        // may hold (never left over the bound unwritten with no rebuild), and the bound's own refusal is not reached.
        pcache_carried = true;
        pcache_side.base = 64, pcache_side.carried = 1;
        pcache_bound_bytes = 72;
        cache_size = 72;  // At the bound: written.
        pipeline_cache_save(true);
        CHECK(new_file() && replacements == 1 && !pcache_carry_over && !nocarry_file());
        cache_size = 76;  // Over the bound, under the cap.
        pipeline_cache_save(true);
        CHECK(copies == 1 && replacements == 1 && pcache_carry_over && nocarry_file() && pcache_saved_size == 72);
        cache_size = 72;
        CHECK(new_file());
        // Not carried, the same size over the bound: the bound's rule as before, no rebuild asked.
        pcache_carried = false;
        pcache_carry_over = false;
        cache_size = 76;
        pipeline_cache_save(true);
        CHECK(copies == 1 && replacements == 1 && !pcache_carry_over);
        CHECK(save_errors == 0 && start_errors == 0);
    } else if (name == "carry-done") {
        // (Triforce) A carried session's history is open (pcache_carry.h Side::open) until its preparation goes through to
        // its end (pcache_carry_close): a stopped one's save is read as carried by the preparation begun again.
        pcache_carried = true;
        pcache_side.base = 64;
        std::memcpy(pcache_side.stamp, "aaaaaaaaaaaaaaaa", 16);   // (the old stamp's file)
        pcache_side = pcache_carry::session(pcache_carry::Start::Carried, pcache_side);
        CHECK(pcache_side.open == 1 && pcache_side.carried == 1);
        pcache_carry_close();   // Nothing saved under these shaders yet: no side file written, the next save writes it.
        CHECK(side_open() == -1 && pcache_side.open == 0);
        pcache_side.open = 1;
        pipeline_cache_save(true);   // Saved (a stop's last save): these shaders' stamp, the history open.
        CHECK(new_file() && side_file(64, 1) && side_open() == 1);
        pcache_carry_close();   // Gone through: no longer open, beside the file at once.
        CHECK(side_file(64, 1) && side_open() == 0 && pcache_side.open == 0 && replacements == 1);
        pcache_side.open = 1;
        cache_size = 80;
        pipeline_cache_save(true);
        CHECK(replacements == 2 && side_open() == 1);
        cache_size = 96;   // Past what it may hold: not written, and not done: the file's history stays open.
        pipeline_cache_save(true);
        CHECK(replacements == 2 && pcache_carry_over && nocarry_file());
        pcache_carry_close();
        CHECK(side_open() == 1 && pcache_side.open == 1);
        // Not carried (a fresh cache, pcache_carry_rebuild): nothing to do.
        pcache_carried = false;
        pcache_carry_over = false;
        pcache_side = pcache_carry::session(pcache_carry::Start::Fresh, pcache_side);
        pipeline_cache_save(true);
        CHECK(replacements == 3 && side_file(96, 0) && side_open() == 0);
        pcache_carry_close();
        CHECK(side_open() == 0 && save_errors == 0 && start_errors == 0);
    } else if (name == "checkpoint") {
        // (Triforce) A checkpoint mid-warm (warm_checkpoint.h) in a session that carried the file over a stamp change (base
        // 64, cap 80): saved under these shaders' stamp with the history open, as a stopped run's save. The start after a
        // crash there reads it as carried still (pcache_carry::resumed), capped as this run was, and past the cap rebuilds.
        pcache_carried = true;
        pcache_side.base = 64;
        std::memcpy(pcache_side.stamp, "aaaaaaaaaaaaaaaa", 16);   // (the old stamp's file)
        pcache_side = pcache_carry::session(pcache_carry::Start::Carried, pcache_side);
        cache_size = 72;
        ram_fits = false;   // The RAM short: nothing fetched, nothing written, the old file kept.
        CHECK(pcache_checkpoint_save() == 0 && fit_asks == 1 && copies == 0 && replacements == 0 && old_file());
        ram_fits = true;
        CHECK(pcache_checkpoint_save() == 72 && copies == 1 && replacements == 1 && new_file());
        CHECK(side_file(64, 1) && side_open() == 1 && !pcache_carry_over);
        {   // (the .for names these shaders now: the next start finds the file the same stamp's)
            FILE* f = std::fopen((pcache_path + ".for").c_str(), "rb");
            CHECK(f);
            char had[17]{};
            const auto got = std::fread(had, 1, 16, f);
            std::fclose(f);
            CHECK(got == 16 && !std::memcmp(had, "1122334455667788", 16));
        }
        CHECK(pcache_checkpoint_save() == 0 && copies == 1 && replacements == 1);   // Nothing new: nothing written.
        {   // The start after a crash here, as pcache_init reads the files: the same stamp, its history open, an update's
            // preparation: carried still, no carry counted again, capped at 80 as this run was.
            FILE* f = std::fopen((pcache_path + ".base").c_str(), "rb");
            CHECK(f);
            std::uint8_t raw[pcache_carry::kSideBytes]{};
            const auto got = std::fread(raw, 1, sizeof raw, f);
            std::fclose(f);
            pcache_carry::Side had;
            CHECK(pcache_carry::get(raw, got, &had));
            const pcache_carry::Side history = pcache_carry::history(true, had, "1122334455667788", 72);
            CHECK(pcache_carry::resumed(true, true, true, history));
            const pcache_carry::Side again = pcache_carry::session(pcache_carry::Start::Same, history);
            CHECK(again.open == 1 && again.carried == 1 && again.base == 64);
            CHECK(!pcache_carry::over(true, 80, again, 0) && pcache_carry::over(true, 81, again, 0));
        }
        cache_size = 96;   // Past the cap at a checkpoint: not written, the rebuild asked, the file as the last checkpoint left it.
        CHECK(pcache_checkpoint_save() == 0 && copies == 1 && replacements == 1 && pcache_carry_over && nocarry_file());
        cache_size = 72;
        CHECK(new_file() && side_open() == 1);
        // Not carried (a fresh cache): a checkpoint writes it, its history not open.
        pcache_carried = false;
        pcache_carry_over = false;
        pcache_side = pcache_carry::session(pcache_carry::Start::Fresh, pcache_side);
        cache_size = 88;
        CHECK(pcache_checkpoint_save() == 88 && replacements == 2 && side_file(88, 0) && side_open() == 0);
        let_go = true;   // The cache let go: not asked.
        const int asked = queries.load();
        CHECK(pcache_checkpoint_save() == 0 && queries == asked && replacements == 2);
        CHECK(save_errors == 0 && start_errors == 0);
    } else if (name == "checkpoint-grow") {
        // (Triforce) A checkpoint while the workers build (warm_checkpoint.h): the cache grows between the size asked and the
        // copy. A save of the other kind (the size asked, no room) gets VK_INCOMPLETE and writes nothing, as before; a
        // checkpoint fetches with room, writes what the driver gave, on the disk before it takes the old file's place.
        grow_on_copy = 8, grow_until = 1u << 30;
        pipeline_cache_save(true);
        CHECK(incompletes == 1 && copies == 1 && replacements == 0 && old_file() && !tmp_left());
        CHECK(pcache_checkpoint_save() == 80 && incompletes == 1 && copies == 2 && replacements == 1 && new_file());
        CHECK(flushes == 1 && write_through == 1 && disk_asks == 1 && pcache_saved_size == 80);
        // The other saves: no flush, no write-through, the disk not asked (nothing changed for them).
        grow_on_copy = 0;
        cache_size = 96;
        pipeline_cache_save(true);
        CHECK(replacements == 2 && new_file() && flushes == 1 && write_through == 1 && disk_asks == 1);
        // Carried (base 64, cap 80): asked at 76, fetched at 84, past the cap. Looked at again on what was fetched: not written,
        // the rebuild asked.
        pcache_carried = true;
        pcache_side.base = 64, pcache_side.carried = 1;
        cache_size = 76, grow_on_copy = 8;
        CHECK(pcache_checkpoint_save() == 0 && copies == 4 && replacements == 2 && pcache_carry_over && nocarry_file());
        pcache_carried = false;
        pcache_carry_over = false;
        // The bound (88): asked at 84, fetched at 92, over it: not written.
        pcache_bound_bytes = 88;
        cache_size = 84;
        CHECK(pcache_checkpoint_save() == 0 && copies == 5 && replacements == 2);
        pcache_bound_bytes = 0;
        // Growing past the room at every try: kFetchTries copies, the size asked again before each, nothing written.
        grow_on_copy = warm_checkpoint::kRoomMin + 8;
        const int had = copies.load(), asked = queries.load();
        CHECK(pcache_checkpoint_save() == 0 && copies == had + warm_checkpoint::kFetchTries && replacements == 2);
        CHECK(queries == asked + 1 + warm_checkpoint::kFetchTries && incompletes == 1 + warm_checkpoint::kFetchTries);
        CHECK(save_errors == 0 && start_errors == 0);
    } else if (name == "checkpoint-stop") {
        // (Triforce) A stop asked (warm_checkpoint.h): a checkpoint gives up, before the fetch or in the write, and leaves no
        // .tmp; the stop's own save (not a checkpoint) writes the cache as ever.
        stop_asked = true;
        CHECK(pcache_checkpoint_save() == 0 && copies == 0 && replacements == 0 && old_file());
        stop_asked = false;
        stop_after = stop_asks.load() + 1;   // (the look before the fetch says not stopped, the one in the write says stopped)
        CHECK(pcache_checkpoint_save() == 0 && copies == 1 && writes == 0 && replacements == 0 && old_file() && !tmp_left());
        stop_after = -1;
        stop_asked = true;
        pipeline_cache_save(true);
        CHECK(replacements == 1 && new_file() && flushes == 0);
        stop_asked = false;
        cache_size = 72;
        CHECK(pcache_checkpoint_save() == 72 && replacements == 2 && new_file());
        // The disk short of room for the .tmp beside the old file: not fetched, asked again later.
        disk_room = false;
        cache_size = 80;
        CHECK(pcache_checkpoint_save() == 0 && copies == 3 && replacements == 2);
        disk_room = true;
        CHECK(pcache_checkpoint_save() == 80 && replacements == 3 && new_file());
        // A write that fails leaves no .tmp to fill the disk.
        cache_size = 88;
        fault = SHORT_WRITE;
        CHECK(pcache_checkpoint_save() == 0 && replacements == 3 && !tmp_left());
        fault = NONE;
        CHECK(save_errors == 0 && start_errors == 0);
    } else if (name == "carry-begun") {
        // (Triforce) pcache_carry.h begun: a carried session's save writes its history (these shaders' stamp, open) before the
        // data takes the old file's place, and the .for after. Cut between them (here: the data not put in place), the start
        // after reads the session's own history, capped, not one fresh set of the file's size, uncapped.
        CHECK(file_writers::write_whole(pcache_path + ".for", "aaaaaaaaaaaaaaaa", 16));
        pcache_carried = true;
        pcache_side.base = 64;
        std::memcpy(pcache_side.stamp, "aaaaaaaaaaaaaaaa", 16);
        pcache_side = pcache_carry::session(pcache_carry::Start::Carried, pcache_side);
        cache_size = 72;
        fault = REPLACE_FAIL;
        CHECK(pcache_checkpoint_save() == 0 && replacements == 1 && old_file() && !tmp_left());
        CHECK(side_file(64, 1) && side_open() == 1 && sixteen(pcache_path + ".for") == "aaaaaaaaaaaaaaaa");
        {
            FILE* f = std::fopen((pcache_path + ".base").c_str(), "rb");
            CHECK(f);
            std::uint8_t raw[pcache_carry::kSideBytes]{};
            const auto got = std::fread(raw, 1, sizeof raw, f);
            std::fclose(f);
            pcache_carry::Side had;
            CHECK(pcache_carry::get(raw, got, &had));
            CHECK(pcache_carry::begun(true, had, "1122334455667788", "aaaaaaaaaaaaaaaa"));
            CHECK(pcache_carry::resumed(true, true, true, had));
            const pcache_carry::Side again = pcache_carry::session(pcache_carry::Start::Same, had);
            CHECK(again.open == 1 && again.carried == 1 && again.base == 64 && pcache_carry::over(true, 81, again, 0));
            // (by the history's own rule the side file is not that file's: one fresh set of its size, never capped)
            CHECK(pcache_carry::history(true, had, "aaaaaaaaaaaaaaaa", 3).open == 0);
        }
        // The history cannot be written: the data is not put in place, the old file and its .for stay.
        fault = NONE;
        std::remove((pcache_path + ".base").c_str());
#ifdef _WIN32
        CHECK(CreateDirectoryA((pcache_path + ".base").c_str(), nullptr));
#else
        CHECK(mkdir((pcache_path + ".base").c_str(), 0700) == 0);
#endif
        CHECK(pcache_checkpoint_save() == 0 && replacements == 1 && old_file() && !tmp_left());
        CHECK(sixteen(pcache_path + ".for") == "aaaaaaaaaaaaaaaa");
#ifdef _WIN32
        CHECK(RemoveDirectoryA((pcache_path + ".base").c_str()));
#else
        CHECK(rmdir((pcache_path + ".base").c_str()) == 0);
#endif
        CHECK(pcache_checkpoint_save() == 72 && replacements == 2 && new_file() && side_file(64, 1) && side_open() == 1);
        CHECK(sixteen(pcache_path + ".for") == "1122334455667788");
        // Not carried: no history written before the data (as before), a cut save leaves none.
        pcache_carried = false;
        pcache_side = pcache_carry::session(pcache_carry::Start::Fresh, pcache_side);
        std::remove((pcache_path + ".base").c_str());
        cache_size = 80;
        fault = REPLACE_FAIL;
        CHECK(pcache_checkpoint_save() == 0 && replacements == 3 && side_open() == -1);
        fault = NONE;
        CHECK(save_errors == 0 && start_errors == 0);
    } else if (name == "own") {
        // (Triforce) <file>.own (pcache_own_mark): written beside the saves of a session that began with no file or with such a
        // file, removed by those of a session that began with another file's data or carried one.
        const std::string own = pcache_path + ".own";
        pipeline_cache_save(true);
        CHECK(new_file() && sixteen(own) == "abcdef0123456789" && pcache_own_named());   // (read so at the next start)
        pcache_from_file = true;   // (a file not of that kind: an older build's, after an update that kept the stamp)
        cache_size = 72;
        pipeline_cache_save(true);
        CHECK(new_file() && sixteen(own).empty() && !pcache_own_named());
        pcache_from_own = true;   // (one of that kind: a checkpoint's before a crash)
        cache_size = 80;
        CHECK(pcache_checkpoint_save() == 80 && sixteen(own) == "abcdef0123456789");
        pcache_carried = true;   // (carried over a stamp change)
        pcache_side.base = 80;
        cache_size = 88;
        pipeline_cache_save(true);
        CHECK(new_file() && sixteen(own).empty() && replacements == 4);
        CHECK(save_errors == 0 && start_errors == 0);
    } else if (name == "carry-check") {
        // (Triforce) The preparation's look at the size (prepare_run, warm_libraries): not carried, the driver is not asked.
        CHECK(!pcache_carry_check(false) && !pcache_carry_check(true) && queries == 0);
        pcache_carried = true;
        pcache_side.base = 64;
        pcache_bound_bytes = 0;
        cache_size = 80;  // At the cap: not past.
        CHECK(!pcache_carry_check(true) && queries == 1 && !pcache_carry_over);
        cache_size = 96;
        dev.props.vendorID = 0x8086;  // Intel: not asked while workers build into the cache, asked once they are joined.
        CHECK(!pcache_carry_check(true) && queries == 1);
        CHECK(pcache_carry_check(false) && queries == 2 && pcache_carry_over && nocarry_file());
        CHECK(pcache_carry_check(true) && queries == 2);  // Once past, it stays so without asking.
        pcache_carry_over = false;
        dev.props.vendorID = 0x10DE;
        CHECK(pcache_carry_check(true) && queries == 3);
        pcache_carry_over = false;
        let_go = true;  // The cache let go: nothing asked.
        CHECK(!pcache_carry_check(false) && queries == 3);
        CHECK(copies == 0 && writes == 0 && old_file());
    } else if (name == "carry-early") {
        // (Triforce) The early drop (warm_libraries, pcache_carry_early): not carried, nothing asked and nothing projected.
        std::size_t from = 1;
        CHECK(!pcache_carry_held(&from) && from == 0 && !pcache_carry_early(64, 100, 1000, 1500) && queries == 0);
        pcache_carried = true;
        pcache_side.base = 64;   // (its limit 80: the room from 64 is 16, the margin 32 of projected growth)
        pcache_bound_bytes = 0;
        CHECK(pcache_carry_held(&from) && from == 64 && queries == 1);
        cache_size = 68;   // 4 bytes for a sample of 100 of 1000 programs: 40 over the warm at this rate, past the margin.
        CHECK(!pcache_carry_early(from, 100, 1000, 1499));
        CHECK(queries == 2 && !pcache_carry_over && !nocarry_file());   // (the sample too small: asked, not dropped)
        cache_size = 65;   // 1 byte for 100 of 1000: 10, under the margin.
        CHECK(!pcache_carry_early(from, 100, 1000, 1500) && queries == 3 && !pcache_carry_over);
        cache_size = 96;   // Past already: left to pcache_carry_check, which says so.
        CHECK(!pcache_carry_early(from, 100, 1000, 1500) && queries == 4 && !pcache_carry_over);
        cache_size = 68;
        dev.props.vendorID = 0x8086;   // Intel: not asked while workers build, and no projection made (the warm's own order).
        from = 1;
        CHECK(!pcache_carry_held(&from) && from == 0 && !pcache_carry_early(64, 100, 1000, 1500) && queries == 4);
        dev.props.vendorID = 0x10DE;
#ifdef _WIN32
        _putenv("RX_VK_PCACHE_CARRY_PROJECT=0");
#else
        setenv("RX_VK_PCACHE_CARRY_PROJECT", "0", 1);
#endif
        // Off: past alone, and no projection made (the warm's own order).
        CHECK(!pcache_carry_held(&from) && from == 0 && queries == 4);
        CHECK(!pcache_carry_early(64, 100, 1000, 1500) && queries == 4 && !pcache_carry_over);
#ifdef _WIN32
        _putenv("RX_VK_PCACHE_CARRY_PROJECT=");
#else
        unsetenv("RX_VK_PCACHE_CARRY_PROJECT");
#endif
        CHECK(pcache_carry_held(&from) && from == 68 && queries == 5);
        from = 64;
        // Dropped: as past what it may hold, the carry refused for these shaders, and the look past it says so without asking.
        CHECK(pcache_carry_early(from, 100, 1000, 1500) && queries == 6 && pcache_carry_over && nocarry_file());
        CHECK(pcache_carry_check(true) && queries == 6 && !pcache_carry_early(from, 100, 1000, 1500) && queries == 6);
        // Not written: a carried cache dropped is not saved, and the file keeps what it was.
        pipeline_cache_save(true);
        CHECK(copies == 0 && writes == 0 && replacements == 0 && old_file());
    } else if (name == "ram-short") {
        ram_fits = false;  // In play, a copy of the data would not fit in the RAM available: it waits, nothing fetched.
        pcache_built_since_save = 32;
        pipeline_cache_save(false);
        CHECK(wait_until(fit_asks, 1));
        Sleep(20);
        CHECK(copies == 0 && old_file() && pcache_built_since_save >= 32);  // Asked again at the next check in play.
        pipeline_cache_save(true);  // After the worker; at the game's close the RAM is not asked, the save is made.
        CHECK(fit_asks == 1 && queries == 2 && copies == 1 && replacements == 1 && new_file());
        pcache_built_since_save = 32;
        ram_fits = true;
        cache_size = 96;
        pipeline_cache_save(false);
        CHECK(wait_until(copies, 2));
        pipeline_cache_save(true);
        CHECK(fit_asks == 2 && replacements == 2 && new_file() && save_errors == 0);
    } else if (name == "let-go") {
        let_go = true;  // The cache let go: no save asks the driver anything, the file keeps its last save.
        pcache_built_since_save = 32;
        pipeline_cache_save(false);
        pipeline_cache_save(true);
        Sleep(50);
        CHECK(queries == 0 && copies == 0 && writes == 0 && replacements == 0 && old_file() && save_errors == 0);
    } else {
        CHECK(false);
    }
    std::printf("PASS: %s\n", name.c_str());
    return 0;
}

int main(int argc, char** argv) {
    // An old detached worker would terminate the process. The original-source negative runs in its own bounded child.
    std::set_terminate([] { std::fputs("uncaught worker exception\n", stderr); std::_Exit(86); });
    if (argc == 2 && std::strcmp(argv[1], "terminate-control") == 0) std::terminate();
    if (argc == 2 && std::strcmp(argv[1], "uncaught-thread-control") == 0) {
#ifdef _MSC_VER
        std::fputs("MSVC-target uncaught-thread control\n", stderr);
#endif
        std::thread worker([] { throw std::bad_alloc(); });
        worker.join();
        return 2;
    }
    if (argc != 3) return 2;
    pcache_path = std::string(argv[2]) + "/authored-pipeline-cache-with-a-path-longer-than-small-string-storage.bin";
    FILE* f = std::fopen(pcache_path.c_str(), "wb");
    if (!f) return 2;
    std::fwrite("old", 1, 3, f);
    std::fclose(f);
    try {
        return run(argv[1]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uncaught caller exception: %s\n", e.what());
        return 87;
    }
}
