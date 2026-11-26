# MochiBerry Agent Engineering Rules

Agent wajib membaca file ini sebelum menambah atau mengubah shim di repo SEVM.

## 1. Invarian arsitektur

- Dilarang menambah fitur Node.js ke `libmochiberry`/engine inti.
- Semua shim masuk ke `node_modules/` atau `mochi_modules/`.
- `mochi_modules/` = native-first. JS di sana hanya binding tipis.
- `node_modules/` = JS-first hybrid. Boleh memanggil native lewat `dynalib()`/`loadNative()`.
- DLL flat yang sudah ada di `mochi_modules/*.dll` adalah legacy kompatibilitas. Jangan memindahkan atau menghapusnya tanpa update `mochinode.resolveNativePath()`.
- Modul baru wajib memakai layout berstruktur di bawah. Modul lama boleh tetap flat sampai dimigrasikan.
- Nama biner: `<modul>_x86_64.dll`, `<modul>_arm64.dylib`, `<modul>_aarch64.so`, `<modul>_x86_64.so`.

## 2. Layout wajib modul baru

### `mochi_modules/<nama>/` (native-first)

```text
mochi_modules/<nama>/
├── README.md
├── CMakeLists.txt
├── include/
│   └── <nama>_abi.h
├── native/
│   └── <nama>.c
├── script/
│   ├── index.js
│   └── test.js
└── lib/
    ├── <nama>_x86_64.dll
    ├── <nama>_x86_64.so
    ├── <nama>_aarch64.so
    └── <nama>_arm64.dylib
```

### `node_modules/<nama>/` (JS-first hybrid)

```text
node_modules/<nama>/
├── package.json
├── README.md
├── CMakeLists.txt
├── include/
│   └── <nama>_abi.h
├── src/
│   ├── <nama>.c
│   └── helper.js
├── script/
│   ├── index.js
│   └── test.js
└── lib/
    ├── <nama>_x86_64.dll
    ├── <nama>_x86_64.so
    ├── <nama>_aarch64.so
    └── <nama>_arm64.dylib
```

`package.json` hybrid wajib memakai `"main": "script/index.js"`. `node_modules` murni JS boleh tanpa `CMakeLists.txt`, `include/`, `src/`, dan `lib/`.

## 3. Aturan implementasi

- C99/NDK/SDK native ditempatkan di `native/` (mochi) atau `src/` (hybrid). Hindari C++ bila tidak perlu.
- Header publik di `include/` memakai guard `#ifndef` dan `extern "C"` bila bisa dipakai C++.
- Ekspor native stabil: `mbjs_init_module` untuk engine plugin; `*_abi_*` untuk dynalib.
- Binding JS memakai path relatif: `mochinode.resolveNativePath('nama.dll', cwd())` atau `../lib/<nama>_<arch>.<ext>`.
- Jangan membuat wrapper objek besar di atas simbol dynalib. Bind langsung ke fungsi/ekspor.
- Setiap `malloc`/`calloc` harus punya `free` atau terikat ke GC/arena yang jelas.
- Jangan mengimpor `mbjs.exe` dari DLL plugin. Simbol engine di-resolve lewat `native/mbjs_runtime.h`.
- Patch di `node_modules` harus punya sumber tetap di repo ini agar tidak hilang saat `npm install`.

## 4. Alur gap-driven

- Baca `MISSING_SHIMS.json`.
- Pilih satu modul dengan prioritas tertinggi.
- Jalankan `python script/scaffold_shim.py <nama> mochi` atau `... node`.
- Implementasi native dahulu untuk `mochi_modules`; JS dahulu untuk `node_modules`.
- Tambah `script/test.js`, lalu jalankan via `E:/bcf/mochiberry/build_manual/mbjs.exe -m script/test.js`.
- Jika lulus, tandai entri `MISSING_SHIMS.json` sebagai `done` dan lanjut ke entri berikutnya.

## 5. Cross-compile

Toolchain tersedia di `cmake/`:

```bash
cmake -S mochi_modules/<nama> -B build/<nama>-win64 -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-windows-mingw.cmake -DMOCHI_ARCH=x86_64
cmake -S mochi_modules/<nama> -B build/<nama>-linux -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-linux.cmake -DMOCHI_ARCH=x86_64
cmake -S mochi_modules/<nama> -B build/<nama>-android -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-android-ndk.cmake -DMOCHI_ARCH=aarch64
cmake --build build/<nama>-win64
```

## 6. Perintah verify

```bash
node --check node_modules/<nama>/script/index.js
E:/bcf/mochiberry/build_manual/mbjs.exe -m selftest/mochi-core.js
E:/bcf/mochiberry/build_manual/mbjs.exe -m selftest/mochi-runtime.js
E:/bcf/mochiberry/build_manual/mbjs.exe -m selftest/mochi-shims.js
```

## 7. Kapan memakai mana

- `mochi_modules`: shim inti sistem (`fs`, `crypto`, `net`, `zlib`, `ws`), driver NDK, protokol berat, atau modul yang perlu RAM minimum.
- `node_modules` hybrid: paket aplikasi/framework yang nyaman di JS/TS tapi punya bagian berat C99.
- `node_modules` murni JS: utilitas ringan yang tidak menyentuh OS dan bukan bottleneck.
