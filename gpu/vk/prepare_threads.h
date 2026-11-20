// (Triforce) How many workers the shader preparation before the title runs on (vk_draw.cpp: prepare_run, warm_libraries,
// state_list_prepare), apart from Vulkan so that tests/test_prepare_threads.py can run it.
//
// It was half the processors, 32 at most: "below normal, and half the CPUs: on all of them at High the driver's compiles
// froze the whole PC". That was a process started at High priority (scripts/run-vk.ps1 starts its test runs so): a thread
// below normal in such a process is still above every program of normal priority, and with one on each processor nothing
// else got a turn. A player's game is a process of normal priority, where a worker below normal gives way to everything
// else on the PC, and nothing of the game runs during the preparation: there the other half of the processors stood idle
// through a wait of minutes (165 s with 12 workers on a 24-thread PC). NX2navi prepares on every processor it is given.
//
// So: every processor, 64 at most, unless the process was started above normal priority (half, as before). Nothing
// bounded the memory a worker takes: each holds one program's translation at a time and the driver's compiler for one
// pipeline, and how much the driver's part is has not been measured here (the compute programs go first, so every worker
// is in the driver's compiler at once when the preparation begins). So the workers past half are given only what RAM is
// free when the preparation starts, 1 GB each; with the RAM not known, half. Never fewer than half: what it was.
// RX_VK_PREPARE_THREADS=<n> sets the number outright, 1 to 64.
#pragma once

#include <algorithm>
#include <cstdint>

namespace prepare_threads {

/// The free RAM a worker past half the processors must find when the preparation starts
inline constexpr uint64_t kRamAWorker = 1ull << 30;

/// What it was, and what the preparation behind the game is counted from: half the processors, 1 to 32
inline unsigned half(unsigned cpus) { return std::clamp(cpus / 2, 1u, 32u); }

/// cpus: the logical processors; ram_free: the physical memory free now, 0 when it is not known; above_normal: the process
/// was started above normal priority; asked: RX_VK_PREPARE_THREADS, 0 or less when it is not set
inline unsigned count(unsigned cpus, uint64_t ram_free, bool above_normal, int asked) {
    if (asked > 0) return (unsigned)std::min(asked, 64);
    const unsigned least = half(cpus), most = std::clamp(cpus, 1u, 64u);
    if (above_normal || !ram_free) return least;
    return (unsigned)std::clamp<uint64_t>(ram_free / kRamAWorker, least, most);
}

}   // namespace prepare_threads
