"""Windows' power throttling of a window not in front, opted out of at the start (host/hos_main.c no_power_throttling; no game).

Windows 11 runs a process whose window is not in front at efficiency speed (EcoQoS) and may ignore its 1 ms timer resolution,
which turns the pacing's 1 ms waits into 15.6 ms ones. Every run opts out at the start, the shader preparation's too, and the
log's "power:" line says what Windows took; HOS_POWER_THROTTLING=1 leaves the throttling as it is. Cuts no_power_throttling out
of hos_main.c and runs it against a stand-in SetProcessInformation, a process each: Windows 11 (speed and timer in one call),
Windows 10 (the timer control refused, speed alone asked), a refusal, no such call, and the switch. Reads the source for what
needs the game: where game_main calls it (after the environment file and the log's first line, before the shader
preparation's return) and logs the outcome, that the 1 ms timer is still asked for, and that the switch is in the
development guide.

    python -I -B tests/test_power_throttling.py [--cc gcc]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_source(main):
    game = block(main, "static int game_main(int argc, char **argv) {")   # (Windows' main and Linux's game_main: one body)
    if game.count("no_power_throttling()") != 1 or main.count("no_power_throttling()") != 1:
        raise SystemExit("hos_main.c: no_power_throttling is not called once, by game_main")
    at = game.index("no_power_throttling()")
    for before, why in (("load_hookshot_env();", "before the environment file is read (HOS_POWER_THROTTLING there)"),
                        ('hos_log("triforce %s", HOOKSHOT_VERSION);', "before the log is open")):
        if not 0 <= game.index(before) < at:
            raise SystemExit("hos_main.c game_main: the power throttling is opted out of " + why)
    if not at < game.index("rx_prepare_shaders()"):
        raise SystemExit("hos_main.c game_main: the shader preparation's run returns before the power throttling is opted out of")
    said = game[at:at + 700]
    if 'hos_log("power: %s"' not in said or "got == 2 ?" not in said or "got == 1 ?" not in said:
        raise SystemExit("hos_main.c game_main: the log does not say what Windows took")
    if game.rindex("#ifdef _WIN32", 0, at) < game.rindex("#endif", 0, at):
        raise SystemExit("hos_main.c game_main: the opt-out is not inside #ifdef _WIN32")
    if "timeBeginPeriod(1);" not in game:
        raise SystemExit("hos_main.c game_main: the 1 ms timer resolution is no longer asked for")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "`HOS_POWER_THROTTLING=1`" not in dev:
        raise SystemExit("docs/development.md: HOS_POWER_THROTTLING is not in the switches table")


FIXTURE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef int BOOL;
typedef void *HANDLE, *LPVOID, *HMODULE;
typedef uint32_t DWORD, ULONG;   /* (Windows': 32 bits) */
typedef void (*FARPROC)(void);
#define WINAPI
static int calls, looked_up, accept_timer, accept_speed, have_call;
static struct { int cls; ULONG version, control, state; DWORD size; HANDLE process; } seen[4];
static BOOL fake_set(HANDLE process, int cls, LPVOID info, DWORD size) {
    CHECK(calls < 4);
    const ULONG *s = (const ULONG *)info;
    seen[calls].cls = cls, seen[calls].version = s[0], seen[calls].control = s[1], seen[calls].state = s[2];
    seen[calls].size = size, seen[calls].process = process;
    calls++;
    return s[1] == (0x1 | 0x4) ? accept_timer : s[1] == 0x1 ? accept_speed : 0;
}
static HMODULE GetModuleHandleA(const char *name) {
    CHECK(!strcmp(name, "kernel32.dll"));
    return (HMODULE)(uintptr_t)0x1000;
}
static FARPROC GetProcAddress(HMODULE module, const char *name) {
    CHECK(module == (HMODULE)(uintptr_t)0x1000 && !strcmp(name, "SetProcessInformation"));
    looked_up++;
    return have_call ? (FARPROC)fake_set : NULL;
}
static HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
/* FUNCTION */
int main(int argc, char **argv) {
    CHECK(argc == 6);
    have_call = atoi(argv[1]), accept_timer = atoi(argv[2]), accept_speed = atoi(argv[3]);
    const int want = atoi(argv[4]), want_calls = atoi(argv[5]);
    const int got = no_power_throttling();
    CHECK(got == want);
    CHECK(calls == want_calls);
    for (int i = 0; i < calls; i++) {
        CHECK(seen[i].cls == 4);                     /* (ProcessPowerThrottling) */
        CHECK(seen[i].version == 1);                 /* (PROCESS_POWER_THROTTLING_CURRENT_VERSION) */
        CHECK(seen[i].size == 12);                   /* (PROCESS_POWER_THROTTLING_STATE: three ULONGs) */
        CHECK(seen[i].state == 0);                   /* (off: not throttled) */
        CHECK(seen[i].process == (HANDLE)(intptr_t)-1);
    }
    if (calls >= 1) CHECK(seen[0].control == (0x1 | 0x4));   /* (execution speed and the timer resolution at once) */
    if (calls >= 2) CHECK(seen[1].control == 0x1);           /* (execution speed alone, Windows 10) */
    if (want == -1) CHECK(looked_up == 0);
    printf("PASS: HOS_POWER_THROTTLING=%s, call %s, timer %s, speed %s: %d\n", getenv("HOS_POWER_THROTTLING") ? getenv("HOS_POWER_THROTTLING") : "(unset)",
           argv[1], argv[2], argv[3], got);
    return 0;
}
'''

# (HOS_POWER_THROTTLING, the call there, the timer control taken, execution speed taken) -> what it returns, the calls made
RUNS = [(None, 1, 1, 1, 2, 1),    # (Windows 11)
        (None, 1, 0, 1, 1, 2),    # (Windows 10: the timer control refused)
        (None, 1, 0, 0, 0, 2),    # (both refused)
        (None, 0, 1, 1, 0, 0),    # (no SetProcessInformation: before Windows 8)
        ("1", 1, 1, 1, -1, 0),    # (left as it is)
        ("0", 1, 1, 1, 2, 1)]     # (0: as unset)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    main_c = (ROOT / "host/hos_main.c").read_text(encoding="utf-8")
    check_source(main_c)
    function = block(main_c, "static int no_power_throttling(void) {")
    with tempfile.TemporaryDirectory(prefix="power-throttling-") as tmp:
        tmp = Path(tmp)
        c, exe = tmp / "fixture.c", tmp / "fixture.exe"
        c.write_text(FIXTURE.replace("/* FUNCTION */", function), encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        for switch, have, timer, speed, want, calls in RUNS:
            env = {k: v for k, v in os.environ.items() if k != "HOS_POWER_THROTTLING"}
            if switch is not None:
                env["HOS_POWER_THROTTLING"] = switch
            subprocess.run([str(exe), str(have), str(timer), str(speed), str(want), str(calls)], env=env, check=True, timeout=30)
    print("PASS: the power throttling opted out of at the start")


if __name__ == "__main__":
    main()
