// Can the engine run a CJS-style wrapper via new Function?
try {
  const wrapper = new Function('module', 'exports', 'require', '__dirname', '__filename',
    'module.exports = { name: "cjs-ok", add: (a,b)=>a+b, dir: __dirname };');
  const module = { exports: {} };
  const exports = module.exports;
  wrapper(module, exports, ()=>null, '/tmp/cjs', '/tmp/cjs/index.js');
  console.log('CJS_WRAPPER=' + JSON.stringify(module.exports));
} catch (e) {
  console.log('CJS_WRAPPER_FAIL: ' + e.message);
}
// eval support
try { const v = eval('1+2'); console.log('EVAL=' + v); } catch (e) { console.log('EVAL_FAIL: ' + e.message); }
// async function wrapper
try {
  const aw = new Function('module', 'exports', 'return (async () => { module.exports = await Promise.resolve(7); return module.exports; })();');
  const m = { exports: {} };
  const p = aw(m, m.exports);
  p.then(v => console.log('ASYNC_WRAP=' + v)).catch(e => console.log('ASYNC_WRAP_FAIL: ' + e.message));
} catch (e) { console.log('ASYNC_WRAP_FAIL2: ' + e.message); }
