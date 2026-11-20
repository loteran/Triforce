// gpu/vk/rtss_limit.cpp and gpu/vk/vk_pace.cpp against a fake RivaTuner Statistics Server: every call of theirs that
// would reach RTSS, its files, the registry or the system goes to the stand-in Sys below (its folder, its hooks library
// with the profile calls, its profile files, its shared memory, the state file beside the game) on a fake clock, and
// vk_pace.cpp's NVIDIA DLSS frame generation, frame generation and host calls go to stand-ins too. vk_pace.cpp is cut
// out of its source by tests/test_rtss_limit.py (pace_cut.inc), its worker thread left out: the scenarios poll the
// worker themselves, every 500 ms of the fake clock. A scenario per name, each in a process of its own:
//     test_rtss_limit <scenario>      test_rtss_limit --list
// (Triforce, after NX2navi's tests/rtss_worker_check.c, whose scenarios several of these are)
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// ---- what vk_pace.cpp takes from vk_internal.h, vk_dlssg.h, vk_framegen.h and vk_reflex.h
struct MkwVkHost {
    int64_t (*now_ns)(void);
};
namespace mkwvk {
extern const MkwVkHost* host;
void logf(const char* fmt, ...);
int framegen_multiplier();
namespace dlssg {
bool active();
int multiplier();
bool pace(int64_t period_ns);
} // namespace dlssg
bool reflex_pace(int64_t period_ns);   // ((Triforce) RX_VK_REFLEX_LIMIT: the renderer's own Reflex holds the cap)
} // namespace mkwvk

#include "rtss_limit.cpp"

static void test_worker_started();
#include "pace_cut.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the fake RTSS and the game around it
struct Prof {
    bool exists = false;
    uint32_t limit = 0, den = 1, time = 0;
    uint64_t stamp = 0;
};
struct Fake {
    uint64_t now = 100000000000ull;   // the clock, ns
    std::vector<std::string> log;
    // RTSS
    bool installed = true, dll_file = true, library_loads = true, has_den = true;
    bool running = true;
    bool writable = true;       // its Profiles folder takes a save (administrator rights)
    bool hooks_game = true;     // it hooks the game (lists it) ...
    uint64_t inject_at = 0;     // ... from this time on
    bool limiter_works = true;  // its limiter holds the presents to the limit it read
    bool counts = true;         // it measures periods of the game's frames
    uint32_t refuse_above = 1000;   // it refuses a FramerateLimit above this (an assumption: RTSS's own range is not known here)
    bool library = false;       // its hooks library is loaded
    std::map<std::string, Prof> files;   // Profiles\<name>.cfg
    Prof global{true, 0, 1, 0, 1};
    Prof loaded;
    bool update_asked = false;
    int64_t eff_mhz = 0;        // the limit RTSS holds the game's presents to, as the game's process last read it
    bool was_listed = false;
    uint64_t period_start = 0;
    uint32_t t0 = 0, t1 = 0, frames = 0, count = 0;
    // counts of every call of RTSS's kind
    int rtss_calls = 0, hooks_loads = 0, saves = 0, deletes = 0, sets = 0, updates = 0, loads = 0;
    std::vector<std::string> set_names, get_names, saved_names, called;
    // the state file beside the game
    bool state_exists = false, state_unreadable = false, state_unwritable = false;
    std::string state;
    int state_writes = 0;
    // the game
    int fg_mult = 0;            // framegen_multiplier()'s answer
    bool dlssg = false;
    int dlssg_mult = 2;
    bool reflex_row = false;
    int reflex_holds = 0;
    int64_t reflex_period = -1;   // the period Reflex was last asked with
    // ((Triforce) the renderer's own Reflex with RX_VK_REFLEX_LIMIT=1, without DLSS frame generation: reflex_pace)
    bool reflex_plain = false;
    int reflex_plain_holds = 0;
    int64_t reflex_plain_period = -1;   // the period it was last asked with
};
static Fake F;

static uint32_t ms() { return (uint32_t)(F.now / 1000000ull); }
static bool listed_now() { return F.running && F.hooks_game && F.now >= F.inject_at; }
static int64_t profile_mhz() {   // what RTSS reads for the game: its own profile, else Global
    const Prof& p = F.files.count("Triforce.exe") ? F.files["Triforce.exe"] : F.global;
    return p.limit ? (int64_t)p.limit * 1000 / (p.den ? p.den : 1) : 0;
}
static void rtss_tick() {   // RTSS's side, at each present and each look at its memory
    const bool l = listed_now();
    if (l && !F.was_listed) F.eff_mhz = profile_mhz(), F.period_start = F.now, F.count = 0;
    if (!l) F.t0 = F.t1 = F.frames = 0;
    F.was_listed = l;
    if (l && F.counts && F.now - F.period_start >= 1000000000ull) {
        F.t0 = (uint32_t)(F.period_start / 1000000ull), F.t1 = ms(), F.frames = F.count;
        F.count = 0, F.period_start = F.now;
    }
}
static void game_presents() {   // a present of the window, as RTSS's hook sees it
    rtss_tick();
    if (!listed_now()) return;
    F.count++;
    if (F.update_asked) F.update_asked = false, F.eff_mhz = profile_mhz();   // (the process reads its profile again)
}

