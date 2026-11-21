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
   region being freed, listed until its pages are discarded (hos_region_free); game: one the game asked for itself
   (nn::os::AllocateAddressRegion), which holds nothing of the game's until it takes pages or a physical map in it */
typedef struct { uint64_t addr, size; int freeing, game; } Range;
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

static uint64_t region_alloc(uint64_t size, uint64_t align, int game) {
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
        regions[at].game = game;
        nregions++;
        if (found < region_floor && found >= HOS_HEAP_BASE) region_floor = found;   /* (not the holes below) */
    }
    UNLOCK();
    if (found) hos_commit(found, size);
    if (!found) hos_fatal(NULL, "address space exhausted (%#llx bytes asked, heap %#llx)", (unsigned long long)size,
                          (unsigned long long)hos_heap_size);
    return found;
}
uint64_t hos_region_alloc(uint64_t size, uint64_t align) { return region_alloc(size, align, 0); }

/* (Triforce) Whether a freed region's pages are discarded (hos_region_free). A region the game asked for itself holds
   nothing of the game's until it takes pages or a physical map in it, and the census knows which of its pages are the
   game's now (GC_GAME: what it was given, less what it gave back, each give-back discarded or decommitted where it was
   made). With none, there is nothing to discard: the pages were committed with the region and never handed over.
   Held to what it is told it has (HOS_MEM_LIMIT), the game's Resource Memory thread asks for a region, is refused its
   pages, frees the region and asks again: 16,640 times in a lab run of three areas (round 79), each free a
   DiscardVirtualMemory over 0.2 to 37 MB of pages nobody had touched, 61% of that thread's time while an area loaded,
   and the game's main thread waited for it: its own part of the frame 8 to 23 ms for seconds at a time, where 2 ms is
   usual (the stutter the limit was set aside for). Without the census (HOS_MEM_CENSUS=0) every region is discarded, as
   before, and so is one the host asked for itself (the renderer's and the pools' storage: written without pages) */
