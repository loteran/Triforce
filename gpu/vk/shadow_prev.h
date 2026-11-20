// (Triforce) The texture shadow's pages that a capture takes again with bytes the shadow holds already (vk_memory.cpp
// take_capture's shadow loop), apart from Vulkan so that tests/test_shadow_prev.py runs the same steps.
//
// The shadow (vk_memory.cpp ShadowPage) is a 4 KB copy of each texture storage page the last capture took, which the
// uploads read (memory_as_submitted) and the re-upload checks hash (memory_hash_as_submitted, its hash kept with the copy
// until a capture copies other bytes into it: upload_same.h). After some tours of the lab's S2 the game rewrites the Market's
// textures every few frames, and about half of the pages a capture takes hold the bytes of their copy (RX_VK_TEX_SAME: no
// upload for them) and the other half other bytes, which the next change takes back (round46: as many pages the same as
// changed, to the page, in every looping tour, and the checks after them found the units the same: 2.5 GB looked at against
// 12 MB uploaded in tour-1's last window). take_capture copied every one of them into the shadow again, about 400 pages a
// take, six takes a frame, and a page copied with other bytes lost the hash a check had kept with it, so the check after the
// change back hashed it again (the checks' 0.2 to 0.3 ms a frame there; the take's "retires and stamps" 0.13 ms against
// 0.016 in a clean tour).
//
// Now, for a page tex_same compared (one with a copy, not a late page found the same):
//   - RX_VK_SHADOW_KEEP: one whose copy holds the capture's bytes (tex_same's memcmp, SAME) keeps it as it is: no copy,
//     its hash kept, as the copy over it with the same bytes kept it before;
//   - RX_VK_SHADOW_PREV: one found with other bytes keeps the copy it had and that copy's hash (kept with the page:
//     its buffer is the one the new bytes go into, CHANGED), and one whose capture holds the bytes of that kept copy (memcmp
//     against it, BACK) takes it back, hash and all: no copy, and the check after it finds the hashes kept. A kept copy that
//     came back none of kMisses changes in a row goes, and the page keeps none for its next kRetry changes; one not given back
//     within kStale takes (a page that changed once and is then taken with the same bytes: its copy kept for nothing, the
//     slot's seq refreshed by every take so the sweeps leave it) goes at its next take that looks at it, or at the sweep every
//     kSweep takes (age); at most kCapPages are kept (64 MB).
// Both only while the page is the capture's once and its slot holds the buffers the look compared (act); any other page is
// copied as before. Exact: after the take the shadow holds what the capture holds in every case (each skip is a memcmp
// equal), a hash is content_hash of the bytes it is kept with or 0, and the stamps, the capture entries, the mirrors and the
// reads as submitted are what they were. =0 each: as before (RX_VK_SHADOW_CHECK=1: each page left uncopied compared with
// the capture's copy again after the take, and the hashes recomputed: the shadow line's "check" counts must stay 0).
//
// ((Triforce, 11-17) RX_VK_SHADOW_VERS: the copies a page keeps, not one. A page whose captures bring more than two versions
// (A, B, C, A every few frames) is given back its copy only when the version it brings is the one before the last (BACK
// above): the third one, A again, is other bytes and copied, and so is every such page's every third take. The kept copies
// are the page's last few versions then, newest first: a capture that brings the bytes of any of them takes that one back
// and the copy it had becomes the newest (back), so every version a page cycles through is copied once and given back after.
// A capture's bytes are compared with the kept copies newest first while they were kept (look, one memcmp each), so at
// RX_VK_SHADOW_VERS=1 (the default, as before) the one copy of PREV is compared exactly as it was; 2 to 4 keep that many, at
// 4 KB each, under the same kCapPages. keep_old keeps the copy the page had as the newest and the oldest goes back to the
// copy's buffer (no allocation at a ring full); a version not given back within kStale takes goes as one does; and the misses
// and the kRetry are the page's, not a version's: a page whose versions none came back for kMisses changes lets them all go
// and keeps none for kRetry changes. A cycle longer than the copies kept still copies: only the versions still among them
// come back. The lab's A/B of it, and the counts that say how many pages were given a copy two or three versions back
// (vk_memory.cpp's shadow line), in the owner's reports folder, triforce-loop-versions.md.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace shadow_prev {

