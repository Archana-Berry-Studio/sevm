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

/* does f() throw with 'msg' in the message? */
function denies(msg, f) {
    try { f(); return false; }
    catch (e) { return String((e && e.message) ? e.message : e).indexOf(msg) >= 0; }
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

    /* security audit vectors — strict mode must deny every code compiler */
    const sb5 = sandbox({ strict: true, budgetMs: 3000 });
    check('audit: strict denies GeneratorFunction ctor', denies('dynamic code disabled', () => sb5.eval(
        "Object.getPrototypeOf(function*(){}).constructor('return 42')().next().value")));
    check('audit: strict denies AsyncFunction ctor', denies('dynamic code disabled', () => sb5.eval(
        "Object.getPrototypeOf(async function(){}).constructor('return 42')()")));
    check('audit: strict denies AsyncGeneratorFunction ctor', denies('dynamic code disabled', () => sb5.eval(
        "Object.getPrototypeOf(async function*(){}).constructor('return 1')()")));
    let impBlocked = false;
    try { sb5.asyncEval("import('fs')"); } catch (e) { impBlocked = true; }
    check('audit: dynamic import() blocked in realm', impBlocked);
    check('audit: Atomics.wait cannot block host', denies('cannot block', () => sb5.eval(
        "Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0)")));
    check('audit: realm has no console', sb5.eval('typeof console') === 'undefined');
    check('audit: realm has no require', sb5.eval('typeof require') === 'undefined');
    check('audit: realm has no setTimeout', sb5.eval('typeof setTimeout') === 'undefined');
    check('audit: realm has no WebAssembly', sb5.eval('typeof WebAssembly') === 'undefined');
    check('audit: realm global surface is bounded',
          sb5.eval('Object.getOwnPropertyNames(globalThis).length') < 100);
    sb5.free();

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


    /* clone: JSON deep-copy host value into the realm (globalThis.__clone__) */
    const sb6 = sandbox({ budgetMs: 5000 });
    const srcObj = { a: 1, b: [1, 2, 3], s: 'x', nested: { deep: [10, 20] } };
    const cpObj = sb6.clone(srcObj);
    check('clone returns deep copy', JSON.stringify(cpObj) === JSON.stringify(srcObj));
    check('clone realm __clone__ matches', sb6.eval('JSON.stringify(__clone__)') === JSON.stringify(srcObj));
    srcObj.b[1] = 999;                     /* host mutation must NOT touch realm copy */
    check('clone is deep (host mutation isolated)', sb6.eval('__clone__.b[1]') === 2);
    check('clone nested path in realm', sb6.clone({ nested: { deep: [10, 20] } }).nested.deep[1] === 20);
    check('clone realm __clone__ holds nested', sb6.eval('__clone__.nested.deep[1]') === 20);
    check('clone string', sb6.clone('deep') === 'deep');
    check('clone null', sb6.clone(null) === null);
    let cloneCyclic = false;
    const cyc = {}; cyc.self = cyc;
    try { sb6.clone(cyc); } catch (e) { cloneCyclic = true; }   /* JSON.stringify throws host-side */
    check('clone cyclic host value throws (no hang)', cloneCyclic);
    sb6.free();

    /* audit: every native intrinsic's .constructor chain is neutered */
    const sb7 = sandbox({ budgetMs: 5000, strict: true });
    const ctorChain = [
        'RegExp.constructor("return 40+2")()',
        '/x/.constructor.constructor("return 40+2")()',
        'Object.constructor("return 40+2")()',
        'Array.constructor("return 40+2")()',
        'Date.constructor("return 40+2")()',
        'JSON.constructor("return 40+2")()',
        'Promise.constructor("return 40+2")()',
        'Error.constructor("return 40+2")()',
        'String.constructor("return 40+2")()',
        'Number.constructor("return 40+2")()',
        'Boolean.constructor("return 40+2")()',
        'ArrayBuffer.constructor("return 40+2")()',
        'Object.getPrototypeOf({}).constructor("return 40+2")()',
        'new Proxy({},{get:(t,k)=>Function(k)}).x',
        '(Object.defineProperty(Object,"constructor",{value:Function}), Object.constructor("return 1")())',
    ];
    let allDenied = true;
    for (const expr of ctorChain) {
        if (!denies('mochisandbox: dynamic code disabled', () => sb7.eval(expr))) { allDenied = false; console.log('  NOT DENIED: ' + expr); }
    }
    check('audit: all native .constructor chains + Proxy->Function denied', allDenied);
    check('audit: RegExp still works (non-code)', sb7.eval('new RegExp("a.c").test("abc")') === true);
    check('audit: RegExp is not a Function compiler', sb7.eval('typeof Object.getPrototypeOf(/x/).constructor("return 1")') === 'object');
    check('audit: Error.prepareStackTrace stays realm-local', sb7.eval('(Error.prepareStackTrace=(e,s)=>42, new Error("x").stack)') === 42);
    check('audit: Symbol.toPrimitive coercion ok', sb7.eval('({[Symbol.toPrimitive]:()=>7}*1)') === 7);
    check('audit: Proxy get trap ok', sb7.eval('new Proxy({a:1},{get:(t,k)=>k==="a"?42:t[k]}).a') === 42);
    check('audit: BigInt computes in-realm (non-JSON result)', sb7.eval('String(BigInt(2)+BigInt(3))') === '5');
    check('audit: Symbol present', sb7.eval('typeof Symbol.iterator') === 'symbol');
    check('audit: ArrayBuffer + view ok', sb7.eval('new Uint8Array(new ArrayBuffer(4)).length') === 4);
    check('audit: dynalib absent', sb7.eval('typeof dynalib') === 'undefined');
    check('audit: WebAssembly absent', sb7.eval('typeof WebAssembly') === 'undefined');
    sb7.free();

    /* audit: intrinsics are isolated PER SANDBOX (not just globals) */
    const sb9 = sandbox({ budgetMs: 5000, strict: true });
    const sb10 = sandbox({ budgetMs: 5000, strict: true });
    sb9.eval('(Object.defineProperty(Array.prototype, "0", {value: 99, writable: false, configurable: true}), 1)');
    check('audit: poisoned prototype applies within the sandbox',
          denies('read-only', () => sb9.eval('new Array(1,2)[0]')));
    check('audit: fresh sandbox is NOT affected by another sandbox prototype',
          sb10.eval('[1,2,3].join(",")') === '1,2,3');
    sb9.free(); sb10.free();

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
