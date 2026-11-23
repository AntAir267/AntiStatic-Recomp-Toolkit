/* Exceptions (from lixs-native and the C++ line's ports): Windows structured exceptions as MSVC 6 code uses them,
   for C++ throw/catch and __try/__except.

   Raising walks the program's own chain of handler registrations (fs:[0], records on its
   stack: {next, handler}) and calls each handler as Windows does (record, frame, context,
   dispatcher context); the translated MSVCRT handlers (__CxxFrameHandler, _except_handler3)
   do the rest: matching, unwinding (RtlUnwind, below, and their own local unwinds, which call
   destructor funclets) and calling the catch funclet. What translation cannot do by itself is
   the last step, going on in the function that caught: MSVCRT jumps into the middle of it
   (_JumpToContinuation 0x7800ac4f for C++, a call of the __except block for SEH).

   tools/eh.py finds those places from the exception tables; every function whose prologue
   registers such a handler starts by registering a frame record here (eh_push: its program
   stack position at entry, its places, a host jump buffer) and goes on at the place when
   resumed. eh_resume picks the youngest live record that holds the place and whose frame lies at or above the program stack the handler resumes with
   (deeper records are callees being abandoned), sets the program's esp and ebp and jumps
   there on the host, abandoning the host frames between (they hold nothing: guest_call's saved
   registers are replaced by the ones resumed with). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "funcs.h"

static __thread EHFrame *eh_top;
/* while handlers run for an exception (raise_exception's loop): icall then resumes a live frame
   whose place it is sent to rather than calling it, as an __except block can also be a function of
   its own (found through the scope table's pointer) that an SEH4 handler jumps into */
__thread int eh_dispatching;
/* the record and CPU a resume goes to, read back by the resumed function right after its
   __builtin_setjmp returns: Clang (Zig's Windows builds) doesn't keep its locals c and eh_ across
   the longjmp, GCC happens to (from penguins-native) */
static __thread EHFrame *eh_resumed;
static __thread CPU *eh_resumed_c;
EHFrame *eh_resumed_frame(void) { return eh_resumed; }
CPU *eh_resumed_cpu(void) { return eh_resumed_c; }

/* A record lives in its function's host frame, from entry to return (eh_pop); records an
   exception jumps past are dropped by eh_resume. */
void eh_push(CPU *c, EHFrame *f, uint32_t esp, const uint32_t *at)
{
    (void)c;
    f->next = eh_top;
    f->esp = esp;
    f->at = at;
    eh_top = f;
}

void eh_pop(EHFrame *f)
{
    eh_top = f->next;
}

void eh_lost(CPU *c, EHFrame *f)
{
    fatal("resumed function at %08x: no place %08x in it", c->esp, f->target);
}

/* Goes on at `target` with these esp and ebp if a live frame holds it; else returns 0. */
static int eh_resume(CPU *c, uint32_t target, uint32_t esp, uint32_t ebp)
{
    for (EHFrame *f = eh_top; f; f = f->next) {
        if (f->esp < esp)
            continue;                    /* a callee being abandoned */
        for (const uint32_t *p = f->at; *p; p++)
            if (*p == target) {
                LOG(LOG_API, "exception handled: going on at %08x (esp %08x, ebp %08x)", target, esp, ebp);
                eh_top = f;
                f->target = target;
                c->esp = esp;
                c->ebp = ebp;
                eh_resumed = f;
                eh_resumed_c = c;
                eh_dispatching = 0;      /* (the dispatch it came from is abandoned) */
                __builtin_longjmp(f->jb, 1);
            }
    }
    return 0;
}

/* the same for the runtime's other non-local returns (longjmp, nonlocal_jump) */
int eh_resume_at(CPU *c, uint32_t target, uint32_t esp, uint32_t ebp)
{
    return eh_resume(c, target, esp, ebp);
}

/* icall, while an exception is dispatched, of a place a live frame goes on at: an __except block
   (also a translated function) that a handler jumps into with the frame's ebp; the block sets esp
   itself. 0 when it isn't one. */
int eh_resume_place(CPU *c, uint32_t target)
{
    return eh_resume(c, target, c->esp, c->ebp);
}

/* icall of code that is no function: an __except block that _except_handler3 calls (ebp is
   the frame's; the block's first act is esp = [ebp-0x18]) */
