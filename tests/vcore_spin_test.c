/* host/hos_thread.c's virtual cores, from vc_lock to vc_yield, included unchanged (vcore_block.inc, which
   tests/test_vcore_spin.py cuts from the source) and run on real threads: Windows' SRW lock and condition variable are
   pthreads here, the thread is the fields the block reads, and the log is printed. The clock is the monotonic one for the
   storms; the scripted cases run on the test's own clock (test_clock), which stands still unless the case moves it, and
   each step waits for the state it needs (a request made, a thread spinning or asleep, a spin left out), never for a
   time: what they check does not depend on how loaded the PC is. A watchdog ends a case that has not ended in 60 s
   (liveness only: a hand-off that never comes, a lock never let go, or a spin that never ends at its grant).

   Each case is a process of its own (the block reads its switches once):
   - storm, storm-off, storm-tight: eight threads of three priorities, some pinned to one core, hand three cores to each
     other tens of thousands of times (blocks, yields, the checkpoint a better waiter's request is seen at), with the spin
     (HOS_VCORE_SPIN_US, up to 8 spinning), without it (0), and with one spinner at most. No core is ever held by two
     threads, no thread ever holds two cores or runs without one, no waiter sleeps on a core it was granted (a grant made
     in the spin and slept through), no core is left owned or waiter queued at the end, and no escape is needed;
   - grant-in-spin: a waiter better than the holder asks it and spins (bound 1 s); the holder's 100 ms pass on the test's
     clock and it gives the core up; the wait ends in the spin at the grant (on that clock the bound never comes), and the
     holder's own wait after it sleeps;
   - grant-off: the same with HOS_VCORE_SPIN_US=0, the old behaviour: both waits sleep, nothing is spun;
   - default: the bound unset spins 250 us (the clock moved until the spin has passed it and the waiter sleeps), then the
     holder gives the core up to the sleeping waiter;
   - small-pc: 8 logical processors and HOS_VCORE_SPIN_MAX unset: no spin at all (off, not left out);
   - cap: HOS_VCORE_SPIN_MAX=1: two waiters each ask a holder; while the first spins (on the test's clock it spins until
     its grant), the second comes to the spin and is left out: never two spinning at once, the count given back;
   - cap-default: vc_spin_most for 4 to 64 logical processors;
   - yield-fast: a yield with no waiter takes no lock; one with a request left (vc_kick) takes it and clears the request;
     one with a waiter as good hands it the core.
   The vcores: lines are printed (LOG:) for the test to read: every wait ended in the spin or slept. */
#include <emmintrin.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- stand-ins: Windows' SRW lock and condition variable, the processor count ---- */
typedef struct { pthread_mutex_t m; } SRWLOCK;
#define SRWLOCK_INIT {PTHREAD_MUTEX_INITIALIZER}
typedef struct { pthread_cond_t c; } CONDITION_VARIABLE;
static _Thread_local long lock_takes;   /* (this thread's takes of the lock: vc_lock is the only one the block takes) */
static void AcquireSRWLockExclusive(SRWLOCK *l) {
    lock_takes++;
    pthread_mutex_lock(&l->m);
}
static void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_mutex_unlock(&l->m); }
static void WakeConditionVariable(CONDITION_VARIABLE *cv) { pthread_cond_signal(&cv->c); }
typedef struct { unsigned long dwNumberOfProcessors; } SYSTEM_INFO;
static int logical = 16;
static void GetSystemInfo(SYSTEM_INFO *si) { si->dwNumberOfProcessors = (unsigned long)logical; }

/* ---- the thread: the fields the block reads ---- */
typedef struct { int poll; } Ctx;
typedef struct HosThread {
    Ctx ctx;
    Ctx *run_ctx;
    int priority, ideal_core;
    uint64_t core_mask;
    char name[48];
    int vc_held, vc_last, vc_queued;
    volatile int vc_kick;
    int64_t vc_since, vc_grant_at;
    CONDITION_VARIABLE vc_cv;
} HosThread;

static int failures;
#define FAIL(...)                                                                                                          \
    do {                                                                                                                   \
        printf("FAIL:%d ", __LINE__);                                                                                      \
        printf(__VA_ARGS__);                                                                                               \
        printf("\n");                                                                                                      \
        fflush(stdout);                                                                                                    \
        __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED);                                                                \
    } while (0)
