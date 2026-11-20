// (Triforce) What the replay's barrier rules ask of the command buffer they record into, kept per command buffer: how many
// commands went into it, and where its last full barrier, its last draw pass's end and its last dispatch were (vk_work.h counts
// the commands; vk_texture.cpp full_barrier, pass_start_barrier and pass_resume_barrier, vk_draw.cpp dispatch read and set the
// marks). Apart from Vulkan's calls so that tests/test_barrier_rules.py, test_pass_barrier.py and test_dispatch_rule_threads.py can
// run it.
//
// After NX2navi's per-context state (runtime/vk_exec.h VkExec.recorded, barrier_at, bar_dirty, bar_passes, bar_dispatches;
// vk_exec.c barrier_to_why, chunk_record, group_put): there a recording context counts its own commands, so its rules hold inside
// the worker threads' chunks. Here one count served every thread (work_epoch, an atomic add on every command from every recording
// thread), so each rule stood aside while the Replay helper threads recorded (most frames at the Market: about 500 full barriers a
// frame, no pass start in its fragment stages only). A command buffer is recorded by one thread at a time (Vulkan asks for that),
// and its state here is only touched by whoever records into it, so nothing in it needs to be atomic; the table that finds it is.
// Another thread's commands go into its own command buffers and leave this one's marks alone; a command buffer two threads take
// turns at (a parallel replay's segment init command buffer, vk_texture.cpp InitRedirect) has every turn's commands counted in it.
//
// RX_VK_BARRIER_CONTEXT=0: the one count of every thread's commands, and the rules standing aside while the helper threads record,
// as before (vk_texture.cpp rules_aside).
#pragma once

#include <vulkan/vulkan_core.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mkwvk {

extern std::atomic<uint64_t> work_epoch;   // (vk_texture.cpp: RX_VK_BARRIER_CONTEXT=0's count, every thread's commands)
inline thread_local uint64_t work_mine = 0;   // (this thread's commands, into any command buffer: work_between)
inline std::atomic<uint64_t> work_submits{0};   // (every queue submit through vk_work.h, any thread's: work_between)

/// RX_VK_BARRIER_CONTEXT=0: the global count, as before; anything else, or unset: per command buffer. Read at the first command
/// (0: not read yet, 1: per command buffer, 2: global)
inline std::atomic<int> work_way{0};
inline bool work_global() {
    int w = work_way.load(std::memory_order_relaxed);
    if (!w) [[unlikely]] {
        const char* e = std::getenv("RX_VK_BARRIER_CONTEXT");
        w = e && std::strcmp(e, "0") == 0 ? 2 : 1;
        work_way.store(w, std::memory_order_relaxed);
    }
    return w == 2;
}

/// One command buffer's state. The marks are counts of it (work_at) taken right after the command they mark; a mark equal to the
/// count now: nothing was recorded into it since
struct CmdWork {
    uint64_t n = 0;                      // (its commands, and its begins: work_begun)
    uint64_t barrier_at = UINT64_MAX;    // (right after its last full barrier: full_barrier leaves out a second one there)
    uint64_t pass_end_at = UINT64_MAX;   // (right after its last draw pass ended, unless the replay split that pass for a hazard)
    uint64_t dispatch_at = UINT64_MAX;   // (right after its last dispatch: RX_VK_DISPATCH_BARRIER)
    bool known = true;                   // (false: the stand-in for a command buffer the table had no room for, never right after anything)
};

