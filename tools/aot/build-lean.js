/*
 * build-lean.js — "compile everything" AOT generator for SEVM.
 *
 * WHY
 * ---
 * The existing AOT pipeline (node_modules/mochijsnative/build.js) only
 * compiles the ENTRY module to bytecode.  Every other module is embedded as
 * plain JS source and extracted to a temp directory at startup
 * (#define BUNDLE_EXTRACT_ALL 1), where the engine PARSES it at runtime.
 * That is why the shipped executable is fat and why it still needs the
 * JavaScript parser inside the engine.
 *
 * This generator does the real thing:
 *
 *   1. collect every module reachable from the entry (static imports,
 *      dynamic require(), native plugins)
 *   2. rewrite every module specifier to the canonical bundle name
 *      "mochibundle:<path-relative-to-root>"
 *   3. compile EVERY module (entry included) to bytecode with mbjsc
 *   4. emit a C header with the bytecode blobs + a specifier->module alias
 *      table, and a small host that registers the modules before the entry
 *      runs
 *
 * Result: no JS source in the binary, no runtime parsing, no parser in the
 * engine (the engine is linked with -DMBJS_DISABLE_PARSER), and the native
 * plugins plus the static assets are loaded from disk next to the app
 * instead of being stuffed into the executable.
 *
 * Run with the engine:
 *   mbjs.exe -m tools/aot/build-lean.js
 */
import * as std from 'mbjs:std';
import * as os from 'mbjs:os';
import { cwd, dirname, join, resolvePath, resolvePackage, resolveNativePath,
         fileExists, readText } from '../../node_modules/mochinode/index.js';

const ROOT = 'E:/sevm';
/* end-of-command marker for exec(); declared early because envFlag() runs
   before the rest of the module body is evaluated */
const EC_MARKER = '__EC__';
/* Release or debug flavour of the bundle (SEVM_AOT_DEBUG=1).  The debug
   flavour keeps the source inside the bytecode so stack traces show source
   lines, compiles the plugins without optimisation and defaults the trace
   to the verbose level. */
const DEBUG_BUILD = envFlag('SEVM_AOT_DEBUG') === '1';
const WORK = DEBUG_BUILD ? 'E:/sevm/build_ex/debug' : 'E:/sevm/build_ex/lean';

const ENTRY_REL = 'examples/sevm-app/src/server.js';
/* The entry can be pointed elsewhere with <WORK>/entry_path.txt
   (used by the smoke test so it does not fight for :8080). */
const ENTRY_POINTER = WORK + '/entry_path.txt';
let ENTRY = join(ROOT, ENTRY_REL);
try {
    const pointer = readText(ENTRY_POINTER).trim();
    if (pointer) ENTRY = norm(pointer);
} catch (e) { /* default entry */ }
/* The prebuilt mbjsc.exe in the engine tree is stale (bytecode v27 vs the
   engine's v29).  tools/aot/build-mbjsc.sh rebuilds it from the same source
   tree with the same feature guards. */
const MBJSC = 'E:/sevm/build_ex/mbjsc_v29/mbjsc.exe';
const NATIVE_EXT = /\.(dll|so|dylib|lbrct|exe)$/i;
/* Mirrors node_modules/mochinode/index.js BUILTIN_ALIASES.  mochinode's
   require() applies this map BEFORE calling __MOCHI_MODULE_NAME__, so the
   runtime loads the ALIASED package ('fs' -> 'mochifs').  The collector must
   follow the same map or it collects the wrong module. */
const BUILTIN_ALIASES = {
    'fs': 'mochifs', 'node:fs': 'mochifs',
    'path': 'mochinode', 'node:path': 'mochinode',
    'child_process': 'mochinode', 'node:child_process': 'mochinode',
    'util': 'mochinode', 'node:util': 'mochinode'
};
const JS_EXT = /\.(js|mjs|cjs)$/i;

/* ------------------------------------------------------------------ */
/* process helpers (mbjs:std has popen(); no child_process here)       */
/* ------------------------------------------------------------------ */

function envFlag(name) {
    try { return exec('echo %' + name + '%').stdout.trim(); } catch (e) { return ''; }
}

function exec(command) {
    const wrapped = 'cmd /v:on /c "' + command + ' & echo ' + EC_MARKER + '!ERRORLEVEL!"';
    const file = std.popen(wrapped, 'r');
    if (!file) throw new Error('popen failed: ' + command);
    const chunk = new ArrayBuffer(64 * 1024);
    let out = '';
    for (;;) {
        const n = file.read(chunk, 0, chunk.byteLength);
        if (n === 0) break;
        if (n < 0) break;
        out += latin1(chunk, n);
    }
    file.close();
    let code = 0;
    const at = out.lastIndexOf(EC_MARKER);
    if (at >= 0) {
        const tail = out.slice(at + EC_MARKER.length);
        const m = tail.match(/-?\d+/);
        code = m ? parseInt(m[0], 10) : 0;
        out = out.slice(0, at);
    }
    return { stdout: out, code };
}

function latin1(buffer, len) {
    const bytes = new Uint8Array(buffer, 0, len);
    let s = '';
    for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
    return s;
}

