// tests/test_parallel_fixes.py compiles and runs this: gpu/vk/touch_quick.h, the lock-free look of a texture in a parallel
// replay. The locked look's steps one at a time, and then both on two threads. No game, no GPU.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include "touch_quick.h"

static int failures;
#define CHECK(x)                                              \
    do {                                                      \
        if (!(x)) {                                           \
            std::printf("FAIL:%d %s\n", __LINE__, #x);        \
            failures++;                                       \
        }                                                     \
    } while (0)

/// The look as it was before: what another thread has noted so far taken for what it will leave
static bool before(const touch_quick::Seen& t, uint32_t epoch, uint64_t rec, uint64_t seg) {
    if (t.upload_value == rec && t.upload_seg > seg) return false;
    return t.uploaded && t.checked_epoch == epoch;
}

static void steps() {
    using touch_quick::Seen;
    using touch_quick::up_to_date;
    constexpr uint32_t kEpoch = 7;
    constexpr uint64_t kRec = 100;
    // a texture uploaded in an earlier submission, whose memory the game wrote since. Segment 3's worker looks at it with
    // the lock and uploads it; segment 1's worker looks without the lock at each step of that
    Seen t{true, kEpoch - 1, 0, kRec - 1, 0};
    CHECK(!up_to_date(t, kEpoch, kRec, 1));   // (not looked at in this submission: the locked look)
    t.touching = 1;                           // the locked look begins
    CHECK(!up_to_date(t, kEpoch, kRec, 1));
    t.checked_epoch = kEpoch;                 // it notes that it looked; the upload is under way
    CHECK(!up_to_date(t, kEpoch, kRec, 1));
    CHECK(before(t, kEpoch, kRec, 1));        // (the hole: the look as it was says "up to date" here)
    t.upload_value = kRec;
    CHECK(!up_to_date(t, kEpoch, kRec, 1));
    t.upload_seg = 3;
    CHECK(!up_to_date(t, kEpoch, kRec, 1));
    t.touching = 0;                           // the locked look is over
    CHECK(!up_to_date(t, kEpoch, kRec, 1));   // (uploaded by a later segment: segment 1 uploads it again, with the lock)
    CHECK(up_to_date(t, kEpoch, kRec, 3) && up_to_date(t, kEpoch, kRec, 5));   // (the uploader's own segment and later ones)
    CHECK(!up_to_date(t, kEpoch + 1, kRec + 1, 5));                           // (the next submission: looked at again)
    // the same look when nothing was written: noted as looked at, nothing uploaded
    Seen u{true, kEpoch - 1, 1, kRec - 1, 0};
    u.checked_epoch = kEpoch;
    CHECK(!up_to_date(u, kEpoch, kRec, 1));
    u.touching = 0;
    CHECK(up_to_date(u, kEpoch, kRec, 1) && up_to_date(u, kEpoch, kRec, 9));
    // one never uploaded is never up to date, and one uploaded in this submission by an earlier segment is
    CHECK(!up_to_date(Seen{false, kEpoch, 0, kRec, 0}, kEpoch, kRec, 1));
    CHECK(up_to_date(Seen{true, kEpoch, 0, kRec, 1}, kEpoch, kRec, 2));
}

// ---- two threads: the fields as vk_texture.h has them (plain ones and the atomic), written under a lock in
// texture_touch's order and read without it in texture_touch_quick's ----
struct Tex {
    bool uploaded = true;
    uint32_t checked_epoch = 0;
    uint64_t upload_value = 0, upload_seg = 0;
    std::atomic<uint32_t> touching{0};
};
static bool quick(const Tex* tex, uint32_t epoch, uint64_t rec, uint64_t seg) {
    touch_quick::Seen seen;
    seen.uploaded = __atomic_load_n(&tex->uploaded, __ATOMIC_SEQ_CST);
    seen.checked_epoch = __atomic_load_n(&tex->checked_epoch, __ATOMIC_SEQ_CST);
    seen.touching = tex->touching.load();
    seen.upload_value = __atomic_load_n(&tex->upload_value, __ATOMIC_SEQ_CST);
    seen.upload_seg = __atomic_load_n(&tex->upload_seg, __ATOMIC_SEQ_CST);
    return touch_quick::up_to_date(seen, epoch, rec, seg);
}
static void threads() {
    // each round is a submission: segment 3's worker uploads the texture with the lock while segment 1's looks without it,
    // over and over. Segment 1 must never be told "up to date": before the look it was not looked at, during it the look
    // is under way, after it a later segment uploaded it
    constexpr int kRounds = 20000;
    std::atomic<int> round{0}, wrong{0}, looks{0};
    std::atomic<bool> stop{false};
    Tex tex;
    std::mutex lock;
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const int r = round.load();
            if (r <= 0) continue;
            const bool up = quick(&tex, (uint32_t)r, (uint64_t)r, 1);
            if (up && round.load() == r) wrong++;   // (the same submission still: the answer was about it)
            looks++;
        }
    });
    // (rounds go on past kRounds until the reader has looked often enough: on a loaded PC, the whole test suite at once,
    // the writer could finish its rounds before the reader was ever scheduled, and the check below failed with nothing wrong)
    int rounds = 0;
    for (int r = 1; r <= kRounds || (looks.load() <= kRounds / 100 && r <= kRounds * 50); r++) {
        rounds = r;
        round.store(r);
        {
            std::lock_guard lk(lock);
            tex.touching.store(1), std::atomic_thread_fence(std::memory_order_seq_cst);   // (as texture_touch's Touching)
            tex.checked_epoch = (uint32_t)r;
            for (volatile int spin = 0; spin < 40; spin = spin + 1) {}   // (the upload)
            tex.upload_value = (uint64_t)r, tex.upload_seg = 3;
            tex.touching.store(0);
        }
        for (volatile int spin = 0; spin < 40; spin = spin + 1) {}
    }
    stop = true;
    reader.join();
    CHECK(wrong.load() == 0);
    CHECK(looks.load() > kRounds / 100);   // (the reader ran beside the writer)
    std::printf("touch_quick on two threads: %d submissions, %d looks without the lock, %d wrong\n", rounds, looks.load(), wrong.load());
}

int main() {
    steps();
    threads();
    if (failures) return 1;
    std::puts("touch_quick: all checks passed");
    return 0;
}
