// A parallel replay's new images in queue order: the production upload command buffer state, create()'s initialization,
// bc_scratch_layout, the texture segment hooks, replay_handle's segment jobs and submits and the init pools' startup,
// included from the init_*.inc files tests/test_parallel_init_order.py cuts from the source. Vulkan is a spy: commands
// are recorded per command buffer (never into one not begun, ended or submitted), and the submitted ones are played in
// queue order, the image layouts tracked as the GPU would see them. The workers run on threads of their own: one after
// another in the order a case sets (the one that makes an image first need not be the first segment in the queue), or
// all at once, held at fixed points by latches (a worker waits for another to be inside its init recording, or for an
// earlier segment's submit). A command buffer can be made to fail its begin or its end; replay_handle's part after the
// segments (begin_recording) is modelled as a recording of its own.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "../gpu/vk/helper_spin.h"   // (replay_handle's spin for a segment before its wait: in init_jobs.inc)
#include "../gpu/vk/upload_earliest.h"   // ((Triforce) the upload's segment: UploadRedirect, in init_upload.inc)

struct Op {
    char kind;   // F: full barrier, T: UNDEFINED -> GENERAL, B: another image barrier, C: clear, U: a use (copy, dispatch, draw),
                 // G: a GPU timestamp (a segment's, gpu_time_begin and gpu_time_end); W: an upload of the submission's bytes into
                 // it (texture_touch's, or the twin), R: a draw that samples it
    VkImage image;
};
struct VkCommandBuffer_T {
    std::string name;
    std::vector<Op> ops;
    bool begun = false, ended = false, submitted = false;
};
std::vector<VkCommandBuffer_T*> all_cbs, queue;
std::vector<std::unique_ptr<VkCommandBuffer_T>> owned_cbs;
std::set<int> published_slots;   // draw-stream CPU writes published before a worker slot reaches the GPU
std::atomic<int> failures{0};
#define CHECK(v) do { if (!(v)) { std::printf("FAIL:%d %s\n", __LINE__, #v); std::fflush(stdout); std::_Exit(1); } } while (0)

struct Latch {
    std::mutex mu;
    std::condition_variable cv;
    bool set = false;
    void post() {
        {
            std::lock_guard l(mu);
            set = true;
        }
        cv.notify_all();
    }
    void wait() {
        std::unique_lock l(mu);
        CHECK(cv.wait_for(l, std::chrono::seconds(10), [&] { return set; }));
    }
};
Latch latch[128];   // (by letter. x: a worker is inside the texture's initialization, y: let it go on, z: segment 0 submitted)

const VkImage kTex = reinterpret_cast<VkImage>(uintptr_t(0x10)), kScratch = reinterpret_cast<VkImage>(uintptr_t(0x20)),
              kScratch2 = reinterpret_cast<VkImage>(uintptr_t(0x30)), kTex2 = reinterpret_cast<VkImage>(uintptr_t(0x40));
