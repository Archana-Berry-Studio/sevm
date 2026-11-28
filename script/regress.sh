#!/usr/bin/env bash
# Run the MochiBerry SEVM self-test suite with a given engine binary.
# usage: script/regress.sh <engine.exe>
ENGINE="${1:-E:/bcf/mochiberry/build_manual/mbjs.exe}"
cd "$(dirname "$0")/.." || exit 1
TESTS="mochi-core mochi-runtime mochi-shims mochicrypto_xcheck mochi-dynalib
       mochi-express mochi-express2 mochi-http-post mochijsnative
       mochi-net-connect mochi-process mochisandbox mochisandbox_memlimit
       mochi-sqlite mochi-tls mochi-tls-socket mochi-wasm mochi-worker mochiws"
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
echo "----"
echo "engine=$ENGINE pass=$pass fail=$fail"
[ -n "$failed" ] && echo "failed:$failed"
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)