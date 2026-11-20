// The early tail's model (tests/test_tail_early.py; gpu/vk/tail_early.h, gpu/vk/vk_replay.cpp "the early tail"): the production
// functions, cut out of vk_replay.cpp into tail_early_cut.inc (the tail's state, tail_hold, tail_seen, tail_begin, tail_at_dispatch,
// tail_words, tail_words_record, tail_end, tail_early_record, finish_recording, gpu_write), run against stand-ins that record the
// command buffers they write into and a queue that runs them in order over a guest memory and its mirror. A driver replays a frame's
// stream as replay() does: the scene, the gameplay upscale's dispatch (tail_at_dispatch), the tail (its draws, uniform updates,
// event signals as NOP_SIGNAL_EVENT gives them to tail_last and tail_hold), then finish_recording. The checks on the queue:
//   order    an early tail's event words reach guest memory after every write-back of the work before the tail and after the
//            tail's uploads, and before any of the tail's commands
//   late     after an early tail's first word nothing writes guest memory but the tail's uniform updates' write-backs, and nothing
//            reads it
//   values   every event word ends with the value the console's signals give it, in guest memory, and an early tail's in the mirror
//   end      a tail that is not early has its words reach guest memory after all its commands, as before
//   barrier  each signal held in a tail keeps a full barrier at its place, before the tail's next command
//   ub       a tail's uniform updates are written in the tail's recording, after its commands; the scene's in the scene's
//   verdict  the tail was judged as the scenario expects
// A scenario that breaks a check prints "FAIL <check>" and exits 1. tests/test_tail_early.py runs each scenario, then each mutation
// of the cut code or the header with the scenario that must then fail.
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "tail_early.h"

namespace model {

enum Kind { BARRIER, XFER_BARRIER, HOST_BARRIER, UPD, WB, DRAW, TAILDRAW, UPLOAD, GUEST_READ, BAKE, SIG };
struct Cmd {
    Kind kind;
    bool import = false;   // (UPD: into guest memory itself; else the mirror)
    uint64_t addr = 0;
    uint32_t v = 0;
    int rec = 0;           // (the recording it was recorded in)
};
struct CB {
    std::vector<Cmd> cmds;
    int rec = 0;
};
std::vector<std::unique_ptr<CB>> cbs;
CB* cb_of(VkCommandBuffer c) { return reinterpret_cast<CB*>(c); }
VkCommandBuffer new_cb(int rec) {
    cbs.push_back(std::make_unique<CB>());
    cbs.back()->rec = rec;
    return reinterpret_cast<VkCommandBuffer>(cbs.back().get());
}
void put(VkCommandBuffer c, Cmd k) {
    k.rec = cb_of(c)->rec;
    cb_of(c)->cmds.push_back(k);
}
const VkBuffer kImport = reinterpret_cast<VkBuffer>(uintptr_t(0x1000)), kMirror = reinterpret_cast<VkBuffer>(uintptr_t(0x2000));

// ---- what a scenario sets
struct Scenario {
    bool written = false, guest = false, program = false, bake = false, outside = false, worker = false, up = true, cut = false;
    tail_early::Why want = tail_early::EARLY;
} sc;
std::set<uint64_t> unmirrored, outside_pools, ub_addrs, event_words;
std::map<uint64_t, uint32_t> console;   // (what the console's signals leave in each event word)
std::map<uint64_t, uint32_t> cpu_guest;   // (guest memory as the CPU wrote it at the replay)
}  // namespace model

using model::put;

// ---- the stand-ins for what the cut functions call (their names as vk_replay.cpp's)
struct BufferRef {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
    uint64_t address = 0;
    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};
