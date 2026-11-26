# mochiberry — aset & dokumentasi engine

Folder ini **bukan kode**. Isinya catatan aset dan kontrak yang berkaitan
dengan **engine MochiBerry** di luar repo SEVM ini. Repo engine inti ada di
`E:/bcf/mochiberry` — di sini kita dokumentasikan *bagian mana* yang dibutuhkan
oleh shim layer dan *kontrak* yang dipegang plugin native.

- [`engine-assets.md`](engine-assets.md) — build engine yang dipakai, tabel
  ekspor, artefak di disk, dan kenapa `build_manual` bukan `tier-core`.
- [`module-loader-contract.md`](module-loader-contract.md) — aturan pemuatan
  plugin native (`mbjs_init_module`, specifier, larangan import `mbjs.exe`).
- [`runtime-symbols.md`](runtime-symbols.md) — simbol `MBJS_*` yang di-resolve
  plugin lewat `native/mbjs_runtime.h`.

Dokumentasi arsitektur shim ada di [`../docs`](../docs).