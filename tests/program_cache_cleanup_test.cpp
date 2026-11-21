// Instrument Vulkan, not the loader: the runner compiles the actual Stage/Program
// declarations, serializer, Reader, make_module and program_cache_load functions.
// Info is a minimal stand-in with the cache's separately serialized containers;
// this test covers module ownership, not Shader::Info layout or SPIR-V validity.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using VkShaderModule = uintptr_t;
using VkDescriptorSetLayout = uintptr_t;
using VkPipelineLayout = uintptr_t;
using VkPipeline = uintptr_t;
using VkDescriptorType = uint32_t;
using VkShaderStageFlagBits = uint32_t;
using VkShaderStageFlags = uint32_t;
using VkPrimitiveTopology = uint32_t;
constexpr uintptr_t VK_NULL_HANDLE = 0;
constexpr uint32_t VK_SHADER_STAGE_VERTEX_BIT = 1, VK_SHADER_STAGE_FRAGMENT_BIT = 16;
constexpr uint32_t VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3;
constexpr size_t kIdxEntry = 20;
constexpr int VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 15;
struct VkShaderModuleCreateInfo {
    int sType;
    size_t codeSize = 0;
    const uint32_t* pCode = nullptr;
};
namespace Shader {
namespace IR { enum class Attribute : uint64_t {}; }
struct Descriptor {   // (a descriptor's array size, which get_info checks)
    uint32_t count;
    Descriptor(uint32_t c = 1) : count(c) {}
};
struct Info {
    uint32_t marker = 0;
    std::vector<Descriptor> constant_buffer_descriptors, storage_buffers_descriptors,
        texture_buffer_descriptors, image_buffer_descriptors, texture_descriptors, image_descriptors;
    std::map<IR::Attribute, IR::Attribute> legacy_stores_mapping;
    std::vector<std::vector<uint32_t>> handle_programs;
};
}
#include "spv_store.h"   // (Triforce: the store a packed stage has in its module's place, gpu/vk; Stage names it)
#include "cache_types.inc"

// ((Triforce) dev.fixes.nan_bits: AMD's older shader compiler, whose modules make_module gives selects of the bits
// (spv_nan_select.h). Off here, as on every other card: every module is made of the record's words as they are.
// tests/test_spv_nan_select.py has the selects)
struct {
    uintptr_t device = 123;
    struct { bool nan_bits = false; } fixes;
} dev;
std::map<VkShaderModule, unsigned> destroyed;
unsigned attempts = 0, created = 0, releases = 0, fail_at = 0;
int vkCreateShaderModule(uintptr_t device, const VkShaderModuleCreateInfo* info, const void* allocator,
                         VkShaderModule* module) {
    CHECK(device == dev.device && allocator == nullptr);
    CHECK(info->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO && info->codeSize && info->pCode);
    ++attempts;
    *module = VK_NULL_HANDLE;
    if (attempts == fail_at) return -1;
    *module = ++created;
    CHECK(destroyed.emplace(*module, 0).second);
    return 0;
}
void vkDestroyShaderModule(uintptr_t device, VkShaderModule module, const void* allocator) {
    CHECK(device == dev.device && allocator == nullptr && module != VK_NULL_HANDLE);
    auto it = destroyed.find(module);
    CHECK(it != destroyed.end() && it->second == 0);
    ++it->second;
    ++releases;
}
void disk_init() {}
void logf(const char*, ...) {}
bool disk_on = true, disk_listing = true;
FILE* disk_in = nullptr;
FILE* disk_out = nullptr;
FILE* disk_idx = nullptr;
std::mutex disk_mu;
std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_records;
std::atomic<int64_t> st_cache_hits{0};
// ((Triforce) vk_shader.cpp's count of programs translated again after a cache of other translations was thrown away, for the
// log: none was here. tests/test_program_carry.py has the carry)
void dropped_note(uint64_t) {}
// (local, RenoDX port: vk_renodx.h's, which make_module asks for a fragment module's replacement. None here: every module
// is the cache's own, so the counts below are of what the loader made)
const std::vector<uint32_t>* renodx_replacement(uint32_t) { return nullptr; }
// ((Triforce) the fog's froxels, gpu/vk/spv_fog.h, which make_module asks of each module it makes: none here is the fog's, so
// every module is made of the record's words as they are. tests/test_spv_fog.py has the fog)
#include "spv_fog.h"
#include "spv_nan_select.h"
#define PROF_EXACT()   // (vk_prof.h's exact-timing scope, since #989: nothing to time here)
// (local, texture detail: vk_shader.cpp's note of which bindings a module reads by exact texel. Nothing to note here)
void exact_note(VkShaderModule, const std::vector<uint32_t>&) {}
// (Triforce, RX_VK_SPV_PACK: vk_shader.cpp's packing of a ready program's stages. Off here, as with RX_VK_SPV_PACK=0:
// every stage keeps its module, which is what this test counts. tests/test_spv_store.py has the packing)
void pack_note(VkShaderModule, const uint32_t*, size_t) {}
void program_pack(Program&) {}
struct PackSkip {
    PackSkip() {}   // (a scope of its own, as vk_shader.cpp's: nothing to change here)
};
// (Triforce: the memory line's count of the modules alive, vk_shader.cpp. Counted here too: a module the loader made is
// noted, and one a refused record made is forgotten as it is let go)
std::map<VkShaderModule, size_t> noted;
void module_note(VkShaderModule m, size_t words) {
    if (m) noted[m] = words;
}
void stage_module_destroy(VkShaderModule m) {
    CHECK(noted.erase(m) == 1);
    vkDestroyShaderModule(dev.device, m, nullptr);
}
// (Triforce: the read-ahead of programs, vk_shader.cpp, off here: every record is read by the loader itself, what this test
// counts. tests/test_program_readahead.py has the read-ahead)
bool readahead_take(uint64_t, std::vector<std::unique_ptr<Program>>&) { return false; }
void readahead_ask(uint64_t, int64_t) {}
std::atomic<int64_t> st_draw_reads{0}, st_draw_read_ns{0}, st_ra_taken{0}, st_ra_read{0}, st_ra_dropped{0}, st_ra_waits{0},
    st_ra_wait_ns{0}, st_ra_let_go{0}, st_ra_by_notes{0}, st_ra_read_ns{0};
