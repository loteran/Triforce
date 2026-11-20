// (Triforce) The performance overlay's machine numbers (perf_machine.h): a thread that samples them twice a second while
// the FPS counter row is on Full, and stops when it leaves Full. The OS's counters for the CPU, the memory and the GPU's
// load (Windows: GetSystemTimes, GetProcessTimes, GlobalMemoryStatusEx, K32GetProcessMemoryInfo, and PDH for the thermal
// zones and the GPU's engines and memory, as Task Manager reads them; Linux: /proc), and the GPU vendor's driver library
// for its temperature: NVML on NVIDIA, ADL's PM log on AMD, both loaded at run time from the system's folders
// and kept for the process while they have the GPU. Nothing of it is linked: on a PC without them (an Intel GPU, an old driver) their numbers stay
// -1 and the overlay shows "-"; a vendor library with no device of the rendering GPU is shut down and let go at once.
// Nothing here runs on the presenting thread but start, stop and latest, which only take the lock (start makes the
// thread); the thread runs at the lowest priority.
//
// (Triforce: from NX2navi runtime/perfmon.c (nvml_open, nvml_sample, adl_open, adl_sample, sampler, perfmon_mode),
// runtime/platform/win32.c (pdh_load, pdh_query, pdh_items, plat_load_open, plat_cpu_load, plat_gpu_load, plat_mem_info),
// runtime/platform/linux.c (plat_cpu_load, plat_mem_info) and runtime/sysinfo.c (sysinfo_cpu_name), at 62cc3b13.) Contains
// code Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors, Copyright (c) 2026 Peiikai (MIT), and AMD
// Display Library declarations (AdapterInfo, ADLSingleSensorData, ADLPMLogDataOutput, the ADL_PMLOG_* sensor numbers and
// the ADL2_* signatures) Copyright (c) 2016 - 2022 Advanced Micro Devices, Inc. (MIT): the notice is in perf_machine.h.
#include "perf_machine.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <intrin.h>
#define PERF_CPUID_MS 1
#endif
#else
#include <dlfcn.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#define PERF_CPUID_GNU 1
#endif
#endif

