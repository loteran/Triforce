// NVN2 programs to SPIR-V with Eden's shader recompiler (gpu/vk/eden). A stage's package is its control section
// (guest memory, 0xd8 bytes: magic 0x98761234, code length +0x38, c1 constants +0x40..+0x44, stage +0x56, workgroup
// size +0x58) and its code area (GPU memory: a graphics stage's 128-byte header, then the Ampere instructions, then
// the c1 constants). The instructions are lowered to Maxwell (frontend/ampere) and translated like any Maxwell
// program, its questions answered from the NVN state: c0 / c2 from the driver banks, c1 from the code area, user
// uniform buffers (c3-c18: slots 0-15) from guest memory, texture types from the registered textures. As the WebGPU
// renderer's gpu/render/rx_shader.cpp does for suyu's recompiler, whose comments say more.
//
// A program's stages are emitted with one Backend::Bindings, so their descriptors share one set in Eden's order
// (per stage: uniform buffers, storage buffers, texture buffers, image buffers, textures, images), which
// vk_pipeline.cpp lays out from the stages' Info the same way.
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#endif
#include <io.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <stdexcept>
#include <cstdio>
#include <functional>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <map>
#include <unordered_map>
#include <vector>

#include "shader_recompiler/backend/spirv/mma_layout.h"
#include "shader_recompiler/ir_opt/oot_visual_workaround.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell_regmap.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

#include "vk_paths.h"
#include "vk_internal.h"
#include "program_readahead.h"   // (Triforce: programs read ahead of their first draw)
#include "program_carry.h"   // (Triforce: the program cache over an update of the translator)
#include "stage_code_cache.h"   // (Triforce: the stages' decoded code kept by their bytes)
#include "vk_jobs.h"
#include "vk_draw.h"

// (EvilOOT, RX_VK_PTR_DIAG) the pointer pass's counts for this thread's translations
// (global_memory_to_storage_buffer_pass.cpp, declared by hand so no Eden header changes)
namespace Shader::Optimization {
struct PointerDiag {
    uint32_t tracked, fallback, fallback_table, mixed, split, fallback_split;
};
extern thread_local PointerDiag g_pointer_diag;
// (collect_shader_info_pass.cpp: NVN2 compute's storage buffer table for the program translated on this thread, and
// where its c0 layout ends: a pointer table at 0x880 when it ends before)
extern thread_local uint32_t g_nvn2_compute_ssbo_base;
extern thread_local uint32_t g_nvn2_compute_ssbo_count;
extern thread_local uint32_t g_nvn2_compute_layout_end;
extern thread_local bool g_nvn2_miss_log_this;
extern thread_local bool g_all_global_this;
extern thread_local bool g_gm_port_this;
}
namespace mkwvk {
struct NvnState;
uint32_t compute_ssbo_table(const NvnState& s, uint32_t* count, uint32_t* layout_end);   // (vk_bank.cpp)
bool seed_cs_buffer_word(uint32_t offset);   // (vk_bank.cpp)
bool key_listed(const char* hex, const char* var);   // (below)
}
namespace {
bool ptr_diag_on() {
    static const bool on = getenv("RX_VK_PTR_DIAG") && atoi(getenv("RX_VK_PTR_DIAG")) != 0;
    return on;
}
void ptr_diag_reset() { Shader::Optimization::g_pointer_diag = {}; }
/// One line a program whose global memory accesses are not all plain constant buffer pointers
void ptr_diag_log(uint64_t key, const char* kind) {
    if (!ptr_diag_on()) return;
    const auto& d = Shader::Optimization::g_pointer_diag;
    if (!d.fallback && !d.mixed && !d.split) return;
    mkwvk::logf("vk: pointers %s %016llx: %u bound (%u with memory in the address, %u SPLIT), %u run-time search (%u through a table, "
         "%u split)",
         kind, (unsigned long long)key, d.tracked, d.mixed, d.split, d.fallback, d.fallback_table, d.fallback_split);
}
} // namespace
#include "vk_shader.h"
#include "spv_exact.h"
#include "vk_texture.h"

namespace mkwvk {
namespace {

constexpr uint32_t kControlMagic = 0x98761234;
constexpr uint32_t kControlSize = 0xd8;
constexpr uint32_t kAmpereHeader = 128;

uint64_t hash_bytes(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

/// A stage's package, read from guest memory and lowered to Maxwell
struct Code {
    int stage = -1;   // NVN stage: 0 vertex, 1 fragment, 5 compute
    uint64_t key = 0;
    std::vector<uint64_t> code;   // header (0x50 bytes, zeros for compute), Maxwell instructions, native address map
    std::vector<uint8_t> c1;
    uint32_t local[3]{};
    uint32_t shared_memory = 0;   // compute: bytes from the NVN2 control section (+0x64; klimovich008's PR 91)
    // (hookshot) compute: local memory, bytes a thread, from the control section (+0x68; +0x50 is the same times 32, a warp's).
    // It was never read: a compute program that spills (LDL/STL) had no local array, and the SPIR-V back end aborted the game
    // at its first local access. OoT's two largest compute programs have 0xd80
    uint32_t local_memory = 0;
    uint32_t texture_bound = ~0u;   // the bank native TEX instructions name (~0: none; TEXS take the stage's own)
    bool isbe = false;              // an NVN2 task (stage 6) or mesh (stage 7) program: it writes the internal stage buffer
    // (task / mesh: control section +0x58 threads 15:0, payload bytes 31:16; +0x60 max primitives 15:0, max vertices
    // 31:16; +0x5f the output topology, 1 points, 3 triangles)
    uint32_t mesh_threads = 0, payload_bytes = 0, max_vertices = 0, max_primitives = 0, topology = 0;
    bool header_only = false;       // a graphics header and no code (NVN2's per-primitive output map of a mesh program)
    uint8_t sph[sizeof(Shader::ProgramHeader)]{};
    bool ok = false;
    // ((Triforce) RX_VK_STAGE_CODE_LAZY) a stage read for the picks as its head (stage_head): whole false, its code not lowered
    // yet, ok until the lowering says (codes_whole), its control section and code area in bytes for that
    bool whole = true;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
};

/// Whether this is a control section, and how much code it says there is
bool control_end(const uint8_t* ctl, uint32_t* end_out) {
    uint32_t magic, len, c1o, end;
    uint16_t st;
    std::memcpy(&magic, ctl, 4);
    std::memcpy(&len, ctl + 0x38, 4);
    std::memcpy(&c1o, ctl + 0x40, 4);
    std::memcpy(&end, ctl + 0x44, 4);
    std::memcpy(&st, ctl + 0x56, 2);
    if (magic != kControlMagic || !len || end > (16u << 20) || c1o > end || len > end || st > 7) return false;
    return *end_out = end, true;
}
uint64_t code_key(const uint8_t* ctl, const uint8_t* area, uint32_t end) {
    return hash_bytes(area, end, hash_bytes(ctl, kControlSize));
}
Code parse_code(const uint8_t* ctl, const uint8_t* area, uint32_t end);

// (Triforce) The stages' decoded code kept by their bytes (stage_code_cache.h has why): a program object met for the first time
// takes what was made of the same bytes before. RX_VK_STAGE_CODE_CACHE=<MB> (64), 0: each decoded again, as before
stage_code_cache::Cache<Code>& stage_cache() {
    static stage_code_cache::Cache<Code> c(stage_code_cache::budget(getenv("RX_VK_STAGE_CODE_CACHE")));
    return c;
}
std::atomic<int64_t> st_stage_reads{0}, st_stage_kept{0}, st_stage_decoded{0}, st_stage_decode_ns{0};
std::atomic<int64_t> st_stage_heads{0}, st_stage_lowered{0};   // ((Triforce) read as heads; lowered for a translation since)
/// What a Code takes besides its stage's bytes (the stage code cache's budget)
size_t code_cost(const Code& c) {
    return sizeof(Code) + c.code.size() * sizeof(uint64_t) + c.c1.size();
}
/// (the log, now and then: the stages read for the picks, how many were taken from the cache, and the decoding's time)
void stage_cache_log() {
    size_t entries = 0, used = 0;
    stage_cache().usage(&entries, &used);
    logf("vk: the stages' code read for the program picks: %lld stages, %lld taken from the stage code cache, %lld read without "
         "lowering their code, %lld of them lowered since for a translation, %lld decoded (%.1f ms in all); the cache keeps %zu "
         "stages, %.1f of %zu MB (RX_VK_STAGE_CODE_CACHE=0: none, each decoded again; RX_VK_STAGE_CODE_LAZY=0: each lowered as read)",
         (long long)st_stage_reads.load(), (long long)st_stage_kept.load(), (long long)st_stage_heads.load(),
         (long long)st_stage_lowered.load(), (long long)st_stage_decoded.load(), st_stage_decode_ns.load() / 1e6, entries,
         used / 1048576.0, stage_cache().budget() >> 20);
}

// ((Triforce) The stages read for the picks before their code is lowered (stage_code_cache.h has why): a pick needs a stage's
// key, NVN stage and header to find its program, the lowered code only to translate one. RX_VK_STAGE_CODE_LAZY=0: each lowered
// as it is read, as before; so too with the switches that act in the lowering itself, whose stages are lowered as before)
bool stage_lazy() {
    static const bool on = stage_code_cache::lazy_on(getenv("RX_VK_STAGE_CODE_LAZY")) && !getenv("RX_VK_REJECT_STAGES") &&
                           !getenv("RX_VK_REJECT_KEYS") && !getenv("RX_VK_OLD_LOWERING") && !getenv("RX_VK_DUMP_NATIVE") &&
                           !getenv("RX_VK_DUMP_FAILED");
    return on;
}
// ((Triforce) A pick reads the stages of a program object met for the first time with the shaders' lock let go where it may
// (codes_read, program_read_once.h). RX_VK_STAGE_CODE_UNLOCKED=0: under the lock, as before)
bool stage_unlocked() {
    static const bool on = stage_code_cache::unlocked_on(getenv("RX_VK_STAGE_CODE_UNLOCKED"));
    return on;
}
/// ((Triforce) A stage's head: what parse_code makes of it before it lowers the code (its key, NVN stage, task / mesh words and
/// graphics header). Whole when that is all there is to it (a header alone); else ok until the lowering says, which parse_code
/// goes on to do
Code stage_head(const uint8_t* ctl, const uint8_t* area, uint32_t end) {
    Code c;
    uint32_t len;
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4);
    std::memcpy(&st, ctl + 0x56, 2);
    c.key = code_key(ctl, area, end);
    c.stage = st;
    if (st == 6 || st == 7) {
        uint32_t w58, w60;
        std::memcpy(&w58, ctl + 0x58, 4), std::memcpy(&w60, ctl + 0x60, 4);
        c.isbe = true;
        c.mesh_threads = w58 & 0xFFFF, c.payload_bytes = w58 >> 16;
        c.max_primitives = w60 & 0xFFFF, c.max_vertices = w60 >> 16;
        c.topology = ctl[0x5f];
    }
    const bool graphics = st != 5;
    const uint32_t hdr = graphics ? kAmpereHeader : 0;
    if (graphics) std::memcpy(c.sph, area, std::min<size_t>(sizeof c.sph, end));
    if (graphics && len <= hdr + 16) {   // (a header alone)
        c.header_only = true;
        c.ok = true;
        return c;
    }
    c.ok = true, c.whole = false;
    return c;
}

/// A stage's code from guest memory, decoded; ((Triforce) head: for the picks (codes_of), its head where RX_VK_STAGE_CODE_LAZY,
/// or what the stage code cache kept for its bytes, whole or a head)
Code read_code(const nvn_stage_data& sd, bool head = false) {
    uint8_t ctl[kControlSize];
    uint32_t end = 0;
    if (!sd.control || !sd.data || !host->gpu_read(sd.control, ctl, sizeof ctl) || !control_end(ctl, &end)) return Code{};
    // ((Triforce) the control section and the code area in one buffer: the stage code cache's key)
    auto bytes = std::make_shared<std::vector<uint8_t>>(kControlSize + end);
    std::memcpy(bytes->data(), ctl, kControlSize);
    const uint8_t* area = bytes->data() + kControlSize;
    if (!host->gpu_read(sd.data, bytes->data() + kControlSize, end)) return Code{};
    if (const char* dir = getenv("RX_VK_DUMP_STAGES")) {   // (hookshot, RX_VK_DUMP_STAGES=<folder>: control and code as the game has them)
        char path[512];
        snprintf(path, sizeof path, "%s/%016llx.bin", dir, (unsigned long long)code_key(ctl, area, end));
        if (FILE* f = fopen(path, "wb")) fwrite(ctl, 1, sizeof ctl, f), fwrite(area, 1, end, f), fclose(f);
    }
    if (st_stage_reads.fetch_add(1, std::memory_order_relaxed) % 1024 == 1023) stage_cache_log();
    if (const auto kept = stage_cache().find(*bytes); kept && (head || kept->whole)) {
        st_stage_kept.fetch_add(1, std::memory_order_relaxed);
        return *kept;
    }
    Code c;
    if (head && stage_lazy()) {
        c = stage_head(ctl, area, end);
        st_stage_heads.fetch_add(1, std::memory_order_relaxed);
        if (!c.whole) c.bytes = bytes;   // (for its lowering: codes_whole)
    } else {
        const auto t0 = std::chrono::steady_clock::now();
        c = parse_code(ctl, area, end);
        st_stage_decoded.fetch_add(1, std::memory_order_relaxed);
        st_stage_decode_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                                     std::memory_order_relaxed);
    }
    stage_cache().put(bytes, std::make_shared<const Code>(c), code_cost(c));
    return c;
}

/// ((Triforce, RX_VK_STAGE_CODE_LAZY) A head's stage whole: what the stage code cache kept whole for its bytes, else lowered now
/// and kept so (in place of the head)
Code stage_whole(const std::shared_ptr<const std::vector<uint8_t>>& bytes) {
    if (const auto kept = stage_cache().find(*bytes); kept && kept->whole) return *kept;
    const auto t0 = std::chrono::steady_clock::now();
    Code c = parse_code(bytes->data(), bytes->data() + kControlSize, (uint32_t)(bytes->size() - kControlSize));
    st_stage_decoded.fetch_add(1, std::memory_order_relaxed), st_stage_lowered.fetch_add(1, std::memory_order_relaxed);
    st_stage_decode_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                                 std::memory_order_relaxed);
    stage_cache().put(bytes, std::make_shared<const Code>(c), code_cost(c));
    return c;
}

/// A control section and its code (end bytes), from the game's memory or the archives (the seed)
Code parse_code(const uint8_t* ctl, const uint8_t* area, uint32_t end) {
    Code c = stage_head(ctl, area, end);   // ((Triforce) the key, the stage, its words and header: stage_head's)
    if (c.whole) return c;
    c.ok = false, c.whole = true;
    uint32_t len, c1o;
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4);
    std::memcpy(&c1o, ctl + 0x40, 4);
    std::memcpy(&st, ctl + 0x56, 2);
    const bool graphics = st != 5;
    const uint32_t hdr = graphics ? kAmpereHeader : 0;
    using Shader::Ampere::Instruction;
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState capture;
    for (uint32_t i = 0; hdr + (i + 1) * sizeof(Instruction) <= len; i++) {
        Instruction& insn = native.emplace_back();
        std::memcpy(&insn, area + hdr + i * sizeof(Instruction), sizeof insn);
        const auto r = capture.Observe(insn, i);
        if (r == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) return c;
        if (r == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    {   // (hookshot, for bisecting a bad translation) RX_VK_REJECT_STAGES=<n,...> (NVN stages) and RX_VK_REJECT_KEYS=
        // <hex prefix,...>: those programs are taken as untranslatable, as a lowering failure leaves them
        static const std::string stages = getenv("RX_VK_REJECT_STAGES") ? getenv("RX_VK_REJECT_STAGES") : "";
        // (none by default: OoT's mesh program 32423793502c71e7 stayed out while it lost the device; it no longer does)
        static const std::string keys = getenv("RX_VK_REJECT_KEYS") && strcmp(getenv("RX_VK_REJECT_KEYS"), "-") ? getenv("RX_VK_REJECT_KEYS") : "";
        bool reject = false;
        for (size_t at = 0; !reject && at < stages.size();) {
            const size_t e = stages.find(',', at);
            const std::string t = stages.substr(at, e == std::string::npos ? std::string::npos : e - at);
            reject = !t.empty() && (unsigned)atoi(t.c_str()) == st;
            at = e == std::string::npos ? stages.size() : e + 1;
        }
        char hex[24];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)c.key);
        for (size_t at = 0; !reject && at < keys.size();) {
            const size_t e = keys.find(',', at);
            const std::string t = keys.substr(at, e == std::string::npos ? std::string::npos : e - at);
            reject = !t.empty() && !strncmp(hex, t.c_str(), t.size());
            at = e == std::string::npos ? keys.size() : e + 1;
        }
        if (reject) return c;
    }
    // (compute only: the host's cooperative matrices are, vk_coopmat.cpp)
    // (hookshot) and only for the programs in RX_VK_NATIVE_MMA_KEYS (key prefixes, default the gameplay TAAU network,
    // whose output vk_draw.cpp's stand-in replaces; "all": every one). The others take the exact expansion: with the
    // cooperative matrices the movie upscaler lost its blue (the title's clip yellow) though the device's fragment
    // layout passes vk_coopmat.cpp's multiply; exact, its colours are right at the same frame rate
    // (default "all": a program left off it is expanded only with EDEN_NX2_TENSOR set, and is otherwise not lowered
    // at all; the per-key default dropped OoT's other gameplay networks, e409733c and others)
    static const std::string native_keys = getenv("RX_VK_NATIVE_MMA_KEYS") ? getenv("RX_VK_NATIVE_MMA_KEYS") : "all";
    bool native_key = native_keys == "all";
    {
        char hex[24];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)c.key);
        for (size_t at = 0; !native_key && at < native_keys.size();) {
            const size_t e = native_keys.find(',', at);
            const std::string t = native_keys.substr(at, e == std::string::npos ? std::string::npos : e - at);
            native_key = !t.empty() && !strncmp(hex, t.c_str(), t.size());
            at = e == std::string::npos ? native_keys.size() : e + 1;
        }
    }
    Shader::Ampere::native_matrix_multiply = Shader::AmpereRegmap::native_matrix_multiply = dev.native_mma && !graphics && native_key;
    // (triforce 11-08) EvilOOT's lowering (lower_to_maxwell.h: high GPRs shadowed in more register banks), and Triforce's old
    // one (lower_to_maxwell_regmap.h: REGMAP_nx2 extended registers) for a program it rejects, so that nothing drawn before is
    // lost. RX_VK_OLD_LOWERING=1: the old one for every program, as before
    static const bool old_lowering = getenv("RX_VK_OLD_LOWERING") != nullptr;
    Shader::Ampere::LoweredProgram lowered;
    if (!old_lowering) lowered = Shader::Ampere::LowerToMaxwell(native);
    if (old_lowering || !lowered) {
        static_assert(sizeof(Shader::AmpereRegmap::Instruction) == sizeof(Instruction));
        const auto old = Shader::AmpereRegmap::LowerToMaxwell(
            std::span{reinterpret_cast<const Shader::AmpereRegmap::Instruction*>(native.data()), native.size()});
        if (old) {
            if (!old_lowering) {
                static std::atomic<int> logged{0};
                if (logged++ < 64)
                    logf("vk: NVN2 program %016llx (stage %u): the old lowering took it (the new one: %s at +%#x)%s",
                         (unsigned long long)c.key, st, std::string(lowered.error).c_str(), lowered.error_offset,
                         old.register_map_words ? " with extended registers" : "");
            }
            lowered = {};
            lowered.instructions = old.instructions;
            lowered.source_instruction_offsets = old.source_instruction_offsets;
            lowered.source_size = old.source_size;
            lowered.required_texture_buffer = old.required_texture_buffer;
            lowered.high_uniform_registers = old.high_uniform_registers;
        } else if (old_lowering) {
            lowered.error = old.error, lowered.error_offset = old.error_offset;
        }
    }
    if (const char* want = getenv("RX_VK_DUMP_NATIVE")) {   // (hookshot) <key prefix,...>: native code of those programs ("all": every one)
        char hex[24];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)c.key);
        const bool all = !strcmp(want, "all");
        for (const char* q = want; *q;) {
            const char* e = q;
            while (*e && *e != ',') e++;
            if (all || (e > q && !strncmp(hex, q, (size_t)(e - q)))) {
                // (triforce: RX_VK_DUMP_NATIVE_DIR=<folder> puts them there, for the lowering tests in nx2/lowering)
                const char* dir = getenv("RX_VK_DUMP_NATIVE_DIR");
                char path[512];
                snprintf(path, sizeof path, "%s%snative_%u_%s.bin", dir ? dir : "", dir ? "/" : "", st, hex);
                if (FILE* f = fopen(path, "wb")) {
                    fwrite(native.data(), sizeof(Instruction), native.size(), f);
                    fclose(f);
                }
            }
            q = *e ? e + 1 : e;
        }
    }
    if (!lowered) {   // (hookshot: each program once, up to 400, with the native opcode it stopped at)
        static std::mutex mu;
        static std::unordered_set<uint64_t> seen;
        std::lock_guard lk(mu);
        if (seen.size() < 400 && seen.insert(c.key).second) {
            const uint32_t at = lowered.error_offset / (uint32_t)sizeof(Instruction);
            const uint32_t op = at < native.size() ? (uint32_t)native[at].Bits(0, 12) : 0xFFFFFFFFu;
            logf("vk: NVN2 program %016llx (stage %u) does not lower at +%#x: %s (native op %#x, %08x %08x %08x %08x)", (unsigned long long)c.key,
                 st, lowered.error_offset, std::string(lowered.error).c_str(), op, at < native.size() ? native[at].words[0] : 0,
                 at < native.size() ? native[at].words[1] : 0, at < native.size() ? native[at].words[2] : 0, at < native.size() ? native[at].words[3] : 0);
            // (RX_VK_DUMP_FAILED=<folder>: the native code, for nx2/lowering tests: <stage>_<key>.bin, 16-byte words)
            if (const char* dir = getenv("RX_VK_DUMP_FAILED")) {
                char path[512];
                snprintf(path, sizeof path, "%s/%u_%016llx.bin", dir, st, (unsigned long long)c.key);
                if (FILE* f = fopen(path, "wb")) {
                    fwrite(native.data(), sizeof(Instruction), native.size(), f);
                    fclose(f);
                }
            }
        }
        return c;
    }
    c.texture_bound = lowered.required_texture_buffer;
    c.code.assign(sizeof(Shader::ProgramHeader) / 8, 0);
    if (graphics) std::memcpy(c.code.data(), area, sizeof(Shader::ProgramHeader));
    for (const uint64_t instruction : lowered.instructions) {
        if (c.code.size() * 8 % 32 == 0) c.code.push_back(0);   // (the Maxwell scheduling word)
        c.code.push_back(instruction);
    }
    c.code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(c.code, lowered.source_instruction_offsets);
    c.c1.assign(area + c1o, area + end);
    if (!graphics) {
        std::memcpy(c.local, ctl + 0x58, 12);
        std::memcpy(&c.shared_memory, ctl + 0x64, 4);
        std::memcpy(&c.local_memory, ctl + 0x68, 4);
        uint32_t warp_local = 0;   // (+0x50: the same times 32, a warp's; a size that does not match it is not trusted)
        std::memcpy(&warp_local, ctl + 0x50, 4);
        if (c.local_memory && (uint64_t{c.local_memory} * 32 != warp_local || c.local_memory > (64u << 10))) {
            static std::atomic<int> told{0};
            if (told++ < 8)
                logf("vk: compute program %016llx: local memory %#x a thread, %#x a warp: not used", (unsigned long long)c.key,
                     c.local_memory, warp_local);
            c.local_memory = 0;
        }
    }
    c.ok = true;
    return c;
}

Shader::TextureType texture_type(int target) {
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: return Shader::TextureType::Color1D;
    case NVN_TEXTURE_TARGET_3D: return Shader::TextureType::Color3D;
    case NVN_TEXTURE_TARGET_1D_ARRAY: return Shader::TextureType::ColorArray1D;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return Shader::TextureType::ColorArray2D;
    case NVN_TEXTURE_TARGET_RECTANGLE: return Shader::TextureType::Color2DRect;
    case NVN_TEXTURE_TARGET_CUBEMAP: return Shader::TextureType::ColorCube;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return Shader::TextureType::ColorArrayCube;
    case NVN_TEXTURE_TARGET_BUFFER: return Shader::TextureType::Buffer;
    default: return Shader::TextureType::Color2D;
    }
}

} // namespace

Shader::TextureType handle_texture_type(uint32_t handle) {
    nvn_tex t;
    nvn_view v;
    bool has_view = false;
    if (!handle || !texture_of((int)(handle & 0xFFFFF), &t, &v, &has_view)) return Shader::TextureType::Color2D;
    return texture_type(has_view && v.set_target ? v.target : t.d.target);
}

namespace {

bool handle_is_integer(uint32_t handle) {
    nvn_tex t;
    nvn_view v;
    bool has_view = false;
    if (!handle || !texture_of((int)(handle & 0xFFFFF), &t, &v, &has_view)) return false;
    const VkFormat f = vk_format(has_view && v.set_format ? v.format : t.d.format).format;
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_S8_UINT: return true;
    default: return false;
    }
}

// (hookshot) A compute program's texture variants (seed_compute): translated with nothing bound, every texture question of a compute program
// is asked of handle 0, and which slot asks is not known. Under `forced`, c0's texture and image handles read as tokens of their
// slots, each question is noted once by its handle, and the answer wanted for it is given in place of an unbound slot's
struct Forced {
    uint32_t lo = 0, hi = 0;   // c0's texture and image handles (the program's layout)
    std::vector<std::pair<uint32_t, uint8_t>> asked;   // (handle, 0: its type / 1: whether its format is integer), in order
    std::map<std::pair<uint32_t, uint8_t>, uint8_t> want;
    uint8_t answer(uint32_t handle, uint8_t kind, uint8_t plain) {
        const std::pair<uint32_t, uint8_t> q{handle, kind};
        if (std::find(asked.begin(), asked.end(), q) == asked.end()) asked.push_back(q);
        const auto it = want.find(q);
        return it != want.end() ? it->second : plain;
    }
};
thread_local Forced* forced = nullptr;
thread_local std::vector<std::vector<uint32_t>>* seed_capture = nullptr;   // (the translation's SPIR-V, kept here and not on disk)

// (hookshot) The texture questions of a translation, in the order asked: noted in play (Program::questions), given back in that order
// when the pipeline state list makes the same variant before the game, with nothing bound to ask about
thread_local std::vector<uint8_t>* questions_out = nullptr;
thread_local const std::vector<uint8_t>* questions_in = nullptr;
thread_local size_t questions_at = 0;
uint8_t question_answer(uint8_t live) {
    if (questions_in) return questions_at < questions_in->size() ? (*questions_in)[questions_at++] : live;
    if (questions_out) questions_out->push_back(live);
    return live;
}
struct QuestionsNoted {
    explicit QuestionsNoted(Program& p) {
        p.questions.clear();
        if (questions_in) p.questions = *questions_in;
        else questions_out = &p.questions;
    }
    ~QuestionsNoted() { questions_out = nullptr; }
};
thread_local const std::vector<Program::Answer>* answers_in = nullptr;   // (and the variant's answers then)