/// The table: a slot a command buffer, taken at its first command or begin by whichever thread comes first and kept for good (the
/// replay makes its command buffers once: a few hundred at most). Full: cmd_work_none, which no rule relaxes on
constexpr size_t kCmdWorkSlots = 1024;
struct alignas(64) CmdWorkSlot {   // (a cache line each: two threads recording two command buffers share none)
    std::atomic<VkCommandBuffer> cmd{VK_NULL_HANDLE};
    CmdWork w;
};
inline CmdWorkSlot cmd_work_slots[kCmdWorkSlots];
inline thread_local CmdWork cmd_work_none{0, UINT64_MAX, UINT64_MAX, UINT64_MAX, false};   // (vk_texture.cpp full_barrier tells it once)
inline CmdWork* cmd_work_find(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE) return &cmd_work_none;
    const size_t h = (size_t)(((uint64_t)(uintptr_t)cmd * 0x9E3779B97F4A7C15ull) >> 54);   // (10 bits: kCmdWorkSlots)
    for (size_t k = 0; k < kCmdWorkSlots; k++) {
        CmdWorkSlot& s = cmd_work_slots[(h + k) % kCmdWorkSlots];
        VkCommandBuffer at = s.cmd.load(std::memory_order_acquire);
        if (at == VK_NULL_HANDLE && s.cmd.compare_exchange_strong(at, cmd, std::memory_order_acq_rel)) return &s.w;
        if (at == cmd) return &s.w;   // (its own, or taken by another thread just now)
    }
    return &cmd_work_none;
}
/// cmd's state (the last one this thread asked for is at hand: most commands go into the command buffer of the one before)
inline thread_local VkCommandBuffer cmd_work_last = VK_NULL_HANDLE;
inline thread_local CmdWork* cmd_work_last_w = nullptr;
inline CmdWork& cmd_work(VkCommandBuffer cmd) {
    if (cmd != cmd_work_last || !cmd_work_last_w) [[unlikely]] cmd_work_last_w = cmd_work_find(cmd), cmd_work_last = cmd;
    return *cmd_work_last_w;
}

/// The count a mark is taken in: the command buffer's own; RX_VK_BARRIER_CONTEXT=0, every thread's (work_epoch), as before
inline uint64_t work_at(const CmdWork& w) {
    return work_global() ? work_epoch.load(std::memory_order_relaxed) : w.n;
}
/// Whether nothing was recorded into the command buffer since the mark was taken
inline bool work_right_after(const CmdWork& w, uint64_t mark) {
    return w.known && mark == work_at(w);
}

/// A command that does or orders GPU work, into cmd (vk_work.h's macros; by hand after a library recorded into cmd itself)
inline void note_work(VkCommandBuffer cmd) {
    // (RX_VK_BARRIER_CONTEXT=0: an add, not a load and a store: a thread preempted between the two would store a stale count over
    // another thread's, which could make the count equal the one a barrier was recorded at with work recorded since)
    if (work_global()) work_epoch.fetch_add(1, std::memory_order_relaxed);
    else cmd_work(cmd).n++;
    work_mine++;
}
inline VkCommandBuffer work_on(VkCommandBuffer cmd) {
    note_work(cmd);
    return cmd;
}
/// cmd begun (vkBeginCommandBuffer): a new recording starts afresh, whatever its handle held before (its last command may have
/// been a full barrier, and the work submitted since then is not after it)
inline VkCommandBuffer work_begun(VkCommandBuffer cmd) {
    CmdWork& w = cmd_work(cmd);
    w.n++;
    w.barrier_at = w.pass_end_at = w.dispatch_at = UINT64_MAX;
    return cmd;
}
/// A submit to the queue (vkQueueSubmit2), any thread's
inline VkQueue work_submitted(VkQueue q) {
    work_submits.fetch_add(1, std::memory_order_relaxed);
    return q;
}
/// Whether anything was recorded or submitted between two points of this thread's recording: equal before and after, nothing.
/// What crosses command buffers asks this (vk_replay.cpp maybe_split: a pass going on in the next part; finish_recording: the
/// early tail's write-back): this thread's own commands, into any command buffer, and every submit to the queue, any thread's
/// (another thread's recording reaches the queue between two of this thread's command buffers only by a submit).
/// RX_VK_BARRIER_CONTEXT=0: every thread's commands, as before
inline uint64_t work_between() {
    return work_global() ? work_epoch.load(std::memory_order_relaxed) : work_mine + work_submits.load(std::memory_order_relaxed);
}

} // namespace mkwvk