namespace mkwvk::perf_machine {
namespace {

void (*g_log)(const char*) = nullptr;
void note(const char* line) {
    if (g_log) g_log(line);
}

// ---- a library some PCs lack (a GPU vendor's): a bare name from the system's folders only
void* lib_open(const char* name) {
#ifdef _WIN32
    const bool bare = !std::strchr(name, '\\') && !std::strchr(name, '/');
    return (void*)LoadLibraryExA(name, nullptr, bare ? LOAD_LIBRARY_SEARCH_SYSTEM32 : 0);
#else
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}
void* lib_sym(void* lib, const char* name) {
#ifdef _WIN32
    return lib ? (void*)GetProcAddress((HMODULE)lib, name) : nullptr;
#else
    return lib ? dlsym(lib, name) : nullptr;
#endif
}
/// A library that has nothing for the rendering GPU, let go (NVML or ADL on a PC whose game renders on another card)
void lib_close(void* lib) {
#ifdef _WIN32
    if (lib) FreeLibrary((HMODULE)lib);
#else
    if (lib) dlclose(lib);
#endif
}

// ---- the CPU's name: CPUID's brand string, without the spaces around it
void cpu_brand(char* out, size_t n) {
    out[0] = 0;
    unsigned r[12] = {};
#if defined(PERF_CPUID_MS)
    int top[4];
    __cpuid(top, (int)0x80000000);
    if ((unsigned)top[0] < 0x80000004u) return;
    for (int i = 0; i < 3; i++) __cpuid((int*)&r[i * 4], (int)(0x80000002u + (unsigned)i));
#elif defined(PERF_CPUID_GNU)
    if (__get_cpuid_max(0x80000000, nullptr) < 0x80000004u) return;
    for (unsigned i = 0; i < 3; i++) __get_cpuid(0x80000002u + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
#else
    (void)n;
    return;
#endif
    char b[49];
    std::memcpy(b, r, 48);
    b[48] = 0;
    const char* p = b;
    while (*p == ' ') p++;
    snprintf(out, n, "%s", p);
    for (char* e = out + std::strlen(out); e > out && e[-1] == ' '; e--) e[-1] = 0;
}

// ---- NVML (NVIDIA): its structs as the library lays them out, no NVIDIA header
struct NvmlUtil { unsigned gpu, memory; };
struct NvmlMem { unsigned long long total, free, used; };
struct Nvml {
    bool tried = false, ok = false;
    void* dev = nullptr;
    int (*util)(void*, NvmlUtil*) = nullptr;
    int (*temp)(void*, int, unsigned*) = nullptr;
    int (*mem)(void*, NvmlMem*) = nullptr;
    int (*clock)(void*, int, unsigned*) = nullptr;
} nv;

template <class F> F sym(void* lib, const char* name) { return (F)lib_sym(lib, name); }

/// Once, on the sampler's thread: the device whose name is the rendering GPU's, else none (a PC that renders on another
/// vendor's GPU: NVML's numbers would be another card's). With none NVML is shut down and let go: it is kept only for the
/// device it samples
void nvml_open(const char* gpu) {
    nv.tried = true;
#ifdef _WIN32
    static const char* const names[] = {"nvml.dll", "C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll"};
#else
    static const char* const names[] = {"libnvidia-ml.so.1"};
#endif
    void* lib = nullptr;
    for (const char* n : names)
        if (!lib) lib = lib_open(n);
    if (!lib) return;
    const auto init = sym<int (*)()>(lib, "nvmlInit_v2");
    const auto count = sym<int (*)(unsigned*)>(lib, "nvmlDeviceGetCount_v2");
    const auto handle = sym<int (*)(unsigned, void**)>(lib, "nvmlDeviceGetHandleByIndex_v2");
    const auto name = sym<int (*)(void*, char*, unsigned)>(lib, "nvmlDeviceGetName");
    nv.util = sym<int (*)(void*, NvmlUtil*)>(lib, "nvmlDeviceGetUtilizationRates");
    nv.temp = sym<int (*)(void*, int, unsigned*)>(lib, "nvmlDeviceGetTemperature");
    nv.mem = sym<int (*)(void*, NvmlMem*)>(lib, "nvmlDeviceGetMemoryInfo");
    // ((Triforce, 11-17) no power reading (nvmlDeviceGetPowerUsage): the owner's call, power polling is reported to cost
    // frames and stutter on some cards; the overlay's GPU row has no watts)
    nv.clock = sym<int (*)(void*, int, unsigned*)>(lib, "nvmlDeviceGetClockInfo");
    const auto shutdown = sym<int (*)()>(lib, "nvmlShutdown");
    const bool up = init && count && handle && name && init() == 0;
    unsigned n = 0;
    if (up && count(&n) == 0)
        for (unsigned i = 0; i < n && !nv.dev; i++) {
            void* d = nullptr;
            char nm[96] = "";
            if (handle(i, &d) != 0 || name(d, nm, sizeof nm) != 0) continue;
            if (name_match(gpu, nm)) nv.dev = d;
        }
    nv.ok = nv.dev != nullptr;
    if (up) note(nv.ok ? "NVML has the rendering GPU: its temperature" : "NVML has no device of the rendering GPU's name");
    if (nv.ok) return;
    if (up && shutdown) shutdown();   // (nvmlInit's own count of users given back, then the library)
    lib_close(lib);
    nv.util = nullptr, nv.temp = nullptr, nv.mem = nullptr, nv.clock = nullptr;
}
void nvml_sample(Machine& m) {
    if (!nv.ok) return;
    NvmlUtil u;
    NvmlMem mem;
    unsigned v;
    if (m.gpu_pct < 0 && nv.util && nv.util(nv.dev, &u) == 0) m.gpu_pct = (float)u.gpu;   // (the system's counters first)
    if (nv.mem && nv.mem(nv.dev, &mem) == 0) {
        if (m.vram_used_mb < 0) m.vram_used_mb = (float)(mem.used >> 20);
        m.vram_total_mb = (float)(mem.total >> 20);
    }
    if (nv.temp && nv.temp(nv.dev, 0 /* NVML_TEMPERATURE_GPU */, &v) == 0) m.gpu_temp_c = (float)v;
    if (nv.clock && nv.clock(nv.dev, 0 /* NVML_CLOCK_GRAPHICS */, &v) == 0) m.gpu_clock_mhz = (float)v;
}

// ---- ADL (AMD): the PM log the driver keeps for Radeon RX 5000 and later (the numbers Adrenalin's overlay shows); an
// older GPU answers with an error and its numbers stay -1. Windows only
#ifdef _WIN32
constexpr int kAdlMaxPath = 256, kAdlSensors = 256;
enum {
    ADL_PMLOG_CLK_GFXCLK = 1,
    ADL_PMLOG_TEMPERATURE_EDGE = 8,
    ADL_PMLOG_INFO_ACTIVITY_GFX = 19,
    ADL_PMLOG_TEMPERATURE_HOTSPOT = 27,
};
struct AdlAdapterInfo {   // (AdapterInfo, the Windows layout)
    int iSize, iAdapterIndex;
    char strUDID[kAdlMaxPath];
    int iBusNumber, iDeviceNumber, iFunctionNumber, iVendorID;
    char strAdapterName[kAdlMaxPath], strDisplayName[kAdlMaxPath];
    int iPresent, iExist;
    char strDriverPath[kAdlMaxPath], strDriverPathExt[kAdlMaxPath], strPNPString[kAdlMaxPath];
    int iOSDisplayIndex;
};
struct AdlSensor { int supported, value; };
struct AdlPmLog { int size; AdlSensor sensors[kAdlSensors]; };
typedef void*(__stdcall* AdlAlloc)(int);
struct Adl {
    bool tried = false, ok = false;
    int index = 0;
    void* ctx = nullptr;
    int (*pmlog)(void*, int, AdlPmLog*) = nullptr;
    AdlPmLog log;   // (the sampler's buffer)
} adl;
void* __stdcall adl_alloc(int n) { return std::malloc((size_t)n); }

/// Once, on the sampler's thread, when NVML has not the GPU: the adapter of the rendering GPU's name whose PM log answers
/// (ADL lists one index per display output of a GPU). With none ADL's context is destroyed and the library let go
void adl_open(const char* gpu) {
    adl.tried = true;
    void* lib = lib_open("atiadlxx.dll");
    if (!lib) return;
    const auto create = sym<int (*)(AdlAlloc, int, void**)>(lib, "ADL2_Main_Control_Create");
    const auto destroy = sym<int (*)(void*)>(lib, "ADL2_Main_Control_Destroy");
    const auto count = sym<int (*)(void*, int*)>(lib, "ADL2_Adapter_NumberOfAdapters_Get");
    const auto info = sym<int (*)(void*, AdlAdapterInfo*, int)>(lib, "ADL2_Adapter_AdapterInfo_Get");
    adl.pmlog = sym<int (*)(void*, int, AdlPmLog*)>(lib, "ADL2_New_QueryPMLogData_Get");
    int n = 0;
    const bool up = create && count && info && adl.pmlog && create(adl_alloc, 1, &adl.ctx) == 0;
    AdlAdapterInfo* a = up && count(adl.ctx, &n) == 0 && n > 0 && n <= 256 ? (AdlAdapterInfo*)std::calloc((size_t)n, sizeof *a) : nullptr;
    if (a) {
        for (int i = 0; i < n; i++) a[i].iSize = sizeof *a;
        if (info(adl.ctx, a, n * (int)sizeof *a) == 0) {
            for (int i = 0; i < n && !adl.ok; i++) {
                if (a[i].iVendorID != 1002 || !name_match(gpu, a[i].strAdapterName)) continue;   // (ADL writes 0x1002 as decimal 1002)
                std::memset(&adl.log, 0, sizeof adl.log);
                adl.log.size = sizeof adl.log;
                if (adl.pmlog(adl.ctx, a[i].iAdapterIndex, &adl.log) != 0) continue;
                adl.index = a[i].iAdapterIndex;
                adl.ok = true;
            }
        }
        std::free(a);
    }
    if (up) note(adl.ok ? "ADL has the rendering GPU's PM log: its temperature" : "ADL has no PM log for the rendering GPU");
    if (adl.ok) return;
    if (up && destroy) destroy(adl.ctx);   // (the context ADL2_Main_Control_Create made, then the library)
    adl.ctx = nullptr, adl.pmlog = nullptr;
    lib_close(lib);
}
void adl_sample(Machine& m) {
    if (!adl.ok) return;
    std::memset(&adl.log, 0, sizeof adl.log);
    adl.log.size = sizeof adl.log;
    if (adl.pmlog(adl.ctx, adl.index, &adl.log) != 0) return;
    const AdlSensor* v = adl.log.sensors;
    if (m.gpu_pct < 0 && v[ADL_PMLOG_INFO_ACTIVITY_GFX].supported) m.gpu_pct = (float)v[ADL_PMLOG_INFO_ACTIVITY_GFX].value;
    if (v[ADL_PMLOG_TEMPERATURE_EDGE].supported) m.gpu_temp_c = (float)v[ADL_PMLOG_TEMPERATURE_EDGE].value;
    else if (v[ADL_PMLOG_TEMPERATURE_HOTSPOT].supported) m.gpu_temp_c = (float)v[ADL_PMLOG_TEMPERATURE_HOTSPOT].value;
    // ((Triforce, 11-17) the card's power draw is not read: see nvml_open)
    if (v[ADL_PMLOG_CLK_GFXCLK].supported) m.gpu_clock_mhz = (float)v[ADL_PMLOG_CLK_GFXCLK].value;
}
#else
struct Adl { bool tried = false, ok = false; } adl;
void adl_open(const char*) { adl.tried = true; }
void adl_sample(Machine&) {}
#endif

// ---- the OS's counters
#ifdef _WIN32
typedef PDH_STATUS(WINAPI* PdhOpenQueryW_t)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
typedef PDH_STATUS(WINAPI* PdhAddEnglishCounterW_t)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
typedef PDH_STATUS(WINAPI* PdhCollectQueryData_t)(PDH_HQUERY);
typedef PDH_STATUS(WINAPI* PdhGetFormattedCounterArrayW_t)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);
typedef PDH_STATUS(WINAPI* PdhCloseQuery_t)(PDH_HQUERY);
struct Pdh {
    PdhOpenQueryW_t open = nullptr;   // (set only when every one of them is there)
    PdhAddEnglishCounterW_t add = nullptr;
    PdhCollectQueryData_t collect = nullptr;
    PdhGetFormattedCounterArrayW_t get = nullptr;
    PdhCloseQuery_t close = nullptr;
};
const Pdh& pdh() {   // (pdh.dll, loaded at the first use, kept)
    static const Pdh p = [] {
        Pdh r;
        void* l = lib_open("pdh.dll");
        if (!l) return r;
        r.add = sym<PdhAddEnglishCounterW_t>(l, "PdhAddEnglishCounterW");
        r.collect = sym<PdhCollectQueryData_t>(l, "PdhCollectQueryData");
        r.get = sym<PdhGetFormattedCounterArrayW_t>(l, "PdhGetFormattedCounterArrayW");
        r.close = sym<PdhCloseQuery_t>(l, "PdhCloseQuery");
        const auto open = sym<PdhOpenQueryW_t>(l, "PdhOpenQueryW");
        if (r.add && r.collect && r.get && r.close) r.open = open;
        return r;
    }();
    return p;
}
/// A PDH query with its wildcard counters ("\Object(*)\Counter"); q null when it could not be opened
struct PdhQuery {
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER c[2] = {nullptr, nullptr};
    void open(const wchar_t* c0, const wchar_t* c1) {
        const Pdh& p = pdh();
        if (!p.open || p.open(nullptr, 0, &q) != ERROR_SUCCESS) {
            q = nullptr;
            return;
        }
        if (p.add(q, c0, 0, &c[0]) != ERROR_SUCCESS) c[0] = nullptr;
        if (!c1 || p.add(q, c1, 0, &c[1]) != ERROR_SUCCESS) c[1] = nullptr;
        if (!c[0] && !c[1]) close();
    }
    void close() {
        if (q) pdh().close(q);
        q = nullptr;
    }
};
/// A counter's valid instances: their names (narrowed: the instance names are ASCII) and values. False when there is no
/// valid data yet (a rate counter needs two collections)
struct Items {
    std::vector<std::string> names;
    std::vector<const char*> ptrs;
    std::vector<double> values;
};
bool pdh_items(PDH_HCOUNTER c, Items& out) {
    out.names.clear(), out.ptrs.clear(), out.values.clear();
    DWORD size = 0, count = 0;
    const Pdh& p = pdh();
    if (!c || p.get(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) != (PDH_STATUS)PDH_MORE_DATA || !size) return false;
    std::vector<unsigned char> buf(size);
    auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
    if (p.get(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS) return false;
    for (DWORD i = 0; i < count; i++) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
        std::string n;
        for (const wchar_t* w = items[i].szName; w && *w; w++) n += (char)(*w < 128 ? *w : '?');
        out.names.push_back(std::move(n));
        out.values.push_back(items[i].FmtValue.doubleValue);
    }
    for (const std::string& n : out.names) out.ptrs.push_back(n.c_str());
    return true;
}
uint64_t ft64(FILETIME f) { return (uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime; }

struct Sources {
    CpuTimes cpu;              // (the sample before's)
    PdhQuery thermal, gpu;     // (gpu: the engines' utilisation, the adapters' dedicated memory)
    char luid[32] = "";        // (the rendering GPU's, luid_name; empty: any)
    Items items;
};
void sources_open(Sources& s, const Gpu& g) {
    if (g.has_luid) luid_name(g.luid, s.luid, sizeof s.luid);
    s.thermal.open(L"\\Thermal Zone Information(*)\\Temperature", nullptr);
    s.gpu.open(L"\\GPU Engine(*)\\Utilization Percentage", L"\\GPU Adapter Memory(*)\\Dedicated Usage");
    char line[160];
    snprintf(line, sizeof line, "Windows' counters: GPU engines %s (%s), thermal zones %s", s.gpu.q ? "open" : "not there",
             s.luid[0] ? s.luid : "any adapter", s.thermal.q ? "open" : "not there");
    note(line);
}
void sources_close(Sources& s) { s.thermal.close(), s.gpu.close(); }
void sample_os(Sources& s, Machine& m) {
    FILETIME idle, kern, user, cr, ex, pk, pu;
    if (GetSystemTimes(&idle, &kern, &user) && GetProcessTimes(GetCurrentProcess(), &cr, &ex, &pk, &pu)) {
        CpuTimes now;
        now.idle = ft64(idle), now.kernel = ft64(kern), now.user = ft64(user), now.proc = ft64(pk) + ft64(pu);
        cpu_load(s.cpu, now, &m.cpu_pct, &m.cpu_game_pct);
        s.cpu = now;
    }
    const Pdh& p = pdh();
    // (the firmware's ACPI zones, near the CPU on most boards: the hottest under 150 degrees, from kelvin)
    if (s.thermal.q && p.collect(s.thermal.q) == ERROR_SUCCESS && pdh_items(s.thermal.c[0], s.items))
        for (double k : s.items.values) {
            const double c = k - 273.15;
            if (c > m.cpu_temp_c && c < 150) m.cpu_temp_c = (float)c;
        }
    if (s.gpu.q && p.collect(s.gpu.q) == ERROR_SUCCESS) {
        if (pdh_items(s.gpu.c[0], s.items))
            m.gpu_pct = engine_load(s.items.ptrs.data(), s.items.values.data(), (int)s.items.values.size(), s.luid);
        if (pdh_items(s.gpu.c[1], s.items))
            m.vram_used_mb = adapter_memory_mb(s.items.ptrs.data(), s.items.values.data(), (int)s.items.values.size(), s.luid);
    }
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys) {
        m.ram_total_mb = (float)(ms.ullTotalPhys >> 20);
        m.ram_used_mb = (float)((ms.ullTotalPhys - ms.ullAvailPhys) >> 20);
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) m.ram_game_mb = (float)(pmc.WorkingSetSize >> 20);
}
#else
struct Sources {
    CpuTimes cpu;   // (/proc/stat's busy jiffies in kernel's place with idle 0, its total in user's; the process's in jiffies)
};
void sources_open(Sources&, const Gpu&) {}
void sources_close(Sources&) {}
void sample_os(Sources& s, Machine& m) {
    unsigned long long v[10] = {};
    int n = 0;
    if (FILE* f = std::fopen("/proc/stat", "r")) {
        n = std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                        &v[6], &v[7], &v[8], &v[9]);
        std::fclose(f);
    }
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    const long tick = sysconf(_SC_CLK_TCK);
    if (n >= 8 && tick > 0) {
        uint64_t total = 0;
        for (int i = 0; i < 8; i++) total += v[i];   // (guest time is already inside user time)
        const uint64_t idle = v[3] + v[4];
        CpuTimes now;   // (cpu_load's terms: kernel + user the total, idle the idle)
        now.kernel = total, now.user = 0, now.idle = idle;
        now.proc = (uint64_t)(((double)ts.tv_sec + ts.tv_nsec / 1e9) * (double)tick);
        cpu_load(s.cpu, now, &m.cpu_pct, &m.cpu_game_pct);
        s.cpu = now;
    }
    if (FILE* f = std::fopen("/proc/meminfo", "r")) {
        char line[128];
        unsigned long long total = 0, avail = 0, k;
        while (std::fgets(line, sizeof line, f)) {
            if (std::sscanf(line, "MemTotal: %llu kB", &k) == 1) total = k;
            else if (std::sscanf(line, "MemAvailable: %llu kB", &k) == 1) avail = k;
        }
        std::fclose(f);
        if (total) m.ram_total_mb = (float)(total >> 10), m.ram_used_mb = (float)((total - std::min(total, avail)) >> 10);
    }
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        unsigned long long size, resident;
        if (std::fscanf(f, "%llu %llu", &size, &resident) == 2)
            m.ram_game_mb = (float)((resident * (uint64_t)sysconf(_SC_PAGESIZE)) >> 20);
        std::fclose(f);
    }
}
#endif

