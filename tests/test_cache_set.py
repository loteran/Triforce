"""One earlier set of the shader caches is kept for a change of a setting, and put back when the setting goes back
(gpu/vk/cache_set.h the rule, gpu/vk/cache_set_files.h the files, gpu/vk/vk_cache_set.cpp this start's; no game/GPU).

The header is compiled and run as it is: the decision for each start (the set there this stamp's: nothing; a stamp that
changed by a setting, the build the same: kept when the disk has twice the set and 10 GB free, counting what deleting the
old kept set frees; the kept set this stamp's and whole: put back, the set there kept in its place when it is this build's;
a kept set with a piece missing: refused and deleted; another build: the kept set deleted, nothing kept; off: nothing
renamed, a kept set deleted for another build or RX_VK_CACHE_SET=0), the kept pieces' names, the record's text read back,
the program cache's head. Then cache_set_files.h runs on folders of the test's own: kept, put back with and without the
set there kept, the player's pipeline list gone through when it changed since, a refused set, a rename that fails rolled
back, a swap cut short and an unreadable record swept, the off switches. Then the source: the set is decided once, right
after the device is made and before anything opens a cache; its pieces are the files the caches have by the stamp; a set
put back has the state lists gone through before the title; the build part is outside the translation stamp, and the
translation stamp's text is what it was; the Robust buffers row says what changing it costs, by the card.
python -I -B tests/test_cache_set.py --cxx clang++
"""
import argparse
import ast
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

