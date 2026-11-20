"""The lift's emit in spawned processes (nx2/emit_pool.py: Windows has no fork) writes what one process writes, and fails whole.

Authored code only: a few hundred small AArch64 functions and three giant ones made here from a seed (adds, compares, stores,
loads, loops, calls, tail calls, an indirect jump, a setjmp owner, a hooked function, an untranslated word), an authored
translator in a64emit's place (no compiler is needed), and the lifter's own discovery, emit_function, write(), chunk split and
page marks over them.

  - the planner: every function in exactly one run, the runs in order, none past its limits but by its last function;
  - the texts a run is sent are exactly the ones the lifter's emit reads for it;
  - the answers come back in the order asked, whichever process ends first, and no more is asked ahead than the processes
    and AHEAD;
  - a lift by 3 and by 5 processes, in runs that neither count divides, twice into the same folder (the chunk boundaries from
    the sizes, then kept): every file of the lifted C and of the image folder, the lines printed and the counters with their
    keys' order are those of one process;
  - a worker that raises, one that dies, one that does not answer and one whose lifter class was set up otherwise than the
    lift's process each end the lift with SystemExit and what was said, soon, and leave no lifted file behind; so does a
    process failing at the page marks;
  - a lift that ends on an error of its own while the page marks are being made leaves every file whole and marked;
  - the processes of a lift that was stopped from outside end by themselves;
  - HOOKSHOT_LIFT_JOBS chooses the count, 1 is one process, the default is half the processors (at most 8, one a GB), and
    neither it nor any name the pool reads is part of the lift key.

    python -I -B tests/test_spawn_emit.py
"""
import ast
import contextlib
import hashlib
import io
import os
import random
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import kit  # noqa: E402,F401  (a64-kit on the path)
import a64emit  # noqa: E402
import a64_lift  # noqa: E402
import chunks  # noqa: E402
import emit_pool  # noqa: E402
import poll  # noqa: E402

BASE = 0x80000000
RET, BLR_X8, BR_X16, UDF = 0xD65F03C0, 0xD63F0100, 0xD61F0200, 0x00000000
STALE = ("code_098.c", "code_098_1.c", "code_hot.c", "code_hot_3.c", "code_hot.c.whole", "code_002.c.part")   # (what a lift's C is)


def check(what, ok):
    if not ok:
        raise SystemExit("FAIL: " + what)


# ---- the authored code -------------------------------------------------------------------------------------------------------
def function(rng, at, size, starts):
    w = [0xF90003FD, 0x910003FD]   # str x29, [sp]; add x29, sp, #0
    while len(w) < size - 1:
        pc, r = at + 4 * len(w), rng.random()
        room = size - 1 - len(w)
        if r < 0.30:
            w.append(0x91000000 | rng.randrange(1, 4000) << 10 | rng.randrange(16) << 5 | rng.randrange(16))   # add
        elif r < 0.42:
            w.append(0xF1000000 | rng.randrange(1, 64) << 10 | rng.randrange(16) << 5 | rng.randrange(16))     # subs
        elif r < 0.60 and room >= 2:
            n = rng.choice((1, 2, 3, 29, 31))
            for k in rng.sample(range(8), 2):                                                                  # two stores, one base
                w.append(0xF9000000 | k << 10 | n << 5 | rng.randrange(16))
        elif r < 0.68:
            w.append(0xF9400000 | rng.randrange(8) << 10 | rng.randrange(1, 4) << 5 | rng.randrange(16))        # ldr
        elif r < 0.74:
            w.append(0x10000000 | rng.randrange(64) << 5 | rng.randrange(16))                                   # adr
        elif r < 0.80 and len(w) > 4:
            back = at + 4 * rng.randrange(2, len(w))
            w.append(0x54000000 | (((back - pc) >> 2) & 0x7FFFF) << 5 | rng.randrange(14))                      # b.cond, backward
        elif r < 0.85 and room >= 3:
            w.append(0xB4000000 | 2 << 5 | rng.randrange(16))                                                   # cbz over one
        elif r < 0.93:
            t = rng.choice(starts) if rng.random() < 0.97 else BASE - 0x1000                                    # bl (rarely to no code)
            w.append(0x94000000 | (((t - pc) >> 2) & 0x3FFFFFF))
        elif r < 0.95:
            w.append(BLR_X8)
        elif r < 0.97:
            w.append(0xD4000001 | rng.randrange(128) << 5)                                                      # svc
        else:
            w.append(UDF)
    pc, r = at + 4 * len(w), rng.random()
    if r < 0.80:
        w.append(RET)
    elif r < 0.95:
        w.append(0x14000000 | (((rng.choice(starts) - pc) >> 2) & 0x3FFFFFF))                                   # b, a tail call
    else:
        w.append(BR_X16)
    return w


