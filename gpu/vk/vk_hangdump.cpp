// (EvilOOT) A watchdog for freezes. When no frame has been presented for RX_VK_HANG_SEC seconds (default 20) after the
// first, every thread of the process is suspended in turn and its stack unwound with Windows' own unwinder (the modules'
// .pdata); the return addresses go to a file as module+offset, with the thread's name, once all threads run again.
// Triforce.map / triforce_vk.map turn the offsets into functions.
// (11-07: a freeze right after Load, every thread waiting, none spinning: which wait is it.)
// Nothing is allocated, locked or logged while a thread is suspended: it may hold the heap's or the log's lock.
//
// (Triforce, after NX2navi's hang watch) On in every run, not only with RX_VK_HANG_DUMP=<file>: a freeze left a log that
// just stopped, and the reports of a game that "stops with no error in the log" had nothing to go by. The file is
// triforce_hang.txt beside the game, and the top of every stack goes into the log as well, so that the log alone says
// where the threads were. Without a file named, a second counts only while the game's window is in front and not
// minimized (behind another window the game may show no frames), a stall is reported once (the next needs a frame
// first) and a run reports at most three (gpu/vk/hang_watch.h). The watch never ends the game: one that recovers goes
// on. RX_VK_HANG_DUMP=<file> writes there and judges every second, as before; RX_VK_HANG_DUMP=0 turns the watch off.
#ifdef _WIN32   // (hookshot: thread stacks through the Win32 debug API; the Linux build has none, see the end)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "file_whole.h"   // ((Triforce, handoff §8) the report whole, and the process's end: file_writers.h)
#include "hang_watch.h"
#include "vk_api.h"
#include "vk_paths.h"

namespace mkwvk {
extern int64_t presents_now;
void logf(const char* fmt, ...);   // (vk_main.cpp)
extern const MkwVkHost* host;      // (vk_main.cpp: its log_flush writes the queued log lines out)

namespace {

constexpr int kMaxThreads = 512;
constexpr int kMaxFrames = 40;
constexpr int kLogFrames = 8;   // (the frames of each stack that go into the log; the file has them all)

struct ThreadStack {
    DWORD id;
    int frames;
    DWORD64 pc[kMaxFrames];
    char name[96];
};
ThreadStack stacks[kMaxThreads];

int unwind(HANDLE h, DWORD64* out) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(h, &ctx)) return 0;
    int n = 0;
    for (; n < kMaxFrames && ctx.Rip; n++) {
        out[n] = ctx.Rip;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
        if (!f) {   // (a leaf: the return address on the top of the stack)
            if (IsBadReadPtr((void*)ctx.Rsp, 8)) break;
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
        } else {
            void* handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &handler_data, &frame, nullptr);
        }
        if (!ctx.Rsp) break;
    }
    return n;
}

/// An address as its module's file name and the offset in it ("?" and the address itself where no module holds it)
DWORD64 in_module(DWORD64 pc, char* mod, size_t size) {
    HMODULE m = nullptr;
    snprintf(mod, size, "?");
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &m) && m) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(m, full, sizeof full)) {
            const char* b = full;
            for (const char* q = full; *q; q++)
                if (*q == '\\' || *q == '/') b = q + 1;
            snprintf(mod, size, "%s", b);
        }
    }
    return m ? pc - (DWORD64)m : pc;
}

