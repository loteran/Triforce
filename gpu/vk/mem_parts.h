// (Triforce) What the process holds in RAM, in parts, for the log's "vk: memory, the parts" line (vk_memory.cpp
// memory_parts_tick, every 600 presents) and the rules that react to the PC's RAM running short. Apart from Vulkan and
// Windows so that tests/test_mem_parts.py runs it alone.
//
// Why: a 32 GB PC ran out of RAM in play (v0.4.0-test7: "RAM short (525 to 850 of 32069 MB available)" 40 times, the
// working set 20.9 GB, frames of 102 and 245 ms outside the replay and the present while Windows paged), and the log
// said only the process's totals. Its private bytes are no guide to the RAM (they grew with the VRAM, 29.7 -> 42.5 GB
// while the device's blocks grew 1.4 -> 11.3 GB: the driver's video memory is charged there), the working set is. The
// working set is the game's own memory (the guest window: its heap and regions, committed at once, in RAM as the game
// touches it) and the rest: the driver's shader modules, pipelines and pipeline cache, the allocator's blocks in system
// memory, the heap. The line gives each part that is known, and the game's share of the working set by a sample of
// its pages (sample_plan, below): one page in every so many of the committed window asked whether it is in RAM.
// (11-16) The rest is split the same way, by what the address space says each run is (Kind): the program's code, the
// driver's, other code, mapped files and views, write-combined or uncached memory (what a driver maps for the GPU), and
// the heaps and other private memory. A session of v0.4.0-test9 had 9.9 GB of working set outside the game's memory
// all through play, with the RAM short, and nothing in the line said what it was.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mem_parts {

/// What a run of the address space is (kNone: not counted: free, reserved, or no access)
enum Kind : uint8_t { kNone = 0, kGuest, kExe, kDriverCode, kOtherCode, kMapped, kGpuVisible, kPrivate, kKinds };
/// A committed run, [lo, hi), and what it is
struct Span {
    uint64_t lo = 0, hi = 0;
    uint8_t kind = kGuest;
};

/// The runs between lo and hi that query(at, &base, &size, &kind) counts, which answers for the region that holds at
/// (VirtualQuery's answer: its base, its size from there, and what it is, kNone for what is not counted); false ends the
/// walk. [skip_lo, skip_hi) is stepped over without a query (the guest window, which has a walk of its own). At most
/// max_queries regions are asked (the walk is a system call a region), and runs of one kind that touch are joined.
/// *queries: how many were asked; *whole: false when max_queries stopped the walk before hi
template <class Query>
std::vector<Span> kind_spans(uint64_t lo, uint64_t hi, Query&& query, size_t max_queries, uint64_t skip_lo = 0,
                             uint64_t skip_hi = 0, size_t* queries = nullptr, bool* whole = nullptr) {
    std::vector<Span> out;
    uint64_t at = lo;
    size_t n = 0;
    while (at < hi && n < max_queries) {
        if (at >= skip_lo && at < skip_hi) {   // (no query spent on it)
            at = skip_hi;
            continue;
        }
        n++;
        uint64_t base = 0, size = 0;
        uint8_t kind = kNone;
        if (!query(at, &base, &size, &kind) || !size) {
            at = hi;   // (the end of the walk: whole as far as anything answers)
            break;
        }
        uint64_t end = base + size > base ? base + size : hi;   // (no wrap past the top)
        if (end <= at) {
            at = hi;
            break;
        }
        if (skip_hi > skip_lo && at < skip_lo && end > skip_lo) end = skip_lo;   // (the skipped window is not counted)
        const uint64_t a = base > at ? base : at, b = end < hi ? end : hi;
        if (kind != kNone && a < b) {
            if (!out.empty() && out.back().hi == a && out.back().kind == kind) out.back().hi = b;
            else out.push_back({a, b, kind});
        }
        at = end;
    }
    if (queries) *queries = n;
    if (whole) *whole = at >= hi;
    return out;
}
/// The committed runs between lo and hi, from query(at, &base, &size, &committed) (the guest window's walk)
template <class Query>
std::vector<Span> committed_spans(uint64_t lo, uint64_t hi, Query&& query, size_t max_queries, size_t* queries = nullptr,
                                  bool* whole = nullptr) {
    return kind_spans(
        lo, hi,
        [&](uint64_t at, uint64_t* base, uint64_t* size, uint8_t* kind) {
            bool committed = false;
            if (!query(at, base, size, &committed)) return false;
            *kind = committed ? kGuest : kNone;
            return true;
        },
        max_queries, 0, 0, queries, whole);
}