def program(seed=7, count=260):
    rng = random.Random(seed)
    sizes = [2500 if i in (40, 41, 150) else rng.randrange(4, 60) for i in range(count)]
    starts, at = [], BASE
    for n in sizes:
        starts.append(at)
        at += 4 * n
    words = []
    for a, n in zip(starts, sizes):
        words += function(rng, a, n, starts)
    return words, starts


class Image:
    def __init__(self):
        self.words, starts = program()
        self.entries = set(starts)
        self.hooks = {starts[17]: "hos_authored_hook"}
        self.names = {starts[23]: "__setjmp", starts[5]: "authored_five"}

    def in_code(self, pc):
        return BASE <= pc < BASE + 4 * len(self.words) and not pc & 3

    def word(self, pc):
        return self.words[(pc - BASE) >> 2]


def translate(items, batch=65536):
    """a64emit.translate's stand-in: C in the lifter's dialect for the authored words (what the lifter's later passes look
    for: flag stores, register writes, stores through a base plus a constant)"""
    texts, flags = [], []
    for pc, w in items:
        d, n, imm, top = w & 31, (w >> 5) & 31, (w >> 10) & 0xFFF, w >> 24
        f = 0
        if top == 0x91:
            t = "{ uint64_t _a=c->x[%d], _b=%dULL; uint64_t _r=_a+_b; c->x[%d]=_r; }\n" % (n, imm, d)
        elif top == 0xF1:
            t = ("{ uint64_t _a=c->x[%d], _b=%dULL; uint64_t _r=_a-_b; c->n = (_r>>63)&1; c->z = _r==0; c->c = _a>=_b; "
                 "c->v = 0; c->x[%d]=_r; }\n" % (n, imm, d))
        elif w >> 22 == 0x3E4:
            t = "recomp_store64(c,(c->x[%d]+%dULL),c->x[%d]);\n" % (n, imm * 8, d)
        elif w >> 22 == 0x3E5:
            t = "c->x[%d] = recomp_load64(c,(c->x[%d]+%dULL));\n" % (d, n, imm * 8)
        elif top & 0x9F == 0x10:
            t = "c->x[%d] = 0x%xULL;\n" % (d, pc + 4 * ((w >> 5) & 0x7FFFF))
        else:
            t, f = "", a64emit.UNHANDLED
        texts.append(t)
        flags.append(f)
    return texts, flags


class Failing(a64_lift.Lifter):
    """The stand-in worker of the failure cases: the lifter, whose emit of one function raises, ends its process or never
    returns (how, at: set on the lifter, so each worker is sent them)"""
    how = at = None

    def emit_function(self, e, texts):
        if e == self.at:
            if self.how == "raise":
                raise ValueError("authored failure in f_%08x" % e)
            if self.how == "die":
                os._exit(9)
            time.sleep(600)
        return super().emit_function(e, texts)


class Plain:
    """a lifter with nothing to send, for the pool alone"""


def late(i, delay):
    time.sleep(delay)
    return i, os.getpid()


# ---- a lift ------------------------------------------------------------------------------------------------------------------
def lift(folder, jobs, cls=a64_lift.Lifter, setup=(poll.install,), **fail):
    """One lift of the authored code into folder, as nx2/lift.py does it (write, edit_c without waiting for the page marks,
    then marks_done): the files it leaves, the lines it printed and its counters, or the SystemExit that ended it"""
    L = cls(Image())
    L.after[sorted(L.entries)[3] + 4] = "c->x[9] = 1;   /* an authored patch after an instruction */"
    vars(L).update(fail)
    L.discover()
    poll.stats["functions"] = poll.stats["polls"] = 0
    gen, image = os.path.join(folder, "gen"), os.path.join(folder, "image")
    os.makedirs(image, exist_ok=True)
    said = io.StringIO()
    try:
        with contextlib.redirect_stdout(said):
            chunks.write(L, gen, image, counters=[poll.stats], jobs=jobs, setup=list(setup))
            chunks.edit_c(gen, L.stats, False, whole=False, wait=False)
            chunks.marks_done()
    except SystemExit as e:
        return e
    files = {}
    for top in (gen, image):
        for name in sorted(os.listdir(top)):
            with open(os.path.join(top, name), "rb") as f:
                files[os.path.basename(top) + "/" + name] = f.read()
    return files, said.getvalue(), [list(c.items()) for c in (L.stats, L.unhandled, L.ijump, poll.stats)]


