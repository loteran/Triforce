// Fixture of tests/test_upload_same.py: gpu/vk/upload_same.h as it is, vk_memory.cpp's memory_visit_as_submitted (cut at
// "/* VISIT */") and vk_texture.cpp's re-upload check, upload_same_on to same_done (cut at "/* CHECK */"), around stand-ins: guest
// memory as one array, the shadow (the pages a kept capture holds) as vk_memory.cpp has it, a Texture of the fields the check
// reads with its levels' geometry given, and texture_touch's flow (same_look, the upload reading the check's copy where it is
// under way as upload_inner does, same_done) with the image as an array of the storage's bytes. (Triforce, 11-16) And ASTC decoded
// on the GPU: its upload modelled as upload_astc_gpu stages it (every layer a write reached, from its first kept level to its end,
// read through the check's copy where it is under way; a BC1 image found with alpha read again whole). (Triforce, 11-16) And the
// parallel replay's twin: two segments of one submission, each its own uploads applied in queue order at the submit.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "upload_same.h"

static int failures;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                \
            failures++;                                                              \
        }                                                                            \
    } while (0)

// ---- guest memory: one array at kBase (below 1 TB, read as submitted) ----
constexpr uint64_t kBase = 0x10f0000000ull, kMem = 24u << 20;   // (room for an ASTC texture of 8 to 16 MB)
static std::vector<uint8_t> guest(kMem);

// ---- vk_memory.cpp's shadow, as its declarations have it ----
namespace mem {
struct ShadowPage {
    std::unique_ptr<uint8_t[]> b;
    uint64_t seq = 0;
    uint64_t hash = 0;
};
std::mutex shadow_mu;
std::unordered_map<uint32_t, ShadowPage> shadow;
std::unordered_map<uint32_t, uint32_t> shadow_regions;
std::atomic<size_t> shadow_n{0};
bool overlay = true;   // (RX_VK_TEX_CAPTURE_OFF_FILE's A/B: false, guest memory as it is)
bool overlay_off() { return !overlay; }
/// a kept capture's copy of a page: the bytes given. As take_capture copies a page into its kept copy: the copy's hash goes unless
/// the capture found the same bytes there (tex_same)
void keep(uint64_t addr, const uint8_t* bytes) {
    const uint32_t p = (uint32_t)(addr >> 12);
    ShadowPage& s = shadow[p];
    if (!s.b) s.b.reset(new uint8_t[4096]), shadow_regions[p >> 9]++, shadow_n++;
    else if (std::memcmp(s.b.get(), bytes, 4096)) s.hash = 0;
    std::memcpy(s.b.get(), bytes, 4096);
}
/// a kept copy of the page as guest memory has it now
void keep_now(uint64_t addr) { keep(addr & ~0xFFFull, guest.data() + ((addr & ~0xFFFull) - kBase)); }
void drop_all() {
    shadow.clear(), shadow_regions.clear(), shadow_n = 0;
}
/* VISIT */
}  // namespace mem
using mem::memory_visit_as_submitted;
using mem::memory_hash_as_submitted;

// ---- vk_texture.cpp's stand-ins ----
enum VkFormat { VK_FORMAT_UNDEFINED = 0, FMT_RGBA8 = 37, FMT_BC7 = 145, FMT_ASTC = 157 };
enum VkSampleCountFlagBits { VK_SAMPLE_COUNT_1_BIT = 1, VK_SAMPLE_COUNT_4_BIT = 4 };
enum : uint32_t { VK_IMAGE_ASPECT_COLOR_BIT = 1, VK_IMAGE_ASPECT_DEPTH_BIT = 2, VK_IMAGE_ASPECT_STENCIL_BIT = 4 };
enum VkImageType { VK_IMAGE_TYPE_1D, VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_3D };
using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;
struct nvn_texdesc {
    int format = 0;
    uint32_t stride = 0;
    bool linear = false;
};
struct nvn_texlayout {
    uint64_t layer_size = 0;
};
struct LevelGeo {
    uint64_t offset;
    uint32_t w, h, d;
    uint32_t tw, th, td;
    unsigned tile_h, tile_d;
    uint64_t swizzled;
};
struct Texture {
    void* image = (void*)1;
    VkFormat format = FMT_BC7;
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool astc = false, bc = false, sparse = false;
    int bpb = 16, nvn_format = 0;
    uint32_t levels = 1, layers = 1;
    uint64_t gpu = 0, guest_size = 0;
    nvn_texdesc d;
    nvn_texlayout L;
    upload_same::State same;
    std::vector<LevelGeo> geo;   // (the fixture's: level_geo's answers)
    bool widen = false;          // (the fixture's: widened's answer)
};
LevelGeo level_geo(const Texture& t, uint32_t level) { return t.geo[level]; }
bool is_bc(VkFormat f) { return f == FMT_BC7; }
bool is_astc(VkFormat f) { return f == FMT_ASTC; }
bool nvn_tex_is_linear(const nvn_texdesc* d) { return d->linear; }
static const Texture* widen_of = nullptr;
VkFormat widened(VkFormat f, int* from) {
    *from = widen_of && widen_of->widen ? 3 : 0;
    return f;
}
struct VkFormatInfo {
    VkFormat format;
};
VkFormatInfo vk_format(int) { return {VK_FORMAT_UNDEFINED}; }
struct Host {
    const uint8_t* gpu_ptr(uint64_t gpu, uint64_t* avail) {
        if (gpu < kBase || gpu >= kBase + kMem) return *avail = 0, nullptr;
        *avail = kBase + kMem - gpu;
        return guest.data() + (gpu - kBase);
    }
    uint32_t tex_linear_stride(const nvn_texdesc* d) { return d->stride; }
};
static Host host_obj;
static Host* host = &host_obj;
static uint64_t frame_no = 1;

namespace tex_check {
void logf(const char*, ...) {}   // (the renderer's log: here before the C library's logf)
/* CHECK */
}  // namespace tex_check
using namespace tex_check;

// ---- the textures ----
/// A BC7 texture of 3 layers and 2 levels, at an address that is not page-aligned: level 0 64 KB a layer in 2 GOB-block rows of
/// 32 KB, level 1 16 KB in 2 rows of 8 KB, a layer 80 KB: 12 units, 240 KB
static Texture bc_texture(uint64_t at = kBase + 0x10200) {
    Texture t;
    t.gpu = at, t.layers = 3, t.levels = 2;
    t.geo.push_back(LevelGeo{0, 256, 160, 1, 64, 40, 1, 2, 0, 64 << 10});
    t.geo.push_back(LevelGeo{64 << 10, 128, 80, 1, 32, 20, 1, 1, 0, 16 << 10});
    t.L.layer_size = 80 << 10;
    t.guest_size = 240 << 10;
    return t;
}
static Ranges all_of(const Texture& t) { return Ranges{{0, t.guest_size}}; }
/// (Triforce, 11-16) An ASTC texture decoded on the GPU, kept as RGBA8, of 3 layers of 80 KB whose first 4 KB are levels the
/// image leaves out (tail0, 16-aligned): level 0 48 KB and level 1 16 KB from there, the rest of a layer the chain's padding.
/// upload_astc_gpu stages a layer from tail0 to its end: 3 units of 76 KB
static Texture astc_texture(uint64_t at = kBase + 0x10200) {
    Texture t;
    t.gpu = at, t.layers = 3, t.levels = 2;
    t.astc = true, t.format = FMT_RGBA8;
    t.geo.push_back(LevelGeo{4096, 160, 160, 1, 32, 32, 1, 2, 0, 48 << 10});
    t.geo.push_back(LevelGeo{4096 + (48 << 10), 80, 80, 1, 16, 16, 1, 1, 0, 16 << 10});
    t.L.layer_size = 80 << 10;
    t.guest_size = 240 << 10;
    return t;
}
/// upload_astc_gpu's layer size, by its expression
static uint64_t astc_layer_size(const Texture& t) {
    return t.layers > 1 ? t.L.layer_size : std::max<uint64_t>(t.L.layer_size, t.guest_size);
}
/// The parts of an ASTC texture's storage its image is made from (each layer from tail0 on): what an image check looks at
static Ranges astc_tails(const Texture& t, const Ranges& over) {
    const uint64_t ls = astc_layer_size(t), tail0 = level_geo(t, 0).offset;
    Ranges out;
    for (const auto& r : over)
        for (uint32_t layer = 0; layer < t.layers; layer++) {
            const uint64_t a = std::max<uint64_t>(r.first, layer * ls + tail0);
            const uint64_t b = std::min<uint64_t>({r.second, (layer + 1) * ls, t.guest_size});
            if (a < b) out.push_back({a, b});
        }
    return out;
}
/// What an image check of t looks at in `over`: all of it, or for ASTC its layers' tails
static Ranges image_part(const Texture& t, const Ranges& over) { return t.astc ? astc_tails(t, over) : over; }
/// The bytes the upload is to give the image at storage offset off: as submitted (a kept copy over guest memory)
static uint8_t submitted(const Texture& t, uint64_t off) {
    const uint64_t a = t.gpu + off;
    if (mem::overlay) {
        const auto it = mem::shadow.find((uint32_t)(a >> 12));
        if (it != mem::shadow.end()) return it->second.b[a & 0xFFF];
    }
    return guest[a - kBase];
}
static void write(const Texture& t, uint64_t off, uint64_t n, uint8_t v) { std::memset(guest.data() + (t.gpu + off - kBase), v, n); }
static void fill_random(const Texture& t, uint64_t off, uint64_t n, std::mt19937& rng) {
    for (uint64_t i = 0; i < n; i++) guest[t.gpu + off + i - kBase] = (uint8_t)rng();
}
/// The written pages of [off, off + n), as the write tracking gives them (memory_written_runs: whole pages, cut at the storage)
static void dirty_pages(const Texture& t, uint64_t off, uint64_t n, Ranges& d) {
    const uint64_t a = (t.gpu + off) & ~0xFFFull, b = (t.gpu + off + n + 0xFFF) & ~0xFFFull;
    const uint64_t lo = std::max(a, t.gpu) - t.gpu, hi = std::min(b, t.gpu + t.guest_size) - t.gpu;
    d.push_back({lo, hi});   // (in any order: merged below)
    std::sort(d.begin(), d.end());
    Ranges m;
    for (const auto& r : d)
        if (!m.empty() && r.first <= m.back().second) m.back().second = std::max(m.back().second, r.second);
        else m.push_back(r);
    d.swap(m);
}

