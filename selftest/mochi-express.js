/* selftest/mochi-express.js — enriched Express router (in-process, no network).
 *
 * The net serve loop is a blocking accept() loop, so we drive the router
 * directly: build an app, then call app(req, res) with mock requests and a
 * captured response writer. Covers: middleware chain, path params, JSON body,
 * query, redirect, static, 404. */
import { express } from '../node_modules/express/index.js';
import { makeResponse } from '../node_modules/net/lib/http.js';

let failures = 0;
let lastWrite = '';
function newRes() {
    lastWrite = '';
    return makeResponse((conn, data) => { lastWrite = data; }, 0);
}
function status(res) {
    return /^HTTP\/1\.1 (\d+)/.exec(lastWrite)?.[1] || '?';
}
function bodyOf(res) {
    const i = lastWrite.indexOf('\r\n\r\n');
    return i >= 0 ? lastWrite.slice(i + 4) : '';
}
function reqFor(method, url, opts = {}) {
    const qIdx = url.indexOf('?');
    const path = qIdx >= 0 ? url.slice(0, qIdx) : url;
    const query = Object.create(null);
    if (qIdx >= 0) {
        for (const pair of url.slice(qIdx + 1).split('&')) {
            const eq = pair.indexOf('=');
            const k = eq >= 0 ? pair.slice(0, eq) : pair;
            const v = eq >= 0 ? pair.slice(eq + 1) : '';
            query[k] = decodeURIComponent(v);
        }
    }
    return {
        method, url, path, query,
        headers: opts.headers || Object.create(null),
        body: opts.body || '',
    };
}

const app = express();

/* middleware: order + marker */
app.use((req, res, next) => { req.marker = 'mw1'; next(); });
app.use('/api', (req, res, next) => { req.marker += '+api'; next(); });
/* JSON body parser only for POST */
app.post('/api/items', express.json(), (req, res) => {
    res.status(201).json({ title: req.body.title, marker: req.marker });
});
/* path params + query */
app.get('/items/:id', (req, res) => {
    res.json({ id: req.params.id, q: req.query.q, marker: req.marker });
});
/* static under /static */
app.use('/static', express.static('selftest/fixtures/static'));
/* redirect */
app.get('/old', (req, res) => res.redirect('/new'));
/* fallthrough to 404 */

function check(name, cond) {
    if (cond) console.log('ok   ' + name);
    else { console.log('FAIL ' + name); failures++; }
}

/* 1. middleware chain + JSON body POST */
let res = newRes();
app(reqFor('POST', '/api/items', { headers: { 'content-type': 'application/json' }, body: '{"title":"hello"}' }), res);
check('post json body + middleware', status(res) === '201' && bodyOf(res).includes('"title":"hello"') && bodyOf(res).includes('"marker":"mw1+api"'));

/* 2. path params + query */
res = newRes();
app(reqFor('GET', '/items/42?q=berry'), res);
check('path params + query', status(res) === '200' && bodyOf(res).includes('"id":"42"') && bodyOf(res).includes('"q":"berry"'));

/* 3. static serving */
res = newRes();
app(reqFor('GET', '/static/index.html'), res);
check('static serves file', status(res) === '200' && bodyOf(res).length > 10 && lastWrite.includes('text/html'));

/* 4. static 404 -> next -> 404 */
res = newRes();
app(reqFor('GET', '/static/nope.html'), res);
check('static missing -> 404', status(res) === '404');

/* 5. redirect */
res = newRes();
app(reqFor('GET', '/old'), res);
check('redirect 302 + location', status(res) === '302' && lastWrite.includes('location: /new'));

/* 6. unknown route -> 404 */
res = newRes();
app(reqFor('GET', '/unknown/path'), res);
check('unknown route 404', status(res) === '404');

/* 7. urlencoded body */
const app2 = express();
app2.post('/form', express.urlencoded(), (req, res) => res.json(req.body));
res = newRes();
app2(reqFor('POST', '/form', { headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: 'a=1&b=two+words' }), res);
check('urlencoded body', bodyOf(res).includes('"a":"1"') && bodyOf(res).includes('"b":"two words"'));

console.log(failures === 0 ? '\nEXPRESS_ALL_OK' : '\nEXPRESS_FAILURES=' + failures);
