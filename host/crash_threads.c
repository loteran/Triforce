/* (Triforce) A crash report's other threads: host/crash_threads.h has what and why. */
#include "crash_threads.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hos_log(const char *fmt, ...);

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

typedef struct {
    DWORD id;
    int frames;
    DWORD64 pc[CRASH_THREADS_FRAMES];
} CrashStack;
static CrashStack crash_stacks[CRASH_THREADS_MAX];   /* (static: nothing is allocated while a thread is stopped) */
static volatile LONG crash_begun, crash_count = -1;  /* (-1 until the snapshot is whole) */
static HANDLE crash_helper;
static DWORD crash_self;

/* (the thread's stack, from its TEB: NtQueryInformationThread, a system call that takes no lock of the process's; found by
   crash_take before it stops any thread, as GetProcAddress takes the loader's lock) */
typedef LONG(NTAPI *CrashQueryThread)(HANDLE, int, PVOID, ULONG, PULONG);
typedef struct {
    LONG ExitStatus;
    PVOID TebBaseAddress;
    PVOID UniqueProcess, UniqueThread;
    ULONG_PTR AffinityMask;
    LONG Priority, BasePriority;
} CrashThreadBasic;   /* (THREAD_BASIC_INFORMATION, class 0) */
static CrashQueryThread crash_query;
static int crash_stack_bounds(HANDLE h, DWORD64 *lo, DWORD64 *hi) {
    CrashThreadBasic bi;
    memset(&bi, 0, sizeof bi);
    if (!crash_query || crash_query(h, 0, &bi, sizeof bi, NULL) < 0 || !bi.TebBaseAddress) return 0;
    const NT_TIB *tib = (const NT_TIB *)bi.TebBaseAddress;   /* (the TEB is the thread's, always there) */
    *lo = (DWORD64)(uintptr_t)tib->StackLimit, *hi = (DWORD64)(uintptr_t)tib->StackBase;
    return *lo < *hi;
}

static int crash_unwind(HANDLE h, DWORD64 *out) {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(h, &ctx)) return 0;
    DWORD64 lo = 0, hi = 0;
    if (!crash_stack_bounds(h, &lo, &hi)) {   /* (no stack known: the top frame alone, nothing read) */
        out[0] = ctx.Rip;
        return ctx.Rip != 0;
    }
    int n = 0;
    for (; n < CRASH_THREADS_FRAMES && ctx.Rip; n++) {
        out[n] = ctx.Rip;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, NULL);
        if (!crash_threads_in_stack(ctx.Rsp, 8, lo, hi)) break;   /* (crash_threads.h: nothing read outside the stack) */
        if (!f) {   /* (a leaf: the return address on the top of the stack) */
            ctx.Rip = *(DWORD64 *)ctx.Rsp;
            ctx.Rsp += 8;
        } else {
            void *handler_data = NULL;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &handler_data, &frame, NULL);
        }
        if (!ctx.Rsp) break;
    }
    return n;
}

/* Every thread but the faulting one and this one, each stopped for its own unwind only. Between a SuspendThread and its
   ResumeThread nothing is allocated, locked or logged: the stopped thread may hold the heap's or the log's lock */
static DWORD WINAPI crash_take(void *unused) {
    (void)unused;
    const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int count = 0;
    crash_query = (CrashQueryThread)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok && count < CRASH_THREADS_MAX; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me || te.th32ThreadID == crash_self) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            CrashStack *s = &crash_stacks[count];
            s->id = te.th32ThreadID, s->frames = 0;
            if (SuspendThread(h) != (DWORD)-1) {
                s->frames = crash_unwind(h, s->pc);
                ResumeThread(h);
            }
            CloseHandle(h);
            count++;
        }
        CloseHandle(snap);
    }
    InterlockedExchange(&crash_count, count);
    return 0;
}

void hos_crash_threads_begin(void) {
    if (!crash_threads_on(getenv("HOS_CRASH_THREADS")) || InterlockedExchange(&crash_begun, 1)) return;
    crash_self = GetCurrentThreadId();
    crash_helper = CreateThread(NULL, 0, crash_take, NULL, 0, NULL);
}

/* An address as its module's file name and the offset in it ("?" and the address where no module holds it) */
static DWORD64 crash_in_module(DWORD64 pc, char *mod, size_t size) {
    HMODULE m = NULL;
    snprintf(mod, size, "?");
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &m) && m) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(m, full, sizeof full)) {
            const char *b = full;
            for (const char *q = full; *q; q++)
                if (*q == '\\' || *q == '/') b = q + 1;
            snprintf(mod, size, "%s", b);
        }
    }
    return m ? pc - (DWORD64)(uintptr_t)m : pc;
}

void hos_crash_threads_end(void) {
    if (!crash_helper) return;
    HANDLE helper = crash_helper;
    crash_helper = NULL;   /* (one report) */
    const DWORD waited = WaitForSingleObject(helper, CRASH_THREADS_WAIT_MS);
    const LONG count = InterlockedCompareExchange(&crash_count, -1, -1);
    if (waited != WAIT_OBJECT_0 || count < 0) {
        hos_log("CRASH: the other threads' stacks were not taken within %d ms", (int)CRASH_THREADS_WAIT_MS);
        return;
    }
    CloseHandle(helper);
    typedef HRESULT(WINAPI * GetDesc)(HANDLE, PWSTR *);
    const GetDesc get_desc = (GetDesc)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    int running = 0;
    for (int pass = 0; pass < 2; pass++)   /* (the threads running code first, then the ones waiting in the system) */
        for (int i = 0; i < count; i++) {
            const CrashStack *s = &crash_stacks[i];
            if (!s->frames) continue;
            char mod[96];
            crash_in_module(s->pc[0], mod, sizeof mod);
            const int waiting = crash_threads_waiting(mod);
            if (waiting != pass) continue;
            running += !waiting;
            char name[96] = "";
            HANDLE h = get_desc ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, s->id) : NULL;
            if (h) {
                PWSTR w = NULL;
                if (SUCCEEDED(get_desc(h, &w)) && w) {
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, name, sizeof name, NULL, NULL);
                    name[sizeof name - 1] = 0;
                    LocalFree(w);
                }
                CloseHandle(h);
            }
            char line[900];
            int n = snprintf(line, sizeof line, "CRASH: beside it, thread \"%s\" (%lu) %s:", name, (unsigned long)s->id,
                             waiting ? "waiting" : "running");
            const int shown = crash_threads_shown(s->frames, waiting);
            for (int k = 0; k < shown && n < (int)sizeof line - 112; k++) {
                const DWORD64 off = crash_in_module(s->pc[k], mod, sizeof mod);
                n += snprintf(line + n, sizeof line - (size_t)n, " %s+%#llx", mod, (unsigned long long)off);
            }
            hos_log("%s", line);
        }
    hos_log("CRASH: beside it: %ld other threads, %d of them running code (HOS_CRASH_THREADS=0: not taken)", (long)count, running);
}

#else
/* (Linux: no stacks of other threads here; the report is as before) */
void hos_crash_threads_begin(void) {}
void hos_crash_threads_end(void) {}
#endif
