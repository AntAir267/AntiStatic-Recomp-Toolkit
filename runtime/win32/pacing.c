/* PACING: waiting for a time without burning a CPU core.

   sleep_until_ns sleeps to an absolute time on the monotonic clock with the thread's timer slack
   cut to 1 microsecond (Linux's default 50 us would make every wake-up that late), so a frame
   limiter needs no spin at the end.

   The frame pacer (frame_limit in main.c) puts frame starts on a grid of exact periods: wake-up
   latency doesn't add up, a late frame is followed by a shorter wait, and a frame more than a
   whole period late moves the grid instead of being followed by a burst. (SwARc's FramePacer,
   src/runtime/pacing.cpp, MIT; written for this runtime.)

   The clock-spin guard (recomp-kit's runtime/clock_spin.h idea, MIT; ours is stricter): a game
   that waits by reading GetTickCount / timeGetTime in a tight loop until the value moves keeps
   a core at 100% for nothing. When a thread reads the same millisecond 16 times in a row with
   no other Windows call anywhere in between, it sleeps to the next millisecond before
   answering: the values the game sees, and when they change, are what it would have seen.
   Off in --test runs (the virtual clock), and in a port with PORT_CLOCK_SPIN 0. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <time.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#ifndef PACING_TEST
#include "runtime.h"
#else                                   /* (tests/pacing: this file alone) */
typedef struct { uint64_t next, period; } Pacer;
extern unsigned long api_calls;
#endif

uint64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

void sleep_until_ns(uint64_t t)
{
#ifdef __linux__
    static __thread int slack_set;
    if (!slack_set) {
        prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
        slack_set = 1;
    }
#endif
    struct timespec ts = {(time_t)(t / 1000000000u), (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

/* the frame pacer: call at each frame's end with the rate; fps <= 0: no limit (the grid starts
   again at the next limited frame). Returns the nanoseconds it waited. */
uint64_t pacer_wait(Pacer *p, double fps)
{
    if (!(fps > 0)) {
        p->next = 0;
        return 0;
    }
    uint64_t period = (uint64_t)(1e9 / fps + 0.5), now = mono_ns(), waited = 0;
    if (!p->next || period != p->period || now >= p->next + period)
        p->next = now;                  /* (first frame, new rate, or a whole period late) */
    else if (now < p->next) {
        sleep_until_ns(p->next);
        waited = p->next - now;
    }
    p->period = period;
    p->next += period;
    return waited;
}

#ifndef PORT_CLOCK_SPIN
#define PORT_CLOCK_SPIN 1
#endif
#define SPIN_POLLS 16

/* the clock-spin guard, for a millisecond clock read (GetTickCount, timeGetTime): `ms` is the
   value about to be answered, `read` reads the clock again. Returns the value to answer. */
uint32_t clock_spin_poll(uint32_t ms, uint32_t (*read)(void))
{
    static __thread uint32_t last_ms, same;
    static __thread unsigned long last_calls;
    extern int deterministic;
    if (!PORT_CLOCK_SPIN || deterministic)
        return ms;
    /* (api_calls counts every thread's Windows calls: one more than last time means nothing
       else happened in between, here or anywhere) */
    unsigned long calls = api_calls;
    int alone = calls == last_calls + 1;
    last_calls = calls;
    if (ms != last_ms || !alone) {
        last_ms = ms;
        same = 0;
        return ms;
    }
    if (++same < SPIN_POLLS)
        return ms;
    same = 0;
    for (int n = 0; n < 4 && (ms = read()) == last_ms; n++)   /* to the next millisecond */
        sleep_until_ns(mono_ns() + 250000);
    last_ms = ms;
    return ms;
}
