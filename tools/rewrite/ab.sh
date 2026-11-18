#!/usr/bin/env bash
# The A/B check for hand-written rewrites (docs/REWRITE.md; from etm-native's and
# other ports' tests/ab.sh): the game runs twice, deterministic (--test) and headless,
# rewrites off (A: the translation) and on (B), each writing its memory hash every few frames
# (--hash), and the two are compared, then how each run ended. Prints "same" or the first frame
# that differs.
#
#   ab.sh --env PREFIX --out DIR [--frames N] [--every N] [--seed DIR] [--id FILE]...
#         [--lock FILE|none] [--fresh] [--serial] [--fncheck SPEC] [--before COMMAND]
#         -- GAME_COMMAND [ARGUMENTS]...
#
# --env      the port's PORT_ENV: A runs with PREFIX_DECOMP_OFF=all; B with what the caller set
#            (empty: every rewrite on; set it to bisect: PREFIX_DECOMP_OFF=4a1920,4a2000)
# --out      where the runs' user folders, logs and hashes go (one per route)
# --frames   presents to run (default 3000); --every: frames between hashes (default 10)
# --seed     a folder copied into each run's fresh user folder first (a prepared save)
# --id       a file the translation's run depends on (the generated code's funcs.h, the runtime
#            sources outside the rewrites, the stack fixes, a build id...): A is saved under an id
#            of all of them plus the command and the route's environment (DIR/a-<id>/, the last
#            four kept), and reused while it is the same (the machine's name and the variables
#            that pick the graphics driver are part of it too). --fresh runs it again.
# --lock     a lock file, so checks run one at a time; default: work/ab.lock in the main checkout
#            of the git repository the current folder is in (shared by all its worktrees); outside
#            one (or in a copy git can't read), ~/.cache/antistatic/ab.lock; "none" for no lock
# --serial   run A and B one after the other instead of side by side
# --before   a shell command run once under the lock before the runs, in DIR (a route that first
#            runs the game a few frames to make the file it then seeds both runs with: --seed
#            names what it made). A file it makes that A's run depends on must also be an --id.
# --fncheck  no A/B: one run (rewrites as B has them) to the frames SPEC names, where each of its
#            per-function checks forks the translation and the rewrite from the same state and
#            compares them (runtime/rewrite/fncheck.c). Report: DIR/fncheck.txt; the exit code is
#            the run's (0: every check the same)
# The route's scripted input goes in the environment as the port reads it (PREFIX_KEYS, ...),
# set by the caller: it is passed through to both runs, and is part of the id. With no display
# (DISPLAY and WAYLAND_DISPLAY unset: a machine over ssh) SDL gets its offscreen driver.
set -eu
env= out= frames=3000 every=10 seed= lock=auto fresh=0 serial=0 fncheck= before= ids=()
while [ $# -gt 0 ]; do
    case "$1" in
    --env) env=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --frames) frames=$2; shift 2 ;;
    --every) every=$2; shift 2 ;;
    --seed) seed=$2; shift 2 ;;
    --id) ids+=("$2"); shift 2 ;;
    --lock) lock=$2; shift 2 ;;
    --fresh) fresh=1; shift ;;
    --serial) serial=1; shift ;;
    --fncheck) fncheck=$2; shift 2 ;;
    --before) before=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "unknown argument $1 (see the comment at the top)"; exit 2 ;;
    esac