int eh_resume_call(CPU *c, uint32_t target)
{
    return eh_resume(c, target, R32(c->ebp - 0x18), c->ebp);
}

/* MSVCRT's _JumpToContinuation(target, registration): unlinks the top registration, esp from
   the frame's saved one ([reg-4]), ebp = reg + 0xc, jumps to target */
extern int (*hook_jump_to_continuation)(CPU *c) __attribute__((weak));
static int jump_to_continuation_hook(CPU *c)
{
    uint32_t target = R32(c->esp + 4), reg = R32(c->esp + 8);
    uint32_t top = R32(c->fs_base);
    W32(c->fs_base, R32(top));
    if (!eh_resume(c, target, R32(reg - 4), reg + 0xc))
        fatal("a C++ catch goes on at %08x, which no live function holds", target);
    return 1;
}

/* ------------------------------------------------------------------ KERNEL32 */
#define EXCEPTION_NONCONTINUABLE 1u
#define EXCEPTION_UNWINDING 2u
#define EXCEPTION_EXIT_UNWIND 4u

static const char *cxx_type(uint32_t args)
{
    uint32_t ti = R32(args + 8);
    if (ti) {
        uint32_t cta = R32(ti + 12);
        if (cta && R32(cta) > 0) {
            uint32_t ct = R32(cta + 4), td = R32(ct + 4);
            if (td) return gstr(td + 8);
        }
    }
    return "?";
}

/* the record (0x50 bytes) and context (0x2cc) on the program's stack below esp */
static void make_record(CPU *c, uint32_t at, uint32_t code, uint32_t flags, uint32_t n, uint32_t args,
                        uint32_t ip)
{
    memset(gptr(at), 0, 0x50 + 0x2cc + 16);
    W32(at, code);
    W32(at + 4, flags);
    W32(at + 0xc, ip);
    if (n > 15) n = 15;
    W32(at + 0x10, n);
    for (uint32_t k = 0; k < n; k++)
        W32(at + 0x14 + 4 * k, R32(args + 4 * k));
    uint32_t ctx = at + 0x50;
    W32(ctx, 0x10007);
    W32(ctx + 0x9c, c->edi); W32(ctx + 0xa0, c->esi); W32(ctx + 0xa4, c->ebx); W32(ctx + 0xa8, c->edx);
    W32(ctx + 0xac, c->ecx); W32(ctx + 0xb0, c->eax); W32(ctx + 0xb4, c->ebp); W32(ctx + 0xb8, ip);
    W32(ctx + 0xc4, c->esp);
}

static uint32_t raise_exception(CPU *c, uint32_t code, uint32_t flags, uint32_t n, uint32_t args, uint32_t ip);
API(KERNEL32, RaiseException, 4)
{
    return raise_exception(c, ARG(0), ARG(1) & EXCEPTION_NONCONTINUABLE, ARG(2), ARG(3), R32(c->esp));
}

/* RaiseException's work: ip is where it was raised from (the return address) */
static uint32_t raise_exception(CPU *c, uint32_t code, uint32_t flags, uint32_t n, uint32_t args, uint32_t ip)
{
    if (code == 0x406d1388u) {
        /* MSVC's "set thread name" for a debugger (args: type 0x1000, name, thread id, flags):
           with no debugger the program's own __except takes it; it simply continues here */
        if (n >= 3 && R32(args) == 0x1000 && R32(args + 4))
            LOG(LOG_THREAD, "thread %x named \"%s\"", R32(args + 8), gstr(R32(args + 4)));
        return 0;
    }
    if (code == 0xe06d7363u && n >= 3)
        LOG(LOG_API, "C++ exception %s thrown (from %08x)", cxx_type(args), ip);
    else
        LOG(LOG_API, "RaiseException(%08x) from %08x", code, ip);
    /* --fncheck: a check child's throw ends its call here, memory as at the throw (fncheck.c) */
    extern void fncheck_throw(CPU *c, uint32_t code, uint32_t nargs, uint32_t args, const char *type)
        __attribute__((weak));
    if (fncheck_throw)
        fncheck_throw(c, code, n, args, code == 0xe06d7363u && n >= 3 ? cxx_type(args) : NULL);
    uint32_t keep_esp = c->esp;
    uint32_t at = (c->esp - 0x400) & ~15u, ctx = at + 0x50, disp = at + 0x50 + 0x2cc;
    make_record(c, at, code, flags, n, args, ip);
    c->esp = at - 64;                    /* the handlers run below the record */
    for (uint32_t reg = R32(c->fs_base); reg != 0xffffffffu && reg; reg = R32(reg)) {
        eh_dispatching++;
        uint32_t r = guest_call(c, R32(reg + 4), 4, at, reg, ctx, disp);
        eh_dispatching--;
        if (r == 0) {                    /* ExceptionContinueExecution */
            if (flags & EXCEPTION_NONCONTINUABLE)
                fatal("a handler continued a non-continuable exception %08x", code);
            c->esp = keep_esp;
            return 0;
        }
    }
    if (code == 0xe06d7363u && n >= 3)
        fatal("the program threw a C++ exception of type %s that nothing catches", cxx_type(args));
    fatal("exception %08x from %08x that nothing handles", code, ip);
}