// ---- the stand-in Sys
static uint64_t s_now() { return F.now; }
static uint64_t s_hosted() { return mkwvk::pace::presented_ns(); }
static void s_log(const char* line) { F.log.push_back(line); }
static bool s_exe(char* out, size_t n) { return snprintf(out, n, "Triforce.exe") < (int)n; }
static bool s_state_path(char* out, size_t n) { return snprintf(out, n, "game\\triforce_rtss_profile.txt") < (int)n; }
static bool is_state(const char* p) { return !strcmp(p, "game\\triforce_rtss_profile.txt"); }
static std::string prof_path(const std::string& name) { return "C:\\RTSS\\Profiles\\" + name + ".cfg"; }
static const Prof* prof_of_path(const char* p) {
    if (!strcmp(p, "C:\\RTSS\\Profiles\\Global")) return &F.global;
    for (auto& kv : F.files)
        if (prof_path(kv.first) == p) return &kv.second;
    return nullptr;
}
static char* s_read(const char* path, size_t* n) {
    std::string text;
    if (is_state(path)) {
        if (!F.state_exists || F.state_unreadable) return nullptr;
        text = F.state;
    } else {   // (RTSS's profile files, read as files)
        const Prof* p = prof_of_path(path);
        if (!p) return nullptr;
        char buf[200];
        snprintf(buf, sizeof buf, "[Framerate]\r\nLimit=%u\r\nLimitDenominator=%u\r\nLimitTime=%u\r\n", p->limit, p->den, p->time);
        text = buf;
    }
    char* t = (char*)malloc(text.size() + 1);
    memcpy(t, text.c_str(), text.size() + 1);
    *n = text.size();
    return t;
}
static int64_t s_size(const char* path) {
    if (is_state(path)) return F.state_exists ? (int64_t)(F.state_unreadable ? 40 : F.state.size()) : -1;
    return -1;
}
static bool s_write(const char* path, const void* data, size_t n) {
    if (!is_state(path) || F.state_unwritable) return false;
    F.state.assign((const char*)data, n), F.state_exists = true, F.state_writes++;
    return true;
}
static void s_delete(const char* path) {
    if (is_state(path) && !F.state_unwritable) F.state_exists = false, F.state.clear();
}
static bool s_install(char* out, size_t n) {
    F.rtss_calls++, F.called.push_back("install_dir");
    return F.installed && snprintf(out, n, "C:\\RTSS") < (int)n;
}
static uint64_t s_stamp(const char* path) {
    F.rtss_calls++, F.called.push_back(path);
    if (!strcmp(path, "C:\\RTSS\\RTSSHooks64.dll")) return F.dll_file ? 7 : 0;
    const Prof* p = prof_of_path(path);
    return p && p->exists ? p->stamp | 1 : 0;
}
static void s_shared(RtssApp* a) {
    F.rtss_calls++, F.called.push_back("shared");
    rtss_tick();
    memset(a, 0, sizeof *a);
    a->running = F.running;
    if (!listed_now()) return;
    a->listed = 1, a->flags = RTSS_API_VULKAN | (F.update_asked ? RTSS_UPDATE_ASKED : 0);
    a->t0 = F.t0, a->t1 = F.t1, a->frames = F.frames;
}
static bool s_hooks_load(const char* path) {
    F.rtss_calls++, F.hooks_loads++;
    CHECK(!strcmp(path, "C:\\RTSS\\RTSSHooks64.dll"), "the hooks library from RTSS's folder: %s", path);
    return F.library = F.library_loads;
}
static void need_library() { CHECK(F.library, "a profile call without RTSS's hooks library"); }
static void s_load(const char* name) {
    F.rtss_calls++, F.loads++, need_library();
    CHECK(name && *name && !strcmp(name, "Triforce.exe"), "a profile other than the game's loaded: '%s'", name ? name : "");
    F.loaded = F.files.count(name) ? F.files[name] : F.global;
}
static void s_save(const char* name) {
    F.rtss_calls++, F.saves++, need_library();
    F.saved_names.push_back(name);
    CHECK(!strcmp(name, "Triforce.exe"), "a profile other than the game's saved: '%s'", name);
    if (!F.writable) return;   // (the save fails silently)
    Prof p = F.loaded;
    p.exists = true, p.stamp = F.now;
    F.files[name] = p;
}
static void s_delete_profile(const char* name) {
    F.rtss_calls++, F.deletes++, need_library();
    CHECK(!strcmp(name, "Triforce.exe"), "a profile other than the game's removed: '%s'", name);
    if (F.writable) F.files.erase(name);
}
static bool s_get(const char* prop, uint32_t* v) {
    F.rtss_calls++, need_library();
    F.get_names.push_back(prop);
    if (!strcmp(prop, "FramerateLimit")) return *v = F.loaded.limit, true;
    if (!strcmp(prop, "FramerateLimitDenominator")) return F.has_den ? (*v = F.loaded.den, true) : false;
    return false;
}
static bool s_set(const char* prop, uint32_t v) {
    F.rtss_calls++, F.sets++, need_library();
    F.set_names.push_back(prop);
    if (!strcmp(prop, "FramerateLimit")) {
        if (v > F.refuse_above) return false;
        return F.loaded.limit = v, true;
    }
    if (!strcmp(prop, "FramerateLimitDenominator")) return F.has_den ? (F.loaded.den = v, true) : false;
    return false;
}
static void s_update() {
    F.rtss_calls++, F.updates++, need_library();
    F.update_asked = listed_now();
}
static const mkwvk::rtss::Sys fake_sys = {s_now,     s_hosted, s_log,  s_exe,    s_state_path,   s_read, s_size, s_write, s_delete, s_install,
                                          s_stamp,   s_shared, s_hooks_load, s_load, s_save, s_delete_profile, s_get, s_set, s_update};
const mkwvk::rtss::Sys* mkwvk::rtss::real_sys() { return &fake_sys; }

