// (Triforce) The Happy Mask Salesman on the Mods page (vk_salesman.h): his files, his poses, and a small rasterizer.
#include "vk_salesman.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

#include "vk_game_font.h"   // (game_font::Reader: the romfs's files packed with the game's dictionary)
#include "vk_mods.h"        // (decode_image: the mask's PNGs; game_folder_path)
#include "vk_bc_decoder.h"
#include "video_core/textures/astc.h"
#include "video_core/textures/decoders.h"

namespace mkwvk {
void logf(const char* fmt, ...);
}

namespace mkwvk::salesman {
namespace {

// ---- small maths ----
struct V3 { float x = 0, y = 0, z = 0; };
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 norm(V3 a) { const float l = std::sqrt(dot(a, a)); return l > 1e-12f ? a * (1 / l) : a; }
struct M4 {   // (row-major, column vectors: p' = M p)
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float& at(int r, int c) { return m[r * 4 + c]; }
    float at(int r, int c) const { return m[r * 4 + c]; }
};
M4 mul(const M4& a, const M4& b) {
    M4 o;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.at(r, k) * b.at(k, c);
            o.at(r, c) = s;
        }
    return o;
}
V3 xform(const M4& a, V3 p) { return {a.at(0, 0) * p.x + a.at(0, 1) * p.y + a.at(0, 2) * p.z + a.at(0, 3), a.at(1, 0) * p.x + a.at(1, 1) * p.y + a.at(1, 2) * p.z + a.at(1, 3),
                                       a.at(2, 0) * p.x + a.at(2, 1) * p.y + a.at(2, 2) * p.z + a.at(2, 3)}; }
V3 xdir(const M4& a, V3 p) { return {a.at(0, 0) * p.x + a.at(0, 1) * p.y + a.at(0, 2) * p.z, a.at(1, 0) * p.x + a.at(1, 1) * p.y + a.at(1, 2) * p.z,
                                      a.at(2, 0) * p.x + a.at(2, 1) * p.y + a.at(2, 2) * p.z}; }
M4 inverse_affine(const M4& a) {   // (a 3x4 affine: the 3x3's inverse, then the translation)
    const float a00 = a.at(0, 0), a01 = a.at(0, 1), a02 = a.at(0, 2), a10 = a.at(1, 0), a11 = a.at(1, 1), a12 = a.at(1, 2), a20 = a.at(2, 0), a21 = a.at(2, 1), a22 = a.at(2, 2);
    const float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
    const float k = std::fabs(det) > 1e-20f ? 1 / det : 0;
    M4 o;
    o.at(0, 0) = (a11 * a22 - a12 * a21) * k, o.at(0, 1) = (a02 * a21 - a01 * a22) * k, o.at(0, 2) = (a01 * a12 - a02 * a11) * k;
    o.at(1, 0) = (a12 * a20 - a10 * a22) * k, o.at(1, 1) = (a00 * a22 - a02 * a20) * k, o.at(1, 2) = (a02 * a10 - a00 * a12) * k;
    o.at(2, 0) = (a10 * a21 - a11 * a20) * k, o.at(2, 1) = (a01 * a20 - a00 * a21) * k, o.at(2, 2) = (a00 * a11 - a01 * a10) * k;
    const V3 t = xdir(o, {a.at(0, 3), a.at(1, 3), a.at(2, 3)});
    o.at(0, 3) = -t.x, o.at(1, 3) = -t.y, o.at(2, 3) = -t.z;
    return o;
}
/// Scale, Euler XYZ rotation (Rz * Ry * Rx, as the game's bones), translation
M4 srt(const float s[3], const float r[3], const float t[3]) {
    const float cx = std::cos(r[0]), sx = std::sin(r[0]), cy = std::cos(r[1]), sy = std::sin(r[1]), cz = std::cos(r[2]), sz = std::sin(r[2]);
    const float R[9] = {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx, sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx, -sy, cy * sx, cy * cx};
    M4 o;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) o.at(i, j) = R[i * 3 + j] * s[j];
    o.at(0, 3) = t[0], o.at(1, 3) = t[1], o.at(2, 3) = t[2];
    return o;
}
M4 quat_m(const float s[3], const float q[4], const float t[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                        2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
    M4 o;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) o.at(i, j) = R[i * 3 + j] * s[j];
    o.at(0, 3) = t[0], o.at(1, 3) = t[1], o.at(2, 3) = t[2];
    return o;
}

// ---- reading the files ----
struct Bytes {
    std::vector<unsigned char> d;
    bool ok(uint64_t o, uint64_t n) const { return o + n <= d.size(); }
    uint8_t u8(uint64_t o) const { return ok(o, 1) ? d[o] : 0; }
    uint16_t u16(uint64_t o) const { uint16_t v = 0; if (ok(o, 2)) memcpy(&v, &d[o], 2); return v; }
    int16_t s16(uint64_t o) const { int16_t v = 0; if (ok(o, 2)) memcpy(&v, &d[o], 2); return v; }
    uint32_t u32(uint64_t o) const { uint32_t v = 0; if (ok(o, 4)) memcpy(&v, &d[o], 4); return v; }
    uint64_t u64(uint64_t o) const { uint64_t v = 0; if (ok(o, 8)) memcpy(&v, &d[o], 8); return v; }
    float f32(uint64_t o) const { float v = 0; if (ok(o, 4)) memcpy(&v, &d[o], 4); return v; }
    std::string str(uint64_t o) const {   // (a BFRES string: u16 length, then its bytes)
        const uint16_t n = u16(o);
        if (!o || n > 512 || !ok(o + 2, n)) return {};
        return std::string((const char*)&d[o + 2], n);
    }
};

