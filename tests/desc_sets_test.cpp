// vk_draw.cpp's set path (bind_descriptors for a program over the push limit) with its kept sets and update templates, against a
// fake driver that writes every set's descriptors into a store of its own, by the write structs or by a template's entries alike.
// tests/test_desc_sets.py cuts the production code into the placeholders below and runs each case in a process of its own (the
// switches are read once).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace fixture {
std::string test;
const char* getenv(const char* name) {
    if (!std::strcmp(name, "RX_VK_DESC_SET_KEEP") && test == "keep-off") return "0";
    if (!std::strcmp(name, "RX_VK_DESC_TEMPLATE") && (test == "template-off" || test == "push-template-off")) return "0";
    return nullptr;
}
void logf(const char*, ...) {}
#define TLS_OBJECT(name, ...) inline __VA_ARGS__& name() { static thread_local __VA_ARGS__ v; return v; }
template <class T> T handle(uintptr_t v) { return reinterpret_cast<T>(v); }

struct { VkDevice device = handle<VkDevice>(1); } dev;
struct Program {
    uint64_t key = 0x1234;
    bool push = false;
    VkPipelineLayout layout = handle<VkPipelineLayout>(0x100);
    VkDescriptorSetLayout set_layout = handle<VkDescriptorSetLayout>(0x200);
    uint32_t renodx = 0, frag_crc = 0;
};
std::atomic<bool> renodx_tracing{false};
void renodx_bind(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t) {}
void renodx_trace_draw(uint32_t) {}
struct DescWriter { std::vector<VkWriteDescriptorSet> writes; } writer;
DescWriter& dw() { return writer; }
bool g_fine = false;
enum { FC_PUSH_FULL, FC_PUSH_WRITES, FC_PUSH_DESCS, FC_PUSH_DELTA, FC_PUSH_NONE, FC_POOL_SETS, FC_POOL_DESCS, FC_N };
int64_t fine_n[FC_N]{};
#define FINE_SCOPE(x) (void)0
#define FINE_N(x, n) (void)0

// ---- the world the kept sets stand on ----
uint64_t destroy_gen = 1, bind_stamp = 7;
thread_local uint64_t desc_rec = 1;
uint64_t texture_destroy_generation() { return destroy_gen; }
uint64_t gpu_bind_stamp() { return bind_stamp; }
struct RecordingRes { VkDescriptorPool pool{}; std::vector<VkDescriptorPool> extra; } recording;
RecordingRes* cur = &recording;
VkDescriptorPool make_pool() { return handle<VkDescriptorPool>(0x77); }
void vkDestroyDescriptorPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*) {}

