"""The device lost's markers at the boundaries between a frame's work (gpu/vk/checkpoint_kinds.h; no game, no GPU).

The owner's test11 and test12 sessions and two lab runs of five lost the device at the title the same way: the top and the
bottom of the pipe both at the last "write-back of pool ..." marker, the next marker the next frame's first dispatch, never
reached. Between the two lay a pool's write-back copies, the write-back diff pass, the end of that recording, the whole present
(its recording, the blit to the window's image after its acquire, the menu over it) and the next recording's start and
uploads, none of them marked: the report could not say which of them the GPU stopped in.

Compiles checkpoint_kinds.h alone and checks that every kind the sources set has a name and a reading of what follows it,
and how the two stages' last markers are read together. Reads the sources for the markers themselves: the write-back diff
pass (w) before its pipeline is bound, a recording's start (r) and end (z, after its write-backs, before its command buffer
ends), its uploads (v, at their first use), the present's blit (p after its first barrier, M before the menu, P before its
command buffer ends); and the report's lines (the two stages read together, what follows the top's marker, the switches, the
queue's timeline, the mirrors' piece binds). Each of these taken out in turn fails the test.

    python -I -B tests/test_checkpoint_kinds.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def block(text, marker):
    """The function that starts at marker, to its closing brace"""
    if text.count(marker) != 1:
        raise SystemExit("FAIL: not found once: " + marker)
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def kinds_set():
    """Every marker kind the back end's sources set (checkpoint_mark(..., 'x') and the draws' and dispatches' own)"""
    kinds = set()
    for f in sorted(VK.glob("*.cpp")):
        text = f.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"checkpoint_mark\([^;]*?'(.)'\)", text):
            kinds.add(m.group(1))
        for m in re.finditer(r"\bcheckpoint\(cmd, p, ([^;]*?)\);", text):
            kinds.update(re.findall(r"'(.)'", m.group(1)))
    return kinds


FIXTURE = r"""
#include "checkpoint_kinds.h"
#include <cstdio>
#include <cstring>
using namespace mkwvk::checkpoint_kinds;
int fails = 0;
#define CHECK(c, what) do { if (!(c)) { std::printf("FAIL: %s\n", what); fails++; } } while (0)
int main() {
    const char kinds[] = "@@KINDS@@";
    for (const char* k = kinds; *k; k++) {
        char what[96];
        std::snprintf(what, sizeof what, "kind '%c' has a name", *k);
        CHECK(std::strcmp(name(*k), "?") != 0, what);
        std::snprintf(what, sizeof what, "kind '%c' has a reading of what follows it", *k);
        CHECK(std::strncmp(gap(*k), "not known", 9) != 0, what);
    }
    CHECK(std::strcmp(name('#'), "?") == 0, "a kind of no name is '?'");
    CHECK(std::strncmp(gap('#'), "not known", 9) == 0, "a kind of no name has no reading");
    // (the boundaries: the write-back's next marker, a recording's end, the present's blit)
    CHECK(std::strstr(gap('b'), "diff pass") && std::strstr(gap('b'), "(z)"), "after a write-back: the diff pass, or the recording's end");
    CHECK(std::strstr(gap('z'), "acquired") && std::strstr(gap('z'), "bound"), "after a recording's end: the next submission's waits");
    CHECK(std::strstr(gap('M'), "menu") != nullptr, "after M: the menu's drawing");
    // (the two stages read together)
    CHECK(std::strstr(reading(0, 0), "no marker") != nullptr, "none reached");
    CHECK(std::strstr(reading(75943, 75943), "nothing after it began") != nullptr, "the owner's case: top and bottom alike");
    CHECK(std::strstr(reading(75950, 75943), "was running") != nullptr, "the top ahead of the bottom: the work between them ran");
    CHECK(std::strstr(reading(75943, 75950), "not in order") != nullptr, "the bottom past the top");
    // ((Triforce, P4) each thread numbers its own markers: two of two threads' rings are not read as one order)
    CHECK(std::strstr(apart(), "two threads") != nullptr, "the two stages' markers in two threads' rings");
    if (!fails) std::printf("checkpoint kinds: %zu kinds named and read\n", std::strlen(kinds));
    return fails ? 1 : 0;
}
"""