// ---- the sampler: runs while want is set, twice a second
struct State {
    std::mutex mu;   // (everything here)
    std::condition_variable cv;
    bool want = false, alive = false;
    int live = 0;    // (the sampler threads made and not yet ended: live_samplers)
    Gpu gpu;
    Machine latest;
};
State& st() {   // (never destroyed: a sampler still asleep at the process's end holds its lock)
    static State* s = new State;
    return *s;
}
void clear(Machine& m) {   // (the numbers, not the names)
    Machine e;
    std::memcpy(e.gpu_name, m.gpu_name, sizeof e.gpu_name);
    std::memcpy(e.cpu_name, m.cpu_name, sizeof e.cpu_name);
    e.samples = m.samples;
    m = e;
}

/// The sampler's thread below the game's own (the replay's and the presenting threads): its counters can wait in the driver
void lowest_priority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__linux__)
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);   // (Linux: a nice of a thread's own, by its id)
#endif
}

void sampler() {
    lowest_priority();
    State& s = st();
    Gpu gpu;
    {
        std::lock_guard lk(s.mu);
        gpu = s.gpu;
    }
    Machine m;
    snprintf(m.gpu_name, sizeof m.gpu_name, "%s", gpu.name);
    cpu_brand(m.cpu_name, sizeof m.cpu_name);
    if (!nv.tried) nvml_open(gpu.name);
    if (!nv.ok && !adl.tried) adl_open(gpu.name);
    Sources src;
    sources_open(src, gpu);
    std::unique_lock lk(s.mu);
    while (s.want) {
        lk.unlock();
        clear(m);
        sample_os(src, m);
        if (gpu.vram_total_mb > 0) m.vram_total_mb = gpu.vram_total_mb;
        nvml_sample(m);
        adl_sample(m);
        m.samples++;
        lk.lock();
        s.latest = m;
        if (s.want) s.cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return !s.want; });
    }
    s.alive = false;   // (in the same locked part as the last look at want: a start after it makes a new thread)
    lk.unlock();
    sources_close(src);
    lk.lock();
    s.live--;          // (this thread's last step: live_samplers)
}

} // namespace

void start(const Gpu& gpu, void (*log)(const char* line)) {
    State& s = st();
    std::lock_guard lk(s.mu);
    s.want = true;
    if (s.alive) return;
    g_log = log;
    s.gpu = gpu;
    s.latest = Machine{};   // ("-" until the first sample, not the numbers of the time before)
    s.alive = true;
    s.live++;
    // (a thread the system will not make: no sampler, and the panel's machine numbers stay "-" (latest, emptied above). Never
    // the exception, which would reach menu_tick on the game's thread; the next start tries again)
    try {
        std::thread(sampler).detach();
    } catch (const std::system_error&) {
        s.alive = false;
        s.live--;
        note("its sampler's thread could not be made: the machine's numbers stay \"-\"");
    }
}

void stop() {
    State& s = st();
    std::lock_guard lk(s.mu);
    s.want = false;
    s.cv.notify_all();
}

Machine latest() {
    State& s = st();
    std::lock_guard lk(s.mu);
    return s.latest;
}

int live_samplers() {
    State& s = st();
    std::lock_guard lk(s.mu);
    return s.live;
}

} // namespace mkwvk::perf_machine