FIXTURE = r"""
#include <cstdio>
#include "cache_set.h"
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); return 1; } } while (0)
using namespace cache_set;
static const uint64_t GB = 1ull << 30;
static const uint64_t A = 0xA0A0A0A0A0A0A0A0ull, B = 0xB0B0B0B0B0B0B0B0ull, C = 0xC0C0C0C0C0C0C0C0ull;
static const uint64_t X = 0x1111ull, Y = 0x2222ull;   // (two builds)
// (a start of stamp B, build X, over the set of stamp A that build X made: the Robust buffers row switched)
static Facts toggled(uint64_t free_gb, uint64_t set_gb) {
    Facts f;
    f.on = true, f.stamp = B, f.build = X;
    f.live = true, f.live_stamp = A, f.live_named = true, f.live_build = X, f.live_bytes = set_gb * GB;
    f.space_known = true, f.free_bytes = free_gb * GB;
    return f;
}
int main() {
    // (the set there is this stamp's: nothing renamed, whatever there is room for)
    {
        Facts f = toggled(500, 6);
        f.stamp = A;
        Plan p = decide(f);
        CHECK(p.what == Do::Same && !p.forget);
        f.kept = true, f.kept_stamp = B, f.kept_build = X, f.kept_whole = true;   // (kept for the other setting: stays)
        p = decide(f);
        CHECK(p.what == Do::Same && !p.forget);
        f.kept_build = Y;   // (another build's kept set: deleted)
        p = decide(f);
        CHECK(p.what == Do::Same && p.forget);
        f.kept_build = X, f.kept_stamp = A, f.kept_whole = false;   // (a kept set naming the set there: a swap cut short)
        p = decide(f);
        CHECK(p.what == Do::Same && p.forget);
    }
    // (a setting changed: kept with twice the set and 10 GB free, 22 GB for a 6 GB set; a byte less is not room)
    CHECK(decide(toggled(50, 6)).what == Do::Keep && !decide(toggled(50, 6)).forget);
    CHECK(decide(toggled(22, 6)).what == Do::Keep);
    {
        Facts f = toggled(22, 6);
        f.free_bytes -= 1;
        CHECK(decide(f).what == Do::Drop && !decide(f).forget);
        // (an old kept set of a third stamp: deleting it frees its bytes, and the one there takes its place)
        f.kept = true, f.kept_stamp = C, f.kept_build = X, f.kept_bytes = 6 * GB, f.kept_whole = true;
        f.free_bytes = 16 * GB;
        Plan p = decide(f);
        CHECK(p.what == Do::Keep && p.forget);
        f.free_bytes = 16 * GB - 1;   // (no room even so: the old kept set stays, the set there is dropped as before)
        p = decide(f);
        CHECK(p.what == Do::Drop && !p.forget);
    }
    CHECK(decide(toggled(0, 0)).what == Do::Drop);   // (10 GB are wanted beyond any set)
    CHECK(decide(toggled(10, 0)).what == Do::Keep);
    {
        Facts f = toggled(500, 6);
        f.space_known = false;   // (the disk's free space not known: not kept)
        CHECK(decide(f).what == Do::Drop);
        f = toggled(500, 6);
        f.live_named = false;   // (no record of what the set there was made for: a build before the rule. Not kept)
        CHECK(decide(f).what == Do::Drop);
        f = toggled(500, 6);
        f.live = false;   // (no program cache there: nothing to keep)
        CHECK(decide(f).what == Do::Drop);
    }
    // (the translator changed: the set there is no use again, and the kept one neither)
    {
        Facts f = toggled(500, 6);
        f.build = Y;
        f.kept = true, f.kept_stamp = B, f.kept_build = X, f.kept_bytes = 6 * GB, f.kept_whole = true;
        Plan p = decide(f);
        CHECK(p.what == Do::Drop && p.forget);
    }
    // (switched back: the kept set is this stamp's and whole: put back, the set there kept in its place)
    {
        Facts f = toggled(0, 6);   // (no free space wanted for that: renames)
        f.kept = true, f.kept_stamp = B, f.kept_build = X, f.kept_bytes = 6 * GB, f.kept_whole = true;
        Plan p = decide(f);
        CHECK(p.what == Do::Restore && p.keep_live && !p.forget);
        f.live_named = false;   // (the set there of a build not known: put back over it, and it is not kept)
        p = decide(f);
        CHECK(p.what == Do::Restore && !p.keep_live && !p.forget);
        f.live = false;   // (no set there at all)
        p = decide(f);
        CHECK(p.what == Do::Restore && !p.keep_live);
        // (a piece missing or not its size: refused and deleted; the set there is then kept if there is room)
        f = toggled(500, 6);
        f.kept = true, f.kept_stamp = B, f.kept_build = X, f.kept_bytes = 6 * GB, f.kept_whole = false;
        p = decide(f);
        CHECK(p.what == Do::Keep && p.forget);
        f.free_bytes = 0;
        p = decide(f);
        CHECK(p.what == Do::Drop && p.forget);
        // (the kept set of this stamp, but of another build: never put back)
        f = toggled(500, 6);
        f.build = Y, f.kept = true, f.kept_stamp = B, f.kept_build = X, f.kept_whole = true;
        p = decide(f);
        CHECK(p.what == Do::Drop && p.forget);
    }
    // (off: nothing renamed; a kept set of this build stays, but for RX_VK_CACHE_SET=0's off; another build's is deleted)
    {
        Facts f = toggled(500, 6);
        f.on = false;
        f.kept = true, f.kept_stamp = B, f.kept_build = X, f.kept_whole = true;
        Plan p = decide(f);
        CHECK(p.what == Do::Drop && !p.forget);
        f.stamp = A;
        CHECK(decide(f).what == Do::Same && !decide(f).forget);
        f.off_forgets = true;
        CHECK(decide(f).what == Do::Same && decide(f).forget);
        f.off_forgets = false, f.kept_build = Y;   // (a translator update while it is off: of no use to any build again)
        CHECK(decide(f).what == Do::Same && decide(f).forget);
        f.stamp = B;
        CHECK(decide(f).what == Do::Drop && decide(f).forget);
    }
    CHECK(room(22 * GB, 0, 6 * GB) && !room(22 * GB - 1, 0, 6 * GB) && room(16 * GB, 6 * GB, 6 * GB));
    CHECK(!room(~0ull, ~0ull, ~0ull / 2) && room(~0ull, ~0ull, 0));   // (no sum wraps)
    CHECK(on(nullptr) && on("1") && on("") && on("00") && !on("0"));

    CHECK(kept_name("mkw_vk_pipelines.bin.for", 0x0123456789abcdefull) == "mkw_vk_pipelines.0123456789abcdef.bin.for");
    CHECK(kept_name("triforce_prepared.bin.warming.skip", 1) == "triforce_prepared.0000000000000001.bin.warming.skip");
    CHECK(kept_name("triforce_programs.idx", B) == "triforce_programs.b0b0b0b0b0b0b0b0.idx");
    for (size_t i = 0; i < kPieceCount; i++)
        for (size_t j = 0; j < kPieceCount; j++) CHECK(i == j || kept_name(kPieces[i], A) != kept_name(kPieces[j], A));
    CHECK(kept_name(kPieces[0], A) != kept_name(kPieces[0], B));
    // (a kept piece's name read back, and nothing else taken for one)
    for (size_t i = 0; i < kPieceCount; i++) {
        uint64_t st = 0;
        CHECK(kept_piece_of(kept_name(kPieces[i], A), &st) && st == A);
        CHECK(!kept_piece_of(kPieces[i], &st));
    }
    {
        uint64_t st = 0;
        CHECK(kept_piece_of("triforce_programs.0123456789abcdef.idx", &st) && st == 0x0123456789abcdefull);
        CHECK(!kept_piece_of("triforce_programs.0123456789ABCDEF.idx", &st));   // (kept_name writes lower case)
        CHECK(!kept_piece_of("triforce_programs.0123456789abcde.idx", &st));
        CHECK(!kept_piece_of("triforce_programs.0123456789abcdef.idx.part", &st));
        CHECK(!kept_piece_of("triforce_shaders.0123456789abcdef.bin", &st));
        CHECK(!kept_piece_of(kKept, &st) && !kept_piece_of(kRecord, &st) && !kept_piece_of(kStates, &st));
    }

    uint8_t head[16] = {0x48, 0x53, 0x50, 0x31, 9, 9, 9, 9};
    std::memcpy(head + 8, &B, 8);
    uint64_t s = 0;
    CHECK(programs_head(head, 16, &s) && s == B);
    CHECK(!programs_head(head, 15, &s));
    head[0] ^= 1;
    CHECK(!programs_head(head, 16, &s));

    Record r;
    r.stamp = A, r.build = X;
    r.parts = {{"depth", 1}, {"robust", 0}, {"env", 77}};
    r.env = "RX_VK_MESH_AHEAD,EDEN_X";
    r.pieces = {{"triforce_programs.bin", 5ull * GB}, {"mkw_vk_pipelines.bin.for", 16}};
    r.states = C;
    Record back;
    CHECK(parse(format(r), &back));
    CHECK(back.stamp == A && back.build == X && back.parts == r.parts && back.env == r.env && back.pieces == r.pieces);
    CHECK(back.states == C);
    CHECK(back.bytes() == 5ull * GB + 16);
    std::string crlf;
    for (char ch : format(r)) {
        if (ch == '\n') crlf += '\r';
        crlf += ch;
    }
    CHECK(parse(crlf, &back) && back.pieces == r.pieces);
    Record none;
    CHECK(parse(format(none), &back) && back.env.empty() && back.pieces.empty() && !back.states);
    CHECK(!parse("", &back) && !parse("triforce-cache-set 2\nstamp 1\nbuild 2\n", &back));
    CHECK(!parse("triforce-cache-set 1\nstamp 1\n", &back));   // (no build part: not a record)
    CHECK(!parse("triforce-cache-set 1\nstamp 1\nbuild 2\npiece ..\\triforce_options.ini 3\n", &back));   // (not a piece)

    Record now = r;
    now.stamp = B;
    now.parts[1].second = 1;
    CHECK(changed(r, now) == std::vector<std::string>{"robust"});
    now.build = Y;
    now.parts.push_back({"gpu", 3});
    CHECK((changed(r, now) == std::vector<std::string>{"build", "robust", "gpu"}));
    CHECK(changed(r, r).empty());
    return 0;
}
"""

