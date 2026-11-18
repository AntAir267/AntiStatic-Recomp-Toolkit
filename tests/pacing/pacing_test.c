/* tests/pacing: runtime/win32/pacing.c on its own (the frame pacer and the clock-spin guard),
   on the host's real clock: rate, CPU use, a late frame, a game spinning on its clock. */
#define PACING_TEST
#include "../../runtime/win32/pacing.c"
#include <stdio.h>
#include <stdlib.h>

unsigned long api_calls;
int deterministic;

static double cpu_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static double wall_ms(void) { return mono_ns() / 1e6; }
static int fails;
static void check(int ok, const char *what, double got)
{
    printf("%-46s %10.3f  %s\n", what, got, ok ? "ok" : "FAILED");
    if (!ok) fails++;
}

static uint64_t t0;
static uint32_t game_clock(void) { return (uint32_t)((mono_ns() - t0) / 1000000u); }

int main(void)
{
    /* 120 frames at 60 fps with nothing to do: 2 s, and next to no CPU */
    Pacer p = {0, 0};
    double w0 = wall_ms(), c0 = cpu_ms();
    for (int i = 0; i <= 120; i++)
        pacer_wait(&p, 60);
    double wall = wall_ms() - w0, cpu = cpu_ms() - c0;
    check(wall > 1980 && wall < 2020, "60 fps, 120 frames: ms (2000 +- 1%)", wall);
    check(cpu < 0.03 * wall, "  CPU used while waiting: % of the time", 100 * cpu / wall);

    /* a 50 ms frame (three periods late): the grid moves, the next frames are not a burst */
    pacer_wait(&p, 60);
    sleep_until_ns(mono_ns() + 50000000u);
    pacer_wait(&p, 60);
    double a = wall_ms();
    pacer_wait(&p, 60);
    pacer_wait(&p, 60);
    double two = wall_ms() - a;
    check(two > 32 && two < 35.5, "after a late frame, two frames: ms (33.3)", two);

    /* a game waiting 40 ms by reading its millisecond clock in a loop */
    t0 = mono_ns();
    uint32_t start = game_clock(), v = start, prev = start, polls = 0, backwards = 0;
    c0 = cpu_ms();
    while ((v = clock_spin_poll(game_clock(), game_clock)) - start < 40) {
        api_calls++;                    /* (what api_return does after each call) */
        if (v < prev) backwards++;
        prev = v;
        polls++;
    }
    double spent = (mono_ns() - t0) / 1e6;
    cpu = cpu_ms() - c0;
    check(spent >= 40 && spent < 42, "spinning wait for 40 ms: ms", spent);
    check(cpu < 0.25 * spent, "  CPU used: % of the time (a spin: 100)", 100 * cpu / spent);
    check(!backwards, "  clock went backwards: times", backwards);
    printf("  (%u polls)\n", polls);
    printf(fails ? "pacing: FAILED\n" : "pacing: ok\n");
    return fails != 0;
}
