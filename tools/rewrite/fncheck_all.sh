#!/usr/bin/env bash
# Every per-function check spec (docs/REWRITE.md "Checking a function no route reaches"; from
# one port's tests/fncheck/run_all.sh) on each of its routes: the spec's
# "# routes: NAME..." line, one run of the port's route script each, in turn (each takes the shared
# A/B lock). One summary line per run:
#   SPEC on ROUTE: N checks: A same, B differ, C errors   (the report: OUT/SPEC-ROUTE/fncheck.txt)
# then the totals. Exit 0 only if every check of every run was the same.
#
#   fncheck_all.sh [--run CMD] [--out DIR] [SPEC...]
#
# --run  the port's route script, which takes `--route NAME --out DIR --fncheck SPEC` and runs
#        tools/rewrite/ab.sh with that route's input (default tests/ab.sh); a spec without a
#        routes line runs once without --route (the script's default route)
# --out  where each run's folder goes (default work/fncheck)
# SPEC   the specs (default tests/fncheck/*.spec)
# A mutant proof runs the same way with the mutant's switch in the environment (or the script's
# BIN= for a copy of the binary): its runs must show DIFFER.
set -u
run=tests/ab.sh outdir=work/fncheck specs=()
while [ $# -gt 0 ]; do
    case "$1" in
    --run) run=$2; shift 2 ;;
    --out) outdir=$2; shift 2 ;;
    *) specs+=("$1"); shift ;;
    esac
done
[ ${#specs[@]} -gt 0 ] || specs=(tests/fncheck/*.spec)
mkdir -p "$outdir"
bad=0 runs=0 tc=0 ts=0 td=0 te=0
for spec in "${specs[@]}"; do
    [ -f "$spec" ] || { echo "$spec: no such spec"; bad=1; continue; }
    name=$(basename "$spec" .spec)
    routes=$(grep -a -m1 '^# routes:' "$spec" | sed 's/^# routes://; s/(.*//')
    [ -n "${routes// /}" ] || routes=-
    for r in $routes; do
        out=$outdir/$name-$r
        mkdir -p "$out"
        runs=$((runs + 1))
        rm -f "$out/fncheck.txt"
        if [ "$r" = - ]; then
            $run --out "$out" --fncheck "$spec" > "$out.log" 2>&1
        else
            $run --route "$r" --out "$out" --fncheck "$spec" > "$out.log" 2>&1
        fi
        st=$?
        res=$(tail -1 "$out.log")
        line=$(grep -a '^fncheck: .* checks:' "$out/fncheck.txt" 2>/dev/null | tail -1)
        if [ -n "$line" ]; then
            read -r c s d e < <(echo "$line" | sed 's/^fncheck: \([0-9]*\) checks: \([0-9]*\) same, \([0-9]*\) differ, \([0-9]*\) errors.*/\1 \2 \3 \4/')
            tc=$((tc + c)) ts=$((ts + s)) td=$((td + d)) te=$((te + e))
            printf '%-20s on %-12s %s   (%s/fncheck.txt)\n' "$name" "$r:" "${line#fncheck: }" "$out"
            [ "$d" = 0 ] && [ "$e" = 0 ] || bad=1
        else
            printf '%-20s on %-12s did not finish (exit %s): %s   (%s)\n' "$name" "$r:" "$st" "$res" "$out"
            bad=1
        fi
    done
done
echo "all: $runs runs, $tc checks: $ts same, $td differ, $te errors"
exit $bad
