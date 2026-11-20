// (Triforce) The stages read for the program picks before their code is lowered (vk_shader.cpp stage_head, read_code, codes_of,
// codes_whole, program_pick, mesh_program_locked, program_code_keys, compute_code_key; RX_VK_STAGE_CODE_LAZY). The runner,
// tests/test_stage_code_lazy.py, cuts them out as they are into stage_lazy_types.inc (the stage's code, its reading and the
// lists) and stage_lazy_picks.inc (the picks); this file gives them guest memory with authored stages, a parse_code that
// lowers a stage unless its first instruction byte is 0xEE (counting the lowerings), and the picks' surroundings: the program
// cache's lists with programs "on disk" the test names, translations recorded instead of made. Each scenario is a process of
// its own: test_stage_code_lazy <scenario>; "transcript" prints what each pick gave, for the runner to compare across switches.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <condition_variable>

#include "program_read_once.h"
#include "stage_code_cache.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::fflush(stderr); std::_Exit(1); } } while (0)
#define PROF_EXACT()

namespace Shader {
struct ProgramHeader {
    uint8_t words[0x50];
};
} // namespace Shader

typedef struct { uint64_t data, control; } nvn_stage_data;
typedef struct { uint32_t magic, nstage; nvn_stage_data sd[6]; uint64_t serial; } nvn_prog;

namespace mkwvk {
struct NvnState {
    bool has_prog = true;
    nvn_prog prog{};
    uint32_t vattr[16]{};
    int nvattr = 0;
};
enum class ProgramKind { Graphics, Compute, Mesh };
struct Program {
    uint64_t key = 0;
    bool compute = false, failed = false;
    std::atomic<bool> ready{false};
    bool mesh = false, fallback = false;
    uint64_t codes[4]{};
    std::string made;   // (the test's: the stages it was translated from, "" when it came from the cache)
};
void logf(const char*, ...) {}
/// shader_mu: a mutex that knows which thread holds it
struct Owned {
    std::mutex m;
    std::atomic<std::thread::id> owner{};
    void lock() {
        m.lock();
        owner = std::this_thread::get_id();
    }
    void unlock() {
        owner = std::thread::id();
        m.unlock();
    }
    bool mine() const { return owner.load() == std::this_thread::get_id(); }
};
Owned shader_mu;
program_read_once::Reads<Owned> draw_once(shader_mu);
// (a slow guest memory: a read of slow_at waits until the test lets it go, and says whether the reader held shader_mu)
std::mutex slow_mu;
std::condition_variable slow_cv;
uint64_t slow_at = ~0ull;
bool slow_open = false;
std::atomic<int> slow_in{0};
std::atomic<bool> slow_locked{false};
std::map<uint64_t, std::vector<uint8_t>> memory;
int gpu_read(uint64_t at, void* dst, uint64_t n) {
    if (at == slow_at) {
        slow_locked = shader_mu.mine();
        slow_in++;
        std::unique_lock lk(slow_mu);
        slow_cv.wait(lk, [] { return slow_open; });
    }
    for (const auto& [base, bytes] : memory)
        if (at >= base && at + n <= base + bytes.size()) {
            std::memcpy(dst, bytes.data() + (at - base), (size_t)n);
            return 1;
        }
    return 0;
}
struct Host {
    int (*gpu_read)(uint64_t, void*, uint64_t);
} host_s{gpu_read};
Host* host = &host_s;
struct Dev {
    bool mesh_shader = true, mesh_fallback = false;
    bool uses_mesh_fallback(bool) const { return false; }
} dev;
namespace {
// (vk_shader.cpp: constants, hash_bytes, struct Code, control_end to read_code, the lists, stages_read to codes_whole)
#include "stage_lazy_types.inc"

std::atomic<int> lowerings{0};
Code parse_code(const uint8_t* ctl, const uint8_t* area, uint32_t end) {
    Code c = stage_head(ctl, area, end);   // (as vk_shader.cpp's: the head, then the lowering)
    if (c.whole) return c;
    c.ok = false, c.whole = true;
    lowerings++;
    const uint32_t hdr = c.stage != 5 ? kAmpereHeader : 0;
    if (area[hdr] == 0xEE) return c;   // (does not lower)
    c.code.assign(end / 8, 0);
    std::memcpy(c.code.data(), area, end / 8 * 8);
    c.c1.assign(ctl, ctl + 8);
    c.ok = true;
    return c;
}

// (the program cache: what the picks asked of it, and the programs "on disk", by key)
std::vector<uint64_t> asked;
std::set<uint64_t> on_disk;
void program_cache_load(uint64_t key, std::vector<std::unique_ptr<Program>>& variants, bool = false) {
    asked.push_back(key);
    if (variants.empty() && on_disk.erase(key)) {
        auto p = std::make_unique<Program>();
        p->key = key;
        p->ready = true;
        variants.push_back(std::move(p));
    }
}
bool valid = true;   // (still_valid: false makes a variant of a program met before)
bool still_valid(const NvnState&, const Program&) { return valid; }
void miss_note(uint64_t, const char*, uint64_t, uint64_t, const NvnState&, const uint8_t*, size_t) {}
void log_program_stages(const NvnState&, const char*) {}
uint64_t graphics_key(const Code& vs, const Code* fs, const uint32_t*, const uint8_t*) { return vs.key ^ (fs ? fs->key * 31 : 0); }
uint64_t mesh_key(const Code* task, const Code& mesh, const Code* fs, const Code* prim, const uint8_t*) {
    return mesh.key * 7 ^ (task ? task->key * 11 : 0) ^ (fs ? fs->key * 13 : 0) ^ (prim ? prim->key * 17 : 0);
}
std::string made_of(std::initializer_list<const Code*> stages) {
    std::string out;
    for (const Code* c : stages) {
        char b[48];
        if (c) CHECK(c->ok && c->whole && !c->code.empty() == !c->header_only);   // (a translation's stages are lowered, all of them)
        std::snprintf(b, sizeof b, " %d:%016llx", c ? c->stage : -1, c ? (unsigned long long)c->key : 0ull);
        out += b;
    }
    return out;
}
void translate_into(Program& p, const NvnState&, const Code* vsc, const Code* fsc, const Code* csc, const uint8_t*) {
    p.made = made_of({vsc, fsc, csc});
    p.ready.store(true);
}
void translate_mesh_into(Program& p, const NvnState&, const Code* task, const Code& mesh, const Code* prim, const Code* fsc,
                         const uint8_t*) {
    p.made = made_of({task, &mesh, prim, fsc});
    p.ready.store(true);
}
void jobs_push(std::function<void()> fn, bool, std::atomic<bool>* ready) {
    fn();
    if (ready) ready->store(true);
}

#include "stage_lazy_picks.inc"   // (vk_shader.cpp: mesh_program_locked, program_code_keys, compute_code_key, program_pick)
} // namespace

const Program* mesh_pick(const NvnState& s, const uint8_t ct[8]) {
    std::lock_guard lk(shader_mu);
    return mesh_program_locked(s, ct);
}
} // namespace mkwvk

