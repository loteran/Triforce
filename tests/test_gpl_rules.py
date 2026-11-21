"""The fast pipeline link's rules, and the driver's part of the preparation made safe to begin again (gpu/vk/gpl_rules.h;
vk_draw.cpp graphics_pipeline, gpl_link, warm_libraries, prepare_run; the Debug tab's Fast pipeline link row; no game/GPU).

The header is compiled and run as it is:
- an entry: not linked leaves it not ready for the whole build; linked makes it ready; the whole build always takes the
  link's place and hands the link back to be retired; a whole build that failed leaves the link in use; with no link the
  entry is as it was without libraries. Draw threads that read entries while workers finish them never see a ready entry
  without a pipeline it should have, nor a pipeline that was retired and destroyed;
- the replaced links are given back only after both waits, the oldest first;
- the journal names what each worker has in the driver's hands, read back from the disk while the file is still open (a
  start that died), and tells another driver's journal and an older build's empty marker apart;
- the skip list keeps each variant once, for one driver;
- starts in a row against a stand-in driver that dies on some programs: the step ends at the next start with those
  programs left out, and is left out for good only after three starts in a row that did not end it;
- a start counts as one of those three only when it died with the journal's "began" word written: for good from the stage
  libraries' first driver call, and in the program pass only while a compute warm is in the driver (Journal::Compiling).
  One that died or was stopped before, or in the translation between or after the compute warms, uses no try, however
  many there are; one killed in a compute compile does; an older build's journal (no word) counts as it did.
Then the source: the link is off unless the row or RX_VK_GPL=1 says so, the entry goes through the header's rules, the
link is made under the guard, the journal is marked around each library build, and the row is where the switch reads it.
python -I -B tests/test_gpl_rules.py --cxx clang++
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cstdio>
#include <memory>
#include <set>
#include <thread>
#include "gpl_rules.h"
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); return 1; } } while (0)
using namespace gpl_rules;
struct Entry {
    std::atomic<bool> ready{false};
    std::atomic<int> pipeline{0}, fast{0};
};
static size_t size_of(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return (size_t)-1;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fclose(f);
    return (size_t)n;
}
// A start of the driver's part as vk_draw.cpp makes it (warm_begin, warm_libraries, warm_end), against a driver that dies
// with a program of `bad` in its hands: 0 the part was made, 1 the start died, 2 the part was left out.
// die: where else the start dies (killed, or a power cut). First comes the program pass, the translation with its compute
// warms spread over it (prepare_run's warm_compute, each held by a Journal::Compiling): kEarly, after the journal is opened
// and before any of them; kAfterCompute, in the translation after compute warms on several workers came back (an update's
// minutes of graphics translation); kInCompute, with one of them in the driver (a hung compile killed)
enum Die { kNone, kEarly, kAfterCompute, kInCompute };
static int start(const std::string& warming, uint64_t stamp, int programs, const std::set<uint64_t>& bad, int workers,
                 std::vector<Id>* skipped, int* built, Die die = kNone) {
    const Left left = journal_left(warming, stamp);
    *skipped = skip_add(warming + ".skip", stamp, left.in_hand);
    if (left.found && !goes_on(left)) return remove(warming.c_str()), 2;
    Journal j;
    if (!j.open(warming, stamp, left.tries + 1)) return 2;
    if (die == kEarly) return 1;
    {   // (the compute warms of the program pass: two workers' at once, then one, then none)
        std::vector<std::unique_ptr<Journal::Compiling>> held;
        held.push_back(std::make_unique<Journal::Compiling>(j));
        held.push_back(std::make_unique<Journal::Compiling>(j));
        if (!j.counts()) return 3;
        held.erase(held.begin());
        if (!j.counts()) return 3;
        if (die == kInCompute) {   // (the process is gone: nothing comes back from the driver, no destructor runs)
            for (auto& h : held) (void)h.release();
            return 1;
        }
        held.clear();
        if (j.counts()) return 3;
        Journal::Compiling again(j);
        if (!j.counts()) return 3;
    }
    if (j.counts() || j.has_begun()) return 3;
    if (die == kAfterCompute) return 1;
    for (int p = 1; p <= programs; p += workers) {   // (the workers take a program each, all in the driver at once)
        int in_driver = 0;
        bool dies = false;
        for (int w = 0; w < workers && p + w <= programs; w++) {
            const Id id{(uint64_t)(p + w), 5};
            bool skip = false;
            for (const Id& s : *skipped) skip = skip || s == id;
            if (skip) continue;
            j.began();   // (before each driver call, as vk_draw.cpp warm_libraries: the first one writes the word)
            j.mark(w, id), in_driver++;
            dies = dies || bad.count(id.key);
        }
        if (dies) return 1;   // (the process is gone: the journal stays as the disk has it; j's file is only closed)
        for (int w = 0; w < workers; w++) j.mark(w, {});
        *built += in_driver;
    }
    j.close();
    remove(warming.c_str());
    return 0;
}

int main(int, char** argv) {
    const std::string dir = argv[1];
    // ---- an entry
    {
        Entry e;
        CHECK(!linked(e, 0) && !e.ready && e.pipeline == 0 && e.fast == 0);   // (a cold library, a refused link: as without)
        CHECK(built(e, 9) == 0 && e.ready && e.pipeline == 9 && e.fast == 0);   // (the whole build alone: nothing to give back)
        Entry f;
        CHECK(linked(f, 7) && f.ready && f.pipeline == 7 && f.fast == 7);
        CHECK(built(f, 9) == 7 && f.ready && f.pipeline == 9 && f.fast == 0);   // (replaced, and the link handed back once)
        CHECK(built(f, 11) == 0 && f.pipeline == 11);
        Entry g;
        CHECK(linked(g, 7) && built(g, 0) == 0 && g.ready && g.pipeline == 7 && g.fast == 7);   // (the build failed: the link stays)
        Entry h;
        CHECK(built(h, 0) == 0 && h.ready && h.pipeline == 0);   // (no link and no build: ready and none, "it failed")
    }
    // ---- the replaced links wait for both: the presents and the time
    {
        Retired<int> r;
        r.add(1, 100, 5000), r.add(2, 100, 9000), r.add(3, 650, 9000);
        CHECK(r.due(100 + kRetirePresents - 1, 1u << 30).empty());
        CHECK(r.due(1u << 30, 5000 + kRetireMs - 1).empty() && r.waiting() == 3);
        CHECK(r.due(100 + kRetirePresents, 5000 + kRetireMs) == std::vector<int>{1});
        CHECK(r.due(100 + kRetirePresents, 9000 + kRetireMs - 1).empty());
        CHECK(r.due(650 + kRetirePresents - 1, 1u << 30) == std::vector<int>{2} && r.waiting() == 1);
        CHECK(r.due(1u << 30, 1u << 30) == std::vector<int>{3} && r.due(1u << 30, 1u << 30).empty() && !r.waiting());
        CHECK(kRetirePresents >= 300 && kRetireMs >= 5000);   // (frames a draw's recording can be behind are a handful)
    }
    // ---- as it runs: draw threads read entries while workers finish them and the present retires the replaced links
    {
        constexpr int kEntries = 3000;
        std::vector<Entry> e(kEntries);
        std::vector<std::atomic<int>> state(kEntries);   // (0 new, 1 linked, 2 whole build done, 3 whole build failed)
        std::vector<std::atomic<bool>> dead(3 * kEntries + 8);   // (by pipeline: destroyed)
        Retired<int> retired;
        std::atomic<uint64_t> present{0};
        std::atomic<int> wrong{0}, at{0};
        std::atomic<bool> stop{false};
        // (entry i: link 3i+1 unless i is a multiple of 5 (a cold library), whole build 3i+2 unless i is a multiple of 7)
        std::thread replay([&] {
            for (int i = 0; i < kEntries; i++) {
                if (i % 5) linked(e[i], 3 * i + 1), state[i] = 1;
                at = i + 1;
                if (i % 64 == 0) std::this_thread::yield();
            }
        });
        std::vector<std::thread> threads;
        for (int t = 0; t < 2; t++)
            threads.emplace_back([&, t] {
                for (int i = t; i < kEntries; i += 2) {
                    while (at.load() <= i) std::this_thread::yield();   // (a job is pushed after the draw's thread linked)
                    const int whole = i % 7 ? 3 * i + 2 : 0;
                    if (const int old = built(e[i], whole)) retired.add(old, present.load(), 0);
                    state[i] = whole ? 2 : 3;
                }
            });
        for (int t = 0; t < 3; t++)
            threads.emplace_back([&, t] {
                unsigned x = 12345u + (unsigned)t;
                while (!stop.load()) {
                    x = x * 1664525u + 1013904223u;
                    const int i = (int)(x >> 8) % kEntries;
                    const uint64_t recorded_at = present.load();
                    if (!e[i].ready.load(std::memory_order_acquire)) continue;   // (not ready: left out or waited for, as before)
                    const int p = e[i].pipeline.load();
                    // (ready: the link or the whole build of this entry, or none only where both were not made)
                    if (p ? p != 3 * i + 1 && p != 3 * i + 2 : i % 5 != 0 || i % 7 != 0) wrong++;
                    // (what a draw recorded is not destroyed for kRetirePresents presents, and an entry never names a
                    // destroyed pipeline)
                    if (p && dead[p].load() && (present.load() < recorded_at + kRetirePresents || e[i].pipeline.load() == p)) wrong++;
                }
            });
        std::thread presents([&] {
            while (!stop.load()) {
                const uint64_t now = ++present;
                for (const int p : retired.due(now, kRetireMs)) dead[p] = true;   // (the time's wait is over: the presents' counts)
                if (now % 256 == 0) std::this_thread::yield();
            }
        });
        replay.join();
        threads[0].join(), threads[1].join();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop = true;
        for (size_t t = 2; t < threads.size(); t++) threads[t].join();
        presents.join();
        CHECK(wrong == 0);
        for (int i = 0; i < kEntries; i++) {   // (every entry ends ready, with its whole build wherever one was made)
            CHECK(e[i].ready && e[i].pipeline == (i % 7 ? 3 * i + 2 : i % 5 ? 3 * i + 1 : 0));
            CHECK(e[i].fast == (i % 7 == 0 && i % 5 ? 3 * i + 1 : 0));   // (a link still in use is still the entry's)
            if (i % 7 && i % 5) CHECK(dead[3 * i + 1] || retired.waiting());   // (a replaced link went to be retired)
            CHECK(!dead[3 * i + 2] && (i % 7 || !dead[3 * i + 1]));   // (nothing in use was destroyed)
        }
    }
    // ---- the journal, read as the disk has it while the step's file is still open
    const uint64_t stamp = 0x1111222233334444ull;
    {
        const std::string path = dir + "/a.warming";
        CHECK(!journal_left(path, stamp).found && goes_on(journal_left(path, stamp)));   // (no journal: an ordinary start)
        Journal j;
        CHECK(j.open(path, stamp, 1));
        CHECK(size_of(path) == 16 + 16 * (size_t)kSlots + 4 && kBeganAt == 16 + 16 * kSlots);
        Left l = journal_left(path, stamp);
        // (died before its first driver call: found, but no try used)
        CHECK(l.found && !l.began && l.tries == 0 && l.in_hand.empty() && goes_on(l) && !j.has_begun());
        {   // ((Triforce, 11-18) a compute warm under way: counted while the driver has it, not once it came back)
            Journal::Compiling c(j);
            l = journal_left(path, stamp);
            CHECK(j.counts() && l.began && l.tries == 1 && !j.has_begun());
            {
                Journal::Compiling d(j);   // (and another worker's at once: the word stays until both came back)
            }
            CHECK(journal_left(path, stamp).began && j.counts());
        }
        l = journal_left(path, stamp);
        CHECK(!j.counts() && !l.began && l.tries == 0 && !j.has_begun());
        j.began(), j.began();
        {   // (once the libraries' pass wrote it, a compute warm that comes back does not take it away)
            Journal::Compiling c(j);
        }
        CHECK(j.counts() && journal_left(path, stamp).began);
        CHECK(j.has_begun() && size_of(path) == 16 + 16 * (size_t)kSlots + 4);
        l = journal_left(path, stamp);
        CHECK(l.found && l.began && l.tries == 1 && l.in_hand.empty() && goes_on(l));   // (died with nothing in the driver's hands)
        j.mark(3, {0xAAAA, 1}), j.mark(5, {0xBBBB, 2}), j.mark(63, {0xCCCC, 0});
        j.mark(64, {0xDDDD, 4}), j.mark(-1, {0xEEEE, 4});   // (past the slots: no line, and no byte)
        CHECK(size_of(path) == 16 + 16 * (size_t)kSlots + 4);
        l = journal_left(path, stamp);
        CHECK(l.in_hand.size() == 3 && l.in_hand[0] == (Id{0xAAAA, 1}) && l.in_hand[1] == (Id{0xBBBB, 2}) && l.in_hand[2] == (Id{0xCCCC, 0}));
        j.mark(3, {});   // (it came back from the driver)
        l = journal_left(path, stamp);
        CHECK(l.in_hand.size() == 2 && l.in_hand[0] == (Id{0xBBBB, 2}));
        // (another driver's or other shaders' journal: found, nothing to count, no variant to blame)
        l = journal_left(path, stamp + 1);
        CHECK(l.found && l.tries == 0 && l.in_hand.empty() && goes_on(l));
        j.close();
        CHECK(journal_left(path, stamp).in_hand.size() == 2);   // (closing is not ending: only removing the file is)
        // (the count of starts: the third that did not end it is the last)
        CHECK(j.open(path, stamp, 2) && journal_left(path, stamp).tries == 1);   // (not counted until it calls the driver)
        j.began();
        CHECK(journal_left(path, stamp).tries == 2 && goes_on(journal_left(path, stamp)));
        // (the third start, ended before its first driver call: the last try is still there)
        CHECK(j.open(path, stamp, kTries) && journal_left(path, stamp).tries == kTries - 1 && goes_on(journal_left(path, stamp)));
        j.began();
        CHECK(journal_left(path, stamp).tries == kTries && !goes_on(journal_left(path, stamp)));
        // (a slot named with the word not written, which no build writes: counted, and the variant blamed)
        CHECK(j.open(path, stamp, 1));
        j.mark(0, {0xF00D, 1});
        l = journal_left(path, stamp);
        CHECK(l.began && l.tries == 1 && l.in_hand.size() == 1);
        j.close();
        // (an older build's journal, the slots and no word after them: counted as it was)
        {
            FILE* o = fopen(path.c_str(), "wb");
            const uint32_t head[2] = {kJournalMagic, 2};
            const uint64_t zero[2 * kSlots]{};
            CHECK(o && fwrite(head, 4, 2, o) == 2 && fwrite(&stamp, 8, 1, o) == 1 &&
                  fwrite(zero, 8, (size_t)(2 * kSlots), o) == (size_t)(2 * kSlots) && fclose(o) == 0);
            l = journal_left(path, stamp);
            CHECK(l.found && l.began && l.tries == 2 && l.in_hand.empty());
        }
        // (an older build's marker, an empty file: one start, no variant named)
        FILE* f = fopen(path.c_str(), "wb");
        CHECK(f && fclose(f) == 0);
        l = journal_left(path, stamp);
        CHECK(l.found && l.tries == 1 && l.in_hand.empty() && goes_on(l));
        CHECK(!Journal().open(dir + "/no-such-folder/x.warming", stamp, 1));
    }
    // ---- the skip list
    {
        const std::string path = dir + "/a.skip";
        const Id A{1, 1}, B{2, 1}, C{2, 2}, D{9, 9};
        CHECK(skip_read(path, stamp).empty());
        CHECK(skip_add(path, stamp, {}).empty() && size_of(path) == (size_t)-1);   // (nothing to add: no file made)
        CHECK(skip_add(path, stamp, {A, B}) == (std::vector<Id>{A, B}));
        CHECK(skip_add(path, stamp, {B, C, C}) == (std::vector<Id>{A, B, C}) && skip_read(path, stamp) == (std::vector<Id>{A, B, C}));
        const size_t n = size_of(path);
        CHECK(n == 16 + 3 * 16 && skip_add(path, stamp, {A}).size() == 3 && size_of(path) == n);
        CHECK(skip_read(path, stamp + 1).empty());   // (another driver's list says nothing here)
        CHECK(skip_add(path, stamp + 1, {D}) == (std::vector<Id>{D}) && skip_read(path, stamp).empty());
    }
    // ---- starts in a row against a driver that dies on some programs
    {
        const std::string w = dir + "/b.warming";
        std::vector<Id> skipped;
        int built = 0;
        // (one program the driver dies on, of 40, four workers: the next start ends the part without it)
        CHECK(start(w, stamp, 40, {10}, 4, &skipped, &built) == 1 && skipped.empty());
        built = 0;
        CHECK(start(w, stamp, 40, {10}, 4, &skipped, &built) == 0);
        CHECK(skipped.size() == 4 && built == 36);   // (the culprit and the three beside it in the driver)
        bool has = false;
        for (const Id& s : skipped) has = has || s == Id{10, 5};
        CHECK(has && !journal_left(w, stamp).found);
        // (later starts: no journal, an ordinary start; the list stays for this driver)
        CHECK(skip_read(w + ".skip", stamp).size() == 4);
    }
    {
        const std::string w = dir + "/c.warming";
        std::vector<Id> skipped;
        int built = 0;
        // (two it dies on, in different batches: the third start ends the part)
        CHECK(start(w, stamp, 40, {10, 30}, 4, &skipped, &built) == 1);
        CHECK(start(w, stamp, 40, {10, 30}, 4, &skipped, &built) == 1);
        built = 0;
        CHECK(start(w, stamp, 40, {10, 30}, 4, &skipped, &built) == 0 && skipped.size() == 8 && built == 32);
    }
    {
        const std::string w = dir + "/d.warming";
        std::vector<Id> skipped;
        int built = 0;
        // (a driver that dies on everything it is given: three starts, then the part is left out, and no fourth death)
        std::set<uint64_t> all;
        for (int i = 1; i <= 40; i++) all.insert((uint64_t)i);
        for (uint32_t i = 0; i < kTries; i++) CHECK(start(w, stamp, 40, all, 4, &skipped, &built) == 1);
        CHECK(start(w, stamp, 40, all, 4, &skipped, &built) == 2 && !journal_left(w, stamp).found);
        CHECK(skipped.size() == 4 * kTries && built == 0);
    }
    {
        const std::string w = dir + "/f.warming";
        std::vector<Id> skipped;
        int built = 0;
        // ((Triforce, 11-18) starts that died before their first driver call use no try, however many: between them, a
        // driver that dies on everything still gets its three starts, and the fourth leaves the part out)
        std::set<uint64_t> all;
        for (int i = 1; i <= 40; i++) all.insert((uint64_t)i);
        for (int i = 0; i < 5; i++) CHECK(start(w, stamp, 40, all, 4, &skipped, &built, kEarly) == 1);
        CHECK(journal_left(w, stamp).tries == 0 && skipped.empty());
        for (uint32_t i = 0; i < kTries; i++) {
            CHECK(start(w, stamp, 40, all, 4, &skipped, &built) == 1);
            if (i + 1 < kTries)
                CHECK(start(w, stamp, 40, all, 4, &skipped, &built, kEarly) == 1 && journal_left(w, stamp).tries == i + 1);
        }
        CHECK(start(w, stamp, 40, all, 4, &skipped, &built) == 2 && skipped.size() == 4 * kTries && built == 0);
    }
    {
        const std::string w = dir + "/g.warming";
        std::vector<Id> skipped;
        int built = 0;
        // (an early death after one in the driver: that one's variants stay on the list, and the next start ends the part)
        CHECK(start(w, stamp, 40, {10}, 4, &skipped, &built) == 1);
        CHECK(start(w, stamp, 40, {10}, 4, &skipped, &built, kEarly) == 1 && skipped.size() == 4);
        built = 0;
        CHECK(start(w, stamp, 40, {10}, 4, &skipped, &built) == 0 && skipped.size() == 4 && built == 36);
    }
    {
        const std::string w = dir + "/h.warming";
        std::vector<Id> skipped;
        int built = 0;
        // ((Triforce, 11-18) killed in the translation after compute warms came back, the minutes after an update: no try,
        // however many; between them a driver that dies on everything still gets its three starts)
        std::set<uint64_t> all;
        for (int i = 1; i <= 40; i++) all.insert((uint64_t)i);
        for (int i = 0; i < 5; i++) CHECK(start(w, stamp, 40, all, 4, &skipped, &built, kAfterCompute) == 1);
        CHECK(journal_left(w, stamp).tries == 0 && !journal_left(w, stamp).began && skipped.empty());
        for (uint32_t i = 0; i < kTries; i++) {
            CHECK(start(w, stamp, 40, all, 4, &skipped, &built) == 1);
            if (i + 1 < kTries)
                CHECK(start(w, stamp, 40, all, 4, &skipped, &built, kAfterCompute) == 1 && journal_left(w, stamp).tries == i + 1);
        }
        CHECK(start(w, stamp, 40, all, 4, &skipped, &built) == 2 && skipped.size() == 4 * kTries && built == 0);
    }
    {
        const std::string w = dir + "/i.warming";
        std::vector<Id> skipped;
        int built = 0;
        // (killed with a compute warm in the driver, a hung compile: each such start is a try, and the fourth leaves the
        // part out; no variant is named, compute warms have no slot)
        for (uint32_t i = 0; i < kTries; i++) {
            CHECK(start(w, stamp, 40, {}, 4, &skipped, &built, kInCompute) == 1);
            CHECK(journal_left(w, stamp).began && journal_left(w, stamp).tries == i + 1);
        }
        CHECK(start(w, stamp, 40, {}, 4, &skipped, &built) == 2 && skipped.empty() && built == 0);
    }
    {
        const std::string w = dir + "/e.warming";
        std::vector<Id> skipped;
        int built = 0;
        // (after a driver update the old journal counts for nothing: the part is made, nothing of the old list applies)
        CHECK(start(w, stamp, 8, {3}, 4, &skipped, &built) == 1);
        built = 0;
        CHECK(start(w, stamp + 7, 8, {}, 4, &skipped, &built) == 0 && skipped.empty() && built == 8);
    }
    return 0;
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    opts = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    # ---- on unless switched off, by the row or the environment (since options_version 5: the owner's default)
    wanted = between(draw, "bool gpl_wanted() {", "\n}\n")
    if 'menu_env_on("RX_VK_GPL", "fast_pipeline_link", true, 5);' not in wanted:
        raise SystemExit("gpl_wanted is not the Fast pipeline link row or RX_VK_GPL, on by default since options_version 5")
    if 'getenv("RX_VK_GPL")' in draw:
        raise SystemExit("RX_VK_GPL is read past gpl_wanted")
    row = re.search(r'\{TAB_DEBUG, "Fast pipeline link", &opts\.fast_pipeline_link, 0, 1, 1, tx_onoff,(.*?)\},\n', menu, re.S)
    if not row or not row.group(1).rstrip().endswith("nullptr, true, true"):
        raise SystemExit("no Fast pipeline link row on the Debug tab (one that applies after a restart and leaves the preset alone)")
    if menu.index('"Developer", nullptr') > row.start() or row.start() > menu.index('constexpr int kRowsEnd'):
        raise SystemExit("the Fast pipeline link row is not under the Debug tab's Developer heading")
    if "int fast_pipeline_link = 1;" not in opts:
        raise SystemExit("the Fast pipeline link row is not on by default")
    # (the row is saved by its name: the key the switch reads at the start is that name's)
    if "fast pipeline link".replace(" ", "_") != "fast_pipeline_link" or "&opts.fast_pipeline_link) return" in menu:
        raise SystemExit("the row is saved under another key than gpl_wanted reads")
    # ---- the entry goes through the header's rules
    gp = between(draw, "PipeEntry* graphics_pipeline(", "\n}\n")
    if "gpl_rules::linked(*e, linked);" not in gp:
        raise SystemExit("graphics_pipeline does not publish a link through gpl_rules::linked")
    if "gpl_retire(gpl_rules::built(*job->e, build_graphics(*job->p, job->s, job->at, job->topo)));" not in gp:
        raise SystemExit("the worker's whole build does not go through gpl_rules::built, or the replaced link is not retired")
    if gp.count("e->pipeline = ") != 1 or "e->pipeline = build_graphics(p, s, at, topo);" not in gp:
        raise SystemExit("graphics_pipeline writes an entry's pipeline past the rules (the synchronous build is the one other)")
    if gp.count("ready.store(") + gp.count("e->ready = true") != 1:
        raise SystemExit("graphics_pipeline makes an entry ready past the rules")
    if gp.index("gpl_rules::linked(") > gp.index("jobs_push([job] {"):
        raise SystemExit("the whole build is not always queued after a link")
    if "std::atomic<VkPipeline> fast{VK_NULL_HANDLE};" not in between(draw, "struct PipeEntry {", "\n};\n"):
        raise SystemExit("PipeEntry does not keep the link in use")
    # ---- the link itself, under the guard, from libraries that are all there
    # ((Triforce) the link made in gpl_link_libs, the warm's too: tests/test_pipe_warm.py)
    link = between(draw, "VkPipeline gpl_link(", "\n}\n")
    made = between(draw, "VkPipeline gpl_link_libs(", "\n}\n")
    if "if (!libs[i]) return VK_NULL_HANDLE;" not in link or "return gpl_link_libs(p, libs, n);" not in link or \
            "guarded_graphics(VK_NULL_HANDLE, &ci, &pl)" not in made or "vkCreateGraphicsPipelines(" in link + made:
        raise SystemExit("gpl_link links without every library, or outside the driver fault guard")
    # ---- the replaced links are destroyed only by the retire pass, once a present
    run = between(draw, "void gpl_retire_run() {", "\n}\n")
    if "gpl_retired.due(present, gpl_ms_now())" not in run or "if (!gpl_wanted()) return;" not in run:
        raise SystemExit("gpl_retire_run destroys something else than what is due, or runs without the link")
    if replay.count("gpl_retire_run();") != 1:
        raise SystemExit("vk_replay.cpp does not run the retire pass once a present")
    # ---- the driver's part: skip, journal around the build, and begun through warm_begin
    warm = between(draw, "bool warm_libraries(", "\n}\n")
    a, b, c = warm.index("if (skip.count(mix(p.key, p.variant))) {"), warm.index("warm_journal.mark(slot, {p.key, p.variant});"), \
        warm.index("build_graphics(p, *blank, at, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, part)")
    if not a < b < c < warm.index("warm_journal.mark(slot, {});"):
        raise SystemExit("warm_libraries does not skip, then name the variant, then build, then clear its line")
    # ((Triforce, 11-18) a start is a try from its first driver call: the word is written before the variant is named)
    if not warm.index("warm_journal.began();") < b:
        raise SystemExit("warm_libraries names a variant in the driver's hands before the journal says the start called the driver")
    if "threads.emplace_back(worker, (int)i);" not in warm:
        raise SystemExit("the workers of the driver's part have no journal slot of their own")
    prep = between(draw, "bool prepare_run(", "\n}\n")
    if prep.count("warm_begin(warming, warmed)") != 2 or "remove(warming" in prep or prep.count("warm_end(warming);") < 5:
        raise SystemExit("prepare_run begins or ends the driver's part past warm_begin and warm_end")
    # (the program pass's compute warms stay under the journal, opened before the translation, and make the start a try
    # only while one is in the driver: held by a Journal::Compiling around guarded_compute alone, not the sticky began(),
    # which would count a death anywhere in the translation after the first of them. The creation is
    # prepare_compute_pipeline's, which the pipeline cache carry's rebuild shares: pcache_carry.h)
    cs = between(prep, "const auto warm_compute = [&](Program& p) {", "\n    };\n")
    build = between(draw, "bool prepare_compute_pipeline(Program& p) {", "\n}\n")
    held = re.search(r"\{\s*const gpl_rules::Journal::Compiling compiling\(warm_journal\);\s*"
                     r"built = guarded_compute\(pcache, &ci, &pl\) == VK_SUCCESS && pl;\s*\}", build)
    if not held or "prepare_compute_pipeline(p)" not in cs or "guarded_compute(" in cs or \
            build.count("guarded_compute(") != 1 or "warm_journal.began();" in cs + build:
        raise SystemExit("prepare_run's compute warms do not hold the journal's word for the driver call alone")
    # (stopped, the state list is not gone through after the program pass: it held a stop for minutes past the kill)
    if "if (!background && !prepare_stopped()) state_list_prepare(nullptr);" not in prep:
        raise SystemExit("prepare_run goes through the state list after a stop")
    sl = between(draw, "void state_list_prepare(", "\n}\n")
    if sl.count("if (stopped()) return;") != 2 or "applied.id = 0;" not in between(sl, "const auto stopped = [&] {", "};") or \
            sl.count("if (prepare_stopped()) return;") != 2:
        raise SystemExit("state_list_prepare does not stop on a stop, or writes the lists down as gone through when it does")
    if not prep.index("if (!warm_begin(warming, warmed)) warm = warm_cs = false;") < prep.index("threads.emplace_back(worker);"):
        raise SystemExit("prepare_run's program pass (its compute warms) is no longer under warm_begin's journal")
    begin = between(draw, "bool warm_begin(", "\n}\n")
    for need in ("gpl_rules::journal_left(warming, warmed)", 'gpl_rules::skip_add(warming + ".skip", warmed, left.in_hand)',
                 "if (left.found && !gpl_rules::goes_on(left)) {", "warm_journal.open(warming, warmed, left.tries + 1)"):
        if need not in begin:
            raise SystemExit("warm_begin: " + need + " is missing")
    # (and in play: no library of a variant on the skip list is built behind a draw, at any start with the libraries)
    if "if (gpl_skipped(p)) cold[0] = cold[1] = 0;" not in gp or "gpl_skip_set(warm_skip);" not in begin or \
            ('if (libraries && !background) {\n        warm_skip_carry(warming + ".skip", libraries, warmed);\n'
             '        gpl_skip_set(gpl_rules::skip_read(warming + ".skip", warmed));') not in prep:
        raise SystemExit("a variant the driver's part leaves out still has its libraries built behind a draw")
    # (without the libraries the marker is as it was: one start that did not end the compute warm leaves it out)
    if 'logf("vk: the last start did not finish the driver\'s part of the shaders: left out");' not in prep:
        raise SystemExit("the marker's rule without stage libraries changed")
    # ---- documented
    if "| Fast pipeline link |" not in (ROOT / "readme.md").read_text(encoding="utf-8"):
        raise SystemExit("readme.md's settings table has no Fast pipeline link row")
    if "`RX_VK_GPL=1` or `0`" not in (ROOT / "docs/development.md").read_text(encoding="utf-8"):
        raise SystemExit("RX_VK_GPL is not in docs/development.md's switches")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="gpl-rules-") as tmp:
        tmp = Path(tmp)
        (tmp / "t.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / "gpl-rules"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread", "-I" + str(ROOT / "gpu/vk"),
                        str(tmp / "t.cpp"), "-o", str(exe)], check=True)
        data = tmp / "data"
        data.mkdir()
        r = subprocess.run([str(exe), str(data)], capture_output=True, text=True, timeout=300)
        if r.returncode:
            raise SystemExit(f"gpl_rules.h: {r.stdout.strip() or r.returncode}")
    print("PASS: a link is always followed and replaced by the whole build, a replaced link is retired late, and the driver's "
          "part goes on after a start that died in it, without the programs that start had in the driver's hands")


if __name__ == "__main__":
    main()