// ---- the fake driver: each set's descriptors, (binding, element) -> (type, the handles and numbers that make it) ----
using Desc = std::tuple<int, uint64_t, uint64_t, uint64_t>;
std::map<VkDescriptorSet, std::map<std::pair<uint32_t, uint32_t>, Desc>> store;
std::map<VkDescriptorSet, VkDescriptorSetLayout> set_layout_of;
uint64_t next_set = 0x1000;
int allocs = 0, updates = 0, tpl_updates = 0, binds = 0, pushes = 0, tpl_made = 0, tpl_tries = 0;
bool tpl_fail = false;
VkDescriptorSet last_bound{};
VkResult vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo* ai, VkDescriptorSet* out) {
    allocs++;
    *out = handle<VkDescriptorSet>(next_set++);
    set_layout_of[*out] = ai->pSetLayouts[0];
    return VK_SUCCESS;
}
Desc desc_of(VkDescriptorType t, const void* info) {
    switch (t) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
        const auto* b = static_cast<const VkDescriptorBufferInfo*>(info);
        return {(int)t, (uint64_t)(uintptr_t)b->buffer, b->offset, b->range};
    }
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
        const auto* i = static_cast<const VkDescriptorImageInfo*>(info);
        return {(int)t, (uint64_t)(uintptr_t)i->sampler, (uint64_t)(uintptr_t)i->imageView, (uint64_t)i->imageLayout};
    }
    default: return {(int)t, (uint64_t)(uintptr_t) * static_cast<const VkBufferView*>(info), 0, 0};
    }
}
const void* info_of(const VkWriteDescriptorSet& w, uint32_t k) {
    if (w.pBufferInfo && (w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER))
        return &w.pBufferInfo[k];
    if (w.pImageInfo && (w.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE))
        return &w.pImageInfo[k];
    return &w.pTexelBufferView[k];
}
void vkUpdateDescriptorSets(VkDevice, uint32_t n, const VkWriteDescriptorSet* w, uint32_t copies, const VkCopyDescriptorSet*) {
    CHECK(copies == 0);
    updates++;
    for (uint32_t i = 0; i < n; i++) {
        CHECK(store.count(w[i].dstSet) || set_layout_of.count(w[i].dstSet));
        for (uint32_t k = 0; k < w[i].descriptorCount; k++)
            store[w[i].dstSet][{w[i].dstBinding, w[i].dstArrayElement + k}] = desc_of(w[i].descriptorType, info_of(w[i], k));
    }
}
struct Tpl {
    std::vector<VkDescriptorUpdateTemplateEntry> e;
    VkDescriptorSetLayout layout;
    VkDescriptorUpdateTemplateType type;   // ((Triforce) a set's, or a push template's with its bind point, pipeline layout and set)
    VkPipelineBindPoint bp;
    VkPipelineLayout pipe_layout;
    uint32_t set;
};
std::map<VkDescriptorUpdateTemplate, Tpl> tpls_made;
VkResult fake_create_template(VkDevice, const VkDescriptorUpdateTemplateCreateInfo* ci, const VkAllocationCallbacks*,
                              VkDescriptorUpdateTemplate* out) {
    CHECK((ci->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET ||
           ci->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR) && ci->descriptorUpdateEntryCount > 0);
    tpl_tries++;
    if (tpl_fail) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = handle<VkDescriptorUpdateTemplate>(0x5000 + ++tpl_made);
    tpls_made[*out] = {std::vector<VkDescriptorUpdateTemplateEntry>(ci->pDescriptorUpdateEntries,
                                                                     ci->pDescriptorUpdateEntries + ci->descriptorUpdateEntryCount),
                       ci->descriptorSetLayout, ci->templateType, ci->pipelineBindPoint, ci->pipelineLayout, ci->set};
    return VK_SUCCESS;
}
void fake_update_template(VkDevice, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void* data) {
    tpl_updates++;
    const Tpl& tp = tpls_made.at(t);
    CHECK(tp.type == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET);
    CHECK(set_layout_of.at(set) == tp.layout);   // (a set of the layout the template was made for)
    for (const VkDescriptorUpdateTemplateEntry& e : tp.e)   // (as the spec has the driver read it: offset + k * stride)
        for (uint32_t k = 0; k < e.descriptorCount; k++)
            store[set][{e.dstBinding, e.dstArrayElement + k}] = desc_of(e.descriptorType, (const uint8_t*)data + e.offset + k * e.stride);
}
PFN_vkCreateDescriptorUpdateTemplate vkCreateDescriptorUpdateTemplate = fake_create_template;
PFN_vkUpdateDescriptorSetWithTemplate vkUpdateDescriptorSetWithTemplate = fake_update_template;
// ((Triforce) what each bind point holds pushed, by the write structs or a push template alike; and the bytes the recorder was told)
std::map<std::pair<VkCommandBuffer, int>, std::map<std::pair<uint32_t, uint32_t>, Desc>> push_store;   // ((command buffer, bind point): its state)
std::map<std::pair<uint32_t, uint32_t>, Desc>& held(VkCommandBuffer c, int compute) { return push_store[{c, compute}]; }
int tpl_pushes = 0;
namespace rec {
size_t told = 0;
void push_template_bytes(size_t n) { told = n; }
} // namespace rec
void fake_push_template(VkCommandBuffer cb, VkDescriptorUpdateTemplate t, VkPipelineLayout layout, uint32_t set, const void* data) {
    tpl_pushes++;
    const Tpl& tp = tpls_made.at(t);
    CHECK(tp.type == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR && tp.pipe_layout == layout && tp.set == 0 && set == 0);
    size_t reads = 0;   // (the recorder's copy must hold all the driver reads)
    for (const VkDescriptorUpdateTemplateEntry& e : tp.e) {
        reads = std::max(reads, e.offset + (e.descriptorCount - 1) * e.stride + (e.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ? sizeof(VkBufferView) : 24));
        for (uint32_t k = 0; k < e.descriptorCount; k++)
            push_store[{cb, tp.bp == VK_PIPELINE_BIND_POINT_COMPUTE}][{e.dstBinding, e.dstArrayElement + k}] =
                desc_of(e.descriptorType, (const uint8_t*)data + e.offset + k * e.stride);
    }
    CHECK(rec::told >= reads);
    rec::told = 0;
}
PFN_vkCmdPushDescriptorSetWithTemplateKHR vkCmdPushDescriptorSetWithTemplateKHR = fake_push_template;
void vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t first, uint32_t n, const VkDescriptorSet* s,
                             uint32_t, const uint32_t*) {
    CHECK(first == 0 && n == 1 && set_layout_of.count(s[0]));
    binds++, last_bound = s[0];
}
void vkCmdPushDescriptorSetKHR(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout, uint32_t, uint32_t n, const VkWriteDescriptorSet* w) {
    pushes++;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t k = 0; k < w[i].descriptorCount; k++)
            push_store[{cb, bp == VK_PIPELINE_BIND_POINT_COMPUTE}][{w[i].dstBinding, w[i].dstArrayElement + k}] =
                desc_of(w[i].descriptorType, info_of(w[i], k));
}