def same(what, one, many):
    check("%s: the lift ended with %r" % (what, many), isinstance(many, tuple))
    check("%s: the files are %s, one process wrote %s" % (what, sorted(many[0]), sorted(one[0])), sorted(many[0]) == sorted(one[0]))
    for name in one[0]:
        check("%s: %s differs from one process's (%s and %s)" % (what, name, hashlib.sha1(many[0][name]).hexdigest()[:12],
                                                                  hashlib.sha1(one[0][name]).hexdigest()[:12]),
              many[0][name] == one[0][name])
    said = many[1].splitlines(True)
    count = [line for line in said if line.startswith("emit: ")]   # (the one line of its own: how many processes)
    check("%s: it does not say how many processes: %s" % (what, count), len(count) == 1 and " processes" in count[0])
    said = "".join(line for line in said if line not in count)
    check("%s: the lines printed differ:\n%s\nfrom one process's:\n%s" % (what, said, one[1]), said == one[1])
    for name, a, b in zip(("stats", "unhandled", "ijump", "the polls"), one[2], many[2]):
        check("%s: %s are %s, one process counted %s" % (what, name, b, a), a == b)


def lifted(folder):
    gen = os.path.join(folder, "gen")
    return sorted(f for f in os.listdir(gen) if f.startswith("code_")) if os.path.isdir(gen) else []


def failed(what, got, *words, within=60, took=0.0):
    check("%s: the lift did not fail (%s)" % (what, type(got).__name__), isinstance(got, SystemExit))
    for w in words:
        check("%s: its message does not say %r: %s" % (what, w, got), w in str(got))
    check("%s: it took %.0f s" % (what, took), took < within)
    check("%s: the processes were not ended" % what, chunks._workers is None)


# ---- the checks --------------------------------------------------------------------------------------------------------------
def test_plan():
    rng = random.Random(3)
    order = list(range(0, 4000, 4))
    size = {e: (9000 if e % 1000 == 0 else rng.randrange(1, 40)) for e in order}
    for most_funcs, most_size in ((7, 150), (2000, 120000), (1, 1), (50, 10 ** 9), (10 ** 9, 500)):
        runs = emit_pool.plan(order, size.get, most_funcs, most_size)
        check("the runs joined are the order", [e for r in runs for e in r] == order)
        check("no run is empty", all(runs))
        for r in runs:
            check("a run of %d functions past %d" % (len(r), most_funcs), len(r) <= most_funcs)
            check("a run past %d instructions before its last function" % most_size, sum(size[e] for e in r[:-1]) < most_size)
    check("a giant function ends its run", all(r[-1] % 1000 == 0 or sum(size[e] for e in r) < 9000
                                                for r in emit_pool.plan(order, size.get, 2000, 5000)))
    check("no functions, no runs", emit_pool.plan([], size.get) == [])


def test_texts_sent():
    """What a run is sent of the texts is what the lifter's emit reads for it: more is only weight, less is a KeyError there"""
    L = a64_lift.Lifter(Image())
    L.discover()
    keys = {}
    for body, _ in L.funcs.values():
        for pc, (k, w) in body.items():
            if k[0] == "op":
                keys.setdefault((w, pc) if a64_lift.pc_relative(w) else w, (pc, w))
    tx, fl = translate(list(keys.values()))
    texts = dict(zip(keys, zip((a64_lift.op_text(t, set()) for t in tx), fl)))

    class Seen(dict):
        def __getitem__(self, k):
            self.read.add(k)
            return dict.__getitem__(self, k)
    order = sorted(L.funcs)
    for es in emit_pool.plan(order, lambda e: len(L.funcs[e][0]), 7, 150)[::5]:
        seen = Seen(texts)
        seen.read = set()
        for e in es:
            L.emit_function(e, seen)
        sent = emit_pool.texts_of(L.funcs, es, texts)
        check("the texts sent for a run (%d) are the ones its emit read (%d)" % (len(sent), len(seen.read)), set(sent) == seen.read)
        check("a text sent is the text", all(sent[k] is texts[k] for k in sent))


