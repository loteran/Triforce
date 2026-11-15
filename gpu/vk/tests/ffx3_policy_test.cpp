// Game-free regression for the context policy of AMD FSR 3.1 through AMD's library (gpu/vk/ffx3_policy.h): which context serves a
// frame, which go and when, which slot a new one takes, and when a context let go of may be destroyed.
#include <cstdio>

#include "../ffx3_policy.h"

namespace {
int failures = 0;
void check(const char* name, bool ok) {
    if (ok) return;
    std::fprintf(stderr, "FAIL %s\n", name);
    failures++;
}
namespace p = mkwvk::ffx3_policy;

p::Slot made(const p::Ask& a, uint64_t tick, uint64_t ns = 0) {
    p::Slot s;
    s.live = true, s.out_w = a.ow, s.out_h = a.oh, s.drs = a.drs, s.hdr = a.hdr, s.used = tick, s.used_ns = ns;
    p::made_for(a, &s.max_w, &s.max_h);
    return s;
}
p::Ask ask(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, bool drs = false, bool hdr = false) {
    p::Ask a;
    a.w = w, a.h = h, a.ow = ow, a.oh = oh, a.drs = drs, a.hdr = hdr;
    return a;
}
}  // namespace

int main() {
    // a fixed share: one context per render size and output size
    {
        p::Slot s[p::kSlots];
        const p::Ask a = ask(1280, 720, 1920, 1080);
        check("empty: none serves", p::find(s, a) < 0);
        s[2] = made(a, 1);
        check("fixed: the same size is served", p::find(s, a) == 2);
        check("fixed: another render size is not", p::find(s, ask(1152, 648, 1920, 1080)) < 0);
        check("fixed: another output size is not", p::find(s, ask(1280, 720, 2560, 1440)) < 0);
        check("fixed: the other colour is not", p::find(s, ask(1280, 720, 1920, 1080, false, true)) < 0);
        check("fixed: not the dynamic kind", p::find(s, ask(1280, 720, 1920, 1080, true)) < 0);
    }
    // dynamic resolution: one context takes every size up to the output
    {
        p::Slot s[p::kSlots];
        const p::Ask a = ask(1000, 560, 1920, 1080, true);
        uint32_t mw = 0, mh = 0;
        p::made_for(a, &mw, &mh);
        check("drs: made for the output's size", mw == 1920 && mh == 1080);
        s[0] = made(a, 1);
        check("drs: a smaller size is served", p::find(s, ask(1280, 720, 1920, 1080, true)) == 0);
        check("drs: the largest is served", p::find(s, ask(1920, 1080, 1920, 1080, true)) == 0);
        check("drs: a scene past the output is not", p::find(s, ask(2000, 1100, 1920, 1080, true)) < 0);
        p::made_for(ask(2000, 1100, 1920, 1080, true), &mw, &mh);
        check("drs: a scene past the output makes it larger", mw == 2000 && mh == 1100);
    }
    // a render size changes: the old context of the output size goes once it is not recent
    {
        p::Slot s[p::kSlots];
        const p::Ask old_a = ask(1280, 720, 1920, 1080), new_a = ask(1152, 648, 1920, 1080);
        s[0] = made(old_a, 100);
        s[1] = made(ask(640, 360, 1280, 720), 100);   // (another output size: not touched)
        check("stale: recent ones stay", p::stale_for_size(s, new_a, -1, 104) == 0);
        check("stale: a context that is not recent goes", p::stale_for_size(s, new_a, -1, 100 + p::kRecent) == 1u);
        check("stale: not the one that serves", p::stale_for_size(s, old_a, 0, 100 + p::kRecent) == 0);
    }
    // the slot a new context takes
    {
        p::Slot s[p::kSlots];
        bool evict = true;
        check("slot: a free one first", p::pick_slot(s, 10, &evict) == 0 && !evict);
        for (int i = 0; i < p::kSlots; i++) s[i] = made(ask(100 + i, 100, 200, 200), 10 + i);
        check("slot: all recent: none", p::pick_slot(s, 13, &evict) < 0 && !evict);
        check("slot: the least recently used, evicted", p::pick_slot(s, 10 + p::kRecent + 2, &evict) == 0 && evict);
        s[0].used = 30;
        check("slot: the oldest of those not recent", p::pick_slot(s, 10 + p::kRecent + 3, &evict) == 1 && evict);
    }
    // idle contexts
    {
        p::Slot s[p::kSlots];
        s[0] = made(ask(1, 1, 2, 2), 1, 1000);
        s[1] = made(ask(1, 1, 3, 3), 1, 1000 + p::kIdleNs);
        check("idle: used a moment ago stays", p::idle_mask(s, 1000 + p::kIdleNs) == 0);
        check("idle: 90 s unused goes", p::idle_mask(s, 1001 + p::kIdleNs) == 1u);
        check("idle: both after a while", p::idle_mask(s, 2000 + p::kIdleNs + p::kIdleNs) == 3u);
    }
    // destruction only once the GPU is past the last use
    check("gpu: not yet", !p::gpu_past(10, 9));
    check("gpu: just past", p::gpu_past(10, 10));
    check("gpu: never used", p::gpu_past(0, 0));
    if (!failures) std::printf("ffx3_policy_test: ok\n");
    return failures ? 1 : 0;
}
