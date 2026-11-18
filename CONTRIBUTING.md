# Contributing to the AntiStatic Recomp Toolkit

Issues and pull requests are welcome: bug reports from a port, missing Win32 or DirectX
behaviour, translator fixes, new checks, documentation.

## Never attach or commit game material

Do not attach to an issue, paste into one, or commit:

- game files of any kind (programs, DLLs, disc images, archives, data files)
- generated C (the translator's output is the game's code in another form)
- extracted assets: textures, models, sounds, music, films, shaders

Describe the problem instead: the function's address, the instruction or API call involved,
log lines, a memory diff, what the original does. A test case written from scratch (an x86
snippet in `tests/selftest/cases.s`, a call in the stub game) is the best way to show a bug.
`tools/audit_public.py DIR` checks a clean checkout for program files, assets and generated C;
pull requests that fail it are not merged.

## Every change keeps the checks passing

Before opening a pull request, run both:

**The stub game** (the whole chain with no game files: build, translate, compile, run, rewrite
tools, register audit):

```bash
ZIG=/path/to/zig SDL3=/path/to/sdl3 PYTHON=python3 tests/stubgame/run.sh
```

Needs Linux on x86-64, gcc, binutils, Zig 0.13 or later (or a mingw i686 gcc via `CC_WIN` and
`DLLTOOL`), an SDL3 build (`include/`, `lib/libSDL3.a`), FFmpeg's development libraries, zlib,
and Python 3 with `capstone`. Every line it prints must end in `ok`, and it must exit 0.

**The translator self-test** (every test case on random inputs, translated and in the Unicorn
emulator, compared register for register):

```bash
python3 tests/selftest/run.py          # or: make selftest; more inputs: make selftest SEEDS=500
```

Needs gcc, binutils and Python 3 with `capstone` and `unicorn`. It must exit 0.

If you change controllers or the frame pacer, also run `make padtest` or `make pacingtest`.

In the pull request, say what changed, why, and how it was checked. If the change alters the
generated C or can move a port's `--test` hashes, say so: it goes in CHANGELOG.md, so ports know
to translate again or to re-check their baselines.

## Style

Follow the code around your change: C for the runtime, Python for the translator and tools.
Comments say what the code does in Windows' or the game's terms, and how that is known (mark
claims `[verified: ...]` with the evidence, or `[guess]`).

## Licence

The toolkit's own code is released under the Unlicense (LICENSE). By contributing you agree to
release your contribution under the same terms. Code taken from another project keeps its
licence: say where it comes from in the file's header and add it to NOTICE.
