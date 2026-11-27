// Probe 2: direct import of the arch-suffixed lib path + real wasm round-trip.
import * as mw from '../../mochi_modules/mochiwasmy/lib/mochiwasmy_x86_64.dll';
console.log('DIRECT_IMPORT_OK', Object.keys(mw).length, Object.keys(mw).sort().join('|'));

// Tiny real wasm module:
//   (module
//     (import "env" "double" (func $double (param i32) (result i32)))
//     (func (export "add") (param i32 i32) (result i32) local.get 0 local.get 1 i32.add)
//     (func (export "call_double") (param i32) (result i32) local.get 0 call $double))
const b64 = 'AGFzbQEAAAABCAh/AggBAgEABwcBAF9fZW52BGJ0b3VibGUKBwEBAEABAAAFBgEBAAEFaQBfYWRkAAAACgYBAQABBWNgbGxfZG91YmxlAAAKAhoAAQAFAAMAFgABAG0AbABvY2FsLjAAbQABAAgAAA==';
function b64ToBytes(s) {
  const bin = atob(s);
  const u = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);
  return u;
}
const bytes = b64ToBytes(b64);
console.log('magic', bytes[0], bytes[1], bytes[2], bytes[3], 'len', bytes.byteLength);
const ab = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
console.log('validate(typed array) =', mw.wasmValidate(bytes));
console.log('validate(ArrayBuffer) =', mw.wasmValidate(ab));
const mh = mw.wasmCompile(ab);
console.log('module handle', mh);
const ih = mw.wasmInstantiate(mh, null);
console.log('instance handle', ih);

const hostLog = [];
function doubleHost(args) { const a = Array.isArray(args) ? args[0] : args[0]; return (a * 2) | 0; }
const okReg = mw.wasmRegisterHost(ih, '__env', 'double', doubleHost);
console.log('REGISTRATION_LINE: wasmRegisterHost(ih=__env,double) ->', okReg);

const addIdx = mw.wasmExportFunc(ih, 'add');
const cdIdx = mw.wasmExportFunc(ih, 'call_double');
console.log('addIdx', addIdx, 'call_doubleIdx', cdIdx);
const addRes = mw.wasmCall(ih, addIdx, [2, 40]);
console.log('add(2,40) ->', JSON.stringify(addRes), 'expect 42');
const cdRes = mw.wasmCall(ih, cdIdx, [21]);
console.log('call_double(21) ->', JSON.stringify(cdRes), 'expect 42 (host import)');