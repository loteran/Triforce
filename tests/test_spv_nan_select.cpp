// spv_nan_select::rewrite (gpu/vk/spv_nan_select.h): float selects of a NaN constant made selects of the bits, on authored
// SPIR-V as the translation writes it; what comes out is read back (each id defined once and below the bound, the new types
// and constants before the first function, every select of a NaN constant an integer one) and evaluated, and gives the same
// bits as the select it replaces for either condition. Modules with nothing to change are left alone.
// tests/test_spv_nan_select.py; with a folder argument, the modules made are written there for spirv-val
#include "../gpu/vk/spv_nan_select.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>

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

// ids: 2 void, 3 function type, 4 bool, 5 float, 6 uint, 7 vec2, 8 float function pointer, 9 half, 10 ushort, 11 half
// function pointer; 20.. constants; 1 main, 100 its label, 101 a float variable, 102 its load (x), 103 the condition,
// 104 a half variable, 105 its load; 110.. the selects
enum : uint32_t {
    kVoid = 2, kFn = 3, kBool = 4, kFloat = 5, kUint = 6, kVec2 = 7, kPtr = 8, kHalf = 9, kUshort = 10, kHptr = 11,
    kNan = 20, kOne = 21, kMinusOne = 22, kInf = 23, kNegNan = 24, kVecNan = 25, kHnan = 26, kHone = 27, kUmask = 28,
    kX = 102, kCond = 103, kHx = 105,
};

struct Options {
    bool uint = true;       // (a 32-bit unsigned type declared)
    bool half = false;      // (16-bit floats, and their selects)
    bool ushort = true;     // (with half: a 16-bit unsigned type declared)
    bool umask = false;     // (an integer constant of the mask's bits already there)
};

struct Module {
    std::vector<uint32_t> w{0x07230203u, 0x00010500u, 0, 1000, 0};
    uint32_t next = 110;
    void op(uint16_t code, std::initializer_list<uint32_t> args) {
        w.push_back((uint32_t)(args.size() + 1) << 16 | code);
        for (uint32_t a : args) w.push_back(a);
    }
    Options o;
    explicit Module(Options opt = {}) : o(opt) {
        op(17, {1});                                  // OpCapability Shader
        if (o.half) op(17, {9}), op(17, {22});        // OpCapability Float16, Int16
        op(14, {0, 1});                               // OpMemoryModel Logical GLSL450
        op(15, {5, 1, 0x6e69616du, 0});               // OpEntryPoint GLCompute %1 "main"
        op(16, {1, 17, 1, 1, 1});                     // OpExecutionMode %1 LocalSize 1 1 1
        op(19, {kVoid});                              // OpTypeVoid
        op(33, {kFn, kVoid});                         // OpTypeFunction
        op(20, {kBool});                              // OpTypeBool
        op(22, {kFloat, 32});                         // OpTypeFloat 32
        if (o.uint) op(21, {kUint, 32, 0});           // OpTypeInt 32 0
        op(23, {kVec2, kFloat, 2});                   // OpTypeVector
        op(32, {kPtr, 7, kFloat});                    // OpTypePointer Function
        if (o.half) {
            op(22, {kHalf, 16});
            if (o.ushort) op(21, {kUshort, 16, 0});
            op(32, {kHptr, 7, kHalf});
        }
        op(43, {kFloat, kNan, 0xffffffffu});          // OpConstant: the mask, typed as a float
        op(43, {kFloat, kOne, bits(1.0f)});
        op(43, {kFloat, kMinusOne, bits(-1.0f)});
        op(43, {kFloat, kInf, 0x7f800000u});          // (infinity: not a NaN)
        op(43, {kFloat, kNegNan, 0xffc00000u});
        op(44, {kVec2, kVecNan, kNan, kOne});         // OpConstantComposite
        if (o.half) op(43, {kHalf, kHnan, 0x7e00u}), op(43, {kHalf, kHone, 0x3c00u});
        if (o.umask && o.uint) op(43, {kUint, kUmask, 0xffffffffu});
        op(54, {kVoid, 1, 0, kFn});                   // OpFunction
        op(248, {100});                               // OpLabel
        op(59, {kPtr, 101, 7});                       // OpVariable Function
        if (o.half) op(59, {kHptr, 104, 7});
        op(61, {kFloat, kX, 101});                    // OpLoad
        if (o.half) op(61, {kHalf, kHx, 104});
        op(184, {kBool, kCond, kX, kOne});            // OpFOrdLessThan
    }
    uint32_t select(uint32_t type, uint32_t a, uint32_t b) {
        const uint32_t id = next++;
        op(169, {type, id, kCond, a, b});
        return id;
    }
    void use(uint32_t id) { op(129, {kFloat, next++, id, kX}); }   // OpFAdd: the select's result read on as a float
    void end() {
        op(253, {});                                  // OpReturn
        op(56, {});                                   // OpFunctionEnd
    }
};

