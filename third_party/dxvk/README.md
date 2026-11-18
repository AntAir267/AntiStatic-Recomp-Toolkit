# DXVK patches for the Direct3D 9 bridge

DXVK itself is not here: a port builds DXVK 3.1.1 (https://github.com/doitsujin/dxvk, tag v3.1.1,
cloned with its submodules) from its own source tree with `tools/d3d9/build_dxvk.sh`, naming the
patches it wants from this folder (runtime/d3d9/README.md). The patches are applied in the order
below; each changes DXVK 3.1.1 and, where noted, its dxbc-spirv subproject.

| Patch (`--patches` name) | What it does | From |
| --- | --- | --- |
| `dxvk-soft-shadows.patch` (`soft-shadows`) | a `d3d9.shadowFilter` setting (0, 3, 5, 7; off unless set): shadow-map comparison samples read through a smooth filter that many texels square (option `shadow_filter`) | larush-native 356c98a (2026-10-23); identical in etm-native |
| `dxvk-depth-texture.patch` (`depth-texture`) | StretchRect from a depth buffer into an INTZ / DF24 / DF16 texture of the same Vulkan format: a copy, or (multisampled) a one-sample resolve of the depth alone, for a post pass that reads the scene's depth (LA Rush's ambient occlusion) | larush-native ccb0ae2 (2026-10-27) |
| `dxvk-live-options.patch` (`live-options`, needs soft-shadows) | exports `DXVK_D3D9SetSamplerAnisotropy` and `DXVK_D3D9SetShadowFilter`, so a graphics page changes them while the game runs | larush-native 959549b (2026-10-27) |
| `dxvk-texbem.patch` (`texbem`) | ps_1_x texbem / texbeml as Direct3D defines them (dxbc-spirv read the bump matrix's off-diagonal terms swapped: LA Rush's checkpoint beam), and DXVK's shader cache tagged `+texbem` so shaders converted before it are not reused | larush-native 55f927c (2026-10-28) |
| `dxvk-windowed-gamma.patch` (`windowed-gamma`, needs soft-shadows for its context) | a `d3d9.windowedGammaRamp` setting: the game's gamma ramps (its flashes) applied to a windowed swap chain too | one of the C++ line's ports (2026-11-05), copied to lixs-native and penguins-native (identical) |

The sets the ports use today: Enter the Matrix `soft-shadows`; LA Rush `soft-shadows
depth-texture live-options texbem`; LIXS and Penguins! (with `--d3d8`) all five;
Stranglehold none. [verified 2026-11-13: the four LA Rush patches applied by build_dxvk.sh to a
clean v3.1.1 give larush-native's patched tree file for file; the soft-shadows build exports the
same symbols as etm-native's library]

## Licences

DXVK: zlib/libpng licence, Copyright (c) 2017 Philip Rebohle, (c) 2019 Joshua Ashton, (c) 2019
Robin Kertels, (c) 2023 Jeffrey Ellison (its LICENSE). dxbc-spirv (DXVK's subproject, touched by
soft-shadows and texbem): MIT, Copyright (c) 2025 Philip Rebohle. The patches are changes to those
files and are under the same licences as the files they change. A port shipping the built
library ships DXVK's LICENSE with it.
