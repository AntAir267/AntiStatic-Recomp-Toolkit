/* Kernel objects: the handle table, threads, events, mutexes, critical sections and waits.
   Waits use one process-wide lock and condition variable: simple, and these objects are
   few and rarely contended. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "runtime.h"
#include "image.h"

extern void set_current_cpu(CPU *c);

static pthread_mutex_t klock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kcond = PTHREAD_COND_INITIALIZER;
static void kick(void);

/* ------------------------------------------------------------------ handles */
#define MAXH 4096
#define HBASE 0x1000u
static struct { int type; void *obj; } htab[MAXH];

uint32_t handle_new(int type, void *obj)
{
    pthread_mutex_lock(&klock);
    for (int i = 1; i < MAXH; i++)
        if (htab[i].type == H_FREE) {
            htab[i].type = type;
            htab[i].obj = obj;
            pthread_mutex_unlock(&klock);
            return HBASE + 4u * i;
        }
    pthread_mutex_unlock(&klock);
    fatal("out of handles");
}

static int hindex(uint32_t h)
{
    if (h < HBASE || (h & 3))
        return -1;
    uint32_t i = (h - HBASE) / 4;
    return i < MAXH ? (int)i : -1;
}

void *handle_get(uint32_t h, int type)
{
    int i = hindex(h);
    if (i < 0 || htab[i].type != type)
        return NULL;
    return htab[i].obj;
}

int handle_type(uint32_t h)
{
    int i = hindex(h);
    return i < 0 ? H_FREE : htab[i].type;
}

void handle_close(uint32_t h)
{
    int i = hindex(h);
    if (i < 0)
        return;
    pthread_mutex_lock(&klock);
    htab[i].type = H_FREE;
    htab[i].obj = NULL;
    kick();                             /* (a waiter on it looks again: a closed handle counts as signalled) */
    pthread_mutex_unlock(&klock);
}

/* ------------------------------------------------------------------ waitable objects */
typedef struct { int manual, signalled; } Event;
typedef struct { CPU *owner; int count, abandoned; } Mutex;   /* abandoned: its owner ended holding it */
struct Thread {
    pthread_t pt;
    CPU *cpu;
    uint32_t start, param, exit_code;
    int finished;
    int suspended;                      /* CREATE_SUSPENDED: waits for ResumeThread before starting */
};

static int signalled_locked(uint32_t h, CPU *self)
{
    switch (handle_type(h)) {
    case H_EVENT: return ((Event *)handle_get(h, H_EVENT))->signalled;
    case H_MUTEX: { Mutex *m = handle_get(h, H_MUTEX); return !m->owner || m->owner == self; }
    case H_THREAD: return ((Thread *)handle_get(h, H_THREAD))->finished;
    case H_SEMAPHORE: { extern int semaphore_signalled_locked(uint32_t); return semaphore_signalled_locked(h); }
    default: return 1;
    }
}

static int acquire_locked(uint32_t h, CPU *self);

/* ------------------------------------------------------------------ test runs: one at a time */
/* In --test runs the game's threads (the main one and its file loader, 0x543a80) take turns
   instead of running at once, so every run interleaves them the same way: one holds the turn
   and runs until it waits for something that is not there, sleeps, finishes, or (the main
   thread) ends a frame; the turn then goes, in a fixed order, to the next thread that can run.
   Everything here is under klock. */
enum { P_RUN, P_WANT, P_SLEEP, P_BLOCK, P_DONE };
#define DET_MAX 32
static struct { CPU *cpu; int state, n, all, timed; uint32_t hs[64]; } parts[DET_MAX];
static int nparts, tok = -1;

static int det_me(CPU *self)
{
    for (int i = 0; i < nparts; i++)
        if (parts[i].cpu == self) return i;
#if defined(PORT_TEST_SCHEDULER) && PORT_TEST_SCHEDULER == 2
    /* (the C++ line's test scheduler) a finished thread's place is taken
       again: the turn order the ports of that line recorded their baselines with */
    for (int i = 0; i < nparts; i++)
        if (parts[i].state == P_DONE && i != tok) {
            memset(&parts[i], 0, sizeof parts[i]);
            parts[i].cpu = self;
            parts[i].state = P_RUN;
            return i;
        }
#endif
    if (nparts == DET_MAX) fatal("--test: more than %d threads", DET_MAX);
    parts[nparts].cpu = self;
    parts[nparts].state = P_RUN;
    if (tok < 0) tok = nparts;
    return nparts++;
}

static int det_satisfied(int i)
{
    int any = 0, all = 1;
    for (int k = 0; k < parts[i].n; k++) {
        int s = signalled_locked(parts[i].hs[k], parts[i].cpu);
        any |= s;
        all &= s;
    }
    return parts[i].all ? all : any;
}

