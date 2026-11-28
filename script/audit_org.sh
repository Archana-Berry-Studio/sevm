#!/usr/bin/env bash
# Audit every repo in an org: does it carry the required two-layer shape?
#   node_modules/{package.json, README.md}
#   mochi_modules/{manifest.json, README.md}
# plus docs/ and mochiberry/ (asset docs) for the module packages.
# Usage: audit_org.sh [org]
set -uo pipefail
ORG="${1:-Mochi-Berry}"
export PATH="$PATH:/c/Users/Administrator/AppData/Local/Temp/ghx/bin"
OUT="$(mktemp)"
for r in $(gh repo list "$ORG" --limit 100 --json name -q '.[].name'); do
  [ "$r" = ".github" ] && continue
  br=$(gh repo view "$ORG/$r" --json defaultBranchRef -q .defaultBranchRef.name 2>/dev/null)
  [ -z "$br" ] && br=main
  tree=$(gh api "repos/$ORG/$r/git/trees/$br?recursive=1" -q '.tree[].path' 2>/dev/null)
  n=$(printf '%s\n' "$tree" | grep -c . || true)
  if [ "$n" -eq 0 ]; then
    echo "SKIP $r (tree unavailable)" >> "$OUT"; continue
  fi
  # a layer may sit directly at node_modules/<file> (JS-first repo) or under
  # node_modules/<module>/<file> (engine-style repo) — accept both shapes
  nm_pkg=$(printf '%s\n' "$tree" | grep -cE '^node_modules/(.*/)?package\.json$')
  nm_md=$(printf '%s\n' "$tree" | grep -cE '^node_modules/(.*/)?[Rr][Ee][Aa][Dd][Mm][Ee]\.md$')
  mm_man=$(printf '%s\n' "$tree" | grep -cE '^mochi_modules/(.*/)?manifest\.json$')
  mm_md=$(printf '%s\n' "$tree" | grep -cE '^mochi_modules/(.*/)?[Rr][Ee][Aa][Dd][Mm][Ee]\.md$')
  docs=$(printf '%s\n' "$tree" | grep -c '^docs/')
  brand=$(printf '%s\n' "$tree" | grep -c '^mochiberry/')
  ok=1
  [ "$nm_pkg" -ge 1 ] || ok=0
  [ "$nm_md" -ge 1 ] || ok=0
  [ "$mm_man" -ge 1 ] || ok=0
  [ "$mm_md" -ge 1 ] || ok=0
  st="BAD"; [ "$ok" = "1" ] && st="OK "
  echo "$st $r files=$n nm_pkg=$nm_pkg nm_md=$nm_md mm_man=$mm_man mm_md=$mm_md docs=$docs brand=$brand" >> "$OUT"
done
cat "$OUT"
echo "=== $(grep -c '^OK ' "$OUT") OK / $(grep -c . "$OUT") total ==="
rm -f "$OUT"