#define CHECK(x)                                                                                                           \
    do {                                                                                                                   \
        if (!(x)) FAIL("%s", #x);                                                                                          \
    } while (0)

/* a sleep on the condition variable: the thread must not be sleeping on a core it was granted (a grant made while it spun
   is not waited out: vc_grant's wake went to nobody). sleeps_begun: the sleeps begun so far (counted with vc_lock held) */
static int slept_granted, sleeps_begun;
static int SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, unsigned ms, unsigned flags) {
    (void)flags;
    HosThread *t = (HosThread *)((char *)cv - offsetof(HosThread, vc_cv));
    if (__atomic_load_n(&t->vc_held, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&slept_granted, 1, __ATOMIC_RELAXED);
        FAIL("%s sleeps holding core %d it was granted", t->name, t->vc_held - 1);
    }
    __atomic_add_fetch(&sleeps_begun, 1, __ATOMIC_ACQ_REL);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    while (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
    pthread_cond_timedwait(&cv->c, &l->m, &ts);
    lock_takes++;
    return 1;
}

/* ---- the clock, the log, the cores ---- */
/* the test's clock (the scripted cases): non-zero, it is what hos_now_ns answers, and it moves only by clock_step */
static int64_t test_clock;
static void clock_step(int64_t ns) { __atomic_add_fetch(&test_clock, ns, __ATOMIC_ACQ_REL); }
static int64_t hos_now_ns(void) {
    const int64_t own = __atomic_load_n(&test_clock, __ATOMIC_ACQUIRE);
    if (own) return own;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static char spin_told[512];
static void hos_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&log_mu);
    printf("LOG: %s\n", line);
    fflush(stdout);
    if (strstr(line, "spins up to")) snprintf(spin_told, sizeof spin_told, "%s", line);
    pthread_mutex_unlock(&log_mu);
}
static int cores = 3;
static int hos_cores(void) { return cores; }
static int hos_thread_core(HosThread *t) {   /* (host/hos_thread.c's) */
    if (!t) return 0;
    if (t->vc_held) return t->vc_held - 1;
    const int n = hos_cores();
    if (t->ideal_core >= 0) return t->ideal_core % n;
    return t->core_mask ? (int)(__builtin_ctzll(t->core_mask) % (unsigned)n) : 0;
}

#include "vcore_block.inc"

/* ---- helpers ---- */
static void sleep_us(long us) {
    struct timespec ts = {us / 1000000, (us % 1000000) * 1000};
    nanosleep(&ts, NULL);
}
static void thread_init(HosThread *t, const char *name, int priority, uint64_t mask) {
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->priority = priority;
    t->core_mask = mask;
    t->ideal_core = -2;
    pthread_cond_init(&t->vc_cv.c, NULL);
}
/* waits (polls) until *p is n or more: a state, not a time (the watchdog bounds it) */
static void wait_for(const volatile int *p, int n) {
    while (__atomic_load_n(p, __ATOMIC_ACQUIRE) < n) sleep_us(200);
}
/* liveness only: a case not ended in 60 s fails (nothing a case checks depends on how soon it ends) */
static void *watchdog(void *arg) {
    (void)arg;
    for (int i = 0; i < 600; i++) sleep_us(100000);
    printf("FAIL: the case did not end in 60 s (a hand-off that never came, a lock never let go, or a spin that never ended)\n");
    fflush(stdout);
    _Exit(1);
}
/* the stats line now, whatever its 5 s (the remainder since the last one) */
static void stats_now(void) {
    AcquireSRWLockExclusive(&vc_lock);
    if (!vc_st.since) vc_st.since = hos_now_ns();
    vc_stats_locked(vc_st.since + 6000000000LL);
    ReleaseSRWLockExclusive(&vc_lock);
}
static void end_state(void) {
    AcquireSRWLockExclusive(&vc_lock);
    for (int k = 0; k < 8; k++)
        if (vc_owner[k]) FAIL("core %d still owned by %s", k, vc_owner[k]->name);
    CHECK(vc_nq == 0);
    CHECK(__atomic_load_n(&vc_spinning, __ATOMIC_RELAXED) == 0);
    CHECK(vc_st.escapes == 0);
    ReleaseSRWLockExclusive(&vc_lock);
    CHECK(slept_granted == 0);
}

