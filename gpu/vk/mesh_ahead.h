// (Triforce) The indirect mesh draws of a pass with their records prepared at the first of them, after NX2navi's mesh_ahead
// (runtime/vk_exec_draw.c mesh_ring_entry, runtime/vk_exec.c op_linear and vk_exec_mesh_run).
//
// NVN's indirect mesh draw reads {count, first} and Vulkan's {x, y, z}: each one's records are copied into a ring as {count, 1, 1}
// before it (vk_draw.cpp mesh_indirect_convert). A copy cannot be recorded inside a pass, so every indirect mesh draw ended its pass
// and the next draw began it again behind a barrier, three barriers and a fill and a copy between (OoT's foliage, grass and leaves:
// about 16 a frame at the Market, 45 in NX2navi's village). Now the first indirect mesh draw of a run copies the records of the ones
// after it as well, in the same barriers; those find their entry and stay in the pass. A run is the indirect mesh draws after it with
// only ops between that store state, draw, or reach the CPU alone (the counter reports, which the replay writes as it records, and the
// render conditions, which it reads): nothing between runs outside a pass or writes memory where the GPU could, so no work of the
// game's between can write the records after they were copied (the culling compute that writes them is a dispatch, which ends the run;
// a draw's shader writes a later draw's records only with the game's barrier between, which ends it too, as on the console, where the
// GPU's front end reads them early). An entry is taken only by the draw it was copied for: its op, the same record address, count,
// stride and word; the replay clears the table at any other op (and at each command buffer's start), and a draw the replay did not
// draw (its render condition) leaves its entry unused. At most kMax draws and kMaxBytes of ring a run, so the ring (4 MB) never comes
// round inside one. RX_VK_MESH_AHEAD=0: each draw copies its own, as before.
//
// Apart from Vulkan and the stream's op numbers so that tests/test_mesh_ahead.py can run the scan and the table over authored streams.
#pragma once

#include <cstdint>
#include <cstring>

namespace mkwvk::mesh_ahead {

constexpr int kMax = 32;                      // (the draws after the first whose records it copies, at most)
constexpr int kMaxOps = 2048;                 // (the ops looked at after it, at most: NX2navi's MESH_RUN_OPS)
constexpr uint64_t kMaxBytes = 1ull << 20;    // (a run's ring bytes, at most: a quarter of vk_draw.cpp's kMeshRing)

/// RX_VK_MESH_AHEAD: on unless "0"
inline bool on(const char* env) {
    return !(env && env[0] == '0' && !env[1]);
}

/// What an indirect mesh draw's conversion copies: its op in the stream (where the replay meets it), its records' address, how many,
/// their stride and the word that holds the count
struct Draw {
    const uint8_t* op = nullptr;
    uint64_t indirect = 0;
    uint32_t n = 0, stride = 0, word = 0;
};
inline bool same(const Draw& a, const Draw& b) {
    return a.op == b.op && a.indirect == b.indirect && a.n == b.n && a.stride == b.stride && a.word == b.word;
}
/// What the conversion copies for an indirect mesh draw of the stream's max and stride (the game's ints): its records, 1 to 4096 of
/// them (as many as the draw may draw), 8 bytes apart when the stride is not given
inline Draw draw_of(const uint8_t* op, uint64_t indirect, int32_t max, int32_t stride, uint32_t word) {
    Draw d;
    d.op = op, d.indirect = indirect, d.word = word;
    d.n = (uint32_t)(max < 1 ? 1 : max > 4096 ? 4096 : max);
    d.stride = stride > 0 ? (uint32_t)stride : 8u;
    return d;
}
/// A draw's ring bytes (vk_draw.cpp's: {count, 1, 1} a record, 16-aligned)
inline uint64_t ring_bytes(uint32_t n) {
    return ((uint64_t)n * 12 + 15) & ~15ull;
}

/// The ops after the one at `at` (records {uint32 op, uint32 size}, sizes 8-aligned), up to `end` (null: none): the indirect mesh draws
/// among them while every op between belongs in a run, into out, at most max; at most kMaxOps ops looked at. kind(op, record): 0 the
/// run ends there (the stream's end too), 1 it may be in a run, 2 an indirect mesh draw (in the run, and collected)
template <class K>
int scan(const uint8_t* at, const uint8_t* end, const uint8_t** out, int max, K kind) {
    uint32_t h[2];
    std::memcpy(h, at, sizeof h);
    if (h[1] < 8 || (h[1] & 7)) return 0;
    const uint8_t* p = at + h[1];
    int n = 0;
    for (int ops = 0; ops < kMaxOps && n < max && p != end; ops++) {
        std::memcpy(h, p, sizeof h);
        if (h[1] < 8 || (h[1] & 7)) break;
        const int k = kind(h[0], p);
        if (!k) break;
        if (k == 2) out[n++] = p;
        p += h[1];
    }
    return n;
}

/// How many of the run's draws after the first get entries: in order, while the ring bytes of the first and theirs stay within kMaxBytes
/// (and no more than the table holds)
inline int fit(const Draw* next, int n, uint32_t first_n) {
    if (n > kMax) n = kMax;
    uint64_t bytes = ring_bytes(first_n);
    int k = 0;
    while (k < n && bytes + ring_bytes(next[k].n) <= kMaxBytes) bytes += ring_bytes(next[k].n), k++;
    return k;
}

/// What the replay knows of the indirect mesh draw it is about to draw (set before draw(), vk_replay.cpp; taken by the conversion):
/// its op, and the draws after it in its run (none when they are in the table already)
struct Pending {
    struct Next {   // (as the stream has it: draw_of makes the conversion's numbers)
        const uint8_t* op;
        uint64_t indirect;
        int32_t max, stride;
    };
    const uint8_t* op = nullptr;
    Next next[kMax];
    int n = 0;
};

/// The records copied ahead: the run's draws after its first, in order, each with its ring entry
struct Table {
    struct Entry {
        Draw d;
        uint64_t at = 0;
    };
    Entry e[kMax];
    int n = 0, i = 0;   // (entries, and the first not taken)
    void clear() { n = i = 0; }
    bool holds(const uint8_t* op) const {
        for (int k = i; k < n; k++)
            if (e[k].d.op == op) return true;
        return false;
    }
    /// The ring entry copied for d (its op, and the same records): true, and the entries before it are let go (draws the replay did not
    /// draw); false when there is none or it does not match, and the table is cleared
    bool take(const Draw& d, uint64_t* at) {
        for (int k = i; k < n; k++)
            if (e[k].d.op == d.op) {
                if (!same(e[k].d, d)) break;
                *at = e[k].at;
                i = k + 1;
                return true;
            }
        clear();
        return false;
    }
};

inline thread_local Pending t_pending;
inline thread_local Table t_table;

} // namespace mkwvk::mesh_ahead
