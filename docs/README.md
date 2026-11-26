# docs — dokumentasi arsitektur shim SEVM

- [`architecture.md`](architecture.md) — model dua-orientasi
  (`node_modules` JS-first vs `mochi_modules` native-first), pohon repo, aturan.
- [`shim-coverage.md`](shim-coverage.md) — matriks jujur: apa yang verified,
  operational, stub, broken (semua diukur di `mbjs.exe`, bukan klaim).
- [`module-resolution.md`](module-resolution.md) — cara `require()`/`loadNative()`
  resolve nama dan `BUILTIN_ALIASES`.
- [`../mochiberry`](../mochiberry) — aset & kontrak engine (di luar folder ini).

Aturan agent: [`../AGENTS.md`](../AGENTS.md). Daftar gap: [`../MISSING_SHIMS.json`](../MISSING_SHIMS.json).