// ---- the stand-ins of vk_pace.cpp's own neighbours
static int64_t host_now() { return (int64_t)F.now; }
static const MkwVkHost the_host = {host_now};
const MkwVkHost* mkwvk::host = &the_host;
void mkwvk::logf(const char* fmt, ...) {
    char line[1000];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    F.log.push_back(line);
}
int mkwvk::framegen_multiplier() { return F.dlssg ? 0 : F.fg_mult; }
bool mkwvk::dlssg::active() { return F.dlssg; }
int mkwvk::dlssg::multiplier() { return F.dlssg ? F.dlssg_mult : 1; }
bool mkwvk::dlssg::pace(int64_t period_ns) {   // (Reflex holds the frame when its row is on, frames are generated and it is given a period)
    F.reflex_period = period_ns;
    const bool held = F.dlssg && F.reflex_row && F.dlssg_mult > 1 && period_ns > 0;
    F.reflex_holds += held;
    return held;
}
bool mkwvk::reflex_pace(int64_t period_ns) {   // (Reflex holds the frame with the switch, given a period)
    F.reflex_plain_period = period_ns;
    const bool held = F.reflex_plain && period_ns > 0;
    F.reflex_plain_holds += held;
    return held;
}
static int workers;
static void test_worker_started() { workers++; }

namespace rt = mkwvk::rtss;
namespace pc = mkwvk::pace;

// ---- the game's loop: frames, presents, and the worker's polls every 500 ms
struct Game {
    int cap = 60;
    int work_us = 2000;          // a frame's own work: past any cap, so that a frame nobody holds runs free
    bool movie = false, minimised = false;
    int refresh = 144;           // (Auto: the display's refreshes)
    double auto_carry = 0;
    uint64_t next_poll = 0;
    // what each frame saw
    double none_s = 0, two_s = 0, held_s[3] = {0, 0, 0};
    int frames = 0, last_owner = -1;
    bool strict = false;          // every frame from now on must have exactly one limiter
    int strict_bad = 0;
};
static void advance(Game& g, uint64_t to) {   // the clock to `to`, the worker polling on the way
    while (g.next_poll && g.next_poll <= to) {
        F.now = g.next_poll;
        if (workers) rt::poll();   // (the worker's thread, once vk_pace.cpp started it)
        g.next_poll += 500000000ull;
    }
    F.now = to;
}
static int presents_of(Game& g, uint64_t frame_ns) {
    if (F.dlssg) return F.dlssg_mult;
    if (F.fg_mult > 0) return F.fg_mult;
    if (F.fg_mult < 0) {   // Auto: the display's refreshes in the frame's time
        g.auto_carry += (double)frame_ns * g.refresh / 1e9;
        const int n = (int)g.auto_carry;
        g.auto_carry -= n;
        return n;
    }
    return 1;
}
// One frame of the game: the host asks who holds it (host/nn_vi.c pace_handed), waits itself or not, the frame's work,
// its presents as RTSS's limiter lets them go. Which limiters held it: exactly one is right
static void frame(Game& g) {
    if (!g.next_poll) g.next_poll = F.now + 500000000ull;
    const int64_t period = 1000000000ll / g.cap;
    pc::set_period_ns((int)period);
    pc::set_movie(g.movie);
    const int handed = pc::frame_start();
    const int who = pc::owner();
    CHECK(handed == (who != pc::GAME), "the host is told to step aside exactly when another limiter holds the frame");
    CHECK(!(who == pc::RTSS && F.reflex_period > 0), "Reflex was given a limit while RTSS holds the frame");
    CHECK(!(who == pc::RTSS && F.reflex_plain_period > 0), "the renderer's own Reflex was given a limit while RTSS holds the frame");
    CHECK(!(g.movie && F.reflex_plain_period > 0), "the renderer's own Reflex was given a limit while a movie plays");
    CHECK(pc::display_yields() == (who == pc::RTSS && F.fg_mult > 0 && !F.dlssg), "the display thread yields only to RTSS");
    const uint64_t base = (uint64_t)std::max<int64_t>(g.work_us * 1000ll, who == pc::GAME || who == pc::REFLEX ? period : 0);
    int n = presents_of(g, (uint64_t)period);
    const bool shows = !g.minimised;
    // RTSS's limiter: the frame's presents no faster than its limit
    const bool rtss_on = shows && listed_now() && F.limiter_works && F.eff_mhz > 0 && n > 0;
    const uint64_t rtss_ns = rtss_on ? (uint64_t)((double)n * 1e12 / (double)F.eff_mhz) : 0;
    const bool binds = rtss_on && rtss_ns > base + base / 2000 + 1000;
    const int waits = (who == pc::GAME) + (who == pc::REFLEX) + binds;
    const uint64_t d = std::max(base, rtss_ns);
    const double secs = d / 1e9;
    if (waits == 0) g.none_s += secs;
    if (waits > 1) g.two_s += secs;
    if (g.strict && waits != 1) g.strict_bad++;
    g.held_s[who] += secs;
    g.last_owner = who, g.frames++;
    const uint64_t start = F.now;
    for (int k = 0; k < n; k++) {
        advance(g, start + d * (uint64_t)(k + 1) / (uint64_t)n);
        if (shows) game_presents(), pc::presented();
    }
    advance(g, start + d);
}
static void run_for(Game& g, double secs) {
    const uint64_t end = F.now + (uint64_t)(secs * 1e9);
    while (F.now < end) frame(g);
}
static bool logged(const char* part) {
    for (auto& l : F.log)
        if (l.find(part) != std::string::npos) return true;
    return false;
}
static int logged_count(const char* part) {
    int n = 0;
    for (auto& l : F.log) n += l.find(part) != std::string::npos;
    return n;
}
static void dump_log() {
    for (auto& l : F.log) printf("  log: %s\n", l.c_str());
}
static std::string overlay() {
    char t[200];
    pc::limiter_text(t, sizeof t);
    return t;
}
static Prof game_prof() { return F.files.count("Triforce.exe") ? F.files["Triforce.exe"] : Prof{}; }

