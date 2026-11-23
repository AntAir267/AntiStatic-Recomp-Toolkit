/* Start-up: map the user's LARush.exe and the DLLs translated with it at their own addresses,
   connect their imports (to the runtime or to each other), run each DLL's entry and then the
   program's on the main thread. Also: calls through pointers, callbacks into the program, and
   the error paths of the generated code. */
#define _GNU_SOURCE
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "runtime.h"
#include "image.h"
#include "input.h"
#include "options.h"
#include "pad.h"

unsigned log_mask = LOG_ALWAYS;
char game_dir[1024];
char user_dir[1024];
uint32_t image_base = IMAGE_BASE;
CPU *main_cpu;
int headless = 1;
int hidden_window;                      /* --hidden: the window DXVK needs, never shown (tests) */
static FILE *logfp;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec t_start;

/* ------------------------------------------------------------------ logging */
/* Test mode (--test): a virtual clock that moves 1/60 s per presented frame and a microsecond
   per read (so busy-waits still end), timers run on the main thread at frame ends, nothing
   depends on the host's timing, and two runs with the same scripted input match exactly. */
int deterministic;
static double vclock;

static double real_ms(void);
double frame_clock_ms(void);                    /* the clock frames move by (the virtual one in --test) */

/* The device recorder (a port's devrec.c, for the function checker's replays) hooks these; without
   one they do nothing. */
/* The port's frame-rate code hears each presented frame's duration here, if it has any. */
__attribute__((weak)) void port_frame_time(double ms) { (void)ms; }

/* The port's last set-up before the program starts, after game_init (installing hand-written
   replacement functions, late hooks), if it has any. */
__attribute__((weak)) void port_runtime_ready(void) {}

/* The port's display layer sets itself up here (a DirectDraw port: ddraw_init), if it needs to. */
__attribute__((weak)) void port_graphics_init(void) {}

__attribute__((weak)) int dev_recording;
__attribute__((weak)) int api_via_thunk;   /* import calls through their thunks (while recording) */
__attribute__((weak)) void capture_return(CPU *c, uint32_t fn) { (void)c; (void)fn; }
__attribute__((weak)) void devrec_call_begin(CPU *c, uint32_t t) { (void)c; (void)t; }
__attribute__((weak)) void devrec_call_end(CPU *c, uint32_t t, const char *name) { (void)c; (void)t; (void)name; }
__attribute__((weak)) void devrec_alloc(uint32_t addr, uint32_t size) { (void)addr; (void)size; }
__attribute__((weak)) void devrec_others_begin(void) {}
__attribute__((weak)) void devrec_others_end(void) {}
__attribute__((weak)) void devrec_upload(uint32_t addr, uint32_t len) { (void)addr; (void)len; }
__attribute__((weak)) void devrec_start(const char *cap_path, uint32_t fn, CPU *c) { (void)cap_path; (void)fn; (void)c; }

char cd_dir[1024];                      /* the CD's files (port.h PORT_CD_LABEL) */

const char *port_getenv(const char *name)
{
    char b[160];
    snprintf(b, sizeof b, PORT_ENV "_%s", name);
    return getenv(b);
}

const char *port_exe_path(void) { return *PORT_START_DIR ? PORT_START_DIR "\\" PORT_EXE : PORT_EXE; }
/* the program's name as its command line shows it: the exe's path, or PORT_CMDLINE_EXE when the
   port's recorded runs had another (li2-native's runtime said "Game.exe" for "LEGO Island 2.exe") */
const char *port_cmdline_exe(void)
{
#if defined(PORT_CMDLINE_EXE)
    return PORT_CMDLINE_EXE;
#else
    return port_exe_path();
#endif
}
double host_ms(void) { return real_ms(); }      /* the host's clock, whatever --test does */

static double real_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t_start.tv_sec) * 1000.0 + (t.tv_nsec - t_start.tv_nsec) / 1e6;
}
double frame_clock_ms(void) { return deterministic ? vclock : real_ms(); }
/* --test with PORT_TEST_SOUND_AT_FRAME: the mixer's clock is the virtual clock as it stood at the
   last presented frame, not as waits and sleeps move it between frames (Off-Road's lockstep) */
static double vclock_frame;
#if defined(PORT_TEST_SOUND_AT_FRAME)
static double test_sound_clock(void) { return vclock_frame; }
#endif
/* --test: time a wait lets pass on the virtual clock (a wait for nothing in particular) */
void vclock_advance(double ms) { if (deterministic) vclock += ms; }
/* --test, PORT_TEST_SCHEDULER 2: the main thread's Sleep moves the virtual clock (0.1 ms for 0) */
void vclock_sleep(double ms)
{
    if (deterministic && current_cpu() == main_cpu)
        vclock += ms > 0 ? ms : 0.1;
}

/* --test: each reading moves the clock a microsecond, so a wait on time ends. With
   PORT_TEST_CLOCK_PER_THREAD only the main thread's readings move it, other threads seeing it
   plus their own readings: how often they read it depends on real scheduling, which made two
   of Hard Truck 2's runs differ [verified: HT2 lockstep 2026-10-26, timing values at 0x55cbd60] */
#if defined(PORT_TEST_CLOCK_PER_THREAD)
static __thread double vlocal;
#endif
double now_ms(void)
{
    if (deterministic) {
#if defined(PORT_TEST_CLOCK_PER_THREAD)
        CPU *c = current_cpu();
        if (c && c->thread_index != 0) {    /* (the first thread is the main one) */
            vlocal += 0.001;
            return vclock + vlocal;
        }
#endif
        vclock += 0.001;
        return vclock;
    }
    return real_ms();
}

/* With a log file everything goes there; LOG_ALWAYS messages are also echoed to stderr. */
static void vlog(unsigned cat, const char *fmt, va_list ap)
{
    char buf[4096];
    vsnprintf(buf, sizeof buf, fmt, ap);
    CPU *c = current_cpu();
    pthread_mutex_lock(&log_lock);
    double t = deterministic ? vclock : real_ms();
    int th = c ? c->thread_index : -1;
    if (logfp) {
        fprintf(logfp, "%9.3f [%d] %s\n", t / 1000.0, th, buf);
        fflush(logfp);
    }
    if (!logfp || cat == LOG_ALWAYS)
        fprintf(stderr, "%9.3f [%d] %s\n", t / 1000.0, th, buf);
    pthread_mutex_unlock(&log_lock);
}

int log_fd(void) { return logfp ? fileno(logfp) : -1; }

void logmsg(unsigned cat, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(cat, fmt, ap);
    va_end(ap);
}

