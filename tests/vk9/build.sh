#!/usr/bin/env bash
# The toolkit's own Vulkan renderer for Direct3D 8 built with no game: the D3D8 bridge (runtime/d3d8/d3d8.c and
# the wrappers tools/d3d8/d3d8gen.py makes from DXVK's d3d8.h), the renderer (runtime/d3d8/vk_d3d8.c,
# vk_capdump.c over runtime/vk9) and its fixed-function and gamma shaders (glslangValidator, checked by
# spirv-val), compiled with the stub game's port.h and the ports' warning flags. Objects only: linking needs a
# game's translation. A built-in pipeline list is tried too (tools/vk9/plist2h.py on a one-line list).
#   DXVK=<DXVK native build, with include/dxvk> [SDL3=<SDL3 build>] [PYTHON=python3] tests/vk9/build.sh
# With ZIG=<zig> SDL3_WIN=<SDL3 for x86_64-windows-gnu> VK_INC=<Vulkan headers> ZLIB_WIN=<zlib for it> the same files are compiled for
# 64-bit Windows too (tools/win/zcc, runtime/win/compat.h before each, as a port's Windows build does).
# Prints "vk9 build: ok" and exits 0, or the compiler's errors and exits 1. Output goes to build/vk9.
set -eu
cd "$(dirname "$0")/../.."
: "${DXVK:?DXVK=<a DXVK native build with include/dxvk>}"
PY=${PYTHON:-python3}
GLSLANG=${GLSLANG:-glslangValidator}
SPIRV_VAL=${SPIRV_VAL:-spirv-val}
B=build/vk9
rm -rf "$B"
mkdir -p "$B/gen" "$B/obj" "$B/plist"
"$PY" tools/d3d8/d3d8gen.py "$DXVK/include/dxvk/d3d8.h" "$B/gen/d3d8_gen.c"
for s in ff.vert ff.frag gamma.vert gamma.frag; do
    n=${s%.*}_${s#*.}
    "$GLSLANG" -V --target-env vulkan1.3 -o "$B/gen/$n.spv" "runtime/vk9/shaders/$s" > "$B/glslang_$n.txt"
    "$SPIRV_VAL" --target-env vulkan1.3 "$B/gen/$n.spv"
    "$GLSLANG" -V --target-env vulkan1.3 --vn "vk_$n" -o "$B/gen/$n.h" "runtime/vk9/shaders/$s" >> "$B/glslang_$n.txt"
done
# (a funcs.h for runtime.h's includes: the stub game's translation makes the real one)
[ -f "$B/gen/funcs.h" ] || printf '#pragma once\n' > "$B/gen/funcs.h"
SDL_INC=${SDL3:+-I$SDL3/include}
CF="-std=gnu11 -O2 -g -fno-strict-aliasing -frounding-math -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter
    -Wno-format-truncation -Wno-missing-field-initializers -Itests/stubgame/port -Iruntime/win32 -Iruntime/cpu
    -Iruntime/rewrite -Iruntime/d3d8 -Iruntime/vk9 -I$B/gen -I$DXVK/include/dxvk $SDL_INC"
fail=0
build() {   # build OBJ SRC [EXTRA...]
    local o=$1 src=$2; shift 2
    if ! gcc $CF "$@" -c "$src" -o "$B/obj/$o.o" 2> "$B/obj/$o.txt"; then
        echo "--- $src"; cat "$B/obj/$o.txt"; fail=1
    fi
}
build d3d8 runtime/d3d8/d3d8.c -Wno-unused-function
build d3d8_gen "$B/gen/d3d8_gen.c" -Wno-unused-function
build vk_d3d8 runtime/d3d8/vk_d3d8.c
build vk_capdump runtime/d3d8/vk_capdump.c
build vk_core runtime/vk9/vk_core.c
build vk_d3d9 runtime/vk9/vk_d3d9.c
build vk_cs runtime/vk9/vk_cs.c
build vk_spv runtime/vk9/vk_spv.c -DVK_SPV_BUILD=0x$(cat runtime/vk9/vk_spv.c runtime/vk9/vk_abi.h | sha1sum | cut -c1-8)u
# with a port's built-in pipeline list (a header line and one made-up pipeline of the right key size)
printf 'keysize 4\n00000000 \n' > "$B/plist/list.txt"
"$PY" tools/vk9/plist2h.py "$B/plist/list.txt" "$B/plist/vk_plist.h"
build vk_d3d9_plist runtime/vk9/vk_d3d9.c -I"$B/plist"
if [ -n "${SDL3_WIN:-}" ]; then
    : "${ZIG:?ZIG=<zig> for the Windows pass}"; export ZIG
    mkdir -p "$B/win"
    WCF="-std=gnu11 -O2 -g -fno-strict-aliasing -fno-omit-frame-pointer -Iruntime/win -include runtime/win/compat.h
         -Itests/stubgame/port -Iruntime/win32 -Iruntime/cpu -Iruntime/rewrite -Iruntime/d3d8 -Iruntime/vk9 -I$B/gen
         -I$SDL3_WIN/include ${VK_INC:+-I$VK_INC} ${ZLIB_WIN:+-I$ZLIB_WIN/include} -w"
    for f in runtime/d3d8/d3d8.c "$B/gen/d3d8_gen.c" runtime/d3d8/vk_d3d8.c runtime/d3d8/vk_capdump.c runtime/vk9/vk_core.c \
             runtime/vk9/vk_d3d9.c runtime/vk9/vk_cs.c runtime/vk9/vk_spv.c; do
        o=$B/win/$(basename "$f" .c)
        if ! tools/win/zcc $WCF -c "$f" -o "$o.o" 2> "$o.txt"; then echo "--- (Windows) $f"; cat "$o.txt"; fail=1; fi
    done
    [ "$fail" != 0 ] || echo "vk9 build (Windows): ok ($(ls "$B"/win/*.o | wc -l) objects)"
fi
nwarn=$(cat "$B"/obj/*.txt | grep -c 'warning:' || true)
if [ "$fail" != 0 ]; then echo "vk9 build: FAILED"; exit 1; fi
[ "$nwarn" = 0 ] || { echo "warnings:"; cat "$B"/obj/*.txt | grep -A2 'warning:'; }
echo "vk9 build: ok ($(ls "$B"/obj/*.o | wc -l) objects, $nwarn warnings)"
