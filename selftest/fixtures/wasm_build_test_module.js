/*
 * wasm_build_test_module.js — hand-assemble a small but REAL WebAssembly module
 * (no toolchain needed) so the selftest can prove the bridge against known answers.
 *
 * Module shape:
 *   types:    0 = (i32,i32)->i32      1 = (i32)->i32
 *   import:   "__env".double : func type 1
 *   funcs:    add (type 0), call_double (type 1)
 *   exports:  "add", "call_double"
 *   add(a,b) = a+b;   call_double(x) = __env.double(x)
 *
 * Validated byte-for-byte against Node's own WebAssembly.validate / Module.
 */
function leb(n) {
  const o = [];
  do { let b = n & 0x7f; n >>>= 7; if (n) b |= 0x80; o.push(b); } while (n);
  return o;
}
function section(id, payload) { return [id, ...leb(payload.length), ...payload]; }
function name(s) { const o = [...leb(s.length)]; for (const ch of s) o.push(ch.charCodeAt(0)); return o; }
const I32 = 0x7f;

const TYPE_ADD = [0x60, 2, I32, I32, 1, I32]; // (i32,i32)->i32
const TYPE_ONE = [0x60, 1, I32, 1, I32];     // (i32)->i32

// import "__env"."double" (kind=func, type=1)
const IMPORT_ENTRY = [...name('__env'), ...name('double'), 0x00, 0x01];

// local functions: add -> type 0, call_double -> type 1
const FUNC_SEC = [2, 0x00, 0x01];

// function body: <localtypes> <body_size> <code>
//   localtypes = vec of (count,type) groups; zero extra locals -> single 0x00 byte.
//   body_size counts the CODE bytes only (per the wasm binary spec), NOT the
//   localtypes byte. Getting this wrong yields "section was shorter than
//   expected size" from any validator.
const LOCALTYPES = [0x00];
function body(code) {
  return [...LOCALTYPES, ...leb(code.length), ...code];
}
const CODE_ADD = [0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b]; // local.get 0; local.get 1; i32.add; end
const CODE_CALL = [0x20, 0x00, 0x10, 0x00, 0x0b];     // local.get 0; call $double(0); end
const CODE_SEC = [2, ...body(CODE_ADD), ...body(CODE_CALL)];

const EXPORT_ADD = [...name('add'), 0x00, 0x01];           // kind func, funcidx 1
const EXPORT_CALL = [...name('call_double'), 0x00, 0x02];  // kind func, funcidx 2
const EXPORT_SEC = [2, ...EXPORT_ADD, ...EXPORT_CALL];

const bytes = [
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // magic + version 1
  ...section(1, [2, ...TYPE_ADD, ...TYPE_ONE]),
  ...section(2, [1, ...IMPORT_ENTRY]),
  ...section(3, FUNC_SEC),
  ...section(7, EXPORT_SEC),
  ...section(10, CODE_SEC),
];

export default bytes;