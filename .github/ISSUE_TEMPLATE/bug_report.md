---
name: Bug report
about: Something in the translator, runtime or tools behaves differently from Windows or the original game
title: ''
labels: bug
assignees: ''
---

**Never attach game files, generated C or extracted assets** (textures, sounds, shaders, saves
from a game). Describe the problem in words, addresses and log lines instead.

**Toolkit version**
The tag or commit (`cat VERSION`, `git describe`).

**Game / port**
Which game and which port (or your own project), and whether it also happens on the port's
previous toolkit version.

**System**
OS and version, CPU, GPU and driver (Vulkan driver for D3D7 / DXVK ports), Linux or Windows build.

**Steps to reproduce**
1.
2.
3.

**What happens, and what you expected**

**Log excerpt**
The relevant lines of the port's log (a few dozen at most), or the failing check's output.

```
paste here
```

**Anything else**
Addresses, the Win32 / DirectX call involved, a memory diff, a test case written from scratch.