static void dump_state(CPU *c)
{
    if (!c)
        return;
    logmsg(LOG_ALWAYS, "  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x",
           c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    if (c->esp >= ARENA_START && c->esp < ARENA_END) {
        char line[256];
        int n = 0;
        for (int i = 0; i < 12; i++)
            n += snprintf(line + n, sizeof line - n, " %08x", R32(c->esp + 4 * i));
        logmsg(LOG_ALWAYS, "  stack:%s", line);
    }
    void *bt[64];
    int nb = backtrace(bt, 64);
    char **sym = backtrace_symbols(bt, nb);
    for (int i = 0; i < nb && sym; i++) {
        const char *p = strstr(sym[i], "(F_");
        const char *q = strstr(sym[i], "(w_");
        if (p || q || i < 6)
            logmsg(LOG_ALWAYS, "  at %s", sym[i]);
    }
    free(sym);
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logmsg(LOG_ALWAYS, "FATAL: %s", buf);
    extern void fncheck_fatal(const char *msg) __attribute__((weak));
    if (fncheck_fatal)
        fncheck_fatal(buf);             /* (a --fncheck case or child ends here: runtime/rewrite/fncheck.c) */
    dump_state(current_cpu());
    if (logfp)
        fflush(logfp);
    _exit(2);
}

/* ------------------------------------------------------------------ calls through pointers */
/* each module's code ranges and the translated function at each address */
typedef struct { uint32_t lo, hi; void (**fns)(CPU *); } TextRange;
static TextRange text_ranges[8 * MODULE_COUNT];
static int ntext_ranges;
static void (*thunk_fns[THUNK_MAX])(CPU *);
static const char *thunk_names[THUNK_MAX];
static int nthunks;
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t thunk_addr(void (*fn)(CPU *c), const char *name)
{
    pthread_mutex_lock(&thunk_lock);
    for (int i = 0; i < nthunks; i++)
        if (thunk_fns[i] == fn) {
            pthread_mutex_unlock(&thunk_lock);
            return THUNK_BASE + 16u * i;
        }
    if (nthunks >= (int)THUNK_MAX)
        fatal("out of thunks");
    thunk_fns[nthunks] = fn;
    thunk_names[nthunks] = name;
    uint32_t a = THUNK_BASE + 16u * nthunks++;
    pthread_mutex_unlock(&thunk_lock);
    return a;
}

const char *thunk_name(uint32_t a)
{
    if (a - THUNK_BASE < 16u * nthunks && !(a & 15))
        return thunk_names[(a - THUNK_BASE) >> 4];
    return NULL;
}

/* The reference build (ref_cpu.c) runs the original code in an emulator instead. */
void (*icall_hook)(CPU *c, uint32_t target);

/* LOG_DRAW: every Direct3D device call with its first arguments */
static void thunk_trace(CPU *c, uint32_t t)
{
    const char *n = thunk_names[(t - THUNK_BASE) >> 4];
    if (n && !strncmp(n, "IDirect3DDevice9::", 18))
        LOG(LOG_DRAW, "%s(%x, %x, %x, %x, %x)", n + 18, R32(c->esp + 8), R32(c->esp + 12),
            R32(c->esp + 16), R32(c->esp + 20), R32(c->esp + 24));
    if (n && !strcmp(n, "IDirect3DDevice9::SetVertexShaderConstantF"))
        for (uint32_t i = 0, p = R32(c->esp + 12); i < R32(c->esp + 16) && i < 8; i++, p += 16) {
            float v[4];
            memcpy(v, gptr(p), 16);
            LOG(LOG_DRAW, "    c%u = %g %g %g %g", R32(c->esp + 8) + i, v[0], v[1], v[2], v[3]);
        }
}

/* The offline checker (tests/fncheck.c) plays recorded calls back in place of the runtime
   (devrec.c): thunk_replay does the call, thunk_replay_name names the thunk (its own tables are
   empty there). */
int (*thunk_replay)(CPU *c, uint32_t t);
const char *(*thunk_replay_name)(uint32_t t);
void devrec_call_begin(CPU *c, uint32_t t), devrec_call_end(CPU *c, uint32_t t, const char *name);

static int is_thunk(uint32_t t)
{
    return !(t & 15) && t - THUNK_BASE < 16u * (thunk_replay ? THUNK_MAX : (uint32_t)nthunks);
}

/* a thunk's call: traced (LOG_DRAW), hashed (--hash), recorded (devrec.c) or played back */
/* a port's look at every call the program makes into the runtime through a thunk (COM methods,
   Direct3D: Enter the Matrix's and L.A. Rush's D3D inventories), before (after 0) and after (1) */
void (*api_call_hook)(CPU *c, const char *name, int after);
/* the same with the thunk's index (0 up, below THUNK_MAX: a profiler's table of calls), for a port's
   own use: dialog.c and the D3D inventories take api_call_hook */
void (*port_thunk_hook)(CPU *c, uint32_t index, const char *name, int after);

/* --trace-calls FILE: every call into the runtime by name (imports too: they go through their
   thunks for the run), one line each: frame, call number, return address, name */
static FILE *calls_fp;

static void run_thunk(CPU *c, uint32_t t)
{
    const char *n = thunk_replay ? thunk_replay_name(t) : thunk_names[(t - THUNK_BASE) >> 4];
    if (calls_fp)
        fprintf(calls_fp, "%lu %lu %08x %s\n", frame_count, api_calls, R32(c->esp), n ? n : "?");
    if (api_call_hook && !thunk_replay)
        api_call_hook(c, n, 0);
    if (port_thunk_hook && !thunk_replay)
        port_thunk_hook(c, (t - THUNK_BASE) >> 4, n, 0);
    if ((log_mask & LOG_DRAW) && !thunk_replay)
        thunk_trace(c, t);
    if (draw_hashing && n && !strncmp(n, "IDirect3DDevice9::", 18))
        draw_hash_call(c, n + 18);
    if (thunk_replay) {
        thunk_replay(c, t);
        return;
    }
    if (dev_recording) devrec_call_begin(c, t);
    thunk_fns[(t - THUNK_BASE) >> 4](c);
    if (dev_recording) devrec_call_end(c, t, n);
    if (port_thunk_hook)
        port_thunk_hook(c, (t - THUNK_BASE) >> 4, n, 1);
    if (api_call_hook)
        api_call_hook(c, n, 1);
}

int call_thunk(CPU *c, uint32_t t)
{
    if (!is_thunk(t))
        return 0;
    run_thunk(c, t);
    return 1;
}

/* a jump through a pointer: an indirect tail call, except while an exception is dispatched, when
   a jump to a place a live frame goes on at resumes that frame (an SEH4 handler going into an
   __except block that is also a translated function; eh.c). Calls never resume: that is how
   handlers call funclets. */
void ijump(CPU *c, uint32_t t)
{
    extern __thread int eh_dispatching;
    extern int eh_resume_place(CPU *c, uint32_t target);
    if (__builtin_expect(eh_dispatching, 0) && eh_resume_place(c, t))
        return;
    icall(c, t);
}

void icall(CPU *c, uint32_t t)
{
    if (icall_hook) {
        icall_hook(c, t);
        return;
    }
    if (is_thunk(t)) {                  /* (first: most calls through pointers are Direct3D's and COM's;
                                           L.A. Rush's race 2.53 -> 2.44 ms a frame) */
        run_thunk(c, t);
        return;
    }
    for (int i = 0; i < ntext_ranges; i++) {
        const TextRange *r = &text_ranges[i];
        if (t - r->lo < r->hi - r->lo) {
            void (*fn)(CPU *) = r->fns[t - r->lo];
            if (fn) {
                fn(c);
                return;
            }
            /* an __except block called by the C runtime: a place an exception goes on at (eh.c) */
            extern int eh_resume_call(CPU *c, uint32_t t);
            if (eh_resume_call(c, t))
                return;                  /* (does not come back) */
            fatal("call to %08x: code the translator did not find as a function (add it to "
                  "EXTRA_ENTRIES), return address %08x", t, R32(c->esp));
        }
    }
    /* an ATL window thunk the program wrote into memory (CWndProcThunk: mov dword [esp+4], this;
       jmp WindowProc): the window procedure gets the object for the window handle, as the thunk
       would (from penguins-native: Penguins! writes C7 44 24 04 <this> E9 <rel32>) */
    extern uint32_t vm_query_base(uint32_t addr);
    if (vm_query_base(t) && R32(t) == 0x042444c7u && R8(t + 8) == 0xe9) {
        W32(c->esp + 4, R32(t + 4));
        icall(c, t + 13 + R32(t + 9));
        return;
    }
    fatal("call to %08x: not code (return address %08x)", t, R32(c->esp));
}

#define SENTINEL 0x7efe0000u

uint32_t guest_callv(CPU *c, uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t keep[8] = {c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi};
    uint32_t keep_ret = c->ret, keep_df = c->df, keep_fl = c->eflags;
    uint32_t esp = c->esp - 64;      /* leave the caller's outgoing area alone */
    for (int i = nargs - 1; i >= 0; i--) {
        esp -= 4;
        W32(esp, args[i]);
    }
    esp -= 4;
    W32(esp, SENTINEL);
    c->esp = esp;
    c->df = 0;
    icall(c, fn);
    if (c->ret != SENTINEL)
        fatal("callback %08x returned to %08x", fn, c->ret);
    uint32_t r = c->eax;
    c->eax = keep[0]; c->ecx = keep[1]; c->edx = keep[2]; c->ebx = keep[3];
    c->esp = keep[4]; c->ebp = keep[5]; c->esi = keep[6]; c->edi = keep[7];
    c->ret = keep_ret;
    c->df = keep_df;
    c->eflags = keep_fl;
    return r;
}

uint32_t guest_call(CPU *c, uint32_t fn, int nargs, ...)
{
    uint32_t args[16];
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++)
        args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return guest_callv(c, fn, nargs, args);
}

/* ------------------------------------------------------------------ error paths */
void bad_ret(CPU *c, uint32_t site, uint32_t expected)
{
    fatal("call at %08x returned to %08x instead of %08x", site, c->ret, expected);
}
void bad_jump(CPU *c, uint32_t site, uint32_t target)
{
    (void)c;
    fatal("jump table at %08x went to %08x, not a known case", site, target);
}
/* a jump that leaves its function for a place in a live one (a translated longjmp's last jump,
   build config NONLOCAL_JUMPS): esp and the registers are already the ones it goes on with */
