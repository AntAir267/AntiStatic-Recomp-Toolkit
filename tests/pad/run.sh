#!/usr/bin/env bash
# tests/pad: runtime/win32/pad.c alone against a fake DirectInput (pad_test.c), with one game
# joystick, with two, and one per controller (0). (pad.c is copied out first so its "runtime.h" is this folder's.)
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/pad_test
cp runtime/win32/pad.c build/pad_test/pad.c
fail=0
for J in 1 2 0 -1; do
    ${CC:-gcc} -O1 -Wall -DPORT_PAD_JOYS=$J -Itests/pad -Iruntime/win32 -o build/pad_test/pad_test$J \
        tests/pad/pad_test.c build/pad_test/pad.c -lm
    echo "-- one game joystick per player up to $J"
    build/pad_test/pad_test$J || fail=1
    build/pad_test/pad_test$J script || fail=1
    [ $J -lt 1 ] || build/pad_test/pad_test$J film || fail=1
    [ $J -lt 1 ] || build/pad_test/pad_test$J split || fail=1
done
exit $fail