class Env final : public Shader::Environment {
public:
    Env(const NvnState& state, const Code& c, Shader::Stage st) : s{state}, nvn{c} {
        stage = st;
        start_address = st == Shader::Stage::Compute ? static_cast<u32>(sizeof(Shader::ProgramHeader)) : 0;
        if (st != Shader::Stage::Compute) {
            std::memcpy(&sph, reinterpret_cast<const uint8_t*>(nvn.code.data()), sizeof sph);
            local_memory = static_cast<uint32_t>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
        } else {
            local_memory = nvn.local_memory;   // (its control section's: a compute program has no header)
        }
        is_proprietary_driver = true;
    }
    u64 ReadInstruction(u32 address) override {
        return address / 8 < nvn.code.size() ? nvn.code[address / 8] : 0;
    }
    u32 TranslateProgramAddress(u32 address) const override {
        if (const auto t = Shader::Ampere::NativeProgramAddress(nvn.code, address)) return *t;
        throw Shader::RuntimeError("indirect branch to native address {:#x}, not in the program", address);
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        if (index == 1) {
            uint32_t v = 0;
            if ((size_t)offset + 4 <= nvn.c1.size()) std::memcpy(&v, nvn.c1.data() + offset, 4);
            return v;
        }
        if (forced && index == 0 && nvn.stage == 5 && offset >= forced->lo && offset < forced->hi && !(offset & 7))
            return 0x8000 | offset;   // (a slot's token: nothing is bound in the seed)
        if (index == 0 || index == 2) return driver_bank_word(s, (int)index, nvn.stage, offset);
        if (index >= 3 && index < 3 + 16) {
            const nvn_p_bind_buf& u = s.ubo[nvn.stage][index - 3];
            uint32_t v = 0;
            if (u.addr && (uint64_t)offset + 4 <= (uint64_t)u.size) host->gpu_read(u.addr + offset, &v, 4);
            return v;
        }
        return 0;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        texture_questions++;
        if (forced && handle) return (Shader::TextureType)forced->answer(handle, 0, (uint8_t)Shader::TextureType::Color2D);
        return static_cast<Shader::TextureType>(question_answer(static_cast<uint8_t>(handle_texture_type(handle))));
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat{};   // (only for formatless image loads, which the device has)
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        texture_questions++;
        if (forced && handle) return forced->answer(handle, 1, 0) != 0;
        return question_answer(handle_is_integer(handle) ? 1 : 0) != 0;
    }
    int texture_questions = 0;   // (the variant then depends on the bound textures: Program::answers)
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        if (nvn.texture_bound != ~0u) return nvn.texture_bound;   // (as Eden's GraphicsEnvironment: TEX names it)
        return stage == Shader::Stage::Compute ? 0 : 2;   // (NVN2 compute: handles in c0)
    }
    u32 LocalMemorySize() const override {
        return local_memory;
    }
    u32 SharedMemorySize() const override {
        // (what the program declares, from the control section: 48 KB for every one was past an RX 9070 XT's 32 KB, and its
        // pipelines were invalid. klimovich008, Hookshot PR 91. RX_VK_SHARED_48K=1: as before)
        static const bool old = getenv("RX_VK_SHARED_48K") != nullptr;
        return old ? 48u << 10 : nvn.shared_memory;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {std::max(1u, nvn.local[0]), std::max(1u, nvn.local[1]), std::max(1u, nvn.local[2])};
    }
    // NVN's draw macros put the draw's base vertex, base instance and draw index in the vertex stage's driver bank, at
    // 0x640, 0x644 and 0x648 (Eden's macro.cpp HLE: DrawIndexedInstanced and the indirect forms); a program reads
    // them there for gl_BaseVertex / gl_BaseInstance / gl_DrawID. They are Vulkan's BaseVertex, BaseInstance and
    // DrawIndex, right for indirect draws too. The bank had zeros there: MKW's terrain patches, which pass log2 of
    // their template grid as the base instance (4: 16 x 16, 3: 8 x 8), took a 1 x 1 grid, every vertex on a corner:
    // each patch two triangles, one of them often degenerate (the triangular holes in the far terrain).
    // (RX_VK_NO_DRAW_PARAMS=1: as before)
    bool HasHLEMacroState() const override {
        static const bool off = getenv("RX_VK_NO_DRAW_PARAMS") != nullptr;
        return !off && (stage == Shader::Stage::VertexA || stage == Shader::Stage::VertexB);
    }
    bool HasNativeInstructions() const override { return true; }   // (lowered NVN2 programs: Lichii's passes ask)
    bool IsNativeProgram() const override { return true; }   // (Fritchii's storage buffer pass asks: EDEN_NX2_GM_NATIVE=0 off)
    /// (Fritchii's storage buffer pass) whether the 8 bytes at c<index>+offset are a buffer's address now: in a memory pool
    /// the game made. A texture handle and a float next to it (0x10001c00a, c5+0x60 of OoT's mesh programs) are not; nor is
    /// anything in c1, the program's own constants. Answered once per word (the pass asks per access).
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {
        if (index == 1) return false;
        // (#940: the seed's compute programs, before anything is bound: c0's buffer tables, as play has them)
        if (seed_cs_control && stage == Shader::Stage::Compute) return index == 0 && seed_cs_buffer_word(offset);
        const uint64_t key = (uint64_t)index << 32 | offset;
        if (const auto it = pointer_answers.find(key); it != pointer_answers.end()) return it->second;
        const uint64_t v = ReadCbufValue(index, offset) | (uint64_t)ReadCbufValue(index, offset + 4) << 32;
        // (4-byte aligned: a pointer the program reads words through. vk_bank.cpp's pointer table asks the same)
        const bool pointer = v && !(v & 3) && host->gpu_guest(v) && gpu_pool_end(v) > v;
        pointer_answers.emplace(key, pointer);
        return pointer;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
        if (bank != 0 || !HasHLEMacroState()) return std::nullopt;
        switch (offset) {
        case 0x640: return Shader::ReplaceConstant::BaseVertex;
        case 0x644: return Shader::ReplaceConstant::BaseInstance;
        case 0x648: return Shader::ReplaceConstant::DrawID;
        default: return std::nullopt;
        }
    }
    void Dump(u64, u64) override {}
    /// A task or mesh program's SET_MESH_SHADER_A / B and SET_MESH_INIT_SHADER, and (mesh) the header-only stage whose
    /// output map lists its per-primitive outputs (Eden's GraphicsEnvironment reads the pass-through geometry
    /// program's header for that)
    void set_mesh(const std::array<u32, 3>& registers, const uint8_t* primitive_sph) {
        mesh_registers = registers;
        if (primitive_sph) std::memcpy(&mesh_primitive_header, primitive_sph, sizeof mesh_primitive_header);
    }

private:
    const NvnState& s;
    const Code& nvn;
    uint32_t local_memory = 0;
    std::unordered_map<uint64_t, bool> pointer_answers;   // (IsCbufStorageBufferPointer's)
};

Shader::Profile make_profile() {
    Shader::Profile p{};
    p.supported_spirv = 0x00010600;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = p.support_int16 = p.support_int64 = true;
    p.support_uniform_and_storage_buffer_8bit = p.support_storage_buffer_8bit = true;
    p.support_uniform_and_storage_buffer_16bit = p.support_storage_buffer_16bit = true;
    p.support_vertex_instance_id = false;
    p.support_float_controls = true;
    p.support_separate_denorm_behavior = p.support_separate_rounding_mode = true;
    p.support_fp32_denorm_preserve = p.support_fp32_denorm_flush = true;
    p.support_fp16_signed_zero_nan_preserve = p.support_fp32_signed_zero_nan_preserve = true;
    p.support_vote = true;
    p.supported_subgroup_stages = VK_SHADER_STAGE_ALL;
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_int64_atomics = true;
    p.support_derivative_control = true;
    p.support_native_ndc = false;   // (the vertex stage converts NVN's -1..1 depth: RuntimeInfo::convert_depth_mode)
    p.support_scaled_attributes = false;   // (*_UI2F / *_I2F read as integers, converted in the shader: R32 has no scaled format)
    p.support_multi_viewport = true;
    p.support_sampled_image_array_nonuniform_indexing = true;
    // Unpinned SPIR-V 1.6 stages can use any supported subgroup size, even when the default is 32.
    // The stage-specific profiles below keep the short path only where pin_subgroup actually pins 32 lanes.
    p.warp_size_potentially_larger_than_guest = dev.subgroup_max > 32;
    // (RX_VK_WARP64=0 or 1, a test: the graphics stages' long warp path off or on, whatever the GPU; in the cache's stamp)
    if (const char* e = getenv("RX_VK_WARP64")) p.warp_size_potentially_larger_than_guest = e[0] == '1';
    p.lower_left_origin_mode = false;
    p.need_declared_frag_colors = false;
    // (the lanes the broadcast fallbacks walk: all of the host's when a warp may be larger than the guest's)
    p.max_subgroup_size = p.warp_size_potentially_larger_than_guest ? dev.subgroup_max : 32;
    // (Lichii #273) AMD's Windows compiler gets fragment-stage subgroup shuffles wrong (a basket in Link's house drew as a white
    // blob): broadcasts and selects there instead
    p.has_broken_spirv_fragment_shuffle = dev.driver_id == VK_DRIVER_ID_AMD_PROPRIETARY;
    // (RX_VK_FRAG_SHUFFLE_FALLBACK=0 or 1, a test: the fragment stages' shuffles native or through broadcasts, whatever the
    // driver; in the cache's stamp)
    if (const char* e = getenv("RX_VK_FRAG_SHUFFLE_FALLBACK")) p.has_broken_spirv_fragment_shuffle = e[0] == '1';
    // (hookshot) gathers nudged by 1/512 of a texel on AMD's and Intel's drivers, as Eden and Lichii have it (vk_vendor.h; in the
    // cache's stamp)
    p.need_gather_subpixel_offset = dev.fixes.gather_offset;
    p.min_ssbo_alignment = storage_binding_align();   // (at least 16: vk_internal.h)
    return p;
}

Shader::HostTranslateInfo host_info() {
    Shader::HostTranslateInfo h{};
    h.support_float64 = true;
    // (RX_VK_NO_F16, a test: the game's f16 math lowered to f32, as a card without f16 shaders would need (GTX 10xx, issue 98). Eden's
    // lowering made a picture of noise of Kokiri on an RTX card, so it is not used by itself)
    h.support_float16 = !getenv("RX_VK_NO_F16");
    h.support_int64 = true;
    h.needs_demote_reorder = false;
    h.support_snorm_render_buffer = true;
    h.support_viewport_index_layer = true;
    h.min_ssbo_alignment = storage_binding_align();
    h.support_geometry_shader_passthrough = false;
    h.support_conditional_barrier = true;   // (barriers in loops kept: see the WebGPU renderer's 4f5e3e6)
    return h;
}

/// A translated stage: its IR program (kept for the next stage's inputs) and the pools its IR lives in
struct Translation {
    uint32_t fallbacks = 0;     // (texture handles no handle program followed: Shader::Optimization::TakeHandleFallbacks)
    uint32_t tables = 0;        // (Triforce: texture handles read from a table of records, one texture an element)
    std::string fallback_ops;
    Shader::ObjectPool<Shader::IR::Inst> insts;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;
    Shader::IR::Program program;
};

std::unique_ptr<Translation> translate(Env& env, bool graphics, bool one_exit = false) {
    auto t = std::make_unique<Translation>();
    // (NVN2 task / mesh programs get one exit: their counts are set there, as Eden's pipeline cache does)
    Shader::Maxwell::Flow::CFG cfg{env, t->flow_blocks,
                                   env.StartAddress() + (graphics ? static_cast<u32>(sizeof(Shader::ProgramHeader)) : 0u),
                                   one_exit};
    Shader::Optimization::TakeHandleFallbacks(t->fallback_ops);   // (none from an earlier one on this thread)
    Shader::Optimization::TakeHandleTables();
    t->program = Shader::Maxwell::TranslateProgram(t->insts, t->blocks, env, cfg, host_info());
    t->fallbacks = Shader::Optimization::TakeHandleFallbacks(t->fallback_ops);
    t->tables = Shader::Optimization::TakeHandleTables();
    return t;
}

} // namespace
} // namespace mkwvk
#include "vk_renodx.h"
namespace mkwvk {
namespace {

// ---- (Triforce) Outside the translation stamp, to the next mark (nx2/build.py translation_text; the rule: program_carry.h): the
// shader modules made of a translation's SPIR-V, which they take const, and their packing. A program read from the cache and one
// just translated go through them alike, and a record holds the SPIR-V, not the module ----
/// (local, RenoDX port) The CRC-32 of a module's words as the bytes they are: RenoDX names the shaders it replaces by it
uint32_t spirv_crc32(const std::vector<uint32_t>& spirv) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> out{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            out[i] = k;
        }
        return out;
    }();
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)spirv.data();
    for (size_t i = 0, n = spirv.size() * 4; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/// (local, RenoDX port) *renodx: set when RenoDX has a replacement for this module, which is made in its place (vk_renodx.h)
// (Triforce, texture detail: the texture bindings each module reads by texel position or asks the size of, spv_exact.h;
// kept only where the card's detail level leaves levels out, and only for the modules that have such a binding)
std::mutex exact_mu;
std::atomic<uint64_t> exact_gen{1};   // (bumped when an entry changes: shader_reads_exact's per-thread answer)
std::unordered_map<VkShaderModule, spv_exact::Bindings> exact_of;
void exact_note(VkShaderModule m, const std::vector<uint32_t>& code) {
    if (!m || texture_detail_level() <= 0) return;
    const spv_exact::Bindings b = spv_exact::scan(code.data(), code.size());
    std::lock_guard lk(exact_mu);
    if (spv_exact::any(b)) exact_of[m] = b;
    else if (!exact_of.erase(m)) return;   // (a handle used again; nothing kept and nothing there: no thread's answer changes)
    exact_gen.fetch_add(1, std::memory_order_release);
}
// ---- (Triforce) RX_VK_SPV_PACK: a ready program's stages keep their SPIR-V packed and no module (spv_store.h, where the
// reasons and the measurements are). make_module packs the words it hands to the driver and keeps the store beside the
// module's handle (pack_note); program_pack, called where a program becomes ready, gives each stage its store and destroys
// the module; StageModule (vk_shader.h) makes one again for a pipeline's creation. On by default since 11-15 (RX_VK_SPV_PACK=0:
// every module kept for the run, as before): Vulkan lets a module go once the pipelines and libraries made with it are
// made, and a module made again from the same words is the same module to the driver's caches (they go by its words) ----
using StageCode = spv_store::Code<VkShaderModule>;
std::mutex pack_mu;
std::unordered_map<VkShaderModule, std::shared_ptr<StageCode>> pack_made;   // (by the module made from the same words)
std::atomic<int64_t> st_pack_stages{0}, st_pack_plain{0}, st_pack_bytes{0}, st_pack_modules{0};
// ((Triforce) the stores alive and what they take, for the memory line (mem_parts.h): a store counts from its fill to its end.
// And what the packing costs, for the "shader code packed" line: the packing, on the thread that made the module, and the
// modules made again for pipelines, on the thread that builds the pipeline)
std::atomic<int64_t> st_pack_live{0}, st_pack_live_bytes{0}, st_pack_ns{0}, st_remake_ns{0}, st_remake_max_ns{0};
// ((Triforce) The programs whose modules are let go soon after they are made gain nothing from a store, and would pay its
// packing (about 0.2 ms a 70 KiB stage) and a module made again from it for each pipeline: those the prepare translates and
// lets go (seed_translate), those borrowed for the driver's part and the predicted pipelines (program_cache_borrow), and
// compute programs, which keep their modules (program_pack). A program a draw reads from the cache itself is read on the
// thread replaying, under shader_mu: it keeps its modules, as before, rather than have its packing there. PackSkip says so
// for the modules made on this thread while it lives)
thread_local int pack_skip = 0;
struct PackSkip {
    PackSkip() { pack_skip++; }
    ~PackSkip() { pack_skip--; }
    PackSkip(const PackSkip&) = delete;
    PackSkip& operator=(const PackSkip&) = delete;
};
/// (a handle used again by the driver names the new module's store: every program module is made by make_module, which
/// writes its entry over whatever an earlier module of that handle left, or takes that entry away)
void pack_note(VkShaderModule m, const uint32_t* words, size_t n) {
    if (!m || !spv_pack_on()) return;
    if (pack_skip) {   // (no store: and none that a module of this handle left before, which program_pack would take for this one)
        std::lock_guard lk(pack_mu);
        pack_made.erase(m);
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::shared_ptr<StageCode> code(new StageCode, [](StageCode* c) {
        if (c->filled()) st_pack_live--, st_pack_live_bytes -= (int64_t)c->packed_bytes();
        delete c;
    });
    const bool ok = code->fill(words, n);
    if (ok) st_pack_live++, st_pack_live_bytes += (int64_t)code->packed_bytes();
    st_pack_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard lk(pack_mu);
    if (ok) pack_made[m] = std::move(code);
    else pack_made.erase(m);   // (it did not come back the same: this stage keeps its module)
}
// ---- (Triforce) The program variants' shader modules alive, and the SPIR-V they were made of: the memory line's
// (mem_parts.h). make_module notes each, stage_module_destroy (and program_pack) forget it; a handle the driver uses again
// for a module let go unseen is noted anew ----
std::mutex module_mu;
std::unordered_map<VkShaderModule, uint32_t> module_words;
std::atomic<int64_t> st_module_live{0}, st_module_bytes{0};
void module_note(VkShaderModule m, size_t words) {
    if (!m) return;
    std::lock_guard lk(module_mu);
    const auto [it, fresh] = module_words.try_emplace(m, (uint32_t)words);
    if (fresh) st_module_live++;
    else st_module_bytes -= (int64_t)it->second * 4, it->second = (uint32_t)words;
    st_module_bytes += (int64_t)words * 4;
}
void module_forget(VkShaderModule m) {
    if (!m) return;
    std::lock_guard lk(module_mu);
    const auto it = module_words.find(m);
    if (it == module_words.end()) return;
    st_module_live--, st_module_bytes -= (int64_t)it->second * 4;
    module_words.erase(it);
}
void exact_move(VkShaderModule from, VkShaderModule to);
/// A program that is about to be ready (no other thread has it yet): each stage's module gives way to its packed SPIR-V.
/// Graphics and mesh programs, the mesh fallback's too: their pipelines are made by build_graphics and
/// fallback_compute_pipelines (vk_draw.cpp), which take the modules through StageModule. A compute program keeps its
/// module: it has one pipeline, and the four places that make it read the module's field
void program_pack(Program& p) {
    if (!spv_pack_on()) return;
    for (Stage& st : p.stages) {
        if (!st.module || st.code) continue;
        std::shared_ptr<StageCode> code;
        {
            std::lock_guard lk(pack_mu);
            const auto it = pack_made.find(st.module);
            if (it == pack_made.end()) continue;
            code = std::move(it->second);
            pack_made.erase(it);
        }
        if (p.failed || p.compute) continue;   // (its store is let go, its module stays)
        exact_move(st.module, (VkShaderModule)(uintptr_t)code.get());
        module_forget(st.module);
        vkDestroyShaderModule(dev.device, st.module, nullptr);
        st.module = VK_NULL_HANDLE;
        st_pack_plain += (int64_t)code->words() * 4, st_pack_bytes += (int64_t)code->packed_bytes();
        st.code = std::move(code);
        const int64_t n = ++st_pack_stages;
        if (n == 1 || n % 4096 == 0)
            logf("vk: shader code packed (RX_VK_SPV_PACK=0: not): %lld stages, %.1f MB of SPIR-V kept as %.1f MB (the packing %.0f ms "
                 "in all); %lld modules made again for pipelines since the start, in %.0f ms in all (the longest %.2f ms)", (long long)n,
                 st_pack_plain.load() / 1048576.0, st_pack_bytes.load() / 1048576.0, st_pack_ns.load() / 1e6,
                 (long long)st_pack_modules.load(), st_remake_ns.load() / 1e6, st_remake_max_ns.load() / 1e6);
    }
}

VkShaderModule make_module(const std::vector<uint32_t>& spirv, uint32_t* renodx = nullptr, uint32_t* crc_out = nullptr) {
    const uint32_t crc = spirv_crc32(spirv);
    if (crc_out) *crc_out = crc;
    // (local, RenoDX port) RX_VK_DUMP_ALL_SPV=<folder>: every module made, as <crc32>.spv
    if (static const char* dir = getenv("RX_VK_DUMP_ALL_SPV"); dir) {
        char path[512];
        snprintf(path, sizeof path, "%s/%08X.spv", dir, crc);
        if (FILE* f = fopen(path, "wb")) fwrite(spirv.data(), 4, spirv.size(), f), fclose(f);
    }
    const std::vector<uint32_t>* const other = renodx ? renodx_replacement(crc) : nullptr;
    if (other) {
        *renodx = crc;
        static std::atomic<int> told{0};
        if (told++ < 16) logf("vk: RenoDX: module %08X replaced (%zu words for %zu)", crc, other->size(), spirv.size());
    }
    const std::vector<uint32_t>& code = other ? *other : spirv;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size() * 4;
    ci.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    // (lab, 11-13) EDEN_SPV_SALT=<n>: every module's generator word (the header's third, free for the generator's use) carries
    // n, so the driver's own cache on disk has met none of them: a start that compiles everything again, at will, without
    // touching that cache. An EDEN_ name: in the program cache's stamp
    static const uint32_t salt = getenv("EDEN_SPV_SALT") ? (uint32_t)strtoul(getenv("EDEN_SPV_SALT"), nullptr, 0) : 0;
    if (salt && spirv.size() > 5) {
        std::vector<uint32_t> salted = spirv;
        salted[2] ^= salt;
        ci.pCode = salted.data();
        vkCreateShaderModule(dev.device, &ci, nullptr, &m);
        module_note(m, salted.size());
        exact_note(m, code);
        pack_note(m, salted.data(), salted.size());
        return m;
    }
    vkCreateShaderModule(dev.device, &ci, nullptr, &m);
    module_note(m, code.size());
    exact_note(m, code);
    pack_note(m, code.data(), code.size());
    return m;
}
/// (the packed stage's answer for shader_reads_exact goes with it, under the key that stands for its module: stage_exact_key)
void exact_move(VkShaderModule from, VkShaderModule to) {
    std::lock_guard lk(exact_mu);
    const auto it = exact_of.find(from);
    const bool had = it != exact_of.end();
    if (had) exact_of[to] = it->second, exact_of.erase(from);
    else if (!exact_of.erase(to)) return;   // (nothing kept for either: no thread's answer changes)
    exact_gen.fetch_add(1, std::memory_order_release);
}

} // namespace

bool spv_pack_on() {
    static const bool on = !(getenv("RX_VK_SPV_PACK") && atoi(getenv("RX_VK_SPV_PACK")) == 0);
    return on;
}
void stage_module_destroy(VkShaderModule m) {
    if (!m) return;
    module_forget(m);
    if (spv_pack_on()) {   // (a store made for it that no program took: let go with it)
        std::lock_guard lk(pack_mu);
        pack_made.erase(m);
    }
    vkDestroyShaderModule(dev.device, m, nullptr);
}
void program_module_stats(int64_t* modules, int64_t* module_bytes, int64_t* packed, int64_t* packed_bytes) {
    *modules = st_module_live.load(), *module_bytes = st_module_bytes.load();
    *packed = st_pack_live.load(), *packed_bytes = st_pack_live_bytes.load();
}
StageModule::StageModule(const Stage& st) {
    if (!st.code) {
        m_ = st.module;
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    bool made = false;
    m_ = st.code->pin([&made](const uint32_t* words, size_t n) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = n * 4;
        ci.pCode = words;
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(dev.device, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
        if (m) st_pack_modules++;
        made = m != VK_NULL_HANDLE;
        return m;
    });
    if (made) {   // ((Triforce) what a module made again costs the thread building the pipeline: the unpacking and the driver's part)
        const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        st_remake_ns += ns;
        for (int64_t was = st_remake_max_ns.load(); ns > was && !st_remake_max_ns.compare_exchange_weak(was, ns);) {
        }
    }
    if (m_) code_ = st.code;
    else {
        static std::atomic<int> logged{0};
        if (logged++ < 10) logf("vk: a packed stage's module could not be made again: its pipeline is not built");
    }
}
StageModule::~StageModule() {
    if (code_) code_->unpin([](VkShaderModule m) { vkDestroyShaderModule(dev.device, m, nullptr); });
}

bool shader_reads_exact(VkShaderModule module, uint32_t binding) {
    // (the last module asked about, a thread: draws that follow each other mostly share their program)
    static thread_local VkShaderModule last = VK_NULL_HANDLE;
    static thread_local spv_exact::Bindings last_bits{};
    static thread_local uint64_t last_gen = 0;
    const uint64_t gen = exact_gen.load(std::memory_order_acquire);   // (a new module may have taken an old one's handle)
    if (module != last || last_gen != gen) {
        std::lock_guard lk(exact_mu);
        const auto it = exact_of.find(module);
        last_bits = it == exact_of.end() ? spv_exact::Bindings{} : it->second;
        last = module, last_gen = gen;
    }
    return spv_exact::has(last_bits, binding);
}
// ---- (Triforce) Inside the translation stamp again ----

/// A word of a stage's constant bank: c0 / c2 the driver's banks, c1 its own constants, c3.. its uniform buffers
uint32_t stage_cbuf_word(const NvnState& s, const Stage& st, uint32_t index, uint32_t offset) {
    if (index == 1) {
        uint32_t v = 0;
        if ((size_t)offset + 4 <= st.c1.size()) std::memcpy(&v, st.c1.data() + offset, 4);
        return v;
    }
    if (index == 0 || index == 2) return driver_bank_word(s, (int)index, st.nvn_stage, offset);
    if (index >= 3 && index < 3 + 16 && st.nvn_stage >= 0 && st.nvn_stage < kNvnStages) {
        const nvn_p_bind_buf& u = s.ubo[st.nvn_stage][index - 3];
        uint32_t v = 0;
        // (as the stream's place sees it, not the game's memory now: that may be its next frames' already, and a
        // handle read from it sampled their textures, for one frame: MKW's shadows in bands. RX_VK_HANDLES_LIVE=1:
        // as before)
        static const bool live = getenv("RX_VK_HANDLES_LIVE") != nullptr;
        // (EvilOOT: and a binding of no recorded size read as far as asked, as the storage buffer descriptors had it before)
        if (u.addr && (!u.size || (uint64_t)offset + 4 <= (uint64_t)u.size)) {
            if (live) host->gpu_read(u.addr + offset, &v, 4);
            else ub_read(u.addr + offset, &v, 4);
        }
        return v;
    }
    return 0;
}

uint32_t stage_handle(const NvnState& s, const Stage& st, uint32_t program, uint32_t element) {
    if (program >= st.info.handle_programs.size()) return 0;
    return Shader::EvaluateHandleProgram(
               st.info.handle_programs[program],
               [&](u32 index, u32 offset) -> u32 { return stage_cbuf_word(s, st, index, offset); },
               [&](u32 binding, u32 offset) -> u32 {
                   if (binding >= st.info.storage_buffers_descriptors.size()) return 0U;
                   const auto& d = st.info.storage_buffers_descriptors[binding];
                   const uint64_t base = stage_cbuf_word(s, st, d.cbuf_index, d.cbuf_offset) |
                                         (uint64_t)stage_cbuf_word(s, st, d.cbuf_index, d.cbuf_offset + 4) << 32;
                   uint32_t v = 0;
                   static const bool live = getenv("RX_VK_HANDLES_LIVE") != nullptr;
                   // (hookshot #3047) a handle table's elements past the buffer's bound size: none (they named textures
                   // the shader never samples, and uploaded them)
                   const uint32_t size = stage_cbuf_word(s, st, d.cbuf_index, d.cbuf_offset + 8);
                   if (size && (uint64_t)offset + 4 > size) return 0U;
                   if (base) {
                       if (live) host->gpu_read(base + offset, &v, 4);
                       else ub_read(base + offset, &v, 4);
                   }
                   return v;
               },
               element)
        .value_or(0U);
}

namespace {

/// Whether the textures a variant was translated for still have its types (each texture descriptor's handle word)
/// The handle a texture descriptor reads (its primary word, ORed with the secondary one as Eden's TexturePair does;
/// a handle the shader reads from a storage buffer, its handle program's)
uint32_t descriptor_handle(const NvnState& s, const Stage& st, const Shader::TextureDescriptor& d) {
    if (d.handle_program != ~0U) return stage_handle(s, st, d.handle_program);
    uint32_t h = stage_cbuf_word(s, st, d.cbuf_index, d.cbuf_offset);
    if (d.has_secondary)
        h = h << d.shift_left | stage_cbuf_word(s, st, d.secondary_cbuf_index, d.secondary_cbuf_offset)
                                    << d.secondary_shift_left;
    return h;
}
/// (hookshot) a storage image descriptor's handle word (its handle program's, read from a storage buffer)
uint32_t descriptor_handle(const NvnState& s, const Stage& st, const Shader::ImageDescriptor& d) {
    if (d.handle_program != ~0U) return stage_handle(s, st, d.handle_program);
    return stage_cbuf_word(s, st, d.cbuf_index, d.cbuf_offset);
}
uint8_t texture_answer(uint32_t handle) {
    if (!handle) return 0xFF;
    // (by the texture slot, until a slot is registered anew: variant checks ask on every draw)
    static thread_local std::unordered_map<uint32_t, std::pair<uint32_t, uint8_t>> known;   // slot -> (version, answer)
    const uint32_t id = handle & 0xFFFFF;
    const uint32_t version = texture_slot_version((int)id);
    if (auto it = known.find(id); it != known.end() && it->second.first == version) return it->second.second;
    const uint8_t a = (uint8_t)((uint8_t)handle_texture_type(handle) | (handle_is_integer(handle) ? 0x80 : 0));
    known[id] = {version, a};
    return a;
}
/// What the bound textures answer now, for a program whose translation asked about them
// (hookshot) storage images too: whether the bound image's format is integer is baked into the translation (Eden's
// is_integer, the SPIR-V image's sampled type). OoT writes R11G11B10 and RGBA8 targets through R32UI views of them,
// packing the bits itself; a variant translated while that slot held a float texture wrote the packed words as
// floats, which the R32UI view converted: the scene went NaN and yellow before the TAAU
/// (hookshot) whether every answer names a stage of the program and a slot of that stage: answers come from the program
/// cache and the pipeline state list, files that are passed around, and index the stages' descriptor lists
bool answers_fit(const Program& p, const std::vector<Program::Answer>& answers) {
    for (const Program::Answer& a : answers) {
        if (a.stage >= p.stages.size()) return false;
        const auto& info = p.stages[a.stage].info;
        if (a.desc >= (a.image ? info.image_descriptors.size() : info.texture_descriptors.size())) return false;
    }
    return true;
}
uint32_t answer_handle(const NvnState& s, const Program& p, const Program::Answer& a) {
    const Stage& st = p.stages[a.stage];
    return a.image ? descriptor_handle(s, st, st.info.image_descriptors[a.desc])
                   : descriptor_handle(s, st, st.info.texture_descriptors[a.desc]);
}
uint8_t answer_of(uint32_t handle, bool image) {
    return image ? (uint8_t)(handle ? texture_answer(handle) & 0x80 : 0xFF) : texture_answer(handle);
}
// Only ignore an answer bit when the translated module already has the corresponding type.
// Keep the entries themselves: handle/version tracking and binding-stage masks still use them.
uint8_t canonical_answer(const Program& p, const Program::Answer& a, uint8_t type) {
    static const bool strict = [] {
        const char* capture = getenv("RX_VK_CAPTURE");
        return getenv("RX_VK_HANDLES_LIVE") || getenv("RX_VK_REGISTER_AT_ONCE") ||
               (capture && atoi(capture) == 0);
    }();
    if (strict) return type;
    const auto& info = p.stages[a.stage].info;
    if (a.image) {
        const auto& d = info.image_descriptors[a.desc];
        if (d.is_integer) {
            switch (d.format) {
            case Shader::ImageFormat::R8_UINT:
            case Shader::ImageFormat::R16_UINT:
            case Shader::ImageFormat::R32_UINT:
            case Shader::ImageFormat::R32G32_UINT:
            case Shader::ImageFormat::R32G32B32A32_UINT:
                return 0x80;
            default:
                break;
            }
        }
    } else {
        const auto& d = info.texture_descriptors[a.desc];
        // (the type is the instruction's own, texture_pass.cpp: whatever is bound there runs the same module through the same
        // view (vk_draw.cpp's view_type(d.type)), so its dimension is no answer. A rectangle and a buffer still are: the pass
        // rewrites an implicit 2D sample and a 1D fetch for them)
        const uint8_t dim = type & 0x7f;
        if (d.type_fixed && dim != (uint8_t)Shader::TextureType::Color2DRect && dim != (uint8_t)Shader::TextureType::Buffer)
            type = (uint8_t)((type & 0x80) | (uint8_t)d.type);
        if (d.is_depth && !d.is_integer) return type & 0x7f;
    }
    return type;
}

uint64_t answers_hash(const Program& p) {
    uint64_t h = 0;
    for (const Program::Answer& a : p.answers)
        h = (h ^ ((uint64_t)a.stage << 32 | (uint64_t)a.desc << 16 | (uint64_t)a.type << 1 | (uint64_t)a.image)) * 1099511628211ull;
    return h;
}
// (an unbound slot answers what the instruction itself says: nothing to tell the variants apart by, and a program prepared before
// the game, with nothing bound at all, comes out as the plain variant of each of its slots)
void record_answers(Program& p, const NvnState& s) {
    p.answers.clear();
    if (answers_in && answers_fit(p, *answers_in)) {   // (the pipeline state list's variant: the answers it was made with)
        p.answers = *answers_in;
        p.variant = answers_hash(p);
        return;
    }
    for (size_t k = 0; k < p.stages.size(); k++) {
        const auto& list = p.stages[k].info.texture_descriptors;
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].count == 1) {
                const uint32_t h = descriptor_handle(s, p.stages[k], list[i]);
                p.answers.push_back({(uint16_t)k, (uint16_t)i,
                                     h ? texture_answer(h) : (uint8_t)((uint8_t)list[i].type | (list[i].is_integer ? 0x80 : 0))});
            }
        const auto& images = p.stages[k].info.image_descriptors;
        for (size_t i = 0; i < images.size(); i++)
            if (images[i].count == 1) {
                const uint32_t h = descriptor_handle(s, p.stages[k], images[i]);
                p.answers.push_back({(uint16_t)k, (uint16_t)i, h ? answer_of(h, true) : (uint8_t)(images[i].is_integer ? 0x80 : 0), true});
            }
    }
    for (Program::Answer& a : p.answers) a.type = canonical_answer(p, a, a.type);
    p.variant = answers_hash(p);
}
/// Whether the textures bound now answer what the variant's translation was told (a variant whose translation
/// asked nothing stands for any, and an unbound slot takes any)
bool still_valid(const NvnState& s, const Program& p) {
    for (const Program::Answer& a : p.answers) {
        const uint32_t h = answer_handle(s, p, a);
        if (h && canonical_answer(p, a, answer_of(h, a.image)) != canonical_answer(p, a, a.type)) return false;
    }
    return true;
}

std::mutex shader_mu;
std::unordered_map<uint64_t, std::vector<std::unique_ptr<Program>>> programs;   // by the stages' code keys
std::unordered_map<uint64_t, std::vector<Code>> codes;                          // by program serial
/// The stages of s's program object read, those that are ok ((Triforce) heads where RX_VK_STAGE_CODE_LAZY: codes_whole lowers
/// them). No lock of the shaders' needed (codes_read lets it go)
std::vector<Code> stages_read(const NvnState& s) {
    std::vector<Code> v;
    for (uint32_t i = 0; i < s.prog.nstage && i < 6; i++) {
        Code c = read_code(s.prog.sd[i], true);
        if (c.ok) v.push_back(std::move(c));
    }
    return v;
}
const Shader::Profile& profile() {
    static const Shader::Profile p = make_profile();
    return p;
}

/// The profile of a stage pinned to 32 lanes (compute, and the mesh fallback's compute stages, pin_subgroup): the guest's warp
/// is the host's there, so the short path for warp ops, as before. The long one in every compute program made their SPIR-V new
/// and longer: the driver's cache had none of it, and AMD compiled the shaders for hours
const Shader::Profile& profile_pinned() {
    static const Shader::Profile p = [] {
        Shader::Profile q = profile();
        if (dev.sg32_stages & VK_SHADER_STAGE_COMPUTE_BIT) q.warp_size_potentially_larger_than_guest = false, q.max_subgroup_size = 32;
        return q;
    }();
    return p;
}

/// The profile of a graphics stage: one the GPU pins to 32 lanes as well (pin_subgroup: RDNA2's and RDNA3's fragment, task
/// and mesh stages, requiredSubgroupSizeStages 0xf0) keeps main's short path; only the ones it cannot pin (the vertex stage
/// there, every graphics stage on RDNA1's 0x20) take the long one. The fragment shuffle fallback stays in both: it is AMD's
/// compiler, not the lanes. RX_VK_WARP64 still sets every graphics stage
const Shader::Profile& profile_graphics(VkShaderStageFlags stage) {
    static const Shader::Profile pinned = [] {
        Shader::Profile q = profile();
        q.warp_size_potentially_larger_than_guest = false, q.max_subgroup_size = 32;
        return q;
    }();
    static const bool forced = getenv("RX_VK_WARP64") != nullptr;
    return !forced && (dev.sg32_stages & stage) ? pinned : profile();
}

const std::vector<Code>& codes_of(const NvnState& s) {
    auto& v = codes[s.prog.serial];
    if (v.empty()) v = stages_read(s);
    return v;
}

/// ((Triforce, shader_mu held; RX_VK_STAGE_CODE_LAZY) The stages of s's program object read as heads made whole (stage_whole),
/// each in its place in its list (codes_of): before a pick translates a variant of them, and for what names them. One that does
/// not lower stays in its place, not ok, so that a pick that has the lock let go keeps its pointers into the list
/// (program_read_once.h): the picks take the stages that are ok, and start again when one they took turns out not to be. A list
/// none of whose stages lowers is emptied, to be read again at the next pick, as such a list was before (no pick of it can have
/// the lock let go: a program none of whose stages lowers is in no cache)
void codes_whole(const NvnState& s) {
    const auto it = codes.find(s.prog.serial);
    if (it == codes.end()) return;
    std::vector<Code>& v = it->second;
    for (Code& c : v)
        if (!c.whole && c.bytes) {
            const auto bytes = c.bytes;
            c = stage_whole(bytes);
        }
    if (!v.empty() && std::none_of(v.begin(), v.end(), [](const Code& c) { return c.ok; })) v.clear();
}

std::atomic<int64_t> st_translated, st_failed, st_translate_ns;

} // namespace