void nonlocal_jump(CPU *c, uint32_t site, uint32_t target)
{
    extern int eh_resume_at(CPU *c, uint32_t target, uint32_t esp, uint32_t ebp);
    if (!eh_resume_at(c, target, c->esp, c->ebp))
        fatal("nonlocal jump at %08x to %08x, which no live function holds (a setjmp the translator "
              "did not see: build config SETJMP; or a catch / __except place the exception tables "
              "didn't give: translator/eh.py)", site, target);
}
void unsupported(CPU *c, uint32_t site, const char *what)
{
    (void)c;
    fatal("instruction at %08x not translated: %s", site, what);
}
void trap_int3(CPU *c, uint32_t site)
{
    (void)c;
    fatal("int3 at %08x", site);
}
void div_fault(CPU *c, uint32_t site)
{
    (void)c;
    fatal("integer divide fault at %08x", site);
}
void fell_off(CPU *c, uint32_t site)
{
    (void)c;
    fatal("ran past the end of translated code after %08x", site);
}
void returned_from_noreturn(CPU *c, uint32_t site)
{
    (void)c;
    fatal("call at %08x to a function that never returns came back", site);
}
void missing_import(CPU *c, const char *dll, const char *name)
{
    fatal("%s!%s is not implemented (called from %08x)", dll, name, R32(c->esp));
}

/* ------------------------------------------------------------------ threads' CPU records */
static __thread CPU *tls_cpu;
CPU *current_cpu(void) { return tls_cpu; }
void set_current_cpu(CPU *c) { tls_cpu = c; }
static int thread_counter;
#define MAX_CPUS 64
static CPU *all_cpus[MAX_CPUS];          /* (records are never freed) */

/* every thread's CPU record so far, for what must reach them all (TlsFree clearing a slot) */
int cpu_each(void (*fn)(CPU *c, void *arg), void *arg)
{
    int n = 0, k = thread_counter < MAX_CPUS ? thread_counter : MAX_CPUS;
    for (int i = 0; i < k; i++) {
        CPU *c = __atomic_load_n(&all_cpus[i], __ATOMIC_ACQUIRE);
        if (c) { fn(c, arg); n++; }
    }
    return n;
}

/* Each thread's dead stack, [stack_lo, esp): what no code can read, for a port's memory checks
   that compare native code against the original (Off-Road's terrain A/B) */
int cpu_dead_stacks(uint32_t *lo, uint32_t *hi, int max)
{
    int n = 0, k = thread_counter < MAX_CPUS ? thread_counter : MAX_CPUS;
    for (int i = 0; i < k && n < max; i++) {
        CPU *c = __atomic_load_n(&all_cpus[i], __ATOMIC_ACQUIRE);
        if (!c)
            continue;
        uint32_t e = c->esp;
        if (e <= c->stack_lo || e > c->stack_hi)
            continue;
        lo[n] = c->stack_lo;
        hi[n] = e;
        n++;
    }
    return n;
}

/* A finished thread's stack, TEB and TLS blocks go to the next thread made with that stack size,
   as Windows frees them (they were never given back, and a game making a thread per job ran out of
   address space: SwARc 76e4a75 saw it after ~1,900). Kept for reuse, not freed, so every other
   allocation stays where it is. */
#define MAX_SPARES 64
static struct { uint32_t stack, size, teb, tls; } spares[MAX_SPARES];
static int nspares;
static pthread_mutex_t spare_lock = PTHREAD_MUTEX_INITIALIZER;

/* the calling thread is ending (its guest code has returned or called ExitThread, its TLS detach
   callbacks have run): what it had goes to the next thread */
void cpu_release(CPU *c)
{
    if (!c->stack_lo || !c->fs_base)
        return;
    pthread_mutex_lock(&spare_lock);
    if (nspares < MAX_SPARES) {
        spares[nspares].stack = c->stack_lo;
        spares[nspares].size = c->stack_hi - c->stack_lo;
        spares[nspares].teb = c->fs_base;
        spares[nspares].tls = R32(c->fs_base + 0x2c);
        nspares++;
    }
    pthread_mutex_unlock(&spare_lock);
    c->stack_lo = c->stack_hi = 0;      /* (no dead stack to report: cpu_dead_stacks) */
    c->fs_base = 0;
}

CPU *cpu_new(uint32_t stack_size)
{
    CPU *c = aligned_alloc(64, (sizeof(CPU) + 63) & ~63u);
    memset(c, 0, sizeof *c);
    if (stack_size < 0x10000)
        stack_size = 0x10000;
    uint32_t stack = 0, teb = 0, tls = 0;
    pthread_mutex_lock(&spare_lock);
    for (int i = nspares - 1; i >= 0 && !stack; i--)
        if (spares[i].size == stack_size) {
            stack = spares[i].stack;
            teb = spares[i].teb;
            tls = spares[i].tls;
            spares[i] = spares[--nspares];
        }
    pthread_mutex_unlock(&spare_lock);
    if (stack) {
        extern void vm_zero(uint32_t a, uint32_t n);
        vm_zero(stack, stack_size);     /* (fresh as a new stack; already left out of the hashes) */
    } else {
        stack = vm_alloc(0, stack_size, 0);
        if (!stack)
            fatal("no memory for a thread stack");
        extern void mem_mark_stack(uint32_t base);
        mem_mark_stack(stack);          /* (left out of memory hashes) */
        teb = galloc(0x1000);
    }
    c->stack_lo = stack;
    c->stack_hi = stack + stack_size;
    memset(gptr(teb), 0, 0x1000);
    W32(teb + 0x00, 0xffffffffu);        /* end of the SEH chain */
    W32(teb + 0x04, c->stack_hi);
    W32(teb + 0x08, c->stack_lo);
    W32(teb + 0x18, teb);
    c->fs_base = teb;
    c->esp = c->stack_hi - 16;
    c->thread_index = __atomic_fetch_add(&thread_counter, 1, __ATOMIC_SEQ_CST);
    W32(teb + 0x24, 0x100 + 4 * c->thread_index);
    if (c->thread_index < MAX_CPUS)
        __atomic_store_n(&all_cpus[c->thread_index], c, __ATOMIC_RELEASE);
    extern void tls_thread_init(CPU *c, uint32_t reuse);
    tls_thread_init(c, tls);             /* static TLS blocks (tls.c), a finished thread's again */
    fpu_new(c);                          /* the host FPU is the caller's: not touched here */
    c->fcw = 0x27f;                      /* what Windows gives a new thread */
    return c;
}

/* ------------------------------------------------------------------ loading the modules */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

const ModuleEntry *module_by_name(const char *name)
{
    const char *b = strrchr(name, '\\');
    b = b ? b + 1 : name;
    size_t n = strlen(b);
    for (int i = 0; i < module_count; i++) {
        const char *m = module_table[i].name;
        size_t k = strlen(m);
        /* "MFC71.dll", "mfc71.DLL" or "mfc71" */
        if ((n == k && !strcasecmp(b, m)) || (n + 4 == k && !strncasecmp(b, m, n) && !strcasecmp(m + n, ".dll")))
            return &module_table[i];
    }
    return NULL;
}

const ModuleEntry *module_at(uint32_t a)
{
    for (int i = 0; i < module_count; i++)
        if (a - module_table[i].base < module_table[i].size)
            return &module_table[i];
    return NULL;
}

/* One module at its own base, laid out as Windows maps it (the translation reads nothing from
   it: the code is the generated C; data, resources and tables come from here). */
/* The modules in memory, i from 0 (all are mapped at start-up), for a reference build mapping
   them into an emulator (TDOR3's lockstep): -1 past the last */
int loader_module_at(int i, uint32_t *base, uint32_t *size)
{
    if (i < 0 || i >= module_count)
        return -1;
    *base = module_table[i].base;
    *size = module_table[i].size;
    return i;
}

