/* PER-FUNCTION DIFFERENTIAL CHECK: `--fncheck SPEC` (with --test and a route's scripted input; from
   one port's own copy, made game-agnostic). fncheck.h says what it is for and what
   a port gives it; docs/REWRITE.md "Checking a function no route reaches" how to use it.
   Off unless --fncheck is given: nothing here runs, no hook is touched.

   PROCESSES. The game (G) runs the route; when the frame a group of the spec names has been
   presented (frame_presented, main thread, inside the game's Present call; frame 0: just before
   the program's entry point, its DLLs started) it forks a case
   process (P) per spec line and waits for it. P builds the case's inputs (a C case runs the
   game's own constructors; a call line evaluates its expressions) and, for each input set and
   each dead-stack fill, forks the translation's child (T: the function's rewrite switched off,
   everything else as in the run) and then the rewrite's (R), from the same state. Each child runs
   the one call, records what it did into memory shared with P and waits; P compares, reads
   differing pages out of both (process_vm_readv) for the report, and lets them go. Every check
   starts from G's state, so cases cannot disturb each other or the route.

   WHAT IS COMPARED (T against R). Memory: the program's image, the translated DLLs' images and every
   region but the thread stacks, page by page (what the A/B frame hash covers); the main stack
   above the call (the caller's frames, out parameters there); the region list. The call's end:
   how it left (returned, threw, fatal error or crash, timed out), the return address it popped,
   esp (arguments popped), eax, ebx, esi, edi, ebp, the direction flag, the x87 control word,
   top and every value pushed (bit for bit); fs:[0] at the end; the call's own argument slots and
   return address slot as the call left them (MSVC uses argument slots as scratch: a rewrite's
   wrong slot write showed only on routes before, 2026-11-12). The runtime calls made (Windows,
   Direct3D, DirectSound stand-ins): their number, arguments (a pointer into the stack counts
   as "a stack address": frames differ) and results. A throw: its exception code, C++ type and the
   thrown object's bytes, with memory as it was at the throw (before any unwinding; the call runs
   with fs:[0] at the chain's end, so the game's own outer handlers never see it). Only reported:
   ecx and edx (scratch; cmp=edx on a call line compares edx, for a 64-bit result), and the dead
   stack below the call, the slots above excepted (what the function leaves there; a later reader of
   those bytes is outside this check: tests/ab.sh's business).
   DEAD-STACK FILLS (fill keep addr zero): before the children fork, the 64 KB of dead stack below
   the call is left as the state had it (keep), filled with a pattern that depends on each dword's
   address (addr), or cleared (zero). Under addr, a read of a never-written stack byte gives a
   value that depends on the reading frame's depth, so a rewrite calling a leftover-reading callee
   at another depth than the original (or reading its own leftovers differently) shows as a memory
   difference. T under keep against T under addr also tells whether the original itself reads
   leftovers (reported as "the translation reads dead stack").

   SPEC FILE (one directive or case per line; # comments):
     frame N            the cases below run when frame N has been presented (several groups in order;
                        after the last the program exits: code 0 if every check was the same)
     timeout S          seconds a child may take (default 20)
     setup S            seconds a case's process may run without reaching a check (its setup: the
                        game's constructors and loaders; default 60): then it is killed, an ERROR
     fill MODE...       the dead-stack fills each check runs under (default: keep addr)
     maxdiff N          differing byte ranges listed per check (default 16)
     case NAME [k=v...] a C case (FNCASE in runtime/fncheck_cases/) with its parameters
                        (env=NAME=VALUE on a case or call line: set in its process and children)
     call NAME fn=ADDR [this=E] [args=E,E,...] [eax=E] [ecx=E] [edx=E] [ebx=E] [esi=E] [edi=E]
                [ebp=E] [fpu=F,F] [ret=ADDR] [expect=return|throw|throw:TYPE] [cmp=edx,ecx]
                [w32=E=E] [w8=E=E] [wf=E=F]     (writes before the call: address=value; repeatable)
   {a|b|c} anywhere in a case or call line runs it once per alternative (several: every combination).
   EXPRESSIONS E: 0x1f, 31, -4; f:1.5 (a float's bits); [E] (the dword at E); E+N, E-N; NAME (a
   name the port gives with FCNAME, fncheck.h: e.g. ctx); obj:VTABLE[#n] (the n-th heap object with that vtable, from 0);
   any:VTABLE[#n] (anywhere: members, .data); stack:VTABLE[#n] (on the main thread's stack, from
   the case's esp up: a game loop's locals); str:TEXT or str:"TEXT" (a new C string); new:N (a
   zeroed heap block from the game's operator new, PORT_FNCHECK_NEW, else the process heap); stk:N (zeroed bytes on the caller's stack);
   sstr:TEXT or sstr:"TEXT" (a new std::string, MSVCP60's 16 bytes, holding it:
   PORT_FNCHECK_STRING_ASSIGN);
   fv:1;0.5;-2 / dw:1;0x10 (a new block holding those floats / dwords; nan and inf allowed).

   LIMITS. Host-side state is not compared: texture and buffer contents behind Direct3D locks,
   DXVK's render state (only the calls' arguments are), sound buffers, files written. A function
   that waits for another thread (the children have only one) times out; calls into Direct3D run
   in the child on the parent's device (fine for state calls, a hazard for resource creation). The
   case's state is a real one (a route's frame), so inputs are what the code would see there, or
   what the case builds. Forked children crash under the NVIDIA driver: run a game that renders under
   lavapipe (Mesa's software Vulkan) for checks. A function whose rewrite is behind the register
   audit's wrapper (<PORT_ENV>_AUDIT=1, audit.c) can't be switched off: not both at once. */
#if defined(_WIN32)
/* The Windows host has no fork: --fncheck says so, and the case API is there (so a port's cases
   link) but stops if used. */
#include "fncheck.h"
int fncheck_child;
int fncheck_load(const char *spec) { (void)spec; fatal("--fncheck needs Linux (fork)"); }
void fncheck_frame(unsigned long frame) { (void)frame; }
void fncheck_fatal(const char *msg) { (void)msg; }
void fncheck_throw(CPU *c, uint32_t code, uint32_t nargs, uint32_t args, const char *type)
{
    (void)c; (void)code; (void)nargs; (void)args; (void)type;
}
int fncheck_api(CPU *c, int nargs) { (void)c; (void)nargs; return 0; }
CPU *fc_cpu(FcCtx *k) { (void)k; return main_cpu; }
const char *fc_param(FcCtx *k, const char *key, const char *def) { (void)k; (void)key; return def; }
uint32_t fc_param_u32(FcCtx *k, const char *key, uint32_t def) { (void)k; (void)key; return def; }
uint32_t fc_eval(FcCtx *k, const char *expr) { (void)k; (void)expr; fatal("fncheck: not on Windows"); }
uint32_t fc_new(FcCtx *k, uint32_t size) { (void)k; (void)size; fatal("fncheck: not on Windows"); }
uint32_t fc_str(FcCtx *k, const char *s) { (void)k; (void)s; fatal("fncheck: not on Windows"); }
uint32_t fc_std_string(FcCtx *k, const char *s) { (void)k; (void)s; fatal("fncheck: not on Windows"); }
void fc_host_patch(FcCtx *k, void (*fn)(CPU *), void (*r)(CPU *)) { (void)k; (void)fn; (void)r; }
uint32_t fc_stack(FcCtx *k, uint32_t size) { (void)k; (void)size; fatal("fncheck: not on Windows"); }
uint32_t fc_find(FcCtx *k, uint32_t vt, int n, int where) { (void)k; (void)vt; (void)n; (void)where; return 0; }
int fc_check(FcCtx *k, const char *label, const FcCall *call) { (void)k; (void)label; (void)call; return 2; }
void fc_note(FcCtx *k, const char *fmt, ...) { (void)k; (void)fmt; }
#else
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "fncheck.h"
#include "image.h"

