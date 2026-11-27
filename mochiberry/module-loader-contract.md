# Native module loader contract

How the engine loads plugins from `mochi_modules/` and the rules a plugin MUST
honour. This is the result of reverse-disassembling `mbjs.exe` + engine source —
not a guess.

## Load flow

1. JS calls `mochinode.loadNative('<module>.dll')`.
2. `loadNative` searches for the file in:
   1. `node_modules/<pkg>/bin/<name><ext>` (package-bundled binary), then
   2. `mochi_modules/<name><ext>` (engine plugin registry).
3. `loadNative` performs `import('<abs path .dll>')`; the engine's native loader
   handles specifiers ending in `.dll`/`.so`/`.lbrct`.
4. The engine calls the entry point **`mbjs_init_module(ctx, name)`**.

## Mandatory rules

- The DLL's exported entry point **must** be `mbjs_init_module`.
- The import specifier **must** end in `.dll` (Windows) / `.so` (Linux) /
  `.lbrct` (other platforms).
- Inside `mbjs_init_module`, call `AddModuleExportList(ctx, m, tab, n)` to
  **register the export names**.
- Inside the module init func, call `SetModuleExportList(ctx, m, tab, n)` to
  fill in the export **values** (see the `zlib_mod_init` + `mbjs_init_module`
  pattern in `native/mochizlib.c`).
- A plugin **must not** import `mbjs.exe`. If the plugin load fails with Windows
  error `126`, it is almost certainly importing engine symbols as static
  imports. Fix: resolve at runtime through `native/mbjs_runtime.h`
  (`mbjs_runtime_resolve(&rt)` fills a function-pointer table).

## Runtime export registry (measured)

Each plugin's JS-visible exports are counted at runtime and recorded in
`mochi_modules/manifest.json` and `mochi_modules/<module>/manifest.json`.
To re-measure:

```js
const { loadNative } = await import('./node_modules/mochinode/index.js');
const ns = await loadNative('mochizlib.dll');
print(Object.keys(ns).sort().join(', '));
```