def test_order_of_answers():
    w = emit_pool.Workers(3, Plain())
    try:
        asked = []

        def calls():
            for i in range(14):
                asked.append(i)
                # (the first is the slowest: others end before it; and none is over at once, so that a second process, which
                # takes a moment to start on a busy PC, finds calls left)
                yield "call %d" % i, (i, 0.5 if i == 0 else 0.15)
        got, ahead = [], []
        for i, pid in w.each(late, calls()):
            got.append((i, pid))
            ahead.append(len(asked) - len(got))
        check("the answers are in the order asked: %s" % [i for i, _ in got], [i for i, _ in got] == list(range(14)))
        check("no more than the processes and AHEAD were asked ahead (%d)" % max(ahead), max(ahead) <= 3 + emit_pool.AHEAD)
        check("more than one process answered", len({pid for _, pid in got}) > 1)
        check("the processes stay for the next step", w.pool is not None)
        check("a second round, by the same processes", [i for i, _ in w.each(late, (("again", (i, 0.0)) for i in range(5)))] == list(range(5)))
    finally:
        w.stop()
    check("stopped", w.pool is None)


def test_the_same_c(tmp):
    one = [lift(os.path.join(tmp, "one"), 1) for _ in range(2)]
    check("one process: the lift ended with %r" % (one[0],), isinstance(one[0], tuple))
    names = sorted(one[0][0])
    check("several chunks, one cut in parts, a hot file, the table: %s" % names,
          len([n for n in names if n.startswith("gen/code_0")]) >= 8 and any("_1.c" in n for n in names)
          and {"gen/code_hot.c", "gen/a64_table.c", "gen/a64_hooks.c", "gen/a64_fpx.h", "gen/functions.txt", "gen/chunk_costs.txt",
               "image/chunks.txt"} <= set(names))
    check("the boundaries were kept the second time", "boundaries from sizes (saved)" in one[0][1] and "boundaries kept" in one[1][1])
    stats = dict(one[0][2][0])
    check("the authored code reaches the counters: %s, polls %s" % (stats, one[0][2][3]),
          all(stats.get(k) for k in ("bad_target", "dead_flag_stores", "after_patches", "setjmp_call_sites", "groups"))
          and one[0][2][1] and one[0][2][2] and dict(one[0][2][3])["polls"])
    check("the page marks changed the C", b"A64_MARK((c->x[" in b"".join(v for k, v in one[0][0].items() if k.startswith("gen/code_")))
    for jobs in (3, 5):
        folder = os.path.join(tmp, "many%d" % jobs)
        for n in range(2):
            many = lift(folder, jobs)
            check("the processes of the lift are ended after it", chunks._workers is None)
            same("%d processes, lift %d" % (jobs, n + 1), one[n], many)
    return one[0]