struct Image {
    std::vector<uint8_t> b;
    int uploads = 0;           // (uploads made)
    uint64_t bytes = 0;        // (and their bytes)
    uint64_t from_memory = 0;  // (bytes the upload read from memory, not the check's copy)
};
/// (Triforce, 11-16) upload_astc_gpu as far as the check sees it: the layers dirty overlaps (all of them without dirty), each
/// staged from tail0 to its end (cut at the storage's end), read from the check's copy where one is under way (same_snap_read),
/// else as submitted (guest_bytes). bypass: the first layer read past the copy. bc1_alpha: an image kept as BC1 whose staged blocks
/// have alpha now: read, then the texture made BC3 and upload_astc_gpu called again for the whole of it (dirty = nullptr)
static void astc_stage(Texture& t, const Ranges* dirty, Image& img, bool bypass, bool bc1_alpha) {
    const uint64_t total = t.guest_size, layer_size = astc_layer_size(t), tail0 = level_geo(t, 0).offset;
    const uint64_t tail = layer_size - tail0;
    std::vector<uint8_t> own;
    bool first = true;
    for (uint32_t layer = 0; layer < t.layers; layer++) {
        const uint64_t at = (uint64_t)layer * layer_size + tail0;
        if (dirty && !upload_same::reaches(*dirty, at, (uint64_t)(layer + 1) * layer_size)) continue;
        if (at >= total) continue;
        const uint64_t n = std::min(tail, total - at);
        const bool past = bypass && first;
        first = false;
        const uint8_t* src = past ? nullptr : same_snap_read(t, at, n);
        if (past) same_snap_read(t, at + n - 1, 2);   // (a read the copy does not cover: short)
        own.resize(n);
        for (uint64_t i = 0; i < n; i++) own[i] = src ? src[i] : submitted(t, at + i);
        if (!src) img.from_memory += n;
        img.bytes += n;
        if (!bc1_alpha) std::memcpy(img.b.data() + at, own.data(), n);   // (staged and decoded into the image)
    }
    if (bc1_alpha) astc_stage(t, nullptr, img, false, false);   // (bc1_retype, then upload_astc_gpu(tex, total, nullptr))
}
static void upload_astc(Texture& t, const Ranges* dirty, Image& img, bool bypass = false, bool bc1_alpha = false) {
    upload_same::changed(t.same);   // (upload())
    img.uploads++;
    astc_stage(t, dirty, img, bypass, bc1_alpha);
}
static std::vector<uint32_t> reached_scratch;
/// upload_inner's CPU path as far as the check sees it: every unit dirty reaches read whole, runs of them at once, from the
/// check's copy where one is under way (same_snap_read), else as submitted; written into the image. bypass: one run read past
/// the copy (an upload_inner that went another way than same_units says)
static void upload_from(Texture& t, const Ranges& dirty, Image& img, bool bypass = false) {
    if (t.astc) return upload_astc(t, &dirty, img, bypass);   // ((Triforce) upload_inner sends ASTC to upload_astc_gpu)
    upload_same::changed(t.same);   // (upload())
    upload_same::reached(t.same, dirty, reached_scratch);
    img.uploads++;
    Ranges runs;
    for (uint32_t i : reached_scratch) {
        const upload_same::Unit& u = t.same.units[i];
        if (!runs.empty() && u.lo <= runs.back().second) runs.back().second = std::max(runs.back().second, u.hi);
        else runs.push_back({u.lo, u.hi});
    }
    for (size_t k = 0; k < runs.size(); k++) {
        const auto& r = runs[k];
        const uint64_t n = r.second - r.first;
        const uint8_t* snap = bypass && k == 0 ? nullptr : same_snap_read(t, r.first, n);
        if (bypass && k == 0) same_snap_read(t, r.first + n - 1, 2);   // (a read the copy does not cover: short)
        for (uint64_t i = 0; i < n; i++) img.b[r.first + i] = snap ? snap[i] : submitted(t, r.first + i);
        if (!snap) img.from_memory += n;
        img.bytes += n;
    }
}
/// The whole storage into the image unchecked (an upload texture_touch makes without the check: a GPU buffer's write, a whole one)
static void upload_whole(Texture& t, Image& img) {
    upload_same::changed(t.same);
    for (uint64_t i = 0; i < t.guest_size; i++) img.b[i] = submitted(t, i);
    img.uploads++;
}
/// texture_touch's flow for a partial re-upload: same_look, then the upload (unless 1) and same_done. The verdict
static int touch(Texture& t, Ranges dirty, Image& img, bool bypass = false, bool bc1_alpha = false) {
    widen_of = &t;
    const int v = same_look(t, dirty);
    if (v == 1) return 1;
    if (bc1_alpha) upload_astc(t, &dirty, img, bypass, true);
    else upload_from(t, dirty, img, bypass);   // (dirty: the units whose bytes changed, after a check)
    if (v == 2) same_done(t, true);
    return v;
}
static bool image_is_submitted(const Texture& t, const Image& img, const Ranges& over0) {
    const Ranges over = image_part(t, over0);
    for (const auto& r : over)
        for (uint64_t i = r.first; i < r.second; i++)
            if (img.b[i] != submitted(t, i)) return false;
    return true;
}
static void reset() {
    std::fill(guest.begin(), guest.end(), 0);
    mem::drop_all();
    mem::overlay = true;
    frame_no = 1;
}

// ---- the cases ----
static void case_hash() {
    std::vector<uint8_t> a(10000);
    for (size_t i = 0; i < a.size(); i++) a[i] = (uint8_t)(i * 7 + 3);
    const uint64_t h = upload_same::content_hash(a.data(), a.size());
    CHECK(h == upload_same::content_hash(a.data(), a.size()));
    for (size_t at : {0ul, 1ul, 63ul, 64ul, 511ul, 4096ul, 9983ul, 9999ul}) {   // (every part of the walk: the chains, the words, the tail)
        a[at] ^= 1;
        CHECK(upload_same::content_hash(a.data(), a.size()) != h);
        a[at] ^= 1;
    }
    CHECK(upload_same::content_hash(a.data(), 9999) != upload_same::content_hash(a.data(), 10000));
    // (a unit's hash: its page parts folded in order, as the visit gives them)
    const uint64_t at = 0x10f0000a00ull;
    upload_same::UnitHash u(a.size());
    u.add(a.data(), 0x600);
    u.add(a.data() + 0x600, 0x1000);
    u.add(a.data() + 0x1600, 0x1000);
    u.add(a.data() + 0x2600, a.size() - 0x2600);
    CHECK(upload_same::hash_at(a.data(), at, a.size()) == u.done());
    CHECK(upload_same::hash_at(a.data(), at, a.size()) != 0);
    upload_same::UnitHash z(0);
    CHECK(z.done() != 0);
}

static void case_units() {
    std::vector<upload_same::Unit> u;
    upload_same::add_units(u, 1000, 300, 100, 1250);   // (rows of 100, the storage ending in the third)
    CHECK(u.size() == 3 && u[0].lo == 1000 && u[0].hi == 1100 && u[2].lo == 1200 && u[2].hi == 1250);
    u.clear();
    upload_same::add_units(u, 0, 250, 100, 10000);   // (the span's end cuts the last)
    CHECK(u.size() == 3 && u[2].hi == 250);
    u.clear();
    upload_same::add_units(u, 5000, 300, 300, 4000);   // (none from the storage's end on)
    CHECK(u.empty());
    u.clear();
    upload_same::add_units(u, 300, 100, 100, 1000);
    upload_same::add_units(u, 0, 100, 100, 1000);
    upload_same::sort_units(u);
    CHECK(u[0].lo == 0 && u[1].lo == 300);
    // (same_units: a block-linear BC texture goes by GOB-block rows, level by level and layer by layer)
    reset();
    Texture t = bc_texture();
    widen_of = &t;
    same_units(t, t.same.units);
    CHECK(t.same.units.size() == 12);
    const uint64_t want[12][2] = {{0, 32768}, {32768, 65536}, {65536, 73728}, {73728, 81920}, {81920, 114688}, {114688, 147456},
                                  {147456, 155648}, {155648, 163840}, {163840, 196608}, {196608, 229376}, {229376, 237568},
                                  {237568, 245760}};
    for (size_t i = 0; i < 12 && i < t.same.units.size(); i++)
        CHECK(t.same.units[i].lo == want[i][0] && t.same.units[i].hi == std::min<uint64_t>(want[i][1], t.guest_size));
    // (a linear one, a widened one and one of several slices go whole, each level and layer)
    for (int k = 0; k < 3; k++) {
        Texture w = bc_texture();
        if (k == 0) w.d.linear = true, w.d.stride = 1024;
        if (k == 1) w.widen = true;
        if (k == 2) w.geo[0].td = 4;
        widen_of = &w;
        same_units(w, w.same.units);
        CHECK(w.same.units.size() == (k == 2 ? 9u : 6u));   // (several slices: level 0 whole, level 1 by rows)
        if (k == 0) CHECK(w.same.units[0].lo == 0 && w.same.units[0].hi == 1024ull * 40);   // (stride x block rows)
        if (k == 1) CHECK(w.same.units[0].lo == 0 && w.same.units[0].hi == 65536);
    }
    // (a level and layer the storage ends in is cut; one past it has none)
    Texture c = bc_texture();
    c.guest_size = 100 << 10;
    widen_of = &c;
    same_units(c, c.same.units);
    CHECK(c.same.units.size() == 5 && c.same.units.back().hi == (100u << 10));
}

