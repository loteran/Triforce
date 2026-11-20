"""The barrier a draw pass starts with (gpu/vk/pass_barrier.h, vk_texture.cpp pass_start_barrier; no game, no GPU).

Before every draw pass the replay records a full barrier, which makes the GPU drain between two passes. The game mode
(RX_VK_PASS_BARRIER=game, on by default with NVIDIA's driver) keeps the wait for the new pass's fragment stages only, where a
draw pass comes right after another with nothing recorded between. Compiles and runs the header's rule over the switch's
values and the vendors, then the production functions cut out of vk_texture.cpp (full_barrier, note_pass_end,
note_pass_split, pass_start_barrier) against a recording vkCmdPipelineBarrier2, in a process each for the switch's values:
which pass starts get the fragment-stage barrier and with which stages, that work recorded between, another command buffer,
a pass the replay split for a hazard and the parallel replay keep the full one, that the game's own barrier stays a full one
with no second barrier after it, and that a full barrier after the fragment-stage one is not left out. Reads the source for
what needs the game: that the replay's hazard splits mark their passes, that end_pass notes the end right after the pass,
and that the switch is in the development guide.

    python -I -B tests/test_pass_barrier.py [--cxx g++] --headers <Vulkan headers' include folder>
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_source():
    texture = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    if '#include "pass_barrier.h"' not in texture:
        raise SystemExit("vk_texture.cpp does not include pass_barrier.h")
    start = block(texture, "void pass_start_barrier(")
    for needle, why in (("pass_barrier::game(e, dev.props.vendorID,", "the mode is not the header's, from the switch and the vendor"),
                        ('getenv("RX_VK_PASS_BARRIER")', "no RX_VK_PASS_BARRIER switch"),
                        ('getenv("RX_VK_BARRIER_EVERY_DRAW")', "RX_VK_BARRIER_EVERY_DRAW does not keep its full barriers"),
                        ("pass_barrier::relaxed(", "the fragment-stage barrier is not the header's rule")):
        if needle not in start:
            raise SystemExit("vk_texture.cpp pass_start_barrier: " + why)
    for name in ("void clear_after_draws(", "uint16_t draw_hazards("):
        body = block(draw, name)
        at = body.rindex("end_pass(cmd);")
        if not body[at:].replace(" ", "").startswith("end_pass(cmd);\nnote_pass_split(cmd);"):
            raise SystemExit("vk_draw.cpp %s: the pass it ends for a hazard is not marked (note_pass_split)" % name.split("(")[0])
    begin = block(draw, "void begin_pass(")
    if not 0 <= begin.index("end_pass(cmd);") < begin.index("pass_start_barrier(cmd);"):
        raise SystemExit("vk_draw.cpp begin_pass: the open pass is not ended before the new one's barrier")
    end = block(draw, "void end_pass(")
    if not end.index("vkCmdEndRendering(cmd);") < end.index("note_pass_end(cmd);") < end.index("dump_on()"):
        raise SystemExit("vk_draw.cpp end_pass: the pass's end is not noted right after it, before the dumps' work")
    game = replay[replay.index("        case NOP_BARRIER:"):]
    game = game[:game.index("break;", game.index("st_barriers++"))]
    if not game.index("end_pass(cmd);") < game.index("full_barrier(cmd,"):
        raise SystemExit("vk_replay.cpp: the game's barrier is not a full one after the pass's end")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "`RX_VK_PASS_BARRIER=game` or `full`" not in dev:
        raise SystemExit("docs/development.md: RX_VK_PASS_BARRIER is not in the switches table")


HEADER_TEST = r'''
#include <cstdio>
#include <cstdlib>
#include "pass_barrier.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::pass_barrier;
int main() {
    CHECK(game(nullptr, 0x10DE, false));         // (unset: NVIDIA's driver)
    CHECK(game("", 0x10DE, false));              // (empty: as unset)
    CHECK(!game(nullptr, 0x1002, false));        // (AMD's)
    CHECK(!game(nullptr, 0x8086, false));        // (Intel's)
    CHECK(!game(nullptr, 0, false));
    CHECK(game("game", 0x1002, false));          // (asked for on any card)
    CHECK(game("game", 0x8086, false));
    CHECK(!game("full", 0x10DE, false));         // (the old way)
    CHECK(!game("0", 0x10DE, false));            // (any other value: full)
    CHECK(!game("1", 0x10DE, false));
    CHECK(!game("GAME", 0x10DE, false));
    CHECK(!game("games", 0x1002, false));
    CHECK(!game(nullptr, 0x10DE, true));         // (RX_VK_BARRIER_EVERY_DRAW)
    CHECK(!game("game", 0x10DE, true));
    CHECK(relaxed(true, true));
    CHECK(!relaxed(false, true));
    CHECK(!relaxed(true, false));                // (not right after a pass)
    std::printf("PASS: the game mode's switch and vendors\n");
}
'''

FUNCTIONS_TEST = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "pass_barrier.h"
#include "barrier_rules.h"
#include "cmd_work.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
namespace mkwvk {
void full_barrier(VkCommandBuffer cmd, const char* why);
void event_writes_record(VkCommandBuffer) {}   // (vk_replay.cpp's; no event's write is held here: tests/test_barrier_rules.py)
struct Recorded { VkCommandBuffer cmd; VkPipelineStageFlags2 src, dst; VkAccessFlags2 src_access, dst_access; };
std::vector<Recorded> recorded;
std::string logged;
void logf(const char* fmt, ...) {
    char line[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    logged += line;
    logged += "\n";
}
std::atomic<bool> par_active{false};
struct { struct { uint32_t vendorID = 0; } props; } dev;
struct GprofSpan { GprofSpan(VkCommandBuffer, const char*) {} };
void record_barrier(VkCommandBuffer cmd, const VkDependencyInfo* d) {
    CHECK(d->memoryBarrierCount == 1 && !d->bufferMemoryBarrierCount && !d->imageMemoryBarrierCount);
    const VkMemoryBarrier2& b = d->pMemoryBarriers[0];
    recorded.push_back({cmd, b.srcStageMask, b.dstStageMask, b.srcAccessMask, b.dstAccessMask});
}
// (as vk_work.h's: every barrier counts as work, in its command buffer: cmd_work.h)
#define vkCmdPipelineBarrier2(cmd, d) (note_work(cmd), record_barrier(cmd, d))
/* FUNCTIONS */
} // namespace mkwvk
using namespace mkwvk;
VkCommandBuffer A = (VkCommandBuffer)(uintptr_t)0x10, B = (VkCommandBuffer)(uintptr_t)0x20, C = (VkCommandBuffer)(uintptr_t)0x30;
const VkPipelineStageFlags2 kFragments = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
bool is_full(const Recorded& r) {
    return r.src == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT && r.dst == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT &&
           r.src_access == VK_ACCESS_2_MEMORY_WRITE_BIT && r.dst_access == (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}
bool is_fragments(const Recorded& r) {
    // (every earlier stage's writes, before this pass's fragment shaders, tests and colour output: their reads and writes)
    return r.src == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT && r.dst == kFragments && r.src_access == VK_ACCESS_2_MEMORY_WRITE_BIT &&
           r.dst_access == (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}
void work(VkCommandBuffer cmd) { note_work(cmd); }   // (a draw, a copy, an update into cmd: vk_work.h's count)
/// a pass ends on cmd (end_pass: vkCmdEndRendering, then note_pass_end), the next starts on next (begin_pass's barrier); what was recorded
std::vector<Recorded> pass_then_pass(VkCommandBuffer cmd, VkCommandBuffer next, void (*between)()) {
    work(cmd);   // (the pass's draws)
    work(cmd);   // (vkCmdEndRendering)
    note_pass_end(cmd);
    if (between) between();
    recorded.clear();
    pass_start_barrier(next);
    return recorded;
}
int main(int argc, char** argv) {
    CHECK(argc == 3);
    dev.props.vendorID = (uint32_t)std::strtoul(argv[1], nullptr, 16);
    const bool want_game = !std::strcmp(argv[2], "game");
    // ((Triforce, P4) each command buffer's own count (cmd_work.h); RX_VK_BARRIER_CONTEXT=0: the one count of every thread's commands)
    const bool own = !work_global();
    int64_t relaxed = 0;
    auto r = pass_then_pass(A, A, nullptr);   // (right after a pass, nothing between)
    CHECK(r.size() == 1 && (want_game ? is_fragments(r[0]) : is_full(r[0])) && r[0].cmd == A);
    CHECK(pass_barriers_relaxed() == (relaxed += want_game));
    CHECK(logged.find("vk: pass barriers: ") != std::string::npos);
    CHECK((logged.find("waits in its fragment stages only") != std::string::npos) == want_game);
    // (a full barrier right after the fragment-stage one is recorded: it orders more than that one did; right after a full one it
    // is left out)
    recorded.clear();
    full_barrier(A, "test");
    CHECK(recorded.size() == (want_game ? 1u : 0u));
    CHECK(recorded.empty() || is_full(recorded[0]));
    CHECK(logged.find(own ? "vk: barrier state: each command buffer's own" : "vk: barrier state: one count of every thread's") != std::string::npos);
    // (work recorded into the command buffer between the two passes: a full barrier, and one right after it is left out)
    r = pass_then_pass(A, A, [] { work(A); });
    CHECK(r.size() == 1 && is_full(r[0]));
    int64_t made = 0, left = 0;
    barrier_stats(&made, &left);
    recorded.clear();
    full_barrier(A, "test");
    int64_t made2 = 0, left2 = 0;
    barrier_stats(&made2, &left2);
    CHECK(recorded.empty() && made2 == made && left2 == left + 1);
    // (the next pass on another command buffer: full)
    r = pass_then_pass(A, B, nullptr);
    CHECK(r.size() == 1 && is_full(r[0]) && r[0].cmd == B);
    // (a pass the replay ended for a hazard, vk_draw.cpp draw_hazards and clear_after_draws: full)
    r = pass_then_pass(A, A, [] { note_pass_split(A); });
    CHECK(r.size() == 1 && is_full(r[0]));
    // (the game's barrier, vk_replay.cpp NOP_BARRIER: the pass ended, a full barrier; the next pass's start then records nothing more)
    r = pass_then_pass(A, A, [] { full_barrier(A, "barrier (the game's nvnCommandBufferBarrier)"); });
    CHECK(r.empty());
    // ((Triforce, P4) work into another command buffer between, this thread's (its uploads) or another thread's (a helper's segment):
    // not between the two passes on the queue, so the game mode holds; with the one count it moved, as before)
    r = pass_then_pass(A, A, [] { work(B); });
    CHECK(r.size() == 1 && (want_game && own ? is_fragments(r[0]) : is_full(r[0])));
    relaxed += want_game && own;
    r = pass_then_pass(A, A, [] { std::thread([] { work(C), work(C); }).join(); });
    CHECK(r.size() == 1 && (want_game && own ? is_fragments(r[0]) : is_full(r[0])));
    relaxed += want_game && own;
    // (the parallel replay: the game mode beside the helper threads; with the one count it stands aside, as before)
    par_active = true;
    r = pass_then_pass(A, A, nullptr);
    CHECK(r.size() == 1 && (want_game && own ? is_fragments(r[0]) : is_full(r[0])));
    relaxed += want_game && own;
    // (and a full barrier right after a full one is left out beside them too; with the one count, recorded)
    recorded.clear();
    full_barrier(A, "test");
    const size_t first = recorded.size();
    full_barrier(A, "test");
    CHECK(recorded.size() == first + (own ? 0u : 1u));
    par_active = false;
    // (a command buffer begun again starts afresh: its last full barrier and its last pass's end are not right before its first command)
    work(B), note_pass_end(B);
    work_begun(B);
    recorded.clear();
    pass_start_barrier(B);
    CHECK(recorded.size() == 1 && is_full(recorded[0]));
    work_begun(B);
    recorded.clear();
    full_barrier(B, "test");
    CHECK(recorded.size() == 1);
    // (and the mode again, the work count moved on since)
    r = pass_then_pass(B, B, nullptr);
    CHECK(r.size() == 1 && (want_game ? is_fragments(r[0]) : is_full(r[0])));
    CHECK(pass_barriers_relaxed() == (relaxed += want_game));
    std::printf("PASS: vendor %s, RX_VK_PASS_BARRIER=%s, RX_VK_BARRIER_CONTEXT=%s: %s\n", argv[1],
                std::getenv("RX_VK_PASS_BARRIER") ? std::getenv("RX_VK_PASS_BARRIER") : "(unset)", own ? "(unset)" : "0",
                want_game ? (own ? "a pass right after another on its command buffer waits in its fragment stages only, beside other "
                                   "command buffers' work and the helper threads too"
                                 : "a pass right after another waits in its fragment stages only, the one count of every thread's commands")
                          : "every pass start a full barrier");
}
'''

# (the switch, RX_VK_BARRIER_EVERY_DRAW, the vendor) -> the game mode
RUNS = [(None, None, "10de", "game"), (None, None, "1002", "full"), (None, None, "8086", "full"),
        ("game", None, "1002", "game"), ("full", None, "10de", "full"), ("other", None, "10de", "full"),
        (None, "1", "10de", "full"), ("game", "1", "1002", "full")]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--headers", type=Path, required=True)
    a = p.parse_args()
    check_source()
    texture = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    start = texture.index("std::atomic<uint64_t> work_epoch{0};")
    functions = texture[start:texture.index("void texture_begin_recording(", start)]
    for name in ("void full_barrier(", "void note_pass_end(", "void note_pass_split(", "void pass_start_barrier("):
        if name not in functions:
            raise SystemExit("vk_texture.cpp: %s is not beside the pass barrier any more" % name.rstrip("("))
    vk = str(ROOT / "gpu" / "vk")
    with tempfile.TemporaryDirectory(prefix="pass-barrier-") as tmp:
        tmp = Path(tmp)
        for name, text in (("header", HEADER_TEST), ("functions", FUNCTIONS_TEST.replace("/* FUNCTIONS */", functions))):
            cpp, exe = tmp / (name + ".cpp"), tmp / (name + ".exe")
            cpp.write_text(text, encoding="utf-8")
            subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                            "-I" + vk, "-I" + str(a.headers), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(tmp / "header.exe")], check=True)
        for switch, every, vendor, want in RUNS:
            for context in (None, "0"):   # ((Triforce, P4) each command buffer's own count, and RX_VK_BARRIER_CONTEXT=0's one)
                env = {k: v for k, v in os.environ.items()
                       if k not in ("RX_VK_PASS_BARRIER", "RX_VK_BARRIER_EVERY_DRAW", "RX_VK_BARRIER_CONTEXT")}
                if switch is not None:
                    env["RX_VK_PASS_BARRIER"] = switch
                if every is not None:
                    env["RX_VK_BARRIER_EVERY_DRAW"] = every
                if context is not None:
                    env["RX_VK_BARRIER_CONTEXT"] = context
                subprocess.run([str(tmp / "functions.exe"), vendor, want], env=env, check=True, timeout=30)
    print("PASS: the barrier a draw pass starts with")


if __name__ == "__main__":
    main()