def test_failures(tmp, one):
    order = sorted(a64_lift.Lifter(Image()).entries)
    for how, words in (("raise", ("authored failure in f_%08x" % order[200], "ValueError", "the emit of f_")),
                       ("die", ("stopped before it answered",)),
                       ("hang", ("gave no answer",))):
        folder = os.path.join(tmp, how)
        os.makedirs(os.path.join(folder, "gen"))
        for name in STALE + ("a64_table.c",):   # (an earlier lift's: the C goes with a lift that failed, the rest is not its to take)
            Path(folder, "gen", name).write_text("void f_80000000(Ctx *__restrict A64_NOESC c) {\n}\n")
        t = time.time()
        with mock.patch.object(emit_pool, "WAIT", 4 if how == "hang" else emit_pool.WAIT), mock.patch.object(emit_pool, "SLICE", 0.5):
            got = lift(folder, 3, cls=Failing, how=how, at=order[200])
        failed("a worker that %ss" % how, got, *words, took=time.time() - t)
        check("a worker that %ss: lifted files are left: %s" % (how, lifted(folder)), lifted(folder) == [])
        check("a worker that %ss: the other files stay" % how, os.path.exists(os.path.join(folder, "gen", "a64_table.c")))
    # (the lift's process has the polls on its lifter class and does not name what put them there: the workers' class would emit
    # without them)
    folder = os.path.join(tmp, "setup")
    got = lift(folder, 3, setup=())
    failed("a lifter class set up otherwise", got, "poll.install.<locals>.emit_function", "setup")
    check("a lifter class set up otherwise: lifted files are left", lifted(folder) == [])
    # (the page marks: a file the worker cannot read, a folder with a chunk's name; the patches before them are not this test's)
    folder = os.path.join(tmp, "marks")
    L = a64_lift.Lifter(Image())
    L.discover()
    gen, image = os.path.join(folder, "gen"), os.path.join(folder, "image")
    os.makedirs(image)
    with contextlib.redirect_stdout(io.StringIO()):
        chunks.write(L, gen, image, counters=[poll.stats], jobs=3, setup=[poll.install])
    check("the processes stay between write() and edit_c()", chunks._workers is not None)
    os.mkdir(os.path.join(gen, "code_001_7.c"))
    stand_in = mock.Mock()
    with mock.patch.dict(sys.modules, {"oot_patches": stand_in}):
        chunks.edit_c(gen, L.stats, False, whole=False, wait=False)   # (the marks are left at it: their failure is marks_done's)
        check("the processes are at the marks after edit_c(wait=False)", chunks._marks is not None)
        try:
            chunks.marks_done()
            got = None
        except SystemExit as e:
            got = e
    failed("a process failing at the page marks", got, "the page marks of code_001_7.c")
    check("the patches ran before the marks", stand_in.apply.called)
    check("a process failing at the page marks: lifted files are left: %s" % lifted(folder), lifted(folder) == ["code_001_7.c"])
    os.rmdir(os.path.join(gen, "code_001_7.c"))
    # (the patches' own failure is said as it was, and the processes go)
    with contextlib.redirect_stdout(io.StringIO()):
        chunks.write(L, gen, image, counters=[poll.stats], jobs=3, setup=[poll.install])
    stand_in.apply.side_effect = SystemExit("oot_patches: authored")
    with mock.patch.dict(sys.modules, {"oot_patches": stand_in}):
        try:
            chunks.edit_c(gen, L.stats, False, whole=False)
            got = None
        except SystemExit as e:
            got = e
    failed("the patches failing", got, "oot_patches: authored")
    check("the patches failing: the C stays", len(lifted(folder)) > 8)
    # (and after all that a lift is still what one process writes)
    same("a lift after the failures", one, lift(os.path.join(tmp, "after"), 3))


def alive(pid):
    if os.name == "nt":
        import ctypes
        k = ctypes.WinDLL("kernel32")
        k.OpenProcess.restype = ctypes.c_void_p
        h = k.OpenProcess(0x00100000, False, pid)   # SYNCHRONIZE
        if not h:
            return False
        try:
            return k.WaitForSingleObject(ctypes.c_void_p(h), 0) == 0x102   # WAIT_TIMEOUT: still running
        finally:
            k.CloseHandle(ctypes.c_void_p(h))
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def orphan(path):
    """(the test's own child) Starts two workers, writes their process ids and waits to be stopped from outside"""
    w = emit_pool.Workers(2, Plain())
    pids = set()
    for _ in range(200):   # (until both have answered: the second takes a moment to start)
        pids |= {pid for _, pid in w.each(late, (("a pid", (i, 0.2)) for i in range(4)))}
        if len(pids) == 2:
            break
    pids = sorted(pids)
    with open(path + ".part", "w") as f:
        f.write(" ".join(map(str, pids)))
    os.replace(path + ".part", path)
    time.sleep(600)


