# Changelog: AntiStatic Recomp Toolkit

"Regenerate" means a port moving to this version should translate its game again (the generated
C changes).

## v0.2.60 (2026-11-26)

- **The register audit compares more** (runtime/rewrite/audit.c, `<PORT_ENV>_AUDIT=1`; asked for by a
  port whose rewrite trial passed rewrites that an end-of-frame comparison can't tell apart):
  - **xmm0-7, mm0-7 and MXCSR**, always, bit for bit (NaN payloads and signed zeros count): the
    translation keeps them in the CPU record at every call and return, so a caller reads what its
    callee left there.
  - **The flags** of a function whose callers read them after it: the translator now writes
    `flags_out_table` into table.c. Translated before this release: the report says flags aren't
    compared; regenerate to have them.
  - **`<PORT_ENV>_AUDIT_MEM=1`: the memory each side wrote**, by the kernel's soft-dirty page bits
    (the image, DLLs, regions, the calling thread's stack), page by page: ` mem A..B T=.. R=..`; the
    dead stack below the call only counted.
  - **`<PORT_ENV>_AUDIT_TAINT=all|addr,...`: taint runs.** On a function's first four audited calls,
    the translation and the rewrite each in a child of its own, with every xmm lane and the float
    inputs `<PORT_ENV>_AUDIT_TAINT_ARGS` names (`addr:a0,t8,p1+4`) set to NaNs with payloads of their
    own, negative NaNs, -0 or denormals: an operand order or a zero or NaN case a route never gives.
    `ADDR TAINT n=.. v=nan same` or its differences.
  - docs/REWRITE.md "The register audit"; the stub game's tests catch an xmm leftover, a stray write
    and a NaN-only bug, and stay silent on the clean rewrite.
- Regenerate only to have flags compared (table.c gains `flags_out_table`; the generated functions
  are unchanged). No baselines to re-record: the audit is off unless asked for.

## v0.2.59 (2026-11-24)

- **Option `screen`: the screen full screen goes to** (win.c: on a PC with three
  screens it always went to the desktop's main one). `auto` (the default, port.h
  `PORT_DEFAULT_SCREEN`): the desktop's choice, as before; `primary`; `internal` (the built-in
  panel: an eDP, LVDS or DSI connector); or a connector's name (`eDP-1`, `DP-13`) or part of a
  screen's name as SDL gives it. The window is put on that screen (SDL_SetWindowPosition with
  SDL_WINDOWPOS_CENTERED_DISPLAY) before each SDL_SetWindowFullscreen, which on Wayland is the only
  thing that decides it (a desktop window rule doesn't hold for full screen). A connector is found
  among SDL's screens by its preferred mode's size in /sys/class/drm (either way round, when exactly
  one screen has it), else by SDL_VIDEO_DISPLAY_PRIORITY set to it before SDL starts, else by name;
  not found: the desktop's choice, logged. Changed in the game's menu (`options_changed()`), the
  window moves at once and the desktop size is measured again.
- **`host_screen_choices(values, labels, max)`** (win.h): the choices for a port's settings row
  (auto, primary, internal when there is a built-in panel, then each connected screen: Linux its
  connector and size, elsewhere SDL's names).
- Test runs (`--test`) and PCs with no screen ignore it. No regenerate, no baselines to re-record.

## v0.2.58 (2026-11-24)

- **The mouse reaches the game's right and bottom edges** (win.c, from pcrecomp a67767b): the
  window's last point is now the game's last pixel. Unless the picture is a whole number of pixels
  a point (a game larger than the window's points, or a fractional HiDPI scale), that point mapped
  a pixel or two short, so a game that scrolls or reacts at its screen's last pixel never did on the
  right or at the bottom. Test runs take no real mouse, so nothing they record changes.
- No regenerate, no baselines to re-record.

## v0.2.57 (2026-11-24)

- **`PORT_D3D9_MSAA_KEEP_DEPTH 1` keeps the depth buffer's twin cleared**: a clear of the device's
  own depth buffer while no twin is bound (Nitronic Rush clears it with the back buffer set, then
  draws its depth and scene passes into twins without clearing again) is made in its twin too. In
  v0.2.56 the twin was never cleared and every depth-tested draw failed (Nitronic Rush at 4x showed
  only its sky and HUD). Settings off: nothing changes.
- **`--log d3d` traces clears** while antialiasing is on (the first 400): their flags, rects, and
  whether a twin and which depth buffer were bound.
- No regenerate, no baselines to re-record (antialiasing off is untouched).

## v0.2.56 (2026-11-24)

- **`PORT_D3D9_MSAA_KEEP_DEPTH 1`**: antialiasing for a game that sets its render targets and keeps
  the device's own depth buffer bound, never setting it again (Nitronic Rush: v0.2.55's trace showed
  no SetDepthStencilSurface at all, so no twin was ever chosen). A target of the depth buffer's size
  gets its twin when it is set (the scene and depth passes; not smaller ones such as a bloom chain).
  Off by default: a game that sets a target while the last pass's depth buffer is still bound, then
  a depth buffer of its own, could have a twin resolved into a pass drawn with another.
- No regenerate.

## v0.2.55 (2026-11-24)

- **`--log d3d` traces what a game binds while antialiasing is on** (d3d9_msaa.c): at each change of
  render target or depth surface, the first 400, their sizes, formats and usage, whether the depth
  surface is the device's own or the game's and the target a texture's, and whether a twin went to
  the device. For a game where `antialiasing` never engages (Nitronic Rush): the bridge twins only
  targets drawn with the device's own depth buffer, chosen when that buffer is set.
- No regenerate.

## v0.2.54 (2026-11-24)

- **Caught exceptions on Windows builds** (Clang, Zig's toolchain): a translated function's body
  holding the resume point's `__builtin_setjmp` is never inlined into its wrapper (`__attribute__
  ((noinline))`), and the rewrites' `EH_OWNER` is split the same way and reads `c` and its record
  back after a resume. A resume's `__builtin_longjmp` restores no callee-saved registers; Clang kept
  the wrapper's record pointer in one, so after one resume its `eh_pop` left the runtime's chain of
  frame records pointing at garbage and the next throw faulted walking it (Penguins!: its first
  caught throw on Windows). GCC builds (Linux) weren't affected. Checked: Penguins! on Windows
  catches its throws and plays into its first level; self-test and stub game unchanged.
- **Regenerate** (every translated function with resume points changes); no re-record: GCC builds
  run the same.

## v0.2.53 (2026-11-24)

- **A shared Direct3D 8 bridge to DXVK**: `runtime/d3d8` (`d3d8.c`, `d3d8_bridge.h`, README) and
  `tools/d3d8/d3d8gen.py`, merged from the D3D8 ports' own copies (LEGO Island Xtreme Stunts' the
  base). In it: the `--test` card as the D3D9 bridge's (`test_card`, `test_vram`; GetAdapterIdentifier,
  both GetDeviceCaps' MaxPointSize, GetAvailableTextureMem, DXVK's vendor), the 1920x1080 test desktop,
  GetAdapterIdentifier copied as the program's 1068 bytes (DXVK wrote 1072), the fixed modes with no
  screen, option `anisotropic`, plain buffer locks covering the whole buffer, lockable back buffers for
  `--shot`, the Windows path (option `d3d8_dll`, DXVK's `d3d8.dll` beside the program, then Windows'
  own), weak hooks for a port's own Vulkan renderer (`vk_create8`, `vk8_buf_host`, `vk_debug8`), and
  `<PORT_ENV>_D3D_INVENTORY` now written at exit (no copy called its writer). Port settings:
  `PORT_D3D8_FIXED_MODES 1` (the fixed list in real runs too), `PORT_D3D8_MODES_FILTER 1` (only
  option `resolution`'s modes).
- Checked: LEGO Island Xtreme Stunts on it, call counts identical at all 84 points and 4 frames
  pixel-identical; Penguins! into its first level, frame 8500 pixel-identical. Memory under `--test`
  moves once (the test card). Windows: builds; a start-up failure there is in the Windows host, not
  the bridge (the ports' old bridge fails the same), and is being looked into.
- A D3D8 port moving onto it drops its own copy (docs/MOVING.md); its generated bridge changes.

## v0.2.52 (2026-11-24)

- **`cpu_x87_sin`, `cpu_x87_cos`, `cpu_x87_tan`, `cpu_x87_atan2`, `cpu_x87_f2xm1`, `cpu_x87_yl2x`,
  `cpu_x87_yl2xp1`** (cpu.h, fpu.c): the x87 transcendentals as values, as the translated
  instructions compute them (portable unless `PORT_X87_PORTABLE 0`), for a port's hand-written
  rewrites. A rewrite's own inline fsin gave the host's answer whatever the setting said. They keep
  the host's control word.
- cpu.h no longer pulls in `<math.h>` (sse_portable.h used it for one square root): C23's `fmul` /
  `fdiv` clashed with a port's own helpers of those names.
- No regenerate.

## v0.2.51 (2026-11-24)

- **DirectSoundEnumerate allocates its strings in a fixed order** (Test Drive 6): two allocations
  in one argument list came out in GCC's order (right to left) in the Linux builds and Clang's (left
  to right) in Zig's, so the program's heap differed between them from frame 1. Each is now its own
  statement, in the order GCC's builds always made them, so recorded GCC runs don't move and Zig
  builds now match them. No other call in the runtime allocates twice in one argument list.
- No regenerate.

## v0.2.50 (2026-11-24)

From Stranglehold (Unreal Engine 3):
- **A call to the next instruction is a push** (the get-PC idiom, `call $+5; pop eax`: UE3's
  context capture after a caught `appThrowf`). The translator made the next instruction a function,
  which popped its own return address and returned with its caller's ("returned to 006fe651
  instead of 006fd3e8"). Self-test case `case_call_next`. **Regenerate** if your game has the idiom
  (the generated code says "call to the next instruction"); L.A. Rush, Nitronic Rush and LEGO
  Island 2 translate byte for byte the same.
- **No screen** (SDL's offscreen driver: a PC with no display, the farm's jobs): the desktop is
  1920x1080 (`host_no_screen()`, win.c), and the D3D9 bridge gives the fixed mode list and desktop
  of `--test`. The probe window measured 1280x720 there and "maximized" did nothing, and UE3 fitted
  its 1920x1080 window into that as 0x0 and never made its device. A screen under 320x240 counts as
  none too.
- No regenerate apart from the first item.

## v0.2.49 (2026-11-24)

- **`<PORT_ENV>_TEST_THROW_TYPE=wstr`**: the test throw is a `wchar_t*` (`L"test throw"`, type
  descriptors `.PA_W` and `.PAX`, thrown non-const) instead of an `int`, so a catch of Unreal Engine
  3's `appThrowf` text (`catch (const TCHAR*)`) takes it (Stranglehold). The stub game throws both kinds.
- The log names each kind of resume: "longjmp (the C runtime's)", "nonlocal jump (a translated
  longjmp)", "exception handled (C++ catch)", "exception handled (__except block)" and "exception
  handled (a C runtime's continuation jump)". All of them read "exception handled" before, so
  libjpeg's longjmps inside d3dx9 looked like caught faults (Nitronic Rush).
- No regenerate.

## v0.2.48 (2026-11-23)

- **`HOOKS = {addr: 'test_throw'}` links now** (Nitronic Rush found it couldn't): a hook is the
  generated table's pointer, which the runtime sets; v0.2.47 defined a function of the same name.
  The function is `eh_test_throw(CPU *c)` (runtime.h), which eh_init puts in `hook_test_throw`, and
  which a port's own hook may call (throwing only from one caller, say). The stub game now hooks its
  throw helper with it and runs once with `STUB_TEST_THROW=1`: the throw is caught and gone on from.
- No regenerate (a port that already moved to v0.2.47 needn't translate again).

## v0.2.47 (2026-11-23)

**Caught exceptions go on in Visual C++ 2005/2008 programs** (Nitronic Rush's MSVCR90; the item open
since v0.2.13). **Regenerate** (the translation changes); **record baselines again** only if your
generated code changed beyond `icall` -> `ijump` (L.A. Rush's and LEGO Island 2's runs don't reach
the changed code).
- **SEH4 scope tables** (translator/eh.py): `push -2; push <table>; push <handler>` frames and
  `__SEH_prolog4` ones (the helper known by its own first bytes), the table's 16-byte cookie
  header, -2 as the outermost level. Nitronic Rush: 2 -> 206 scope tables, 144 -> 204 places.
- **`__except` blocks are translated** as code of their own (SEH3 and SEH4): normal flow jumps over
  them, and a program without relocations gave no other way to find them (Nitronic Rush had 5 that
  were never translated).
- **A C runtime's continuation jumps are found by the translator**: a jump through a register right
  after esp and ebp are both loaded anew (not an epilogue's `mov esp, ebp; pop ebp`) is a non-local
  jump, which resumes the live function holding the place with that esp and ebp. That is
  `_JumpToContinuation` in MSVCR90 (0x7857d796, and its catch-block frame switch 0x7857da11), MSVCR71
  (L.A. Rush) and LEGO Island 2's statically linked CRT. Before, it went through `icall`, which took
  esp from an SEH3 frame's slot: wrong for a C++ frame.
- **`ijump`**: a jump through a register (an indirect tail call) now calls `ijump`, which is `icall`
  except while an exception is dispatched: then a jump to a place a live frame goes on at resumes
  that frame. An SEH4 handler goes into an `__except` block that is also a translated function;
  calling it as one returned from the wrong frame. Calls never resume (handlers call funclets).
  A port with its own `icall` (an offline checker) needs an `ijump` beside it.
- Catch funclets' continuations are read up to the padding after a `ret`: reading on into the next
  function took its `mov eax, imm; ... ret` for a continuation.
- **`HOOKS = {addr: 'test_throw'}`** with `<PORT_ENV>_TEST_THROW=N`: the function's Nth call throws a
  C++ `int` from its first instruction, to check on the real game that a catch goes on.
- Checked: the stub game has a VC 2008 try/catch (FuncInfo 0x19930522, MSVCR90's
  `_JumpToContinuation` instructions) and an SEH4 `__try/__except` (an `__except` block reached only
  from its table), each thrown into from two calls down: both go on in their own frame with their
  locals. Self-test unchanged. Translations: LEGO Island 2 and L.A. Rush differ only in their
  continuation jumps (and `ijump`); Nitronic Rush gains the tables, places and 6 functions above.

## v0.2.46 (2026-11-23)

Test runs give the same results on every PC: recorded routes no longer depend on the CPU's maker or
the graphics card. **Ports record their baselines again once at this move** (docs/MOVING.md).
- **`PORT_X87_PORTABLE` is on by default** (v0.2.45's x87soft.h: fsin, fcos, fsincos, fptan, fpatan,
  f2xm1, fyl2x, fyl2xp1 from basic arithmetic). `PORT_X87_PORTABLE 0` keeps the host's instructions
  (timing comparisons).
- **`PORT_SSE_PORTABLE`, on by default** (runtime/cpu/sse_portable.h): rcpss, rcpps, rsqrtss and
  rsqrtps exact, the same on every x86 CPU. The instructions are 12-bit approximations whose tables
  differ between Intel and AMD (L.A. Rush's routes split at frame 204). The exact result is within the
  instructions' own bound, and their special cases are kept (NaNs, zeros and denormals, infinities,
  tiny results flushed, negative roots). cpu.h redirects `_mm_rcp_ss`, `_mm_rcp_ps`, `_mm_rsqrt_ss`
  and `_mm_rsqrt_ps`, so generated code and rewrites that use them both get these; `cpu_rcp1` and
  `cpu_rsqrt1` are the scalar forms for a reference build. cpu.h now reads the port's port.h: make
  generated objects depend on it and on runtime/cpu/*.h.
- **A test card under `--test`** (D3D9): option `test_card = nvidia` (default) | `amd` | `intel` |
  `real`, and `test_vram = MB` (default 2047). GetAdapterIdentifier, GetAvailableTextureMem and
  GetDeviceCaps' MaxPointSize (the one capability DXVK takes from the Vulkan driver) answer as that
  card, and DXVK is given its vendor for its own vendor-dependent answers. Each card is the same on
  every PC, so a game's own per-vendor path can be recorded and checked on any PC. Outside `--test`,
  always the PC's own card.
- GetAdapterIdentifier copies the program's 1100-byte structure: DXVK wrote the host's 1104 bytes into it.
- Measured on L.A. Rush (game-thread CPU): the x87 setting +4-6%, the SSE one +1-2%, about 0.2 ms a
  frame together; mean frame times unchanged within noise. Proof: L.A. Rush byte-identical between an
  Intel and an AMD PC for 600 frames on two routes; Nitronic Rush's AMD laptop matching an NVIDIA PC
  on all five routes where it differed from frame 100 (a later split, from frame 1500, is colours
  read back from the GPU, which only a software Vulkan driver makes the same).
- Self-test: cases for fcmov after each kind of flag-setting instruction, the SSE bitwise ops and movq
  on xmm, and fincstp / fdecstp (from xboxrecomp v0.18.0's lifter fixes; all match).
- tools/rewrite/ab.sh: the game program the command starts is part of A's cache id, so a new build
  never reuses an old build's translation run (Nitronic Rush's false "differ").
- Regenerate the D3D9 bridge (tools/d3d9/d3d9gen.py changed); the game's C doesn't change.

## v0.2.45 (2026-11-18)

- **`PORT_X87_PORTABLE 1`: the x87 transcendentals give the same results on every x86 CPU.**
  fsin, fcos, fsincos, fptan, fpatan, f2xm1, fyl2x and fyl2xp1 ran on the host's own x87, and Intel's
  and AMD's differ in the last bits on all eight (measured: two Intel CPUs agree with each other and
  not with two AMD ones). So a recorded run's memory moved
  between PCs: L.A. Rush's hash streams split between an AMD and an Intel PC at frame 204, Nitronic
  Rush's at frame 100. With the setting, runtime/cpu/x87soft.h
  computes them from + - * / and sqrt alone (which IEEE fixes bit for bit), in the full 64-bit
  significand with round-to-nearest: π/2 reduction in four exact parts, Taylor kernels, angle
  halving for the arctangent, the atanh series for the logarithms. Zeros, infinities and NaNs keep
  the hardware's answer, which the x87 defines exactly.
  - Accuracy against 400-bit references below 2^31: sin 1.5, cos 2.2, tan 2.9, atan2 2.7, f2xm1 2.8,
    log2 1.1, log2(1+x) 2.3 units in the last place at worst. Near multiples of π the hardware is
    worse (AMD's fsin(π) is off by 2·10^13 units; the software by 0.45).
  - Results hash the same on an AMD and two Intel CPUs over 2,000,000 inputs each.
    The self-test passes with it on and off (`SELFTEST_CFLAGS=-DPORT_X87_PORTABLE=1`).
  - Off by default, so recorded runs keep their values; a port that turns it on records its
    baselines again once, and they then hold on every PC. The C2/C1 condition bits are 0 after a
    soft result.
- No regenerate.

## v0.2.44 (2026-11-18)

- **Miles, under `PORT_MILES_COPY_SAMPLES`: streams as LEGO Island 2's runs had them** (its town
  route, frame 6275). AIL_open_stream sets the stream's gain when it makes the voice, and the
  once-a-frame `mss_frame` looks at each playing stream's status and calls the stream's
  AIL_register_stream_callback callback when it has ended (li2-native's mss_frame). Each look reads
  the `--test` clock. Without the setting nothing changes (streams still never call back).
- No regenerate.

## v0.2.43 (2026-11-18)

L.A. Rush's open gaps 3, 4, 5, 7, 8 and 9, in the shapes its chat asked for:
- **Crash reports for host threads** (Linux x86-64; from L.A. Rush's crash.c). A crash on a thread
  with no guest CPU (DXVK's, the driver's, SDL's) is reported from the signal handler: the faulting
  address, the thread's name, the pc as module+offset (for `addr2line`) and a backtrace by
  module+offset, then by name. Written with write() only; an alarm ends a report that hangs; then the
  signal's own action, so the exit status (128 + the signal) and a core dump are as without it. A
  game thread's crash gets the pc line and then fatal()'s report as before. SIGABRT is caught too.
  `crash_line(fmt, ...)` and an optional `port_crash_report(int guest_thread)` add a port's lines;
  `log_fd()` gives the log file's descriptor. A port's own handler can go.
- **`port_thunk_hook(c, index, name, after)`**: the per-call hook with the thunk's index (a
  profiler's table), before and after every call into the runtime through a thunk, beside
  `api_call_hook` (by name). One predictable branch per call when unset.
- **Speed**: calls through pointers check the thunks before the module ranges (most are Direct3D's
  and COM's; L.A. Rush's race 2.53 -> 2.44 ms a frame in its runtime), and `com_host` / `d3d_host`
  are inline (2.21 -> 2.12 ms).
- **`port_file_redirect(rel, out, n)`**: a port's own place for a game file it reads, before the
  user folder, mods and xbox_data (writes still go to the user folder).
- **File answers as Windows gives them** (FIXES.md 2026-11-12): CREATE_NEW / CREATE_ALWAYS /
  TRUNCATE_EXISTING without GENERIC_WRITE, and OPEN_ALWAYS of a file that is nowhere, make the file
  in the user folder, not the game folder; MoveFile onto a target that exists fails with
  ERROR_ALREADY_EXISTS; CreateDirectory of a folder the game folder has says ERROR_ALREADY_EXISTS;
  GetTempFileName takes the next free number instead of emptying a file.
- **`decomp = NAME`**: only the rewrites of a port's named set, from
  `const char *port_rewrite_subset(const char *name)` (a hex comma list; NULL: no such set).
- **PEEK pointer chains**: `<PORT_ENV>_PEEK=addr>off>off:n` reads the dword at addr and adds off,
  up to three times; a pointer outside the program's memory logs `addr>-`.
- No regenerate.

## v0.2.42 (2026-11-18)

From L.A. Rush's move (its gaps 14-18):
- **A table listed in `JUMP_TABLES` stays a bounded switch** even when every entry is a known
  function (its cases become tail calls). Before, any table of functions became an indirect tail
  call, listed or not, so listing one changed nothing. L.A. Rush's seven `cmp al,N; jbe; movzx
  edx,al; jmp [edx*4+T]` tables are switches again.
- **Discovery refuses a code pointer found in data whose body holds an instruction no 32-bit
  compiler emits** (`enter` with a nesting level, a 16-bit address): it is data that decodes.
  L.A. Rush's MFC71 loses 3 such functions (0x7c171c20, 0x7c173a2c, 0x7c173a40) and their 3
  warnings, and gains 0x7c173ade, a catch handler's continuation one of them had swallowed.
- `PORT_CMDLINE_EXE_W`: GetCommandLineW's program name when it differs from GetCommandLineA's.
- `PORT_CURSOR_START_X` / `_Y`: where GetCursorPos starts (320, 240 unless set).
- The v0.2.19 entry now says a port moving past it must compile runtime/rewrite/audit.c.
- **Regenerate** if your build lists a table of functions in `JUMP_TABLES` (L.A. Rush) or finds
  code pointers in data (`CODE_SECTION_POINTERS`, relocated DLLs). Nitronic Rush and LEGO Island 2
  translate byte for byte the same.

## v0.2.41 (2026-11-18)

- **Miles, under `PORT_MILES_COPY_SAMPLES`: setting a sample's file follows the old order** (LEGO
  Island 2's town route, frame 6221). Replacing a playing sample's file no longer pauses and rewinds
  the old voice before freeing it, and the new voice gets its gain rather than a second rate (its
  rate is set when it is made). Each of those calls reads the `--test` clock, so the old order moves
  it 2 reads less per replacement. Same sound; nothing changes without the setting.
- No regenerate.

## v0.2.40 (2026-11-18)

- **Start-up raises the open-file limit** (Linux) from the soft limit to the hard one. Each open
  guest file is one host descriptor, and the usual soft limit of 1,024 stopped a game that keeps
  2,025 files open after loading (Nitronic Rush on two PCs with the usual limit: 848 "failed to open", then a
  crash). Windows has no such limit. Logged under `--log file`.
- Running out of descriptors anyway is logged always ("too many files open") and answers
  ERROR_TOO_MANY_OPEN_FILES (4) instead of ERROR_ACCESS_DENIED.
- No regenerate. A port that raises the limit itself (Nitronic Rush's game_init) can drop that.

## v0.2.39 (2026-11-18)

- **The D3D9 bridge applies options `anisotropic` and `shadow_filter` while the game runs**, so a
  port can put them in the game's own video options (Stranglehold).
  - On `options_changed()` the bridge calls DXVK's `DXVK_D3D9SetSamplerAnisotropy` and
    `DXVK_D3D9SetShadowFilter` on the device DXVK made, when the DXVK build exports them
    (third_party/dxvk/dxvk-live-options.patch; without the patch they still take effect at the next
    start).
  - From L.A. Rush's d3d9.c.
  - Compiles; not run in a game here. The stub game passes and its Windows build links.

## v0.2.38 (2026-11-18)

Ready to publish:
- A README for people new to the toolkit, CONTRIBUTING.md, and issue templates.
- NOTICE credits every borrowed source, and SwARc's licence sits beside the code taken from it.
- tools/audit_public.py checks more (it fails an export holding anything private).
- tools/win's Zig wrappers use `$ZIG`, or `zig` on the PATH (they named a path on one machine).
- Comments and notes no longer carry private details. No code changed (the stub game passes).

## v0.2.37 (2026-11-18)

More from L.A. Rush's move (its work/as-toolkit-gaps.md, 13 items; v0.2.36 had #1 and #13). Verified: stub
game and its Windows link, tests/pad (a new split-screen case that fails on v0.2.36), and LEGO Island
2's hashes are identical.
- **"program exited with code N" is the log's last line**, after the exit work's own lines. The
  normal-quit note joins it: `program exited with code 13 (a normal quit: 0 to whoever started it)`.
  A second thread exiting meanwhile waits for the first to end the process, and exit work that exits
  again ends at once (#2: they could interleave).
- **COM objects' host table is fixed chunks that never move**, with its count published last, so
  `com_host` (no lock) can't read a table another thread is reallocating. This is LIXS's fix, which
  only LIXS had (#6, FIXES.md 2026-11-06).
- **Split screen chosen with no controller connected**: the first controller plugged in is player 1's
  (it went to player 2) (#12).
- **input.h declares `input_key_held`** (#11).
- Left for later (L.A. Rush works around them):
  - #3 a crash report for host threads;
  - #4 a per-call hook;
  - #5 icall's check order and com_host inline;
  - #7 a file-redirect hook and CreateFile/MoveFile/CreateDirectory answers;
  - #8 rewrite subsets;
  - #9 PEEK pointer chains;
  - #10 seven `cmp al,N; jbe; movzx edx,al; jmp [edx*4+T]` jump tables discovery no longer bounds
    (they dispatch correctly through icall meanwhile).

## v0.2.36 (2026-11-18)

From L.A. Rush's move onto the toolkit:
- **The draw-stream hash looks Direct3D 9 calls up again** (runtime/win32/drawhash.c).
  - The bug: its fallback method and argument tables were weak definitions with one entry, so GCC
    took the lookup for a loop over one entry and folded it. Every device call hashed as method 0
    with no arguments, even when the port's own tables (d3d9gen.py) won the link, and with
    `-fno-aggressive-loop-optimizations` it became an endless loop.
  - The fix: the tables are now weak references of unknown size, NULL in a port without them,
    which then hashes no device calls, as meant.
  - Checked by reading GCC's code at -O2, -O3 and with `-fno-aggressive-loop-optimizations`: the old
    lookup tested entry 0 alone and never called strcmp, the new one walks the table.
  - Who: every D3D9 port's `--hash` draw column (Enter the Matrix, Stranglehold, Nitronic Rush, L.A.
    Rush). Its memory column never depended on it. Expect the draw hashes to change, since they
    count the calls now, and rerecord those baselines once.
  - L.A. Rush's workaround (drawhash.c at -O0) can go.
- **docs/REWRITE.md**: `-mfpmath=387` is for x87-built games. An /arch:SSE game (L.A. Rush) keeps its
  rewrites in SSE, with `X87()` where the original goes through the x87.

## v0.2.35 (2026-11-18)

Discovery refuses data that looked like a function's start. This came from pcrecomp's merged-.text
work (6d1af42, 62d538b; work/upstream/REVIEW-pcrecomp-discovery.md), rewritten around relocations,
because upstream's own tests caught none of LA Rush's errors. Regenerate.
- **`discover.data_at`**: a code pointer found in data, or an export, whose bytes are data is not a
  function. It applies only in a module with relocations; without them neither test can tell.
  - Relocated word: one of the first three instructions reads across a relocated word. A relocation
    inside a real instruction lies wholly inside it, after its opcode. This catches C++ FuncInfo
    records named by `mov eax, offset FuncInfo`, vtables, and CRuntimeClass and message-map
    structures.
  - String: the start is a NUL-terminated string (`push offset "CLSID"`), unless it opens with two
    register pushes ("SVWj") or its first instructions hold a relocated address.
  - Exports that are data by their bytes (MFC71's, exported by ordinal) are no longer seeds.
- **Measured** (discovery only):

  | Game | Before | After |
  |---|---|---|
  | LA Rush | 22,987 functions, 185 errors | 22,437 functions, 112 errors |
  | Nitronic Rush | 29,916 functions, 289 errors | 29,902 functions, 288 errors |

  - LA Rush: MFC71 lost 553 fake functions and gained 3 real ones that a fake body had hidden. The
    exe, msvcr71, msvcp71 and binkw32 are unchanged.
  - Nitronic Rush: 15 FuncInfo records and pointer tables gone from its DLLs, 1 real function found,
    the exe unchanged.
  - LA Rush's exe imports none of the 16 exports now treated as data. LEGO Island 2's translation
    is byte-identical.
- **A first version was too eager:** the string test in a module without relocations took
  `push ecx; push <address>` for "Qh" plus text (the addresses end in a zero byte), and cost Nitronic
  Rush's exe 120 real functions and LA Rush's 44. Hence "relocated modules only".
- **Verified:** self-test 50/50 + 375/375, stub game.

## v0.2.34 (2026-11-18)

From reading Surveyor's notes on pcrecomp's latest changes (nothing downloaded; SWEEP.md "Sweep 5").
Log lines only; nothing a game does changes:
- **Fixed-size tables say when they are full** instead of quietly leaving the rest out (pcrecomp
  08023da's lesson): mod folders past 32, SetWindowsHookEx past 32 hooks, FMOD past 512 samples or
  32 streams, and the register audit's nesting list past 256. The runtime's thunks and handles
  already stopped with a message.
- Checked and already right: a new thread's TLS slots start empty (pcrecomp 79c4dbd: its TEB had
  been a copy of the first thread's), since the whole TEB is zeroed, reused ones too.
- Different from Windows, noted: FreeLibrary does nothing, so a DLL loaded again keeps its state
  (pcrecomp 85e7fc1 restores writable sections on a reload). Cross-thread SendMessage runs the
  window procedure on the sender's thread, so pcrecomp's deadlock (e882cd5) can't happen here.
- Verified: the stub game passes.

## v0.2.33 (2026-11-14)

- **The far-terrain draw (`vk_far_draw`) decides on the game thread whether it goes to the GPU**
  (from 1NSANE: its map copies flickered).
  - The bug: `vkr_far_draw` called `vk_tnl_ok` on the render thread, but that reads the game
    thread's draw count, widescreen HUD and menu depths and the shadow pass. With the game a frame
    ahead (165 Hz), it read the next frame's state and dropped the whole far terrain, every copy of
    the map, from that frame.
  - 1NSANE measured it: 3-5 frames in 114 before the fix, 0 after, and 0 with the render thread off.
  - Now `vk_far_draw` checks right after counting the draw, for both paths, and `vkr_far_draw` only
    checks that Vulkan and the mesh are there.
  - Only ports that call `vk_far_draw` (1NSANE) are affected; `--test` runs have no render thread.
    Compiles here; 1NSANE's port verified the same change in its own copy.

## v0.2.32 (2026-11-14)

The two SwARc changes v0.2.31 left out, now taken in. **They can move a
port's `--test` hashes**, so each port checks them when it moves its pin.
- **A finished thread's stack, TEB and TLS blocks go to the next thread** created with the same
  stack size.
  - The stack is zeroed (`vm_zero`), the TEB is rewritten, and the TLS blocks are filled from
    their templates again. They are kept for reuse, not freed, so every other allocation stays
    where it is.
  - The release happens under the kernel lock before the finished thread hands over its turn, so in
    `--test` which thread gets them never depends on timing.
  - Host threads are created detached, so the system gets them back.
  - Why: nothing was ever given back, and a game making a thread per job runs out of address space
    (SwARc saw it after about 1,900 threads).
  - What moves: a thread created after another finished now sits at the old one's addresses, so
    pointers into its stack or TEB that land in hashed memory change. A finished thread's CPU record
    keeps no stack or TEB (`cpu_dead_stacks` doesn't report it).
- **DuplicateHandle makes a new handle to the same object** for events, mutexes, semaphores,
  threads and the other kernel objects, so closing the original leaves the duplicate working.
  - `GetCurrentThread()` duplicated becomes a real handle to that thread. It used to mean "the
    caller" to whichever thread used it, so a watchdog waiting on the main thread saw it signalled at
    once. The main thread gets a thread record of its own that never finishes.
  - DUPLICATE_CLOSE_SOURCE is honoured.
  - Files and finds keep their one handle, because their objects go when it is closed.
  - **CloseHandle on the two pseudo handles succeeds**, as on Windows.
  - What moves: the new handle values, if they land in hashed memory during a test run (Stranglehold
    and Nitronic Rush, among others, import DuplicateHandle).
- **Verified:**
  - The stub game checks both, plus v0.2.31's abandoned mutex: a second thread on the first one's
    stack with fresh TLS, a duplicated main-thread handle not signalled while main runs,
    WAIT_ABANDONED, closing a pseudo handle. It passes in `--test` and in three real-time runs.
  - Its Windows build links.
  - LEGO Island 2: its menu and town hashes are identical.

## v0.2.31 (2026-11-14)

From the fourth upstream sweep: pcrecomp up to 837e037 and SwARc up to 96b72fb, both MIT
(work/upstream/REVIEW-*.md, SWEEP.md "Sweep 4"). Regenerate: the translator gained
instructions. Verified: self-test 50/50 on 200 inputs (3 new cases) + 375/375, stub game 9/9 and
its Windows link, tests/dosmask 32/32, tests/pad, and LEGO Island 2 (its translation unchanged; menu and town hashes identical).

**Translator and x87** (pcrecomp):
- **fbld / fbstp** (packed BCD: Borland's and Delphi's float-to-text) and **ffreep** are
  translated. Before, they stopped the game as "not translated".
- **fptan / fsincos with a finite |x| >= 2^63** push nothing and set C2, caught before the host
  instruction. Its asm has two outputs, so the compiler popped two values the CPU never pushed.
  - In a function whose x87 stack the translator keeps in locals, that case can't be followed, so
    it now stops with a message (`fpu_locals_push_check`) instead of reading the wrong slot.
- **pushf / popf with the 0x66 prefix** move 16 bits; popf leaves ID and AC as they were.
- **loop / loope / loopne with the 0x67 prefix** count cx.

**Windows layer** (SwARc, and two bugs of ours found beside it):
- **GetTextMetricsA**: tmPitchAndFamily is at +51 (it was written at +52, which is tmCharSet);
  tmDigitizedAspectX/Y are 96.
- **One-shot timeSetEvent timers free their slot once fired** in real runs. They kept it, so the
  17th timeSetEvent failed; `--test` already freed it.
- **A thread ending while it holds a mutex** abandons it: the next wait gets WAIT_ABANDONED
  (0x80 + i) once. Before, every later waiter waited for good.
- **TlsFree** clears that slot in every thread, so a reused index starts at 0, and fails for an
  index that isn't allocated.
- **wsprintfA**: each conversion can fill the whole 1024-character output; it was cut at 511.
- **FindFirstFile matches masks as Windows does** (runtime/win32/dosmask.h, SwARc's matcher):
  "name.*" matches "name", "file?.txt" matches "file.txt", "*." matches names with no extension,
  and `[` is literal. POSIX fnmatch got these wrong. tests/dosmask has SwARc's table plus four
  more cases. A header, so no port's file list changes.
- **VirtualAlloc** refuses a size that would wrap (near 4 GB: it handed out an address in use).
  **VirtualFree** takes exactly MEM_RELEASE or MEM_DECOMMIT, and answers ERROR_INVALID_ADDRESS
  (487) for a range that is no reservation's (it said 87).

**Direct3D 9 bridge** (SwARc):
- **LockRect / LockBox** refuse a rectangle or box outside the level, an empty one, or one off a
  compressed format's block grid, with D3DERR_INVALIDCALL. The whole level is locked on the host,
  so such a rectangle pointed past the program's copy into the runtime's heap.

**Not taken yet** (they can move ports' hashes, so they wait for a decision): SwARc's clean-up of
finished threads (reusing their stacks, TEB and TLS) and DuplicateHandle making real duplicates.

## v0.2.30 (2026-11-14)

- **A game quitting normally exits 0 in real runs.** That covers ExitProcess, the entry point
  returning, the last thread's ExitThread and TerminateProcess on itself.
  - Why: on Windows nothing reads that code, and old games often leave a stray value there (Test
    Drive 6 gives 1, Off-Road 38). The toolkit passed it on, as the ports' own runtimes did before
    the move, so a launcher (AntRecomp) said "exited with code 38" after a normal quit.
  - The log still says `program exited with code N` (the game's), plus a line saying 0 went to the
    launcher.
  - What keeps its code: `--test` runs (the checks compare it: the stub game's 42), FatalExit, the
    runtime's own fatal errors (2) and crashes (signals). `PORT_KEEP_EXIT_CODE` keeps the game's
    code in real runs too.
  - Verified: the stub game passes (42 in `--test`), its real run exits 0 with 42 in the log, and the
    Windows build links.

## v0.2.29 (2026-11-14)

- **The D3D7 stats line names the thread its CPU figure is from.** `<PORT_ENV>_VK_STATS`'s per-100-frames
  line said "game thread CPU" but timed the thread the stats run on, which is the render thread
  whenever that is on. It now says "render thread CPU" there, and "game thread CPU" only with the
  render thread off. Off-Road's 0.99 (pre-move), 1.71 (v0.2.9) and 1.57 (v0.2.26) were all
  render-thread figures; its game thread, by its own clock, stayed at 2.9-3.0 ms throughout.
- v0.2.28's spin of 200 is confirmed by Off-Road on the Deck. Frame time is the same at 2000, 200 and
  0 (within 0.03 ms; the GPU sets the pace). 200 saves about 1.4 s of render-thread CPU over a race
  run, and 0 saves nothing more. The whole run's CPU (about 21.3 s) is back at the pre-move build's
  21.8 s.

## v0.2.28 (2026-11-14)

- **The D3D7 render thread spins 200 pauses before sleeping, not 2000** (`<PORT_ENV>_RENDER_SPIN=N`
  sets it; `RENDER_SPIN_DEFAULT` in port.h).
  - Why: while the game thread publishes draws closer together than the spin, the render thread
    never sleeps and burns every gap. Off-Road on v0.2.26 had 13 times the pre-move spin, about
    0.27 ms of CPU a frame, with its game thread back at the pre-move CPU and its pictures identical.
  - Asleep, the thread is still woken for a batch of 32 commands, at the frame's end and by every
    wait for it.
  - Timing only: it changes no picture and no `--test` run (the render thread is off there).
  - Not yet measured: Off-Road's port compares 2000, 200 and 0 on the Deck.
- v0.2.26 confirmed by Off-Road: pictures identical to v0.2.9 on every repeatable frame, and the game
  thread's own CPU clock back at the pre-move build's (2.90 ms a frame against 2.88-2.93).

## v0.2.27 (2026-11-14)

- **`PORT_PAD_JOYS -1`: one merged joystick that comes with the first controller and goes with the
  last** (from 1NSANE). Every controller drives it (buttons held on any, each stick and trigger from
  the one pushing furthest, rumble on all), and a session with no controller has no joystick at all,
  as the game shipped. It is named after the controller that made it appear.
  - Why: 1NSANE drives a race from the one joystick it was set up with, so with `PORT_PAD_JOYS 0`
    (a joystick per controller) a second controller, beside a handheld PC's own pad, drove the
    menus and rumbled but couldn't steer. Mode 0 has that problem for any game
    that binds one joystick; use -1 there.
  - tests/pad checks the new mode: none before a controller, one for two, kept while one is left,
    gone with the last, back on the next plug-in.
  - Nothing changes for ports that don't set it.

## v0.2.26 (2026-11-14)

Off-Road's game-thread CPU, which rose about 70% on moving onto the toolkit (Steam Deck, its bench:
0.99 against 1.71 ms a frame; frame time only +2% there because the Deck is GPU-bound).
- **`PORT_D3D7_FAST_SCREEN`'s 40-byte screen-space vertices go to the render thread as they are.**
  The game thread wrote each one as a 128-byte SwVertex and then copied those into the render
  thread's queue. Now the queue carries the game's 40 bytes (`vk_draw_screen40`, command
  `C_DRAW40`), and the render thread does the expanding (w = 1/rhw, colour bytes) into the same
  SwVertex values as before, so the pictures shouldn't change. With the render thread off it is
  done inline.
- Only a port with `PORT_D3D7_FAST_SCREEN` (Off-Road) is affected. Not yet measured or
  picture-checked: Off-Road's port runs its frame dumps and Deck benchmark on it.

## v0.2.25 (2026-11-14)

A fix for the host window, from one port's installed build: its window vanished after the intro films
while the game kept running.
- **Hiding the main window while another of the game's windows still shows** now hands the host window
  to that one, which stays shown. Before, the host window was hidden, and DestroyWindow's takeover
  (v0.2.9, Hard Truck 2) only moved `main_hwnd` and never showed it again. That game shows its film
  window (an owned popup) first, so that one becomes the main window; it then shows its own window,
  hides the film window and destroys it.
- **A window shown while the main one is hidden** takes the host window over and shows it again.
- The stub game checks all four steps through WindowFromPoint.
- This may also explain that port's slow vsync rates on the desktop: those were most
  likely measured on such a hidden window, which the compositor throttles.
- Only a game that hides its main window while another shows, or shows a new one after hiding it,
  sees a difference. Verified: LEGO Island 2's hashes are unchanged, and the stub game passes.

## v0.2.24 (2026-11-14)

Two fixes to the rewrite tools from a port's rewrite; nothing a game runs changes.
- **tools/rewrite/ab.sh:** the saved translation run's identity now includes the machine's name and
  the variables that pick the graphics driver (`VK_DRIVER_FILES`, `VK_ICD_FILENAMES`, `DRI_PRIME`,
  `MESA_VK_DEVICE_SELECT`, `__GLX_VENDOR_LIBRARY_NAME`, `LIBGL_ALWAYS_SOFTWARE`, `SDL_VIDEO_DRIVER`,
  `DXVK_*`). The driver's device data is in game memory, so NVIDIA's and lavapipe's translation
  hashes differ from frame 10. Before, a lavapipe pass in an NVIDIA gate's folders reused NVIDIA's
  run and said "differ" everywhere. Saved runs are made again once after the move.
- **auditlog.py** labels a moved slot `moved`. It tested "one run only" first, and each half of a
  move is "kept" in one run, so `moved` never printed.

## v0.2.23 (2026-11-14)

More from a port's rewrite. The rewrite tools change, as do two things a
game can see: a film-timed scripted press is now let go when its film ends, and a forked check's
child keeps the critical sections it held. Verified: the stub game, the controller unit test (a
new film case that fails on v0.2.22), bisect.sh against a stand-in A/B script, and the
critical-section fix in a standalone program. A run of that port's affected functions is still to come.
- **A forked check's child can re-enter its own critical sections.** The register audit and fncheck
  fork from a thread that may hold a critical section (std::_Lockit inside a tree erase). glibc's
  recursive mutex names its owner by kernel thread id, which the child's thread doesn't share, so
  re-entering it spun for ever (`--test`'s EnterCriticalSection is a trylock loop). That was
  that port's `why=timeout progress=0` functions. `sync_fork_child(old_tid)` (sync.c) gives the
  forking thread's held sections to the child's thread.
- **auditlog.py:** a function is `SLOTS` whenever its register digests agree and the full ones
  don't, whatever the inputs; v0.2.22 also wanted equal inputs, which callers that differ almost
  never give. A checkpoint range starts after the last full record, so `calls 16-31`, not `16-16`.
- **A film-timed scripted press (`~ticks:`) is let go at the next presented frame** when its film
  ended before its release tick came (pad.c). A press that skipped its own film stayed held, so
  the next scripted press of that button was no new press. tests/pad has the case.
- **tools/rewrite/bisect.sh**: which rewrite makes an A/B route differ.
  docs/REWRITE.md: run the routes under a second graphics driver too.

## v0.2.22 (2026-11-14)

More from a port's rewrite. Only the rewrite tools change; nothing a
game runs without `_AUDIT` / `_AUDIT_LOG` moves. Verified: the stub game passes (its slot mutant is now
a SLOTS line), and the blocked-child detection was checked in a standalone program (a child stuck on a
lock reads as futex, a busy one as running).
- **VERSION says 0.2.22.** It had stayed at 0.2.19 through v0.2.20 and v0.2.21. A port that names it
  as a check's identity (ab.sh `--id`) would have reused old runs.
- **The two-run log counts slot-only differences apart.** auditlog.py prints `SLOTS` (each slot
  `moved`, `one run only` or `other value`) for a function whose registers are the same in both runs,
  and exits 1 only on a `DIFF`. The log records a second digest without the slots (`reg=`); older logs
  still read. That port's one DIFF (an iterator's `++` writing into its caller's frame) is
  this.
- **A forked audit child stuck on a lock ends after 0.2 s** with no progress, as `NOCHILD
  why=blocked:CALL progress=N`, instead of after 2 s. A timeout says `progress=N`, so a recursion that
  kept going shows a large count.
- **tools/rewrite/status.py:**
  - LIST `-` counts against GEN's extents.txt (the game's own functions), `--range LO HI` limits it to
    one module, and `--skip` addresses that are registered are "rewritten besides", not "not in the
    list".
  - A rewrite on a function with a hook of its own (gen/funcs.h's `hook_* ... at ADDR`: a stack fix's)
    is "runs on its own hook", not "no hook: it never runs".
- **tools/rewrite/ab.sh `--lock auto`:** where git can't find the main checkout (a worktree copied
  to another machine), the lock is `~/.cache/antistatic/ab.lock` for every check on that machine, not none.

## v0.2.21 (2026-11-14)

From a port's move onto the toolkit. Everything new is off unless a port sets it, so
other games don't change. Regenerate only for the `.translated` stamp. Verified: LEGO Island 2's menu
and town hashes are identical to its baselines, the stub game passes, and so does the self-test.
- **`port_frame_end(frame)`**: a weak hook run after the frame's input and window pump (`game_frame`
  runs before them). A port can pump a network layer's messages there.
- **`PORT_DEFAULT_DISPLAY "fullscreen"`**: start full screen unless options.txt says `display =
  window`. The default stays "window".
- **Rumble**: once a port calls `win_rumble_set`, win.c stops reading the "rumble" option itself, so
  a port's own option with levels (off / low / normal / strong) isn't read as on / off.
- **`host_fullscreen()`** is declared in runtime.h.
- **`PORT_PAD_ALWAYS 1`**: the game's joystick is there in `--test` runs with no scripted controller
  too. It is for a game that lists its devices once and whose baselines have it.
- **`PORT_FILM_SKIP_ANY 1`**: any key or controller button skips a film (`video_skip_press`), not
  only Escape.
- **`PORT_TEST_PEEK_YIELD`**: scheduler 2's empty-PeekMessage turn is its own setting. It is on by
  default with scheduler 2, as before (LIXS, Penguins!); a port whose own scheduler never had it sets 0.
- **`dialog_drive(c, proc, param, msgs, n)`** (runtime/win32/dialog.c): a dialog's own procedure
  run with no window, for a port's `port_dialog_box` in test switches. The stub game tests it.
- **The translator stops on a `REWRITES` address** that isn't the start of a function (padding, the
  middle of one) and on one listed twice. Both used to be accepted silently.
- **The translator touches `gen/.translated`** at the end of every run. Outputs that didn't change
  keep their times, so a Makefile rule on `gen/funcs.h` translated again every time; name the stamp
  instead (docs/GUIDE.md).
- **tools/rewrite/status.py `--nm BINARY`** counts what the build registers (its
  `decomp_entry_<addr>` symbols, made by every DECOMP / DECOMP_SF and the macros over them), and
  `--skip FILE` leaves addresses out (exception funclets). The source search also sees DECOMP_SF.
- **tools/rewrite/ab.sh `--before COMMAND`**: a command run once under the lock before both runs (a
  route that first makes the file it seeds the runs with).
- **tools/rewrite/disfn.sh GEN GAME_FOLDER ADDR** picks the module by address from gen/table.c.
- **The register audit's argument slots, two-run log and isolation** (runtime/rewrite/audit.c, from
  a port's argument-slot sweep; Linux only):
  - `<PORT_ENV>_AUDIT_SLOTS=1` also compares each audited call's own argument slots as the function
    leaves them (a callee-pop function's own, up to 12; a cdecl one's first 6) and the
    return-address slot. MSVC uses them as scratch.
  - `<PORT_ENV>_AUDIT_LOG=PREFIX` is a mode with no fork. A run with `_DECOMP_OFF=all` writes
    PREFIX.a.txt and a run of the rewrites writes PREFIX.b.txt: per function, the call count and
    digests of the inputs and of everything each call left, plus its first 16 calls in full.
    tools/rewrite/auditlog.py compares the two, which also covers functions whose forked child can't
    finish (NOCHILD). Recording only reads, so the pair stays an ordinary A/B run.
  - `<PORT_ENV>_AUDIT_SKIP=addr,...` (and the port's `PORT_AUDIT_SKIP`) leaves functions out. The
    program's entry point, and a call of a function already running further up its thread, are left
    out on their own.
  - A forked child can no longer write anything the parent sees. Its log and write-only files go to
    /dev/null, and a seccomp filter ends it at its first file change, I/O on a shared descriptor or
    signal to another process, reported as `NOCHILD why=sys:NAME`. `<PORT_ENV>_AUDIT_ISOLATE=0`
    switches the filter off.
  - NOCHILD lines now say why (thunk, sys:NAME, timeout, ended).
  - These change what an `_AUDIT=1` run reports (expect more NOCHILD lines); a run without
    `_AUDIT` / `_AUDIT_LOG` is untouched. Verified: the stub game's new checks catch an ebx mutant and
    an argument-slot mutant in both modes, and report nothing on the clean build.
- docs/MOVING.md: when moving, copy the old runtime's `--test` answers into port.h (the test date
  first: a game that seeds rand() from time() makes hundreds of extra calls a frame otherwise). A baseline
  from a port's own runtime can drift by a millisecond of game clock from the first frames because
  the toolkit answers some things as Windows does; that is not a bug to hunt.

## v0.2.20 (2026-11-14)

From 1NSANE's first real Windows run (two Windows-host bugs). Regenerate: gen/funcs.h, stubs.c
and the new gen/funcs_weak.h change [verified: the stub game on Linux, its Windows build read back
from the .exe; LEGO Island 2's menu and town hashes identical to its baselines; 1NSANE on real
Windows: its test race's hashes and runtime calls identical to Linux on all 3200 frames]:
- **A port's own stand-in for an import is a real definition again.** gen/funcs.h declared every
  import weak, so a port file that includes it and defines one (1NSANE's IFC21 stand-ins) made a
  weak definition. On Windows (COFF) a weak reference and a weak definition go to whichever object
  the linker sees first: generated code linked ahead of the port's file (or of the runtime's own
  replaceable defaults, API_WEAK) got NULL, and "is not implemented" at start-up. Now funcs.h's
  prototypes are plain. The generated files (and rewrite.h) include gen/funcs_weak.h, which makes
  their references weak on Linux only; on Windows stubs.c names each import's `missing_<name>` as
  its `/alternatename`, which any definition, weak or strong, beats in any link order. A port file
  that defines an import must not include funcs_weak.h. On Linux a port definition that used to be
  silently weak is now strong: two definitions of one stand-in are now a link error instead of
  the first one linked winning.
  - The Windows fallback needs LLVM's linker (lld: Zig, or `-fuse-ld=lld`); GNU ld doesn't take
    `-alternatename` [guess: not tried].
- **GetProcAddress finds the runtime's stand-ins by a name table**, not the program's exports. Zig
  0.17 drops `-Wl,--export-all-symbols` (and `-rdynamic`) for Windows, so a stand-in the program
  didn't import (1NSANE's IsProcessorFeaturePresent) wasn't found there and the call went
  differently than on Linux. Every API / API_WEAK / HOSTFN now also puts its name and function in
  the `api_names` section (`.apinm` on Windows), and `api_by_name(name)` searches it; dlsym stays
  as the fallback for a function made without the macros. No link flag is needed on either system.
- The stub game imports a DLL only its port stands in for (port/ext.c, defined in a file that
  includes funcs.h), a replaceable default (GetPriorityClass), and calls a stand-in it didn't import
  through GetProcAddress; its Windows build links without `--export-all-symbols`.

## v0.2.19 (2026-11-14)

**Moving a pin past here:** rewrite.c now calls `rewrite_audit_wrap`, so a port compiling only some
of runtime/rewrite/*.c must add `runtime/rewrite/audit.c` (or compile all of them, as
docs/REWRITE.md says), or the link fails. (Note added at v0.2.42.)

From the first port rewritten to the end (every function hand-written C);
everything is off unless a port asks (a flag, an environment variable, a PORT_ setting or a build
config key), so other games' output doesn't change [verified: LEGO Island 2's menu and town hashes
identical to its baselines; its translation identical]:
- **The per-function check** (`--fncheck SPEC`, runtime/rewrite/fncheck.c + fncheck.h): at a frame
  the spec names the process forks twice from one state, the translation and the rewrite each run
  one call to its return, and memory, registers, the x87 stack, argument and return-address slots,
  runtime calls and a C++ throw are compared. Spec lines, C cases (FNCASE), names a port gives
  (FCNAME), `PORT_FNCHECK_NEW` / `PORT_FNCHECK_STRING_ASSIGN`, `frame 0` (before the entry point),
  `<PORT_ENV>_FNCHECK_OUT`; `tools/rewrite/ab.sh --fncheck SPEC`, `tools/rewrite/fncheck_all.sh`.
  docs/REWRITE.md "Checking a function no route reaches". The core's side: weak calls from main.c
  (the frame loop, fatal, the API trace, `--fncheck`), eh.c (RaiseException) and sync.c (a forked
  child takes no turns); mem.c's `mem_walk` / `mem_heap_block`; `api_trace(c, nargs)`.
- **The register audit** (`<PORT_ENV>_AUDIT=1`, runtime/rewrite/audit.c): every rewrite's eax, ebx,
  esi, edi, ebp, esp, return address and x87 stack against the translation's on sampled calls of an
  ordinary run (`_AUDIT_OUT`, `_AUDIT_ONLY`).
- **runtime/rewrite/orig_frame.h** (calls at the original's depth: OCALL / AT / ATP, vmethod,
  tail_call, eh_open / eh_enter / eh_leave / eh_close, EH_STATE; with OP_DELETE and the MSVCP60
  names, MSVC 6's deleting destructors and inlined std::string) and **eh_owner.h** (EH_OWNER: a
  rewrite of a function with catch blocks). rewrite.h: `DECOMP_SF`, `Saved` / `save_regs` /
  `restore_regs`, `EARG`, `x87()`, `fcom_ax`, `hook_inner_register`, `rewrite_switch_off`.
- **`INNER_HOOKS`** (build config, off unless set): a function both hooked and rewritten gets its
  rewrite inside the port's hook. Regenerate only if you set it.
- **ab.sh**: compares how both runs ended (`differ at exit`), `grep -a` / `diff -a`, the translation's
  run cached per id (the last four), SDL's offscreen driver with no display, a default lock shared
  across a repository's worktrees (`--lock none` for none), old hash files removed before a run.
- docs/REWRITE.md: floats (x87(), the denormal rule, x87 regression cases), C++
  exceptions with catch blocks, the batch process and the traps that cost the most.

## v0.2.18 (2026-11-14)

From Nitronic Rush:
- **Discovery 7x faster where code-section pointers abound, same result**: a cached body is checked
  with set operations (a body asking about thousands of addresses was checked one at a time in
  Python, a billion checks); a code-pointer candidate inside known code is turned away before it is
  walked, and a candidate's walk stops at its first error (one error decides, only the first is
  kept; such a cut-short walk isn't cached). D3DCompiler_43's 9,568 candidates each walked about
  59,000 instructions. [verified: every function, rejected pointer, log line, no-return answer and
  table end identical on LEGO Island 2, Stranglehold and Nitronic Rush (to the end of the
  CODE_SECTION_POINTERS reruns); Nitronic Rush 427 -> 57 s]
- **Raw input**: RegisterRawInputDevices for the keyboard and mouse, WM_INPUT for each key, mouse
  movement, button and wheel turn, GetRawInputData in Windows' 32-bit layout, RIDEV_NOLEGACY honoured
  (weak; nothing changes for a game that doesn't register).
- CoCreateInstance takes CLSID_DirectSound8 and IID_IDirectSound8 (Wwise): the same object.

## v0.2.17 (2026-11-13)

From LEGO Island 2's move (its own sound and input code replaced by the toolkit's):
- **MP3 streams**: an MP3 frame cut short at the end of the data is left out, as Miles' decoder
  and dr_mp3 do; FFmpeg concealed it as a frame of near silence, so every one of LEGO Island 2's
  speech lines ended 1,152 samples late [verified: LECSIN01 288,000 samples from dr_mp3, 289,152
  from FFmpeg, identical but for the last frame, 50 of 156 bytes]. **Changes** any port whose MP3
  data ends in a partial frame (by that frame, at the end).
- An empty `<PORT_ENV>_PAD` is no scripted controller (it gave keyboard test runs a joystick).
- `PORT_MILES_OWN_STRINGS`: Miles keeps its own copy of the redist folder name and its error text
  out of program memory until AIL_last_error; `PORT_MILES_COPY_SAMPLES`: a sample's voice is made
  when its file is set, from a decoded copy in program memory, and a start rewinds it. Both are
  li2-native's heap behaviour (its recorded runs); the sound is the same either way.
  [verified: LEGO Island 2's branch on these: menu 30/30 hashes identical, town identical through
  frame 6180 (was frame 60 in memory, 2160 in drawing)]
- `IMPORT_ALIASES` keys in any case; a key naming a module or import the build doesn't have stops
  the translation. runtime/d3d9/README.md: `--hidden` for headless DXVK runs, and pacing
  frame-numbered hidden scripts.

## v0.2.16 (2026-11-13)

**Direct3D 9 through DXVK** (`runtime/d3d9/`, `tools/d3d9/`, `third_party/dxvk/`; its README):
Enter the Matrix's bridge (the program's COM objects over DXVK's native Linux build, locks through
copies in program memory, the generated vtables from `tools/d3d9/d3d9gen.py`), with Stranglehold's
additions as settings (`PORT_D3D9_KEEP_ASPECT`, `PORT_D3D9_KEEP_LOCK_COPIES`) and the game's parts
as hooks (`port_d3d9_renderer`, `_screen`, `_viewport`, `_render_state`, `_ui2d_layout`,
`_dxvk_config`, `_buffer_host`). Settings `PORT_D3D9_ANTIALIASING`, `_ANISOTROPIC`,
`_SHADOW_FILTER`. The device lock is let go around `frame_presented` (both Presents), so --test's
turn can't stick. DXVK's source isn't included: `tools/d3d9/build_dxvk.sh` builds 3.1.1 with the
port's patch set (soft-shadows, depth-texture, live-options, texbem, windowed-gamma).
[verified: d3d9gen.py's output byte-identical to ETM's; ETM's lobby route (6,910 frames) with the
toolkit's bridge: 691 hashes identical to its own; LA Rush's patched DXVK reproduced file for file]

## v0.2.15 (2026-11-13)

From Nitronic Rush:
- **XInput 1.3** (`runtime/win32/xinput.c`, weak; 1.4 and 9.1.0 by name too): XInputGetState /
  SetState / GetCapabilities on the core's controllers: user 0 every controller merged and always
  there, users 1-3 players 2-4 in split screen; rumble on the player's game joystick
  (`pad_game_joy`). Not yet run with a controller.
- Weak Windows calls: CreateEventW, WaitForMultipleObjectsEx, SleepEx (no queued routines run),
  GetSystemWindowsDirectoryW, GetWindowInfo, DragAcceptFiles.
- `IMPORT_ALIASES` (build config): a module's imports from one DLL linked to another translated
  module (d3dx9_43.dll's msvcrt.dll imports to the translated msvcr90.dll).

## v0.2.14 (2026-11-13)

- `PORT_POLL_CLOCK_TIMERS_ONLY`: an empty PeekMessage reads the clock only when the thread has a
  timer. In --test each reading moves the virtual clock; 1NSANE's runtime read nothing there, and
  with the toolkit's reading its timeGetTime drifted a millisecond over the menus and its race
  picked other opponents (frame 1714). Off by default: LEGO Island 2's runs were recorded with the
  reading [verified: with it on, li2's memory differs from frame 180; off, identical].

## v0.2.13 (2026-11-13)

Translator, from Nitronic Rush (Visual C++ 2008 with MSVCR90 / MSVCP90 translated):
- **Exported data is not a function**: a decorated export whose name doesn't end in `Z` (vtables
  `??_7...@@6B@`, static members) is left out of the entries (MSVCP90 keeps 128 in `.text`; they
  made false functions and calls to nowhere). **Changes the translation** of a program with such a
  DLL: Stranglehold's msvcp80 has 317, msvcr80 5 (false functions disappear). LEGO Island 2's
  translation is byte-identical.
- `CODE_SECTION_POINTERS` (build config, opt-in): function pointers a code section holds in its
  own data (MSVCR90's initializer tables), discovery rerun until nothing new (Nitronic Rush's patch).
- C++ exception tables of Visual C++ 7 / 2005 / 2008 (FuncInfo magics 0x19930521 / 0x19930522,
  `__CxxFrameHandler3`, a translated msvcr70-90's handler found by name). Not yet: SEH4 scope
  tables and the C runtime's continuation jumps.

## v0.2.12 (2026-11-13)

From 1NSANE's move:
- `PORT_HASH_STACKS`: memory hashes include thread stacks again (since v0.2.0 the toolkit leaves
  them out; 1NSANE's baselines and lockstep have them).
- `PORT_PAD_JOYS 0`: one joystick per controller, made when it is plugged in and named after it,
  gone when it is unplugged, each with only its own state and rumble (1NSANE's model: the game
  lists what is plugged in, none in its --test runs). tests/pad checks it.
- `PORT_NO_BUILTIN_MODS`: the built-in <user>/mods overlays off (1NSANE has its own fan content
  and the same option name).
- Hooks: `port_file_extra(rel, out, n)` (a port's file where the game folder has none: reads and
  a find's folder), `port_find_extra(rel, names, max)` (names added to a folder's listing, after
  the game's, first spelling wins), `port_d3d7_surface_gone(s)` (before a surface's memory goes:
  1NSANE's far-draw tiles), `port_async_key(vk)` (GetAsyncKeyState's answer for a key: 1NSANE's
  Escape after a film skip), `font_atlas` (font.h declares it; weak default none), `ifc_exit` weak.
- The pipeline cache and the draw summary are written at exit again: process_exit ends with
  `_exit`, so their atexit never ran; now on the runtime's exit list (once).

Behaviour that moves --test memory against a port's old runtime (since v0.2.0, from the other
games; listed here as 1NSANE found them): the first window's HWND 0x10020 (was 0x10010);
LoadBitmapA, GetDC and DC handles are GDI objects; more window messages at create, ShowWindow,
SetWindowPos and SetWindowText, in Windows' order; PeekMessage's empty poll reads the clock
(+0.001 ms of virtual time); CreateThread with size 0 takes the exe's stack reserve;
GetProcAddress("IsProcessorFeaturePresent") resolves; larger DirectSound / DirectInput vtables and
classes made at start-up shift heap addresses; the --test scheduler yields at every frame.

## v0.2.11 (2026-11-13)

- **The Windows host** (`runtime/win/`, `tools/win/`; its README): 1NSANE's and Penguins!'s
  compatibility layer for building the runtime as a 64-bit Windows program, with the game's names
  taken from port.h. New in it: `pwrite`, `dlsym(RTLD_DEFAULT)` (with
  `-Wl,--export-all-symbols`). In the core: the time-zone offset on Windows (no tm_gmtoff), and the
  --test watchdog's per-thread stacks left out there (Linux signals). msvc_shim.c for DLSS.
- tests/stubgame/run.sh builds the stub game for Windows too when given SDL3_WIN (and ZLIB_WIN,
  VK_INC): translated, compiled and linked with Zig [verified: a 3.8 MB PE32+ x86-64 program; not run
  here, no Windows]; the Linux stub game unchanged.

## v0.2.10 (2026-11-13)

- `PORT_CMDLINE_EXE`: the program's name as GetCommandLine shows it, when a port's recorded runs
  had another than PORT_EXE (li2-native's runtime said "Game.exe" for "LEGO Island 2.exe").
  [verified: LEGO Island 2 on v0.2.10's whole core and renderer, the harness's own kernel32.c edit
  gone: menu 30 + town 160 hashes identical to li2-native's baselines]

## v0.2.9 (2026-11-13)

**Off-Road's renderer features in runtime/d3d7**, each off unless the port turns it on (so Off-Road
can drop its own renderer copy): `PORT_D3D7_SCREEN_WORLD` (a screen-space world told from the
HUD; Off-Road's widescreen spreading; hook `port_ws_hud_centre`), `PORT_D3D7_SSAO`,
`PORT_D3D7_BAKED_LIGHT` (baked lighting and car lights: vk_light_load / _view / _cars),
`PORT_D3D7_WATER` (vk_water_step / _draw; hook `port_d3d7_alpha_gain` for the spray),
`PORT_D3D7_SCENE_OBJECTS`, `PORT_D3D7_DEPTHBACK`, `PORT_D3D7_STENCIL`, `PORT_D3D7_AA_MODES` (2x,
FXAA), `PORT_D3D7_TAA_HP_HISTORY`, `PORT_D3D7_SMOKE_STEADY`, `PORT_D3D7_FAST_SCREEN`, value
settings (`PORT_D3D7_SMOKE_DENSITY`, `PORT_D3D7_UNITS_PER_METRE`, `PORT_DEFAULT_TEXTURES`,
`PORT_DEFAULT_COLOUR_PRECISION`), A/B record hooks `port_d3d7_ab_mode` / `_record`; shaders
ssao.frag, depthback.frag, water.vert/.frag, water_sim.frag; draw.frag/.vert and taa.frag are
Off-Road's (supersets: the new paths run only on flags the C side sets for those features).
Off-Road's game code (water.c, light.c, native_land.c, menu.c, widescreen.c, taa.c) stays in it.

Shared fixes that came with it:
- **SetSurfaceDesc** takes WIDTH / HEIGHT / PITCH / PIXELFORMAT / CAPS with LPSURFACE (Test Drive
  6's texture manager: all 254 calls in a race load were refused and its levels drew black since
  v0.2.0). LPSURFACE alone runs the old code.
- A palette's SetEntries marks 8-bit surfaces changed (their GPU copies kept old colours).
- A sampled depth+stencil image gets a depth-only view.
- A render target that changes shape is cleared for one frame instead of the old picture stretched.
- Smoke puffs at equal distance sort by serial (they swapped frame to frame).
- tnl.vert.motion.spv.h was stale (built before the 36-byte form): regenerated.
- Pipeline cache keys that collided in Off-Road's copy (SSAO vs TAA, water vs AO) are distinct.
[verified: LEGO Island 2 menu + town hashes identical; its 12 menu and town frames, scene and
presented, byte-identical before and after on lavapipe; stub game; argc 820/0; all files compile
with no settings and with every PORT_D3D7_* set]

## v0.2.8 (2026-11-13)

- **The D3D7 renderer draws the game's fog by default** (`PORT_DEFAULT_FOG`, 1): only LEGO Island
  2 left it out (its port sets 0); Test Drive 6, 1NSANE and Hard Truck 2 always drew it, and on
  the toolkit their skies lost the haze toward the horizon. **Changes the picture** of ports on
  v0.2.x's renderer that use fog.
- `PORT_DEFAULT_RESOLUTION`: the render resolution when options.txt doesn't say ("original", or
  "native" for Hard Truck 2 and Off-Road, whose renderers drew at the window's size).
  [verified: HT2's yard frame 1500 with both set is pixel-identical to its own renderer's]

## v0.2.7 (2026-11-13)

From Hard Truck 2 (lockstep identical on 1800 frames on v0.2.3):
- **Widescreen by the projection**: a port's `widescreen_projection_x(x, perspective, vp_x, vp_w,
  rt_w, y)` widens the game's projection when it sets one (SetTransform, MultiplyTransform) or a
  viewport; GetTransform gives the program its own back (HT2's and Off-Road's way; without the
  function nothing changes, as for LEGO Island 2, which widens in the renderer).
- `<PORT_ENV>_DUMP_LIST=300,420,600`: frame dumps of only those frames.

## v0.2.6 (2026-11-13)

- Every call in `winmore.c` is a weak default now (16 were strong: a port with its own stand-in,
  Stranglehold's IPHLPAPI GetAdaptersInfo, failed to link).

## v0.2.5 (2026-11-13)

From Enter the Matrix's move:
- `--trace-calls FILE`: every call into the runtime by name, imports included (they go through
  their thunks for that run), one line each: frame, call number, return address, name.
- translator/memdiff.py takes `--max N` and `--skip LO-HI` (ETM's rate.sh: memory only drawing
  writes left out); a skipped range is cut out of a differing range, not just dropped.
- `PORT_MILES_NO_FFMPEG`: Miles streams without the FFmpeg libraries (WAV, and Ogg Vorbis when the
  port links stb_vorbis_impl.c), for a self-contained build such as a Steam Deck copy.
- REWRITE.md: compile rewrites with `-mfpmath=387 -fexcess-precision=standard` since v0.2.1's
  x87 translation (ETM's A/B differed without them). Ports with rewrites (L.A. Rush) take note.

## v0.2.4 (2026-11-13)

- `PORT_PAD_AXES` / `PORT_PAD_BUTTONS`: the joysticks the game sees (default 6 and 16; TDOR3's
  were 4 and 8, which its controls page lists).
- port_example.h: `PORT_CPU_COUNT` can change a game's behaviour (TDOR3's sr.dll re-measured its
  timer on every start with more than one CPU).
- Reference builds: fprem / fprem1 are among the instructions reference runs compute on the host
  (D9 F8, D9 F5); a port's Unicorn handler must take them (TDOR3 looped in MSVCRT's sin/cos).

## v0.2.3 (2026-11-13)

Three Windows answers Hard Truck 2 found wrong:
- LoadBitmapA makes a bitmap object from the resource (GetObject gives its size and depth; its
  pixels are the resource's). It returned the resource's data address, which HT2 sized its first
  window by (1818435698 x 1953391977).
- mmioOpenA with no name and no memory buffer is refused (MMIOERR_FILENOTFOUND) instead of
  crashing on a NULL path (HT2's LOAD.RES sounds).
- MultiByteToWideChar / WideCharToMultiByte with no source, or a length of 0, fail with
  ERROR_INVALID_PARAMETER, as Windows does, instead of reading address 0.

## v0.2.2 (2026-11-13)

- **Weak defaults reach the program** (found by TD6): an import the runtime provides with
  `API_WEAK` (49 of them: GetSystemInfo, GlobalMemoryStatus, mciSendCommandA, the INI and network
  calls...) lost to the translator's own weak "not implemented" stub whenever the generated objects
  linked first, and the game stopped at its first call. Imports are now weak references (a port's
  strong definition beats the runtime's weak default, which beats nothing) and each has a
  separately named `missing_<name>` stand-in used only when nothing defines it. **Regenerate**:
  the translator writes the new tables (`ImportEntry` gains `missing`); an old translation still
  links but keeps the bug.
- `PORT_CPUID_FEATURES`: CPUID leaf 1's edx (TD6: 0x0383fbff, the APIC bit).
- The software rasteriser's functions are `swrast_init` / `_active` / `_present` / `_draw` (found by
  TD6): `swr_init` is FFmpeg libswresample's, and with -rdynamic the renderer's replaced it, so
  Bink and WMA sound (and the toolkit's own Miles MP3/Ogg path through swresample) failed silently.

## v0.2.1 (2026-11-13)

Give-backs from the first ports moving over.
- **x87 maths stays x87**: every translated fadd/fsub/fmul/fdiv result passes
  through `X87()` (cpu.h), so GCC can no longer fold a float operation into one SSE instruction
  that rounds once where the game's x87 rounds twice (results in the float denormal range; hundreds
  of sites in one port). **Regenerate.**
- **Files** (from L.A. Rush): a file opened for writing counts as existing only if it did
  (CreateFile's ERROR_ALREADY_EXISTS was set for every new file written with CREATE_ALWAYS or
  OPEN_ALWAYS, so games that write a new file's header only when GetLastError is 0 never did:
  L.A. Rush's LOAD PROFILE then deleted the save); every CreateFile disposition answers as
  Windows' (CREATE_NEW fails if it exists, OPEN_EXISTING and TRUNCATE_EXISTING if it doesn't,
  an unknown one with ERROR_INVALID_PARAMETER); CopyFile's fail-if-exists sees the game folder.
- **Exception and longjmp resumes** (from Penguins!): the resumed function reads its CPU record
  and frame back from the runtime after `__builtin_setjmp` (Clang, and so Zig's Windows builds,
  keeps neither across the jump). **Regenerate** for games with C++ exceptions or setjmp.
- Calls through an **ATL window thunk** the program wrote into memory go to its window procedure
  with the object, as the thunk would (from Penguins!).
- **More Windows calls** (`runtime/win32/winmore.c`, from Penguins!): GetFileAttributesExA,
  StringFromCLSID, CoInitializeSecurity, CoSetProxyBlanket, GetErrorInfo, CreateDispTypeInfo (a
  stand-in ITypeInfo), ShellExecuteExA (refused), WININET and iphlpapi (offline, nothing sent),
  security-descriptor and ACL calls (succeed).
- DirectInput 8's wide interfaces (IDirectInput8W, IDirectInputDevice8W) answer as the same objects
  (from Stranglehold: Unreal Engine 3 asks for them for its mouse).
- `port_option_help`: a port's own comment lines for its keys in options.txt.
- **FMOD 3 stand-in** (`runtime/music/fmod.c`, from Penguins!), with `third_party/stb_vorbis`
  (public domain).
- **Test scheduler 2** (`PORT_TEST_SCHEDULER 2`, from LEGO Island Xtreme Stunts and Penguins!):
  an empty PeekMessage gives the other threads their turn (a loading thread spinning on it froze
  --test runs before frame 1), and a wait that times out moves the virtual clock by its timeout
  (up to 50 ms) while the main thread waits without one for what only time brings (a sound
  stream's notification). Both games' own runtimes had them; scheduler 1 is unchanged.
- `PORT_TEST_SOUND_AT_FRAME` (from Off-Road): in --test runs the mixer's clock moves only at
  presented frames, as Off-Road's lockstep was recorded with. Off by default.
- **Game frames** (from ETM): `port_frame_ticks` says how many game updates a presented frame ran
  (default 1); scripted input, --hash, --dump-memory, --frames and game_frame count those
  (`game_frames`), so runs at different drawn rates meet the same input on the same update.
- **Full-screen size** (from ETM): the window's size is read once the compositor has made it full
  screen (up to a second), else the screen's own desktop size is taken (Wayland at 2x scaling
  gave a 16:9 size on a 16:10 panel).
- **Scripted controller during films**: "~N:" inputs count a port's film ticks
  (`pad_film_tick`, called while a film plays and no frames are presented).
- **Modules in a subfolder** (from Stranglehold): a MODULES file may be a path relative to the game
  folder (`Binaries/Game.exe`), matched without regard to case; imports still match its bare name.
- More hooks and helpers: `drop_key_messages` (TDOR3: key presses queued during a skipped movie
  thrown away), `loader_module_at` (each module's place, for a reference build), `win_key` (a
  port's key to DirectInput and the window, weak), `mem_heap_ops`, `mem_ranges` and
  `cpu_dead_stacks` (Off-Road's native-code memory check), a first-start options.txt listing every
  option at its default (`port_option_default`, Off-Road), and `PORT_TEST_SCHEDULER`'s comment.
- **Discovery about a third faster, same result** (from Stranglehold): a function's traversal is
  kept with the answers it depended on (which addresses are entries, which callees never return)
  and reused while they hold (Stranglehold's 73,683 functions were traversed 636,238 times), and
  "inside another instruction" is looked up instead of built as a set of every such byte.
  [verified: every function's body, calls, tails, tables, errors, the rejected pointers, the
  no-return set and table ends identical on LEGO Island 2 and Stranglehold; 96 -> 61 s and
  24 -> 16 s] Only repeated lines are gone from its log.
- **Hard Truck 2's blockers**: DirectDraw takes NORMAL with EXCLUSIVE | FULLSCREEN as exclusive
  (Windows does); `PORT_DDRAW_8BIT` lists 8-bit palettized display modes (HT2's menus, Off-Road);
  DirectDrawEnumerateA; a destroyed main window hands the host window to another visible
  top-level window first (full screen kept on Wayland); the renderer snapshots `ws_world_aspect`
  with each frame (it read it live on the render thread: widescreen flickered); INI section and
  key listing (GetPrivateProfileStringA with no section or key, from HT2's inifile.c);
  `sound_stream_volume` / `sound_stream_playing`; `PORT_D3D_NATIVE40`: renderer = native the HT2
  way (the game's own layouts packed into the 40-byte record, a second texture set in the colour
  slots: tnl.vert flags.x bit 64); test clocks `PORT_TEST_CLOCK_PER_THREAD` (other threads'
  readings don't move the shared clock) and `PORT_TEST_SLEEP_MOVES_CLOCK` (scheduler 1).
- **Widescreen placement settings** (from TD6): `PORT_WS_BG_DEPTH` (a quad over the whole view
  with depth on is the frame's background too), `PORT_WS_HUD_MIDDLE` (the HUD's middle band stays
  centred), and the `port_ws_menu_place` hook (a port's own placement of its menus' draws).
- **The machine the program sees** as port settings (from TD6): `PORT_CPU_COUNT` (4 in --test
  runs whatever the host, unless set), `PORT_CPU_REVISION`, `PORT_CPUID_SIGNATURE`,
  `PORT_CPUID_MAX_LEAF`, `PORT_QPC_HZ`, `PORT_MEMORY_MB`, `PORT_COMPUTER_NAME`, `PORT_TEST_DATE`;
  GetSystemInfo and GlobalMemoryStatus replaceable (weak). Defaults are the answers v0.2.0 gave.
- **Network stand-ins** (from Penguins! and another port): bind, listen, accept, recv, select,
  htonl / ntohl / ntohs, gethostname, gethostbyname (this computer, 127.0.0.1), WSACleanup, and
  WS2_32's same ordinals; `PORT_WINSOCK_STARTS` lets Winsock start with every socket failing.
- **More Windows calls** (weak, so a port's own copy still wins): GetPriorityClass,
  SHGetSpecialFolderPathA (CSIDL_APPDATA, COMMON_APPDATA, PERSONAL), CoFreeUnusedLibraries,
  GetRunningObjectTable, CreateItemMoniker, the COMDLG32 file and colour pickers (cancelled), AVI
  capture (AVIFIL32, unsupported) and VideoForWindowsVersion (MSVFW32 #2), from LIXS and Penguins!.

## v0.2.0 (2026-11-12)

The first version ports move onto ([docs/MOVING.md](docs/MOVING.md)): the runtime core, controllers,
DirectInput, DirectSound, the music libraries and the DirectDraw / Direct3D 7 renderer.
**Regenerate** when moving (several translator fixes change generated C).

- Translator: `CXX_FRAME_HANDLERS`, `SEH_HANDLERS`, `SEH_PROLOGS` build-config settings for a C
  runtime linked into the program (from Penguins!).

- **Runtime core** (`runtime/win32`): the Windows stand-in, from Stranglehold's port made
  game-agnostic (`port.h`, port hooks with weak defaults), with every Win32 function any of the
  eleven ports implements. Builds and runs LEGO Island 2 with the same memory and picture as its
  own port in its hash runs. Port hooks: `port_graphics_init`, `port_peek_hook`, `port_cocreate`,
  `port_frame_time`, `port_file_hidden`; input/video defaults in `port_defaults.c`.
- Translator tables: the module table carries every code range and the base a module was linked
  for (the loader applies relocations when they differ); `link_table` replaces `import_links`.
  **Regenerate.**
- Translator: padding NOPs with prefixes (`nop word ptr cs:[eax+eax]`, from GCC/LLVM/ICC) no
  longer make discovery reject a function.
- **Rewrite track** (`runtime/rewrite`, `tools/rewrite`, `docs/REWRITE.md`): replacing translated
  functions with hand-written C, from Enter the Matrix's and L.A. Rush's ports and one more.
  `REWRITES` build-config list, `DECOMP` registrations switched by option `decomp` or
  `<PORT_ENV>_DECOMP_OFF`, `STACKFIX` (registered like `DECOMP`, no central install list),
  coverage builds, the A/B check (`ab.sh`, any port), status, class and module maps. The stub
  game tests a rewrite both ways. The runtime core runs `rewrite_install` after `game_init` and
  `coverage_write` at exit when they are linked.
- **Static TLS** (`runtime/win32/tls.c`): modules with a TLS directory get their index and a
  per-thread copy of their block at `fs:[0x2c]`; TLS callbacks run as on Windows XP (a DLL's
  before its DllMain, the program's before its entry point, every thread's start and end). The
  translator finds the callbacks as functions. The stub game tests it. None of the current
  ports' translated modules have static TLS. **Regenerate** only for a game that has it.
- A thread polling with `Sleep(0)` sleeps a millisecond from its fourth call in a row instead of
  holding a core (pcrecomp; `<PORT_ENV>_SLEEP0=yield` keeps the yield; not in test runs).
- **DirectDraw / Direct3D 7 renderer** (`runtime/d3d7`): LEGO Island 2's copy of the DirectX 6/7
  ports' Vulkan renderer, with Off-Road's render-thread waking per batch, state sent only when it
  changed, 8-bit palettized textures and the mouse mapped into the picture. LEGO Island 2 built on
  it keeps its menu and town hashes identical (draw stream and memory, render thread off and on).
- Runtime interface for renderers: the mouse mapped into the game's picture (`port_window_to_game`,
  `port_game_per_pixel`, HiDPI), `host_game_size`, an exit list (`runtime_at_exit`, `port_at_exit`)
  before `_exit` (no host teardown while other threads still use the driver), and
  `options_changed` (frame limit, controllers, window, then `port_options_changed`).
- **Music libraries** (`runtime/music`): the Miles Sound System 6.0 / 6.5 stand-in from Off-Road
  (85 calls) with LEGO Island 2's MIDI, DLS and sequence calls, its volume classes made port hooks;
  MIDI through DLS (`midi.c`); DirectMusic 6/7 and 8 stand-ins from LEGO Island 2 and Xtreme
  Stunts; GothicKit's DirectMusic library in `third_party/dmusic` (MIT). The stub game plays a
  Miles sample and checks it plays and ends.
- **DirectSound** (`runtime/win32/dsound.c`, `sound.h`): the ports' newest line (LEGO Island
  Xtreme Stunts, Off-Road, Enter the Matrix): buffers in program memory, the mixer,
  3D sound, notifications, duplicates, the runtime's own voices; Stranglehold's device enumeration
  and capture entries (Unreal Engine 3 needs them); DirectSoundCreate / 8 by name as well as by
  ordinal. The stub game plays a buffer looping and once and checks its positions.
- **Controllers and DirectInput** (`runtime/win32/pad.c`, `pad.h`, `dinput.c`): every controller
  merged into the joysticks the game sees, whenever it is plugged in; Windows' own Xbox-controller
  layout (or a port's, `port_pad_send`); split screen for up to four players tied to each
  controller's identity (`pad_players`), with players a game has no joystick for readable by the
  port (`pad_player_state`); rumble on every controller of a joystick; the scripted controller
  (`<PORT_ENV>_PAD`, with JOIN / 2. / PLUG / UNPLUG / +N). DirectInput 3 to 8 from the LEGO Island
  Xtreme Stunts line, plus DX7's DirectInputCreateEx, the device-to-controller query
  and the wheel-force rule from Off-Road and Hard Truck 2. `tests/pad` (`make padtest`).
- Upstream sweep 2 (pcrecomp, SwARc, recomp-kit to 2026-11-10): the x87 control word reads back
  as hardware keeps it (bit 6 set, bits 13-15 clear); VirtualFree(MEM_RELEASE) fails unless it
  frees a whole reservation; waiting on handle 0 or a closed handle fails as on Windows, and
  closing a handle wakes its waiters; thread stacks follow the exe's stack reserve; `jcxz`;
  SSE/MMX registers reloaded when a function resumes after an exception or longjmp.
- **Pacing** (`runtime/win32/pacing.c`): the frame limiter sleeps to each frame's start on a grid
  of exact periods with 1 us timer slack instead of spinning its last half millisecond (SwARc's
  FramePacer); a game waiting by reading GetTickCount / timeGetTime in a tight loop sleeps to the
  next millisecond instead of holding a core (recomp-kit's idea, stricter: only when no other
  Windows call happens between the reads; off under --test and with `PORT_CLOCK_SPIN 0`).
  `tests/pacing`: 60 fps within 1%, 0.2% CPU waiting, no burst after a late frame, a spinning
  wait at 1.6% CPU instead of 100%.
- **Stack-pop check** (`tools/check_api_argc.py`, `make argc`): all 765 Windows functions the
  runtime provides match mingw-w64's 32-bit definitions (ordinal imports checked by the name
  their comment gives). The eleven game ports' own runtimes match too.
- **Game instruction forms** (`tests/selftest/forms.py`, `make forms`, SwARc's idea): every
  distinct instruction form a translated game uses becomes a self-test case, run on random inputs.
  LEGO Island 2 (758 forms), LEGO Island Xtreme Stunts (1,269) and L.A. Rush (3,359, with its
  SSE2) all match.
- **setjmp / longjmp**: the translator makes the place after every `setjmp` call a resume place
  (imports and translated C-runtime exports by name, a static copy by build config `SETJMP`);
  `longjmp` imported from MSVCRT/MSVCR70/71/80 is the runtime's (SEH frames unwound, `_setjmp3`'s
  unwind function, registers restored); a translated `longjmp` (or `LONGJMP`) ends in a nonlocal
  jump, which now resumes instead of stopping. The stub game tests both. **Regenerate** for games
  that use setjmp.
- **SSE2**: double-precision arithmetic, compares, conversions, moves and shuffles, the byte
  shifts and half shuffles (`translator/simd.py`, host intrinsics). Fixed: SSE2's `movsd` and
  `cmpsd` were translated as the string instructions of the same name. The stub game is now built
  for an SSE2 CPU. The runtime still reports a CPU without SSE2 (a port can change that once its
  game's SSE2 code translates). **Regenerate** for games with SSE2 code.
- Translator: 16-bit `push`/`pop`, byte and word `xadd`/`cmpxchg`, and `cmpxchg8b`.
- Self-test: pcrecomp's 375 lifter cases (MIT), each from its fixed start state; the x87
  exception bits left out of the comparison as in lockstep (the emulator does not model them);
  new SSE2 cases. 47 own cases and 375 pcrecomp cases match.
- Self-test: x87 arithmetic under eight control words (precision and rounding), 45 cases.
- `tests/stubgame`: a small Windows program of our own through the translator and runtime core.
- `tools/coverage`: function-discovery scoring (pcrecomp's, MIT) and an adapter for our output.
- `tools/audit_public.py`: run by `tools/export_public.sh` on the public tree and its history.
- Runtime core, for the C++ line (from lixs-native and its sibling ports): C++ exceptions and SEH
  (`eh.c`); an optional game CD drive (`PORT_CD_LABEL`); DLLs loaded later (build config
  `load='dynamic'`) start at LoadLibrary; DLL_THREAD_ATTACH to started DLLs for each new thread
  (changes memory in games with translated C runtime DLLs); mouse helpers and GetCursorPos /
  SetCursorPos that track the cursor; `--shot FRAME FILE` (`+N` from the scripted input's mark).


The first version: the translator and the CPU core, merged from the three lines of the ports
that grew from 1NSANE: Recompiled (the DirectDraw/Direct3D 7 line, the Direct3D 9 line and the
C++ exceptions/Direct3D 8 line). **Regenerate.**

Translator, compared with each line's own:
- One driver for every build: several modules at any base (relocations applied), C++ exception
  tables, exports and ordinals as entries, case-insensitive file names, files rewritten only when
  they change. Reads both the dict and the older tuple form of `MODULES`, and single-program
  configs (`SHA256`).
- Every instruction any line supported, in one place: the integer set, string ops, `enter`,
  16-bit `pusha/popa` and `pushf/popf` (ID and AC kept, as CPU detection toggles them), bit tests
  on bit strings, `cmpxchg/xadd/bswap/bsf/bsr/cmov`, `fcomi/fcmov`, SSE and MMX (one
  implementation; the six-instruction MMX of the D3D7 line is gone).
- New: 8- and 16-bit `rcl/rcr`, 8- and 16-bit `idiv`, 16-bit `div`, and the decimal adjusts
  (`daa das aaa aas aam aad`), none of which any line translated.
- Fixed: an 8-bit `div` produced C that doesn't compile (most lines). An instruction running on
  into the next function dropped the flags that function reads (all lines but the newest). The
  x87 stack position was stale in `fnstsw ax` where only AH is read (D3D7 line).
- Discovery: code in any number of code sections (DLLs like Bink have several; the C++ line
  read only `.text`); relocated modules take their code pointers from their relocations only,
  and an immediate counts as an address only where a relocation marks it; the jump-table bound
  search follows the switch value through memory (both lines' versions, which had stopped each
  other from matching when combined); incremental-linking stubs keep their own address; a
  function never starts on alignment padding; a frame-setting prologue is a start wherever it is.
- From the D3D9 line: `MEM_OVERRIDES`, `CONSTS` on narrow literals, `FRAME_STEPS`,
  `FRAME_COUNTS`, `TICKS`, `CODE_PATCHES`, `ICALL_FPU`, `API_CALL` on import calls, capture
  points and the store tracing used for frame-rate checks (off unless built with `-DFTRACE`).

Checked by translating four games with it and with their own translators and comparing every
function: the differences are the changes above.

CPU core (`runtime/cpu`): `cpu.h` and `fpu.c` merged from the three lines; `tables.h` for the
generated tables; `fatal()` is the one thing a runtime must provide to link it.

Self-test (`tests/selftest`): 36 cases, compared against Unicorn on random inputs. It catches
the two inherited translator bugs above when they are put back.