// A new process: rtss_limit.cpp's and vk_pace.cpp's state as at a start; RTSS's files and the state file stay
static void new_process(const char* hos_rtss) {
#ifdef _WIN32
    _putenv_s("HOS_RTSS", hos_rtss ? hos_rtss : "");
#else
    if (hos_rtss) setenv("HOS_RTSS", hos_rtss, 1); else unsetenv("HOS_RTSS");
#endif
    rt::reset_for_test();
    pc::test_reset();
    F.library = false, F.log.clear(), F.was_listed = false, F.eff_mhz = 0;
    F.rtss_calls = F.hooks_loads = F.saves = F.deletes = F.sets = F.updates = F.loads = 0;
    F.set_names.clear(), F.get_names.clear(), F.saved_names.clear(), F.called.clear();
    workers = 0;
}
static void exit_in_order() { pc::exiting(); }

// ---- the scenarios

// RTSS installed and running, the profile writable: the cap goes into a profile the game makes, RTSS holds the game, the
// game's own wait stands aside; the exit removes the profile and the state file
static void writes_and_holds() {
    new_process(nullptr);
    Game g;
    run_for(g, 1.0);
    CHECK(workers == 1, "the worker starts at the first frame with a period");
    CHECK(F.state_exists && F.state.find("rtss1 0 0 1 60 1 60 1 Triforce.exe") != std::string::npos,
          "the state file has the profile as it was (none) before the first change:\n%s", F.state.c_str());
    run_for(g, 5.0);
    Prof p = game_prof();
    CHECK(p.exists && p.limit == 60 && p.den == 1, "the profile holds 60/1 (%u/%u)", p.limit, p.den);
    CHECK(g.last_owner == pc::RTSS, "RTSS holds the frames");
    CHECK(logged("RivaTuner Statistics Server profile Triforce.exe created"), "the log says the profile was made");
    CHECK(logged("RivaTuner Statistics Server limits the game, with the game's limit of 60.000 fps; the game's own wait stands aside"),
          "the log says RTSS limits");
    CHECK(overlay() == "RivaTuner, at 60", "the overlay: '%s'", overlay().c_str());
    for (auto& n : F.set_names) CHECK(n == "FramerateLimit" || n == "FramerateLimitDenominator", "a property other than the limit's set: %s", n.c_str());
    for (auto& n : F.get_names) CHECK(n == "FramerateLimit" || n == "FramerateLimitDenominator", "a property other than the limit's read: %s", n.c_str());
    CHECK(F.global.limit == 0 && F.global.stamp == 1, "the global profile is never saved");
    exit_in_order();
    CHECK(!F.files.count("Triforce.exe") && !F.state_exists, "the exit removed the profile and the state file");
    CHECK(logged("removed, as before the game started"), "the log says so");
    const int saves = F.saves, deletes = F.deletes, sets = F.sets;
    run_for(g, 3.0);
    CHECK(!F.files.count("Triforce.exe") && F.saves == saves && F.deletes == deletes && F.sets == sets,
          "nothing reaches the profile after the exit");
    CHECK(g.last_owner == pc::GAME, "the game's own wait after the exit");
    if (fails) dump_log();
}

// The player's own profile with 30/1: the game's 120 goes in, and 30/1 comes back at the exit
static void player_profile() {
    F.files["Triforce.exe"] = Prof{true, 30, 1, 0, 5};
    new_process(nullptr);
    Game g;
    g.cap = 120;
    run_for(g, 6.0);
    CHECK(game_prof().limit == 120 && g.last_owner == pc::RTSS, "120 held (%u)", game_prof().limit);
    CHECK(logged("held a limit of 30/1; it gets that back when the game closes"), "the log says what comes back");
    exit_in_order();
    CHECK(game_prof().exists && game_prof().limit == 30 && !F.state_exists, "30/1 back, the state file gone");
}

// Profiles under Program Files without administrator rights: the save does not stick, read back, and the game's own
// wait holds; said once in the log and in the overlay; no write after it
static void readonly_run() {
    F.writable = false;
    new_process(nullptr);
    Game g;
    run_for(g, 8.0);
    CHECK(!F.files.count("Triforce.exe") && g.last_owner == pc::GAME, "nothing of the game's in RTSS, the game's wait holds");
    CHECK(logged_count("Run the game as administrator, or set the limit in RivaTuner yourself") == 1, "said once");
    CHECK(overlay() == "the game's own wait (run as administrator, or set the limit in RivaTuner)", "overlay: '%s'", overlay().c_str());
    CHECK(!F.state_exists, "the state file's line went with the failed write");
    const int saves = F.saves;
    g.cap = 90;
    run_for(g, 70.0);
    CHECK(F.saves == saves, "not tried again (%d saves)", F.saves - saves);
    CHECK(g.two_s == 0 && g.none_s == 0, "one limiter throughout (two %.2f s, none %.2f s)", g.two_s, g.none_s);
    exit_in_order();
    if (fails) dump_log();
}

// The same with the player's own limit in RTSS's Global profile at 50, under the game's 60: RTSS holds the game by it,
// the game's own wait stands aside; at 90, above the game's, the game's own wait holds and the log says two limiters
static void readonly_own() {
    F.writable = false, F.global.limit = 50;
    new_process(nullptr);
    Game g;
    run_for(g, 8.0);
    CHECK(g.last_owner == pc::RTSS, "the player's 50 holds the game");
    CHECK(logged("with your own limit of 50.000 fps in RivaTuner"), "the log says whose limit");
    CHECK(overlay() == "RivaTuner, at your own limit of 50", "overlay: '%s'", overlay().c_str());
    g.strict = true;
    g.movie = true;
    run_for(g, 3.0);
    g.movie = false;
    run_for(g, 3.0);
    CHECK(!g.strict_bad, "one limiter, a movie too (%d frames)", g.strict_bad);
    F.global.limit = 90, F.global.stamp++;
    g.strict = false;
    run_for(g, 4.0);
    CHECK(g.last_owner == pc::GAME, "90, above the game's 60: the game's own wait");
    CHECK(logged("two limiters: RivaTuner Statistics Server's own setting for the game"), "the log says two limiters");
    exit_in_order();
    F.global.limit = 0;
    if (fails) dump_log();
}