static void case_check_rules() {
    upload_same::State s;
    upload_same::add_units(s.units, 0, 400, 100, 400);
    std::vector<uint8_t> bytes(400, 1);
    int looked = 0;
    auto now = [&](const upload_same::Unit& u) {
        looked++;
        return upload_same::hash_at(bytes.data() + u.lo, kBase + u.lo, u.hi - u.lo);
    };
    std::vector<uint32_t> up, read;
    // (nothing known: every unit reached is uploaded, none looked at)
    upload_same::Verdict v = upload_same::check(s, upload_same::Ranges{{0, 400}}, now, up);
    CHECK(up.size() == 4 && looked == 0 && v.changed == 400 && v.same == 0 && v.known == 0);
    // (the upload of those: one change, from the copy: the hashes are the image's)
    std::vector<uint64_t> hashes;
    for (uint32_t i : up) hashes.push_back(now(s.units[i]));
    looked = 0;
    upload_same::changed(s);
    upload_same::commit(s, up, hashes, true);
    // (the same bytes again: nothing to upload, each reached unit looked at once)
    v = upload_same::check(s, upload_same::Ranges{{0, 400}}, now, up);
    CHECK(up.empty() && looked == 4 && v.same == 400 && v.known == 400 && v.changed == 0);
    // (a write reaching one unit: only it looked at; its bytes changed: only it uploaded)
    looked = 0;
    bytes[250] = 9;
    v = upload_same::check(s, upload_same::Ranges{{200, 300}}, now, up);
    CHECK(up.size() == 1 && up[0] == 2 && looked == 1 && v.changed == 100);
    upload_same::Ranges r;
    upload_same::ranges_of(s, up, r);
    CHECK(r.size() == 1 && r[0].first == 200 && r[0].second == 300);
    upload_same::reached(s, upload_same::Ranges{{150, 210}}, read);
    CHECK(read.size() == 2 && read[0] == 1 && read[1] == 2);
    // (another change to the image between the check and the commit (a GPU write): forgotten, all of them)
    upload_same::changed(s);   // (the GPU)
    upload_same::changed(s);   // (the upload)
    upload_same::commit(s, up, std::vector<uint64_t>{now(s.units[2])}, true);
    looked = 0;
    v = upload_same::check(s, upload_same::Ranges{{0, 400}}, now, up);
    CHECK(up.size() == 4 && looked == 0);
    // (kept again; then a GPU write after the commit: forgotten at the next check)
    hashes.clear();
    for (uint32_t i : up) hashes.push_back(now(s.units[i]));
    upload_same::changed(s);
    upload_same::commit(s, up, hashes, true);
    upload_same::changed(s);   // (texture_written)
    v = upload_same::check(s, upload_same::Ranges{{0, 400}}, now, up);
    CHECK(up.size() == 4 && v.known == 0);
    // (an upload that read memory somewhere, or whose hashes do not match the units read: forgotten)
    upload_same::changed(s);
    upload_same::commit(s, up, hashes, false);
    v = upload_same::check(s, upload_same::Ranges{{0, 400}}, now, up);
    CHECK(up.size() == 4 && v.known == 0);
    upload_same::changed(s);
    upload_same::commit(s, up, std::vector<uint64_t>{1, 2}, true);
    v = upload_same::check(s, upload_same::Ranges{{0, 400}}, now, up);
    CHECK(up.size() == 4 && v.known == 0);
    // (units the upload did not read keep their hashes)
    hashes.clear();
    for (uint32_t i : up) hashes.push_back(now(s.units[i]));
    upload_same::changed(s);
    upload_same::commit(s, up, hashes, true);
    bytes[10] = 5;
    v = upload_same::check(s, upload_same::Ranges{{0, 100}}, now, up);
    CHECK(up.size() == 1 && up[0] == 0);
    upload_same::changed(s);
    upload_same::commit(s, up, std::vector<uint64_t>{now(s.units[0])}, true);
    looked = 0;
    v = upload_same::check(s, upload_same::Ranges{{0, 400}}, now, up);
    CHECK(up.empty() && looked == 4 && v.same == 400);
}

static void case_misses() {
    upload_same::State s;
    upload_same::add_units(s.units, 0, 200, 100, 200);
    uint64_t serial = 100;
    auto other = [&](const upload_same::Unit&) { return ++serial; };   // (every look: other bytes)
    auto keep_all = [&]() {
        std::vector<uint32_t> all{0, 1};
        upload_same::changed(s);
        upload_same::commit(s, all, std::vector<uint64_t>{7, 8}, true);
    };
    std::vector<uint32_t> up;
    upload_same::check(s, upload_same::Ranges{{0, 200}}, other, up);   // (nothing known: no miss)
    CHECK(s.misses == 0);
    for (uint32_t k = 0; k < upload_same::kMisses; k++) {
        CHECK(upload_same::take(s));
        keep_all();
        upload_same::check(s, upload_same::Ranges{{0, 200}}, other, up);
    }
    CHECK(s.rest == upload_same::kRest && s.misses == 0);
    for (uint32_t k = 0; k < upload_same::kRest; k++) CHECK(!upload_same::take(s));
    CHECK(upload_same::take(s));
    // (a check that found anything the same starts the count again)
    keep_all();
    upload_same::check(s, upload_same::Ranges{{0, 200}}, other, up);
    CHECK(s.misses == 1);
    keep_all();
    auto half = [&](const upload_same::Unit& u) { return u.lo == 0 ? (uint64_t)7 : ++serial; };
    upload_same::check(s, upload_same::Ranges{{0, 200}}, half, up);
    CHECK(s.misses == 0 && up.size() == 1);
}

static void case_copy() {
    std::vector<uint8_t> b(1000, 3);
    upload_same::Copy c;
    c.bytes = b.data(), c.total = 1000, c.filled = {{100, 300}, {500, 1000}};
    CHECK(upload_same::copy_read(c, 100, 200) == b.data() + 100 && c.used && !c.short_read);
    CHECK(upload_same::copy_read(c, 600, 400) == b.data() + 600 && !c.short_read);
    CHECK(upload_same::copy_read(c, 0, 0) == nullptr && !c.short_read);
    CHECK(upload_same::copy_read(c, 250, 100) == nullptr && c.short_read);   // (past a filled range)
    c.short_read = false;
    CHECK(upload_same::copy_read(c, 900, 200) == nullptr && c.short_read);   // (past the storage)
    c.short_read = false;
    CHECK(upload_same::copy_read(c, 50, 100) == nullptr && c.short_read);    // (before one)
}

static void case_visit() {
    reset();
    for (size_t i = 0; i < 0x4000; i++) guest[i] = (uint8_t)(i >> 4);
    std::vector<uint8_t> other(4096, 0xAB);
    mem::keep(kBase + 0x1000, other.data());   // (the second page: a kept capture's copy)
    struct Part {
        uint64_t off, len;
        bool from_copy;
    };
    std::vector<Part> parts;
    static const uint8_t* live;   // (where the visit's guest memory begins)
    auto fn = [](void* c, uint64_t off, const uint8_t* b, uint64_t len) {
        auto* v = static_cast<std::vector<Part>*>(c);
        const bool copy = std::all_of(b, b + len, [](uint8_t x) { return x == 0xAB; });
        v->push_back(Part{off, len, copy});
        CHECK(copy || b == live + off);   // (guest memory itself where no copy is kept)
    };
    live = guest.data() + 0x200;
    memory_visit_as_submitted(kBase + 0x200, guest.data() + 0x200, 0x2400, fn, &parts);
    CHECK(parts.size() == 3);
    if (parts.size() == 3) {
        CHECK(parts[0].off == 0 && parts[0].len == 0xE00 && !parts[0].from_copy);
        CHECK(parts[1].off == 0xE00 && parts[1].len == 0x1000 && parts[1].from_copy);
        CHECK(parts[2].off == 0x1E00 && parts[2].len == 0x600 && !parts[2].from_copy);
    }
    parts.clear();
    mem::overlay = false;   // (the A/B file: guest memory as it is)
    memory_visit_as_submitted(kBase + 0x200, guest.data() + 0x200, 0x2400, fn, &parts);
    CHECK(parts.size() == 3 && !parts[1].from_copy);
    parts.clear();
    mem::overlay = true;
    mem::drop_all();
    live = guest.data() + 0x1000;
    memory_visit_as_submitted(kBase + 0x1000, guest.data() + 0x1000, 0x1000, fn, &parts);
    CHECK(parts.size() == 1 && parts[0].len == 0x1000 && !parts[0].from_copy);
    parts.clear();
    memory_visit_as_submitted(kBase, guest.data(), 0, fn, &parts);
    CHECK(parts.empty());
}

/// memory_hash_as_submitted: a whole kept page hashed once and its hash kept with its copy, until other bytes are copied in; a
/// part of a page, and guest memory where no copy is kept, hashed each time; and a unit's hash the same whichever way it is
/// taken (the check's against the copy's, hash_at)
static void case_kept() {
    reset();
    std::mt19937 rng(7);
    for (size_t i = 0; i < 0x6000; i++) guest[i] = (uint8_t)rng();
    std::vector<uint8_t> page(4096);
    for (auto& x : page) x = (uint8_t)rng();
    mem::keep(kBase + 0x1000, page.data());
    mem::keep(kBase + 0x3000, page.data());
    struct Part {
        uint64_t off, len, hash;
    };
    std::vector<Part> parts;
    auto fn = [](void* c, uint64_t off, uint64_t len, uint64_t h) { static_cast<std::vector<Part>*>(c)->push_back(Part{off, len, h}); };
    mem::memory_hash_as_submitted(kBase + 0x800, guest.data() + 0x800, 0x3000, fn, &parts);   // (+0x800 .. +0x3800)
    CHECK(parts.size() == 4);
    if (parts.size() == 4) {
        CHECK(parts[0].off == 0 && parts[0].len == 0x800 && parts[0].hash == upload_same::content_hash(guest.data() + 0x800, 0x800));
        CHECK(parts[1].off == 0x800 && parts[1].len == 0x1000 && parts[1].hash == upload_same::content_hash(page.data(), 4096));
        CHECK(parts[2].hash == upload_same::content_hash(guest.data() + 0x2000, 0x1000));
        CHECK(parts[3].len == 0x800 && parts[3].hash == upload_same::content_hash(page.data(), 0x800));   // (the kept copy's part)
    }
    CHECK(mem::shadow[(uint32_t)((kBase + 0x1000) >> 12)].hash == upload_same::content_hash(page.data(), 4096));   // (kept)
    CHECK(mem::shadow[(uint32_t)((kBase + 0x3000) >> 12)].hash == 0);                                               // (a part of it only: not kept)
    // (the kept hash is the one given while the copy keeps it: the copy's bytes changed behind its back show the old one)
    mem::shadow[(uint32_t)((kBase + 0x1000) >> 12)].b[5] ^= 1;
    parts.clear();
    mem::memory_hash_as_submitted(kBase + 0x1000, guest.data() + 0x1000, 0x1000, fn, &parts);
    CHECK(parts.size() == 1 && parts[0].hash == upload_same::content_hash(page.data(), 4096));
    mem::shadow[(uint32_t)((kBase + 0x1000) >> 12)].b[5] ^= 1;
    // (a capture that copies the same bytes in keeps it; other bytes and it goes, and the next look hashes them)
    mem::keep(kBase + 0x1000, page.data());
    CHECK(mem::shadow[(uint32_t)((kBase + 0x1000) >> 12)].hash == upload_same::content_hash(page.data(), 4096));
    page[100] ^= 0x40;
    mem::keep(kBase + 0x1000, page.data());
    CHECK(mem::shadow[(uint32_t)((kBase + 0x1000) >> 12)].hash == 0);
    parts.clear();
    mem::memory_hash_as_submitted(kBase + 0x1000, guest.data() + 0x1000, 0x1000, fn, &parts);
    CHECK(parts.size() == 1 && parts[0].hash == upload_same::content_hash(page.data(), 4096));
    // (the A/B file: guest memory as it is, no kept hash used)
    mem::overlay = false;
    parts.clear();
    mem::memory_hash_as_submitted(kBase + 0x1000, guest.data() + 0x1000, 0x1000, fn, &parts);
    CHECK(parts.size() == 1 && parts[0].hash == upload_same::content_hash(guest.data() + 0x1000, 4096));
    mem::overlay = true;
    // (a unit's hash: the check's way (kept hashes) and the copy's way (hash_at over the bytes as submitted) agree)
    Texture t = bc_texture(kBase + 0x200);
    widen_of = &t;
    same_units(t, t.same.units);
    std::vector<uint8_t> sub(t.guest_size);
    for (uint64_t i = 0; i < t.guest_size; i++) sub[i] = submitted(t, i);
    for (const upload_same::Unit& u : t.same.units)
        CHECK(same_hash_now(t, u) == upload_same::hash_at(sub.data() + u.lo, t.gpu + u.lo, u.hi - u.lo));
}

