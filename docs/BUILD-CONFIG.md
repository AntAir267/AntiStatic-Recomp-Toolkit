# Build config reference

A build config is a Python file describing one exact build of a game: which files, and what the
translator needs to know about them that it can't find by itself. It holds addresses and
numbers only, never the game's code or data. `translator/recomp.py` reads it.

Every key but `NAME` and `MODULES` (or `SHA256`) is optional.

## The files

| Key | Form | Meaning |
| --- | --- | --- |
| `NAME` | `'mygame-1.0'` | The build's name, written into `image.h` as `BUILD_NAME`. |
| `MODULES` | `[dict(file=, sha256=, base=None, load='start'), ...]` | The program and the DLLs translated with it, the program first. `base`: where the runtime maps it (`None`: its preferred base; another base applies its relocations). `load`: `'start'` if it's loaded with the program, anything else if the program loads it itself (LoadLibrary). The older form `(name, file, sha256[, base])` is read too. |
| `SHA256` | `'...'` | Instead of `MODULES` for a single program: then the first argument to `recomp.py` is the `.exe` itself. |

## Finding the code

| Key | Form | Meaning |
| --- | --- | --- |
| `EXTRA_ENTRIES` | `[addr, ...]` | Function entries discovery can't reach (only called through pointers it can't see). |
| `NOT_ENTRIES` | `[addr, ...]` | Addresses that look like code pointers but aren't functions. |
| `JUMP_TABLES` | `{jmp_addr: (table, count)}` | Switch tables whose size the translator can't work out. |
| `NONLOCAL_JUMPS` | `[addr, ...]` | Jumps through a register or memory that leave the function for a place in a live one (exception continuations, a `longjmp`'s last jump): the runtime resumes the function that holds the place. |
| `SETJMP` | `[addr, ...]` | `setjmp` functions linked into the program (a static C runtime): the place after each call to one can be resumed by a `longjmp`. Imported ones (`_setjmp`, `_setjmp3`, `setjmp`) and those a translated C runtime exports are found by name. |
| `LONGJMP` | `[addr, ...]` | `longjmp` functions linked into the program: their jumps through a register or memory become `NONLOCAL_JUMPS`. A translated C runtime's `longjmp` is found by name; an imported one is the runtime's own. |
| `NORETURN_IMPORTS` | `['ExitProcess', ...]` | Imported functions that never return. |
| `NORETURN_EXPORTS` | `[(file, name), ...]` | Functions another translated module exports that never return. |
| `NORETURN_ADDRS` | `[addr, ...]` | The program's own functions that never return (a statically linked `exit`, an error handler). |
| `INT3_CONTINUE` | `[addr, ...]` | `int3` instructions the program carries on after (debug asserts): translated as no-ops. |
| `CODE_SECTION_POINTERS` | `True` | Also take code pointers a code section holds in its own data (relocated words no instruction holds and no switch table lists): Microsoft's runtime DLLs (MSVCR90) keep vtables and initializer tables in `.text`. Off by default; discovery reruns until nothing new. |
| `IMPORT_ALIASES` | `{'msvcrt.dll': 'msvcr90.dll'}` or `{('d3dx9_43.dll', 'msvcrt.dll'): 'msvcr90.dll'}` | A module's imports from one DLL linked to another translated module's exports instead (every name must exist there; data imports' layouts are the port's to check). |
| `CODE_PATCHES` | `{addr: ('original hex', 'new hex')}` | Bytes changed before translation. The original bytes are checked, so a patch never lands on another build. |

## Changing behaviour

| Key | Form | Meaning |
| --- | --- | --- |
| `HOOKS` | `{addr: 'name'}` | The runtime's `hook_name(c)` runs at the function's entry; it can call `F_<addr>_orig(c)` and return 1 to replace the function. |
| `INLINE` | `{addr: 'ftol'}` | Calls to a known runtime helper (MSVC's `_ftol`) done in place. |
| `TABLES` | `[dict(name=, base=, entry=, count=, funcs=None, limits=[])]` | Fixed-size tables the runtime may move and enlarge: the code reads them through `reloc_<name>`, and the `limits` instructions compare against `limit_<name>`. |
| `CONSTS` | `{name: dict(value=, insns=[...])}` | Immediate values the runtime may change (`rc_<name>`), such as a resolution. |
| `MEM_OVERRIDES` | `{name: (insn or [insns], address)}` | Fixed memory operands the runtime may point elsewhere (`rm_<name>`), such as a per-frame constant. |
| `ICALL_FPU` | `{call_addr: delta}` | A call through a pointer that changes the x87 stack depth by `delta`. |
| `FRAME_STEPS` | `{insn: 'add' / 'lerp' / 'pow' / 'dither', ...}` | Per-frame amounts scaled to the game's clock when it runs above its designed frame rate (`_st0` suffix: the amount is the result left in ST(0)). |
| `FRAME_COUNTS` | `{insn: 'imm' / 'mem'}` | Counts of frames scaled the same way. |
| `REWRITES` | `'rewrites.txt'` | A file (beside the config) listing the functions being rewritten as hand-written C, one hex address a line; each gets the hook `hook_dc_<address>` for its `DECOMP` ([REWRITE.md](REWRITE.md)). |
| `INNER_HOOKS` | `True` or `{addr: 'name'}` | Off unless set. A function in both `HOOKS` and `REWRITES` gets its rewrite's hook (`hook_dc_<address>`) inside `F_<addr>_orig`, so the port's hook runs around the rewrite as it ran around the translation (widescreen, controller menus over a rewritten function). `True`: every such function (without it, the translator stops on the clash); a dict: those, as `hook_<name>` (each must have a `HOOKS` entry). |
| `CXX_FRAME_HANDLERS` | `[addr, ...]` | `__CxxFrameHandler` copies linked into the program (a static C runtime): C++ exception tables found through them, as through the import. |
| `SEH_HANDLERS` | `[addr, ...]` | `_except_handler3` copies linked into the program: `__try` scope tables found through them. |
| `SEH_PROLOGS` | `[addr, ...]` | MSVC 7's `__SEH_prolog` helpers: functions that set their SEH frame up by calling one (`push size; push scopetable; call`). |
| `TICKS` | `[addr, ...]` | Functions run at the game's fixed tick by the runtime's `framerate_tick_call`. |
| `DATA` | `{name: addr}` | Addresses the runtime uses, written into `image.h` as `GAME_<NAME>`. |

## Example

```python
"""My Game 1.0 (GAME.EXE of 2001-05-04). Addresses only: no code or data from the game."""
NAME = 'mygame-1.0'
MODULES = [
    dict(file='GAME.EXE', sha256='0123...'),
    dict(file='binkw32.dll', sha256='4567...'),
]
EXTRA_ENTRIES = [0x4a1230]                 # a callback only registered through a table
NOT_ENTRIES = []
JUMP_TABLES = {0x41c0fe: (0x41c1a0, 12)}   # `cmp eax, ecx` with ecx = 11 set far above
NORETURN_IMPORTS = ['ExitProcess', 'ExitThread', 'TerminateProcess']
HOOKS = {0x402f10: 'set_display_mode'}     # widescreen: the runtime picks the size
```
