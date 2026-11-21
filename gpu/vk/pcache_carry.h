// (Triforce) The pipeline cache's file carried over a stamp change at an update's preparation, capped (vk_draw.cpp pcache_init,
// pipeline_cache_save, pcache_carry_rebuild; vk_main.cpp's HOOKSHOT_PREPARE_ONLY start), apart from Vulkan so that
// tests/test_pcache_carry.py runs its rules.
//
// Why. mkw_vk_pipelines.bin is kept for one stamp (program_cache_stamp, written beside it as <file>.for: the rule of 11-14).
// Any change of the stamp threw the whole file away, and the stamp changes with every translator update, a Robust buffers
// toggle, a vk_bank.cpp edit no translation calls (436f9ac -> int2: sampler_bias_changed). The update's preparation then built
// the ~91,600 stage libraries and the precompiled pipelines into an empty cache: 5608 s for the warm on AMD 26.10.1 with its
// own cache wiped, 1868 s for the precompile, where a cache that has them takes 16.4 s and 4.4 s. The driver finds an entry by
// its own hash of the whole creation, so an entry of the old file whose shader did not change is a hit, and one whose shader
// did is never used, never wrong.
//
// Why the rule of 11-14 is there all the same: growth. With the old files always read, each translator update added its
// ~650 MB to pipelines of shaders that no longer exist, and a player's file reached 4.1 GB, "RAM short" in play. So:
//  - only at an update's preparation (HOOKSHOT_PREPARE_ONLY, Setup's "Preparing shaders"), only a file of this device and
//    driver (the header's vendor, device and pipelineCacheUUID, checked as before) within the file's bound (pcache_bound.h),
//    and only while the file is at most kCapNum/kCapDen (1.25) times its base;
//  - the base: the size of the last set built fresh, kept beside the file in <file>.base (Side) with the number of carries
//    since. A file with no side file of its own (one written before this rule, or under RX_VK_PCACHE_KEEP_OLD) is taken as
//    one fresh set: the rule of 11-14 kept it for one stamp. While a file has never been carried into, what play adds to it
//    is that set's too, and raises the base; once carried into, the base stays what it was, so the cap is the whole growth
//    over every carry since the set was built (cumulative, not a quarter more at each update);
//  - a carried session whose cache grows past what it may hold (limit: the cap, or the file's bound where that is lower, so
//    a carried cache never ends up over the bound unwritten) stops building into it as soon as that is seen (looked at every
//    kCheckMs while the preparation's workers build, and at each save; while the stage libraries are built, as soon as a
//    sample of them shows that the rate it grows at clearly takes it there: the early drop, below), does not write it, and
//    names this stamp in <file>.nocarry, so that a preparation begun again after a kill starts fresh rather than carrying the
//    same file again.
//    vk_main.cpp's preparation then builds the shaders' driver part again into a fresh cache (the compute pipelines the
//    carried run built and the stage libraries, from the program cache: nothing translated again), which is saved with a
//    new base. What the carried run compiled is in the driver's own cache by then, where the driver keeps one;
//  - a carried save writes the new stamp beside the file (unlike RX_VK_PCACHE_KEEP_OLD): it is these shaders' file now. So
//    the side file says too that the carried preparation is not done yet (Side::open): one that was stopped (Setup's Cancel,
//    the builder's stop after a long silence, a closed window) saved its merged cache under the new stamp, and the
//    preparation begun again would read it as these shaders' own file, uncapped, and build the rest of the warm into it with
//    only the bound to stop it. It is read as carried instead (resumed: capped, refused by .nocarry, built again into a fresh
//    cache past its limit), until a carried preparation goes through to its end (vk_main.cpp, pcache_carry_done). Its save
//    writes the side file before the data and the .for (begun), so that one cut between them is read so too;
// RX_VK_PCACHE_CARRY=0: no carry, the rule of 11-14 as it was. The play file (pcache_play.h) keeps its own rule: it is
// for one stamp, as before. A cache changes how long a pipeline takes to make, never the pipeline.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace pcache_carry {