static void load_module(const ModuleEntry *me)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", game_dir, me->file);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        fatal("cannot open %s: %s", path, strerror(errno));
    struct stat st;
    fstat(fd, &st);
    uint8_t *f = malloc(st.st_size);
    if (read(fd, f, st.st_size) != st.st_size)
        fatal("cannot read %s", path);
    close(fd);
    uint32_t pe = rd32(f + 0x3c);
    if (memcmp(f + pe, "PE\0\0", 4))
        fatal("%s is not a Windows program", path);
    uint16_t nsec = rd16(f + pe + 6), optsz = rd16(f + pe + 20);
    const uint8_t *opt = f + pe + 24;
    if (rd32(opt + 28) != me->pref_base || rd32(opt + 56) != me->size)
        fatal("%s is not the %s this build was translated from (%s)", path, me->name, BUILD_NAME);
    uint32_t size = (me->size + 0xffff) & ~0xffffu;
    void *m;
    if (me->base >= ARENA_START && me->base + size <= ARENA_END) {
        /* inside the program's memory: taken from it like any allocation at a chosen address */
        if (vm_alloc(me->base, size, 0) != me->base)
            fatal("cannot place %s at %08x", me->name, me->base);
        m = (void *)(uintptr_t)me->base;
        memset(m, 0, size);
    } else {
        m = mmap((void *)(uintptr_t)me->base, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m != (void *)(uintptr_t)me->base)
            fatal("cannot map %s at %08x: %s", me->name, me->base, strerror(errno));
    }
    uint32_t hdrsize = rd32(opt + 60);
    memcpy(m, f, hdrsize < (uint32_t)st.st_size ? hdrsize : (uint32_t)st.st_size);
    const uint8_t *sh = opt + optsz;
    for (int i = 0; i < nsec; i++, sh += 40) {
        uint32_t vs = rd32(sh + 8), va = rd32(sh + 12), rs = rd32(sh + 16), ra = rd32(sh + 20);
        uint32_t n = rs < vs || !vs ? rs : vs;
        if (ra + n > (uint32_t)st.st_size)
            fatal("damaged section in %s", path);
        memcpy((uint8_t *)m + va, f + ra, n);
    }
    /* placed somewhere else than it was linked for (build config MODULES base): its base
       relocations move every address it holds, as the Windows loader does */
    if (me->base != me->pref_base) {
        uint32_t delta = me->base - me->pref_base;
        uint32_t rva = rd32(opt + 96 + 8 * 5), rsize = rd32(opt + 96 + 8 * 5 + 4);
        if (rd32(opt + 92) <= 5 || !rva)
            fatal("%s has no relocations: it can't be placed at %08x", me->name, me->base);
        for (uint32_t o = rva; o + 8 <= rva + rsize; ) {
            uint32_t page = R32(me->base + o), bsize = R32(me->base + o + 4);
            if (bsize < 8)
                break;
            for (uint32_t k = 8; k + 2 <= bsize; k += 2) {
                uint16_t e = R16(me->base + o + k);
                if (e >> 12 == 3) {                     /* IMAGE_REL_BASED_HIGHLOW */
                    uint32_t a = me->base + page + (e & 0xfff);
                    W32(a, R32(a) + delta);
                }
            }
            o += bsize;
        }
    }
    free(f);
}

/* An imported variable (MSVC name "?name@@3..." or a static member "?name@class@@0-2...") from a DLL
   that is not translated (wxWidgets: wxDefaultSize, wxAppConsole::ms_appInitFn, which a start-up
   initialiser writes): the slot points at a block of program memory instead of a thunk. wx's
   default size and position are (-1, -1). */
static int is_data_import(const char *n)
{
    const char *at = n[0] == '?' ? strstr(n, "@@") : NULL;
    return at && at[2] >= '0' && at[2] <= '4';
}

static void connect_imports(void)
{
    for (int i = 0; i < import_count; i++) {
        const ImportEntry *e = &import_table[i];
        if (is_data_import(e->name)) {
            uint32_t b = galloc(64);
            memset(gptr(b), !strncmp(e->name, "?wxDefault", 10) ? 0xff : 0, 64);
            W32(e->iat, b);
            LOG(LOG_API, "%s!%s: a variable at %08x", e->dll, e->name, b);
            continue;
        }
        char *name = malloc(strlen(e->dll) + strlen(e->name) + 2);
        sprintf(name, "%s!%s", e->dll, e->name);
        void (*fn)(CPU *c) = e->fn ? e->fn : e->missing;   /* (old translations: missing NULL, fn a stub) */
        if (!fn)
            fatal("%s has no function and no stand-in (translation older than the runtime?)", name);
        W32(e->iat, thunk_addr(fn, name));
    }
    for (int i = 0; i < link_count; i++)
        W32(link_table[i].iat, link_table[i].target);
}

static void build_function_index(void)
{
    for (int k = 0; k < module_count; k++)
        for (int j = 0; j < module_table[k].ntext; j++) {
            TextRange *r = &text_ranges[ntext_ranges++];
            r->lo = module_table[k].text[j][0];
            r->hi = module_table[k].text[j][1];
            r->fns = calloc(r->hi - r->lo, sizeof *r->fns);
        }
    for (int i = 0; i < func_count; i++) {
        uint32_t a = func_table[i].addr;
        for (int j = 0; j < ntext_ranges; j++)
            if (a - text_ranges[j].lo < text_ranges[j].hi - text_ranges[j].lo)
                text_ranges[j].fns[a - text_ranges[j].lo] = func_table[i].fn;
    }
}

/* ------------------------------------------------------------------ crashes */
#if defined(__linux__) && defined(__x86_64__)
/* A crash on a thread without a guest CPU (DXVK's, the driver's, SDL's) has no guest state to show,
   so fatal()'s report was its first line only. Its report is written here from the signal handler
   (from L.A. Rush's crash.c): the faulting address, the instruction's address with the module it
   falls in and the offset there (from /proc/self/maps: addr2line -e MODULE OFFSET names it), the
   thread's name, and a backtrace, each frame by module and offset, then by name where the dynamic
   symbols have one (link -rdynamic). Written with write() alone, no lock and no allocation (the
   thread may have crashed holding the log's lock or the heap's); the names last, as
   backtrace_symbols_fd looks them up under the loader's lock. An alarm ends a report that hangs.
   Then the signal's own action, so the exit status (128 + the signal) and a core dump are what they
   would be without the handler. A guest thread's crash also gets its pc line, then fatal()'s report. */
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
static int crash_log_fd = -1;
static char crash_maps[1 << 20];        /* /proc/self/maps, read at the crash */
static size_t crash_maps_n;

static void crash_out(const char *s, size_t n)
{
    if (crash_log_fd >= 0 && write(crash_log_fd, s, n) < 0) {}
    if (write(2, s, n) < 0) {}
}

void crash_line(const char *fmt, ...)
{
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%9.3f [-] ", (deterministic ? vclock : real_ms()) / 1000.0);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2)
        n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    crash_out(buf, (size_t)n);
}

static void crash_maps_read(void)
{
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    crash_maps_n = 0;
    if (fd < 0)
        return;
    ssize_t r;
    while (crash_maps_n < sizeof crash_maps - 1 &&
           (r = read(fd, crash_maps + crash_maps_n, sizeof crash_maps - 1 - crash_maps_n)) > 0)
        crash_maps_n += (size_t)r;
    close(fd);
    crash_maps[crash_maps_n] = 0;
}

/* the module holding a (its file's name, the mapping's name in brackets, or "?") and the offset from
   where the file is loaded (its first mapping, file offset 0) */
static const char *crash_module(uintptr_t a, uintptr_t *off, char *name, size_t cap)
{
    uintptr_t base = 0;
    char cur[256] = "";
    for (char *l = crash_maps; l < crash_maps + crash_maps_n; ) {
        char *e = memchr(l, '\n', (size_t)(crash_maps + crash_maps_n - l));
        if (!e) e = crash_maps + crash_maps_n;
        unsigned long lo, hi, fo;
        char perms[8];
        int path_at = 0;
        if (sscanf(l, "%lx-%lx %7s %lx %*s %*s %n", &lo, &hi, perms, &fo, &path_at) >= 4) {
            const char *path = path_at && l + path_at < e ? l + path_at : "";
            size_t pn = (size_t)(e - path);
            if (pn >= sizeof cur) pn = sizeof cur - 1;
            if (pn != strlen(cur) || memcmp(cur, path, pn)) {   /* a new file: its load base */
                memcpy(cur, path, pn);
                cur[pn] = 0;
                base = lo - fo;
            }
            if (a >= lo && a < hi) {
                const char *b = strrchr(cur, '/');
                snprintf(name, cap, "%s", !cur[0] ? "?" : b ? b + 1 : cur);
                *off = cur[0] == '/' ? a - base : a - lo;
                return name;
            }
        }
        l = e + 1;
    }
    snprintf(name, cap, "?");
    *off = a;
    return name;
}

