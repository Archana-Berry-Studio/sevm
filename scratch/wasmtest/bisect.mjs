function leb(n){const o=[];do{let b=n&0x7f;n>>>=7;if(n)b|=0x80;o.push(b);}while(n);return o;}
function sec(id,p){return [id,...leb(p.length),...p];}
function nm(s){return [...leb(s.length),...[...s].map(c=>c.charCodeAt(0))];}
const I32=0x7f;

// Type 0: (i32,i32)->i32 ; Type 1: (i32)->i32
const types=[...leb(2),0x60,2,I32,I32,1,I32, 0x60,1,I32,1,I32];
const imp=[1, ...nm('__env'), ...nm('double'), 0x00, 0x01];
const funcs=[2,0x00,0x01];
const exp=[2, ...nm('add'),0x00,0x01, ...nm('call_double'),0x00,0x02];
function body(code){return [1,1,I32, ...leb(code.length), ...code];}
const code=[2, ...body([0x20,0x00,0x20,0x01,0x6a,0x0b]), ...body([0x20,0x00,0x10,0x00,0x0b])];

function build(opts){
  const parts=[0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00, sec(1,types)];
  if(opts.imp) parts.push(sec(2,imp));
  parts.push(sec(3,funcs), sec(7,exp), sec(10,code));
  return Uint8Array.from(parts.flat());
}
const tests={
  'full':build({imp:1}),
  'no_import':Uint8Array.from([0,0x61,0x73,0x6d,1,0,0,0, sec(1,types), sec(3,[2,0,0x01]), sec(7,[1,...nm('add'),0,1]), sec(10,[1,...body([0x20,0,0x20,1,0x6a,0x0b])]).flat()]),
};
for(const [k,u] of Object.entries(tests)){
  console.log('=== '+k+' len='+u.byteLength+' validate='+WebAssembly.validate(u));
  console.log('   hex='+Buffer.from(u).toString('hex'));
}