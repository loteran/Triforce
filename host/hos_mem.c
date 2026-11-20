/* Guest memory map: the system region (our own objects), the heap, address regions.
   The whole 4 GB window is committed up front (hos_main.c), so "mapping" here is only bookkeeping and zeroing. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&lock)
#define UNLOCK() ReleaseSRWLockExclusive(&lock)
#else
#include <pthread.h>
#include <sys/mman.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)
#endif

uint64_t hos_heap_size, hos_heap_used, hos_total_memory, hos_program_size;
static uint64_t sys_next = HOS_SYSTEM_BASE;

/* (Triforce) the guest memory census's map (guest_census.h; the notes, the counts and the give-back's decommit are at this
   file's end). Its chunks come from the system, not the heap: the first touch's exception handler may make one */
static void *census_alloc(size_t n) {
#if defined(_WIN32)
    return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}
static void census_free(void *p, size_t n) {
#if defined(_WIN32)
    (void)n;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, n);
#endif
}
#define GC_ALLOC(n) census_alloc(n)
#define GC_FREE(p) census_free((p), sizeof(GcChunk))
#include "guest_census.h"
static GcMap census;

/* Address regions are handed out top-down from HOS_SPACE_END; the heap grows up from HOS_HEAP_BASE. (Triforce) freeing: a
   region being freed, listed until its pages are discarded (hos_region_free) */
typedef struct { uint64_t addr, size; int freeing; } Range;
static Range *regions;
static int nregions, capregions;
static uint64_t region_floor = ~0ULL;   /* the lowest region (none yet: the window's end) */

/* Free address space below the heap that regions may also use (web: the window is tight): above the null guard
   to the system region, and from the image's end to the heap. */
static uint64_t low_hole[2][2];

void hos_mem_init(uint64_t image_end) {
    hos_program_size = image_end - hos_image_base;
    hos_commit(HOS_SYSTEM_BASE, HOS_SYSTEM_END - HOS_SYSTEM_BASE);
    hos_mem_census_start();   /* (Triforce: the census line and the give-back, mkw_glue.c) */
    low_hole[0][0] = 0x00100000ULL, low_hole[0][1] = HOS_SYSTEM_BASE;
    low_hole[1][0] = (image_end + 0xFFFFFULL) & ~0xFFFFFULL, low_hole[1][1] = HOS_HEAP_BASE;
    /* Retail Switch gives an application 3285 MB. HOS_MEMORY_MB overrides it: TOTK sizes its heap from this
       (heap = total - used - 1080 MB), which is the lever for fitting the web build into wasm32. */
    const char *mb = getenv("HOS_MEMORY_MB");
#ifdef A64_MEM_ZERO
    /* (web: TOTK keeps 1080 MB of the total for address regions (its resource memory grows into them) and its heap
       must stay 2104 MB (a 1902 MB heap: an indirect call into nothing 4 s in), so the window must hold heap +
       regions; the holes below the heap help) */
    hos_total_memory = (mb ? strtoull(mb, NULL, 0) : 3285ULL) << 20;
    hos_log("memory: %llu MB for the game (window %llu MB)", (unsigned long long)(hos_total_memory >> 20),
            (unsigned long long)(HOS_SPACE_END >> 20));
#else
    /* mkw-recomp: a Switch 2 application gets 9809 MB (Eglantine's KSystemControl, 12 GB arrangement) */
    hos_total_memory = (mb ? strtoull(mb, NULL, 0) : 9809ULL) << 20;
#endif
}

uint64_t hos_region_floor(void) { return region_floor; }

uint64_t hos_sys_alloc(uint64_t size, uint64_t align) {
    LOCK();
    uint64_t a = (sys_next + align - 1) & ~(align - 1);
    if (a + size > HOS_SYSTEM_END) {
        UNLOCK();
        hos_fatal(NULL, "system region exhausted (%#llx bytes asked)", (unsigned long long)size);
    }
    sys_next = a + size;
    UNLOCK();
    memset(GP(a), 0, size);
    HOS_WRITTEN(GP(a), size);
    return a;
}