inline constexpr uint64_t kCapNum = 5, kCapDen = 4;   // (1.25 times the base)
inline constexpr char kMagic[8] = {'T', 'F', 'P', 'C', 'B', 'A', 'S', 'E'};
inline constexpr size_t kSideBytes = 40;   // (magic 8, the stamp's 16 hex digits, base 8, carries 4, open 4)

/// RX_VK_PCACHE_CARRY: on unless set to 0
inline bool on(const char* env) { return !env || std::atoi(env) != 0; }

/// The most a file of that base may hold and still be carried into, or saved by a carried session
inline uint64_t cap(uint64_t base) {
    return base > UINT64_MAX / kCapNum ? UINT64_MAX : base * kCapNum / kCapDen;
}
inline bool fits(uint64_t size, uint64_t base) { return size <= cap(base); }
/// What a carried session's cache may hold: its cap, or the file's bound (pcache_bound.h, 0: none) where that is lower. Over
/// the bound it could not be written at all (pipeline_cache_save), and the update's preparation would end with the old file
inline uint64_t limit(uint64_t base, uint64_t bound) {
    const uint64_t c = cap(base);
    return bound && bound < c ? bound : c;
}
/// How often the preparation looks at a carried cache's size while its workers build (vk_draw.cpp pcache_carry_check)
inline constexpr unsigned kCheckMs = 10000;

// (Triforce, 11-18) The early drop (vk_draw.cpp warm_libraries, pcache_carry_early). Past its limit alone was seen only once it
// got there: on an RX 7900 XT an update carried a 1973 MB file built by an older translator, nearly every stage library missed
// (pre-raster 1.3-2.0 s each, fragment 0.18 s, as cold), and the cache reached 2469 MB of its 2467 after 2040 s and 36,906 of
// the ~91,600 libraries (16,018 of 42,532 programs): 34 minutes built into a cache that was then let go, before the fresh build
// began. So a carried warm builds a sample of the whole order first, and the cache's growth over it, per program of the sample,
// times the programs of the whole order, on top of what it held when they began, is where it ends at this rate (projected);
// clearly past its limit (drop_early), it is dropped then, as past it (pcache_carry_over, .nocarry, the rebuild into a fresh
// cache), a few minutes in rather than half an hour.
//  - the sample: every kSampleStride-th program of the warm's order (sample_order), built before the others, which wait for its
//    projection to begin (so that the growth looked at is the sample's alone), and looked at once. Not the order's head: the
//    warm takes the programs a state list loaded and those the notes name first (9,420 of 42,532 programs, some 20,000
//    libraries), the others in the program cache's file order, so that hits and misses come in stretches of it. That run had
//    one: 12,391 libraries between 960 and 1080 s at 0.17 s a pre-raster, as hits, then 5.2 s each. A head that misses
//    while the rest hits projected 4.5 times its growth (42,532 / 9,420), past any margin: dropped, and the ~70,000 libraries
//    that would have been hits compiled cold into the fresh cache. Every 32nd of the whole order follows such stretches as
//    they lie. The sample costs nothing built (the same programs, only sooner), but the notes' programs wait for it: some
//    1,330 programs, ~2,900 libraries, a few minutes cold, much less where they hit; and the workers idle while its last
//    programs are built;
//  - the margin: the projected growth over the warm at least kProjectNum/kProjectDen (2) times the room the cache had at its
//    start (its limit less its size then), for what the sample's libraries differ from the others in size. A carry that would
//    not fit and is not dropped early is dropped later all the same, once it is past (over); at least kProjectLibs stage
//    libraries in the sample, or none is made (a warm of under ~22,000 programs, whose cold build takes less time);
//  - the growth is counted from the cache's size when the libraries began (what prepare_run's compute pipelines added before
//    is in that size, and so out of the room). The compute pass is not projected (its programs done are translations, the
//    compute ones first, and nothing after them builds into the cache) and keeps the look past the limit alone, as do
//    precompile_run and the saves;
// RX_VK_PCACHE_CARRY_PROJECT=0: only once past the limit, and the warm in its own order, as before
inline constexpr uint64_t kSampleStride = 32;   // (the sample: every 32nd program of the warm's order)
inline constexpr uint64_t kProjectLibs = 1500;   // (and at least so many stage libraries built for it)
inline constexpr uint64_t kProjectNum = 2, kProjectDen = 1;   // (the margin: the projected growth over the room at the start)

