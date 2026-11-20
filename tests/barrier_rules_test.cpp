// (Triforce) tests/test_barrier_rules.py's model: the replay's barriers on recorded command streams (gpu/vk/barrier_rules.h).
//
// The production barrier functions of vk_texture.cpp (full_barrier and its repeats left out, the pass starts, the pass begun again
// after a part's split, the event writes held for a full barrier) are pasted in at FUNCTIONS by the test, with a recording
// vkCmdPipelineBarrier2. The model below replays a stream of the game's ops the way vk_replay.cpp and vk_draw.cpp do (maybe_split,
// NOP_SIGNAL_EVENT, NOP_WAIT_EVENT, NOP_BARRIER, the copies, dispatch, draw with draw_hazards and begin_pass, submit_recording), into
// command buffers in submission order. A checker then walks every pair of accesses to one resource of which one writes, and tells
// whether the recorded barriers order them (Vulkan's execution and memory dependencies, chained; attachments' accesses in one pass
// instance in the order drawn), against what the game's ordering asks for (barrier_rules.h's head):
//  R1 a game barrier between; R2 a draw and a dispatch; R3 a copy, update, upload or write-back (a transfer); R4 two draws'
//  attachment accesses; R5 in one of the game's passes, a draw that samples a target an earlier draw wrote, or writes one an earlier
//  draw sampled (draw_hazards); R6 the work before a signal before the event's write, and before the work after a wait for it; R7 a
//  later pass's fragment-side access after any earlier access (the pass barrier's game mode, pass_barrier.h); R8 two dispatches
//  (with RX_VK_DISPATCH_BARRIER=game only an indirect one's arguments).
// Not asked for, and so allowed to go unordered: in one of the game's passes, two draws' shader accesses (no barrier of the game's
// between: on the console too), and a later pass's vertex-stage access of an earlier pass's (the game mode's own).
//
//     barrier_rules_test <vendor hex> <part size: draws and dispatches, 0 whole> <case> [compare]
//
// case: a seed (random streams) or a named stream; compare: the stream also replayed whole (part size 0) and in parts the old way
// (RX_VK_SPLIT_BARRIER's rule off), and every pair both of those order must be ordered in this one too: a split that goes on in its
// pass orders all that the pass did in one part. (A split the old way starts its next part afresh, so what the whole stream orders
// only through draw_hazards' state is not kept by it either.) Exit 1 with the first hazard left open.
//
// ((Triforce, P4) The rules' counts and marks are each command buffer's own (gpu/vk/cmd_work.h, the real header): every command the
// model records is counted in its command buffer as vk_work.h counts it, a begin starts it afresh, and a recording's command buffers
// are taken from a few handles used again and again, as the replay's slots are. Another submitter's work between two parts is
// recorded on a thread of its own and submitted (work_submitted), as the present's or a read-back's is.
//
//     barrier_rules_test <vendor hex> threads <case> [shared]
//
// A parallel replay: the replay thread and a helper thread record a segment each at once (par_active), op by op in turn, each into
// its own command buffers, each segment begun and ended with a full barrier (vk_replay.cpp record_segment). Each thread's barriers
// must be the ones the same segment gets recorded alone (the rules act in each command buffer as they would alone; with
// RX_VK_BARRIER_CONTEXT=0 they stand aside instead, as before, and record more full barriers), no hazard is left open in either, nor
// across the two in queue order (no rule relaxes across a segment's edge). shared: the two threads take turns at one command buffer
// (as a segment's init command buffer is shared, vk_texture.cpp InitRedirect): each turn's commands count in it, so a thread's full
// barrier after the other's write is not left out for a repeat of its own one before.)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include "pass_barrier.h"
#include "barrier_rules.h"
#include "cmd_work.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace mkwvk {
std::string logged;
void logf(const char* fmt, ...) {
    char line[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    logged += line;
    logged += "\n";
}
std::atomic<bool> par_active{false};
struct { struct { uint32_t vendorID = 0; } props; } dev;
struct GprofSpan { GprofSpan(VkCommandBuffer, const char*) {} int items = 0; uint64_t bytes = 0; };
void full_barrier(VkCommandBuffer cmd, const char* why);
void event_writes_record(VkCommandBuffer cmd);   // (the model's, as vk_replay.cpp's)

// ---- what is recorded ----
enum : uint32_t { T_COLOR_R = 1, T_COLOR_W = 2, T_DEPTH_R = 4, T_DEPTH_W = 8, T_SHADER_R = 16, T_SHADER_W = 32, T_XFER_R = 64,
                  T_XFER_W = 128, T_IND_R = 256 };
constexpr uint32_t T_WRITES = T_COLOR_W | T_DEPTH_W | T_SHADER_W | T_XFER_W;
constexpr uint64_t S_V = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, S_F = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   S_E = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, S_L = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   S_C = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, S_K = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   S_X = VK_PIPELINE_STAGE_2_COPY_BIT, S_I = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
constexpr uint64_t S_ALL = S_V | S_F | S_E | S_L | S_C | S_K | S_X | S_I;
constexpr uint64_t S_FRAGMENT_SIDE = S_F | S_E | S_L | S_C;
uint64_t stages(uint64_t s) {
    uint64_t o = s & S_ALL;
    if (s & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) o |= S_ALL;
    if (s & VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT) o |= S_V | S_F | S_E | S_L | S_C | S_I;
    if (s & VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT) o |= S_X;
    if (s & (VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT)) o |= S_X;
    return o;
}
uint32_t types(VkAccessFlags2 a) {
    uint32_t t = 0;
    if (a & VK_ACCESS_2_MEMORY_READ_BIT) t |= T_COLOR_R | T_DEPTH_R | T_SHADER_R | T_XFER_R | T_IND_R;
    if (a & VK_ACCESS_2_MEMORY_WRITE_BIT) t |= T_WRITES;
    if (a & VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT) t |= T_COLOR_R;
    if (a & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) t |= T_COLOR_W;
    if (a & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT) t |= T_DEPTH_R;
    if (a & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) t |= T_DEPTH_W;
    if (a & (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
             VK_ACCESS_2_UNIFORM_READ_BIT))
        t |= T_SHADER_R;
    if (a & (VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) t |= T_SHADER_W;
    if (a & VK_ACCESS_2_TRANSFER_READ_BIT) t |= T_XFER_R;
    if (a & VK_ACCESS_2_TRANSFER_WRITE_BIT) t |= T_XFER_W;
    if (a & VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT) t |= T_IND_R;
    return t;
}
enum Kind { K_BARRIER, K_WORK, K_BEGIN, K_END };
struct Acc { int res = -1; uint64_t st = 0; uint32_t type = 0; int op = -1; int sub = 0; };
struct Rec { Kind k; uint64_t src = 0, dst = 0; uint32_t sa = 0, da = 0; Acc acc; };
std::map<VkCommandBuffer, std::vector<Rec>> lists;
void record_barrier(VkCommandBuffer cmd, const VkDependencyInfo* d) {
    CHECK(d->memoryBarrierCount == 1 && !d->bufferMemoryBarrierCount && !d->imageMemoryBarrierCount);
    const VkMemoryBarrier2& b = d->pMemoryBarriers[0];
    Rec r{K_BARRIER};
    r.src = stages(b.srcStageMask), r.dst = stages(b.dstStageMask), r.sa = types(b.srcAccessMask), r.da = types(b.dstAccessMask);
    lists[cmd].push_back(r);
}
// (as vk_work.h's: every barrier counts as work, in its command buffer: cmd_work.h)
#define vkCmdPipelineBarrier2(cmd, d) (note_work(cmd), record_barrier(cmd, d))
/* FUNCTIONS */
#undef vkCmdPipelineBarrier2

void rec(VkCommandBuffer cmd, Kind k, Acc a = {}) {
    note_work(cmd);   // (vkCmdBeginRendering, vkCmdEndRendering, draws, dispatches, copies, updates: vk_work.h's work)
    Rec r{k};
    r.acc = a;
    lists[cmd].push_back(r);
}

// ---- the game's ops ----
enum OpT { O_DRAW, O_DISPATCH, O_COPY, O_BARRIER, O_SIGNAL, O_WAIT, O_UB, O_WB, O_BETWEEN };
struct Op {
    OpT t = O_DRAW;
    int color = -1, depth = -1;     // (a draw's targets; -1: none)
    bool wc = false, wd = false;    // (it writes its colour, its depth)
    int sample = -1;                // (a rendered target or a texture it samples, in sample_st)
    uint64_t sample_st = S_F;
    int sread = -1, swrite = -1;    // (a storage buffer it reads, writes, in s_st)
    uint64_t s_st = S_F;
    int upload = -1;                // (a texture its draw uploads first: the part's upload command buffer)
    int a = -1, b = -1;             // (a copy's source and destination; a dispatch's read and write; the resource of the rest)
    bool indirect = false;          // (a dispatch's grid read from the arguments' buffer)
};
constexpr int R_ARGS = 60;

// ---- the replay, as vk_replay.cpp and vk_draw.cpp do it ----
struct Config { bool split_rule, event_rule, dispatch_game; int every; };
Config config() {
    Config c;
    const bool every_draw = std::getenv("RX_VK_BARRIER_EVERY_DRAW") != nullptr;
    c.split_rule = barrier_rules::split_in_pass(std::getenv("RX_VK_SPLIT_BARRIER"), dev.props.vendorID, every_draw);
    c.event_rule = barrier_rules::event_late(std::getenv("RX_VK_EVENT_BARRIER"),
                                             pass_barrier::game(std::getenv("RX_VK_PASS_BARRIER"), dev.props.vendorID, every_draw));
    c.dispatch_game = barrier_rules::dispatch_game(std::getenv("RX_VK_DISPATCH_BARRIER"));
    c.every = 0;
    return c;
}
uintptr_t next_handle = 0x1000;
VkCommandBuffer new_cmd() { return (VkCommandBuffer)(next_handle += 16); }
// ((Triforce, P4) a Replay's recordings take their command buffers from so many pairs, used again in turn as the replay's slots are:
// a handle's state must start afresh at its begin)
int g_slots = 2;
constexpr int kHeld = 16;   // (vk_replay.cpp kHeldWrites)
/// (Triforce, P4) Two threads in turn, one op each (threads mode): a thread waits for its turn, and gives it on after its op
struct Turns {
    std::mutex mu;
    std::condition_variable cv;
    int turn = 0;
    bool done[2] = {false, false};
    void wait(int me) {
        std::unique_lock l(mu);
        cv.wait(l, [&] { return turn == me || done[1 - me]; });
    }
    void pass(int me) {
        {
            std::lock_guard l(mu);
            turn = 1 - me;
        }
        cv.notify_all();
    }
    void finish(int me) {
        {
            std::lock_guard l(mu);
            done[me] = true, turn = 1 - me;
        }
        cv.notify_all();
    }
};
struct Replay {
    Config c;
    const std::vector<Op>* ops = nullptr;
    int op = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE, up = VK_NULL_HANDLE;
    bool up_used = false;
    std::vector<std::vector<Rec>> order;   // ((Triforce, P4) what each command buffer held as it went to the queue, in that order)
    std::vector<std::pair<VkCommandBuffer, VkCommandBuffer>> slots;
    size_t slot = 0;
    void queue(VkCommandBuffer cb) {
        order.push_back(std::move(lists[cb]));
        lists.erase(cb);
    }
    // (the open pass and draw_hazards' state: colour bit 0, depth bit 8)
    bool open = false;
    int pc = -2, pd = -2;
    uint16_t written = 0, sampled_targets = 0;
    // (pass_carry_take / pass_carry_resume)
    bool carry_on = false, resuming = false;
    int cc = -2, cd = -2;
    uint16_t cw = 0, cs = 0;
    bool work_since_barrier = false;
    std::vector<int> ub_pending, wb_pending, between_pending;
    int part_work = 0;
    std::vector<std::pair<int, int>> held;   // (the held writes: event resource, its signal's op)
    int64_t splits = 0, carried = 0;

    void begin_recording() {   // (vk_replay.cpp begin_recording: vkBeginCommandBuffer, texture_begin_recording, draw_begin_recording)
        if (slots.empty())
            for (int k = 0; k < g_slots; k++) slots.push_back({new_cmd(), new_cmd()});
        cmd = slots[slot % slots.size()].first, up = slots[slot % slots.size()].second, slot++;
        lists[cmd].clear(), lists[up].clear();
        work_begun(cmd), work_begun(up);   // ((Triforce, P4) vk_work.h's vkBeginCommandBuffer: afresh, whatever the handle held)
        up_used = false, uploaded.clear();
        open = false, pc = pd = -2, written = sampled_targets = 0;   // (draw_begin_recording: pass() = Pass{})
        work_since_barrier = false;
    }
    void submit_recording() {
        if (event_writes_held(cmd)) end_pass(), full_barrier(cmd, "barrier (an event's held write, at its recording's end)");
        if (up_used) full_barrier(up, "upload end");   // (vk_texture.cpp texture_uploads_recorded)
        if (up_used) queue(up);
        else lists.erase(up);
        queue(cmd);
    }
    void end_pass() {   // (vk_draw.cpp end_pass)
        if (!open) return;
        rec(cmd, K_END);
        open = false;
        note_pass_end(cmd);
    }
    void begin_pass(int color, int depth) {   // (vk_draw.cpp begin_pass)
        if (open && pc == color && pd == depth) return;
        end_pass();
        if (resuming) pass_resume_barrier(cmd);
        else pass_start_barrier(cmd);
        work_since_barrier = true;
        rec(cmd, K_BEGIN);
        open = true, pc = color, pd = depth, written = sampled_targets = 0;
    }
    std::set<int> uploaded;   // (a texture is uploaded once a recording: its pages are clean after)
    void upload(int res) {   // (vk_texture.cpp upload_cmd: a full barrier at its first use)
        if (!uploaded.insert(res).second) return;
        if (!up_used) up_used = true, full_barrier(up, "upload start");
        rec(up, K_WORK, {res, S_X, T_XFER_W, op, 9});
    }
    void draw(const Op& o) {
        if (o.upload >= 0) upload(o.upload);   // (build_descriptors, before the pass)
        const uint16_t writes = (o.color >= 0 && o.wc ? 1 : 0) | (o.depth >= 0 && o.wd ? 0x100 : 0);
        // (draw_hazards)
        if (open && pc == o.color && pd == o.depth) {
            const int k = o.sample >= 0 && o.sample == o.color ? 0 : o.sample >= 0 && o.sample == o.depth ? 8 : -1;
            const bool samples_written = k >= 0 && (written >> k & 1) && !(writes >> k & 1);
            if (samples_written || (writes & sampled_targets)) end_pass(), note_pass_split(cmd);
        }
        begin_pass(o.color, o.depth);
        int sub = 0;
        if (o.color >= 0 && o.wc) rec(cmd, K_WORK, {o.color, S_C, T_COLOR_W, op, sub++});
        if (o.depth >= 0) rec(cmd, K_WORK, {o.depth, S_E | S_L, o.wd ? T_DEPTH_W : T_DEPTH_R, op, sub++});
        if (o.sample >= 0) rec(cmd, K_WORK, {o.sample, o.sample_st, T_SHADER_R, op, sub++});
        if (o.sread >= 0) rec(cmd, K_WORK, {o.sread, o.s_st, T_SHADER_R, op, sub++});
        if (o.swrite >= 0) rec(cmd, K_WORK, {o.swrite, o.s_st, T_SHADER_W, op, sub++});
        if (o.sample >= 0 && o.sample == o.color) sampled_targets |= 1;
        if (o.sample >= 0 && o.sample == o.depth) sampled_targets |= 0x100;
        written |= writes;
        maybe_split();
    }
    void dispatch(const Op& o) {   // (vk_draw.cpp dispatch)
        end_pass();
        CmdWork& wk = cmd_work(cmd);   // ((Triforce, P4) the command buffer's own mark of its last dispatch, as vk_draw.cpp's)
        const bool right_after = c.dispatch_game && work_right_after(wk, wk.dispatch_at);
        if (barrier_rules::dispatch_barrier(work_since_barrier, false, c.dispatch_game, right_after, o.indirect))
            full_barrier(cmd, "barrier (dispatch)");
        work_since_barrier = true;
        int sub = 0;
        if (o.a >= 0) rec(cmd, K_WORK, {o.a, S_K, T_SHADER_R, op, sub++});
        if (o.b >= 0) rec(cmd, K_WORK, {o.b, S_K, T_SHADER_W, op, sub++});
        if (o.indirect) rec(cmd, K_WORK, {R_ARGS, S_I, T_IND_R, op, sub++});
        wk.dispatch_at = work_at(wk);
        maybe_split();
    }
    void copy(const Op& o) {   // (vk_replay.cpp NOP_COPY_B2B)
        end_pass();
        full_barrier(cmd, "copy");
        rec(cmd, K_WORK, {o.a, S_X, T_XFER_R, op, 0});
        rec(cmd, K_WORK, {o.b, S_X, T_XFER_W, op, 1});
        full_barrier(cmd, "copy");
    }
    int held_at(int res) const {
        for (size_t i = 0; i < held.size(); i++)
            if (held[i].first == res) return (int)i;
        return -1;
    }
    std::vector<std::pair<int, int>> ev_bound;   // (the events' pages bound in this recording, and their signals: its write-backs)
    std::set<int> ev_written;                    // (the signals whose words are written so far)
    void signal(const Op& o) {   // (vk_replay.cpp NOP_SIGNAL_EVENT)
        ev_bound.push_back({o.a, op});   // (gpu_buffer_bind at the signal: its page among the recording's write-backs)
        if ((held.empty() || event_writes_held(cmd)) &&
            barrier_rules::event_hold(c.event_rule, par_active.load(std::memory_order_relaxed), (int)held.size(), kHeld)) {
            held.push_back({o.a, op});
            event_writes_hold(cmd);
        } else {
            end_pass();
            full_barrier(cmd, "event signal");
            rec(cmd, K_WORK, {o.a, S_X, T_XFER_W, op, 0});
            ev_written.insert(op);
        }
    }
    void wait(const Op& o) {   // (vk_replay.cpp NOP_WAIT_EVENT, met)
#ifndef MUT_NO_WAIT_BARRIER
        if (barrier_rules::wait_barrier(!held.empty() && held_at(o.a) >= 0 && event_writes_held(cmd))) {
            end_pass();
            full_barrier(cmd, "barrier (a wait whose signal's write was held)");
        }
#else
        (void)o;
#endif
    }
    void flush_ub_updates() {   // (vk_replay.cpp flush_ub_updates)
        if (ub_pending.empty()) return;
        full_barrier(cmd, "ubupd");
        for (int k : ub_pending) rec(cmd, K_WORK, {(*ops)[k].a, S_X, T_XFER_W, k, 0});
        full_barrier(cmd, "ubupd");
        ub_pending.clear();
    }
    void end_writebacks() {   // (vk_texture.cpp texture_end_recording: a full barrier before its copies, none after)
        if (!wb_pending.empty()) {
            full_barrier(cmd, "bake write-back: before the copies");
            for (int k : wb_pending) rec(cmd, K_WORK, {(*ops)[k].a, S_X, T_XFER_R, k, 0});
            wb_pending.clear();
        }
        // (vk_memory.cpp memory_end_submission: the pages bound written, the events' among them, copied to the game's memory behind
        // its transfer_barrier, a full one)
        if (ev_bound.empty()) return;
        full_barrier(cmd, "barrier (write-back or sync)");
        for (const auto& [res, s] : ev_bound) {
            if (!ev_written.count(s)) {
                std::fprintf(stderr, "FAIL (write-back): the write-back of op %d's event page comes before the write of its word\n", s);
                std::exit(1);
            }
            rec(cmd, K_WORK, {res, S_X, T_XFER_R, s, 7});
        }
        ev_bound.clear();
    }
    void maybe_split() {   // (vk_replay.cpp maybe_split)
        if (c.every <= 0 || ++part_work < c.every) return;
        splits++;
        const bool open_ = c.split_rule && take();
        end_pass();
        const bool writes_held = event_writes_held(cmd);
#ifndef MUT_NO_END_FLUSH
        event_writes_flush(cmd, "barrier (events' held writes, before the recording's end)");
#endif
        const uint64_t ended = work_between();   // ((Triforce, P4) cmd_work.h, as vk_replay.cpp's)
        flush_ub_updates();
        end_writebacks();   // (texture_end_recording, memory_end_submission)
        const bool quiet = work_between() == ended;
        const bool carry = barrier_rules::split_carry(c.split_rule, open_, quiet, writes_held);
        if (!carry) full_barrier(cmd, "barrier (recording end)");
        submit_recording();
        const uint64_t sent = work_between();
        for (int k : between_pending) {   // (another submitter's work between the two parts on the queue: behind a full barrier of its
            const VkCommandBuffer other = new_cmd();   // own, as every other submit's (vk_memory.cpp, vk_texture.cpp's read-backs, the
            const int res = (*ops)[k].a;                  // present), with none after it; (P4) recorded on a thread of its own and
            std::thread([other, res, k] {                 // submitted, as theirs are: not this thread's commands)
                work_begun(other);
                full_barrier(other, "another submitter");
                rec(other, K_WORK, {res, S_X, T_XFER_W, k, 0});
                work_submitted(VK_NULL_HANDLE);
            }).join();
            queue(other);
        }
        between_pending.clear();
        begin_recording();
        if (barrier_rules::split_start_full(carry, work_between() != sent)) full_barrier(cmd, "barrier (recording start)");
        part_work = 0;
        resume(carry);
        carried += carry;
    }
    bool take() {   // (vk_draw.cpp pass_carry_take)
        carry_on = open;
        if (carry_on) cc = pc, cd = pd, cw = written, cs = sampled_targets;
        return carry_on;
    }
    void resume(bool go) {   // (vk_draw.cpp pass_carry_resume)
        if (!carry_on) return;
        carry_on = false;
        if (!go) return;
        resuming = true;
        begin_pass(cc, cd);
        resuming = false;
#ifndef MUT_NO_CARRY_STATE
        written = cw, sampled_targets = cs;
#endif
    }
    void step(const Op& o) {
        switch (o.t) {
        case O_DRAW: draw(o); break;
        case O_DISPATCH: dispatch(o); break;
        case O_COPY: copy(o); break;
        case O_BARRIER: end_pass(), full_barrier(cmd, "barrier (the game's nvnCommandBufferBarrier)"); break;
        case O_SIGNAL: signal(o); break;
        case O_WAIT: wait(o); break;
        case O_UB: ub_pending.push_back(op); break;
        case O_WB: wb_pending.push_back(op); break;
        case O_BETWEEN: between_pending.push_back(op); break;
        }
    }
    /// ((Triforce, P4) vk_replay.cpp record_segment, run_job: a parallel replay's segment, its own command buffers begun afresh, a full
    /// barrier first and last, never split; with turns, one op at a time in turn with another thread's segment)
    void run_segment(const std::vector<Op>& stream, Turns* turns = nullptr, int me = 0) {
        ops = &stream;
        if (turns) turns->wait(me);
        begin_recording();
#ifndef MUT_NO_SEGMENT_EDGES
        full_barrier(cmd, "segment start");   // (after what the command buffers before it did)
#endif
        for (op = 0; op < (int)stream.size(); op++) {
            step(stream[op]);
            if (turns) turns->pass(me), turns->wait(me);
        }
        end_pass();
        flush_ub_updates();
        end_writebacks();
#ifndef MUT_NO_SEGMENT_EDGES
        full_barrier(cmd, "segment end");
#endif
        submit_recording();
        if (turns) turns->finish(me);
    }
    void run(const std::vector<Op>& stream) {
        ops = &stream;
        begin_recording();
        full_barrier(cmd, "barrier (recording start)");   // (run_submit)
        for (op = 0; op < (int)stream.size(); op++) step(stream[op]);
        // (finish_recording)
        end_pass();
#ifndef MUT_NO_END_FLUSH
        event_writes_flush(cmd, "barrier (events' held writes, before the recording's end)");
#endif
        flush_ub_updates();
        end_writebacks();
        full_barrier(cmd, "barrier (recording end)");
        submit_recording();
        CHECK(held.empty() && !event_writes_held(VK_NULL_HANDLE));   // (every signal's word written)
    }
};
void event_writes_record_into(Replay& r, VkCommandBuffer cmd) {   // (vk_replay.cpp event_writes_record)
    for (size_t i = 0; i < r.held.size(); i++) {
        if (i) {   // (one after another, as the signals were)
#ifndef MUT_EVENT_WRITES_UNORDERED
            VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1, d.pMemoryBarriers = &b;
            note_work(cmd);
            record_barrier(cmd, &d);
#endif
        }
        rec(cmd, K_WORK, {r.held[i].first, S_X, T_XFER_W, r.held[i].second, 0});
        r.ev_written.insert(r.held[i].second);
    }
    r.held.clear();
}
thread_local Replay* current = nullptr;   // ((Triforce, P4) each thread's own: threads mode)
void event_writes_record(VkCommandBuffer cmd) {
    if (current) event_writes_record_into(*current, cmd);
}

// ---- the checker ----
struct Flat { Rec r; int pass = -1; };
struct Result {
    std::vector<Flat> flat;
    std::set<std::tuple<int, int, int, int>> covered;   // (op, sub, op, sub) of the ordered pairs, earlier first by op
    int64_t full = 0, partial = 0;
};
bool conflict(const Acc& a, const Acc& b) {
    return a.res == b.res && ((a.type | b.type) & T_WRITES);
}
bool attachment(uint32_t t) { return (t & (T_COLOR_R | T_COLOR_W | T_DEPTH_R | T_DEPTH_W)) != 0; }
bool same_attachment(uint32_t a, uint32_t b) {
    return ((a & (T_COLOR_R | T_COLOR_W)) && (b & (T_COLOR_R | T_COLOR_W))) || ((a & (T_DEPTH_R | T_DEPTH_W)) && (b & (T_DEPTH_R | T_DEPTH_W)));
}
struct Semantics {
    const std::vector<Op>& ops;
    bool dispatch_game;
    std::vector<int> barriers_before;   // (the game's barriers among the ops before each)
    std::vector<int> pass_of;           // (the game's pass of each draw: draws with the same targets, nothing but signals, waits,
                                        // updates and write-backs between)
    std::vector<std::pair<int, int>> sig_wait;   // (a signal and a wait for its event after it)
    explicit Semantics(const std::vector<Op>& o, bool dg) : ops(o), dispatch_game(dg) {
        int n = 0, pass = 0, pc = -2, pd = -2;
        bool any = false;
        std::map<int, int> last_signal;
        for (size_t i = 0; i < ops.size(); i++) {
            barriers_before.push_back(n);
            const Op& x = ops[i];
            if (x.t == O_BARRIER) n++;
            if (x.t == O_DRAW) {
                if (!any || x.color != pc || x.depth != pd) pass++, any = true, pc = x.color, pd = x.depth;
            } else if (x.t == O_DISPATCH || x.t == O_COPY || x.t == O_BARRIER) {
                any = false;
            }
            pass_of.push_back(x.t == O_DRAW ? pass : -1);
            if (x.t == O_SIGNAL) last_signal[x.a] = (int)i;
            if (x.t == O_WAIT && last_signal.count(x.a)) sig_wait.push_back({last_signal[x.a], (int)i});
        }
        barriers_before.push_back(n);
    }
    bool barrier_between(int a, int b) const {
        const int lo = std::min(a, b), hi = std::max(a, b);
        return barriers_before[hi] - barriers_before[lo + 1] > 0;
    }
    /// Whether the game's ordering asks for i (executed first) before j: R1 to R8 above
    bool asked(const Acc& i, const Acc& j) const {
        const Op& oi = ops[i.op];
        const Op& oj = ops[j.op];
        // (one command's own accesses: a draw that samples what it writes itself is left as it is, draw_hazards. Not a signal's: its
        // word's write and the write-back's copy of its page are two commands)
        if (i.op == j.op && oi.t != O_SIGNAL) return false;
        // (a recording's uniform updates go into guest memory one after another with no barrier between, as they always did
        // (vk_replay.cpp flush_ub_updates): two of one buffer are left to the transfers' own order there, by the old way too)
        if (oi.t == O_UB && oj.t == O_UB) return false;
        if (barrier_between(i.op, j.op)) return true;                                                     // R1
        if ((i.type | j.type) & (T_XFER_R | T_XFER_W)) return true;                                       // R3
        for (const auto& [s, w] : sig_wait)
            if (std::min(i.op, j.op) < s && std::max(i.op, j.op) > w) return true;                       // R6
        if (oi.t != oj.t) return true;                                                                    // R2
        // (R8; with RX_VK_DISPATCH_BARRIER=game an indirect dispatch's grid after what wrote it: the front end reads a grid as it
        // starts its dispatch, in order, so a later dispatch's write of it is left to the game, as the rest)
        if (oi.t == O_DISPATCH) return !dispatch_game || (j.type & T_IND_R);
        if (attachment(i.type) && attachment(j.type) && same_attachment(i.type, j.type)) return true;    // R4
        if (pass_of[i.op] == pass_of[j.op]) {                                                             // R5
            const bool i_first = i.op < j.op;
            const Acc& e = i_first ? i : j;   // (the earlier draw's access, in the stream's order)
            const Acc& l = i_first ? j : i;
            const Op& ol = ops[l.op];
            const bool l_writes_it = (ol.color == l.res && ol.wc) || (ol.depth == l.res && ol.wd);
            if ((e.type & (T_COLOR_W | T_DEPTH_W)) && (l.type & T_SHADER_R) && !l_writes_it) return true;
            if ((e.type & T_SHADER_R) && (l.type & (T_COLOR_W | T_DEPTH_W))) return true;
            return false;
        }
        const Acc& l = i.op < j.op ? j : i;
        return (l.st & ~S_FRAGMENT_SIDE) == 0;                                                            // R7
    }
};
Result flatten(const Replay& r) {
    Result out;
    int pass = 0, cur = -1;
    for (const std::vector<Rec>& cb : r.order) {
        for (const Rec& x : cb) {
            if (x.k == K_BEGIN) cur = ++pass;
            out.flat.push_back({x, x.k == K_WORK ? cur : -1});
            if (x.k == K_END) cur = -1;
            if (x.k == K_BARRIER) {
                if (x.src == S_ALL && x.dst == S_ALL) out.full++;
                else out.partial++;
            }
        }
        cur = -1;   // (a pass does not go on past its command buffer)
    }
    return out;
}
/// Walks every pair; fails at the first the game's ordering asks for that the barriers leave open. Fills covered
void check(Result& res, const Semantics& sem, const char* what) {
    const auto& f = res.flat;
    for (size_t a = 0; a < f.size(); a++) {
        if (f[a].r.k != K_WORK) continue;
        const Acc& i = f[a].r.acc;
        const bool iw = (i.type & T_WRITES) != 0;
        uint64_t exec = 0;   // (the stages ordered after i so far)
        bool avail = false;
        std::map<uint64_t, uint32_t> vis;   // (stage -> the access types i's write is visible to there)
        for (size_t b = a + 1; b < f.size(); b++) {
            const Rec& x = f[b].r;
            if (x.k == K_BARRIER) {
                const bool first = (i.st & ~x.src) == 0;   // (i in its first scope: every stage it ran in)
                if (!first && !(exec & x.src)) continue;   // (not in the chain)
                if (first && iw && (x.sa & i.type)) avail = true;
                exec |= x.dst;
                if (avail)
                    for (uint64_t s = 1; s; s <<= 1)
                        if (x.dst & s) vis[s] |= x.da;
                continue;
            }
            if (x.k != K_WORK) continue;
            const Acc& j = x.acc;
            // (R6: the work before a signal before its event's write, whatever it touched: an execution dependency. Not the
            // write-backs' reads of the events' pages, which are a recording's end's)
            const bool its_write = sem.ops[j.op].t == O_SIGNAL && j.res == sem.ops[j.op].a && (j.type & T_XFER_W);
            if (!conflict(i, j) && !its_write) continue;
            const bool event_write = its_write && i.op < j.op && !conflict(i, j);
            if (!event_write && !conflict(i, j)) continue;
            bool ordered = (j.st & ~exec) == 0;
            if (ordered && iw && !event_write) {
                for (uint64_t s = 1; s; s <<= 1)
                    if ((j.st & s) && !(vis[s] & j.type)) ordered = false;
            }
            if (!ordered && !event_write && f[a].pass >= 0 && f[a].pass == f[b].pass && attachment(i.type) && attachment(j.type) &&
                same_attachment(i.type, j.type))
                ordered = true;   // (one pass instance: the attachments' accesses in the order drawn)
            if (event_write) {
                if (!ordered) {
                    std::fprintf(stderr, "FAIL (%s): op %d's access %d (resource %d, type %#x, at %zu) is not ordered before the write of "
                                         "op %d's signal (at %zu)\n", what, i.op, i.sub, i.res, i.type, a, j.op, b);
                    for (size_t q = a; q <= b; q++)
                        if (f[q].r.k == K_BARRIER)
                            std::fprintf(stderr, "  %zu: barrier %#llx -> %#llx\n", q, (unsigned long long)f[q].r.src,
                                         (unsigned long long)f[q].r.dst);
                        else if (f[q].r.k == K_WORK)
                            std::fprintf(stderr, "  %zu: op %d access %d resource %d type %#x\n", q, f[q].r.acc.op, f[q].r.acc.sub,
                                         f[q].r.acc.res, f[q].r.acc.type);
                    std::exit(1);
                }
                continue;
            }
            if (ordered) {
                if (std::make_pair(i.op, i.sub) < std::make_pair(j.op, j.sub)) res.covered.insert({i.op, i.sub, j.op, j.sub});
                else res.covered.insert({j.op, j.sub, i.op, i.sub});
            } else if (sem.asked(i, j)) {
                std::fprintf(stderr, "FAIL (%s): op %d (kind %d) access %d (resource %d, type %#x, stages %#llx) and op %d (kind %d) "
                                     "access %d (type %#x, stages %#llx): asked for, not ordered\n",
                             what, i.op, (int)sem.ops[i.op].t, i.sub, i.res, i.type, (unsigned long long)i.st, j.op,
                             (int)sem.ops[j.op].t, j.sub, j.type, (unsigned long long)j.st);
                std::exit(1);
            }
        }
    }
}

// ---- the streams ----
uint64_t rng = 1;
uint32_t rnd(uint32_t n) {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(rng >> 33) % n;
}
std::vector<Op> random_stream(uint64_t seed, int n) {
    rng = seed * 2654435761ull + 7;
    std::vector<Op> s;
    const int colors[] = {0, 1, 2, -1}, depths[] = {10, 11, -1, 10};
    int pass = 0;
    std::vector<int> open_signals;
    for (int k = 0; k < n; k++) {
        Op o;
        const uint32_t r = rnd(100);
        if (r < 62) {
            if (rnd(100) < 6) pass = (int)rnd(4);   // (another pass: other targets)
            o.t = O_DRAW;
            o.color = colors[pass], o.depth = depths[pass];
            o.wc = o.color >= 0 && rnd(100) < 85, o.wd = o.depth >= 0 && rnd(100) < 60;
            if (rnd(100) < 12) {   // (a rendered target: this pass's own sometimes)
                const int t = (int)rnd(5);
                o.sample = t < 3 ? t : t == 3 ? 10 : 11;
                o.sample_st = rnd(2) ? S_V : S_F;
            } else if (rnd(100) < 10) {
                o.sample = 30 + (int)rnd(3), o.sample_st = S_F;
            }
            if (rnd(100) < 15) o.sread = 20 + (int)rnd(3);
            if (rnd(100) < 8) o.swrite = 20 + (int)rnd(3);
            o.s_st = rnd(3) ? S_F : S_V;
            if (rnd(100) < 4) o.upload = 30 + (int)rnd(3);
        } else if (r < 74) {
            o.t = O_DISPATCH;
            o.a = rnd(2) ? 20 + (int)rnd(3) : (int)rnd(3);
            o.b = rnd(4) ? 20 + (int)rnd(3) : R_ARGS;
            o.indirect = rnd(100) < 20;
        } else if (r < 78) {
            o.t = O_COPY, o.a = 20 + (int)rnd(3), o.b = 20 + (int)rnd(3);
            if (o.a == o.b) o.b = 23;
        } else if (r < 84) {
            o.t = O_BARRIER;
        } else if (r < 90) {
            o.t = O_SIGNAL, o.a = 50 + (int)rnd(3);
            open_signals.push_back(o.a);
        } else if (r < 94) {
            if (open_signals.empty()) continue;
            o.t = O_WAIT, o.a = open_signals[rnd((uint32_t)open_signals.size())];
        } else if (r < 97) {
            o.t = O_UB, o.a = 40 + (int)rnd(2);
        } else if (r < 99) {
            o.t = O_WB, o.a = (int)rnd(3);
        } else {
            o.t = O_BETWEEN, o.a = 20 + (int)rnd(3);
        }
        s.push_back(o);
        if (o.t == O_UB && rnd(2)) {   // (a draw reading the uniform buffer after its update)
            Op d;
            d.t = O_DRAW, d.color = colors[pass], d.depth = depths[pass], d.wc = d.color >= 0, d.sread = o.a, d.s_st = S_V;
            s.push_back(d);
        }
    }
    return s;
}
Op draw(int color, int depth, bool wc, bool wd) {
    Op o;
    o.t = O_DRAW, o.color = color, o.depth = depth, o.wc = wc, o.wd = wd;
    return o;
}
Op simple(OpT t, int a = -1, int b = -1) {
    Op o;
    o.t = t, o.a = a, o.b = b;
    return o;
}
/// The named streams: each one opens a hazard when one of the rules is taken away (tests/test_barrier_rules.py's mutations)
std::vector<Op> named(const std::string& name) {
    std::vector<Op> s;
    auto fill = [&](int n, int color, int depth) {
        for (int k = 0; k < n; k++) s.push_back(draw(color, depth, true, true));
    };
    if (name == "carry") {   // (one long pass over several parts, its depth and colour written by every draw)
        fill(40, 0, 10);
    } else if (name == "carry-sample") {   // (a draw in a later part samples the target the pass wrote in an earlier one)
        fill(5, 0, 10);
        Op o = draw(0, 10, false, true);
        o.sample = 0, o.sample_st = S_F;
        s.push_back(o);
        fill(5, 0, 10);
    } else if (name == "carry-writeback") {   // (the part's end copies a target back, then the pass writes it on)
        fill(3, 0, 10);
        s.push_back(simple(O_WB, 0));
        fill(5, 0, 10);
    } else if (name == "carry-between") {   // (another submitter's write between two parts, what the pass's draws read)
        Op o = draw(0, 10, true, true);
        o.sread = 21, o.s_st = S_V;
        for (int k = 0; k < 3; k++) s.push_back(o);
        s.push_back(simple(O_BETWEEN, 21));
        for (int k = 0; k < 5; k++) s.push_back(o);
    } else if (name == "dispatch-chain") {   // (dispatches, each reading what the one before wrote, over parts)
        for (int k = 0; k < 6; k++) s.push_back(simple(O_DISPATCH, 20 + k % 3, 20 + (k + 1) % 3));
    } else if (name == "wait") {   // (a signal, and a wait for it in the same pass: the waiting work reads what was written before)
        Op w = draw(0, 10, true, false);
        w.swrite = 20, w.s_st = S_F;
        s.push_back(w);
        s.push_back(simple(O_SIGNAL, 50));
        s.push_back(simple(O_WAIT, 50));
        Op r = draw(0, 10, true, false);
        r.sread = 20, r.s_st = S_V;
        s.push_back(r);
    } else if (name == "rewrite") {   // (one event signalled twice, a barrier of the game's after each)
        s.push_back(draw(0, 10, true, true));
        s.push_back(simple(O_SIGNAL, 50));
        s.push_back(simple(O_BARRIER));
        s.push_back(simple(O_SIGNAL, 50));
        s.push_back(simple(O_BARRIER));
        s.push_back(draw(0, 10, true, true));
    } else if (name == "carry-signal") {   // (a signal's write held over a part's split, and a wait for it in the next part)
        Op w = draw(0, 10, true, false);
        w.swrite = 20, w.s_st = S_F;
        s.push_back(w);
        s.push_back(simple(O_SIGNAL, 50));
        fill(3, 0, 10);
        s.push_back(simple(O_WAIT, 50));
        Op r = draw(0, 10, true, false);
        r.sread = 20, r.s_st = S_V;
        s.push_back(r);
    } else if (name == "signal-again") {   // (one event signalled twice with nothing between: both writes held, in order)
        fill(2, 0, 10);
        s.push_back(simple(O_SIGNAL, 50));
        s.push_back(simple(O_SIGNAL, 50));
        fill(2, 0, 10);
        s.push_back(simple(O_BARRIER));
    } else if (name == "signal-writeback") {   // (a signal's write after a barrier of the game's, its page written back at the end)
        s.push_back(draw(0, 10, true, true));
        s.push_back(simple(O_SIGNAL, 50));
        s.push_back(simple(O_BARRIER));
    } else if (name == "two-signals") {   // (two events signalled one after the other: their words written in that order)
        fill(2, 0, 10);
        s.push_back(simple(O_SIGNAL, 50));
        s.push_back(simple(O_SIGNAL, 51));
        s.push_back(simple(O_BARRIER));
    } else if (name == "signal") {   // (a signal after draws: its write after them)
        fill(3, 0, 10);
        s.push_back(simple(O_SIGNAL, 51));
        fill(3, 0, 10);
    } else if (name == "dispatch-indirect") {   // (a dispatch writes the grid of the indirect one right after it)
        s.push_back(simple(O_DISPATCH, 20, R_ARGS));
        Op o = simple(O_DISPATCH, 21, 22);
        o.indirect = true;
        s.push_back(o);
    } else if (name == "hazard-split") {   // ((Triforce, P4) a draw samples in its vertex stage the colour its pass's draws wrote: the
        fill(3, 0, 10);                        // replay ends the pass there (draw_hazards) and the next pass start is a full barrier)
        Op o = draw(0, 10, false, true);
        o.sample = 0, o.sample_st = S_V;
        s.push_back(o);
    } else if (name == "dispatch-draw") {   // (a dispatch, a draw that writes what the next dispatch reads)
        s.push_back(simple(O_DISPATCH, 20, 21));
        Op o = draw(0, 10, true, false);
        o.swrite = 22, o.s_st = S_F;
        s.push_back(o);
        s.push_back(simple(O_DISPATCH, 22, 23));
    } else {
        std::fprintf(stderr, "no stream %s\n", name.c_str());
        std::exit(2);
    }
    return s;
}

} // namespace mkwvk
using namespace mkwvk;

Result run_one(const std::vector<Op>& s, int every, Replay* keep = nullptr, bool old_split = false) {
    Replay r;
    r.c = config();
    r.c.every = every;
    if (old_split) r.c.split_rule = false;
    current = &r;
    r.run(s);
    current = nullptr;
    Result res = flatten(r);
    if (keep) *keep = r;
    return res;
}

// ---- (Triforce, P4) the parallel replay: two segments recorded at once ----
bool same_recs(const std::vector<std::vector<Rec>>& a, const std::vector<std::vector<Rec>>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].size() != b[i].size()) return false;
        for (size_t j = 0; j < a[i].size(); j++) {
            const Rec &x = a[i][j], &y = b[i][j];
            if (x.k != y.k || x.src != y.src || x.dst != y.dst || x.sa != y.sa || x.da != y.da || x.acc.res != y.acc.res ||
                x.acc.st != y.acc.st || x.acc.type != y.acc.type || x.acc.op != y.acc.op || x.acc.sub != y.acc.sub)
                return false;
        }
    }
    return true;
}
/// (a random stream as a segment's: no other submitter's work between parts, a segment being never split)
std::vector<Op> segment_stream(uint64_t seed, int n) {
    std::vector<Op> s = random_stream(seed, n);
    s.erase(std::remove_if(s.begin(), s.end(), [](const Op& o) { return o.t == O_BETWEEN; }), s.end());
    return s;
}
/// Two threads take turns at one command buffer (a segment's init command buffer, vk_texture.cpp InitRedirect): the first's full
/// barrier, the other's write, the first's full barrier again and its read of what was written. The second barrier must be recorded
int shared_case() {
    const VkCommandBuffer x = new_cmd();
    work_begun(x);
    Turns turns;
    std::thread t0([&] {
        turns.wait(0);
        full_barrier(x, "a segment's init command buffer at its first use");
        turns.pass(0), turns.wait(0);
        full_barrier(x, "before what this thread does with it next");
        rec(x, K_WORK, {34, S_X, T_XFER_R, 2, 0});
        turns.finish(0);
    });
    std::thread t1([&] {
        turns.wait(1);
        rec(x, K_WORK, {34, S_X, T_XFER_W, 1, 0});   // (the other segment's thread, sent to it: a new image's first change)
        turns.finish(1);
    });
    t0.join(), t1.join();
    const std::vector<Rec>& l = lists[x];
    int write = -1, read = -1;
    for (int i = 0; i < (int)l.size(); i++)
        if (l[i].k == K_WORK) (l[i].acc.type & T_XFER_W ? write : read) = i;
    bool barrier = false;
    for (int i = write + 1; write >= 0 && i < read; i++) barrier |= l[i].k == K_BARRIER && l[i].src == S_ALL && l[i].dst == S_ALL;
    if (write < 0 || read < write || !barrier) {
        std::fprintf(stderr, "FAIL (shared): one thread's write into a command buffer two threads take turns at, and the other's read "
                             "after its own full barrier: no full barrier between (%zu commands)\n", l.size());
        return 1;
    }
    std::printf("PASS: shared: a command buffer two threads take turns at counts both threads' commands (%zu commands, the second "
                "full barrier recorded)\n", l.size());
    return 0;
}
int threads_main(const char* vendor, const std::string& what) {
    if (what == "shared") return shared_case();
    const Config c = config();
    std::vector<std::pair<std::vector<Op>, std::vector<Op>>> pairs;
    if (what.find_first_not_of("0123456789") == std::string::npos) {
        const uint64_t seed = std::strtoull(what.c_str(), nullptr, 10);
        for (uint64_t k = 0; k < 12; k++)
            pairs.push_back({segment_stream(seed * 100 + k, 160 + 40 * (int)(k % 5)), segment_stream(seed * 100 + 50 + k, 120 + 40 * (int)(k % 3))});
    } else {
        pairs.push_back({named(what), named(what)});   // (the same stream twice: what the first leaves, the second's start takes)
    }
    int64_t full_alone = 0, full_together = 0, partial_together = 0;
    int differ = 0;
    for (const auto& pr : pairs) {
        const std::vector<Op>& a = pr.first;
        const std::vector<Op>& b = pr.second;
        // (each alone, on one thread, no parallel replay)
        par_active = false;
        Replay sa, sb;
        sa.c = sb.c = c;
        current = &sa, sa.run_segment(a);
        current = &sb, sb.run_segment(b);
        current = nullptr;
        // (the two at once: the replay thread's and a helper's, op by op in turn)
        par_active = true;
        Replay ta, tb;
        ta.c = tb.c = c;
        Turns turns;
        std::thread t0([&] { current = &ta, ta.run_segment(a, &turns, 0), current = nullptr; });
        std::thread t1([&] { current = &tb, tb.run_segment(b, &turns, 1), current = nullptr; });
        t0.join(), t1.join();
        par_active = false;
        // (no hazard in either, nor across the two in queue order: the replay thread's segment, then the helper's)
        Result ra = flatten(ta), rb = flatten(tb);
        check(ra, Semantics(a, c.dispatch_game), "threads: the replay thread's segment");
        check(rb, Semantics(b, c.dispatch_game), "threads: the helper thread's segment");
        Replay both;
        both.order = ta.order;
        for (std::vector<Rec> cb : tb.order) {
            for (Rec& x : cb)
                if (x.k == K_WORK) x.acc.op += (int)a.size();
            both.order.push_back(cb);
        }
        std::vector<Op> ab = a;
        ab.insert(ab.end(), b.begin(), b.end());
        Result rab = flatten(both);
        check(rab, Semantics(ab, c.dispatch_game), "threads: the two segments in queue order");
        full_alone += flatten(sa).full + flatten(sb).full, full_together += ra.full + rb.full, partial_together += ra.partial + rb.partial;
        if (!same_recs(ta.order, sa.order) || !same_recs(tb.order, sb.order)) differ++;
    }
    const bool global = work_global();
    if (!global && differ) {
        std::fprintf(stderr, "FAIL (threads): %d of %zu pairs of segments recorded at once got other barriers than each alone (%lld full "
                             "barriers at once, %lld alone)\n", differ, pairs.size(), (long long)full_together, (long long)full_alone);
        return 1;
    }
    // (RX_VK_BARRIER_CONTEXT=0: never fewer full barriers beside the helper; more over the random streams, where something is always
    // left out or relaxed alone)
    const bool random = what.find_first_not_of("0123456789") == std::string::npos;
    if (global && (full_together < full_alone || (random && full_together == full_alone))) {
        std::fprintf(stderr, "FAIL (threads): RX_VK_BARRIER_CONTEXT=0 and the rules did not stand aside beside the helper thread (%lld full "
                             "barriers at once, %lld alone)\n", (long long)full_together, (long long)full_alone);
        return 1;
    }
    std::printf("PASS: vendor %s, threads, %s: %zu pairs of segments at once, %lld full barriers and %lld others, %lld full alone (%s); "
                "no hazard in either or across them; dispatch game %d\n", vendor, what.c_str(), pairs.size(), (long long)full_together,
                (long long)partial_together, (long long)full_alone,
                global ? "RX_VK_BARRIER_CONTEXT=0: the rules stood aside beside the helper, as before"
                       : "each command buffer's own count: the same barriers as alone",
                (int)c.dispatch_game);
    return 0;
}