/* COUNTS */
/* WORDS */
std::array<Pushed, 2>& pushed() { static std::array<Pushed, 2> p; return p; }
/* SETS */
/* BIND */

// ---- the descriptors a draw built: three buffers, two textures, a storage image, two texel views ----
VkDescriptorBufferInfo bufs[3] = {{handle<VkBuffer>(0x11), 0, 256}, {handle<VkBuffer>(0x12), 512, 64}, {handle<VkBuffer>(0x13), 0, 16}};
VkDescriptorImageInfo imgs[3] = {{handle<VkSampler>(0x21), handle<VkImageView>(0x31), VK_IMAGE_LAYOUT_GENERAL},
                                 {handle<VkSampler>(0x22), handle<VkImageView>(0x32), VK_IMAGE_LAYOUT_GENERAL},
                                 {VK_NULL_HANDLE, handle<VkImageView>(0x33), VK_IMAGE_LAYOUT_GENERAL}};
VkBufferView views[2] = {handle<VkBufferView>(0x41), handle<VkBufferView>(0x42)};
void build(uint32_t tex_count = 2) {
    dw().writes.clear();
    auto w = [](VkDescriptorType t, uint32_t count) {
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstBinding = (uint32_t)dw().writes.size(), x.descriptorType = t, x.descriptorCount = count;
        return x;
    };
    VkWriteDescriptorSet a = w(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2); a.pBufferInfo = &bufs[0]; dw().writes.push_back(a);
    VkWriteDescriptorSet b = w(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1); b.pBufferInfo = &bufs[2]; dw().writes.push_back(b);
    VkWriteDescriptorSet c = w(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2); c.pTexelBufferView = views; dw().writes.push_back(c);
    VkWriteDescriptorSet d = w(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, tex_count); d.pImageInfo = &imgs[0]; dw().writes.push_back(d);
    VkWriteDescriptorSet e = w(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1); e.pImageInfo = &imgs[2]; dw().writes.push_back(e);
}
/// What the writes say each descriptor is (the reference the store is compared with)
std::map<std::pair<uint32_t, uint32_t>, Desc> expected() {
    std::map<std::pair<uint32_t, uint32_t>, Desc> m;
    for (const VkWriteDescriptorSet& w : dw().writes)
        for (uint32_t k = 0; k < w.descriptorCount; k++) m[{w.dstBinding, k}] = desc_of(w.descriptorType, info_of(w, k));
    return m;
}
const VkCommandBuffer cmd = handle<VkCommandBuffer>(0x900), cmd2 = handle<VkCommandBuffer>(0x901);
const VkPipelineBindPoint G = VK_PIPELINE_BIND_POINT_GRAPHICS, C = VK_PIPELINE_BIND_POINT_COMPUTE;
Program prog;
VkDescriptorSet bind(VkPipelineBindPoint bp = G, VkCommandBuffer c = cmd, const Program& p = prog) {
    CHECK(bind_descriptors(c, bp, p));
    CHECK(store.at(last_bound) == expected());   // (whatever path: the set bound holds exactly the descriptors built)
    return last_bound;
}

