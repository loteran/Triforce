// tests/test_occ_answer.py: gpu/vk/occ_answer.h without a device (the samples-passed reports from occlusion queries).
#include "occ_answer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace mkwvk::occ;
using P = std::pair<uint64_t, uint64_t>;

static int failures = 0;
#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            failures++;                                                       \
        }                                                                     \
    } while (0)

// (recording serials: occ_slot_begin's. A and B: two recordings; A2: the slot of A begun again, its command buffer the same)
static const uint64_t A = 7, B = 8, A2 = 55;

int main() {
    {   // RX_VK_OCC_QUERY_SKIP
        const auto v = parse_addrs("0x10, 0x20;32 0x10");
        CHECK(v.size() == 2 && v[0] == 0x10 && v[1] == 0x20);
        CHECK(listed(v, 0x20) && !listed(v, 0x30));
        CHECK(parse_addrs(nullptr).empty() && parse_addrs("").empty() && parse_addrs("zz").empty());
        CHECK(parse_addrs("0x1000,junk,0x2000").size() == 1);   // (anything else ends the list)
    }
    {   // the lab knob, alone or joined
        CHECK(knob_names("occq", "occq") && knob_names("reclast+occq", "occq") && knob_names("occq+tlrec", "occq"));
        CHECK(!knob_names("occqx", "occq") && !knob_names("xoccq", "occq") && !knob_names("", "occq"));
    }
    {   // a block's sums: all available, or none
        const uint64_t pairs[] = {10, 1, 20, 1, 7, 0, 5, 1};
        uint64_t s = 99;
        CHECK(sum_range(pairs, 0, 2, &s) && s == 30);
        CHECK(!sum_range(pairs, 1, 2, &s));
        CHECK(sum_range(pairs, 3, 1, &s) && s == 5);
        CHECK(sum_range(pairs, 0, 0, &s) && s == 0);   // (no draw recorded: nothing passed)
    }
    {   // reset, two draws in two passes, report: the two queries
        Counter c;
        bool keep_now = true;
        Rep r;
        c.reset(3, A, 5);
        c.draw_enter(false);
        CHECK(c.begin(false, A));
        c.open = 5;                  // (the caller took index 5)
        c.draw_issued(false);
        c.open = -1;                 // (the pass ended)
        c.draw_enter(false);
        CHECK(c.begin(false, A));
        c.open = 6;
        c.draw_issued(false);
        c.draw_enter(false);
        CHECK(!c.begin(false, A));   // (the same pass: its query is open)
        c.draw_issued(false);
        c.open = -1;                 // (the report ends it)
        CHECK(c.report(false, A, 7, &r, &keep_now) == Counter::kMeasured && !keep_now);
        CHECK(r.first == 5 && r.n == 2);
        CHECK(!c.counting(false));   // (counting stops at the report)
        c.draw_enter(false);
        CHECK(!c.begin(false, A));
    }
    {   // a segment's report whose reset came before it: not measured
        Counter c;
        bool keep_now = false;
        Rep r;
        c.segment();
        c.draw_enter(false);
        CHECK(!c.begin(false, A));
        CHECK(c.report(false, A, 0, &r, &keep_now) == Counter::kNoReset);
    }
    {   // a draw the draw path left out: the count is incomplete
        Counter c;
        bool keep_now = false;
        Rep r;
        c.reset(0, A, 0);
        c.draw_enter(false);
        CHECK(c.begin(false, A));
        c.open = 0;
        c.draw_issued(false);
        c.draw_enter(false);   // (skipped: no issued)
        c.open = -1;
        CHECK(c.report(false, A, 1, &r, &keep_now) == Counter::kIncomplete && c.why == Counter::kSkipped);
    }
    {   // a draw recorded into another command buffer (a split): incomplete, and no query begun there
        Counter c;
        bool keep_now = false;
        Rep r;
        c.reset(0, A, 0);
        c.draw_enter(false);
        CHECK(!c.begin(false, B));
        c.draw_issued(false);
        CHECK(c.report(false, A, 0, &r, &keep_now) == Counter::kIncomplete && c.why == Counter::kOtherCmd);
    }
    {   // no recording's command buffer, or a full block
        Counter c;
        bool keep_now = false;
        Rep r;
        c.reset(-1, A, 0);
        CHECK(!c.begin(false, A));
        CHECK(c.report(false, A, 0, &r, &keep_now) == Counter::kIncomplete);
        Counter d;
        d.reset(1, A, 512);
        d.draw_enter(false);
        CHECK(d.begin(false, A));
        d.full();
        d.draw_issued(false);
        CHECK(d.report(false, A, 512, &r, &keep_now) == Counter::kIncomplete && d.why == Counter::kFull);
    }
    {   // reported twice without a reset: the second is not whole, the keep starts; with keep, counting goes on (cumulative)
        Counter c;
        bool keep_now = false;
        Rep r;
        c.reset(2, A, 0);
        c.draw_enter(false);
        CHECK(c.begin(false, A));
        c.open = 0;
        c.draw_issued(false);
        c.open = -1;
        CHECK(c.report(false, A, 1, &r, &keep_now) == Counter::kMeasured);
        CHECK(c.report(false, A, 1, &r, &keep_now) == Counter::kDouble && keep_now);
        CHECK(c.report(true, A, 1, &r, &keep_now) == Counter::kIncomplete);   // (the gap stays until the next reset)
        c.reset(2, A, 1);
        c.draw_enter(true);
        CHECK(c.begin(true, A));
        c.open = 1;
        c.draw_issued(true);
        c.open = -1;
        CHECK(c.report(true, A, 2, &r, &keep_now) == Counter::kMeasured && r.first == 1 && r.n == 1);
        CHECK(c.counting(true));   // (keep: on past the report)
        c.draw_enter(true);
        CHECK(c.begin(true, A));
        c.open = 2;
        c.draw_issued(true);
        c.open = -1;
        CHECK(c.report(true, A, 3, &r, &keep_now) == Counter::kMeasured && r.first == 1 && r.n == 2);   // (from the reset)
    }
    {   // M1: a count is its recording's (by serial, not by command buffer): a slot begun again under it loses it
        Counter c;
        bool keep_now = false;
        Rep r;
        c.reset(4, A, 3);
        c.draw_enter(false);
        CHECK(c.may_begin(false) && c.begin(false, A));
        c.open = 3;
        c.draw_issued(false);
        c.open = -1;
        c.draw_enter(false);
        CHECK(!c.begin(false, A2));   // (the same slot, the same command buffer, begun again: another recording)
        CHECK(c.lost && c.why == Counter::kOtherCmd && !c.may_begin(false));
        c.draw_issued(false);
        CHECK(c.report(false, A2, 5, &r, &keep_now) == Counter::kIncomplete);
        Counter d;   // (no draw at all, the report in another recording than the reset: incomplete too)
        d.reset(4, A, 0);
        CHECK(d.report(false, B, 0, &r, &keep_now) == Counter::kIncomplete && d.why == Counter::kOtherCmd);
        Counter e;   // (serial 0: no recording's)
        e.reset(4, 0, 0);
        CHECK(e.lost && e.slot == -1 && !e.may_begin(false));
        Counter f;   // (a segment's edge: nothing counting, a report has no reset)
        f.reset(2, A, 0);
        f.segment();
        CHECK(f.report(false, A, 0, &r, &keep_now) == Counter::kNoReset);
    }
    {   // Writers: the newest report at an address in stream order; a count only into its own report's record
        Writers w;
        CHECK(!w.is_report(0x100) && w.verdict(0x100, 10) == Writers::kUnknown);
        w.placed(0x100, 10, 1, true, kVisible);
        CHECK(w.is_report(0x100) && w.verdict(0x100, 10) == Writers::kWrite && w.value(0x100) == kVisible);
        w.placed(0x100, 25, 2, true, kVisible);   // (the ring came round: another object's report at the same address)
        CHECK(w.verdict(0x100, 10) == Writers::kSuperseded);   // (the older count: never over the newer placeholder)
        CHECK(w.verdict(0x100, 25) == Writers::kWrite);
        CHECK(w.verdict(0x100, 30) == Writers::kUnknown);      // (a report never placed there)
        w.placed(0x200, 5, 2, false, kVisible);   // (skip list, knob off or a clear flag: its count is not written)
        CHECK(w.verdict(0x200, 5) == Writers::kFixed);
        w.placed(0x400, 1, 1, true, kVisible);
        w.prune(1 + kKeepAge, kKeepAge);   // (not older than the age yet)
        CHECK(w.is_report(0x400));
        w.prune(2 + kKeepAge, kKeepAge);   // (reported last more than kKeepAge presents ago: gone, its count unknown)
        CHECK(!w.is_report(0x400) && w.verdict(0x400, 1) == Writers::kUnknown && w.is_report(0x100) && w.size() == 2);
    }
    {   // Writers: where a count stands, as a render condition finds the word (NX2navi's Pending: pending until written)
        Writers w;
        CHECK(w.word(0x500, 3) == Writers::kNone);              // (no samples-passed report there: memory, as before)
        w.placed(0x500, 40, 3, true, kVisible);
        CHECK(w.word(0x500, 3) == Writers::kPendingSrc);        // (its count to come: unknown)
        CHECK(w.word(0x500, 3 + kLateAge) == Writers::kPendingSrc);
        CHECK(w.word(0x500, 4 + kLateAge) == Writers::kFinalSrc);   // (too late to be written now: memory)
        w.wrote(0x500, 39, 0);                                  // (another place's count: not this report's)
        CHECK(w.word(0x500, 3) == Writers::kPendingSrc);
        w.wrote(0x500, 40, 0);
        CHECK(w.word(0x500, 3) == Writers::kWrittenSrc && w.value(0x500) == 0);
        CHECK(w.verdict(0x500, 40) == Writers::kWrite);         // (the same place again, the later of two: over the first)
        w.settle(0x500, 40);                                    // (settles only a pending one)
        CHECK(w.word(0x500, 3) == Writers::kWrittenSrc);
        w.placed(0x500, 41, 4, true, kVisible);                 // (the next report: pending again, the old count is history)
        CHECK(w.word(0x500, 4) == Writers::kPendingSrc && w.value(0x500) == kVisible);
        w.settle(0x500, 40);                                    // (an older place: nothing)
        CHECK(w.word(0x500, 4) == Writers::kPendingSrc);
        w.settle(0x500, 41);                                    // (not counted whole, lost, never sent: no count comes)
        CHECK(w.word(0x500, 4) == Writers::kFinalSrc && w.verdict(0x500, 41) == Writers::kFixed);
        w.placed(0x600, 50, 4, false, kVisible);                // (knob off, skip list, clear flag: memory at once)
        CHECK(w.word(0x600, 4) == Writers::kFinalSrc);
        // a report of another counter type at the address: the newest there, the samples-passed one leaves
        w.placed(0x700, 60, 4, true, kVisible);
        CHECK(w.erase(0x700) && !w.erase(0x700) && w.word(0x700, 4) == Writers::kNone && w.verdict(0x700, 60) == Writers::kUnknown);
        // a small buffer clear in stream order over records: those whose count word it reaches leave
        Writers c;
        CHECK(c.cleared(0x1000, 0x1010) == 0);                  // (nothing placed)
        c.placed(0x1000, 1, 1, true, kVisible), c.placed(0x1010, 2, 1, true, kVisible), c.placed(0x1020, 3, 1, true, kVisible);
        c.placed(0x2ff8, 4, 1, true, kVisible);                 // (its count word up to the page's end)
        CHECK(c.cleared(0x1008, 0x1010) == 0);                  // (0x1000's timestamp word only: its count stays)
        CHECK(c.is_report(0x1000) && c.cleared(0x1004, 0x1008) == 1 && !c.is_report(0x1000));   // (half its count word)
        CHECK(c.cleared(0x1014, 0x1030) == 2 && !c.is_report(0x1010) && !c.is_report(0x1020));
        CHECK(c.cleared(0x2ffc, 0x3000) == 1 && !c.is_report(0x2ff8));
        CHECK(c.cleared(0x9000, 0x9100) == 0 && c.size() == 0);
    }
    {   // a render condition over the words' sources: NX2navi's cond_enabled (a count not done yet renders)
        using S = Writers;
        // (no samples-passed word: the answer from memory, whatever the switches)
        CHECK(!cond_decide(false, S::kNone, S::kNone, true, true).enabled && cond_decide(false, S::kNone, S::kNone, true, true).kind == Cond::kNotSamples);
        // (one word pending, the other anything: renders)
        CHECK(cond_decide(false, S::kPendingSrc, S::kNone, true, true).enabled && cond_decide(false, S::kNone, S::kPendingSrc, true, true).kind == Cond::kUnknown);
        CHECK(cond_decide(false, S::kWrittenSrc, S::kPendingSrc, true, true).enabled);
        // (a written count: memory decides, a 0 can skip)
        CHECK(!cond_decide(false, S::kWrittenSrc, S::kNone, true, true).enabled && cond_decide(false, S::kWrittenSrc, S::kNone, true, true).kind == Cond::kWrittenOff);
        CHECK(cond_decide(true, S::kNone, S::kWrittenSrc, true, true).kind == Cond::kWrittenOn);
        // (no count to come: memory, as before)
        CHECK(!cond_decide(false, S::kFinalSrc, S::kNone, true, true).enabled && cond_decide(false, S::kFinalSrc, S::kFinalSrc, true, true).kind == Cond::kMemory);
        // (the knob's off phase: the base answer even over a pending word)
        CHECK(!cond_decide(false, S::kPendingSrc, S::kPendingSrc, false, true).enabled && cond_decide(false, S::kPendingSrc, S::kNone, false, true).kind == Cond::kBase);
        // (RX_VK_OCC_COND=0: renders, written 0 or not)
        CHECK(cond_decide(false, S::kWrittenSrc, S::kNone, true, false).enabled && cond_decide(false, S::kFinalSrc, S::kNone, true, false).kind == Cond::kForced);
        CHECK(std::strcmp(src_name(S::kPendingSrc), "pending") == 0 && std::strcmp(src_name(S::kNone), "memory") == 0);
        Stats st;
        st.cond(Cond::kNotSamples), st.cond(Cond::kUnknown), st.cond(Cond::kWrittenOff), st.cond(Cond::kWrittenOff), st.cond(Cond::kBase);
        CHECK(st.conds == 4 && st.cond_unknown == 1 && st.cond_written_off == 2 && st.cond_base == 1 && st.cond_memory == 0);
    }
    {   // complete: whole counts written (0 too), every other case leaves the record; latency; the compare-and-write
        Writers w;
        Stats st;
        // (block: q0 30, q1 0, q2 7 not available, q3 0, q4 5, q5 9)
        const uint64_t pairs[] = {30, 1, 0, 1, 7, 0, 0, 1, 5, 1, 9, 1};
        std::vector<Rep> reps;
        const auto rep = [&](uint64_t addr, uint32_t first, uint32_t n, uint64_t pos, int64_t present) {
            Rep r;
            r.addr = addr, r.first = first, r.n = n, r.pos = pos, r.present = present;
            reps.push_back(r);
        };
        rep(0x10, 0, 2, 100, 50);   // 30: written
        rep(0x20, 3, 1, 101, 50);   // 0: written as 0 (whole)
        rep(0x30, 2, 1, 102, 50);   // q2 not available: lost
        rep(0x40, 4, 1, 103, 50);   // superseded: a later report at 0x40
        rep(0x50, 5, 1, 104, 40);   // late: read back more than kLateAge presents after
        rep(0x60, 4, 3, 105, 50);   // outside the queries the recording took: lost
        rep(0x70, 0, 0, 106, 49);   // no draw at all: 0, whole
        rep(0x80, 5, 1, 107, 50);   // knob off when placed: kept
        rep(0x90, 5, 1, 108, 50);   // the CPU re-armed the record since: the compare fails, not written
        rep(0xa0, 5, 1, 109, 50);   // pruned (or rewritten in the stream): unknown
        w.placed(0x10, 100, 50, true, kVisible), w.placed(0x20, 101, 50, true, kVisible), w.placed(0x30, 102, 50, true, kVisible);
        w.placed(0x40, 103, 50, true, kVisible), w.placed(0x40, 200, 51, true, kVisible);
        w.placed(0x50, 104, 40, true, kVisible), w.placed(0x60, 105, 50, true, kVisible), w.placed(0x70, 106, 49, true, kVisible);
        w.placed(0x80, 107, 50, false, kVisible), w.placed(0x90, 108, 50, true, kVisible);
        std::unordered_map<uint64_t, uint64_t> mem;   // (the guest words: the replay's placeholders, 0x90 re-armed with 1)
        for (uint64_t a = 0x10; a <= 0xa0; a += 0x10) mem[a] = kVisible;
        mem[0x90] = 1;
        std::vector<std::pair<uint64_t, uint64_t>> wrote, seen;
        const auto cas = [&](uint64_t a, uint64_t expect, uint64_t c) {
            if (mem[a] != expect) return false;
            mem[a] = c;
            wrote.push_back({a, c});
            return true;
        };
        complete(reps, pairs, 6, true, w, 50 + kLateAge - 1, st, cas, [&](uint64_t a, uint64_t c) { seen.push_back({a, c}); });
        // (now = 57: 0x50's report at 40 is 17 old, late; the others 7 or 8 old)
        CHECK(wrote.size() == 3 && wrote[0] == P(0x10, 30) &&
              wrote[1] == P(0x20, 0) && wrote[2] == P(0x70, 0));
        CHECK(st.written == 3 && st.zeros == 2 && st.lost == 2 && st.superseded == 1 && st.late == 1 && st.kept_fixed == 1 &&
              st.rearmed == 1 && st.unknown == 1);
        CHECK(mem[0x90] == 1 && mem[0x40] == kVisible);   // (the CPU's 1 stays; the newer object's placeholder stays)
        CHECK(seen.size() == 8);   // (every whole count read back, written or not)
        CHECK(st.lat_sum == 7 + 7 + 8 && st.lat_ge1 == 3 && st.lat_ge2 == 3);
        // (where the counts stand now, for a condition: written, or no count to come)
        CHECK(w.word(0x10, 57) == Writers::kWrittenSrc && w.word(0x20, 57) == Writers::kWrittenSrc);
        CHECK(w.word(0x30, 57) == Writers::kFinalSrc && w.word(0x60, 57) == Writers::kFinalSrc && w.word(0x90, 57) == Writers::kFinalSrc);
        CHECK(w.word(0x50, 47) == Writers::kFinalSrc);    // (late: settled, even while it would still count as recent)
        CHECK(w.word(0x40, 57) == Writers::kPendingSrc);  // (the newer report's count is still to come)
        Stats s2;
        wrote.clear();
        Writers w2;
        w2.placed(0x10, 100, 50, true, kVisible);
        complete(reps, pairs, 6, false, w2, 50, s2, cas, [](uint64_t, uint64_t) {});
        CHECK(wrote.empty() && s2.lost == (int64_t)reps.size());   // (the read failed: nothing written)
        CHECK(w2.word(0x10, 50) == Writers::kFinalSrc);             // (and no count comes)
        Stats s3;
        std::vector<Rep> same;   // (two reports at one address and one place (no draw between): in stream order, the last wins)
        Rep r1, r2;
        r1.addr = r2.addr = 0xb0, r1.pos = r2.pos = 300, r1.present = r2.present = 60, r1.first = 0, r1.n = 1, r2.first = 1, r2.n = 0;
        same.push_back(r1), same.push_back(r2);
        w.placed(0xb0, 300, 60, true, kVisible);
        mem[0xb0] = kVisible;
        wrote.clear();
        complete(same, pairs, 6, true, w, 60, s3, cas, [](uint64_t, uint64_t) {});
        CHECK(wrote.size() == 2 && wrote.back().second == 0 && mem[0xb0] == 0 && s3.lat_ge1 == 0);
        // (never sent: every report settled, counted)
        Writers w4;
        Stats s4;
        w4.placed(0x10, 100, 50, true, kVisible), w4.placed(0x20, 101, 50, true, kVisible);
        dropped(reps, w4, s4);
        CHECK(s4.dropped == (int64_t)reps.size() && w4.word(0x10, 50) == Writers::kFinalSrc && w4.word(0x20, 50) == Writers::kFinalSrc);
    }
    {   // the stats line
        Stats s;
        s.reports = 7, s.in_segments = 5, s.measured = 6, s.written = 4, s.zeros = 1, s.superseded = 2, s.rearmed = 1;
        s.lat_sum = 6, s.lat_ge1 = 3, s.lat_ge2 = 2, s.at[Stats::kAtWait] = 9, s.at[Stats::kAtReuse] = 1, s.rewritten = 4;
        s.conds = 10, s.cond_base = 1, s.cond_unknown = 2, s.cond_forced = 0, s.cond_written_on = 3, s.cond_written_off = 3,
        s.cond_memory = 1, s.draws_skipped = 12, s.clears_skipped = 2;
        char line[2048];
        CHECK(s.line(line, sizeof line, 600, 11) < (int)sizeof line);
        const std::string l = line;
        CHECK(l.find("600 presents: 7 samples-passed reports, 4096 written at the replay as before (5 by a parallel") != std::string::npos);
        CHECK(l.find("4 records rewritten in the stream after their report") != std::string::npos);
        CHECK(l.find("6 counted whole") != std::string::npos);
        CHECK(l.find("At completion: 4 counts written (1 of them 0)") != std::string::npos);
        CHECK(l.find("2 superseded (the record reported again first)") != std::string::npos);
        CHECK(l.find("1 not written: the record no longer held the replay's value; mean latency 1.50 presents, 3 written after the next "
                     "present, 2 after two") != std::string::npos);
        CHECK(l.find("recordings read at: 9 a game's fence wait, 0 a recording's begin, 0 a present, 1 the slot's reuse") !=
              std::string::npos);
        CHECK(l.find("Render conditions on samples-passed words: 10 (1 as before: knob occq off; 2 rendered: a count pending "
                     "(unknown); 0 rendered: RX_VK_OCC_COND=0; 3 rendered from a written count; 3 skipped from a written count; 1 "
                     "from memory, no count to come); skipped by them: 12 draws and dispatches, 2 clears") != std::string::npos);
        CHECK(l.find("11 tracked") != std::string::npos);
    }
    {   // the first 64 sites only
        Sites st;
        for (uint64_t a = 1; a <= 70; a++) st.reported(a * 16);
        st.reported(16);
        st.measured(16, 3);
        st.measured(16, 9);
        st.wrote(16);
        st.measured(70 * 16, 1);   // (not among them)
        st.wrote(70 * 16);
        CHECK(st.n == 64 && st.s[0].reports == 2 && st.s[0].measured == 2 && st.s[0].lo == 3 && st.s[0].hi == 9 &&
              st.s[0].last == 9 && st.s[0].written == 1);
        int lines = 0;
        st.log([&](const char* l) {
            lines++;
            CHECK(std::strstr(l, "vk: occlusion query sites") == l);
        });
        CHECK(lines == 8);
        Distinct d;
        d.note(1), d.note(1), d.note(2);
        CHECK(d.take() == 2 && d.take() == 0);
    }
    if (failures) std::fprintf(stderr, "%d failed\n", failures);
    else std::printf("occ_answer: all cases pass\n");
    return failures ? 1 : 0;
}