#define PAGE 4096u
#define DEAD 0x10000u                   /* dead stack below the call: filled, and reported */
#define STK_AREA 0x4000u                /* stk: buffers, above the call */
#define MAXRANGES 16384
#define MAXAPI 4096

extern unsigned long api_calls;
extern int api_trace_on;
extern const DecompEntry __start_decomp_table[] __attribute__((weak)), __stop_decomp_table[] __attribute__((weak));
extern const FcCase __start_fncheck_cases[] __attribute__((weak)), __stop_fncheck_cases[] __attribute__((weak));
extern const FcName __start_fncheck_names[] __attribute__((weak)), __stop_fncheck_names[] __attribute__((weak));
extern int fncheck_solo;                /* sync.c: no turns given away in a forked process */
extern volatile int det_watch_quiet;    /* sync.c: the stuck-turn watchdog looks away */

/* ------------------------------------------------------------------ the spec */
typedef struct { unsigned long frame; int first, n; } Group;
static char **lines;
static int nlines;
static Group groups[64];
static int ngroups, cur_group;
static int timeout_s = 20, maxdiff = 16, setup_s = 60;
static char fills[4][8] = {"keep", "addr"};
static int nfills = 2;
static int report_fd = 1;

/* totals across case processes: shared memory */
typedef struct {
    int checks, same, differ, error;
    volatile int in_check;              /* the case's process is inside fc_check (its children run) */
    volatile double since;              /* when it last started or left one (its setup's clock) */
} Totals;
static Totals *totals;

int fncheck_child;
static int role;                        /* 0 G, 1 P, 2 a child */

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    if (write(report_fd, buf, n) < 0) {}
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    return s;
}

