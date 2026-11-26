/*
 * mochisandbox — thin JS binding over the native x86_64 binary.
 * Loads lib/mochisandbox_x86_64.dll via the engine's native loader and
 * re-exports a sandbox() factory. Each sandbox is a dedicated engine
 * runtime+context: fresh globals, own GC, own memory limit, own instruction
 * budget, optional strict mode (eval/Function neutralized).
 *
 * Usage:
 *   const { sandbox } = await init();
 *   const sb = sandbox({ budgetMs: 200, memLimit: 16*1024*1024, strict: true });
 *   sb.setGlobal('log', 'hello');        // primitive only
 *   const out = sb.run('1 + 2');         // -> "3" (JSON string)
 *   sb.free();
 */
import { loadNative } from '../../../node_modules/mochinode/index.js';

let _m = null;
async function ensure() {
  if (!_m) _m = await loadNative('mochisandbox.dll');
  return _m;
}

function jstr(s) {
  /* JSON-quote a string to a valid JS literal for splicing into sandbox code. */
  return JSON.stringify(String(s));
}

/* High-level sandbox object (lives on the JS side; the native side keeps a
   runtime handle keyed by id). */
export class Sandbox {
  constructor(m, id, opts) {
    this._m = m;
    this._id = id;
    this._opts = opts || {};
  }
  setGlobal(name, value) {
    this._m.setGlobal(this._id, jstr(name), jstr(value));
  }
  /* setRawGlobal: splice a pre-formed literal (e.g. '42', 'true', 'null',
     '[1,2,3]'). Use with caution — the literal is evaluated in the realm. */
  setRawGlobal(name, lit) {
    this._m.setGlobal(this._id, jstr(name), String(lit));
  }
  run(source) {
    return this._m.run(this._id, String(source));
  }
  /* runAsync: eval an async global script, drain this realm's microtask queue,
     then resolve the resulting promise. Returns {state, value} or
     {state:"pending"}; a rejected promise throws here. */
  runAsync(source) {
    return JSON.parse(this._m.runAsync(this._id, String(source)));
  }
  free() {
    this._m.free(this._id);
  }
}

export async function init() { return ensure(); }
export function isNativeReady() { return _m != null; }

export async function sandbox(opts) {
  const m = await ensure();
  const id = m.create(opts || {});
  return new Sandbox(m, id, opts || {});
}

export function __exports() {
  return _m ? Object.keys(_m) : [];
}

export default {
  load: () => ensure(),
  sandbox,
  exports: () => (_m ? Object.keys(_m) : []),
};