static void case_skip() {
    reset();
    std::mt19937 rng(1);
    Texture t = bc_texture();
    Image img;
    img.b.assign(t.guest_size, 0);
    fill_random(t, 0, t.guest_size, rng);
    // (the first re-upload: nothing known yet, all of it uploaded from the check's copy)
    CHECK(touch(t, all_of(t), img) == 2);
    CHECK(img.uploads == 1 && img.from_memory == 0 && image_is_submitted(t, img, all_of(t)));
    // (written again whole with the same bytes: nothing uploaded)
    std::vector<uint8_t> was(guest.begin() + (t.gpu - kBase), guest.begin() + (t.gpu - kBase) + t.guest_size);
    write(t, 0, t.guest_size, 0x55);
    std::memcpy(guest.data() + (t.gpu - kBase), was.data(), was.size());
    CHECK(touch(t, all_of(t), img) == 1);
    CHECK(img.uploads == 1 && image_is_submitted(t, img, all_of(t)));
    // (a page of it written with other bytes: that unit's row alone uploaded)
    write(t, 90000, 16, 0x77);
    Ranges d;
    dirty_pages(t, 90000, 16, d);
    const uint64_t bytes0 = img.bytes;
    CHECK(touch(t, d, img) == 2);
    CHECK(img.uploads == 2 && img.bytes - bytes0 == 32768 && image_is_submitted(t, img, all_of(t)));
    // (a page the storage shares with the next thing in memory, written there: the texture's own bytes the same, nothing uploaded)
    write(t, t.guest_size, 64, 0x99);
    d.clear();
    dirty_pages(t, t.guest_size - 1, 2, d);
    CHECK(touch(t, d, img) == 1);
}

static void case_aba() {
    reset();
    std::mt19937 rng(2);
    Texture t = bc_texture();
    Image img;
    img.b.assign(t.guest_size, 0);
    fill_random(t, 0, t.guest_size, rng);
    CHECK(touch(t, all_of(t), img) == 2);
    const std::vector<uint8_t> a(guest.begin() + (t.gpu - kBase), guest.begin() + (t.gpu - kBase) + t.guest_size);
    // (B uploaded; A back: A uploaded again, not taken for the image's. The writes reach the next row too, with its own bytes)
    fill_random(t, 0, 4096, rng);
    Ranges d;
    dirty_pages(t, 0, 40000, d);
    CHECK(touch(t, d, img) == 2);
    std::memcpy(guest.data() + (t.gpu - kBase), a.data(), 4096);
    CHECK(touch(t, d, img) == 2);
    CHECK(image_is_submitted(t, img, all_of(t)));
    // (the game writes while the upload is under way: the upload reads the check's copy, whose hashes are kept: back to the
    // copy's bytes before the next look is nothing to upload, the other bytes kept are uploaded)
    fill_random(t, 0, 4096, rng);
    widen_of = &t;
    Ranges dd = d;
    CHECK(same_look(t, dd) == 2);
    const Texture other = bc_texture(t.gpu);   // (another texture's upload meanwhile reads memory, not this copy)
    CHECK(same_snap_read(other, 0, 4096) == nullptr && same_snap_read(t, 0, 4096) != nullptr);
    const std::vector<uint8_t> copy_bytes(guest.begin() + (t.gpu - kBase), guest.begin() + (t.gpu - kBase) + 32768);
    fill_random(t, 0, 4096, rng);   // (meanwhile)
    upload_from(t, dd, img);
    same_done(t, true);
    CHECK(img.from_memory == 0 && std::equal(copy_bytes.begin(), copy_bytes.end(), img.b.begin()));
    const std::vector<uint8_t> meanwhile(guest.begin() + (t.gpu - kBase), guest.begin() + (t.gpu - kBase) + 4096);
    std::memcpy(guest.data() + (t.gpu - kBase), copy_bytes.data(), 4096);
    CHECK(touch(t, d, img) == 1 && image_is_submitted(t, img, all_of(t)));
    std::memcpy(guest.data() + (t.gpu - kBase), meanwhile.data(), 4096);
    CHECK(touch(t, d, img) == 2 && image_is_submitted(t, img, all_of(t)));
}

static void case_forget() {
    reset();
    std::mt19937 rng(3);
    Texture t = bc_texture();
    Image img;
    img.b.assign(t.guest_size, 0);
    fill_random(t, 0, t.guest_size, rng);
    CHECK(touch(t, all_of(t), img) == 2);
    // (the GPU wrote the image (texture_written): the same bytes in memory are not the image's)
    for (auto& x : img.b) x = 0xEE;
    upload_same::changed(t.same);
    CHECK(touch(t, all_of(t), img) == 2 && image_is_submitted(t, img, all_of(t)));
    // (an upload not checked (a GPU buffer's write over its storage, a whole one): the same)
    upload_whole(t, img);
    CHECK(touch(t, all_of(t), img) == 2);
    CHECK(touch(t, all_of(t), img) == 1);
    // (an upload that read past the copy: its hashes forgotten, the next re-upload of the same bytes made)
    Ranges d;
    dirty_pages(t, 0, 70000, d);
    fill_random(t, 0, 70000, rng);
    CHECK(touch(t, d, img, true) == 2);
    CHECK(touch(t, all_of(t), img) == 2 && image_is_submitted(t, img, all_of(t)));
    CHECK(touch(t, all_of(t), img) == 1);
    // (and when the read past the copy found other bytes than the copy's (the game wrote meanwhile) while the rest came from the
    // copy: the image has those other bytes there, and the copy's bytes back are uploaded, not taken for the image's)
    Ranges two;
    dirty_pages(t, 0, 100, two);
    dirty_pages(t, 200000, 100, two);
    fill_random(t, 0, 100, rng);
    fill_random(t, 200000, 100, rng);
    widen_of = &t;
    Ranges dd = two;
    CHECK(same_look(t, dd) == 2 && dd.size() == 2);
    const std::vector<uint8_t> copied(guest.begin() + (t.gpu - kBase), guest.begin() + (t.gpu - kBase) + 4096);
    fill_random(t, 0, 100, rng);   // (meanwhile: what the read past the copy finds)
    upload_from(t, dd, img, true);
    same_done(t, true);
    std::memcpy(guest.data() + (t.gpu - kBase), copied.data(), copied.size());
    Ranges first;
    dirty_pages(t, 0, 100, first);
    CHECK(touch(t, first, img) == 2 && image_is_submitted(t, img, all_of(t)));
}

static void case_submitted() {
    reset();
    std::mt19937 rng(4);
    Texture t = bc_texture();
    Image img;
    img.b.assign(t.guest_size, 0);
    fill_random(t, 0, t.guest_size, rng);
    // (the capture kept the first pages as they were at the submit; the game rewrote them since)
    for (uint64_t a = t.gpu & ~0xFFFull; a < t.gpu + 0x3000; a += 0x1000) mem::keep_now(a);
    fill_random(t, 0, 0x2000, rng);
    CHECK(touch(t, all_of(t), img) == 2);
    CHECK(image_is_submitted(t, img, all_of(t)));
    // (the next submit's capture took the new bytes: other than the image's, uploaded)
    for (uint64_t a = t.gpu & ~0xFFFull; a < t.gpu + 0x3000; a += 0x1000) mem::keep_now(a);
    Ranges d;
    dirty_pages(t, 0, 0x2000, d);
    CHECK(touch(t, d, img) == 2 && image_is_submitted(t, img, all_of(t)));
    // (and a capture's copy with the bytes the image has, over guest memory that has others: nothing to upload)
    std::vector<uint8_t> page(4096);
    const uint64_t p = (t.gpu + 0x10000) & ~0xFFFull;
    std::memcpy(page.data(), guest.data() + (p - kBase), 4096);
    mem::keep(p, page.data());
    write(t, p - t.gpu, 4096, 0x31);
    d.clear();
    dirty_pages(t, p - t.gpu, 4096, d);
    CHECK(touch(t, d, img) == 1 && image_is_submitted(t, img, all_of(t)));
}

static void case_unwanted() {
    reset();
    const int why[9] = {SAME_KIND, SAME_KIND, SAME_KIND, SAME_KIND, SAME_KIND, SAME_SIZE, SAME_SIZE, SAME_KIND, SAME_KIND};
    for (int k = 0; k < 9; k++) {
        Texture t = k == 0 ? astc_texture() : bc_texture();
        if (k == 0) t.type = VK_IMAGE_TYPE_3D;                                   // (ASTC of a 3D image: not upload_astc_gpu's)
        if (k == 1) t.bc = true;                                                 // (kept as BC3, not from ASTC)
        if (k == 2) t.sparse = true;
        if (k == 3) t.samples = VK_SAMPLE_COUNT_4_BIT;
        if (k == 4) t.aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        if (k == 5) t.format = FMT_RGBA8, t.guest_size = 8u << 20;               // (unswizzled on the GPU)
        if (k == 6) t.guest_size = upload_same::kMaxBytes + 4096;
        if (k == 7) t.gpu = kBase + kMem - 4096;                                 // (not in one piece)
        if (k == 8) t.image = nullptr;
        Ranges d = all_of(t);
        widen_of = &t;
        CHECK(same_look(t, d) == 0 && t.same.units.empty() && d.size() == 1 && d[0].second == t.guest_size);
        CHECK(tl_same_why == why[k]);
    }
    // ((Triforce) ASTC decoded on the GPU of 8 to 16 MB: its image is RGBA8 (or BC), not one the 8 MB rule is for: checked. Not
    // over kMaxBytes)
    Texture big = astc_texture();
    big.guest_size = 10u << 20;
    Ranges bd = all_of(big);
    widen_of = &big;
    CHECK(same_look(big, bd) == 2 && big.same.units.size() == 3);
    same_done(big, false);
    big = astc_texture();
    big.guest_size = upload_same::kMaxBytes + 4096;
    bd = all_of(big);
    widen_of = &big;
    CHECK(same_look(big, bd) == 0 && tl_same_why == SAME_SIZE);
    Texture small = bc_texture();
    small.format = FMT_RGBA8, small.bpb = 4;   // (uncompressed under 8 MB: the CPU's upload, checked)
    Ranges d = all_of(small);
    widen_of = &small;
    CHECK(same_look(small, d) == 2);
    same_done(small, false);
}

static void case_rest() {
    reset();
    std::mt19937 rng(5);
    Texture t = bc_texture();
    Image img;
    img.b.assign(t.guest_size, 0);
    fill_random(t, 0, t.guest_size, rng);
    CHECK(touch(t, all_of(t), img) == 2);
    for (uint32_t k = 0; k < upload_same::kMisses; k++) {   // (the CPU really rewrites it, every byte every time)
        fill_random(t, 0, t.guest_size, rng);
        CHECK(touch(t, all_of(t), img) == 2);
    }
    for (uint32_t k = 0; k < upload_same::kRest; k++) {
        fill_random(t, 0, t.guest_size, rng);
        CHECK(touch(t, all_of(t), img) == 0);
        CHECK(image_is_submitted(t, img, all_of(t)));
    }
    CHECK(touch(t, all_of(t), img) == 2);   // (checked again: nothing known after the unchecked ones)
    CHECK(touch(t, all_of(t), img) == 1);
}

