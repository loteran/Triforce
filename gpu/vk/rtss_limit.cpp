// (Triforce) RivaTuner Statistics Server as the game's frame limiter, after NX2navi's runtime/rtss_limit.c, whose
// worker this is: where RTSS is installed and running, the game's frame limit goes into RTSS's profile for this
// executable and, while RTSS is seen holding the game to it, the game's own wait steps aside (vk_pace.cpp), so there
// is one limiter. Anywhere else, or when any step fails, the game's own wait paces the frames as it always did.
//
// RTSS's hooks library (RTSSHooks64.dll in the folder its installer records) has the profile calls, loaded at run
// time: LoadProfile(name) reads a profile (the global one's values for a name without its own), Get/SetProfileProperty
// read and change the loaded copy, SaveProfile(name) writes it to Profiles\<name>.cfg, UpdateProfiles makes every
// hooked process read its profile again, DeleteProfile removes one. "FramerateLimit" is the limit in frames per
// second (0: none); "FramerateLimitDenominator" divides it. SaveProfile reports nothing: RTSS's Profiles folder sits
// under Program Files, which a process without administrator rights cannot write, and the save then fails silently.
// So every change is read back from the saved profile, and a limit that did not stick leaves the game's own wait in
// charge (and RTSS too, where the player set a limit of their own in it: rtss_own_decide).
//
// What is written, and it is all that is: the profile of this game's executable, by name, through those calls; of it
// the two properties above and nothing else; and it is put back. On every orderly exit (restore) the profile gets back
// what it held before the game changed it: the old limit, or no profile of its own when there was none. What it held
// is also kept in triforce_rtss_profile.txt in the game folder (rtss_decide.h RtssSaved), written before the run's
// first change of the profile and removed once the profile is back: an exit that skips the restore (a kill, a crash, a
// lost device) leaves the line, and the next start puts the profile back from it before it changes anything
// (left_start, restore_left), unless the player changed the profile since. No other profile is ever saved, not the
// global one; RTSS is never started or stopped; nothing else of it is changed.
//
// The game's own wait stands aside only once RTSS is seen to hold the game (rtss_decide.h rtss_decide), and takes the
// frames back whenever one of the signs ends. Everything here runs on the worker's thread, every half second, so a
// present never waits on the registry, a file or RTSS's library; want(), movie(), fast() and active() are the
// presenting thread's and only touch atomics.
//
// Everything outside this file is reached through rtss_limit.h's Sys: the tests run this file against a stand-in.
#include "rtss_limit.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace mkwvk::rtss {

namespace {

// The two properties of a profile this file ever sets, and the only ones it reads through the library.
constexpr const char* kLimit = "FramerateLimit";
constexpr const char* kDenominator = "FramerateLimitDenominator";
constexpr int kLimiterLines = 100;

struct St {
    const Sys* sys = nullptr;
    std::atomic<int64_t> want{RTSS_UNSET};        // millihertz from the pacing: >0 a limit, 0 none; RTSS_OUT_* kept out
    std::atomic<int64_t> active_for{RTSS_UNSET};  // the want RTSS holds the game to now; RTSS_UNSET none
    std::atomic<int> active_own{0};               // and it holds it by a limit of the player's own (rtss_own_decide)
    std::atomic<int64_t> movie_mhz{0};            // a movie's rate while one plays, 0 none
    std::atomic<int> fast{0};                     // the frames came too soon since the poll before (fast())
    std::atomic<int> closed{0};                   // restore ran: nothing reaches the profile any more
    std::atomic<int> ready{0};                    // the worker found RTSS: dll_path, rtss_dir and profile hold their values
    char dll_path[560] = "", profile[RTSS_NAME_MAX] = "", rtss_dir[520] = "", state_path[560] = "";
    int found = -1;               // the worker's look for RTSS: -1 not yet, 0 not installed, 1 found
    bool hooks = false;           // RTSS's hooks library is loaded
    RtssState S = RTSS_STATE_INIT;   // the worker's decisions (rtss_decide.h), under lock
    RtssOwnState own = RTSS_OWN_INIT;   // and those about the player's own limit
    int has_denominator = 0;      // this RTSS knows FramerateLimitDenominator
    int had_profile = 0;          // the profile existed before this run changed it
    uint32_t old_limit = 0, old_denominator = 1;
    int owed = 0;                 // this run changed the profile and has not put it back: the three above stay as found
    int creations = 0, removals = 0;   // the profile's "created" and "removed" lines this run, numbered
    RtssSaved rec{};              // this run's line in the state file, while the profile holds a value of this run
    std::atomic<int> recorded{0};   // the file holds rec
    int state_off = 0;            // a line this run could not act on stays as it is: no writes of the file
    int unrecorded_said = 0;
    // the worker's memory between polls
    int was_running = -1, was_listed = 0, left_done = 0, left_tried = 0;
    int said_not_running = 0, said_readonly = 0;
    // set_active's
    int logged = -1, lines = 0;
    int64_t logged_mhz = 0;       // (the limit of the hold logged last: a hold at another limit is logged too)
    char said[200] = "";
    // read_settings' and watch's
    RtssSettings settings{};
    uint64_t stamps[2] = {~0ull, ~0ull};
    char settings_file[RTSS_NAME_MAX + 24] = "";
    int two_said = 0, two_lines = 0, settings_lines = 0;
    char two_by[64] = "", two_text[160] = "", settings_said[RTSS_NAME_MAX + 200] = "";
    // what is in force, for view()
    std::atomic<int> v_limiter{GAME}, v_why{RTSS_WHY_UNSET}, v_fail{RTSS_FAIL_NONE}, v_looked{0}, v_installed{0}, v_running{0}, v_listed{0};
    std::atomic<int64_t> v_mhz{0};
};
St* st = new St;
std::mutex lock;   // the hooks calls and the worker's state

void say(const char* fmt, ...) {
    char line[900];
    const int head = snprintf(line, sizeof line, "vk: pace: ");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + head, sizeof line - (size_t)head, fmt, ap);
    va_end(ap);
    if (st->sys && st->sys->log) st->sys->log(line);
}

