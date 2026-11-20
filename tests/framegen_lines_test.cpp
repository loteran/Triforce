// tests/test_framegen_lines.py's program: frame generation's own passes (gpu/vk/shaders/fg_*.comp, the files the SPIR-V is
// made from, turned into C++ by the test) run on pictures made here, of straight lines moving as a camera moves them, and
// each made frame measured against the picture of the same moment made the same way.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace glsl {
// ---- just enough of GLSL for the fg_*.comp passes ----
struct vec2;
struct bvec2 { bool x, y; };
struct ivec2 {
    int x = 0, y = 0;
    ivec2() {}
    explicit ivec2(int v) : x(v), y(v) {}
    ivec2(int a, int b) : x(a), y(b) {}
    explicit ivec2(const vec2& v);
};
struct vec2 {
    float x = 0, y = 0;
    vec2() {}
    explicit vec2(float v) : x(v), y(v) {}
    vec2(float a, float b) : x(a), y(b) {}
    explicit vec2(const ivec2& v) : x((float)v.x), y((float)v.y) {}
};
inline ivec2::ivec2(const vec2& v) : x((int)v.x), y((int)v.y) {}
struct vec3 {
    union { float x, r; };
    union { float y, g; };
    union { float z, b; };
    vec3() : x(0), y(0), z(0) {}
    explicit vec3(float v) : x(v), y(v), z(v) {}
    vec3(float a, float c, float d) : x(a), y(c), z(d) {}
};
struct vec4 {
    union { float x, r; };
    union { float y, g; };
    union { float z, b; };
    union { float w, a; };
    vec4() : x(0), y(0), z(0), w(0) {}
    explicit vec4(float v) : x(v), y(v), z(v), w(v) {}
    vec4(float a_, float b_, float c_, float d_) : x(a_), y(b_), z(c_), w(d_) {}
    vec4(vec3 v, float d_) : x(v.x), y(v.y), z(v.z), w(d_) {}
    vec4(vec2 v, float c_, float d_) : x(v.x), y(v.y), z(c_), w(d_) {}
    vec3 rgb() const { return vec3(x, y, z); }
    vec2 xy() const { return vec2(x, y); }
};
struct uvec3 {
    unsigned x = 0, y = 0, z = 0;
    ivec2 xy() const { return ivec2((int)x, (int)y); }
};
static uvec3 gl_GlobalInvocationID;