using namespace mkwvk;

// ---- authored stages ----

/// A stage at control / code: a control section (magic, length, c1 at the end, NVN stage) and its area, lowered unless bad (its
/// first instruction byte 0xEE); header: a graphics header alone
void place(uint64_t control, uint64_t code, uint16_t stage, uint8_t fill, bool bad = false, bool header = false) {
    const uint32_t hdr = stage != 5 ? kAmpereHeader : 0, end = header ? hdr + 16 : hdr + 512;
    std::vector<uint8_t> ctl(kControlSize, 0), area(end, 0);
    const uint32_t magic = kControlMagic, len = end, c1o = end;
    std::memcpy(ctl.data(), &magic, 4);
    std::memcpy(ctl.data() + 0x38, &len, 4);
    std::memcpy(ctl.data() + 0x40, &c1o, 4);
    std::memcpy(ctl.data() + 0x44, &end, 4);
    std::memcpy(ctl.data() + 0x56, &stage, 2);
    for (uint32_t i = 0; i < end; i++) area[i] = (uint8_t)(fill * 37 + i * 31 + stage);
    if (!header && area[hdr] == 0xEE) area[hdr] = 0xED;
    if (bad) area[hdr] = 0xEE;
    memory[control] = ctl, memory[code] = area;
}
uint64_t next_serial = 100;
NvnState program(std::initializer_list<std::pair<uint64_t, uint64_t>> stages) {   // ({code, control} each)
    NvnState s;
    s.prog.nstage = 0;
    for (const auto& [code, control] : stages) s.prog.sd[s.prog.nstage++] = {code, control};
    s.prog.serial = next_serial++;
    return s;
}
uint64_t key_at(uint64_t control, uint64_t code) {
    const auto& ctl = memory[control];
    uint32_t end;
    std::memcpy(&end, ctl.data() + 0x44, 4);
    return code_key(ctl.data(), memory[code].data(), end);
}
const uint8_t kTypes[8] = {};
std::string said(const char* what, const NvnState& s, const Program* p) {
    char b[64];
    std::snprintf(b, sizeof b, " %016llx", p ? (unsigned long long)p->key : 0ull);
    std::string out = std::string(what) + ": " + (p ? "program" : "none") + b;
    if (p && !p->made.empty()) out += " made of" + p->made;
    out += "; keys" + program_code_keys(s);   // (before compute_code_key, which lowers the stages too)
    std::snprintf(b, sizeof b, "; compute %016llx", (unsigned long long)compute_code_key(s));
    return out + b + "\n";
}