int hos_region_discard_needed(int game_region, int census_on, uint64_t game_pages) {
    return !(game_region && census_on && !game_pages);
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
    /* ((Triforce, 11-19) pages a Vulkan import holds (a pool's chunk) are zeroed where they are, as FreeMemoryPages and Linux below
       do: the import keeps them locked in RAM, so a discard gave nothing back, and it was slow there: 3.7 ms a region on average,
       23 ms at most, 42 s on the game's "Resource Memory" thread over a lab tour once the game let areas go
       (QueryMemoryInfo above), with the game's frames waiting on it (Hyrule Field 34 fps, Castle Town 15). Zeroed: the pages
       read as zero, as freed pages; the census keeps them for the give-back once no import holds them) */
    extern int rx_guest_imported(uint64_t lo, uint64_t hi);
    {
        const int io = hos_guest_io_begin(GP(lo), hi - lo);
        const int held = rx_guest_imported(lo, hi);
        if (held) memset(GP(lo), 0, (size_t)(hi - lo)), hos_mem_note(HOS_MEM_ZEROED, lo, hi - lo);
        hos_guest_io_end(io);
        if (held) return;   /* (lab tour: 5.2 GB zeroed in 0.47 s in all) */
    }
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
    /* ((Triforce) the thread's virtual core given up for the discard itself: milliseconds in the kernel for a region of some MB,
       in which the thread runs no guest code and cannot see a better thread's ask for its core (hos_thread.c vc_kick). Held to
       its memory (HOS_MEM_LIMIT) the game frees regions by the hundred while an area loads, on its Resource Memory thread.
       Only with the limit on, where it was measured: without it the game frees few regions and the path is upstream's) */
    const int away = hos_mem_limit() ? hos_vcore_away() : 0;
    if (!discard || discard(GP(lo), (SIZE_T)(hi - lo)) != ERROR_SUCCESS)
        VirtualAlloc(GP(lo), (SIZE_T)(hi - lo), MEM_RESET, PAGE_READWRITE);
    if (away) hos_vcore_back();
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

static int64_t region_frees_bare;   /* ((Triforce) regions freed without a discard: hos_region_discard_needed; the refusal line's) */
void hos_region_free(uint64_t addr) {
    uint64_t freed_size = 0;
    int game = 0;
    LOCK();
    for (int i = 0; i < nregions; i++) {
        if (regions[i].addr == addr && !regions[i].freeing) {
            pages_mark_used_locked(regions[i].addr, regions[i].size);
            freed_size = regions[i].size;
            game = regions[i].game;
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
    /* ((Triforce) hos_region_discard_needed: a region of the game's own with none of its pages has nothing to discard. Its
       pages are noted let go all the same, as a discard notes them: committed, and nobody's. Only with the limit on
       (HOS_MEM_LIMIT), where the refused asks make such regions by the thousand and it was measured; without it every
       region is discarded, as upstream does) */
    const uint64_t game_pages = addr + freed_size <= GC_SPACE ? gc_count(&census, GC_GAME, addr, addr + freed_size) : ~0ULL;
    if (hos_region_discard_needed(game && hos_mem_limit(), hos_mem_census_on(), game_pages)) {
        discard_pages(addr, freed_size);
    } else {
        hos_mem_note(HOS_MEM_DISCARDED, addr, freed_size);
        __atomic_add_fetch(&region_frees_bare, 1, __ATOMIC_RELAXED);
    }
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

/* ((Triforce, 11-19, lab) HOS_MEM_TRACE=1: every address region and page call of the game, with its size and caller, for the
   memory that grows all session and is never let go (the census's "let go 0" through whole tours) */
int hos_mem_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("HOS_MEM_TRACE") && atoi(getenv("HOS_MEM_TRACE")) != 0;
    return on;
}
#define MEM_TRACE(...) do { if (hos_mem_trace()) hos_log(__VA_ARGS__); } while (0)
/* (the callers above it: the guest's frame records, x29 up) */
static const char *mem_trace_stack(const Ctx *c, char *out, size_t n) {
    uint64_t fp = c->x[29];
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < 10 && at + 20 < n; i++) {
        if (!fp || (fp & 7) || !hos_guest_range(fp, 16)) break;
        const uint64_t ret = G64(fp + 8), up = G64(fp);
        at += (size_t)snprintf(out + at, n - at, " %#llx", (unsigned long long)ret);
        if (up <= fp) break;
        fp = up;
    }
    return out;
}

/* nn::os::AllocateAddressRegion(unsigned long*, unsigned long) -> Result */
HLE(_ZN2nn2os21AllocateAddressRegionEPmm) {
    HOS_TRACE_CALL("AllocateAddressRegion");
    G64(ARG(0)) = region_alloc(ARG(1), 0x10000, 1);   /* ((Triforce) the game's own: hos_region_discard_needed) */
    if (hos_mem_trace()) {
        char st[256];
        hos_log("memtrace: region %#llx size %#llx lr %#llx st%s", (unsigned long long)G64(ARG(0)), (unsigned long long)ARG(1),
                (unsigned long long)c->x[30], mem_trace_stack(c, st, sizeof st));
    }
    RET(0);
}

/* nn::os::FreeAddressRegion(unsigned long) -> Result */
HLE(_ZN2nn2os17FreeAddressRegionEm) {
    HOS_TRACE_CALL("FreeAddressRegion");
    MEM_TRACE("memtrace: region free %#llx lr %#llx", (unsigned long long)ARG(0), (unsigned long long)c->x[30]);
    hos_region_free(ARG(0));
    RET(0);
}

/* nn::os::AllocateMemoryPages(unsigned long, unsigned long) -> Result: new pages read as zero */
HLE(_ZN2nn2os19AllocateMemoryPagesEmm) {
    HOS_TRACE_CALL("AllocateMemoryPages");
    const uint64_t a = ARG(0), n = ARG(1), end = a + n;
    MEM_TRACE("memtrace: pages %#llx size %#llx lr %#llx", (unsigned long long)a, (unsigned long long)n, (unsigned long long)c->x[30]);
    if (!hos_guest_range(a, n)) {   /* (hookshot: a range that wrapped around was zeroed whole, past the guest window) */
        hos_log("AllocateMemoryPages(%#llx, %#llx): outside the guest window", (unsigned long long)a, (unsigned long long)n);
        RET(0xCA01);   /* svc::ResultOutOfMemory */
        return;
    }
    if (!hos_mem_room(a, n)) {   /* ((Triforce) HOS_MEM_LIMIT: past what the game is told it has, as the console refuses) */
        RET(0x1A01);   /* os::ResultOutOfMemory */
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
    MEM_TRACE("memtrace: pages free %#llx size %#llx lr %#llx", (unsigned long long)ARG(0), (unsigned long long)ARG(1), (unsigned long long)c->x[30]);
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

/* ((Triforce, 11-19) the game's pages beyond its heap (game_pages_now below) */
static uint64_t game_pages_now(void);
/* ((Triforce, 11-19) What Horizon answers as the memory in use: the program, the system's part, the heap and every page the game
   has mapped beyond it (AllocateMemoryPages, MapPhysicalMemory), at most the total. The game's "Resource Memory" allocator
   (main 0x8080f010 / 0x8080f090, beside its region and page calls) sizes what it may still take as total - used from
   QueryMemoryInfo. It was told program + system only, so it always saw about 9.6 GB free, kept every area's resources it had
   ever loaded, and the game's own memory climbed to 12.5 GB of 9809 MB after four areas (16 GB PCs ran out: #bug-tracker
   "0.5.1 Memory-Retention Regression", "Ton of performance issues after a certain amount of time playing", the Steam Deck's
   "crash after loading save"). Told what it holds, it lets old areas go as on a Switch. HOS_MEM_QUERY_USED=0: program + system
   only, as before (also when the census is off: the pages are not counted then) */
static uint64_t query_used(void) {
    static int full = -1;
    static uint64_t slack;
    if (full < 0) {
        const char *e = getenv("HOS_MEM_QUERY_USED"), *k = getenv("HOS_MEM_QUERY_SLACK_MB");
        full = !(e && *e == '0' && !e[1]) && hos_mem_census_on();
        slack = (uint64_t)(k && *k ? atoll(k) : 1024) << 20;
        hos_log("mem: QueryMemoryInfo's memory in use: %s, %llu MB less (HOS_MEM_QUERY_USED, HOS_MEM_QUERY_SLACK_MB)",
                full ? "the program, the heap and the game's pages, as Horizon" : "the program only, as before",
                (unsigned long long)(slack >> 20));
    }
    const uint64_t base = hos_program_size + (16ULL << 20);
    if (!full) return base;
    /* (the slack, 1024 MB by default (HOS_MEM_QUERY_SLACK_MB): room the game's cache may take beyond what a Switch leaves it,
       as NX2navi's pool extra; from its pages only, so the heap the game sizes at its start from total - used is the same. Our
       game needs ~3.2 GB of address regions at a save load where a Switch leaves ~2.8: with no slack it evicted and reloaded
       in every scene (lab tour, A/B in one build: Kokiri 52 fps, Lake Hylia 42 against 60 / 59.5 with the old answer); with
       1024 MB 60.0 / 59.9 and its own count held at 10.7 GB instead of 12.5 and climbing) */
    const uint64_t pages = game_pages_now(), used = base + hos_heap_size + (pages > slack ? pages - slack : 0);
    return used < hos_total_memory ? used : hos_total_memory;
}

/* nn::os::QueryMemoryInfo(nn::os::MemoryInfo*). TOTK reads +0 (total available) and +8 (total used):
   heap = align_down(total - used, 2 MB) - 0x43800000 (main+0x1131004). */
HLE(_ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE) {
    uint64_t info = ARG(0);
    HOS_TRACE_CALL("QueryMemoryInfo");
    const uint64_t used = query_used();
    MEM_TRACE("memtrace: query used %llu MB, free %llu MB lr %#llx", (unsigned long long)(used >> 20),
              (unsigned long long)((hos_total_memory - used) >> 20), (unsigned long long)c->x[30]);
    G64(info + 0x00) = hos_total_memory;                       /* total available */
    G64(info + 0x08) = used;                                   /* total used: program + system (+ heap + the game's pages) */
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
static int64_t census_game_bytes;   /* ((Triforce, 11-19) the game's pages and physical maps, as they are noted: hos_mem_used) */
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

/* ((Triforce, 11-19) The game's pages beyond its heap: the census's GC_GAME map (AllocateMemoryPages, MapPhysicalMemory, less
   what it let go), counted as the map changes (census_game_bytes, which HOS_MEM_LIMIT's hos_mem_used reads too), so a page mapped
   over one the game has already counts once. For QueryMemoryInfo's answer above. (HOS_MEM_BUDGET_MB, the budget that was here
   on 11-19, is HOS_MEM_LIMIT now: the same refusal, with its stutter fix) */
static uint64_t game_pages_now(void) {
    const int64_t g = __atomic_load_n(&census_game_bytes, __ATOMIC_RELAXED);
    return g > 0 ? (uint64_t)g : 0;
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
        __atomic_add_fetch(&census_game_bytes, (int64_t)gc_change(&census, GC_GAME, lo, hi, 1), __ATOMIC_RELAXED);
        break;
    case HOS_MEM_DECOMMITTED:
        gc_change(&census, GC_HELD, lo, hi, 0);
        __atomic_sub_fetch(&census_game_bytes, (int64_t)gc_change(&census, GC_GAME, lo, hi, 0), __ATOMIC_RELAXED);
        gc_change(&census, GC_KEPT, lo, hi, 0);
        break;
    case HOS_MEM_ZEROED:   /* (every page of it committed: the zeroing touched it) */
        gc_change(&census, GC_HELD, lo, hi, 0);
        __atomic_sub_fetch(&census_game_bytes, (int64_t)gc_change(&census, GC_GAME, lo, hi, 0), __ATOMIC_RELAXED);
        __atomic_add_fetch(&census_zeroed, gc_change(&census, GC_KEPT, lo, hi, 1), __ATOMIC_RELAXED);
        break;
    case HOS_MEM_DISCARDED:   /* (only what was committed stays so) */
        __atomic_sub_fetch(&census_game_bytes, (int64_t)gc_change(&census, GC_GAME, lo, hi, 0), __ATOMIC_RELAXED);
        __atomic_add_fetch(&census_discarded, gc_move(&census, GC_HELD, GC_KEPT, lo, hi), __ATOMIC_RELAXED);
        break;
    }
}

/* ---- (Triforce, 11-19) The game's memory held to what it is told it has (HOS_MEM_LIMIT, the lab's) ----------------------
   The game is told 9809 MB (hos_total_memory), and on the console that is a limit: its heap, its program and the pages and
   physical maps it takes count against it, and a map past it is refused (LimitReached). Here nothing was refused until the
   PC's own commit ran out, and "used" was told as the program and the heap alone, whatever the game had mapped since. So
   the game holds more than a console lets it: 11.7 GB by its own count standing in a fresh Market, 13.5 GB after a tour of
   ten areas (the census line), its resources never let go for want of room. Its 3.2 GB GPU heap then stays within some
   15 MB of full, and its texture compaction, which finds nothing to do in the fresh Market, runs a step every frame from
   the first area change on (host/oot_tex_compact.c): the after-tour slowdown of the lab's tours.

   HOS_MEM_LIMIT=1: the limit is kept as the console keeps it. AllocateMemoryPages and MapPhysicalMemory are refused when
   the game's count would pass what it is told it has, and UsedMemorySize is the count (never more than the total).
   HOS_MEM_LIMIT=<MB> (above 1): refused past that many MB instead, to try a limit between the console's and none.
   Unset or 0: as before. Needs the census (HOS_MEM_CENSUS, on by default).
   What the game does when refused (lab round 79): it stays within a few MB of the limit and its Resource Memory thread asks
   again, some 16,000 times in a run of three areas, each ask a region taken, its pages refused, the region freed. That
   free's discard of pages nobody had touched was the stutter the limit was first set aside for (the game's main thread
   waits for that thread): hos_region_discard_needed leaves it out. The regions it frees with pages in them, some hundreds
   while an area loads, are discarded with the thread's virtual core given up (discard_pages). With both, the game's own
   part of the frame is what it is without the limit (over 8 ms in 20 frames of a three-area run either way), and the
   game holds 9.8 GB where it held 11.7. */
int64_t hos_mem_limit(void) {
    static volatile int64_t v = -1;
    int64_t l = v;
    if (l < 0) {
        const char *e = getenv("HOS_MEM_LIMIT");
        long long n = e && *e ? atoll(e) : 0;
        if (n > (1LL << 24)) n = 1LL << 24;   /* ((Triforce) 16 TB at most: a larger size would overflow the shift below) */
        if (n > 0 && !hos_mem_census_on())
            hos_log("mem: HOS_MEM_LIMIT is set but the census is off (HOS_MEM_CENSUS=0): it needs the census's count, so no limit is kept");
        l = n <= 0 || !hos_mem_census_on() ? 0 : n == 1 ? (int64_t)hos_total_memory : (int64_t)n << 20;
        if (l) hos_log("mem: the game's memory is held to %lld MB (HOS_MEM_LIMIT; it is told %llu MB)", (long long)(l >> 20),
                       (unsigned long long)(hos_total_memory >> 20));
        /* ((Triforce, 11-20) the console's own size starves this build: its heap takes 6.9 GB of the 9.8, and an area's
           resources need 4.7 GB and more (a fresh Market; 6.5 after a tour). The owner's save stayed on its loading screen
           with the game at 9,807 MB and no longer asking: its load threads idle on their queues, read with the hang watch's
           asked-for stacks. A size in MB leaves room: 12800 loaded and played four areas) */
        if (n == 1 && l)
            hos_log("mem: HOS_MEM_LIMIT=1 is the console's own size, which this build does not fit in: a save's load can stay on "
                    "its loading screen for good. Give a size in MB instead (12800 loaded four areas)");
        v = l;
    }
    return l;
}
/* What the game holds in Horizon's terms: its program, its heap, its pages and physical maps (the census's "own count") */
uint64_t hos_mem_used(void) {
    const int64_t g = __atomic_load_n(&census_game_bytes, __ATOMIC_RELAXED);
    return hos_program_size + hos_heap_size + (uint64_t)(g > 0 ? g : 0);
}
/* The rule: 1 when taking `fresh` more bytes keeps the count within the limit (limit 0: no limit, always). A range the game
   holds whole already (fresh 0) takes nothing, and fits wherever the count stands */
int hos_mem_fits(uint64_t used, uint64_t fresh, uint64_t limit) { return !limit || !fresh || used + fresh <= limit; }
/* Whether the game may take [addr, addr + size) as pages or a physical map: only the pages of it that it does not hold yet
   count. A refusal is counted, and the first 16 and every 256th are logged */
int hos_mem_room(uint64_t addr, uint64_t size) {
    const int64_t limit = hos_mem_limit();
    if (!limit || !size || addr + size < addr || addr + size > GC_SPACE) return 1;
    const uint64_t held = gc_count(&census, GC_GAME, addr, addr + size);
    const uint64_t fresh = size > held ? size - held : 0, used = hos_mem_used();
    if (hos_mem_fits(used, fresh, (uint64_t)limit)) return 1;
    static int64_t refused;
    const int64_t n = __atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED);
    if (n <= 16 || n % 256 == 0)
        hos_log("mem: refused %llu KB at %#llx: the game holds %llu MB of %lld (HOS_MEM_LIMIT; %lld refusals so far; %lld regions "
                "freed with no page of the game's in them, not discarded)",
                (unsigned long long)(fresh >> 10), (unsigned long long)addr, (unsigned long long)(used >> 20), (long long)(limit >> 20),
                (long long)n, (long long)__atomic_load_n(&region_frees_bare, __ATOMIC_RELAXED));
    return 0;
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
