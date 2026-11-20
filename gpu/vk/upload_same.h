// (Triforce, 11-16) A texture the CPU keeps rewriting: its re-upload checked against the bytes its image already holds
// (vk_texture.cpp texture_touch, same_look and same_done), apart from Vulkan so that tests/test_upload_same.py can run it.
//
// After some tours of the lab's scenario S2 the Market ran at 62 to 78 fps against 83 to 96, p95 27 to 35 ms against 12 to 15,
// with about 26 GB of uploads in its stay (round36, r36-t29-tour: 25,996 MB). The game rewrites, from the CPU, the storage of a
// few big block-compressed textures the Market samples: 1528 x 920 x 3 and 1120 x 1516 x 3 (BC7 and NVN 0x50), 2240 x 3032 and
// 2288 x 6112 (BC5), 4 to 13 MB each, the same four to six in every bad tour (r27, r33, r36, r37), at two to six places each at
// once and at new places now and then, each place written whole again every 4 to 5 frames, and with the same bytes each time:
// the written part of each of them had one count of bytes not zero at every place and every time in those logs, and
// RX_VK_STAMP_SRC=3 (r29-stamp-tour) said "bytes THE SAME as at its last upload" of them. (Who writes them is the game's: the
// frame-stability notes' guess is its texture data manager, which has a compaction thread, moving or re-streaming them in a
// heap the tour left fragmented; a clean tour's Market has none of it from its start.) Every 4 to 5 frames 12 to 15 uploads
// of 40 to 60 MB came in one frame (13% of the frames, 27 ms against 12: frames.csv), most in the parallel replay's segments,
// under the recording lock. RX_VK_TEX_SAME (vk_memory.cpp take_capture) leaves out a page whose bytes are those of its last
// capture's copy, and here about half of the pages passed through other bytes between two captures (in r36-t29's Market,
// 1.42 million pages found the same and 1.49 million changed in 1000 frames: the shadow staging line's copies "from the capture
// buffer" are these pages), so they were stamped and the textures over them uploaded whole again, with the bytes they had.
//
// So a texture uploaded again keeps a hash of what its image got, unit by unit of its storage as its upload reads it (a level's
// and layer's GOB-block row where the upload goes by rows, else the level's and layer's whole span: a 1528 x 920 x 3 BC7
// texture is 6 rows). A later write over it uploads only the units whose bytes hash otherwise now, and nothing when none does.
// The hashes are of exactly what the image got: the upload that keeps them reads the copy that was hashed (the units' bytes
// as submitted, vk_memory.cpp memory_visit_as_submitted, copied before the upload), and any other change to the image (an
// upload not made so, a GPU write: changed()) forgets them all until such an upload again. So bytes the image does not hold
// are never taken for its own (but for a 64-bit hash's collision): a page that went A, B, A while it was uploaded cannot leave
// B in the image under A's hash.
// What it costs: the units the writes reached read once more, and hashed (17 to 20 GB/s, content_hash below), but a page the
// submit-time capture keeps a copy of (the textures' pages the game wrote since a recent submit) is hashed once and its hash kept
// with the copy until a capture copies other bytes into it (vk_memory.cpp memory_hash_as_submitted; RX_VK_UPLOAD_SAME_KEEP=0:
// every byte hashed at every look); and an upload reads the copy instead of guest memory.
// Only for a texture uploaded before (area loads are new textures), of kMaxBytes or less, whose upload is the CPU's (vk_texture.cpp
// same_wanted), or (Triforce, 11-16) ASTC of a 2D image decoded on the GPU (upload_astc_gpu): there a unit is a layer from its first
// kept level on, what that upload stages of a layer a write reached, and its staging reads the copy too. At the Market after a tour
// the game rewrote such textures with the bytes they had as well (1536 x 1536 ASTC 5x5 of 11 levels, 768 x 768 kept as BC3,
// 512 x 512 ASTC), staged whole and decoded again unchecked: about 1.4 GB in 10 s, 90 fps down to 72, the 1% low from 57 to 33.
// RX_VK_UPLOAD_SAME_ASTC=0: those as before. One whose checks find nothing the same kMisses times in a row is uploaded as
// before for its next kRest re-uploads (the CPU really rewrites it), then checked again. NX2navi has the whole texture's hash as uploaded
// (NX2NAVI_TEX_SAME_BIG, its vk_texture.c same_content): its notes measured the same thing in a run through Hyrule Field, 44,000
// to 52,000 second uploads of 33 to 47 GiB down to 592 of 195 MiB. RX_VK_UPLOAD_SAME=0: every re-upload as before.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace upload_same {

