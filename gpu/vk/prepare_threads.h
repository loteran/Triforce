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
//
// (Triforce, 11-18) And measured: on AMD's Windows driver for RDNA 1 and 2 (vk_vendor.h amd_older_compiler) the compiles do
// not add up. With an empty driver cache the preparation begins with the compute programs, up to 64 texture variants each,
// every worker in the driver's compiler at once (warm_compute): on an RX 6800 XT (24 processors) each of those compiles
// ran 6-8 times slower with 22 workers than with 6 (the 8x4x4 programs 17-37 s against 2.4-3.8 s, the 128x1x1 ones
// 120-145 s against 22 s), and the translation pass ended at minute 24 against minute 15; 12 workers ended its compute part
// a minute behind 6, 16 between 12 and 22. Neither the shared pipeline cache nor the shader modules were it. But the
// steps after it, the stage libraries and the precompile, mostly graphics pipelines, went slower on 6 (the libraries 2222 s
// against 2074 s on 23; the precompile about 35 min against 27): so the cap is the translation pass's alone (translate,
// vk_draw.cpp prepare_run), kAmdOlderMost at most on that driver; the switch still decides outright. 8 workers on every
// step (11-18) ended the translation at minute 15 as 6 did, the libraries in about 2010 s and the precompile in about
// 27 min as 23 did, title at 4640 s: 8 it is. The libraries kept only about 4 of the 8 busy, the precompile's last
// pipelines all 8: both left on count's
#pragma once

#include <algorithm>
#include <cstdint>

namespace prepare_threads {

/// The free RAM a worker past half the processors must find when the preparation starts
inline constexpr uint64_t kRamAWorker = 1ull << 30;

/// What it was, and what the preparation behind the game is counted from: half the processors, 1 to 32
inline unsigned half(unsigned cpus) { return std::clamp(cpus / 2, 1u, 32u); }

/// The most workers of the translation pass on AMD's Windows driver for RDNA 1 and 2 (above)
inline constexpr unsigned kAmdOlderMost = 8;

/// cpus: the logical processors; ram_free: the physical memory free now, 0 when it is not known; above_normal: the process
/// was started above normal priority; asked: RX_VK_PREPARE_THREADS, 0 or less when it is not set
inline unsigned count(unsigned cpus, uint64_t ram_free, bool above_normal, int asked) {
    if (asked > 0) return (unsigned)std::min(asked, 64);
    const unsigned least = half(cpus), most = std::clamp(cpus, 1u, 64u);
    if (above_normal || !ram_free) return least;
    return (unsigned)std::clamp<uint64_t>(ram_free / kRamAWorker, least, most);
}

/// The translation pass's workers (prepare_run): all, count's; on AMD's older driver kAmdOlderMost at most, unless the
/// switch asked for a number
inline unsigned translate(unsigned all, bool amd_older, bool asked) {
    return amd_older && !asked ? std::min(all, kAmdOlderMost) : all;
}

}   // namespace prepare_threads
