// spv_fog::find and keep_froxels (gpu/vk/spv_fog.h): the volumetric fog's first pass, found by its shape in authored SPIR-V
// as the translation writes it (NX2navi's, and Triforce's with the comparison carried by a bool OpPhi), and modules of nearly
// that shape left alone. tests/test_spv_fog.py
#include "../gpu/vk/spv_fog.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

namespace {

uint32_t bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// ids: 2 bool, 3 float, 4 uint, 5 uvec4, 6 vec4, 7 void, 8 function type; 10 0u, 11 1.0f's bits, 12 1/255's bits, 15 2.0f;
// 13 true, 14 false; 30, 31 the froxel's distance and the table's depth, 32 a flag, 33 another float, 50 the image, 51 the
// coordinates (left undefined: the matcher never looks at them)
enum : uint32_t { kBool = 2, kUint = 4, kUvec4 = 5, kVec4 = 6, kZero = 10, kOne = 11, kMask = 12, kTrue = 13, kTwo = 15 };

struct Module {
    std::vector<uint32_t> w{0x07230203u, 0x00010500u, 0, 1000, 0};
    uint32_t next = 600;   // (ids made as needed)
    void op(uint16_t code, std::initializer_list<uint32_t> args) {
        w.push_back((uint32_t)(args.size() + 1) << 16 | code);
        for (uint32_t a : args) w.push_back(a);
    }
    explicit Module(uint32_t model = 5) {
        op(17, {1});                                  // OpCapability Shader
        op(14, {0, 1});                               // OpMemoryModel
        op(15, {model, 1, 0x6e69616du, 0});           // OpEntryPoint <model> %1 "main"
        op(20, {kBool});                              // OpTypeBool
        op(22, {3, 32});                              // OpTypeFloat
        op(21, {kUint, 32, 0});                       // OpTypeInt
        op(23, {kUvec4, kUint, 4});                   // OpTypeVector
        op(23, {kVec4, 3, 4});
        op(19, {7});                                  // OpTypeVoid
        op(33, {8, 7});                               // OpTypeFunction
        op(43, {kUint, kZero, 0});                    // OpConstant
        op(43, {kUint, kOne, bits(1.0f)});
        op(43, {kUint, kMask, bits(1.0f / 255.0f)});
        op(43, {kUint, kTwo, bits(2.0f)});
        op(41, {kBool, kTrue});                       // OpConstantTrue
        op(42, {kBool, 14});                          // OpConstantFalse
        op(54, {7, 1, 0, 8});                         // OpFunction
        op(248, {100});                               // OpLabel
    }
    void end() { op(56, {}); }                        // OpFunctionEnd
    void label(uint32_t id) { op(248, {id}); }
    void branch(uint32_t id) { op(249, {id}); }
    void ret() { op(253, {}); }
    /// An image write of (a, b, c, d) (uint constants' ids), its colour bitcast to a float vector as for a float image
    void write(uint32_t a, uint32_t b, uint32_t c, uint32_t d, bool bitcast = true) {
        const uint32_t composite = next++;
        op(80, {kUvec4, composite, a, b, c, d});      // OpCompositeConstruct
        uint32_t texel = composite;
        if (bitcast) texel = next++, op(124, {kVec4, texel, composite});   // OpBitcast
        op(99, {50, 51, texel});                       // OpImageWrite
    }
    /// The skipped froxel's exit at label id: four writes of (0,0,0,1), then the mask (1/255,0,0,0), and a return
    void exit(uint32_t id, bool through_return_block = true, int writes = 5, uint32_t mask = kMask) {
        label(id);
        for (int i = 0; i < writes; i++) {
            if (i == writes - 1 && writes >= 5) write(mask, kZero, kZero, kZero);
            else write(kZero, kZero, kZero, kOne);
        }
        if (through_return_block) {
            const uint32_t r = next++;
            branch(r), label(r);
        }
        ret();
    }
    /// The froxel computed at label id (a write of something else), then a return
    void compute(uint32_t id) {
        label(id);
        write(kTwo, kTwo, kTwo, kOne);
        const uint32_t r = next++;
        branch(r), label(r), ret();
    }
    void gt(uint32_t id, uint32_t a = 30, uint32_t b = 31, uint16_t code = 186) { op(code, {kBool, id, a, b}); }
    void logical_not(uint32_t id, uint32_t a) { op(168, {kBool, id, a}); }
    void logical_or(uint32_t id, uint32_t a, uint32_t b) { op(166, {kBool, id, a, b}); }
    void logical_and(uint32_t id, uint32_t a, uint32_t b) { op(167, {kBool, id, a, b}); }
    void if_(uint32_t cond, uint32_t then, uint32_t merge, bool merge_op = true) {
        if (merge_op) op(247, {merge, 0});            // OpSelectionMerge
        op(250, {cond, then, merge});                 // OpBranchConditional
    }
    spv_fog::Result find() const { return spv_fog::find(w.data(), w.size()); }
};

/// NX2navi's shape: if (!(distance > table) || flag) { the froxel } else { its exit }
Module game_shape(bool through_return_block = true) {
    Module m;
    m.gt(40);
    m.logical_not(41, 40);
    m.logical_or(42, 41, 32);
    m.if_(42, 101, 102);
    m.compute(101);
    m.exit(102, through_return_block);
    m.end();
    return m;
}

/// The other way round, as the translation may write a forward branch: if (distance > table && !flag) { its exit }, the froxel
/// after it, the exit reached through a block that only branches on
Module body_shape() {
    Module m;
    m.gt(40);
    m.logical_not(41, 32);
    m.logical_and(42, 40, 41);
    m.if_(42, 101, 102);
    m.label(101), m.branch(103);
    m.exit(103);
    m.compute(102);
    m.end();
    return m;
}

/// Triforce's shape: the comparison made in a block that only some invocations enter (202), and carried to the branch by a
/// bool OpPhi (46) whose other value, round, comes from the block that went round it (201, on its branch's false side, whose
/// condition is (flag || !first)); the branch: if (first || !phi) { the froxel } else { its exit, the merge }. first (43) is a
/// phi of constants made the same way one block before. lookup_after: the comparison's block put after the phi's (a back
/// edge, as a loop's); read: an id an OpSelect in the froxel's block reads too (0: none)
Module phi_shape(uint32_t round = 32, bool lookup_after = false, uint32_t read = 0) {
    Module m;
    m.logical_not(41, 32), m.if_(41, 200, 201);                 // (in 100: round the first block on the flag)
    m.label(200), m.branch(201);
    m.label(201);
    m.op(245, {kBool, 43, 14, 100, kTrue, 200});                // OpPhi: first = (false from 100, true from 200)
    m.logical_not(44, 43), m.logical_or(45, 32, 44), m.if_(45, 202, 203);
    auto lookup = [&] { m.label(202), m.gt(40), m.branch(203); };
    if (!lookup_after) lookup();
    m.label(203);
    m.op(245, {kBool, 46, round, 201, 40, 202});                // OpPhi: (round from 201, the comparison from 202)
    m.logical_not(47, 46), m.logical_or(48, 43, 47), m.if_(48, 101, 102);
    if (read) {
        m.label(101);
        m.op(169, {kUint, 60, read, kTwo, kZero});              // OpSelect on it, in the froxel's block
        m.write(60, kTwo, kTwo, kOne);
        m.ret();
    } else {
        m.compute(101);
    }
    m.exit(102);
    if (lookup_after) lookup();
    m.end();
    return m;
}

size_t word_of(const Module& m, uint16_t code, uint32_t result) {
    for (size_t i = 5; i < m.w.size(); i += m.w[i] >> 16)
        if ((m.w[i] & 0xffffu) == code && m.w[i] >> 16 >= 3 && m.w[i + 2] == result) return i;
    return 0;
}

void expect_none(const Module& m, const char* what) {
    const spv_fog::Result r = m.find();
    expect(r.compare == 0, what);
    expect(spv_fog::keep_froxels(m.w.data(), m.w.size()).empty(), what);
}

} // namespace

