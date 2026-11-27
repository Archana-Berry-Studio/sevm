# mochi_modules — native engine plugins (native-first)

This directory holds **native plugins for the MochiBerry engine**: shared
objects / DLLs loaded into the engine at runtime through the native module
loader. Orientation: **native**. JS here is only a thin binding — heavy logic
belongs in C99.

Sibling layer: [`node_modules/`](../node_modules) = NPM-style JS/framework packages.

| | `node_modules/` | `mochi_modules/` |
|---|---|---|
| orientation | **JS / script / framework** | **native engine plugin** |
| main content | JS packages (`index.js` + `package.json`) | DLL / `.so` / `.dylib` |
| native lib | allowed, in `bin/` or `lib/` | this IS the point |
| examples | `express`, `net`, `sqlite`, `mochicrypto` | `mochicrypto/`, `mochifs/`, `mochiws/` |

## Available plugins (status measured on mbjs.exe)

| module | runtime exports | status |
|---|---|---|
| `mochicrypto` | 16 (hash/hmac/pbkdf2/aes) | verified |
| `mochifs` | 11 (fs primitives) | verified |
| `mochizlib` | 7 (gzip/gunzip/raw deflate/inflate/crc32, verified vs Python oracle) | verified |
| `mochinode_net` | 5 (httpServer/Accept/Read/Write/Close) | operational |
| `mochinode_sqlite` | 15 — **no bind export yet** | operational |
| `mochinode_dynalib` | 13 (libdynalib bridge) | operational |
| `mochiws` | 5 (connect/poll/send/close/httpGet) | operational |
| `mochiwasmy` | 1 (`ping`) — wasm nearly a stub | stub |
| `mochijsnative` | 51 (eval/jit*/runtime*/value*/context*) | verified |
| `mochisandbox` | 7 (realm-isolated sandbox) + SECURITY.md | verified |

Export details & ABI: `mochi_modules/manifest.json` +
`mochi_modules/<module>/manifest.json`.

## Layout (per module)

```
mochi_modules/
├── README.md                 # this file
├── manifest.json             # aggregate registry (computed from runtime)
├── <module>/
│   ├── README.md
│   ├── manifest.json         # per-module: runtime exports, sources, binaries
│   ├── native/<module>.c     # C99 source (engine plugin)
│   ├── include/<module>_abi.h# ABI header
│   ├── script/               # test / generator bindings
│   └── lib/<module>_x86_64.dll | .so | .dylib   # distribution binary, arch-suffixed
```

- **`lib/<module>_<arch>.{dll,so,dylib}`** is the distribution copy with the
  arch-naming convention from AGENTS.md. The loader search order prefers this
  location (see `mochinode.resolveNativePath`).
- Per-module dirs are populated by `python3 script/populate_modules.py`; binary
  copies are refreshed by `python3 script/gen_manifests.py --force`.

## Load contract (verified from mbjs.exe disassembly + engine source)

- The DLL's exported entry point **must** be `mbjs_init_module`;
- The import specifier **must** end in `.dll` / `.so` / `.lbrct`;
- `AddModuleExportList` is called inside `mbjs_init_module` (export names),
  `SetModuleExportList` in the module init func (export values);
- A plugin **must not** import `mbjs.exe` (Windows error 126) — engine symbols
  are resolved at runtime via `native/mbjs_runtime.h`;
- C functions that throw **must** return `MBJS_EXCEPTION` (not `MBJS_UNDEFINED`),
  otherwise the exception stays pending and crashes at teardown.

## Rebuild (needs gcc + engine headers in `E:/bcf/mochiberry`)

```bash
ENGINE_INC=E:/bcf/mochiberry   # engine headers
gcc -shared -O2 -o mochi_modules/mochizlib/lib/mochizlib_x86_64.dll        native/mochizlib.c        -I$ENGINE_INC
gcc -shared -O2 -o mochi_modules/mochicrypto/lib/mochicrypto_x86_64.dll    native/mochicrypto.c      -I$ENGINE_INC
gcc -shared -O2 -o mochi_modules/mochifs/lib/mochifs_x86_64.dll            native/mochifs.c          -I$ENGINE_INC
gcc -shared -O2 -o mochi_modules/mochiws/lib/mochiws_x86_64.dll            native/mochiws.c          -I$ENGINE_INC
gcc -shared -O2 -o mochi_modules/mochinode_net/lib/mochinode_net_x86_64.dll native/mochinode_net.c    -I$ENGINE_INC -lws2_32
gcc -shared -O2 -o mochi_modules/mochinode_sqlite/lib/mochinode_sqlite_x86_64.dll native/mochinode_sqlite.c -I$ENGINE_INC
```

Cross-compile across OS/arch uses the toolchains in `cmake/` (see AGENTS.md §5).
After a build, run `python3 script/gen_manifests.py --force` to refresh `lib/`
and `manifest.json`.

> Note: the engine must export the `MBJS_*` symbols (the `build_manual` build
> does); the stripped `tier-core` build has no export table, so native modules
> cannot resolve engine symbols. See `MISSING_SHIMS.json` for honest gaps.
