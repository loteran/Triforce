// Fixture of tests/test_upload_owed.py: gpu/vk/upload_owed.h as it is, and the two parts of gpu/vk/vk_texture.cpp's texture_touch
// that decide what a failed upload leaves behind, cut from the source (HEAD: from the wait to the dirty ranges; TAIL: the
// failed upload's branches to the function's end), around an upload of the test's own that succeeds, finds no staging memory
// or is of an image no upload fills. The write tracking, the GPU's buffer writes and the eviction's notes are stand-ins that
// tell what they have once, as the real ones do.
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "upload_owed.h"
#include "write_gen.h"   // (a page write generation, 64 bits: the cut parts hold them as WriteGen)

using Ranges = upload_owed::Ranges;
using mkwvk::WriteGen;
static WriteGen G(uint64_t v) { return WriteGen{v}; }

struct Texture {
    bool uploaded = false, gpu_written = false, released = false, bc = false;
    WriteGen upload_gen, release_gen;
    uint32_t checked_epoch = 0;
    uint64_t bc_retry_frame = 0, gpu_wrote_seen = 0, last_written = 0;
    uint64_t gpu = 0x1040000000ull, guest_size = 0x40000;
    uint32_t id = 7, width = 256, height = 256, layers = 1;
    int nvn_format = 0x25;
    upload_owed::Owed owed;
};

static uint64_t frame_no = 1000;
static WriteGen generation{10};
static WriteGen memory_generation() { return generation; }
static thread_local bool upload_starved = false;
static thread_local const char* astc_fail = nullptr;
static int logs;
static std::string last_log;
static void logf(const char* f, ...) {
    logs++;
    last_log = f;
}

// ---- the eviction's note of a texture that came from memory (vk_texture.cpp evicted_from_memory)
static std::mutex from_memory_mu;
static std::unordered_map<uint64_t, uint64_t> evicted_from_memory;
static uint64_t evicted_key(uint64_t gpu, uint32_t w, uint32_t h, uint32_t fmt) {
    return gpu ^ ((uint64_t)w << 48) ^ ((uint64_t)h << 32) ^ ((uint64_t)fmt << 20);
}
static bool note_taken;   // (texture_touch found the note and took it, for this look)

// ---- what changed the storage: each told once, or while it is newer than the generation asked about
static Ranges remapped;                 // (a virtual texture's pages mapped anew: texture_dirty tells them once)
static Ranges written;                  // (pages the CPU wrote, at written_gen: told while that is past the generation asked)
static WriteGen written_gen;
static uint64_t gpu_write;              // (a GPU buffer's write into the storage, by its submission: told while not seen)
static int asked_dirty, asked_gpu;
static bool texture_dirty(Texture&, WriteGen gen, Ranges& out, bool = false) {
    asked_dirty++;
    out = remapped;
    remapped.clear();
    if (written_gen > gen) upload_owed::add(out, written);
    return !out.empty();
}
static bool gpu_wrote_into(Texture& tex) {
    asked_gpu++;
    if (gpu_write <= tex.gpu_wrote_seen) return false;
    tex.gpu_wrote_seen = gpu_write;
    return true;
}

// ---- the upload: what it was asked for, and how it ends
enum Ends { DONE, STARVED, NEVER };
static Ends upload_ends = DONE;
static int uploads;
static bool up_whole;
static Ranges up_ranges;
static bool upload(Texture&, const Ranges* dirty) {
    uploads++;
    upload_starved = false;
    up_whole = dirty == nullptr;
    up_ranges = dirty ? *dirty : Ranges{};
    if (upload_ends == STARVED) upload_starved = true;
    return upload_ends == DONE;
}

// ---- (Triforce) the frame's uploads past their budget (vk_texture.cpp upload_again_later): the test says when, the rule of
// frames in a row is the real one's (11-16: RX_VK_UPLOAD_AGAIN_FRAMES's 2, may_wait_again)
static bool budget_taken = false;
static constexpr uint32_t kAgainFrames = 2;
static bool upload_again_later(const Texture& tex) { return budget_taken && upload_owed::may_wait_again(tex.owed, kAgainFrames); }