// A kill: the state file keeps the line; the next start puts the profile back from it before it changes anything
static void killed() {
    F.files["Triforce.exe"] = Prof{true, 30, 1, 0, 5};
    new_process(nullptr);
    Game g;
    run_for(g, 6.0);
    CHECK(game_prof().limit == 60 && F.state.find("rtss1 1 30 1 60 1 60 1 Triforce.exe") != std::string::npos, "killed with 60 in");
    new_process(nullptr);   // (no exit)
    Game h;
    h.cap = 144;
    bool seen_back = false;
    for (int i = 0; i < 400 && !seen_back; i++) {
        frame(h);
        seen_back = logged("had the game's limit of 60/1 from a run that did not close");
        if (seen_back) CHECK(game_prof().limit == 30, "30/1 back before anything of this run: %u", game_prof().limit);
    }
    CHECK(seen_back, "the next start put it back");
    run_for(h, 5.0);
    CHECK(game_prof().limit == 144 && F.state.find("rtss1 1 30 1 144 1 144 1 Triforce.exe") != std::string::npos,
          "then this run's own limit, with the player's 30 to put back:\n%s", F.state.c_str());
    exit_in_order();
    CHECK(game_prof().limit == 30 && !F.state_exists, "30 back at the exit");
    // the player changed the profile since: it stays theirs, the line goes
    new_process(nullptr);
    Game k;
    run_for(k, 6.0);
    new_process(nullptr);
    F.files["Triforce.exe"].limit = 75, F.files["Triforce.exe"].stamp++;
    Game m;
    run_for(m, 0.6);
    CHECK(logged("changed since a run that did not close") && game_prof().limit == 75, "the player's 75 left as it is");
    run_for(m, 5.0);
    exit_in_order();
    CHECK(game_prof().limit == 75 && !F.state_exists, "and 75 is what the exit puts back");
    // a kill of a run that made the profile: the next start removes it, a start that keeps RTSS out too
    F.files.erase("Triforce.exe");
    new_process(nullptr);
    Game a;
    run_for(a, 6.0);
    new_process(nullptr);
    Game b;
    F.fg_mult = -1;   // (the next start runs frame generation on Auto: RTSS kept out, the old limit still goes)
    run_for(b, 3.0);
    CHECK(!F.files.count("Triforce.exe") && !F.state_exists && logged("removed, as before that run"), "removed at a start kept out");
    F.fg_mult = 0;
    exit_in_order();
    if (fails) dump_log();
}

// The state file holds another executable's line: it stays through the run; a file that is there but does not read is
// left alone, and the run goes on with a line in the log
static void state_file() {
    F.state_exists = true, F.state = std::string(RTSS_SAVED_HEADER) + "rtss1 1 30 1 144 1 144 1 Other.exe\n";
    new_process(nullptr);
    Game g;
    run_for(g, 6.0);
    CHECK(F.state.find("Other.exe") != std::string::npos && F.state.find("Triforce.exe") != std::string::npos, "both lines");
    exit_in_order();
    CHECK(F.state_exists && F.state == std::string(RTSS_SAVED_HEADER) + "rtss1 1 30 1 144 1 144 1 Other.exe\n",
          "the other executable's line alone:\n%s", F.state.c_str());
    F.state_unreadable = true;
    const int writes = F.state_writes;
    new_process(nullptr);
    Game h;
    run_for(h, 6.0);
    CHECK(F.state_writes == writes && F.state_exists, "a file that does not read is not written");
    CHECK(logged("could not be written; a crash would leave the game's limit"), "the log says so");
    CHECK(h.last_owner == pc::RTSS, "the run goes on");
    exit_in_order();
    CHECK(!F.files.count("Triforce.exe"), "and the exit still puts the profile back");
    F.state_unreadable = false, F.state_exists = false;
}

// HOS_RTSS=0: nothing of RTSS's is called at all; a line a run that did not close left is only reported
static void hos_rtss_0() {
    F.state_exists = true, F.state = std::string(RTSS_SAVED_HEADER) + "rtss1 1 30 1 60 1 60 1 Triforce.exe\n";
    F.files["Triforce.exe"] = Prof{true, 60, 1, 0, 9};
    new_process("0");
    Game g;
    run_for(g, 10.0);
    CHECK(F.rtss_calls == 0, "%d calls of RTSS's kind, the first %s", F.rtss_calls, F.called.empty() ? "" : F.called[0].c_str());
    CHECK(workers == 0, "no worker");
    CHECK(g.last_owner == pc::GAME && game_prof().limit == 60 && F.state_exists, "the profile and the line as they were");
    CHECK(logged("HOS_RTSS=0: RivaTuner Statistics Server is kept out") &&
              logged("holds the game's limit of 60/1 for RivaTuner Statistics Server's profile Triforce.exe from a run that did not close"),
          "the log reports the line");
    CHECK(overlay() == "the game's own wait (HOS_RTSS=0)", "overlay: '%s'", overlay().c_str());
    exit_in_order();
    CHECK(F.rtss_calls == 0 && game_prof().limit == 60, "nothing at the exit either");
    F.files.erase("Triforce.exe"), F.state_exists = false;
    if (fails) dump_log();
}

