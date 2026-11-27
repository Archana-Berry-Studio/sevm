# Engine assets

## Build in use

| path | role | used? |
|---|---|---|
| `E:/bcf/mochiberry/build_manual/mbjs.exe` | runtime + JIT/AOT, **has the `MBJS_*` export table** | **yes** — every selftest runs here |
| `E:/bcf/mochiberry/build/mbjs.exe` | generic build | maybe, exports may differ |
| `E:/bcf/mochiberry/build/tier-core/mbjs.exe` | stripped | **no** — no export table → native plugins fail to resolve engine symbols |

Why `build_manual` and not `tier-core`: native plugins in `mochi_modules/`
resolve engine symbols through the host image export table
(`native/mbjs_runtime.h`). A stripped build does not export `MBJS_*`, so
`mbjs_init_module` cannot find the runtime symbols.

## Headers & source a plugin build needs

- `E:/bcf/mochiberry/*.h` — engine headers (`mbjs.h`, `mbjs-runtime.h`, etc.).
  Build flags: `gcc -shared -O2 -I E:/bcf/mochiberry …`.
- `E:/bcf/mochiberry/mbjs-libc.c` — implementation of the builtins `mbjs:os`
  and `mbjs:std` (`os.platform/getcwd/getenv/setTimeout/setInterval/clearTimeout/clearInterval`,
  `std.loadFile/exit`).

## Engine export table

- `E:/sevm/build_ex/exports.txt` — dump of the 385 `MBJS_*` symbols (reference;
  regenerate if the engine is rebuilt).

## Platform

The build host today is **Windows x86_64** (`.dll`). Cross-platform naming:

| OS | binary name |
|---|---|
| Windows x64 | `<module>_x86_64.dll` |
| Linux x64 | `<module>_x86_64.so` |
| Linux/Android ARM64 | `<module>_aarch64.so` |
| macOS Intel | `<module>_x86_64.dylib` |
| macOS Apple Silicon | `<module>_arm64.dylib` |