inline constexpr uint64_t kMaxBytes = 16ull << 20;   // (the storage at most: the bursts' textures were 4 to 13.6 MB)
inline constexpr uint32_t kMisses = 4;               // (checks in a row that found every unit they knew changed:
inline constexpr uint32_t kRest = 64;                // then this many re-uploads as before, unchecked)
inline constexpr uint64_t kPage = 4096;              // (a unit's hash is folded from its parts at these pages)

/// A hash of n bytes (NX2navi's content_hash, runtime/vk_texture.c, chrg6, its commits 42ae87bc and a389f245): eight chains
/// side by side over 64 bytes a step, folded into one. A chain's multiply waits for the one before it, so the chains, which
/// wait for nothing of each other, read eight times what one reads in the same time; a step is a bijection of its chain for
/// the word it takes and of the word for the chain, as is each fold. Its notes measured 17 to 20 GB/s on textures read from
/// memory (Ryzen 7 9850X3D, clang -O2), one chain 7
inline uint64_t content_hash(const void* p, uint64_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t h = 0x9e3779b97f4a7c15ull ^ n, i = 0;
    if (n >= 512) {
        uint64_t c[8];
        for (int k = 0; k < 8; k++) c[k] = h ^ (0xc2b2ae3d27d4eb4full * (uint64_t)(k + 1));
        for (; i + 64 <= n; i += 64) {
            uint64_t w[8];
            std::memcpy(w, b + i, 64);
            for (int k = 0; k < 8; k++) {
                c[k] = (c[k] ^ w[k]) * 0xbf58476d1ce4e5b9ull;
                c[k] ^= c[k] >> 29;
            }
        }
        for (int k = 0; k < 8; k++) {
            h = (h ^ c[k]) * 0x94d049bb133111ebull;
            h ^= h >> 29;
        }
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, b + i, 8);
        h = (h ^ w) * 0xbf58476d1ce4e5b9ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 0x94d049bb133111ebull;
    return h ^ (h >> 31);
}

/// A unit's hash, made from its parts in order: each part is what lies within one kPage page of the GPU address (where a
/// capture's copy of a page begins and ends, memory_visit_as_submitted), hashed by content_hash, folded. Never 0 (0 is "not
/// known")
struct UnitHash {
    uint64_t h;
    explicit UnitHash(uint64_t n) : h(0x6a09e667f3bcc909ull ^ n) {}
    void add(const uint8_t* p, uint64_t n) { add_hash(content_hash(p, n)); }
    /// (a part whose content_hash is known already: a page a kept capture holds, vk_memory.cpp memory_hash_as_submitted)
    void add_hash(uint64_t part) {
        h = (h ^ part) * 0x9e3779b97f4a7c15ull;
        h ^= h >> 32;
    }
    uint64_t done() const { return h ? h : 1; }
};
/// The hash of n bytes at p that lie at GPU address `at`, split into its pages' parts as above
inline uint64_t hash_at(const uint8_t* p, uint64_t at, uint64_t n) {
    UnitHash u(n);
    for (uint64_t o = 0; o < n;) {
        const uint64_t e = std::min(n, (at + o) / kPage * kPage + kPage - at);
        u.add(p + o, e - o);
        o = e;
    }
    return u.done();
}

using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;   // (byte ranges from the storage's start)