// RTSS not installed, or installed and not running: said in plain words, once; the game's own wait
static void not_there() {
    F.installed = false;
    new_process(nullptr);
    Game g;
    run_for(g, 3.0);
    CHECK(logged("RivaTuner Statistics Server is not installed") && g.last_owner == pc::GAME, "not installed");
    CHECK(overlay() == "the game's own wait (RivaTuner is not installed)", "overlay: '%s'", overlay().c_str());
    F.installed = true, F.running = false;
    new_process(nullptr);
    Game h;
    run_for(h, 5.0);
    CHECK(logged_count("is installed but not running: start it and it limits the game's frames") == 1 && !F.hooks_loads, "not running");
    CHECK(overlay() == "the game's own wait (RivaTuner is not running: start it and it limits the game)", "overlay: '%s'", overlay().c_str());
    F.running = true;   // (the player starts RTSS)
    run_for(h, 6.0);
    CHECK(h.last_owner == pc::RTSS && game_prof().limit == 60, "started: it limits the game");
    exit_in_order();
    if (fails) dump_log();
}

// The limit written for each frame generation mode and multiplier: the cap times the frames shown for each of the
// game's with the fixed multipliers; Auto and DLSS frame generation keep RTSS out and put back what was written
static void framegen_limits() {
    for (int cap : {30, 45, 60, 90, 144, 240}) {
        for (int m : {2, 3, 4, 8}) {
            new_process(nullptr);
            F.fg_mult = m;
            Game g;
            g.cap = cap;
            run_for(g, 6.0);
            if (cap * m > (int)F.refuse_above) {   // (a limit past what this RTSS takes: refused, and the game's own wait holds it)
                CHECK(game_prof().limit == 0 && g.last_owner == pc::GAME && logged("refused a limit of"), "cap %d at %dx: refused", cap, m);
                exit_in_order();
                continue;
            }
            CHECK(game_prof().limit == (uint32_t)(cap * m) && g.last_owner == pc::RTSS, "cap %d at %dx: %u", cap, m, game_prof().limit);
            CHECK(pc::display_yields(), "the display thread yields");
            if (m == 2 && cap == 60) CHECK(overlay() == "RivaTuner, at 120 shown (60 x 2)", "overlay: '%s'", overlay().c_str());
            exit_in_order();
        }
    }
    new_process(nullptr);
    F.fg_mult = 0;
    Game g;
    run_for(g, 6.0);
    CHECK(game_prof().limit == 60, "60");
    F.fg_mult = -1;   // (the player picks Auto)
    run_for(g, 3.0);
    CHECK(!F.files.count("Triforce.exe") && g.last_owner == pc::GAME, "Auto: put back, the game's wait");
    CHECK(logged("frame generation on Auto"), "said");
    CHECK(overlay() == "the game's own wait (frame generation on Auto)", "overlay: '%s'", overlay().c_str());
    const int saves = F.saves + F.deletes;
    run_for(g, 20.0);
    CHECK(F.saves + F.deletes == saves, "Auto's frames per game frame change all the time: the profile is never touched");
    F.fg_mult = 0, F.dlssg = true, F.dlssg_mult = 3, F.reflex_row = true;
    run_for(g, 5.0);
    CHECK(!F.files.count("Triforce.exe") && g.last_owner == pc::REFLEX && F.reflex_period > 0, "DLSS frame generation: Reflex with its row");
    F.reflex_row = false;
    run_for(g, 2.0);
    CHECK(g.last_owner == pc::GAME, "and the game's wait without it");
    F.dlssg = false;
    run_for(g, 5.0);
    CHECK(game_prof().limit == 60 && g.last_owner == pc::RTSS, "frame generation off: RTSS again");
    exit_in_order();
    // HOS_RTSS_DLSSG=1: RTSS tries, at the cap times DLSS's multiplier, and Reflex is given no limit
    new_process(nullptr);
#ifdef _WIN32
    _putenv_s("HOS_RTSS_DLSSG", "1");
#else
    setenv("HOS_RTSS_DLSSG", "1", 1);
#endif
    pc::test_reset();
    F.dlssg = true, F.dlssg_mult = 2, F.reflex_row = true;
    Game d;
    run_for(d, 6.0);
    CHECK(game_prof().limit == 120 && d.last_owner == pc::RTSS && F.reflex_period == 0, "DLSS 2x under RTSS: 120, Reflex no limit");
    // generation paused (a menu): the multiplier 1 for 3 s, then back: no write, the game's wait meanwhile
    const int writes = F.saves;
    F.dlssg_mult = 1;
    run_for(d, 3.0);
    CHECK(d.last_owner == pc::GAME && F.saves == writes, "paused 3 s: the game's wait, the profile keeps 120");
    F.dlssg_mult = 2;
    run_for(d, 3.0);
    CHECK(d.last_owner == pc::RTSS && F.saves == writes, "back: RTSS, no write");
    // a multiplier that flickers every 0.7 s for a minute: one write at most
    for (int i = 0; i < 86; i++) F.dlssg_mult = i & 1 ? 1 : 2, run_for(d, 0.7);
    CHECK(F.saves - writes <= 1, "a flicker for a minute: %d writes", F.saves - writes);
    exit_in_order();
#ifdef _WIN32
    _putenv_s("HOS_RTSS_DLSSG", "");
#else
    unsetenv("HOS_RTSS_DLSSG");
#endif
    F.dlssg = false, F.reflex_row = false;
    if (fails) dump_log();
}

