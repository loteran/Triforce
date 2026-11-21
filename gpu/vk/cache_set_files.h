// (Triforce) The kept set of the shader caches in the game folder: what a start finds there, and what it renames, deletes and
// writes for cache_set.h's rule. The standard library and file_whole.h alone, nothing of the device: vk_cache_set.cpp gives it
// this start's record (the stamp and its parts) and the switches, and tests/test_cache_set.py runs it on a folder of its own.
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "cache_set.h"
#include "file_whole.h"

namespace cache_set {

using Log = void (*)(const char* fmt, ...);

inline std::string stamp_hex(uint64_t s) {
    char hex[20];
    std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)s);
    return hex;
}
inline bool read_text(const std::string& path, std::string* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    out->clear();
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0 && out->size() < (1u << 20);) out->append(buf, n);
    std::fclose(f);
    return true;
}
inline bool read_record(const std::string& path, Record* r) {
    std::string text;
    return read_text(path, &text) && parse(text, r);
}
/// The stamp a program cache's head names
inline bool head_of(const std::string& path, uint64_t* stamp) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t b[16];
    const size_t n = std::fread(b, 1, sizeof b, f);
    std::fclose(f);
    return programs_head(b, n, stamp);
}
inline bool size_of(const std::string& path, uint64_t* size) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
    const auto n = std::filesystem::file_size(path, ec);
    if (ec) return false;
    *size = (uint64_t)n;
    return true;
}
/// The pieces in the game folder by their names there, and their sizes
inline std::vector<std::pair<std::string, uint64_t>> pieces_there(const std::string& dir) {
    std::vector<std::pair<std::string, uint64_t>> out;
    for (const char* p : kPieces) {
        uint64_t n = 0;
        if (size_of(dir + p, &n)) out.push_back({p, n});
    }
    return out;
}
/// The player's state list (kStates) as it is now: its bytes hashed, with its size; 0 when there is none, or no record in it
inline uint64_t states_id(const std::string& dir) {
    FILE* f = std::fopen((dir + kStates).c_str(), "rb");
    if (!f) return 0;
    uint64_t h = 0xCBF29CE484222325ull, total = 0;
    std::vector<uint8_t> buf(1 << 16);
    for (size_t n; (n = std::fread(buf.data(), 1, buf.size(), f)) > 0; total += n)
        for (size_t i = 0; i < n; i++) h = (h ^ buf[i]) * 1099511628211ull;
    std::fclose(f);
    for (int i = 0; i < 8; i++) h = (h ^ (uint8_t)(total >> (8 * i))) * 1099511628211ull;
    return total > 12 ? (h | 1) : 0;   // (12: the list's head alone)
}
/// Every piece of the kept set there, at its size, and its program cache's head naming its stamp
inline bool kept_whole(const std::string& dir, const Record& kept) {
    bool programs = false;
    for (const auto& [name, size] : kept.pieces) {
        const std::string at = dir + kept_name(name, kept.stamp);
        uint64_t n = 0;
        if (!size_of(at, &n) || n != size) return false;
        if (name == kPieces[0]) {
            uint64_t stamp = 0;
            if (!head_of(at, &stamp) || stamp != kept.stamp) return false;
            programs = true;
        }
    }
    return programs;
}
/// The kept set deleted: every piece its stamp names (what its record lists, and what a start cut short left), then its record
inline void forget(const std::string& dir, const Record& kept) {
    for (const char* p : kPieces) {
        std::error_code ec;
        std::filesystem::remove(dir + kept_name(p, kept.stamp), ec);
    }
    std::remove((dir + kKept).c_str());
}
/// The files of a kept set's names (kept_piece_of) that are not the kept set's (kept: its record read; kept_stamp: its stamp)
/// deleted: what a start that ended in a swap left, or a set whose record cannot be read. Their count and bytes
inline std::pair<size_t, uint64_t> sweep(const std::string& dir, bool kept, uint64_t kept_stamp) {
    std::pair<size_t, uint64_t> gone{0, 0};
    std::error_code ec;
    std::vector<std::filesystem::path> drop;
    for (std::filesystem::directory_iterator it(dir.empty() ? std::string(".") : dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) continue;
        uint64_t stamp = 0;
        if (!kept_piece_of(it->path().filename().string(), &stamp) || (kept && stamp == kept_stamp)) continue;
        drop.push_back(it->path());
    }
    for (const auto& p : drop) {
        std::error_code sized, removed;
        const auto n = std::filesystem::file_size(p, sized);
        if (std::filesystem::remove(p, removed) && !removed) gone.first++, gone.second += sized ? 0 : (uint64_t)n;
    }
    return gone;
}
/// Each file renamed (a file there in the way replaced), or none: one that cannot be leaves those done renamed back
inline bool move_all(const std::vector<std::pair<std::string, std::string>>& moves, Log log) {
    for (size_t i = 0; i < moves.size(); i++) {
        if (file_writers::replace_file(moves[i].first, moves[i].second)) continue;
        log("vk: shader caches: %s cannot be renamed to %s: the set is left as it was", moves[i].first.c_str(),
            moves[i].second.c_str());
        for (size_t j = i; j-- > 0;) file_writers::replace_file(moves[j].second, moves[j].first);
        return false;
    }
    return true;
}
inline std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ", ") + (x == "robust" ? std::string("Robust buffers") : x);
    return s;
}

