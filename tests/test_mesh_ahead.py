"""A run of indirect mesh draws, their records copied at the first of them (gpu/vk/mesh_ahead.h; no game, no GPU).

After NX2navi's mesh_ahead: every indirect mesh draw has its records copied into a ring as {count, 1, 1} before it, outside the
pass, so each one ended its pass. Now the first of a run copies the records of the indirect mesh draws after it too (those with only
state, draws, counter reports and render conditions between), and they stay in the pass. RX_VK_MESH_AHEAD=0: each its own.

1. The header: the scan over authored op streams (the run's end at an op that ends it, at the stream's end, at a segment's end, after
   kMaxOps ops, at a record of a bad size), fit() within kMaxBytes and the table, the table's take (in order, a draw not drawn let go,
   a mismatch clears it), draw_of's numbers.
2. The model: vk_replay.cpp's mesh_run_kind and mesh_ahead_note and vk_draw.cpp's ring and mesh_indirect_convert, cut out of the
   source and run on a fake GPU that runs every recorded fill and copy in order on the game's memory and the ring, with a walk of
   authored streams as replay() walks them (the table cleared at its start and at each op that ends a run, the note before each
   indirect mesh draw). Each draw reads its ring entry where it is drawn and must find the counts its records hold then: a run, a
   dispatch that writes the next draw's records (it ends the run: the draw after it copies its own), a barrier, a uniform update, new
   targets, a draw left out by its render condition, the stream walked again with new counts, a segment's end, the switch at 0, a
   record changed under the walk, 40 draws (the table's bound), draws of 4,096 records (the ring's share: no run's entries over
   kMaxBytes, none overlapping). Then rules taken out in turn (mutations of the cut code or the header): a case must fail each time.
3. The source: mesh_run_kind sends every op it does not list to the run's end, and lists none that records outside a pass or writes
   memory on the GPU's timeline; replay() clears the table where the model does and notes the draw before draw(); the switch is in
   the development guide.

    python -I -B tests/test_mesh_ahead.py [--cxx g++] --headers <Vulkan headers' include folder>
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


HEADER_TEST = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "mesh_ahead.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::mesh_ahead;
enum { END = 0, STATE = 1, MESH = 2, DISPATCH = 3, MESHD = 4 };   // (the test's own op numbers; kind() below says what each is)
std::vector<uint8_t> s;
size_t put(uint32_t op, uint32_t size = 16) { size_t at = s.size(); s.resize(at + size); uint32_t h[2] = {op, size}; std::memcpy(&s[at], h, 8); return at; }
int kind(uint32_t op, const uint8_t*) { return op == STATE || op == MESHD ? 1 : op == MESH ? 2 : 0; }
int main() {
    CHECK(on(nullptr) && on("") && on("1") && on("00") && !on("0"));
    const size_t d1 = put(MESH); put(STATE); const size_t d2 = put(MESH); put(MESHD); put(STATE, 64); const size_t d3 = put(MESH);
    put(DISPATCH); put(MESH); put(END);
    const uint8_t* out[kMax];
    CHECK(scan(&s[d1], nullptr, out, kMax, kind) == 2 && out[0] == &s[d2] && out[1] == &s[d3]);   // (to the dispatch)
    CHECK(scan(&s[d1], nullptr, out, 1, kind) == 1);                                             // (at most max)
    CHECK(scan(&s[d1], &s[d3], out, kMax, kind) == 1);                                           // (a segment's end)
    CHECK(scan(&s[d3], nullptr, out, kMax, kind) == 0);                                          // (a dispatch right after)
    s.clear();
    const size_t a = put(MESH);
    for (int i = 0; i < kMaxOps + 10; i++) put(STATE);
    put(MESH); put(END);
    CHECK(scan(&s[a], nullptr, out, kMax, kind) == 0);                                           // (past kMaxOps ops: not looked at)
    s.clear();
    const size_t b = put(MESH); put(STATE); const size_t bad = put(STATE); put(MESH); put(END);
    uint32_t h[2] = {STATE, 12};
    std::memcpy(&s[bad], h, 8);                                                                  // (a size not 8-aligned: the end)
    CHECK(scan(&s[b], nullptr, out, kMax, kind) == 0);
    h[1] = 0; std::memcpy(&s[bad], h, 8);
    CHECK(scan(&s[b], nullptr, out, kMax, kind) == 0);
    // draw_of: as the conversion counts them
    const Draw x = draw_of(nullptr, 0x1000, 0, -4, 1);
    CHECK(x.n == 1 && x.stride == 8 && x.word == 1);
    CHECK(draw_of(nullptr, 0, 9000, 16, 0).n == 4096 && draw_of(nullptr, 0, 7, 16, 0).n == 7 && draw_of(nullptr, 0, 7, 16, 0).stride == 16);
    CHECK(ring_bytes(1) == 16 && ring_bytes(4) == 48 && ring_bytes(5) == 64);
    // fit: within kMaxBytes with the first, and kMax at most
    Draw big[kMax];
    for (auto& d : big) d.n = 4096;
    const int f = fit(big, kMax, 4096);
    CHECK(f >= 1 && (uint64_t)(f + 1) * ring_bytes(4096) <= kMaxBytes && (uint64_t)(f + 2) * ring_bytes(4096) > kMaxBytes);
    Draw small[kMax + 8];
    for (auto& d : small) d.n = 1;
    CHECK(fit(small, kMax + 8, 1) == kMax && fit(small, 3, 1) == 3 && fit(small, 0, 1) == 0);
    // the table
    Table t;
    const uint8_t ops[4] = {};
    for (int i = 0; i < 3; i++) t.e[i].d = draw_of(&ops[i + 1], 0x100 * (i + 1), 4, 8, 0), t.e[i].at = 1000 * (i + 1);
    t.n = 3;
    uint64_t at = 0;
    CHECK(t.holds(&ops[1]) && t.holds(&ops[3]) && !t.holds(&ops[0]));
    CHECK(t.take(draw_of(&ops[1], 0x100, 4, 8, 0), &at) && at == 1000 && !t.holds(&ops[1]));
    CHECK(t.take(draw_of(&ops[3], 0x300, 4, 8, 0), &at) && at == 3000);          // (the one between not drawn: let go)
    CHECK(t.i == 3 && !t.holds(&ops[2]));
    t.n = 3, t.i = 0;
    CHECK(!t.take(draw_of(&ops[1], 0x999, 4, 8, 0), &at) && t.n == 0);           // (another record address: none, and cleared)
    t.n = 3, t.i = 0;
    CHECK(!t.take(draw_of(&ops[1], 0x100, 5, 8, 0), &at) && t.n == 0);           // (another count)
    t.n = 3, t.i = 0;
    CHECK(!t.take(draw_of(&ops[1], 0x100, 4, 16, 0), &at) && t.n == 0);          // (another stride)
    t.n = 3, t.i = 0;
    CHECK(!t.take(draw_of(&ops[1], 0x100, 4, 8, 1), &at) && t.n == 0);           // (another word)
    t.n = 3, t.i = 0;
    CHECK(!t.take(draw_of(&ops[0], 0x100, 4, 8, 0), &at) && t.n == 0);           // (an op it does not hold)
    std::printf("PASS: the scan, fit, draw_of and the table\n");
}
'''

MODEL_TEST = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "rx_nvn.h"
#include "mesh_ahead.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d (%s) %s\n", __LINE__, g_case.c_str(), #x); std::exit(1); } } while (0)
typedef struct VmaAllocation_T* VmaAllocation;
std::string g_case;
namespace mkwvk {
const char* getenv(const char* n) { return !std::strcmp(n, "RX_VK_MESH_AHEAD") && g_case == "off" ? "0" : nullptr; }
void logf(const char*, ...) {}
#define PROF_EXACT() (void)0
struct BufferRef {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t offset = 0, size = 0;
    VkDeviceAddress address = 0;
    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};
struct DrawArgs { bool mesh = false; uint64_t indirect = 0, count_addr = 0; int max = 0, stride = 0; };
// ---- the fake GPU: the game's memory (one buffer, its GPU addresses from kBase) and the ring; every command run as recorded, in order
constexpr uint64_t kBase = 0x100000000ull;
const VkBuffer GUEST = (VkBuffer)(uintptr_t)0x11, RING = (VkBuffer)(uintptr_t)0x22;
std::vector<uint8_t> guest(1 << 22), ring;
int passes_ended = 0, barriers = 0, fills = 0, copies = 0;
struct Span { uint64_t lo, hi; };
std::vector<Span> batch;   // (the ring ranges filled since the last barrier into the transfer stage)
BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t size, bool) {
    if (gpu < kBase || gpu + size > kBase + guest.size()) return {};
    BufferRef r;
    r.buffer = GUEST, r.offset = gpu - kBase, r.size = size;
    return r;
}
bool create_buffer(uint64_t size, VkBufferUsageFlags, bool, VkBuffer& b, VmaAllocation& m, void** mapped, bool = false) {
    ring.assign(size, 0xEE), b = RING, m = nullptr, *mapped = nullptr;
    return true;
}
void end_pass(VkCommandBuffer) { passes_ended++; }
void vkCmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo* d) {
    barriers++;
    if (d->pMemoryBarriers[0].srcStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) batch.clear();   // (a conversion's first: its fills follow)
}
void vkCmdFillBuffer(VkCommandBuffer, VkBuffer b, VkDeviceSize off, VkDeviceSize size, uint32_t v) {
    CHECK(b == RING && off + size <= ring.size() && size % 4 == 0);
    for (Span& s : batch) CHECK(off + size <= s.lo || off >= s.hi);   // (no two entries of one batch overlap)
    batch.push_back({off, off + size});
    uint64_t lo = batch[0].lo, hi = batch[0].hi;
    for (const Span& s : batch) lo = std::min(lo, s.lo), hi = std::max(hi, s.hi);
    CHECK(hi - lo <= mesh_ahead::kMaxBytes);   // (a run's entries within the ring's share)
    for (uint64_t i = 0; i < size; i += 4) std::memcpy(&ring[off + i], &v, 4);
    fills++;
}
void vkCmdCopyBuffer(VkCommandBuffer, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy* r) {
    CHECK(src == GUEST && dst == RING);
    for (uint32_t i = 0; i < n; i++) {
        CHECK(r[i].srcOffset + r[i].size <= guest.size() && r[i].dstOffset + r[i].size <= ring.size());
        std::memcpy(&ring[r[i].dstOffset], &guest[r[i].srcOffset], r[i].size);
    }
    copies++;
}
bool mesh_ahead_enabled();
namespace {
/* REPLAY */
/* DRAW */
} // namespace
/* ENABLED */
} // namespace mkwvk
using namespace mkwvk;

// ---- the walk, as vk_replay.cpp's replay() walks a command buffer
std::vector<uint8_t> st;
size_t rec(uint32_t op, const void* pl, uint32_t bytes) {
    const uint32_t size = (8 + bytes + 7) & ~7u;
    const size_t at = st.size();
    st.resize(at + size);
    const nvn_op_hdr h{op, size};
    std::memcpy(&st[at], &h, 8);
    if (bytes) std::memcpy(&st[at + 8], pl, bytes);
    return at;
}
size_t mesh(uint64_t records, int max, int stride = 8) { nvn_p_draw_mesh d{}; d.kind = 2, d.indirect = records, d.max = max, d.stride = stride; return rec(NOP_DRAW_MESH, &d, sizeof d); }
size_t op(uint32_t o) { uint8_t z[64] = {}; return rec(o, z, o == NOP_UPDATE_UB ? 24 : 16); }
// the game's records for a draw at `records`: n of them, stride bytes apart, the count in word 0
void counts(uint64_t records, int n, int stride, uint32_t first) {
    for (int i = 0; i < n; i++) { const uint32_t c = first + (uint32_t)i; std::memcpy(&guest[records - kBase + (uint64_t)i * stride], &c, 4); }
}
struct Walk { std::vector<size_t> skip; size_t end = (size_t)-1; size_t from = 0; std::vector<std::pair<size_t, uint64_t>> dispatch_writes; };
int draws = 0;
void walk(const Walk& w = {}) {
    const uint8_t* p = st.data() + w.from;
    const uint8_t* end = w.end == (size_t)-1 ? nullptr : st.data() + w.end;
    mesh_ahead::t_table.clear();
    VkCommandBuffer cmd = (VkCommandBuffer)(uintptr_t)0x900;
    for (;;) {
        if (p == end) return;
        nvn_op_hdr h;
        std::memcpy(&h, p, sizeof h);
        if (mesh_ahead::t_table.n && !mesh_run_kind(h.op, p)) mesh_ahead::t_table.clear();
        const size_t at = (size_t)(p - st.data());
        if (h.op == NOP_END) return;
        if (h.op == NOP_DISPATCH)   // (the culling compute: writes the next draws' records on the GPU, in order)
            for (const auto& [where, records] : w.dispatch_writes)
                if (where == at) counts(records, 16, 8, 500);
        if (h.op == NOP_DRAW_MESH && std::find(w.skip.begin(), w.skip.end(), at) == w.skip.end()) {
            nvn_p_draw_mesh d;
            std::memcpy(&d, p + 8, sizeof d);
            DrawArgs da;
            da.mesh = true, da.indirect = d.indirect, da.max = d.max, da.stride = d.stride;
            if (da.indirect) mesh_ahead_note(p, end);
            CHECK(mesh_indirect_convert(cmd, da));
            CHECK(mesh_conv_buf == RING);
            // (the draw reads its entry now: the counts its records hold now, {count, 1, 1} each)
            const mesh_ahead::Draw x = mesh_ahead::draw_of(nullptr, d.indirect, d.max, d.stride, 0);
            for (uint32_t i = 0; i < x.n; i++) {
                uint32_t want, got[3];
                std::memcpy(&want, &guest[d.indirect - kBase + (uint64_t)i * x.stride], 4);
                std::memcpy(got, &ring[mesh_conv_off + 12ull * i], 12);
                CHECK(got[0] == want && got[1] == 1 && got[2] == 1);
            }
            draws++;
        }
        p += h.size;
    }
}
void reset() {
    st.clear(), mesh_ahead::t_table.clear(), mesh_ahead::t_pending = {};
    passes_ended = barriers = fills = copies = draws = 0;
    st_mesh_ahead_taken = st_mesh_ahead_own = st_mesh_ahead_filled = 0;
}
const uint64_t R1 = kBase + 0x1000, R2 = kBase + 0x2000, R3 = kBase + 0x3000, R4 = kBase + 0x4000;
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    g_case = argv[1];
    reset();
    counts(R1, 16, 8, 10), counts(R2, 16, 8, 20), counts(R3, 16, 8, 30), counts(R4, 16, 16, 40);
    if (g_case == "run") {   // (state, a counter report, a plain draw between: one run, the first copies all three)
        mesh(R1, 16), op(NOP_BIND_TEX), mesh(R2, 5), op(NOP_REPORT_COUNTER), op(NOP_DRAW), mesh(R4, 16, 16), op(NOP_END);
        walk();
        CHECK(draws == 3 && passes_ended == 1 && st_mesh_ahead_own == 1 && st_mesh_ahead_taken == 2 && st_mesh_ahead_filled == 2);
        CHECK(barriers == 3);   // (the three of one conversion, for all of them)
    } else if (g_case == "dispatch") {   // (a dispatch writes the third's records: it ends the run, the third copies after it)
        mesh(R1, 16), mesh(R2, 16);
        const size_t dsp = op(NOP_DISPATCH);
        mesh(R3, 16), op(NOP_END);
        Walk w;
        w.dispatch_writes = {{dsp, R3}};
        walk(w);
        CHECK(draws == 3 && st_mesh_ahead_own == 2 && st_mesh_ahead_taken == 1 && passes_ended == 2);
    } else if (g_case == "ends") {   // (a barrier, a uniform update, new targets, a signal, a clear, a copy: each ends the run)
        for (uint32_t o : {(uint32_t)NOP_BARRIER, (uint32_t)NOP_UPDATE_UB, (uint32_t)NOP_SET_RT, (uint32_t)NOP_SIGNAL_EVENT,
                           (uint32_t)NOP_CLEAR_COLOR, (uint32_t)NOP_COPY_B2B, (uint32_t)NOP_WAIT_EVENT, (uint32_t)NOP_CLEAR_BUF}) {
            reset();
            mesh(R1, 16), op(o), mesh(R2, 16), op(NOP_END);
            walk();
            CHECK(draws == 2 && st_mesh_ahead_own == 2 && st_mesh_ahead_taken == 0);
        }
    } else if (g_case == "skip") {   // (the second left out by its render condition: the third takes its own entry)
        mesh(R1, 16);
        const size_t d2 = mesh(R2, 16);
        op(NOP_RENDER_COND), mesh(R3, 16), op(NOP_END);
        Walk w;
        w.skip = {d2};
        walk(w);
        CHECK(draws == 2 && st_mesh_ahead_own == 1 && st_mesh_ahead_taken == 1);
    } else if (g_case == "again") {   // (walked again with new counts: copied again)
        mesh(R1, 16), mesh(R2, 16), mesh(R3, 16), op(NOP_END);
        walk();
        counts(R1, 16, 8, 70), counts(R2, 16, 8, 80), counts(R3, 16, 8, 90);
        walk();
        CHECK(draws == 6 && st_mesh_ahead_own == 2 && st_mesh_ahead_taken == 4);
    } else if (g_case == "segment") {   // (a segment's end between: the next segment's draw copies its own)
        mesh(R1, 16), mesh(R2, 16);
        const size_t cut = st.size();
        mesh(R3, 16), op(NOP_END);
        Walk a;
        a.end = cut;
        walk(a);
        Walk b;
        b.from = cut;
        walk(b);
        CHECK(draws == 3 && st_mesh_ahead_own == 2 && st_mesh_ahead_taken == 1);
    } else if (g_case == "off") {   // (RX_VK_MESH_AHEAD=0: each its own, as before)
        mesh(R1, 16), mesh(R2, 16), mesh(R3, 16), op(NOP_END);
        walk();
        CHECK(draws == 3 && st_mesh_ahead_own == 3 && st_mesh_ahead_taken == 0 && passes_ended == 3 && barriers == 9);
    } else if (g_case == "changed") {   // (the next draw's record changed under the walk: it copies its own)
        mesh(R1, 16);
        const size_t d2 = mesh(R2, 16);
        op(NOP_END);
        mesh_ahead::t_table.clear();
        // (the first draw, then the second's op changed to other records, then the second: one walk, the table not cleared between)
        const uint8_t* p = st.data();
        nvn_p_draw_mesh d;
        std::memcpy(&d, p + 8, sizeof d);
        DrawArgs da;
        da.mesh = true, da.indirect = d.indirect, da.max = d.max, da.stride = d.stride;
        mesh_ahead_note(p, nullptr);
        CHECK(mesh_indirect_convert((VkCommandBuffer)(uintptr_t)0x900, da) && mesh_ahead::t_table.n == 1);
        nvn_p_draw_mesh d2p;
        std::memcpy(&d2p, &st[d2 + 8], sizeof d2p);
        d2p.indirect = R3;
        std::memcpy(&st[d2 + 8], &d2p, sizeof d2p);
        const uint8_t* q = st.data() + d2;
        DrawArgs db;
        db.mesh = true, db.indirect = R3, db.max = 16, db.stride = 8;
        mesh_ahead_note(q, nullptr);
        CHECK(mesh_indirect_convert((VkCommandBuffer)(uintptr_t)0x900, db));
        uint32_t got;
        std::memcpy(&got, &ring[mesh_conv_off], 4);
        CHECK(got == 30 && st_mesh_ahead_own == 2 && st_mesh_ahead_taken == 0);
    } else if (g_case == "many") {   // (40 in a run: the table's bound, then the next copies its own and the rest)
        for (int i = 0; i < 40; i++) mesh(R1 + 0x10000ull * (uint64_t)i, 4), counts(R1 + 0x10000ull * (uint64_t)i, 4, 8, 100 * (uint32_t)i);
        op(NOP_END);
        walk();
        CHECK(draws == 40 && st_mesh_ahead_own == 2 && st_mesh_ahead_taken == 38 && st_mesh_ahead_filled == 38);
    } else if (g_case == "big") {   // (4,096 records each: the run's entries within the ring's share, none overlapping)
        for (int i = 0; i < 30; i++) mesh(R1 + 0x8000ull * (uint64_t)i, 4096, 1), counts(R1 + 0x8000ull * (uint64_t)i, 4096, 1, (uint32_t)i << 16);
        op(NOP_END);
        walk();
        const int per = mesh_ahead::fit(nullptr, 0, 4096) + (int)(mesh_ahead::kMaxBytes / mesh_ahead::ring_bytes(4096)) - 1;
        CHECK(draws == 30 && st_mesh_ahead_own >= 2 && st_mesh_ahead_own <= 30 / std::max(per, 1) + 2);
    } else {
        std::fprintf(stderr, "no case %s\n", g_case.c_str());
        return 2;
    }
    std::printf("PASS %s\n", g_case.c_str());
    return 0;
}
'''

CASES = ["run", "dispatch", "ends", "skip", "again", "segment", "off", "changed", "many", "big"]

# (each: what is taken out, the file it is in, its text, what replaces it, the case that must then fail)
MUTATIONS = [
    ("a dispatch ending the run", "replay", "    case NOP_REPORT_COUNTER: case NOP_RESET_COUNTER: case NOP_RENDER_ENABLE: case NOP_RENDER_COND:\n        return 1;",
     "    case NOP_REPORT_COUNTER: case NOP_RESET_COUNTER: case NOP_RENDER_ENABLE: case NOP_RENDER_COND: case NOP_DISPATCH:\n        return 1;",
     "dispatch"),
    ("a barrier ending the run", "replay", "    case NOP_REPORT_COUNTER: case NOP_RESET_COUNTER: case NOP_RENDER_ENABLE: case NOP_RENDER_COND:\n        return 1;",
     "    case NOP_REPORT_COUNTER: case NOP_RESET_COUNTER: case NOP_RENDER_ENABLE: case NOP_RENDER_COND: case NOP_BARRIER:\n        return 1;",
     "ends"),
    ("an entry for each next draw", "draw", "for (int k = 0; k < m; k++) next[k].off = mesh_ring_at, mesh_ring_at += mesh_ahead::ring_bytes(next[k].d.n);",
     "for (int k = 0; k < m; k++) next[k].off = off;", "run"),
    ("each next draw's own records copied", "draw", "for (int k = 0; k < m; k++) copy(next[k].args, next[k].d.n, next[k].d.stride, next[k].off);",
     "for (int k = 0; k < m; k++) copy(args, next[k].d.n, next[k].d.stride, next[k].off);", "run"),
    ("the record checked at the take", "header", "                if (!same(e[k].d, d)) break;\n", "", "changed"),
    ("the ring's share", "header", "while (k < n && bytes + ring_bytes(next[k].n) <= kMaxBytes)",
     "while (k < n && bytes + ring_bytes(next[k].n) <= 4 * kMaxBytes)", "big"),
    ("the switch", "draw", "const bool v = mesh_ahead::on(getenv(\"RX_VK_MESH_AHEAD\"));", "const bool v = true;", "off"),
]


def cut_model(replay, draw):
    payload = replay[replay.index("template <class T>\nT payload(const uint8_t* a, uint32_t bytes) {"):]
    payload = payload[:payload.index("\n}\n") + 3]
    rep = payload + block(replay, "int mesh_run_kind(uint32_t op, const uint8_t* rec) {") + "\n" + block(replay, "void mesh_ahead_note(")
    start = draw.index("constexpr uint64_t kMeshRing = 4ull << 20;")
    conv = block(draw, "bool mesh_indirect_convert(VkCommandBuffer cmd, const DrawArgs& d) {")
    dr = draw[start:draw.index(conv) + len(conv)]
    return rep, dr, block(draw, "bool mesh_ahead_enabled() {")


def check_source(replay, draw, dev):
    kind = block(replay, "int mesh_run_kind(uint32_t op, const uint8_t* rec) {")
    if "default: return 0;" not in kind:
        raise SystemExit("vk_replay.cpp mesh_run_kind: an op it does not list must end the run (default: return 0)")
    listed = set(re.findall(r"case (NOP_\w+):", kind))
    never = {"NOP_END", "NOP_SIGNAL_EVENT", "NOP_COPY_B2B", "NOP_UPDATE_UB", "NOP_SET_RT", "NOP_CLEAR_COLOR", "NOP_CLEAR_DS", "NOP_DISPATCH",
             "NOP_BARRIER", "NOP_COPY_B2T", "NOP_COPY_T2B", "NOP_COPY_T2T", "NOP_CLEAR_TEX", "NOP_DISPATCH_INDIRECT", "NOP_WAIT_EVENT",
             "NOP_CLEAR_BUF"}
    if listed & never:
        raise SystemExit("vk_replay.cpp mesh_run_kind lets %s into a run (it records outside a pass, writes memory on the GPU's "
                         "timeline, changes the targets or orders the queues)" % sorted(listed & never))
    body = block(replay, "void replay(VkCommandBuffer& cmd, NvnState& s, const uint8_t* p, const uint8_t* end = nullptr) {")
    head = body[:body.index("switch (h.op) {")]
    if "mesh_ahead::t_table.clear();" not in head[:head.index("for (;;)")]:
        raise SystemExit("vk_replay.cpp replay: the table is not cleared at the walk's start")
    if "if (mesh_ahead::t_table.n && !mesh_run_kind(h.op, p)) mesh_ahead::t_table.clear();" not in head[head.index("for (;;)"):]:
        raise SystemExit("vk_replay.cpp replay: the table is not cleared at an op that ends a run, before the op")
    case = body[body.index("        case NOP_DRAW_MESH: {"):]
    case = case[:case.index("break;")]
    if not re.search(r"if \(da\.indirect\) mesh_ahead_note\(p, end\);[^\n]*\n\s*draw\(cmd, s, da\);", case):
        raise SystemExit("vk_replay.cpp NOP_DRAW_MESH: the draw is not noted (mesh_ahead_note) right before draw()")
    d = block(draw, "void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {")
    if d.index("mesh_indirect_convert(cmd, d);") > d.index("Targets at ="):
        raise SystemExit("vk_draw.cpp draw: the conversion is not the draw's first step")
    if "| `RX_VK_MESH_AHEAD=0` |" not in dev:
        raise SystemExit("docs/development.md: RX_VK_MESH_AHEAD is not in the switches table")


def build(cxx, flags, d, name, src):
    cpp, exe = d / (name + ".cpp"), d / (name + ".exe")
    cpp.write_text(src, encoding="utf-8")
    r = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-missing-field-initializers",
                        *flags, str(cpp), "-o", str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        raise SystemExit("%s: the build failed:\n%s" % (name, r.stdout))
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    ap.add_argument("--headers", required=True)
    a = ap.parse_args()
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    header = (VK / "mesh_ahead.h").read_text(encoding="utf-8")
    check_source(replay, draw, (ROOT / "docs" / "development.md").read_text(encoding="utf-8"))
    print("PASS sources: the ops of a run, the table cleared where the walk leaves one, the draw noted before draw()")
    with tempfile.TemporaryDirectory(prefix="mesh-ahead-") as tmp:
        d = Path(tmp)
        hdir = d / "inc"
        hdir.mkdir()

        def model(rep_text, draw_text, header_text, name):
            sub = d / name
            sub.mkdir()
            (sub / "mesh_ahead.h").write_text(header_text, encoding="utf-8")
            rep, dr, en = cut_model(rep_text, draw_text)
            src = MODEL_TEST.replace("/* REPLAY */", rep).replace("/* DRAW */", dr).replace("/* ENABLED */", en)
            return build(a.cxx, ["-I" + str(sub), "-I" + str(ROOT / "gpu" / "render"), "-I" + a.headers], d, name, src)

        (hdir / "mesh_ahead.h").write_text(header, encoding="utf-8")
        exe = build(a.cxx, ["-I" + str(hdir)], d, "header", HEADER_TEST)
        r = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        if r.returncode:
            raise SystemExit("the header test failed:\n" + r.stdout)
        print(r.stdout.strip())
        exe = model(replay, draw, header, "model")
        for case in CASES:
            r = subprocess.run([str(exe), case], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
            if r.returncode:
                raise SystemExit("model case %s failed:\n%s" % (case, r.stdout))
        print("PASS model: %d cases: every draw reads the counts its records hold where it is drawn" % len(CASES))
        for i, (what, where, old, new, case) in enumerate(MUTATIONS):
            texts = {"replay": replay, "draw": draw, "header": header}
            if texts[where].count(old) != 1:
                raise SystemExit("mutation %r: its text is not once in %s (update the test with the code)" % (what, where))
            texts[where] = texts[where].replace(old, new)
            exe = model(texts["replay"], texts["draw"], texts["header"], "mutant%d" % i)
            r = subprocess.run([str(exe), case], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
            if r.returncode == 0:
                raise SystemExit("mutation %r: case %s still passed without it" % (what, case))
            last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "exit %d" % r.returncode
            print("PASS mutation: without %s, case %s fails (%s)" % (what, case, last))
    print("ALL PASS")


if __name__ == "__main__":
    main()