// (Triforce) RX_VK_REFLEX_LIMIT=1: without DLSS frame generation the renderer's own Reflex holds the cap (vk_reflex.cpp
// reflex_pace) where RivaTuner does not, a movie is the game's own wait's, and DLSS frame generation's Reflex is asked
// instead of it; without the switch (reflex_pace's answer false) the game's own wait, as before
static void plain_reflex() {
    F.running = false;
    new_process(nullptr);
    F.reflex_plain = true;
    Game g;
    run_for(g, 3.0);
    CHECK(g.last_owner == pc::REFLEX && F.reflex_plain_period == 1000000000ll / 60 && F.reflex_plain_holds > 100,
          "no RivaTuner: Reflex holds the cap, asked with its period");
    CHECK(overlay() == "NVIDIA Reflex", "overlay: '%s'", overlay().c_str());
    g.movie = true;
    run_for(g, 2.0);
    CHECK(g.last_owner == pc::GAME && F.reflex_plain_period == 0, "a movie: the game's own wait, Reflex given no limit");
    g.movie = false;
    run_for(g, 2.0);
    CHECK(g.last_owner == pc::REFLEX, "the movie over: Reflex again");
    F.running = true;   // (the player starts RTSS: it holds the game, and Reflex is given no limit)
    run_for(g, 6.0);
    CHECK(g.last_owner == pc::RTSS && F.reflex_plain_period == 0 && game_prof().limit == 60, "RivaTuner: it holds the game");
    F.running = false;
    run_for(g, 3.0);
    CHECK(g.last_owner == pc::REFLEX, "RivaTuner closed: Reflex again");
    F.dlssg = true, F.dlssg_mult = 2, F.reflex_row = true, F.reflex_plain_period = -1;
    run_for(g, 2.0);
    CHECK(g.last_owner == pc::REFLEX && F.reflex_period > 0 && F.reflex_plain_period == -1,
          "DLSS frame generation: its own Reflex, the renderer's not asked");
    F.dlssg = false, F.reflex_row = false;
    F.reflex_plain = false;   // (the switch unset: the answer is always false)
    g.strict = true;
    run_for(g, 3.0);
    CHECK(g.last_owner == pc::GAME && !g.strict_bad, "without the switch: the game's own wait, one limiter a frame");
    exit_in_order();
    if (fails) dump_log();
}

// RTSS hooks the game with the limit but its limiter does not hold the presents: the frames' own check gives them back
// at once, and RTSS is tried again a minute later, three times; at once when RTSS starts again or the cap changes
static void retry_run() {
    F.limiter_works = false;
    new_process(nullptr);
    Game g;
    run_for(g, 10.0);
    CHECK(g.last_owner == pc::GAME, "given back");
    CHECK(logged("RivaTuner Statistics Server is tried again in a minute"), "said");
    CHECK(g.none_s < 1.0, "the game ran past its cap for %.2f s only", g.none_s);
    run_for(g, 240.0);
    CHECK(logged_count("is tried again as the game's limiter (a minute after it was given up)") == RTSS_RETRIES, "tried %d times",
          logged_count("a minute after it was given up"));
    CHECK(g.none_s < 4.0 && g.two_s == 0, "past its cap for %.2f s in all, two limiters %.2f s", g.none_s, g.two_s);
    CHECK(logged("tried again when it starts again or another limit is wanted"), "then only on a change");
    F.limiter_works = true;
    g.cap = 90;
    run_for(g, 6.0);
    CHECK(logged("another limit is wanted") && g.last_owner == pc::RTSS && game_prof().limit == 90, "a new cap: tried at once, and it holds");
    F.limiter_works = false;
    run_for(g, 5.0);
    CHECK(g.last_owner == pc::GAME, "given back again");
    F.running = false;
    run_for(g, 2.0);
    F.running = true, F.limiter_works = true;
    run_for(g, 6.0);
    CHECK(logged("it was closed and runs again") && g.last_owner == pc::RTSS, "RTSS started again: at once");
    exit_in_order();
    if (fails) dump_log();
}

// RTSS lists the game with the limit and pictures go out but never shows its hold: 30 s, then the profile goes back
static void unproven_run() {
    F.counts = false;
    new_process(nullptr);
    Game g;
    run_for(g, 25.0);
    CHECK(game_prof().limit == 60 && g.last_owner == pc::GAME, "not yet");
    run_for(g, 10.0);
    CHECK(!F.files.count("Triforce.exe") && logged("for 30 s and never showed it holds it"), "put back after 30 s");
    CHECK(g.two_s == 0, "one limiter throughout");
    F.counts = true;
    run_for(g, 70.0);
    CHECK(g.last_owner == pc::RTSS, "tried again a minute later, and it holds now");
    exit_in_order();
    if (fails) dump_log();
}

// A want after the exit never reaches the profile; an exit before the first want neither
static void restore_race() {
    new_process(nullptr);
    exit_in_order();
    Game g;
    run_for(g, 5.0);
    CHECK(!F.files.count("Triforce.exe"), "an exit before the first frame: nothing written after it");
    new_process(nullptr);
    Game h;
    run_for(h, 6.0);
    exit_in_order();
    h.cap = 144;
    run_for(h, 5.0);
    CHECK(!F.files.count("Triforce.exe") && h.last_owner == pc::GAME, "after the exit: nothing, the game's wait");
}

// One limiter in every combination: caps (Monitor's 75 and those under 60 among them), each frame generation mode,
// Reflex's row on and off, a movie and a minimised window in each run, and each physical safeguard. Every frame is held
// by exactly one of RTSS, Reflex and the game's own wait; the moments where RTSS is being found out or given back are
// bounded, and from the run's last stretch on it is exactly one for every frame
enum { S_OK, S_NOT_INSTALLED, S_NOT_RUNNING, S_NOT_HOOKED, S_INJECT_LATE, S_READONLY, S_READONLY_OWN, S_READONLY_ABOVE, S_LIBRARY,
       S_NOT_HOLDING, S_UNPROVEN, S_COUNT };
static const char* const s_names[] = {"ok", "not installed", "not running", "not hooked", "hooked late", "read-only",
                                      "read-only, the player's 50", "read-only, the player's 400", "no library", "RTSS does not hold",
                                      "never shows its hold"};
