// (Triforce) The pipeline cache let go in play when the PC's RAM runs short (vk_draw.cpp pcache_let_go): the gate every
// pipeline creation with the cache passes, so that the cache is destroyed only once no creation is using it. Apart from
// Vulkan so that tests/test_pcache_gate.py runs it alone, with threads. (11-16: the letting go is off by default,
// RX_VK_PCACHE_RAM=1: in v0.4.0-test9 it gave back nothing the working set showed; the gate stays, passed as ever)
//
// Why: the driver holds the whole cache in RAM for the run, as it was read from mkw_vk_pipelines.bin (2.6 to 2.7 GB on a
// 32 GB PC with the stage libraries in it, v0.4.0-test5 to test7; the lab measured as much more RAM in use at the title
// with a 3.9 GB file than with a fresh one). In play it serves the pipelines built there, and the driver's own cache on
// disk serves most of those too (NVIDIA's: ~460 of ~12,900 precompiled pipelines found in ours). When the PC has under
// 1.5 GB of RAM available, Windows pages, and a frame waits 100-250 ms for it; the cache is then worth less than the RAM.
// Vulkan's rule: a cache may be destroyed once no command that takes it runs (pipelines made with it do not depend on it).
// Every creation that may run in play takes the cache through Gate::Use; Gate::close turns new ones away (they create
// without a cache) and waits for those inside. Sequentially consistent atomics: a Use that entered before close saw no
// creation inside has either left or sees the gate closed.
#pragma once

#include <atomic>
#include <cstdint>

namespace pcache_gate {

class Gate {
public:
    /// In: true, the caller may use the cache until leave(); false, the gate is closed (the caller goes without a cache)
    bool enter() {
        users_.fetch_add(1);
        if (closed_.load()) {
            users_.fetch_sub(1);
            return false;
        }
        return true;
    }
    void leave() { users_.fetch_sub(1); }
    /// Closed from now on, and returns once nobody is inside; wait() is called while somebody is (a sleep). Once only:
    /// false when it was closed already
    template <class Wait>
    bool close(Wait&& wait) {
        if (closed_.exchange(true)) return false;
        while (users_.load()) wait();
        return true;
    }
    bool closed() const { return closed_.load(); }
    int64_t inside() const { return users_.load(); }

private:
    std::atomic<int64_t> users_{0};
    std::atomic<bool> closed_{false};
};

/// The cache for one creation: cache itself while the gate is open, none once it is closed (or when none was asked for).
/// Held for as long as the creation runs: a temporary in the call's full expression does (Use(gate, pcache).cache)
template <class Cache>
struct Use {
    Gate& gate;
    Cache cache;
    bool in;
    Use(Gate& g, Cache want) : gate(g), cache(want), in(want != Cache{} && g.enter()) {
        if (!in) cache = Cache{};
    }
    ~Use() {
        if (in) gate.leave();
    }
    Use(const Use&) = delete;
    Use& operator=(const Use&) = delete;
};

}   // namespace pcache_gate
