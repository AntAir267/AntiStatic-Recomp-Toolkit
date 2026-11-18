# The Windows host

The runtime is written against Linux. A port builds the same sources for 64-bit Windows
(x86_64-windows-gnu: MinGW's ABI, GCC's 80-bit `long double` and inline assembly) with this folder
standing in for the POSIX pieces MinGW lacks. From 1NSANE's and Penguins!'s Windows builds.

- `compat.h` is included before every runtime source (`-Iruntime/win -include runtime/win/compat.h`),
  not before the generated C. The headers next to it (`sys/mman.h`, `dlfcn.h`, `spawn.h`,
  `execinfo.h`, `signal.h`, `ucontext.h`, `fnmatch.h`, `sys/wait.h`, ...) declare what
  `platform.c` implements, so the sources stay as they are on Linux.
- `platform.c` (compiled into the program): the program's memory below 2 GB reserved first thing
  (if anything already sits there the program starts itself again with the range reserved,
  `<PORT_ENV>_WIN_RELAUNCH=1` forces that path), microsecond sleeps on a high-resolution waitable
  timer, mmap / mprotect on VirtualAlloc / VirtualProtect, dlopen on LoadLibrary, posix_spawn on
  CreateProcess, backtraces and names for the translated code, the sampling profiler, `pread` /
  `pwrite`.
- kernel32.c's GetProcAddress for a stand-in the program didn't import finds it in the runtime's
  name table (runtime.h's API_NAME, the `.apinm` section), so no export flag is needed (Zig 0.17
  drops `-Wl,--export-all-symbols` anyway). `dlsym(RTLD_DEFAULT, ...)` still searches the program's
  exports, which are empty unless the linker keeps that flag.
- Link with LLVM's linker (Zig, or `-fuse-ld=lld`): the generated stubs.c gives each import a
  fallback with `-alternatename`, which GNU ld doesn't take [guess: not tried].
- `msvc_shim.c`: empty stand-ins for the MSVC runtime pieces NVIDIA's DLSS library names, for a port
  that links DLSS (1NSANE).
- The port supplies its icon, manifest and resource script (`<game>.rc`), and leaves out what has
  no Windows build (Penguins! replaced its FFmpeg film player with a "not available" one).

Building: `tools/win/zcc` (and zcxx, zar, zranlib, zrc) are Zig 0.17.0 for x86_64-windows-gnu (`$ZIG`, or `zig` on
the PATH), so a
Linux machine builds the Windows program (Penguins!); 1NSANE builds in MSYS2's UCRT64 with GCC or
the same Zig (its tools/release/windows.sh). Link SDL3 and zlib static, plus `-lm -luser32 -lgdi32
-lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lcfgmgr32 -lshell32
-lpsapi -ldbghelp -Wl,--stack,8388608` (and `-mwindows` for no console). Miles streams without
FFmpeg: `PORT_MILES_NO_FFMPEG`. `tools/win/wine_lowfree.c` is a Wine-only test aid (from TD6).

Checked: the stub game (tests/stubgame/run.sh with SDL3_WIN, ZLIB_WIN, VK_INC) translates, compiles
and links for Windows; it isn't run here (no Windows). Every runtime/win32, cpu, rewrite, music
(but the FFmpeg and DirectMusic ones' third-party headers) and d3d7 source compiles for Windows.