static void host_crash_report(int sig, const char *sname, siginfo_t *si, ucontext_t *uc, CPU *c)
{
    char tname[17] = "";
    prctl(PR_GET_NAME, (unsigned long)tname, 0, 0, 0);
    long tid = syscall(SYS_gettid);
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP], off;
    char mod[128];
    crash_maps_read();
    crash_line("FATAL: signal %d (%s) at address %p, %s thread \"%s\" (tid %ld)", sig, sname, si->si_addr,
               c ? "game" : "host", tname, tid);
    crash_module(pc, &off, mod, sizeof mod);
    crash_line("  pc %#lx: %s+%#lx (sp %#lx)", (unsigned long)pc, mod, (unsigned long)off,
               (unsigned long)uc->uc_mcontext.gregs[REG_RSP]);
    extern void port_crash_report(int guest_thread) __attribute__((weak));
    if (port_crash_report)
        port_crash_report(c != NULL);       /* (the port's own lines, through crash_line) */
    if (c)
        return;                             /* (a game thread: fatal()'s report follows) */
    void *bt[48];
    int nb = backtrace(bt, 48), from = 0;
    for (int i = 0; i < nb; i++)            /* (from the faulting frame: past the handler's) */
        if ((uintptr_t)bt[i] == pc) { from = i; break; }
    for (int i = from; i < nb; i++) {
        crash_module((uintptr_t)bt[i], &off, mod, sizeof mod);
        crash_line("  #%d %p %s+%#lx", i - from, bt[i], mod, (unsigned long)off);
    }
    crash_line("  the same frames by name:");
    if (crash_log_fd >= 0)
        backtrace_symbols_fd(bt + from, nb - from, crash_log_fd);
    backtrace_symbols_fd(bt + from, nb - from, 2);
}

static void on_signal(int sig, siginfo_t *si, void *ucv)
{
    static volatile long reporting;
    long me = syscall(SYS_gettid);
    if (!__sync_bool_compare_and_swap(&reporting, 0, me)) {
        if (reporting == me)
            _exit(3);                       /* (a fault in the report itself) */
        for (;;)
            pause();                        /* (another thread's report ends the process) */
    }
    signal(SIGALRM, SIG_DFL);
    alarm(5);                               /* (a report that hangs: the process ends) */
    const char *sname = sig == SIGSEGV ? "Segmentation fault" : sig == SIGBUS ? "Bus error" :
                        sig == SIGFPE ? "Floating point exception" : sig == SIGILL ? "Illegal instruction" :
                        sig == SIGABRT ? "Aborted" : "?";
    CPU *c = current_cpu();
    host_crash_report(sig, sname, si, (ucontext_t *)ucv, c);
    if (c)
        fatal("signal %d (%s) at address %p", sig, sname, si->si_addr);    /* (the guest's state) */
    alarm(0);
    signal(sig, SIG_DFL);                   /* (the fault again, as if unhandled; abort's raised) */
    if (sig == SIGABRT)
        raise(sig);
}

static void crash_handlers_install(void)
{
    crash_log_fd = logfp ? fileno(logfp) : -1;
    struct sigaction sa = {0};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    void *warm[2];
    backtrace(warm, 2);                     /* (loads the unwinder now, not in a crash's handler) */
}
#else
void crash_line(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logmsg(LOG_ALWAYS, "%s", buf);
}

static __attribute__((unused)) void on_signal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    static volatile int in;
    if (in++)
        _exit(3);
    fatal("signal %d (%s) at address %p", sig, strsignal(sig), si->si_addr);
}

static void crash_handlers_install(void)
{
    struct sigaction sa = {0};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}
#endif

/* EXIT. The runtime's and the port's own exit work runs first (runtime_at_exit, last registered
   first; port_at_exit: e.g. stopping a render thread), then the process ends with _exit: no host
   atexit teardown, which ran while the program's other threads still used DXVK and SDL (LA Rush
   crashed in a driver library at exit, Xtreme Stunts hung). */
static void (*exit_fns[16])(void);
static int nexit_fns;
void runtime_at_exit(void (*fn)(void))
{
    if (nexit_fns < 16)
        exit_fns[nexit_fns++] = fn;
}
__attribute__((weak)) void port_at_exit(void) {}

static void exit_as(int code, int status) __attribute__((noreturn));
void process_exit(int code) { exit_as(code, code); }

/* The game quitting on its own (ExitProcess, its entry point returning, its last thread ending,
   TerminateProcess on itself): on Windows nothing reads that code, and old games often give a
   leftover value (Test Drive 6 1, Off-Road 38), which a launcher takes for an error (AntRecomp's
   "exited with code"). So a real run tells the launcher 0; --test runs keep the game's code (the
   checks compare it), as does a port with PORT_KEEP_EXIT_CODE. The log says the game's either way.
   A crash, the runtime's fatal() (2) and FatalExit keep theirs. */
void game_exit(int code)
{
#ifdef PORT_KEEP_EXIT_CODE
    exit_as(code, code);
#else
    exit_as(code, deterministic ? code : 0);
#endif
}

/* One thread does the exit work; one that exits meanwhile waits here for good (the first ends the
   process), and exit work that exits again ends at once. "program exited ..." is the log's last
   line, after the exit work's own lines (L.A. Rush's clean-end rule reads the last line; it was
   first, and two exiting threads interleaved) */
static void exit_as(int code, int status)
{
    static pthread_mutex_t exit_lock = PTHREAD_MUTEX_INITIALIZER;
    static pthread_t exiting;
    static int started;
    if (__atomic_load_n(&started, __ATOMIC_ACQUIRE) && pthread_equal(exiting, pthread_self()))
        _exit(status);
    pthread_mutex_lock(&exit_lock);
    exiting = pthread_self();
    __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    port_at_exit();
    extern void ifc_exit(void) __attribute__((weak));
    if (ifc_exit)
        ifc_exit();                     /* (no controller motor left running) */
    settings_save();
    extern void coverage_write(void) __attribute__((weak));
    if (coverage_write)
        coverage_write();               /* (coverage builds: runtime/rewrite/coverage.c) */
    while (nexit_fns > 0)
        exit_fns[--nexit_fns]();
    LOG(LOG_ALWAYS, "program exited with code %d%s", code,
        status != code ? " (a normal quit: 0 to whoever started it)" : "");
    if (logfp)
        fflush(logfp);
    fflush(NULL);
    _exit(status);
}

extern void sync_init(void);
extern void files_init(void);

/* Everything before the program's entry point runs: memory, the image, imports, devices. */
void runtime_setup(void)
{
    mem_init();
    for (int i = 0; i < module_count; i++)
        load_module(&module_table[i]);
    build_function_index();
    connect_imports();
    extern void sysdlls_map(void);
    sysdlls_map();
    extern void tls_setup(void);
    tls_setup();                        /* static TLS (tls.c): before the first thread */
    sync_init();
    files_init();
    settings_load();
    win_init();
    pad_init();
    port_graphics_init();               /* the port's display layer (DirectDraw, Direct3D 8/9) */
    dsound_init();
#if defined(PORT_TEST_SOUND_AT_FRAME)
    if (deterministic) {                /* --test: the mixer's clock steps with the frames */
        extern void sound_set_clock(double (*fn)(void));
        sound_set_clock(test_sound_clock);
    }
#endif
    dinput_init();
    extern void eh_init(void);
    eh_init();                          /* exceptions: eh.c */
    extern void game_init(void);
    game_init();
    /* the hand-written C (runtime/rewrite/, for ports with rewrites): option decomp = off or
       <PORT_ENV>_DECOMP_OFF leaves it out; the stack fixes are in either way */
    extern void rewrite_install(void) __attribute__((weak));
    if (rewrite_install)
        rewrite_install();
    port_runtime_ready();
}

/* DLL_PROCESS_ATTACH for each DLL, in the order the build lists them (the C runtime first) */
/* DllMain, as Windows runs it (from one port's loader): PROCESS_ATTACH once, for the
   DLLs the program starts with (build config load='start', in the build's order: the C runtimes
   first) before its entry point, and for the others when the program loads them (LoadLibrary);
   THREAD_ATTACH / THREAD_DETACH to every started DLL as threads come and go. */
static unsigned char module_attached[64];

int module_started(const ModuleEntry *m)
{
    int i = (int)(m - module_table);
    return !m->is_dll || m->at_start || (i >= 0 && i < 64 && module_attached[i]);
}

void module_attach(const ModuleEntry *m, CPU *c)
{
    int i = (int)(m - module_table);
    if (i <= 0 || i >= 64 || module_attached[i] || !m->is_dll)
        return;
    module_attached[i] = 1;
    tls_callbacks(c, m, 1);             /* its TLS callbacks first, as Windows */
    if (!m->entry)
        return;
    uint32_t r = guest_call(c, m->entry, 3, m->base, 1u, 0u);
    LOG(LOG_ALWAYS, "%s started (%u)", m->name, r);
    if (!r)
        fatal("%s failed to start (its DllMain returned 0)", m->name);
}

void dlls_attach(CPU *c)
{
    for (int i = 1; i < module_count; i++)
        if (module_table[i].at_start)
            module_attach(&module_table[i], c);
    tls_callbacks(c, &module_table[0], 1);   /* the program's TLS callbacks, before its entry point */
}

