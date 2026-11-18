/* AntiStatic self-test harness: runs translated test functions (tests/selftest/cases.s, through
   translator/recomp.py) on the states run.py sends, and sends back what they left.

     harness IMAGE.bin < states > results

   The program image is mapped at 0x400000 as Windows would map it, the scratch memory at
   0x10000000 and a stack at 0x20000000, the same places run.py gives the Unicorn emulator.
   Each state: the function's address, eax..edi, the scratch page. Each result: eax..edi, the
   scratch page, the 256 bytes below the starting stack pointer, and a status (0 = returned). */
#define _GNU_SOURCE
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "runtime.h"

#define IMAGE_AT 0x400000u
#define SCRATCH 0x10000000u
#define SCRATCH_SIZE 0x1000u
#define STACK 0x20000000u
#define STACK_SIZE 0x10000u
#define STACK_TOP (STACK + 0xf000u)
#define SENTINEL 0x7efe0000u

typedef struct { uint32_t fn, regs[8]; uint8_t scratch[SCRATCH_SIZE]; } In;
typedef struct { uint32_t regs[8]; uint8_t scratch[SCRATCH_SIZE]; uint8_t stack[256]; uint32_t status; } Out;

static jmp_buf stop;
static uint32_t stop_status;

/* ------------------------------------------------------------------ what the generated code calls */
static void halt(uint32_t status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    stop_status = status;
    longjmp(stop, 1);
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    stop_status = 9;
    longjmp(stop, 1);
}

int capture_armed, dev_recording, api_via_thunk;
void capture_point(CPU *c, uint32_t fn) { (void)c; (void)fn; }
void capture_return(CPU *c, uint32_t fn) { (void)c; (void)fn; }

static void (*find(uint32_t a))(CPU *)
{
    int lo = 0, hi = func_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (func_table[mid].addr == a) return func_table[mid].fn;
        if (func_table[mid].addr < a) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

void icall(CPU *c, uint32_t t)
{
    void (*f)(CPU *) = find(t);
    if (!f) halt(2, "call through a pointer to %08x: no function there", t);
    f(c);
}
void bad_ret(CPU *c, uint32_t site, uint32_t expected)
{
    if (c->ret == SENTINEL) return;
    halt(3, "return at %08x went to %08x, not %08x", site, c->ret, expected);
}
void bad_jump(CPU *c, uint32_t site, uint32_t t) { (void)c; halt(3, "jump at %08x to %08x", site, t); }
void nonlocal_jump(CPU *c, uint32_t site, uint32_t t) { (void)c; halt(3, "nonlocal jump at %08x to %08x", site, t); }
void unsupported(CPU *c, uint32_t site, const char *what) { (void)c; halt(4, "untranslated at %08x: %s", site, what); }
void trap_int3(CPU *c, uint32_t site) { (void)c; halt(5, "int3 at %08x", site); }
void div_fault(CPU *c, uint32_t site) { (void)c; halt(6, "divide fault at %08x", site); }
void fell_off(CPU *c, uint32_t site) { (void)c; halt(3, "fell off the end at %08x", site); }
void returned_from_noreturn(CPU *c, uint32_t site) { (void)c; halt(3, "noreturn returned at %08x", site); }
void missing_import(CPU *c, const char *dll, const char *name) { (void)c; halt(4, "import %s!%s", dll, name); }
uint64_t cpu_rdtsc(CPU *c) { (void)c; return 0x123456789ull; }

void rep_movs(CPU *c, int n, uint32_t src, uint32_t dst, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        memmove((void *)GP(dst), (const void *)GP(src), n);
        if (c->df) { src -= n; dst -= n; } else { src += n; dst += n; }
    }
}

void rep_stos(CPU *c, int n, uint32_t dst, uint32_t value, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        memcpy((void *)GP(dst), &value, n);
        if (c->df) dst -= n; else dst += n;
    }
}

/* the frame-rate options (build config FRAME_STEPS / TICKS): not used by the cases */
f80 fstep_add(f80 v) { return v; }
f80 fstep_lerp(f80 v) { return v; }
f80 fstep_pow(f80 v) { return v; }
int64_t fpu_ftol_dither(f80 v, uint32_t *n) { (void)n; return fpu_ftol(v); }
uint32_t fcount(uint32_t frames) { return frames; }
int framerate_tick_call(CPU *c, void (*orig)(CPU *), uint32_t fn) { (void)fn; orig(c); return 1; }
void eh_push(CPU *c, EHFrame *f, uint32_t esp, const uint32_t *at) { (void)c; (void)f; (void)esp; (void)at; }
void eh_pop(EHFrame *f) { (void)f; }
void eh_lost(CPU *c, EHFrame *f) { (void)c; (void)f; halt(7, "exception frame lost"); }

/* ------------------------------------------------------------------ the runs */
static void map(uint32_t at, uint32_t size)
{
    void *p = mmap((void *)(uintptr_t)at, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)at) { perror("mmap"); exit(1); }
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: harness IMAGE.bin\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    map(IMAGE_AT, (uint32_t)((n + 0xfff) & ~0xfff));
    if (fread((void *)(uintptr_t)IMAGE_AT, 1, n, f) != (size_t)n) { perror("read"); return 1; }
    fclose(f);
    map(SCRATCH, SCRATCH_SIZE);
    map(STACK, STACK_SIZE);

    fpu_sw_exceptions = 0;              /* the emulator does not model the x87 exception bits (as lockstep) */
    static In in;
    static Out out;
    static CPU cpu;
    while (fread(&in, sizeof in, 1, stdin) == 1) {
        CPU *c = &cpu;
        memset(c, 0, sizeof *c);
        fpu_new(c);
        fpu_activate(c);
        memcpy(&c->eax, in.regs, sizeof in.regs);   /* eax ecx edx ebx esp ebp esi edi */
        memcpy((void *)(uintptr_t)SCRATCH, in.scratch, SCRATCH_SIZE);
        memset((void *)(uintptr_t)STACK, 0, STACK_SIZE);
        c->esp = STACK_TOP - 4;
        W32(c->esp, SENTINEL);
        c->ret = SENTINEL;
        stop_status = 0;
        void (*fn)(CPU *) = find(in.fn);
        if (!fn)
            stop_status = 8;
        else if (!setjmp(stop))
            fn(c);
        memcpy(out.regs, &c->eax, sizeof out.regs);
        memcpy(out.scratch, (void *)(uintptr_t)SCRATCH, SCRATCH_SIZE);
        memcpy(out.stack, (void *)(uintptr_t)(STACK_TOP - 256), 256);
        out.status = stop_status;
        fwrite(&out, sizeof out, 1, stdout);
        fflush(stdout);
    }
    return 0;
}