bool load_hooks() {
    St& s = *st;
    if (s.hooks) return true;
    if (!s.sys->hooks_load(s.dll_path)) {
        say("RivaTuner Statistics Server's %s did not load, or lacks the profile calls", s.dll_path);
        return false;
    }
    return s.hooks = true;
}

// The profile exists on its own: RTSS keeps it as Profiles\<name>.cfg in its folder. (RTSS's EnumProfiles would say
// the same, but NX2navi found it to crash a process whose heap lies above 4 GB: it is never called, nor looked up.)
bool profile_exists() {
    St& s = *st;
    char path[1100];
    snprintf(path, sizeof path, "%s\\Profiles\\%s.cfg", s.rtss_dir, s.profile);
    return s.sys->file_stamp(path) != 0;
}

// Writes limit/denominator into the profile and reads them back from the saved file: 1 when they stuck, 0 when RTSS
// refused a value, -1 when the save did not reach the file. RTSS checks each value against the other one already in
// the profile (the limit over the denominator must be a rate it accepts), so the limit goes to 0 (none) before the
// denominator changes.
int write_limit(uint32_t limit, uint32_t denominator) {
    St& s = *st;
    s.sys->load_profile(s.profile);
    if (!s.sys->set_property(kLimit, 0)) return 0;
    if (s.has_denominator && !s.sys->set_property(kDenominator, denominator)) return 0;
    if (limit && !s.sys->set_property(kLimit, limit)) return 0;
    s.sys->save_profile(s.profile);
    s.sys->load_profile(s.profile);
    uint32_t l = ~0u, d = 1;
    if (!s.sys->get_property(kLimit, &l)) return -1;
    if (s.has_denominator && !s.sys->get_property(kDenominator, &d)) return -1;
    return l == limit && (!s.has_denominator || d == denominator) ? 1 : -1;
}

// The state file with this profile's line set to r, or removed for r null; the file goes once it holds no line. Under
// lock: the worker, or restore's thread at the exit. True when the file holds what was asked.
bool state_put(const RtssSaved* r) {
    St& s = *st;
    if (s.state_off || !*s.state_path) return false;
    size_t n = 0;
    char* text = s.sys->file_read(s.state_path, &n);
    // a file that is there but did not read (another program holds it, a scan) keeps the lines of the other executables
    // in the folder: rewritten from nothing, they would go
    if (!text && s.sys->file_size(s.state_path) > 0) return false;
    const size_t cap = (text ? n : 0) + sizeof RTSS_SAVED_HEADER + 2 * (RTSS_NAME_MAX + 96);
    char* out = (char*)malloc(cap);
    int records = 0;
    bool ok = false;
    const size_t len = out ? rtss_saved_update(text ? text : "", s.profile, r, out, cap, &records) : 0;
    if (len && !records) {
        s.sys->file_delete(s.state_path);
        ok = s.sys->file_size(s.state_path) < 0;
    } else if (len) {
        ok = s.sys->file_write(s.state_path, out, len);
    }
    free(out);
    free(text);
    return ok;
}