static int det_runnable(int i, int yielding)
{
    Thread *t = parts[i].cpu->thread;
    if (t && t->suspended > 0) return 0;
    switch (parts[i].state) {
    case P_WANT: case P_SLEEP: return 1;
    case P_BLOCK: return det_satisfied(i) || (parts[i].timed && yielding);
    default: return 0;
    }
}

static void *det_watch(void *arg);
static volatile unsigned long det_turns;

/* the turn goes to the next thread after `from` that can run (itself last) */
static void det_schedule(int from, int yielding)
{
    static int watching;
    if (!watching) {
        watching = 1;
        pthread_t w;
        pthread_create(&w, NULL, det_watch, NULL);
    }
    det_turns++;
    tok = -1;
    for (int k = 1; k <= nparts; k++) {
        int i = (from + k) % nparts;
        if (det_runnable(i, yielding)) { tok = i; break; }
    }
    LOG(LOG_THREAD, "turn: %d -> %d (%s; states %d %d %d)", from, tok, yielding ? "yield" : "block",
        parts[0].state, nparts > 1 ? parts[1].state : -1, nparts > 2 ? parts[2].state : -1);
    kick();
}

/* a watchdog for test runs: the turn stuck for 5 s prints every thread's stack (Linux: by a signal
   to each thread; on Windows it says so only) */
#include <dirent.h>
#include <execinfo.h>
#include <signal.h>
#ifndef _WIN32
#include <sys/syscall.h>
#endif
#include <unistd.h>
static volatile unsigned long det_turns;
#ifndef _WIN32
static void det_stack(int sig)
{
    void *bt[24];
    int n = backtrace(bt, 24);
    char head[64];
    int k = snprintf(head, sizeof head, "--- thread %ld:\n", (long)syscall(SYS_gettid));
    if (write(2, head, k) < 0) {}
    backtrace_symbols_fd(bt, n, 2);
    (void)sig;
}
#endif
volatile int det_watch_quiet;           /* (set while a capture is written: capture.c) */
static void *det_watch(void *arg)
{
    unsigned long last = 0;
    int same = 0;
    for (;;) {
        sleep(1);
        if (det_turns != last || det_watch_quiet) { last = det_turns; same = 0; continue; }
        if (++same != 5) continue;
#ifdef _WIN32
        if (write(2, "--test: the turn is stuck\n", 26) < 0) {}
#else
        if (write(2, "--test: the turn is stuck; stacks:\n", 36) < 0) {}
        signal(SIGUSR2, det_stack);
        DIR *d = opendir("/proc/self/task");
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (e->d_name[0] != '.') {
                syscall(SYS_tgkill, getpid(), atoi(e->d_name), SIGUSR2);
                usleep(50000);
            }
        if (d) closedir(d);
#endif
    }
    (void)arg;
    return NULL;
}

void devrec_others_begin(void), devrec_others_end(void);

static void det_wait_turn(int me)
{
    int others = tok != me && dev_recording;    /* (other threads' writes meanwhile: devrec.c) */
    if (others) devrec_others_begin();
    while (tok != me)
        pthread_cond_wait(&kcond, &klock);
    parts[me].state = P_RUN;
    if (others) devrec_others_end();
}

/* a sleep, or the main thread's frame end: the others that can run take their turns first */
/* a forked check child (runtime/rewrite/fncheck.c, audit.c) has only the thread that forked: it
   never gives its turn away (no other thread could give it back) */
int fncheck_solo;
void det_yield(void)
{
    if (!deterministic || fncheck_solo) return;
    CPU *self = current_cpu();
    if (!self) return;
    pthread_mutex_lock(&klock);
    int me = det_me(self);
    parts[me].state = P_SLEEP;
    det_schedule(me, 1);
    det_wait_turn(me);
    pthread_mutex_unlock(&klock);
}

/* takes the object; 1 if it was a mutex its owner left behind (the wait says WAIT_ABANDONED, once) */
static int acquire_locked(uint32_t h, CPU *self)
{
    switch (handle_type(h)) {
    case H_EVENT: { Event *e = handle_get(h, H_EVENT); if (!e->manual) e->signalled = 0; break; }
    case H_MUTEX: {
        Mutex *m = handle_get(h, H_MUTEX);
        m->owner = self;
        m->count++;
        int was = m->abandoned;
        m->abandoned = 0;
        return was;
    }
    case H_SEMAPHORE: { extern void semaphore_take_locked(uint32_t); semaphore_take_locked(h); break; }
    default: break;
    }
    return 0;
}