int fncheck_load(const char *spec)
{
    FILE *f = fopen(spec, "r");
    if (!f) fatal("fncheck: cannot read %s", spec);
    char buf[4096];
    while (fgets(buf, sizeof buf, f)) {
        char *h = strchr(buf, '#');
        if (h && (h == buf || h[-1] == ' ' || h[-1] == '\t')) *h = 0;   /* (obj:..#n keeps its #) */
        char *s = trim(buf);
        if (!*s) continue;
        if (!strncmp(s, "frame ", 6)) {
            if (ngroups == 64) fatal("fncheck: too many frame groups");
            groups[ngroups].frame = strtoul(s + 6, NULL, 0);
            if (ngroups && groups[ngroups].frame <= groups[ngroups - 1].frame)
                fatal("fncheck: %s: frame %lu after frame %lu (groups run in frame order: merge them)", spec,
                      groups[ngroups].frame, groups[ngroups - 1].frame);
            groups[ngroups].first = nlines;
            ngroups++;
            continue;
        }
        if (!strncmp(s, "timeout ", 8)) { timeout_s = atoi(s + 8); continue; }
        if (!strncmp(s, "setup ", 6)) { setup_s = atoi(s + 6); continue; }
        if (!strncmp(s, "maxdiff ", 8)) { maxdiff = atoi(s + 8); continue; }
        if (!strncmp(s, "fill ", 5)) {
            nfills = 0;
            for (char *t = strtok(s + 5, " \t"); t && nfills < 4; t = strtok(NULL, " \t"))
                snprintf(fills[nfills++], sizeof fills[0], "%s", t);
            continue;
        }
        if (strncmp(s, "case ", 5) && strncmp(s, "call ", 5))
            fatal("fncheck: %s: not a directive: %s", spec, s);
        if (!ngroups) fatal("fncheck: %s: a case before the first frame line", spec);
        lines = realloc(lines, (nlines + 1) * sizeof *lines);
        lines[nlines++] = strdup(s);
        groups[ngroups - 1].n++;
    }
    fclose(f);
    if (!ngroups) fatal("fncheck: %s has no frame line", spec);
    const char *o = port_getenv("FNCHECK_OUT");
    if (o && *o) {
        report_fd = open(o, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (report_fd < 0) fatal("fncheck: cannot write %s", o);
    }
    totals = mmap(NULL, sizeof *totals, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    memset(totals, 0, sizeof *totals);
    return 1;
}

/* ------------------------------------------------------------------ the case context */
struct FcCtx {
    CPU *c;
    char name[96];
    const char *params;                 /* the case line after its name */
    uint32_t top, stk, base;            /* stk area [base + .., top), call from base */
};

CPU *fc_cpu(FcCtx *k) { return k->c; }

void fc_note(FcCtx *k, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out("  %s: %s\n", k->name, buf);
}

/* key=value in the params; values may be "quoted" */
const char *fc_param(FcCtx *k, const char *key, const char *def)
{
    static char val[4][1024];
    static int slot;
    size_t kl = strlen(key);
    const char *p = k->params;
    while (p && *p) {
        while (*p == ' ') p++;
        const char *ks = p;
        while (*p && *p != '=' && *p != ' ') p++;
        if (*p != '=') { while (*p && *p != ' ') p++; continue; }
        int match = (size_t)(p - ks) == kl && !strncmp(ks, key, kl);
        p++;
        char *v = val[slot = (slot + 1) & 3];
        size_t n = 0;
        int q = 0;
        while (*p && (q || *p != ' ')) {
            if (*p == '"') q = !q;
            if (n < sizeof val[0] - 1) v[n++] = *p;
            p++;
        }
        v[n] = 0;
        if (match) return v;
    }
    return def;
}

uint32_t fc_param_u32(FcCtx *k, const char *key, uint32_t def)
{
    const char *v = fc_param(k, key, NULL);
    return v ? fc_eval(k, v) : def;
}

uint32_t fc_new(FcCtx *k, uint32_t size)
{
#if defined(PORT_FNCHECK_NEW)
    CPU *c = k->c;
    uint32_t p = game_call(c, PORT_FNCHECK_NEW, 1, size);
#else
    (void)k;
    uint32_t p = heap_alloc(process_heap, size ? size : 1, 1);
#endif
    if (!p) fatal("fncheck: operator new(%u) failed", size);
    memset(gptr(p), 0, size);
    return p;
}

uint32_t fc_str(FcCtx *k, const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = fc_new(k, n);
    memcpy(gptr(p), s, n);
    return p;
}

/* a runtime function replaced in the case's process (and so its children): its first 12 bytes
   become movabs rax, replacement; jmp rax. The translation calls the Windows stand-ins directly
   (w_WINMM_timeBeginPeriod), not through the import table, so this is how a case makes an API
   answer otherwise (a refusal, a failure) for both children. fn must be at least 12 bytes long. */
void fc_host_patch(FcCtx *k, void (*fn)(CPU *), void (*replacement)(CPU *))
{
    (void)k;
    uintptr_t p = (uintptr_t)fn, pg = p & ~(uintptr_t)0xfff;
    if (mprotect((void *)pg, (p + 12 - pg + 0xfff) & ~(uintptr_t)0xfff, PROT_READ | PROT_WRITE | PROT_EXEC))
        fatal("fncheck: cannot patch %p: %s", (void *)fn, strerror(errno));
    uint8_t *b = (uint8_t *)p;
    b[0] = 0x48;
    b[1] = 0xb8;
    memcpy(b + 2, &replacement, 8);
    b[10] = 0xff;
    b[11] = 0xe0;
}

/* (PORT_FNCHECK_STRING_ASSIGN: the import slot of MSVCP60's string::assign(const char *, n)) */
uint32_t fc_std_string(FcCtx *k, const char *s)
{
#if defined(PORT_FNCHECK_STRING_ASSIGN)
    CPU *c = k->c;
    uint32_t str = fc_new(k, 16), cs = fc_str(k, s);
    game_callm(c, R32(PORT_FNCHECK_STRING_ASSIGN), str, 2, cs, (uint32_t)strlen(s));
    return str;
#else
    (void)k; (void)s;
    fatal("fncheck: a std::string needs PORT_FNCHECK_STRING_ASSIGN in port.h");
#endif
}

uint32_t fc_stack(FcCtx *k, uint32_t size)
{
    uint32_t sz = (size + 15) & ~15u;
    if (k->stk - sz < k->base + 64) fatal("fncheck: stk: area full");
    k->stk -= sz;
    memset(gptr(k->stk), 0, sz);
    return k->stk;
}

typedef struct { uint32_t vt; int n, any; uint32_t found; } FindCtx;
static void find_range(void *u, uint32_t base, uint32_t size, int kind)
{
    FindCtx *f = u;
    if (f->found || kind == 3 || (main_cpu && base < main_cpu->stack_hi && base + size > main_cpu->stack_lo)) return;
    uint32_t step = f->any ? 4 : 8;
    for (uint32_t a = base; a + 4 <= base + size; a += step) {
        if (R32(a) != f->vt) continue;
        if (!f->any) {
            uint32_t s, z;
            if (!mem_heap_block(a, &s, &z) || s != a) continue;
        }
        if (f->n-- == 0) { f->found = a; return; }
    }
}


uint32_t fc_find(FcCtx *k, uint32_t vtable, int n, int where)
{
    if (where == FC_STACK) {
        /* the main thread's stack from the case's esp up, dword by dword (a game loop's locals) */
        for (uint32_t a = k->c->esp & ~3u; a + 4 <= k->c->stack_hi; a += 4)
            if (R32(a) == vtable && n-- == 0) return a;
        return 0;
    }
    FindCtx f = {vtable, n, where == FC_ANYWHERE, 0};
    mem_walk(find_range, &f);
    return f.found;
}

/* expressions */
static uint32_t ev_expr(FcCtx *k, const char **pp);
/* p starts with one of the language's own prefixes (obj:, str:, f: ...) */
static int fc_prefix(const char *p)
{
    static const char *pre[] = {"f:", "obj:", "any:", "stack:", "str:", "sstr:", "fv:", "dw:", "new:", "stk:"};
    for (size_t i = 0; i < sizeof pre / sizeof *pre; i++)
        if (!strncmp(p, pre[i], strlen(pre[i]))) return 1;
    return 0;
}
static uint32_t ev_num(const char **pp)
{
    char *e;
    long long v = strtoll(*pp, &e, 0);
    if (e == *pp) fatal("fncheck: a number expected at \"%s\"", *pp);
    *pp = e;
    return (uint32_t)v;
}
static uint32_t ev_primary(FcCtx *k, const char **pp)
{
    const char *p = *pp;
    uint32_t v;
    if (*p == '[') {
        p++;
        uint32_t a = ev_expr(k, &p);
        if (*p != ']') fatal("fncheck: ] expected at \"%s\"", p);
        p++;
        v = R32(a);
    } else if (!strncmp(p, "f:", 2)) {
        char *e;
        float f = strtof(p + 2, &e);
        memcpy(&v, &f, 4);
        p = e;
    } else if ((isalpha((unsigned char)*p) || *p == '_') && !fc_prefix(p)) {
        const char *e = p;              /* a name the port gives (FCNAME) */
        while (isalnum((unsigned char)*e) || *e == '_') e++;
        const FcName *f = __start_fncheck_names;
        for (; f < __stop_fncheck_names; f++)
            if (strlen(f->name) == (size_t)(e - p) && !strncmp(f->name, p, (size_t)(e - p))) break;
        if (f >= __stop_fncheck_names) fatal("fncheck: no name %.*s (FCNAME in the port)", (int)(e - p), p);
        v = f->fn(k);
        p = e;
    } else if (!strncmp(p, "obj:", 4) || !strncmp(p, "any:", 4) || !strncmp(p, "stack:", 6)) {
        int where = p[0] == 'a' ? FC_ANYWHERE : p[0] == 's' ? FC_STACK : FC_HEAP;
        p += where == FC_STACK ? 6 : 4;
        uint32_t vt = ev_num(&p);
        int n = 0;
        if (*p == '#') { p++; n = (int)ev_num(&p); }
        v = fc_find(k, vt, n, where);
        if (!v) fatal("fncheck: no object %d with vtable %08x", n, vt);
    } else if (!strncmp(p, "str:", 4) || !strncmp(p, "sstr:", 5)) {
        int std = p[0] == 's';          /* sstr: a std::string (MSVCP60's, 16 bytes) holding it */
        p += std ? 5 : 4;
        char buf[1024];
        size_t n = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"' && n < sizeof buf - 1) {
                if (*p == '\\' && p[1] == 'n') { buf[n++] = '\n'; p += 2; continue; }
                if (*p == '\\' && p[1]) p++;
                buf[n++] = *p++;
            }
            if (*p == '"') p++;
        } else
            while (*p && *p != ',' && *p != ' ' && *p != ']' && n < sizeof buf - 1) buf[n++] = *p++;
        buf[n] = 0;
        v = std ? fc_std_string(k, buf) : fc_str(k, buf);
    } else if (!strncmp(p, "fv:", 3) || !strncmp(p, "dw:", 3)) {
        int fl = p[0] == 'f';           /* a new block holding floats / dwords: fv:1;0.5;-2 */
        p += 3;
        uint32_t vals[64];
        int n = 0;
        for (;;) {
            char *e;
            if (fl) { float f = strtof(p, &e); memcpy(&vals[n], &f, 4); }
            else vals[n] = (uint32_t)strtoll(p, &e, 0);
            if (e == p) fatal("fncheck: a value expected at \"%s\"", p);
            p = e;
            if (++n == 64 || *p != ';') break;
            p++;
        }
        v = fc_new(k, 4u * (uint32_t)n);
        memcpy(gptr(v), vals, 4u * (uint32_t)n);
    } else if (!strncmp(p, "new:", 4)) {
        p += 4;
        v = fc_new(k, ev_num(&p));
    } else if (!strncmp(p, "stk:", 4)) {
        p += 4;
        v = fc_stack(k, ev_num(&p));
    } else
        v = ev_num(&p);
    *pp = p;
    return v;
}
static uint32_t ev_expr(FcCtx *k, const char **pp)
{
    uint32_t v = ev_primary(k, pp);
    while (**pp == '+' || **pp == '-') {
        int neg = **pp == '-';
        (*pp)++;
        uint32_t w = ev_primary(k, pp);
        v = neg ? v - w : v + w;
    }
    return v;
}
uint32_t fc_eval(FcCtx *k, const char *expr)
{
    const char *p = expr;
    uint32_t v = ev_expr(k, &p);
    if (*p) fatal("fncheck: junk after expression: \"%s\"", p);
    return v;
}