inline bool reaches(const Ranges& r, uint64_t a, uint64_t b) {
    for (const auto& x : r)
        if (x.first < b && a < x.second) return true;
    return false;
}

/// A part of the storage the upload reads whole when it reads any of it, from the storage's start
struct Unit {
    uint64_t lo = 0, hi = 0;
    uint64_t hash = 0;   // (of what the image holds there, as uploaded; 0: not known)
};
/// The units of one level and layer at lo, span bytes long: steps of `step` bytes (its GOB-block rows; the span itself for a
/// level and layer uploaded whole), each cut at the span's and at the storage's end (total); none from the storage's end on
inline void add_units(std::vector<Unit>& u, uint64_t lo, uint64_t span, uint64_t step, uint64_t total) {
    if (!span || !step) return;
    for (uint64_t a = lo; a < lo + span && a < total; a += step) u.push_back(Unit{a, std::min({a + step, lo + span, total}), 0});
}
/// In storage order (the levels' and layers' units come level by level; the upload's ranges are asked in order)
inline void sort_units(std::vector<Unit>& u) {
    std::sort(u.begin(), u.end(), [](const Unit& a, const Unit& b) { return a.lo != b.lo ? a.lo < b.lo : a.hi < b.hi; });
}

/// (Triforce, 11-16) The uploads into the image recorded in one submission (upload(): every one), for the parallel replay's twin
/// (twin_ranges below): what they asked for, the image they went into
struct SegUploads {
    uint64_t value = 0;   // (the submission they were recorded in; 0: none yet)
    uint64_t image = 0;   // (the image the last of them went into)
    bool whole = false;   // (one of them was of the whole storage, or the image was made again meanwhile)
    Ranges ranges;        // (else what they asked for, in order, merged)
};
struct State {
    std::vector<Unit> units;   // (made at the first check: a texture's layout does not change)
    uint32_t content = 0;      // (counts the changes to the image's bytes: every upload, every GPU write; changed())
    uint32_t known = 0;        // (the count the units' hashes go with: another, and they are forgotten at the next check)
    uint32_t misses = 0;       // (checks in a row that found every unit they knew changed)
    uint32_t rest = 0;         // (re-uploads still to make unchecked, after kMisses such checks)
    SegUploads seg;            // ((Triforce, 11-16) this submission's uploads into it, for the parallel replay's twin)
};
/// The image's bytes may have changed otherwise than by a checked upload (any upload, a GPU write): its units' hashes go
inline void changed(State& s) { s.content++; }
/// Whether this re-upload is checked: not while the texture rests after kMisses checks that found nothing the same
inline bool take(State& s) {
    if (s.rest) {
        s.rest--;
        return false;
    }
    return true;
}