Shader::AttributeType attribute_type_of(uint32_t vattr) {
    // (NVN vertex formats: the integer ones read as integers, everything else as float)
    const nvn_va_st a = *reinterpret_cast<const nvn_va_st*>(&vattr);
    switch (a.fmt) {
    case NVN_FORMAT_R8UI: case NVN_FORMAT_RG8UI: case NVN_FORMAT_RGB8UI: case NVN_FORMAT_RGBA8UI:
    case NVN_FORMAT_R16UI: case NVN_FORMAT_RG16UI: case NVN_FORMAT_RGB16UI: case NVN_FORMAT_RGBA16UI:
    case NVN_FORMAT_R32UI: case NVN_FORMAT_RG32UI: case NVN_FORMAT_RGB32UI: case NVN_FORMAT_RGBA32UI:
    case NVN_FORMAT_RGB10A2UI:
        return Shader::AttributeType::UnsignedInt;
    case NVN_FORMAT_R8I: case NVN_FORMAT_RG8I: case NVN_FORMAT_RGB8I: case NVN_FORMAT_RGBA8I: case NVN_FORMAT_R16I:
    case NVN_FORMAT_RG16I: case NVN_FORMAT_RGB16I: case NVN_FORMAT_RGBA16I: case NVN_FORMAT_R32I: case NVN_FORMAT_RG32I:
    case NVN_FORMAT_RGB32I: case NVN_FORMAT_RGBA32I: case NVN_FORMAT_RGB10A2I:
        return Shader::AttributeType::SignedInt;
    case NVN_FORMAT_RGB10A2_UI2F: return Shader::AttributeType::UnsignedScaled;
    case NVN_FORMAT_RGB10A2_I2F: return Shader::AttributeType::SignedScaled;
    default:
        if (a.fmt >= NVN_FORMAT_R8_UI2F && a.fmt <= NVN_FORMAT_RGBA32_I2F)
            return (a.fmt - NVN_FORMAT_R8_UI2F) & 1 ? Shader::AttributeType::SignedScaled : Shader::AttributeType::UnsignedScaled;
        return Shader::AttributeType::Float;
    }
}

namespace {
const Shader::ProgramHeader& header_of(const Code& c) {
    return *reinterpret_cast<const Shader::ProgramHeader*>(c.sph);
}
/// The colour output types a fragment program can tell apart: those of the targets it writes. They were all in the program's key,
/// and the same program drawn with another integer target it never writes was another program
void written_types(const Code* fs, const uint8_t color_types[8], uint8_t out[8]) {
    for (int i = 0; i < 8; i++) out[i] = fs && header_of(*fs).ps.HasOutputComponents(i) ? color_types[i] : 0;
}
/// A vertex and fragment program's key: their code, and the vertex input and colour output types that change what they translate to
uint64_t graphics_key(const Code& vs, const Code* fs, const uint32_t vattr[16], const uint8_t color_types[8]) {
    uint64_t key = vs.key ^ (fs ? fs->key * 31 : 0);
    for (int i = 0; i < 16; i++) {
        const auto used = header_of(vs).vtg.InputGeneric((size_t)i);
        const bool read = used[0] || used[1] || used[2] || used[3];
        key = key * 1099511628211ull ^ (uint64_t)(read ? attribute_type_of(vattr[i]) : Shader::AttributeType::Float) << i;
    }
    uint8_t written[8];
    written_types(fs, color_types, written);
    return hash_bytes(written, 8, key);
}
uint64_t mesh_key(const Code* task, const Code& mesh, const Code* fs, const Code* prim, const uint8_t color_types[8]) {
    uint64_t key = hash_bytes("mesh", 4, mesh.key);
    if (task) key = key * 1099511628211ull ^ task->key;
    if (fs) key = key * 1099511628211ull ^ fs->key;
    if (prim) key = hash_bytes(prim->sph, sizeof prim->sph, key);
    uint8_t written[8];
    written_types(fs, color_types, written);
    return hash_bytes(written, 8, key);
}
} // namespace

void log_program_stages(const NvnState& s, const char* why) {
    char line[1024];
    int n = snprintf(line, sizeof line, "vk: %s: program %llx, %u stages:", why, (unsigned long long)s.prog.serial,
                     s.prog.nstage);
    for (uint32_t i = 0; i < s.prog.nstage && i < 6 && n < (int)sizeof line - 32; i++) {
        uint8_t ctl[0x60]{};
        host->gpu_read(s.prog.sd[i].control, ctl, sizeof ctl);
        uint16_t st = 0;
        uint32_t magic = 0;
        std::memcpy(&magic, ctl, 4);
        std::memcpy(&st, ctl + 0x56, 2);
        const Code c = read_code(s.prog.sd[i]);
        uint32_t sph0 = 0;
        if (c.ok && !c.code.empty()) std::memcpy(&sph0, c.code.data(), 4);
        n += snprintf(line + n, sizeof line - n, " [%u: stage %u%s, code %#llx, sph type %u bits24-25 %u, isbe %d payload %u]",
                      i, st, magic == kControlMagic ? "" : " ?", (unsigned long long)s.prog.sd[i].data,
                      (sph0 >> 10) & 0xf, (sph0 >> 24) & 3, (int)c.isbe, c.payload_bytes);
    }
    logf("%s", line);
}

namespace {

/// (hookshot) A stage whose texture handles no handle program followed: they sample the first constant buffer word on their
/// path, which is rarely the texture (the water's lights in OoT: squares on the water, #2509). Once for each translation
void note_fallbacks(const Program& p, const char* stage, const Translation& t) {
    // (Triforce: a prepare translates 38000 programs: the first few of each kind are named, then every 200th with the count)
    static std::atomic<uint32_t> table_stages{0}, fallback_stages{0};
    if (t.tables) {
        const uint32_t n = ++table_stages;
        if (n <= 8 || n % 200 == 0)
            logf("vk: program %016llx, %s stage: %u texture handle(s) read from a table of records, one texture an element "
                 "(RX_VK_HANDLE_TABLE=0: one for all, as before); %u stages so far", (unsigned long long)p.key, stage, t.tables, n);
    }
    if (!t.fallbacks) return;
    const uint32_t n = ++fallback_stages;
    if (n <= 8 || n % 200 == 0)
        logf("vk: program %016llx, %s stage: %u texture handle(s) not followed per draw (stopped at %s): the first constant word "
             "on the path is sampled; %u stages so far", (unsigned long long)p.key, stage, t.fallbacks, t.fallback_ops.c_str(), n);
}

/// RX_VK_IR=<program key>[,...] (hex, a prefix is enough): the stages' Eden IR written to ir_<key>_<stage>.txt in the
/// working folder when the program is translated (diagnostics)
void dump_ir(const Program& p, const char* stage, const Shader::IR::Program& prog, uint64_t code_key = 0) {
    static const char* want = getenv("RX_VK_IR");
    if (!want) return;
    char key[24], alt[24];
    snprintf(key, sizeof key, "%016llx", (unsigned long long)p.key);
    snprintf(alt, sizeof alt, "%016llx", (unsigned long long)code_key);   // (hookshot: or the stage code's own key)
    bool match = false;
    for (const char* q = want; *q;) {
        const char* e = q;
        while (*e && *e != ',') e++;
        if (e > q && (!strncmp(key, q, (size_t)(e - q)) || (code_key && !strncmp(alt, q, (size_t)(e - q))))) match = true;
        q = *e ? e + 1 : e;
    }
    if (!match) return;
    char path[96];
    snprintf(path, sizeof path, "ir_%s_%s.txt", key, stage);
    if (FILE* f = fopen(path, "w")) {
        const std::string text = Shader::IR::DumpProgram(prog);
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
        logf("vk: IR of program %s, %s stage, written to %s", key, stage, path);
    }
}

/// Translates a program variant's stages into p (on a worker for graphics programs: s and the codes are copies)
// (hookshot) ir_opt/oot_visual_workaround.h, on the game's programs
void oot_visual(Shader::IR::Program& program, uint64_t key) {
    // (EvilOOT, 11-07: off unless RX_VK_OOT_VISUAL=1. With the foliage culling, the pointer reads and the first GPU work right, the
    // game's own fog is the soft haze with sun shafts of the console's picture at Kokiri's rock; the 0.08 factor was tuned against
    // the white wall those bugs made, and thinned every scene's fog)
    static const bool on = getenv("RX_VK_OOT_VISUAL") && atoi(getenv("RX_VK_OOT_VISUAL")) != 0;
    if (!on) return;
    static const float fog = getenv("RX_VK_OOT_FOG") ? (float)atof(getenv("RX_VK_OOT_FOG")) : 0.08f;   // (0.25, Orchid's, left a white forest once the game no longer lost its start)
    // (1: the game's own exposure. 0.65 was for the washed-out outdoors before shaders followed pointers; with the lighting right it
    // made scenes too dark, issue 103)
    static const float exposure = getenv("RX_VK_OOT_EXPOSURE") ? (float)atof(getenv("RX_VK_OOT_EXPOSURE")) : 1.0f;
    if (const size_t n = Shader::Optimization::ApplyOotVisualWorkaround(program, fog, exposure))
        logf("vk: OoT visual workaround: %s program %016llx, %zu edits", program.stage == Shader::Stage::Compute ? "fog" : "exposure",
             (unsigned long long)key, n);
}

/// (Evil Hookshot, diagnosis) RX_VK_FRAG_PROBE=<program key prefix>:attr<n>|tex<k>|flag: that program's fragment colour 0 replaced:
/// attr<n> its input generic n (x, y, z), tex<k> the k-th texture sample's rgb (one in the colour write's block only),
/// flag green where generic 0's bit 15 is set, red where not. To see on screen what an input holds (the cyan stems, 11-08).
/// With RX_VK_NO_PROGRAM_CACHE=1
void frag_probe(Shader::IR::Program& program, uint64_t key) {
    static const char* e = getenv("RX_VK_FRAG_PROBE");
    // (11-13) RX_VK_FRAG_SET=<program key prefix>:<n>=<value>[,<n>=<value>...]: the value numbered %n in the RX_VK_IR dump is
    // replaced by a constant wherever it is used, in any block (a light's term taken out to see what it carried). <value>: a
    // number (a U32 value takes the float's bits, a U1 true unless 0) or x<hex>, the bits themselves. With or without a probe
    static const char* set_e = getenv("RX_VK_FRAG_SET");
    if (!e && !set_e) return;
    char hex[24];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)key);
    const auto mine = [&](const char* v) -> const char* {
        const char* c = v ? strchr(v, ':') : nullptr;
        return c && !strncmp(hex, v, (size_t)(c - v)) ? c : nullptr;
    };
    const char* set_colon = mine(set_e);
    const char* colon = mine(e);
    if (!colon && !set_colon) return;
    if (!colon) colon = ":";   // (no probe for this program: the sets alone)
    using namespace Shader::IR;
    int n = -1;
    const bool attr = sscanf(colon + 1, "attr%d", &n) == 1 && n >= 0 && n < 32;
    const bool flag = !strcmp(colon + 1, "flag");
    // (v<a>,<b>,<c>: the values numbered %a %b %c in the RX_VK_IR dump (its numbering: blocks first, then each instruction and its
    // arguments as met), saturated, as red, green, blue; one number: grey. Values whose block dominates the colour write's only)
    int vn[3] = {-1, -1, -1};
    const int nv = colon[1] == 'v' ? sscanf(colon + 2, "%d,%d,%d", &vn[0], &vn[1], &vn[2]) : 0;
    if (!attr && !flag && nv <= 0 && !set_colon) return;
    // (which blocks dominate which: a value may stand in for the colour only where every path to the write passes its block)
    std::unordered_map<const Block*, size_t> block_index;
    for (size_t i = 0; i < program.blocks.size(); i++) block_index[program.blocks[i]] = i;
    const size_t nb = program.blocks.size();
    std::vector<std::vector<bool>> dom(nb, std::vector<bool>(nb, true));
    if (nv > 0 && nb) {
        dom[0].assign(nb, false), dom[0][0] = true;
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t i = 1; i < nb; i++) {
                std::vector<bool> d(nb, true);
                bool any = false;
                for (const Block* pred : program.blocks[i]->ImmPredecessors()) {
                    const auto f = block_index.find(pred);
                    if (f == block_index.end()) continue;
                    any = true;
                    for (size_t k = 0; k < nb; k++) d[k] = d[k] && dom[f->second][k];
                }
                if (!any) d.assign(nb, false);
                d[i] = true;
                if (d != dom[i]) dom[i] = d, changed = true;
            }
        }
    }
    const auto dominates = [&](const Block* a, const Block* b) {
        const auto fa = block_index.find(a), fb = block_index.find(b);
        return fa != block_index.end() && fb != block_index.end() && dom[fb->second][fa->second];
    };
    std::map<size_t, Inst*> by_index;
    std::unordered_map<const Inst*, Block*> block_of;
    if (nv > 0 || set_colon) {
        size_t index = program.blocks.size();
        std::map<const Inst*, size_t> seen;
        const auto number = [&](const Inst* i) {
            if (seen.emplace(i, index + 1).second) index++, by_index[index] = const_cast<Inst*>(i);
        };
        for (Block* block : program.blocks)
            for (Inst& inst : block->Instructions()) {
                block_of[&inst] = block;
                if (TypeOf(inst.GetOpcode()) != Type::Void) number(&inst);
                for (size_t a = 0; a < inst.NumArgs(); a++) {
                    const Value arg = inst.Arg(a);
                    if (!arg.IsEmpty() && (!arg.IsImmediate() || arg.IsIdentity())) number(arg.Inst());
                }
            }
    }
    // (what the numbers name, said in the log: the dump the numbers were read from may be of another translation)
    const auto named = [&](int want) {
        const auto f = by_index.find((size_t)want);
        if (f == by_index.end()) return std::string("nothing");
        const auto b = block_index.find(block_of[f->second]);
        return std::string(NameOf(f->second->GetOpcode())) + " in block " + std::to_string(b == block_index.end() ? -1 : (long long)b->second);
    };
    for (int i = 0; i < nv; i++) logf("vk: probe: %%%d is %s", vn[i], named(vn[i]).c_str());
    if (set_colon) {
        // (the instructions first, then the changes: a replaced value's users no longer meet it, and the numbers after it move)
        std::vector<std::pair<Inst*, std::string>> sets;
        for (const char* q = set_colon + 1; *q;) {
            const char* end = q;
            while (*end && *end != ',') end++;
            int want = -1;
            char text[40] = "";
            if (sscanf(q, "%d=%39[^,]", &want, text) == 2) {
                const auto f = by_index.find((size_t)want);
                logf("vk: set: %%%d (%s) = %s%s", want, named(want).c_str(), text, f == by_index.end() ? ": left out" : "");
                if (f != by_index.end()) sets.push_back({f->second, text});
            }
            q = *end ? end + 1 : end;
        }
        for (const auto& [inst, text] : sets) {
            if (!strncmp(text.c_str(), "sub", 3)) {
                // (<n>=sub<m>: %n's users get %m - %n, two F32 values of one block, %m first: a coordinate mirrored where it is used)
                const auto fm = by_index.find((size_t)atoi(text.c_str() + 3));
                Block* const b = block_of[inst];
                if (fm == by_index.end() || !b || inst->Type() != Type::F32 || fm->second->Type() != Type::F32) {
                    logf("vk: set: sub needs two F32 values: left out");
                    continue;
                }
                // (the users first: the two instructions made below read %n themselves and must go on reading it)
                std::vector<std::pair<Inst*, size_t>> before;
                for (Block* other : program.blocks)
                    for (Inst& user : other->Instructions())
                        for (size_t a = 0; a < user.NumArgs(); a++) {
                            const Value arg = user.Arg(a);
                            if (arg.IsEmpty() || (arg.IsImmediate() && !arg.IsIdentity())) continue;
                            if (arg.Inst() == inst) before.push_back({&user, a});
                        }
                Inst* made = nullptr;
                for (auto it = b->Instructions().begin(); it != b->Instructions().end(); ++it)
                    if (&*it == inst) {
                        IREmitter ir{*b, std::next(it)};
                        made = F32{ir.FPAdd(F32{Value{fm->second}}, F32{ir.FPNeg(F32{Value{inst}})})}.Inst();
                        break;
                    }
                int users = 0;
                if (made)
                    for (const auto& [user, a] : before) user->SetArg(a, Value{made}), users++;
                logf("vk: set: %d users now read the mirrored value", users);
                continue;
            }
            const bool raw = text[0] == 'x';
            const float fv = raw ? 0.0f : (float)atof(text.c_str());
            uint32_t bits = raw ? (uint32_t)strtoul(text.c_str() + 1, nullptr, 16) : 0;
            if (!raw) memcpy(&bits, &fv, 4);
            float as_float;
            memcpy(&as_float, &bits, 4);
            const Type t = inst->Type();
            if (t == Type::F32) inst->ReplaceUsesWith(Value{as_float});
            else if (t == Type::U32) inst->ReplaceUsesWith(Value{bits});
            else if (t == Type::U1) inst->ReplaceUsesWith(Value{bits != 0});
            else logf("vk: set: a value of that type is not replaced");
        }
    }
    if (!attr && !flag && nv <= 0) return;
    // (RX_VK_FRAG_PROBE_SCALE=<s>: the probed values times s before they are shown, small ones made visible)
    static const float probe_scale = getenv("RX_VK_FRAG_PROBE_SCALE") ? (float)atof(getenv("RX_VK_FRAG_PROBE_SCALE")) : 1.0f;
    int edits = 0;
    for (Block* block : program.blocks)
        for (auto it = block->Instructions().begin(); it != block->Instructions().end(); ++it) {
            if (it->GetOpcode() != Opcode::SetFragColor || it->Arg(0).U32() != 0) continue;
            const u32 comp = it->Arg(1).U32();
            IREmitter ir{*block, it};
            F32 v = ir.Imm32(1.0f);
            if (attr && comp < 3)
                v = ir.GetAttribute(static_cast<Attribute>(static_cast<size_t>(Attribute::Generic0X) + (size_t)n * 4 + comp));
            if (nv > 0 && comp < 3) {
                const int want = vn[nv == 1 ? 0 : std::min<int>((int)comp, nv - 1)];
                const auto f = by_index.find((size_t)want);
                Block* vb = f != by_index.end() ? block_of[f->second] : nullptr;
                if (!vb || !dominates(vb, block)) {
                    logf("vk: probe: %%%d is not in the colour write's block or the first: left out", want);
                } else {
                    Value val{f->second};
                    const Type t = f->second->Type();
                    F32 x = t == Type::F32 ? F32{val}
                          : t == Type::U32 && getenv("RX_VK_FRAG_PROBE_INT")   // (=<n>: the integer / n, an index's size)
                              ? F32{ir.FPMul(F32{ir.ConvertUToF(32, 32, U32{val})}, ir.Imm32(1.0f / (float)std::max(1, atoi(getenv("RX_VK_FRAG_PROBE_INT")))))}
                          : t == Type::U32 ? ir.BitCast<F32, U32>(U32{val})
                          : t == Type::U1  ? F32{ir.Select(U1{val}, ir.Imm32(1.0f), ir.Imm32(0.0f))}
                                           : ir.Imm32(0.0f);
                    // (RX_VK_FRAG_PROBE_NEG=1: minus the value, to see negative ones)
                    static const bool neg = getenv("RX_VK_FRAG_PROBE_NEG") != nullptr;
                    if (neg) x = F32{ir.FPNeg(x)};
                    if (probe_scale != 1.0f) x = F32{ir.FPMul(x, ir.Imm32(probe_scale))};
                    // (RX_VK_FRAG_PROBE_NAN=1: white where the value is NaN or beyond +-1e30, black elsewhere)
                    static const bool nan = getenv("RX_VK_FRAG_PROBE_NAN") != nullptr;
                    if (nan) {
                        const U1 bad = ir.LogicalOr(ir.FPIsNan(x), ir.FPGreaterThan(F32{ir.FPAbs(x)}, ir.Imm32(1e30f)));
                        x = F32{ir.Select(bad, ir.Imm32(1.0f), ir.Imm32(0.0f))};
                    }
                    // (RX_VK_FRAG_PROBE_SIGNED=<scale>, one value: red its positive part x scale, green its negative part, blue its
                    // size / 100: sign and magnitude both readable)
                    static const float sgn = getenv("RX_VK_FRAG_PROBE_SIGNED") ? (float)atof(getenv("RX_VK_FRAG_PROBE_SIGNED")) : 0.0f;
                    if (sgn != 0.0f) {
                        const F32 scaled = F32{ir.FPMul(x, ir.Imm32(sgn))};
                        x = comp == 0 ? scaled : comp == 1 ? F32{ir.FPNeg(scaled)} : F32{ir.FPMul(F32{ir.FPAbs(x)}, ir.Imm32(0.01f))};
                    }
                    v = F32{ir.FPSaturate(x)};
                }
            }
            if (flag && comp < 3) {
                const U32 bits = ir.BitCast<U32>(ir.GetAttribute(Attribute::Generic0X));
                const U1 set = ir.INotEqual(ir.BitwiseAnd(bits, ir.Imm32(0x8000u)), ir.Imm32(0u));
                v = comp == 0 ? F32{ir.Select(set, ir.Imm32(0.0f), ir.Imm32(1.0f))}
                              : comp == 1 ? F32{ir.Select(set, ir.Imm32(1.0f), ir.Imm32(0.0f))} : ir.Imm32(0.0f);
            }
            it->SetArg(2, v);
            edits++;
        }
    logf("vk: probe: program %s fragment colour 0 = %s (%d writes)", hex, colon + 1, edits);
}

