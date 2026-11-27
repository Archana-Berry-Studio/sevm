// Prove the CommonJS loader works end-to-end (sync require of CJS + builtin).
import { ensureGlobals, require, cwd } from './node_modules/mochinode/index.js';
await ensureGlobals();
const out = [];
function ok(name, cond) { out.push((cond ? 'PASS ' : 'FAIL ') + name); }
const dir = cwd();

try {
  const m = await require('./cjs_main.cjs', dir);
  ok('CJS module.exports', m && m.fromMain === 'yes');
  ok('CJS nested require (cjs_lib)', m.lib && m.lib.who === 'cjs-lib');
  ok('CJS __dirname', typeof m.dir === 'string' && m.dir.length > 0);
  ok('CJS __filename', typeof m.file === 'string' && m.file.endsWith('cjs_main.cjs'));
  ok('CJS builtin require (path)', typeof m.lib.join === 'string' && m.lib.sep === '/' && m.lib.join.includes('a'));
  ok('CJS builtin require (os)', typeof m.osPlatform === 'string' && m.osPlatform.length > 0);
  ok('CJS function export', m.lib.add(2, 3) === 5);
} catch (e) { ok('CJS load', false); console.log('CJS_ERR=' + e.message); }

// require.main / resolve / cache
try {
  ok('require.main.filename', typeof require.main === 'object' && typeof require.main.filename === 'string');
  const rp = require.resolve('./cjs_lib.cjs', dir);
  ok('require.resolve', typeof rp === 'string' && rp.length > 0 && rp.includes('cjs_lib'));
  ok('require.cache populated', require.cache instanceof Map && require.cache.size >= 2);
} catch (e) { ok('require.main/resolve/cache', false); console.log('RMAIN_ERR=' + e.message); }

// ESM still works after CJS loading
try {
  const os = await require('os', dir);
  ok('ESM still loads', typeof os.platform() === 'string');
} catch (e) { ok('ESM still loads', false); }

console.log(out.join('\n'));
console.log('CJS=' + out.filter(x => x.startsWith('PASS')).length + '/' + out.length);