/* ------------------------------------------------------------------ the children's record */
typedef struct { uint32_t base, size; int kind; uint32_t first; } RangeRec;   /* first: its first page's index */
typedef struct { uint32_t nargs, ret; uint32_t args[6]; uint64_t more; } ApiRec;
enum { ST_NONE, ST_RET, ST_THROW, ST_FATAL, ST_CRASH, ST_TIMEOUT, ST_NOSWITCH };
static const char *st_name[] = {"nothing", "returned", "threw", "fatal error", "crashed", "timed out",
                                "could not switch the rewrite off"};
typedef struct {
    int status;
    char msg[600];
    uint32_t r[8], ret, ecx, df, fcw, ftop0, ftop, fs0;
    uint8_t st[8][10];
    int npushed, nargs;
    uint32_t slots[1 + 24];             /* the return address slot and the argument slots after the call */
    uint32_t exc_code, exc_size;
    uint64_t exc_hash;
    char exc_type[128];
    unsigned long napi;
    ApiRec api[MAXAPI];
    int nranges;
    RangeRec ranges[MAXRANGES];
    uint32_t npages;
    uint64_t pages[];                   /* page hashes, in range order */
} Result;
static Result *res;                     /* (in a child: its record) */
static size_t res_size;
static uint32_t c_base, c_live_hi, c_fs_saved;

static uint64_t page_hash(uint32_t a)
{
    const uint64_t *p = (const uint64_t *)(uintptr_t)a;
    uint64_t h = 0xcbf29ce484222325ull ^ a;
    for (uint32_t i = 0; i < PAGE / 8; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
        h ^= h >> 29;
    }
    return h;
}

static void add_range(Result *r, uint32_t base, uint32_t size, int kind)
{
    if (r->nranges == MAXRANGES) fatal("fncheck: more than %d memory ranges", MAXRANGES);
    RangeRec *g = &r->ranges[r->nranges++];
    g->base = base; g->size = size; g->kind = kind; g->first = r->npages;
    for (uint32_t a = base; a < base + size; a += PAGE)
        r->pages[r->npages++] = page_hash(a);
}
/* the main thread's stack is compared by the call's own windows (kinds 4 and 5), never as a region
   (PORT_HASH_STACKS ports have their stacks as plain regions) */
static int main_stack(uint32_t base, uint32_t size)
{
    return main_cpu && base < main_cpu->stack_hi && base + size > main_cpu->stack_lo;
}
static void range_cb(void *u, uint32_t base, uint32_t size, int kind)
{
    if (kind != 3 && !main_stack(base, size)) add_range(u, base, size, kind);
}
/* kinds: 0 the image, 1 DLL image, 2 region, 4 caller stack (live, above the call), 5 dead stack */
static void hash_memory(Result *r)
{
    r->nranges = 0;
    r->npages = 0;
    mem_walk(range_cb, r);
    add_range(r, c_base, c_live_hi - c_base, 4);
    add_range(r, c_base - DEAD, DEAD, 5);
}
static void count_cb(void *u, uint32_t base, uint32_t size, int kind)
{
    (void)base; (void)kind;
    *(uint32_t *)u += (size + PAGE - 1) / PAGE;
}
static uint32_t count_pages(void)
{
    uint32_t n = 0;
    mem_walk(count_cb, &n);
    return n + (c_live_hi - c_base) / PAGE + DEAD / PAGE + 64;
}

/* the child's end: what the call left, memory hashed, then wait for P */
static int done_fd = -1, release_fd = -1;
static void child_finish(CPU *c, int status)
{
    api_trace_on = 0;
    fncheck_child = 0;                  /* (no hooks while hashing) */
    res->status = status;
    res->fs0 = R32(c->fs_base);
    W32(c->fs_base, c_fs_saved);
    res->r[0] = c->eax; res->r[1] = c->ecx; res->r[2] = c->edx; res->r[3] = c->ebx;
    res->r[4] = c->esp - c_base; res->r[5] = c->ebp; res->r[6] = c->esi; res->r[7] = c->edi;
    res->ecx = c->ecx;
    res->ret = c->ret;
    res->df = c->df;
    res->fcw = c->fcw;
    res->ftop = c->ftop;
    int pushed = (int)((res->ftop0 - c->ftop) & 7);
    res->npushed = pushed;
    for (int i = 0; i < pushed; i++)
        memcpy(res->st[i], &c->st[(c->ftop + i) & 7], 10);
    uint32_t at = c_base - 4u * (uint32_t)(res->nargs + 1);   /* the call's esp: return address, arguments */
    for (int i = 0; i <= res->nargs; i++) res->slots[i] = R32(at + 4u * (uint32_t)i);
    hash_memory(res);
    char b = 'd';
    if (write(done_fd, &b, 1) < 0) {}
    while (read(release_fd, &b, 1) > 0) {}
    _exit(0);
}

void fncheck_fatal(const char *msg)
{
    if (role == 2 && res) {
        res->status = strncmp(msg, "signal ", 7) ? ST_FATAL : ST_CRASH;
        snprintf(res->msg, sizeof res->msg, "%s", msg);
        char b = 'f';
        if (write(done_fd, &b, 1) < 0) {}
        _exit(2);
    }
    if (role == 1) {
        out("  setup FATAL: %s\n", msg);
        _exit(2);
    }
}

void fncheck_throw(CPU *c, uint32_t code, uint32_t nargs, uint32_t args, const char *type)
{
    if (role != 2 || !res) return;
    res->exc_code = code;
    snprintf(res->exc_type, sizeof res->exc_type, "%s", type ? type : "");
    if (code == 0xe06d7363u && nargs >= 3) {
        uint32_t obj = R32(args + 4), ti = R32(args + 8), size = 0;
        if (ti && R32(ti + 12) && R32(R32(ti + 12)) > 0)
            size = R32(R32(R32(ti + 12) + 4) + 0x14);     /* CatchableType's sizeOrOffset */
        if (size > 0x1000) size = 0x1000;
        res->exc_size = size;
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint32_t i = 0; obj && i < size; i++) { h ^= R8(obj + i); h *= 0x100000001b3ull; }
        res->exc_hash = h;
    }
    child_finish(c, ST_THROW);
}

int fncheck_api(CPU *c, int nargs)
{
    if (!fncheck_child || !res) return 0;
    unsigned long i = res->napi++;
    if (i >= MAXAPI) return 1;
    ApiRec *a = &res->api[i];
    a->nargs = (uint32_t)nargs;
    a->ret = c->eax;
    uint32_t at = c->esp - 4u * (uint32_t)nargs;
    uint64_t h = 0;
    for (int j = 0; j < nargs; j++) {
        uint32_t v = R32(at + 4u * (uint32_t)j);
        if (v >= c->stack_lo && v < c->stack_hi) v = 0x57ac0000u;   /* a stack address */
        if (j < 6) a->args[j] = v;
        else { h ^= v; h *= 0x100000001b3ull; }
    }
    a->more = h;
    return 1;
}

