"""Virtual cores: a waiter that asked a holder for its core spins for it before it sleeps (host/hos_thread.c vc_spin, the
third performance audit's P1, after NX2navi's core_take), and a yield with nobody waiting takes no lock (NX2navi's yield fast
path). The spin changes how soon a granted waiter runs, never who gets a core.

host/hos_thread.c's virtual cores (vc_lock to vc_yield) are cut out unchanged and run on real threads by
tests/vcore_spin_test.c, with pthreads for Windows' SRW lock and condition variable: threads hand cores to each other tens
of thousands of times with the spin, without it (HOS_VCORE_SPIN_US=0) and with one spinner at most, and no core is ever held
twice, no thread holds two or runs without one, no grant is slept through or lost; scripted hand-offs show a wait ending in
the spin at the grant, the old behaviour with the switch off, the 250 us default, no spin on a PC of 8 logical processors,
the spinner cap, and the yield's fast path. The scripted cases run on the fixture's own clock, which moves only when a case
moves it, and wait for states (a request made, a thread spinning or asleep), never for a time: a loaded PC does not change
what they see (the cap case failed once beside 14 other tests while it depended on a 30 ms window). Every vcores: line the
cases log is read here: each wait ended in the spin or slept, and the spin's counts are there.

    python -I -B tests/test_vcore_spin.py [--cc gcc] [--source host/hos_thread.c] [--threads-only]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
FAR = "60000000"   # (60 s: no escape in these cases)
CASES = {
    "storm": {"HOS_VCORE_SPIN_US": "250", "HOS_VCORE_SPIN_MAX": "8"},
    "storm-off": {"HOS_VCORE_SPIN_US": "0"},
    "storm-tight": {"HOS_VCORE_SPIN_US": "250", "HOS_VCORE_SPIN_MAX": "1"},
    "grant-in-spin": {"HOS_VCORE_SPIN_US": "1000000", "HOS_VCORE_SPIN_MAX": "4"},
    "grant-off": {"HOS_VCORE_SPIN_US": "0", "HOS_VCORE_SPIN_MAX": "4"},
    "default": {},
    "small-pc": {"TEST_LOGICAL": "8"},
    "cap": {"HOS_VCORE_SPIN_US": "1000000", "HOS_VCORE_SPIN_MAX": "1"},
    "cap-default": {},
    "yield-fast": {"HOS_VCORE_SLICE_US": "10000000"},
}
LINE = re.compile(r"vcores: (\d+) takes, (\d+) waits \(mean \d+ us, longest [\d.]+ ms\), (\d+) kicks, (\d+) yields, (\d+) escapes "
                  r"in \d+ s; (\d+) of the waits ended in the spin \(([\d.]+) ms spun, (\d+) spins left out with "
                  r"HOS_VCORE_SPIN_MAX threads spinning\), (\d+) slept; a granted core ran ([\d.]+) us after its grant after a spin "
                  r"\((\d+)\), ([\d.]+) us after a sleep \((\d+)\), longest (\d+) us$")


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def fail(message):
    raise SystemExit("FAIL: " + message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--source", type=Path, default=ROOT / "host/hos_thread.c")
    parser.add_argument("--threads-only", action="store_true",
                        help="only the cases on threads, not the reading of the source (to try a changed copy of it)")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    cores = source[source.index("static SRWLOCK vc_lock = SRWLOCK_INIT;"):source.index("int hos_vcore_away(void) {")]
    try:
        if not args.threads_only:
            read_source(cores)
    except ValueError as e:
        fail("a text this test reads moved (%s)" % e)
    run(args, cores)


def read_source(cores):
    """Where the pieces sit (the cases on threads run them)"""
    acquire = block(cores, "static void vc_acquire(HosThread *t) {")
    spin_at = acquire.index("if (vc_spin(t, spin)) {")
    if not acquire.index("vc_st.kicks++;") < spin_at < acquire.index("SleepConditionVariableSRW(&t->vc_cv, &vc_lock, 1, 0);"):
        fail("vc_acquire no longer spins after asking the holder and before it sleeps")
    after = acquire[spin_at:]
    if not after.index("if (t->vc_held) {") < after.index("vc_free_for(t)"):
        fail("after the spin vc_acquire looks for a free core before it looks at the core it was granted (a core held twice)")
    grant = block(cores, "static void vc_grant(int c) {")
    if grant.index("best->vc_grant_at = hos_now_ns();") > grant.index("vc_take(best, c);"):
        fail("vc_grant stamps the grant after it hands the core over: a spinner could run before the stamp")
    take = block(cores, "static void vc_take(HosThread *t, int c) {")
    if "__atomic_store_n(&t->vc_held, c + 1, __ATOMIC_RELEASE);" not in take:
        fail("vc_take no longer publishes vc_held for the spinner that reads it without the lock")
    yld = block(cores, "static void vc_yield(HosThread *t, int strict) {")
    fast = "if (!__atomic_load_n(&vc_nq, __ATOMIC_RELAXED) && !t->vc_kick) return;"
    if fast not in yld or yld.index(fast) > yld.index("AcquireSRWLockExclusive(&vc_lock);"):
        fail("vc_yield no longer looks for waiters before it takes the lock")


def run(args, cores):
    compiler = shutil.which(args.cc)
    if not compiler:
        raise SystemExit("C compiler not found: " + args.cc)
    env = os.environ.copy()
    flags = 0
    if os.name == "nt":
        flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
        for name in ("INCLUDE", "LIB", "LIBPATH"):
            env.pop(name, None)
        windows = Path(os.environ["SystemRoot"])
        env["PATH"] = os.pathsep.join(map(str, (Path(compiler).resolve().parent, windows / "System32", windows)))
    for name in list(env):
        if name.startswith("HOS_VCORE") or name in ("HOS_CORES", "TEST_LOGICAL"):
            del env[name]
    failed = []
    with tempfile.TemporaryDirectory(prefix="vcore-spin-") as temporary:
        folder = Path(temporary)
        (folder / "vcore_block.inc").write_text(cores, encoding="utf-8")
        binary = folder / "fixture.exe"
        command = [compiler, "-std=gnu17", "-O2", "-Wall", "-Werror", "-pthread", "-I" + str(folder),
                   str(ROOT / "tests/vcore_spin_test.c"), "-o", str(binary)] + (["-lwinmm"] if os.name == "nt" else [])
        built = subprocess.run(command, env=env, creationflags=flags, capture_output=True, text=True, timeout=120)
        if built.returncode:
            print(built.stdout + built.stderr)
            fail("the fixture did not build")
        for case, extra in CASES.items():
            case_env = dict(env, HOS_VCORE_ESCAPE_US=FAR, HOS_VCORE_ESCAPE_EQ_US=FAR, **extra)
            began = time.monotonic()
            try:
                done = subprocess.run([str(binary), case], env=case_env, creationflags=flags, capture_output=True, text=True,
                                      encoding="utf-8", errors="replace", timeout=120)
            except subprocess.TimeoutExpired:
                print("%s: no end in 120 s (a core never handed over, or a lock never let go)" % case)
                failed.append(case)
                continue
            out = done.stdout
            lines = [LINE.search(line) for line in out.splitlines() if line.startswith("LOG: vcores: ") and " takes, " in line]
            problems = []
            if done.returncode or "PASS " + case not in out:
                problems.append("the fixture failed (exit %d)" % done.returncode)
            if any(m is None for m in lines):
                problems.append("a vcores: line not as this test reads it")
            lines = [m for m in lines if m]
            for m in lines:
                waits, spun, slept = int(m.group(2)), int(m.group(6)), int(m.group(9))
                if waits != spun + slept:
                    problems.append("%d waits, %d ended in the spin, %d slept: some neither" % (waits, spun, slept))
                if int(m.group(5)):
                    problems.append("an escape")
            spun = sum(int(m.group(6)) for m in lines)
            spun_ms = sum(float(m.group(7)) for m in lines)
            waits = sum(int(m.group(2)) for m in lines)
            if case.startswith("storm"):
                if waits < 300:
                    problems.append("only %d waits: the threads did not contend" % waits)
                if case == "storm-off" and (spun or spun_ms):
                    problems.append("HOS_VCORE_SPIN_US=0 spun")
                if case != "storm-off" and not spun:
                    problems.append("no wait ended in the spin")
            if problems:
                print(out[-6000:])
                print(done.stderr[-2000:], file=sys.stderr)
                print("%s: %s" % (case, "; ".join(problems)))
                failed.append(case)
            else:
                print("%s: %d waits, %d ended in the spin (%.1f s)" % (case, waits, spun, time.monotonic() - began))
    if failed:
        fail("virtual cores with the spin: " + ", ".join(failed))
    print("PASS: no core held twice, no grant lost, waits end in the spin at their grant, HOS_VCORE_SPIN_US=0 as before, "
          "the spinner cap, and the yield's fast path (%d cases)" % len(CASES))


if __name__ == "__main__":
    main()
