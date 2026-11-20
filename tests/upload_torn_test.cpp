// (Triforce, wp18-flash) tests/test_upload_torn.py's fixture: vk_memory.cpp memory_rewritten_since_capture and vk_texture.cpp's
// upload_again_later with torn_later, cut from the source, run against a model of the game's memory, a capture's copies of its
// pages (the shadow) and a texture's image. No game, no GPU.
//
// The model of a frame: the game writes texture storage; its submit's capture copies the pages written since the last one (as
// vk_memory.cpp take_capture does into the shadow); the CPU may go on writing while the replay runs; the draw's look asks
// upload_again_later, and either the image keeps its bytes (the upload owed to the next frame, as texture_touch does) or the
// upload reads the storage as submitted (each page's copy where the shadow holds one, guest memory for the rest, as
// memory_as_submitted gives it) into the image. The frame is drawn with the image.
//
// Bytes: 0x11 the old picture, 0x22 the new one, 0xEE what passes through the storage while the game moves or refills a texture
// (another texture's bytes: BC6H decodes them to bright colours, test29's white frames). An image holding any 0xEE is a wrong frame.
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "upload_owed.h"
#include "upload_torn.h"

namespace mkwvk {

// ---- stand-ins for what the cut functions use ----
void logf(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vprintf(fmt, a);
    va_end(a);
    std::printf("\n");
}
constexpr uint64_t kBase = 0x10000000ull;   // (the model's guest memory: 32 MB from here)
std::vector<uint8_t> mem(32u << 20, 0);
struct FakeHost {
    const uint8_t* gpu_ptr(uint64_t gpu, uint64_t* avail) const {
        if (gpu < kBase || gpu >= kBase + mem.size()) return nullptr;
        *avail = kBase + mem.size() - gpu;
        return mem.data() + (gpu - kBase);
    }
};
FakeHost fake_host;
const FakeHost* host = &fake_host;
struct ShadowPage {
    std::unique_ptr<uint8_t[]> b;
    uint64_t seq = 0;
};
std::mutex shadow_mu;
std::unordered_map<uint32_t, ShadowPage> shadow;
std::unordered_map<uint32_t, uint32_t> shadow_regions;
std::atomic<size_t> shadow_n{0};
bool overlay_off() { return false; }

thread_local bool upload_defer_ok = false;
std::atomic<int64_t> frame_upload_ns{0};
uint64_t frame_no = 1;
struct Texture {
    uint32_t id = 0;
    uint64_t gpu = 0, guest_size = 0;
    upload_owed::Owed owed;
};

/* CUT MEMORY */
/* CUT TEXTURE */

}  // namespace mkwvk

using namespace mkwvk;

namespace {

int failures = 0;
void expect(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        failures++;
    }
}

uint8_t* at(uint64_t guest) { return mem.data() + (guest - kBase); }
void fill(uint64_t guest, uint64_t n, uint8_t v) { std::memset(at(guest), v, n); }

/// A capture at a submit: the pages written since the last one, copied into the shadow (vk_memory.cpp take_capture)
void capture(const std::vector<uint64_t>& pages) {
    std::lock_guard lk(shadow_mu);
    for (uint64_t p : pages) {
        ShadowPage& s = shadow[(uint32_t)p];
        if (!s.b) {
            s.b.reset(new uint8_t[4096]);
            shadow_regions[(uint32_t)(p >> 9)]++;
            shadow_n++;
        }
        std::memcpy(s.b.get(), at(p * 4096), 4096);
    }
}
std::vector<uint64_t> pages_of(uint64_t guest, uint64_t n) {
    std::vector<uint64_t> v;
    for (uint64_t p = guest / 4096; p < (guest + n + 4095) / 4096; p++) v.push_back(p);
    return v;
}
void shadow_clear() {
    std::lock_guard lk(shadow_mu);
    shadow.clear(), shadow_regions.clear(), shadow_n = 0;
}

/// A texture's image and its looks
struct Model {
    Texture tex;
    std::vector<uint8_t> image;
    int left = 0, uploads = 0;
    Model(uint32_t id, uint64_t gpu, uint64_t size) : image(size, 0) { tex.id = id, tex.gpu = gpu, tex.guest_size = size; }
    /// The storage as submitted: each page's copy where the shadow holds one, guest memory for the rest (memory_as_submitted)
    void upload() {
        std::lock_guard lk(shadow_mu);
        for (uint64_t off = 0; off < tex.guest_size;) {
            const uint64_t g = tex.gpu + off, p = g / 4096, page_end = (p + 1) * 4096;
            const uint64_t n = std::min(page_end, tex.gpu + tex.guest_size) - g;
            const auto it = shadow.find((uint32_t)p);
            const uint8_t* src = it != shadow.end() ? it->second.b.get() + (g - p * 4096) : at(g);
            std::memcpy(image.data() + off, src, n);
            off += n;
        }
    }
    /// A draw's look at present `frame` (texture_touch's re-upload branch): left for the next frame, or uploaded
    void look(uint64_t frame, bool draw = true) {
        frame_no = frame;
        upload_defer_ok = draw;
        const bool later = upload_again_later(tex);
        upload_defer_ok = false;
        if (later) {
            upload_owed::later(tex.owed, nullptr, frame);
            left++;
        } else {
            upload();
            upload_owed::paid(tex.owed);
            uploads++;
        }
    }
    bool holds(uint8_t v) const {
        for (uint8_t b : image)
            if (b == v) return true;
        return false;
    }
    bool all(uint8_t v) const {
        for (uint8_t b : image)
            if (b != v) return false;
        return true;
    }
};

