/* (Triforce) The pages of a range read before the driver imports it (vk_memory.cpp import_pool, RX_VK_IMPORT_TOUCH).
   An import of the game's memory (VK_EXT_external_memory_host) is the driver locking the range's pages inside
   vkAllocateMemory, and while it does a vkQueueSubmit2 from another thread waits for it. The ranges imported ahead are
   whole, up to 1 GB, and much of one is pages nobody has touched since they were committed: the driver faults each in under
   its lock. In the lab's Market stands (RTX 5090, rounds 74 and 77) the allocate of the ranges of 288 to 752 MB took 16 to
   93 ms and the frame's queue submit stood beside it for as long: the 26 to 64 ms hitches of the owner's walks. With each
   page read first, on the importing thread and outside the driver, the same allocates took 4 to 13 ms, and the longest
   queue submit beside an import in play 10 to 12 ms for 71 to 93.
   Only on the background import thread (the imports another thread's submit stands beside: an import on a binding's own
   thread gains nothing and the read would add to it), only on Windows, and only where the range was committed whole. There
   the reads add no memory: the import holds every page of its range in RAM as it is. They are not free in time: a stand's
   imports of 64 MB or more took 581 and 614 ms with the read (392 and 420 ms reading, 168 and 172 allocating) against 405 ms
   without, on the background thread; 1,212 ms in the first run after a build.
   Plain functions: tests/test_import_touch.py checks them.
   RX_VK_IMPORT_TOUCH=0: off, as before. */
#pragma once

#include <cstddef>
#include <cstdint>

namespace mkwvk::import_touch {

constexpr size_t kPage = 4096;
constexpr size_t kRun = 4u << 20;   // (bytes read in one guarded call: a run that faults is left to the driver, the next goes on)

constexpr uint64_t kLargeMb = 64;   // (imports of this size or more: their own count of log lines, vk_memory.cpp import_pool's report)

/// RX_VK_IMPORT_TOUCH: on unless it is 0
inline bool on(const char* env) { return !(env && env[0] == '0' && !env[1]); }

/// Whether an import's pages are read first: the switch, on the background import thread, the range committed whole
inline bool may(bool on, bool background_thread, bool committed_whole) { return on && background_thread && committed_whole; }

/// The length of the run at off in a range of bytes: kRun, or what is left
inline size_t run(uint64_t bytes, uint64_t off) { return off >= bytes ? 0 : (size_t)(bytes - off < kRun ? bytes - off : kRun); }

/// One byte of each page of [p, p + n) read, p on a page's start: the pages read. Not inlined: the caller's handler takes a
/// fault only across a call (vk_memory.cpp guarded_copy has why)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
inline uint64_t read_pages(const volatile uint8_t* p, size_t n) {
    uint64_t pages = 0;
    unsigned sink = 0;
    for (size_t i = 0; i < n; i += kPage) sink += p[i], pages++;
    (void)sink;
    return pages;
}

} // namespace mkwvk::import_touch
