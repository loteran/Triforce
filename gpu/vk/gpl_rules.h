// (Triforce) The rules of the fast pipeline link (VK_EXT_graphics_pipeline_library; vk_draw.cpp graphics_pipeline,
// warm_libraries, prepare_run), apart from Vulkan so that tests/test_gpl_rules.py runs them. Off unless the hidden Debug
// tab's Fast pipeline link row or RX_VK_GPL=1 switches it on: nobody has run the game with it.
//
// What "the driver step" was, and why the link stayed off "until the driver step is safe". A draw can only be linked from
// its program's two shader libraries if the driver has them compiled; compiled in the draw they stall it for a compile
// (0.2 to 1.1 s measured on NX2navi), so a draw asks for them under VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED
// and goes without when they are cold. They are warm only if the start's "driver's part" made them: the step between the
// translation and the pipelines (the start-up screen's "Compiling shaders", vk_draw.cpp warm_libraries), where every
// kept program variant's two libraries are compiled once, on every processor, into the pipeline cache. That step was not
// safe to leave on:
//  - it hands every shader of the game to the driver's compiler, and a driver's compiler faults on some of them (amdvlk64
//    on a mesh program, NVIDIA 615.78 on one vertex library at the same point of every preparation, NX2navi's issue 30;
//    the lab's start of 11-13 that hung with the driver's own lock held after such a fault);
//  - what it kept of a start that died in it was one empty file, <prepared>.warming: "this step began and did not end".
//    The next start then left the whole step out and wrote it down as done for that driver, for good. One shader the
//    driver cannot compile, a start closed from the task manager, a power cut: each left every library cold, and with
//    cold libraries no draw is linked, while each new pipeline still has two libraries built behind it on the workers
//    its whole build waits for. The link then costs and gives nothing;
//  - a fault the guard catches in that step starts the game again (driver_fault_restart), into exactly that.
// NX2navi's equivalent step is its library warm (runtime/vk_pipeline.c lw_worker, lb_warm_prepared). What makes it safe
// there, and here now: while a worker has a variant in the driver's hands, a journal on disk names it. A start that finds
// a journal left over puts the variants it names on a skip list for this driver, which the step leaves out from then on
// (each is built at its first draw, as without libraries), and goes on with the step: what the last start compiled comes
// back from the driver's own cache in a moment. Only after kTries starts in a row that began the step and did not end it
// is the step left out, as it was after the first. NX2navi's journal and skip list are not its owner's lines alone, so
// nothing of them is copied: this is written from what they do.
//
// And the rules of the link itself, which were in graphics_pipeline's lines:
//  - a draw is linked only from libraries that are all there; a cold or failed library, or a link the driver refuses,
//    leaves the entry not ready and the whole build to finish it: what a draw does with an entry that is not ready is
//    what it does without libraries, nothing more is drawn with it;
//  - the worker's whole (optimised) build always follows a link, and takes its place when it is made; if it fails, the
//    linked pipeline stays, as before;
//  - the linked pipeline a whole build replaced is given back to the driver (it was kept for the run: NX2navi measured
//    491 MiB of driver memory a session for its replaced links), but only kRetirePresents presents and kRetireMs later:
//    a draw recorded with it may still be on its way to the GPU. RX_VK_GPL_KEEP_LINKS=1: kept, as before.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace gpl_rules {

// ---- an entry: linked first, then the whole build over it

/// The draw's thread, for a new entry: link is what the libraries linked to, "none" when a library was cold or failed or
/// the driver refused the link. True: the entry is ready with it. False: nothing changed, the whole build makes it ready
template <class Entry, class Pipeline>
bool linked(Entry& e, Pipeline link) {
    if (link == Pipeline{}) return false;
    e.fast.store(link, std::memory_order_relaxed);
    e.pipeline.store(link, std::memory_order_relaxed);
    e.ready.store(true, std::memory_order_release);
    return true;
}

/// The worker, the whole build over: whole is "none" when it failed. The entry is ready from here on: with the whole
/// build, or with the link it had (the build failed), or with nothing (it failed and there was no link: the draws are left
/// out, as before). Returns the link the whole build replaced, for retire(); "none" when there is nothing to give back
template <class Entry, class Pipeline>
Pipeline built(Entry& e, Pipeline whole) {
    Pipeline replaced{};
    if (!(whole == Pipeline{})) {
        e.pipeline.store(whole, std::memory_order_release);
        replaced = e.fast.exchange(Pipeline{}, std::memory_order_acq_rel);
    }
    e.ready.store(true, std::memory_order_release);
    return replaced;
}

// ---- the replaced links, given back later

inline constexpr uint64_t kRetirePresents = 600;
inline constexpr uint64_t kRetireMs = 10000;

/// The links whole builds replaced, until both waits are over (any thread adds, the present's thread takes)
template <class Pipeline>
class Retired {
public:
    void add(Pipeline p, uint64_t present, uint64_t ms) {
        std::lock_guard lk(mu_);
        q_.push_back({p, present, ms});
    }
    /// Those replaced kRetirePresents presents and kRetireMs before now, the oldest first: the caller destroys them
    std::vector<Pipeline> due(uint64_t present, uint64_t ms) {
        std::vector<Pipeline> out;
        std::lock_guard lk(mu_);
        while (!q_.empty() && present >= q_.front().present + kRetirePresents && ms >= q_.front().ms + kRetireMs) {
            out.push_back(q_.front().p);
            q_.pop_front();
        }
        return out;
    }
    size_t waiting() {
        std::lock_guard lk(mu_);
        return q_.size();
    }

private:
    struct One {
        Pipeline p;
        uint64_t present, ms;
    };
    std::mutex mu_;
    std::deque<One> q_;
};