/* ------------------------------------------------------------------ one child */
static const DecompEntry *entry_of(uint32_t fn)
{
    for (const DecompEntry *d = __start_decomp_table; d < __stop_decomp_table; d++)
        if (d->addr == fn) return d;
    return NULL;
}

/* the rewrite out of the function's hook chain (rewrite.c: the hook itself, a stack fix's or a
   registered runtime hook's inner pointer that holds it); how many places held it */
static int switch_off(const DecompEntry *d)
{
    return rewrite_switch_off(d, d->fn);
}

/* read-only regular files: their offsets, put back before each child runs (a forked child shares
   them with its parent: a parser reading an open file would move the other child's position) */
typedef struct { int n; struct { int fd; off_t off; } f[256]; } FdSet;
static FdSet fds_p, fds_g;
static void fds_save(FdSet *s)
{
    s->n = 0;
    for (int fd = 3; fd < 1024 && s->n < 256; fd++) {
        int fl = fcntl(fd, F_GETFL);
        struct stat st;
        if (fl < 0 || (fl & O_ACCMODE) != O_RDONLY || fstat(fd, &st) || !S_ISREG(st.st_mode)) continue;
        s->f[s->n].fd = fd;
        s->f[s->n++].off = lseek(fd, 0, SEEK_CUR);
    }
}
static void fds_restore(const FdSet *s)
{
    for (int i = 0; i < s->n; i++) lseek(s->f[i].fd, s->f[i].off, SEEK_SET);
}

typedef struct { pid_t pid; int done, release; Result *r; } Child;
/* P's ends of the live children's pipes: a new child closes them (else the first child never sees
   its release pipe close: the second holds a copy of its write end) */
static int live_fds[8], nlive;

static void run_child(FcCtx *k, const FcCall *call, int which, const DecompEntry *d, Child *ch, Result *r)
{
    int p1[2], p2[2];
    if (pipe(p1) || pipe(p2)) fatal("fncheck: pipe: %s", strerror(errno));
    memset(r, 0, offsetof(Result, ranges));
    r->nranges = 0;
    r->npages = 0;
    fds_restore(&fds_p);
    int tid0 = (int)gettid();
    pid_t pid = fork();
    if (pid < 0) fatal("fncheck: fork: %s", strerror(errno));
    if (pid == 0) {
        sync_fork_child(tid0);
        close(p1[0]); close(p2[1]);
        for (int i = 0; i < nlive; i++) close(live_fds[i]);
        done_fd = p1[1]; release_fd = p2[0];
        role = 2;
        res = r;
        fncheck_solo = 1;
        CPU *c = k->c;
        if (which == 1 && d && !switch_off(d)) {
            r->status = ST_NOSWITCH;
            char b = 'x';
            if (write(done_fd, &b, 1) < 0) {}
            _exit(0);
        }
        /* the call: arguments below base, the return address below them */
        uint32_t esp = c_base - 4u * (uint32_t)call->nargs;
        for (int i = 0; i < call->nargs; i++) W32(esp + 4u * (uint32_t)i, call->args[i]);
        esp -= 4;
        W32(esp, call->ret ? call->ret : DC_RET);
        static const uint32_t defaults[8] = {0, 0, 0, 0xb0b0b0b0u, 0, 0xbebebebeu, 0x51515151u, 0xd1d1d1d1u};
        uint32_t *regs = &c->eax;
        for (int i = 0; i < 8; i++)
            if (i != FC_ESP_) regs[i] = (call->regmask >> i) & 1 ? call->reg[i] : defaults[i];
        c->esp = esp;
        c->df = 0;
        for (int i = 0; i < call->nfpu; i++) FPUSH(call->fpu[i]);
        r->ftop0 = c->ftop;
        r->nargs = call->nargs;
        c_fs_saved = R32(c->fs_base);
        W32(c->fs_base, 0xffffffffu);   /* a throw finds no handler of the game's: fncheck_throw */
        fncheck_child = which;
        api_trace_on = 1;
        icall(c, call->fn);
        child_finish(c, ST_RET);
    }
    close(p1[1]); close(p2[0]);
    if (nlive + 2 <= 8) { live_fds[nlive++] = p1[0]; live_fds[nlive++] = p2[1]; }
    ch->pid = pid; ch->done = p1[0]; ch->release = p2[1]; ch->r = r;
    struct pollfd pf = {p1[0], POLLIN, 0};
    int n = poll(&pf, 1, timeout_s * 1000);
    char b = 0;
    if (n <= 0) {
        kill(pid, SIGKILL);
        r->status = ST_TIMEOUT;
    } else if (read(p1[0], &b, 1) != 1) {
        int ws = 0;
        waitpid(pid, &ws, 0);
        ch->pid = 0;
        if (r->status == ST_NONE || r->status == ST_RET) {
            r->status = ST_CRASH;
            snprintf(r->msg, sizeof r->msg, "child ended (%s %d) without a word",
                     WIFSIGNALED(ws) ? "signal" : "exit", WIFSIGNALED(ws) ? WTERMSIG(ws) : WEXITSTATUS(ws));
        }
    }
    (void)k;
}

static void release(Child *ch)
{
    if (ch->release >= 0) close(ch->release);
    if (ch->done >= 0) close(ch->done);
    if (ch->pid > 0) {
        int ws;
        if (ch->r->status == ST_TIMEOUT) kill(ch->pid, SIGKILL);
        waitpid(ch->pid, &ws, 0);
    }
    for (int i = 0; i < nlive; i++)
        if (live_fds[i] == ch->release || live_fds[i] == ch->done) live_fds[i--] = live_fds[--nlive];
    ch->pid = 0; ch->release = ch->done = -1;
}

/* ------------------------------------------------------------------ comparing */
static const char *kind_name[] = {"image", "dll", "region", "stack", "caller stack", "dead stack"};

static int read_child(pid_t pid, uint32_t a, void *buf, size_t n)
{
    struct iovec l = {buf, n}, rm = {(void *)(uintptr_t)a, n};
    return process_vm_readv(pid, &l, 1, &rm, 1, 0) == (ssize_t)n;
}

static void where(char *s, size_t n, uint32_t a, int kind)
{
    uint32_t st, sz;
    if (kind == 2 && mem_heap_block(a, &st, &sz))
        snprintf(s, n, "heap block %08x+%x (size %x, first dword %08x)", st, a - st, sz, R32(st));
    else if (kind >= 4)
        snprintf(s, n, "%s, base%+d", kind_name[kind], (int)(a - c_base));
    else
        snprintf(s, n, "%s", kind_name[kind]);
}

