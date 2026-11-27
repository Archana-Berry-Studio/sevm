const I32=0x7f;
function leb(n){const o=[];do{let b=n&0x7f;n>>>=7;if(n)b|=0x80;o.push(b);}while(n);return o;}
function sec(id,p){return [id,...leb(p.length),...p];}
function nm(s){return [...leb(s.length),...[...s].map(c=>c.charCodeAt(0))];}

// ---- Variant A: minimal, no imports ----
// types: 0 = ()->() ; func add() = 42
const A = [
  0,0x61,0x73,0x6d,1,0,0,0,
  ...sec(1,[1,0x60,0,0]),                       // 1 type: ()->()
  ...sec(3,[1,0]),                              // 1 func, type 0
  ...sec(7,[1,...nm('f'),0,0]),                // export f = func 0
  ...sec(10,[1, 0, ...leb(3), 0x41,0x2a,0x0b]), // body: i32.const 42; end
];
console.log('A validate', WebAssembly.validate(Uint8Array.from(A)), 'len', A.length);
console.log('A hex', Buffer.from(A).toString('hex'));
try {
  const m = new WebAssembly.Module(Uint8Array.from(A));
  const i = new WebAssembly.Instance(m);
  console.log('A f() =', i.exports.f());
} catch (e) { console.log('A ERR', e.message); }

// ---- Variant B: (i32,i32)->i32 add, no imports ----
const typesB=[2, 0x60,2,I32,I32,1,I32];
const B = [
  0,0x61,0x73,0x6d,1,0,0,0,
  ...sec(1,typesB),
  ...sec(3,[1,0]),
  ...sec(7,[1,...nm('add'),0,0]),
  ...sec(10,[1, 0, ...leb(7), 0x20,0,0x20,1,0x6a,0x0b]), // local.get 0; local.get 1; i32.add; end
];
console.log('B validate', WebAssembly.validate(Uint8Array.from(B)), 'len', B.length);
console.log('B hex', Buffer.from(B).toString('hex'));
try {
  const m = new WebAssembly.Module(Uint8Array.from(B));
  const i = new WebAssembly.Instance(m);
  console.log('B add(2,40) =', i.exports.add(2,40));
} catch (e) { console.log('B ERR', e.message); }