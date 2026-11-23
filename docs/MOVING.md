# Moving a port onto the AntiStatic Recomp Toolkit

A port that grew its own copies of the translator and runtime moves onto the toolkit by pinning a
toolkit version and deleting its copies, keeping only what is its game's: `port.h`, its build
config, its game code (hooks, menus, widescreen, effects) and its own checks. Each game moves at
its own pace and checks only itself.

## What comes from the toolkit

| Toolkit | Replaces the port's |
| --- | --- |
| `translator/` | `tools/recomp.py`, `codegen.py`, `discover.py`, `fpustack.py`, `simd.py`, `eh.py`, `pe.py`, `x86.py`, `image.py`, `memdiff.py` |
| `runtime/cpu/` | `cpu.h`, `fpu.c`, `tables.h` |
| `runtime/win32/` | the Windows layer: `main.c`, `mem.c`, `sync.c`, `kernel32*.c`, `user32*.c`, `gdi32.c`, `winmm.c`, `advapi32.c`, `ole32.c`, `com.c`, `res.c`, `files.c`, `storage.c`, `eh.c`, `tls.c`, `win.c`, `keys.c`, `options.c`, `capture.c`, `drawhash.c`, `prof.c`, `pacing.c`, and now `dinput.c`, `pad.c`, `dsound.c` |
| `runtime/music/` | `mss32.c`, `midi.c`, `dmusic.c` (as `dmusic7.c` or `dmusic8.c`), `fmod.c`, `third_party/dmusic`, `third_party/stb_vorbis` |
| `runtime/d3d7/` | the DirectX 6/7 renderer: `ddraw.c`, `d3d.c`, `vk.c`, `vkthread.c`, `swrast.c`, `shaders/` |
| `runtime/d3d9/`, `tools/d3d9/`, `third_party/dxvk/` | Direct3D 9 through DXVK (the bridge): `runtime/d3d9.c`, `d3d9_bridge.h`, `d3d9_msaa.c`, `d3d9_inv.c`, `tools/d3d9gen.py`, `d3dasm.py`, `d3dinv_report.py`, the `third_party/dxvk-*.patch` files and the DXVK build steps (`tools/d3d9/build_dxvk.sh`); the game's parts become `port_d3d9_*` hooks ([its README](../runtime/d3d9/README.md)) |
| `runtime/rewrite/`, `tools/rewrite/` | the rewrite track (`decomp.h`/`decomp.c`, `ab.sh`, status tools) |

Direct3D 8 (the D3D8 DXVK bridge) and our own `vk_core.c` / `vk_d3d9.c` renderer are not in the
toolkit yet: those ports keep their renderer files for now and move them when the toolkit's lands.
A port with its own renderer beside the D3D9 bridge (Enter the Matrix) keeps it and hands it to the
bridge through `port_d3d9_renderer`. Test Drive Off-Road 3's SurRender driver stays in its port.

## How

1. **Baselines first.** Before changing anything, record the port's own checks on its current
   build: its hash scenes (`--test --hash`), lockstep or function checks, A/B routes, frame dumps.
   These are what the moved port must match.
   Note the old runtime's `--test` answers too: its test date (`PORT_TEST_DATE`; a game that seeds
   rand() from time() goes another way with another date: hundreds of extra calls a frame), CPU
   count and revision, CPUID and clock rates, computer name, whether it starts Winsock
   (`PORT_WINSOCK_STARTS`), and set the same in port.h.
2. **Pin a version.** Add the toolkit as a git submodule (`third_party/antistatic`) at a release
   tag (`v0.2.0` or later), and build from its folders. Moving to a newer toolkit is moving the
   pin, then running the port's checks again. Point the build at it with a variable named
   `ANTISTATIC` (`ANTISTATIC ?= third_party/antistatic`), not `AS`: that is make's assembler.
3. **`port.h`.** Start from `runtime/win32/port_example.h`: `PORT_TITLE`, `PORT_SLUG`,
   `PORT_ENV` (the prefix of the port's environment variables: keep the one it has, e.g. `ORR`,
   so its test scripts still work), `PORT_VROOT`, `PORT_EXE`, `PORT_CMDLINE_ARGS`,
   `PORT_VOLUME_LABEL`, `PORT_ARENA_START`, and the optional ones (CD, screen size, test idle,
   test scheduler, `PORT_PAD_JOYS`, `PORT_CLOCK_SPIN`, `PORT_DMUSIC_SEARCH_DIR`).
4. **Translate with the toolkit's translator** (regenerate) and compare the output with the
   port's own (a function-by-function count and diff). Every difference should be one the
   toolkit's notes explain (`CHANGELOG.md`, and the shared fixes list your ports keep).
