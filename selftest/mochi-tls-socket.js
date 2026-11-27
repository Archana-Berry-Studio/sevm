/* selftest/mochi-tls-socket.js — the public node:tls shim surface.
 * tls.connect() -> TLSSocket (real Schannel TLS, chain+hostname validated),
 * write() an HTTPS GET, collect 'data' events for a bounded window. */
import * as tls from '../node_modules/mochitls/index.js';
import { setTimeout as osSetTimeout } from 'mbjs:os';

let failures = 0;
function check(name, cond) { if (cond) return; failures++; console.log('FAIL ' + name); }

const endpoints = [
  { host: 'example.com', body: 'Example Domain' },
  { host: 'www.google.com', body: '<title>' },
];

let done = false;
for (const ep of endpoints) {
  if (done) break;
  const sock = tls.connect({ port: 443, host: ep.host });
  let err = null, dataBuf = '';
  let secure = false;

  sock.on('secureConnect', () => { secure = true; });
  sock.on('error', (e) => { err = e; });
  sock.on('data', (bytes) => {
    let s = '';
    for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
    dataBuf += s;
  });

  /* wait for secureConnect or error (max 15s) */
  for (let i = 0; i < 150 && !secure && !err; i++) await new Promise((r) => osSetTimeout(r, 100));
  if (err || !secure) {
    console.log('  connect ' + ep.host + ' failed: ' + (err ? err.message : 'timeout'));
    if (ep.host !== endpoints[endpoints.length - 1].host) continue;
    failures++;
    break;
  }
  done = true;

  check('secureConnect emitted', secure);
  check('socket authorized', sock.authorized === true);
  check('socket writable', sock.writable === true);

  const req = 'GET / HTTP/1.1\r\nHost: ' + ep.host + '\r\nConnection: close\r\nUser-Agent: mochitls/0.1\r\n\r\n';
  const wrote = sock.write(req);
  check('write accepted', wrote === true);

  /* collect data for a bounded window */
  for (let i = 0; i < 100 && dataBuf.length < 200; i++) await new Promise((r) => osSetTimeout(r, 100));
  console.log('  response head: ' + dataBuf.slice(0, 60).replace(/\s+/g, ' '));
  console.log('  response bytes: ' + dataBuf.length);
  check('HTTP 200', /HTTP\/1\.[01] 200/.test(dataBuf));
  check('body marker (' + ep.body + ')', dataBuf.includes(ep.body));

  sock.end();
}

console.log(failures === 0 ? '\nTLS_SOCKET_ALL_OK' : '\nTLS_SOCKET_FAILURES=' + failures);
if (failures) throw new Error('tls socket failures: ' + failures);
