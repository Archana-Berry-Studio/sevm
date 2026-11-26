/*
 * selftest/mochisandbox.js — JS-first path (node_modules/mochisandbox).
 * Run: mbjs.exe -m selftest/mochisandbox.js
 */
import { sandbox, count } from '../node_modules/mochisandbox/index.js';

const results = [];
function check(name, cond) {
    results.push([name, !!cond]);
    console.log((cond ? 'PASS ' : 'FAIL ') + name);
}

async function main() {
    /* realm isolation: no host globals inside */
    const sb = sandbox({ budgetMs: 5000 });
    check('host global hidden (process)', sb.run('typeof process') === '"undefined"');
    check('host global hidden (dynalib)', sb.run('typeof dynalib') === '"undefined"');

    /* arithmetic + JSON */
    check('arith', sb.run('1 + 2 * 3') === '7');
    check('object json', sb.run('({a:[1,2,3], b:"x"})') === '{"a":[1,2,3],"b":"x"}');
    check('eval() parses to host value', sb.eval('1 + 2') === 3);

    /* injected primitives */
    sb.setGlobal('greeting', 'halo');
    sb.setGlobal('n', 42);
    check('setGlobal primitive', sb.run('greeting + " " + n') === '"halo 42"');

    /* exceptions surface as host errors */
    let threw = false, msg = '';
    try { sb.run('throw new Error("boom")'); } catch (e) { threw = true; msg = String(e && e.message || e); }
    check('exception propagates', threw && msg.includes('boom'));

    /* instruction budget kills a runaway loop */
    const sb2 = sandbox({ budgetMs: 300 });
    let killed = false;
    try { sb2.run('for(;;){}'); } catch (e) { killed = true; }
    check('budget kills infinite loop', killed);
    sb2.free();

    /* strict mode blocks dynamic code */
    const sb3 = sandbox({ budgetMs: 5000, strict: true });
    let denied = false;
    try { sb3.run('eval("1+1")'); } catch (e) { denied = true; }
    check('strict denies eval', denied);
    denied = false;
    try { sb3.run('({}).constructor.constructor("return 2")()'); } catch (e) { denied = true; }
    check('strict denies constructor chain', denied);
    sb3.free();

    /* async execution: microtask queue drained, promise settled per realm */
    const sb4 = sandbox({ budgetMs: 5000 });
    check('async fn awaited', sb4.asyncEval(
        'async function f(){ return 40 + 2; } f()').value === 42);
    check('chained microtasks', sb4.asyncEval(
        'Promise.resolve(1).then(v => v * 2).then(v => v + 10)').state === 'fulfilled');
    check('chained microtasks value',
          sb4.asyncEval('Promise.resolve(1).then(v => v * 2).then(v => v + 10)').value === 12);
    check('50 microtask ticks', sb4.asyncEval(
        'let p = Promise.resolve(0); for (let i = 0; i < 50; i++) p = p.then(v => v + 1); p'
    ).value === 50);
    check('await inside async iife', sb4.asyncEval(
        '(async () => { const a = await Promise.resolve(21); return a * 2; })()').value === 42);
    check('non-promise value passes through', sb4.asyncEval('1 + 1').value === 2);
    let rejected = false;
    try { sb4.asyncEval('Promise.reject(new Error("boom"))'); } catch (e) { rejected = true; }
    check('rejected promise throws', rejected);
    sb4.free();

    /* free + lifecycle */
    check('count reports live sandboxes', typeof count() === 'number');
    sb.free();
    let usedAfterFree = false;
    try { sb.run('1'); } catch (e) { usedAfterFree = true; }
    check('use after free throws', usedAfterFree);

    const fails = results.filter(r => !r[1]).length;
    console.log('---');
    if (fails === 0) { console.log('ALL OK'); return; }
    throw new Error('FAILURES: ' + fails);
}

main().catch(e => { console.log('FATAL: ' + (e && e.message || e)); throw e; });
