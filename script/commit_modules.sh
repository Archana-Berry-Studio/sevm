#!/bin/bash
# Per-module commits for E:/sevm per Nonamey's instruction ("per modul per mochi modul").
set -e
cd /e/sevm

commit() {
  local msg="$1"; shift
  git add -- "$@"
  if ! git diff --cached --quiet; then
    git commit -q -m "$msg" && echo "COMMITTED: $msg"
  else
    echo "SKIP (empty): $msg"
  fi
}

# 1. mochizlib — native gzip/raw/crc32 + per-module layout + MBJS_EXCEPTION contract
commit "mochizlib: gzip/raw/crc32Hex native + per-module lib/ layout + MBJS_EXCEPTION error contract" \
  native/mochizlib.c mochi_modules/mochizlib.dll mochi_modules/mochizlib/ \
  node_modules/mochizlib/index.js node_modules/mochizlib/README.md node_modules/mochizlib/package.json

# 2. mochifs — MBJS_EXCEPTION contract + per-module layout
commit "mochifs: MBJS_EXCEPTION error contract fix + per-module lib/ layout" \
  native/mochifs.c mochi_modules/mochifs.dll mochi_modules/mochifs/ \
  node_modules/mochifs/index.js node_modules/mochifs/README.md node_modules/mochifs/package.json

# 3. mochicrypto — MBJS_EXCEPTION contract + per-module layout
commit "mochicrypto: MBJS_EXCEPTION error contract fix + per-module lib/ layout" \
  native/mochicrypto.c mochi_modules/mochicrypto.dll mochi_modules/mochicrypto/ \
  node_modules/mochicrypto/README.md node_modules/mochicrypto/package.json

# 4. mochiws — MBJS_EXCEPTION contract + per-module layout
commit "mochiws: MBJS_EXCEPTION error contract fix + per-module lib/ layout" \
  native/mochiws.c mochi_modules/mochiws.dll mochi_modules/mochiws/ \
  node_modules/mochiws/README.md node_modules/mochiws/package.json

# 5. mochinode — per-module lib/ resolution + async globals install
commit "mochinode: resolve native binaries from <mod>/lib/ first + async globals install" \
  node_modules/mochinode/index.js

# 6. mochiprocess — arch/platform/exit/uptime + metadata
commit "mochiprocess: arch/platform/exit/uptime + package metadata" \
  node_modules/mochiprocess/index.js node_modules/mochiprocess/README.md node_modules/mochiprocess/package.json

# 7. mochitimers — new timer shim
commit "mochitimers: setTimeout/setInterval/clear* shim over mbjs:os" \
  node_modules/mochitimers/index.js

# 8. mochiglobals — new globals installer
commit "mochiglobals: web/Node globals installer (URL, TextEncoder, AbortController, fetch, crypto, timers)" \
  node_modules/mochiglobals/index.js

# 9. remaining native modules — move binaries to per-module lib/ layout
commit "native modules: relocate binaries into per-module lib/ layout" \
  mochi_modules/mochinode_net.dll mochi_modules/mochinode_net/ \
  mochi_modules/mochinode_dynalib.dll mochi_modules/mochinode_dynalib/ \
  mochi_modules/mochinode_sqlite.dll mochi_modules/mochinode_sqlite/ \
  mochi_modules/mochiwasmy.dll mochi_modules/mochiwasmy/ \
  mochi_modules/mochijsnative.dll mochi_modules/mochijsnative/

# 10. repo structure & docs (everything else, including generated metadata)
commit "repo: per-module manifests, docs/, mochiberry/ assets, honest MISSING_SHIMS, scripts" \
  script/gen_manifests.py script/populate_modules.py \
  docs/ mochiberry/ \
  MISSING_SHIMS.json mochi_modules/README.md mochi_modules/manifest.json \
  node_modules/README.md node_modules/package.json \
  node_modules/mochiasync_hooks/README.md node_modules/mochibuffer/README.md \
  node_modules/mochidiagnostics_channel/README.md node_modules/mochidiagnostics_channel/package.json \
  node_modules/mochievents/README.md node_modules/mochihttp/README.md \
  node_modules/mochihttps/README.md node_modules/mochijsnative/README.md \
  node_modules/mochimodule/README.md node_modules/mochimodule/package.json \
  node_modules/mochinet/README.md node_modules/mochios/README.md \
  node_modules/mochipath/README.md node_modules/mochiperf_hooks/README.md \
  node_modules/mochiperf_hooks/package.json node_modules/mochistream/README.md \
  node_modules/mochitls/README.md node_modules/mochiurl/README.md \
  node_modules/mochiutil/README.md node_modules/mochiworker_threads/README.md \
  node_modules/mochiworker_threads/package.json \
  selftest/mochiws.js .gitignore

echo "=== remaining uncommitted ==="
git status --short | head -30
