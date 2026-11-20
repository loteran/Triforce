// tests/test_wb_rec_wait.py: gpu/vk/wb_rec_wait.h (a wait for a write-back by its writer's recording), and the stall line's
// held part (gpu/vk/stall_watch.h)
#include "../gpu/vk/wb_rec_wait.h"
#include "../gpu/vk/stall_watch.h"

#include <cstdio>
#include <cstring>
#include <vector>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    namespace wr = mkwvk::wb_rec_wait;

    // the switch: on with a number other than 0, off unset, empty, 0 or a word
    CHECK("unset", !wr::on(nullptr));
    CHECK("empty", !wr::on(""));
    CHECK("0", !wr::on("0"));
    CHECK("1", wr::on("1"));
    CHECK("2", wr::on("2"));
    CHECK("a word", !wr::on("yes"));

    // the finished submissions: known only once noted, the newest note of a value kept, the oldest values past the cap gone
    wr::Finished fin;
    CHECK("nothing known", fin.of(7) == 0);
    fin.note(7, 100);
    CHECK("noted", fin.of(7) == 100);
    CHECK("another value is not", fin.of(8) == 0);
    fin.note(7, 104);   // (finished again later: never in the replay, but the newer recording is the one that covers it)
    CHECK("the newer recording", fin.of(7) == 104);
    fin.note(0, 5), fin.note(9, 0);
    CHECK("no value or no recording: nothing noted", fin.of(0) == 0 && fin.of(9) == 0 && fin.by_value.size() == 1);
    fin.cap = 3;
    for (uint64_t v = 10; v < 14; v++) fin.note(v, 200 + v);
    CHECK("cap held", fin.by_value.size() == 3);
    CHECK("the oldest went", fin.of(7) == 0 && fin.of(10) == 0);
    CHECK("the newest stay", fin.of(11) == 211 && fin.of(13) == 213);

    // (Triforce) a burst in parts: piece 1 (value 40) finished, then the next piece's parallel replay, whose setup is a part's
    // submit (recording 300) and its post-part goes on; a split (301) later; the burst's end (302) records the write-backs. 40 is
    // known only at 302, and so is 41, finished after the setup; the parts' submits never finish a value
    wr::Finished burst;
    std::vector<uint64_t> open{40};
    burst.submitted(open, 300, false), open.clear();
    CHECK("burst: a part's submit does not finish the piece before it", burst.of(40) == 0);
    open.push_back(41);
    burst.submitted(open, 301, false), open.clear();
    CHECK("burst: nor a later part's", burst.of(40) == 0 && burst.of(41) == 0);
    burst.submitted(open, 302, true);
    CHECK("burst: its end finishes them", burst.of(40) == 302 && burst.of(41) == 302);
    CHECK("burst: nothing left pending", burst.pending.empty());
    open.push_back(50);
    burst.submitted(open, 303, true), open.clear();   // (a recording in one part: at its own submit, as before)
    CHECK("one part: at its submit", burst.of(50) == 303 && burst.of(40) == 302);
    burst.submitted(open, 304, false), burst.submitted(open, 305, true);   // (none finished in it: nothing noted)
    CHECK("none finished: nothing noted", burst.by_value.size() == 3);

    // the timeline's budget after the recording's wait: the bound less what that took, never below 0
    CHECK("nothing spent", wr::budget_left(wr::kBoundNs, 0) == wr::kBoundNs);
    CHECK("negative spent", wr::budget_left(wr::kBoundNs, -5) == wr::kBoundNs);
    CHECK("some spent", wr::budget_left(wr::kBoundNs, 50000000) == wr::kBoundNs - 50000000);
    CHECK("all spent", wr::budget_left(wr::kBoundNs, (int64_t)wr::kBoundNs) == 0);
    CHECK("more than all", wr::budget_left(wr::kBoundNs, (int64_t)wr::kBoundNs + 1) == 0);

    // the wait ends with the recording's only with the switch on and the recording known and done
    CHECK("on, known, done", wr::ends_at_recording(true, true, true));
    CHECK("off", !wr::ends_at_recording(false, true, true));
    CHECK("not known", !wr::ends_at_recording(true, false, false));
    CHECK("not done (its wait ran out)", !wr::ends_at_recording(true, true, false));

    // the held part: the wait after the recording was done; none when not known or not done; the signals' latency is not held
    CHECK("held: 40 ms wait, the recording done 2 ms in", wr::held_ns(true, true, 2000000, 40000000) == 38000000);
    CHECK("held: done at once", wr::held_ns(true, true, 0, 30000000) == 30000000);
    CHECK("not known", wr::held_ns(false, false, 0, 30000000) == 0);
    CHECK("not done", wr::held_ns(true, false, 250000000, 250000000) == 0);
    CHECK("the latency alone", wr::held_ns(true, true, 5000000, 5000000 + wr::kHeldMinNs - 1) == 0);
    CHECK("just over the latency", wr::held_ns(true, true, 5000000, 5000000 + wr::kHeldMinNs) == wr::kHeldMinNs);
    CHECK("the wait ended with the recording", wr::held_ns(true, true, 7000000, 7000000) == 0);

    // the stall line says the held part, only when there is one
    namespace sw = mkwvk::stall_watch;
    sw::Frame f;
    f.no = 23104, f.interval = 50.8, f.replay = 47.9, f.wb_wait = 39.4, f.wb_waits = 1, f.wb_ahead = 4;
    char b[2048];
    sw::stall_line(f, b, sizeof b);
    CHECK("no held part: not named", !std::strstr(b, "after their writer's recording"));
    f.wb_held = 1, f.wb_held_ms = 37.9;
    sw::stall_line(f, b, sizeof b);
    CHECK("held part named",
          std::strstr(b, "; 1 waits for write-backs still on the GPU 39.4 ms (up to 4 submissions ahead of it, 1 of them 37.9 ms after "
                         "their writer's recording was done (the timeline held by work stopped at a wait))") != nullptr);

    std::puts("PASS: write-back waits by the writer's recording, and the held part");
    return 0;
}