// The state file did not take this run's line: a kill would then leave the game's limit in the profile for good.
// Logged once a run.
void unrecorded() {
    St& s = *st;
    s.recorded.store(0);
    if (!s.state_off && !s.unrecorded_said++)
        say("%s could not be written; a crash would leave the game's limit in RivaTuner Statistics Server's profile %s",
            *s.state_path ? s.state_path : "triforce_rtss_profile.txt", s.profile);
}

// Before a write of limit/denominator into the profile: rec's written value is that one, and the one written before it
// stays as the previous one (rtss_decide.h RtssSaved), so that a run that ends before the profile changed, or after,
// leaves a profile the next start tells from a value the player set since.
void record_before(uint32_t limit, uint32_t denominator) {
    St& s = *st;
    if (!s.recorded.load() || (s.rec.limit == limit && s.rec.den == denominator)) return;
    s.rec.prev_limit = s.rec.limit, s.rec.prev_den = s.rec.den;
    s.rec.limit = limit, s.rec.den = denominator;
    if (!state_put(&s.rec)) unrecorded();
}

// After that write: the profile holds the written value alone (landed), or kept the previous one.
void record_after(bool landed) {
    St& s = *st;
    if (!s.recorded.load() || (s.rec.limit == s.rec.prev_limit && s.rec.den == s.rec.prev_den)) return;
    if (landed) s.rec.prev_limit = s.rec.limit, s.rec.prev_den = s.rec.den;
    else s.rec.limit = s.rec.prev_limit, s.rec.den = s.rec.prev_den;
    if (!state_put(&s.rec)) unrecorded();
}

// Writes mhz into the profile (rtss_poll's RtssOps.apply): RTSS_WROTE_*. first: the run's first write, which records
// what to put back on exit, in the state file too before anything changes; clear: on a refusal, clear the limit of this
// run the profile holds.
int apply(void*, int64_t mhz, int first, int clear) {
    St& s = *st;
    if (first && !s.owed) {   // what to put back on exit; after a put back that did not land, the profile still owes it
        s.had_profile = profile_exists();
        s.sys->load_profile(s.profile);
        s.old_limit = 0, s.old_denominator = 1;
        s.sys->get_property(kLimit, &s.old_limit);
        s.has_denominator = s.sys->get_property(kDenominator, &s.old_denominator);
        if (!s.old_denominator) s.old_denominator = 1;
    }
    uint32_t limit, denominator;
    rtss_limit_value(mhz, s.has_denominator, &limit, &denominator);
    if (first) {   // the line goes in before the profile changes: a kill between the two finds it as it was
        const RtssSaved was = s.rec;   // still in the file after a put back that did not land: the profile holds its value
        const bool kept = s.recorded.load() != 0;
        s.rec = RtssSaved{s.had_profile, s.old_limit, s.old_denominator, limit, denominator, kept ? was.limit : limit,
                          kept ? was.den : denominator, ""};
        snprintf(s.rec.name, sizeof s.rec.name, "%s", s.profile);
        if (state_put(&s.rec)) s.recorded.store(1);
        else unrecorded();
    } else {
        record_before(limit, denominator);
    }
    const int r = write_limit(limit, denominator);
    if (r < 0) {   // the line keeps both values: the profile may hold either
        if (first && !s.owed && s.recorded.exchange(0)) state_put(nullptr);   // the profile is as it was
        return RTSS_WROTE_READONLY;
    }
    s.owed = 1;
    if (first) {
        if (!s.had_profile)   // numbered: a want that flips between a limit and kept out creates it again at each flip
            say("RivaTuner Statistics Server profile %s created; it goes away when the game closes (creation %d this run)",
                s.profile, ++s.creations);
        else
            say("RivaTuner Statistics Server profile %s held a limit of %u/%u; it gets that back when the game closes", s.profile,
                s.old_limit, s.old_denominator);
    }
    if (r == 0) {   // RTSS must not keep an earlier limit of this run under the game's own one
        say("RivaTuner Statistics Server refused a limit of %.3f fps", mhz / 1000.0);
        record_after(false);
        if (!clear) return RTSS_WROTE_REFUSED;
        record_before(0, 1);
        const bool cleared = write_limit(0, 1) > 0;
        record_after(cleared);
        if (!cleared) return RTSS_WROTE_REFUSED;
        s.sys->update_profiles();
        return RTSS_WROTE_REFUSED_CLEARED;
    }
    record_after(true);
    s.sys->update_profiles();
    if (!mhz) say("RivaTuner Statistics Server's profile %s has no limit", s.profile);
    else if ((int64_t)limit * 1000 == mhz * denominator)
        say("RivaTuner Statistics Server's profile %s limits to %.3f fps", s.profile, mhz / 1000.0);
    else   // rounded to whole frames by an RTSS without FramerateLimitDenominator: the log says what RTSS holds
        say("RivaTuner Statistics Server's profile %s limits to %u/%u (%.3f fps; the game asked %.3f)", s.profile, limit,
            denominator, (double)limit / denominator, mhz / 1000.0);
    return RTSS_WROTE_HELD;
}

