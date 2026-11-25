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

## MochiBake (the DLL baking organ)

MochiBake is an **internal organ of MochiBerry**, not a separate tool and not
another module directory.  It is compiled into the engine archive
(`tools/aot/mbjs-bake.c` -> `mbjs-bake.o`) and exposed to JavaScript as the
built-in module `mbjs:bake`:

    bake.peInfo(path)        -> {machine,imageSize,exports[],imports[]}
    bake.read(path)          -> Uint8Array (the DLL bytes)
    bake.write(name,bytes)   -> materialise an embedded DLL
    bake.tempDir()           -> the bake scratch directory

What it does for a bundle:

1. **It reads the programmer's intent from the JavaScript.**  Every
   `loadNative()/require()/resolveNative()` of a `.dll`, every
   `dynalib(file, reg, symbol)` call and every property the program calls on
   an imported native module is a declaration of "this program needs this".
2. **Only those DLLs are baked.**  A DLL inside the project is embedded in
   the binary as raw bytes; a DLL nobody calls is left out entirely.
3. **System DLLs are not copied.**  `C:/Windows/System32/msvcrt.dll` and
   friends stay with the OS loader - the essential base components remain
   pulled, they are simply not duplicated.
4. **At startup the baked plugins are materialised** (next to the app in
   `mochi_modules/`, or in the temp bake directory when that folder is not
   writable) and the loader hands those copies to the engine.  An existing
   file on disk is never overwritten.
5. **The manifest is visible, not silent** - debug level 1 prints every
   baked plugin with its size and the symbols the program calls:

        [lean] [00:00.000] bake    mochifs.dll -> .../mochi_modules/mochifs.dll
               (66294 B, symbols: exists,readText,writeText,appendText,
                stat,readdir,mkdir,rename,remove,copyFile,cwd)

The result is a single-file application: the JavaScript is bytecode, the
native plugins are inside the executable, and the app runs with no external
plugin directory at all.

Sizes measured on this tree (production entry, x86-64):

    without baked plugins   808.974 B   (needs mochi_modules/ on disk)
    with baked plugins    1.164.302 B   (runs with mochi_modules/ removed)

## Per-function slicing (recompile, not just re-embed)

A plugin whose C source we own is not embedded as-is: it is **recompiled
with only the functions the program calls**.

1. The compiler already knows the JS symbols the program uses
   (`native.readText(`, `native.callSymbol.apply(`, `native.httpWrite`
   handed over as a callback - any property access on the native namespace
   counts).
2. A **derived copy** of the plugin source is written to
   `build_ex/lean/sliced/<name>_sliced.c` (the original file in `native/` is
   never touched).  Its `MBJS_CFUNC_DEF(...)` table is trimmed to the used
   names and the hard-coded table counts (`tab, 11`, `DL_TAB_LEN`) are fixed.
3. The copy is compiled with `-Os -ffunction-sections -fdata-sections
   -Wl,--gc-sections -Wl,-s`, so every static function that nothing
   references any more is collected by the linker.
4. The *sliced* DLL is what gets embedded in the binary.

Measured on this tree (production entry, x86-64):

    mochinode_sqlite.dll    61.289 -> 22.016 B  (35.9 %)
    mochifs.dll              66.294 -> 23.040 B  (34.8 %)
    mochinode_dynalib.dll    82.195 -> 45.056 B  (54.8 %)
    mochinode_net.dll        60.597 -> 19.456 B  (32.1 %)
    baked natives          270.375 -> 109.568 B  (-59.5 %)

    exe without plugins       808.974 B
    exe with whole DLLs     1.166.350 B
    exe with sliced DLLs    1.006.094 B

Verified: the binary runs with `mochi_modules/` removed, the materialised
copies have exactly the sliced sizes, and `/api/health`,
`/api/dynalib/float?v=49` and `/api/items` all answer.

A plugin without a recipe in `NATIVE_RECIPES` (or without a symbol
manifest) is baked whole - the build log says so, it never fails silently.
