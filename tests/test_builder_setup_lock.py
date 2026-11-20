"""(Triforce) One builder setup at a time for a setup folder (builder/setup/setup.c, builder/py/hookshot_builder/setup_parts.py,
builder/HookshotBuilder/Reuse.cs; no network, no release, no Setup started).

Two installs into one folder ran at once before, and the second one's sweep (undo_every_reuse) gave the installed version back
the folders the first had just moved into its <tag>.part: the first then finished without its Python. Now every install holds
a lock for the folder across its whole run, its sweep included, and the Hookshot Builder's Free takes it too.

Checked with real processes against real folders in a temporary place:
- the lock's name is one for each spelling of a folder (case, '/' or '\\', a separator at its end) and another for another
  folder, and setup.c, setup_parts.py and Reuse.cs name it the same (Windows);
- a second install of the Python builder waits a bounded time for a holder (here another process: Python, setup.c's own lock
  code, or Reuse.cs's) and then fails, saying so and naming the folder, with the holder's <tag>.part and its reused.txt left
  as they were (its sweep never ran); one that waits gets the folder of a holder that ends meanwhile, and the folder of a
  holder that was killed is taken; another folder is not refused. On Linux the lock is a flock'ed file, checked the same way
  between Python processes;
- setup.c's lock code is cut out of it and run as it is, and the source is read for what only a real setup would show: the
  lock is taken before the downloads folder and the sweep, a setup that cannot get it fails, fail() lets go of it, and it is
  let go of before the builder starts.

    python -I -B tests/test_builder_setup_lock.py [--cc gcc]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "builder" / "py"))
from hookshot_builder import setup_parts as sp  # noqa: E402

fails = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails.append(what)


PY_CHILD = r"""
import os, sys, time
sys.path.insert(0, %r)
from hookshot_builder import setup_parts as sp
mode, base = sys.argv[1], sys.argv[2]
os.environ["HOOKSHOT_SETUP_BASE"] = base
if mode == "name":
    print(sp.lock_name(base))
elif mode == "hold":
    lock = sp.take_lock(0, base)
    print("1" if lock else "0", flush=True)
    sys.stdin.read()
elif mode == "try":
    lock = sp.take_lock(int(sys.argv[3]) / 1000, base)
    print("1" if lock else "0")
elif mode == "install":
    # the Python builder's install, its wait made short: what it says, or "reached" when it got past the lock and the sweep
    sp.LOCK_WAIT = int(sys.argv[3]) / 1000
    class Reached(Exception):
        pass
    def manifest(status, tag=None, cancel=None):
        raise Reached()
    sp.read_manifest = manifest
    try:
        sp.install(lambda s: None)
    except sp.SetupError as e:
        print("error|" + str(e))
    except Reached:
        print("reached")
