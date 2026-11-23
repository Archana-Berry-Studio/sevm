# dynalib

Dynamic loader API for the SEVM stack — the JavaScript face of the C99
`libdynalib` subsystem (PE/ELF export tables, ASLR-aware offsets, JIT
trampolines, W^X pages).

```js
import { dynalib } from './node_modules/dynalib/index.js';

const target = dynalib(file, reg_or_offset, symbol, abi_sig, opts);
```

## Two modes

**1. Direct** — `symbol` or `reg_or_offset` is given:

```js
const add = dynalib('dbtest.dll', 0, 'dbtest_add', 'i32(i32,i32)');
add.call(3, 4);          // -> 7   (a real native call)
add.ptr;                 // absolute address of the function
add.base;                // module base after ASLR
add.offset;              // RVA inside the module
add.abi;                 // parsed ABI description
add.close();
```

**2. Module** — both `reg_or_offset` and `symbol` are `null`:

```js
const mod = dynalib('C:/Python314/DLLs/sqlite3.dll', null, null);
mod.exports();           // [{ name, rva, ordinal }, ...]  (287 for sqlite3)
mod.base;                // mapped base address
mod.sym('sqlite3_libversion');   // -> direct-mode handle
mod.close();
```

## Parameters

| param | meaning |
|-------|---------|
| `file` | explicit path, or a short name searched in `mochi_modules/` (JS) and `./mochimodule/` (libdynalib's own C convention) |
| `reg_or_offset` | RVA relative to the module base (ASLR-compensated), or `null` |
| `symbol` | export name, or `null` for a pure offset jump |
| `abi_sig` | type contract: `"i32(i32,i32)"`, `"ptr()"`, `"u64(ptr,u32)"`, plus an optional ABI word (`"sysv"`, `"ms_abi"`, `"stdcall"`, `"syscall"`, or `"reg:rcx,rdx->rax"`) |
| `opts` | `{ rawAddr: true }` returns the raw address instead of a handle |

When both `offset` and `symbol` are given, the symbol is tried first and the
loader falls back to `base + offset` for stripped images.

## Limits (measured, not guessed)

- **Stack arguments work.** Calls with 5, 7, 8 and 12 arguments return the
  right values and keep slot order (`mix7(1..7) == 1234567`); the MS64
  stack-argument offset was measured on this host (arg 5 sits at
  `[entry_rsp + 0x28]`, after 32 bytes of shadow space).
- **64-bit returns arrive as `BigInt`** (`u64` → BigInt, `i64` → BigInt,
  `void` → `undefined`). `u64`/`i64` *arguments* should be passed as
  `BigInt`; a JS Number loses precision above 2^53.
- **Floating-point arguments and returns work.** `f32`/`f64` slots are
  bit-cast into the argument buffer and routed into xmm registers by the
  trampoline; mixed contracts (`f64(i32, f64, i32, f64, f64)`) keep the
  integer and float budgets apart, and an `f32` return is widened for the
  caller. Verified: `fadd(2.5, 4.25) == 6.75`, `fmix5 == 1239`,
  `fsum9(1..9) == 45`, `fhalf(7.0) == 3.5`, and `msvcrt!sqrt(2) ==
  1.4142135623730951` through a real CRT call.
- **At most 12 arguments** through this bridge (the ABI contract table
  holds 16, the JS bridge exposes 12).
- Calling without an `abi_sig` is a *raw* call: the stub forwards zero
  arguments and hands back the raw 64-bit result as a Number.
- `opts.pinGc` and `opts.lazy` are accepted but not implemented yet.

## Win32 from JavaScript, zero C wrapper

```js
const tick = dynalib('C:/Windows/System32/kernel32.dll', 0,
                     'GetTickCount64', 'u64()');
tick.call();        // -> 182347031n   (BigInt)

const metrics = dynalib('C:/Windows/System32/user32.dll', 0,
                        'GetSystemMetrics', 'i32(i32)');
metrics.call(0);    // -> screen width

const sqrt = dynalib('C:/Windows/System32/msvcrt.dll', 0,
                     'sqrt', 'f64(f64)');
sqrt.call(2);       // -> 1.4142135623730951
```

The library side of this bridge lives in its own repo:
https://github.com/Archana-Berry-Studio/libmochiberry
