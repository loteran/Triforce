"""A submit-time capture's copies behind the game's ordering point (gpu/vk/capture_behind.h, vk_memory.cpp BehindCopy).

With RX_VK_CAPTURE_BEHIND on, capture_make takes the write marks on the submitting thread (the ordering point), copies there
the pages take_capture reads at once (the late pages, the texture storage), and leaves the rest in parts to the capture's copy
helpers while the submitting thread replays. What this proves without the game:

1. The parts (capture_behind.h, compiled): every page left behind is in exactly one part and no page copied ahead is in any;
   each part is copied exactly once whoever takes it (helpers, readers, the final wait); a reader that asks for a page gets it
   whole (a part being copied is waited for, not read half done); after finish every part is done.
2. The production code (vk_memory.cpp's copy helpers, BehindCopy, behind_read, behind_ready, memory_capture_copies_done, cut out
   and compiled with stand-ins): for the same pages and guest memory, the bytes the GPU can see once the copies are done (the
   capture buffer as flushed, non-coherent memory) equal the bytes the old copy on the submitting thread gave, page for page,
   with 0, 2 and 4 helpers, with pages in order and out of order, with the helpers shared with other captures' and the texture
   shadow's copies at the same time; the replay's reads of a page see its final bytes; the flush comes only once every part is
   copied and before any caller of memory_capture_copies_done returns; an unreadable page is "not copied" for the replay's reads
   as before (its slot zeros: the one difference, for the GPU's mirror sync of a page the game unmapped meanwhile).
3. The wiring (source read): every read of a capture buffer's bytes in vk_memory.cpp makes its page ready first; every recording
   is submitted after memory_mirror_binds_flush, which finishes the copies; the submitting thread finishes them before it goes back
   to the game; only the synchronous replay's submit leaves copies behind; the switch and the 120-present line are there.

What it cannot show: that guest memory the game's other threads write while the helpers copy reaches a capture as it did when the
submitting thread copied it all at once (a race in both: the window is the copy's time either way, longer by up to the replay's
first queue submit when the helpers are slow), and the frame time it saves: that is the lab's S1 run.

    python -I -B tests/test_capture_behind.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker, tail=""):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    if tail:
        end = text.index(tail, end) + len(tail)
    return text[start:end]


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def before(text, a, b, why):
    check(a in text and b in text and text.index(a) < text.index(b), why)


HEADER_FIXTURE = r'''#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include "capture_behind.h"
namespace cb = mkwvk::capture_behind;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::fflush(stderr); std::_Exit(1); } } while (0)
static uint64_t seed = 0x9e3779b97f4a7c15ull;
static uint64_t rnd() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }
constexpr size_t P = 256;   // (a "page" of the fixture: small, so that many rounds are cheap)
int main() {
    // (the parts: every index behind in exactly one, none copied ahead in any, cut only at an index ahead, at the end or full)
    for (int round = 0; round < 3000; round++) {
        const int64_t n = (int64_t)(rnd() % 300);
        const int64_t pp = round % 3 == 0 ? 1 : round % 3 == 1 ? 3 : 32;
        std::vector<uint8_t> ahead((size_t)n);
        for (auto& a : ahead) a = rnd() % 5 == 0;
        cb::Work w;
        w.plan(round % 7 == 0 ? nullptr : ahead.data(), n, pp);
        if (round % 7 == 0) std::fill(ahead.begin(), ahead.end(), 0);
        std::vector<int> seen((size_t)n, -1);
        int64_t end = 0;
        for (int k = 0; k < w.nparts; k++) {
            const auto& p = w.parts[k];
            CHECK(p.i0 < p.i1 && p.i1 - p.i0 <= pp && p.i0 >= end && p.state.load() == 0);
            for (int64_t i = end; i < p.i0; i++) CHECK(ahead[(size_t)i]);   // (what lies between two parts is all ahead)
            for (int64_t i = p.i0; i < p.i1; i++) {
                CHECK(!ahead[(size_t)i] && seen[(size_t)i] == -1 && w.part_of[(size_t)i] == k);
                seen[(size_t)i] = k;
            }
            CHECK(p.i1 == n || ahead[(size_t)p.i1] || p.i1 - p.i0 == pp);
            end = p.i1;
        }
        for (int64_t i = 0; i < n; i++) {
            CHECK((seen[(size_t)i] >= 0) == !ahead[(size_t)i]);
            if (ahead[(size_t)i]) CHECK(w.part_of[(size_t)i] == -1 && w.ready(i));
            else CHECK(!w.ready(i));
        }
        CHECK(w.left.load() == w.nparts && w.done() == (w.nparts == 0));
    }
    // (copied once each, readers get whole pages, finish leaves none: helpers, readers and a finisher at once)
    for (int round = 0; round < 300; round++) {
        const int64_t n = 1 + (int64_t)(rnd() % 400);
        std::vector<uint8_t> guest((size_t)n * P), out((size_t)n * P, 0), ahead((size_t)n);
        for (auto& g : guest) g = (uint8_t)(1 + rnd() % 255);
        for (auto& a : ahead) a = rnd() % 10 == 0;
        std::vector<std::atomic<int>> copies((size_t)n);
        cb::Work w;
        w.plan(ahead.data(), n, 1 + (int64_t)(rnd() % 40));
        // (a part's pages copied in two halves with a pause between: a reader let go before the end would see a half)
        auto copy = [&](int64_t a, int64_t z) {
            for (int64_t i = a; i < z; i++) {
                std::memcpy(&out[(size_t)i * P], &guest[(size_t)i * P], P / 2);
                std::this_thread::yield();
                std::memcpy(&out[(size_t)i * P + P / 2], &guest[(size_t)i * P + P / 2], P / 2);
                copies[(size_t)i]++;
            }
        };
        std::vector<std::thread> ts;
        for (int h = 0; h < 2; h++)
            ts.emplace_back([&] {
                for (int k = 0; k < w.nparts; k++)
                    if (w.claim(k)) copy(w.parts[k].i0, w.parts[k].i1), w.copied(k);
            });
        for (int r = 0; r < 2; r++)
            ts.emplace_back([&, r] {
                uint64_t s = seed + (uint64_t)r * 7919 + (uint64_t)round;
                for (int t = 0; t < 200; t++) {
                    s ^= s << 13, s ^= s >> 7, s ^= s << 17;
                    const int64_t i = (int64_t)(s % (uint64_t)n);
                    if (ahead[(size_t)i]) continue;
                    w.ensure(i, copy);
                    CHECK(w.ready(i));
                    CHECK(std::memcmp(&out[(size_t)i * P], &guest[(size_t)i * P], P) == 0);
                }
            });
        if (round % 2) std::this_thread::yield();
        w.finish(copy);
        CHECK(w.done());
        for (int64_t i = 0; i < n; i++)
            if (!ahead[(size_t)i]) CHECK(std::memcmp(&out[(size_t)i * P], &guest[(size_t)i * P], P) == 0);
        for (auto& t : ts) t.join();
        for (int64_t i = 0; i < n; i++) {
            CHECK(copies[(size_t)i].load() == (ahead[(size_t)i] ? 0 : 1));
            if (ahead[(size_t)i]) CHECK(out[(size_t)i * P] == 0);   // (not this work's: left alone)
        }
    }
    // (left behind only with helpers, enough pages behind, and few ahead)
    CHECK(!cb::worth_behind(0, 2000, 0));
    CHECK(cb::worth_behind(4, 2000, 0) && cb::worth_behind(2, 128, 0) && !cb::worth_behind(4, 127, 0));
    CHECK(cb::worth_behind(4, 2000, 512) && !cb::worth_behind(4, 2000, 513) && !cb::worth_behind(4, 600, 500));
    std::puts("PASS: capture_behind.h: parts, each copied once, readers see whole pages, finish leaves none");
    std::fflush(stdout);
    return 0;
}
'''


def production_fixture(mem):
    parts = [
        block(mem, "bool env_value_off(const char* v) {"),
        mem[mem.index("struct CopyPair {"):mem.index("\n", mem.index("struct CopyPair {"))],
        block(mem, "struct CopyPart {", ";"),
        block(mem, "void copy_part(const CopyPart& w)"),
        block(mem, "struct BehindCopy : capture_behind::Work {", ";"),
    ]
    start = mem.index("namespace {   // (BehindCopy's")
    parts.append(mem[start:mem.index("} // namespace", start) + len("} // namespace")])
    for marker, tail in (("struct CopyHelpers {", ";"), ("void copy_helper_main()", ""), ("int capture_copy_helpers()", ""),
                         ("void copy_split(CopyPart whole", ""), ("void copy_pages(const uint32_t* pages", ""),
                         ("void copy_pairs(const CopyPair* pairs", ""), ("bool behind_on() {", ""),
                         ("void behind_start(const std::shared_ptr<BehindCopy>& b) {", ""),
                         ("void memory_capture_copies_done() {", "")):
        parts.append(block(mem, marker, tail))
    return r'''#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "capture_behind.h"
namespace capture_behind = mkwvk::capture_behind;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::fflush(stderr); std::_Exit(1); } } while (0)
typedef size_t SIZE_T;
constexpr uint64_t kPageShift = 12;
constexpr uint32_t kGuestPages = 4096;
static std::vector<uint8_t> guest((size_t)kGuestPages * 4096);   // (16 MB of "guest memory")
static std::vector<uint8_t> bad(kGuestPages);                   // (pages a copy faults on: unreadable)
struct Host { uint8_t* guest_base() { return guest.data(); } uint64_t guest_size() { return guest.size(); } } host_obj, *host = &host_obj;
// (a fault partway: the bytes up to the unreadable page copied, then false, as the guarded copy leaves)
static bool guarded_copy(void* dst, const void* src, size_t n) {
    const uint64_t at = (uint64_t)((const uint8_t*)src - guest.data());
    for (uint64_t p = at >> 12; p < (at + n + 4095) >> 12; p++)
        if (bad[(size_t)p]) {
            std::memcpy(dst, src, (size_t)((p << 12) - at));
            return false;
        }
    std::memcpy(dst, src, n);
    return true;
}
static void copy_fence() {}
static int GetCurrentThread() { return 0; }
static void SetThreadDescription(int, const wchar_t*) {}
static void logf(const char*, ...) {}
struct Alloc { std::vector<uint8_t>* cpu; std::vector<uint8_t> gpu; int flushes = 0; };   // (non-coherent memory: the GPU sees what was flushed)
using VmaAllocation = Alloc*;
struct BehindCopy;
static std::atomic<BehindCopy*> flush_expect{nullptr};
static void flush_mapped(VmaAllocation a, uint64_t size);
struct Capture { std::shared_ptr<BehindCopy> behind; };
''' + "\n".join(parts) + r'''
static void flush_mapped(VmaAllocation a, uint64_t size) {
    if (BehindCopy* b = flush_expect.load()) CHECK(b->done());   // (every part copied before the flush)
    CHECK(size <= a->cpu->size());
    a->gpu.assign(a->cpu->begin(), a->cpu->begin() + (ptrdiff_t)size);
    a->flushes++;
}
static std::mt19937_64 rng(12345);
static std::vector<uint32_t> some_pages(size_t want) {   // (runs of pages, sorted, as the scan gives them)
    std::vector<uint32_t> v;
    while (v.size() < want) {
        const uint32_t s = (uint32_t)(rng() % (kGuestPages - 64)), len = 1 + (uint32_t)(rng() % 40);
        for (uint32_t k = 0; k < len; k++) v.push_back(s + k);
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    if (v.size() > want) v.resize(want);
    return v;
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "switch") {
        std::printf("behind %d\n", behind_on() ? 1 : 0);
        return 0;
    }
    for (auto& g : guest) g = (uint8_t)rng();
    std::atomic<bool> stop{false};
    // (another thread keeps the helpers busy with the texture shadow's pair copies and another capture's, as take_capture's and
    // a resumed queue's would)
    std::thread other([&] {
        std::vector<uint8_t> src(300 * 4096), dst(300 * 4096);
        std::vector<uint32_t> pages(500);
        std::vector<uint8_t> out(500 * 4096), ok(500);
        for (int r = 0; !stop.load(); r++) {
            for (auto& x : src) x = (uint8_t)(x + 1 + r);
            std::vector<CopyPair> pairs;
            for (int i = 0; i < 300; i++) pairs.push_back({dst.data() + (size_t)i * 4096, src.data() + (size_t)i * 4096});
            copy_pairs(pairs.data(), (int64_t)pairs.size());
            CHECK(dst == src);
            for (size_t i = 0; i < pages.size(); i++) pages[i] = (uint32_t)((i * 5 + (size_t)r) % 2048 + 2048);
            copy_pages(pages.data(), (int64_t)pages.size(), out.data(), ok.data());
        }
    });
    int64_t parts_total = 0, rounds = 0;
    for (int round = 0; round < 160; round++) {
        const size_t n = 128 + rng() % 1900;
        std::vector<uint32_t> pages = some_pages(n);
        if (round % 8 == 7) std::shuffle(pages.begin(), pages.end(), rng);   // (not in order: a reader waits for all of it)
        const int64_t np = (int64_t)pages.size();
        std::fill(bad.begin(), bad.end(), 0);
        if (round % 3 == 0)
            for (int k = 0; k < 3; k++) bad[pages[(size_t)(rng() % pages.size())]] = 1;
        std::vector<uint8_t> ahead((size_t)np);
        for (auto& a : ahead) a = rng() % 12 == 0;
        // (as before: the whole capture copied on the submitting thread, then flushed)
        std::vector<uint8_t> out_old((size_t)np * 4096, 0xcd), ok_old((size_t)np, 0);
        Alloc alloc_old{&out_old, {}, 0};
        copy_pages(pages.data(), np, out_old.data(), ok_old.data());
        flush_mapped(&alloc_old, (uint64_t)np * 4096);
        // (behind, as capture_make makes it: the work planned, published, its parts to the helpers, the pages ahead copied here)
        std::vector<uint8_t> out_new((size_t)np * 4096, 0xcd);
        Alloc alloc_new{&out_new, {}, 0};
        auto b = std::make_shared<BehindCopy>();
        b->pages.assign(pages.begin(), pages.end());
        b->sorted = std::is_sorted(b->pages.begin(), b->pages.end());
        b->out = out_new.data(), b->alloc = &alloc_new, b->nbytes = (uint64_t)np * 4096;
        b->ok.assign((size_t)np, 0);
        b->plan(ahead.data(), np);
        parts_total += b->nparts;
        flush_expect = b.get();
        {
            std::lock_guard lk(behind_mu);
            behind_pending = b;
            behind_now.store(b.get());
        }
        behind_start(b);
        for (int64_t i = 0; i < np;) {
            if (!ahead[(size_t)i]) { i++; continue; }
            int64_t j = i + 1;
            while (j < np && ahead[(size_t)j]) j++;
            CopyPart w{pages.data(), i, j, out_new.data(), b->ok.data()};
            copy_part(w);
            i = j;
        }
        // (the replay: reads of pages as submitted, each made ready first, as memory_read_as_submitted does; a second thread too)
        auto replay = [&](uint64_t s) {
            std::mt19937_64 r(s);
            for (int t = 0; t < 150; t++) {
                const size_t i = (size_t)(r() % (uint64_t)np);
                if (t % 2) {   // (memory_read_as_submitted's look before the pools' lock, by page, alone)
                    if (BehindCopy* now = behind_now.load()) behind_read(*now, pages[i], pages[i]);
                } else {       // (its look in the lock, and take_capture's, by index)
                    behind_ready(b.get(), (int64_t)i);
                }
                CHECK(b->ready((int64_t)i));
                CHECK(b->ok[i] == ok_old[i]);
                if (ok_old[i]) CHECK(std::memcmp(&out_new[i * 4096], &guest[(size_t)pages[i] * 4096], 4096) == 0);
            }
        };
        std::thread second(replay, (uint64_t)round * 31 + 7);
        replay((uint64_t)round * 17 + 3);
        // (the end: before a recording's submit and before the submitting thread goes back to the game; from two threads at once,
        // each back only once the flush is made)
        std::thread end_other([&] { memory_capture_copies_done(); CHECK(alloc_new.flushes == 1); });
        memory_capture_copies_done();
        CHECK(alloc_new.flushes == 1);
        end_other.join();
        second.join();
        CHECK(behind_now.load() == nullptr && !behind_pending);
        memory_capture_copies_done();   // (nothing behind: nothing done, no second flush)
        CHECK(alloc_new.flushes == 1);
        // (what the GPU sees: page for page the old copy's, for every page the capture holds; an unreadable page none's)
        for (size_t i = 0; i < (size_t)np; i++) {
            CHECK(b->ok[i] == ok_old[i]);
            if (ok_old[i]) CHECK(std::memcmp(&alloc_new.gpu[i * 4096], &alloc_old.gpu[i * 4096], 4096) == 0);
            else if (!ahead[i]) CHECK(std::all_of(&alloc_new.gpu[i * 4096], &alloc_new.gpu[i * 4096] + 4096, [](uint8_t x) { return x == 0; }));
        }
        flush_expect = nullptr;
        rounds++;
    }
    stop = true;
    other.join();
    // (every part copied once: by a helper, a reader or the end)
    CHECK(st_cg_parts_helper.load() + st_cg_parts_reader.load() + st_cg_parts_finish.load() == parts_total);
    std::printf("PASS: %lld captures behind: the GPU sees the old copy's bytes page for page; %lld parts by the helpers, %lld by "
                "readers, %lld at the end, %lld waits\n", (long long)rounds, (long long)st_cg_parts_helper.load(),
                (long long)st_cg_parts_reader.load(), (long long)st_cg_parts_finish.load(), (long long)st_cg_waited.load());
    std::fflush(stdout);
    std::_Exit(0);   // (the helpers wait for good, as in the game)
}
'''


def check_source(mem, replay, texture, dev):
    # (every read of a capture buffer's bytes makes its page ready first)
    read = block(mem, "bool memory_read_as_submitted(")
    before(read, "behind_read(*b, page0, page1)", "std::lock_guard lk(pools_mu)",
           "memory_read_as_submitted: the pages of a capture behind are not made ready before the pools' lock")
    before(read, "behind_ready(bc, e.slot);", "std::memcpy((uint8_t*)dst",
           "memory_read_as_submitted: a capture's bytes read before the page is ready")
    before(read, "if (!bc->ok[e.slot]) continue;", "std::memcpy((uint8_t*)dst",
           "memory_read_as_submitted: an unreadable page behind is read from the capture")
    late = block(mem, "void late_unresolved_read(uint64_t page0, uint64_t page1) {")
    before(late, "behind_ready(bc, e.slot);", "guarded_copy((uint8_t*)b->second.map",
           "late_unresolved_read: the second copy may be overwritten by the first")
    take = block(mem, "bool take_capture(")
    at = 0
    while True:
        at = take.find("c->map + (uint64_t)", at)
        if at < 0:
            break
        check("cap_ready(*c, " in take[max(0, at - 420):at], "take_capture: a read of the capture's bytes at %d without cap_ready" % at)
        at += 1
    at = 0
    while True:
        at = take.find("second.map + (uint64_t)", at)
        if at < 0:
            break
        check("behind_ready(b->second.behind.get()" in take[max(0, at - 200):at], "take_capture: an older capture's bytes read unready")
        at += 1
    before(take, "CapBuf b{value, c->buffer, c->alloc, c->bsize, c->map};", "b.behind = c->behind;",
           "take_capture: the kept capture does not carry its copies behind")
    check("if (c->map && c->behind) memory_capture_copies_done();" in take, "take_capture: a buffer given back with copies behind into it")
    # (the parts: taken by the helpers only when unclaimed)
    helper = block(mem, "void copy_helper_main()")
    before(helper, "if (b.claim(w.part))", "behind_copy(b, b.parts[w.part].i0", "copy_helper_main: a part copied without claiming it")
    # (capture_make: the last copies done before the marks; this capture's published before its helpers start; the pages read at
    # once copied ahead; a page behind counted as copied)
    make = block(mem, "std::unique_ptr<Capture> capture_make(bool behind) {")
    before(make, "memory_capture_copies_done();", "host->collect_written_in(", "capture_make: the copies behind not done before the scan")
    check("if (behind && behind_on())" in make, "capture_make: the copies go behind without the caller's leave or the switch")
    for a, b in (("behind_now.store(b.get()", "behind_start(b);"), ("c->behind = b;", "behind_start(b);"),
                 ("behind_start(b);", "copy_part(w);"), ("for (uint32_t i : c->late) na += !ahead[i], ahead[i] = 1;", "b->plan(ahead.data(), n);"),
                 ("g < std::prev(it)->second) ahead[(size_t)i] = 1", "b->plan(ahead.data(), n);")):
        before(make, a, b, "capture_make: %s before %s" % (a, b))
    check("if (!ahead[(size_t)i] || b->ok[(size_t)i]) c->copied.push_back(pages[i])" in make,
          "capture_make: the pages copied ahead that failed are counted as copied, or those behind are not")
    behind_branch = make[make.index("if (b) {"):make.index("} else {", make.index("if (b) {"))]
    check("flush_mapped(" not in behind_branch, "capture_make: the copies behind flushed before they are made")
    # (before a recording is submitted: memory_mirror_binds_flush, first thing)
    flush = block(mem, "void memory_mirror_binds_flush() {")
    before(flush, "memory_capture_copies_done();", "return", "memory_mirror_binds_flush does not finish the copies first")
    up = block(texture, "bool texture_uploads_recorded() {")
    check("memory_mirror_binds_flush();" in up, "texture_uploads_recorded does not flush the mirrors' binds (and the copies behind)")
    sub = block(replay, "void submit_recording(")
    before(sub, "texture_uploads_recorded();", "pend.cbs.push_back(", "submit_recording: a recording batched before the copies are done")
    before(sub, "texture_uploads_recorded();", "vkQueueSubmit2(", "submit_recording: a recording submitted before the copies are done")
    before(replay, "j->uploads = texture_uploads_recorded();", "seg_cv.wait(l, [j] { return j->done.load(); });",
           "a parallel replay's segment is submitted before its uploads' flush")
    seg = replay[replay.index("seg_cv.wait(l, [j] { return j->done.load(); });"):]
    check(seg.index("seg_cv.wait(l, [j] { return j->done.load(); });") < seg.index("vkQueueSubmit2(dev.queue, 1, &si, j->r->fence)"),
          "a parallel replay's segment is submitted before it is done")
    # ((Triforce) the early segments' path (replay_handle_early, RX_VK_SEG_EARLY) submits its segments as replay_handle does: two
    # segment submits, each after its job's wait, and each job flushes its uploads (run_job: texture_uploads_recorded) before it is done)
    seg_submit = "vkQueueSubmit2(dev.queue, 1, &si, j->r->fence)"
    seg_wait = "seg_cv.wait(l, [j] { return j->done.load(); });"
    check(replay.count(seg_submit) == 2 and replay.count(seg_wait) == 2 and
          replay.count("j->uploads = texture_uploads_recorded();") == 2,
          "the parallel replay's two segment paths no longer submit as this test knows")
    at = 0
    for _ in range(2):
        w = replay.index(seg_wait, at)
        s = replay.index(seg_submit, at)
        check(w < s, "a parallel replay's segment (one of the two paths) is submitted before it is done")
        at = s + 1
    check(replay.count("vkQueueSubmit2(") == 5, "vk_replay.cpp submits in a place this test does not know: check that the copies "
          "behind are done before it")
    # (the submitting thread: the synchronous replay leaves copies behind, and it finishes them before it returns)
    q = block(replay, "void submit_q(")
    sync = q[:q.index("std::lock_guard lk(submit_order_mu);")]
    # ((Triforce) a held submit's capture (HOS_NVN_HOLD=2: its game thread already runs again either way) leaves its copies
    # behind too unless RX_VK_HELD_BEHIND=0: the synchronous path's call is memory_capture_submit(!tl_submit_held || held_behind))
    sync_call = "memory_capture_submit(!tl_submit_held || held_behind);"
    before(sync, sync_call, "~CopiesDone() { memory_capture_copies_done(); }",
           "submit_q: the copies behind are not done before the game's thread returns")
    before(sync, "} copies_done;", "return do_submit(handles, n);", "submit_q: the replay runs after the copies are done")
    # ((Triforce, async_rules.h 2) the replay thread's capture shares its copies with the helpers too, and has every one of them
    # made before the hand-over: the game goes on once the submit returns, and nothing is left behind it)
    own = q[q.index("std::lock_guard lk(submit_order_mu);"):]
    check(own.count("memory_capture_submit(true);") == 1, "submit_q: the replay thread's capture is not the one this test knows")
    before(own, "memory_capture_submit(true);", "memory_capture_copies_done();",
           "submit_q: the replay thread's capture leaves copies behind while the game goes on")
    before(own, "memory_capture_copies_done();", "enqueue(std::move(j));",
           "submit_q: the replay thread's capture is handed over before its copies are made")
    check(replay.count("memory_capture_submit(true)") == 1 and replay.count(sync_call) == 1,
          "a capture other than the two submits' leaves copies behind")
    # (the figures and the switch)
    check("vk: a frame's captures at the game's submits:" in replay, "no 120-present line of the captures on the game's threads")
    check("RX_VK_CAPTURE_BEHIND" in dev, "docs/development.md: RX_VK_CAPTURE_BEHIND is not in the switches table")


def run(cmd, **kw):
    kw.setdefault("check", True)
    if os.name == "nt":
        kw["creationflags"] = subprocess.BELOW_NORMAL_PRIORITY_CLASS
    return subprocess.run(cmd, **kw)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    vk = ROOT / "gpu" / "vk"
    mem = (vk / "vk_memory.cpp").read_text(encoding="utf-8")
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    texture = (vk / "vk_texture.cpp").read_text(encoding="utf-8")
    dev = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
    check_source(mem, replay, texture, dev)
    with tempfile.TemporaryDirectory(prefix="capture-behind-") as tmp:
        for name, text in (("header", HEADER_FIXTURE), ("production", production_fixture(mem))):
            cpp = Path(tmp) / (name + ".cpp")
            exe = Path(tmp) / (name + (".exe" if os.name == "nt" else ""))
            cpp.write_text(text, encoding="utf-8")
            # (the production block's other counters and cap_ready are capture_make's and take_capture's, not cut out here)
            run([a.cxx, "-std=c++20", "-O2", "-pthread", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS",
                 "-Wno-unused-variable", "-Wno-unused-function", "-I", str(vk), str(cpp), "-o", str(exe)])
            if name == "header":
                run([str(exe)], timeout=300)
                continue
            for threads in ("0", "2", "4"):
                run([str(exe)], timeout=300, env={**os.environ, "RX_VK_CAPTURE_THREADS": threads})
            for value, want in ((None, "1"), ("0", "0"), ("off", "0"), ("No", "0"), ("1", "1")):
                env = {k: v for k, v in os.environ.items() if k != "RX_VK_CAPTURE_BEHIND"}
                if value is not None:
                    env["RX_VK_CAPTURE_BEHIND"] = value
                out = run([str(exe), "switch"], env=env, capture_output=True, text=True, timeout=60).stdout
                check(out.strip() == "behind " + want, "RX_VK_CAPTURE_BEHIND=%s: %s" % (value, out.strip()))
    print("PASS: the capture's copies behind the submit: the same bytes reach the GPU, every reader makes its page ready first")


if __name__ == "__main__":
    main()
