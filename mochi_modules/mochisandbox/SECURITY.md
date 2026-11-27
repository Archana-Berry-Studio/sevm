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


## Hasil audit lanjutan (2026-11-27) — constructor chains, clone, isolasi intrinsik

Ditambahkan bersama `clone(id, jsonText)` (deep-copy JSON host → realm,
tersimpan di `globalThis.__clone__`). Semua terukur pada `mbjs.exe` 7.329.331 B,
JS-first selftest 50/50, native 14/14:

1. **Rantai `.constructor` intrinsik — DITUTUP.** Untuk SETIAP intrinsik native
   (`RegExp`, `Object`, `Array`, `Date`, `JSON`, `Promise`, `Error`, `String`,
   `Number`, `Boolean`, `ArrayBuffer`, dan `Object.getPrototypeOf({}).constructor`),
   `X.constructor('return 40+2')()` ditolak dengan
   `mochisandbox: dynamic code disabled`. Ini menutup rute bypass yang lewat
   `RegExp.constructor`/`/x/.constructor.constructor` (dulu tidak dinetralkan).
2. **`Proxy` tidak bisa dipakai untuk kompilasi** — trap `get` yang memanggil
   `Function(k)` ditolak; trap data biasa (`new Proxy({a:1},{get:...}).a`) tetap
   jalan (nilai realm-eksternal saja).
3. **`Object.defineProperty` tidak bisa menghidupkan kembali `Function`** —
   `defineProperty(Object,'constructor',{value:Function})` tetap memunculkan
   deny, karena `Function` global sendiri sudah diganti deny().
4. **`Error.captureStackTrace` / `Error.prepareStackTrace`** tidak memberi
   escape: `prepareStackTrace` mengembalikan nilai realm-eksternal (terukur 42),
   tidak pernah menyediakan akses host atau kompilasi kode.
5. **`Symbol.toPrimitive`, `Proxy` get, `Symbol`, `ArrayBuffer`/typed array**
   berfungsi normal di realm tanpa escape. `BigInt` menghitung di realm tetapi
   **tidak dapat diserialisasi JSON** (keterbatasan `sbox_json`, bukan celah
   keamanan).
6. **Isolasi intrinsik PER-SANDBOX (bukan hanya global).** Mencemari
   `Array.prototype[0]` (read-only, value 99) di sandbox A:
   - di A: `new Array(1,2)[0]` → `TypeError: '0' is read-only` (racun berlaku),
   - di B (sandbox baru): `[1,2,3].join(',')` → `'1,2,3'` (TIDAK terpengaruh),
   - host: `[1,2,3]` → `[1,2,3]` (TIDAK terpengaruh).
   Setiap `MBJS_NewRuntime()+NewContext()` membawa intrinsik sendiri; racun
   intrinsik tidak menembus sandbox lain maupun host.
7. **Quirk engine (bukan celah):** array-literal `[1,2]` memakai fast-path yang
   melewati cek `[[Set]]` prototipe — jadi `[1,2][0]` tetap `1` meski
   `Array.prototype[0]` dijadikan non-writable; `new Array(1,2)[0]` justru
   melempar `TypeError`. Untuk mendeteksi racun prototipe, vektor yang benar
   adalah path konstruktor.

## clone(id, jsonText)
```js
sb.clone({ a: [1,2,3], s: 'x' });   // -> deep copy (host side)
sb.eval('__clone__.a[1]');          // -> 2  (realm side, copy asli)
```
`JSON.stringify` + `JSON.parse` di sisi realm — host object TIDAK direferensikan
(terukur: memutasi objek host setelah `clone` tidak mengubah salinan realm).
Objek siklik → `JSON.stringify` melempar di sisi host (tidak menggantung).

## Cara memakai
```js
const { sandbox } = await import('mochisandbox');
const s = sandbox({ strict: true, budgetMs: 2000, memLimit: 4 << 20 });
s.eval('1 + 1');              // 2
s.asyncEval('(async()=>1)()'); // {state:'fulfilled', value:1}
s.free();
```