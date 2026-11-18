# Rewriting functions as hand-written C

A port starts as translated code: every function of the game turned into C mechanically. A
**rewrite** replaces one translated function with readable C written by hand, over the game's
own memory, that behaves identically. Done function by function, it turns a translation into a
decompilation that people can read and change. It's also how a port removes overhead where a
profile points, or opens up a function an enhancement needs.

The bar is identical behaviour: the same memory, frame by frame, with the rewrites on as with
them off. The **A/B check** (tools/rewrite/ab.sh) proves it by running the game twice,
deterministically, and comparing memory hashes.

The method comes from three ports that rewrote hundreds of functions this way. It is written up
here without any game's code.

## The pieces

| Piece | What it does |
| --- | --- |
| `REWRITES = 'rewrites.txt'` in the build config | The list of functions being rewritten, one hex address a line (`#` comments), relative to the config. The translator gives each one a hook, `hook_dc_<address>` (generated `funcs.h`; lower-case hex, no leading zeros), run at the function's entry. A function can't also have another hook in `HOOKS`. Adding an address means translating again. |
| `runtime/rewrite/rewrite.h` | `DECOMP(address, fn)` registers a rewrite, at file scope, in any file of the port. Also the calling helpers below. |
| `runtime/rewrite/rewrite.c` | `rewrite_install()`, which the runtime core runs at start-up after `game_init`. It switches in every registered rewrite, then every stack fix. |
| option `decomp = off`, `<PORT_ENV>_DECOMP_OFF=addr,addr` or `all` | Leaves rewrites out: all of them, or the listed ones. This is how the A/B check runs the translation, and how a difference is bisected. |
| option `decomp = NAME` | Only the rewrites of one of the port's named sets: `const char *port_rewrite_subset(const char *name)` returns their addresses as a hex comma list (NULL: no such set). `_DECOMP_OFF` still applies on top. |
| `STACKFIX(address, bytes, hook)` | Clears a leaking function's dead stack on entry, in both modes (see Stack leftovers). `<PORT_ENV>_STACKFIX_OFF=addr,...` leaves fixes out, to see what one changes. |
| `runtime/rewrite/coverage.c` | Coverage builds: every translated function a run enters, written at exit to the file `<PORT_ENV>_COVERAGE` names. |
| `tools/rewrite/ab.sh` | The A/B check; with `--fncheck SPEC` the per-function check. |
| `runtime/rewrite/fncheck.c`, `fncheck.h` | The per-function check (`--fncheck SPEC`): one function, translation against rewrite, forked from one state. See Checking a function no route reaches. |
| `tools/rewrite/fncheck_all.sh` | Every spec on each of its routes, with totals. |
| `runtime/rewrite/audit.c` | The register audit, `<PORT_ENV>_AUDIT=1`: every rewrite's registers against the translation's on sampled calls of an ordinary run. |
| `runtime/rewrite/orig_frame.h` | Calls from the original's stack depth with its return addresses, its exception record, MSVC 6's deleting destructors and inlined `std::string` (Calls at the original's depth). |
| `runtime/rewrite/eh_owner.h` | `EH_OWNER`: a rewrite of a function with catch blocks (C++ exceptions). |
| `INNER_HOOKS = True` in the build config | A rewritten function the port also hooks gets its rewrite inside the port's hook ([BUILD-CONFIG.md](BUILD-CONFIG.md)). |
| `tools/rewrite/status.py` | How far the rewrite has got: rewrites per file, the total against the list, `--todo` with sizes. `--nm BINARY` counts what the build registers; LIST `-` with `--range LO HI` counts against the game's own functions (GEN's extents) instead of the REWRITES list; `--skip FILE` leaves addresses out (funclets), counted as "rewritten besides". |
| `tools/rewrite/classes.py`, `modules.py` | The program's C++ classes from its RTTI, and a map of the program into modules, with what a route reaches, for picking batches. |
| `tools/rewrite/disfn.sh` | One function's instructions, as discovery bounded it. |
| `tools/rewrite/bisect.sh` | Which rewrite makes an A/B route differ (halving the switched-off rewrites). |

A port with rewrites compiles `runtime/rewrite/*.c` with `-Iruntime/rewrite`. The rewrite files
build with `-ffp-contract=off`, so no floating-point operation is fused. The stub game
(`tests/stubgame/port/rewrite.c`) is a small complete example.

Since v0.2.1 the translation keeps x87 maths on the x87, rounding under the game's control word
(single precision, the game's rounding mode). Rewrites must too, or an A/B check differs where the
original rounds down and plain C's SSE rounds to nearest: compile `runtime/rewrite/*.c` and the
port's rewrite files with `-mfpmath=387 -fexcess-precision=standard` as well. Both are needed: the
second makes casts and assignments to `float` round as the original's stores do [verified: Enter
the Matrix's A/B differed at frame 2290 without the first, and in a path-curve rewrite's drawing
without the second].

