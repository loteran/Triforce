// (Triforce) One earlier set of the shader caches, kept for a change of a setting and put back when the setting goes back:
// this start's part. cache_set.h has why and the rule, cache_set_files.h what is done with the files; this gives them this
// start's stamp and switches, once, right after the device is made (vk_main.cpp mkwvk_init), before anything opens a cache.
// tests/test_cache_set.py
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "cache_set.h"
#include "cache_set_files.h"
#include "vk_api.h"
#include "vk_internal.h"
#include "vk_paths.h"
#include "vk_shader.h"

namespace mkwvk {
namespace {

std::string game_dir() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1);
}

uint64_t fnv(const void* d, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)d)[i]) * 1099511628211ull;
    return h;
}

/// Whether another Triforce has the program cache open (vk_shader.cpp disk_init's lock, held until its process ends): its
/// files are then left as they are
bool other_triforce(const std::string& programs) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const char ch : programs) h = (h ^ (uint8_t)tolower((unsigned char)ch)) * 1099511628211ull;
#ifdef _WIN32
    char name[64];
    snprintf(name, sizeof name, "Local\\TriforceProgramCache_%016llx", (unsigned long long)h);
    if (const HANDLE m = OpenMutexA(SYNCHRONIZE, FALSE, name)) {
        CloseHandle(m);
        return true;
    }
    return false;
#else
    const int fd = open((programs + ".lock").c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    const bool held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    if (!held) flock(fd, LOCK_UN);
    close(fd);
    return held;
#endif
}

/// What this start's stamp is made of, by part, for the record and the log (the decision takes the stamp and the build part
/// alone: program_cache_stamp, program_cache_build_part). The parts follow disk_stamp's inputs as far as they are seen here
cache_set::Record this_start() {
    cache_set::Record r;
    r.stamp = program_cache_stamp();
    r.build = program_cache_build_part();
    const uint32_t depth = host && host->depth_minus_w && host->depth_minus_w() != 0;
    r.parts.push_back({"depth", depth});
    const uint32_t mesh = (dev.mesh_shader ? 1u : 0u) | (dev.mesh_fallback ? 2u : 0u) | (dev.task_mesh_fallback ? 4u : 0u);
    r.parts.push_back({"mesh", mesh});
    const uint32_t lanes[] = {dev.subgroup_max, (uint32_t)dev.driver_id, (uint32_t)dev.sg32_stages,
                              (uint32_t)storage_binding_align(), getenv("RX_VK_WARP64") ? 1u : 0u};
    r.parts.push_back({"lanes", fnv(lanes, sizeof lanes)});
    const uint32_t gpu[] = {dev.props.vendorID, dev.props.deviceID};
    r.parts.push_back({"gpu", fnv(gpu, sizeof gpu)});
    const uint32_t vendor = (dev.fixes.fetch_bounds ? 1u : 0u) | (dev.fixes.nan_bits ? 2u : 0u) | (dev.fixes.gather_offset ? 4u : 0u);
    r.parts.push_back({"vendor", vendor});
    // (the Robust buffers row: with robust buffer access the translation leaves the buffer reads' bounds to it, vk_coopmat.cpp
    // fetch_bounds_buffers)
    r.parts.push_back({"robust", dev.features.robustBufferAccess ? 1u : 0u});
    uint64_t env = 0xCBF29CE484222325ull;
    for (char** e = _environ; e && *e; e++)
        for (const char* pre : {"RX_VK_OOT", "RX_VK_MMA", "RX_VK_NATIVE", "RX_VK_NAN", "RX_VK_TASK_CAP", "RX_VK_NETWORK", "RX_VK_GP_", "RX_VK_FOLLOW", "RX_VK_NO_FOLLOW", "RX_VK_NO_F16", "RX_VK_SHARED", "RX_VK_MESH", "RX_VK_NO_INVARIANT", "RX_VK_NO_NAN_GUARD", "RX_VK_MARKERS", "RX_VK_HANDLE_TABLE", "RX_VK_DLSS_CAPTURE", "EDEN_"})
            if (!strncmp(*e, pre, strlen(pre))) {
                env = fnv(*e, strlen(*e), env);
                const char* eq = strchr(*e, '=');
                if (!r.env.empty()) r.env += ",";
                r.env += eq ? std::string(*e, (size_t)(eq - *e)) : std::string(*e);
                break;
            }
    r.parts.push_back({"env", env});
    return r;
}

void cache_set_run() {
    const std::string dir = game_dir();
    if (other_triforce(dir + cache_set::kPieces[0])) {
        logf("vk: shader caches: another Triforce is using them: no set kept or put back");
        return;
    }
    // (no program cache: nothing says which set is there. Another build's kept set is still deleted)
    if (getenv("RX_VK_NO_PROGRAM_CACHE")) return cache_set::tidy(dir, program_cache_build_part(), logf);
    const cache_set::Record now = this_start();
    cache_set::Switches sw;
    sw.on = cache_set::on(getenv("RX_VK_CACHE_SET")) && !getenv("RX_VK_PCACHE_KEEP_OLD");
    sw.off_forgets = !cache_set::on(getenv("RX_VK_CACHE_SET"));
    cache_set::run(dir, now, sw, logf);
}

}  // namespace

void cache_set_init() {
    static std::once_flag once;
    std::call_once(once, [] { cache_set_run(); });
}

bool cache_set_states_pending() {
    if (getenv("RX_VK_NO_STATE_LIST")) return false;
    std::error_code ec;
    return std::filesystem::exists(game_dir() + cache_set::kStatesPending, ec) && !ec;
}

void cache_set_states_done() {
    std::error_code ec;
    std::filesystem::remove(game_dir() + cache_set::kStatesPending, ec);
}

}  // namespace mkwvk
