# MochiBerry Agent Engineering Rules

An agent MUST read this file before adding or modifying any shim in the SEVM repo.

## 1. Architecture invariants

- Adding Node.js features to `libmochiberry`/the core engine is forbidden.
- All shims go into `node_modules/` or `mochi_modules/`.
- `mochi_modules/` = native-first. JS there is only a thin binding.
- `node_modules/` = JS-first hybrid. It may call native through `dynalib()`/`loadNative()`.
- Flat DLLs already present in `mochi_modules/*.dll` are a legacy-compat artifact. Do
  not move or delete them without updating `mochinode.resolveNativePath()`.
- A new module must use the structured layout below. Old modules may stay flat
  until migrated.
- Binary naming: `<module>_x86_64.dll`, `<module>_arm64.dylib`, `<module>_aarch64.so`,
  `<module>_x86_64.so`.

## 2. Mandatory layout for a new module

### `mochi_modules/<name>/` (native-first)

```text
mochi_modules/<name>/
├── README.md
├── CMakeLists.txt
├── include/
│   └── <name>_abi.h
├── native/
│   └── <name>.c
├── script/
│   ├── index.js
│   └── test.js
└── lib/
    ├── <name>_x86_64.dll
    ├── <name>_x86_64.so
    ├── <name>_aarch64.so
    └── <name>_arm64.dylib
```

### `node_modules/<name>/` (JS-first hybrid)

```text
node_modules/<name>/
├── package.json
├── README.md
├── CMakeLists.txt
├── include/
│   └── <name>_abi.h
├── src/
│   ├── <name>.c
│   └── helper.js
├── script/
│   ├── index.js
│   └── test.js
└── lib/
    ├── <name>_x86_64.dll
    ├── <name>_x86_64.so
    ├── <name>_aarch64.so
    └── <name>_arm64.dylib
```

A hybrid `package.json` must use `"main": "script/index.js"`. A pure-JS
`node_modules` package may omit `CMakeLists.txt`, `include/`, `src/` and `lib/`.

## 3. Implementation rules

- C99/NDK/SDK native code goes in `native/` (mochi) or `src/` (hybrid). Avoid C++
  unless needed.
- Public headers in `include/` use `#ifndef` guards and `extern "C"` when usable
  from C++.
- Stable native exports: `mbjs_init_module` for engine plugins; `*_abi_*` for dynalib.
- JS bindings use relative paths: `mochinode.resolveNativePath('name.dll', cwd())`
  or `../lib/<name>_<arch>.<ext>`.
- Do not build large wrapper objects over dynalib symbols. Bind directly to
  functions/exports.
- Every `malloc`/`calloc` must have a `free` or be bound to a clear GC/arena.
- Do not import `mbjs.exe` from a plugin DLL. Engine symbols are resolved via
  `native/mbjs_runtime.h`.
- Patches in `node_modules` must have a permanent source in this repo so they do
  not vanish on `npm install`.

## 4. Gap-driven flow

- Read `MISSING_SHIMS.json`.
- Pick the highest-priority module.
- Run `python script/scaffold_shim.py <name> mochi` or `... node`.
- Implement native first for `mochi_modules`; JS first for `node_modules`.
- Add `script/test.js`, then run via
  `E:/bcf/mochiberry/build_manual/mbjs.exe -m script/test.js`.
- If it passes, mark the `MISSING_SHIMS.json` entry `done` and move to the next.

## 5. Cross-compile

Toolchains are available in `cmake/`:

```bash
cmake -S mochi_modules/<name> -B build/<name>-win64 -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-windows-mingw.cmake -DMOCHI_ARCH=x86_64
cmake -S mochi_modules/<name> -B build/<name>-linux -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-linux.cmake -DMOCHI_ARCH=x86_64
cmake -S mochi_modules/<name> -B build/<name>-android -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-android-ndk.cmake -DMOCHI_ARCH=aarch64
cmake --build build/<name>-win64
```

## 6. Verify commands

```bash
node --check node_modules/<name>/script/index.js
E:/bcf/mochiberry/build_manual/mbjs.exe -m selftest/mochi-core.js
E:/bcf/mochiberry/build_manual/mbjs.exe -m selftest/mochi-runtime.js
E:/bcf/mochiberry/build_manual/mbjs.exe -m selftest/mochi-shims.js
```

## 7. When to use which

- `mochi_modules`: core system shims (`fs`, `crypto`, `net`, `zlib`, `ws`), NDK
  drivers, heavy protocols, or modules that need minimum RAM.
- `node_modules` hybrid: app/framework packages comfortable in JS/TS but with a
  heavy C99 part.
- `node_modules` pure JS: lightweight utilities that touch no OS and are not a
  bottleneck.
