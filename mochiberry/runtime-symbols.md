# Runtime symbols (`MBJS_*`)

Plugin native tidak meng-`import` `mbjs.exe`; simbol engine di-resolve saat
runtime lewat tabel fungsi pointer yang diisi `mbjs_runtime_resolve()` dari
`E:/sevm/native/mbjs_runtime.h` (sumber asli di repo engine).

## Cara pakai (pattern tiap plugin)

```c
#include "mbjs_runtime.h"
static MBJSRuntime rt;
...
if (mbjs_runtime_resolve(&rt)) return NULL;   // gagal resolve -> init gagal
m = rt.NewCModule(ctx, "mochinama", mod_init);
rt.AddModuleExportList(ctx, m, tab, n);
```

## Simbol yang biasa dipakai plugin SEVM

| fungsi | dipakai untuk |
|---|---|
| `NewCModule` | membuat modul C (plugin engine) |
| `AddModuleExportList` | daftar nama ekspor di `mbjs_init_module` |
| `SetModuleExportList` | isi nilai ekspor di func init |
| `NewStringLen` | buat string JS hasil plugin |
| `ToCStringLen` / `ToFloat64` | baca argumen JS |
| `ThrowTypeError` | lempar error ke JS |
| `NewUint8Array` / dll. | (kalau plugin menyentuh buffer) |

Tabel lengkap: `E:/sevm/build_ex/exports.txt` (385 simbol). Host engine harus
mengekspor simbol ini (`build_manual`), kalau tidak plugin gagal resolve.

## Builtin module

- `mbjs:os` — `os.platform/getcwd/getenv/setTimeout/setInterval/clearTimeout/clearInterval`.
- `mbjs:std` — `std.loadFile/exit` (dipedulikan oleh `mochinode` dan `mochifs`).