uint64_t hos_region_alloc(uint64_t size, uint64_t align) {
    if (align < 0x1000) align = 0x1000;
    size = (size + 0xFFF) & ~0xFFFULL;
    LOCK();
    /* first fit, top-down, between existing regions (kept sorted by address, descending) */
    uint64_t top = HOS_SPACE_END, found = 0;
    int at = nregions;
    for (int i = 0; i <= nregions; i++) {
        uint64_t bottom = i < nregions ? regions[i].addr + regions[i].size : HOS_HEAP_BASE + hos_heap_size;
        if (top >= size) {
            uint64_t a = (top - size) & ~(align - 1);
            if (a >= bottom && a >= HOS_HEAP_BASE + hos_heap_size) { found = a; at = i; break; }
        }
        if (i < nregions) top = regions[i].addr;
    }
    if (!found)   /* the holes below the heap, top-down, between the regions there (the list is descending) */
        for (int h = 1; h >= 0 && !found; h--) {
            uint64_t hi = low_hole[h][1];
            int i = 0;
            while (i < nregions && regions[i].addr >= hi) i++;
            for (;; i++) {
                const uint64_t lo = i < nregions && regions[i].addr + regions[i].size > low_hole[h][0]
                                        ? regions[i].addr + regions[i].size : low_hole[h][0];
                if (hi >= size) {
                    const uint64_t a = (hi - size) & ~(align - 1);
                    if (a >= lo) { found = a; at = i; break; }
                }
                if (i >= nregions || regions[i].addr < low_hole[h][0]) break;
                hi = regions[i].addr;
            }
        }
    if (found) {
        if (nregions == capregions) {
            capregions = capregions ? capregions * 2 : 64;
            regions = (Range *)realloc(regions, capregions * sizeof(Range));
        }
        memmove(&regions[at + 1], &regions[at], (nregions - at) * sizeof(Range));
        regions[at].addr = found;
        regions[at].size = size;
        regions[at].freeing = 0;
        nregions++;
        if (found < region_floor && found >= HOS_HEAP_BASE) region_floor = found;   /* (not the holes below) */
    }
    UNLOCK();
    if (found) hos_commit(found, size);
    if (!found) hos_fatal(NULL, "address space exhausted (%#llx bytes asked, heap %#llx)", (unsigned long long)size,
                          (unsigned long long)hos_heap_size);
    return found;
}

static uint64_t pages_used[(1ULL << 32) / 0x1000 / 64];

static void pages_mark_used_locked(uint64_t addr, uint64_t size) {
    for (uint64_t p = addr >> 12; p < (addr + size + 0xFFF) >> 12 && p < (1ULL << 20); p++) pages_used[p >> 6] |= 1ULL << (p & 63);
}

static void discard_pages(uint64_t addr, uint64_t size) {
    if (!hos_guest_range(addr, size)) return;   /* (hookshot: guest pages only; Linux's discard zeroes what it is given) */
    const uint64_t lo = (addr + 0xFFF) & ~0xFFFULL, hi = (addr + size) & ~0xFFFULL;
    static int keep = -1;
    if (keep < 0) keep = getenv("HOS_KEEP_FREED_PAGES") != NULL;
    if (hi <= lo || keep) return;
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    extern uint64_t hos_large_lo, hos_large_hi;   /* (hos_main.c: large pages cannot be discarded) */
    if (hos_large_hi && lo < hos_large_hi && hi > hos_large_lo) return;
    /* (Triforce, the census: let go and still committed. Not the large pages above, left as they were, nor Linux's below,
       noted zeroed or decommitted there) */
    hos_mem_note(HOS_MEM_DISCARDED, lo, hi - lo);
    typedef DWORD(WINAPI * DiscardFn)(PVOID, SIZE_T);
    static DiscardFn discard;
    static int looked;
    if (!looked) {
        looked = 1;
        discard = (DiscardFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "DiscardVirtualMemory");
    }
    const int io = hos_guest_io_begin(GP(lo), hi - lo);   /* (Triforce: the pages' state changes under the lazy write-back's: rx_backend.c) */
    if (!discard || discard(GP(lo), (SIZE_T)(hi - lo)) != ERROR_SUCCESS)
        VirtualAlloc(GP(lo), (SIZE_T)(hi - lo), MEM_RESET, PAGE_READWRITE);
    hos_guest_io_end(io);
#elif !defined(__EMSCRIPTEN__)
    extern int hos_posix_guest_discard(void *p, size_t n);   /* (hos_main.c: MADV_REMOVE on the memfd window) */
    extern int rx_guest_imported(uint64_t lo, uint64_t hi);
    /* (pages a dma-buf import holds stay where they are, zeroed: a hole punched under one would leave the GPU on
       the old pages and the game on new ones) */
    if (rx_guest_imported(lo, hi)) memset(GP(lo), 0, (size_t)(hi - lo)), hos_mem_note(HOS_MEM_ZEROED, lo, hi - lo);
    else hos_posix_guest_discard(GP(lo), (size_t)(hi - lo)), hos_mem_note(HOS_MEM_DECOMMITTED, lo, hi - lo);
#endif
}