That is for a game built for the x87. A game built with /arch:SSE does its float maths in SSE under
its own MXCSR (L.A. Rush sets truncating rounding there), and the translation does the same: its
rewrites stay SSE, without `-mfpmath=387`, and keep `X87()` on what the original rounds through
the x87 [verified by larush-native: 21 of 21 offline checks differed at -mfpmath=387, X87() on or
off; 21 of 21 the same as SSE with X87() on]. A game that mixes both takes the flag per file, as the
rewritten functions do.

## Writing one

A rewrite has the form `int fn(CPU *c)`. It runs with the CPU record as at the original's entry:

- `c->esp` points at the return address, and the stack arguments are above it (`ARG(0)` is the first).
- `ecx` holds the object for a method (thiscall).

It does the whole call and returns 1 through `decomp_ret(c, entry, eax, npop)`. `npop` is the
original's `ret N` divided by 4 (0 for cdecl). A float result goes on the x87 stack with
`decomp_ret_st0`.

rewrite.h also has `Saved` / `save_regs` / `restore_regs` (the caller's callee-saved registers, kept
and put back), `EARG(n)` (an argument read from the entry `E` after c->esp moved), `x87()` and
`fcom_ax` (Floats below), and `DECOMP_SF(address, fn)` for a function whose only hook is a stack
fix's (`hook_sf_<address>`). DECOMP and DECOMP_SF name their entry `decomp_entry_<address>`, so
`nm` on the objects counts what is registered.

```c
#include "rewrite.h"
#define ROTATE 0x00401200u          /* the translated helper it calls (named, never a bare address) */

/* what this function does, in the game's terms */
static int mix(CPU *c)
{
    uint32_t entry = c->esp;
    uint32_t a = ARG(0), b = ARG(1);          /* read every ARG before the first call or dc_frame */
    uint32_t h = a * 0x9e3779b1u;
    for (uint32_t i = 0; i < 8; i++)
        h = game_call(c, ROTATE, 2, h ^ b, i + 5) + i;
    return decomp_ret(c, entry, h, 0);
}
DECOMP(401300, mix);
```

### Calling other code

Translated code, an import, or another rewritten function (through its hook, so `DECOMP_OFF`
can still switch each off) is called through these:

- `game_call(c, FN, nargs, args...)`
- `game_callm(c, FN, object, nargs, ...)` for a method
- `game_call_st0` and `game_callm_st0` when the result is a float on the x87 stack
- `game_call_at(c, ret, ...)` when the callee looks at its return address (it passes the original's)

A COM method is called through the object's vtable, as the original does. COM passes `this` on the stack:

```c
game_call(c, R32(R32(obj) + offset), n + 1, obj, ...)
```

### The stack

A rewrite keeps none of the original's stack layout. It calls at the current stack pointer:

- Locals that a callee is handed live in guest memory. Get them with `uint32_t F = dc_frame(c, size)`
  and call `dc_unframe(c, F, size)` before returning. Size the frame to the original's whole
  local area wherever a callee's buffer sits at its top.
- Locals no callee sees are ordinary C locals.
- `local_call` calls a rewrite as C, for a helper that has no hook of its own.

### Registers

- Callee-saved registers (ebx, esi, edi, ebp) must hold the caller's values when you return.
- Hidden register arguments are set before the call, as the original does.

## Reading the original

Get the function's instructions with `disfn.sh`. Use a decompiler's output (Ghidra, REA) for the
shape only. Where the two disagree, the listing wins. Watch for:

- **Hidden register arguments.** A decompiled callee's `in_EAX` or `unaff_EDI` means it reads that
  register. The callee's first instructions tell you.
- **x87 maths.** Do it as `f80` (long double), in the same operations and the same order. It
  runs with the game's control word, so results round as the original's. Round to `float` where
  the original stores a dword.
  - Where the original multiplies by -1, use `mul_neg1(x)`; GCC's sign flip changes a NaN's sign.
  - `_ftol` pops only its own copy.
- **NaN.** Keep the original's operand order, even for commutative operations. Copy floats as
  integers where the original only moves them with `fld`/`fstp`: a signalling NaN stays signalling.
- **x87 compares.** For `fcomp` / `fnstsw ax` / `test ah, 0x41`, work out the flags case by case:
  C3 = equal, C0 = below, C2 = unordered. Unordered results go where the original's flags send them.
- **Integer compares.** `jl`/`jg` are signed; `jb`/`ja` are unsigned.
- **Memory read twice.** Unoptimised code re-reads globals and fields after every call, so read
  them again at the same point: a callee may have changed them.
- **eax.** After a void function, leave eax as the original leaves it. A callee that sets only
  `al` hands back the rest of the caller's eax.
- **Writable data isn't a constant.** Read values in `.data` from memory; only `.rdata` is constant.
- **ARG(n) is the argument's value.** A float argument is `RF32(entry + 4 + 4*n)` (or ARG(n)'s
  bits), never `RF32(ARG(n))`.
