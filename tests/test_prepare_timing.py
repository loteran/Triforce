"""The shader preparation says how its workers' time went, and a start that only prepares runs them at normal priority.

A cold driver's part (gpu/vk/vk_draw.cpp warm_libraries) on AMD 26.10.1 made ~91,600 stage libraries in 5608 s on 16 workers,
16 a second, and nothing in the log said whether those workers compiled or waited on one another. warm_libraries and
precompile_run log now, beside what they did: libraries (pipelines) a second, the summed thread milliseconds of each kind of
build (the libraries' pre-raster, fragment and shading rate parts; the precompile's compute, mesh fallback and graphics
pipelines), and the workers' share of a processor over the time they ran beside the process's share of every processor.
Only clocks are read around the builds: each build is the call it was.

The workers of the preparation before the title (prepare_run, warm_libraries, state_list_prepare, predict_run) ran below
normal priority; in a start that only prepares (HOOKSHOT_PREPARE_ONLY: Setup's build and update) they run at normal, but only
in a process known to be below normal already (Windows: the below normal or idle class; elsewhere a nice value above 0), so
that they stay below every program of normal priority. The game's own starts keep below normal, the preparation behind the
game idle, and precompile_run's workers the priority they had. The timing line every two minutes is written only after
progress, so that a warm stuck in one build still goes silent for Setup's silence watch.

Source checks, no game or GPU: python -I -B tests/test_prepare_timing.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit("FAIL: " + where + " no longer has: " + needle)


def main():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")

    # ---- the priority: normal only where the start only prepares and the process is known to be below normal (Windows:
    # the below normal or idle class; elsewhere a nice value above 0, as the Linux builder sets none)
    prio = between(draw, "int prepare_priority() {", "\n}\n")
    need("prepare_priority()", prio, [
        'if (!getenv("HOOKSHOT_PREPARE_ONLY")) return (int)THREAD_PRIORITY_BELOW_NORMAL;',
        "GetPriorityClass(GetCurrentProcess())",
        "if (cls != BELOW_NORMAL_PRIORITY_CLASS && cls != IDLE_PRIORITY_CLASS) return (int)THREAD_PRIORITY_BELOW_NORMAL;",
        "getpriority(PRIO_PROCESS, 0)",
        "if (errno != 0 || nice_now <= 0) return (int)THREAD_PRIORITY_BELOW_NORMAL;",
        "return (int)THREAD_PRIORITY_NORMAL;"])
    normal = prio.index("return (int)THREAD_PRIORITY_NORMAL;")
    if not prio.index("HOOKSHOT_PREPARE_ONLY") < prio.index("#ifdef _WIN32") < prio.index("GetPriorityClass") < \
            prio.index("#else") < prio.index("getpriority(PRIO_PROCESS, 0)") < prio.index("#endif") < normal or \
            prio.count("THREAD_PRIORITY_NORMAL;") != 1:
        raise SystemExit("FAIL: prepare_priority() can give normal priority without knowing the process is below normal")
    if "<sys/resource.h>" not in draw[:draw.index("int prepare_priority() {")]:
        raise SystemExit("FAIL: vk_draw.cpp does not include <sys/resource.h> for getpriority off Windows")
    main_cpp = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    need("vk_main.cpp", main_cpp, ['bool prepare_only() { return getenv("HOOKSHOT_PREPARE_ONLY") != nullptr; }'])
    for name, marker, count in [
            ("state_list_prepare", "void state_list_prepare(void (*progress)(size_t done, size_t total)) {", 2),
            ("warm_libraries", "bool warm_libraries(void (*progress)(size_t done, size_t total)) {", 1),
            ("predict_run", "void predict_run(void (*progress)(size_t done, size_t total)) {", 1),
            # (the pipeline cache carry's rebuild of the compute pipelines, prepare_run's foreground: pcache_carry.h)
            ("warm_kept_computes", "bool warm_kept_computes(void (*progress)(size_t done, size_t total)) {", 1)]:
        body = between(draw, marker, "\n}\n")
        if body.count("SetThreadPriority(GetCurrentThread(), prepare_priority());") != count or \
                "THREAD_PRIORITY_BELOW_NORMAL" in body or "THREAD_PRIORITY_NORMAL" in body:
            raise SystemExit("FAIL: %s(): its workers do not take prepare_priority()" % name)
    prepare = between(draw, "bool prepare_run(void (*progress)(size_t done, size_t total), bool background) {", "\n}\n")
    need("prepare_run()", prepare,
         ["SetThreadPriority(GetCurrentThread(), background ? THREAD_PRIORITY_IDLE : prepare_priority());"])
    pre = between(draw, "void precompile_run(void (*progress)(size_t done, size_t total)) {", "\n}\n")
    if "SetThreadPriority" in pre or "prepare_priority" in pre:
        raise SystemExit("FAIL: precompile_run()'s workers had no priority of their own and were to stay so")
    if draw.count("prepare_priority()") != 7:   # (the definition and the six workers above, nowhere in play)
        raise SystemExit("FAIL: prepare_priority() is used past the preparation's workers (%d)" % draw.count("prepare_priority()"))

    # ---- the timing: clocks around the same builds
    times = between(draw, "struct PrepTimes {", "\n};\n")
    need("PrepTimes", times, ["thread_cpu_us()", "process_cpu_us()", "void tick() {", "~Worker() { tick(); }",
                              "std::string kind_text(int kind, const char* name) const {",
                              "std::string cpu_text(unsigned workers) const {"])
    need("thread_cpu_us()", between(draw, "uint64_t thread_cpu_us() {", "\n}\n"),
         ["GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)", "CLOCK_THREAD_CPUTIME_ID"])
    need("process_cpu_us()", between(draw, "uint64_t process_cpu_us() {", "\n}\n"),
         ["GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)", "CLOCK_PROCESS_CPUTIME_ID"])

    warm = between(draw, "bool warm_libraries(void (*progress)(size_t done, size_t total)) {", "\n}\n")
    need("warm_libraries()", warm, [
        "PrepTimes times;", "PrepTimes::Worker timed(times);", "timed.tick();",
        "const VkPipeline lib = build_graphics(p, *blank, at, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, part);",
        "times.add(rate ? 2 : part - 1, built_from);",
        'times.kind_text(0, "pre-raster")', 'times.kind_text(1, "fragment")', 'times.kind_text(2, "shading rate")',
        "times.cpu_text(workers)", 'timing(" so far");', 'timing("");'])
    a, b, c = (warm.index(t) for t in ("const auto built_from = std::chrono::steady_clock::now();",
                                       "const VkPipeline lib = build_graphics(", "times.add(rate ? 2 : part - 1, built_from);"))
    if not a < b < c or "vkDestroyPipeline(dev.device, lib, nullptr);" not in warm[c:] or "libs++;" not in warm[c:]:
        raise SystemExit("FAIL: warm_libraries() times something else than each library's build, or no longer lets it go")
    # (the line every two minutes only after progress: Setup's silence watch, make_game.py PREPARE_SILENCE, reads the same
    # pipe, and a warm stuck in one build must still go silent for it)
    so_far = warm.index('timing(" so far");')
    guard = warm.rfind("if (now_done != timing_done) {", 0, so_far)
    if guard < 0 or "timing_done = now_done;" not in warm[so_far:so_far + 120] or \
            warm.rfind("const size_t now_done = done.load();", 0, guard) < 0 or warm.count('timing(" so far");') != 1:
        raise SystemExit("FAIL: warm_libraries() logs its timing every two minutes without progress: a stuck warm never goes "
                         "silent for Setup's silence watch")
    join = warm.index("for (std::thread& t : threads) t.join();")
    if not join < warm.index('timing("");') < warm.index("if (prepare_stopped()) {", join):
        raise SystemExit("FAIL: warm_libraries()'s last timing line is not after its workers, for a stopped part too")

    need("precompile_run()", pre, [
        "PrepTimes::Worker timed(pre->times);", "timed.tick();", "pre->times.add(0, built_from);",
        "pre->times.add(1, built_from);", "pre->times.add(2, built_from);",
        "const VkPipeline made = build_graphics(*p, n.s, note_targets(n), (VkPrimitiveTopology)n.topo);",
        "if (cached_compute(pcache_use().cache, &ci, &made) != VK_SUCCESS) made = VK_NULL_HANDLE;",
        "fallback_compute_pipelines(p);", 'logf("vk: precompile: timing:',
        "tm.cpu_text((unsigned)threads.size())"])
    line = pre.index('logf("vk: precompile: timing:')
    if not pre.index("for (std::thread& t : threads) t.join();") < line < pre.index("delete pre;") or \
            pre.index("for (std::thread& t : threads) t.detach();") < line:
        raise SystemExit("FAIL: precompile_run() reads its timing after the workers' state is freed or let go")
    print("ok: the preparation's timing is logged around the same builds, and only a start that only prepares runs at normal")


if __name__ == "__main__":
    main()