static void case_off() {
    reset();
    Texture t = bc_texture();
    Ranges d = all_of(t);
    widen_of = &t;
    CHECK(same_look(t, d) == 0);
}

/// (Triforce, 11-16) ASTC decoded on the GPU (upload_astc_gpu): one unit a layer from tail0 on, staged through the check's copy:
/// the same bytes again stage nothing, a layer changed is staged alone, A B A stages A again, a write only into the levels the
/// image leaves out stages nothing, a BC1 image's whole read again past the copy forgets the hashes, as a stage past the copy does
static void case_astc() {
    reset();
    std::mt19937 rng(8);
    Texture t = astc_texture();
    widen_of = &t;
    same_units(t, t.same.units);
    CHECK(t.same.units.size() == 3);
    const uint64_t ls = 80 << 10, tail0 = 4096, tail = ls - tail0;
    for (size_t i = 0; i < 3 && i < t.same.units.size(); i++)
        CHECK(t.same.units[i].lo == i * ls + tail0 && t.same.units[i].hi == (i + 1) * ls);
    t.same.units.clear();
    Image img;
    img.b.assign(t.guest_size, 0);
    fill_random(t, 0, t.guest_size, rng);
    // (the first re-upload: nothing known, the three layers staged from the check's copy)
    CHECK(touch(t, all_of(t), img) == 2);
    CHECK(img.uploads == 1 && img.bytes == 3 * tail && img.from_memory == 0 && image_is_submitted(t, img, all_of(t)));
    // (the same bytes written again, all of it: nothing staged, nothing decoded)
    const std::vector<uint8_t> was(guest.begin() + (t.gpu - kBase), guest.begin() + (t.gpu - kBase) + t.guest_size);
    write(t, 0, t.guest_size, 0x55);
    std::memcpy(guest.data() + (t.gpu - kBase), was.data(), was.size());
    CHECK(touch(t, all_of(t), img) == 1 && img.uploads == 1);
    // (a page of the second layer written with other bytes: that layer alone, from the copy, and its hash kept)
    write(t, ls + 30000, 16, 0x77);
    Ranges d;
    dirty_pages(t, ls + 30000, 16, d);
    uint64_t bytes0 = img.bytes;
    CHECK(touch(t, d, img) == 2);
    CHECK(img.uploads == 2 && img.bytes - bytes0 == tail && img.from_memory == 0 && image_is_submitted(t, img, all_of(t)));
    CHECK(touch(t, d, img) == 1 && touch(t, all_of(t), img) == 1 && img.uploads == 2);
    // (A, B, A in the first layer: B staged; A back staged again, not taken for the image's)
    const std::vector<uint8_t> a(guest.begin() + (t.gpu - kBase), guest.begin() + (t.gpu - kBase) + 8192);
    fill_random(t, tail0, 4096, rng);
    d.clear();
    dirty_pages(t, tail0, 4096, d);
    CHECK(touch(t, d, img) == 2 && image_is_submitted(t, img, all_of(t)));
    std::memcpy(guest.data() + (t.gpu - kBase), a.data(), a.size());
    CHECK(touch(t, d, img) == 2 && image_is_submitted(t, img, all_of(t)));
    CHECK(touch(t, all_of(t), img) == 1);
    // (a write only before a layer's tail0, into levels the image leaves out: no unit reached, nothing staged, as upload_astc_gpu
    // finds no layer to stage)
    const int up0 = img.uploads;
    for (uint64_t off : {(uint64_t)0, ls, 2 * ls}) {
        write(t, off, tail0, 0x13);
        d.clear();
        d.push_back({off, off + tail0});
        CHECK(touch(t, d, img) == 1);
    }
    CHECK(img.uploads == up0 && image_is_submitted(t, img, all_of(t)));
    // (an image kept as BC1 whose layer has alpha now: its staged layer read from the copy, then the texture made BC3 and all of
    // it read again (dirty = nullptr): the other layers past the copy, short: the hashes forgotten, the next look uploads)
    fill_random(t, 2 * ls + 10000, 100, rng);
    d.clear();
    dirty_pages(t, 2 * ls + 10000, 100, d);
    bytes0 = img.bytes;
    const uint64_t mem0 = img.from_memory;
    CHECK(touch(t, d, img, false, true) == 2);
    CHECK(img.bytes - bytes0 == 4 * tail && img.from_memory - mem0 == 2 * tail && image_is_submitted(t, img, all_of(t)));
    CHECK(touch(t, all_of(t), img) == 2);   // (nothing known)
    CHECK(touch(t, all_of(t), img) == 1);
    // (and every layer staged from the copy before the whole read: the whole read is the copy's too, all of the image: kept)
    fill_random(t, 0, t.guest_size, rng);
    CHECK(touch(t, all_of(t), img, false, true) == 2 && image_is_submitted(t, img, all_of(t)));
    CHECK(touch(t, all_of(t), img) == 1);
    // (a stage that read past the copy: forgotten)
    fill_random(t, ls + tail0, 4096, rng);
    d.clear();
    dirty_pages(t, ls + tail0, 4096, d);
    CHECK(touch(t, d, img, true) == 2 && image_is_submitted(t, img, all_of(t)));
    CHECK(touch(t, all_of(t), img) == 2 && touch(t, all_of(t), img) == 1);

    // (one layer whose layout's layer size is under the storage's: the storage is the layer, as upload_astc_gpu takes it)
    reset();
    Texture one = astc_texture();
    one.layers = 1, one.L.layer_size = 64 << 10, one.guest_size = 80 << 10;
    widen_of = &one;
    same_units(one, one.same.units);
    CHECK(one.same.units.size() == 1 && one.same.units[0].lo == tail0 && one.same.units[0].hi == one.guest_size);
    one.same.units.clear();
    Image oi;
    oi.b.assign(one.guest_size, 0);
    fill_random(one, 0, one.guest_size, rng);
    CHECK(touch(one, all_of(one), oi) == 2 && oi.bytes == one.guest_size - tail0 && image_is_submitted(one, oi, all_of(one)));
    write(one, 70000, 100, 0x42);
    d.clear();
    dirty_pages(one, 70000, 100, d);
    CHECK(touch(one, d, oi) == 2 && image_is_submitted(one, oi, all_of(one)));
    CHECK(touch(one, all_of(one), oi) == 1);

    // (a layout upload_astc_gpu does not take (tail0 not 16-aligned): no units, not checked)
    Texture odd = astc_texture();
    odd.geo[0].offset = 4100;
    Ranges od = all_of(odd);
    widen_of = &odd;
    CHECK(same_look(odd, od) == 0 && odd.same.units.empty() && tl_same_why == SAME_KIND);
    // (ASTC of a 3D image: not checked)
    Texture d3 = astc_texture();
    d3.type = VK_IMAGE_TYPE_3D;
    Ranges dd = all_of(d3);
    widen_of = &d3;
    CHECK(same_look(d3, dd) == 0 && d3.same.units.empty() && tl_same_why == SAME_KIND);
}

/// RX_VK_UPLOAD_SAME_ASTC=0: ASTC decoded on the GPU not checked, its re-uploads as before; the others checked all the same
static void case_astc_off() {
    reset();
    Texture t = astc_texture();
    Ranges d = all_of(t);
    widen_of = &t;
    CHECK(same_look(t, d) == 0 && t.same.units.empty() && tl_same_why == SAME_ASTC_OFF && d.size() == 1);
    Texture b = bc_texture();
    Ranges bd = all_of(b);
    widen_of = &b;
    CHECK(same_look(b, bd) == 2);
    same_done(b, false);
}

/// Writes of every kind (other bytes, the same bytes, back to an older version, through the capture's kept copies, at the
/// storage's shared edge pages), GPU writes, unchecked uploads, uploads that read past the copy and writes while an upload is
/// under way, in any order: after each look the image holds what the game had at the submit wherever the writes reached, and
/// after a last look over everything, everywhere
static void case_random() {
    reset();
    std::mt19937 rng(6);
    int all_skipped = 0, all_looked = 0, astc_skipped = 0, astc_looked = 0;
    for (int round = 0; round < 40; round++) {
        mem::drop_all();
        const uint64_t at = kBase + 0x10000 + 0x200 * (uint64_t)(rng() % 64);
        Texture t = round % 2 ? astc_texture(at) : bc_texture(at);   // ((Triforce) and ASTC decoded on the GPU)
        Image img;
        img.b.assign(t.guest_size, 0);
        fill_random(t, 0, t.guest_size, rng);
        upload_whole(t, img);
        std::vector<std::vector<uint8_t>> versions;
        Ranges d;
        int skipped = 0, looked = 0;
        for (int step = 0; step < 300; step++) {
            const uint32_t op = rng() % 12;
            const uint64_t off = rng() % t.guest_size, n = 1 + rng() % 40000;
            const uint64_t m = std::min(n, t.guest_size + 4096 - off);
            if (op <= 2) {   // (other bytes)
                fill_random(t, off, m, rng);
                dirty_pages(t, off, m, d);
            } else if (op <= 4) {   // (the same bytes again)
                dirty_pages(t, off, std::min(m, t.guest_size - off), d);
            } else if (op == 5) {   // (back to a version from before)
                if (!versions.empty()) {
                    const auto& v = versions[rng() % versions.size()];
                    std::memcpy(guest.data() + (t.gpu - kBase), v.data(), v.size());
                    dirty_pages(t, 0, t.guest_size, d);
                }
            } else if (op == 6) {   // (a capture keeps the pages written as they are now)
                for (const auto& r : d)
                    for (uint64_t a = (t.gpu + r.first) & ~0xFFFull; a < t.gpu + r.second; a += 0x1000) mem::keep_now(a);
            } else if (op == 7) {   // (the GPU drew into the image)
                for (uint64_t i = 0; i < 512; i++) img.b[(off + i) % t.guest_size] = (uint8_t)rng();
                upload_same::changed(t.same);
                upload_whole(t, img);   // (and the game's next write makes it the CPU's again: a whole upload, unchecked)
                d.clear();
            } else if (op == 8) {   // (a write while an upload is under way)
                if (!d.empty()) {
                    widen_of = &t;
                    Ranges dd = d;
                    const int v = same_look(t, dd);
                    if (v == 2) {
                        fill_random(t, off, std::min<uint64_t>(m, 8192), rng);
                        upload_from(t, dd, img);
                        same_done(t, true);
                        d.clear();
                        dirty_pages(t, off, std::min<uint64_t>(m, 8192), d);
                    } else if (v == 1) {
                        d.clear();
                    } else {
                        upload_from(t, d, img);
                        d.clear();
                    }
                }
            } else if (op == 9) {   // (an upload that read past the copy)
                if (!d.empty()) {
                    touch(t, d, img, true);
                    CHECK(image_is_submitted(t, img, d));
                    d.clear();
                }
            } else {   // (the look a draw makes)
                if (!d.empty()) {
                    const int v = touch(t, d, img);
                    looked++, skipped += v == 1;
                    CHECK(image_is_submitted(t, img, d));
                    d.clear();
                }
            }
            if (rng() % 16 == 0)
                versions.emplace_back(guest.begin() + (t.gpu - kBase), guest.begin() + (t.gpu - kBase) + t.guest_size);
        }
        touch(t, all_of(t), img);
        CHECK(image_is_submitted(t, img, all_of(t)));
        CHECK(looked > 0);
        all_skipped += skipped, all_looked += looked;
        if (t.astc) astc_looked += looked, astc_skipped += skipped;
    }
    std::printf("random: %d looks, %d of them nothing to upload (ASTC: %d, %d)\n", all_looked, all_skipped, astc_looked,
                astc_skipped);
    CHECK(astc_skipped * 10 > astc_looked);
    CHECK(all_skipped * 10 > all_looked);   // (the same bytes again are left out: not a test that uploads everything)
}

