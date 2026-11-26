import { EventEmitter } from '../node_modules/mochievents/index.js';
import * as mochipath from '../node_modules/mochipath/index.js';
import { inspect, format } from '../node_modules/mochiutil/index.js';
import { Buffer } from '../node_modules/mochibuffer/index.js';
import {
    randomBytes,
    createHash,
    createHmac,
    pbkdf2Sync,
    timingSafeEqual,
    createCipheriv,
    createDecipheriv,
    isNativeReady,
    nativeError,
} from '../node_modules/mochicrypto/index.js';

let failures = 0;

function check(name, actual, expected) {
    const ok = String(actual) === String(expected);
    if (!ok) failures++;
    print(`${ok ? 'ok  ' : 'FAIL'} ${name}: ${actual}${ok ? '' : ` != ${expected}`}`);
}

const emitter = new EventEmitter();
let hits = 0;
emitter.once('ping', (value) => { hits += value; });
emitter.emit('ping', 2);
emitter.emit('ping', 3);
check('events.once', hits, 2);

check('path.join', mochipath.join('E:/a', 'b', '../c'), 'E:/a/c');
check('path.basename', mochipath.basename('E:/a/b.txt'), 'b.txt');
check('util.format', format('%s=%d', 'x', 7), 'x=7');
check('util.inspect', inspect({ a: 1 }), '{ a: 1 }');

check('buffer.utf8-hex', Buffer.from('abc').toString('hex'), '616263');
check('buffer.hex-utf8', Buffer.from('616263', 'hex').toString(), 'abc');
check('buffer.base64', Buffer.from('abc').toString('base64'), 'YWJj');
check('buffer.base64url', Buffer.from('abc').toString('base64url'), 'YWJj');
check('buffer.u32be', (() => { const b = Buffer.alloc(4); b.writeUInt32BE(0x01020304); return b.toString('hex'); })(), '01020304');

check('crypto.native', isNativeReady(), true);
if (!isNativeReady()) print(`crypto.error: ${nativeError() && nativeError().message}`);

check('crypto.sha256', createHash('sha256').update('abc').digest('hex'), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
check('crypto.md5', createHash('md5').update('abc').digest('hex'), '900150983cd24fb0d6963f7d28e17f72');
check('crypto.hmac', createHmac('sha256', 'key').update('The quick brown fox jumps over the lazy dog').digest('hex'), 'f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8');
check('crypto.pbkdf2', pbkdf2Sync('password', 'salt', 1, 20, 'sha1').toString('hex'), '0c60c80f961f0e71f3a9b524af6012062fe037a6');
check('crypto.random', randomBytes(16).length, 16);
check('crypto.timingSafeEqual', timingSafeEqual(Buffer.from('abc'), Buffer.from('abc')), true);

const key = createHash('sha256').update('mochi-key').digest();
const iv = Buffer.from('0102030405060708090a0b0c', 'hex');
const iv16 = Buffer.from('0102030405060708090a0b0c0d0e0f10', 'hex');
const aad = Buffer.from('mochi-aad');
const plain = Buffer.from('halo mochiberry');
const gcmCipher = createCipheriv('aes-256-gcm', key, iv);
gcmCipher.setAAD(aad);
const gcmBody = Buffer.concat([gcmCipher.update(plain), gcmCipher.final()]);
const gcmTag = gcmCipher.getAuthTag();
const gcmPacked = Buffer.concat([gcmBody, gcmTag]);
const gcmDecipher = createDecipheriv('aes-256-gcm', key, iv);
gcmDecipher.setAAD(aad);
gcmDecipher.setAuthTag(gcmTag);
const gcmOut = Buffer.concat([gcmDecipher.update(gcmBody), gcmDecipher.final()]);
check('crypto.aes-256-gcm', gcmOut.toString(), plain.toString());
check('crypto.gcm-tag-length', gcmTag.length, 16);

const ctrCipher = createCipheriv('aes-256-ctr', key, iv16);
const ctrBody = Buffer.concat([ctrCipher.update(plain), ctrCipher.final()]);
const ctrDecipher = createDecipheriv('aes-256-ctr', key, iv16);
const ctrOut = Buffer.concat([ctrDecipher.update(ctrBody), ctrDecipher.final()]);
check('crypto.aes-256-ctr', ctrOut.toString(), plain.toString());

const cbcCipher = createCipheriv('aes-256-cbc', key, iv16);
const cbcBody = Buffer.concat([cbcCipher.update(plain), cbcCipher.final()]);
const cbcDecipher = createDecipheriv('aes-256-cbc', key, iv16);
const cbcOut = Buffer.concat([cbcDecipher.update(cbcBody), cbcDecipher.final()]);
check('crypto.aes-256-cbc', cbcOut.toString(), plain.toString());

print(failures === 0 ? 'ALL_OK' : `FAILED=${failures}`);
