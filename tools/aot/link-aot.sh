#!/usr/bin/env bash
# link-aot.sh — link a generated SEVM AOT host against the parser-free
# engine profile produced by build-sevm-aot.sh.
#
# Usage:
#   bash tools/aot/link-aot.sh <host.c> <out.exe> [engine-dir]
#
# The link deliberately does NOT use -Wl,--whole-archive: the AOT host only
# needs the engine objects it actually references, and --gc-sections drops
# the rest (the features are already excluded at compile time by the
# MBJS_NO_* guards in the profile).
set -eu

HOST_C="${1:?usage: link-aot.sh <host.c> <out.exe> [engine-dir]}"
OUT_EXE="${2:?usage: link-aot.sh <host.c> <out.exe> [engine-dir]}"
ENGINE="${3:-/e/sevm/build_ex/engine_sevm_aot}"
WORK="${4:-$(dirname "$HOST_C")}"
ENGINE_SRC="${ENGINE_ROOT:-/e/bcf/mochiberry}"

WLDIR="/c/Users/Administrator/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.MCF.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin"
CC="$WLDIR/gcc.exe"
STRIP="$WLDIR/strip.exe"

HOST_WIN="$(cd "$(dirname "$HOST_C")" && pwd -W)/$(basename "$HOST_C")"
OUT_WIN="$(cd "$(dirname "$OUT_EXE")" && pwd -W)/$(basename "$OUT_EXE")"
ENGINE_WIN="$(cd "$ENGINE" && pwd -W)"
ENGINE_SRC_WIN="$(cd "$ENGINE_SRC" && pwd -W)"

WORK_WIN="$(cd "$WORK" && pwd -W)"
CFLAGS="-Os -std=gnu11 -D_GNU_SOURCE -I$ENGINE_SRC_WIN -I$ENGINE_WIN -I$WORK_WIN \
        -ffunction-sections -fdata-sections \
        -fno-asynchronous-unwind-tables -fno-ident -fno-jump-tables"
# --export-all-symbols: the AOT executable doubles as a shared library for
# the native plugins, which resolve the engine's MBJS_* symbols through
# GetModuleHandle(NULL) + GetProcAddress.  The original pipeline used the
# same flag.
LDFLAGS="-Wl,--gc-sections -Wl,--export-all-symbols"

# mbjs-libc.o (prebuilt) references stat64i32; MinGW's UCRT runtime exports
# it as _stat64i32.  Same shim the original pipeline used.
"$CC" $CFLAGS "$HOST_WIN" "$ENGINE_WIN/libmbjs-sevm-aot.a" \
      $LDFLAGS -o "$OUT_WIN" -lm -lpthread \
      -Wl,--defsym=stat64i32=_stat64i32

cp -f "$OUT_EXE" "$OUT_EXE.stripped"
"$STRIP" -s "$OUT_WIN.stripped"

printf '   %-28s %10s\n' "$(basename "$OUT_EXE")" "$(wc -c < "$OUT_EXE")"
printf '   %-28s %10s\n' "$(basename "$OUT_EXE").stripped" "$(wc -c < "$OUT_EXE.stripped")"