struct Tex {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
};
/// The first texture of a BNTX: ASTC (any block), BC1/2/3/4/5/7, unswizzled from the console's block-linear order
bool bntx_first(const std::vector<unsigned char>& raw, Tex& out) {
    Bytes b{raw};
    if (raw.size() < 0x40 || memcmp(raw.data(), "BNTX", 4)) return false;
    const uint32_t count = b.u32(0x24);
    const uint64_t table = b.u64(0x28);
    if (!count) return false;
    const uint64_t p = b.u64(table);
    if (!b.ok(p, 0xA0) || memcmp(&raw[p], "BRTI", 4)) return false;
    const uint32_t fmt = b.u32(p + 0x1C), w = b.u32(p + 0x24), h = b.u32(p + 0x28), bhlog = b.u32(p + 0x34) & 7, size = b.u32(p + 0x50);
    const uint64_t data = b.u64(b.u64(p + 0x70));
    if (!w || !h || w > 4096 || h > 4096) return false;
    const uint32_t kind = fmt >> 8;
    uint32_t bw = 4, bh = 4, bytes = 16;
    static const uint8_t astc[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    const bool is_astc = kind >= 0x2d && kind <= 0x3a;
    if (is_astc) bw = astc[kind - 0x2d][0], bh = astc[kind - 0x2d][1];
    else if (kind == 0x1a || kind == 0x1d) bytes = 8;
    else if (!(kind == 0x1b || kind == 0x1c || kind == 0x1e || kind == 0x20)) return false;
    const uint32_t nbx = (w + bw - 1) / bw, nby = (h + bh - 1) / bh;
    const size_t need = Tegra::Texture::CalculateSize(true, bytes, nbx, nby, 1, bhlog, 0);
    if (need > size || !b.ok(data, need)) return false;
    std::vector<unsigned char> blocks((size_t)nbx * nby * bytes);
    Tegra::Texture::UnswizzleTexture(std::span<unsigned char>(blocks), std::span<const unsigned char>(raw.data() + data, need), bytes, nbx, nby, 1, bhlog, 0, 0);
    out.w = (int)w, out.h = (int)h;
    out.rgba.assign((size_t)w * h * 4, 255);
    if (is_astc) {
        Tegra::Texture::ASTC::DecompressSerial(std::span<const unsigned char>(blocks), w, h, 1, bw, bh, std::span<unsigned char>(out.rgba));
        return true;
    }
    const uint32_t obpp = kind == 0x1d ? 1 : kind == 0x1e ? 2 : 4;
    std::vector<unsigned char> o((size_t)w * h * obpp, 0);
    for (uint32_t by = 0; by < nby; by++)
        for (uint32_t bx = 0; bx < nbx; bx++) {
            const unsigned char* src = blocks.data() + ((size_t)by * nbx + bx) * bytes;
            unsigned char* dst = o.data() + ((size_t)by * 4 * w + bx * 4) * obpp;
            switch (kind) {
            case 0x1a: bcn::DecodeBc1(src, dst, bx * 4, by * 4, w, h); break;
            case 0x1b: bcn::DecodeBc2(src, dst, bx * 4, by * 4, w, h); break;
            case 0x1c: bcn::DecodeBc3(src, dst, bx * 4, by * 4, w, h); break;
            case 0x1d: bcn::DecodeBc4(src, dst, bx * 4, by * 4, w, h, false); break;
            case 0x1e: bcn::DecodeBc5(src, dst, bx * 4, by * 4, w, h, false); break;
            case 0x20: bcn::DecodeBc7(src, dst, bx * 4, by * 4, w, h); break;
            }
        }
    for (size_t k = 0; k < (size_t)w * h; k++) {
        if (obpp == 4) memcpy(&out.rgba[k * 4], &o[k * 4], 4);
        else if (obpp == 1) out.rgba[k * 4] = out.rgba[k * 4 + 1] = out.rgba[k * 4 + 2] = o[k];
        else out.rgba[k * 4] = o[k * 2], out.rgba[k * 4 + 1] = o[k * 2 + 1];
    }
    return true;
}

struct Bone {
    std::string name;
    int parent = -1;
    float s[3] = {1, 1, 1}, r[4] = {0, 0, 0, 1}, t[3] = {0, 0, 0};
    bool euler = true;
    M4 W;   // (the rest pose, model space)
};
struct Part {
    std::string material;
    int mode = 0;          // (0 opaque, 1 alpha test, 2 skip)
    bool both = false;     // (drawn from both sides)
    float tint[3] = {1, 1, 1};
    float depth_bias = 0;  // (the pants under the coat: a little further)
    float spec = 0.06f;
    const Tex* alb = nullptr;
    const Tex* ao = nullptr;
    const Tex* emis = nullptr;
    std::vector<V3> pos, nrm;      // (the rest pose, model space)
    std::vector<float> uv;         // (2 a vertex)
    std::vector<uint8_t> idx;      // (4 a vertex: matrix indices)
    std::vector<float> wt;         // (4 a vertex)
    int skin = 0;
    std::vector<uint32_t> tri;
    bool mask = false;             // (Majora's Mask: on the head bone, not skinned)
};
struct Curve {
    int comp = 0;   // (0..2 scale, 3..5 translate, 6..9 rotate)
    std::vector<float> frames, vals;
};
struct Track {
    int bone = -1;
    bool has_s = false, has_r = false, has_t = false;
    float s[3] = {1, 1, 1}, r[4] = {0, 0, 0, 1}, t[3] = {0, 0, 0};
    std::vector<Curve> curves;
};
struct Anim {
    float frames = 1;
    bool euler = true;
    std::vector<Track> tracks;
};

struct Model {
    std::vector<Bone> bones;
    std::vector<int> m2b;
    std::vector<M4> inv;   // (smooth matrices' inverse rest poses)
    int nsmooth = 0;
    std::vector<Part> parts;
    std::vector<std::unique_ptr<Tex>> texes;
    int head = -1, waist = -1;
    M4 mask_rel;   // (the mask on the head bone)
    bool have_mask = false;
    Anim idle, joy, face;
};

float attr_num(const Bytes& b, uint64_t at, uint32_t fmt, int c) {
    switch (fmt) {
    case 0x1805: case 0x1905: case 0x1705: return b.f32(at + 4 * c);
    case 0x1202: return std::max(b.s16(at + 2 * c) / 32767.0f, -1.0f);
    case 0x1201: return b.u16(at + 2 * c) / 65535.0f;
    case 0x1205: {
        const uint16_t hbits = b.u16(at + 2 * c);
        const uint32_t sign = (hbits >> 15) & 1, e = (hbits >> 10) & 31, m = hbits & 1023;
        float v = e == 0 ? std::ldexp((float)m, -24) : e == 31 ? 65504.0f : std::ldexp((float)(m | 1024), (int)e - 25);
        return sign ? -v : v;
    }
    case 0x0e02: {
        const uint32_t v = b.u32(at);
        if (c == 3) { const int w = (int)(v >> 30); return (float)(w >= 2 ? w - 4 : w); }
        int x = (int)((v >> (10 * c)) & 0x3ff);
        if (x >= 512) x -= 1024;
        return std::max(x / 511.0f, -1.0f);
    }
    case 0x0903: case 0x0b03: case 0x0203: return (float)b.u8(at + c);
    case 0x0901: case 0x0b01: case 0x0201: return b.u8(at + c) / 255.0f;
    }
    return 0;
}

bool parse_model(const Bytes& b, Model& md, std::string* why) {
    if (b.d.size() < 0x200 || memcmp(b.d.data(), "FRES", 4)) { *why = "his model is not a BFRES"; return false; }
    const uint64_t data = b.u64(0x170), fmdl = b.u64(0x28);
    if (!b.ok(fmdl, 0x70) || memcmp(&b.d[fmdl], "FMDL", 4)) { *why = "his model has no FMDL"; return false; }
    const uint64_t skl = b.u64(fmdl + 0x18), sha = b.u64(fmdl + 0x28), mata = b.u64(fmdl + 0x38);
    const int nsh = b.u16(fmdl + 0x6a), nmat = b.u16(fmdl + 0x6c);
    const int nb = b.u16(skl + 0x38), nsmooth = b.u16(skl + 0x3a), nrigid = b.u16(skl + 0x3c);
    const uint64_t bones_at = b.u64(skl + 0x10), m2b_at = b.u64(skl + 0x18), inv_at = b.u64(skl + 0x20);
    if (nb <= 0 || nb > 1024) { *why = "his skeleton could not be read"; return false; }
    md.bones.resize(nb);
    for (int i = 0; i < nb; i++) {
        const uint64_t o = bones_at + 0x58 * i;
        Bone& bn = md.bones[i];
        bn.name = b.str(b.u64(o));
        bn.parent = b.s16(o + 0x22);
        bn.euler = (b.u32(o + 0x2c) & 0x1000) != 0;
        for (int k = 0; k < 3; k++) bn.s[k] = b.f32(o + 0x30 + 4 * k), bn.t[k] = b.f32(o + 0x4c + 4 * k);
        for (int k = 0; k < 4; k++) bn.r[k] = b.f32(o + 0x3c + 4 * k);
        const M4 L = bn.euler ? srt(bn.s, bn.r, bn.t) : quat_m(bn.s, bn.r, bn.t);
        bn.W = bn.parent >= 0 && bn.parent < i ? mul(md.bones[bn.parent].W, L) : L;
        if (bn.name == "Head") md.head = i;
        if (bn.name == "Waist") md.waist = i;
    }
    md.nsmooth = nsmooth;
    for (int i = 0; i < nsmooth + nrigid; i++) md.m2b.push_back(b.u16(m2b_at + 2 * i));
    for (int k = 0; k < nsmooth; k++) {
        M4 m;
        for (int j = 0; j < 12; j++) m.m[j] = b.f32(inv_at + 48 * k + 4 * j);
        md.inv.push_back(m);
    }
    auto good_name = [&](uint64_t p) {
        const uint16_t n = b.u16(p);
        if (!p || !n || n > 200 || !b.ok(p + 2, n)) return false;
        for (int k = 0; k < n; k++) if (b.d[p + 2 + k] < 32 || b.d[p + 2 + k] > 126) return false;
        return true;
    };
    struct Mat { std::string name, mode, cull; std::vector<std::string> tex; };
    std::vector<Mat> mats(nmat);
    for (int i = 0; i < nmat; i++) {
        const uint64_t f = mata + 0xb0 * i;
        mats[i].name = b.str(b.u64(f + 8));
        const uint64_t tp = b.u64(f + 0x20);
        for (int k = 0; k < 16 && b.ok(tp + 8 * k, 8) && good_name(b.u64(tp + 8 * k)); k++) mats[i].tex.push_back(b.str(b.u64(tp + 8 * k)));
        const uint64_t rp = b.u64(f + 0x40);
        if (b.ok(rp, 96) && good_name(b.u64(rp + 8 * 11))) mats[i].mode = b.str(b.u64(rp + 8 * 11)), mats[i].cull = b.str(b.u64(rp + 8 * 10));
    }
    for (int i = 0; i < nsh; i++) {
        const uint64_t sh = sha + 0x60 * i, vb = b.u64(sh + 0x10), mesh = b.u64(sh + 0x18);
        const int mi = b.u16(sh + 0x52);
        if (mi >= nmat) continue;
        Part pt;
        pt.material = mats[mi].name;
        if (pt.material.find("EyeShadow") != std::string::npos || pt.material.find("Eye_Outer") != std::string::npos ||
            pt.material.find("Teeth") != std::string::npos)
            continue;
        pt.mode = mats[mi].mode == "mask" ? 1 : 0;
        pt.both = mats[mi].cull == "both";
        if (pt.material.find("Skin") != std::string::npos || pt.material.find("Face") != std::string::npos) pt.tint[0] = 1.1f, pt.tint[1] = 0.99f, pt.tint[2] = 0.86f;
        if (pt.material.find("Satin") != std::string::npos) pt.depth_bias = 0.0025f;
        if (pt.material.find("Metal") != std::string::npos) pt.spec = 0.6f;
        else if (pt.material.find("Satin") != std::string::npos || pt.material.find("Eye") != std::string::npos) pt.spec = 0.25f;
        const int na = b.u8(vb + 0x4c), nbuf = b.u8(vb + 0x4d);
        const uint32_t vc = b.u32(vb + 0x50), bufoff = b.u32(vb + 0x48);
        pt.skin = b.u8(vb + 0x54);
        uint64_t starts[8] = {}, strides[8] = {};
        uint64_t at = data + bufoff;
        for (int k = 0; k < nbuf && k < 8; k++) {
            starts[k] = at, strides[k] = b.u32(b.u64(vb + 0x38) + 0x10 * k);
            at += (b.u32(b.u64(vb + 0x30) + 0x10 * k) + 7) & ~7ull;
        }
        pt.pos.resize(vc), pt.nrm.resize(vc), pt.uv.assign(vc * 2, 0), pt.idx.assign(vc * 4, 0), pt.wt.assign(vc * 4, 0);
        for (int k = 0; k < na; k++) {
            const uint64_t a = b.u64(vb + 8) + 0x10 * k;
            const std::string nm = b.str(b.u64(a));
            const uint32_t fmt = b.u32(a + 8);
            const uint32_t off = b.u16(a + 0xc), bi = b.u16(a + 0xe);
            if (bi >= 8) continue;
            for (uint32_t v = 0; v < vc; v++) {
                const uint64_t p = starts[bi] + strides[bi] * v + off;
                if (nm == "_p0") pt.pos[v] = {attr_num(b, p, fmt, 0), attr_num(b, p, fmt, 1), attr_num(b, p, fmt, 2)};
                else if (nm == "_n0") pt.nrm[v] = {attr_num(b, p, fmt, 0), attr_num(b, p, fmt, 1), attr_num(b, p, fmt, 2)};
                else if (nm == "_u0") pt.uv[v * 2] = attr_num(b, p, fmt, 0), pt.uv[v * 2 + 1] = attr_num(b, p, fmt, 1);
                else if (nm == "_i0") for (int c = 0; c < 4; c++) pt.idx[v * 4 + c] = (uint8_t)(c < (fmt >> 8 == 0x0b ? 4 : fmt >> 8 == 0x09 ? 2 : 1) ? attr_num(b, p, fmt, c) : 0);
                else if (nm == "_w0") for (int c = 0; c < 4; c++) pt.wt[v * 4 + c] = c < (fmt >> 8 == 0x0b ? 4 : fmt >> 8 == 0x09 ? 2 : 1) ? attr_num(b, p, fmt, c) : 0;
            }
        }
        // (one bone, rigid: the positions are that bone's; into the model's space once, as the others are)
        for (uint32_t v = 0; v < vc; v++) {
            if (pt.skin == 1) {
                const int k = pt.idx[v * 4];
                if (k < (int)md.m2b.size() && md.m2b[k] < md.bones.size()) {
                    const M4& W = md.bones[md.m2b[k]].W;
                    pt.pos[v] = xform(W, pt.pos[v]), pt.nrm[v] = xdir(W, pt.nrm[v]);
                }
                pt.wt[v * 4] = 1;
            } else if (pt.skin > 1) {
                float sum = 0;
                for (int c = 0; c < 4; c++) sum += pt.wt[v * 4 + c];
                for (int c = 0; c < 4; c++) pt.wt[v * 4 + c] = sum > 0 ? pt.wt[v * 4 + c] / sum : (c == 0 ? 1.0f : 0.0f);
            }
            pt.nrm[v] = norm(pt.nrm[v]);
        }
        const uint32_t faceoff = b.u32(mesh + 0x20), ifmt = b.u32(mesh + 0x28), icount = b.u32(mesh + 0x2c), firstv = b.u32(mesh + 0x30);
        const uint32_t isz = ifmt == 0 ? 1 : ifmt == 1 ? 2 : 4;
        if (!b.ok(data + faceoff, (uint64_t)icount * isz)) continue;
        pt.tri.resize(icount);
        for (uint32_t k = 0; k < icount; k++) {
            const uint64_t p = data + faceoff + (uint64_t)k * isz;
            pt.tri[k] = (isz == 1 ? b.u8(p) : isz == 2 ? b.u16(p) : b.u32(p)) + firstv;
            if (pt.tri[k] >= vc) pt.tri[k] = 0;
        }
        md.parts.push_back(std::move(pt));   // (its textures: part_textures, by its material's name)
    }
    return !md.parts.empty();
}

/// The texture names of each part (the material's albedo, its AO), read again from the model's materials
std::vector<std::pair<std::string, std::string>> part_textures(const Bytes& b, const Model& md) {
    std::vector<std::pair<std::string, std::string>> out;
    const uint64_t fmdl = b.u64(0x28), mata = b.u64(fmdl + 0x38);
    const int nmat = b.u16(fmdl + 0x6c);
    for (const Part& p : md.parts) {
        std::string alb, ao;
        for (int i = 0; i < nmat; i++) {
            const uint64_t f = mata + 0xb0 * i;
            if (b.str(b.u64(f + 8)) != p.material) continue;
            const uint64_t tp = b.u64(f + 0x20);
            for (int k = 0; k < 16; k++) {
                const std::string t = b.str(b.u64(tp + 8 * k));
                if (t.empty()) break;
                if (alb.empty() && t.find("_Alb") != std::string::npos) alb = t;
                if (ao.empty() && t.find("_AO") != std::string::npos) ao = t;
            }
        }
        out.emplace_back(alb, ao);
    }
    return out;
}

bool parse_anim(const Bytes& b, const char* name, const Model& md, Anim& out) {
    for (size_t at = 0; at + 0x50 < b.d.size(); at++) {
        if (memcmp(&b.d[at], "FSKA", 4)) continue;
        if (b.str(b.u64(at + 8)) != name) continue;
        out.euler = (b.u32(at + 4) & 0x1000) != 0;
        out.frames = std::max(1.0f, (float)b.u32(at + 0x40));
        const uint64_t ba = b.u64(at + 0x28);
        const int n = b.u16(at + 0x4c);
        for (int i = 0; i < n; i++) {
            const uint64_t o = ba + 0x38 * i;
            const std::string bn = b.str(b.u64(o));
            Track tr;
            for (size_t k = 0; k < md.bones.size(); k++) if (md.bones[k].name == bn) { tr.bone = (int)k; break; }
            if (tr.bone < 0) continue;
            uint64_t p = b.u64(o + 0x10);
            const uint32_t fl = b.u32(o + 0x28);
            if (fl & 0x08) { tr.has_s = true; for (int k = 0; k < 3; k++) tr.s[k] = b.f32(p + 4 * k); p += 12; }
            if (fl & 0x10) { tr.has_r = true; for (int k = 0; k < 4; k++) tr.r[k] = b.f32(p + 4 * k); p += 16; }
            if (fl & 0x20) { tr.has_t = true; for (int k = 0; k < 3; k++) tr.t[k] = b.f32(p + 4 * k); p += 12; }
            const int nc = b.u8(o + 0x2e);
            const uint64_t cv = b.u64(o + 8);
            for (int k = 0; k < nc; k++) {
                const uint64_t c = cv + 0x30 * k;
                const uint64_t fr = b.u64(c), ky = b.u64(c + 8);
                const uint16_t cfl = b.u16(c + 0x10), nk = b.u16(c + 0x12);
                const uint32_t off = b.u32(c + 0x14);
                const float scale = b.f32(c + 0x24), offset = b.f32(c + 0x28);
                if (!nk || nk > 4096) continue;
                Curve cu;
                if (off >= 0x04 && off <= 0x0c) cu.comp = (off - 0x04) / 4;
                else if (off >= 0x10 && off <= 0x18) cu.comp = 3 + (off - 0x10) / 4;
                else if (off >= 0x20 && off <= 0x2c) cu.comp = 6 + (off - 0x20) / 4;
                else continue;
                const int ft = cfl & 3, kt = (cfl >> 2) & 3;
                for (int j = 0; j < nk; j++) {
                    cu.frames.push_back(ft == 2 ? (float)b.u8(fr + j) : ft == 1 ? b.s16(fr + 2 * j) / 32.0f : b.f32(fr + 4 * j));
                    const uint64_t kp = ky + 8;
                    cu.vals.push_back(kt == 2 ? offset + b.u8(kp + 2 * j) * scale : kt == 1 ? offset + b.u16(kp + 4 * j) * scale : b.f32(kp + 8 * j));
                }
                tr.curves.push_back(std::move(cu));
            }
            out.tracks.push_back(std::move(tr));
        }
        return true;
    }
    return false;
}

float eval(const Curve& c, float t) {
    const int n = (int)c.vals.size();
    if (n == 1) return c.vals[0];
    t = std::clamp(t, c.frames[0], c.frames[n - 1]);
    int i = 0;
    while (i < n - 2 && c.frames[i + 1] < t) i++;
    const float f0 = c.frames[i], f1 = c.frames[i + 1], u = f1 > f0 ? (t - f0) / (f1 - f0) : 0;
    const float p0 = c.vals[i > 0 ? i - 1 : i], p1 = c.vals[i], p2 = c.vals[i + 1], p3 = c.vals[i + 2 < n ? i + 2 : i + 1];
    return 0.5f * (2 * p1 + (-p0 + p2) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u * u + (-p0 + 3 * p1 - 3 * p2 + p3) * u * u * u);
}

// ---- the pose: each bone's scale, Euler rotation and translation ----
struct Local { float s[3], r[3], t[3]; };
void rest_locals(const Model& md, std::vector<Local>& L) {
    L.resize(md.bones.size());
    for (size_t i = 0; i < md.bones.size(); i++) {
        const Bone& b = md.bones[i];
        for (int k = 0; k < 3; k++) L[i].s[k] = b.s[k], L[i].r[k] = b.r[k], L[i].t[k] = b.t[k];
    }
}
bool under(const Model& md, int i, int top) {
    for (; i >= 0; i = md.bones[i].parent) if (i == top) return true;
    return false;
}
/// An animation's frame over L: every bone it has (top >= 0: only that bone's branch; face: only the head's branch, not the head)
void apply(const Model& md, const Anim& a, float seconds, std::vector<Local>& L, int top, bool face) {
    const float f = std::fmod(seconds * 30.0f, a.frames);
    for (const Track& tr : a.tracks) {
        if (top >= 0 && !under(md, tr.bone, top)) continue;
        if (face && (tr.bone == md.head || !under(md, tr.bone, md.head))) continue;
        float v[10];
        for (int k = 0; k < 3; k++) v[k] = tr.has_s ? tr.s[k] : L[tr.bone].s[k], v[3 + k] = tr.has_t ? tr.t[k] : L[tr.bone].t[k];
        for (int k = 0; k < 4; k++) v[6 + k] = tr.has_r ? tr.r[k] : k < 3 ? L[tr.bone].r[k] : 0;
        for (const Curve& c : tr.curves) v[c.comp] = eval(c, f);
        Local& l = L[tr.bone];
        for (int k = 0; k < 3; k++) l.s[k] = v[k], l.t[k] = v[3 + k];
        if (tr.has_r || !tr.curves.empty()) for (int k = 0; k < 3; k++) l.r[k] = v[6 + k];
    }
}

// ---- the picture ----
struct Frame {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
};
float lin_lut[256];
unsigned char srgb_lut[4096];   // (linear 0..1 in 4096 steps -> the byte shown)
inline void sample(const Tex* t, float u, float v, float out[4]) {
    if (!t || t->rgba.empty()) { out[0] = out[1] = out[2] = out[3] = 1; return; }
    u -= std::floor(u), v -= std::floor(v);
    const float x = u * t->w - 0.5f, y = v * t->h - 0.5f;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = x - x0, fy = y - y0;
    float acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            const int xi = ((x0 + i) % t->w + t->w) % t->w, yi = ((y0 + j) % t->h + t->h) % t->h;
            const float k = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            const unsigned char* p = &t->rgba[((size_t)yi * t->w + xi) * 4];
            acc[0] += lin_lut[p[0]] * k, acc[1] += lin_lut[p[1]] * k, acc[2] += lin_lut[p[2]] * k, acc[3] += p[3] / 255.0f * k;
        }
    for (int c = 0; c < 4; c++) out[c] = acc[c];
}

