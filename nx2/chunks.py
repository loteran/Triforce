"""Stable chunk boundaries for the lifted C (lift.py --write).

a64-kit's write() closes a chunk once it reaches CHUNK_BYTES of text, so a function that grows (a jump table found,
a fix in one form) moves every boundary after it, and build.py recompiles every chunk from there on: a fix to two
functions cost a full 45-minute rebuild. Here the boundaries are guest addresses, kept in work/image/chunks.txt:
a chunk ends after the last function below the next boundary, so only the chunks holding changed functions differ.
Without the file (or with --rechunk) the boundaries come from the sizes, as before, and are saved.

write()'s test is `size >= chunk_bytes`. chunk_bytes is a Boundaries instance: an int whose reflected comparison
(__le__, which Python tries first for a subclass on the right) decides from the function just emitted.

The functions' C is made by several processes (fork, so Linux and macOS; one process elsewhere): each emits a run of
consecutive functions from the lifter as it stands, and write() takes the texts in its own order, as if it had made
them. The lifter's counters (stats, unhandled, ijump, and any passed in `counters`, such as poll.stats) get each
process's additions. The output is the one-process output, byte for byte (tests/test_parallel_emit.py).

(Triforce) Where there is no fork (Windows) the processes are spawned and sent what they need (nx2/emit_pool.py), and the same
processes then cut the big chunks (split_big) and share the page marks (edit_c), a file each: 46 s, 6 s and 19 s of the lift's
127 s in one process on a 16-thread PC, 13 s, 1.4 s and 5 s of its 69 s with 8 processes. HOOKSHOT_LIFT_JOBS=1 is the one
process, as before, on every system.
"""
import bisect
import collections
import glob
import multiprocessing
import os
import re
import threading
import whole_write   # (rewrites written beside and moved over: issue 1197)

import a64_lift
import emit_pool   # ((Triforce) the emit's processes where there is no fork)
import lifted_c

FILE = "chunks.txt"


class Boundaries(int):
    def __new__(cls, size, starts, funcs):
        self = super().__new__(cls, size)
        self.starts = sorted(starts)
        self.recorded = []   # (without saved boundaries) where the size rule started a chunk
        self.current = None
        self.after = None    # the function emitted after `current`
        order = sorted(funcs)
        self.ends = set()
        for b in self.starts:
            i = bisect.bisect_left(order, b)
            if 0 < i < len(order):
                self.ends.add(order[i - 1])
        self.next_of = {a: b for a, b in zip(order, order[1:])}
        return self

    def __le__(self, size):   # `size >= self`
        e = self.current
        if self.starts:
            return e in self.ends
        if size >= int(self):
            nxt = self.next_of.get(e)
            if nxt is not None:
                self.recorded.append(nxt)
            return True
        return False


def load(img_dir):
    path = os.path.join(img_dir, FILE)
    if not os.path.exists(path):
        return []
    return [int(l, 16) for l in open(path) if l.strip() and not l.startswith("#")]


def save(img_dir, starts):
    with open(os.path.join(img_dir, FILE), "w") as f:
        f.write("# first guest address of each lifted C chunk after the first (nx2/chunks.py)\n")
        f.write("".join("%08x\n" % s for s in sorted(starts)))


def seed_from_gen(gen_dir):
    """The boundaries of an existing size-chunked lift: the first function defined in each code_NNN.c after the
    first"""
    starts = []
    pat = re.compile(r"^void f_([0-9a-f]+)\(Ctx \*__restrict A64_NOESC c\) \{")
    n = 1
    while os.path.exists(os.path.join(gen_dir, "code_%03d.c" % n)):
        for line in open(os.path.join(gen_dir, "code_%03d.c" % n)):
            m = pat.match(line)
            if m:
                starts.append(int(m.group(1), 16))
                break
        n += 1
    return starts


_job = None   # (the forked workers' view: the emitter, the instruction texts and the counters, inherited)


def _counts(counters):
    return [collections.Counter(c) for c in counters]


def _emit_run(es):
    emit, texts, counters = _job
    before = _counts(counters)
    out = [(e,) + tuple(emit(e, texts)) for e in es]
    return out, [collections.Counter({k: v - b.get(k, 0) for k, v in c.items() if v != b.get(k, 0)})
                 for b, c in zip(before, counters)]