/* ---- storm: many hand-offs, with the invariants checked at every hold ---- */
static HosThread *occupant[8];
typedef struct {
    HosThread t;
    int iters;
    unsigned seed;
} Worker;
static unsigned rnd(unsigned *s) {
    *s = *s * 1103515245u + 12345u;
    return *s >> 8;
}
static void hold(HosThread *t) {
    const int c = t->vc_held - 1;
    if (c < 0 || c >= cores) {
        FAIL("%s runs without a core (vc_held %d)", t->name, t->vc_held);
        return;
    }
    HosThread *was = __atomic_exchange_n(&occupant[c], t, __ATOMIC_ACQ_REL);
    if (was) FAIL("core %d run by %s and %s at once", c, was->name, t->name);
    AcquireSRWLockExclusive(&vc_lock);
    int mine = 0;
    for (int k = 0; k < 8; k++) mine += vc_owner[k] == t;
    if (mine != 1 || vc_owner[c] != t) FAIL("%s owns %d cores, vc_held says core %d", t->name, mine, c);
    ReleaseSRWLockExclusive(&vc_lock);
    for (int i = 0; i < 200; i++) _mm_pause();   /* (its work on the core: microseconds) */
    HosThread *mine_back = t;
    if (!__atomic_compare_exchange_n(&occupant[c], &mine_back, NULL, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        FAIL("core %d taken from %s while it ran", c, t->name);
}
static int storm_ready;
static void *storm_thread(void *arg) {
    Worker *w = (Worker *)arg;
    HosThread *t = &w->t;
    __atomic_add_fetch(&storm_ready, 1, __ATOMIC_ACQ_REL);   /* (all eight start together) */
    while (__atomic_load_n(&storm_ready, __ATOMIC_ACQUIRE) < 8) sched_yield();
    vc_acquire(t);
    for (int i = 0; i < w->iters && !__atomic_load_n(&failures, __ATOMIC_RELAXED); i++) {
        hold(t);
        const unsigned r = rnd(&w->seed) % 100;
        if (r < 50) {   /* a checkpoint: the request of a better waiter (or a slice's) is met here */
            if (t->vc_kick) vc_yield(t, t->vc_kick == 1);
        } else if (r < 65) {   /* a yield, SleepThread(0) */
            vc_yield(t, 0);
        } else {   /* a block: the core given up, taken again after it */
            vc_release(t);
            if (r < 80) sched_yield();
            else if (r < 90)
                for (unsigned k = rnd(&w->seed) % 2000; k; k--) _mm_pause();   /* (a short block, off the core) */
            vc_acquire(t);
        }
        if (!t->vc_held) FAIL("%s came back without a core", t->name);
    }
    vc_release(t);
    return NULL;
}
static void storm(void) {
    static Worker w[8];
    static const int prio[8] = {10, 10, 16, 16, 16, 28, 28, 28};
    static const uint64_t mask[8] = {7, 1, 7, 2, 4, 7, 1, 4};
    pthread_t th[8];
    for (int i = 0; i < 8; i++) {
        char name[16];
        snprintf(name, sizeof name, "w%d", i);
        thread_init(&w[i].t, name, prio[i], mask[i]);
        w[i].iters = 25000;
        w[i].seed = 1234u + (unsigned)i * 7919u;
        pthread_create(&th[i], NULL, storm_thread, &w[i]);
    }
    for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < 8; i++) CHECK(!w[i].t.vc_held && !w[i].t.vc_queued);
    end_state();
    stats_now();
}

/* ---- the scripted hand-off: a better waiter asks the holder, which gives the core up HOLD_NS later on the test's clock
   (SPIN_TO_GRANT: while the waiter spins; SPIN_EXPIRES: once the waiter's spin passed its bound and it sleeps; NO_SPIN:
   once the waiter sleeps, as it does at once with no spin) ---- */
#define HOLD_NS 100000000LL
enum { SPIN_TO_GRANT, SPIN_EXPIRES, NO_SPIN };
static HosThread holder, waiter;
static int64_t waiter_waited;
static int waiter_got;
static void *waiter_thread(void *arg) {
    (void)arg;
    const int64_t t0 = hos_now_ns();
    vc_acquire(&waiter);
    waiter_waited = hos_now_ns() - t0;
    CHECK(waiter.vc_held == 1);
    __atomic_store_n(&waiter_got, 1, __ATOMIC_RELEASE);
    wait_for(&vc_nq, 1);   /* (holds it until the holder waits for it again: the release grants it) */
    vc_release(&waiter);
    return NULL;
}
typedef struct {
    int64_t waits, spun, slept, spin_ns, capped, run_spin_n, run_sleep_n, kicks, run_spin_ns;
} Seen;
static Seen handoff(int mode) {
    cores = 1;
    test_clock = 1000000000000LL;
    thread_init(&holder, "holder", 30, 1);
    thread_init(&waiter, "waiter", 10, 1);
    vc_acquire(&holder);
    CHECK(holder.vc_held == 1);
    pthread_t th;
    pthread_create(&th, NULL, waiter_thread, NULL);
    wait_for(&holder.vc_kick, 1);   /* (asked: the waiter is better) */
    CHECK(holder.ctx.poll == 1);
    if (mode == SPIN_TO_GRANT) {   /* the waiter spins; on the test's clock its bound never comes */
        wait_for(&vc_spinning, 1);
        clock_step(HOLD_NS);   /* (the holder's guest code until its next checkpoint) */
    } else if (mode == SPIN_EXPIRES) {   /* the clock moves until the waiter's spin has passed its bound and it sleeps */
        wait_for(&vc_spinning, 1);
        while (!__atomic_load_n(&sleeps_begun, __ATOMIC_ACQUIRE)) {
            clock_step(50000);
            sleep_us(100);
        }
    } else {
        wait_for(&sleeps_begun, 1);
        clock_step(HOLD_NS);
    }
    vc_yield(&holder, holder.vc_kick == 1);   /* hos_vcore_checkpoint: gives the core up, waits for it again */
    CHECK(holder.vc_held == 1);
    CHECK(__atomic_load_n(&waiter_got, __ATOMIC_ACQUIRE));   /* (the waiter ran on it meanwhile) */
    pthread_join(th, NULL);
    vc_release(&holder);
    Seen s;
    AcquireSRWLockExclusive(&vc_lock);
    s.waits = vc_st.waits, s.spun = vc_st.spun, s.slept = vc_st.slept, s.spin_ns = vc_st.spin_ns, s.capped = vc_st.spin_capped;
    s.run_spin_n = vc_st.run_spin_n, s.run_sleep_n = vc_st.run_sleep_n, s.kicks = vc_st.kicks, s.run_spin_ns = vc_st.run_spin_ns;
    ReleaseSRWLockExclusive(&vc_lock);
    end_state();
    stats_now();
    printf("SEEN: waits %lld spun %lld slept %lld spin %.3f ms capped %lld; waiter waited %.3f ms\n", (long long)s.waits,
           (long long)s.spun, (long long)s.slept, s.spin_ns / 1e6, (long long)s.capped, waiter_waited / 1e6);
    return s;
}

/* ---- the cap: two waiters ask two holders; one spinner at most. On the test's clock a spin ends only at its grant, and
   the holders give their cores up only when told (go): b1 spins for as long as the case needs, however loaded the PC ---- */
typedef struct Actor {
    HosThread t;
    int ready, go;
    struct Actor *peer;   /* (a waiter's: the holder it asks) */
} Actor;
static Actor a1, a2, b1, b2;
static void *holder_actor(void *arg) {
    Actor *a = (Actor *)arg;
    vc_acquire(&a->t);
    __atomic_store_n(&a->ready, 1, __ATOMIC_RELEASE);
    wait_for(&a->t.vc_kick, 1);
    wait_for(&a->go, 1);
    vc_yield(&a->t, a->t.vc_kick == 1);
    CHECK(a->t.vc_held);
    vc_release(&a->t);
    return NULL;
}
static void *waiter_actor(void *arg) {
    Actor *b = (Actor *)arg;
    vc_acquire(&b->t);
    CHECK(b->t.vc_held);
    wait_for(&b->peer->t.vc_queued, 1);   /* (holds it until its holder waits for it again: the release grants it) */
    vc_release(&b->t);
    return NULL;
}
static void cap(void) {
    cores = 2;
    test_clock = 1000000000000LL;
    thread_init(&a1.t, "a1", 30, 1);
    thread_init(&a2.t, "a2", 30, 2);
    thread_init(&b1.t, "b1", 10, 1);
    thread_init(&b2.t, "b2", 10, 2);
    b1.peer = &a1, b2.peer = &a2;
    pthread_t th[4];
    pthread_create(&th[0], NULL, holder_actor, &a1);
    pthread_create(&th[1], NULL, holder_actor, &a2);
    wait_for(&a1.ready, 1);
    wait_for(&a2.ready, 1);
    pthread_create(&th[2], NULL, waiter_actor, &b1);
    wait_for(&vc_spinning, 1);   /* (b1 asked a1 and spins, until a1 gives its core up) */
    pthread_create(&th[3], NULL, waiter_actor, &b2);
    /* b2 asks a2 and comes to the spin while b1 spins: left out (counted, the count given back, all with vc_lock held), or,
       with no cap kept, a second spinner */
    int64_t capped = 0;
    int spinning = 0;
    for (;;) {
        AcquireSRWLockExclusive(&vc_lock);
        capped = vc_st.spin_capped;
        spinning = __atomic_load_n(&vc_spinning, __ATOMIC_ACQUIRE);
        ReleaseSRWLockExclusive(&vc_lock);
        if (capped || spinning >= 2) break;
        sleep_us(200);
    }
    if (capped != 1 || spinning != 1) FAIL("with b1 spinning, b2: %lld spins left out, %d spinning", (long long)capped, spinning);
    __atomic_store_n(&a1.go, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&a2.go, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    AcquireSRWLockExclusive(&vc_lock);
    printf("SEEN: waits %lld spun %lld slept %lld capped %lld kicks %lld\n", (long long)vc_st.waits, (long long)vc_st.spun,
           (long long)vc_st.slept, (long long)vc_st.spin_capped, (long long)vc_st.kicks);
    /* (kicks: each waiter asks its holder once; a sleeping waiter may ask again in the moment a yielding holder has let the
       lock go between vc_yield and vc_release, a request cleared and not yet met: harmless, so two or more) */
    CHECK(vc_st.waits == 4 && vc_st.spun == 1 && vc_st.slept == 3 && vc_st.spin_capped == 1 && vc_st.kicks >= 2);
    ReleaseSRWLockExclusive(&vc_lock);
    end_state();
    stats_now();
}

/* ---- the yield's fast path ---- */
static HosThread yt, yw;
static int yw_held;
static void *yield_waiter(void *arg) {
    (void)arg;
    vc_acquire(&yw);
    __atomic_store_n(&yw_held, 1, __ATOMIC_RELEASE);
    wait_for(&yt.vc_queued, 1);   /* (holds it until the yielder waits for it again) */
    vc_release(&yw);
    return NULL;
}
static void yield_fast(void) {
    cores = 1;
    test_clock = 1000000000000LL;   /* (standing still: the as-good waiter's time slice, HOS_VCORE_SLICE_US, never comes) */
    thread_init(&yt, "yielder", 20, 1);
    thread_init(&yw, "as good", 20, 1);
    vc_acquire(&yt);
    long before = lock_takes;
    vc_yield(&yt, 0);   /* nobody waits: no lock */
    CHECK(lock_takes == before);
    CHECK(yt.vc_held == 1);
    yt.vc_kick = 2;   /* a request left from a waiter gone: the lock taken, the request cleared */
    before = lock_takes;
    vc_yield(&yt, 0);
    CHECK(lock_takes > before);
    CHECK(yt.vc_kick == 0 && yt.vc_held == 1);
    pthread_t th;
    pthread_create(&th, NULL, yield_waiter, NULL);
    wait_for(&sleeps_begun, 1);   /* (the waiter queued and asleep) */
    CHECK(vc_nq == 1);
    CHECK(yt.vc_kick == 0);   /* (an as-good waiter asks only after its time slice) */
    vc_yield(&yt, 0);   /* a waiter as good: it gets the core; this thread waits for it again */
    CHECK(yw_held);
    CHECK(yt.vc_held == 1);
    vc_release(&yt);   /* (before the join: with no hand-off the waiter still waits for it) */
    pthread_join(th, NULL);
    AcquireSRWLockExclusive(&vc_lock);
    CHECK(vc_st.yields == 1);
    ReleaseSRWLockExclusive(&vc_lock);
    end_state();
}

#ifdef _WIN32
unsigned __stdcall timeBeginPeriod(unsigned ms);   /* (winmm: the game keeps the 1 ms timer resolution too, for its timed waits) */
#endif
int main(int argc, char **argv) {
    const char *c = argc > 1 ? argv[1] : "";
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    if (getenv("TEST_LOGICAL")) logical = atoi(getenv("TEST_LOGICAL"));
    {   /* (a liveness bound: the storms end in a second or so, the scripted cases sooner) */
        pthread_t dog;
        pthread_create(&dog, NULL, watchdog, NULL);
        pthread_detach(dog);
    }
    if (!strcmp(c, "storm") || !strcmp(c, "storm-off") || !strcmp(c, "storm-tight")) {
        storm();
    } else if (!strcmp(c, "grant-in-spin")) {   /* HOS_VCORE_SPIN_US=1000000 */
        const Seen s = handoff(SPIN_TO_GRANT);
        CHECK(s.waits == 2 && s.spun == 1 && s.slept == 1 && s.kicks == 1 && s.capped == 0);
        CHECK(s.run_spin_n == 1 && s.run_sleep_n == 1);
        CHECK(waiter_waited == HOLD_NS);   /* (ended at the grant: the clock moved by the holder's time only) */
        CHECK(s.spin_ns <= HOLD_NS);
        CHECK(s.run_spin_ns == 0);   /* (the grant and the run at the same instant of the test's clock) */
        CHECK(strstr(spin_told, "spins up to 1000000 us") != NULL);
    } else if (!strcmp(c, "grant-off")) {   /* HOS_VCORE_SPIN_US=0 */
        const Seen s = handoff(NO_SPIN);
        /* (the sleeping waiter may ask again during the holder's yield: cap's note on kicks) */
        CHECK(s.waits == 2 && s.spun == 0 && s.slept == 2 && s.spin_ns == 0 && s.capped == 0 && s.kicks >= 1);
        CHECK(s.run_spin_n == 0 && s.run_sleep_n == 2);
        CHECK(waiter_waited == HOLD_NS);
        CHECK(strstr(spin_told, "spins up to 0 us") != NULL);
    } else if (!strcmp(c, "default")) {   /* HOS_VCORE_SPIN_US unset: 250 us, then the sleep */
        const Seen s = handoff(SPIN_EXPIRES);
        CHECK(s.waits == 2 && s.spun == 0 && s.slept == 2 && s.capped == 0);
        CHECK(s.spin_ns >= 250000LL);
        CHECK(strstr(spin_told, "spins up to 250 us") != NULL);
    } else if (!strcmp(c, "small-pc")) {   /* TEST_LOGICAL=8, HOS_VCORE_SPIN_MAX unset */
        const Seen s = handoff(NO_SPIN);
        CHECK(s.waits == 2 && s.spun == 0 && s.slept == 2 && s.spin_ns == 0 && s.capped == 0);
        CHECK(strstr(spin_told, "spins up to 0 us") != NULL);
    } else if (!strcmp(c, "cap")) {   /* HOS_VCORE_SPIN_MAX=1, HOS_VCORE_SPIN_US=1000000 */
        cap();
    } else if (!strcmp(c, "cap-default")) {
        static const int in[9] = {4, 8, 9, 11, 12, 16, 20, 24, 64}, out[9] = {0, 0, 0, 0, 1, 2, 3, 4, 4};
        for (int i = 0; i < 9; i++)
            if (vc_spin_most(in[i]) != out[i]) FAIL("vc_spin_most(%d) = %d, not %d", in[i], vc_spin_most(in[i]), out[i]);
    } else if (!strcmp(c, "yield-fast")) {   /* HOS_VCORE_SLICE_US=10000000 */
        yield_fast();
    } else {
        FAIL("unknown case %s", c);
    }
    if (failures) return 1;
    printf("PASS %s\n", c);
    return 0;
}
