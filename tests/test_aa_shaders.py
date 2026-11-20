"""The Anti-aliasing row's passes (gpu/vk/vk_aa.cpp; gpu/vk/shaders/aa_post.comp and smaa_*.comp): each *_comp_spv.h names the
hash of the sources it was compiled from (the pass, what it includes, and for SMAA third_party/smaa/SMAA.hlsl), checked here
against the files as they are, with or without glslang; with glslangValidator of the version the headers name, each header is
its pass compiled now, byte for byte. third_party/smaa's files are the ones its PROVENANCE.md names (their sha256), the two
lookup textures of the sizes vk_aa.cpp uploads.

And the passes' own per-pixel logic, run here: aa_common.glsl (the stored form: the square root of each channel, packed as
RGB9E5) and aa_fxaa.glsl (FXAA) are written in what GLSL and C++ share, so the very files the SPIR-V is made from are
compiled as C++ over a small vec2 / vec3 and run on authored pictures: a packed colour decodes to itself within the format's
steps, a flat field and a soft gradient come out untouched, a stair-stepped edge comes out smoothed without leaving the range
of the pixels around it, and what is far from an edge is left alone. (SMAA's passes are the reference implementation's own
code and are not run here.)

--write makes the five headers: glslang's output under a first line naming the sources' hash.

    python -I -B tests/test_aa_shaders.py [--cxx g++] [--glslang glslangValidator] [--write]
"""
import argparse
import hashlib
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "gpu" / "vk" / "shaders"
SMAA = ROOT / "third_party" / "smaa"

# each pass: what its text is made of, the pass first, then every file it includes (and those include)
PASSES = {
    "aa_post": ("aa_post.comp", "aa_common.glsl", "aa_fxaa.glsl"),
    "smaa_luma": ("smaa_luma.comp", "aa_common.glsl"),
    "smaa_edge": ("smaa_edge.comp", "smaa_setup.glsl", "SMAA.hlsl"),
    "smaa_weight": ("smaa_weight.comp", "smaa_setup.glsl", "SMAA.hlsl"),
    "smaa_blend": ("smaa_blend.comp", "smaa_setup.glsl", "SMAA.hlsl", "aa_common.glsl"),
}
FLAGS = ["-V", "--target-env", "vulkan1.2", "-I" + str(SMAA)]


def fail(what):
    raise SystemExit("FAIL: " + what)


def source(name):
    return (SMAA if name == "SMAA.hlsl" else SHADERS) / name


def includes(path):
    return re.findall(r'^#include "([^"]+)"', path.read_text(encoding="latin-1"), re.M)


def source_line(name):
    """the header's first line: the hash of the pass's sources (each by its name and its bytes, line ends as LF) and how it is made"""
    digest = hashlib.sha256()
    for part in PASSES[name]:
        digest.update(part.encode() + b"\0" + source(part).read_bytes().replace(b"\r\n", b"\n") + b"\0")
    return ("// %s sha256 %s (tests/test_aa_shaders.py --write: glslangValidator -V --target-env vulkan1.2 "
            "-I third_party/smaa --vn %s_COMP_SPV)\n" % (" + ".join(PASSES[name]), digest.hexdigest(), name.upper())).encode()


def compile_header(glslang, name, out):
    r = subprocess.run([glslang, *FLAGS, "--vn", name.upper() + "_COMP_SPV", "-o", str(out), str(SHADERS / (name + ".comp"))],
                       capture_output=True, text=True)
    if r.returncode:
        fail("%s.comp does not compile:\n%s%s" % (name, r.stdout, r.stderr))
    return out.read_bytes().replace(b"\r\n", b"\n")


def array_bytes(path, array, count):
    text = path.read_text(encoding="ascii")
    body = re.search(r"static const unsigned char %s\[\] = \{(.*?)\};" % array, text, re.S)
    if not body:
        fail("%s has no %s" % (path.name, array))
    data = bytes(int(n) for n in re.findall(r"\d+", body.group(1)))
    if len(data) != count:
        fail("%s: %s has %d bytes, not %d" % (path.name, array, len(data), count))
    digest = hashlib.sha256(data).hexdigest()
    if "sha256 of the bytes: %s" % digest not in text:
        fail("%s: its bytes are not the ones whose sha256 it names" % path.name)
    return digest


