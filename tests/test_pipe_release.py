"""The kept pipelines no draw has had, given back to the driver while the PC's RAM is short (gpu/vk/pipe_release.h;
vk_draw.cpp graphics_pipeline, precompile_run and pipelines_release_tick; no game/GPU).

The header is compiled and run as it is, against a table of stand-in entries: a pass is due only with the RAM short and
not before so many presents since the last; a pass takes at most its batch, only the entries the rule lets go, and
leaves every other entry where it was (the same object, at the same key); a table with none to give leaves the pass
empty-handed. Then the source, where the safety is: graphics_pipeline marks an entry it hands out before any thread's
table can hold it, and an entry a draw made is marked from the start; the pass lets go only entries no draw has had,
built, with a pipeline and no fast link; it runs on the thread presenting, under the table's two locks (the parallel
replay's and the precompile's), and the pipelines are destroyed on a thread of its own; it is called from the presents;
the precompile in light mode keeps none of the graphics pipelines it builds and destroys a pipeline it built that it
does not keep; and the switches are in docs/development.md.
python -I -B tests/test_pipe_release.py --cxx clang++
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>
#include "pipe_release.h"
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); return 1; } } while (0)
struct Entry {
    bool looked = false, ready = true;
    int pipeline = 1, fast = 0;
};
static bool may_go(const Entry& e) { return !e.looked && e.ready && e.pipeline && !e.fast; }
int main() {
    using namespace pipe_release;
    // (due: the RAM short, and every presents since the last pass; the first at once)
    CHECK(!due(false, 1000, 0, 300) && due(true, 1000, 0, 300) && !due(true, 1299, 1000, 300) && due(true, 1300, 1000, 300));
    CHECK(!due(false, 5000, 1000, 300));
    std::mt19937 rng(5);
    for (int round = 0; round < 50; round++) {
        std::unordered_map<uint64_t, std::unique_ptr<Entry>> table;
        std::map<uint64_t, Entry*> before;
        const int n = 1 + (int)(rng() % 3000);
        size_t can = 0;
        for (int i = 0; i < n; i++) {
            auto e = std::make_unique<Entry>();
            e->looked = rng() % 3 == 0, e->ready = rng() % 10 != 0, e->pipeline = rng() % 20 ? 1 : 0, e->fast = rng() % 25 == 0;
            can += may_go(*e);
            before[(uint64_t)i * 7919] = e.get();
            table.emplace((uint64_t)i * 7919, std::move(e));
        }
        if (round % 7 == 0) table.emplace(999999999ull, nullptr);   // (an empty slot: never touched)
        std::vector<std::unique_ptr<Entry>> out;
        size_t taken = 0, passes = 0;
        for (;;) {
            const size_t got = take(table, kBatch, out, may_go);
            CHECK(got <= kBatch && out.size() == taken + got);
            taken += got, passes++;
            if (!got) break;
        }
        CHECK(taken == can && passes == (can + kBatch - 1) / kBatch + 1);
        for (const auto& e : out) CHECK(e && may_go(*e));
        // (what stays is every entry the rule keeps, the same object at the same key)
        size_t kept = 0;
        for (const auto& [key, ptr] : before) {
            const auto it = table.find(key);
            if (may_go(*ptr)) {
                CHECK(it == table.end());
            } else {
                CHECK(it != table.end() && it->second.get() == ptr);
                kept++;
            }
        }
        CHECK(kept + taken == (size_t)n && table.size() == kept + (round % 7 == 0 ? 1 : 0));
    }
    return 0;
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    if "std::atomic<bool> looked{false};" not in between(draw, "struct PipeEntry {", "\n};\n"):
        raise SystemExit("a pipeline entry does not say whether a draw has had it")
    gp = between(draw, "PipeEntry* graphics_pipeline(", "\n}\n")
    found = gp[gp.index("auto it = pipelines.find(key);"):gp.index("pcache_init();", gp.index("auto it = pipelines.find(key);"))]
    if "found->looked.exchange(true)" not in found or found.index("looked.exchange(true)") > found.index("return remember(mine[key] = found);"):
        raise SystemExit("graphics_pipeline hands out an entry before it marks it")
    made = gp[gp.index("PipeEntry* e = pipelines.emplace(key,"):]
    if made.index("e->looked.store(true") > made.index("return"):
        raise SystemExit("an entry a draw made is not marked as had before it is handed out")
    if gp.count("pipelines.find(") != 1 or gp.count("mine[key] = ") != 1:
        raise SystemExit("graphics_pipeline hands entries out in more ways than this test knows")
    # (the precompile is the only other place that reaches into the table, and only to add entries)
    if draw.count("pipelines.find(") != 2 or draw.count("pipelines.emplace(") != 2:
        raise SystemExit("vk_draw.cpp reaches into the pipeline table in a way this test does not know")
    tick = between(draw, "void pipelines_release_tick(uint64_t presents) {", "\n}\n")
    for need, why in (('getenv("RX_VK_PIPE_RELEASE")', "the switch"),
                      ("pipe_release::due(memory_ram_short(), presents, last, every)", "the RAM's rule"),
                      ("ParLock lk;\n        std::lock_guard nl(note_mu);", "the table's two locks"),
                      ("!e.looked.load(std::memory_order_acquire) && e.ready.load(std::memory_order_acquire) &&\n"
                       "                   e.pipeline.load(std::memory_order_relaxed) && !e.fast.load(std::memory_order_relaxed);",
                       "the rule (no draw has had it, built, a pipeline, no fast link)"),
                      ("std::thread([dead] {", "a thread of its own for the destroys")):
        if need not in tick:
            raise SystemExit("pipelines_release_tick lacks " + why)
    take = tick.index("pipe_release::take(")
    if tick.index("ParLock lk;") > take or tick.index("vkDestroyPipeline(") < take:
        raise SystemExit("the table is changed outside its locks, or a pipeline destroyed before it leaves the table")
    if "pipelines_release_tick((uint64_t)presents);" not in between(replay, "void log_frame_stats() {", "\n}\n"):
        raise SystemExit("the pass is not called from the presents")
    pre = between(draw, "void precompile_run(", "\n}\n")
    if "pre->light = light;" not in pre or "if (pre->light) {   // ((Triforce) in the caches now: its first draw builds it from there)\n" \
            "                        if (made) vkDestroyPipeline(dev.device, made, nullptr);" not in pre:
        raise SystemExit("the precompile in light mode keeps the graphics pipelines it builds")
    if "} else if (made) {" not in pre:
        raise SystemExit("the precompile loses a pipeline it built and does not keep")
    dev_md = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "`RX_VK_PIPE_RELEASE=0`, `RX_VK_PIPE_RELEASE_EVERY=<presents>`" not in dev_md:
        raise SystemExit("RX_VK_PIPE_RELEASE is not in docs/development.md's switches")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="pipe-release-") as tmp:
        tmp = Path(tmp)
        (tmp / "t.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / "pipe-release"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "gpu/vk"),
                        str(tmp / "t.cpp"), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        if r.returncode:
            raise SystemExit(f"pipe_release.h: {r.stdout.strip() or r.returncode}")
    print("PASS: the kept pipelines no draw has had are taken a batch at a time with the RAM short, every other one stays, and "
          "no entry a draw may hold can go")


if __name__ == "__main__":
    main()
