/* selftest/mochi-tls.js — real HTTPS request over the Schannel mochitls module.
 *
 * Exercises the full stack: JS shim -> native mochitls_x86_64.dll -> Winsock2
 * connect -> Schannel handshake (default credentials = full certificate chain
 * validation against the system trust store + hostname validation) ->
 * EncryptMessage/DecryptMessage -> HTTP GET against a public endpoint.
 *
 * Prints exactly TLS_ALL_OK when the handshake AND the HTTPS response are both
 * verified. Falls back to https://www.google.com if example.com is unreachable.
 *
 * Run from E:/sevm:
 *   E:/bcf/mochiberry/build_manual/mbjs.exe -m selftest/mochi-tls.js
 */
import * as tls from '../mochi_modules/mochitls/script/index.js';

let failures = 0;
function check(name, cond) {
  if (cond) console.log('ok   ' + name);
  else { console.log('FAIL ' + name); failures++; }
}

/* 1. native export surface (loaded via the shim) */
const ns = await tls.init();
const exps = Object.keys(ns).sort();
check('7 tls exports', exps.length === 7);
for (const e of ['tlsConnect', 'tlsWrite', 'tlsRead', 'tlsClose',
                 'tlsShutdown', 'tlsLastError', 'tlsVersion'])
  check('has ' + e, typeof ns[e] === 'function');

/* 2. pick an endpoint: example.com first, fall back to www.google.com */
const endpoints = [
  { host: 'example.com', body: 'Example Domain', status: /HTTP\/1\.[01] 200/ },
  { host: 'www.google.com', body: '<title>', status: /HTTP\/1\.[01] 200/ },
];

let done = false;
for (const ep of endpoints) {
  if (done) break;
  const handle = await tls.connect(ep.host, 443);
  if (handle <= 0) {
    console.log('  connect ' + ep.host + ' failed: ' + await tls.lastError());
    if (ep.host !== endpoints[endpoints.length - 1].host) continue;
    failures++;
    break;
  }
  done = true;

  const ver = await tls.version(handle);
  console.log('  connected ' + ep.host + ':443  negotiated=' + ver);
  check('negotiated TLS 1.2 or 1.3',
        ver === 'TLS 1.2' || ver === 'TLS 1.3');

  /* 3. send the request */
  const req = 'GET / HTTP/1.1\r\nHost: ' + ep.host + '\r\n' +
              'Connection: close\r\nUser-Agent: mochitls/0.1\r\n\r\n';
  const n = await tls.write(handle, req);
  check('tlsWrite sent full request (' + req.length + ' bytes)', n === req.length);

  /* 4. read the response until EOF / end-of-document */
  let resp = '';
  let reads = 0;
  while (reads < 256) {
    const chunk = await tls.read(handle, 65536);
    if (chunk === undefined) break;
    resp += chunk;
    reads++;
    if (resp.includes('</html>') || resp.includes('</HTML>')) break;
  }
  console.log('  response head: ' + resp.slice(0, 72).replace(/\s+/g, ' '));
  console.log('  response bytes: ' + resp.length);
  check('HTTP status line 200', ep.status.test(resp));
  check('body marker present (' + ep.body + ')', resp.includes(ep.body));

  await tls.close(handle);
}

console.log(failures === 0 ? '\nTLS_ALL_OK' : '\nTLS_FAILURES=' + failures);
if (failures) throw new Error('mochi-tls failures: ' + failures);
