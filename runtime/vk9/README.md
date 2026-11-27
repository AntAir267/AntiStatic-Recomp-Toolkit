# runtime/vk9: our own Direct3D 9 renderer on Vulkan

Direct3D 9 written in C as host COM objects with Direct3D's vtables, drawing with Vulkan 1.3 (dynamic
rendering, a bindless texture set, pipelines by content). It is the base the toolkit's Direct3D 8
renderer (`runtime/d3d8/vk_d3d8.c`) stands on, as DXVK's d3d8 front end stands on its d3d9. Its answers
and pictures follow DXVK 3.1.1's, so a program's memory and frames are the same under either: the ports
that built it checked that with A/B runs and frame dumps against DXVK.

| File | What |
| --- | --- |
| `vk.h` | What the renderer's parts share: Vulkan's functions, memory, the frame ring, the renderer's state `R`, the device's hot methods by name |
| `vk_abi.h` | The interface between the shader compiler and the renderer (sampler slots, specialization constants, push constants) |
| `vk_core.c` | Vulkan itself: instance and device (through SDL3's Vulkan, or the loader alone when headless), memory, frames, uploads, samplers, the bindless set, the swapchain (sRGB views when the device has `VK_KHR_swapchain_mutable_format`, resized with the window), the driver's pipeline cache in the user folder |
| `vk_d3d9.c` | The Direct3D 9 objects and the device's state, draws, render passes, resolves and copies; fixed function as two uber-shaders specialized by the state; pipelines made ahead by worker threads; gamma ramps; display modes and caps as DXVK answers them |
| `vk_cs.c` | Recording on a thread of its own (DXVK's "CS thread"): the game thread's Vulkan commands queued and made, in order, by a worker |
| `vk_spv.c` | The game's shaders (shader model 1.x bytecode, as Direct3D 8 gives it) compiled to SPIR-V at run time, as DXVK's dxbc-spirv converts them; kept in the user folder (`vk_spirv.bin`) |
| `shaders/ff.vert`, `ff.frag` | Direct3D 9's fixed-function vertex processing and texture stages (adapted from DXVK 3.1.1's, zlib licence: NOTICE) |
| `shaders/gamma.vert`, `gamma.frag` | The gamma ramp's pass at present, as DXVK's presenter applies one |
| `../../tools/vk9/plist2h.py`, `vk_plist.py` | A port's built-in pipeline list: user folders' `vk_pipelines.bin` merged into a text list (keys and vertex declarations only, no game code), then a header for the build |

## What a port supplies (with the D3D8 bridge)

- The files above and `runtime/d3d8/vk_d3d8.c` + `vk_capdump.c` compiled with
  `-I$(ANTISTATIC)/runtime/vk9 -I$(ANTISTATIC)/runtime/d3d8 -I$(DXVK)/include/dxvk` beside the core's
  include paths (DXVK's headers for the Direct3D types; Vulkan's own headers), with SDL3. `vk_spv.c` with
  `-DVK_SPV_BUILD=0x<8 hex digits>u` (a hash of `vk_spv.c` and `vk_abi.h`: the saved SPIR-V is dropped when
  the compiler changes).
- The shaders as C arrays in a folder on the include path: `ff_vert.h`, `ff_frag.h`, `gamma_vert.h`,
  `gamma_frag.h`, from `glslangValidator -V --target-env vulkan1.3 --vn vk_ff_vert -o ff_vert.h shaders/ff.vert`
  (and so on: `vk_ff_frag`, `vk_gamma_vert`, `vk_gamma_frag`), checked with `spirv-val --target-env vulkan1.3`.
- Optionally a built-in pipeline list: a `vk_plist.h` on the include path (`tools/vk9/plist2h.py LIST vk_plist.h`).
  Without one only the user folder's list is made ahead.
- Optionally `vk9_options_changed()` called when the port's own settings page changes `anisotropic`.
- `tests/vk9/build.sh` builds all of it with the stub game's `port.h` (Linux, and Windows with Zig).

## Options

`renderer = vulkan` starts it (the bridge's option; `PORT_D3D8_RENDERER` its default), `vsync` (on),
`anisotropic` (off | 2 | 4 | 8 | 16), `gamma_flashes` (on: the game's ramps in a window; DXVK shows them only
full screen).

## Measuring aids (environment, `<PORT_ENV>_...`)

| Variable | What |
| --- | --- |
| `RENDERER=vulkan\|dxvk` | The renderer without the option (the bridge's) |
| `VK_VALIDATE=1` | Khronos validation layer on, its messages in the log |
| `VK_DEVICE=<text>` | The GPU whose name contains it |
| `VK_DYN_BLEND=1` | Colour blend states dynamic (`VK_EXT_extended_dynamic_state3`), as DXVK where the device has them |
| `VK_CS=0` | Record on the game thread |
| `VK_DIRECT=0` | The Direct3D 8 front end calls through vtables, not by name |
| `VK_BUF_HOST=0` | Buffers keep a copy of their own instead of the program's |
| `VK_SC_DIRECT=0` | The back buffer blitted, not drawn straight into the swapchain image |
| `VK_FFV_PARTS=0` | The fixed-function block computed whole at every change |
| `VK_RP_RESOLVE=0` | Every resolve by `vkCmdResolveImage` |
| `VK_NARROW=0` | Never the narrow pixel shader variants |
| `VK_SPIRV_CACHE=0` | The saved SPIR-V neither read nor written |
| `VK_PLIST_BUILTIN=0` | The built-in pipeline list not used (for making it again) |
| `VK_LOG_PIPES=1` | Every pipeline made, logged |
| `VK_SHADER_DUMP=<folder>` | The game's shader bytecode as handed over (from the player's files: never into a repository) |
| `VK_SKIP=<bits>` | Draws left out by kind (1 game vertex shaders, 2 game pixel shaders, 4 lit, 8 unlit, 16 pre-transformed, 32 cube targets) |
| `VK_GAMMA=1`, `VK_GAMMA_DUMP=F` | Gamma ramps applied; frame F through the gamma pass to `work/vk/` |
| `VK_TRACE_STATES=1` | Each render / texture stage state value the game sets, once |
| `DUMP_CAPS=1` | The answers the game can ask of an adapter and device, logged (either renderer) |
| `DRAWSHOT=F:A:B[:DIR]` | The back buffer after each draw A..B of frame F (either renderer); `DRAWSHOT_PPM`, `DRAWSHOT_RS=S=V,...`, `DRAWSHOT_TSS=T:S=V,...`, `DRAWSHOT_XF=1` (transforms and first vertices) |
| `FRAMETIME=A:B` | Frame times from frame A to B |
| `PROF=A:B[:FILE]` | The presenting thread sampled (Linux) |

## Files in the user folder

`vk_spirv.bin` (the compiled shaders), `vk_pipelines.bin` (the pipelines made, for making them ahead next
time), `vk_pipeline_cache.bin` (the driver's cache data). All made on the player's PC from the player's copy.

## Not here

Volume textures and additional swap chains, ps_1_4, texbeml, texreg2ar/gb, texm3x3 alone, texm3x2depth, dst,
relative addressing in pixel shaders, vertex blending (logged; draws with them are skipped or drawn without).
Direct3D 9 programs: the objects are Direct3D 9's, but only the Direct3D 8 front end drives it so far.