/// The switches a start has (vk_cache_set.cpp reads them from the environment)
struct Switches {
    bool on = true;            // (RX_VK_CACHE_SET not 0, RX_VK_PCACHE_KEEP_OLD not set)
    bool off_forgets = false;  // (off by RX_VK_CACHE_SET=0: a kept set is deleted)
    bool space_given = false;  // (tests: the disk's free space as given, not as the disk says)
    uint64_t free_bytes = 0;
};

/// The start's kept set decided and done in dir (its name ends in a separator, or is empty): now is this start's record (the
/// stamp, the build part, the parts). What a start does before anything opens a cache
inline Plan run(const std::string& dir, const Record& now, const Switches& sw, Log log) {
    Record had, kept;
    const bool had_read = read_record(dir + kRecord, &had);
    Facts f;
    f.on = sw.on, f.off_forgets = sw.off_forgets;
    f.stamp = now.stamp, f.build = now.build;
    f.kept = read_record(dir + kKept, &kept);
    // (no set is left there that nothing knows of: before the free space is measured, which it frees)
    const auto [swept, swept_bytes] = sweep(dir, f.kept, kept.stamp);
    if (swept)
        log("vk: shader caches: %zu files of a kept set no record names deleted (%llu MB: a start that ended while it swapped "
            "the sets, or a record that cannot be read)", swept, (unsigned long long)(swept_bytes >> 20));
    f.live = head_of(dir + kPieces[0], &f.live_stamp);
    f.live_named = f.live && had_read && had.stamp == f.live_stamp;
    f.live_build = had.build;
    const std::vector<std::pair<std::string, uint64_t>> live = pieces_there(dir);
    for (const auto& p : live) f.live_bytes += p.second;
    if (f.kept) {
        f.kept_stamp = kept.stamp, f.kept_build = kept.build, f.kept_bytes = kept.bytes();
        f.kept_whole = kept_whole(dir, kept);
    }
    if (sw.space_given) {
        f.space_known = true, f.free_bytes = sw.free_bytes;
    } else {
        std::error_code ec;
        const auto space = std::filesystem::space(dir.empty() ? std::string(".") : dir, ec);
        f.space_known = !ec;
        f.free_bytes = ec ? 0 : (uint64_t)space.available;
    }
    const Plan plan = decide(f);

    if (f.live && f.live_stamp != f.stamp) {   // (what changed, when it is known)
        std::string what = "not known: the set there has no record of what it was made for (a build before this rule)";
        if (f.live_named) {
            const std::vector<std::string> parts = changed(had, now);
            what = parts.empty() ? "a part of the translation's settings not named here (its per-GPU settings)" : joined(parts);
            if (had.env != now.env) what += "; the switches set: " + (had.env.empty() ? "none" : had.env) + " -> " +
                                            (now.env.empty() ? "none" : now.env);
        }
        log("vk: shader caches: made for %s, this start is %s: changed: %s", stamp_hex(f.live_stamp).c_str(),
            stamp_hex(f.stamp).c_str(), what.c_str());
    }
    if (plan.forget && f.kept) {
        forget(dir, kept);
        log("vk: shader caches: the kept set for %s (%llu MB) deleted: %s", stamp_hex(kept.stamp).c_str(),
            (unsigned long long)(kept.bytes() >> 20),
            kept.build != now.build                        ? "it is another build's"
            : !f.on                                        ? "RX_VK_CACHE_SET=0"
            : !f.kept_whole && kept.stamp == now.stamp     ? "a piece of it is missing or not its size"
                                                           : "one set is kept, and this one takes its place");
    } else if (f.kept && !f.on) {
        log("vk: shader caches: the kept set for %s (%llu MB, as <name>.%s.<ext>) stays while it is off (RX_VK_PCACHE_KEEP_OLD; "
            "RX_VK_CACHE_SET=0 deletes it)", stamp_hex(kept.stamp).c_str(), (unsigned long long)(kept.bytes() >> 20),
            stamp_hex(kept.stamp).c_str());
    }
    const auto renames = [&](const std::vector<std::pair<std::string, uint64_t>>& pieces, uint64_t stamp, bool to_kept) {
        std::vector<std::pair<std::string, std::string>> m;
        for (const auto& p : pieces) {
            const std::string a = dir + p.first, b = dir + kept_name(p.first, stamp);
            m.push_back(to_kept ? std::make_pair(a, b) : std::make_pair(b, a));
        }
        return m;
    };
    if (plan.what == Do::Keep) {
        // (its record first: a start that ends while the pieces are renamed leaves a set the next one refuses and deletes)
        Record k = had;
        k.pieces = live;
        k.states = states_id(dir);
        if (!file_writers::write_whole(dir + kKept, format(k))) {
            log("vk: shader caches: %s cannot be written: the set there is not kept", kKept);
        } else if (!move_all(renames(live, f.live_stamp, true), log)) {
            std::remove((dir + kKept).c_str());
        } else {
            log("vk: shader caches: the set for %s kept (%llu MB, %zu files, as <name>.%s.<ext>): a start for it again puts it "
                "back, and its shaders are not prepared again (RX_VK_CACHE_SET=0: not kept). The caches start empty for "
                "this stamp", stamp_hex(f.live_stamp).c_str(), (unsigned long long)(f.live_bytes >> 20), live.size(),
                stamp_hex(f.live_stamp).c_str());
        }
    } else if (plan.what == Do::Restore) {
        bool ok = true;
        std::vector<std::pair<std::string, std::string>> aside;
        if (plan.keep_live) {
            aside = renames(live, f.live_stamp, true);
            ok = move_all(aside, log);
        }
        if (ok && !move_all(renames(kept.pieces, kept.stamp, false), log)) {
            ok = false;
            for (size_t j = aside.size(); j-- > 0;) file_writers::replace_file(aside[j].second, aside[j].first);
        }
        if (!ok) {
            log("vk: shader caches: the kept set for %s is not put back: the caches drop the set there, as before",
                stamp_hex(kept.stamp).c_str());
        } else {
            // (the pieces the kept set did not have are of the set there: theirs are another stamp's, or none)
            if (!plan.keep_live)
                for (const auto& p : live) {
                    bool theirs = false;
                    for (const auto& q : kept.pieces) theirs = theirs || q.first == p.first;
                    if (!theirs) std::remove((dir + p.first).c_str());
                }
            const uint64_t states = states_id(dir);
            bool kept_now = false;
            if (plan.keep_live) {
                Record k = had;
                k.pieces = live;
                k.states = states;
                kept_now = file_writers::write_whole(dir + kKept, format(k));
                if (!kept_now) {
                    for (const auto& p : live) std::remove((dir + kept_name(p.first, f.live_stamp)).c_str());
                    std::remove((dir + kKept).c_str());
                }
            } else {
                std::remove((dir + kKept).c_str());
            }
            // (the player met pipelines while the set put back was not there: its notes lack them. The prepare before the
            // title goes through the state lists, and they are built before the game, as a prepare of every shader would)
            const bool lists = states && states != kept.states;
            if (lists && !file_writers::write_whole(dir + kStatesPending, "put back " + stamp_hex(kept.stamp) + "\n"))
                log("vk: shader caches: %s cannot be written: the pipelines met since the set was kept are built in play",
                    kStatesPending);
            log("vk: shader caches: the kept set for %s put back (%llu MB, %zu files): its shaders are not prepared again%s%s",
                stamp_hex(kept.stamp).c_str(), (unsigned long long)(kept.bytes() >> 20), kept.pieces.size(),
                kept_now ? (", and the set for " + stamp_hex(f.live_stamp) + " is kept in its place").c_str()
                         : plan.keep_live ? ", and the set there could not be kept: deleted" : "",
                lists ? "; the pipeline state list changed since it was kept: its pipelines are noted before the title" : "");
        }
    } else if (plan.what == Do::Drop && f.live && f.live_stamp != f.stamp) {
        const char* why = !f.on ? "RX_VK_CACHE_SET=0 or RX_VK_PCACHE_KEEP_OLD"
                          : !f.live_named ? "what it was made for is not known"
                          : f.live_build != f.build ? "another build of the translator made it: it is of no use again"
                          : "the disk has no room for it beside a new one (twice the set and 10 GB free)";
        log("vk: shader caches: the set for %s (%llu MB) is not kept: %s; free %llu MB", stamp_hex(f.live_stamp).c_str(),
            (unsigned long long)(f.live_bytes >> 20), why, (unsigned long long)(f.free_bytes >> 20));
    }
    // (what the set there is made for from now on: this start's caches are written for its stamp)
    std::string text;
    const std::string want = format(now);
    if (!read_text(dir + kRecord, &text) || text != want)
        if (!file_writers::write_whole(dir + kRecord, want)) log("vk: shader caches: %s cannot be written", kRecord);
    return plan;
}

/// A start that leaves the caches alone (RX_VK_NO_PROGRAM_CACHE: no program cache to say which set is there): a kept set of
/// another build is still deleted, and files of a kept set's names that no record names
inline void tidy(const std::string& dir, uint64_t build, Log log) {
    Record kept;
    const bool known = read_record(dir + kKept, &kept);
    const auto [swept, swept_bytes] = sweep(dir, known, kept.stamp);
    if (swept)
        log("vk: shader caches: %zu files of a kept set no record names deleted (%llu MB)", swept,
            (unsigned long long)(swept_bytes >> 20));
    if (known && kept.build != build) {
        forget(dir, kept);
        log("vk: shader caches: the kept set for %s (%llu MB) deleted: it is another build's", stamp_hex(kept.stamp).c_str(),
            (unsigned long long)(kept.bytes() >> 20));
    } else if (known) {
        log("vk: shader caches: the kept set for %s (%llu MB) stays: RX_VK_NO_PROGRAM_CACHE", stamp_hex(kept.stamp).c_str(),
            (unsigned long long)(kept.bytes() >> 20));
    }
}

}  // namespace cache_set