// (hookshot) RX_VK_MESH_NOCULL=<task shader code key prefixes,...>, off unless set. OoT's meshlet
// culling task shader walks every cell of a coarse depth grid its meshlet covers, reading four words a cell from c3 by a
// dynamic index: on RDNA 2 one draw runs past the kernel's 10 s and the GPU is reset (Hookshot issue 817). Its answer is
// visible = (result & 1) | (c3[2320] & 4), bit 4 being the game's own "draw everything". That comparison becomes true,
// and dead code elimination takes the grid walk with it: every meshlet is drawn (the rasterizer clips what is off
// screen), none is left out; RX_VK_SKIP_CODE left these programs out instead, and their models with them. The Steam
// Deck plays with RX_VK_MESH_NOCULL=2c56dc1006942f74 (issue 817). A name under RX_VK_MESH, so the program cache starts
// again when it changes.
bool mesh_nocull_wanted(uint64_t code_key) {
    static const std::string list = getenv("RX_VK_MESH_NOCULL") ? getenv("RX_VK_MESH_NOCULL") : "";
    if (list.empty() || list == "0") return false;
    char hex[24];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)code_key);
    for (size_t at = 0; at < list.size();) {
        const size_t e = list.find(',', at);
        const std::string k = list.substr(at, e == std::string::npos ? std::string::npos : e - at);
        if (!k.empty() && !strncmp(hex, k.c_str(), k.size())) return true;
        at = e == std::string::npos ? list.size() : e + 1;
    }
    return false;
}

bool reads_c3_2320_bit4(const Shader::IR::Value& v) {   // (BitwiseAnd32 of #4 and GetCbufU32 #3, #2320, through Identity)
    using namespace Shader::IR;
    const Value r = v.Resolve();
    if (r.IsImmediate()) return false;
    const Inst* and_ = r.InstRecursive();
    if (and_->GetOpcode() != Opcode::BitwiseAnd32) return false;
    bool four = false, cbuf = false;
    for (size_t i = 0; i < 2; i++) {
        const Value a = and_->Arg(i).Resolve();
        if (a.IsImmediate()) {
            four = four || a.U32() == 4;
            continue;
        }
        const Inst* c = a.InstRecursive();
        if (c->GetOpcode() != Opcode::GetCbufU32) continue;
        const Value bank = c->Arg(0).Resolve(), off = c->Arg(1).Resolve();
        cbuf = cbuf || (bank.IsImmediate() && off.IsImmediate() && bank.U32() == 3 && off.U32() == 2320);
    }
    return four && cbuf;
}

void mesh_nocull(Shader::IR::Program& program, uint64_t code_key) {
    using namespace Shader::IR;
    if (!mesh_nocull_wanted(code_key)) return;
    int made = 0;
    for (Block* block : program.blocks)
        for (Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != Opcode::INotEqual) continue;
            const Value zero = inst.Arg(1).Resolve();
            if (!zero.IsImmediate() || zero.U32() != 0) continue;
            const Value orv = inst.Arg(0).Resolve();
            if (orv.IsImmediate()) continue;
            const Inst* o = orv.InstRecursive();
            if (o->GetOpcode() != Opcode::BitwiseOr32) continue;
            if (!reads_c3_2320_bit4(o->Arg(0)) && !reads_c3_2320_bit4(o->Arg(1))) continue;
            inst.ReplaceUsesWith(Value{true});
            made++;
        }
    if (made) Shader::Optimization::DeadCodeEliminationPass(program);
    static std::atomic<int> told{0};
    if (told++ < 8)
        logf("vk: task code %016llx: culling %s (RX_VK_MESH_NOCULL)", (unsigned long long)code_key,
             made ? "off, every meshlet drawn" : "pattern not found, left as it is");
}

// ---- (hookshot) The program cache, triforce_programs.bin beside the executable: what translation made, by the key of the
// stages' code and inputs. A program met for the first time stopped the frame for its translation, 9 s of it each run,
// the long stutters when walking somewhere (replay workers in jobs_wait for up to 150 ms). The pipelines themselves were
// already kept (mkw_vk_pipelines.bin). Written as made; good for these switches and this translator only (a file made by another
// starts again). (Triforce) By what of the translator translates: a build that changed none of it carries the file over
// (HOOKSHOT_TRANSLATION_STAMP; program_carry.h has the rule; RX_VK_PROGRAM_CARRY=0: a file for one build of the translator, as
// before). RX_VK_NO_PROGRAM_CACHE=1: off ----
// ---- (Triforce) Outside the translation stamp, to the next mark (nx2/build.py translation_text; the rule: program_carry.h): the
// program cache's file and its list of records, where the records are, not what they hold ----
struct DiskRecord { int64_t at; size_t size; uint32_t kind; };
std::mutex disk_mu;
// (read as wanted: with every program of the game in it the file is gigabytes, and it was read into memory whole)
FILE* disk_in = nullptr;
std::string disk_path;   // (triforce_programs.bin: the precompile opens it once a thread)
std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_records;
FILE* disk_out = nullptr;
// (the records' heads, in the file's order, in triforce_programs.idx beside it: see disk_init)
FILE* disk_idx = nullptr;
bool disk_listing = false;
bool disk_on = false;
std::atomic<int64_t> st_cache_hits{0};
constexpr uint32_t kDiskMagic = 0x31505348;
constexpr uint32_t kIdxMagic = 0x58495048;   // ("HPIX", then a version and the cache's stamp; 20 bytes a record: key, size, kind)
constexpr uint32_t kIdxVersion = 1;
constexpr size_t kIdxEntry = 20;
// ((Triforce) after a start that threw the cache away, its keys as its index named them and how many records it had, and the
// programs translated since whose key it had and those it had not: the log's, dropped_note. disk_mu)
std::unordered_set<uint64_t> disk_dropped;
size_t disk_dropped_records = 0;
int64_t st_again = 0, st_first = 0;

void idx_put(std::vector<uint8_t>& o, uint64_t key, uint64_t size, uint32_t kind) {
    uint8_t e[kIdxEntry];
    std::memcpy(e, &key, 8), std::memcpy(e + 8, &size, 8), std::memcpy(e + 16, &kind, 4);
    o.insert(o.end(), e, e + kIdxEntry);
}
/// (disk_mu held, or at the start) the index made again whole, for these records in the file's order, and left open to add to
void idx_write(const std::string& path, uint64_t stamp, const std::vector<std::pair<uint64_t, DiskRecord>>& order) {
    if (disk_idx) fclose(disk_idx), disk_idx = nullptr;
    std::vector<uint8_t> o;
    o.reserve(16 + order.size() * kIdxEntry);
    const uint32_t h[2] = {kIdxMagic, kIdxVersion};
    o.insert(o.end(), (const uint8_t*)h, (const uint8_t*)h + 8);
    o.insert(o.end(), (const uint8_t*)&stamp, (const uint8_t*)&stamp + 8);
    for (const auto& [key, d] : order) idx_put(o, key, d.size, d.kind);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    const bool ok = fwrite(o.data(), 1, o.size(), f) == o.size() && fflush(f) == 0;
    fclose(f);
    if (ok) disk_idx = fopen(path.c_str(), "ab");
    else remove(path.c_str());   // (a short one would only be read as far as it goes, but the next start makes it again anyway)
}
/// ((Triforce) disk_mu held) A program just kept (program_cache_save): after a start that threw a cache of other translations
/// away (disk_init), counted as translated again (that cache had its key) or not, for the log now and then
void dropped_note(uint64_t key) {
    if (disk_dropped.empty()) return;
    (disk_dropped.count(key) ? st_again : st_first)++;
    if (st_again + st_first == 1 || (st_again + st_first) % 4096 == 0)
        logf("vk: program cache: %lld programs translated again since the translation changed (of keys the cache before had: %zu "
             "programs of %zu keys), %lld of keys it had not", (long long)st_again, disk_dropped_records, disk_dropped.size(),
             (long long)st_first);
}
// ---- (Triforce) Inside the translation stamp again ----

// (Triforce) The translation stamp (nx2/build.py translation_stamp): 0 in a build made without it, which then keeps a file for
// one build of the translator, as before
#ifdef HOOKSHOT_TRANSLATION_STAMP
constexpr uint64_t kTranslationStamp = HOOKSHOT_TRANSLATION_STAMP;
#else
constexpr uint64_t kTranslationStamp = 0;
#endif
/// (Triforce) Whether the program cache is carried over an update by the translation stamp (RX_VK_PROGRAM_CARRY, on unless 0)
bool carry_on() {
    static const bool on = kTranslationStamp != 0 && program_carry::on(getenv("RX_VK_PROGRAM_CARRY"));
    return on;
}

uint64_t disk_stamp() {
    uint64_t h = 0xCBF29CE484222325ull;
    const auto add = [&h](const void* d, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)d)[i]) * 1099511628211ull;
    };
    // (what translates shaders, nx2/build.py: a rebuild of anything else keeps the cache. It was the DLL's bytes, and every build
    // lost it. (Triforce) With the carry, what of it translates: HOOKSHOT_TRANSLATION_STAMP, program_carry.h)
    uint64_t translator[2];
    program_carry::translator_words(carry_on(), kTranslationStamp, HOOKSHOT_SHADER_STAMP, translator);
    add(translator, sizeof translator);
    const uint32_t depth = host->depth_minus_w() != 0;   // (d3zessete's PR 54: programs made in either depth mode are kept apart)
    add(&depth, sizeof depth);
    // (and a GPU's mesh shaders: a file of native task and mesh programs is no use to the compute fallback, nor its to one)
    const uint32_t mesh = (dev.mesh_shader ? 1u : 0u) | (dev.mesh_fallback ? 2u : 0u) | (dev.task_mesh_fallback ? 4u : 0u);
    add(&mesh, sizeof mesh);
    // (and what of the GPU the translator is told (make_profile): its warps' lanes and the driver's shuffle workaround, and
    // which stages it pins to 32 lanes, which keep the short path: profile_graphics. An explicit warp override bypasses
    // the pinned graphics profile even if the base profile already has the requested width)
    const uint32_t lanes[] = {profile().warp_size_potentially_larger_than_guest ? 1u : 0u, profile().max_subgroup_size,
                              profile().has_broken_spirv_fragment_shuffle ? 1u : 0u, (uint32_t)dev.sg32_stages,
                              getenv("RX_VK_WARP64") ? 1u : 0u,
                              // (and where the translation starts a storage binding: offsets are counted from there, so a cache
                              // made on a 16-aligned GPU is no use on a 64-aligned one in the same folder, a laptop's two GPUs)
                              (uint32_t)storage_binding_align()};
    add(lanes, sizeof lanes);
    // (the records keep Shader::Info as it lies in memory: a cache written by another C++ library (the MSVC build's, then the
    // llvm-mingw one's libc++) has other layouts, and reading it crashed at start. The library and the sizes, so it starts again)
    const uint32_t abi[] = {(uint32_t)sizeof(Shader::Info), (uint32_t)sizeof(Program), (uint32_t)sizeof(std::vector<uint32_t>),
#if defined(_LIBCPP_VERSION)
                            1u, (uint32_t)_LIBCPP_VERSION
#elif defined(_MSVC_STL_VERSION)
                            2u, (uint32_t)_MSVC_STL_VERSION
#else
                            3u, 0u
#endif
    };
    add(abi, sizeof abi);
    {   // (the translation's per-GPU settings, set by coopmat_init at device creation: AMD/Intel's invariant positions, the colour
        // NaN guard, the matrix layout measured on this device. Without them a cache made on one GPU was used on another (a laptop's
        // integrated and dedicated GPUs), and RX_VK_NO_INVARIANT / RX_VK_NO_NAN_GUARD did nothing for programs already cached)
        using namespace Shader::Backend::SPIRV;
        // (and the vendor fixes of the translation, vk_vendor.h: out-of-bounds fetches, NaN tests, the gather offset)
        const uint32_t flags = (position_invariant ? 1u : 0u) | (frag_nan_guard ? 2u : 0u) | (mma_software ? 4u : 0u) |
                               (fetch_bounds ? 16u : 0u) | (nan_bits ? 32u : 0u) |
                               (profile().need_gather_subpixel_offset ? 64u : 0u) | (fetch_bounds_buffers ? 128u : 0u);
        add(&flags, sizeof flags);
        add(mma_order_a.data(), sizeof(uint32_t) * mma_order_a.size());
        add(mma_order_b.data(), sizeof(uint32_t) * mma_order_b.size());
        add(mma_order_c.data(), sizeof(uint32_t) * mma_order_c.size());
    }
    // (RX_VK_GP_, not RX_VK_GP: that took RX_VK_GPL with it, which changes no translation, and switching the stage libraries on
    // threw the whole program cache away. What the libraries are good for is the prepare's second stamp, vk_draw.cpp warmed_stamp)
    for (char** e = _environ; e && *e; e++)
        for (const char* pre : {"RX_VK_OOT", "RX_VK_MMA", "RX_VK_NATIVE", "RX_VK_NAN", "RX_VK_TASK_CAP", "RX_VK_NETWORK", "RX_VK_GP_", "RX_VK_FOLLOW", "RX_VK_NO_FOLLOW", "RX_VK_NO_F16", "RX_VK_SHARED", "RX_VK_MESH", "RX_VK_NO_INVARIANT", "RX_VK_NO_NAN_GUARD", "RX_VK_MARKERS", "RX_VK_HANDLE_TABLE", "RX_VK_DLSS_CAPTURE", "EDEN_"})
            if (!strncmp(*e, pre, strlen(pre))) add(*e, strlen(*e));
    return h;
}

// ---- (Triforce) Outside the translation stamp, to the next mark (nx2/build.py translation_text; the rule: program_carry.h): the
// program cache opened, its records listed when it was made for this stamp ----
void disk_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (getenv("RX_VK_NO_PROGRAM_CACHE")) return;
        char exe[MAX_PATH];
        const DWORD n = game_module_path(exe, MAX_PATH);
        std::string path(exe, n);
        path = path.substr(0, path.find_last_of("\\/") + 1) + "triforce_programs.bin";
        // (one Triforce at a time on a cache: a second one, started while the first wrote, took the record being written for one
        // cut short and cut the file there, under the first, whose list of records then named bytes written over since. Held
        // until the process ends)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (const char ch : path) h = (h ^ (uint8_t)tolower((unsigned char)ch)) * 1099511628211ull;
            char name[64];
            snprintf(name, sizeof name, "Local\\TriforceProgramCache_%016llx", (unsigned long long)h);
#ifdef _WIN32
            const HANDLE held = CreateMutexA(nullptr, FALSE, name);
            const bool taken = !held || GetLastError() == ERROR_ALREADY_EXISTS;
#else   // (linux: a lock on a file beside the cache, let go when the process ends)
            const int fd = open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
            const bool taken = fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0;
#endif
            if (taken) {
                logf("vk: program cache %s: another Triforce is using it: this one runs without it", path.c_str());
                return;
            }
        }
        const uint64_t stamp = disk_stamp();
        // ((Triforce) the file's second word: the build of the translator that opened it last. A file made for this stamp by
        // another build is carried over the update: its translations are the ones this build makes, program_carry.h)
        const uint32_t writer = program_carry::writer(HOOKSHOT_SHADER_STAMP);
        program_carry::Verdict verdict = program_carry::Verdict::Fresh;
        uint32_t had_writer = 0;
        uint64_t had_stamp = 0;
        // (triforce_programs.idx: the records' heads, 20 bytes each, in the file's order. The records follow one another in the
        // file, so finding them was a read of each one's head, 22000 of them over the 4 GB: seconds at every start, each a read
        // somewhere else on the disk (and on a compressed file system a block of 128 KB unpacked for those 20 bytes). The index
        // is taken when its stamp is the cache's, its sizes fit in the file and the last record's head in the file is the one
        // it names; the records past it (written by a build without the index, or after a run that ended between the two
        // writes) are read as before. Anything else: all of them read as before, and the index written again)
        const std::string idx_path = path.substr(0, path.size() - 4) + ".idx";
        std::vector<std::pair<uint64_t, DiskRecord>> order;   // (every record, in the file's order: for the index)
        bool idx_whole = false;   // (the index named every record, and nothing more)
        int64_t good = 0, whole = 0;
        if (FILE* f = fopen(path.c_str(), "rb")) {
            _fseeki64(f, 0, SEEK_END);
            whole = _ftelli64(f);
            _fseeki64(f, 0, SEEK_SET);
            uint32_t head[2]{};
            uint64_t kept = 0;
            const bool read = whole >= 16 && fread(head, 4, 2, f) == 2 && fread(&kept, 8, 1, f) == 1;
            verdict = program_carry::verdict(read, head[0], head[1], kept, kDiskMagic, stamp, writer);
            had_writer = head[1], had_stamp = kept;
            if (verdict == program_carry::Verdict::Kept || verdict == program_carry::Verdict::Carried) {   // (kept == stamp)
                good = 16;
                size_t idx_n = 0;
                bool idx_short = false;   // (it named records the file does not have whole)
                if (FILE* x = fopen(idx_path.c_str(), "rb")) {
                    std::vector<uint8_t> idx;
                    _fseeki64(x, 0, SEEK_END);
                    const int64_t n = _ftelli64(x);
                    _fseeki64(x, 0, SEEK_SET);
                    if (n >= 16) {
                        idx.resize((size_t)n);
                        if (fread(idx.data(), 1, idx.size(), x) != idx.size()) idx.clear();
                    }
                    fclose(x);
                    uint32_t ih[2]{};
                    uint64_t is = 0;
                    if (idx.size() >= 16) std::memcpy(ih, idx.data(), 8), std::memcpy(&is, idx.data() + 8, 8);
                    if (ih[0] == kIdxMagic && ih[1] == kIdxVersion && is == stamp) {
                        const size_t entries = (idx.size() - 16) / kIdxEntry;
                        int64_t at = 16;
                        for (size_t i = 0; i < entries; i++) {
                            const uint8_t* e = idx.data() + 16 + i * kIdxEntry;
                            uint64_t key, size;
                            uint32_t kind;
                            std::memcpy(&key, e, 8), std::memcpy(&size, e + 8, 8), std::memcpy(&kind, e + 16, 4);
                            if (size > (uint64_t)whole || at + 16 + (int64_t)size > whole) {
                                idx_short = true;
                                break;
                            }
                            order.push_back({key, {at + 16, (size_t)size, kind}});
                            at += 16 + (int64_t)size;
                        }
                        // (the last one's head as the file has it: an index of another file, or of this one before it was cut
                        // or made again, is not taken)
                        bool same = !order.empty();
                        if (same) {
                            const auto& [key, d] = order.back();
                            uint64_t rec[2]{};
                            uint32_t kind = 0;
                            _fseeki64(f, d.at - 16, SEEK_SET);
                            same = fread(rec, 8, 2, f) == 2 && fread(&kind, 4, 1, f) == 1 && rec[0] == key && rec[1] == d.size &&
                                   kind == d.kind;
                        }
                        if (same) {
                            idx_n = order.size();
                            good = at;
                            idx_whole = !idx_short && idx.size() == 16 + entries * kIdxEntry;
                        } else {
                            order.clear();
                        }
                    }
                }
                size_t scanned = 0;
                while (good + 20 <= whole) {
                    uint64_t rec[2];
                    uint32_t kind = 0;
                    _fseeki64(f, good, SEEK_SET);
                    if (fread(rec, 8, 2, f) != 2 || fread(&kind, 4, 1, f) != 1) break;
                    if (rec[1] > (uint64_t)(whole - good - 16)) break;   // (the run stopped while writing it; unsigned: no size wraps)
                    order.push_back({rec[0], {good + 16, (size_t)rec[1], kind}});
                    good += 16 + (int64_t)rec[1];
                    scanned++;
                }
                if (scanned) idx_whole = false;
                for (const auto& [key, d] : order) disk_records[key].push_back(d);
                if (idx_n && !idx_whole)
                    logf("vk: program cache index: %zu records from it, %zu more read from the cache", idx_n, scanned);
            }
            fclose(f);
        }
        // ((Triforce) a cache made for another stamp: its keys, as its own index names them, before the index is made again, for
        // the log's "translated again")
        if (verdict == program_carry::Verdict::Dropped) {
            if (FILE* x = fopen(idx_path.c_str(), "rb")) {
                uint32_t ih[2]{};
                uint64_t is = 0;
                uint8_t e[kIdxEntry];
                if (fread(ih, 4, 2, x) == 2 && fread(&is, 8, 1, x) == 1 && ih[0] == kIdxMagic && ih[1] == kIdxVersion && is == had_stamp)
                    while (fread(e, 1, kIdxEntry, x) == kIdxEntry) {
                        uint64_t key;
                        std::memcpy(&key, e, 8);
                        disk_dropped.insert(key), disk_dropped_records++;
                    }
                fclose(x);
            }
        }
        if (good) {
            if (good != whole) {   // (a record cut short: without it)
                if (FILE* f = fopen(path.c_str(), "r+b")) _chsize_s(_fileno(f), good), fclose(f);
            }
            // ((Triforce) carried: its head names this build now, and the next start keeps it)
            if (verdict == program_carry::Verdict::Carried) {
                if (FILE* f = fopen(path.c_str(), "r+b")) {
                    _fseeki64(f, 4, SEEK_SET);
                    fwrite(&writer, 4, 1, f);
                    fclose(f);
                }
            }
            disk_out = fopen(path.c_str(), "ab");
        } else {
            disk_out = fopen(path.c_str(), "wb");
            if (disk_out) {
                const uint32_t h[2] = {kDiskMagic, writer};
                fwrite(h, 4, 2, disk_out);
                fwrite(&stamp, 8, 1, disk_out);
                fflush(disk_out);
            }
            order.clear();
        }
        if (disk_out) {
            if (idx_whole) disk_idx = fopen(idx_path.c_str(), "ab");
            else idx_write(idx_path, stamp, order);
        }
        disk_in = disk_out ? fopen(path.c_str(), "rb") : nullptr;
        disk_path = path;
        disk_on = disk_out != nullptr && disk_in != nullptr;
        if (verdict == program_carry::Verdict::Carried) {
            logf("vk: program cache %s: %zu program keys kept, %lld KB: their %zu programs carried over an update of the translator "
                 "(made by its build %08x, this one is %08x), which did not change what translates them (program_carry.h; "
                 "RX_VK_PROGRAM_CARRY=0: translated again)", path.c_str(), disk_records.size(), (long long)(good >> 10), order.size(),
                 had_writer, writer);
        } else if (verdict == program_carry::Verdict::Dropped) {
            char had[96];
            if (disk_dropped_records)
                snprintf(had, sizeof had, "%zu programs of %zu keys, %lld KB", disk_dropped_records, disk_dropped.size(),
                         (long long)(whole >> 10));
            else
                snprintf(had, sizeof had, "%lld KB", (long long)(whole >> 10));
            logf("vk: program cache %s: 0 program keys kept, 0 KB: the one there (%s) was made for other translations (a translator "
                 "whose translation is another%s, or another GPU or other switches): its programs are translated again as they are met",
                 path.c_str(), had, carry_on() ? "" : ", or another build of it: RX_VK_PROGRAM_CARRY=0");
        } else {
            logf("vk: program cache %s: %zu program keys kept, %lld KB", path.c_str(), disk_records.size(), (long long)(good >> 10));
        }
    });
}
// ---- (Triforce) Inside the translation stamp again ----

void put_bytes(std::vector<uint8_t>& o, const void* d, size_t n) {
    o.insert(o.end(), (const uint8_t*)d, (const uint8_t*)d + n);
}
template <class T> void put(std::vector<uint8_t>& o, const T& v) { put_bytes(o, &v, sizeof v); }
template <class V> void put_list(std::vector<uint8_t>& o, const V& v) {
    put(o, (uint64_t)v.size());
    for (const auto& e : v) put(o, e);
}
// (a record is read within its own bytes: one made wrong, a count that ran past them, read on into the heap until it met an
// unmapped page, a crash in program_cache_load. Past the end, bad is set, the gets give zeros, and the record is left out)
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool bad = false;
    bool has(uint64_t n) {
        if (!bad && n > (uint64_t)(end - p)) bad = true;
        return !bad;
    }
    template <class T> T get() {
        alignas(T) uint8_t b[sizeof(T)]{};
        if (has(sizeof(T))) {
            std::memcpy(b, p, sizeof(T));
            p += sizeof(T);
        }
        return *(T*)b;
    }
    /// a count of elements of size elem, no more than the bytes left could hold (0 when bad)
    uint64_t count(size_t elem) {
        const uint64_t n = get<uint64_t>();
        if (!bad && n > (uint64_t)(end - p) / (elem ? elem : 1)) bad = true;
        return bad ? 0 : n;
    }
    template <class V> void list(V& v) {
        const uint64_t n = count(sizeof(typename V::value_type));
        v.clear();
        v.reserve((size_t)n);
        for (uint64_t i = 0; i < n && !bad; i++) v.push_back(get<typename V::value_type>());
    }
};

/// (hookshot) A program record's head, what comes before its stages: written and read only here, so that program_cache_read and
/// program_cache_each read the same fields. program_cache_each read only the first ones and took the mesh fallback's five words
/// for the stage count: no kept program reached the prediction (vk_predict.cpp, "predicted 0 pipelines")
void put_head(std::vector<uint8_t>& o, const Program& p) {
    put(o, (uint32_t)((p.compute ? 1 : 0) | (p.mesh ? 2 : 0) | (p.fallback ? 4 : 0) | (p.fallback_task ? 8 : 0)));
    put_list(o, p.answers);
    put_list(o, p.questions);
    put(o, p.workgroup);
    put(o, p.shared_memory);
    put(o, (uint32_t)p.fallback_topology);
    put(o, p.fallback_vertices_per_workgroup);
    put(o, p.fallback_record_words);
    put(o, p.fallback_payload_words);
    put(o, p.fallback_task_threads);
}
void get_head(Reader& r, Program& p) {
    const uint32_t kind = r.get<uint32_t>();
    p.compute = (kind & 1) != 0, p.mesh = (kind & 2) != 0, p.fallback = (kind & 4) != 0, p.fallback_task = (kind & 8) != 0;
    r.list(p.answers);
    r.list(p.questions);
    p.workgroup = r.get<std::array<uint32_t, 3>>();
    p.shared_memory = r.get<uint32_t>();
    p.fallback_topology = (VkPrimitiveTopology)r.get<uint32_t>();
    p.fallback_vertices_per_workgroup = r.get<uint32_t>();
    p.fallback_record_words = r.get<uint32_t>();
    p.fallback_payload_words = r.get<uint32_t>();
    p.fallback_task_threads = r.get<uint32_t>();
}

