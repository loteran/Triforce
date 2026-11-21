/* (Triforce) What the other threads were doing when one crashed (host/crash_threads.c; HOS_CRASH_THREADS=0: not taken).
   A crash report had the faulting thread alone: its registers, its guest frames, the memory three of its registers point to.
   The crash while a save loads (BaseProcCreateAndDeleteThread, the game's f_80119570 walking objects whose memory holds
   something else: -1, text, zeros; three in 105 lab runs, lab round 78) looks like an object freed and used again under
   that walk, and nothing in its reports says which thread freed it. So a report now starts by taking every other
   thread's host stack, each thread stopped only for its own unwind, on a helper thread while the faulting thread writes
   its own lines, and ends with one line a thread: the ones running game or host code first, with more frames, then the
   ones waiting in the system. Triforce.map turns the offsets into the game's functions (scripts/prof.py).
   Plain functions here: tests/test_crash_threads.py checks them. */
#ifndef HOS_CRASH_THREADS_H
#define HOS_CRASH_THREADS_H

#include <stdint.h>

enum {
    CRASH_THREADS_MAX = 384,      /* (threads in a snapshot at most: the game has about 120) */
    CRASH_THREADS_FRAMES = 16,    /* (frames kept of each stack) */
    CRASH_THREADS_WAIT_MS = 1500, /* (the report waits this long for the snapshot, then goes on without it) */
};

/* HOS_CRASH_THREADS: on unless it is 0 */
static inline int crash_threads_on(const char *env) { return !(env && env[0] == '0' && !env[1]); }

static inline int crash_threads_lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }
static inline int crash_threads_prefix(const char *s, const char *lower_prefix) {
    for (; *lower_prefix; s++, lower_prefix++)
        if (crash_threads_lower((unsigned char)*s) != *lower_prefix) return 0;
    return 1;
}
/* A thread whose top frame is in this module is waiting in the system (a sleep, a wait, a system call), not running the
   game's or the host's code: the report lists it after the others, with fewer frames. No module known ("?"): running */
static inline int crash_threads_waiting(const char *top_module) {
    return crash_threads_prefix(top_module, "ntdll") || crash_threads_prefix(top_module, "kernelbase") ||
           crash_threads_prefix(top_module, "kernel32") || crash_threads_prefix(top_module, "win32u") ||
           crash_threads_prefix(top_module, "user32");
}
/* The frames of a stack that go into its line: all kept for a running thread, 12 for a waiting one (the system's and the
   host's wait take the first four or five; the game's functions come after them), never more than it has */
static inline int crash_threads_shown(int frames, int waiting) {
    const int most = waiting ? 12 : CRASH_THREADS_FRAMES;
    return frames < 0 ? 0 : frames < most ? frames : most;
}

/* Whether `need` bytes at sp lie in a thread's stack [lo, hi). The walk of a stopped thread reads nothing outside it: a read
   there can fault, a fault goes through the host's handlers first (host/mkw_glue.c commit_on_touch commits a reserved guest
   page and logs it: the log's lock and the heap's, which the stopped thread may hold), and the report would hang where it
   should end the process. A frame the walk cannot follow inside the stack ends the walk */
static inline int crash_threads_in_stack(uint64_t sp, uint64_t need, uint64_t lo, uint64_t hi) {
    return sp >= lo && sp <= hi && need <= hi - sp;
}

/* Starts the snapshot (once a process; the calling thread is left out). Then the caller writes its own report */
void hos_crash_threads_begin(void);
/* Waits for the snapshot, CRASH_THREADS_WAIT_MS at most, and writes its lines */
void hos_crash_threads_end(void);

#endif