inline constexpr uint64_t kPage = 4096;
inline constexpr int64_t kCapPages = 16384;   // (kept copies at most: 64 MB beside the shadow's 256, any number of versions)
inline constexpr int kMaxVers = 4;            // (copies beside the page's own, at most: RX_VK_SHADOW_VERS's range)
inline constexpr uint8_t kMisses = 8;         // (changes in a row without the kept copy coming back: it goes,
inline constexpr uint8_t kRetry = 32;         // and none is kept for this many changes of the page)
inline constexpr uint64_t kStale = 64;        // (takes: a kept copy not given back within this many goes, age)
inline constexpr uint64_t kSweep = 256;       // (takes between the sweeps that age every kept copy: vk_memory.cpp take_capture)
inline constexpr int kVersDefault = 1;        // (RX_VK_SHADOW_VERS unset: one copy, as before)

/// RX_VK_<name>: on unless set to a number that is 0 (as RX_VK_TEX_SAME)
inline bool env_on(const char* name) {
    const char* e = getenv(name);
    return !(e && !atoi(e));
}

/// RX_VK_SHADOW_VERS: the copies a page keeps beside its own (1 to kMaxVers; unset, empty or below 1: kVersDefault; more: kMaxVers)
inline int env_vers() {
    const char* e = getenv("RX_VK_SHADOW_VERS");
    if (!e || !*e) return kVersDefault;
    const int v = atoi(e);
    return v < 1 ? kVersDefault : v > kMaxVers ? kMaxVers : v;
}

/// The copies a page keeps, as the number the steps below compare and keep (1 to kMaxVers)
inline int vers_count(int vers) { return vers < 1 ? 1 : vers > kMaxVers ? kMaxVers : vers; }

/// The kept copies alive (each one with a buffer counts once)
inline std::atomic<int64_t>& alive() {
    static std::atomic<int64_t> n{0};
    return n;
}

/// A page's copy before one of its changes, with that copy's hash (content_hash of b, or 0: none taken)
struct Prev {
    std::unique_ptr<uint8_t[]> b;
    uint64_t hash = 0;
    uint64_t at = 0;   // (the take, shadow_seq, that kept it or gave it back; age)
    Prev() = default;
    Prev(const Prev&) = delete;
    Prev& operator=(const Prev&) = delete;
    Prev(Prev&& o) noexcept : b(std::move(o.b)), hash(o.hash), at(o.at) { o.hash = 0, o.at = 0; }
    Prev& operator=(Prev&& o) noexcept {
        if (this != &o) drop(), b = std::move(o.b), hash = o.hash, at = o.at, o.hash = 0, o.at = 0;
        return *this;
    }
    ~Prev() { drop(); }
    void drop() {
        if (b) b.reset(), alive().fetch_sub(1, std::memory_order_relaxed);
        hash = 0;
    }
};

/// A page's last few versions, newest first (RX_VK_SHADOW_VERS): p[0..n-1] with a buffer each, the misses and the retry the
/// page's (as one Prev's were: changes since a copy came back, and changes left before one is kept again)
struct Vers {
    Prev p[kMaxVers];
    uint8_t n = 0;        // (kept now, newest first)
    uint8_t misses = 0;   // (with n: changes since one came back; without: changes left before one is kept again)
    Vers() = default;
    Vers(const Vers&) = delete;
    Vers& operator=(const Vers&) = delete;
    Vers(Vers&& o) noexcept { *this = std::move(o); }
    Vers& operator=(Vers&& o) noexcept {
        if (this != &o) {
            for (int k = 0; k < kMaxVers; k++) p[k] = std::move(o.p[k]);   // (each buffer's count moves with it)
            n = o.n, misses = o.misses;
            o.n = 0, o.misses = 0;
        }
        return *this;
    }
};