void hos_region_free(uint64_t addr) {
    uint64_t freed_size = 0;
    LOCK();
    for (int i = 0; i < nregions; i++) {
        if (regions[i].addr == addr && !regions[i].freeing) {
            pages_mark_used_locked(regions[i].addr, regions[i].size);
            freed_size = regions[i].size;
            regions[i].freeing = 1;
            break;
        }
    }
    UNLOCK();
    if (!freed_size) return;
    /* (Triforce) discarded, and noted so in the census, while the region is still listed: a region asked for meanwhile is not
       given these pages, which a discard after it would take from it, and a commit of them marked kept by the late note. Not
       under the lock: the discard may wait for the GPU (the lazy write-back) with the thread's virtual core given up, and a
       thread taking its core back with the lock held could wait on one that holds a core and waits for the lock */
    discard_pages(addr, freed_size);
    LOCK();
    for (int i = 0; i < nregions; i++) {
        if (regions[i].addr == addr && regions[i].freeing) {
            memmove(&regions[i], &regions[i + 1], (nregions - i - 1) * sizeof(Range));
            nregions--;
            break;
        }
    }
    UNLOCK();
}

/* ---- nn::os memory -------------------------------------------------------------------------------------- */

#if defined(A64_WRITE_TRACK) && !defined(__EMSCRIPTEN__)
/* HOS_WRITTEN natively (the soft write watch, rt/a64.h A64_MARK): host code wrote [p, p + n) of guest memory */
void hos_mark_written(const void *p, size_t n) {
    const uintptr_t a = (uintptr_t)p - (uintptr_t)A64_MEM;
    if (!n || (uintptr_t)p < (uintptr_t)A64_MEM || a >= HOS_GUEST_SPACE) return;
    for (uintptr_t q = a >> 12; q <= (a + n - 1) >> 12 && q < A64_DIRTY_PAGES; q++)
        __atomic_store_n(&a64_dirty[q], 1, __ATOMIC_RELEASE);
}
#endif

/* (hookshot) a heap of size bytes ends below the address regions and the guest window's end (no wrap-around: the size
   comes from the game). Also svc SetHeapSize's check (hos_svc.c) */
int hos_heap_size_fits(uint64_t size) {
    const uint64_t limit = region_floor < HOS_SPACE_END ? region_floor : HOS_SPACE_END;
    return limit >= HOS_HEAP_BASE && size <= limit - HOS_HEAP_BASE;
}
/* nn::os::SetMemoryHeapSize(unsigned long) -> Result */
HLE(_ZN2nn2os17SetMemoryHeapSizeEm) {
    uint64_t size = ARG(0);
    HOS_TRACE_CALL("SetMemoryHeapSize");
    if (!hos_heap_size_fits(size)) {
        hos_log("SetMemoryHeapSize(%#llx): collides with address regions at %#llx", (unsigned long long)size,
                (unsigned long long)region_floor);
        RET(0xCA01);   /* svc::ResultOutOfMemory */
        return;
    }
    if (size < hos_heap_size) {
        memset(GP(HOS_HEAP_BASE + size), 0, hos_heap_size - size);
        HOS_WRITTEN(GP(HOS_HEAP_BASE + size), hos_heap_size - size);
    }
    /* HOS_PREFAULT=1 (experiment): the new heap pages touched now (a zero written to fresh, zero memory), so that the
       game's first touches while streaming are not page faults (which may hold up the renderer's write-watch scans) */
    if (size > hos_heap_size && getenv("HOS_PREFAULT")) {
        const int64_t t0 = hos_now_ns();
        for (uint64_t a = HOS_HEAP_BASE + hos_heap_size; a < HOS_HEAP_BASE + size; a += 4096) *(volatile uint8_t *)GP(a) = 0;
        hos_log("heap: %llu MB prefaulted in %.0f ms", (unsigned long long)((size - hos_heap_size) >> 20), (hos_now_ns() - t0) / 1e6);
    }
    if (size > hos_heap_size) hos_commit(HOS_HEAP_BASE + hos_heap_size, size - hos_heap_size);
    hos_heap_size = size;
    hos_log("heap: %llu MB at %#llx", (unsigned long long)(size >> 20), (unsigned long long)HOS_HEAP_BASE);
    RET(0);
}