/// Shader::Info is plain data but for these members (the boost containers are heap vectors here, static_vector too);
/// they go on their own, the rest as it lies
void put_info(std::vector<uint8_t>& o, const Shader::Info& i) {
    put_bytes(o, &i, sizeof i);
    put_list(o, i.constant_buffer_descriptors);
    put_list(o, i.storage_buffers_descriptors);
    put_list(o, i.texture_buffer_descriptors);
    put_list(o, i.image_buffer_descriptors);
    put_list(o, i.texture_descriptors);
    put_list(o, i.image_descriptors);
    put(o, (uint64_t)i.legacy_stores_mapping.size());
    for (const auto& kv : i.legacy_stores_mapping) put(o, kv.first), put(o, kv.second);
    put(o, (uint64_t)i.handle_programs.size());
    for (const auto& h : i.handle_programs) put_list(o, h);
}
/// (hookshot) every descriptor array within kMaxDescriptorCount: the counts come from triforce_programs.bin, and two of 2^31
/// wrapped the layout's sum to 0 (a push layout with billions of descriptors), and one made the binds loop that often
constexpr uint32_t kMaxDescriptorCount = 4096;
template <class Info> bool descriptor_counts_fit(const Info& i) {
    auto fit = [](const auto& list) {
        for (const auto& d : list)
            if (d.count > kMaxDescriptorCount) return false;
        return true;
    };
    return fit(i.constant_buffer_descriptors) && fit(i.storage_buffers_descriptors) && fit(i.texture_buffer_descriptors) &&
           fit(i.image_buffer_descriptors) && fit(i.texture_descriptors) && fit(i.image_descriptors);
}
void get_info(Reader& r, Shader::Info& i) {
    if (!r.has(sizeof i)) return;   // (left as constructed: the record is bad, and left out)
    alignas(Shader::Info) static thread_local uint8_t fresh[sizeof(Shader::Info)];
    std::memcpy(fresh, (const void*)&i, sizeof i);   // (those members as constructed, here)
    std::memcpy((void*)&i, r.p, sizeof i);
    r.p += sizeof i;
#define KEEP(m) std::memcpy((void*)&i.m, fresh + ((const uint8_t*)&i.m - (const uint8_t*)&i), sizeof i.m)
    KEEP(constant_buffer_descriptors);
    KEEP(storage_buffers_descriptors);
    KEEP(texture_buffer_descriptors);
    KEEP(image_buffer_descriptors);
    KEEP(texture_descriptors);
    KEEP(image_descriptors);
    KEEP(legacy_stores_mapping);
    KEEP(handle_programs);
#undef KEEP
    r.list(i.constant_buffer_descriptors);
    r.list(i.storage_buffers_descriptors);
    r.list(i.texture_buffer_descriptors);
    r.list(i.image_buffer_descriptors);
    r.list(i.texture_descriptors);
    r.list(i.image_descriptors);
    for (uint64_t n = r.count(2 * sizeof(Shader::IR::Attribute)); n && !r.bad; n--) {
        const auto k = r.get<Shader::IR::Attribute>();
        i.legacy_stores_mapping[k] = r.get<Shader::IR::Attribute>();
    }
    i.handle_programs.resize((size_t)r.count(sizeof(uint64_t)));   // (each at least its own count)
    for (auto& h : i.handle_programs) r.list(h);
    if (!descriptor_counts_fit(i)) r.bad = true;   // (left out, and translated again)
}

/// (11-13) A program RX_VK_FRAG_PROBE or RX_VK_FRAG_SET names is translated at its first draw and never kept: the probe is
/// in the translation, and the kept program has none (it needed RX_VK_NO_PROGRAM_CACHE=1 before, every program translated again)
bool probe_names(uint64_t key) {
    static const char* const envs[2] = {getenv("RX_VK_FRAG_PROBE"), getenv("RX_VK_FRAG_SET")};
    if (!envs[0] && !envs[1]) return false;
    char hex[24];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)key);
    for (const char* v : envs) {
        const char* c = v ? strchr(v, ':') : nullptr;
        if (c && c > v && !strncmp(hex, v, (size_t)(c - v))) return true;
    }
    return false;
}

void program_cache_save(const Program& p, const std::vector<std::vector<uint32_t>>& spirv) {
    disk_init();
    if (!disk_on || spirv.size() != p.stages.size() || probe_names(p.key)) return;
    std::vector<uint8_t> o;
    o.reserve(256 << 10);
    put_head(o, p);
    put(o, (uint64_t)p.stages.size());
    for (size_t k = 0; k < p.stages.size(); k++) {
        const Stage& st = p.stages[k];
        put(o, st.nvn_stage);
        put(o, st.vk_stage);
        put_list(o, spirv[k]);
        put_list(o, st.c1);
        put_info(o, st.info);
    }
    const uint64_t head[2] = {p.key, o.size()};
    std::lock_guard lk(disk_mu);
    _fseeki64(disk_out, 0, SEEK_END);
    const int64_t at = _ftelli64(disk_out);
    fwrite(head, 8, 2, disk_out);
    fwrite(o.data(), 1, o.size(), disk_out);
    fflush(disk_out);
    if (disk_idx) {   // (after the record: an index ahead of the file would name a record it does not have)
        std::vector<uint8_t> e;
        idx_put(e, p.key, o.size(), *(const uint32_t*)o.data());
        fwrite(e.data(), 1, e.size(), disk_idx);
        fflush(disk_idx);
    }
    // (the precompile's: nothing holds them but the file, and the game starts on them at once)
    if (disk_listing) disk_records[p.key].push_back({at + 16, o.size(), (uint32_t)((p.compute ? 1 : 0) | (p.mesh ? 2 : 0) | (p.fallback ? 4 : 0))});
    dropped_note(p.key);   // ((Triforce) the log's "translated again")
}

/// The kept variants of key read from the file (in: a handle on it, used under mu when given) and made ready, to variants
void program_cache_read(FILE* in, std::mutex* mu, uint64_t key, const std::vector<DiskRecord>& recs,
                        std::vector<std::unique_ptr<Program>>& variants) {
    for (const DiskRecord& d : recs) {
        std::vector<uint8_t> data(16 + d.size);   // (with its head, the key and size: the index's word is checked against the file's)
        {
            std::unique_lock<std::mutex> lk;
            if (mu) lk = std::unique_lock<std::mutex>(*mu);
            _fseeki64(in, d.at - 16, SEEK_SET);
            if (fread(data.data(), 1, data.size(), in) != data.size()) continue;
        }
        uint64_t head[2];
        std::memcpy(head, data.data(), 16);
        if (head[0] != key || head[1] != d.size) {
            static std::atomic<int> logged{0};
            if (logged++ < 20)
                logf("vk: program cache: no record of program %016llx at %lld as the index said: translated again", (unsigned long long)key,
                     (long long)d.at);
            continue;
        }
        Reader r{data.data() + 16, data.data() + data.size()};
        auto p = std::make_unique<Program>();
        p->key = key;
        get_head(r, *p);
        p->variant = answers_hash(*p);
        const uint64_t n = r.count(1);
        bool ok = !r.bad && n <= 8;   // (a program has at most a few stages)
        std::optional<PackSkip> compute_skip;   // ((Triforce) a compute program keeps its module: no store for it)
        if (p->compute) compute_skip.emplace();
        for (uint64_t k = 0; ok && k < n; k++) {
            p->stages.emplace_back();
            Stage& st = p->stages.back();
            st.nvn_stage = r.get<int>();
            st.vk_stage = r.get<VkShaderStageFlagBits>();
            std::vector<uint32_t> spirv;
            r.list(spirv);
            r.list(st.c1);
            get_info(r, st.info);
            if (r.bad || spirv.empty()) {
                ok = false;
                break;
            }
            st.module = make_module(spirv, st.vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT ? &st.renodx : nullptr, &st.crc);
            ok = ok && st.module;
        }
        ok = ok && answers_fit(*p, p->answers);   // (hookshot: an answer past the stages or their slots: not this record)
        if (!ok) {
            static std::atomic<int> logged{0};
            // Program owns the stage containers, but their Vulkan handles are not RAII-managed.
            for (Stage& st : p->stages) {
                if (st.module) {
                    stage_module_destroy(st.module);
                    st.module = VK_NULL_HANDLE;
                }
            }
            if (r.bad && logged++ < 20)
                logf("vk: program cache: the record of program %016llx at %lld (%zu bytes) is not whole: left out, translated again",
                     (unsigned long long)key, (long long)d.at, d.size);
            continue;
        }
        program_pack(*p);   // ((Triforce) RX_VK_SPV_PACK: before anyone else has it)
        p->ready.store(true, std::memory_order_release);
        variants.push_back(std::move(p));
        st_cache_hits++;
    }
}

// ---- (Triforce) Outside the translation stamp, to the next mark (nx2/build.py translation_text; the rule: program_carry.h): the
// records read for the draws and read ahead of them, which program_cache_read reads ----
// ---- (Triforce) programs read ahead of their first draw (program_readahead.h has why). A worker with a handle of its own on the
// file reads, after each program a draw read itself, the next records in the file's order and keeps them made (parked); the
// draw that comes to one takes it from there, and its record leaves disk_records then, as when the draw reads it. The records
// stay listed while parked, so that every other reader of the list (the precompile, the borrows, the prepare in play) sees
// them as before. Locks: ra_mu, then disk_mu, never the other way; the worker never takes shader_mu. ----
std::mutex ra_mu;
std::condition_variable ra_cv;
std::deque<int64_t> ra_asks;        // (file offsets to read after)
std::deque<int64_t> ra_note_asks;   // ((Triforce, test16) note indexes to read after, program_readahead.h notes_order)
std::unordered_map<uint64_t, std::vector<std::unique_ptr<Program>>> ra_parked;
std::deque<uint64_t> ra_park_order;   // ((Triforce) the parked keys, oldest first: the full shelf lets these go first)
std::unordered_set<uint64_t> ra_reading;   // (the keys the workers read now)
std::deque<std::pair<uint64_t, std::vector<DiskRecord>>> ra_pending;   // ((Triforce) keys an ask chose, for the workers to read)
std::vector<std::pair<int64_t, uint64_t>> ra_order;   // (the listed records' first offsets and keys, in the file's order; ra_mu)
std::vector<std::pair<int64_t, uint64_t>> ra_note_order;   // ((Triforce) the notes' programs in the notes' order; ra_mu)
std::vector<std::pair<uint64_t, int64_t>> ra_note_index;   // ((Triforce) key -> note index, program_readahead::by_key; ra_mu)
bool ra_started = false;
std::atomic<int64_t> st_ra_read{0}, st_ra_taken{0}, st_ra_waits{0}, st_ra_wait_ns{0}, st_ra_dropped{0}, st_ra_let_go{0};
std::atomic<int64_t> st_ra_by_notes{0}, st_ra_read_ns{0};   // ((Triforce) read ahead in the notes' order; the workers' read time)
std::atomic<int64_t> st_draw_reads{0}, st_draw_read_ns{0};   // (programs a draw read itself, and their time: the stall line's)
// ((Triforce) the draws' own reads with shader_mu let go (program_read_once.h has why): the keys being read and the picks that
// have the lock let go, and for the stall line the waits for a program another draw was reading and those to take the lock
// again. RX_VK_PROGRAM_READ_UNLOCKED=0: read under the lock, as before)
program_read_once::Reads<std::mutex> draw_once(shader_mu);
std::atomic<int64_t> st_once_waits{0}, st_once_wait_ns{0}, st_once_relock_ns{0}, st_once_unlocked{0};
bool read_unlocked() {
    static const bool on = program_read_once::on(getenv("RX_VK_PROGRAM_READ_UNLOCKED"));
    return on;
}
// ((Triforce) the read-ahead in the order the last sessions first needed their programs (program_order.h has why): where it is
// and what it keeps (ra_mu), this session's order and the file's at the start (order_mu: taken under shader_mu or alone, nothing
// taken under it), and for the stall line why the draws read what they read themselves. RX_VK_PROGRAM_ORDER=0: none, as before)
program_order::Ahead ra_ahead;
std::unordered_set<uint64_t> ra_let_go_keys;   // (let go unused, from the shelf or the order: "let go before"; ra_mu)
std::mutex order_mu, order_file_mu;
program_order::Record order_rec;
std::vector<uint64_t> order_before;
std::chrono::steady_clock::time_point order_written;
std::atomic<int64_t> st_order_read{0}, st_order_taken{0}, st_order_let_go{0}, st_order_writes{0};
std::atomic<int64_t> st_why_unnamed{0}, st_why_let_go{0}, st_why_ahead{0};
bool order_on() {
    static const bool on = program_order::on(getenv("RX_VK_PROGRAM_ORDER"));
    return on;
}
// ((Triforce, wp19) the programs read in the order kept until needed, the whole order read (program_order.h Ahead::keep_all);
// RX_VK_PROGRAM_ORDER_KEEP=0: 512 at most, let go behind where the draws are, as before)
bool order_keep_all() {
    static const bool on = program_order::on(getenv("RX_VK_PROGRAM_ORDER_KEEP"));
    return on;
}
std::atomic<int64_t> st_order_asks_kept{0}, st_order_ram_let_go{0}, st_order_rewarmed{0};   // ((Triforce, wp19) asks' reads kept with
// the order's; let go, the RAM short; read cold and warmed later)
std::string order_path() {   // (beside the program cache)
    return disk_path.substr(0, disk_path.find_last_of("\\/") + 1) + "triforce_program_order.bin";
}
int readahead_count() {
    static const int n = program_readahead::count(getenv("RX_VK_PROGRAM_READAHEAD"));
    return n;
}
bool readahead_notes() {
    static const bool on = program_readahead::notes_on(getenv("RX_VK_PROGRAM_READAHEAD_NOTES"));
    return on;
}
void readahead_drop(std::vector<std::unique_ptr<Program>>& variants) {   // (made for nothing: their modules go)
    for (auto& p : variants) {
        for (Stage& st : p->stages)
            if (st.module) stage_module_destroy(st.module), st.module = VK_NULL_HANDLE;
        // ((Triforce, test18) a compute pipeline the warm made for it (vk_draw.cpp warm_compute): no dispatch had it
        if (p->compute_pipeline) vkDestroyPipeline(dev.device, p->compute_pipeline, nullptr), p->compute_pipeline = VK_NULL_HANDLE;
    }
    variants.clear();
}
/// ((Triforce, ra_mu held) The shelf's programs: those parked but the ones kept for the sessions' order, which it does not count
/// nor let go (program_order.h)
size_t shelf_locked() {
    return ra_parked.size() - std::min(ra_parked.size(), ra_ahead.kept.size());
}
/// ((Triforce, ra_mu held) k let go unused: noted for the stall line's "let go before" (bounded: a long session forgets)
void let_go_note_locked(uint64_t k) {
    if (ra_let_go_keys.size() >= 65536) ra_let_go_keys.clear();
    ra_let_go_keys.insert(k);
}
/// ((Triforce, ra_mu held) A draw met key (took it, read it or found it nowhere): the sessions' order's furthest place moves on,
/// and what it kept far behind that goes (program_order.h Ahead::met)
void ahead_met_locked(uint64_t key) {
    if (!order_on() || ra_ahead.order.empty()) return;
    std::vector<uint64_t> gone;
    ra_ahead.met(key, gone);
    for (const uint64_t k : gone) {
        auto it = ra_parked.find(k);
        if (it == ra_parked.end()) continue;
        readahead_drop(it->second), ra_parked.erase(it), st_order_let_go++;
        let_go_note_locked(k);
    }
}
/// ((Triforce, ra_mu held) Whether the sessions' order has more for the workers to read now (not with the RAM short)
bool ahead_more_locked() {
    return order_on() && ra_ahead.more() && !memory_ram_low() && !memory_ram_short();
}
/// ((Triforce, shader_mu held) A draw needs key for the first time this session: into the session's order
void order_note(uint64_t key) {
    std::lock_guard lk(order_mu);
    order_rec.add(key);
}
/// ((Triforce, ra_mu held, as the workers start) The last sessions' order, read from its file: the workers read in it
void order_load_locked() {
    std::vector<uint64_t> keys;
    if (FILE* f = fopen(order_path().c_str(), "rb")) {
        std::vector<uint8_t> data(16 + program_order::kKeysMax * 8 + 8 + 1);
        const size_t n = fread(data.data(), 1, data.size(), f);
        fclose(f);
        keys = program_order::read(data.data(), n);
    }
    logf("vk: programs read ahead in the order the last sessions first needed them: %zu programs (%s; RX_VK_PROGRAM_ORDER=0: "
         "none)", keys.size(), order_path().c_str());
    {
        std::lock_guard lk(order_mu);
        order_before = keys, order_written = std::chrono::steady_clock::now();
    }
    ra_ahead.keep_all = order_keep_all();   // ((Triforce, wp19))
    ra_ahead.load(std::move(keys));
}
/// ((Triforce, wp19, ra_mu held) The RAM short: every program kept for the sessions' order goes (program_order.h keep_all; the
/// rules before let none go for it)
void ahead_ram_let_go_locked() {
    if (!ra_ahead.keep_all || ra_ahead.kept.empty() || !memory_ram_short()) return;
    std::vector<uint64_t> gone;
    ra_ahead.let_go_all(gone);
    for (const uint64_t k : gone) {
        auto it = ra_parked.find(k);
        if (it == ra_parked.end()) continue;
        readahead_drop(it->second), ra_parked.erase(it), st_order_let_go++, st_order_ram_let_go++;
        let_go_note_locked(k);
    }
    logf("vk: the RAM is short: %zu programs read ahead in the last sessions' order let go (%lld so far)", gone.size(),
         (long long)st_order_ram_let_go.load());
}
/// ((Triforce, a worker's, no lock held) The session's order written beside the program cache when it grew, at most every few
/// seconds (program_order.h kSaveEveryMs): this session's programs first, then those of the sessions before it has not met.
/// Whole or not at all: a file of its own, then put in the old one's place
void order_write_maybe() {
    std::unique_lock fk(order_file_mu, std::try_to_lock);
    if (!fk.owns_lock()) return;
    std::vector<uint64_t> keys;
    {
        std::lock_guard lk(order_mu);
        const auto now = std::chrono::steady_clock::now();
        if (!order_rec.due(std::chrono::duration_cast<std::chrono::milliseconds>(now - order_written).count())) return;
        keys = program_order::merge(order_rec.keys, order_before);
        order_rec.saved = order_rec.keys.size(), order_written = now;
    }
    const std::string path = order_path(), part = path + ".part";
    const std::vector<uint8_t> data = program_order::write(keys);
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) return;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0 || !ok) {
        remove(part.c_str());
        return;
    }
#ifdef _WIN32
    const bool moved = MoveFileExA(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    const bool moved = rename(part.c_str(), path.c_str()) == 0;
#endif
    if (!moved) remove(part.c_str());
    else if (++st_order_writes == 1)
        logf("vk: the order this session first needed its programs in written beside the program cache: %zu programs (%s)", keys.size(),
             path.c_str());
}
/// ((Triforce, shader_mu held) A draw reads key itself: why the read-ahead had not read it (the stall line): named by no order
/// (neither the pipeline notes' nor the sessions'), read ahead and let go unused before, or not read ahead yet
void readahead_why(uint64_t key) {
    if (!readahead_count()) return;
    std::lock_guard lk(ra_mu);
    const bool named = (!ra_note_index.empty() && program_readahead::index_of(ra_note_index, key) >= 0) || ra_ahead.place.count(key);
    (!named ? st_why_unnamed : ra_let_go_keys.count(key) ? st_why_let_go : st_why_ahead)++;
}
} // namespace (readahead_computes_let_go is vk_shader.h's: vk_draw.cpp links to it)
/// ((Triforce) Why the draws read what they read themselves (readahead_why), and the programs taken read ahead in the sessions'
/// order (program_order.h), for the stall line (vk_replay.cpp status_tick)
void program_order_stats(int64_t* unnamed, int64_t* let_go, int64_t* ahead, int64_t* order_taken) {
    *unnamed = st_why_unnamed.load(std::memory_order_relaxed), *let_go = st_why_let_go.load(std::memory_order_relaxed);
    *ahead = st_why_ahead.load(std::memory_order_relaxed), *order_taken = st_order_taken.load(std::memory_order_relaxed);
}
/// ((Triforce, test19) The draws' and dispatches' waits for a program the read-ahead was reading, and their time, for the stall
/// line (vk_replay.cpp status_tick)
void readahead_wait_stats(int64_t* waits, int64_t* wait_ns) {
    *waits = st_ra_waits.load(std::memory_order_relaxed), *wait_ns = st_ra_wait_ns.load(std::memory_order_relaxed);
}
/// ((Triforce, test18) vk_shader.h: the warm's compute pipelines on the programs still read ahead, the RAM short. ra_mu tried:
/// the presents' thread waits for nobody)
bool readahead_computes_let_go(std::vector<VkPipeline>& out) {
    std::unique_lock lk(ra_mu, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    for (auto& [k, variants] : ra_parked)
        for (auto& p : variants)
            if (p->compute_pipeline) {
                out.push_back(p->compute_pipeline);
                p->compute_pipeline = VK_NULL_HANDLE;
                p->compute_state.store(0, std::memory_order_release);   // (its dispatch builds it as before)
            }
    return true;
}
/// ((Triforce) The draws' own reads with shader_mu let go (program_read_once.h), for the stall line (vk_replay.cpp status_tick):
/// the waits for a program another draw was reading and their time, and the picks' waits to take shader_mu again after a read
/// or a wait for the read-ahead's worker
void program_read_stats(int64_t* waits, int64_t* wait_ns, int64_t* relock_ns) {
    *waits = st_once_waits.load(std::memory_order_relaxed), *wait_ns = st_once_wait_ns.load(std::memory_order_relaxed);
    *relock_ns = st_once_relock_ns.load(std::memory_order_relaxed);
}
/// ((Triforce) vk_replay.cpp do_program_released) p let go once no program pick has shader_mu let go: such a pick holds its
/// program's stage codes (codes_of), which the release erases (program_read_once.h close)
void program_released_after_reads(const nvn_prog* p) {
    const program_read_once::Closed closed(draw_once);
    program_released(p);
}
namespace {
/// (ra_mu held) An ask's keys to the pending list: the next records after its place in the notes' order, or in the file's
void readahead_choose_locked() {
    const bool by_notes = !ra_note_asks.empty();
    const int64_t at = by_notes ? ra_note_asks.front() : ra_asks.front();
    (by_notes ? ra_note_asks : ra_asks).pop_front();
    // ((Triforce) the shelf full: as before, nothing more (RX_VK_PROGRAM_READAHEAD_NOTES=0); with the notes, its oldest go)
    // ((Triforce) the shelf: not the programs kept for the sessions' order, shelf_locked)
    if (!readahead_notes() && shelf_locked() >= program_readahead::kParkedMax) return;
    std::lock_guard dk(disk_mu);
    if (ra_order.empty()) {   // (once: the list as it is now; records written later are not read ahead)
        for (const auto& [k, recs] : disk_records)
            if (!recs.empty()) ra_order.push_back({recs[0].at, k});
        std::sort(ra_order.begin(), ra_order.end());
    }
    const size_t room = readahead_notes() ? program_readahead::kParkedMax
                                          : program_readahead::kParkedMax - std::min(shelf_locked(), program_readahead::kParkedMax);
    const auto wanted = [](uint64_t k) {   // ((Triforce) and not being read by a draw: program_read_once.h)
        if (ra_parked.count(k) || ra_reading.count(k) || !disk_records.count(k) || draw_once.reading(k)) return false;
        for (const auto& [pk, recs] : ra_pending)
            if (pk == k) return false;
        return true;
    };
    for (const uint64_t k : program_readahead::next_keys(by_notes ? ra_note_order : ra_order, at,
                                                         std::min<size_t>(room, (size_t)readahead_count()), wanted)) {
        const std::vector<DiskRecord>& recs = disk_records.find(k)->second;   // (listed: next_keys asked)
        if (recs.empty()) continue;
        // ((Triforce) the compute programs too, with the notes: where the precompile keeps no pipelines (RAM spared) it reads none
        // of them either, and a load's dispatches read theirs where they are dispatched. As before without: left to the precompile)
        if (!readahead_notes() && (recs[0].kind & 1)) continue;
        ra_pending.push_back({k, recs});
        if (by_notes) st_ra_by_notes++;
    }
}
/// ((Triforce, ra_mu held) Nothing asked: the next program of the sessions' order to the pending list (program_order.h), one
/// at a time, so that an ask that comes is served after the read under way
void ahead_choose_locked() {
    if (!ahead_more_locked()) return;
    std::lock_guard dk(disk_mu);
    const auto wanted = [](uint64_t k) {
        if (ra_parked.count(k) || ra_reading.count(k) || draw_once.reading(k)) return false;
        const auto it = disk_records.find(k);
        // (no compute program without the notes: as the asks' choice, left to the precompile)
        if (it == disk_records.end() || it->second.empty() || (!readahead_notes() && (it->second[0].kind & 1))) return false;
        for (const auto& [pk, recs] : ra_pending)
            if (pk == k) return false;
        return true;
    };
    if (const uint64_t k = ra_ahead.choose(wanted)) ra_pending.push_back({k, disk_records.find(k)->second});
    // ((Triforce, wp19) the whole order gone through: said once, for the lab's runs (read before the load or not)
    static bool told = false;
    if (!told && ra_ahead.next >= (int64_t)ra_ahead.order.size()) {
        told = true;
        logf("vk: programs read ahead in the order the last sessions first needed them: all %zu places gone through, %zu programs kept "
             "for the draws (%zu at most; %zu of them read without the pipelines' warm, made when the draws come near), %lld taken by "
             "them so far, %lld read by the asks kept with them (RX_VK_PROGRAM_ORDER_KEEP=0: 512 at most, let go behind where the "
             "draws are)", ra_ahead.order.size(), ra_ahead.kept.size(), ra_ahead.max_kept(), ra_ahead.cold.size(),
             (long long)st_order_taken.load(), (long long)st_order_asks_kept.load());
    }
}
void readahead_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk program read-ahead");
    FILE* in = fopen(disk_path.c_str(), "rb");
    if (!in) return;
    for (;;) {
        uint64_t k = 0;
        int64_t at = -1;   // ((Triforce, test18) its place in the notes' order, for the warm)
        std::vector<DiskRecord> recs;
        std::vector<std::unique_ptr<Program>> made;
        bool rewarm = false, cold = false;   // ((Triforce, wp19) a kept program's warm now; one read without it: program_order.h)
        {
            std::unique_lock lk(ra_mu);
            const auto work = [] {
                return !ra_pending.empty() || !ra_asks.empty() || !ra_note_asks.empty() || ahead_more_locked() || ra_ahead.warm_due();
            };
            // ((Triforce) with the sessions' order, awake now and then to write this session's: order_write_maybe)
            if (order_on()) ra_cv.wait_for(lk, std::chrono::seconds(1), work);
            else ra_cv.wait(lk, work);
            if (order_on()) ahead_ram_let_go_locked();   // ((Triforce, wp19) the RAM short: what the order kept goes)
            if (ra_pending.empty() && (!ra_asks.empty() || !ra_note_asks.empty())) readahead_choose_locked();
            // ((Triforce, wp19) nothing asked: first the warm of the nearest program the order kept cold, now that the warm has
            // room (the draws took warmed ones), taken in hand as a read is (a draw that comes to it waits for it)
            if (ra_pending.empty() && order_on())
                if (const uint64_t w = ra_ahead.next_warm())
                    if (auto it = ra_parked.find(w); it != ra_parked.end())
                        k = w, made = std::move(it->second), ra_parked.erase(it), ra_reading.insert(k), rewarm = true;
            if (!rewarm && ra_pending.empty()) ahead_choose_locked();   // ((Triforce) nothing asked: the sessions' order)
            if (!rewarm && ra_pending.empty()) {
                lk.unlock();
                if (order_on()) order_write_maybe();
                continue;
            }
            if (!rewarm) {
                k = ra_pending.front().first, recs = std::move(ra_pending.front().second);
                ra_pending.pop_front();
                ra_reading.insert(k);
                cold = ra_ahead.is_cold(k);
            }
            if (!ra_note_index.empty()) at = program_readahead::index_of(ra_note_index, k);
        }
        if (!rewarm) {
            const auto t0 = std::chrono::steady_clock::now();
            program_cache_read(in, nullptr, k, recs, made);
            st_ra_read_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        }
        // ((Triforce, test16) their noted pipelines made, while no draw can have them (k is being read: a draw waits for it):
        // vk_draw.cpp pipelines_warm, pipe_warm.h. A worker that has them still in hand holds no lock of the read-ahead's.
        // ((Triforce, wp19) not for one read cold, beyond the warm's window: its warm comes when the window reaches it)
        if (!cold) pipelines_warm(made, at);
        std::lock_guard lk(ra_mu);
        bool listed;   // ((Triforce) and not being read by a draw, which keeps its record listed until done: program_read_once.h)
        {
            std::lock_guard dk(disk_mu);
            listed = disk_records.count(k) != 0 && !draw_once.reading(k);
        }
        if (rewarm) {   // ((Triforce, wp19) parked again, unless a draw read it itself meanwhile or the RAM let it go)
            if (listed && !made.empty() && ra_ahead.kept.count(k)) {
                ra_parked[k] = std::move(made), st_order_rewarmed++;
            } else {
                readahead_drop(made), st_ra_dropped++;
                ra_ahead.taken(k);
            }
            ra_reading.erase(k);
            ra_cv.notify_all();
            continue;
        }
        const bool by_order = ra_ahead.is_chosen(k);   // ((Triforce) the sessions' order's: kept apart from the shelf)
        if (listed && !made.empty() && by_order) {
            ra_parked[k] = std::move(made), ra_ahead.read_done(k, true), st_order_read++;
        } else if (listed && !made.empty() && order_on() && !memory_ram_short() && ra_ahead.keep(k)) {
            // ((Triforce, wp19) an ask's read of a program the order names: kept with the order's, not on the shelf, which lets
            // its oldest go; the order passed it by while it was being read or parked (program_order.h keep). Not while the RAM
            // is short: the shelf's, as before
            ra_parked[k] = std::move(made), st_order_asks_kept++;
        } else if (listed && !made.empty()) {
            // ((Triforce) the shelf full: its oldest go, program_readahead.h to_let_go)
            for (size_t n = program_readahead::to_let_go(shelf_locked(), 1); n && !ra_park_order.empty(); ra_park_order.pop_front()) {
                auto old = ra_parked.find(ra_park_order.front());
                if (old == ra_parked.end()) continue;   // (taken since)
                let_go_note_locked(old->first);
                readahead_drop(old->second), ra_parked.erase(old), st_ra_let_go++, n--;
            }
            ra_parked[k] = std::move(made), ra_park_order.push_back(k), st_ra_read++;
        } else {
            readahead_drop(made), st_ra_dropped++;   // (taken by another reader meanwhile: the precompile's, say)
            if (by_order) ra_ahead.read_done(k, false);
        }
        ra_reading.erase(k);
        ra_cv.notify_all();
    }
}
/// (shader_mu held) key's variants read ahead, into variants: true when there were (a key the worker is reading is waited for,
/// at most program_readahead::kWaitMs); after a read of its own, the worker is asked to read on from there (at)
bool readahead_take(uint64_t key, std::vector<std::unique_ptr<Program>>& variants) {
    if (!readahead_count()) return false;
    std::unique_lock lk(ra_mu);
    if (ra_reading.count(key)) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto wait = [&] {
            ra_cv.wait_for(lk, std::chrono::milliseconds(program_readahead::kWaitMs), [&] { return !ra_reading.count(key); });
        };
        // ((Triforce) with shader_mu let go, program_read_once.h: the other picks go on meanwhile. ra_mu is let go before
        // shader_mu is taken again and taken after it, as everywhere; a pick of the same program that took it meanwhile left
        // nothing parked, and its records unlisted: the caller finds its variants in the list)
        if (read_unlocked() && draw_once.may_let_go()) {
            draw_once.unlocked([&] {
                wait();
                lk.unlock();
            }, &st_once_relock_ns);
            lk.lock();
        } else {
            wait();
        }
        st_ra_waits++;
        st_ra_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }
    // ((Triforce) a program the draws need for the first time: where they are in the sessions' order, and what it kept far
    // behind them goes, program_order.h)
    const bool by_order = ra_ahead.kept.count(key) != 0;
    if (variants.empty()) ahead_met_locked(key);
    auto it = ra_parked.find(key);
    if (it == ra_parked.end()) return false;
    for (auto& p : it->second) variants.push_back(std::move(p));
    ra_parked.erase(it), ra_ahead.taken(key);
    st_ra_taken++;
    if (by_order && ++st_order_taken % 256 == 0)
        logf("vk: programs read ahead in the order the last sessions first needed them: %lld taken by the draws, %lld read, %lld let go "
             "far behind the draws, %zu kept now (%zu at most), %zu places of %zu read (RX_VK_PROGRAM_ORDER=0: none)",
             (long long)st_order_taken.load(), (long long)st_order_read.load(), (long long)st_order_let_go.load(), ra_ahead.kept.size(),
             ra_ahead.max_kept(), (size_t)ra_ahead.next, ra_ahead.order.size());
    return true;
}
/// After key, read or taken by a draw (its record's place in the file at): the next records in the notes' order when the notes
/// name it ((Triforce) program_readahead.h), else in the file's
void readahead_ask(uint64_t key, int64_t at) {
    if (!readahead_count() || disk_path.empty()) return;
    std::lock_guard lk(ra_mu);
    if (!ra_started) {
        ra_started = true;
        if (order_on()) order_load_locked();   // ((Triforce) the last sessions' order, for the workers: program_order.h)
        for (int i = 0; i < (readahead_notes() ? program_readahead::kWorkers : 1); i++) std::thread(readahead_main).detach();
    }
    const int64_t note = readahead_notes() && !ra_note_index.empty() ? program_readahead::index_of(ra_note_index, key) : -1;
    pipelines_warm_consumed(note);   // ((Triforce, test18) where the draws are, for the warm: vk_draw.cpp, pipe_warm.h)
    if (note >= 0) program_readahead::ask(ra_note_asks, note);
    else program_readahead::ask(ra_asks, at);
    ra_cv.notify_all();
}

