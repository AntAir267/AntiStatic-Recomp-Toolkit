#!/usr/bin/env bash
# Which rewrite makes an A/B route differ (from one port's tests/bisect.sh). The build's
# registered rewrites (its decomp_entry_<address> symbols, in address order) are switched off from
# the front (<PREFIX>_DECOMP_OFF for the rewrites' run) by halving: the smallest count that makes the
# route "same" names one rewrite the difference needs, which is then tried switched off alone. About
# log2(rewrites) + 2 runs of the route (ab.sh keeps the translation's run after the first).
#
#   bisect.sh --nm BINARY ab.sh-ARGUMENTS... -- GAME_COMMAND [ARGUMENTS]...
#
# BINARY: the built program (for nm). Everything else goes to tools/rewrite/ab.sh as it is (--env,
# --out, --frames, --seed, --id, --lock ...): one route, as you would run it on its own, and the
# route's scripted input in the environment. Run it where the route differs (under lavapipe, on
# another machine). Prints each step; the last line names the rewrite.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin= args=() env=
while [ $# -gt 0 ]; do
    case "$1" in
    --nm) bin=$2; shift 2 ;;
    --env) env=$2; args+=("$1" "$2"); shift 2 ;;
    --) break ;;
    *) args+=("$1"); shift ;;
    esac
done
[ -n "$bin" ] && [ -n "$env" ] && [ $# -gt 1 ] || { echo "needs --nm BINARY, --env PREFIX and -- GAME_COMMAND (see the top)"; exit 2; }
all=($(nm "$bin" | sed -n 's/.* decomp_entry_\([0-9a-f]*\)$/\1/p' | sort))
n=${#all[@]}
[ "$n" -gt 0 ] || { echo "$bin registers no rewrites (no decomp_entry_ symbols)"; exit 2; }
off_var=${env}_DECOMP_OFF
run() {     # run OFF_LIST -- GAME_COMMAND...: the route's verdict line with those rewrites off
    local o=$1
    shift
    env "$off_var=$o" "$here/ab.sh" "${args[@]}" "$@" 2>&1 | tail -1
}
same() {    # same K: is the route same with the first K rewrites off?
    local off= r
    [ "$1" -gt 0 ] && off=$(IFS=,; echo "${all[*]:0:$1}")
    r=$(run "$off" "${cmd[@]}")
    echo "  first $1 of $n off: ${r:0:100}"
    case "$r" in same*) return 0 ;; *) return 1 ;; esac
}
cmd=("$@")
if same 0; then echo "the route is same with every rewrite on: nothing to find"; exit 0; fi
lo=0 hi=$n          # differs with the first lo off; same with the first hi off (all off is the translation)
while [ $((hi - lo)) -gt 1 ]; do
    mid=$(((lo + hi) / 2))
    if same $mid; then hi=$mid; else lo=$mid; fi
done
a=${all[$((hi - 1))]}
r=$(run "$a" "${cmd[@]}")
echo "the difference needs the rewrite of $a (that one off alone: ${r:0:100})"