// ---- scenarios ----

void scenario_transcript() {
    // (what each pick gives, printed: the runner compares it with RX_VK_STAGE_CODE_LAZY on and off, the cache on and off)
    std::string out;
    place(0x1000, 0x10000, 0, 1), place(0x1100, 0x11000, 1, 2);                // (vertex, fragment: both lower)
    place(0x1200, 0x12000, 1, 3, true);                                         // (a fragment stage that does not lower)
    place(0x1300, 0x13000, 0, 4, true);                                         // (a vertex stage that does not lower)
    place(0x1400, 0x14000, 5, 5), place(0x1500, 0x15000, 5, 6, true);           // (compute: lowers, does not)
    place(0x1600, 0x16000, 6, 7, true), place(0x1700, 0x17000, 7, 8);           // (task: does not lower; mesh: lowers)
    place(0x1800, 0x18000, 1, 9, false, true);                                  // (a header alone)
    place(0x1900, 0x19000, 7, 10, true);                                        // (mesh: does not lower)
    place(0x1a00, 0x1a000, 2, 11, true);                                        // (geometry, not translated: does not lower)
    place(0x1b00, 0x1b000, 0, 12), place(0x1c00, 0x1c000, 1, 13);               // (a program the cache has)
    place(0x2000, 0x20000, 0, 1), place(0x2100, 0x21000, 1, 2);                 // (the first two stages' bytes, elsewhere)
    const NvnState a = program({{0x10000, 0x1000}, {0x11000, 0x1100}});
    out += said("vertex+fragment", a, program_pick(a, ProgramKind::Graphics, kTypes));
    const NvnState a2 = program({{0x20000, 0x2000}, {0x21000, 0x2100}});
    out += said("the same, another object", a2, program_pick(a2, ProgramKind::Graphics, kTypes));
    valid = false;   // (another variant of a program met before)
    out += said("another variant", a2, program_pick(a2, ProgramKind::Graphics, kTypes));
    valid = true;
    const NvnState b = program({{0x10000, 0x1000}, {0x12000, 0x1200}});
    out += said("fragment does not lower", b, program_pick(b, ProgramKind::Graphics, kTypes));
    out += said("again", b, program_pick(b, ProgramKind::Graphics, kTypes));
    const NvnState c = program({{0x13000, 0x1300}, {0x11000, 0x1100}});
    out += said("vertex does not lower", c, program_pick(c, ProgramKind::Graphics, kTypes));
    const NvnState d = program({{0x14000, 0x1400}});
    out += said("compute", d, program_pick(d, ProgramKind::Compute, kTypes));
    const NvnState e = program({{0x15000, 0x1500}});
    out += said("compute does not lower", e, program_pick(e, ProgramKind::Compute, kTypes));
    const NvnState f = program({{0x16000, 0x1600}, {0x17000, 0x1700}, {0x11000, 0x1100}, {0x18000, 0x1800}});
    out += said("mesh, task does not lower", f, mesh_pick(f, kTypes));
    const NvnState g = program({{0x19000, 0x1900}, {0x11000, 0x1100}});
    out += said("mesh does not lower", g, mesh_pick(g, kTypes));
    const NvnState h = program({{0x10000, 0x1000}, {0x1a000, 0x1a00}, {0x11000, 0x1100}});
    out += said("geometry does not lower", h, program_pick(h, ProgramKind::Graphics, kTypes));
    on_disk.insert(key_at(0x1b00, 0x1b000) ^ key_at(0x1c00, 0x1c000) * 31);
    const NvnState k = program({{0x1b000, 0x1b00}, {0x1c000, 0x1c00}});
    out += said("from the cache", k, program_pick(k, ProgramKind::Graphics, kTypes));
    const NvnState m = program({{0x17000, 0x1700}, {0x11000, 0x1100}, {0x18000, 0x1800}});
    out += said("mesh", m, mesh_pick(m, kTypes));
    std::fputs(out.c_str(), stdout);
}

