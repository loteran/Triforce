/* (Triforce) The stalls in the log (vk_replay.cpp status_tick). A public benchmark of v0.4.0 had a 0.1 % low of 6 fps: frames
   of over 150 ms now and then, which the log's 120- and 600-present averages cannot show. So every 600 of the game's frames a
   line of how long they were, present to present and in the replay's own time, and for a frame over 50 ms one line of where
   its time went: the replay's phases, the memory sync's parts and its waits for write-backs still on the GPU, the waits for
   shaders and pipelines, the uploads, the waits for the GPU, the present and the game's own fence waits. At most three such
   lines a second and 1500 in a session (Bound): a load of many slow frames in a row says so in the 600-frame line instead.
   Plain functions: tests/test_stall_watch.cpp checks them. RX_VK_STALL_LOG=0: neither line; =<ms>: the stall line's
   threshold (default 50). */
#pragma once

#include <cstdint>
#include <cstdio>

namespace mkwvk::stall_watch {

constexpr int kEvery = 600;                                  // (the game's frames between two lines of frame times)
constexpr int kEdges = 4;
constexpr double kEdgeMs[kEdges] = {33.0, 50.0, 100.0, 250.0};   // (the counts of the frames over each)
constexpr double kStallMs = 50.0;                             // (a frame this long gets a line of its own)

/// The frames of one stretch: how many, the longest, and how many over each edge
struct Histogram {
    int64_t frames = 0;
    double longest = 0;
    int64_t over[kEdges] = {};
    void add(double ms) {
        frames++;
        if (ms > longest) longest = ms;
        for (int i = 0; i < kEdges; i++)
            if (ms > kEdgeMs[i]) over[i]++;
    }
    void reset() { *this = Histogram{}; }
};

/// "the longest 252.0 ms, 12 over 33 ms, 3 over 50, 1 over 100, 1 over 250"
inline int describe(const Histogram& h, char* out, size_t cap) {
    return snprintf(out, cap, "the longest %.1f ms, %lld over 33 ms, %lld over 50, %lld over 100, %lld over 250", h.longest,
                    (long long)h.over[0], (long long)h.over[1], (long long)h.over[2], (long long)h.over[3]);
}

/// At most per_second lines a second (a burst of that many after a quiet stretch), and at most total in a session; held: the
/// lines it refused since the last look (taken by held_since)
struct Bound {
    double per_second = 3.0, tokens = 3.0;
    int64_t last_ns = 0;
    int64_t total = 1500, told = 0, held = 0;
    bool take(int64_t now_ns) {
        if (last_ns) {
            tokens += (double)(now_ns - last_ns) / 1e9 * per_second;
            if (tokens > per_second) tokens = per_second;
        }
        last_ns = now_ns;
        if (told >= total || tokens < 1.0) {
            held++;
            return false;
        }
        tokens -= 1.0;
        told++;
        return true;
    }
    int64_t held_since() {
        const int64_t h = held;
        held = 0;
        return h;
    }
};

/// One frame's account, in milliseconds unless named a count: what the stall line says
struct Frame {
    int64_t no = 0;                 // (the game's frame, as the status line counts them)
    double interval = 0;            // (present to present)
    double replay = 0;              // (the replay's time for it: its submits and its present, on every thread that replays)
    int phases = 0;                 // (the replay's phases: names[i], ms[i]; the first two, submit and present, are the replay's own)
    const char* const* names = nullptr;
    const double* ms = nullptr;
    double maps = 0, capture = 0, collect = 0;   // (memory sync's parts: the pools' refresh, the capture taken, the writes collected)
    double wb_wait = 0;             // (waits for write-backs still on the GPU: the captures' late pages, and the reads and bindings of pages put off)
    int64_t wb_waits = 0, wb_ahead = 0, wb_behind_present = 0;   // (how many; the most submissions ahead of the GPU one waited for; how many waited behind a present's blit)
    int64_t wb_unsent = 0;          // ((Triforce) how many waited for a submission not on the queue yet: the replay's, not the GPU's work)
    double wb_unsent_ms = 0;
    int64_t wb_held = 0;            // ((Triforce, wp20) how many went on after the writer's recording was done on the GPU, and
    double wb_held_ms = 0;          //  that part of them: the submissions' timeline held by work stopped at a wait, wb_rec_wait.h)
    double pipe_wait = 0;          // (waits for shaders and pipelines)
    int64_t pipe_waits = 0, left_out = 0;   // (how many; draws left out while theirs were built)
    int64_t pipes_first = 0, pipes_ahead = 0, pipes_order = 0;   // ((Triforce, wp18) vk_draw.cpp graphics_pipeline: the pipelines the
    int64_t pipes_links = 0, pipes_libs = 0;                      //  draws needed first, those the warm had linked for them (of those
                                                                  //  the last sessions' order's, pipe_order.h), and the links and the
                                                                  //  stage libraries the draws or their jobs made from the caches)
    int64_t uploads = 0;
    double upload_mb = 0;
    double gpu_bound = 0;           // (the replay waited for a recording slot: the GPU behind)
    double present = 0;             // (the present's own time: the swapchain's image, its fence, the present call, Reflex's sleep)
    double present_wait = 0;        // ((Triforce) of it, the wait for the present before to go out: with the present thread
                                    //  (RX_VK_PRESENT_THREAD) the present call is that thread's, present the presenting thread's part)
    double game_wait = 0;           // (the game's fence waits, every thread)
    int64_t game_waits = 0;
    int64_t imports = 0;            // (vk_memory.cpp: pools imported where they were bound, on the replay's threads, and their time)
    double import_ms = 0;
    int64_t prog_reads = 0;         // (vk_shader.cpp: programs a draw read from the program cache itself, their time; and those it
    double prog_read_ms = 0;        //  took read ahead by the worker, program_readahead.h)
    int64_t prog_ahead = 0;
    int64_t uploads_later = 0;      // (vk_texture.cpp: textures uploaded before whose new upload was left for the next frame)
    int trial_phase = 0;           // (vk_memory.cpp demote_trials: a pool's trial in guest memory under way, 1 to 4; 0: none)
    uint64_t trial_pool = 0;
    int64_t new_later = 0;          // ((Triforce) vk_texture.cpp: new textures whose first upload was left for the next frame)
    bool held = false;              // ((Triforce) vk_replay.cpp, present_hold.h: this frame was not shown, the last picture held,
    double hold_ms = 0;             //  for so long by now)
    int64_t split_parts = 0;        // ((Triforce) vk_texture.cpp, layer_split.h: parts of new layered textures' first uploads)
    int64_t view_lookups = 0;       // ((Triforce) vk_draw.cpp texture_view_of: texture views resolved anew, not from its cache)
    double pick_lock_ms = 0, pick_codes_ms = 0;   // ((Triforce) vk_shader.cpp program_pick: waits for shader_mu, code reads,
    int64_t pick_inline = 0;        //  and programs translated where asked (compute: in the dispatch), their time)
    double pick_inline_ms = 0;
    int64_t once_waits = 0;         // ((Triforce) vk_shader.cpp program_cache_load, program_read_once.h: a draw's waits for a
    double once_wait_ms = 0;        //  program another draw was reading with the shaders' lock let go, their time)
    int64_t why_unnamed = 0, why_let_go = 0, why_ahead = 0;   // ((Triforce) vk_shader.cpp readahead_why: of prog_reads, those no
    int64_t order_taken = 0;        //  order named, those read ahead and let go before, those not read ahead yet; of prog_ahead,
                                    //  those taken read ahead in the sessions' order, program_order.h)
    int64_t made = 0, made_later = 0;   // ((Triforce) vk_texture.cpp texture_get: textures made, their time, those not made yet for
    double made_ms = 0;                 //  the budget (create_later); vk_vma.cpp: the images made, render targets apart, their time)
    int64_t images = 0, targets = 0;
    double images_ms = 0, targets_ms = 0;
    int64_t qs_bg = 0;              // ((Triforce) vk_replay.cpp submit_recording: queue submits made while the background thread
    double qs_bg_ms = 0;            //  imported, their time; vk_memory.cpp: imports given back for the RAM or the heap, and ranges
    int64_t given_back = 0, ahead_sent = 0;   //  sent to be imported ahead)
    double disp_pick_ms = 0, disp_pipe_ms = 0;   // ((Triforce, test19) vk_draw.cpp dispatch: the programs' picks (their reads, the
    int64_t disp_ready = 0, disp_made = 0;       //  waits for the read-ahead's) and the first layouts and pipelines; the first
    int64_t ra_waits = 0;                        //  dispatches that found their pipeline made, those that made one; vk_shader.cpp:
    double ra_wait_ms = 0;                       //  the draws' and dispatches' waits for a program the read-ahead was reading)
    double captures = 0;            // ((Triforce) vk_memory.cpp: the submit-time captures on the game's threads, the copies behind
                                    //  them made or waited for there included; outside the replay's submit time)
};

/// (Triforce) The holds of the last picture in a stretch of frames (present_hold.h), for the 600-frame line
struct Holds {
    int64_t holds = 0, frames = 0, bounded = 0, new_later = 0;
    double ms = 0, longest = 0;   // (the holds that ended in the stretch: their time in all, the longest)
};
/// "2 pictures held (14 frames, 230 ms in all, the longest 180 ms, 0 at the 250 ms bound), 312 new textures left for the next frame"
inline int describe_holds(const Holds& h, double bound_ms, char* out, size_t cap) {
    return snprintf(out, cap, "%lld pictures held (%lld frames, %.0f ms in all, the longest %.0f ms, %lld at the %.0f ms bound), %lld "
                    "new textures left for the next frame", (long long)h.holds, (long long)h.frames, h.ms, h.longest,
                    (long long)h.bounded, bound_ms, (long long)h.new_later);
}

/// A part worth naming: 1 ms or more (the line stays short; the 120-present lines have the rest)
inline bool worth(double ms) { return ms >= 1.0; }

/// The stall line: "vk: stall: frame 12345: 123.4 ms present to present; replay 98.1 ms (draw 5.1, memory sync 90.0 [capture
/// taken 89.9, of it 89.5 waiting for 1 write-back 3 submissions ahead of the GPU, 1 behind a present]); ..."
inline int stall_line(const Frame& f, char* out, size_t cap) {
    if (!cap) return 0;
    size_t k = 0;
    auto put = [&](const char* fmt, auto... a) {
        if (k >= cap) return;
        const int n = snprintf(out + k, cap - k, fmt, a...);
        if (n > 0) k += (size_t)n < cap - k ? (size_t)n : cap - k - 1;
    };
    put("vk: stall: frame %lld: %.1f ms present to present; replay %.1f ms", (long long)f.no, f.interval, f.replay);
    bool open = false;
    for (int i = 2; i < f.phases; i++) {   // (from draw on: submit and present hold them)
        if (!worth(f.ms[i])) continue;
        put("%s%s %.1f", open ? ", " : " (", f.names[i], f.ms[i]);
        open = true;
        const bool memsync = f.names[i][0] == 'm' && f.names[i][1] == 'e' && f.names[i][2] == 'm';   // ("memory sync")
        if (!memsync) continue;
        bool in = false;
        auto part = [&](const char* name, double ms) {
            if (!worth(ms)) return;
            put("%s%s %.1f", in ? ", " : " [", name, ms);
            in = true;
        };
        part("maps", f.maps);
        part("capture taken", f.capture);
        part("writes collected", f.collect);
        if (in) put("%s", "]");
    }
    if (open) put("%s", ")");
    // (the captures' late pages wait in "capture taken"; a binding or a read of a page put off waits in its own phase)
    if (f.wb_waits && worth(f.wb_wait)) {
        put("; %lld waits for write-backs still on the GPU %.1f ms (up to %lld submissions ahead of it", (long long)f.wb_waits,
            f.wb_wait, (long long)f.wb_ahead);
        if (f.wb_behind_present) put(", %lld of the waits behind a present's blit", (long long)f.wb_behind_present);
        if (f.wb_unsent) put(", %lld of them for a submission not on the queue yet, %.1f ms", (long long)f.wb_unsent, f.wb_unsent_ms);
        if (f.wb_held)   // ((Triforce, wp20) the timeline held past the writer's recording: wb_rec_wait.h)
            put(", %lld of them %.1f ms after their writer's recording was done (the timeline held by work stopped at a wait)",
                (long long)f.wb_held, f.wb_held_ms);
        put("%s", ")");
    }
    if (f.pipe_waits || f.left_out)
        put("; %lld waits for shaders or pipelines (%.1f ms), %lld draws left out", (long long)f.pipe_waits, f.pipe_wait,
            (long long)f.left_out);
    if (f.pipes_first || f.pipes_links)   // ((Triforce, wp18) in "pipelines" above)
        put("; %lld pipelines new to the draws, %lld of them linked ahead (%lld in the last sessions' order), %lld linked by the "
            "draws or their jobs, %lld stage libraries made for them from the caches", (long long)f.pipes_first,
            (long long)f.pipes_ahead, (long long)f.pipes_order, (long long)f.pipes_links, (long long)f.pipes_libs);
    if (f.uploads) put("; %lld uploads (%.1f MB)", (long long)f.uploads, f.upload_mb);
    if (f.uploads_later) put("; %lld uploads of textures held before left for the next frame", (long long)f.uploads_later);
    if (f.new_later) put("; %lld new textures left for the next frame", (long long)f.new_later);
    if (f.held) put("; not shown: the last picture held (%.0f ms so far)", f.hold_ms);
    if (f.imports) put("; %lld pools imported where bound (%.1f ms)", (long long)f.imports, f.import_ms);   // (in draw or dispatch above)
    if (f.prog_reads || f.prog_ahead)   // (in "programs" above)
        put("; %lld programs read from the cache by the draws (%.1f ms), %lld taken read ahead", (long long)f.prog_reads,
            f.prog_read_ms, (long long)f.prog_ahead);
    if (f.why_unnamed || f.why_let_go || f.why_ahead)   // ((Triforce) why the read-ahead had not read those)
        put("; of the programs the draws read, %lld named by no order, %lld read ahead and let go before, %lld not read ahead yet",
            (long long)f.why_unnamed, (long long)f.why_let_go, (long long)f.why_ahead);
    if (f.order_taken) put("; %lld taken read ahead in the last sessions' order", (long long)f.order_taken);
    if (f.pick_inline) put("; %lld programs translated where asked (%.1f ms)", (long long)f.pick_inline, f.pick_inline_ms);
    if (f.ra_waits) put("; %lld waits for programs being read ahead (%.1f ms)", (long long)f.ra_waits, f.ra_wait_ms);   // (in "programs")
    if (f.once_waits)   // ((Triforce) in "programs")
        put("; %lld waits for a program another draw was reading (%.1f ms)", (long long)f.once_waits, f.once_wait_ms);
    if (worth(f.disp_pick_ms) || worth(f.disp_pipe_ms) || f.disp_made || f.disp_ready)   // (in "dispatch" above)
        put("; the dispatches' program picks %.1f ms, their first layouts and pipelines %.1f ms (%lld made there, %lld found made)",
            f.disp_pick_ms, f.disp_pipe_ms, (long long)f.disp_made, (long long)f.disp_ready);
    if (worth(f.pick_lock_ms)) put("; the program picks waited %.1f ms for the shaders' lock", f.pick_lock_ms);
    if (worth(f.pick_codes_ms)) put("; the program picks read the stages' code %.1f ms", f.pick_codes_ms);
    if (f.split_parts) put("; %lld parts of layered textures' first uploads", (long long)f.split_parts);
    if (f.view_lookups) put("; %lld texture views resolved anew", (long long)f.view_lookups);   // (in "descriptors" above)
    if (f.made || f.made_later)   // (in "texture lookups" above)
        put("; %lld textures made (%.1f ms), %lld not made yet", (long long)f.made, f.made_ms, (long long)f.made_later);
    if (f.qs_bg) put("; %lld queue submits while a background import ran (%.1f ms)", (long long)f.qs_bg, f.qs_bg_ms);   // (in "queue submit")
    if (f.given_back || f.ahead_sent)
        put("; %lld imports given back, %lld ranges sent to be imported ahead", (long long)f.given_back, (long long)f.ahead_sent);
    if (f.images || f.targets)   // (wherever they were made: texture lookups, or a pass's targets in "commands")
        put("; images made: %lld textures (%.1f ms), %lld render targets (%.1f ms)", (long long)f.images, f.images_ms,
            (long long)f.targets, f.targets_ms);
    if (worth(f.gpu_bound)) put("; the GPU behind %.1f ms", f.gpu_bound);
    if (worth(f.present)) put("; present %.1f ms", f.present);
    if (worth(f.present) && worth(f.present_wait)) put(" (%.1f of it waiting for the present before on the present thread)", f.present_wait);
    if (f.game_waits && worth(f.game_wait)) put("; the game's fence waits %.1f ms in %lld", f.game_wait, (long long)f.game_waits);
    const double outside = f.interval - f.replay - f.present;
    if (worth(outside)) put("; outside the replay and the present %.1f ms", outside);
    if (worth(f.captures)) put("; the captures at the game's submits %.1f ms", f.captures);   // ((Triforce) mostly in "outside")
    // (the GPU's time while a pool both write is tried in guest memory is the trial's: its waits land in memory sync above)
    if (f.trial_phase >= 1 && f.trial_phase <= 4) {
        static const char* const what[4] = {"settling in guest memory", "measured in guest memory", "settling mirrored again",
                                            "measured mirrored again"};
        put("; pool %#llx on trial (%s)", (unsigned long long)f.trial_pool, what[f.trial_phase - 1]);
    }
    return (int)k;
}

} // namespace mkwvk::stall_watch
