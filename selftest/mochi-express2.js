/* selftest/mochi-express2.js — Express v2 features: template engine
 * (app.engine/app.set('view engine')/res.render + built-in mini-EJS), vhost,
 * app.param, error middleware. In-process (mock req/res), no network. */
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
function check(name, cond) {
    if (cond) return;
    failures++;
    console.log('FAIL ' + name + ' | status=' + status() + ' body=' + bodyOf().slice(0, 80));
}

/* 1. template engine + res.render (mini-EJS interpolation + control flow) */
const app = express();
app.set('views', 'selftest/fixtures/views');
app.set('view engine', 'ejs');
app.engine('ejs', express.defaultEngine);
app.get('/page', (req, res) => res.render('page', { name: 'Berry', items: ['a', 'b', 'c'] }));

let res = newRes();
app(reqFor('GET', '/page'), res);
const b1 = bodyOf();
check('render interpolated', b1.includes('Hello Berry!'));
check('render control flow', b1.includes('<li>a</li>') && b1.includes('<li>c</li>'));
check('render escaped', b1.includes('Hello Berry!') && !b1.includes('<h1>Hello &lt;'));
console.log('  render ->', b1.replace(/\s+/g, ' ').slice(0, 90));

/* 2. vhost: sub-app served on matching Host, next() on non-match */
const api = express();
api.get('/', (req, res) => res.json({ vhost: 'api' }));
const app2 = express();
app2.use(express.vhost('*.example.com', api));
app2.get('/', (req, res) => res.json({ vhost: 'root' }));

res = newRes();
app2(reqFor('GET', '/', { headers: { host: 'api.example.com' } }), res);
check('vhost matched', bodyOf().includes('"vhost":"api"'));
res = newRes();
app2(reqFor('GET', '/', { headers: { host: 'other.org' } }), res);
check('vhost non-match falls through', bodyOf().includes('"vhost":"root"'));

/* 3. app.param: runs before the route handler with the captured value */
const app3 = express();
app3.param('id', (req, res, next, val, name) => { req.paramValue = name + '=' + val; next(); });
app3.get('/u/:id', (req, res) => res.json({ id: req.params.id, pv: req.paramValue }));

res = newRes();
app3(reqFor('GET', '/u/42'), res);
check('app.param ran', bodyOf().includes('"id":"42"') && bodyOf().includes('"pv":"id=42"'));

/* 4. error middleware (arity 4): registered AFTER the route, catches thrown error */
const app4 = express();
app4.get('/boom', () => { throw new Error('kaboom'); });
app4.use((err, req, res, next) => res.status(500).json({ caught: err.message }));

res = newRes();
app4(reqFor('GET', '/boom'), res);
check('error middleware caught', status() === '500' && bodyOf().includes('"caught":"kaboom"'));

/* 4b. no error middleware -> default 500 */
const app5 = express();
app5.get('/boom', () => { throw new Error('kaboom2'); });
res = newRes();
app5(reqFor('GET', '/boom'), res);
check('default 500 on throw', status() === '500');

/* 4c. app.set/get setting round-trip */
const app6 = express();
app6.set('x-powered', true);
check('app.get setting', app6.get('x-powered') === true);

console.log(failures === 0 ? '\nEXPRESS_V2_ALL_OK' : '\nEXPRESS_V2_FAILURES=' + failures);
if (failures) throw new Error('express v2 failures: ' + failures);