function q(value) {
    return '"' + String(value).replace(/\\/g, '/').replace(/"/g, '\\"') + '"';
}

function norm(value) {
    const s = String(value).replace(/\\/g, '/');
    const drive = /^([A-Za-z]:)\//.exec(s);
    const absolute = !!drive || s.startsWith('/');
    const prefix = drive ? drive[1] + '/' : absolute ? '/' : '';
    const rest = drive ? s.slice(3) : absolute ? s.slice(1) : s;
    const parts = [];
    for (const part of rest.split('/')) {
        if (!part || part === '.') continue;
        if (part === '..') {
            if (parts.length && parts[parts.length - 1] !== '..') parts.pop();
            else if (!absolute) parts.push('..');
        } else parts.push(part);
    }
    return prefix + parts.join('/');
}

function writeText(path, text) {
    const fd = os.open(String(path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC);
    if (fd < 0) throw new Error('cannot write ' + path);
    const bytes = new TextEncoder().encode(String(text));
    let off = 0;
    while (off < bytes.length) {
        const n = os.write(fd, bytes.buffer, off, Math.min(65536, bytes.length - off));
        if (!n || n < 0) break;
        off += n;
    }
    os.close(fd);
}

function readBytes(path) {
    const st = os.stat(String(path))[0];
    if (!st) throw new Error('cannot stat ' + path);
    const size = Number(st.size || 0);
    const fd = os.open(String(path), os.O_RDONLY);
    if (fd < 0) throw new Error('cannot open ' + path);
    const out = new Uint8Array(size);
    let off = 0;
    while (off < size) {
        const n = os.read(fd, out.buffer, off, Math.min(65536, size - off));
        if (!n || n < 0) break;
        off += n;
    }
    os.close(fd);
    return out.subarray(0, off);
}

/* ------------------------------------------------------------------ */
/* module collection                                                   */
/* ------------------------------------------------------------------ */

function specifiers(source) {
    const out = [];
    const code = String(source)
        .replace(/\/\*[\s\S]*?\*\//g, ' ')
        .replace(/(^|[^:])\/\/[^\n]*/g, '$1 ');
    const rules = [
        /(?:import|export)\s+(?:[^'";]*?\s+from\s+)?['"]([^'"]+)['"]/g,
        /\bimport\s*\(\s*['"]([^'"]+)['"]\s*\)/g,
        /\b(?:require|mrequire)\s*\(\s*['"]([^'"]+)['"]/g,
        /\b(?:loadNative|resolveNativePath)\s*\(\s*['"]([^'"]+)['"]/g
    ];
    for (const re of rules) {
        let match;
        while ((match = re.exec(code))) {
            if (out.indexOf(match[1]) < 0) out.push(match[1]);
        }
    }
    return out;
}

function resolveSpecifier(spec, importer) {
    const s = String(spec);
    if (s.startsWith('mbjs:') || s.startsWith('data:') || s.startsWith('node:')) return null;
    if (NATIVE_EXT.test(s)) return resolveNativePath(s, dirname(importer));
    if (s.startsWith('.') || s.startsWith('/') || /^[A-Za-z]:[\\/]/.test(s)) {
        const target = resolvePath(importer, s);
        if (fileExists(target)) return target;
        if (fileExists(target + '.js')) return target + '.js';
        if (fileExists(join(target, 'index.js'))) return join(target, 'index.js');
        return null;
    }
    return resolvePackage(s, dirname(importer));
}

function findPackageRoot(path) {
    let dir = dirname(path);
    while (dir && dir !== '/' && !/^[A-Za-z]:$/.test(dir)) {
        if (fileExists(join(dir, 'package.json'))) return dir;
        const parent = dirname(dir);
        if (parent === dir) break;
        dir = parent;
    }
    return null;
}

function collect(entryPath) {
    const jsFiles = new Set();
    const bare = new Map();          // specifier -> resolved path
    const natives = new Set();
    const scanned = new Set();
    const deps = new Map();          // path -> [dependency paths]
    const queue = [norm(entryPath)];

    while (queue.length) {
        const current = queue.shift();
        if (scanned.has(current)) continue;
        scanned.add(current);
        let source;
        try { source = readText(current); } catch (e) { continue; }
        const edges = [];
        for (const spec of specifiers(source)) {
            const aliased = BUILTIN_ALIASES[spec] || spec;
            const resolved = resolveSpecifier(aliased, current);
            if (!resolved) continue;
            const target = norm(resolved);
            if (NATIVE_EXT.test(target)) { natives.add(target); continue; }
            const isBare = !spec.startsWith('.') && !spec.startsWith('/') &&
                          !/^[A-Za-z]:[\\/]/.test(spec);
            if (isBare && !bare.has(aliased)) bare.set(aliased, target);
            if (JS_EXT.test(target)) {
                jsFiles.add(target);
                if (edges.indexOf(target) < 0) edges.push(target);
                if (!scanned.has(target)) queue.push(target);
            }
        }
        deps.set(current, edges);
    }
    return { jsFiles: [...jsFiles], bare, natives: [...natives], deps };
}

/* A module that is registered with mbjs_std_eval_binary(..., load_only=1)
   is INSTANTIATED right away, which resolves its imports.  So the modules
   have to be registered in dependency order: dependencies first. */
function topoSort(files, deps) {
    const state = new Map();   // 0 = new, 1 = visiting, 2 = done
    const out = [];
    function visit(path, stack) {
        const s = state.get(path) || 0;
        if (s === 2) return;
        if (s === 1) return;    // cycle: break it
        state.set(path, 1);
        for (const dep of deps.get(path) || []) visit(dep, stack);
        state.set(path, 2);
        out.push(path);
    }
    for (const f of files) visit(norm(f), []);
    return out;
}

function relativeTo(path, root) {
    const p = norm(path);
    const prefix = root.replace(/\/+$/, '') + '/';
    return p.startsWith(prefix) ? p.slice(prefix.length) : p.replace(/^[A-Za-z]:\//, '');
}

/* ------------------------------------------------------------------ */
/* compile                                                             */
/* ------------------------------------------------------------------ */

function rewriteSpecifiers(source, importer, keyByPath) {
    return String(source).replace(/(['"])([^'"\\]+)\1/g, (whole, quote, spec) => {
        if (spec.startsWith('mbjs:') || spec.startsWith('mochibundle:') ||
            spec.startsWith('data:') || spec.startsWith('node:') ||
            NATIVE_EXT.test(spec) || /\s/.test(spec)) return whole;
        const resolved = resolveSpecifier(spec, importer);
        if (!resolved) return whole;
        const key = keyByPath.get(norm(resolved));
        return key ? quote + 'mochibundle:' + key + quote : whole;
    });
}

function bytecodeFromC(source) {
    const calls = source.match(/mbjs_std_eval_binary\(ctx,\s*([A-Za-z0-9_]+),/g) || [];
    if (!calls.length) throw new Error('no bytecode array in generated C');
    const cname = calls[calls.length - 1].match(/,\s*([A-Za-z0-9_]+),/)[1];
    const match = source.match(new RegExp('const uint8_t ' + cname + '\\[(\\d+)\\] = \\{([\\s\\S]*?)\\};'));
    if (!match) throw new Error('cannot read ' + cname);
    const hex = match[2].match(/0x[0-9a-fA-F]{2}/g) || [];
    const bytes = new Uint8Array(hex.length);
    for (let i = 0; i < hex.length; i++) bytes[i] = parseInt(hex[i].slice(2), 16);
    return bytes;
}

/* ---- incremental compile cache --------------------------------------
 *
 * mbjsc is invoked once per module, and every invocation also re-compiles
 * the module's static dependency tree.  Re-running the whole pipeline for a
 * one-line change was therefore needlessly slow.  The cache keys the
 * bytecode on (source + module name + compiler stamp), so only modules that
 * actually changed are handed to the compiler again.
 */
/* mbjsc flags for the AOT build.
 *   -s -s   strip the embedded source text and the debug info from the
 *           bytecode.  The source is only kept for stack traces; for a
 *           release AOT binary it is dead weight: the C output of a single
 *           module drops from 394 KB to 103 KB (-74%) and the embedded
 *           bytecode shrinks proportionally.
 *   -O10    (default) the stripper optimisation level.
 *
 * A debug build drops the -s entirely: the source stays inside the bytecode
 * so a stack trace can point at a source line.
 */
const COMPILE_FLAGS = DEBUG_BUILD ? [] : ['-s'];

const CACHE = WORK + '/cache';
let cacheHits = 0;
let cacheMisses = 0;

function fnv(text) {
    let h1 = 0x811c9dc5 >>> 0;
    let h2 = 0x01000193 >>> 0;
    for (let i = 0; i < text.length; i++) {
        const c = text.charCodeAt(i);
        h1 = Math.imul(h1 ^ c, 16777619) >>> 0;
        h2 = Math.imul(h2 ^ c, 2654435761) >>> 0;
    }
    return h1.toString(16) + '-' + h2.toString(16) + '-' + text.length.toString(16);
}

function compilerStamp() {
    let size = 0;
    try {
        const st = os.stat(MBJSC)[0];
        if (st) size = Number(st.size || 0);
    } catch (e) { /* ignore */ }
    return 'mbjsc-v29:' + size;
}

function writeBytes(path, bytes) {
    const fd = os.open(String(path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC);
    if (fd < 0) throw new Error('cannot write ' + path);
    let off = 0;
    while (off < bytes.length) {
        const n = os.write(fd, bytes.buffer, bytes.byteOffset + off,
                           Math.min(65536, bytes.length - off));
        if (!n || n < 0) break;
        off += n;
    }
    os.close(fd);
}

function compileModule(source, moduleName, tag) {
    const key = fnv(source + '\n@@' + moduleName + '\n@@' + compilerStamp() +
                         '\n@@' + COMPILE_FLAGS.join(' '));
    const cacheFile = join(CACHE, key + '.mbc');
    if (fileExists(cacheFile)) {
        cacheHits++;
        return readBytes(cacheFile);
    }
    const srcPath = join(WORK, 'src_' + tag + '.js');
    const cPath = join(WORK, 'mod_' + tag + '.c');
    writeText(srcPath, source);
    /* mbjsc resolves a module's imports relative to the module NAME's
       directory, using the current working directory as the base.  The
       canonical names are relative to ROOT ('node_modules/...'), so the
       compiler has to run from ROOT. */
    const cmd = 'cd /d ' + q(ROOT) + ' && ' + q(MBJSC) + ' -e ' +
                COMPILE_FLAGS.join(' ') + ' -m -n ' + q(moduleName) +
                ' -o ' + q(cPath) + ' ' + q(srcPath);
    const r = exec(cmd);
    if (r.code !== 0) {
        throw new Error('mbjsc failed for ' + moduleName + '\n' + r.stdout);
    }
    const bytes = bytecodeFromC(readText(cPath));
    writeBytes(cacheFile, bytes);
    cacheMisses++;
    return bytes;
}

/* ------------------------------------------------------------------ */
/* C emission                                                          */
/* ------------------------------------------------------------------ */

function hexArray(name, bytes) {
    let out = 'static const unsigned char ' + name + '[' + bytes.length + '] = {\n';
    for (let i = 0; i < bytes.length; i += 16) {
        const chunk = [];
        for (let j = i; j < Math.min(i + 16, bytes.length); j++) {
            chunk.push('0x' + bytes[j].toString(16).padStart(2, '0'));
        }
        out += '  ' + chunk.join(',') + (i + 16 < bytes.length ? ',\n' : '\n');
    }
    out += '};\n';
    return out;
}

function cString(s) {
    let out = '"';
    for (const ch of String(s)) {
        const code = ch.codePointAt(0);
        if (ch === '"' || ch === '\\') out += '\\' + ch;
        else if (code < 32 || code > 126) out += '\\' + code.toString(8).padStart(3, '0');
        else out += ch;
    }
    return out + '"';
}

/* ------------------------------------------------------------------ */
/* MochiBake                                                            */
/* ------------------------------------------------------------------ */
/* MochiBake is the internal organ that bakes the native plugins a program
 * actually calls into the bundle, so the result is a small monolith that
 * runs without any external source file.
 *
 * The compiler's view of a DLL is its PE export/import table.  The
 * programmer's intent is read straight from the JavaScript: every
 * loadNative()/require() of a .dll and every dynalib(path, 'symbol') call
 * is a declaration of "this program needs this".  Nothing is guessed and
 * nothing is bundled blindly.
 *
 * Rule:
 *   - a DLL that lives inside the project is baked into the binary;
 *   - a system DLL (C:/Windows/...) is left to the OS loader: the
 *     essential base components stay pulled, they are just not copied;
 *   - a DLL nobody calls is not baked at all.
 */
function peInfo(bytes) {
    const u16 = (o) => bytes[o] | (bytes[o + 1] << 8);
    const u32 = (o) => (bytes[o] | (bytes[o + 1] << 8) |
                        (bytes[o + 2] << 16) | (bytes[o + 3] << 24)) >>> 0;
    const str = (off) => {
        let e = off, s = '';
        while (e < bytes.length && bytes[e] !== 0) {
            s += String.fromCharCode(bytes[e]);
            e++;
        }
        return s;
    };
    if (bytes.length < 0x40) return null;
    const peoff = u32(0x3c);
    if (peoff + 4 + 20 + 0x38 > bytes.length) return null;
    const nsec = u16(peoff + 6);
    const optsize = u16(peoff + 20);
    const opt = peoff + 24;
    const magic = u16(opt);
    const dd = opt + (magic === 0x20b ? 0x70 : 0x60);
    const secs = [];
    const secBase = opt + optsize;
    for (let i = 0; i < nsec; i++) {
        const s = secBase + i * 40;
        secs.push({ va: u32(s + 12), vsz: u32(s + 8),
                    rsz: u32(s + 16), raw: u32(s + 20) });
    }
    const rva2off = (rva) => {
        for (const s of secs) {
            const span = s.vsz > s.rsz ? s.vsz : s.rsz;
            if (rva >= s.va && rva < s.va + span) return rva - s.va + s.raw;
        }
        return -1;
    };
    const exports = [];
    const expRva = dd + 8 <= bytes.length ? u32(dd) : 0;
    if (expRva) {
        const e = rva2off(expRva);
        if (e >= 0 && e + 40 <= bytes.length) {
            const nnames = u32(e + 24), anames = u32(e + 32);
            const no = rva2off(anames);
            for (let i = 0; i < nnames && no >= 0; i++) {
                const n = str(rva2off(u32(no + i * 4)));
                if (n) exports.push(n);
            }
        }
    }
    const imports = [];
    const impRva = u32(dd + 8);
    if (impRva) {
        let d = rva2off(impRva);
        while (d >= 0 && d + 20 <= bytes.length) {
            const nameRva = u32(d + 12);
            if (nameRva === 0 && u32(d + 16) === 0) break;
            const nm = str(rva2off(nameRva));
            if (nm) imports.push(nm);
            d += 20;
        }
    }
    return { machine: u16(peoff + 4), imageSize: u32(opt + 0x38),
             exports: exports, imports: imports };
}

function stripJsComments(src) {
    return String(src)
        .replace(/\/\*[\s\S]*?\*\//g, ' ')
        .replace(/(^|[^:])\/\/[^\n]*/g, '$1 ');
}

function baseName(path) {
    const s = String(path).replace(/\\/g, '/');
    const i = s.lastIndexOf('/');
    return i < 0 ? s : s.slice(i + 1);
}

function nativeCandidates(spec, importer) {
    const s = String(spec).replace(/\\/g, '/');
    const hasExt = /\.(dll|so|dylib|lbrct|exe)$/i.test(s);
    const exts = hasExt ? [s] : [s + '.dll', s + '.so', s + '.dylib', s + '.lbrct'];
    const bases = [ROOT + '/mochi_modules/', ROOT + '/node_modules/', dirname(importer) + '/'];
    for (const cand of exts) {
        if (/^[A-Za-z]:\//.test(cand)) {
            const st = os.stat(cand);
            if (st && st[0]) return norm(cand);
            continue;
        }
        for (const b of bases) {
            const path = norm(b + cand);
            if (fileExists(path)) return path;
        }
    }
    return null;
}

function scanNativeUses(files) {
    const uses = new Map();
    const note = (path, symbol) => {
        if (!path) return;
        let e = uses.get(path);
        if (!e) {
            e = { path: path, name: baseName(path), symbols: new Set() };
            uses.set(path, e);
        }
        if (symbol) e.symbols.add(symbol);
    };
    for (const f of files) {
        const code = stripJsComments(readText(f));
        let m;

        /* 1. direct native specifiers: loadNative / require / import /
              resolveNative — with or without the file extension. */
        const reSpec = /\b(?:loadNative|require|mrequire|resolveNative|resolveNativePath)\s*\(\s*['"]([^'"]+)['"]/g;
        while ((m = reSpec.exec(code))) {
            const p = nativeCandidates(m[1], f);
            if (p) note(p, null);
        }

        /* 2. a native module bound to a variable:
              const nativePath = resolveNative('mochifs');
              native = await import(nativePath);          */
        const varPath = new Map();
        const reBind = /(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=\s*(?:await\s+)?(?:resolveNative|resolveNativePath)\s*\(\s*['"]([^'"]+)['"]/g;
        while ((m = reBind.exec(code))) {
            const p = nativeCandidates(m[2], f);
            if (p) { varPath.set(m[1], p); note(p, null); }
        }
        const reImport = /(?:const|let|var)?\s*([A-Za-z_$][\w$]*)\s*=\s*await\s+(?:import|loadNative|require|mrequire)\s*\(\s*([A-Za-z_$][\w$]*|['"][^'"]+['"])\s*\)/g;
        const nsVars = new Map();
        while ((m = reImport.exec(code))) {
            const spec = m[2].replace(/^['"]|['"]$/g, '');
            const p = varPath.has(spec) ? varPath.get(spec) : nativeCandidates(spec, f);
            if (p) { nsVars.set(m[1], p); note(p, null); }
        }

        /* 3. the symbols the program actually calls on that module:
              native.readText(...)  ->  readText is what it wants. */
        nsVars.forEach(function (path, varName) {
            /* Any property access counts: the symbol may be called directly
               (native.readText(...)), through .apply()/.call(), or handed
               over as a callback (makeResponse(native.httpWrite, conn)).
               Over-reporting keeps a function alive that could have been
               dropped - the safe direction. */
            const reUse = new RegExp('\\b' + varName + '\\.([A-Za-z_$][\\w$]*)', 'g');
            let u;
            while ((u = reUse.exec(code))) note(path, u[1]);
            const reDest = new RegExp('\\{([^}]*)\\}\\s*=\\s*' + varName + '\\b', 'g');
            while ((u = reDest.exec(code))) {
                const parts = u[1].split(',');
                for (const part of parts) {
                    const nm = part.split(':').pop().trim();
                    if (nm) note(path, nm);
                }
            }
        });

        /* 4. dynalib(file, regOrOffset, symbol, abiSig, opts) */
        const reDyn = /\bdynalib\s*\(\s*['"]([^'"]+)['"]\s*,\s*([^,]+)\s*,\s*(?:null|'([^']*)'|"([^"]*)")/g;
        while ((m = reDyn.exec(code))) {
            const p = nativeCandidates(m[1], f);
            if (p) note(p, m[3] || m[4] || null);
        }
    }
    return uses;
}

/* ------------------------------------------------------------------ */
/* MochiBake: per-function slicer (recompile, not just re-embed)        */
/* ------------------------------------------------------------------ */
/* A plugin whose C source we own can be recompiled with only the
 * functions the program calls.  The export table in a *derived copy* of
 * the source is trimmed to the used JS names (the original file is never
 * touched), the hard-coded table counts are fixed, and the linker garbage
 * collects every static function nothing references any more.  The result
 * is a smaller DLL that is still a real native module. */
const GCC = 'C:/Users/Administrator/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.MCF.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin/gcc.exe';
const SLICE_DIR = WORK + '/sliced';

const NATIVE_RECIPES = {
    'mochifs.dll': {
        src: ['E:/sevm/native/mochifs.c'],
        inc: ['E:/bcf/mochiberry', 'E:/sevm/native'],
        libs: []
    },
    'mochinode_sqlite.dll': {
        src: ['E:/sevm/native/mochinode_sqlite.c'],
        inc: ['E:/bcf/mochiberry', 'E:/sevm/native'],
        libs: []
    },
    'mochinode_net.dll': {
        src: ['E:/sevm/native/mochinode_net.c'],
        inc: ['E:/bcf/mochiberry', 'E:/sevm/native'],
        libs: ['-lws2_32']
    },
    'mochinode_dynalib.dll': {
        src: ['E:/sevm/native/mochinode_dynalib.c',
              'E:/sevm/libdynalib/dynalib.c',
              'E:/sevm/libdynalib/dynalib_abi.c',
              'E:/sevm/libdynalib/dynalib_trampoline.c'],
        inc: ['E:/bcf/mochiberry', 'E:/sevm/native', 'E:/sevm/libdynalib'],
        libs: []
    }
};

function defLine(line) {
    return line.match(/^\s*MBJS_CFUNC_DEF\(\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*([A-Za-z_]\w*)\s*\)\s*,?\s*$/);
}

function absolutizeIncludes(text, baseDir) {
    return text.replace(/#include\s+"(\.\.?\/[^"]+)"/g, function (all, inc) {
        return '#include "' + norm(baseDir + '/' + inc) + '"';
    });
}

/* trim the CFUNC_DEF tables to the used JS names and fix the counts */
function slicePluginSource(srcText, used, baseDir) {
    const lines = srcText.split('\n');
    const out = [];
    const macroFix = [];
    let kept = 0, removed = 0;
    let i = 0;
    while (i < lines.length) {
        if (!defLine(lines[i])) { out.push(lines[i]); i++; continue; }
        const run = [];
        let j = i;
        while (j < lines.length) {
            const mm = defLine(lines[j]);
            if (!mm) break;
            run.push({ line: lines[j], name: mm[1] });
            j++;
        }
        const keep = [];
        for (const e of run) if (used.has(e.name)) keep.push(e);
        removed += run.length - keep.length;
        kept += keep.length;
        for (const e of keep) out.push(e.line);
        let k = j;
        while (k < lines.length && k < j + 6) {
            const cm = lines[k].match(/([A-Za-z_]\w*)\s*,\s*(\d+|[A-Za-z_]\w*)\s*\)/);
            if (cm) {
                const n = cm[2];
                if (/^\d+$/.test(n) && Number(n) === run.length) {
                    out.push(lines[k].replace(/([A-Za-z_]\w*)\s*,\s*\d+\s*\)/, '$1, ' + keep.length + ')'));
                    k++;
                    break;
                }
                if (/^[A-Za-z_]\w*$/.test(n) && n !== cm[1]) {
                    macroFix.push([n, run.length, keep.length]);
                    out.push(lines[k]);
                    k++;
                    break;
                }
            }
            out.push(lines[k]);
            k++;
        }
        i = k;
    }
    let text = out.join('\n');
    for (const fix of macroFix) {
        const re = new RegExp('#define\\s+' + fix[0] + '\\s+' + fix[1] + '\\b');
        text = text.replace(re, '#define ' + fix[0] + ' ' + fix[2]);
    }
    return { text: absolutizeIncludes(text, baseDir), kept: kept, removed: removed };
}

/* recompile one plugin with only the called functions */
function compileSliced(name, recipe, used) {
    exec('if not exist ' + q(SLICE_DIR) + ' mkdir ' + q(SLICE_DIR));
    const srcs = [];
    let kept = 0, removed = 0;
    for (let idx = 0; idx < recipe.src.length; idx++) {
        const p = recipe.src[idx];
        if (idx === 0) {
            const r = slicePluginSource(readText(p), used, dirname(p));
            const outPath = join(SLICE_DIR, name.replace(/\.(dll|so)$/i, '') + '_sliced.c');
            writeText(outPath, r.text);
            srcs.push(outPath);
            kept = r.kept; removed = r.removed;
        } else {
            srcs.push(p);
        }
    }
    const incs = recipe.inc.map(function (d) { return '-I' + q(d); }).join(' ');
    const libs = (recipe.libs || []).join(' ');
    const outDll = join(SLICE_DIR, name);
    const cmd = 'cd /d ' + q(SLICE_DIR) + ' && ' + q(GCC) +
                ' -shared -Os -ffunction-sections -fdata-sections' +
                ' -Wl,--gc-sections -Wl,-s ' + incs + ' -o ' + q(outDll) + ' ' +
                srcs.map(function (s) { return q(s); }).join(' ') + (libs ? ' ' + libs : '');
    const r = exec(cmd);
    if (r.code !== 0 || !fileExists(outDll)) {
        return { ok: false, err: (r.stdout || '').split('\n').slice(-3).join(' | '), kept: kept, removed: removed };
    }
    return { ok: true, dll: outDll, kept: kept, removed: removed };
}

const HOST_C = `/* lean_host.c — generated by tools/aot/build-lean.js.
 *
 * A minimal AOT host: the JavaScript parser is not even linked into this
 * binary (the engine is built with -DMBJS_DISABLE_PARSER).  Every module is
 * pre-compiled to bytecode and registered here before the entry module runs.
 *
 * DEBUG LEVELS
 * ------------
 *   SEVM_DEBUG=0 (or unset)  release — no trace, only real errors
 *   SEVM_DEBUG=1             file-load trace, timestamped
 *   SEVM_DEBUG=2             verbose: level 1 + the deepest addresses
 *                            (bytecode blobs inside this exe, and for the
 *                            native plugins their absolute mapping base,
 *                            PE image size, entry point and the address of
 *                            the exported mbjs_init_module), the bake
 *                            manifest, the per-module load result and
 *                            duration, every specifier the JS side asks
 *                            the runtime to resolve, and a closing summary
 *
 * The level can also be given on the command line: --debug=2
 *
 * The native plugins are NOT embedded in this binary: they are loaded from
 * mochi_modules/ at runtime, so the addresses printed at level 2 are the
 * absolute addresses the loader mapped them at.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef LEAN_DEBUG_BUILD
/* winsock2.h pulls in windows.h, so in a debug build the Win32 prototypes
   below must NOT be declared by hand. */
#include <winsock2.h>
#define LEAN_WIN32_PROVIDED 1
#endif

#include "mbjs.h"
#include "mbjs-libc.h"

#include "lean_bundle.h"

/* trace helpers (defined below, used by the generated loader first) */
static void lean_trace(const char *fmt, ...);
static void lean_append(char *buf, size_t n, const char *fmt, ...);
static void *lean_proc_addr(void *module, const char *name);

/* ---- debug plumbing ------------------------------------------------ */

#ifndef LEAN_WIN32_PROVIDED
extern unsigned long long __stdcall GetTickCount64(void);
extern void *__stdcall GetModuleHandleA(const char *name);
extern void *__stdcall GetProcAddress(void *module, const char *name);
extern long long __stdcall QueryPerformanceCounter(long long *counter);
extern long long __stdcall QueryPerformanceFrequency(long long *freq);
extern unsigned long __stdcall GetCurrentProcessId(void);
extern unsigned long __stdcall GetModuleFileNameA(void *module, char *buf, unsigned long size);
#endif

#ifndef LEAN_DEFAULT_DEBUG
#define LEAN_DEFAULT_DEBUG 0
#endif

static int lean_debug = -1;
static unsigned long long lean_t0 = 0;      /* QPC ticks at process start */
static unsigned long long lean_freq = 0;    /* QPC ticks per second */

static int lean_dbg(void)
{
  if (lean_debug < 0) {
    const char *env = getenv("SEVM_DEBUG");
    lean_debug = env ? atoi(env) : LEAN_DEFAULT_DEBUG;
    if (lean_debug < 0) lean_debug = 0;
    if (lean_debug > 3) lean_debug = 3;
  }
  return lean_debug;
}

/* QueryPerformanceCounter takes a LARGE_INTEGER * under windows.h and a
   long long * under the hand-declared flavour. */
static long long lean_qpc_get(void)
{
#ifndef LEAN_WIN32_PROVIDED
  long long v = 0;
  QueryPerformanceCounter(&v);
  return v;
#else
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  return li.QuadPart;
#endif
}

static long long lean_qpc_freq(void)
{
#ifndef LEAN_WIN32_PROVIDED
  long long v = 0;
  QueryPerformanceFrequency(&v);
  return v;
#else
  LARGE_INTEGER li;
  QueryPerformanceFrequency(&li);
  return li.QuadPart;
#endif
}

/* Clock: QueryPerformanceCounter, so the fraction is real (100 ns steps),
   not the millisecond tick of GetTickCount64. */
static void lean_clock_init(void)
{
  lean_freq = (unsigned long long)lean_qpc_freq();
  lean_t0 = (unsigned long long)lean_qpc_get();
}

/* [JJ:MM:DD.mmmmmmmm] counted from process start.
   JJ = hour, MM = minute, DD = second, the eight fractional digits are the
   sub-second part (milli / micro / pico / sub-nano steps). */
static void lean_stamp(char *buf, size_t n)
{
  long long now = 0;
  unsigned long long ticks = 0, units = 0, h, m, s, frac;
  now = lean_qpc_get();
  if (lean_freq)
    ticks = (unsigned long long)now - lean_t0;
  units = lean_freq ? (ticks * 100000000ULL) / lean_freq : 0;   /* 100 ns units */
  h = units / 36000000000ULL;
  m = (units / 600000000ULL) % 60;
  s = (units / 100000000ULL) % 60;
  frac = units % 100000000ULL;
  snprintf(buf, n, "%02llu:%02llu:%02llu.%08llu", h, m, s, frac);
}

/* SizeOfImage of a mapped PE (0 when it is not a PE). */
static unsigned long lean_image_size(void *base)
{
  unsigned char *b = (unsigned char *)base;
  unsigned long off;
  if (!b) return 0;
  off = *(unsigned long *)(b + 0x3C);
  if (!off || off >= 0x1000) return 0;
  return *(unsigned long *)(b + off + 4 + 20 + 0x38);
}

/* 100 ns units since process start (same clock as the stamp). */
static unsigned long long lean_now(void)
{
  long long now = 0;
  now = lean_qpc_get();
  if (!lean_freq) return 0;
  return ((unsigned long long)now - lean_t0) * 100000000ULL / lean_freq;
}

/* Deep address information of a module the loader has just mapped.
   These are absolute addresses in the process image, not embedded copies. */
static void lean_pe_info(const char *path, char *buf, size_t n)
{
  unsigned char *base = (unsigned char *)GetModuleHandleA(path);
  unsigned long off = 0, entry = 0, size = 0;
  void *init = NULL;

  if (!base) {
    snprintf(buf, n, "base=<not mapped>");
    return;
  }
  off = *(unsigned long *)(base + 0x3C);
  if (off && off < 0x1000) {
    entry = *(unsigned long *)(base + off + 4 + 20 + 0x10);
    size  = *(unsigned long *)(base + off + 4 + 20 + 0x38);
  }
  init = lean_proc_addr(base, "mbjs_init_module");
  snprintf(buf, n, "base=%p image=0x%lx entry=0x%lx init=%p",
           (void *)base, size, entry, init);
}

/* ---- MochiBake: the baked native plugins --------------------------- */
/* The compiler embedded the DLLs this program actually calls.  At startup
   they are materialised into the bake scratch directory and the loader
   hands those copies to the engine, so the app runs from a single file
   with no external plugin directory. */
#ifndef LEAN_WIN32_PROVIDED
extern int CreateDirectoryA(const char *path, void *attrs);
extern unsigned long __stdcall GetLastError(void);
extern unsigned long __stdcall GetCurrentDirectoryA(unsigned long len, char *buf);
#endif

#define LEAN_BAKE_MAX 32
static char lean_bake_path[LEAN_BAKE_MAX][1200];
static char lean_bake_dir[1024];

static int lean_file_exists(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fclose(f);
  return 1;
}

/* Materialise the baked plugins.
   The JS-side resolver (mochinode) looks for native plugins in
   <cwd>/mochi_modules, so the copies are placed there when possible; a
   plugin that already exists on disk is never overwritten, that one goes
   to the temp bake directory instead. */
static void lean_bake_prepare(void)
{
  const char *tmp = getenv("TEMP");
  char cwd[1024];
  char localDir[1200], tempDir[1200];
  unsigned long n;
  size_t i;

  if (!tmp) tmp = ".";
  snprintf(tempDir, sizeof tempDir, "%s/mochibake", tmp);
  CreateDirectoryA(tempDir, 0);

  n = GetCurrentDirectoryA(sizeof cwd, cwd);
  if (n > 0 && n < sizeof cwd) {
    snprintf(localDir, sizeof localDir, "%s/mochi_modules", cwd);
    CreateDirectoryA(localDir, 0);
  } else {
    localDir[0] = 0;
  }

  if (lean_dbg() >= 2) {
    char ts[32];
    unsigned long long total = 0;
    lean_stamp(ts, sizeof ts);
    for (i = 0; i < lean_bake_count && i < LEAN_BAKE_MAX; i++)
      total += lean_bakes[i].size;
    lean_trace("[lean] [%s] manifest %lu baked native(s), %llu B embedded in this exe\\n",
            ts, (unsigned long)lean_bake_count, total);
  }

  for (i = 0; i < lean_bake_count && i < LEAN_BAKE_MAX; i++) {
    char local[1200], tmpPath[1200], path[1200];
    int found = 0;
    FILE *f;

    snprintf(local, sizeof local, "%s/%s", localDir, lean_bakes[i].name);
    snprintf(tmpPath, sizeof tmpPath, "%s/%s", tempDir, lean_bakes[i].name);

    /* 1. already reachable next to the app -> use it, never write over it */
    if (localDir[0] && lean_file_exists(local)) {
      snprintf(path, sizeof path, "%s", local);
      found = 1;
    }
    /* 2. the temp bake copy from an earlier run */
    if (!found && lean_file_exists(tmpPath)) {
      snprintf(path, sizeof path, "%s", tmpPath);
      found = 1;
    }
    /* 3. create it next to the app (the JS resolver looks there) */
    if (!found && localDir[0]) {
      f = fopen(local, "wb");
      if (f) {
        fwrite(lean_bakes[i].data, 1, lean_bakes[i].size, f);
        fclose(f);
        snprintf(path, sizeof path, "%s", local);
        found = 1;
      }
    }
    /* 4. last resort: the temp bake directory */
    if (!found) {
      f = fopen(tmpPath, "wb");
      if (!f) {
        lean_trace("lean: cannot materialize %s (err=%lu)\\n",
                lean_bakes[i].name, GetLastError());
        lean_bake_path[i][0] = 0;
        continue;
      }
      fwrite(lean_bakes[i].data, 1, lean_bakes[i].size, f);
      fclose(f);
      snprintf(path, sizeof path, "%s", tmpPath);
    }
    snprintf(lean_bake_path[i], sizeof lean_bake_path[i], "%s", path);
    if (lean_dbg() >= 1) {
      char ts[32];
      lean_stamp(ts, sizeof ts);
      lean_trace("[lean] [%s] bake    %s -> %s (%lu B, symbols: %s)\\n",
              ts, lean_bakes[i].name, path, (unsigned long)lean_bakes[i].size,
              lean_bakes[i].symbols[0] ? lean_bakes[i].symbols : "(all)");
      if (lean_dbg() >= 2) {
        /* where the DLL bytes live inside this exe */
        unsigned char *img = (unsigned char *)GetModuleHandleA(NULL);
        unsigned long long abs = (unsigned long long)(size_t)lean_bakes[i].data;
        unsigned long long rva = img ? (abs - (unsigned long long)(size_t)img) : 0;
        const char *s = lean_bakes[i].symbols;
        int nsym = 0;
        while (*s) { if (*s == ',') nsym++; s++; }
        if (*lean_bakes[i].symbols) nsym++;
        lean_trace("         embedded  %lu B at +0x%llx (abs 0x%llx), %d symbol(s)\\n",
                (unsigned long)lean_bakes[i].size, rva, abs, nsym);
      }
    }
  }
}

/* If the engine asks for a plugin we have baked, hand it the materialised
   copy instead of the one sitting in mochi_modules/. */
static const char *lean_bake_lookup(const char *name)
{
  static char path[1200];
  const char *base = name;
  const char *slash = strrchr(name, '/');
  const char *bslash = strrchr(name, '\\\\');
  size_t i;
  if (slash) base = slash + 1;
  if (bslash && bslash + 1 > base) base = bslash + 1;
  for (i = 0; i < lean_bake_count && i < LEAN_BAKE_MAX; i++) {
    if (!strcmp(base, lean_bakes[i].name)) {
      if (!lean_bake_path[i][0]) return NULL;
      snprintf(path, sizeof path, "%s", lean_bake_path[i]);
      return path;
    }
  }
  return NULL;
}

/* Called by the generated resolver helpers (see lean_bundle.h). */
void lean_trace_resolve(const char *kind, const char *spec, const char *name, int found)
{
  char ts[32];
  if (lean_dbg() < 2) return;
  lean_stamp(ts, sizeof ts);
  lean_trace("[lean] [%s] %-7s  %s -> %s\\n", ts, kind, spec,
          found ? name : "(unresolved)");
}

/* Loader wrapper: traces what the engine asks for, then forwards to the
   engine loader (which performs the actual native plugin load). */
static MBJSModuleDef *lean_module_loader(MBJSContext *ctx, const char *name,
                                         void *opaque, MBJSValue attributes)
{
  extern MBJSModuleDef *mbjs_module_loader(MBJSContext *ctx, const char *name,
                                           void *opaque, MBJSValue attributes);
  int d = lean_dbg();
  char ts[32];
  MBJSModuleDef *m;

  {
    const char *baked = lean_bake_lookup(name);
    const char *target = baked ? baked : name;
    m = mbjs_module_loader(ctx, target, opaque, attributes);
    if (d >= 1) {
      char line[2048];
      line[0] = 0;
      lean_stamp(ts, sizeof ts);
      lean_append(line, sizeof line, "[lean] [%s] load    %s -> %s", ts, name,
                  m ? "ok" : "FAILED");
      if (baked)
        lean_append(line, sizeof line, "  [bake: %s]", baked);
      if (d >= 2) {
        char pe[192];
        lean_pe_info(target, pe, sizeof pe);
        lean_append(line, sizeof line, "  (%s)", pe);
      }
      lean_append(line, sizeof line, "\\n");
      lean_trace("%s", line);
    }
  }
  return m;
}

static MBJSContext *MBJS_NewCustomContext(MBJSRuntime *rt)
{
  MBJSContext *ctx = MBJS_NewContext(rt);
  size_t k;
  int d = lean_dbg();
  char ts[32];

  if (!ctx)
    return NULL;
  {
    extern MBJSModuleDef *mbjs_init_module_std(MBJSContext *ctx, const char *name);
    mbjs_init_module_std(ctx, "mbjs:std");
  }
  {
    extern MBJSModuleDef *mbjs_init_module_os(MBJSContext *ctx, const char *name);
    mbjs_init_module_os(ctx, "mbjs:os");
  }
  /* MochiBake: the engine's own DLL baking organ. */
  {
    extern MBJSModuleDef *mbjs_init_module_bake(MBJSContext *ctx, const char *name);
    mbjs_init_module_bake(ctx, "mbjs:bake");
  }
  /* Register every pre-compiled module under its bundle name.  After this
     the engine resolves the module names from the registry and never
     touches the file system for JavaScript. */
  for (k = 0; k < lean_module_count; k++) {
    if (d >= 1) {
      lean_stamp(ts, sizeof ts);
      {
        char line[2048];
        line[0] = 0;
        lean_append(line, sizeof line, "[lean] [%s] register %s -> %lu B", ts,
                    lean_modules[k].name, (unsigned long)lean_modules[k].size);
        if (d >= 2) {
          /* the blob lives in this exe: show the offset inside the image
             first, then where it really is in the process */
          unsigned char *img = (unsigned char *)GetModuleHandleA(NULL);
          unsigned long long abs = (unsigned long long)(size_t)lean_modules[k].data;
          unsigned long long rva = img ? (abs - (unsigned long long)(size_t)img) : 0;
          lean_append(line, sizeof line, " -> +0x%llx (abs 0x%llx, bytecode inside this exe)",
                      rva, abs);
        }
        lean_append(line, sizeof line, "\\n");
        lean_trace("%s", line);
      }
    }
    {
      unsigned long long t0 = lean_now();
      mbjs_std_eval_binary(ctx, lean_modules[k].data, lean_modules[k].size, 1);
      if (d >= 2) {
        char ts[32];
        unsigned long long t1 = lean_now();
        lean_stamp(ts, sizeof ts);
        lean_trace("[lean] [%s] module  %s -> %s in %llu us\\n",
                ts, lean_modules[k].name,
                MBJS_HasException(ctx) ? "FAILED" : "ok", (t1 - t0) * 10);
      }
    }
    if (MBJS_HasException(ctx)) {
      lean_trace("lean: module %s failed to load\\n", lean_modules[k].name);
      mbjs_std_dump_error(ctx);
    }
  }
  return ctx;
}

/* ------------------------------------------------------------------ */
/* remote debugger  —  level 3 (debug build only)                     */
/*                                                                     */
/* A TCP server inside the binary, on 127.0.0.1:9229 by default.      */
/* It works like a two-way UART / adb line: the remote side sends     */
/* commands, the binary answers, and every trace line is pushed to a  */
/* subscribed client while it happens.  Commands that need the JS      */
/* runtime (eval) are handed over to the main thread, which services   */
/* them from its own event loop — the engine is never touched from    */
/* the socket thread.                                                  */
/* ------------------------------------------------------------------ */
#if defined(LEAN_DEBUG_BUILD)
#include <winsock2.h>

/* winsock2.h brings in windows.h here, so Sleep / Interlocked* /
   CreateThread / CloseHandle are already declared — declaring them again
   only fights the real prototypes. */

#define DBG_RING_SIZE (256 * 1024)
#define DBG_CMD_MAX   2048

static volatile long dbg_spin_log = 0;
static volatile long dbg_spin_req = 0;
#define DBG_TAKE(v) while (InterlockedCompareExchange(&(v), 1, 0)) { }
#define DBG_DROP(v) InterlockedExchange(&(v), 0)

static char dbg_ring[DBG_RING_SIZE];
static unsigned long dbg_ring_len = 0;
static SOCKET dbg_client = INVALID_SOCKET;
static int dbg_client_stream = 0;
static int dbg_quit = 0;
static int dbg_port = 9229;
static MBJSContext *dbg_ctx = NULL;

typedef struct {
  int pending, done, ok;
  char expr[DBG_CMD_MAX];
  char result[8192];
} DbgRequest;
static DbgRequest dbg_req = { 0, 1, 0, { 0 }, { 0 } };

static void dbg_send(const char *s)
{
  if (dbg_client == INVALID_SOCKET) return;
  send(dbg_client, s, (int)strlen(s), 0);
}

/* Every trace line ends up here: the ring buffer (for "logs") and, when
   the remote subscribed, straight down the socket. */
static void dbg_sink(const char *s)
{
  size_t n = strlen(s);
  int stream;
  DBG_TAKE(dbg_spin_log);
  if (dbg_ring_len + n + 1 > DBG_RING_SIZE) {
    unsigned long keep = DBG_RING_SIZE / 2;
    memmove(dbg_ring, dbg_ring + (dbg_ring_len - keep), keep);
    dbg_ring_len = keep;
  }
  memcpy(dbg_ring + dbg_ring_len, s, n);
  dbg_ring_len += n;
  dbg_ring[dbg_ring_len] = 0;
  stream = dbg_client_stream;
  DBG_DROP(dbg_spin_log);
  if (stream) dbg_send(s);
}
#else
static void dbg_sink(const char *s) { (void)s; }
#endif

/* GetProcAddress differs between the hand-declared and the windows.h
   flavour (FARPROC vs void *), so all call sites go through this helper. */
static void *lean_proc_addr(void *module, const char *name)
{
#ifdef LEAN_WIN32_PROVIDED
  return (void *)(size_t)GetProcAddress((void *)module, name);
#else
  return GetProcAddress(module, name);
#endif
}

/* One trace line.  Always stderr; the remote ring when built in. */
static void lean_trace(const char *fmt, ...)
{
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  dbg_sink(buf);
  fputs(buf, stderr);
}

/* append to a line buffer (used to build one trace line in one go) */
static void lean_append(char *buf, size_t n, const char *fmt, ...)
{
  size_t l = strlen(buf);
  va_list ap;
  if (l + 1 >= n) return;
  va_start(ap, fmt);
  vsnprintf(buf + l, n - l, fmt, ap);
  va_end(ap);
}

#if defined(LEAN_DEBUG_BUILD)
/* ---- symbol lookup: the exe's own export table --------------------- */
static void dbg_sym(unsigned long long addr, char *buf, size_t n)
{
  unsigned char *img = (unsigned char *)GetModuleHandleA(NULL);
  unsigned long peoff, expRva, expSize, nNames, funcsRva, namesRva, ordsRva, i;
  unsigned char *exp;
  const char *best = NULL;
  unsigned long long bestAddr = 0;

  buf[0] = 0;
  if (!img) { snprintf(buf, n, "(no image)"); return; }
  peoff = *(unsigned long *)(img + 0x3C);
  if (!peoff || peoff >= 0x1000) { snprintf(buf, n, "(bad PE header)"); return; }
  expRva  = *(unsigned long *)(img + peoff + 4 + 20 + 0x70);
  expSize = *(unsigned long *)(img + peoff + 4 + 20 + 0x74);
  if (!expRva || !expSize) { snprintf(buf, n, "(no export directory)"); return; }
  exp = img + expRva;
  nNames  = *(unsigned long *)(exp + 0x18);
  funcsRva = *(unsigned long *)(exp + 0x1C);
  namesRva = *(unsigned long *)(exp + 0x20);
  ordsRva  = *(unsigned long *)(exp + 0x24);
  if (!nNames || !namesRva || !funcsRva || !ordsRva) { snprintf(buf, n, "(no named exports)"); return; }
  {
    /* the name ordinals are 16 bit (IMAGE_EXPORT_DIRECTORY keeps WORDs) —
       reading them as dwords turns every index into garbage and walks off
       the image */
    unsigned long *names = (unsigned long *)(img + namesRva);
    unsigned long *funcs = (unsigned long *)(img + funcsRva);
    unsigned short *ords = (unsigned short *)(img + ordsRva);
    unsigned long nFuncs = *(unsigned long *)(exp + 0x14);
    unsigned long imgSize = lean_image_size(img);
    for (i = 0; i < nNames; i++) {
      unsigned long ord = ords[i];
      unsigned long rva, nameRva;
      unsigned long long a;
      if (ord >= nFuncs) continue;
      rva = funcs[ord];
      nameRva = names[i];
      if (!rva || rva >= imgSize) continue;
      if (!nameRva || nameRva >= imgSize) continue;
      a = (unsigned long long)(size_t)(img + rva);
      if (addr >= a && (best == NULL || a > bestAddr)) {
        best = (const char *)(img + nameRva);
        bestAddr = a;
      }
    }
  }
  if (best) snprintf(buf, n, "%s+0x%llx", best, addr - bestAddr);
  else snprintf(buf, n, "(unknown address)");
}

static void dbg_cat(char *out, size_t n, const char *s)
{
  size_t l = strlen(out);
  if (l + 1 < n) strncat(out, s, n - l - 1);
}

/* Is the range inside something that is really mapped?  A debugger must
   never kill its subject, so a raw read of an unmapped address is refused
   instead of faulting. */
static int dbg_range_ok(unsigned long long a, unsigned long len)
{
  unsigned char *img = (unsigned char *)GetModuleHandleA(NULL);
  unsigned long long sz = lean_image_size(img);
  unsigned long k;

  if (img && a >= (unsigned long long)(size_t)img &&
      a + len <= (unsigned long long)(size_t)img + sz)
    return 1;
  for (k = 0; k < lean_module_count; k++) {
    unsigned long long b = (unsigned long long)(size_t)lean_modules[k].data;
    if (a >= b && a + len <= b + lean_modules[k].size) return 1;
  }
  for (k = 0; k < lean_bake_count; k++) {
    unsigned long long b = (unsigned long long)(size_t)lean_bakes[k].data;
    if (a >= b && a + len <= b + lean_bakes[k].size) return 1;
  }
  for (k = 0; k < lean_bake_count; k++) {
    void *m = GetModuleHandleA(lean_bake_path[k]);
    unsigned long long b = (unsigned long long)(size_t)m, s = lean_image_size(m);
    if (m && a >= b && a + len <= b + s) return 1;
  }
  return 0;
}

/* ---- the command set ---------------------------------------------- */
static void dbg_command(char *line)
{
  static char scratch[DBG_RING_SIZE];
  char out[32768];
  char *cmd = line, *arg = strchr(line, ' ');
  if (arg) { *arg = 0; arg++; }

  if (!strcmp(cmd, "ping")) { dbg_send("pong\\n"); return; }
  if (!strcmp(cmd, "help")) {
    dbg_send("ping | info | modules | bakes | logs [n] | stream | nostream | "
             "mem <addr> <len> | sym <addr> | eval <expr> | shutdown\\n");
    return;
  }
  if (!strcmp(cmd, "info")) {
    unsigned char *img = (unsigned char *)GetModuleHandleA(NULL);
    unsigned long long total = 0;
    size_t k;
    char exe[1024];
    for (k = 0; k < lean_module_count; k++) total += lean_modules[k].size;
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(out, sizeof out,
      "pid=%lu exe=%s image=0x%lx base=%p qpc=%llu Hz uptime=%llu us debug=%d "
      "modules=%lu aliases=%lu bakes=%lu bytecode=%llu B entry=%lu B ring=%lu B\\n",
      GetCurrentProcessId(), exe, lean_image_size(img), (void *)img, lean_freq,
      lean_now() * 10, lean_dbg(), (unsigned long)lean_module_count,
      (unsigned long)lean_alias_count, (unsigned long)lean_bake_count, total,
      (unsigned long)lean_entry_size, dbg_ring_len);
    dbg_send(out);
    return;
  }
  if (!strcmp(cmd, "modules")) {
    unsigned char *img = (unsigned char *)GetModuleHandleA(NULL);
    size_t k;
    out[0] = 0;
    for (k = 0; k < lean_module_count; k++) {
      unsigned long long abs = (unsigned long long)(size_t)lean_modules[k].data;
      unsigned long long rva = img ? (abs - (unsigned long long)(size_t)img) : 0;
      char one[512];
      snprintf(one, sizeof one, "%2lu %-46s %8lu B  +0x%llx  abs 0x%llx\\n",
               (unsigned long)k, lean_modules[k].name,
               (unsigned long)lean_modules[k].size, rva, abs);
      dbg_cat(out, sizeof out, one);
    }
    dbg_send(out);
    return;
  }
  if (!strcmp(cmd, "bakes")) {
    unsigned char *img = (unsigned char *)GetModuleHandleA(NULL);
    size_t k;
    out[0] = 0;
    for (k = 0; k < lean_bake_count; k++) {
      unsigned long long abs = (unsigned long long)(size_t)lean_bakes[k].data;
      unsigned long long rva = img ? (abs - (unsigned long long)(size_t)img) : 0;
      char one[1024];
      snprintf(one, sizeof one, "%2lu %-26s %8lu B  +0x%llx  abs 0x%llx  %s\\n",
               (unsigned long)k, lean_bakes[k].name,
               (unsigned long)lean_bakes[k].size, rva, abs,
               lean_bakes[k].symbols[0] ? lean_bakes[k].symbols : "(all)");
      dbg_cat(out, sizeof out, one);
    }
    dbg_send(out);
    return;
  }
  if (!strcmp(cmd, "logs")) {
    unsigned long want = 40, count = 0, i;
    long start;
    if (arg) want = strtoul(arg, NULL, 10);
    DBG_TAKE(dbg_spin_log);
    memcpy(scratch, dbg_ring, dbg_ring_len);
    scratch[dbg_ring_len] = 0;
    DBG_DROP(dbg_spin_log);
    for (i = 0; i < dbg_ring_len; i++) if (scratch[i] == '\\n') count++;
    if (want > count) want = count;
    start = (long)dbg_ring_len;
    for (i = 0; i < want; i++) {
      long j;
      start--;
      while (start > 0 && scratch[start] != '\\n') start--;
    }
    dbg_send(scratch + (start > 0 ? start + 1 : 0));
    return;
  }
  if (!strcmp(cmd, "stream")) { dbg_client_stream = 1; dbg_send("streaming on\\n"); return; }
  if (!strcmp(cmd, "nostream")) { dbg_client_stream = 0; dbg_send("streaming off\\n"); return; }
  if (!strcmp(cmd, "mem")) {
    unsigned long long a = 0;
    unsigned long len = 0, i;
    char *p2 = arg ? strchr(arg, ' ') : NULL;
    if (p2) { *p2 = 0; len = strtoul(p2 + 1, NULL, 0); }
    if (arg) a = strtoull(arg, NULL, 0);
    if (len > 4096) len = 4096;
    if (!dbg_range_ok(a, len)) {
      dbg_send("ERR address not mapped (the debugger will not fault)\\n");
      return;
    }
    out[0] = 0;
    for (i = 0; i < len; i++) {
      char b[8];
      snprintf(b, sizeof b, "%02x ", ((unsigned char *)(size_t)a)[i]);
      dbg_cat(out, sizeof out, b);
      if ((i & 15) == 15) dbg_cat(out, sizeof out, "\\n");
    }
    dbg_cat(out, sizeof out, "\\n");
    dbg_send(out);
    return;
  }
  if (!strcmp(cmd, "sym")) {
    unsigned long long a = arg ? strtoull(arg, NULL, 0) : 0;
    char s[256];
    size_t k;
    for (k = 0; k < lean_module_count; k++) {
      unsigned long long b = (unsigned long long)(size_t)lean_modules[k].data;
      if (a >= b && a < b + lean_modules[k].size) {
        snprintf(s, sizeof s, "lean_modules[%lu].data (%s) +0x%llx",
                 (unsigned long)k, lean_modules[k].name, a - b);
        dbg_send(s); dbg_send("\\n");
        return;
      }
    }
    for (k = 0; k < lean_bake_count; k++) {
      unsigned long long b = (unsigned long long)(size_t)lean_bakes[k].data;
      if (a >= b && a < b + lean_bakes[k].size) {
        snprintf(s, sizeof s, "lean_bakes[%lu].data (%s) +0x%llx",
                 (unsigned long)k, lean_bakes[k].name, a - b);
        dbg_send(s); dbg_send("\\n");
        return;
      }
    }
    dbg_sym(a, s, sizeof s);
    dbg_send(s); dbg_send("\\n");
    return;
  }
  if (!strcmp(cmd, "eval")) {
    unsigned long waited = 0;
    int done = 0;
    if (!arg) { dbg_send("ERR eval needs an expression\\n"); return; }
    {
      char ts[32];
      lean_stamp(ts, sizeof ts);
      {
        char note[512];
        snprintf(note, sizeof note, "[lean] [%s] debug   cmd eval arg=%s\\n", ts, arg);
        dbg_sink(note); fputs(note, stderr);
      }
    }
    DBG_TAKE(dbg_spin_req);
    dbg_req.pending = 1; dbg_req.done = 0; dbg_req.ok = 0; dbg_req.result[0] = 0;
    strncpy(dbg_req.expr, arg, DBG_CMD_MAX - 1);
    dbg_req.expr[DBG_CMD_MAX - 1] = 0;
    DBG_DROP(dbg_spin_req);
    while (waited < 5000) {
      DBG_TAKE(dbg_spin_req); done = dbg_req.done; DBG_DROP(dbg_spin_req);
      if (done) break;
      Sleep(2); waited += 2;
    }
    DBG_TAKE(dbg_spin_req);
    if (!done) {
      dbg_req.pending = 0; dbg_req.done = 1;
      dbg_send("ERR timeout: the engine did not reach a safe point\\n");
    } else {
      snprintf(out, sizeof out, "%s\\n", dbg_req.result[0] ? dbg_req.result : "(no result)");
      dbg_send(out);
    }
    DBG_DROP(dbg_spin_req);
    return;
  }
  if (!strcmp(cmd, "shutdown")) { dbg_send("bye\\n"); dbg_quit = 1; return; }
  dbg_send("ERR unknown command (try help)\\n");
}

/* ---- serviced by the MAIN thread (the engine is single threaded) ---- */
static void dbg_service(void)
{
  char expr[DBG_CMD_MAX], script[DBG_CMD_MAX + 512];
  int pending;
  DBG_TAKE(dbg_spin_req);
  pending = dbg_req.pending;
  DBG_DROP(dbg_spin_req);
  if (!pending) return;
  if (!dbg_ctx) {
    /* nobody set the context: answer the socket thread instead of leaving
       it waiting forever */
    char ts[32];
    lean_stamp(ts, sizeof ts);
    DBG_TAKE(dbg_spin_req);
    dbg_req.done = 1; dbg_req.pending = 0;
    snprintf(dbg_req.result, sizeof dbg_req.result, "ERR no runtime context");
    DBG_DROP(dbg_spin_req);
    {
      char note[256];
      snprintf(note, sizeof note, "[lean] [%s] debug   eval refused (no ctx)\\n", ts);
      dbg_sink(note); fputs(note, stderr);
    }
    return;
  }
  DBG_TAKE(dbg_spin_req);
  strncpy(expr, dbg_req.expr, sizeof expr - 1);
  expr[sizeof expr - 1] = 0;
  DBG_DROP(dbg_spin_req);
  {
    char ts[32];
    lean_stamp(ts, sizeof ts);
    {
      char note[512];
      snprintf(note, sizeof note, "[lean] [%s] debug   eval queued expr=%s\\n", ts, expr);
      dbg_sink(note); fputs(note, stderr);
    }
  }

  snprintf(script, sizeof script,
    "(function(){try{var __r=(%s);return JSON.stringify(__r===undefined?'undefined':__r)}"
    "catch(e){var __m=(e&&e.message)?String(e.message):String(e);"
    "var __s=(e&&e.stack)?String(e.stack):'';"
    "return 'ERR: '+__m+(__s.indexOf(__m)<0?': '+__s:'')}})()",
    expr);

  {
    MBJSValue v = MBJS_Eval(dbg_ctx, script, strlen(script), "<debugger>",
                            MBJS_EVAL_TYPE_GLOBAL);
    char result[8192];
    int ok = 1;
    result[0] = 0;
    if (MBJS_HasException(dbg_ctx)) {
      MBJSValue exc = MBJS_GetException(dbg_ctx);
      const char *s = MBJS_ToCString(dbg_ctx, exc);
      ok = 0;
      snprintf(result, sizeof result, "ERR %s", s ? s : "(exception)");
      if (s) MBJS_FreeCString(dbg_ctx, s);
      MBJS_FreeValue(dbg_ctx, exc);
    } else {
      const char *s = MBJS_ToCString(dbg_ctx, v);
      if (s) { snprintf(result, sizeof result, "%s", s); MBJS_FreeCString(dbg_ctx, s); }
      else snprintf(result, sizeof result, "(unprintable)");
      MBJS_FreeValue(dbg_ctx, v);
    }
    {
      char ts[32];
      lean_stamp(ts, sizeof ts);
      {
        char note[1024];
        snprintf(note, sizeof note, "[lean] [%s] eval    %s -> %s\\n", ts, expr,
                 ok ? "ok" : "error");
        dbg_sink(note);
        fputs(note, stderr);
      }
    }
    DBG_TAKE(dbg_spin_req);
    dbg_req.ok = ok;
    snprintf(dbg_req.result, sizeof dbg_req.result, "%s", result);
    dbg_req.done = 1;
    dbg_req.pending = 0;
    DBG_DROP(dbg_spin_req);
  }
}

/* ---- the socket thread -------------------------------------------- */
static unsigned long __stdcall dbg_server_thread(void *arg)
{
  WSADATA wsa;
  SOCKET s;
  struct sockaddr_in a;
  int yes = 1;
  char buf[8192];
  int used = 0;

  (void)arg;
  if (WSAStartup(0x0202, &wsa)) return 1;
  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) { WSACleanup(); return 1; }
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons((unsigned short)dbg_port);
  if (bind(s, (struct sockaddr *)&a, sizeof a) == SOCKET_ERROR) { closesocket(s); WSACleanup(); return 2; }
  if (listen(s, 4) == SOCKET_ERROR) { closesocket(s); WSACleanup(); return 3; }

  {
    char msg[256], ts[32];
    lean_stamp(ts, sizeof ts);
    snprintf(msg, sizeof msg,
             "[lean] [%s] debugger listening on 127.0.0.1:%d\\n", ts, dbg_port);
    dbg_sink(msg);
    fputs(msg, stderr);
  }

  for (;;) {
    SOCKET c = accept(s, NULL, NULL);
    if (c == INVALID_SOCKET) break;
    dbg_client = c;
    dbg_client_stream = 0;
    used = 0;
    for (;;) {
      int n = recv(c, buf + used, (int)(sizeof buf - used - 1), 0);
      if (n <= 0) break;
      used += n;
      buf[used] = 0;
      {
        char *p = buf, *nl;
        int dispatched = 0;
        (void)0;
        while ((nl = strchr(p, '\\n')) != NULL) {
          *nl = 0;
          dbg_command(p);
          dispatched = 1;
          p = nl + 1;
          /* one-shot command (everything but 'stream'): answer then let
             the client reconnect, so a fresh command is not held hostage
             by a connection that stays open waiting for more input. */
          if (!dbg_client_stream) break;
        }
        used = (int)strlen(p);
        memmove(buf, p, used + 1);
        /* only one-shot-close once a whole command was dispatched — a recv
           that ended mid-command (no newline yet) must keep reading, not
           drop the socket */
        if (dispatched && !dbg_client_stream) break;
      }
      if (used >= (int)sizeof buf - 1) used = 0;
    }
    closesocket(c);
    dbg_client = INVALID_SOCKET;
    dbg_client_stream = 0;
  }
  closesocket(s);
  WSACleanup();
  return 0;
}
#endif /* LEAN_DEBUG_BUILD */

int main(int argc, char **argv)
{
  int r, i;
  MBJSRuntime *rt;
  MBJSContext *ctx;
  MBJSValue globals;
  char ts[32];

  lean_clock_init();
  lean_bake_prepare();
  for (i = 1; i < argc; i++) {
    if (!strncmp(argv[i], "--debug=", 8)) {
      lean_debug = atoi(argv[i] + 8);
      if (lean_debug < 0) lean_debug = 0;
      if (lean_debug > 3) lean_debug = 3;
    }
  }

  if (lean_dbg() >= 2) {
    char ts[32], exe[1024];
    unsigned char *img = (unsigned char *)GetModuleHandleA(NULL);
    unsigned long long total = 0;
    size_t k;
    lean_stamp(ts, sizeof ts);
    GetModuleFileNameA(NULL, exe, sizeof exe);
    for (k = 0; k < lean_module_count; k++) total += lean_modules[k].size;
    lean_trace("[lean] [%s] host     exe=%s pid=%lu image=0x%lx qpc=%llu Hz debug=%d "
            "modules=%lu aliases=%lu bakes=%lu bytecode=%llu B entry=%lu B\\n",
            ts, exe, GetCurrentProcessId(), lean_image_size(img), lean_freq,
            lean_dbg(), (unsigned long)lean_module_count,
            (unsigned long)lean_alias_count, (unsigned long)lean_bake_count,
            total, (unsigned long)lean_entry_size);
  }

  r = 0;
  rt = MBJS_NewRuntime();
  mbjs_std_set_worker_new_context_func(MBJS_NewCustomContext);
  mbjs_std_init_handlers(rt);
  MBJS_SetModuleLoaderFunc2(rt, NULL, lean_module_loader, mbjs_module_check_attributes, NULL);
  ctx = MBJS_NewCustomContext(rt);
  if (!ctx) {
    lean_trace("lean: cannot create context\\n");
    return 1;
  }
  globals = MBJS_GetGlobalObject(ctx);
  MBJS_SetPropertyStr(ctx, globals, "__MOCHI_MODULE_NAME__",
                      MBJS_NewCFunction(ctx, lean_module_resolve, "moduleName", 1));
  MBJS_SetPropertyStr(ctx, globals, "__MOCHI_MODULE_EXISTS__",
                      MBJS_NewCFunction(ctx, lean_module_exists, "moduleExists", 1));
  /* MochiBake as a global too: a bundled app must not need the compiler to
     know the "mbjs:bake" specifier, the organ is simply there at runtime. */
  {
    extern MBJSValue mbjs_bake_object(MBJSContext *ctx);
    MBJS_SetPropertyStr(ctx, globals, "bake", mbjs_bake_object(ctx));
  }
  MBJS_FreeValue(ctx, globals);

  mbjs_std_add_helpers(ctx, argc, argv);
#if defined(LEAN_DEBUG_BUILD)
  /* Level 3: the remote debugger.  The socket thread never touches the
     engine; commands that need JS are queued for the main thread. */
  if (lean_dbg() >= 3) {
    const char *portEnv = getenv("SEVM_DEBUG_PORT");
    unsigned long tid = 0;
    void *th;
    dbg_ctx = ctx;
    if (portEnv) dbg_port = atoi(portEnv);
    if (dbg_port <= 0 || dbg_port > 65535) dbg_port = 9229;
    th = CreateThread(NULL, 0, dbg_server_thread, NULL, 0, &tid);
    if (th) CloseHandle(th);
  }
#endif
  if (lean_dbg() >= 1) {
    lean_stamp(ts, sizeof ts);
    {
      char line[2048];
      line[0] = 0;
      lean_append(line, sizeof line, "[lean] [%s] eval    entry -> %lu B", ts,
                  (unsigned long)lean_entry_size);
      if (lean_dbg() >= 2) {
        unsigned char *img = (unsigned char *)GetModuleHandleA(NULL);
        unsigned long long abs = (unsigned long long)(size_t)lean_entry;
        unsigned long long rva = img ? (abs - (unsigned long long)(size_t)img) : 0;
        lean_append(line, sizeof line, " -> +0x%llx (abs 0x%llx, bytecode inside this exe)",
                    rva, abs);
      }
      lean_append(line, sizeof line, "\\n");
      lean_trace("%s", line);
    }
  }
#if defined(LEAN_DEBUG_BUILD)
  /* The debug build drives the entry itself: mbjs_std_eval_binary() waits on
     the entry promise inside its own loop and never returns for a
     long-running app, which would leave the remote debugger unserviced.
     Reading / resolving / evaluating here lets us interleave
     dbg_service() with the engine's poll so level 3 works while the app
     is running. */
  {
    MBJSValue obj = MBJS_ReadObject(ctx, lean_entry, lean_entry_size,
                                    MBJS_READ_OBJ_BYTECODE);
    if (MBJS_IsException(obj)) {
      mbjs_std_dump_error(ctx);
      r = 1;
    } else {
      if (MBJS_VALUE_GET_TAG(obj) == MBJS_TAG_MODULE) {
        if (MBJS_ResolveModule(ctx, obj) < 0) {
          mbjs_std_dump_error(ctx);
          r = 1;
        } else if (mbjs_module_set_import_meta(ctx, obj, false, true) < 0) {
          mbjs_std_dump_error(ctx);
          r = 1;
        }
      }
      if (!r) {
        MBJSValue val = MBJS_EvalFunction(ctx, obj);
        if (MBJS_IsException(val)) {
          mbjs_std_dump_error(ctx);
          r = 1;
        }
        MBJS_FreeValue(ctx, val);
      }
      MBJS_FreeValue(ctx, obj);
    }
  }
  /* The engine event loop, with the debugger getting a safe point every
     iteration.  mbjs_std_loop_once() runs one expired timer and reports the
     next delay, so the app keeps serving while the socket thread stays
     responsive. */
  if (!r) {
    for (;;) {
      int delay = mbjs_std_loop_once(ctx);
      dbg_service();
      if (dbg_quit) { r = 0; break; }
      if (MBJS_HasException(ctx)) { r = 1; break; }
      if (delay == 0) continue;
      if (delay < 0) delay = 50;   /* idle: short slice so the debugger runs */
      mbjs_std_poll_io(ctx, delay);
    }
  }
#else
  mbjs_std_eval_binary(ctx, lean_entry, lean_entry_size, 0);
  if (MBJS_HasException(ctx)) {
    lean_trace("lean: entry failed\\n");
    mbjs_std_dump_error(ctx);
  }
  r = mbjs_std_loop(ctx);
#endif
  if (r) {
    mbjs_std_dump_error(ctx);
  }
  if (lean_dbg() >= 2) {
    char ts[32];
    lean_stamp(ts, sizeof ts);
    lean_trace("[lean] [%s] summary  %lu module(s) registered, %lu B bytecode, "
                    "%lu baked native(s), exit=%d, uptime=%llu us\\n",
            ts, (unsigned long)lean_module_count, (unsigned long)lean_entry_size,
            (unsigned long)lean_bake_count, r, lean_now() * 10);
  }
  mbjs_std_free_handlers(rt);
  MBJS_FreeContext(ctx);
  MBJS_FreeRuntime(rt);
  return r;
}
`;

/* ------------------------------------------------------------------ */

async function main() {
    exec('if not exist ' + q(WORK) + ' mkdir ' + q(WORK));
    exec('if not exist ' + q(CACHE) + ' mkdir ' + q(CACHE));

    const collected = collect(ENTRY);
    const root = ROOT;
    const keyByPath = new Map();
    for (const p of collected.jsFiles) keyByPath.set(norm(p), relativeTo(p, root));

    console.log('modules found: ' + collected.jsFiles.length);
    for (const p of collected.jsFiles) {
        console.log('  ' + relativeTo(p, root));
    }
    console.log('native plugins: ' + collected.natives.length);
    for (const p of collected.natives) console.log('  ' + relativeTo(p, root));

    /* compile every module (dependency order: the host registers them in
       this order, so an import is always resolvable) */
    const ordered = topoSort(collected.jsFiles, collected.deps);
    const compiled = [];
    let index = 0;
    for (const path of ordered) {
        const key = keyByPath.get(norm(path));
        const moduleName = key;
        /* No specifier rewriting: the module is compiled under its canonical
           path, so the engine's own runtime resolution (import './x.js' from
           'node_modules/foo/index.js' -> 'node_modules/foo/x.js') lands
           exactly on the name the module was registered under. */
        const bytes = compileModule(readText(path), moduleName, String(index));
        compiled.push({ key, name: moduleName, bytes });
        index++;
    }

    /* the entry is compiled like every other module, but it is evaluated as
       the program (not registered in the module registry) */
    const entryKey = relativeTo(ENTRY, root);
    const entryBytes = compileModule(readText(ENTRY), entryKey, 'entry');

    /* alias table: bare specifier -> bundle module name (for runtime
       require('express') and friends) */
    const aliases = [];
    for (const [spec, path] of collected.bare) {
        const key = keyByPath.get(norm(path));
        if (key) aliases.push({ spec, name: key });
    }

    /* ---- MochiBake: bake the native plugins the program calls -------- */
    const uses = scanNativeUses(collected.jsFiles);
    const bakes = [];
    const systemDlls = [];
    uses.forEach(function (e, path) {
        if (path.indexOf(ROOT + '/') !== 0) { systemDlls.push(e); return; }
        const bytes = readBytes(path);
        const info = peInfo(bytes);
        bakes.push({ name: e.name, path: path, bytes: bytes,
                     symbols: Array.from(e.symbols), info: info });
    });

    /* per-function slice: recompile the plugins we own with only the
       functions the program actually calls */
    function bakeBytesOf(b) { return b.sliced ? b.sliced.bytes : b.bytes; }
    for (const b of bakes) {
        const recipe = NATIVE_RECIPES[b.name];
        b.sliced = null;
        if (!recipe) { console.log('  slice ' + b.name + ': no recipe, bake whole'); continue; }
        if (!b.symbols.length) { console.log('  slice ' + b.name + ': no symbol manifest, bake whole'); continue; }
        const r = compileSliced(b.name, recipe, new Set(b.symbols));
        if (!r.ok) {
            console.log('  slice FAILED ' + b.name + ': ' + r.err + ' (baking the whole DLL)');
            continue;
        }
        const sbytes = readBytes(r.dll);
        const sinfo = peInfo(sbytes);
        b.sliced = { bytes: sbytes, info: sinfo, kept: r.kept, removed: r.removed };
        console.log('  slice ' + b.name + ': ' + b.bytes.length + ' -> ' + sbytes.length + ' B (' +
                    (100 * sbytes.length / b.bytes.length).toFixed(1) + '%), kept=' + r.kept +
                    ' removed=' + r.removed + ', exports=' + sinfo.exports.length);
    }

    /* emit the C header */
    let header = '/* lean_bundle.h — generated by tools/aot/build-lean.js */\n' +
                 '#ifndef LEAN_BUNDLE_H\n#define LEAN_BUNDLE_H\n' +
                 '#include "mbjs.h"\n#include <string.h>\n\n' +
                 'typedef struct { const char *name; const unsigned char *data; unsigned long size; } LeanModule;\n\n';
    for (const m of compiled) header += hexArray('lean_mod_' + m.key.replace(/[^A-Za-z0-9]/g, '_'), m.bytes);
    header += '\nstatic const LeanModule lean_modules[] = {\n';
    for (const m of compiled) {
        header += '  { ' + cString(m.name) + ', lean_mod_' +
                  m.key.replace(/[^A-Za-z0-9]/g, '_') + ', ' + m.bytes.length + ' },\n';
    }
    header += '};\n\n';
    header += 'static const size_t lean_module_count = sizeof(lean_modules) / sizeof(lean_modules[0]);\n\n';
    header += 'typedef struct { const char *spec; const char *name; } LeanAlias;\n\n';
    header += 'static const LeanAlias lean_aliases[] = {\n';
    for (const a of aliases) header += '  { ' + cString(a.spec) + ', ' + cString(a.name) + ' },\n';
    header += '};\n\n';
    header += 'static const size_t lean_alias_count = sizeof(lean_aliases) / sizeof(lean_aliases[0]);\n\n';
    for (let i = 0; i < bakes.length; i++)
        header += hexArray('lean_bake_' + i, bakeBytesOf(bakes[i]));
    header += '\ntypedef struct { const char *name; const unsigned char *data; unsigned long size; const char *symbols; } LeanBake;\n\n';
    header += 'static const LeanBake lean_bakes[] = {\n';
    for (let i = 0; i < bakes.length; i++) {
        header += '  { ' + cString(bakes[i].name) + ', lean_bake_' + i + ', ' +
                  bakeBytesOf(bakes[i]).length + ', ' +
                  cString(bakes[i].symbols.join(',')) + ' },\n';
    }
    header += '};\n\nstatic const size_t lean_bake_count = sizeof(lean_bakes) / sizeof(lean_bakes[0]);\n\n';
    header += hexArray('lean_entry', entryBytes);
    header += 'static const unsigned long lean_entry_size = ' + entryBytes.length + ';\n\n';
    header += `
/* traced by the host at debug level 2: every specifier the JS side asks
   the runtime about */
void lean_trace_resolve(const char *kind, const char *spec, const char *name, int found);

static MBJSValue lean_module_resolve(MBJSContext *ctx, MBJSValueConst this_val,
                                      int argc, MBJSValueConst *argv)
{
  const char *spec; size_t i;
  if (argc < 1) return MBJS_UNDEFINED;
  spec = MBJS_ToCString(ctx, argv[0]);
  if (!spec) return MBJS_UNDEFINED;
  for (i = 0; i < lean_alias_count; i++) {
    if (!strcmp(lean_aliases[i].spec, spec)) {
      lean_trace_resolve("resolve", spec, lean_aliases[i].name, 1);
      MBJS_FreeCString(ctx, spec);
      return MBJS_NewString(ctx, lean_aliases[i].name);
    }
  }
  lean_trace_resolve("resolve", spec, "", 0);
  MBJS_FreeCString(ctx, spec);
  return MBJS_UNDEFINED;
}

static MBJSValue lean_module_exists(MBJSContext *ctx, MBJSValueConst this_val,
                                     int argc, MBJSValueConst *argv)
{
  const char *spec; size_t i;
  if (argc < 1) return MBJS_FALSE;
  spec = MBJS_ToCString(ctx, argv[0]);
  if (!spec) return MBJS_FALSE;
  for (i = 0; i < lean_module_count; i++) {
    if (!strcmp(lean_modules[i].name, spec)) {
      lean_trace_resolve("exists", spec, lean_modules[i].name, 1);
      MBJS_FreeCString(ctx, spec);
      return MBJS_TRUE;
    }
  }
  lean_trace_resolve("exists", spec, "", 0);
  MBJS_FreeCString(ctx, spec);
  return MBJS_FALSE;
}

#endif /* LEAN_BUNDLE_H */
`;
    writeText(join(WORK, 'lean_bundle.h'), header);
    /* A debug build defaults the trace to the verbose level. */
    const hostPre = DEBUG_BUILD
      ? '#define LEAN_DEBUG_BUILD 1\n#define LEAN_DEFAULT_DEBUG 3\n'
      : '';
    writeText(join(WORK, 'lean_host.c'), hostPre + HOST_C);

    const totalBytes = compiled.reduce((n, m) => n + m.bytes.length, 0);
    console.log('');
    console.log('compiled modules : ' + compiled.length +
                ' (' + cacheHits + ' from cache, ' + cacheMisses + ' recompiled)');
    console.log('bytecode bytes   : ' + totalBytes + ' (entry ' + entryBytes.length + ')');
    console.log('aliases          : ' + aliases.length);
    console.log('source bytes kept: 0 (JS is not embedded)');
    let bakeBytes = 0;
    for (const b of bakes) bakeBytes += bakeBytesOf(b).length;
    console.log('baked natives    : ' + bakes.length + ' (' + bakeBytes + ' B, embedded in the exe)');
    for (const b of bakes) {
        console.log('  ' + b.name + '  ' + b.bytes.length + ' B  exports=' +
                    (b.info ? b.info.exports.length : '?') +
                    '  symbols=' + (b.symbols.length ? b.symbols.join(',') : '(module)'));
    }
    console.log('system natives   : ' + systemDlls.length + ' (left to the OS loader)');
    for (const e of systemDlls) {
        console.log('  ' + e.name + '  symbols=' +
                    (e.symbols.size ? Array.from(e.symbols).join(',') : '(module)'));
    }
    console.log('written          : ' + join(WORK, 'lean_bundle.h') + ', ' + join(WORK, 'lean_host.c'));
}

main().catch((e) => {
    console.error('build-lean failed:', e && e.message ? e.message : e);
    process.exit(1);
});
