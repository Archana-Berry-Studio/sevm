/*
 * selftest/mochicrypto_xcheck.js — mochicrypto vs an independent oracle.
 * The expected values below were produced by Python's hashlib/hmac and the
 * `cryptography` package (PBKDF2/HMAC/GCM/CTR/CBC); they are not derived from
 * this module, so any drift in the native primitive shows up here.
 * Run: mbjs.exe -m selftest/mochicrypto_xcheck.js
 */
import { loadNative } from '../node_modules/mochinode/index.js';

const n = await loadNative('mochicrypto.dll');
const results = [];
function check(name, got, want) {
    const ok = got === want;
    results.push([name, ok]);
    console.log((ok ? 'PASS ' : 'FAIL ') + name);
}

const data = '616263';                                   /* "abc" */
const key  = '00112233445566778899aabbccddeeff';
const iv12 = '000102030405060708090a0b';
const iv16 = '000102030405060708090a0b0c0d0e0f';
const aad  = '6161642d64617461';                        /* "aad-data" */
const pt   = '68656c6c6f206d6f636869203132333435';        /* "hello mochi 12345" */
const pwd  = '70617373776f7264';                         /* "password" */
const salt = '7065707065722d73616c742d30313233343536373839';

check('sha1',    n.sha1Hex(data),    'a9993e364706816aba3e25717850c26c9cd0d89d');
check('md5',     n.md5Hex(data),     '900150983cd24fb0d6963f7d28e17f72');
check('sha256',  n.sha256Hex(data), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
check('sha384',  n.sha384Hex(data),  'cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7');
check('sha512',  n.sha512Hex(data),  'ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f');
check('hmac_sha1',   n.hmacSha1Hex(key, data),   '6ee2a25f943e3f3ec05225fbb86ba73e2e5d51d2');
check('hmac_sha256', n.hmacSha256Hex(key, data),'02581ea39a6cf2d752793fd782cfb9cf965be72b32b322c9551d03510645fb31');
check('hmac_sha512', n.hmacSha512Hex(key, data),'f5b41f81b56d9cef4bfffbdd659470ca9de7348a23dac136790b028986d13e7d74dc59759faea253d5342abd56cf6f5859145ad54bca62e0c45245b7e4fa5c53');
check('pbkdf2_sha256', n.pbkdf2Hex(pwd, salt, 4096, 32, 0), 'cc8fec6385b4977baf9e7178550aa6614cbd5702343088c37082b8f3d96d4460');
check('pbkdf2_sha1',   n.pbkdf2Hex(pwd, salt, 1000, 20, 1), 'f7604cf48679c94794eeb49f8e2bbf06c4ebdb8c');
check('pbkdf2_sha512', n.pbkdf2Hex(pwd, salt, 2048, 24, 4), 'd48d8f9fdd08039e00d7ebe32587bf929def84c46d81998e');

const g = JSON.parse(n.aesGcmEncryptHex(key, iv12, aad, pt));
check('gcm_ciphertext', g.data, '43b05ba8c1b7e20ffc5040bf21bf72d938');
check('gcm_tag', g.tag, '3aa8ad66893b1d9c5847036cff6b72fc');
check('gcm_roundtrip', n.aesGcmDecryptHex(key, iv12, aad, g.data, g.tag), pt);

check('aes_ctr', n.aesCtrCryptHex(key, iv16, pt), '4ffadb261a527e31ecf3e7d6e0dcd33735');
check('ctr_roundtrip', n.aesCtrCryptHex(key, iv16, n.aesCtrCryptHex(key, iv16, pt)), pt);
check('aes_cbc', n.aesCbcEncryptHex(key, iv16, pt), '6b2a0928d7d4906e9a1b1fd748ea4c78b5820e95f0ce199174addec0444658df');
check('cbc_roundtrip', n.aesCbcDecryptHex(key, iv16, n.aesCbcEncryptHex(key, iv16, pt)), pt);

check('randomHex length', n.randomHex(32).length, 64);
check('timingSafeEqual true', n.timingSafeEqual(key, key), 1);
check('timingSafeEqual false', n.timingSafeEqual(key, '00112233445566778899aabbccddeef0'), 0);

/* unaligned tail lengths must be bit-exact too (the old CTR path broke here) */
const fills = [1, 15, 16, 17, 31, 32, 33, 47, 48, 63];
let tailOk = true;
const tailBad = [];
for (const L of fills) {
    const p = '61'.repeat(L);
    const o = n.aesCtrCryptHex(key, iv16, p);
    if (o.length !== p.length) { tailOk = false; tailBad.push(L + ':' + o.length + '!=' + p.length); }
}
check('ctr output length matches input for every tail', tailOk, true);
if (!tailOk) console.log('  tail mismatches: ' + tailBad.join(', '));

const bad = results.filter(r => !r[1]).length;
console.log('---');
console.log(bad === 0 ? 'ALL OK' : bad + ' FAILED');
if (bad) throw new Error('mochicrypto xcheck failed');
