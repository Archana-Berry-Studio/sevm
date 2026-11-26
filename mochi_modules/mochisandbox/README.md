# mochisandbox

Realm-isolated JavaScript execution for MochiBerry.

## Why

The host engine context exposes the application's globals (`process`,
`dynalib`, `fetch`, `mochi.*`, ...). Running untrusted plugin code in that
context gives it the whole host. `mochisandbox` gives you a **Lua-`_ENV`
style sandbox**: each sandbox is a *fresh engine runtime + context*, so the
code inside sees only standard JS intrinsics and whatever you explicitly
inject with `setGlobal()`.

## Enforcement (separation of mechanism / policy)

| Capability | Mechanism (engine) | Policy (this module) |
|---|---|---|
| Realm isolation | `MBJS_NewRuntime` + `MBJS_NewContext` (own atom table, own GC) | `sandbox()` creates one |
| Memory limit | `MBJS_SetMemoryLimit` (runtime-wide) | `memLimit` option |
| Instruction budget | `MBJS_SetInterruptHandler` (deadline check on VM back-edges) | `budgetMs` option |
| No dynamic code | `eval`/`Function`/`constructor` chain neutralized | `strict: true` |

## Layout

```
mochisandbox/
├── CMakeLists.txt
├── include/mochisandbox_abi.h      ABI surface (dynalib / loadNative)
├── native/mochisandbox.c           engine plugin (mbjs_init_module)
├── script/index.js                 JS binding: sandbox() factory
├── script/test.js                  selftest (mbjs.exe -m script/test.js)
├── lib/mochisandbox_x86_64.dll     built binary
└── manifest.json
```

## API

```js
import { sandbox, init } from 'mochisandbox';

await init();
const sb = sandbox({ budgetMs: 200, memLimit: 16 * 1024 * 1024, strict: true });

sb.setGlobal('greeting', 'halo');       // primitives only
const out = sb.run('greeting + " world"');  // -> '"halo world"' (JSON string)
sb.free();
```

- `sandbox(opts)` → `Sandbox`. `opts`:
  - `budgetMs` — interrupt deadline; a runaway loop throws `mochisandbox: interrupted`-style error.
  - `memLimit` — bytes; the sandbox runtime aborts with a memory error when exceeded.
  - `strict` — neutralize `eval`, `Function`, and the `constructor` chain.
- `Sandbox.setGlobal(name, value)` — inject a primitive (string/number/bool/null).
- `Sandbox.setRawGlobal(name, literal)` — splice a pre-formed literal (`'42'`, `'[1,2,3]'`). Use with caution.
- `Sandbox.run(source)` — evaluate as a global script; returns `JSON.stringify` of the completion value.
  Throws a host `Error` with the sandbox exception message on failure.
- `Sandbox.free()` — destroys the runtime + context (frees its GC heap).

## Boundary / limits (v0.1)

- Results cross the realm as **JSON strings only** — a value from one runtime
  cannot be used in another. No object/function handles.
- `setGlobal` accepts primitives only; arrays/objects/functions must be
  serialized to JSON by the caller and re-parsed inside (`setRawGlobal`).
- `run()` is a global script eval, not a module loader — no `import`/`require`
  resolution inside the sandbox in v0.1.
- Host capabilities (`process`, `dynalib`, `mochi.*`) are **not** injected by
  default; add them deliberately via `setGlobal`/`setRawGlobal` if a policy
  allows it.

## Building

```
cmake -S . -B build -G "MinGW Makefiles" -DMBJS_INC=E:/bcf/mochiberry
cmake --build build
# -> lib/mochisandbox_x86_64.dll
```
