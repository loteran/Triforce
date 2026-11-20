// (Triforce) RivaTuner Statistics Server as the game's frame limiter (rtss_limit.cpp), after NX2navi's
// runtime/rtss_limit.c: what the pacing asks of it (vk_pace.cpp), and the one interface through which anything of
// RTSS's, or of the system, is reached (Sys). The game's build gives the real one (rtss_sys_win.cpp); the tests give a
// stand-in with a fake hooks library, a fake shared memory and a fake clock, and never the real one.
#pragma once

#include <cstddef>
#include <cstdint>

#include "rtss_decide.h"

namespace mkwvk::rtss {

/// Everything rtss_limit.cpp asks for outside itself. Three kinds, and the third is the only one that changes
/// anything of RTSS's: its profile calls, which rtss_limit.cpp makes for the game's own profile alone.
struct Sys {
    // ---- the game's own: the clock, the log, the executable's name, and the state file beside the game
    uint64_t (*now_ns)();
    uint64_t (*hosted_ns)();                          // the clock after the newest present of the window, 0 none yet
    void (*log)(const char* line);
    bool (*exe_name)(char* out, size_t n);            // the executable's file name: what RTSS names its profile by
    bool (*state_path)(char* out, size_t n);          // triforce_rtss_profile.txt in the game folder
    char* (*file_read)(const char* path, size_t* n);  // a whole file, malloc'd and NUL-terminated; null: none, or it does not read
    int64_t (*file_size)(const char* path);           // -1: no such file
    bool (*file_write)(const char* path, const void* data, size_t n);   // the whole file, replaced in one step
    void (*file_delete)(const char* path);
    // ---- RTSS, read only
    bool (*install_dir)(char* out, size_t n);         // its folder, as its installer recorded it; false: not installed
    uint64_t (*file_stamp)(const char* path);         // a file's write time, 0: no such file (its library, its profile files)
    void (*shared)(RtssApp* a);                       // its shared memory as rtss_parse reads it for this process
    // ---- RTSS's hooks library: the profile calls, loaded at run time. A name is a profile's, never empty
    bool (*hooks_load)(const char* dll_path);
    void (*load_profile)(const char* name);           // reads a profile into the loaded copy (the global one's values for a name without its own)
    void (*save_profile)(const char* name);           // writes the loaded copy as Profiles\<name>.cfg; reports nothing
    void (*delete_profile)(const char* name);
    bool (*get_property)(const char* property, uint32_t* value);
    bool (*set_property)(const char* property, uint32_t value);
    void (*update_profiles)();                        // every hooked process reads its profile again
};

/// What is in force, for the log and the performance overlay's line.
enum Limiter { GAME = 0, RTSS_GAME_LIMIT = 1, RTSS_OWN_LIMIT = 2 };
struct View {
    int limiter = GAME;    // a Limiter
    int why = RTSS_WHY_UNSET;   // while it is the game's own wait: RTSS_WHY_*, of the game's limit or of the player's own
    int fail = RTSS_FAIL_NONE;
    int64_t want = RTSS_UNSET;  // what RTSS was asked to hold
    int64_t mhz = 0;       // the limit RTSS holds the game to, while it does
    bool looked = false;   // the worker has looked for RTSS
    bool installed = false, running = false, listed = false;
};

/// The interface, once, before anything else here is called.
void start(const Sys* sys);
/// Where the game's frame starts (the game's presenting thread): what RTSS is to hold, in millihertz of presented
/// frames, or RTSS_OUT_*. A new want is the game's own wait's until the worker has RTSS hold it. Makes no system call.
void want(int64_t mhz);
/// The rate of a movie that plays, 0 none: the game's own wait holds a movie.
void movie(int64_t mhz);
/// The frames RTSS was to hold came sooner than the game's period (rtss_watch_step): the game's own wait from now, and
/// the worker gives up on RTSS until it is tried again.
void fast();
/// RTSS holds the game to the want now: the game's own wait stands aside (with rtss_frame_holds).
bool active();
/// That hold is by a limit the player set in RTSS (the game's own could not be written): it holds a movie too, which
/// the game's own wait holds while RTSS has the game's limit.
bool own();
/// One pass of the worker (its thread calls it every half second; the first one looks for RTSS). False: there is
/// nothing more to do in this run (RTSS is not installed).
bool poll();
/// An orderly exit: the profile gets back what it held before the game changed it; nothing reaches it after.
void restore();
/// HOS_RTSS=0: RTSS is out entirely, and nothing of it is read or loaded. A line a run that did not close left in the
/// state file is only reported: the game's own file, read once.
void report_left();
View view();
/// The real interface (rtss_sys_win.cpp); null where there is no RTSS (not Windows).
const Sys* real_sys();
/// (tests) Everything as at the start of a run: the next start() begins a new one.
void reset_for_test();

} // namespace mkwvk::rtss
