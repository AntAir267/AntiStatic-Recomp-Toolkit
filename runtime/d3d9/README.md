# Direct3D 9 through DXVK (the bridge)

The DXVK line of the ports (Enter the Matrix, L.A. Rush, Stranglehold). The program's Direct3D 9
runs on DXVK 3.1.1's native Linux build (Vulkan, SDL3 windows). The program's COM objects are the runtime's (`com.c`); their vtables point
at runtime functions that read the program's 32-bit stdcall arguments, convert what differs from
DXVK's 64-bit layouts, and call DXVK's object behind them. The program never sees DXVK's memory
(above 4 GB): every lock hands it a copy in its own memory, copied into DXVK's at unlock.

It is Enter the Matrix's bridge (etm-native, itself LA Rush's with that game's code taken out),
with Stranglehold's picture shape and kept lock copies as port settings, and a few toolkit
changes: the game's parts as hooks, `<PORT>_` variables, `--shot`, and the device lock let go
around every present.

| File | What |
| --- | --- |
| `d3d9.c` | `Direct3DCreate9` (loads `lib/libdxvk_d3d9.so.0` beside the program), object identity and reference counts (death watchers: an entry goes when DXVK destroys its object), present parameters, the methods with 32/64-bit differences, buffer / texture / surface / volume locks, display modes (a fixed list under `--test`), screen-space draws, shader substitution, frame pictures, the device lock |
| `d3d9_bridge.h` | the bridge's interface, the `HOSTFN` / `METHOD` macros with the device lock, the port hooks |
| `d3d9_msaa.c` | anti-aliasing for games that draw their scene into render targets with the device's depth buffer (option `antialiasing`, default `PORT_D3D9_ANTIALIASING`, `off`) |
| `d3d9_inv.c` | optional: the Direct3D call inventory (`<PORT>_D3D_INV=from-to,...`), for building a port's own renderer; build it or leave it out |

