/* The switch for the rewritten functions and the stack fixes (rewrite.h). Built into a port only
   when it has rewrites (its REWRITES list): rewrite.h includes the generated funcs.h. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "rewrite.h"
#include "options.h"

/* is hex address a in the comma list s ("all" matches every one) */
static int in_list(const char *s, uint32_t a)
{
    if (!s) return 0;
    if (!strcasecmp(s, "all")) return 1;
    while (*s) {
        char *q;
        unsigned long v = strtoul(s, &q, 16);
        if (q == s) break;
        if (v == a) return 1;
        while (*q == ',' || *q == ' ') q++;
        s = q;
    }
    return 0;
}

/* A port's named sets of rewrites (L.A. Rush's "core": the frame and the game-state machine, the
   reference for its two-player A/B runs): the rewrites a name keeps on, as a hex comma list, or
   NULL for a name the port doesn't have. */
__attribute__((weak)) const char *port_rewrite_subset(const char *name)
{
    (void)name;
    return NULL;
}

/* option decomp = on | off | a port's subset name (only those on); <PORT_ENV>_DECOMP_OFF=addr,addr,...
   (hex) or all, switched off on top */
int decomp_enabled(uint32_t fn)
{
    static int all = -1;
    static const char *only;
    if (all < 0) {
        const char *v = option_str("decomp", "on");
        all = strcasecmp(v, "off") != 0;
        if (all && strcasecmp(v, "on")) {
            only = port_rewrite_subset(v);
            if (!only)
                fatal("option decomp = %s: the port has no such set of rewrites (on, off or one of its own)", v);
            LOG(LOG_ALWAYS, "rewrites: only the set \"%s\" (%s)", v, only);
        }
    }
    return all && (!only || in_list(only, fn)) && !in_list(port_getenv("DECOMP_OFF"), fn);
}

void stackfix_clear(CPU *c, uint32_t size)
{
    memset(gptr(c->esp - size), 0, size);
}

extern const DecompEntry __start_decomp_table[] __attribute__((weak)), __stop_decomp_table[] __attribute__((weak));
extern const StackFixEntry __start_stackfix_table[] __attribute__((weak)), __stop_stackfix_table[] __attribute__((weak));

/* hook chains' inner pointers a port registered (rewrite.h hook_inner_register) */
static DecompHook **inners;
static int ninners;
void hook_inner_register(DecompHook *inner)
{
    inners = realloc(inners, (size_t)(ninners + 1) * sizeof *inners);
    if (!inners)
        fatal("out of memory");
    inners[ninners++] = inner;
}
DecompHook **hook_inner_list(int *n)
{
    *n = ninners;
    return inners;
}

/* the rewrite fn out of d's hook chain: the hook itself, a stack fix's inner pointer or a registered
   one that holds it (for a forked check child: fncheck.c); returns how many places held it */
int rewrite_switch_off(const DecompEntry *d, int (*fn)(CPU *))
{
    int n = 0;
    if (*d->hook == fn) { *d->hook = NULL; n++; }
    for (const StackFixEntry *s = __start_stackfix_table; s < __stop_stackfix_table; s++)
        if (*s->inner == fn) { *s->inner = NULL; n++; }
    for (int i = 0; i < ninners; i++)
        if (*inners[i] == fn) { *inners[i] = NULL; n++; }
    return n;
}

/* the register audit (audit.c): with <PORT_ENV>_AUDIT=1 each rewrite is installed behind a wrapper */
int (*rewrite_audit_wrap(const DecompEntry *d))(CPU *);

void rewrite_install(void)
{
    int n = 0, nf = 0;
    for (const DecompEntry *d = __start_decomp_table; d < __stop_decomp_table; d++)
        if (decomp_enabled(d->addr)) {
            *d->hook = rewrite_audit_wrap(d);
            n++;
        }
    /* after the rewrites: each fix runs in front of whatever its hook holds */
    for (const StackFixEntry *s = __start_stackfix_table; s < __stop_stackfix_table; s++)
        if (!in_list(port_getenv("STACKFIX_OFF"), (uint32_t)strtoul(s->addr, NULL, 16))) {
            *s->inner = *s->hook;
            *s->hook = s->fix;
            nf++;
        }
    if (n || nf)
        LOG(LOG_ALWAYS, "rewrite: %d rewritten functions in, %d stack fixes", n, nf);
}