int main() {
    {   // the game's shape: the comparison made a > a, nothing else changed
        const Module m = game_shape();
        const spv_fog::Result r = m.find();
        const size_t at = word_of(m, 186, 40);
        expect(at != 0 && r.compare == at, "the fog's first pass: its comparison found");
        expect(r.exits == 1 && r.branches == 1, "one exit, one branch");
        const std::vector<uint32_t> o = spv_fog::keep_froxels(m.w.data(), m.w.size());
        expect(o.size() == m.w.size(), "the patched module is the same length");
        size_t differ = 0;
        for (size_t i = 0; i < o.size() && o.size() == m.w.size(); i++) differ += o[i] != m.w[i];
        expect(differ == 1 && o[at + 3] == 30 && o[at + 4] == 30, "one word changed: distance > distance, never true");
        expect(spv_fog::find(o.data(), o.size()).compare == 0, "the patched module does not match again");
    }
    {   // the exit returning at once, without a block of its own for the return
        const Module m = game_shape(false);
        expect(m.find().compare == word_of(m, 186, 40), "an exit that returns at once");
    }
    {   // the exit as the branch's body, reached through a block that only branches on
        const Module m = body_shape();
        expect(m.find().compare == word_of(m, 186, 40), "an exit taken on (distance > table && !flag), through a forwarding block");
    }
    {   // the colour written as it is, an integer image (no bitcast)
        Module m;
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.compute(101);
        m.label(102);
        for (int i = 0; i < 4; i++) m.write(kZero, kZero, kZero, kOne, false);
        m.write(kMask, kZero, kZero, kZero, false);
        m.ret();
        m.end();
        expect(m.find().compare == word_of(m, 186, 40), "an exit whose colours are not bitcast");
    }
    {   // Triforce's shape: the comparison carried by a phi, the value round its block known false on its edge
        const Module m = phi_shape();
        const spv_fog::Result r = m.find();
        const size_t at = word_of(m, 186, 40);
        expect(at != 0 && r.compare == at && r.exits == 1 && r.branches == 1 && r.shared == 0,
               "Triforce's shape: the comparison through the phi found");
        const std::vector<uint32_t> o = spv_fog::keep_froxels(m.w.data(), m.w.size());
        size_t differ = 0;
        for (size_t i = 0; i < o.size() && o.size() == m.w.size(); i++) differ += o[i] != m.w[i];
        expect(o.size() == m.w.size() && differ == 1 && o[at + 3] == 30 && o[at + 4] == 30,
               "Triforce's shape: one word changed, distance > distance");
        expect(spv_fog::find(o.data(), o.size()).compare == 0, "Triforce's shape: the patched module does not match again");
    }
    {   // the value round the lookup known through an OpLogicalAnd taken on true: (flag && first) to the merge, !flag round it
        Module m;
        m.logical_not(41, 32), m.if_(41, 200, 201);
        m.label(200), m.branch(201);
        m.label(201);
        m.op(245, {kBool, 43, 14, 100, kTrue, 200});
        m.logical_not(49, 32), m.logical_and(45, 32, 43);
        m.op(247, {203, 0}), m.op(250, {45, 203, 202});          // (true: to the merge, round the lookup)
        m.label(202), m.gt(40), m.branch(203);
        m.label(203);
        m.op(245, {kBool, 46, 49, 201, 40, 202});
        m.logical_not(47, 46), m.logical_or(48, 43, 47), m.if_(48, 101, 102);
        m.compute(101), m.exit(102), m.end();
        expect(m.find().compare == word_of(m, 186, 40), "a phi whose value round the lookup an OpLogicalAnd's true side decides");
    }
    // ---- near misses: nothing changed ----
    {   // the value round the lookup not known on its edge: with the comparison false the skip may still be taken
        const Module m = phi_shape(34);
        const spv_fog::Result r = m.find();
        expect_none(m, "a phi whose other value is not known");
        expect(r.exits == 1 && r.branches == 0 && r.shared == 0, "an unknown value round the lookup: the exit found alone");
    }
    {   // the value round the lookup known true on its edge: the skip taken there whatever the comparison
        const Module m = phi_shape(kTrue);
        expect_none(m, "a phi whose other value is true");
    }
    {   // a phi of a block after its own (a loop's back edge): its values are not followed
        const Module m = phi_shape(32, true);
        const spv_fog::Result r = m.find();
        expect_none(m, "a phi on a back edge");
        expect(r.exits == 1 && r.branches == 0, "a back edge's phi: no comparison found");
    }
    {   // a single-block loop (203, its own continue target) whose phi takes the comparison from its own block's branch back to
        // itself: its label is before the phi, but that edge is a back edge all the same, so its values are not followed
        Module m;
        m.logical_not(41, 32), m.if_(41, 200, 201);
        m.label(200), m.branch(201);
        m.label(201);
        m.op(245, {kBool, 43, 14, 100, kTrue, 200});
        m.logical_not(44, 43), m.logical_or(45, 32, 44);
        m.op(247, {204, 0}), m.op(250, {45, 204, 203});          // (false, round known false on that edge: into the loop)
        m.label(203);
        m.op(245, {kBool, 46, 32, 201, 40, 203});                // OpPhi: (round from 201, the comparison from 203 itself)
        m.gt(40);
        m.op(246, {204, 203, 0}), m.op(250, {57, 203, 204});     // OpLoopMerge, then back to 203 or out
        m.label(204);
        m.logical_not(47, 46), m.logical_or(48, 43, 47), m.if_(48, 101, 102);
        m.compute(101), m.exit(102), m.end();
        const spv_fog::Result r = m.find();
        expect_none(m, "a phi on a single-block loop's back edge");
        expect(r.exits == 1 && r.branches == 0 && r.shared == 0, "a self-looping block's phi: no comparison found");
    }
    {   // the phi read again past the branch: the patch would change that reader too
        const Module m = phi_shape(32, false, 46);
        const spv_fog::Result r = m.find();
        expect_none(m, "a phi an OpSelect reads too");
        expect(r.exits == 1 && r.branches == 0 && r.shared == 1, "the phi read elsewhere: counted as shared");
    }
    {   // the comparison read by the phi and by an OpSelect
        const Module m = phi_shape(32, false, 40);
        const spv_fog::Result r = m.find();
        expect_none(m, "a comparison a phi and an OpSelect read");
        expect(r.shared == 1, "the comparison read past its phi: counted as shared");
    }
    {   // two such branches, one through a phi and one as NX2navi's: neither patched
        Module m = phi_shape();
        m.w.pop_back();   // (the OpFunctionEnd, for one more branch in the function)
        m.label(110);
        m.gt(54, 33, 31), m.logical_not(55, 54), m.logical_or(56, 55, 32), m.if_(56, 111, 112), m.compute(111), m.exit(112);
        m.end();
        const spv_fog::Result r = m.find();
        expect_none(m, "two such branches, one through a phi");
        expect(r.branches == 2, "both counted");
    }
    {
        Module m(4);   // (a fragment shader of the same shape)
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.compute(101), m.exit(102), m.end();
        expect_none(m, "a fragment shader");
    }
    {
        Module m;
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.compute(101), m.exit(102, true, 5, kOne);
        m.end();
        expect_none(m, "an exit whose last write is (1,0,0,0), not the mask");
        expect(m.find().exits == 0, "no exit found there");
    }
    {
        Module m;
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.compute(101);
        m.label(102);
        m.write(kMask, kZero, kZero, kZero);
        for (int i = 0; i < 4; i++) m.write(kZero, kZero, kZero, kOne);
        m.ret(), m.end();
        expect_none(m, "the mask written first");
    }
    {
        Module m;
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.compute(101), m.exit(102, true, 4);
        m.end();
        expect_none(m, "an exit of four writes");
    }
    {
        Module m;
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.compute(101);
        m.label(102);
        for (int i = 0; i < 5; i++) m.write(kZero, kZero, kZero, kOne);
        m.write(kMask, kZero, kZero, kZero);
        m.ret(), m.end();
        expect_none(m, "an exit of six writes");
    }
    {
        Module m;
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.compute(101);
        m.label(102);
        for (int i = 0; i < 4; i++) m.write(kZero, kZero, kZero, kOne);
        m.write(kMask, kZero, kZero, kZero);
        m.branch(101);   // (on to a block that does more than return)
        m.end();
        expect_none(m, "an exit that goes on to other work");
    }
    {
        Module m;
        m.gt(40, 30, 31, 184);   // (OpFOrdLessThan)
        m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.compute(101), m.exit(102), m.end();
        const spv_fog::Result r = m.find();
        expect_none(m, "a less-than comparison");
        expect(r.exits == 1 && r.branches == 0, "a less-than: the exit found, not its comparison (the log's near miss)");
    }
    {
        Module m;
        m.gt(40), m.logical_or(42, 40, 32), m.if_(42, 101, 102), m.compute(101), m.exit(102), m.end();
        expect_none(m, "(distance > table || flag) to the froxel: the comparison false does not keep the branch off the exit");
    }
    {
        Module m;
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, kTrue), m.if_(42, 101, 102), m.compute(101), m.exit(102), m.end();
        expect_none(m, "a branch the comparison does not decide (!(distance > table) || true)");
    }
    {
        Module m;
        m.gt(40), m.gt(43, 33, 31), m.logical_not(41, 40), m.logical_or(42, 41, 43), m.if_(42, 101, 102), m.compute(101);
        m.exit(102), m.end();
        expect_none(m, "two comparisons in the condition");
    }
    {
        Module m;
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102, false), m.compute(101), m.exit(102), m.end();
        expect_none(m, "a branch with no selection merge (a loop's break)");
    }
    {
        Module m;
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.exit(101), m.exit(102), m.end();
        expect_none(m, "both sides of the branch the exit");
    }
    {
        Module m;
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.compute(101), m.exit(102);
        m.label(110);
        m.gt(44, 33, 31), m.logical_not(45, 44), m.logical_or(46, 45, 32), m.if_(46, 111, 112), m.compute(111), m.exit(112);
        m.end();
        const spv_fog::Result r = m.find();
        expect_none(m, "two such branches");
        expect(r.branches == 2, "both counted (the log's 'more than once')");
    }
    {   // the comparison read again past the branch (a guest predicate kept as one value): a > a would change that too
        Module m;
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102);
        m.label(101);
        m.op(169, {kUint, 60, 40, kTwo, kZero});   // OpSelect on the comparison, in the froxel's block
        m.write(60, kTwo, kTwo, kOne);
        m.ret();
        m.exit(102), m.end();
        const spv_fog::Result r = m.find();
        expect_none(m, "a comparison an OpSelect reads too");
        expect(r.exits == 1 && r.branches == 0 && r.shared == 1, "the comparison read elsewhere: a near miss of its own (the log)");
    }
    {   // two such branches, one whose comparison an OpSelect reads too: which is the fog's skip cannot be told, so neither is
        // patched (the clean one is not patched on its own)
        Module m;
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.compute(101), m.exit(102);
        m.label(110);
        m.gt(44, 33, 31), m.logical_not(45, 44), m.logical_or(46, 45, 32), m.if_(46, 111, 112);
        m.label(111);
        m.op(169, {kUint, 61, 44, kTwo, kZero});   // OpSelect on the second comparison, in its froxel's block
        m.write(61, kTwo, kTwo, kOne);
        m.ret();
        m.exit(112), m.end();
        const spv_fog::Result r = m.find();
        expect_none(m, "two such branches, one of them with its comparison read elsewhere");
        expect(r.exits == 2 && r.branches == 1 && r.shared == 1, "one clean, one shared: both counted (the log's 'more than once')");
    }
    {   // a term made of the comparison read elsewhere (here the OpLogicalNot by another branch's condition)
        Module m;
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.compute(101), m.exit(102);
        m.label(110);
        m.if_(41, 111, 112), m.compute(111), m.compute(112);
        m.end();
        expect_none(m, "a condition whose part made of the comparison another branch reads");
        expect(m.find().shared == 1, "counted as read elsewhere");
    }
    {   // another term of the condition read elsewhere: its value does not depend on the comparison, so the patch holds
        Module m;
        m.logical_not(32, 34);   // (the flag, made of a value of no comparison, read by a second branch too)
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.compute(101), m.exit(102);
        m.label(110);
        m.if_(32, 111, 112), m.compute(111), m.compute(112);
        m.end();
        const spv_fog::Result r = m.find();
        expect(r.compare == word_of(m, 186, 40) && r.shared == 0, "the other term read elsewhere: the comparison still found");
    }
    {
        Module m;   // (no exit at all: a compute shader with a comparison and a branch)
        m.gt(40), m.logical_not(41, 40), m.logical_or(42, 41, 32), m.if_(42, 101, 102), m.compute(101), m.compute(102), m.end();
        expect_none(m, "a compute shader with no exit of the five writes");
        expect(m.find().exits == 0, "no exit counted");
    }
    {   // modules that are not modules: no crash, nothing found
        expect(spv_fog::find(nullptr, 0).compare == 0, "no words");
        const uint32_t bad[] = {0x07230203u, 0x00010500u, 0, 1000, 0, 0x00000011u};   // (an instruction of no words)
        expect(spv_fog::find(bad, 6).compare == 0, "an instruction of length 0");
        Module m = game_shape();
        m.w.resize(m.w.size() - 3);   // (cut short)
        expect(spv_fog::find(m.w.data(), m.w.size()).compare == 0, "a module cut short");
        Module huge = game_shape();
        huge.w[3] = 0x7fffffffu;      // (a bound no module of this size has)
        expect(spv_fog::find(huge.w.data(), huge.w.size()).compare == 0, "an absurd id bound");
    }
    {   // RX_VK_FOG_CULL
        expect(!spv_fog::cull_kept(nullptr) && !spv_fog::cull_kept("") && !spv_fog::cull_kept("0"), "unset, empty or 0: patched");
        expect(spv_fog::cull_kept("1"), "1: the game's skip kept");
    }
    if (failures) return 1;
    std::printf("spv_fog: ok\n");
    return 0;
}