/* the differing bytes of one page, as ranges; returns ranges listed */
static int page_diff(const Child *t, const Child *rr, uint32_t pa, int kind, int budget)
{
    static uint8_t A[PAGE], B[PAGE];
    if (!read_child(t->pid, pa, A, PAGE) || !read_child(rr->pid, pa, B, PAGE)) {
        out("    page %08x (%s): could not read the children's copies\n", pa, kind_name[kind]);
        return 1;
    }
    int listed = 0;
    for (uint32_t i = 0; i < PAGE && listed < budget;) {
        if (A[i] == B[i]) { i++; continue; }
        uint32_t j = i;
        while (j < PAGE && (A[j] != B[j] || (j + 1 < PAGE && A[j + 1] != B[j + 1]) ||
                            (j + 2 < PAGE && A[j + 2] != B[j + 2])))
            j++;
        if (j > i + 32) j = i + 32;
        char w[160], ha[100] = "", hb[100] = "";
        where(w, sizeof w, pa + i, kind);
        for (uint32_t x = i; x < j && x < i + 32; x++) {
            snprintf(ha + strlen(ha), sizeof ha - strlen(ha), "%02x", A[x]);
            snprintf(hb + strlen(hb), sizeof hb - strlen(hb), "%02x", B[x]);
        }
        out("    %08x (%s): translation %s, rewrite %s\n", pa + i, w, ha, hb);
        listed++;
        i = j;
    }
    return listed;
}

static const RangeRec *range_at(const Result *r, uint32_t base)
{
    for (int i = 0; i < r->nranges; i++)
        if (r->ranges[i].base == base) return &r->ranges[i];
    return NULL;
}

/* pages of b that differ from a (same ranges by base); calls fn per differing page */
static int pages_differ(const Result *a, const Result *b, int with_dead,
                        void (*fn)(void *u, uint32_t page, int kind), void *u)
{
    int n = 0;
    for (int i = 0; i < a->nranges; i++) {
        const RangeRec *ra = &a->ranges[i], *rb = range_at(b, ra->base);
        if (!rb || (ra->kind == 5 && !with_dead)) continue;
        uint32_t np = ((ra->size < rb->size ? ra->size : rb->size) + PAGE - 1) / PAGE;
        for (uint32_t p = 0; p < np; p++)
            if (a->pages[ra->first + p] != b->pages[rb->first + p]) {
                n++;
                if (fn) fn(u, ra->base + p * PAGE, ra->kind);
            }
    }
    return n;
}

static const char *fmt_st(const uint8_t *s, char *buf)
{
    f80 v;
    memcpy(&v, s, 10);
    snprintf(buf, 40, "%.9Lg", v);
    return buf;
}

static void describe(const char *who, const Result *r, int changed)
{
    char b[48];
    out("    %s: %s", who, st_name[r->status]);
    if (r->status == ST_RET || r->status == ST_THROW) {
        if (r->status == ST_RET) out(" eax=%08x edx=%08x pop=%d", r->r[0], r->r[2], (int)r->r[4] / 4 + r->nargs);
        for (int i = 0; i < r->npushed; i++) out(" st%d=%s", i, fmt_st(r->st[i], b));
        out(", %lu runtime calls, %d pages changed", r->napi, changed);
    }
    if (r->status == ST_THROW) out(", %s (code %08x, %u bytes)", r->exc_type, r->exc_code, r->exc_size);
    if (r->msg[0]) out(": %s", r->msg);
    out("\n");
}

typedef struct { const Child *t, *r; int budget, pages; } DiffCtx;
static void diff_page(void *u, uint32_t page, int kind)
{
    DiffCtx *d = u;
    d->pages++;
    if (d->budget > 0) d->budget -= page_diff(d->t, d->r, page, kind, d->budget);
}

