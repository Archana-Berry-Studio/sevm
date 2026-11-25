#!/usr/bin/env bash
# build-sevm-aot.sh — build the SEVM AOT runtime: a parser-free, feature-pruned
# MochiBerry engine that can execute pre-compiled bytecode only.
#
# WHY THIS EXISTS
# ---------------
# The stock SEVM AOT build (node_modules/mochijsnative/build.js) links the
# full engine archive with -Wl,--whole-archive and keeps every feature:
#
#     sevm_ex_aot.exe / sevm_aot_bundle.exe   ~1.38 MB
#
# But an AOT binary never parses JS at runtime: every module is compiled to
# bytecode ahead of time (mbjsc -e) and embedded in the executable.  The
# parser + bytecode compiler are therefore dead weight in the shipped file.
#
# This script builds a dedicated profile:
#
#   * -DMBJS_DISABLE_PARSER      drops lexer/parser/AST/bytecode-compiler
#   * MBJS_NO_* feature guards   drops Date, RegExp, Proxy, Map/Set,
#                                TypedArrays, WeakRef, AToB, Performance,
#                                DOMException, Atomics
#   * -ffunction-sections        one section per function so --gc-sections can
#   * -fdata-sections            actually collect the dropped features
#   * -fno-asynchronous-unwind-tables   MANDATORY on PE/COFF: otherwise the
#                                .pdata/.xdata pairs keep every .text section
#                                reachable and --gc-sections collects nothing
#   * -fno-jump-tables           the shared jump-table .rdata blob otherwise
#                                pins JSON/RegExp/TypedArray code
#
# KEEP: JSON (express/sqlite use JSON.parse/stringify) and BigInt (the
# dynalib bridge returns BigInt values) plus Promise (language core).
#
# Usage:
#   bash tools/aot/build-sevm-aot.sh [outdir]
#
# Outputs (outdir):
#   libmbjs-sevm-aot.a   static engine archive for the AOT host
#   objects/             the .o files (kept for inspection)
set -eu

ENGINE="${ENGINE_ROOT:-/e/bcf/mochiberry}"
OUT="${1:-/e/sevm/build_ex/engine_sevm_aot}"
WLDIR="/c/Users/Administrator/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.MCF.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin"
CC="$WLDIR/gcc.exe"
AR="$WLDIR/ar.exe"

ENGINE_WIN="$(cd "$ENGINE" && pwd -W 2>/dev/null || echo "$ENGINE")"
OUT_WIN="$(mkdir -p "$OUT" && cd "$OUT" && pwd -W)"

# Feature set actually used by the SEVM app (measured by grepping the JS):
#   JSON.parse / JSON.stringify (mochinode, net/http, server.js)
#   BigInt (dynalib bridge)   Promise (async/await everywhere)
# Everything else is dropped.
# KEEP: RegExp (mochinode/index.js uses regex literals) and TypedArrays
# (mochinode/lib/process.js uses Uint8Array/ArrayBuffer).  Dropping either
# breaks the app at compile time.
DEFS="-DMBJS_DISABLE_PARSER \
      -DMBJS_NO_DATE -DMBJS_NO_PROXY -DMBJS_NO_MAPSET \
      -DMBJS_NO_WEAKREF -DMBJS_NO_ATOB \
      -DMBJS_NO_PERFORMANCE -DMBJS_NO_DOM_EXCEPTION -DMBJS_NO_ATOMICS"

CFLAGS="-Os -std=gnu11 -D_GNU_SOURCE -I$ENGINE_WIN \
        -ffunction-sections -fdata-sections \
        -fno-asynchronous-unwind-tables -fno-ident -fno-jump-tables"

LDFLAGS="-Wl,--gc-sections"

# Engine TUs rebuilt from source.  mbjs-jit / mbjs-llvm are deliberately NOT
# included: they are embedder-facing code generators, an AOT host never calls
# them, and --gc-sections would drop them anyway.
SRC="mbjs dtoa libregexp libunicode"

# TUs whose .c file is not present in this source tree (only the prebuilt
# object exists): the memory arena (mbjs-mem), the mulch allocator
# (mbjs-mulch), the language-superset table (mbjs-superset), the inline-C
# tier (mbjs-inline) and the standard-library modules mbjs:std / mbjs:os
# (mbjs-libc).  They are reused as-is.
#
# mbjs-libc references MBJS_Eval, which stays available and reports
# "eval is not supported" when the parser is disabled; none of them
# reference the parser symbols directly (verified with nm -u).
PREBUILT="mbjs-mem mbjs-mulch mbjs-superset mbjs-inline mbjs-libc"

mkdir -p "$OUT/objects"
pids=""
for f in $SRC; do
    ( cd "$ENGINE" && "$CC" $CFLAGS $DEFS -c "$f.c" -o "$OUT_WIN/objects/$f.o" ) &
    pids="$pids $!"
done
fail=0
for p in $pids; do wait "$p" || fail=1; done
[ "$fail" -eq 0 ] || { echo "FAIL: engine compile"; exit 1; }

for f in $PREBUILT; do
    cp -f "$ENGINE/build_manual/$f.o" "$OUT/objects/$f.o"
done

# MochiBake: the engine-internal organ that reads native-plugin PE export/import
# tables and materialises the DLLs an app actually calls back onto disk at
# startup.  It is compiled into the engine archive, not shipped as a tool.
TOOLS="$(cd "$(dirname "$0")" && pwd)"
TOOLS_WIN="$(cd "$TOOLS" && pwd -W)"
OBJS_EXTRA=""
if [ -f "$TOOLS/mbjs-bake.c" ]; then
    "$CC" $CFLAGS $DEFS -I"$ENGINE_WIN" -c "$TOOLS_WIN/mbjs-bake.c" -o "$OUT_WIN/objects/mbjs-bake.o" \
        || { echo "FAIL: mbjs-bake compile"; exit 1; }
    OBJS_EXTRA=" $OUT_WIN/objects/mbjs-bake.o"
fi

OBJS=""
for f in $SRC $PREBUILT; do OBJS="$OBJS $OUT_WIN/objects/$f.o"; done
OBJS="$OBJS$OBJS_EXTRA"
rm -f "$OUT/libmbjs-sevm-aot.a"
"$AR" rcs "$OUT_WIN/libmbjs-sevm-aot.a" $OBJS

printf '   %-24s %10s\n' "libmbjs-sevm-aot.a" "$(wc -c < "$OUT/libmbjs-sevm-aot.a")"
echo "[OK] engine profile built in $OUT"
