"""The pipeline cache let go in play when the PC's RAM runs short, only once no creation is using it (gpu/vk/pcache_gate.h;
vk_draw.cpp pcache_use and pcache_let_go; no game/GPU).

The header is compiled and run as it is, with threads: sixteen builders make "pipelines" with the cache over and over,
each through a Use held for the call, against a stand-in driver that knows whether the cache is alive and counts each
creation that ran with it; a seventeenth closes the gate and destroys the cache while they run. No creation ever runs
with a destroyed cache, none takes it once the gate is closed, every builder goes on without it, close returns only
once nobody is inside, and a second close does nothing. A Use asked for no cache never enters.
Then the source: every pipeline creation of vk_draw.cpp that may run in play takes the cache through the gate (the
graphics builds, the clear pipelines, the mesh fallback's, the compute dispatches', the precompile's computes, which may
run on after the game starts), vk_aa.cpp's and vk_fsr3.cpp's through shared_cache_compute, the save through the gate
too, and the cache is destroyed after the gate is closed, by a thread of its own, once a session, behind
RX_VK_PCACHE_RAM.
python -I -B tests/test_pcache_gate.py --cxx clang++
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include "pcache_gate.h"
#define CHECK(x) do { if (!(x)) { std::printf("line %d: %s\n", __LINE__, #x); return 1; } } while (0)
using Cache = uintptr_t;
static std::atomic<bool> alive{true};
static std::atomic<int64_t> with_cache{0}, without{0}, after_close{0}, dead_use{0};
static std::atomic<bool> closed_seen{false};
// The stand-in driver: a creation with the cache reads it for a while; one that finds it destroyed is the bug
static void create(Cache c) {
    if (!c) {
        without++;
        return;
    }
    with_cache++;
    if (closed_seen.load()) after_close++;
    for (int i = 0; i < 50; i++)
        if (!alive.load()) dead_use++;
}
int main() {
    pcache_gate::Gate gate;
    const Cache cache = 0x5eed;
    {   // (a Use asked for no cache never enters)
        pcache_gate::Use<Cache> u(gate, 0);
        CHECK(!u.in && u.cache == 0 && gate.inside() == 0);
    }
    {
        pcache_gate::Use<Cache> u(gate, cache);
        CHECK(u.in && u.cache == cache && gate.inside() == 1);
    }
    CHECK(gate.inside() == 0);
    std::atomic<bool> stop{false};
    std::vector<std::thread> builders;
    for (int t = 0; t < 16; t++)
        builders.emplace_back([&] {
            while (!stop.load()) create(pcache_gate::Use<Cache>(gate, cache).cache);   // (held for the call, as vk_draw.cpp's)
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int waits = 0;
    const bool first = gate.close([&] {
        waits++;
        std::this_thread::yield();
    });
    // (closed and nobody was inside: from here no creation takes the cache. A builder turned away is counted in and out
    // again, so inside() may be above 0 for a moment after this: none of those uses the cache)
    closed_seen = true;
    alive = false;   // (vkDestroyPipelineCache)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop = true;
    for (std::thread& t : builders) t.join();
    CHECK(first && gate.closed());
    CHECK(dead_use == 0 && after_close == 0);
    CHECK(with_cache > 0 && without > 0);
    CHECK(!gate.close([] {}));   // (once only)
    pcache_gate::Use<Cache> late(gate, cache);
    CHECK(!late.in && late.cache == 0 && gate.inside() == 0);
    std::printf("%lld with the cache, %lld without, %d waits\n", (long long)with_cache.load(), (long long)without.load(), waits);
    return 0;
}
"""


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    # (every creation given the cache: through the gate. Left as they were: the prepare's and warm pass's, before play and
    # joined before it begins)
    allowed_raw = ("void warm_libraries(", "bool prepare_run(")
    for m in re.finditer(r"guarded_compute\((pcache\b[^,]*),", draw):
        if m.group(1) == "pcache_use().cache":
            continue
        owner = max((draw.rfind("\n" + f, 0, m.start()), f) for f in ("bool prepare_run(", "void precompile_run(",
                                                                       "void warm_libraries(", "bool fallback_compute_pipelines(",
                                                                       "VkPipeline clear_pipeline(", "void dispatch("))
        if owner[1] not in allowed_raw:
            raise SystemExit("a compute pipeline is made with the cache past its gate: " + draw[m.start():draw.index("\n", m.start())])
    for line in ("vkCreateGraphicsPipelines(dev.device, pcache_use().cache, 1, &ci, nullptr, &pl);",
                 "si.module ? guarded_compute(pcache_use().cache, &ci, &made[i])"):
        if line not in draw:
            raise SystemExit("not through the gate: " + line)
    if "vkCreateGraphicsPipelines(dev.device, pcache," in draw or "vkCreateComputePipelines(dev.device, pcache," in draw:
        raise SystemExit("a pipeline is made with the cache past its gate")
    pre = between(draw, "void precompile_run(", "\n}\n")
    if "guarded_compute(pcache_use().cache, &ci, &made)" not in pre:
        raise SystemExit("the precompile's computes, which may run on after the game starts, do not take the gate")
    build = between(draw, "VkPrimitiveTopology topo, int part, bool* cold) {", "\n}\n")
    # ((Triforce) cached_graphics: the guard's creation through the play cache, tests/test_pcache_play.py)
    if "const PcacheUse use(pcache_door, wanted);" not in build or build.count("cached_graphics(cache, &ci, &pl)") != 2:
        raise SystemExit("build_graphics does not hold the gate over its creation and its retry")
    if "return vkCreateComputePipelines(dev.device, pcache_use().cache, 1, ci, nullptr, out);" not in \
            between(draw, "VkResult shared_cache_compute(", "\n}\n"):
        raise SystemExit("the other files' compute pipelines are made past the gate")
    for name in ("vk_aa.cpp", "vk_fsr3.cpp"):
        text = (ROOT / "gpu/vk" / name).read_text(encoding="utf-8")
        if "shared_pipeline_cache()" in text or "shared_cache_compute(&ci, " not in text:
            raise SystemExit(name + " makes its pipelines with the cache past the gate")
    save = between(draw, "void pipeline_cache_save(bool force) {", "\n}\n")
    if "const auto use = pcache_use();\n            if (!use.cache) return;" not in save or \
            save.index("pcache_use()") > save.index("vkGetPipelineCacheData("):
        raise SystemExit("the save asks the driver for a cache that may be let go")
    go = between(draw, "void pcache_let_go(uint64_t avail) {", "\n}\n")
    if 'getenv("RX_VK_PCACHE_RAM")' not in go or "asked.exchange(true)" not in go or "std::thread(" not in go:
        raise SystemExit("the cache is let go without its switch, more than once, or on the thread that asked")
    if go.index("pcache_door.close(") > go.index("vkDestroyPipelineCache(dev.device, pcache, nullptr);"):
        raise SystemExit("the cache is destroyed before its gate is closed")
    if "pcache_saved_size.load() < (256ull << 20)" not in go:
        raise SystemExit("a small cache is let go")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="pcache-gate-") as tmp:
        tmp = Path(tmp)
        (tmp / "t.cpp").write_text(FIXTURE, encoding="utf-8")
        exe = tmp / "pcache-gate"
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread", "-I" + str(ROOT / "gpu/vk"),
                        str(tmp / "t.cpp"), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        if r.returncode:
            raise SystemExit(f"pcache_gate.h: {r.stdout.strip() or r.returncode}")
    print("PASS: the pipeline cache is destroyed only once no creation is inside its gate, and none takes it after: " +
          r.stdout.strip())


if __name__ == "__main__":
    main()
