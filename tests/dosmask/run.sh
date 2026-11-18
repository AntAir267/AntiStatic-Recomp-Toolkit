#!/usr/bin/env bash
# tests/dosmask: runtime/win32/dosmask.h (FindFirstFile's masks) against Windows' answers
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/dosmask
${CC:-gcc} -O1 -Wall -Iruntime/win32 -o build/dosmask/test tests/dosmask/test.c
build/dosmask/test
