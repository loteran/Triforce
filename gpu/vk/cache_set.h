// (Triforce) One earlier set of the shader caches, for a change of a setting: the rule (vk_cache_set.cpp does the files).
//
// Why. Everything the game keeps of its shaders is kept for one stamp (vk_shader.cpp disk_stamp, program_cache_stamp): the
// program cache (triforce_programs.bin), the pipeline cache (mkw_vk_pipelines.bin by its .for, and the play file), the
// pipeline notes and the prepare's record. A start of another stamp drops all of them whole, and prepares every shader again:
// on AMD's driver 1.5 to 2 hours (91,600 stage libraries at 16 a second, a cold precompile, the translation). Most stamps
// change with the translator, and then nothing of the old set is any use again. Some change with a setting: the menu's Robust
// buffers row (the translation's out-of-bounds buffer reads, vk_coopmat.cpp fetch_bounds_buffers), the mesh shaders row, a GPU
// of a laptop's two, an RX_VK_ switch of the translation. Switching such a setting back made the same two hours again.
// So: when the stamp changed and the build part did not (the translator and its C++ library, vk_shader.cpp
// program_cache_build_part: what only a new build changes), the set there is kept, renamed to <name>.<stamp>.<ext>, when the
// disk has room for it and for the new one (twice the set and 10 GB free). One set is kept, no more. A start whose stamp is the
// kept set's puts it back (every piece, or none: a set with a piece missing or of another size is refused and deleted), and
// the set it found takes its place as the kept one. A start of another build deletes the kept set: it is of no use to it.
// What a start found, and what changed (triforce_cache_set.txt beside the caches, the parts of the stamp), is in the log.
// The driver matches what it is handed by its own hash of the whole creation: a set put back is the very data the driver made
// for that stamp, and the caches' own checks (the .for files, the program cache's head, the pipeline cache's device and UUID
// header) still read it as they read any start's.
// A set put back has the pipeline notes of its own time: what the player met while the other set was there is in the
// player's state list (triforce_pipeline_states.bin, not a piece: it is good for any stamp). When that list is not the one
// the set was kept with, kStatesPending asks the prepare before the title to go through the lists (vk_draw.cpp prepare_run,
// state_list_prepare), as a prepare of every shader would have: those pipelines are built then, not at their first draw.
// Files of a kept set's names that no record names (a start that ended in a swap, a record that cannot be read) are deleted
// at the next start: no set is left on the disk that nothing knows of.
// RX_VK_CACHE_SET=0: no set kept or put back (a change of stamp drops the caches, as before), and a kept set is deleted;
// RX_VK_PCACHE_KEEP_OLD and RX_VK_NO_PROGRAM_CACHE leave it off too (a kept set of this build stays, another build's is
// deleted). tests/test_cache_set.py (cache_set_files.h does the files)
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace cache_set {

// (every file kept for one stamp, as the game folder names it: vk_shader.cpp disk_init's two, vk_draw.cpp pcache_init's (the
// carry's .base too, pcache_carry.h) and pcache_play_open's, the prepare's record and what prepare_run keeps beside it, the notes
// and what vk_draw.cpp keeps beside them by the stamp. The first is the set's own: a set is the one its program cache's head names)
inline constexpr const char* kPieces[] = {
    "triforce_programs.bin",         "triforce_programs.idx",
    "mkw_vk_pipelines.bin",          "mkw_vk_pipelines.bin.for",
    "mkw_vk_pipelines.bin.base",     // (the carry's history of that file, by its stamp: pcache_carry.h)
    "mkw_vk_pipelines.bin.own",      // ((Triforce) that file holds the driver's part's own alone: vk_draw.cpp pcache_own_mark)
    "mkw_vk_pipelines_play.bin",     "mkw_vk_pipelines_play.bin.for",
    "triforce_prepared.bin",         "triforce_prepared.bin.check",
    "triforce_prepared.bin.warming", "triforce_prepared.bin.warming.skip",
    "triforce_pipelines.bin",        "triforce_pipelines.bin.predicted",
    "triforce_pipelines.bin.plan",   "triforce_pipelines.bin.built",
    "triforce_states_applied.bin",
};
inline constexpr size_t kPieceCount = sizeof kPieces / sizeof kPieces[0];
inline constexpr const char* kRecord = "triforce_cache_set.txt";       // (what the set there was made for)
inline constexpr const char* kKept = "triforce_cache_set.kept.txt";    // (the kept set: what it was made for, and its pieces)
inline constexpr const char* kStates = "triforce_pipeline_states.bin"; // (the player's state list, vk_draw.cpp: not a piece)
inline constexpr const char* kStatesPending = "triforce_cache_set.states.txt";   // (a set put back: the lists to go through)
inline constexpr uint32_t kProgramsMagic = 0x31505348;                 // (vk_shader.cpp kDiskMagic: the program cache's head)
inline constexpr uint64_t kMargin = 10ull << 30;                       // (free beyond twice the set)

/// RX_VK_CACHE_SET: on unless it is 0
inline bool on(const char* env) { return !(env && env[0] == '0' && !env[1]); }

/// A piece's name in the kept set: its name to the first '.', the stamp's 16 digits, the rest
/// (mkw_vk_pipelines.bin.for: mkw_vk_pipelines.0123456789abcdef.bin.for)
inline std::string kept_name(const std::string& live, uint64_t stamp) {
    char hex[20];
    std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)stamp);
    const size_t dot = live.find('.');
    if (dot == std::string::npos) return live + "." + hex;
    return live.substr(0, dot) + "." + hex + live.substr(dot);
}

