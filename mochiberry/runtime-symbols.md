# Runtime symbols (`MBJS_*`)

Native plugins do not `import` `mbjs.exe`; engine symbols are resolved at
runtime through the function-pointer table filled by `mbjs_runtime_resolve()`
from `E:/sevm/native/mbjs_runtime.h` (original source in the engine repo).

## Usage (per-plugin pattern)

```c
#include "mbjs_runtime.h"
static MBJSRuntime rt;
...
if (mbjs_runtime_resolve(&rt)) return NULL;   // resolve failed -> init fails
m = rt.NewCModule(ctx, "module-name", mod_init);
rt.AddModuleExportList(ctx, m, tab, n);
```

## Symbols commonly used by SEVM plugins

| function | used for |
|---|---|
| `NewCModule` | create a C module (engine plugin) |
| `AddModuleExportList` | register export names in `mbjs_init_module` |
| `SetModuleExportList` | fill export values in the init func |
| `NewStringLen` | build a JS string result |
| `ToCStringLen` / `ToFloat64` | read JS arguments |
| `ThrowTypeError` | throw an error into JS |
| `NewUint8Array` / etc. | (when a plugin touches buffers) |

Full table: `E:/sevm/build_ex/exports.txt` (385 symbols). The host engine must
export these (`build_manual`), otherwise plugins fail to resolve.

## Builtin modules

- `mbjs:os` — `os.platform/getcwd/getenv/setTimeout/setInterval/clearTimeout/clearInterval`.
- `mbjs:std` — `std.loadFile/exit` (used by `mochinode` and `mochifs`).
