# Architecture: the two-orientation module model

SEVM leaves the **MochiBerry core engine untouched** and puts the whole Node.js
layer in two directories with different **dominant orientations**. The boundary
is not "allowed/not allowed" but **where the execution weight sits**.

```
sevm/
├── node_modules/            # JS-FIRST (NPM packages, call native when needed)
│   ├── README.md            # overview + module table
│   ├── package.json          # root manifest (all shims)
│   └── <module>/{index.js, package.json, README.md}
├── mochi_modules/           # NATIVE-FIRST (engine plugins, DLL/so/dylib)
│   ├── README.md
│   ├── manifest.json
│   └── <module>/{native/*.c, include/*_abi.h, lib/*_x86_64.dll, script/, README.md, manifest.json}
├── mochiberry/              # engine assets & documentation (not code)
├── docs/                    # architecture docs (this folder)
├── native/                  # C99 sources (flat legacy location + shared headers)
├── selftest/                # selftests run by mbjs.exe
├── script/                  # generators (scaffold_shim.py, gen_manifests.py)
├── cmake/                   # cross-compile toolchains
├── AGENTS.md                # agent rules contract
└── MISSING_SHIMS.json       # gap-driven queue
```

## Why the split

| | `node_modules/` | `mochi_modules/` |
|---|---|---|
| weight | JS/TS | C99 / native ABI |
| usage | familiar NPM (`package.json`, `main`) | loaded by the engine as a plugin |
| pick when | framework, app, parser | core system shims (`fs`,`crypto`,`zlib`,`net`,`ws`), heavy protocols |
| footprint | larger (JS objects on the heap) | small (muscle in C99) |
| `mochibake` slice | possible, but JS is still parsed | PE/ELF/Mach-O export tables can be trimmed |

## Fence rules (from AGENTS.md)

- No Node.js features in the `libmochiberry` core engine.
- `mochi_modules` = native-first, JS is only a thin binding.
- `node_modules` = JS-first hybrid, may use `loadNative()`/`dynalib()` for heavy parts.
- Every `malloc`/`calloc` on the C99 side must have a `free` or be bound to a GC/arena.
- A plugin must not import `mbjs.exe` (see `../mochiberry/module-loader-contract.md`).