// ---- reading a module back ----

struct Read {
    bool ok = true;
    std::string why;
    std::map<uint32_t, size_t> def;   // (each id's defining instruction)
    size_t fn_at = 0;
};

// (which instructions have a result id, and where it is: enough for what the fixtures hold)
int result_at(uint32_t o) {
    switch (o) {
    case 19: case 20: case 21: case 22: case 23: case 32: case 33: case 248: return 1;
    case 41: case 42: case 43: case 44: case 54: case 59: case 61: case 124: case 129: case 169: case 184: return 2;
    default: return 0;
    }
}

Read read(const std::vector<uint32_t>& w) {
    Read r;
    if (w.size() < 5 || w[0] != 0x07230203u) return r.ok = false, r.why = "no header", r;
    for (size_t i = 5; i < w.size();) {
        const uint32_t o = w[i] & 0xffffu, l = w[i] >> 16;
        if (!l || i + l > w.size()) return r.ok = false, r.why = "cut short", r;
        if (o == 54 && !r.fn_at) r.fn_at = i;
        if (const int at = result_at(o)) {
            const uint32_t id = w[i + at];
            if (id == 0 || id >= w[3]) return r.ok = false, r.why = "an id past the bound", r;
            if (!r.def.emplace(id, i).second) return r.ok = false, r.why = "an id defined twice", r;
            // (types, constants and variables of module scope before the first function; nothing else there)
            const bool global = o == 19 || o == 20 || o == 21 || o == 22 || o == 23 || o == 32 || o == 33 || o == 43 || o == 44;
            if (global && r.fn_at) return r.ok = false, r.why = "a type or constant inside a function", r;
            if (!global && !r.fn_at) return r.ok = false, r.why = "an instruction outside a function", r;
        }
        // (every operand id defined before it is used, but for a function's own forward references, which these have none of)
        if (o == 43 || o == 124 || o == 169 || o == 129) {
            for (uint32_t k = 1; k < l; k++) {
                if (o == 43 && k == 3) break;   // (a constant's literal)
                if (k == 2) continue;           // (the result)
                if (!r.def.count(w[i + k])) return r.ok = false, r.why = "an operand used before it is defined", r;
            }
        }
        i += l;
    }
    return r;
}

/// The bits of id with the condition c (only what the fixtures make: constants, bitcasts, selects; x stands as 0x3f000000)
bool eval(const std::vector<uint32_t>& w, const Read& r, uint32_t id, bool c, uint32_t* out, int depth = 0) {
    if (depth > 8) return false;
    if (id == kCond) return *out = c, true;
    if (id == kX) return *out = bits(0.5f), true;
    if (id == kHx) return *out = 0x3800u, true;
    const auto it = r.def.find(id);
    if (it == r.def.end()) return false;
    const size_t i = it->second;
    switch (w[i] & 0xffffu) {
    case 43: return *out = w[i + 3], true;
    case 124: return eval(w, r, w[i + 3], c, out, depth + 1);
    case 169: {
        uint32_t cv;
        if (!eval(w, r, w[i + 3], c, &cv, depth + 1)) return false;
        return eval(w, r, w[i + (cv ? 4 : 5)], c, out, depth + 1);
    }
    default: return false;
    }
}

