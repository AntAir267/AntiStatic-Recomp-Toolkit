/* The stub game's rewrite test (tests/stubgame/run.sh): game.c's stub_mix rewritten by hand as C,
   the way a port rewrites a game function (docs/REWRITE.md). It calls stub_rotl, which stays
   translated, through game_call. rewrite_addrs.h is written by run.sh from the program's export
   table: the two addresses, and the registrations (DECOMP and STACKFIX need the address as a
   literal, as a port writes it: DECOMP(4a1920, fn)).

   Also the checks' test pieces: a port hook around the rewrite (INNER_HOOKS), a name and a case for
   the per-function check (fncheck.h; tests/stubgame/fncheck.spec), and two deliberate bugs switched
   by STUB_MIX_MUTANT, which the checks must catch: "1" a wrong result for an input the stub game
   itself never gives (only fncheck reaches it), "ebx" ebx left changed (the register audit),
   "slot" its second argument's slot scribbled (the audit's argument slots, AUDIT_SLOTS and
   AUDIT_LOG). */
#include <string.h>
#include "rewrite.h"
#include "fncheck.h"
#include "rewrite_addrs.h"

/* unsigned __cdecl stub_mix(unsigned a, unsigned b): a hashed with b through eight rotations */
static int mix(CPU *c)
{
    uint32_t entry = c->esp;
    uint32_t a = ARG(0), b = ARG(1);
    uint32_t h = a * 0x9e3779b1u;
    for (uint32_t i = 0; i < 8; i++)
        h = game_call(c, STUB_ROTL, 2, h ^ b, i + 5) + i;     /* (stdcall: it pops its two) */
    const char *m = port_getenv("MIX_MUTANT");
    if (m && !strcmp(m, "1") && a == 0xdeadbeefu)
        h++;                                                  /* (the bug only fncheck reaches) */
    if (m && !strcmp(m, "ebx"))
        c->ebx ^= 0x100;                                      /* (the bug only the audit sees) */
    if (m && !strcmp(m, "slot"))
        W32(entry + 8, b ^ 0x10);                             /* (b's slot: the original leaves it) */
    return decomp_ret(c, entry, h, 0);                        /* cdecl: the caller pops */
}
REGISTER_MIX(mix);
/* a stack fix on the same function: runs in front of the rewrite (or the translation) */
STACKFIX_MIX(64);

/* the port's own hook on stub_mix (HOOKS {mix: 'mix_outer'}, INNER_HOOKS = True): it runs around
   whatever F_<mix>_orig does, the rewrite (its hook_dc_ is inside) or the translation */
static int outer_calls;
static int mix_outer(CPU *c)
{
    if (outer_calls++ == 0)
        LOG(LOG_ALWAYS, "stub: the port's hook on stub_mix ran");
    MIX_ORIG(c);
    return 1;
}
void port_runtime_ready(void)
{
    hook_mix_outer = mix_outer;
}

/* the per-function check's pieces (tests/stubgame/fncheck.spec): a name its call lines use, and a
   case sweeping inputs in C */
FCNAME(mix_b, "the second input the stub game itself gives stub_mix")
{
    (void)k;
    return 0xcafef00du;
}
FNCASE(mix_sweep, "stub_mix on a sweep of inputs, one of them in a heap block")
{
    static const uint32_t as[] = {0, 1, 0x80000000u, 0xdeadbeefu, 0xffffffffu};
    uint32_t blk = fc_new(k, 8);          /* (an input read from memory the case made) */
    W32(blk, 0x0badf00du);
    for (unsigned i = 0; i < sizeof as / sizeof *as; i++) {
        char label[32];
        snprintf(label, sizeof label, "a=%08x", as[i]);
        fc_check(k, label, &(FcCall){.fn = STUB_MIX, FC_ARGS(as[i], R32(blk)), .expect = "return"});
    }
}
