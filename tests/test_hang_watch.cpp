// The hang watch's judgement (gpu/vk/hang_watch.h): when a report is due, with authored presents and seconds.
#include <cstdio>
#include <cstdlib>

#include "../gpu/vk/hang_watch.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

using namespace mkwvk::hangwatch;

int main() {
    {   // frames keep coming: never a report
        Watch w;
        for (int s = 0; s < 100; s++) CHECK(!step(w, s, true, 20));
        CHECK(w.reports == 0);
    }
    {   // no frame for the limit, judged: one report at the limit's second, none after it while the stall lasts
        Watch w;
        CHECK(!step(w, 7, true, 20));   // (the first look only takes the count)
        for (int s = 1; s < 20; s++) CHECK(!step(w, 7, true, 20));
        CHECK(step(w, 7, true, 20));
        for (int s = 0; s < 100; s++) CHECK(!step(w, 7, true, 20));
        CHECK(w.reports == 1);
        // a frame, then another stall: its own report
        CHECK(!step(w, 8, true, 20));
        for (int s = 1; s < 20; s++) CHECK(!step(w, 8, true, 20));
        CHECK(step(w, 8, true, 20));
        CHECK(w.reports == 2);
    }
    {   // behind another window nothing counts, and the count starts again when the game is in front
        Watch w;
        CHECK(!step(w, 3, true, 20));
        for (int s = 0; s < 200; s++) CHECK(!step(w, 3, false, 20));
        for (int s = 1; s < 20; s++) CHECK(!step(w, 3, true, 20));
        CHECK(step(w, 3, true, 20));
        // nineteen seconds in front, one behind, nineteen in front: no report
        Watch v;
        CHECK(!step(v, 3, true, 20));
        for (int s = 1; s < 20; s++) CHECK(!step(v, 3, true, 20));
        CHECK(!step(v, 3, false, 20));
        for (int s = 1; s < 20; s++) CHECK(!step(v, 3, true, 20));
        CHECK(v.reports == 0);
    }
    {   // at most kReports a run
        Watch w;
        int told = 0;
        for (int stall = 0; stall < 10; stall++) {
            CHECK(!step(w, stall, true, 5));
            for (int s = 0; s < 30; s++) told += step(w, stall, true, 5);
        }
        CHECK(told == kReports && w.reports == kReports);
    }
    {   // ((Triforce) the stacks asked for from outside: served kAsked times a run, then no more)
        int served = 0, yes = 0;
        for (int i = 0; i < kAsked + 5; i++) yes += ask(served);
        CHECK(yes == kAsked && served == kAsked && kAsked >= 4 && kAsked <= 64);
        CHECK(kAskSuffix[0] == '.' && kAskSuffix[1] == 'n');
    }
    std::puts("PASS: the hang watch reports a stall once, only while judged, and a few times a run");
    return 0;
}