const char* image_name(VkImage i) {
    return i == kTex ? "t" : i == kScratch ? "s" : i == kScratch2 ? "s2" : i == kTex2 ? "t2" : "?";
}
bool hold_in_init;   // (close_race: the texture's initialization waits there, its lock held, for the other worker)
void record(VkCommandBuffer cb, char kind, VkImage image = VK_NULL_HANDLE) {
    CHECK(cb && cb->begun && !cb->ended && !cb->submitted);
    cb->ops.push_back({kind, image});
}
void vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* di) {
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount; i++) {
        const VkImageMemoryBarrier2& b = di->pImageMemoryBarriers[i];
        const bool first = b.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && b.newLayout == VK_IMAGE_LAYOUT_GENERAL;
        record(cb, first ? 'T' : 'B', b.image);
        if (first && b.image == kTex && hold_in_init) {
            latch['x'].post();
            latch['y'].wait();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));   // (the other worker closing its segment meanwhile)
        }
    }
}
void vkCmdClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*) {
    record(cb, 'C', i);
}
void vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout, const VkClearDepthStencilValue*, uint32_t,
                                 const VkImageSubresourceRange*) {
    record(cb, 'C', i);
}
VkResult vkResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags) {
    return VK_SUCCESS;
}
std::string fail_begin, fail_end;   // (this command buffer cannot be begun / ended)
VkResult vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo*) {
    CHECK(cb);
    if (cb->name == fail_begin) return VK_ERROR_OUT_OF_HOST_MEMORY;
    cb->ops.clear(), cb->begun = true, cb->ended = false;
    return VK_SUCCESS;
}
VkResult vkEndCommandBuffer(VkCommandBuffer cb) {
    CHECK(cb && cb->begun && !cb->ended);
    if (cb->name == fail_end) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    cb->ended = true;
    return VK_SUCCESS;
}
VkResult vkQueueSubmit2(VkQueue, uint32_t n, const VkSubmitInfo2* si, VkFence) {
    for (uint32_t s = 0; s < n; s++)
        for (uint32_t i = 0; i < si[s].commandBufferInfoCount; i++) {
            VkCommandBuffer cb = si[s].pCommandBufferInfos[i].commandBuffer;
            CHECK(cb && cb->ended && !cb->submitted);
            if (cb->name.rfind("cmd_", 0) == 0)
                CHECK(published_slots.count(std::stoi(cb->name.substr(4))) == 1);
            cb->submitted = true;
            queue.push_back(cb);
            if (cb->name == "cmd_0") latch['z'].post();
        }
    return VK_SUCCESS;
}
std::mutex cbs_mu;
VkCommandBuffer make_cb(const char* what, int k) {
    auto cb = std::make_unique<VkCommandBuffer_T>();
    cb->name = std::string(what) + "_" + std::to_string(k);
    auto* result = cb.get();
    std::lock_guard l(cbs_mu);
    all_cbs.push_back(result);
    owned_cbs.push_back(std::move(cb));
    return result;
}
// (the init pools' startup: each pool creation and allocation can be made to fail, its output left as garbage)
const uintptr_t kGarbage = 0xBAD0;
int alloc_calls, fail_at;
std::set<uintptr_t> live_pools;
uintptr_t next_pool = 0x1000;
VkResult vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* p) {
    if (++alloc_calls == fail_at) {
        *p = reinterpret_cast<VkCommandPool>(kGarbage);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    live_pools.insert(next_pool);
    *p = reinterpret_cast<VkCommandPool>(next_pool++);
    return VK_SUCCESS;
}
VkResult vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out) {
    CHECK(ai->commandBufferCount == 1 && live_pools.count(reinterpret_cast<uintptr_t>(ai->commandPool)));
    if (++alloc_calls == fail_at) {
        *out = reinterpret_cast<VkCommandBuffer>(kGarbage);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *out = make_cb("pool", alloc_calls);
    return VK_SUCCESS;
}
void vkDestroyCommandPool(VkDevice, VkCommandPool p, const VkAllocationCallbacks*) {
    CHECK(p && live_pools.erase(reinterpret_cast<uintptr_t>(p)));   // (only a live one, once)
}

namespace mkwvk {
std::mutex log_mu;
std::string log_text;
std::function<void(const std::string&)> on_log;   // (init_end_fail: looks at the state when the replay stops)
void logf(const char* f, ...) {
    char line[512];
    va_list a;
    va_start(a, f);
    std::vsnprintf(line, sizeof line, f, a);
    va_end(a);
    {
        std::lock_guard l(log_mu);
        log_text += std::string(line) + "\n";
    }
    if (on_log) on_log(line);
}
static const struct FixtureHost { void (*log_flush)(); } fixture_host{}, *host = &fixture_host;   // (replay_fatal's)
[[noreturn]] void end_process(int code) { std::_Exit(code); }   // (vk_replay.cpp: TerminateProcess on Windows)
#include "init_fatal.inc"
void full_barrier(VkCommandBuffer cmd, const char* = "") {
    record(cmd, 'F');
}
void memory_table_begin() {}
bool memory_table_flush() { return false; }
bool memory_rb_refresh(VkCommandBuffer) { return false; }
void memory_rb_recording_begun() {}
void memory_mirror_binds_flush() {}   // (the mirrors' pieces: none here, so no bind to wait for)
void checkpoint_mark(VkCommandBuffer, uint64_t, char) {}   // (RX_VK_CHECKPOINTS' markers: none here)
bool memory_mirror_bind_wait(VkSemaphoreSubmitInfo*) { return false; }
// ((Triforce, P4) the barrier rules' marks are each command buffer's own, cleared at its begin (gpu/vk/cmd_work.h work_begun,
// checked by tests/test_barrier_rules.py): texture_begin_recording has none of its own to reset)
bool is_bc(VkFormat) { return false; }
bool is_astc(VkFormat) { return false; }
#include "init_upload.inc"
#include "init_hooks.inc"

// ---- create()'s initialization of a new image ----
struct Texture {
    VkImage image;
};
std::vector<std::unique_ptr<Texture>> textures;
std::mutex par_lock;   // (texture_get's ParLock: an image is made once, by whichever worker gets there first)
Texture* create(VkImage image) {
    struct { VkImage image; VkFormat format; VkImageAspectFlags aspect; } x{image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT};
    const bool depth = false;
    auto tex = std::make_unique<Texture>(Texture{image});
#include "init_create.inc"
    textures.push_back(std::move(tex));
    return p;
}
void texture_use() {   // (texture_get, and texture_touch's upload again: a copy into it in this thread's uploads)
    std::lock_guard lk(par_lock);
    if (textures.empty()) create(kTex);
    record(upload_cmd(), 'U', kTex);
}

// ---- the BC3 scratch, and the second one (an upload at another size) ----
std::mutex bc_mu;
std::chrono::steady_clock::time_point bc_used_at;
uint64_t bc_last_value = 0;
std::atomic<bool> bc_have{false};
constexpr uint32_t kBcLayers = 8;
VkImage bc_scratch = VK_NULL_HANDLE, bc_scratch2 = VK_NULL_HANDLE;
bool bc_scratch_fresh = false, bc_scratch2_fresh = false;
bool bc_scratch_make() {
    bc_scratch = kScratch, bc_scratch_fresh = true, bc_have = true;
    return true;
}
bool bc_scratch2_make() {
    bc_scratch2 = kScratch2, bc_scratch2_fresh = true;
    return true;
}
#include "init_scratch.inc"
void scratch_use(bool resized) {   // (an ASTC/BC3 upload: decoded into the scratch (and blitted into the second), in this thread's uploads)
    std::lock_guard lk(par_lock);
    CHECK(bc_scratch_layout(upload_cmd(), resized));
    record(upload_cmd(), 'U', kScratch);
    if (resized) record(upload_cmd(), 'U', kScratch2);
}

// ---- (Triforce, 11-17) texture_touch's look at a texture the CPU wrote before this submission: its upload, or the twin ----
// The steps of vk_texture.cpp texture_touch that decide where the upload goes (tests/test_upload_same.py reads them there): the
// twin when a later segment of this submission uploaded it, nothing more once it was looked at in this submission, else the
// upload (W), into the earliest segment's init command buffer still recorded before this one (UploadRedirect, cut from the
// source with InitRedirect) unless upload_inner reads it on the GPU (upload_reads_live: gpu_route here), and upload_seg the
// segment it went into.
struct Look {
    bool uploaded = false;
    uint64_t upload_value = 0, upload_seg = 0;
    uint32_t checked_epoch = 0;
    bool gpu_written = false;
};
Look look;
std::atomic<int> twins{0};
bool hold_in_upload = false;   // (upload_close_race: the upload waits, the init lock held, for the other worker to close its segment)
bool nested_upload = false;    // (upload_nested: the upload makes an image and both BC3 scratches meanwhile)
bool hold_for_other = false;   // (upload_end_other: the upload waits, segment 0's lock held, for segment 1 to end its own)
bool gpu_route = false;        // (upload_gpu_route: upload_inner would read the texture on the GPU, upload_reads_live)
void texture_look() {
    std::lock_guard lk(par_lock);
    if (textures.empty()) create(kTex);   // (texture_get)
    if (look.uploaded && look.upload_value == rec_value && look.upload_seg > tl_seg) {   // (the twin, in this segment's uploads)
        record(upload_cmd(), 'W', kTex);
        look.upload_seg = tl_seg;
        twins++;
        return;
    }
    if (look.uploaded && look.checked_epoch == epoch) return;
    look.checked_epoch = epoch;
    UploadRedirect early(upload_earliest::wanted(upload_earliest_on(), look.gpu_written, false) && tl_seg_index >= 0 && !gpu_route);
    if (nested_upload) {   // (upload(): its image made again, and the BC3 encoder's scratches made, inside the redirect)
        create(kTex2);
        record(upload_cmd(), 'U', kTex2);
        CHECK(bc_scratch_layout(upload_cmd(), true));
        record(upload_cmd(), 'U', kScratch), record(upload_cmd(), 'U', kScratch2);
    }
    record(upload_cmd(), 'W', kTex);
    if (hold_in_upload) {
        latch['x'].post();
        latch['y'].wait();
        // (until the other worker's end of its segment waits for this one's lock, the upload still recorded)
        const auto t0 = std::chrono::steady_clock::now();
        while (!st_seg_end_waiting.load() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) std::this_thread::yield();
        if (!st_seg_end_waiting.load()) std::printf("  FAIL: segment 0's end did not wait for the upload's lock\n"), failures++;
    }
    if (hold_for_other) {
        latch['x'].post();
        const auto t0 = std::chrono::steady_clock::now();
        while (seg_inits[1].open.load() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) std::this_thread::yield();
        if (seg_inits[1].open.load())
            std::printf("  FAIL: segment 1's end waited for an upload recorded into segment 0's init command buffer\n"), failures++;
    }
    const uint64_t up_seg = early.order;
    early.done();
    look.upload_value = rec_value, look.upload_seg = up_seg, look.uploaded = true;
}
bool tex2_made = false;
void texture2_use() {   // (another new texture, made by whichever worker gets there first and copied into in its uploads)
    std::lock_guard lk(par_lock);
    if (!tex2_made) create(kTex2), tex2_made = true;
    record(upload_cmd(), 'U', kTex2);
}

// ---- replay_handle's segments ----
#include "init_recording.inc"
struct Segment {
    // t: the texture (made if new, uploaded, drawn with), s: the scratch, S: both scratches, -: nothing,
    // x / z: wait for latch x (a worker inside the texture's initialization) / z (segment 0 submitted), d: 100 ms more,
    // L: the initialization holds segment 0's init lock (seg_inits[0].mu) while it records; then latch y lets it go on,
    // W / w: post latch w / wait for it (one segment waits for another to have made the texture),
    // u: a draw that samples the texture the CPU wrote (texture_look, then R), r: 0 to 3 ms more, n: another new texture used
    std::string script;
    const uint8_t* end = nullptr;   // (kStop: to the command buffer's end, every segment's commands in order)
    // (vk_replay.cpp's Segment has these too, for a temporal upscaler's tail: none here, as with the upscalers off)
    const uint8_t* p = nullptr;
    bool tail = false;
    void* order = nullptr;
};
const uint8_t* const kStop = reinterpret_cast<const uint8_t*>("stop");
const std::vector<Segment>* cur_segs;
const std::thread::id replay_thread = std::this_thread::get_id();   // (main's: the replay thread's part of submit_segments)
std::mutex recorded_mu;
std::vector<std::pair<std::string, bool>> recorded;   // (each segment's script as it was recorded, and whether by the replay thread)
int64_t record_segment(VkCommandBuffer& cmd, const Segment& g) {   // (vk_replay.cpp's returns its recording's time)
    std::string script = g.script;
    if (g.end == kStop)
        for (size_t k = 1; k < cur_segs->size(); k++) script += (*cur_segs)[k].script;
    {
        std::lock_guard l(recorded_mu);
        recorded.push_back({script, std::this_thread::get_id() == replay_thread});
    }
    for (char a : script) {
        if (a == 't') texture_use(), record(cmd, 'U', kTex);
        if (a == 'u') texture_look(), record(cmd, 'R', kTex);
        if (a == 'n') texture2_use(), record(cmd, 'U', kTex2);
        if (a == 'r') std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 3000));
        if (a == 's' || a == 'S') scratch_use(a == 'S');
        if (a == 'x' || a == 'z' || a == 'w') latch[(int)a].wait();
        if (a == 'W') latch['w'].post();
        if (a == 'd') std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (a == 'L') {
#if HAVE_SEG_INIT
            if (seg_inits[0].mu.try_lock()) {   // (segment 0's own: what is recorded into its init command buffer)
                seg_inits[0].mu.unlock();
                std::printf("  FAIL: the init lock is free while another worker records a new image's initialization\n");
                failures++;
            }
#endif
            latch['y'].post();
        }
    }
    return 1;
}
void draw_begin_recording(int, VkCommandBuffer, uint64_t) {}
// (the segments' GPU timestamps: G, into both command buffers while they are open, as vk_replay.cpp's own record them)
void gpu_time_begin(Recording& r) { record(r.cmd, 'G'), record(r.upload, 'G'); }
void gpu_time_end(Recording& r, bool) { record(r.cmd, 'G'), record(r.upload, 'G'); }
#include "init_parstats.inc"
void draw_flush_recording(int slot) { CHECK(slot >= 0 && slot < 8); published_slots.insert(slot); }
void queue_flush_locked() {}
void checkpoints_report() {}
[[noreturn]] void device_lost() { std::_Exit(3); }   // (vk_replay.cpp: the report, then the process ends)
struct {
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::mutex present_mu;
} dev;
std::atomic<int64_t> par_lock_wait_ns{0}, par_locks{0};
std::atomic<bool> par_active{false};
std::vector<int> run_order;   // (the order the workers run in, one after another; empty: all at once)
std::vector<std::thread> workers;
bool jobs_started;
std::atomic<int> running{0};   // (workers still recording)
int helper_late_ms = 0;   // (at once: the workers begin this late, so that the replay thread takes the segments it reaches first)
namespace rec {
void segments_begin() {}   // (vk_recorder.h's: the recorder's ring run before the segments; no recorder here)
}
bool own_on = true;
bool par_own_on() { return own_on; }   // (vk_replay.cpp's: RX_VK_PARALLEL_OWN)
struct RunJobs {
    void notify_all();
};
struct {
    std::mutex mu;
    std::vector<std::function<void()>> jobs;
    int pending = 0;
    RunJobs cv;
    std::condition_variable done;
    std::atomic<int> queued{0};   // (vk_replay.cpp's: the jobs' count replay_handle stores for helper threads that spin; none here)
} par_pool;
void RunJobs::notify_all() {
    jobs_started = true;
    std::vector<std::function<void()>> jobs;
    {
        std::lock_guard lk(par_pool.mu);
        jobs.swap(par_pool.jobs);
    }
    if (run_order.empty()) {   // (at once, as par_worker_main runs them; the replay thread submits meanwhile)
        for (auto& job : jobs) {
            running++;
            workers.emplace_back([job] {
                if (helper_late_ms) std::this_thread::sleep_for(std::chrono::milliseconds(helper_late_ms));
                job();
                running--;
                {
                    std::lock_guard lk(par_pool.mu);
                    par_pool.pending--;
                }
                par_pool.done.notify_all();
            });
        }
        return;
    }
    CHECK(jobs.size() == run_order.size());
    for (int k : run_order) std::thread(jobs[k]).join();
    std::lock_guard lk(par_pool.mu);
    par_pool.pending = 0;
}
// ((Triforce) RX_VK_OCC_QUERY's part of a job's submit: off here (occ_live false), nothing sealed
bool occ_live = false;
VkSemaphore rec_timeline = VK_NULL_HANDLE;
uint64_t rec_seq = 0;
void occ_seal(Recording&, uint64_t) {}
Recording recordings[8];
int recording_count = 0;
uint64_t rec_cur_value = 1;
Recording& begin_segment_recording(uint64_t value) {
    const int k = recording_count++;
    published_slots.erase(k);
    Recording& r = recordings[k];
    r.cmd = make_cb("cmd", k), r.upload = make_cb("upload", k);
    [k](auto& x) {   // (the fixed code's own command buffer for its new images)
        if constexpr (requires { x.init; }) x.init = make_cb("init", k);
    }(r);
    vkBeginCommandBuffer(r.cmd, nullptr);   // (as begin_segment_recording does; the job begins the uploads)
    r.value = value, r.slot = k;   // (the submission's, as vk_replay.cpp's: its segments are one submission, one epoch)
    return r;
}
// (replay_handle's part after the segments: the replay thread's next recording, submitted later as [its uploads, it])
Recording post;
Recording* rec_cur = nullptr;
int rec_parts = 0;
Recording& begin_recording(uint64_t value) {
    post.cmd = make_cb("post", 0), post.upload = make_cb("postupload", 0);
    vkBeginCommandBuffer(post.cmd, nullptr);
    vkBeginCommandBuffer(post.upload, nullptr);
    post.value = value;
    texture_begin_recording(post.upload, value);
    draw_begin_recording(post.slot, post.cmd, value);
    return post;
}
void submit_post() {
    VkCommandBufferSubmitInfo cb[2]{{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
    uint32_t n = 0;
    if (texture_uploads_recorded()) cb[n++].commandBuffer = post.upload;
    cb[n++].commandBuffer = post.cmd;
    vkEndCommandBuffer(post.upload);
    vkEndCommandBuffer(post.cmd);
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = n, si.pCommandBufferInfos = cb;
    vkQueueSubmit2(VK_NULL_HANDLE, 1, &si, VK_NULL_HANDLE);
}
// ((Triforce) the early tail's rest after the segments, from the gameplay upscale's dispatch on (vk_replay.cpp tail_rest): none here)
void tail_rest(VkCommandBuffer&, uint64_t, const uint8_t*) {}
/// replay_handle from its segments on (the dry pass, the part before and the slots' order are the caller's)
void submit_segments(std::vector<Segment> seg, std::vector<int> order) {
    run_order = order;
    cur_segs = &seg;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    const uint8_t* const stop = kStop;
    const bool up = false;   // ((Triforce) replay_handle's: no gameplay upscale's dispatch found by the dry pass (par_up))
    const uint64_t cb = 0;   // (replay_handle's: the command buffer's replay, for tail_rest)
    uint64_t seq = 10;
    const uint64_t base = 1;
    const auto t_dry0 = std::chrono::steady_clock::now(), t_set0 = t_dry0;   // (replay_handle's: the command buffer's start, the setup's)
    [&]() -> const uint8_t* {
#include "init_jobs.inc"
        return nullptr;
    }();
    for (auto& w : workers) w.join();
}
#include "init_pools.inc"
}  // namespace mkwvk

std::string ops_text(const VkCommandBuffer_T* cb) {
    std::string s;
    for (const Op& o : cb->ops) s += std::string(s.empty() ? "" : " ") + o.kind + (o.image ? image_name(o.image) : "");
    return s;
}
std::string queue_text() {
    std::string order;
    for (auto* cb : queue) order += " " + cb->name + "[" + ops_text(cb) + "]";
    return order;
}
/// The submitted command buffers in queue order: each image has one UNDEFINED -> GENERAL change, before every use
void play(const char* name) {
    std::printf("%s: queue%s\n", name, queue_text().c_str());
    std::map<VkImage, int> made, early;
    for (auto* cb : queue)
        for (const Op& o : cb->ops) {
            if (!o.image) continue;
            if (o.kind == 'T') {
                if (made[o.image]++)
                    std::printf("  FAIL: image %s made UNDEFINED again in %s (what came before is discarded)\n", image_name(o.image),
                                cb->name.c_str()), failures++;
            } else if (!made[o.image] && !early[o.image]++) {
                std::printf("  FAIL: %s uses image %s (%c) while it is still UNDEFINED in the queue (VUID-vkCmdDraw-None-09600)\n",
                            cb->name.c_str(), image_name(o.image), o.kind), failures++;
            }
        }
    for (auto [image, n] : made)
        if (n != 1) std::printf("  FAIL: image %s changed from UNDEFINED %d times\n", image_name(image), n), failures++;
    for (auto* cb : all_cbs)
        for (const Op& o : cb->ops)
            if (o.kind != 'F' && o.kind != 'G' && !cb->submitted) {   // (a barrier or a timestamp left unsubmitted does nothing)
                std::printf("  FAIL: %s has commands (%c%s) and was never submitted\n", cb->name.c_str(), o.kind, image_name(o.image));
                failures++;
                break;
            }
}
VkCommandBuffer_T* named(const char* name) {
    for (auto* cb : all_cbs)
        if (cb->name == name) return cb;
    return nullptr;
}
/// Which command buffer holds image i's UNDEFINED -> GENERAL change ("": none)
std::string init_of(VkImage i) {
    for (auto* cb : all_cbs)
        for (const Op& o : cb->ops)
            if (o.kind == 'T' && o.image == i) return cb->name;
    return "";
}

// ---- (Triforce, 11-17) the uploads of a texture the segments sample (texture_look): what each draw sees ----
/// The queue for the texture, in order: every write to it (an upload W, a clear) after a barrier since its last use, every read
/// after a barrier since its last write, and each draw from queue entry `from` on (this submission's) that samples it (R) after
/// an upload made in this submission. Returns the uploads made in it
int play_content(const char* name, size_t from) {
    int uploads = 0;
    char last = 0;   // ('w': written last, 'r': read last)
    bool barrier = true, wrote_now = false;
    for (size_t q = 0; q < queue.size(); q++)
        for (const Op& o : queue[q]->ops) {
            if (o.kind == 'F' || (o.image == kTex && (o.kind == 'T' || o.kind == 'B'))) {
                barrier = true;
                continue;
            }
            if (o.image != kTex) continue;
            const bool write = o.kind == 'W' || o.kind == 'C';
            if (last && (write || last == 'w') && !barrier)
                std::printf("  FAIL: %s: %s's %c of the texture with no barrier since its last %s\n", name, queue[q]->name.c_str(), o.kind,
                            last == 'w' ? "write" : "read"), failures++;
            if (o.kind == 'R' && q >= from && !wrote_now)
                std::printf("  FAIL: %s: %s samples the texture before this submission's upload in the queue\n", name,
                            queue[q]->name.c_str()), failures++;
            if (o.kind == 'W' && q >= from) uploads++, wrote_now = true;
            last = write ? 'w' : 'r', barrier = false;
        }
    return uploads;
}
/// The command buffers this submission's uploads of the texture are in, in queue order
std::string uploads_in(size_t from) {
    std::string s;
    for (size_t q = from; q < queue.size(); q++)
        for (const Op& o : queue[q]->ops)
            if (o.kind == 'W' && o.image == kTex) s += (s.empty() ? "" : " ") + queue[q]->name;
    return s;
}
/// Another run from scratch (upload_random): the queue, the command buffers, the texture and the look, and a new submission
void reset_run() {
    using namespace mkwvk;
    queue.clear(), all_cbs.clear(), recording_count = 0;
    textures.clear();
    look = Look{};
    twins = 0;
    tex2_made = false;
    {
        std::lock_guard l(recorded_mu);
        recorded.clear();
    }
    workers.clear();
    rec_cur_value++;
}
/// One run of `scripts` (in `order`, or all at once when empty): the images' layouts, the texture's content, the uploads (one,
/// no twin, when RX_VK_UPLOAD_EARLIEST is on), and the command buffer the upload is in when `want_in` says (empty: not checked)
void upload_run(const char* name, const std::vector<std::string>& scripts, const std::vector<int>& order, const std::string& want_in,
                bool quiet = false) {
    using namespace mkwvk;
    std::vector<Segment> seg;
    for (const auto& s : scripts) seg.push_back({s});
    const size_t from = queue.size();
    submit_segments(seg, order);
    if (!quiet) play(name);
    else {
        const int f0 = failures;
        std::map<VkImage, int> made;
        for (auto* cb : queue)
            for (const Op& o : cb->ops)
                if (o.kind == 'T') made[o.image]++;
                else if (o.image && !made[o.image]) failures++;
        if (failures != f0) std::printf("  FAIL: %s: an image used while UNDEFINED in the queue%s\n", name, queue_text().c_str());
    }
    const int n = play_content(name, from);
    const bool on = upload_earliest_on() && !gpu_route;   // (a GPU-read upload: its segment's own, the twin as before)
    bool sampled = false;
    for (const auto& s : scripts) sampled |= s.find('u') != std::string::npos;
    if (!quiet) std::printf("%s: uploads in [%s], %d twins, upload_seg %llu\n", name, uploads_in(from).c_str(), twins.load(),
                            (unsigned long long)look.upload_seg);
    if (sampled && (on ? n != 1 || twins != 0 : n != 1 + twins.load()))
        std::printf("  FAIL: %s: %d uploads, %d twins (want %s)%s\n", name, n, twins.load(), on ? "one, no twin" : "one and the twins",
                    queue_text().c_str()), failures++;
    if (!want_in.empty() && uploads_in(from) != want_in)
        std::printf("  FAIL: %s: the uploads are in [%s], want [%s]%s\n", name, uploads_in(from).c_str(), want_in.c_str(),
                    queue_text().c_str()), failures++;
}

int main(int argc, char** argv) {
    using namespace mkwvk;
    const std::string c = argc > 1 ? argv[1] : "";
    if (c.rfind("upload_", 0) == 0)   // (a lock taken twice by one thread, or a latch never posted: a failure, not a hang)
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(40));
            std::printf("  FAIL: still running after 40 s (a deadlock)\n");
            std::fflush(stdout);
            std::_Exit(1);
        }).detach();
    if (c == "later_makes") {   // (segment 1's worker makes the texture, then segment 0's uploads it again and draws with it)
        submit_segments({{"t"}, {"t"}}, {1, 0});
        play(c.c_str());
    } else if (c == "three") {   // (segment 0 is done; 2 makes it while 1 still records, and 1 uses it)
        submit_segments({{"-"}, {"t"}, {"t"}}, {0, 2, 1});
        play(c.c_str());
    } else if (c == "scratch") {   // (the BC3 scratch: made by segment 1's worker, used by segment 0's too)
        submit_segments({{"s"}, {"s"}}, {1, 0});
        play(c.c_str());
    } else if (c == "scratch2") {   // (segment 1 makes the scratch, then the second one; segment 0 uses both; then both fresh at once)
        submit_segments({{"S"}, {"sS"}}, {1, 0});
        play(c.c_str());
        queue.clear(), all_cbs.clear(), recording_count = 0;
        bc_scratch = bc_scratch2 = VK_NULL_HANDLE, bc_have = false;
        submit_segments({{"S"}, {"S"}}, {1, 0});
        play("scratch2 (both fresh)");
    } else if (c == "in_order") {   // (made by the first segment, as serial replay would)
        submit_segments({{"t"}, {"t"}}, {0, 1});
        play(c.c_str());
    } else if (c == "serial") {   // (no segments: the replay thread's uploads get exactly the calls they always did)
        VkCommandBuffer up = make_cb("upload", 0);
        vkBeginCommandBuffer(up, nullptr);
        texture_begin_recording(up, 1);
        texture_use();
        scratch_use(false);
        scratch_use(true);
        CHECK(texture_uploads_recorded());
        vkEndCommandBuffer(up);
        const std::string got = ops_text(up), want = "F Tt Ct Bt Ut Ts Us Ts2 Us Us2 F";
        std::printf("serial: %s[%s]\n", up->name.c_str(), got.c_str());
        if (got != want) std::printf("  FAIL: serial uploads changed, want [%s]\n", want.c_str()), failures++;
        for (auto* cb : all_cbs)
            if (cb != up && !cb->ops.empty()) std::printf("  FAIL: serial replay recorded into %s\n", cb->name.c_str()), failures++;
    } else if (c == "close_race") {   // (at once: segment 1 records the texture's initialization into segment 0's init command
                                      // buffer, holding the lock, while segment 0's worker finishes and closes it)
        hold_in_init = true;
        submit_segments({{"xL"}, {"t"}}, {});
        play(c.c_str());
        if (HAVE_SEG_INIT && init_of(kTex) != "init_0")
            std::printf("  FAIL: the texture's initialization is in %s, not init_0\n", init_of(kTex).c_str()), failures++;
    } else if (c == "after_submit") {   // (at once: segment 1 makes the texture only after segment 0 was submitted)
        submit_segments({{"-"}, {"zt"}}, {});
        play(c.c_str());
        if (named("init_0") && !named("init_0")->ops.empty())
            std::printf("  FAIL: init_0 (submitted) was recorded into\n"), failures++;
    } else if (c == "init_begin_fail") {   // (segment 1's init command buffer cannot be begun: no worker starts, and the
                                           // whole command buffer is recorded in order on the replay thread, after the segments)
#if HAVE_SEG_INIT
        fail_begin = "init_1";
        submit_segments({{"-"}, {"t"}, {"t"}}, {0, 2, 1});
        if (jobs_started) std::printf("  FAIL: a segment's worker started\n"), failures++;
        submit_post();
        play(c.c_str());
        for (auto* cb : queue)
            if (cb->name.rfind("post", 0) != 0) std::printf("  FAIL: segment command buffer %s submitted\n", cb->name.c_str()), failures++;
        if (init_of(kTex) != "postupload_0")
            std::printf("  FAIL: the texture's initialization is in %s, not postupload_0\n", init_of(kTex).c_str()), failures++;
        if (!named("post_0") || ops_text(named("post_0")) != "F Ut Ut") std::printf("  FAIL: not every segment's draw was recorded\n"), failures++;
#else
        std::printf("init_begin_fail: no init command buffers in this code\n");
#endif
    } else if (c == "init_end_fail") {   // (at once: segments 1 and 2 wait for segment 0's submit, then make and use the texture,
                                         // whose initialization goes to init_1; init_1 cannot be ended. The replay must stop
                                         // for good (replay_fatal ends the process, exit code 3) after segment 2's worker is done,
                                         // with nothing that uses the texture submitted; later work using it must never run)
#if HAVE_SEG_INIT
        fail_end = "init_1";
        on_log = [](const std::string& line) {
            if (line.find("FATAL") == std::string::npos) return;
            std::printf("init_end_fail: stopped: queue%s\n", queue_text().c_str());
            if (running) std::printf("  FAIL: the replay stopped with a worker still recording\n");
            CHECK(published_slots == std::set<int>{0});   // lost init and later slots were not published
            for (auto* cb : queue)
                for (const Op& o : cb->ops)
                    if (o.image == kTex) std::printf("  FAIL: %s, submitted, uses the texture whose initialization was lost\n", cb->name.c_str());
            if (init_of(kTex) != "init_1") std::printf("  FAIL: the texture's initialization is in %s, not init_1\n", init_of(kTex).c_str());
            std::fflush(stdout);
        };
        submit_segments({{"-"}, {"zt"}, {"ztd"}}, {});
        // (the replay went on: the next recording (and frame) uses the texture again)
        Recording& later = begin_recording(rec_cur_value);
        texture_use();
        record(later.cmd, 'U', kTex);
        submit_post();
        std::printf("%s: went on: queue%s\n", c.c_str(), queue_text().c_str());
        std::printf("  FAIL: the replay went on after a new image's initialization was lost\n"), failures++;
#else
        std::printf("init_end_fail: no init command buffers in this code\n");
#endif
    } else if (c == "own_uses" || c == "own_all" || c == "own_off") {
        // (Triforce: the replay thread records the next segment to submit when no worker has begun it. own_uses: the workers late,
        // it records segment 0, which waits there until segment 1's worker has made the texture (into segment 0's init command
        // buffer, still open) and then uses it. own_all: the workers later still, it records every segment, each the next to
        // submit. own_off: RX_VK_PARALLEL_OWN=0, the workers alone however late)
        const bool uses = c == "own_uses", all = c == "own_all";
        own_on = c != "own_off";
        helper_late_ms = all ? 300 : 50;
        if (uses) submit_segments({{"wt"}, {"tW"}}, {});
        else if (all) submit_segments({{"t"}, {"s"}, {"t"}}, {});
        else submit_segments({{"t"}, {"t"}}, {});
        play(c.c_str());
        const int64_t want = uses ? 1 : all ? 3 : 0;
        int by_replay = 0;
        for (const auto& [script, own] : recorded) by_replay += own;
        std::printf("%s: %lld segments recorded by the replay thread (ParStats), %d seen\n", c.c_str(), (long long)par_st.own.load(), by_replay);
        if (par_st.own.load() != want || by_replay != want) std::printf("  FAIL: want %lld by the replay thread\n", (long long)want), failures++;
        if (uses && !(recorded.size() == 2 && recorded[0] == std::pair<std::string, bool>{"wt", true} && !recorded[1].second))
            std::printf("  FAIL: segment 0 was not the replay thread's, or segment 1 not a worker's\n"), failures++;
        if (uses && init_of(kTex) != "init_0")
            std::printf("  FAIL: the texture's initialization is in %s, not init_0\n", init_of(kTex).c_str()), failures++;
        if (queue.size() < (size_t)(uses ? 4 : all ? 6 : 4)) std::printf("  FAIL: not every segment was submitted\n"), failures++;
    } else if (c == "alloc_fail") {   // (init_pools_make: each of its pool creations and allocations failing in turn)
#if HAVE_INIT_POOLS
        const int n = (int)(sizeof(recordings) / sizeof(recordings[0]));
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        for (int f = 1; f <= 2 * n + 1; f++) {   // (the last: none fails)
            alloc_calls = 0, fail_at = f <= 2 * n ? f : 0;
            const bool ok = init_pools_make(pi);
            if (ok != !fail_at) std::printf("  FAIL: step %d failing, init_pools_make returned %d\n", f, ok), failures++;
            for (const Recording& r : recordings)
                if (ok ? (!r.init_pool || !r.init) : (r.init_pool || r.init))
                    std::printf("  FAIL: step %d failing, a recording's init pool/buffer is %p/%p\n", f, (void*)r.init_pool, (void*)r.init),
                        failures++;
            if (!ok && !live_pools.empty())
                std::printf("  FAIL: step %d failing, %zu init pools left made\n", f, live_pools.size()), failures++;
        }
        if (live_pools.size() != (size_t)n) std::printf("  FAIL: %zu init pools made, want %d\n", live_pools.size(), n), failures++;
        init_pools_destroy();
        if (!live_pools.empty()) std::printf("  FAIL: init pools left after init_pools_destroy\n"), failures++;
        std::printf("alloc_fail: each of the %d steps failing gives back every pool made and leaves no handle; none failing makes %d\n",
                    2 * n, n);
#else
        std::printf("alloc_fail: no init pools in this code\n");
#endif
    } else if (c == "upload_rules") {   // ((Triforce, 11-17) upload_earliest.h's rules)
        CHECK(upload_earliest::wanted(true, false, false) && !upload_earliest::wanted(false, false, false) &&
              !upload_earliest::wanted(true, true, false) && !upload_earliest::wanted(true, false, true));
        const bool open[5] = {false, true, false, true, true};
        auto is_open = [&](int k) { return open[k]; };
        CHECK(upload_earliest::target(is_open, 5, 4) == 1 && upload_earliest::target(is_open, 5, 3) == 1);
        CHECK(upload_earliest::target(is_open, 5, 1) == -1);   // (itself the earliest open: its own uploads)
        CHECK(upload_earliest::target(is_open, 5, 0) == -1 && upload_earliest::target(is_open, 2, 4) == 1);
        const bool none[3] = {false, false, false};
        CHECK(upload_earliest::target([&](int k) { return none[k]; }, 3, 2) == -1);
        CHECK(upload_earliest::order_of(17, 4, 1) == 14 && upload_earliest::order_of(17, 4, 4) == 17);
        std::printf("upload_rules: wanted, target and order_of as upload_earliest.h says\n");
    } else if (c == "upload_later_first") {   // (segment 1's worker uploads the texture first, then segment 0 samples it: one upload,
                                              // in segment 0's init command buffer, before segment 0's draw; no twin)
        upload_run(c.c_str(), {"u", "u"}, {1, 0}, upload_earliest_on() ? "init_0" : "upload_0 upload_1");
        if (upload_earliest_on() && look.upload_seg != 1)
            std::printf("  FAIL: upload_seg %llu, want segment 0's order 1\n", (unsigned long long)look.upload_seg), failures++;
    } else if (c == "upload_three") {   // (segment 0 is done (its init ended), 2 uploads while 1 still records: into init_1)
        upload_run(c.c_str(), {"-", "u", "u"}, {0, 2, 1}, "init_1");
        if (look.upload_seg != 2) std::printf("  FAIL: upload_seg %llu, want segment 1's order 2\n", (unsigned long long)look.upload_seg), failures++;
    } else if (c == "upload_own") {   // (the earliest segment still recorded uploads in its own uploads, as before)
        upload_run(c.c_str(), {"u", "u"}, {0, 1}, "upload_0");
        if (named("init_0") && !named("init_0")->ops.empty() && ops_text(named("init_0")) != "F Tt Ct Bt F")
            std::printf("  FAIL: init_0 holds [%s]\n", ops_text(named("init_0")).c_str()), failures++;
        reset_run();
        upload_run("upload_own (after a done segment)", {"-", "u"}, {0, 1}, "upload_1");
    } else if (c == "upload_close_race") {   // (at once: segment 1 records the upload into segment 0's init command buffer, the init
                                             // lock held, while segment 0's worker finishes and closes it)
        hold_in_upload = true;
        upload_run(c.c_str(), {"xL", "u"}, {}, "init_0");
        const std::string init0 = named("init_0") ? ops_text(named("init_0")) : "";
        if (init0.size() < 4 || init0.compare(init0.size() - 4, 4, "Wt F") != 0)
            std::printf("  FAIL: init_0 is [%s]: not ended after the upload with its barrier\n", init0.c_str()), failures++;
        std::printf("%s: segment ends that waited for the init lock: %lld\n", c.c_str(), (long long)st_seg_end_waits.load());
        if (st_seg_end_waits.load() < 1) std::printf("  FAIL: segment 0's end did not wait for the upload\n"), failures++;
    } else if (c == "upload_end_other") {   // (at once: segment 2 records the upload into segment 0's init command buffer, its lock
                                            // held, and segment 1 ends meanwhile: its end does not wait, only segment 0's may)
        hold_for_other = true;
        upload_run(c.c_str(), {"x", "x", "u"}, {}, "init_0");
    } else if (c == "upload_nested") {   // (the upload makes an image and both BC3 scratches inside the redirect: their first layout
                                         // changes there too, before the copies, and no deadlock on the init lock)
        nested_upload = true;
        upload_run(c.c_str(), {"u", "u"}, {1, 0}, "init_0");
        for (VkImage i : {kTex2, kScratch, kScratch2})
            if (init_of(i) != "init_0")
                std::printf("  FAIL: image %s's first layout change is in %s, not init_0\n", image_name(i), init_of(i).c_str()), failures++;
    } else if (c == "upload_own_nested") {   // (the earliest segment still recorded uploads in its own uploads, and makes an image and
                                             // both BC3 scratches meanwhile: their first layout changes in its init, no lock held twice)
        nested_upload = true;
        upload_run(c.c_str(), {"u", "u"}, {0, 1}, "upload_0");
        for (VkImage i : {kTex2, kScratch, kScratch2})
            if (init_of(i) != "init_0")
                std::printf("  FAIL: image %s's first layout change is in %s, not init_0\n", image_name(i), init_of(i).c_str()), failures++;
    } else if (c == "upload_gpu_route") {   // (a texture upload_inner reads on the GPU (the GPU unswizzle, the all-zero clear) stays in
                                            // the uploads of the segment that looked first, the twin in segment 0's as before; one
                                            // staged from the CPU goes into init_0)
        gpu_route = true;
        upload_run(c.c_str(), {"u", "u"}, {1, 0}, "upload_0 upload_1");
        if (twins != 1) std::printf("  FAIL: the GPU-read upload was not left in segment 1's uploads, with the twin\n"), failures++;
        gpu_route = false;
        reset_run();
        upload_run("upload_gpu_route (staged from the CPU)", {"u", "u"}, {1, 0}, upload_earliest_on() ? "init_0" : "upload_0 upload_1");
    } else if (c == "upload_then_make") {   // (segment 2's worker uploads the texture into init_0, then makes another texture that
                                            // segment 0 uses too: its first layout change goes to init_0 as well, the redirect over)
        upload_run(c.c_str(), {"n", "-", "un"}, {2, 1, 0}, "init_0");
        if (init_of(kTex2) != "init_0")
            std::printf("  FAIL: the second texture's first layout change is in %s, not init_0\n", init_of(kTex2).c_str()), failures++;
        // (the redirect gave the worker its own upload command buffer back, unused: segment 2's copy into the second texture
        // after the upload is there, after its leading full barrier)
        if (!named("upload_2") || ops_text(named("upload_2")) != "G F Ut2 F G")
            std::printf("  FAIL: upload_2 is [%s], want [G F Ut2 F G]\n", named("upload_2") ? ops_text(named("upload_2")).c_str() : "-"),
                failures++;
    } else if (c == "upload_barrier") {   // (a submission before reads the texture; this one's upload is init_0's first command:
                                          // a full barrier before it)
        VkCommandBuffer up = make_cb("prevupload", 0), prev = make_cb("prev", 0);
        vkBeginCommandBuffer(up, nullptr), vkBeginCommandBuffer(prev, nullptr);
        texture_begin_recording(up, rec_cur_value);
        texture_look(), record(prev, 'R', kTex);
        CHECK(texture_uploads_recorded());
        vkEndCommandBuffer(up), vkEndCommandBuffer(prev);
        VkCommandBufferSubmitInfo cb[2]{{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
        cb[0].commandBuffer = up, cb[1].commandBuffer = prev;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 2, si.pCommandBufferInfos = cb;
        vkQueueSubmit2(VK_NULL_HANDLE, 1, &si, VK_NULL_HANDLE);
        rec_cur_value++;   // (the next submission)
        upload_run(c.c_str(), {"u", "u"}, {1, 0}, "init_0");
        if (ops_text(named("init_0")) != "F Wt F") std::printf("  FAIL: init_0 is [%s], want [F Wt F]\n", ops_text(named("init_0")).c_str()), failures++;
    } else if (c == "upload_off") {   // (RX_VK_UPLOAD_EARLIEST=0: each segment's uploads its own, the twin in segment 0's: as before)
        if (upload_earliest_on()) std::printf("  FAIL: run it with RX_VK_UPLOAD_EARLIEST=0\n"), failures++;
        upload_run(c.c_str(), {"u", "u"}, {1, 0}, "upload_0 upload_1");
        for (auto [name, want] : {std::pair<const char*, const char*>{"init_0", "F Tt Ct Bt F"}, {"upload_0", "G F Wt F G"},
                                  {"upload_1", "G F Wt F G"}, {"cmd_0", "G Rt G"}, {"cmd_1", "G Rt G"}})
            if (!named(name) || ops_text(named(name)) != want)
                std::printf("  FAIL: %s is [%s], want [%s] as before\n", name, named(name) ? ops_text(named(name)).c_str() : "-", want), failures++;
        if (twins != 1 || look.upload_seg != 1) std::printf("  FAIL: want one twin, by segment 0\n"), failures++;
    } else if (c == "upload_random") {   // (every order of 4 segments' workers and every set of them sampling the texture, one after
                                         // another; then runs at once with the workers' timing mixed)
        const bool on = upload_earliest_on();
        int runs = 0;
        for (int mask = 1; mask < 16; mask++) {
            std::vector<int> ord = {0, 1, 2, 3};
            do {
                reset_run();
                std::vector<std::string> scripts;
                for (int k = 0; k < 4; k++) scripts.push_back(mask >> k & 1 ? "u" : "-");
                size_t f = 0;   // (the first to sample it, in the run order)
                while (!(mask >> ord[f] & 1)) f++;
                int target = -1;   // (the earliest segment not run yet, before it)
                for (int k = 0; k < ord[f] && target < 0; k++)
                    if (std::find(ord.begin(), ord.begin() + (ptrdiff_t)f, k) == ord.begin() + (ptrdiff_t)f) target = k;
                const std::string want = !on ? "" : target >= 0 ? "init_" + std::to_string(target) : "upload_" + std::to_string(ord[f]);
                char name[64];
                std::snprintf(name, sizeof name, "upload_random mask %d order %d%d%d%d", mask, ord[0], ord[1], ord[2], ord[3]);
                const int f0 = failures;
                upload_run(name, scripts, ord, want, true);
                if (on && look.upload_seg != 1 + (uint64_t)(target >= 0 ? target : ord[f]))
                    std::printf("  FAIL: %s: upload_seg %llu\n", name, (unsigned long long)look.upload_seg), failures++;
                if (failures != f0) break;
                runs++;
            } while (std::next_permutation(ord.begin(), ord.end()));
        }
        std::srand(7);
        const char* pick[] = {"-", "u", "ru", "ur", "r", "rru"};
        for (int i = 0; i < 60 && !failures; i++, runs++) {
            reset_run();
            std::vector<std::string> scripts;
            for (int k = 0; k < 4; k++) scripts.push_back(pick[std::rand() % 6]);
            upload_run("upload_random (at once)", scripts, {}, "", true);
        }
        std::printf("%s: %d runs, each draw sampling this submission's upload; %s\n", c.c_str(), runs,
                    on ? "one upload a run, no twin" : "the twins as before (RX_VK_UPLOAD_EARLIEST=0)");
    } else {
        std::printf("unknown case: %s\n", c.c_str());
        return 2;
    }
    std::fflush(stdout);
    return failures ? 1 : 0;
}