struct NvnState {};
struct Recording {
    VkCommandBuffer cmd = VK_NULL_HANDLE, upload = VK_NULL_HANDLE;
    uint64_t value = 0;
    int id = 0;
    bool uploads = false;   // (the replay recorded uploads into it: texture_uploads_recorded)
};
enum class Mode { Serial, Dry, Worker };
Mode rmode = Mode::Serial;
size_t ub_limit = SIZE_MAX;
struct UbUpdate {
    uint64_t dst;
    uint32_t v;
};
std::vector<UbUpdate> ub_updates;
size_t ub_first_of_recording = 0;
Recording* rec_cur = nullptr;
uint64_t rec_cur_value = 0;
int rec_parts = 0;
int64_t part_work = 0;
constexpr int kMaxParts = 32;
std::atomic<bool> par_active{false};
std::atomic<uint64_t> work_epoch{0};
uint64_t work_between() { return work_epoch.load(); }   // ((Triforce, P4) cmd_work.h's: what this thread recorded, the bake's write-back below)
uint64_t done_max = 0, signaled = 0;
uint64_t oldest_unfinished() { return UINT64_MAX; }
std::vector<std::unique_ptr<Recording>> recordings;
std::vector<VkCommandBuffer> queue;   // (the command buffers in the order they went to the queue)
uint64_t st_written = 0, st_guest = 0, st_programs = 0;
std::vector<uint64_t> noted;   // (the recording's written ranges: its write-back's)
bool watch = false;

void logf(const char*, ...) {}
bool dump_on() { return false; }
bool tail_wanted() { return true; }
bool upscale_dispatch(NvnState&) { return model::sc.up; }
void end_pass(VkCommandBuffer) {}
void event_writes_flush(VkCommandBuffer, const char*) {}
void full_barrier(VkCommandBuffer c, const char* = nullptr) { put(c, {model::BARRIER}); }
void vkCmdPipelineBarrier2(VkCommandBuffer c, const VkDependencyInfo* d) {
    const VkPipelineStageFlags2 dst = d->pMemoryBarriers[0].dstStageMask;
    put(c, {(dst & VK_PIPELINE_STAGE_2_HOST_BIT) ? model::HOST_BARRIER
            : dst == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT ? model::XFER_BARRIER : model::BARRIER});
}
void vkCmdUpdateBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize off, VkDeviceSize n, const void* data) {
    uint32_t v = 0;
    std::memcpy(&v, data, n < 4 ? n : 4);
    model::Cmd k{model::UPD};
    k.import = b == model::kImport, k.addr = off, k.v = v;
    put(c, k);
}
void memory_bind_event_write(bool) {}
void memory_bind_counts(uint64_t* written, uint64_t* guest) { *written = st_written, *guest = st_guest; }
void draw_tail_watch(bool on) { watch = on; }
uint64_t draw_tail_programs() { return st_programs; }
void guest_cpu_write(uint64_t gpu, const void* d, uint64_t n) {
    uint32_t v = 0;
    std::memcpy(&v, d, n < 4 ? n : 4);
    model::cpu_guest[gpu] = v;
}
BufferRef gpu_buffer(uint64_t gpu, uint64_t n) {
    if (model::outside_pools.count(gpu)) return {};
    return {model::kImport, gpu, n, 0};
}
BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t n, bool written) {
    if (model::outside_pools.count(gpu)) return {};
    if (written) {
        st_written++;
        noted.push_back(gpu);
        return {model::kMirror, gpu, n, 0};
    }
    if (model::unmirrored.count(gpu)) return st_guest++, BufferRef{model::kImport, gpu, n, 0};
    return {model::kMirror, gpu, n, 0};
}
void gpu_write(VkCommandBuffer cmd, uint64_t gpu, const void* data, uint64_t n);   // (cut)
void flush_ub_updates(VkCommandBuffer cmd, size_t lim = SIZE_MAX) {   // (vk_replay.cpp's: the uniform updates as gpu_write writes them)
    const size_t end = lim < ub_updates.size() ? lim : ub_updates.size();
    if (ub_first_of_recording >= end) return;
    full_barrier(cmd);
    for (size_t i = ub_first_of_recording; i < end; i++) gpu_write(cmd, ub_updates[i].dst, &ub_updates[i].v, 4);
    full_barrier(cmd);
}
void texture_end_recording(VkCommandBuffer cmd) {   // (a bake's write-back into guest memory, in the tail's recording)
    if (model::sc.bake && rec_cur == nullptr && model::cb_of(cmd)->rec > 1) put(cmd, {model::BAKE}), work_epoch++;
}
void memory_end_submission(VkCommandBuffer cmd) {
    for (const uint64_t a : noted) {
        model::Cmd k{model::WB};
        k.addr = a;
        put(cmd, k);
    }
    noted.clear();
}
void memory_begin_submission() { noted.clear(); }
Recording& begin_recording(uint64_t value) {
    recordings.push_back(std::make_unique<Recording>());
    Recording& r = *recordings.back();
    r.id = (int)recordings.size();
    r.value = value;
    r.cmd = model::new_cb(r.id), r.upload = model::new_cb(r.id);
    return r;
}
bool tail_early_record(Recording& r);   // (cut)
void submit_recording(Recording& r, bool = true, uint64_t = 0, bool = false) {   // (vk_replay.cpp's: the upload command buffer, then the commands)
    bool uploads = r.uploads;
    if (tail_early_record(r)) uploads = true;
    if (uploads) queue.push_back(r.upload);
    queue.push_back(r.cmd);
}