static void one_limiter() {
    static const int caps[] = {30, 40, 45, 60, 75, 90, 120, 144, 165, 240};
    struct Mode { int fg, dlssg, mult; const char* name; };
    static const Mode modes[] = {{0, 0, 1, "off"}, {2, 0, 2, "2x"}, {3, 0, 3, "3x"}, {4, 0, 4, "4x"}, {8, 0, 8, "8x"},
                                 {-1, 0, 1, "Auto"}, {0, 1, 2, "DLSS 2x"}, {0, 1, 4, "DLSS 4x"}};
    int runs = 0;
    for (int sc = 0; sc < S_COUNT; sc++)
        for (const Mode& md : modes)
            for (int cap : caps)
                for (int reflex = 0; reflex < 2; reflex++) {
                    const bool slow = sc == S_NOT_HOLDING || sc == S_UNPROVEN;
                    if (slow && cap != 60 && cap != 144 && cap != 30) continue;
                    F = Fake{};
                    F.installed = sc != S_NOT_INSTALLED, F.running = sc != S_NOT_RUNNING, F.hooks_game = sc != S_NOT_HOOKED;
                    F.inject_at = sc == S_INJECT_LATE ? F.now + 8000000000ull : 0;
                    F.writable = sc != S_READONLY && sc != S_READONLY_OWN && sc != S_READONLY_ABOVE;
                    F.global.limit = sc == S_READONLY_OWN ? 50 : sc == S_READONLY_ABOVE ? 400 : 0;
                    F.library_loads = sc != S_LIBRARY, F.limiter_works = sc != S_NOT_HOLDING, F.counts = sc != S_UNPROVEN;
                    F.fg_mult = md.fg, F.dlssg = md.dlssg != 0, F.dlssg_mult = md.mult, F.reflex_row = reflex != 0;
                    new_process(nullptr);
                    Game g;
                    g.cap = cap;
                    run_for(g, 6.0);
                    g.movie = true;        // a movie (DLSS frame generation pauses for it)
                    if (F.dlssg) F.dlssg_mult = 1;
                    run_for(g, 3.0);
                    g.movie = false, F.dlssg_mult = md.mult;
                    run_for(g, 3.0);
                    g.minimised = true;    // a minimised window
                    run_for(g, 2.0);
                    g.minimised = false;
                    run_for(g, slow ? 250.0 : 8.0);
                    g.strict = true;
                    run_for(g, 4.0);
                    runs++;
                    const int64_t want = (int64_t)cap * 1000 * (md.fg > 0 ? md.fg : 1);
                    const int64_t own = (int64_t)F.global.limit * 1000;   // (the player's own limit, in RTSS's Global profile)
                    // (RTSS refuses a limit past refuse_above, 1000 here: 8x at 144 and more)
                    const bool rtss_can = !F.dlssg && md.fg >= 0 && (want <= (int64_t)F.refuse_above * 1000 || own);
                    const bool readonly_own = sc == S_READONLY_OWN || sc == S_READONLY_ABOVE;
                    const bool refused = want > (int64_t)F.refuse_above * 1000;
                    const bool rtss_expected = rtss_can && (((sc == S_OK || sc == S_INJECT_LATE) && !refused) || (readonly_own && own <= want));
                    const int expect = rtss_expected ? pc::RTSS : F.dlssg && reflex ? pc::REFLEX : pc::GAME;
                    // (RTSS kept out, frame generation on Auto or DLSS's, beside a limit the player set in RTSS itself: the
                    // game cannot take that limit out, and the log says there are two limiters)
                    const bool players_two = readonly_own && !rtss_can;
                    char what[160];
                    snprintf(what, sizeof what, "%s, %s, cap %d, Reflex row %s", s_names[sc], md.name, cap, reflex ? "on" : "off");
                    if (players_two) {
                        CHECK(!g.strict_bad || logged("two limiters: RivaTuner Statistics Server's own setting for the game"),
                              "%s: two limiters, not said", what);
                        g.strict_bad = 0, g.two_s = 0;
                    }
                    CHECK(!g.strict_bad, "%s: %d frames at the end without exactly one limiter", what, g.strict_bad);
                    CHECK(g.last_owner == expect, "%s: held by %d, not %d", what, g.last_owner, expect);
                    // (a limiter that was just given the frames: RTSS's own limit of the player's at 50 holds beside the
                    // game's wait until it is seen to hold, a second or two; the game's own wait settles a new limit
                    // while the old one is in the profile; RTSS that does not hold is found within a fraction of a second)
                    const double two_max = sc == S_READONLY_OWN ? 4.0 : 2.5, none_max = sc == S_NOT_HOLDING ? 4.0 : 0.05;
                    CHECK(g.two_s <= two_max, "%s: two limiters for %.2f s", what, g.two_s);
                    CHECK(g.none_s <= none_max, "%s: no limiter for %.2f s", what, g.none_s);
                    exit_in_order();
                    CHECK(!F.files.count("Triforce.exe") && !F.state_exists, "%s: the profile and the state file back at the exit", what);
                    if (fails > 20) {
                        dump_log();
                        return;
                    }
                }
    printf("%d runs\n", runs);
}

struct Scenario { const char* name; void (*fn)(); };
static const Scenario scenarios[] = {
    {"writes_and_holds", writes_and_holds}, {"player_profile", player_profile}, {"readonly", readonly_run},
    {"readonly_own", readonly_own},         {"killed", killed},                 {"state_file", state_file},
    {"hos_rtss_0", hos_rtss_0},             {"not_there", not_there},           {"framegen_limits", framegen_limits},
    {"retry", retry_run},                   {"unproven", unproven_run},         {"restore_race", restore_race},
    {"one_limiter", one_limiter},           {"plain_reflex", plain_reflex},
};

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--list")) {
        for (const Scenario& s : scenarios) printf("%s\n", s.name);
        return 0;
    }
    for (const Scenario& s : scenarios)
        if (argc == 2 && !strcmp(argv[1], s.name)) {
            s.fn();
            printf("%s: %s\n", s.name, fails ? "FAIL" : "ok");
            return fails != 0;
        }
    printf("test_rtss_limit <scenario> | --list\n");
    return 2;
}
