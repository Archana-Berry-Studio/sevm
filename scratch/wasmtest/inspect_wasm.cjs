const fs = require('fs');
for (const f of ['test.wasm','wrb_main.wasm']) {
  const b = fs.readFileSync(f);
  console.log('==', f, 'size', b.length, 'magic', b.slice(0,4).toString('hex'));
  try {
    const m = new WebAssembly.Module(b);
    console.log('  imports', JSON.stringify(WebAssembly.Module.imports(m)).slice(0, 800));
    console.log('  exports', JSON.stringify(WebAssembly.Module.exports(m)).slice(0, 800));
  } catch (e) { console.log('  ERR', e.message); }
}