/* T against R: returns 0 same, 1 differ, 2 error */
static int compare(const Child *t, const Child *rch, unsigned cmp, char *why, size_t nwhy, char *note, size_t nnote)
{
    const Result *a = t->r, *b = rch->r;
    why[0] = 0;
#define WHY(...) snprintf(why + strlen(why), nwhy - strlen(why), __VA_ARGS__)
    if (a->status == ST_TIMEOUT || a->status == ST_NOSWITCH) { WHY("translation %s", st_name[a->status]); return 2; }
    if (a->status != b->status) { WHY("translation %s, rewrite %s", st_name[a->status], st_name[b->status]); return 1; }
    if (a->status == ST_FATAL || a->status == ST_CRASH || a->status == ST_TIMEOUT) {
        if (strcmp(a->msg, b->msg)) { WHY("messages differ"); return 1; }
        return 0;
    }
    static const char *rn[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    if (a->status == ST_RET) {          /* (at a throw the registers are the thrower's, inside) */
        for (int i = 0; i < 8; i++)
            if (a->r[i] != b->r[i]) {
                if ((i == FC_ECX || i == FC_EDX) && !((cmp >> i) & 1))
                    snprintf(note + strlen(note), nnote - strlen(note), "%s %08x / %08x; ", rn[i], a->r[i], b->r[i]);
                else
                    WHY("%s %08x / %08x; ", rn[i], a->r[i], b->r[i]);
            }
        if (a->ret != b->ret) WHY("returned to %08x / %08x; ", a->ret, b->ret);
        if (a->df != b->df) WHY("df; ");
        if (a->fcw != b->fcw) WHY("fpu control %04x / %04x; ", a->fcw, b->fcw);
        if (a->ftop != b->ftop) WHY("x87 depth %d / %d; ", a->npushed, b->npushed);
        else
            for (int i = 0; i < a->npushed; i++)
                if (memcmp(a->st[i], b->st[i], 10)) WHY("st%d; ", i);
        if (a->fs0 != b->fs0) WHY("fs:[0] %08x / %08x; ", a->fs0, b->fs0);
    }
    if (a->status == ST_RET || a->status == ST_THROW)   /* (in the dead window, but the call's own) */
        for (int i = 0; i <= a->nargs; i++)
            if (a->slots[i] != b->slots[i]) {
                if (i) WHY("argument slot %d %08x / %08x; ", i - 1, a->slots[i], b->slots[i]);
                else WHY("return address slot %08x / %08x; ", a->slots[i], b->slots[i]);
            }
    if (a->status == ST_THROW && (strcmp(a->exc_type, b->exc_type) || a->exc_code != b->exc_code ||
                                  a->exc_size != b->exc_size || a->exc_hash != b->exc_hash))
        WHY("the thrown objects differ; ");
    unsigned long na = a->napi < MAXAPI ? a->napi : MAXAPI;
    if (a->napi != b->napi) WHY("%lu / %lu runtime calls; ", a->napi, b->napi);
    else
        for (unsigned long i = 0; i < na; i++)
            if (memcmp(&a->api[i], &b->api[i], sizeof a->api[i])) {
                WHY("runtime call %lu differs (args %08x %08x %08x %08x, result %08x / %08x %08x %08x %08x, %08x); ",
                    i, a->api[i].args[0], a->api[i].args[1], a->api[i].args[2], a->api[i].args[3], a->api[i].ret,
                    b->api[i].args[0], b->api[i].args[1], b->api[i].args[2], b->api[i].args[3], b->api[i].ret);
                break;
            }
    if (a->nranges != b->nranges) WHY("%d / %d memory ranges; ", a->nranges, b->nranges);
    int np = pages_differ(a, b, 0, NULL, NULL);
    if (np) WHY("%d pages differ; ", np);

#undef WHY
    return why[0] ? 1 : 0;
}

/* ------------------------------------------------------------------ a check */
static Result *rbuf[2 + 4];
static Result *base_res;

static Result *new_result(void)
{
    Result *r = mmap(NULL, res_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (r == MAP_FAILED) fatal("fncheck: no memory for a result (%zu bytes)", res_size);
    return r;
}

static void fill_dead(const char *mode, const uint8_t *keep)
{
    uint32_t lo = c_base - DEAD;
    if (!strcmp(mode, "keep")) memcpy(gptr(lo), keep, DEAD);
    else if (!strcmp(mode, "zero")) memset(gptr(lo), 0, DEAD);
    else if (!strcmp(mode, "addr"))
        for (uint32_t a = lo; a < c_base; a += 4) W32(a, (a * 2654435761u) ^ 0x5ca1ab1eu);
    else fatal("fncheck: unknown fill %s", mode);
}

int fc_check(FcCtx *k, const char *label, const FcCall *call)
{
    totals->in_check = 1;               /* (the setup clock stops: the children have their own timeout) */
    const DecompEntry *d = entry_of(call->fn);
    int have_rw = d && decomp_enabled(call->fn);
    if (!res_size) {
        res_size = offsetof(Result, pages) + (size_t)count_pages() * 8 + 0x100000;
        res_size = (res_size + 0xfff) & ~(size_t)0xfff;
    }
    for (int i = 0; i < 2 + nfills; i++) if (!rbuf[i]) rbuf[i] = new_result();
    /* the state before the call, for "pages changed" (P's own memory) */
    if (!base_res) base_res = new_result();
    hash_memory(base_res);
    static uint8_t keep[DEAD];
    memcpy(keep, gptr(c_base - DEAD), DEAD);
    fds_save(&fds_p);
    int worst = 0;
    Result *t_first = NULL;
    for (int f = 0; f < nfills; f++) {
        fill_dead(fills[f], keep);
        Child t = {0, -1, -1, NULL}, r = {0, -1, -1, NULL};
        Result *tr = rbuf[2 + f], *rr = rbuf[1];
        run_child(k, call, 1, d, &t, tr);
        int verdict;
        char why[2048] = "", note[256] = "";
        totals->checks++;
        if (have_rw) {
            run_child(k, call, 2, d, &r, rr);
            verdict = compare(&t, &r, call->cmp, why, sizeof why, note, sizeof note);
        } else
            verdict = tr->status == ST_TIMEOUT ? 2 : 0;
        if (verdict == 0 && call->expect) {
            const char *e = call->expect;
            if ((!strcmp(e, "return") && tr->status != ST_RET) ||
                (!strncmp(e, "throw", 5) && (tr->status != ST_THROW || (e[5] == ':' && strcmp(e + 6, tr->exc_type))))) {
                verdict = 2;
                snprintf(why, sizeof why, "expected %s, the translation %s", e, st_name[tr->status]);
            }
        }
        int ct = pages_differ(base_res, tr, 0, NULL, NULL);
        out("%s %s%s%s [%s]: %s%s%s\n", verdict == 0 ? "same  " : verdict == 1 ? "DIFFER" : "ERROR ",
            k->name, label && *label ? " / " : "", label ? label : "", fills[f],
            verdict == 0 ? (have_rw ? "same" : d ? "rewrite switched off (DECOMP_OFF): translation only"
                                             : "no rewrite registered: translation only") : "",
            verdict ? why : "", "");
        describe("translation", tr, ct);
        if (note[0]) out("    (scratch registers differ, not compared: %s)\n", note);
        if (have_rw && rr->status != ST_NONE && (verdict || port_getenv("FNCHECK_VERBOSE")))
            describe("rewrite    ", rr, pages_differ(base_res, rr, 0, NULL, NULL));
        if (verdict == 1 && t.pid > 0 && r.pid > 0 && tr->npages && rr->npages) {
            DiffCtx dc = {&t, &r, maxdiff, 0};
            pages_differ(tr, rr, 0, diff_page, &dc);
        }
        if (have_rw && (verdict || port_getenv("FNCHECK_VERBOSE")) && t.pid > 0 && r.pid > 0 && tr->npages && rr->npages) {
            int dead = pages_differ(tr, rr, 1, NULL, NULL) - pages_differ(tr, rr, 0, NULL, NULL);
            if (dead) out("    (the dead stack below the call differs on %d page%s: leftovers, not compared)\n",
                          dead, dead > 1 ? "s" : "");
        }
        if (!t_first && tr->npages) t_first = tr;
        else if (t_first && tr->npages && tr != t_first) {
            int n = pages_differ(t_first, tr, 0, NULL, NULL);
            if (n) out("    the translation reads dead stack: fill %s against %s changes %d page%s of memory\n",
                       fills[0], fills[f], n, n > 1 ? "s" : "");
        }
        release(&t);
        release(&r);
        if (verdict == 0) totals->same++;
        else if (verdict == 1) totals->differ++;
        else totals->error++;
        if (verdict > worst) worst = verdict;
    }
    fill_dead("keep", keep);
    fds_restore(&fds_p);
    totals->since = now_s();
    totals->in_check = 0;
    return worst;
}

/* ------------------------------------------------------------------ call lines */
static void call_line(FcCtx *k, char *line)
{
    FcCall call = {0};
    char label[512] = "";
    char *p = line;
    struct { uint32_t a; uint32_t v; int size; } writes[32];
    int nw = 0;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *key = p;
        while (*p && *p != '=' && *p != ' ') p++;
        if (*p != '=') fatal("fncheck: call: key=value expected at \"%s\"", key);
        *p++ = 0;
        char *v = p;
        int q = 0, br = 0;
        while (*p && (q || br || *p != ' ')) {
            if (*p == '"') q = !q;
            else if (!q && *p == '[') br++;
            else if (!q && *p == ']') br--;
            p++;
        }
        if (*p) *p++ = 0;
        static const char *rn[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
        int ri = -1;
        for (int i = 0; i < 8; i++) if (!strcmp(key, rn[i])) ri = i;
        if (!strcmp(key, "fn")) call.fn = (uint32_t)strtoul(v, NULL, 16);
        else if (!strcmp(key, "this")) { call.reg[FC_ECX] = fc_eval(k, v); call.regmask |= 1u << FC_ECX; }
        else if (ri >= 0 && ri != FC_ESP_) { call.reg[ri] = fc_eval(k, v); call.regmask |= 1u << ri; }
        else if (!strcmp(key, "ret")) call.ret = fc_eval(k, v);
        else if (!strcmp(key, "cmp")) {           /* scratch registers compared too: cmp=edx,ecx */
            if (strstr(v, "edx")) call.cmp |= 1u << FC_EDX;
            if (strstr(v, "ecx")) call.cmp |= 1u << FC_ECX;
        }
        else if (!strcmp(key, "expect")) call.expect = strdup(v);
        else if (!strcmp(key, "label")) snprintf(label, sizeof label, "%s", v);
        else if (!strcmp(key, "env")) putenv(strdup(v));     /* (in the case's process: its children inherit it) */
        else if (!strcmp(key, "fpu")) {
            for (char *t = strtok(v, ","); t && call.nfpu < 8; t = strtok(NULL, ","))
                call.fpu[call.nfpu++] = strtold(t, NULL);
        } else if (!strcmp(key, "args")) {
            char *s = v;
            while (*s && call.nargs < 24) {
                char *e = s;
                int qq = 0, bb = 0;
                while (*e && (qq || bb || *e != ',')) {
                    if (*e == '"') qq = !qq;
                    else if (!qq && *e == '[') bb++;
                    else if (!qq && *e == ']') bb--;
                    e++;
                }
                char save = *e;
                *e = 0;
                call.args[call.nargs++] = fc_eval(k, s);
                if (!save) break;
                s = e + 1;
            }
        } else if (!strcmp(key, "w32") || !strcmp(key, "w8") || !strcmp(key, "wf")) {
            char *colon = NULL;
            int bb = 0, qq = 0;
            for (char *s2 = v; *s2; s2++) {
                if (*s2 == '"') qq = !qq;
                else if (!qq && *s2 == '[') bb++;
                else if (!qq && *s2 == ']') bb--;
                else if (!qq && !bb && *s2 == '=') { colon = s2; break; }
            }
            if (!colon || nw == 32) fatal("fncheck: %s=ADDRESS=VALUE expected", key);
            *colon = 0;
            writes[nw].a = fc_eval(k, v);
            if (!strcmp(key, "wf")) { float f = strtof(colon + 1, NULL); memcpy(&writes[nw].v, &f, 4); }
            else writes[nw].v = fc_eval(k, colon + 1);
            writes[nw].size = key[1] == '8' ? 1 : 4;
            nw++;
        } else
            fatal("fncheck: call: unknown key %s", key);
    }
    if (!call.fn) fatal("fncheck: call without fn=");
    for (int i = 0; i < nw; i++) {
        if (writes[i].size == 1) W8(writes[i].a, writes[i].v);
        else W32(writes[i].a, writes[i].v);
    }
    fc_check(k, label, &call);
}

/* ------------------------------------------------------------------ G: the groups */
/* {a|b} alternatives: the line once per alternative (recursively: every combination) */
static void expand(char *line, void (*fn)(char *, const char *), const char *alts)
{
    char *o = NULL;
    int q = 0;
    for (char *s = line; *s; s++) {
        if (*s == '"') q = !q;
        if (!q && *s == '{') { o = s; break; }
    }
    if (!o) { fn(line, alts); return; }
    char *cl = strchr(o, '}');
    if (!cl) fatal("fncheck: { without }");
    char *opts = strndup(o + 1, cl - o - 1);
    for (char *t = opts, *bar; t; t = bar ? bar + 1 : NULL) {
        bar = strchr(t, '|');
        size_t tl = bar ? (size_t)(bar - t) : strlen(t);
        size_t n = (size_t)(o - line) + tl + strlen(cl + 1) + 1;
        char *nl = malloc(n);
        snprintf(nl, n, "%.*s%.*s%s", (int)(o - line), line, (int)tl, t, cl + 1);
        char a2[512];
        snprintf(a2, sizeof a2, "%s%s%.*s", alts, *alts ? "," : "", (int)tl, t);
        expand(nl, fn, a2);
        free(nl);
    }
    free(opts);
}

static void run_in_p(char *line, const char *alts)
{
    CPU *c = main_cpu;
    FcCtx k = {0};
    k.c = c;
    k.top = (c->esp - 0x100) & ~15u;
    k.stk = k.top;
    k.base = (k.top - STK_AREA) & ~(PAGE - 1);
    c_base = k.base;
    c_live_hi = c->stack_hi;
    if (k.base - DEAD - 0x10000 < c->stack_lo) fatal("fncheck: too little stack below the frame");
    c->esp = k.base;                    /* (setup calls run below the stk: area, above the dead window) */
    char *kind = line, *name = line + 5;
    while (*name == ' ') name++;
    char *rest = name;
    while (*rest && *rest != ' ') rest++;
    if (*rest) *rest++ = 0;
    snprintf(k.name, sizeof k.name, "%s%s%s%s", name, *alts ? " {" : "", alts, *alts ? "}" : "");
    k.params = rest;
    if (!strncmp(kind, "call", 4)) {
        call_line(&k, rest);
        return;
    }
    for (const FcCase *f = __start_fncheck_cases; f < __stop_fncheck_cases; f++)
        if (!strcmp(f->name, name)) {
            const char *env = fc_param(&k, "env", NULL);
            if (env) putenv(strdup(env));
            f->fn(&k);
            return;
        }
    out("ERROR  %s: no such case (FNCASE in runtime/fncheck_cases/)\n", name);
    totals->error++;
}

static void run_line(char *line, const char *alts)
{
    char *copy = strdup(line);
    fds_save(&fds_g);
    totals->in_check = 0;
    totals->since = now_s();
    int tid0 = (int)gettid();
    pid_t pid = fork();
    if (pid < 0) fatal("fncheck: fork: %s", strerror(errno));
    if (pid == 0) {
        sync_fork_child(tid0);
        role = 1;
        fncheck_solo = 1;
        run_in_p(copy, alts);
        _exit(0);
    }
    free(copy);
    int ws = 0;
    struct timespec ts = {0, 20 * 1000 * 1000};
    double waited = 0, limit = timeout_s * 40.0 + 60;
    while (waitpid(pid, &ws, WNOHANG) == 0) {
        nanosleep(&ts, NULL);
        waited += 0.02;
        /* a setup that never reaches a check (a constructor waiting for the loading thread: the
           case's process has only the main thread) is killed after setup_s; the whole case after limit */
        int setup_hung = !totals->in_check && now_s() - totals->since > setup_s;
        if (setup_hung || waited > limit) {
            kill(pid, SIGKILL);
            waitpid(pid, &ws, 0);
            if (setup_hung)
                out("ERROR  %s: the case's setup ran %d s without reaching a check (killed; a call waiting "
                    "for another thread?)\n", line, setup_s);
            else
                out("ERROR  %s: the case process timed out\n", line);
            totals->error++;
            fds_restore(&fds_g);
            return;
        }
    }
    fds_restore(&fds_g);
    if (!WIFEXITED(ws) || WEXITSTATUS(ws)) {
        out("ERROR  %s: the case process ended (%s %d)\n", line, WIFSIGNALED(ws) ? "signal" : "exit",
            WIFSIGNALED(ws) ? WTERMSIG(ws) : WEXITSTATUS(ws));
        totals->error++;
    }
}

void fncheck_frame(unsigned long frame)
{
    if (!ngroups || role || cur_group >= ngroups || frame != groups[cur_group].frame) return;
    Group *g = &groups[cur_group++];
    out("fncheck: frame %lu, %d line%s\n", frame, g->n, g->n == 1 ? "" : "s");
    fflush(NULL);                       /* (the case processes must not write the game's buffers again) */
    det_watch_quiet = 1;                /* (the turn stands still while the cases run: sync.c's watch) */
    uint32_t keep_esp = main_cpu->esp;
    for (int i = 0; i < g->n; i++) {
        uint32_t save = main_cpu->esp;
        expand(lines[g->first + i], run_line, "");
        main_cpu->esp = save;
    }
    main_cpu->esp = keep_esp;
    det_watch_quiet = 0;
    if (cur_group == ngroups) {
        out("fncheck: %d checks: %d same, %d differ, %d errors\n", totals->checks, totals->same, totals->differ,
            totals->error);
        fflush(NULL);
        _exit(totals->differ || totals->error ? 1 : 0);
    }
}
#endif
