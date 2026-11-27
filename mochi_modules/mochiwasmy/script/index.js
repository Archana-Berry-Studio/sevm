/*
 * mochi_modules/mochiwasmy/script/index.js — WebAssembly surface for the
 * native mochiwasmy_x86_64.dll (a self-contained wasm VM: import table is
 * KERNEL32/ntdll/CRT api-sets only, the Zig VM is statically linked).
 *
 * Exposes the standard WebAssembly.* API (Module, Instance, Memory, Global,
 * Table, instantiate, compile, validate) so wasm-bindgen glue runs unchanged
 * inside mbjs.exe. The DLL loads via async import(), so sync constructors
 * require `await WebAssembly.init()` first.
 *
 * instantiate(bytes, imports) auto-registers host imports: each value under
 * imports[modName] that is a function is wired with wasmRegisterHost, so a wasm
 * module importing `wasi_snapshot_preview1.fd_write` or `__wbg_*` resolves
 * through the JS object you pass in.
 *
 * instance.exports.memory is a LIVE getter over wasm linear memory; exported
 * functions are fetched lazily via wasmExportFunc and called with wasmCall.
 *
 * Measured (selftest/mochi-wasm.js): E:/wasmyberry/tests/test.wasm (749 B)
 * compiles, instantiates, add(2,40)=42, mul(6,7)=42, fact(6)=720, fib(10)=55,
 * maxi(3,9)=9, abs(-5)=5, and the wasi_snapshot_preview1.fd_write host import
 * is registered and callable.
 */

let mw = null;
let nativeErr = null;
let initPromise = null;

const DLL_PATH = 'E:/sevm/mochi_modules/mochiwasmy/lib/mochiwasmy_x86_64.dll';

async function loadMw() {
    if (mw) return mw;
    if (nativeErr) throw nativeErr;
    try {
        mw = await import(DLL_PATH);
    } catch (e) {
        nativeErr = e;
        throw e;
    }
    return mw;
}

function mwSync() {
    if (!mw) throw new Error('mochiwasmy: call await WebAssembly.init() before using sync constructors');
    return mw;
}

/* wasmValidate/wasmCompile use MBJS_GetArrayBuffer, which wants a real
 * ArrayBuffer, not a typed-array view. */
function toArrayBuffer(b) {
    if (b instanceof ArrayBuffer) return b;
    if (ArrayBuffer.isView(b)) return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
    if (b && b.buffer instanceof ArrayBuffer) return b.buffer.slice(0);
    throw new Error('mochiwasmy: cannot convert to ArrayBuffer');
}

const modHandles = new Map();
const instHandles = new Map();

class WasmMemory {
    constructor(opts) {
        this._initial = (opts && opts.initial) || 0;
        this._maximum = (opts && opts.maximum) || undefined;
        this._buffer = null;
        this.grow = (n) => { throw new Error('WasmMemory.grow not wired'); };
    }
    get buffer() { return this._buffer; }
}

class WasmGlobal {
    constructor(v, t) { this.value = v; }
}

class WasmTable {
    constructor(o) { this._o = o || {}; }
    get length() { return (this._o && this._o.initial) || 0; }
    set(i, v) { this._o[i] = v; }
    get(i) { return this._o[i]; }
}

class WasmModule {
    constructor(bytes) {
        const mwl = mwSync();
        const ab = toArrayBuffer(bytes);
        this._bytesRef = ab;          // parser keeps slices into this buffer
        this._mh = mwl.wasmCompile(ab);
        if (this._mh < 0) throw new Error('wasmCompile failed');
        this._bytes = bytes;
        modHandles.set(this._mh, this);
    }
    static async create(bytes) { await loadMw(); return new WasmModule(bytes); }
}