""" % str(ROOT / "builder" / "py")

C_FIXTURE = r"""
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
/* ACTUAL */
int wmain(int argc, wchar_t **argv) {
    (void)argc;
    if (!wcscmp(argv[1], L"name")) {
        wchar_t name[96];
        setup_lock_name(argv[2], name, 96);
        printf("%ls\n", name);
        return 0;
    }
    const int got = setup_lock_take(argv[2], (DWORD)_wtoi(argv[3]));
    printf("%d\n", got);
    fflush(stdout);
    if (!wcscmp(argv[1], L"hold")) {
        char buf[16];
        while (fread(buf, 1, sizeof buf, stdin) > 0) {}   /* (held until the test closes stdin) */
    }
    setup_lock_release();
    return 0;
}
"""

CS_FIXTURE = r"""
using System;
namespace HookshotBuilder;
static class Fixture {
    static int Main(string[] a) {
        if (a[0] == "name") { Console.WriteLine(Reuse.LockName(a[1])); return 0; }
        using var held = Reuse.TakeLock(a[1], int.Parse(a[2]));
        Console.WriteLine(held != null ? "1" : "0");
        Console.Out.Flush();
        if (a[0] == "hold") Console.In.ReadToEnd();
        return 0;
    }
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def check_source(src):
    work = between(src, "static DWORD WINAPI work(LPVOID unused)", "static LRESULT CALLBACK wndproc(")
    take = work.find("if (!setup_lock_take(base, 0))")
    check(0 <= take < work.index('swprintf(downloads, MAX_PATH, L"%ls\\\\downloads", base);')
          < work.index("if (!undo_every_reuse(versions, left, MAX_PATH))"),
          "setup.c: the setup's folder is taken before its downloads folder and before the sweep")
    wait = work[take:work.index('swprintf(downloads, MAX_PATH, L"%ls\\\\downloads", base);')]
    check("setup_lock_take(base, SETUP_LOCK_WAIT_MS)" in wait and "fail(L\"Another Hookshot Builder setup is installing" in wait,
          "setup.c: one that cannot get it waits SETUP_LOCK_WAIT_MS, then fails saying so")
    check(work.index("rmtree(downloads);") < work.index("setup_lock_release();") < work.index("CreateProcessW(NULL, cmd,"),
          "setup.c: it is let go of once installed, before the builder starts")
    fail = between(src, "static void fail(const wchar_t *fmt", "/* ---- HTTPS GET")
    check(fail.index("undo_reuse(g_versions_dir, g_part_dir)") < fail.index("setup_lock_release();") < fail.index("ExitProcess(1)"),
          "setup.c: fail() lets go of it after its undo")
    py = (ROOT / "builder/py/hookshot_builder/setup_parts.py").read_text(encoding="utf-8")
    inst = between(py, "def install(status", "def install_locked(")
    check("take_lock(LOCK_WAIT)" in inst and "lock.release()" in inst and "install_locked(" in inst,
          "setup_parts.install: the whole install (install_locked, its sweep included) runs with the lock held")
    check(between(py, "def install_locked(", "def unpack_parts(").count("undo_every_reuse()") == 1,
          "setup_parts.install_locked: the sweep is there")


def child(cmd, *args, timeout=120):
    r = subprocess.run(cmd + list(args), capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    return r.stdout.strip()


def hold(cmd, base):
    p = subprocess.Popen(cmd + ["hold", base, "0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8")
    return p, p.stdout.readline().strip()


def lent_part(base, tag="v3"):
    """an install of tag in progress: its <tag>.part holds the installed version's python and the note"""
    part = Path(base) / "versions" / (tag + ".part")
    (part / "HookshotBuilder" / "tools" / "python").mkdir(parents=True, exist_ok=True)
    (part / "HookshotBuilder" / "tools" / "python" / "python.exe").write_bytes(b"py")
    (part / sp.REUSED).write_text("v1\nHookshotBuilder\\tools\\python\n", encoding="utf-8")
    return part


def exercise(label, holder, folder, other):
    """a holder of folder (holder + [mode, folder, wait_ms]) against the Python builder's install and its lock"""
    py = [sys.executable, "-I", "-B", "-c", PY_CHILD]
    part = lent_part(folder)
    p, said = hold(holder, folder)
    try:
        check(said == "1", "%s: the first one takes the folder" % label)
        t0 = time.monotonic()
        got = child(py, "install", folder.upper() + "\\", "1500")
        took = time.monotonic() - t0
        check(got.startswith("error|Another Hookshot Builder setup is installing into") and folder.lower() in got.lower(),
              "%s: a second install fails, saying so and naming the folder (%s)" % (label, got[:160]))
        check(1.4 <= took < 30, "%s: after waiting a bounded time (%.1f s)" % (label, took))
        check((part / sp.REUSED).is_file() and (part / "HookshotBuilder" / "tools" / "python" / "python.exe").is_file()
              and not (Path(folder) / "versions" / "v1" / "HookshotBuilder" / "tools" / "python").exists(),
              "%s: the holder's <tag>.part keeps its note and its folders (the second one's sweep never ran)" % label)
        check(child(py, "try", other, "0") == "1", "%s: another folder is not refused" % label)
        waiter = subprocess.Popen(py + ["install", folder, "15000"], stdout=subprocess.PIPE, text=True, encoding="utf-8")
        time.sleep(1.0)
        p.stdin.close()   # (the holder ends while the second one waits)
        p.wait(timeout=60)
        out = waiter.communicate(timeout=60)[0].strip()
        check(out == "reached", "%s: one that waits gets the folder of a holder that ends meanwhile (%s)" % (label, out[:120]))
        check((Path(folder) / "versions" / "v1" / "HookshotBuilder" / "tools" / "python" / "python.exe").is_file()
              and not (part / sp.REUSED).exists(), "%s: and its sweep then gives the installed version its folders back" % label)
    finally:
        if p.poll() is None:
            p.kill()
    p, said = hold(holder, folder)
    p.kill()   # (TerminateProcess: a mutex is abandoned, a flock goes with the process)
    p.wait(timeout=60)
    check(child(py, "try", folder, "2000") == "1", "%s: the folder of a holder that was killed is taken" % label)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="gcc")
    a = ap.parse_args()
    src = (ROOT / "builder/setup/setup.c").read_text(encoding="utf-8")
    check_source(src)
    py = [sys.executable, "-I", "-B", "-c", PY_CHILD]
    with tempfile.TemporaryDirectory(prefix="triforce-setup-lock-") as tmp:
        folders = []

        def fresh(name):
            d = os.path.join(tmp, name, "HookshotBuilder")
            os.makedirs(os.path.join(d, "versions"))
            folders.append(d)
            return d

        exercise("python", py, fresh("py"), fresh("other"))
        if os.name != "nt":
            print("NOT RUN: setup.c's and Reuse.cs's locks (Windows only)")
        else:
            cc = shutil.which(a.cc) or shutil.which("clang") or a.cc
            Path(tmp, "f.c").write_text(C_FIXTURE.replace("/* ACTUAL */", between(
                src, "/* (Triforce) One setup at a time for a setup folder", "/* The checksums of the parts")), encoding="utf-8")
            exe = os.path.join(tmp, "setup_lock.exe")
            subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wno-unused-function", "-municode", "-DUNICODE", "-D_UNICODE",
                            os.path.join(tmp, "f.c"), "-o", exe], check=True, timeout=300)
            cmds = {"python": py, "setup.c": [exe]}
            import setup_cs
            csc = setup_cs.find_csc()
            if not csc:
                why = "no C# compiler here (tests/setup_cs.py): Reuse.cs's lock was not run"
                if os.environ.get("CI"):
                    raise SystemExit(why)
                print("NOT RUN: " + why)
            else:
                Path(tmp, "f.cs").write_text(CS_FIXTURE, encoding="utf-8")
                bexe = os.path.join(tmp, "reuse_lock.exe")
                subprocess.run([csc, "/nologo", "/target:exe", "/langversion:latest", "/nullable:enable", "/out:" + bexe,
                                "/reference:" + os.path.join(setup_cs.FRAMEWORK, "System.dll"),
                                "/reference:" + os.path.join(setup_cs.FRAMEWORK, "System.Core.dll"),
                                str(ROOT / "builder" / "HookshotBuilder" / "Reuse.cs"), os.path.join(tmp, "f.cs")],
                               check=True, timeout=300, stdout=subprocess.DEVNULL)
                cmds["Reuse.cs"] = [bexe]
            base, other = folders
            for label, cmd in cmds.items():
                names = {child(cmd, "name", s) for s in (base, base.lower() + "\\", base.upper().replace("\\", "/"))}
                check(len(names) == 1 and next(iter(names)).startswith("Local\\HookshotBuilder-Setup-"),
                      "%s: one name for each spelling of a folder %s" % (label, sorted(names)))
                check(child(cmd, "name", other) not in names, "%s: another name for another folder" % label)
            every = {label: child(cmd, "name", base) for label, cmd in cmds.items()}
            check(len(set(every.values())) == 1, "setup.c, setup_parts.py and Reuse.cs name the lock the same %s" % every)
            for label in ("setup.c", "Reuse.cs"):
                if label in cmds:
                    exercise(label, cmds[label], fresh(label.replace(".", "_")), other)
            # (and the other way round: a Python install holds the folder, setup.c and Free's lock are refused it)
            p, said = hold(py, base)
            try:
                for label in ("setup.c", "Reuse.cs"):
                    if label in cmds:
                        check(child(cmds[label], "try", base, "300") == "0", "%s: refused the folder a Python install holds" % label)
            finally:
                p.stdin.close()
                p.wait(timeout=60)
    if fails:
        raise SystemExit("%d failed" % len(fails))
    print("all passed")


if __name__ == "__main__":
    main()
