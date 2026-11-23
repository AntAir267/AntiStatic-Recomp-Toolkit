/* x87 operations the generated code calls out of line. They run the real x87 instruction on
   the host so results match the original bit for bit (the host's control word mirrors the
   program's, with exceptions masked). */
#include <math.h>
#include "cpu.h"
#if __has_include("port.h")
#include "port.h"                       /* (PORT_CPUID_* and PORT_X87_PORTABLE settings) */
#endif

static inline void host_fldcw(uint16_t cw) { __asm__ volatile("fldcw %0" : : "m"(cw)); }

/* the exceptions the host x87 keeps masked: all; a debugging option may unmask invalid
   operations so the first NaN stops under a debugger */
__thread uint16_t fpu_host_mask = 0x3f;
#define MASKED(cw) ((uint16_t)(((cw) & ~0x3f) | fpu_host_mask))
static inline uint16_t host_fnstsw(void) { uint16_t sw; __asm__ volatile("fnstsw %0" : "=am"(sw)); return sw; }

/* FLDCW: as a real x87 keeps it, bit 6 reads back as 1 and bits 13-15 as 0 (fnstcw of 0x033f
   is 0x037f) [verified on the host x87, 2026-11-10; SwARc's x87.cpp found it] */
void fpu_setcw(CPU *c, uint32_t cw)
{
    c->fcw = (uint16_t)((cw & 0x1f3fu) | 0x40u);
    host_fldcw(MASKED(cw));
}

/* A new CPU record's x87 state; the host FPU is not touched (the record may be for another
   thread): fpu_activate loads it once the record runs on a thread. */
void fpu_new(CPU *c)
{
    c->ftop = 0;
    c->fsw = 0;
    for (int i = 0; i < 8; i++)
        c->st[i] = 0;
    c->fcw = 0x37f;
    c->mxcsr = 0x1f80;
}

/* This host thread now runs c: its FPU takes c's control word. */
void fpu_activate(CPU *c)
{
    host_fldcw(MASKED(c->fcw));
    _mm_setcsr(c->mxcsr | 0x1f80u);
}

/* LDMXCSR: the program's rounding and flush-to-zero choices reach the host's SSE (which the
   generated code computes with); its exceptions stay masked on the host. */
void cpu_ldmxcsr(CPU *c, uint32_t v)
{
    c->mxcsr = v & 0xffffu;
    _mm_setcsr(c->mxcsr | 0x1f80u);
}

/* CPUID: a Pentium III (family 6, model 8): MMX and SSE, no SSE2 and no 3DNow!, so the program,
   D3DX and the C runtime take their x87/SSE code paths, all of which the translator covers.
   PORT_CPUID_SIGNATURE (leaf 1's eax) and PORT_CPUID_MAX_LEAF give another model (TD6: model 7);
   a port may answer as another CPU altogether with a strong cpu_cpuid (a game that picks code
   paths and DLLs by the CPU, Test Drive Off-Road 3) */
#ifndef PORT_CPUID_SIGNATURE
#define PORT_CPUID_SIGNATURE 0x686
#endif
#ifndef PORT_CPUID_MAX_LEAF
#define PORT_CPUID_MAX_LEAF 2
#endif
#ifndef PORT_CPUID_FEATURES                /* leaf 1's edx (TD6: 0x0383fbff, with the APIC bit) */
#define PORT_CPUID_FEATURES 0x0383f9ffu
#endif
__attribute__((weak)) void cpu_cpuid(CPU *c)
{
    uint32_t leaf = c->eax;
    c->ecx = 0;
    switch (leaf) {
    case 0:
        c->eax = PORT_CPUID_MAX_LEAF;
        c->ebx = 0x756e6547u;           /* "Genu" */
        c->edx = 0x49656e69u;           /* "ineI" */
        c->ecx = 0x6c65746eu;           /* "ntel" */
        break;
    case 1:
        c->eax = PORT_CPUID_SIGNATURE;
        c->ebx = 0;
        c->edx = PORT_CPUID_FEATURES;   /* FPU ... CMOV, PAT, PSE36, MMX, FXSR, SSE */
        break;
    case 2:
        c->eax = 0x03020101u;
        c->ebx = 0;
        c->edx = 0x0c040882u;
        break;
    default:
        c->eax = leaf >= 0x80000000u ? 0x80000000u : 0;   /* no extended leaves */
        c->ebx = c->edx = 0;
        break;
    }
}

