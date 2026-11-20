// tests/test_frame_trace.py: gpu/vk/frame_trace.h, the frame trace's split of each frame (triforce_frames.csv's columns after
// the first ones), and its outside as the stall line's (gpu/vk/stall_watch.h)
#include "../gpu/vk/frame_trace.h"
#include "../gpu/vk/stall_watch.h"

#include <cstdio>
#include <cstring>
#include <string>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

static int commas(const char* s) {
    int n = 0;
    for (; *s; s++) n += *s == ',';
    return n;
}

int main() {
    namespace ft = mkwvk::frame_trace;
    namespace sw = mkwvk::stall_watch;
    char b[2048];

    // as many values as columns, in their order; outside: the interval less the replay and the present
    ft::Split s;
    s.interval = 16.0, s.replay = 9.5, s.present = 0.5, s.dispatch = 0.75, s.qsubmit = 0.25, s.par_wait = 1.5, s.game_wait = 2.0;
    s.captures = 1.25, s.wb_wait = 3.0, s.wb_held = 2.5, s.qs_bg = 0.1;
    CHECK("outside", ft::outside(s) == 6.0);
    char l[512];
    const int n = ft::line(s, l, sizeof l);
    CHECK("a line", n > 0 && (size_t)n < sizeof l);
    CHECK("as many values as columns", commas(l) == commas(ft::kColumns));
    CHECK("the values in the columns' order", !std::strcmp(l, "9.50,0.50,0.75,0.25,1.50,6.00,2.00,1.25,3.00,2.50,0.10"));
    CHECK("the columns", std::string(ft::kColumns) == "replay_ms,present_ms,dispatch_ms,qsubmit_ms,par_wait_ms,outside_ms,"
                                                      "game_wait_ms,captures_ms,wb_wait_ms,wb_held_ms,qs_bg_ms");
    // (a replay that began before the present before, a submit across it: outside below 0, written as it is)
    ft::Split o;
    o.interval = 10.0, o.replay = 11.0, o.present = 0.5;
    ft::line(o, l, sizeof l);
    CHECK("negative outside kept", !std::strncmp(l, "11.00,0.50,0.00,0.00,0.00,-1.50,", 32));
    // (a short buffer: snprintf's count, the text cut)
    char tiny[8];
    CHECK("cut", ft::line(s, tiny, sizeof tiny) == n && std::strlen(tiny) == sizeof tiny - 1);

    // the same frame's outside as the stall line says it
    sw::Frame g;
    g.interval = s.interval, g.replay = s.replay, g.present = s.present;
    sw::stall_line(g, b, sizeof b);
    CHECK("the stall line's outside agrees", std::strstr(b, "; outside the replay and the present 6.0 ms") != nullptr);

    std::puts("PASS: the frame trace's split of each frame");
    return 0;
}
