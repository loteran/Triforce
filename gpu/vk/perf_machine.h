// (Triforce) The performance overlay's machine numbers (vk_menu.cpp draw_perf, the FPS counter row on Full): the GPU's
// load and temperature (no power: removed 11-17, the owner's call), the video memory the card has in use and in all, the CPU's load, temperature and this
// game's share of it, the system memory in use, in all and the game's, and the GPU's and the CPU's names. Sampled twice a
// second on a thread of their own (perf_machine.cpp) while the row is on Full, since some of the calls take milliseconds:
// the presenting thread only reads the latest sample. A number the machine does not give is -1, and the overlay shows "-"
// (an AMD or Intel card has no NVIDIA library, an old Radeon no PM log, a board no thermal zone, Linux no temperatures).
//
// (Triforce: from NX2navi runtime/perfmon.c, runtime/perfmon.h, runtime/platform/win32.c plat_cpu_load, plat_gpu_load and
// plat_mem_info, runtime/platform/linux.c and runtime/sysinfo.c sysinfo_cpu_name, at 62cc3b13.) That code contains code
// Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors, Copyright (c) 2026 Peiikai (MIT: the performance
// overlay of github.com/Revivedx/SavageWindRecomp, its NVML calls and sampling loop), and its AMD Display Library
// declarations Copyright (c) 2016 - 2022 Advanced Micro Devices, Inc. (MIT: github.com/GPUOpen-LibrariesAndSDKs/display-library).
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright notice and
// this permission notice shall be included in all copies or substantial portions of the Software. THE SOFTWARE IS
// PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// The parts below with no clock and no system call of their own (cpu_load, luid_name, engine_load, adapter_memory_mb,
// cell_text, local_heap_mb, name_match) are what tests/test_perf_overlay.cpp runs; it runs the sampler too, for a GPU of no
// vendor's name.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mkwvk::perf_machine {

struct Machine {
    char gpu_name[96] = "";   // (the GPU the game renders on, as Vulkan names it)
    char cpu_name[64] = "";   // (the CPU's brand string, CPUID)
    float gpu_pct = -1;       // (the busiest engine type of the rendering GPU, as Task Manager shows it; else NVML's or ADL's)
    float gpu_temp_c = -1;    // (NVML; ADL's edge temperature, else its hotspot)
    float vram_used_mb = -1;  // (the card's dedicated memory in use by every program: Windows' adapter counter, else NVML)
    float vram_total_mb = -1; // (the card's: NVML, else the device-local heaps Vulkan reports, added up)
    float gpu_clock_mhz = -1; // (the graphics clock: NVML, else ADL's; the VRAM row's third cell when the game's figure is unknown)
    float cpu_pct = -1;       // (the whole machine, percent of all logical processors, since the sample before)
    float cpu_game_pct = -1;  // (this process, the same percent)
    float cpu_temp_c = -1;    // (the hottest ACPI thermal zone: a zone near the CPU on most boards, not the package itself)
    float ram_used_mb = -1, ram_total_mb = -1, ram_game_mb = -1;   // (in use, installed, this process's working set)
    int samples = 0;          // (made since the sampler started: 0 until the first)
};

/// The GPU the game renders on, given when the sampler starts
struct Gpu {
    char name[96] = "";
    uint8_t luid[8] = {};     // (Vulkan's deviceLUID: Windows' counters name an adapter by it)
    bool has_luid = false;
    float vram_total_mb = -1;
};

/// Starts the sampler (the row went to Full) unless it runs; gpu is read then. log, or null: a line on what was found
/// (NVML, ADL, the counters), once a run
void start(const Gpu& gpu, void (*log)(const char* line));
/// Stops it (the row left Full): its thread ends at its next wake, within a moment
void stop();
/// The latest sample (all -1 before the first): a copy, under the sampler's lock for as long as the copy takes
Machine latest();
/// (for tests/test_perf_overlay.cpp) The sampler threads alive now: 1 while it runs, 0 once it has ended (a second start
/// while one runs makes no second)
int live_samplers();

// ---- the parts with no clock and no system call ----

/// The CPU's times as GetSystemTimes and GetProcessTimes give them (100 ns units; kernel includes idle), or /proc/stat's
/// (busy and total jiffies in kernel and idle's place) with the process's in the same unit
struct CpuTimes {
    uint64_t idle = 0, kernel = 0, user = 0, proc = 0;
};
/// The machine's and this process's share of all logical processors between two readings, percent; -1 when there is no
/// reading before (before.kernel 0) or no time went by
inline void cpu_load(const CpuTimes& before, const CpuTimes& now, float* total_pct, float* game_pct) {
    *total_pct = *game_pct = -1;
    const uint64_t total = (now.kernel - before.kernel) + (now.user - before.user);   // (kernel time includes idle time)
    if (!before.kernel || !total || now.kernel < before.kernel || now.user < before.user) return;
    const uint64_t idle = now.idle >= before.idle ? now.idle - before.idle : 0;
    *total_pct = 100.0f * (float)(total - std::min(total, idle)) / (float)total;
    *game_pct = 100.0f * (float)(now.proc >= before.proc ? now.proc - before.proc : 0) / (float)total;
}

