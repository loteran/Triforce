"""The save's lock between the game and Setup, the game's side (host/save_tx.h save_tx_lock; no game).

Setup copied a save after a look at the running programs alone: a game started after that look mounted half a save. Now the
game holds <user>\\save.lock open without sharing from its first save mount to its end, and Setup holds the same file across
its copy (setup/Guard.cs SaveLock, tests/test_setup_partial.py).

Compiles save_tx.h (as tests/test_save_tx.py does) into a program and runs it in a temporary folder: the lock is taken (the
folder made when it is missing) and kept, a second open of the file without sharing is refused meanwhile, a second process
asking for it waits the time it is given and is told 0 (another program holds it), it gets the lock once the first lets go
within that time, and HOS_SAVE_LOCK=0 takes none. Reads nn_fs.c for where it is taken: at the run's first save mount,
before the save is read.

    python -I -B tests/test_save_lock.py [--cc gcc]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]
RUN_ALL_SKIP = {"linux": "the lock is Windows' file sharing (on Linux the game and Setup do not meet)"}

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define HOS_FS_PATH 1024
#include "save_tx.h"
#include <stdio.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "ask")) return save_tx_lock(argv[2], atoi(argv[3]));   /* (the second process) */
    const char *user = argv[1];
    char file[HOS_FS_PATH], self[MAX_PATH], cmd[3 * HOS_FS_PATH];
    snprintf(file, sizeof file, "%s\\save.lock", user);
    GetModuleFileNameA(NULL, self, MAX_PATH);
    CHECK(save_tx_lock(user, 0) == 1);
    CHECK(GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES);
    CHECK(save_tx_lock(user, 0) == 1);   /* (taken once a run) */
    HANDLE other = CreateFileA(file, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, 0, NULL);
    CHECK(other == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    /* a second process waits the time it is given, and is told that another program holds it */
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    DWORD code = 99;
    snprintf(cmd, sizeof cmd, "\"%s\" ask \"%s\" 300", self, user);
    const DWORD t0 = GetTickCount();
    CHECK(CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    CHECK(code == 0);
    CHECK(GetTickCount() - t0 >= 250);
    /* and gets it when the holder lets go within that time */
    snprintf(cmd, sizeof cmd, "\"%s\" ask \"%s\" 5000", self, user);
    CHECK(CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
    Sleep(300);
    CloseHandle(save_tx_lock_handle), save_tx_lock_handle = INVALID_HANDLE_VALUE;
    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    CHECK(code == 1);
    /* HOS_SAVE_LOCK=0: none */
    SetEnvironmentVariableA("HOS_SAVE_LOCK", "0");
    _putenv("HOS_SAVE_LOCK=0");
    CHECK(save_tx_lock(user, 0) == 2 && save_tx_lock_handle == INVALID_HANDLE_VALUE);
    if (!fails) printf("PASS: the save's lock is held, refused to a second holder, waited for, and off with HOS_SAVE_LOCK=0\n");
    return fails != 0;
}
"""


def check_source():
    fs = (ROOT / "host/nn_fs.c").read_text(encoding="utf-8")
    mount = fs[fs.index("static int tx_mount(const char *name"):]
    mount = mount[:mount.index("\n}\n")]
    if "save_tx_lock(hos_user_dir, 30000)" not in mount or mount.index("save_tx_lock(") > mount.index("save_tx_mount(&m->p"):
        raise SystemExit("host/nn_fs.c tx_mount no longer takes the save's lock before the save is read")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    check_source()
    if sys.platform != "win32":
        print("NOT RUN: Windows only")
        return
    with tempfile.TemporaryDirectory(prefix="save-lock-") as tmp:
        c, exe = Path(tmp) / "lock.c", Path(tmp) / "lock.exe"
        c.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu11", "-O1", "-Wall", "-Wno-unused-function", "-I", str(ROOT / "host"), str(c), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe), str(Path(tmp) / "game" / "user")], check=True, timeout=60)


if __name__ == "__main__":
    main()