/* a thread's start (reason 2): each started DLL's TLS callbacks and DllMain, then the program's
   TLS callbacks (XP's LdrpInitializeThread) */
void dlls_thread_notify(CPU *c, int reason)
{
    for (int i = 1; i < module_count && i < 64; i++)
        if (module_attached[i]) {
            tls_callbacks(c, &module_table[i], (uint32_t)reason);
            if (module_table[i].entry)
                guest_call(c, module_table[i].entry, 3, module_table[i].base, (uint32_t)reason, 0u);
        }
    tls_callbacks(c, &module_table[0], (uint32_t)reason);
}

/* ------------------------------------------------------------------ main */
int run_frames_limit;
static unsigned long dump_frame, dump_call;
static char dump_path[1024];
double run_seconds_limit;
unsigned long frame_count;
/* The game's own frames: what scripted input, --hash, --dump-memory, --frames and game_frame
   count. One a presented frame, unless the port's port_frame_ticks says how many game updates the
   frame ran (ETM with frames drawn between updates: the same input meets the same update at any
   drawn rate) */
unsigned long game_frames;
__attribute__((weak)) int port_frame_ticks(void) { return 1; }
double test_frame_ms = 1000.0 / 60.0;   /* --test: the virtual clock's step this frame (<PORT_ENV>_TEST_FPS) */
static unsigned long wait_frames;       /* input_wait's frames */

extern void timers_run_due(double now);
static FILE *hashfp;
static unsigned long hash_every = 1;
unsigned long api_calls;
int api_trace_on;
static unsigned long trace_lo, trace_hi;
static FILE *tracefp;

/* --hash-calls FROM TO FILE: a memory hash after each runtime call in that range, with the
   call site, to find the first call after which two runs differ. */
void api_trace(CPU *c, int nargs)
{
    extern int fncheck_api(CPU *c, int nargs) __attribute__((weak));
    if (fncheck_api && fncheck_api(c, nargs))
        return;                         /* (a --fncheck child's record: runtime/rewrite/fncheck.c) */
    if (dump_call && api_calls == dump_call) {
        mem_dump(dump_path);
        LOG(LOG_ALWAYS, "memory after call %lu (to %08x) written to %s", api_calls, c->ret, dump_path);
    }
    if (!tracefp || api_calls < trace_lo || api_calls > trace_hi)
        return;
    fprintf(tracefp, "%lu %08x %016llx\n", api_calls, c->ret, (unsigned long long)mem_hash());
    if (api_calls == trace_hi)
        fflush(tracefp);
}

/* FRAME LIMIT (Enhanced; options.txt frame_limit = off | screen | <fps>; not in test runs): a frame
   waits for its time before the game goes on (its input read after the wait, so what the next frame
   shows is as fresh as it can be). "screen" is 3 under the screen's refresh: with variable refresh
   (G-Sync, FreeSync) frames then stay inside the range the screen follows, never waiting for vsync.
   The wait is a sleep to an absolute time with 1 us timer slack (pacing.c): no spinning. */
static double limit_ms;
static int limit_read;

void frame_limit_options(void) { limit_read = 0; }

int frame_limit_screen(void)
{
    int hz = host_refresh_hz();
    return hz > 33 ? hz - 3 : hz;
}

/* the limiter's frame time this frame (ms, 0 = no limit), given the option's: a strong
   port_frame_limit_ms caps some parts of a game (Enter the Matrix's driving and hovercraft levels
   at 30 whatever the option says) */
__attribute__((weak)) double port_frame_limit_ms(double ms) { return ms; }

static void frame_limit(void)
{
    if (deterministic)
        return;
    if (!limit_read) {
        const char *v = option_str("frame_limit", "off");
        int fps = !strcasecmp(v, "screen") ? frame_limit_screen() : atoi(v);
        limit_ms = fps >= 10 ? 1000.0 / fps : 0;
        limit_read = 1;
        if (fps >= 10)
            LOG(LOG_ALWAYS, "frame limit: %d fps", fps);
    }
    /* <PORT>_FRAME_TIMES: each 300 frames, the average and slowest frame time without the
       limiter's wait (what the frame rate would be uncapped) */
    static int ft_on = -1;
    static double ft_after, ft_sum, ft_max;
    static int ft_n;
    if (ft_on < 0) ft_on = port_getenv("FRAME_TIMES") != NULL;
    if (ft_on) {
        double t = real_ms();
        if (ft_after > 0) {
            double d = t - ft_after;
            ft_sum += d;
            if (d > ft_max) ft_max = d;
            if (++ft_n == 300) {
                LOG(LOG_ALWAYS, "frames %lu: %.2f ms a frame (%.0f fps uncapped), slowest %.1f ms",
                    frame_count, ft_sum / ft_n, 1000.0 * ft_n / ft_sum, ft_max);
                ft_n = 0; ft_sum = ft_max = 0;
            }
        }
    }
    /* the wait: a sleep to the frame's start on a grid of exact periods (pacing.c), no spin */
    static Pacer pacer;
    double ms = port_frame_limit_ms(limit_ms);   /* (a port's own cap this frame: ETM's driving levels at 30) */
    pacer_wait(&pacer, ms > 0 ? 1000.0 / ms : 0);
    if (ft_on) ft_after = real_ms();
}

/* <PORT>_POKE="addr=value[@frame],..." (hex, the frame decimal): dwords written into the
   program's memory every frame, from that frame on (experiments: flags found with REA) */
static void pokes(void)
{
    static int n = -1;
    static uint32_t at[16], val[16];
    static unsigned long from[16];
    if (n < 0) {
        n = 0;
        const char *e = port_getenv("POKE");
        for (const char *p = e; p && *p && n < 16; ) {
            char *q;
            at[n] = strtoul(p, &q, 16);
            if (*q != '=') break;
            val[n] = strtoul(q + 1, &q, 16);
            from[n] = *q == '@' ? strtoul(q + 1, &q, 10) : 0;
            LOG(LOG_ALWAYS, "poke %08x = %08x every frame from frame %lu", at[n], val[n], from[n]);
            n++;
            p = *q == ',' ? q + 1 : q;
        }
    }
    for (int i = 0; i < n; i++)
        if (frame_count >= from[i])
            W32(at[i], val[i]);
}

void game_frame(unsigned long frame);   /* game.c: before the frame's input and window pump */
/* the port's work after the frame's input and window pump (e.g. pumping a network layer's messages) */
__attribute__((weak)) void port_frame_end(unsigned long frame) { (void)frame; }

/* --shot FRAME FILE: the frame presented as number FRAME saved as a PNG by the display layer
   (d3d_write_png), headless too; "+N" counts from the scripted input's mark */
static struct { unsigned long frame; int relative; char path[512]; } shots[64];
static int nshots;
/* how many --shot frames were asked for (a renderer making its back buffer readable before the
   device exists: Penguins!' d3d8to9 on Windows) */
int shots_requested(void) { return nshots; }
static unsigned long shot_frame(int i)
{
    long mark = input_script_mark_frame();
    return !shots[i].relative ? shots[i].frame : mark >= 0 ? (unsigned long)mark + shots[i].frame : 0;
}
/* the display layer asks: is the frame being presented now to be saved? */
int shot_due(void)
{
    for (int i = 0; i < nshots; i++)
        if (shot_frame(i) == frame_count + 1)
            return 1;
    return 0;
}

