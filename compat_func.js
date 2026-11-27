// Functional verification of the enriched shim layer (not just typeof).
import { ensureGlobals, require, cwd } from './node_modules/mochinode/index.js';
await ensureGlobals();
const out = [];
function ok(name, cond) { out.push((cond ? 'PASS ' : 'FAIL ') + name); }

// console.error actually prints (ERR-MARK appears in captured stdout)
let consoleErrorPrints = false;
try { console.error('ERR-MARK'); consoleErrorPrints = true; } catch (e) { consoleErrorPrints = false; }
ok('console.error callable', consoleErrorPrints);

// process real values
ok('process.cwd() non-empty', typeof process.cwd() === 'string' && process.cwd().length > 0);
ok('process.platform', process.platform === 'win32' || process.platform === 'linux' || process.platform === 'android');
ok('process.argv array', Array.isArray(process.argv) && process.argv.length >= 1);
ok('process.env object', process.env && typeof process.env === 'object');
ok('process.versions.node', typeof process.versions.node === 'string');

// global === globalThis
ok('global === globalThis', global.global === globalThis);

// events.once real resolution
try {
  const ev = await require('events', cwd());
  const em = new ev.EventEmitter();
  const p = ev.once(em, 'data');
  setTimeout(() => em.emit('data', 42), 10);
  const [v] = await p;
  ok('events.once resolves 42', v === 42);
} catch (e) { ok('events.once resolves 42', false); }

// setImmediate really fires
try {
  let ran = false;
  setImmediate(() => { ran = true; });
  await new Promise(r => setTimeout(r, 30));
  ok('setImmediate fires', ran);
} catch (e) { ok('setImmediate fires', false); }

// os new functions
try {
  const os = await require('os', cwd());
  const ni = os.networkInterfaces();
  const ui = os.userInfo();
  ok('os.networkInterfaces lo', ni && ni.lo && ni.lo[0] && ni.lo[0].address === '127.0.0.1');
  ok('os.userInfo username str', ui && typeof ui.username === 'string' && ui.username.length > 0);
} catch (e) { ok('os new funcs', false); }

// Buffer real
try {
  const B = globalThis.Buffer;
  ok('Buffer.from hex', B.from('616263', 'hex').toString() === 'abc');
  ok('Buffer.concat', B.concat([B.from('a'), B.from('b')]).toString() === 'ab');
} catch (e) { ok('Buffer real', false); }

// zlib real roundtrip
try {
  const z = await require('zlib', cwd());
  const gz = z.gzipSync(Buffer.from('hello-shim'));
  const back = z.gunzipSync(gz).toString();
  ok('zlib gzip/gunzip roundtrip', back === 'hello-shim');
} catch (e) { ok('zlib roundtrip', false); }

console.log(out.join('\n'));
console.log('FUNC=' + out.filter(x => x.startsWith('PASS')).length + '/' + out.length);
