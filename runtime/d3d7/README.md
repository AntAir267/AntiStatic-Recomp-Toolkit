# DirectDraw and Direct3D 7 on Vulkan

The renderer of the DirectX 6/7 ports (1NSANE, Test Drive 6, Hard Truck 2, Off-Road, LEGO Island 2):
IDirectDraw / 7, surfaces, palettes, clippers and gamma (`ddraw.c`), IDirect3D7, IDirect3DDevice7
and vertex buffers (`d3d.c`), drawn by our own Vulkan renderer (`vk.c`, with its render thread
`vkthread.c`), or by the software rasterizer (`swrast.c`, for frame dumps without a GPU).

It is LEGO Island 2's copy (state blocks, the retained-vertex-buffer native path, pictures the
runtime draws itself), with Off-Road's later work folded in: the render thread woken per batch
instead of per draw (it spun, 22% of the game's CPU), the draw state sent only when it changed,
8-bit palettized textures, the mouse mapped into the game's picture, and (2026-11-13) its
renderer's own features behind port settings.

## What a port supplies

- `port_graphics_init` calls `ddraw_init()` (and its own widescreen and video set-up).
- `widescreen.c`: `widescreen_init`, `widescreen_options`, `widescreen_on`, `widescreen_screen`, and
  the values the renderer reads: `ws_aspect` (the picture's width / height), `ws_world_aspect` (the
  widened camera's projection), `ws_race_depth` / `ws_hud_depth` / `ws_menu_depth` (0 if unused).
  A port without widescreen sets them for 4:3 and returns 0 from `widescreen_on`.
- `font.h`, `fsr2.h`, `dlss.h`: the interfaces of optional upscalers (FSR 2 is MIT; NVIDIA's DLSS
  SDK has its own licence and is never part of this repository: a port without it builds the
  stubs).
- Effects its game code feeds: Off-Road's baked lighting and car lights, water, scene objects,
  SSAO and depth-back are here behind `PORT_D3D7_*` settings (port_example.h; vk.c's head), off
  unless a port sets them, with the game side's hooks weak: `port_d3d7_alpha_gain` (a screen-space
  draw's alpha gain: Off-Road's spray), `port_ws_hud_centre` (a HUD piece centred in widescreen:
  its countdown), `port_d3d7_ab_mode` / `port_d3d7_ab_record` (draw calls recorded for an A/B
  check), and `d3d_options()` re-reads fog, gpu_transform and renderer. 1NSANE's shadows, smoke,
  far draw and TAA scene are here as before.

## Shaders

The GLSL in `shaders/` is ours (no game shader code). Each `.spv.h` is generated and kept in the
repository:

    glslangValidator --target-env vulkan1.3 --vn spv_<name with _ for .> -o shaders/<file>.spv.h shaders/<file>

## Checks

LEGO Island 2 built on this copy (the out-of-tree build of the toolkit's handoff notes) keeps its
menu (30 hashes) and town (160 hashes) scenes identical to its own port, draw stream and memory,
with the render thread off and forced on.
