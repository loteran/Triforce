// Fixture of tests/test_present_hold.py: the new textures' budget of gpu/vk/vk_texture.cpp, cut from the source as it is
// (new_texture_over_budget, new_texture_later, new_texture_waits and the ASTC rule upload_defer), run against stand-ins for the
// frame's upload counters, the hold's mode and the texture. Unset RX_VK_UPLOAD_* switches: their defaults.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "present_hold.h"

struct Texture {
    bool uploaded = false, astc = false, gpu_written = false;
};
static std::atomic<int64_t> frame_upload_ns{0}, frame_upload_bytes{0};
static std::atomic<int> frame_astc_admitted{0};
static std::atomic<int64_t> st_deferred{0}, st_deferred_frame{0};
static thread_local bool upload_defer_ok = false;
static std::atomic<int> hold_mode{present_hold::AS_BEFORE};
static std::atomic<int64_t> frame_owed{0}, st_new_later{0}, st_new_later_frame{0};

/* CUT */

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s: failed at line %d: %s\n", name.c_str(), __LINE__, #x); return 1; } } while (0)

static void next_frame() {
    frame_upload_ns = 0, frame_upload_bytes = 0, frame_astc_admitted = 0, frame_owed = 0;
}

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "";
    constexpr int64_t ms = 1000000, mb = 1 << 20;
    Texture plain, astc;
    astc.astc = true;
    upload_defer_ok = true;
    if (name == "budget") {
        // (the hold may leave textures: a new one is left once the frame's uploads took 12 ms or 64 MB, not before)
        hold_mode = present_hold::DEFERS;
        CHECK(!new_texture_later(plain));   // (the frame's first upload always goes)
        frame_upload_ns = 11 * ms, frame_upload_bytes = 63 * mb;
        CHECK(!new_texture_later(plain) && frame_owed == 0);
        frame_upload_ns = 12 * ms;
        CHECK(new_texture_later(plain) && frame_owed == 1 && st_new_later == 1 && st_new_later_frame == 1);
        frame_upload_ns = 1 * ms, frame_upload_bytes = 64 * mb;   // (by bytes alone: the GPU's backlog)
        CHECK(new_texture_later(plain) && frame_owed == 2);
        CHECK(new_texture_waits() && frame_owed == 3);
        // (a dispatch's look: never)
        upload_defer_ok = false;
        CHECK(!new_texture_later(plain) && !new_texture_waits() && frame_owed == 3);
        upload_defer_ok = true;
        // (no hold, or resting, or the hold's bound: never)
        for (int m : {present_hold::AS_BEFORE, present_hold::FLUSH}) {
            hold_mode = m;
            CHECK(!new_texture_later(plain) && !new_texture_waits());
        }
        CHECK(frame_owed == 3 && st_new_later == 2);
    } else if (name == "astc-hold") {
        // a new ASTC texture while the hold may leave textures: the new textures' budget too (its first 24 not kept from it),
        // counted as owed; at the hold's bound: never left
        hold_mode = present_hold::DEFERS;
        CHECK(!upload_defer(astc));
        frame_upload_bytes = 64 * mb;
        CHECK(upload_defer(astc) && frame_owed == 1 && st_deferred == 1);
        hold_mode = present_hold::FLUSH;
        frame_upload_ns = 500 * ms;
        for (int i = 0; i < 100; i++) CHECK(!upload_defer(astc));
        CHECK(frame_owed == 1);
        // (not a texture of the ASTC rule: uploaded, drawn into by the GPU, or not ASTC)
        hold_mode = present_hold::DEFERS;
        Texture up = astc, drawn = astc;
        up.uploaded = true, drawn.gpu_written = true;
        CHECK(!upload_defer(up) && !upload_defer(drawn) && !upload_defer(plain));
    } else if (name == "astc-as-before") {
        // no hold (RX_VK_HOLD_MS=0, or resting): the ASTC rule as before, its 40 ms after the frame's first 24, the frame
        // counted as owed (the placeholder drawn) for the hold's rule, which shows it then
        hold_mode = present_hold::AS_BEFORE;
        frame_upload_ns = 39 * ms, frame_upload_bytes = 1000 * mb;
        for (int i = 0; i < 30; i++) CHECK(!upload_defer(astc));
        frame_upload_ns = 40 * ms;
        CHECK(upload_defer(astc) && frame_owed == 1);
        next_frame();
        frame_upload_ns = 40 * ms;
        for (int i = 0; i < 24; i++) CHECK(!upload_defer(astc));   // (the frame's first 24 go)
        CHECK(upload_defer(astc));
        // (a plain new texture is never left without the hold)
        CHECK(!new_texture_later(plain));
    } else {
        std::fprintf(stderr, "unknown case %s\n", name.c_str());
        return 2;
    }
    return 0;
}
