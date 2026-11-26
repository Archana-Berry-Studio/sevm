# Kontrak loader modul native

Bagaimana engine memuat plugin dari `mochi_modules/` dan aturan yang WAJIB
dipegang plugin. Ini hasil reverse disassembly `mbjs.exe` + source engine —
bukan tebakan.

## Alur pemuatan

1. JS memanggil `mochinode.loadNative('<modul>.dll')`.
2. `loadNative` mencari file di:
   1. `node_modules/<pkg>/bin/<name><ext>` (biner bundel paket), lalu
   2. `mochi_modules/<name><ext>` (registry plugin engine).
3. `loadNative` melakukan `import('<path abs .dll>')`; loader native engine
   menangani specifier berakhiran `.dll`/`.so`/`.lbrct`.
4. Engine memanggil titik masuk **`mbjs_init_module(ctx, name)`**.

## Aturan wajib

- Titik masuk ekspor DLL **wajib** `mbjs_init_module`.
- Specifier import **wajib** berakhiran `.dll` (Windows) / `.so` (Linux) /
  `.lbrct` (platform lain).
- Di dalam `mbjs_init_module`, panggil `AddModuleExportList(ctx, m, tab, n)`
  untuk **mendaftarkan nama** ekspor.
- Di func init modul, panggil `SetModuleExportList(ctx, m, tab, n)` untuk
  mengisi **nilai** ekspor (lihat pattern `zlib_mod_init` + `mbjs_init_module`
  di `native/mochizlib.c`).
- Plugin **tidak boleh** import `mbjs.exe`. Kalau masalah pemuatan plugin adalah
  Windows error `126`, hampir pasti ia meng-import simbol engine sebagai import
  statis. Solusi: resolve saat runtime lewat `native/mbjs_runtime.h`
  (`mbjs_runtime_resolve(&rt)` mengisi tabel fungsi pointer).

## Registri ekspor runtime (terukur)

Ekspor JS-visible tiap plugin dihitung saat runtime dan dicatat di
`mochi_modules/manifest.json` dan `mochi_modules/<modul>/manifest.json`.
Cara mengukur ulang:

```js
const { loadNative } = await import('./node_modules/mochinode/index.js');
const ns = await loadNative('mochizlib.dll');
print(Object.keys(ns).sort().join(', '));
```