struct Verdict {
    uint64_t same = 0;      // (bytes of the units the writes reached that hold what the image holds)
    uint64_t known = 0;     // (bytes of those whose hash was known: the ones looked at)
    uint64_t changed = 0;   // (bytes of the units to upload)
};
/// A re-upload whose writes reached `dirty`: of the units dirty reaches, one whose bytes hash now (hash_now(unit)) as its
/// kept hash is the image's already and is left out; the others' indices go into `upload`, in order. A unit whose hash is not
/// known is uploaded without a look. Hashes kept for another content of the image than now are forgotten first
template <class HashNow>
Verdict check(State& s, const Ranges& dirty, HashNow&& hash_now, std::vector<uint32_t>& upload) {
    upload.clear();
    if (s.known != s.content) {
        for (Unit& u : s.units) u.hash = 0;
        s.known = s.content;
    }
    Verdict v;
    for (uint32_t i = 0; i < (uint32_t)s.units.size(); i++) {
        const Unit& u = s.units[i];
        if (!reaches(dirty, u.lo, u.hi)) continue;
        if (u.hash) {
            v.known += u.hi - u.lo;
            if (hash_now(u) == u.hash) {
                v.same += u.hi - u.lo;
                continue;
            }
        }
        v.changed += u.hi - u.lo;
        upload.push_back(i);
    }
    if (v.same) {
        s.misses = 0;
    } else if (v.known && ++s.misses >= kMisses) {
        s.misses = 0;
        s.rest = kRest;
    }
    return v;
}
/// What the upload of those units is asked for: their ranges, in order, merged
inline void ranges_of(const State& s, const std::vector<uint32_t>& units, Ranges& out) {
    out.clear();
    for (uint32_t i : units) {
        const Unit& u = s.units[i];
        if (!out.empty() && u.lo <= out.back().second) out.back().second = std::max(out.back().second, u.hi);
        else out.push_back({u.lo, u.hi});
    }
}
/// The units an upload asked for `dirty` reads: every one dirty reaches (the upload reads a level and layer, or a row, whole)
inline void reached(const State& s, const Ranges& dirty, std::vector<uint32_t>& out) {
    out.clear();
    for (uint32_t i = 0; i < (uint32_t)s.units.size(); i++)
        if (reaches(dirty, s.units[i].lo, s.units[i].hi)) out.push_back(i);
}
/// After the upload of a check: `read` the units it read, `hashes` the hashes of the copy it read them from (in read's
/// order), from_copy whether it read the copy alone. When that upload is the one change to the image since the check (it
/// counts one: changed()) and it read the copy alone, those hashes are the image's, and the units it did not read keep theirs.
/// Otherwise (it failed or read guest memory somewhere, or something else changed the image meanwhile) every hash goes
inline void commit(State& s, const std::vector<uint32_t>& read, const std::vector<uint64_t>& hashes, bool from_copy) {
    if (!from_copy || s.content != s.known + 1 || read.size() != hashes.size()) {
        s.known = s.content + 1;   // (another count than the content's: forgotten at the next check)
        return;
    }
    for (size_t k = 0; k < read.size(); k++) s.units[read[k]].hash = hashes[k];
    s.known = s.content;
}

// ---- (Triforce, 11-16) the parallel replay's twin (vk_texture.cpp texture_touch, same_twin_look and same_twin_done) ----
// A parallel replay records a submission's segments at once, each its own uploads (submitted just before its own commands) and
// its own draws, and submits them in stream order. A texture is looked at once a submission (checked_epoch), by whichever
// segment's worker reaches it first; when that is a later segment's, its upload lands after the earlier segments' draws in the
// queue, and an earlier segment that samples it would see the image as it was before this submission. So the earlier segment
// uploads it again in its own uploads (the twin): the bytes as submitted, which its draws are to see.
// The twin uploaded the whole storage, and as an upload not checked it forgot the kept hashes (changed()): the later segment's
// checked upload had just made them the image's, and the next rewrite of the same bytes found none known, uploaded every unit it
// reached again, and so on, the twin with it, every few frames (round46: about 97% of what was still uploaded unchecked in the
// loops at the S2 Market after a tour, up to 6.9 GB in one stay). Now (RX_VK_TWIN_SAME, on unless 0):
// 1. The twin uploads only what this submission's uploads into the image asked for (SegUploads, noted by upload() itself), and
//    so writes what they write: an upload of the same ranges into the same image reads the same units (or layers). Outside them
//    neither writes, and the earlier segment's draws see there what the later segment's do: the image as it was, which the write
//    tracking a partial re-upload goes by says is the bytes as submitted. The whole storage when one of those uploads was whole,
//    or the image was made again in this submission, by one of them or since (an image made again holds nothing before the later
//    segment's upload: the earlier draws need all of it).
// 2. Where the units it reads all have kept hashes, the twin copies them as submitted, hashing the copy, and uploads from the
//    copy (same_twin_look, the same copy same_look makes). When the copy's hashes are the kept ones, the twin wrote the bytes the
//    hashes are of, the later segment's upload (after it in the queue) writes those bytes again, and the image ends up as the
//    hashes say: they are kept (twin_commit). Anything else (other bytes, a read past the copy, a failed upload, another change
//    meanwhile), and they are forgotten, as before.
// RX_VK_TWIN_SAME=0: the whole storage again, the hashes forgotten, as before.