/// (shader_mu held, but by a borrow (keep)) the kept variants of key go to its list, ready, once
void program_cache_load(uint64_t key, std::vector<std::unique_ptr<Program>>& variants, bool keep = false) {
    disk_init();
    if (!disk_on || probe_names(key)) return;
    // ((Triforce) a program a draw needs for the first time this session: into its order, program_order.h)
    if (!keep && variants.empty() && order_on()) order_note(key);
    // (Triforce) read ahead already (readahead_take): only the record's place in the list goes, as below
    if (!keep && readahead_take(key, variants)) {
        int64_t at = -1;
        {
            std::lock_guard lk(disk_mu);
            auto it = disk_records.find(key);
            if (it != disk_records.end() && !it->second.empty()) at = it->second[0].at;
            disk_records.erase(key);
        }
        if (at >= 0) readahead_ask(key, at);   // (and on from there: the scene's draws keep ahead of the worker's reads otherwise)
        return;
    }
    // ((Triforce) a draw's own read with shader_mu let go, program_read_once.h: its records stay listed until it is done, and a
    // second draw of the program waits for the first's result. RX_VK_PROGRAM_READ_UNLOCKED=0: under the lock, as before)
    const bool once = !keep && read_unlocked();
    std::vector<DiskRecord> recs;
    {
        std::lock_guard lk(disk_mu);
        auto it = disk_records.find(key);
        if (it == disk_records.end()) return;
        if (keep || once) {   // (borrowed: still on disk for play; read once: listed until read)
            recs = it->second;
        } else {
            recs = std::move(it->second);
            disk_records.erase(it);
        }
    }
    if (once && !draw_once.claim(key, &st_once_wait_ns)) {   // (another draw read it: its variants are in the list by now)
        st_once_waits++;
        return;
    }
    if (!keep) readahead_why(key);   // ((Triforce) why the read-ahead had not read it, for the stall line)
    PROF_EXACT();   // (from disk: a mesh program's too)
    const auto t0 = std::chrono::steady_clock::now();
    auto t1 = t0;
    if (once) {
        const struct Done {   // (at this block's end, what was read in the list by then: the draws waiting for it go on)
            uint64_t key;
            ~Done() { draw_once.done(key); }
        } done{key};
        std::vector<std::unique_ptr<Program>> made;
        const auto read = [&] {
            const PackSkip skip;   // (on the thread replaying, as below)
            program_cache_read(disk_in, &disk_mu, key, recs, made);
            t1 = std::chrono::steady_clock::now();
        };
        if (draw_once.may_let_go()) {
            draw_once.unlocked(read, &st_once_relock_ns);
            st_once_unlocked++;
        } else {
            read();   // (a program's release waits for the picks: under the lock, as before)
        }
        bool listed;   // (the records read still listed: nobody took them meanwhile)
        {
            std::lock_guard lk(disk_mu);
            auto it = disk_records.find(key);
            listed = it != disk_records.end();
            if (listed) {   // (the ones read go; one written since (the precompile lists what it keeps) stays)
                std::erase_if(it->second, [&](const DiskRecord& d) {
                    return std::any_of(recs.begin(), recs.end(), [&](const DiskRecord& r) { return r.at == d.at; });
                });
                if (it->second.empty()) disk_records.erase(it);
            }
        }
        // (taken meanwhile by the precompile's load (program_cache_load_some), which puts its own in the list: ours go, unless
        // the list is still empty, where the draw would translate the program again)
        if (listed || variants.empty()) {
            for (auto& p : made) variants.push_back(std::move(p));
        } else {
            readahead_drop(made);
        }
    } else {   // ((Triforce) a draw's own read, on the thread replaying, or a borrow that lets the program go: its modules stay unpacked)
        const PackSkip skip;
        program_cache_read(disk_in, &disk_mu, key, recs, variants);
        t1 = std::chrono::steady_clock::now();
    }
    if (!keep && !recs.empty()) {   // (Triforce: a draw's own read; the worker reads on from it)
        st_draw_reads++;
        st_draw_read_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        readahead_ask(key, recs[0].at);
        static std::atomic<int64_t> n{0};
        if (++n % 500 == 0)
            logf("vk: programs from the cache: %lld read by the draws themselves (%.0f ms in all), %lld read ahead and taken, %lld read "
                 "ahead not taken yet (%lld dropped, %lld let go from a full shelf), %lld waits for one being read (%.1f ms in all); "
                 "%lld chosen by the notes' order, the workers' reads %.0f ms in all (RX_VK_PROGRAM_READAHEAD=0: none ahead; "
                 "RX_VK_PROGRAM_READAHEAD_NOTES=0: the file's order)",
                 (long long)st_draw_reads.load(), st_draw_read_ns.load() / 1e6, (long long)st_ra_taken.load(),
                 (long long)(st_ra_read.load() - st_ra_taken.load() - st_ra_let_go.load()), (long long)st_ra_dropped.load(),
                 (long long)st_ra_let_go.load(), (long long)st_ra_waits.load(), st_ra_wait_ns.load() / 1e6,
                 (long long)st_ra_by_notes.load(), st_ra_read_ns.load() / 1e6);
    }
}
// ---- (Triforce) Inside the translation stamp again ----

void translate_into(Program& p, const NvnState& s, const Code* vsc, const Code* fsc, const Code* csc,
                    const uint8_t color_types[8]) {
    const auto t0 = std::chrono::steady_clock::now();
    QuestionsNoted noted{p};
    ptr_diag_reset();
    try {
        Shader::Backend::Bindings bindings{};
        int questions = 0;
        std::vector<std::vector<uint32_t>> kept;   // (each stage's SPIR-V, for the program cache)
        if (csc) {
            Env env{s, *csc, Shader::Stage::Compute};
            {   // (EvilOOT) where this program's storage buffer table is in c0 (NVN2's layout, not Maxwell's 0x310)
                uint32_t count = 0, end = 0;
                const uint32_t base = compute_ssbo_table(s, &count, &end);
                Shader::Optimization::g_nvn2_compute_ssbo_base = count ? base : 0;   // (no table found: Maxwell's, as before)
                Shader::Optimization::g_nvn2_compute_ssbo_count = count ? count : 16;
                Shader::Optimization::g_nvn2_compute_layout_end = count ? end : 0;
                // (RX_VK_MISS_LOG_PROG=<key prefix>: the miss log for that program only)
                static const char* only = getenv("RX_VK_MISS_LOG_PROG");
                char hex[24];
                snprintf(hex, sizeof hex, "%016llx", (unsigned long long)p.key);
                Shader::Optimization::g_nvn2_miss_log_this = !only || !strncmp(hex, only, strlen(only));
                // (RX_VK_ALL_GLOBAL_PROGS=<key prefix>[,...], a test: no access of those programs bound to a descriptor)
                bool all_global = false;
                if (const char* ag = getenv("RX_VK_ALL_GLOBAL_PROGS"))
                    for (const char* q = ag; *q;) {
                        const char* e = strchr(q, ',');
                        const size_t len = e ? (size_t)(e - q) : strlen(q);
                        if (len && !strncmp(hex, q, len)) all_global = true;
                        q = e ? e + 1 : q + len;
                    }
                Shader::Optimization::g_all_global_this = all_global;
                if (all_global) logf("vk: program %s: every memory access left global (RX_VK_ALL_GLOBAL_PROGS)", hex);
                Shader::Optimization::g_gm_port_this = key_listed(hex, "EDEN_NX2_GM_PORT_PROGS");
                if (Shader::Optimization::g_gm_port_this) logf("vk: program %s: the Fritchii tracking (EDEN_NX2_GM_PORT_PROGS)", hex);
            }
            struct ResetTable {
                ~ResetTable() {
                    Shader::Optimization::g_nvn2_compute_ssbo_base = 0, Shader::Optimization::g_nvn2_compute_ssbo_count = 16;
                    Shader::Optimization::g_nvn2_compute_layout_end = 0;
                    Shader::Optimization::g_all_global_this = false;
                    Shader::Optimization::g_gm_port_this = false;
                }
            } reset_table;
            auto t = translate(env, false);
            questions += env.texture_questions;
            oot_visual(t->program, csc->key);
            Shader::RuntimeInfo rt{};
            note_fallbacks(p, "compute", *t);
            dump_ir(p, "compute", t->program);   // (before the SPIR-V: the programs it fails on are the ones to read)
            if (getenv("RX_VK_IR"))
                logf("vk: compute %016llx: workgroup %ux%ux%u, shared memory %u bytes, local memory %u", (unsigned long long)p.key,
                     t->program.workgroup_size[0], t->program.workgroup_size[1], t->program.workgroup_size[2],
                     t->program.shared_memory_size, t->program.local_memory_size);
            // (hookshot) The movie upscaler's dozen networks, on a card that cannot run them as the console does: their software matrix multiplies
            // took minutes each for AMD's compiler (the black screen with sound, Hookshot issue 26: JaredEzz's analysis) and the movies ran at 10 fps
            // (issues 38, 43). Left out, the last pass becomes a linear upscale of the movie (upscale_stub in vk_draw.cpp): the same picture at 60
            // fps, a little softer. The gameplay TAAU network stays. RX_VK_NETWORK_KEEP=<key prefixes>, "all": every one runs
            if (t->program.info.uses_cooperative_matrix) {
                static const std::string keep = getenv("RX_VK_NETWORK_KEEP") ? getenv("RX_VK_NETWORK_KEEP") : "fd029a317a9a4b94,9dc98871eb7fc101";   // (amd-fix: 9dc98871 resolves the dynamic-resolution scene to 2048x1152; left out, the 3D picture is black under the HUD)
                char hex[24];
                snprintf(hex, sizeof hex, "%016llx", (unsigned long long)p.key);
                bool kept_one = keep == "all";
                for (size_t at = 0; !kept_one && at < keep.size();) {
                    const size_t e = keep.find(',', at);
                    const std::string k = keep.substr(at, e == std::string::npos ? std::string::npos : e - at);
                    kept_one = !k.empty() && !strncmp(hex, k.c_str(), k.size());
                    at = e == std::string::npos ? keep.size() : e + 1;
                }
                if ((!kept_one && (Shader::Backend::SPIRV::mma_software || dev.props.vendorID != 0x10DE)) || getenv("RX_VK_NO_F16"))
                    throw std::runtime_error("a network left out on this card");
            }
            const std::vector<uint32_t> spirv = Shader::Backend::SPIRV::EmitSPIRV(profile_pinned(), rt, t->program, bindings);
            // (EvilOOT) RX_VK_DUMP_SPV=<folder>: the SPIR-V of the programs RX_VK_MISS_LOG_PROG names (all: unset), for spirv-val
            if (const char* dir = getenv("RX_VK_DUMP_SPV"); dir && Shader::Optimization::g_nvn2_miss_log_this) {
                char path[512];
                snprintf(path, sizeof path, "%s/%016llx.spv", dir, (unsigned long long)p.key);
                if (FILE* f = fopen(path, "wb")) fwrite(spirv.data(), 4, spirv.size(), f), fclose(f);
            }
            if (const char* d = getenv("RX_VK_IR")) {   // (the SPIR-V too, for spirv-val)
                char name[64];
                snprintf(name, sizeof name, "spv_%016llx.spv", (unsigned long long)p.key);
                if (strstr(d, name + 4 > name ? name + 4 : name) || true)
                    if (std::string(d).find(std::string(name + 4, 16)) != std::string::npos)
                        if (FILE* f = fopen(name, "wb")) fwrite(spirv.data(), 4, spirv.size(), f), fclose(f);
            }
            Stage st;
            st.nvn_stage = 5;
            st.info = t->program.info;
            {
                const PackSkip skip;   // ((Triforce) a compute program keeps its module: no store for it)
                st.module = make_module(spirv);
            }
            kept.push_back(spirv);
            st.vk_stage = VK_SHADER_STAGE_COMPUTE_BIT;
            st.c1 = csc->c1;
            p.workgroup = {t->program.workgroup_size[0], t->program.workgroup_size[1], t->program.workgroup_size[2]};
            p.shared_memory = t->program.shared_memory_size;
            p.stages.push_back(std::move(st));
        } else {
            Env vs_env{s, *vsc, Shader::Stage::VertexB};
            auto vs = translate(vs_env, true);
            questions += vs_env.texture_questions;
            std::unique_ptr<Translation> fs;
            if (fsc) {
                Env fs_env{s, *fsc, Shader::Stage::Fragment};
                fs = translate(fs_env, true);
                questions += fs_env.texture_questions;
            }
            Shader::RuntimeInfo vrt{};
            Shader::RuntimeInfo frt{};
            if (!vs->program.info.stores[Shader::IR::Attribute::PointSize]) vrt.fixed_state_point_size = 1.0f;
            // (an attribute the draw leaves unset: build_graphics gives it a stream of (0, 0, 0, 1). It took Disabled here, under the
            // same key)
            for (int i = 0; i < 16; i++) vrt.generic_input_types[i] = attribute_type_of(s.vattr[i]);
            vrt.previous_stage_stores.mask.set();
            vrt.convert_depth_mode = host->depth_minus_w() != 0;
            // (hookshot) the stage also stores its positions where a draw pushes an address: the temporal upscaler's motion
            // of what the game moves in its vertex shaders (vk_temporal.cpp capture_motion; vk_draw.cpp push_capture).
            // RX_VK_DLSS_CAPTURE=0: not (in the cache's stamp), the motion then the bones' (RX_VK_DLSS_ACTORS=bones)
            static const bool capture = !getenv("RX_VK_DLSS_CAPTURE") || atoi(getenv("RX_VK_DLSS_CAPTURE")) != 0;
            vrt.capture_position = capture;
            // Deku Tree web (#455): with DontInline, NVIDIA's compiled vertex shader substitutes the grid-origin
            // load result for the grid dimensions in the coordinate clamps. The SPIR-V names the right result.
            constexpr uint64_t kDekuWebVertexCode = 0xb449bcdb69b36a8aull;
            vrt.inline_global_128_loads = dev.props.vendorID == 0x10DE && vsc->key == kDekuWebVertexCode;
            // (OoT's Royal Family Tomb water writes clip.xy/clip.w as a noperspective varying. Clipping a
            // vertex with w close to zero then makes AMD interpolate the wrong depth, whose exp2 overflows
            // into the red dotted line. Write it times Position.w and interpolate perspective-correct in
            // the fragment stage, where multiplying by FragCoord.w restores the screen-linear value.)
            // Indexed fragment reads cannot undo that multiply, transform feedback would capture the
            // scaled value, and mesh pipelines keep their existing per-primitive interpolation.
            if (fs && !fs->program.info.loads_indexed_attributes && vrt.xfb_count == 0) {
                for (size_t i = 0; i < fs->program.info.interpolation.size(); ++i) {
                    if (fs->program.info.interpolation[i] == Shader::Interpolation::NoPerspective) {
                        vrt.noperspective_outputs.set(i);
                    }
                }
                frt.scaled_noperspective_inputs = vrt.noperspective_outputs.any();
            }
            Stage v;
            v.nvn_stage = 0;
            const std::vector<uint32_t> vspirv = Shader::Backend::SPIRV::EmitSPIRV(profile_graphics(VK_SHADER_STAGE_VERTEX_BIT), vrt, vs->program, bindings);
            note_fallbacks(p, "vertex", *vs);
            dump_ir(p, "vertex", vs->program);
            {   // (the programs that read the draw's parameters from the driver bank, each once)
                const auto& ld = vs->program.info.loads;
                const bool bi = ld[Shader::IR::Attribute::BaseInstance], bv = ld[Shader::IR::Attribute::BaseVertex],
                           di = ld[Shader::IR::Attribute::DrawID];
                static std::atomic<int> logged{0};
                if ((bi || bv || di) && logged++ < 64)
                    logf("vk: program %016llx reads the draw's%s%s%s", (unsigned long long)p.key, bv ? " base vertex" : "",
                         bi ? " base instance" : "", di ? " draw index" : "");
            }
            v.info = vs->program.info;
            v.module = make_module(vspirv);
            kept.push_back(vspirv);
            v.vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
            v.c1 = vsc->c1;
            p.stages.push_back(std::move(v));
            if (fs) {
                oot_visual(fs->program, fsc->key);
                frag_probe(fs->program, p.key);
                frt.previous_stage_stores = vs->program.info.stores;
                for (int i = 0; i < 8; i++) frt.color_output_types[i] = static_cast<Shader::AttributeType>(color_types[i]);
                static const bool nan_guard = !getenv("RX_VK_NAN_GUARD") || atoi(getenv("RX_VK_NAN_GUARD")) != 0;
                if (nan_guard) {
                    uint32_t float_targets = 0;
                    for (int i = 0; i < 8; i++)
                        if (frt.color_output_types[i] == Shader::AttributeType::Float) float_targets |= 1u << i;
                    Shader::Optimization::GuardFragmentOutputs(fs->program, float_targets);
                }
                const std::vector<uint32_t> fspirv = Shader::Backend::SPIRV::EmitSPIRV(profile_graphics(VK_SHADER_STAGE_FRAGMENT_BIT), frt, fs->program, bindings);
                note_fallbacks(p, "fragment", *fs);
                dump_ir(p, "fragment", fs->program);
                Stage f;
                f.nvn_stage = 1;
                f.info = fs->program.info;
                f.module = make_module(fspirv, &f.renodx, &f.crc);
                kept.push_back(fspirv);
                f.vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                f.c1 = fsc->c1;
                p.stages.push_back(std::move(f));
            }
        }
        for (const Stage& st : p.stages)
            if (!st.module) throw Shader::LogicError("vkCreateShaderModule failed");
        if (questions || answers_in) record_answers(p, s);
        st_translated++;
        ptr_diag_log(p.key, csc ? "compute" : fsc ? "graphics" : "vertex");
        if (seed_capture) *seed_capture = std::move(kept);
        else program_cache_save(p, kept);
    } catch (const std::exception& e) {
        p.failed = true;
        const bool trial = forced && !forced->want.empty();   // (an answer seed_compute tries: it may well not translate)
        if (!trial) st_failed++;
        static std::atomic<int> logged;
        if (!trial && logged++ < 30) logf("vk: program %016llx not translated: %s", (unsigned long long)p.key, e.what());
    }
    st_translate_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    program_pack(p);   // ((Triforce) RX_VK_SPV_PACK: before anyone else has it)
    p.ready.store(true, std::memory_order_release);
}

} // namespace

/// (Triforce) The programs the draws read from the program cache themselves and their time, and those they took read ahead
/// (program_cache_load), for the stall line (vk_replay.cpp status_tick)
void program_cache_stats(int64_t* draw_reads, int64_t* draw_read_ns, int64_t* ahead_taken) {
    *draw_reads = st_draw_reads.load(std::memory_order_relaxed), *draw_read_ns = st_draw_read_ns.load(std::memory_order_relaxed);
    *ahead_taken = st_ra_taken.load(std::memory_order_relaxed);
}
/// (Triforce) The picks' waits for shader_mu, their code reads, and the programs translated where asked, for the stall line
void program_pick_stats(int64_t* lock_ns, int64_t* codes_ns, int64_t* inline_n, int64_t* inline_ns);