// ---- (Triforce) the hold of the last picture (vk_texture.cpp new_texture_later, new_texture_waits): the test says whether the
// frame's budget is taken and whether the look may leave a texture (a draw's while the hold may: not a dispatch's, nor at the
// hold's bound), and counts the looks that left a texture owed its first bytes
static bool new_budget_taken = false, may_leave = true;
static int owed_looks;
static bool new_texture_later(const Texture&) {
    if (!may_leave || !new_budget_taken) return false;
    owed_looks++;
    return true;
}
static bool new_texture_waits() {
    if (!may_leave) return false;
    owed_looks++;
    return true;
}
// (Triforce, layer_split.h: a layered texture whose first upload goes in parts. The test says whether this one's parts are under
// way; the parts themselves are after the cut, in texture_touch's own upload, tests/test_layer_split.py)
static bool split_on = false;
static bool split_pending(const Texture&) { return split_on; }

static Texture* touch(Texture* tex) {
/* HEAD */
    const WriteGen gen = memory_generation();
    const bool sparse_written = false;
    const bool from_memory = note_taken;
    const bool up_ok = upload(*tex, partial || sparse_written ? &dirty : nullptr);
    if (up_ok) {
        tex->uploaded = true;
        tex->upload_gen = gen;
        tex->gpu_written = false;
        tex->released = false;
        upload_owed::paid(tex->owed);
/* TAIL */

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s: failed at line %d: %s\n", name.c_str(), __LINE__, #x); return 1; } } while (0)

static bool is(const Ranges& r, std::initializer_list<std::pair<uint64_t, uint64_t>> want) { return r == Ranges(want); }

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "";
    Texture t;
    if (name == "rules") {   // upload_owed.h alone
        using namespace upload_owed;
        CHECK(wait(0) == 30 && wait(1) == 30 && wait(2) == 60 && wait(3) == 120 && wait(4) == 240 && wait(5) == 480);
        CHECK(wait(6) == 480 && wait(1000000) == 480);
        Ranges r{{0x3000, 0x4000}, {0x1000, 0x2000}};
        add(r, {{0x2000, 0x2800}, {0x8000, 0x8000}, {0x3800, 0x5000}, {0x9000, 0xa000}});
        CHECK(is(r, {{0x1000, 0x2800}, {0x3000, 0x5000}, {0x9000, 0xa000}}));   // (in order, touching ones one, the empty one gone)
        add(r, r);
        CHECK(is(r, {{0x1000, 0x2800}, {0x3000, 0x5000}, {0x9000, 0xa000}}));
        add(r, {});
        CHECK(r.size() == 3);
        Owed o;
        CHECK(!o.any() && !waits(o, 0));
        const Ranges a{{0x1000, 0x2000}}, b{{0x1800, 0x3000}};
        failed(o, &a, 500);
        CHECK(o.any() && !o.whole && o.failed == 1 && o.from == 530 && waits(o, 529) && !waits(o, 530));
        failed(o, &b, 530);
        CHECK(is(o.ranges, {{0x1000, 0x3000}}) && o.failed == 2 && o.from == 590);
        failed(o, nullptr, 590);   // (the whole storage: no ranges to keep beside it)
        CHECK(o.whole && o.ranges.empty() && o.from == 710);
        failed(o, &a, 710);        // (and a part after it does not make it less)
        CHECK(o.whole && o.ranges.empty() && o.from == 950);
        failed(o, &a, 950, 30);    // (a fixed wait: a BC3 image's)
        CHECK(o.from == 980 && o.failed == 5);
        paid(o);
        CHECK(!o.any() && !o.failed && !o.from && !waits(o, 0));
    } else if (name == "later-rules") {   // (Triforce) upload_owed::later alone
        using namespace upload_owed;
        Owed o;
        const Ranges a{{0x1000, 0x2000}}, b{{0x1800, 0x3000}};
        later(o, &a, 500);
        CHECK(o.any() && o.later == 1 && o.failed == 0 && o.from == 501 && waits(o, 500) && !waits(o, 501));
        later(o, &b, 501);
        CHECK(is(o.ranges, {{0x1000, 0x3000}}) && o.later == 2 && o.from == 502);
        for (uint32_t i = 2; i < kLaterMax; i++) {
            CHECK(may_wait(o));
            later(o, nullptr, 502 + i);
        }
        CHECK(!may_wait(o) && o.whole && o.ranges.empty() && o.later == kLaterMax);
        paid(o);
        CHECK(!o.any() && !o.later && may_wait(o));
    } else if (name == "again-rules") {   // (Triforce, 11-16) upload_owed::may_wait_again alone
        using namespace upload_owed;
        Owed o;
        CHECK(may_wait_again(o, 2) && !may_wait_again(o, 0));
        later(o, nullptr, 10);
        CHECK(may_wait_again(o, 2) && !may_wait_again(o, 1));
        later(o, nullptr, 11);
        CHECK(!may_wait_again(o, 2) && may_wait_again(o, 8) && may_wait(o));   // (8: as before, kLaterMax's rule)
        for (uint32_t i = 2; i < kLaterMax; i++) later(o, nullptr, 10 + i);
        CHECK(!may_wait_again(o, 100) && !may_wait(o));   // (never past kLaterMax)
        paid(o);
        CHECK(may_wait_again(o, 1));
    } else if (name == "left-for-later") {
        // (Triforce) an uploaded texture the CPU wrote a part of, met after the frame's uploads took their budget: not uploaded
        // now, its generation and its last bytes kept, owed to the next present with no failure counted and nothing logged
        t.uploaded = true, t.upload_gen = G(10);
        written = {{0x4000, 0x6000}}, written_gen = G(28), generation = G(28);
        budget_taken = true;
        CHECK(touch(&t) == &t && uploads == 0 && t.uploaded && t.upload_gen == G(10));
        CHECK(is(t.owed.ranges, {{0x4000, 0x6000}}) && t.owed.from == frame_no + 1 && t.owed.later == 1 && t.owed.failed == 0);
        CHECK(logs == 0);
        CHECK(touch(&t) == &t && uploads == 0);   // (the same present: nothing more)
        // (the next present's budget free: uploaded, what was owed with it)
        frame_no++, generation = G(30);
        budget_taken = false;
        CHECK(touch(&t) == &t && uploads == 1 && !up_whole && is(up_ranges, {{0x4000, 0x6000}}));
        CHECK(t.upload_gen == G(30) && !t.owed.any() && !t.owed.later);
        // (a budget taken every frame: left at most two frames in a row, then made all the same)
        written = {{0x8000, 0x9000}}, written_gen = G(31), generation = G(31);
        budget_taken = true;
        for (uint32_t i = 0; i < kAgainFrames; i++) {
            frame_no++;
            CHECK(touch(&t) == &t && uploads == 1 && t.owed.later == i + 1);
        }
        frame_no++;
        CHECK(touch(&t) == &t && uploads == 2 && is(up_ranges, {{0x8000, 0x9000}}) && !t.owed.any());
        // (a texture seen for the first time is never left so: its image holds nothing yet)
        Texture n;
        CHECK(touch(&n) == &n && uploads == 3 && n.uploaded);
    } else if (name == "left-for-later-gpu-write") {
        // (Triforce) a GPU buffer's write into the storage, noted as seen when asked: not seen again when its upload is left for
        // the next frame, which then uploads it whole
        t.uploaded = true, t.upload_gen = G(10), t.gpu_wrote_seen = 3;
        gpu_write = 5;
        budget_taken = true;
        CHECK(touch(&t) == &t && uploads == 0 && t.gpu_wrote_seen == 3 && t.owed.whole && t.owed.later == 1);
        frame_no++;
        budget_taken = false;
        CHECK(touch(&t) == &t && uploads == 1 && up_whole && t.gpu_wrote_seen == 5 && !t.owed.any());
    } else if (name == "first-rules") {   // (Triforce) upload_owed::may_leave_first and first_later alone
        using namespace upload_owed;
        Owed o;
        CHECK(may_leave_first(o, false) && !may_leave_first(o, true) && !first_later(o, false));
        later(o, nullptr, 500);
        CHECK(first_later(o, false) && !first_later(o, true) && may_leave_first(o, false));
        failed(o, nullptr, 501);   // (no memory for it: its own wait, not the budget's)
        CHECK(!first_later(o, false) && !may_leave_first(o, false));
        paid(o);
        for (uint32_t i = 0; i < kLaterMax; i++) later(o, nullptr, 600 + i);
        CHECK(!may_leave_first(o, false) && first_later(o, false));
    } else if (name == "split-goes-on") {
        // (Triforce) a layered texture whose first upload is in parts is not left whole by the frame's budget: its next part goes
        new_budget_taken = true, split_on = true;
        CHECK(touch(&t) == &t && uploads == 1 && owed_looks == 0 && !t.owed.whole);
    } else if (name == "new-left-for-later") {
        // (Triforce) a new texture a draw samples after the frame's uploads took their budget: no upload, the dummy (null), its
        // whole first upload owed to the next present, no failure counted, nothing logged, nothing asked
        new_budget_taken = true;
        CHECK(touch(&t) == nullptr && uploads == 0 && !t.uploaded && t.owed.whole && t.owed.later == 1 && !t.owed.failed);
        CHECK(t.owed.from == frame_no + 1 && owed_looks == 1 && logs == 0 && asked_dirty == 0 && asked_gpu == 0);
        // (the frame's later draws: the dummy again, counted, nothing asked or tried)
        t.checked_epoch++;
        CHECK(touch(&t) == nullptr && uploads == 0 && owed_looks == 2 && t.owed.later == 1 && asked_dirty == 0);
        // (the next present, its budget free: uploaded whole, nothing owed)
        frame_no++, generation = G(40);
        new_budget_taken = false;
        CHECK(touch(&t) == &t && uploads == 1 && up_whole && t.uploaded && t.upload_gen == G(40) && !t.owed.any() && !t.owed.later);
        // (a budget taken every frame: left at most kLaterMax frames in a row, then made all the same)
        Texture n;
        new_budget_taken = true;
        for (uint32_t i = 0; i < upload_owed::kLaterMax; i++) {
            frame_no++;
            CHECK(touch(&n) == nullptr && uploads == 1 && n.owed.later == i + 1);
        }
        frame_no++;
        CHECK(touch(&n) == &n && uploads == 2 && up_whole && n.uploaded && !n.owed.any());
        // (a texture uploaded before is not this rule's: its new bytes are upload_again_later's, not taken here)
        frame_no++;
        written = {{0, 0x1000}}, written_gen = G(generation.v + 1), generation = G(generation.v + 1);
        CHECK(touch(&n) == &n && uploads == 3);
    } else if (name == "new-left-dispatch") {
        // (Triforce) left for the next frame by a draw, then met in the same frame by a look that may not leave it (a dispatch,
        // or the hold's bound reached): uploaded now, whole
        new_budget_taken = true;
        CHECK(touch(&t) == nullptr && uploads == 0);
        may_leave = false;
        t.checked_epoch++;
        CHECK(touch(&t) == &t && uploads == 1 && up_whole && t.uploaded && !t.owed.any() && owed_looks == 1);
        // (and a new one met by such a look is uploaded at once whatever the budget)
        Texture n;
        CHECK(touch(&n) == &n && uploads == 2 && n.uploaded);
    } else if (name == "new-left-starved") {
        // (Triforce) a new texture whose upload found no memory waits its own 30 presents; once they are over it is tried, not
        // left for later by the budget, and its wait is not the budget's (no dummy, no frame held for it)
        upload_ends = STARVED;
        CHECK(touch(&t) == &t && uploads == 1 && t.owed.failed == 1);
        new_budget_taken = true;
        frame_no++;
        CHECK(touch(&t) == &t && uploads == 1 && owed_looks == 0);
        frame_no += 29;
        CHECK(touch(&t) == &t && uploads == 2 && owed_looks == 0 && t.owed.failed == 2);
    } else if (name == "new-texture") {
        // a texture seen for the first time whose upload finds no staging memory: not marked uploaded, its generation kept
        upload_ends = STARVED;
        CHECK(touch(&t) == &t && uploads == 1 && up_whole);
        CHECK(!t.uploaded && !t.upload_gen && t.owed.whole && t.owed.from == frame_no + 30);
        CHECK(logs == 1 && last_log.find("no staging memory") != std::string::npos);
        // (the draws of the next presents ask nothing and try nothing)
        for (int i = 0; i < 29; i++) {
            frame_no++, generation.v++;
            CHECK(touch(&t) == &t);
        }
        CHECK(uploads == 1 && asked_dirty == 0 && asked_gpu == 0);
        // (its wait over: tried again, the whole of it; still none: twice the wait)
        frame_no++;
        CHECK(touch(&t) == &t && uploads == 2 && up_whole && !t.uploaded && t.owed.from == frame_no + 60);
        frame_no += 59;
        CHECK(touch(&t) == &t && uploads == 2);
        // (and when there is memory again it is uploaded, at that try's generation, with nothing owed)
        frame_no++, generation = G(77);
        upload_ends = DONE;
        CHECK(touch(&t) == &t && uploads == 3 && up_whole && t.uploaded && t.upload_gen == G(77) && !t.owed.any() && !t.owed.failed);
        // (from then on as any uploaded texture: nothing written, nothing uploaded)
        frame_no++;
        CHECK(touch(&t) == &t && uploads == 3);
    } else if (name == "written-part") {
        // an uploaded texture the CPU wrote a part of: the failed upload leaves its generation where it was
        // (Triforce: the generations of this case lie on both sides of 2^32, where the 32-bit ones started again at 0 and the
        // write at 2^32 + 12 was older than the upload at 2^32 - 6: not uploaded)
        const uint64_t base = (1ull << 32) - 16;
        t.uploaded = true, t.upload_gen = G(base + 10);
        written = {{0x4000, 0x6000}}, written_gen = G(base + 28), generation = G(base + 28);
        upload_ends = STARVED;
        CHECK(touch(&t) == &t && uploads == 1 && !up_whole && is(up_ranges, {{0x4000, 0x6000}}));
        CHECK(t.uploaded && t.upload_gen == G(base + 10) && is(t.owed.ranges, {{0x4000, 0x6000}}) && !t.owed.whole);
        // (the game writes the same bytes again, which is no write to a texture (RX_VK_TEX_SAME): nothing newer than the
        // write at base + 28 is told. Before, the generation had moved there and this texture was never uploaded again)
        frame_no += 30, generation = G(base + 36);
        upload_ends = DONE;
        CHECK(touch(&t) == &t && uploads == 2 && !up_whole && is(up_ranges, {{0x4000, 0x6000}}));
        CHECK(t.upload_gen == G(base + 36) && !t.owed.any());
        frame_no++;
        CHECK(touch(&t) == &t && uploads == 2);
    } else if (name == "remapped-part") {
        // a virtual texture's pages mapped anew are told once: the failed upload keeps them for the next try, with what is
        // written by then
        t.uploaded = true, t.upload_gen = G(10);
        remapped = {{0x10000, 0x20000}};
        upload_ends = STARVED;
        CHECK(touch(&t) == &t && uploads == 1 && is(up_ranges, {{0x10000, 0x20000}}) && is(t.owed.ranges, {{0x10000, 0x20000}}));
        frame_no += 30, generation = G(30);
        written = {{0x1f000, 0x21000}, {0x30000, 0x31000}}, written_gen = G(25);
        upload_ends = DONE;
        CHECK(touch(&t) == &t && uploads == 2 && !up_whole && is(up_ranges, {{0x10000, 0x21000}, {0x30000, 0x31000}}));
        CHECK(t.upload_gen == G(30) && !t.owed.any());
    } else if (name == "owed-alone") {
        // nothing is written and nothing mapped anew at the next try: what is owed is uploaded all the same
        t.uploaded = true, t.upload_gen = G(10);
        remapped = {{0x10000, 0x20000}};
        upload_ends = STARVED;
        touch(&t);
        frame_no += 30;
        upload_ends = DONE;
        CHECK(touch(&t) == &t && uploads == 2 && is(up_ranges, {{0x10000, 0x20000}}) && !t.owed.any());
    } else if (name == "gpu-write") {
        // a GPU buffer wrote the storage: noted as seen when asked, and not seen again once the upload it asks for failed
        t.uploaded = true, t.upload_gen = G(10), t.gpu_wrote_seen = 3;
        gpu_write = 5;
        upload_ends = STARVED;
        CHECK(touch(&t) == &t && uploads == 1 && up_whole);
        CHECK(t.gpu_wrote_seen == 3 && t.owed.whole && t.upload_gen == G(10));
        frame_no += 30, generation = G(40);
        upload_ends = DONE;
        CHECK(touch(&t) == &t && uploads == 2 && up_whole && t.gpu_wrote_seen == 5 && t.upload_gen == G(40) && !t.owed.any());
        frame_no++;
        CHECK(touch(&t) == &t && uploads == 2);
    } else if (name == "no-upload-fills-it") {
        // an image no upload fills (multisampled, depth and stencil): marked uploaded as before, not tried at every draw
        upload_ends = NEVER;
        CHECK(touch(&t) == &t && uploads == 1 && t.uploaded && t.upload_gen == generation && !t.owed.any() && !t.owed.from);
        CHECK(logs == 0);
        frame_no += 100;
        CHECK(touch(&t) == &t && uploads == 1);
        written = {{0, 0x1000}}, written_gen = G(generation.v + 1);   // (a write to its memory tries again)
        CHECK(touch(&t) == &t && uploads == 2);
    } else if (name == "bc3") {
        // a BC3 image's failed upload, of any cause: its own 30 presents each time, and what a failed upload of a part was to
        // bring is kept too
        t.bc = true;
        upload_ends = NEVER;   // (its encoder's scratch could not be made: not the staging)
        astc_fail = "no memory for the BC3 scratch";
        CHECK(touch(&t) == &t && uploads == 1 && !t.uploaded && t.bc_retry_frame == frame_no + 30 && t.owed.from == frame_no + 30);
        frame_no += 30;
        CHECK(touch(&t) == &t && uploads == 2 && t.owed.from == frame_no + 30 && t.owed.failed == 2);
        frame_no += 30;
        upload_ends = DONE;
        CHECK(touch(&t) == &t && uploads == 3 && t.uploaded && !t.owed.any());
        remapped = {{0x8000, 0x9000}};
        upload_ends = NEVER;
        frame_no++;
        CHECK(touch(&t) == &t && uploads == 4 && is(t.owed.ranges, {{0x8000, 0x9000}}) && t.uploaded);
        frame_no += 30;
        upload_ends = DONE;
        CHECK(touch(&t) == &t && uploads == 5 && is(up_ranges, {{0x8000, 0x9000}}) && !t.owed.any());
    } else if (name == "from-memory-note") {
        // one evicted that came from memory, made again over storage render targets took: its note, taken for the try, is
        // there again for the next one when the upload failed, and gone when it happened
        const uint64_t key = evicted_key(t.gpu, t.width, t.height, (uint32_t)t.nvn_format);
        note_taken = true;
        upload_ends = STARVED;
        CHECK(touch(&t) == &t && evicted_from_memory.count(key) == 1 && evicted_from_memory[key] == t.guest_size);
        evicted_from_memory.clear();
        frame_no += 30;
        upload_ends = DONE;
        CHECK(touch(&t) == &t && t.uploaded && evicted_from_memory.empty());
    } else {
        std::fprintf(stderr, "unknown case %s\n", name.c_str());
        return 2;
    }
    return 0;
}