/* a thread ending: the mutexes it still owns are let go, abandoned (as Windows does; before, every
   later waiter waited for good: SwARc 76e4a75) */
static void mutexes_abandon_locked(CPU *self)
{
    for (int i = 1; i < MAXH; i++)
        if (htab[i].type == H_MUTEX) {
            Mutex *m = htab[i].obj;
            if (m->owner == self) {
                LOG(LOG_THREAD, "thread %d ended holding a mutex: abandoned", self->thread_index);
                m->owner = NULL;
                m->count = 0;
                m->abandoned = 1;
            }
        }
}

static int det_check_locked(int n, const uint32_t *hs, int all, CPU *self)
{
    if (all) {
        for (int i = 0; i < n; i++)
            if (!signalled_locked(hs[i], self)) return -1;
        int ab = 0;
        for (int i = 0; i < n; i++) ab |= acquire_locked(hs[i], self);
        return ab ? 0x80 : 0;           /* (WAIT_ABANDONED_0) */
    }
    for (int i = 0; i < n; i++)
        if (signalled_locked(hs[i], self)) return acquire_locked(hs[i], self) ? 0x80 + i : i;
    return -1;
}

int wait_handles(int n, const uint32_t *hs, int all, uint32_t ms)
{
    CPU *self = current_cpu();
    /* a handle that is no handle (0, or one closed before the wait) fails the wait, as on
       Windows (WAIT_FAILED, ERROR_INVALID_HANDLE); one closed during the wait counts as
       signalled, Windows keeping the object alive for it (pcrecomp b114191) */
    for (int i = 0; i < n; i++)
        if (hs[i] == 0 || (hindex(hs[i]) >= 0 && handle_type(hs[i]) == H_FREE)) {
            extern void set_last_error(uint32_t e);
            set_last_error(6);
            LOG(LOG_THREAD, "wait on %x: not a handle", hs[i]);
            return (int)0xffffffffu;
        }
    if (deterministic && self) {
        pthread_mutex_lock(&klock);
        int me = det_me(self), r = det_check_locked(n, hs, all, self);
        while (r < 0 && ms != 0) {
            parts[me].state = P_BLOCK;
            parts[me].n = n > 64 ? 64 : n;
            memcpy(parts[me].hs, hs, 4 * (size_t)parts[me].n);
            parts[me].all = all;
            parts[me].timed = ms != 0xffffffffu;
            det_schedule(me, 0);
            det_wait_turn(me);
            r = det_check_locked(n, hs, all, self);
            if (r < 0 && ms != 0xffffffffu) {           /* given the turn at a frame end: time out */
#if defined(PORT_TEST_SCHEDULER) && PORT_TEST_SCHEDULER == 2
                /* ... and if the main thread waits without a timeout for what only time brings
                   (LIXS: a DirectSound stream's notification, while its sound thread ticks in
                   10 ms waits), the time this wait gave up after passes, as it would in real time
                   [verified: a lixs --test run frozen at 2.92 s; penguins had it too] */
                extern void vclock_advance(double ms);
                if (me != 0 && parts[0].state == P_BLOCK && !parts[0].timed && !det_satisfied(0))
                    vclock_advance(ms > 50 ? 50.0 : (double)ms);
#endif
                break;
            }
        }
        pthread_mutex_unlock(&klock);
        return r < 0 ? 0x102 : r;
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    if (ms != 0xffffffffu) {
        until.tv_sec += ms / 1000;
        until.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    }
    pthread_mutex_lock(&klock);
    for (;;) {
        if (all) {
            int ok = 1;
            for (int i = 0; i < n; i++)
                ok &= signalled_locked(hs[i], self);
            if (ok) {
                int ab = 0;
                for (int i = 0; i < n; i++)
                    ab |= acquire_locked(hs[i], self);
                pthread_mutex_unlock(&klock);
                return ab ? 0x80 : 0;
            }
        } else {
            for (int i = 0; i < n; i++)
                if (signalled_locked(hs[i], self)) {
                    int ab = acquire_locked(hs[i], self);
                    pthread_mutex_unlock(&klock);
                    return ab ? 0x80 + i : i;
                }
        }
        if (ms == 0) break;
        int r = ms == 0xffffffffu ? pthread_cond_wait(&kcond, &klock)
                                  : pthread_cond_timedwait(&kcond, &klock, &until);
        if (r == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&klock);
    return 0x102;                         /* WAIT_TIMEOUT */
}

int wait_handle(uint32_t h, uint32_t ms) { return wait_handles(1, &h, 0, ms); }

static void kick(void) { pthread_cond_broadcast(&kcond); }

/* an event for the runtime (WSACreateEvent) */
uint32_t event_create(int manual, int initial)
{
    Event *e = calloc(1, sizeof *e);
    e->manual = manual;
    e->signalled = initial;
    return handle_new(H_EVENT, e);
}

API(KERNEL32, CreateEventA, 4)
{
    uint32_t h = event_create(ARG(1) != 0, ARG(2) != 0);
    LOG(LOG_THREAD, "CreateEventA(manual=%d, initial=%d, %s) -> %x", ARG(1) != 0, ARG(2) != 0,
        ARG(3) ? gstr(ARG(3)) : "unnamed", h);
    set_last_error(0);
    return h;
}

/* events are not named: one by name is never there */
API(KERNEL32, OpenEventW, 3)
{
    char b[256];
    LOG(LOG_THREAD, "OpenEventW(\"%s\") -> not found", gwstr(ARG(2), b, sizeof b));
    set_last_error(2);
    return 0;
}

/* PulseEvent: releases the waiting threads and leaves the event reset. An auto-reset event
   acts as SetEvent (the one released waiter resets it; with none waiting it stays set, which
   Windows would not do [guess: harmless]); a manual-reset one is set, the waiters woken, and
   reset once they had a moment to run. */
API(KERNEL32, PulseEvent, 1)
{
    pthread_mutex_lock(&klock);
    Event *e = handle_get(ARG(0), H_EVENT);
    if (e) {
        e->signalled = 1;
        kick();
    }
    pthread_mutex_unlock(&klock);
    if (e && e->manual) {
        sched_yield();
        pthread_mutex_lock(&klock);
        e->signalled = 0;
        pthread_mutex_unlock(&klock);
    }
    return e != NULL;
}

API(KERNEL32, SetEvent, 1)
{
    pthread_mutex_lock(&klock);
    Event *e = handle_get(ARG(0), H_EVENT);
    if (e) {
        e->signalled = 1;
        kick();
    }
    pthread_mutex_unlock(&klock);
    LOG(LOG_THREAD, "SetEvent(%x)", ARG(0));
    return e != NULL;
}

static uint32_t create_mutex(int owned, const char *name)
{
    Mutex *m = calloc(1, sizeof *m);
    if (owned) {
        m->owner = current_cpu();
        m->count = 1;
    }
    uint32_t h = handle_new(H_MUTEX, m);
    LOG(LOG_THREAD, "CreateMutex(owned=%d, %s) -> %x", owned, name ? name : "unnamed", h);
    set_last_error(0);                   /* the game treats any error here as "already running" */
    return h;
}

API(KERNEL32, CreateMutexA, 3) { return create_mutex(ARG(1) != 0, gstr(ARG(2))); }
API(KERNEL32, CreateMutexW, 3)
{
    char b[256];
    return create_mutex(ARG(1) != 0, ARG(2) ? gwstr(ARG(2), b, sizeof b) : NULL);
}

API(KERNEL32, ReleaseMutex, 1)
{
    pthread_mutex_lock(&klock);
    Mutex *m = handle_get(ARG(0), H_MUTEX);
    int ok = m && m->owner == current_cpu();
    if (ok && --m->count == 0) {
        m->owner = NULL;
        kick();
    }
    pthread_mutex_unlock(&klock);
    return ok;
}

API(KERNEL32, WaitForSingleObject, 2)
{
    LOG(LOG_THREAD, "WaitForSingleObject(%x, %d)", ARG(0), (int)ARG(1));
    int r = wait_handle(ARG(0), ARG(1));
    LOG(LOG_THREAD, "WaitForSingleObject(%x) -> %x", ARG(0), r);
    return r;
}

API(KERNEL32, WaitForMultipleObjects, 4)
{
    uint32_t n = ARG(0), hs[64];
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++)
        hs[i] = R32(ARG(1) + 4 * i);
    LOG(LOG_THREAD, "WaitForMultipleObjects(%u, all=%d, %d)", n, ARG(2) != 0, (int)ARG(3));
    return wait_handles(n, hs, ARG(2) != 0, ARG(3));
}

/* ------------------------------------------------------------------ threads */
static void *thread_main(void *arg)
{
    Thread *t = arg;
    pthread_mutex_lock(&klock);
    while (t->suspended > 0)
        pthread_cond_wait(&kcond, &klock);
    if (deterministic)
        det_wait_turn(det_me(t->cpu));
    pthread_mutex_unlock(&klock);
    set_current_cpu(t->cpu);
    fpu_activate(t->cpu);
    dlls_thread_notify(t->cpu, 2);      /* DLL_THREAD_ATTACH (the translated C runtimes' per-thread data) */
    LOG(LOG_THREAD, "thread %d starts at %08x(%08x)", t->cpu->thread_index, t->start, t->param);
    uint32_t r = guest_call(t->cpu, t->start, 1, t->param);
    tls_thread_end(t->cpu);             /* THREAD_DETACH TLS callbacks (tls.c) */
    pthread_mutex_lock(&klock);
    mutexes_abandon_locked(t->cpu);
    extern void cpu_release(CPU *c);
    cpu_release(t->cpu);                /* (its stack, TEB, TLS for the next thread: before the others' turn,
                                           so which thread gets them doesn't depend on timing) */
    t->exit_code = r;
    t->finished = 1;
    if (deterministic) {
        int me = det_me(t->cpu);
        parts[me].state = P_DONE;
        det_schedule(me, 1);
    }
    kick();
    pthread_mutex_unlock(&klock);
    LOG(LOG_THREAD, "thread %d returned %u", t->cpu->thread_index, r);
    return NULL;
}

API(KERNEL32, CreateThread, 6)
{
    Thread *t = calloc(1, sizeof *t);
    /* the stack: the program's own reserve (its PE header), or the size asked when that is a
       reservation (STACK_SIZE_PARAM_IS_A_RESERVATION) or more than the reserve, as Windows */
    uint32_t stack = (ARG(4) & 0x10000) && ARG(1) ? ARG(1) : ARG(1) > STACK_RESERVE ? ARG(1) : STACK_RESERVE;
    t->cpu = cpu_new(stack);
    t->cpu->thread = t;
    t->start = ARG(2);
    t->param = ARG(3);
    if (ARG(4) & 4)
        t->suspended = 1;               /* CREATE_SUSPENDED */
    if (deterministic) {                /* (it takes turns: det_yield) */
        pthread_mutex_lock(&klock);
        det_me(current_cpu());
        parts[det_me(t->cpu)].state = P_WANT;
        pthread_mutex_unlock(&klock);
    }
    uint32_t h = handle_new(H_THREAD, t);
    if (ARG(5))
        W32(ARG(5), R32(t->cpu->fs_base + 0x24));
    LOG(LOG_THREAD, "CreateThread(start=%08x, param=%08x, stack=%x) -> %x (thread %d)",
        t->start, t->param, ARG(1), h, t->cpu->thread_index);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8 << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);   /* (nothing joins it: given back when it ends) */
    pthread_create(&t->pt, &at, thread_main, t);
    pthread_attr_destroy(&at);
    return h;
}

