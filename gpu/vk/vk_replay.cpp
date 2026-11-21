// The NVN replay on Vulkan: the game's recorded command buffers (host/nvn_native.c, records in rx_nvn.h) at each
// submit, recorded into a Vulkan command buffer and submitted with the next value of a timeline semaphore (the NVN
// fences); and the window presents.
//
// Each recording has two command buffers: the replay's, and one submitted right before it that texture uploads and
// new images' layout changes go to (vk_texture.cpp), so no pass has to end for them. The stream's memory writes
// happen on the GPU timeline: guest memory is shared with the GPU, so a CPU write at replay time would reach draws
// recorded before it. Uniform updates (UPDATE_UB) are written at the end of their recording, and the draws after
// one bind a copy of the range with it in (ub_overlay), so a pass need not end for them either.
//
// Without a window (HOS_HEADLESS) presents render into an image of their own; RX_VK_SHOTS=<dir> writes every
// RX_VK_SHOT_EVERY-th one (default 300) there as a PNG.
#include <algorithm>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <exception>
#include <thread>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>   // (RX_VK_SEG_EARLY: ub_grow_mu)

#include "vk_dlssg.h"
#include "vk_temporal.h"
#include "vk_fsr3_inputs.h"   // (the camera jitter while AMD FSR 3.1 upscales)
#include "status_line.h"
#include "stall_watch.h"   // (Triforce: the frame times and the stall lines, status_tick)
#include "lab_ab.h"        // (Triforce, lab: in-run A/B phases and delay sites, RX_VK_LAB_AB)
#include "occ_answer.h"    // (Triforce: samples-passed reports from occlusion queries, RX_VK_OCC_QUERY)
#include <cstdarg>
#include "present_hold.h"  // (Triforce: the hold of the last picture, do_present)
#include "present_smooth.h"   // (Triforce: Frame pacing on the plain present, RX_VK_PACE_SMOOTH: present_image)
#include "present_thread.h"   // (Triforce: the present thread, RX_VK_PRESENT_THREAD: present_image)
#include "vk_reflex.h"
#include "vk_antilag.h"
#include "vk_draw.h"
#include "vk_dump.h"
#include "vk_paths.h"   // (game_module_path: the frame trace beside the game)
#include "file_writers.h"   // ((Triforce, handoff §8) the writes of threads of their own, and the process's end)
#include "vk_gprof.h"
#include "vk_framegen.h"
#include "vk_ngxfg.h"   // (DLSS Frame Generation: idle)
#include "vk_hudless.h"   // (the picture without the HUD kept for the made-frame engines: idle)
#include "vk_ffx_fg.h"   // (AMD FSR 3.1 frame generation through AMD's library: its swapchain)
#include "vk_menu.h"
#include "vk_jobs.h"
#include "helper_spin.h"   // (Triforce: the helper threads' spin for work and the replay thread's for a segment, RX_VK_HELPER_SPIN_US)
#include "vk_shader.h"
#include "vk_state.h"
#include "vk_texture.h"
#include "vk_xfer.h"
#include "vk_present_rect.h"
#include "vk_present_trace.h"
#include "fps_cap.h"   // (the Game frame rate cap's Monitor's rate, for the host)
#include "vk_pace.h"   // (Triforce: who holds a frame to that cap, for the host)
#include "replay_threads.h"   // (the Replay helper threads row's Automatic)
#include "stream_order.h"     // (Triforce: the temporal hooks in stream order with the helper threads)
#include "barrier_rules.h"    // (Triforce: a part's split inside a pass, an event's write held, the held submits)
#include "tail_early.h"       // (Triforce: the early tail's rules, Start next frame early)
#include "pass_barrier.h"     // (Triforce: the pass barrier's game mode, which RX_VK_EVENT_BARRIER follows when unset)
#include "replay_prio.h"      // (Triforce: the replay's priority while it records, RX_VK_REPLAY_PRIO: ReplayPin)
#include "mesh_ahead.h"       // (Triforce: a run of indirect mesh draws, RX_VK_MESH_AHEAD: mesh_ahead_note)
#include "gate_wait.h"        // (Triforce: a submission that begins with a wait for the CPU's signal, RX_VK_GATE_WAIT_MS: submit_q)
#include "async_rules.h"      // (Triforce: the replay thread's rules, RX_VK_ASYNC=1: the capture at a signal, the present's drain)
#include "wb_rec_wait.h"      // (Triforce, wp20: a wait for a write-back, by its writer's recording: vk_timeline_wait)
#include "frame_trace.h"      // (Triforce, wp20: the frame trace's split of each frame, status_tick)
#include "segment_plan.h"     // (Triforce: the early segments' count and boundaries, RX_VK_SEG_EARLY)

#include "vk_internal.h"
#include "vk_renodx.h"
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
extern std::atomic<bool> movie_networks_slow;   // (vk_draw.cpp: the movie networks left out, issue 109)
int64_t presents_now;   // (the presents so far, for SlowWatch)
void recorder_auto(bool on) { rec::g_auto_thread = on; }
bool vk_ft_on() {
    static const int64_t from = getenv("RX_VK_FT") ? atoll(getenv("RX_VK_FT")) : -1;
    static const int64_t n = getenv("RX_VK_FT_N") ? atoll(getenv("RX_VK_FT_N")) : 8;
    return from >= 0 && presents_now >= from && presents_now < from + n;
}
void vk_ft(const char* fmt, ...) {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    const double ms = (double)(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() %
                               100000000000ll) / 1e6;
    logf("vk: ft %.3f %s", ms, line);
}
void hang_watch_start();   // (EvilOOT, vk_hangdump.cpp: RX_VK_HANG_DUMP)
int display_refresh_mhz();   // (vk_main.cpp: the window's display, mHz; 0 when not known)
int memory_demote_trial(uint64_t* lo);   // (Triforce, vk_memory.cpp: a pool's trial in guest memory under way, its phase; for the stall line)
void memory_import_stats(int64_t* imports, int64_t* ns);   // (Triforce, vk_memory.cpp: the pools imported where bound; the stall line)
void program_cache_stats(int64_t* draw_reads, int64_t* draw_read_ns, int64_t* ahead_taken);   // (Triforce, vk_shader.cpp: the stall line)
int64_t texture_uploads_later();   // (Triforce, vk_texture.cpp: uploads left for the next frame; the stall line)
void texture_reupload_early();     // (Triforce, vk_texture.cpp: the textures the CPU keeps rewriting, before the segments)
int64_t texture_split_parts();     // (Triforce, vk_texture.cpp: parts of layered textures' first uploads; the stall line)
int64_t draw_view_lookups();       // (Triforce, vk_draw.cpp: texture views resolved anew; the stall line)
void program_pick_stats(int64_t* lock_ns, int64_t* codes_ns, int64_t* inline_n, int64_t* inline_ns);   // (Triforce, vk_shader.cpp)
void dispatch_split_stats(int64_t* pick_ns, int64_t* pipe_ns, int64_t* ready, int64_t* made);   // ((Triforce, test19) vk_draw.cpp)
void pipeline_ahead_stats(int64_t* first, int64_t* ahead, int64_t* by_order, int64_t* links, int64_t* libs);   // ((Triforce, wp18) vk_draw.cpp)
void readahead_wait_stats(int64_t* waits, int64_t* wait_ns);   // ((Triforce, test19) vk_shader.cpp)
void program_read_stats(int64_t* waits, int64_t* wait_ns, int64_t* relock_ns);   // ((Triforce) vk_shader.cpp, program_read_once.h)
void program_order_stats(int64_t* unnamed, int64_t* let_go, int64_t* ahead, int64_t* order_taken);   // ((Triforce) vk_shader.cpp, program_order.h)
void texture_create_stats(int64_t* made, int64_t* made_ns, int64_t* later);   // (Triforce, vk_texture.cpp: the stall line)
void vma_image_stats(int64_t* images, int64_t* images_ns, int64_t* targets, int64_t* targets_ns);   // (Triforce, vk_vma.cpp)
int memory_bg_importing();   // (Triforce, vk_memory.cpp: imports the background thread is making now; the stall line)
void memory_import_churn(int64_t* given_back, int64_t* ahead_sent);   // (Triforce, vk_memory.cpp: the stall line)
void memory_bind_counts(uint64_t* written, uint64_t* guest);   // (Triforce, vk_memory.cpp: the early tail's counts, tail_seen)
bool menu_row_at_start(const char* env_name, const char* ini_key_name, bool def, int since);   // (vk_draw.cpp: a row at the start)
bool mesh_ahead_enabled();   // (Triforce, vk_draw.cpp: RX_VK_MESH_AHEAD, mesh_ahead.h)
struct CondNote { int64_t present; uint64_t addr, x, y; int mode; bool on; };   // (the last render conditions, for a dark frame)
CondNote cond_ring[1 << 15];
uint32_t cond_ring_at = 0;
namespace {

// ---- submissions: a timeline semaphore; value n = the n-th submit done ----
VkSemaphore timeline = VK_NULL_HANDLE;
// ---- (hookshot) recordings by number: a second timeline, signalled at the end of every recording with its number, and the
// recording each submission was finished in. The timeline above is signalled only as far as no older submission is stopped
// at a wait, and every NVN queue's submissions share its numbers: a fence taken on one queue waited for the other queues'
// stopped work too, and a wait for it forced that work past the waits it stopped at (OoT's presentation work run ahead of
// the scene work it waits for: a frame of the fog, haze and sun shafts of the frame before). NVN's fences are per queue: a
// wait for one is a wait for that queue's work, here for the recordings it was finished in. RX_VK_PER_QUEUE_FENCES=0: as
// before ----
VkSemaphore rec_timeline = VK_NULL_HANDLE;
uint64_t rec_seq = 0;                       // (the recordings submitted so far; replay_mu)
std::vector<uint64_t> rec_open_values;      // (the submissions finished in the recording being made; replay_mu)
std::mutex rec_map_mu;
std::map<uint64_t, uint64_t> value_rec;     // submission value (or a recording's own) -> the recording that finished it
// ((Triforce, wp20) the submissions finished in a recording alone, by the recording they were finished in: value_rec keeps a
// recording's own value at its first part's submit, which a wait for a write-back may not stop at. Noted at the submit that ends
// the recording, not a part's (submit_recording's part). rec_map_mu; wb_rec_wait.h)
::mkwvk::wb_rec_wait::Finished fin_rec;
std::unordered_map<uint64_t, uint64_t> q_last;   // queue -> its last submission's value (sync_order_mu)
std::map<uint64_t, uint64_t> value_queue;   // a submission's value -> its queue (sync_order_mu)
std::atomic<uint64_t> pushed{0};
std::atomic<uint64_t> q_values{0};   // the fence values handed out (a submit or a present takes one each)
thread_local bool tl_submit_held = false;   // (HOS_NVN_HOLD=2: this thread replays a held submit now: mkwvk_submit_held)
// RX_VK_FENCE_POLL_FAST (default on with the ticket; =0 and RX_VK_READBACK_TICKET=0: off): the submissions finished whole (not
// stopped at a wait) on a queue, by value (slot = value & 16383 holds the value), marked once value_rec has their recording: a
// fence poll (a wait of timeout 0) on one of them answers from the recordings' timeline without sync_order_mu, which submit_q
// and present hold for whole replays (the main thread's two nvnSyncWait looks a frame waited ~1.4 ms a frame for it behind the
// Presentation Thread's replay). See wait and invalidate
std::vector<uint64_t> rec_open_qvalues;     // (those of rec_open_values submitted on a queue: value_queue has them; replay_mu)
constexpr uint64_t kWholeSlots = 16384;
std::atomic<uint64_t> whole_val[kWholeSlots];
/// RX_VK_READBACK_TICKET (default 1; 0: off): the fast poll above keeps the readbacks' order it skipped by a ticket: the poll
/// notes the sync_order_mu section in progress, and a CPU readback (invalidate) waits until that section has left, the order the
/// poll's lock gave it. Never the fast poll without it: the CPU's exposure readback then read the write-back state of before the
/// replay in progress, and the scene blew out to white
int readback_ticket() {
    static const int v = getenv("RX_VK_READBACK_TICKET") ? atoi(getenv("RX_VK_READBACK_TICKET")) : 1;
    return v;
}
bool fence_poll_fast_on() {
    static const bool v = readback_ticket() == 1 && !(getenv("RX_VK_FENCE_POLL_FAST") && atoi(getenv("RX_VK_FENCE_POLL_FAST")) == 0);
    return v;
}
::mkwvk::rec::DrainingMutex replay_mu;   // one replay at a time (the game submits from several threads; RX_VK_RECORDER=defer: what
                                         // a thread recorded is drained before another may take it)
/// ((Triforce) RX_VK_REPLAY_PRIO, replay_prio.h: the level a thread is raised to while it replays; 0: none)
int replay_prio_level() {
    static const int v = [] {
        const int l = replay_prio::level(getenv("RX_VK_REPLAY_PRIO"));
        logf("vk: a thread that replays runs at %s priority while it replays (RX_VK_REPLAY_PRIO=0: its own, 1: above normal)",
             l >= replay_prio::kHighest ? "the highest" : l ? "above normal" : "its own");
        return l;
    }();
    return v;
}
// (RX_VK_RECORDER=thread: the recorder thread named, and above normal priority as par_worker_main's threads; (Triforce) at the replay's
// level, RX_VK_REPLAY_PRIO, when that is higher: it makes the calls of a thread raised while it replays)
// RX_VK_RECORDER_PIN (experiment): next = the physical core below the reserved one (the replay thread's: hos_reserved_cpus),
// sibling = the reserved core itself (shares it with the replay thread), or a hex mask; unset: not pinned
const bool rec_thread_hook = (::mkwvk::rec::g_thread_start_hook = [] {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk recorder");
    SetThreadPriority(GetCurrentThread(), replay_prio::own_thread(replay_prio_level()));
    const char* pin = getenv("RX_VK_RECORDER_PIN");
    if (!pin || !*pin) return;
    const uint64_t reserved = host && host->reserved_cpus ? host->reserved_cpus() : 0;
    uint64_t m = 0;
    if (!strcmp(pin, "next")) m = reserved ? reserved >> __builtin_popcountll(reserved) : 0;
    else if (!strcmp(pin, "sibling")) m = reserved;
    else m = strtoull(pin, nullptr, 16);
    const bool ok = m && SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)m) != 0;
    ::mkwvk::logf("vk: RX_VK_RECORDER_PIN=%s: the recorder %s %#llx (reserved %#llx)", pin, ok ? "pinned to" : "NOT pinned, mask",
                  (unsigned long long)m, (unsigned long long)reserved);
}, true);
// (RX_VK_BATCH: see queue_flush_locked)
VkSemaphore batch_tl = VK_NULL_HANDLE;
uint64_t batch_next = 0, batch_flushed = 0;
struct { std::vector<VkCommandBufferSubmitInfo> cbs; uint64_t signal = 0; bool any_signal = false; uint64_t rec = 0; size_t recs = 0; } pend;   // (rec: the recordings' timeline value the held ones reach; recs: how many recordings, RX_VK_BATCH=parts)
int64_t st_flushes, st_batched_cbs;
enum { FR_OTHER, FR_POLL, FR_WAIT, FR_SLOT, FR_PRESENT, FR_CAP, FR_VALUE, FR_COUNT };
int64_t st_reason[FR_COUNT];
int flush_reason = FR_OTHER;
std::chrono::steady_clock::time_point pend_since;
// (Triforce) What the stall line reads (stall_watch.h, status_tick), counted where it is spent: the waits for write-backs still
// on the GPU (vk_timeline_wait: the captures' late pages, the reads and bindings of pages put off), the highest timeline value
// handed to the queue and where each present's blit went among them (the queue runs in order: a wait for a value after a blit
// waits for the blit's swapchain image too), the game's fence waits (wait_counted) and the present's own time
struct StallCounts {
    std::atomic<int64_t> wb_wait_ns{0}, wb_waits{0}, wb_behind_present{0};
    std::atomic<int64_t> wb_unsent{0}, wb_unsent_ns{0};   // (those whose value was not on the queue yet when the wait began)
    std::atomic<int64_t> wb_held{0}, wb_held_ns{0};   // ((Triforce, wp20) those that went on after the writer's recording was done,
                                                      //  and that part of them: the timeline held, wb_rec_wait.h)
    std::atomic<int64_t> qs_bg{0}, qs_bg_ns{0};   // ((Triforce) queue submits made while the background thread imported, their time)
    std::atomic<uint64_t> wb_ahead_max{0};   // (since the stall line's last look: taken with exchange)
    std::atomic<uint64_t> queue_top{0};      // (dev.present_mu: written; read anywhere)
    std::atomic<uint64_t> blit_after[8];     // (the queue's top when each of the last eight blits went to it)
    std::atomic<uint32_t> blits{0};
    std::atomic<int64_t> game_wait_ns{0}, game_waits{0};
    std::atomic<int64_t> present_ns{0};
} stall_ct;
void note_queue_top(uint64_t v) {   // (dev.present_mu held)
    if (v > stall_ct.queue_top.load(std::memory_order_relaxed)) stall_ct.queue_top.store(v, std::memory_order_relaxed);
}

struct Recording {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPool upload_pool = VK_NULL_HANDLE;   // (its own: a parallel replay's threads record uploads into it while the
                                                  // replay thread records cmd, and a pool is one thread's at a time)
    VkCommandBuffer cmd = VK_NULL_HANDLE, upload = VK_NULL_HANDLE;
    // (a parallel replay's segment: new images' first layout changes, submitted before its uploads. Its own pool: any
    // segment's worker records into the earliest open segment's, while that segment's worker records cmd and upload)
    VkCommandPool init_pool = VK_NULL_HANDLE;
    VkCommandBuffer init = VK_NULL_HANDLE;
    uint64_t value = 0;   // the timeline value its submit signals (reusable once reached)
    int slot = 0;         // its index: the draw path keeps a descriptor pool and stream per recording
    // (its slot comes round again once its own submit is done: a fence of its own. The timeline is signalled as far
    // as no older submission is unfinished (submit_job), and a slot waiting on its value could wait on the replay)
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
    uint64_t batch = 0;   // (RX_VK_BATCH: the batch it went out in, waited for on batch_tl; 0: its own fence)
    bool segment = false;   // (Triforce: a parallel replay's segment last: its GPU time is counted apart too, gpu_time_collect)
    bool seg_unbegun = false;   // ((Triforce) RX_VK_SEG_SETUP_PAR: a segment's slot taken, its command buffers begun by its job)
};
// (24: the event waits cut a frame's command buffers into pieces, each with recordings of its own, and with 16 the
// replay waited ~1.5 ms a frame for a slot the GPU still had)
constexpr int kRecordings = 48;   // (test: 24 before; with RX_VK_BATCH the ring came round to held submits once a frame)
Recording recordings[kRecordings];
int recording_index = 0;

/// (a recording slot's last submit done: before it is used again)
void recording_wait(Recording& r) {
    if (!r.submitted) return;
    if (r.batch) {
        {
            std::lock_guard lk(dev.present_mu);
            if (batch_flushed < r.batch) flush_reason = FR_SLOT, queue_flush_locked();
        }
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &batch_tl;
        wi.pValues = &r.batch;
        vkWaitSemaphores(dev.device, &wi, UINT64_MAX);
        r.batch = 0;
        r.submitted = false;
        return;
    }
    vkWaitForFences(dev.device, 1, &r.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev.device, 1, &r.fence);
    r.submitted = false;
}

uint64_t completed() {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(dev.device, timeline, &v);
    return v;
}
// ---- (test) RX_VK_BATCH=1: the replay's submits held and sent as one vkQueueSubmit2 ----
// On amdgpu every command submission pays for all imported host memory (userptr) again; the replay made ~6 a frame.
// Held submits go out at the next point that needs the GPU's results or the queue's order: a wait, a readback, a
// present, a sparse bind, another submit (queue_flush_locked under dev.present_mu).
// (hookshot) on by default with Mesa's RADV, where it was measured (issue 413: a Radeon 8060S on Bazzite, ~10 -> 30-50 fps, the
// replay's queue submit ~70 -> ~6 ms a frame); elsewhere as before. RX_VK_BATCH=0 / 1: off / on on any driver
// (Triforce) RX_VK_BATCH=parts (barrier_rules.h), off unless set: only a whole submission's recordings are held, and only while the
// game's submit is replayed (submit_job): a part goes at once with those held before it, as do 4 held (RX_VK_BATCH_MAX), and the
// held go when submit_job returns. NX2navi holds its submissions the same way (up to 4, its parts at once). Not the default with
// NVIDIA's driver: there a queue submit is about 10 us, the GPU's early start is what the parts are for, and the submits a game's
// submit sets off are mostly one recording already (its burst), so little is left to join; RX_VK_BATCH=1 holds the parts too
barrier_rules::Batch batch_way() {
    static const barrier_rules::Batch way = [] {
        bool radv = false;
        VkPhysicalDeviceDriverProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &dp;
        if (dev.physical) vkGetPhysicalDeviceProperties2(dev.physical, &p2), radv = dp.driverID == VK_DRIVER_ID_MESA_RADV;
        const char* e = getenv("RX_VK_BATCH");
        const barrier_rules::Batch v = barrier_rules::batch_mode(e, radv);
        logf("vk: batched submits %s (RX_VK_BATCH%s)",
             v == barrier_rules::Batch::Parts ? "on for whole submissions, while a game's submit is replayed (parts at once)"
             : v == barrier_rules::Batch::Hold ? "on" : "off", !e && radv ? ", on with RADV" : "");
        return v;
    }();
    return batch_tl != VK_NULL_HANDLE ? way : barrier_rules::Batch::Off;
}
bool batch_on() {
    return batch_way() != barrier_rules::Batch::Off;
}
thread_local int submit_depth = 0;   // ((Triforce) RX_VK_BATCH=parts: inside submit_job, the replay of a game's submit)
// ((Triforce) RX_VK_SPLIT_BARRIER, maybe_split: how the parts' splits went, logged every 3,600 splits; and the command buffer of the
// last part a pass went on into, for whether its uploads (which have full barriers of their own) ordered it after the part before)
VkCommandBuffer split_carried_cmd = VK_NULL_HANDLE;
std::atomic<int64_t> st_signals_held{0}, st_waits_barred{0};   // (RX_VK_EVENT_BARRIER: signals held, waits given a barrier for one)
int64_t st_split[6];   // (splits; with the rule on; a pass open; nothing recorded at the part's end; went on in the pass; of those, uploads)
void split_count(bool on, bool open, bool quiet, bool held, bool carry) {
    st_split[0]++, st_split[1] += on, st_split[2] += open, st_split[3] += open && quiet && !held, st_split[4] += carry;
    if (st_split[0] % 3600) return;
    logf("vk: a submission's parts (3600 splits): %lld with RX_VK_SPLIT_BARRIER's rule, %lld of them in an open pass, %lld with nothing "
         "recorded at the part's end (no uniform update, write-back or event's write held), %lld went on in the pass behind its "
         "attachments' barrier (%lld of those parts had uploads, whose "
         "full barriers order them after the part before all the same); %lld waits after a signal whose write was held, %lld signals held",
         (long long)st_split[1], (long long)st_split[2], (long long)st_split[3], (long long)st_split[4], (long long)st_split[5],
         (long long)st_waits_barred.load(std::memory_order_relaxed), (long long)st_signals_held.load(std::memory_order_relaxed));
    for (int64_t& k : st_split) k = 0;
    st_waits_barred = 0, st_signals_held = 0;
}
void split_part_submitted(VkCommandBuffer cmd, bool uploads) {
    if (cmd != split_carried_cmd) return;
    split_carried_cmd = VK_NULL_HANDLE;
    st_split[5] += uploads;
}
} // namespace
// ((Triforce) RX_VK_LATE_QUEUE, vk_memory.cpp late_queue_on: the takes' late pages put off to the queue. Before anything goes to it,
// the pending fixes' write-backs waited for (sent already: the GPU reaches them with nothing more submitted) and their pages copied
// again into their captures' buffers, as the take did after its wait. At most 250 ms at a time, 4 times: a submission the event
// ordering holds behind a later one cannot hold the queue's lock for ever (the fix goes in as it is, counted)
uint64_t vk_timeline_signal_sent() { return stall_ct.queue_top.load(std::memory_order_relaxed); }
bool vk_timeline_wait_sent(uint64_t v) {
    if (!timeline) return true;
    for (int i = 0; i < 4; i++) {
        if (completed() >= v) return true;
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1, wi.pSemaphores = &timeline, wi.pValues = &v;
        if (vkWaitSemaphores(dev.device, &wi, 250000000ull) == VK_SUCCESS) return true;
    }
    return completed() >= v;
}
namespace {
std::atomic<int64_t> st_lq_waits{0}, st_lq_wait_ns{0}, st_lq_timeouts{0};
}
void late_fix_flush_locked() {   // (dev.present_mu held)
    const uint64_t v = memory_late_fix_pending();
    if (!v) return;
    if (completed() < v) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!vk_timeline_wait_sent(v)) {
            static std::atomic<int> logged{0};
            if (logged++ < 8) logf("vk: RX_VK_LATE_QUEUE: write-back %llu not done after 1 s (done %llu): the late pages copied as they are",
                                   (unsigned long long)v, (unsigned long long)completed());
            st_lq_timeouts.fetch_add(1, std::memory_order_relaxed);
        }
        st_lq_waits.fetch_add(1, std::memory_order_relaxed);
        st_lq_wait_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                                std::memory_order_relaxed);
    }
    memory_late_fix_apply(UINT64_MAX);   // (all pending: v is the highest of them)
}
void vk_late_queue_stats(int64_t* waits, int64_t* wait_ns, int64_t* timeouts) {
    *waits = st_lq_waits.exchange(0), *wait_ns = st_lq_wait_ns.exchange(0), *timeouts = st_lq_timeouts.exchange(0);
}
void queue_flush_locked() {   // (dev.present_mu held)
    late_fix_flush_locked();
    if (pend.cbs.empty()) return;
    VkSemaphoreSubmitInfo sig[3]{{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO},
                                 {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    uint32_t n = 0;
    if (pend.any_signal) {
        sig[n].semaphore = timeline, sig[n].value = pend.signal, sig[n].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        n++;
    }
    if (pend.rec) {   // (the recordings' timeline too: wait_recorded waits on it, and held submits carried no signal of it)
        sig[n].semaphore = rec_timeline, sig[n].value = pend.rec, sig[n].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        n++;
    }
    sig[n].semaphore = batch_tl, sig[n].value = batch_next, sig[n].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    n++;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = (uint32_t)pend.cbs.size();
    si.pCommandBufferInfos = pend.cbs.data();
    si.signalSemaphoreInfoCount = n;
    si.pSignalSemaphoreInfos = sig;
    VkSemaphoreSubmitInfo bind_wait;   // (Triforce: after the memory of the mirrors' new pieces is bound, vk_memory.cpp)
    if (memory_mirror_bind_wait(&bind_wait)) si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &bind_wait;
    PROF(PROF_QSUBMIT);
    const VkResult res = vkQueueSubmit2(dev.queue, 1, &si, VK_NULL_HANDLE);
    static int failures;
    if (res != VK_SUCCESS && failures++ < 8) logf("vk: batched submit %llu failed (%d)", (unsigned long long)batch_next, (int)res);
    if (res == VK_ERROR_DEVICE_LOST) device_lost();
    if (res == VK_SUCCESS && pend.any_signal) note_queue_top(pend.signal);   // (Triforce: the stall line's)
    batch_flushed = batch_next;
    st_flushes++, st_batched_cbs += (int64_t)pend.cbs.size();
    st_reason[flush_reason]++;
    flush_reason = FR_OTHER;
    pend.cbs.clear(), pend.any_signal = false, pend.signal = 0, pend.rec = 0, pend.recs = 0;
}
void queue_flush() {
    std::lock_guard lk(dev.present_mu);
    queue_flush_locked();
}
void device_wait_idle_locked() {   // (dev.present_mu held)
    queue_flush_locked();
    std::lock_guard pq(dev.present_queue_mu);
    vkDeviceWaitIdle(dev.device);
}
namespace {
void queue_flush_for(int why) {
    std::lock_guard lk(dev.present_mu);
    if (pend.cbs.empty()) return;
    flush_reason = why;
    queue_flush_locked();
}
// (RX_VK_BATCH_POLL_US=<us>: a progress poll sends held submits only once the oldest has waited that long; 0: always.
// Default 2000: the game polls many times a frame, and each poll that flushed was a whole command submission, the cost
// batching is there to avoid. RX 7800 XT, RADV, Kokiri: 525 -> 67 poll flushes per 120 presents, 38 -> 42.5 fps; issues
// 413, 528)
void queue_flush_poll() {
    static const int64_t us = getenv("RX_VK_BATCH_POLL_US") ? atoll(getenv("RX_VK_BATCH_POLL_US")) : 2000;
    std::lock_guard lk(dev.present_mu);
    if (pend.cbs.empty()) return;
    if (us > 0 && std::chrono::steady_clock::now() - pend_since < std::chrono::microseconds(us)) return;
    flush_reason = FR_POLL;
    queue_flush_locked();
}
} // namespace
namespace {
void wait_value(uint64_t v) {
    queue_flush_for(FR_VALUE);
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &v;
    vkWaitSemaphores(dev.device, &wi, UINT64_MAX);
}
bool wait_recorded(uint64_t f, uint64_t timeout_ns, int* ok);
/// (Triforce, lab; upstream #2419, #2458, #2125) the readback chain's three fixes, each behind a switch while they are measured:
/// RX_VK_REC_LAST=1 a value's recording is its LAST part (a fence or readback wait covers the whole submission),
/// RX_VK_TL_REC=1 the capture's wait for a write-back waits on the recordings' timeline where its recording is known,
/// RX_VK_RB_SETTLE=1 a readback lets the stopped work whose waits are met go on before it looks its write-back up.
/// In-run A/B (lab_ab.h): the knobs reclast, tlrec, rbsettle turn each on for their phases
bool rb_fix(int which) {
    static const bool env[3] = {getenv("RX_VK_REC_LAST") && atoi(getenv("RX_VK_REC_LAST")) != 0,
                                getenv("RX_VK_TL_REC") && atoi(getenv("RX_VK_TL_REC")) != 0,
                                getenv("RX_VK_RB_SETTLE") && atoi(getenv("RX_VK_RB_SETTLE")) != 0};
    static const char* const knob[3] = {"reclast", "tlrec", "rbsettle"};
    return env[which] || lab_ab::on(knob[which]);
}
} // namespace
uint64_t vk_timeline_completed() { return timeline ? completed() : 0; }
/// ((Triforce, wp20, wb_rec_wait.h) RX_VK_WB_WAIT_REC=1: a wait for a write-back ends once its writer's recording is done
static bool wb_wait_rec_on() {
    static const bool v = [] {
        const bool on = ::mkwvk::wb_rec_wait::on(getenv("RX_VK_WB_WAIT_REC"));
        if (on) logf("vk: RX_VK_WB_WAIT_REC=1: a wait for a write-back still on the GPU ends once the recording its submission was "
                     "finished in is done, not when the submissions' timeline reaches it");
        return on;
    }();
    return v;
}
static thread_local int64_t wb_held_last_ns = 0;   // ((Triforce, wp20) this thread's last wait's held part: vk_timeline_wait_held_last)
int64_t vk_timeline_wait_held_last() { return wb_held_last_ns; }
void vk_timeline_wait(uint64_t v) {   // (at most 250 ms: a submission held back behind the caller's thread goes on without it)
    wb_held_last_ns = 0;
    if (!timeline) return;
    const uint64_t done0 = completed();
    if (done0 >= v) return;
    const auto t0 = std::chrono::steady_clock::now();
    // (Triforce, the stall line: a value past the queue's top was not handed to the GPU yet (a submission the event ordering
    // holds back, or one not recorded yet): its wait is the replay's, not the GPU's work)
    const bool unsent = v > stall_ct.queue_top.load(std::memory_order_relaxed);
    if (rb_fix(1)) {   // (upstream #2458: where the recording that carries the value is known, its own timeline)
        int ok = 0;
        if (vk_ft_on()) vk_ft("gpuwait> value %llu on the recordings timeline (done %llu)", (unsigned long long)v, (unsigned long long)done0);
        if (wait_recorded(v, 250000000ull, &ok)) {
            const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            if (vk_ft_on()) vk_ft("gpuwait< %.3f ms (recording%s)", ns / 1e6, ok ? "" : ", timed out");
            stall_ct.wb_wait_ns.fetch_add(ns, std::memory_order_relaxed);
            stall_ct.wb_waits.fetch_add(1, std::memory_order_relaxed);
            if (!ok) {
                static std::atomic<int> logged{0};
                if (logged++ < 8) logf("vk: a capture's wait for write-back %llu timed out on the recordings' timeline (done %llu)", (unsigned long long)v, (unsigned long long)completed());
            }
            return;
        }
    }
    if (vk_ft_on()) vk_ft("gpuwait> value %llu (done %llu%s)", (unsigned long long)v, (unsigned long long)done0, unsent ? ", not sent yet" : "");
    queue_flush_for(FR_VALUE);
    // ((Triforce, wp20; the handoff's §12.9) first the recording v was finished in, on the recordings' timeline: once it is done
    // so are v's write-backs, whatever older submission stopped at an event wait holds the timeline below v (finish_recording).
    // Never longer than the timeline's own wait, which cannot reach v before it; what the timeline's wait takes after it is the
    // timeline held (stall_ct.wb_held_ns). RX_VK_WB_WAIT_REC=1: the wait ends there. Not known (v still being recorded, stopped,
    // or no recordings' timeline): the timeline's wait alone, as before. wb_rec_wait.h)
    namespace wr = ::mkwvk::wb_rec_wait;
    uint64_t seq = 0;
    if (rec_timeline) {
        std::lock_guard mk(rec_map_mu);
        seq = fin_rec.of(v);
    }
    bool rec_done = false;
    int64_t rec_ns = 0;
    if (seq) {
        VkSemaphoreWaitInfo ri{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        ri.semaphoreCount = 1;
        ri.pSemaphores = &rec_timeline;
        ri.pValues = &seq;
        rec_done = vkWaitSemaphores(dev.device, &ri, wr::kBoundNs) == VK_SUCCESS;
        rec_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }
    if (!wr::ends_at_recording(wb_wait_rec_on(), seq != 0, rec_done)) {
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &timeline;
        wi.pValues = &v;
        if (vkWaitSemaphores(dev.device, &wi, wr::budget_left(wr::kBoundNs, rec_ns)) == VK_TIMEOUT) {
            static std::atomic<int> logged{0};
            if (logged++ < 8) logf("vk: a capture's wait for write-back %llu timed out (done %llu)", (unsigned long long)v, (unsigned long long)completed());
        }
    }
    // (Triforce) for the stall line: how long, how far ahead of the GPU, and whether a present's blit lay between: a blit that
    // went to the queue when its top was b runs before every value over b, after its swapchain image is acquired
    const int64_t waited_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    if (vk_ft_on()) vk_ft("gpuwait< %.3f ms", waited_ns / 1e6);
    stall_ct.wb_wait_ns.fetch_add(waited_ns, std::memory_order_relaxed);
    stall_ct.wb_waits.fetch_add(1, std::memory_order_relaxed);
    if (unsent) stall_ct.wb_unsent.fetch_add(1, std::memory_order_relaxed), stall_ct.wb_unsent_ns.fetch_add(waited_ns, std::memory_order_relaxed);
    if (const int64_t held = wr::held_ns(seq != 0, rec_done, rec_ns, waited_ns)) {   // ((Triforce, wp20) the timeline held)
        stall_ct.wb_held.fetch_add(1, std::memory_order_relaxed), stall_ct.wb_held_ns.fetch_add(held, std::memory_order_relaxed);
        wb_held_last_ns = held;
    }
    const uint64_t ahead = v - done0;
    uint64_t was = stall_ct.wb_ahead_max.load(std::memory_order_relaxed);
    while (ahead > was && !stall_ct.wb_ahead_max.compare_exchange_weak(was, ahead, std::memory_order_relaxed)) {}
    for (const auto& b : stall_ct.blit_after) {
        const uint64_t at = b.load(std::memory_order_relaxed);
        if (at && at >= done0 && at < v) {
            stall_ct.wb_behind_present.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
}
namespace {

int64_t st_wait_ns;   // (time the replay waited for a recording slot: the GPU behind)

// GPU time: a timestamp at the start and the end of each recording's command buffer, read when its slot comes
// round again (the work done by then)
VkQueryPool ts_pool = VK_NULL_HANDLE;
bool ts_pending[kRecordings];
int64_t st_gpu_ns, st_gpu_max_ns;
// (Triforce) and the same around the recording's upload command buffer, which goes to the queue before it when it holds any
// uploads: the textures' staging copies, the GPU's unswizzles and ASTC decodes, the mirrors' syncs. The GPU time above leaves
// it out: after an area change's 360 MB of uploads (the owner's test5 log) the 120-present line said 0.5 ms a frame of GPU
// time while a capture waited 61.6 ms for a write-back four submissions ahead
VkQueryPool ts_up_pool = VK_NULL_HANDLE;
bool ts_up_pending[kRecordings];
int64_t st_gpu_up_ns, st_gpu_up_max_ns;
// (Triforce, 11-15) the parallel replay's segments have their timestamps too (they had none: in the owner's test8 the GPU
// time line read 2.2 to 2.8 ms a frame while the helpers recorded, 6.4 without them), and their share is counted apart
int64_t st_gpu_seg_ns;
void gpu_time_collect(Recording& r) {
    if (ts_up_pool && ts_up_pending[r.slot]) {
        ts_up_pending[r.slot] = false;
        uint64_t t[2] = {0, 0};
        if (vkGetQueryPoolResults(dev.device, ts_up_pool, 2 * r.slot, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            t[1] > t[0]) {
            const int64_t ns = (int64_t)((t[1] - t[0]) * (double)dev.props.limits.timestampPeriod);
            st_gpu_up_ns += ns;
            st_gpu_up_max_ns = std::max(st_gpu_up_max_ns, ns);
            if (r.segment) st_gpu_seg_ns += ns;
        }
    }
    if (!ts_pool || !ts_pending[r.slot]) return;
    ts_pending[r.slot] = false;
    uint64_t t[2] = {0, 0};
    if (vkGetQueryPoolResults(dev.device, ts_pool, 2 * r.slot, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    if (t[1] <= t[0]) return;
    const int64_t ns = (int64_t)((t[1] - t[0]) * (double)dev.props.limits.timestampPeriod);
    st_gpu_ns += ns;
    st_gpu_max_ns = std::max(st_gpu_max_ns, ns);
    if (r.segment) st_gpu_seg_ns += ns;
}
void gpu_time_begin(Recording& r) {
    if (!ts_pool) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * kRecordings;
        if (vkCreateQueryPool(dev.device, &qi, nullptr, &ts_pool) != VK_SUCCESS) ts_pool = VK_NULL_HANDLE;
        if (!ts_pool) return;
        if (vkCreateQueryPool(dev.device, &qi, nullptr, &ts_up_pool) != VK_SUCCESS) ts_up_pool = VK_NULL_HANDLE;
    }
    vkCmdResetQueryPool(r.cmd, ts_pool, 2 * r.slot, 2);
    vkCmdWriteTimestamp2(r.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, ts_pool, 2 * r.slot);
    if (ts_up_pool) {   // (in the upload command buffer, begun just before)
        vkCmdResetQueryPool(r.upload, ts_up_pool, 2 * r.slot, 2);
        vkCmdWriteTimestamp2(r.upload, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, ts_up_pool, 2 * r.slot);
    }
}
/// uploads: the upload command buffer goes to the queue (submit_recording); one that does not is never read
void gpu_time_end(Recording& r, bool uploads) {
    if (!ts_pool) return;
    vkCmdWriteTimestamp2(r.cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, ts_pool, 2 * r.slot + 1);
    ts_pending[r.slot] = true;
    if (ts_up_pool) {
        vkCmdWriteTimestamp2(r.upload, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, ts_up_pool, 2 * r.slot + 1);
        ts_up_pending[r.slot] = uploads;
    }
}


// ---- (Triforce, 11-17) samples-passed reports from occlusion queries: RX_VK_OCC_QUERY (default on, =0 off; occ_answer.h) ----
// NVN_COUNTER_TYPE_SAMPLES_PASSED reports (the game's render job f_80084de0, near lr 0x800876dc: ~1,700 a frame at Castle
// Town in the lab, into a ring of report records: ~49,000 addresses in 600 presents) tell the game which objects show; it
// does not draw one whose count is 0. The replay writes each report as it passes it (NOP_REPORT_COUNTER), with 4096 for the
// count (everything visible: RX_VK_OCC_FIXED). The lab's upper bound, every count 0 (everything hidden, which breaks the
// picture and turns the sun white): 2955 -> 1826 draws at Castle Town, 1522 -> 1093 at the Market, +17-18% fps.
// With this the replay still writes that placeholder, where and as it did, and the draws from a reset to its report are
// counted by Vulkan occlusion queries (precise where the device has occlusionQueryPrecise); once the recording that holds
// them is done on the GPU the count goes into the report's record, over the placeholder, as NX2navi writes a report at
// completion (runtime/vk_exec_xfer.c). The game reads its counts once it has waited for the frame. (11-17, the lab: an
// earlier frame's count kept per address could not answer a report as it is written: the addresses rotate, the last count
// for one was another object's or too old, and most reports are written by the dry pass, far ahead of the GPU.)
//   - each recording slot has its block of kOccPerSlot queries, and each begin of a slot a new serial (occ_slot_begin): a
//     count is matched to its recording by that serial, never by the command buffer's handle, which the slot reuses every
//     kRecordings recordings. The block is reset on the host after it is read (hostQueryReset: only the queries the
//     recording took), else by the recording's command buffer as it begins. A query is begun by the first draw of a pass
//     while counting (vk_draw.cpp, occ_draw_begin) and ended with the pass (end_pass) or at the report. A count's queries
//     follow one another in their block (occ::Counter);
//   - the counting: the replay's, one per NVN queue's state (occ_mains, under replay_mu, whichever thread holds it: with
//     RX_VK_ASYNC=0 the game's submitting threads take turns), and a parallel replay's segment's, the thread's own
//     (occ_seg). A parallel replay gives the replay's counting up (occ_serial_give_up): a report after it without a reset
//     of its own is not measured;
//   - a report counted whole goes with its recording's slot (OccSlot::reps), by its address and its place in the stream
//     (occ_pos: the same for the dry pass that writes its placeholder and the thread that records its draws). When the
//     recording is submitted its slot is sealed with its number on the recordings' timeline (occ_seal): submit_recording
//     for the replay's recordings, and replay_handle's own submit for a parallel replay's segments, which signal the
//     recordings' timeline too while the queries run (11-17, the lab: the segments' recordings were never sealed, and
//     their counts, 83% of all, were dropped as "never sent" when their slots came round). The sealed slots the GPU is done
//     with are read (one look at that timeline, no wait, occ_harvest_done) where the GPU's progress is learnt anyway: as a
//     game's fence wait ends (wait_counted: the game reads its counts after it), at each recording's begin and at each
//     present; at the latest when the slot comes round again (recording_wait has waited for it);
//   - the count is written (occ_write) only when it is whole (every query available, no draw left out, reset and report
//     in one recording), read back at most occ::kLateAge presents after its report, its report is still the newest at its
//     address (occ::Writers: the ring has not come round to another object's report there, no small buffer clear or other
//     counter type rewrote the record in the stream since), and the word still holds what the replay left there: a
//     compare-and-write under occ_mu, so a count never lands over the game's CPU re-arming the record (NX2navi's
//     vk_exec_xfer.c Pending: "Ocarina re-arms its condition records with 1 for the next frame"). Else the record keeps
//     its 4096 (or what the CPU wrote). A count of 0 is written as 0: the object is hidden;
//   - a render condition (NOP_RENDER_COND) reads report words as the replay passes it (the replay thread or the dry pass,
//     in stream order with the placements), decided as NX2navi decides it (vk_exec_xfer.c counter_word, cond_enabled): a
//     word whose newest report is a samples-passed one whose count is still to come is unknown, and the condition renders;
//     a word whose count was written, or for which none comes, is read from memory as before, where a written 0 can skip
//     the draws and clears it gates (occ_cond, occ::cond_decide). RX_VK_OCC_COND=0: a condition on a samples-passed word
//     always renders (the counts are still written). In the lab knob's off phases the answer is the one from memory, as
//     before. RX_VK_OCC_LOG_COND=1: one such condition in 499 logged (300 lines), with both words and their sources.
// The writes, the coherence: a count is written as the replay writes a report (the guest page marked written, the
// write-back diff's shadows told, memory_cpu_owned: the word is cut out of the mirrors' write-backs, so a mirror's stale
// copy never lands over it; with the lazy write-back such a page is never owed, and a CPU write to an owed page pays first),
// and under occ_mu, as is each placeholder's write with its Writers entry and a small clear's: a count cannot land after a
// later report's placeholder or a clear. occ_mu is never taken with dev.present_mu held (a lazy payment takes it).
// RX_VK_OCC_QUERY_SKIP=<addr>[,...]: these report addresses keep the fixed answer. The line every 600 presents is followed
// (at its 1st and every 10th) by the first 64 report addresses with their counts.
// Lab: RX_VK_LAB_AB naming the knob occq runs the queries the whole run; a report placed in a phase without the knob gets
// no count written (4096 stays, as before).
constexpr uint32_t kOccPerSlot = 1024;   // (a recording is ~120 draws, a segment ~160 or more; a count is a query a pass)
struct OccSlot {
    uint32_t used = 0;       // (queries of the block taken by the recording made in it, not yet reset)
    bool armed = false;      // (its block is reset: queries may begin)
    uint64_t serial = 0;     // (the recording being made in it: a new one at each begin, occ_serial_next)
    std::vector<occ::Rep> reps;   // (reports counted whole in it: its recording thread's until it is sealed)
    bool sealed = false;     // (submitted: used and reps are the readers' (occ_mu) until it is read or begun again)
    uint64_t seq = 0;        // (its number on the recordings' timeline; 0: none, its fence or batch tells)
};
OccSlot occ_slots[kRecordings];
std::atomic<uint64_t> occ_serial_next{0};
std::atomic<int> occ_pending{0};         // (sealed slots with reports not read yet; always 0 with the switch off)
std::atomic<int64_t> occ_presents{0};    // (presents_now, for every thread: the latencies, the prune)
VkQueryPool occ_pool = VK_NULL_HANDLE;
VkQueryControlFlags occ_flags = 0;
bool occ_host_reset = false;   // (hostQueryReset: a block's used queries reset on the host once read, not all in each recording)
bool occ_env_real = false;     // (RX_VK_OCC_QUERY=1: the counts written; else only for reports in the lab knob's phases)
bool occ_cond_on = true;       // (RX_VK_OCC_COND=0: a render condition on a samples-passed word always renders)
bool occ_log_cond = false;     // (RX_VK_OCC_LOG_COND=1: one such condition in 499 logged, 300 lines at most)
std::vector<uint64_t> occ_skip;
std::mutex occ_mu;   // (the sealed slots, the writers, the stats, the sites; a placeholder's write with its entry; the counts' writes)
// (the replay thread's, in stream order (the serial replay or the dry pass): the queue state whose render condition a
// written count turned off, until its next condition or render enable; the draws and clears it skips, for the line)
const NvnState* occ_cond_off = nullptr;
std::atomic<int64_t> occ_skips_draw{0}, occ_skips_clear{0};
occ::Writers occ_writers;
occ::Stats occ_st;
occ::Sites occ_sites;
occ::Distinct occ_distinct;
std::atomic<bool> occ_keep{false};   // (the game reported twice without a reset: counting goes on past reports, as NVN counts)
occ::Counter& occ_ctr();   // (the counting the thread records for: the replay's, its queue's, or the segment's; below rmode)
thread_local bool occ_open_here = false;   // (this thread began the query open now: end_pass on another thread leaves it alone)
/// RX_VK_OCC_QUERY=1, or the lab knob occq: the pool and the switches, once (begin_recording); false: off, nothing changes
bool occ_init() {
    // (11-17: on by default after the night's measures, +2.7..+4.6% in every scene, no flicker; RX_VK_OCC_QUERY=0: off.
    // With the lab knob occq and no switch set, the knob's phases decide as before)
    const char* e = getenv("RX_VK_OCC_QUERY");
    lab_ab::init();
    bool knob = false;
    for (int k = 0; k < lab_ab::n_knobs; k++) knob = knob || occ::knob_names(lab_ab::knobs[k].name, "occq");
    occ_env_real = e ? atoi(e) != 0 : !knob;
    if (!occ_env_real && !knob) return false;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qi.queryCount = kOccPerSlot * kRecordings;
    if (vkCreateQueryPool(dev.device, &qi, nullptr, &occ_pool) != VK_SUCCESS) {
        occ_pool = VK_NULL_HANDLE;
        logf("vk: occlusion queries (RX_VK_OCC_QUERY): the query pool could not be made: samples-passed reports stay fixed");
        return false;
    }
    occ_flags = dev.features.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
    occ_host_reset = dev.vk12.hostQueryReset != VK_FALSE;
    if (occ_host_reset) vkResetQueryPool(dev.device, occ_pool, 0, qi.queryCount);   // (all of them, once: then as they are read)
    occ_skip = occ::parse_addrs(getenv("RX_VK_OCC_QUERY_SKIP"));
    occ_cond_on = !getenv("RX_VK_OCC_COND") || atoi(getenv("RX_VK_OCC_COND")) != 0;
    occ_log_cond = getenv("RX_VK_OCC_LOG_COND") && atoi(getenv("RX_VK_OCC_LOG_COND")) != 0;
    logf("vk: occlusion queries (RX_VK_OCC_QUERY%s): samples-passed reports written 4096 at the replay as before, and their "
         "counts written into the records once the GPU is done%s; %s counts, %d recordings x %u queries, reset %s; done "
         "recordings found on %s; %zu report addresses kept fixed (RX_VK_OCC_QUERY_SKIP); render conditions on their words: %s%s",
         occ_env_real ? "=1" : ", lab knob occq", occ_env_real ? "" : " (for reports in the knob's phases)",
         occ_flags ? "precise" : "non-precise (no occlusionQueryPrecise)", kRecordings, kOccPerSlot,
         occ_host_reset ? "on the host once read" : "in each recording (no hostQueryReset)",
         rec_timeline ? "the recordings' timeline" : "their fences (no recordings' timeline: the replay thread only)", occ_skip.size(),
         occ_cond_on ? "a count still to come renders, a written one is read from memory (a 0 skips)"
                     : "always rendered (RX_VK_OCC_COND=0)",
         occ_log_cond ? "; RX_VK_OCC_LOG_COND=1: one in 499 logged" : "");
    return true;
}
/// The recording slot whose command buffer this is, its block armed (-1: none)
int occ_slot_of(VkCommandBuffer cmd) {
    for (int i = 0; i < kRecordings; i++)
        if (recordings[i].cmd == cmd) return occ_slots[i].armed ? i : -1;
    return -1;
}
/// The serial of the recording being made in cmd (0: none)
uint64_t occ_serial_of(VkCommandBuffer cmd, int* slot) {
    *slot = occ_slot_of(cmd);
    return *slot >= 0 ? occ_slots[*slot].serial : 0;
}
/// (occ_mu held) A count into its report's record, the count word only (the timestamp beside it stays the replay's), and
/// only while the word still holds expect, what the replay left there: a compare-and-write on the guest word itself, so a
/// count read back late never lands over the game's CPU re-arming the record for its next frame (NX2navi vk_exec_xfer.c,
/// the Pending table's comment: the CPU re-arms its condition records with 1 between frames; a later CPU write simply
/// overwrites the count, in its order). Then as the replay writes a record (NOP_REPORT_COUNTER, guest_cpu_write): the
/// guest page marked written, the write-back diff's shadows told (guest_cpu_wrote), and the word kept out of the mirrors'
/// write-backs (memory_cpu_owned: no write-back of a mirror's stale copy lands over it). False: not written
bool occ_write(uint64_t addr, uint64_t expect, uint64_t count) {
    uint64_t avail = 0;
    uint8_t* const p = host->gpu_ptr ? host->gpu_ptr(addr, &avail) : nullptr;
    bool ok;
    if (p && avail >= 8 && !((uintptr_t)p & 7)) {
        ok = __atomic_compare_exchange_n(reinterpret_cast<uint64_t*>(p), &expect, count, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
        if (ok && host->mark_written) host->mark_written(p, 8);
    } else {   // (not one aligned word of the window: read, compare and write; no record of the game's is like that)
        uint64_t now = 0;
        ok = host->gpu_read(addr, &now, 8) && now == expect && host->gpu_write(addr, &count, 8);
    }
    if (!ok) return false;
    guest_cpu_wrote(addr, 8);
    memory_cpu_owned(host->gpu_guest(addr), 16);
    return true;
}
/// (occ_mu held) A sealed slot whose recording the GPU is done with: its block read (no WAIT bit) and the whole counts
/// written into their records (occ::complete); then the queries it took reset on the host (hostQueryReset). at: where
void occ_read_slot(int i, occ::Stats::At at) {
    OccSlot& sl = occ_slots[i];
    if (!sl.reps.empty()) {
        static std::vector<uint64_t> v;   // (occ_mu)
        v.assign(2 * (size_t)sl.used, 0);
        const VkResult res = sl.used ? vkGetQueryPoolResults(dev.device, occ_pool, (uint32_t)i * kOccPerSlot, sl.used, v.size() * 8,
                                                             v.data(), 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
                                     : VK_SUCCESS;
        occ::complete(sl.reps, v.data(), sl.used, res == VK_SUCCESS || res == VK_NOT_READY, occ_writers,
                      occ_presents.load(std::memory_order_relaxed), occ_st,
                      [](uint64_t addr, uint64_t expect, uint64_t count) {
                          const bool ok = occ_write(addr, expect, count);
                          if (ok) occ_sites.wrote(addr);
                          return ok;
                      },
                      [](uint64_t addr, uint64_t count) { occ_sites.measured(addr, count); });
        occ_st.at[at]++;
        sl.reps.clear();
        if (sl.sealed) occ_pending.fetch_sub(1, std::memory_order_relaxed);
    }
    if (occ_host_reset && sl.used) vkResetQueryPool(dev.device, occ_pool, (uint32_t)i * kOccPerSlot, sl.used), sl.used = 0;
    sl.sealed = false;
}
/// The sealed recordings the GPU is done with, read and their counts written: one look at the recordings' timeline, nothing
/// waits. From any thread: a game's fence wait that ended (wait_counted), a recording's begin and a present (the replay
/// thread's, replay_mu). Without the recordings' timeline only the replay thread looks, at their fences and batches
void occ_harvest_done(occ::Stats::At at) {
    if (!occ_pending.load(std::memory_order_acquire)) return;
    if (!rec_timeline && at == occ::Stats::kAtWait) return;
    std::lock_guard lk(occ_mu);
    uint64_t done = 0, batch_done = 0;
    bool batch_read = false;
    if (rec_timeline) vkGetSemaphoreCounterValue(dev.device, rec_timeline, &done);
    for (int i = 0; i < kRecordings && occ_pending.load(std::memory_order_relaxed); i++) {
        OccSlot& sl = occ_slots[i];
        if (!sl.sealed || sl.reps.empty()) continue;
        bool is_done;
        if (sl.seq) {
            is_done = sl.seq <= done;
        } else {   // (no recordings' timeline: the replay thread, as recording_wait knows the slot)
            if (at == occ::Stats::kAtWait) continue;
            const Recording& o = recordings[i];
            if (!o.submitted) continue;
            if (o.batch) {
                if (!batch_read) vkGetSemaphoreCounterValue(dev.device, batch_tl, &batch_done), batch_read = true;
                is_done = batch_done >= o.batch;
            } else {
                is_done = vkWaitForFences(dev.device, 1, &o.fence, VK_TRUE, 0) == VK_SUCCESS;
            }
        }
        if (is_done) occ_read_slot(i, at);
    }
}
/// Before a recording slot is begun again (recording_wait has waited for its last recording): its block read if it was
/// sealed (submitted), else its reports dropped (it never went to the GPU); its queries reset either way. Then the other
/// sealed slots the GPU is done with. Here the recorder thread (RX_VK_RECORDER) is idle already: recording_wait and the
/// slot's pool reset wait for it
void occ_before_begin(Recording& r) {
    {
        std::lock_guard lk(occ_mu);
        OccSlot& sl = occ_slots[r.slot];
        if (sl.sealed) {
            occ_read_slot(r.slot, occ::Stats::kAtReuse);
        } else if (!sl.reps.empty()) {
            occ::dropped(sl.reps, occ_writers, occ_st);
            sl.reps.clear();
        }
        if (occ_host_reset && sl.used) vkResetQueryPool(dev.device, occ_pool, (uint32_t)r.slot * kOccPerSlot, sl.used), sl.used = 0;
        sl.armed = false, sl.sealed = false, sl.seq = 0;
    }
    occ_harvest_done(occ::Stats::kAtBegin);
}
/// A recording slot begun (its command buffer just begun): a new serial; the block reset in it (without hostQueryReset:
/// with it, it was reset on the host once read); queries may begin. Not sealed: nothing but its recording thread touches it
void occ_slot_begin(Recording& r) {
    OccSlot& sl = occ_slots[r.slot];
    if (!occ_host_reset) vkCmdResetQueryPool(r.cmd, occ_pool, (uint32_t)r.slot * kOccPerSlot, kOccPerSlot), sl.used = 0;
    sl.serial = occ_serial_next.fetch_add(1, std::memory_order_relaxed) + 1;
    sl.armed = true;
}
/// A recording went to the queue (submit_recording, with dev.present_mu no longer held: a reader writes guest memory under
/// occ_mu, and a lazy write-back's payment takes dev.present_mu). seq: its number on the recordings' timeline (0: none).
/// From now on its block and reports are the readers', and read once that timeline reaches seq
void occ_seal(Recording& r, uint64_t seq) {
    std::lock_guard lk(occ_mu);
    OccSlot& sl = occ_slots[r.slot];
    if (!sl.armed || sl.sealed) return;
    sl.sealed = true, sl.seq = seq;
    if (!sl.reps.empty()) occ_pending.fetch_add(1, std::memory_order_release);
}
/// The open query of c ends (inside its pass: at the pass's end, a report or a reset). Only in the recording it was begun
/// in: elsewhere it cannot be ended, and the count is lost
void occ_close(occ::Counter& c, VkCommandBuffer cmd) {
    if (c.open < 0) return;
    int slot;
    if (c.slot >= 0 && occ_serial_of(cmd, &slot) == c.serial && slot == c.slot)
        vkCmdEndQuery(cmd, occ_pool, (uint32_t)c.slot * kOccPerSlot + (uint32_t)c.open);
    else
        c.lost = true, c.why = occ::Counter::kOtherCmd;
    c.open = -1;
    occ_open_here = false;
}
/// A parallel replay's segment begins or ends on this thread (record_segment): the segment's counting starts afresh (the
/// query of its pass ended first)
void occ_segment_edge(VkCommandBuffer cmd) {
    occ::Counter& c = occ_ctr();
    occ_close(c, cmd);
    c.segment();
}
/// A command buffer replayed in parallel (replay_handle, before its dry pass; the replay thread, replay_mu): the replay's
/// counting given up, its query ended in cmd (the segments record what follows, a small one in cmd's open pass): a report
/// after it is measured only from a reset of its own
void occ_serial_give_up(VkCommandBuffer cmd) {
    occ::Counter& c = occ_ctr();
    occ_close(c, cmd);
    c.segment();
}
/// NOP_RESET_COUNTER of SAMPLES_PASSED, on the thread recording cmd (not the dry pass)
void occ_reset(VkCommandBuffer cmd) {
    occ::Counter& c = occ_ctr();
    occ_close(c, cmd);
    int slot;
    const uint64_t serial = occ_serial_of(cmd, &slot);
    c.reset(slot, serial, slot >= 0 ? occ_slots[slot].used : 0);
}
/// NOP_REPORT_COUNTER of SAMPLES_PASSED, on the thread recording cmd (not the dry pass): what the queries since the reset
/// counted, when whole, goes with the recording, its count to be written once the recording is done. pos: the report's
/// place in the stream (occ_pos); writable false: its count word is a clear flag (not written, but the counting goes on)
void occ_report_measure(VkCommandBuffer cmd, uint64_t addr, uint64_t pos, bool writable) {
    occ::Counter& c = occ_ctr();
    occ_close(c, cmd);
    int slot;
    const uint64_t serial = occ_serial_of(cmd, &slot);
    occ::Rep rep;
    bool keep_now = false;
    const occ::Counter::Outcome o = c.report(occ_keep.load(std::memory_order_relaxed), serial,
                                             c.slot >= 0 && c.slot == slot ? occ_slots[c.slot].used : 0, &rep, &keep_now);
    if (keep_now && !occ_keep.exchange(true))
        logf("vk: occlusion queries: samples passed reported twice without a reset: counting goes on past reports from now on");
    if (o == occ::Counter::kMeasured && writable) {   // (its slot is the recording being made here: not sealed yet)
        rep.addr = addr, rep.pos = pos, rep.present = occ_presents.load(std::memory_order_relaxed);
        occ_slots[c.slot].reps.push_back(rep);
    }
    std::lock_guard lk(occ_mu);
    if (o != occ::Counter::kMeasured) occ_writers.settle(addr, pos);   // (no count comes: a condition reads memory, as before)
    switch (o) {
    case occ::Counter::kMeasured: occ_st.measured++; break;
    case occ::Counter::kNoReset: occ_st.no_reset++; break;
    case occ::Counter::kDouble: occ_st.doubles++; break;
    case occ::Counter::kIncomplete:
        occ_st.incomplete++;
        (c.why == occ::Counter::kOtherCmd ? occ_st.inc_other_cmd : c.why == occ::Counter::kFull ? occ_st.inc_full : occ_st.inc_skipped)++;
        break;
    }
}
/// (occ_mu held, the placeholder's write right after it) A samples-passed report written at the replay, in stream order (dry:
/// by a parallel replay's dry pass; flag: its count word a clear flag nvn_native.c wrote, left alone): the newest report at
/// its address from now on, and whether its count is to be written there once read back. value: what the replay writes in
/// the count word (the placeholder: a count goes in only over it). (lab) Without RX_VK_OCC_QUERY=1, only for a report
/// placed in a phase of the knob occq: the queries run in every phase, a flip at any frame is safe
void occ_placed(uint64_t addr, uint64_t pos, bool dry, bool flag, uint64_t value) {
    occ_st.reports++;
    occ_st.in_segments += dry ? 1 : 0;
    occ_distinct.note(addr);
    occ_sites.reported(addr);
    bool write = !flag;
    if (flag) occ_st.flagged++;
    else if (occ::listed(occ_skip, addr)) occ_st.skipped++, write = false;
    else if (!occ_env_real && !lab_ab::on("occq")) occ_st.fixed++, write = false;
    occ_writers.placed(addr, pos, occ_presents.load(std::memory_order_relaxed), write, value);
}
/// A record rewritten in the stream (the replay thread, in stream order: the serial replay or the dry pass): a report of
/// another counter type at addr, or a small buffer clear of [addr, addr + size) the replay applies on the CPU. The
/// samples-passed reports there are no longer the newest: their counts are not written over it, and a condition reads
/// memory, as before (NX2navi vk_exec_xfer.c: "the last report to addr decides", clear_small). occ_mu held, the record's
/// write with it: no count lands after it
void occ_rewritten(uint64_t addr, uint64_t size, bool report) {
    occ_st.rewritten += report ? (occ_writers.erase(addr) ? 1 : 0) : occ_writers.cleared(addr, addr + size);
}
/// A render condition over the words at addr and addr + 16 (the replay thread, in stream order with the placements: the
/// serial replay or the dry pass, whose answers the segments take; not one snapped at record, HOS_COND_RECORD's test).
/// *enabled: the replay's answer from memory, as before; it renders instead when a word's newest report is a samples-passed
/// one whose count is still to come (unknown, NX2navi's cond_enabled), and stays the answer from memory when a word's
/// count was written (a 0 skips) or none comes (occ::cond_decide). Only in RX_VK_OCC_QUERY=1 or the lab knob's phases;
/// RX_VK_OCC_COND=0: such a condition always renders. x, y: the words, read again under occ_mu when a count was written
void occ_cond(const NvnState& s, uint64_t addr, int mode, uint64_t& x, uint64_t& y, bool* enabled) {
    occ_cond_off = nullptr;
    std::lock_guard lk(occ_mu);
    const int64_t now = occ_presents.load(std::memory_order_relaxed);
    const occ::Writers::Src sx = occ_writers.word(addr, now), sy = occ_writers.word(addr + 16, now);
    if (sx == occ::Writers::kNone && sy == occ::Writers::kNone) return;   // (no samples-passed report: as before)
    const bool active = occ_env_real || lab_ab::on("occq");
    if (active && (sx == occ::Writers::kWrittenSrc || sy == occ::Writers::kWrittenSrc)) {
        // (read again under occ_mu: a count written between the replay's read and here is what memory holds now)
        host->gpu_read(addr, &x, 8);
        host->gpu_read(addr + 16, &y, 8);
        *enabled = mode == 0 ? x == y : x != y;
    }
    const occ::Cond c = occ::cond_decide(*enabled, sx, sy, active, occ_cond_on);
    occ_st.cond(c.kind);
    *enabled = c.enabled;
    if (c.kind == occ::Cond::kWrittenOff) occ_cond_off = &s;
    static int64_t seen = 0, logged = 0;
    if (occ_log_cond && seen++ % 499 == 0 && logged < 300) {
        logged++;
        logf("vk: occlusion condition at %#llx mode %d: %llu (%s) vs %llu (%s): %s (present %lld)", (unsigned long long)addr,
             mode, (unsigned long long)x, occ::src_name(sx), (unsigned long long)y, occ::src_name(sy),
             c.kind == occ::Cond::kUnknown ? "renders, a count pending"
             : c.kind == occ::Cond::kForced ? "renders (RX_VK_OCC_COND=0)"
             : c.kind == occ::Cond::kBase ? (c.enabled ? "on, as before (knob occq off)" : "off, as before (knob occq off)")
             : c.kind == occ::Cond::kWrittenOff ? "SKIPS, from a written count"
             : c.kind == occ::Cond::kWrittenOn ? "renders, from a written count"
             : (c.enabled ? "on, from memory" : "off, from memory"),
             (long long)now);
    }
}
/// The draw, dispatch or clear the replay thread passes with rendering off (the serial replay or the dry pass): one a
/// written count's condition skips, counted for the line
void occ_skip_note(const NvnState& s, bool clear) {
    if (s.render_enabled || occ_cond_off != &s) return;
    (clear ? occ_skips_clear : occ_skips_draw).fetch_add(1, std::memory_order_relaxed);
}
/// Once a present: the sealed recordings done read; Writers' addresses not reported for occ::kKeepAge presents pruned every
/// 60, and the line every 600 presents (and the first 64 report addresses at its 1st and every 10th)
void occ_present_tick() {
    static int64_t n = 0, lines = 0;
    occ_presents.store(presents_now, std::memory_order_relaxed);
    occ_harvest_done(occ::Stats::kAtPresent);
    if (++n % 60) return;
    std::lock_guard lk(occ_mu);
    occ_writers.prune(presents_now, occ::kKeepAge);
    if (n % 600) return;
    occ_st.addrs = occ_distinct.take();
    occ_st.draws_skipped = occ_skips_draw.exchange(0, std::memory_order_relaxed);
    occ_st.clears_skipped = occ_skips_clear.exchange(0, std::memory_order_relaxed);
    char line[2048];
    occ_st.line(line, sizeof line, 600, occ_writers.size());
    // (the log's lines hold about 1000 characters: the line in two, at its render conditions)
    if (char* cut = strstr(line, " Render conditions")) {
        *cut = 0;
        logf("%s", line);
        logf("vk: occlusion queries (RX_VK_OCC_QUERY), 600 presents (2): %s", cut + 1);
    } else {
        logf("%s", line);
    }
    if (lines++ % 10 == 0) occ_sites.log([](const char* l) { logf("%s", l); });
    occ_st = occ::Stats{};
}
} // namespace
bool occ_live = false;   // (vk_draw.h: RX_VK_OCC_QUERY's queries run)
void occ_draw_enter() { occ_ctr().draw_enter(occ_keep.load(std::memory_order_relaxed)); }
void occ_draw_issued() { occ_ctr().draw_issued(occ_keep.load(std::memory_order_relaxed)); }
void occ_draw_begin(VkCommandBuffer cmd) {
    occ::Counter& c = occ_ctr();
    const bool keep = occ_keep.load(std::memory_order_relaxed);
    if (!c.may_begin(keep)) return;
    int slot;
    if (!c.begin(keep, occ_serial_of(cmd, &slot))) return;
    OccSlot& sl = occ_slots[c.slot];
    if (sl.used >= kOccPerSlot) return c.full();
    c.open = (int)sl.used++;
    vkCmdBeginQuery(cmd, occ_pool, (uint32_t)c.slot * kOccPerSlot + (uint32_t)c.open, occ_flags);
    occ_open_here = true;
}
void occ_pass_end(VkCommandBuffer cmd) {   // (a pass of this thread's: only where this thread began the query open now. The
    if (occ_open_here) occ_close(occ_ctr(), cmd);   // replay's counting is under replay_mu, which a pass's end may not hold)
}
namespace {
/// The next recording slot, begun, for the submission that will signal value
Recording& begin_recording(uint64_t value) {
    static const bool occ_on = (occ_live = occ_init());   // (Triforce: RX_VK_OCC_QUERY, once; off: nothing below runs)
    (void)occ_on;
    Recording& r = recordings[recording_index];
    recording_index = (recording_index + 1) % kRecordings;
    if (r.value) {
        PROF(PROF_SLOTWAIT);
        const auto t0 = std::chrono::steady_clock::now();
        recording_wait(r);
        st_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        gpu_time_collect(r);
        gprof_collect(r.slot);
    }
    if (occ_live) occ_before_begin(r);   // (Triforce: its counts read and written, RX_VK_OCC_QUERY)
    r.segment = false; r.seg_unbegun = false;
    vkResetCommandPool(dev.device, r.pool, 0);
    vkResetCommandPool(dev.device, r.upload_pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(r.cmd, &bi);
    vkBeginCommandBuffer(r.upload, &bi);
    if (occ_live) occ_slot_begin(r);   // (Triforce: its block of occlusion queries reset in it)
    gpu_time_begin(r);
    gprof_begin_recording(r.slot, r.cmd, r.upload);
    checkpoint_mark(r.cmd, value, 'r');   // ((Triforce) RX_VK_CHECKPOINTS: a recording's start, its end below: checkpoint_kinds.h)
    r.value = value;
    memory_recording_value(value, completed());
    texture_begin_recording(r.upload, value);
    draw_begin_recording(r.slot, r.cmd, value);
    return r;
}
/// (Triforce, lab, RX_VK_FT) a thread that says when the GPU has finished each part of the traced frames: the timeline of
/// the recordings looked at in a tight loop while the trace is on (one core for those frames)
void ft_gpu_watch_start() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            uint64_t last = 0, lastv = 0;
            auto quiet = std::chrono::steady_clock::now();
            for (;;) {
                uint64_t s = 0, v = 0;
                if (rec_timeline) vkGetSemaphoreCounterValue(dev.device, rec_timeline, &s);
                vkGetSemaphoreCounterValue(dev.device, timeline, &v);
                if (s != last || v != lastv) {
                    vk_ft("gpu done seq %llu value %llu", (unsigned long long)s, (unsigned long long)v);
                    last = s, lastv = v;
                }
                if (vk_ft_on()) quiet = std::chrono::steady_clock::now();
                else if (std::chrono::steady_clock::now() - quiet > std::chrono::milliseconds(60)) break;
                for (int i = 0; i < 40; i++) _mm_pause();
            }
        }).detach();
    });
}
bool tail_early_record(Recording& r);   // ((Triforce) the early tail, below maybe_split)
/// part: ((Triforce) a part of the recording, which goes on in another (maybe_split, tail_begin, a parallel replay's setup): the
/// submissions finished in it are known to a wait for a write-back only at the submit that ends it, wb_rec_wait.h Finished)
void submit_recording(Recording& r, bool signal_value = true, uint64_t signal_at = 0, bool part = false) {
    // ((Triforce) RX_VK_EVENT_BARRIER: an event's write still held goes in before the recording ends. Every end the replay makes
    // writes them before its write-backs (event_writes_flush): this is in case another comes, which the log names)
    if (event_writes_held(r.cmd)) {
        static int told;
        if (told++ < 4) logf("vk: an event's write was still held at a recording's end: written there, after its write-backs");
        end_pass(r.cmd), full_barrier(r.cmd, "barrier (an event's held write, at its recording's end)");
    }
    draw_flush_recording(r.slot);
    bool uploads = texture_uploads_recorded();
    // ((Triforce) the early tail: a tail's event words after its uploads and ahead of its commands, in its upload command buffer,
    // which then goes to the queue)
    if (tail_early_record(r)) uploads = true;
    split_part_submitted(r.cmd, uploads);   // ((Triforce) RX_VK_SPLIT_BARRIER's count)
    gprof_submitted(r.slot, uploads);
    checkpoint_mark(r.cmd, r.value, 'z');   // ((Triforce) RX_VK_CHECKPOINTS: all of the recording before it, its write-backs too)
    gpu_time_end(r, uploads);
    vkEndCommandBuffer(r.upload);
    vkEndCommandBuffer(r.cmd);
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = timeline;
    signal.value = signal_at ? signal_at : r.value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb[2]{{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
    uint32_t n = 0;
    if (uploads) cb[n++].commandBuffer = r.upload;
    cb[n++].commandBuffer = r.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = n;
    si.pCommandBufferInfos = cb;
    VkSemaphoreSubmitInfo signals[2] = {signal, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    uint32_t nsig = signal_value ? 1 : 0;
    uint64_t seq = 0;
    if (rec_timeline) {   // (and its number on the recordings' timeline; every caller holds replay_mu: in order)
        seq = ++rec_seq;
        signals[nsig].semaphore = rec_timeline;
        signals[nsig].value = seq;
        signals[nsig].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        nsig++;
        std::lock_guard mk(rec_map_mu);
        // (upstream #2419, behind RX_VK_REC_LAST: a submission recorded in parts maps its value to its LAST part. emplace keeps
        // the first, and a wait for the submission through wait_recorded comes back after its first ~120 draws)
        const bool last_part = rb_fix(0);
        const auto finished_in = [&](uint64_t v) {
            auto it = value_rec.find(v);
            if (it == value_rec.end()) value_rec.emplace(v, seq);
            else if (last_part && it->second < seq) it->second = seq;
        };
        for (uint64_t v : rec_open_values) finished_in(v);
        fin_rec.submitted(rec_open_values, seq, !part);   // ((Triforce, wp20) a part's: noted at the recording's end)
        if (r.value) finished_in(r.value);
        rec_open_values.clear();
        // (RX_VK_FENCE_POLL_FAST: after value_rec has them, under rec_map_mu, so a poll that sees the mark finds the recording)
        for (uint64_t v : rec_open_qvalues) whole_val[v & (kWholeSlots - 1)].store(v, std::memory_order_release);
        rec_open_qvalues.clear();
        while (value_rec.size() > 8192) value_rec.erase(value_rec.begin());
    }
    si.signalSemaphoreInfoCount = nsig;
    si.pSignalSemaphoreInfos = nsig ? signals : nullptr;
    // (Triforce, RX_VK_OCC_QUERY: once on the queue, its occlusion queries are the readers', by seq. Declared before the lock
    // below, so run after it is let go: occ_mu is never taken with dev.present_mu held)
    struct OccSeal {
        Recording& rec;
        uint64_t at;
        ~OccSeal() {
            if (occ_live && rec.submitted) occ_seal(rec, at);
        }
    } occ_sealed{r, seq};
    std::lock_guard lk(dev.present_mu);   // (one queue: submits and presents take turns)
    if (batch_on()) {
        // ((Triforce) RX_VK_BATCH=parts: a part (no value of its own to signal) is not held; it goes with the ones held before it)
        const barrier_rules::Batch way = batch_way();
        const bool hold = barrier_rules::batch_hold(way, !signal_value, submit_depth > 0);
        if (hold || !pend.cbs.empty()) {
            if (pend.cbs.empty()) batch_next++, pend_since = std::chrono::steady_clock::now();
            for (uint32_t i = 0; i < n; i++) pend.cbs.push_back(cb[i]);
            if (signal_value) pend.signal = std::max(pend.signal, signal.value), pend.any_signal = true;
            if (seq) pend.rec = std::max(pend.rec, seq);
            pend.recs++;
            r.batch = batch_next;
            r.submitted = true;
            // (RX_VK_BATCH_MAX=<command buffers>: held at most this many, default 32; RX_VK_BATCH=parts: recordings, default 4)
            static const size_t cap = getenv("RX_VK_BATCH_MAX") ? (size_t)std::max(1, atoi(getenv("RX_VK_BATCH_MAX")))
                                                                : way == barrier_rules::Batch::Parts ? 4 : 32;
            if (barrier_rules::batch_flush(way, hold, way == barrier_rules::Batch::Parts ? pend.recs : pend.cbs.size(), cap))
                flush_reason = hold ? FR_CAP : FR_OTHER, queue_flush_locked();
            memory_lazy_submitted();   // (Triforce, lazy write-back: on the queue as far as a payment is concerned: it flushes first)
            return;
        }
    }
    PROF(PROF_QSUBMIT);
    VkSemaphoreSubmitInfo bind_wait;   // (Triforce: after the memory of the mirrors' new pieces is bound, vk_memory.cpp)
    if (memory_mirror_bind_wait(&bind_wait)) si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &bind_wait;
    late_fix_flush_locked();   // ((Triforce) RX_VK_LATE_QUEUE: the late pages' fixes before anything goes to the queue)
    const bool bg_import = memory_bg_importing() > 0;   // ((Triforce) the stall line's: a submit beside a background import)
    const auto qs0 = bg_import ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const VkResult res = vkQueueSubmit2(dev.queue, 1, &si, r.fence);
    if (vk_ft_on()) {
        ft_gpu_watch_start();
        vk_ft("part sent seq %llu value %llu%s%s", (unsigned long long)seq, (unsigned long long)signal.value, signal_value ? " (signals it)" : "",
              uploads ? " +uploads" : "");
    }
    if (bg_import || memory_bg_importing() > 0) {
        stall_ct.qs_bg.fetch_add(1, std::memory_order_relaxed);
        if (bg_import)
            stall_ct.qs_bg_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - qs0).count(),
                                        std::memory_order_relaxed);
    }
    r.submitted = res == VK_SUCCESS;
    if (r.submitted && signal_value) note_queue_top(signal.value);   // (Triforce: the stall line's)
    static int failures;
    if (res != VK_SUCCESS && failures++ < 8) logf("vk: submit %llu failed (%d)", (unsigned long long)r.value, (int)res);
    if (res == VK_ERROR_DEVICE_LOST) device_lost();
    memory_lazy_submitted();   // (Triforce, lazy write-back: what was paid before this submit gets its rest; nothing with it off)
}

// ---- NVN state (the draw path's, vk_state.h) ----
NvnState state;
// (each of the game's queues has one of its own, as the console's channels do: their command buffers are replayed in
// turns at their event waits (submit_job), and one's draws went on with what another's had set. A queue seen first
// starts from the state the replay was at, as all of them did before. The queue-less replays (RX_VK_ASYNC=0, the
// bench) keep the one above. RX_VK_QUEUE_STATE=0: one for all of them, as before.)
std::unordered_map<uint64_t, NvnState> qstates;
NvnState* cur_state = &state;
bool queue_state_on() {
    static const bool v = !getenv("RX_VK_QUEUE_STATE") || atoi(getenv("RX_VK_QUEUE_STATE")) != 0;
    return v;
}
/// The state the replay goes on with: its queue's. (The draw path's caches made afresh: they are keyed on the state's
/// counters, which the queues' states share no order of.)
void use_queue_state(uint64_t queue) {
    NvnState* s = &state;
    if (queue && queue_state_on()) {
        auto it = qstates.find(queue);
        if (it == qstates.end()) it = qstates.emplace(queue, *cur_state).first;
        s = &it->second;
    }
    if (s == cur_state) return;
    cur_state = s;
    static uint64_t gen_base = 0;
    gen_base += 1ull << 32;
    s->bind_gen = gen_base, s->rt_serial = gen_base;
    for (int k = 0; k < kNvnStages; k++) s->tex_gen[k] = gen_base, s->ssbo_gen[k] = gen_base;
}

/// (hookshot) payload's copy of a payload shorter than its type, rare: a function of its own, so that the compiler can't merge it
/// with the copy at the type's size into one call of memcpy with a chosen size (clang 23 does, at -O2)
[[gnu::noinline, gnu::cold]] void payload_short(void* v, const uint8_t* a, uint32_t bytes) { std::memcpy(v, a, bytes); }

template <class T>
T payload(const uint8_t* a, uint32_t bytes) {
    // (hookshot: a payload at least as long as T, nearly every one, is copied at T's size: a copy the compiler makes inline. A
    // variable size was a call of the C library's memcpy for every op replayed, a few percent of the Steam Deck's render thread,
    // where it shows as __memmove_avx_unaligned_erms)
    T v{};
    if (bytes >= sizeof v) [[likely]]
        std::memcpy(&v, a, sizeof v);
    else
        payload_short(&v, a, bytes);
    return v;
}

int64_t st_barriers, st_ub_updates, st_ub_overlays, st_submits;
std::map<int32_t, int64_t> st_barrier_flags;   // (the game's nvnCommandBufferBarrier bits: how many of each combination, the frame log)
std::mutex st_barrier_flags_mu;   // (a parallel replay's threads count their barriers too)
// ---- events in replay order: the values the stream's signals and the CPU's set, for the GPU's waits on them. The
// game's three queues order their work with events (a command buffer waits for another queue's to reach a stage);
// replayed on one queue in the order they were submitted, a waiter submitted before its signaller ran first ----
std::unordered_map<uint64_t, uint32_t> ev_values;
// (hookshot) The CPU's signals, while a replay holds replay_mu: the main thread stopped for it in nvnEventSignal, ~1.7 ms
// a frame of the critical path. They wait here for the replay's next look at the events (ev_cpu_apply).
std::mutex ev_cpu_mu;
std::vector<std::pair<uint64_t, uint32_t>> ev_cpu_pending;
std::atomic<bool> ev_cpu_any{false};
std::unordered_set<uint64_t> cpu_events;   // (events the CPU has signalled, nvnEventSignal; ev_cpu_mu)
void ev_cpu_apply() {
    if (!ev_cpu_any.load(std::memory_order_acquire)) return;
    std::lock_guard lk(ev_cpu_mu);
    for (const auto& e : ev_cpu_pending) ev_values[e.first] = e.second, cpu_events.insert(e.first);
    ev_cpu_pending.clear();
    ev_cpu_any.store(false, std::memory_order_release);
}
int64_t st_waits_met, st_waits_unmet;
bool event_wait_met(uint32_t have, uint32_t mode, uint32_t want) {
    return mode == 0 ? have == want : (int32_t)(have - want) >= 0;   // (NVN: EQUAL, GEQUAL_WRAP)
}
using EvMap = std::unordered_map<uint64_t, uint32_t>;
uint32_t event_value_in(const EvMap& ev, uint64_t gpu) {   // (one the stream never set: as the CPU has it)
    auto it = ev.find(gpu);
    if (it != ev.end()) return it->second;
    uint32_t v = 0;
    host->gpu_read(gpu, &v, 4);
    return v;
}
thread_local bool suspend_ok = false;               // (a serial replay that stops at a wait not met: submit_job)
thread_local const uint8_t* suspend_at = nullptr;   // (the wait it stopped at)
uint64_t ts_anchor_ns = 0;   // (the last present's time: counter timestamps are scaled from it)
// (Triforce, 11-17) The game's dynamic resolution (Resolution Auto) measures a frame's GPU time as the difference of two timestamp
// reports, which the replay writes as it passes them, scaled by RX_VK_TS_SCALE (0.1) from the last present: 0.1 x the replay's
// wall time, under 1 ms where its threshold is ~16.4 ms, so Auto stayed at its top level (1536x864) on every card. Now the
// scale makes that difference the card's real time: the frames' measured GPU time (the recordings' timestamps, st_gpu_ns +
// st_gpu_up_ns) over the replay's wall time between the frame's first and last timestamp reports, both averaged over ~30
// presents. A card that cannot draw the frame in ~16.4 ms then gets the 3D stepped down, as on the console. Only the game's
// own controller reads these reports, and it runs only on Auto. RX_VK_DYNRES_GPU=0 (or RX_VK_TS_SCALE set): the fixed scale
uint64_t ts_first_ns = 0, ts_last_ns = 0;   // (this frame's first and last timestamp report, real clock)
double ts_ema_gpu_ns = 0, ts_ema_span_ns = 0, ts_dyn_scale = 0.1;
int64_t ts_gpu_last = 0;
bool ts_dyn_on() {
    static const bool v = !getenv("RX_VK_TS_SCALE") && !(getenv("RX_VK_DYNRES_GPU") && atoi(getenv("RX_VK_DYNRES_GPU")) == 0);
    return v;
}
void ts_dyn_present() {   // (once a present, before the anchor moves)
    if (!ts_dyn_on()) return;
    const int64_t g = st_gpu_ns + st_gpu_up_ns;
    const double dg = (double)(g - ts_gpu_last);
    ts_gpu_last = g;
    const double span = ts_last_ns > ts_first_ns ? (double)(ts_last_ns - ts_first_ns) : 0.0;
    ts_first_ns = ts_last_ns = 0;
    if (dg <= 0 || dg > 200e6 || span <= 0) return;   // (no GPU time read this present, or no frame of reports)
    const double k = 1.0 / 30.0;
    ts_ema_gpu_ns = ts_ema_gpu_ns ? ts_ema_gpu_ns + k * (dg - ts_ema_gpu_ns) : dg;
    ts_ema_span_ns = ts_ema_span_ns ? ts_ema_span_ns + k * (span - ts_ema_span_ns) : span;
    ts_dyn_scale = std::clamp(ts_ema_gpu_ns / ts_ema_span_ns, 0.02, 8.0);
    static int64_t n = 0;
    if (++n % 1200 == 0)
        logf("vk: dynamic resolution's clock: the frame's GPU time %.2f ms, the replay's span between its timestamp reports %.2f ms: "
             "scale %.3f (RX_VK_DYNRES_GPU=0: 0.1)", ts_ema_gpu_ns / 1e6, ts_ema_span_ns / 1e6, ts_dyn_scale);
}

/// A GPU-timeline write of guest memory (the stream's own writes: they reach memory in command order)
void gpu_write(VkCommandBuffer cmd, uint64_t gpu, const void* data, uint64_t n) {
    const uint8_t* d = (const uint8_t*)data;
    while (n) {   // (vkCmdUpdateBuffer: up to 64 KB, 4-byte aligned)
        const uint64_t k = std::min<uint64_t>(n, 65536);
        BufferRef b = gpu_buffer_bind(gpu, k, true);
        if (!b || (b.offset & 3) || (k & 3)) {   // (unaligned or outside the pools: on the CPU, as before)
            guest_cpu_write(gpu, d, k);
        } else {
            vkCmdUpdateBuffer(cmd, b.buffer, b.offset, k, d);
        }
        gpu += k, d += k, n -= k;
    }
}

// ---- (Triforce) RX_VK_EVENT_BARRIER (barrier_rules.h): the stream's event signals whose words wait for the next full barrier on
// their command buffer (vk_texture.cpp full_barrier records them right after it), in the order of the stream (a barrier of the
// transfers between each two: event_writes_record). Each word's buffer is bound at its
// signal, as the write was (memory_bind_event_write, gpu_buffer_bind: the page goes on the recording's write-backs then), so that
// recording them takes no lock: a full barrier may be recorded under the pools' lock ----
struct HeldWrite { uint64_t gpu; uint32_t v; VkBuffer buffer; VkDeviceSize offset; };
constexpr int kHeldWrites = 16;
thread_local HeldWrite held_writes[kHeldWrites];
thread_local int held_n = 0;
const HeldWrite* held_write(uint64_t gpu) {   // (the last held for that word)
    for (int i = held_n - 1; i >= 0; i--)
        if (held_writes[i].gpu == gpu) return &held_writes[i];
    return nullptr;
}
bool event_late_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_EVENT_BARRIER");
        const bool every = getenv("RX_VK_BARRIER_EVERY_DRAW") != nullptr;
        const bool game = pass_barrier::game(getenv("RX_VK_PASS_BARRIER"), dev.props.vendorID, every);
        const bool v = barrier_rules::event_late(e, game);
        logf("vk: event signals: %s (RX_VK_EVENT_BARRIER%s%s; =full: a full barrier and the write at each signal, =late: on any card)",
             v ? "the word written right after the next full barrier on its command buffer, the pass going on"
               : "a full barrier and the word's write at each signal",
             e && *e ? "=" : !game ? ", the pass barriers' game mode off" : ", with the pass barriers' game mode", e && *e ? e : "");
        return v;
    }();
    return on;
}
} // namespace
void event_writes_record(VkCommandBuffer cmd) {
    if (!held_n) return;
    GprofSpan gp(cmd, "event signal (its write, after a full barrier)");
    gp.items = held_n;
    for (int i = 0; i < held_n; i++) {
        if (i) {   // (one after another, as the signals were: the game sees them done in their order)
            VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1, d.pMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &d);
        }
        vkCmdUpdateBuffer(cmd, held_writes[i].buffer, held_writes[i].offset, 4, &held_writes[i].v);
    }
    held_n = 0;
}
namespace {

// ---- uniform updates (UPDATE_UB): written at the end of their recording; ub_overlay applies them to copies ----
struct UbUpdate {
    uint64_t value;   // the submission that writes it
    uint64_t dst;
    std::vector<uint8_t> bytes;
};
std::deque<UbUpdate> ub_updates;   // in stream order, oldest first
std::atomic<uint64_t> ub_list_epoch{1};   // (bumped when that deque gains or loses an update)
thread_local uint64_t ub_gen = 0;   // (each recording thread's: its draws' overlays change with what it passed)

// ---- parallel replay (RX_VK_PARALLEL=<threads>): MKW hands over nearly a frame's draws in one command buffer with
// its present, and waits for the last frame's GPU work before it submits the next: the replay of that command buffer
// (6-10 ms) was on the frame's critical path. A pass over its ops on the replay thread keeps only the state, and
// what has to happen in stream order (counter reports and the render conditions that read them, uniform updates);
// segments of it are then recorded on several threads from the state there, each into a command buffer of its own,
// submitted in order. ----
enum class Mode { Serial, Dry, Worker };
thread_local Mode rmode = Mode::Serial;
// ---- RX_VK_SEG_EARLY: segments handed out while the dry pass still runs (replay_handle_early). The dry pass appends to ub_updates
// and par_conds while the helpers read them (a deque's map and a vector's buffer may move as they grow). While ub_growing, the
// replay thread's appends hold ub_grow_mu exclusively and the helpers' reads (ub_overlay, ub_read, flush_ub_updates, a render
// condition) hold it shared; a helper reads only entries appended before its segment was handed out (its end was passed), the
// lock is for the containers. An append waiting for the lock holds back reads not begun yet (ub_grow_waiting): four or more
// helpers read the list at every draw, and a lock that lets new readers in while a writer waits kept an append waiting ----
std::shared_mutex ub_grow_mu;
std::atomic<bool> ub_growing{false};
std::atomic<bool> ub_grow_waiting{false};
thread_local int ub_read_depth = 0;   // (a read inside a read holds the lock once)
struct UbReadGuard {
    std::shared_lock<std::shared_mutex> lk;
    UbReadGuard() {
        if (ub_read_depth++ == 0 && rmode == Mode::Worker && ub_growing.load(std::memory_order_acquire)) {
            for (int spin = 0; ub_grow_waiting.load(std::memory_order_acquire); spin++)
                if (spin >= 64) std::this_thread::yield();
            lk = std::shared_lock<std::shared_mutex>(ub_grow_mu);
        }
    }
    ~UbReadGuard() { ub_read_depth--; }
    UbReadGuard(const UbReadGuard&) = delete;
    UbReadGuard& operator=(const UbReadGuard&) = delete;
};
struct UbGrowGuard {
    std::unique_lock<std::shared_mutex> lk;
    UbGrowGuard() {
        if (!ub_growing.load(std::memory_order_relaxed)) return;
        ub_grow_waiting.store(true, std::memory_order_release);
        lk = std::unique_lock<std::shared_mutex>(ub_grow_mu);
        ub_grow_waiting.store(false, std::memory_order_release);
    }
};
// (Triforce, RX_VK_OCC_QUERY: "occlusion queries" above) The samples-passed counting of the replay, one per NVN queue's state
// (replay_mu: whichever thread holds it replays the main stream), and of a parallel replay's segment, the recording thread's
// own (a helper thread's, or the replay thread's while it records a segment: rmode Worker)
std::unordered_map<const NvnState*, occ::Counter> occ_mains;
thread_local occ::Counter occ_seg;
occ::Counter& occ_ctr() {
    if (rmode == Mode::Worker) return occ_seg;
    static const NvnState* at = nullptr;   // (the last queue's, kept: its node does not move)
    static occ::Counter* c = nullptr;
    if (at != cur_state || !c) at = cur_state, c = &occ_mains[cur_state];
    return *c;
}
thread_local size_t ub_limit = SIZE_MAX;   // (a worker: the uniform updates before its place in the stream)
thread_local size_t cond_at = 0;           // (a worker: the next render condition's result)
std::vector<uint8_t> par_conds;            // (the render conditions' results, in stream order: the dry pass's)
// (RX_VK_SEG_EARLY) how many of par_conds are written, for the helpers' reads: the dry pass appends while early segments read, and a
// helper reads this count, never the vector's own size; par_conds_reset keeps room so an append never moves its buffer in play
std::atomic<size_t> par_conds_n{0};
constexpr size_t kCondRoom = 8192;
void par_conds_reset() {
    par_conds.clear();
    if (par_conds.capacity() < kCondRoom) par_conds.reserve(kCondRoom);
    par_conds_n.store(0, std::memory_order_release);
}
struct ParPoint {
    const uint8_t* p;
    NvnState s;
    size_t ub, cond;
    int64_t work;
};
std::vector<ParPoint> par_points;   // (every 64 draws and dispatches of the dry pass; RX_VK_PARALLEL_POINT below)
/// ((Triforce) RX_VK_PARALLEL_POINT=<n>, 8-256: a point every n draws and dispatches instead of 64. With early segments
/// (RX_VK_SEG_EARLY) a segment goes out once the dry pass passes the first point after its planned end: finer points hand it out
/// sooner. The early path copies only the points it takes; the old path copies every one (~27 KB each))
int64_t par_point_every() {
    static const int64_t v = getenv("RX_VK_PARALLEL_POINT") ? std::clamp<int64_t>(atoi(getenv("RX_VK_PARALLEL_POINT")), 8, 256) : 64;
    return v;
}
// (RX_VK_SEG_EARLY) the dry pass calls on_point at each point it keeps while replay_handle_early runs (the replay thread's only);
// wants says whether on_point would take the point at this work count and place (else its state is not copied)
struct SegEarlyHook {
    std::function<void(const ParPoint&)> on_point;
    std::function<bool(int64_t, const uint8_t*)> wants;
};
SegEarlyHook* seg_early_hook = nullptr;
// (Triforce) A temporal upscaler's or a frame generation's hooks in the draws, in stream order (stream_order.h): the dry pass
// tells them (dry_tail_find) up to the first draw or dispatch that takes what they noted (par_tail: the engine's point, or the
// upscale's dispatch); from there the command buffer is recorded in order, as one segment, after the others
bool dry_tail_find = false;
bool par_tail_at = false;
ParPoint par_tail;
thread_local bool hooks_told = false;   // (the segment being recorded on this thread: its draws' hooks told by the dry pass)
// (each draw's and dispatch's place in the stream: the command buffer's replay, the draws and dispatches before it in it)
uint64_t stream_cbs = 0;                // (the replay thread's: one more for each command buffer replayed)
thread_local uint64_t stream_cb = 0;
thread_local int64_t stream_work = 0;
// ((Triforce, 11-19) tex_warm.h: the draws and dispatches replayed since the last present, and those of the frame before it:
// a submission that follows a light frame makes and uploads the registered textures ahead, texture_warm_step)
std::atomic<int64_t> frame_work{0}, last_frame_work{0};
bool dry_clears_on() {   // (the dry pass marks a clear's targets too: NOP_CLEAR_COLOR in replay())
    static const bool v = getenv("RX_VK_NO_DRY_CLEARS") == nullptr;
    return v;
}
bool dry_copies_on() {   // (Triforce: and what copies and texture clears write: vk_xfer.cpp dry_copy_t2t. RX_VK_NO_DRY_COPIES=1: not)
    static const bool v = getenv("RX_VK_NO_DRY_COPIES") == nullptr;
    return v;
}
int64_t dry_work = 0;
// (Triforce, RX_VK_OCC_QUERY) A samples-passed report's place in the stream: the dry pass's (the command buffer it goes over,
// the draws and dispatches before it there) is what the segment's thread finds there too (record_segment: stream_cb, stream_work)
uint64_t occ_dry_cb = 0;
uint64_t occ_pos(bool dry) { return dry ? occ_dry_cb << 32 | (uint64_t)(dry_work & 0xffffffff) : replay_stream_pos(); }
size_t ub_first_of_recording = 0;  // (the current recording's first entry)
uint64_t rec_value_now = 0;

bool deferred_late() {   // (RX_VK_DEFERRED_LATE=1: deferred binds read from the game's arrays at the replay)
    static const bool v = getenv("RX_VK_DEFERRED_LATE") != nullptr;
    return v;
}

void ub_prune(uint64_t done) {
    if (ub_updates.empty() || ub_updates.front().value > done) return;
    while (!ub_updates.empty() && ub_updates.front().value <= done) {
        ub_updates.pop_front();
        if (ub_first_of_recording) ub_first_of_recording--;
    }
    ub_list_epoch.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

/// (Evil Hookshot) the GPU's time in all recordings so far, ns (the timestamps read back so far): vk_memory.cpp's mirror trials
int64_t gpu_time_total_ns() { return st_gpu_ns; }
/// (Triforce) and with the upload command buffers' own time (the 120-present line's two): the performance overlay's GPU busy
/// per frame (vk_menu.cpp perf_sample; NX2navi's gpu_ms). 0 while the device has no timestamps
int64_t gpu_time_busy_ns() { return st_gpu_ns + st_gpu_up_ns; }

uint64_t ub_generation() {
    return ub_gen;
}
bool replay_hooks_told() { return hooks_told; }
uint64_t replay_stream_pos() { return stream_cb << 32 | (uint64_t)(stream_work & 0xffffffff); }
void ub_thread_off() {
    ub_limit = 0;
}
/// The open gap around [addr, addr+bytes) that no update covers. False when one does. [lo, hi) is that gap.
bool ub_gap(const uint64_t* dst, const uint64_t* end, int n, uint64_t addr, uint64_t bytes, uint64_t* lo, uint64_t* hi) {
    if (!bytes || addr + bytes < addr) return false;
    uint64_t left = 0, right = ~uint64_t{0};
    const uint64_t stop = addr + bytes;
    for (int i = 0; i < n; i++) {
        if (dst[i] < stop && addr < end[i]) return false;
        if (end[i] <= addr && end[i] > left) left = end[i];
        if (dst[i] >= stop && dst[i] < right) right = dst[i];
    }
    *lo = left;
    *hi = right;
    return true;
}
struct UbGapCache {
    uint64_t epoch = 0, lo = 0, hi = 0;
    size_t first = 0, lim = 0;
    bool on = false;
};
thread_local UbGapCache ub_gap_cache;
bool ub_gap_holds(uint64_t addr, uint64_t n, size_t first, size_t lim) {
    UbReadGuard ub_guard;   // (RX_VK_SEG_EARLY)
    return n && ub_gap_cache.on && ub_gap_cache.epoch == ub_list_epoch.load(std::memory_order_relaxed) &&
           ub_gap_cache.first == first && ub_gap_cache.lim == lim && addr >= ub_gap_cache.lo &&
           addr + n >= addr && addr + n <= ub_gap_cache.hi;
}
bool ub_try_gap(size_t first, size_t lim, uint64_t addr, uint64_t n) {
    UbReadGuard ub_guard;   // (RX_VK_SEG_EARLY)
    if (first > lim) return false;
    const uint64_t epoch = ub_list_epoch.load(std::memory_order_relaxed);
    static thread_local std::vector<uint64_t> dst, end;
    dst.clear();
    end.clear();
    dst.reserve(lim - first);
    end.reserve(lim - first);
    for (size_t i = first; i < lim; i++) {
        dst.push_back(ub_updates[i].dst);
        end.push_back(ub_updates[i].dst + ub_updates[i].bytes.size());
    }
    uint64_t lo = 0, hi = 0;
    if (!ub_gap(dst.data(), end.data(), (int)dst.size(), addr, n, &lo, &hi)) return false;
    if (ub_list_epoch.load(std::memory_order_relaxed) != epoch) return false;   // (the list changed: do not keep the gap)
    ub_gap_cache = {epoch, lo, hi, first, lim, true};
    return true;
}
// (a draw reads the same uniform words as the draw before it. The gap is what says later updates missed the word:
// it is kept while that gap covers it and its page is unstamped)
struct UbWord {
    uint64_t addr = 0, rec = 0;
    WriteGen page;   // (its page's write generation then, 64 bits: write_gen.h)
    uint32_t v = 0;
    bool on = false;
};
thread_local UbWord ub_words[128];
bool ub_word_hit(uint64_t addr, size_t lim, uint32_t* out) {
    UbReadGuard ub_guard;   // (RX_VK_SEG_EARLY)
    UbWord& w = ub_words[(addr >> 2) & 127];
    if (!w.on || w.addr != addr || w.rec != rec_value_now || w.page != memory_page_stamp(addr)) return false;
    if (!ub_gap_holds(addr, 4, 0, lim)) return false;
    *out = w.v;
    return true;
}
// (a range an update does overlap: the stream copy stays while the gap's update list is unchanged and the copy is
// still in the recording's stream. A range in the gap has no copy)
struct UbCopy {
    const uint8_t* base = nullptr;
    uint64_t addr = 0, n = 0, at = 0, rec = 0, view = 0, epoch = 0;
    size_t lim = 0, first = 0;
    bool on = false;
};
thread_local UbCopy ub_copies[8];
bool ub_copy_hit(uint64_t addr, uint64_t n, const uint8_t* base, size_t first, size_t lim, uint64_t* at) {
    UbReadGuard ub_guard;   // (RX_VK_SEG_EARLY)
    UbCopy& c = ub_copies[((addr >> 4) ^ n) & 7];
    if (!c.on || c.base != base || c.addr != addr || c.n != n || c.first != first || c.lim != lim ||
        c.rec != rec_value_now || c.epoch != ub_list_epoch.load(std::memory_order_relaxed) ||
        c.view != memory_capture_view())
        return false;
    if (!stream_holds(base, c.at, n)) return false;
    *at = c.at;
    return true;
}
void ub_copy_keep(uint64_t addr, uint64_t n, const uint8_t* base, size_t first, size_t lim, uint64_t at,
                  uint64_t view, uint64_t epoch) {
    if (view != memory_capture_view()) return;
    if (epoch != ub_list_epoch.load(std::memory_order_relaxed)) return;
    UbCopy& c = ub_copies[((addr >> 4) ^ n) & 7];
    c = {};
    c.on = true, c.base = base, c.addr = addr, c.n = n, c.at = at, c.first = first, c.lim = lim;
    c.rec = rec_value_now, c.view = view, c.epoch = epoch;
}
uint64_t ub_overlay(uint64_t addr, uint64_t n, uint8_t* base, const std::function<uint64_t(uint64_t)>& alloc) {
    UbReadGuard ub_guard;   // (RX_VK_SEG_EARLY)
    // (a worker: those before its place in the stream; a background thread: none, not even the list's size)
    const size_t lim = ub_limit ? std::min(ub_limit, ub_updates.size()) : 0;
    static const bool cur_only = getenv("RX_VK_UB_CUR_ONLY") != nullptr;   // (A/B: this submission's updates only)
    const size_t first = cur_only ? std::min(ub_first_of_recording, lim) : 0;
    if (ub_gap_holds(addr, n, first, lim)) return UINT64_MAX;
    const uint64_t view = memory_capture_view();
    uint64_t kept = 0;
    if (ub_copy_hit(addr, n, base, first, lim, &kept)) return kept;
    if (ub_try_gap(first, lim, addr, n)) return UINT64_MAX;
    const uint64_t epoch = ub_list_epoch.load(std::memory_order_relaxed);
    bool any = false;
    for (size_t i = first; i < lim; i++) {
        const UbUpdate& u = ub_updates[i];
        if (u.dst < addr + n && addr < u.dst + u.bytes.size()) {
            any = true;
            break;
        }
    }
    if (!any) return UINT64_MAX;
    const uint64_t at = alloc(n);
    if (at == UINT64_MAX) return UINT64_MAX;
    uint8_t* p = base + at;
    if (!memory_read_as_submitted(addr, p, n)) std::memset(p, 0, n);
    for (size_t i = first; i < lim; i++) {
        const UbUpdate& u = ub_updates[i];
        const uint64_t lo = std::max(addr, u.dst), hi = std::min(addr + n, u.dst + u.bytes.size());
        if (lo < hi) std::memcpy(p + (lo - addr), u.bytes.data() + (lo - u.dst), hi - lo);
    }
    st_ub_overlays++;
    ub_copy_keep(addr, n, base, first, lim, at, view, epoch);
    return at;
}

/// Guest memory as the stream's place sees it: as it was at the submission (the capture), with the uniform updates
/// the stream made before this place (a worker: before its segment) over it. For what the replay reads on the CPU for
/// the GPU's sake (handles in uniform and storage buffers): the game's memory now may be its next frames'.
bool ub_read(uint64_t addr, void* dst, uint64_t n) {
    UbReadGuard ub_guard;   // (RX_VK_SEG_EARLY)
    const size_t lim = ub_limit ? std::min(ub_limit, ub_updates.size()) : 0;
    if (n == 4) {
        uint32_t v = 0;
        if (ub_word_hit(addr, lim, &v)) {
            std::memcpy(dst, &v, 4);
            return true;
        }
    }
    const WriteGen page = n == 4 ? memory_page_stamp(addr) : WriteGen{};
    if (!memory_read_as_submitted(addr, dst, n)) return false;
    // (most words are not inside an inline update: the scan that finds that is kept for the gap, and the next words
    // in the gap do not walk the list. A word kept from that gap is the capture's own bytes)
    if (ub_gap_holds(addr, n, 0, lim) || ub_try_gap(0, lim, addr, n)) {
        if (n == 4 && page == memory_page_stamp(addr)) {
            UbWord& w = ub_words[(addr >> 2) & 127];
            uint32_t v = 0;
            std::memcpy(&v, dst, 4);
            w = {};
            w.on = true, w.addr = addr, w.v = v, w.rec = rec_value_now, w.page = page;
        }
        return true;
    }
    for (size_t i = 0; i < lim; i++) {
        const UbUpdate& u = ub_updates[i];
        const uint64_t lo = std::max(addr, u.dst), hi = std::min(addr + n, u.dst + u.bytes.size());
        if (lo < hi) std::memcpy((uint8_t*)dst + (lo - addr), u.bytes.data() + (lo - u.dst), hi - lo);
    }
    return true;
}

namespace {

/// The recording's uniform updates into guest memory (after its draws: the ones that came after an update read
/// their overlay copies). ((Triforce) lim: those before that entry only, the early tail's cut in a parallel replay's recording
/// after its segments, whose dry pass listed the tail's updates already: tail_begin)
void flush_ub_updates(VkCommandBuffer cmd, size_t lim = SIZE_MAX) {
    UbReadGuard ub_guard;   // (RX_VK_SEG_EARLY)
    const size_t end = std::min(lim, ub_updates.size());
    if (ub_first_of_recording >= end) return;
    GprofSpan gp(cmd, "ubupd (the stream's uniform updates -> guest memory, with its barriers)");
    gp.items = (int)(end - ub_first_of_recording);
    full_barrier(cmd);
    for (size_t i = ub_first_of_recording; i < end; i++)
        gpu_write(cmd, ub_updates[i].dst, ub_updates[i].bytes.data(), ub_updates[i].bytes.size()), gp.bytes += ub_updates[i].bytes.size();
    full_barrier(cmd);
}

/// (mesh shading: the stages its resources are bound to, beyond NVN's six)
void log_stage_bind(uint32_t op, int stage) {
    static int logged[64];
    if (stage >= 0 && stage < 64 && logged[stage]++ < 3) logf("vk: record %u binds to stage %d", op, stage);
}

void maybe_split(VkCommandBuffer& cmd);
// ((Triforce) the early tail, below maybe_split: RX_VK_TAIL_EARLY, tail_early.h)
bool tail_on();
bool tail_last(uint64_t gpu, uint32_t* v);
bool tail_hold(VkCommandBuffer cmd, uint64_t gpu, uint32_t v);
void tail_at_dispatch(VkCommandBuffer& cmd, NvnState& s);
// (the gameplay upscale's dispatch, as the parallel replay's dry pass finds it with the early tail on: par_up, the state before it.
// The segments end there and the rest of the command buffer is recorded after them on the replay thread, where the tail begins)
bool dry_up_find = false, par_up_at = false;
ParPoint par_up;
void dry_up_check(const uint8_t* p, const nvn_op_hdr& h, NvnState& s) {
    if ((h.op != NOP_DISPATCH && h.op != NOP_DISPATCH_INDIRECT) || !s.render_enabled || !upscale_dispatch(s)) return;
    dry_up_find = false, par_up_at = true;
    par_up.p = p, par_up.s = s, par_up.ub = ub_updates.size(), par_up.cond = par_conds.size(), par_up.work = dry_work;
}

/// The handles already stored, so a rebind of the same ones does not rebuild descriptors
bool handles_unchanged(const uint64_t* have, const uint64_t* next, int n) {
    if (n < 0) return false;
    for (int i = 0; i < n; i++)
        if (have[i] != next[i]) return false;
    return true;
}
/// A uniform or storage binding already stored
bool buf_unchanged(int32_t stage, int32_t index, uint64_t addr, uint64_t size, int32_t nstage, int32_t nindex,
                   uint64_t naddr, uint64_t nsize) {
    return stage == nstage && index == nindex && addr == naddr && size == nsize;
}

/// (Triforce) The dry pass, at each draw and dispatch while dry_tail_find: told to the temporal hooks as replay() would
/// draw or dispatch it (vk_draw.cpp dry_temporal_draw, dry_temporal_dispatch); at the first that takes what they noted, the
/// state before it is kept as par_tail and the telling ends
void dry_tail_check(const uint8_t* p, const nvn_op_hdr& h, const uint8_t* a, uint32_t avail, NvnState& s) {
    const bool drawn = s.render_enabled && !s.raster_discard;
    bool at = false;
    switch (h.op) {
    case NOP_DRAW: at = drawn && dry_temporal_draw(s, payload<nvn_p_draw>(a, avail).prim, false); break;
    case NOP_DRAW_IDX: at = drawn && dry_temporal_draw(s, payload<nvn_p_draw_idx>(a, avail).prim, false); break;
    case NOP_DRAW_INDIRECT: at = drawn && dry_temporal_draw(s, payload<nvn_p_draw_indirect>(a, avail).prim, false); break;
    case NOP_DRAW_MESH: at = drawn && dry_temporal_draw(s, 0, true); break;
    case NOP_DISPATCH: case NOP_DISPATCH_INDIRECT: at = s.render_enabled && dry_temporal_dispatch(s); break;
    default: return;
    }
    if (!at) return;
    dry_tail_find = false, par_tail_at = true;
    par_tail.p = p, par_tail.s = s, par_tail.ub = ub_updates.size(), par_tail.cond = par_conds.size(), par_tail.work = dry_work;
}

// ---- (Triforce) RX_VK_MESH_AHEAD (mesh_ahead.h): a run of indirect mesh draws, their records copied at the first of them ----
/// What an op is to a run (NX2navi's op_linear): 0 it ends the run (it records outside a pass, writes memory on the GPU's timeline,
/// changes the targets, orders the queues, or ends the stream), 1 it may be in one (state, draws, what reaches only the CPU: the
/// counter reports the replay writes as it records, the render conditions it reads), 2 an indirect mesh draw
int mesh_run_kind(uint32_t op, const uint8_t* rec) {
    switch (op) {
    case NOP_BEGIN: case NOP_JUMP: case NOP_CALL: case NOP_VIEWPORTS: case NOP_SCISSOR: case NOP_DEPTH_RANGE: case NOP_STENCIL:
    case NOP_POLY_OFFSET: case NOP_BLEND_COLOR: case NOP_DEPTH_CLAMP: case NOP_RASTER_DISCARD: case NOP_PRIMITIVE_RESTART:
    case NOP_COPY_STRIDE: case NOP_SET_TEX_POOL: case NOP_SET_SAMP_POOL: case NOP_DRAW: case NOP_DRAW_IDX: case NOP_DRAW_INDIRECT:
    case NOP_BIND_PROGRAM: case NOP_BIND_STATE: case NOP_BIND_VATTR: case NOP_BIND_VSTREAM: case NOP_BIND_VBUF: case NOP_BIND_UBO:
    case NOP_BIND_SSBO: case NOP_BIND_TEX: case NOP_BIND_SAMP: case NOP_BIND_IMG: case NOP_BIND_BUFS: case NOP_PUSH_CONST:
    case NOP_REPORT_COUNTER: case NOP_RESET_COUNTER: case NOP_RENDER_ENABLE: case NOP_RENDER_COND:
        return 1;
    case NOP_DRAW_MESH: {
        nvn_op_hdr h;
        std::memcpy(&h, rec, sizeof h);
        return payload<nvn_p_draw_mesh>(rec + 8, h.size - 8).indirect ? 2 : 1;
    }
    default: return 0;
    }
}
/// The indirect mesh draw at p is about to be drawn: its op, and the draws after it in its run (up to end) unless their records were
/// copied already, for vk_draw.cpp's conversion (mesh_indirect_convert)
void mesh_ahead_note(const uint8_t* p, const uint8_t* end) {
    mesh_ahead::Pending& m = mesh_ahead::t_pending;
    m.op = p, m.n = 0;
    if (!mesh_ahead_enabled() || mesh_ahead::t_table.holds(p)) return;
    const uint8_t* run[mesh_ahead::kMax];
    const int k = mesh_ahead::scan(p, end, run, mesh_ahead::kMax, mesh_run_kind);
    for (int i = 0; i < k; i++) {
        nvn_op_hdr h;
        std::memcpy(&h, run[i], sizeof h);
        const auto d = payload<nvn_p_draw_mesh>(run[i] + 8, h.size - 8);
        m.next[m.n++] = {run[i], d.indirect, d.max, d.stride};
    }
}

void replay(VkCommandBuffer& cmd, NvnState& s, const uint8_t* p, const uint8_t* end = nullptr) {
    const Mode mode = rmode;
    const bool dry = mode == Mode::Dry, worker = mode == Mode::Worker;
    mesh_ahead::t_table.clear();   // ((Triforce) RX_VK_MESH_AHEAD: what was copied ahead holds within one walk of a command buffer)
    for (;;) {
        if (p == end) return;
        nvn_op_hdr h;
        std::memcpy(&h, p, sizeof h);
        if (h.size < 8) {   // (Triforce-dev, diagnostics: an op that cannot be one: the stream was freed or written over)
            static int logged;
            if (logged++ < 20)
                logf("vk: replay: op %u of size %u at %p (recording value %llu, %s): the rest of this command buffer is left out",
                     (unsigned)h.op, (unsigned)h.size, (const void*)p, (unsigned long long)rec_value_now,
                     dry ? "dry" : worker ? "worker" : "serial");
            return;
        }
        const uint8_t* a = p + 8;
        const uint32_t avail = h.size - 8;
        if (dry && dry_tail_find) dry_tail_check(p, h, a, avail, s);
        if (dry && dry_up_find) dry_up_check(p, h, s);   // ((Triforce) the early tail: par_up)
        if (mesh_ahead::t_table.n && !mesh_run_kind(h.op, p)) mesh_ahead::t_table.clear();   // ((Triforce) the run ends here)
        switch (h.op) {
        case NOP_END: return;
        case NOP_BEGIN: case NOP_JUMP: case NOP_CALL: break;
        // ---- memory the stream writes: on the GPU timeline ----
        case NOP_SIGNAL_EVENT: {
            if (!worker) {   // (in stream order: the replay thread's pass)
                const auto e = payload<nvn_p_signal>(a, avail);
                ev_cpu_apply();
                auto it = ev_values.find(e.gpu);
                uint32_t old = it != ev_values.end() ? it->second : 0;
                if (it == ev_values.end()) host->gpu_read(e.gpu, &old, 4);
                ev_values[e.gpu] = e.mode == NVN_EVENT_SIGNAL_MODE_ADD ? old + e.value : e.value;
            }
            if (dry) break;
            const auto e = payload<nvn_p_signal>(a, avail);
            uint32_t v = e.value;
            const HeldWrite* held = held_n ? held_write(e.gpu) : nullptr;   // ((Triforce) RX_VK_EVENT_BARRIER: its word not written yet)
            if (e.mode == NVN_EVENT_SIGNAL_MODE_ADD) {   // (the add on the CPU's view: events are few and in order)
                uint32_t old = 0;
                if (held) old = held->v;
                else if (!tail_last(e.gpu, &old)) host->gpu_read(e.gpu, &old, 4);
                v = old + e.value;
            }
            // ((Triforce) RX_VK_TAIL_EARLY, the early tail (tail_early.h): in a frame's tail the word is held, and written before the
            // tail's commands once the tail is judged at its recording's end (tail_early_record), else at that end (tail_end). Not
            // with RX_VK_EVENT_EARLY's write below either: the two are not for one run)
            if (tail_hold(cmd, e.gpu, v)) break;
            // ((Triforce) RX_VK_EVENT_BARRIER, barrier_rules.h: the word written right after the next full barrier on this command
            // buffer, the pass going on; a second signal of a held word is held after the first, written after it in order)
            BufferRef word{};
            const bool hold = (!held_n || event_writes_held(cmd)) &&
                              barrier_rules::event_hold(event_late_on(), par_active.load(std::memory_order_relaxed), held_n, kHeldWrites);
            if (hold) {   // (its buffer bound now, as gpu_write below binds it)
                memory_bind_event_write(true);
                word = gpu_buffer_bind(e.gpu, 4, true);
                memory_bind_event_write(false);
            }
            if (hold && word && !(word.offset & 3)) {
                held_writes[held_n++] = {e.gpu, v, word.buffer, word.offset};
                event_writes_hold(cmd);
                st_signals_held.fetch_add(1, std::memory_order_relaxed);
            } else if (hold) {   // (outside the pools or unaligned: on the CPU now, as gpu_write writes it)
                guest_cpu_write(e.gpu, &v, 4);
            } else {
                end_pass(cmd);
                GprofSpan gp(cmd, "event signal (barrier, write, barrier)");
                full_barrier(cmd);
                memory_bind_event_write(true);   // (Triforce: not a write-back the captures of its page wait for)
                gpu_write(cmd, e.gpu, &v, 4);
                memory_bind_event_write(false);
            }
            {   // (Triforce, 11-14, the lab) RX_VK_EVENT_EARLY=1: the word reaches the game's memory as the replay passes the
                // signal, not when the GPU has run it and its write-back has landed. OoT's Presentation thread polls its
                // frame's event (nvnEventGetValue) before it presents and lets the next frame's command buffers be built:
                // that poll ends at the GPU's end of the frame. With this it ends at the replay's end of it, the GPU's tail
                // (and the write-back, and the poll's own sleep) no longer in the frame's cycle. The game then writes its
                // next frame's memory while the GPU still reads this frame's
                static const bool early = getenv("RX_VK_EVENT_EARLY") && atoi(getenv("RX_VK_EVENT_EARLY")) != 0;
                if (early) guest_cpu_write(e.gpu, &v, 4);
            }
            break;
        }
        case NOP_WAIT_EVENT: {   // (a wait for an event another queue's work signals: submit_job orders them)
            if (worker) break;
            const auto w = payload<nvn_p_wait_event>(a, avail);
            ev_cpu_apply();
            const uint32_t have = event_value_in(ev_values, w.gpu);
            if (event_wait_met(have, w.mode, w.value)) {
                st_waits_met++;
                // ((Triforce) RX_VK_EVENT_BARRIER: the signal it waits for drained nothing; its write still held, the work after this
                // wait goes behind a full barrier, which writes it: after the signalling queue's work before the signal)
                if (!dry && barrier_rules::wait_barrier(held_n && held_write(w.gpu) && event_writes_held(cmd))) {
                    end_pass(cmd);
                    full_barrier(cmd, "barrier (a wait whose signal's write was held)");
                    st_waits_barred.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (suspend_ok) {   // (on once that work is replayed; a dry pass: its part up to here)
                suspend_at = p;
                return;
            } else {
                st_waits_unmet++;
                static int logged;
                if (logged++ < 20 || st_waits_unmet % 5000 == 0)
                    logf("vk: a wait not met in replay order: event %#llx wants %s %u, has %u (submit %llu; %lld met, %lld "
                         "not)", (unsigned long long)w.gpu, w.mode == 0 ? "==" : ">=", w.value, have,
                         (unsigned long long)st_submits, (long long)st_waits_met, (long long)st_waits_unmet);
            }
            break;
        }
        case NOP_REPORT_COUNTER: {
            if (worker) {   // (written by the dry pass, in stream order)
                if (occ_live) {   // (Triforce, RX_VK_OCC_QUERY: counted by the queries of the draws this segment recorded)
                    const auto c = payload<nvn_p_counter>(a, avail);
                    if ((uint32_t)c.type == NVN_COUNTER_TYPE_SAMPLES_PASSED)
                        occ_report_measure(cmd, c.addr, replay_stream_pos(), (c.type & NVN_COUNTER_WRITTEN) == 0);
                }
                break;
            }
            const auto c = payload<nvn_p_counter>(a, avail);
            const bool flag_written = (c.type & NVN_COUNTER_WRITTEN) != 0;   // (a clear flag: nvn_native.c wrote it as it was recorded)
            const bool samples = (uint32_t)c.type == NVN_COUNTER_TYPE_SAMPLES_PASSED;
            // (timestamps on the guest tick clock in counter ticks (19.2 MHz, nvnDeviceGetTimestampInNanoseconds
            // converts), the replay's time; occlusion counts grow with every draw, as in the WebGPU renderer)
            // (the GPU's time as the game measures it: from the frame's present on, at RX_VK_TS_SCALE (default 0.1)
            // times the real clock. The reports are written as the replay records them, so the real clock gave the
            // replay's time and its lag, not the GPU's: MKW's dynamic resolution moved every frame (each new size a
            // new set of ~20 render targets, re-registered textures, 20-25 ms of replay a frame) and kept low. The
            // GPU takes 6-7 ms a frame at the resolution it then holds. RX_VK_TS_SCALE=1: the real clock;
            // RX_VK_TS_GLOBAL=1: scaled from the start, not from each present.)
            static const double ts_scale = getenv("RX_VK_TS_SCALE") ? atof(getenv("RX_VK_TS_SCALE")) : 0.1;
            static const bool ts_global = getenv("RX_VK_TS_GLOBAL") != nullptr;
            static uint64_t ts_base = 0;
            uint64_t ns = host->ticks() * 32;
            if (!samples) {   // (a timestamp: the frame's span between its first and last, for ts_dyn_present)
                if (!ts_first_ns) ts_first_ns = ns;
                ts_last_ns = ns;
            }
            const double scale_now = ts_dyn_on() ? ts_dyn_scale : ts_scale;
            if (scale_now != 1.0) {
                const uint64_t base = ts_global ? (ts_base ? ts_base : (ts_base = ns)) : std::min(ns, ts_anchor_ns ? ts_anchor_ns : ns);
                ns = base + (uint64_t)((double)(ns - base) * scale_now);
            }
            // (hookshot) Every samples-passed report reads 4096: everything visible. The count of 16 a draw since the reset went
            // up and down from frame to frame, and OoT's sun visibility followed it: for one frame in ~70 the sunlight left the
            // ground and the haze brightened over the whole picture (one-frame flicker, most of it at load-in; 30-52 per 2,700
            // frames in Kokiri, 4-6 with this, RX 7900 GRE). RX_VK_OCC_FIXED=<n>: that value; -1: the per-draw count as before
            static const int64_t occ_fixed = getenv("RX_VK_OCC_FIXED") ? atoll(getenv("RX_VK_OCC_FIXED")) : 4096;
            const uint64_t count = samples ? (occ_fixed >= 0 ? (uint64_t)occ_fixed : s.samples_passed) : 0;
            const uint64_t rec[2] = {count, ns * 12 / 625};
            if (dump_on())
                logf("vk: dump-op %03d REPORT_COUNTER type %d at %#llx: %llu", dump_seq(), (int)c.type,
                     (unsigned long long)c.addr, (unsigned long long)rec[0]);
            // (written now, in stream order, not on the GPU timeline: SetRenderEnableConditional reads these words at
            // replay time, and MKW gates clears with them as TOTK does (the CPU writes 1 to ask for a clear, the report
            // after the clear resets it). A GPU-timeline write left the 1 in place for the stream's later conditions on
            // the same words, depending on how far the GPU was: extra clears of the HDR target, black frames at random;
            // and its late write could undo the game's next request. No pass ends for it either.)
            const auto write_record = [&] {
                if (flag_written) guest_cpu_write(c.addr + 8, rec + 1, 8);
                else guest_cpu_write(c.addr, rec, sizeof rec);
                memory_cpu_owned(host->gpu_guest(c.addr), sizeof rec);   // (vk_memory.cpp: kept out of the mirrors' write-backs)
            };
            // (Triforce, 11-17) RX_VK_OCC_QUERY=1: the record written here as before, 4096 for the count; the draws since the
            // reset are counted by occlusion queries (recorded here, or by the segment's thread after the dry pass), and once
            // the GPU is done the count goes over the 4096 if this is still the newest report at the address
            // ("occlusion queries" above). The placeholder's write and its entry under occ_mu: no count lands between them.
            // Another counter type (or a clear flag's report) at the address is the newest there from now on: no count of
            // an earlier samples-passed report is written over it, and a condition reads it from memory as before
            if (samples && occ_live) {
                const uint64_t pos = occ_pos(dry);
                {
                    std::lock_guard lk(occ_mu);
                    occ_placed(c.addr, pos, dry, flag_written, count);
                    write_record();
                }
                if (!dry) occ_report_measure(cmd, c.addr, pos, !flag_written);
            } else if (occ_live) {
                std::lock_guard lk(occ_mu);
                occ_rewritten(c.addr, 16, true);
                write_record();
            } else {
                write_record();
            }
            break;
        }
        case NOP_RESET_COUNTER: {
            const auto c = payload<nvn_p_counter>(a, avail);
            if (c.type == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
                s.samples_passed = 0;
                if (occ_live && !dry) occ_reset(cmd);   // (Triforce, RX_VK_OCC_QUERY: counting by occlusion queries from here)
            }
            break;
        }
        case NOP_COPY_B2B: {
            if (dry) break;
            ParLock lk;
            const auto c = payload<nvn_p_copy_bb>(a, avail);
            BufferRef src = gpu_buffer_bind(c.src, c.size, false), dst = gpu_buffer_bind(c.dst, c.size, true);
            {   // (RX_VK_WATCH_RANGE=<lo>:<hi>: buffer copies into it or out of it, 400 lines)
                static const std::pair<uint64_t, uint64_t> range = [] {
                    unsigned long long lo = 0, hi = 0;
                    if (const char* e = getenv("RX_VK_WATCH_RANGE")) sscanf(e, "%llx:%llx", &lo, &hi);
                    return std::pair<uint64_t, uint64_t>{lo, hi};
                }();
                static int logged;
                const bool hit = range.second && ((c.dst < range.second && c.dst + c.size > range.first) ||
                                                  (c.src < range.second && c.src + c.size > range.first));
                if (hit && logged++ < 400)
                    logf("vk: watch range buffer copy %#llx -> %#llx (%#llx bytes): source %s, destination %s", (unsigned long long)c.src,
                         (unsigned long long)c.dst, (unsigned long long)c.size, src ? "bound" : "NOT bound",
                         dst ? "bound" : "NOT bound (on the CPU)");
            }
            end_pass(cmd);
            GprofSpan gp(cmd, "copy buffer->buffer (with its barriers)");
            gp.bytes = c.size;
            full_barrier(cmd);
            if (src && dst) {
                const VkBufferCopy r{src.offset, dst.offset, c.size};
                vkCmdCopyBuffer(cmd, src.buffer, dst.buffer, 1, &r);
                const uint64_t guest = host->gpu_guest(c.dst);
                if (guest) memory_stamp_gpu(guest, c.size);
            } else {   // (outside the pools: on the CPU)
                for (uint64_t done = 0; done < c.size;) {
                    uint8_t buf[65536];
                    const uint64_t k = std::min<uint64_t>(sizeof buf, c.size - done);
                    if (!host->gpu_read(c.src + done, buf, k) || !guest_cpu_write(c.dst + done, buf, k)) break;
                    done += k;
                }
            }
            full_barrier(cmd);
            break;
        }
        case NOP_CLEAR_BUF: {   // (hookshot) nvnCommandBufferClearBuffer: on the GPU in order, as a buffer copy
            const auto c0 = payload<nvn_p_clear_buf>(a, avail);
            // (the small ones are the counters the reports and conditions use, which happen here on the CPU in stream order. Left
            // to the GPU, a clear earlier in the stream reached guest memory after a later report and wiped it, as far as the GPU
            // was behind: lights and whole rooms switched off by their conditions for a frame. RX_VK_CLEAR_CPU_MAX=0: on the GPU)
            static const uint64_t cpu_max = getenv("RX_VK_CLEAR_CPU_MAX") ? (uint64_t)atoll(getenv("RX_VK_CLEAR_CPU_MAX")) : 256;
            const bool in_stream = c0.size && c0.size <= cpu_max;
            if (in_stream && !worker) {
                uint32_t words[64];
                for (auto& w : words) w = c0.value;
                if (occ_live) {   // (Triforce, RX_VK_OCC_QUERY: a samples-passed record it covers is the clear's from now on)
                    std::lock_guard lk(occ_mu);
                    occ_rewritten(c0.dst, std::min<uint64_t>(c0.size, sizeof words), false);
                    guest_cpu_write(c0.dst, words, std::min<uint64_t>(c0.size, sizeof words));
                } else {
                    guest_cpu_write(c0.dst, words, std::min<uint64_t>(c0.size, sizeof words));   // (hookshot: not past words)
                }
            }
            if (dry) break;
            ParLock lk;
            const auto c = c0;
            if (!c.size) break;
            BufferRef dst = gpu_buffer_bind(c.dst, c.size, true);
            {   // (EvilOOT: RX_VK_WATCH_RANGE, the clears into it too)
                unsigned long long lo = 0, hi = 0;
                static const char* e = getenv("RX_VK_WATCH_RANGE");
                if (e) sscanf(e, "%llx:%llx", &lo, &hi);
                static int logged;
                if (hi && c.dst < hi && c.dst + c.size > lo && logged++ < 400)
                    logf("vk: watch range buffer clear %#llx (%#llx bytes) to %#x: %s", (unsigned long long)c.dst,
                         (unsigned long long)c.size, (unsigned)c.value, dst ? "on the GPU" : "NOT bound");
            }
            end_pass(cmd);
            GprofSpan gp(cmd, "clear buffer (with its barriers)");
            gp.bytes = c.size;
            full_barrier(cmd);
            if (dst && !(dst.offset & 3) && !(c.size & 3)) {
                vkCmdFillBuffer(cmd, dst.buffer, dst.offset, c.size, c.value);
                const uint64_t guest = host->gpu_guest(c.dst);
                if (guest && !in_stream) memory_stamp_gpu(guest, c.size);
            } else if (!in_stream) {   // (outside the pools or unaligned: on the CPU)
                uint32_t buf[1024];
                for (auto& w : buf) w = c.value;
                for (uint64_t done = 0; done < c.size;) {
                    const uint64_t k = std::min<uint64_t>(sizeof buf, c.size - done);
                    if (!guest_cpu_write(c.dst + done, buf, k)) break;
                    done += k;
                }
            }
            full_barrier(cmd);
            break;
        }
        // (Triforce) in the dry pass a copy's and a texture clear's destination is marked as the GPU's, in stream order, as a
        // draw's targets are (dry_mark_written): vk_xfer.cpp, "the parallel replay's dry pass". The copies the CPU makes
        // (ASTC blocks into a texture's storage) are made there, and not again by the worker
        case NOP_COPY_B2T:
            if (dry && dry_copies_on()) dry_copy_b2t(s, payload<nvn_p_copy_bt>(a, avail));
            if (!dry) { ParLock lk; copy_b2t(cmd, s, payload<nvn_p_copy_bt>(a, avail), true, worker && dry_copies_on()); }
            break;
        case NOP_COPY_T2B: if (!dry) { ParLock lk; copy_b2t(cmd, s, payload<nvn_p_copy_bt>(a, avail), false); } break;
        case NOP_COPY_T2T:
            if (dry && dry_copies_on()) dry_copy_t2t(payload<nvn_p_copy_tt>(a, avail));
            if (!dry) { ParLock lk; copy_t2t(cmd, s, payload<nvn_p_copy_tt>(a, avail)); }
            break;
        case NOP_CLEAR_TEX:
            if (dry && dry_copies_on()) dry_clear_tex(payload<nvn_p_clear_tex>(a, avail));
            if (!dry) { ParLock lk; clear_tex(cmd, s, payload<nvn_p_clear_tex>(a, avail)); }
            break;
        case NOP_UPDATE_UB: {
            const auto u = payload<nvn_p_update_ub>(a, avail);
            if (worker) {   // (appended by the dry pass: the draws after it read it from here on)
                // (hookshot) counted only when the dry pass appended it (the same test as below): an update of no bytes, or of more
                // than the stream holds, is not in ub_updates, and counting it moved every later draw of the segment one update ahead
                if (u.size && u.size <= avail - sizeof u) ub_limit++, ub_gen++;
                break;
            }
            if (dump_on() && u.size && u.size <= avail - sizeof u) {   // (in a dump: each update, its first words)
                float f[12] = {};
                std::memcpy(f, a + sizeof u, std::min<size_t>(sizeof f, u.size));
                logf("vk: dump-op %03d UPDATE_UB %#llx +%#x: %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g", dump_seq(),
                     (unsigned long long)u.dst, (unsigned)u.size, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
            }
            if (u.size && u.size <= avail - sizeof u) {
                UbUpdate up{rec_value_now, u.dst, std::vector<uint8_t>(a + sizeof u, a + sizeof u + u.size)};
                {
                    UbGrowGuard g;   // (RX_VK_SEG_EARLY: helpers may read the list while the dry pass appends)
                    ub_updates.push_back(std::move(up));
                }
                ub_list_epoch.fetch_add(1, std::memory_order_relaxed);
                ub_gen++;
                st_ub_updates++;
            }
            break;
        }
        // ---- state ----
        case NOP_SET_RT: s.rt = payload<nvn_p_set_rt>(a, avail), s.rt_serial++; break;
        case NOP_VIEWPORTS: {
            const auto v = payload<nvn_p_viewports>(a, avail);
            for (int i = 0; i < v.n && v.first + i < 16; i++) std::memcpy(s.viewport[v.first + i], v.v[i], 16);
            break;
        }
        case NOP_SCISSOR: {
            const auto c = payload<nvn_p_scissor>(a, avail);
            s.scissor[0] = c.x, s.scissor[1] = c.y, s.scissor[2] = c.w, s.scissor[3] = c.h;
            break;
        }
        case NOP_DEPTH_RANGE: {
            const auto d = payload<nvn_p_depth_range>(a, avail);
            s.depth_near = d.n, s.depth_far = d.f;
            break;
        }
        case NOP_BIND_PROGRAM: {
            const auto b = payload<nvn_p_bind_program>(a, avail);
            // (the same program bound again: its descriptors already stand)
            const bool same = b.prog && s.has_prog ? std::memcmp(&s.prog, b.prog, sizeof s.prog) == 0 : !b.prog && !s.has_prog;
            if (!same) s.bind_gen++;
            if (b.prog) s.prog = *b.prog;
            s.has_prog = b.prog != nullptr && b.prog->nstage > 0;
            break;
        }
        case NOP_BIND_STATE: {
            const auto b = payload<nvn_p_bind_state>(a, avail);
            switch (b.kind) {
            case 0: {
                nvn_blend_st st;
                std::memcpy(&st, b.bytes, sizeof st);
                s.blend[st.target & 7] = st;
                break;
            }
            case 1: std::memcpy(&s.color, b.bytes, sizeof s.color); break;
            case 2: std::memcpy(&s.chmask, b.bytes, sizeof s.chmask); break;
            case 3: std::memcpy(&s.ms, b.bytes, sizeof s.ms); break;
            case 4: std::memcpy(&s.poly, b.bytes, sizeof s.poly); break;
            case 5: std::memcpy(&s.ds, b.bytes, sizeof s.ds); break;
            default: break;
            }
            break;
        }
        case NOP_BIND_VATTR: {
            const auto v = payload<nvn_p_bind_vattr>(a, avail);
            std::memset(s.vattr, 0, sizeof s.vattr);
            for (int i = 0; i < v.n && i < 16; i++) s.vattr[i] = v.st[i];
            s.nvattr = std::min(v.n, 16);
            break;
        }
        case NOP_BIND_VSTREAM: {
            const auto v = payload<nvn_p_bind_vstream>(a, avail);
            for (int i = 0; i < v.n && i < 16; i++) s.vstream[i] = v.st[i];
            break;
        }
        case NOP_BIND_VBUF: {
            const auto v = payload<nvn_p_bind_vbuf>(a, avail);
            for (int i = 0; i < v.n && v.first + i < 16; i++) s.vbuf[v.first + i] = v.r[i];
            break;
        }
        case NOP_BIND_BUFS: {
            auto b = payload<nvn_p_bind_bufs>(a, avail);
            // (deferred binds: the ranges as the game's array held them at the submit, which the host copied into
            // the recording then (nvn_native.c resolve_deferred). Read again here, at the replay, the array was
            // often the next frame's already (MKW refills it as soon as the submit returns): the terrain patches
            // took other patches' placements (holes in the far terrain, the minimap's triangles). RX_VK_DEFERRED_LATE=1:
            // read at the replay, as before.)
            {   // (EvilOOT, RX_VK_LOG_DEFERRED=1: compute's deferred binds, as at the submit and as the array holds them now)
                static const bool on = getenv("RX_VK_LOG_DEFERRED") != nullptr;
                static std::atomic<int> told{0};
                if (on && b.src && b.stage == 5 && told < 400) {
                    NVNbufferRange now[16]{};
                    const int n = std::clamp(b.n, 0, 16);
                    host->gpu_read(b.src, now, (uint64_t)n * sizeof(NVNbufferRange));
                    for (int i = 0; i < n && told < 400; i++, told++)
                        logf("vk: deferred %s stage 5 slot %d (array %#llx): submit %#llx+%#llx, now %#llx+%#llx%s", b.ssbo ? "ssbo" : "ubo",
                             b.first + i, (unsigned long long)b.src, (unsigned long long)b.r[i].address, (unsigned long long)b.r[i].size,
                             (unsigned long long)now[i].address, (unsigned long long)now[i].size,
                             now[i].address != b.r[i].address || now[i].size != b.r[i].size ? "  DIFFERS" : "");
                }
            }
            if (b.src && deferred_late()) {
                const int n = std::clamp(b.n, 0, 16);
                std::memset(b.r, 0, sizeof b.r);
                host->gpu_read(b.src, b.r, (uint64_t)n * sizeof(NVNbufferRange));
            }
            for (int i = 0; i < b.n && i < 16; i++) {
                const int index = b.first + i;
                if (b.stage < 0 || b.stage >= kNvnStages || index < 0 || index >= 16) continue;
                {
                    static const bool on = getenv("RX_VK_LOG_MESH_BINDS") != nullptr;
                    static std::atomic<int> told{0};
                    if (on && b.stage >= 6 && b.r[i].size != 0x100 && told++ < 300)
                        logf("vk: bind (many) %s stage %d index %d addr %#llx size %#llx", b.ssbo ? "ssbo" : "ubo", b.stage, index,
                             (unsigned long long)b.r[i].address, (unsigned long long)b.r[i].size);
                }
                nvn_p_bind_buf& u = (b.ssbo ? s.ssbo : s.ubo)[b.stage][index];
                if (!buf_unchanged(u.stage, u.index, u.addr, u.size, b.stage, index, b.r[i].address, b.r[i].size)) {
                    s.bind_gen++;
                    if (b.ssbo) s.ssbo_gen[b.stage] = s.bind_gen;
                }
                u.stage = b.stage, u.index = index, u.addr = b.r[i].address, u.size = b.r[i].size;
            }
            break;
        }
        case NOP_BIND_UBO: case NOP_BIND_SSBO: {
            const auto b = payload<nvn_p_bind_buf>(a, avail);
            if (b.stage >= kNvnStages) log_stage_bind(h.op, b.stage);
            {   // (hookshot) RX_VK_LOG_MESH_BINDS=1: what the game binds to the task and mesh stages, 80 lines
                static const bool on = getenv("RX_VK_LOG_MESH_BINDS") != nullptr;
                static std::atomic<int> told{0};
                if (on && b.stage >= 6 && b.size != 0x100 && told++ < 300)
                    logf("vk: bind %s stage %d index %d addr %#llx size %#llx", h.op == NOP_BIND_UBO ? "ubo" : "ssbo", b.stage, b.index,
                         (unsigned long long)b.addr, (unsigned long long)b.size);
            }
            if (b.stage >= 0 && b.stage < kNvnStages && b.index >= 0 && b.index < 16) {
                nvn_p_bind_buf& u = (h.op == NOP_BIND_UBO ? s.ubo : s.ssbo)[b.stage][b.index];
                if (!buf_unchanged(u.stage, u.index, u.addr, u.size, b.stage, b.index, b.addr, b.size)) {
                    s.bind_gen++;
                    if (h.op == NOP_BIND_SSBO) s.ssbo_gen[b.stage] = s.bind_gen;
                }
                u = b;
            }
            break;
        }
        case NOP_BIND_TEX: case NOP_BIND_SAMP: case NOP_BIND_IMG: {
            nvn_p_bind_handles b;
            std::memset(&b, 0, sizeof b);
            std::memcpy(&b, a, std::min<size_t>(sizeof b, h.size - 8));
            if (b.src && deferred_late()) {   // (deferred: the handles as of the submit, as the ranges above)
                const int n = std::clamp(b.n, 0, 32);
                std::memset(b.h, 0, sizeof b.h);
                host->gpu_read(b.src, b.h, (uint64_t)n * 8);
            }
            if (b.stage >= kNvnStages) log_stage_bind(h.op, b.stage);
            if (b.stage < 0 || b.stage >= kNvnStages || b.first < 0) break;
            uint64_t* to = h.op == NOP_BIND_TEX ? s.tex[b.stage] : h.op == NOP_BIND_SAMP ? s.samp[b.stage] : s.img[b.stage];
            const int lim = h.op == NOP_BIND_IMG ? 32 : 128;
            int n = 0;
            while (n < b.n && b.first + n < lim) n++;
            if (!handles_unchanged(to + b.first, b.h, n)) {   // (the same handles: the draw keeps its descriptors)
                s.bind_gen++;
                s.tex_gen[b.stage] = s.bind_gen;
            }
            for (int k = 0; k < n; k++) to[b.first + k] = b.h[k];
            break;
        }
        case NOP_STENCIL: {
            const auto st = payload<nvn_p_stencil>(a, avail);
            for (int f = 0; f < 2; f++) {
                if (!(st.face & (1 << f))) continue;
                if (st.which == 0) s.stencil_value_mask[f] = (uint32_t)st.value_mask;
                else if (st.which == 1) s.stencil_mask[f] = (uint32_t)st.mask;
                else s.stencil_ref[f] = (uint32_t)st.ref;
            }
            break;
        }
        case NOP_POLY_OFFSET: {
            const auto o = payload<nvn_p_poly_offset>(a, avail);
            s.poly_offset[0] = o.factor, s.poly_offset[1] = o.units, s.poly_offset[2] = o.clamp;
            break;
        }
        case NOP_BLEND_COLOR: std::memcpy(s.blend_color, payload<nvn_p_blend_color>(a, avail).rgba, 16); break;
        case NOP_PRIMITIVE_RESTART: {
            const auto r = payload<nvn_p_restart>(a, avail);
            s.restart = r.enable != 0, s.restart_index = (uint32_t)r.index;
            break;
        }
        case NOP_DEPTH_CLAMP: s.depth_clamp = payload<nvn_p_flag>(a, avail).value != 0; break;
        case NOP_RASTER_DISCARD: s.raster_discard = payload<nvn_p_flag>(a, avail).value != 0; break;
        case NOP_RENDER_ENABLE:
            s.render_enabled = payload<nvn_p_flag>(a, avail).value != 0;
            if (occ_live && !worker && occ_cond_off == &s) occ_cond_off = nullptr;   // (Triforce: no condition's from here)
            if (dump_on()) logf("vk: dump-op %03d RENDER_ENABLE %d", dump_seq(), (int)s.render_enabled);
            break;
        case NOP_RENDER_COND: {   // (read on the CPU for now; VK_EXT_conditional_rendering later)
            if (worker) {   // (as the dry pass found it, with the reports before it written and none after)
                UbReadGuard g;   // (RX_VK_SEG_EARLY: the dry pass may still append)
                s.render_enabled = cond_at < par_conds_n.load(std::memory_order_acquire) ? par_conds[cond_at] != 0 : true;
                cond_at++;
                break;
            }
            const auto r = payload<nvn_p_render_cond>(a, avail);
            uint64_t x = r.x, y = r.y;
            if (!r.snapped) {
                host->gpu_read(r.addr, &x, 8);
                host->gpu_read(r.addr + 16, &y, 8);
            }
            s.render_enabled = r.mode == 0 ? x == y : x != y;
            // (Triforce, RX_VK_OCC_QUERY: a samples-passed word whose count is still to come renders; one whose count was
            // written is read from memory as above, where a 0 skips; "occlusion queries" above)
            if (occ_live) {
                if (r.snapped) occ_cond_off = nullptr;
                else occ_cond(s, r.addr, (int)r.mode, x, y, &s.render_enabled);
            }
            {   // (RX_VK_COND_ALWAYS=1 (test): every condition holds. The frame log's dark frames dump the last ones)
                static const bool always = getenv("RX_VK_COND_ALWAYS") != nullptr;
                const uint32_t at = cond_ring_at++ % (1 << 15);
                cond_ring[at] = {presents_now, r.addr, x, y, (int)r.mode, s.render_enabled};
                if (always) s.render_enabled = true;
            }
            if (dry) {   // (RX_VK_SEG_EARLY: the lock only when the append moves the buffer; the count published after it)
                if (par_conds.size() == par_conds.capacity()) {
                    UbGrowGuard g;
                    par_conds.push_back(s.render_enabled ? 1 : 0);
                } else {
                    par_conds.push_back(s.render_enabled ? 1 : 0);
                }
                par_conds_n.store(par_conds.size(), std::memory_order_release);
            }
            if (dump_on())
                logf("vk: dump-op %03d RENDER_COND mode %d at %#llx: %llu vs %llu: %s", dump_seq(), r.mode,
                     (unsigned long long)r.addr, (unsigned long long)x, (unsigned long long)y,
                     s.render_enabled ? "on" : "off");
            break;
        }
        case NOP_SET_TEX_POOL: case NOP_SET_SAMP_POOL:
            s.bind_gen++;
            for (int k = 0; k < kNvnStages; k++) s.tex_gen[k] = s.bind_gen;
            break;
        case NOP_COPY_STRIDE: s.copy_stride = payload<nvn_p_copy_stride>(a, avail).stride; break;
        case NOP_PUSH_CONST: {   // (hookshot) compute push constants, kept in the state (vk_bank.cpp serves them from c0)
            const auto u = payload<nvn_p_push_const>(a, avail);
            if (u.size && u.size <= avail - sizeof u && (uint64_t)u.offset + u.size <= sizeof s.cs_push) {
                const uint8_t* src = a + sizeof u;
                const bool same = (uint64_t)u.offset + u.size <= s.cs_push_n && std::memcmp(s.cs_push + u.offset, src, u.size) == 0;
                std::memcpy(s.cs_push + u.offset, src, u.size);
                s.cs_push_n = std::max<uint32_t>(s.cs_push_n, u.offset + u.size);
                if (!same) s.bind_gen++;
            }
            break;
        }
        case NOP_BARRIER:
            if (dry) break;
            {
                const int32_t bits = payload<int32_t>(a, avail);
                std::lock_guard lk(st_barrier_flags_mu);
                st_barrier_flags[bits]++;
            }
            end_pass(cmd);
            full_barrier(cmd, "barrier (the game's nvnCommandBufferBarrier)");
            st_barriers++;
            break;
        // ---- work ----
        case NOP_CLEAR_COLOR:
            // (hookshot) in the dry pass a clear marks its pass's targets as the GPU's, as a draw does (dry_mark_written): the clear
            // opens a pass on the worker, and that marks them (begin_pass), but only when the worker gets there. A target the game
            // clears in one segment and samples in a later one was still "never written" for the later segment when it was
            // recorded first: it uploaded what the target's memory held, an upload that runs after the clear. 13 of them in 4,300
            // presents of Castle Town's market with the camera turning, each an 853 x 480 target made that frame ("written in
            // segment N, uploaded by the later segment N+2; written by: a pass's colour target"), among 38 one-frame flashes
            // against 9 on one thread. RX_VK_NO_DRY_CLEARS=1: as before
            if (occ_live && !worker) occ_skip_note(s, true);   // (Triforce: one a written count's condition skips, counted)
            if (s.render_enabled && dry && dry_clears_on()) dry_mark_written(s, false, true);
            if (s.render_enabled && !dry) {
                const auto c = payload<nvn_p_clear_color>(a, avail);
                clear_color_target(cmd, s, c.index, c.rgba, (uint32_t)c.mask);
            }
            break;
        case NOP_CLEAR_DS:
            if (occ_live && !worker) occ_skip_note(s, true);   // (Triforce: as NOP_CLEAR_COLOR above)
            if (s.render_enabled && dry && dry_clears_on()) dry_mark_written(s, false, true);   // (as NOP_CLEAR_COLOR above)
            if (s.render_enabled && !dry) {
                const auto c = payload<nvn_p_clear_ds>(a, avail);
                clear_depth_stencil(cmd, s, c.depth, c.depth_mask != 0, (uint32_t)c.stencil, (uint32_t)c.stencil_mask);
            }
            break;
        case NOP_DRAW: {
            const auto d = payload<nvn_p_draw>(a, avail);
            if (s.render_enabled && !s.raster_discard && !dry) {
                DrawArgs da;
                da.prim = d.prim, da.first = d.first, da.count = d.count;
                da.base_instance = d.base_instance, da.instances = d.instances;
                draw(cmd, s, da);
            }
            s.samples_passed += 16;
            break;
        }
        case NOP_DRAW_IDX: {
            const auto d = payload<nvn_p_draw_idx>(a, avail);
            if (s.render_enabled && !s.raster_discard && !dry) {
                DrawArgs da;
                da.prim = d.prim, da.indexed = true, da.itype = d.itype, da.count = d.count;
                da.base_vertex = d.base_vertex, da.base_instance = d.base_instance, da.instances = d.instances;
                da.ibuf = d.ibuf;
                draw(cmd, s, da);
            }
            s.samples_passed += 16;
            break;
        }
        case NOP_DRAW_INDIRECT: {
            const auto d = payload<nvn_p_draw_indirect>(a, avail);
            if (s.render_enabled && !s.raster_discard && !dry) {
                DrawArgs da;
                da.prim = d.prim, da.indexed = d.indexed != 0, da.itype = d.itype;
                da.indirect = d.indirect, da.count_addr = d.count_addr, da.max = d.max, da.stride = d.stride;
                da.ibuf = d.ibuf;
                draw(cmd, s, da);
            }
            s.samples_passed += 16;
            break;
        }
        case NOP_DRAW_MESH: {
            const auto d = payload<nvn_p_draw_mesh>(a, avail);
            static int logged_mesh;
            if (logged_mesh++ < 8)
                logf("vk: mesh draw kind %d: %d %d %d, indirect %#llx count %#llx max %d stride %d", d.kind, d.x, d.y,
                     d.z, (unsigned long long)d.indirect, (unsigned long long)d.count_addr, d.max, d.stride);
            if (s.render_enabled && !s.raster_discard && !dry) {
                DrawArgs da;
                da.mesh = true;
                // (DrawMeshTasks(first, count): the count of task workgroups, as Eden draws NVN2's mesh draws; a
                // first task other than 0 has no Vulkan counterpart)
                da.groups[0] = d.y, da.groups[1] = 1, da.groups[2] = 1;
                if (d.kind == 0) da.first_task = d.x;   // (hookshot: the task programs add it, vk_draw.cpp's mesh_args)
                da.indirect = d.indirect, da.count_addr = d.count_addr, da.max = d.max, da.stride = d.stride;
                if (da.indirect) mesh_ahead_note(p, end);   // ((Triforce) RX_VK_MESH_AHEAD: its op and its run's next draws)
                draw(cmd, s, da);
            }
            s.samples_passed += 16;
            break;
        }
        case NOP_DISPATCH: {
            const auto d = payload<nvn_p_dispatch>(a, avail);
            if (s.render_enabled && !dry) tail_at_dispatch(cmd, s);   // ((Triforce) the early tail: it may begin here)
            if (s.render_enabled && !dry) dispatch(cmd, s, (uint32_t)d.x, (uint32_t)d.y, (uint32_t)d.z, 0);
            break;
        }
        case NOP_DISPATCH_INDIRECT: {
            const auto d = payload<nvn_p_dispatch_indirect>(a, avail);
            if (s.render_enabled && !dry) tail_at_dispatch(cmd, s);
            if (s.render_enabled && !dry) dispatch(cmd, s, 1, 1, 1, d.addr);
            break;
        }
        default: break;
        }
        if (h.op == NOP_DRAW || h.op == NOP_DRAW_IDX || h.op == NOP_DRAW_INDIRECT || h.op == NOP_DRAW_MESH ||
            h.op == NOP_DISPATCH || h.op == NOP_DISPATCH_INDIRECT) {
            if (occ_live && !worker) occ_skip_note(s, false);   // (Triforce: one a written count's condition skips, counted)
            if (dry) {
                const bool compute = h.op == NOP_DISPATCH || h.op == NOP_DISPATCH_INDIRECT;
                if (s.render_enabled && (compute || !s.raster_discard)) dry_mark_written(s, compute, dry_work == 0);
                if (++dry_work % par_point_every() == 0) {
                    // (RX_VK_SEG_EARLY: the early path takes one point a boundary; a point it does not take is not copied: ~27 KB of state)
                    if (!seg_early_hook || seg_early_hook->wants(dry_work, p + h.size)) {
                        par_points.push_back({p + h.size, s, ub_updates.size(), par_conds.size(), dry_work});
                        if (seg_early_hook) seg_early_hook->on_point(par_points.back());
                    }
                }
            } else {
                stream_work++;   // (Triforce: counted as dry_work counts them, for replay_stream_pos)
                // ((Triforce) tex_warm.h: how light the frame is. Only with the warm-up on: an atomic add on one line from every
                // replay thread, for every draw, is a cost the default path should not pay for a count nothing reads)
                static const bool count_work = texture_warm_on();
                if (count_work) frame_work.fetch_add(1, std::memory_order_relaxed);
                if (!worker) maybe_split(cmd);
            }
        }
        p += h.size;
    }
}

// ---- chunks: a long submission goes to the GPU in parts (RX_VK_CHUNK=<draws and dispatches a part>, default 120;
// 0: whole), so the GPU starts on its first part while the rest is recorded. The game waits for its fences within
// the frame: with the whole submission recorded before the GPU saw any of it, its wait was the replay's time plus
// the GPU's. Only the last part signals the submission's timeline value; the parts' slots free with it. ----
Recording* rec_cur = nullptr;
uint64_t rec_cur_value = 0;
int rec_parts = 0;
int64_t part_work = 0;
// (a slot is used again once its own submit is done (recording_wait): a submission's parts may outnumber the slots,
// which a wait on the timeline value it signals at its end kept them from. A parallel replay's segments are recorded
// at once, kMaxBatch of them at most: fewer than the slots)
constexpr int kMaxParts = 32;
constexpr int kMaxBatch = 6;   // (three recording threads: more segments only took slots and submits)
/// (Triforce, 11-15) The most segments a command buffer is cut into: kMaxBatch, or RX_VK_PARALLEL_SEGS=<2..12>. With kMaxBatch a
/// smaller RX_VK_PARALLEL_SEG cuts no finer once a command buffer has kMaxBatch segments' worth (the owner's test8: ~750 draws and
/// dispatches each, 4.6 segments of ~160): RX_VK_PARALLEL_SEG=64 gives 6 segments there, 32 the same 6. Up to 12, so that three
/// command buffers' segments and the serial parts stay within the 48 slots a frame or so
int par_seg_most() {
    static const int v = getenv("RX_VK_PARALLEL_SEGS") ? std::clamp(atoi(getenv("RX_VK_PARALLEL_SEGS")), 2, 12) : kMaxBatch;
    return v;
}

void maybe_split(VkCommandBuffer& cmd) {
    static const int64_t every = getenv("RX_VK_CHUNK") ? atoll(getenv("RX_VK_CHUNK")) : 120;
    if (tail_on()) return;   // ((Triforce) the early tail stays one command buffer, to its recording's end)
    if (!rec_cur || every <= 0 || ++part_work < every || rec_parts + 1 >= kMaxParts || dump_on()) return;
    // ((Triforce) RX_VK_SPLIT_BARRIER, barrier_rules.h: a pass open at the split goes on in the next part behind a barrier of the
    // attachments' outputs alone, where nothing else is recorded at the split; otherwise a full barrier at the part's end and the
    // next one's start, as before)
    static const bool in_pass = [] {
        const char* e = getenv("RX_VK_SPLIT_BARRIER");
        const bool every_draw = getenv("RX_VK_BARRIER_EVERY_DRAW") != nullptr;
        const bool v = barrier_rules::split_in_pass(e, dev.props.vendorID, every_draw);
        logf("vk: a submission's parts: %s (RX_VK_SPLIT_BARRIER%s%s; =full: full barriers at every split, =pass: on any card)",
             v ? "a pass open at a split goes on in the next part behind a barrier of its attachments alone"
               : "a full barrier at each part's end and the next one's start",
             e && *e ? "=" : every_draw ? ", RX_VK_BARRIER_EVERY_DRAW" : dev.props.vendorID == 0x10DE ? ", NVIDIA's driver" : ", not NVIDIA's driver",
             e && *e ? e : "");
        return v;
    }();
    const bool open = in_pass && pass_carry_take();
    end_pass(cmd);
    // (the events' held writes before the part's end: its write-backs copy their words to the game's memory. Then no carry: the
    // barrier drained the GPU there all the same)
    const bool held = event_writes_held(cmd);
    event_writes_flush(cmd, "barrier (events' held writes, before the recording's end)");
    // ((Triforce, P4) "recorded" by this thread, into any command buffer, or submitted to the queue by any (cmd_work.h work_between).
    // No parallel replay's argument: its segments are never split (maybe_split runs in the serial replay, never while they record))
    const uint64_t ended = work_between();
    flush_ub_updates(cmd);
    texture_end_recording(cmd);
    memory_end_submission(cmd);
    const bool quiet = work_between() == ended;
    const bool carry = barrier_rules::split_carry(in_pass, open, quiet, held);
    if (!carry) full_barrier(cmd, "barrier (recording end)");
    split_count(in_pass, open, quiet, held, carry);
    submit_recording(*rec_cur, false, 0, true);   // (a part)
    ub_first_of_recording = ub_updates.size();
    const uint64_t sent = work_between();
    Recording& r = begin_recording(rec_cur_value);
    memory_begin_submission();
    if (barrier_rules::split_start_full(carry, work_between() != sent))
        full_barrier(r.cmd, "barrier (recording start)");
    rec_cur = &r;
    cmd = r.cmd;
    part_work = 0;
    rec_parts++;
    pass_carry_resume(cmd, carry);   // (Triforce: vk_draw.cpp, the pass again with its targets and what its draws did)
    if (carry) split_carried_cmd = cmd;
}

// ---- (Triforce) the early tail: Start next frame early (the Debug tab's Developer row, tail_early=; RX_VK_TAIL_EARLY=1 / 0 over
// it; on by default since options_version 6, the owner's choice). What it is, its rules and why each: tail_early.h. After
// NX2navi's tail_early (runtime/vk_exec.c "the early tail", vk_sync.c, by epicmeme546; aec5e720), reimplemented for this
// replay: NX2navi ends the game's waits on a timeline its
// completion thread follows and writes the results on the CPU; here the game polls its event's word in its own memory, so the
// held words are written on the GPU, in the tail's upload command buffer, ahead of the tail's commands.
// The tail begins at the gameplay upscale's dispatch (vk_draw.cpp upscale_dispatch) recorded on the replay thread: in a serial
// replay, or after a parallel replay's segments (par_up, tail_rest). Its recording is the rest of the submission (the burst's),
// one command buffer to its end: no split (maybe_split), no parallel replay (replay_handle). Not with a frame dump or the bench
// (RX_VK_BENCH: its recordings end in bench_replay). The lazy write-back needs no rule of its own: a page it owes is paid by a
// copy after everything on the queue, the tail too, and the tail writes no page but its uniform updates' (WRITES).
// A tail that ends some other way than at finish_recording (CUT, a safety net: none is known) has its words written at its end,
// in guest memory and in the mirror, as an early tail's are but after its commands (tail_early_record).
// Every 1200 tails a line says how many went early and why the others did not (tail_stats). ----
struct TailState {
    bool on = false;          // (a tail is being recorded: rec)
    bool early = false;       // (judged early: its words go into rec's upload command buffer, tail_early_record)
    Recording* rec = nullptr;
    uint64_t written0 = 0, guest0 = 0, programs0 = 0;   // (the counts when it began: tail_seen)
    tail_early::Held held;
    struct Word { VkBuffer buffer; VkDeviceSize offset; uint32_t v; };
    std::vector<Word> words;   // (each held word's writes, guest memory's and its mirror's, in the order of the signals)
} tl;
tail_early::Stats tl_stats;
thread_local bool tail_here = false;   // (the replay thread records a parallel replay's rest after its segments: tail_rest)

bool tail_wanted() {
    static const bool v = [] {
        const bool bench = getenv("RX_VK_BENCH") != nullptr;
        const bool on = !bench && menu_row_at_start("RX_VK_TAIL_EARLY", "tail_early", true, 6);
        if (on || bench)
            logf("vk: the early tail %s (RX_VK_TAIL_EARLY, the Debug tab's Start next frame early): %s", on ? "on" : "off",
                 on ? "the game's frame wait ends once the scene's write-backs are in its memory, before the upscaler and the interface"
                    : "not with the bench (RX_VK_BENCH)");
        return on;
    }();
    return v;
}
bool tail_on() { return tl.on; }
bool tail_last(uint64_t gpu, uint32_t* v) { return tl.on && tl.held.last(gpu, v); }
/// A signal in the tail: its word held, and the full barrier its write had kept at its place, so that the work after a wait met
/// for it (another queue's, replayed later into the tail) is still after the work before the signal on the GPU
bool tail_hold(VkCommandBuffer cmd, uint64_t gpu, uint32_t v) {
    if (!tl.on) return false;
    end_pass(cmd);
    full_barrier(cmd, "barrier (an event signal in the early tail: its word held)");
    tl.held.add(gpu, v);
    return true;
}
/// What the tail's commands did since it began (tail_early.h Seen), before its recording's own end adds to it
tail_early::Seen tail_seen() {
    tail_early::Seen s;
    uint64_t written = 0, guest = 0;
    memory_bind_counts(&written, &guest);
    s.written = written - tl.written0, s.guest = guest - tl.guest0, s.programs = draw_tail_programs() - tl.programs0;
    return s;
}
/// (at the gameplay upscale's dispatch, before it is recorded) The tail begins here: the recording so far ends as a part does
/// (maybe_split, with full barriers: its uniform updates before this place in the stream, its textures' and its write-backs,
/// to the queue), and the rest goes into a recording of its own
void tail_begin(VkCommandBuffer& cmd) {
    // (the uniform updates before this place: a parallel replay's dry pass listed the rest of the command buffer's already,
    // and this recording's draws read them from ub_limit on)
    const size_t ub_here = rmode == Mode::Worker ? std::min(ub_limit, ub_updates.size()) : ub_updates.size();
    end_pass(cmd);
    event_writes_flush(cmd, "barrier (events' held writes, before the recording's end)");
    flush_ub_updates(cmd, ub_here);
    texture_end_recording(cmd);
    memory_end_submission(cmd);
    full_barrier(cmd, "barrier (recording end)");
    submit_recording(*rec_cur, false, 0, true);   // (a part)
    ub_first_of_recording = ub_here;
    Recording& r = begin_recording(rec_cur_value);
    memory_begin_submission();
    full_barrier(r.cmd, "barrier (recording start)");
    rec_cur = &r;
    cmd = r.cmd;
    part_work = 0;
    rec_parts++;
    tl.on = true, tl.early = false, tl.rec = &r;
    tl.held.clear(), tl.words.clear();
    memory_bind_counts(&tl.written0, &tl.guest0);
    tl.programs0 = draw_tail_programs();
    draw_tail_watch(true);
}
void tail_at_dispatch(VkCommandBuffer& cmd, NvnState& s) {
    if (tl.on || !tail_wanted()) return;
    if (rmode == Mode::Worker ? !tail_here : (rmode != Mode::Serial || par_active.load(std::memory_order_relaxed))) return;
    if (!rec_cur || rec_parts + 1 >= kMaxParts || dump_on() || !upscale_dispatch(s)) return;
    tail_begin(cmd);
}
/// (a parallel replay with par_up: the command buffer from the gameplay upscale's dispatch on, recorded on this thread into the
/// recording after the segments, from the state the dry pass had there. Its first command begins the tail)
void tail_rest(VkCommandBuffer& cmd, uint64_t cb, const uint8_t* stop);
void tail_stats() {
    if (tl_stats.tails % 1200) return;
    const tail_early::Stats& t = tl_stats;
    logf("vk: the early tail (1200 tails): %llu went early, %llu of their %llu event signals written before their tail; the rest at "
         "their end, as before: %llu with %s, %llu %s, %llu %s, %llu %s, %llu %s, %llu %s",
         (unsigned long long)t.why[tail_early::EARLY], (unsigned long long)t.moved, (unsigned long long)t.signals,
         (unsigned long long)t.why[tail_early::WRITES], tail_early::why_name(tail_early::WRITES),
         (unsigned long long)t.why[tail_early::GUEST], tail_early::why_name(tail_early::GUEST),
         (unsigned long long)t.why[tail_early::PROGRAMS], tail_early::why_name(tail_early::PROGRAMS),
         (unsigned long long)t.why[tail_early::BAKE], tail_early::why_name(tail_early::BAKE),
         (unsigned long long)t.why[tail_early::EVENT], tail_early::why_name(tail_early::EVENT),
         (unsigned long long)t.why[tail_early::CUT], tail_early::why_name(tail_early::CUT));
    tl_stats = {};
}
/// The held words' buffers for their writes, in guest memory (the pool's import: what the game polls) and in the mirror the
/// replay's own reads would take (a read binding: no write-back, the page owes nothing). False: a word with no pool, or not in
/// whole words (EVENT)
bool tail_words() {
    tl.words.clear();
    for (const tail_early::Signal& g : tl.held.all()) {
        const BufferRef in_guest = gpu_buffer(g.gpu, 4);
        if (!in_guest || (in_guest.offset & 3)) return false;
        const BufferRef mirror = gpu_buffer_bind(g.gpu, 4, false);
        if (mirror && !(mirror.offset & 3) && (mirror.buffer != in_guest.buffer || mirror.offset != in_guest.offset))
            tl.words.push_back({mirror.buffer, mirror.offset, g.v});
        tl.words.push_back({in_guest.buffer, in_guest.offset, g.v});
    }
    return true;
}
/// The held words' writes into cb, one after another as their signals were (a barrier of the transfers between each two), after
/// everything before them (a full barrier), and made visible to the host that polls them
void tail_words_record(VkCommandBuffer cb) {
    full_barrier(cb, "barrier (the early tail's event words: after everything before them)");
    for (size_t i = 0; i < tl.words.size(); i++) {
        if (i) {
            VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1, d.pMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cb, &d);
        }
        vkCmdUpdateBuffer(cb, tl.words[i].buffer, tl.words[i].offset, 4, &tl.words[i].v);
    }
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1, d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}
/// (finish_recording, r the tail's recording, its uniform updates written and its textures' write-back recorded: baked, whether
/// that recorded anything) The tail judged. Early: its words' buffers found, written in r's upload command buffer when it is
/// submitted (tail_early_record). Not: its words written here, at its end, before its write-backs, as their signals wrote them
void tail_end(Recording& r, const tail_early::Seen& seen, bool baked) {
    tl.on = false;
    draw_tail_watch(false);
    tail_early::Seen s = seen;
    s.bake = baked;
    tail_early::Why why = tail_early::verdict(s);
    if (why == tail_early::EARLY && !tail_words()) why = tail_early::EVENT;
    tl_stats.judged(why, tl.held.all().size());
    tail_stats();
    if (why == tail_early::EARLY) {
        tl.early = true;
        return;
    }
    tl.words.clear();
    for (const tail_early::Signal& g : tl.held.all()) {
        end_pass(r.cmd);
        full_barrier(r.cmd);
        memory_bind_event_write(true);   // (as NOP_SIGNAL_EVENT writes it: written back with the recording's others)
        gpu_write(r.cmd, g.gpu, &g.v, 4);
        memory_bind_event_write(false);
    }
    tl.held.clear();
}
/// (submit_recording, after the recording's uploads are ended: texture_uploads_recorded) An early tail's words into r's upload
/// command buffer, after its uploads and ahead of its commands. True: r's upload command buffer goes to the queue for them.
/// A tail's recording submitted before it was judged (CUT) has them written at the end of its commands instead
bool tail_early_record(Recording& r) {
    if (tl.rec != &r) return false;
    if (tl.on) {   // (CUT, not judged at finish_recording: a safety net)
        tl.on = false;
        draw_tail_watch(false);
        static int told;
        if (told++ < 4) logf("vk: the early tail: a tail's recording was submitted before its end; its event words written after it");
        tl_stats.judged(tail_early::CUT, tl.held.all().size());
        tail_stats();
        if (tail_words()) tail_words_record(r.cmd);
        else
            for (const tail_early::Signal& g : tl.held.all()) guest_cpu_write(g.gpu, &g.v, 4);   // (as gpu_write's own outside the pools)
        tl.held.clear(), tl.words.clear(), tl.rec = nullptr;
        return false;
    }
    tl.rec = nullptr;
    if (!tl.early) return false;
    tl.early = false;
    tail_words_record(r.upload);
    tl.held.clear(), tl.words.clear();
    return true;
}

// ---- parallel replay: the threads and a command buffer's segments ----
// (RX_VK_PARALLEL=<threads recording a command buffer, the replay thread one of them>; 0, 1: off, the default. Segments
// recorded out of order left one-frame black scenes in play, 48-93 in a walk through Kakariko against none on the
// replay thread alone, at about the same frame rate here: 36-40 fps against 38-41)
// (hookshot) One cause of those is gone: the dry pass did not mark the targets of a clear (NOP_CLEAR_COLOR in replay()). With
// that, Castle Town's market and eight other places showed no black frame on 3 to 8 threads; the Kakariko walk was not run
// again, so it stays off. On a 9950X3D the replay's wall time in the market goes from 10.9 ms a frame to 7.75 on 3 threads.
// (Triforce) RX_VK_PARALLEL_AUTO=<n> (2 to 8): the parallel replay on <n> threads where it is the faster of the two, measured
// as the game runs. On three threads it is faster where the replay is the frame's limit and slower where the GPU is: at the
// Castle Town save 67.1 fps against 58.8 (RTX 4070 SUPER, 2560 x 1440, windowed), on a tour of Hyrule Field, Kakariko and
// Lake Hylia 59.5 and 61.9 against 64.8 and 66.7 (its recordings done sooner, the game's thread only waits longer for the
// GPU, and the dry pass and the hand-over are not free); the flicker counts of the frame log were the same either way.
// par_tune_present tries the other way for a moment every so often and keeps the one whose frames are shorter.
// RX_VK_PARALLEL=<n> still means always, as before.
// (Triforce) The hidden Debug tab's Replay helper threads row is the same as RX_VK_PARALLEL_AUTO=<n>, for a tester without
// an environment to set; the switches go over it.
// Its first runs as an experiment, off unless asked for (2026-11-11), gained nothing: 54.7 fps against 58.9 standing in Castle Town
// and 60.4 against 61.7 on the tour. The frame time of a scene with a crowd moves by 2 ms within seconds (one trial read 18.59
// and then 16.44 ms for the same way), more than the difference being measured, so the trials decide on noise
// (Triforce) The row's Automatic (replay_threads.h): the thread count this PC gets by its processors and its graphics
// driver, tried as the row's fixed counts are. What the row is unless the options file says otherwise, since the holes an
// audit of this path found were closed (that header lists them, and what is left). One line of the log at the start says
// what was chosen and by what; the tuner's trial lines below say when the threads are in use
int replay_threads_automatic() {
    static const int n = [] {
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &drv;
        if (dev.physical) vkGetPhysicalDeviceProperties2(dev.physical, &p2);
        VendorFacts facts;
        facts.vendor_id = p2.properties.vendorID;
        facts.driver_id = drv.driverID;
        facts.device_type = p2.properties.deviceType;
        facts.driver_info = drv.driverInfo;
        const unsigned logical = std::thread::hardware_concurrency();
        const bool amd_older = amd_older_compiler(facts);
        const int threads = replay_threads::automatic(logical, amd_older);
        logf("vk: replay helper threads: Automatic gives this PC %d (%u logical processors: %s). %s", threads, logical,
             replay_threads::why(logical, amd_older),
             threads ? "Tried for a moment every so often and kept where frames get shorter (the \"parallel replay, trial\" lines), "
                       "with DLSS or FSR on too (RX_VK_PARALLEL_TEMPORAL=0: not then). Off on the Debug tab, or RX_VK_PARALLEL=0: never"
                     : "The replay thread alone");
        return threads;
    }();
    return n;
}
/// (the log's line at the start for the other ways the count is chosen: a switch, a fixed count of the row, the row's Off)
int par_threads_told(int threads, const char* by) {
    if (threads >= 2) logf("vk: replay helper threads: %d (%s)", threads, by);
    else logf("vk: replay helper threads: none, the replay thread alone (%s)", by);
    return threads;
}
int par_threads_max() {
    static const int n = [] {
        if (const char* e = getenv("RX_VK_PARALLEL")) return par_threads_told(std::clamp(atoi(e), 0, 8), "RX_VK_PARALLEL: always");
        if (const char* e = getenv("RX_VK_PARALLEL_AUTO"))
            return par_threads_told(std::clamp(atoi(e), 0, 8), "RX_VK_PARALLEL_AUTO: where trials find them faster");
        // (Triforce) the hidden Debug tab's Replay helper threads row, as the options file had it at the start: what
        // RX_VK_PARALLEL_AUTO=<n> does, on 2 to 8 threads (vk_menu.h replay_threads_of), or Automatic's count. Automatic
        // unless the file says otherwise (replay_threads.h kDefaultRow), and either switch above goes over the row,
        // RX_VK_PARALLEL_AUTO=0 too
        if (replay_threads_at_start == replay_threads::kRowAutomatic) return replay_threads_automatic();
        return par_threads_told(replay_threads_of(replay_threads_at_start),
                                "the Debug tab's Replay helper threads row, replay_helper_threads in triforce_options.ini");
    }();
    return n;
}
bool par_auto() {
    static const bool v = !getenv("RX_VK_PARALLEL") && par_threads_max() >= 2;
    return v;
}
bool par_auto_on = false;   // (the replay thread's: the parallel replay is the one in use now)
int par_threads() {
    const int n = par_threads_max();
    return par_auto() && !par_auto_on ? 0 : n;
}
/// (Triforce) Whether a temporal upscaler (NVIDIA DLSS, AMD FSR 4 or 3.1) or DLSS's or FSR's frame generation is on (the
/// renderer's own frame generation takes nothing from the draws: vk_framegen.cpp). Their hooks in the draws are told things
/// in the order the draws are recorded and decide by it: the scene's camera is the first draw's into the largest target
/// (temporal_note_scene_draw, fsr3_inputs::note_scene_draw), the engine draws at "the first draw after the scene's last"
/// (temporal_pre_point), and the upscale's dispatch takes the camera noted before it. Recorded on several threads, that
/// order is theirs, not the stream's: so the dry pass tells those hooks in stream order, up to the first draw or dispatch
/// that takes what they noted, and from there the command buffer is recorded in order (stream_order.h, replay_handle)
bool par_temporal_hooks() { return temporal_wanted() || fsr3_inputs::wanted(); }
/// RX_VK_PARALLEL_TEMPORAL=0: as before, such a frame on the replay thread alone
bool par_temporal_on() {
    static const bool v = !getenv("RX_VK_PARALLEL_TEMPORAL") || atoi(getenv("RX_VK_PARALLEL_TEMPORAL")) != 0;
    return v;
}
/// Whether the frame's draws have to be recorded on the replay thread alone, whatever the helper threads' setting: with
/// those hooks on and RX_VK_PARALLEL_TEMPORAL=0. Asked for each command buffer, so the rows take effect as they are changed
bool par_stream_order_only() {
    const bool hooks = par_temporal_hooks();
    const bool only = hooks && !par_temporal_on();
    const int now = only ? 1 : hooks ? 2 : 0;
    static std::atomic<int> told{-1};
    if (par_threads_max() >= 2 && told.exchange(now, std::memory_order_relaxed) != now)
        logf(now == 1 ? "vk: replay helper threads: not used while DLSS or FSR is on, for upscaling or frame generation "
                        "(RX_VK_PARALLEL_TEMPORAL=0): the replay thread alone"
             : now == 2 ? "vk: replay helper threads: DLSS or FSR is on: the dry pass tells their hooks in stream order, and each "
                          "command buffer from the upscale (or the engine's point before the bloom) on is recorded in order"
                        : "vk: replay helper threads: neither DLSS nor FSR is on: they may be used");
    return only;
}
struct ParPool {
    std::mutex mu;
    std::condition_variable cv, done;
    std::deque<std::function<void()>> jobs;
    int pending = 0;
    bool started = false;
    // (Triforce) jobs.size(), stored with mu held wherever jobs changes: what a helper thread spinning for work reads without
    // mu (par_worker_main). And the helpers' waits for work since the start (the frame statistics' spin line, par_stats_line):
    // those that found a job in the spin, those that went to the condition variable, the time spun
    std::atomic<int> queued{0};
    std::atomic<int64_t> spun{0}, slept{0}, spin_ns{0};
};
ParPool par_pool;
void par_worker_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk replay worker");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        std::function<void()> fn;
        // (Triforce, the third performance audit's P6, after NX2navi's rw_main: helper_spin.h) with no job handed over, a spin
        // for one first (RX_VK_HELPER_SPIN_US, 100): the replay's next command buffer in segments often comes that soon, and its
        // segments are then begun without the condition variable's wake. The wait below decides as before
        const bool idle = par_pool.queued.load(std::memory_order_acquire) == 0;
        int64_t spent = 0;
        const bool found = idle && helper_spin::until([] { return par_pool.queued.load(std::memory_order_acquire) > 0; },
                                                      helper_spin::spin_ns(), &spent);
        {
            std::unique_lock lk(par_pool.mu);
            if (idle) {
                par_pool.spin_ns += spent;
                (found && !par_pool.jobs.empty() ? par_pool.spun : par_pool.slept)++;
            }
            par_pool.cv.wait(lk, [] { return !par_pool.jobs.empty(); });
            fn = std::move(par_pool.jobs.front());
            par_pool.jobs.pop_front();
            par_pool.queued.store((int)par_pool.jobs.size(), std::memory_order_relaxed);
        }
        fn();
        std::lock_guard lk(par_pool.mu);
        if (--par_pool.pending == 0) par_pool.done.notify_all();
    }
}

/// A recording slot for a segment a worker records (its command buffer only: uploads go to the shared one)
Recording& begin_segment_recording(uint64_t value) {
    Recording& r = recordings[recording_index];
    recording_index = (recording_index + 1) % kRecordings;
    if (r.value) {
        PROF(PROF_SLOTWAIT);
        recording_wait(r);
        gpu_time_collect(r);
        gprof_collect(r.slot);
    }
    if (occ_live) occ_before_begin(r);   // (Triforce: its counts read and written, RX_VK_OCC_QUERY)
    r.segment = true;   // (Triforce: timed on the GPU: replay_handle's gpu_time_begin once its uploads are begun, the job's gpu_time_end)
    r.seg_unbegun = false;
    vkResetCommandPool(dev.device, r.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(r.cmd, &bi);
    if (occ_live) occ_slot_begin(r);   // (Triforce: its block of occlusion queries reset in it, for the helper thread's draws)
    r.value = value;
    return r;
}

// ---- ((Triforce) RX_VK_SEG_SETUP_PAR=1, off by default: a segment's command buffers begun by the thread that records it ----
// The replay thread took each segment's slot and then reset its two command pools, began its command buffer and its upload one,
// reset its occlusion block in it and wrote their start timestamps, for every segment before any went out (part of the parallel
// replay's "setup", ~0.5 ms a frame at the owner's save). Here it takes the slots in order (the wait for the slot's last use, its
// GPU times and occlusion counts collected, its value and its occlusion serial, which give the order the readers go by), and the
// resets, the begins and the timestamps move to the start of the job, on the thread that records it, before anything goes into
// them. The same commands in the same command buffers: a slot's pools and command buffers are touched only by its job's thread
// from its start to its end, and it is submitted only after its end. A slot never recorded (its segment not reached, or no init
// command buffer) is left unbegun and unsubmitted; its pools are reset when it comes round, as a begun one's are. Only with the
// timestamp pool made (the first recording makes it)
bool seg_setup_par_on() {
    static const bool on = [] {
        const bool v = getenv("RX_VK_SEG_SETUP_PAR") && atoi(getenv("RX_VK_SEG_SETUP_PAR")) != 0;
        if (v) logf("vk: a segment's command buffers begun by the thread recording it (RX_VK_SEG_SETUP_PAR)");
        return v;
    }();
    return on && ts_pool;
}

Recording& begin_segment_slot(uint64_t value) {   // (begin_segment_recording without its command buffer's reset and begin)
    Recording& r = recordings[recording_index];
    recording_index = (recording_index + 1) % kRecordings;
    if (r.value) {
        PROF(PROF_SLOTWAIT);
        recording_wait(r);
        gpu_time_collect(r);
        gprof_collect(r.slot);
    }
    if (occ_live) {
        occ_before_begin(r);
        OccSlot& sl = occ_slots[r.slot];   // (occ_slot_begin's bookkeeping here, in slot order; its reset command in the job)
        if (!occ_host_reset) sl.used = 0;
        sl.serial = occ_serial_next.fetch_add(1, std::memory_order_relaxed) + 1;
        sl.armed = true;
    }
    r.segment = true;
    r.seg_unbegun = true;
    r.value = value;
    return r;
}
/// (the job's first step, on its thread: the slot's command buffers reset and begun, as begin_segment_recording and the
/// setup after it did; nothing when they were begun already)
void segment_begin_here(Recording& r) {
    if (!r.seg_unbegun) return;
    r.seg_unbegun = false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandPool(dev.device, r.pool, 0);
    vkBeginCommandBuffer(r.cmd, &bi);
    if (occ_live && !occ_host_reset) vkCmdResetQueryPool(r.cmd, occ_pool, (uint32_t)r.slot * kOccPerSlot, kOccPerSlot);
    vkResetCommandPool(dev.device, r.upload_pool, 0);
    vkBeginCommandBuffer(r.upload, &bi);
    gpu_time_begin(r);
}
/// (the draws and dispatches a segment is cut for: RX_VK_PARALLEL_SEG, 160 unless set, 32 at least)
int64_t par_seg_per() {
    static const int64_t per = getenv("RX_VK_PARALLEL_SEG") ? std::max(32, atoi(getenv("RX_VK_PARALLEL_SEG"))) : 160;
    return per;
}
/// (Triforce, 11-15) whether the replay thread records the segment it is to submit next when no helper thread has begun it
/// (replay_handle); RX_VK_PARALLEL_OWN=0: not, it waits for the helper threads as before
bool par_own_on() {
    static const bool v = !getenv("RX_VK_PARALLEL_OWN") || atoi(getenv("RX_VK_PARALLEL_OWN")) != 0;
    return v;
}

// (Triforce) What the parallel replay did, for the frame statistics' line (log_frame_stats, par_stats_line): command buffers
// in segments and on the replay thread alone (and why), segments, tails, and the time in them
// (Triforce, 11-15) and the parts of a command buffer in segments, so that a trial says why its frames were longer or shorter
// (par_tune_present): setup, from the dry pass's end until the threads have their segments (the part before submitted, each
// segment's recordings begun); first, from the command buffer's start until its first segment went to the GPU (the serial
// replay sends its first part after RX_VK_CHUNK draws); wake, from the hand-over until a helper thread began a segment;
// longest, each command buffer's longest segment (beside rec_ns / segments: how even the cut was); submit, the segments'
// vkQueueSubmit2 with its lock; post, the recording after them begun; own, the segments the replay thread recorded itself
struct ParStats {
    std::atomic<int64_t> segmented{0}, alone_off{0}, alone_debug{0}, alone_order{0}, alone_small{0};
    std::atomic<int64_t> segments{0}, tails{0}, work{0}, tail_work{0};
    std::atomic<int64_t> rec_ns{0}, tail_wait_ns{0}, wait_ns{0}, dry_ns{0};
    std::atomic<int64_t> setup_ns{0}, first_ns{0}, wake_ns{0}, wakes{0}, longest_ns{0}, submit_ns{0}, post_ns{0};
    std::atomic<int64_t> own_ns{0}, own{0};
    // ((Triforce) the replay thread's waits for a segment not yet done: those that ended in the spin, those that slept, the
    // time spun: helper_spin.h, replay_handle)
    std::atomic<int64_t> seg_spun{0}, seg_slept{0}, seg_spin_ns{0};
    std::atomic<int64_t> cut_work{0};   // (draws and dispatches in command buffers with two segments' worth or more, either way)
    std::atomic<bool> used{false};   // (since the last present)
};
ParStats par_st;
inline int64_t par_ns(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}
inline int64_t par_ns_between(std::chrono::steady_clock::time_point t0, std::chrono::steady_clock::time_point t1) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

struct Segment {
    const uint8_t *p, *end;
    const NvnState* s;
    size_t ub, cond;
    int64_t work = 0;    // (Triforce: the draws and dispatches before it in the command buffer: replay_stream_pos)
    uint64_t cb = 0;     // (the command buffer's replay: stream_cbs)
    bool told = false;   // (its draws' temporal hooks told by the dry pass: not again)
    bool tail = false;   // (from the first draw or dispatch that takes what they noted: after every segment before it)
    stream_order::InOrder* order = nullptr;   // (its command buffer's segments on the threads with a tail: their order)
    size_t index = 0;
};
/// One segment recorded into cmd from the state at its start (any thread: the replay thread's own included). The tail
/// waits for every segment before it, whose draws noted what it takes (the skinned draws, the stored positions).
/// Returns how long the recording took (ns), the tail's wait left out
int64_t record_segment(VkCommandBuffer& cmd, const Segment& g) {
    if (g.tail && g.order) {
        const auto tw = std::chrono::steady_clock::now();
        g.order->wait_before(g.index);
        par_st.tail_wait_ns += par_ns(tw);
    }
    const auto t0 = std::chrono::steady_clock::now();
    rmode = Mode::Worker;
    hooks_told = g.told;
    stream_cb = g.cb, stream_work = g.work;
    NvnState s = *g.s;
    ub_limit = g.ub, cond_at = g.cond;
    ub_gen++;
    // (Triforce, RX_VK_OCC_QUERY: the segment counts samples passed from its own resets on; the replay thread's query, open
    // in the pass a small command buffer's segment goes on in, ends first)
    if (occ_live) occ_segment_edge(cmd);
    full_barrier(cmd);   // (after what the command buffers before it did)
    replay(cmd, s, g.p, g.end);
    end_pass(cmd);
    if (occ_live) occ_segment_edge(cmd);   // (what this thread counted is the segment's: not the next one's, nor the replay's)
    full_barrier(cmd);
    rmode = Mode::Serial;
    hooks_told = false;
    ub_limit = SIZE_MAX;
    const int64_t took = par_ns(t0);
    par_st.rec_ns += took;
    if (g.order) g.order->done(g.index);
    return took;
}

/// (Triforce, 11-15) The frame just presented as the tuner sees it: its time and its parts since the last call (made at every
/// present), from the replay's counters: replay_threads::Frame
replay_threads::Frame par_frame_now(double ms) {
    int64_t d = 0, sk = 0, di = 0, pl = 0, made = 0, left = 0;
    draw_stats(&d, &sk, &di, &pl);
    barrier_stats(&made, &left);
    const int64_t v[15] = {prof_ns[PROF_SUBMIT], st_gpu_ns + st_gpu_up_ns, made, d + di, par_st.cut_work,
                           prof_ns[PROF_DRAW] + prof_ns[PROF_DISPATCH], par_st.segmented, par_st.dry_ns, par_st.setup_ns,
                           par_st.rec_ns, par_st.own_ns, par_st.wait_ns, par_st.submit_ns, par_st.first_ns, 0};
    static int64_t l[15];
    double x[15];
    for (int i = 0; i < 15; i++) x[i] = (double)(v[i] - l[i]), l[i] = v[i];
    replay_threads::Frame f;
    f.frame = ms, f.replay = x[0] / 1e6, f.gpu = x[1] / 1e6, f.barriers = x[2], f.draws = x[3], f.cut = x[4], f.op = x[5] / 1e6;
    f.segmented = x[6], f.dry = x[7] / 1e6, f.setup = x[8] / 1e6, f.rec = x[9] / 1e6, f.own = x[10] / 1e6;
    f.waited = (x[11] - x[10] - x[12]) / 1e6, f.submit = x[12] / 1e6, f.first = x[13] / 1e6;
    f.parallel = par_threads() >= 2;
    return f;
}
/// (a trial's line after it: each of its three parts' means)
void par_trial_parts(long long trial, const replay_threads::Part* part) {
    replay_threads::Frame m[3];
    for (int k = 0; k < 3; k++) m[k] = part[k].mean();
    const replay_threads::Frame& p = m[1].parallel ? m[1] : m[0];   // (the helper threads' part)
    logf("vk: parallel replay, trial %lld's parts (ms a frame, %s / %s / %s again): frame %.2f / %.2f / %.2f; the replay thread "
         "%.2f / %.2f / %.2f; the GPU %.2f / %.2f / %.2f; the rest of the frame %.2f / %.2f / %.2f; full barriers %.0f / %.0f / "
         "%.0f; draws and dispatches %.0f / %.0f / %.0f (%.0f / %.0f / %.0f in command buffers to cut). With the helper threads: "
         "%.1f command buffers in segments, dry pass %.2f, setup %.2f, recording %.2f on all threads (%.2f of it by the replay "
         "thread), the replay thread waited %.2f, submits %.2f, the first segment to the GPU %.2f ms after its command buffer began",
         trial, m[0].parallel ? "parallel" : "serial", m[1].parallel ? "parallel" : "serial", m[2].parallel ? "parallel" : "serial",
         m[0].frame, m[1].frame, m[2].frame, m[0].replay, m[1].replay, m[2].replay, m[0].gpu, m[1].gpu, m[2].gpu,
         m[0].frame - m[0].replay, m[1].frame - m[1].replay, m[2].frame - m[2].replay, m[0].barriers, m[1].barriers, m[2].barriers,
         m[0].draws, m[1].draws, m[2].draws, m[0].cut, m[1].cut, m[2].cut, p.segmented, p.dry, p.setup, p.rec, p.own, p.waited,
         p.submit, p.first);
}
/// (Triforce, 11-15) RX_VK_PARALLEL_RULE=0: the trials alone decide, as before (replay_threads.h rule)
bool par_rule_on() {
    static const bool v = !getenv("RX_VK_PARALLEL_RULE") || atoi(getenv("RX_VK_PARALLEL_RULE")) != 0;
    return v;
}
/// (Triforce, 11-16) RX_VK_PARALLEL_BACKOFF=0: a trial every 1,800 presents after one that kept the way, each as long as the
/// first, as before (replay_threads.h Tuner, the back-off)
bool par_backoff_on() {
    static const bool v = !getenv("RX_VK_PARALLEL_BACKOFF") || atoi(getenv("RX_VK_PARALLEL_BACKOFF")) != 0;
    return v;
}
/// At each present (the replay thread): the tuner, replay_threads::Tuner (its trials as they were here, and the rule that turns
/// the helper threads on without a trial where they plainly pay, or spares a trial where they cannot). Each counted frame's
/// parts are summed beside its time (par_frame_now), and a trial's line is followed by one with them (par_trial_parts)
void par_tune_present() {
    if (!par_auto()) return;
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    const replay_threads::Frame fr = par_frame_now(ms);
    static replay_threads::Tuner tuner = [] {
        replay_threads::Tuner t;
        t.use_rule = par_rule_on();
        t.backoff = par_backoff_on();
        t.threads = par_threads_max() + (par_own_on() ? 1 : 0);   // (the replay thread records segments too: replay_handle)
        return t;
    }();
    // (both ways are the replay thread alone with DLSS or FSR and RX_VK_PARALLEL_TEMPORAL=0: no trial and no rule until that ends)
    const replay_threads::Tuner::Step s = tuner.step(fr, par_auto_on, par_stream_order_only());
    if (s.flip) par_auto_on = !par_auto_on;
    if (s.ruled && s.rule_new) {
        const replay_threads::Frame& w = s.window;
        const replay_threads::Verdict& v = s.verdict;
        static int told;
        if (told++ < 40 || s.flip)
            logf("vk: parallel replay, the rule at present %lld: %s (a frame of %.2f ms on the replay thread alone: the replay "
                 "thread %.2f, %.0f%% of it, the GPU %.2f; %.0f of its %.0f draws and dispatches in command buffers to cut, %.2f us "
                 "each: %d threads would take %.2f ms off the replay thread, and the frame %.2f ms at most): %s",
                 (long long)tuner.frames(), s.flip ? "the helper threads on, without a trial"
                 : v.kind == replay_threads::Verdict::kOff ? "no trial while this holds" : "the trials decide",
                 w.frame, w.replay, 100.0 * v.share, w.gpu, w.cut, w.draws, w.draws > 0 ? 1000.0 * w.op / w.draws : 0.0,
                 tuner.threads, v.saved, v.gain, v.why);
    }
    // (Triforce, 11-16: with the back-off the trials are few, so each one is logged, and says whether it was a short one and
    // when the next may come)
    if (s.trial && (s.better || s.trials <= 8 || s.trials % 20 == 0 || tuner.backoff)) {
        char more[200] = "";
        if (s.recheck)
            snprintf(more, sizeof more, "; a short trial (%d frames the other way) found it shorter: a full trial follows",
                     replay_threads::Tuner::kTakeShort);
        else if (tuner.backoff)
            snprintf(more, sizeof more, "; %sthe next trial in %lld presents at the earliest, sooner if the scene changes",
                     s.short_trial ? "a short trial; " : "", (long long)(s.next - tuner.frames()));
        logf("vk: parallel replay, trial %lld: %s %.2f ms a frame, the other way %.2f, %s again %.2f: %s (%lld changes so far)%s",
             (long long)s.trials, par_auto_on != s.better ? "parallel" : "serial", s.a, s.b,
             par_auto_on != s.better ? "parallel" : "serial", s.c,
             s.better ? (par_auto_on ? "parallel from now on" : "serial from now on") : (s.ok ? "as it was" : "not measured (a load)"),
             (long long)s.changes, more);
        par_trial_parts((long long)s.trials, s.parts);
    }
    if (s.scene) {   // (Triforce, 11-16: the back-off's early trial)
        static int told;
        if (told++ < 40)
            logf("vk: parallel replay, the scene changed at present %lld (its draws, or the replay thread's or the GPU's time): the "
                 "next trial at present %lld at the earliest", (long long)tuner.frames(), (long long)s.next);
    }
}

/// Ends the process at once with this code, and nothing else: on Windows TerminateProcess, not _Exit. In the C runtime
/// _Exit is ExitProcess, which stops the other threads wherever they are and then runs every library's detach on this
/// thread: an upscaler's or a driver's detach that faults there turned a fatal error's end into a crash report of its
/// own, after the lines that said what had happened, and one that waits held the process open with its window. The
/// caller has written its lines out first (host->log_flush, fflush): nothing is flushed here. After NX2navi's
/// plat_terminate. tests/test_device_lost_exit.py
[[noreturn]] void end_process(int code) {
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
    std::_Exit(code);
}
// (the recorder thread's fatal end the same way: vk_recorder.h has no windows.h)
const bool rec_end_hook = (::mkwvk::rec::g_end_process_hook = [](int code) { end_process(code); }, true);

/// The replay cannot go on: logged, the host's queued log written out (as hos_fatal does: a device lost's report is many
/// lines, and the queue went with the process), written to stderr and the process ended (exit code 3). For a parallel
/// replay's init command buffer that could not be ended: images already made and published lost their first layout
/// change, and any later submission may use them
[[noreturn]] void replay_fatal(const char* why) {
    logf("vk: FATAL: %s", why);
    if (host && host->log_flush) host->log_flush();
    std::fprintf(stderr, "vk: FATAL: %s\n", why);
    std::fflush(stderr);
    end_process(3);
}
std::recursive_mutex par_mu;
uint64_t par_seq = 0;   // (the parallel replay's segments' order, across submissions: texture_set_segment; replay_handle and its early path)

// ---- RX_VK_SEG_EARLY (default off; =1: on. Measured 11-20 at the owner's Market spot, 3 helper threads: 79.0 -> 70.9 fps, the replay
// thread then records the last segment only after the pass, where the old path has it record one beside the helpers from the start): a big command buffer's segments handed to the
// helper threads while its dry pass still runs. Its draws and dispatches are counted first by the ops' headers (count_work: a walk,
// no replay), its boundaries planned from that count, and each segment goes out as the dry pass passes its end; the replay thread
// records the last one once the pass is over. The dry pass of a big command buffer (~0.5 ms each, three a frame at the Market) was
// time every helper waited for. With the temporal hooks on (a told tail found by the dry pass), the old path. The early tail's
// upscale dispatch (par_up) ends the handing out where the dry pass meets it: the open segment ends there, the rest is recorded
// after the segments, as before. texture_reupload_early (the CPU-rewritten textures' uploads moved into the recording before the
// segments) needs the whole pass's marks: not on this path, those uploads are the segments' own ----
bool seg_early_on() {
    static const bool v = [] {
        const char* e = getenv("RX_VK_SEG_EARLY");
        const bool on = e && atoi(e) != 0;   // (off by default: measured slower on this engine, 79.0 -> 70.9 fps at the owner's spot)
        logf("vk: parallel replay: big command buffers' segments handed out %s (RX_VK_SEG_EARLY=%d)",
             on ? "while their dry pass runs" : "after their dry pass", on ? 1 : 0);
        return on;
    }();
    return v;
}
/// The draws and dispatches of a command buffer's stream from h to its end, by the ops' headers alone (as the dry pass counts them)
int64_t count_work(const uint8_t* p) {
    int64_t n = 0;
    for (;;) {
        nvn_op_hdr h;
        std::memcpy(&h, p, sizeof h);
        if (h.op == NOP_END || h.size < sizeof h) return n;
        n += h.op == NOP_DRAW || h.op == NOP_DRAW_IDX || h.op == NOP_DRAW_INDIRECT || h.op == NOP_DRAW_MESH || h.op == NOP_DISPATCH ||
             h.op == NOP_DISPATCH_INDIRECT;
        p += h.size;
    }
}
// (the early tail's cut, par_up, where the dry pass found it the last time, by the command buffer's count of draws and dispatches: the
// segments are planned over the part before it, which is all they cover; the replay thread's only)
std::unordered_map<int64_t, int64_t> early_until;
const uint8_t* replay_handle_early(VkCommandBuffer& cmd, const uint8_t* h, bool may_stop, uint64_t cb, int threads, int nplan,
                                   const int64_t* plan, int64_t counted, ::mkwvk::segment_plan::Profile* prof,
                                   int64_t* reached) {
    NvnState& state = *cur_state;
    const auto t_dry0 = std::chrono::steady_clock::now();
    const size_t ub0 = ub_updates.size();
    par_points.clear(), par_conds_reset(), dry_work = 0;
    dry_marks_begin();
    dry_tail_find = false, par_tail_at = false;
    if (occ_live) occ_serial_give_up(cmd), occ_dry_cb = cb;   // (as replay_handle: the segments count from their own resets)
    dry_up_find = tail_wanted(), par_up_at = false;           // (the early tail: par_up)
    // what came before, with the mirrors' syncs in its uploads (ahead of every segment); submitted after the dry pass, whose own
    // uploads it takes too, and before any segment
    Recording& r = *rec_cur;
    end_pass(r.cmd);
    full_barrier(r.cmd);
    texture_reupload_early();   // (the textures the CPU keeps rewriting: their uploads in r's, not a segment's under the lock; par_active off)
    memory_sync_mirrors_before_segments();
    const auto t_set0 = std::chrono::steady_clock::now();
    uint64_t& seq = par_seq;   // (the segments' order, across submissions and both paths: a recording's uploads precede it)
    const uint64_t base = ++seq;
    seq += (uint64_t)nplan;
    memory_rb_segments_begin(base, (uint64_t)nplan);
    struct Job {
        Recording* r;
        Segment g;
        NvnState s;   // (its starting state: the dry pass's points move as it goes on)
        uint64_t order;
        int index = 0;
        std::atomic<bool> done{false};
        std::atomic<bool> taken{false};
        bool uploads = false;
        int inits = 0;
        int64_t rec = 0;
        std::chrono::steady_clock::time_point handed;
    };
    std::vector<std::unique_ptr<Job>> jobs;
    for (int k = 0; k < nplan; k++) {
        auto j = std::make_unique<Job>();
        if (seg_setup_par_on()) {   // ((Triforce) RX_VK_SEG_SETUP_PAR: begun by its job, run_job)
            j->r = &begin_segment_slot(rec_cur_value);
        } else {
            j->r = &begin_segment_recording(rec_cur_value);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkResetCommandPool(dev.device, j->r->upload_pool, 0);
            vkBeginCommandBuffer(j->r->upload, &bi);
            gpu_time_begin(*j->r);
        }
        j->order = base + (uint64_t)k;
        j->index = k;
        jobs.push_back(std::move(j));
    }
    std::vector<VkCommandBuffer> inits;
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        for (auto& jp : jobs)
            if (jp->r->init && vkResetCommandPool(dev.device, jp->r->init_pool, 0) == VK_SUCCESS &&
                vkBeginCommandBuffer(jp->r->init, &bi) == VK_SUCCESS)
                inits.push_back(jp->r->init);
            else
                break;
    }
    const bool early = inits.size() == jobs.size();   // (else: no segment, the dry pass then the whole here, below)
    if (early) {
        texture_segments_begin(inits.data(), inits.size());
        if (!par_pool.started) {
            par_pool.started = true;
            for (int i = 0; i < threads; i++) std::thread(par_worker_main).detach();
            logf("vk: parallel replay: big command buffers recorded on %d threads in segments of ~%lld draws and dispatches",
                 threads, (long long)par_seg_per());
        }
    }
    par_st.setup_ns += par_ns(t_set0);
    std::condition_variable seg_cv;
    std::mutex seg_mu;
    const auto run_job = [&seg_cv, &seg_mu](Job* j) {   // (as replay_handle's)
        texture_set_segment(j->order);
        texture_segment_worker(j->index);
        segment_begin_here(*j->r);   // ((Triforce) RX_VK_SEG_SETUP_PAR: its command buffers begun here, on its thread)
        texture_begin_recording(j->r->upload, j->r->value);
        draw_begin_recording(j->r->slot, j->r->cmd, j->r->value);
        VkCommandBuffer c = j->r->cmd;
        j->rec = record_segment(c, j->g);
        j->uploads = texture_uploads_recorded();
        j->inits = texture_segment_end(j->index);
        texture_segment_worker(-1);
        gpu_time_end(*j->r, j->uploads);
        vkEndCommandBuffer(j->r->upload);
        vkEndCommandBuffer(j->r->cmd);
        {
            std::lock_guard l(seg_mu);
            j->done = true;
        }
        seg_cv.notify_all();
    };
    // the dry pass, handing segments out as it passes their planned ends
    size_t open = 0;   // (the segment whose end is looked for)
    jobs[0]->s = state;
    jobs[0]->g = {h, nullptr, &jobs[0]->s, ub0, 0, 0, cb, false};
    SegEarlyHook hook;
    hook.wants = [&](int64_t work, const uint8_t* at) {
        return early && !par_up_at && (int)open + 1 < nplan && work >= plan[open] && at != jobs[open]->g.p;
    };
    hook.on_point = [&](const ParPoint& q) {
        if (!hook.wants(q.work, q.p)) return;
        Job* j = jobs[open].get();
        j->g.end = q.p;
        j->handed = std::chrono::steady_clock::now();
        {
            std::lock_guard lk(par_pool.mu);
            par_pool.jobs.push_back([j, &run_job] {
                if (j->taken.exchange(true)) return;   // (the replay thread has it)
                par_st.wake_ns += par_ns(j->handed), par_st.wakes++;
                run_job(j);
            });
            par_pool.pending++;
            par_pool.queued.store((int)par_pool.jobs.size(), std::memory_order_release);
        }
        par_pool.cv.notify_one();
        Job* n = jobs[++open].get();
        n->s = q.s;
        n->g = {q.p, nullptr, &n->s, q.ub, q.cond, q.work, cb, false};
    };
    const int64_t wait0 = par_lock_wait_ns.load(), locks0 = par_locks.load();
    if (early) {
        ::mkwvk::rec::segments_begin();   // (the recorder's ring run before any segment records directly: as replay_handle)
        par_active = true;                // (the dry pass's texture paths and marks take ParLock from here on)
        ub_growing.store(true, std::memory_order_release);
        seg_early_hook = &hook;
    }
    rmode = Mode::Dry;
    VkCommandBuffer none = cmd;
    suspend_ok = may_stop;
    replay(none, state, h);
    suspend_ok = false;
    rmode = Mode::Serial;
    seg_early_hook = nullptr;
    dry_tail_find = false;
    dry_up_find = false;
    ub_growing.store(false, std::memory_order_release);   // (nothing is appended from here on)
    const uint8_t* const stop = suspend_at;
    const bool up = par_up_at;
    const int64_t total = dry_work;
    par_st.dry_ns += par_ns(t_dry0);
    if (reached) *reached = up ? par_up.work : total;   // ((Triforce) for the next frame's plan of this place: replay_handle)
    par_st.work += total;
    if (total >= 2 * par_seg_per()) par_st.cut_work += total;
    static double dry_ms, seg_ms, segs_n, early_n, plan_n, total_n, tail_cut_n;
    plan_n += nplan, total_n += (double)total, tail_cut_n += up ? 1 : 0;
    {   // (a look at the first plans: where the boundaries were wanted and where the dry pass went)
        static int looked;
        if (looked < 12 && nplan >= 2 && ++looked) {
            char b[256];
            int o = 0;
            for (int k = 0; k + 1 < nplan && o < 200; k++) o += snprintf(b + o, sizeof b - o, " %lld", (long long)plan[k]);
            logf("vk: early plan: %d segments, targets%s; counted %lld, dry pass reached %lld (up %d at %lld), handed out %zu", nplan, b,
                 (long long)counted, (long long)total, (int)up, (long long)(up ? par_up.work : 0), open);
        }
    }
    static int64_t timed, whole_n;
    dry_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_dry0).count();
    if (up) {   // (the next time this command buffer comes, its segments are planned over the part before the cut)
        if (early_until.size() > 256) early_until.clear();
        early_until[counted] = par_up.work;
    } else {
        early_until.erase(counted);
    }
    const size_t used = open + 1;
    if (early)
        for (size_t k = used; k < jobs.size(); k++) texture_segment_end((int)k);   // (never reached: their init command buffers ended)
    if (used < 2 || !early) {   // (no boundary reached, or no init command buffer: the whole command buffer here, into r)
        if (early) {
            texture_segment_end(0);
            par_active = false;
            texture_segments_begin(nullptr, 0);
        }
        whole_n++, par_st.alone_small++;
        Segment whole = jobs[0]->g;
        whole.end = up ? par_up.p : stop;
        record_segment(cmd, whole);
        if (up) tail_rest(cmd, cb, stop);
        return stop;
    }
    Job* last = jobs[open].get();
    last->g.end = up ? par_up.p : stop;
    par_st.segmented++, par_st.segments += (int64_t)used, par_st.used = true;
    submit_recording(r, false, 0, true);   // (a part: what came before and the dry pass's uploads, ahead of every segment)
    const auto t_seg0 = std::chrono::steady_clock::now();
    bool init_lost = false;
    for (size_t k = 0; k < used; k++) {   // (in order, each as soon as it is done)
        Job* j = jobs[k].get();
        // (the next segment to submit, which no helper has begun: recorded here; the last one always is, never handed out)
        if (!init_lost && (k + 1 == used || par_own_on()) && !j->taken.exchange(true)) {
            const auto t_own = std::chrono::steady_clock::now();
            run_job(j);
            par_st.own_ns += par_ns(t_own), par_st.own++;
        }
        if (!j->done.load()) {
            int64_t spent = 0;
            const bool ended = helper_spin::until([j] { return j->done.load(std::memory_order_acquire); }, helper_spin::spin_ns(),
                                                  &spent);
            par_st.seg_spin_ns += spent;
            (ended ? par_st.seg_spun : par_st.seg_slept)++;
        }
        {
            std::unique_lock l(seg_mu);
            seg_cv.wait(l, [j] { return j->done.load(); });
        }
        if (j->inits < 0) init_lost = true;
        if (init_lost) continue;
        draw_flush_recording(j->r->slot);
        VkCommandBufferSubmitInfo cbi[3]{{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO},
                                         {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
        uint32_t n = 0;
        if (j->inits > 0) cbi[n++].commandBuffer = j->r->init;
        if (j->uploads) cbi[n++].commandBuffer = j->r->upload;
        cbi[n++].commandBuffer = j->r->cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = n;
        si.pCommandBufferInfos = cbi;
        VkSemaphoreSubmitInfo occ_sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};   // (RX_VK_OCC_QUERY: as replay_handle's)
        uint64_t occ_seq = 0;
        if (occ_live && rec_timeline) {
            occ_seq = ++rec_seq;
            occ_sig.semaphore = rec_timeline, occ_sig.value = occ_seq, occ_sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            si.signalSemaphoreInfoCount = 1, si.pSignalSemaphoreInfos = &occ_sig;
        }
        struct OccSeal {
            Recording& rec;
            uint64_t at;
            ~OccSeal() {
                if (occ_live && rec.submitted) occ_seal(rec, at);
            }
        } occ_sealed{*j->r, occ_seq};
        const auto t_sub = std::chrono::steady_clock::now();
        std::lock_guard lk(dev.present_mu);
        queue_flush_locked();
        VkSemaphoreSubmitInfo bind_wait;
        if (memory_mirror_bind_wait(&bind_wait)) si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &bind_wait;
        const VkResult res = vkQueueSubmit2(dev.queue, 1, &si, j->r->fence);
        par_st.submit_ns += par_ns(t_sub);
        if (j->index == 0) par_st.first_ns += par_ns(t_dry0);
        j->r->submitted = res == VK_SUCCESS;
        if (res != VK_SUCCESS) logf("vk: segment submit %llu failed (%d)", (unsigned long long)j->r->value, (int)res);
        if (res == VK_ERROR_DEVICE_LOST) device_lost();
    }
    {
        std::unique_lock lk(par_pool.mu);
        par_pool.done.wait(lk, [] { return par_pool.pending == 0; });
    }
    if (init_lost)
        replay_fatal("a parallel replay segment's init command buffer could not be ended: new images lost their first layout "
                     "change, and the work after it may use them");
    par_active = false;
    texture_segments_begin(nullptr, 0);
    {
        int64_t longest = 0;
        for (size_t k = 0; k < used; k++) longest = std::max(longest, jobs[k]->rec);
        par_st.longest_ns += longest;
    }
    // ((Triforce) what each segment took over its part of the command buffer, for the next frame's boundaries: segment_plan.h)
    if (prof && used >= 2 && used <= 16) {
        const double span = (double)std::max<int64_t>(1, up ? par_up.work : total);
        double from[16], to[16], ms[16];
        for (size_t k = 0; k < used; k++) {
            from[k] = (double)jobs[k]->g.work / span;
            to[k] = k + 1 < used ? (double)jobs[k + 1]->g.work / span : 1.0;
            ms[k] = (double)jobs[k]->rec / 1e6;
        }
        ::mkwvk::segment_plan::profile_add(*prof, from, to, ms, used, 0.25);
    }
    seg_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_seg0).count();
    par_st.wait_ns += par_ns(t_seg0);
    segs_n += (double)used, early_n += (double)(used - 1);
    static double wait_ms, locks_n;
    wait_ms += (par_lock_wait_ns.load() - wait0) / 1e6, locks_n += (double)(par_locks.load() - locks0);
    if (++timed % 240 == 0) {
        logf("vk: parallel replay, early segments (RX_VK_SEG_EARLY), a command buffer: dry pass %.2f ms (segments out during it %.1f), "
             "segments after it %.2f ms (%.1f of them), lock taken %.0f times, waited for %.2f ms (all threads); %lld recorded whole; planned %.1f segments over %.0f draws and dispatches, %.0f of 240 cut at the early tail",
             dry_ms / 240, early_n / 240, seg_ms / 240, segs_n / 240, locks_n / 240, wait_ms / 240, (long long)whole_n, plan_n / 240, total_n / 240, tail_cut_n);
        dry_ms = seg_ms = segs_n = early_n = wait_ms = locks_n = 0;
        plan_n = total_n = tail_cut_n = 0;
    }
    // what follows, after them all (as replay_handle)
    const auto t_post = std::chrono::steady_clock::now();
    texture_set_segment(++seq);
    Recording& nr = begin_recording(rec_cur_value);
    memory_table_segments_end();
    full_barrier(nr.cmd);
    rec_cur = &nr;
    cmd = nr.cmd;
    rec_parts += (int)used + 1;
    par_st.post_ns += par_ns(t_post);
    if (up) tail_rest(cmd, cb, stop);
    return stop;
}

/// A command buffer of the submission from h (its start, or where it stopped at a wait): on the replay thread alone
/// (RX_VK_PARALLEL off, a small one, dumps and profiles), or its dry pass there and its segments on the threads.
/// may_stop: up to a wait not met, which it returns (null: the command buffer done)
const uint8_t* replay_handle(VkCommandBuffer& cmd, const uint8_t* h, bool may_stop = false) {
    const int threads = par_threads();
    NvnState& state = *cur_state;
    suspend_at = nullptr;
    const uint64_t cb = ++stream_cbs;   // (Triforce: this command buffer's place, for replay_stream_pos)
    // ((Triforce) its place among this frame's command buffers on this thread, up to 63: the early segments' profile, segment_plan.h)
    thread_local uint64_t cbf_frame = ~0ull;
    thread_local int cbf_ord = 0;
    if (texture_frame_no() != cbf_frame) cbf_frame = texture_frame_no(), cbf_ord = 0;
    const int cb_in_frame = std::min(cbf_ord++, 63);
    thread_local int64_t cb_reached[64] = {};   // (how far the dry pass of each place went last time: the early plan's total)
    // (Triforce: and with the temporal hooks on, one that starts with the engine's work pending from the one before (the
    // engine drew before the bloom, the upscale's dispatch still to come) on the replay thread alone too: the dry pass's
    // telling ends at the first draw or dispatch that takes what they noted, which such a command buffer may not have)
    const bool hooks = threads >= 2 && par_temporal_hooks();
    const bool debug = dump_on() || gprof_on() || probe_serial();
    // ((Triforce) and in the early tail, which stays one command buffer: tail_on)
    if (threads < 2 || !rec_cur || debug || tail_on() || par_stream_order_only() || (hooks && temporal_pre_pending())) {
        (threads < 2 || !rec_cur ? par_st.alone_off : debug ? par_st.alone_debug : par_st.alone_order)++;
        stream_cb = cb, stream_work = 0;
        suspend_ok = may_stop;
        replay(cmd, state, h);
        suspend_ok = false;
        if (stream_work >= 2 * par_seg_per()) par_st.cut_work += stream_work;   // (Triforce: what the helpers could have cut)
        return suspend_at;
    }
    // (RX_VK_SEG_EARLY) a big command buffer's segments handed out while its dry pass runs, planned from its count of draws and
    // dispatches (even parts of ~par_seg_per() each, at most par_seg_most()): not with the temporal hooks' told tail
    if (seg_early_on() && !hooks) {
        const int64_t counted = count_work(h);
        const auto u = early_until.find(counted);   // (the early tail's cut found here last time: the part before it)
        const int64_t total0 = u != early_until.end() && u->second > 0 && u->second < counted ? u->second : counted;
        // ((Triforce) a command buffer often stops at a wait the game has not met yet and goes on later: the dry pass covers only
        // the part before it. Planned over what the dry pass of this place in the frame before reached, when that is shorter; the
        // count of the headers alone put the later boundaries past the stop, never reached, and the replay thread recorded the rest)
        const int64_t total = cb_reached[cb_in_frame] > 0 && cb_reached[cb_in_frame] < total0 ? cb_reached[cb_in_frame] : total0;
        // ((Triforce) one segment a recorder at most (the helper threads and this thread, which records the last), one each
        // RX_VK_SEG_EARLY_PER draws and dispatches (default 128), the boundaries where the command buffer of this place in the
        // frames before spent equal recording time (segment_plan.h; RX_VK_SEG_BALANCE=0: equal counts). RX_VK_SEG_ONE_ROUND=0:
        // as before, one each par_seg_per() up to par_seg_most())
        static const bool one_round = !(getenv("RX_VK_SEG_ONE_ROUND") && atoi(getenv("RX_VK_SEG_ONE_ROUND")) == 0);
        static const bool balance = !(getenv("RX_VK_SEG_BALANCE") && atoi(getenv("RX_VK_SEG_BALANCE")) == 0);
        static const int64_t per = getenv("RX_VK_SEG_EARLY_PER") ? std::max(32, atoi(getenv("RX_VK_SEG_EARLY_PER"))) : 128;
        thread_local ::mkwvk::segment_plan::Profile profiles[64];
        const int n = one_round ? ::mkwvk::segment_plan::segment_count(total, per, ::mkwvk::segment_plan::recorders(threads, true),
                                                                       std::min(par_seg_most(), 16), true)
                                : (int)std::clamp<int64_t>(total / std::max<int64_t>(par_seg_per(), 1), 1, std::min(par_seg_most(), 16));
        if (n >= 2) {
            int64_t plan[16];
            ::mkwvk::segment_plan::Profile* const prof = balance ? &profiles[cb_in_frame] : nullptr;
            if (prof) ::mkwvk::segment_plan::balanced_targets(*prof, n, total, plan);
            else for (int k = 0; k + 1 < n; k++) plan[k] = total * (k + 1) / n;
            for (int k = 1; k + 1 < n; k++) plan[k] = std::max(plan[k], plan[k - 1] + 1);   // (rising: the hook looks for them in order)
            return replay_handle_early(cmd, h, may_stop, cb, threads, n, plan, counted, prof, &cb_reached[cb_in_frame]);
        }
    }
    // the dry pass: the state at every 64 draws and dispatches, the render conditions, the uniform updates (Triforce: and
    // the temporal hooks told, up to the first draw or dispatch that takes what they noted: par_tail)
    const auto t_dry0 = std::chrono::steady_clock::now();
    const NvnState start = state;
    const size_t ub0 = ub_updates.size();
    par_points.clear(), par_conds_reset(), dry_work = 0;
    dry_marks_begin();
    dry_tail_find = hooks, par_tail_at = false;
    if (occ_live) occ_serial_give_up(cmd), occ_dry_cb = cb;   // (Triforce, RX_VK_OCC_QUERY: the segments count from here, each
                                                               // from its own resets; the dry pass's reports placed at cb's)
    dry_up_find = tail_wanted(), par_up_at = false;   // ((Triforce) the early tail: par_up)
    rmode = Mode::Dry;
    VkCommandBuffer none = cmd;
    suspend_ok = may_stop;
    replay(none, state, h);
    suspend_ok = false;
    rmode = Mode::Serial;
    dry_tail_find = false;
    dry_up_find = false;
    const uint8_t* const stop = suspend_at;   // (the segments end there)
    const int64_t total = dry_work;
    static double dry_ms, seg_ms, segs_n;
    static int64_t timed;
    dry_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_dry0).count();
    par_st.dry_ns += par_ns(t_dry0);
    // (segments of ~RX_VK_PARALLEL_SEG draws and dispatches (default 160), taken in order by the threads and submitted
    // as each is done with those before it: the GPU starts on the first while the rest are recorded)
    // (Triforce: over the part before the tail, the tail one more: stream_order.h cuts)
    const int64_t per = par_seg_per();
    if (total >= 2 * per) par_st.cut_work += total;
    // ((Triforce) the early tail: the gameplay upscale's dispatch (par_up) ends the segments, the told part's tail with them, and
    // the rest is recorded after them on this thread, where the early tail begins (tail_rest). A told part ending at that same
    // dispatch leaves its tail segment empty: none)
    const bool up = par_up_at;
    const bool tail = par_tail_at && !(up && par_tail.p == par_up.p);
    const int64_t until = tail ? par_tail.work : up ? par_up.work : total;
    std::vector<Segment> seg;
    seg.push_back({h, nullptr, &start, ub0, 0, 0, cb, hooks});
    const int most = par_seg_most();
    for (const size_t i : stream_order::cuts(par_points, until, per, most - (tail ? 1 : 0),
                                             [](const ParPoint& q) { return q.work; })) {
        const ParPoint& at = par_points[i];
        seg.back().end = at.p;
        seg.push_back({at.p, nullptr, &at.s, at.ub, at.cond, at.work, cb, hooks});
    }
    if (tail) {
        seg.back().end = par_tail.p;
        seg.push_back({par_tail.p, nullptr, &par_tail.s, par_tail.ub, par_tail.cond, par_tail.work, cb, false, true});
    }
    seg.back().end = up ? par_up.p : stop;
    par_st.work += total, par_st.tail_work += tail ? total - until : 0;
    if (seg.size() == 1 || (tail && seg.size() == 2)) {   // (one segment, and its tail: here, as the dry pass left the
        par_st.alone_small++;                                 // reports and conditions)
        for (const Segment& g : seg) record_segment(cmd, g);
        if (up) tail_rest(cmd, cb, stop);
        return stop;
    }
    par_st.segmented++, par_st.segments += (int64_t)seg.size(), par_st.tails += tail ? 1 : 0, par_st.used = true;
    stream_order::InOrder in_order(seg.size());   // (the tail waits for the segments before it: record_segment)
    if (tail)
        for (size_t k = 0; k < seg.size(); k++) seg[k].order = &in_order, seg[k].index = k;
    if (!par_pool.started) {
        par_pool.started = true;
        for (int i = 0; i < threads; i++) std::thread(par_worker_main).detach();
        logf("vk: parallel replay: big command buffers recorded on %d threads in segments of ~%lld draws and dispatches",
             threads, (long long)per);
    }
    // what came before, to the GPU now (its uploads included); then each segment into a recording of its own (its
    // command buffer and uploads), recorded in order by the threads and submitted in order as they are done
    const auto t_set0 = std::chrono::steady_clock::now();   // (Triforce: the setup's time, ParStats)
    Recording& r = *rec_cur;
    end_pass(r.cmd);
    full_barrier(r.cmd);
    // (Triforce, 11-16) the textures the CPU keeps rewriting, looked at here: their uploads in r's too, not a segment's under the
    // recording lock (vk_texture.cpp texture_reupload_early, reupload_early.h; after the dry pass, which marked what this
    // command buffer draws into)
    texture_reupload_early();
    memory_sync_mirrors_before_segments();   // (in r's uploads, ahead of every segment)
    submit_recording(r, false, 0, true);   // (a part: no write-backs of its own, they are recorded at the recording's end)
    uint64_t& seq = par_seq;   // (the segments' order, across submissions and both paths: a recording's uploads precede it)
    const uint64_t base = ++seq;
    seq += seg.size();
    memory_rb_segments_begin(base, seg.size());   // (their order in the queue: memory_rb_refresh)
    struct Job {
        Recording* r;
        Segment g;
        uint64_t order;
        int index = 0;
        std::atomic<bool> done{false};
        bool uploads = false;
        int inits = 0;   // (texture_segment_end's)
        int64_t rec = 0;   // (Triforce: its recording's time, record_segment's)
        std::chrono::steady_clock::time_point handed;   // (when the threads were given it)
        std::atomic<bool> taken{false};   // (a thread records it: a helper, or the replay thread when it is the next to submit)
    };
    std::vector<std::unique_ptr<Job>> jobs;
    for (size_t k = 0; k < seg.size(); k++) {
        auto j = std::make_unique<Job>();
        if (seg_setup_par_on()) {   // ((Triforce) RX_VK_SEG_SETUP_PAR: begun by its job, run_job)
            j->r = &begin_segment_slot(rec_cur_value);
        } else {
            j->r = &begin_segment_recording(rec_cur_value);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkResetCommandPool(dev.device, j->r->upload_pool, 0);
            vkBeginCommandBuffer(j->r->upload, &bi);
            gpu_time_begin(*j->r);   // (Triforce: its command buffers timed on the GPU as a recording's are; ended by the job)
        }
        j->g = seg[k];
        j->order = base + k;
        j->index = (int)k;
        jobs.push_back(std::move(j));
    }
    std::vector<VkCommandBuffer> inits;   // (each one's init command buffer, begun)
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        for (auto& jp : jobs)
            if (jp->r->init && vkResetCommandPool(dev.device, jp->r->init_pool, 0) == VK_SUCCESS &&
                vkBeginCommandBuffer(jp->r->init, &bi) == VK_SUCCESS)
                inits.push_back(jp->r->init);
            else
                break;
    }
    if (inits.size() != jobs.size()) {
        // (one could not be begun: no worker has started, and the segments are not recorded. The whole command buffer is
        // recorded here as one segment, as a short one is, into what follows (the post-part as below): its new images are
        // made in queue order. The segments' recordings go unsubmitted; their slots come round again as unused ones)
        static int told;
        if (told++ < 8) logf("vk: parallel replay: a segment's init command buffer could not be begun: recorded on the replay thread");
        texture_set_segment(++seq);
        Recording& rest = begin_recording(rec_cur_value);
        full_barrier(rest.cmd);
        rec_cur = &rest;
        cmd = rest.cmd;
        rec_parts++;
        Segment whole = seg[0];
        // (Triforce: the part the dry pass told, then the tail; with the early tail's par_up the segments end at that dispatch)
        whole.end = seg.back().tail ? seg.back().p : up ? seg.back().end : stop;
        whole.order = nullptr;
        record_segment(cmd, whole);
        if (seg.back().tail) {
            Segment rest_of = seg.back();
            rest_of.order = nullptr;
            record_segment(cmd, rest_of);
        }
        if (up) tail_rest(cmd, cb, stop);   // ((Triforce) the early tail)
        return stop;
    }
    texture_segments_begin(inits.data(), inits.size());
    const auto t_seg0 = std::chrono::steady_clock::now();
    par_st.setup_ns += par_ns_between(t_set0, t_seg0);
    const int64_t wait0 = par_lock_wait_ns.load(), locks0 = par_locks.load();
    // (Triforce, 11-15) RX_VK_RECORDER: what the replay thread put in the recorder's ring (the segments' timestamps among it) run
    // before any segment is recorded directly; nothing goes into it again until par_active is let go (vk_recorder.h)
    ::mkwvk::rec::segments_begin();
    par_active = true;
    std::condition_variable seg_cv;
    std::mutex seg_mu;
    // (a segment's job, on whichever thread takes it first: Job::taken)
    const auto run_job = [&seg_cv, &seg_mu](Job* j) {
        texture_set_segment(j->order);
        texture_segment_worker(j->index);
        segment_begin_here(*j->r);   // ((Triforce) RX_VK_SEG_SETUP_PAR: its command buffers begun here, on its thread)
        texture_begin_recording(j->r->upload, j->r->value);
        draw_begin_recording(j->r->slot, j->r->cmd, j->r->value);
        VkCommandBuffer c = j->r->cmd;
        j->rec = record_segment(c, j->g);
        j->uploads = texture_uploads_recorded();
        j->inits = texture_segment_end(j->index);   // (after its last new image)
        texture_segment_worker(-1);
        gpu_time_end(*j->r, j->uploads);
        vkEndCommandBuffer(j->r->upload);
        vkEndCommandBuffer(j->r->cmd);
        {
            std::lock_guard l(seg_mu);
            j->done = true;
        }
        seg_cv.notify_all();
    };
    {
        std::lock_guard lk(par_pool.mu);
        for (auto& jp : jobs) {
            Job* j = jp.get();
            j->handed = std::chrono::steady_clock::now();
            par_pool.jobs.push_back([j, run_job] {
                if (j->taken.exchange(true)) return;   // (the replay thread has it, below)
                par_st.wake_ns += par_ns(j->handed), par_st.wakes++;
                run_job(j);
            });
            par_pool.pending++;
        }
        // ((Triforce) for the helper threads spinning for work: they see the jobs without the wake that follows)
        par_pool.queued.store((int)par_pool.jobs.size(), std::memory_order_release);
    }
    par_pool.cv.notify_all();
    bool init_lost = false;
    for (auto& jp : jobs) {   // (in order, each as soon as it is done)
        Job* j = jp.get();
        // (Triforce, 11-15) the next segment to submit, which no helper thread has begun: recorded here, where the replay thread
        // only waited before (its core is the one kept for the replay, HOS_RESERVE_CORE, and idle meanwhile). It is the one the
        // queue needs next: none it submits after it waits on it, and a job waits only on the segments before it (the tail,
        // InOrder), all of them done by now. RX_VK_PARALLEL_OWN=0: the helper threads alone, as before
        if (!init_lost && par_own_on() && !j->taken.exchange(true)) {
            const auto t_own = std::chrono::steady_clock::now();
            run_job(j);
            par_st.own_ns += par_ns(t_own), par_st.own++;
        }
        if (!j->done.load()) {   // ((Triforce) a helper records it: a spin for its end first, RX_VK_HELPER_SPIN_US, helper_spin.h)
            int64_t spent = 0;
            const bool ended = helper_spin::until([j] { return j->done.load(std::memory_order_acquire); }, helper_spin::spin_ns(),
                                                  &spent);
            par_st.seg_spin_ns += spent;
            (ended ? par_st.seg_spun : par_st.seg_slept)++;
        }
        {
            std::unique_lock l(seg_mu);
            seg_cv.wait(l, [j] { return j->done.load(); });
        }
        if (j->inits < 0) init_lost = true;   // (its init command buffer could not be ended: the replay stops, below)
        if (init_lost) continue;              // (nothing from it on is submitted)
        draw_flush_recording(j->r->slot);
        VkCommandBufferSubmitInfo cb[3]{{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO},
                                        {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
        uint32_t n = 0;
        if (j->inits > 0) cb[n++].commandBuffer = j->r->init;   // (new images made in it or after it: first in the queue)
        if (j->uploads) cb[n++].commandBuffer = j->r->upload;
        cb[n++].commandBuffer = j->r->cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = n;
        si.pCommandBufferInfos = cb;
        // (Triforce, RX_VK_OCC_QUERY: the segment's recording holds the queries of the reports it measured, and is submitted
        // here, not by submit_recording: it signals the recordings' timeline with the next number too (the replay thread,
        // replay_mu, in queue order: the batch before it is flushed first) and is sealed by it once dev.present_mu is let
        // go (declared before the lock), so its counts are read and written when the GPU is done. Before, its slot was
        // never sealed and its counts were dropped as "never sent" when the slot came round: 83% of them at Castle Town.
        // Off: nothing of this, the submit as before)
        VkSemaphoreSubmitInfo occ_sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        uint64_t occ_seq = 0;
        if (occ_live && rec_timeline) {
            occ_seq = ++rec_seq;
            occ_sig.semaphore = rec_timeline, occ_sig.value = occ_seq, occ_sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            si.signalSemaphoreInfoCount = 1, si.pSignalSemaphoreInfos = &occ_sig;
        }
        struct OccSeal {
            Recording& rec;
            uint64_t at;
            ~OccSeal() {
                if (occ_live && rec.submitted) occ_seal(rec, at);
            }
        } occ_sealed{*j->r, occ_seq};
        const auto t_sub = std::chrono::steady_clock::now();
        std::lock_guard lk(dev.present_mu);
        queue_flush_locked();
        VkSemaphoreSubmitInfo bind_wait;   // (Triforce: after the memory of the mirrors' new pieces is bound, vk_memory.cpp)
        if (memory_mirror_bind_wait(&bind_wait)) si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &bind_wait;
        const VkResult res = vkQueueSubmit2(dev.queue, 1, &si, j->r->fence);
        par_st.submit_ns += par_ns(t_sub);
        if (j->index == 0) par_st.first_ns += par_ns(t_dry0);   // (Triforce: the GPU's first work of this command buffer)
        j->r->submitted = res == VK_SUCCESS;
        if (res != VK_SUCCESS) logf("vk: segment submit %llu failed (%d)", (unsigned long long)j->r->value, (int)res);
        if (res == VK_ERROR_DEVICE_LOST) device_lost();
    }
    {
        std::unique_lock lk(par_pool.mu);
        par_pool.done.wait(lk, [] { return par_pool.pending == 0; });
    }
    if (init_lost)   // (every worker done: none is left with a job)
        replay_fatal("a parallel replay segment's init command buffer could not be ended: new images lost their first layout "
                     "change, and the work after it may use them");
    par_active = false;
    texture_segments_begin(nullptr, 0);
    {
        int64_t longest = 0;
        for (auto& jp : jobs) longest = std::max(longest, jp->rec);
        par_st.longest_ns += longest;
    }
    seg_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_seg0).count();
    par_st.wait_ns += par_ns(t_seg0);
    segs_n += (double)seg.size();
    static double wait_ms, locks_n;
    wait_ms += (par_lock_wait_ns.load() - wait0) / 1e6, locks_n += (double)(par_locks.load() - locks0);
    if (++timed % 240 == 0) {
        logf("vk: parallel replay, a command buffer: dry pass %.2f ms, segments %.2f ms (%.1f of them), lock taken %.0f "
             "times, waited for %.2f ms (all threads)", dry_ms / 240, seg_ms / 240, segs_n / 240, locks_n / 240, wait_ms / 240);
        dry_ms = seg_ms = segs_n = wait_ms = locks_n = 0;
    }
    // what follows, after them all (the uniform updates are written to guest memory at the submission's end)
    const auto t_post = std::chrono::steady_clock::now();
    texture_set_segment(++seq);
    Recording& nr = begin_recording(rec_cur_value);
    // (Triforce) the page table's changes of the segments, which the queue ran in the segments' order, once more in the
    // order they were made: in this recording's uploads, after them all (vk_memory.cpp par_table_log)
    memory_table_segments_end();
    full_barrier(nr.cmd);
    rec_cur = &nr;
    cmd = nr.cmd;
    rec_parts += (int)seg.size() + 1;
    par_st.post_ns += par_ns(t_post);
    if (up) tail_rest(cmd, cb, stop);   // ((Triforce) the early tail: from the gameplay upscale's dispatch on, into this recording)
    return stop;
}
void tail_rest(VkCommandBuffer& cmd, uint64_t cb, const uint8_t* stop) {
    Segment g{par_up.p, stop, &par_up.s, par_up.ub, par_up.cond, par_up.work, cb};
    tail_here = true;
    record_segment(cmd, g);
    tail_here = false;
}

// ---- the rx_* functions ----

// ---- a benchmark (RX_VK_BENCH=<present>[,<times>], default 50 times): the frame before that present replayed again and
// again, the game held (its next present waits) and the recordings kept (the replayed value the host frees them by
// is held at the frame's start); the replay's CPU time each time, logged. The same frame every time: what a change to
// the replay costs or saves, without the title scenes' differences. ----
std::vector<std::vector<uint64_t>> frame_submits;   // (this frame's submits' handles)
uint64_t frame_first_value = 0;
std::atomic<uint64_t> replayed_hold{0};             // (0: none)

void bench_replay(const std::vector<uint64_t>& handles) {
    const uint64_t value = pushed.load();   // (reached already: no slot waits on it)
    rec_value_now = value;
    ub_first_of_recording = ub_updates.size();
    Recording& first = begin_recording(value);
    memory_begin_submission();
    full_barrier(first.cmd);
    rec_cur = &first, rec_cur_value = value, rec_parts = 0, part_work = 0;
    VkCommandBuffer cmd = first.cmd;
    for (uint64_t h : handles)
        if (h) replay(cmd, *cur_state, reinterpret_cast<const uint8_t*>(h));
    Recording& r = *rec_cur;
    rec_cur = nullptr;
    end_pass(r.cmd);
    event_writes_flush(r.cmd, "barrier (events' held writes, before the recording's end)");   // ((Triforce) as finish_recording)
    flush_ub_updates(r.cmd);
    texture_end_recording(r.cmd), memory_end_submission(r.cmd);
    full_barrier(r.cmd);
    submit_recording(r, false);
}

void bench_frame(int64_t presents) {
    static int64_t at = -2;
    static int times = 50;
    if (at == -2) {
        at = -1;
        if (const char* e = getenv("RX_VK_BENCH")) {
            at = atoll(e);
            if (const char* c = strchr(e, ',')) times = std::max(1, atoi(c + 1));
        }
    }
    if (at < 0 || presents + 1 != at || frame_submits.empty()) return;
    replayed_hold = std::max<uint64_t>(1, frame_first_value - 1);
    int64_t draws0 = 0, sk = 0, di = 0, pl = 0;
    draw_stats(&draws0, &sk, &di, &pl);
    std::vector<double> ms;
    int64_t prof0[PROF_COUNT];
    prof_full++;   // (every phase timed, not one draw in eight: the A/B differences are small)
    std::memcpy(prof0, prof_ns, sizeof prof0);
    const size_t ub_before = ub_updates.size();
    queue_flush();
    for (int i = 0; i < times; i++) {
        vkDeviceWaitIdle(dev.device);
        ub_updates.resize(ub_before);   // (the last time's uniform updates are written: as if pruned)
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& h : frame_submits) bench_replay(h);
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    vkDeviceWaitIdle(dev.device);
    prof_full--;
    int64_t draws1 = 0;
    draw_stats(&draws1, &sk, &di, &pl);
    replayed_hold = 0;
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double v : ms) sum += v;
    logf("vk: bench: the frame before present %lld (%zu submits, %lld draws) replayed %d times: median %.2f ms, min %.2f, "
         "mean %.2f, max %.2f", (long long)at, frame_submits.size(), (long long)((draws1 - draws0) / times), times,
         sorted[sorted.size() / 2], sorted.front(), sum / times, sorted.back());
    static const char* names[PROF_COUNT] = {"submit", "present", "draw", "descriptors", "texture lookups", "uploads",
                                            "pipelines", "programs", "commands", "dispatch", "memory sync",
                                            "queue submit", "slot wait"};
    char line[600];
    int n = snprintf(line, sizeof line, "vk: bench: a replay's time (ms):");
    for (int i = 2; i < PROF_COUNT; i++)
        n += snprintf(line + n, sizeof line - n, " %s %.2f%s", names[i], (prof_ns[i] - prof0[i]) / (double)times / 1e6,
                      i + 1 < PROF_COUNT ? "," : "");
    logf("%s", line);
}

/// A slow submit (over 15 ms): what took its time, logged (60 of them). The stalls in it are exact (PROF, PROF_EXACT); the
/// per-draw phases' cheap part is sampled, one draw in eight (PROF_HOT): an estimate, exact with RX_VK_PROF_ALL=1
struct SlowWatch {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    int64_t prof0[PROF_COUNT];
    int64_t ms0[4];   // (hookshot: memory sync's parts and the lock wait, at the start)
    const char* what;
    explicit SlowWatch(const char* w) : what(w) {
        std::memcpy(prof0, prof_ns, sizeof prof0);
        for (int i = 0; i < 3; i++) ms0[i] = memsync_part_ns[i].load(std::memory_order_relaxed);
        ms0[3] = memsync_lock_wait_ns.load(std::memory_order_relaxed);
    }
    ~SlowWatch() {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        static int logged;
        if (ms < 15.0 || logged >= 80 || presents_now < 600) return;   // (after the first load)
        logged++;
        static const char* names[PROF_COUNT] = {"submit", "present", "draw", "descriptors", "texture lookups",
                                                "uploads", "pipelines", "programs", "commands", "dispatch",
                                                "memory sync", "queue submit", "slot wait"};
        char line[600];
        int k = snprintf(line, sizeof line, "vk: slow %s: %.1f ms:", what, ms);
        for (int i = 2; i < PROF_COUNT; i++) {
            const double d = (prof_ns[i] - prof0[i]) / 1e6;
            if (d >= 1.0) k += snprintf(line + k, sizeof line - k, " %s %.1f", names[i], d);
        }
        {   // (hookshot: which part of "memory sync" it was: the pools' refresh [and its lock waits], the capture, the written pages collected.
            // Counted across threads, so a part another thread spent meanwhile shows too)
            const double a = (memsync_part_ns[0].load(std::memory_order_relaxed) - ms0[0]) / 1e6,
                         b = (memsync_part_ns[1].load(std::memory_order_relaxed) - ms0[1]) / 1e6,
                         c = (memsync_part_ns[2].load(std::memory_order_relaxed) - ms0[2]) / 1e6,
                         w = (memsync_lock_wait_ns.load(std::memory_order_relaxed) - ms0[3]) / 1e6;
            if (a + b + c >= 1.0 || w >= 1.0)
                k += snprintf(line + k, sizeof line - k, " [memory sync: maps %.1f (lock waits %.1f), capture %.1f, collect %.1f]", a, w, b, c);
        }
        logf("%s", line);
    }
};

/// The memory work before a submission's recording: the pools mapped, its capture taken, the other writes collected
/// (replay_mu held). In the order of the submits: one behind its queue's stopped one (submit_job) has it done as it comes.
void submit_memory_work(uint64_t value) {
    PROF(PROF_MEMSYNC);
    const auto m0 = std::chrono::steady_clock::now();
    memory_sync();
    const auto m1a = std::chrono::steady_clock::now();
    memsync_part_ns[0] += std::chrono::duration_cast<std::chrono::nanoseconds>(m1a - m0).count();
    // (the capture takes only the mirrored pools' pages, the collection the rest: skipping it whenever a capture was
    // taken, which with the replay thread is at every submit, left every other CPU write uncollected. Textures never
    // saw them: MK World's font cache kept its first glyphs (the menus lost letters), CPU-streamed terrain went stale)
    const auto m1 = std::chrono::steady_clock::now();
    memory_take_capture(value, completed());
    const auto m2 = std::chrono::steady_clock::now();
    memory_collect_written();
    const auto m3 = std::chrono::steady_clock::now();
    memsync_part_ns[1] += std::chrono::duration_cast<std::chrono::nanoseconds>(m2 - m1).count();
    memsync_part_ns[2] += std::chrono::duration_cast<std::chrono::nanoseconds>(m3 - m2).count();
    {   // (every 600 submits: what the memory work before a submit's recording cost)
        static double a, b, c;
        static int64_t k;
        a += std::chrono::duration<double, std::milli>(m1 - m0).count();
        b += std::chrono::duration<double, std::milli>(m2 - m1).count();
        c += std::chrono::duration<double, std::milli>(m3 - m2).count();
        if (++k % 600 == 0) {
            logf("vk: a submit's memory work (ms): maps %.2f, capture taken %.2f, writes collected %.2f", a / 600, b / 600, c / 600);
            a = b = c = 0;
        }
    }
}

// ---- the game's queues ordered by their events (RX_VK_EVENT_WAITS=0: every submit in the order it came, as before) ----
// MK World's two worker queues pass stages back and forth with events: a command buffer waits (nvnCommandBufferWaitEvent)
// for the other queue's to reach a stage, several times a frame, and the two are submitted one after the other.
// Replayed one after the other on the one Vulkan queue, the first went past waits for work the second had not done
// yet: what that work writes was read before it was written (55% of the waits). Now a submission is replayed as it
// comes as far as its waits allow, and what it recorded goes to the GPU; at a wait not met it stops, and its queue's
// later submits wait behind it (their memory work done in turn). Once another queue's work has signalled the event
// it goes on from there, as on the console, where it would have waited on the GPU. A present, a released texture or
// program, a wait from another thread for the replay, or RX_VK_HOLD_MS (default 3) with one stopped, and the stopped
// ones go on as their waits allow, the oldest past its wait whenever all of them wait. The timeline is signalled as
// far as no older submission is unfinished. (Holding a whole submission until the other queue's came, as this did
// first, had the registrations made meanwhile reach its draws before it: the menus' scenes in rectangles of fog.)
struct LaterSubmit {
    std::vector<uint64_t> handles;
    uint64_t value = 0;
};
struct QueueRun {   // (a queue stopped at a wait)
    bool waiting = false;
    std::vector<uint64_t> handles;   // (the submission it stopped in)
    int cb = 0;                      // (its command buffer, and the wait there)
    const uint8_t* p = nullptr;
    uint64_t value = 0;
    std::deque<LaterSubmit> later;   // (its queue's submits since)
    bool blocked() const { return waiting || !later.empty(); }
    uint64_t oldest() const { return waiting ? value : later.front().value; }
    uint64_t at_present = 0;         // (Triforce-dev: the oldest() it was stopped with at the last present; drain_stale)
};
std::unordered_map<uint64_t, QueueRun> qruns;
std::atomic<int> blocked_n{0}, drain_waiters{0};    // (queues stopped, for the other threads; those in wait_queue_empty)
std::chrono::steady_clock::time_point blocked_since;
uint64_t done_max = 0, signaled = 0;                // (the newest value recorded whole; the timeline's last signal)
int64_t st_stops, st_resumed, st_forced_waits, st_forced[4], st_resume_captures;   // (made to go by: presents, releases, waits, time)
double st_stopped_max_ms;
extern int64_t st_present_waits, st_present_waits_out;
bool event_waits_on() {
    static const bool v = !getenv("RX_VK_EVENT_WAITS") || atoi(getenv("RX_VK_EVENT_WAITS")) != 0;
    return v;
}
int hold_ms() {
    // (50: with the replay caught up, the worker queue's submission came up to 16 ms before the one that lets it on;
    // at 3 ms it went past its waits ~80 times a race. The game never waits for it before that submit)
    static const int v = getenv("RX_VK_HOLD_MS") ? std::max(0, atoi(getenv("RX_VK_HOLD_MS"))) : 50;
    return v;
}
const uint8_t* op_next(const uint8_t* p) {
    nvn_op_hdr h;
    std::memcpy(&h, p, sizeof h);
    return p + h.size;
}
nvn_p_wait_event wait_at(const uint8_t* p) {   // (p: a wait op)
    nvn_op_hdr h;
    std::memcpy(&h, p, sizeof h);
    return payload<nvn_p_wait_event>(p + 8, h.size - 8);
}
bool wait_met_at(const uint8_t* p) {
    const nvn_p_wait_event w = wait_at(p);
    ev_cpu_apply();
    return event_wait_met(event_value_in(ev_values, w.gpu), w.mode, w.value);
}
uint64_t oldest_unfinished() {
    uint64_t m = UINT64_MAX;
    for (const auto& [queue, q] : qruns)
        if (q.blocked()) m = std::min(m, q.oldest());
    return m;
}
// ---- (from PR 430, YunaraDebugs) Releases while queues are stopped: kept until the stopped work has gone on, instead of making it
// go at once (drain 1). Made to go early, a queue's work ran before the work it waits for, and about every 13th frame in Kokiri a
// pass read what was not there yet: the ground fog thinner for that frame, the light dimmed. The stopped work runs as before, at
// its wait, a present or hold_ms; the release follows it. RX_VK_RELEASE_DRAIN=1: as before ----
void do_texture_released(const nvn_tex* t);
void do_program_released(const nvn_prog* p);
std::mutex held_mu;   // (its own: sync_order_mu is held while whole submissions replay, and every release waited for them)
std::vector<nvn_tex> held_tex_releases;    // (held_mu)
std::vector<nvn_prog> held_prog_releases;  // (held_mu)
std::atomic<int64_t> st_held_releases{0};
bool hold_releases() {
    static const bool v = !getenv("RX_VK_RELEASE_DRAIN") || atoi(getenv("RX_VK_RELEASE_DRAIN")) == 0;
    return v;
}
void update_blocked() {
    int n = 0;
    for (const auto& [queue, q] : qruns) n += q.blocked();
    if (n && !blocked_n.load()) blocked_since = std::chrono::steady_clock::now();
    blocked_n = n;
    if (!n) {   // (the stopped work has gone on: its releases now)
        std::vector<nvn_tex> ts;
        std::vector<nvn_prog> ps;
        {
            std::lock_guard hl(held_mu);
            ts.swap(held_tex_releases);
            ps.swap(held_prog_releases);
        }
        for (const nvn_tex& t : ts) do_texture_released(&t);
        for (const nvn_prog& p : ps) do_program_released(&p);
    }
}
void log_order_stats() {
    logf("vk: queues ordered by their events: %lld submissions stopped at a wait, %lld went on once it was reached, %lld "
         "waits gone past; made to go by presents %lld, releases %lld, waits for the replay %lld, %d ms %lld (stopped "
         "%.2f ms at most); waits met %lld, not met %lld; captures at a CPU event's wait %lld", (long long)st_stops, (long long)st_resumed,
         (long long)st_forced_waits, (long long)st_forced[0], (long long)st_forced[1], (long long)st_forced[2], hold_ms(),
         (long long)st_forced[3], st_stopped_max_ms, (long long)st_waits_met, (long long)st_waits_unmet,
         (long long)st_resume_captures);
    if (st_present_waits)
        logf("vk: presents that waited for their frame's signal: %lld (%lld in vain)", (long long)st_present_waits,
             (long long)st_present_waits_out);
}

// ---- a burst: the pieces replayed one after the other with no memory work between them (a submit's job: its own
// piece and those of the stopped queues it lets on, then theirs again: MK World's two queues pass five or six stages
// back and forth a frame) go into one recording. Each piece had its own, and its memory sync, write-backs and submit:
// ~8 a frame. The burst's recording is tagged with the newest value among them (what is used in it is kept until
// that one is done; the timeline is signalled at its end as far as allowed, as for any). ----
bool burst_open = false;
uint64_t burst_value = 0;
void burst_begin(uint64_t value) {
    burst_open = event_waits_on() && !dump_on();
    burst_value = value;
    for (const auto& [queue, q] : qruns) {
        if (q.waiting) burst_value = std::max(burst_value, q.value);
        if (!q.later.empty()) burst_value = std::max(burst_value, q.later.back().value);
    }
}
void finish_recording(uint64_t value);
void burst_end() {
    if (!burst_open) return;
    burst_open = false;
    std::lock_guard lk(replay_mu);
    if (rec_cur) finish_recording(burst_value);
}

/// The recording being made: ended and submitted, the timeline signalled as far as no older submission is unfinished
void finish_recording(uint64_t value) {
    (void)value;
    Recording& r = *rec_cur;
    rec_cur = nullptr;
    end_pass(r.cmd);
    // ((Triforce) RX_VK_EVENT_BARRIER: the events' held writes before the write-backs, which copy their words to the game's memory)
    event_writes_flush(r.cmd, "barrier (events' held writes, before the recording's end)");
    // ((Triforce) the early tail: what its commands did, taken before the recording's own end adds its uniform updates; judged
    // once the textures' write-back is recorded, before the write-backs: tail_end)
    const bool tail = tail_on() && tl.rec == &r;
    const tail_early::Seen seen = tail ? tail_seen() : tail_early::Seen{};
    flush_ub_updates(r.cmd);
    const uint64_t baked = work_between();   // ((Triforce, P4) cmd_work.h: this thread's commands, and the queue's submits)
    texture_end_recording(r.cmd);
    if (tail) tail_end(r, seen, work_between() != baked);
    memory_end_submission(r.cmd);
    full_barrier(r.cmd, "barrier (recording end)");
    const uint64_t oldest = oldest_unfinished();
    const uint64_t at = oldest == UINT64_MAX ? done_max : std::min(done_max, oldest - 1);
    const bool signal = at > signaled;
    if (signal) signaled = at;
    submit_recording(r, signal, at);
}

// ---- (hookshot) the synchronous replay on the core kept for the replay ----
// With RX_VK_ASYNC off (the default) a submission is replayed on the thread that submits it, or on whichever thread goes on
// with a queue stopped at an event wait, and the core the host keeps free of the game's threads for the replay thread
// (host->reserved_cpus: hos_reserved_cpus, the last core of the fastest kind) had nothing on it. Now a thread runs there
// while it replays and takes a submission's capture, and goes back to its own processors after: every thread that replays,
// none by name; with RX_VK_ASYNC=1 the replay thread is there already. Ryzen 9 3900X, Kokiri Forest: see the PR.
// RX_VK_REPLAY_PIN=0: off. (Only preferring the core, as the thread's ideal processor with every processor allowed, gained
// nothing there: the pin is what helps)
bool async_on();
// (Windows only: on Linux a thread inherits the affinity of the one that makes it, and the back end's threads made on
// first use inside a replay (the capture's copy helpers, the shader workers, the pool imports...) would all have stayed on
// the reserved core. Windows gives a new thread the process's)
uint64_t replay_pin_mask() {
#if defined(_WIN32)
    static const uint64_t m = [] {
        const char* e = getenv("RX_VK_REPLAY_PIN");
        const uint64_t r = !(e && atoi(e) == 0) && host->reserved_cpus ? host->reserved_cpus() : 0;
        if (r) logf("vk: replays run on logical processors %#llx while they replay (RX_VK_REPLAY_PIN=0: off)", (unsigned long long)r);
        return r;
    }();
    return m;
#else
    return 0;
#endif
}
thread_local int replay_pin_depth = 0;
thread_local DWORD_PTR replay_pin_prev = 0;
// ((Triforce) RX_VK_REPLAY_PRIO, replay_prio.h: the thread's own priority while the outermost pin has it raised; kPrioNone: not raised)
constexpr int kPrioNone = -1000;
thread_local int replay_prio_prev = kPrioNone;
struct ReplayPin {
    bool counted = false;
    ReplayPin() {
        if (async_on()) return;
        counted = true;
        if (replay_pin_depth++) return;   // (the outermost one: a submit's replay inside its capture's)
        if (const uint64_t m = replay_pin_mask()) replay_pin_prev = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)m);
        // ((Triforce) raised while it replays, its own priority back after: replay_prio.h)
        if (const int want = replay_prio_level()) {
            const int now = GetThreadPriority(GetCurrentThread());
            if (replay_prio::raise(want, now) && SetThreadPriority(GetCurrentThread(), want)) replay_prio_prev = now;
        }
    }
    ~ReplayPin() {
        if (!counted || --replay_pin_depth) return;
        if (replay_prio_prev != kPrioNone) SetThreadPriority(GetCurrentThread(), replay_prio_prev), replay_prio_prev = kPrioNone;
        if (replay_pin_prev) SetThreadAffinityMask(GetCurrentThread(), replay_pin_prev), replay_pin_prev = 0;
    }
    ReplayPin(const ReplayPin&) = delete;
    ReplayPin& operator=(const ReplayPin&) = delete;
};

/// A submission's command buffers from command buffer cb (op p; null: its start) on, into recordings of its own,
/// submitted: true when it finished; false when it stopped at a wait not met (may_stop), its place kept in qruns.
/// memory_work: its memory work first (else done as it came)
bool run_submit(const uint64_t* handles, int n, uint64_t value, uint64_t queue, bool memory_work, int cb = 0,
                const uint8_t* p = nullptr, bool may_stop = false) {
    std::lock_guard lk(replay_mu);
    ReplayPin pin;
    SlowWatch slow("submit");
    PROF(PROF_SUBMIT);
    struct FtSpan {   // (lab, RX_VK_FT)
        bool on;
        uint64_t v;
        ~FtSpan() { if (on) vk_ft("replay< value %llu", (unsigned long long)v); }
    } ft_span{vk_ft_on(), value};
    if (ft_span.on) vk_ft("replay> value %llu queue %#llx, %d handles%s", (unsigned long long)value, (unsigned long long)queue, n, p ? " (resumed)" : "");
    if (memory_work) {
        if (frame_submits.empty()) frame_first_value = value;
        frame_submits.emplace_back(handles, handles + std::max(n, 0));
        submit_memory_work(value);
    }
    if (burst_open && rec_cur) {   // (the burst's recording goes on: what the piece before did, done first)
        end_pass(rec_cur->cmd);
        full_barrier(rec_cur->cmd, "barrier (a burst's next submit start)");
        use_queue_state(queue);
    } else {
        const uint64_t tag = burst_open ? std::max(burst_value, value) : value;
        use_queue_state(queue);
        ub_prune(completed());
        rec_value_now = tag;
        ub_first_of_recording = ub_updates.size();
        Recording& first = begin_recording(tag);
        memory_begin_submission();
        full_barrier(first.cmd, "barrier (recording start)");
        rec_cur = &first, rec_cur_value = tag, rec_parts = 0, part_work = 0;
        // ((Triforce) tex_warm.h, RX_VK_TEX_WARM: a game's submit after a light frame (a loading screen): the textures the
        // game registered are made and uploaded here, in this recording, before the area's first frame asks for them all)
        if (memory_work && !p)   // (the heavier of the frame before and this frame so far: not in an area's first frame once it draws)
            texture_warm_step(std::max(last_frame_work.load(std::memory_order_relaxed), frame_work.load(std::memory_order_relaxed)));
    }
    VkCommandBuffer cmd = rec_cur->cmd;
    const uint8_t* stop = nullptr;
    for (; cb < n; cb++, p = nullptr) {
        if (!handles[cb]) continue;
        stop = replay_handle(cmd, p ? p : reinterpret_cast<const uint8_t*>(handles[cb]), may_stop);
        if (stop) break;
    }
    if (stop) {
        QueueRun& q = qruns[queue];
        q.waiting = true;
        q.handles.assign(handles, handles + n);
        q.cb = cb, q.p = stop, q.value = value;
        if (st_stops++ < 12) {
            const nvn_p_wait_event w = wait_at(stop);
            logf("vk: submission %llu (queue %#llx) stopped at command buffer %d's wait for event %#llx %s %u (has %u)",
                 (unsigned long long)value, (unsigned long long)queue, cb, (unsigned long long)w.gpu,
                 w.mode == 0 ? "==" : ">=", w.value, event_value_in(ev_values, w.gpu));
        }
        if (st_stops % 6000 == 0) log_order_stats();
    } else {
        done_max = std::max(done_max, value);
        rec_open_values.push_back(value);
        if (queue && rec_timeline) rec_open_qvalues.push_back(value);   // (RX_VK_FENCE_POLL_FAST: value_queue has it: submit_q)
    }
    if (!burst_open) finish_recording(value);   // (a burst's: at its end)
    if (stop) return false;
    if (pushed.load() < value) pushed = value;
    st_submits++;
    if (!burst_open && dump_pending()) wait_value(value), dump_write();
    return true;
}
void do_submit(const uint64_t* handles, int n) {   // (RX_VK_ASYNC=0: on the submitting thread, in its order)
    run_submit(handles, n, ++q_values, 0, true);
}

/// A stopped queue on (forced: past the wait it stopped at), then its later submits, as far as their waits allow
void async_wait_done(uint64_t value);   // ((Triforce) async_rules.h: a submission handed to the replay thread replayed whole)
void resume_queue(uint64_t queue, bool forced) {
    QueueRun& q = qruns[queue];
    for (;;) {
        if (q.waiting) {
            // (hookshot) a wait for an event the CPU signals, met: the game wrote what this work reads after it submitted it,
            // up to the signal (OoT's presentation queue waits for its frame's data this way, 1 frame in 12 here). The
            // console's GPU reads memory after its wait; the capture taken at the submit had the frame before's: the fog and
            // exposure of the frame before, for a frame. A capture now, for what follows the wait. RX_VK_RESUME_CAPTURE=0: none
            static const bool resume_capture = !getenv("RX_VK_RESUME_CAPTURE") || atoi(getenv("RX_VK_RESUME_CAPTURE")) != 0;
            bool cpu_event = false;
            if (resume_capture && !forced) {
                std::lock_guard ek(ev_cpu_mu);
                cpu_event = cpu_events.count(wait_at(q.p).gpu) != 0;
            }
            if (cpu_event) {
                std::lock_guard lk(replay_mu);
                if (rec_cur) finish_recording(burst_open ? burst_value : q.value);   // (its mirrors synced again after)
                memory_capture_now(q.value, completed());
                st_resume_captures++;
            }
            q.waiting = false;
            std::vector<uint64_t> handles = std::move(q.handles);
            q.handles.clear();
            const uint8_t* from = forced ? op_next(q.p) : q.p;
            (forced ? st_forced_waits : st_resumed)++;
            const uint64_t resumed = q.value;   // (run_submit keeps the queue's place again when it stops: the value before)
            if (!run_submit(handles.data(), (int)handles.size(), resumed, queue, false, q.cb, from, true)) return;
            async_wait_done(resumed);
            continue;
        }
        if (q.later.empty()) return;
        LaterSubmit s = std::move(q.later.front());
        q.later.pop_front();
        if (!run_submit(s.handles.data(), (int)s.handles.size(), s.value, queue, false, 0, nullptr, true)) return;
        async_wait_done(s.value);
    }
}
/// The stopped queues whose events are reached go on, until none can
void schedule() {
    for (bool moved = true; moved;) {
        moved = false;
        for (auto& [queue, q] : qruns)
            if (q.waiting && wait_met_at(q.p)) {
                resume_queue(queue, false);
                moved = true;
            }
    }
}
/// Every stopped queue on now (why: an index of st_forced): in turn as their waits allow; when all of them wait, the
/// oldest past its wait
void drain(int why) {
    if (oldest_unfinished() == UINT64_MAX) return;
    st_forced[why]++;
    st_stopped_max_ms = std::max(st_stopped_max_ms,
                                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - blocked_since).count());
    burst_begin(0);
    for (;;) {
        schedule();
        uint64_t best = UINT64_MAX, bq = 0;
        for (const auto& [queue, q] : qruns)
            if (q.blocked() && q.oldest() < best) best = q.oldest(), bq = queue;
        if (best == UINT64_MAX) break;
        resume_queue(bq, qruns[bq].waiting);
    }
    burst_end();
    update_blocked();
}

/// (Triforce-dev) A present on the replay thread, with the game a frame ahead: the stopped queues whose waits are met go
/// on, and only those stopped since the present before (the same submission still waiting: nothing but a present makes a
/// load's go) are made to go past their waits. What stopped since is mostly the NEXT frame's work, handed over early and
/// waiting for a signal that comes after this present: drain(0) made it go ahead of its signaller, ~11,000 waits gone
/// past in a 150 s Market walk against 0 on the submitting thread, and the 3D scene black for a frame now and then.
/// RX_VK_PRESENT_DRAIN=2
void drain_stale() {
    burst_begin(0);
    schedule();
    std::vector<uint64_t> stale;
    for (const auto& [queue, q] : qruns)
        if (q.blocked() && q.at_present == q.oldest()) stale.push_back(queue);
    if (!stale.empty()) {
        st_forced[0]++;
        for (;;) {
            schedule();
            uint64_t best = UINT64_MAX, bq = 0;
            for (const uint64_t queue : stale) {
                const QueueRun& q = qruns[queue];
                if (q.blocked() && q.oldest() < best) best = q.oldest(), bq = queue;
            }
            if (best == UINT64_MAX) break;
            resume_queue(bq, qruns[bq].waiting);
        }
    }
    for (auto& [queue, q] : qruns) q.at_present = q.blocked() ? q.oldest() : 0;
    burst_end();
    update_blocked();
}

// ---- (hookshot) the same on the submitting thread, the default. do_submit replayed each queue's submission as it came and went
// past every wait: 2 of 3 were not met. A lit pass waiting for another queue's work that had not been submitted yet read what
// that work makes before it was made, and for a frame lit surfaces came out black (a race with the game's threads: 3-9% of
// frames with frame generation off, less the less they overlapped). The stopped work goes on from whichever thread's submit or
// signal lets it, or a present, a wait for it, or hold_ms (sync_watch: nothing else may come while the game polls).
// RX_VK_SYNC_WAITS=0: as before ----
bool async_on();
/// RX_VK_PER_QUEUE_FENCES=0: a fence waits for every queue's work up to it, stopped work forced on, as before
bool per_queue_fences_off() {
    static const bool v = getenv("RX_VK_PER_QUEUE_FENCES") && atoi(getenv("RX_VK_PER_QUEUE_FENCES")) == 0;
    return v;
}
// sync_order_mu counts its sections: `entered` once a holder has it, `left` as it lets it go (both changed only by the holder, so
// entered - left is 1 while one is held, else 0). Every recording of the sync-waits replay (and so every write-back registration)
// is made inside such a section: submit_q, present, the drains and settles. A fast fence poll keeps the number of the section in
// progress as a ticket (order_ticket_take); a readback waits only until that section has left (order_ticket_wait), not for
// whichever replay holds the lock when it reads. See invalidate for why that is the order it needs
struct OrderMutex {
    std::mutex mu;
    std::atomic<uint64_t> entered{0}, left{0};
    void lock() {
        mu.lock();
        entered.fetch_add(1, std::memory_order_seq_cst);
    }
    bool try_lock() {
        if (!mu.try_lock()) return false;
        entered.fetch_add(1, std::memory_order_seq_cst);
        return true;
    }
    void unlock();
};
OrderMutex sync_order_mu;   // (before replay_mu, always; all of the order's state: qruns, burst_*)
std::atomic<uint64_t> order_ticket{0};   // (the newest section a fast poll answered during; see order_ticket_take)
std::atomic<int> order_waiters{0};
std::mutex order_wait_mu;
std::condition_variable order_wait_cv;
void OrderMutex::unlock() {
    left.fetch_add(1, std::memory_order_seq_cst);   // (after the section's registrations: release)
    mu.unlock();
    // (a readback waiting for this section: woken. Its waiters++ and this load are both seq_cst, as are this left++ and its
    // check, so either it sees left or this sees it waiting and the notify comes after it is in the wait: no lost wake-up)
    if (order_waiters.load(std::memory_order_seq_cst)) {
        { std::lock_guard lk(order_wait_mu); }
        order_wait_cv.notify_all();
    }
}
/// a fast poll: the section in progress now (0, or the one past the last that left: none to wait for), kept as the newest
/// ticket (a maximum: tickets of polls on other threads only make a readback wait longer, never shorter)
void order_ticket_take() {
    const uint64_t t = sync_order_mu.entered.load(std::memory_order_seq_cst);
    uint64_t cur = order_ticket.load(std::memory_order_relaxed);
    while (cur < t && !order_ticket.compare_exchange_weak(cur, t, std::memory_order_seq_cst, std::memory_order_relaxed)) {}
}
/// until the section of the newest ticket has left (at once when it has: two loads)
void order_ticket_wait() {
    const uint64_t t = order_ticket.load(std::memory_order_seq_cst);
    if (sync_order_mu.left.load(std::memory_order_acquire) >= t) return;
    std::unique_lock lk(order_wait_mu);
    order_waiters.fetch_add(1, std::memory_order_seq_cst);
    order_wait_cv.wait(lk, [t] { return sync_order_mu.left.load(std::memory_order_seq_cst) >= t; });
    order_waiters.fetch_sub(1, std::memory_order_seq_cst);
}
bool sync_waits_on() {
    static const bool v = !getenv("RX_VK_SYNC_WAITS") || atoi(getenv("RX_VK_SYNC_WAITS")) != 0;
    return v && event_waits_on() && !async_on() && !dump_on();
}
void sync_drain(int why) {
    if (!blocked_n.load()) return;
    std::lock_guard lk(sync_order_mu);
    drain(why);
    update_blocked();
}
/// (hookshot) The stopped queues whose waits are met go on; none is forced past its wait
void sync_settle() {
    if (!blocked_n.load()) return;
    std::lock_guard lk(sync_order_mu);
    burst_begin(0);
    schedule();
    burst_end();
    update_blocked();
}
void sync_watch() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk order");
    for (;;) {
        Sleep(1);
        if (!blocked_n.load()) continue;
        std::unique_lock lk(sync_order_mu, std::try_to_lock);
        if (!lk) continue;
        burst_begin(0);   // (a signal from the CPU the thread that made it could not act on)
        schedule();
        burst_end();
        update_blocked();
        const double held = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - blocked_since).count();
        if (blocked_n.load() && held > hold_ms()) {
            drain(3);
            update_blocked();
        }
    }
}

/// A submit on the replay thread: replayed as far as its waits allow, or (its queue stopped) after its queue's
void submit_job(std::vector<uint64_t>& handles, uint64_t value, uint64_t queue) {
    // ((Triforce) RX_VK_BATCH=parts: what this submit's replay held goes to the queue when it returns)
    struct Depth {
        Depth() { submit_depth++; }
        ~Depth() {
            if (!--submit_depth && batch_way() == barrier_rules::Batch::Parts) queue_flush_for(FR_OTHER);
        }
    } depth;
    const bool may_stop = event_waits_on() && !dump_on();
    if (!may_stop) drain(2);
    auto it = qruns.find(queue);
    if (it != qruns.end() && it->second.blocked()) {   // (its memory work now, in the order of the submits)
        std::lock_guard lk(replay_mu);
        if (frame_submits.empty()) frame_first_value = value;
        frame_submits.emplace_back(handles);
        submit_memory_work(value);
        it->second.later.push_back({std::move(handles), value});
        return;
    }
    if (may_stop) burst_begin(value);   // (its piece and what it lets on: one recording)
    if (run_submit(handles.data(), (int)handles.size(), value, queue, true, 0, nullptr, may_stop)) async_wait_done(value);
    schedule();   // (what it signalled may let the stopped ones on)
    burst_end();
}
uint64_t get_replayed() {
    queue_flush_poll();
    const uint64_t hold = replayed_hold.load();
    return hold ? std::min(hold, completed()) : completed();
}
/// (hookshot) Waits for the recording submission value f (or the recording of value f) was finished in, on the recordings'
/// timeline: true when that recording is known (the result in *ok), false when it is not (f not finished yet)
bool wait_recorded(uint64_t f, uint64_t timeout_ns, int* ok) {
    if (!rec_timeline || per_queue_fences_off()) return false;
    uint64_t seq = 0;
    {
        std::lock_guard mk(rec_map_mu);
        auto it = value_rec.find(f);
        if (it == value_rec.end()) return false;
        seq = it->second;
    }
    // (a batched submit holds its recordings' signal until the batch goes out: the readback wait in invalidate came here
    // without a flush, and the job that waited held up the main thread, so nothing flushed it: the load hang on RADV)
    queue_flush_for(FR_WAIT);
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &rec_timeline;
    wi.pValues = &seq;
    *ok = vkWaitSemaphores(dev.device, &wi, timeout_ns) == VK_SUCCESS;
    return true;
}
uint64_t fence();
/// (hookshot) A fence of one queue (nvnQueueFenceSync): the value of that queue's last submission
bool async_unforced();
uint64_t fence_q_async(uint64_t queue);
bool async_wait_step(uint64_t f);
uint64_t fence_q(uint64_t queue) {
    if (async_unforced() && queue && !per_queue_fences_off()) return fence_q_async(queue);
    if (async_on() || !sync_waits_on() || per_queue_fences_off() || !queue) return fence();
    std::lock_guard lk(sync_order_mu);
    auto it = q_last.find(queue);
    return it != q_last.end() ? it->second : fence();
}
int wait(uint64_t f, uint64_t timeout_ns) {
    if (completed() >= f) return 1;
    queue_flush_for(FR_WAIT);
    // (Triforce, 11-14) the replay thread, nothing forced: the fence's own work, on the recordings' timeline once the replay
    // thread has recorded it whole; until then the wait looks again each time a job of the thread's ends. As below for the
    // submitting thread's replay: not the other queues' stopped work, which the timeline's value waits for
    if (async_unforced() && !per_queue_fences_off()) {
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            if (completed() >= f) return 1;
            const uint64_t waited = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            int ok = 0;
            if (wait_recorded(f, timeout_ns > waited ? timeout_ns - waited : 0, &ok)) return ok;
            if (waited >= timeout_ns) return 0;
            if (!async_wait_step(f)) break;   // (not a value the replay thread still has to record: the timeline's, below)
        }
    }
    // RX_VK_FENCE_POLL_FAST: a poll (timeout 0) of a fence whose submission was finished whole on its queue skips sync_order_mu
    // below, held by submit_q / present for whole replays. The answer is the locked path's: (1) "mine" is true for such a value:
    // a queue's submissions run in order, so one finished whole has no unfinished one of its queue at or below it, and
    // value_queue has it (submitted on a queue, fewer than 8000 values since: value_queue keeps the last 8192); being finished
    // never reverts, so the lock would only have made the same decision later; (2) then wait_recorded(f) as there, under
    // rec_map_mu, which the mark is set under. Only a poll: a wait with a timeout keeps the locked path. Any other case (not
    // marked, an older value, a slot reused by a newer one) takes the locked path. The lock also ordered the poll after the
    // replay in progress, which the CPU's readbacks of GPU results need: the ticket keeps that order (invalidate)
    if (!timeout_ns && fence_poll_fast_on() && !async_on() && sync_waits_on() && !per_queue_fences_off() &&
        whole_val[f & (kWholeSlots - 1)].load(std::memory_order_acquire) == f && f + 8000 > q_values.load()) {
        int ok = 0;
        order_ticket_take();   // (the section the lock would have waited for; see invalidate)
        if (wait_recorded(f, 0, &ok)) return ok;
    }
    // (hookshot) a fence of one queue whose work up to it is recorded whole (none of it stopped): that work, not the other
    // queues' stopped work the timeline above waits for (which a wait for it forced past its waits after RX_VK_WAIT_HOLD_US)
    if (!async_on() && sync_waits_on() && !per_queue_fences_off()) {
        bool mine = false;
        {
            std::lock_guard lk(sync_order_mu);
            auto it = value_queue.find(f);
            if (it != value_queue.end()) {
                auto q = qruns.find(it->second);
                mine = q == qruns.end() || !q->second.blocked() || q->second.oldest() > f;
            }
        }
        int ok = 0;
        if (mine && wait_recorded(f, timeout_ns, &ok)) return ok;
    }
    // (a wait that needs stopped work: another queue's submit usually lets that on in a moment. Forced at once, as the main thread
    // waits ~15 times a second, 69000 waits went past in five minutes, and each one that mattered was a black frame.
    // RX_VK_WAIT_HOLD_US: how long the waiter gives it. A wait for older work than the stopped ones needs nothing of them)
    if (timeout_ns && !async_on() && blocked_n.load()) {
        static const int64_t hold_ns = (getenv("RX_VK_WAIT_HOLD_US") ? atoll(getenv("RX_VK_WAIT_HOLD_US")) : 3000) * 1000;
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            queue_flush_for(FR_WAIT);
            if (completed() >= f) return 1;
            bool needs;
            {
                std::lock_guard lk(sync_order_mu);
                needs = oldest_unfinished() <= f;
            }
            if (!needs) break;
            const int64_t waited = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            if ((uint64_t)waited >= timeout_ns) break;   // (sync_watch's, past hold_ms)
            if (waited >= hold_ns) {
                sync_drain(2);
                break;
            }
            std::this_thread::yield();
        }
    }
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &f;
    queue_flush_for(FR_WAIT);
    const VkResult res = vkWaitSemaphores(dev.device, &wi, timeout_ns);
    static int logged;
    if (res != VK_SUCCESS && timeout_ns > 1000000000ull && logged++ < 8)
        logf("vk: wait for fence %llu (done %llu, pushed %llu): %d", (unsigned long long)f,
             (unsigned long long)completed(), (unsigned long long)pushed.load(), (int)res);
    if (res == VK_ERROR_DEVICE_LOST) device_lost();
    return res == VK_SUCCESS;
}
/// (Triforce) The game's fence waits, timed for the stall line (stall_watch.h); a fence already done is no wait
int wait_counted(uint64_t f, uint64_t timeout_ns) {
    // (Triforce, RX_VK_OCC_QUERY: a wait that ends lets the game read what the GPU finished: the counts of the recordings
    // done are written first. occ_pending is 0 with the switch off)
    if (completed() >= f) {
        if (occ_pending.load(std::memory_order_relaxed)) occ_harvest_done(occ::Stats::kAtWait);
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const int ok = wait(f, timeout_ns);
    if (ok && occ_pending.load(std::memory_order_relaxed)) occ_harvest_done(occ::Stats::kAtWait);
    stall_ct.game_wait_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                                    std::memory_order_relaxed);
    stall_ct.game_waits.fetch_add(1, std::memory_order_relaxed);
    return ok;
}
void wait_queue_empty();
bool replay_behind();
void finish() {
    wait_queue_empty();
    wait_value(pushed.load());
}
void wait_writebacks_reading(const void*, uint64_t) {}   // (nothing to write back: the GPU writes guest memory)
/// (host: the CPU is about to read [gpu, gpu + size), nvnMemoryPoolInvalidateMappedRange / nvnBufferInvalidateMappedRange)
/// RX_VK_LOG_INVALIDATE=1: the ranges, 400 lines, and the textures the GPU wrote there (their texels are not in memory)
// ---- live textures' storage, by 4 KB page (as the host made and finalized them, ahead of the replay) ----
// For invalidate: the CPU reads a range back; texels the GPU wrote into a texture there are not in memory until a
// write-back, and a texture the replay has not listed yet (it runs behind) needs it to catch up first. Ranges no live
// texture's storage touches hold buffers only, which the submissions write back themselves: no catch-up for them.
// (Counts per page: storage shared by several textures; two levels, chunks of 1M pages made when first touched.)
constexpr uint64_t kTexPageChunk = 1u << 20;
std::unique_ptr<std::atomic<uint16_t>[]> tex_pages[(1ull << 39 >> 12) / kTexPageChunk];
std::mutex tex_pages_mu;
// (Triforce, 11-14) and by their exact bytes, for the replay thread's readbacks: the page counts above answer "a texture's
// storage is in these pages", and OoT's exposure meter buffers (0x1040 bytes each, read back every frame) share their last
// page with a texture's storage. With the replay a frame behind that answer sent every one of those readbacks to wait for
// the replay thread to have nothing left: the main thread or a worker held for up to a whole replay, 5% of their time at the
// Market, and the frame's hand-over with them. The exact ranges say no texture is there
std::mutex tex_exact_mu;
std::vector<std::pair<uint64_t, uint64_t>> tex_exact;   // (live textures' storage [start, end), the guest window; tex_exact_mu)
void tex_exact_add(uint64_t lo, uint64_t hi, int d) {
    std::lock_guard lk(tex_exact_mu);
    if (d > 0) return tex_exact.emplace_back(lo, hi), void();
    for (size_t i = tex_exact.size(); i-- > 0;)
        if (tex_exact[i].first == lo && tex_exact[i].second == hi) {
            tex_exact[i] = tex_exact.back();
            tex_exact.pop_back();
            return;
        }
}
bool tex_exact_any(uint64_t gpu, uint64_t size) {
    if (gpu >= (1ull << 39)) return true;   // (not tracked: as before)
    std::lock_guard lk(tex_exact_mu);
    for (const auto& r : tex_exact)
        if (r.first < gpu + size && r.second > gpu) return true;
    return false;
}
void tex_pages_add(const nvn_tex* t, int d) {
    if (!t || !t->gpu || t->gpu >= (1ull << 39) || !t->L.size) return;   // (the guest window: virtual pools are not read)
    tex_exact_add(t->gpu, t->gpu + t->L.size, d);
    const uint64_t p0 = t->gpu >> 12, p1 = std::min((t->gpu + t->L.size - 1) >> 12, uint64_t((1ull << 39 >> 12) - 1));
    for (uint64_t p = p0; p <= p1; p++) {
        auto& c = tex_pages[p / kTexPageChunk];
        if (!c) {
            std::lock_guard lk(tex_pages_mu);
            if (!c) {
                auto n = std::make_unique<std::atomic<uint16_t>[]>(kTexPageChunk);
                for (uint64_t i = 0; i < kTexPageChunk; i++) n[i].store(0, std::memory_order_relaxed);
                c = std::move(n);
            }
        }
        std::atomic<uint16_t>& v = c[p % kTexPageChunk];
        if (d > 0) v.fetch_add(1, std::memory_order_relaxed);
        else if (v.load(std::memory_order_relaxed)) v.fetch_sub(1, std::memory_order_relaxed);
    }
}
bool tex_pages_any(uint64_t gpu, uint64_t size) {
    if (gpu >= (1ull << 39)) return true;   // (not tracked: as before)
    const uint64_t p0 = gpu >> 12, p1 = std::min((gpu + size - 1) >> 12, uint64_t((1ull << 39 >> 12) - 1));
    for (uint64_t p = p0; p <= p1; p++) {
        const auto& c = tex_pages[p / kTexPageChunk];
        if (!c) { p = (p / kTexPageChunk + 1) * kTexPageChunk - 1; continue; }
        if (c[p % kTexPageChunk].load(std::memory_order_relaxed)) return true;
    }
    return false;
}
void texture_created_of(const nvn_tex* t) { tex_pages_add(t, +1); }
uint64_t fence();
void invalidate(uint64_t gpu, uint64_t size) {
    // RX_VK_READBACK_TICKET: the fast fence poll (wait) no longer waits for the sync_order_mu section in progress (a submit_q /
    // present replay); it notes that section's number t and the CPU's readback here waits until section t has left, before
    // anything else it does. That is all the locked poll gave it: the locked poll took the lock after every section that held
    // it when the poll came, so the readback after it saw every write-back registration those sections made. t is `entered`
    // read at the poll: section t+1 increments it only after taking the lock and before it records anything, so none of its
    // registrations is one the poll was ordered after. Section t's registrations come before its `left` increment (seq_cst);
    // the wait reads left >= t and so sees them all. The ticket is taken before the poll returns, so this thread's next
    // readback sees it, and any thread the game hands work to after the poll sees it or a newer one (which only waits longer).
    // Readbacks with no fast poll before them wait for nothing. The poll's wait (~1.4 ms a frame on the main thread) becomes a
    // wait at the first readback after it, only for what is left of that one section by then
    if (size && fence_poll_fast_on()) order_ticket_wait();
    static const bool log_inv = getenv("RX_VK_LOG_INVALIDATE") != nullptr;
    static std::atomic<int> logged{0};
    if (log_inv && logged++ < 400) {
        std::string hits;
        {
            std::lock_guard lk(replay_mu);
            texture_describe_written(gpu, size, hits);
        }
        logf("vk: invalidate %#llx +%#llx:%s", (unsigned long long)gpu, (unsigned long long)size,
             hits.empty() ? " no texture the GPU wrote" : hits.c_str());
    }
    {   // (EvilBoyce, diagnostics) RX_VK_LOG_READBACK=<present>: from that present on, 300 invalidates with the fence handed
        // out, the GPU's completed value, whether the replay is behind, and the first words the CPU is about to read
        static const int64_t from = getenv("RX_VK_LOG_READBACK") ? atoll(getenv("RX_VK_LOG_READBACK")) : -1;
        static std::atomic<int> n{0};
        if (from >= 0 && presents_now >= from && n++ < 300) {
            uint32_t w[8]{};
            host->gpu_read(gpu, w, sizeof w);
            logf("vk: readback %lld %#llx +%#llx: fence %llu done %llu behind %d | %08x %08x %08x %08x %08x %08x %08x %08x",
                 (long long)presents_now, (unsigned long long)gpu, (unsigned long long)size, (unsigned long long)fence(),
                 (unsigned long long)completed(), (int)replay_behind(), w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }
    // (EvilBoyce) GPU results in buffers: the CPU reads them once the submission that writes them back is done. The game
    // reads them when its own pacing says the GPU has finished that frame, which on the console's coherent memory means the
    // values are there; here they reach guest memory only with the write-back at the end of the submission that the replay
    // may not have recorded or the GPU not finished yet. OoT reads its exposure meter (a sum the GPU adds to each frame and
    // the CPU reads and sets to 0) this way: it read 0 one frame, then the old sum came back over the CPU's 0 and the next
    // frame added to it (2x): the exposure stepped 0.2 EV up and down several times a second (the white flashes).
    // RX_VK_NO_READBACK_WAIT=1: as before
    if (size) {
        static const bool off = getenv("RX_VK_NO_READBACK_WAIT") != nullptr;
        bool ever = false;
        if (!off && (memory_writeback_value(host->gpu_guest(gpu), size, &ever), ever)) {
            memory_note_readback(host->gpu_guest(gpu), size);   // (PR 430: the CPU may write it back: memory_rb_refresh)
            static std::atomic<int64_t> calls{0}, waited{0}, ns{0};
            const auto w0 = std::chrono::steady_clock::now();
            bool w = false;
            const uint64_t f0 = fence(), c0 = completed();
            const bool behind0 = replay_behind();
            static const bool no_catchup = getenv("RX_VK_READBACK_NO_CATCHUP") != nullptr;   // (A/B: recorded work only)
            // (hookshot) not on the submitting thread's replay (the default): there what is handed over is recorded at once, and
            // the work not recorded yet is stopped at an event wait. Catching up forced it past that wait, ahead of the work it
            // waits for (OoT's presentation work before the scene's: the fog of the frame before, for a frame, at 1 in 20 of the
            // exposure readbacks). The write-back is waited for on the recordings' timeline instead, which a stopped queue
            // does not hold back
            // (Triforce, 11-14: and on the replay thread with nothing forced, async_unforced: the same rule. The catch-up there
            // held the game's thread for the whole replay behind it, 11.5 ms at 520 of 3000 readbacks at the Market, and made
            // the stopped queues go past their waits)
            const bool unforced = async_unforced() || (!async_on() && sync_waits_on() && !per_queue_fences_off());
            // (Triforce, 11-19, rounds 55 and 56: a wait here on the replay thread, for the jobs handed over before the readback and
            // nothing forced, was tried against the player's character drawn black for a frame after a tour, and taken out again:
            // it cost a quarter of the frame rate (67.7 fps for 85 to 96: the readbacks are the update's workers', about 30 a
            // frame), and RX_VK_LOG_READBACK_ZERO below showed no readback that read 0 at any of 43 such frames)
            if (behind0 && !no_catchup && !unforced) wait_queue_empty(), w = true;   // (work handed over may write it back too)
            // (upstream #2125, behind RX_VK_RB_SETTLE: the stopped work whose waits are met goes on first, never forced: it may
            // be what writes this range, and its write-back is known only once it is recorded)
            else if (behind0 && !no_catchup && !async_on() && rb_fix(2)) sync_settle(), w = true;
            const auto w1 = std::chrono::steady_clock::now();
            const uint64_t v = memory_writeback_value(host->gpu_guest(gpu), size, nullptr);
            if (v > completed()) {
                int ok = 0;
                if (!(unforced && wait_recorded(v, UINT64_MAX, &ok))) {
                    if (unforced && !async_on() && replay_behind()) wait_queue_empty();   // (not recorded whole: as before)
                    if (async_unforced()) {   // (its recording is being made: when the replay thread has it, on its timeline)
                        while (completed() < v && !wait_recorded(v, UINT64_MAX, &ok) && async_wait_step(v)) {}
                        if (completed() < v && !ok) wait_value(v);
                    } else {
                        wait_value(v);
                    }
                }
                w = true;
            }
            if (w) {
                waited++;
                const int64_t d = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - w0).count();
                ns += d;
                static const bool log_each = getenv("RX_VK_LOG_READBACK_WAITS") != nullptr;
                static std::atomic<int> logged{0};
                if (log_each && presents_now > 4000 && logged++ < 300)
                    logf("vk: readback wait %lld %#llx +%#llx: %.2f ms (catch-up %.2f) value %llu, fence %llu, done %llu, behind %d",
                         (long long)presents_now, (unsigned long long)gpu, (unsigned long long)size, d / 1e6,
                         std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count() / 1e6, (unsigned long long)v,
                         (unsigned long long)f0, (unsigned long long)c0, (int)behind0);
            }
            {   // ((Triforce, a diagnostic) RX_VK_LOG_READBACK_ZERO=1: a range whose first words are all 0 as the game is about to
                // read them, where they were not at its readback before: which readback hands the game a word it set to 0
                // itself, the frame, and how far the replay was. 6000 lines a run at most
                static const bool zlog = getenv("RX_VK_LOG_READBACK_ZERO") != nullptr;
                if (zlog) {
                    static std::mutex zm;
                    static std::unordered_map<uint64_t, uint8_t> last;   // (a range: 1 not all 0 at its last readback, 2 all 0)
                    static int zn;
                    uint32_t zw[8]{};
                    host->gpu_read(gpu, zw, (size_t)std::min<uint64_t>(size, sizeof zw));
                    bool nz = false;
                    for (const uint32_t x : zw) nz |= x != 0;
                    std::lock_guard zk(zm);
                    uint8_t& l = last[gpu];
                    if (l == 1 && !nz && zn++ < 6000)
                        logf("vk: readback zero %lld %#llx +%#llx: all 0 where it was not at its last readback (fence %llu, GPU done %llu, "
                             "its write-back %llu, the replay behind %d, waited %d)", (long long)presents_now, (unsigned long long)gpu,
                             (unsigned long long)size, (unsigned long long)f0, (unsigned long long)completed(), (unsigned long long)v,
                             (int)behind0, (int)w);
                    l = nz ? 1 : 2;
                }
            }
            if (++calls % 3000 == 0) {
                const int64_t n = waited.exchange(0);
                logf("vk: readbacks of GPU results: %lld of 3000 waited for their write-back, %.2f ms each", (long long)n,
                     n ? ns.exchange(0) / (double)n / 1e6 : 0.0);
            }
        }
    }
    // (textures the GPU wrote there: their texels into memory before the CPU reads it, as vmap_changing does. The game
    // waited for the work that wrote them; the replay is caught up first)
    {   // (RX_VK_READBACK_OFF_FILE=<path> (A/B): while that file exists, not)
        static const char* off_file = getenv("RX_VK_READBACK_OFF_FILE");
        if (off_file && GetFileAttributesA(off_file) != INVALID_FILE_ATTRIBUTES) return;
    }
    // (the replay caught up first only for ranges a texture the GPU wrote may be in: MKW invalidates a few buffer
    // ranges every frame (GPU results it reads back, written back with their submission; one of them a ring of
    // slots at a new address each time), and the catch-up for each held its main thread until the replay had
    // recorded everything queued: ~10% of the main thread's time. A 1 MB region caught up for kProbe times without a
    // texture found holds buffers only from then on; one a texture was ever found in keeps the catch-up.
    // RX_VK_INVALIDATE_WAIT=1: every range, as before)
    // (decided by the live textures' storage, not by what the replay has listed: a region guessed to hold buffers
    // only, after probes that found no texture, left MKW's Grand Prix waiting at its map for a texture's texels)
    static const bool always = getenv("RX_VK_INVALIDATE_WAIT") != nullptr;
    if (!size) return;
    // (hookshot) and on the submitting thread's replay, the catch-up lets on what may go without forcing anything past its wait
    // (once the exposure readback above no longer did, this one forced OoT's presentation work on, every frame or two)
    const bool unforced = !async_on() && sync_waits_on() && !per_queue_fences_off();
    if (!texture_gpu_written_in(gpu, size)) {
        if (!replay_behind()) return;
        if (!always && !tex_pages_any(gpu, size)) return;   // (no texture's storage there: buffers only)
        // (Triforce: RX_VK_TEX_EXACT=1, the lab: none in these bytes, no wait: see tex_exact. NOT the default: the wait is the
        // one that orders the exposure meter. Without it the Market flickered at once, 4,416 cell blinks in 6,732 frames against
        // 0, for 70.3 fps against 67.6-68.0: the game reads each frame's sums whole, after that frame's replay and GPU work)
        if (static const bool exact = getenv("RX_VK_TEX_EXACT") != nullptr; exact && !always && async_unforced() && !tex_exact_any(gpu, size)) return;
        unforced ? sync_settle() : wait_queue_empty();
        if (!texture_gpu_written_in(gpu, size)) return;
    }
    {   // (what the catch-ups for a texture cost the game's thread, every 300 calls)
        static std::atomic<int64_t> calls{0}, ns{0};
        const auto w0 = std::chrono::steady_clock::now();
        unforced ? sync_settle() : wait_queue_empty();
        ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - w0).count();
        if (++calls % 300 == 0)
            logf("vk: invalidate %#llx +%#llx: textures there; the catch-ups waited %.2f ms each (300 calls)",
                 (unsigned long long)gpu, (unsigned long long)size, ns.exchange(0) / 300.0 / 1e6);
    }
    std::lock_guard lk(replay_mu);
    texture_flush_to_guest(gpu, size);
}
void do_texture_released(const nvn_tex* t) {
    std::lock_guard lk(replay_mu);
    if (t) {
        mkwvk::texture_released(t->serial);   // (vk_bank.cpp: the registrations of it)
        mkwvk::texture_released(t);           // (vk_texture.cpp: its image may take CPU writes again)
    }
}

// ---- presents ----
struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
};
Frame frames[3];
uint32_t frame_index = 0;
int64_t presents = 0;

// offscreen (no window): the picture, and the readback of a screenshot
VkImage off_image = VK_NULL_HANDLE;
VmaAllocation off_alloc = nullptr;
// (two readbacks, by the present's parity: a shot is read at a later present, after that one's own copy was submitted,
// and with one buffer the CPU read it while the GPU wrote the next frame over it: torn shots, and flashes counted)
VkBuffer shot_bufs[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VmaAllocation shot_allocs[2] = {nullptr, nullptr};
void* shot_maps[2] = {nullptr, nullptr};
// (RX_VK_SHOT_W / RX_VK_SHOT_H: another size, e.g. the game's output size for its own pixels)
uint32_t shot_dimension(const char* text, uint32_t fallback) {
    if (!text) return fallback;
    if (!*text) return 0;
    uint32_t value = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9') return 0;
        const uint32_t digit = (uint32_t)(*text - '0');
        if (value > (UINT32_MAX - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    return value;
}
const uint32_t kShotW = shot_dimension(getenv("RX_VK_SHOT_W"), 1280),
               kShotH = shot_dimension(getenv("RX_VK_SHOT_H"), 720);
VkImage shot_image = VK_NULL_HANDLE;
VkImageView shot_view = VK_NULL_HANDLE;   // (hookshot: RX_VK_SHOT_MENU's menu draws through it)
VkBuffer made_buf = VK_NULL_HANDLE;   // (RX_VK_SHOT_MADE: a made frame's readback)
VmaAllocation made_buf_alloc = nullptr;
void* made_map = nullptr;
VkImage made_image = VK_NULL_HANDLE;
VmaAllocation made_image_alloc = nullptr;
VmaAllocation shot_image_alloc = nullptr;

bool shot_dimensions_ready() {
    static int state = 0;
    if (state) return state > 0;
    state = -1;
    const uint32_t limit = dev.props.limits.maxImageDimension2D;
    const uint64_t pixels = (uint64_t)kShotW * kShotH, row = (uint64_t)kShotW * 3 + 1;
    bool valid = false;
    if (kShotW && kShotH && kShotW <= limit && kShotH <= limit && pixels <= SIZE_MAX / 4) {
        const uint64_t raw = row * kShotH;
        // write_png emits one IDAT: zlib header, stored blocks (5 bytes per65535), and Adler32.
        // PNG chunks are limited to2^31-1 bytes. This is an encoder limit, not a memory budget.
        constexpr uint64_t max_chunk = 0x7fffffffu;
        if (raw <= max_chunk) {
            const uint64_t encoded = 2 + raw + 5 * ((raw + 65534) / 65535) + 4;
            valid = encoded <= max_chunk && encoded <= SIZE_MAX - 4;
        }
    }
    if (!valid) {
        logf("vk: screenshots disabled: RX_VK_SHOT_W/H must be positive decimal dimensions within the device limit "
             "(%u) and the PNG/readback size limits (got %ux%u)", limit, kShotW, kShotH);
        return false;
    }
    state = 1;
    return true;
}

bool made_shot_init() {
    if (made_image) return true;
    VkImage next_image = VK_NULL_HANDLE;
    VmaAllocation next_alloc = nullptr, next_buf_alloc = nullptr;
    VkBuffer next_buf = VK_NULL_HANDLE;
    void* next_map = nullptr;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {kShotW, kShotH, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!create_image(ci, next_image, next_alloc)) return false;
    if (!create_buffer((uint64_t)kShotW * kShotH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                       next_buf, next_buf_alloc, &next_map)) {
        destroy_image(next_image, next_alloc);
        return false;
    }
    made_image = next_image, made_image_alloc = next_alloc;
    made_buf = next_buf, made_buf_alloc = next_buf_alloc, made_map = next_map;
    return true;
}

bool offscreen_init() {
    if (!shot_dimensions_ready()) return false;
    if (off_image) return true;
    VkImage next_off = VK_NULL_HANDLE, next_shot = VK_NULL_HANDLE;
    VkImageView next_view = VK_NULL_HANDLE;
    VmaAllocation next_off_alloc = nullptr, next_shot_alloc = nullptr, next_allocs[2]{};
    VkBuffer next_bufs[2]{};
    void* next_maps[2]{};
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {dev.swap_extent.width, dev.swap_extent.height, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    bool ok = create_image(ci, next_off, next_off_alloc);
    ci.extent = {kShotW, kShotH, 1};
    ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;   // (hookshot: RX_VK_SHOT_MENU draws the menu over it, vk_menu.cpp)
    ok = ok && create_image(ci, next_shot, next_shot_alloc);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = next_shot;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ok = ok && vkCreateImageView(dev.device, &vi, nullptr, &next_view) == VK_SUCCESS;
    for (int i = 0; i < 2; i++)
        ok = ok && create_buffer((uint64_t)kShotW * kShotH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                 next_bufs[i], next_allocs[i], &next_maps[i]);
    if (!ok) {
        for (int i = 0; i < 2; i++)
            if (next_bufs[i]) destroy_buffer(next_bufs[i], next_allocs[i]);
        if (next_view) vkDestroyImageView(dev.device, next_view, nullptr);
        if (next_shot) destroy_image(next_shot, next_shot_alloc);
        if (next_off) destroy_image(next_off, next_off_alloc);
        return false;
    }
    // Publish the ready marker only after both images, the menu view and both mapped readbacks exist.
    off_image = next_off, off_alloc = next_off_alloc;
    shot_image = next_shot, shot_image_alloc = next_shot_alloc;
    shot_view = next_view;
    for (int i = 0; i < 2; i++)
        shot_bufs[i] = next_bufs[i], shot_allocs[i] = next_allocs[i], shot_maps[i] = next_maps[i];
    return true;
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

// A completed capture copy must also be made available in the host memory domain.
void capture_host_read_barrier(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize bytes) {
    VkBufferMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buffer;
    b.size = bytes;
    VkDependencyInfo d{};
    d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    d.bufferMemoryBarrierCount = 1;
    d.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

/// The crop of src (x, y, w, h; w 0: all of it) scaled into a rectangle of dst.
/// An empty source clears the whole destination image; current callers give that fallback a full-image rectangle.
void blit_crop_to_rect(VkCommandBuffer cmd, VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImage dst,
                       VkImageLayout dst_layout, PresentRect rect) {
    const PresentSourceRect source = present_source_rect(sw, sh, crop);
    if (!rect.w || !rect.h) return;
    if (!source.w || !source.h) {
        const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, dst, dst_layout, &black, 1, &range);
        return;
    }
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[0] = {(int)source.x, (int)source.y, 0};
    b.srcOffsets[1] = {(int)(source.x + source.w), (int)(source.y + source.h), 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[0] = {(int)rect.x, (int)rect.y, 0};
    b.dstOffsets[1] = {(int)(rect.x + rect.w), (int)(rect.y + rect.h), 1};
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, dst, dst_layout, 1, &b, VK_FILTER_LINEAR);
}

/// The crop of src scaled to the whole destination (captures keep their existing dimensions).
void blit_crop(VkCommandBuffer cmd, VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImage dst,
               VkImageLayout dst_layout, uint32_t dw, uint32_t dh) {
    blit_crop_to_rect(cmd, src, sw, sh, crop, dst, dst_layout, {0, 0, dw, dh});
}

// ---- PNG (stored deflate blocks: no compression library needed) ----
uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> out{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            out[i] = k;
        }
        return out;
    }();
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
} // namespace

/// (Triforce) The device lost's report (vk_draw.cpp checkpoints_report): the queue's timeline as the GPU left it against the
/// values handed out, recorded whole and signalled, and the replay's ways then (the helper threads, the recorder). Read without
/// replay_mu: the thread that lost the device may hold it
void replay_lost_report() {
    uint64_t reached = 0;
    const VkResult r = timeline ? vkGetSemaphoreCounterValue(dev.device, timeline, &reached) : VK_NOT_READY;
    char at[64];
    if (r == VK_SUCCESS) snprintf(at, sizeof at, "%llu", (unsigned long long)reached);
    else snprintf(at, sizeof at, "not readable (%d)", (int)r);
    logf("vk: device lost: the queue's timeline at %s; values handed out %llu, recorded whole %llu, signalled %llu, pushed %llu; "
         "%llu recordings submitted; %lld presents; helper threads %s (%d), the recorder %d (RX_VK_RECORDER)", at,
         (unsigned long long)q_values.load(), (unsigned long long)done_max, (unsigned long long)signaled,
         (unsigned long long)pushed.load(), (unsigned long long)rec_seq, (long long)presents_now,
         par_auto_on ? "in use" : "not in use", par_threads(), rec::mode());
}

/// A submission or a wait came back VK_ERROR_DEVICE_LOST. Nothing sent from here on runs, and the replay kept going: no
/// submission completed again, so no capture or staging arena was let go, and system memory filled until the window was
/// closed (issue 1275: captures kept 25 -> 6527, staging 256 -> 1408 MB). Now the report once (a thread that sees the loss
/// too waits for it, instead of ending the process before it is written), then the process ends as replay_fatal does
[[noreturn]] void device_lost() {
    memory_trial_device_lost();   // (Triforce: with the memory handling on trial on this card: off from the next start)
    static std::once_flag once;
    std::call_once(once, checkpoints_report);
    replay_fatal("the GPU device was lost (VK_ERROR_DEVICE_LOST): the driver stopped or reset the GPU, and nothing sent "
                 "to it runs any more (on Linux, dmesg or journalctl -k around this time names the reset)");
}

bool write_png(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
    auto failed = [path](const char* phase, unsigned error = 0) {
        static std::atomic<unsigned> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 8)
            logf("vk: PNG %s failed (error %u): %s", phase, error, path ? path : "(null)");
        return false;
    };
    if (!path || !*path || !rgba || !w || !h || w > 0x7fffffffu || h > 0x7fffffffu)
        return failed("dimensions/path");
    const uint64_t pixels = (uint64_t)w * h;
    if (pixels > SIZE_MAX / 4) return failed("size");
    const uint64_t raw_size = ((uint64_t)w * 3 + 1) * h;
    constexpr uint64_t max_chunk = 0x7fffffffu;
    if (raw_size > max_chunk || 6 + raw_size + 5 * ((raw_size + 65534) / 65535) > max_chunk)
        return failed("size");
    try {
        std::vector<uint8_t> raw;
        raw.reserve((size_t)(w * 3 + 1) * h);
        for (uint32_t y = 0; y < h; y++) {
            raw.push_back(0);
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t* q = rgba + ((size_t)y * w + x) * 4;
                raw.insert(raw.end(), q, q + 3);
            }
        }
        std::vector<uint8_t> z = {0x78, 0x01};
        uint32_t a = 1, b = 0;
        for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
        for (size_t at = 0; at < raw.size();) {
            const size_t n = std::min<size_t>(65535, raw.size() - at);
            z.push_back(at + n == raw.size() ? 1 : 0);
            z.push_back((uint8_t)n), z.push_back((uint8_t)(n >> 8));
            z.push_back((uint8_t)~n), z.push_back((uint8_t)(~n >> 8));
            z.insert(z.end(), raw.begin() + (long)at, raw.begin() + (long)(at + n));
            at += n;
        }
        const uint32_t adler = b << 16 | a;
        for (int i = 3; i >= 0; i--) z.push_back((uint8_t)(adler >> (i * 8)));

        // Encode before opening a file; only a completed private temporary replaces the destination.
        // ((Triforce, handoff §8) the pictures are written on threads of their own: the exit waits for one under way, and
        // none begins once it has begun, file_writers.h)
        const file_writers::Writing writing;
        if (!writing) return failed("the game's end");
        static std::atomic<uint64_t> next{0};
        std::string temporary;
        FILE* f = nullptr;
        for (int attempt = 0; attempt < 128; attempt++) {
            temporary = std::string(path) + ".part." + std::to_string(next.fetch_add(1, std::memory_order_relaxed));
            f = fopen(temporary.c_str(), "wbx");
            if (f || errno != EEXIST) break;
        }
        if (!f) return failed("open", errno);
        bool ok = true;
        unsigned error = 0;
        const char* phase = "write";
        auto put = [&](const void* bytes, size_t n) {
            if (ok && n && fwrite(bytes, 1, n, f) != n) {
                error = errno ? errno : EIO;
                ok = false;
            }
        };
        auto be32 = [&](uint32_t v) {
            const uint8_t q[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
            put(q, sizeof q);
        };
        auto chunk = [&](const char* type, const uint8_t* d, size_t n) {
            be32((uint32_t)n);
            put(type, 4);
            put(d, n);
            be32(crc32(d, n, crc32((const uint8_t*)type, 4)) ^ 0xFFFFFFFFu);
        };
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
        put(sig, sizeof sig);
        const uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                                 (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
        chunk("IHDR", ihdr, sizeof ihdr);
        chunk("IDAT", z.data(), z.size());
        chunk("IEND", nullptr, 0);
        if (fclose(f) != 0) {
            if (ok) phase = "close", error = errno ? errno : EIO;
            ok = false;
        }
        if (ok) {
            phase = "publish";
#ifdef _WIN32
            ok = MoveFileExA(temporary.c_str(), path, MOVEFILE_REPLACE_EXISTING) != 0;
            if (!ok) error = GetLastError();
#else
            ok = rename(temporary.c_str(), path) == 0;
            if (!ok) error = errno;
#endif
        }
        if (!ok) {
            if (remove(temporary.c_str()) != 0) failed("temporary cleanup", errno);
            return failed(phase, error);
        }
        return true;
    } catch (const std::exception&) {
        return failed("encoding allocation");
    }
}

// (Triforce) outside the unnamed namespace below: vk_framegen.cpp reads it too (AMD FSR 4's frame generation is told
// whether the picture is HDR10 codes), as it reads present_kind
std::atomic<bool> present_native_pq{false};   // (do_present: the game's picture is already HDR10)
namespace {

int64_t fps_n = 0;
std::chrono::steady_clock::time_point fps_t = std::chrono::steady_clock::now();

/// RX_VK_SLOW_LOG=<ms>: a frame that long after the last: its replay's times, for what the stutters are
void log_slow_frame() {
    static const int limit = getenv("RX_VK_SLOW_LOG") ? atoi(getenv("RX_VK_SLOW_LOG")) : 0;
    if (!limit) return;
    static auto last_t = std::chrono::steady_clock::now();
    static int64_t last[PROF_COUNT];
    static int64_t last_pl, last_tr, last_d, last_up;
    static int logged;
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - last_t).count();
    last_t = now;
    int64_t d = 0, sk = 0, di = 0, pl = 0, tr = 0, fl = 0, up = 0, astc = 0, upb = 0;
    draw_stats(&d, &sk, &di, &pl);
    shader_stats(&tr, &fl);
    texture_stats(&up, &astc, &upb);
    if (ms >= limit && d - last_d >= 500 && logged < 800) {
        logged++;
        const auto dp = [](int i) { return (prof_ns[i] - last[i]) / 1e6; };
        logf("vk: slow frame at present %lld: %.1f ms; submit %.1f, draw %.1f, descriptors %.1f, pipelines %.1f, programs %.1f, "
             "commands %.1f, memory sync %.1f, queue submit %.1f, slot wait %.1f; %lld draws, %lld new pipelines, %lld new programs, "
             "%lld uploads", (long long)presents, ms, dp(PROF_SUBMIT), dp(PROF_DRAW), dp(PROF_DESC), dp(PROF_PIPE), dp(PROF_PROGRAM),
             dp(PROF_CMDS), dp(PROF_MEMSYNC), dp(PROF_QSUBMIT), dp(PROF_SLOTWAIT), (long long)(d - last_d), (long long)(pl - last_pl),
             (long long)(tr - last_tr), (long long)(up - last_up));
    }
    for (int i = 0; i < PROF_COUNT; i++) last[i] = prof_ns[i];
    last_pl = pl, last_tr = tr, last_d = d, last_up = up;
}

uint64_t made_ring[32];
int64_t made_ring_present = 0;
/// (Triforce) The parallel replay over the frames since the last line: in how many of them the helper threads recorded, the
/// command buffers in segments and on the replay thread alone (and why), and the time: the dry pass's, the replay thread's
/// wait for the segments, and their recording on all threads (the replay time line's draw, commands and descriptors count
/// the helper threads' time too, summed). Nothing while there are no helper threads
void par_stats_line(int frames, int used) {
    if (par_threads_max() < 2 || frames <= 0) return;
    static int64_t l[13];
    const int64_t v[13] = {par_st.segmented, par_st.alone_off, par_st.alone_debug, par_st.alone_order, par_st.alone_small,
                           par_st.segments, par_st.tails, par_st.work, par_st.tail_work, par_st.rec_ns, par_st.tail_wait_ns,
                           par_st.wait_ns, par_st.dry_ns};
    int64_t d[13];
    for (int i = 0; i < 13; i++) d[i] = v[i] - l[i], l[i] = v[i];
    const double f = frames;
    logf("vk: a frame's parallel replay: helper threads recorded in %d of %d frames (%s); command buffers a frame: %.1f in "
         "segments (%.1f segments, %.1f of them a tail recorded in order from the upscale on, %.0f%% of the draws and "
         "dispatches there), %.1f on the replay thread alone (%.1f off or the trials' serial way, %.1f small, %.1f DLSS or FSR "
         "kept in order, %.1f dumps or profiles); dry pass %.2f ms, the replay thread was %.2f ms in the segments' part (its waits "
         "for them, their submits, and those it recorded itself: the next line), which took %.2f ms on all threads (the tail "
         "waited %.2f ms for those before it)",
         used, frames, par_auto() ? (par_auto_on ? "the trials keep them" : "the trials keep the replay thread alone") : "always",
         d[0] / f, d[5] / f, d[6] / f, d[7] ? 100.0 * (double)d[8] / (double)d[7] : 0.0, (d[1] + d[2] + d[3] + d[4]) / f,
         d[1] / f, d[4] / f, d[3] / f, d[2] / f, d[12] / f / 1e6, d[11] / f / 1e6, d[9] / f / 1e6, d[10] / f / 1e6);
    // (Triforce, 11-15) its parts (ParStats), and the segments' GPU time (gpu_time_collect), while the threads recorded any
    static int64_t lp[11], l_gpu_seg;
    const int64_t pv[11] = {par_st.setup_ns, par_st.first_ns, par_st.wake_ns, par_st.wakes, par_st.longest_ns, par_st.submit_ns,
                            par_st.post_ns, par_st.own_ns, par_st.own, par_lock_wait_ns.load(), par_locks.load()};
    int64_t p[11];
    for (int i = 0; i < 11; i++) p[i] = pv[i] - lp[i], lp[i] = pv[i];
    const int64_t gpu_seg = st_gpu_seg_ns - l_gpu_seg;
    l_gpu_seg = st_gpu_seg_ns;
    if (!d[0]) return;
    const double cbs = (double)d[0];
    // (the replay thread's wait: its time in the segments' part less its own recording and the submits)
    const double waited = (double)(d[11] - p[7] - p[5]);
    logf("vk: a frame's parallel replay, its parts (ms a frame unless said): setup %.2f (the part before submitted, the segments' "
         "recordings begun), the first segment to the GPU %.2f ms after its command buffer's replay began (each one's), the helper "
         "threads began a segment %.3f ms after they were given it (each), recording %.2f on all threads (%.2f of it by the replay "
         "thread, %.1f segments), each command buffer's longest segment %.2f ms (its mean %.2f), the replay thread waited %.2f, "
         "submits %.2f, the recording after them %.2f; the recording lock waited for %.3f (taken %.0f times); GPU time in "
         "segments %.2f",
         p[0] / f / 1e6, p[1] / cbs / 1e6, p[3] ? p[2] / (double)p[3] / 1e6 : 0.0, d[9] / f / 1e6, p[7] / f / 1e6, p[8] / f,
         p[4] / cbs / 1e6, d[5] ? d[9] / (double)d[5] / 1e6 : 0.0, waited / f / 1e6, p[5] / f / 1e6, p[6] / f / 1e6,
         p[9] / f / 1e6, p[10] / f, gpu_seg / f / 1e6);
    // ((Triforce, the third performance audit's P6) the spins before those waits: helper_spin.h, par_worker_main, replay_handle)
    static int64_t ls[6];
    const int64_t sv[6] = {par_pool.spun.load(), par_pool.slept.load(), par_pool.spin_ns.load(), par_st.seg_spun.load(),
                           par_st.seg_slept.load(), par_st.seg_spin_ns.load()};
    int64_t s[6];
    for (int i = 0; i < 6; i++) s[i] = sv[i] - ls[i], ls[i] = sv[i];
    logf("vk: a frame's parallel replay, its spins (RX_VK_HELPER_SPIN_US=%lld; 0: off): the helper threads found a segment in "
         "the spin %.1f times, went to sleep for one %.1f times, spun %.3f ms (all threads); the replay thread's waits for a "
         "segment ended in the spin %.1f times, slept %.1f times, spun %.3f ms",
         (long long)(helper_spin::spin_ns() / 1000), s[0] / f, s[1] / f, s[2] / f / 1e6, s[3] / f, s[4] / f, s[5] / f / 1e6);
}

/// (Triforce, 11-15) RX_VK_RECORDER=thread: a frame's share of the recorder thread's work (the driver's calls it made for the
/// replay thread, which the replay time line's commands no longer has: it has their packing), and of the replay thread's waits
/// for it, beside the commands left on the replay thread. The way to read what it saves: commands with it against without it
void rec_stats_line(int frames) {
    if (rec::mode() != rec::THREAD || frames <= 0) return;
    const rec::Recorder& g = rec::g_rec;
    static uint64_t l[4];
    const uint64_t v[4] = {g.all_packets.load(), g.all_busy_ns.load(), g.all_waits.load(), g.all_wait_ns.load()};
    double d[4];
    for (int i = 0; i < 4; i++) d[i] = (double)(v[i] - l[i]) / frames, l[i] = v[i];
    logf("vk: a frame's recorder thread (RX_VK_RECORDER=thread): %.0f of the replay's calls made by it in %.2f ms; the replay thread "
         "waited for it %.1f times, %.2f ms (the replay time line's commands are its packing now; a parallel replay's segments make "
         "their calls themselves)", d[0], d[1] / 1e6, d[2], d[3] / 1e6);
}

void present_thread_line();     // ((Triforce) the present thread's figures, RX_VK_PRESENT_THREAD: below present_image)
int64_t present_waited_ns();    // ((Triforce) present_image's waits for the present before it, counted from the start)

void log_frame_stats() {
    log_slow_frame();
    memory_ram_watch((uint64_t)presents);   // ((Triforce) the PC's RAM, and every 600 presents the memory line: vk_memory.cpp)
    pipelines_release_tick((uint64_t)presents);   // ((Triforce) with the RAM short, kept pipelines no draw has had: vk_draw.cpp)
    made_ring[presents % 32] = texture_made();
    made_ring_present = presents;
    static int par_used_frames = 0;   // (Triforce: frames whose command buffers the helper threads recorded some of)
    if (par_st.used.exchange(false, std::memory_order_relaxed)) par_used_frames++;
    if (++fps_n == 120) {
        par_stats_line(120, par_used_frames);
        rec_stats_line(120);
        par_used_frames = 0;
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - fps_t).count();
        int64_t d = 0, sk = 0, di = 0, pl = 0, tr = 0, fl = 0, passes = 0, mesh = 0, up = 0, astc = 0, upb = 0;
        int64_t st_clears = 0, masked = 0;
        clear_stats(&st_clears, &masked);
        double pms = 0;
        draw_stats(&d, &sk, &di, &pl);
        draw_stats2(&passes, &mesh, &pms);
        shader_stats(&tr, &fl);
        texture_stats(&up, &astc, &upb);
        int64_t pskip = 0, pwaits = 0;
        double pwait_ms = 0;
        draw_stats3(&pskip, &pwaits, &pwait_ms);
        static int64_t l_pskip, l_pwaits;
        static double l_pwait_ms;
        static int64_t l_reused, l_saves;
        int64_t links = 0, lib_hits = 0, lib_cold = 0, lib_built = 0;
        double link_ms = 0, lib_build_ms = 0;
        static int64_t l_links;
        static double l_link_ms;
        char libs[256] = "";   // (the stage libraries' part of the line, with RX_VK_GPL only: other logs keep the line they had)
        if (draw_stats4(&links, &link_ms, &lib_hits, &lib_cold, &lib_built, &lib_build_ms))
            snprintf(libs, sizeof libs, "; %.2f library links tried (%.2f ms), %lld libraries the driver had and %lld cold in all; "
                     "%lld of the cold built by workers (%.0f ms)", (links - l_links) / 120.0, (link_ms - l_link_ms) / 120.0,
                     (long long)lib_hits, (long long)lib_cold, (long long)lib_built, lib_build_ms);
        logf("vk: a frame: %.0f draws kept their neighbour's descriptors (%.0f of them through other slots' registrations)",
             (draw_desc_reused() - l_reused) / 120.0, (draw_slotgen_saves() - l_saves) / 120.0);
        l_reused = draw_desc_reused(), l_saves = draw_slotgen_saves();
        logf("vk: a frame: %.1f draws left out (not ready), %.2f waits for shaders or pipelines (%.1f ms); "
             "translation %.0f ms in all, %d jobs queued%s",
             (pskip - l_pskip) / 120.0, (pwaits - l_pwaits) / 120.0, (pwait_ms - l_pwait_ms) / 120.0,
             shader_translate_ms(), jobs_pending(), libs);
        l_pskip = pskip, l_pwaits = pwaits, l_pwait_ms = pwait_ms, l_links = links, l_link_ms = link_ms;
        {
            static const char* names[PROF_COUNT] = {"submit", "present", "draw", "descriptors", "texture lookups",
                                                    "uploads", "pipelines", "programs", "commands", "dispatch",
                                                    "memory sync", "queue submit", "slot wait"};
            static int64_t last[PROF_COUNT];
            char line[600];
            int n = snprintf(line, sizeof line, "vk: a frame's replay time (ms):");
            for (int i = 0; i < PROF_COUNT; i++) {
                n += snprintf(line + n, sizeof line - n, " %s %.2f%s", names[i], (prof_ns[i] - last[i]) / 120.0 / 1e6,
                              i + 1 < PROF_COUNT ? "," : "");
                last[i] = prof_ns[i];
            }
            logf("%s", line);
        }
        {   // ((Triforce) the submit-time captures (vk_memory.cpp capture_make): their time on the game's threads, outside the replay's
            // submit above, and what the copy helpers took off it (RX_VK_CAPTURE_BEHIND)
            MemoryCaptureStats s;
            memory_capture_stats(&s);
            static MemoryCaptureStats l;
            const double f = 120.0;
            logf("vk: a frame's captures at the game's submits: %.2f ms on the game's threads (%.1f captures: scan %.2f, copies made there "
                 "%.2f; the copies behind made or waited for by the replay %.2f); %.2f MB copied, %.2f MB of it behind the submit (parts: "
                 "%.1f by the helpers, %.1f by the replay's reads, %.1f at the end, %.1f waits; %lld pages unreadable)",
                 (s.ns - l.ns + s.wait_ns - l.wait_ns) / f / 1e6, (s.captures - l.captures) / f, (s.scan_ns - l.scan_ns) / f / 1e6,
                 (s.copy_ns - l.copy_ns) / f / 1e6, (s.wait_ns - l.wait_ns) / f / 1e6, (s.pages - l.pages) * 4096.0 / f / 1048576.0,
                 (s.behind - l.behind) * 4096.0 / f / 1048576.0, (s.parts_helper - l.parts_helper) / f, (s.parts_reader - l.parts_reader) / f,
                 (s.parts_finish - l.parts_finish) / f, (s.waited - l.waited) / f, (long long)(s.failed - l.failed));
            l = s;
        }
        present_thread_line();   // ((Triforce) the present's own: its present thread, what the presenting thread waited for)
        int64_t mirrors = 0, sync_b = 0, sync_c = 0, wb_b = 0;
        uint64_t mirror_mb = 0;
        memory_stats(&mirrors, &mirror_mb, &sync_b, &sync_c, &wb_b);
        static int64_t l_sync_b, l_sync_c, l_wb_b;
        logf("vk: a frame: mirrors %lld (%llu MB): %.2f MB synced in %.1f copies, %.2f MB written back",
             (long long)mirrors, (unsigned long long)mirror_mb, (sync_b - l_sync_b) / 120.0 / 1048576.0,
             (sync_c - l_sync_c) / 120.0, (wb_b - l_wb_b) / 120.0 / 1048576.0);
        l_sync_b = sync_b, l_sync_c = sync_c, l_wb_b = wb_b;
        static int64_t l_d, l_sk, l_di, l_pass, l_sub, l_up, l_upb, l_bar, l_ubu, l_ubo, l_wait;
        logf("vk: %lld presents, %.1f fps; a frame: %.0f draws (%.0f skipped), %.0f dispatches, %.0f passes, %.0f "
             "barriers, %.0f submits, %.1f uploads (%.1f MB), %.0f ub updates (%.0f overlays), GPU-bound wait %.1f ms; "
             "%lld pipelines (%.0f ms), %lld programs (%lld failed), %zu textures (%llu MB)",
             (long long)presents, 120.0 / secs, (d - l_d) / 120.0, (sk - l_sk) / 120.0, (di - l_di) / 120.0,
             (passes - l_pass) / 120.0, (st_barriers - l_bar) / 120.0, (st_submits - l_sub) / 120.0,
             (up - l_up) / 120.0, (upb - l_upb) / 120.0 / 1048576.0, (st_ub_updates - l_ubu) / 120.0,
             (st_ub_overlays - l_ubo) / 120.0, (st_wait_ns - l_wait) / 120.0 / 1e6, (long long)pl, pms,
             (long long)tr, (long long)fl, texture_count(), (unsigned long long)(texture_bytes() >> 20));
        draw_skip_report(120.0);
        {   // (hookshot: the full barriers recorded and the ones left out as the second of two in a row, RX_VK_BARRIER_DEDUPE_OFF=1)
            static int64_t l_made, l_left;
            int64_t made = 0, left = 0;
            barrier_stats(&made, &left);
            static int64_t l_relaxed;
            const int64_t relaxed = pass_barriers_relaxed();
            logf("vk: a frame: %.1f full barriers recorded, %.1f left out (nothing recorded since the one before), %.1f pass starts "
                 "ordered in their fragment stages only (the default with NVIDIA's driver; RX_VK_PASS_BARRIER=full: none)", (made - l_made) / 120.0, (left - l_left) / 120.0,
                 (relaxed - l_relaxed) / 120.0);
            l_made = made, l_left = left, l_relaxed = relaxed;
            std::string fl;   // (NVN's barrier bits: ORDER_PRIMITIVES 1, ORDER_FRAGMENTS 2, ORDER_FRAGMENTS_TILED 4, ORDER_INDIRECT_DATA 8, INVALIDATE_TEXTURE 0x10, _SHADER 0x20, _TEXTURE_DESCRIPTOR 0x40, _ZCULL 0x80)
            std::lock_guard lk(st_barrier_flags_mu);
            for (const auto& [bits, n] : st_barrier_flags) {
                char b[48];
                snprintf(b, sizeof b, " %#x: %.1f;", (unsigned)bits, n / 120.0);
                fl += b;
            }
            logf("vk: a frame: the game's barriers by their NVN flags:%s", fl.c_str());
            st_barrier_flags.clear();
        }
        static int64_t l_gpu, l_gpu_up;
        static uint64_t l_made;
        logf("vk: GPU time %.1f ms a frame (the longest submission %.1f ms); %.1f textures made a frame; (Triforce) the uploads' command "
             "buffers %.1f ms a frame more (the longest %.1f ms)",
             (st_gpu_ns - l_gpu) / 120.0 / 1e6, st_gpu_max_ns / 1e6, (texture_made() - l_made) / 120.0,
             (st_gpu_up_ns - l_gpu_up) / 120.0 / 1e6, st_gpu_up_max_ns / 1e6);
        l_gpu_up = st_gpu_up_ns, st_gpu_up_max_ns = 0;
        {   // (issue 109: the movie networks too slow on this card: left out from now on, vk_draw.cpp movie_networks_slow)
            static const bool keep_all = getenv("RX_VK_NETWORK_KEEP") && !strcmp(getenv("RX_VK_NETWORK_KEEP"), "all");
            const double gpu_ms = (st_gpu_ns - l_gpu) / 120.0 / 1e6, draws = (d - l_d) / 120.0;
            if (!keep_all && gpu_ms > 40.0 && draws < 40.0 && !movie_networks_slow.exchange(true))
                logf("vk: %.0f ms of GPU time a frame for %.0f draws (a movie): its upscaler networks are left out from now on "
                     "(a linear upscale; RX_VK_NETWORK_KEEP=all keeps them)", gpu_ms, draws);
        }
        l_made = texture_made();
        {
            static int64_t l_ub, l_un;
            int64_t ub = 0, un = 0;
            texture_unswizzle_stats(&ub, &un);
            logf("vk: a frame: %.2f MB unswizzled on the CPU in %.2f ms", (ub - l_ub) / 120.0 / 1048576.0,
                 (un - l_un) / 120.0 / 1e6);
            l_ub = ub, l_un = un;
        }
        l_gpu = st_gpu_ns, st_gpu_max_ns = 0;
        gprof_report(120);
        l_d = d, l_sk = sk, l_di = di, l_pass = passes, l_sub = st_submits, l_up = up, l_upb = upb, l_bar = st_barriers;
        l_ubu = st_ub_updates, l_ubo = st_ub_overlays, l_wait = st_wait_ns;
        fps_t = now, fps_n = 0;
    }
}

// ---- (Triforce) The hold of the last picture (present_hold.h, the owner's choice): a frame drawn with a texture still owed
// its first bytes (vk_texture.cpp new_texture_later, the frame's uploads past their budget) is not shown; the window gets the
// last complete picture again, the Insert menu, the counters and the notices drawn over it as over any frame (present_image),
// until a frame is drawn with every texture complete or the hold reaches its bound. The picture is a copy of the window
// texture shown last, taken at the hold's first frame (that texture is not the one the held frame drew into, and nothing has
// drawn into it since: the game draws its frames into its window textures in turn), in an image of its own kept while holds
// come (let go 1800 presents after the last). RX_VK_HOLD_MS=<ms>: the bound (250); 0: no hold and no new texture left for
// later, the frame that takes them all at once as before
int64_t hold_bound_ns() {
    static const int64_t v = (int64_t)((getenv("RX_VK_HOLD_MS") ? atof(getenv("RX_VK_HOLD_MS")) : 250.0) * 1e6);
    return v > 0 ? v : 0;
}
present_hold::State hold;
struct HoldNow { bool held = false; double ms = 0; } hold_now;   // (this present's, for the stall line)
double hold_stretch_longest = 0;   // (the longest hold that ended since the last 600-frame line, ms)
std::atomic<bool> held_last{false};   // (the last present held its frame: present_held_last)
std::atomic<bool> held_now{false};    // (the present under way shows the held picture: present_hold_now)
std::atomic<bool> after_hold_now{false};   // (the present under way is the first shown after a hold: present_after_hold)
thread_local bool smooth_mine = false;     // ((Triforce) the present under way is one of the game's, from do_present: smooth_present)
/// The window texture shown last, for the held picture's copy: the texture (while texture_gone says it is there still), or
/// the present's own copy of it in the presented format (do_present's Retyped ring, which a held frame does not touch)
struct LastShown {
    bool on = false;
    const Texture* tex = nullptr;
    uint32_t id = 0;
    uint64_t gone = 0;
    VkImage copy = VK_NULL_HANDLE;
    VkFormat f = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
    int crop[4] = {0, 0, 0, 0};
    bool has_crop = false, pq = false;
} last_shown;
struct HeldPicture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat f = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
    int crop[4] = {0, 0, 0, 0};
    bool has_crop = false, pq = false;
    bool valid = false;      // (holds the picture of the hold under way)
    int64_t used_at = 0;     // (the present it was last shown at)
} held_pic;
void held_drop() {
    if (held_pic.view) vkDestroyImageView(dev.device, held_pic.view, nullptr);
    if (held_pic.image) destroy_image(held_pic.image, held_pic.alloc);
    held_pic = HeldPicture{};
}
/// At a hold's first frame, in its present's recording: the picture shown last copied into the held image. False when there
/// is none to take (no frame shown yet, the texture gone, or the held frame drew into that same texture): the hold then
/// presents nothing, and the window keeps the picture it was given last, the overlays with it
bool held_take(VkCommandBuffer cmd, const Texture* now) {
    held_pic.valid = false;
    if (!last_shown.on || !last_shown.w || !last_shown.h || last_shown.f == VK_FORMAT_UNDEFINED) return false;
    VkImage from = last_shown.copy;
    if (!from) {
        if (!last_shown.tex || last_shown.tex == now || texture_destroyed_since(last_shown.id, last_shown.gone)) return false;
        from = last_shown.tex->image;
    }
    if (!from) return false;
    if (held_pic.image && (held_pic.f != last_shown.f || held_pic.w != last_shown.w || held_pic.h != last_shown.h)) {
        std::lock_guard qk(dev.present_mu);   // (the old one may still be in a present)
        device_wait_idle_locked();
        held_drop();
    }
    if (!held_pic.image) {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D, ic.format = last_shown.f, ic.extent = {last_shown.w, last_shown.h, 1};
        ic.mipLevels = 1, ic.arrayLayers = 1, ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!create_image(ic, held_pic.image, held_pic.alloc)) {
            held_pic = HeldPicture{};
            static int told;
            if (told++ < 4) logf("vk: hold: no image for the held picture could be made: the window keeps its last one");
            return false;
        }
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = held_pic.image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = last_shown.f;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(dev.device, &vi, nullptr, &held_pic.view) != VK_SUCCESS) held_pic.view = VK_NULL_HANDLE;
        held_pic.f = last_shown.f, held_pic.w = last_shown.w, held_pic.h = last_shown.h;
        image_barrier(cmd, held_pic.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }
    full_barrier(cmd);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {last_shown.w, last_shown.h, 1};
    vkCmdCopyImage(cmd, from, VK_IMAGE_LAYOUT_GENERAL, held_pic.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(cmd);
    std::memcpy(held_pic.crop, last_shown.crop, sizeof held_pic.crop);
    held_pic.has_crop = last_shown.has_crop, held_pic.pq = last_shown.pq;
    held_pic.valid = true;
    return true;
}
/// At the top of each present: this frame held or shown, the mode of the next frame's draws, and a line for each hold that ended
bool hold_step() {
    const int64_t bound = dev.offscreen ? 0 : hold_bound_ns();
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const uint64_t present = (uint64_t)presents + 1;   // (this one's number: do_present counts it further down)
    // ((Triforce) and a frame with draws left out because their pipeline was still being made: held the same way (NX2navi's
    // frame hold; vk_draw.cpp, pipe_wait.h). RX_VK_HOLD_PIPELINES=0: those frames are shown, as before)
    const int64_t left_out = draw_left_out_take();
    const bool held = present_hold::decide(hold, bound, texture_frame_owed() > 0 || left_out > 0, now, present);
    hold_now.held = held, hold_now.ms = held ? (now - hold.since_ns) / 1e6 : 0;
    texture_hold_mode(present_hold::mode(hold, bound, present + 1));
    draw_hold_mode(present_hold::mode(hold, bound, present + 1));
    held_last.store(held, std::memory_order_relaxed);
    if (hold.ended_frames) {
        const double ms = hold.ended_ns / 1e6;
        hold_stretch_longest = std::max(hold_stretch_longest, ms);
        static int told;
        if (told++ < 40 || hold.holds % 100 == 0) {
            char rest[160] = "";
            if (hold.ended_bounded)
                snprintf(rest, sizeof rest, "; it reached its bound: the rest were uploaded at once, and no picture is held for the "
                         "next %llu presents", (unsigned long long)(hold.rest_until - present));
            logf("vk: hold: the last picture was held %.0f ms (%lld frames not shown) while new textures or pipelines came in%s (%lld holds so far; "
                 "RX_VK_HOLD_MS=0: none)", ms, (long long)hold.ended_frames, rest, (long long)hold.holds);
        }
    }
    if (!hold.holding && held_pic.image && presents > held_pic.used_at + 1800) held_drop();   // (long done with on the GPU)
    return held;
}

/// (hookshot) Once a game frame: every 600 a status line (the video memory, DLSS's details while it draws), and every 60
/// a look at the driver's allowance, warned of once when the use passes it (again only after it was back under)
void status_tick() {
    namespace sl = status_line;
    static uint64_t frames = 0;
    const int lab_col = lab_ab::phase_tick();   // (lab: this frame's A/B phase, the trace's last column)
    // (the slowest frame since the last line, present to present: a stall shows here, to be matched with what the log says
    // around it, such as DLSS's feature made again)
    static int64_t last_ns = 0, slowest_ns = 0;
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (last_ns) slowest_ns = std::max(slowest_ns, now_ns - last_ns);
    // (RX_VK_FRAME_TRACE=1: every frame a line of triforce_frames.csv beside the game, present to present and what the frame did,
    // so that each stutter can be matched to its cause: waits for shaders or pipelines, translation, uploads, the memory sync,
    // pipeline compiles, the GPU behind. RX_VK_FRAME_LOG has the frame times alone)
    // ((Triforce, wp20) and after those columns the frame split as the stall line splits a long one: the replay, the present, what
    // is outside them (the game's own update), and of ours within: frame_trace.h. scripts/frame_rank.py ranks the slow frames by it)
    static FILE* trace = [] {
        if (!getenv("RX_VK_FRAME_TRACE") || !atoi(getenv("RX_VK_FRAME_TRACE"))) return (FILE*)nullptr;
        char exe[MAX_PATH];
        const DWORD n = game_module_path(exe, MAX_PATH);
        std::string dir(exe, n);
        FILE* f = fopen((dir.substr(0, dir.find_last_of("\\/") + 1) + "triforce_frames.csv").c_str(), "w");
        if (f) fprintf(f, "frame,ms,pipe_waits,pipe_wait_ms,left_out,translate_ms,uploads,upload_mb,textures,memsync_ms,pipelines_ms,"
                          "uploads_ms,programs_ms,slot_wait_ms,gpu_behind_ms,draw_ms,desc_ms,cmds_ms,tex_lookup_ms,textures_gone,"
                          "dummies,draws,%s", frame_trace::kColumns);
        if (f) fputs(",lab\n", f);   // ((Triforce, lab) the in-run A/B phase, last: lab_ab.py reads it by name)
        return f;
    }();
    if (trace && last_ns) {
        static int64_t l_ps, l_pw, l_up, l_upb, l_wait, l_prof[PROF_COUNT];
        static double l_pwms, l_tr;
        int64_t ps = 0, pw = 0, up = 0, astc = 0, upb = 0;
        double pwms = 0;
        draw_stats3(&ps, &pw, &pwms);
        texture_stats(&up, &astc, &upb);
        const double tr = shader_translate_ms();
        const auto dp = [](int i) { return (prof_ns[i] - l_prof[i]) / 1e6; };
        static uint64_t l_gone;
        const uint64_t gone = texture_destroy_generation();
        static int64_t l_dummy, l_draws;   // (Triforce: the bindings that sampled the dummy, and the draws, this frame)
        const int64_t dummy = draw_dummy_textures();
        int64_t draws_now = 0, sk_now = 0, di_now = 0, pl_now = 0;
        draw_stats(&draws_now, &sk_now, &di_now, &pl_now);
        // ((Triforce, wp20) the split: this frame's from the counters the stall line reads (frame_trace.h)
        static int64_t l_present_ns, l_game_ns, l_cap_ns, l_wb_ns, l_wb_held_ns, l_qsbg_ns, l_par_wait;
        MemoryCaptureStats caps;
        memory_capture_stats(&caps);
        const int64_t cap_ns = caps.ns + caps.wait_ns;
        const int64_t present_ns = stall_ct.present_ns.load(std::memory_order_relaxed), game_ns = stall_ct.game_wait_ns.load(std::memory_order_relaxed),
                      wb_ns = stall_ct.wb_wait_ns.load(std::memory_order_relaxed), wb_held_ns = stall_ct.wb_held_ns.load(std::memory_order_relaxed),
                      qsbg_ns = stall_ct.qs_bg_ns.load(std::memory_order_relaxed);
        // (the replay thread's wait for the helpers' segments: its time in their part less its own recording and the submits, as
        // the parallel replay line has it)
        const int64_t par_wait = par_st.wait_ns.load() - par_st.own_ns.load() - par_st.submit_ns.load();
        frame_trace::Split sp;
        sp.interval = (now_ns - last_ns) / 1e6, sp.replay = dp(PROF_SUBMIT) + dp(PROF_PRESENT);
        sp.present = (present_ns - l_present_ns) / 1e6;
        sp.dispatch = dp(PROF_DISPATCH), sp.qsubmit = dp(PROF_QSUBMIT);
        sp.par_wait = (par_wait - l_par_wait) / 1e6;
        sp.game_wait = (game_ns - l_game_ns) / 1e6;
        sp.captures = (cap_ns - l_cap_ns) / 1e6;
        sp.wb_wait = (wb_ns - l_wb_ns) / 1e6, sp.wb_held = (wb_held_ns - l_wb_held_ns) / 1e6;
        sp.qs_bg = (qsbg_ns - l_qsbg_ns) / 1e6;
        char split[256];
        frame_trace::line(sp, split, sizeof split);
        char split_lab[288];   // ((Triforce, lab) the in-run A/B phase after the split: the line's last column, 'lab')
        snprintf(split_lab, sizeof split_lab, "%s,%d", split, lab_col);
        l_present_ns = present_ns, l_game_ns = game_ns, l_cap_ns = cap_ns, l_wb_ns = wb_ns, l_wb_held_ns = wb_held_ns;
        l_qsbg_ns = qsbg_ns, l_par_wait = par_wait;
        fprintf(trace, "%llu,%.2f,%lld,%.2f,%lld,%.1f,%lld,%.2f,%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%llu,%lld,%lld,%s\n",
                (unsigned long long)frames, (now_ns - last_ns) / 1e6, (long long)(pw - l_pw), pwms - l_pwms, (long long)(ps - l_ps),
                tr - l_tr, (long long)(up - l_up), (upb - l_upb) / 1048576.0, texture_count(), dp(PROF_MEMSYNC), dp(PROF_PIPE),
                dp(PROF_UPLOAD), dp(PROF_PROGRAM), dp(PROF_SLOTWAIT), (st_wait_ns - l_wait) / 1e6, dp(PROF_DRAW), dp(PROF_DESC),
                dp(PROF_CMDS), dp(PROF_TEXGET), (unsigned long long)(gone - l_gone), (long long)(dummy - l_dummy),
                (long long)(draws_now - l_draws), split_lab);
        l_gone = gone, l_dummy = dummy, l_draws = draws_now;
        if (frames % 60 == 0) fflush(trace);
        l_ps = ps, l_pw = pw, l_pwms = pwms, l_up = up, l_upb = upb, l_tr = tr, l_wait = st_wait_ns;
        std::memcpy(l_prof, prof_ns, sizeof l_prof);
    }
    {   // (Triforce, stall_watch.h) every frame's time into the 600-frame line, present to present and the replay's own (its
        // submits and its present, on whichever threads replay), and a line of where the time of a long one went.
        // RX_VK_STALL_LOG=0: neither; =<ms>: a stall line from that long (default 50)
        namespace sw = stall_watch;
        static const double stall_ms = getenv("RX_VK_STALL_LOG") ? atof(getenv("RX_VK_STALL_LOG")) : sw::kStallMs;
        static sw::Histogram h_present, h_replay;
        static sw::Bound bound;
        static int64_t s_prof[PROF_COUNT], s_part[3], s_wb_ns, s_wb_n, s_wb_bp, s_game_ns, s_game_n, s_present_ns, s_gpu_ns;
        static int64_t s_wb_un, s_wb_un_ns;
        static int64_t s_wb_held, s_wb_held_ns;   // ((Triforce, wp20) the waits that went on after their writer's recording was done)
        static int64_t s_splits, s_lookups, s_pk_lock, s_pk_codes, s_pk_n, s_pk_ns;
        static int64_t s_mk, s_mk_ns, s_mk_later, s_im, s_im_ns, s_rt, s_rt_ns;
        static int64_t s_qsbg, s_qsbg_ns, s_given, s_ahead;
        static int64_t s_present_wait;   // ((Triforce) present_image's waits for the present before, RX_VK_PRESENT_THREAD)
        static int64_t s_ps, s_pw, s_up, s_upb, s_frames, s_lines, s_imp, s_imp_ns, s_pr, s_pr_ns, s_pa, s_later;
        static double s_pwms;
        static int64_t s_new_later, s_new_later_line, s_holds, s_held_frames, s_held_ns, s_bounded;   // ((Triforce) the hold of the last picture)
        if (stall_ms > 0) {
            int64_t ps = 0, pw = 0, up = 0, astc = 0, upb = 0, part[3], imp = 0, imp_ns = 0, pr = 0, pr_ns = 0, pa = 0;
            double pwms = 0;
            draw_stats3(&ps, &pw, &pwms);
            texture_stats(&up, &astc, &upb);
            memory_import_stats(&imp, &imp_ns);
            program_cache_stats(&pr, &pr_ns, &pa);
            int64_t dp_pick = 0, dp_pipe = 0, dp_ready = 0, dp_made = 0, raw_n = 0, raw_ns = 0;   // ((Triforce, test19))
            dispatch_split_stats(&dp_pick, &dp_pipe, &dp_ready, &dp_made);
            readahead_wait_stats(&raw_n, &raw_ns);
            static int64_t s_dp_pick, s_dp_pipe, s_dp_ready, s_dp_made, s_raw_n, s_raw_ns;
            int64_t pf = 0, pah = 0, por = 0, pln = 0, plb = 0;   // ((Triforce, wp18) the pipelines new to the draws, and made ahead)
            pipeline_ahead_stats(&pf, &pah, &por, &pln, &plb);
            static int64_t s_pf, s_pah, s_por, s_pln, s_plb;
            const int64_t later = texture_uploads_later();
            const int64_t new_later = texture_new_later();
            const int64_t splits = texture_split_parts(), lookups = draw_view_lookups();
            int64_t pk_lock = 0, pk_codes = 0, pk_n = 0, pk_ns = 0;
            program_pick_stats(&pk_lock, &pk_codes, &pk_n, &pk_ns);
            int64_t once_n = 0, once_ns = 0, relock_ns = 0;   // ((Triforce) the draws' reads with the shaders' lock let go)
            program_read_stats(&once_n, &once_ns, &relock_ns);
            static int64_t s_once_n, s_once_ns, s_relock_ns;
            int64_t why_un = 0, why_go = 0, why_ahead = 0, by_order = 0;   // ((Triforce) why the draws read what they read; the sessions' order)
            program_order_stats(&why_un, &why_go, &why_ahead, &by_order);
            static int64_t s_why_un, s_why_go, s_why_ahead, s_by_order;
            int64_t mk = 0, mk_ns = 0, mk_later = 0, im = 0, im_ns = 0, rt = 0, rt_ns = 0;
            texture_create_stats(&mk, &mk_ns, &mk_later);
            vma_image_stats(&im, &im_ns, &rt, &rt_ns);
            int64_t given = 0, ahead_n = 0;
            memory_import_churn(&given, &ahead_n);
            const int64_t qsbg = stall_ct.qs_bg.load(std::memory_order_relaxed), qsbg_ns = stall_ct.qs_bg_ns.load(std::memory_order_relaxed);
            for (int i = 0; i < 3; i++) part[i] = memsync_part_ns[i].load(std::memory_order_relaxed);
            const int64_t wb_ns = stall_ct.wb_wait_ns.load(std::memory_order_relaxed), wb_n = stall_ct.wb_waits.load(std::memory_order_relaxed),
                          wb_bp = stall_ct.wb_behind_present.load(std::memory_order_relaxed),
                          wb_un = stall_ct.wb_unsent.load(std::memory_order_relaxed), wb_un_ns = stall_ct.wb_unsent_ns.load(std::memory_order_relaxed),
                          game_ns = stall_ct.game_wait_ns.load(std::memory_order_relaxed), game_n = stall_ct.game_waits.load(std::memory_order_relaxed),
                          present_ns = stall_ct.present_ns.load(std::memory_order_relaxed);
            const int64_t wb_held = stall_ct.wb_held.load(std::memory_order_relaxed), wb_held_ns = stall_ct.wb_held_ns.load(std::memory_order_relaxed);
            const int64_t present_wait = present_waited_ns();
            MemoryCaptureStats caps;   // ((Triforce) the submit-time captures on the game's threads)
            memory_capture_stats(&caps);
            const int64_t cap_ns = caps.ns + caps.wait_ns;
            static int64_t s_cap_ns;
            const uint64_t ahead = stall_ct.wb_ahead_max.exchange(0, std::memory_order_relaxed);
            if (last_ns) {
                static const char* names[PROF_COUNT] = {"submit", "present", "draw", "descriptors", "texture lookups", "uploads",
                                                        "pipelines", "programs", "commands", "dispatch", "memory sync",
                                                        "queue submit", "slot wait"};
                double d[PROF_COUNT];
                for (int i = 0; i < PROF_COUNT; i++) d[i] = (prof_ns[i] - s_prof[i]) / 1e6;
                sw::Frame f;
                f.no = (int64_t)frames, f.interval = (now_ns - last_ns) / 1e6, f.replay = d[PROF_SUBMIT] + d[PROF_PRESENT];
                h_present.add(f.interval), h_replay.add(f.replay);
                if (f.interval >= stall_ms && bound.take(now_ns)) {
                    f.phases = PROF_COUNT, f.names = names, f.ms = d;
                    f.maps = (part[0] - s_part[0]) / 1e6, f.capture = (part[1] - s_part[1]) / 1e6, f.collect = (part[2] - s_part[2]) / 1e6;
                    f.wb_wait = (wb_ns - s_wb_ns) / 1e6, f.wb_waits = wb_n - s_wb_n, f.wb_ahead = (int64_t)ahead, f.wb_behind_present = wb_bp - s_wb_bp;
                    f.wb_unsent = wb_un - s_wb_un, f.wb_unsent_ms = (wb_un_ns - s_wb_un_ns) / 1e6;
                    f.wb_held = wb_held - s_wb_held, f.wb_held_ms = (wb_held_ns - s_wb_held_ns) / 1e6;
                    f.pipe_waits = pw - s_pw, f.pipe_wait = pwms - s_pwms, f.left_out = ps - s_ps;
                    f.pipes_first = pf - s_pf, f.pipes_ahead = pah - s_pah, f.pipes_order = por - s_por;
                    f.pipes_links = pln - s_pln, f.pipes_libs = plb - s_plb;
                    f.uploads = up - s_up, f.upload_mb = (upb - s_upb) / 1048576.0;
                    f.gpu_bound = (st_wait_ns - s_gpu_ns) / 1e6;
                    f.present = (present_ns - s_present_ns) / 1e6, f.present_wait = (present_wait - s_present_wait) / 1e6;
                    f.game_wait = (game_ns - s_game_ns) / 1e6, f.game_waits = game_n - s_game_n;
                    f.imports = imp - s_imp, f.import_ms = (imp_ns - s_imp_ns) / 1e6;
                    f.prog_reads = pr - s_pr, f.prog_read_ms = (pr_ns - s_pr_ns) / 1e6, f.prog_ahead = pa - s_pa;
                    f.uploads_later = later - s_later;
                    f.new_later = new_later - s_new_later, f.held = hold_now.held, f.hold_ms = hold_now.ms;
                    f.trial_phase = memory_demote_trial(&f.trial_pool);
                    f.split_parts = splits - s_splits, f.view_lookups = lookups - s_lookups;
                    // ((Triforce) the picks' waits to take the lock again after a read with it let go are waits for it too)
                    f.pick_lock_ms = (pk_lock - s_pk_lock + relock_ns - s_relock_ns) / 1e6, f.pick_codes_ms = (pk_codes - s_pk_codes) / 1e6;
                    f.once_waits = once_n - s_once_n, f.once_wait_ms = (once_ns - s_once_ns) / 1e6;
                    f.why_unnamed = why_un - s_why_un, f.why_let_go = why_go - s_why_go, f.why_ahead = why_ahead - s_why_ahead;
                    f.order_taken = by_order - s_by_order;
                    f.pick_inline = pk_n - s_pk_n, f.pick_inline_ms = (pk_ns - s_pk_ns) / 1e6;
                    f.made = mk - s_mk, f.made_ms = (mk_ns - s_mk_ns) / 1e6, f.made_later = mk_later - s_mk_later;
                    f.images = im - s_im, f.images_ms = (im_ns - s_im_ns) / 1e6, f.targets = rt - s_rt, f.targets_ms = (rt_ns - s_rt_ns) / 1e6;
                    f.qs_bg = qsbg - s_qsbg, f.qs_bg_ms = (qsbg_ns - s_qsbg_ns) / 1e6;
                    f.disp_pick_ms = (dp_pick - s_dp_pick) / 1e6, f.disp_pipe_ms = (dp_pipe - s_dp_pipe) / 1e6;
                    f.disp_ready = dp_ready - s_dp_ready, f.disp_made = dp_made - s_dp_made;
                    f.ra_waits = raw_n - s_raw_n, f.ra_wait_ms = (raw_ns - s_raw_ns) / 1e6;
                    f.given_back = given - s_given, f.ahead_sent = ahead_n - s_ahead;
                    f.captures = (cap_ns - s_cap_ns) / 1e6;
                    char line[2048];
                    sw::stall_line(f, line, sizeof line);
                    logf("%s", line);
                    s_lines++;
                }
                if (++s_frames % sw::kEvery == 0) {
                    char a[160], b[160];
                    sw::describe(h_present, a, sizeof a);
                    sw::describe(h_replay, b, sizeof b);
                    const int64_t held = bound.held_since();
                    char c[240] = "";
                    if (hold_bound_ns()) {   // ((Triforce) the holds of the last picture that ended in these frames)
                        sw::Holds hs;
                        hs.holds = hold.holds - s_holds, hs.frames = hold.held_frames - s_held_frames, hs.bounded = hold.bounded - s_bounded;
                        hs.new_later = new_later - s_new_later_line, hs.ms = (hold.held_ns - s_held_ns) / 1e6, hs.longest = hold_stretch_longest;
                        c[0] = ';', c[1] = ' ';
                        sw::describe_holds(hs, hold_bound_ns() / 1e6, c + 2, sizeof c - 2);
                        s_holds = hold.holds, s_held_frames = hold.held_frames, s_bounded = hold.bounded, s_held_ns = hold.held_ns;
                        s_new_later_line = new_later, hold_stretch_longest = 0;
                    }
                    logf("vk: frame times (%d frames): present to present %s; the replay's own %s; %lld stall lines (frames of %.0f ms "
                         "or more), %lld more not logged (at most 3 a second, 1500 a session)%s", sw::kEvery, a, b, (long long)s_lines,
                         stall_ms, (long long)held, c);
                    h_present.reset(), h_replay.reset();
                    s_lines = 0;
                }
            }
            std::memcpy(s_prof, prof_ns, sizeof s_prof);
            std::memcpy(s_part, part, sizeof s_part);
            s_wb_ns = wb_ns, s_wb_n = wb_n, s_wb_bp = wb_bp, s_game_ns = game_ns, s_game_n = game_n, s_present_ns = present_ns;
            s_present_wait = present_wait;
            s_cap_ns = cap_ns;
            s_wb_un = wb_un, s_wb_un_ns = wb_un_ns;
            s_wb_held = wb_held, s_wb_held_ns = wb_held_ns;
            s_gpu_ns = st_wait_ns;
            s_ps = ps, s_pw = pw, s_pwms = pwms, s_up = up, s_upb = upb, s_imp = imp, s_imp_ns = imp_ns;
            s_pr = pr, s_pr_ns = pr_ns, s_pa = pa, s_later = later;
            s_new_later = new_later;
            s_splits = splits, s_lookups = lookups, s_pk_lock = pk_lock, s_pk_codes = pk_codes, s_pk_n = pk_n, s_pk_ns = pk_ns;
            s_mk = mk, s_mk_ns = mk_ns, s_mk_later = mk_later, s_im = im, s_im_ns = im_ns, s_rt = rt, s_rt_ns = rt_ns;
            s_qsbg = qsbg, s_qsbg_ns = qsbg_ns, s_given = given, s_ahead = ahead_n;
            s_dp_pick = dp_pick, s_dp_pipe = dp_pipe, s_dp_ready = dp_ready, s_dp_made = dp_made, s_raw_n = raw_n, s_raw_ns = raw_ns;
            s_pf = pf, s_pah = pah, s_por = por, s_pln = pln, s_plb = plb;
            s_once_n = once_n, s_once_ns = once_ns, s_relock_ns = relock_ns;
            s_why_un = why_un, s_why_go = why_go, s_why_ahead = why_ahead, s_by_order = by_order;
        }
    }
    last_ns = now_ns;
    if (++frames % sl::kCheckEvery) return;
    uint64_t used = 0, allowed = 0;
    const bool known = vram_usage_now(&used, &allowed);
    static bool told_unknown = false;
    if (!known && !told_unknown) {
        told_unknown = true;
        logf("vk: status: the driver gives no video-memory budget (VK_EXT_memory_budget): only the game's own allocations are known");
    }
    char dlss[320] = "";
    const bool line = frames % sl::kEvery == 0;
    const int mode = line ? temporal_status(dlss, sizeof dlss) : temporal_mode();   // (temporal_status: since the last line, so only for one)
    static bool over = false;
    if (known && sl::crossed(over, used, allowed)) {
        char w[200];
        sl::over_text(w, sizeof w, used, allowed, mode);
        logf("%s", w);
    }
    if (!line) return;
    char v[128];
    sl::vram_text(v, sizeof v, used, allowed, known);
    logf("vk: status: %d frames, the slowest %.1f ms; %s%s", sl::kEvery, slowest_ns / 1e6, v, dlss);
    slowest_ns = 0;
}

// (local fix, from the old HDR port) a colour format's bytes a texel, for the present's copy between formats of one size (0: not one of these)
static uint32_t present_texel_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R32_SFLOAT:
        return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R32G32_SFLOAT:
        return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    default:
        return 0;
    }
}
uint64_t present_value = 0;   // (the present job's value, handed out as it came; 0: the next one)
struct PendingShot {
    bool on = false;
    uint64_t value = 0;
    int64_t presents = 0;
    std::shared_ptr<std::vector<uint8_t>> pixels;
};
std::shared_ptr<std::vector<uint8_t>> shot_pixels(uint64_t at_value, int64_t at_present) {
    wait_value(at_value);
    // The completed transfer may target non-coherent host memory.
    invalidate_mapped(shot_allocs[at_present & 1], (uint64_t)kShotW * kShotH * 4);
    const uint8_t* const sm = (const uint8_t*)shot_maps[at_present & 1];
    return std::make_shared<std::vector<uint8_t>>(sm, sm + (size_t)kShotW * kShotH * 4);
}
void do_present(const nvn_tex* t, const int crop[4]) {
    static PendingShot pending;
    // (hookshot) NVIDIA Reflex (vk_reflex.cpp): this frame's simulation ended, its submission starts; on the way out (after the
    // present) the driver's sleep until the next frame should begin. AMD Anti-Lag 2 (vk_antilag.cpp) waits there as well: the
    // next frame's input stage
    renodx_present();   // (local, RenoDX port) its settings as the menu has them now
    temporal_idle();   // (DLSS, FSR: their video memory let go once not used for a while)
    ngxfg::idle();   // (DLSS Frame Generation's too)
    hudless::idle();   // (the picture without the HUD, kept for it and for AMD's frame generation)
    framegen_engine_tick();   // (who makes the frames: AMD's library has a swapchain of its own, switched here, with the display thread idle)
    // (Triforce) the hold of the last picture: whether this frame is shown, before the stall line that says so
    const bool was_held = held_last.load(std::memory_order_relaxed);
    const bool held = hold_step();
    after_hold_now.store(was_held && !held, std::memory_order_relaxed);
    status_tick();   // (the status line, and the video-memory warning)
    reflex_present_begins();
    // (Triforce, NVIDIA DLSS frame generation, vk_dlssg.cpp: Streamline's Reflex sleeps there instead, and the next
    // frame's latency markers start)
    // (Triforce: its time is the present's in the stall line)
    struct ReflexSleep {
        ~ReflexSleep() {
            const auto t0 = std::chrono::steady_clock::now();
            reflex_sleep(); antilag_input(); dlssg::frame_start();
            if (vk_ft_on()) vk_ft("reflex slept %.3f ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            stall_ct.present_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                                          std::memory_order_relaxed);
        }
    } reflex_sleep_after;
    {   // (hookshot, RX_VK_PASS_TRACE_AT=<frames>: the passes of the frames around these frame numbers, for finding what a frame is made of)
        static std::atomic<int64_t> seen{0};
        static const int64_t at = getenv("RX_VK_PASS_TRACE_AT") ? atoll(getenv("RX_VK_PASS_TRACE_AT")) : 0;
        if (at && ++seen % at == 0) pass_traces_log(texture_frame_no() - 1, texture_frame_no());
    }
    static const char* shots = getenv("RX_VK_SHOTS");
    static const int shot_every = getenv("RX_VK_SHOT_EVERY") ? std::max(1, atoi(getenv("RX_VK_SHOT_EVERY"))) : 300;
    VkImage src = VK_NULL_HANDLE;
    VkImageView direct_view = VK_NULL_HANDLE;
    uint32_t sw = 0, sh = 0;
    bool shot = false, interpolated = false;
    VkImage made_src = VK_NULL_HANDLE;
    const int* show_crop = crop;   // ((Triforce) a held frame's: the held picture's own)
    {
        std::lock_guard lk(replay_mu);
        bench_frame(presents);
        SlowWatch slow("present");
        frame_submits.clear();
        PROF(PROF_PRESENT);
        const uint64_t value = present_value ? present_value : ++q_values;
        present_value = 0;
        rec_value_now = value;
        ub_first_of_recording = ub_updates.size();
        Recording& r = begin_recording(value);
        memory_begin_submission();
        Texture* tex = t ? texture_for_present(t) : nullptr;
        static int logged_p;
        if (logged_p++ < 4)
            logf("vk: present texture %#llx %ux%u format %#x: %s", (unsigned long long)(t ? t->gpu : 0),
                 t ? t->d.width : 0, t ? t->d.height : 0, t ? t->d.format : 0,
                 tex ? (tex->gpu_written ? "rendered" : "never rendered") : "no image");
        if (tex) src = tex->image, sw = tex->width, sh = tex->height;
        // (Triforce) a held frame shows the held picture (taken at the hold's first frame), or nothing; no frame is made from it
        // by frame generation, and the one shown after a hold starts frame generation's history anew (framegen_new_history)
        if (held) {
            const bool have = hold.frames == 1 ? held_take(r.cmd, tex) : held_pic.valid;
            src = have ? held_pic.image : VK_NULL_HANDLE;
            sw = have ? held_pic.w : 0, sh = have ? held_pic.h : 0;
            if (have) held_pic.used_at = presents;
            if (!dlssg::active() || !have) {   // (its depth and motion are never a made frame's; DLSS frame generation's frame drops them itself)
                fsr3_inputs::Frame dropped;
                fsr3_inputs::take(dropped);
            }
        } else if (was_held && tex) {
            framegen_new_history();
        }
        if (held) show_crop = held_pic.has_crop ? held_pic.crop : nullptr;
        // (local fix, from the old HDR port) the present's own format, when the image found at the window texture's storage was
        // made in another format of the same texel size: HDR turned to Native in play, the game writes RGB10A2 into the window
        // textures that were made as RGBA8. Their bits are copied as they are into an image of the present's format, so a
        // blit reads them as what they are. Each change of the presented format is logged
        const VkFormat want_fmt = t ? vk_format((int)t->d.format).format : VK_FORMAT_UNDEFINED;
        {
            static uint32_t last_fmt = ~0u;
            if (t && (uint32_t)t->d.format != last_fmt) {
                last_fmt = (uint32_t)t->d.format;
                logf("vk: present format %#x (vk %d): the image there is vk %d, %s", (unsigned)t->d.format, (int)want_fmt,
                     tex ? (int)tex->format : -1, tex ? (tex->gpu_written ? "rendered" : "never rendered") : "no image");
            }
        }
        // ((Triforce, FG-5) the retyped copy's view: frame generation copies the frame from it, not from the window texture's
        // own view, which read the game's RGB10A2 bits as RGBA8: wrong colours, not only 8 bits)
        VkImageView retyped_view = VK_NULL_HANDLE;
        if (tex && src && !dev.offscreen && !held) {
            const uint32_t bytes = present_texel_bytes(want_fmt);
            if (want_fmt != VK_FORMAT_UNDEFINED && tex->format != want_fmt && bytes && bytes == present_texel_bytes(tex->format)) {
                struct Retyped { VkImage image = VK_NULL_HANDLE; VmaAllocation alloc = nullptr; VkFormat f = VK_FORMAT_UNDEFINED; uint32_t w = 0, h = 0; VkImageView view = VK_NULL_HANDLE; };
                static Retyped ring[4];   // (frames in flight: the display thread blits one while the next is recorded)
                static unsigned at_ring;
                Retyped& rt = ring[at_ring++ % 4];
                if (rt.f != want_fmt || rt.w != sw || rt.h != sh) {
                    if (rt.image) {
                        {
                            std::lock_guard qk(dev.present_mu);   // (the old one may still be in a blit)
                            device_wait_idle_locked();
                        }
                        if (rt.view) vkDestroyImageView(dev.device, rt.view, nullptr);
                        destroy_image(rt.image, rt.alloc);
                        rt = Retyped{};
                    }
                    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                    ic.imageType = VK_IMAGE_TYPE_2D, ic.format = want_fmt, ic.extent = {sw, sh, 1}, ic.mipLevels = 1, ic.arrayLayers = 1;
                    ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
                    ic.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                    if (create_image(ic, rt.image, rt.alloc)) {
                        rt.f = want_fmt, rt.w = sw, rt.h = sh;
                        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                        vc.image = rt.image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = want_fmt;
                        vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        if (vkCreateImageView(dev.device, &vc, nullptr, &rt.view) != VK_SUCCESS) rt.view = VK_NULL_HANDLE;
                        image_barrier(r.cmd, rt.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                        static int told;
                        if (told++ < 4) logf("vk: present: the window texture's image is vk format %d, the present's %d: copied as %d",
                                             (int)tex->format, (int)want_fmt, (int)want_fmt);
                    } else {
                        rt = Retyped{};
                    }
                }
                if (rt.image) {
                    full_barrier(r.cmd);
                    VkImageCopy c{};
                    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    c.extent = {sw, sh, 1};
                    vkCmdCopyImage(r.cmd, src, VK_IMAGE_LAYOUT_GENERAL, rt.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                    full_barrier(r.cmd);
                    src = rt.image;
                    retyped_view = rt.view;
                }
            }
        }
        // (the Insert menu's HDR set to Native: the game, told its display is HDR10, presents PQ codes in RGB10A2 (vk
        // A2B10G10R10); that picture goes to the HDR10 swapchain as it is, not through the SDR-to-HDR pass. Local fix: by the
        // format the game presents, not the cached image's, which stays RGBA8 when HDR is turned on in play)
        const bool native_pq = tex && dev.hdr && opts.hdr == 2 && want_fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        present_native_pq.store(held ? held_pic.valid && held_pic.pq : native_pq, std::memory_order_relaxed);
        upscale_lod_tick();   // ((Triforce, S8) the texture LOD bias follows DLSS's or FSR's render scale: vk_draw.h)
        if (held) {   // (Triforce: the held picture's own view, for the same passes)
            if (src && !dev.offscreen && (opts.upscale || opts.sharpness || dev.hdr || wide_sides_possible())) direct_view = held_pic.view;
        } else if (tex && !dev.offscreen && (opts.upscale || opts.sharpness || dev.hdr || wide_sides_possible())) {   // (and ultrawide's menu sides)
            // Keep the sampled view with the same cached image as the direct present, before advancing its age.
            direct_view = texture_view(tex, t, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample);
            tex->last_frame = texture_frame_no();
            tex->last_value = std::max(tex->last_value, value);
        }
        // (frame interpolation: made frames and this one shown by the display thread, vk_framegen.cpp. Recorded
        // after the submit that signals the present's value, in a submit of its own: the game waits for that value
        // before it submits its next frame, and the interpolation's GPU work (~1 ms) held it back. The display
        // thread's present follows it on the one queue. RX_VK_SHOT_MADE shots need it in this recording.)
        static const bool fg_inline = getenv("RX_VK_SHOT_MADE") != nullptr;
        // (AMD's frame generation, when its swapchain is the one in place: it presents the made frame itself, so the renderer makes
        // none and presents the game's frame alone, here, through that swapchain: present_image below)
        const bool ffx_swapchain = ffxfg::active() || ffxfg::target();   // (switching in or out: this frame goes straight to present_image too)
        const bool fg_after = framegen_on() && tex && !fg_inline && !held && !ffx_swapchain;
        interpolated = framegen_on() && tex && fg_inline && !held && !ffx_swapchain &&
                       framegen_record(r.cmd, retyped_view ? retyped_view : texture_view(tex, t, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample),
                                       sw, sh, crop);
        texture_frame(completed());
        last_frame_work.store(frame_work.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);   // ((Triforce) tex_warm.h)
        full_barrier(r.cmd);
        presents++;
        presents_now = presents;
        {   // (Triforce, 11-14) this frame's draws, for the predicted pipelines' builder: it rests while the game is in play
            static int64_t d_last;
            int64_t d_now = 0, x;
            draw_stats(&d_now, &x, &x, &x);
            frame_shown_note((int)std::clamp<int64_t>(d_now - d_last, 0, 1 << 30));
            d_last = d_now;
        }
        par_tune_present();   // (Triforce: RX_VK_PARALLEL_AUTO)
        wide_sides_tick();
        if (occ_live) occ_present_tick();   // (Triforce: RX_VK_OCC_QUERY's line every 600 presents)
        framegen_present_tick();   // (Triforce: AMD FSR 4's frame generation context, let go once it is not used: vk_fsr4.h)
        if (presents == 1) hang_watch_start();
        {   // (EvilBoyce, diagnostics) RX_VK_WATCH_CPU=<present>:<gpu address>[,...] (hex addresses): from that present on, the
            // CPU's writes to those 4 KB pages logged with the writer's stack (host page watch: 12 reports)
            static bool armed = false;
            static const char* e = getenv("RX_VK_WATCH_CPU");
            if (e && !armed && host->watch_guest_writes && presents >= atoll(e)) {
                armed = true;
                for (const char* q = strchr(e, ':'); q && *q;) {
                    char* end;
                    const uint64_t a = strtoull(q + 1, &end, 16);
                    if (end == q + 1) break;
                    if (const uint64_t g = host->gpu_guest(a)) host->watch_guest_writes(g & ~4095ull, 4096);
                    q = *end ? end : nullptr;
                }
            }
        }
        ts_dyn_present();
        ts_anchor_ns = host->ticks() * 32;
        dump_present(presents);
        if (src && (dev.offscreen || shots) && offscreen_init()) {
            if (dev.offscreen) {
                image_barrier(r.cmd, off_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                blit_crop(r.cmd, src, sw, sh, crop, off_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          dev.swap_extent.width, dev.swap_extent.height);
            }
            // (RX_VK_SHOT_FROM / _TO: every present in that range too)
            static const int64_t shot_from = getenv("RX_VK_SHOT_FROM") ? atoll(getenv("RX_VK_SHOT_FROM")) : -1;
            static const int64_t shot_to = getenv("RX_VK_SHOT_TO") ? atoll(getenv("RX_VK_SHOT_TO")) : -1;
            shot = shots && (presents % shot_every == 0 || (presents >= shot_from && presents <= shot_to));
            // (RX_VK_SHOT_GO=<file>: every present while that file is there too, a test's run of frames)
            static const char* shot_go = getenv("RX_VK_SHOT_GO");
            if (shots && shot_go && !shot) {
                struct stat sb;
                shot = stat(shot_go, &sb) == 0;
            }
            // (RX_VK_SHOT_MENU=1, a debug aid: without a window the Insert menu is neither ticked nor drawn; this ticks it
            // at each present and draws it into the shots, for pictures of it from a headless run)
            static const bool shot_menu = getenv("RX_VK_SHOT_MENU") != nullptr;
            if (shot_menu && dev.offscreen) menu_tick();
            // (RX_VK_SHOT_MADE=1: the frame interpolation made before it too, written beside it as vk_N_made.png)
            static const bool shot_made = getenv("RX_VK_SHOT_MADE") != nullptr;
            uint32_t mw = 0, mh = 0;
            made_src = shot && shot_made && interpolated ? framegen_made_image(&mw, &mh) : VK_NULL_HANDLE;
            if (made_src && !made_shot_init()) made_src = VK_NULL_HANDLE;
            if (made_src) {
                image_barrier(r.cmd, made_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                blit_crop(r.cmd, made_src, mw, mh, nullptr, made_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kShotW, kShotH);
                image_barrier(r.cmd, made_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {kShotW, kShotH, 1}};
                vkCmdCopyImageToBuffer(r.cmd, made_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, made_buf, 1, &c);
                capture_host_read_barrier(r.cmd, made_buf, (VkDeviceSize)kShotW * kShotH * 4);
            }
            if (shot) {
                // A pending shot may be two (or more) presents old. Own its pixels before this slot is written again.
                if (pending.on && (pending.presents & 1) == (presents & 1) && !pending.pixels)
                    pending.pixels = shot_pixels(pending.value, pending.presents);
                image_barrier(r.cmd, shot_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                // (RX_VK_SHOT_FX=1: through the window's scale pass too, ultrawide's menu sides as the window shows them. For headless
                // runs: in a window it resizes the pass's image to the shot's size and the present back again, two device waits a
                // shot, and it is not under present_image_mu, which frame generation's display thread takes for the same pass)
                static const bool shot_fx = getenv("RX_VK_SHOT_FX") != nullptr;
                const VkImage fx_img = shot_fx && tex ? framegen_scale(r.cmd, texture_view(tex, t, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample),
                                                                       src, sw, sh, crop, kShotW, kShotH) : VK_NULL_HANDLE;
                if (fx_img) full_barrier(r.cmd), blit_crop(r.cmd, fx_img, kShotW, kShotH, nullptr, shot_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kShotW, kShotH);
                else blit_crop(r.cmd, src, sw, sh, crop, shot_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kShotW, kShotH);
                if (shot_menu && dev.offscreen) menu_record_image(r.cmd, shot_image, shot_view, kShotW, kShotH);   // (headless: the menu over the shot)
                image_barrier(r.cmd, shot_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {kShotW, kShotH, 1}};
                vkCmdCopyImageToBuffer(r.cmd, shot_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot_bufs[presents & 1], 1, &c);
                capture_host_read_barrier(r.cmd, shot_bufs[presents & 1], (VkDeviceSize)kShotW * kShotH * 4);
            }
        }
        texture_end_recording(r.cmd), memory_end_submission(r.cmd);
        full_barrier(r.cmd);
        // (Triforce-dev) a present that leaves work stopped (drain_stale) signals the timeline only as far as no older
        // submission is unfinished, as finish_recording does: signalled up to its own value, the host freed the stopped
        // submissions' command copies (nvn_native.c free_dead_copies) and their replay went on in freed memory
        done_max = std::max(done_max, value);
        {
            const uint64_t oldest = oldest_unfinished();
            const uint64_t at = oldest == UINT64_MAX ? done_max : std::min(done_max, oldest - 1);
            const bool signal = at > signaled;
            if (signal) signaled = at;
            submit_recording(r, signal, at);
        }
        if (pushed.load() < value) pushed = value;
        if (fg_after) {
            // (its slot is free again once the next submit's value is reached: that one comes after it)
            Recording& g = begin_recording(value + 1);
            interpolated = framegen_record(g.cmd, retyped_view ? retyped_view : texture_view(tex, t, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample),
                                           sw, sh, crop);
            submit_recording(g, false);
        }
        if (interpolated) framegen_queue(value);
        // (the FPS counter's, as frame generation's thread counts what it shows. (Triforce, 11-16: the owner saw no frame
        // generation rate) DLSS frame generation's frames go out inside this present, through Streamline: the frames it shows
        // for it, its multiplier, are what the screen gets; 1 while it does not generate. (linux fork) AMD's swapchain shows a
        // made frame too)
        else if (!held) menu_note_frame(true, ffx_swapchain && ffxfg::generating() ? 2 : dlssg::multiplier());
        // (Triforce: a held frame is not counted, nothing new is shown; the window texture of a frame shown is noted for the
        // held picture of a hold that may follow)
        if (!held && tex && src && !dev.offscreen) {
            last_shown.on = true;
            last_shown.tex = tex, last_shown.id = tex->id, last_shown.gone = texture_destroyed_count();
            last_shown.copy = src != tex->image ? src : VK_NULL_HANDLE;
            last_shown.f = src != tex->image ? want_fmt : tex->format;   // (the copy's: of the same format as its source)
            last_shown.w = sw, last_shown.h = sh;
            last_shown.has_crop = crop != nullptr;
            if (crop) std::memcpy(last_shown.crop, crop, sizeof last_shown.crop);
            last_shown.pq = native_pq;
        }
        {   // (RX_VK_WAIT_GPU=<frames>: the replay no further ahead of the GPU than that)
            static uint64_t values[8];
            static const int ahead = [] {
                const char* e = getenv("RX_VK_WAIT_GPU");
                const long count = e ? std::strtol(e, nullptr, 10) : 0;
                constexpr long capacity = sizeof(values) / sizeof(values[0]);
                if (count > capacity) logf("vk: RX_VK_WAIT_GPU=%ld exceeds the ring capacity; using %ld", count, capacity);
                return (int)std::clamp<long>(count, 0, capacity);
            }();
            if (ahead > 0) {
                const uint64_t old = values[presents % ahead];
                values[presents % ahead] = value;
                if (old) wait_value(old);
            }
        }
        if (presents % 300 == 0) pipeline_cache_save(false);
        gpl_retire_run();   // ((Triforce) the fast links whole builds replaced, given back once old enough: vk_draw.cpp)
        {   // (RX_VK_DUMP_KEY: F9 dumps the next frame, 8 times at most)
            static const bool dump_key = getenv("RX_VK_DUMP_KEY") != nullptr;
            static bool was_down;
            static int key_left = 8;
            const bool down = dump_key && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            if (down && !was_down && key_left > 0) {
                key_left--;
                static const int n = getenv("RX_VK_DUMP_KEY_N") ? std::max(1, atoi(getenv("RX_VK_DUMP_KEY_N"))) : 1;
                logf("vk: F9 at present %lld: the next %d frame(s) are dumped", (long long)presents, n);
                dump_request(presents + 1, n);
            }
            was_down = down;
        }
        {   // (RX_VK_DUMP_FILE=<path>: that file appearing dumps the next frame, and is deleted: a test that steers the
            // game live (HOS_INPUT_LIVE) dumps where it stands)
            // (RX_VK_DUMP_MOD=<m>,<r>[,<n>]: the file arms the next n frames (default 2) whose present is r mod m
            // instead: a glitch that comes back every m frames, dumped in its own frame (the flash detector names it))
            static const char* dump_file = getenv("RX_VK_DUMP_FILE");
            static int mod_m = -1, mod_r = 0, mod_n = 2, mod_left = 0;
            if (mod_m < 0) {
                mod_m = 0;
                if (const char* e = getenv("RX_VK_DUMP_MOD")) sscanf(e, "%d,%d,%d", &mod_m, &mod_r, &mod_n);
            }
            if (dump_file && presents % 10 == 0 && GetFileAttributesA(dump_file) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileA(dump_file);
                if (mod_m > 0) {
                    mod_left = std::max(1, mod_n);
                    logf("vk: dump file at present %lld: the next %d frames at %d mod %d are dumped", (long long)presents,
                         mod_left, mod_r, mod_m);
                } else {
                    logf("vk: dump file at present %lld: the next frame is dumped", (long long)presents);
                    dump_request(presents + 1, 1);
                }
            }
            if (mod_left > 0 && (presents + 2) % mod_m == mod_r) {   // (armed at the present before the frame's own)
                mod_left--;
                dump_request(presents + 1, 1);
            }
        }
        // (a shot is read once the GPU reached its frame, at a later present, and written on a thread of its own:
        // waiting for the GPU and encoding the PNG there took the replay ~570 ms a shot, and frequent shots
        // halved the frame rate test runs measured. RX_VK_SHOT_MADE shots are taken at once, as before.)
        auto take = [&](uint64_t at_value, int64_t at_present) {
            auto px = pending.on && pending.presents == at_present && pending.pixels
                          ? std::move(pending.pixels) : shot_pixels(at_value, at_present);
            // (RX_VK_DUMP_BLACK=<n>: a shot with 12-90% pure black pixels in a lit picture, or a green cast over it,
            // dumps the next frame, n times at most: the glitches some scenes show now and then)
            static int black_left = getenv("RX_VK_DUMP_BLACK") ? atoi(getenv("RX_VK_DUMP_BLACK")) : 0;
            if (black_left > 0) {
                const uint8_t* q0 = px->data();
                uint64_t black = 0, white = 0, sum = 0, ch[3] = {};
                const uint64_t n = (uint64_t)kShotW * kShotH;
                for (uint64_t i = 0; i < n; i++) {
                    const uint8_t* q = q0 + i * 4;
                    black += q[0] < 3 && q[1] < 3 && q[2] < 3;
                    white += q[0] > 235 && q[1] > 235 && q[2] > 235;
                    sum += q[0] + q[1] + q[2];
                    ch[0] += q[0], ch[1] += q[1], ch[2] += q[2];
                }
                // (hookshot: a picture blown out to white, the HUD on it: written as white_<present>.png, 40 of them)
                static int white_left = 40;
                if (at_present > 600 && white * 100 > n * 40 && white_left > 0) {
                    white_left--;
                    logf("vk: present %lld: %.1f%% white pixels, mean %llu %llu %llu", (long long)at_present, 100.0 * white / n,
                         (unsigned long long)(ch[0] / n), (unsigned long long)(ch[1] / n), (unsigned long long)(ch[2] / n));
                    auto w0 = px;
                    const std::string name = std::string(shots) + "/white_" + std::to_string(at_present) + ".png";
                    std::thread([w0, name] { write_png(name.c_str(), w0->data(), kShotW, kShotH); }).detach();
                }
                const bool green = ch[1] * 10 > ch[0] * 12 && ch[1] * 10 > ch[2] * 14 && ch[1] / n > 120;
                if (at_present > 600 && ((black * 8 > n && black * 100 < n * 97 && sum / (n * 3) > 4) || green)) {
                    black_left--;
                    logf("vk: present %lld: %.1f%% black pixels, mean %llu %llu %llu: the next frame is dumped",
                         (long long)at_present, 100.0 * black / n, (unsigned long long)(ch[0] / n),
                         (unsigned long long)(ch[1] / n), (unsigned long long)(ch[2] / n));
                    dump_request(presents + 1);
                }
            }
            // (RX_VK_FLASH=1, with RX_VK_SHOT_EVERY=1: a frame that differs from both its neighbours in a part of the
            // picture where they agree is a one-frame flash: the three are written as flash_<present>_{a,b,c}.png and
            // the next frame is dumped (a few times). The other shots are written every 300th present only.)
            static const bool flash = getenv("RX_VK_FLASH") && atoi(getenv("RX_VK_FLASH")) != 0;
            if (flash) {
                constexpr uint32_t W = 64, H = 36;
                auto small = std::make_shared<std::vector<uint8_t>>(W * H * 3);
                for (uint32_t y = 0; y < H; y++) {
                    const uint32_t y0 = (uint64_t)y * kShotH / H;
                    const uint32_t y1 = std::max(y0 + 1, (uint32_t)((uint64_t)(y + 1) * kShotH / H));
                    for (uint32_t x = 0; x < W; x++) {
                        const uint32_t x0 = (uint64_t)x * kShotW / W;
                        const uint32_t x1 = std::max(x0 + 1, (uint32_t)((uint64_t)(x + 1) * kShotW / W));
                        for (int ch = 0; ch < 3; ch++) {
                            uint64_t sum = 0, samples = 0;
                            // Partition the full image and count the samples actually visited. A partial
                            // stride must not brighten the cell or wrap its mean back through uint8.
                            for (uint32_t yy = y0; yy < y1; yy += 4)
                                for (uint32_t xx = x0; xx < x1; xx += 4) {
                                    sum += (*px)[((size_t)yy * kShotW + xx) * 4 + ch];
                                    samples++;
                                }
                            (*small)[(y * W + x) * 3 + ch] = (uint8_t)(sum / samples);
                        }
                    }
                }
                static std::shared_ptr<std::vector<uint8_t>> sa, sb, pa, pb;
                static int64_t present_b = -1;
                static int flashes_left = 400, dumps_left = getenv("RX_VK_FLASH_DUMPS") ? atoi(getenv("RX_VK_FLASH_DUMPS")) : 6;
                if (sa && sb && present_b == at_present - 1) {
                    // (RX_VK_FLASH_DIFF, RX_VK_FLASH_CELLS: a subtler flash; logged with how many cells went up, and where)
                    static const int fl_diff = getenv("RX_VK_FLASH_DIFF") ? atoi(getenv("RX_VK_FLASH_DIFF")) : 48;
                    static const int fl_cells = getenv("RX_VK_FLASH_CELLS") ? atoi(getenv("RX_VK_FLASH_CELLS")) : 40;
                    int hits = 0, up = 0, minx = W, maxx = -1, miny = H, maxy = -1, peak = 0;
                    uint64_t tab = 0, tac = 0;   // (camera motion: a and c differ as much as either from b; a flash: a ~ c)
                    for (uint32_t i = 0; i < W * H * 3; i++)
                        tab += std::abs((*sa)[i] - (*sb)[i]), tac += std::abs((*sa)[i] - (*small)[i]);
                    for (uint32_t i = 0; i < W * H; i++) {
                        int dab = 0, dbc = 0, dac = 0;
                        for (int ch = 0; ch < 3; ch++) {
                            dab = std::max(dab, std::abs((*sa)[i * 3 + ch] - (*sb)[i * 3 + ch]));
                            dbc = std::max(dbc, std::abs((*sb)[i * 3 + ch] - (*small)[i * 3 + ch]));
                            dac = std::max(dac, std::abs((*sa)[i * 3 + ch] - (*small)[i * 3 + ch]));
                        }
                        if (dab > fl_diff && dbc > fl_diff && dac < fl_diff / 3) {
                            hits++;
                            up += (*sb)[i * 3 + 1] > (*sa)[i * 3 + 1];
                            peak = std::max(peak, dab);
                            const int cx = (int)(i % W), cy = (int)(i / W);
                            minx = std::min(minx, cx), maxx = std::max(maxx, cx);
                            miny = std::min(miny, cy), maxy = std::max(maxy, cy);
                        }
                    }
                    // (RX_VK_FLASH_REGION=<level>: also while the picture moves, when the cells' test cannot hold: a 16x9 block of
                    // the picture whose mean brightness jumps by the level and comes back the next frame)
                    static const int fl_region = getenv("RX_VK_FLASH_REGION") ? atoi(getenv("RX_VK_FLASH_REGION")) : 0;
                    int region_hits = 0, region_peak = 0, region_at = -1;
                    if (fl_region)
                        for (uint32_t ry = 0; ry < 9; ry++)
                            for (uint32_t rx = 0; rx < 16; rx++) {
                                int m[3] = {0, 0, 0};
                                for (uint32_t y = ry * 4; y < ry * 4 + 4; y++)
                                    for (uint32_t x = rx * 4; x < rx * 4 + 4; x++) {
                                        const size_t o = (y * W + x) * 3 + 1;
                                        m[0] += (*sa)[o], m[1] += (*sb)[o], m[2] += (*small)[o];
                                    }
                                const int ab = (m[1] - m[0]) / 16, cb = (m[1] - m[2]) / 16, ac = std::abs(m[0] - m[2]) / 16;
                                if (((ab > fl_region && cb > fl_region) || (ab < -fl_region && cb < -fl_region)) &&
                                    ac * 2 < std::min(std::abs(ab), std::abs(cb))) {
                                    region_hits++;
                                    if (std::abs(ab) > std::abs(region_peak)) region_peak = ab, region_at = (int)(ry * 16 + rx);
                                }
                            }
                    if (region_hits && flashes_left > 0)
                        logf("vk: region flash at present %lld: %d regions, peak %+d at region %d,%d", (long long)present_b, region_hits,
                             region_peak, region_at % 16, region_at / 16);
                    if (((hits >= fl_cells && tac * 2 < tab) || region_hits) && flashes_left > 0) {
                        flashes_left--;
                        logf("vk: flash at present %lld: %d of %u cells differ from both neighbours, %d brighter, peak %d, "
                             "cells %d,%d-%d,%d (now present %lld, texture frame %llu)", (long long)present_b, hits, W * H, up,
                             peak, minx, miny, maxx, maxy, (long long)presents, (unsigned long long)texture_frame_no());
                        const std::string base = std::string(shots) + "/flash_" + std::to_string(present_b);
                        auto a0 = pa, b0 = pb, c0 = px;
                        std::thread([a0, b0, c0, base] {
                            write_png((base + "_a.png").c_str(), a0->data(), kShotW, kShotH);
                            write_png((base + "_b.png").c_str(), b0->data(), kShotW, kShotH);
                            write_png((base + "_c.png").c_str(), c0->data(), kShotW, kShotH);
                        }).detach();
                        if (dumps_left > 0) dumps_left--, dump_request(presents + 1);
                        static int records_left = 12;   // (RX_VK_FLASH_RECORD: the draws around it)
                        const uint64_t tf = (uint64_t)(present_b + (int64_t)texture_frame_no() - presents);
                        probe_log(tf);
                        if (records_left > 0) records_left--, draw_records_log(tf - 1, tf + 1), pass_traces_log(tf - 1, tf + 1);
                    }
                }
                sa = sb, pa = pb, sb = small, pb = px, present_b = at_present;
                if (at_present % 300) return;
            }
            std::string path = std::string(shots) + "/vk_" + std::to_string(1000000 + at_present).substr(1) + ".png";
            std::thread([px, path, at_present] {
                if (write_png(path.c_str(), px->data(), kShotW, kShotH))
                    logf("vk: present %lld written to %s", (long long)at_present, path.c_str());
            }).detach();
        };
        if (pending.on && (shot || completed() >= pending.value)) take(pending.value, pending.presents), pending.on = false;
        if (shot && made_src && made_map) {
            take(value, presents);
            invalidate_mapped(made_buf_alloc, (uint64_t)kShotW * kShotH * 4);
            char path[1024];
            snprintf(path, sizeof path, "%s/vk_%06lld_made.png", shots, (long long)presents);
            write_png(path, (const uint8_t*)made_map, kShotW, kShotH);
        } else if (shot) {
            pending = {true, value, presents, {}};
        }
        log_frame_stats();
    }
    if (held) {
        // (Triforce) the held picture again, with the overlays over it: through frame generation's display thread when it runs,
        // after the frames it has still to show; none to show, no present (the window keeps the picture it was given last).
        // Never without a window (no hold there: hold_step) and never a frame interpolated (held frames are not given to it)
        if (!src) return;
        if (framegen_on()) return framegen_queue_held(src, direct_view, sw, sh, show_crop);
        held_now.store(true, std::memory_order_relaxed);
        smooth_mine = true;   // ((Triforce) the game's own picture, for Frame pacing on the plain present: smooth_present)
        present_image(src, sw, sh, show_crop, direct_view);
        smooth_mine = false;
        held_now.store(false, std::memory_order_relaxed);
        return;
    }
    if (dev.offscreen || interpolated) return;
    smooth_mine = true;
    present_image(src, sw, sh, crop, direct_view);
    smooth_mine = false;
}

} // namespace

// (Triforce) The hold of the last picture, for the passes that keep a history (vk_temporal.cpp, vk_dlssg.cpp): the last present
// held its frame (the next frame drawn starts its history anew), the present under way shows the held picture, and it is the
// first frame shown after a hold
bool present_held_last() { return held_last.load(std::memory_order_relaxed); }
bool present_hold_now() { return held_now.load(std::memory_order_relaxed); }
bool present_after_hold() { return after_hold_now.load(std::memory_order_relaxed); }

// (hookshot) RX_VK_FRAMELOG=<file>: every frame shown, the game's and the made ones, as they go to the screen, 320x180, while
// <file>.go exists (scripts/framelog.py finds the flashes). Taken at the last step and written on a thread of its own, so
// that the game keeps its speed: a shot of every frame at 1280x720 held it at 30 fps, where the flashes may not show.
// A record: u64 time (ns), u32 sequence, u32 kind (0 the game's, 1 made), 320x180 RGBA.
void exposure_log_dump(const char* why);   // (vk_draw.cpp)
void probe_dump_all(const char* why);
bool probe_serial();
namespace {
constexpr uint32_t kLogW = 320, kLogH = 180;
const char* framelog_path() {
    static const char* p = getenv("RX_VK_FRAMELOG");
    return p;
}
struct LogSlot {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    bool used = false;
    int64_t at = 0;
    uint32_t seq = 0, kind = 0;
};
LogSlot log_slots[3];
VkImage log_image = VK_NULL_HANDLE;
VmaAllocation log_image_alloc = nullptr;
std::mutex log_mu;
std::condition_variable log_cv;
std::deque<std::vector<uint8_t>> log_queue;
std::atomic<int> log_state{0};   // 0: not tried; 1: complete sink; -1: disabled after a failure
void framelog_resources_drop() {
    for (LogSlot& l : log_slots) {
        if (l.buf) destroy_buffer(l.buf, l.alloc);
        l = LogSlot{};
    }
    if (log_image) destroy_image(log_image, log_image_alloc);
    log_image = VK_NULL_HANDLE;
    log_image_alloc = nullptr;
}
void framelog_write(FILE* f) {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk frame log");
    for (;;) {
        std::vector<uint8_t> rec;
        {
            std::unique_lock lk(log_mu);
            log_cv.wait(lk, [] { return !log_queue.empty(); });
            rec = std::move(log_queue.front());
            log_queue.pop_front();
        }
        // ((Triforce, handoff §8) each record whole: the exit waits for one under way, and the log ends once it has begun)
        const file_writers::Writing writing;
        if (!writing) {
            fclose(f);
            return;
        }
        const size_t written = fwrite(rec.data(), 1, rec.size(), f);
        if (written != rec.size() || fflush(f) != 0) {
            {
                std::lock_guard lk(log_mu);
                log_state.store(-1, std::memory_order_release);
                log_queue.clear();   // (no worker remains to consume these; don't retain the queued CPU pictures)
            }
            logf("vk: frame log stopped: %s failed (wrote %zu of %zu bytes)", written == rec.size() ? "flush" : "write",
                 written, rec.size());
            fclose(f);
            return;
        }
    }
}
void framelog_enqueue(std::vector<uint8_t> rec) {
    std::lock_guard lk(log_mu);
    if (log_state.load(std::memory_order_acquire) > 0 && log_queue.size() < 512) {
        log_queue.push_back(std::move(rec));
        log_cv.notify_one();
    }
}
bool framelog_ready() {
    const int state = log_state.load(std::memory_order_acquire);
    if (state) return state > 0;
    log_state.store(-1, std::memory_order_release);
    if (!framelog_path()) return false;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {kLogW, kLogH, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!create_image(ci, log_image, log_image_alloc)) {
        framelog_resources_drop();
        logf("vk: frame log disabled: the readback image could not be made");
        return false;
    }
    for (LogSlot& l : log_slots) {
        if (!create_buffer_cached((uint64_t)kLogW * kLogH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, l.buf, l.alloc, &l.map) || !l.map) {
            framelog_resources_drop();
            logf("vk: frame log disabled: the complete mapped readback set could not be made");
            return false;
        }
    }
    FILE* f = fopen(framelog_path(), "wb");
    if (!f) {
        framelog_resources_drop();
        logf("vk: frame log disabled: cannot open %s", framelog_path());
        return false;
    }
    // Other presenters are serialized by present_image_mu. Publish before the worker can report a failure.
    log_state.store(1, std::memory_order_release);
    try {
        std::thread([f] { framelog_write(f); }).detach();
    } catch (const std::exception& e) {
        log_state.store(-1, std::memory_order_release);
        fclose(f);
        framelog_resources_drop();
        logf("vk: frame log disabled: writer thread could not start (%s)", e.what());
        return false;
    }
    logf("vk: the frame log writes every frame shown to %s", framelog_path());
    return true;
}
bool framelog_on() {
    static bool on = false;
    static int64_t next = 0;
    if (!framelog_path()) return false;
    const int64_t now = prof_now();
    if (now >= next) {
        next = now + 250000000;
        const std::string go = std::string(framelog_path()) + ".go";
        on = GetFileAttributesA(go.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    return on && framelog_ready();
}
void framelog_collect(int slot) {
    LogSlot& l = log_slots[slot];
    if (!l.used) return;
    l.used = false;
    // present_image waited for this slot's fence; expose its GPU writes before the CPU copy.
    invalidate_mapped(l.alloc, (uint64_t)kLogW * kLogH * 4);
    std::vector<uint8_t> rec(16 + (size_t)kLogW * kLogH * 4);
    std::memcpy(rec.data(), &l.at, 8);
    std::memcpy(rec.data() + 8, &l.seq, 4);
    std::memcpy(rec.data() + 12, &l.kind, 4);
    std::memcpy(rec.data() + 16, l.map, (size_t)kLogW * kLogH * 4);
    {   // (a picture blown out to white: what the exposure was)
        const uint8_t* q = rec.data() + 16;
        uint64_t sum = 0;
        const size_t n = (size_t)kLogW * kLogH;
        for (size_t i = 0; i < n; i += 7) sum += q[i * 4 + 1];
        const uint64_t mean = sum / (n / 7 + 1);
        static uint64_t last_mean;
        if (mean > 225 && last_mean < 215 && l.kind == 0) {
            logf("vk: white frame: frame log sequence %u, present %lld, brightness %llu after %llu", l.seq, (long long)presents,
                 (unsigned long long)mean, (unsigned long long)last_mean);
            char line[700];
            int n2 = snprintf(line, sizeof line, "vk: white detail: textures made by present (now %lld):", (long long)made_ring_present);
            for (int k = 30; k >= 0 && n2 < 660; k--) {
                const int64_t at = made_ring_present - k;
                const uint64_t a = made_ring[(at - 1) & 31], b = made_ring[at & 31];
                if (b > a) n2 += snprintf(line + n2, sizeof line - n2, " -%d:%llu", k, (unsigned long long)(b - a));
            }
            logf("%s", line);
            exposure_log_dump("white frame");
            probe_dump_all("white frame");
        }
        if (l.kind == 0 && mean * 2 < last_mean && last_mean > 40) {   // (a picture gone dark for a frame, the HUD on it)
            static int dark_left = 6;
            if (dark_left > 0) {
                dark_left--;
                logf("vk: dark frame: frame log sequence %u, present %lld, brightness %llu after %llu", l.seq, (long long)presents,
                     (unsigned long long)mean, (unsigned long long)last_mean);
                char name[32];
                snprintf(name, sizeof name, "cond_%d.txt", dark_left);
                if (FILE* o = fopen(name, "w")) {
                    for (uint32_t i = 0; i < (1 << 15); i++) {
                        const CondNote& c = cond_ring[(cond_ring_at + i) % (1 << 15)];
                        if (c.addr)
                            fprintf(o, "%lld %#llx %d %llu %llu %d%c", (long long)c.present, (unsigned long long)c.addr, c.mode,
                                    (unsigned long long)c.x, (unsigned long long)c.y, c.on ? 1 : 0, 10);
                    }
                    fclose(o);
                }
                exposure_log_dump("dark frame");
                probe_dump_all("dark frame");
                pass_traces_log(texture_frame_no() - 7, texture_frame_no());
            }
        }
        {   // ((Triforce, a diagnostic) RX_VK_REGION_TRAP=1, with RX_VK_PASS_TRACE=1: the picture's bottom middle (where the
            // player's character stands in the lab's Market view) gone dark from one frame to the next, the rest as it was:
            // the last frames' passes and render conditions, to see what the dark frame's replay did not do. 8 a run
            static const bool trap = getenv("RX_VK_REGION_TRAP") != nullptr;
            if (trap && l.kind == 0) {
                uint64_t rs = 0, rn = 0;
                for (int y = kLogH * 5 / 6; y < kLogH; y += 2)
                    for (int x = kLogW * 3 / 8; x < kLogW * 6 / 8; x += 2) rs += q[((size_t)y * kLogW + (size_t)x) * 4 + 1], rn++;
                const uint64_t rm = rs / std::max<uint64_t>(rn, 1);
                static uint64_t last_rm;
                static int left = 8;
                if (left > 0 && last_rm > 30 && rm + 12 < last_rm && mean * 10 > last_mean * 8) {
                    left--;
                    logf("vk: region dark: frame log sequence %u, present %lld, texture frame %llu: its bottom middle %llu after %llu "
                         "(the whole picture %llu after %llu)", l.seq, (long long)presents, (unsigned long long)texture_frame_no(),
                         (unsigned long long)rm, (unsigned long long)last_rm, (unsigned long long)mean, (unsigned long long)last_mean);
                    char name[40];
                    snprintf(name, sizeof name, "region_cond_%d.txt", left);
                    if (FILE* o = fopen(name, "w")) {
                        for (uint32_t i = 0; i < (1 << 15); i++) {
                            const CondNote& c = cond_ring[(cond_ring_at + i) % (1 << 15)];
                            if (c.addr)
                                fprintf(o, "%lld %#llx %d %llu %llu %d%c", (long long)c.present, (unsigned long long)c.addr, c.mode,
                                        (unsigned long long)c.x, (unsigned long long)c.y, c.on ? 1 : 0, 10);
                        }
                        fclose(o);
                    }
                    pass_traces_log(texture_frame_no() - 7, texture_frame_no());
                }
                last_rm = rm;
            }
        }
        if (l.kind == 0) last_mean = mean;
    }
    static const bool drop = getenv("RX_VK_FRAMELOG_DROP") != nullptr;
    if (drop) return;
    framelog_enqueue(std::move(rec));
}
} // namespace
std::atomic<uint32_t> present_kind{0};   // (vk_framegen.cpp: 1 while a made frame goes out)

namespace {
// (Triforce, after NX2navi's pace_smooth: present_smooth.h) The Frame pacing row on the plain present, with
// RX_VK_PACE_SMOOTH (on unless =0): the game's own pictures (do_present marks its calls, smooth_mine), not frame generation's display
// thread's (its own pacing, RX_VK_PACE) nor those DLSS frame generation's Streamline shows on a pace of its own
PresentSmooth smooth{};                  // (present_image_mu; (Triforce) or the present thread's, while it has a present in hand)
struct { int64_t shown, waited, wait_ns, longest_ns; } smooth_ct{};   // (as smooth: since the last line)
bool smooth_wanted() {
    static const bool on = [] {
        // ((Triforce, 11-15) On by default, as NX2navi's pace_smooth: the lab's A/B at the Market (cap 240, Frame pacing on,
        // 2 runs each) gave the lowest frame-to-frame jitter (1.13 and 1.10 ms against 1.18 to 1.26), no frame rate cost and
        // better worst frames. RX_VK_PACE_SMOOTH=0: off)
        const char* e = getenv("RX_VK_PACE_SMOOTH");
        return !(e && *e && atoi(e) == 0);
    }();
    // ((Triforce) the Steady frame rate row holds the game's pictures to its period whatever the Frame pacing row and the
    // switch say: pace::steady_floor_ns, 0 without it; present_smooth.h present_smooth_paced_at)
    return smooth_mine && !framegen_on() && !dlssg::active() && ((on && opts.pace != 0) || pace::steady_floor_ns() > 0);
}
/// Until host->now_ns() reads at: the high-resolution timer to 0.6 ms before it (for a wait of 1.6 ms or more; the timer
/// wakes up to about 0.5 ms late: host/pace_wait.h), the rest yielded away
void smooth_wait_until(int64_t at) {
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const int64_t left = at - host->now_ns();
    if (timer && left >= 1600000) {
        LARGE_INTEGER d;
        d.QuadPart = -((left - 600000) / 100);   // (relative, 100 ns units)
        if (SetWaitableTimerEx(timer, &d, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, INFINITE);
    }
    while (host->now_ns() < at)
        if (!SwitchToThread()) YieldProcessor();
}
/// Where the picture under way goes out, in present_image after its blit's submit and before its present: an early one
/// waits for the floor with the queue's lock let go (qk), and every one's time is noted for the recent interval. A line
/// every 1200 pictures says how many waited and how long
void smooth_present(std::unique_lock<std::mutex>& qk) {
    if (lab_ab::on("nopace")) return;   // (lab: no wait in these phases)
    if (!smooth_wanted()) {
        if (smooth.last_ns) present_smooth_reset(&smooth);   // (the next time it is on starts afresh)
        return;
    }
    const int64_t paced = pace::steady_floor_ns();   // ((Triforce) the Steady frame rate row's period, 0 without it)
    const int64_t now = host->now_ns(), at = present_smooth_paced_at(&smooth, now, paced);
    if (at > now) {
        if (qk.owns_lock()) qk.unlock();   // ((Triforce) on the present thread it was never taken)
        smooth_wait_until(at);
        if (paced) pace::steady_held(host->now_ns() - now);   // ((Triforce) the host measures what frames need without it)
        smooth_ct.waited++, smooth_ct.wait_ns += at - now, smooth_ct.longest_ns = std::max(smooth_ct.longest_ns, at - now);
    }
    present_smooth_shown(&smooth, host->now_ns());
    if (++smooth_ct.shown >= 1200) {
        logf("vk: frame pacing: %lld of %lld pictures waited for %s (%.2f ms), %.2f ms on average, %.2f ms the longest "
             "(RX_VK_PACE_SMOOTH)", (long long)smooth_ct.waited, (long long)smooth_ct.shown,
             paced ? "the steady frame rate's period" : "seven eighths of the recent interval", paced ? paced / 1e6 : smooth.avg_ns / 1e6,
             smooth_ct.waited ? smooth_ct.wait_ns / 1e6 / smooth_ct.waited : 0.0, smooth_ct.longest_ns / 1e6);
        smooth_ct = {};
    }
}

// ---- (Triforce) The present thread (present_thread.h), RX_VK_PRESENT_THREAD (on unless =0): a present of the game's records
// and submits its blit on the presenting thread as before, then hands its last step (present_out: the pacing's wait and the
// present call) to a thread of its own and returns; the next present_image waits for it first
/// What present_out needs of a present: the swapchain's image, and whether the picture is the game's (smooth_mine)
struct PresentOut {
    uint32_t index = 0;
    bool smooth = false;
    bool ffx = false;   // (linux fork: the swapchain is AMD's frame generation one, vk_ffx_fg.h: its present is AMD's)
};
present_thread::Handoff<PresentOut> present_hand;
/// The presents' figures for the 120-present line (present_thread_line) and the stall line, counted from the start
struct PresentFigures {
    std::atomic<int64_t> games{0}, games_ns{0};   // (the game's pictures and present_image's time for them on the presenting thread)
    std::atomic<int64_t> handed{0};               // (of them, those handed to the present thread)
    std::atomic<int64_t> waits{0}, wait_ns{0}, wait_max_ns{0};   // (present_image's waits for the present before, every caller's)
    std::atomic<int64_t> outs{0}, paced_ns{0}, call_ns{0}, call_max_ns{0};   // (present_out: the pacing's wait, the present call)
} present_fig;
void fig_max(std::atomic<int64_t>& m, int64_t v) {
    int64_t was = m.load(std::memory_order_relaxed);
    while (v > was && !m.compare_exchange_weak(was, v, std::memory_order_relaxed)) {}
}
bool present_thread_on() {
    static const bool on = [] {
        // ((Triforce, 11-16) Off by default: the lab's Market runs showed no gain (on 91.9 / 92.1 fps, off 89.4 / 93.6), and
        // the frame trace left only ~0.09 ms of present work on the game's thread. RX_VK_PRESENT_THREAD=1 on)
        const char* e = getenv("RX_VK_PRESENT_THREAD");
        const bool v = e && *e && atoi(e) == 1;
        logf("vk: the game's presents go out %s (RX_VK_PRESENT_THREAD%s)", v ? "from a present thread of their own: the presenting "
             "thread records and submits the blit and goes on" : "from the presenting thread", v ? "=1 asked" : "=1: a present thread");
        return v;
    }();
    return on;
}
/// This present goes out from the present thread: one of the game's pictures, with none of the presents of their own
bool present_thread_wanted() {
    // (linux fork: never with AMD's frame generation swapchain, whose presents go to AMD's own queue and thread)
    return !ffxfg::active() &&
           present_thread_use(present_thread_on(), smooth_mine, framegen_on(), dlssg::active(), antilag_running()) != 0;
}
/// present_image's first step (present_image_mu held): the present handed over before this one out
void present_thread_settle() {
    const int64_t ns = present_hand.settle();
    if (ns < 0) return;
    present_fig.waits.fetch_add(1, std::memory_order_relaxed), present_fig.wait_ns.fetch_add(ns, std::memory_order_relaxed);
    fig_max(present_fig.wait_max_ns, ns);
}
/// present_image's time on the presenting thread, for the game's pictures
void present_note_game(int64_t ns) {
    if (!smooth_mine) return;
    present_fig.games.fetch_add(1, std::memory_order_relaxed), present_fig.games_ns.fetch_add(ns, std::memory_order_relaxed);
}
int64_t present_waited_ns() { return present_fig.wait_ns.load(std::memory_order_relaxed); }
} // namespace

static void present_out(const PresentOut& o, std::unique_lock<std::mutex>& qk, bool on_thread);

namespace {
void present_thread_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk present");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   // (as frame generation's display thread)
    for (;;) {
        const PresentOut o = present_hand.take();
        smooth_mine = o.smooth;   // (smooth_wanted: this thread's own mark)
        {
            std::unique_lock<std::mutex> qk(dev.present_mu, std::defer_lock);
            present_out(o, qk, true);
        }
        smooth_mine = false;
        present_hand.done();
    }
}
/// The rest of this present to the present thread (started at the first)
void present_thread_hand(const PresentOut& o) {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(present_thread_main).detach(); });
    present_fig.handed.fetch_add(1, std::memory_order_relaxed);
    present_hand.hand(o);
}
/// The 120-present line: where the game's presents went out, what the presenting thread waited for, and the present's own
/// last step (on whichever thread it ran)
void present_thread_line() {
    static int64_t l_games, l_games_ns, l_handed, l_waits, l_wait_ns, l_outs, l_paced, l_call;
    const int64_t games = present_fig.games.load(), games_ns = present_fig.games_ns.load(), handed = present_fig.handed.load();
    const int64_t waits = present_fig.waits.load(), wait_ns = present_fig.wait_ns.load(), outs = present_fig.outs.load();
    const int64_t paced = present_fig.paced_ns.load(), call = present_fig.call_ns.load();
    const int64_t wait_max = present_fig.wait_max_ns.exchange(0), call_max = present_fig.call_max_ns.exchange(0);
    if (outs == l_outs && games == l_games) return;
    logf("vk: a frame's present: %lld of %lld of the game's pictures went out from the present thread (RX_VK_PRESENT_THREAD); the "
         "presenting thread %.2f ms in it, %.2f ms of that waiting for the present before (%lld times, the longest %.2f ms); "
         "%lld presents: the pacing's wait %.2f ms, the present call %.2f ms (the longest %.2f ms)",
         (long long)(handed - l_handed), (long long)(games - l_games), (games_ns - l_games_ns) / 120.0 / 1e6,
         (wait_ns - l_wait_ns) / 120.0 / 1e6, (long long)(waits - l_waits), wait_max / 1e6, (long long)(outs - l_outs),
         (paced - l_paced) / 120.0 / 1e6, (call - l_call) / 120.0 / 1e6, call_max / 1e6);
    l_games = games, l_games_ns = games_ns, l_handed = handed, l_waits = waits, l_wait_ns = wait_ns, l_outs = outs;
    l_paced = paced, l_call = call;
}
} // namespace

// (hookshot) One thread in here at a time; the queue's own lock (dev.present_mu, the replay's submits take it too) is held only
// for the submit and the present. Held over vkAcquireNextImageKHR and the fence, it stopped the replay's submits for as long as
// the swapchain had no image free, twice a frame with frame interpolation.
std::mutex present_image_mu;
void present_image(VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImageView view) {
    struct Timed {   // (Triforce: the present's own time, the lock's wait with it, for the stall line)
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() {
            const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            stall_ct.present_ns.fetch_add(ns, std::memory_order_relaxed);
            present_note_game(ns);   // ((Triforce) and the game's pictures' apart, for the present's 120-present line)
        }
    } timed;
    std::lock_guard lk(present_image_mu);
    present_thread_settle();   // ((Triforce) RX_VK_PRESENT_THREAD: the present handed to the present thread before this one out first)
    static int hdr_was = -1;   // (the Insert menu's HDR as the swapchain was made for it; -1: not yet looked at)
    const bool hdr_changed = hdr_was >= 0 && hdr_was != opts.hdr;   // (turned on or off: the swapchain's format)
    if (hdr_was < 0) hdr_was = opts.hdr;
    // (Triforce) and the Insert menu's VSync: the swapchain's present mode (vk_main.cpp create_swapchain, present_mode.h)
    static int vsync_was = -1;
    const bool vsync_changed = vsync_was >= 0 && vsync_was != opts.vsync;
    if (vsync_was < 0) vsync_was = opts.vsync;
    const uint64_t want = dev.swap_resize.exchange(0, std::memory_order_acquire);
    // (resized, gone, Reflex, HDR or VSync changed, or the engine choice changed the kind of swapchain: AMD's frame generation has one of its own)
    if (want || !dev.swapchain || reflex_swapchain_stale() || hdr_changed || vsync_changed || ffxfg::swapchain_stale()) {
        ffxfg::quiesce();   // (AMD's threads done with what they were handed, before the lock they submit under is taken)
        std::lock_guard qk(dev.present_mu);
        const uint32_t width = want ? (uint32_t)(want >> 32) : dev.swap_extent.width;
        const uint32_t height = want ? (uint32_t)want : dev.swap_extent.height;
        if (!create_swapchain(width, height, true)) {
            // Retry without replacing a newer resize published while recreation was in progress.
            uint64_t empty = 0;
            dev.swap_resize.compare_exchange_strong(empty, ((uint64_t)width << 32) | height,
                                                   std::memory_order_release, std::memory_order_relaxed);
            return;   // (the old swapchain may already be retired; do not acquire or reset a frame fence)
        }
        hdr_was = opts.hdr, vsync_was = opts.vsync;
    }
    if (src) {   // (the Window scaling filter's row: whether the window is the picture's size. Only the game's frames and the
                 // made ones: the start-up screen (no src) is drawn at the window's own size, which made the row read "Not
                 // needed" while the game's output was another size; until the first frame the size is not known: needed)
        const bool cropped = crop && crop[2] > 0 && crop[3] > 0;
        menu_note_present_sizes(cropped ? (uint32_t)crop[2] : sw, cropped ? (uint32_t)crop[3] : sh, dev.swap_extent.width,
                                dev.swap_extent.height);
    }
    menu_tick();
    Frame& f = frames[frame_index];
    const int log_slot = frame_index;
    frame_index = (frame_index + 1) % 3;
    if (vkWaitForFences(dev.device, 1, &f.done, VK_TRUE, UINT64_MAX) == VK_ERROR_DEVICE_LOST) device_lost();
    framelog_collect(log_slot);
    uint32_t index = 0;
#ifdef _WIN32
    const uint64_t acquire_timeout = UINT64_MAX;
#else
    // (linux: a Wayland compositor gives no image back while the window cannot be seen: its screen asleep, another workspace. A wait
    // without end there held the presentation thread, and with it the game, until the screen came back. 100 ms, then the window is
    // taken as not shown: frames are not waited for and not shown, and the game goes on at its speed, as on Windows, until an image
    // comes back)
    static bool unseen = false;
    const uint64_t acquire_timeout = unseen ? 0 : 100000000ull;
#endif
    const bool ffx = ffxfg::active();   // (the swapchain is AMD's: its functions acquire, and present below)
    VkResult res = ffx ? ffxfg::acquire(dev.swapchain, acquire_timeout, f.acquired, &index) : vkAcquireNextImageKHR(dev.device, dev.swapchain, acquire_timeout, f.acquired, VK_NULL_HANDLE, &index);
    if (res == VK_ERROR_DEVICE_LOST) device_lost();
#ifndef _WIN32
    if (res == VK_TIMEOUT || res == VK_NOT_READY) {
        if (!unseen) logf("vk: the window is not shown (no image back in 100 ms): frames are not shown until it is");
        unseen = true;
        return;
    }
    if (unseen && (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR)) {
        unseen = false;
        logf("vk: the window is shown again");
    }
#endif
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        if (ffx) ffxfg::quiesce();
        std::lock_guard qk(dev.present_mu);
        create_swapchain(dev.swap_extent.width, dev.swap_extent.height, true);
        return;
    }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) return;
    if (vkResetFences(dev.device, 1, &f.done) == VK_ERROR_DEVICE_LOST) device_lost();
    if (vkResetCommandPool(dev.device, f.pool, 0) == VK_ERROR_DEVICE_LOST) device_lost();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(f.cmd, &bi) == VK_ERROR_DEVICE_LOST) device_lost();
    VkImage dst = dev.swap_images[index];
    full_barrier(f.cmd);   // (after the game's work for the frame: earlier submits on this queue)
    // ((Triforce) RX_VK_CHECKPOINTS: the present's blit has markers: begun (after the image's acquire, which its submit waits for),
    // the menu, and its end. The owner's device losses stopped between two frames, where the present had none: checkpoint_kinds.h)
    checkpoint_mark(f.cmd, index, 'p');
    image_barrier(f.cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const PresentAspect aspect{src && host && host->aspect_ratio ? host->aspect_ratio() : 0.0f};
    const PresentRect rect = present_rect(sw, sh, crop, dev.swap_extent.width, dev.swap_extent.height, aspect);
    [[maybe_unused]] uint32_t trace_route = 0; // direct; 1 scaled, 2 clear, 3 HDR conversion
    const bool has_bars = rect.x || rect.y || rect.w != dev.swap_extent.width || rect.h != dev.swap_extent.height;
    if (has_bars) {
        const VkClearColorValue black{0.0f, 0.0f, 0.0f, 1.0f};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        // The following blit overlaps the clear in the content rectangle.
        image_barrier(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    const VkImage scaled = src && view ? framegen_scale(f.cmd, view, src, sw, sh, crop, rect.w, rect.h) : VK_NULL_HANDLE;
    // (hookshot) the Insert menu's HDR: the picture (scaled, or the game's own with its crop) to HDR10, copied in as the SDR one is
    // (lab, 11-13) RX_VK_TEST_HDR_PASS=1: the HDR pass on a screen that is not HDR (its colours are wrong there, by design):
    // whether the pass gets a picture at all can be seen without an HDR display
    static const bool test_hdr = getenv("RX_VK_TEST_HDR_PASS") != nullptr;
    const VkImage hdr_image = (dev.hdr || test_hdr) && src && view && !present_native_pq.load(std::memory_order_relaxed)
        ? (scaled ? framegen_hdr(f.cmd, framegen_scaled_view(), rect.w, rect.h, nullptr, rect.w, rect.h)
                  : framegen_hdr(f.cmd, view, sw, sh, crop, rect.w, rect.h))
        : VK_NULL_HANDLE;
    if (hdr_image) {
        trace_route = 3;
        full_barrier(f.cmd);
        blit_crop_to_rect(f.cmd, hdr_image, rect.w, rect.h, nullptr, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rect);
    } else if (scaled) {
        trace_route = 1;
        blit_crop_to_rect(f.cmd, scaled, rect.w, rect.h, nullptr, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rect);
    } else if (src) {
        blit_crop_to_rect(f.cmd, src, sw, sh, crop, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rect);
    } else {
        trace_route = 2;
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    }
    if (src && framelog_on()) {
        static uint32_t seq;
        LogSlot& l = log_slots[log_slot];
        image_barrier(f.cmd, log_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        blit_crop(f.cmd, src, sw, sh, crop, log_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kLogW, kLogH);
        image_barrier(f.cmd, log_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {kLogW, kLogH, 1}};
        vkCmdCopyImageToBuffer(f.cmd, log_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l.buf, 1, &c);
        capture_host_read_barrier(f.cmd, l.buf, (VkDeviceSize)kLogW * kLogH * 4);
        l.used = true, l.at = host->now_ns(), l.seq = seq++, l.kind = present_kind.load();
    }
    if (ffx) {   // (AMD's frame generation: this frame's configuration, and its preparation recorded here, before the overlays go over the picture)
        ffxfg::FrameArgs fa;
        fa.cmd = f.cmd, fa.swapchain = dev.swapchain, fa.format = dev.swap_format, fa.extent = dev.swap_extent;
        fa.rect[0] = (int)rect.x, fa.rect[1] = (int)rect.y, fa.rect[2] = (int)rect.w, fa.rect[3] = (int)rect.h;
        fa.game_frame = src != VK_NULL_HANDLE;
        fa.plain = trace_route == 0;
        ffxfg::frame(fa);
    }
    // Geometry trace: after choosing the actual presentation route, without frame-log pixel readback.
    static const bool trace_geometry = getenv("RX_VK_PRESENT_LOG") && atoi(getenv("RX_VK_PRESENT_LOG")) != 0;
    if (trace_geometry) {
        static PresentGeometryTrace trace;
        PresentGeometrySample sample;
        const PresentContentSize content = present_content_size(sw, sh, crop);
        sample.source = {sw, sh};
        sample.content = {content.w, content.h};
        sample.output = {dev.swap_extent.width, dev.swap_extent.height};
        sample.destination = {rect.x, rect.y, rect.w, rect.h};
        sample.crop_supplied = crop != nullptr;
        if (crop) std::copy_n(crop, 4, sample.crop.begin());
        sample.sampled_view = view != VK_NULL_HANDLE;
        sample.host_aspect(src && host && host->aspect_ratio, aspect.ratio);
        sample.kind = src ? std::min(present_kind.load(), 1u) : 2;
        sample.route = trace_route;
        sample.filter = opts.upscale;
        sample.sharpness = opts.sharpness;
        sample.framegen = opts.framegen;
        const PresentTraceDecision decision = trace.note(sample);
        if (decision == PresentTraceDecision::Emit) {
            const char* kinds[] = {"game", "made", "startup"};
            const char* routes[] = {"direct", "scaled", "clear", "hdr"};
            const char* aspects[] = {"unavailable", "invalid", "valid"};
            logf("vk: present geometry: kind=%s source=%ux%u crop=%s:%d,%d %dx%d content=%ux%u output=%ux%u "
                 "destination=%u,%u %ux%u host-aspect=%s:%.9g route=%s view=%d filter-request=%d sharpness-request=%d framegen-request=%d",
                 kinds[sample.kind], sw, sh, crop ? "supplied" : "none", sample.crop[0], sample.crop[1], sample.crop[2], sample.crop[3],
                 content.w, content.h, sample.output[0], sample.output[1], rect.x, rect.y, rect.w, rect.h,
                 aspects[sample.aspect_state], sample.aspect, routes[sample.route], int(sample.sampled_view), sample.filter,
                 sample.sharpness, sample.framegen);
        } else if (decision == PresentTraceDecision::Limit) {
            logf("vk: present geometry: limit of %u changes reached; further geometry messages suppressed", PresentGeometryTrace::kLimit);
        }
    }
    checkpoint_mark(f.cmd, index, 'M');   // ((Triforce) RX_VK_CHECKPOINTS)
    menu_record(f.cmd, index, dev.swap_extent.width, dev.swap_extent.height);   // (hookshot) Dear ImGui's overlays, over all of it
    image_barrier(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    dlssg::frame(f.cmd, dev.swap_extent.width, dev.swap_extent.height);   // (Triforce: this frame's camera, depth and motion for Streamline)
    checkpoint_mark(f.cmd, index, 'P');   // ((Triforce) RX_VK_CHECKPOINTS)
    if (vkEndCommandBuffer(f.cmd) == VK_ERROR_DEVICE_LOST) device_lost();
    // after the image's acquisition. (Not on the timeline: one queue runs submits in order, and a wait for a value
    // another thread had counted but not yet submitted blocked the queue for good)
    VkSemaphoreSubmitInfo waits[1] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    waits[0].semaphore = f.acquired;
    waits[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = dev.swap_rendered[index];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = f.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = waits;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    std::unique_lock qk(dev.present_mu);
    if (!pend.cbs.empty()) flush_reason = FR_PRESENT;
    queue_flush_locked();
    static int64_t presents_counted, st_flushes0, st_cbs0, r0[FR_COUNT];
    if (st_flushes && ++presents_counted % 120 == 0) {
        logf("vk: batched submits: %lld flushes, %.1f command buffers each, over 120 presents; by reason: poll %lld, wait %lld, "
             "slot %lld, present %lld, cap %lld, value %lld, other %lld", (long long)(st_flushes - st_flushes0),
             (double)(st_batched_cbs - st_cbs0) / std::max<int64_t>(1, st_flushes - st_flushes0),
             (long long)(st_reason[FR_POLL] - r0[FR_POLL]), (long long)(st_reason[FR_WAIT] - r0[FR_WAIT]),
             (long long)(st_reason[FR_SLOT] - r0[FR_SLOT]), (long long)(st_reason[FR_PRESENT] - r0[FR_PRESENT]),
             (long long)(st_reason[FR_CAP] - r0[FR_CAP]), (long long)(st_reason[FR_VALUE] - r0[FR_VALUE]),
             (long long)(st_reason[FR_OTHER] - r0[FR_OTHER]));
        st_flushes0 = st_flushes, st_cbs0 = st_batched_cbs;
        std::memcpy(r0, st_reason, sizeof r0);
    }
    reflex_submit_ends();
    dlssg::submit_end();
    // (Triforce: where the blit goes among the timeline's values, for the stall line's waits behind a present)
    stall_ct.blit_after[stall_ct.blits.fetch_add(1, std::memory_order_relaxed) % 8].store(stall_ct.queue_top.load(std::memory_order_relaxed),
                                                                                        std::memory_order_relaxed);
    res = vkQueueSubmit2(dev.queue, 1, &si, f.done);
    if (res == VK_ERROR_DEVICE_LOST) device_lost();
    // ((Triforce) RX_VK_PRESENT_THREAD: a picture of the game's goes on from here on the present thread (present_thread.h), its
    // blit on the queue as before; this thread goes on to its next frame. Otherwise the rest here, as before)
    const PresentOut out{index, smooth_mine, ffx};
    if (present_thread_wanted()) {
        qk.unlock();
        return present_thread_hand(out);
    }
    present_out(out, qk, false);
}

/// (Triforce) A present's last step, its blit on the queue: the pacing's wait for an early picture, the present with its
/// markers, the frame log. On the presenting thread with the queue's lock (qk) held (on_thread false), or on the present
/// thread with it not taken: there a swapchain out of date or suboptimal is made again by the next present_image, as after
/// a resize (it waits for this one first)
static void present_out(const PresentOut& o, std::unique_lock<std::mutex>& qk, bool on_thread) {
    const bool ffx = o.ffx;
    uint32_t index = o.index;
    VkResult res = VK_SUCCESS;
    const auto t0 = std::chrono::steady_clock::now();
    smooth_present(qk);   // ((Triforce) RX_VK_PACE_SMOOTH: an early picture of the game's waits here, its blit on the queue)
    const auto t1 = std::chrono::steady_clock::now();
    if (vk_ft_on()) vk_ft("pace waited %.3f ms", std::chrono::duration<double, std::milli>(t1 - t0).count());
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.pNext = reflex_present_id(nullptr);   // (NVIDIA Reflex: the frame's present id, and its present's markers)
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &dev.swap_rendered[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &dev.swapchain;
    pi.pImageIndices = &index;
    antilag_present();   // (AMD Anti-Lag 2: the frame's present stage)
    dlssg::present_marker(false);   // (Triforce, DLSS frame generation: the present goes through Streamline's interposer below)
    // (hookshot) vkQueuePresentKHR blocked a quarter of the display thread's time with frame interpolation at 4x (a 14700K and an
    // RTX 5070 Ti), and with the queue's lock held there the game's main thread waited ~9% of its time and the presentation thread
    // ~15% to submit. With a queue of their own (dev.present_queue, a compute family's: vk_main.cpp create_device) the presents go
    // out with the submits' lock let go: the blit (present_image) signals swap_rendered on the replay's queue (submitted already, as a binary
    // semaphore's wait needs). (Waiting for the blit's fence before the present, with the lock let go, was worse.)
    // ((Triforce) the lock may be let go already: smooth_present waited without it)
    const auto ft_qp0 = std::chrono::steady_clock::now();
    if (ffx) {
        // (AMD's swapchain: its present submits on the game's queue through its callback, which takes dev.present_mu itself, and
        // hands the real presents to its own queue and thread: so with the lock let go, as for the queue of their own)
        if (qk.owns_lock()) qk.unlock();
        res = ffxfg::present(&pi);
    } else if (dev.present_queue) {
        if (qk.owns_lock()) qk.unlock();
        std::lock_guard pq(dev.present_queue_mu);
        res = vkQueuePresentKHR(dev.present_queue, &pi);
    } else {
        if (!qk.owns_lock()) qk.lock();
        res = vkQueuePresentKHR(dev.queue, &pi);
    }
    if (vk_ft_on()) vk_ft("qpresent %.3f ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ft_qp0).count());
    if (res == VK_ERROR_DEVICE_LOST) device_lost();
    {   // ((Triforce) the present's 120-present line: present_thread_line)
        const auto t2 = std::chrono::steady_clock::now();
        const int64_t call = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        present_fig.outs.fetch_add(1, std::memory_order_relaxed);
        present_fig.paced_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count(), std::memory_order_relaxed);
        present_fig.call_ns.fetch_add(call, std::memory_order_relaxed);
        fig_max(present_fig.call_max_ns, call);
    }
    dlssg::present_marker(true);
    pace::presented();   // (Triforce: a present went out, for the limiters beside the game's own wait: vk_pace.cpp)
    reflex_presented();
    if (res < 0 && res != VK_ERROR_OUT_OF_DATE_KHR) {   // (a present that failed: said, a few times)
        static int logged;
        if (logged++ < 8) logf("vk: present failed (VkResult %d%s)", (int)res, dev.present_queue ? ", on the presents' own queue" : "");
    }
    {   // (RX_VK_FRAME_LOG=<file>: one CSV line a present: frame, seconds since the first, frame time in ms. Frame
        // pacing on the Steam Deck, where MangoHud's log stays empty under gamescope)
        static FILE* fl = getenv("RX_VK_FRAME_LOG") ? std::fopen(getenv("RX_VK_FRAME_LOG"), "w") : nullptr;
        static std::chrono::steady_clock::time_point first, prev;
        static uint64_t frames;
        if (fl) {
            const auto t = std::chrono::steady_clock::now();
            if (!frames) first = prev = t, std::fputs("frame,time_s,frametime_ms\n", fl);
            std::fprintf(fl, "%llu,%.4f,%.3f\n", (unsigned long long)frames, std::chrono::duration<double>(t - first).count(),
                         std::chrono::duration<double, std::milli>(t - prev).count());
            prev = t;
            if (++frames % 60 == 0) std::fflush(fl);
        }
    }
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        if (ffx) ffxfg::quiesce();
        if (on_thread) {   // ((Triforce) as a resize: unless a newer one is noted already, which the next present takes)
            uint64_t none = 0;
            dev.swap_resize.compare_exchange_strong(none, (uint64_t)dev.swap_extent.width << 32 | dev.swap_extent.height,
                                                    std::memory_order_release, std::memory_order_relaxed);
            return;
        }
        if (!qk.owns_lock()) qk.lock();
        create_swapchain(dev.swap_extent.width, dev.swap_extent.height, true);
    }
}


namespace {

void set_texture_pool_size(int) {}
void read_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* dst, uint64_t row,
                 uint64_t img) {
    finish();   // (what the submits before it wrote)
    std::lock_guard lk(replay_mu);
    {   // (RX_VK_LOG_READ_TEXELS=1: the CPU reading textures back, 200 lines: are they ones the GPU wrote?)
        static const bool log_reads = getenv("RX_VK_LOG_READ_TEXELS") != nullptr;
        static int logged;
        if (log_reads && t && logged < 200) {
            logged++;
            Texture* tex = texture_get(t, false);
            logf("vk: ReadTexels %#llx %ux%ux%u NVN %#x level %u layer %u region (%d,%d,%d %dx%dx%d) row %llu: %s", (unsigned long long)t->gpu,
                 t->d.width, t->d.height, t->d.depth, t->d.format, level, layer0, r ? r->xoffset : 0, r ? r->yoffset : 0,
                 r ? r->zoffset : 0, r ? r->width : 0, r ? r->height : 0, r ? r->depth : 0, (unsigned long long)row,
                 !tex ? "no image" : tex->gpu_written ? (tex->released ? "rendered, finalized" : "RENDERED (guest memory is stale)")
                                                     : "uploaded");
        }
    }
    texture_read_texels(t, level, layer0, r, dst, row, img, false);
}
/// (host: virtual pages [gpu, gpu + size) are about to be unmapped or remapped) what the GPU wrote into textures there
/// goes into their memory first, as on the console, where it is there all along (vk_texture.cpp, texture_flush_to_guest)
bool replay_behind();
void vmap_changing(uint64_t gpu, uint64_t size) {
    if (!size) return;
    static const bool log_calls = getenv("RX_VK_LOG_VMAP_CHANGING") != nullptr;   // (each call, 300 lines)
    static std::atomic<int> logged{0};
    // (the list of GPU-written textures is complete only once the replay has caught up: MK World unmaps a tile's pool
    // a frame or two after it submitted the drawing, which the replay may not have recorded yet)
    const bool listed = texture_gpu_written_in(gpu, size);
    bool behind = false, listed_after = listed;
    if (!listed) {
        behind = replay_behind();
        if (behind) {
            wait_queue_empty();
            listed_after = texture_gpu_written_in(gpu, size);
        }
    }
    if (log_calls && logged++ < 300)
        logf("vk: vmap changing %#llx..%#llx: listed %d, replay behind %d, listed after %d", (unsigned long long)gpu,
             (unsigned long long)(gpu + size), (int)listed, (int)behind, (int)listed_after);
    if (!listed_after) return;
    wait_queue_empty();   // (the work the game recorded before: submitted)
    std::lock_guard lk(replay_mu);
    const int n = texture_flush_to_guest(gpu, size);
    if (log_calls && logged < 300) logf("vk: vmap changing %#llx: %d written back", (unsigned long long)gpu, n);
}
void write_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, const uint8_t* src,
                  uint64_t row, uint64_t img) {
    finish();   // (after the submits before it read the texture)
    std::lock_guard lk(replay_mu);
    texture_read_texels(t, level, layer0, r, const_cast<uint8_t*>(src), row, img, true);
}
void stats(int64_t* draws, int64_t* skipped, int64_t* pipelines) {
    int64_t v[4] = {0, 0, 0, 0};
    draw_stats(&v[0], &v[1], &v[2], &v[3]);
    if (draws) *draws = v[0];
    if (skipped) *skipped = v[1];
    if (pipelines) *pipelines = v[3];
}
void register_texture_locked(int id, const nvn_tex* t, const nvn_view* v) {
    register_texture(id, t, v);
    texture_warm_note(id);   // ((Triforce) tex_warm.h)
}
void do_program_released(const nvn_prog* p) {
    std::lock_guard lk(replay_mu);   // (the replay may be translating it)
    // ((Triforce) and once no program pick has the shaders' lock let go for a read (vk_shader.cpp, program_read_once.h): the
    // pick holds its program's stage codes, which the release lets go)
    program_released_after_reads(p);
}

// ---- the replay thread (RX_VK_ASYNC=0: none) ----
// The game's submits and presents are queued and replayed by a thread of the back end's own, so the game's thread
// (MKW's Presentation Thread, 70% busy replaying before) goes on with its frame while the last one is recorded. As on
// the console, where the GPU reads the commands and the memory they name after the submit: the host resolves the
// deferred bindings at the submit and keeps the recordings until rx_nvn_replayed() passes them (nvn_native.c), and
// a fence value is handed out at the submit, signalled when the replay's submission completes. At most two presents
// wait: the game is held at that when the replay falls behind. Finalized textures and programs are queued too, in
// order with the work that may still use them.
enum JobKind { JOB_SUBMIT, JOB_PRESENT, JOB_TEXTURE_RELEASED, JOB_PROGRAM_RELEASED, JOB_REGISTER_TEXTURE, JOB_REGISTER_SAMPLER,
               JOB_EVENT_SIGNAL };
struct Job {
    JobKind kind;
    std::vector<uint64_t> handles;
    nvn_tex tex;
    bool has_tex = false;
    int id = 0;                 // (a registration's slot, its view and sampler)
    nvn_view view{};
    bool has_view = false;
    nvn_samp_par samp{};
    uint64_t ev_gpu = 0;        // (a CPU signal: the event and its value)
    uint32_t ev_value = 0;
    bool ev_capture = false;    // (Triforce: a capture was taken at the signal, on the game's thread: event_signal_queued)
    uint64_t value = 0;         // (a submit's or present's timeline value: its fence, handed out as it came)
    uint64_t queue = 0;         // (a submit's queue)
    int crop[4] = {0, 0, 0, 0};
    bool has_crop = false;
    nvn_prog prog;
    std::vector<uint64_t> waits;   // ((Triforce) a submit: the events its command buffers wait for, when the host told them)
    bool waits_told = false;
    std::chrono::steady_clock::time_point queued;
};
std::mutex q_mu;
std::condition_variable q_cv, q_space, q_idle, q_done;
std::deque<Job> q_jobs;
std::atomic<int> q_count{0};   // ((Triforce) q_jobs' size, for the replay thread's look before it sleeps: no lock)
int q_presents = 0;
bool q_busy = false;
std::unordered_map<uint64_t, uint64_t> q_last_async;   // (queue -> the value of its last submit handed over; q_mu)
// (Triforce, async_rules.h 1) The submissions handed over and not replayed whole yet, with the events they wait for: a signal of
// the CPU's takes its capture on the game's thread only when one of them may still wait for that event (event_signal_queued).
// RX_VK_ASYNC_EVENT_NOTES=0: at every signal with the thread behind, as before
async_rules::Notes wait_notes;                  // (q_mu)
std::atomic<int> wait_notes_n{0};               // (its size: nothing to take the lock for on the replay's side when 0)
std::atomic<int64_t> st_signals{0}, st_signal_captures{0}, st_signal_captures_left{0};
thread_local std::vector<uint64_t> tl_submit_waits;   // (the host's word for the submit this thread makes next: submit_waits)
thread_local bool tl_submit_waits_told = false;
bool wait_notes_on() {
    static const bool v = async_on() && async_rules::on_unless_zero(getenv("RX_VK_ASYNC_EVENT_NOTES"));
    return v;
}
/// (the host, by name: mkwvk_submit_waits) The events the submit this thread is about to make waits for; n 0: none
void submit_waits(const uint64_t* events, int n) {
    if (!wait_notes_on()) return;
    tl_submit_waits.assign(events, events + std::max(n, 0));
    tl_submit_waits_told = true;
}
/// The replay thread: the submission of that value is replayed whole (run_submit returned true for it)
void async_wait_done(uint64_t value) {
    if (!wait_notes_n.load(std::memory_order_acquire)) return;   // (always 0 with the replay on the submitting thread)
    std::lock_guard lk(q_mu);
    wait_notes.done(value);
    wait_notes_n.store((int)wait_notes.pending.size() + (wait_notes.over ? 1 : 0), std::memory_order_release);
}
/// (the host, by name: mkwvk_replay_idle_wait) Until the replay thread has nothing handed over left to record, or the time is up:
/// the game's present poll waits here without looking, while its frame is still being recorded (host/hos_thread.c present_poll).
/// True when the thread is idle (or there is none)
bool replay_idle_wait(uint64_t timeout_ns) {
    if (!async_on()) return true;
    std::unique_lock lk(q_mu);
    return q_done.wait_for(lk, std::chrono::nanoseconds(timeout_ns), [] { return q_jobs.empty() && !q_busy; });
}
// (Triforce, 11-14) The replay thread with nothing forced (the default with RX_VK_ASYNC=1; RX_VK_ASYNC_FORCED=1: as it was).
// The thread's mode was left behind when the submitting thread's replay got its ordering rules (sync_settle, the queues' own
// fences, readbacks waited for on the recordings' timeline): every catch-up of the game's threads (a readback, a virtual
// pool remapped, a finish) still made the stopped queues go past their event waits, ahead of the work they wait for, and
// held the caller until the replay thread had nothing left. At the Market with the game held one present ahead: 36.7 fps
// against 60 on the submitting thread, 522 of 3000 readbacks waiting 11.5 ms each, 60,000 waits gone past in a run where
// the submitting thread's replay has none, 52 cell blinks in 3,616 frames against 0. Now, as there: a catch-up waits for
// what was handed over to be replayed as far as its waits allow and forces nothing; a fence is its own queue's last submit,
// waited for on the recordings' timeline; a readback waits for the recording that writes its range back, not for the queue
bool async_unforced() {
    static const bool v = async_on() && !(getenv("RX_VK_ASYNC_FORCED") && atoi(getenv("RX_VK_ASYNC_FORCED")) != 0);
    return v;
}
uint64_t fence_q_async(uint64_t queue) {
    std::lock_guard lk(q_mu);
    auto it = q_last_async.find(queue);
    return it != q_last_async.end() ? it->second : q_values.load();
}
/// A wait for a value the replay thread has not recorded whole yet: until its next job ends (or half a millisecond). False
/// when the thread has nothing that could record it (idle, no queue stopped)
bool async_wait_step(uint64_t) {
    std::unique_lock lk(q_mu);
    if (q_jobs.empty() && !q_busy && !blocked_n.load()) return false;
    q_done.wait_for(lk, std::chrono::microseconds(500));
    return true;
}
int presents_ahead() {   // (RX_VK_PRESENTS_AHEAD=<n>: how many presents the game may hand over ahead of the replay)
    // (Triforce: one with nothing forced: OoT writes its double-buffered data again a frame after it handed it over, and a
    // replay two presents behind read the next frame's: the white and black frames the thread was turned off for)
    static const int n = std::clamp(getenv("RX_VK_PRESENTS_AHEAD") ? atoi(getenv("RX_VK_PRESENTS_AHEAD")) : async_unforced() ? 1 : 2, 1, 8);
    return n;
}

bool async_on() {
    // (hookshot: off by default. OoT rewrites double-buffered data right after a submit with no fence, which lands
    // inside a replay running behind (TotK Recomp's reason to replay on the submitting thread): whole-screen white or
    // black frames, 28 per 1000 gameplay frames, against 1.3 synchronous, at the same frame rate. RX_VK_ASYNC=1: on)
    static const bool on = getenv("RX_VK_ASYNC") && atoi(getenv("RX_VK_ASYNC")) != 0;
    return on;
}

int64_t st_present_waits, st_present_waits_out;   // (Triforce-dev: presents that waited for their frame's signal; in vain)
/// One job of the replay thread
void run_job(Job& j) {
    switch (j.kind) {
    case JOB_SUBMIT:
        lab_ab::delay("replay");   // (lab: the replay thread, once a frame)
        submit_job(j.handles, j.value, j.queue);
        break;
    case JOB_PRESENT:   // (the frame's work whole before it is shown)
        // (Triforce, 11-14) With the game a frame ahead of the replay, what is stopped at a present is mostly the NEXT
        // frame's work: the worker queues hand theirs over early, each waiting for the main queue's signal of that
        // frame, which comes after this present. Made to go here it ran ahead of its signaller: one present in ten at the
        // Market (1,499 of 15,000, 14,375 waits gone past) against 29 in a run of the submitting thread's replay. So a
        // present can let on what may go and force nothing (RX_VK_PRESENT_DRAIN=0); work that stays stopped then goes on
        // at its signal, or after hold_ms as anywhere. NOT the default: the first run with it stood at a loading screen
        // for good (the Market save, 5 draws a frame for six minutes, 11-14 03:20): a load waits for work only a present
        // makes go. The picture with the presents forcing showed no flicker (0 cell blinks in 6,472 frames)
        // (Triforce, async_rules.h 5) The stale way is the default with nothing forced: the forcing was 5,649 to 15,166 waits gone
        // past in a run of the thread against 0 on the submitting thread, each of them work that ran ahead of what it waits for.
        // RX_VK_PRESENT_DRAIN=1: every present forces, as it was; =0: none does
        if (static const async_rules::PresentDrain way = async_rules::present_drain(getenv("RX_VK_PRESENT_DRAIN"));
            async_unforced() && way == async_rules::PresentDrain::Keep) {
            burst_begin(0);
            schedule();
            burst_end();
        } else if (async_unforced() && way == async_rules::PresentDrain::Stale) {
            // (Triforce-dev) The present and the CPU's signal that lets the frame's stopped work on come from two of the
            // game's threads: one frame in 25 at the Market the present was handed over first (0.15 ms before the signal).
            // Shown at once it was the frame before again, and the next present had two frames' work before it (34 ms).
            // When nothing stopped has gone on since the last present, the jobs behind this one are done first, until
            // stopped work goes on or RX_VK_PRESENT_WAIT_MS (8) have passed
            static const int wait_ms = getenv("RX_VK_PRESENT_WAIT_MS") ? std::max(0, atoi(getenv("RX_VK_PRESENT_WAIT_MS"))) : 8;
            static int64_t resumed_at_present = -1;
            if (wait_ms && blocked_n.load() && st_resumed == resumed_at_present) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
                st_present_waits++;
                while (blocked_n.load() && st_resumed == resumed_at_present) {
                    Job k;
                    {
                        std::unique_lock lk(q_mu);
                        if (!q_cv.wait_until(lk, deadline, [] { return !q_jobs.empty(); })) break;
                        if (q_jobs.front().kind == JOB_PRESENT) break;
                        k = std::move(q_jobs.front()), q_jobs.pop_front();
                        q_count.fetch_sub(1, std::memory_order_release);
                    }
                    run_job(k);
                    update_blocked();
                }
                if (st_resumed == resumed_at_present) st_present_waits_out++;
            }
            drain_stale();
            resumed_at_present = st_resumed;
        } else {
            drain(0);
        }
        present_value = j.value;
        do_present(j.has_tex ? &j.tex : nullptr, j.has_crop ? j.crop : nullptr);
        break;
    case JOB_TEXTURE_RELEASED:   // (after the stopped work that may use it)
        // (Triforce: with nothing forced the release is kept until the stopped work has gone on, as on the submitting
        // thread's replay: update_blocked makes it then)
        if (async_unforced() && hold_releases() && blocked_n.load()) {
            std::lock_guard hl(held_mu);
            held_tex_releases.push_back(j.tex);
            st_held_releases++;
            break;
        }
        drain(1);
        do_texture_released(&j.tex);
        break;
    case JOB_PROGRAM_RELEASED:
        if (async_unforced() && hold_releases() && blocked_n.load()) {
            std::lock_guard hl(held_mu);
            held_prog_releases.push_back(j.prog);
            st_held_releases++;
            break;
        }
        drain(1);
        do_program_released(&j.prog);
        break;
    case JOB_REGISTER_TEXTURE:
        register_texture_copy(j.id, j.tex, j.has_view ? &j.view : nullptr);
        texture_warm_note(j.id);   // ((Triforce) tex_warm.h)
        break;
    case JOB_REGISTER_SAMPLER: register_sampler_copy(j.id, j.samp); break;
    case JOB_EVENT_SIGNAL:   // (the CPU set an event: the stopped ones may go on)
        ev_values[j.ev_gpu] = j.ev_value;
        if (j.ev_capture) {   // (the CPU's writes up to its signal, for what goes on from here: see event_signal_queued)
            std::lock_guard lk(replay_mu);
            uint64_t v = rec_value_now;
            for (const auto& [queue, q] : qruns)
                if (q.waiting) v = std::max(v, q.value);
            memory_take_capture(v, completed());
            st_resume_captures++;
        }
        burst_begin(0);
        schedule();
        burst_end();
        break;
    }
}

void replay_thread_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk replay");
    SetThreadPriority(GetCurrentThread(), replay_prio::own_thread(replay_prio_level()));   // ((Triforce) RX_VK_REPLAY_PRIO: replay_prio.h)
    // (on the core the host keeps free of the game's threads, hos_reserved_cpus: its last physical one)
    if (host->reserved_cpus) {
        if (const uint64_t m = host->reserved_cpus()) {
            SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)m);
            logf("vk: the replay thread runs on logical processors %#llx, kept from the game's threads", (unsigned long long)m);
        }
    }
    // ((Triforce) RX_VK_ASYNC_SPIN_US: the thread looks for its next job a moment before it sleeps (async_rules.h): a frame's
    // hand-overs come a fraction of a millisecond apart, and each wake of a sleeping thread was tens of microseconds of a frame
    // the game waits for)
    static const int64_t spin_ns = async_rules::spin_ns(getenv("RX_VK_ASYNC_SPIN_US"));
    for (;;) {
        Job j;
        bool have = false;
        if (spin_ns) {
            const auto t0 = std::chrono::steady_clock::now();
            while (!q_count.load(std::memory_order_acquire) && drain_waiters.load() <= 0 &&
                   std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count() < spin_ns)
                gate_wait::pause();
        }
        {
            std::unique_lock lk(q_mu);
            if (blocked_n.load())   // (a queue stopped at a wait: the next job, or on without it after a while)
                q_cv.wait_until(lk, blocked_since + std::chrono::milliseconds(hold_ms()),
                                [] { return !q_jobs.empty() || drain_waiters.load() > 0; });
            else
                q_cv.wait(lk, [] { return !q_jobs.empty(); });
            if (!q_jobs.empty()) j = std::move(q_jobs.front()), q_jobs.pop_front(), have = true, q_count.fetch_sub(1, std::memory_order_release);
            q_busy = true;
        }
        if (blocked_n.load()) {   // (stopped ones: on now when another thread waits for the replay, or stopped too long)
            if (!have && drain_waiters.load() > 0) drain(2);   // (the jobs before its wait done)
            else if (std::chrono::steady_clock::now() >= blocked_since + std::chrono::milliseconds(hold_ms())) drain(3);
        }
        if (!have) {
            update_blocked();
            std::lock_guard lk(q_mu);
            q_busy = false;
            if (q_jobs.empty()) q_idle.notify_all();   // (the waiters look at blocked_n themselves: wait_queue_empty)
            q_done.notify_all();
            continue;
        }
        // (RX_VK_TIMELINE: one frame's jobs every 240 presents: when the game handed each over, how long it waited
        // for the replay thread, how long it took, its draws; the present's time is 0)
        static const bool timeline = getenv("RX_VK_TIMELINE") != nullptr;
        static const bool timeline_late = timeline && atoi(getenv("RX_VK_TIMELINE")) >= 2;
        const auto t_start = std::chrono::steady_clock::now();
        int64_t d0 = 0, x;
        if (timeline) draw_stats(&d0, &x, &x, &x);
        // (Triforce, 11-14, diagnostics) RX_VK_FT=<present>: from that present on, RX_VK_FT_N presents (8): each job of this
        // thread at its start and its end, by the host's clock (QueryPerformanceCounter ms, the last 100 s): the renderer's
        // half of the host's frame trace (HOS_FT, nvn_native.c)
        static const int64_t ft_from = getenv("RX_VK_FT") ? atoll(getenv("RX_VK_FT")) : -1;
        static const int64_t ft_n = getenv("RX_VK_FT_N") ? atoll(getenv("RX_VK_FT_N")) : 8;
        const bool ft = ft_from >= 0 && presents >= ft_from && presents < ft_from + ft_n;
        const auto ft_ms = [] {
            return (double)(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() %
                            100000000000ll) / 1e6;
        };
        static const char* const kinds[] = {"submit", "present", "texture released", "program released", "register texture",
                                            "register sampler", "event signal"};
        int64_t ft_d0 = 0;
        if (ft) {
            draw_stats(&ft_d0, &x, &x, &x);
            if (j.kind == JOB_SUBMIT || j.kind == JOB_PRESENT || j.kind == JOB_EVENT_SIGNAL)
                logf("ft %.3f job> %s value %llu queue %#llx (handed over %.2f ms ago; GPU done up to %llu)", ft_ms(),
                     kinds[j.kind], (unsigned long long)j.value, (unsigned long long)j.queue,
                     std::chrono::duration<double, std::milli>(t_start - j.queued).count(), (unsigned long long)completed());
        }
        run_job(j);
        update_blocked();
        if (ft && (j.kind == JOB_SUBMIT || j.kind == JOB_PRESENT || j.kind == JOB_EVENT_SIGNAL)) {
            int64_t ft_d1 = 0;
            draw_stats(&ft_d1, &x, &x, &x);
            logf("ft %.3f job< %s value %llu: %lld draws, %d queues stopped, GPU done up to %llu", ft_ms(), kinds[j.kind],
                 (unsigned long long)j.value, (long long)(ft_d1 - ft_d0), blocked_n.load(), (unsigned long long)completed());
        }
        if (j.kind == JOB_PRESENT && presents % 3600 == 0) log_order_stats();
        if (timeline && (j.kind == JOB_SUBMIT || j.kind == JOB_PRESENT)) {
            struct T { char k; double q, s, e; int64_t d; size_t h; };
            static std::vector<T> frame;
            const auto t_end = std::chrono::steady_clock::now();
            int64_t d1 = 0;
            draw_stats(&d1, &x, &x, &x);
            auto ms = [](auto a) { return std::chrono::duration<double, std::milli>(a.time_since_epoch()).count(); };
            frame.push_back({j.kind == JOB_SUBMIT ? 's' : 'p', ms(j.queued), ms(t_start), ms(t_end), d1 - d0, j.handles.size()});
            if (j.kind == JOB_PRESENT) {
                // (RX_VK_TIMELINE=2: also every frame handed over 20 ms or more after the last one, 200 of them)
                static double last_q = 0;
                static int late_logged = 0;
                const double q_now = frame.back().q;
                const bool late = timeline_late && last_q && q_now - last_q >= 20.0 && late_logged < 200;
                if (late) late_logged++;
                last_q = q_now;
                if (presents % 240 == 0 || late) {
                    const double base = frame.back().q;
                    char line[2048];
                    int n = snprintf(line, sizeof line, "vk: timeline%s (ms from the present's hand-over):",
                                     late ? " of a late frame" : "");
                    for (const T& t : frame)
                        if (n < 1900)
                            n += snprintf(line + n, sizeof line - n, " %c q%+.1f s%+.1f e%+.1f %lldd %zuh;", t.k, t.q - base,
                                          t.s - base, t.e - base, (long long)t.d, t.h);
                    logf("%s", line);
                }
                frame.clear();
            }
        }
        std::lock_guard lk(q_mu);
        if (j.kind == JOB_PRESENT) q_presents--, q_space.notify_all();
        q_busy = false;
        if (q_jobs.empty()) q_idle.notify_all();
        q_done.notify_all();
    }
}

void enqueue(Job&& j) {
    j.queued = std::chrono::steady_clock::now();
    std::unique_lock lk(q_mu);
    static bool started;
    if (!started) {
        started = true;
        std::thread(replay_thread_main).detach();
        logf("vk: the replay runs on a thread of its own (RX_VK_ASYNC=0: on the submitting thread)");
    }
    if (j.kind == JOB_PRESENT) {
        q_space.wait(lk, [] { return q_presents < presents_ahead(); });
        q_presents++;
    }
    if (j.kind == JOB_SUBMIT || j.kind == JOB_PRESENT) j.value = ++q_values;
    if (j.kind == JOB_SUBMIT && j.queue) q_last_async[j.queue] = j.value;
    if (j.kind == JOB_SUBMIT && wait_notes_on()) {   // ((Triforce) noted until it is replayed whole: async_wait_done)
        wait_notes.add(j.value, j.waits_told, j.waits.data(), (int)j.waits.size());
        wait_notes_n.store((int)wait_notes.pending.size() + (wait_notes.over ? 1 : 0), std::memory_order_release);
    }
    q_jobs.push_back(std::move(j));
    q_count.fetch_add(1, std::memory_order_release);
    q_cv.notify_one();
}

void wait_queue_empty() {
    if (!async_on()) {
        // A catch-up (a queue finish, a CPU read back of what the GPU wrote, a pool remapped) used to force every queue
        // stopped at an event wait past it. The work those waits are for is mostly not submitted yet (the waiting thread is the one
        // that submits it), so the stopped submission went on without it and its lit passes read what that work makes before it
        // was made: the scene black, or grey, or blown out for a frame, every other frame for seconds at a time in Kokiri Forest
        // once the game ran at 60 fps (at 4-8 fps the work had always come first). Measured over 70 s of Kokiri gameplay: 10000-
        // 40000 waits gone past and 250-1100 flashing frames before, ~300 and ~65 now, 60 fps either way. A catch-up now lets on the
        // waits that are met; what stays stopped goes on with its own queue's work, a present, or sync_watch's hold_ms as before.
        // RX_VK_CATCHUP_FORCE=1: forced, as before.
        static const bool force = getenv("RX_VK_CATCHUP_FORCE") != nullptr;
        if (force) return sync_drain(2);
        if (!blocked_n.load()) return;
        std::lock_guard lk(sync_order_mu);
        burst_begin(0);
        schedule();
        burst_end();
        update_blocked();
        return;
    }
    std::unique_lock lk(q_mu);
    if (async_unforced()) {   // (what was handed over, replayed as far as its waits allow: nothing made to go past one)
        q_idle.wait(lk, [] { return q_jobs.empty() && !q_busy; });
        return;
    }
    drain_waiters++;   // (stopped submissions go on now: replay_thread_main)
    q_cv.notify_all();
    q_idle.wait(lk, [] { return q_jobs.empty() && !q_busy && blocked_n.load() == 0; });
    drain_waiters--;
}
/// (work handed to the replay thread and not yet recorded)
bool replay_behind() {
    if (!async_on()) return blocked_n.load() > 0;
    std::lock_guard lk(q_mu);
    return !q_jobs.empty() || q_busy || blocked_n.load() > 0;
}

// ---- (Triforce) RX_VK_GATE_WAIT_MS (gate_wait.h): a submission that begins with a wait for the CPU's signal waits for it at the
// submit, before its capture, after NX2navi's gate_wait_ms ----
/// What an op is to the gate's look for the wait a command buffer begins with: 1 state the replay only stores, 2 a wait, 0 else
int gate_kind(uint32_t op) {
    switch (op) {
    case NOP_BEGIN: case NOP_JUMP: case NOP_CALL: case NOP_SET_RT: case NOP_VIEWPORTS: case NOP_SCISSOR: case NOP_DEPTH_RANGE:
    case NOP_BIND_PROGRAM: case NOP_BIND_STATE: case NOP_BIND_VATTR: case NOP_BIND_VSTREAM: case NOP_BIND_VBUF: case NOP_BIND_UBO:
    case NOP_BIND_SSBO: case NOP_BIND_TEX: case NOP_BIND_SAMP: case NOP_BIND_IMG: case NOP_BIND_BUFS: case NOP_STENCIL:
    case NOP_POLY_OFFSET: case NOP_SET_TEX_POOL: case NOP_SET_SAMP_POOL: case NOP_COPY_STRIDE: case NOP_BLEND_COLOR:
    case NOP_DEPTH_CLAMP: case NOP_RASTER_DISCARD: case NOP_PRIMITIVE_RESTART: case NOP_PUSH_CONST:
        return 1;
    case NOP_WAIT_EVENT: return 2;
    default: return 0;
    }
}
int64_t gate_clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<int64_t> gate_n{0}, gate_ns{0}, gate_outs{0}, gate_all{0}, gate_all_outs{0};
std::atomic<bool> gate_off{false};
/// The submitting thread, before the order lock and the capture (submit_q): waits for the CPU's signal of the event the submission's
/// first command buffer begins with a wait for, when the replay would stop there; at most the bound. Several threads submit: atomics
void gate_wait_signal(const uint64_t* handles, int n) {
    static const int64_t cap_ns = [] {
        const int ms = gate_wait::ms(getenv("RX_VK_GATE_WAIT_MS"));
        if (ms) logf("vk: RX_VK_GATE_WAIT_MS=%d: a submission that begins with a wait for an event the game signals from the CPU waits "
                     "for the signal at its submit, %d ms at most, before its capture", ms, ms);
        return (int64_t)ms * 1000000;
    }();
    if (!cap_ns || n < 1 || !handles[0] || !sync_waits_on() || gate_off.load(std::memory_order_relaxed)) return;
    const uint8_t* const w = gate_wait::first_wait(reinterpret_cast<const uint8_t*>(handles[0]), gate_kind);
    if (!w) return;
    const nvn_p_wait_event ev = wait_at(w);
    {   // (only where the replay would stop at it, for an event the CPU signals; a replay under way elsewhere: as before)
        std::unique_lock lk(sync_order_mu, std::try_to_lock);
        if (!lk || wait_met_at(w)) return;
        std::lock_guard ek(ev_cpu_mu);
        if (!cpu_events.count(ev.gpu)) return;
    }
    const int64_t t0 = gate_clock_ns();
    bool met = false;
    for (uint32_t i = 0;; i++) {   // (no lock held: the event's word in the game's memory, which the CPU's signal writes)
        uint32_t v = 0;
        host->gpu_read(ev.gpu, &v, 4);
        if (event_wait_met(v, ev.mode, ev.value)) {
            met = true;
            break;
        }
        if ((i & 15) == 15 && gate_clock_ns() - t0 >= cap_ns) break;
        if (i < 20000) gate_wait::pause();
        else std::this_thread::yield();
    }
    gate_ns.fetch_add(gate_clock_ns() - t0, std::memory_order_relaxed);
    const int64_t all = gate_all.fetch_add(1, std::memory_order_relaxed) + 1;
    const int64_t all_outs = met ? gate_all_outs.load(std::memory_order_relaxed) : gate_all_outs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!met) gate_outs.fetch_add(1, std::memory_order_relaxed);
    if (!met && gate_wait::off_after(all, all_outs) && !gate_off.exchange(true))
        logf("vk: RX_VK_GATE_WAIT_MS: %lld of %lld waits for the CPU's signal ran out: off for this run (the signal comes after the "
             "submit returns)", (long long)all_outs, (long long)all);
    if (gate_n.fetch_add(1, std::memory_order_relaxed) + 1 == 600) {   // (the thread whose wait ends the window logs it)
        const int64_t wn = gate_n.exchange(0), wns = gate_ns.exchange(0), outs = gate_outs.exchange(0);
        logf("vk: RX_VK_GATE_WAIT_MS: %lld submissions waited for the CPU's signal before their capture, %.2f ms on average; %lld ran "
             "out and went as before", (long long)wn, wn ? (double)wns / (double)wn / 1e6 : 0.0, (long long)outs);
    }
}

std::mutex submit_order_mu;   // (captures and submits queued in the same order)
void submit_q(const uint64_t* handles, int n, uint64_t queue) {
    if (!async_on()) {
        gate_wait_signal(handles, n);   // ((Triforce) RX_VK_GATE_WAIT_MS, off unless set: before the order lock and the capture)
        std::lock_guard order(sync_order_mu);
        ReplayPin pin;   // (the capture and the replay on the replay's core)
        // (the replay here is on the submitting thread, but the game's other threads go on writing their next frames: a pool
        // synced from guest memory later in the frame read those. The capture is taken here too)
        // (EvilOOT) on by default: without it every mirror sync read guest memory as it was at the replay, the game's later
        // frames. OoT's GPU culling alternates two list sets: every other frame its setup read the next frame's records,
        // counted into the other set, and the foliage, grass, characters and particles of that frame were never drawn.
        // With it the syncs take the submits' captures, as the console's GPU sees its memory. RX_VK_SYNC_CAPTURE=0: off.
        static const bool capture = !getenv("RX_VK_SYNC_CAPTURE") || atoi(getenv("RX_VK_SYNC_CAPTURE")) != 0;
        // (lab: a delay on the presenting thread's first submit of a frame, or on another thread's (the game's main thread))
        lab_ab::delay(GetCurrentThreadId() == lab_ab::pres_thread.load(std::memory_order_relaxed) ? "submit_pres" : "submit_main");
        // (HOS_NVN_HOLD=2: a held submit, its game thread already running again: every page copied before the replay)
        // (RX_VK_HELD_BEHIND, default on; =0: every page copied before the replay. A held submit's copies left behind too, as a game
        // thread's own submit leaves them: the game's thread runs on either way, and the copy only ends later. Lab 11-20, 3+3 interleaved
        // runs at the owner's Market spot: 76.2 -> 77.1 fps, 1% low 57.2 -> 59.2, never below the other way in a round, the picture right)
        static const bool held_behind = !getenv("RX_VK_HELD_BEHIND") || atoi(getenv("RX_VK_HELD_BEHIND")) != 0;
        if (capture) memory_capture_submit(!tl_submit_held || held_behind);
        // ((Triforce) its copies left to the helpers while this thread replays (vk_memory.cpp BehindCopy) are all made before it
        // goes back to the game: what the game writes after its submit never reaches them. RX_VK_CAPTURE_BEHIND=0: none left)
        struct CopiesDone {
            ~CopiesDone() { memory_capture_copies_done(); }
        } copies_done;
        // (RX_VK_SUBMIT_DELAY_US=<us> (a test): a console's GPU starts later than this replay does. Does what a submission reads
        // arrive just after it, from the game's other threads?)
        static const int delay_us = getenv("RX_VK_SUBMIT_DELAY_US") ? atoi(getenv("RX_VK_SUBMIT_DELAY_US")) : 0;
        if (delay_us) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(delay_us);
            while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
        }
        if (sync_waits_on()) {
            static std::once_flag watch;
            std::call_once(watch, [] { std::thread(sync_watch).detach(); });
            std::vector<uint64_t> h(handles, handles + std::max(n, 0));
            const uint64_t value = ++q_values;
            if (queue) {
                q_last[queue] = value;
                value_queue[value] = queue;
                while (value_queue.size() > 8192) value_queue.erase(value_queue.begin());
            }
            submit_job(h, value, queue);
            update_blocked();
            return;
        }
        return do_submit(handles, n);
    }
    std::lock_guard lk(submit_order_mu);
    lab_ab::delay("submit_game");   // (lab: the game's thread at a hand-over to the replay thread)
    // (the pages as they are now, on the game's thread: vk_memory.cpp)
    // (Triforce, async_rules.h 2) Its copies shared with the copy helpers, as the submitting thread's replay has them
    // (BehindCopy), and all of them made before the hand-over: the game goes on writing once this returns, and the replay
    // thread takes a capture whose copies are whole. RX_VK_ASYNC_CAPTURE_BEHIND=0: every copy on this thread, as before
    static const bool behind = async_rules::on_unless_zero(getenv("RX_VK_ASYNC_CAPTURE_BEHIND"));
    if (behind) {
        memory_capture_submit(true);
        memory_capture_copies_done();
    } else {
        memory_capture_submit();
    }
    Job j{JOB_SUBMIT};
    j.handles.assign(handles, handles + std::max(n, 0));
    j.queue = queue;
    if (tl_submit_waits_told) {   // ((Triforce) the host's word for this submit: mkwvk_submit_waits, just before it)
        j.waits.swap(tl_submit_waits);
        j.waits_told = true;
        tl_submit_waits_told = false;
        tl_submit_waits.clear();
    }
    enqueue(std::move(j));
}
void submit(const uint64_t* handles, int n) {
    submit_q(handles, n, 0);
}
void present(const nvn_tex* t, const int crop[4]) {
    memory_cap_present();   // (RX_VK_CAP_ONLY_GPU: its counts every 10 s)
    if (!async_on()) {
        std::lock_guard order(sync_order_mu);
        ReplayPin pin;   // (the frame's last replays and the present on the replay's core)
        lab_ab::pres_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        lab_ab::delay("present");   // (lab: the presenting thread, once a frame, under the replay's lock)
        if (sync_waits_on()) {
            if (vk_ft_on()) vk_ft("pdrain>");
            drain(0);   // (the frame's work whole before it is shown)
            if (vk_ft_on()) vk_ft("pdrain<");
            update_blocked();
            present_value = ++q_values;
            return do_present(t, crop);
        }
        return do_present(t, crop);
    }
    Job j{JOB_PRESENT};
    if (t) j.tex = *t, j.has_tex = true;
    if (crop) std::memcpy(j.crop, crop, sizeof j.crop), j.has_crop = true;
    enqueue(std::move(j));
}
void texture_released_of(const nvn_tex* t) {
    if (!t) return;
    tex_pages_add(t, -1);
    if (!async_on()) {
        // (PR 430: after the stopped work that may use it, when it goes on. The lock is held_mu, only while queues are
        // stopped: sync_order_mu, taken on every release, made the thread finalizing textures (OptimizeChangeBank) wait for whole
        // submissions being replayed under it, 45% of its time in Kokiri. A release just after the stopped work went on waits
        // for the next update_blocked: the next submit or present)
        if (hold_releases() && sync_waits_on() && blocked_n.load()) {
            {
                std::lock_guard hl(held_mu);
                held_tex_releases.push_back(*t);
            }
            if (++st_held_releases % 2000 == 1) logf("vk: releases kept for stopped work: %lld", (long long)st_held_releases.load());
            return;
        }
        sync_drain(1);   // (after the stopped work that may use it)
        return do_texture_released(t);
    }
    Job j{JOB_TEXTURE_RELEASED};
    j.tex = *t, j.has_tex = true;
    enqueue(std::move(j));
}
void program_released_locked(const nvn_prog* p) {
    if (!p) return;
    if (!async_on()) {
        if (hold_releases() && sync_waits_on() && blocked_n.load()) {   // (PR 430: as texture releases, on held_mu)
            {
                std::lock_guard hl(held_mu);
                held_prog_releases.push_back(*p);
            }
            st_held_releases++;
            return;
        }
        sync_drain(1);
        return do_program_released(p);
    }
    Job j{JOB_PROGRAM_RELEASED};
    j.prog = *p;
    enqueue(std::move(j));
}
// ---- texture and sampler registrations: in order with the submits ----
// A registration took effect at once, on the game's thread, while the replay recorded submits up to two presents
// older: the game, which registers the slots of its next frames as it builds them (sets of render targets at each
// dynamic-resolution step), had the frame being recorded sample the next ones' textures, of another size: its
// picture went to pieces below or above a line (edges clamped into stripes, the sky's or fog's colour, the shadows
// dark) for a frame, with the size changes (every few frames when the resolution swung). Now they are jobs, applied
// where the game made them among its submits, as finalized textures already were. RX_VK_REGISTER_AT_ONCE=1: as before.
bool register_at_once() {
    static const bool v = getenv("RX_VK_REGISTER_AT_ONCE") != nullptr;
    return v;
}
void register_texture_queued(int id, const nvn_tex* t, const nvn_view* v) {
    if (!async_on() || register_at_once()) return register_texture_locked(id, t, v);
    if (id < 0 || id >= (1 << 20) || !t) return;
    t = (const nvn_tex*)descriptor_ok(t, sizeof(nvn_tex));
    if (!t) return;
    v = (const nvn_view*)descriptor_ok(v, sizeof(nvn_view));
    Job j{JOB_REGISTER_TEXTURE};
    j.id = id, j.tex = *t;
    if (v) j.view = *v, j.has_view = true;
    enqueue(std::move(j));
}
void event_signal_queued(uint64_t gpu, uint32_t value) {
    if (!async_on()) {
        std::lock_guard lk(ev_cpu_mu);
        ev_cpu_pending.emplace_back(gpu, value);
        ev_cpu_any.store(true, std::memory_order_release);
        return;   // (sync_watch or the next submit lets the stopped ones on: not this thread, ~1.7 ms a frame of the critical path)
    }
    Job j{JOB_EVENT_SIGNAL};
    j.ev_gpu = gpu, j.ev_value = value;
    // (Triforce, 11-14) The game writes what a queue's work reads after it submitted that work, up to this signal (OoT's
    // presentation queue waits for its frame's data this way): the console's GPU reads memory after its wait. The submitting
    // thread's replay takes a capture when a stopped queue goes on from a CPU event (resume_queue, RX_VK_RESUME_CAPTURE); the
    // replay thread took none, and what went on from the wait read the frame before's fog and exposure: a frame's flash, 27
    // cell blinks in 6,600 frames at the Market against none. Here the capture is taken at the signal itself, on the game's
    // thread, where the data is as the game means it, and the replay thread takes it in at the signal's place among its
    // jobs. Only while the thread has work behind it: with none, the next submit's own capture has it all
    // (Triforce, async_rules.h 1) And only when a submission handed over before this signal may still wait for this event: one
    // handed over after it has its own capture, taken after the game's writes up to here.
    // ((Triforce, 11-20) The look, the capture and the job's place in the queue are one step, under submit_order_mu, the lock a
    // hand-over takes for its capture and its job. A submission is noted only at its enqueue, after its capture and that capture's
    // copies: a signal that looked without the lock missed one between the two, took no capture, and could even be queued ahead of
    // it, so that the submission went past its wait on memory from before the writes the signal stands for (the frame-before's
    // data the capture at the signal is for). Lock order as submit_q's: submit_order_mu, then q_mu)
    static const bool capture = !getenv("RX_VK_RESUME_CAPTURE") || atoi(getenv("RX_VK_RESUME_CAPTURE")) != 0;
    if (capture && async_unforced()) {
        std::lock_guard order(submit_order_mu);
        const int64_t signals = st_signals.fetch_add(1, std::memory_order_relaxed) + 1;
        const bool behind = replay_behind();
        bool take = behind;
        if (behind && wait_notes_on()) {
            std::lock_guard nk(q_mu);
            take = async_rules::capture_at_signal(true, true, wait_notes, gpu);
        }
        if (behind && !take) st_signal_captures_left.fetch_add(1, std::memory_order_relaxed);
        if (signals % 20000 == 0)
            logf("vk: the replay thread: %lld signals of the CPU's so far: %lld took a capture on the game's thread, %lld left it out "
                 "(no submission handed over waits for the event: RX_VK_ASYNC_EVENT_NOTES)", (long long)signals,
                 (long long)st_signal_captures.load(std::memory_order_relaxed) + (take ? 1 : 0),
                 (long long)st_signal_captures_left.load(std::memory_order_relaxed));
        if (take) {
            st_signal_captures.fetch_add(1, std::memory_order_relaxed);
            j.ev_capture = memory_capture_submit();   // (captures and jobs in one order: submit_order_mu, held above)
        }
        enqueue(std::move(j));
        return;
    }
    enqueue(std::move(j));
}
void register_sampler_queued(int id, const nvn_samp_par* p) {
    if (!async_on() || register_at_once()) return register_sampler(id, p);
    if (id < 0 || id >= (1 << 12) || !p) return;
    p = (const nvn_samp_par*)descriptor_ok(p, sizeof(nvn_samp_par));
    if (!p) return;
    Job j{JOB_REGISTER_SAMPLER};
    j.id = id, j.samp = *p;
    enqueue(std::move(j));
}
uint64_t fence() { return async_on() || sync_waits_on() ? q_values.load() : pushed.load(); }
uint64_t get_pushed() { return fence(); }

// (a parallel replay's segments) each recording's init command pool and buffer: made with RX_VK_PARALLEL only (par_threads
// is the process's), last in replay_start, which runs once a process. All or none: a failure gives back what was made and
// the start fails, as with the recordings' other pools. Kept as long as the device, like those
void init_pools_destroy() {
    for (Recording& r : recordings) {
        if (r.init_pool) vkDestroyCommandPool(dev.device, r.init_pool, nullptr);   // (its command buffer with it)
        r.init_pool = VK_NULL_HANDLE, r.init = VK_NULL_HANDLE;
    }
}
bool init_pools_make(const VkCommandPoolCreateInfo& pi) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    for (Recording& r : recordings) {
        VkCommandPool pool = VK_NULL_HANDLE;   // (the outputs are kept only on success)
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (vkCreateCommandPool(dev.device, &pi, nullptr, &pool) != VK_SUCCESS) {
            init_pools_destroy();
            return false;
        }
        r.init_pool = ai.commandPool = pool;
        if (vkAllocateCommandBuffers(dev.device, &ai, &cb) != VK_SUCCESS) {
            init_pools_destroy();
            return false;
        }
        r.init = cb;
    }
    return true;
}

} // namespace

bool replay_start() {
    menu_init();
    if (!memory_init() || !vma_init(instance_proc())) return false;
    VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo tsi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    tsi.pNext = &ti;
    if (vkCreateSemaphore(dev.device, &tsi, nullptr, &timeline) != VK_SUCCESS) return false;
    if (vkCreateSemaphore(dev.device, &tsi, nullptr, &batch_tl) != VK_SUCCESS) batch_tl = VK_NULL_HANDLE;
    if (vkCreateSemaphore(dev.device, &tsi, nullptr, &rec_timeline) != VK_SUCCESS) rec_timeline = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = dev.queue_family;
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    int slot = 0;
    for (Recording& r : recordings) {
        r.slot = slot++;
        VkFenceCreateInfo rfi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev.device, &rfi, nullptr, &r.fence) != VK_SUCCESS) return false;
        if (vkCreateCommandPool(dev.device, &pi, nullptr, &r.pool) != VK_SUCCESS) return false;
        if (vkCreateCommandPool(dev.device, &pi, nullptr, &r.upload_pool) != VK_SUCCESS) return false;
        ai.commandPool = r.pool;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev.device, &ai, &r.cmd);
        ai.commandPool = r.upload_pool;
        vkAllocateCommandBuffers(dev.device, &ai, &r.upload);
    }
    ai.commandBufferCount = 1;
    for (Frame& f : frames) {
        if (vkCreateCommandPool(dev.device, &pi, nullptr, &f.pool) != VK_SUCCESS) return false;
        ai.commandPool = f.pool;
        vkAllocateCommandBuffers(dev.device, &ai, &f.cmd);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev.device, &fi, nullptr, &f.done);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(dev.device, &si, nullptr, &f.acquired);
    }
    if (par_threads_max() >= 2 && !init_pools_make(pi)) return false;   // (last: nothing after it fails and leaves them made)
    logf("vk: replay ready (%s; push descriptors %s (%u), extended dynamic state 3 %s, custom border colours %s)",
         dev.offscreen ? "offscreen" : "window", dev.push_descriptor ? "yes" : "no", dev.max_push_descriptors,
         dev.eds3_blend ? "yes" : "no", dev.custom_border_color ? "yes" : "no");
    return true;
}

/// (hookshot) The host's camera jitter (nx2/oot_jitter.py, host/rx_backend.c): the scene's size while a temporal upscaler
/// draws, NVIDIA DLSS or AMD FSR 4 (vk_temporal.cpp) or AMD FSR 3.1 (vk_fsr3_inputs.cpp), whichever it is (one at a time: the
/// Upscaler row); 0, no jitter, otherwise
int jitter_option(int id) {
    const int axis = id - HOS_OPT_DLSS_JITTER_W;
    const uint32_t d = temporal_jitter_size(axis);
    return (int)(d ? d : fsr3_inputs::jitter_size(axis));
}

void fill_replay_table(MkwVkRenderer& t) {
    t.stats = stats;
    t.nvn_submit = submit;
    t.nvn_submit_q = submit_q;
    t.nvn_fence_q = fence_q;
    t.nvn_fence = fence;
    t.nvn_pushed = get_pushed;
    t.nvn_replayed = get_replayed;
    t.nvn_wait = wait_counted;
    t.wait_writebacks_reading = wait_writebacks_reading;
    t.nvn_finish = finish;
    t.nvn_present = present;
    t.nvn_register_texture = register_texture_queued;
    t.nvn_event_signal = event_signal_queued;
    t.option = [](int id) -> int {   // (3: the fixed path's scale in 1/10000, vk_menu.cpp; DLAA: full scale, vk_dlss.cpp)
        if (id == 11) return opts.stick_dead_zone;
        if (id == HOS_OPT_LSTICK_ZONE) return std::clamp(opts.lstick_dead_zone, -1, 50) + 1;   // (0: none of its own)
        if (id == HOS_OPT_RSTICK_ZONE) return std::clamp(opts.rstick_dead_zone, -1, 50) + 1;
        if (id == HOS_OPT_RUMBLE_CURVE) return std::clamp(opts.rumble_curve, 20, 150);
        if (id == HOS_OPT_RUMBLE_TRIGGERS) return opts.rumble_triggers != 0;
        if (id == HOS_OPT_WALK_KEY) return opts.walk_key;
        if (id == HOS_OPT_WALK_SCALE) return std::clamp(opts.walk_scale, 20, 90);
        if (id == HOS_OPT_CHEAT_HEARTS) return menu_heart_protection();
        if (id == HOS_OPT_CHEAT_FLIGHT) return menu_player_flight();
        if (id == HOS_OPT_CHEAT_ACTION) return menu_take_cheat_action();
        if (id == HOS_OPT_CHEAT_CAMERA) return menu_free_camera();
        if (id == HOS_OPT_DEBUG_ASK) return menu_take_debug_ask();     // (Triforce: the Debug tab's Game flags and Cutscenes pages, host/hos_main.c)
        if (id == HOS_OPT_AMIIBO_ASK) return menu_take_amiibo_ask();   // (and its Scan amiibo page, host/nn_nfp.c)
        if (id == HOS_OPT_HDR_NATIVE) return dev.hdr && opts.hdr == 2;
        if (id == HOS_OPT_HDR_PEAK) return std::clamp(opts.hdr_peak, 400, 10000);
        if (id == HOS_OPT_HDR_WHITE) return std::clamp(opts.hdr_white, 80, 500);
        if (id == HOS_OPT_DISCORD) return opts.discord != 0;
        if (id == HOS_OPT_VOLUME) return std::clamp(opts.volume, 0, 100);
        if (id == HOS_OPT_MENU_SOUND) return menu_take_sound();   // (Triforce: the options' own sounds, host/menu_sound.c's thread)
        if (id == HOS_OPT_MENU_SOUND_VOLUME) return std::clamp(opts.menu_sounds, 0, 100);   // (and the Menu sounds row)
        if (id == HOS_OPT_SPRINT) return opts.sprint_button;
        if (id == HOS_OPT_SPRINT_HOLD) return opts.sprint_mode;
        if (id == HOS_OPT_DISPLAY_CAP) return fps_cap::of_display(display_refresh_mhz());   // (Monitor's rate: host/nn_vi.c fps_max)
        if (id == HOS_OPT_PACE_START) return pace::frame_start();   // (Triforce: the game's frame starts, host/nn_vi.c hos_vi_native_acquire)
        if (id == HOS_OPT_PACE_STEADY) return pace::steady_held_us();   // (Triforce: the present's hold for the Steady frame rate row)
        if (id == HOS_OPT_SHARE_ASK) return menu_take_share_ask();   // (Triforce: the System tab's save rows, host/save_share.c)
        if (id == HOS_OPT_BUG_SHOT) return menu_bug_shot();   // (and a bug capture's picture: host/bug_capture.c)
        if (id == HOS_OPT_MODCHEAT_ASK) return menu_take_mod_cheat();   // (Triforce: a mod's cheat switched on its row, host/mod_cheats.c)
        if (id == HOS_OPT_MODS_SELECTED) return menu_mods_selected();   // (Triforce: the Mods entry has the cursor, host/oot_menu.c)
        if (id == HOS_OPT_TEXT_WANTED) return menu_text_wanted();   // (Triforce: the Mods screen's search takes text, host/display.c)
        return id == 0 ? opts.realtime : id == 1 ? menu_is_open() : id == 2 ? resolution_auto() && !temporal_full_scale() : id == 3 ? (temporal_full_scale() ? 10000 : resolution_scale()) : id == 4 ? opts.pad_background : id == 5 ? opts.text_speed : id == 6 ? opts.mouse_native :
               id == 7 ? opts.mouse_sens : id == 8 ? opts.mouse_invert : id == 9 ? opts.rumble : id == 16 ? opts.invert_cam_x : id == 17 ? opts.invert_cam_y : id == HOS_OPT_GYRO ? opts.gyro : id == HOS_OPT_GYRO_SENS_X ? opts.gyro_sens_x : id == HOS_OPT_GYRO_SENS_Y ? opts.gyro_sens_y : id == HOS_OPT_GYRO_INV_X ? opts.gyro_inv_x : id == HOS_OPT_GYRO_INV_Y ? opts.gyro_inv_y : id == HOS_OPT_GYRO_DEADZONE ? opts.gyro_deadzone :
               id == HOS_OPT_DLSS_JITTER_W || id == HOS_OPT_DLSS_JITTER_H || id == HOS_OPT_DLSS_JITTER_PHASES ? jitter_option(id) : 0;
    };
    t.option_set = [](int id, int value) {
        if (id == HOS_OPT_EXITING) {   // ((Triforce, handoff §8) the files threads of their own are writing, finished first)
            const file_writers::Stopped s = file_writers::stop(file_writers::exit_wait_ms());
            if (s.left || s.waited_ms >= 1 || s.refused)
                logf("vk: exit: the files being written on threads of their own were waited for %.0f ms%s (%d not begun; "
                     "RX_VK_WRITERS_WAIT_MS)", s.waited_ms, s.left ? (", " + std::to_string(s.left) + " still under way: cut, "
                     "their old files stay").c_str() : "", s.refused);
        }
        if (id == HOS_OPT_EXITING) framegen_exiting();   // (Triforce: no call into AMD's frame generation library from here on)
        if (id == HOS_OPT_EXITING) pace::exiting();   // (Triforce: RivaTuner Statistics Server's profile gets back what it held)
        if (id == HOS_OPT_EXITING) return dlssg::shutdown();   // (the process is about to exit: host/hos_relaunch.c)
        if (id == HOS_OPT_PACE_PERIOD) return pace::set_period_ns(value);   // (Triforce: the period the host paces by, vk_pace.cpp)
        if (id == HOS_OPT_PACE_MOVIE) return pace::set_movie(value);        // (and whether a movie plays)
        if (id == HOS_OPT_PACE_STEADY) return pace::set_steady(value);      // ((Triforce) and whether it is the Steady frame rate row's)
        menu_option_set(id, value);
    };
    t.nvn_register_sampler = register_sampler_queued;
    t.nvn_set_texture_pool_size = set_texture_pool_size;
    t.nvn_texture_released = texture_released_of;
    t.nvn_program_released = program_released_locked;
    t.nvn_read_texels = read_texels;
    t.nvn_write_texels = write_texels;
    t.nvn_invalidate = invalidate;
    t.nvn_pool_created = memory_pool_created;
    t.guest_imported = memory_guest_imported;
    t.nvn_vmap_changing = vmap_changing;
    t.nvn_texture_created = texture_created_of;
}

std::atomic<bool> par_active{false};
std::atomic<int64_t> par_lock_wait_ns{0}, par_locks{0};
std::recursive_mutex& par_mutex() {
    return par_mu;
}
int64_t par_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int par_spin() {
    static const int n = std::max(0, getenv("RX_VK_PARLOCK_SPIN") ? atoi(getenv("RX_VK_PARLOCK_SPIN")) : 4000);
    return n;
}

} // namespace mkwvk

// (HOS_NVN_HOLD=2, host/nvn_native.c held_flush_why) the submits this thread replays next are held ones: the game's thread that made
// them is already running again, so their capture copies every page before the replay (no copies left behind it: capture_behind.h's
// rule is that nothing the game writes after its submit reaches them, which holds only while the submitting thread waits). Looked up
// by name (host/rx_backend.c rx_nvn_submit_held): an older executable never calls it
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void mkwvk_submit_held(int on) {
    mkwvk::tl_submit_held = on != 0;
}
// (Triforce, async_rules.h) host/rx_backend.c's rx_nvn_submit_waits and rx_nvn_replay_idle_wait, found by name (MkwVkHost and
// MkwVkRenderer stay as they are): the events the submit a thread is about to make waits for, and the present poll's wait for
// the replay thread. Both do nothing with the replay on the submitting thread
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void mkwvk_submit_waits(const uint64_t* events, int n) {
    mkwvk::submit_waits(events, n);
}
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int mkwvk_replay_idle_wait(uint64_t timeout_ns) {
    return mkwvk::replay_idle_wait(timeout_ns) ? 1 : 0;
}