void frame_presented(void)
{
    frame_count++;
    game_frames += (unsigned long)port_frame_ticks();
    for (int i = 0; i < nshots; i++)
        if (shot_frame(i) == frame_count) {
            extern int d3d_write_png(const char *path);
            LOG(LOG_ALWAYS, "frame %lu %s %s", frame_count, d3d_write_png(shots[i].path) ? "saved to" : "not saved:",
                shots[i].path);
        }
    extern void det_yield(void);
    det_yield();                        /* --test: the other threads' turn (sync.c) */
    pokes();
    {   /* <PORT>_CPU_FROM=<frame>: the game thread's CPU time a frame from then, logged every 400 frames
           (timing changes on the game thread without its waits; work/cpu_far.sh) */
        static long from = -2;
        static double t0;
        if (from == -2)
            from = port_getenv("CPU_FROM") ? atol(port_getenv("CPU_FROM")) : -1;
        if (from >= 0 && frame_count >= (unsigned long)from) {
            struct timespec ts;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            double t = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
            if (frame_count == (unsigned long)from)
                t0 = t;
            else if ((frame_count - (unsigned long)from) % 400 == 0)
                LOG(LOG_ALWAYS, "game thread CPU %.3f ms a frame over %lu frames", (t - t0) / (frame_count - (unsigned long)from),
                    frame_count - (unsigned long)from);
        }
    }
    {   /* each frame's duration on the game's clock (the virtual one in --test), for a port's
           frame-rate code (port_frame_time: per-frame amounts kept to the game's pace) */
        static double prev = -1;
        double now = frame_clock_ms();
        if (prev >= 0)
            port_frame_time(now - prev);
        prev = now;
    }
    {   /* <PORT>_LOG_FRAME=<n>: the d3d and draw logs for that one frame */
        static long log_frame = -2;
        static unsigned saved_mask;
        if (log_frame == -2) {
            const char *e = port_getenv("LOG_FRAME");
            log_frame = e && *e ? atol(e) : -1;
        }
        if (log_frame >= 0 && frame_count == (unsigned long)log_frame) {
            saved_mask = log_mask;
            log_mask |= LOG_D3D | LOG_DRAW;
        } else if (log_frame >= 0 && frame_count == (unsigned long)log_frame + 1)
            log_mask = saved_mask;
    }
    if (deterministic) {
        /* <PORT>_TEST_FPS=<fps>[@<frame>]: the virtual clock's frame time from that frame on (60 fps
           otherwise: a race as a faster or slower machine plays it, still the same every run) */
        static double vstep = -1;
        static unsigned long vstep_from;
        if (vstep < 0) {
            const char *e = port_getenv("TEST_FPS");
            const char *at = e ? strchr(e, '@') : NULL;
            vstep = e && atof(e) > 0 ? 1000.0 / atof(e) : 1000.0 / 60.0;
            vstep_from = at ? strtoul(at + 1, NULL, 10) : 0;
        }
        test_frame_ms = frame_count >= vstep_from ? vstep : 1000.0 / 60.0;
        vclock += test_frame_ms;
        vclock_frame = vclock;
        timers_run_due(vclock);
    }
    {
        /* <PORT>_PACE=<fps>: frames no faster than that in real time, as with vsync (timing a race
           as it is played: the CPU and GPU share the APU's power, which a flat-out run does not; and
           real-time runs off-screen, which have no vsync, at a monitor's rate) */
        static double pace_ms = -1, pace_next;
        if (pace_ms < 0) {
            const char *e = port_getenv("PACE");
            pace_ms = e && atof(e) > 0 ? 1000.0 / atof(e) : 0;
        }
        if (pace_ms > 0) {
            double now = real_ms();
            if (pace_next > now) {
                struct timespec ts = {0, (long)((pace_next - now) * 1e6)};
                nanosleep(&ts, NULL);
            }
            pace_next = (pace_next > now - pace_ms ? pace_next : now) + pace_ms;
        }
    }
    frame_limit();
    static unsigned long hashed = ~0ul;
    if (hashfp && game_frames % hash_every == 0 && game_frames != hashed) {
        hashed = game_frames;
        fprintf(hashfp, "%lu %lu %016llx %016llx\n", game_frames, api_calls, (unsigned long long)mem_hash(),
                (unsigned long long)draw_hash);
        fflush(hashfp);
    }
    static unsigned long dumped = ~0ul;
    if (dump_frame && game_frames >= dump_frame && game_frames != dumped) {
        /* <PORT>_DUMP_COUNT=N: N frames running from FRAME, each to FILE.<frame> */
        static long count = -1;
        if (count < 0) count = port_getenv("DUMP_COUNT") ? atol(port_getenv("DUMP_COUNT")) : 0;
        if (game_frames == dump_frame || game_frames < dump_frame + (unsigned long)count) {
            char p[1200];
            dumped = game_frames;
            if (count > 0) snprintf(p, sizeof p, "%s.%lu", dump_path, game_frames);
            else snprintf(p, sizeof p, "%s", dump_path);
            mem_dump(p);
            LOG(LOG_ALWAYS, "memory of frame %lu written to %s", game_frames, p);
        }
    }
    /* --fncheck SPEC: the per-function checks the spec names for this frame (runtime/rewrite/fncheck.c;
       a port with rewrites); nothing without the flag */
    extern void fncheck_frame(unsigned long frame) __attribute__((weak));
    if (fncheck_frame)
        fncheck_frame(game_frames);
    {
        /* <PORT>_PEEK=addr[>off...][:n],... (addresses and offsets hex, n decimal; tests): those n
           dwords (default 1) logged every frame. addr>off reads the dword at addr and adds off, up to
           three times (L.A. Rush's pointer chains: 8a0850>14>8:2); a pointer outside the program's
           memory (0x10000 up to 0x80000000) logs the entry as addr>- */
        static int np = -1;
        static uint32_t pa[16], pn[16], poff[16][3];
        static int pdepth[16];
        if (np < 0) {
            np = 0;
            for (const char *e = port_getenv("PEEK"); e && *e && np < 16; ) {
                char *q;
                pa[np] = (uint32_t)strtoul(e, &q, 16);
                pdepth[np] = 0;
                while (*q == '>' && pdepth[np] < 3)
                    poff[np][pdepth[np]++] = (uint32_t)strtoul(q + 1, &q, 16);
                pn[np] = *q == ':' ? (uint32_t)strtoul(q + 1, &q, 10) : 1;
                np++;
                e = *q == ',' ? q + 1 : q;
            }
        }
        if (np) {
            char line[2048];
            int o = snprintf(line, sizeof line, "peek %lu", frame_count);
            for (int k = 0; k < np; k++) {
                uint32_t at = pa[k];
                int bad = 0;
                for (int d = 0; d < pdepth[k] && !bad; d++) {
                    if (at < 0x10000u || at > 0x80000000u - 4) { bad = 1; break; }
                    uint32_t p = R32(at);
                    if (p < 0x10000u || p >= 0x80000000u) bad = 1;
                    at = p + poff[k][d];
                }
                if (bad || (pdepth[k] && (at < 0x10000u || at > 0x80000000u - 4 * pn[k]))) {
                    o += snprintf(line + o, sizeof line - o, " %08x>-", pa[k]);
                    continue;
                }
                for (uint32_t j = 0; j < pn[k] && o < (int)sizeof line - 40; j++) {
                    uint32_t w = R32(at + 4 * j);
                    float f;
                    memcpy(&f, &w, 4);
                    o += snprintf(line + o, sizeof line - o, " %08x=%08x(%g)", at + 4 * j, w, f);
                }
            }
            LOG(LOG_ALWAYS, "%s", line);
        }
    }
    if (run_frames_limit && game_frames >= (unsigned long)run_frames_limit) {
        LOG(LOG_ALWAYS, "frame limit %d reached", run_frames_limit);
        process_exit(0);
    }
    if (run_seconds_limit > 0 && (deterministic ? vclock : real_ms()) > run_seconds_limit * 1000.0) {
        LOG(LOG_ALWAYS, "time limit reached after %lu frames", frame_count);
        process_exit(0);
    }
    game_frame(game_frames + wait_frames);
    input_frame(game_frames + wait_frames);
    win_pump();
    port_frame_end(game_frames + wait_frames);
}

void frame_presented_idle(void)
{
    if (run_seconds_limit > 0 && (deterministic ? vclock : real_ms()) > run_seconds_limit * 1000.0) {
        LOG(LOG_ALWAYS, "time limit reached after %lu frames (waiting for messages)", frame_count);
        process_exit(0);
    }
}

/* The program reads its devices without presenting frames while it waits for a key (the
   controls menus' "press the control for ..." busy-waits on DirectInput): host events and
   scripted input keep coming as if frames passed - every 1/60 s, or with --test every 60 reads
   (no clock moves there). Scripted frame numbers count these too. */
void input_wait(void)
{
    static unsigned long reads, frame_seen;
    static double last;
    if (current_cpu() != main_cpu)
        return;
    if (frame_seen != frame_count) {
        frame_seen = frame_count;
        reads = 0;
        last = deterministic ? 0 : real_ms();
        return;
    }
    if (deterministic ? ++reads < 60 : real_ms() - last < 1000.0 / 60.0)
        return;
    reads = 0;
    last = deterministic ? 0 : real_ms();
    wait_frames++;
    LOG(LOG_INPUT, "input: the program waits on its devices (frame %lu + %lu)", frame_count, wait_frames);
    game_frame(game_frames + wait_frames);
    input_frame(game_frames + wait_frames);
    win_pump();
    port_frame_end(game_frames + wait_frames);
}