// The profile as it was before this run changed it (the caller checked S.changed or owed, and marks S done). Its line
// in the state file goes once it is back; one that did not go back keeps the line, and owed, for the exit and the next
// start.
void put_back() {
    St& s = *st;
    bool back = true;
    if (!s.had_profile) {
        s.sys->delete_profile(s.profile);
        back = !profile_exists();
        if (back)
            say("RivaTuner Statistics Server profile %s removed, as before the game started (removal %d this run)", s.profile,
                ++s.removals);
        else say("RivaTuner Statistics Server profile %s did not go away", s.profile);
    } else if (write_limit(s.old_limit, s.old_denominator) > 0) {
        say("RivaTuner Statistics Server profile %s has its limit of %u/%u back", s.profile, s.old_limit, s.old_denominator);
    } else {
        say("RivaTuner Statistics Server profile %s did not take its old limit back", s.profile);
        back = false;
    }
    s.sys->update_profiles();
    if (back) s.owed = 0;
    if (back && s.recorded.exchange(0)) state_put(nullptr);
}

// This profile's line in the state file into r: true when there is one.
bool left_line(RtssSaved* r) {
    St& s = *st;
    size_t n = 0;
    char* text = *s.state_path ? s.sys->file_read(s.state_path, &n) : nullptr;
    const bool found = text && rtss_saved_find(text, s.profile, r);
    free(text);
    return found;
}

// The line r of this profile, left in the state file by a run that did not close (left_start): before this run changes
// anything, the profile gets back what it held before that run (rtss_restore_plan), unless the player changed it since,
// and the line goes. A profile that does not go back keeps the line, and this run leaves the file alone.
void restore_left(const RtssSaved* r) {
    St& s = *st;
    const bool exists = profile_exists();
    s.sys->load_profile(s.profile);
    uint32_t limit = 0, den = 1;
    s.sys->get_property(kLimit, &limit);
    s.has_denominator = s.sys->get_property(kDenominator, &den);
    if (!den) den = 1;
    const int plan = rtss_restore_plan(r, exists, limit, den);
    if (plan == RTSS_PLAN_DELETE) s.sys->delete_profile(s.profile);
    if ((plan == RTSS_PLAN_WRITE && write_limit(r->old_limit, r->old_den) <= 0) || (plan == RTSS_PLAN_DELETE && profile_exists())) {
        say("RivaTuner Statistics Server profile %s still has the game's limit of %u/%u from a run that did not close, and did "
            "not go back as it was; %s keeps its line", s.profile, limit, den, s.state_path);
        s.state_off = 1;
        return;
    }
    if (plan == RTSS_PLAN_WRITE || plan == RTSS_PLAN_DELETE) s.sys->update_profiles();
    if (plan == RTSS_PLAN_WRITE)
        say("RivaTuner Statistics Server profile %s had the game's limit of %u/%u from a run that did not close (%s); its limit "
            "of %u/%u is back", s.profile, limit, den, s.state_path, r->old_limit, r->old_den);
    else if (plan == RTSS_PLAN_DELETE)
        say("RivaTuner Statistics Server profile %s had the game's limit of %u/%u from a run that did not close (%s); removed, "
            "as before that run", s.profile, limit, den, s.state_path);
    else if (plan == RTSS_PLAN_LEAVE)
        say("RivaTuner Statistics Server profile %s changed since a run that did not close (%s); left as it is", s.profile,
            s.state_path);
    else
        say("RivaTuner Statistics Server profile %s is as it was before a run that did not close (%s)", s.profile, s.state_path);
    if (!state_put(nullptr)) {
        say("%s could not be written; its line for %s stays", s.state_path, s.profile);
        s.state_off = 1;
    }
}