/* nn::os::AllocateMemoryBlock(unsigned long*, unsigned long) -> Result: 2 MB-aligned blocks from the heap */
HLE(_ZN2nn2os19AllocateMemoryBlockEPmm) {
    uint64_t out = ARG(0), size = (ARG(1) + 0x1FFFFF) & ~0x1FFFFFULL;
    HOS_TRACE_CALL("AllocateMemoryBlock");
    LOCK();
    uint64_t a = HOS_HEAP_BASE + hos_heap_used;
    int ok = hos_heap_used + size <= hos_heap_size;
    if (ok) hos_heap_used += size;
    UNLOCK();
    if (!ok) {
        hos_log("AllocateMemoryBlock(%#llx): heap exhausted", (unsigned long long)size);
        RET(0x1A01);   /* os::ResultOutOfMemory */
        return;
    }
    G64(out) = a;
    RET(0);
}

/* nn::os::AllocateAddressRegion(unsigned long*, unsigned long) -> Result */
HLE(_ZN2nn2os21AllocateAddressRegionEPmm) {
    HOS_TRACE_CALL("AllocateAddressRegion");
    G64(ARG(0)) = hos_region_alloc(ARG(1), 0x10000);
    RET(0);
}

/* nn::os::FreeAddressRegion(unsigned long) -> Result */
HLE(_ZN2nn2os17FreeAddressRegionEm) {
    HOS_TRACE_CALL("FreeAddressRegion");
    hos_region_free(ARG(0));
    RET(0);
}

/* nn::os::AllocateMemoryPages(unsigned long, unsigned long) -> Result: new pages read as zero */
HLE(_ZN2nn2os19AllocateMemoryPagesEmm) {
    HOS_TRACE_CALL("AllocateMemoryPages");
    const uint64_t a = ARG(0), n = ARG(1), end = a + n;
    if (!hos_guest_range(a, n)) {   /* (hookshot: a range that wrapped around was zeroed whole, past the guest window) */
        hos_log("AllocateMemoryPages(%#llx, %#llx): outside the guest window", (unsigned long long)a, (unsigned long long)n);
        RET(0xCA01);   /* svc::ResultOutOfMemory */
        return;
    }
    static int zero_all = -1;
    if (zero_all < 0) zero_all = getenv("HOS_ZERO_ALL_PAGES") != NULL;
    if (end > (1ULL << 32) && !zero_all) {
        /* (mkw-recomp) above 4 GB the used-page bitmap does not reach, and zeroing the whole range wiped live data
           when the game mapped pages over memory it already used (a heap's list lost its links). Horizon's rule
           instead: only newly mapped pages read as zero. A commit leaves committed pages as they are and gives fresh
           ones zeroed; FreeMemoryPages decommits. */
        hos_commit(a, n);
        hos_mem_note(HOS_MEM_PAGES, a, n);   /* (Triforce: the census, the game's own pages) */
    } else if (zero_all || end > (1ULL << 32) || end < a) {
        memset(GP(a), 0, n);
    } else {
        uint64_t runs[64][2];
        int nruns = 0, overflow = 0, in_run = 0;
        LOCK();
        for (uint64_t p = a & ~0xFFFULL; p < end; p += 0x1000) {
            const uint64_t pg = p >> 12, bit = 1ULL << (pg & 63);
            const int used = (pages_used[pg >> 6] & bit) != 0;
            pages_used[pg >> 6] |= bit;
            if (used && !in_run) {
                if (nruns == 64) overflow = 1;
                else runs[nruns][0] = p < a ? a : p;
                in_run = 1;
            } else if (!used && in_run) {
                if (nruns < 64) runs[nruns++][1] = p;
                in_run = 0;
            }
        }
        if (in_run && nruns < 64) runs[nruns++][1] = end;
        UNLOCK();
        if (overflow) memset(GP(a), 0, n);
        else
            for (int i = 0; i < nruns; i++) memset(GP(runs[i][0]), 0, runs[i][1] - runs[i][0]);
    }
    HOS_WRITTEN(GP(a), n);
    RET(0);
}

