// (Triforce, after NX2navi's e38f42a, its second half: issue #156) A float select of a constant whose bits are a NaN, made a
// select of the bits. The translation types an integer mask such as 0xffffffff as a float where the guest's code moves it
// through a float register, and picks between it and another value with OpSelect. A compiler that reasons about floats may
// take a NaN operand as "any value" and fold the select (NX2navi saw a select between 1 and -1 give 2^32 on an RX 9070 XT
// without the float mode), and the bits that come out are no longer the mask: stray black or white blocks. A select of the
// integers that are the same bits cannot be folded that way:
//   %r = OpSelect %float %c %a %b   ->   %r = OpBitcast %float (OpSelect %uint %c a' b')
// where a' and b' are the operands' bits: for a float constant, an integer constant of the same bits (declared here, so no
// compiler has a float NaN to fold, not even through a bitcast of one), else an OpBitcast of the operand. Scalars of 32-bit
// floats, and of 16-bit floats in a module that already has a 16-bit unsigned integer type (no capability is added); vector
// selects are left as they are (the translation's selects are scalar: emit_spirv_select.cpp).
//
// For AMD's older Windows shader compiler alone (vk_vendor.h nan_bits, RX 5000 and RX 6000, or RX_VK_NAN_BITS=1), the same
// cards whose NaN tests the translation already makes on the bits (emit_spirv_floating_point.cpp IsNan). vk_shader.cpp
// make_module applies it to the module it hands the driver, outside the translation stamp (program_carry.h): the program
// cache's records keep the translation as it was made, a record read back is rewritten the same way as a program just
// translated, and on every other card no module changes at all. Where it applies, only the modules that have such a select
// are new to the driver's caches. RX_VK_NAN_SELECTS=0: left as they are (the NaN tests on bits stay).
// tests/test_spv_nan_select.py
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace spv_nan_select {

enum : uint32_t {
    OpTypeInt = 21, OpTypeFloat = 22, OpConstant = 43, OpFunction = 54, OpBitcast = 124, OpSelect = 169,
};
constexpr uint32_t kMaxBound = 0x3fffff;   // (Vulkan's least bound a driver takes: past it, nothing is changed)

/// RX_VK_NAN_SELECTS: set to 0, the selects are left as they are
inline bool on(const char* env) { return !(env && *env && std::atoi(env) == 0); }

/// Whether bits of a float of that width are a NaN: an exponent of all ones and a mantissa that is not 0
inline bool nan(uint32_t width, uint32_t bits) {
    return width == 32 ? (bits & 0x7fffffffu) > 0x7f800000u : width == 16 ? (bits & 0x7fffu) > 0x7c00u : false;
}

struct Result {
    uint32_t selects = 0;   // (selects rewritten)
    uint32_t left = 0;      // (16-bit ones left, for want of a 16-bit unsigned type in the module)
};

/// The module with its float selects of a NaN constant made selects of the bits (a copy), or empty: nothing to change, or a
/// module this does not read (not SPIR-V, cut short, a bound past kMaxBound)
inline std::vector<uint32_t> rewrite(const uint32_t* w, size_t n, Result* out = nullptr) {
    Result r;
    if (out) *out = r;
    if (!w || n < 6 || w[0] != 0x07230203u) return {};
    const uint32_t bound = w[3];
    if (!bound || bound > kMaxBound || bound > 8 * n + 1024) return {};
    // (before the first function: the float types by width, the first unsigned 16- and 32-bit types, the float constants)
    std::vector<uint8_t> width(bound, 0);   // (a float type's width, or a float constant's)
    std::vector<uint32_t> bits(bound, 0);   // (a float constant's bits)
    std::vector<uint8_t> constant(bound, 0);
    uint32_t u16 = 0, u32 = 0;
    size_t fn_at = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t o = w[i] & 0xffffu, l = w[i] >> 16;
        if (!l || i + l > n) return {};
        if (o == OpFunction) {
            fn_at = i;
            break;
        }
        if (o == OpTypeFloat && l == 3 && w[i + 1] < bound && (w[i + 2] == 16 || w[i + 2] == 32)) width[w[i + 1]] = (uint8_t)w[i + 2];
        else if (o == OpTypeInt && l == 4 && w[i + 3] == 0) {
            if (w[i + 2] == 16 && !u16) u16 = w[i + 1];
            if (w[i + 2] == 32 && !u32) u32 = w[i + 1];
        } else if (o == OpConstant && l == 4 && w[i + 1] < bound && w[i + 2] < bound && width[w[i + 1]]) {
            constant[w[i + 2]] = 1, width[w[i + 2]] = width[w[i + 1]], bits[w[i + 2]] = w[i + 3];
        }
        i += l;
    }
    if (!fn_at) return {};
    // (the selects: a scalar float result, and an operand that is a constant of NaN bits)
    const auto picks = [&](size_t i) -> uint32_t {   // (the select's width, 0: not one to rewrite)
        const uint32_t t = w[i + 1], a = w[i + 4], b = w[i + 5];
        if (t >= bound || a >= bound || b >= bound || constant[t] || !width[t]) return 0;
        const uint32_t wd = width[t];
        if (!(constant[a] && nan(wd, bits[a])) && !(constant[b] && nan(wd, bits[b]))) return 0;
        return wd;
    };
    for (size_t i = fn_at; i < n;) {
        const uint32_t o = w[i] & 0xffffu, l = w[i] >> 16;
        if (!l || i + l > n) return {};
        if (o == OpSelect && l == 6)
            if (const uint32_t wd = picks(i)) {
                if (wd == 16 && !u16) r.left++;
                else r.selects++;
            }
        i += l;
    }
    if (out) *out = r;
    if (!r.selects) return {};
    // (the declarations, right before the first function: the 32-bit unsigned type where there is none, then an integer
    // constant for each float constant a select takes, of the same bits)
    uint32_t next = bound;
    std::vector<uint32_t> decl;
    const bool need32 = !u32;
    std::vector<uint32_t> as_int(bound, 0);   // (a float constant's integer twin)
    for (size_t i = fn_at; i < n; i += w[i] >> 16) {
        if ((w[i] & 0xffffu) != OpSelect || w[i] >> 16 != 6) continue;
        const uint32_t wd = picks(i);
        if (!wd || (wd == 16 && !u16)) continue;
        if (wd == 32 && !u32) u32 = next++;
        for (int k = 4; k <= 5; k++) {
            const uint32_t a = w[i + k];
            if (!constant[a] || as_int[a]) continue;
            as_int[a] = next++;
            decl.insert(decl.end(), {4u << 16 | OpConstant, wd == 16 ? u16 : u32, as_int[a], wd == 16 ? bits[a] & 0xffffu : bits[a]});
        }
    }
    if (need32 && u32) decl.insert(decl.begin(), {4u << 16 | OpTypeInt, u32, 32, 0});
    // (each select: at most two bitcasts in, the select, a bitcast out)
    if ((uint64_t)next + 4ull * r.selects > kMaxBound) {
        if (out) out->selects = 0;
        return {};
    }
    std::vector<uint32_t> o;
    o.reserve(n + decl.size() + 9ull * r.selects);
    o.insert(o.end(), w, w + fn_at);
    o.insert(o.end(), decl.begin(), decl.end());
    for (size_t i = fn_at; i < n;) {
        const uint32_t l = w[i] >> 16;
        const uint32_t wd = (w[i] & 0xffffu) == OpSelect && l == 6 ? picks(i) : 0;
        if (!wd || (wd == 16 && !u16)) {
            o.insert(o.end(), w + i, w + i + l);
            i += l;
            continue;
        }
        const uint32_t ut = wd == 16 ? u16 : u32, t = w[i + 1], result = w[i + 2], c = w[i + 3];
        uint32_t ab[2];
        for (int k = 0; k < 2; k++) {
            const uint32_t a = w[i + 4 + k];
            if (as_int[a]) ab[k] = as_int[a];
            else ab[k] = next++, o.insert(o.end(), {4u << 16 | OpBitcast, ut, ab[k], a});
        }
        const uint32_t s = next++;
        o.insert(o.end(), {6u << 16 | OpSelect, ut, s, c, ab[0], ab[1]});
        o.insert(o.end(), {4u << 16 | OpBitcast, t, result, s});
        i += l;
    }
    o[3] = next;
    return o;
}

} // namespace spv_nan_select