/// The adapter's part of a counter's instance names, from Vulkan's deviceLUID (the LUID struct's memory: LowPart, then
/// HighPart): "luid_0x<high>_0x<low>", lower case
inline void luid_name(const uint8_t luid[8], char* out, size_t n) {
    uint32_t lo, hi;
    std::memcpy(&lo, luid, 4);
    std::memcpy(&hi, luid + 4, 4);
    snprintf(out, n, "luid_0x%08x_0x%08x", hi, lo);
}
/// The "luid_0x..._0x..." part of an instance name, lower case, into out (27 bytes at least); empty when it has none
inline void instance_luid(const char* name, char* out) {
    const char* p = std::strstr(name, "luid_");
    size_t n = 0;
    while (p && p[n] && n < 26) out[n] = (char)(p[n] >= 'A' && p[n] <= 'Z' ? p[n] + 32 : p[n]), n++;   // (luid_0x%08x_0x%08x: 26)
    out[n] = 0;
}

/// The GPU's load from "\GPU Engine(*)\Utilization Percentage": instances "pid_<n>_luid_0x.._0x.._phys_<n>_eng_<n>_engtype_<type>"
/// with each process's share of an engine. Per adapter and engine type the shares add up; the load is the busiest type,
/// 100 at most. luid: the adapter's (luid_name), or empty for any. -1 when no instance counts
inline float engine_load(const char* const* names, const double* values, int n, const char* luid) {
    enum { kTypes = 32 };
    struct { char luid[32], type[32]; double sum; } t[kTypes];
    int nt = 0;
    for (int i = 0; i < n; i++) {
        char lu[32];
        instance_luid(names[i], lu);
        if (luid[0] && std::strcmp(lu, luid)) continue;
        const char* ty = std::strstr(names[i], "engtype_");
        ty = ty ? ty + 8 : "";
        int k = 0;
        while (k < nt && (std::strcmp(t[k].luid, lu) || std::strncmp(t[k].type, ty, 31))) k++;
        if (k == nt) {
            if (nt == kTypes) continue;
            std::memcpy(t[k].luid, lu, sizeof lu);
            std::strncpy(t[k].type, ty, 31);
            t[k].type[31] = 0;
            t[k].sum = 0;
            nt++;
        }
        t[k].sum += values[i];
    }
    float load = -1;
    for (int k = 0; k < nt; k++)
        if (t[k].sum > load) load = (float)(t[k].sum > 100 ? 100 : t[k].sum);
    return load;
}
/// The adapter's dedicated memory in use from "\GPU Adapter Memory(*)\Dedicated Usage" (instances
/// "luid_0x.._0x.._phys_<n>", bytes), MB: the instances of luid added up (-1 when they come to nothing), or with luid
/// empty the adapter that uses the most (-1 when there is none)
inline float adapter_memory_mb(const char* const* names, const double* values, int n, const char* luid) {
    double sum = 0, best = -1;
    for (int i = 0; i < n; i++) {
        char lu[32];
        instance_luid(names[i], lu);
        if (luid[0] && !std::strcmp(lu, luid)) sum += values[i];
        if (values[i] > best) best = values[i];
    }
    if (luid[0] ? sum <= 0 : best < 0) return -1;
    return (float)((luid[0] ? sum : best) / 1048576.0);
}

/// A number of the overlay's as its panel writes it: fmt with x, or "-" when x is under 0 (the machine, its driver or the game
/// gives none: no NVIDIA or AMD library for this GPU, no thermal zone, no sample yet); true for "-", which the panel dims
/// (NX2navi's perf_panel.cpp cell)
inline bool cell_text(char* out, size_t n, const char* fmt, double x) {
    if (x < 0) {
        snprintf(out, n, "-");
        return true;
    }
    snprintf(out, n, fmt, x);
    return false;
}

/// The card's memory as Vulkan reports it, MB: the heaps with local_bit (VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) added up, the
/// system memory a card can also reach left out; -1 when there is none (NX2navi's perf_gpu_once). Heap: Vulkan's
/// VkMemoryHeap, or any struct with its size and flags
template <class Heap> float local_heap_mb(const Heap* heaps, uint32_t n, uint32_t local_bit) {
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < n; i++)
        if (heaps[i].flags & local_bit) bytes += heaps[i].size;
    return bytes ? (float)(bytes >> 20) : -1.0f;
}

/// Whether a vendor library's device name is the rendering GPU's: either within the other (NVIDIA's names match NVIDIA's
/// GPUs only), both not empty
inline bool name_match(const char* vulkan, const char* vendor) {
    return vulkan[0] && vendor[0] && (std::strstr(vulkan, vendor) || std::strstr(vendor, vulkan));
}

} // namespace mkwvk::perf_machine