/* nn::os::FreeMemoryPages(unsigned long, unsigned long) -> Result */
HLE(_ZN2nn2os15FreeMemoryPagesEmm) {
    HOS_TRACE_CALL("FreeMemoryPages");
    if (!hos_guest_range(ARG(0), ARG(1))) {   /* (hookshot: decommitted or discarded: guest pages only) */
        hos_log("FreeMemoryPages(%#llx, %#llx): outside the guest window", (unsigned long long)ARG(0), (unsigned long long)ARG(1));
        RET(0xCA01);
        return;
    }
    if (ARG(0) + ARG(1) > (1ULL << 32)) {   /* (mkw-recomp: the pair of AllocateMemoryPages' commit above) */
        const uint64_t lo = (ARG(0) + 0xFFF) & ~0xFFFULL, hi = (ARG(0) + ARG(1)) & ~0xFFFULL;
        /* (pages the Vulkan back end imported, as part of a pool's 16 MB chunk, stay committed and are zeroed: the GPU
           still maps them, and decommitted pages under an import faulted the device. Read as zero, as freed pages) */
        extern int rx_guest_imported(uint64_t lo, uint64_t hi);
        /* (Triforce: zeroed or decommitted: the lazy write-back's pages there first, rx_backend.c) */
        const int io = hi > lo ? hos_guest_io_begin(GP(lo), hi - lo) : 0;
        /* ((Triforce) the census: zeroed pages stay committed, kept for the give-back once no import holds them) */
        if (hi > lo && rx_guest_imported(lo, hi)) memset(GP(lo), 0, (size_t)(hi - lo)), hos_mem_note(HOS_MEM_ZEROED, lo, hi - lo);
#if defined(_WIN32)
        else if (hi > lo) VirtualFree(GP(lo), (SIZE_T)(hi - lo), MEM_DECOMMIT), hos_mem_note(HOS_MEM_DECOMMITTED, lo, hi - lo);
#else
        else if (hi > lo) {   /* (linux: kept mapped, the pages read as zero; MADV_REMOVE on the memfd window) */
            extern int hos_posix_guest_discard(void *p, size_t n);
            hos_posix_guest_discard(GP(lo), (size_t)(hi - lo));
            hos_mem_note(HOS_MEM_DECOMMITTED, lo, hi - lo);
        }
#endif
        hos_guest_io_end(io);
        RET(0);
        return;
    }
    discard_pages(ARG(0), ARG(1));
    RET(0);
}

/* nn::os::QueryMemoryInfo(nn::os::MemoryInfo*). TOTK reads +0 (total available) and +8 (total used):
   heap = align_down(total - used, 2 MB) - 0x43800000 (main+0x1131004). */
HLE(_ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE) {
    uint64_t info = ARG(0);
    HOS_TRACE_CALL("QueryMemoryInfo");
    G64(info + 0x00) = hos_total_memory;                       /* total available */
    G64(info + 0x08) = hos_program_size + (16ULL << 20);       /* total used: program + system */
    G64(info + 0x10) = hos_heap_size;                          /* total heap */
    G64(info + 0x18) = hos_heap_used;                          /* allocated heap */
    G64(info + 0x20) = hos_program_size;                       /* program */
    G64(info + 0x28) = 0;                                      /* thread stacks */
    G32(info + 0x30) = 1;                                      /* threads */
}

/* nn::os::SetMemoryAllocatorForThreadLocal runs as the SDK's own code: it stores the game's allocator where
   nnosAllocateMemoryForThreadLocal finds it, which nnmusl uses for each thread's TLS (hos_thread.c thread_main). */

/* nn::os::SetDyingMessageRegion(unsigned long, unsigned long) -> Result */
HLE(_ZN2nn2os21SetDyingMessageRegionEmm) { RET(0); }

/* ---- (Triforce) the guest memory census (guest_census.h has why) ----------------------------------------------------------
   The notes: what each place that commits, maps or lets go of guest pages did, into the census's map. Kept while the census
   line or the give-back is on (HOS_MEM_CENSUS, on by default; HOS_MEM_GIVE_BACK: mkw_glue.c hos_mem_census_start); with both
   off nothing is noted. */