/* A TEST THROW, for checking that a port's caught C++ exceptions go on (Nitronic Rush's request): a
   port names a function that runs inside a try block in its build config, HOOKS = {addr:
   'test_throw'}, and with <PORT_ENV>_TEST_THROW=N the Nth call of it (1: the first) throws a C++
   exception of type int from its first instruction, as `throw 0;` as its first statement would.
   A catch(...) or catch(int) around the call takes it; nothing catching it ends the run (fatal),
   which says the function wasn't inside a try. Without the variable the function runs as it is. */
int hook_test_throw(CPU *c)
{
    static int nth = -1, calls;
    static uint32_t blk;
    if (nth < 0) {
        const char *e = port_getenv("TEST_THROW");
        nth = e ? atoi(e) : 0;
    }
    if (!nth || ++calls != nth)
        return 0;
    if (!blk) {
        /* in program memory: the thrown int, its ThrowInfo {attributes, unwind, forward compat,
           catchable types}, the array {1, type}, the CatchableType {simple, type descriptor,
           displacement 0 -1 0, size 4, no copy} and the TypeDescriptor {vtable, spare, ".H"} */
        blk = galloc(128);
        uint32_t obj = blk, ti = blk + 8, cta = blk + 24, ct = blk + 32, td = blk + 64, args = blk + 96;
        W32(obj, 0);
        W32(ti, 0); W32(ti + 4, 0); W32(ti + 8, 0); W32(ti + 12, cta);
        W32(cta, 1); W32(cta + 4, ct);
        W32(ct, 1); W32(ct + 4, td); W32(ct + 8, 0); W32(ct + 12, 0xffffffffu); W32(ct + 16, 0);
        W32(ct + 20, 4); W32(ct + 24, 0);
        W32(td, 0); W32(td + 4, 0); W8(td + 8, '.'); W8(td + 9, 'H'); W8(td + 10, 0);
        W32(args, 0x19930520u); W32(args + 4, obj); W32(args + 8, ti);
    }
    LOG(LOG_ALWAYS, "test throw: call %d of the function at the hook (return address %08x)", calls, R32(c->esp));
    raise_exception(c, 0xe06d7363u, EXCEPTION_NONCONTINUABLE, 3, blk + 96, R32(c->esp));
    return 1;
}

/* RtlUnwind(target frame, target ip, record, return value): every registration above the
   target is called to unwind, then unlinked; returns to the caller (MSVCRT's _global_unwind2
   passes its own return address as the target ip) */
API(KERNEL32, RtlUnwind, 4)
{
    uint32_t target = ARG(0), rec = ARG(2), rv = ARG(3);
    uint32_t keep_esp = c->esp;
    uint32_t at = (c->esp - 0x400) & ~15u, ctx = at + 0x50, disp = at + 0x50 + 0x2cc;
    if (rec) {
        memmove(gptr(at), gptr(rec), 0x50);
        memset(gptr(ctx), 0, 0x2cc + 16);
    } else
        make_record(c, at, 0xc0000027u, 0, 0, 0, R32(c->esp));
    W32(at + 4, R32(at + 4) | EXCEPTION_UNWINDING | (target == 0xffffffffu || !target ? EXCEPTION_EXIT_UNWIND : 0));
    c->esp = at - 64;
    uint32_t reg = R32(c->fs_base);
    while (reg != 0xffffffffu && reg && reg != target) {
        guest_call(c, R32(reg + 4), 4, at, reg, ctx, disp);
        reg = R32(reg);
        W32(c->fs_base, reg);
    }
    c->esp = keep_esp;
    return rv;
}