// The state file's line for this profile, on the worker's first poll that finds RTSS running (w the want it read,
// hooks as RtssIn.hooks): true once the line is dealt with or there is none, false when a restore is still owed, for
// the first poll with the hooks library. A limit puts the profile back with the library it loads for its own writes. A
// kept-out want (frame generation on Auto, DLSS frame generation) writes nothing of its own, but a profile left with an
// old limit would hold the game beside the game's own wait: the library is loaded for that one restore.
bool left_start(int64_t w, int hooks) {
    RtssSaved r;
    if (!left_line(&r)) return true;
    if (hooks != 1 && (w >= 0 || !load_hooks())) return false;
    restore_left(&r);
    return true;
}

void put_back_op(void*, const RtssOut* o) {   // rtss_poll's RtssOps.put_back
    St& s = *st;
    if (o->why == RTSS_WHY_FAILED && s.S.fail == RTSS_FAIL_NOT_LIMITING)   // the frame count check gave up on RTSS
        say("RivaTuner Statistics Server measures %.1f fps with its limit at %.3f fps", o->fps, o->mhz / 1000.0);
    else if (o->why == RTSS_WHY_FAILED && s.S.fail == RTSS_FAIL_UNPROVEN)   // the lines before say which sign never came
        say("RivaTuner Statistics Server listed the game with its limit at %.3f fps for %d s and never showed it holds it",
            o->mhz / 1000.0, RTSS_UNPROVEN_POLLS / 2);
    else if (o->why == RTSS_WHY_FAILED)
        say("the game's frames came sooner than its limit while RivaTuner Statistics Server was to hold them at %.3f fps",
            o->mhz / 1000.0);
    put_back();
}

// Whether RTSS holds the game to the want w this poll read, and the log of which of the three is in force: RTSS with
// the game's limit, RTSS with the player's own limit, or the game's own wait and why. The first decision, then each
// change, up to kLimiterLines lines. The want may have changed since the poll read it: active() compares the two, so a
// stale store never lets the game's own wait stand aside for a newer want. The store is made either way, but a hold is
// logged only while its want is still the current one.
void set_active(int64_t w, int limiter, int64_t mhz, const char* why) {
    St& s = *st;
    const bool on = limiter != GAME;
    s.active_own.store(limiter == RTSS_OWN_LIMIT);
    s.active_for.store(on ? w : (int64_t)RTSS_UNSET);
    if ((on && s.want.load() != w) || (s.logged == limiter && (on ? mhz == s.logged_mhz : !strcmp(why, s.said)))) return;
    s.logged = limiter, s.logged_mhz = mhz;
    snprintf(s.said, sizeof s.said, "%s", on ? "" : why);
    if (++s.lines > kLimiterLines) return;
    if (limiter == RTSS_GAME_LIMIT)
        say("RivaTuner Statistics Server limits the game, with the game's limit of %.3f fps; the game's own wait stands aside",
            mhz / 1000.0);
    else if (limiter == RTSS_OWN_LIMIT)
        say("RivaTuner Statistics Server limits the game, with your own limit of %.3f fps in RivaTuner (the game could not put "
            "its own into RivaTuner's profile); the game's own wait stands aside", mhz / 1000.0);
    else say("the game's own wait limits the game (%s)", why);
    if (s.lines == kLimiterLines) say("later changes of the limiter in charge are not logged");
}

