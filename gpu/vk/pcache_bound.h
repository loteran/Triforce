// (Triforce) The bound on the pipeline cache's file (mkw_vk_pipelines.bin; vk_draw.cpp pcache_init, pipeline_cache_save),
// apart from Vulkan so that tests/test_pcache_bound.py can run it. As NX2navi's (runtime/vk_cache_file.h and its cache_cap).
//
// The file is read into memory whole at every start and handed to the driver, which keeps its own copy for the session, and
// every save fetches all of it again. Nothing bounded it: a player's file reached 4.1 GB on a 32 GB PC whose log said "RAM
// short" all session, the lab's 3.9 GB. The rule of 11-14 (the file is kept for one set of translated shaders) stops the
// growth from update to update; what one set of shaders can reach on some driver, nothing stops. So:
//  - a file over the bound is not read at all: its size is asked before a byte of it is, so it never takes its size in
//    memory. The cache starts empty, the driver's own cache serves the pipelines, and the first save writes a new file
//    over the old one;
//  - data over the bound is not written: the file keeps its last save, which is under the bound and is read at the next
//    start; what was built after that save is compiled again then (from the driver's own cache, where it has one). A file
//    the next start would refuse is never made;
//  - a file at or under the bound is read and written as before, byte for byte (in pieces now: no single read call has
//    to take gigabytes), and one whose read ends early is no cache at all, never part of one.
// The bound: RX_VK_PCACHE_MAX_MB megabytes when set (0: none, as before); unset, an eighth of the PC's RAM, 2048 MB at
// least and 8192 MB at most (16 GB: 2048, 32 GB: 4096, 64 GB and more: 8192). Files measured: 647 MB fresh and 1146 MB
// with the precompile's 13,000 pipelines (vk_draw.cpp's notes), 1631 MB in an install on the lab's PC: all under the
// least of them. Never below 2048: a bound under what one run builds would have the data refused at every save, and every
// start compile again (NX2navi's 512 MB did that on an RTX 2080 Ti). The RAM, not what is free of it at the start: the
// same file is not read at one start and refused at the next.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <vector>