# (just enough of GLSL for aa_common.glsl and aa_fxaa.glsl: in a namespace of its own, so min, max, abs and the rest are these)
SHIM = r'''
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
namespace glsl {
typedef unsigned int uint;
struct vec2 {
    float x, y;
    vec2() : x(0), y(0) {}
    vec2(float a, float b) : x(a), y(b) {}
};
struct vec3 {
    float r, g, b;
    vec3() : r(0), g(0), b(0) {}
    explicit vec3(float v) : r(v), g(v), b(v) {}
    vec3(float x, float y, float z) : r(x), g(y), b(z) {}
};
static vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
static vec2 operator-(vec2 a, vec2 b) { return {a.x - b.x, a.y - b.y}; }
static vec2 operator*(vec2 a, float b) { return {a.x * b, a.y * b}; }
static float min(float a, float b) { return b < a ? b : a; }
static float max(float a, float b) { return a < b ? b : a; }
static float abs(float a) { return std::fabs(a); }
static float floor(float a) { return std::floor(a); }
static float log2(float a) { return std::log2(a); }
static float exp2(float a) { return std::exp2(a); }
static float clamp(float v, float lo, float hi) { return min(max(v, lo), hi); }
static float dot(vec3 a, vec3 b) { return a.r * b.r + a.g * b.g + a.b * b.b; }
static vec3 max(vec3 a, vec3 b) { return {max(a.r, b.r), max(a.g, b.g), max(a.b, b.b)}; }
static vec3 sqrt(vec3 a) { return {std::sqrt(a.r), std::sqrt(a.g), std::sqrt(a.b)}; }
'''

FIXTURE = r'''
// ---- an authored picture, sampled as the pass samples its input: bilinear between texel centres, clamped at the edges ----
static int W = 0, H = 0;
static std::vector<vec3> picture;   // (linear light, as the game's pass samples it)
static long samples = 0;
static vec3 texel(int x, int y) {
    x = x < 0 ? 0 : x >= W ? W - 1 : x, y = y < 0 ? 0 : y >= H ? H - 1 : y;
    return picture[(size_t)y * W + x];
}
static float mix1(float a, float b, float t) { return t <= 0.0f ? a : t >= 1.0f ? b : a + (b - a) * t; }
static vec3 mix3(vec3 a, vec3 b, float t) { return {mix1(a.r, b.r, t), mix1(a.g, b.g, t), mix1(a.b, b.b, t)}; }
// (a texel's weight in 256ths, as a GPU's filter has it: a texel's centre samples that texel alone)
static float weight(float f) { return std::floor(f * 256.0f + 0.5f) / 256.0f; }
static vec3 bilinear(vec2 uv) {
    const float fx = uv.x * W - 0.5f, fy = uv.y * H - 0.5f;
    const float x0 = std::floor(fx), y0 = std::floor(fy);
    const float tx = weight(fx - x0), ty = weight(fy - y0);
    const vec3 top = mix3(texel((int)x0, (int)y0), texel((int)x0 + 1, (int)y0), tx);
    const vec3 bottom = mix3(texel((int)x0, (int)y0 + 1), texel((int)x0 + 1, (int)y0 + 1), tx);
    return mix3(top, bottom, ty);
}
'''