/* FNINIT on the running CPU */
void fpu_init(CPU *c)
{
    __asm__ volatile("fninit");
    c->ftop = 0;
    c->fsw = 0;
    for (int i = 0; i < 8; i++)
        c->st[i] = 0;
    fpu_setcw(c, 0x37f);
}

/* The exception bits come from the host x87, which ran the same operations. Tests against
   an emulator that does not model them can turn them off. (fpu_sw itself is inline, cpu.h.) */
int fpu_sw_exceptions = 1;
uint32_t fpu_swcc_poison;

void fpu_clex(CPU *c)
{
    (void)c;
    __asm__ volatile("fnclex");
}

static void setcc(CPU *c, uint16_t sw, uint16_t mask)
{
    c->fsw = (uint16_t)((c->fsw & ~mask) | (sw & mask));
}

void fpu_xam(CPU *c)
{
    f80 v = ST(0);
    uint16_t sw;
    __asm__("fxam; fnstsw %0" : "=am"(sw) : "t"(v));
    setcc(c, sw, 0x4700);
}

void fpu_ldconst(CPU *c, int which)
{
    f80 v;
    switch (which) {
    case 0: __asm__("fld1" : "=t"(v)); break;
    case 1: __asm__("fldl2t" : "=t"(v)); break;
    case 2: __asm__("fldl2e" : "=t"(v)); break;
    case 3: __asm__("fldpi" : "=t"(v)); break;
    case 4: __asm__("fldlg2" : "=t"(v)); break;
    default: __asm__("fldln2" : "=t"(v)); break;
    }
    FPUSH(v);
}

/* PORT_X87_PORTABLE (1 unless the port says 0): the transcendentals below by basic arithmetic
   (x87soft.h), the same on every x86 CPU. 0: the host's own instructions (bit for bit the original's
   on that CPU, but Intel's and AMD's differ in the last bits), e.g. for a timing comparison. */
#ifndef PORT_X87_PORTABLE
#define PORT_X87_PORTABLE 1
#endif
#if PORT_X87_PORTABLE
#include "x87soft.h"
#define SOFT_BEGIN(c) host_fldcw(0x037f)            /* (extended precision, nearest, all masked) */
#define SOFT_END(c) host_fldcw(MASKED((c)->fcw))
static int soft_ok(f80 x) { return isfinite(x) && x != 0; }
#endif

void fpu_f2xm1(CPU *c)
{
    f80 v = ST(0);
#if PORT_X87_PORTABLE
    if (soft_ok(v) && fabsl(v) <= 1.0L) {
        SOFT_BEGIN(c);
        ST(0) = x87s_f2xm1(v);
        SOFT_END(c);
        return;
    }
#endif
    __asm__("f2xm1" : "+t"(v));
    ST(0) = v;
}

void fpu_fyl2x(CPU *c)
{
    f80 x = ST(0), y = ST(1), r;
#if PORT_X87_PORTABLE
    if (soft_ok(x) && x > 0 && x != 1.0L && soft_ok(y)) {
        SOFT_BEGIN(c);
        r = y * x87s_log2(x);
        SOFT_END(c);
        FPOP();
        ST(0) = r;
        return;
    }
#endif
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    FPOP();
    ST(0) = r;
}

