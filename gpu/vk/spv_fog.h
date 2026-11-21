// (Triforce, after NX2navi's 0379b00) The volumetric fog's dark trails. The fog's first compute pass skips a froxel that lies
// behind a coarse depth table and writes it as empty, and the next pass's mask keeps it empty. That table comes from the frame
// before, so in the columns a near object has just left, every froxel past the object's old depth came out with no fog: a dark
// block of the fog grid beside the object, which the fog's history then smeared into a trail, worst at the screen edges, where
// near objects move fastest. A froxel behind today's depth is never seen, so computing it only costs a little GPU time: the
// comparison is made never true (x > x, false for every x, NaN too), as NX2navi does.
//
// Found by its shape in the module's SPIR-V, not by a hash, and nothing else is touched: a compute module (its one entry
// point GLCompute) with a block that makes five image writes, four of (0,0,0,1) and last the mask value (1/255,0,0,0), and
// then returns (OpReturn, or a branch to a block that only returns); one structured branch (OpSelectionMerge, then
// OpBranchConditional) whose one side leads there, directly or through blocks that only branch on; and in its condition,
// through the logical operations alone (and a bool OpPhi of forward edges: see below), one float comparison, an ordered
// greater-than, which decides it: with the comparison false the branch never takes that side, whatever the rest of the
// condition holds, and with it true it may; and that
// comparison and what the condition makes of it read by nothing but the branch (a predicate the guest reads again elsewhere
// would change there too). Exactly one such branch in the module, or nothing is changed: a branch of that shape whose
// comparison is read elsewhere counts as one too (the matcher cannot tell which of two is the fog's skip, so with one shared
// and one not, neither is patched).
//
// Triforce's translator keeps the guest's predicate across the table's lookup as a bool OpPhi in the block after it: the
// comparison is made in the lookup's block, which only some froxels enter, and the phi takes it from there and another
// value (a flag of the guest's) from the block that went round it; the skip's condition reads the phi. A phi counts as a
// logical operation when every block it takes a value from comes before its own (no loop's back edge), and its value is
// worked out on each edge on its own, with what that edge's branch says of its condition (taken on false through an
// OpLogicalOr: both its terms false; on true through an OpLogicalAnd: both true; through an OpLogicalNot: the other way):
// so the value that comes round the lookup, false whenever the block before took that way, is known on its edge. The phi
// is part of the condition's tree, so it must be read by nothing but the condition too. Nothing more is looked at than a
// few blocks' terminators and the condition's own tree, within a step budget (a branch over it is left alone).
//
// vk_shader.cpp make_module applies it to the module it hands the driver, outside the translation stamp (program_carry.h):
// the program cache's records keep the translation as it was made, so no program is translated again for it, and a program
// read back from the cache is patched the same way as one just translated. Only that one compute pipeline is new to the
// driver's caches. RX_VK_FOG_CULL=1: the game's skip kept. tests/test_spv_fog.py
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace spv_fog {

enum : uint32_t {
    OpEntryPoint = 15, OpFunctionEnd = 56, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpConstantComposite = 44,
    OpCompositeConstruct = 80, OpImageWrite = 99, OpBitcast = 124, OpLogicalEqual = 164, OpLogicalNotEqual = 165,
    OpLogicalOr = 166, OpLogicalAnd = 167, OpLogicalNot = 168, OpSelect = 169, OpFOrdEqual = 180, OpFOrdGreaterThan = 186,
    OpFUnordGreaterThanEqual = 191, OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249,
    OpBranchConditional = 250, OpSwitch = 251, OpKill = 252, OpReturn = 253, OpReturnValue = 254, OpUnreachable = 255,
    OpTerminateInvocation = 4416, OpPhi = 245, OpFunction = 54,
};
constexpr uint32_t kGLCompute = 5;

/// RX_VK_FOG_CULL: set to a number other than 0, the game's skip is kept
inline bool cull_kept(const char* env) { return env && *env && std::atoi(env) != 0; }

/// The matcher's version, in the light precompile's record of the compute programs built (vk_draw.cpp built_computes_key):
/// raised whenever find matches modules it did not before (or no longer matches one), so a record written by an earlier
/// matcher, whose fog module the driver was then handed unpatched, builds the compute programs once more before the game.
/// 1: c209c48 to af843c8's parent, which matched none of Triforce's fog modules; 2: through the bool phi (af843c8)
constexpr uint64_t kMatcher = 2;

struct Result {
    size_t compare = 0;   // (the word of the OpFOrdGreaterThan to make never true; 0: not that shader)
    int exits = 0;        // (blocks of the five writes found: one with no compare is a near miss, for the log)
    int branches = 0;     // (branches that match: one is patched when it is the only branch of that shape, shared ones counted)
    int shared = 0;       // (branches that would match but whose condition is read elsewhere too: never patched, and with one
                          // that matches, neither is: the log's "more than once")
};

namespace detail {

struct Module {
    const uint32_t* w;
    size_t n;
    uint32_t bound;
    std::vector<uint32_t> def;   // (the word of each id's definition, for the instructions looked at; 0: none)
    std::vector<uint32_t> blk;   // (the label of the block each OpPhi is in, by its result)
    mutable uint32_t steps = 0;  // (the work done for one branch: over kSteps, that branch is left alone)
    static constexpr uint32_t kSteps = 1u << 20;
    bool spent() const { return ++steps > kSteps; }

    uint32_t op(size_t i) const { return w[i] & 0xffffu; }
    uint32_t len(size_t i) const { return w[i] >> 16; }
    size_t at(uint32_t id) const { return id < bound ? def[id] : 0; }

    /// The float of a 32-bit OpConstant, false when id is not one
    bool constant(uint32_t id, float* out) const {
        const size_t d = at(id);
        if (!d || op(d) != OpConstant || len(d) != 4) return false;
        std::memcpy(out, &w[d + 3], 4);
        return true;
    }
    /// The four values a texel id is made of (through one OpBitcast, as the translation writes a float image)
    bool texel(uint32_t id, float v[4]) const {
        size_t d = at(id);
        if (d && op(d) == OpBitcast && len(d) == 4) d = at(w[d + 3]);
        if (!d || (op(d) != OpCompositeConstruct && op(d) != OpConstantComposite) || len(d) != 7) return false;
        for (int k = 0; k < 4; k++)
            if (!constant(w[d + 3 + k], &v[k])) return false;
        return true;
    }
    /// Whether the block of label id only returns
    bool returns(uint32_t id) const {
        const size_t d = at(id);
        if (!d || op(d) != OpLabel) return false;
        const size_t i = d + len(d);
        return i < n && op(i) == OpReturn;
    }
    /// The block a branch to label id ends up in, through blocks that only branch on (a few at most)
    uint32_t lead(uint32_t id) const {
        for (int hop = 0; hop < 4; hop++) {
            const size_t d = at(id);
            if (!d || op(d) != OpLabel) return id;
            const size_t i = d + len(d);
            if (i >= n || op(i) != OpBranch || len(i) != 2) return id;
            id = w[i + 1];
        }
        return id;
    }
    /// Whether the OpPhi at word d takes each of its values from a block before its own: no loop's back edge, whose value
    /// would be another iteration's. Each parent's label comes before the phi's own block's label, so its own block is not
    /// one of them either (a single-block loop, its own continue target, branches back to itself)
    bool forward_phi(size_t d) const {
        const uint32_t l = len(d);
        if (op(d) != OpPhi || l < 5 || (l - 3) % 2 || w[d + 2] >= bound) return false;
        const size_t own = at(blk[w[d + 2]]);
        if (!own || op(own) != OpLabel || own > d) return false;
        for (uint32_t k = 4; k < l; k += 2) {
            const size_t p = at(w[d + k]);
            if (!p || op(p) != OpLabel || p >= own) return false;
        }
        return true;
    }
    /// The word of the terminator of the block of label id, 0 when none is found
    size_t end_of(uint32_t id) const {
        const size_t d = at(id);
        if (!d || op(d) != OpLabel) return 0;
        for (size_t i = d + len(d); i < n && len(i) && !spent(); i += len(i)) switch (op(i)) {
            case OpBranch: case OpBranchConditional: case OpSwitch: case OpReturn: case OpReturnValue: case OpKill:
            case OpUnreachable: case OpTerminateInvocation:
                return i;
            case OpLabel: case OpFunctionEnd: return 0;
            default: break;
            }
        return 0;
    }
    /// A value known on one edge: id is v (0 or 1)
    struct Fact {
        uint32_t id;
        int v;
    };
    /// What a bool id's value v says of what it is made of: through an OpLogicalNot the other way, an OpLogicalOr false both
    /// its terms false, an OpLogicalAnd true both true (each fact true wherever id has that value)
    void derive(uint32_t id, int v, std::vector<Fact>& f, int depth = 0) const {
        if (depth > 32 || spent()) return;
        f.push_back({id, v});
        const size_t d = at(id);
        if (!d) return;
        const uint32_t o = op(d), l = len(d);
        if (o == OpLogicalNot && l == 4) derive(w[d + 3], !v, f, depth + 1);
        else if (l == 5 && ((o == OpLogicalOr && v == 0) || (o == OpLogicalAnd && v == 1)))
            derive(w[d + 3], v, f, depth + 1), derive(w[d + 4], v, f, depth + 1);
    }
    /// What is known on the edge from the block of label from to the block of label to: from's branch's condition, when
    /// that branch is conditional and only one of its sides is to
    std::vector<Fact> edge(uint32_t from, uint32_t to) const {
        std::vector<Fact> f;
        const size_t t = end_of(from);
        if (!t || op(t) != OpBranchConditional || len(t) < 4) return f;
        const bool on_true = w[t + 2] == to, on_false = w[t + 3] == to;
        if (on_true != on_false) derive(w[t + 1], on_true ? 1 : 0, f);
        return f;
    }
    /// Whether the block of label id is the skipped froxel's exit: five image writes, four of (0,0,0,1) and last
    /// (1/255,0,0,0), then a return, with no other branch in it
    bool exit(uint32_t id) const {
        const size_t d = at(id);
        if (!d || op(d) != OpLabel) return false;
        int writes = 0;
        for (size_t i = d + len(d); i < n;) {
            const uint32_t o = op(i), l = len(i);
            if (!l || i + l > n) return false;
            switch (o) {
            case OpImageWrite: {
                float v[4];
                if (l < 4 || ++writes > 5 || !texel(w[i + 3], v)) return false;
                const bool ok = writes == 5 ? v[0] > 0.0039f && v[0] < 0.0040f && v[1] == 0 && v[2] == 0 && v[3] == 0
                                            : v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 1;
                if (!ok) return false;
                break;
            }
            case OpReturn: return writes == 5;
            case OpBranch: return writes == 5 && l == 2 && returns(w[i + 1]);
            case OpLabel: case OpBranchConditional: case OpSwitch: case OpKill: case OpReturnValue: case OpUnreachable:
            case OpSelectionMerge: case OpLoopMerge: case OpTerminateInvocation: case OpFunctionEnd:
                return false;
            default: break;
            }
            i += l;
        }
        return false;
    }
    static bool compare_op(uint32_t o) { return o >= OpFOrdEqual && o <= OpFUnordGreaterThanEqual; }
    /// An a > a, as the patch leaves it: false for every a, so not a comparison of the condition (a module patched once does
    /// not match again)
    bool never(size_t d) const { return op(d) == OpFOrdGreaterThan && len(d) == 5 && w[d + 3] == w[d + 4]; }
    static bool logical_op(uint32_t o) { return (o >= OpLogicalEqual && o <= OpSelect); }
    /// A logical operation, or a bool OpPhi of forward edges (one that takes a comparison or a logical value is bool)
    bool logical_at(size_t d) const { return logical_op(op(d)) || forward_phi(d); }
    /// The float comparisons a condition is made of through the logical operations (each once); false: too deep
    bool compares(uint32_t id, std::vector<uint32_t>& out, int depth = 0) const {
        if (depth > 32 || spent()) return false;
        const size_t d = at(id);
        if (!d) return true;
        const uint32_t o = op(d), l = len(d);
        if (never(d)) return true;
        if (compare_op(o)) {
            for (uint32_t c : out)
                if (c == id) return true;
            out.push_back(id);
            return true;
        }
        if (forward_phi(d)) {   // (its values; the labels after them are not)
            for (uint32_t k = 3; k < l; k += 2)
                if (!compares(w[d + k], out, depth + 1)) return false;
            return true;
        }
        if (!logical_op(o)) return true;
        const uint32_t args = o == OpLogicalNot ? 1u : o == OpSelect ? 3u : 2u;
        if (l != 3 + args) return false;
        for (uint32_t k = 0; k < args; k++)
            if (!compares(w[d + 3 + k], out, depth + 1)) return false;
        return true;
    }
    /// The ids of a condition whose value the comparison gt decides a part of: gt, and the logical operations made of it on the
    /// way to the condition (the other terms keep their value whatever is done to gt). True: id is one of them
    bool tree(uint32_t id, uint32_t gt, std::vector<uint32_t>& out, int depth = 0) const {
        if (id == gt) {
            for (uint32_t c : out)
                if (c == id) return true;
            out.push_back(id);
            return true;
        }
        const size_t d = at(id);
        if (!d || depth > 32 || spent() || !logical_at(d)) return false;
        const uint32_t step = op(d) == OpPhi ? 2 : 1;   // (a phi's values, not its labels)
        bool made_of = false;
        for (uint32_t k = 3; k < len(d); k += step) made_of = tree(w[d + k], gt, out, depth + 1) || made_of;
        if (!made_of) return false;
        for (uint32_t c : out)
            if (c == id) return true;
        out.push_back(id);
        return true;
    }
    /// Whether the condition's ids are read by nothing but one another and the branch at word branch: a comparison made never
    /// true changes every reader of it and of what is made of it, so another reader (a select after the merge, a store, another
    /// branch) would change too. Any word of an instruction in the functions that names one of them counts as a read, a literal
    /// that happens to be one too (left alone, then)
    bool read_alone(const std::vector<uint32_t>& ids, size_t branch) const {
        size_t i = 5;
        while (i < n && op(i) != OpFunction) i += len(i);   // (from the first OpFunction: nothing before it can read a function's ids)
        for (; i < n; i += len(i)) {
            const uint32_t o = op(i), l = len(i);
            if (!l) return false;
            if (i == branch) continue;
            if ((compare_op(o) || logical_op(o) || o == OpPhi) && l >= 3) {
                bool part = false;
                for (uint32_t c : ids) part = part || c == w[i + 2];
                if (part) continue;
            }
            for (uint32_t k = 1; k < l; k++)
                for (uint32_t c : ids)
                    if (w[i + k] == c) return false;
        }
        return true;
    }
    /// A condition's value with the comparison gt at value g: 0, 1, or 2 when it depends on what else it is made of. f: what
    /// is known on the edge it is looked at for (a phi's value, on its edge)
    int eval(uint32_t id, uint32_t gt, int g, const std::vector<Fact>* f = nullptr, int depth = 0) const {
        if (id == gt) return g;
        if (f)
            for (const Fact& x : *f)
                if (x.id == id) return x.v;
        const size_t d = at(id);
        if (!d || depth > 32 || spent()) return 2;
        if (never(d)) return 0;
        const uint32_t o = op(d), l = len(d);
        switch (o) {
        case OpPhi: {   // (each edge on its own, with only what that edge's branch says: one value for all of them, or 2)
            if (!forward_phi(d) || !blk[id]) return 2;
            int out = -1;
            for (uint32_t k = 3; k + 1 < l; k += 2) {
                const std::vector<Fact> e = edge(w[d + k + 1], blk[id]);
                const int v = eval(w[d + k], gt, g, &e, depth + 1);
                out = out < 0 || out == v ? v : 2;
                if (out == 2) return 2;
            }
            return out < 0 ? 2 : out;
        }
        case OpConstantTrue: return 1;
        case OpConstantFalse: return 0;
        case OpLogicalNot: {
            if (l != 4) return 2;
            const int a = eval(w[d + 3], gt, g, f, depth + 1);
            return a == 2 ? 2 : !a;
        }
        case OpLogicalOr: case OpLogicalAnd: case OpLogicalEqual: case OpLogicalNotEqual: {
            if (l != 5) return 2;
            const int a = eval(w[d + 3], gt, g, f, depth + 1), b = eval(w[d + 4], gt, g, f, depth + 1);
            if (o == OpLogicalOr) return a == 1 || b == 1 ? 1 : a == 0 && b == 0 ? 0 : 2;
            if (o == OpLogicalAnd) return a == 0 || b == 0 ? 0 : a == 1 && b == 1 ? 1 : 2;
            if (a == 2 || b == 2) return 2;
            return o == OpLogicalEqual ? a == b : a != b;
        }
        case OpSelect: {
            if (l != 6) return 2;
            const int c = eval(w[d + 3], gt, g, f, depth + 1);
            const int a = eval(w[d + 4], gt, g, f, depth + 1), b = eval(w[d + 5], gt, g, f, depth + 1);
            return c == 1 ? a : c == 0 ? b : a == b ? a : 2;
        }
        default: return 2;
        }
    }
};

} // namespace detail

/// The fog's first pass in a module's words, or a result with compare 0 (any other module; it costs a look at the first
/// instructions for a module of another stage)
inline Result find(const uint32_t* w, size_t n) {
    Result r;
    if (!w || n < 6 || w[0] != 0x07230203u) return r;
    const uint32_t bound = w[3];
    if (!bound || bound > 8 * n + 1024) return r;
    int entries = 0, writes = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t o = w[i] & 0xffffu, l = w[i] >> 16;
        if (!l || i + l > n) return r;
        if (o == OpEntryPoint) {
            if (l < 3 || w[i + 1] != kGLCompute) return r;   // (a graphics or mesh stage: no further look)
            entries++;
        }
        writes += o == OpImageWrite;
        i += l;
    }
    if (entries != 1 || writes < 5) return r;
    detail::Module m{w, n, bound, std::vector<uint32_t>(bound, 0), std::vector<uint32_t>(bound, 0)};
    uint32_t label = 0;   // (the block the instructions are in)
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        const uint32_t o = w[i] & 0xffffu, l = w[i] >> 16;
        if (o == OpLabel && l >= 2 && w[i + 1] < bound) m.def[w[i + 1]] = (uint32_t)i, label = w[i + 1];
        else if ((o == OpConstantTrue || o == OpConstantFalse || o == OpConstant || o == OpConstantComposite ||
                  o == OpCompositeConstruct || o == OpBitcast || o == OpPhi || detail::Module::logical_op(o) ||
                  detail::Module::compare_op(o)) &&
                 l >= 3 && w[i + 2] < bound) {
            m.def[w[i + 2]] = (uint32_t)i;
            if (o == OpPhi) m.blk[w[i + 2]] = label;
        }
    }
    for (size_t i = 5; i < n; i += w[i] >> 16)
        if ((w[i] & 0xffffu) == OpLabel && w[i] >> 16 == 2 && m.exit(w[i + 1])) r.exits++;
    if (!r.exits) return r;
    size_t found = 0;
    for (size_t i = 5, prev = 0; i < n; prev = i, i += w[i] >> 16) {
        if ((w[i] & 0xffffu) != OpBranchConditional || w[i] >> 16 < 4 || !prev || (w[prev] & 0xffffu) != OpSelectionMerge) continue;
        const bool on_true = m.exit(m.lead(w[i + 2])), on_false = m.exit(m.lead(w[i + 3]));
        if (on_true == on_false) continue;
        m.steps = 0;
        std::vector<uint32_t> cmps;
        if (!m.compares(w[i + 1], cmps) || cmps.size() != 1) continue;
        const size_t c = m.at(cmps[0]);
        if (m.op(c) != OpFOrdGreaterThan || m.len(c) != 5) continue;
        const int away = on_true ? 0 : 1;   // (the condition's value that keeps the branch off the exit)
        if (m.eval(w[i + 1], cmps[0], 0) != away || m.eval(w[i + 1], cmps[0], 1) == away) continue;
        // (the comparison and the condition made of it read by this branch alone, or the patch would change more than the skip)
        std::vector<uint32_t> ids;
        m.tree(w[i + 1], cmps[0], ids);
        if (m.steps > detail::Module::kSteps) continue;   // (too much to look at: left alone, as no match)
        if (!m.read_alone(ids, i)) {
            r.shared++;
            continue;
        }
        r.branches++;
        found = c;
    }
    // (a shared branch is a candidate too: with one of each, which is the fog's skip cannot be told, so neither is patched)
    if (r.branches == 1 && r.shared == 0) r.compare = found;
    return r;
}

/// The module with the fog's skip never taken (a copy), or empty: not that shader
inline std::vector<uint32_t> keep_froxels(const uint32_t* w, size_t n, Result* out = nullptr) {
    const Result r = find(w, n);
    if (out) *out = r;
    if (!r.compare) return {};
    std::vector<uint32_t> o(w, w + n);
    o[r.compare + 4] = o[r.compare + 3];   // (OpFOrdGreaterThan type, result, a, b: a > a)
    return o;
}

} // namespace spv_fog
