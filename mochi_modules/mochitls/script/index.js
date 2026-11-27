/*
 * mochitls — thin JS binding over the native x86_64 binary.
 * Loads lib/mochitls_x86_64.dll via the engine's native loader and re-exports
 * the C ABI. Prefer native-first: keep heavy work in C, keep this file a
 * 1:1 binding (no object-wrapping layer).
 *
 * The native module exports:
 *   tlsConnect(host, port) -> handle (or -1)
 *   tlsWrite(handle, data) -> plaintext bytes sent (or -1)
 *   tlsRead(handle, max)   -> decrypted string (or undefined on EOF/error)
 *   tlsClose(handle)
 *   tlsShutdown(handle)
 *   tlsLastError([handle]) -> string
 *   tlsVersion(handle)     -> negotiated protocol
 */
import { loadNative } from '../../../node_modules/mochinode/index.js';

let _m = null;
async function ensure() {
  if (!_m) _m = await loadNative('mochitls.dll');
  return _m;
}

export async function init() { return ensure(); }
export function isNativeReady() { return _m != null; }

export function __exports() {
  return _m ? Object.keys(_m) : [];
}

/* Thin 1:1 passthroughs (callers may use these directly). */
export async function connect(host, port) {
  const m = await ensure();
  return m.tlsConnect(host, port);
}
export async function write(handle, data) {
  const m = await ensure();
  return m.tlsWrite(handle, data);
}
export async function read(handle, max) {
  const m = await ensure();
  return m.tlsRead(handle, max);
}
export async function close(handle) {
  const m = await ensure();
  return m.tlsClose(handle);
}
export async function shutdown(handle) {
  const m = await ensure();
  return m.tlsShutdown(handle);
}
export async function lastError(handle) {
  const m = await ensure();
  return handle == null ? m.tlsLastError() : m.tlsLastError(handle);
}
export async function version(handle) {
  const m = await ensure();
  return m.tlsVersion(handle);
}

export default {
  load: () => ensure(),
  exports: () => (_m ? Object.keys(_m) : []),
};
