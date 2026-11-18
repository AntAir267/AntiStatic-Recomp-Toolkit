/* REWRITES THAT OWN A CATCH BLOCK OR AN __except BLOCK (from one port's special wave, which
   rewrote every function with catch blocks and its catch funclets; runtime/win32/eh.c,
   translator/eh.py; docs/REWRITE.md "C++ exceptions").

   How the game's exceptions run here: a throw (_CxxThrowException -> RaiseException, eh.c) walks
   the program's own SEH chain at fs:[0] and calls each record's handler as Windows would. For C++
   the handler is the function's __ehhandler stub (mov eax, FuncInfo; jmp __CxxFrameHandler), the
   translated MSVCRT matching the thrown type against the try blocks of the function's current
   try-level ([ebp-4], the record's third dword), unwinding (RtlUnwind, the destructor funclets)
   and calling the catch funclet with the function's ebp. The funclet returns the address to go on
   at, and MSVCRT's _JumpToContinuation (eh.c's hook) sets esp from the frame's saved one
   ([ebp-0x10]) and ebp, and jumps there. For an __except block (_except_handler3) the jump is a
   call of the block, which never returns (eh.c's eh_resume_call): esp from [ebp-0x18].

   That last jump is the only part translation cannot do by itself: it lands in the middle of a
   function. The translated owner registers a host frame record (eh_push: its entry esp, the
   places it may go on at, a jump buffer) and the runtime longjmps into it. A rewrite does the
   same, so the translated pieces (the stubs, MSVCRT's handler, the funclets) still run:
     - the rewrite builds the original's frame: ebp at E-4, the SEH record at ebp-0xc {next,
       handler, try-level} (C++; _except_handler3's at ebp-0x10 {next, handler, scope table,
       level}), the saved esp at ebp-0x10 (ebp-0x18 for SEH3), fs:[0] pointed at the record, and
       writes the try-level (ebp-4) where the original does, at least at each try block's entry
       and exit (the handler reads it only when an exception passes; the unwind states in between
       only decide which destructor funclets run);
     - the locals a funclet reads are at the original's ebp offsets (the catch object, strings);
     - EH_OWNER below wraps it: the runtime's record with the continuation addresses, and a C
       continuation that runs when the runtime resumes there (c->esp and c->ebp as the original's
       code at that address has them; the callee-saved registers are in their pushed slots).
   The original's try-level writes between are left out where no exception can pass (no call). */
#pragma once
#include "rewrite.h"

/* EH_OWNER(addr, name, body, cont, places...): name, the registered rewrite of addr, runs body(c)
   with a runtime frame record holding the continuation addresses; when the runtime resumes at
   one, cont(c, target) runs instead (it finishes the function: its epilogue and return). body and
   cont return as decomp_ret does. The record lives from entry to return, as the translated
   owner's (translator/codegen.py), so a throw deeper in the call chain finds it. */
#define EH_OWNER(addr, name, body, cont, ...)                                   \
    static __attribute__((noinline)) int name(CPU *c)                         \
    {                                                                           \
        static const uint32_t at_[] = {__VA_ARGS__, 0};                         \
        EHFrame eh_;                                                            \
        eh_push(c, &eh_, c->esp, at_);                                          \
        if (__builtin_setjmp(eh_.jb))                                           \
            cont(c, eh_.target);                                                \
        else                                                                    \
            body(c);                                                            \
        eh_pop(&eh_);                                                           \
        return 1;                                                               \
    }                                                                           \
    DECOMP(addr, name)

/* the C++ prologue (push ebp; mov ebp, esp; push -1; push handler; push fs:[0]; mov fs:[0], esp):
   the frame pointer (E - 4) returned, the caller's ebp in its slot */
static inline uint32_t ehx_open(CPU *c, uint32_t E, uint32_t handler)
{
    uint32_t fp = E - 4;
    W32(fp, c->ebp);
    W32(fp - 4, 0xffffffffu);
    W32(fp - 8, handler);
    W32(fp - 0xc, R32(c->fs_base));
    W32(c->fs_base, fp - 0xc);
    c->ebp = fp;
    return fp;
}
/* the _except_handler3 prologue (push ebp; mov ebp, esp; push -1; push scope table; push handler;
   push fs:[0]; mov fs:[0], esp) */
static inline uint32_t ehs_open(CPU *c, uint32_t E, uint32_t scopes, uint32_t handler)
{
    uint32_t fp = E - 4;
    W32(fp, c->ebp);
    W32(fp - 4, 0xffffffffu);
    W32(fp - 8, scopes);
    W32(fp - 0xc, handler);
    W32(fp - 0x10, R32(c->fs_base));
    W32(c->fs_base, fp - 0x10);
    c->ebp = fp;
    return fp;
}
/* the C++ epilogue (mov ecx, [ebp-0xc]; ... mov fs:[0], ecx; ... mov esp, ebp; pop ebp; ret n):
   fs:[0] back, ecx the previous record as the original leaves it, ebp the caller's */
static inline int ehx_close(CPU *c, uint32_t fp, uint32_t eax, int npop)
{
    c->ecx = R32(fp - 0xc);
    W32(c->fs_base, c->ecx);
    c->ebp = R32(fp);
    return decomp_ret(c, fp + 4, eax, npop);
}
