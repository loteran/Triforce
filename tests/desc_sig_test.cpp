// The whole-draw skip by the signature of a draw's descriptor inputs (gpu/vk/desc_sig.h): vk_draw.cpp's sig block (desc_sig_mode,
// sig_shape, desc_sig_walk, sig_sampled, sig_check), the per-binding reuse it stands on (the block before build_descriptors) and the
// handle a texture is read with (handle_of, handle_is_storage_pointer, sampled_handle), cut into the placeholders below by
// tests/test_desc_sig.py, against stand-ins for what they read. A model of build_descriptors resolves a program's descriptors through
// the cut helpers in its order (kept_uniform, the storage buffers' take and buf_keep, kept_texture with sampled_handle), counting the
// lookups; a draw is the walk, then a build (always: what it gives is what a skip would have kept), then the bind point's signature
// as draw() commits it. Each case runs in a process of its own (the switches are read once).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "desc_keep.h"
#include "desc_sig.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s (step %d)\n", __LINE__, #x, step); std::exit(1); } } while (0)

namespace fixture {
std::string test;
int step = -1;
const char* getenv(const char* name) {
    if (!std::strcmp(name, "RX_VK_DESC_SIG") && test == "sig-off") return "0";
    if (!std::strcmp(name, "RX_VK_DESC_SIG") && test.rfind("check", 0) == 0) return "check";
    if (!std::strcmp(name, "RX_VK_DESC_SIG")) return "1";   // (off by default since 11-16: the cases ask for it, as =1 does)
    if (!std::strcmp(name, "RX_VK_DESC_KEEP") && test == "keep-off") return "0";
    if (!std::strcmp(name, "RX_VK_DESC_KEEP") && test == "keep-check") return "check";
    if (!std::strcmp(name, "RX_VK_PUSH_STATE_OLD") && test == "check-old") return "1";
    return nullptr;
}
int logged = 0;
void logf(const char*, ...) { logged++; }
#define TLS_OBJECT(name, ...) inline __VA_ARGS__& name() { static thread_local __VA_ARGS__ v; return v; }
template <class T> T handle(uint64_t v) { return reinterpret_cast<T>((uintptr_t)v); }

namespace Shader {
enum class TextureType { Color2D };
struct ConstantBufferDescriptor { uint32_t index, count; };
struct StorageBufferDescriptor { uint32_t cbuf_index, cbuf_offset, count; bool is_written; };
struct TextureDescriptor {
    TextureType type = TextureType::Color2D;
    bool is_depth = false, has_secondary = false;
    uint32_t cbuf_index = 2, cbuf_offset = 0, shift_left = 0, secondary_cbuf_index = 0, secondary_cbuf_offset = 0,
             secondary_shift_left = 0, count = 1, size_shift = 3, handle_program = ~0U;
};
struct TextureBufferDescriptor { uint32_t count = 1; };
struct ImageDescriptor { uint32_t count = 1; };
struct Info {
    std::vector<ConstantBufferDescriptor> constant_buffer_descriptors;
    std::vector<StorageBufferDescriptor> storage_buffers_descriptors;
    std::vector<TextureBufferDescriptor> texture_buffer_descriptors, image_buffer_descriptors;
    std::vector<TextureDescriptor> texture_descriptors;
    std::vector<ImageDescriptor> image_descriptors;
    bool native_global_bounds = false;
};
} // namespace Shader

constexpr int kNvnStages = 8;
struct nvn_p_bind_buf { uint64_t addr = 0, size = 0; };
struct NvnState {
    nvn_p_bind_buf ubo[kNvnStages][16];
    uint64_t bind_gen = 100, tex_gen[kNvnStages]{}, ssbo_gen[kNvnStages]{};
};
struct Stage {
    int nvn_stage = 0;
    VkShaderStageFlagBits vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
    Shader::Info info;
};
struct Program {
    uint64_t key = 0xabcdef;
    bool compute = false, fallback = false, push = true;
    uint32_t renodx = 0;
    VkPipelineLayout layout = handle<VkPipelineLayout>(0x1a);
    std::vector<Stage> stages;
};
struct Texture { uint32_t slot; bool gpu_written; };
enum class ViewUse { Sample, Storage };

// ---- the world the lookups and the constant banks answer from ----
struct World {
    uint64_t stamp = 5, ub = 9, samp = 3, destroy = 11, dummy = 13, epoch = 17;
    std::map<uint32_t, uint32_t> version;                          // (a slot's)
    std::map<std::tuple<int, uint32_t, uint32_t>, uint32_t> cbuf;   // (stage, bank, offset) -> word
    bool sampler_fail = false, overlay = false, unbound = false, move_destroy = false;
    int views = 0, samplers = 0, uniforms = 0, binds = 0;
    uint64_t at = 0x100;
    uint64_t bias = 0;   // (what the next lookups answer otherwise with the same inputs)
    struct Bank { bool made = false; uint64_t gen = 0, at = 0; } banks[3][kNvnStages];
} w;
Texture textures[16];
thread_local bool view_slot_known = true;
thread_local bool upload_defer_ok = false;
thread_local bool draw_may_defer = true;   // ((Triforce, redrawn.h) set by vk_draw.cpp draw: may this draw leave textures)
bool texture_owes(const Texture*) { return false; }   // ((Triforce) no texture here still owes bytes)
uint32_t texture_slot_version(int slot) { return w.version[(uint32_t)slot]; }
uint64_t sampler_registrations_generation() { return w.samp; }
uint64_t texture_destroy_generation() { return w.destroy; }
uint64_t texture_dummy_generation() { return w.dummy; }
uint32_t texture_epoch() { return (uint32_t)w.epoch; }
uint64_t gpu_bind_stamp() { return w.stamp; }
uint64_t ub_generation() { return w.ub; }
uint32_t stage_cbuf_word(const NvnState&, const Stage& st, uint32_t index, uint32_t offset) {
    const auto it = w.cbuf.find({st.nvn_stage, index, offset});
    return it == w.cbuf.end() ? 0 : it->second;
}
uint32_t stage_handle(const NvnState&, const Stage&, uint32_t, uint32_t) { return 0; }
VkImageView texture_view_of(uint32_t h, VkImageViewType vt, ViewUse use, VkFormat, Texture** tex) {
    CHECK(use == ViewUse::Sample);
    w.views++;
    const uint32_t slot = h & 0xFFFFF;
    const uint64_t v = (uint64_t)slot << 32 ^ (uint64_t)w.version[slot] << 20 ^ w.destroy << 8 ^ (uint64_t)vt ^ w.bias;
    if (w.move_destroy) w.destroy++;
    *tex = &textures[slot & 15];
    return handle<VkImageView>(v | 1);
}
VkSampler sampler_for(int id, bool compare, bool) {   // ((Triforce, S8) the scene flag: a draw's own, none here)
    w.samplers++;
    if (w.sampler_fail) return VK_NULL_HANDLE;
    return handle<VkSampler>(0x7000000ull | (uint64_t)id << 8 | (uint64_t)compare << 4 | w.samp << 32 | w.bias);
}
struct RecordingRes {
    struct Stream { VkBuffer buffer; };
    VkBuffer stream = handle<VkBuffer>(0x51);
    std::vector<Stream> streams{{handle<VkBuffer>(0x51)}};
} recording;
thread_local RecordingRes* cur = &recording;
/// (uniform_range: c1, the banks as its cache keeps them, by the stage's generation, and the user buffers)
VkDescriptorBufferInfo uniform_range(const NvnState& s, const Stage& st, uint32_t index) {
    if (index != 0 && index != 2) w.uniforms++;   // (the banks are resolved at every build: not counted, their results compared)
    if (index == 1) return {cur->stream, 0x40 + (uint64_t)st.nvn_stage * 0x100, 64};
    if (index == 0 || index == 2) {
        const uint64_t gen = index == 2 ? s.tex_gen[st.nvn_stage] : s.ssbo_gen[st.nvn_stage];
        World::Bank& b = w.banks[index][st.nvn_stage];
        if (!b.made || b.gen != gen) b.made = true, b.gen = gen, b.at = w.at += 0x100;
        return {cur->stream, b.at, 0x200};
    }
    const nvn_p_bind_buf& u = s.ubo[st.nvn_stage][index - 3];
    if (!u.addr) return {VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
    if (w.overlay) return {cur->stream, w.at += 0x100, u.size};   // (a copy with an update in)
    return {handle<VkBuffer>(0x9000 + (u.addr >> 16)), (u.addr & 0xffff) + w.bias, u.size};
}
/// (gpu_buffer_bind for a read-only storage buffer: by its address)
VkDescriptorBufferInfo storage_bind(uint64_t addr, uint64_t size) {
    w.binds++;
    if (w.unbound || !addr) return {handle<VkBuffer>(0x2e0), 0, 16};   // (the zero buffer: not bound)
    return {handle<VkBuffer>(0xA000 + (addr >> 20)), (addr & 0xfffff) + w.bias, std::max<uint64_t>(size, 16)};
}

struct DescWriter {
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufs;
    std::vector<VkDescriptorImageInfo> imgs;
};
TLS_OBJECT(dw, DescWriter)
TLS_OBJECT(draw_sampled, std::vector<const Texture*>)

/* HANDLES */

/* KEEP */

/* WORDS */
TLS_OBJECT(pushed, std::array<Pushed, 2>)

/* SIG */

// ---- a model of build_descriptors for the programs the signature stands for (each kind through the cut helpers, in its order) ----
struct Built {
    std::vector<uint64_t> words;   // (write_words' of every write)
    int lookups = 0;               // (views, uniform ranges and storage binds: what a kept descriptor does not ask)
    bool armed = false;
    std::vector<const void*> tex;
    std::vector<const Texture*> sampled;
};
Built build(const NvnState& s, const Program& p) {
    DescWriter& d = dw();
    d.writes.clear(), d.bufs.clear(), d.imgs.clear();
    draw_sampled().clear();
    size_t n = 0;
    for (const Stage& st : p.stages) {
        for (const auto& c : st.info.constant_buffer_descriptors) n += c.count;
        for (const auto& c : st.info.storage_buffers_descriptors) n += c.count;
        for (const auto& c : st.info.texture_descriptors) n += c.count;
    }
    d.bufs.reserve(n), d.imgs.reserve(n);
    desc_keep::Prog* const kp = desc_keep_mode() ? &desc_keep::prog_of(kept_descs(), &p, n) : nullptr;
    const int l0 = w.views + w.uniforms + w.binds;
    const int64_t drops = desc_drops;
    Built b;
    size_t jn = 0;
    uint32_t binding = 0;
    for (const Stage& st : p.stages) {
        for (const auto& c : st.info.constant_buffer_descriptors) {
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            x.dstBinding = binding++, x.descriptorCount = c.count, x.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            const size_t first = d.bufs.size(), jb = jn;
            jn += c.count;
            for (uint32_t k = 0; k < c.count; k++) d.bufs.push_back(kept_uniform(kp, jb + k, s, st, c.index + k));
            x.pBufferInfo = &d.bufs[first];
            d.writes.push_back(x);
        }
        for (const auto& c : st.info.storage_buffers_descriptors) {
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            x.dstBinding = binding++, x.descriptorCount = c.count, x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            const size_t first = d.bufs.size(), jb = jn;
            jn += c.count;
            const uint64_t addr = stage_cbuf_word(s, st, c.cbuf_index, c.cbuf_offset) |
                                  (uint64_t)stage_cbuf_word(s, st, c.cbuf_index, c.cbuf_offset + 4) << 32;
            const uint64_t size = stage_cbuf_word(s, st, c.cbuf_index, c.cbuf_offset + 8);
            desc_keep::Elem* const ke = kp ? &kp->e[jb] : nullptr;
            const desc_keep::Stamps kn = ke ? buf_stamps(desc_keep::kSsbo) : desc_keep::Stamps{};
            const bool took = ke && desc_keep::take(*ke, desc_keep::kSsbo, addr, size, 0, kn);
            if (took && desc_keep_mode() != 2) {
                count_add(st_dk_taken, 1);
                d.bufs.push_back({handle<VkBuffer>(ke->out[0]), ke->out[1], ke->out[2]});
            } else {
                const VkDescriptorBufferInfo bi = storage_bind(addr, size);
                const bool bound = bi.buffer != handle<VkBuffer>(0x2e0);
                d.bufs.push_back(bi);
                if (ke) buf_keep(*kp, jb, *ke, desc_keep::kSsbo, addr, size, kn, took, bound, d.bufs.back());
            }
            x.pBufferInfo = &d.bufs[first];
            d.writes.push_back(x);
        }
        for (const auto& c : st.info.texture_descriptors) {
            VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            x.dstBinding = binding++, x.descriptorCount = c.count, x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            const size_t first = d.imgs.size(), jb = jn;
            jn += c.count;
            for (uint32_t k = 0; k < c.count; k++) {
                const uint32_t h = sampled_handle(s, st, c, k);
                Texture* tex = nullptr;
                VkImageView view = VK_NULL_HANDLE;
                const VkSampler smp = kept_texture(kp, jb + k, h, VK_IMAGE_VIEW_TYPE_2D, c.is_depth, false, false, &view, &tex);
                if (view && tex) b.tex.push_back(tex);
                if (view && tex && tex->gpu_written) draw_sampled().push_back(tex);
                if (!view) view = handle<VkImageView>(0xD0);   // (the dummy)
                d.imgs.push_back({smp, view, VK_IMAGE_LAYOUT_GENERAL});
            }
            x.pImageInfo = &d.imgs[first];
            d.writes.push_back(x);
        }
    }
    for (const VkWriteDescriptorSet& x : d.writes) words_append(x, b.words);
    b.lookups = w.views + w.uniforms + w.binds - l0;
    b.armed = desc_drops == drops;
    for (const VkDescriptorBufferInfo& x : d.bufs) b.armed &= x.buffer != VK_NULL_HANDLE;
    b.sampled = draw_sampled();
    return b;
}

// ---- the program: a vertex and a fragment stage, every kind the signature stands for ----
Program prog;
NvnState s{};
constexpr uint32_t kVsTex = 0x168, kFsTex = 0x170, kSsboAt = 0x10;
void make_program() {
    Stage vs, fs;
    vs.nvn_stage = 0, vs.vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
    fs.nvn_stage = 1, fs.vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    vs.info.constant_buffer_descriptors = {{0, 2}, {3, 1}};   // (c0 the bank, c1 the constants; c3)
    vs.info.storage_buffers_descriptors = {{3, kSsboAt, 1, false}};
    Shader::TextureDescriptor t;
    t.cbuf_index = 2, t.cbuf_offset = kVsTex;
    vs.info.texture_descriptors = {t};
    fs.info.constant_buffer_descriptors = {{2, 1}, {4, 1}};   // (c2 the bank; c4)
    fs.info.storage_buffers_descriptors = {{5, 0, 1, false}};
    Shader::TextureDescriptor a, ptr;
    a.cbuf_index = 2, a.cbuf_offset = kFsTex, a.count = 2;
    ptr.cbuf_index = 5, ptr.cbuf_offset = 0;   // (where the storage buffer keeps its address: no handle, the dummy)
    fs.info.texture_descriptors = {a, ptr};
    prog.stages = {vs, fs};
    s.ubo[0][0] = {0x12340000, 0x100};
    s.ubo[1][1] = {0x56780000, 0x40};
    s.ubo[1][2] = {0x9abc0000, 0x80};   // (c5 of the fragment stage: its storage buffer's address words)
    w.cbuf[{0, 2, kVsTex}] = 0x123 | 7u << 20;
    w.cbuf[{1, 2, kFsTex}] = 0x124 | 7u << 20, w.cbuf[{1, 2, kFsTex + 8}] = 0x125 | 7u << 20;
    w.cbuf[{0, 3, kSsboAt}] = 0x00500000, w.cbuf[{0, 3, kSsboAt + 4}] = 0x10, w.cbuf[{0, 3, kSsboAt + 8}] = 0x400;
    w.cbuf[{1, 5, 0}] = 0x00700000, w.cbuf[{1, 5, 4}] = 0x10, w.cbuf[{1, 5, 8}] = 0x200;
    for (uint32_t slot : {0x123u, 0x124u, 0x125u, 0x126u}) w.version[slot] = 1;
    pushed()[0].cmd = handle<VkCommandBuffer>(0xc0), pushed()[0].layout = prog.layout;
}

// ---- a draw as draw() makes it: the walk, a hit or not, the build, the bind point's signature ----
desc_sig::Sig bound_sig, now_sig;
Built bound_built;
struct Draw { bool walked, hit; Built b; };
Draw draw_once() {
    Draw r{};
    r.walked = desc_sig_walk(s, prog, now_sig);
    r.hit = r.walked && desc_sig::hit(bound_sig, now_sig, &prog);
    r.b = build(s, prog);
    if (r.hit) {   // (what a skip keeps is what this build gives: every descriptor taken, the same writes, the same textures)
        CHECK(r.b.lookups == 0 && r.b.armed && r.b.words == bound_built.words);
        std::vector<const Texture*> again;
        sig_sampled(bound_sig, again);   // (the textures commit kept with the bind point's signature)
        CHECK(again == r.b.sampled);
    }
    now_sig.tex = r.b.tex;
    if (r.walked) desc_sig::commit(bound_sig, now_sig, r.b.armed);
    else desc_sig::forget(bound_sig);
    bound_built = r.b;
    return r;
}
bool hits() { return draw_once().hit; }
void settle() { draw_once(), draw_once(); }

int header() {   // (desc_sig.h alone)
    using namespace desc_sig;
    Sig b, n;
    int a = 0, c = 0;
    n.prog = &a, n.w = {1, 2, 3};
    CHECK(!hit(b, n, &a));   // (nothing bound)
    commit(b, n, true);
    CHECK(b.prog == &a && b.armed && b.w == (std::vector<uint64_t>{1, 2, 3}) && n.prog == nullptr && n.w.empty());
    n.prog = &a, n.w = {1, 2, 3};
    CHECK(hit(b, n, &a) && !hit(b, n, &c));
    n.w = {1, 2, 4};
    CHECK(!hit(b, n, &a));
    n.w = {1, 2, 3}, n.prog = &c;
    CHECK(!hit(b, n, &a));
    n.prog = &a;
    commit(b, n, false);   // (a build that dropped one)
    n.prog = &a, n.w = {1, 2, 3};
    CHECK(!hit(b, n, &a));
    commit(b, n, true);
    forget(b);
    n.prog = &a, n.w = {1, 2, 3};
    CHECK(!hit(b, n, &a) && b.w.empty());
    n.w.clear();
    b.prog = &a, b.armed = true;
    CHECK(!hit(b, n, &a));   // (an empty signature: never)
    // (the head: each kind's stamps as desc_keep.h's stamps_of has them, none of another's)
    desc_keep::Stamps st;
    st.rec = 1, st.bind = 2, st.ub = 3, st.samp = 4, st.destroy = 5, st.dummy = 6, st.epoch = 7;
    for (desc_keep::Kind k : {desc_keep::kTex, desc_keep::kUbo, desc_keep::kC1, desc_keep::kSsbo}) {
        std::vector<uint64_t> h;
        put_head(h, &a, bit(k), st);
        const auto want = desc_keep::stamps_of(k, st);
        CHECK(h.size() == 3 + 4 && h[1] == 1 && std::equal(want.begin(), want.end(), h.begin() + 3));
        for (int i = 0; i < 7; i++) {   // (a stamp moves the head exactly when the kind stands on it)
            desc_keep::Stamps x = st;
            uint64_t* f[7] = {&x.rec, &x.bind, &x.ub, &x.samp, &x.destroy, &x.dummy, &x.epoch};
            (*f[i])++;
            std::vector<uint64_t> g;
            put_head(g, &a, bit(k), x);
            const auto on = desc_keep::stamps_of(k, x);
            CHECK((g != h) == (i == 0 || on != want));
        }
    }
    std::vector<uint64_t> bank;
    put_head(bank, &a, kBank, st);
    CHECK(bank.size() == 3 && bank[1] == 1);   // (a bank: the recording only; its stage generation is the walk's)
    return 0;
}

/// The coverage: each input and stamp the program's descriptors stand on stops a hit; what it does not read does not
int coverage() {
    make_program();
    settle();
    CHECK(hits() && hits());
    auto moved = [&](const char* what, auto change, bool stops) {
        change();
        const bool h = hits();
        if (h == stops) std::fprintf(stderr, "%s: %s\n", what, stops ? "a hit, though it moved" : "no hit, though not read");
        CHECK(h != stops);
        if (stops) CHECK(hits());   // (built again, the next one keeps it)
    };
    moved("c3 address", [] { s.ubo[0][0].addr += 0x100; }, true);
    moved("c3 size", [] { s.ubo[0][0].size = 0x200; }, true);
    moved("c4 address", [] { s.ubo[1][1].addr += 0x100; }, true);
    moved("c4 of the vertex stage, not read", [] { s.ubo[0][1].addr += 0x100; s.bind_gen++; }, false);
    moved("another stage's uniform buffer", [] { s.ubo[3][0].addr += 0x100; s.bind_gen++; }, false);
    moved("the bindings' generation alone", [] { s.bind_gen++; }, false);
    moved("a storage buffer's address", [] { w.cbuf[{0, 3, kSsboAt}] += 0x1000; }, true);
    moved("a storage buffer's high address", [] { w.cbuf[{0, 3, kSsboAt + 4}]++; }, true);
    moved("a storage buffer's size", [] { w.cbuf[{0, 3, kSsboAt + 8}] = 0x800; }, true);
    moved("the fragment storage buffer's address", [] { w.cbuf[{1, 5, 0}] += 0x1000; }, true);
    moved("a texture's handle", [] { w.cbuf[{0, 2, kVsTex}] = 0x126 | 7u << 20; s.tex_gen[0]++; }, true);
    moved("a texture's sampler", [] { w.cbuf[{0, 2, kVsTex}] ^= 1u << 21; s.tex_gen[0]++; }, true);
    moved("an array texture's second handle", [] { w.cbuf[{1, 2, kFsTex + 8}] = 0x123 | 7u << 20; s.tex_gen[1]++; }, true);
    moved("a texture's slot registered anew", [] { w.version[0x126]++; }, true);
    moved("the array's slot registered anew", [] { w.version[0x124]++; }, true);
    moved("a slot the program does not sample", [] { w.version[0x200]++; }, false);
    moved("a sampler registration", [] { w.samp++; }, true);
    moved("a texture destroyed (issue 1126)", [] { w.destroy++; }, true);
    moved("a texture left for later", [] { w.dummy++; }, true);
    moved("the next submission's epoch", [] { w.epoch++; }, true);
    moved("a pool moved, a mirror made", [] { w.stamp++; }, true);
    moved("an inline uniform update", [] { w.ub++; }, true);
    moved("a new recording", [] { desc_rec++; }, true);
    moved("the vertex stage's storage buffers (its c0)", [] { s.ssbo_gen[0]++; }, true);
    moved("the fragment stage's textures (its c2)", [] { s.tex_gen[1]++; }, true);
    moved("the fragment stage's storage buffers (no c0 read)", [] { s.ssbo_gen[1]++; }, false);
    moved("another stage's textures", [] { s.tex_gen[3]++; }, false);
    // (the handle read where a storage buffer keeps its address: none in the fragment stage, whatever the word)
    const uint32_t word = w.cbuf[{1, 5, 0}];
    CHECK(sampled_handle(s, prog.stages[1], prog.stages[1].info.texture_descriptors[1], 0) == 0 && word);
    // (a texture the GPU wrote since: not an input, but a hit reads it again, as a build would: draw_once checks)
    textures[0x123 & 15].gpu_written = true;
    CHECK(hits());
    textures[0x124 & 15].gpu_written = true;
    CHECK(hits());
    return 0;
}

/// A build that kept not every descriptor stands for nothing: the next draw builds, and one after it that kept them all is kept
int armed() {
    make_program();
    settle();
    CHECK(hits());
    w.overlay = true, w.ub++;   // (an update into the range: a copy in the stream, never kept)
    Draw d = draw_once();
    CHECK(!d.hit && !d.b.armed);
    d = draw_once();
    CHECK(!d.hit && !d.b.armed);   // (the same signature, but its build dropped one)
    w.overlay = false, w.ub++;
    CHECK(!hits() && hits());
    w.unbound = true, w.stamp++;   // (a storage buffer not bound: the zero buffer, never kept)
    CHECK(!hits() && !hits());
    w.unbound = false, w.stamp++;
    CHECK(!hits() && hits());
    s.ubo[1][1] = {};   // (no buffer at c4: nothing kept)
    CHECK(!hits() && !hits());
    s.ubo[1][1] = {0x56780000, 0x40};
    CHECK(!hits() && hits());
    w.move_destroy = true, w.version[0x123]++;   // (a texture destroyed while its lookup ran: the next walk differs)
    d = draw_once();
    w.move_destroy = false;
    CHECK(!d.hit && !hits() && hits());
    return 0;
}

/// Random moves of everything the program reads and does not: every hit is a build that would have taken every descriptor and
/// given the writes and the sampled textures the bind point holds (draw_once's checks), and the hits are not rare
int random_moves() {
    make_program();
    std::mt19937_64 rng(0x5eed);
    int hit_n = 0, unread_n = 0, unread_hits = 0;
    bool unread_last = false;
    for (step = 0; step < 20000; step++) {
        const Draw d = draw_once();
        hit_n += d.hit;
        if (unread_last) unread_n++, unread_hits += d.hit;
        const int m = (int)(rng() % 24);
        unread_last = m >= 18;
        switch (m) {
        case 0: s.ubo[0][0].addr = 0x12340000 + (rng() % 3) * 0x100; break;
        case 1: s.ubo[1][1].size = 0x40 + (rng() % 2) * 0x40; break;
        case 2: w.cbuf[{0, 3, kSsboAt}] = 0x00500000 + (uint32_t)(rng() % 3) * 0x1000; break;
        case 3: w.cbuf[{1, 2, kFsTex + 8 * (rng() % 2)}] = (0x123 + (uint32_t)(rng() % 4)) | 7u << 20, s.tex_gen[1]++; break;
        case 4: w.version[0x123 + (uint32_t)(rng() % 4)]++; break;
        case 5: w.samp++; break;
        case 6: w.destroy++; break;
        case 7: w.dummy++; break;
        case 8: w.epoch++; break;
        case 9: w.stamp++; break;
        case 10: w.ub++; break;
        case 11: desc_rec++; break;
        case 12: s.ssbo_gen[0]++; break;
        case 13: w.overlay = !w.overlay; break;
        case 14: w.unbound = (rng() % 4) == 0; break;
        case 15: textures[rng() % 16].gpu_written ^= true; break;
        case 16: w.cbuf[{1, 5, 0}] = 0x00700000 + (uint32_t)(rng() % 2) * 0x1000; break;
        case 17: w.cbuf[{0, 2, kVsTex}] = (0x123 + (uint32_t)(rng() % 2)) | 7u << 20, s.tex_gen[0]++; break;
        // (what the program does not read)
        case 18: s.ubo[3][rng() % 16].addr += 0x100, s.bind_gen++; break;
        case 19: w.version[0x300 + (uint32_t)(rng() % 8)]++; break;
        case 20: s.bind_gen++; break;
        case 21: s.tex_gen[3]++, s.ssbo_gen[1]++; break;
        case 22: s.ubo[0][5].size += 0x10, s.bind_gen++; break;
        default: break;
        }
    }
    std::printf("random: %d hits of 20000 draws; after a move of what the program does not read, %d of %d\n", hit_n, unread_hits,
                unread_n);
    CHECK(hit_n > 2000 && unread_hits > unread_n / 3);
    return 0;
}

/// The programs the signature does not stand for: no walk
int shapes() {
    make_program();
    auto refused = [&](const char* what, auto change) {
        Program saved = prog;
        change(prog);
        prog.key++;
        desc_sig::Sig out;
        out.w = {1};
        Program* q = new Program(prog);   // (another address: the shape is made once a program)
        const bool walked = desc_sig_walk(s, *q, out);
        if (walked) std::fprintf(stderr, "walked: %s\n", what);
        CHECK(!walked && out.w.empty() && out.prog == nullptr);
        prog = saved;
    };
    desc_sig::Sig out;
    CHECK(desc_sig_walk(s, prog, out) && out.prog == &prog && !out.w.empty());
    refused("a written storage buffer", [](Program& p) { p.stages[0].info.storage_buffers_descriptors[0].is_written = true; });
    refused("a storage buffer table", [](Program& p) { p.stages[0].info.storage_buffers_descriptors[0].count = 3; });
    refused("the miss binding", [](Program& p) { p.stages[0].info.storage_buffers_descriptors.push_back({0, 0xff0, 1, false}); });
    refused("pointers picked at run time", [](Program& p) { p.stages[1].info.native_global_bounds = true; });
    refused("a texel buffer", [](Program& p) { p.stages[1].info.texture_buffer_descriptors.resize(1); });
    refused("a storage texel buffer", [](Program& p) { p.stages[1].info.image_buffer_descriptors.resize(1); });
    refused("a storage image", [](Program& p) { p.stages[1].info.image_descriptors.resize(1); });
    refused("a uniform buffer past c18", [](Program& p) { p.stages[1].info.constant_buffer_descriptors.push_back({19, 1}); });
    refused("a compute program", [](Program& p) { p.compute = true; });
    refused("the mesh fallback", [](Program& p) { p.fallback = true; });
    refused("RenoDX's", [](Program& p) { p.renodx = 1; });
    refused("a compute stage", [](Program& p) { p.stages[1].nvn_stage = 5; });
    refused("a stage out of range", [](Program& p) { p.stages[1].nvn_stage = kNvnStages; });
    return 0;
}

/// RX_VK_DESC_SIG=check: a draw built that the signature would have kept, compared with what is pushed
int check_case() {
    CHECK(desc_sig_mode() == 2);
    make_program();
    const VkCommandBuffer cmd = pushed()[0].cmd;
    const Built first = build(s, prog);
    Pushed& ps = pushed()[0];
    ps.n = dw().writes.size();
    ps.words.clear(), ps.writes.clear();
    for (const VkWriteDescriptorSet& x : dw().writes) {
        words_append(x, ps.words);
        ps.writes.emplace_back();
        write_words(x, ps.writes.back());
    }
    desc_sig::Sig& b = sig_bound();
    b.tex = first.tex;
    const int l0 = logged;
    sig_check(cmd, prog);
    CHECK(st_sig_checked.load() == 1 && st_sig_bad.load() == 0 && logged == l0);
    w.bias = 0x10;   // (the same inputs, other descriptors)
    desc_rec++;
    build(s, prog);
    sig_check(cmd, prog);
    CHECK(st_sig_checked.load() == 2 && st_sig_bad.load() == 1 && logged == l0 + 1);
    w.bias = 0, desc_rec++;
    build(s, prog);
    sig_check(cmd, prog);
    CHECK(st_sig_bad.load() == 1);
    textures[0x124 & 15].gpu_written = true;   // (a texture the GPU wrote since: read again from the bound textures, the same)
    build(s, prog);
    sig_check(cmd, prog);
    CHECK(st_sig_bad.load() == 1);
    b.tex.clear();   // (bound textures that are not the build's: a sampled texture the GPU wrote would be missed)
    sig_check(cmd, prog);
    CHECK(st_sig_bad.load() == 2);
    b.tex = first.tex;
    sig_check(handle<VkCommandBuffer>(0xc1), prog);   // (another command buffer's push)
    CHECK(st_sig_bad.load() == 3);
    return 0;
}

int run() {
    if (test == "header") return header();
    if (test == "on") {
        CHECK(desc_sig_mode() == 1 && desc_keep_mode() == 1);
        return 0;
    }
    if (test == "sig-off" || test == "keep-off" || test == "keep-check") {
        CHECK(desc_sig_mode() == 0);
        make_program();
        desc_sig::Sig out;
        CHECK(desc_sig_walk(s, prog, out));   // (the walk itself is draw()'s to leave out: its condition reads the mode)
        return 0;
    }
    if (test == "coverage") return coverage();
    if (test == "armed") return armed();
    if (test == "random") return random_moves();
    if (test == "shapes") return shapes();
    if (test == "check" || test == "check-old") return check_case();
    std::fprintf(stderr, "no case %s\n", test.c_str());
    return 2;
}
} // namespace fixture
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    fixture::test = argv[1];
    return fixture::run();
}