struct Raster {
    int W = 0, H = 0;
    std::vector<float> z, col;   // (col: RGBA premultiplied, linear)
    M4 vp;
    V3 eye;
    // ((Triforce) 11-17, his "THE MASK GUY NEEDS SHADOWS TOO") The key light's shadows: a depth map seen from the key light
    // (L1, orthographic over his whole body), made from every opaque part before the picture; each pixel the key light
    // lights is looked up in it (3 x 3 samples, soft edges) and the key light taken off where something is between: the mask
    // over his face, his arms over the coat, the collar over the chest
    static constexpr int SM = 768;
    std::vector<float> sm;
    V3 lr, lu, lf;            // (the light's right, up and forward: light space)
    float lx0 = 0, ly0 = 0, lsx = 1, lsy = 1, lrange = 1;
    static V3 key() { return norm({-0.55f, 0.75f, 0.75f}); }
    void shadow_begin(const std::vector<V3>& all) {
        lf = norm(key() * -1.0f), lr = norm(cross(lf, {0, 1, 0})), lu = cross(lr, lf);
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
        for (const V3& v : all) {
            const float x = dot(v, lr), y = dot(v, lu), zz = dot(v, lf);
            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y), z0 = std::min(z0, zz), z1 = std::max(z1, zz);
        }
        const float m = 0.02f;
        lx0 = x0 - m, ly0 = y0 - m, lsx = (SM - 1) / std::max(x1 - x0 + 2 * m, 1e-4f), lsy = (SM - 1) / std::max(y1 - y0 + 2 * m, 1e-4f);
        lrange = std::max(z1 - z0, 1e-4f);
        sm.assign((size_t)SM * SM, 1e30f);
    }
    void shadow_tri(V3 a, V3 b, V3 c) {
        const float ax = (dot(a, lr) - lx0) * lsx, ay = (dot(a, lu) - ly0) * lsy, az = dot(a, lf);
        const float bx = (dot(b, lr) - lx0) * lsx, by = (dot(b, lu) - ly0) * lsy, bz = dot(b, lf);
        const float cx = (dot(c, lr) - lx0) * lsx, cy = (dot(c, lu) - ly0) * lsy, cz = dot(c, lf);
        const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (std::fabs(area) < 1e-8f) return;
        const float ia = 1 / area;
        const int x0 = std::max(0, (int)std::floor(std::min({ax, bx, cx}))), x1 = std::min(SM - 1, (int)std::ceil(std::max({ax, bx, cx})));
        const int y0 = std::max(0, (int)std::floor(std::min({ay, by, cy}))), y1 = std::min(SM - 1, (int)std::ceil(std::max({ay, by, cy})));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) * ia;
                const float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) * ia;
                const float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float& d = sm[(size_t)y * SM + x];
                d = std::min(d, w0 * az + w1 * bz + w2 * cz);
            }
    }
    /// How lit by the key light a point is, 0 (behind something) to 1, its normal n facing the light by cosl
    float lit(V3 p, V3 n, float cosl) const {
        if (sm.empty()) return 1;
        const V3 q = p + n * 0.006f;   // (off the surface a little: no shadow of itself)
        const float fx = (dot(q, lr) - lx0) * lsx, fy = (dot(q, lu) - ly0) * lsy, fz = dot(q, lf);
        const float bias = lrange * (0.004f + 0.012f * (1 - cosl));
        int x = (int)fx, y = (int)fy, in = 0, all = 0;
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++) {
                const int sx = std::clamp(x + i, 0, SM - 1), sy = std::clamp(y + j, 0, SM - 1);
                all++;
                if (fz - bias <= sm[(size_t)sy * SM + sx]) in++;
            }
        return (float)in / all;
    }
    void begin(int w, int h, V3 e, V3 target, float fov_deg) {
        W = w, H = h, eye = e;
        z.assign((size_t)w * h, 1e30f);
        col.assign((size_t)w * h * 4, 0);
        const V3 f = norm(target - e), r = norm(cross(f, {0, 1, 0})), u = cross(r, f);
        M4 view;
        view.at(0, 0) = r.x, view.at(0, 1) = r.y, view.at(0, 2) = r.z, view.at(0, 3) = -dot(r, e);
        view.at(1, 0) = u.x, view.at(1, 1) = u.y, view.at(1, 2) = u.z, view.at(1, 3) = -dot(u, e);
        view.at(2, 0) = -f.x, view.at(2, 1) = -f.y, view.at(2, 2) = -f.z, view.at(2, 3) = dot(f, e);
        const float t = 1 / std::tan(fov_deg * 3.14159265f / 360), asp = (float)w / h, zn = 0.05f, zf = 20;
        M4 proj;
        proj.at(0, 0) = t / asp, proj.at(1, 1) = t, proj.at(2, 2) = (zf + zn) / (zn - zf), proj.at(2, 3) = 2 * zf * zn / (zn - zf), proj.at(3, 2) = -1, proj.at(3, 3) = 0;
        vp = mul(proj, view);
    }
    struct SV { float x, y, z, iw; V3 p, n; float u, v; };
    SV project(V3 p, V3 n, float u, float v) const {
        const float cx = vp.at(0, 0) * p.x + vp.at(0, 1) * p.y + vp.at(0, 2) * p.z + vp.at(0, 3);
        const float cy = vp.at(1, 0) * p.x + vp.at(1, 1) * p.y + vp.at(1, 2) * p.z + vp.at(1, 3);
        const float cz = vp.at(2, 0) * p.x + vp.at(2, 1) * p.y + vp.at(2, 2) * p.z + vp.at(2, 3);
        const float cw = vp.at(3, 0) * p.x + vp.at(3, 1) * p.y + vp.at(3, 2) * p.z + vp.at(3, 3);
        const float iw = cw > 1e-6f ? 1 / cw : 0;
        return {(cx * iw * 0.5f + 0.5f) * W, (0.5f - cy * iw * 0.5f) * H, cz * iw, iw, p, n, u, v};
    }
    std::vector<SV> proj(const Part& pt, const std::vector<V3>& P, const std::vector<V3>& N) const {
        std::vector<SV> sv(P.size());
        for (size_t i = 0; i < P.size(); i++) sv[i] = project(P[i], N[i], pt.uv[i * 2], pt.uv[i * 2 + 1]);
        return sv;
    }
    /// One part's triangles over the rows ry0..ry1 (a band: the bands are drawn side by side on threads); alpha < 1: drawn
    /// over what is there (the mask lifting away), else opaque with the depth
    void part(const Part& pt, const std::vector<SV>& sv, float alpha, int ry0, int ry1) {
        const V3 L1 = key(), L2 = norm({0.8f, 0.25f, 0.4f}), L3 = norm({0.2f, 0.5f, -1.0f});
        for (size_t k = 0; k + 2 < pt.tri.size(); k += 3) {
            const SV& a = sv[pt.tri[k]];
            const SV& b = sv[pt.tri[k + 1]];
            const SV& c = sv[pt.tri[k + 2]];
            if (!a.iw || !b.iw || !c.iw) continue;
            const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (std::fabs(area) < 1e-8f) continue;
            if (!pt.both && area > 0) continue;   // (the back: screen y goes down, so a front face winds the other way)
            const int x0 = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x}))), x1 = std::min(W - 1, (int)std::ceil(std::max({a.x, b.x, c.x})));
            const int y0 = std::max(ry0, (int)std::floor(std::min({a.y, b.y, c.y}))), y1 = std::min(ry1 - 1, (int)std::ceil(std::max({a.y, b.y, c.y})));
            if (x0 > x1 || y0 > y1) continue;
            const float ia = 1 / area;
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * ia;
                    const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * ia;
                    const float w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    const float zz = w0 * a.z + w1 * b.z + w2 * c.z + pt.depth_bias;
                    const size_t at = (size_t)y * W + x;
                    if (zz >= z[at]) continue;
                    // (perspective-correct: the attributes over w)
                    const float q0 = w0 * a.iw, q1 = w1 * b.iw, q2 = w2 * c.iw, qs = 1 / (q0 + q1 + q2);
                    const float u = (q0 * a.u + q1 * b.u + q2 * c.u) * qs, v = (q0 * a.v + q1 * b.v + q2 * c.v) * qs;
                    float al[4];
                    sample(pt.alb, u, v, al);
                    if (pt.mode == 1 && al[3] < 0.5f) continue;
                    V3 n = norm((a.n * q0 + b.n * q1 + c.n * q2) * qs);
                    const V3 p = (a.p * q0 + b.p * q1 + c.p * q2) * qs;
                    const V3 vdir = norm(eye - p);
                    if (dot(n, vdir) < 0) n = n * -1;
                    float occ = 1;
                    if (pt.ao) { float o[4]; sample(pt.ao, u, v, o); occ = 1 + (o[0] - 1) * 0.85f; }
                    const float c1 = dot(n, L1);
                    const float sh = c1 > 0 ? lit(p, n, c1) : 1;   // ((Triforce) the key light's shadows)
                    const float d1 = std::max(c1, 0.0f) * sh, d2 = std::max(dot(n, L2), 0.0f);
                    const float fr = 1 - std::max(dot(n, vdir), 0.0f);
                    const float rim = fr * fr * fr * std::max(dot(n, L3) + 0.4f, 0.0f);
                    float h1 = std::max(dot(n, norm(L1 + vdir)), 0.0f);   // (to the 48th: by squaring)
                    const float h2 = h1 * h1, h4 = h2 * h2, h8 = h4 * h4, h16 = h8 * h8, h32 = h16 * h16;
                    const float sp = h32 * h16 * pt.spec * sh;
                    float rgb[3];
                    const float warm[3] = {1.0f, 0.92f, 0.82f}, cool[3] = {0.55f, 0.62f, 0.8f}, amb[3] = {0.32f, 0.3f, 0.34f}, rimc[3] = {1.0f, 0.85f, 0.7f};
                    float em[4] = {0, 0, 0, 0};
                    if (pt.emis) sample(pt.emis, u, v, em);
                    for (int ch = 0; ch < 3; ch++) {
                        const float base = al[ch] * pt.tint[ch];
                        float cc = base * (warm[ch] * d1 * 1.25f + cool[ch] * d2 * 0.45f + amb[ch] * (0.72f + 0.28f * sh)) * occ + rimc[ch] * rim * 0.55f + sp * occ + em[ch] * base * 2.2f;
                        rgb[ch] = cc / (1 + cc * 0.25f);
                    }
                    float* o = &col[at * 4];
                    if (alpha >= 1) {
                        z[at] = zz;
                        o[0] = rgb[0], o[1] = rgb[1], o[2] = rgb[2], o[3] = 1;
                    } else {   // (the mask lifting away: over what is there, no depth written)
                        for (int ch = 0; ch < 3; ch++) o[ch] = rgb[ch] * alpha + o[ch] * (1 - alpha);
                        o[3] = alpha + o[3] * (1 - alpha);
                    }
                }
        }
    }
    /// Down by `ss` in each direction into straight-alpha sRGB bytes
    void resolve(int ss, Frame& out) const {
        out.w = W / ss, out.h = H / ss;
        out.rgba.assign((size_t)out.w * out.h * 4, 0);
        const float k = 1.0f / (ss * ss);
        for (int y = 0; y < out.h; y++)
            for (int x = 0; x < out.w; x++) {
                float acc[4] = {0, 0, 0, 0};
                for (int j = 0; j < ss; j++)
                    for (int i = 0; i < ss; i++) {
                        const float* p = &col[((size_t)(y * ss + j) * W + x * ss + i) * 4];
                        for (int c = 0; c < 4; c++) acc[c] += p[c] * (c < 3 ? p[3] : 1);
                    }
                unsigned char* d = &out.rgba[((size_t)y * out.w + x) * 4];
                const float a = acc[3] * k;
                for (int c = 0; c < 3; c++) {
                    const float v = a > 0 ? acc[c] * k / a : 0;
                    d[c] = srgb_lut[(int)(std::clamp(v, 0.0f, 1.0f) * 4095.0f + 0.5f)];
                }
                d[3] = (unsigned char)std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f);
            }
    }
};

