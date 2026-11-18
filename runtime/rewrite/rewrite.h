/* REWRITES: a game's functions rewritten as readable, hand-written C, one at a time, each
   replacing the translated function it was written from (docs/REWRITE.md). From the ports'
   rewrite code, first written in etm-native and larush-native.

   The build config's REWRITES file lists the functions being rewritten; the translator gives
   each a hook, hook_dc_<address> (gen/funcs.h, lower-case hex without leading zeros), run at the
   function's entry. A port file registers a rewrite for one with DECOMP(address, fn) at file
   scope; rewrite_install() (at start-up, runtime_setup) switches in every registered one. The
   table is a linker section, so files register without a central list.

   Each can be switched off, to compare a run with the rewrites against one without:
   option decomp = off (all), or <PORT_ENV>_DECOMP_OFF=addr,addr,... (hex, no 0x) or all.
   tools/rewrite/ab.sh runs a game both ways under --test and compares the memory hashes.

   Beside this header: orig_frame.h (calls from the original's stack depth, its exception record),
   eh_owner.h (EH_OWNER: a rewrite of a function with catch blocks), fncheck.h (the per-function
   check, --fncheck SPEC) and audit.c (the register audit, <PORT_ENV>_AUDIT=1); docs/REWRITE.md. */
#pragma once
#include <stdarg.h>
#include "runtime.h"
#include "funcs.h"
/* rewrites call imports as the translation does (weak references: NULL when nothing defines one).
   A rewrite file that defines an import itself must not include this header. */
#if __has_include("funcs_weak.h")
#include "funcs_weak.h"
#endif
#include "image.h"

void rewrite_install(void);         /* the rewrites (DECOMP), then the stack fixes (STACKFIX) */
int decomp_enabled(uint32_t fn);    /* not switched off by option decomp or <PORT_ENV>_DECOMP_OFF */

/* fn is the hook: it runs with the CPU record as at the original's entry (esp at the return
   address, the arguments above it, ecx the object for a method) and returns 1 after doing the
   whole call, decomp_ret included. */