/// An upload into the image, recorded in submission `value`: `part` what it asked for (null: the whole storage); image0 and image
/// the image before and after it (another after it: made again by it, the whole storage). For a checked upload `part` is what
/// same_look left of the writes: the ranges of the units found changed alone (ranges_of), so the note, and the twin with it,
/// holds only those; the units the check found the same are the image's already, before the twin as after it
inline void note_upload(SegUploads& n, uint64_t value, uint64_t image0, uint64_t image, const Ranges* part) {
    if (n.value != value) {
        n.value = value, n.whole = false, n.ranges.clear();
        n.image = image0;
    }
    if (n.image != image0 || image0 != image || !part) n.whole = true;   // (the image made again since, or by it; or all of it)
    n.image = image;
    if (n.whole) {
        n.ranges.clear();
        return;
    }
    for (const auto& r : *part)
        if (r.first < r.second) n.ranges.push_back(r);
    std::sort(n.ranges.begin(), n.ranges.end());
    Ranges m;
    for (const auto& r : n.ranges)
        if (!m.empty() && r.first <= m.back().second) m.back().second = std::max(m.back().second, r.second);
        else m.push_back(r);
    n.ranges.swap(m);
}
/// What the twin of submission `value` uploads again into `image`: what that submission's uploads into it asked for; null (the
/// whole storage) when none is noted for it, one of them was whole, or the image is not the one they went into
inline const Ranges* twin_ranges(const SegUploads& n, uint64_t value, uint64_t image) {
    if (n.value != value || n.whole || n.image != image || n.ranges.empty()) return nullptr;
    return &n.ranges;
}
/// Whether the twin is worth a copy: it reads a unit (`read`: the units its upload reads), and each has a hash kept for the image
/// as it is now (no change to the image since they were kept)
inline bool twin_known(const State& s, const std::vector<uint32_t>& read) {
    if (read.empty() || s.known != s.content) return false;
    for (uint32_t i : read)
        if (i >= s.units.size() || !s.units[i].hash) return false;
    return true;
}
/// After the twin's upload (upload() counted it: changed()): `read` the units it read, `hashes` its copy's, from_copy whether it
/// read the copy alone and succeeded. The kept hashes stay when that upload is the one change since they were kept and the copy
/// held, unit by unit, the bytes they are of; otherwise they are forgotten. True when kept
inline bool twin_commit(State& s, const std::vector<uint32_t>& read, const std::vector<uint64_t>& hashes, bool from_copy) {
    bool keep = from_copy && s.content == s.known + 1 && !read.empty() && read.size() == hashes.size();
    for (size_t k = 0; keep && k < read.size(); k++)
        keep = read[k] < s.units.size() && s.units[read[k]].hash && s.units[read[k]].hash == hashes[k];
    if (!keep) {
        s.known = s.content + 1;   // (forgotten at the next check)
        return false;
    }
    s.known = s.content;
    return true;
}

/// The copy an upload of a check reads (vk_texture.cpp upload_inner, same_snap_read): bytes at the storage's offsets, and the
/// ranges filled. A read it does not cover is read from guest memory as before, and marks the copy short (commit forgets)
struct Copy {
    const uint8_t* bytes = nullptr;
    uint64_t total = 0;    // (the storage's size: nothing is read past it)
    Ranges filled;         // (in order, merged)
    bool used = false, short_read = false;
};
/// The copy's bytes for [off, off + n), or null (and the copy short) when it does not hold all of them
inline const uint8_t* copy_read(Copy& c, uint64_t off, uint64_t n) {
    if (!n) return nullptr;
    if (off + n > c.total || off + n < off) {
        c.short_read = true;
        return nullptr;
    }
    for (const auto& r : c.filled)
        if (r.first <= off && off + n <= r.second) {
            c.used = true;
            return c.bytes + off;
        }
    c.short_read = true;
    return nullptr;
}

}  // namespace upload_same
