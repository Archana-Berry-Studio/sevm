# docs — SEVM shim-layer architecture documentation

- [`architecture.md`](architecture.md) — the two-orientation model
  (`node_modules` JS-first vs `mochi_modules` native-first), repo tree, rules.
- [`shim-coverage.md`](shim-coverage.md) — honest matrix: what is verified,
  operational, stub, broken (all measured on `mbjs.exe`, not claims).
- [`module-resolution.md`](module-resolution.md) — how `require()`/`loadNative()`
  resolve names and the `BUILTIN_ALIASES`.
- [`../mochiberry`](../mochiberry) — engine assets & contracts (outside this folder).

Agent rules: [`../AGENTS.md`](../AGENTS.md). Gap list: [`../MISSING_SHIMS.json`](../MISSING_SHIMS.json).