RUN = r'''
static void make(int w, int h) { W = w, H = h, picture.assign((size_t)w * h, vec3(0.0f)); }
static vec2 centre(int x, int y) { return {(x + 0.5f) / W, (y + 0.5f) / H}; }
static vec2 texel_size() { return {1.0f / W, 1.0f / H}; }
static vec3 smoothed(int x, int y) { return aa_fxaa(centre(x, y), texel_size()); }
static vec3 stored(int x, int y) { return aa_encode(texel(x, y)); }   // (what Off writes for the pixel)
static bool same(vec3 a, vec3 b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
// (RGB9E5, as Vulkan's E5B9G9R9_UFLOAT_PACK32 decodes it)
static vec3 unpack(uint w) {
    const float scale = std::ldexp(1.0f, (int)(w >> 27) - 15 - 9);
    return {(float)(w & 511u) * scale, (float)((w >> 9) & 511u) * scale, (float)((w >> 18) & 511u) * scale};
}
static unsigned seed = 12345;
static float rnd() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.0f; }

static int run() {
    // ---- the stored form ----
    CHECK(aa_pack_rgb9e5(vec3(0.0f)) == 0u);
    CHECK(aa_pack_rgb9e5(vec3(1.0f)) == (256u | 256u << 9 | 256u << 18 | 16u << 27));        // (1.0: 256 / 512 * 2^1)
    CHECK(aa_pack_rgb9e5(vec3(0.5f, 0.25f, 0.0f)) == (256u | 128u << 9 | 0u << 18 | 15u << 27));
    CHECK(same(unpack(aa_pack_rgb9e5(vec3(1.0f, 0.5f, 0.25f))), vec3(1.0f, 0.5f, 0.25f)));
    // (nothing negative, no NaN, nothing past the format's largest value: its largest mantissa under its largest exponent)
    CHECK(aa_pack_rgb9e5(vec3(-1.0f, -0.0f, std::nanf(""))) == 0u);
    CHECK(aa_pack_rgb9e5(vec3(1e9f, 0.0f, 0.0f)) == (511u | 31u << 27));
    CHECK(same(unpack(aa_pack_rgb9e5(vec3(1e9f))), vec3(AA_MAX_9E5)));
    // (the largest channel rounding up to the next power of two takes the next exponent, not a tenth mantissa bit)
    CHECK(aa_pack_rgb9e5(vec3(0.9999f, 0.0f, 0.0f)) == (256u | 16u << 27));
    for (int i = 0; i < 200000; i++) {
        const float top = std::ldexp(1.0f, (int)(rnd() * 24.0f) - 12);
        const vec3 c(rnd() * top, rnd() * top, rnd() * top);
        const uint w = aa_pack_rgb9e5(c);
        const vec3 d = unpack(w);
        const float m = max(max(c.r, c.g), c.b);
        const float step = std::ldexp(1.0f, (int)(w >> 27) - 15 - 9);   // (one mantissa step of the shared exponent)
        CHECK((w & 511u) < 512u && ((w >> 9) & 511u) < 512u && ((w >> 18) & 511u) < 512u);
        CHECK(std::fabs(d.r - c.r) <= 0.5f * step + 1e-12f && std::fabs(d.g - c.g) <= 0.5f * step + 1e-12f &&
              std::fabs(d.b - c.b) <= 0.5f * step + 1e-12f);
        if (m > 1e-4f) CHECK(step <= m / 255.0f);   // (the exponent is the largest channel's: 8 bits of it at least)
    }
    CHECK(same(aa_encode(vec3(4.0f, 0.25f, 0.0f)), vec3(2.0f, 0.5f, 0.0f)) && same(aa_encode(vec3(-3.0f)), vec3(0.0f)));
    for (int i = 0; i < AA_STEPS; i++) CHECK(aa_step_len(i) >= 1.0f && (i == 0 || aa_step_len(i) >= aa_step_len(i - 1)));
    CHECK(aa_step_len(0) == 1.0f && aa_step_len(5) == 1.5f && aa_step_len(9) == 2.0f && aa_step_len(10) == 4.0f && aa_step_len(11) == 8.0f);

    // ---- FXAA ----
    // a flat field: every pixel as Off stores it, from one sample of its own and its four neighbours' (no walk)
    make(32, 24);
    for (vec3& p : picture) p = vec3(0.3f, 0.5f, 0.2f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            samples = 0;
            CHECK(same(smoothed(x, y), stored(x, y)));
            CHECK(samples == 5);
        }
    // black, and a field as bright as the format holds, too
    for (float v : {0.0f, 60000.0f}) {
        for (vec3& p : picture) p = vec3(v);
        CHECK(same(smoothed(7, 7), stored(7, 7)) && same(smoothed(0, 0), stored(0, 0)));
    }
    // a soft gradient (a sky): below the contrast FXAA looks at, untouched
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) picture[(size_t)y * W + x] = vec3(0.2f + 0.01f * x + 0.005f * y);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) CHECK(same(smoothed(x, y), stored(x, y)));

    // a stair-stepped edge: dark above a line that drops one pixel every six, bright below (what a slanted roof looks like
    // without anti-aliasing). Each step's corner pixels come out between the two sides; nothing leaves their range; the
    // picture away from the edge is untouched
    make(96, 48);
    const vec3 dark(0.02f, 0.03f, 0.05f), bright(0.8f, 0.7f, 0.5f);
    const auto edge_at = [](int x) { return 12 + x / 6; };   // (the first bright row of column x)
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) picture[(size_t)y * W + x] = y >= edge_at(x) ? bright : dark;
    const vec3 lo = aa_encode(dark), hi = aa_encode(bright);
    // (how far a pixel went from what it was toward the other side, in light: the stored value squared again, green)
    const auto moved = [&](vec3 o, vec3 was) { return std::fabs(o.g * o.g - was.g * was.g) / (bright.g - dark.g); };
    int changed = 0, blended = 0;
    float most = 0.0f;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const vec3 o = smoothed(x, y), was = stored(x, y);
            CHECK(std::isfinite(o.r) && std::isfinite(o.g) && std::isfinite(o.b));
            CHECK(o.r >= lo.r - 1e-5f && o.r <= hi.r + 1e-5f && o.g >= lo.g - 1e-5f && o.g <= hi.g + 1e-5f &&
                  o.b >= lo.b - 1e-5f && o.b <= hi.b + 1e-5f);
            const int away = y - edge_at(x);   // (rows from the edge: 0 and -1 touch it)
            if (away >= 2 || away <= -3) CHECK(same(o, was));   // (two rows from it or more: not an edge pixel)
            if (!same(o, was)) changed++;
            if (moved(o, was) > 0.2f && x >= 12 && x < W - 12) blended++;
            most = max(most, moved(o, was));
        }
    // (each step has four such pixels, the two on either side of the drop in its row: twelve steps stand clear of the
    // picture's sides)
    CHECK(changed > 0 && blended >= 40);
    CHECK(most > 0.3f && most <= 0.5f + 1e-4f);   // (an edge's pixel never goes more than half way to the other side)
    // the pixels at a step's corner, where the stair shows most: the bright pixel left of the drop and the dark one right of
    // it. The edge's run is six pixels and they sit one from its end: each goes 0.5 - 1 / 7.5 of the way to the other side
    {
        const int x = 47, y = edge_at(47);   // (column 47 is the last before the line drops at 48)
        const vec3 a = smoothed(x, y), b = smoothed(x + 1, y);   // (bright at (47, y), dark at (48, y))
        CHECK(same(stored(x, y), hi) && same(stored(x + 1, y), lo));
        CHECK(std::fabs(moved(a, hi) - (0.5f - 1.0f / 7.5f)) < 0.02f && std::fabs(moved(b, lo) - (0.5f - 1.0f / 7.5f)) < 0.02f);
        CHECK(a.g < hi.g && b.g > lo.g);
        // (and the next ones out, two from the end: less)
        CHECK(std::fabs(moved(smoothed(x - 1, y), hi) - (0.5f - 2.0f / 7.5f)) < 0.02f);
        CHECK(std::fabs(moved(smoothed(x + 2, y), lo) - (0.5f - 2.0f / 7.5f)) < 0.02f);
    }
    // the same edge mirrored left to right is smoothed as its mirror image
    {
        std::vector<vec3> first((size_t)W * H);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) first[(size_t)y * W + x] = smoothed(x, y);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W / 2; x++) std::swap(picture[(size_t)y * W + x], picture[(size_t)y * W + (W - 1 - x)]);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const vec3 o = smoothed(x, y), m = first[(size_t)y * W + (W - 1 - x)];
                CHECK(std::fabs(o.r - m.r) < 1e-4f && std::fabs(o.g - m.g) < 1e-4f && std::fabs(o.b - m.b) < 1e-4f);
            }
    }
    // a straight edge along the pixels has no stair to remove: its two rows are only softened a little, the rest untouched
    make(40, 20);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) picture[(size_t)y * W + x] = y >= 10 ? bright : dark;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const vec3 o = smoothed(x, y), was = stored(x, y);
            if (y < 9 || y > 10) CHECK(same(o, was));
            else CHECK(moved(o, was) > 0.0f && moved(o, was) <= 0.1f);
        }
    // a single bright pixel (a spark thinner than any edge): blurred toward its surroundings by AA_SUBPIXEL, never brighter
    make(16, 16);
    for (vec3& p : picture) p = dark;
    picture[8 * 16 + 8] = bright;
    {
        const vec3 o = smoothed(8, 8);
        CHECK(std::fabs(moved(o, hi) - AA_SUBPIXEL) < 0.02f && o.g > lo.g && o.g < hi.g);
    }
    std::printf("PASS: the stored form packs and clamps as RGB9E5; FXAA leaves flat fields, gradients and what is away from an "
                "edge alone and smooths a stair-stepped edge inside its pixels' range\n");
    return 0;
}
'''