def _available_gb():
    """MemAvailable from /proc/meminfo in GB; (Triforce) on Windows the physical memory GlobalMemoryStatusEx calls available
    (None where there is neither)"""
    try:
        with open("/proc/meminfo", encoding="ascii") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / (1024 * 1024)
    except (OSError, ValueError, IndexError):
        pass
    if os.name == "nt":
        try:
            import ctypes

            class Status(ctypes.Structure):
                _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong)] + [
                    (name, ctypes.c_ulonglong) for name in ("ullTotalPhys", "ullAvailPhys", "ullTotalPageFile", "ullAvailPageFile",
                                                            "ullTotalVirtual", "ullAvailVirtual", "ullAvailExtendedVirtual")]
            s = Status()
            s.dwLength = ctypes.sizeof(s)
            if ctypes.WinDLL("kernel32").GlobalMemoryStatusEx(ctypes.byref(s)):
                return s.ullAvailPhys / (1 << 30)
        except (OSError, AttributeError, ValueError):
            pass
    return None


FORK = "fork" in multiprocessing.get_all_start_methods()
SPAWN_MOST = 8     # ((Triforce) spawned emit processes at most: the lift's own process cannot keep more busy, emit_pool.py)
SPAWN_GB = 1       # (and one per GB of available memory: one holds its run or a file of lifted C, 480 MB at the most seen)
SPAWN_LEAST = 2 * emit_pool.RUN_FUNCS   # (functions below which the lift stays one process: starting the others costs more)