// ---- (Triforce, 11-16) the parallel replay's twin: two segments of one submission (upload_same.h, "the parallel replay's twin";
// vk_texture.cpp texture_touch, same_twin_look and same_twin_done) ----
// A submission's segments record at once, each into its own upload command buffer (submitted just before its own commands); the
// queue runs segment 0's uploads, its draws, segment 1's uploads, its draws. The model: each segment's uploads are the writes
// they make into the image, applied at the submit in queue order; segment 0's draws see the image after its own uploads, before
// segment 1's. texture_touch's look as a segment's worker makes it: once a submission (checked_epoch), the twin when a later
// segment uploaded it in this submission (upload_value, upload_seg).
struct SegCmds {
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> writes;   // (offset, bytes: in the order recorded)
    uint64_t bytes = 0;
    int uploads = 0;
};
struct Looks {   // (the fields of Texture texture_touch's twin goes by)
    uint64_t upload_value = 0, upload_seg = 0, checked = 0;
};
struct TwinCount {
    int twins = 0, whole = 0;
    uint64_t bytes = 0;
};
static TwinCount twin_count;
static uint64_t image_ids = 0x1000;
/// upload() as the twin needs it: counted as a change to the image (changed()), and noted with what it asked for and the image it
/// went into (note_upload, as upload() does unless RX_VK_TWIN_SAME=0). The units part reaches (all without part, or when the
/// image is made again by it: a BC1 image made BC3 is read whole again), read where the check's copy is under way from it
/// (same_snap_read), else as submitted; for ASTC the layers' tails it reaches (upload_astc_gpu's staging), the same way.
/// remake: the image made again by this upload (bc1_retype), its old bytes gone
static void seg_upload(Texture& t, const Ranges* part, SegCmds& cmds, uint64_t value, bool remake = false) {
    upload_same::changed(t.same);
    const uint64_t image0 = same_image_id(t);
    if (remake) t.image = (void*)(uintptr_t)(image_ids += 0x10);
    const Ranges all = all_of(t);
    const Ranges& want = part && !remake ? *part : all;
    Ranges runs;
    if (t.astc) {
        const uint64_t ls = astc_layer_size(t), tail0 = level_geo(t, 0).offset;
        for (uint32_t layer = 0; layer < t.layers; layer++) {
            const uint64_t at = (uint64_t)layer * ls + tail0;
            if (at >= t.guest_size || !upload_same::reaches(want, at, (uint64_t)(layer + 1) * ls)) continue;
            runs.push_back({at, std::min<uint64_t>((uint64_t)(layer + 1) * ls, t.guest_size)});
        }
    } else {
        if (t.same.units.empty()) same_units(t, t.same.units);
        upload_same::reached(t.same, want, reached_scratch);
        for (uint32_t i : reached_scratch) {
            const upload_same::Unit& u = t.same.units[i];
            if (!runs.empty() && u.lo <= runs.back().second) runs.back().second = std::max(runs.back().second, u.hi);
            else runs.push_back({u.lo, u.hi});
        }
    }
    for (const auto& r : runs) {
        const uint64_t n = r.second - r.first;
        const uint8_t* snap = same_snap_read(t, r.first, n);
        std::vector<uint8_t> b(n);
        for (uint64_t i = 0; i < n; i++) b[i] = snap ? snap[i] : submitted(t, r.first + i);
        cmds.writes.push_back({r.first, std::move(b)});
        cmds.bytes += n;
    }
    cmds.uploads++;
    if (twin_same_on()) upload_same::note_upload(t.same.seg, value, image0, same_image_id(t), part);
}
/// texture_touch's look by segment `seg` of submission `value`, for a draw that samples t: the twin (3) when a later segment
/// uploaded it in this submission; else nothing once looked at in this submission (0) or with nothing written (dirty empty);
/// else the check (same_look) and its upload (2, or 1 when nothing is to be uploaded), or an upload without it (0): whole (a
/// GPU buffer's write, owed whole: not partial) or `remake` (the upload makes the image again)
static int seg_look(Texture& t, Looks& lk, const Ranges& dirty, uint64_t seg, SegCmds& cmds, uint64_t value, bool whole = false,
                    bool remake = false) {
    widen_of = &t;
    if (lk.upload_value == value && lk.upload_seg > seg) {   // (texture_touch's twin, as it stands)
        static Ranges twin_part;
        const Ranges* noted = twin_same_on() ? upload_same::twin_ranges(t.same.seg, value, same_image_id(t)) : nullptr;
        if (noted) twin_part = *noted;
        const Ranges* part = noted ? &twin_part : nullptr;
        const bool looked = part && same_twin_look(t, *part);
        const uint64_t b0 = cmds.bytes;
        seg_upload(t, part, cmds, value);
        same_twin_done(t, part, looked, true);
        lk.upload_seg = seg;
        twin_count.twins++, twin_count.whole += !part, twin_count.bytes += cmds.bytes - b0;
        return 3;
    }
    if (lk.checked == value || (dirty.empty() && !whole)) return 0;
    lk.checked = value;
    Ranges d = dirty;
    const int v = whole || remake ? 0 : same_look(t, d);
    if (v == 1) return 1;
    seg_upload(t, whole ? nullptr : &d, cmds, value, remake);
    if (v == 2) same_done(t, true);
    lk.upload_value = value, lk.upload_seg = seg;
    return v;
}
/// What the draws of a segment see: the image as the queue has it there, checked against the bytes as submitted (image_part: an
/// ASTC texture's layers' tails)
static bool view_is_submitted(const Texture& t, const std::vector<uint8_t>& b) {
    for (const auto& r : image_part(t, all_of(t)))
        for (uint64_t i = r.first; i < r.second; i++)
            if (b[i] != submitted(t, i)) return false;
    return true;
}
/// The submit: segment 0's uploads, its draws (early_view: what they see), segment 1's uploads. An image made again in this
/// submission holds nothing before the first upload into it in the queue (0xCD)
static void submit(Texture& t, Image& img, SegCmds& s0, SegCmds& s1, uint64_t image_before, std::vector<uint8_t>* early_view) {
    std::vector<uint8_t> b = img.b;
    if (same_image_id(t) != image_before) std::fill(b.begin(), b.end(), 0xCD);
    for (const auto& w : s0.writes) std::memcpy(b.data() + w.first, w.second.data(), w.second.size());
    if (early_view) *early_view = b;
    for (const auto& w : s1.writes) std::memcpy(b.data() + w.first, w.second.data(), w.second.size());
    img.b.swap(b);
    img.uploads += s0.uploads + s1.uploads, img.bytes += s0.bytes + s1.bytes;
    s0 = SegCmds{}, s1 = SegCmds{};
}
/// The kept hashes are of what the image holds: while they go with its content (known == content), every unit with one hashes
/// so over the image's bytes. (The one thing the check relies on: a unit taken for the image's that is not would stay stale)
static bool hashes_hold(const Texture& t, const Image& img) {
    if (t.same.known != t.same.content) return true;
    for (const upload_same::Unit& u : t.same.units)
        if (u.hash && upload_same::hash_at(img.b.data() + u.lo, t.gpu + u.lo, u.hi - u.lo) != u.hash) return false;
    return true;
}
/// The same bytes written again whole (as the S2 Market's textures were, every 4 to 5 frames)
static void rewrite_same(const Texture& t) {
    const std::vector<uint8_t> was(guest.begin() + (t.gpu - kBase), guest.begin() + (t.gpu - kBase) + t.guest_size);
    write(t, 0, t.guest_size, 0x55);
    std::memcpy(guest.data() + (t.gpu - kBase), was.data(), was.size());
}