def test_orphans(tmp):
    path = os.path.join(tmp, "pids.txt")
    p = subprocess.Popen([sys.executable, "-I", "-B", os.path.abspath(__file__), "--orphan", path],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        t = time.time()
        while not os.path.exists(path) and time.time() - t < 120 and p.poll() is None:
            time.sleep(0.1)
        check("the child lift started its workers", os.path.exists(path))
        pids = [int(x) for x in open(path).read().split()]
        check("two workers run: %s" % pids, len(pids) == 2 and all(alive(x) for x in pids))
    finally:
        p.kill()
        p.wait()
    t = time.time()
    while any(alive(x) for x in pids) and time.time() - t < 30:
        time.sleep(0.1)
    left = [x for x in pids if alive(x)]
    for x in left:   # (not left on the PC by a test that fails)
        with contextlib.suppress(OSError):
            os.kill(x, signal.SIGTERM)
    check("the workers of a lift stopped from outside end by themselves", not left)


def abandon(folder):
    """(the test's own child) A lift whose process ends on an error of its own while the page marks are being made, as
    nx2/lift.py would if writing its last files failed"""
    authored()
    L = a64_lift.Lifter(Image())
    L.after[sorted(L.entries)[3] + 4] = "c->x[9] = 1;   /* an authored patch after an instruction */"
    L.discover()
    gen, image = os.path.join(folder, "gen"), os.path.join(folder, "image")
    os.makedirs(image)
    chunks.write(L, gen, image, counters=[poll.stats], jobs=3, setup=[poll.install])
    chunks.edit_c(gen, L.stats, False, whole=False, wait=False)
    raise SystemExit("authored: the lift's last files could not be written")


def test_abandoned_marks(tmp, one):
    """The lift ends on another error while its processes share the page marks: they finish every file (none is left half
    marked or cut short), and the lift's own error is what ends it"""
    folder = os.path.join(tmp, "abandoned")
    p = subprocess.run([sys.executable, "-I", "-B", os.path.abspath(__file__), "--abandon", folder], capture_output=True, text=True,
                       timeout=300)
    check("the lift ended on its own error: status %d, %s" % (p.returncode, p.stderr[-300:]),
          p.returncode == 1 and "authored: the lift's last files could not be written" in p.stderr)
    for name, data in one[0].items():
        if name.startswith("gen/code_"):
            path = os.path.join(folder, name)
            check("%s is there after a lift that ended at the marks" % name, os.path.exists(path))
            with open(path, "rb") as f:
                check("%s is whole and marked after a lift that ended at the marks" % name, f.read() == data)


def test_jobs():
    def jobs(cpus, gb, env=None, fork=False):
        with mock.patch.object(chunks, "FORK", fork), mock.patch.object(chunks, "_available_gb", lambda: gb), \
                mock.patch.object(chunks.os, "cpu_count", lambda: cpus), \
                mock.patch.object(chunks.os, "sched_getaffinity", lambda _: range(cpus), create=True), \
                mock.patch.dict(os.environ):
            os.environ.pop("HOOKSHOT_LIFT_JOBS", None)
            if env is not None:
                os.environ["HOOKSHOT_LIFT_JOBS"] = env
            return chunks.emit_jobs()
    for cpus, gb, want in ((16, 32, 8), (32, 64, 8), (12, 32, 6), (8, 16, 4), (4, 16, 2), (2, 16, 1), (1, 16, 1), (16, 3.5, 3),
                           (16, 0.4, 1), (16, None, 8)):
        check("%d processors, %s GB: %d processes, not %d" % (cpus, gb, jobs(cpus, gb), want), jobs(cpus, gb) == want)
    check("HOOKSHOT_LIFT_JOBS is followed", jobs(16, 32, "5") == 5 and jobs(2, 1, "12") == 12 and jobs(16, 32, "1") == 1)
    check("with fork the count is as it was", (jobs(16, 32, fork=True), jobs(64, 64, fork=True), jobs(16, 7, fork=True),
                                               jobs(16, 32, "3", fork=True)) == (16, 16, 3, 3))
    # (one process is the kit's own loop over the lifter's emit_function, in this process: no pool at all)
    with mock.patch.object(emit_pool, "Workers", side_effect=AssertionError("a pool for one process")):
        with tempfile.TemporaryDirectory(prefix="hookshot-spawn-emit-") as tmp:
            check("one process makes no pool", isinstance(lift(tmp, 1), tuple))


def test_key():
    """The count of processes changes nothing of the C, so nothing of the key: lift_key hashes the environment's A64_, OOT_ and
    RX_ names, and the pool reads none of its own under them"""
    source = ROOT / "nx2" / "make_game.py"
    tree = ast.parse(source.read_text(encoding="utf-8"), filename=str(source))
    body = [n for n in tree.body
            if (isinstance(n, ast.Assign) and any(isinstance(x, ast.Name) and x.id == "LIFT_KEY_NOT" for x in n.targets))
            or (isinstance(n, ast.FunctionDef) and n.name in ("lift_key_file", "lift_key"))]
    check("lift_key is where it was in make_game.py", len(body) == 3)
    with tempfile.TemporaryDirectory(prefix="hookshot-spawn-key-") as tmp:
        root = Path(tmp)
        (root / "nx2").mkdir()
        (root / "third_party" / "a64-kit").mkdir(parents=True)
        (root / "exefs").mkdir()
        (root / "work").mkdir()
        (root / "exefs" / "main").write_bytes(b"authored")
        (root / "nx2" / "lift.py").write_text("# authored\n", encoding="utf-8")
        scope = dict(os=os, sys=sys, hashlib=hashlib, HERE=str(root / "nx2"), ROOT=str(root))
        exec(compile(ast.Module(body=body, type_ignores=[]), str(source), "exec"), scope)
        key = lambda: scope["lift_key"](str(root / "exefs"), str(root / "work"), {"OOT_WORK": "w"})
        with mock.patch.dict(os.environ):
            os.environ.pop("HOOKSHOT_LIFT_JOBS", None)
            first = key()
            for n in ("1", "3", "8"):
                os.environ["HOOKSHOT_LIFT_JOBS"] = n
                check("HOOKSHOT_LIFT_JOBS=%s changes the lift key" % n, key() == first)
            os.environ["A64_AUTHORED"] = "1"
            check("an A64_ name is in the key (the test would see one)", key() != first)
    for name in ("emit_pool.py", "chunks.py"):
        text = (ROOT / "nx2" / name).read_text(encoding="utf-8")
        strings = {n.value for n in ast.walk(ast.parse(text)) if isinstance(n, ast.Constant) and isinstance(n.value, str)}
        hashed = sorted(s for s in strings if s.startswith(("A64_", "OOT_", "RX_")) and s.replace("_", "").isalnum())
        check("%s reads an environment name the lift key hashes: %s" % (name, hashed), not hashed)


def authored():
    """What a lift of the authored code needs, in the lift's process only: the workers are sent what it made, and translate
    nothing"""
    a64emit.translate = translate
    a64_lift.Lifter.jump_table = lambda self, br_pc, reg, window=32: None   # (no tables in the authored code: its br is an indirect jump)
    a64_lift.CHUNK_BYTES = 60000
    chunks.FORK = False            # (the spawned processes on every system: the forked ones are tests/test_parallel_emit.py's)
    chunks.SPAWN_LEAST = 0
    emit_pool.RUN_FUNCS, emit_pool.RUN_INSNS = 7, 150
    poll.install()


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--orphan":
        return orphan(sys.argv[2])
    if len(sys.argv) == 3 and sys.argv[1] == "--abandon":
        return abandon(sys.argv[2])
    test_plan()
    started = time.time()
    authored()
    with tempfile.TemporaryDirectory(prefix="hookshot-spawn-emit-") as tmp:
        hot = os.path.join(tmp, "hot_funcs.txt")
        order = sorted(Image().entries)
        with open(hot, "w") as f:
            f.write("".join("f_%08x 1\n" % e for e in order[60:90:3] + [order[41]]))
        os.environ.update(A64_HOT_FUNCS=hot, A64_HOT_CHUNK="1")
        for k in ("A64_LOCAL_REGS", "A64_KEEP_FLAGS", "A64_MARK_EACH", "A64_MARK_FRAME", "A64_MARK_STACK", "A64_DEVIRT", "A64_NO_LOCAL",
                  "HOOKSHOT_LIFT_JOBS"):
            os.environ.pop(k, None)
        test_texts_sent()
        test_order_of_answers()
        one = test_the_same_c(tmp)
        test_failures(tmp, one)
        test_abandoned_marks(tmp, one)
        test_orphans(tmp)
        test_jobs()
    test_key()
    print("ok: the authored lift by 3 and by 5 spawned processes is one process's, file by file; a worker that raises, dies or "
          "hangs ends the lift and leaves no lifted file (%.0f s)" % (time.time() - started))


if __name__ == "__main__":
    main()