int main(int argc, char** argv) {
    CHECK(argc >= 4);
    dev.props.vendorID = (uint32_t)std::strtoul(argv[1], nullptr, 16);
    if (!std::strcmp(argv[2], "threads")) return threads_main(argv[1], argv[3]);
    const int every = std::atoi(argv[2]);
    const std::string what = argv[3];
    bool compare = false;
    for (int i = 4; i < argc; i++) {
        if (!std::strcmp(argv[i], "compare")) compare = true;
        if (!std::strncmp(argv[i], "slots=", 6)) g_slots = std::max(1, std::atoi(argv[i] + 6));
    }
    const Config c = config();
    std::vector<std::vector<Op>> streams;
    if (what.find_first_not_of("0123456789") == std::string::npos) {
        const uint64_t seed = std::strtoull(what.c_str(), nullptr, 10);
        for (int k = 0; k < 24; k++) streams.push_back(random_stream(seed * 100 + (uint64_t)k, 160 + 40 * (k % 5)));
    } else {
        streams.push_back(named(what));
    }
    int64_t full = 0, partial = 0, splits = 0, carried = 0;
    for (size_t k = 0; k < streams.size(); k++) {
        const Semantics sem(streams[k], c.dispatch_game);
        Replay r;
        Result res = run_one(streams[k], every, &r);
        check(res, sem, (what + " split").c_str());
        full += res.full, partial += res.partial, splits += r.splits, carried += r.carried;
        if (compare) {
            // (every pair the stream replayed whole orders, and the old way's parts order too, the parts order: a split the old way
            // started its next part afresh, so what the whole stream ordered only through draw_hazards' state was not kept there either)
            Result whole = run_one(streams[k], 0);
            check(whole, sem, (what + " whole").c_str());
            Result old = run_one(streams[k], every, nullptr, true);
            check(old, sem, (what + " old parts").c_str());
            for (const auto& p : whole.covered)
                if (old.covered.count(p) && !res.covered.count(p)) {
                    const auto find = [&](int op, int sub) {
                        for (const Flat& f : whole.flat)
                            if (f.r.k == K_WORK && f.r.acc.op == op && f.r.acc.sub == sub) return f.r.acc;
                        return Acc{};
                    };
                    const Acc x = find(std::get<0>(p), std::get<1>(p)), y = find(std::get<2>(p), std::get<3>(p));
                    std::fprintf(stderr, "FAIL (%s, stream %zu): op %d (kind %d) access %d (resource %d, type %#x) and op %d (kind %d) "
                                         "access %d (resource %d, type %#x) are ordered in one part, not across the parts\n", what.c_str(),
                                 k, x.op, (int)streams[k][x.op].t, x.sub, x.res, x.type, y.op, (int)streams[k][y.op].t, y.sub, y.res,
                                 y.type);
                    for (int o = std::max(0, x.op - 8); o <= y.op && o < (int)streams[k].size(); o++) {
                        const Op& q = streams[k][o];
                        std::fprintf(stderr, "  op %d: kind %d targets %d/%d writes %d/%d sample %d storage %d/%d a %d b %d%s\n", o, (int)q.t,
                                     q.color, q.depth, (int)q.wc, (int)q.wd, q.sample, q.sread, q.swrite, q.a, q.b, q.indirect ? " indirect" : "");
                    }
                    return 1;
                }
        }
    }
    std::printf("PASS: vendor %s, parts of %d, %s: %zu streams, %lld full barriers and %lld others, %lld splits (%lld went on in their "
                "pass); split rule %d, event rule %d, dispatch game %d\n", argv[1], every, what.c_str(), streams.size(), (long long)full,
                (long long)partial, (long long)splits, (long long)carried, (int)c.split_rule, (int)c.event_rule, (int)c.dispatch_game);
    return 0;
}
