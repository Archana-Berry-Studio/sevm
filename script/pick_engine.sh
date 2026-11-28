#!/usr/bin/env bash
# Print the path of an engine binary that can actually load native plugins.
#
# The canonical build path (E:/bcf/mochiberry/build_manual/mbjs.exe) is
# rewritten by other processes on this host; some of those builds link
# without -Wl,--export-all-symbols and then no plugin DLL can resolve the
# MBJS_* symbols it needs ("resolveEngine failed at MBJS_NewCModule").
# Probe the candidates and print the first one that loads all plugins.
#
# usage: bash script/pick_engine.sh [engine-dir]
DIR="${1:-E:/bcf/mochiberry/build_manual}"
cd "$(dirname "$0")/.." || exit 1
for e in mbjs-host.exe mbjs.exe mbjs.new.exe mbjs.mochi.exe; do
  p="$DIR/$e"
  [ -f "$p" ] || continue
  out=$(timeout 90 "$p" -m selftest/plugin-load.js 2>&1 | tail -1)
  case "$out" in
    ALL_OK*) echo "$p"; exit 0 ;;
  esac
done
echo "no engine in $DIR can load the native plugins" >&2
exit 1