// mochi-http-post.js — verify http.request POST/PUT with a body reaches the
// server and the response comes back, via the mochiws.httpRequest native.
console.log('START');
const http = await import('../node_modules/mochihttp/index.js');
console.log('IMPORTED', typeof http.request, typeof http.ClientRequest);

async function collect(res) {
    let s = '';
    for await (const chunk of res) {
        const u = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
        for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]);
    }
    return s;
}

function post(method, path, payload) {
    return new Promise((resolve, reject) => {
        const req = http.request(`http://127.0.0.1:8099${path}`, { method, headers: { 'Content-Type': 'application/json' } }, (res) => {
            collect(res).then((text) => resolve({ status: res.statusCode, text }));
        });
        req.on('error', reject);
        req.end(payload);
    });
}

(async () => {
    let ok = 0, total = 0;
    const check = (name, cond) => { total++; if (cond) ok++; else console.log('FAIL ' + name); };

    // POST with JSON body
    const r1 = await post('POST', '/echo', JSON.stringify({ hello: 'berry' }));
    check('POST status 201', r1.status === 201);
    check('POST body echoed', r1.text.includes('{"echo":') && r1.text.includes('hello'));
    console.log('  POST ->', r1.status, r1.text.slice(0, 60));

    // PUT with body
    const r2 = await post('PUT', '/echo', 'put-payload');
    check('PUT status 201', r2.status === 201);
    check('PUT body echoed', r2.text.includes('put-payload'));
    console.log('  PUT ->', r2.status, r2.text.slice(0, 60));

    // PATCH with body
    const r3 = await post('PATCH', '/echo', 'patch-payload');
    check('PATCH status 201', r3.status === 201);
    check('PATCH body echoed', r3.text.includes('patch-payload'));
    console.log('  PATCH ->', r3.status, r3.text.slice(0, 60));

    console.log(`HTTP_REQUEST ${ok}/${total} ${ok === total ? 'ALL_OK' : 'FAIL'}`);
    if (ok !== total) throw new Error('http request failures');
})();