/// Pages to ask about: one every stride bytes of each committed run (stride a whole number of pages, at least one, made
/// larger until the runs' pages are at most max_samples strides), each standing for the bytes up to the next (the run's
/// last for what is left). Each run has its first page asked, so there are at most max_samples + spans.size() of them
struct Plan {
    uint64_t committed = 0, stride = 0;
    std::vector<uint64_t> at, bytes;
    std::vector<uint8_t> kind;   // (each sample's run's)
};
inline Plan sample_plan(const std::vector<Span>& spans, uint64_t page, size_t max_samples) {
    Plan p;
    if (!page) page = 4096;
    for (const Span& s : spans) p.committed += s.hi - s.lo;
    if (!max_samples) max_samples = 1;
    const uint64_t pages = (p.committed + page - 1) / page;
    const uint64_t every = pages > max_samples ? (pages + max_samples - 1) / max_samples : 1;
    p.stride = every * page;
    for (const Span& s : spans)   // (a run shorter than the stride counts all of itself by its first page)
        for (uint64_t a = s.lo; a < s.hi; a += p.stride) {
            p.at.push_back(a);
            p.bytes.push_back(s.hi - a < p.stride ? s.hi - a : p.stride);
            p.kind.push_back(s.kind);
            if (s.hi - a <= p.stride) break;   // (and no step past the top of the address space)
        }
    return p;
}
/// What the pages found in RAM stand for, in bytes (resident[i] answers for plan.at[i])
inline uint64_t resident_bytes(const Plan& p, const std::vector<bool>& resident) {
    uint64_t r = 0;
    for (size_t i = 0; i < p.at.size() && i < resident.size(); i++)
        if (resident[i]) r += p.bytes[i];
    return r;
}
/// The same, by the samples' kinds, into out[kKinds] (added to what is there)
inline void resident_by_kind(const Plan& p, const std::vector<bool>& resident, uint64_t out[kKinds]) {
    for (size_t i = 0; i < p.at.size() && i < resident.size(); i++)
        if (resident[i] && p.kind[i] < kKinds) out[p.kind[i]] += p.bytes[i];
}

/// The parts the line gives, in bytes (-1: not known on this platform or in this run)
struct Parts {
    int64_t working_set = -1, private_bytes = -1;
    int64_t guest_committed = -1, guest_resident = -1;
    int64_t by_kind[kKinds] = {-1, -1, -1, -1, -1, -1, -1, -1};   // (the rest in RAM by what it is; -1: not sampled)
    int64_t modules = 0, module_bytes = 0;                 // (program variants' shader modules alive, and their SPIR-V)
    int64_t packed_stages = 0, packed_bytes = 0;           // (stages kept packed instead, and what that takes)
    int64_t pcache_bytes = -1;                             // (the pipeline cache as last read or saved; -1: let go)
    int64_t pipelines = 0, libraries = 0;                  // (graphics pipelines and stage libraries kept)
    int64_t drawn = -1;                                    // (of those pipelines, how many a draw has looked up; -1: not known)
    int64_t released = 0;                                  // (kept pipelines no draw used, given back for the RAM)
    int64_t system_blocks = -1;                            // (the allocator's blocks in system memory)
    int64_t ram_avail = -1, ram_total = -1;
};
inline std::string mb(int64_t b) {
    if (b < 0) return "?";
    char t[32];
    std::snprintf(t, sizeof t, "%lld", (long long)(b >> 20));
    return t;
}
/// The line, without its "vk: " head
inline std::string line(const Parts& p) {
    std::string s = "memory, the parts (MB): working set " + mb(p.working_set) + " (private " + mb(p.private_bytes) + ")";
    s += "; the game's own memory " + mb(p.guest_committed) + " committed, " + mb(p.guest_resident) + " of it in RAM";
    if (p.working_set >= 0 && p.guest_resident >= 0)
        s += "; the rest of the working set " + mb(p.working_set > p.guest_resident ? p.working_set - p.guest_resident : 0);
    if (p.by_kind[kPrivate] >= 0) {
        static const char* const names[kKinds] = {"", "", "the program's code", "the driver's code", "other code",
                                                  "mapped files and views", "write-combined or uncached memory",
                                                  "the heaps and other private memory"};
        s += " (sampled:";
        for (int k = kExe; k < kKinds; k++) s += std::string(k == kExe ? " " : ", ") + names[k] + " " + mb(p.by_kind[k]);
        s += ")";
    }
    char t[160];
    std::snprintf(t, sizeof t, " (%lld modules", (long long)p.modules);
    s += ": shader modules " + mb(p.module_bytes) + t;
    std::snprintf(t, sizeof t, "; %lld stages packed in ", (long long)p.packed_stages);
    s += t + mb(p.packed_bytes) + ")";
    s += p.pcache_bytes < 0 ? ", the pipeline cache let go" : ", the pipeline cache " + mb(p.pcache_bytes);
    std::snprintf(t, sizeof t, ", %lld pipelines and %lld stage libraries kept", (long long)p.pipelines, (long long)p.libraries);
    s += t;
    if (p.drawn >= 0) {
        std::snprintf(t, sizeof t, " (%lld of the pipelines drawn; %lld no draw used given back)", (long long)p.drawn,
                      (long long)p.released);
        s += t;
    }
    s += ", the allocator's blocks in system memory " + mb(p.system_blocks);
    s += "; RAM " + mb(p.ram_avail) + " of " + mb(p.ram_total) + " available";
    return s;
}

