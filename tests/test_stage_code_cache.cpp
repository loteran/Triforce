// (Triforce) The stages' decoded code kept by their bytes (gpu/vk/stage_code_cache.h, vk_shader.cpp read_code). The runner,
// tests/test_stage_code_cache.py, cuts vk_shader.cpp's struct Code, hash_bytes, control_end, code_key and its stage code
// cache's part with read_code out as they are into stage_code_functions.inc; this file gives them a guest memory of authored
// stages and a parse_code that counts its calls. Each scenario is a process of its own: test_stage_code_cache <scenario>.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "stage_code_cache.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::fflush(stderr); std::_Exit(1); } } while (0)

namespace Shader {
struct ProgramHeader {
    uint8_t words[0x50];
};
} // namespace Shader

typedef struct { uint64_t data, control; } nvn_stage_data;

namespace mkwvk {
void logf(const char*, ...) {}
// (guest memory: authored control sections and code areas at addresses of the test's)
std::map<uint64_t, std::vector<uint8_t>> memory;
int gpu_read(uint64_t at, void* dst, uint64_t n) {
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
namespace {
#include "stage_code_functions.inc"   // (vk_shader.cpp's Code, hash_bytes, control_end, code_key, its cache and read_code)

// (parse_code: what the decoding makes of the bytes, a function of them alone; its calls counted)
std::atomic<int> parses{0};
Code parse_code(const uint8_t* ctl, const uint8_t* area, uint32_t end) {
    parses++;
    Code c;
    uint16_t st;
    std::memcpy(&st, ctl + 0x56, 2);
    c.key = code_key(ctl, area, end);
    c.stage = st;
    c.code.assign(area, area + end);   // (the "lowered" code: the bytes, so that a copy of another stage's shows)
    c.c1.assign(ctl, ctl + 16);
    c.ok = true;
    return c;
}
} // namespace
} // namespace mkwvk

using namespace mkwvk;

// ---- the header alone ----

struct Made {
    std::vector<uint8_t> of;   // (the bytes it was made of)
};
using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
Bytes bytes_of(std::initializer_list<uint8_t> b, size_t pad = 0) {
    auto v = std::make_shared<std::vector<uint8_t>>(b);
    v->resize(v->size() + pad, 0x5a);
    return v;
}
std::shared_ptr<const Made> made_of(const Bytes& b) { return std::make_shared<const Made>(Made{*b}); }
uint64_t same_hash(const uint8_t*, size_t) { return 42; }   // (every lookup the same: only the bytes tell entries apart)

void scenario_budget() {
    CHECK(stage_code_cache::budget(nullptr) == stage_code_cache::kDefaultMb << 20);
    CHECK(stage_code_cache::budget("") == stage_code_cache::kDefaultMb << 20);
    CHECK(stage_code_cache::budget("0") == 0);
    CHECK(stage_code_cache::budget("16") == (size_t)16 << 20);
    CHECK(stage_code_cache::budget("x") == stage_code_cache::kDefaultMb << 20);
    CHECK(stage_code_cache::budget("12x") == stage_code_cache::kDefaultMb << 20);
    CHECK(stage_code_cache::budget("-3") == stage_code_cache::kDefaultMb << 20);
    CHECK(stage_code_cache::budget("100000") == (size_t)4096 << 20);
}

void scenario_hit() {
    stage_code_cache::Cache<Made> cache(1 << 20);
    const Bytes a = bytes_of({1, 2, 3, 4}, 100), b = bytes_of({1, 2, 3, 5}, 100), a2 = bytes_of({1, 2, 3, 4}, 100);
    CHECK(!cache.find(*a));
    const auto ma = made_of(a);
    cache.put(a, ma, 10);
    CHECK(cache.find(*a2) == ma);   // (equal bytes, another buffer)
    CHECK(!cache.find(*b));         // (one byte differs)
    CHECK(!cache.find(*bytes_of({1, 2, 3, 4}, 99)));   // (a byte fewer)
    CHECK(!cache.find(*bytes_of({1, 2, 3, 4}, 101)));  // (a byte more)
    size_t n, used;
    cache.usage(&n, &used);
    CHECK(n == 1 && used == a->size() + 10);
    // (the lookup hash: the length and every byte count, the tail's too)
    const std::vector<uint8_t> z32(32, 0), z33(33, 0);
    CHECK(stage_code_cache::quick_hash(z32.data(), 32) != stage_code_cache::quick_hash(z33.data(), 33));
    std::vector<uint8_t> t(70, 7), u(70, 7);
    u[69] = 8;
    CHECK(stage_code_cache::quick_hash(t.data(), 70) != stage_code_cache::quick_hash(u.data(), 70));
    u[69] = 7, u[3] = 9;
    CHECK(stage_code_cache::quick_hash(t.data(), 70) != stage_code_cache::quick_hash(u.data(), 70));
}

void scenario_collide() {
    // (every entry under one lookup hash: each found only by its own bytes)
    stage_code_cache::Cache<Made, same_hash> cache(1 << 20);
    std::vector<Bytes> all;
    for (uint8_t i = 0; i < 8; i++) all.push_back(bytes_of({i, 0, 0, i}, 40));
    for (const Bytes& b : all) cache.put(b, made_of(b), 1);
    for (const Bytes& b : all) {
        const auto m = cache.find(*bytes_of({(*b)[0], 0, 0, (*b)[3]}, 40));
        CHECK(m && m->of == *b);
    }
    CHECK(!cache.find(*bytes_of({9, 0, 0, 9}, 40)));
    CHECK(!cache.find(*bytes_of({1, 0, 0, 2}, 40)));
}

void scenario_replace() {
    // (put again for equal bytes: what it was made of now is kept, once)
    stage_code_cache::Cache<Made> cache(1 << 20);
    const Bytes a = bytes_of({1}, 50), a2 = bytes_of({1}, 50);
    cache.put(a, made_of(a), 5);
    const auto later = std::make_shared<const Made>(Made{{9, 9}});
    cache.put(a2, later, 20);
    CHECK(cache.find(*a) == later);
    size_t n, used;
    cache.usage(&n, &used);
    CHECK(n == 1 && used == a->size() + 20);
}

void scenario_evict() {
    // (each entry 100 bytes and 100 of its own; room for three)
    stage_code_cache::Cache<Made> cache(600);
    const Bytes a = bytes_of({1}, 99), b = bytes_of({2}, 99), c = bytes_of({3}, 99), d = bytes_of({4}, 99), e = bytes_of({5}, 99);
    cache.put(a, made_of(a), 100), cache.put(b, made_of(b), 100), cache.put(c, made_of(c), 100);
    CHECK(cache.find(*a) && cache.find(*b) && cache.find(*c));   // (a used last but two, then b, then c)
    CHECK(cache.find(*a));                                       // (a most recently: b the least now)
    cache.put(d, made_of(d), 100);
    CHECK(!cache.find(*b));
    CHECK(cache.find(*a) && cache.find(*c) && cache.find(*d));
    size_t n, used;
    cache.usage(&n, &used);
    CHECK(n == 3 && used == 600);
    // (one alone over the budget: not kept, and nothing else is let go for it but what has to)
    cache.put(e, made_of(e), 1000);
    cache.usage(&n, &used);
    CHECK(!cache.find(*e) && n == 0 && used == 0);
}

void scenario_none() {
    // (RX_VK_STAGE_CODE_CACHE=0: nothing kept)
    stage_code_cache::Cache<Made> cache(0);
    const Bytes a = bytes_of({1}, 10);
    cache.put(a, made_of(a), 1);
    CHECK(!cache.find(*a));
    size_t n, used;
    cache.usage(&n, &used);
    CHECK(n == 0 && used == 0);
}

void scenario_threads() {
    // (finds and puts on four threads at once, a budget that lets go all the time: whatever is found is what its bytes made)
    stage_code_cache::Cache<Made> cache(4000);
    std::vector<Bytes> all;
    for (int i = 0; i < 64; i++) all.push_back(bytes_of({(uint8_t)i, (uint8_t)(i * 7)}, 200));
    std::atomic<int> found{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++)
        threads.emplace_back([&, t] {
            for (int k = 0; k < 20000; k++) {
                const Bytes& b = all[(k * 13 + t * 5) % all.size()];
                if (const auto m = cache.find(*b)) {
                    CHECK(m->of == *b);
                    found++;
                } else {
                    cache.put(b, made_of(b), 200);
                }
            }
        });
    for (auto& th : threads) th.join();
    CHECK(found > 0);
    size_t n, used;
    cache.usage(&n, &used);
    CHECK(used <= 4000);
}

// ---- vk_shader.cpp's read_code over guest memory ----

/// An authored stage at control / code: a control section (magic, code length, c1 at its end, stage) and an area of its own
void place(uint64_t control, uint64_t code, uint16_t stage, uint32_t end, uint8_t fill) {
    std::vector<uint8_t> ctl(kControlSize, 0), area(end, fill);
    const uint32_t magic = kControlMagic, len = end, c1o = end;
    std::memcpy(ctl.data(), &magic, 4);
    std::memcpy(ctl.data() + 0x38, &len, 4);
    std::memcpy(ctl.data() + 0x40, &c1o, 4);
    std::memcpy(ctl.data() + 0x44, &end, 4);
    std::memcpy(ctl.data() + 0x56, &stage, 2);
    for (uint32_t i = 0; i < end; i++) area[i] = (uint8_t)(fill + i * 31);
    memory[control] = ctl, memory[code] = area;
}
bool same(const Code& a, const Code& b) {
    return a.ok == b.ok && a.key == b.key && a.stage == b.stage && a.code == b.code && a.c1 == b.c1;
}

void scenario_read() {
    // (two program objects' stages at other addresses, the same bytes: decoded once, and the copy is the decoding's)
    place(0x1000, 0x10000, 0, 4096, 1);   // (vertex)
    place(0x2000, 0x20000, 1, 2048, 2);   // (fragment)
    place(0x3000, 0x30000, 0, 4096, 1);   // (the vertex stage's bytes again, elsewhere)
    const Code v = read_code({0x10000, 0x1000}), f = read_code({0x20000, 0x2000});
    CHECK(parses == 2 && v.ok && f.ok && v.key != f.key);
    const Code v2 = read_code({0x30000, 0x3000});
    CHECK(parses == 2);
    CHECK(same(v, v2));
    const uint8_t* ctl = memory[0x1000].data();
    CHECK(v2.key == code_key(ctl, memory[0x10000].data(), 4096));
    // (a byte of the code area changed: decoded again, its own)
    memory[0x30000][4000] ^= 1;
    const Code v3 = read_code({0x30000, 0x3000});
    CHECK(parses == 3 && v3.key != v.key && v3.code != v.code);
    // (a byte of the control section changed, the area as it was: decoded again)
    memory[0x30000][4000] ^= 1;
    memory[0x3000][0x20] ^= 1;
    const Code v4 = read_code({0x30000, 0x3000});
    CHECK(parses == 4 && v4.key != v.key);
    // (not a stage, or not there: nothing, nothing decoded, nothing kept)
    CHECK(!read_code({0x40000, 0x4000}).ok && !read_code({0, 0x1000}).ok && parses == 4);
    memory[0x5000] = memory[0x1000];
    CHECK(!read_code({0x50000, 0x5000}).ok && parses == 4);
}

void scenario_read_none() {
    // (RX_VK_STAGE_CODE_CACHE=0, set by the runner: each read decoded again, as before)
    place(0x1000, 0x10000, 0, 4096, 1);
    const Code a = read_code({0x10000, 0x1000}), b = read_code({0x10000, 0x1000});
    CHECK(parses == 2 && same(a, b));
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string s = argv[1];
    if (s == "budget") scenario_budget();
    else if (s == "hit") scenario_hit();
    else if (s == "collide") scenario_collide();
    else if (s == "replace") scenario_replace();
    else if (s == "evict") scenario_evict();
    else if (s == "none") scenario_none();
    else if (s == "threads") scenario_threads();
    else if (s == "read") scenario_read();
    else if (s == "read-none") scenario_read_none();
    else return 2;
    std::printf("%s: ok\n", s.c_str());
    return 0;
}
