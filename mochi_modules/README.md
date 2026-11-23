# mochi_modules — native engine plugins

Berbeda dengan `node_modules/` (paket JS/framework), direktori ini khusus
berisi **plugin native untuk engine MochiBerry**: shared object / DLL yang
di-load ke dalam engine saat runtime lewat loader modul native.

| file | fungsi |
|------|--------|
| `mochinode_net.dll` | server TCP/HTTP blocking (Winsock2) — dibungkus `node_modules/net` |
| `mochinode_sqlite.dll` | bridge SQLite (libsqlite3 / sqlite3.dll) — dibungkus `node_modules/sqlite` |
| `mochinode_dynalib.dll` | bridge dynamic loader C99 (`libdynalib`) ke JS — dibungkus `node_modules/dynalib` |

Kontrak pemuatan (terverifikasi dari disassembly `mbjs.exe` + source engine):

- ekspor DLL **wajib** bernama `mbjs_init_module`;
- specifier import **wajib** berakhiran `.dll` (atau `.so` / `.lbrct` di
  platform lain);
- `AddModuleExportList` dipanggil di dalam `mbjs_init_module` (nama ekspor),
  `SetModuleExportList` dipanggil di init func modul (nilai ekspor);
- modul **tidak boleh** meng-import `mbjs.exe` (Windows error 126) — simbol
  engine di-resolve saat runtime dari host image lewat `native/mbjs_runtime.h`.

Build ulang (butuh gcc + header engine di `E:/bcf/mochiberry`):

```bash
cd native
gcc -shared -O2 -o ../mochi_modules/mochinode_net.dll mochinode_net.c \
    -IE:/bcf/mochiberry -lws2_32
gcc -shared -O2 -o ../mochi_modules/mochinode_sqlite.dll mochinode_sqlite.c \
    -IE:/bcf/mochiberry
```

> Catatan: engine harus mengekspor simbol `MBJS_*` (build `build_manual` ya);
> build `tier-core` yang sudah di-strip tidak memiliki export table sehingga
> modul native tidak bisa resolve simbol engine.