- **ARG after lowering esp.** Once you have lowered `c->esp` (dc_frame, or pushes for a call),
  `ARG(n)` reads the wrong slot. Read every argument before that.
- **Argument evaluation order.** `game_callm` sets ecx before its arguments are evaluated, so an
  argument that itself calls the game must be computed into a local first.
- **Import stubs.** Where the exe calls a DLL through a one-jump stub (`jmp [IAT]`), the
  translation calls the DLL's function directly. Call it by its address in that module.
- **Data imports** (MSVCP60's `npos`, for example) are read through the import table: `R32(R32(IAT))`.
- **Structs passed by value.** Copy them to the stack as the original pushes them, and pop what
  the callee pops.
- **vtordisp thunks** (`sub ecx, [ecx-4]; jmp body`) jump into bodies that aren't entries.
  Write it as: `c->esp = entry + 4`, then `game_call_at(c, R32(entry), body, 0)`.
- **No-CD and other patches** in the player's exe: transcribe the bytes as they are.

### Floats

- Only products and quotients that land in float's denormal range (below 1.18e-38) differ between
  the x87 at 24-bit precision and SSE: sums and differences of two floats are exact either way, and
  powers of two are exact. A check that is to see a float site's rounding needs operands whose
  product or quotient double-rounds there; search for them with a small program that computes both
  [verified: one port's x87 sweep, 2026-11-12].
- `x87(a * b)` (rewrite.h) keeps one operation on the x87 whatever the compiler flags: GCC otherwise
  folds `(float)((f80)a * (f80)b)` of two floats into one `mulss`.
- A helper that rounds through the x87 quiets a signalling NaN even on a pure copy: copy floats as
  dwords where the original only moves them.
- After `fcom` / `fnstsw ax` the original's eax is the status word, which holds the x87 stack top:
  `fcom_ax(c, a, b, depth)` leaves eax as it does, `depth` the values still on its x87 stack at that
  point. A void function whose last eax write was that fnstsw returns it.

A regression check for the translator's x87 rounding stays port-side, because it needs a
game's functions: keep a few fncheck cases, commented out, that should be `same` on a translation
made with v0.2.1 or later and DIFFER on an older one (where the translation folded to SSE too):
a matrix-from-angles or quaternion-to-matrix function given inputs whose products land in float's
denormal range (one port's: a sine times a cosine of -4.4e-39, x87 0x802f98f8 against SSE
0x802f98f7). Live cases of the same kind (a product or quotient near 1.2e-38) prove the rewrites'
`x87()` sites.

## Writing it well

- **Name things.** Make globals and fields `#define`s with a comment saying what they are and how
  that's known. Unknown ones keep their address in the name (`OBJ_0x88`), not a guess. Mark
  claims [verified] with the evidence, or [guess].
- **Comment.** Give each file and each function a header comment saying what it does in the game's terms.
- **Write C, not assembly.** Don't copy the original's register shuffling or loop forms when C
  says the same thing. Keep its order of memory accesses and calls.
- **Types.** Use `uint32_t`/`int32_t` with the original's signedness, `R8/R16/R32/RF32` and
  `W8/W16/W32/WF32` for memory.
- **Shared names.** Check a name isn't already defined elsewhere in the port with another value.

## Checking

Run tools/rewrite/ab.sh after every few functions. It must print `same`.

```bash
tools/rewrite/ab.sh --env MYGAME --out work/ab --frames 3000 --every 10 \
    --id gen/funcs.h --id runtime/stackfix.c --lock work/ab.lock -- ./build/mygame
```

It runs the game twice under `--test`, headless (with no display, SDL's offscreen driver: a
machine over ssh), one check at a time under a lock (default: `work/ab.lock` in the main checkout
of the git repository, shared by its worktrees):

- **A** is the translation (`<PORT_ENV>_DECOMP_OFF=all`). It depends only on the files given
  with `--id`, the command and the route's environment, so it's saved under that id
  (`OUT/a-<id>/`, the last four kept) and reused while those are the same (`--fresh` runs it
  again). List the generated `funcs.h`, the runtime outside the rewrites, and the stack fixes.
- **B** has the rewrites on. Anything the caller set in `<PORT_ENV>_DECOMP_OFF` stays, so you
  can bisect: `MYGAME_DECOMP_OFF=4a1920,4a2000 tools/rewrite/ab.sh ...`.

The route's scripted input goes in the environment, as the port reads it (`MYGAME_KEYS`,
`MYGAME_MOUSE`, `MYGAME_PAD`...). It's passed to both runs and is part of A's identity. A
prepared save goes in with `--seed DIR`, copied into each run's fresh user folder.

The memory hash covers the program's memory without the stacks, every `--every` frames; the
hashes are running ones, so the frames between are covered too.

If the runs differ:

1. Bisect with `DECOMP_OFF`.
2. Dump memory at the first differing frame in both modes (`--dump-memory FRAME FILE`).
3. Compare the dumps with `translator/memdiff.py A B`.
4. Find who writes the differing address: a gdb write watchpoint on it, with a backtrace of each hit.

### What the check can't see

- **Sensitivity.** "same" only covers what the route reaches. Before trusting it for a path,
  break the rewrite on purpose and see the check fail. Keep a route for each part of the game,
  and note in the rewrite which branches no route reaches.
- **Stack objects aren't hashed.** A local of the main loop (often the whole front end or the
  race) lives on the stack, so its fields aren't compared. Check those some other way.