/// The newest copy's bytes (nullptr: none)
inline const uint8_t* newest(const Vers& v) { return v.n ? v.p[0].b.get() : nullptr; }

/// Let go of every copy the page keeps (its misses and its retry are the caller's)
inline void drop_all(Vers& v) {
    for (int k = 0; k < (int)v.n; k++) v.p[k].drop();
    v.n = 0;
}

enum : uint8_t {
    NONE = 0,      // (not compared, or not to be left: copied as before)
    SAME = 1,      // (its copy holds the capture's bytes: left as it is)
    CHANGED = 2,   // (other bytes: its copy kept as the page's newest version, then copied over)
    BACK = 3,      // (the bytes of one of its versions: that one taken back, no copy)
};

/// tex_same's look at a page that has a copy: what it found, the buffers it compared (for act), and which kept version
/// holds the capture's bytes (BACK: 0 the newest, 1 the one before it...)
struct Look {
    uint8_t v = NONE;
    const uint8_t* b = nullptr;
    const uint8_t* prev = nullptr;
    uint8_t depth = 0;
    uint64_t at = 0;   // (BACK: the take that kept that version: with depth, the version's identity)
};

/// same: tex_same's memcmp of the copy b against the capture's copy cap found them equal. A changed page whose kept
/// versions hold cap's bytes is BACK, at the newest that does (prev_on only); cap is read only then
inline Look look(bool same, const uint8_t* b, const Vers& v, const uint8_t* cap, bool prev_on, int vers) {
    Look l{same ? SAME : CHANGED, b, newest(v), 0};
    if (!same && prev_on && cap) {
        const int want = vers_count(vers);
        const int n = want < (int)v.n ? want : (int)v.n;
        for (int d = 0; d < n; d++)
            if (v.p[d].b && std::memcmp(v.p[d].b.get(), cap, kPage) == 0) {
                l.v = BACK, l.depth = (uint8_t)d, l.prev = v.p[d].b.get(), l.at = v.p[d].at;
                break;
            }
    }
    return l;
}

/// What the shadow's loop does with the page now: SAME and BACK only while the slot still holds the buffers the look
/// compared (b_now, and for BACK the version's) and the page is the capture's once (twice: its copies may hold other bytes
/// than the one compared); CHANGED (prev_on) when it still has a copy; else NONE (copied as before). BACK's version is
/// compared by its buffer and the take it was kept at: a buffer given back into the copy and filled with other bytes, then
/// kept again, is not the version the look compared
inline uint8_t act(const Look& l, bool keep_on, bool prev_on, bool twice, const uint8_t* b_now, const Vers& v_now) {
    if (l.v == NONE || twice || !b_now || b_now != l.b) return NONE;
    if (l.v == SAME) return keep_on ? SAME : NONE;
    if (!prev_on) return NONE;
    if (l.v == BACK && l.depth < v_now.n && v_now.p[l.depth].b.get() == l.prev && v_now.p[l.depth].at == l.at) return BACK;
    return CHANGED;
}

/// BACK at take now, depth d: that version holds the capture's bytes: it and its hash become the page's copy, the copy it
/// had and its hash the newest version (given back: the page's misses and the newest's age start again)
inline void back(std::unique_ptr<uint8_t[]>& b, uint64_t& hash, Vers& v, uint8_t d, uint64_t now) {
    if (d >= v.n || !v.p[d].b) return;
    std::swap(b, v.p[d].b);
    std::swap(hash, v.p[d].hash);
    Prev took = std::move(v.p[d]);   // (the bytes the page had, now a version)
    for (int k = (int)d; k > 0; k--) v.p[k] = std::move(v.p[k - 1]);
    v.p[0] = std::move(took);
    v.p[0].at = now;   // (it was the page's copy until this take)
    v.misses = 0;
}

