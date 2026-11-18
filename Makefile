# AntiStatic Recomp Toolkit: the toolkit's own checks.
#   make selftest            the translator against Unicorn (tests/selftest)
#   make selftest SEEDS=500  more random inputs per case
#   make stubtest            a stub Windows program through the translator and runtime core
#   make forms GAME=<game folder> GEN=<its translation>
#                            every distinct instruction form the game uses, checked the same way
#                            (the cases are made in build/ from the player's own files)
#   make padtest             controllers: merging, late plug-in, split screen, the scripted one
#   make pacingtest          the frame pacer and clock-spin guard on the real clock (~2.5 s)
#   make argc DEFS=<mingw-w64 lib32 folder, e.g. <zig>/lib/libc/mingw/lib32>
#                            every Windows function's stack-pop count against mingw-w64
# PY is a Python 3 with capstone and unicorn.
PY ?= python3
SEEDS ?= 200

selftest:
	PYTHON=$(PY) $(PY) tests/selftest/run.py --seeds $(SEEDS)

# the whole chain with no game files: ZIG=<zig> SDL3=<SDL3 build> (tests/stubgame/run.sh)
stubtest:
	PYTHON=$(PY) tests/stubgame/run.sh

forms:
	$(PY) tests/selftest/forms.py "$(GAME)" "$(GEN)" --out build/selftest/forms_game.s
	PYTHON=$(PY) $(PY) tests/selftest/run.py --seeds $(or $(FORM_SEEDS),20) --forms build/selftest/forms_game.s --only-forms

padtest:
	tests/pad/run.sh

pacingtest:
	mkdir -p build && $(CC) -O2 -Wall -o build/pacing_test tests/pacing/pacing_test.c && build/pacing_test

argc:
	$(PY) tools/check_api_argc.py "$(DEFS)" runtime

clean:
	rm -rf build

.PHONY: selftest stubtest forms padtest pacingtest argc clean