/// Whether a file's name is a piece's name in a kept set (kept_name's), and the stamp it names
inline bool kept_piece_of(const std::string& file, uint64_t* stamp) {
    for (const char* p : kPieces) {
        const std::string live = p;
        const size_t dot = live.find('.');
        const std::string stem = live.substr(0, dot), rest = live.substr(dot);
        if (file.size() != stem.size() + 17 + rest.size() || file.compare(0, stem.size(), stem) || file[stem.size()] != '.' ||
            file.compare(file.size() - rest.size(), rest.size(), rest))
            continue;
        uint64_t v = 0;
        bool hex = true;
        for (size_t i = stem.size() + 1; i < stem.size() + 17 && hex; i++) {
            const char c = file[i];
            hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            v = v << 4 | (uint64_t)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        if (!hex) continue;
        *stamp = v;
        return true;
    }
    return false;
}

/// The stamp a program cache's first 16 bytes name (magic, the build that wrote it, the stamp); false: no program cache's head
inline bool programs_head(const uint8_t* b, size_t n, uint64_t* stamp) {
    if (n < 16) return false;
    uint32_t magic;
    std::memcpy(&magic, b, 4);
    if (magic != kProgramsMagic) return false;
    std::memcpy(stamp, b + 8, 8);
    return true;
}

/// What a set was made for (the stamp, the build part, and the parts of the stamp by name for the log) and, for the kept
/// one, its pieces as they were renamed: each one's name in the game folder and its size
struct Record {
    uint64_t stamp = 0, build = 0;
    std::vector<std::pair<std::string, uint64_t>> parts;
    std::string env;   // (the stamp's RX_VK_ and EDEN_ switches that were set, by name)
    uint64_t states = 0;   // (the kept one: the player's state list as it was when kept, cache_set_files.h states_id; 0: none)
    std::vector<std::pair<std::string, uint64_t>> pieces;
    uint64_t bytes() const {
        uint64_t n = 0;
        for (const auto& p : pieces) n += p.second;
        return n;
    }
};

inline std::string format(const Record& r) {
    std::string s = "triforce-cache-set 1\n";
    char line[160];
    std::snprintf(line, sizeof line, "stamp %016llx\nbuild %016llx\n", (unsigned long long)r.stamp, (unsigned long long)r.build);
    s += line;
    for (const auto& [name, v] : r.parts) {
        std::snprintf(line, sizeof line, "part %s %016llx\n", name.c_str(), (unsigned long long)v);
        s += line;
    }
    s += "env " + (r.env.empty() ? std::string("-") : r.env) + "\n";
    if (r.states) {
        std::snprintf(line, sizeof line, "states %016llx\n", (unsigned long long)r.states);
        s += line;
    }
    for (const auto& [name, size] : r.pieces) {
        std::snprintf(line, sizeof line, "piece %s %llu\n", name.c_str(), (unsigned long long)size);
        s += line;
    }
    return s;
}

/// format's text read back; false when it is not one (no head, no stamp or build, a piece that is not one of kPieces)
inline bool parse(const std::string& text, Record* out) {
    Record r;
    bool head = false, stamp = false, build = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const size_t sp = line.find(' ');
        const std::string key = line.substr(0, sp), rest = sp == std::string::npos ? "" : line.substr(sp + 1);
        if (!head) {
            if (line != "triforce-cache-set 1") return false;
            head = true;
            continue;
        }
        unsigned long long v = 0;
        char name[96];
        if (key == "stamp" && std::sscanf(rest.c_str(), "%16llx", &v) == 1) {
            r.stamp = v, stamp = true;
        } else if (key == "build" && std::sscanf(rest.c_str(), "%16llx", &v) == 1) {
            r.build = v, build = true;
        } else if (key == "part" && std::sscanf(rest.c_str(), "%95s %16llx", name, &v) == 2) {
            r.parts.push_back({name, v});
        } else if (key == "states" && std::sscanf(rest.c_str(), "%16llx", &v) == 1) {
            r.states = v;
        } else if (key == "env") {
            r.env = rest == "-" ? "" : rest;
        } else if (key == "piece" && std::sscanf(rest.c_str(), "%95s %llu", name, &v) == 2) {
            bool known = false;
            for (const char* p : kPieces) known = known || !std::strcmp(p, name);
            if (!known) return false;
            r.pieces.push_back({name, v});
        }
    }
    if (!head || !stamp || !build) return false;
    *out = std::move(r);
    return true;
}