// ---- the driver's part of the preparation: its journal and its skip list

/// A program variant (Program::key, Program::variant)
struct Id {
    uint64_t key = 0, variant = 0;
    bool operator==(const Id& o) const { return key == o.key && variant == o.variant; }
};

inline constexpr uint32_t kJournalMagic = 0x4A574654;   // ("TFWJ")
inline constexpr uint32_t kSkipMagic = 0x53574654;      // ("TFWS")
inline constexpr int kSlots = 64;                       // (the workers of the preparation at most: prepare_threads.h)
inline constexpr uint32_t kTries = 3;                   // (starts in a row that may begin the step and not end it)

/// What a start found of the last one's journal
struct Left {
    bool found = false;       // (a journal was there: the last start began the step and did not end it)
    uint32_t tries = 0;       // (the starts in a row that did so, the last one counted; 0: none, or another driver's)
    std::vector<Id> in_hand;  // (the variants its workers had in the driver's hands when it ended)
};

/// The journal at path as the last start left it. stamp: what the driver's part is made for now (vk_draw.cpp warmed_stamp).
/// A file of another stamp is another driver's or other shaders': found, with nothing to count and no variant to blame. A
/// file without the header is an older build's empty marker: one start, no variant named
inline Left journal_left(const std::string& path, uint64_t stamp) {
    Left l;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return l;
    l.found = true;
    uint32_t head[2]{};
    uint64_t was = 0;
    if (fread(head, 4, 2, f) != 2 || fread(&was, 8, 1, f) != 1 || head[0] != kJournalMagic) {
        l.tries = 1;
    } else if (was == stamp) {
        l.tries = head[1] ? head[1] : 1;
        for (int i = 0; i < kSlots; i++) {
            uint64_t r[2];
            if (fread(r, 8, 2, f) != 2) break;
            if (r[0] || r[1]) l.in_hand.push_back({r[0], r[1]});
        }
    }
    fclose(f);
    return l;
}

/// Whether a start that found l goes on with the step (true) or leaves it out for this driver, as every start did before
inline bool goes_on(const Left& l) { return !l.found || l.tries < kTries; }

/// The journal of a step under way. Every mark is on its way to the disk before the driver is called
class Journal {
public:
    Journal() = default;
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    ~Journal() { close(); }
    /// tries: the starts in a row that began the step, this one counted
    bool open(const std::string& path, uint64_t stamp, uint32_t tries) {
        std::lock_guard lk(mu_);
        if (f_) fclose(f_);
        f_ = fopen(path.c_str(), "wb");
        if (!f_) return false;
        const uint32_t head[2] = {kJournalMagic, tries};
        const uint64_t zero[2 * kSlots]{};
        const bool ok = fwrite(head, 4, 2, f_) == 2 && fwrite(&stamp, 8, 1, f_) == 1 &&
                        fwrite(zero, 8, (size_t)(2 * kSlots), f_) == (size_t)(2 * kSlots) && fflush(f_) == 0;
        if (!ok) fclose(f_), f_ = nullptr;
        return ok;
    }
    /// Worker slot's variant, about to go to the driver ({0, 0}: it came back). A slot past the last has no line
    void mark(int slot, Id id) {
        if (slot < 0 || slot >= kSlots) return;
        std::lock_guard lk(mu_);
        if (!f_) return;
        const uint64_t r[2] = {id.key, id.variant};
        if (fseek(f_, 16 + 16 * slot, SEEK_SET) == 0 && fwrite(r, 8, 2, f_) == 2) fflush(f_);
    }
    void close() {
        std::lock_guard lk(mu_);
        if (f_) fclose(f_), f_ = nullptr;
    }

private:
    std::mutex mu_;
    FILE* f_ = nullptr;
};

/// The skip list at path for this stamp (another stamp's, or no file: empty)
inline std::vector<Id> skip_read(const std::string& path, uint64_t stamp) {
    std::vector<Id> ids;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return ids;
    uint32_t head[2]{};
    uint64_t was = 0;
    if (fread(head, 4, 2, f) == 2 && fread(&was, 8, 1, f) == 1 && head[0] == kSkipMagic && was == stamp) {
        uint64_t r[2];
        while (ids.size() < 65536 && fread(r, 8, 2, f) == 2) ids.push_back({r[0], r[1]});
    }
    fclose(f);
    return ids;
}

/// The skip list at path with more added (each variant once), written whole. Returns the list, as skip_read would give it
inline std::vector<Id> skip_add(const std::string& path, uint64_t stamp, const std::vector<Id>& more) {
    std::vector<Id> ids = skip_read(path, stamp);
    bool grew = false;
    for (const Id& id : more) {
        bool have = false;
        for (const Id& o : ids) have = have || o == id;
        if (!have) ids.push_back(id), grew = true;
    }
    if (!grew) return ids;
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return ids;
    const uint32_t head[2] = {kSkipMagic, 0};
    bool ok = fwrite(head, 4, 2, f) == 2 && fwrite(&stamp, 8, 1, f) == 1;
    for (const Id& id : ids) {
        const uint64_t r[2] = {id.key, id.variant};
        ok = ok && fwrite(r, 8, 2, f) == 2;
    }
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(path.c_str());   // (rename does not replace on Windows)
        ok = rename(tmp.c_str(), path.c_str()) == 0;
    }
    if (!ok) remove(tmp.c_str());
    return ids;
}

}   // namespace gpl_rules