/// Every other thread's stack into the file, and with to_log the top of each into the log. False when the file could
/// not be written (the log's lines are written all the same)
bool dump(const char* path, bool to_log) {
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    int count = 0;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok && count < kMaxThreads; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        ThreadStack& s = stacks[count];
        s.id = te.th32ThreadID, s.frames = 0, s.name[0] = 0;
        if (SuspendThread(h) != (DWORD)-1) {
            s.frames = unwind(h, s.pc);
            ResumeThread(h);
        }
        CloseHandle(h);
        count++;
    }
    CloseHandle(snap);
    // (all running again: now the names, the modules, the file and the log)
    using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const GetDesc get_desc = (GetDesc)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    for (int i = 0; i < count && get_desc; i++) {
        ThreadStack& s = stacks[i];
        if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, s.id)) {
            PWSTR w = nullptr;
            if (SUCCEEDED(get_desc(h, &w)) && w) {
                WideCharToMultiByte(CP_UTF8, 0, w, -1, s.name, sizeof s.name, nullptr, nullptr);
                s.name[sizeof s.name - 1] = 0;
                LocalFree(w);
            }
            CloseHandle(h);
        }
    }
    // ((Triforce, handoff §8) written beside it and put in its place: a report cut by the game's end (or by its close from
    // outside, a hang's usual end) left half of one over the last; the exit waits for one under way, file_writers.h)
    const file_writers::Writing writing;
    const std::string part = std::string(path) + ".part";
    FILE* f = writing ? fopen(part.c_str(), "w") : nullptr;
    if (f) {
        fprintf(f, "# hang dump: %d threads, %lld presents\n", count, (long long)presents_now);
        for (int i = 0; i < count; i++) {
            const ThreadStack& s = stacks[i];
            fprintf(f, "\nthread %lu \"%s\" (%d frames)\n", (unsigned long)s.id, s.name, s.frames);
            for (int k = 0; k < s.frames; k++) {
                char mod[MAX_PATH];
                const DWORD64 off = in_module(s.pc[k], mod, sizeof mod);
                fprintf(f, "  %2d %s+%#llx (%#llx)\n", k, mod, (unsigned long long)off, (unsigned long long)s.pc[k]);
            }
        }
        const bool whole = !ferror(f);
        if (fclose(f) != 0 || !whole || !file_writers::replace_file(part, path)) {
            remove(part.c_str());
            f = nullptr;
        }
    }
    for (int i = 0; i < count && to_log; i++) {
        const ThreadStack& s = stacks[i];
        if (!s.frames) continue;
        char line[900];
        int n = snprintf(line, sizeof line, "vk: hang: thread %lu \"%s\":", (unsigned long)s.id, s.name);
        for (int k = 0; k < s.frames && k < kLogFrames && n < (int)sizeof line - 80; k++) {
            char mod[64];
            const DWORD64 off = in_module(s.pc[k], mod, sizeof mod);
            n += snprintf(line + n, sizeof line - (size_t)n, " %s+%#llx", mod, (unsigned long long)off);
        }
        logf("%s", line);
    }
    return f != nullptr;
}

/// The game's own window is the one in front, and not minimized
bool window_in_front() {
    const HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (!w) return false;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId() && !IsIconic(w);
}

/// triforce_hang.txt in the game folder (game_module_path's rule, as the caches beside the exe)
std::string default_path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    const std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + "triforce_hang.txt";
}

}  // namespace