def emit_jobs():
    """Processes for the emit: HOOKSHOT_LIFT_JOBS, else the cores this process may use (at most 16; each holds its run's
    texts and the pages of the lift it touches), and at most one per 2 GB of available memory: a Steam Deck (16 GB) ran
    out with 8, and the system's out-of-memory handling stopped the lift ("lift.py stopped (signal 15): BrokenPipeError")
    or one worker (the update then sat at "jump tables" for good).
    (Triforce) Where there is no fork (Windows; before, always 1 there): half the processors, at most SPAWN_MOST, and at most
    one per SPAWN_GB of available memory. Half, because the files that need no translated code are compiled beside the lift
    there (make_game's EarlyCompile, one compiler a processor less one) and because the lift's own process, which sends the
    runs and writes the C, is what more of them would wait for (emit_pool.py has the measure). 1 is the one process on
    every system"""
    if os.environ.get("HOOKSHOT_LIFT_JOBS"):
        return max(1, int(os.environ["HOOKSHOT_LIFT_JOBS"]))
    try:
        n = len(os.sched_getaffinity(0))
    except AttributeError:
        n = os.cpu_count() or 1
    if not FORK:
        n = max(1, min(SPAWN_MOST, n // 2))
        gb = _available_gb()
        if gb is not None:
            n = max(1, min(n, int(gb // SPAWN_GB)))
        return n
    n = max(1, min(16, n))
    gb = _available_gb()
    if gb is not None:
        n = max(1, min(n, int(gb // 2)))
    return n


def parallel_emit(emit, order, texts, counters, jobs, run=2000):
    """(e, text, refs) for every function of order, in order, emitted by `jobs` forked processes. A worker that dies
    (stopped for memory) ends the pool with an error instead of a wait forever (multiprocessing.Pool's imap never
    returns then); the runs not done yet are emitted here, one process."""
    global _job
    if jobs <= 1 or len(order) < 2 * run:
        for e in order:
            yield (e,) + tuple(emit(e, texts))
        return
    import gc
    from concurrent.futures import ProcessPoolExecutor
    from concurrent.futures.process import BrokenProcessPool
    runs = [order[i:i + run] for i in range(0, len(order), run)]
    done = 0
    _job = (emit, texts, counters)
    gc.collect()
    gc.freeze()   # (the lift's objects out of the collector's sight: its passes would write to every page the children share)
    try:
        ctx = multiprocessing.get_context("fork")
        with ProcessPoolExecutor(max_workers=jobs, mp_context=ctx) as pool:
            for out, deltas in pool.map(_emit_run, runs):
                for c, d in zip(counters, deltas):
                    for k, v in d.items():
                        c[k] = c.get(k, 0) + v
                done += 1
                yield from out
    except BrokenProcessPool:
        print("emit: a worker process stopped (out of memory?): the last %d of %d runs are emitted in this process"
              % (len(runs) - done, len(runs)), flush=True)
    finally:
        _job = None
        gc.unfreeze()
    for es in runs[done:]:
        for e in es:
            yield (e,) + tuple(emit(e, texts))


_workers = None   # ((Triforce) the spawned processes of this lift, from write() until edit_c()'s marks end, emit_pool.Workers)
_marks = None     # (the page marks in flight after edit_c(wait=False): the thread waiting for them, what ended it, the folder)


def stop_workers(kill=False):
    """(Triforce) The spawned processes ended, when there are any: after what they are doing, or (kill) now"""
    global _workers
    w, _workers = _workers, None
    if w is not None:
        w.stop(kill)


def drop_lifted(gen_dir):
    """(Triforce) The lifted C taken away after a lift that stopped while its processes were writing it: the chunks, their
    parts, the hot file and its pieces, whole or cut short. What is left of such a lift looks like a lift (every file that
    is there may end as a function does, and an earlier lift's a64_table.c still names every function), and the next lift
    would take its chunk boundaries from it (seed_from_gen) and a build its files. The same files make_game takes away
    before a lift"""
    for f in os.listdir(gen_dir) if os.path.isdir(gen_dir) else ():
        if f.startswith("code_") and f.endswith((".c", ".c.whole", ".part")):
            try:
                os.remove(os.path.join(gen_dir, f))
            except OSError:
                pass   # (held by something else: the lift's own failure is what is said)


def gave_up(gen_dir):
    """(Triforce) A lift with spawned processes that failed: they are ended now and its C goes"""
    stop_workers(kill=True)
    drop_lifted(gen_dir)


def write(L, gen_dir, img_dir, rechunk=False, counters=(), jobs=None, setup=()):
    """L.write(gen_dir) with stable boundaries; returns its files. counters: dicts the emit adds to besides the
    lifter's own (poll.stats); jobs: emit processes (emit_jobs()); (Triforce) setup: what set the lifter's class up for
    the emit in this process (poll.install), module-level functions the spawned processes call before they emit"""
    global _workers
    starts = [] if rechunk else load(img_dir)
    if not starts and not rechunk:
        starts = seed_from_gen(gen_dir)
    b = Boundaries(a64_lift.CHUNK_BYTES, starts, L.funcs)
    base_emit = L.emit_function
    jobs = emit_jobs() if jobs is None else jobs
    spawned = jobs > 1 and not FORK and len(L.funcs) >= SPAWN_LEAST
    made = None

    def emit_function(e, texts):
        nonlocal made
        global _workers
        b.current = e
        if made is None:   # (the first call: write() has its texts now, and asks for the functions in sorted order)
            every = [L.stats, L.unhandled, L.ijump] + list(counters)
            if spawned:
                stop_workers(kill=True)   # (an earlier write()'s, which no edit_c() ended)
                _workers = emit_pool.Workers(jobs, L, every, setup)
                print("emit: %d processes (HOOKSHOT_LIFT_JOBS=<n> for another count, 1 for one process)" % _workers.jobs, flush=True)
                made = emit_pool.emit(_workers, L, sorted(L.funcs), texts, every)
            else:
                made = parallel_emit(base_emit, sorted(L.funcs), texts, every, jobs)
        got = next(made, None)
        if got is None or got[0] != e:
            raise SystemExit("chunks: write() asked for f_%08x, the emit made %s" % (e, "f_%08x" % got[0] if got else "nothing"))
        return got[1], got[2]
    L.emit_function = emit_function
    try:
        files = L.write(gen_dir, chunk_bytes=b)
    except BaseException:
        if spawned:   # ((Triforce) a process failed, died or hung, or this one was stopped: no part of a lift is left behind)
            gave_up(gen_dir)
        raise
    finally:
        del L.emit_function
        if made is not None:
            made.close()   # (the processes go now, not when the generator is collected)
    # (the numbered chunks only: files has code_hot.c too, and counting it skipped the first stale chunk, linked twice)
    n = sum(1 for f in files if re.fullmatch(r"code_\d{3}\.c", os.path.basename(f)))
    while os.path.exists(os.path.join(gen_dir, "code_%03d.c" % n)):   # (fewer chunks than the last lift)
        os.remove(os.path.join(gen_dir, "code_%03d.c" % n))
        n += 1
    # (an earlier lift's hot pieces go now, with or without a new code_hot.c, before the patches and the page-mark groups read
    # code_*.c: split_hot.py makes this lift's after it. A lift with no code_hot.c (no hot list, A64_HOT_CHUNK unset, or none of the
    # listed functions in it): split_hot.py had renamed the old one code_hot.c.whole and left code_hot_*.c, which build.py compiled
    # again, the earlier lift's hot functions, each defined in a chunk too, linked twice. A lift with one: the old pieces, patched
    # already, and the new whole file each held the output-size places, and nx2/oot_patches.py's count of them failed the lift)
    for f in os.listdir(gen_dir):
        if f == "code_hot.c.whole" or HOT_PIECE.fullmatch(f):
            os.remove(os.path.join(gen_dir, f))
    try:
        files += split_big(gen_dir, files, workers=_workers if spawned else None)
    except BaseException:
        if spawned:
            gave_up(gen_dir)
        raise
    save(img_dir, starts or b.recorded)
    print("chunks: %d, boundaries %s" % (len(files), "kept" if starts else "from sizes (saved)"))
    return files


HEAD = lifted_c.HEAD_NOTED
PART = re.compile(r"code_(\d{3})_(\d+)\.c")
HOT_PIECE = re.compile(r"code_hot_\d+\.c")   # (nx2/split_hot.py's pieces of code_hot.c)
# (a function's compile time grows with the square of its size: LLVM's passes over one function of tens of thousands of blocks. Fitted to
# a cold build of the 48 chunks at -O1 on a 24-thread desktop: seconds ~ 1.5 x MB + 4.4 x MB^2 per function, so a function of QUAD bytes
# costs twice its size. A 24 MB chunk of small functions took ~45 s; one of 20 MB holding a few 2 MB functions, 350 s)
QUAD = 350000
COSTS = "chunk_costs.txt"   # (in the gen folder: each file's estimated compile cost, for build.py's order)


def fn_cost(size):
    """A function's estimated compile cost, in bytes of plain code"""
    return size + size * size // QUAD


def text_cost(text):
    """The estimated compile cost of a C file of lifted functions"""
    marks = [m.start() for m in HEAD.finditer(text)] + [len(text)]
    return marks[0] + sum(fn_cost(b - a) for a, b in zip(marks, marks[1:]))


def write_costs(gen_dir, costs):
    """Adds or replaces files' costs in chunk_costs.txt; costs: {file name: cost}"""
    path = os.path.join(gen_dir, COSTS)
    known = {}
    if os.path.exists(path):
        for line in open(path):
            name, _, n = line.partition(" ")
            if n.strip().isdigit() and os.path.exists(os.path.join(gen_dir, name)):
                known[name] = int(n)
    known.update(costs)
    with open(path, "w", newline="\n") as f:
        f.write("".join("%s %d\n" % kv for kv in sorted(known.items())))


def split_one(gen_dir, path, budget):
    """split_big for one chunk (a spawned process's share of it too): the new parts' paths, {file name: cost} of the chunk or of
    each of its parts, and what to say of a chunk that was cut (None of one left whole)"""
    m = re.fullmatch(r"code_(\d{3})\.c", os.path.basename(path))
    made, costs = [], {}
    with open(path, encoding="utf-8", newline="") as f:
        text = f.read()
    marks = [h.start() for h in HEAD.finditer(text)]
    ends = marks[1:] + [len(text)]
    cost = [fn_cost(b - a) for a, b in zip(marks, ends)]
    total = sum(cost)
    if total <= budget * 3 // 2 or len(marks) < 2:
        costs[os.path.basename(path)] = marks[0] + total if marks else len(text)
        return made, costs, None
    parts = -(-total // budget)
    cuts, run, k = [0], 0, 1   # (indexes into marks where a part starts)
    for i, c in enumerate(cost):
        if i and run >= total * k // parts:
            cuts.append(i)
            k = run * parts // total + 1   # (past a giant function: the next share, not a sliver after it)
        run += c
    bounds = [marks[i] for i in cuts] + [len(text)]
    preamble = text[:marks[0]]
    for j in range(len(cuts)):
        out = path if j == 0 else os.path.join(gen_dir, "code_%s_%d.c" % (m.group(1), j))
        piece = text[:bounds[1]] if j == 0 else preamble + text[bounds[j]:bounds[j + 1]]
        whole_write.write_whole(out, piece)
        costs[os.path.basename(out)] = len(preamble) + sum(cost[cuts[j]:cuts[j + 1] if j + 1 < len(cuts) else len(cost)])
        if j:
            made.append(out)
    return made, costs, "chunks: %s (%d MB, cost %d MB) in %d parts" % (os.path.basename(path), len(text) >> 20, total >> 20, len(cuts))


def split_big(gen_dir, files, budget=None, workers=None):
    """Chunks whose estimated compile time (fn_cost) is well past a plain chunk's, cut at function starts into parts of about equal
    cost: code_NNN.c, code_NNN_1.c ... A chunk of the giant functions took 350 s, alone at the end of the build, where a chunk of
    ordinary functions takes 45; the boundaries kept in chunks.txt also let chunks grow (44 MB beside 25). The first part keeps the
    name, so seed_from_gen and the boundaries are as before. Writes chunk_costs.txt for every chunk; returns the new parts' paths.
    (Triforce) workers: the spawned processes that take a chunk each (emit_pool.Workers; None: this process takes them all)"""
    budget = budget or a64_lift.CHUNK_BYTES
    made, costs = [], {}
    paths = [path for path in files if re.fullmatch(r"code_(\d{3})\.c", os.path.basename(path))]
    if workers is None:
        answers = (split_one(gen_dir, path, budget) for path in paths)
    else:
        answers = workers.each(split_one, (("the split of %s" % os.path.basename(path), (gen_dir, path, budget)) for path in paths))
    for new, cost, said in answers:   # (in the chunks' order, whoever cut them)
        made += new
        costs.update(cost)
        if said:
            print(said)
    for f in os.listdir(gen_dir):   # (parts of an earlier lift that this one did not make)
        if PART.fullmatch(f) and os.path.join(gen_dir, f) not in made:
            os.remove(os.path.join(gen_dir, f))
    write_costs(gen_dir, costs)
    return made


def marks_one(path):
    """(Triforce) a64_lift.group_marks for one file, in a spawned process: whether it changed the file, and its counts"""
    st = collections.Counter()
    return a64_lift.group_marks([path], st), list(st.items())


def ask_marks(paths, workers):
    """(Triforce) a64_lift.group_marks asked of the spawned processes, a file each, the largest first (the hot file, 82 MB,
    is the last by name and would end alone). Every file is asked for here, at once: a lift that ends on an error of its
    own while they are at it asks for nothing more, and the processes finish the files, all of them. What take_marks() takes"""
    by_size = sorted(paths, key=lambda p: (-os.path.getsize(p), p))
    return by_size, workers.ask(marks_one, [("the page marks of %s" % os.path.basename(p), (p,)) for p in by_size])


def take_marks(paths, stats, workers, asked):
    """(Triforce) Waits for what ask_marks() asked: the counts go into stats in the paths' order, as one process adds them
    when it groups the files itself. The files changed"""
    by_size, calls = asked
    answers = dict(zip(by_size, workers.answers(calls)))
    changed = 0
    for p in paths:
        n, counts = answers[p]
        changed += n
        for k, v in counts:
            stats[k] = stats.get(k, 0) + v
    return changed


def edit_c(gen, stats, supported, whole=True, wait=True):
    """(nx2/lift.py, after the lift writes its C; here, beside the writing, so it imports without the lift's tools) The edits of the written C, in this order: the game's patches (nx2/oot_patches.py, which find the lift's exact text; the
    supported build's whole standalone lift fails when an output-size edit finds no place), then, last, the page marks of each
    group of stores shared (a64_lift.group_marks), which changes that text. stats: the lift's, for its report.
    (Triforce) The marks are made by write()'s spawned processes when it left any, which end with them. wait=False: this
    returns while they are at it, and marks_done() waits for them: the lift writes its last files meanwhile (hos_image.c takes
    as long as the marks, and reads nothing of the C); stats is theirs until then"""
    global _marks
    import oot_patches
    try:
        oot_patches.apply(gen, protect_hearts=supported, required=supported and whole)
    except BaseException:
        stop_workers(kill=True)   # (the patches' own failure, said as before: the C stays as it is, for whoever looks into it)
        raise
    paths = sorted(glob.glob(os.path.join(glob.escape(gen), "code_*.c")))
    workers = _workers
    if workers is None:
        a64_lift.group_marks(paths, stats)
        return
    try:
        asked = ask_marks(paths, workers)
    except BaseException:
        gave_up(gen)
        raise
    ended = []

    def marks():   # (this thread only waits for the processes and adds their counts up)
        try:
            take_marks(paths, stats, workers, asked)
        except BaseException as e:
            ended.append(e)
    thread = threading.Thread(target=marks)
    thread.start()
    _marks = (thread, ended, gen)
    if wait:
        marks_done()


def marks_done():
    """(Triforce) After edit_c(wait=False): waits for the page marks the spawned processes are making, and ends the
    processes. Nothing to do when edit_c made the marks itself (one process, or wait)"""
    global _marks
    m, _marks = _marks, None
    if m is None:
        return
    thread, ended, gen = m
    thread.join()
    if ended:   # (a process failed in the middle of the files: some marked, some not, one perhaps cut short)
        gave_up(gen)
        raise ended[0]
    stop_workers()