Tools (`tools/d3d9/`): `d3d9gen.py` (the generated wrappers), `d3dasm.py` (shader assembler /
disassembler, the replacement-shader table), `d3dinv_report.py` (the inventory summed up),
`build_dxvk.sh` (DXVK with the port's patch set). DXVK patches: `third_party/dxvk/`.

## What a port supplies

- **DXVK**: a DXVK 3.1.1 source tree, built with `tools/d3d9/build_dxvk.sh --src <dxvk> --sdl3
  <sdl3 install> --prefix third_party/dxvk-native --patches "<its set>"` (third_party/dxvk/README.md
  lists the patches; pin one set per port: device answers can change game memory). The headers are
  `<prefix>/include/dxvk`; the library `<prefix>/lib/x86_64-linux-gnu/libdxvk_d3d9.so.0` goes in
  `lib/` beside the program (with `libSDL3.so.0`, which DXVK opens by name).
- **The generated wrappers**: `gen/d3d9_gen.c` and `gen/d3d9_gen.h`, from DXVK's d3d9.h:

      gen/d3d9_gen.c gen/d3d9_gen.h: $(ANTISTATIC)/tools/d3d9/d3d9gen.py $(DXVK)/include/dxvk/d3d9.h
      	python3 $(ANTISTATIC)/tools/d3d9/d3d9gen.py $(DXVK)/include/dxvk/d3d9.h gen/d3d9_gen.c

  `--special Iface::Method,...` names more methods the port writes itself (as
  `HOSTFN(sp_<Iface>_<Method>, n)` in its own files, including `d3d9_bridge.h`);
  `--device-bodies` adds the generated device methods' bodies by vtable index
  (`d3d_device_bodies[]`, LA Rush's direct calls).
- **Shader replacements** (optional, option `shaders = improved`): `<hash>.vsh` / `.psh` in a folder
  of the port's, assembled into `gen/shaders_gen.c` by `tools/d3d9/d3dasm.py ctable <dir>
  gen/shaders_gen.c` (rewritten only when it changes, so run it on every make). A port without
  replacements leaves it out: empty weak tables stand in.
- **Build**: compile `runtime/d3d9/d3d9.c`, `d3d9_msaa.c` (and `d3d9_inv.c` if wanted) and
  `gen/d3d9_gen.c` with `-I$(ANTISTATIC)/runtime/d3d9 -Igen -I$(DXVK)/include/dxvk` beside the
  usual runtime includes (`d3d9.c` and `d3d9_gen.c` with `-Wno-unused-function`); link with
  `-ldl -lz -pthread`. The program imports `d3d9.dll`'s `Direct3DCreate9`; nothing else of the
  DLL is needed.

## Settings (port.h)

| Setting | Default | What |
| --- | --- | --- |
| `PORT_D3D9_ANTIALIASING` | `"off"` | option `antialiasing`'s default (`off`, `2x`, `4x`, `8x`; d3d9_msaa.c). etm-native: `"4x"` |
| `PORT_D3D9_MSAA_KEEP_DEPTH` | `0` | 1: a target of the device depth buffer's size set while that buffer stays bound gets its twin then (a game that never sets its depth buffer again, Nitronic Rush); off for a game that sets a target and then a depth buffer of its own |
| `PORT_D3D9_ANISOTROPIC` | `"16"` | option `anisotropic`'s default (`off`, `2`, `4`, `8`, `16`: DXVK's d3d9.samplerAnisotropy) |
| `PORT_D3D9_SHADOW_FILTER` | `"original"` | option `shadow_filter`'s default (`original`, `3`, `5`, `7`; needs DXVK's soft-shadows patch) |
| `PORT_D3D9_KEEP_ASPECT` | 0 | 1: a back buffer of another shape than the window is shown centred, black bars, not stretched (Stranglehold) |
| `PORT_D3D9_KEEP_LOCK_COPIES` | 0 | 1: a DYNAMIC or SYSTEMMEM texture's lock gives the same program copy every time (Bink videos decoding into old frames: Stranglehold). Changes the program's allocations, so memory hashes |

Options read: `vsync` (off: a game's interval ONE becomes IMMEDIATE; on: ONE), `anisotropic`,
`shadow_filter`, `antialiasing`, `shaders` (`original` / `improved`).

## Hooks (weak defaults in d3d9.c; the port defines the strong one)

| Hook | Default | What (who uses it) |
| --- | --- | --- |
| `void *port_d3d9_renderer(uint32_t sdk)` | NULL | another renderer's IDirect3D9 instead of DXVK's, asked at each Direct3DCreate9 until it first returns NULL (etm-native: option renderer = vulkan, its vk_d3d9.c) |
| `void port_d3d9_buffer_host(void *buf, void *copy)` | nothing | that renderer's buffers: the program's copy of a vertex / index buffer (etm-native's vk_buf_host) |
| `void port_d3d9_screen(uint32_t w, uint32_t h)` | nothing | the back buffer's size at CreateDevice / Reset / CreateAdditionalSwapChain |
| `void port_d3d9_viewport(uint32_t *x, *y, *w, *h)` | unchanged | SetViewport's viewport, changed in place (etm-native: the in-engine scenes' band widened) |
| `uint32_t port_d3d9_render_state(uint32_t state, uint32_t value)` | value | SetRenderState's value (etm-native: option draw_distance scales FOGSTART / FOGEND) |
| `int port_d3d9_ui2d_layout(caller, minx, maxx, miny, maxy, &xs, &xo, &ys, &yo)` | 0 | screen-space (XYZRHW) draws from memory: 1 and a scale and offset for x and y to lay the 2D out again (etm-native's widescreen 2D) |
| `const char *port_d3d9_dxvk_config(void)` | NULL | more DXVK_CONFIG settings (`"d3d9.windowedGammaRamp = True;"` with the windowed-gamma patch) |

## Variables (`<PORT>_NAME`, port_getenv)

`SHOT=frame,+frame,...` (back buffers saved as `shot_<frame>.png` in the user folder; `+` counts
from the scripted input's mark), `DUMP_CAPS=1` (DXVK's caps, formats and modes in the log),
`LOG_2D=1` (each screen-space draw), `LOG_LOCKS=1` (texture locks), `D3D_INV=from-to,...`
(d3d9_inv.c). DXVK's own: `DXVK_CONFIG`, `DXVK_LOG_PATH` / `DXVK_STATE_CACHE_PATH` (the user
folder unless set), `DXVK_WSI_DRIVER` (SDL3). The runtime's `--shot FRAME FILE` works through
`d3d_write_png`: the back buffer is read before the present when `shot_due()` says so.

## Threads and --test

Every program call into Direct3D takes one recursive device lock (`HOSTFN` / `METHOD`): a game
drawing loading frames from a second thread while its main thread makes resources must not have
both in the device at once. A present lets go of the lock entirely around `frame_presented()`
(`d3d_present_done`, both IDirect3DDevice9::Present and IDirect3DSwapChain9::Present): under
`--test` that hands the turn to another thread (`det_yield`), and a thread holding the lock while
it waits for its turn would leave the main thread waiting on the lock forever (etm-native's lobby
route: "--test: the turn is stuck" at frame 2280). A port's own hook must not wait for the turn
while it runs inside a Direct3D call.

## Things a port should know

- The display modes under `--test` are a fixed list (640x480 to 2560x1600, 60 Hz) and the desktop
  1920x1080, so runs don't depend on the monitors plugged in; otherwise DXVK's.
- The graphics card under `--test` is a test card, the same on every PC (GetAdapterIdentifier,
  GetAvailableTextureMem, GetDeviceCaps' MaxPointSize, and DXVK's own vendor-dependent answers): option `test_card = nvidia`
  (the default: an RTX 3060's identity), `amd` (an RX 6700 XT's), `intel` (a UHD 630's) or `real`
  (the PC's own; runs not comparable between PCs), and `test_vram = MB` (default 2047). A game that
  does something of its own for a vendor gets a route per card, each recorded and checked on any PC
  (`--option "test_card = amd"`). Outside `--test`, always the PC's own card.
- Full screen becomes a window of the back buffer's size (the desktop mode is never changed); the
  host window is resized to the back buffer at CreateDevice / Reset.
- CreateDevice sets the program's x87 control word to single precision unless the game asks for
  FPU_PRESERVE, as Direct3D 9 does; DXVK always gets FPU_PRESERVE.
- SetPrivateData with D3DSPD_IUNKNOWN stores the program's pointer as bytes (DXVK never reference
  counts a program object). GetDC on surfaces is not supported (D3DERR_INVALIDCALL).
- IDirect3DDevice9Ex and Direct3DCreate9Ex are not wrapped: a game (or a D3DX) asking for the Ex
  interfaces gets the plain ones' methods only.
- Headless runs: with no host window DXVK can't present, so DXVK test runs use `--hidden` (a hidden
  window) rather than none (Nitronic Rush).
- A hidden window presents with interval 0 (no vsync), so `--hidden` scripted runs go as fast as the
  machine: a port whose scripted inputs are numbered by frame and were recorded at a held rate
  (Stranglehold's menus at 20 fps) paces them with `<PORT_ENV>_PACE=<fps>`, or every input lands
  elsewhere.