/// (vk_replay.cpp, at the first present) starts the watchdog, unless RX_VK_HANG_DUMP=0
void hang_watch_start() {
    static std::atomic<bool> started{false};
    const char* named = getenv("RX_VK_HANG_DUMP");
    if ((named && !strcmp(named, "0")) || started.exchange(true)) return;
    const bool by_name = named && *named;   // (asked for by name: every second is judged, whatever window is in front)
    const std::string path = by_name ? std::string(named) : default_path();
    const int sec = getenv("RX_VK_HANG_SEC") ? std::max(5, atoi(getenv("RX_VK_HANG_SEC"))) : 20;
    std::thread([path, sec, by_name] {
        SetThreadDescription(GetCurrentThread(), L"mkw_vk hang watch");
        hangwatch::Watch watch;
        for (;;) {
            Sleep(1000);
            if (file_writers::stopping()) return;   // ((Triforce) the game is ending: no report begins)
            const int64_t presents = presents_now;
            if (!hangwatch::step(watch, presents, by_name || window_in_front(), sec)) continue;
            logf("vk: HANG: no frame was shown for %d s%s (after present %lld). The game goes on if it can; where every "
                 "thread was follows, and their whole stacks go to %s: send that file with this log. Report %d of at "
                 "most %d", sec, by_name ? "" : " with the game's window in front", (long long)presents, path.c_str(),
                 watch.reports, hangwatch::kReports);
            if (!dump(path.c_str(), true)) logf("vk: hang: %s could not be written", path.c_str());
            // (the queued lines written out now: a game that never moves again is closed from outside, and the queue
            // would go with it)
            if (host && host->log_flush) host->log_flush();
        }
    }).detach();
}

}  // namespace mkwvk
#else
// (Linux: the same watchdog, with signals. When no frame has been presented for RX_VK_HANG_SEC seconds (default 20) after the
// first, a line goes to the log, and every thread is sent SIGUSR2 in turn: its handler takes the thread's stack (backtrace) and
// the watchdog writes it, with the thread's name, its state and the kernel function it sleeps in (/proc/self/task/<tid>/wchan), to
// RX_VK_HANG_DUMP (default triforce_hang.txt beside the log). Off with RX_VK_HANG_DUMP=0)
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace mkwvk {
extern int64_t presents_now;
void logf(const char* fmt, ...);

namespace {
constexpr int kFrames = 40;
void* g_frames[kFrames];
std::atomic<int> g_count{-1};   // (-1: not taken yet)

void on_stack(int) {
    g_count.store(backtrace(g_frames, kFrames), std::memory_order_release);
}

std::string read_file(const std::string& path) {
    std::string out;
    if (FILE* f = fopen(path.c_str(), "r")) {
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf - 1, f)) > 0 && out.size() < 4096) out.append(buf, n);
        fclose(f);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

void dump(const char* path) {
    const pid_t pid = getpid();
    const pid_t self = (pid_t)syscall(SYS_gettid);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# hang dump: %lld presents\n", (long long)presents_now);
    std::vector<pid_t> tids;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] >= '0' && e->d_name[0] <= '9') tids.push_back((pid_t)atoi(e->d_name));
        closedir(d);
    }
    for (pid_t tid : tids) {
        const std::string base = "/proc/self/task/" + std::to_string(tid);
        std::string stat = read_file(base + "/stat");
        const size_t close = stat.rfind(')');
        const char state = close != std::string::npos && close + 2 < stat.size() ? stat[close + 2] : '?';
        fprintf(f, "\nthread %d \"%s\" state %c wchan %s\n", (int)tid, read_file(base + "/comm").c_str(), state,
                read_file(base + "/wchan").c_str());
        if (tid == self) continue;
        g_count.store(-1, std::memory_order_release);
        if (syscall(SYS_tgkill, pid, tid, SIGUSR2) != 0) continue;
        for (int i = 0; i < 100 && g_count.load(std::memory_order_acquire) < 0; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const int n = g_count.load(std::memory_order_acquire);
        for (int k = 0; k < n; k++) {
            Dl_info info{};
            const char* mod = "?";
            if (dladdr(g_frames[k], &info) && info.dli_fname) {
                mod = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
            }
            fprintf(f, "  %2d %s+%#llx %s (%p)\n", k, mod,
                    (unsigned long long)((char*)g_frames[k] - (char*)(info.dli_fbase ? info.dli_fbase : nullptr)),
                    info.dli_sname ? info.dli_sname : "", g_frames[k]);
        }
    }
    fclose(f);
}
}  // namespace

void hang_watch_start() {
    static std::atomic<bool> started{false};
    const char* env = getenv("RX_VK_HANG_DUMP");
    if (env && !strcmp(env, "0")) return;
    if (started.exchange(true)) return;
    const std::string path = env && *env ? env : "triforce_hang.txt";
    const int sec = getenv("RX_VK_HANG_SEC") ? std::max(5, atoi(getenv("RX_VK_HANG_SEC"))) : 20;
    struct sigaction sa{};
    sa.sa_handler = on_stack;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, nullptr);
    std::thread([path, sec] {
        pthread_setname_np(pthread_self(), "mkw_vk hang");
        void* warm[2];
        backtrace(warm, 2);   // (loads libgcc's unwinder here, not inside a signal handler)
        int64_t last = -1;
        int still = 0;
        bool dumped = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const int64_t now = presents_now;
            if (now != last) last = now, still = 0, dumped = false;
            else if (++still >= sec && !dumped) {
                logf("vk: hang watch: no present for %d s (%lld presents so far): every thread's stack and wait go to %s", still,
                     (long long)now, path.c_str());
                dump(path.c_str());
                dumped = true;
            }
        }
    }).detach();
}
}  // namespace mkwvk
#endif
