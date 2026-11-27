# mochisandbox — security audit

Direct measurement (not guesswork) of the v0.1 module pushed to
`Mochi-Berry/sevm`. Every row is the real output of one separate `mbjs.exe`
process, so a crash in one vector does not discard the other results.

Reproduction commands:

```sh
cd E:/sevm
mbjs.exe -m mochi_modules/mochisandbox/script/test.js      # 11/11
mbjs.exe -m selftest/mochisandbox.js                       # 29/29
mbjs.exe -m selftest/mochisandbox_memlimit.js              # memory-limit probe
```

## Defense layers

| Layer | Mechanism | Evidence |
|---|---|---|
| Realm | `MBJS_NewRuntime` + `MBJS_NewContext` new per sandbox | `process`/`dynalib`/`require`/`console`/`setTimeout` = `undefined` |
| Instructions | `MBJS_SetInterruptHandler` + `budgetMs` | `while(true);` killed, host stays `ALIVE` |
| Memory | `MBJS_SetMemoryLimit` (runtime-wide, sandbox-local) | `new SharedArrayBuffer(1<<30)` @ 1 MB limit -> `InternalError: out of memory` |
| Dynamic code | bootstrap replaces `eval`, `Function`, `GeneratorFunction`, `AsyncFunction`, `AsyncGeneratorFunction` + `.constructor` chains | all -> `mochisandbox: dynamic code disabled` |
| Modules | realm has no loader | `import('fs')`, `import('mochifs')`, `import('data:...')` -> `could not load module` |
| Host blocking | engine API refuses blocking from a sandbox | `Atomics.wait(...)` -> `TypeError: cannot block in this thread` |

## Audit findings (31 + 20 + 13 + 12 vectors)

### 1. BYPASS FOUND — fixed
Strict mode initially only closed `eval`, `Function` and the plain `.constructor`
chain. Other constructors that **also** compile code were still native:

```js
Object.getPrototypeOf(function*(){}).constructor('return 40+2')().next().value  // => 42
Object.getPrototypeOf(async function(){}).constructor('return 40+2')()          // => promise(42)
```

Confirmed from the realm itself:

```
what_is_function : "function|function deny(){throw new Error('mochisandbox: dynamic code "
what_is_ctor     : "function deny(){throw new Error('mochisandbox: dynamic code "
what_is_genfn    : "function GeneratorFunction() {\n    [native code]\n}"
what_is_asyncfn  : "function AsyncFunction() {\n    [native code]\n}"
```

**Fix:** the bootstrap now captures those three constructors *before*
neutralising them, then replaces each `C.prototype.constructor` with `deny`.
Afterwards everything -> `dynamic code disabled`, and `Constructor.name === "deny"`.

### 2. No reversals (clean)
| Vector | Result |
|---|---|
| `Error.constructor('return 1')()` | `dynamic code disabled` |
| `Object.getPrototypeOf(function(){}).constructor('return 1')()` | `dynamic code disabled` |
| `(function*(){})` / `Reflect.construct(Function,[...])` / `(0,eval)` | `dynamic code disabled` |
| `import('fs')` / `import('mochifs')` / `import('data:text/javascript,...')` | `could not load module` (even `data:` URL), no side effects |
| `Atomics.wait` (infinite) | `TypeError: cannot block in this thread` — host **never** hangs |
| `function f(){return f()} f()` | `RangeError: Maximum call stack size exceeded` (caught) |
| `new SharedArrayBuffer(1<<30)` @ memLimit 1 MB | `InternalError: out of memory` (caught) |

### 3. Minor leakage (kept, recorded)
| Item | Result | Assessment |
|---|---|---|
| `JSON.stringify(this)` | `{"performance":{"timeOrigin":58733921.3519}}` | A `performance` global exists; leaks monotonic offset since boot. Timing side channel, not host access. |
| `new Error('x').stack` | `    at <eval> (<sandbox>:1:5)\n` | Only realm label, **no** host path. |
| `Error.captureStackTrace` | present but returns `undefined` | Empty V8 shim; `.stack` -> `TypeError`. No leakage. |
| `Object.defineProperty(Error,'name',{value:'PWNED'})` | returns `PWNED` | Mutates the **realm** itself. `setGlobal` evaluates the literal *inside* the realm, so the host cannot inject by-reference objects -> host prototypes unreachable. |

