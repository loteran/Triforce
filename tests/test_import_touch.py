"""A range's pages read before the driver imports it (gpu/vk/import_touch.h, RX_VK_IMPORT_TOUCH; no game, no GPU).

The driver locks an import's pages inside vkAllocateMemory, faulting in the ones nobody touched, and a queue submit from
another thread waits for it: 16 to 93 ms beside the large ranges imported ahead. The pages are read first, outside the
driver. The rules of the header, compiled and run: on unless the switch is 0; only on the background import thread and
only where the range was committed whole; the runs cover a range once and whole; every page of a run is read, one byte
each, and nothing past its end. Each rule taken out in turn (a mutation) must make the test fail, and each mutant must
build. On Windows the read itself runs: guarded_touch, cut out of vk_memory.cpp, makes freshly committed pages resident
(the working set says so before and after), and returns false at a page that cannot be read instead of faulting, with the
header's read taken out or its volatile dropped failing that. Then the sources: import_pool reads the pages after the lazy
write-back let the range go and after the commit, before the allocate, through a handler that leaves a fault behind (a call
through a pointer inside its __try block, to a function that is not inlined), on Windows; the background thread says it is
one; the time is kept out of the report's commit part; the switch's row is in the development guide.

    python -I -B tests/test_import_touch.py [--cxx g++]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


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
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "import_touch.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::import_touch;
int main() {
    // (on unless the switch is 0)
    CHECK(on(nullptr) && on("") && on("1") && on("2") && on("00") && !on("0"));
    // (read first: the switch, the background import thread, the range committed whole; nothing else)
    CHECK(may(true, true, true));
    CHECK(!may(false, true, true) && !may(true, false, true) && !may(true, true, false) && !may(false, false, false));
    CHECK(kLargeMb >= 32 && kLargeMb <= 256);
    // (the runs of a range: each kRun but the last, none past the end, the range covered once)
    const uint64_t MB = 1ull << 20;
    CHECK(kRun % kPage == 0 && kRun >= MB && kRun <= 64 * MB);
    CHECK(run(816 * MB, 0) == kRun && run(816 * MB, 816 * MB - kRun) == kRun && run(816 * MB, 816 * MB) == 0);
    CHECK(run(kRun + 3 * kPage, kRun) == 3 * kPage && run(3 * kPage, 0) == 3 * kPage && run(0, 0) == 0 && run(kPage, 2 * kPage) == 0);
    for (const uint64_t bytes : {uint64_t(kPage), uint64_t(kRun), kRun + kPage, 5 * kRun - kPage, 16 * MB}) {
        uint64_t sum = 0, n = 0;
        for (uint64_t off = 0; off < bytes; off += kRun) sum += run(bytes, off), n++;
        CHECK(sum == bytes && n == (bytes + kRun - 1) / kRun);
    }
    // (every page of a run read, none past its end)
    std::vector<uint8_t> buf(64 * kPage + kPage, 1);
    CHECK(read_pages(buf.data(), 0) == 0 && read_pages(buf.data(), kPage) == 1 && read_pages(buf.data(), 64 * kPage) == 64);
    CHECK(read_pages(buf.data() + kPage, 3 * kPage) == 3);
    std::puts("PASS header: the switch, where the pages are read, a range's runs, a run's pages");
    return 0;
}
'''

# (Windows: guarded_touch as vk_memory.cpp has it, run. Fresh pages are not in the working set until something reads them;
# a reserved page cannot be read, and no handler of the host's is here to commit it)
RUNTIME_TEST = r'''
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "import_touch.h"
using namespace mkwvk;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
@GUARDED@
static int resident(uint8_t* p, int pages) {
    int n = 0;
    for (int i = 0; i < pages; i++) {
        PSAPI_WORKING_SET_EX_INFORMATION w{};
        w.VirtualAddress = p + (size_t)i * 4096;
        CHECK(K32QueryWorkingSetEx(GetCurrentProcess(), &w, sizeof w));
        n += (int)w.VirtualAttributes.Valid;
    }
    return n;
}
int main() {
    const int kPages = 256;
    uint8_t* fresh = (uint8_t*)VirtualAlloc(nullptr, (size_t)kPages * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(fresh);
    CHECK(resident(fresh, kPages) == 0);                 // (committed, never touched: not in RAM yet)
    uint64_t pages = 0;
    CHECK(guarded_touch(fresh, (size_t)kPages * 4096, &pages) && pages == (uint64_t)kPages);
    CHECK(resident(fresh, kPages) == kPages);            // (each page read: each in RAM now, before the driver locks them)
    // (two pages, the second reserved only: the read leaves through the handler, and the count is not taken)
    uint8_t* two = (uint8_t*)VirtualAlloc(nullptr, 2 * 4096, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(two && VirtualAlloc(two, 4096, MEM_COMMIT, PAGE_READWRITE) == two);
    pages = 0;
    CHECK(!guarded_touch(two, 2 * 4096, &pages) && pages == 0);
    CHECK(guarded_touch(two, 4096, &pages) && pages == 1);
    std::puts("PASS run (Windows): the read brings fresh pages into RAM, and leaves a page it cannot read behind");
    return 0;
}
'''

# (header mutations the Windows run must fail on: the count alone does not see them)
RUNTIME_MUTATIONS = [
    ("the pages are read", "for (size_t i = 0; i < n; i += kPage) sink += p[i], pages++;", "for (size_t i = 0; i < n; i += kPage) sink += (unsigned)(size_t)(p + i), pages++;"),
    ("the read is volatile", "inline uint64_t read_pages(const volatile uint8_t* p, size_t n) {", "inline uint64_t read_pages(const uint8_t* p, size_t n) {"),
]

MUTATIONS = [
    ("only with the switch", "return on && background_thread && committed_whole;", "return background_thread && committed_whole + 0 * on;"),
    ("only on the background import thread", "return on && background_thread && committed_whole;", "return on && committed_whole + 0 * background_thread;"),
    ("only where the range was committed whole", "return on && background_thread && committed_whole;", "return on && background_thread + 0 * committed_whole;"),
    ("off at 0", "return !(env && env[0] == '0' && !env[1]);", "return true || env;"),
    ("on when unset", "return !(env && env[0] == '0' && !env[1]);", "return env && env[0] == '1';"),
    ("0 alone is off", "return !(env && env[0] == '0' && !env[1]);", "return !(env && env[0] == '0');"),
    ("the last run ends with the range", "(size_t)(bytes - off < kRun ? bytes - off : kRun)", "kRun"),
    ("no run past the end", "return off >= bytes ? 0 : ", "return "),
    ("every page read", "for (size_t i = 0; i < n; i += kPage) sink += p[i], pages++;",
     "for (size_t i = 0; i < n; i += 2 * kPage) sink += p[i], pages++;"),
    ("the pages counted", "for (size_t i = 0; i < n; i += kPage) sink += p[i], pages++;", "for (size_t i = 0; i < n; i += kPage) sink += p[i];"),
]


TOUCH_SIG = "__declspec(noinline) bool guarded_touch(const uint8_t* p, size_t n, uint64_t* pages) {"
TOUCH_PTR = "uint64_t (*volatile touch_read)(const volatile uint8_t*, size_t) = import_touch::read_pages;"


def check_source(memory, header, dev):
    if '#include "import_touch.h"' not in memory:
        raise SystemExit("vk_memory.cpp does not include import_touch.h")
    at = memory.index(TOUCH_SIG)
    if memory.rfind("#ifdef _WIN32", 0, at) < memory.rfind("#endif", 0, at):
        raise SystemExit("vk_memory.cpp: guarded_touch is not inside #ifdef _WIN32 (its handler is Windows')")
    # (the read is a call the compiler cannot see through, to a function it may not inline: a fault leaves through the handler)
    in_order(memory, [TOUCH_PTR, TOUCH_SIG], "vk_memory.cpp guarded_touch's pointer")
    touch = block(memory, TOUCH_SIG)
    in_order(touch, ["__try {", "*pages += touch_read(p, n);",
                     "} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {",
                     "return false;", "return true;"], "vk_memory.cpp guarded_touch")
    in_order(header, ["__attribute__((noinline))", "inline uint64_t read_pages(const volatile uint8_t* p, size_t n) {",
                      "for (size_t i = 0; i < n; i += kPage) sink += p[i], pages++;"], "import_touch.h read_pages")
    pool = block(memory, "bool import_pool(uint64_t lo, uint64_t hi, Pool& p) {")
    in_order(pool, ["} lazy_io{memory_lazy_io_begin(ptr, bytes)};",
                    "bool committed_whole = false;",
                    "if (page_watches || !(committed_whole = VirtualAlloc(ptr, (SIZE_T)bytes, MEM_COMMIT, PAGE_READWRITE) == ptr))",
                    "#ifdef _WIN32",
                    'static const bool touch = import_touch::on(getenv("RX_VK_IMPORT_TOUCH"));',
                    "if (import_touch::may(touch, import_on_bg_thread, committed_whole)) {",
                    "for (uint64_t off = 0; off < bytes; off += import_touch::kRun) {",
                    "if (bg_waiters.load(std::memory_order_relaxed)) {", "break;",
                    "if (!guarded_touch(ptr + off, import_touch::run(bytes, off), &pages)) faulted++;",
                    "touch_ms = std::chrono::duration<double, std::milli>(clk::now() - a).count();",
                    "#endif",
                    "t1 = clk::now();",
                    "const VkResult allocated = vkAllocateMemory(dev.device, &ai, nullptr, &p.memory);"], "vk_memory.cpp import_pool")
    if pool.count("guarded_touch(") != 1 or memory.count("touch_read(") != 1 or memory.count("import_touch::read_pages") != 1:
        raise SystemExit("vk_memory.cpp: the pages are read in a place this test does not know")
    # (a lookup that waits for an import says so for the whole of its wait, and the read stops while it does)
    in_order(memory, ["std::atomic<int> bg_waiters{0};", "bg_waiters.fetch_add(1, std::memory_order_relaxed);",
                      "bg_done_cv.wait(lk, [&] { return !under_way(); });", "bg_waiters.fetch_sub(1, std::memory_order_relaxed);"],
             "vk_memory.cpp pool_lookup's wait")
    if memory.count("bg_waiters.fetch_add(") != 1 or memory.count("bg_waiters.fetch_sub(") != 1:
        raise SystemExit("vk_memory.cpp: bg_waiters is changed in a place this test does not know")
    if pool.count("committed_whole") != 3:
        raise SystemExit("vk_memory.cpp import_pool: committed_whole is set or read in a place this test does not know")
    # (the background thread says so, once, before its first import; nothing else does)
    if memory.count("import_on_bg_thread = true;") != 1 or memory.count("import_on_bg_thread") != 3:
        raise SystemExit("vk_memory.cpp: import_on_bg_thread is set or read in a place this test does not know")
    in_order(memory, ["thread_local bool import_on_bg_thread = false;"], "vk_memory.cpp")
    bg = block(memory, "void bg_main() {")
    in_order(bg, ["import_on_bg_thread = true;", "for (;;) {", "import_pool(r.first, r.second, p);"], "vk_memory.cpp bg_main")
    # (the report: the read's time named, and not counted as the commit's; the large imports on their own count of lines)
    in_order(pool, ["double touch_ms = 0;", "const double& touch;",
                    "if (total < 2.0 || (mb >= import_touch::kLargeMb ? logged_large++ >= 48 : logged++ >= 12)) return;",
                    "pages read first %.1f", "ms(a, b) - touch, touch,",
                    "} report{t0, t1, t2, t3, t4, touch_ms, bytes >> 20};"], "vk_memory.cpp import_pool's report")
    row = next((l for l in dev.splitlines() if l.startswith("| `RX_VK_IMPORT_TOUCH=0` |")), "")
    if "Windows" not in row or "background thread" not in row:
        raise SystemExit("docs/development.md: RX_VK_IMPORT_TOUCH's row is missing, or does not say where the read is made")


def build(cxx, d, name, src, include, extra=()):
    cpp, exe = d / (name + ".cpp"), d / (name + ".exe")
    cpp.write_text(src, encoding="utf-8")
    r = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", *extra, "-I" + str(include), str(cpp),
                        "-o", str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        return None, r.stdout
    return exe, ""


def run(exe):
    return subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    a = ap.parse_args()
    memory = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
    header = (VK / "import_touch.h").read_text(encoding="utf-8")
    check_source(memory, header, (ROOT / "docs" / "development.md").read_text(encoding="utf-8"))
    print("PASS sources: import_pool reads the pages on the background thread, where the range was committed whole, after the "
          "commit and before the allocate, through a handler that leaves a fault behind; the report names the read's time; the "
          "switch's row")
    with tempfile.TemporaryDirectory(prefix="import-touch-") as tmp:
        d = Path(tmp)
        exe, out = build(a.cxx, d, "header", HEADER_TEST, VK)
        if not exe:
            raise SystemExit("header: the build failed:\n" + out)
        r = run(exe)
        if r.returncode:
            raise SystemExit("the header test failed:\n" + r.stdout)
        print(r.stdout.strip())
        for i, (what, old, new) in enumerate(MUTATIONS):
            if header.count(old) != 1:
                raise SystemExit("mutation %r: its text is not once in import_touch.h (update the test with the header)" % what)
            inc = d / ("mutant%d" % i)
            inc.mkdir()
            (inc / "import_touch.h").write_text(header.replace(old, new), encoding="utf-8")
            exe, out = build(a.cxx, d, "mutant%d" % i, HEADER_TEST, inc)
            if not exe:
                raise SystemExit("mutation %r: the mutant does not build (a mutant must fail the test, not the compiler):\n%s" % (what, out))
            if run(exe).returncode == 0:
                raise SystemExit("mutation %r: the header test still passed without it" % what)
        print("PASS mutations: %d rules of import_touch.h, each taken out, fail the header's test" % len(MUTATIONS))
        if sys.platform != "win32":
            print("SKIP run: guarded_touch's handler is Windows' (the read is not made elsewhere)")
            return
        at = memory.index(TOUCH_PTR)
        guarded = memory[at:memory.index(TOUCH_SIG)] + block(memory, TOUCH_SIG)
        guarded = guarded[:guarded.rindex("}")] + "}"
        src = RUNTIME_TEST.replace("@GUARDED@", guarded)
        ms = ["-fms-extensions"]   # (__try, as the game's build has it)
        exe, out = build(a.cxx, d, "run", src, VK, ms)
        if not exe:
            raise SystemExit("the Windows run: the build failed:\n" + out)
        r = run(exe)
        if r.returncode:
            raise SystemExit("the Windows run failed:\n" + r.stdout)
        print(r.stdout.strip())
        for i, (what, old, new) in enumerate(RUNTIME_MUTATIONS):
            if header.count(old) != 1:
                raise SystemExit("mutation %r: its text is not once in import_touch.h (update the test with the header)" % what)
            inc = d / ("runmutant%d" % i)
            inc.mkdir()
            (inc / "import_touch.h").write_text(header.replace(old, new), encoding="utf-8")
            exe, out = build(a.cxx, d, "runmutant%d" % i, src.replace("(*volatile touch_read)(const volatile uint8_t*, size_t)",
                                                                       "(*volatile touch_read)(const uint8_t*, size_t)") if "volatile" in what else src, inc, ms)
            if not exe:
                raise SystemExit("mutation %r: the mutant does not build:\n%s" % (what, out))
            if run(exe).returncode == 0:
                raise SystemExit("mutation %r: the Windows run still passed without it" % what)
        print("PASS mutations: %d more, on the read itself, fail the Windows run" % len(RUNTIME_MUTATIONS))


if __name__ == "__main__":
    main()