done
[ -n "$env" ] && [ -n "$out" ] && [ $# -gt 0 ] || { echo "needs --env, --out and -- GAME_COMMAND"; exit 2; }
cmd=("$@")
off_var=${env}_DECOMP_OFF
b_off=${!off_var:-}
mkdir -p "$out"
outabs=$(cd "$out" && pwd)
# a machine with no display (a build machine over ssh): SDL's offscreen driver (headless runs never present)
[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || export SDL_VIDEO_DRIVER="${SDL_VIDEO_DRIVER:-offscreen}"
if [ "$lock" = auto ]; then
    # one check at a time on the machine, across worktrees: the main checkout's lock (a helper's
    # worktree shares it); the first worktree git lists is the main one
    main=$(git worktree list --porcelain 2>/dev/null | awk '/^worktree /{print $2; exit}' || true)
    lock=${main:+$main/work/ab.lock}
    # a copy whose .git points nowhere (a worktree copied to another machine) or no repository at all: one
    # lock for every check of this user on the machine, so they still run one at a time
    [ -n "$lock" ] || lock=${XDG_CACHE_HOME:-$HOME/.cache}/antistatic/ab.lock
    [ -z "$lock" ] || mkdir -p "$(dirname "$lock")"
fi
[ "$lock" != none ] || lock=
if [ -n "$lock" ]; then
    exec 9> "$lock"
    flock 9
fi
if [ -n "$before" ]; then
    (cd "$outabs" && bash -c "$before") > "$outabs/before.log" 2>&1 ||
        { echo "the --before command failed ($outabs/before.log)"; exit 2; }
fi
# how a run ended (code after the last hash, the exit's destructors, shows only here); grep -a: a
# log with a NUL in it is "binary" to grep, which then prints nothing (a false difference)
ending() { grep -a -o "program exited with code [0-9-]*\|FATAL.*" "$1" 2>/dev/null | tail -1 || true; }
run() {    # run <a|b|fncheck> <DECOMP_OFF value> <hash file or -> <log file> [extra arguments]
    local u="$outabs/user_$1" off=$2 hf=$3 lf=$4
    shift 4
    rm -rf -- "$u"
    mkdir -p "$u"
    # (a run that fails to start must not leave the last run's files to compare)
    [ "$hf" = - ] || rm -f -- "$hf"
    rm -f -- "$lf"
    [ -z "$seed" ] || cp -a "$seed/." "$u/"
    env "$off_var=$off" timeout -s KILL 3600 "${cmd[@]}" --test --user "$u" \
        --frames "$frames" "$@" > "$lf" 2>&1
}
if [ -n "$fncheck" ]; then    # the per-function check: one run, the report in $out/fncheck.txt
    rm -f "$out/fncheck.txt"
    st=0
    export "${env}_FNCHECK_OUT=$outabs/fncheck.txt"
    run fncheck "$b_off" - "$out/log_fncheck.txt" --fncheck "$fncheck" || st=$?
    [ -s "$out/fncheck.txt" ] || { echo "no report (exit $st; see $out/log_fncheck.txt)"; exit 1; }
    grep -a '^fncheck: .* checks' "$out/fncheck.txt" || echo "the run ended before its last check group (exit $st; $out/log_fncheck.txt)"
    exit $st
fi
id=$( { for f in ${ids[@]+"${ids[@]}"}; do cat "$f"; done
        echo "${cmd[*]} $frames $every"; env | grep "^${env}_" | grep -v "^$off_var=" | sort
        # the machine and the graphics driver: their device data is in game memory, so the
        # translation's hashes differ between them (NVIDIA against lavapipe from frame 10)
        uname -n
        env | grep -E '^(VK_DRIVER_FILES|VK_ICD_FILENAMES|VK_LOADER_DRIVERS_SELECT|DRI_PRIME|MESA_VK_DEVICE_SELECT|__GLX_VENDOR_LIBRARY_NAME|LIBGL_ALWAYS_SOFTWARE|SDL_VIDEO_DRIVER|DXVK_[A-Z_]*)=' | sort
        [ -z "$seed" ] || find "$seed" -type f -exec sha1sum {} + | sort; } | sha1sum | cut -c1-16)
cache="$out/a-$id"
if [ $fresh = 1 ] || ! [ -s "$cache/hash.txt" ]; then
    rm -rf -- "$cache"
    mkdir -p "$cache"
    if [ $serial = 1 ]; then
        run a all "$cache/hash.txt" "$cache/log.txt" --hash "$cache/hash.txt" --hash-every "$every" || true
        run b "$b_off" "$out/hash_b.txt" "$out/log_b.txt" --hash "$out/hash_b.txt" --hash-every "$every" || true
    else
        run a all "$cache/hash.txt" "$cache/log.txt" --hash "$cache/hash.txt" --hash-every "$every" &
        pa=$!
        run b "$b_off" "$out/hash_b.txt" "$out/log_b.txt" --hash "$out/hash_b.txt" --hash-every "$every" &
        pb=$!
        wait $pa || true
        wait $pb || true
    fi
    # (the last four translation runs kept: a route checked against two builds in turn reuses both)
    ls -dt "$out"/a-* 2>/dev/null | tail -n +5 | while read -r old; do rm -rf -- "$old"; done
else
    run b "$b_off" "$out/hash_b.txt" "$out/log_b.txt" --hash "$out/hash_b.txt" --hash-every "$every" || true
fi
cp "$cache/hash.txt" "$out/hash_a.txt" 2>/dev/null || rm -f "$out/hash_a.txt"
cp "$cache/log.txt" "$out/log_a.txt" 2>/dev/null || true
echo "$id" > "$out/hash_a.id"
if ! [ -s "$out/hash_a.txt" ] || ! [ -s "$out/hash_b.txt" ]; then
    rm -rf -- "$cache"
    echo "no hashes (see $out/log_a.txt, $out/log_b.txt)"; exit 1
fi
d=$(diff -a "$out/hash_a.txt" "$out/hash_b.txt" | grep -a '^[<>]' | head -2 || true)
na=$(wc -l < "$out/hash_a.txt"); nb=$(wc -l < "$out/hash_b.txt")
ea=$(ending "$out/log_a.txt"); eb=$(ending "$out/log_b.txt")
if [ -z "$d" ] && [ "$na" = "$nb" ] && [ "$ea" != "$eb" ]; then
    echo "differ at exit: translation \"$ea\", rewrites \"$eb\" ($out/log_b.txt)"
    exit 1
fi
if [ -z "$d" ] && [ "$na" = "$nb" ]; then
    echo "same ($na hashes to frame $(tail -1 "$out/hash_a.txt" | cut -d' ' -f1))"
else
    # (the first line of each run where they part: the translation's, then the rewrites')
    echo "differ: $(echo "$d" | grep -a '^<' | head -1 | cut -c3-) (translation) vs $(echo "$d" | grep -a '^>' | head -1 | cut -c3-) (rewrites); $na / $nb hashes"
    exit 1
fi