### 4. Realm global surface (measured, 70 names)
```
Object Function Error EvalError RangeError ReferenceError SyntaxError
TypeError URIError InternalError AggregateError SuppressedError Iterator
Array parseInt parseFloat isNaN isFinite queueMicrotask decodeURI
decodeURIComponent encodeURI encodeURIComponent escape unescape Infinity NaN
undefined eval Number Boolean String Math Reflect Symbol DisposableStack
globalThis BigInt Date RegExp JSON Proxy Map Set WeakMap WeakSet ArrayBuffer
SharedArrayBuffer Uint8ClampedArray Int8Array Uint8Array Int16Array Uint16Array
Int32Array Uint32Array BigInt64Array BigUint64Array Float16Array Float32Array
Float64Array DataView Atomics Promise AsyncDisposableStack WeakRef
FinalizationRegistry DOMException btoa atob performance
```
No `console`, `process`, `require`, `dynalib`, `mbjs_runtime`, `setTimeout`,
`setInterval`, `WebAssembly`, `Worker`, `fetch`, `crypto`.

`queueMicrotask` and `WeakRef`/`FinalizationRegistry` are available but can only
associate objects **within the same realm** — no bridge to the host, because the
host never hands a by-reference value to the realm.

## Honest limitations
1. `eval`/`Function` are neutralised **only** when `strict: true`. Without
   strict, `eval('1+1')` -> `2` and `Function('return 1')()` -> `1`. Realm
   isolation still holds (host unreachable), but the "no dynamic code"
   guarantee only applies in strict mode.
2. `budgetMs` is measured through the VM interrupt handler, not wall-clock —
   a loop that never returns to the VM (e.g. blocked in native host code) is not
   counted. On this engine `Atomics.wait` is already guarded
   (`cannot block in this thread`).
3. `memLimit` is a runtime limit, not a per-eval quota; the sandbox may spend its
   budget and stay alive, but the next allocation will OOM until a new runtime is
   created.


## Follow-up audit results (2026-11-27) — constructor chains, clone, intrinsic isolation

Added together with `clone(id, jsonText)` (deep-copy JSON host -> realm, stored
in `globalThis.__clone__`). All measured on `mbjs.exe` 7,329,331 B, JS-first
selftest 50/50, native 14/14:

1. **Intrinsic `.constructor` chains — CLOSED.** For EVERY native intrinsic
   (`RegExp`, `Object`, `Array`, `Date`, `JSON`, `Promise`, `Error`, `String`,
   `Number`, `Boolean`, `ArrayBuffer`, and `Object.getPrototypeOf({}).constructor`),
   `X.constructor('return 40+2')()` is rejected with
   `mochisandbox: dynamic code disabled`. This closes the bypass route through
   `RegExp.constructor`/`/x/.constructor.constructor` (previously not neutralised).
2. **`Proxy` cannot be used to compile** — a `get` trap calling `Function(k)` is
   rejected; a plain data trap (`new Proxy({a:1},{get:...}).a`) still works
   (realm-external value only).
3. **`Object.defineProperty` cannot revive `Function`** —
   `defineProperty(Object,'constructor',{value:Function})` still surfaces deny,
   because the global `Function` itself is already replaced with `deny()`.
4. **`Error.captureStackTrace` / `Error.prepareStackTrace`** give no escape:
   `prepareStackTrace` returns a realm-external value (measured 42), never
   provides host access or code compilation.
5. **`Symbol.toPrimitive`, `Proxy` get, `Symbol`, `ArrayBuffer`/typed arrays**
   work normally in the realm without escape. `BigInt` computes in the realm but
   is **not JSON-serialisable** (a `sbox_json` limitation, not a security hole).
6. **PER-SANDBOX intrinsic isolation (not just global).** Poisoning
   `Array.prototype[0]` (read-only, value 99) in sandbox A:
   - in A: `new Array(1,2)[0]` -> `TypeError: '0' is read-only` (poison applies),
   - in B (new sandbox): `[1,2,3].join(',')` -> `'1,2,3'` (NOT affected),
   - host: `[1,2,3]` -> `[1,2,3]` (NOT affected).
   Each `MBJS_NewRuntime()+NewContext()` carries its own intrinsics; an intrinsic
   poison does not cross into another sandbox or the host.
7. **Engine quirk (not a hole):** the array literal `[1,2]` uses a fast path that
   bypasses the prototype `[[Set]]` check — so `[1,2][0]` stays `1` even when
   `Array.prototype[0]` is made non-writable; `new Array(1,2)[0]` throws a
   `TypeError`. To detect prototype poison, the correct vector is the
   constructor path.

## clone(id, jsonText)
```js
sb.clone({ a: [1,2,3], s: 'x' });   // -> deep copy (host side)
sb.eval('__clone__.a[1]');          // -> 2  (realm side, real copy)
```
`JSON.stringify` + `JSON.parse` on the realm side — the host object is NOT
referenced (measured: mutating the host object after `clone` does not change the
realm copy). Cyclic objects -> `JSON.stringify` throws on the host side (no hang).

## Usage
```js
const { sandbox } = await import('mochisandbox');
const s = sandbox({ strict: true, budgetMs: 2000, memLimit: 4 << 20 });
s.eval('1 + 1');              // 2
s.asyncEval('(async()=>1)()'); // {state:'fulfilled', value:1}
s.free();
```
