import { Readable, Transform, collect } from '../node_modules/mochistream/index.js';
import { inflateSync, deflateSync, isNativeReady } from '../node_modules/mochizlib/index.js';
import * as os from '../node_modules/mochios/index.js';
import * as url from '../node_modules/mochiurl/index.js';
import { require as mochiRequire } from '../node_modules/mochinode/index.js';
import { Buffer } from '../node_modules/mochibuffer/index.js';

let failures = 0;
function check(name, actual, expected) {
    const ok = String(actual) === String(expected);
    if (!ok) failures++;
    print(`${ok ? 'ok  ' : 'FAIL'} ${name}: ${actual}${ok ? '' : ` != ${expected}`}`);
}

const r = Readable.from([Buffer.from('a'), Buffer.from('b')]);
const collected = await collect(r);
check('stream.collect', Buffer.from(collected).toString(), 'ab');

const t = new Transform({ transform(chunk, _enc, cb) { cb(null, Buffer.from(Buffer.from(chunk).toString().toUpperCase())); } });
const out = [];
t.on('data', (chunk) => out.push(Buffer.from(chunk).toString()));
t.end('hello');
await new Promise((resolve) => t.on('finish', resolve));
check('stream.transform', out.join(''), 'HELLO');

check('zlib.native', isNativeReady(), true);
const compressed = deflateSync(Buffer.from('hello mochiberry'));
const inflated = inflateSync(compressed);
check('zlib.roundtrip', inflated.toString(), 'hello mochiberry');

check('os.platform', os.platform(), 'win32');
check('os.tmpdir.exists', os.tmpdir().length > 0, true);
check('url.fileURLToPath', url.fileURLToPath('file:///E:/pingbot/index.js'), 'E:\\pingbot\\index.js');

const streamMod = await mochiRequire('stream', 'E:/pingbot');
check('mochinode.resolve.stream', typeof streamMod.Readable, 'function');
const zlibMod = await mochiRequire('zlib', 'E:/pingbot');
check('mochinode.resolve.zlib', typeof zlibMod.inflateSync, 'function');

print(failures === 0 ? 'ALL_OK' : `FAILED=${failures}`);
