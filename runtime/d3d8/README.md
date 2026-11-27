# runtime/d3d8: the Direct3D 8 bridge to DXVK

A program's Direct3D 8 runs on DXVK's native Linux build (its d3d8 front end over its d3d9), and on
Windows on Windows' own Direct3D 8 or DXVK's `d3d8.dll`. The program's COM objects are the runtime's
(`win32/com.c`): their vtables point at runtime functions, mostly generated from DXVK's `d3d8.h`
(`tools/d3d8/d3d8gen.py`), which call DXVK's object behind them. It is the toolkit's Direct3D 9
bridge (`runtime/d3d9`) for Direct3D 8, made from the D3D8 ports' own copies.

| File | What |
| --- | --- |
| `d3d8.c` | `Direct3DCreate8` (DXVK beside the program; on Windows option `d3d8_dll`, then DXVK's `d3d8.dll` beside the program, then Windows' own), object identity and reference counts (death watchers), present parameters, the methods with 32/64-bit differences, buffer / texture / surface / volume locks (copies in the program's memory), display modes, the test card, frame pictures |
| `d3d8_bridge.h` | What the bridge's parts share |
| `vk_d3d8.c` | The toolkit's own Vulkan renderer for Direct3D 8: DXVK 3.1.1's d3d8 front end written in C, over our Direct3D 9 renderer (`runtime/vk9`, its README); `vk_create8`, `vk8_buf_host` |
| `vk_capdump.c` | Its measuring aids, for either renderer (`vk_debug8`: `DUMP_CAPS`, `DRAWSHOT`, `FRAMETIME`, `PROF`) |
| `../../tools/d3d8/d3d8gen.py` | Generates `gen/d3d8_gen.c` and `gen/d3d8_gen.h` (the method wrappers and every method's vtable index, `VI_<Iface>_<Method>`) |

## What a port supplies

- The generator in its Makefile, and the bridge compiled with DXVK's headers:

      gen/d3d8_gen.c gen/d3d8_gen.h: $(ANTISTATIC)/tools/d3d8/d3d8gen.py $(DXVK)/include/dxvk/d3d8.h
      	python3 $(ANTISTATIC)/tools/d3d8/d3d8gen.py $(DXVK)/include/dxvk/d3d8.h gen/d3d8_gen.c

  `runtime/d3d8/d3d8.c` and `gen/d3d8_gen.c` with `-I$(ANTISTATIC)/runtime/d3d8 -Igen -I$(DXVK)/include/dxvk`
  beside the core's include paths. DXVK's `libdxvk_d3d8.so.0` and `libdxvk_d3d9.so.0` in `lib/`
  beside the program. The toolkit's DXVK patches (`third_party/dxvk`, with `--d3d8`) give the
  windowed gamma ramps and the rest.
- Optionally the toolkit's Vulkan renderer: `vk_d3d8.c`, `vk_capdump.c` and `runtime/vk9` linked in (what
  to compile: `runtime/vk9/README.md`). Their `vk_create8()` (its `IDirect3D8`), `vk8_buf_host()` and
  `vk_debug8()` replace the bridge's weak defaults; option `renderer = vulkan` then starts it (DXVK if it
  can't start, logged). `d3d8_vulkan_running()` says which runs, `d3d8_card_vendor()` gives it the test
  card's vendor. A port with a renderer of its own can still supply those three functions instead.

## Settings (port.h)

| Setting | What |
| --- | --- |
| `PORT_D3D8_FIXED_MODES 1` | The fixed mode list in real runs too, not only under `--test` (a game that rebuilds its device when the list lacks the mode it asks for) |
| `PORT_D3D8_RENDERER "vulkan"` | Option `renderer`'s default (`"dxvk"` unless set) |
| `PORT_D3D8_MODES_FILTER 1` | Only the modes of option `resolution`'s size (a game that picks its mode from the list in a dialog the port never shows) |

## Options (options.txt, `--option`)

`vsync` (on), `gamma_flashes` (on: the game's gamma ramps in a window), `anisotropic` (off | 2 | 4 |
8 | 16), `renderer` (dxvk | vulkan), `resolution` (with `PORT_D3D8_MODES_FILTER`), `d3d8_dll`
(Windows), and under `--test` `test_card` (nvidia | amd | intel | real) and `test_vram` (MB, 2047).

## Things a port should know

- Under `--test`, and on a PC with no screen (SDL's offscreen driver), the display modes are a fixed
  list (640x480 to 2560x1600, 16 and 32 bits) and the desktop 1920x1080 at 60 Hz.
- The graphics card under `--test` is a test card, the same on every PC: GetAdapterIdentifier,
  GetAvailableTextureMem, both GetDeviceCaps' MaxPointSize and the vendor DXVK is given answer as
  `test_card` says (nvidia: an RTX 3060's identity, the default; amd; intel; real: the PC's own),
  as the D3D9 bridge's do. GetAdapterIdentifier copies the program's 1068-byte structure (DXVK's
  host one is 1072).
- `<PORT_ENV>_RENDERER=vulkan|dxvk`: the renderer without the option, so both renderers' test runs have the same
  options (and the same memory, where the game keeps the option's text).
- `<PORT_ENV>_D3D_INVENTORY=file`: every Direct3D 8 method the program called, with counts, at exit.
