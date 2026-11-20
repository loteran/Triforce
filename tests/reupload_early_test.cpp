// tests/test_reupload_early.py compiles and runs this: gpu/vk/reupload_early.h, and vk_texture.cpp's early looks before a
// parallel replay's segments (texture_reupload_early and what it calls, cut from the source at "/* CUT */") around stand-ins:
// a Texture of the fields they read, a texture_touch that uploads a texture whose memory the CPU wrote and notes it as the
// real one does (reupload_note), the textures-gone record (texture_gone.h, the real one). Cases:
// "list": the list's rules (hot after two re-uploads within kWindow presents, cold after kWindow without, kMost kept, the
//   order: owed first, then the newest) and wanted()'s: each thing that rules a texture out does.
// "picks": the early looks touch the hot textures the rule wants, owed first, with a draw's leave to defer (upload_defer_ok)
//   and only then, and give them back their age (last_frame, let_go, back_gap: no take-back for keep taken); the rest are
//   not touched.
// "gone": a texture gone since it was listed is dropped from the list without its memory being read (its id there may be
//   another texture's by then); so is every texture when more went since than the record keeps.
// "busy": nothing while the segments record (par_active), nothing with RX_VK_REUPLOAD_EARLY=0 ("off", its own process).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "reupload_early.h"
#include "texture_gone.h"
#include "upload_owed.h"