int run() {
    recording.pool = handle<VkDescriptorPool>(0x66);
    build();
    if (test == "reuse") {   // (the same descriptors again: the set written before, bound again)
        const VkDescriptorSet a = bind(), b = bind();
        CHECK(a == b && allocs == 1 && updates + tpl_updates == 1 && binds == 2 && st_set_again.load() == 1 && st_set_new.load() == 1);
        CHECK(st_set_new_n.load() == 8);
        return 0;
    }
    if (test == "changed") {   // (one descriptor otherwise: a new set, and the next one alike is that one)
        const VkDescriptorSet a = bind();
        bufs[1].offset = 576;
        const VkDescriptorSet b = bind(), c = bind();
        CHECK(a != b && b == c && allocs == 2 && st_set_again.load() == 1);
        imgs[1].imageView = handle<VkImageView>(0x39);
        CHECK(bind() != b && allocs == 3);
        views[1] = handle<VkBufferView>(0x49);
        CHECK(allocs == 3 && bind() && allocs == 4);
        return 0;
    }
    if (test == "destroy") {   // (a texture destroyed or made again since the set was written: not kept, issue 1126)
        const VkDescriptorSet a = bind();
        destroy_gen++;
        CHECK(bind() != a && allocs == 2);
        return 0;
    }
    if (test == "stamp") {   // (a pool moved, a mirror made, the next submission)
        const VkDescriptorSet a = bind();
        bind_stamp++;
        CHECK(bind() != a && allocs == 2);
        return 0;
    }
    if (test == "recording") {   // (another recording: its pool was reset, the set is gone)
        const VkDescriptorSet a = bind();
        desc_rec++;
        CHECK(bind() != a && allocs == 2);
        return 0;
    }
    if (test == "cmd") {   // (another command buffer)
        const VkDescriptorSet a = bind();
        CHECK(bind(G, cmd2) != a && allocs == 2);
        return 0;
    }
    if (test == "bindpoint") {   // (each bind point keeps its own; a set written at one is not taken at the other)
        const VkDescriptorSet a = bind(G), b = bind(C);
        CHECK(a != b && allocs == 2 && bind(G) == a && bind(C) == b && allocs == 2);
        return 0;
    }
    if (test == "layout") {   // (another layout, the same words: a set of its own; the first layout's still kept)
        Program other;
        other.key = 0x5678, other.layout = handle<VkPipelineLayout>(0x110), other.set_layout = handle<VkDescriptorSetLayout>(0x210);
        const VkDescriptorSet a = bind(G, cmd, prog), b = bind(G, cmd, other);
        CHECK(a != b && set_layout_of.at(b) == other.set_layout);
        CHECK(bind(G, cmd, prog) == a && bind(G, cmd, other) == b && allocs == 2);
        return 0;
    }
    if (test == "keep-off") {   // (RX_VK_DESC_SET_KEEP=0: a new set each time, as before)
        const VkDescriptorSet a = bind(), b = bind();
        CHECK(a != b && allocs == 2 && st_set_again.load() == 0);
        return 0;
    }
    if (test == "template") {   // (written through the program's template, the same descriptors as the write structs give)
        const VkDescriptorSet a = bind();
        CHECK(tpl_updates == 1 && updates == 0 && tpl_made == 1 && st_set_tpl.load() == 1);
        bufs[0].range = 128;
        const VkDescriptorSet b = bind();
        CHECK(a != b && tpl_updates == 2 && tpl_made == 1);   // (one template for the program)
        const Tpl& t = tpls_made.begin()->second;
        CHECK(t.layout == prog.set_layout && t.e.size() == 5);
        return 0;
    }
    if (test == "template-off") {   // (RX_VK_DESC_TEMPLATE=0)
        bind();
        CHECK(updates == 1 && tpl_updates == 0 && tpl_made == 0);
        return 0;
    }
    if (test == "template-misfit") {   // (writes that are not those it was made from: the write structs, as before)
        bind();
        CHECK(tpl_updates == 1);
        build(1);
        bind();
        CHECK(updates == 1 && tpl_updates == 1 && tpl_made == 1);
        build(2);
        dw().writes[3].pImageInfo = nullptr;   // (a write without the infos its type is read from: not packed)
        CHECK(!template_fits(set_template(prog)->sig, dw().writes.data(), dw().writes.size()));
        return 0;
    }
    if (test == "template-fail") {   // (no template could be made: the write structs, and it is not tried at every set)
        tpl_fail = true;
        bind();
        bufs[0].offset = 256;
        bind();
        CHECK(updates == 2 && tpl_updates == 0 && tpl_tries == 1);
        return 0;
    }
    if (test == "template-entries") {   // (writes not one a binding from its first element: no template)
        std::vector<VkDescriptorUpdateTemplateEntry> e;
        std::vector<uint64_t> sig;
        uint32_t slots = 0;
        CHECK(template_entries(dw().writes.data(), dw().writes.size(), e, &sig, &slots) && slots == 8 && e.size() == 5);
        CHECK(e[0].offset == 0 && e[1].offset == 2 * kTplSlot && e[2].offset == 3 * kTplSlot && e[3].offset == 5 * kTplSlot &&
              e[4].offset == 7 * kTplSlot);
        for (const auto& x : e) CHECK(x.stride == kTplSlot && x.dstArrayElement == 0);
        dw().writes[2].dstBinding = 5;
        CHECK(!template_entries(dw().writes.data(), dw().writes.size(), e, &sig, &slots));
        build();
        dw().writes[1].descriptorCount = 0;
        CHECK(!template_entries(dw().writes.data(), dw().writes.size(), e, &sig, &slots));
        build();
        dw().writes[1].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        CHECK(!template_entries(dw().writes.data(), dw().writes.size(), e, &sig, &slots));
        CHECK(!template_entries(dw().writes.data(), 0, e, &sig, &slots));
        return 0;
    }
    if (test == "push") {   // (a program that fits the push limit: pushed, no set, counted as before; (Triforce) whole through its push template)
        prog.push = true;
        CHECK(bind_descriptors(cmd, G, prog) && tpl_pushes == 1 && pushes == 0 && allocs == 0 && st_push_full.load() == 1 &&
              st_push_full_n.load() == 8 && st_push_tpl.load() == 1);
        CHECK(held(cmd, 0) == expected());
        CHECK(bind_descriptors(cmd, G, prog) && tpl_pushes == 1 && pushes == 0 && st_push_none.load() == 1);
        bufs[2].range = 32;
        CHECK(bind_descriptors(cmd, G, prog) && pushes == 1 && tpl_pushes == 1 && st_push_delta.load() == 1 && st_push_delta_n.load() == 1);
        CHECK(held(cmd, 0) == expected());   // (the changed binding by its write struct over what the template pushed)
        return 0;
    }
    if (test == "push-template") {   // ((Triforce) one push template a program and bind point, for its layout's set 0)
        prog.push = true;
        CHECK(bind_descriptors(cmd, G, prog) && tpl_pushes == 1 && tpl_made == 1);
        const Tpl& g = tpls_made.begin()->second;
        CHECK(g.type == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR && g.bp == G && g.pipe_layout == prog.layout && g.set == 0 &&
              g.e.size() == 5);
        CHECK(bind_descriptors(cmd, C, prog) && tpl_pushes == 2 && tpl_made == 2 && held(cmd, 1) == expected());   // (the other bind point's)
        CHECK(tpls_made.rbegin()->second.bp == C);
        bufs[0].offset = 64;
        CHECK(bind_descriptors(cmd2, G, prog) && tpl_pushes == 3 && tpl_made == 2 && held(cmd2, 0) == expected());   // (another command buffer: whole again, the same template)
        Program other;   // (another program: a template of its own)
        other.key = 0x5678, other.push = true, other.layout = handle<VkPipelineLayout>(0x110), other.set_layout = handle<VkDescriptorSetLayout>(0x210);
        CHECK(bind_descriptors(cmd2, G, other) && tpl_pushes == 4 && tpl_made == 3 && tpls_made.rbegin()->second.pipe_layout == other.layout);
        prog.layout = handle<VkPipelineLayout>(0x120);   // (the program's layout made again: a new template for it)
        CHECK(bind_descriptors(cmd, G, prog) && tpl_made == 4 && tpls_made.rbegin()->second.pipe_layout == prog.layout);
        CHECK(pushes == 0 && st_push_tpl.load() == 5);
        return 0;
    }
    if (test == "push-template-off") {   // (RX_VK_DESC_TEMPLATE=0: the write structs, as before)
        prog.push = true;
        CHECK(bind_descriptors(cmd, G, prog) && pushes == 1 && tpl_pushes == 0 && tpl_made == 0 && held(cmd, 0) == expected());
        return 0;
    }
    if (test == "push-template-none") {   // (no vkCmdPushDescriptorSetWithTemplateKHR: the write structs, no template made)
        prog.push = true;
        vkCmdPushDescriptorSetWithTemplateKHR = nullptr;
        CHECK(bind_descriptors(cmd, G, prog) && pushes == 1 && tpl_made == 0 && held(cmd, 0) == expected());
        return 0;
    }
    if (test == "push-template-misfit") {   // (writes that are not those it was made from, or none could be made: the write structs)
        prog.push = true;
        CHECK(bind_descriptors(cmd, G, prog) && tpl_pushes == 1);
        build(1);
        CHECK(bind_descriptors(cmd2, G, prog) && pushes == 1 && tpl_pushes == 1 && tpl_made == 1 && held(cmd2, 0) == expected());
        tpl_fail = true;
        Program other;
        other.key = 0x5678, other.push = true, other.layout = handle<VkPipelineLayout>(0x110), other.set_layout = handle<VkDescriptorSetLayout>(0x210);
        CHECK(bind_descriptors(cmd, G, other) && pushes == 2 && tpl_pushes == 1);
        bufs[0].offset = 128;
        CHECK(bind_descriptors(cmd2, G, other) && pushes == 3 && tpl_tries == 2);   // (not tried again at every push)
        return 0;
    }
    std::fprintf(stderr, "no case %s\n", test.c_str());
    return 2;
}
} // namespace fixture
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    fixture::test = argv[1];
    return fixture::run();
}