/// The type a result id has
uint32_t type_of(const std::vector<uint32_t>& w, const Read& r, uint32_t id) {
    const auto it = r.def.find(id);
    return it == r.def.end() ? 0 : w[it->second + 1];
}

bool float_select_of_nan(const std::vector<uint32_t>& w, const Read& r) {
    for (size_t i = r.fn_at; i && i < w.size(); i += w[i] >> 16) {
        if ((w[i] & 0xffffu) != 169) continue;
        const uint32_t t = w[i + 1];
        if (t != kFloat && t != kHalf) continue;
        for (int k = 4; k <= 5; k++) {
            uint32_t b = 0;
            const auto d = r.def.find(w[i + k]);
            if (d != r.def.end() && (w[d->second] & 0xffffu) == 43 && eval(w, r, w[i + k], false, &b) &&
                spv_nan_select::nan(t == kFloat ? 32 : 16, b))
                return true;
        }
    }
    return false;
}

const char* out_dir = nullptr;
int written = 0;
void save(const std::vector<uint32_t>& w, const char* name) {
    if (!out_dir) return;
    const std::string path = std::string(out_dir) + "/" + name + ".spv";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(w.data(), 4, w.size(), f), std::fclose(f), written++;
    } else {
        expect(false, "could not write a module for spirv-val");
    }
}