FILES = r"""
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>
#include "cache_set_files.h"
#define CHECK(x) do { if (!(x)) { std::printf("%s line %d: %s\n", where, __LINE__, #x); return 1; } } while (0)
using namespace cache_set;
namespace fs = std::filesystem;
static const uint64_t GB = 1ull << 30;
static const uint64_t A = 0xA0A0A0A0A0A0A0A0ull, B = 0xB0B0B0B0B0B0B0B0ull, C = 0xC0C0C0C0C0C0C0C0ull;
static const uint64_t X = 0x1111ull, Y = 0x2222ull;
static const char* where = "";
static void log_(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stderr, fmt, a);
    va_end(a);
    std::fputc('\n', stderr);
}
static std::string base;
static std::string folder(const char* name) {
    where = name;
    const std::string d = base + "/" + name + "/";
    fs::create_directories(d);
    return d;
}
// (n bytes; a program cache's head naming stamp when it is not 0)
static void put(const std::string& path, size_t n, uint64_t stamp = 0) {
    std::string b(n, 'x');
    if (stamp && n >= 16) {
        const uint32_t magic = kProgramsMagic, build = 7;
        std::memcpy(&b[0], &magic, 4), std::memcpy(&b[4], &build, 4), std::memcpy(&b[8], &stamp, 8);
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
}
static uint64_t size(const std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return ~0ull;
    const auto n = fs::file_size(path, ec);
    return ec ? ~0ull : (uint64_t)n;
}
static bool has(const std::string& path) { return fs::exists(path); }
static uint64_t head(const std::string& path) {
    uint64_t s = 0;
    return head_of(path, &s) ? s : 0;
}
static Record rec(uint64_t stamp, uint64_t build) {
    Record r;
    r.stamp = stamp, r.build = build;
    r.parts = {{"robust", stamp == A ? 0u : 1u}};
    return r;
}
static void record(const std::string& d, const Record& r) { file_writers::write_whole(d + kRecord, format(r)); }
static void kept_record(const std::string& d, const Record& r) { file_writers::write_whole(d + kKept, format(r)); }
// (the files in the folder, but for the record of what the set there is for)
static size_t count(const std::string& d) {
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(d)) n += e.path().filename() != kRecord;
    return n;
}
static Switches room_for(bool on = true) {
    Switches s;
    s.on = on, s.space_given = true, s.free_bytes = 20 * GB;
    return s;
}
static const std::string P = "triforce_programs.bin", M = "mkw_vk_pipelines.bin", R = "triforce_prepared.bin";

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    base = argv[1];
    const std::string a = folder("swap");
    // (a setting changed: the set for A kept by its names, its record first, the player's list unmoved; the record says B)
    {
        put(a + P, 100, A), put(a + M, 50), put(a + kStates, 40);
        record(a, rec(A, X));
        const Plan p = run(a, rec(B, X), room_for(), log_);
        CHECK(p.what == Do::Keep);
        CHECK(!has(a + P) && !has(a + M) && size(a + kept_name(P, A)) == 100 && size(a + kept_name(M, A)) == 50);
        CHECK(head(a + kept_name(P, A)) == A && size(a + kStates) == 40);
        Record k, now;
        CHECK(read_record(a + kKept, &k) && k.stamp == A && k.build == X && k.states == states_id(a) && k.states);
        CHECK((k.pieces == std::vector<std::pair<std::string, uint64_t>>{{P, 100}, {M, 50}}));
        CHECK(read_record(a + kRecord, &now) && now.stamp == B);
        CHECK(!has(a + kStatesPending) && count(a) == 4);
    }
    // (switched back after play under B, the player's list longer: A put back, B kept in its place with a piece A lacks,
    // and the lists are gone through before the title)
    {
        put(a + P, 70, B), put(a + M, 30), put(a + R, 9), put(a + kStates, 60);
        const Plan p = run(a, rec(A, X), room_for(), log_);
        CHECK(p.what == Do::Restore && p.keep_live && !p.forget);
        CHECK(head(a + P) == A && size(a + P) == 100 && size(a + M) == 50 && !has(a + R));
        CHECK(head(a + kept_name(P, B)) == B && size(a + kept_name(P, B)) == 70 && size(a + kept_name(M, B)) == 30);
        CHECK(size(a + kept_name(R, B)) == 9 && !has(a + kept_name(P, A)) && !has(a + kept_name(M, A)));
        Record k;
        CHECK(read_record(a + kKept, &k) && k.stamp == B && k.pieces.size() == 3 && k.states == states_id(a));
        CHECK(has(a + kStatesPending) && size(a + kStates) == 60);
        CHECK(count(a) == 8);   // (P M, the three of B, the kept record, the list, the marker)
    }
    // (and to B again, the list as B was kept with: put back, no lists to go through for it)
    {
        fs::remove(a + kStatesPending);
        const Plan p = run(a, rec(B, X), room_for(), log_);
        CHECK(p.what == Do::Restore && p.keep_live);
        CHECK(head(a + P) == B && size(a + P) == 70 && size(a + M) == 30 && size(a + R) == 9);
        CHECK(size(a + kept_name(P, A)) == 100 && size(a + kept_name(M, A)) == 50 && !has(a + kept_name(R, A)));
        CHECK(!has(a + kStatesPending) && count(a) == 7);
        // (the same start again: nothing moves)
        CHECK(run(a, rec(B, X), room_for(), log_).what == Do::Same && count(a) == 7 && size(a + kept_name(P, A)) == 100);
    }
    // (a new translator: the kept set deleted, nothing kept)
    {
        const Plan p = run(a, rec(C, Y), room_for(), log_);
        CHECK(p.what == Do::Drop && p.forget);
        CHECK(!has(a + kKept) && !has(a + kept_name(P, A)) && !has(a + kept_name(M, A)) && size(a + P) == 70);
    }
    // (put back over a set nothing says what it was made for: it is not kept, and its pieces the kept set lacks are deleted)
    {
        const std::string d = folder("restore-unnamed");
        put(d + kept_name(P, A), 100, A), put(d + kept_name(M, A), 50);
        Record k = rec(A, X);
        k.pieces = {{P, 100}, {M, 50}};
        kept_record(d, k);
        put(d + P, 70, C), put(d + M, 30), put(d + R, 9);
        const Plan p = run(d, rec(A, X), room_for(), log_);
        CHECK(p.what == Do::Restore && !p.keep_live);
        CHECK(head(d + P) == A && size(d + P) == 100 && size(d + M) == 50 && !has(d + R) && !has(d + kKept));
        CHECK(!has(d + kStatesPending) && count(d) == 2);   // (no list: none to go through)
    }
    // (a kept set with a piece not its size: refused and deleted, and the set there kept in its place)
    {
        const std::string d = folder("refused");
        put(d + kept_name(P, B), 70, B), put(d + kept_name(M, B), 29);
        Record k = rec(B, X);
        k.pieces = {{P, 70}, {M, 30}};
        kept_record(d, k);
        put(d + P, 100, A), put(d + M, 50);
        record(d, rec(A, X));
        const Plan p = run(d, rec(B, X), room_for(), log_);
        CHECK(p.what == Do::Keep && p.forget);
        CHECK(!has(d + kept_name(P, B)) && !has(d + kept_name(M, B)) && !has(d + P) && !has(d + M));
        CHECK(size(d + kept_name(P, A)) == 100 && size(d + kept_name(M, A)) == 50);
        CHECK(read_record(d + kKept, &k) && k.stamp == A && count(d) == 3);
    }
    // (a rename that cannot be made: those made are renamed back, and nothing is kept)
    {
        const std::string d = folder("rollback-keep");
        put(d + P, 100, A), put(d + M, 50);
        record(d, rec(A, X));
        fs::create_directories(d + kept_name(M, A));
        put(d + kept_name(M, A) + "/in-the-way", 1);
        run(d, rec(B, X), room_for(), log_);
        CHECK(head(d + P) == A && size(d + P) == 100 && size(d + M) == 50 && !has(d + kept_name(P, A)));
        CHECK(!has(d + kKept));
    }
    {
        const std::string d = folder("rollback-restore");
        put(d + kept_name(P, A), 100, A), put(d + kept_name(M, A), 50), put(d + kept_name(R, A), 9);
        Record k = rec(A, X);
        k.pieces = {{P, 100}, {M, 50}, {R, 9}};
        kept_record(d, k);
        put(d + P, 70, B), put(d + M, 30);
        record(d, rec(B, X));
        fs::create_directories(d + R);
        put(d + R + "/in-the-way", 1);
        const Plan p = run(d, rec(A, X), room_for(), log_);
        CHECK(p.what == Do::Restore && p.keep_live);
        CHECK(head(d + P) == B && size(d + P) == 70 && size(d + M) == 30);
        CHECK(size(d + kept_name(P, A)) == 100 && size(d + kept_name(M, A)) == 50 && size(d + kept_name(R, A)) == 9);
        CHECK(!has(d + kept_name(P, B)) && !has(d + kept_name(M, B)));
        CHECK(read_record(d + kKept, &k) && k.stamp == A);
    }
    // (a start that ended in a swap, after the set for B was renamed aside and A put back, before the record said so: the
    // record names A, which is there; B's pieces, which nothing names, are deleted, and nothing else)
    {
        const std::string d = folder("cut-swap");
        put(d + P, 100, A), put(d + M, 50);
        record(d, rec(A, X));
        Record k = rec(A, X);
        k.pieces = {{P, 100}, {M, 50}};
        kept_record(d, k);
        put(d + kept_name(P, B), 70, B), put(d + kept_name(M + ".for", B), 3), put(d + kept_name(R, B), 9);
        put(d + "notes.0123456789abcdef.txt", 5), put(d + P + ".part", 5), put(d + "triforce_shaders.bin", 5);
        const Plan p = run(d, rec(A, X), room_for(), log_);
        CHECK(p.what == Do::Same && p.forget);
        CHECK(size(d + P) == 100 && size(d + M) == 50 && !has(d + kKept));
        CHECK(!has(d + kept_name(P, B)) && !has(d + kept_name(M + ".for", B)) && !has(d + kept_name(R, B)));
        CHECK(has(d + "notes.0123456789abcdef.txt") && has(d + P + ".part") && has(d + "triforce_shaders.bin") && count(d) == 5);
    }
    // (a kept record that cannot be read: its pieces, which nothing names now, are deleted)
    {
        const std::string d = folder("unreadable");
        put(d + kept_name(P, B), 70, B), put(d + kept_name(M, B), 30);
        file_writers::write_whole(d + kKept, std::string("triforce-cache-set 9\n"));
        put(d + P, 100, A);
        record(d, rec(A, X));
        run(d, rec(A, X), room_for(), log_);
        CHECK(!has(d + kept_name(P, B)) && !has(d + kept_name(M, B)) && size(d + P) == 100);
    }
    // (off: RX_VK_CACHE_SET=0 deletes the kept set; RX_VK_PCACHE_KEEP_OLD leaves this build's, and deletes another's)
    {
        const auto kept_at = [](const std::string& d, uint64_t build) {
            put(d + kept_name(P, B), 70, B);
            Record k = rec(B, build);
            k.pieces = {{P, 70}};
            kept_record(d, k);
            put(d + P, 100, A);
            record(d, rec(A, X));
        };
        std::string d = folder("off-0");
        kept_at(d, X);
        Switches s = room_for(false);
        s.off_forgets = true;
        CHECK(run(d, rec(A, X), s, log_).what == Do::Same && !has(d + kept_name(P, B)) && !has(d + kKept));
        d = folder("off-keep-old");
        kept_at(d, X);
        CHECK(run(d, rec(C, X), room_for(false), log_).what == Do::Drop && size(d + kept_name(P, B)) == 70 && has(d + kKept));
        CHECK(size(d + P) == 100);   // (nothing renamed: the caches drop it themselves)
        d = folder("off-update");
        kept_at(d, Y);
        run(d, rec(A, X), room_for(false), log_);
        CHECK(!has(d + kept_name(P, B)) && !has(d + kKept));
        // (RX_VK_NO_PROGRAM_CACHE: the same, without a decision)
        d = folder("tidy");
        kept_at(d, Y);
        put(d + kept_name(M, C), 4);
        tidy(d, X, log_);
        CHECK(!has(d + kept_name(P, B)) && !has(d + kKept) && !has(d + kept_name(M, C)) && size(d + P) == 100);
        d = folder("tidy-ours");
        kept_at(d, X);
        tidy(d, X, log_);
        CHECK(size(d + kept_name(P, B)) == 70 && has(d + kKept));
    }
    return 0;
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def function(text, head):
    a = text.index(head)
    return text[a:text.index("\n}\n", a) + 2]


def check(ok, why):
    if not ok:
        raise SystemExit(why)


def stamped_shader():
    """vk_shader.cpp as nx2/build.py's translation_text takes it for the translation stamp"""
    tree = ast.parse((ROOT / "nx2" / "build.py").read_text(encoding="utf-8"))
    functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ("strip_comments", "translation_text")]
    ns = {}
    exec(compile(ast.Module(body=functions, type_ignores=[]), "build.py", "exec"), ns)
    return ns["translation_text"]("vk_shader.cpp", (VK / "vk_shader.cpp").read_bytes()).decode("utf-8")