static int failures;
#define CHECK(x)                                              \
    do {                                                      \
        if (!(x)) {                                           \
            std::printf("FAIL:%d %s\n", __LINE__, #x);        \
            failures++;                                       \
        }                                                     \
    } while (0)

// ---- the stand-ins ----
struct Texture {
    uint32_t id = 0;
    bool uploaded = true, gpu_written = false, released = false, sparse = false, only_cleared = false;
    uint64_t guest_size = 4u << 20, last_frame = 0, let_go = 0;
    uint32_t back_gap = 0;   // ((Triforce) keep taken: what a take-back after a let-go notes)
    upload_owed::Owed owed;
    bool cpu_wrote = false;   // (its memory written: the next look uploads it)
    bool poisoned = false;    // (gone: reading it is the fault this test is for)
};
static uint64_t frame_no = 1000;
static std::atomic<bool> par_active{false};
static int64_t st_uploads;
static std::atomic<int64_t> frame_upload_bytes{0};
static thread_local bool upload_defer_ok = false;
static texture_gone::Record gone_record;
static int logs;
template <typename... A>
static void logf(const char*, A...) { logs++; }
int logs_seen() { return logs; }
static std::vector<uint32_t> touched;
static bool defer_ok_at_every_touch = true;
static bool poison_read = false;
namespace {
void reupload_note(Texture& tex);   // (the cut's, in its unnamed namespace)
}
Texture* texture_touch(Texture* tex, bool for_sampling) {
    if (tex->poisoned) poison_read = true;
    if (tex->let_go) tex->back_gap = (uint32_t)std::max<uint64_t>(tex->back_gap, frame_no - tex->let_go);   // (as the real one)
    tex->last_frame = frame_no, tex->let_go = 0;   // (as the real one: a draw's look)
    if (!for_sampling) return tex;
    touched.push_back(tex->id);
    if (!upload_defer_ok) defer_ok_at_every_touch = false;
    if (tex->cpu_wrote) {
        tex->cpu_wrote = false, st_uploads++;
        frame_upload_bytes += (int64_t)tex->guest_size;
        reupload_note(*tex);
    }
    return tex;
}

/* CUT */

// ---- the cases ----
static Texture make(uint32_t id) {
    Texture t;
    t.id = id, t.last_frame = frame_no;
    return t;
}
/// two re-uploads of t in a row (the second at frame_no), as the segments' looks note them
static void hot(Texture& t) {
    reupload_note(t);
    frame_no += 10;
    reupload_note(t);
    t.last_frame = frame_no;
}

static void list_case() {
    using namespace reupload_early;
    List l;
    int a = 0, b = 0;
    l.note(&a, 1, 100, 0);
    CHECK(l.hot(100).empty());   // (once: not yet)
    l.note(&a, 1, 100 + kWindow, 0);
    CHECK(l.hot(100 + kWindow).size() == 1);
    CHECK(l.hot(100 + 2 * kWindow).size() == 1);       // (hot while its last re-upload is within kWindow)
    CHECK(l.hot(101 + 2 * kWindow).empty() && l.size() == 0);   // (then dropped)
    l.note(&a, 1, 1000, 0);
    l.note(&a, 1, 1001 + kWindow, 0);   // (the second too long after the first: in a row from it again)
    CHECK(l.hot(1001 + kWindow).empty());
    l.note(&b, 2, 1002 + kWindow, 0);
    l.note(&b, 3, 1003 + kWindow, 0);   // (the same memory, another texture's number: another entry)
    CHECK(l.size() == 3 && l.hot(1003 + kWindow).empty());
    l.forget(&b, 2);
    CHECK(l.size() == 2);
    std::vector<int> many(kMost + 10);
    for (size_t i = 0; i < many.size(); i++) l.note(&many[i], 100 + (uint32_t)i, 5000 + i, 0);
    CHECK(l.size() == kMost);   // (the oldest make room)
    // wanted(): each rule
    Seen s;
    s.uploaded = true, s.bytes = kMinBytes, s.last_frame = 99;
    CHECK(wanted(s, 100) && !wanted(s, 101));   // (sampled this frame or the one before)
    Seen x = s;
    x.uploaded = false;
    CHECK(!wanted(x, 100));
    x = s, x.gpu_written = true;
    CHECK(!wanted(x, 100));
    x = s, x.released = true;
    CHECK(!wanted(x, 100));
    x = s, x.sparse = true;
    CHECK(!wanted(x, 100));
    x = s, x.only_cleared = true;
    CHECK(!wanted(x, 100));
    x = s, x.bytes = kMinBytes - 1;
    CHECK(!wanted(x, 100));
    // order(): owed first, then the newest re-upload
    Entry e1{nullptr, 1, 0, 10, 2}, e2{nullptr, 2, 0, 30, 2}, e3{nullptr, 3, 0, 20, 2};
    Seen owed = s;
    owed.owed_later = 1;
    std::vector<std::pair<Entry*, Seen>> v{{&e1, s}, {&e2, s}, {&e3, owed}};
    order(v);
    CHECK(v[0].first == &e3 && v[1].first == &e2 && v[2].first == &e1);
    std::printf("list: hot after %u re-uploads within %llu presents, %zu kept, owed first\n", kHotAfter,
                (unsigned long long)kWindow, kMost);
}

static void picks_case() {
    Texture a = make(1), b = make(2), c = make(3), d = make(4), e = make(5), f = make(6), g = make(7), h = make(8);
    for (Texture* t : {&a, &b, &c, &e, &f, &g, &h}) hot(*t);
    reupload_note(d);              // (once only)
    for (Texture* t : {&a, &b, &c, &d, &e, &f, &g, &h}) t->last_frame = frame_no;   // (all sampled now, but e)
    b.gpu_written = true;          // (the dry pass marked it: this command buffer draws into it)
    c.released = true;
    e.last_frame = frame_no - 2;   // (not sampled in this frame or the one before)
    g.guest_size = 512u << 10;     // (small)
    f.owed.later = 1;              // (left for this frame by the budget)
    a.cpu_wrote = f.cpu_wrote = true;
    h.only_cleared = true;
    a.let_go = 7, f.let_go = 9;
    const uint64_t a_age = a.last_frame, f_age = f.last_frame;
    texture_reupload_early();
    CHECK(touched.size() == 2 && touched[0] == 6 && touched[1] == 1);   // (f owed, first; then a)
    CHECK(defer_ok_at_every_touch && !upload_defer_ok);
    CHECK(a.last_frame == a_age && f.last_frame == f_age && a.let_go == 7 && f.let_go == 9);   // (their age the draws')
    CHECK(a.back_gap == 0 && f.back_gap == 0);   // (and an early look is no take-back)
    CHECK(st_uploads == 2 && !a.cpu_wrote && !f.cpu_wrote);
    // the next frame: nothing written, looked at again (no upload); then not sampled by any draw for two frames: not looked at
    touched.clear();
    frame_no++;
    texture_reupload_early();
    CHECK(touched.size() == 2 && st_uploads == 2);
    touched.clear();
    frame_no++;
    texture_reupload_early();
    CHECK(touched.empty());   // (the early looks did not make them look sampled)
    std::printf("picks: the owed texture, then the newest; one the GPU draws into, finalized, unsampled, small, once only or "
                "only cleared left to the segments\n");
}

static void gone_case() {
    Texture a = make(1), b = make(2);
    hot(a), hot(b);
    a.last_frame = b.last_frame = frame_no;
    a.cpu_wrote = b.cpu_wrote = true;
    gone_record.went(1);                     // (a went; its memory now another texture's, say)
    a.poisoned = true, a.id = 1;
    texture_reupload_early();
    CHECK(!poison_read && touched.size() == 1 && touched[0] == 2);
    CHECK(reup_size() == 1);                 // (dropped)
    // more gone since than the record keeps: b cannot be told, and counts as gone
    for (uint64_t i = 0; i < texture_gone::Record::kKept + 1; i++) gone_record.went(10000 + (uint32_t)i);
    touched.clear();
    b.poisoned = true;
    texture_reupload_early();
    CHECK(!poison_read && touched.empty() && reup_size() == 0);
    std::printf("gone: a texture gone since it was listed dropped unread\n");
}

static void busy_case() {
    Texture a = make(1);
    hot(a);
    a.cpu_wrote = true;
    par_active = true;
    texture_reupload_early();
    CHECK(touched.empty());
    par_active = false;
    texture_reupload_early();
    CHECK(touched.size() == 1);
    std::printf("busy: nothing while the segments record\n");
}

static void off_case() {   // (RX_VK_REUPLOAD_EARLY=0 set by the caller)
    Texture a = make(1);
    hot(a);
    a.cpu_wrote = true;
    texture_reupload_early();
    CHECK(touched.empty() && reup_size() == 0);   // (not even listed)
    std::printf("off: nothing listed, nothing looked at\n");
}

int main(int argc, char** argv) {
    const std::string c = argc > 1 ? argv[1] : "";
    if (c == "list") list_case();
    else if (c == "picks") picks_case();
    else if (c == "gone") gone_case();
    else if (c == "busy") busy_case();
    else if (c == "off") off_case();
    else return std::printf("which case?\n"), 2;
    return failures ? 1 : 0;
}
