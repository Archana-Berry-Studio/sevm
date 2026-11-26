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
    const handle = ws.connect('wss://echo.websocket.events');
    let opened = false;
    let echoed = false;
    const deadline = Date.now() + 10000;
    ws.send(handle, Buffer.from('hello'), true);
    while (Date.now() < deadline && !echoed) {
        const events = ws.poll(handle);
        for (const ev of events) {
            if (ev.type === 'open') opened = true;
            if ((ev.type === 'message' || ev.type === 'text') && ev.data) {
                const text = Buffer.from(ev.data, 'hex').toString();
                if (text.includes('hello')) echoed = true;
            }
            if (ev.type === 'error') print(`ws.error: ${ev.error}`);
        }
        await new Promise((r) => setTimeout(r, 100));
    }
    check('mochiws.open', opened, true);
    check('mochiws.echo', echoed, true);
    try { ws.close(handle); } catch {}
}

print(failures === 0 ? 'ALL_OK' : `FAILED=${failures}`);
