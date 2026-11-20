"""The parallel replay's spins (the third performance audit's P6, after NX2navi's replay workers): a helper thread with no
segment to record spins for the next before it sleeps on the pool's condition variable (gpu/vk/vk_replay.cpp
par_worker_main), and the replay thread spins for a segment a helper records before it sleeps for it (replay_handle), both
bounded by RX_VK_HELPER_SPIN_US (100 us; 0: off, as before; gpu/vk/helper_spin.h). The spins only make a wait shorter: the
condition variable's wait with its predicate follows each as before.

gpu/vk/helper_spin.h and vk_replay.cpp's ParPool and par_worker_main (cut out unchanged) run in tests/helper_spin_test.cpp:
the bound and until() alone; a helper that spins begins a job handed over with no wake at all, at once, not after the bound;
with the switch at 0 every wait goes to the condition variable; four helpers and thousands of hand-overs run each job once
and leave the count the spinners read at the queue's. replay_handle's side is read: the count is stored under the pool's
lock before the wake, the segment wait spins before its wait, and the frame statistics have the spins' line.

    python -I -B tests/test_helper_spin.py [--cxx clang++] [--source gpu/vk/vk_replay.cpp] [--threads-only]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
CASES = {"until": None, "spin": "1000000", "off": "0", "many": None, "many-off": "0"}


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


def in_order(text, parts, what):
    at = 0
    for p in parts:
        found = text.find(p, at)
        if found < 0:
            fail("%s: %r missing or out of order" % (what, p))
        at = found + len(p)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_replay.cpp")
    parser.add_argument("--threads-only", action="store_true",
                        help="only the cases on threads, not the reading of the source (to try a changed copy of it)")
    args = parser.parse_args()
    replay = args.source.read_text(encoding="utf-8")
    pool = replay[replay.index("struct ParPool {"):]
    pool = pool[:pool.index(block(pool, "void par_worker_main() {")) + len(block(pool, "void par_worker_main() {"))]
    try:
        if not args.threads_only:
            read_source(replay)
    except ValueError as e:
        fail("a text this test reads moved (%s)" % e)
    run(args, pool)


def read_source(replay):
    """replay_handle's side and the frame statistics (the cases on threads run ParPool and par_worker_main)"""
    handle = block(replay, "const uint8_t* replay_handle(VkCommandBuffer& cmd, const uint8_t* h, bool may_stop = false) {")
    # (the jobs' count stored with the lock held, after the jobs and before the wake)
    in_order(handle, ["std::lock_guard lk(par_pool.mu);", "par_pool.pending++;",
                      "par_pool.queued.store((int)par_pool.jobs.size(), std::memory_order_release);", "\n    }\n",
                      "par_pool.cv.notify_all();"], "replay_handle's hand-over")
    # (the segment wait: the spin, then the wait as before, then the submit)
    in_order(handle, ["if (!init_lost && par_own_on() && !j->taken.exchange(true)) {",
                      "helper_spin::until([j] { return j->done.load(std::memory_order_acquire); }, helper_spin::spin_ns(),",
                      "seg_cv.wait(l, [j] { return j->done.load(); });", "vkQueueSubmit2(dev.queue, 1, &si, j->r->fence)"],
             "replay_handle's segment wait")
    stats = block(replay, "void par_stats_line(int frames, int used) {")
    if "its spins (RX_VK_HELPER_SPIN_US=" not in stats or "par_pool.spun.load()" not in stats or "par_st.seg_spun.load()" not in stats:
        fail("the frame statistics no longer have the spins' line")


def run(args, pool):
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    env = os.environ.copy()
    flags = 0
    if os.name == "nt":
        flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
        for name in ("INCLUDE", "LIB", "LIBPATH"):
            env.pop(name, None)
        windows = Path(os.environ["SystemRoot"])
        env["PATH"] = os.pathsep.join(map(str, (Path(compiler).resolve().parent, windows / "System32", windows)))
    env.pop("RX_VK_HELPER_SPIN_US", None)
    failed = []
    with tempfile.TemporaryDirectory(prefix="helper-spin-") as temporary:
        folder = Path(temporary)
        (folder / "par_pool.inc").write_text(pool + "\n", encoding="utf-8")
        binary = folder / "fixture.exe"
        command = [compiler, "-std=c++20", "-O2", "-Wall", "-Werror", "-pthread", "-I" + str(ROOT / "gpu/vk"), "-I" + str(folder),
                   str(ROOT / "tests/helper_spin_test.cpp"), "-o", str(binary)]
        built = subprocess.run(command, env=env, creationflags=flags, capture_output=True, text=True, timeout=180)
        if built.returncode:
            print(built.stdout + built.stderr)
            fail("the fixture did not build")
        for case, spin in CASES.items():
            case_env = dict(env)
            if spin is not None:
                case_env["RX_VK_HELPER_SPIN_US"] = spin
            try:
                done = subprocess.run([str(binary), case], env=case_env, creationflags=flags, capture_output=True, text=True,
                                      encoding="utf-8", errors="replace", timeout=120)
            except subprocess.TimeoutExpired:
                print("%s: no end in 120 s" % case)
                failed.append(case)
                continue
            print(done.stdout, end="")
            print(done.stderr, end="", file=sys.stderr)
            if done.returncode or "PASS " + case not in done.stdout:
                failed.append(case)
    if failed:
        fail("the parallel replay's spins: " + ", ".join(failed))
    print("PASS: a helper that spins begins a job handed over meanwhile without a wake, RX_VK_HELPER_SPIN_US=0 as before, "
          "every job once; replay_handle stores the count before the wake and spins before its segment wait (%d cases)"
          % len(CASES))


if __name__ == "__main__":
    main()