def check_source():
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    main = (VK / "vk_main.cpp").read_text(encoding="utf-8")
    files = (VK / "vk_cache_set.cpp").read_text(encoding="utf-8")
    rule = (VK / "cache_set.h").read_text(encoding="utf-8")
    steps = (VK / "cache_set_files.h").read_text(encoding="utf-8")

    # (once the device is made, before anything opens a cache: mkwvk_init, right after create_device)
    init = function(main, "const MkwVkRenderer* mkwvk_init(const MkwVkHost* h) {")
    made = "if (!create_instance() || !pick_device() || !create_device()) return nullptr;"
    check(made in init and "cache_set_init();" in init, "mkwvk_init does not decide the caches' kept set")
    check(init.index(made) < init.index("cache_set_init();") < init.index("table.version"),
          "the kept set is not decided right after the device is made")
    run = function(files, "void cache_set_run() {")
    check("std::call_once(once, [] { cache_set_run(); });" in files, "the kept set is not decided once")
    check(run.index("other_triforce(") < run.index('getenv("RX_VK_NO_PROGRAM_CACHE")') < run.index("this_start()") <
          run.index("cache_set::run(dir, now, sw, logf);"),
          "the set is decided before it is known that no other Triforce has the caches")
    check('getenv("RX_VK_PCACHE_KEEP_OLD")' in run and 'cache_set::on(getenv("RX_VK_CACHE_SET"))' in run and
          'sw.off_forgets = !cache_set::on(getenv("RX_VK_CACHE_SET"));' in run and
          "cache_set::tidy(dir, program_cache_build_part(), logf)" in run, "the switches that leave the kept set off are not read")
    check("r.stamp = program_cache_stamp();" in files and "r.build = program_cache_build_part();" in files,
          "the decision is not made by the caches' own stamp")
    # (the files' run, which the FILES fixture runs: the folder swept and measured, then decided, then a piece moved)
    steps_run = function(steps, "inline Plan run(const std::string& dir, const Record& now, const Switches& sw, Log log) {")
    check("f.stamp = now.stamp, f.build = now.build;" in steps_run and
          steps_run.index("sweep(") < steps_run.index("std::filesystem::space(") < steps_run.index("decide(f)") <
          steps_run.index("move_all("), "the folder is not swept and measured before the decision, or a piece moves before it")
    # (a set put back whose notes are older than the player's list: the prepare before the title goes through the lists)
    prep = function(draw, "bool prepare_run(void (*progress)(size_t done, size_t total), bool background) {")
    check("const bool put_back = !background && cache_set_states_pending();" in prep and "if (shipped_new || put_back) {" in prep,
          "prepare_run does not go through the state lists for a kept set put back")
    lists = function(draw, "void state_list_prepare(void (*progress)(size_t done, size_t total)) {")
    check("if (!stopped) cache_set_states_done();" in lists, "state_list_prepare does not clear the put-back marker")
    # (a stop leaves the marker: the lists are gone through again at the next start, the set put back's notes with them)
    stop = lists[lists.index("const auto stopped = [&] {"):]
    check("lists_done.stopped = true;" in stop[:stop.index("};")], "state_list_prepare clears the put-back marker after a stop")
    check('state_list_path("triforce_pipeline_states.bin")' in draw and 'kStates = "triforce_pipeline_states.bin";' in rule,
          "cache_set.h kStates is not vk_draw.cpp's state list")

    # (the pieces: the files the caches keep by the stamp, by the names the game folder has them)
    pieces = re.findall(r'"([a-z_]+(?:\.[a-z]+)+)"', between(rule, "inline constexpr const char* kPieces[] = {", "};"))
    check(len(pieces) == len(set(pieces)) == 17 and pieces[0] == "triforce_programs.bin", "cache_set.h kPieces: %r" % pieces)
    check('"triforce_programs.bin"' in function(shader, "void disk_init() {") and
          'path.substr(0, path.size() - 4) + ".idx"' in shader, "vk_shader.cpp no longer names the program cache's files so")
    for needle in ('pcache_path = dir + "mkw_vk_pipelines.bin";', 'pcache_path + ".for"', 'pcache_path + ".base"',
                   'pcache_path + ".own"', '+ "_play.bin";',
                   'pcache_play_path + ".for"', '+ "triforce_prepared.bin";', 'prepared + ".check"', 'path + ".warming";',
                   'warming + ".skip"', '+ "triforce_pipelines.bin";', 'pipe_notes_path() + ".predicted"',
                   'pipe_notes_path() + ".plan"', '(pipe_notes_path() + ".built")', 'state_list_path("triforce_states_applied.bin")'):
        check(needle in draw, "vk_draw.cpp no longer names a cache's file as cache_set.h does: " + needle)
    magic = re.search(r"constexpr uint32_t kDiskMagic = (0x[0-9A-Fa-f]+);", shader)
    check(magic and ("kProgramsMagic = %s;" % magic.group(1)) in rule, "cache_set.h's program cache head is not vk_shader.cpp's")
    # (the switches the log names are disk_stamp's)
    lists = [re.search(r'for \(const char\* pre : \{([^}]*)\}\)', t).group(1) for t in (function(shader, "uint64_t disk_stamp() {"), files)]
    check(lists[0] == lists[1], "vk_cache_set.cpp's list of the stamp's switches is not disk_stamp's")

    # (the build part: outside the translation stamp, and the stamp's text has nothing of it; the translator's words as
    # disk_stamp takes them)
    stamped = stamped_shader()
    check("program_cache_build_part" not in stamped and "uint64_t program_cache_build_part() {" in shader,
          "program_cache_build_part is in the translation stamp: the program cache would be translated again")
    words = "program_carry::translator_words(carry_on(), kTranslationStamp, HOOKSHOT_SHADER_STAMP, translator);"
    check(words in function(shader, "uint64_t program_cache_build_part() {") and words in function(shader, "uint64_t disk_stamp() {"),
          "the build part does not take the translator's words as disk_stamp does")
    for name in ("cache_set", "vk_cache_set", "cache_set_files"):
        check(name not in stamped, "the translation stamp's text names " + name)

    # (the Robust buffers row says what changing it costs, and that switching back is quick with a kept set)
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    row = between(menu, '{TAB_DEBUG, "Robust buffers", &opts.robust,', "hn_robust, true},")
    text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', row))
    check("prepares every shader again" in text and "long first start on AMD" in text and "Switching back is quick" in text
          and "on NVIDIA cards it does not" in text, "the Robust buffers row does not say what changing it costs: " + text)
    # (the cost is the translation's, by the card: fetch_bounds_buffers depends on the row only where fetch_bounds is on,
    # which it is not on NVIDIA)
    coop = (VK / "vk_coopmat.cpp").read_text(encoding="utf-8")
    vendor = (VK / "vk_vendor.h").read_text(encoding="utf-8")
    check("fetch_bounds_buffers = dev.fixes.fetch_bounds && !dev.features.robustBufferAccess;" in coop and
          "x.fetch_bounds = f.vendor_id != 0x10DE;" in vendor, "the Robust buffers row's cost is no longer by the card")
    dev_md = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("`RX_VK_CACHE_SET=0`" in dev_md, "RX_VK_CACHE_SET is not in docs/development.md's switches")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="cache-set-") as tmp:
        tmp = Path(tmp)
        (tmp / "t.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / "cache-set"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(VK), str(tmp / "t.cpp"), "-o", str(exe)],
                       check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"cache_set.h: {r.stdout.strip()}")
        # (the files: each start's renames, deletes and records, on folders of the test's own)
        (tmp / "f.cpp").write_text(FILES, encoding="utf-8")
        exe = tmp / "cache-set-files"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(VK), str(tmp / "f.cpp"), "-o", str(exe)],
                       check=True)
        (tmp / "folders").mkdir()
        r = subprocess.run([str(exe), str(tmp / "folders")], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"cache_set_files.h: {r.stdout.strip()}\n{r.stderr.strip()}")
    print("PASS: a set of the shader caches is kept for a change of a setting, put back whole when it goes back (the state "
          "lists gone through when the player's grew), and deleted at a change of the translator, by RX_VK_CACHE_SET=0, or "
          "when no record names it")


if __name__ == "__main__":
    main()