// RTSS's own settings for the game, read from its profile files as RTSS keeps them: Profiles\<exe>.cfg, else
// Profiles\Global, and a key the game's profile lacks from Global. Read as files, so no call goes into RTSS's hooks
// library; read again only when the time either was written changes. Worker thread only, while RTSS lists the game.
void read_settings() {
    St& s = *st;
    char own[1100], global[1100];
    snprintf(own, sizeof own, "%s\\Profiles\\%s.cfg", s.rtss_dir, s.profile);
    snprintf(global, sizeof global, "%s\\Profiles\\Global", s.rtss_dir);
    const uint64_t a = s.sys->file_stamp(own), b = s.sys->file_stamp(global);
    if (a == s.stamps[0] && b == s.stamps[1]) return;
    RtssProfile po{}, pg{};
    bool have_own = false, have_global = false;
    size_t n = 0;
    if (char* t = a ? s.sys->file_read(own, &n) : nullptr) rtss_profile_parse(t, n, &po), have_own = true, free(t);
    if (char* t = b ? s.sys->file_read(global, &n) : nullptr) rtss_profile_parse(t, n, &pg), have_global = true, free(t);
    if ((a && !have_own) || (b && !have_global)) return;   // (in use this moment: read again at the next poll)
    s.stamps[0] = a, s.stamps[1] = b;
    s.settings = rtss_settings_of(have_own ? &po : nullptr, have_global ? &pg : nullptr);
    if (a) snprintf(s.settings_file, sizeof s.settings_file, "Profiles\\%s.cfg", s.profile);
    else snprintf(s.settings_file, sizeof s.settings_file, "Profiles\\Global");
}

// While RTSS lists the game, the settings it holds for it are logged once per change. While RTSS has no part in the
// game's limit (rtss_out_of_limit) the game's own wait or Reflex holds the frames, and a limit the player set in RTSS
// itself for the game (its global profile or the game's own, a frame time limit or scanline sync) still holds them a
// second time, unless it is the one limiter in force (own_holds: rtss_own_decide). Logged once per change.
void watch(int64_t w, int fail, bool listed, bool own_holds) {
    St& s = *st;
    const bool out = rtss_out_of_limit(w, fail);
    bool on = false;
    const char* by = rtss_out_limiter(w);
    char text[160] = "";
    if (listed) {
        read_settings();
        char all[RTSS_NAME_MAX + 200], values[160];
        rtss_settings_text(values, sizeof values, &s.settings);
        snprintf(all, sizeof all, "(%s): %s", s.settings_file, values);
        if (strcmp(all, s.settings_said) && s.settings_lines++ < 40) say("RivaTuner Statistics Server's settings for the game %s", all);
        snprintf(s.settings_said, sizeof s.settings_said, "%s", all);
    }
    if (out && listed) {
        on = rtss_two(w, fail, listed, rtss_settings_limit(&s.settings)) && !own_holds;
        rtss_settings_text(text, sizeof text, &s.settings);
    }
    if (!out) s.two_said = 0;   // RTSS takes the game's limit: a later want out of it logs afresh
    else if ((int)on != s.two_said || (on && (strcmp(text, s.two_text) || strcmp(by, s.two_by)))) {
        if (s.two_lines++ < 40) {
            if (on)
                say("two limiters: RivaTuner Statistics Server's own setting for the game (%s: %s) limits it beside %s",
                    s.settings_file, text, by);
            else if (s.two_said) say("RivaTuner Statistics Server no longer limits the game beside %s", by);
        }
        s.two_said = on;
        snprintf(s.two_by, sizeof s.two_by, "%s", by);
        snprintf(s.two_text, sizeof s.two_text, "%s", text);
    }
}

// RTSS's folder, its hooks library and this executable's profile name, on the worker before its first poll. False
// when RTSS is not installed: the game's own wait for the run.
bool find_rtss() {
    St& s = *st;
    char dir[500];
    if (!s.sys->install_dir(dir, sizeof dir) || !*dir) {
        say("RivaTuner Statistics Server is not installed: the game's own wait limits the game");
        return false;
    }
    snprintf(s.dll_path, sizeof s.dll_path, "%s\\RTSSHooks64.dll", dir);
    snprintf(s.rtss_dir, sizeof s.rtss_dir, "%s", dir);
    if (!s.sys->file_stamp(s.dll_path)) {
        say("RivaTuner Statistics Server's folder %s has no RTSSHooks64.dll: the game's own wait limits the game", dir);
        return false;
    }
    // (RTSS names a profile after the executable, as Triforce.exe. Without a name nothing is ever loaded or saved: an
    // empty name is the global profile's)
    if (!s.sys->exe_name(s.profile, sizeof s.profile) || !*s.profile) return false;
    if (!s.sys->state_path(s.state_path, sizeof s.state_path)) s.state_path[0] = 0;
    const int64_t first = s.want.load();
    if (first >= 0 || first == RTSS_UNSET)
        say("RivaTuner Statistics Server found in %s; it takes the game's frame limit while it runs", dir);
    else
        say("RivaTuner Statistics Server found in %s; kept out (%s), and while it is kept out its thread only reads RTSS's own "
            "settings for the game, to tell when RTSS limits it a second time", dir, rtss_out_text(first));
    return true;
}

} // namespace