// ((Triforce) a draw's own read with the shaders' lock let go, program_read_once.h: the loader is called as a pick calls it,
// shader_mu held (load below). tests/test_program_read_once.py has the threads; here what it reads is counted)
#include "program_read_once.h"
std::mutex shader_mu;
mkwvk::program_read_once::Reads<std::mutex> draw_once(shader_mu);
std::atomic<int64_t> st_once_waits{0}, st_once_wait_ns{0}, st_once_relock_ns{0}, st_once_unlocked{0};
bool read_unlocked() { return true; }
void readahead_drop(std::vector<std::unique_ptr<Program>>& variants) {   // (as vk_shader.cpp's: never here, nothing takes a record)
    for (auto& p : variants)
        for (auto& st : p->stages)
            if (st.module) stage_module_destroy(st.module), st.module = VK_NULL_HANDLE;
    variants.clear();
}
// ((Triforce) the sessions' order, program_order.h: off here; tests/test_program_order.py has it)
bool order_on() { return false; }
void order_note(uint64_t) {}
void readahead_why(uint64_t) {}
#include <chrono>
#include "cache_functions.inc"
void load(uint64_t key, std::vector<std::unique_ptr<Program>>& programs) {   // (as program_pick calls it)
    std::lock_guard lk(shader_mu);
    program_cache_load(key, programs);
}

void save(uint64_t key, bool empty_second = false, int bad_answer = 0) {
    Program program;
    program.key = key;
    program.stages.emplace_back();
    program.stages.emplace_back();
    program.stages[0].nvn_stage = 0;
    program.stages[0].vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
    program.stages[1].nvn_stage = 1;
    program.stages[1].vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    program.stages[1].info.handle_programs = {{1, 2}};
    program.stages[1].info.texture_descriptors = {0};
    program.answers.push_back({1, 0, 1, false});   // (a fitting answer: stage 1's one texture)
    // (the file is passed around: an answer naming a stage or slot the program does not have is refused)
    if (bad_answer == 1) program.answers.push_back({7, 0, 1, false});
    if (bad_answer == 2) program.answers.push_back({1, 3, 1, false});
    std::vector<std::vector<uint32_t>> code{{0x07230203}, {0x07230203}};
    if (empty_second) code[1].clear();
    program_cache_save(program, code);
    CHECK(disk_records.at(key).size() > 0);
}
void release_accepted(std::vector<std::unique_ptr<Program>>& programs) {
    for (auto& p : programs) for (auto& stage : p->stages) {
        CHECK(stage.module && destroyed.at(stage.module) == 0);
        vkDestroyShaderModule(dev.device, stage.module, nullptr);
        stage.module = VK_NULL_HANDLE;
    }
    programs.clear();
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    disk_in = disk_out = std::tmpfile();
    CHECK(disk_in);
    const std::string scenario = argv[1];
    save(7, scenario == "empty", scenario == "answer_stage" ? 1 : scenario == "answer_desc" ? 2 : 0);
    if (scenario == "first") fail_at = 1;
    if (scenario == "second" || scenario == "mixed") fail_at = 2;
    if (scenario == "truncated") --disk_records.at(7).back().size;
    if (scenario == "mixed") {
        save(7); // A valid variant after the failing variant, under the same key.
        save(9); // An independent key must also remain usable.
    }
    std::vector<std::unique_ptr<Program>> programs;
    load(7, programs);
    unsigned accepted = scenario == "valid" || scenario == "mixed" ? 1 : 0;
    CHECK(programs.size() == accepted && st_cache_hits == accepted);
    std::printf("%s: created=%u destroyed=%u accepted_modules=%u\n", scenario.c_str(), created, releases, accepted * 2);
    CHECK(created - releases == accepted * 2);
    if (scenario == "first") CHECK(created == 0 && releases == 0);
    if (scenario == "second" || scenario == "empty") CHECK(created == 1 && releases == 1);
    if (scenario == "truncated") CHECK(created == 0 && releases == 0);
    if (scenario == "answer_stage" || scenario == "answer_desc") CHECK(created == 2 && releases == 2);
    if (scenario == "valid") CHECK(created == 2 && releases == 0);
    if (scenario == "mixed") {
        CHECK(created == 3 && releases == 1);
        load(9, programs);
        CHECK(programs.size() == 2 && st_cache_hits == 2 && created == 5 && releases == 1);
        load(7, programs); // Consumed records cannot be cleaned up twice.
        CHECK(programs.size() == 2 && created == 5 && releases == 1);
    }
    for (const auto& p : programs) CHECK(p->ready.load() && p->stages.size() == 2);
    release_accepted(programs);
    CHECK(created == releases);
    for (const auto& [module, count] : destroyed) { (void)module; CHECK(count == 1); }
    CHECK(std::fclose(disk_in) == 0);
    std::puts("PASS: no live, invalid or double-destroyed module");
}
