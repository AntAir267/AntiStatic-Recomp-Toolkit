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

/* option decomp = on | off; <PORT_ENV>_DECOMP_OFF=addr,addr,... (hex) or all */
int decomp_enabled(uint32_t fn)
{
    static int all = -1;
    if (all < 0)
        all = strcasecmp(option_str("decomp", "on"), "off") != 0;
    return all && !in_list(port_getenv("DECOMP_OFF"), fn);
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