// ---- the thread ----
std::mutex mu;
State st = State::Idle;
std::string why_failed;
bool active = false;
int want_w = 0, want_h = 0;
Frame shown;
uint64_t shown_seq = 0;
std::atomic<int64_t> smile_until_ms{0};
std::unique_ptr<Model> model;

int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

bool load(const std::string& romfs, std::string* why) {
    for (int i = 0; i < 256; i++) lin_lut[i] = std::pow(i / 255.0f, 2.2f);
    for (int i = 0; i < 4096; i++) srgb_lut[i] = (unsigned char)std::clamp(std::pow(i / 4095.0f, 1 / 2.2f) * 255.0f + 0.5f, 0.0f, 255.0f);
    game_font::Reader rd;
    if (!rd.open(romfs)) { *why = "the game files could not be read at " + romfs; return false; }
    auto md = std::make_unique<Model>();
    Bytes mb;
    if (!rd.read_packed("Model/Npc_Hylia_MN_005.Npc_Hylia_MN_005.bfres.zs", mb.d)) { *why = "his model is not in the game files"; return false; }
    if (!parse_model(mb, *md, why)) return false;
    // (his textures: each part's albedo and its AO, read once each)
    const auto names = part_textures(mb, *md);
    std::vector<std::pair<std::string, Tex*>> got;
    auto texture = [&](const std::string& n) -> const Tex* {
        if (n.empty()) return nullptr;
        for (auto& g : got) if (g.first == n) return g.second;
        std::vector<unsigned char> raw;
        auto t = std::make_unique<Tex>();
        if (!rd.read_packed(("Tex/" + n + ".bntx.zs").c_str(), raw) || !bntx_first(raw, *t)) { got.emplace_back(n, nullptr); return nullptr; }
        Tex* p = t.get();
        md->texes.push_back(std::move(t));
        got.emplace_back(n, p);
        return p;
    };
    for (size_t i = 0; i < md->parts.size(); i++) {
        md->parts[i].alb = texture(names[i].first);
        md->parts[i].ao = texture(names[i].second);
    }
    // (his animations: his own idle and joyful face; the joyful stance of his kind)
    Bytes own, shared;
    rd.read_packed("Model/Npc_Hylia_MN_005_Animation.anim.bfres.zs", own.d);
    rd.read_packed("Model/Npc_Hylia_MN_Animation.anim.bfres.zs", shared.d);
    const bool a1 = parse_anim(own, "Act_Admiring_Man_Wait", *md, md->idle);
    const bool a2 = parse_anim(shared, "Def_Joy_Talk_Wait", *md, md->joy);
    const bool a3 = parse_anim(own, "Face_Def_Joy_Wait", *md, md->face);
    if (!a1) logf("salesman: his idle animation was not found: he stands as modelled");
    if (!a2 || !a3) logf("salesman: a joyful animation was not found (%d %d)", a2, a3);
    // (the mask, shipped beside the game: TFMM, its place on the head bone, its mesh; its PNGs)
    {
        const std::string dir = mods::game_folder_path() + "\\triforce_assets\\";
        FILE* f = fopen((dir + "majora_mask.bin").c_str(), "rb");
        std::vector<unsigned char> raw;
        if (f) {
            fseek(f, 0, SEEK_END);
            raw.resize((size_t)ftell(f));
            fseek(f, 0, SEEK_SET);
            if (fread(raw.data(), 1, raw.size(), f) != raw.size()) raw.clear();
            fclose(f);
        }
        Bytes m{raw};
        if (raw.size() > 80 && !memcmp(raw.data(), "TFMM", 4) && md->head >= 0) {
            for (int k = 0; k < 16; k++) md->mask_rel.m[k] = m.f32(8 + 4 * k);
            const uint32_t vc = m.u32(72), ic = m.u32(76);
            if (m.ok(80, (uint64_t)vc * 32 + (uint64_t)ic * 4)) {
                Part pt;
                pt.material = "Majora";
                pt.mask = true, pt.both = true, pt.spec = 0.2f;
                pt.pos.resize(vc), pt.nrm.resize(vc), pt.uv.resize(vc * 2);
                for (uint32_t v = 0; v < vc; v++) {
                    const uint64_t p = 80 + (uint64_t)v * 32;
                    pt.pos[v] = {m.f32(p), m.f32(p + 4), m.f32(p + 8)};
                    pt.nrm[v] = {m.f32(p + 12), m.f32(p + 16), m.f32(p + 20)};
                    pt.uv[v * 2] = m.f32(p + 24), pt.uv[v * 2 + 1] = m.f32(p + 28);
                }
                pt.tri.resize(ic);
                for (uint32_t k = 0; k < ic; k++) pt.tri[k] = std::min(m.u32(80 + (uint64_t)vc * 32 + 4 * k), vc - 1);
                auto png = [&](const char* name) -> const Tex* {
                    FILE* pf = fopen((dir + name).c_str(), "rb");
                    if (!pf) return nullptr;
                    std::string bytes;
                    char buf[65536];
                    size_t n;
                    while ((n = fread(buf, 1, sizeof buf, pf)) > 0) bytes.append(buf, n);
                    fclose(pf);
                    auto t = std::make_unique<Tex>();
                    if (!mods::decode_image(bytes, 1024, t->rgba, &t->w, &t->h)) return nullptr;
                    const Tex* p = t.get();
                    md->texes.push_back(std::move(t));
                    return p;
                };
                pt.alb = png("majora_alb.png");
                pt.emis = png("majora_emc.png");
                if (pt.alb) {   // (its colours or nothing: never a white mask)
                    md->parts.push_back(std::move(pt));
                    md->have_mask = true;
                }
            }
        }
        if (!md->have_mask) logf("salesman: Majora's Mask was not found in %s: he shows his face", dir.c_str());
    }
    size_t tris = 0;
    for (const Part& p : md->parts) tris += p.tri.size() / 3;
    logf("salesman: ready: %zu bones, %zu parts, %zu triangles, %zu textures, mask %s", md->bones.size(), md->parts.size(), tris,
         md->texes.size(), md->have_mask ? "on" : "none");
    model = std::move(md);
    return true;
}