def check_sources():
    memory = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    texture = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    fails = []

    def before(text, first, then, why):
        a, b = text.find(first), text.find(then)
        if a < 0 or b < 0 or a > b:
            fails.append(why)

    end = block(memory, "void memory_end_submission(VkCommandBuffer cmd) {")
    before(end, "checkpoint_mark(cmd, chunks.size(), 'w');", "vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wbd_pipe.pipe);",
           "the write-back diff pass has no marker before its pipeline is bound (vk_memory.cpp memory_end_submission)")
    begin = block(replay, "Recording& begin_recording(uint64_t value) {")
    before(begin, "vkBeginCommandBuffer(r.cmd, &bi);", "checkpoint_mark(r.cmd, value, 'r');",
           "a recording's start has no marker after its command buffer begins (vk_replay.cpp begin_recording)")
    submit = block(replay, "void submit_recording(Recording& r, bool signal_value, uint64_t signal_at, bool part) {") \
        if "void submit_recording(Recording& r, bool signal_value, uint64_t signal_at, bool part) {" in replay \
        else block(replay, "void submit_recording(Recording& r, bool signal_value = true, uint64_t signal_at = 0, bool part = false) {")
    before(submit, "texture_uploads_recorded();", "checkpoint_mark(r.cmd, r.value, 'z');",
           "a recording's end marker comes before its last uploads' flush (vk_replay.cpp submit_recording)")
    before(submit, "checkpoint_mark(r.cmd, r.value, 'z');", "vkEndCommandBuffer(r.cmd);",
           "a recording's end has no marker before its command buffer ends (vk_replay.cpp submit_recording)")
    up = block(texture, "VkCommandBuffer upload_cmd() {")
    before(up, "full_barrier(up_cmd);", "checkpoint_mark(up_cmd, rec_value, 'v');",
           "a recording's uploads have no marker at their first use (vk_texture.cpp upload_cmd)")
    present = block(replay, "void present_image(VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImageView view) {")
    before(present, "full_barrier(f.cmd);", "checkpoint_mark(f.cmd, index, 'p');",
           "the present's blit has no marker after its first barrier (vk_replay.cpp present_image)")
    before(present, "checkpoint_mark(f.cmd, index, 'M');", "menu_record(f.cmd,",
           "the menu has no marker before it is drawn (vk_replay.cpp present_image)")
    before(present, "checkpoint_mark(f.cmd, index, 'P');", "if (vkEndCommandBuffer(f.cmd) == VK_ERROR_DEVICE_LOST)",
           "the present's blit has no marker before its command buffer ends (vk_replay.cpp present_image)")
    report = block(draw, "void checkpoints_report() {")
    for needle, why in (("checkpoint_kinds::reading(top, bottom)", "the report does not read the two stages together"),
                        ("apart ? checkpoint_kinds::apart() :", "the report reads two threads' markers as one order ((Triforce, P4) rings)"),
                        ("checkpoint_kinds::gap(top_kind)", "the report does not say what follows the top's marker"),
                        ("replay_lost_report();", "the report leaves out the queue's timeline (vk_replay.cpp replay_lost_report)"),
                        ("memory_lost_report();", "the report leaves out the memory work and the piece binds (vk_memory.cpp)"),
                        ("RX_VK_DESC_KEEP", "the report leaves out the descriptor switches")):
        if needle not in report:
            fails.append(why)
    if "return checkpoint_kinds::name(c);" not in draw:
        fails.append("vk_draw.cpp checkpoint_kind does not name the kinds by checkpoint_kinds.h")
    # ((Triforce, P4) each marking thread's own ring and sequence, no atomic shared by the recording threads)
    for name in ("void checkpoint(VkCommandBuffer cmd, const Program& p, char kind) {", "void checkpoint_mark(VkCommandBuffer cmd, uint64_t key, char kind) {"):
        body = block(draw, name)
        if "checkpoint_next(&seq)" not in body or "checkpoint_n" in body.replace("checkpoint_next", ""):
            fails.append("vk_draw.cpp %s: not this thread's ring and sequence (checkpoint_next)" % name.split("(")[0])
    nxt = block(draw, "uint32_t checkpoint_next(uint32_t* seq) {")
    if "if (checkpoint_ring_alone) s = n.load(std::memory_order_relaxed) + 1, n.store(s, std::memory_order_relaxed);" not in nxt:
        fails.append("vk_draw.cpp checkpoint_next: a thread alone in its ring does not count with a plain load and store")
    lost = block(memory, "void memory_lost_report() {")
    for needle in ("piece_bind_value.load()", "vkGetSemaphoreCounterValue(dev.device, piece_bind_tl", "piece_binds_mu.try_lock()"):
        if needle not in lost:
            fails.append("vk_memory.cpp memory_lost_report: " + needle + " is missing (the piece binds the submits wait for, read "
                         "without a lock the lost thread may hold)")
    rlost = block(replay, "void replay_lost_report() {")
    if "vkGetSemaphoreCounterValue(dev.device, timeline" not in rlost or "lock" in rlost:
        fails.append("vk_replay.cpp replay_lost_report: the timeline's value, read without replay_mu")
    for f in fails:
        print("FAIL: " + f)
    return not fails


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    ok = check_sources()
    kinds = kinds_set()
    for k in "dcimbugwrvzpMP":
        if k not in kinds:
            print("FAIL: no marker of kind '%s' is set in gpu/vk/*.cpp" % k)
            ok = False
    with tempfile.TemporaryDirectory(prefix="checkpoint-kinds-") as tmp:
        src = Path(tmp) / "kinds.cpp"
        src.write_text(FIXTURE.replace("@@KINDS@@", "".join(sorted(kinds))), encoding="utf-8")
        exe = Path(tmp) / "kinds.exe"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(VK), str(src), "-o", str(exe)], check=True)
        ok = subprocess.run([str(exe)]).returncode == 0 and ok
    if not ok:
        raise SystemExit(1)
    print("ok: the frame's boundaries are marked and the report reads them")


if __name__ == "__main__":
    main()
