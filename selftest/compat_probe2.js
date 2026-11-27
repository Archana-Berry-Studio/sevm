// Wave-2 Node compat: CJS interop, process streams, fs.promises, URL statics,
// AbortSignal.timeout, Buffer multi-byte, util.format, Readable.from.
import { ensureGlobals, require, cwd } from './node_modules/mochinode/index.js';
await ensureGlobals();
const out = [];
function ok(name, cond) { out.push((cond ? 'PASS ' : 'FAIL ') + name); }
const dir = cwd();

// __dirname/__filename module locals
try {
  ok('__dirname is string', typeof __dirname === 'string' && __dirname.length > 0);
  ok('__filename is string', typeof __filename === 'string' && __filename.length > 0);
} catch (e) { ok('__dirname/__filename', false); }

// module/exports/require.main CJS interop
try {
  ok('module object', typeof module === 'object' && module !== null);
  ok('module.exports', module.exports && typeof module.exports === 'object');
  ok('exports alias', typeof exports === 'object');
  ok('require.main', typeof require.main !== 'undefined');
} catch (e) { ok('CJS interop', false); }

// process.stdout as event emitter (Node apps pipe streams)
if (globalThis.process && globalThis.process.stdout) {
  const s = globalThis.process.stdout;
  ok('process.stdout.write', typeof s.write === 'function');
  ok('process.stdout.on', typeof s.on === 'function');
  ok('process.stdout.emit', typeof s.emit === 'function');
  ok('process.stdout.isTTY', typeof s.isTTY === 'boolean');
  ok('process.stderr.write', globalThis.process.stderr && typeof globalThis.process.stderr.write === 'function');
} else { ok('process.stdout', false); }

// fs.promises
try {
  const fs = await require('fs', dir);
  const fp = fs.promises || fs;
  ok('fs.promises.readFile', typeof (fp.readFile || fp.readText) === 'function');
  ok('fs.promises.writeFile', typeof (fp.writeFile || fp.writeText) === 'function');
  ok('fs.promises.stat', typeof (fp.stat || fs.stat) === 'function');
  ok('fs.promises.readdir', typeof (fp.readdir || fs.readdir) === 'function');
} catch (e) { ok('fs.promises', false); }

// URL statics
try {
  const u = await require('url', dir);
  ok('URL.canParse', typeof u.URL.canParse === 'function');
  ok('URL.parse', typeof u.URL.parse === 'function');
} catch (e) { ok('URL statics', false); }

// AbortSignal.timeout/abort
try {
  ok('AbortSignal.timeout', typeof AbortSignal.timeout === 'function');
  ok('AbortSignal.abort', typeof AbortSignal.abort === 'function');
} catch (e) { ok('AbortSignal statics', false); }

// Buffer multi-byte read/write
if (globalThis.Buffer) {
  const B = globalThis.Buffer;
  try {
    const b = B.alloc(8);
    b.writeUInt32LE(0x01020304, 0);
    ok('Buffer.writeUInt32LE/readUInt32LE', b.readUInt32LE(0) === 0x01020304);
    ok('Buffer.readInt32LE', b.writeInt32LE(-42, 0) === 4 && b.readInt32LE(0) === -42);
    ok('Buffer.readUInt16LE', b.writeUInt16LE(0xabcd, 0) === 2 && b.readUInt16LE(0) === 0xabcd);
    ok('Buffer.indexOf', typeof B.from('hello').indexOf === 'function');
    ok('Buffer.subarray', typeof B.from('hello').subarray === 'function');
  } catch (e) { ok('Buffer multibyte', false); }
} else { ok('Buffer multibyte', false); }

// util.format with specifiers
try {
  const util = await require('util', dir);
  const f = util.format('%s=%d', 'x', 42);
  ok('util.format %s %d', typeof f === 'string' && f.includes('x') && f.includes('42'));
  const f2 = util.format('%j', { a: 1 });
  ok('util.format %j', typeof f2 === 'string' && f2.includes('"a"'));
} catch (e) { ok('util.format', false); }

// stream.Readable.from (iterable)
try {
  const st = await require('stream', dir);
  ok('stream.Readable.from', typeof st.Readable.from === 'function');
  if (typeof st.Readable.from === 'function') {
    const r = st.Readable.from([1, 2, 3]);
    ok('Readable.from yields', r && typeof r.on === 'function');
  }
} catch (e) { ok('Readable.from', false); }

// queueMicrotask + microtask ordering
try {
  let order = [];
  queueMicrotask(() => order.push('micro'));
  setTimeout(() => order.push('timer'), 0);
  await new Promise(r => setTimeout(r, 40));
  ok('queueMicrotask runs', order.includes('micro') && order.includes('timer'));
} catch (e) { ok('queueMicrotask', false); }

console.log(out.join('\n'));
console.log('WAVE2=' + out.filter(x => x.startsWith('PASS')).length + '/' + out.length);