/// CHANGED at take now, before the capture's bytes are copied into b: b's bytes and hash kept as the newest version, b
/// given the oldest version's buffer (or a new one while fewer than the cap are kept), b's hash 0 (the copy brings other
/// bytes). False when none is kept: b is copied over as before (its hash let go by the loop)
inline bool keep_old(std::unique_ptr<uint8_t[]>& b, uint64_t& hash, Vers& v, int64_t cap, uint64_t now, int vers) {
    if (!b) return false;
    if (v.n) {
        if (++v.misses >= kMisses) {   // (none comes back: they all go, and none kept for a while)
            drop_all(v);
            v.misses = kRetry;
            return false;
        }
    } else if (v.misses) {
        v.misses--;
        return false;
    }
    const int maxn = vers_count(vers);
    std::unique_ptr<uint8_t[]> fresh;
    if ((int)v.n >= maxn) {   // (the oldest's buffer is the copy's: the ring is never more than maxn)
        fresh = std::move(v.p[maxn - 1].b);
        v.n = (uint8_t)(maxn - 1);
    } else {
        if (alive().load(std::memory_order_relaxed) >= cap) return false;
        fresh.reset(new (std::nothrow) uint8_t[kPage]);
        if (!fresh) return false;
        alive().fetch_add(1, std::memory_order_relaxed);
    }
    for (int k = (int)v.n; k > 0; k--) v.p[k] = std::move(v.p[k - 1]);
    v.p[0].b = std::move(b);
    v.p[0].hash = hash;
    v.p[0].at = now;
    v.n++;
    b = std::move(fresh);
    hash = 0;
    return true;
}

/// At take now, a page taken without a change (SAME, or copied with a look that kept nothing), and every kept copy at the
/// sweep: a version not given back within kStale takes goes (the others stay, newest first), and the next change keeps one
/// again. Without this a page changed once and taken the same ever after held its copy for good, and once kCapPages were
/// held so, keep_old kept none for any page. True when one went
inline bool age(Vers& v, uint64_t now) {
    bool any = false;
    for (int k = 0; k < (int)v.n;) {
        if (!v.p[k].b || now - v.p[k].at <= kStale) {
            k++;
            continue;
        }
        v.p[k].drop();
        for (int j = k; j + 1 < (int)v.n; j++) v.p[j] = std::move(v.p[j + 1]);
        v.n--;
        any = true;
    }
    // ((Triforce) as HEAD's: the page's misses start again once the copy it kept has gone. Not when nothing went: the take
    // that set keep_old's kRetry cooldown calls age too, and that must survive it)
    if (any && !v.n) v.misses = 0;
    return any;
}

/// Whether page p is in sorted more than once (the capture's twice)
inline bool twice(const std::vector<uint32_t>& sorted, uint32_t p) {
    const auto r = std::equal_range(sorted.begin(), sorted.end(), p);
    return r.second - r.first > 1;
}

/// (RX_VK_SHADOW_CHECK) Whether a copy b with hash holds cap's bytes (cap null: not compared) and its hash is theirs or 0
template <class Hash>
bool holds(const uint8_t* b, uint64_t hash, const uint8_t* cap, Hash&& content_hash) {
    if (!b) return false;
    if (cap && std::memcmp(b, cap, kPage) != 0) return false;
    return !hash || hash == content_hash(b, kPage);
}

/// (RX_VK_SHADOW_CHECK) Every copy the page keeps holds a hash of its bytes or 0. False: one of them does not
template <class Hash>
bool holds_all(const Vers& v, Hash&& content_hash) {
    for (int k = 0; k < (int)v.n; k++)
        if (!holds(v.p[k].b.get(), v.p[k].hash, nullptr, content_hash)) return false;
    return true;
}

} // namespace shadow_prev