inline ivec2 operator+(ivec2 a, ivec2 b) { return {a.x + b.x, a.y + b.y}; }
inline ivec2 operator-(ivec2 a, ivec2 b) { return {a.x - b.x, a.y - b.y}; }
inline ivec2 operator+(ivec2 a, int b) { return {a.x + b, a.y + b}; }
inline ivec2 operator-(ivec2 a, int b) { return {a.x - b, a.y - b}; }
inline ivec2 operator*(ivec2 a, int b) { return {a.x * b, a.y * b}; }
inline ivec2 operator/(ivec2 a, int b) { return {a.x / b, a.y / b}; }
inline ivec2& operator+=(ivec2& a, ivec2 b) { return a = a + b; }
inline bool operator==(ivec2 a, ivec2 b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(ivec2 a, ivec2 b) { return !(a == b); }
inline vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline vec2 operator-(vec2 a, vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline vec2 operator*(vec2 a, vec2 b) { return {a.x * b.x, a.y * b.y}; }
inline vec2 operator/(vec2 a, vec2 b) { return {a.x / b.x, a.y / b.y}; }
inline vec2 operator+(vec2 a, float b) { return {a.x + b, a.y + b}; }
inline vec2 operator-(vec2 a, float b) { return {a.x - b, a.y - b}; }
inline vec2 operator*(vec2 a, float b) { return {a.x * b, a.y * b}; }
inline vec2 operator*(float b, vec2 a) { return {a.x * b, a.y * b}; }
inline vec2 operator/(vec2 a, float b) { return {a.x / b, a.y / b}; }
inline vec2 operator/(float b, vec2 a) { return {b / a.x, b / a.y}; }
inline vec2 operator-(vec2 a) { return {-a.x, -a.y}; }
inline vec2& operator+=(vec2& a, vec2 b) { return a = a + b; }
inline vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline vec3 operator*(vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
inline vec3 operator*(float b, vec3 a) { return {a.x * b, a.y * b, a.z * b}; }
inline vec3 operator/(vec3 a, float b) { return {a.x / b, a.y / b, a.z / b}; }
inline vec3& operator+=(vec3& a, vec3 b) { return a = a + b; }
inline bvec2 greaterThanEqual(ivec2 a, ivec2 b) { return {a.x >= b.x, a.y >= b.y}; }
inline bool any(bvec2 v) { return v.x || v.y; }
inline int abs(int v) { return v < 0 ? -v : v; }
inline float abs(float v) { return std::fabs(v); }
inline vec2 abs(vec2 v) { return {std::fabs(v.x), std::fabs(v.y)}; }
inline vec3 abs(vec3 v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }
inline float min(float a, float b) { return b < a ? b : a; }
inline float max(float a, float b) { return a < b ? b : a; }
inline int min(int a, int b) { return b < a ? b : a; }
inline int max(int a, int b) { return a < b ? b : a; }
inline ivec2 min(ivec2 a, ivec2 b) { return {min(a.x, b.x), min(a.y, b.y)}; }
inline ivec2 max(ivec2 a, ivec2 b) { return {max(a.x, b.x), max(a.y, b.y)}; }
inline vec2 min(vec2 a, vec2 b) { return {min(a.x, b.x), min(a.y, b.y)}; }
inline vec2 max(vec2 a, vec2 b) { return {max(a.x, b.x), max(a.y, b.y)}; }
inline float clamp(float v, float lo, float hi) { return min(max(v, lo), hi); }
inline int clamp(int v, int lo, int hi) { return min(max(v, lo), hi); }
inline ivec2 clamp(ivec2 v, ivec2 lo, ivec2 hi) { return min(max(v, lo), hi); }
inline vec2 clamp(vec2 v, vec2 lo, vec2 hi) { return min(max(v, lo), hi); }
inline vec2 clamp(vec2 v, float lo, float hi) { return min(max(v, vec2(lo)), vec2(hi)); }
inline float floor(float v) { return std::floor(v); }
inline vec2 floor(vec2 v) { return {std::floor(v.x), std::floor(v.y)}; }
inline float round(float v) { return std::nearbyint(v); }   // (round to even, as GPUs do it)
inline vec2 round(vec2 v) { return {round(v.x), round(v.y)}; }
inline float sqrt(float v) { return std::sqrt(v); }
inline float length(vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
inline float dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
inline float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float mix(float a, float b, float t) { return a + (b - a) * t; }
inline vec2 mix(vec2 a, vec2 b, float t) { return {mix(a.x, b.x, t), mix(a.y, b.y, t)}; }
inline vec3 mix(vec3 a, vec3 b, float t) { return {mix(a.x, b.x, t), mix(a.y, b.y, t), mix(a.z, b.z, t)}; }
inline float smoothstep(float e0, float e1, float x) {
    const float t = clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ---- images: their formats as stored, sampled as a GPU samples them ----
enum Format { RGBA8, R16F, RGBA16F };
struct Tex {
    int w = 0, h = 0;
    Format fmt = RGBA8;
    std::vector<vec4> px;
    void make(int ww, int hh, Format f) { w = ww, h = hh, fmt = f, px.assign((size_t)ww * hh, vec4(0.0f)); }
    vec4& at(int x, int y) { return px[(size_t)y * w + x]; }
};
static long long fetches = 0;
static float half_float(float v) {   // (the nearest binary16, as an R16F / RGBA16F store keeps it)
    if (v == 0.0f || !std::isfinite(v)) return v;
    int e;
    const float m = std::frexp(v, &e);   // (v = m 2^e, 0.5 <= |m| < 1)
    if (e < -13) return std::ldexp(std::nearbyint(std::ldexp(v, 24)), -24);   // (subnormal steps)
    return std::ldexp(std::nearbyint(std::ldexp(m, 11)), e - 11);
}
static float unorm8(float v) { return std::nearbyint(clamp(v, 0.0f, 1.0f) * 255.0f) / 255.0f; }
struct Sampler { Tex* tex = nullptr; bool linear = false; };
struct Image { Tex* tex = nullptr; };
inline ivec2 textureSize(const Sampler& s, int) { return {s.tex->w, s.tex->h}; }
inline ivec2 imageSize(const Image& i) { return {i.tex->w, i.tex->h}; }
inline vec4 texelFetch(const Sampler& s, ivec2 q, int) {
    CHECK(q.x >= 0 && q.y >= 0 && q.x < s.tex->w && q.y < s.tex->h);
    fetches++;
    return s.tex->at(q.x, q.y);
}
inline vec4 texel_clamped(Tex* t, int x, int y) {
    x = x < 0 ? 0 : x >= t->w ? t->w - 1 : x, y = y < 0 ? 0 : y >= t->h ? t->h - 1 : y;
    return t->at(x, y);
}
inline vec4 mix4(vec4 a, vec4 b, float t) { return {mix(a.x, b.x, t), mix(a.y, b.y, t), mix(a.z, b.z, t), mix(a.w, b.w, t)}; }
inline vec4 textureLod(const Sampler& s, vec2 uv, float) {   // (clamped at the edges; a filter's weights in 256ths)
    fetches++;
    const float fx = uv.x * s.tex->w - 0.5f, fy = uv.y * s.tex->h - 0.5f;
    if (!s.linear) return texel_clamped(s.tex, (int)std::floor(fx + 0.5f), (int)std::floor(fy + 0.5f));
    const float x0 = std::floor(fx), y0 = std::floor(fy);
    const float tx = std::nearbyint((fx - x0) * 256.0f) / 256.0f, ty = std::nearbyint((fy - y0) * 256.0f) / 256.0f;
    const vec4 top = mix4(texel_clamped(s.tex, (int)x0, (int)y0), texel_clamped(s.tex, (int)x0 + 1, (int)y0), tx);
    const vec4 bottom = mix4(texel_clamped(s.tex, (int)x0, (int)y0 + 1), texel_clamped(s.tex, (int)x0 + 1, (int)y0 + 1), tx);
    return mix4(top, bottom, ty);
}
inline void imageStore(const Image& i, ivec2 q, vec4 v) {
    CHECK(q.x >= 0 && q.y >= 0 && q.x < i.tex->w && q.y < i.tex->h);
    if (i.tex->fmt == RGBA8) v = {unorm8(v.x), unorm8(v.y), unorm8(v.z), unorm8(v.w)};
    else if (i.tex->fmt == R16F) v = {half_float(v.x), 0.0f, 0.0f, 1.0f};
    else v = {half_float(v.x), half_float(v.y), half_float(v.z), half_float(v.w)};
    i.tex->at(q.x, q.y) = v;
}

/* SHADERS */

/// A pass over w x h in 8x8 groups, as vk_framegen.cpp's pass() dispatches it (each shader leaves out what is past its image)
template <class F> void dispatch(int w, int h, F main_) {
    for (int y = 0; y < (h + 7) / 8 * 8; y++)
        for (int x = 0; x < (w + 7) / 8 * 8; x++) {
            gl_GlobalInvocationID.x = (unsigned)x, gl_GlobalInvocationID.y = (unsigned)y;
            main_();
        }
}

constexpr double kPi = 3.14159265358979323846;
// ---- the pictures: a world of straight lines on a surface with some texture, seen by a camera that moves ----
struct Line { double px, py, ang, half; };   // (through (px, py) at angle ang, half its width)
struct Scene {
    const char* name;
    double pan_x, pan_y;   // (the camera's motion, pixels a frame)
    double turn;           // (its turn, radians a frame, about the picture's centre)
    double zoom;           // (its zoom a frame: 1 none)
    bool object;           // (a box moving over the still world, a motion of its own)
    double obj_x, obj_y;   // (the box's motion, pixels a frame)
    std::vector<double> ts;
};
constexpr int W = 960, H = 540, MARGIN = 48;
const double kLine = 0.08;   // (the lines' brightness: the world's surface is 0.45 to 0.65, the box 0.75, the HUD 0.9)
const Line kWorld[] = {
    {0, 120, 0.0, 1.0},       {0, 400, 0.0, 0.7},       {230, 0, kPi / 2, 1.0},  {720, 0, kPi / 2, 0.7},
    {480, 270, kPi / 6, 1.0}, {300, 300, 1.1, 0.8},     {600, 200, 0.21, 0.7},    {500, 300, 2.36, 1.2},
    {150, 500, -0.35, 0.8},   {820, 260, 1.9, 0.9},
};
const Line kBox[] = {{0, 30, 0.0, 1.0}, {60, 0, kPi / 2, 0.8}, {40, 40, 0.6, 0.9}, {200, 40, 2.2, 0.8}};
constexpr double kBoxW = 300, kBoxH = 170, kBoxX = 330, kBoxY = 150;   // (the box at time 0, its corner)
constexpr double kHudX = 40, kHudY = 470, kHudW = 260, kHudH = 34;    // (a HUD bar, still on screen)
const Line kHud[] = {{0, 17, 0.0, 0.8}, {30, 0, kPi / 2, 0.8}, {90, 0, kPi / 2, 0.8}, {150, 0, 1.0, 0.8}};
// (a fence: lines 11 world units apart in one place of the world, the repeated pattern block matching finds most often wrong)
constexpr double kFenceX0 = 560, kFenceX1 = 800, kFenceY0 = 330, kFenceY1 = 470, kFenceStep = 11, kFenceHalf = 0.6;

double hash(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xffffff) / 16777216.0;
}
double noise(double x, double y) {   // (value noise, smoothly between its points)
    const int ix = (int)std::floor(x), iy = (int)std::floor(y);
    double fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
    const double a = hash(ix, iy), b = hash(ix + 1, iy), c = hash(ix, iy + 1), d = hash(ix + 1, iy + 1);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}
double surface(double x, double y) { return 0.47 + 0.10 * noise(x / 23.0, y / 23.0) + 0.06 * noise(x / 5.0 + 17, y / 5.0); }
double line_dist(const Line& l, double x, double y) {
    return std::fabs(-(x - l.px) * std::sin(l.ang) + (y - l.py) * std::cos(l.ang));
}
double fence_dist(double x, double y) {   // (to the nearest of the fence's lines, which lean at 1.3 radians)
    const double u = (x - kFenceX0) * std::cos(1.3) + (y - kFenceY0) * std::sin(1.3);
    const double v = -(x - kFenceX0) * std::sin(1.3) + (y - kFenceY0) * std::cos(1.3);
    (void)u;
    const double m = std::fmod(std::fabs(v), kFenceStep);
    return std::min(m, kFenceStep - m);
}
bool in_fence(double x, double y) { return x >= kFenceX0 && x < kFenceX1 && y >= kFenceY0 && y < kFenceY1; }

struct View {   // (the camera at a time: a world point to the screen, scale s, turned by r, moved by (ox, oy))
    double s, r, ox, oy;
};
View view(const Scene& sc, double t) {
    return {std::pow(sc.zoom, t), sc.turn * t, sc.pan_x * t, sc.pan_y * t};
}
void to_world(const View& v, double sx, double sy, double& wx, double& wy) {   // (the inverse: screen to world)
    const double x = (sx - W / 2.0 - v.ox) / v.s, y = (sy - H / 2.0 - v.oy) / v.s;
    wx = x * std::cos(v.r) + y * std::sin(v.r) + W / 2.0, wy = -x * std::sin(v.r) + y * std::cos(v.r) + H / 2.0;
}
// What is at a screen point at time t: its brightness, and how far it is from the middle of the nearest line (screen pixels,
// minus that line's half width: 0 inside it)
struct Hit { double lum, off; };
Hit look(const Scene& sc, double t, double sx, double sy) {
    if (sx >= kHudX && sx < kHudX + kHudW && sy >= kHudY && sy < kHudY + kHudH) {
        double off = 1e9;
        for (const Line& l : kHud) off = std::min(off, line_dist(l, sx - kHudX, sy - kHudY) - l.half);
        return {off < 0 ? kLine : 0.9, std::max(0.0, off)};
    }
    if (sc.object) {
        const double bx = kBoxX + sc.obj_x * t, by = kBoxY + sc.obj_y * t;
        if (sx >= bx && sx < bx + kBoxW && sy >= by && sy < by + kBoxH) {
            double off = 1e9;
            for (const Line& l : kBox) off = std::min(off, line_dist(l, sx - bx, sy - by) - l.half);
            return {off < 0 ? kLine : 0.75, std::max(0.0, off)};
        }
    }
    const View v = view(sc, t);
    double wx, wy;
    to_world(v, sx, sy, wx, wy);
    double off = 1e9;
    for (const Line& l : kWorld) off = std::min(off, (line_dist(l, wx, wy) - l.half) * v.s);
    if (in_fence(wx, wy)) off = std::min(off, (fence_dist(wx, wy) - kFenceHalf) * v.s);
    return {off < 0 ? kLine : surface(wx, wy), std::max(0.0, off)};
}
/// The picture at time t, each pixel the mean of 4x4 points in it (lines with smooth edges, as a game's anti-aliased ones),
/// stored as the frame copies are (RGBA8). The surface is tinted a little: the passes see colour, not only light
void render(const Scene& sc, double t, Tex& out) {
    out.make(W, H, RGBA8);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double sum = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) sum += look(sc, t, x + (i + 0.5) / 4, y + (j + 0.5) / 4).lum;
            const float l = (float)(sum / 16);
            out.at(x, y) = vec4(unorm8(l * 1.02f), unorm8(l), unorm8(l * 0.95f), 1.0f);
        }
}

// ---- the two ways, as vk_framegen.cpp's framegen_record records them ----
struct Frames { Tex frame, luma, half; };
void reduce(Frames& f) {   // (fg_luma.comp's quarter-size luma, fg_half.comp's half-size)
    f.luma.make((W + 3) / 4, (H + 3) / 4, R16F);
    sh_fg_luma::src = {&f.frame, false}, sh_fg_luma::dst = {&f.luma};
    dispatch(f.luma.w, f.luma.h, sh_fg_luma::main_);
    f.half.make((W + 1) / 2, (H + 1) / 2, R16F);
    sh_fg_half::src = {&f.frame, false}, sh_fg_half::dst = {&f.half};
    dispatch(f.half.w, f.half.h, sh_fg_half::main_);
}
struct Vectors { Tex flow0, flow1, fine, fine0; long long flow_fetches = 0, fine_fetches = 0; };   // (fetches: texel reads)
void coarse(Frames& a, Frames& b, Vectors& v, float bias) {   // (fg_flow.comp, then fg_median.comp)
    const int qw = (W + 3) / 4, qh = (H + 3) / 4, cw = (qw + 3) / 4, ch = (qh + 3) / 4;
    v.flow0.make(cw, ch, RGBA16F), v.flow1.make(cw, ch, RGBA16F);
    const long long f0 = fetches;
    sh_fg_flow::BIAS = bias;
    sh_fg_flow::la = {&a.luma, false}, sh_fg_flow::lb = {&b.luma, false}, sh_fg_flow::flow = {&v.flow0};
    dispatch(cw, ch, sh_fg_flow::main_);
    sh_fg_median::src = {&v.flow0, false}, sh_fg_median::dst = {&v.flow1};
    dispatch(cw, ch, sh_fg_median::main_);
    v.flow_fetches = fetches - f0;
}
void fine(Frames& a, Frames& b, Vectors& v) {   // (fg_fine_flow.comp, then fg_fine_pick.comp twice)
    v.fine.make((W + 7) / 8, (H + 7) / 8, RGBA16F);
    const long long f0 = fetches;
    sh_fg_fine_flow::la = {&a.half, false}, sh_fg_fine_flow::lb = {&b.half, false};
    sh_fg_fine_flow::coarse = {&v.flow1, false}, sh_fg_fine_flow::raw = {&v.flow0, false};
    sh_fg_fine_flow::fine = {&v.fine};
    sh_fg_fine_flow::fa = {&a.frame, true}, sh_fg_fine_flow::fb = {&b.frame, true};
    dispatch(v.fine.w, v.fine.h, sh_fg_fine_flow::main_);
    for (int i = 0; i < 2; i++) {   // (fg_fine_pick.comp, twice)
        v.fine0 = v.fine;
        sh_fg_fine_pick::fa = {&a.frame, true}, sh_fg_fine_pick::fb = {&b.frame, true};
        sh_fg_fine_pick::src = {&v.fine0, false}, sh_fg_fine_pick::dst = {&v.fine};
        dispatch(v.fine.w, v.fine.h, sh_fg_fine_pick::main_);
    }
    v.fine_fetches = fetches - f0;
    if (std::getenv("FG_LINES_DEBUG")) std::printf(", with the picks %.1f\n", (double)(fetches - f0) / (W * H));
}
long long made_old(Frames& a, Frames& b, Vectors& v, float t, Tex& out) {   // (fg_synth.comp)
    out.make(W, H, RGBA8);
    const long long f0 = fetches;
    sh_fg_synth::fa = {&a.frame, true}, sh_fg_synth::fb = {&b.frame, true}, sh_fg_synth::flow = {&v.flow1, false};
    sh_fg_synth::dst = {&out};
    sh_fg_synth::pc.t = t;
    dispatch(W, H, sh_fg_synth::main_);
    return fetches - f0;
}
long long made_fine(Frames& a, Frames& b, Vectors& v, float t, Tex& out) {   // (fg_fine_synth.comp)
    out.make(W, H, RGBA8);
    const long long f0 = fetches;
    sh_fg_fine_synth::fa = {&a.frame, true}, sh_fg_fine_synth::fb = {&b.frame, true};
    sh_fg_fine_synth::fine = {&v.fine, false};
    sh_fg_fine_synth::dst = {&out};
    sh_fg_fine_synth::pc.t = t;
    dispatch(W, H, sh_fg_fine_synth::main_);
    return fetches - f0;
}

// ---- the measure: how far a made frame's line pixels are from the true lines, and how many true line pixels it lost ----
float lum(const vec4& c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; }
constexpr float kDark = 0.30f;   // (a line's pixel: darker than any surface; a line's edge pixel half covered is about 0.3)
struct Score {
    double mean_off = 0, p99_off = 0, max_off = 0, far_share = 0, missing = 0, hud_err = 0, psnr = 0;
    long line_px = 0;
};
Score measure(const Scene& sc, double t, Tex& made, Tex& truth) {
    Score s;
    std::vector<double> offs;
    long truth_px = 0, lost = 0;
    double se = 0;
    long n = 0;
    for (int y = MARGIN; y < H - MARGIN; y++)
        for (int x = MARGIN; x < W - MARGIN; x++) {
            const float m = lum(made.at(x, y)), g = lum(truth.at(x, y));
            se += (double)(m - g) * (m - g), n++;
            if (m < kDark) offs.push_back(std::max(0.0, look(sc, t, x + 0.5, y + 0.5).off - 0.5));
            if (g < kDark) {
                truth_px++;
                bool found = false;
                for (int j = -1; j <= 1 && !found; j++)
                    for (int i = -1; i <= 1 && !found; i++) found = lum(made.at(x + i, y + j)) < kDark;
                lost += !found;
            }
            if (x >= kHudX && x < kHudX + kHudW && y >= kHudY && y < kHudY + kHudH)
                s.hud_err = std::max(s.hud_err, (double)std::fabs(m - g));
        }
    std::sort(offs.begin(), offs.end());
    s.line_px = (long)offs.size();
    if (!offs.empty()) {
        double sum = 0;
        long far = 0;
        for (double o : offs) sum += o, far += o > 1.0;
        s.mean_off = sum / offs.size(), s.max_off = offs.back(), s.p99_off = offs[(size_t)(offs.size() * 0.99)];
        s.far_share = (double)far / offs.size();
    }
    s.missing = truth_px ? (double)lost / truth_px : 0;
    s.psnr = 10 * std::log10(1.0 / std::max(1e-12, se / n));
    return s;
}

/// Where the point at (x, y) of the frame before is in this one: the motion a vector there should have
void true_motion(const Scene& sc, double x, double y, double& mx, double& my) {
    mx = my = 0;
    if (x >= kHudX && x < kHudX + kHudW && y >= kHudY && y < kHudY + kHudH) return;
    if (sc.object) {
        if (x >= kBoxX && x < kBoxX + kBoxW && y >= kBoxY && y < kBoxY + kBoxH) mx = sc.obj_x, my = sc.obj_y;
        return;
    }
    double wx, wy;
    to_world(view(sc, 0.0), x, y, wx, wy);
    const View v = view(sc, 1.0);
    const double px = wx - W / 2.0, py = wy - H / 2.0;
    const double sx = (px * std::cos(v.r) - py * std::sin(v.r)) * v.s + W / 2.0 + v.ox;
    const double sy = (px * std::sin(v.r) + py * std::cos(v.r)) * v.s + H / 2.0 + v.oy;
    mx = sx - x, my = sy - y;
}
void vector_report(const Scene& sc, Tex& fine) {   // (debugging: FG_LINES_DEBUG)
    long n = 0, off_half = 0, off_two = 0;
    double sum = 0;
    for (int y = 0; y < fine.h; y++)
        for (int x = 0; x < fine.w; x++) {
            const double cx = x * 8 + 4, cy = y * 8 + 4;
            if (cx < MARGIN || cy < MARGIN || cx > W - MARGIN || cy > H - MARGIN) continue;
            double mx, my;
            true_motion(sc, cx, cy, mx, my);
            const double e = std::hypot(fine.at(x, y).x - mx, fine.at(x, y).y - my);
            n++, sum += e, off_half += e > 0.5, off_two += e > 2;
                std::printf("    block %3d,%3d true %6.2f %6.2f fine %6.2f %6.2f sad %.4f\n", x, y, mx, my, fine.at(x, y).x,
                            fine.at(x, y).y, fine.at(x, y).z);
        }
    std::printf("  fine vectors: mean error %.3f px, over 0.5 px %.1f%%, over 2 px %.1f%%\n", sum / n, 100.0 * off_half / n,
                100.0 * off_two / n);
    if (const char* dir = std::getenv("FG_LINES_DEBUG")) {   // (the error of each block: grey 1 px, white 4 px and more)
        Tex e;
        e.make(fine.w, fine.h, RGBA8);
        for (int y = 0; y < fine.h; y++)
            for (int x = 0; x < fine.w; x++) {
                double mx, my;
                true_motion(sc, x * 8 + 4, y * 8 + 4, mx, my);
                const float d = (float)std::min(1.0, std::hypot(fine.at(x, y).x - mx, fine.at(x, y).y - my) / 4);
                e.at(x, y) = vec4(d, d, d, 1.0f);
            }
        char path[256];
        std::snprintf(path, sizeof path, "%s-vec-error", sc.name);
        extern void dump(const char*, const char*, Tex&);
        dump(dir, path, e);
    }
}
void dump(const char* dir, const char* name, Tex& t) {
    char path[512];
    std::snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6 %d %d 255\n", t.w, t.h);
    for (const vec4& c : t.px) {
        const unsigned char p[3] = {(unsigned char)(clamp(c.r, 0.0f, 1.0f) * 255 + 0.5f), (unsigned char)(clamp(c.g, 0.0f, 1.0f) * 255 + 0.5f),
                                    (unsigned char)(clamp(c.b, 0.0f, 1.0f) * 255 + 0.5f)};
        std::fwrite(p, 1, 3, f);
    }
    std::fclose(f);
}

void print(const char* way, const Scene& sc, double t, const Score& s, double fetch_px) {
    std::printf("%-10s %-4s t=%.3f  line px %6ld  off mean %.3f p99 %.2f max %.1f  >1px %5.2f%%  lost %5.2f%%  "
                "HUD %.3f  PSNR %.1f dB  fetches/px %.1f\n",
                sc.name, way, t, s.line_px, s.mean_off, s.p99_off, s.max_off, 100 * s.far_share, 100 * s.missing, s.hud_err,
                s.psnr, fetch_px);
}

static const float FINE_BIAS = /* FINE_BIAS */;   // (vk_framegen.cpp's kFineBias, put here by the test)
int run() {
    const std::vector<Scene> scenes = {
        {"pan", 6.3, 2.6, 0, 1, false, 0, 0, {0.5}},
        {"pan-fast", 17.7, -5.2, 0, 1, false, 0, 0, {0.5}},
        {"turn", 0, 0, 0.8 * kPi / 180, 1.012, false, 0, 0, {0.5}},
        {"box", 0, 0, 0, 1, true, 9.4, 3.7, {0.5}},
        {"pan-3x", 6.3, 2.6, 0, 1, false, 0, 0, {1.0 / 3, 2.0 / 3}},
        {"diag-3x", 3.7, 8.9, 0.3 * kPi / 180, 1.0, false, 0, 0, {1.0 / 3, 2.0 / 3}},
    };
    double worst_old_far = 0, worst_new_far = 0, worst_old_lost = 0, worst_new_lost = 0, sum_old_far = 0, sum_new_far = 0;
    double sum_old_lost = 0, sum_new_lost = 0, worst_new_p99 = 0, worst_new_hud = 0, cut_old = 0, cut_new = 0;
    int made = 0;
    for (const Scene& sc : scenes) {
        Frames a, b;
        render(sc, 0.0, a.frame), render(sc, 1.0, b.frame);
        reduce(a), reduce(b);
        Vectors old_v, v;
        coarse(a, b, old_v, 1.0f);
        coarse(a, b, v, FINE_BIAS);
        fine(a, b, v);
        for (double t : sc.ts) {
            Tex truth, old_made, new_made;
            render(sc, t, truth);
            const long long fo = made_old(a, b, old_v, (float)t, old_made), fn = made_fine(a, b, v, (float)t, new_made);
            const Score so = measure(sc, t, old_made, truth), sn = measure(sc, t, new_made, truth);
            if (made == 0)
                std::printf("texel reads a pixel: old way coarse vectors %.1f, made frame %.1f; fine way coarse vectors %.1f, fine "
                            "vectors and their picks %.1f, made frame %.1f\n", (double)old_v.flow_fetches / (W * H),
                            (double)fo / (W * H), (double)v.flow_fetches / (W * H), (double)v.fine_fetches / (W * H),
                            (double)fn / (W * H));
            print("old", sc, t, so, (double)(fo + old_v.flow_fetches) / (W * H));
            print("fine", sc, t, sn, (double)(fn + v.flow_fetches + v.fine_fetches) / (W * H));
            if (const char* dir = std::getenv("FG_LINES_DEBUG")) {
                vector_report(sc, v.fine);
                char n[128];
                std::snprintf(n, sizeof n, "%s-%.2f-truth", sc.name, t), dump(dir, n, truth);
                std::snprintf(n, sizeof n, "%s-%.2f-old", sc.name, t), dump(dir, n, old_made);
                std::snprintf(n, sizeof n, "%s-%.2f-fine", sc.name, t), dump(dir, n, new_made);
            }
            // (the fine vectors keep the lines in every scene, and do better than the old way there on both counts)
            CHECK(sn.far_share <= 0.03 && sn.missing <= 0.05 && sn.hud_err <= 0.06);
            CHECK(sn.far_share <= so.far_share && sn.missing < so.missing);
            worst_old_far = std::max(worst_old_far, so.far_share), worst_new_far = std::max(worst_new_far, sn.far_share);
            worst_old_lost = std::max(worst_old_lost, so.missing), worst_new_lost = std::max(worst_new_lost, sn.missing);
            worst_new_p99 = std::max(worst_new_p99, sn.p99_off), worst_new_hud = std::max(worst_new_hud, sn.hud_err);
            sum_old_far += so.far_share, sum_new_far += sn.far_share, sum_old_lost += so.missing, sum_new_lost += sn.missing;
            made++;
        }
    }
    std::printf("over %d made frames: line pixels over 1 px off the true line, mean %.2f%% -> %.2f%% (worst %.2f%% -> %.2f%%); "
                "true line pixels lost, mean %.2f%% -> %.2f%% (worst %.2f%% -> %.2f%%)\n",
                made, 100 * sum_old_far / made, 100 * sum_new_far / made, 100 * worst_old_far, 100 * worst_new_far,
                100 * sum_old_lost / made, 100 * sum_new_lost / made, 100 * worst_old_lost, 100 * worst_new_lost);
    // ---- two pictures alike: the made frame is that picture, both ways ----
    // ---- a cut: two pictures with nothing in common (a camera moved 300 pixels), where no vector explains anything ----
    const auto both = [](Frames& a, Frames& b, Tex& old_made, Tex& new_made) {
        reduce(a), reduce(b);
        Vectors old_v, v;
        coarse(a, b, old_v, 1.0f);
        coarse(a, b, v, FINE_BIAS);
        fine(a, b, v);
        made_old(a, b, old_v, 0.5f, old_made), made_fine(a, b, v, 0.5f, new_made);
    };
    {
        Frames a, b;
        render(scenes[0], 0.0, a.frame), render(scenes[0], 0.0, b.frame);
        Tex old_made, new_made;
        both(a, b, old_made, new_made);
        bool same_old = true, same_new = true;
        for (size_t i = 0; i < a.frame.px.size(); i++) {
            const vec4 p = a.frame.px[i], o = old_made.px[i], n = new_made.px[i];
            same_old = same_old && p.r == o.r && p.g == o.g && p.b == o.b;
            same_new = same_new && p.r == n.r && p.g == n.g && p.b == n.b;
        }
        std::printf("a still picture: the made frame is it, old %s, fine %s\n", same_old ? "yes" : "no", same_new ? "yes" : "no");
        CHECK(same_new);
    }
    {
        Scene far = scenes[0];
        far.pan_x = 300, far.pan_y = 40;
        Frames a, b;
        render(far, 0.0, a.frame), render(far, 1.0, b.frame);
        Tex old_made, new_made;
        both(a, b, old_made, new_made);
        long odd_old = 0, odd_new = 0, n = 0;
        for (int y = MARGIN; y < H - MARGIN; y++)
            for (int x = MARGIN; x < W - MARGIN; x++) {
                const float a0 = lum(a.frame.at(x, y)), b0 = lum(b.frame.at(x, y)), mid = 0.5f * (a0 + b0);
                const auto odd = [&](float m) {
                    return std::fabs(m - a0) > 0.06f && std::fabs(m - b0) > 0.06f && std::fabs(m - mid) > 0.06f;
                };
                odd_old += odd(lum(old_made.at(x, y))), odd_new += odd(lum(new_made.at(x, y))), n++;
            }
        cut_old = (double)odd_old / n, cut_new = (double)odd_new / n;
        std::printf("a cut: pixels of the made frame neither picture nor their blend has, old %.2f%%, fine %.2f%%\n",
                    100 * cut_old, 100 * cut_new);
    }
    CHECK(cut_new <= 0.02);
    CHECK(sum_new_far * 3 < sum_old_far && sum_new_lost * 10 < sum_old_lost);
    (void)worst_new_p99, (void)worst_new_hud;
    return 0;
}
} // namespace glsl

int main() { return glsl::run(); }