def run_logic(cxx):
    common = (SHADERS / "aa_common.glsl").read_text(encoding="utf-8")
    fxaa = (SHADERS / "aa_fxaa.glsl").read_text(encoding="utf-8")
    post = (SHADERS / "aa_post.comp").read_text(encoding="utf-8")
    # the pass is made of the two files run here, in this order, and what it says the picture is is what the fixture says
    order = [post.find(s) for s in ('#include "aa_common.glsl"', "vec3 aa_at(vec2 uv) { return aa_encode(textureLod(src, uv, 0.0).rgb); }",
                                    '#include "aa_fxaa.glsl"', "void main()")]
    if -1 in order or order != sorted(order):
        fail("aa_post.comp no longer includes aa_common.glsl and aa_fxaa.glsl around its aa_at")
    for need in ("pc.mode == 1 ? aa_fxaa(uv, pc.inv) : aa_encode(texelFetch(src, p, 0).rgb)",
                 "imageStore(dst, p, uvec4(aa_pack_rgb9e5(c), 0u, 0u, 0u));", "vec2 uv = (vec2(p) + 0.5) * pc.inv;",
                 "layout(binding = 1, r32ui) uniform writeonly uimage2D dst;"):
        if need not in post:
            fail("aa_post.comp no longer has: " + need)
    blend = (SHADERS / "smaa_blend.comp").read_text(encoding="utf-8")
    if "imageStore(dst, p, uvec4(aa_pack_rgb9e5(aa_encode(c)), 0u, 0u, 0u));" not in blend:
        fail("smaa_blend.comp does not store its picture as the other passes do (aa_common.glsl)")
    fixture = (SHIM + common + FIXTURE +
               "static vec3 aa_at(vec2 uv) { samples++; return aa_encode(bilinear(uv)); }\n" + fxaa + RUN +
               "} // namespace glsl\nint main() { return glsl::run(); }\n")
    with tempfile.TemporaryDirectory(prefix="aa-shaders-") as tmp:
        cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Wno-unused-function", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--glslang", default="glslangValidator")
    p.add_argument("--write", action="store_true", help="make the five *_comp_spv.h from their passes")
    a = p.parse_args()
    glslang = shutil.which(a.glslang)
    if a.write:
        if not glslang:
            raise SystemExit("--write needs %s" % a.glslang)
        with tempfile.TemporaryDirectory(prefix="aa-shaders-") as tmp:
            for name in PASSES:
                (SHADERS / (name + "_comp_spv.h")).write_bytes(source_line(name) + compile_header(glslang, name, Path(tmp) / "out.h"))
                print("wrote gpu/vk/shaders/%s_comp_spv.h" % name)
        return

    # each pass's list above is what it includes
    for name, parts in PASSES.items():
        seen, todo = [], [parts[0]]
        while todo:
            part = todo.pop()
            if part in seen:
                continue
            seen.append(part)
            todo += includes(source(part))
        if sorted(seen) != sorted(parts):
            fail("%s.comp includes %s: PASSES here lists %s" % (name, sorted(seen), sorted(parts)))

    # third_party/smaa: the files its PROVENANCE.md names
    note = (SMAA / "PROVENANCE.md").read_text(encoding="utf-8")
    if hashlib.sha256((SMAA / "SMAA.hlsl").read_bytes().replace(b"\r\n", b"\n")).hexdigest() not in note:
        fail("third_party/smaa/SMAA.hlsl is not the file PROVENANCE.md names (its sha256)")
    for path, array, count in ((SMAA / "AreaTex.h", "areaTexBytes", 160 * 560 * 2), (SMAA / "SearchTex.h", "searchTexBytes", 64 * 16)):
        if array_bytes(path, array, count) not in note:
            fail("third_party/smaa/%s: PROVENANCE.md does not name its bytes' sha256" % path.name)
    if "Permission is hereby granted, free of charge" not in (SMAA / "LICENSE.txt").read_text(encoding="utf-8"):
        fail("third_party/smaa/LICENSE.txt is not SMAA's MIT notice")
    aa = (ROOT / "gpu" / "vk" / "vk_aa.cpp").read_text(encoding="utf-8")
    for need in ('#include "../../third_party/smaa/AreaTex.h"', '#include "../../third_party/smaa/SearchTex.h"',
                 "sizeof areaTexBytes == AREATEX_SIZE && sizeof searchTexBytes == SEARCHTEX_SIZE",
                 "VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT", "VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT"):
        if need not in aa:
            fail("vk_aa.cpp no longer has: " + need)
    for name in PASSES:
        if '#include "shaders/%s_comp_spv.h"' % name not in aa or "%s_COMP_SPV, sizeof %s_COMP_SPV" % (name.upper(), name.upper()) not in aa:
            fail("vk_aa.cpp does not make a pass of shaders/%s_comp_spv.h" % name)

    # the headers are their sources'
    bodies = {}
    for name in PASSES:
        header = (SHADERS / (name + "_comp_spv.h")).read_bytes().replace(b"\r\n", b"\n")
        first, _, bodies[name] = header.partition(b"\n")
        if first + b"\n" != source_line(name):
            fail("%s_comp_spv.h was not made from these sources (its first line names another hash): "
                 "python -I -B tests/test_aa_shaders.py --write" % name)
        if ("const uint32_t %s_COMP_SPV[] = {" % name.upper()).encode() not in bodies[name]:
            fail("%s_comp_spv.h does not define %s_COMP_SPV" % (name, name.upper()))

    run_logic(a.cxx)

    if not glslang:
        print("PASS: the five headers name their sources (no %s: their compile not checked)" % a.glslang)
        return
    with tempfile.TemporaryDirectory(prefix="aa-shaders-") as tmp:
        compared = 0
        for name in PASSES:
            made = compile_header(glslang, name, Path(tmp) / "out.h")
            if made.splitlines()[0] != bodies[name].splitlines()[0]:
                continue   # (another glslang than the header's: it compiles, the bytes are not compared)
            if made != bodies[name]:
                fail("%s_comp_spv.h is not %s.comp compiled: python -I -B tests/test_aa_shaders.py --write" % (name, name))
            compared += 1
    print("PASS: the five headers name their sources and the passes compile (%d of 5 compared byte for byte with this glslang)" % compared)


if __name__ == "__main__":
    main()
