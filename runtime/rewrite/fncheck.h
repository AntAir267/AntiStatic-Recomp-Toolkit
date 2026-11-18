/* PER-FUNCTION DIFFERENTIAL CHECK (--fncheck SPEC, fncheck.c; docs/REWRITE.md "Checking a function
   no route reaches"; first used on a port rewritten to the end). A route is run to a
   chosen frame; there, for each check, the process forks twice from one state: one child runs the
   translated function (its rewrite switched off), the other the rewrite, with the same inputs, and
   the two are compared: every page of memory the A/B hash covers, the caller's stack, the return
   registers, the x87 stack, the runtime (Windows / DirectX stand-in) calls made, a C++ throw.

   Cases are lines of the spec file (`call ...`, fncheck.c's header) or C functions a port writes
   (FNCASE, in any of its files; a linker section collects them) that build their inputs with the
   game's own code first (game_call / game_callm from rewrite.h: constructors, loaders, factories,
   run in the case's own process, so every check of the case starts from what they built) and then
   call fc_check once per input set:

     #include "fncheck.h"
     FNCASE(quat_normalize, "0x401230 on degenerate quaternions")
     {
         uint32_t q = fc_new(k, 16);
         W32(q, 0); ...
         fc_check(k, "zero", &(FcCall){.fn = 0x401230, FC_THIS(q)});
     }

   What a port gives it (all optional):
     FCNAME(name, about) { return ...; }   a name the spec's expressions can use (e.g. `ctx`: R32
                                           of the game's context pointer), in any port file
     PORT_FNCHECK_NEW 0x...u               (port.h) the game's operator new / malloc (cdecl, one
                                           argument): fc_new and new: blocks come from it, so the code
                                           under test may free them; without it, the process heap
     PORT_FNCHECK_STRING_ASSIGN 0x...u     (port.h) the import slot of MSVCP60's
                                           basic_string::assign(const char *, n): fc_std_string, sstr:
     hook_inner_register(&ptr)             (rewrite.h) a port hook's inner pointer that can hold a
                                           rewrite: the check takes the rewrite out of it too
   <PORT_ENV>_FNCHECK_OUT=FILE: the report (appended; else standard output); <PORT_ENV>_FNCHECK_VERBOSE=1
   describes the rewrite's child on same checks too. */
#pragma once
#include "rewrite.h"

typedef struct FcCtx FcCtx;

/* registers by the CPU record's order */
enum { FC_EAX, FC_ECX, FC_EDX, FC_EBX, FC_ESP_, FC_EBP, FC_ESI, FC_EDI };

typedef struct {
    uint32_t fn;            /* the function's entry (called through its hook: stack fixes stay in) */
    uint32_t reg[8];        /* register inputs, those in regmask (ecx = this for a method) */
    unsigned regmask;
    int nargs;
    uint32_t args[24];      /* stack arguments, args[0] first (lowest) */
    int nfpu;
    f80 fpu[8];             /* pushed on the x87 stack in order: fpu[nfpu - 1] ends in st(0) */
    uint32_t ret;           /* return address pushed (0: DC_RET) */
    const char *expect;     /* NULL, "return", "throw" or "throw:<type name>": checked on the translation */
    unsigned cmp;           /* scratch registers compared too (1u << FC_EDX: a 64-bit or fastcall result,
                               1u << FC_ECX); otherwise their differences are only noted */
} FcCall;
#define FC_THIS(obj) .reg = {[FC_ECX] = (obj)}, .regmask = 1u << FC_ECX
#define FC_ARGS(...) .nargs = (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), .args = {__VA_ARGS__}

typedef struct { const char *name; void (*fn)(FcCtx *k); const char *about; } FcCase;
#define FNCASE(name, about) \
    static void fncase_##name(FcCtx *k); \
    static const FcCase fc_case_##name __attribute__((used, section("fncheck_cases"), aligned(8))) = \
        {#name, fncase_##name, about}; \
    static void fncase_##name(FcCtx *k)

/* a name for the spec's expressions, its value computed in the case's process when used */
typedef struct { const char *name; uint32_t (*fn)(FcCtx *k); const char *about; } FcName;
#define FCNAME(name, about) \
    static uint32_t fcname_##name(FcCtx *k); \
    static const FcName fc_name_##name __attribute__((used, section("fncheck_names"), aligned(8))) = \
        {#name, fcname_##name, about}; \
    static uint32_t fcname_##name(FcCtx *k)

CPU *fc_cpu(FcCtx *k);
/* the case line's key=value parameters (spec: case NAME key=value ...) */
const char *fc_param(FcCtx *k, const char *key, const char *def);
uint32_t fc_param_u32(FcCtx *k, const char *key, uint32_t def);
/* an expression of the spec language (fncheck.c's header): [addr], obj:VTABLE#n, str:"x", ... */
uint32_t fc_eval(FcCtx *k, const char *expr);
/* inputs: a zeroed heap block from the game's own operator new (so the code may free it), a C
   string there, a zeroed buffer on the caller's stack above the call (out parameters the original
   is handed on its caller's stack) */
uint32_t fc_new(FcCtx *k, uint32_t size);
uint32_t fc_str(FcCtx *k, const char *s);
uint32_t fc_std_string(FcCtx *k, const char *s);   /* a new std::string (MSVCP60's) holding s */
/* a runtime function (a Windows stand-in, w_DLL_Name) replaced in the case's process: the translation
   calls the stand-ins directly, so this makes an API answer otherwise for both children */
void fc_host_patch(FcCtx *k, void (*fn)(CPU *), void (*replacement)(CPU *));
uint32_t fc_stack(FcCtx *k, uint32_t size);
/* the n-th (from 0) object whose first dword is vtable; 0 if there are fewer. where: FC_HEAP (heap
   blocks starting with it), FC_ANYWHERE (members and .data too; the stacks never), FC_STACK (the main
   thread's stack from the case's esp up, dword by dword: a game loop's locals, which are no heap
   objects) */
enum { FC_HEAP = 0, FC_ANYWHERE = 1, FC_STACK = 2 };
uint32_t fc_find(FcCtx *k, uint32_t vtable, int n, int where);
/* run the call in both children and compare; returns 0 same, 1 differ, 2 error (no rewrite to
   compare counts as same, with a note). label names the input set in the report. */
int fc_check(FcCtx *k, const char *label, const FcCall *call);
void fc_note(FcCtx *k, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* the runtime core's side (runtime/win32 main.c, eh.c: weak references, so a port without
   runtime/rewrite links without them) */
int fncheck_load(const char *spec);     /* --fncheck SPEC */
void fncheck_frame(unsigned long frame);   /* each presented frame (game frames); 0 before the entry point */
extern int fncheck_child;               /* 1 the translation's child, 2 the rewrite's */
void fncheck_fatal(const char *msg);    /* fatal(): a case or child ends there */
void fncheck_throw(CPU *c, uint32_t code, uint32_t nargs, uint32_t args, const char *type);
int fncheck_api(CPU *c, int nargs);     /* api_trace: 1 when a child recorded the call */