- **Texture lock buffers aren't hashed.** Check pixels written into them some other way.
- **How each run ended.** Code after the last hash (the exit's destructors) shows only in how the
  run ended: ab.sh compares the last "program exited with code N" or FATAL line of both logs and
  says "differ at exit" when they part [verified: one port, a rewrite crashing in an exit-time
  destructor].
- **The memory dump leaves out DLL images.** The hash covers them, so dump them too when the
  difference is there.
- **One graphics driver.** A rewrite that reads the wrong field can agree with the translation
  under one driver, because what sits there happens to be equal, and not under another. Run the
  routes under a second driver too (lavapipe, `--vk-software`, or an AMD card). One port's rewrite
  tested a byte at another offset than the original: same on every route under NVIDIA, different
  on two under lavapipe. `tools/rewrite/bisect.sh --nm BINARY ab.sh-arguments --
  GAME_COMMAND` then names the rewrite a differing route needs, switching the build's rewrites off
  from the front by halving (about log2(rewrites) + 2 runs).

## Checking a function no route reaches

A/B routes never reach everything: error paths, rarely taken branches, whole classes no route
creates. The per-function check (`runtime/rewrite/fncheck.c`, first used on a port rewritten to the
end, where it proved the last few percent of its rewrites) proves those, and finds what memory
hashes can't: registers left behind, argument slots used as scratch, dead-stack contents.

```bash
tools/rewrite/ab.sh --env MYGAME --out work/fc --fncheck tests/fncheck/menus.spec -- ./build/mygame
```

The route runs once (rewrites as B has them) to the frames the spec names. There the process forks
once per spec line (the case's process), which builds the inputs, then twice per check from that
state: one child runs the translation (the rewrite taken out of its hook chain; stack fixes and
other rewrites stay), the other the rewrite. Each runs the one call to its return, records what it
did and waits; the two are compared, and every check starts again from the route's state. The
report is `OUT/fncheck.txt`, one line per check: `same`, `DIFFER` (with the reason, both children's
ends and the differing bytes, each with its heap block and first dword) or `ERROR` (a timeout, a
case that failed to build its inputs). The run's exit code is 0 only if every check was the same.

**What is compared**: every page of memory the A/B hash covers (the image, the translated DLLs, every
region but the thread stacks) and the caller's stack above the call; how the call ended (returned,
threw, fatal error or crash, timed out) and the return address it popped; esp (the arguments
popped), eax, ebx, esi, edi, ebp, the direction flag, the x87 control word, depth and every value
pushed, bit for bit; fs:[0]; the call's own argument slots and return-address slot as it left them;
the runtime calls made (Windows / DirectX stand-ins: their number, arguments, a stack pointer counted
as "a stack address", and results); a C++ throw (its code, type and the thrown object's bytes, with
memory as at the throw). ecx and edx are only noted unless the line says `cmp=edx`; the dead stack
below the call is reported, not compared.

**Dead-stack fills** (`fill keep addr zero`; default `keep addr`): each check runs with the 64 KB of
dead stack below the call as the state had it, and filled with a pattern that depends on each
dword's address. Under `addr`, a read of a never-written stack byte depends on the reading frame's
depth, so a rewrite that calls a leftover-reading callee at another depth shows as a difference.
"the translation reads dead stack: fill keep against addr changes N pages" means the original itself
reads never-written stack: the rewrite must read the same bytes at the same depth, or the function
needs a stack fix.

**The spec** (one directive or case per line, `#` comments):

| Line | Meaning |
| --- | --- |
| `frame N` | The lines below run when game frame N has been presented (several groups, in frame order); `frame 0` runs just before the program's entry point, for functions that need nothing it sets up. After the last group the program exits. |
| `timeout S` | Seconds a child may take (default 20). |
| `setup S` | Seconds a case's process may run without reaching a check (default 60), then it's an ERROR. |
| `fill MODE...` | The dead-stack fills (default `keep addr`). |
| `maxdiff N` | Differing byte ranges listed per check (default 16). |
| `call NAME fn=ADDR [this=E] [args=E,E,...] [eax=E] [ecx=E] [edx=E] [ebx=E] [esi=E] [edi=E] [ebp=E] [fpu=F,F] [ret=ADDR] [expect=return\|throw\|throw:TYPE] [cmp=edx,ecx] [w32=E=E] [w8=E=E] [wf=E=F] [env=VAR=VALUE] [label=TEXT]` | One check, its inputs in place: the w32 / w8 / wf writes are made first. `expect` is checked on the translation. |
| `case NAME [key=value...]` | A C case (`FNCASE` in a port file) with its parameters. |
| `# routes: a b` | The routes `fncheck_all.sh` runs the spec on. |

`{a|b|c}` anywhere in a line runs it once per alternative (several: every combination).
Expressions: numbers (`0x1f`, `31`, `-4`), `f:1.5` (a float's bits), `[E]` (the dword at E), `E+N`,
`E-N`, `obj:VTABLE[#n]` (the n-th heap object with that vtable), `any:VTABLE[#n]` (anywhere: members,
.data), `stack:VTABLE[#n]` (on the main thread's stack: a game loop's locals), `str:TEXT` or
`str:"TEXT"` (a new C string), `sstr:...` (a new MSVCP60 std::string), `new:N` (a zeroed heap block),
`stk:N` (zeroed bytes on the caller's stack), `fv:1;0.5;nan` / `dw:1;0x10` (a new block of floats /
dwords), and any name the port gives with `FCNAME`.

**The port's side** (fncheck.h): C cases build objects with the game's own constructors and
loaders (`game_call` in the case's process, so both children start from what they built) and call
`fc_check` once per input set; `fc_find` finds objects by vtable; `fc_host_patch` makes a runtime
stand-in answer otherwise (a refused API call, for an error path) in the case's process.
`FCNAME(name, about)` gives the spec a name (a game context pointer); `PORT_FNCHECK_NEW` (the game's
operator new) and `PORT_FNCHECK_STRING_ASSIGN` in port.h make `new:` blocks the game can free and
`sstr:` strings; `hook_inner_register` tells the check about a port hook's inner pointer that may
hold a rewrite. `<PORT_ENV>_FNCHECK_OUT` is where the report goes (ab.sh sets it).

For each rewritten function no route reaches:

1. Pick a state where its inputs can exist (a front-end frame, a race frame).
2. Write its inputs: plain ones as a `call` line, objects in a C case.
3. Sweep its branches, error paths included (a throw is compared like a return).
4. **Prove the case is sensitive**: a deliberate bug in the branch (a bug switched by an environment
   variable, `env=` on the line, or a copy of the binary) must give DIFFER. Two traps:
   *equivalent mutants*, a bug the input can't show (replacing a minimum by an equal value, a scale
   on a zero quaternion), and *blind data*, copying 0 over 0. Pick a bug that changes a stored value
   and make the case's data non-uniform. A check whose "pages changed" is 0 checks nothing in memory.
5. Keep the spec and the cases with the rewrite, so the check can be run again after a tidy.

`tools/rewrite/fncheck_all.sh --run tests/ab.sh` runs every spec on each route its `# routes:` line
names (the port's route script takes `--route NAME --out DIR --fncheck SPEC`) and prints totals.
The stub game's test has a worked example (tests/stubgame/fncheck.spec, port/rewrite.c).

**Limits.**
- A child has only the thread that forked: a function that waits for another thread, presents a
  frame or loops on input times out; so does a case's setup that waits for a loading thread.
- Calls into the window system or the DirectX stand-ins run on the parent's devices: fine for state
  calls, a hazard for creating resources, and a child can hang or differ for reasons that aren't
  the function (a sizeable share of functions in one port's audit). Host-side state (texture and buffer
  contents behind locks, the renderer's state, sound buffers, files written) isn't compared: a
  function that writes a file writes it twice.
- Forked children crash under the NVIDIA driver: run a game that renders under lavapipe (Mesa's
  software Vulkan) for checks.
- A function checked on its own may read leftovers its route's caller hides, and a depth mistake
  shows only where a callee reads stack nobody wrote since the call: build the state the original
  hit (an empty list's first add) to reach it.

## The register audit

`<PORT_ENV>_AUDIT=1` puts every rewrite behind a wrapper (runtime/rewrite/audit.c). On sampled calls
of an ordinary run (each rewrite's first six, the first two from each return address, then 1, 2, 4,
8... up to 60) it forks; the child runs the translation from the same state, the parent then the
rewrite, and eax, ebx, esi, edi, ebp, esp, the return address, the direction flag and the x87 stack
are compared. Differences go to `<PORT_ENV>_AUDIT_OUT` (default `audit.txt` in the user folder),
one line each (`ADDR DIFF ... eax T=translation R=rewrite`); `<PORT_ENV>_AUDIT_ONLY=addr,...` limits
it. Run it on every route: it makes "does my rewrite leave the registers as the original" a route
run with no spec.

In one port it found well over twice as many rewrites that returned the right answer but left eax
otherwise as a static scan had, and no route showed any of them: fnstsw status words, `mov al` over a
stale eax, the last pointer or count through eax, an exception prologue's old fs:[0], MSVCRT's
free(0) returning the fs:[0] it read [verified: that port's eax sweep, 2026-11-12/13]. Such bugs surface
months later, when a caller reads the register by accident.

The child runs `F_<addr>_orig` when the port links `-rdynamic` (found by name), else `F_<addr>` with
the rewrite answering "not done": the same, except that a port hook outside the rewrite
(`INNER_HOOKS`) runs a second time in the child. A child that calls through a pointer into the
runtime (Direct3D, sound, network) ends without a result (`NOCHILD why=thunk`) and that rewrite isn't
audited again. A child asleep in the kernel with no call made for 0.2 s (waiting on a lock or event
that only a thread the fork didn't copy would release) is ended then, as `NOCHILD why=blocked:futex
progress=N`, instead of after the full two seconds; one still working at 2 s is `why=timeout
progress=N` (N: its calls through wrappers and pointers, so a recursion that kept going shows a large
N). Not together with `--fncheck`. As with fncheck, run under lavapipe where the driver can't fork.

**Argument slots.** `<PORT_ENV>_AUDIT_SLOTS=1` also compares the call's own argument slots as the
function leaves them (MSVC uses them as scratch; fncheck compares them too): each slot the call popped
(a callee-pop function's own, up to 12), a cdecl function's first 6, and the return address slot.
A difference reads ` slot1 T=.. R=.. (in ..)` (` retslot` for the return address slot).

**Leaving functions out.** `<PORT_ENV>_AUDIT_SKIP=addr,...` (hex), and the port's own list as
`#define PORT_AUDIT_SKIP "401230,4015a0"` in port.h, are never audited; nor is the program's entry
point, nor a call of a function already running further up the same thread's stack (a re-entered
window procedure, a recursion's inner calls). Put WinMain and the main loop in the skip list: they
never return, so a fork of either runs the game's start-up until its two seconds are up, and it
can't be known that a function won't return until a child of it has timed out once.

**An audited run stays its own run.** The forked child writes nothing the parent can see: in the
child, standard output and error and every file open for writing only (the log, hash files) are
/dev/null, and a seccomp filter ends it (`NOCHILD why=sys:NAME`) at the first system call that would
change a file (an open for writing, unlink, rename, mkdir, truncate, a shared writable file mapping),
do I/O on a descriptor it shares with the parent (sockets, pipes, devices, eventfds: the window
system's connection, sound; a file open read-write) or signal another process. Files open read-only
are read as before, their offsets put back afterwards. So the child neither writes the user folder
(one port's start-up children wrote its settings file before the parent asked for it) nor
puts its log lines in the parent's log. `<PORT_ENV>_AUDIT_ISOLATE=0` switches the filter off; where it
can't be installed (no seccomp: a sandbox that forbids it, a host other than x86-64 or arm64 Linux)
the report says so once and children run as before. What stays: the parent's run is still slowed by
each fork and wait, so its timing differs (nothing in a `--test` run reads real time), and the
rewrites run behind a wrapper. Compare an audited run's own lines, not an A/B verdict against it.

**The two-run log** (no fork). `<PORT_ENV>_AUDIT_LOG=PREFIX` records every call of every rewrite as it
returns, in an ordinary run: in a run with `<PORT_ENV>_DECOMP_OFF=all` each wrapper runs its function's
translation and the record goes to `PREFIX.a.txt`; in a run of the rewrites to `PREFIX.b.txt`. With the
variable set for both of `tools/rewrite/ab.sh`'s runs that is one A/B route; then
`tools/rewrite/auditlog.py PREFIX` compares (exit 1 on a DIFF):

    4010a0 DIFF call 0: ebx T== R=c0ffee     call 0 left ebx otherwise, the same eax/ecx/edx coming in
    4010a0 SLOTS call 0: slot1 T== R=.. (one run only)   only argument slots differ: counted apart
    4010a0 COUNT T n=12 R n=10               called a different number of times
    auditlog: 1 functions: 0 same, 1 differ, 0 differ with other inputs coming in, ...

Per function: the call count, digests of the inputs (eax, ecx, edx) and of what each call left (eax;
ebx, esi, edi, ebp kept or their new value; esp; the return address kept or not; df; the x87 values
pushed; the argument slots as above), the digests at calls 16, 32, 64..., and the first 16 calls in
full. `=` is "kept as it came in", `stk` a stack address (the two runs' frames differ). A difference
whose inputs differed too is counted (`--all` lists it as `INPUTS`): usually the effect of another
finding in a caller. A function whose registers are the same in both runs and only its argument
slots differ is a `SLOTS` line, not a `DIFF`, and doesn't fail the check: a cdecl function's first 6
slots are compared whatever its argument count, so a callee writing through a pointer into its
caller's frame (a member of a stack object: an iterator's `++`) differs whenever the two runs'
callers lay their frames out differently, which a rewritten caller always does. Each slot says
`moved` (the value is in another slot of the other run), `one run only` (kept in the other) or
`other value`; read them against the function's real argument count. This covers the functions whose forked child can't finish (`NOCHILD`: a sizeable
share in one port), and since recording only reads, the two runs are the A/B pair they would be
without it. It needs `F_<addr>_orig` by name (`-rdynamic`); `AUDIT_ONLY` and `AUDIT_SKIP` apply;
`AUDIT=1` is ignored beside it. Linux only, as the forked audit (the Windows host says so).

## Calls at the original's depth

Once fncheck and the stack hashes look at the dead stack, most rewrites must leave it as the
original does: their calls are made from the original's stack depth with the original's return
address, so the callee's pushes land where the original's did. Without that every batch needed
stack fixes. `runtime/rewrite/orig_frame.h` has the helpers, one name each (merged from eight
per-batch copies); a function using them keeps `uint32_t E = c->esp;` at entry:

- `OCALL(at, ret, fn, n, ...)` / `OCALLM(at, ret, fn, obj, n, ...)`: esp = E - at at the call
  instruction (as an annotated listing gives it); `_ST0` forms for a float result.
- `AT(sp, ...)` / `ATM` / `ATV`: a stack pointer before the pushes; `ATP` / `ATPM`: one at the call.
- `vmethod(obj, off)`, `IMP(slot)`, `tail_call(c, E, fn, npop)` (a `jmp` to another function).
- The exception record: `eh_open`, `eh_enter` (the old link left in eax), `eh_leave`, `eh_close`,
  `EH_STATE(n)`; `regs_set` for the callee-saved registers the original has at a call.
- x87: `fsw_cmp`, `of_fcmp`, `of_fcom`, `fbits`; `dwords_copy`, `vector_count`.
- With the port's `OP_DELETE` defined first: MSVC 6's scalar deleting destructors (`scalar_delete`,
  `scalar_delete_adj`); with the MSVCP60 import names (`IAT_STRING_TIDY` and the others the header
  lists): the inlined `std::string` release and constructors (`ostr_release` and its forms,
  `ostr_grow`, `ostr_assign`, `ostr_cleared`, `ostr_assign_string`, `str_clear`, `vec_clear`).

Helpers that look alike are not always the same helper: before folding two, compare what each
leaves (eax and ecx after an inlined release, eax after the exception prologue, whether a method
macro sets ecx before or after esp, whether a flag is read before or after a call).

## Stack leftovers

The original sometimes copies stack bytes it never wrote into memory: padding in a struct built
on the stack, a set or map insert's `bool` stored as a whole dword, a field a parser never fills.
Those bytes come from earlier calls' frames. A rewrite leaves different bytes there, so memory
differs with nothing wrong in the C.

Don't imitate the original's stack. Fix the leak where it happens, in both modes: add
`STACKFIX(address, bytes, hook)` in the port's stackfix file. It clears `bytes` of dead stack
below the leaking function's entry before its rewrite or translation runs, so both runs copy
zeros. The hook is `hook_dc_<address>` for a function in the list. Any other function needs a
`HOOKS` entry of its own (e.g. `sf_<address>`), which means translating again. Say in each
fix's comment what leaks and how that was found.

Leftovers turn up in many forms:

- **Values that steer behaviour.** A leftover read as data can change a branch, not just bytes.
- **Callee frames moving.** Rewriting a caller moves its callees' frames, which exposes leftovers
  inside them and in later code.
- **Shared callees.** A leftover can spread through a constructor or allocator that many callers
  share.
- **Allocator bytes.** An allocator can take bytes from a pointer's low byte or from a
  callee-saved register's saved slot.
- **Return addresses.** These are leftovers too: a rewrite that calls from another depth leaves a
  different return address in the dead stack.
- **The exception chain.** The SEH chain head (`fs:[0]`) is in hashed memory. A rewrite of a
  function with an exception frame should point `fs:[0]` where the original did, and put it back
  before returning. A thread that never returns keeps its SEH record there.
- **Destructor chains** leave the C runtime's `free` frames at fixed depths.

When a function is rewritten, it clears the same bytes itself.

## C++ exceptions

A function that registers an exception frame only so destructors run on unwind (`mov eax, fs:[0]`
/ `push __ehhandler`, a FuncInfo) can be rewritten without the registration, as long as the game
only unwinds on fatal errors. Say in its header comment which objects the original destroyed on
unwind. Where the original's free(0) or a null-destination copy constructor returns the fs:[0] it
read, the record must still be registered where the original's was: those leave it in eax.

A function with catch blocks can be rewritten too (`runtime/rewrite/eh_owner.h`):
the rewrite builds the original's frame (ebp, the SEH record, the try-level where the original
writes it, the locals a funclet reads at the original's ebp offsets), and `EH_OWNER(addr, name,
body, cont, places...)` registers it with the runtime as the owner of its continuations: when a
catch funclet returns a place to go on at, `cont(c, target)` finishes the function in C.
`ehx_open` / `ehs_open` / `ehx_close` are the C++ and `_except_handler3` prologues and the epilogue.
The translated pieces (the `__ehhandler` stubs, MSVCRT's handler, the funclets) keep running; catch
funclets can be rewritten as functions returning the continuation. A one-jump thunk to
`_except_handler3` (`jmp [IAT]`) the program calls through a pointer goes in `EXTRA_ENTRIES`.

A port hook on a function (widescreen, a controller menu) and its rewrite coexist with
`INNER_HOOKS = True` in the build config: the rewrite's hook runs inside `F_<addr>_orig`, so the
port's hook wraps the rewrite as it wrapped the translation.

## Working in batches

A whole game is rewritten in batches, each by one helper in its own git worktree.

1. Make a coverage build and run the A/B routes in it, so you know which functions they reach.
2. Run `classes.py` on the exe, then `modules.py GEN classes.txt coverage.txt`. That maps the
   program into modules and counts what the routes reach.
3. Cut batches from the reached functions of one module or a few neighbouring ones. The linker
   keeps a source file's functions together, and template instances sit right after the file that
   uses them.
4. The coordinator adds every batch's functions to the REWRITES list before the helpers start,
   so nobody has to translate again.
5. Each helper starts from a copy of the main checkout's generated code, build and saved A run.
6. Merge one branch at a time and run the A/B check on every route after each merge.
7. Fold each helper's report (names found, traps) back into the port's notes.

A helper's brief names:

- the functions and roughly how big they are
- the files to read first: this guide, a worked example, the port's notes, existing decompiles
- the bar: `same` on every route before reporting, checks one at a time under a shared lock
- what it may edit: only its new files
- what to report: the branch and last commit; what each function does; anything left to the
  translation, and why; data structures named; branches no route reaches; traps worth adding here

What made rewriting every function of a whole game tractable:

- **Batches by module** (`modules.py`), 40 to 160 functions each, one helper per batch in its own
  git worktree, merged one at a time, every merge gated by all routes and every spec.
- **Routes are the coverage lever**: add one for each part of the game a route doesn't yet reach
  (each track, a won championship, multiplayer, the options pages, a recorded network client). Test switches
  (an autopilot, a dialog answerer, a network loopback / record / replay) exist only to make routes:
  each is off by default and installs nothing when off.
- **The last few percent** no route reaches are proved with fncheck cases and their mutant proofs.
- **The register audit on every route** before a batch is called done.
- **A tidy at the end**, one pass, each step proved without a game run where it is a pure refactor:
  a normalised `objdump -dr` of every rewrite object before and after (functions sorted by name,
  addresses stripped) must be identical. Where a merged helper orders two statements differently,
  build once with the helper in the old order: then only the order is left to argue.

### Traps that cost the most

- **Leftovers are read late and far away**: a callee's never-written read can come from a sibling
  call's frame much earlier, a value in a loop is inherited from the previous iteration at the
  same depth, and an allocator byte can come from a pointer's low byte or a callee-saved register's
  pushed slot. "Only all off together match" across unrelated rewrites points at such a read.
- **Callee-saved registers leak as leftovers**: callees push them, later code reads those slots.
  Give them the values the original has at each call (`regs_set`).
- **Argument slots are scratch**: MSVC stores lengths, widget addresses and sizes in its incoming
  argument slots; the next reader of that stack sees them. fncheck compares them.
- **Stale stack pointers in heap objects**: a callee that stores a pointer into its own frame makes
  the frame's depth part of hashed memory.
- **Stack objects aren't hashed**: a game loop's locals (often the whole front end or the race) are
  on the main thread's stack. Check them with fncheck (`stack:` finds them) or a memory compare.
- **A thread that never returns keeps its SEH record in the TEB**, which is hashed.
- **Exit-time code runs after the last hash**: compare how both runs ended, and memory at exit
  (`--dump-memory-call` at the call before process_exit, both modes).
- **Template copies and look-alike STL inlines differ per instance** (one builds its end pointer in
  edx, another in eax): never fix a shared helper from one listing.
- **`game_callm` sets ecx before its arguments are evaluated**: an argument that calls something or
  reads c->ecx must be computed first; a nested call in a macro argument isn't expanded as expected.
- **A comment-only edit can change the binary** (line numbers in asserts): run the checks again.
- **Building while a check is queued** may test the new binary: check a copy.
- **Logs can contain NUL bytes**: grep them with `-a`.
- **Hashes differ between machines** from the first frames where the graphics card's device data is
  in game memory: A/B runs compare within one machine only.

Keep "how big" honest. Rewriting a whole game function by function gives the least speed for
the most work: rewrite what a profile points at, what an enhancement needs, and what someone
needs to read.
