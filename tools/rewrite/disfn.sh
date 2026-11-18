#!/usr/bin/env bash
# The instructions of one translated function as discovery bounded it (GEN/extents.txt), from
# the player's own module file, Intel syntax (from one port's tools/disfn.sh).
#   disfn.sh GEN MODULE_FILE ADDR      (ADDR hex, e.g. 44c680; MODULE_FILE the exe or DLL holding it)
#   disfn.sh GEN GAME_FOLDER ADDR      (the module picked by address from GEN/table.c's module
#                                       table, its file under GAME_FOLDER; a DLL loaded away from
#                                       its preferred base is disassembled at the translation's)
set -eu
gen=$1 file=$2
a=$(printf '%08x' "0x${3#0x}")
line=$(grep "^$a " "$gen/extents.txt") || { echo "no function at $a"; exit 1; }
read -r e lo hi n _ <<< "$line"
adjust=0
if [ -d "$file" ]; then
    # {"name", "file", "sha256", base, size, entry, ntexts, {texts}, preferred_base, is_dll, load}
    pick=$(python3 - "$gen/table.c" $((0x$a)) <<'PY'
import re, sys
a = int(sys.argv[2])
for m in re.finditer(r'^    \{"[^"]*", "([^"]*)", "[0-9a-f]*", (0x[0-9a-f]+)u, (0x[0-9a-f]+)u, .*\}\}, (0x[0-9a-f]+)u, \d+, \d+\},$',
                     open(sys.argv[1]).read(), re.M):
    base, size, pref = int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)
    if base <= a < base + size:
        print(m.group(1), base - pref)
        break
PY
)
    [ -n "$pick" ] || { echo "no module of $gen/table.c holds $a"; exit 1; }
    read -r rel adjust <<< "$pick"
    file="$file/$rel"
fi
echo "# $a: $n instructions, $lo-$hi ($(basename "$file")$([ "$adjust" = 0 ] || printf '; shown at its preferred base, %+d' $((-adjust))))"
objdump -d -M intel --no-show-raw-insn --start-address=$((0x$lo - adjust)) --stop-address=$((0x$hi - adjust)) "$file" | tail -n +8