void draw(Model& md, float secs, float lift, int out_w, int out_h, Frame& out) {
    // (the pose: masked = his admiring idle; lifted = his kind's joyful stance, his legs and coat from the idle (the coat is
    // cloth the game moves), his joyful face; between: blended)
    std::vector<Local> A, B;
    rest_locals(md, A);
    B = A;
    if (!md.idle.tracks.empty()) apply(md, md.idle, secs, A, -1, false);
    if (lift > 0) {
        if (!md.joy.tracks.empty()) apply(md, md.joy, secs, B, -1, false);
        if (!md.idle.tracks.empty() && md.waist >= 0) apply(md, md.idle, secs, B, md.waist, false);
        if (!md.face.tracks.empty() && md.head >= 0) apply(md, md.face, secs, B, -1, true);
    }
    const float k = lift * lift * (3 - 2 * lift);   // (eased)
    // (the two poses blended as rotations, not as angles: an angle and the same one 2 pi on would spin the bone between them)
    auto to_quat = [](const float r[3], float q[4]) {
        const float one[3] = {1, 1, 1}, zero[3] = {0, 0, 0};
        const M4 m = srt(one, r, zero);
        const float tr = m.at(0, 0) + m.at(1, 1) + m.at(2, 2);
        if (tr > 0) {
            const float s4 = std::sqrt(tr + 1) * 2;
            q[3] = 0.25f * s4, q[0] = (m.at(2, 1) - m.at(1, 2)) / s4, q[1] = (m.at(0, 2) - m.at(2, 0)) / s4, q[2] = (m.at(1, 0) - m.at(0, 1)) / s4;
        } else if (m.at(0, 0) > m.at(1, 1) && m.at(0, 0) > m.at(2, 2)) {
            const float s4 = std::sqrt(1 + m.at(0, 0) - m.at(1, 1) - m.at(2, 2)) * 2;
            q[3] = (m.at(2, 1) - m.at(1, 2)) / s4, q[0] = 0.25f * s4, q[1] = (m.at(0, 1) + m.at(1, 0)) / s4, q[2] = (m.at(0, 2) + m.at(2, 0)) / s4;
        } else if (m.at(1, 1) > m.at(2, 2)) {
            const float s4 = std::sqrt(1 + m.at(1, 1) - m.at(0, 0) - m.at(2, 2)) * 2;
            q[3] = (m.at(0, 2) - m.at(2, 0)) / s4, q[0] = (m.at(0, 1) + m.at(1, 0)) / s4, q[1] = 0.25f * s4, q[2] = (m.at(1, 2) + m.at(2, 1)) / s4;
        } else {
            const float s4 = std::sqrt(1 + m.at(2, 2) - m.at(0, 0) - m.at(1, 1)) * 2;
            q[3] = (m.at(1, 0) - m.at(0, 1)) / s4, q[0] = (m.at(0, 2) + m.at(2, 0)) / s4, q[1] = (m.at(1, 2) + m.at(2, 1)) / s4, q[2] = 0.25f * s4;
        }
    };
    std::vector<M4> Wp(md.bones.size());
    for (size_t i = 0; i < md.bones.size(); i++) {
        float s[3], t[3];
        for (int c = 0; c < 3; c++) s[c] = A[i].s[c] + (B[i].s[c] - A[i].s[c]) * k, t[c] = A[i].t[c] + (B[i].t[c] - A[i].t[c]) * k;
        M4 L;
        if (k <= 0.0001f) L = srt(s, A[i].r, t);
        else if (k >= 0.9999f) L = srt(s, B[i].r, t);
        else {
            float qa[4], qb[4], q[4];
            to_quat(A[i].r, qa), to_quat(B[i].r, qb);
            float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
            if (d < 0) for (int c = 0; c < 4; c++) qb[c] = -qb[c], d = -d;   // (the short way round)
            float wa = 1 - k, wb = k;
            if (d < 0.9995f) {
                const float th = std::acos(std::min(d, 1.0f)), sn = std::sin(th);
                wa = std::sin((1 - k) * th) / sn, wb = std::sin(k * th) / sn;
            }
            float n = 0;
            for (int c = 0; c < 4; c++) q[c] = qa[c] * wa + qb[c] * wb, n += q[c] * q[c];
            n = 1 / std::sqrt(std::max(n, 1e-12f));
            for (int c = 0; c < 4; c++) q[c] *= n;
            L = quat_m(s, q, t);
        }
        Wp[i] = md.bones[i].parent >= 0 ? mul(Wp[md.bones[i].parent], L) : L;
    }
    std::vector<M4> S(md.m2b.size());
    for (size_t k2 = 0; k2 < md.m2b.size(); k2++) {
        const int bi = md.m2b[k2] < md.bones.size() ? md.m2b[k2] : 0;
        S[k2] = (int)k2 < md.nsmooth ? mul(Wp[bi], md.inv[k2]) : mul(Wp[bi], inverse_affine(md.bones[bi].W));
    }
    const int ss = 2;
    Raster rs;
    // (framed as on the mockup, 30 degrees high for a corner 0.65 as wide as it is high: a narrower corner sees as wide)
    const float aspect = (float)out_w / std::max(1, out_h);
    const float fov = std::max(30.0f, 2 * std::atan(std::tan(15 * 3.14159265f / 180) * 0.646f / aspect) * 180 / 3.14159265f);
    // (his head kept where the mockup has it: the extra height a narrow corner sees goes below, to his fading legs)
    const float d = 2.33f, drop = d * (std::tan(fov * 3.14159265f / 360) - std::tan(15 * 3.14159265f / 180));
    rs.begin(out_w * ss, out_h * ss, {0.556f, 1.42f - drop, 2.317f}, {-0.064f, 1.24f - drop, 0.017f}, fov);
    // (every part skinned and projected once; then the picture's rows in bands, a thread each: the opaque parts, then the mask)
    std::vector<V3> P, N;
    std::vector<std::vector<Raster::SV>> sv(md.parts.size());
    std::vector<float> alpha(md.parts.size(), 1);
    for (size_t pi = 0; pi < md.parts.size(); pi++) {
        const Part& pt = md.parts[pi];
        const size_t n = pt.pos.size();
        P.resize(n), N.resize(n);
        if (pt.mask) {   // (on the head; lifted away and faded as he smiles)
            if (md.head < 0 || k >= 0.999f) continue;
            M4 up;
            up.at(1, 3) = 0.9f * k, up.at(2, 3) = 0.35f * k;
            const M4 Mw = mul(mul(Wp[md.head], md.mask_rel), up);
            for (size_t v = 0; v < n; v++) P[v] = xform(Mw, pt.pos[v]), N[v] = norm(xdir(Mw, pt.nrm[v]));
            alpha[pi] = k < 0.02f ? 1 : 1 - k;
        } else {
            for (size_t v = 0; v < n; v++) {
                if (!pt.skin) { P[v] = pt.pos[v], N[v] = pt.nrm[v]; continue; }
                V3 p{}, q{};
                for (int c = 0; c < 4; c++) {
                    const float w = pt.wt[v * 4 + c];
                    if (w <= 0) continue;
                    const size_t m = std::min<size_t>(pt.idx[v * 4 + c], S.size() - 1);
                    p = p + xform(S[m], pt.pos[v]) * w;
                    q = q + xdir(S[m], pt.nrm[v]) * w;
                }
                P[v] = p, N[v] = norm(q);
            }
        }
        sv[pi] = rs.proj(pt, P, N);
    }
    {   // ((Triforce) the key light's shadow map, of what is opaque now (the mask too while it is on his face))
        std::vector<V3> all;
        for (size_t pi = 0; pi < md.parts.size(); pi++)
            if (alpha[pi] >= 1)
                for (const Raster::SV& v : sv[pi]) all.push_back(v.p);
        if (!all.empty()) {
            rs.shadow_begin(all);
            for (size_t pi = 0; pi < md.parts.size(); pi++) {
                if (alpha[pi] < 1 || sv[pi].empty()) continue;
                const Part& pt = md.parts[pi];
                for (size_t t = 0; t + 2 < pt.tri.size(); t += 3) rs.shadow_tri(sv[pi][pt.tri[t]].p, sv[pi][pt.tri[t + 1]].p, sv[pi][pt.tri[t + 2]].p);
            }
        }
    }
    const int bands = std::clamp((int)std::thread::hardware_concurrency() - 1, 1, 6);
    std::vector<std::thread> th;
    for (int bi = 0; bi < bands; bi++) {
        const int ry0 = rs.H * bi / bands, ry1 = rs.H * (bi + 1) / bands;
        th.emplace_back([&, ry0, ry1] {
            for (int pass = 0; pass < 2; pass++)   // (the mask last: over what is behind it when it fades)
                for (size_t pi = 0; pi < md.parts.size(); pi++)
                    if (md.parts[pi].mask == (pass == 1) && !sv[pi].empty()) rs.part(md.parts[pi], sv[pi], alpha[pi], ry0, ry1);
        });
    }
    for (std::thread& t : th) t.join();
    rs.resolve(ss, out);
    // (his legs fade into the dark at the foot of his corner: the last fifth of the rows)
    const int fade0 = out.h * 4 / 5;
    for (int y = fade0; y < out.h; y++) {
        const float f = 1 - (float)(y - fade0) / std::max(1, out.h - fade0);
        for (int x = 0; x < out.w; x++) {
            unsigned char* p = &out.rgba[((size_t)y * out.w + x) * 4 + 3];
            *p = (unsigned char)(*p * f * f);
        }
    }
}

