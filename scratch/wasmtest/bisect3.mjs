function leb(n){const o=[];do{let b=n&0x7f;n>>>=7;if(n)b|=0x80;o.push(b);}while(n);return o;}
function sec(id,p){return [id,...leb(p.length),...p];}
function nm(s){return [...leb(s.length),...[...s].map(c=>c.charCodeAt(0))];}
const I32=0x7f;
function hex(a){return Buffer.from(a).toString('hex');}
function tryIt(name, arr){
  const u = Uint8Array.from(arr.flat(9));
  const ok = WebAssembly.validate(u);
  console.log(name.padEnd(22), 'len='+String(u.byteLength).padStart(3), 'validate='+ok);
  if (!ok) {
    console.log('   '+hex(u));
    try { new WebAssembly.Module(u); } catch(e){ console.log('   ERR '+e.message); }
  }
  return u;
}

// 1. empty module
tryIt('empty', [0,0x61,0x73,0x6d,1,0,0,0]);
// 2. + type ()->i32
tryIt('type_only', [0,0x61,0x73,0x6d,1,0,0,0, ...sec(1,[1,0x60,0,1,I32])]);
// 3. + func
tryIt('type_func', [0,0x61,0x73,0x6d,1,0,0,0, ...sec(1,[1,0x60,0,1,I32]), ...sec(3,[1,0])]);
// 4. + code: body = i32.const 42 ; end ; NO locals vector entry
tryIt('code_nolocvec', [0,0x61,0x73,0x6d,1,0,0,0, ...sec(1,[1,0x60,0,1,I32]), ...sec(3,[1,0]),
  ...sec(10,[1, ...leb(3), 0x41,0x2a,0x0b])]);
// 5. + code WITH empty locals vector byte
tryIt('code_locvec0', [0,0x61,0x73,0x6d,1,0,0,0, ...sec(1,[1,0x60,0,1,I32]), ...sec(3,[1,0]),
  ...sec(10,[1, 0x00, ...leb(3), 0x41,0x2a,0x0b])]);
// 6. + export
tryIt('full_f', [0,0x61,0x73,0x6d,1,0,0,0, ...sec(1,[1,0x60,0,1,I32]), ...sec(3,[1,0]),
  ...sec(7,[1,...nm('f'),0,0]), ...sec(10,[1, 0x00, ...leb(3), 0x41,0x2a,0x0b])]);