API(KERNEL32, ExitThread, 1)
{
    CPU *self = current_cpu();
    Thread *t = self->thread;
    LOG(LOG_THREAD, "ExitThread(%u) on thread %d", ARG(0), self->thread_index);
    if (!t)
        game_exit((int)ARG(0));
    uint32_t code = ARG(0);
    tls_thread_end(self);
    pthread_mutex_lock(&klock);
    mutexes_abandon_locked(self);
    extern void cpu_release(CPU *c);
    cpu_release(self);
    t->exit_code = code;
    t->finished = 1;
    if (deterministic) {
        int me = det_me(self);
        parts[me].state = P_DONE;
        det_schedule(me, 1);
    }
    kick();
    pthread_mutex_unlock(&klock);
    pthread_exit(NULL);
}

API(KERNEL32, SetThreadPriority, 2)
{
    LOG(LOG_THREAD, "SetThreadPriority(%x, %d)", ARG(0), (int)ARG(1));
    return 1;
}

API(KERNEL32, Sleep, 1)
{
    uint32_t ms = ARG(0);
    if (deterministic) {                /* (time is the virtual clock's: a turn for the others) */
#if defined(PORT_TEST_SCHEDULER) && PORT_TEST_SCHEDULER == 2
        extern void vclock_sleep(double ms);
        vclock_sleep(ms);               /* (the C++ line: the main thread's sleep moves the clock) */
#elif defined(PORT_TEST_SLEEP_MOVES_CLOCK)
        /* the main thread's Sleep(ms) moves the virtual clock by ms, Sleep(0) not at all (Hard
           Truck 2 times its clock against timeGetTime over a Sleep(2000) at start-up, 0x6035f0:
           with the clock standing still its game time was NaN) */
        extern void vclock_advance(double ms);
        if (current_cpu() == main_cpu && ms != 0xffffffffu)
            vclock_advance(ms);
#endif
        det_yield();
        return 0;
    }
    if (ms == 0) {
        /* a thread that does nothing but poll with Sleep(0) ran flat out, as it did on Windows
           (which gave it every idle cycle): from the fourth Sleep(0) in a row, each under 2 ms
           after the last, it sleeps a millisecond instead (pcrecomp, Theme Park World's pointer
           thread). <PORT_ENV>_SLEEP0=yield keeps every Sleep(0) a yield. */
        static int yield_only = -1;
        static __thread uint64_t last_us;
        static __thread unsigned run;
        if (yield_only < 0) {
            const char *e = port_getenv("SLEEP0");
            yield_only = e && !strcmp(e, "yield");
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t us = (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
        run = us - last_us < 2000 ? run + 1 : 0;
        last_us = us;
        if (run < 4 || yield_only) {
            sched_yield();
            return 0;
        }
        ms = 1;
    }
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
    return 0;
}

API(KERNEL32, InterlockedExchange, 2)
{
    return __atomic_exchange_n((uint32_t *)gptr(ARG(0)), ARG(1), __ATOMIC_SEQ_CST);
}

/* (destination, exchange, comparand): the value before */
API(KERNEL32, InterlockedCompareExchange, 3)
{
    uint32_t expected = ARG(2);
    __atomic_compare_exchange_n((uint32_t *)gptr(ARG(0)), &expected, ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

API(KERNEL32, InterlockedExchangeAdd, 2)
{
    return __atomic_fetch_add((uint32_t *)gptr(ARG(0)), ARG(1), __ATOMIC_SEQ_CST);
}

/* ------------------------------------------------------------------ critical sections
   The program's CRITICAL_SECTION (24 bytes) keeps an index into our table at +0x10 and a
   marker at +0x14. */
#define CS_MAGIC 0x43534e4cu
#define MAXCS 1024
static pthread_mutex_t cs_tab[MAXCS];
static int ncs;

static pthread_mutex_t *cs_get(uint32_t cs)
{
    if (R32(cs + 0x14) != CS_MAGIC)
        fatal("critical section %08x used before InitializeCriticalSection", cs);
    return &cs_tab[R32(cs + 0x10)];
}

API(KERNEL32, InitializeCriticalSection, 1)
{
    uint32_t cs = ARG(0);
    pthread_mutex_lock(&klock);
    if (ncs >= MAXCS)
        fatal("too many critical sections");
    int i = ncs++;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs_tab[i], &a);
    pthread_mutexattr_destroy(&a);
    pthread_mutex_unlock(&klock);
    memset(gptr(cs), 0, 24);
    W32(cs + 4, 0xffffffffu);            /* LockCount */
    W32(cs + 0x10, i);
    W32(cs + 0x14, CS_MAGIC);
    return 0;
}

/* (the spin count makes no difference here) */
API(KERNEL32, InitializeCriticalSectionAndSpinCount, 2)
{
    w_KERNEL32_InitializeCriticalSection_body(c);
    return 1;
}

API(KERNEL32, DeleteCriticalSection, 1)
{
    W32(ARG(0) + 0x14, 0);
    return 0;
}

API(KERNEL32, EnterCriticalSection, 1)
{
    if (deterministic) {                /* held by a thread waiting its turn: let it run */
        while (pthread_mutex_trylock(cs_get(ARG(0))) != 0)
            det_yield();
        return 0;
    }
    pthread_mutex_lock(cs_get(ARG(0)));
    return 0;
}

API(KERNEL32, TryEnterCriticalSection, 1)
{
    return pthread_mutex_trylock(cs_get(ARG(0))) == 0;
}

API(KERNEL32, LeaveCriticalSection, 1)
{
    pthread_mutex_unlock(cs_get(ARG(0)));
    return 0;
}

void sync_init(void) {}

/* In a forked child (the register audit's, fncheck's): the critical sections the forking thread
   held still name its kernel thread id as their owner, and the child's thread has another, so
   re-entering one (std::_Lockit taken again inside the audited call) failed for ever. They are
   given to the child's thread, as held by it. (glibc's recursive mutex keeps the owner's tid;
   elsewhere this does nothing.) */
void sync_fork_child(int old_tid)
{
#if defined(__linux__) && defined(__GLIBC__)
    int now = (int)gettid();
    for (int i = 0; i < ncs; i++)
        if (cs_tab[i].__data.__owner == old_tid)
            cs_tab[i].__data.__owner = now;
#else
    (void)old_tid;
#endif
}

/* ------------------------------------------------------------------ more objects (LA Rush) */
typedef struct { int count, max; } Semaphore;

int event_reset(uint32_t h)
{
    pthread_mutex_lock(&klock);
    Event *e = handle_get(h, H_EVENT);
    if (e)
        e->signalled = 0;
    pthread_mutex_unlock(&klock);
    return e != NULL;
}

API(KERNEL32, ResetEvent, 1) { return event_reset(ARG(0)); }

API(KERNEL32, CreateSemaphoreA, 4)
{
    Semaphore *s = calloc(1, sizeof *s);
    s->count = (int)ARG(1);
    s->max = (int)ARG(2);
    uint32_t h = handle_new(H_SEMAPHORE, s);
    LOG(LOG_THREAD, "CreateSemaphoreA(%d, %d) -> %x", s->count, s->max, h);
    return h;
}

API(KERNEL32, ReleaseSemaphore, 3)
{
    pthread_mutex_lock(&klock);
    Semaphore *s = handle_get(ARG(0), H_SEMAPHORE);
    int ok = s && s->count + (int)ARG(1) <= s->max;
    if (ok) {
        if (ARG(2)) W32(ARG(2), (uint32_t)s->count);
        s->count += (int)ARG(1);
        kick();
    }
    pthread_mutex_unlock(&klock);
    if (!ok) set_last_error(298);       /* ERROR_TOO_MANY_POSTS */
    return ok;
}

int semaphore_signalled_locked(uint32_t h) { Semaphore *s = handle_get(h, H_SEMAPHORE); return s && s->count > 0; }
void semaphore_take_locked(uint32_t h) { Semaphore *s = handle_get(h, H_SEMAPHORE); if (s) s->count--; }

/* A new handle to the same object, as Windows (SwARc 76e4a75): closing the original leaves the
   duplicate working. GetCurrentThread()'s 0xfffffffe becomes a real handle to the calling thread
   (it meant "the caller" to whichever thread used it later: a watchdog waiting on the main thread
   saw it signalled at once); the main thread gets a record of its own that never finishes. Files
   and finds keep their one handle (their objects go when it is closed). DUPLICATE_CLOSE_SOURCE (1)
   closes the source. */
static Thread main_thread_rec;          /* (the main thread's, for its duplicated handle) */
API(KERNEL32, DuplicateHandle, 7)
{
    uint32_t h = ARG(1), nh = h;
    int type = handle_type(h);
    if (h == 0xfffffffeu) {
        CPU *self = current_cpu();
        Thread *t = self ? self->thread : NULL;
        if (!t) {
            main_thread_rec.cpu = self;
            t = &main_thread_rec;
        }
        nh = handle_new(H_THREAD, t);
    } else if (type == H_EVENT || type == H_MUTEX || type == H_THREAD || type == H_SEMAPHORE || type == H_MISC) {
        nh = handle_new(type, handle_get(h, type));
        if (ARG(6) & 1)
            handle_close(h);
    }
    LOG(LOG_THREAD, "DuplicateHandle(%x) -> %x", h, nh);
    if (ARG(3)) W32(ARG(3), nh);
    return 1;
}

API(KERNEL32, SuspendThread, 1) { LOG(LOG_ALWAYS, "SuspendThread(%x) ignored", ARG(0)); return 0; }

/* only a thread created suspended can be resumed (suspending a running one is not done) */
API(KERNEL32, ResumeThread, 1)
{
    pthread_mutex_lock(&klock);
    Thread *t = handle_get(ARG(0), H_THREAD);
    uint32_t prev = t ? (uint32_t)t->suspended : 0xffffffffu;
    if (t && t->suspended > 0) {
        t->suspended--;
        kick();
    }
    pthread_mutex_unlock(&klock);
    return prev;
}
API(KERNEL32, GetExitCodeThread, 2)
{
    pthread_mutex_lock(&klock);
    Thread *t = handle_get(ARG(0), H_THREAD);
    if (t && ARG(1)) W32(ARG(1), t->finished ? t->exit_code : 259);       /* STILL_ACTIVE */
    pthread_mutex_unlock(&klock);
    if (!t && ARG(0) == 0xfffffffeu) { if (ARG(1)) W32(ARG(1), 259); return 1; }   /* the calling thread */
    if (!t) set_last_error(6);
    return t != NULL;
}

/* a host thread cannot be stopped from outside: refused, the thread runs on */
API(KERNEL32, TerminateThread, 2)
{
    LOG(LOG_ALWAYS, "TerminateThread(%x, %u) refused", ARG(0), ARG(1));
    set_last_error(5);
    return 0;
}

API(KERNEL32, GetCurrentThread, 0) { return 0xfffffffeu; }
API(KERNEL32, GetCurrentThreadId, 0) { return current_thread_id(); }
API(KERNEL32, GetCurrentProcessId, 0) { return 0x7d0u; }

uint32_t current_thread_id(void)
{
    CPU *c = current_cpu();
    return c ? R32(c->fs_base + 0x24) : 0x100u;
}

API(KERNEL32, InterlockedIncrement, 1)
{
    return __atomic_add_fetch((uint32_t *)gptr(ARG(0)), 1u, __ATOMIC_SEQ_CST);
}

API(KERNEL32, InterlockedDecrement, 1)
{
    return __atomic_sub_fetch((uint32_t *)gptr(ARG(0)), 1u, __ATOMIC_SEQ_CST);
}

/* ------------------------------------------------------------------ thread-local storage
   The slots live in the thread's TEB (TlsSlots at +0xe10), as on Windows. */
static uint8_t tls_used[64];

API(KERNEL32, TlsAlloc, 0)
{
    pthread_mutex_lock(&klock);
    for (int i = 0; i < 64; i++)
        if (!tls_used[i]) {
            tls_used[i] = 1;
            pthread_mutex_unlock(&klock);
            return (uint32_t)i;
        }
    pthread_mutex_unlock(&klock);
    return 0xffffffffu;
}

static void tls_clear_slot(CPU *t, void *arg)
{
    if (t->fs_base)
        W32(t->fs_base + 0xe10 + 4 * *(uint32_t *)arg, 0);
}

/* the slot is cleared in every thread, as Windows does, so an index handed out again starts at 0 in
   each (it kept the last owner's pointers); an index not allocated fails (SwARc 76e4a75) */
API(KERNEL32, TlsFree, 1)
{
    uint32_t i = ARG(0);
    pthread_mutex_lock(&klock);
    int ok = i < 64 && tls_used[i];
    if (ok) tls_used[i] = 0;
    pthread_mutex_unlock(&klock);
    if (!ok) { set_last_error(87); return 0; }
    extern int cpu_each(void (*fn)(CPU *c, void *arg), void *arg);
    cpu_each(tls_clear_slot, &i);
    return 1;
}

API(KERNEL32, TlsGetValue, 1)
{
    if (ARG(0) >= 64) { set_last_error(87); return 0; }
    set_last_error(0);
    return R32(c->fs_base + 0xe10 + 4 * ARG(0));
}

API(KERNEL32, TlsSetValue, 2)
{
    if (ARG(0) >= 64) { set_last_error(87); return 0; }
    W32(c->fs_base + 0xe10 + 4 * ARG(0), ARG(1));
    return 1;
}

/* SetEvent for the runtime (DirectSound's position notifications) */
int event_set(uint32_t h)
{
    pthread_mutex_lock(&klock);
    Event *e = handle_get(h, H_EVENT);
    if (e) {
        e->signalled = 1;
        if (deterministic && tok < 0 && nparts)  /* (everyone was waiting) */
            det_schedule(nparts - 1, 0);
        kick();
    }
    pthread_mutex_unlock(&klock);
    return e != NULL;
}

/* ------------------------------------------------------------------ gathered from the other ports (AntiStatic) */
/* (from etm-native) */
/* ------------------------------------------------------------------ completion routines (APCs)
   ReadFileEx completes at once (files.c) and queues its completion routine for the calling
   thread; Windows runs it at that thread's next alertable wait, and so do we. Only the thread
   itself queues (nothing calls QueueUserAPC), so an alertable wait with nothing queued is an
   ordinary wait. */
typedef struct Apc { struct Apc *next; uint32_t tid, fn, a, b, c; } Apc;
static Apc *apcs;
static pthread_mutex_t apc_lock = PTHREAD_MUTEX_INITIALIZER;

void apc_queue(uint32_t fn, uint32_t a, uint32_t b, uint32_t c)
{
    Apc *q = calloc(1, sizeof *q);
    q->tid = current_thread_id();
    q->fn = fn; q->a = a; q->b = b; q->c = c;
    pthread_mutex_lock(&apc_lock);
    Apc **pp = &apcs;
    while (*pp)
        pp = &(*pp)->next;
    *pp = q;
    pthread_mutex_unlock(&apc_lock);
}

/* run this thread's queued routines (stdcall, three arguments); the number run */
static int apc_run(CPU *c)
{
    uint32_t tid = current_thread_id();
    int n = 0;
    for (;;) {
        pthread_mutex_lock(&apc_lock);
        Apc **pp = &apcs, *q = NULL;
        for (; *pp; pp = &(*pp)->next)
            if ((*pp)->tid == tid) {
                q = *pp;
                *pp = q->next;
                break;
            }
        pthread_mutex_unlock(&apc_lock);
        if (!q)
            return n;
        guest_call(c, q->fn, 3, q->a, q->b, q->c);
        free(q);
        n++;
    }
}

/* (handle, ms, alertable): WAIT_IO_COMPLETION (0xc0) once queued routines ran */
API(KERNEL32, WaitForSingleObjectEx, 3)
{
    if (ARG(2) && apc_run(c))
        return 0xc0;
    return wait_handle(ARG(0), ARG(1));
}