#if defined(_WIN32)
static SRWLOCK census_lock = SRWLOCK_INIT;
#define CENSUS_LOCK() AcquireSRWLockExclusive(&census_lock)
#define CENSUS_UNLOCK() ReleaseSRWLockExclusive(&census_lock)
#else
static pthread_mutex_t census_lock = PTHREAD_MUTEX_INITIALIZER;
#define CENSUS_LOCK() pthread_mutex_lock(&census_lock)
#define CENSUS_UNLOCK() pthread_mutex_unlock(&census_lock)
#endif
static uint64_t census_touched, census_zeroed, census_discarded, census_given, census_given_runs, census_refused;
static int census_unsupported;   /* (the renderer has no mkwvk_guest_release: no give-back) */

int hos_mem_census_on(void) {
    static volatile int on = -1;
    int v = on;
    if (v < 0) {   /* (asked first by the program's commit, before any thread runs) */
        const char *c = getenv("HOS_MEM_CENSUS"), *g = getenv("HOS_MEM_GIVE_BACK");
        v = !(c && atoi(c) <= 0) || (g && atoi(g) > 0);
        on = v;
    }
    return v;
}

void hos_mem_note(int what, uint64_t addr, uint64_t size) {
    const uint64_t lo = addr, hi = addr + size;
    if (!size || hi < lo || hi > GC_SPACE || !hos_mem_census_on()) return;
    switch (what) {
    case HOS_MEM_COMMITTED:   /* (before the commit, under the lock the give-back checks and decommits under) */
        CENSUS_LOCK();
        gc_change(&census, GC_HELD, lo, hi, 1);
        gc_change(&census, GC_KEPT, lo, hi, 0);
        CENSUS_UNLOCK();
        break;
    case HOS_MEM_TOUCHED:   /* (the first touch's exception handler: no lock. A page it commits was reserved, never kept) */
        gc_change(&census, GC_HELD, lo, hi, 1);
        gc_change(&census, GC_KEPT, lo, hi, 0);
        __atomic_add_fetch(&census_touched, 1, __ATOMIC_RELAXED);
        break;
    case HOS_MEM_PAGES:
    case HOS_MEM_MAPPED:
        gc_change(&census, GC_GAME, lo, hi, 1);
        break;
    case HOS_MEM_DECOMMITTED:
        gc_change(&census, GC_HELD, lo, hi, 0);
        gc_change(&census, GC_GAME, lo, hi, 0);
        gc_change(&census, GC_KEPT, lo, hi, 0);
        break;
    case HOS_MEM_ZEROED:   /* (every page of it committed: the zeroing touched it) */
        gc_change(&census, GC_HELD, lo, hi, 0);
        gc_change(&census, GC_GAME, lo, hi, 0);
        __atomic_add_fetch(&census_zeroed, gc_change(&census, GC_KEPT, lo, hi, 1), __ATOMIC_RELAXED);
        break;
    case HOS_MEM_DISCARDED:   /* (only what was committed stays so) */
        gc_change(&census, GC_GAME, lo, hi, 0);
        __atomic_add_fetch(&census_discarded, gc_move(&census, GC_HELD, GC_KEPT, lo, hi), __ATOMIC_RELAXED);
        break;
    }
}

/* hos_commit's notes (mkw_glue.c), around its commit: held before it, under the lock the give-back checks and decommits under
   (so no give-back takes a page it is committing), and no longer kept only once the commit is made. A commit that failed
   puts back the pages it set held (kept ones kept, never committed ones neither); begin's answer goes to end */
void *hos_mem_commit_begin(uint64_t addr, uint64_t size) {
    const uint64_t lo = addr, hi = addr + size;
    if (!size || hi < lo || hi > GC_SPACE || !hos_mem_census_on()) return NULL;
    const uint64_t n = gc_words(lo, hi);
    uint64_t *had = n ? (uint64_t *)malloc((size_t)n * sizeof *had) : NULL;
    if (had) memset(had, 0xFF, (size_t)n * sizeof *had);
    CENSUS_LOCK();
    if (had) gc_set_keeping(&census, GC_HELD, lo, hi, had);
    else gc_change(&census, GC_HELD, lo, hi, 1);   /* (no room to say which were held: a failed commit leaves them held) */
    CENSUS_UNLOCK();
    return had;
}
void hos_mem_commit_end(void *had, uint64_t addr, uint64_t size, int committed) {
    const uint64_t lo = addr, hi = addr + size;
    if (size && hi >= lo && hi <= GC_SPACE && hos_mem_census_on()) {
        CENSUS_LOCK();
        if (committed) gc_change(&census, GC_KEPT, lo, hi, 0);
        else if (had) gc_unset_had(&census, GC_HELD, lo, hi, (const uint64_t *)had);
        CENSUS_UNLOCK();
    }
    free(had);
}