#include "tail_early_cut.inc"

namespace model {

int failures = 0;
void fail(const char* check, const char* what) {
    std::printf("FAIL %s: %s\n", check, what);
    failures++;
}

/// A signal as NOP_SIGNAL_EVENT gives it: the add on the last held value, else on guest memory as the CPU sees it
void signal(VkCommandBuffer& cmd, uint64_t gpu, bool add, uint32_t value) {
    event_words.insert(gpu);
    uint32_t v = value;
    if (add) {
        uint32_t old = 0;
        if (!tail_last(gpu, &old)) old = cpu_guest.count(gpu) ? cpu_guest[gpu] : 0;
        v = old + value;
    }
    console[gpu] = add ? console[gpu] + value : value;
    if (tl.on) put(cmd, {SIG});
    if (tail_hold(cmd, gpu, v)) return;
    end_pass(cmd);
    full_barrier(cmd);
    memory_bind_event_write(true);
    gpu_write(cmd, gpu, &v, 4);
    memory_bind_event_write(false);
}

void run() {
    unmirrored = {0x9000};
    if (sc.outside) outside_pools.insert(0x5008);
    rec_cur_value = 7;
    Recording& first = begin_recording(7);
    rec_cur = &first;
    VkCommandBuffer cmd = first.cmd;
    // the scene: draws, a written binding (a write-back), a uniform update, an event's signal
    put(cmd, {DRAW});
    gpu_buffer_bind(0x3000, 64, true);
    put(cmd, {DRAW});
    ub_updates.push_back({0x4000, 11});
    signal(cmd, 0x5000, false, 1);
    put(cmd, {DRAW});
    // a parallel replay's rest after its segments: the dry pass listed the tail's uniform update already
    if (sc.worker) {
        ub_updates.push_back({0x4010, 22});
        rmode = Mode::Worker, ub_limit = 1, tail_here = true;
    }
    // the gameplay upscale's dispatch
    NvnState s;
    tail_at_dispatch(cmd, s);
    if (sc.up && !tl.on) fail("verdict", "no tail began at the upscale's dispatch");
    if (sc.up && rec_cur == &first) fail("verdict", "the tail did not get a recording of its own");
    // the tail: its uploads, the upscaler, the interface with its uniform updates, the frame's event, another word
    rec_cur->uploads = true;
    put(rec_cur->upload, {UPLOAD});
    put(cmd, {TAILDRAW});
    if (!sc.worker) ub_updates.push_back({0x4010, 22});
    else ub_limit++;
    ub_addrs = {0x4000, 0x4010};
    put(cmd, {TAILDRAW});
    if (sc.written) gpu_buffer_bind(0x6000, 16, true), put(cmd, {TAILDRAW});
    if (sc.guest) gpu_buffer_bind(0x9000, 16, false), put(cmd, {GUEST_READ});
    if (sc.program) st_programs += watch, put(cmd, {GUEST_READ});
    signal(cmd, 0x5004, false, 5);
    put(cmd, {TAILDRAW});
    // (an add on a word set before it in the same recording: held in a tail, it adds to the held value. Without a tail it adds to
    // what guest memory has at the replay, as before (the set not run yet): left out there)
    if (sc.up) signal(cmd, 0x5004, true, 2);
    signal(cmd, sc.outside ? 0x5008 : 0x500c, false, 9);
    put(cmd, {TAILDRAW});
    rmode = Mode::Serial, ub_limit = SIZE_MAX, tail_here = false;
    if (sc.cut) {   // (the tail's recording submitted by something other than finish_recording: the safety net)
        Recording& r = *rec_cur;
        rec_cur = nullptr;
        flush_ub_updates(r.cmd);
        memory_end_submission(r.cmd);
        submit_recording(r, true, 7);
        return;
    }
    finish_recording(7);
}

void check() {
    // the queue run in order: guest memory and the mirror
    std::map<uint64_t, uint32_t> guest = cpu_guest, mirror;
    struct At { size_t i; Cmd k; };
    std::vector<At> all;
    for (VkCommandBuffer c : queue)
        for (const Cmd& k : cb_of(c)->cmds) all.push_back({all.size(), k});
    std::map<uint64_t, uint32_t> ub_v;
    for (const UbUpdate& u : ub_updates) ub_v[u.dst] = u.v;
    const int tail_rec = sc.up ? (int)recordings.size() : -1;
    size_t first_word = SIZE_MAX, first_tail = SIZE_MAX, last_tail = 0, last_scene_wb = 0, last_upload = 0, last_word = 0;
    bool any_word = false;
    for (const At& a : all) {
        const Cmd& k = a.k;
        const bool tail = k.rec == tail_rec;
        if (k.kind == UPD) {
            // (a mirror write of a uniform update carries its value; so does an event's)
            (k.import ? guest : mirror)[k.addr] = k.v;
            if (k.import && event_words.count(k.addr) && tail) {
                first_word = std::min(first_word, a.i), last_word = a.i, any_word = true;
            }
        } else if (k.kind == WB) {
            guest[k.addr] = mirror[k.addr];
            if (!tail) last_scene_wb = a.i;
            if (event_words.count(k.addr) && tail) last_word = a.i, any_word = true;
        } else if (k.kind == TAILDRAW || k.kind == GUEST_READ) {
            first_tail = std::min(first_tail, a.i), last_tail = a.i;
        } else if (k.kind == UPLOAD) {
            last_upload = a.i;
        }
    }
    // verdict
    if (sc.up) {
        uint64_t judged = 0;
        for (uint64_t n : tl_stats.why) judged += n;
        if (judged != 1 || tl_stats.why[sc.want] != 1) fail("verdict", "the tail was not judged as expected");
    }
    // (what the tail did, not what the scenario expects of it: a tail let go early by a rule taken away must break the stream)
    const bool early = sc.up && tl_stats.why[tail_early::EARLY] == 1;
    if (early) {
        if (first_word == SIZE_MAX) fail("order", "no event word reached guest memory before the tail");
        else {
            if (first_word < last_scene_wb) fail("order", "an event word before a write-back of the scene");
            if (first_word < last_upload) fail("order", "an event word before the tail's uploads");
            if (last_word > first_tail) fail("order", "an event word after a command of the tail");
        }
        for (const At& a : all) {
            if (a.i <= first_word) continue;
            const Cmd& k = a.k;
            if (k.kind == BAKE) fail("late", "a texture written back into guest memory after the event words");
            if (k.kind == GUEST_READ) fail("late", "guest memory read after the event words");
            if (k.kind == WB && !ub_addrs.count(k.addr)) fail("late", "a write-back other than a uniform update's after the event words");
            if (k.kind == UPD && k.import && !(event_words.count(k.addr) && a.i <= last_word))
                fail("late", "guest memory written after the event words");
        }
        for (const uint64_t w : event_words)
            if (w != 0x5000 && mirror[w] != console[w]) fail("values", "the mirror's word differs from guest memory's");
    } else if (sc.up && any_word && last_word < last_tail) {
        fail("end", "a tail that is not early had an event word reach guest memory before its end");
    }
    for (const uint64_t w : event_words)
        if (guest[w] != console[w]) {
            char b[96];
            std::snprintf(b, sizeof b, "word %#llx has %u, the console's signals %u", (unsigned long long)w, guest[w], console[w]);
            fail("values", b);
        }
    // each signal held in the tail keeps a full barrier at its place
    for (VkCommandBuffer c : queue) {
        const auto& v = cb_of(c)->cmds;
        for (size_t i = 0; i < v.size(); i++)
            if (v[i].kind == SIG) {
                size_t j = i + 1;
                while (j < v.size() && v[j].kind != BARRIER && v[j].kind != TAILDRAW) j++;
                if (j == v.size() || v[j].kind != BARRIER) fail("barrier", "a held signal without its full barrier before the next command");
            }
    }
    // the uniform updates: the scene's in the scene's recording, the tail's in the tail's after its commands
    if (sc.up) {
        size_t tail_ub = SIZE_MAX;
        bool scene_ub = false;
        for (const At& a : all)
            if (a.k.kind == UPD && !a.k.import && ub_addrs.count(a.k.addr)) {
                if (a.k.addr == 0x4000 && a.k.rec != tail_rec) scene_ub = true;
                if (a.k.addr == 0x4010) {
                    if (a.k.rec != tail_rec) fail("ub", "the tail's uniform update written before the tail");
                    tail_ub = a.i;
                }
                if (a.k.addr == 0x4000 && a.k.rec == tail_rec) fail("ub", "the scene's uniform update written in the tail");
            }
        if (!scene_ub) fail("ub", "the scene's uniform update not written in the scene's recording");
        if (tail_ub == SIZE_MAX || tail_ub < last_tail) fail("ub", "the tail's uniform update not after the tail's commands");
        for (const auto& [a, v] : ub_v)
            if (guest[a] != v) fail("ub", "a uniform update did not reach guest memory");
    }
}

}  // namespace model

int main(int argc, char** argv) {
    using namespace model;
    const std::string name = argc > 1 ? argv[1] : "clean";
    if (name == "clean") sc = {};
    else if (name == "worker") sc = {}, sc.worker = true;
    else if (name == "written") sc = {}, sc.written = true, sc.want = tail_early::WRITES;
    else if (name == "guest") sc = {}, sc.guest = true, sc.want = tail_early::GUEST;
    else if (name == "program") sc = {}, sc.program = true, sc.want = tail_early::PROGRAMS;
    else if (name == "bake") sc = {}, sc.bake = true, sc.want = tail_early::BAKE;
    else if (name == "outside") sc = {}, sc.outside = true, sc.want = tail_early::EVENT;
    else if (name == "cut") sc = {}, sc.cut = true, sc.want = tail_early::CUT;
    else if (name == "none") sc = {}, sc.up = false;
    else return std::printf("unknown scenario %s\n", name.c_str()), 2;
    run();
    check();
    if (failures) return 1;
    std::printf("ok %s: %llu tails judged, %llu signals, %llu written before their tail\n", name.c_str(),
                (unsigned long long)tl_stats.tails, (unsigned long long)tl_stats.signals, (unsigned long long)tl_stats.moved);
    return 0;
}