namespace pcache_bound {

inline constexpr uint64_t kLeastMb = 2048, kMostMb = 8192;
/// A read asks the C library for this much at a time
inline constexpr size_t kPiece = (size_t)64 << 20;

/// The bound in bytes, 0: none. env: RX_VK_PCACHE_MAX_MB (null: unset); ram_bytes: the PC's RAM, 0 when it is not known
/// (the least then). A value that is no number counts as unset, never as "no bound"
inline uint64_t bytes(const char* env, uint64_t ram_bytes) {
    if (env && *env) {
        char* end = nullptr;
        const unsigned long long mb = strtoull(env, &end, 10);
        if (end != env && !*end && env[0] != '-') return mb > (UINT64_MAX >> 20) ? 0 : (uint64_t)mb << 20;
    }
    return std::clamp<uint64_t>((ram_bytes >> 20) / 8, kLeastMb, kMostMb) << 20;
}

/// Whether a file or data of that size is within the bound (0: none)
inline bool within(uint64_t size, uint64_t bound) { return !bound || size <= bound; }

// ((Triforce, 11-18) The carry over an update. The rule of 11-14 left a file made for other shaders (an update that changed the
// translation) unread: right for the growth, but on AMD's driver, which keeps no cache of its own for these, it meant every
// pipeline and stage library compiled again after each such update: 87 minutes before the title on an RX 7900 XT (32 of them
// the 14,467 noted pipelines, 53 the 88,742 stage libraries). What the update did not change still matches the old file: of
// two caches a translator apart on the lab's PC, 39.6 % of the SPIR-V modules were the same (12.3 % of their bytes). So a file
// for other shaders is read all the same when it is no larger than the carry limit: the driver finds again what it compiled for
// the shaders that did not change, and the save writes old and new together for the new shaders. The limit stops the growth:
// a fresh file is 650-860 MB, one carried and saved again is over 1536 MB, and the next update's start leaves it unread, as
// before. RX_VK_PCACHE_CARRY_MB=<MB> sets the limit, 0: never carried (the rule of 11-14 alone))
inline constexpr uint64_t kCarryMb = 1536;
/// The carry limit in bytes (0: none carried). env: RX_VK_PCACHE_CARRY_MB (null or no number: the default)
inline uint64_t carry_limit(const char* env) {
    if (env && *env) {
        char* end = nullptr;
        const unsigned long long mb = strtoull(env, &end, 10);
        if (end != env && !*end && env[0] != '-') return mb > (UINT64_MAX >> 20) ? kCarryMb << 20 : (uint64_t)mb << 20;
    }
    return kCarryMb << 20;
}
/// Whether a file for other shaders (not same) of size bytes is read all the same: carried over the update. And only when it
/// leaves room under the bound (bound, 0: none) for as much again: the update's own pipelines go into the same file, and a save
/// over the bound is not written, so a carried file that left no room would have them compiled again at every start
/// And only on a PC with the RAM for it (ram_bytes, 0: not known, then never): the driver holds the carried data for the whole
/// session, up to 1-1.5 GB more than a fresh file, and on 16 GB PCs that already run short of RAM (logs that say so dozens of
/// times a session) that is paging and hitches in play. 31 GB and more: a 32 GB PC reports a little under 32
inline constexpr uint64_t kCarryRamGb = 31;
inline bool carried(bool same, uint64_t size, uint64_t limit, uint64_t bound, uint64_t ram_bytes) {
    return !same && limit && size > 32 && size <= limit && (!bound || size <= bound / 2) && ram_bytes >= (kCarryRamGb << 30);
}

enum class Read {
    Ok,
    None,        // (no such file: a first start)
    Unsized,     // (its length could not be asked)
    Short,       // (not longer than a pipeline cache's header)
    OverBound,   // (longer than the bound: not a byte of it read)
    NoMemory,    // (no block of its size)
    Cut,         // (the read ended early: nothing of it is used)
};

inline const char* why(Read r) {
    switch (r) {
    case Read::Unsized: return "its size cannot be asked";
    case Read::Short: return "it is shorter than a pipeline cache's header";
    case Read::OverBound: return "it is over the bound";
    case Read::NoMemory: return "there is no memory for a block of its size";
    case Read::Cut: return "reading it ended early";
    default: return "";
    }
}

/// The whole file into out, or nothing: out is empty unless the answer is Ok. *len: the file's length whenever it could be
/// asked, read or not. least: a file of at most this many bytes is refused; bound: one longer than this too (0: none)
inline Read read(const char* path, uint64_t least, uint64_t bound, std::vector<uint8_t>& out, uint64_t* len) {
    out.clear();
    *len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return Read::None;
#ifdef _WIN32
    int64_t n = _fseeki64(f, 0, SEEK_END) ? -1 : _ftelli64(f);
    if (n >= 0 && _fseeki64(f, 0, SEEK_SET)) n = -1;
#else
    int64_t n = fseeko(f, 0, SEEK_END) ? -1 : (int64_t)ftello(f);
    if (n >= 0 && fseeko(f, 0, SEEK_SET)) n = -1;
#endif
    Read r = Read::Ok;
    if (n >= 0) *len = (uint64_t)n;
    if (n < 0) r = Read::Unsized;
    else if ((uint64_t)n <= least) r = Read::Short;
    else if (!within((uint64_t)n, bound)) r = Read::OverBound;
    else if ((uint64_t)n > SIZE_MAX) r = Read::NoMemory;
    else {
        try {
            out.resize((size_t)n);
        } catch (const std::bad_alloc&) {
            r = Read::NoMemory;
        } catch (const std::length_error&) {
            r = Read::NoMemory;
        }
        size_t got = 0;
        while (r == Read::Ok && got < out.size()) {
            const size_t piece = std::min(out.size() - got, kPiece);
            const size_t had = fread(out.data() + got, 1, piece, f);
            got += had;
            if (had != piece) r = Read::Cut;
        }
    }
    fclose(f);
    if (r != Read::Ok) out.clear(), out.shrink_to_fit();
    return r;
}

}   // namespace pcache_bound