namespace {

/// The mesh registers of an NVN2 task / mesh pair as the Switch 2 driver sets them up (clc597.h SET_MESH_SHADER_A / B,
/// SET_MESH_INIT_SHADER), from their control sections: what Eden's recompiler sizes the stage buffer and outputs by
/// A program translated in play: what it was (the shader precompile leaves none of these to be met)
std::unordered_set<uint64_t> seed_keys;   // (RX_VK_SEED_CHECK=1: the seed's stages, for miss_note)
void miss_note(uint64_t key, const char* what, uint64_t a, uint64_t b, const NvnState& s, const uint8_t color_types[8], size_t others) {
    static std::atomic<int> told{0};
    if (told++ >= 1500) return;
    char attrs[20]{};
    for (int i = 0; i < 16; i++) attrs[i] = (char)('0' + (int)attribute_type_of(s.vattr[i]));
    logf("vk: in play: %s %016llx (%016llx%s %016llx%s), inputs %s, outputs %02x %02x %02x %02x %02x %02x %02x %02x, %zu other variants", what,
         (unsigned long long)key, (unsigned long long)a, seed_keys.count(a) ? "+" : "", (unsigned long long)b, seed_keys.count(b) ? "+" : "", attrs,
         color_types[0], color_types[1], color_types[2],
         color_types[3], color_types[4], color_types[5], color_types[6], color_types[7], others);
}

std::array<u32, 3> mesh_registers_of(const Code* task, const Code& mesh) {
    // (the control section's topology in GL's numbers: 3 triangles; 1 points, not lines. OoT's four mesh programs with a 1 (the grass
    // drawn into the 2400 x 2400 shadow map around Link) write one index byte per primitive, the primitive count = their vertex
    // count, and a point size. Read as lines, each joined vertex 2i to 2i + 1 of its meshlet: a mesh of long thin dark stripes on
    // the ground, the water and Link around him, on AMD; Hookshot issue 302)
    // (Triforce 11-09: this is also the sun flicker of the distant haze in Hyrule Field: the streaks in the sun's shadow map changed
    // every frame, and the sun in-scatter volume that samples it (compute ee7764a6b1ef459c) with them. RX_VK_MESH_LINES=1: lines, as
    // before; the name is in the program cache's stamp (RX_VK_MESH), so either way gets its own translations)
    static const bool as_lines = getenv("RX_VK_MESH_LINES") != nullptr;
    const uint32_t topology = mesh.topology == 3 ? 4 : mesh.topology == 1 ? (as_lines ? 1 : 0) : mesh.topology;
    const uint32_t lines = task ? (task->payload_bytes + 127) / 128 : 0;
    // (the larger of the two for both: the control section's pair is not always vertices and primitives in that order. The door's
    // says 126 and 64, where its code writes 64 vertices and up to 128 primitives; held to 64, a meshlet's other primitives were
    // never output: holes in models and through the walls of the scene)
    const uint32_t most = std::max(mesh.max_vertices, mesh.max_primitives);
    return {(topology & 0xF) | (std::min(most, 0x100u) << 4) | (std::min(most, 0x100u) << 13),
            std::min(mesh.mesh_threads, 0x3Fu) << 20,
            (task ? std::min(task->mesh_threads, 0x3Fu) : 0) | (std::min(lines, 0x1FFu) << 12) |
                (std::min(lines, 0x1FFu) << 22)};
}

/// The stage whose banks a task / mesh program reads (its control section's stage when it is one of NVN's six)
int bank_stage(const Code& c) {
    return c.stage >= 0 && c.stage < kNvnStages ? c.stage : 0;   // (NVN2: task 6, mesh 7, with their own bindings)
}

void translate_mesh_into(Program& p, const NvnState& s, const Code* task, const Code& mesh, const Code* prim,
                         const Code* fsc, const uint8_t color_types[8]) {
    const auto t0 = std::chrono::steady_clock::now();
    QuestionsNoted noted{p};
    ptr_diag_reset();
    {   // (EvilOOT, RX_VK_MISS_LOG_PROG=<key prefix>: the miss log for that program only)
        static const char* only = getenv("RX_VK_MISS_LOG_PROG");
        char hex[24];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)p.key);
        Shader::Optimization::g_nvn2_miss_log_this = !only || !strncmp(hex, only, strlen(only));
    }
    try {
        Shader::Backend::Bindings bindings{};
        std::vector<std::vector<uint32_t>> kept;   // (each stage's SPIR-V, for the program cache: mesh programs were never kept,
                                                   // so each was translated and its pipeline built in the middle of play, every run)
        const bool fallback = p.fallback;
        std::unique_ptr<Translation> task_t;
        const std::array<u32, 3> mesh_regs = mesh_registers_of(task, mesh);
        if (task) {
            Env env{s, *task, Shader::Stage::Task};
            env.set_mesh(mesh_regs, nullptr);
            task_t = translate(env, true, true);
            mesh_nocull(task_t->program, task->key);
            note_fallbacks(p, "task", *task_t);
            dump_ir(p, "task", task_t->program, task->key);
            Shader::RuntimeInfo rt{};
            rt.previous_stage_stores.mask.set();
            rt.mesh_fallback = fallback;
            rt.mesh_fallback_capacity = Shader::Backend::SPIRV::MESH_FALLBACK_CAPACITY;
            const std::vector<uint32_t> spirv = Shader::Backend::SPIRV::EmitSPIRV(fallback ? profile_pinned() : profile_graphics(VK_SHADER_STAGE_TASK_BIT_EXT), rt, task_t->program, bindings);
            kept.push_back(spirv);
            Stage st;
            st.nvn_stage = bank_stage(*task);
            st.info = task_t->program.info;
            st.module = make_module(spirv);
            st.vk_stage = fallback ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_TASK_BIT_EXT;
            st.c1 = task->c1;
            p.stages.push_back(std::move(st));
        }
        Env menv{s, mesh, Shader::Stage::Mesh};
        menv.set_mesh(mesh_regs, prim ? prim->sph : nullptr);
        auto mesh_t = translate(menv, true, true);
        dump_ir(p, "mesh", mesh_t->program, mesh.key);
        {
            static std::atomic<int> told{0};
            if (told++ < 400) {
                const auto& mi = mesh_t->program.mesh;
                char l[300];
                int k = snprintf(l, sizeof l, "vk: mesh %016llx: threads %u max v %u p %u payload %u, %zu vertex lines:", (unsigned long long)p.key,
                                 mesh_t->program.workgroup_size[0], mi.max_vertices, mi.max_primitives, mi.payload_words, mi.vertex_lines.size());
                for (const u32 line : mi.vertex_lines)
                    if (k < 270) k += snprintf(l + k, sizeof l - k, " %x", line);
                k += snprintf(l + k, sizeof l - k, "; %zu primitive", mi.primitive_lines.size());
                logf("%s", l);
            }
        }
        int questions = menv.texture_questions;
        Shader::RuntimeInfo mrt{};
        if (task_t) mrt.previous_stage_stores = task_t->program.info.stores;
        else mrt.previous_stage_stores.mask.set();
        mrt.convert_depth_mode = host->depth_minus_w() != 0;
        mrt.mesh_fallback = fallback;
        mrt.mesh_fallback_capacity = Shader::Backend::SPIRV::MESH_FALLBACK_CAPACITY;
        mrt.mesh_fallback_task = task_t != nullptr;
        mrt.mesh_fallback_task_threads = task_t ? task_t->program.workgroup_size[0] : 0;
        const std::vector<uint32_t> mspirv = Shader::Backend::SPIRV::EmitSPIRV(fallback ? profile_pinned() : profile_graphics(VK_SHADER_STAGE_MESH_BIT_EXT), mrt, mesh_t->program, bindings);
        kept.push_back(mspirv);
        Stage ms;
        ms.nvn_stage = bank_stage(mesh);
        ms.info = mesh_t->program.info;
        ms.module = make_module(mspirv);
        ms.vk_stage = fallback ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_MESH_BIT_EXT;
        ms.c1 = mesh.c1;
        p.stages.push_back(std::move(ms));
        if (fallback) {   // (the ordinary vertex stage that reads the mesh epilogue's records)
            const Shader::MeshInfo& mi = mesh_t->program.mesh;
            Shader::RuntimeInfo vrt{};
            vrt.mesh_fallback = true;
            vrt.mesh_fallback_vertex = true;
            vrt.mesh_fallback_capacity = Shader::Backend::SPIRV::MESH_FALLBACK_CAPACITY;
            vrt.convert_depth_mode = host->depth_minus_w() != 0;
            const Shader::Stage saved_stage{mesh_t->program.stage};
            // (the generated vertex stage has no descriptors of its own: its inputs are the records. Only the stores
            // (the outputs) are kept from the mesh program's info.)
            Shader::Info saved_info{std::move(mesh_t->program.info)};
            mesh_t->program.info = Shader::Info{};
            mesh_t->program.info.stores = saved_info.stores;
            mesh_t->program.stage = Shader::Stage::VertexB;
            for (const u32 line : mi.primitive_lines) {
                const auto attribute = static_cast<Shader::IR::Attribute>(line);
                if (Shader::IR::IsGeneric(attribute))
                    vrt.per_primitive_generics.set(Shader::IR::GenericAttributeIndex(attribute));
            }
            const std::vector<uint32_t> vspirv =
                Shader::Backend::SPIRV::EmitSPIRV(profile_graphics(VK_SHADER_STAGE_VERTEX_BIT), vrt, mesh_t->program, bindings);
            mesh_t->program.info = std::move(saved_info);
            mesh_t->program.stage = saved_stage;
            kept.push_back(vspirv);
            Stage v;
            v.nvn_stage = 0;
            v.vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
            v.module = make_module(vspirv);
            v.c1 = mesh.c1;
            p.stages.push_back(std::move(v));
            const u32 topo{mesh.topology == 3 ? 4u : mesh.topology == 1 ? 0u : mesh.topology};
            p.fallback = true;
            p.fallback_task = task_t != nullptr;
            p.fallback_task_threads = task_t ? task_t->program.workgroup_size[0] : 1;
            p.fallback_topology = topo == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                                 : topo == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                             : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            p.fallback_vertices_per_workgroup = mi.max_primitives * mi.Corners();
            p.fallback_record_words = 1 + static_cast<u32>(mi.vertex_lines.size()) +
                                      static_cast<u32>(mi.primitive_lines.size());
            p.fallback_payload_words = mi.payload_words;
        }
        if (fsc) {
            Env fenv{s, *fsc, Shader::Stage::Fragment};
            auto fs = translate(fenv, true);
            dump_ir(p, "fragment", fs->program, fsc->key);   // (Evil Hookshot: a mesh program's fragment stage too, RX_VK_IR)
            frag_probe(fs->program, p.key);
            questions += fenv.texture_questions;
            Shader::RuntimeInfo frt{};
            // (what the mesh program stores, and its per-primitive generics: Eden's MakeRuntimeInfo)
            frt.previous_stage_stores = mesh_t->program.info.stores;
            for (const u32 line : mesh_t->program.mesh.primitive_lines) {
                const auto attribute = static_cast<Shader::IR::Attribute>(line);
                if (!Shader::IR::IsGeneric(attribute)) continue;
                const u32 index = Shader::IR::GenericAttributeIndex(attribute);
                if (fallback)   // (a vertex pipeline has no PerPrimitiveEXT: Flat the same values, as the generated stage writes them)
                    fs->program.info.interpolation[index] = Shader::Interpolation::Flat;
                else
                    frt.per_primitive_generics.set(index);
            }
            // (points: a generic the fragment program reads and the mesh program never writes is the point sprite's coordinate,
            // as the hardware replaces it. OoT's grass shadow reads its alpha mask at generic 5's (s, t): left at (0, 0, 0, 1),
            // each point was a filled square in the shadow map instead of a tuft)
            if (mesh_t->program.mesh.topology == 0)
                for (size_t i = 0; i < Shader::IR::NUM_GENERICS; i++)
                    if (fs->program.info.loads.Generic(i) && !mesh_t->program.info.stores.Generic(i))
                        frt.point_coord_generics.set(i);
            for (int i = 0; i < 8; i++) frt.color_output_types[i] = static_cast<Shader::AttributeType>(color_types[i]);
            const std::vector<uint32_t> fspirv = Shader::Backend::SPIRV::EmitSPIRV(profile_graphics(VK_SHADER_STAGE_FRAGMENT_BIT), frt, fs->program, bindings);
            kept.push_back(fspirv);
            Stage f;
            f.nvn_stage = 1;
            f.info = fs->program.info;
            f.module = make_module(fspirv, &f.renodx, &f.crc);
            f.vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            f.c1 = fsc->c1;
            p.stages.push_back(std::move(f));
        }
        // (RX_VK_DUMP_MESH_SPV=<folder>: each stage's SPIR-V of the mesh programs translated, <key>_<n>_<stage>.spv, for
        // spirv-val; the fallback's compute and generated vertex stages too. A program the cache holds is not translated)
        if (const char* dir = getenv("RX_VK_DUMP_MESH_SPV"); dir && kept.size() == p.stages.size()) {
            static std::atomic<uint32_t> dumped{0};
            const uint32_t n = dumped++;
            for (size_t k = 0; k < kept.size(); k++) {
                const VkShaderStageFlags vs = p.stages[k].vk_stage;
                const char* name = vs == VK_SHADER_STAGE_COMPUTE_BIT     ? (p.fallback_task && k == 0 ? "task_cs" : "mesh_cs")
                                   : vs == VK_SHADER_STAGE_TASK_BIT_EXT  ? "task"
                                   : vs == VK_SHADER_STAGE_MESH_BIT_EXT  ? "mesh"
                                   : vs == VK_SHADER_STAGE_VERTEX_BIT    ? "vert"
                                   : vs == VK_SHADER_STAGE_FRAGMENT_BIT ? "frag"
                                                                         : "stage";
                char path[512];
                snprintf(path, sizeof path, "%s/%016llx_%u_%s.spv", dir, (unsigned long long)p.key, n, name);
                if (FILE* f = fopen(path, "wb")) fwrite(kept[k].data(), 4, kept[k].size(), f), fclose(f);
            }
        }
        for (const Stage& st : p.stages)
            if (!st.module) throw Shader::LogicError("vkCreateShaderModule failed");
        record_answers(p, s);   // (task questions not counted: kept conservative)
        (void)questions;
        ptr_diag_log(p.key, task ? "task+mesh" : "mesh");
        program_cache_save(p, kept);
        // (where its stages read their storage buffers: the driver bank layout of NVN2's stages 6 and 7)
        static std::atomic<int> logged_ssbo;
        for (const Stage& st : p.stages)
            if (!st.info.storage_buffers_descriptors.empty() && logged_ssbo++ < 12)
                logf("vk: mesh program %016llx, NVN stage %d: %zu storage buffers, first in c%u at %#x, base %#x",
                     (unsigned long long)p.key, st.nvn_stage, st.info.storage_buffers_descriptors.size(),
                     st.info.storage_buffers_descriptors[0].cbuf_index, st.info.storage_buffers_descriptors[0].cbuf_offset,
                     st.info.nvn_buffer_base);
        st_translated++;
    } catch (const std::exception& e) {
        p.failed = true;
        st_failed++;
        static std::atomic<int> logged;
        if (logged++ < 30) logf("vk: mesh program %016llx not translated: %s", (unsigned long long)p.key, e.what());
    }
    st_translate_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    program_pack(p);   // ((Triforce) RX_VK_SPV_PACK: before anyone else has it)
    p.ready.store(true, std::memory_order_release);
}

/// (Triforce) The stages a mesh pick takes from its program object's list: the header-only stage, the task, mesh and fragment
/// stages, of those that lower (ok: a stage read as a head counts until its lowering says otherwise, codes_whole)
struct MeshStages {
    const Code *fs = nullptr, *prim = nullptr, *task = nullptr, *mesh = nullptr;
    bool operator==(const MeshStages&) const = default;
};
MeshStages mesh_stages(const std::vector<Code>& cs) {
    MeshStages m;
    for (const Code& c : cs) {
        if (!c.ok) continue;
        if (c.header_only) m.prim = &c;
        else if (c.stage == 6) m.task = &c;
        else if (c.stage == 7) m.mesh = &c;
        else if (c.stage == 1) m.fs = &c;
    }
    return m;
}

/// ((Triforce, shader_mu held by a pick) codes_of, the stages of a program object met for the first time read with the lock let
/// go where the pick may let it go (program_read_once.h): the replay's other threads go on picking meanwhile. Two picks of the
/// object at once each read it, and the first list put stands. RX_VK_STAGE_CODE_UNLOCKED=0: under the lock, as before
const std::vector<Code>& codes_read(const NvnState& s) {
    if (const auto it = codes.find(s.prog.serial); it != codes.end() && !it->second.empty()) return it->second;
    if (!stage_unlocked() || !draw_once.may_let_go()) return codes_of(s);
    std::vector<Code> read;
    draw_once.unlocked([&] { read = stages_read(s); });
    auto& v = codes[s.prog.serial];
    if (v.empty()) v = std::move(read);
    return v;
}

/// An NVN2 mesh program: the stages that write the internal stage buffer are its task (attribute writes only, when
/// there are two) and mesh programs; a header-only stage, its per-primitive outputs (shader_lock held)
const Program* mesh_program_locked(const NvnState& s, const uint8_t color_types[8]) {
    if (!dev.mesh_shader && !dev.mesh_fallback) return nullptr;
    const std::vector<Code>& cs = codes_read(s);
    const MeshStages m = mesh_stages(cs);
    const Code *fsc = m.fs, *prim = m.prim, *task = m.task, *mesh = m.mesh;
    static int logged;
    if (logged++ < 6) log_program_stages(s, "mesh draw");
    if (!mesh) return nullptr;
    const bool fallback = dev.uses_mesh_fallback(task != nullptr);
    const uint64_t key = mesh_key(task, *mesh, fsc, prim, color_types);
    auto& variants = programs[key];
    program_cache_load(key, variants);
    const uint64_t codes[4] = {mesh->key, fsc ? fsc->key : 0, task ? task->key : 0, prim ? prim->key : 0};
    for (auto& p : variants)
        if (!p->codes[0]) std::memcpy(p->codes, codes, sizeof codes);
    for (auto& p : variants) {
        if (p->fallback != fallback || p->mesh == fallback) continue;
        if (!p->ready.load(std::memory_order_acquire)) return p.get();
        if (p->failed) return nullptr;
        if (still_valid(s, *p)) return p.get();
    }
    // ((Triforce) a variant to translate: its stages lowered first where they were read as heads (codes_whole). Should one of
    // them not lower, this key is not the program's: picked again from the stages that do, the program it was before)
    codes_whole(s);
    if (!(mesh_stages(cs) == m)) return mesh_program_locked(s, color_types);
    miss_note(key, "mesh", mesh->key, fsc ? fsc->key : 0, s, color_types, variants.size());
    auto p = std::make_unique<Program>();
    p->key = key;
    p->mesh = !fallback;
    p->fallback = fallback;
    std::memcpy(p->codes, codes, sizeof codes);
    Program* raw = p.get();
    variants.push_back(std::move(p));
    struct Job {
        Program* p;
        NvnState s;
        Code task, mesh, prim, fs;
        bool has_task, has_prim, has_fs;
        uint8_t ct[8];
    };
    auto job = std::make_shared<Job>();
    job->p = raw, job->s = s;
    job->has_task = task != nullptr, job->has_prim = prim != nullptr, job->has_fs = fsc != nullptr;
    if (task) job->task = *task;
    job->mesh = *mesh;
    if (prim) job->prim = *prim;
    if (fsc) job->fs = *fsc;
    std::memcpy(job->ct, color_types, 8);
    jobs_push([job] {
        translate_mesh_into(*job->p, job->s, job->has_task ? &job->task : nullptr, job->mesh,
                            job->has_prim ? &job->prim : nullptr, job->has_fs ? &job->fs : nullptr, job->ct);
    }, false, &raw->ready);   // (a draw that waits for it runs it at once if it is still queued: jobs_wait)
    return raw;
}

const Program* mesh_program_for(const NvnState& s, const uint8_t color_types[8]) {
    std::lock_guard lk(shader_mu);
    return mesh_program_locked(s, color_types);
}

} // namespace

namespace {
/// Picks by program and inputs (the replay thread's): draws mostly use programs seen a moment ago with the same
/// inputs; the variant search runs again only when those changed, and a variant chosen for its textures' types is
/// kept while the handles it was checked against are the same
struct Pick {
    const Program* p = nullptr;
    std::vector<uint32_t> handles;   // (at its answers' descriptors: their slots and the slots' versions)
};
thread_local std::unordered_map<uint64_t, Pick> picks;   // (each recording thread's: program_pick has its own lock)
uint64_t pick_inputs(const NvnState& s, const uint8_t color_types[8]) {
    uint64_t h = 0x9E37;
    for (int i = 0; i < 16; i++) h = h * 1099511628211ull ^ (uint64_t)s.vattr[i];
    h = h * 1099511628211ull ^ (uint64_t)s.nvattr;
    uint64_t ct = 0;
    std::memcpy(&ct, color_types, 8);
    return h * 1099511628211ull ^ ct;
}
void answer_handles(const NvnState& s, const Program& p, std::vector<uint32_t>& out) {
    out.clear();
    for (const Program::Answer& a : p.answers) {
        const uint32_t id = answer_handle(s, p, a) & 0xFFFFF;
        out.push_back(id);
        out.push_back(texture_slot_version((int)id));
    }
}
} // namespace

/// (hookshot) the bound program's stages and code keys, as "stage:key" words (RX_VK_SKIP_PROG's log)
/// (for logs) what the program's textures resolve to now
std::string program_textures(const NvnState& s, const Program& p) {
    std::string out;
    for (const Stage& st : p.stages)
        for (const auto& d : st.info.texture_descriptors) {
            const uint32_t h = descriptor_handle(s, st, d);
            nvn_tex t;
            nvn_view v;
            bool has_view = false;
            char b[160];
            const bool known = h && texture_of((int)(h & 0xFFFFF), &t, &v, &has_view);
            if (known)
                snprintf(b, sizeof b, " tex %#x %ux%ux%u fmt %#x target %d view %d", h, (unsigned)t.d.width, (unsigned)t.d.height, (unsigned)t.d.depth,
                         (unsigned)t.d.format, (int)t.d.target, (int)has_view);
            else
                snprintf(b, sizeof b, " tex %#x (not registered)", h);
            out += b;
        }
    return out;
}

std::string program_code_keys(const NvnState& s) {
    std::string out;
    if (!s.has_prog) return out;
    // ((Triforce) under the shaders' lock, the stages read as heads lowered: the stages that lower, as before, codes_whole)
    std::lock_guard lk(shader_mu);
    const std::vector<Code>& cs = codes_of(s);
    codes_whole(s);
    for (const Code& c : cs) {
        if (!c.ok) continue;
        char b[40];
        snprintf(b, sizeof b, " %u:%016llx", c.stage, (unsigned long long)c.key);
        out += b;
    }
    return out;
}

/// (hookshot) the bound compute program's code key (0: none): what "does not lower" names, for a rejected dispatch
uint64_t compute_code_key(const NvnState& s) {
    if (!s.has_prog) return 0;
    std::lock_guard lk(shader_mu);   // ((Triforce) as program_code_keys)
    const std::vector<Code>& cs = codes_of(s);
    codes_whole(s);
    for (const Code& c : cs)
        if (c.ok && c.stage == 5 && !c.header_only) return c.key;
    return 0;
}

/// The last answer for this serial, these stages, these bindings and these vertex inputs
bool prog_same(int kind, uint64_t serial, uint64_t stages, uint64_t bind, uint64_t reg, uint64_t ub, int nvattr,
               const uint32_t* vattr, const uint8_t* ct, int ckind, uint64_t cserial, uint64_t cstages, uint64_t cbind,
               uint64_t creg, uint64_t cub, int cnvattr, const uint32_t* cvattr, const uint8_t* cct) {
    if (kind != ckind || serial != cserial || stages != cstages || bind != cbind || reg != creg || ub != cub ||
        nvattr != cnvattr)
        return false;
    for (int i = 0; i < 16; i++)
        if (vattr[i] != cvattr[i]) return false;
    for (int i = 0; i < 8; i++)
        if (ct[i] != cct[i]) return false;
    return true;
}
uint64_t prog_stage_sig(const nvn_prog& p) {
    uint64_t h = (uint64_t)p.nstage << 32 | p.magic;
    for (int i = 0; i < 6; i++) h = h * 1099511628211ull ^ p.sd[i].data ^ (p.sd[i].control << 1);
    return h ^ p.serial;
}
// (hookshot) The stages whose bindings a graphics program's answers are read from, one bit a stage, when each answer is a handle in c2 of the
// stage itself (a handle program reads uniform memory, another bank other state): then the pick stands while those stages' textures,
// samplers and images do (NvnState::tex_gen), whatever else was bound. ~0: not so, the pick stands while nothing at all was bound anew
uint32_t answer_stage_mask(const Program& p) {
    uint32_t mask = 0;
    for (const Program::Answer& a : p.answers) {
        const Stage& st = p.stages[a.stage];
        if (st.nvn_stage < 0 || st.nvn_stage > 4) return ~0u;
        bool c2;
        if (a.image) {
            const auto& d = st.info.image_descriptors[a.desc];
            c2 = d.handle_program == ~0U && d.cbuf_index == 2;
        } else {
            const auto& d = st.info.texture_descriptors[a.desc];
            c2 = d.handle_program == ~0U && d.cbuf_index == 2 && (!d.has_secondary || d.secondary_cbuf_index == 2);
        }
        if (!c2) return ~0u;
        mask |= 1u << st.nvn_stage;
    }
    return mask;
}
struct ProgFast {
    bool on = false;
    int kind = 0;
    uint64_t serial = 0, stages = 0, bind = 0, reg = 0, ub = 0;
    int nvattr = 0;
    uint32_t vattr[16]{};
    uint8_t ct[8]{};
    const Program* p = nullptr;
    uint32_t amask = ~0u;           // (answer_stage_mask of p, ~0 for a compute program)
    uint64_t tex_gen[kNvnStages]{};   // (the stages' tex_gen it was kept at)
};
thread_local ProgFast prog_fast[2];

const Program* program_for(const NvnState& s, ProgramKind kind, const uint8_t color_types[8]) {
    if (!s.has_prog) return nullptr;
    if (kind == ProgramKind::Mesh) return mesh_program_for(s, color_types);
    const bool compute = kind == ProgramKind::Compute;
    static int64_t st_hit, st_none, st_differ, st_calls, st_by_tex;   // (logged now and then: how the cache does)
    if (++st_calls % 200000 == 0)
        logf("vk: program picks: %lld kept (%lld by the last picks' textures, a uniform buffer bound since), %lld new, %lld with other textures (of %lld)",
             (long long)st_hit, (long long)st_by_tex, (long long)st_none, (long long)st_differ, (long long)st_calls);
    const uint64_t stages = prog_stage_sig(s.prog);
    const uint64_t bind = s.bind_gen, reg = registrations_generation(), ub = ub_generation();
    // (hookshot) the pair of the last picks stood only while nothing at all was bound anew, and OoT binds a uniform buffer for nearly every
    // draw: a draw of the same program with the same textures found its program through the full pick (the key, the answers' handles, a
    // compare). It stands now while the textures its answers are read from do (answer_stage_mask). RX_VK_PICK_BIND_GEN=1: as before
    static const bool by_bind = getenv("RX_VK_PICK_BIND_GEN") != nullptr;
    for (const ProgFast& f : prog_fast) {   // (the draw before this one, and the one that took turns with it)
        if (!(f.on && f.p && f.p->ready.load(std::memory_order_acquire) && !f.p->failed)) continue;
        uint64_t fbind = f.bind;
        bool by_tex = false;
        if (fbind != bind && !by_bind && !compute && f.amask != ~0u) {
            by_tex = true;
            for (int k = 0; k < kNvnStages; k++)
                if ((f.amask >> k & 1) && f.tex_gen[k] != s.tex_gen[k]) by_tex = false;
            if (by_tex) fbind = bind;
        }
        if (prog_same((int)kind, s.prog.serial, stages, bind, reg, ub, s.nvattr, s.vattr, color_types, f.kind, f.serial,
                      f.stages, fbind, f.reg, f.ub, f.nvattr, f.vattr, f.ct)) {
            st_by_tex += by_tex && f.bind != bind;
            return st_hit++, f.p;
        }
    }
    const uint64_t key = (compute ? 0x5EED : pick_inputs(s, color_types)) * 1099511628211ull ^ s.prog.serial;
    Pick& pk = picks[key];
    auto keep = [&](const Program* p) {
        if (!p || !p->ready.load(std::memory_order_acquire) || p->failed) return p;
        if (!(prog_fast[0].on && prog_fast[0].p == p)) prog_fast[1] = prog_fast[0];
        ProgFast& f = prog_fast[0];
        f.on = true, f.kind = (int)kind, f.serial = s.prog.serial, f.stages = stages, f.bind = bind, f.reg = reg, f.ub = ub;
        f.nvattr = s.nvattr, f.p = p;
        f.amask = compute ? ~0u : answer_stage_mask(*p);
        std::memcpy(f.tex_gen, s.tex_gen, sizeof f.tex_gen);
        std::memcpy(f.vattr, s.vattr, sizeof f.vattr);
        std::memcpy(f.ct, color_types, sizeof f.ct);
        return p;
    };
    if (pk.p && pk.p->ready.load(std::memory_order_acquire) && !pk.p->failed) {
        if (pk.p->answers.empty()) return st_hit++, keep(pk.p);
        static thread_local std::vector<uint32_t> now;
        answer_handles(s, *pk.p, now);
        if (now == pk.handles) return st_hit++, keep(pk.p);
        st_differ++;
        if (still_valid(s, *pk.p)) {   // (other textures of the same types: the same variant)
            pk.handles.swap(now);
            return keep(pk.p);
        }
    } else {
        st_none++;
    }
    const Program* picked = program_pick(s, kind, color_types);
    if (picked && picked->ready.load(std::memory_order_acquire) && !picked->failed) {
        pk.p = picked;
        answer_handles(s, *picked, pk.handles);
    } else {
        pk.p = nullptr;
    }
    if (picks.size() > 65536) picks.clear();
    return keep(picked);
}

// (Triforce) For the stall line (vk_replay.cpp status_tick, program_pick_stats): the picks' waits for shader_mu, their reads of
// the stages' code (codes_read: with the lock let go, its taking again too), and the programs translated where they were asked
// (compute programs: a dispatch is never left out), with their times
std::atomic<int64_t> st_pick_lock_ns{0}, st_pick_codes_ns{0}, st_pick_inline{0}, st_pick_inline_ns{0};
namespace {
/// (Triforce) The stages a graphics or compute pick takes from its program object's list, by NVN stage, of those that lower (ok:
/// a stage read as a head counts until its lowering says otherwise, codes_whole)
void pick_stages(const std::vector<Code>& cs, const Code* stage_code[8]) {
    for (const Code& c : cs)
        if (c.ok && !c.header_only) stage_code[c.stage] = &c;
}
/// ((Triforce, shader_mu held) program_pick's choice among its program object's stages cs (codes_of), from the start again
/// should a stage it took not lower)
const Program* program_pick_locked(const NvnState& s, ProgramKind kind, const uint8_t color_types[8], const std::vector<Code>& cs) {
    const bool compute = kind == ProgramKind::Compute;
    const Code* stage_code[8]{};
    pick_stages(cs, stage_code);
    if (compute ? !stage_code[5] : !stage_code[0]) return nullptr;
    if (!compute && (stage_code[2] || stage_code[3] || stage_code[4])) {   // (geometry/tessellation: not translated)
        static int logged;
        if (logged++ < 40) log_program_stages(s, "dropped stages");
    }
    // the variants of these stages, for these vertex input types and colour output types
    const uint64_t key = compute ? stage_code[5]->key : graphics_key(*stage_code[0], stage_code[1], s.vattr, color_types);
    auto& variants = programs[key];
    program_cache_load(key, variants);
    const uint64_t codes[4] = {stage_code[compute ? 5 : 0]->key, !compute && stage_code[1] ? stage_code[1]->key : 0, 0, 0};
    for (auto& p : variants)
        if (!p->codes[0]) std::memcpy(p->codes, codes, sizeof codes);
    for (auto& p : variants) {
        if (!p->ready.load(std::memory_order_acquire)) return p.get();   // (being translated)
        if (p->failed) return nullptr;
        if (still_valid(s, *p)) return p.get();
    }
    // ((Triforce) a variant to translate: its stages lowered first where they were read as heads (codes_whole). Should one of
    // them not lower, this key is not the program's: picked again from the stages that do, the program it was before)
    codes_whole(s);
    const Code* now[8]{};
    pick_stages(cs, now);
    if (std::memcmp(now, stage_code, sizeof now)) return program_pick_locked(s, kind, color_types, cs);
    miss_note(key, compute ? "compute" : "graphics", stage_code[compute ? 5 : 0]->key, stage_code[1] ? stage_code[1]->key : 0, s, color_types,
              variants.size());
    auto p = std::make_unique<Program>();
    p->key = key;
    p->compute = compute;
    std::memcpy(p->codes, codes, sizeof codes);
    Program* raw = p.get();
    variants.push_back(std::move(p));
    static const bool sync = getenv("RX_VK_SYNC_SHADERS") != nullptr;
    if (compute || sync) {   // (compute: dispatches are never left out, so they are translated where they are)
        const auto t0 = std::chrono::steady_clock::now();
        translate_into(*raw, s, stage_code[0], stage_code[1], compute ? stage_code[5] : nullptr, color_types);
        st_pick_inline.fetch_add(1, std::memory_order_relaxed);   // (Triforce: program_pick_stats)
        st_pick_inline_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                                    std::memory_order_relaxed);
        return raw->failed ? nullptr : raw;
    }
    struct Job {
        Program* p;
        NvnState s;
        Code vs, fs;
        bool has_fs;
        uint8_t ct[8];
    };
    auto job = std::make_shared<Job>();
    job->p = raw, job->s = s, job->vs = *stage_code[0], job->has_fs = stage_code[1] != nullptr;
    if (job->has_fs) job->fs = *stage_code[1];
    std::memcpy(job->ct, color_types, 8);
    jobs_push([job] { translate_into(*job->p, job->s, &job->vs, job->has_fs ? &job->fs : nullptr, nullptr, job->ct); }, false,
              &raw->ready);   // (as the mesh one: a waiting draw runs it, not every job queued before it)
    return raw;
}
} // namespace