void start(const Sys* sys) { st->sys = sys; }

void want(int64_t mhz) {
    St& s = *st;
    if (s.closed.load(std::memory_order_relaxed)) return;   // the exit put the profile back
    // a new limit: the game's own wait holds it until the worker had RTSS apply it, up to half a second later:
    // active_for no longer matches
    s.want.store(mhz);
}

void movie(int64_t mhz) { st->movie_mhz.store(mhz, std::memory_order_relaxed); }

void fast() {
    St& s = *st;
    s.fast.store(1);
    s.active_for.store(RTSS_UNSET);   // the game's own wait from this frame on, until the worker says otherwise
}

bool active() {
    St& s = *st;
    if (s.closed.load(std::memory_order_relaxed)) return false;
    const int64_t a = s.active_for.load(std::memory_order_relaxed);
    return a > 0 && a == s.want.load(std::memory_order_relaxed);
}

bool own() { return st->active_own.load(std::memory_order_relaxed) != 0; }

bool poll() {
    St& s = *st;
    if (s.found < 0) {
        s.found = find_rtss() ? 1 : 0;
        s.v_installed.store(s.found), s.v_looked.store(1);
        if (s.found) s.ready.store(1, std::memory_order_release);   // restore may read the names now
    }
    if (!s.found) return false;
    const int64_t w = s.want.load();
    if (w == RTSS_UNSET) return true;
    std::lock_guard lk(lock);
    if (s.closed.load()) return true;   // a want read before restore must not reach the profile after it
    RtssIn in{};
    in.want = w, in.movie_mhz = s.movie_mhz.load(), in.now_ns = s.sys->now_ns(), in.hosted_ns = s.sys->hosted_ns();
    in.fast = s.fast.exchange(0);
    s.sys->shared(&in.app);
    if (in.app.running != s.was_running || in.app.listed != s.was_listed) {
        if (!in.app.running && !s.said_not_running++)
            say("RivaTuner Statistics Server is installed but not running: start it and it limits the game's frames; until then "
                "the game limits itself");
        else if (!in.app.running) say("RivaTuner Statistics Server is not running");
        else if (!in.app.listed) say("RivaTuner Statistics Server runs; it has not hooked the game yet");
        else say("RivaTuner Statistics Server has hooked the game");
        s.was_running = in.app.running, s.was_listed = in.app.listed;
    }
    in.hooks = s.hooks ? 1 : rtss_wants_hooks(&s.S, &in) ? (load_hooks() ? 1 : 0) : -1;
    if (!s.left_done && in.app.running && (in.hooks == 1 || !s.left_tried)) {   // before this run writes anything
        s.left_tried = 1;
        s.left_done = left_start(w, in.hooks);
        if (s.hooks) in.hooks = 1;   // (loaded for the restore alone)
    }
    in.current = s.want.load() == w;
    const RtssOps ops = {nullptr, apply, put_back_op};
    const int fail_before = s.S.fail;
    RtssOut o = rtss_poll(&s.S, &in, &ops);
    if (o.retry)
        say("RivaTuner Statistics Server is tried again as the game's limiter (%s)",
            o.retry == 2 ? "it was closed and runs again" : o.retry == 3 ? "another limit is wanted" : "a minute after it was given up");
    // (each run: the exit takes the game's limit out of the profile again, and a file under Program Files that an
    // administrator's run made is not a user's to change either)
    if (s.S.fail == RTSS_FAIL_READONLY && fail_before != RTSS_FAIL_READONLY && !s.said_readonly++)
        say("RivaTuner Statistics Server's profile %s did not keep the game's limit: its Profiles folder needs administrator "
            "rights. Run the game as administrator, or set the limit in RivaTuner yourself; until then the game limits "
            "itself, unless a limit of your own in RivaTuner already holds it", s.profile);
    if (rtss_fail_retried(s.S.fail) && !rtss_fail_retried(fail_before))
        say(s.S.retries < RTSS_RETRIES
                ? "the game's own wait limits the game for now; RivaTuner Statistics Server is tried again in a minute"
                : "the game's own wait limits the game; RivaTuner Statistics Server is tried again when it starts again or "
                  "another limit is wanted");
    // (Triforce) the game's limit cannot go into the profile (it cannot be written, RTSS's library did not load, or RTSS
    // refused this limit): RTSS may still hold the game by a limit of the player's own
    int limiter = o.holds ? RTSS_GAME_LIMIT : GAME, why = o.why;
    int64_t mhz = o.holds ? o.mhz : 0;
    char text[200];
    rtss_why_text(text, sizeof text, &o, s.S.fail);
    if (!o.holds && w > 0 && in.current &&
        (s.S.fail == RTSS_FAIL_READONLY || s.S.fail == RTSS_FAIL_LIBRARY || o.why == RTSS_WHY_REFUSED)) {
        if (in.app.listed) read_settings();
        RtssOwnIn oi{};
        oi.want = w, oi.limit_mhz = in.app.listed ? rtss_settings_mhz(&s.settings) : 0;
        oi.other = in.app.listed && rtss_settings_other(&s.settings);
        oi.movie_mhz = in.movie_mhz, oi.now_ns = in.now_ns, oi.hosted_ns = in.hosted_ns, oi.fast = in.fast, oi.app = in.app;
        const RtssOut oo = rtss_own_decide(&s.own, &oi);
        why = oo.why;
        if (oo.holds) limiter = RTSS_OWN_LIMIT, mhz = oi.limit_mhz;
        else {
            char base[200];
            snprintf(base, sizeof base, "%s", text);
            snprintf(text, sizeof text, "%s; %s", base, rtss_own_why_text(oo.why, s.own.fail));
        }
    }
    if (o.why != RTSS_WHY_NEWER) set_active(w, limiter, mhz, text);   // a newer want waits for the next poll
    watch(w, s.S.fail, in.app.listed != 0, limiter == RTSS_OWN_LIMIT);
    s.v_limiter.store(limiter), s.v_why.store(why), s.v_fail.store(s.S.fail), s.v_mhz.store(mhz);
    s.v_running.store(in.app.running), s.v_listed.store(in.app.listed);
    return true;
}