/// The rewrite of m, checked: as many selects as want, the output well formed, the selects' results the same bits under
/// either condition and of the same float type, and no float select of a NaN constant left (but 16-bit ones, left)
void check(Module& m, uint32_t want, const std::vector<uint32_t>& selects, const char* name, uint32_t want_left = 0) {
    m.end();
    save(m.w, (std::string(name) + "-in").c_str());
    const Read before = read(m.w);
    expect(before.ok, (std::string(name) + ": the fixture reads back (") .append(before.why).append(")").c_str());
    spv_nan_select::Result res;
    const std::vector<uint32_t> out = spv_nan_select::rewrite(m.w.data(), m.w.size(), &res);
    expect(res.selects == want, (std::string(name) + ": selects rewritten").c_str());
    expect(res.left == want_left, (std::string(name) + ": 16-bit selects left").c_str());
    if (!want) {
        expect(out.empty(), (std::string(name) + ": nothing changed").c_str());
        return;
    }
    if (out.empty()) return expect(false, (std::string(name) + ": a module came out").c_str());
    save(out, (std::string(name) + "-out").c_str());
    const Read after = read(out);
    expect(after.ok, (std::string(name) + ": the output reads back (").append(after.why).append(")").c_str());
    if (!after.ok) return;
    expect(out[3] > m.w[3], (std::string(name) + ": the bound grows").c_str());
    expect(std::equal(m.w.begin(), m.w.begin() + 3, out.begin()) && out[4] == m.w[4],
           (std::string(name) + ": the header but its bound kept").c_str());
    expect(!want_left ? !float_select_of_nan(out, after) : true, (std::string(name) + ": no float select of a NaN left").c_str());
    for (uint32_t s : selects) {
        expect(type_of(out, after, s) == type_of(m.w, before, s), (std::string(name) + ": a select's result keeps its type").c_str());
        for (bool c : {false, true}) {
            uint32_t a = 1, b = 2;
            const bool ea = eval(m.w, before, s, c, &a), eb = eval(out, after, s, c, &b);
            expect(ea && eb && a == b, (std::string(name) + ": a select gives the same bits").c_str());
        }
    }
    // (a second pass finds nothing: what came out is not rewritten again)
    spv_nan_select::Result again;
    expect(spv_nan_select::rewrite(out.data(), out.size(), &again).empty() && again.selects == 0,
           (std::string(name) + ": rewritten once").c_str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) out_dir = argv[1];
    using namespace spv_nan_select;
    expect(nan(32, 0xffffffffu) && nan(32, 0x7f800001u) && nan(32, 0xffc00000u) && nan(32, 0x7fc00000u), "32-bit NaNs");
    expect(!nan(32, 0x7f800000u) && !nan(32, 0xff800000u) && !nan(32, bits(1.0f)) && !nan(32, 0), "32-bit numbers");
    expect(nan(16, 0x7e00u) && nan(16, 0xfc01u) && !nan(16, 0x7c00u) && !nan(16, 0x3c00u) && !nan(64, 0xffffffffu), "16-bit");
    expect(on(nullptr) && on("") && on("1") && !on("0"), "RX_VK_NAN_SELECTS");

    {   // (the mask on either side, against a constant and against a value; a select's result read on)
        Module m;
        const uint32_t a = m.select(kFloat, kNan, kOne), b = m.select(kFloat, kX, kNan), c = m.select(kFloat, kNegNan, kNan);
        m.use(a), m.use(b);
        check(m, 3, {a, b, c}, "f32");
    }
    {   // (no 32-bit unsigned type in the module: one is declared)
        Module m(Options{false});
        const uint32_t a = m.select(kFloat, kOne, kNan);
        check(m, 1, {a}, "f32-no-uint");
    }
    {   // (an integer constant of the mask's bits already there: another is declared, which SPIR-V allows)
        Options o;
        o.umask = true;
        Module m(o);
        const uint32_t a = m.select(kFloat, kNan, kMinusOne);
        check(m, 1, {a}, "f32-umask");
    }
    {   // (16-bit floats with a 16-bit unsigned type: rewritten too)
        Options o;
        o.half = true;
        Module m(o);
        const uint32_t a = m.select(kHalf, kHnan, kHone), b = m.select(kHalf, kHx, kHnan), c = m.select(kFloat, kNan, kX);
        check(m, 3, {a, b, c}, "f16");
    }
    {   // (16-bit floats without one: those left, the 32-bit one rewritten)
        Options o;
        o.half = true, o.ushort = false;
        Module m(o);
        m.select(kHalf, kHnan, kHone);
        const uint32_t c = m.select(kFloat, kNan, kX);
        check(m, 1, {c}, "f16-no-ushort", 1);
    }
    {   // (nothing to change: numbers, infinity, a vector select, a select of no constant)
        Module m;
        m.select(kFloat, kOne, kMinusOne), m.select(kFloat, kInf, kX), m.select(kFloat, kX, kX);
        const uint32_t v = m.next++;
        m.op(169, {kVec2, v, kCond, kVecNan, kVecNan});   // (a vector select: left as it is)
        check(m, 0, {}, "none");
    }
    {   // (only 16-bit ones and no 16-bit unsigned type: nothing changed, and said)
        Options o;
        o.half = true, o.ushort = false;
        Module m(o);
        m.select(kHalf, kHone, kHnan);
        check(m, 0, {}, "f16-only-left", 1);
    }
    {   // (what it does not read: left alone)
        Module m;
        m.select(kFloat, kNan, kOne);
        m.end();
        std::vector<uint32_t> w = m.w;
        expect(rewrite(nullptr, 0).empty(), "no module");
        w[0] = 0x12345678u;
        expect(rewrite(w.data(), w.size()).empty(), "not SPIR-V");
        w = m.w, w[3] = 0;
        expect(rewrite(w.data(), w.size()).empty(), "a bound of 0");
        w = m.w, w[3] = kMaxBound + 1;
        expect(rewrite(w.data(), w.size()).empty(), "a bound past the limit");
        w = m.w, w.resize(w.size() - 3);
        expect(rewrite(w.data(), w.size()).empty(), "cut short");
        w = m.w, w[w.size() - 2] = 0;   // (an instruction of no length: the return)
        expect(rewrite(w.data(), w.size()).empty(), "an instruction of no length");
        expect(!rewrite(m.w.data(), m.w.size()).empty(), "the module itself is rewritten");
    }
    if (out_dir) std::printf("%d modules written for spirv-val\n", written);
    if (failures) return 1;
    std::printf("spv_nan_select: ok\n");
    return 0;
}