/// RX_VK_PCACHE_CARRY_PROJECT: on unless set to 0
inline bool project_on(const char* env) { return !env || std::atoi(env) != 0; }

/// How many of the warm's total programs the sample takes: every kSampleStride-th, from the first
inline uint64_t sample_count(uint64_t total) { return total / kSampleStride + (total % kSampleStride != 0); }
/// The program of the warm's order (0 .. total-1) built at its claim c (c < total): the sample's first (c < sample_count, every
/// kSampleStride-th, in order), then the others in their order. Every program once
inline uint64_t sample_order(uint64_t c, uint64_t total) {
    const uint64_t m = sample_count(total);
    if (c < m) return c * kSampleStride;
    c -= m;
    return c / (kSampleStride - 1) * kSampleStride + c % (kSampleStride - 1) + 1;
}

/// Where a cache of now bytes, from bytes when the warm began, ends at the rate it grew with done of total programs done (the
/// growth per program done, times the programs left, on top of now). Nothing grown, nothing done or nothing left: now
inline uint64_t projected(uint64_t now, uint64_t from, uint64_t done, uint64_t total) {
    if (!done || now <= from || done >= total) return now;
    const double end = (double)now + (double)(now - from) * (double)(total - done) / (double)done;
    return end >= 18446744073709549568.0 ? UINT64_MAX : (uint64_t)end;
}
/// Whether a carried cache is dropped now on its projection, its sample built (done of the total programs, the sample's
/// libs stage libraries, now bytes): the sample large enough, it grew, programs are left, it had room under its limit at the
/// warm's start (limit: the cap, or the bound where lower), and the projected growth is at least the margin times that room.
/// Past the limit already, the growth may be under the margin: over says it
inline bool drop_early(uint64_t now, uint64_t from, uint64_t done, uint64_t total, uint64_t libs, uint64_t limit) {
    if (libs < kProjectLibs || from >= limit || now <= from || done >= total) return false;
    const uint64_t grown = projected(now, from, done, total) - from;
    return (double)grown * (double)kProjectDen >= (double)(limit - from) * (double)kProjectNum;
}

/// What <file>.base says: the stamp of the file it was written with (the .for's 16 hex digits), the base, the carries since,
/// and whether the file was saved by a carried preparation that has not gone through to its end (open: 1; a side file of
/// before this, whose 4 last bytes were spare and 0, is not)
struct Side {
    char stamp[16]{};
    uint64_t base = 0;
    uint32_t carried = 0;
    uint32_t open = 0;
};

inline void put(const Side& s, uint8_t out[kSideBytes]) {
    std::memset(out, 0, kSideBytes);
    std::memcpy(out, kMagic, 8);
    std::memcpy(out + 8, s.stamp, 16);
    std::memcpy(out + 24, &s.base, 8);
    std::memcpy(out + 32, &s.carried, 4);
    std::memcpy(out + 36, &s.open, 4);
}
/// False: not a side file (too short, another magic)
inline bool get(const uint8_t* in, size_t n, Side* s) {
    if (n < kSideBytes || std::memcmp(in, kMagic, 8)) return false;
    std::memcpy(s->stamp, in + 8, 16);
    std::memcpy(&s->base, in + 24, 8);
    std::memcpy(&s->carried, in + 32, 4);
    std::memcpy(&s->open, in + 36, 4);
    return true;
}

/// The file's history at a start: its side file when that was written with the file's own stamp (for: the .for's 16 digits,
/// null when there is none), else one fresh set of the file's size
inline Side history(bool side_read, const Side& side, const char* file_for, uint64_t file_size) {
    if (side_read && file_for && !std::memcmp(side.stamp, file_for, 16) && side.base) return side;
    Side s;
    if (file_for) std::memcpy(s.stamp, file_for, 16);
    s.base = file_size;
    return s;
}

