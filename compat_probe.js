// Node-compat surface probe (measures what the SHIM layer provides).
import { ensureGlobals, require, cwd } from './node_modules/mochinode/index.js';
await ensureGlobals();
const results = [];
function chk(name, cond) { results.push((cond ? 'PASS' : 'FAIL') + ' ' + name); }
const dir = cwd();
// --- globals ---
chk('global.process', typeof globalThis.process !== 'undefined');
chk('global.Buffer', typeof globalThis.Buffer !== 'undefined');
chk('global.global', typeof globalThis.global !== 'undefined');
chk('global.setTimeout', typeof globalThis.setTimeout === 'function');
chk('global.clearTimeout', typeof globalThis.clearTimeout === 'function');
chk('global.setInterval', typeof globalThis.setInterval === 'function');
chk('global.clearInterval', typeof globalThis.clearInterval === 'function');
chk('global.setImmediate', typeof globalThis.setImmediate === 'function');
chk('global.queueMicrotask', typeof globalThis.queueMicrotask === 'function');
chk('global.TextEncoder', typeof globalThis.TextEncoder === 'function');
chk('global.TextDecoder', typeof globalThis.TextDecoder === 'function');
chk('global.URL', typeof globalThis.URL === 'function');
chk('global.URLSearchParams', typeof globalThis.URLSearchParams === 'function');
chk('global.AbortController', typeof globalThis.AbortController === 'function');
chk('global.structuredClone', typeof globalThis.structuredClone === 'function');
chk('global.crypto', typeof globalThis.crypto !== 'undefined');
chk('global.fetch', typeof globalThis.fetch === 'function');
chk('console.error', typeof console.error === 'function');
chk('console.warn', typeof console.warn === 'function');
chk('console.info', typeof console.info === 'function');
chk('console.debug', typeof console.debug === 'function');
// --- process ---
if (globalThis.process) {
  const p = globalThis.process;
  chk('process.env', p.env && typeof p.env === 'object');
  chk('process.platform', typeof p.platform === 'string');
  chk('process.arch', typeof p.arch === 'string');
  chk('process.cwd()', typeof p.cwd === 'function' && typeof p.cwd() === 'string');
  chk('process.argv', Array.isArray(p.argv));
  chk('process.exit', typeof p.exit === 'function');
  chk('process.nextTick', typeof p.nextTick === 'function');
  chk('process.pid', typeof p.pid === 'number');
  chk('process.versions.node', p.versions && typeof p.versions.node === 'string');
  chk('process.stdout.write', p.stdout && typeof p.stdout.write === 'function');
  chk('process.memoryUsage()', typeof p.memoryUsage === 'function' && typeof p.memoryUsage().heapUsed === 'number');
  chk('process.uptime()', typeof p.uptime === 'function');
  chk('process.hrtime()', typeof p.hrtime === 'function');
  chk('process.on', typeof p.on === 'function');
}
// --- Buffer ---
if (globalThis.Buffer) {
  const B = globalThis.Buffer;
  chk('Buffer.from', typeof B.from === 'function');
  chk('Buffer.alloc', typeof B.alloc === 'function');
  chk('Buffer.concat', typeof B.concat === 'function');
  chk('Buffer.isBuffer', typeof B.isBuffer === 'function');
  chk('Buffer.byteLength', typeof B.byteLength === 'function');
  try { chk('buf.toString(hex)', B.from('ab','hex').toString('hex') === 'ab'); } catch(e) { chk('buf.toString(hex)', false); }
  try { chk('Buffer.concat works', B.concat([B.from('a'),B.from('b')]).toString() === 'ab'); } catch(e) { chk('Buffer.concat works', false); }
}
// --- path ---
try { const path = await require('path', dir);
  chk('path.join', typeof path.join === 'function' && path.join('a','b').length > 0);
  chk('path.resolve', typeof path.resolve === 'function');
  chk('path.dirname', typeof path.dirname === 'function');
  chk('path.basename', typeof path.basename === 'function');
  chk('path.extname', typeof path.extname === 'function' && path.extname('a.js') === '.js');
  chk('path.normalize', typeof path.normalize === 'function');
  chk('path.isAbsolute', typeof path.isAbsolute === 'function');
  chk('path.parse', typeof path.parse === 'function');
  chk('path.sep', typeof path.sep === 'string');
  chk('path.win32', path.win32 && typeof path.win32.join === 'function');
  chk('path.posix', path.posix && typeof path.posix.join === 'function');
} catch(e) { chk('path module', false); }
// --- os ---
try { const os = await require('os', dir);
  chk('os.platform()', typeof os.platform === 'function' && typeof os.platform() === 'string');
  chk('os.arch()', typeof os.arch === 'function');
  chk('os.hostname()', typeof os.hostname === 'function');
  chk('os.tmpdir()', typeof os.tmpdir === 'function');
  chk('os.homedir()', typeof os.homedir === 'function');
  chk('os.cpus()', typeof os.cpus === 'function');
  chk('os.totalmem()', typeof os.totalmem === 'function');
  chk('os.freemem()', typeof os.freemem === 'function');
  chk('os.EOL', typeof os.EOL === 'string');
  chk('os.type()', typeof os.type === 'function');
  chk('os.networkInterfaces()', typeof os.networkInterfaces === 'function');
  chk('os.userInfo()', typeof os.userInfo === 'function');
} catch(e) { chk('os module', false); }
// --- util ---
try { const util = await require('util', dir);
  chk('util.inspect', typeof util.inspect === 'function');
  chk('util.format', typeof util.format === 'function');
  chk('util.promisify', typeof util.promisify === 'function');
  chk('util.callbackify', typeof util.callbackify === 'function');
  chk('util.types', util.types && typeof util.types === 'object');
  chk('util.isDeepStrictEqual', typeof util.isDeepStrictEqual === 'function');
  chk('util.deprecate', typeof util.deprecate === 'function');
  chk('util.inherits', typeof util.inherits === 'function');
} catch(e) { chk('util module', false); }
// --- events ---
try { const ev = await require('events', dir);
  chk('events.EventEmitter', typeof ev.EventEmitter === 'function');
  chk('events.once', typeof ev.once === 'function');
  try { const em = new ev.EventEmitter(); let got=0; em.on('x',()=>{got++}); em.emit('x'); chk('EventEmitter emit', got===1); } catch(e) { chk('EventEmitter emit', false); }
} catch(e) { chk('events module', false); }
// --- stream ---
try { const st = await require('stream', dir);
  chk('stream.Readable', typeof st.Readable === 'function');
  chk('stream.Writable', typeof st.Writable === 'function');
  chk('stream.Duplex', typeof st.Duplex === 'function');
  chk('stream.Transform', typeof st.Transform === 'function');
  chk('stream.PassThrough', typeof st.PassThrough === 'function');
  chk('stream.pipeline', typeof st.pipeline === 'function');
  chk('stream.finished', typeof st.finished === 'function');
  chk('stream.promises', st.promises && typeof st.promises === 'object');
} catch(e) { chk('stream module', false); }
// --- url ---
try { const u = await require('url', dir);
  chk('url.URL', typeof u.URL === 'function');
  chk('url.URLSearchParams', typeof u.URLSearchParams === 'function');
  chk('url.fileURLToPath', typeof u.fileURLToPath === 'function');
  chk('url.pathToFileURL', typeof u.pathToFileURL === 'function');
  chk('url.parse', typeof u.parse === 'function');
} catch(e) { chk('url module', false); }
// --- timers: setImmediate actually runs ---
try {
  let ran = false;
  globalThis.setImmediate && globalThis.setImmediate(() => { ran = true; });
  await new Promise(r => setTimeout(r, 30));
  chk('setImmediate runs', ran);
} catch(e) { chk('setImmediate runs', false); }
// --- zlib (native shim) ---
try { const z = await require('zlib', dir); chk('zlib module', typeof z === 'object' && typeof z.gzipSync === 'function' && typeof z.deflateSync === 'function'); } catch(e) { chk('zlib module', false); }
const pass = results.filter(r => r.startsWith('PASS')).length;
console.log(results.join('\n'));
console.log('COMPAT=' + pass + '/' + results.length);