typedef struct { uint32_t addr; int (**hook)(CPU *); int (*fn)(CPU *); } DecompEntry;
#define DECOMP(a, f) \
    static const DecompEntry decomp_entry_##a __attribute__((used, section("decomp_table"), aligned(8))) = \
        {0x##a, &hook_dc_##a, f}
/* (a forked check takes one out of its chain: every place in it that holds fn; fncheck.c) */
int rewrite_switch_off(const DecompEntry *d, int (*fn)(CPU *));

/* a rewrite registered on another hook than its own hook_dc_<addr>: a function with only a stack fix's
   hook (HOOKS {addr: 'sf_<addr>'}, no REWRITES line; STACKFIX chains in front of it) [from a
   port where three files had their own copy] */
#define DECOMP_SF(a, f) \
    static const DecompEntry decomp_entry_##a __attribute__((used, section("decomp_table"), aligned(8))) = \
        {0x##a, &hook_sf_##a, f}
/* (DECOMP and DECOMP_SF both name their entry decomp_entry_<addr>: `nm` on a rewrite's object lists
   what it registers, which is how a port counts its rewrites against the REWRITES list) */

/* Pointers a hook chain keeps to what it calls after itself (a port's runtime hook chained in front
   of a rewrite, as STACKFIX's inner is): registered, so the per-function check (fncheck.c) can take
   one rewrite out of its chain in a forked child. Registering changes nothing else. The stack fixes'
   inner pointers are known without registering. */
typedef int (*DecompHook)(CPU *);
void hook_inner_register(DecompHook *inner);
DecompHook **hook_inner_list(int *n);

static inline int decomp_hook(CPU *c, int (*dc)(CPU *)) { return dc && dc(c); }
static inline void decomp_or_orig(CPU *c, int (*dc)(CPU *), void (*orig)(CPU *))
{
    if (!decomp_hook(c, dc))
        orig(c);
}

/* CALLS AT THE CURRENT STACK POINTER. A rewritten function keeps no stack layout of the
   original's: it calls other code (translated or rewritten, through its hook, so DECOMP_OFF
   still switches each off) with game_call, the arguments pushed below c->esp and DC_RET as the
   return address, and keeps the locals it hands to callees in a frame it lowers c->esp for
   (dc_frame / dc_unframe). Registers as the original: hidden register arguments set before the
   call, callee-saved ones put back before returning, eax/ecx/edx the callee's. Where the callee
   looks at its return address, game_call_at passes the original's. DC_RET is the program's
   first byte (its "MZ" header): mapped, and no code. */
#define DC_RET IMAGE_BASE
static inline uint32_t game_callv(CPU *c, uint32_t ret, uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t sp = c->esp, esp = sp - 4u * (uint32_t)nargs;
    for (int i = 0; i < nargs; i++)
        W32(esp + 4u * (uint32_t)i, args[i]);
    c->esp = esp - 4;
    W32(c->esp, ret);
    icall(c, fn);
    if (c->ret != ret)
        fatal("rewrite: %08x returned to %08x, not %08x", fn, c->ret, ret);
    c->esp = sp;
    return c->eax;
}
/* (the arguments are evaluated into a compound literal before the call: a nested game_call in an
   argument runs first, as C evaluates it; game_callm sets ecx before that, so an argument that
   calls the game must be computed into a local first) */
#define DC_ARGS_(nargs, ...) ((const uint32_t[]){0, ##__VA_ARGS__} + 1)
#define game_call(c, fn, nargs, ...) game_callv(c, DC_RET, fn, nargs, DC_ARGS_(nargs, ##__VA_ARGS__))
#define game_call_at(c, ret, fn, nargs, ...) game_callv(c, ret, fn, nargs, DC_ARGS_(nargs, ##__VA_ARGS__))
#define game_callm(c, fn, obj, nargs, ...) ((c)->ecx = (obj), game_call(c, fn, nargs, ##__VA_ARGS__))
#define game_call_st0(...) ({ game_call(__VA_ARGS__); f80 _v = ST(0); FPOP(); _v; })
#define game_callm_st0(...) ({ game_callm(__VA_ARGS__); f80 _v = ST(0); FPOP(); _v; })

/* the callee-saved registers as the caller had them, kept at entry and put back before returning */
typedef struct { uint32_t ebx, esi, edi, ebp; } Saved;
static inline Saved save_regs(CPU *c) { return (Saved){c->ebx, c->esi, c->edi, c->ebp}; }
static inline void restore_regs(CPU *c, Saved s)
{
    c->ebx = s.ebx; c->esi = s.esi; c->edi = s.edi; c->ebp = s.ebp;
}

/* argument n as at the function's entry, for a rewrite that keeps `uint32_t E = c->esp;` and then
   moves c->esp (ARG reads above the current c->esp: "ARG after lowering esp", REWRITE.md) */
#define EARG(n) R32(E + 4 + 4 * (n))
#define EARGF(n) RF32(E + 4 + 4 * (n))

/* the same for a rewritten function called as C (int fn(CPU *): it reads its arguments above
   c->esp, ARG(n), and returns with decomp_ret), for one with no hook of its own; one with a hook
   is called with game_call, so it can be switched off alone */
static inline uint32_t local_callv(CPU *c, int (*fn)(CPU *), int nargs, const uint32_t *args)
{
    uint32_t sp = c->esp, esp = sp - 4u * (uint32_t)nargs;
    for (int i = 0; i < nargs; i++)
        W32(esp + 4u * (uint32_t)i, args[i]);
    c->esp = esp - 4;
    W32(c->esp, DC_RET);
    fn(c);
    c->esp = sp;
    return c->eax;
}
#define local_call(c, fn, nargs, ...) local_callv(c, fn, nargs, DC_ARGS_(nargs, ##__VA_ARGS__))

/* a frame for locals a callee is handed (guest memory): c->esp lowered by size (rounded to 16),
   its base returned; dc_unframe puts c->esp back before the function returns. Size it to the
   original's whole local area where a callee's buffer sits at its top. */
static inline uint32_t dc_frame(CPU *c, uint32_t size)
{
    c->esp -= (size + 15u) & ~15u;
    return c->esp;
}
static inline void dc_unframe(CPU *c, uint32_t base, uint32_t size)
{
    c->esp = base + ((size + 15u) & ~15u);
}

/* x times -1 as the original's multiplication computes it: GCC turns x * -1.0f into a sign flip,
   which flips a NaN's sign where the multiplication returns the NaN unchanged */
static inline float mul_neg1(float x)
{
    float m1 = -1.0f;
#if defined(__x86_64__) || defined(__i386__)
    __asm__("mulss %1, %0" : "+x"(x) : "x"(m1));
#else
    volatile float v = m1;
    x = x * v;
#endif
    return x;
}

/* an x87 result the compiler must keep as one. GCC folds (float)((f80)a OP (f80)b) of two floats
   (OP * / + -) into one SSE mulss / divss / addss / subss, rounding once straight to float, because
   with a 64-bit mantissa that gives the same number. It does not under a game's control word: at
   24-bit precision the x87 keeps its own exponent range, so the original's fmul rounds once there and
   its fstp to a dword rounds again, which differs from SSE's single rounding where the result is a
   float denormal (below 1.18e-38) [verified: one port's physics, 0.0745627 x -1.2835e-37 x 10,
   one bit apart]. -mfpmath=387 (REWRITE.md) stops most of
   the folding; x87(a * b) is the barrier for a site where it must not happen whatever the flags,
   both operands floats and the result rounded to float. */
static inline f80 x87(f80 v)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__("" : "+t"(v));
#endif
    return v;
}

/* fcom / fcomp / fcompp / ficomp (a against b), then fnstsw ax, as the original leaves eax: the
   compare's condition bits go into the record's status word (C0 0x100 below, C3 0x4000 equal, C0 C2
   C3 unordered), ax becomes the status word with the stack top as the original has it at its fnstsw
   (`depth` values still on its x87 stack beyond the record's own at that point; usually 0) and the
   rest of eax stays. Returns ah's compare bits (C0 0x01, C2 0x04, C3 0x40) for the original's
   `test ah, mask`. A void function whose last eax write is such an fnstsw returns that eax
   [verified: one port's register audit, 2026-11-12: the status word left in eax was its most
   common latent difference] */
static inline unsigned fcom_ax(CPU *c, f80 a, f80 b, int depth)
{
    unsigned cc = (a != a || b != b) ? 0x4500u : a < b ? 0x0100u : a == b ? 0x4000u : 0;
    c->fsw = (uint16_t)((c->fsw & ~0x4700u) | cc);
    uint32_t top = c->ftop;
    c->ftop = (c->ftop - (uint32_t)depth) & 7;
    c->eax = (c->eax & 0xffff0000u) | fpu_sw(c);
    c->ftop = top;
    return cc >> 8;
}

/* the replaced function's return (esp back at entry): eax, pop the return address and nargs
   dwords (stdcall: the original's ret N / 4; 0 for cdecl) */
static inline int decomp_ret(CPU *c, uint32_t entry, uint32_t eax, int nargs)
{
    c->esp = entry;
    c->eax = eax;
    c->ret = R32(c->esp);
    c->esp += 4 + 4u * (uint32_t)nargs;
    return 1;
}

/* the same for a function whose result is a float: pushed on the x87 stack as the original's */
static inline int decomp_ret_st0(CPU *c, uint32_t entry, f80 v, int nargs)
{
    FPUSH(v);
    return decomp_ret(c, entry, c->eax, nargs);
}

/* STACK LEFTOVERS. Some functions copy part of their stack frame into game memory without
   writing it first, so the copy holds whatever an earlier call left at that depth. Rewrites use
   the stack differently, so those bytes differ between the runs with and without them, though
   nothing behaves differently. STACKFIX(addr, size, hookvar) clears [esp - size, esp) at the
   function's entry, in both modes (it is not switched by DECOMP_OFF), so the copies hold zeros
   and the A/B run compares exactly. hookvar is the function's hook: hook_dc_<addr> for one in
   the REWRITES list, else one of its own (HOOKS, e.g. hook_sf_<addr>). It runs in front of
   whatever the hook holds (rewrite or nothing), being installed after the rewrites.
   <PORT_ENV>_STACKFIX_OFF=addr,... leaves those out (seeing what one changes; a test aid). */
typedef struct { const char *addr; int (**hook)(CPU *); int (**inner)(CPU *); int (*fix)(CPU *); } StackFixEntry;
void stackfix_clear(CPU *c, uint32_t size);
#define STACKFIX(a, size, hookvar) \
    static int (*sf_inner_##a)(CPU *); \
    static int sf_fix_##a(CPU *c) { stackfix_clear(c, size); return sf_inner_##a ? sf_inner_##a(c) : 0; } \
    static const StackFixEntry stackfix_entry_##a __attribute__((used, section("stackfix_table"), aligned(8))) = \
        {#a, &hookvar, &sf_inner_##a, sf_fix_##a}
