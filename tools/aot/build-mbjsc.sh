#!/usr/bin/env bash
# build-mbjsc.sh — rebuild mbjsc (the bytecode compiler) from the SAME source
# tree as the AOT runtime.
#
# WHY
# ---
# The prebuilt build_manual/mbjsc.exe in the MochiBerry tree is STALE: it
# emits bytecode version 27, while the current engine sources (mbjs.c,
# BC_VERSION 29) only accept version 29:
#
#     SyntaxError: invalid version (27 expected=29)
#
# An AOT toolchain needs the compiler and the runtime to agree on the
# bytecode format.  This script therefore builds mbjsc with:
#
#   * the same MBJS_NO_* feature guards as the AOT runtime (so the compiler
#     and the runtime expose the same intrinsic set), but WITHOUT
#     MBJS_DISABLE_PARSER (the compiler has to parse JavaScript)
#   * the same prebuilt TUs the AOT engine profile uses
#
# Usage: bash tools/aot/build-mbjsc.sh [outdir]
set -eu

ENGINE="${ENGINE_ROOT:-/e/bcf/mochiberry}"
OUT="${1:-/e/sevm/build_ex/mbjsc_v29}"
WLDIR="/c/Users/Administrator/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.MCF.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin"
CC="$WLDIR/gcc.exe"
AR="$WLDIR/ar.exe"

ENGINE_WIN="$(cd "$ENGINE" && pwd -W 2>/dev/null || echo "$ENGINE")"
OUT_WIN="$(mkdir -p "$OUT" && cd "$OUT" && pwd -W)"

# Same feature set as the AOT runtime, but the parser stays enabled: this is
# the compiler, not the shipped runtime.
# Same feature set as the AOT runtime (see build-sevm-aot.sh): RegExp and
# TypedArrays are kept, the parser is enabled because this is the compiler.
DEFS="-DMBJS_NO_DATE -DMBJS_NO_PROXY -DMBJS_NO_MAPSET \
      -DMBJS_NO_WEAKREF -DMBJS_NO_ATOB \
      -DMBJS_NO_PERFORMANCE -DMBJS_NO_DOM_EXCEPTION -DMBJS_NO_ATOMICS"

CFLAGS="-Os -std=gnu11 -D_GNU_SOURCE -I$ENGINE_WIN \
        -ffunction-sections -fdata-sections \
        -fno-asynchronous-unwind-tables -fno-ident -fno-jump-tables"

SRC="mbjs dtoa libregexp libunicode"
PREBUILT="mbjs-mem mbjs-mulch mbjs-superset mbjs-inline mbjs-libc mbjs-opt mbjs-llvm mbjs-jit"
COMP="mbjsc mbjsc-compile mbjsc-emit mbjsc-module mbjsc-namelist mbjsc-pseudo"

mkdir -p "$OUT/objects"
pids=""
for f in $SRC $COMP; do
    ( cd "$ENGINE" && "$CC" $CFLAGS $DEFS -c "$f.c" -o "$OUT_WIN/objects/$f.o" ) &
    pids="$pids $!"
done
fail=0
for p in $pids; do wait "$p" || fail=1; done
[ "$fail" -eq 0 ] || { echo "FAIL: compiler compile"; exit 1; }

for f in $PREBUILT; do
    cp -f "$ENGINE/build_manual/$f.o" "$OUT/objects/$f.o"
done

OBJS=""
for f in $SRC $PREBUILT $COMP; do OBJS="$OBJS $OUT_WIN/objects/$f.o"; done
rm -f "$OUT/mbjsc.exe"
"$CC" $CFLAGS $OBJS -o "$OUT_WIN/mbjsc.exe" -lm -lpthread \
      -Wl,--defsym=stat64i32=_stat64i32

printf '   %-24s %10s\n' "mbjsc.exe" "$(wc -c < "$OUT/mbjsc.exe")"
echo "[OK] mbjsc built in $OUT"
