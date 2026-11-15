// Game-free regression for the camera-cut ring of AMD FSR 3.1 frame generation (gpu/vk/vk_fg_reset.h): a cut noted under a frame id
// is what the generation callback finds for that id, and an id the ring does not hold counts as a cut.
#include <cstdio>

#include "../vk_fg_reset.h"

namespace {
int failures = 0;
void check(const char* name, bool ok) {
    if (ok) return;
    std::fprintf(stderr, "FAIL %s\n", name);
    failures++;
}
namespace r = mkwvk::fg_reset;
}  // namespace

int main() {
    r::Ring ring;
    check("never noted: a cut", r::is_cut(ring, 1));
    check("id 0: a cut", r::is_cut(ring, 0));
    r::note(ring, 5, false);
    check("noted without a cut", !r::is_cut(ring, 5));
    r::note(ring, 6, true);
    check("noted as a cut", r::is_cut(ring, 6));
    check("the frame before is kept", !r::is_cut(ring, 5));
    // the slot of 5 is reused 32 frames on: the old id no longer matches, and an id the ring lost counts as a cut
    r::note(ring, 5 + r::kSlots, false);
    check("a new id in the slot", !r::is_cut(ring, 5 + r::kSlots));
    check("the old id of that slot is a cut", r::is_cut(ring, 5));
    check("an id never noted in a used slot is a cut", r::is_cut(ring, 5 + 2 * r::kSlots));
    // ids past what fits beside the cut bit are never kept and always cut
    r::note(ring, r::kMaxId + 1, false);
    check("past the maximum: a cut", r::is_cut(ring, r::kMaxId + 1));
    r::note(ring, r::kMaxId, false);
    check("the maximum is kept", !r::is_cut(ring, r::kMaxId));
    r::note(ring, 0, false);
    check("id 0 stays a cut", r::is_cut(ring, 0));
    // a run of frames: every id of the last kSlots is found, the ones before are not
    r::Ring run;
    for (uint64_t id = 1; id <= 100; id++) r::note(run, id, id % 10 == 0);
    bool ok = true;
    for (uint64_t id = 100 - r::kSlots + 1; id <= 100; id++) ok &= r::is_cut(run, id) == (id % 10 == 0);
    check("the last kSlots frames answer", ok);
    check("older frames are cuts", r::is_cut(run, 100 - r::kSlots) && r::is_cut(run, 1));
    if (failures) return 1;
    std::puts("fg_reset_test: all passed");
    return 0;
}