void thread_main(std::string romfs) {
    std::string why;
    const bool ok = load(romfs, &why);
    {
        std::lock_guard<std::mutex> g(mu);
        st = ok ? State::Ready : State::Failed;
        why_failed = why;
    }
    if (!ok) { logf("salesman: %s", why.c_str()); return; }
    const int64_t t0 = now_ms();
    float lift = 0;
    int64_t last = now_ms();
    for (;;) {
        bool on;
        int w, h;
        {
            std::lock_guard<std::mutex> g(mu);
            on = active, w = want_w, h = want_h;
        }
        if (!on || w < 16 || h < 16) { std::this_thread::sleep_for(std::chrono::milliseconds(60)); last = now_ms(); continue; }
        const int64_t now = now_ms();
        const float dt = std::min(0.1f, (now - last) / 1000.0f);
        last = now;
        const bool smiling = now < smile_until_ms.load();
        lift = std::clamp(lift + (smiling ? dt : -dt) / 0.45f, 0.0f, 1.0f);   // (the mask off or on in 0.45 s)
        Frame f;
        draw(*model, (now - t0) / 1000.0f, lift, w, h, f);
        {
            std::lock_guard<std::mutex> g(mu);
            shown = std::move(f);
            shown_seq++;
        }
        const int64_t spent = now_ms() - now;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max<int64_t>(4, 33 - spent)));
    }
}

}  // namespace

void start(const std::string& romfs) {
    {
        std::lock_guard<std::mutex> g(mu);
        if (st != State::Idle) return;
        st = State::Loading;
    }
    std::thread(thread_main, romfs).detach();
}
State state(std::string* why) {
    std::lock_guard<std::mutex> g(mu);
    if (why) *why = why_failed;
    return st;
}
void show(bool on, int w, int h) {
    std::lock_guard<std::mutex> g(mu);
    active = on;   // (drawn at four fifths of the size shown, two by two inside: the GPU scales him up, smooth)
    if (on) want_w = std::min(w * 4 / 5, 900), want_h = std::min(h * 4 / 5, 1500);
}
void smile(float seconds) {
    smile_until_ms = now_ms() + (int64_t)(seconds * 1000);
    logf("salesman: the mask off for %.1f s", seconds);
}
bool take(std::vector<unsigned char>& rgba, int* w, int* h, uint64_t* seq) {
    std::lock_guard<std::mutex> g(mu);
    if (shown_seq == *seq || shown.rgba.empty()) return false;
    rgba = shown.rgba, *w = shown.w, *h = shown.h, *seq = shown_seq;
    return true;
}

}  // namespace mkwvk::salesman
