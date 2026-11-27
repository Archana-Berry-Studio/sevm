# mochisandbox — audit keamanan

Hasil pengukuran langsung (bukan tebakan) terhadap modul v0.1 yang dipush ke
`Mochi-Berry/sevm`. Setiap baris adalah output nyata dari satu proses
`mbjs.exe` terpisah, agar crash pada satu vektor tidak membuang hasil yang lain.

Perintah reproduksi:

```sh
cd E:/sevm
mbjs.exe -m mochi_modules/mochisandbox/script/test.js      # 11/11
mbjs.exe -m selftest/mochisandbox.js                       # 29/29
mbjs.exe -m selftest/mochisandbox_memlimit.js              # probe batas memori
```

## Garis pertahanan

| Lapis | Mekanisme | Bukti |
|---|---|---|
| Realm | `MBJS_NewRuntime` + `MBJS_NewContext` baru per sandbox | `process`/`dynalib`/`require`/`console`/`setTimeout` = `undefined` |
| Instruksi | `MBJS_SetInterruptHandler` + `budgetMs` | `while(true);` dibunuh, host tetap `ALIVE` |
| Memori | `MBJS_SetMemoryLimit` (runtime-wide, lokal sandbox) | `new SharedArrayBuffer(1<<30)` @ limit 1 MB → `InternalError: out of memory` |
| Kode dinamis | bootstrap mengganti `eval`, `Function`, `GeneratorFunction`, `AsyncFunction`, `AsyncGeneratorFunction` + `.constructor` rantai | ketiganya → `mochisandbox: dynamic code disabled` |
| Modul | realm tidak punya loader | `import('fs')`, `import('mochifs')`, `import('data:...')` → `could not load module` |
| Blok host | API engine menolak blok dari sandbox | `Atomics.wait(...)` → `TypeError: cannot block in this thread` |

## Temuan audit (31 + 20 + 13 + 12 vektor)

### 1. BYPASS TEMUKAN — sudah diperbaiki
Strict mode awalnya hanya menutup `eval`, `Function`, dan rantai `.constructor` biasa.
Constructor lain yang **juga** bisa kompilasi kode ternyata masih native asli:

```js
Object.getPrototypeOf(function*(){}).constructor('return 40+2')().next().value  // => 42
Object.getPrototypeOf(async function(){}).constructor('return 40+2')()          // => promise(42)
```

Konfirmasi dari realm sendiri:

```
what_is_function : "function|function deny(){throw new Error('mochisandbox: dynamic code "
what_is_ctor     : "function deny(){throw new Error('mochisandbox: dynamic code "
what_is_genfn    : "function GeneratorFunction() {\n    [native code]\n}"   <-- LUPUT
what_is_asyncfn  : "function AsyncFunction() {\n    [native code]\n}"     <-- LUPUT
```

**Perbaikan:** bootstrap kini menangkap tiga constructor tersebut *sebelum*
menetralkan, lalu mengganti `C.prototype.constructor` masing-masing dengan `deny`.
Sesudahnya semuanya → `dynamic code disabled`, dan `Constructor.name === "deny"`.

### 2. Tidak ada kebalikan (clean)
| Vektor | Hasil |
|---|---|
| `Error.constructor('return 1')()` | `dynamic code disabled` |
| `Object.getPrototypeOf(function(){}).constructor('return 1')()` | `dynamic code disabled` |
| `(function*(){})` / `Reflect.construct(Function,[...])` / `(0,eval)` | `dynamic code disabled` |
| `import('fs')` / `import('mochifs')` / `import('data:text/javascript,...')` | `could not load module` (bahkan `data:` URL), efek samping **tidak** terjadi |
| `Atomics.wait` (infinite) | `TypeError: cannot block in this thread` — host **tidak** pernah hang |
| `function f(){return f()} f()` | `RangeError: Maximum call stack size exceeded` (tertangkap) |
| `new SharedArrayBuffer(1<<30)` @ memLimit 1 MB | `InternalError: out of memory` (tertangkap) |

### 3. Rembesan minor (dipertahankan, dicatat)
| Item | Hasil | Penilaian |
|---|---|---|
| `JSON.stringify(this)` | `{"performance":{"timeOrigin":58733921.3519}}` | Ada global `performance`; membocorkan offset monotonic sejak boot. Side channel timing, bukan akses host. |
| `new Error('x').stack` | `    at <eval> (<sandbox>:1:5)\n` | Hanya label realm, **tidak** ada path host. |
| `Error.captureStackTrace` | ada tapi mengembalikan `undefined` | Shim V8 kosong; `.stack` → `TypeError`. Tidak ada rembesan. |
| `Object.defineProperty(Error,'name',{value:'PWNED'})` | mengembalikan `PWNED` | Mutasi **realm** sendiri. `setGlobal` mengevaluasi literal *di dalam* realm, jadi host tidak bisa menyuntikkan objek secara by-reference → prototipe host tidak terjangkau. |

### 4. Permukaan global realm (terukur, 70 nama)
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
Tidak ada `console`, `process`, `require`, `dynalib`, `mbjs_runtime`,
`setTimeout`, `setInterval`, `WebAssembly`, `Worker`, `fetch`, `crypto`.

`queueMicrotask` dan `WeakRef`/`FinalizationRegistry` tersedia, tetapi hanya
bisa mengaitkan objek **dalam realm yang sama** — tidak ada jembatan ke host
karena host tidak pernah menyerahkan nilai by-reference ke realm.

## Batasan yang harus jujur
1. `eval`/`Function` dinetralkan **hanya** ketika `strict: true`. Tanpa strict,
   `eval('1+1')` → `2` dan `Function('return 1')()` → `1`. Isolasinya tetap
   realm (host tetap tak terjangkau), tapi jaminan "no dynamic code" hanya
   berlaku pada mode strict.
2. `budgetMs` diukur lewat interrupt handler VM, bukan wall-clock — loop yang
   tak pernah kembali ke VM (misalnya blok di kode native host) tidak dihitung.
   Pada engine ini `Atomics.wait` sudah dijaga sendiri (`cannot block in this
   thread`).
3. `memLimit` adalah batas runtime, bukan kuota per-eval; sandbox boleh habis
   jatahnya lalu tetap hidup, tetapi alokasi berikutnya akan OOM hingga runtime
   baru dibuat.

## Cara memakai
```js
const { sandbox } = await import('mochisandbox');
const s = sandbox({ strict: true, budgetMs: 2000, memLimit: 4 << 20 });
s.eval('1 + 1');              // 2
s.asyncEval('(async()=>1)()'); // {state:'fulfilled', value:1}
s.free();
```