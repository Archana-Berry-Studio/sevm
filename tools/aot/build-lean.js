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
const ENTRY_REL = 'examples/sevm-app/src/server.js';
/* The entry can be pointed elsewhere with build_ex/lean/entry_path.txt
   (used by the port-8099 smoke test so it does not fight for :8080). */
const ENTRY_POINTER = 'E:/sevm/build_ex/lean/entry_path.txt';
let ENTRY = join(ROOT, ENTRY_REL);
try {
    const pointer = readText(ENTRY_POINTER).trim();
    if (pointer) ENTRY = norm(pointer);
} catch (e) { /* default entry */ }
const WORK = 'E:/sevm/build_ex/lean';
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
const EC_MARKER = '__EC__';

/* ------------------------------------------------------------------ */
/* process helpers (mbjs:std has popen(); no child_process here)       */
/* ------------------------------------------------------------------ */

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

function compileModule(source, moduleName, tag) {
    const srcPath = join(WORK, 'src_' + tag + '.js');
    const cPath = join(WORK, 'mod_' + tag + '.c');
    writeText(srcPath, source);
    /* mbjsc resolves a module's imports relative to the module NAME's
       directory, using the current working directory as the base.  The
       canonical names are relative to ROOT ('node_modules/...'), so the
       compiler has to run from ROOT. */
    const cmd = 'cd /d ' + q(ROOT) + ' && ' + q(MBJSC) + ' -e -m -n ' + q(moduleName) +
                ' -o ' + q(cPath) + ' ' + q(srcPath);
    const r = exec(cmd);
    if (r.code !== 0) {
        throw new Error('mbjsc failed for ' + moduleName + '\n' + r.stdout);
    }
    return bytecodeFromC(readText(cPath));
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

const HOST_C = `/* lean_host.c — generated by tools/aot/build-lean.js.
 *
 * A minimal AOT host: the JavaScript parser is not even linked into this
 * binary (the engine is built with -DMBJS_DISABLE_PARSER).  Every module is
 * pre-compiled to bytecode and registered here before the entry module runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbjs.h"
#include "mbjs-libc.h"

#include "lean_bundle.h"

/* Diagnostic loader: prints the module name the engine asks for, then
   forwards to the engine loader (which handles the native .dll plugins). */
static MBJSModuleDef *lean_module_loader(MBJSContext *ctx, const char *name,
                                         void *opaque, MBJSValue attributes)
{
  extern MBJSModuleDef *mbjs_module_loader(MBJSContext *ctx, const char *name,
                                           void *opaque, MBJSValue attributes);
  fprintf(stderr, "[lean] loader: '%s'\\n", name);
  return mbjs_module_loader(ctx, name, opaque, attributes);
}

static MBJSContext *MBJS_NewCustomContext(MBJSRuntime *rt)
{
  MBJSContext *ctx = MBJS_NewContext(rt);
  size_t k;
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
  /* Register every pre-compiled module under its bundle name.  After this
     the engine resolves "mochibundle:<path>" from the module registry and
     never touches the file system. */
  for (k = 0; k < lean_module_count; k++) {
    fprintf(stderr, "[lean] register %s\\n", lean_modules[k].name);
    mbjs_std_eval_binary(ctx, lean_modules[k].data, lean_modules[k].size, 1);
    if (MBJS_HasException(ctx)) {
      fprintf(stderr, "lean: module %s failed to load\\n", lean_modules[k].name);
      mbjs_std_dump_error(ctx);
    }
  }
  return ctx;
}

int main(int argc, char **argv)
{
  int r;
  MBJSRuntime *rt;
  MBJSContext *ctx;
  MBJSValue globals;

  r = 0;
  rt = MBJS_NewRuntime();
  mbjs_std_set_worker_new_context_func(MBJS_NewCustomContext);
  mbjs_std_init_handlers(rt);
  MBJS_SetModuleLoaderFunc2(rt, NULL, lean_module_loader, mbjs_module_check_attributes, NULL);
  ctx = MBJS_NewCustomContext(rt);
  if (!ctx) {
    fprintf(stderr, "lean: cannot create context\\n");
    return 1;
  }
  globals = MBJS_GetGlobalObject(ctx);
  MBJS_SetPropertyStr(ctx, globals, "__MOCHI_MODULE_NAME__",
                      MBJS_NewCFunction(ctx, lean_module_resolve, "moduleName", 1));
  MBJS_SetPropertyStr(ctx, globals, "__MOCHI_MODULE_EXISTS__",
                      MBJS_NewCFunction(ctx, lean_module_exists, "moduleExists", 1));
  MBJS_FreeValue(ctx, globals);

  mbjs_std_add_helpers(ctx, argc, argv);
  fprintf(stderr, "[lean] eval entry\\n");
  mbjs_std_eval_binary(ctx, lean_entry, lean_entry_size, 0);
  if (MBJS_HasException(ctx)) {
    fprintf(stderr, "lean: entry failed\\n");
    mbjs_std_dump_error(ctx);
  }
  r = mbjs_std_loop(ctx);
  if (r) {
    mbjs_std_dump_error(ctx);
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
    header += hexArray('lean_entry', entryBytes);
    header += 'static const unsigned long lean_entry_size = ' + entryBytes.length + ';\n\n';
    header += `
static MBJSValue lean_module_resolve(MBJSContext *ctx, MBJSValueConst this_val,
                                      int argc, MBJSValueConst *argv)
{
  const char *spec; size_t i;
  if (argc < 1) return MBJS_UNDEFINED;
  spec = MBJS_ToCString(ctx, argv[0]);
  if (!spec) return MBJS_UNDEFINED;
  for (i = 0; i < lean_alias_count; i++) {
    if (!strcmp(lean_aliases[i].spec, spec)) {
      MBJS_FreeCString(ctx, spec);
      return MBJS_NewString(ctx, lean_aliases[i].name);
    }
  }
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
      MBJS_FreeCString(ctx, spec);
      return MBJS_TRUE;
    }
  }
  MBJS_FreeCString(ctx, spec);
  return MBJS_FALSE;
}

#endif /* LEAN_BUNDLE_H */
`;
    writeText(join(WORK, 'lean_bundle.h'), header);
    writeText(join(WORK, 'lean_host.c'), HOST_C);

    const totalBytes = compiled.reduce((n, m) => n + m.bytes.length, 0);
    console.log('');
    console.log('compiled modules : ' + compiled.length);
    console.log('bytecode bytes   : ' + totalBytes + ' (entry ' + entryBytes.length + ')');
    console.log('aliases          : ' + aliases.length);
    console.log('source bytes kept: 0 (JS is not embedded)');
    console.log('written          : ' + join(WORK, 'lean_bundle.h') + ', ' + join(WORK, 'lean_host.c'));
}

main().catch((e) => {
    console.error('build-lean failed:', e && e.message ? e.message : e);
    process.exit(1);
});
