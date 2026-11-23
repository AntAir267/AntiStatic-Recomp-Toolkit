#!/usr/bin/env bash
# tests/stubgame/run.sh: the whole chain with no game files. Builds the stub game (game.c) as a
# 32-bit Windows program, translates it, builds the C with the runtime core, runs it headless and
# checks its result.txt against expected.txt and its exit code (42).
#   ZIG=<zig>   a zig 0.13+ (zig cc targets 32-bit Windows; a mingw i686 gcc works too: CC_WIN,
#               with DLLTOOL=i686-w64-mingw32-dlltool)
#   SDL3=<dir>  an SDL3 build (include/, lib/libSDL3.a)
#   PYTHON=<python with capstone>
#   FFmpeg's development libraries (libavformat, libavcodec, libswresample: the Miles stand-in's streams)
set -eu
cd "$(dirname "$0")/../.."
B=build/stubgame
ZIG=${ZIG:-zig}; SDL3=${SDL3:?set SDL3 to an SDL3 build}; PY=${PYTHON:-python3}
CC_WIN=${CC_WIN:-"$ZIG cc -target x86-windows-gnu"}
mkdir -p $B/game/CD $B/gen $B/obj $B/user
printf "on the disc" > $B/game/CD/disc.txt
$CC_WIN -O1 -fno-stack-protector -c tests/stubgame/game.c -o $B/game.o
# the two C runtime functions it imports (setjmp / longjmp) and DirectSoundCreate, as import
# libraries of their own
printf 'LIBRARY msvcrt.dll\nEXPORTS\n_setjmp3\nlongjmp\n' > $B/msvcrt.def
${DLLTOOL:-$ZIG dlltool} -m i386 -d $B/msvcrt.def -l $B/libmsvcrt_stub.a
printf 'LIBRARY dsound.dll\nEXPORTS\nDirectSoundCreate@12\n' > $B/dsound.def
${DLLTOOL:-$ZIG dlltool} -m i386 -k -d $B/dsound.def -l $B/libdsound_stub.a
printf 'LIBRARY mss32.dll\nEXPORTS\n' > $B/mss32.def
for f in AIL_startup@0 AIL_waveOutOpen@16 AIL_allocate_sample_handle@4 AIL_set_sample_file@12 AIL_start_sample@4 \
         AIL_sample_status@4 AIL_set_sample_volume@8 AIL_release_sample_handle@4 AIL_shutdown@0; do
    echo "$f == _$f" >> $B/mss32.def               # (Miles exports the decorated names)
done
${DLLTOOL:-$ZIG dlltool} -m i386 -d $B/mss32.def -l $B/libmss32_stub.a
printf 'LIBRARY stubext.dll\nEXPORTS\nStubExtValue@0\n' > $B/stubext.def   # (only the port stands in for it)
${DLLTOOL:-$ZIG dlltool} -m i386 -k -d $B/stubext.def -l $B/libstubext_stub.a
$CC_WIN -nostdlib -Wl,--entry=start -Wl,--subsystem,windows -o $B/game/stub.exe $B/game.o -lkernel32 -ladvapi32 -luser32 $B/libmsvcrt_stub.a $B/libdsound_stub.a $B/libmss32_stub.a $B/libstubext_stub.a
sha=$(sha256sum $B/game/stub.exe | cut -d' ' -f1)
# from the export table: the rewrite test's two functions (port/rewrite.c), the game's own setjmp pair
$PY - $B/game/stub.exe $B <<'PY'
import sys
sys.path.insert(0, 'translator')
from pe import PE
m = PE(sys.argv[1])
mix, rotl = m.export('_stub_mix') or m.export('stub_mix'), m.export('_stub_rotl@8') or m.export('stub_rotl@8') or m.export('stub_rotl')
assert mix and rotl, sorted(m.exports)
open(sys.argv[2] + '/rewrites.txt', 'w').write('%08x   # stub_mix (port/rewrite.c)\n' % mix)
open(sys.argv[2] + '/gen/rewrite_addrs.h', 'w').write(
    '#define STUB_ROTL 0x%08xu\n#define STUB_MIX 0x%08xu\n#define REGISTER_MIX(f) DECOMP(%x, f)\n'
    '#define STACKFIX_MIX(n) STACKFIX(%x, n, hook_dc_%x)\n#define MIX_ORIG F_%08x_orig\n'
    % (rotl, mix, mix, mix, mix, mix))
