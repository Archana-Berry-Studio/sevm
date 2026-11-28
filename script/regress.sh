#!/usr/bin/env bash
# Run the MochiBerry SEVM self-test suite with a given engine binary.
# usage: script/regress.sh [engine.exe]
#
# With no argument it picks an engine that can actually load the native
# plugins (the canonical build path is rewritten by other processes on this
# host, and a build without -Wl,--export-all-symbols cannot resolve MBJS_*).
ENGINE="${1:-}"
cd "$(dirname "$0")/.." || exit 1
if [ -z "$ENGINE" ]; then
  ENGINE=$(bash script/pick_engine.sh 2>/dev/null)
  [ -n "$ENGINE" ] || { echo "no working engine found" >&2; exit 2; }
fi
TESTS="mochi-core mochi-runtime mochi-shims plugin-load mochicrypto_xcheck
       mochi-dynalib mochi-express mochi-express2 mochi-http-post
       mochijsnative mochi-net-connect mochi-process mochisandbox
       mochisandbox_memlimit mochi-sqlite mochi-tls mochi-tls-socket
       mochi-wasm mochi-worker mochiws"
pass=0; fail=0; failed=""
for t in $TESTS; do
  out=$(timeout 60 "$ENGINE" -m "selftest/$t.js" 2>&1)
  # a FAIL is a FAIL line, a FAILED=n, or a real error — but not an expected
  # "caught: <error>" from the sandbox OOM probes (the sandbox is SUPPOSED to
  # catch those; the probe passes by surviving and printing DONE)
  if printf '%s' "$out" | grep -qiE '^FAIL|FAILED=[1-9]' || \
     (printf '%s' "$out" | grep -qiE 'Error:|ReferenceError|TypeError' && \
      ! printf '%s' "$out" | grep -qiE 'DONE \(process survived\)'); then
    fail=$((fail+1)); failed="$failed $t"
    printf '%-26s FAIL\n' "$t"
    printf '%s' "$out" | grep -iE '^FAIL|FAILED=|Error:' | head -2 | sed 's/^/    /'
  else
    pass=$((pass+1)); printf '%-26s PASS\n' "$t"
  fi
done

# --- HTTP race regression -------------------------------------------------
# The server reads a request off a NON-BLOCKING socket. When the request
# arrives in two TCP segments, recv() reports WSAEWOULDBLOCK; the old code
# treated that as EOF and closed the socket with unread bytes queued, so
# Windows answered with RST and the client saw "Connection was aborted"
# (~20% of short requests). This fires 25 split-send requests and demands
# that every one of them answers 200.
PY=$(command -v python3 || command -v python || echo /c/Python314/python.exe)
if [ -x "$PY" ] || command -v "$PY" >/dev/null 2>&1; then
  for p in $(tasklist 2>/dev/null | grep -iE "mbjs" | awk '{print $2}'); do
    MSYS_NO_PATHCONV=1 taskkill /F /PID "$p" >/dev/null 2>&1
  done
  # subprocess needs a native Windows path, not an MSYS /e/... one
  engine_native=$(printf '%s' "$ENGINE" | sed -E 's|^/([A-Za-z])/|\1:/|')
  race=$("$PY" script/measure_net2.py "$engine_native" 25 2>&1 | head -1)
  if printf '%s' "$race" | grep -qE 'ok=25 fail=0'; then
    printf '%-26s PASS\n' "net-split-send-race"
    pass=$((pass+1))
  else
    printf '%-26s FAIL\n' "net-split-send-race"
    printf '    %s\n' "$race"
    fail=$((fail+1)); failed="$failed net-split-send-race"
  fi
else
  echo "net-split-send-race       SKIP (no python)"
fi

echo "----"
echo "engine=$ENGINE pass=$pass fail=$fail"
[ -n "$failed" ] && echo "failed:$failed"
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)