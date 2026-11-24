# SEVM AOT toolchain — "compile everything" pipeline

The stock AOT pipeline (`node_modules/mochijsnative/build.js`) only compiles
the **entry module** to bytecode.  Every other module is embedded as plain
JS source and extracted to a temp directory at startup, where the engine
**parses it at runtime** (`#define BUNDLE_EXTRACT_ALL 1`).  That is why the
shipped executable is fat (~1.86 MB) and why it still needs the JavaScript
parser inside the engine.

This directory holds the replacement toolchain.  It compiles **every**
reachable module to bytecode, embeds **no JS source**, and links an engine
profile built with the parser **removed**.

## Pipeline

```
build-sevm-aot.sh  -> libmbjs-sevm-aot.a   parser-free engine profile
build-mbjsc.sh     -> mbjsc_v29/mbjsc.exe  bytecode compiler, rebuilt to v29
build-lean.js      -> build_ex/lean/lean_bundle.h + lean_host.c
link-aot.sh        -> build_ex/<name>.exe  (links host + engine archive)
```

`build-lean.js` is run with the engine itself:

```
mbjs.exe -m tools/aot/build-lean.js
```

It collects the module graph from `examples/sevm-app/src/server.js`
(mirroring `node_modules/mochinode/index.js` `BUILTIN_ALIASES`), compiles
every module with `mbjsc -e -m -n <path-relative-to-root>`, and emits a C
host that registers all modules (dependency order, `load_only=1`) before
evaluating the entry (`load_only=0`).  The host also installs
`__MOCHI_MODULE_NAME__` / `__MOCHI_MODULE_EXISTS__` so bare `require()`
specifiers resolve against the module registry instead of the file system.

## Feature set

The engine profile is built with `-DMBJS_DISABLE_PARSER` and the
`MBJS_NO_*` guards for the features the SEVM app does not use (Date, Proxy,
Map/Set, WeakRef, AToB, Performance, DOMException, Atomics).  **RegExp and
TypedArrays are kept** — `mochinode/index.js` uses regex literals and
`mochinode/lib/process.js` uses `Uint8Array`/`ArrayBuffer`.  Dropping either
breaks the app at compile time.

## Link flags that matter

* **no** `-Wl,--whole-archive` — only the referenced engine objects are kept
* `-Wl,--gc-sections` — drops the dead features
* `-Wl,--export-all-symbols` — the AOT executable doubles as a shared
  library for the native plugins, which resolve `MBJS_*` through
  `GetModuleHandle(NULL)`
* `-Wl,--defsym=stat64i32=_stat64i32` — shim for the prebuilt `mbjs-libc.o`
* `-fno-asynchronous-unwind-tables` / `-fno-jump-tables` — mandatory on
  PE/COFF so `--gc-sections` can actually collect

## Result (measured)

| metric                       | stock bundle   | lean AOT       |
|------------------------------|----------------|----------------|
| modules compiled to bytecode | 1 (entry only) | 8 / 8          |
| embedded JS source           | 210 KB         | 0              |
| embedded DLL / assets        | 204 + 178 KB   | 0 (from disk)  |
| parser in engine             | yes            | no             |
| stripped size                | 1,861,134 B    | ~828,430 B     |

Verified end-to-end on port 8099: `/api/health`, `/api/dynalib/float?v=N`
(native `msvcrt!sqrt`), `/api/items` (SQLite) all return 200.