// ---- The PC's RAM running short: what the back end does about it that costs play nothing (vk_memory.cpp ram_watch) ----
/// Short: under 1.5 GB available, the imports' rule (vk_memory.cpp imports_give_back), whatever the PC has
constexpr uint64_t kShortBytes = 1536ull << 20;
inline bool short_now(uint64_t avail) { return avail < kShortBytes; }
/// Low: under 3 GB available, or a tenth of the RAM on a PC of more than 30 GB. The pipeline builder behind the game
/// (vk_draw.cpp predict_background) rests then: what the driver keeps of each pipeline it builds is RAM
inline bool low_now(uint64_t avail, uint64_t total) {
    const uint64_t tenth = total / 10;
    return avail < (tenth > (3ull << 30) ? tenth : (3ull << 30));
}
/// Whether a copy of bytes fits in RAM now with 2 GB to spare: the pipeline cache's save in play fetches the whole cache into
/// memory (2 to 3 GB with the stage libraries in it), and with less than that available it is what Windows pages out
inline bool copy_fits(uint64_t avail, uint64_t bytes) { return avail > bytes && avail - bytes > (2ull << 30); }
/// (11-16) Whether the precompile keeps every pipeline it builds for the session (vk_draw.cpp precompile_keeps_all): only where
/// the RAM the game can have, what is available plus what the process holds already (own), is 22 GB or more, on a PC of
/// 20 GB or more. The figure is v0.4.0-test9's, a 32 GB PC that started with 18.4 GB free: the game's own memory reached
/// 9.7 GB in RAM, the rest of the working set 9.9 GB, of which the precompile's 14,535 graphics and 1,033 compute
/// pipelines took about 3.3 GB (the working set before and after it); 9.7 + 6.6 + 3.3 GB, and 2 GB for Windows to page
/// with. The PC's total alone says nothing of what its other programs hold
constexpr uint64_t kKeepAllBytes = 22ull << 30;
inline bool keep_all_pipelines(uint64_t total, uint64_t avail, uint64_t own) {
    return total >= (20ull << 30) && avail + own >= kKeepAllBytes;
}
/// (11-18) What the process holds that counts as the game's for keep_all_pipelines: its working set (ws) less what reading the
/// pipeline cache added (cache_read; the driver holds that all session, so it is no room for the pipelines). Two 32 GB PCs on
/// v0.5.1 with 2.7 and 3.0 GB cache files (4.7 and 4.9 GB of working set once read): 18.2 GB available + 7.0 held and 19.1 +
/// 7.7, both over 22 GB, everything kept, and RAM short all session (39 times on one; the other lost the GPU after the save
/// loaded). Without the cache: 20.5 and 21.9 GB, kept by none. Not on AMD's driver (amd: the vendor 0x1002), which the rule
/// keeps as it was: there a pipeline left to its first draw cost ~100 ms (an RX 6650 XT: 152 of them in one 15.7 s frame),
/// worse than the RAM
inline uint64_t own_for_keep(uint64_t ws, uint64_t cache_read, bool amd) {
    if (amd) return ws;
    return ws > cache_read ? ws - cache_read : 0;
}

}   // namespace mem_parts