/// The parts by name whose value is not the same in a and b (a part only one of them has counts as changed), for the log
inline std::vector<std::string> changed(const Record& a, const Record& b) {
    std::vector<std::string> out;
    if (a.build != b.build) out.push_back("build");
    const auto find = [](const Record& r, const std::string& name) -> const uint64_t* {
        for (const auto& p : r.parts)
            if (p.first == name) return &p.second;
        return nullptr;
    };
    for (const auto& p : a.parts) {
        const uint64_t* v = find(b, p.first);
        if (!v || *v != p.second) out.push_back(p.first);
    }
    for (const auto& p : b.parts)
        if (!find(a, p.first)) out.push_back(p.first);
    return out;
}

/// Room for a set kept beside the new one: twice the set and kMargin free, counting what deleting the old kept set frees
inline bool room(uint64_t free_bytes, uint64_t freed, uint64_t set_bytes) {
    const uint64_t have = free_bytes + freed < free_bytes ? ~0ull : free_bytes + freed;
    if (set_bytes > (~0ull - kMargin) / 2) return false;
    return have >= 2 * set_bytes + kMargin;
}

/// What a start knows: its stamp and build part; the set there (its program cache's head, and the record of what it was made
/// for when the record names that stamp); the kept set (its record, and whether every piece is there whole); the disk
struct Facts {
    bool on = false;                  // (keeping and putting back allowed)
    bool off_forgets = false;         // (off by RX_VK_CACHE_SET=0: a kept set of this build is deleted too, for its space)
    uint64_t stamp = 0, build = 0;    // (this start's)
    bool live = false;                // (a program cache there, its head read)
    uint64_t live_stamp = 0;          // (what its head names)
    bool live_named = false;          // (the record names live_stamp: live_build is known)
    uint64_t live_build = 0, live_bytes = 0;
    bool kept = false;                // (a kept set's record)
    uint64_t kept_stamp = 0, kept_build = 0, kept_bytes = 0;
    bool kept_whole = false;          // (each of its pieces there at its size, its program cache's head naming kept_stamp)
    bool space_known = false;
    uint64_t free_bytes = 0;
};
enum class Do {
    Same,      // (the set there is this stamp's: nothing to do)
    Restore,   // (the kept set is this stamp's: put back; keep_live: the set there becomes the kept one, else it is deleted)
    Keep,      // (the set there is kept, renamed; the caches start empty for this stamp)
    Drop,      // (nothing renamed: the caches drop the set there, as before)
};
struct Plan {
    Do what = Do::Drop;
    bool forget = false;      // (the kept set is deleted first)
    bool keep_live = false;   // (Restore: the set there is kept in its place)
};

inline Plan decide(const Facts& f) {
    Plan p;
    const bool same = f.live && f.live_stamp == f.stamp;
    if (!f.on) {
        // (off: nothing kept or put back. A kept set of another build is of no use to any build again: deleted; one of this
        // build too when the off is RX_VK_CACHE_SET=0's, the switch that frees its space)
        p.what = same ? Do::Same : Do::Drop;
        p.forget = f.kept && (f.kept_build != f.build || f.off_forgets);
        return p;
    }
    bool kept_ours = f.kept && f.kept_build == f.build;
    // (another build's kept set is of no use to this one, nor to any later: deleted. A kept set that names the set there
    // is what a start that ended while it swapped the two left: its pieces are not where it says)
    p.forget = f.kept && (!kept_ours || (same && f.kept_stamp == f.stamp));
    if (same) {
        p.what = Do::Same;
        return p;
    }
    const bool live_ours = f.live && f.live_named && f.live_build == f.build;
    if (kept_ours && f.kept_stamp == f.stamp) {
        if (f.kept_whole) {
            p.what = Do::Restore;
            p.keep_live = live_ours;
            return p;
        }
        p.forget = true;   // (a piece missing or cut: refused)
    }
    if (live_ours && f.space_known && room(f.free_bytes, f.kept ? f.kept_bytes : 0, f.live_bytes)) {
        p.what = Do::Keep;
        p.forget = f.kept;   // (one set kept: the one there takes the old one's place)
        return p;
    }
    return p;
}

}  // namespace cache_set
