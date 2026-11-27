import '../node_modules/mochiglobals/index.js';
import { EventEmitter } from '../mochievents/index.js';
import * as ws from '../mochiws/index.js';
import { MochiBuffer as Buffer } from '../mochibuffer/index.js';

let failures = 0;
function check(name, actual, expected) {
    const ok = String(actual) === String(expected);
    if (!ok) failures++;
    print(`${ok ? 'ok  ' : 'FAIL'} ${name}: ${actual}${ok ? '' : ` != ${expected}`}`);
}

check('mochiws.native', ws.isNativeReady(), true);
if (ws.isNativeReady()) {
    /* Public contract: ws.connect() returns a WebSocket (EventEmitter) whose
     * internal poll loop emits 'open'/'message'/'close'/'error'. The old
     * selftest called ws.send/poll with a raw handle, but connect() now
     * returns a WebSocket instance — poll(String(instance)) yields
     * '[object Object]' and the native module answers "mochiws: bad handle".
     * That is a legacy raw-handle path; the class API is the one to test.
     * echo.websocket.events was unreachable from this host (curl 000);
     * echo.websocket.org answers 101 Switching Protocols — use it. */
    const sock = ws.connect('wss://echo.websocket.org');
    let opened = false;
    let echoed = false;
    sock.on('open', () => {
        opened = true;
        sock.send(Buffer.from('hello'));
    });
    sock.on('message', (data) => {
        /* data may be a MochiBuffer (binary echo) or a string (text frame). */
        const bytes = (data && data.data !== undefined) ? data.data : data;
        let text = String(bytes);
        if (Array.isArray(bytes)) {
            text = String.fromCharCode(...bytes);
        } else if (bytes && typeof bytes.toString === 'function') {
            const s = bytes.toString();
            if (/^[0-9,]+$/.test(s)) text = String.fromCharCode(...s.split(',').map(Number));
        }
        if (text.includes('hello')) echoed = true;
    });
    sock.on('error', (e) => print('ws.error: ' + JSON.stringify(e).slice(0, 140)));
    const deadline = Date.now() + 12000;
    while (Date.now() < deadline && !echoed) await new Promise((r) => setTimeout(r, 100));
    check('mochiws.open', opened, true);
    check('mochiws.echo', echoed, true);
    try { sock.close(); } catch {}
}

print(failures === 0 ? 'ALL_OK' : `FAILED=${failures}`);