/// upload_same.h's twin rules: note_upload's ranges (merged across a submission's uploads, whole for a whole one, the image made
/// again by one or between two, a new submission starting over), twin_ranges (none for another submission or another image),
/// twin_known and twin_commit (kept only for one change since, from the copy alone, every unit's copy hash the kept one)
static void case_twin_rules() {
    upload_same::SegUploads n;
    auto note = [&n](uint64_t value, uint64_t image0, uint64_t image, Ranges part) {
        upload_same::note_upload(n, value, image0, image, &part);
    };
    CHECK(upload_same::twin_ranges(n, 5, 1) == nullptr);   // (none noted)
    note(5, 1, 1, Ranges{{100, 200}});
    note(5, 1, 1, Ranges{{300, 400}, {150, 250}});
    const Ranges* r = upload_same::twin_ranges(n, 5, 1);
    CHECK(r && r->size() == 2 && (*r)[0].first == 100 && (*r)[0].second == 250 && (*r)[1].first == 300);
    CHECK(upload_same::twin_ranges(n, 6, 1) == nullptr);   // (another submission's)
    CHECK(upload_same::twin_ranges(n, 5, 2) == nullptr);   // (the image made again since)
    upload_same::note_upload(n, 5, 1, 1, nullptr);          // (a whole one)
    CHECK(upload_same::twin_ranges(n, 5, 1) == nullptr);
    note(5, 1, 1, Ranges{{0, 10}});   // (whole stays whole in the submission)
    CHECK(upload_same::twin_ranges(n, 5, 1) == nullptr);
    note(6, 1, 1, Ranges{{0, 10}});   // (the next submission starts over)
    r = upload_same::twin_ranges(n, 6, 1);
    CHECK(r && r->size() == 1 && (*r)[0].second == 10);
    note(6, 1, 2, Ranges{{20, 30}});   // (the image made again by an upload: whole)
    CHECK(upload_same::twin_ranges(n, 6, 2) == nullptr);
    note(7, 2, 2, Ranges{{20, 30}});
    note(7, 3, 3, Ranges{{40, 50}});   // (made again between two uploads: whole)
    CHECK(upload_same::twin_ranges(n, 7, 3) == nullptr);
    note(8, 1, 3, Ranges{{40, 50}});   // (made again by the first one of a submission: whole)
    CHECK(upload_same::twin_ranges(n, 8, 3) == nullptr);
    note(9, 3, 3, Ranges{{5, 5}});      // (an empty range asks for nothing)
    CHECK(upload_same::twin_ranges(n, 9, 3) == nullptr);

    upload_same::State s;
    upload_same::add_units(s.units, 0, 300, 100, 300);
    std::vector<uint32_t> read{0, 1};
    CHECK(!upload_same::twin_known(s, read));   // (no hash kept)
    upload_same::changed(s);
    upload_same::commit(s, std::vector<uint32_t>{0, 1, 2}, std::vector<uint64_t>{11, 12, 13}, true);
    CHECK(upload_same::twin_known(s, read) && !upload_same::twin_known(s, std::vector<uint32_t>{}));
    // (the copy held the kept bytes: kept)
    upload_same::changed(s);
    CHECK(upload_same::twin_commit(s, read, std::vector<uint64_t>{11, 12}, true) && s.known == s.content);
    CHECK(s.units[0].hash == 11 && s.units[2].hash == 13 && upload_same::twin_known(s, read));
    // (one unit's copy other bytes: all forgotten)
    upload_same::changed(s);
    CHECK(!upload_same::twin_commit(s, read, std::vector<uint64_t>{11, 99}, true) && s.known != s.content);
    CHECK(!upload_same::twin_known(s, read));
    auto keep_again = [&] {   // (a check that forgets what is not the image's any more, then its upload, from the copy)
        std::vector<uint32_t> up;
        upload_same::check(s, upload_same::Ranges{{0, 300}}, [](const upload_same::Unit&) { return (uint64_t)0; }, up);
        upload_same::changed(s);
        upload_same::commit(s, std::vector<uint32_t>{0, 1, 2}, std::vector<uint64_t>{11, 12, 13}, true);
    };
    // (read past the copy, or failed: forgotten)
    keep_again();
    upload_same::changed(s);
    CHECK(!upload_same::twin_commit(s, read, std::vector<uint64_t>{11, 12}, false) && !upload_same::twin_known(s, read));
    // (another change to the image meanwhile (a GPU write): forgotten)
    keep_again();
    upload_same::changed(s);
    upload_same::changed(s);
    CHECK(!upload_same::twin_commit(s, read, std::vector<uint64_t>{11, 12}, true) && !upload_same::twin_known(s, read));
    // (the hashes not one for each unit read, or nothing read: forgotten)
    keep_again();
    upload_same::changed(s);
    CHECK(!upload_same::twin_commit(s, read, std::vector<uint64_t>{11}, true));
    keep_again();
    upload_same::changed(s);
    CHECK(!upload_same::twin_commit(s, std::vector<uint32_t>{}, std::vector<uint64_t>{}, true));
    // (a unit read whose hash is not known: no claim on it, forgotten)
    keep_again();
    s.units[1].hash = 0;
    CHECK(!upload_same::twin_known(s, read));
    upload_same::changed(s);
    CHECK(!upload_same::twin_commit(s, read, std::vector<uint64_t>{11, 0}, true));
    // (a hash changed since twin_known: as it is at the commit)
    keep_again();
    CHECK(upload_same::twin_known(s, read));
    upload_same::changed(s);
    s.units[0].hash = 77;
    CHECK(!upload_same::twin_commit(s, read, std::vector<uint64_t>{11, 12}, true));
}

/// The loop at the S2 Market after a tour, over a BC texture and an ASTC one: the game rewrites the storage with the bytes it has;
/// the later segment's worker looks first and makes the checked upload, the earlier one makes the twin. The twin uploads what that
/// upload asked for alone, from a copy that holds the kept hashes' bytes: kept, and the next rewrites are found the same (no upload,
/// no twin). The earlier segment's draws see the bytes as submitted throughout; the hashes are of what the image holds.
/// off (RX_VK_TWIN_SAME=0): the twin as before, the whole storage, the hashes forgotten, and the loop goes on every submission
static void twin_loop(Texture t, bool off) {
    reset();
    twin_count = TwinCount{};
    std::mt19937 rng(21);
    Image img;
    img.b.assign(t.guest_size, 0);
    fill_random(t, 0, t.guest_size, rng);
    CHECK(touch(t, all_of(t), img) == 2 && image_is_submitted(t, img, all_of(t)));   // (uploaded, its hashes kept)
    upload_same::changed(t.same);   // (forgotten: the loop's state, where the old twin left it)
    Looks lk;
    SegCmds s0, s1;
    std::vector<uint8_t> early;
    int checked_uploads = 0, found_same = 0;
    const uint64_t whole_bytes = t.astc ? 3ull * ((80 << 10) - 4096) : t.guest_size;   // (what an upload of all of it writes)
    for (uint64_t value = 10; value < 30; value++) {
        rewrite_same(t);
        const uint64_t image0 = same_image_id(t);
        const int v = seg_look(t, lk, all_of(t), 1, s1, value);   // (the later segment's worker first)
        checked_uploads += v == 2, found_same += v == 1;
        CHECK(v == 1 || v == 2);
        const int v0 = seg_look(t, lk, all_of(t), 0, s0, value);
        CHECK(v0 == (v == 2 ? 3 : 0));   // (the twin only after an upload in the later segment)
        submit(t, img, s0, s1, image0, &early);
        CHECK(view_is_submitted(t, early));
        CHECK(image_is_submitted(t, img, all_of(t)) && hashes_hold(t, img));
    }
    std::printf("twin loop%s%s: %d checked uploads, %d found the same, %d twins (%d whole, %llu KB)\n", t.astc ? " (ASTC)" : "",
                off ? " (RX_VK_TWIN_SAME=0)" : "", checked_uploads, found_same, twin_count.twins, twin_count.whole,
                (unsigned long long)(twin_count.bytes >> 10));
    if (off) {   // (as before: every rewrite uploaded again and twinned whole)
        CHECK(checked_uploads == 20 && found_same == 0 && twin_count.twins == 20 && twin_count.whole == 20);
        CHECK(twin_count.bytes == 20 * whole_bytes);
    } else {     // (one checked upload and one twin, of the units it read: the loop ends)
        CHECK(checked_uploads == 1 && found_same == 19 && twin_count.twins == 1 && twin_count.whole == 0);
        CHECK(twin_count.bytes == whole_bytes);   // (all of it was asked for: nothing was known)
    }
}

/// The twin's cases one by one (BC texture): a row of other bytes (the twin uploads that row alone, its hash kept); the game writing
/// the row again between the later segment's copy and the twin (the twin's copy holds other bytes: forgotten, and the earlier draws
/// see what the twin read); the image made again by the later segment's upload (the twin uploads the whole storage: the earlier
/// draws see no leftover of the new image); a whole upload in the later segment (twinned whole); a partial one not checked (the
/// twin of its ranges without a copy); the earlier segment's worker first (no twin); the hashes kept by a checked upload in an
/// earlier submission, then in this one an upload not checked or a GPU write by the later segment (the twin makes no copy:
/// twin_known, as the image is now, finds them not the image's)
static void case_twin() {
    twin_loop(bc_texture(), false);
    twin_loop(astc_texture(), false);
    reset();
    std::mt19937 rng(22);
    Texture t = bc_texture();
    Image img;
    img.b.assign(t.guest_size, 0);
    fill_random(t, 0, t.guest_size, rng);
    CHECK(touch(t, all_of(t), img) == 2);
    Looks lk;
    SegCmds s0, s1;
    std::vector<uint8_t> early;
    uint64_t value = 100;
    // (a row of other bytes: the later segment uploads that row; the twin that row alone, from its copy, its hash kept)
    twin_count = TwinCount{};
    write(t, 90000, 16, 0x77);
    Ranges d;
    dirty_pages(t, 90000, 16, d);
    uint64_t image0 = same_image_id(t);
    CHECK(seg_look(t, lk, d, 1, s1, value) == 2 && s1.bytes == 32768);
    CHECK(seg_look(t, lk, d, 0, s0, value) == 3 && s0.bytes == 32768 && twin_count.whole == 0);
    CHECK(t.same.known == t.same.content);   // (kept)
    submit(t, img, s0, s1, image0, &early);
    CHECK(view_is_submitted(t, early) && image_is_submitted(t, img, all_of(t)) && hashes_hold(t, img));
    rewrite_same(t);
    value++;
    CHECK(seg_look(t, lk, all_of(t), 1, s1, value) == 1 && seg_look(t, lk, all_of(t), 0, s0, value) == 0);
    submit(t, img, s0, s1, image0, nullptr);
    // (the row written again between the later segment's copy and the twin, in memory no capture kept: the twin's copy holds other
    // bytes than the hashes': forgotten. The earlier draws see what the twin read, the image ends with the later segment's)
    write(t, 40000, 16, 0x31);
    d.clear();
    dirty_pages(t, 40000, 16, d);
    value++;
    CHECK(seg_look(t, lk, d, 1, s1, value) == 2);
    const std::vector<uint8_t> later_row(s1.writes[0].second);
    write(t, 40000, 16, 0x32);   // (meanwhile)
    CHECK(seg_look(t, lk, d, 0, s0, value) == 3 && t.same.known != t.same.content);
    submit(t, img, s0, s1, image0, &early);
    CHECK(view_is_submitted(t, early));
    CHECK(std::equal(later_row.begin(), later_row.end(), img.b.begin() + 32768) && hashes_hold(t, img));
    value++;   // (the next look finds it written: uploaded, not taken for the image's)
    CHECK(seg_look(t, lk, d, 1, s1, value) == 2 && seg_look(t, lk, d, 0, s0, value) == 3);
    submit(t, img, s0, s1, image0, &early);
    CHECK(view_is_submitted(t, early) && image_is_submitted(t, img, all_of(t)) && hashes_hold(t, img));
    // (the image made again by the later segment's partial upload (a BC1 image found with alpha, made BC3 and read whole): the
    // earlier segment's draws would see nothing of the new image outside what was asked for: the twin uploads the whole storage)
    write(t, 150000, 16, 0x41);
    d.clear();
    dirty_pages(t, 150000, 16, d);
    value++;
    twin_count = TwinCount{};
    image0 = same_image_id(t);
    CHECK(seg_look(t, lk, d, 1, s1, value, false, true) == 0);
    CHECK(seg_look(t, lk, d, 0, s0, value) == 3 && twin_count.whole == 1 && s0.bytes == t.guest_size);
    submit(t, img, s0, s1, image0, &early);
    CHECK(view_is_submitted(t, early) && image_is_submitted(t, img, all_of(t)) && hashes_hold(t, img));
    // (and made again after the later segment's upload, before the twin (as the compaction moves an image): the whole storage)
    write(t, 150000, 16, 0x42);
    value++;
    twin_count = TwinCount{};
    image0 = same_image_id(t);
    CHECK(seg_look(t, lk, d, 1, s1, value) == 2);
    t.image = (void*)(uintptr_t)(image_ids += 0x10);
    upload_same::changed(t.same);
    CHECK(seg_look(t, lk, d, 0, s0, value) == 3 && twin_count.whole == 1);
    submit(t, img, s0, s1, image0, &early);
    CHECK(view_is_submitted(t, early) && image_is_submitted(t, img, all_of(t)) && hashes_hold(t, img));
    // (a whole upload in the later segment (not partial: a GPU buffer wrote its storage): twinned whole)
    value++;
    twin_count = TwinCount{};
    fill_random(t, 0, 1000, rng);
    image0 = same_image_id(t);
    CHECK(seg_look(t, lk, Ranges{}, 1, s1, value, true) == 0);
    CHECK(seg_look(t, lk, Ranges{}, 0, s0, value) == 3 && twin_count.whole == 1 && s0.bytes == t.guest_size);
    submit(t, img, s0, s1, image0, &early);
    CHECK(view_is_submitted(t, early) && image_is_submitted(t, img, all_of(t)) && hashes_hold(t, img));
    // (a partial upload not checked (resting after checks that found nothing the same): the twin of its ranges, no copy)
    t.same.rest = 2;
    write(t, 210000, 16, 0x51);
    d.clear();
    dirty_pages(t, 210000, 16, d);
    value++;
    twin_count = TwinCount{};
    CHECK(seg_look(t, lk, d, 1, s1, value) == 0 && s1.bytes == 32768);
    CHECK(seg_look(t, lk, d, 0, s0, value) == 3 && twin_count.whole == 0 && s0.bytes == 32768);
    submit(t, img, s0, s1, image0, &early);
    CHECK(view_is_submitted(t, early) && image_is_submitted(t, img, all_of(t)) && hashes_hold(t, img));
    // (the earlier segment's worker first: its own upload, no twin)
    t.same.rest = 0;
    write(t, 10, 16, 0x61);
    d.clear();
    dirty_pages(t, 10, 16, d);
    value++;
    twin_count = TwinCount{};
    CHECK(seg_look(t, lk, d, 0, s0, value) == 2 && seg_look(t, lk, d, 1, s1, value) == 0 && twin_count.twins == 0);
    submit(t, img, s0, s1, image0, &early);
    CHECK(view_is_submitted(t, early) && image_is_submitted(t, img, all_of(t)) && hashes_hold(t, img));
    // (hashes kept by a checked upload in an earlier submission; in this one the later segment's upload is not checked (how 0),
    // or is checked and the GPU then writes the image (how 1, texture_written): the image is not what they are of, and the twin
    // uploads its ranges without a copy (st_twin_plain), neither keeping nor forgetting from one (st_twin_kept, st_twin_forgot))
    for (int how = 0; how < 2; how++) {
        t.same.rest = 0, t.same.misses = 0;   // (checked: not resting after the checks above found nothing the same)
        write(t, 60000, 16, (uint8_t)(0x71 + how));
        d.clear();
        dirty_pages(t, 60000, 16, d);
        value++;
        image0 = same_image_id(t);
        CHECK(seg_look(t, lk, d, 0, s0, value) == 2 && t.same.known == t.same.content);   // (the earlier submission: kept)
        submit(t, img, s0, s1, image0, &early);
        CHECK(image_is_submitted(t, img, all_of(t)) && hashes_hold(t, img));
        write(t, 60000, 16, (uint8_t)(0x81 + how));
        value++;
        image0 = same_image_id(t);
        t.same.misses = 0;
        if (how == 0) {
            t.same.rest = 2;   // (resting: uploaded unchecked)
            CHECK(seg_look(t, lk, d, 1, s1, value) == 0);
        } else {
            CHECK(seg_look(t, lk, d, 1, s1, value) == 2 && t.same.known == t.same.content);
            upload_same::changed(t.same);   // (the later segment's draws write the image: texture_written)
        }
        CHECK(t.same.known != t.same.content && std::any_of(t.same.units.begin(), t.same.units.end(),   // (hashes there, for
                                                               [](const upload_same::Unit& u) { return u.hash != 0; }));   // another content)
        const int64_t plain0 = st_twin_plain.load(), kept0 = st_twin_kept.load(), forgot0 = st_twin_forgot.load();
        CHECK(seg_look(t, lk, d, 0, s0, value) == 3 && tl_same_tex == nullptr);
        CHECK(st_twin_plain.load() == plain0 + 1 && st_twin_kept.load() == kept0 && st_twin_forgot.load() == forgot0);
        submit(t, img, s0, s1, image0, &early);
        CHECK(view_is_submitted(t, early) && image_is_submitted(t, img, all_of(t)) && hashes_hold(t, img));
    }
}

