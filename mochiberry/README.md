# mochiberry — engine assets & documentation

This folder is **not code**. It holds notes and contracts about the
**MochiBerry engine** that lives outside this SEVM repo. The core engine repo
is at `E:/bcf/mochiberry` — here we document *which parts* the shim layer needs
and the *contract* that native plugins must honour.

- [`engine-assets.md`](engine-assets.md) — the engine build in use, the export
  table, on-disk artifacts, and why `build_manual` is used instead of `tier-core`.
- [`module-loader-contract.md`](module-loader-contract.md) — native plugin
  loading rules (`mbjs_init_module`, specifier, the `mbjs.exe` import ban).
- [`runtime-symbols.md`](runtime-symbols.md) — the `MBJS_*` symbols plugins
  resolve through `native/mbjs_runtime.h`.

Shim-layer architecture documentation lives in [`../docs`](../docs).
