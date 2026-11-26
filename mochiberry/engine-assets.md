# Engine assets

## Build yang dipakai

| path | peran | dipakai? |
|---|---|---|
| `E:/bcf/mochiberry/build_manual/mbjs.exe` | runtime + JIT/AOT, **punya tabel ekspor `MBJS_*`** | **ya** — semua selftest jalan di sini |
| `E:/bcf/mochiberry/build/mbjs.exe` | build generic | bisa, ekspor mungkin beda |
| `E:/bcf/mochiberry/build/tier-core/mbjs.exe` | stripped | **tidak** — tanpa export table → plugin native gagal resolve simbol engine |

Kenapa `build_manual` bukan `tier-core`: plugin native di `mochi_modules/`
resolve simbol engine lewat tabel ekspor host image (`native/mbjs_runtime.h`).
Build yang di-strip tidak mengekspor `MBJS_*`, sehingga `mbjs_init_module`
tidak menemukan simbol runtime.

## Header & source yang dibutuhkan build plugin

- `E:/bcf/mochiberry/*.h` — header engine (`mbjs.h`, `mbjs-runtime.h`, dst).
  Flag build: `gcc -shared -O2 -I E:/bcf/mochiberry …`.
- `E:/bcf/mochiberry/mbjs-libc.c` — implementasi builtin `mbjs:os` dan
  `mbjs:std` (`os.platform/getcwd/getenv/setTimeout/setInterval/clearTimeout/clearInterval`,
  `std.loadFile/exit`).

## Tabel ekspor engine

- `E:/sevm/build_ex/exports.txt` — dump 385 simbol `MBJS_*` (referensi;
  regenerate kalau engine di-rebuild).

## Platform

Host build saat ini **Windows x86_64** (`.dll`). Konvensi lintas platform:

| OS | nama biner |
|---|---|
| Windows x64 | `<modul>_x86_64.dll` |
| Linux x64 | `<modul>_x86_64.so` |
| Linux/Android ARM64 | `<modul>_aarch64.so` |
| macOS Intel | `<modul>_x86_64.dylib` |
| macOS Apple Silicon | `<modul>_arm64.dylib` |