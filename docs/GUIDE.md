# How to recomp a Win32 game with the AntiStatic Recomp Toolkit

This is the method the AntiStatic ports follow, from a game's `.exe` to a native Linux program.
It assumes you know C and some x86, and that you own the game.

## 1. Is the game a good fit?

AntiStatic handles **32-bit Windows programs built with a C/C++ compiler** (Microsoft Visual C++
from the late 1990s to the mid 2000s is the usual case) that talk to Windows and DirectX through
imports. Good signs:

- A PE32 `.exe` (and maybe some DLLs of its own), not packed or encrypted. Check with any PE
  viewer: section names like `.text .rdata .data`, an import table naming KERNEL32, USER32,
  DDRAW / D3D8 / D3D9, DINPUT, DSOUND, WINMM.
- No copy protection that decrypts code at run time. Remove it with the publisher's own no-CD
  patch or a GOG / digital release, or skip the check with a hook (step 6).
- Middleware DLLs (Bink, Miles) can be translated along with the game, or replaced by your own
  stand-ins.

Not a fit: 16-bit Windows or DOS programs, .NET, games whose code is mostly a scripting VM
(translate the VM, then the scripts still run as data), or code generated at run time.

## 2. Start a port

A port is its own repository with:

```
builds/<build>.py   what you know about this exact build (addresses only)
runtime/            your Windows/DirectX stand-in (the ports' runtimes for now; see the README)
gen/                generated C (never committed, never shared)
Makefile            translate, compile, link
```

Pin AntiStatic in it (a git submodule or a checkout at a tag) and keep game-specific code out of
the toolkit: hooks and fixes for one game live in the port.

## 3. Describe the build

Write `builds/<build>.py` (reference: [BUILD-CONFIG.md](BUILD-CONFIG.md)). The minimum:

```python
NAME = 'mygame-1.0'
MODULES = [dict(file='GAME.EXE', sha256='...')]
EXTRA_ENTRIES = []
NOT_ENTRIES = []
JUMP_TABLES = {}
NORETURN_IMPORTS = ['ExitProcess', 'ExitThread', 'TerminateProcess']
```

The SHA-256 ties everything you write to one exact file: a different version of the game is a
different build, with its own config.

## 4. Translate

```bash
python3 translator/recomp.py ~/games/mygame builds/mygame.py gen
```

It prints what discovery found (functions, instructions, errors per module), the jump tables it
had to guess at, and writes `gen/warnings.txt`. Outputs that didn't change keep their file times (so
only what changed recompiles); every finished run touches `gen/.translated`, which is the target a
Makefile's translate rule should name (`gen/.translated: <translator files> <build config>`), or
make translates again on every run. Work through them:

- **"jump table at X: no bound found"**: the bound check before a `jmp [table + reg*4]` didn't
  match a known pattern. Read the disassembly and add `JUMP_TABLES[jmp] = (table, count)`.
- **Errors inside a function** ("unexpected instruction"): usually data decoded as code. Either a
  wrong entry (add it to `NOT_ENTRIES`) or a real function that runs into data after a call that
  never returns (add the callee to `NORETURN_ADDRS`).
- **Warnings for an instruction**: the translator doesn't do it yet. The function stops with a
  message if it ever runs. Add the instruction to `translator/codegen.py` and a case to
  `tests/selftest/cases.s`.
- **Functions found only at run time** (a call through a pointer lands where no function is):
  add the address to `EXTRA_ENTRIES`.

Cross-check discovery against a disassembler's function list (Ghidra, IDA, REA): every function it
finds should be translated, and every one only you find should have a reason (vtables, callbacks,
exception handlers).

## 5. The runtime

The generated C calls the runtime for every import, for calls through pointers (`icall`), for x87
helpers and for faults. A port's runtime:

- **maps the program** at its preferred base (`IMAGE_BASE`, usually 0x400000) and every address
  the program allocates below 2 GB, so a program address is a host pointer;
- **answers imports**: each `DLL!Name` the program imports is a C function
  `w_DLL_Name(CPU *c)` that reads its arguments from the stack (`R32(c->esp + 4)`, ...), sets
  `c->eax` and pops them as stdcall does. Missing ones are weak stubs that stop with the name,
  so you implement them as the game reaches them;
- **runs the program's threads** as host threads, one CPU record each, with Windows XP's
  static thread-local storage (`__declspec(thread)` blocks at `fs:[0x2c]`, TLS callbacks on
  start-up and on each thread's start and end);
- **jumps back into live functions**: a C++ `catch`, an `__except` block, or a `longjmp` resumes
  the function that holds the place, from the frame it keeps for that (`setjmp` call sites are
  found by the translator);
- **draws, plays and reads input** through SDL3 and Vulkan.

The CPU core is `runtime/cpu/`, the Windows stand-in `runtime/win32/` (see the README).

## 6. Hooks: changing what a function does

`HOOKS = {addr: 'name'}` makes the runtime's `hook_name(c)` run at that function's entry. It can
look at or change the arguments, call the original (`F_<addr>_orig(c)`) and change its result,
or replace the function (return 1). Use hooks for widescreen, frame rate, skipping a CD check
and new options. Hand-written functions you write yourself can be published; generated C cannot.

To replace translated functions with hand-written C for good (a decompilation people can read,
less overhead where a profile points), list them in `REWRITES` and register each rewrite with
`DECOMP`: every one can be switched off, and the A/B check proves the game behaves the same
with them on. [REWRITE.md](REWRITE.md) is the method.

## 7. Check it

Correctness first, then speed:

- **The toolkit's self-test** (`tests/selftest/run.py`) after any translator change.
- **The game's own instruction forms** (`make forms GAME=... GEN=...`): every distinct
  instruction the translation holds, run on random inputs in Unicorn and translated. It checks
  exactly the instructions your game uses, before you run the game. The cases are made in
  `build/` from your own copy and are never published.
- **Stack-pop counts** (`make argc DEFS=...`): every Windows function the runtime (and your port)
  provides, checked against mingw-w64's 32-bit import definitions. A wrong count leaves the
  stack pointer off after the call. Run it after adding Windows functions.
- **Function checks**: each translated function run on random inputs both as the original code in
  Unicorn and as C, compared. This finds translator bugs in the instructions the game actually
  uses.
- **Lockstep**: the whole game run twice, once translated and once as the original code in
  Unicorn on the same runtime, compared frame by frame. This finds anything the function check
  can't reach (threads, callbacks, the runtime's own answers).
- **Frame dumps and hashes** from scripted runs (`--test`, a virtual clock and scripted input), so
  a change that alters the picture or the game state shows up as a different hash.

## 8. Make it a native game

Once it plays, the ports add, each as an option in the game's own menu: any resolution and
widescreen, frame rates beyond the original's with the game's timing kept exact (`FRAME_STEPS`,
`FRAME_COUNTS`, `TICKS`, `MEM_OVERRIDES`), every controller working whenever it's plugged in,
and graphics upgrades. The original look and behaviour always stay one setting away.

## 9. Share it without sharing the game

A public port repository has the toolkit, your runtime, your hooks, the build config and a
launcher. The player provides the game: the launcher installs from their disc or files, checks
the SHA-256, translates and compiles on their machine, and caches the result. Never commit `gen/`,
game files, or shaders taken from the game.

## 10. Give back

A bug fixed in toolkit code goes into AntiStatic with a self-test case that would have caught it,
and a line in the changelog saying whether ports need to regenerate.