void fpu_fyl2xp1(CPU *c)
{
    f80 x = ST(0), y = ST(1), r;
#if PORT_X87_PORTABLE
    if (soft_ok(x) && x > -1.0L && soft_ok(y)) {
        SOFT_BEGIN(c);
        r = y * x87s_log2p1(x);
        SOFT_END(c);
        FPOP();
        ST(0) = r;
        return;
    }
#endif
    __asm__("fyl2xp1" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    FPOP();
    ST(0) = r;
}

/* fbld / fbstp: the 10-byte packed BCD integer (Borland's and Delphi's float-to-text), by the host
   instruction (the game's rounding mode is the host's: fpu_setcw) */
void fpu_fbld(CPU *c, uint32_t a)
{
    uint8_t b[10];
    for (int k = 0; k < 10; k++)
        b[k] = (uint8_t)R8(a + (uint32_t)k);
    f80 r;
    __asm__("fbld %1" : "=t"(r) : "m"(b));
    FPUSH(r);
}

void fpu_fbstp(CPU *c, uint32_t a)
{
    uint8_t b[10];
    f80 x = ST(0);
    __asm__("fbstp %0" : "=m"(b) : "t"(x) : "st");
    for (int k = 0; k < 10; k++)
        W8(a + (uint32_t)k, b[k]);
    FPOP();
}

void fpu_fpatan(CPU *c)
{
    f80 x = ST(0), y = ST(1), r;
#if PORT_X87_PORTABLE
    if (soft_ok(x) && soft_ok(y)) {
        SOFT_BEGIN(c);
        r = x87s_atan2(y, x);
        SOFT_END(c);
        FPOP();
        ST(0) = r;
        return;
    }
#endif
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    FPOP();
    ST(0) = r;
}

/* fptan / fsincos with a finite |x| >= 2^63 push nothing and set C2 (the operand stays). Caught
   before the host instruction: its asm has two outputs, so the compiler would pop two values the
   CPU never pushed (from pcrecomp 240965e's finding) */
static int trig_out_of_range(CPU *c, f80 x)
{
    if (!(isfinite(x) && fabsl(x) >= 0x1p63L))
        return 0;
    setcc(c, 0x0400, 0x0600);
    return 1;
}

void fpu_fptan(CPU *c)
{
    f80 x = ST(0), one;
    uint16_t sw;
    if (trig_out_of_range(c, x))
        return;
#if PORT_X87_PORTABLE
    if (soft_ok(x)) {
        SOFT_BEGIN(c);
        f80 t = x87s_tan(x);
        SOFT_END(c);
        setcc(c, 0, 0x0600);
        ST(0) = t;
        FPUSH(1.0L);
        return;
    }
#endif
    __asm__("fptan; fnstsw %2" : "=t"(one), "=u"(x), "=am"(sw) : "0"(x));
    setcc(c, sw, 0x0600);
    if (sw & 0x0400)                 /* out of range: operand unchanged, nothing pushed */
        return;
    ST(0) = x;
    FPUSH(one);
}

void fpu_fsincos(CPU *c)
{
    f80 x = ST(0), s, co;
    uint16_t sw;
    if (trig_out_of_range(c, x))
        return;
#if PORT_X87_PORTABLE
    if (soft_ok(x)) {
        SOFT_BEGIN(c);
        s = x87s_sin(x);
        co = x87s_cos(x);
        SOFT_END(c);
        setcc(c, 0, 0x0600);
        ST(0) = s;
        FPUSH(co);
        return;
    }
#endif
    __asm__("fsincos; fnstsw %2" : "=t"(co), "=u"(s), "=am"(sw) : "0"(x));
    setcc(c, sw, 0x0600);
    if (sw & 0x0400)
        return;
    ST(0) = s;
    FPUSH(co);
}

void fpu_fsin(CPU *c)
{
    f80 x = ST(0);
    uint16_t sw;
#if PORT_X87_PORTABLE
    if (soft_ok(x) && fabsl(x) < 0x1p63L) {
        SOFT_BEGIN(c);
        ST(0) = x87s_sin(x);
        SOFT_END(c);
        setcc(c, 0, 0x0600);
        return;
    }
#endif
    __asm__("fsin; fnstsw %1" : "+t"(x), "=am"(sw));
    setcc(c, sw, 0x0600);
    ST(0) = x;
}

void fpu_fcos(CPU *c)
{
    f80 x = ST(0);
    uint16_t sw;
#if PORT_X87_PORTABLE
    if (soft_ok(x) && fabsl(x) < 0x1p63L) {
        SOFT_BEGIN(c);
        ST(0) = x87s_cos(x);
        SOFT_END(c);
        setcc(c, 0, 0x0600);
        return;
    }
#endif
    __asm__("fcos; fnstsw %1" : "+t"(x), "=am"(sw));
    setcc(c, sw, 0x0600);
    ST(0) = x;
}

void fpu_fscale(CPU *c)
{
    f80 x = ST(0), y = ST(1);
    __asm__("fscale" : "+t"(x) : "u"(y));
    ST(0) = x;
}

void fpu_fxtract(CPU *c)
{
    f80 x = ST(0), sig, ex;
    __asm__("fxtract" : "=t"(sig), "=u"(ex) : "0"(x));
    ST(0) = ex;
    FPUSH(sig);
}

void fpu_fprem(CPU *c)
{
    f80 x = ST(0), y = ST(1);
    uint16_t sw;
    __asm__("fprem; fnstsw %1" : "+t"(x), "=am"(sw) : "u"(y));
    setcc(c, sw, 0x4700);
    ST(0) = x;
}

void fpu_fprem1(CPU *c)
{
    f80 x = ST(0), y = ST(1);
    uint16_t sw;
    __asm__("fprem1; fnstsw %1" : "+t"(x), "=am"(sw) : "u"(y));
    setcc(c, sw, 0x4700);
    ST(0) = x;
}

/* 28-byte protected-mode environment: CW, SW, TW, then instruction/operand pointers. */
void fpu_stenv(CPU *c, uint32_t a)
{
    W32(a, c->fcw | 0xffff0000u);
    W32(a + 4, fpu_sw(c) | 0xffff0000u);
    W32(a + 8, 0xffffffffu);
    for (int i = 12; i < 28; i += 4)
        W32(a + i, 0);
}

void fpu_ldenv(CPU *c, uint32_t a)
{
    fpu_setcw(c, R16(a));
    uint16_t sw = (uint16_t)R16(a + 4);
    c->fsw = sw & 0x4700;
    c->ftop = (sw >> 11) & 7;
}

void fpu_save(CPU *c, uint32_t a)
{
    fpu_stenv(c, a);
    W32(a + 8, 0);                    /* all registers tagged valid */
    for (int i = 0; i < 8; i++)
        WF80(a + 28 + 10 * i, ST(i));
    fpu_init(c);
}

void fpu_rstor(CPU *c, uint32_t a)
{
    fpu_ldenv(c, a);
    for (int i = 0; i < 8; i++)
        ST(i) = RF80(a + 28 + 10 * i);
}

/* Generated code keeping the x87 stack in locals assumed a call through a pointer leaves the
   stack depth as it was (tools/fpustack.py); it did not. */
/* (generated code, the x87 stack in locals: after fptan / fsincos, which push nothing for an
   argument past 2^63) */
void fpu_locals_push_check(CPU *c, uint32_t want_top)
{
    if (c->ftop != want_top)
        fatal("fptan/fsincos on an argument past 2^63 (nothing pushed) in a function whose x87 stack "
              "the translator keeps in locals: not followed");
}

void fpu_depth_fail(CPU *c, uint32_t at)
{
    fatal("x87 stack depth changed by the call at %08x (top %u): add the callee's effect to the "
          "translator (tools/fpustack.py)", at, c->ftop);
}