# the port's own hook on stub_mix, with the rewrite inside it (INNER_HOOKS)
# and the toolkit's test throw on eh_throw1 (game.c), which eh_cxx calls inside its try block
th = m.export('eh_throw1')
assert th, sorted(m.exports)
open(sys.argv[2] + '/hooks_cfg.py', 'w').write('HOOKS = {0x%x: "mix_outer", 0x%x: "test_throw"}\nINNER_HOOKS = True\n'
                                             % (mix, th))
open(sys.argv[2] + '/fncheck.spec', 'w').write(open('tests/stubgame/fncheck.spec').read().replace('@MIX@', '%x' % mix))
# the game's own setjmp / longjmp (game.c): the build config's SETJMP / LONGJMP
sj, lj = m.export('my_setjmp'), m.export('my_longjmp')
assert sj and lj, sorted(m.exports)
# the stand-in for MSVCR90's __CxxFrameHandler3 (game.c eh_cxx): the build config's CXX_FRAME_HANDLERS
ch = m.export('eh_cxx_handler')
assert ch, sorted(m.exports)
open(sys.argv[2] + '/setjmp_cfg.py', 'w').write('SETJMP = [0x%x]\nLONGJMP = [0x%x]\nCXX_FRAME_HANDLERS = [0x%x]\n'
                                              % (sj, lj, ch))
