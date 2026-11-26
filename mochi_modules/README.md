# mochi_modules — plugin native engine (native-first)

Direktori ini khusus berisi **plugin native untuk engine MochiBerry**: shared
object / DLL yang di-load ke engine saat runtime lewat loader modul native.
Orientasi: **native**. JS di sini hanya binding tipis — logika berat wajib di C99.

Layer sebelah: [`node_modules/`](../node_modules) = paket JS/framework style NPM.

| | `node_modules/` | `mochi_modules/` |
|---|---|---|
| orientasi | **JS / script / framework** | **native plugin engine** |
| isi utama | paket JS (`index.js` + `package.json`) | DLL / `.so` / `.dylib` |
| native lib | boleh, di `bin/` atau `lib/` | justru intinya |
| contoh | `express`, `net`, `sqlite`, `mochicrypto` | `mochicrypto/`, `mochifs/`, `mochiws/` |

## Plugin yang tersedia (status terukur di mbjs.exe)

| modul | ekspor runtime | status |
|---|---|---|
| `mochicrypto` | 16 (hash/hmac/pbkdf2/aes) | verified |
| `mochifs` | 11 (fs primitives) | verified |
| `mochizlib` | 2 (deflateHex/inflateHex) — **gzip/raw belum ada** | operational |
| `mochinode_net` | 5 (httpServer/Accept/Read/Write/Close) | operational |
| `mochinode_sqlite` | 15 — **tidak ada bind** | operational |
| `mochinode_dynalib` | 13 (libdynalib bridge) | operational |
| `mochiws` | 5 (connect/poll/send/close/httpGet) | operational |
| `mochiwasmy` | 1 (`ping`) — wasm nyaris stub | stub |
| `mochijsnative` | — **gagal init** | broken |

Detail ekspor & ABI: `mochi_modules/manifest.json` + `mochi_modules/<modul>/manifest.json`.

## Layout (per modul)

```
mochi_modules/
├── README.md                 # file ini
├── manifest.json             # registri agregat (dihitung dari runtime)
├── <modul>/
│   ├── README.md
│   ├── manifest.json         # per-modul: ekspor runtime, sumber, binari
│   ├── native/<modul>.c      # sumber C99 (engine plugin)
│   ├── include/<modul>_abi.h # header ABI
│   ├── script/               # binding test / generator
│   └── lib/<modul>_x86_64.dll| .so | .dylib   # binari distribusi arch-suffixed
└── <modul>.dll               # (legacy/flat, tetap untuk kompatibilitas loader)
```

- **Flat `*.dll`** di root `mochi_modules/` adalah artefak yang benar-benar
  di-load engine — jangan dihapus. `mochinode.loadNative()` memindainya.
- **`lib/<modul>_<arch>.{dll,so,dylib}`** adalah salinan distribusi dengan
  konvensi nama arsitektur dari AGENTS.md.

## Kontrak pemuatan (terverifikasi dari disassembly mbjs.exe + source engine)

- ekspor DLL **wajib** bernama `mbjs_init_module`;
- specifier import **wajib** berakhiran `.dll` / `.so` / `.lbrct`;
- `AddModuleExportList` dipanggil di dalam `mbjs_init_module` (nama ekspor),
  `SetModuleExportList` dipanggil di init func modul (nilai ekspor);
- plugin **tidak boleh** import `mbjs.exe` (Windows error 126) — simbol engine
  di-resolve saat runtime lewat `native/mbjs_runtime.h`.

## Build ulang (butuh gcc + header engine di `E:/bcf/mochiberry`)

```bash
ENGINE_INC=E:/bcf/mochiberry   # header engine
gcc -shared -O2 -o mochi_modules/mochizlib.dll        native/mochizlib.c        -I$ENGINE_INC
gcc -shared -O2 -o mochi_modules/mochicrypto.dll      native/mochicrypto.c      -I$ENGINE_INC
gcc -shared -O2 -o mochi_modules/mochifs.dll          native/mochifs.c          -I$ENGINE_INC
gcc -shared -O2 -o mochi_modules/mochiws.dll          native/mochiws.c          -I$ENGINE_INC
gcc -shared -O2 -o mochi_modules/mochinode_net.dll    native/mochinode_net.c    -I$ENGINE_INC -lws2_32
gcc -shared -O2 -o mochi_modules/mochinode_sqlite.dll native/mochinode_sqlite.c -I$ENGINE_INC
```

Cross-compile lintas OS/arch pakai toolchain di `cmake/` (lihat AGENTS.md §5).
Setelah build, jalankan `python3 script/gen_manifests.py --force` untuk
merefresh `lib/` dan `manifest.json`.

> Catatan: engine harus mengekspor simbol `MBJS_*` (build `build_manual` ya);
> build `tier-core` yang sudah di-strip tidak punya export table sehingga modul
> native tidak bisa resolve simbol engine. `mochijsnative.dll` gagal init pada
> engine build sekarang — lihat `MISSING_SHIMS.json`.
