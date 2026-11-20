"""The performance overlay: the FPS counter row's third value, Full (no game, no GPU).

NX2navi's overlay has Off, FPS and Full; Triforce had the FPS counter alone. The FPS counter row now has Off, On and Full.
Full is NX2navi's full performance panel (the owner, 11-16: "add the old perf full overlay back from nx2navi", "the exact
same layout and design but themed using the in game assets to match the options menu", the name "changed" to "reflect the
current build of triforce", "semi transparent"): its rows in its order and formats, laid out by its metrics, at its place
(the top right corner); drawn with the options' own caption plate, separator and font from the player's game files, the
plate see-through; the title Triforce's build and the game's version (gpu/vk/vk_menu.cpp perf_sample and draw_perf). The row
keeps its name, so its key in the options file is the one it had.

Compiles and runs tests/test_perf_overlay.cpp: the frame times' unit (gpu/vk/perf_stats.h: the ring, the newest frames for
the graph, the percentiles, the 1% and 0.1% lows) and the machine's (gpu/vk/perf_machine.h/.cpp: the CPU's load, the GPU's
from Windows' counters, a number or its "-", and the sampler run for a GPU no vendor library has, whose temperature, power
and clock stay "-", one thread however often it starts, ended when stopped), and the figures per frame (perf_stats.h
per_frame). Compiles the title's and the frame limit's text out of vk_menu.cpp with several builds' names, the Frame
generation, FG and Upscaling texts with stand-ins for what runs, and the panel's text size baked at once (bake_perf, with a
stand-in font: printable ASCII and the degree sign, again for a new size or font). Reads vk_menu.cpp, vk_draw.cpp and
perf_machine.cpp for the rest: that an options file's 0 and 1 mean what they did, that nothing is measured unless the row is
on Full, where each number comes from (every wait for a pipeline in Pipeline waits, every wait for a translation, the dry
pass's too, in Shader waits; the waits' counters atomic), the panel's rows, metrics, scale, place (the exact top right corner,
no counter beside it, under an open menu), edge, theme and opacity, the text size baked ahead, and that the sampler's start,
stop and latest sample nothing, a thread the system will not make is no exception out of start, its thread at the lowest
priority and the vendor libraries let go when they have not the rendering GPU. tests/test_fps_overlay_order.py runs
menu_record and menu_record_image themselves: the panel in the counter's place on Full, under the options, and a bug
capture's picture taken before any of it. tests/test_draw_side_effects.py runs note_wait and draw_wait_split: each wait in
its counter.

    python -I -B tests/test_perf_overlay.py [--cxx g++]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def in_order(text, needles, why):
    at = -1
    for n in needles:
        i = text.find(n, at + 1)
        check(i > at, "%s: %r is missing or out of order" % (why, n))
        at = i


def check_source():
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    menu_h = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    ui_h = (ROOT / "gpu/vk/vk_ui.h").read_text(encoding="utf-8")
    machine_h = (ROOT / "gpu/vk/perf_machine.h").read_text(encoding="utf-8")
    machine = (ROOT / "gpu/vk/perf_machine.cpp").read_text(encoding="utf-8")
    # (the row: the field and the key it always had, 0 to 2, off by default; 0 and 1 are Off and the counter, unchanged: no
    # options migration)
    check('{TAB_DISPLAY, "FPS counter", &opts.show_fps, 0, 2, 1, tx_perf,' in menu,
          "the row is not the Display tab's FPS counter, 0 to 2, on opts.show_fps (its name is its key in the options file: fps_counter)")
    check('const char* tx_perf(int v) { return v == 1 ? "On" : v >= 2 ? "Full" : "Off"; }' in menu, "the row's values are not Off, On, Full")
    check("show_fps" not in block(menu, "const char* ini_key(const Row& r) {"), "the row's key is no longer its name's")
    check(re.search(r"\n    int show_fps = 0;", menu_h) is not None, "the overlay is not off by default")
    # ((Triforce) 11-17, the tabs in sections: the last row of the Display tab's Frame rate, before High dynamic range)
    check(menu.index('{TAB_DISPLAY, "VSync"') < menu.index('{TAB_DISPLAY, "FPS counter"') < menu.index('{TAB_DISPLAY, "High dynamic range"'),
          "the row is not where it was")
    # (the counter itself for 1 and for 2: menu_record's `fps` is the row not 0; the FPS-only mode as it was)
    # (Full is NX2navi's panel alone, in the counter's place (its perf_draw: mode 2 draws perf_full_draw and returns), and under
    # an open menu (its BringWindowToDisplayBack): draw_menu draws it over its dim, before its pages; the game tab's header over
    # it. tests/test_fps_overlay_order.py runs menu_record for the order)
    for name in ("void menu_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) {",
                 "void menu_record_image(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h) {"):
        record = block(menu, name)
        check("const bool fps = opts.show_fps && !start;" in record or "const bool fps = opts.show_fps && !startup_on.load(std::memory_order_acquire);" in record,
              "the counter's and the panel's switch is not the row")
        check("const bool full = fps && opts.show_fps == 2;" in record and "if (open) draw_menu(c, full);" in record and
              "if (full) draw_perf(c);" in record and "if (fps && !full) draw_fps(c, open || game_tab);" in record,
              "Full does not draw the panel alone, under the menu (%s)" % name[:24])
        check(record.index("if (full) draw_perf(c);") < record.index("if (game_tab) draw_game_tab(c);"), "the game tab's header is not over the panel")
        check(record.count("draw_perf(") == 1, "the panel is drawn twice")
    record = block(menu, "void menu_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) {")
    check(record.index("bugshot_record(") < record.index("ui::begin("), "a bug capture's picture is taken after the overlays could be drawn")
    dm = block(menu, "void draw_menu(ui::Ctx& c, bool perf_under) {")
    check(dm.index("ui::kDimAlpha));") < dm.index("if (perf_under) draw_perf(c);") < dm.index("const int vtx0 = c.d->VtxBuffer.Size"),
          "the panel is not over the menu's dim and under its pages (nor kept out of the column's move)")
    check("void draw_menu(ui::Ctx& c, bool perf_under = false);" in menu, "draw_menu's other callers ask for the panel")
    check("fps_drawn" not in menu, "the panel still moves for the counter, which Full does not draw")

    # (nothing measured unless the row is on Full: the frame times, the sample, the machine's sampler)
    note = block(menu, "void menu_note_frame(bool game_frame, int shown) {")
    check(re.search(r"if \(game_frame && opts\.show_fps == 2\) \{[^}]*perf_stats::note\(perf_ring, prof_now\(\)\);", note) is not None,
          "frame times are written while the overlay is not on Full")
    check(menu.count("perf_stats::note(") == 1, "frame times are written somewhere else too")
    tick = menu[menu.index("void menu_tick()"):menu.index("void menu_record(")]
    check("if (opts.show_fps == 2) perf_sample(shown, game);" in tick and menu.count("perf_sample(") == 2, "the panel's numbers are read while it is not shown")
    check("if (opts.show_fps != 2) perf_view.full = false, perf_view.have = false;" in tick, "frame times from before stay in a panel turned on again")
    check("const bool want = opts.show_fps == 2 && dev.props.deviceName[0];" in tick and "if (want) perf_machine_start();" in tick and
          "else perf_machine::stop();" in tick and menu.count("perf_machine_start()") == 2,
          "the machine's sampler does not run on Full alone (started and stopped in menu_tick)")
    sample = block(menu, "static void perf_sample(double shown, double game) {")
    check("if (!v.full) perf_ring = perf_stats::Ring{};" in sample, "a ring from an earlier time is not emptied")
    # (the machine's numbers: the sampler's latest, never a call that waits on the presenting thread)
    check("const perf_machine::Machine m = perf_machine::latest();" in sample, "the machine's numbers are not the sampler's")
    for call in ("PdhCollectQueryData", "GetSystemTimes", "nvml", "ADL2_", "__cpuid", "GlobalMemoryStatusEx", "K32GetProcessMemoryInfo"):
        check(call not in sample and call not in block(menu, "void draw_perf(ui::Ctx& c) {"),
              "the panel reads the machine itself (%s) on the presenting thread" % call)
    check("std::thread(sampler).detach();" in machine and "std::chrono::milliseconds(500)" in machine,
          "the machine is not sampled twice a second on a thread of its own (NX2navi's rate)")
    check('LoadLibraryExA(name, nullptr, bare ? LOAD_LIBRARY_SEARCH_SYSTEM32 : 0)' in machine and '"nvml.dll"' in machine and
          '"atiadlxx.dll"' in machine and '"pdh.dll"' in machine and "nvml" not in (ROOT / "nx2/build.py").read_text(encoding="utf-8"),
          "the vendor libraries and PDH are not loaded at run time from the system's folders (nothing linked)")
    # (the panel's rows, with NX2navi's names, in its order)
    check('constexpr const char* kPerfTop[] = {"GPU", "VRAM", "CPU", "RAM", "FPS", "FG", "Frame"};' in menu, "the top block's rows are not NX2navi's")
    check('constexpr const char* kPerfFrame[] = {"GPU frame prep", "GPU busy per frame", "Pipeline waits", "Shader waits", "Texture uploads"};' in menu,
          "the figures per frame are not NX2navi's")
    check('constexpr const char* kPerfSetting[] = {"Window", "Game image", "Upscaling", "Anti-aliasing", "Frame limit", "VSync",\n'
          '                                        "Widescreen", "Resolution", "HDR output", "Frame generation"};' in menu,
          "the settings' rows are not NX2navi's (its 3D scene is hidden on this game)")
    # (each cell: NX2navi's format, from what is measured anyway or the sampler)
    for needle in (
            'cell(v.top[0][0], "%.0f%%", m.gpu_pct);', 'cell(v.top[0][1], "%.0f \\xC2\\xB0" "C", m.gpu_temp_c);', 'empty(v.top[0][2]);',
            'const bool known = vram_usage(&used, &allowed) && used;', 'cell(v.top[1][0], "%.1f GB", gb(m.vram_used_mb));',
            'cell(v.top[1][1], "/ %.1f GB", gb(m.vram_total_mb));', 'if (known) cell(v.top[1][2], "game %.1f GB", (double)used / 1073741824.0);',
            'else cell(v.top[1][2], "%.0f MHz", m.gpu_clock_mhz);',
            'cell(v.top[2][0], "%.0f%%", m.cpu_pct);', 'cell(v.top[2][1], "%.0f \\xC2\\xB0" "C", m.cpu_temp_c);', 'cell(v.top[2][2], "game %.0f%%", m.cpu_game_pct);',
            'cell(v.top[3][0], "%.1f GB", gb(m.ram_used_mb));', 'cell(v.top[3][1], "/ %.1f GB", gb(m.ram_total_mb));', 'cell(v.top[3][2], "game %.1f GB", gb(m.ram_game_mb));',
            'second = perf_stats::summarize(perf_ring, 1000000000ll);', 'ten = perf_stats::summarize(perf_ring);',
            'cell(v.top[4][0], "%.0f", second.frames ? second.fps : game);', 'cell(v.top[4][1], "1%% low %.0f", ten.low1_fps);',
            'cell(v.top[4][2], "0.1%% low %.0f", ten.low01_fps);',
            'v.fg = perf_fg_running();', 'cell(v.top[kPerfFG][0], "%.0f", shown);',
            'cell(v.top[6][0], "%.1f ms", second.frames ? second.avg_ms : -1.0);', 'cell(v.top[6][1], "max %.1f ms", second.frames ? second.max_ms : -1.0);',
            'now.frames = game_frames.load(std::memory_order_relaxed);',
            'now.prep_ns = prof_ns[PROF_SUBMIT] + prof_ns[PROF_PRESENT], now.gpu_ns = gpu_time_busy_ns();',
            'texture_stats(&now.uploads, &astc, &bytes);', 'draw_wait_split(&now.pipe_us, &now.shader_us);',
            'if (perf_stats::per_frame(v.base, now, fresh, &f) != perf_stats::STEP_KEEP) {',
            'cell(v.frame[0], "%.2f ms", f.prep_ms);', 'cell(v.frame[1], "%.2f ms", f.gpu_ms);', 'cell(v.frame[2], "%.2f ms", f.pipe_ms);',
            'cell(v.frame[3], "%.2f ms", f.shader_ms);', 'cell(v.frame[4], "%.1f per frame", f.uploads);',
            'c.dim = perf_machine::cell_text(c.t, sizeof c.t, fmt, x);',
            'present_sizes.load(std::memory_order_relaxed)', 'set(2, perf_upscaling_text(t, sizeof t));', 'set(3, tx_aa(opts.aa));',
            'perf_rate_text(tx_fps_cap(opts.fps_cap), t, sizeof t);', 'set(5, tx_onoff(opts.vsync));', 'set(6, tx_ultrawide(opts.ultrawide));',
            'sscanf(tx_output_res(opts.output_res), "%ux%u", &ow, &oh) == 2 && oh) snprintf(t, sizeof t, "%up", oh);', 'set(8, tx_hdr(opts.hdr));',
            'set(9, perf_framegen_text(t, sizeof t));', 'perf_title(v.title, sizeof v.title);'):
        check(needle in sample, "the panel's text is not NX2navi's, or not from where it is measured: " + needle)
    # (Full turned on: the ring and the figures per frame start anew, the figures by perf_stats::per_frame (tests/test_perf_overlay.cpp))
    check(sample.index("const bool fresh = !v.full;") < sample.index("v.full = true;") and sample.count("per_frame(") == 1,
          "the figures per frame do not start anew when Full is turned on again")
    for gone in ("opts.framegen >= 1", "tx_framegen(opts.framegen)", "draw_stats3(", "v.frames0"):
        check(gone not in sample, "the panel reads the row, not what runs, or counts per frame by itself: " + gone)
    check('snprintf(v.gpu_name, sizeof v.gpu_name, "%s", m.gpu_name);' in sample and 'snprintf(v.cpu_name, sizeof v.cpu_name, "%s", m.cpu_name);' in sample,
          "the GPU's and the CPU's names are not the sampler's")
    check("perf_machine::Gpu g;" in menu and "g.has_luid = gpu_device_luid(g.luid);" in menu and
          "g.vram_total_mb = perf_machine::local_heap_mb(dev.memory.memoryHeaps, dev.memory.memoryHeapCount, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);" in menu,
          "the sampler is not told the rendering GPU (its name, LUID and device-local heaps)")
    # (the presenting thread's calls into the sampler only take its lock, set its flag, copy, or make its thread: none samples)
    for fn in ("void start(const Gpu& gpu, void (*log)(const char* line)) {", "void stop() {", "Machine latest() {", "int live_samplers() {"):
        body = block(machine, fn)
        for call in ("sample_os", "nvml_", "adl_", "pdh", "Pdh", "GetSystemTimes", "GetProcessTimes", "sources_", "cpu_brand", "Sleep", "sleep_for",
                     "wait_for", "LoadLibrary", "lib_open"):
            check(call not in body, "%s does more than lock, flag, copy and make the thread: %s" % (fn[:fn.index("(")], call))
    # (a thread the system will not make: std::system_error kept in start (it would reach menu_tick on the game's thread),
    # alive and live as they were, one line in the log, the numbers "-")
    start = block(machine, "void start(const Gpu& gpu, void (*log)(const char* line)) {")
    check(re.search(r"try \{\s*std::thread\(sampler\)\.detach\(\);\s*\} catch \(const std::system_error&\) \{\s*s\.alive = false;\s*"
                    r's\.live--;\s*note\("(?:[^"\\]|\\.)*"\);\s*\}\s*\}$', start) is not None and start.index("s.latest = Machine{};") < start.index("try {") and
          "#include <system_error>" in machine, "a failed thread start throws out of perf_machine::start, or leaves alive or live set")
    check(block(machine, "void start(const Gpu& gpu, void (*log)(const char* line)) {").count("std::thread(") == 1 and
          "if (s.alive) return;" in block(machine, "void start(const Gpu& gpu, void (*log)(const char* line)) {"),
          "a start while the sampler runs makes another thread")
    # (a vendor library with no device of the rendering GPU is shut down and let go; the thread runs below the game's)
    nvml = block(machine, "void nvml_open(const char* gpu) {")
    check('sym<int (*)()>(lib, "nvmlShutdown")' in nvml and "if (up && shutdown) shutdown();" in nvml and
          nvml.index("if (nv.ok) return;") < nvml.index("if (up && shutdown) shutdown();") < nvml.index("lib_close(lib);"),
          "NVML with no device of the rendering GPU is not shut down and let go")
    adl_fn = block(machine, "void adl_open(const char* gpu) {")
    check('sym<int (*)(void*)>(lib, "ADL2_Main_Control_Destroy")' in adl_fn and "if (up && destroy) destroy(adl.ctx);" in adl_fn and
          adl_fn.index("if (adl.ok) return;") < adl_fn.index("if (up && destroy) destroy(adl.ctx);") < adl_fn.index("lib_close(lib);"),
          "ADL with no PM log of the rendering GPU is not destroyed and let go")
    check("if (lib) FreeLibrary((HMODULE)lib);" in block(machine, "void lib_close(void* lib) {"), "lib_close does not free the library")
    check("SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);" in machine and "setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);" in machine
          and block(machine, "void sampler() {").startswith("void sampler() {\n    lowest_priority();"),
          "the sampler's thread is not at the lowest priority")
    # ((Triforce, 11-16) "The FPS counter doesn't show the frame generation FPS, and the base frame rate": DLSS frame generation's
    # frames are counted by the present's multiplier, and the counter writes the game's own rate beside them while it generates)
    replay = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    check("else if (!held) menu_note_frame(true, dlssg::multiplier());" in replay,
          "the present does not count DLSS frame generation's frames it shows")
    check("if (opts.framegen >= 1 || mkwvk::dlssg::multiplier() > 1) snprintf(fps_sub, sizeof fps_sub, \"%.0f\", game);" in menu,
          "the counter does not write the game's own rate with DLSS frame generation")
    check("int64_t gpu_time_busy_ns() { return st_gpu_ns + st_gpu_up_ns; }" in replay, "GPU busy per frame is not the recordings' and uploads' time")
    # (Pipeline waits: every wait for a pipeline, the whole one of the default WaitAll and pipe_wait_for's from the frame's budget;
    # Shader waits: the waits for a draw's and a dispatch's translation. Atomic microseconds: draws record on several threads)
    draw_c = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    check("std::atomic<int64_t> st_pipe_wait_us{0}, st_shader_wait_us{0};" in draw_c and "st_pipe_wait_ms" not in draw_c,
          "the overlay's wait counters are not atomic")
    check("std::atomic<int64_t> st_waits{0}, st_wait_us{0};" in draw_c and "st_wait_ms" not in draw_c and
          re.search(r"int64_t [^;]*\bst_waits\b[^;]*;", draw_c.replace("std::atomic<int64_t> st_waits{0}", "")) is None,
          "the waits and their time (draws record on several threads) are not atomic")
    note_w = block(draw_c, "void note_wait(std::chrono::steady_clock::time_point t0, std::atomic<int64_t>& into) {")
    check("st_waits.fetch_add(1, std::memory_order_relaxed);" in note_w and "st_wait_us.fetch_add(us, std::memory_order_relaxed);" in note_w and
          "into.fetch_add(us, std::memory_order_relaxed);" in note_w,
          "a wait is not counted with the waits and into the overlay's counter")
    check("*waits = st_waits.load(std::memory_order_relaxed);" in block(draw_c, "void draw_stats3(") and
          "*wait_ms = st_wait_us.load(std::memory_order_relaxed) / 1000.0;" in block(draw_c, "void draw_stats3("),
          "draw_stats3 does not give the waits and their time in ms as before")
    check("note_wait(t0, st_pipe_wait_us);" in block(draw_c, "bool pipe_wait_for(const std::atomic<bool>& ready, int64_t ns) {"),
          "pipe_wait_for's waits are not Pipeline waits")
    check("wait_for(pe->ready, st_pipe_wait_us);" in draw_c and "wait_for(cp->ready, st_shader_wait_us);" in draw_c and
          "note_wait(t0, st_shader_wait_us);" in block(draw_c, "void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {"),
          "the default path's pipeline waits, or the translations', are not in their own counter")
    # (the dry pass's waits for a translation are Shader waits too: the counter only, the wait as it was)
    for fn in ("void dry_mark_written(NvnState& s, bool compute, bool first) {", "bool dry_temporal_dispatch(NvnState& s) {"):
        check("note_wait(t0, st_shader_wait_us);" in block(draw_c, fn), "the dry pass's translation waits are not counted (%s)" % fn[:fn.index("(")])
    # (every place that counts a wait counts it into one of the two: none adds to the waits by itself; every jobs_wait counted)
    check(draw_c.count("st_wait_us.fetch_add") == 1 and draw_c.count("st_waits.fetch_add") == 1 and "st_waits++" not in draw_c and
          draw_c.count("note_wait(t0, st_pipe_wait_us)") == 1 and draw_c.count("note_wait(t0, st_shader_wait_us)") == 3 and
          draw_c.count("note_wait(t0, into)") == 1, "a wait is counted outside note_wait")
    waits = re.findall(r"\n[^\n/]*\bjobs_wait\([^)]*\);[^\n]*\n([^\n]*)", draw_c)
    check(len(waits) == 4 and all("note_wait(t0, " in nxt for nxt in waits), "a jobs_wait in vk_draw.cpp is not timed into a wait counter: %r" % waits)
    check("void draw_wait_split(int64_t* pipe_us, int64_t* shader_us);" in (ROOT / "gpu/vk/vk_draw.h").read_text(encoding="utf-8"),
          "vk_draw.h does not give the two counters")

    # (the drawing: NX2navi's layout by its metrics, its place, the options' theme)
    draw = block(menu, "void draw_perf(ui::Ctx& c) {")
    for needle, why in (
            ("const float k = perf_scale(c.w, c.h);", "NX2navi's scale"),
            ("const float px = perf_px(k), cell_y = 1.5f * k, gap = 2 * 7.0f * k, item = 12 * k, row = px + 2 * cell_y;",
             "NX2navi's text, cells and item spacing"),
            ("const float pad_x = 24 * k, pad_y = 20 * k, ds = 0.72f * k, div_h = 26 * ds, graph_h = 48 * k;", "NX2navi's padding, divider and graph"),
            ('{{"100%", "888.8 GB", "888", "888.8 ms", ""},', "NX2navi's first column's samples"),
            ('{"188 \\xC2\\xB0" "C", "/ 888.8 GB", "1% low 888", "max 8888.8 ms", ""},', "NX2navi's second column's samples"),
            ('{"", "8888 MHz", "game 100%", "game 888.8 GB", "0.1% low 888"}};', "NX2navi's third column's samples"),
            ('wait_value = std::max(tw("888.88 ms"), tw("888.8 per frame"))', "NX2navi's per-frame value column"),
            ("const float m = 12 * k, panel_w = std::ceil(inner + 2 * pad_x), panel_h = std::ceil(content + 2 * pad_y);", "NX2navi's margin"),
            ("const ImVec2 a(std::floor(c.w - m - panel_w), std::floor(m)), b(a.x + panel_w, a.y + panel_h);", "the exact top right corner"),
            ("ui::box(c, a, b, kPerfOpacity);", "the options' caption plate, see-through"),
            ("ImFont* font = c.f.body ? c.f.body : c.f.strong;", "the options' font (menu_ctx)"),
            ("const ImU32 label_col = ui::kAccent, value_col = ui::kInk, dim_col = ui::alpha(ui::kSoft, 0.8f);", "the options' gold and white"),
            ("ui::separator(dc, x0 - 4 * ds, x1 + 4 * ds, std::floor(y + div_h * 0.5f) + 0.5f, ui::rgba(196, 164, 112), ui::rgba(150, 126, 90));",
             "the options' separator as NX2navi's divider"),
            ("c.d->AddRectFilled(g0, g1, ui::rgba(0, 0, 0, 82));", "the graph's band, black at 0.32"),
            ("p0(g0.x + 14 * k, g0.y + 8 * k), p1(g1.x - 14 * k, g1.y - 8 * k)", "the graph inside its frame's padding"),
            ("n = perf_stats::newest(perf_ring, ms, kPerfGraph);", "the graph from the ring's newest frames"),
            ("c.d->AddPolyline(pts, kPerfGraph, label_col, ImDrawFlags_None, 1.0f);", "the graph's gold line, a pixel wide as PlotLines'"),
            ("const auto text = [&](ImVec2 p, ImU32 col, const char* t) { edged_text(c, font, px, p, col, t); };", "the texts' thin dark edge"),
            ("std::lock_guard lk(perf_mu);", "the ring under its lock")):
        check(needle in draw, "the panel is not drawn as NX2navi's in the options' look: " + why)
    check("constexpr int kPerfGraph = 120;" in menu, "the graph is not NX2navi's 120 frames")
    in_order(draw, ["text(ImVec2(x0, y), dim_col, v.title);", "divider();", "table(0, 4);", "divider();", "table(4, kPerfTopRows);",
                    "y += 4 * k + item;", "AddRectFilled(g0, g1", "y += graph_h + item + 2 * k + item;", "label(kPerfFrame[i])", "divider();",
                    "label(kPerfSetting[i])", "if (names) {", "divider();", "v.gpu_name), y += px + item;", "v.cpu_name);"],
             "the panel's parts are not in NX2navi's order")
    # (every text through the edge: none drawn bare; the edge the FPS counter's own)
    check(draw.count("AddText(") == 0 and "edged_text(c, f, px, ImVec2(x, ui::mid_top(f, px, y)), col, t);" in block(menu, "void draw_fps(ui::Ctx& c, bool under_banner) {"),
          "the panel's texts, or the counter's, are not drawn with the one edge")
    edge = block(menu, "void edged_text(ui::Ctx& c, ImFont* f, float px, ImVec2 p, ImU32 col, const char* t) {")
    check("const float e = std::max(1.0f, px * 0.05f);" in edge and "ui::rgba(20, 14, 6, 150)" in edge, "the edge is not the HUD's thin soft one")
    # (the scale: NX2navi's menu_scale with its DPI floor, 720 lines at the window's display scale; prebake bakes the panel's
    # text at it while the row is on Full)
    scale = block(menu, "float perf_scale(float w, float h) {")
    check('GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow")' in scale and "get(app_window())" in scale and
          "return std::max(std::min(h, w * 0.68f), 720.0f * dpi / 96.0f) / 1080.0f;" in scale, "the panel's scale has not NX2navi's DPI floor")
    check("float perf_px(float k) { return 25.5f * 0.72f * k; }" in menu, "the panel's text size")
    bake = block(menu, "void prebake(const ui::Ctx& c) {")
    check("const float perf = opts.show_fps == 2 ? perf_px(perf_scale(c.w, c.h)) : 0;" in bake and "perf == prebaked[3]" in bake and
          "if (perf > 0) px[baked++] = perf;" in bake and "ui::prebake(px, baked);" in bake,
          "the panel's text size is not baked ahead (its first frame and a resize would bake glyphs on the presenting thread)")
    # (ui::prebake only queues; the panel's size is baked at once, in prebake before its early return (a new size alone
    # changes prebaked[3]), with draw_perf's font: check_bake runs it)
    check(bake.index("bake_perf(c, perf);") < bake.index("perf == prebaked[3]") and bake.count("bake_perf(") == 1,
          "the panel's text size is not baked at once when it changes")
    check("ImFont* font = c.f.body ? c.f.body : c.f.strong;   // (draw_perf's)" in block(menu, "void bake_perf(const ui::Ctx& c, float px) {") and
          "ImFont* font = c.f.body ? c.f.body : c.f.strong;" in draw, "the panel's glyphs are baked in another font than draw_perf's")
    check(record.index("prebake(c);") < record.index("if (open) draw_menu(c, full);"), "the bake is not before the drawing")
    check("if (r == kPerfFG && !v.fg) continue;" in draw, "the FG row shows without frame generation")
    check("if (v.setting[i][0]) label(kPerfSetting[i])" in draw, "a setting without a value is not left out")
    # (no panel of its own making: the options' plate, separator and font; the game's art read at run time from the player's
    # files, never shipped)
    for gone in ("AddRectFilledMultiColor", "perf_curl", "ui::fonts()", "ui::flourish("):
        check(gone not in draw, "the panel draws a look of its own (%s), not the options'" % gone)
    tex = (ROOT / "gpu/vk/vk_game_tex.h").read_text(encoding="utf-8")
    names = tex[tex.index("static const char* const names[] = {"):]
    names = names[:names.index("};")]
    for art in ("SystemCaptionBg_00", "OptionSeparateMark_00", "SystemLine_03"):
        check('"%s"' % art in names, "the options' %s is not read from the player's game files" % art)
        check('art("%s")' % art in ui_h, "vk_ui.h does not draw with %s" % art)
    check("const ImVec2 b(std::floor(c.w - ui::lu(c, 30))" not in draw, "the panel is still the old one at the right edge, halfway down")
    # (see-through: the plate at the share the options dim the game by; text and dividers whole)
    check("constexpr float kPerfOpacity = ui::kDimAlpha / 255.0f;" in menu, "the panel's opacity is not the options' dim")
    dim = int(re.search(r"constexpr int kDimAlpha = (\d+);", ui_h).group(1))
    check(0.5 <= dim / 255 <= 0.9, "the panel is not see-through and still dark enough to read on (%.2f)" % (dim / 255))
    box = block(ui_h, "inline void box(Ctx& c, ImVec2 a, ImVec2 b, float opacity = 1) {")
    check("nine(c, t, a - ImVec2(m, m), b + ImVec2(m, m), 133, 133, alpha(IM_COL32_WHITE, opacity));" in box and
          "c.d->AddRectFilled(a, b, alpha(kBox, opacity));" in box, "the caption plate is not drawn at the opacity asked for")
    sep = block(ui_h, "inline void separator(Ctx& c, float x0, float x1, float y, ImU32 col = rgba(104, 80, 46), ImU32 line_col = 0) {")
    check("if (!line_col) line_col = col;" in sep and sep.count("line_col") >= 6, "the separator's lines do not take their tint")
    for t in ("alpha(label_col", "alpha(value_col", "alpha(ui::kInk", "alpha(ui::kAccent"):
        check(t not in draw, "the panel's text is see-through too")
    # (the credit, and NX2navi's notices)
    for text, where in ((menu[menu.index("/// (Triforce) The performance overlay on Full"):menu.index("void draw_perf(ui::Ctx& c) {")], "draw_perf"),
                        (machine_h, "perf_machine.h"), (machine, "perf_machine.cpp")):
        check("(Triforce: from NX2navi" in text, where + " does not credit NX2navi")
    check("Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors" in machine_h and "Permission is hereby granted" in machine_h and
          "Advanced Micro Devices" in machine_h, "the MIT notices of the code from NX2navi are missing")
    for doc, needle in (("readme.md", "| FPS counter | Off, On, Full: On is the frame rate in the top right corner; Full shows NX2navi's performance overlay there in its place (under the options while they are open)"),
                        ("docs/development.md", "| `fps_counter` | 0 Off, 1 On, 2 Full (NX2navi's full performance overlay in the counter's place")):
        check(needle in (ROOT / doc).read_text(encoding="utf-8"), doc + " does not say " + needle)


def check_title(cxx, tmp):
    """The title's text and the Frame limit's, compiled out of vk_menu.cpp: this build of Triforce (nx2/version.py's name
    without its sources' fingerprint) and the game's version, the one build Triforce makes it from"""
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    game = re.search(r'constexpr const char\* kGameVersion = "([0-9.]+)";', menu).group(1)
    make = (ROOT / "nx2/make_game.py").read_text(encoding="utf-8")
    zstd_h = (ROOT / "host/host_zstd.h").read_text(encoding="utf-8")
    build_id = re.search(r'BUILD_ID = "([0-9a-f]+)"', make).group(1)
    check("Game %s, build %s" % (game, build_id) in zstd_h,
          "the title's game version %s is not the build Triforce makes the game from (%s)" % (game, build_id))
    funcs = block(menu, "void perf_title(char* out, size_t n) {") + "\n" + block(menu, "const char* perf_rate_text(const char* cap, char* out, size_t n) {")
    for version, want in (("v0.4.0-test41 (source 1a2b3c4d)", "Triforce v0.4.0-test41, game " + game),
                          ("v0.4.0-254-g436f9ac-dirty (source 0badf00d)", "Triforce v0.4.0-254-g436f9ac-dirty, game " + game),
                          ("unknown (source 0badf00d)", "Triforce unknown, game " + game), ("v0.5.0", "Triforce v0.5.0, game " + game)):
        src = Path(tmp) / "title.cpp"
        src.write_text('#include <cstdio>\n#include <cstdlib>\n#include <cstring>\n#include <string>\n#define HOOKSHOT_VERSION "%s"\n'
                       'constexpr const char* kGameVersion = "%s";\n%s\n'
                       'int main() {\n    char t[96], r[48];\n    perf_title(t, sizeof t);\n    puts(t);\n'
                       '    for (const char* c : {"60 (the console\'s)", "Monitor\'s rate (144)", "Monitor\'s rate", "45"}) puts(perf_rate_text(c, r, sizeof r));\n'
                       '}\n' % (version, game, funcs), encoding="utf-8")
        exe = Path(tmp) / "title"
        subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.splitlines()
        check(out[0] == want, "the title of build %r is %r, not %r" % (version, out[0], want))
        check(out[1:] == ["60 FPS", "144 FPS", "Monitor's rate", "45 FPS"], "the Frame limit's texts: %r" % out[1:])


def check_labels(cxx, tmp):
    """The Frame generation and Upscaling settings' texts and the FG row's switch, compiled out of vk_menu.cpp with stand-ins
    for what runs: the generator by name while it makes frames (NVIDIA DLSS's multiplier, AMD FSR 4's library with its
    version, AMD FSR 3.1, Triforce's own with the row's value), Off while none does, whatever the row says; the game's own
    upscaler in words of its own way, the others as the rows name them"""
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    funcs = "\n".join(block(menu, f) for f in ("const char* perf_upscaling_text(char* out, size_t n) {", "bool perf_fg_running() {",
                                                "const char* perf_framegen_text(char* out, size_t n) {"))
    src = Path(tmp) / "labels.cpp"
    src.write_text(r'''#include <cstdio>
#include <cstring>
#include <string>
struct { int framegen = 0, resolution = 0; } opts;
namespace framegen_rows { enum { FSR4 = 9 }; }
namespace ur { enum { GAME = 0, DLSS = 1, DLAA = 2 }; }
int dlss_mult = 1, up = 0;
bool fg_on = false, fsr4_known = false, fsr3 = false;
const char* fsr4_version = "";
namespace mkwvk::dlssg { int multiplier() { return dlss_mult; } }
namespace fsr4fg { bool offered_known() { return fsr4_known; } const char* version() { return fsr4_version; } }
namespace fsr3fi { unsigned frames_wanted() { return fsr3 ? 1 : 0; } }
bool framegen_on() { return fg_on; }
const char* tx_framegen(int v) { static const char* t[] = {"Off", "Auto", "2x", "3x", "4x", "5x", "6x", "7x", "8x", "2x"}; return t[v]; }
int upscaler() { return up; }
bool resolution_auto() { return opts.resolution == 0; }
const char* tx_resolution(int v) { return up == ur::DLAA ? "Native (DLAA)" : v == 0 ? "Auto (dynamic)" : up == ur::DLSS ? "Quality (67%)" : "75% (1440x810)"; }
const char* tx_upscaler(int) { return up == ur::GAME ? "Game" : up == ur::DLSS ? "NVIDIA DLSS" : "NVIDIA DLAA"; }
''' + funcs + r'''
int main() {
    char t[48];
    struct Fg { int row, mult; bool on, known, fsr3; const char* ver; };
    const Fg cases[] = {{0, 1, false, false, false, ""}, {2, 1, false, false, false, ""}, {3, 3, false, false, false, ""},
                        {0, 2, false, false, false, ""}, {2, 1, true, false, true, ""}, {9, 1, true, true, false, "4.0.2"},
                        {9, 1, true, true, false, ""}, {9, 1, true, false, false, ""}, {4, 1, true, false, false, ""}, {1, 1, true, false, false, ""}};
    for (const Fg& c : cases) {
        opts.framegen = c.row, dlss_mult = c.mult, fg_on = c.on, fsr4_known = c.known, fsr3 = c.fsr3, fsr4_version = c.ver;
        printf("%s|%d\n", perf_framegen_text(t, sizeof t), (int)perf_fg_running());
    }
    for (int u : {0, 1, 2}) for (int r : {0, 3}) { up = u, opts.resolution = r; puts(perf_upscaling_text(t, sizeof t)); }
}
''', encoding="utf-8")
    exe = Path(tmp) / "labels"
    subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.splitlines()
    want = ["Off|0",                        # (nothing runs)
            "Off|0",                        # (the row at 2x, where frame generation cannot run: not by the row)
            "NVIDIA DLSS 3x|1",             # (DLSS frame generation at 3x, whatever the row)
            "NVIDIA DLSS 2x|1",
            "AMD FSR 3.1|1",
            "AMD FSR 4 library (4.0.2)|1",  # (NX2navi's fg4_label, with the version AMD's library runs)
            "AMD FSR 4 library|1",
            "Triforce's own, 2x|1",         # (AMD FSR 4 set where AMD's library cannot: the renderer's own at 2x)
            "Triforce's own, 4x|1",
            "Triforce's own, Auto|1",
            "Game's own (dynamic)", "Game's own, 75% (1440x810)",   # (the game's own upscaler: not "Game, Auto (dynamic)")
            "NVIDIA DLSS, Auto (dynamic)", "NVIDIA DLSS, Quality (67%)", "NVIDIA DLAA (native)", "NVIDIA DLAA (native)"]
    check(out == want, "the panel's Frame generation, FG and Upscaling texts: %r" % out)


def check_bake(cxx, tmp):
    """The Full panel's text size baked at once (vk_menu.cpp bake_perf, compiled out with a stand-in font that keeps what is
    baked): printable ASCII and the degree sign (U+00B0, which vk_imgui.cpp bake_some leaves out) at the size asked, in
    draw_perf's font; once for a size and font, again for a new size, a new font, and after the row has left Full"""
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    src = Path(tmp) / "bake.cpp"
    src.write_text(r'''#include <cstdio>
#include <set>
#include <vector>
typedef unsigned short ImWchar;
struct ImFontBaked {
    float px;
    std::set<ImWchar> glyphs;
    void* FindGlyph(ImWchar ch) { glyphs.insert(ch); return nullptr; }
};
struct ImFont {
    std::vector<ImFontBaked> made;
    ImFontBaked* GetFontBaked(float px) { made.reserve(16); made.push_back({px, {}}); return &made.back(); }
};
namespace ui {
struct Fonts { ImFont* body = nullptr; ImFont* strong = nullptr; };
struct Ctx { Fonts f; };
}
''' + block(menu, "void bake_perf(const ui::Ctx& c, float px) {") + r'''
void show(const char* step, const ImFont& f) {
    if (f.made.empty()) { printf("%s: none\n", step); return; }
    const ImFontBaked& b = f.made.back();
    bool ascii = true;
    for (int ch = 32; ch < 127; ch++) ascii = ascii && b.glyphs.count((ImWchar)ch);
    printf("%s: %d at %.2f, %d glyphs, ascii %d, degree %d\n", step, (int)f.made.size(), b.px, (int)b.glyphs.size(), (int)ascii,
           (int)b.glyphs.count(0x00B0));
}
int main() {
    ImFont body, strong, game;
    ui::Ctx c;
    c.f.body = &body, c.f.strong = &strong;
    bake_perf(c, 18.36f), show("first", body);
    bake_perf(c, 18.36f), show("again", body);
    bake_perf(c, 27.54f), show("resized", body);
    bake_perf(c, 0), show("off", body);
    bake_perf(c, 27.54f), show("on again", body);
    c.f.body = &game;
    bake_perf(c, 27.54f), show("game font", game);
    c.f.body = nullptr;
    bake_perf(c, 27.54f), show("strong", strong);
    c.f.strong = nullptr;
    bake_perf(c, 30), show("no font", strong);
}
''', encoding="utf-8")
    exe = Path(tmp) / "bake"
    subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.splitlines()
    want = ["first: 1 at 18.36, 96 glyphs, ascii 1, degree 1",
            "again: 1 at 18.36, 96 glyphs, ascii 1, degree 1",     # (the same size and font: nothing baked again)
            "resized: 2 at 27.54, 96 glyphs, ascii 1, degree 1",   # (a resize, a new DPI)
            "off: 2 at 27.54, 96 glyphs, ascii 1, degree 1",
            "on again: 3 at 27.54, 96 glyphs, ascii 1, degree 1",  # (Full again after Off: baked anew)
            "game font: 1 at 27.54, 96 glyphs, ascii 1, degree 1", # (the game's font, once read: its glyphs)
            "strong: 1 at 27.54, 96 glyphs, ascii 1, degree 1",    # (draw_perf's strong where there is no body)
            "no font: 1 at 27.54, 96 glyphs, ascii 1, degree 1"]   # (no font: nothing, no crash)
    check(out == want, "the panel's text size is not baked at once, with its degree sign: %r" % out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="perf-overlay-") as tmp:
        check_title(args.cxx, tmp)
        check_labels(args.cxx, tmp)
        check_bake(args.cxx, tmp)
        exe = Path(tmp) / "perf-overlay"
        libs = [] if sys.platform == "win32" else ["-pthread", "-ldl"]
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"),
                        str(ROOT / "tests/test_perf_overlay.cpp"), str(ROOT / "gpu/vk/perf_machine.cpp"), "-o", str(exe)] + libs, check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: NX2navi's full overlay in the options' look: its rows, numbers, title and sampler; the row's three values")


if __name__ == "__main__":
    main()
