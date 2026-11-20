"""A failed run's report is written whole (host/hos_thread.c hos_stack_guarantee and hos_ending_enter; no game).

Two ways a crash report was lost or cut:

- A stack overflow leaves the faulting thread about a guard page of stack, less than the report needs: it faulted again
  inside the report and the process ended with nothing in the log. The game's threads and fibers now keep 32 KiB back
  (SetThreadStackGuarantee) at their start.
- A second thread that crashed or hit a fatal error while the first was reporting ended the process under it. The first
  report now owns the end: the same thread again is told so, and any other thread waits.

Everywhere: the source is read for where the two are called. On Windows the two functions are cut out of
host/hos_thread.c and run: a thread with the guarantee that calls itself without end reaches an exception filter that
uses 16 KiB of stack and writes a line (exit code 42), and hos_ending_enter answers 1, then 0 to the same thread, and
holds another thread.

    python -I -B tests/test_crash_report.py [--cc gcc]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); fflush(stderr); TerminateProcess(GetCurrentProcess(), 1); } } while (0)
@@PRODUCTION@@
static LONG WINAPI filter(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    volatile char report[16384];   /* (what a report takes: the log's kilobyte buffers and the frame walk) */
    for (int i = 0; i < (int)sizeof report; i += 512) report[i] = (char)i;
    fputs("report written\n", stdout);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 42);
    return EXCEPTION_CONTINUE_SEARCH;
}
static int deeper(int n) {
    volatile char pad[1024];
    pad[0] = (char)n;
    return deeper(n + 1) + pad[0];
}
static DWORD WINAPI overflowing(void *guarantee) {
    if (guarantee) hos_stack_guarantee();
    return (DWORD)deeper(0);
}
static volatile LONG other_state;
static DWORD WINAPI other(void *p) {
    (void)p;
    other_state = 1;
    hos_ending_enter();   /* (waits: another thread owns the end) */
    other_state = 2;
    return 0;
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!strcmp(argv[1], "overflow") || !strcmp(argv[1], "overflow-bare")) {
        SetUnhandledExceptionFilter(filter);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        HANDLE t = CreateThread(NULL, 1 << 20, overflowing, argv[1][8] ? NULL : (void *)1, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        CHECK(t);
        WaitForSingleObject(t, 20000);
        return 3;   /* (not reached: the overflow ends the process) */
    }
    CHECK(hos_ending_enter() == 1);   /* the first caller reports */
    CHECK(hos_ending_enter() == 0);   /* a failure inside its own report: no second one */
    HANDLE t = CreateThread(NULL, 0, other, NULL, 0, NULL);
    CHECK(t);
    for (int i = 0; i < 200 && !other_state; i++) Sleep(5);
    Sleep(300);
    CHECK(other_state == 1);          /* another thread is held until the process ends */
    puts("PASS: one end for a failed run");
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
"""


def block(text, marker):
    if text.count(marker) != 1:
        raise SystemExit("marker not found once: " + marker)
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_source(thread, main_c, glue):
    for marker, what in (("static DWORD WINAPI thread_main(", "a game thread"), ("static void WINAPI fiber_main(", "a fiber")):
        body = block(thread, marker)
        if "hos_stack_guarantee();" not in body or body.index("hos_stack_guarantee();") > body.index("a64_"):
            raise SystemExit("FAIL: %s does not keep stack back for its crash report before it runs game code" % what)
    if "hos_stack_guarantee();" not in block(main_c, "static DWORD WINAPI game_thread("):
        raise SystemExit("FAIL: the game's main thread does not keep stack back for its crash report")
    fatal = block(thread, "void hos_fatal(")
    if "if (!hos_ending_enter())" not in fatal or fatal.index("hos_ending_enter()") > fatal.index("log_flush();"):
        raise SystemExit("FAIL: hos_fatal does not ask for the end before it writes")
    crash = block(glue, "static LONG WINAPI report_crash(")
    if "if (!hos_ending_enter()) return EXCEPTION_CONTINUE_SEARCH;" not in crash or \
            crash.index("hos_ending_enter()") > crash.index("hos_log("):
        raise SystemExit("FAIL: report_crash does not ask for the end before it writes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    thread = (ROOT / "host/hos_thread.c").read_text(encoding="utf-8")
    check_source(thread, (ROOT / "host/hos_main.c").read_text(encoding="utf-8"),
                 (ROOT / "host/mkw_glue.c").read_text(encoding="utf-8"))
    if os.name != "nt":
        print("PASS: the calls are in place (the run is Windows': SetThreadStackGuarantee)")
        return
    production = block(thread, "void hos_stack_guarantee(void)") + "\n" + block(thread, "int hos_ending_enter(void)")
    flags = subprocess.CREATE_NO_WINDOW
    with tempfile.TemporaryDirectory(prefix="crash-report-") as tmp:
        src, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture.exe"
        src.write_text(FIXTURE.replace("@@PRODUCTION@@", production), encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror", "-Wno-infinite-recursion",
                        str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe), "overflow"], capture_output=True, text=True, timeout=60, creationflags=flags)
        if r.returncode != 42 or "report written" not in r.stdout:
            raise SystemExit("FAIL: with the guarantee a stack overflow's report was not written (exit code %d, said %r)"
                             % (r.returncode, r.stdout + r.stderr))
        bare = subprocess.run([str(exe), "overflow-bare"], capture_output=True, text=True, timeout=60, creationflags=flags)
        print("without the guarantee: exit code %#x, report %s" % (bare.returncode & 0xFFFFFFFF,
                                                                   "written" if "report written" in bare.stdout else "lost"))
        r = subprocess.run([str(exe), "ending"], capture_output=True, text=True, timeout=60, creationflags=flags)
        print(r.stdout, end="")
        if r.returncode != 0 or "PASS" not in r.stdout:
            raise SystemExit("FAIL: hos_ending_enter (exit code %d) %s" % (r.returncode, r.stderr))
    print("PASS: a stack overflow's report is written whole, and a failed run has one end")


if __name__ == "__main__":
    main()
