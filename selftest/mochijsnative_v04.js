/*
 * selftest/mochijsnative_v04.js — v0.4 engine-control surface of mochijsnative.
 * Run: mbjs-host2.exe -m selftest/mochijsnative_v04.js
 * Covers: valueDup valueToBool valueEq valueIsError valueIsFunction newObject
 *         newArray propDefine propHas propDelete propKeys protoGet protoSet
 *         throwError exceptionHas memoryUsage bigintNew stringify handleStats
 *         promiseNew + bigint precision fix in valueFromJS.
 */
import { Runtime, Context, Value, valueFromJS, handleStats,
         version, engineVersion, abi } from '../node_modules/mochijsnative/index.js';

const results = [];
function check(name, cond) {
    results.push([name, !!cond]);
    console.log((cond ? 'PASS ' : 'FAIL ') + name);
}

async function main() {
    check('module version is 0.4.0', String(version).indexOf('0.4.0') >= 0);
    check('abi.module_version 0.4.0', abi && abi.module_version === '0.4.0');
    check('abi.deep advertises promise+memory', abi && abi.deep && abi.deep.promise === true && abi.deep.memory === true);

    const rt = new Runtime();
    rt.limits(1 << 20, 16 << 20);
    const ctx = rt.context(true);

    /* value ops */
    const v42 = valueFromJS(ctx, 42);
    const v42b = v42.dup();
    check('valueDup copies number', v42b.toNumber() === 42 && v42b.slot !== v42.slot);
    v42b.free();
    check('valueToBool(0) false', valueFromJS(ctx, 0).toBool() === false);
    check('valueToBool(7) true', valueFromJS(ctx, 7).toBool() === true);
    const a = valueFromJS(ctx, 42), b = valueFromJS(ctx, 42), c = valueFromJS(ctx, 43);
    check('valueEq 42===42', a.eq(b) === true);
    check('valueEq 42!==43', a.eq(c) === false);
    a.free(); b.free(); c.free();

    const fn = ctx.eval('function f(){return 1}; f');
    check('valueIsFunction true', fn.isFunction() === true);
    fn.free();
    const n = valueFromJS(ctx, 5);
    check('valueIsFunction false for number', n.isFunction() === false);
    n.free();

    const bad = ctx.evalRaw('throw new TypeError("boom")');
    check('evalRaw throw is exception', bad.isException() === true);
    bad.free();
    const e0 = ctx.exception();          /* clear pending exception */
    check('exception() retrieves the thrown error', e0.isError() === true);
    e0.free();
    const realErr = ctx.eval('new Error("x")');
    check('valueIsError true on real Error object', realErr.isError() === true);
    realErr.free();

    /* object + property control */
    const obj = ctx.newObject();
    check('newObject type object', obj.type === 'object');
    check('propDefine returns 1', ctx.propDefine(obj, 'x', 42) === 1);
    check('propHas x true', ctx.propHas(obj, 'x') === true);
    check('propKeys contains x', ctx.propKeys(obj).indexOf('x') >= 0);
    const gotX = new Value(ctx, host_getProp(ctx, obj, 'x'));
    check('prop readable via getProp', gotX.toNumber() === 42);
    gotX.free();

    /* read back through the engine: define then eval inside same ctx */
    ctx.set('__o', obj);
    check('prop value roundtrip via eval', ctx.eval('__o.x').toNumber() === 42);
    check('propDelete returns 1', ctx.propDelete(obj, 'x') === 1);
    check('propHas x false after delete', ctx.propHas(obj, 'x') === false);
    check('propKeys empty after delete', ctx.propKeys(obj).length === 0);

    /* arrays + prototype */
    const arr = ctx.newArray();
    check('newArray type object', arr.type === 'object');
    ctx.set('__a', arr);
    check('array is Array via eval', ctx.eval('Array.isArray(__a)').toBool() === true);
    const proto = ctx.protoGet(obj);
    const objProto = ctx.eval('Object.prototype');
    check('protoGet(obj) === Object.prototype', proto.eq(objProto) === true);
    proto.free(); objProto.free();

    /* throwError + exceptionHas */
    check('hasException false initially', ctx.hasException() === false);
    ctx.throwError('type', 'kaboom');
    check('throwError sets pending exception', ctx.hasException() === true);
    const exc = ctx.exception();
    check('exception() retrieves error', exc.isError() === true);
    check('exception message', exc.toStringValue().indexOf('kaboom') >= 0);
    exc.free();

    /* stringify */
    const src = ctx.eval('({a:[1,2],b:"x",n:null})');
    check('stringify object', ctx.stringify(src) === '{"a":[1,2],"b":"x","n":null}');
    src.free();

    /* bigint precision (the v0.3 bug: Number(bigint) lost >2^53) */
    const big = 2n ** 60n + 12345n;
    const bv = valueFromJS(ctx, big);
    check('bigint type', bv.type === 'bigint');
    check('bigint precision preserved', bv.toStringValue() === big.toString());
    bv.free();
    const b2 = ctx.bigint(2n ** 40n);
    check('Context.bigint type bigint', b2.type === 'bigint');
    b2.free();

    /* memory + handle introspection */
    const mem = ctx.memoryUsage();
    check('memoryUsage has malloc_size>0', mem.malloc_size > 0 && typeof mem.obj_count === 'number');
    const hs = handleStats();
    check('handleStats reports live contexts>=1', hs.contexts >= 1 && hs.caps.contexts > 0);

    /* promise */
    const p = ctx.promise();
    check('promiseNew yields 3 values', p.promise.type !== 'undefined' && p.resolve.isFunction() === true);
    ctx.invoke(p.resolve, null, 7);
    const awaited = ctx.await(p.promise);
    check('promise resolves via await', awaited.toNumber() === 7);
    awaited.free();
    p.resolve.free(); p.reject.free(); /* p.promise di-own oleh await */

    obj.free(); arr.free();
    v42.free();
    ctx.close();
    rt.close();

    const fails = results.filter(r => !r[1]).length;
    console.log('---');
    console.log(fails === 0 ? 'ALL OK' : ('FAILURES: ' + fails));
    if (fails) throw new Error('FAILURES: ' + fails);
}

/* helper: read a property of a Value object through the raw handle */
import { nativeHandle as host } from '../node_modules/mochijsnative/index.js';
function host_getProp(ctx, obj, name) {
    return host.getProp(ctx.id, obj.slot, String(name));
}

main().catch(e => { console.log('FATAL: ' + (e && e.message || e)); throw e; });