void scenario_found() {
    // (a program object whose program the cache has: its stages are read, keyed and found, and none is lowered)
    place(0x1000, 0x10000, 0, 1), place(0x1100, 0x11000, 1, 2);
    on_disk.insert(key_at(0x1000, 0x10000) ^ key_at(0x1100, 0x11000) * 31);
    const NvnState a = program({{0x10000, 0x1000}, {0x11000, 0x1100}});
    const Program* p = program_pick(a, ProgramKind::Graphics, kTypes);
    CHECK(p && p->made.empty());
    CHECK(lowerings == 0);
    // (a variant to translate: its stages lowered then, once)
    valid = false;
    const Program* q = program_pick(a, ProgramKind::Graphics, kTypes);
    CHECK(q && q != p && !q->made.empty() && lowerings == 2);
    // (the same bytes in another object, its program met: neither keyed again by FNV nor lowered, a copy each)
    valid = true;
    place(0x2000, 0x20000, 0, 1), place(0x2100, 0x21000, 1, 2);
    const NvnState a2 = program({{0x20000, 0x2000}, {0x21000, 0x2100}});
    CHECK(program_pick(a2, ProgramKind::Graphics, kTypes) && lowerings == 2);
}

void scenario_reread() {
    // (a program object none of whose stages lowers: none; read again at the next pick, as before, and picked once they do)
    place(0x1000, 0x10000, 0, 1, true);
    const NvnState a = program({{0x10000, 0x1000}});
    CHECK(!program_pick(a, ProgramKind::Graphics, kTypes));
    place(0x1000, 0x10000, 0, 1);
    const Program* p = program_pick(a, ProgramKind::Graphics, kTypes);
    CHECK(p && !p->made.empty());
}

/// Until pred, at most seconds
template <class Pred> bool within(double seconds, Pred pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}
void slow_go() {
    {
        std::lock_guard lk(slow_mu);
        slow_open = true;
    }
    slow_cv.notify_all();
}

void scenario_unlocked() {
    // (a pick reading a program object's stages from a slow guest memory does not hold the shaders' lock: another pick of another
    // object goes on meanwhile)
    place(0x1000, 0x10000, 0, 1), place(0x1100, 0x11000, 1, 2);
    place(0x2000, 0x20000, 0, 3), place(0x2100, 0x21000, 1, 4);
    slow_at = 0x11000;
    const NvnState a = program({{0x10000, 0x1000}, {0x11000, 0x1100}}), b = program({{0x20000, 0x2000}, {0x21000, 0x2100}});
    const Program* pa = nullptr;
    std::atomic<const Program*> pb{nullptr};
    std::thread ta([&] { pa = program_pick(a, ProgramKind::Graphics, kTypes); });
    CHECK(within(10, [] { return slow_in.load() == 1; }));
    CHECK(!slow_locked);
    CHECK(draw_once.open() == 1);
    std::thread tb([&] { pb = program_pick(b, ProgramKind::Graphics, kTypes); });
    const bool b_done = within(10, [&] { return pb.load() != nullptr; });
    slow_go();
    ta.join(), tb.join();
    CHECK(b_done);
    CHECK(pa && pb.load() && pa != pb.load() && !pa->made.empty());
    CHECK(codes[a.prog.serial].size() == 2 && draw_once.open() == 0);
}

void scenario_gate() {
    // (a program's release waiting, the gate closed: the stages are read under the lock, as before)
    place(0x1000, 0x10000, 0, 1), place(0x1100, 0x11000, 1, 2);
    slow_at = 0x11000;
    const NvnState a = program({{0x10000, 0x1000}, {0x11000, 0x1100}});
    draw_once.close();
    const Program* pa = nullptr;
    std::thread ta([&] { pa = program_pick(a, ProgramKind::Graphics, kTypes); });
    CHECK(within(10, [] { return slow_in.load() == 1; }));
    CHECK(slow_locked);
    slow_go();
    ta.join();
    draw_once.reopen();
    CHECK(pa && !pa->made.empty());
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string s = argv[1];
    if (s == "transcript") scenario_transcript();
    else if (s == "found") scenario_found();
    else if (s == "reread") scenario_reread();
    else if (s == "unlocked") scenario_unlocked();
    else if (s == "gate") scenario_gate();
    else return 2;
    std::printf("%s: ok\n", s.c_str());
    return 0;
}