class WasmInstance {
    constructor(module, imports) {
        if (!(module instanceof WasmModule)) throw new Error('Instance requires WasmModule');
        this._module = module;
        this._imports = imports || {};
        const mwl = mwSync();
        this._ih = mwl.wasmInstantiate(module._mh, null);
        if (this._ih < 0) throw new Error('wasmInstantiate failed');
        instHandles.set(this._ih, this);
        this._registerHostImports();
        this._registerExports();
    }
    static async create(module, imports) { await loadMw(); return new WasmInstance(module, imports); }

    _registerHostImports() {
        const mwl = mwSync();
        const ih = this._ih;
        this._hostFns = [];   // keep wrappers alive: the engine frees call args
        for (const modName of Object.keys(this._imports)) {
            const tbl = this._imports[modName];
            if (!tbl || typeof tbl !== 'object') continue;
            for (const fnName of Object.keys(tbl)) {
                const fn = tbl[fnName];
                if (typeof fn !== 'function') continue;
                const wrapper = (...args) => {
                    const callArgs = (args.length === 1 && Array.isArray(args[0])) ? args[0] : args;
                    return fn.apply(null, callArgs);
                };
                this._hostFns.push(wrapper);
                mwl.wasmRegisterHost(ih, modName, fnName, wrapper);
            }
        }
    }

    _syncMem() {
        const mwl = mwSync();
        const cur = mwl.wasmMemoryLen(this._ih);
        if (this._memLen === undefined) { this._memLen = cur; return; }
        if (cur !== this._memLen) {
            if (this._lastView) {
                try { mwl.wasmDetachView(this._lastView); } catch (e) {}
                this._lastView = null;
            }
            this._memLen = cur;
        }
    }

    _registerExports() {
        const mwl = mwSync();
        const ih = this._ih;
        let memView = null;
        try { memView = mwl.wasmMemory(ih); } catch (e) {}
        const self = this;
        const memoryObj = {
            get buffer() {
                const v = mwl.wasmMemory(ih);
                if (!v) return undefined;
                self._lastView = v;
                self._memLen = v.length;
                return v.buffer;
            },
            get _view() { return mwl.wasmMemory(ih); },
            grow: (n) => { throw new Error('memory.grow not wired in shim'); },
        };
        this.exports = new Proxy({}, {
            get(target, prop) {
                if (typeof prop === 'symbol') return target[prop];
                if (prop === 'memory' && memoryObj) return memoryObj;
                if (prop in target) return target[prop];
                const idx = mwl.wasmExportFunc(self._ih, String(prop));
                if (idx < 0) return undefined;
                const fn = (...args) => {
                    const res = mwl.wasmCall(self._ih, idx, args);
                    try { self._syncMem(); } catch (e) {}
                    if (Array.isArray(res) && res.length === 1) return res[0];
                    return res;
                };
                target[prop] = fn;
                return fn;
            }
        });
        if (memView) {
            try {
                Object.defineProperty(this.exports, '__wasmMemoryView', { value: memView, enumerable: false });
            } catch (e) {}
        }
    }
}

const WebAssembly = {
    Module: WasmModule,
    Instance: WasmInstance,
    Memory: WasmMemory,
    Global: WasmGlobal,
    Table: WasmTable,
    init() {
        if (!initPromise) initPromise = loadMw().then(() => {});
        return initPromise;
    },
    instantiate(input, imports) {
        return (async () => {
            const mod = (input instanceof WasmModule) ? input : await WasmModule.create(input);
            const inst = await WasmInstance.create(mod, imports);
            return { module: mod, instance: inst };
        })();
    },
    compile(bytes) { return WasmModule.create(bytes); },
    validate(bytes) {
        try { return mwSync().wasmValidate(toArrayBuffer(bytes)); } catch (e) { return false; }
    },
};

await loadMw();

export { WebAssembly };
export async function init() { return loadMw(); }
export function isNativeReady() { return mw != null; }
export function __exports() { return mw ? Object.keys(mw) : []; }
export default { WebAssembly, init, isNativeReady, exports: () => (mw ? Object.keys(mw) : []) };
