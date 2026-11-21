"""The trial of the newer memory handling on a card that is not NVIDIA's (gpu/vk/vk_memory.cpp memory_trial_on,
memory_trial_used, memory_trial_tick, memory_trial_device_lost; triforce_memtrial.txt; no game/GPU).

A lab run with RX_VK_MIRROR_PIECES=0 and RX_VK_COMPACT=0 passed the trial on an AMD card whose session with the handling
on had blue-screened the PC that morning: memory_trial_tick said "ok" 300 s after the start whatever those switches said,
so the trial passed with neither part ever on, and the next run without them had both on, untested. A session counts
for the trial now only when the handling runs in it, and passes once all of it has run for the trial's time.

The trial's code is cut from vk_memory.cpp into a fixture (stand-ins for the device, the log and the executable's path)
and run once a session, on a file of its own:
- with a switch that keeps a part off (RX_VK_MIRROR_PIECES=0, RX_VK_MIRROR=0, RX_VK_COMPACT=0, RX_VK_NO_IMAGE_POOL) the
  file stays as it was, byte for byte, through the trial's time, a device lost and an orderly end, and the log says which
  switch; a pass of earlier sessions' "trying" is not turned into a failure there either;
- a session in which nothing of the handling ran says nothing: the file stays as it was;
- one part alone in use (the compaction, on a card that has mirrors in pieces) begins the trial (trying) but does not pass
  it, and an orderly end then says new;
- both parts in use pass it ("ok"), and on a card without sparse residency buffers the compaction alone does, as before;
- a device lost with it on turns it off, as before, and the same after a trial that passed starts one more;
- a session with a switch keeping a part off has the other parts off too (memory_trial_on says no): nothing would turn
  them off if they lost the GPU there;
- the file says v=2 now; a v=1 ok came from a build that passed the trial with the handling switched off, so the trial
  runs again; a v=1 trying is one bad session, and a v=1 off stays off;
- a part the driver could not make (a mirror's sparse buffer refused, the textures' pool not made) is not waited for, or
  the trial could never pass there;
- RX_VK_MIRROR=0 or RX_VK_MIRROR_PIECES=0 on an integrated GPU (mirrors off by default) changes nothing for the trial;
- the log says why a session gave no verdict: the part it waits for, an end before the trial was over, nothing ran.
Then the source: the first piece of a mirror bound and a texture made in the compacted pool say so (memory_trial_used),
a refused sparse buffer or textures' pool says so (memory_trial_refused), and memory_trial_tick passes only once all of
it is in use.

python -I -B tests/test_memory_trial.py [--cxx g++]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


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


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit("FAIL: " + where + " no longer has: " + needle)


FIXTURE = r"""
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
typedef unsigned long DWORD;
typedef long LONG;
#define WINAPI
#define MAX_PATH 260
#define EXCEPTION_CONTINUE_SEARCH 0
struct EXCEPTION_POINTERS {};
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS*);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER) { return nullptr; }
#endif
namespace mkwvk {
struct {
    struct { uint32_t vendorID = 0x1002, deviceID = 0x744c, driverVersion = 0x8001a0; } props;
    bool sparse_buffers = true;
    struct { bool sparseResidencyBuffer = true; } features;
} dev;
void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
}
DWORD game_module_path(char* buf, DWORD size) {
    const char* p = std::getenv("TRIAL_FIXTURE_EXE");
    std::snprintf(buf, size, "%s", p ? p : "game.exe");
    return (DWORD)std::strlen(buf);
}
@@ENUM@@
bool memory_trial_on();
void memory_trial_used(MemoryTrialPart part);
void memory_trial_refused(MemoryTrialPart part);
@@TRIAL@@
namespace {   // (as vk_memory.cpp's: RX_VK_MIRROR decides, else on a discrete GPU; TRIAL_FIXTURE_IGPU plays an integrated one)
bool mirror_by_default() { return std::getenv("TRIAL_FIXTURE_IGPU") == nullptr; }
bool mirror_on() { return std::getenv("RX_VK_MIRROR") ? std::atoi(std::getenv("RX_VK_MIRROR")) != 0 : mirror_by_default(); }
}
}   // namespace mkwvk
int main(int argc, char** argv) {
    using namespace mkwvk;
    if (argc > 2 && argv[2][0] == '0') dev.sparse_buffers = false;
    for (const char* a = argc > 1 ? argv[1] : ""; *a; a++) switch (*a) {
        case 'o': std::printf("on: %d\n", (int)memory_trial_on()); break;
        case 'p': memory_trial_used(MEMTRIAL_PIECES); break;
        case 'c': memory_trial_used(MEMTRIAL_COMPACT); break;
        case 'r': memory_trial_refused(MEMTRIAL_PIECES); break;
        case 'R': memory_trial_refused(MEMTRIAL_COMPACT); break;
        case 't': memory_trial_tick(); break;
        case 'l': memory_trial_device_lost(); break;
        case 'e': memory_trial_orderly_end(); break;
    }
    std::fflush(stdout);
    std::_Exit(0);   // (as a session killed: the exit handler's own word is 'e')
}
"""

HEAD = "v=2 vendor=1002 device=744c driver=8001a0 state=%s fails=%d\n"
HEAD_V1 = "v=1 vendor=1002 device=744c driver=8001a0 state=%s fails=%d\n"   # (the builds before 11-17)


def trial_file(state, fails, head=HEAD):
    return head % (state, fails) + "# set by the test\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    vma = (ROOT / "gpu/vk/vk_vma.cpp").read_text(encoding="utf-8")
    internal = (ROOT / "gpu/vk/vk_internal.h").read_text(encoding="utf-8")

    # ---- the source: where the parts say they ran, and the pass waits for them
    tick = block(mem, "void memory_trial_tick() {")
    need("vk_memory.cpp: memory_trial_tick()", tick, ["in_use.load(std::memory_order_acquire)", 'write(OK, 0, "played with it on: kept");'])
    on = block(mem, "bool memory_trial_on() {")
    need("vk_memory.cpp: memory_trial_on()", on, ["held_off = switches_off();", "if (was != OFF && held_off.empty()) write(OFF,",
                                                   "does not count for the trial", "candidate = true;", "(v == kVersion || v == 1)",
                                                   "if (v == 1 && was == OK) {"])
    if "write(TRYING" in on or "SetUnhandledExceptionFilter" in on:
        raise SystemExit("FAIL: memory_trial_on begins the trial before any of the handling has run")
    if on.index("if (!held_off.empty())") > on.index("candidate = true;"):
        raise SystemExit("FAIL: memory_trial_on makes a session with a part switched off count for the trial")
    held = block(on, "if (!held_off.empty()) {")
    if "return false;" not in held:
        raise SystemExit("FAIL: memory_trial_on turns on, with no trial to guard them, the parts no switch names")
    if "constexpr int kVersion = 2;" not in mem:
        raise SystemExit("FAIL: vk_memory.cpp's triforce_memtrial.txt is not v=2: a v=1 file's ok cannot be told from a pass given switched off")
    used = block(mem, "void memory_trial_used(MemoryTrialPart part) {")
    need("vk_memory.cpp: memory_trial_used()", used, ["if (!candidate || state != TRYING) return;", 'write(TRYING, fails, "a session has it on");',
                                                       "all_in_use();"])
    need("vk_memory.cpp: all_in_use()", block(mem, "void all_in_use() {"),
         ["if (needed(p) && !used[p].load()) return;", "in_use.store(true, std::memory_order_release);"])
    need("vk_memory.cpp: needed()", block(mem, "bool needed(int part) {"), ["if (refused[part].load()) return false;"])
    need("vk_memory.cpp: memory_trial_refused()", block(mem, "void memory_trial_refused(MemoryTrialPart part) {"),
         ["if (active.load()) all_in_use();"])
    need("vk_memory.cpp: switches_off()", block(mem, "std::string switches_off() {"),
         ['if (pieces_can() && mirror_on()) add("RX_VK_MIRROR_PIECES")', 'if (pieces_can() && mirror_by_default()) add("RX_VK_MIRROR")',
          'add("RX_VK_COMPACT")', 'getenv("RX_VK_NO_IMAGE_POOL")'])
    need("vk_memory.cpp: mirror_on()", block(mem, "bool mirror_on() {\n    static"), ["atoi(e) != 0 : mirror_by_default();"])
    need("vk_memory.cpp: mirror_pieces_make()", block(mem, "bool mirror_pieces_make(Pool& p,"),
         ["st_piece_binds++;", "memory_trial_used(MEMTRIAL_PIECES);"])
    make = block(mem, "if (mirror_pieces_on() && n <= (1ull << 30) && !tight && n >= 4 * piece_cfg) {")
    need("vk_memory.cpp: mirror_make()'s pieces", make, ["if (!in_pieces) memory_trial_refused(MEMTRIAL_PIECES);"])
    need("vk_vma.cpp: textures_pool()", block(vma, "VmaPool textures_pool() {"),
         ["memory_trial_refused(MEMTRIAL_COMPACT);", "if (!texture_pool) memory_trial_refused(MEMTRIAL_COMPACT);"])
    # (v0.5.1: the work is create_image_inner_, create_image_inner names the image for RX_VK_RT_MAP around it)
    need("vk_vma.cpp: create_image_inner_()", block(vma, "bool create_image_inner_(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool dedicated, bool target, bool textures) {"),
         ["if (pool == texture_pool) memory_trial_used(MEMTRIAL_COMPACT);", "== VK_SUCCESS) return made();"])
    enum = [line for line in internal.splitlines() if line.startswith("enum MemoryTrialPart")]
    if len(enum) != 1:
        raise SystemExit("FAIL: vk_internal.h has no enum MemoryTrialPart line")

    # ---- the trial's code, run
    start = mem.index("namespace { bool mirror_on(); bool mirror_by_default(); }")
    lost = mem.index("void memory_trial_device_lost() {")
    trial = mem[start:lost] + block(mem, "void memory_trial_device_lost() {")
    with tempfile.TemporaryDirectory(prefix="memtrial-") as tmp:
        src, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        src.write_text(FIXTURE.replace("@@ENUM@@", enum[0]).replace("@@TRIAL@@", trial), encoding="utf-8")
        command = [args.cxx, "-std=c++20", "-O1", str(src), "-o", str(exe)] + ([] if os.name == "nt" else ["-pthread"])
        r = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("the fixture did not build")
        file = Path(tmp) / "triforce_memtrial.txt"
        base_env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_") and not k.startswith("TRIAL_FIXTURE_")}
        base_env.update(TRIAL_FIXTURE_EXE=str(Path(tmp) / "game.exe"), RX_VK_TEST_TRIAL_S="0")
        failures = []

        def session(name, before, steps, env=(), sparse=True, after=None, log=()):
            if before is None:
                file.unlink(missing_ok=True)
            else:
                file.write_text(before, encoding="utf-8", newline="\n")
            e = dict(base_env)
            e.update(env)
            r = subprocess.run([str(exe), steps, "1" if sparse else "0"], capture_output=True, text=True, timeout=30, env=e)
            got = file.read_text(encoding="utf-8") if file.exists() else None
            # (after: None for the file as it was, byte for byte; else the state and fails it must say)
            want = before if after is None else HEAD % after
            ok = r.returncode == 0 and (got == before if after is None else got is not None and got.startswith(want))
            ok = ok and all(s in r.stdout for s in log)
            print(("pass " if ok else "FAIL ") + name)
            if not ok:
                failures.append(name)
                print("  log:\n    " + r.stdout.strip().replace("\n", "\n    ") + "\n  file: " + repr(got) + "\n  wanted: " + repr(want))

        trying = trial_file("trying", 0)   # (the session that blue-screened the PC: it left no word)
        # (the lab's pins: the file stays as it was through the trial's time, a device lost and an orderly end)
        for pins in ({"RX_VK_MIRROR_PIECES": "0", "RX_VK_COMPACT": "0"}, {"RX_VK_COMPACT": "0"}, {"RX_VK_MIRROR_PIECES": "0"},
                     {"RX_VK_MIRROR": "0"}, {"RX_VK_NO_IMAGE_POOL": "1"}):
            label = ", ".join("%s=%s" % kv for kv in pins.items())
            session("pinned (%s): no verdict, the rest off too" % label, trying, "otttte", pins,
                    log=["does not count for the trial", label.split(",")[0].replace("=1", ""), "on: 0"])
            session("pinned (%s), parts it lets on used: no verdict" % label, trying, "opcttt", pins)
            session("pinned (%s), device lost: no verdict" % label, trying, "optl", pins)
            session("pinned (%s), after a pass, device lost: no verdict" % label, trial_file("ok", 0), "opcl", pins)
        session("pinned, one earlier failure and no word: not turned off here", trial_file("trying", 1), "otl",
                {"RX_VK_MIRROR_PIECES": "0", "RX_VK_COMPACT": "0"})
        # (no pieces on this card: RX_VK_MIRROR_PIECES=0 keeps nothing off, and the compaction alone passes)
        session("no sparse buffers, RX_VK_MIRROR_PIECES=0: compaction alone passes", trying, "octt", {"RX_VK_MIRROR_PIECES": "0"}, False, ("ok", 0))
        # (the players' sessions, no switches)
        session("nothing of it ran: no verdict", trying, "ottte", log=["nothing of it ran in this session: no verdict"])
        session("nothing of it ran, ended with no word: no verdict", trial_file("new", 0), "ott")
        session("compaction alone in use on a card with pieces: trying, not passed", trying, "octtt", after=("trying", 1),
                log=["waits for mirrors in pieces, which has not run"])
        session("compaction alone in use, orderly end: new", trying, "octte", after=("new", 1),
                log=["ended before the trial was over: no verdict"])
        # (a part the driver could not make is not waited for)
        session("sparse buffer refused: compaction alone passes", None, "ocrt", after=("ok", 0), log=["goes on without mirrors in pieces"])
        session("sparse buffer refused before any use: compaction alone passes", None, "orct", after=("ok", 0))
        session("textures' pool not made: pieces alone pass", None, "oRpt", after=("ok", 0))
        session("sparse buffer refused, nothing else ran: no verdict", trying, "ortte")
        # (an integrated GPU: mirrors off by default, so the switches that keep them off change nothing)
        igpu = {"TRIAL_FIXTURE_IGPU": "1"}
        session("integrated GPU, RX_VK_MIRROR=0: compaction alone passes", None, "oct", dict(igpu, RX_VK_MIRROR="0"), after=("ok", 0))
        session("integrated GPU, RX_VK_MIRROR_PIECES=0: compaction alone passes", None, "oct", dict(igpu, RX_VK_MIRROR_PIECES="0"), after=("ok", 0))
        session("integrated GPU, RX_VK_MIRROR=1 and RX_VK_MIRROR_PIECES=0: no verdict", trying, "opctl",
                dict(igpu, RX_VK_MIRROR="1", RX_VK_MIRROR_PIECES="0"), log=["RX_VK_MIRROR_PIECES=0 keeps a part of it off"])
        session("discrete GPU, RX_VK_MIRROR=0: no verdict", trying, "octl", {"RX_VK_MIRROR": "0"}, log=["RX_VK_MIRROR=0 keeps"])
        # (a v=1 file, from the builds before 11-17: its ok may have been given with the handling switched off)
        old_ok = trial_file("ok", 0, HEAD_V1)
        session("v=1 ok: on trial again, and passes when it runs", old_ok, "opct", after=("ok", 0), log=["passed under an older build"])
        session("v=1 ok: on trial again, killed before the trial's time: trying", old_ok, "opc", {"RX_VK_TEST_TRIAL_S": "3600"}, after=("trying", 0))
        session("v=1 ok, pinned: off, file as it was", old_ok, "opctl", {"RX_VK_MIRROR_PIECES": "0", "RX_VK_COMPACT": "0"}, log=["on: 0"])
        session("v=1 ok, nothing of it ran: file as it was", old_ok, "ottte")
        session("v=1 trying: one bad session", trial_file("trying", 0, HEAD_V1), "octt", after=("trying", 1))
        session("v=1 trying, one earlier failure: off", trial_file("trying", 1, HEAD_V1), "o", after=("off", 2), log=["on: 0"])
        session("v=1 off: stays off", trial_file("off", 2, HEAD_V1), "opctl", log=["on: 0", "is off on this card"])
        session("both in use: passed", trying, "octpt", after=("ok", 0), log=["passed its trial"])
        session("both in use, first session: passed", None, "opct", after=("ok", 0))
        session("no sparse buffers: compaction alone passes", None, "oct", sparse=False, after=("ok", 0))
        session("mirrors off (integrated GPU): compaction alone passes", None, "oct", {"TRIAL_FIXTURE_IGPU": "1"}, after=("ok", 0))
        session("both in use, ended before the trial's time: new", None, "opce", {"RX_VK_TEST_TRIAL_S": "3600"}, after=("new", 0))
        session("both in use, killed before the trial's time: trying", None, "opct", {"RX_VK_TEST_TRIAL_S": "3600"}, after=("trying", 0))
        session("device lost with it on: off", trying, "opcl", after=("off", 2))
        session("device lost after a pass: tried once more", trial_file("ok", 0), "opcl", after=("new", 1))
        session("two sessions without a word: off", trial_file("trying", 1), "o", after=("off", 2))
        session("a pass on another card does not carry over", "v=2 vendor=1002 device=7480 driver=8001a0 state=ok fails=0\n", "opct", after=("ok", 0),
                log=["is on trial on this card"])
        session("NVIDIA: on, no file", None, "opctl", {"RX_VK_TEST_VENDOR": "10de"}, log=["on: 1"])
        if failures:
            raise SystemExit("FAIL: %d session(s): %s" % (len(failures), "; ".join(failures)))
    print("PASS: a session counts for the memory trial only when the handling runs in it; switches that keep it off leave the file as it was")


if __name__ == "__main__":
    main()
