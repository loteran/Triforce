"""A crash report says what the other threads were doing (host/crash_threads.h and .c, HOS_CRASH_THREADS; no game).

A crash report had the faulting thread alone. The crash while a save loads reads an object whose memory holds something
else, and its reports could not say which thread had freed it. A report now starts by taking every other thread's host
stack on a helper thread and ends with a line a thread.

The header's rules, compiled and run: on unless the switch is 0; a thread whose top frame is in a system library is a
waiting one, any other a running one; a running thread's line has every frame kept and a waiting one's twelve. Each rule
taken out in turn (a mutation) must make the test fail, and each mutant must build.

On Windows host/crash_threads.c itself is compiled and run beside a thread that spins in a function of its own and one
that sleeps: the report has a "running" line for the spinner with a frame inside the test's own program and its name, a
"waiting" line for the sleeper, the running ones first, no line for the reporting thread, and the count; with the switch
at 0 nothing is taken or written; a second begin takes nothing again.

The sources: nothing is allocated, logged or locked between a thread's SuspendThread and its ResumeThread; the crash
report and the fatal error's begin the snapshot before their own lines and write it after them, with the lines so far
flushed first; the switch's row is in the development guide.

    python -I -B tests/test_crash_threads.py [--cc gcc]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "host"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def in_order(text, parts, where):
    at = 0
    for part in parts:
        found = text.find(part, at)
        if found < 0:
            raise SystemExit("%s: %r is missing, or not after the part before it (update the test with the code)" % (where, part))
        at = found + len(part)


HEADER_TEST = r'''
#include <stdio.h>
#include <stdlib.h>
#include "crash_threads.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void) {
    /* (on unless the switch is 0) */
    CHECK(crash_threads_on(NULL) && crash_threads_on("") && crash_threads_on("1") && crash_threads_on("00") && !crash_threads_on("0"));
    /* (a top frame in a system library: waiting; in the game, the back end, a driver, or no module: running) */
    CHECK(crash_threads_waiting("ntdll.dll") && crash_threads_waiting("NTDLL.DLL") && crash_threads_waiting("KERNELBASE.dll"));
    CHECK(crash_threads_waiting("kernel32.dll") && crash_threads_waiting("win32u.dll") && crash_threads_waiting("USER32.dll"));
    CHECK(!crash_threads_waiting("Triforce.exe") && !crash_threads_waiting("triforce_vk.dll") && !crash_threads_waiting("nvoglv64.dll"));
    CHECK(!crash_threads_waiting("?") && !crash_threads_waiting("") && !crash_threads_waiting("nt") && !crash_threads_waiting("kernel"));
    /* (a line's frames: all kept of a running thread, twelve of a waiting one, never more than there are) */
    CHECK(crash_threads_shown(CRASH_THREADS_FRAMES, 0) == CRASH_THREADS_FRAMES && crash_threads_shown(3, 0) == 3);
    CHECK(crash_threads_shown(CRASH_THREADS_FRAMES, 1) == 12 && crash_threads_shown(2, 1) == 2 && crash_threads_shown(0, 0) == 0);
    CHECK(crash_threads_shown(-1, 0) == 0 && crash_threads_shown(100, 0) == CRASH_THREADS_FRAMES);
    CHECK(CRASH_THREADS_MAX >= 256 && CRASH_THREADS_FRAMES > 12 && CRASH_THREADS_FRAMES <= 40);
    /* (a stopped thread's walk reads only inside its stack [lo, hi)) */
    CHECK(crash_threads_in_stack(0x1000, 8, 0x1000, 0x2000) && crash_threads_in_stack(0x1ff8, 8, 0x1000, 0x2000));
    CHECK(!crash_threads_in_stack(0xff8, 8, 0x1000, 0x2000) && !crash_threads_in_stack(0x1ffc, 8, 0x1000, 0x2000));
    CHECK(!crash_threads_in_stack(0x3000, 8, 0x1000, 0x2000) && !crash_threads_in_stack(0x1800, 0x900, 0x1000, 0x2000));
    CHECK(!crash_threads_in_stack(0x1000, 8, 0x2000, 0x1000) && !crash_threads_in_stack(0x1000, ~0ull, 0x1000, 0x2000));
    CHECK(CRASH_THREADS_WAIT_MS >= 500 && CRASH_THREADS_WAIT_MS <= 5000);
    puts("PASS header: the switch, waiting or running, a line's frames");
    return 0;
}
'''

MUTATIONS = [
    ("off at 0", "return !(env && env[0] == '0' && !env[1]);", "return 1 + 0 * (env != 0);"),
    ("on when unset", "return !(env && env[0] == '0' && !env[1]);", "return env && env[0] == '1';"),
    ("0 alone is off", "return !(env && env[0] == '0' && !env[1]);", "return !(env && env[0] == '0');"),
    ("ntdll waits", 'crash_threads_prefix(top_module, "ntdll") || ', ""),
    ("kernelbase waits", 'crash_threads_prefix(top_module, "kernelbase") ||', ""),
    ("win32u waits", 'crash_threads_prefix(top_module, "win32u") ||', ""),
    ("whatever its case", "return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;", "return ch;"),
    ("the whole prefix", "if (crash_threads_lower((unsigned char)*s) != *lower_prefix) return 0;",
     "if (crash_threads_lower((unsigned char)*s) != *lower_prefix) return s[0] == 'n';"),
    ("a waiting thread's line is short", "const int most = waiting ? 12 : CRASH_THREADS_FRAMES;", "const int most = CRASH_THREADS_FRAMES + 0 * waiting;"),
    ("a running thread's line is whole", "const int most = waiting ? 12 : CRASH_THREADS_FRAMES;", "const int most = 12 + 0 * waiting;"),
    ("no more frames than there are", "return frames < 0 ? 0 : frames < most ? frames : most;", "return frames < 0 ? 0 : most;"),
    ("the stack's bottom", "return sp >= lo && sp <= hi && need <= hi - sp;", "return (int)(0 * lo) + (sp <= hi && need <= hi - sp);"),
    ("the stack's top", "return sp >= lo && sp <= hi && need <= hi - sp;", "return sp >= lo && sp <= hi + 0 * need;"),
    ("past the top by the read", "return sp >= lo && sp <= hi && need <= hi - sp;", "return sp >= lo && sp < hi + 0 * need;"),
]

# (Windows: crash_threads.c as it is, with the log's lines kept for the checks)
RUNTIME_TEST = r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
static char lines[64][1024];
static int nlines;
void hos_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (nlines < 64) vsnprintf(lines[nlines++], sizeof lines[0], fmt, ap);
    va_end(ap);
}
#include "@SOURCE@"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); for (int i = 0; i < nlines; i++) fprintf(stderr, "  %s\n", lines[i]); exit(1); } } while (0)
static volatile LONG stop, spinning;
__declspec(noinline) static DWORD spin_here(void) {
    volatile unsigned n = 0;
    InterlockedExchange(&spinning, 1);
    while (!stop) n++;
    return n;
}
static DWORD WINAPI spinner(void *p) { (void)p; SetThreadDescription(GetCurrentThread(), L"the spinner"); return spin_here() + 1; }
static DWORD WINAPI sleeper(void *p) { (void)p; SetThreadDescription(GetCurrentThread(), L"the sleeper"); Sleep(60000); return 0; }
static int find(const char *what) {
    for (int i = 0; i < nlines; i++)
        if (strstr(lines[i], what)) return i;
    return -1;
}
int main(int argc, char **argv) {
    (void)argv;
    HANDLE a = CreateThread(NULL, 0, spinner, NULL, 0, NULL), b = CreateThread(NULL, 0, sleeper, NULL, 0, NULL);
    CHECK(a && b);
    while (!spinning) Sleep(1);
    Sleep(100);   /* (the sleeper is in its sleep by now) */
    if (argc > 1) {   /* (the switch at 0, set by the test: nothing taken, nothing written) */
        hos_crash_threads_begin();
        hos_crash_threads_end();
        CHECK(nlines == 0);
        puts("PASS run (Windows), HOS_CRASH_THREADS=0: nothing taken");
        return 0;
    }
    SetThreadDescription(GetCurrentThread(), L"the reporter");
    hos_crash_threads_begin();
    hos_crash_threads_begin();   /* (once a process) */
    hos_crash_threads_end();
    const int run = find("thread \"the spinner\""), wait = find("thread \"the sleeper\""), total = find("other threads,");
    CHECK(run >= 0 && wait >= 0 && total >= 0);
    CHECK(strstr(lines[run], ") running:") && strstr(lines[wait], ") waiting:"));
    CHECK(run < wait && wait < total && total == nlines - 1);   /* (the running ones first, the count last) */
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);
    const char *base = strrchr(self, '\\') ? strrchr(self, '\\') + 1 : self;
    CHECK(strstr(lines[run], base));                          /* (a frame in this program: the spin) */
    CHECK(strstr(lines[wait], "ntdll") || strstr(lines[wait], "KERNELBASE") || strstr(lines[wait], "kernel"));
    CHECK(find("the reporter") < 0);                          /* (the reporting thread has its own report) */
    CHECK(find("not taken within") < 0);
    int spaces = 0;                                            /* (a waiting line: twelve frames at most) */
    for (const char *q = strstr(lines[wait], "waiting:"); q && *q; q++) spaces += *q == ' ';
    CHECK(spaces >= 1 && spaces <= 12);
    const int before = nlines;
    hos_crash_threads_end();                                   /* (one report) */
    CHECK(nlines == before);
    InterlockedExchange(&stop, 1);
    puts("PASS run (Windows): a running line for the spinner, a waiting line for the sleeper, none for the reporter");
    return 0;
}
'''


def check_source(cfile, glue, thread, dev):
    take = block(cfile, "static DWORD WINAPI crash_take(void *unused) {")
    a, b = take.index("if (SuspendThread(h) != (DWORD)-1) {"), take.index("ResumeThread(h);")
    held = take[a:b]
    for bad in ("hos_log", "malloc", "calloc", "printf", "fopen", "OpenThread", "CloseHandle", "CreateToolhelp32Snapshot", "Acquire"):
        if bad in held:
            raise SystemExit("crash_threads.c: %s between a thread's SuspendThread and its ResumeThread (it may hold that lock)" % bad)
    if "s->frames = crash_unwind(h, s->pc);" not in held:
        raise SystemExit("crash_threads.c: the unwind is not between SuspendThread and ResumeThread")
    in_order(take, ["te.th32ThreadID == me || te.th32ThreadID == crash_self", "SuspendThread(h)", "ResumeThread(h);",
                    "InterlockedExchange(&crash_count, count);"], "crash_threads.c crash_take")
    unwind = block(cfile, "static int crash_unwind(HANDLE h, DWORD64 *out) {")
    for bad in ("hos_log", "malloc", "printf", "IsBadReadPtr", "GetProcAddress"):
        if bad in unwind:
            raise SystemExit("crash_threads.c crash_unwind: %s while the thread is stopped" % bad)
    # (nothing read outside the stopped thread's own stack: the bounds first, each step checked before it reads)
    in_order(unwind, ["if (!crash_stack_bounds(h, &lo, &hi)) {", "for (; n < CRASH_THREADS_FRAMES && ctx.Rip; n++) {",
                      "if (!crash_threads_in_stack(ctx.Rsp, 8, lo, hi)) break;", "ctx.Rip = *(DWORD64 *)ctx.Rsp;",
                      "RtlVirtualUnwind("], "crash_threads.c crash_unwind")
    in_order(take, ['crash_query = (CrashQueryThread)(void *)GetProcAddress(', "for (BOOL ok = Thread32First(", "SuspendThread(h)"],
             "crash_threads.c crash_take: NtQueryInformationThread found before any thread is stopped")
    begin = block(cfile, "void hos_crash_threads_begin(void) {")
    in_order(begin, ['if (!crash_threads_on(getenv("HOS_CRASH_THREADS")) || InterlockedExchange(&crash_begun, 1)) return;',
                     "crash_self = GetCurrentThreadId();", "crash_helper = CreateThread(NULL, 0, crash_take, NULL, 0, NULL);"],
             "crash_threads.c hos_crash_threads_begin")
    end = block(cfile, "void hos_crash_threads_end(void) {")
    in_order(end, ["if (!crash_helper) return;", "WaitForSingleObject(helper, CRASH_THREADS_WAIT_MS);",
                   "if (waited != WAIT_OBJECT_0 || count < 0) {", "return;", "for (int pass = 0; pass < 2; pass++)",
                   "const int waiting = crash_threads_waiting(mod);", "if (waiting != pass) continue;",
                   "const int shown = crash_threads_shown(s->frames, waiting);"], "crash_threads.c hos_crash_threads_end")
    # (the crash report: the snapshot begun before its first line, written after its last, the lines so far flushed first)
    crash = block(glue, "static LONG WINAPI report_crash(EXCEPTION_POINTERS *info) {")
    in_order(crash, ["if (!hos_ending_enter()) return EXCEPTION_CONTINUE_SEARCH;", "hos_crash_threads_begin();",
                     'hos_log("CRASH: exception %#lx', 'hos_log("CRASH: [x%d+%#05x]', "hos_log_flush();",
                     "hos_crash_threads_end();", "hos_log_flush();", "return EXCEPTION_CONTINUE_SEARCH;"], "mkw_glue.c report_crash")
    fatal = block(thread, "void hos_fatal(Ctx *c, const char *fmt, ...) {")
    in_order(fatal, ["if (!hos_ending_enter()) {", "hos_crash_threads_begin();", 'FATAL: %s"', "hos_backtrace();",
                     "hos_crash_threads_end();", "log_flush();", "TerminateProcess(GetCurrentProcess(), 3);"], "hos_thread.c hos_fatal")
    for name, text in (("mkw_glue.c", glue), ("hos_thread.c", thread)):
        if text.count("hos_crash_threads_begin();") != 1 or text.count("hos_crash_threads_end();") != 1:
            raise SystemExit("%s: the snapshot is begun or written in a place this test does not know" % name)
    in_order(glue, ["static DWORD WINAPI crash_test_after(void *p) {", 'getenv("HOS_CRASH_TEST_S")'], "mkw_glue.c the test crash")
    if "| `HOS_CRASH_THREADS=0` |" not in dev:
        raise SystemExit("docs/development.md: HOS_CRASH_THREADS is not in the switches table")


def build(cc, d, name, src, include, extra=()):
    c, exe = d / (name + ".c"), d / (name + ".exe")
    c.write_text(src, encoding="utf-8")
    r = subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", *extra, "-I" + str(include), str(c),
                        "-o", str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        return None, r.stdout
    return exe, ""


def run(exe, args=(), env=None):
    return subprocess.run([str(exe), *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60, env=env)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    a = ap.parse_args()
    cfile = (HOST / "crash_threads.c").read_text(encoding="utf-8")
    header = (HOST / "crash_threads.h").read_text(encoding="utf-8")
    check_source(cfile, (HOST / "mkw_glue.c").read_text(encoding="utf-8"), (HOST / "hos_thread.c").read_text(encoding="utf-8"),
                 (ROOT / "docs" / "development.md").read_text(encoding="utf-8"))
    print("PASS sources: nothing allocated or logged while a thread is stopped; the crash report and the fatal error's begin "
          "the snapshot first and write it last; the switch's row")
    with tempfile.TemporaryDirectory(prefix="crash-threads-") as tmp:
        d = Path(tmp)
        exe, out = build(a.cc, d, "header", HEADER_TEST, HOST)
        if not exe:
            raise SystemExit("header: the build failed:\n" + out)
        r = run(exe)
        if r.returncode:
            raise SystemExit("the header test failed:\n" + r.stdout)
        print(r.stdout.strip())
        for i, (what, old, new) in enumerate(MUTATIONS):
            if header.count(old) != 1:
                raise SystemExit("mutation %r: its text is not once in crash_threads.h (update the test with the header)" % what)
            inc = d / ("mutant%d" % i)
            inc.mkdir()
            (inc / "crash_threads.h").write_text(header.replace(old, new), encoding="utf-8")
            exe, out = build(a.cc, d, "mutant%d" % i, HEADER_TEST, inc)
            if not exe:
                raise SystemExit("mutation %r: the mutant does not build (a mutant must fail the test, not the compiler):\n%s" % (what, out))
            if run(exe).returncode == 0:
                raise SystemExit("mutation %r: the header test still passed without it" % what)
        print("PASS mutations: %d rules of crash_threads.h, each taken out, fail the header's test" % len(MUTATIONS))
        if sys.platform != "win32":
            print("SKIP run: the other threads' stacks are taken on Windows only")
            return
        src = RUNTIME_TEST.replace("@SOURCE@", str(HOST / "crash_threads.c").replace("\\", "/"))
        exe, out = build(a.cc, d, "run", src, HOST)
        if not exe:
            raise SystemExit("the Windows run: the build failed:\n" + out)
        env = dict(os.environ)
        env.pop("HOS_CRASH_THREADS", None)
        r = run(exe, env=env)
        if r.returncode:
            raise SystemExit("the Windows run failed:\n" + r.stdout)
        print(r.stdout.strip())
        env["HOS_CRASH_THREADS"] = "0"
        r = run(exe, ["off"], env=env)
        if r.returncode:
            raise SystemExit("the Windows run with HOS_CRASH_THREADS=0 failed:\n" + r.stdout)
        print(r.stdout.strip())


if __name__ == "__main__":
    main()
