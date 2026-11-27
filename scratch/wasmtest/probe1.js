// Probe: does the pre-built full wasm-bridge plugin load under E:/sevm's engine?
import { loadNative } from '../../node_modules/mochinode/index.js';

const t0 = Date.now();
let mw = null;
try {
  mw = await loadNative('mochiwasmy_x86_64.dll');
  console.log('LOADED via loadNative(mochiwasmy_x86_64.dll)');
} catch (e) {
  console.log('LOAD_FAIL loadNative:', e && e.message ? e.message : e);
  try {
    mw = await loadNative('mochiwasmy.dll');
    console.log('LOADED via loadNative(mochiwasmy.dll)');
  } catch (e2) {
    console.log('LOAD_FAIL mochiwasmy.dll:', e2 && e2.message ? e2.message : e2);
  }
}

if (mw) {
  const keys = Object.keys(mw).sort();
  console.log('EXPORT_COUNT', keys.length);
  console.log('EXPORTS', keys.join(' | '));
  for (const k of ['wasmCompile', 'wasmInstantiate', 'wasmRegisterHost', 'wasmExportFunc', 'wasmCall', 'wasmValidate', 'wasmMemory', 'wasmMemoryLen', 'wasmDetachView', 'wasmDumpFunc']) {
    console.log('  has', k, '=', typeof mw[k]);
  }
}
console.log('probe1 ms', Date.now() - t0);