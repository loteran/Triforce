// Fixture of tests/test_texture_pool_ids.py: gpu/vk/vk_bank.cpp's texture and sampler registrations, cut from the source
// (their tables, register_texture_copy, register_sampler_copy, texture_of, sampler_of, the slot versions and
// texture_released), with the game's texture pool registered into them: 33,068 descriptors, ids 256 to 33,067.
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ---- the NVN layer's objects, as far as the registrations read them (gpu/render/rx_nvn.h)
struct nvn_texdesc { uint32_t width, height; };
struct nvn_tex { nvn_texdesc d; uint64_t gpu, serial; };
struct nvn_view { uint32_t base_level, format; };
struct nvn_samp_par { uint32_t minf, magf; float lod_max; };
static void logf(const char*, ...) {}

/* TABLES */

/* FUNCTIONS */

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s: failed at line %d: %s\n", name.c_str(), __LINE__, #x); return 1; } } while (0)

static nvn_tex tex_of_id(int id) { return {{(uint32_t)(id % 4096 + 1), (uint32_t)(id / 4096 + 1)}, 0x1040000000ull + (uint64_t)id * 0x10000, (uint64_t)id + 1000000}; }

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "";
    constexpr int kReserved = 256, kPool = 33068;   // (NVN's reserved descriptors; the game's texture pool: host/nvn_native.c logs it)
    nvn_tex t{};
    nvn_view v{};
    bool has_view = true;
    if (name == "the-games-pool") {
        // every descriptor of the pool, in the order a game might fill it: each id answers its own texture afterwards
        for (int id = kPool - 1; id >= kReserved; id--) {
            const nvn_tex mine = tex_of_id(id);
            const nvn_view view{(uint32_t)id, 7};
            register_texture_copy(id, mine, id & 1 ? &view : nullptr);
        }
        for (int id = kReserved; id < kPool; id++) {
            CHECK(texture_of(id, &t, &v, &has_view));
            CHECK(t.serial == (uint64_t)id + 1000000 && t.gpu == tex_of_id(id).gpu && has_view == (bool)(id & 1));
            CHECK(!has_view || v.base_level == (uint32_t)id);
            CHECK(texture_slot_version(id) == 1);
        }
        // (the ids NX2navi's table of 32,768 did not hold: 32,768 to 33,067 are 300 textures of their own, not slot 32,767's)
        CHECK(texture_of(32767, &t, &v, &has_view) && t.serial == 32767 + 1000000);
        CHECK(texture_of(32768, &t, &v, &has_view) && t.serial == 32768 + 1000000);
        CHECK(texture_of(33067, &t, &v, &has_view) && t.serial == 33067 + 1000000);
        CHECK(!texture_of(33068, &t, &v, &has_view) && !texture_of(255, &t, &v, &has_view));
    } else if (name == "the-handles-range") {
        // a handle's texture id is 20 bits: every id up to 0xFFFFF registers and answers, and past it nothing is taken
        for (int id : {1, 32767, 32768, 65535, 65536, 0x7FFFF, 0xFFFFF}) {
            register_texture_copy(id, tex_of_id(id), nullptr);
            CHECK(texture_of(id, &t, &v, &has_view) && t.serial == (uint64_t)id + 1000000 && !has_view);
            CHECK(texture_slot_version(id) == 1);
        }
        for (int id : {-1, 0x100000, 0x7FFFFFFF}) {
            register_texture_copy(id, tex_of_id(1), nullptr);
            CHECK(!texture_of(id, &t, &v, &has_view) && texture_slot_version(id) == 0);
        }
        CHECK(!texture_of(32766, &t, &v, &has_view));   // (a slot between registered ones, never registered)
    } else if (name == "registered-again") {
        // a slot past 32,767 registered again and its texture finalized: the slot's own version moves, no other slot's
        register_texture_copy(32767, tex_of_id(32767), nullptr);
        register_texture_copy(33000, tex_of_id(33000), nullptr);
        const uint64_t gen = registrations_generation();
        nvn_tex other = tex_of_id(5);
        other.serial = 77;
        register_texture_copy(33000, other, nullptr);
        CHECK(texture_slot_version(33000) == 2 && texture_slot_version(32767) == 1 && registrations_generation() == gen + 1);
        CHECK(texture_of(33000, &t, &v, &has_view) && t.serial == 77);
        texture_released(77);
        CHECK(texture_slot_version(33000) == 3 && texture_slot_version(32767) == 1);
        texture_released(33000 + 1000000);   // (the texture the slot named before: no slot names it now)
        CHECK(texture_slot_version(33000) == 3);
    } else if (name == "samplers") {
        // a handle's sampler id is 12 bits: 4,096 samplers, the pool size the game is told
        nvn_samp_par p{};
        for (int id : {0, 256, 4095}) {
            const nvn_samp_par mine{(uint32_t)id, 1, 1000.0f};
            register_sampler_copy(id, mine);
            CHECK(sampler_of(id, &p) && p.minf == (uint32_t)id && sampler_slot_version(id) == 1);
        }
        for (int id : {-1, 4096, 65536}) {
            register_sampler_copy(id, nvn_samp_par{9, 9, 9.0f});
            CHECK(!sampler_of(id, &p) && sampler_slot_version(id) == 0);
        }
    } else {
        std::fprintf(stderr, "unknown case %s\n", name.c_str());
        return 2;
    }
    return 0;
}