const Program* program_pick(const NvnState& s, ProgramKind kind, const uint8_t color_types[8]) {
    PROF_EXACT();   // (the picks' slow path: a translation, a cache read; PROF_PROGRAM samples the rest)
    const auto l0 = std::chrono::steady_clock::now();
    std::lock_guard lk(shader_mu);
    const auto l1 = std::chrono::steady_clock::now();
    st_pick_lock_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(l1 - l0).count(), std::memory_order_relaxed);
    const std::vector<Code>& cs = codes_read(s);
    st_pick_codes_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - l1).count(),
                               std::memory_order_relaxed);
    return program_pick_locked(s, kind, color_types, cs);
}

void program_wait(const Program* p) {
    if (p && !p->ready.load(std::memory_order_acquire)) jobs_wait(p->ready);
}

/// (hookshot) Whether the environment variable var lists hex (key prefixes, comma-separated)
bool key_listed(const char* hex, const char* var) {
    const char* list = getenv(var);
    for (const char* q = list ? list : ""; *q;) {
        const char* e = strchr(q, ',');
        const size_t len = e ? (size_t)(e - q) : strlen(q);
        if (len && !strncmp(hex, q, len)) return true;
        q = e ? e + 1 : q + len;
    }
    return false;
}
uint64_t program_cache_stamp() { return disk_stamp(); }
// ---- (Triforce) Outside the translation stamp, to the next mark (nx2/build.py translation_text; the rule: program_carry.h): the
// precompile's and the prepare's loads of kept records, which program_cache_read reads ----
/// (the precompile, before the game: vk_draw.cpp) the kept compute programs and those named, ready, each given to fn. The rest of the
/// game's programs stay on disk until a draw asks for one
size_t program_cache_load_some(const std::unordered_set<uint64_t>& wanted, const std::function<void(Program&)>& fn, bool computes,
                               bool fallbacks) {
    disk_init();
    // (read and made on several threads, each with a handle of its own on the file: one after the other, the few hundred records
    // (about 200 MB) were a second or more of the start before the first pipeline was built, a CPU or the disk at a time)
    std::vector<std::pair<uint64_t, std::vector<DiskRecord>>> work;
    if (disk_on) {
        std::lock_guard lk(disk_mu);
        for (auto it = disk_records.begin(); it != disk_records.end();) {
            // (fallbacks: and every program translated for the mesh fallback (kind 4), not only those play has drawn, whatever the
            // compute list says: their compute pipelines, made in play, took AMD up to 12 s each on first sight, the foliage and the
            // door white until then. Only a GPU without mesh shaders has such programs)
            if (wanted.count(it->first) ||
                (!it->second.empty() && ((computes && (it->second[0].kind & 1)) || (fallbacks && (it->second[0].kind & 4))))) {
                work.push_back({it->first, std::move(it->second)});
                it = disk_records.erase(it);
            } else {
                ++it;
            }
        }
    }
    std::vector<std::vector<std::unique_ptr<Program>>> made(work.size());
    std::atomic<size_t> next{0};
    const auto worker = [&] {
        FILE* in = fopen(disk_path.c_str(), "rb");
        for (;;) {
            const size_t i = next++;
            if (i >= work.size()) break;
            if (in) program_cache_read(in, nullptr, work[i].first, work[i].second, made[i]);
            else program_cache_read(disk_in, &disk_mu, work[i].first, work[i].second, made[i]);
        }
        if (in) fclose(in);
    };
    {
        const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        for (unsigned i = 1; i < std::min({cpus, 16u, (unsigned)work.size()}); i++) threads.emplace_back(worker);
        worker();
        for (std::thread& t : threads) t.join();
    }
    std::lock_guard lk(shader_mu);
    size_t n = 0;
    for (size_t i = 0; i < work.size(); i++) {
        auto& variants = programs[work[i].first];
        for (auto& p : made[i]) variants.push_back(std::move(p));
        for (auto& p : variants) n++, fn(*p);
    }
    return n;
}

/// (Triforce, test16) The pipeline notes' programs, one key a note in the notes' order (vk_draw.cpp precompile_run): the order the
/// read-ahead reads in (program_readahead.h notes_order). RX_VK_PROGRAM_READAHEAD_NOTES=0: not used
void program_readahead_notes(const std::vector<uint64_t>& note_programs) {
    if (!readahead_notes() || note_programs.empty()) return;
    auto order = program_readahead::notes_order(note_programs);
    auto index = program_readahead::by_key(order);
    std::lock_guard lk(ra_mu);
    ra_note_order = std::move(order), ra_note_index = std::move(index);
    logf("vk: programs read ahead in the pipeline notes' order: %zu programs named by %zu notes (RX_VK_PROGRAM_READAHEAD_NOTES=0: the "
         "program cache's order)", ra_note_order.size(), note_programs.size());
}
/// (hookshot) Program key's kept variants made for fn, then let go (modules and layouts): the records stay for play
void program_cache_borrow(uint64_t key, const std::function<void(Program&)>& fn) {
    std::vector<std::unique_ptr<Program>> variants;
    program_cache_load(key, variants, true);
    for (auto& p : variants) {
        fn(*p);
        for (Stage& st : p->stages)
            if (st.module) stage_module_destroy(st.module);
        if (p->layout) vkDestroyPipelineLayout(dev.device, p->layout, nullptr);
        if (p->set_layout) vkDestroyDescriptorSetLayout(dev.device, p->set_layout, nullptr);
    }
}

/// (hookshot, the prepare's driver part: vk_draw.cpp warm_libraries) the key of every kept graphics and mesh program, whoever made
/// it (the seed, a state list, play), in the file's order, from the list of records alone: program_cache_each reads every record
/// for its list, the whole file (6.7 GB with every program of the game). *loaded: the variants read in already, whose records
/// that list no longer names (see loaded_variants)
std::vector<uint64_t> program_cache_keys(std::vector<Program*>* loaded) {
    disk_init();
    std::vector<std::pair<int64_t, uint64_t>> at;
    if (disk_on) {
        std::lock_guard lk(disk_mu);
        for (const auto& kv : disk_records)
            if (!kv.second.empty() && !(kv.second[0].kind & 1)) at.push_back({kv.second[0].at, kv.first});
    }
    std::sort(at.begin(), at.end());   // (file order: borrowed one after the other, the disk is read forwards)
    std::vector<uint64_t> keys;
    keys.reserve(at.size());
    for (const auto& a : at) keys.push_back(a.second);
    std::lock_guard lk(shader_mu);
    for (const auto& kv : programs)
        for (const auto& p : kv.second)
            if (p->ready.load(std::memory_order_acquire) && !p->failed && !p->compute) loaded->push_back(p.get());
    return keys;
}
// ---- (Triforce) Inside the translation stamp again ----

/// (hookshot, the prediction: vk_predict.cpp) every kept graphics program variant, read without making its modules
void program_cache_each(const std::function<void(const KeptProgram&)>& fn) {
    disk_init();
    if (!disk_on) return;
    std::vector<std::pair<uint64_t, DiskRecord>> all;
    {
        std::lock_guard lk(disk_mu);
        for (const auto& kv : disk_records)
            for (const DiskRecord& d : kv.second)
                if (!(d.kind & 1)) all.push_back({kv.first, d});
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second.at < b.second.at; });   // (file order)
    KeptProgram k;
    for (const auto& [key, d] : all) {
        std::vector<uint8_t> data(d.size);
        {
            std::lock_guard lk(disk_mu);
            _fseeki64(disk_in, d.at, SEEK_SET);
            if (fread(data.data(), 1, d.size, disk_in) != d.size) continue;
        }
        Reader r{data.data(), data.data() + data.size()};
        Program p;
        p.key = key;
        get_head(r, p);   // (the whole head: reading past only part of it took the fallback's fields for the stage count)
        k.key = key, k.variant = answers_hash(p), k.mesh = p.mesh;
        const uint64_t n = r.count(1);
        k.stages.resize(n <= 8 ? n : 0);
        for (auto& [stage, spirv] : k.stages) {
            r.get<int>();
            stage = r.get<VkShaderStageFlagBits>();
            r.list(spirv);
            std::vector<uint8_t> c1;
            r.list(c1);
            Shader::Info info;
            get_info(r, info);
        }
        if (!r.bad && !k.stages.empty()) fn(k);
    }
}

// ---- (hookshot) Every shader of the game before it starts, as TotK Recomp's tools/prepare_shaders.py and totk.exe --prepare-shaders: the
// builder writes triforce_shaders.bin (nx2/shader_census.py) from the archives, each program's stages, and here each is translated
// for the default state (nothing bound, float inputs and outputs) and kept in triforce_programs.bin. What a draw then asks for is on
// disk, whatever part of the game it is first met in ----
namespace {
struct SeedStage {
    uint64_t key = 0;
    uint32_t stage = 0, end = 0;
    size_t control = 0, code = 0;
    uint8_t sph[sizeof(Shader::ProgramHeader)]{};
};
struct Seed {
    std::vector<uint8_t> blob;
    std::vector<SeedStage> stages;
    std::vector<std::vector<uint32_t>> programs;
    std::unordered_map<uint64_t, uint32_t> index;   // (a stage by its code key: the pipeline state list's programs)
} seed;
std::string beside_exe(const char* name) {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + name;
}
Code stage_of_seed(const SeedStage& st) {
    return parse_code(seed.blob.data() + st.control, seed.blob.data() + st.code, st.end);
}
} // namespace

std::string seed_path() { return beside_exe("triforce_shaders.bin"); }

size_t seed_open() {
    if (FILE* f = fopen(seed_path().c_str(), "rb")) {
        _fseeki64(f, 0, SEEK_END);
        seed.blob.resize((size_t)_ftelli64(f));
        _fseeki64(f, 0, SEEK_SET);
        if (seed.blob.size() < 16 || fread(seed.blob.data(), 1, seed.blob.size(), f) != seed.blob.size() ||
            memcmp(seed.blob.data(), "HKSEED01", 8))
            seed.blob.clear();
        fclose(f);
    }
    if (seed.blob.empty()) return 0;
    disk_init();
    disk_listing = true;
    size_t at = 16;
    // (hookshot) every count, size and index is checked against the file before it is used: a file cut short or made
    // wrong is no seed, never a read past it
    const size_t size = seed.blob.size();
    const auto u32 = [](size_t pos) { uint32_t v; std::memcpy(&v, seed.blob.data() + pos, 4); return v; };
    const auto bad = [] {
        seed.blob.clear(), seed.stages.clear(), seed.programs.clear(), seed.index.clear();
        disk_listing = false;
        return (size_t)0;
    };
    const uint32_t nprograms = u32(8), nstages = u32(12);
    if (nstages > (size - 16) / 20 || nprograms > (size - 16) / 4) return bad();
    seed.stages.resize(nstages);
    for (SeedStage& st : seed.stages) {
        if (size - at < 20) return bad();
        const uint32_t csize = u32(at + 12), bytes = u32(at + 16);
        if (csize > size - at - 20 || bytes > size - at - 20 - csize) return bad();
        st.stage = u32(at + 8);
        st.control = at + 20;
        st.code = st.control + csize;
        at = st.code + bytes;
        if (csize < kControlSize || !control_end(seed.blob.data() + st.control, &st.end) || st.end > bytes) {
            st.end = 0;   // (not one this game's programs can be)
            continue;
        }
        st.key = code_key(seed.blob.data() + st.control, seed.blob.data() + st.code, st.end);
        seed_keys.insert(st.key);
        seed.index.emplace(st.key, (uint32_t)(&st - seed.stages.data()));
        std::memcpy(st.sph, seed.blob.data() + st.code, std::min<size_t>(sizeof st.sph, st.end));
    }
    seed.programs.resize(nprograms);
    for (auto& list : seed.programs) {
        if (size - at < 4) return bad();
        const uint32_t n = u32(at);
        if (n > (size - at - 4) / 4) return bad();
        list.resize(n);
        if (n) std::memcpy(list.data(), seed.blob.data() + at + 4, (size_t)n * 4);
        for (const uint32_t k : list)
            if (k >= nstages) return bad();
        at += 4 + (size_t)n * 4;
    }
    return seed.programs.size();
}

void seed_close() {
    seed = Seed{};
    disk_listing = false;
}


std::unique_ptr<Program> seed_translate(const SeedStage* const by_stage[8], const uint32_t attributes[16], const uint8_t color_types[8],
                                        uint64_t* key_out);

/// (hookshot) How many variants of key are in memory, made or read in already, their variant hashes to *variants. They are kept
/// too, though disk_records no longer lists them: program_cache_load and program_cache_load_some take a key's records out as they
/// read them. By disk_records alone the prepare in play (prepare_background) translated again every program the precompile had
/// read, every kept compute program among them, and kept each a second time
size_t loaded_variants(uint64_t key, std::unordered_set<uint64_t>* variants) {
    std::lock_guard lk(shader_mu);
    const auto it = programs.find(key);
    if (it == programs.end()) return 0;
    size_t n = 0;
    for (const auto& p : it->second)
        if (p->ready.load(std::memory_order_acquire) && !p->failed) {
            n++;
            if (variants) variants->insert(p->variant);
        }
    return n;
}

// (the game's skinned models read their bone indices as unsigned integers from generic attributes 6 and 7: each vertex program that
// reads those is also made that way)
std::unique_ptr<Program> seed_program(size_t i, int inputs) {
    static const uint8_t no_types[8]{};
    uint32_t attributes[16]{};
    if (inputs) {
        nvn_va_st a{};
        a.fmt = NVN_FORMAT_RGBA8UI;
        std::memcpy(&attributes[6], &a, 4);
        if (inputs == 2) attributes[7] = attributes[6];
    }
    const SeedStage* by_stage[8]{};
    for (const uint32_t k : seed.programs[i]) {
        if (!seed.stages[k].end) return nullptr;
        by_stage[seed.stages[k].stage & 7] = &seed.stages[k];
    }
    const SeedStage* one = by_stage[5] ? by_stage[5] : by_stage[7] ? by_stage[7] : by_stage[0];
    if (!one) return nullptr;
    if (inputs) {   // (a variant only where it changes what the program reads)
        if (by_stage[5] || by_stage[7]) return nullptr;
        const auto read = [&](size_t g) {
            const auto used = reinterpret_cast<const Shader::ProgramHeader*>(by_stage[0]->sph)->vtg.InputGeneric(g);
            return used[0] || used[1] || used[2] || used[3];
        };
        if (!read(6) || (inputs == 2 && !read(7))) return nullptr;
    }
    return seed_translate(by_stage, attributes, no_types, nullptr);
}

/// The program of these seed stages for these vertex inputs and colour outputs, translated and kept (null: kept already, or it does
/// not translate); its key to *key_out
std::unique_ptr<Program> seed_translate(const SeedStage* const by_stage[8], const uint32_t attributes[16], const uint8_t color_types[8],
                                        uint64_t* key_out) {
    const PackSkip skip;   // ((Triforce) its callers let it go (prepare_run, state_list_prepare): its modules stay unpacked)
    // the key first, from what the stages say of themselves: a program kept already is only read back
    Code head[8];   // (the back end's Code is a large object only once parsed)
    for (int k = 0; k < 8; k++)
        if (by_stage[k]) head[k].key = by_stage[k]->key, std::memcpy(head[k].sph, by_stage[k]->sph, sizeof head[k].sph);
    const Code* fs_head = by_stage[1] ? &head[1] : nullptr;
    const uint64_t key = by_stage[5] ? by_stage[5]->key
                         : by_stage[7] ? mesh_key(by_stage[6] ? &head[6] : nullptr, head[7], fs_head, nullptr, color_types)
                                       : graphics_key(head[0], fs_head, attributes, color_types);
    if (key_out) *key_out = key;
    bool have;
    {
        std::lock_guard lk(disk_mu);
        have = disk_records.count(key) != 0;
    }
    if (have || loaded_variants(key, nullptr)) return nullptr;
    Code code[8];
    for (int k = 0; k < 8; k++)
        if (by_stage[k]) {
            code[k] = stage_of_seed(*by_stage[k]);
            if (!code[k].ok) return st_failed++, nullptr;
        }
    auto blank = std::make_unique<NvnState>();
    blank->nvattr = 16;
    std::memcpy(blank->vattr, attributes, sizeof blank->vattr);
    auto p = std::make_unique<Program>();
    p->key = key;
    p->compute = by_stage[5] != nullptr;
    const bool mesh = by_stage[7] != nullptr;
    if (mesh && !dev.mesh_shader && !dev.mesh_fallback) return nullptr;
    p->fallback = mesh && dev.uses_mesh_fallback(by_stage[6] != nullptr);
    p->mesh = mesh && !p->fallback;
    if (mesh)
        translate_mesh_into(*p, *blank, by_stage[6] ? &code[6] : nullptr, code[7], nullptr, by_stage[1] ? &code[1] : nullptr, color_types);
    else if (p->compute) {
        seed_cs_control = seed.blob.data() + by_stage[5]->control;
        translate_into(*p, *blank, nullptr, nullptr, &code[5], color_types);
        seed_cs_control = nullptr;
    } else
        translate_into(*p, *blank, &code[0], by_stage[1] ? &code[1] : nullptr, nullptr, color_types);
    return p;
}

bool seed_is_compute(size_t i) {
    for (const uint32_t k : seed.programs[i])
        if ((seed.stages[k].stage & 7) == 5) return true;
    return false;
}

// (hookshot) The seed's compute programs, each with its texture variants. A program's texture questions are answered in play by the
// textures bound (Program::answers), which nothing before the game knows; the questions themselves are few, so each is tried with every
// answer it can get, and an answer that changes the SPIR-V makes a variant. Variants of two answers and more are taken fewest first, up
// to cap: one of them is the variant play makes, which still_valid then finds kept. All go to triforce_programs.bin, as play keeps them,
// and each new one is given to made (its modules live until it returns)
SeedVariants seed_compute(size_t i, size_t cap, const std::function<void(Program&)>& made) {
    static const uint8_t no_types[8]{};
    SeedVariants r;
    const SeedStage* cs = nullptr;
    for (const uint32_t k : seed.programs[i])
        if ((seed.stages[k].stage & 7) == 5) cs = &seed.stages[k];
    if (!cs || !cs->end) return r;
    // (the variants kept already, by play or an earlier prepare: the others are
    // still asked for, as a kept base says nothing of them)
    std::unordered_set<uint64_t> variants;
    {
        std::lock_guard lk(disk_mu);
        if (auto it = disk_records.find(cs->key); it != disk_records.end())
            for (const DiskRecord& d : it->second) {
                std::vector<uint8_t> data(d.size);
                _fseeki64(disk_in, d.at, SEEK_SET);
                if (fread(data.data(), 1, d.size, disk_in) != d.size) continue;
                Reader rd{data.data(), data.data() + data.size()};
                Program q;
                rd.get<uint32_t>();
                rd.list(q.answers);
                if (!rd.bad) variants.insert(answers_hash(q));
            }
    }
    loaded_variants(cs->key, &variants);
    const size_t had = variants.size();
    const Code code = stage_of_seed(*cs);
    if (!code.ok) return st_failed++, r.failed = true, r;
    auto blank = std::make_unique<NvnState>();
    blank->nvattr = 16;
    seed_cs_control = seed.blob.data() + cs->control;
    struct Reset { ~Reset() { seed_cs_control = nullptr, forced = nullptr, seed_capture = nullptr; } } reset;
    Forced f;
    uint32_t count = 0, end = 0;
    f.lo = compute_ssbo_table(*blank, &count, &end) + 16 * count;   // (the texture handles follow the storage buffers)
    f.hi = end;
    struct Try {
        std::unique_ptr<Program> p;
        std::vector<std::vector<uint32_t>> spirv;
        std::vector<std::pair<uint32_t, uint8_t>> asked;
    };
    const auto run = [&](std::map<std::pair<uint32_t, uint8_t>, uint8_t> want) {
        Try t;
        f.asked.clear();
        f.want = std::move(want);
        t.p = std::make_unique<Program>();
        t.p->key = cs->key;
        t.p->compute = true;
        forced = &f, seed_capture = &t.spirv;
        translate_into(*t.p, *blank, nullptr, nullptr, &code, no_types);
        forced = nullptr, seed_capture = nullptr;
        t.asked = f.asked;
        return t;
    };
    const auto drop = [](Try& t) {
        for (Stage& st : t.p->stages)
            if (st.module) stage_module_destroy(st.module), st.module = VK_NULL_HANDLE;
    };
    std::vector<Try> kept;
    kept.push_back(run({}));
    if (kept[0].p->failed) return drop(kept[0]), r.failed = true, r;
    const auto base_asked = kept[0].asked;
    const auto base_spirv = kept[0].spirv;
    r.questions = base_asked.size();
    // each question alone: the answers that change the SPIR-V
    std::vector<std::vector<uint8_t>> options(base_asked.size());
    for (size_t q = 0; q < base_asked.size(); q++) {
        const uint8_t kind = base_asked[q].second, plain = kind ? 0 : (uint8_t)Shader::TextureType::Color2D;
        for (uint8_t a = 0; a < (kind ? 2 : 9); a++) {
            if (a == plain) continue;
            Try t = run({{base_asked[q], a}});
            if (!t.p->failed && t.asked == base_asked && t.spirv != base_spirv && kept.size() < cap) {
                options[q].push_back(a);
                kept.push_back(std::move(t));
            } else {
                drop(t);
            }
        }
    }
    // then two answers and more, fewest first
    std::vector<size_t> open;
    for (size_t q = 0; q < options.size(); q++)
        if (!options[q].empty()) open.push_back(q);
    for (size_t n = 2; n <= open.size() && kept.size() < cap; n++) {
        std::vector<size_t> pick(n), choice(n);
        for (size_t k = 0; k < n; k++) pick[k] = k;
        for (;;) {   // (each n of the open questions, each answer of each)
            std::fill(choice.begin(), choice.end(), 0);
            for (;;) {
                if (kept.size() >= cap) break;
                std::map<std::pair<uint32_t, uint8_t>, uint8_t> want;
                for (size_t k = 0; k < n; k++) want[base_asked[open[pick[k]]]] = options[open[pick[k]]][choice[k]];
                Try t = run(std::move(want));
                if (!t.p->failed && t.asked == base_asked) kept.push_back(std::move(t));
                else drop(t);
                size_t k = 0;
                while (k < n && ++choice[k] == options[open[pick[k]]].size()) choice[k++] = 0;
                if (k == n) break;
            }
            if (kept.size() >= cap) break;
            size_t k = n;
            while (k > 0 && pick[k - 1] == open.size() - n + k - 1) k--;
            if (k == 0) break;
            pick[k - 1]++;
            for (size_t j = k; j < n; j++) pick[j] = pick[j - 1] + 1;
        }
    }
    r.capped = kept.size() >= cap;
    for (Try& t : kept) {
        if (variants.insert(t.p->variant).second) {
            program_cache_save(*t.p, t.spirv), r.variants++;
            if (made) made(*t.p);
        }
        drop(t);
    }
    r.kept = had && !r.variants;
    return r;
}


/// (hookshot) a program of the pipeline state list (vk_draw.cpp): by its stages' code keys (graphics: vertex, fragment; mesh: mesh,
/// fragment, task), as the seed holds them, and the vertex input and colour output types it was drawn with
std::unique_ptr<Program> seed_program_of(const uint64_t code[3], bool mesh, const uint32_t attributes[16], const uint8_t color_types[8],
                                         uint64_t* key_out) {
    *key_out = 0;
    const SeedStage* by_stage[8]{};
    const int want[3] = {mesh ? 7 : 0, 1, mesh ? 6 : -1};
    for (int k = 0; k < 3; k++) {
        if (!code[k]) continue;
        const auto it = seed.index.find(code[k]);
        if (want[k] < 0 || it == seed.index.end()) return nullptr;
        const SeedStage& st = seed.stages[it->second];
        if ((int)(st.stage & 7) != want[k]) return nullptr;
        by_stage[want[k]] = &st;
    }
    if (!by_stage[want[0]]) return nullptr;
    return seed_translate(by_stage, attributes, color_types, key_out);
}
const Program* seed_variant_of(const uint64_t code[3], bool mesh, const uint32_t attributes[16], const uint8_t color_types[8],
                               const std::vector<uint8_t>& questions, const std::vector<Program::Answer>& answers) {
    const SeedStage* by_stage[8]{};
    const int want[3] = {mesh ? 7 : 0, 1, mesh ? 6 : -1};
    for (int k = 0; k < 3; k++) {
        if (!code[k]) continue;
        const auto it = seed.index.find(code[k]);
        if (want[k] < 0 || it == seed.index.end()) return nullptr;
        const SeedStage& st = seed.stages[it->second];
        if ((int)(st.stage & 7) != want[k]) return nullptr;
        by_stage[want[k]] = &st;
    }
    if (!by_stage[want[0]]) return nullptr;
    Code code_of[8];
    for (int k = 0; k < 8; k++)
        if (by_stage[k]) {
            code_of[k] = stage_of_seed(*by_stage[k]);
            if (!code_of[k].ok) return nullptr;
        }
    const Code* fs = by_stage[1] ? &code_of[1] : nullptr;
    auto blank = std::make_unique<NvnState>();
    blank->nvattr = 16;
    std::memcpy(blank->vattr, attributes, sizeof blank->vattr);
    auto p = std::make_unique<Program>();
    p->key = mesh ? mesh_key(by_stage[6] ? &code_of[6] : nullptr, code_of[7], fs, nullptr, color_types)
                  : graphics_key(code_of[0], fs, attributes, color_types);
    if (mesh && !dev.mesh_shader && !dev.mesh_fallback) return nullptr;
    p->fallback = mesh && dev.uses_mesh_fallback(by_stage[6] != nullptr);
    p->mesh = mesh && !p->fallback;
    p->codes[0] = code[0], p->codes[1] = code[1], p->codes[2] = code[2];
    questions_in = &questions, questions_at = 0, answers_in = &answers;
    struct Reset { ~Reset() { questions_in = nullptr, answers_in = nullptr; } } reset;
    if (mesh) translate_mesh_into(*p, *blank, by_stage[6] ? &code_of[6] : nullptr, code_of[7], nullptr, fs, color_types);
    else translate_into(*p, *blank, &code_of[0], fs, nullptr, color_types);
    if (p->failed) {   // (upstream #2358: a failed variant's modules went nowhere; ours forget their note too)
        for (Stage& st : p->stages)
            if (st.module) stage_module_destroy(st.module);
        return nullptr;
    }
    std::lock_guard lk(shader_mu);
    auto& variants = programs[p->key];
    for (auto& v : variants)
        if (v->variant == p->variant && v->mesh == p->mesh && v->fallback == p->fallback && v->ready.load(std::memory_order_acquire) &&
            !v->failed) {
            for (Stage& st : p->stages)
                if (st.module) stage_module_destroy(st.module);
            return v.get();
        }
    variants.push_back(std::move(p));
    return variants.back().get();
}

const Program* program_find(uint64_t key, uint64_t variant) {
    std::lock_guard lk(shader_mu);
    auto it = programs.find(key);
    if (it == programs.end()) return nullptr;
    for (auto& p : it->second)
        if (p->variant == variant && p->ready.load(std::memory_order_acquire) && !p->failed) return p.get();
    return nullptr;
}

void shader_stats(int64_t* translated, int64_t* failed) {
    static int64_t hits_logged;
    if (st_cache_hits.load() >= hits_logged + 500) logf("vk: %lld programs from the program cache", (long long)(hits_logged = st_cache_hits.load()));
    *translated = st_translated, *failed = st_failed;
}
double shader_translate_ms() {
    return st_translate_ns.load() / 1e6;
}

void program_released(const nvn_prog* p) {
    if (!p) return;
    std::lock_guard lk(shader_mu);
    codes.erase(p->serial);
}

void program_pick_stats(int64_t* lock_ns, int64_t* codes_ns, int64_t* inline_n, int64_t* inline_ns) {
    *lock_ns = st_pick_lock_ns.load(std::memory_order_relaxed), *codes_ns = st_pick_codes_ns.load(std::memory_order_relaxed);
    *inline_n = st_pick_inline.load(std::memory_order_relaxed), *inline_ns = st_pick_inline_ns.load(std::memory_order_relaxed);
}

} // namespace mkwvk