/* The counts (mkw_glue.c's line): the live address regions copied under the regions' lock, then counted without it */
void hos_mem_census(GcCensus *c) {
    static uint64_t (*list)[2];
    static int cap;
    int n = 0;
    LOCK();
    if (nregions > cap) {
        uint64_t(*grown)[2] = (uint64_t(*)[2])realloc(list, (size_t)nregions * sizeof *list);
        if (grown) list = grown, cap = nregions;
    }
    for (; n < nregions && n < cap; n++) list[n][0] = regions[n].addr, list[n][1] = regions[n].size;
    UNLOCK();
    gc_census_count(&census, (const uint64_t(*)[2])list, n, HOS_HEAP_BASE, hos_heap_size, c);
    c->touched = __atomic_load_n(&census_touched, __ATOMIC_RELAXED);
    c->kept_import = __atomic_load_n(&census_zeroed, __ATOMIC_RELAXED);
    c->kept_discard = __atomic_load_n(&census_discarded, __ATOMIC_RELAXED);
    c->given_back = census_given, c->given_runs = census_given_runs, c->refused = census_refused;
    c->budget = hos_total_memory, c->program = hos_program_size;
}

/* The give-back's decommit of [lo, hi), run by the renderer with its import locks held (rx_backend.c rx_guest_release): no
   import holds the pages, and none can start to meanwhile. Only when every page is still kept and none was handed out again
   since the pass found them (hos_commit notes before it commits, under this lock). 1: given back */
static int census_release(uint64_t lo, uint64_t hi) {
    extern uint64_t hos_large_lo, hos_large_hi;
    if (hos_large_hi && lo < hos_large_hi && hi > hos_large_lo) return 0;   /* (large pages are not decommitted) */
    if (lo < (1ULL << 32)) return 0;   /* (below 4 GB pages are handed out without a commit: hos_mem_give_back) */
    CENSUS_LOCK();
    int ok = gc_count(&census, GC_KEPT, lo, hi) == hi - lo && !gc_count(&census, GC_HELD, lo, hi);
    if (ok) {
#if defined(_WIN32)
        ok = VirtualFree(GP(lo), (SIZE_T)(hi - lo), MEM_DECOMMIT) != 0;
#else
        extern int hos_posix_guest_discard(void *p, size_t n);
        ok = hos_posix_guest_discard(GP(lo), (size_t)(hi - lo)) == 0;
#endif
        if (ok) gc_change(&census, GC_KEPT, lo, hi, 0);
    }
    CENSUS_UNLOCK();
    return ok;
}
/* One kept run offered: the lazy write-back's pages there paid first (as FreeMemoryPages does), then the renderer asked */
static int census_offer(void *ctx, uint64_t lo, uint64_t hi) {
    (void)ctx;
    int rx_guest_release(uint64_t lo, uint64_t hi, int (*release)(uint64_t lo, uint64_t hi));   /* (rx_backend.c) */
    const int io = hos_guest_io_begin(GP(lo), hi - lo);
    const int r = rx_guest_release(lo, hi, census_release);
    hos_guest_io_end(io);
    if (r == -2) census_unsupported = 1;
    return r == 1;
}
/* A pass of the give-back (the census's thread, mkw_glue.c): kept runs of 16 MB at most, 64 of them at most, until max_bytes
   are given back. The bytes given back; -1 when the renderer cannot (an older one, without mkwvk_guest_release) */
int64_t hos_mem_give_back(uint64_t max_bytes) {
    static uint64_t cursor;
    if (census_unsupported) return -1;
    if (!hos_mem_census_on()) return 0;
    uint64_t runs = 0, refused = 0;
    /* (from 4 GB up: below it AllocateMemoryPages zeroes pages without committing them, and a host call handed one that was
       given back would fail on it without a fault) */
    const uint64_t given = gc_give_back(&census, 1ULL << 32, &cursor, 16ULL << 20, max_bytes, 64, census_offer, NULL, &runs, &refused);
    census_given += given, census_given_runs += runs, census_refused += refused;
    return census_unsupported && !given ? -1 : (int64_t)given;
}