/// The game refills a texture's storage at the Market: its first upload holds the old picture; at the burst frame's submit the
/// refill is part done (new bytes, then a stretch of passing bytes, then the old), and goes on while the replay runs; the next
/// frame's submit finds it done
void refill(const char* name, uint32_t id, uint64_t gpu, uint64_t size, bool same_picture, bool expect_left) {
    shadow_clear();
    const uint8_t old_b = 0x11, new_b = same_picture ? 0x11 : 0x22;
    fill(gpu, size, old_b);
    Model m(id, gpu, size);
    capture(pages_of(gpu, size));
    m.look(100);   // (held before: the old picture)
    expect(m.all(old_b), std::string(name) + ": the first upload holds the old picture");
    // (the burst frame: half refilled, a quarter of it passing bytes, the rest as it was; the pages written are captured)
    const uint64_t a = size / 2, b = size / 2 + size / 4;
    fill(gpu, a, new_b);
    fill(gpu + a, b - a, 0xEE);
    capture(pages_of(gpu, b));
    fill(gpu + a, size - a, new_b);   // (the CPU goes on while the replay runs)
    m.look(101);
    if (expect_left) {
        expect(m.left == 1, std::string(name) + ": the burst frame's re-upload is left for the next frame");
        expect(!m.holds(0xEE), std::string(name) + ": the burst frame is drawn with bytes that passed through (the white frame)");
        expect(m.all(old_b), std::string(name) + ": the burst frame is drawn with the image's last picture");
    } else {
        expect(m.left == 0, std::string(name) + ": the re-upload is left although the rule does not apply");
    }
    // (the next frame: its capture takes the rest; the CPU is done)
    capture(pages_of(gpu + a, size - a));
    m.look(102);
    expect(m.all(new_b), std::string(name) + ": the frame after the burst holds the new picture whole");
    expect(m.tex.owed.later == 0, std::string(name) + ": nothing owed after the next frame's upload");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "off") {   // (RX_VK_UPLOAD_TORN=0: the burst frame is uploaded at once, as before: its picture holds the passing bytes)
        shadow_clear();
        const uint64_t gpu = kBase + (1u << 20), size = 4u << 20;
        fill(gpu, size, 0x11);
        Model m(7, gpu, size);
        capture(pages_of(gpu, size));
        m.look(100);
        fill(gpu, size / 2, 0x22);
        fill(gpu + size / 2, size / 4, 0xEE);
        capture(pages_of(gpu, size / 2 + size / 4));
        fill(gpu + size / 2, size / 2, 0x22);
        m.look(101);
        expect(m.left == 0, "off: a re-upload left with RX_VK_UPLOAD_TORN=0");
        expect(m.holds(0xEE), "off: the model no longer makes the wrong frame it guards against");
        std::printf(failures ? "off: FAILED\n" : "off: as before, the burst frame uploaded at once (and wrong)\n");
        return failures ? 1 : 0;
    }

    // 1. test29's case: a 4 MB texture refilled with another picture, and one refilled with the same bytes (the storm's)
    refill("refill", 1, kBase + (1u << 20), 4u << 20, false, true);
    refill("same bytes", 2, kBase + (8u << 20), 4u << 20, true, true);
    // (a texture not page-aligned: its whole pages are compared)
    refill("unaligned", 3, kBase + (16u << 20) + 0x800, (3u << 20) + 0x1000, false, true);
    // (under 1 MB: as before)
    refill("small", 4, kBase + (24u << 20), 512u << 10, false, false);

    {   // 2. a dispatch's look (no leave to defer) uploads at once
        shadow_clear();
        const uint64_t gpu = kBase + (1u << 20), size = 2u << 20;
        fill(gpu, size, 0x11);
        Model m(9, gpu, size);
        capture(pages_of(gpu, size));
        m.look(100);
        fill(gpu, size, 0x22);
        capture(pages_of(gpu, size / 2));
        m.look(101, false);
        expect(m.left == 0, "dispatch: a dispatch's re-upload left for the next frame");
    }
    {   // 3. one the CPU rewrites at every frame, half before its submit and half while the replay runs (a movie): a frame late once
        shadow_clear();
        const uint64_t gpu = kBase + (2u << 20), size = 2u << 20;
        fill(gpu, size, 0x01);
        Model m(11, gpu, size);
        capture(pages_of(gpu, size));
        m.look(100);
        for (uint64_t f = 101; f < 141; f++) {
            const uint8_t v = (uint8_t)(0x20 + f % 64);
            fill(gpu, size / 2, v);
            capture(pages_of(gpu, size));   // (the first half written before the submit, the rest the frame before's)
            fill(gpu + size / 2, size / 2, v);   // (the rest after the submit)
            m.look(f);
        }
        expect(m.left <= 1, "every frame: a texture the CPU rewrites at every frame left " + std::to_string(m.left) +
                                " times in 40 frames (once at most)");
    }
    {   // 4. a burst whose refill goes on at the next frame too: left once, uploaded at the next frame whatever it holds
        shadow_clear();
        const uint64_t gpu = kBase + (4u << 20), size = 2u << 20;
        fill(gpu, size, 0x11);
        Model m(13, gpu, size);
        capture(pages_of(gpu, size));
        m.look(100);
        fill(gpu, size, 0x22);
        capture(pages_of(gpu, size / 2));
        fill(gpu, size, 0x33);
        m.look(101);
        capture(pages_of(gpu, size));
        fill(gpu, size, 0x44);   // (still writing at the next frame's replay)
        m.look(102);
        expect(m.left == 1, "bound: left " + std::to_string(m.left) + " times in a row (once at most)");
        expect(m.uploads == 2, "bound: the next frame's look did not upload");
        // (and a later burst is left again: the Market's refills of a texture came every 4 to 5 frames)
        capture(pages_of(gpu, size));
        m.look(106);
        fill(gpu, size, 0x55);
        capture(pages_of(gpu, size / 2));
        fill(gpu, size, 0x66);
        m.look(107);
        expect(m.left == 2, "bound: a later burst, six presents after the last one found so, is not left");
    }
    {   // 5. the pages a texture shares with other data at its start and its end: the CPU writing there is no sign
        shadow_clear();
        const uint64_t gpu = kBase + (6u << 20) + 0x800, size = (2u << 20) - 0x1000;
        fill(gpu - 0x800, size + 0x1000, 0x11);
        Model m(15, gpu, size);
        capture(pages_of(gpu, size));
        m.look(100);
        for (uint64_t f = 101; f < 105; f++) {
            fill(gpu, 0x100, (uint8_t)f);   // (a texel row in the first page changes: an upload is due)
            capture(pages_of(gpu, 0x100));
            fill(gpu - 0x800, 0x800, (uint8_t)(f + 1));   // (the neighbour's bytes in the first page, after the capture)
            fill(gpu + size, 0x800, (uint8_t)(f + 1));    // (and in the last)
            m.look(f);
        }
        expect(m.left == 0, "edges: a texture left for the next frame for writes to pages it only shares");
    }
    {   // 7. a refill whose last pages were still passing bytes at the capture (only the last whole page is on no stride)
        shadow_clear();
        const uint64_t gpu = kBase + (12u << 20), size = 2u << 20;
        fill(gpu, size, 0x11);
        Model m(19, gpu, size);
        capture(pages_of(gpu, size));
        m.look(100);
        fill(gpu, size - 0x2000, 0x22);
        fill(gpu + size - 0x2000, 0x2000, 0xEE);
        capture(pages_of(gpu, size));
        fill(gpu + size - 0x2000, 0x2000, 0x22);   // (its last two pages written while the replay runs)
        m.look(101);
        expect(m.left == 1 && !m.holds(0xEE), "tail: a refill caught in its last two pages is drawn with the bytes passing through");
        capture(pages_of(gpu + size - 0x2000, 0x2000));
        m.look(102);
        expect(m.all(0x22), "tail: the frame after holds the new picture whole");
    }
    {   // 8. one the frame's budget left the frame before (RX_VK_UPLOAD_AGAIN_MS): uploaded now, not left once more
        shadow_clear();
        const uint64_t gpu = kBase + (14u << 20), size = 2u << 20;
        fill(gpu, size, 0x11);
        Model m(21, gpu, size);
        capture(pages_of(gpu, size));
        m.look(100);
        upload_owed::later(m.tex.owed, nullptr, 100);   // (as upload_again_later's budget left it)
        fill(gpu, size / 2, 0x22);
        capture(pages_of(gpu, size / 2));
        fill(gpu + size / 2, size / 2, 0x22);
        m.look(101);
        expect(m.left == 0, "budget: a re-upload left by the budget the frame before is left again");
    }
    {   // 6. pages no capture holds (guest memory as it is for the upload anyway): nothing to compare
        shadow_clear();
        const uint64_t gpu = kBase + (10u << 20), size = 2u << 20;
        fill(gpu, size, 0x11);
        Model m(17, gpu, size);
        m.look(100);
        fill(gpu, size, 0x22);
        m.look(101);
        expect(m.left == 0, "no copy: a texture left whose pages no capture holds");
    }
    std::printf(failures ? "upload_torn_test: %d FAILED\n" : "upload_torn_test: all passed\n", failures);
    return failures ? 1 : 0;
}
