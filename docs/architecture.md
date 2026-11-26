# Arsitektur: dua orientasi modul

SEVM membiarkan **engine MochiBerry inti tetap tidak disentuh** dan menaruh
seluruh lapisan Node.js di dua direktori dengan **orientasi dominan** berbeda.
Batas ini bukan "boleh/tidak", tapi **di mana titik berat eksekusi**.

```
sevm/
├── node_modules/            # JS-FIRST (paket NPM, call native bila perlu)
│   ├── README.md            # overview + tabel modul
│   ├── package.json          # manifest root (semua shim)
│   └── <modul>/{index.js, package.json, README.md}
├── mochi_modules/           # NATIVE-FIRST (plugin engine, DLL/so/dylib)
│   ├── README.md
│   ├── manifest.json
│   └── <modul>/{native/*.c, include/*_abi.h, lib/*_x86_64.dll, script/, README.md, manifest.json}
├── mochiberry/              # aset & dokumentasi engine (bukan kode)
├── docs/                    # dokumentasi arsitektur (folder ini)
├── native/                  # sumber C99 (lokasi flat legacy + header bersama)
├── selftest/                # selftest yang dijalankan mbjs.exe
├── script/                  # generator (scaffold_shim.py, gen_manifests.py)
├── cmake/                   # toolchain cross-compile
├── AGENTS.md                # kontrak aturan untuk agent
└── MISSING_SHIMS.json       # antrean gap-driven
```

## Kenapa dipisah

| | `node_modules/` | `mochi_modules/` |
|---|---|---|
| titik berat | JS/TS | C99 / ABI native |
| cara pakai | familiar NPM (`package.json`, `main`) | dimuat engine sebagai plugin |
| waktu pilih | framework, aplikasi, parser | shim inti sistem (`fs`,`crypto`,`zlib`,`net`,`ws`), protokol berat |
| footprint | lebih besar (objek JS di heap) | kecil (muscle di C99) |
| `mochibake` slice | bisa, tapi JS tetap di-parse | tabel ekspor PE/ELF/Mach-O bisa dipangkas |

## Aturan pagar (dari AGENTS.md)

- Tidak ada fitur Node.js di engine inti `libmochiberry`.
- `mochi_modules` = native-first, JS cuma binding tipis.
- `node_modules` = JS-first hybrid, boleh `loadNative()`/`dynalib()` untuk berat.
- Setiap `malloc`/`calloc` di sisi C99 wajib punya `free` atau terikat GC/arena.
- Plugin tidak boleh import `mbjs.exe` (lihat `../mochiberry/module-loader-contract.md`).