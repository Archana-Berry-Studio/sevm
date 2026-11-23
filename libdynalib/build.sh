#!/usr/bin/env bash
#
# build.sh — build the libdynalib test harness and its fixtures.
#
# Needs a C11 gcc that emits native PE on Windows (this host: WinLibs MCF).
#   gcc -shared -O2 -o tests/dbtest.dll tests/dbtest.c
#   gcc -O2 -g -std=gnu11 -I. -o tests/dynalib_test.exe tests/dynalib_test.c \
#       dynalib.c dynalib_abi.c dynalib_trampoline.c
#
# Override the target DLLs with SQLITE_DLL / DBTEST_DLL.
set -eu

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SRC_DIR"

CC="${CC:-gcc}"
SQLITE_DLL="${SQLITE_DLL:-C:/Python314/DLLs/sqlite3.dll}"
DBTEST_DLL="${DBTEST_DLL:-tests/dbtest.dll}"

if [ ! -f "$SQLITE_DLL" ]; then
    echo "warn: $SQLITE_DLL not found, T1-T4 will report it and skip" >&2
fi

echo "== dbtest.dll =="
"$CC" -shared -O2 -o tests/dbtest.dll tests/dbtest.c

echo "== dynalib_test.exe =="
"$CC" -O2 -g -std=gnu11 -I. \
    -o tests/dynalib_test.exe \
    tests/dynalib_test.c dynalib.c dynalib_abi.c dynalib_trampoline.c

echo "== diagnostic tools =="
"$CC" -O2 -g -std=gnu11 -I. -o tests/dbg.exe         tests/dbg.c
"$CC" -O2 -g -std=gnu11 -I. -o tests/stub_dump.exe   tests/stub_dump.c \
    dynalib.c dynalib_abi.c dynalib_trampoline.c
"$CC" -O2 -g -std=gnu11 -I. -o tests/crash_locate.exe tests/crash_locate.c \
    dynalib.c dynalib_abi.c dynalib_trampoline.c

echo "== run =="
tests/dynalib_test.exe "$SQLITE_DLL" "$DBTEST_DLL"
