// tests/test_stall_watch.py: gpu/vk/stall_watch.h, the frame times and the stall lines of the log
#include "../gpu/vk/stall_watch.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    namespace sw = mkwvk::stall_watch;
    char b[1024];

    // the frame times: the longest, and the counts over each edge (over, not at)
    sw::Histogram h;
    for (double ms : {16.6, 33.0, 33.1, 50.0, 51.0, 100.0, 120.0, 250.0, 252.0, 8.0}) h.add(ms);
    CHECK("frames", h.frames == 10);
    CHECK("the longest", h.longest == 252.0);
    CHECK("over 33", h.over[0] == 7);
    CHECK("over 50", h.over[1] == 5);
    CHECK("over 100", h.over[2] == 3);
    CHECK("over 250", h.over[3] == 1);
    sw::describe(h, b, sizeof b);
    CHECK("described", !std::strcmp(b, "the longest 252.0 ms, 7 over 33 ms, 5 over 50, 3 over 100, 1 over 250"));
    h.reset();
    CHECK("reset", h.frames == 0 && h.longest == 0 && h.over[0] == 0 && h.over[3] == 0);

    // the bound: three at once, then one a third of a second; never more than the session's total
    sw::Bound bound;
    const int64_t s = 1000000000;
    int64_t t = 5 * s;
    CHECK("first", bound.take(t));
    CHECK("second", bound.take(t + 1));
    CHECK("third", bound.take(t + 2));
    CHECK("a fourth at once is held", !bound.take(t + 3));
    CHECK("held counted", bound.held_since() == 1 && bound.held_since() == 0);
    CHECK("a third of a second later: one more", bound.take(t + s / 3 + 4));
    CHECK("and not two", !bound.take(t + s / 3 + 5));
    CHECK("after a quiet minute: a burst of three, not sixty", bound.take(t + 61 * s) && bound.take(t + 61 * s + 1) &&
                                                                    bound.take(t + 61 * s + 2) && !bound.take(t + 61 * s + 3));
    sw::Bound few;
    few.total = 2;
    CHECK("the session's total", few.take(s) && few.take(10 * s) && !few.take(20 * s) && !few.take(30 * s));
    // (a second's lines at most three: over a minute of stalls every frame, about 180)
    sw::Bound minute;
    int lines = 0;
    for (int64_t i = 0; i < 60 * 60; i++) lines += minute.take(s + i * (s / 60));
    CHECK("a minute of stalls at 60 frames a second", lines >= 178 && lines <= 183);

    // the stall line: the phases of 1 ms or more, memory sync's parts and its waits for write-backs, then the rest
    static const char* names[13] = {"submit", "present", "draw", "descriptors", "texture lookups", "uploads", "pipelines",
                                    "programs", "commands", "dispatch", "memory sync", "queue submit", "slot wait"};
    double ms[13] = {95.0, 0.4, 5.1, 0.9, 0.0, 3.0, 0.2, 0.0, 1.2, 0.0, 90.0, 0.3, 0.0};
    sw::Frame f;
    f.no = 12345, f.interval = 123.4, f.replay = 95.4, f.phases = 13, f.names = names, f.ms = ms;
    f.maps = 0.0, f.capture = 89.9, f.collect = 0.1;
    f.wb_wait = 89.5, f.wb_waits = 1, f.wb_ahead = 3, f.wb_behind_present = 1;
    f.uploads = 2, f.upload_mb = 1.5;
    f.present = 2.0;
    f.game_wait = 12.0, f.game_waits = 3;
    sw::stall_line(f, b, sizeof b);
    std::printf("%s\n", b);
    CHECK("the frame", !std::strncmp(b, "vk: stall: frame 12345: 123.4 ms present to present; replay 95.4 ms (draw 5.1, uploads 3.0, ", 92));
    CHECK("a phase under 1 ms is not named", !std::strstr(b, "descriptors") && !std::strstr(b, "queue submit"));
    CHECK("submit and present hold the rest: not named as phases", !std::strstr(b, "(submit") && !std::strstr(b, ", submit"));
    CHECK("memory sync with its parts", std::strstr(b, "memory sync 90.0 [capture taken 89.9])") != nullptr);
    CHECK("the write-back waits", std::strstr(b, "; 1 waits for write-backs still on the GPU 89.5 ms (up to 3 submissions ahead "
                                                 "of it, 1 of the waits behind a present's blit)") != nullptr);
    CHECK("a part under 1 ms is not named", !std::strstr(b, "maps") && !std::strstr(b, "writes collected"));
    CHECK("none unsent: not named", !std::strstr(b, "not on the queue"));
    f.wb_unsent = 1, f.wb_unsent_ms = 61.6;
    sw::stall_line(f, b, sizeof b);
    CHECK("a wait for a submission not on the queue yet", std::strstr(b, "1 of the waits behind a present's blit, 1 of them for a "
                                                                         "submission not on the queue yet, 61.6 ms)") != nullptr);
    f.wb_unsent = 0, f.wb_unsent_ms = 0;
    sw::stall_line(f, b, sizeof b);
    CHECK("no pipeline waits: not named", !std::strstr(b, "pipelines ("));
    CHECK("uploads", std::strstr(b, "; 2 uploads (1.5 MB)") != nullptr);
    CHECK("present", std::strstr(b, "; present 2.0 ms") != nullptr);
    CHECK("no wait for the present before: not named", !std::strstr(b, "present before"));
    // ((Triforce) RX_VK_PRESENT_THREAD: of the presenting thread's present, the wait for the one handed over before it)
    f.present = 9.5, f.present_wait = 8.0;
    sw::stall_line(f, b, sizeof b);
    CHECK("the wait for the present before", std::strstr(b, "; present 9.5 ms (8.0 of it waiting for the present before on the "
                                                            "present thread); the game's fence waits") != nullptr);
    f.present = 2.0, f.present_wait = 0.5;
    sw::stall_line(f, b, sizeof b);
    CHECK("a wait under 1 ms: not named", std::strstr(b, "; present 2.0 ms;") != nullptr && !std::strstr(b, "present before"));
    f.present_wait = 0;
    CHECK("the game's waits", std::strstr(b, "; the game's fence waits 12.0 ms in 3") != nullptr);
    CHECK("the rest outside", std::strstr(b, "; outside the replay and the present 26.0 ms") != nullptr);
    // ((Triforce) the submit-time captures on the game's threads, after "outside", which holds most of them)
    CHECK("no captures of 1 ms: not named", !std::strstr(b, "captures at the game's submits"));
    f.captures = 2.5;
    sw::stall_line(f, b, sizeof b);
    CHECK("the captures", std::strstr(b, "; outside the replay and the present 26.0 ms; the captures at the game's submits 2.5 ms") != nullptr);
    f.captures = 0.9;
    sw::stall_line(f, b, sizeof b);
    CHECK("captures under 1 ms: not named", !std::strstr(b, "captures at the game's submits"));
    f.captures = 0;
    sw::stall_line(f, b, sizeof b);
    CHECK("no GPU-behind under 1 ms", !std::strstr(b, "GPU behind"));
    CHECK("no imports: not named", !std::strstr(b, "imported"));
    f.imports = 7, f.import_ms = 78.6;
    sw::stall_line(f, b, sizeof b);
    CHECK("the pools imported where bound", std::strstr(b, "; 2 uploads (1.5 MB); 7 pools imported where bound (78.6 ms)") != nullptr);
    f.imports = 0;
    CHECK("no programs read: not named", !std::strstr(b, "programs read"));
    CHECK("no uploads left: not named", !std::strstr(b, "left for the next frame"));
    f.prog_reads = 12, f.prog_read_ms = 31.5, f.prog_ahead = 40, f.uploads_later = 3;
    sw::stall_line(f, b, sizeof b);
    CHECK("the uploads left for the next frame",
          std::strstr(b, "; 2 uploads (1.5 MB); 3 uploads of textures held before left for the next frame") != nullptr);
    CHECK("the programs read", std::strstr(b, "; 12 programs read from the cache by the draws (31.5 ms), 40 taken read ahead") != nullptr);
    // ((Triforce) why the draws read them, and those taken in the sessions' order: named only when there are some)
    CHECK("no why: not named", !std::strstr(b, "named by no order") && !std::strstr(b, "sessions' order"));
    f.why_unnamed = 5, f.why_let_go = 3, f.why_ahead = 4, f.order_taken = 25;
    sw::stall_line(f, b, sizeof b);
    CHECK("why the draws read them", std::strstr(b, "; 12 programs read from the cache by the draws (31.5 ms), 40 taken read ahead; of the "
                                                    "programs the draws read, 5 named by no order, 3 read ahead and let go before, 4 not "
                                                    "read ahead yet; 25 taken read ahead in the last sessions' order") != nullptr);
    f.why_unnamed = f.why_let_go = f.why_ahead = f.order_taken = 0;
    // ((Triforce, wp18) the pipelines new to the draws, those linked ahead and those the draws linked: named only when there are some)
    CHECK("no pipelines new: not named", !std::strstr(b, "new to the draws"));
    f.pipes_first = 412, f.pipes_ahead = 398, f.pipes_order = 395, f.pipes_links = 14, f.pipes_libs = 9;
    sw::stall_line(f, b, sizeof b);
    CHECK("the pipelines new to the draws", std::strstr(b, "; 412 pipelines new to the draws, 398 of them linked ahead (395 in the last "
                                                           "sessions' order), 14 linked by the draws or their jobs, 9 stage libraries "
                                                           "made for them from the caches") != nullptr);
    f.pipes_first = f.pipes_ahead = f.pipes_order = f.pipes_libs = 0;   // (links by a draw's job alone, a frame after its first need)
    sw::stall_line(f, b, sizeof b);
    CHECK("a link by a job alone", std::strstr(b, "; 0 pipelines new to the draws, 0 of them linked ahead (0 in the last sessions' "
                                                  "order), 14 linked by the draws or their jobs") != nullptr);
    f.pipes_links = 0;
    f.prog_reads = f.prog_ahead = f.uploads_later = 0, f.prog_read_ms = 0;
    // ((Triforce) the program picks' parts, the layered textures' parts, the views resolved anew)
    CHECK("none of them: not named", !std::strstr(b, "translated where asked") && !std::strstr(b, "shaders' lock") &&
                                         !std::strstr(b, "parts of layered") && !std::strstr(b, "resolved anew"));
    f.pick_inline = 4, f.pick_inline_ms = 61.0, f.pick_lock_ms = 12.5, f.pick_codes_ms = 3.0, f.split_parts = 1, f.view_lookups = 2400;
    sw::stall_line(f, b, sizeof b);
    CHECK("the program picks", std::strstr(b, "; 4 programs translated where asked (61.0 ms); the program picks waited 12.5 ms for "
                                              "the shaders' lock; the program picks read the stages' code 3.0 ms") != nullptr);
    CHECK("the parts and the views", std::strstr(b, "; 1 parts of layered textures' first uploads; 2400 texture views resolved anew") != nullptr);
    f.pick_inline = 0, f.pick_inline_ms = f.pick_lock_ms = f.pick_codes_ms = 0, f.split_parts = f.view_lookups = 0;
    // ((Triforce) the draws' waits for a program another draw was reading, the shaders' lock let go: program_read_once.h)
    sw::stall_line(f, b, sizeof b);
    CHECK("no waits for another draw's read: not named", !std::strstr(b, "another draw"));
    f.ra_waits = 1, f.ra_wait_ms = 0.9, f.once_waits = 3, f.once_wait_ms = 2.4;
    sw::stall_line(f, b, sizeof b);
    CHECK("the waits for another draw's read", std::strstr(b, "; 1 waits for programs being read ahead (0.9 ms); 3 waits for a program "
                                                              "another draw was reading (2.4 ms)") != nullptr);
    f.ra_waits = f.once_waits = 0, f.ra_wait_ms = f.once_wait_ms = 0;
    // ((Triforce, test8) the textures made, those not made yet, the images made)
    sw::stall_line(f, b, sizeof b);
    CHECK("none made: not named", !std::strstr(b, "textures made") && !std::strstr(b, "images made"));
    f.made = 120, f.made_ms = 31.0, f.made_later = 640, f.images = 118, f.images_ms = 22.5, f.targets = 3, f.targets_ms = 40.2;
    sw::stall_line(f, b, sizeof b);
    CHECK("the textures made", std::strstr(b, "; 120 textures made (31.0 ms), 640 not made yet; images made: 118 textures (22.5 ms), "
                                              "3 render targets (40.2 ms)") != nullptr);
    f.made = f.made_later = f.images = f.targets = 0, f.made_ms = f.images_ms = f.targets_ms = 0;
    // ((Triforce, test9) the queue submits beside a background import, the imports given back and sent ahead)
    sw::stall_line(f, b, sizeof b);
    CHECK("none beside an import: not named", !std::strstr(b, "background import") && !std::strstr(b, "given back"));
    f.qs_bg = 2, f.qs_bg_ms = 41.6, f.given_back = 64, f.ahead_sent = 16;
    sw::stall_line(f, b, sizeof b);
    CHECK("the submits beside an import", std::strstr(b, "; 2 queue submits while a background import ran (41.6 ms); 64 imports given "
                                                         "back, 16 ranges sent to be imported ahead") != nullptr);
    f.qs_bg = f.given_back = f.ahead_sent = 0, f.qs_bg_ms = 0;
    // ((Triforce) the hold of the last picture: new textures left for the next frame, and a frame not shown)
    CHECK("no new textures left, nothing held: not named", !std::strstr(b, "new textures") && !std::strstr(b, "not shown"));
    f.new_later = 41, f.held = true, f.hold_ms = 83.4;
    sw::stall_line(f, b, sizeof b);
    CHECK("the new textures left and the hold",
          std::strstr(b, "; 41 new textures left for the next frame; not shown: the last picture held (83 ms so far)") != nullptr);
    f.new_later = 0, f.held = false, f.hold_ms = 0;
    sw::Holds hs;
    hs.holds = 2, hs.frames = 14, hs.ms = 230.4, hs.longest = 180.2, hs.bounded = 1, hs.new_later = 312;
    sw::describe_holds(hs, 250.0, b, sizeof b);
    CHECK("the holds of 600 frames", !std::strcmp(b, "2 pictures held (14 frames, 230 ms in all, the longest 180 ms, 1 at the 250 ms "
                                                     "bound), 312 new textures left for the next frame"));

    // pipeline waits, the GPU behind; nothing of memory sync's when it is short
    double ms2[13] = {60.0, 0.0, 1.0, 0, 0, 0, 55.0, 0, 0, 0, 0.5, 0, 0};
    sw::Frame g;
    g.no = 7, g.interval = 60.5, g.replay = 60.0, g.phases = 13, g.names = names, g.ms = ms2;
    g.pipe_waits = 4, g.pipe_wait = 54.0, g.left_out = 2, g.gpu_bound = 3.5;
    sw::stall_line(g, b, sizeof b);
    std::printf("%s\n", b);
    CHECK("pipelines", std::strstr(b, "(draw 1.0, pipelines 55.0)") != nullptr);
    CHECK("pipeline waits", std::strstr(b, "; 4 waits for shaders or pipelines (54.0 ms), 2 draws left out") != nullptr);
    CHECK("the GPU behind", std::strstr(b, "; the GPU behind 3.5 ms") != nullptr);
    CHECK("no memory sync under 1 ms", !std::strstr(b, "memory sync"));
    CHECK("nothing outside under 1 ms", !std::strstr(b, "outside"));
    CHECK("no trial: not named", !std::strstr(b, "on trial"));

    // a pool's trial in guest memory under way (vk_memory.cpp demote_trials): named with its phase
    g.trial_phase = 1, g.trial_pool = 0x113f000000ull;
    sw::stall_line(g, b, sizeof b);
    std::printf("%s\n", b);
    CHECK("the trial", std::strstr(b, "; pool 0x113f000000 on trial (settling in guest memory)") != nullptr);
    g.trial_phase = 4;
    sw::stall_line(g, b, sizeof b);
    CHECK("its phase", std::strstr(b, "on trial (measured mirrored again)") != nullptr);
    g.trial_phase = 5;
    sw::stall_line(g, b, sizeof b);
    CHECK("no such phase: not named", !std::strstr(b, "on trial"));

    // a line never runs past its buffer, and ends in a 0
    char small[40];
    std::memset(small, 'x', sizeof small);
    const int n = sw::stall_line(f, small, sizeof small);
    CHECK("bounded", n < (int)sizeof small && small[sizeof small - 1] == 0 && std::strlen(small) == (size_t)n);
    std::puts("stall watch: ok");
    return 0;
}
