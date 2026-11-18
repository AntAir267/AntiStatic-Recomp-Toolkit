#!/bin/bash
# Build DXVK 3.1.1's native Linux libraries (libdxvk_d3d9, and libdxvk_d3d8 with --d3d8) for a
# port of the Direct3D 9 bridge (runtime/d3d9), with the DXVK patches the port names
# (third_party/dxvk/README.md). The port gives its own DXVK source tree, its SDL3 install and where
# to install; nothing of DXVK's is kept in the toolkit but the patches.
#
#   tools/d3d9/build_dxvk.sh --src DIR --sdl3 DIR --prefix DIR [--build DIR] [--patches "a b ..."]
#                            [--d3d8] [--jobs N] [--check]
#
#   --src      DXVK 3.1.1's source (git clone --recursive --branch v3.1.1
#              https://github.com/doitsujin/dxvk), unpatched or patched by this script before with
#              the same set. Patched here, in place.
#   --sdl3     an SDL3 install (its lib/pkgconfig/sdl3.pc): DXVK's window system (DXVK_WSI_DRIVER=SDL3)
#   --prefix   where DXVK installs: <prefix>/include/dxvk (the headers the bridge and d3d9gen.py read)
#              and <prefix>/lib/<multiarch>/libdxvk_d3d9.so.0 (beside the port's program, in lib/)
#   --build    the meson build folder (default: <src>-build)
#   --patches  the patch set, by short name: soft-shadows depth-texture live-options texbem
#              windowed-gamma (default: none). Applied in that order whatever order is given.
#   --d3d8     also DXVK's Direct3D 8 front end (libdxvk_d3d8)
#   --jobs     ninja's parallel jobs (default: ninja's own, every core)
#   --check    only apply the patches (or say they are applied) and stop
# A port pins its set: device answers can change the game's memory (etm-native's depth format), so
# a port's hash baselines hold for its own set only.
set -e
here=$(cd "$(dirname "$0")" && pwd)
pdir=$here/../../third_party/dxvk
order="soft-shadows depth-texture live-options texbem windowed-gamma"
src= sdl3= prefix= build= want= d3d8=false check=0 jobs=
while [ $# -gt 0 ]; do
    case "$1" in
    --src) src=$2; shift 2 ;;
    --sdl3) sdl3=$2; shift 2 ;;
    --prefix) prefix=$2; shift 2 ;;
    --build) build=$2; shift 2 ;;
    --patches) want=$2; shift 2 ;;
    --d3d8) d3d8=true; shift ;;
    --jobs) jobs=$2; shift 2 ;;
    --check) check=1; shift ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "build_dxvk: unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -n "$src" ] || { echo "build_dxvk: --src is needed" >&2; exit 2; }
src=$(cd "$src" && pwd)
grep -q "version : '3.1.1'" "$src/meson.build" || { echo "build_dxvk: $src is not DXVK 3.1.1 (meson.build)" >&2; exit 1; }
[ -f "$src/subprojects/dxbc-spirv/meson.build" ] || { echo "build_dxvk: $src has no subprojects (clone with --recursive)" >&2; exit 1; }

# the set, in the fixed order, each known and with what it needs
set_=
for p in $order; do
    for w in $want; do [ "$w" = "$p" ] && set_="$set_ $p"; done
done
for w in $want; do
    case " $order " in *" $w "*) ;; *) echo "build_dxvk: no patch named $w (known: $order)" >&2; exit 2 ;; esac
done
case " $set_ " in
*" live-options "*|*" windowed-gamma "*)
    case " $set_ " in *" soft-shadows "*) ;; *) echo "build_dxvk: live-options and windowed-gamma need soft-shadows" >&2; exit 2 ;; esac ;;
esac
set_=${set_# }

# applied once: the set is recorded in the source tree; another set needs a clean source
stamp=$src/.antistatic-dxvk-patches
if [ -f "$stamp" ]; then
    if [ "$(cat "$stamp")" != "$set_" ]; then
        echo "build_dxvk: $src was patched with \"$(cat "$stamp")\", not \"$set_\": give a clean DXVK source" >&2
        exit 1
    fi
    echo "build_dxvk: patches already applied: ${set_:-none}"
else
    for p in $set_; do
        f=$pdir/dxvk-$p.patch
        if ! patch -d "$src" -p1 --dry-run --forward -s < "$f" > /dev/null; then
            echo "build_dxvk: dxvk-$p.patch does not apply to $src (patched already by hand? a clean source is needed)" >&2
            exit 1
        fi
        patch -d "$src" -p1 --forward -s < "$f"
        echo "build_dxvk: applied dxvk-$p.patch"
    done
    echo "$set_" > "$stamp"
fi
[ $check = 1 ] && exit 0

[ -n "$sdl3" ] && [ -n "$prefix" ] || { echo "build_dxvk: --sdl3 and --prefix are needed to build" >&2; exit 2; }
sdl3=$(cd "$sdl3" && pwd)
mkdir -p "$prefix"
prefix=$(cd "$prefix" && pwd)
pc=$(ls -d "$sdl3"/lib*/pkgconfig 2>/dev/null | head -1)
[ -f "$pc/sdl3.pc" ] || { echo "build_dxvk: no sdl3.pc under $sdl3/lib*/pkgconfig" >&2; exit 1; }
build=${build:-$src-build}
if [ ! -f "$build/build.ninja" ]; then
    PKG_CONFIG_PATH=$pc meson setup "$build" "$src" --buildtype release --prefix="$prefix" \
        -Denable_d3d9=true -Denable_d3d8=$d3d8 -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=false \
        -Dnative_sdl3=enabled -Dnative_sdl2=disabled -Dnative_glfw=disabled
fi
ninja -C "$build" ${jobs:+-j "$jobs"} install
lib=$(ls "$prefix"/lib*/libdxvk_d3d9.so.0 "$prefix"/lib/*/libdxvk_d3d9.so.0 2>/dev/null | head -1)
echo "build_dxvk: ${lib:-libdxvk_d3d9.so.0 not found under $prefix} (patches: ${set_:-none})"
[ -n "$lib" ]