#ifndef RUNTIME_NO_MAIN
void port_usage(void);
int port_arg(int argc, char **argv, int i);
static void usage(void)
{
    fprintf(stderr,
            "usage: " PORT_SLUG " [--game DIR] [--user DIR] [--log LIST] [--window|--fullscreen] [--frames N] [--seconds S]\n"
            "              [--test] [--hash FILE] [--hash-every N] [--dump-memory FRAME FILE] [--fncheck SPEC]\n"
            "              [--option KEY=VALUE]   (see options.txt in the user folder)\n"
            "              [--trace-calls FILE]   (every call into the runtime by name, a line each)\n"
            "  --test  deterministic run: virtual clock, timers at frame ends, no real-time sound\n"
            "  --log  comma list: api,file,d3d,ddraw,sound,input,win,mem,thread,reg,draw,all\n");
    port_usage();
    exit(1);
}

/* a port's own command-line arguments (Enter the Matrix's --level NAME, --list-levels): a strong
   port_arg looks at argv[i] and returns how many arguments it took (0: not one of its own);
   port_usage prints its lines under the runtime's */
__attribute__((weak)) int port_arg(int argc, char **argv, int i) { (void)argc; (void)argv; (void)i; return 0; }
__attribute__((weak)) void port_usage(void) {}

int main(int argc, char **argv)
{
    const char *cli_opts[16];
    int ncli = 0;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
#ifdef _WIN32
    const char *home = getenv("APPDATA");           /* %APPDATA%\<slug>, '/' separated */
    snprintf(user_dir, sizeof user_dir, "%s/" PORT_SLUG, home ? home : ".");
    for (char *p = user_dir; *p; p++)
        if (*p == '\\') *p = '/';
#else
    const char *home = getenv("HOME");
    snprintf(user_dir, sizeof user_dir, "%s/.local/share/" PORT_SLUG, home ? home : ".");
#endif
    snprintf(game_dir, sizeof game_dir, "%s/game", user_dir);
    const char *logpath = NULL;
    /* Started without arguments (the applications-menu entry, a double click) it plays: in a window
       (options.txt's display decides full screen), the log in the user folder. With arguments it is
       a test run, headless unless --window. */
    static char default_log[1100];
    if (argc == 1) {
        headless = 0;
        snprintf(default_log, sizeof default_log, "%s/last-run.log", user_dir);
        logpath = default_log;
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--game") && v) { snprintf(game_dir, sizeof game_dir, "%s", v); i++; }
        else if (!strcmp(a, "--user") && v) { snprintf(user_dir, sizeof user_dir, "%s", v); i++; }
        else if (!strcmp(a, "--cd") && v) { snprintf(cd_dir, sizeof cd_dir, "%s", v); i++; }
        else if (!strcmp(a, "--shot") && i + 2 < argc && nshots < 64) {
            const char *f = argv[i + 1];
            shots[nshots].relative = f[0] == '+';
            shots[nshots].frame = strtoul(f + shots[nshots].relative, NULL, 10);
            snprintf(shots[nshots].path, sizeof shots[nshots].path, "%s", argv[i + 2]);
            nshots++;
            i += 2;
        }
        else if (!strcmp(a, "--logfile") && v) { logpath = v; i++; }
        else if (!strcmp(a, "--window")) headless = 0;
        else if (!strcmp(a, "--hidden")) { headless = 0; hidden_window = 1; }    /* (a window never shown) */
        else if (!strcmp(a, "--fullscreen")) { extern int start_fullscreen; headless = 0; start_fullscreen = 1; }
        else if (!strcmp(a, "--test")) deterministic = 1;
        else if (!strcmp(a, "--option") && v) { cli_opts[ncli++ & 15] = v; i++; }
        else if (!strcmp(a, "--hash") && v) {
            hashfp = fopen(v, "w");
            draw_hashing = 1;
            if (!hashfp) fatal("cannot write %s", v);
            i++;
        }
        else if (!strcmp(a, "--trace-calls") && v) {
            calls_fp = fopen(v, "w");
            if (!calls_fp) fatal("cannot write %s", v);
            setvbuf(calls_fp, NULL, _IOFBF, 1 << 20);
            api_via_thunk = 1;          /* (imports too: by their thunks, where their names are) */
            i++;
        }
        else if (!strcmp(a, "--hash-calls") && i + 3 < argc) {
            trace_lo = strtoul(argv[i + 1], NULL, 10);
            trace_hi = strtoul(argv[i + 2], NULL, 10);
            tracefp = fopen(argv[i + 3], "w");
            if (!tracefp) fatal("cannot write %s", argv[i + 3]);
            api_trace_on = 1;
            i += 3;
        }
        else if (!strcmp(a, "--dump-memory-call") && i + 2 < argc) {
            dump_call = strtoul(argv[i + 1], NULL, 10);
            snprintf(dump_path, sizeof dump_path, "%s", argv[i + 2]);
            api_trace_on = 1;
            i += 2;
        }
        else if (!strcmp(a, "--hash-every") && v) { hash_every = strtoul(v, NULL, 10); if (!hash_every) hash_every = 1; i++; }
        else if (!strcmp(a, "--dump-memory") && i + 2 < argc) {
            dump_frame = strtoul(argv[i + 1], NULL, 10);
            snprintf(dump_path, sizeof dump_path, "%s", argv[i + 2]);
            i += 2;
        }
        else if (!strcmp(a, "--frames") && v) { run_frames_limit = atoi(v); i++; }
        else if (!strcmp(a, "--fncheck") && v) {
            /* the per-function check (docs/REWRITE.md "Checking a function no route reaches") */
            extern int fncheck_load(const char *spec) __attribute__((weak));
            if (!fncheck_load)
                fatal("--fncheck: this build has no runtime/rewrite (a port with rewrites)");
            fncheck_load(v);
            i++;
        }
        else if (!strcmp(a, "--seconds") && v) { run_seconds_limit = atof(v); i++; }
        else if (!strcmp(a, "--log") && v) {
            static const struct { const char *n; unsigned m; } cats[] = {
                {"api", LOG_API}, {"file", LOG_FILE}, {"d3d", LOG_D3D}, {"ddraw", LOG_DDRAW},
                {"sound", LOG_SOUND}, {"input", LOG_INPUT}, {"win", LOG_WIN}, {"mem", LOG_MEM},
                {"thread", LOG_THREAD}, {"reg", LOG_REG}, {"draw", LOG_DRAW}, {"all", 0xffffu}};
            char tmp[256];
            snprintf(tmp, sizeof tmp, "%s", v);
            for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ","))
                for (size_t k = 0; k < sizeof cats / sizeof *cats; k++)
                    if (!strcmp(t, cats[k].n))
                        log_mask |= cats[k].m;
            i++;
        } else {
            int took = port_arg(argc, argv, i);
            if (took <= 0)
                usage();
            i += took - 1;
        }
    }
    mkdir(user_dir, 0755);
    if (logpath) {
        logfp = fopen(logpath, "w");
        if (!logfp)
            fatal("cannot write %s", logpath);
    }
    crash_handlers_install();
#ifndef _WIN32
    /* Windows has no open-file limit a game would meet, and each open guest file is one host
       descriptor here: the usual soft limit of 1,024 stops a game that keeps a few thousand open
       (one keeps 2,025 after loading). Raise it to the hard limit. */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rlim_t was = rl.rlim_cur;
        rl.rlim_cur = rl.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &rl) == 0)
            LOG(LOG_FILE, "open-file limit raised from %llu to %llu", (unsigned long long)was, (unsigned long long)rl.rlim_max);
    }
#endif

    options_load();
    for (int k = 0; k < ncli && k < 16; k++)
        option_parse(cli_opts[k]);
    if (deterministic) {
        extern int fpu_sw_exceptions;
        fpu_sw_exceptions = 0;          /* the reference emulator does not model them */
    }
    extern void prof_init(void);
    prof_init();
    if (!cd_dir[0])
        snprintf(cd_dir, sizeof cd_dir, "%s/CD", game_dir);   /* the CD's files (port.h PORT_CD_LABEL) */
    runtime_setup();
    logmsg(LOG_ALWAYS, PORT_TITLE " (AntiStatic, %s): %d functions, game folder %s", BUILD_NAME, func_count, game_dir);

    CPU *c = cpu_new(STACK_RESERVE);
    main_cpu = c;
    set_current_cpu(c);
    fpu_activate(c);
    dlls_attach(c);
    c->esp -= 4;
    W32(c->esp, SENTINEL);
    /* --fncheck: a spec's `frame 0` checks run here, before the program's entry point (its DLLs
       started): functions that need nothing the program sets up */
    extern void fncheck_frame(unsigned long frame) __attribute__((weak));
    if (fncheck_frame)
        fncheck_frame(0);
    icall(c, IMAGE_ENTRY);
    game_exit((int)c->eax);
}
#endif