5. **Build with the toolkit's runtime** plus the port's own files, deleting the port's copies.
   What was game-specific in those copies becomes a hook (weak defaults in the toolkit; the port
   defines the strong one):
   - start-up and frames: `port_graphics_init`, `game_init`, `game_frame`, `port_runtime_ready`,
     `port_idle_hook`, `port_peek_hook`, `port_frame_time`, `port_at_exit`, `port_options_changed`
   - Windows answers: `port_cocreate`, `port_file_hidden`, `port_dialog_box`
   - controllers: `port_pad_send` (another joystick layout), `port_pad_name`, `port_pad_frame`,
     `pad_players` / `pad_player_state` for split screen
   - mouse: `port_window_to_game`, `port_game_per_pixel` (the D3D7 renderer defines them)
   - options.txt: `port_option_help` (comments for the port's own keys)
   - Miles: `port_miles_class` and `miles_set_class_gain` (volume sliders by sample class)
   - rewrites: `REWRITES` in the build config, `DECOMP` registrations
6. **Check.** Run the port's checks against the baselines from step 1. A difference is either a
   toolkit fix the port didn't have (say which) or a problem: report it to the toolkit (an issue)
   with the check, the first differing frame and what was found. Don't patch the pinned copy
   inside a port; fixes and new pieces go into the toolkit itself, which tags a new version.
6b. **Windows.** A port with a Windows build uses `runtime/win/` (its README) in place of its own
   copy, keeping its icon, manifest and resource script.
7. **Public repos.** A port's public repository can point its submodule at the public
   AntiStatic Recomp Toolkit repository; it must never publish generated C or game files.

## Things that changed under the ports (worth knowing before the checks)

Behaviour the toolkit has that most ports' copies don't (each can move a hash):
- the x87 control word reads back as hardware keeps it (bit 6 set, bits 13-15 clear)
- waits on handle 0 or a closed handle fail; closing a handle wakes its waiters
- `VirtualFree(MEM_RELEASE)` only frees a whole reservation
- thread stacks follow the exe's stack reserve
- SSE2 `movsd` / `cmpsd` are SSE2, not the string instructions (dormant while CPUID says no SSE2)
- exit runs the exit list and the port's `port_at_exit`, then `_exit`
- (v0.2.1) CreateFile's ERROR_ALREADY_EXISTS only for a file that was there, and each disposition
  as Windows answers it
- (v0.2.8) the D3D7 renderer's fog is on unless options.txt or `PORT_DEFAULT_FOG 0` says off, and
  the render resolution defaults to `PORT_DEFAULT_RESOLUTION` ("original")
- (v0.2.12) see CHANGELOG v0.2.12's list: window handles and messages, GDI objects, the clock read
  by an empty PeekMessage, thread stacks out of the hash unless `PORT_HASH_STACKS`
- (v0.2.20) a port's stand-in for an import, defined in a file that includes gen/funcs.h, is a
  strong definition: a second definition of the same stand-in is now a link error (before, the
  first one linked won, and on Windows generated code linked first got none)
- (v0.2.12+) the window's messages arrive in Windows' order, a second RegisterClass of a class fails,
  handles start elsewhere, and GetProcAddress finds IsProcessorFeaturePresent: a baseline recorded on a
  port's own runtime can be a millisecond apart on the game's clock from the first frames, so long
  routes drift. That is not a bug to hunt (one port measured it); record
  new baselines on the toolkit once the short ones agree
- (v0.2.30) a real run exits 0 when the game quits normally, whatever code it gave (`--test` keeps
  the game's; `PORT_KEEP_EXIT_CODE` keeps it everywhere)
- (v0.2.32) a thread created after another finished reuses its stack, TEB and TLS blocks (their
  addresses), and DuplicateHandle returns a new handle: either can move hashes in a run that does it
- (v0.2.46) the x87 transcendentals (fsin, fcos, fsincos, fptan, fpatan, f2xm1, fyl2x, fyl2xp1) are
  computed by the toolkit, the same on every x86 CPU, not by the host's x87 (Intel's and AMD's differ
  in the last bits, so runs differed between PCs). Any game, C runtime or D3DX that uses them moves
  its hashes once: record baselines again, once, at this move; they then hold on every PC.
  `PORT_X87_PORTABLE 0` keeps the host's instructions (timing comparisons). Likewise the SSE
  reciprocal approximations (rcpss, rcpps, rsqrtss, rsqrtps) are exact divisions and square roots,
  the same on every CPU, in the generated code and in rewrites that use the intrinsics
  (`_mm_rcp_ss` and the rest; `PORT_SSE_PORTABLE 0` keeps the host's)
  Since v0.2.46 cpu.h reads the port's port.h (for these settings) and includes sse_portable.h:
  make the generated objects depend on port.h and runtime/cpu/*.h, or a changed setting leaves stale
  objects built with the old one (L.A. Rush's Makefile has the rule)
- (v0.2.46) D3D9 under `--test`: GetAdapterIdentifier and GetAvailableTextureMem give a test card, the
  same on every PC (`test_card = nvidia` by default: an RTX 3060's identity, 2047 MB; `amd`, `intel`,
  `real`; `test_vram = MB`), and DXVK is told the same vendor; a game that keeps anything from them
  moves its hashes once (the same re-record). If the game branches on the vendor, record a route per
  test card (runtime/d3d9/README.md)
- outside test runs only: `Sleep(0)` polling and clock-spin waits sleep instead of spinning; the
  frame limiter sleeps to its deadline

## Example

LEGO Island 2 was the first game built out of tree on the toolkit (core, then the D3D7 renderer),
matching its own port's hash scenes exactly.
