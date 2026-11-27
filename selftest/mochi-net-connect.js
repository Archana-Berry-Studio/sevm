/* selftest/mochi-net-connect.js — net.connect over mochinode_net.tcpConnect.
 * Talks raw HTTP to the Python echo server (127.0.0.1:8099): write a GET,
 * collect 'data' events for a bounded window, verify the response. */
import { Socket } from '../node_modules/mochinet/index.js';
import { setTimeout as osSetTimeout } from 'mbjs:os';

let failures = 0;
function check(name, cond) { if (cond) return; failures++; console.log('FAIL ' + name); }

/* 1. connect succeeds + emits 'connect' */
const sock = new Socket();
let connected = false, errSeen = null, dataBuf = '';
sock.on('connect', () => { connected = true; });
sock.on('error', (e) => { errSeen = e; });
sock.on('data', (bytes) => {
    let s = '';
    for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
    dataBuf += s;
});

sock.connect(8099, '127.0.0.1');
check('connect emitted', connected);
check('no connect error', errSeen === null);
check('socket writable', sock.writable === true);

/* 2. write a raw HTTP GET */
const wrote = sock.write('GET / HTTP/1.1\r\nHost: 127.0.0.1:8099\r\nConnection: close\r\n\r\n');
check('write accepted', wrote === true);

/* 3. collect data for a bounded window, then check + close */
await new Promise((r) => osSetTimeout(r, 400));
console.log('  response head:', dataBuf.slice(0, 60).replace(/\s+/g, ' '));
check('got response', dataBuf.includes('get-ok') || dataBuf.includes('200 OK'));
check('got status line', /HTTP\/1\.[01] 200/.test(dataBuf));

sock.end();
console.log(failures === 0 ? '\nNET_CONNECT_ALL_OK' : '\nNET_CONNECT_FAILURES=' + failures);
if (failures) throw new Error('net-connect failures: ' + failures);