/// RX_VK_TWIN_SAME=0: the twin as before, the whole storage, the hashes forgotten (nothing noted)
static void case_twin_off() {
    twin_loop(bc_texture(), true);
    twin_loop(astc_texture(), true);
    Texture t = bc_texture();
    upload_same::SegUploads none;
    SegCmds c;
    const Ranges part{{0, 100}};
    seg_upload(t, &part, c, 5);
    CHECK(t.same.seg.value == none.value && t.same.seg.ranges.empty());
    CHECK(!same_twin_look(t, part));
}

/// Two segments of many submissions over a BC and an ASTC texture, the writes of every kind (other bytes, the same bytes, back to
/// an older version, a capture keeping pages, a write between the later segment's look and the twin, a GPU write, a whole upload,
/// an image made again), either segment's worker first: the earlier segment's draws see the bytes as submitted when the twin
/// read them, the image ends as submitted (but where the game wrote meanwhile), and the kept hashes are always the image's
static void case_twin_random() {
    std::mt19937 rng(23);
    uint64_t on_bytes = 0;
    int twins = 0, same = 0;
    for (int round = 0; round < 24; round++) {
        reset();
        twin_count = TwinCount{};
        const uint64_t at = kBase + 0x10000 + 0x200 * (uint64_t)(rng() % 64);
        Texture t = round % 2 ? astc_texture(at) : bc_texture(at);
        Image img;
        img.b.assign(t.guest_size, 0);
        fill_random(t, 0, t.guest_size, rng);
        CHECK(touch(t, all_of(t), img) == 2);
        Looks lk;
        SegCmds s0, s1;
        std::vector<uint8_t> early;
        std::vector<std::vector<uint8_t>> versions;
        for (uint64_t value = 10; value < 130; value++) {
            Ranges d;
            const int writes = (int)(rng() % 3);
            for (int k = 0; k < writes; k++) {
                const uint64_t off = rng() % t.guest_size, n = 1 + rng() % 30000, m = std::min(n, t.guest_size - off);
                const uint32_t op = rng() % 6;
                if (op <= 1) fill_random(t, off, m, rng);
                else if (op == 2 && !versions.empty()) {
                    const auto& v = versions[rng() % versions.size()];
                    std::memcpy(guest.data() + (t.gpu - kBase), v.data(), v.size());
                    dirty_pages(t, 0, t.guest_size, d);
                }
                dirty_pages(t, off, m, d);   // (op 3 to 5: the same bytes again)
            }
            if (rng() % 4 == 0)   // (a capture keeps the pages written as they are now)
                for (const auto& r : d)
                    for (uint64_t a = (t.gpu + r.first) & ~0xFFFull; a < t.gpu + r.second; a += 0x1000) mem::keep_now(a);
            if (rng() % 8 == 0) versions.emplace_back(guest.begin() + (t.gpu - kBase), guest.begin() + (t.gpu - kBase) + t.guest_size);
            const uint32_t kind = rng() % 16;
            const bool whole = kind == 0, remake = kind == 1 && !t.astc, later_first = rng() % 4 != 0;
            if (kind == 2) {   // (the GPU drew into it: the image's bytes are not the hashes'; a whole upload follows)
                for (uint64_t i = 0; i < 512; i++) img.b[(rng() + i) % t.guest_size] = (uint8_t)rng();
                upload_same::changed(t.same);
            }
            const uint64_t image0 = same_image_id(t);
            const bool any = whole || kind == 2 || !d.empty();
            bool meanwhile = false;   // (d grows by what the game writes meanwhile, for the next submission's look)
            if (later_first) {
                const int v = seg_look(t, lk, d, 1, s1, value, whole || kind == 2, remake);
                same += v == 1;
                if (rng() % 6 == 0 && !s1.writes.empty()) {   // (the game writes into what the later segment uploaded, before the twin)
                    const auto& w = s1.writes[rng() % s1.writes.size()];
                    fill_random(t, w.first, std::min<uint64_t>(64, w.second.size()), rng);
                    dirty_pages(t, w.first, std::min<uint64_t>(64, w.second.size()), d);   // (for the next look)
                    meanwhile = true;
                }
                seg_look(t, lk, d, 0, s0, value);
            } else {
                seg_look(t, lk, d, 0, s0, value, whole || kind == 2, remake);
                seg_look(t, lk, d, 1, s1, value);
            }
            (void)any;
            submit(t, img, s0, s1, image0, &early);
            CHECK(view_is_submitted(t, early));
            CHECK(hashes_hold(t, img));
            if (!meanwhile) CHECK(image_is_submitted(t, img, all_of(t)));
            else {   // (the image holds the later segment's bytes where the game wrote meanwhile: the next look takes them)
                value++;
                Ranges again;
                for (const auto& r : d) again.push_back(r);
                seg_look(t, lk, again, 1, s1, value);
                seg_look(t, lk, again, 0, s0, value);
                const uint64_t image1 = same_image_id(t);
                submit(t, img, s0, s1, image1, &early);
                CHECK(view_is_submitted(t, early) && hashes_hold(t, img) && image_is_submitted(t, img, all_of(t)));
            }
        }
        twins += twin_count.twins, on_bytes += twin_count.bytes;
    }
    std::printf("twin random: %d twins, %llu KB in all, %d looks found the same\n", twins, (unsigned long long)(on_bytes >> 10), same);
    CHECK(twins > 0 && same > 0);
}

int main(int argc, char** argv) {
    const std::string c = argc > 1 ? argv[1] : "";
    if (c == "hash") case_hash();
    else if (c == "units") case_units();
    else if (c == "rules") case_check_rules();
    else if (c == "misses") case_misses();
    else if (c == "copy") case_copy();
    else if (c == "visit") case_visit();
    else if (c == "kept") case_kept();
    else if (c == "skip") case_skip();
    else if (c == "aba") case_aba();
    else if (c == "forget") case_forget();
    else if (c == "submitted") case_submitted();
    else if (c == "unwanted") case_unwanted();
    else if (c == "rest") case_rest();
    else if (c == "off") case_off();
    else if (c == "astc") case_astc();
    else if (c == "astc_off") case_astc_off();
    else if (c == "random") case_random();
    else if (c == "twin_rules") case_twin_rules();
    else if (c == "twin") case_twin();
    else if (c == "twin_off") case_twin_off();
    else if (c == "twin_random") case_twin_random();
    else {
        std::printf("unknown case %s\n", c.c_str());
        return 2;
    }
    if (failures) return 1;
    std::printf("upload_same %s: passed\n", c.c_str());
    return 0;
}