/// Whether a file for another stamp is read at this start (carried): an update's preparation, the switch on, no carry refused
/// for these shaders already (refused: <file>.nocarry names this stamp), the file within its bound and at most cap(base) of
/// its history. The device's header is checked after, on its bytes
inline bool may_carry(bool on, bool preparing, bool refused, bool within_bound, uint64_t file_size, const Side& had) {
    return on && preparing && !refused && within_bound && file_size > 32 && had.base && fits(file_size, had.base);
}

/// Whether a file for these shaders (same: its .for names this stamp) is read as carried all the same at this start
/// (resumed): an update's preparation, the switch on, and its history open, saved by a carried preparation that did not go
/// through to its end. Read with the history it has (Start::Same: no carry counted again), capped as the carried run was;
/// refused (<file>.nocarry names this stamp: that run went past its limit), it is not read at all, and the cache starts fresh
inline bool resumed(bool on, bool preparing, bool same, const Side& had) {
    return on && preparing && same && had.open != 0;
}

/// Whether the side file says a carried session of these shaders (stamp: this stamp's 16 digits) began a save that did not
/// get as far as the .for (file_for: its 16 digits, null when there is none): open, named for these shaders, while the .for
/// names another stamp or none. A carried save writes the side file first, then the data, then the .for (vk_draw.cpp
/// pipeline_cache_save), so that a save cut anywhere (a crash, a power cut, a kill in a checkpoint of warm_checkpoint.h)
/// leaves the session's own history: the file there is the old one or the merged one, both within that session's limit.
/// Read with that history as a file of these shaders whose carried preparation is not done (resumed), never as one fresh set
/// of its size, uncapped
inline bool begun(bool side_read, const Side& side, const char* stamp, const char* file_for) {
    return side_read && side.open && side.base && !std::memcmp(side.stamp, stamp, 16) &&
           (!file_for || std::memcmp(file_for, stamp, 16));
}

/// ((Triforce, 11-19) Whether v0.5.2's carry outside Setup's preparation (pcache_bound.h carried: a file for other shaders
/// read whole by its size and the PC's RAM) is asked at all: not at the preparation (may_carry's), not for a file read as
/// carried or resumed already, not when a carry was refused for these shaders (refused: <file>.nocarry names them. So
/// warm_libraries leaves a file it dropped for what it held that nothing asks for, its .for gone: read again, its cache would
/// be over the bound again and not written, and the stage libraries compiled at their first draw every session), and not for
/// a carried preparation's save cut short (begun: its own capped history, read as resumed)
inline bool plain_carry(bool preparing, bool carried, bool resumed, bool refused, bool begun) {
    return !preparing && !carried && !resumed && !refused && !begun;
}

/// The session's history from the start: fresh (nothing read), the file's own (read for these shaders), or one carry more
/// (read across a stamp change: open until its preparation goes through to its end)
enum class Start { Fresh, Same, Carried };
inline Side session(Start how, const Side& had) {
    Side s;
    if (how == Start::Fresh) return s;
    s = had;
    if (how == Start::Carried) s.carried = had.carried + 1, s.open = 1;
    return s;
}

/// Whether a session's cache of size bytes is past what it may hold: a carried session's over its limit (the cap, or the bound
/// where lower). The others never (the bound alone, as before: pipeline_cache_save)
inline bool over(bool carried_session, uint64_t size, const Side& s, uint64_t bound) {
    return carried_session && size > limit(s.base, bound);
}
/// Whether a save of size bytes is written as far as the carry goes: not when it is over
inline bool save_ok(bool carried_session, uint64_t size, const Side& s, uint64_t bound) {
    return !over(carried_session, size, s, bound);
}

/// The history once size bytes are saved under stamp: a file never carried into is one set, so its base follows it (the
/// largest saved); one carried into keeps its base. Open stays as the session has it
inline Side saved(const Side& s, uint64_t size, const char* stamp) {
    Side n = s;
    std::memcpy(n.stamp, stamp, 16);
    if (!n.carried && size > n.base) n.base = size;
    return n;
}

}   // namespace pcache_carry