void eh_init(void)
{
    if (&hook_jump_to_continuation) hook_jump_to_continuation = jump_to_continuation_hook;
}

/* ------------------------------------------------------------------ setjmp / longjmp
   For a program that imports them from its C runtime (one translated with the program has its
   own, whose last jump comes back through nonlocal_jump). The buffer is MSVCRT's: ebp, ebx,
   edi, esi, esp at the call (the return address on top), the return address, the SEH
   registration, the try level, then _setjmp3's "VC20" cookie and unwind function. The place
   after each setjmp call is one its function can be resumed at (translator setjmp_places). */
static void setjmp_save(CPU *c, uint32_t b)
{
    W32(b + 0x00, c->ebp);
    W32(b + 0x04, c->ebx);
    W32(b + 0x08, c->edi);
    W32(b + 0x0c, c->esi);
    W32(b + 0x10, c->esp);
    W32(b + 0x14, R32(c->esp));
    W32(b + 0x18, R32(c->fs_base));
    W32(b + 0x1c, 0xffffffffu);
}
#define SETJMP_DLL(dll) \
    API_CDECL(dll, _setjmp) { setjmp_save(c, ARG(0)); return 0; } \
    API_CDECL(dll, _setjmp3) \
    { \
        uint32_t b = ARG(0), n = ARG(1); \
        setjmp_save(c, b); \
        W32(b + 0x20, 0x56433230u);                     /* "VC20" */ \
        W32(b + 0x24, n >= 1 ? ARG(2) : 0);             /* the unwind function */ \
        if (n >= 2) W32(b + 0x1c, ARG(3));              /* the try level */ \
        return 0; \
    } \
    API_CDECL(dll, longjmp) { longjmp_to(c, ARG(0), ARG(1)); return 0; }

/* longjmp(buf, value): the SEH frames registered since setjmp unwound (their handlers called,
   as MSVCRT's _global_unwind2), _setjmp3's unwind function for the try levels, the registers
   back, and the function that called setjmp resumed after the call with eax = value (1 for 0) */
static void longjmp_to(CPU *c, uint32_t b, uint32_t value) __attribute__((noreturn));
static void longjmp_to(CPU *c, uint32_t b, uint32_t value)
{
    uint32_t reg = R32(b + 0x18), target = R32(b + 0x14);
    LOG(LOG_API, "longjmp to %08x (esp %08x)", target, R32(b + 0x10) + 4);
    if (reg != R32(c->fs_base)) {
        uint32_t at = (c->esp - 0x400) & ~15u, ctx = at + 0x50, disp = at + 0x50 + 0x2cc;
        make_record(c, at, 0xc0000027u, 0, 0, 0, target);
        W32(at + 4, R32(at + 4) | EXCEPTION_UNWINDING);
        uint32_t keep = c->esp;
        c->esp = at - 64;
        for (uint32_t r = R32(c->fs_base); r != 0xffffffffu && r && r != reg; r = R32(r)) {
            guest_call(c, R32(r + 4), 4, at, r, ctx, disp);
            W32(c->fs_base, R32(r));
        }
        c->esp = keep;
    }
    if (reg && reg != 0xffffffffu && R32(b + 0x20) == 0x56433230u && R32(b + 0x24))
        guest_call(c, R32(b + 0x24), 1, b);            /* local unwind to the try level */
    c->ebx = R32(b + 0x04);
    c->edi = R32(b + 0x08);
    c->esi = R32(b + 0x0c);
    c->eax = value ? value : 1;
    if (!eh_resume(c, target, R32(b + 0x10) + 4, R32(b + 0x00)))
        fatal("longjmp to %08x, which no live function holds (a setjmp the translator did not see: "
              "build config SETJMP)", target);
    __builtin_unreachable();
}
SETJMP_DLL(MSVCRT)
SETJMP_DLL(MSVCR70)
SETJMP_DLL(MSVCR71)
SETJMP_DLL(MSVCR80)