void restore() {
    St& s = *st;
    // The game presents on after this call (the rest of the exit runs first), so from here the game's own wait holds
    // the frames left and no want reaches the profile again: the worker would otherwise write the game's limit back
    // into the profile just put back, and the next run would take it for the player's own. Set before the check
    // below, so that an exit before the first limit also stops a later want from reaching the worker.
    s.closed.store(1);
    s.active_for.store(RTSS_UNSET);
    if (!s.ready.load(std::memory_order_acquire)) return;
    // the exit may come while the worker is inside RTSS's library: give up after a second rather than wait on it
    for (int i = 0; i < 100; i++) {
        if (lock.try_lock()) {
            if (s.hooks && (s.S.changed || s.owed)) put_back(), rtss_put_back_done(&s.S);
            lock.unlock();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void report_left() {
    St& s = *st;
    char name[RTSS_NAME_MAX], path[560];
    if (!s.sys || !s.sys->exe_name(name, sizeof name) || !*name || !s.sys->state_path(path, sizeof path)) return;
    size_t n = 0;
    char* text = s.sys->file_read(path, &n);
    RtssSaved r;
    if (text && rtss_saved_find(text, name, &r))
        say("%s holds the game's limit of %u/%u for RivaTuner Statistics Server's profile %s from a run that did not close; "
            "HOS_RTSS=0 leaves the profile as it is, and a start without it puts it back", path, r.limit, r.den, name);
    free(text);
}

View view() {
    St& s = *st;
    View v;
    v.limiter = s.closed.load() ? (int)GAME : s.v_limiter.load(), v.why = s.v_why.load(), v.fail = s.v_fail.load();
    v.want = s.want.load(), v.mhz = s.v_mhz.load();
    v.looked = s.v_looked.load() != 0, v.installed = s.v_installed.load() != 0, v.running = s.v_running.load() != 0;
    v.listed = s.v_listed.load() != 0;
    return v;
}

void reset_for_test() {
    delete st;
    st = new St;
}

} // namespace mkwvk::rtss