PY
cat > $B/stub_cfg.py <<CFG
NAME = 'stub-game'
MODULES = [dict(file='stub.exe', sha256='$sha')]
EXTRA_ENTRIES = []
NOT_ENTRIES = []
JUMP_TABLES = {}
NORETURN_IMPORTS = ['ExitProcess', 'ExitThread', 'TerminateProcess']
REWRITES = 'rewrites.txt'
CFG
cat $B/setjmp_cfg.py $B/hooks_cfg.py >> $B/stub_cfg.py
$PY translator/recomp.py $B/game $B/stub_cfg.py $B/gen > $B/translate.log
CF="-std=gnu11 -O1 -fno-strict-aliasing -frounding-math -w -DCPU_FAST_MEM -Itests/stubgame/port -Iruntime/win32 -Iruntime/cpu -Iruntime/rewrite -I$B/gen -I$SDL3/include"
for f in $B/gen/*.c runtime/cpu/fpu.c runtime/win32/*.c runtime/rewrite/*.c runtime/music/mss32.c tests/stubgame/port/*.c; do
    gcc $CF -c "$f" -o $B/obj/$(basename "$(dirname "$f")")_$(basename "$f" .c).o
done
gcc -rdynamic -o $B/stub $B/obj/*.o "$SDL3/lib/libSDL3.a" -pthread -lm -ldl -lz -lavformat -lavcodec -lswresample -lavutil
# twice: the rewrite in, then left out (STUB_DECOMP_OFF=all); both must give expected.txt
fail=0
for mode in on off; do
    rm -f $B/user/result.txt
    set +e
    if [ $mode = on ]; then off=; else off=all; fi
    STUB_DECOMP_OFF=$off $B/stub --game "$PWD/$B/game" --user "$PWD/$B/user" --test --hidden --logfile "$PWD/$B/run_$mode.log" >/dev/null 2>&1
    code=$?
    set -e
    want="rewrite: $([ $mode = on ] && echo 1 || echo 0) rewritten functions in, 1 stack fixes"
    if [ $code = 42 ] && cmp -s $B/user/result.txt tests/stubgame/expected.txt && grep -q "$want" $B/run_$mode.log &&
       grep -q "stub: the port's hook on stub_mix ran" $B/run_$mode.log; then
        echo "stub game (rewrites $mode): ok"
    else
        echo "stub game (rewrites $mode): FAILED (exit $code; $B/run_$mode.log, diff below)"
        grep -q "$want" $B/run_$mode.log || echo "(no log line: $want)"
        grep -q "stub: the port's hook" $B/run_$mode.log || echo "(the port's hook around the rewrite did not run: INNER_HOOKS)"
        diff tests/stubgame/expected.txt $B/user/result.txt || true
        fail=1
    fi
done
# The test throw (eh.c eh_test_throw, HOOKS 'test_throw'): eh_throw1's 1st call throws a C++ int from
# its first instruction instead; eh_cxx's catch takes it, so the result is the same
rm -f $B/user/result.txt
set +e
STUB_TEST_THROW=1 $B/stub --game "$PWD/$B/game" --user "$PWD/$B/user" --test --hidden --logfile "$PWD/$B/run_throw.log" >/dev/null 2>&1
code=$?
set -e
if [ $code = 42 ] && cmp -s $B/user/result.txt tests/stubgame/expected.txt && grep -q "test throw: call 1" $B/run_throw.log; then
    echo "stub game (test throw): ok, caught and gone on from"
else
    echo "stub game (test throw): FAILED (exit $code; $B/run_throw.log)"
    fail=1
fi
# The per-function check (runtime/rewrite/fncheck.c) through the A/B script: stub_mix's rewrite
# against its translation must be the same on every input of fncheck.spec, and the deliberate bug
# (STUB_MIX_MUTANT=1, an input the game never gives) must show as DIFFER
G=(-- $B/stub --game "$PWD/$B/game")
st=0; tools/rewrite/ab.sh --env STUB --out $B/fncheck --lock none --frames 10 --fncheck $B/fncheck.spec "${G[@]}" > $B/fncheck.log 2>&1 || st=$?
line=$(grep -a '^fncheck: .* checks' $B/fncheck/fncheck.txt 2>/dev/null | tail -1)
if [ $st = 0 ] && echo "$line" | grep -q ' 0 differ, 0 errors' && ! echo "$line" | grep -q ': 0 checks'; then
    echo "stub game (fncheck): ok (${line#fncheck: })"
else
    echo "stub game (fncheck): FAILED (exit $st: $line; $B/fncheck/fncheck.txt, $B/fncheck/log_fncheck.txt)"; fail=1
fi
st=0; STUB_MIX_MUTANT=1 tools/rewrite/ab.sh --env STUB --out $B/fncheck_mutant --lock none --frames 10 --fncheck $B/fncheck.spec "${G[@]}" > $B/fncheck_mutant.log 2>&1 || st=$?
line=$(grep -a '^fncheck: .* checks' $B/fncheck_mutant/fncheck.txt 2>/dev/null | tail -1)
if [ $st = 1 ] && grep -a -q '^DIFFER mix_other {0xdeadbeef' $B/fncheck_mutant/fncheck.txt && echo "$line" | grep -q ' 0 errors' &&
   ! grep -a '^DIFFER' $B/fncheck_mutant/fncheck.txt | grep -v -q deadbeef; then
    echo "stub game (fncheck mutant): ok, DIFFER where the bug is (${line#fncheck: })"
else
    echo "stub game (fncheck mutant): FAILED (exit $st: $line; $B/fncheck_mutant/fncheck.txt)"; fail=1
fi
# The register audit (runtime/rewrite/audit.c, STUB_AUDIT=1): the rewrite checked against the
# translation on the game's own call; the deliberate ebx bug (STUB_MIX_MUTANT=ebx) must show
for m in none ebx; do
    rm -f $B/audit_$m.txt $B/user/result.txt
    STUB_MIX_MUTANT=$([ $m = none ] || echo $m) STUB_AUDIT=1 STUB_AUDIT_OUT="$PWD/$B/audit_$m.txt" \
        $B/stub --game "$PWD/$B/game" --user "$PWD/$B/user" --test --logfile "$PWD/$B/audit_$m.log" >/dev/null 2>&1 || true
done
if grep -q 'REACHED' $B/audit_none.txt 2>/dev/null && ! grep -q 'DIFF\|NOCHILD' $B/audit_none.txt &&
   grep -q ' DIFF .* ebx T=' $B/audit_ebx.txt 2>/dev/null; then
    echo "stub game (register audit): ok, same, and the mutant's ebx found"
else
    echo "stub game (register audit): FAILED ($B/audit_none.txt, $B/audit_ebx.txt)"; fail=1
fi
# its argument slots (STUB_AUDIT_SLOTS=1): the slot mutant (STUB_MIX_MUTANT=slot, b's slot scribbled)
# must show there and only there (the plain audit doesn't look at slots); the forked children are
# isolated (no note that they could not be) and the audited run still gives expected.txt
mix=$(cut -c1-8 $B/rewrites.txt | head -1)
aud() {    # aud NAME MUTANT VAR=VALUE...: an audited run, its report $B/audit_NAME.txt
    local n=$1 m=$2; shift 2
    rm -f $B/audit_$n.txt $B/user/result.txt
    env STUB_MIX_MUTANT=$m STUB_AUDIT=1 STUB_AUDIT_OUT="$PWD/$B/audit_$n.txt" "$@" \
        $B/stub --game "$PWD/$B/game" --user "$PWD/$B/user" --test --logfile "$PWD/$B/audit_$n.log" >/dev/null 2>&1 || true
}
aud slots_none "" STUB_AUDIT_SLOTS=1
cmp -s $B/user/result.txt tests/stubgame/expected.txt && res_ok=1 || res_ok=0
aud slots_slot slot STUB_AUDIT_SLOTS=1
aud plain_slot slot
if grep -q 'REACHED' $B/audit_slots_none.txt 2>/dev/null && ! grep -q 'DIFF\|NOCHILD\|could not be isolated' $B/audit_slots_none.txt &&
   [ $res_ok = 1 ] && grep -q ' DIFF .* slot1 T=' $B/audit_slots_slot.txt 2>/dev/null &&
   grep -q 'REACHED' $B/audit_plain_slot.txt 2>/dev/null && ! grep -q 'DIFF' $B/audit_plain_slot.txt; then
    echo "stub game (register audit, argument slots): ok, same, and the mutant's slot found only with AUDIT_SLOTS"
else
    echo "stub game (register audit, argument slots): FAILED ($B/audit_slots_none.txt, $B/audit_slots_slot.txt, $B/audit_plain_slot.txt; result.txt ok=$res_ok)"; fail=1
fi
# STUB_AUDIT_SKIP: the ebx mutant left out of the audit is not reported
aud skip_ebx ebx STUB_AUDIT_SKIP=$mix
if grep -q "^# $mix not audited (AUDIT_SKIP)" $B/audit_skip_ebx.txt 2>/dev/null && ! grep -q 'REACHED\|DIFF' $B/audit_skip_ebx.txt; then
    echo "stub game (register audit, AUDIT_SKIP): ok, the skipped function not audited"
else
    echo "stub game (register audit, AUDIT_SKIP): FAILED ($B/audit_skip_ebx.txt)"; fail=1
fi
# The two-run log (STUB_AUDIT_LOG=PREFIX, no fork): a run of the translation (STUB_DECOMP_OFF=all) and
# runs of the rewrites each write their log; tools/rewrite/auditlog.py compares. The clean rewrite
# must be the same, both mutants must show; the logged runs still give expected.txt
alog() {   # alog PREFIX DECOMP_OFF MUTANT
    rm -f $B/user/result.txt "$PWD/$1.$([ "$2" = all ] && echo a || echo b).txt"
    STUB_MIX_MUTANT=$3 STUB_DECOMP_OFF=$2 STUB_AUDIT_LOG="$PWD/$1" \
        $B/stub --game "$PWD/$B/game" --user "$PWD/$B/user" --test --logfile "$PWD/$1.log" >/dev/null 2>&1
    local code=$?
    [ $code = 42 ] && cmp -s $B/user/result.txt tests/stubgame/expected.txt
}
lres=0
alog $B/alog all "" || lres=1
for m in none ebx slot; do
    [ $m = none ] || cp $B/alog.a.txt $B/alog_$m.a.txt 2>/dev/null || true
    p=$B/alog; [ $m = none ] || p=$B/alog_$m
    alog $p "" "$([ $m = none ] || echo $m)" || { [ $m = none ] && lres=1; }
    st=0; $PY tools/rewrite/auditlog.py $p > $p.cmp.txt 2>&1 || st=$?
    eval "alog_st_$m=$st"
done
if [ $lres = 0 ] && [ $alog_st_none = 0 ] && grep -q '^auditlog: [1-9][0-9]* functions: [1-9][0-9]* same, 0 differ' $B/alog.cmp.txt &&
   [ $alog_st_ebx = 1 ] && grep -q "^$mix DIFF call 0: .*ebx T== R=" $B/alog_ebx.cmp.txt &&
   [ $alog_st_slot = 0 ] && grep -q "^$mix SLOTS call 0: slot1 T== R=.* (one run only)" $B/alog_slot.cmp.txt &&
   grep -q ' 1 differ in argument slots only' $B/alog_slot.cmp.txt; then
    echo "stub game (two-run audit log): ok, same ($(tail -1 $B/alog.cmp.txt | sed 's/^auditlog: //')), the ebx mutant a DIFF, the slot mutant counted apart (SLOTS)"
else
    echo "stub game (two-run audit log): FAILED (runs ok=$((1 - lres)); $B/alog.cmp.txt, $B/alog_ebx.cmp.txt, $B/alog_slot.cmp.txt)"; fail=1
fi
# The Windows host (runtime/win): with SDL3_WIN=<an SDL3 built for x86_64-windows-gnu, include/ and
# lib/libSDL3.a> (and ZLIB_WIN likewise) the same translation is compiled and linked for 64-bit
# Windows with Zig; it can't be run here (no Windows), so this checks that it builds
if [ -n "${SDL3_WIN:-}" ]; then
    W=$B/win; mkdir -p $W
    WCC="$ZIG cc -target x86_64-windows-gnu"
    WCF="-std=gnu11 -O1 -fno-strict-aliasing -w -DCPU_FAST_MEM -DPORT_MILES_NO_FFMPEG -Itests/stubgame/port -Iruntime/win32 -Iruntime/cpu -Iruntime/rewrite -I$B/gen -I$SDL3_WIN/include ${VK_INC:+-I$VK_INC}"
    wfail=0
    for f in $B/gen/*.c runtime/cpu/fpu.c runtime/win32/*.c runtime/rewrite/*.c runtime/music/mss32.c tests/stubgame/port/*.c runtime/win/platform.c; do
        extra=; case $f in $B/gen/*) ;; *) extra="-Iruntime/win -include runtime/win/compat.h ${ZLIB_WIN:+-I$ZLIB_WIN/include}";; esac
        $WCC $WCF $extra -c "$f" -o $W/$(basename "$(dirname "$f")")_$(basename "$f" .c).o 2>> $W/build.log || { echo "  (Windows) $f did not compile"; wfail=1; }
    done
    [ $wfail = 0 ] && $WCC -o $W/stub.exe $W/*.o -static "$SDL3_WIN/lib/libSDL3.a" ${ZLIB_WIN:+$ZLIB_WIN/lib/libz.a} \
        -lm -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lcfgmgr32 \
        -lshell32 -lpsapi -ldbghelp -Wl,--stack,8388608 >> $W/build.log 2>&1 || wfail=1
    if [ $wfail = 0 ] && [ -s $W/stub.exe ]; then
        echo "stub game (Windows host): built $(stat -c %s $W/stub.exe) bytes (not run: no Windows here)"
    else
        echo "stub game (Windows host): FAILED to build ($W/build.log)"; fail=1
    fi
fi
exit $fail
