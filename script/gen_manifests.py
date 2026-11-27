#!/usr/bin/env python3
"""
gen_manifests.py — regenerate repo metadata for the SEVM shim layer.

Single source of truth lives in the REGISTRY below (measured against
E:/bcf/mochiberry/build_manual/mbjs.exe at runtime, see README for how the
export lists were obtained). It writes:

  node_modules/<mod>/README.md          docs for each JS-first shim
  node_modules/<mod>/package.json       (created only when missing)
  node_modules/README.md                overview of the JS shim layer
  node_modules/package.json             root manifest listing all JS shims
  mochi_modules/<mod>/{README.md,manifest.json,lib/,native/,include/}
  mochi_modules/<mod>/lib/<mod>_x86_64.dll   (copy of the flat plugin)
  mochi_modules/manifest.json           aggregate native-plugin registry

Run:  python3 script/gen_manifests.py [--force]
"""
import json, os, shutil, sys, datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FORCE = "--force" in sys.argv

# ---------------------------------------------------------------- registry ---
# status: verified (exercised on the engine this session),
#         operational (implemented, network/integration dependent),
#         stub (intentionally throws unsupported),
#         broken (fails to load/init)
NODE_MODULES = [
    ("mochinode",        "Core loader: require(), loadNative(), resolvePackage, BUILTIN_ALIASES passthrough", "verified"),
    ("mochiglobals",     "Installs missing globals: setTimeout/setInterval/clear*, URL, TextEncoder/Decoder, AbortController, structuredClone, fetch, crypto", "verified"),
    ("mochitimers",      "Node timers over mbjs:os, also installed as globals", "verified"),
    ("mochicrypto",      "node:crypto shim: createHash/createHmac/pbkdf2/Cipheriv/randomBytes over mochicrypto.dll", "verified"),
    ("mochizlib",        "node:zlib shim: gzip/gunzip/inflateRaw/deflateRaw/crc32 + transforms over mochizlib.dll (native framing verified vs Python)", "verified"),
    ("mochifs",          "node:fs shim over mochifs.dll with mbjs:std fallback", "verified"),
    ("mochibuffer",      "node:buffer shim (MochiBuffer extends Uint8Array), installs globalThis.Buffer", "verified"),
    ("mochievents",      "node:events shim: EventEmitter", "verified"),
    ("mochipath",        "node:path shim: join/resolve/dirname/basename/normalize", "verified"),
    ("mochiutil",        "node:util shim: format/inspect/promisify", "verified"),
    ("mochistream",      "node:stream shim: Readable/Writable/Transform", "verified"),
    ("mochiurl",         "node:url shim: URL/URLSearchParams (+ SimpleURL fallback)", "verified"),
    ("mochios",          "node:os shim: platform/arch/tmpdir/homedir/cpus/freemem", "verified"),
    ("mochiprocess",     "node:process shim: env/argv/arch/hrtime/uptime + globalThis.process", "verified"),
    ("mochimodule",      "node:module shim: createRequire()", "verified"),
    ("mochisandbox",     "Realm-isolated sandbox: create/run/runAsync/clone/free/count/setGlobal, per-realm intrinsics, memLimit/budgetMs; SECURITY.md", "verified"),
    ("mochiws",          "node:ws / WebSocket shim + httpGet/httpRequest over mochiws.dll (WinHTTP): GET verified; httpRequest(method,url,body,ct) enables POST/PUT/PATCH/DELETE + payload", "verified"),
    ("mochihttp",        "node:http shim: ClientRequest/IncomingMessage; get() over httpGet, request() over httpRequest — POST/PUT/PATCH/DELETE with body verified (201, body echoed)", "verified"),
    ("mochihttps",       "node:https shim: request()/get() delegated to mochihttp (POST/PUT/PATCH inherited)", "verified"),
    ("mochiasync_hooks", "node:async_hooks shim: AsyncLocalStorage (single-context, non-async-correct)", "stub"),
    ("mochitls",         "node:tls shim: TLSSocket marker only, connect() unsupported", "stub"),
    ("mochinet",         "node:net shim: Socket/createServer markers; connect() emits MOCHI_UNSUPPORTED (client connect needs a mochinode_net export)", "stub"),
    ("mochidiagnostics_channel", "node:diagnostics_channel shim: no-op channel()", "stub"),
    ("mochiperf_hooks",  "node:perf_hooks shim: performance passthrough + no-op PerformanceObserver", "stub"),
    ("mochiworker_threads", "node:worker_threads shim: Worker throws unavailable, isMainThread=true", "stub"),
    ("mochijsnative",    "AOT/JIT + WASM native bridge (eval/jit*/runtime*/value*/context*, hostBase/hostExport/lastError) — 51 exports verified", "verified"),
    ("mochijsnative_stub", "fallback for mochijsnative when native unavailable", "operational"),
    ("dynalib",          "JS API for libdynalib (dynalib(file, offset, symbol, abi, opts))", "operational"),
    ("events",           "stand-in node:events for the express/net stack", "operational"),
    ("express",          "Express-compatible router: middleware chain, path params, query, json/urlencoded/text body, static + traversal guard, res helpers, template engines (mini-EJS), vhost, app.param, error middleware", "verified"),
    ("fs",               "node:fs entry re-exported from mochifs", "operational"),
    ("net",              "node:net entry over mochinode_net.dll", "operational"),
    ("sqlite",           "node:sqlite-style shim over mochinode_sqlite.dll (bind parameters not wired)", "operational"),
]

# Machine-measured JS export lists (mbjs loadNative(...) keys, sorted)
MOCHI_MODULES = [
    ("mochicrypto", "OpenSSL-grade hash/HMAC/PBKDF2/AES primitives, C99 over the engine host image",
     ["aesCbcDecryptHex","aesCbcEncryptHex","aesCtrCryptHex","aesGcmDecryptHex","aesGcmEncryptHex",
      "hmacSha1Hex","hmacSha256Hex","hmacSha512Hex","md5Hex","pbkdf2Hex","randomHex","sha1Hex",
      "sha256Hex","sha384Hex","sha512Hex","timingSafeEqual"], "verified"),
    ("mochifs", "Filesystem primitives (read/write/stat/readdir/copy/rename/remove/mkdir) via host image",
     ["appendText","copyFile","cwd","exists","mkdir","readText","readdir","remove","rename","stat","writeText"], "verified"),
    ("mochizlib", "zlib primitives: gzipHex/gunzipHex/inflateRawHex/deflateRawHex/crc32Hex + legacy deflate/inflate (verified vs Python oracle)",
     ["crc32Hex","deflateHex","deflateRawHex","gunzipHex","gzipHex","inflateHex","inflateRawHex"], "verified"),
    ("mochinode_net", "Blocking TCP/HTTP server primitives over Winsock2 (httpServer/httpAccept/httpRead/httpWrite/httpClose)",
     ["httpAccept","httpClose","httpRead","httpServer","httpWrite"], "operational"),
    ("mochinode_sqlite", "SQLite bridge (libsqlite3/sqlite3.dll). Parameterized statements: bindText/bindInt/bindDouble/bindNull/reset/clearBindings/bindParameterCount",
     ["bindDouble","bindInt","bindNull","bindParameterCount","bindText","clearBindings","close",
      "columnBlobHex","columnCount","columnDouble","columnInt","columnName","columnText","columnType",
      "errmsg","exec","finalize","open","prepare","reset","step","version"], "verified"),
    ("mochinode_dynalib", "Bridge from the embedded C99 libdynalib to JS (open/resolve/call symbols in any DLL)",
     ["abiParse","callSymbol","closeModule","freeSymbol","lastError","moduleBase","moduleExports","openModule",
      "resolveSymbol","symbolAbi","symbolBase","symbolOffset","symbolPtr"], "operational"),
    ("mochiws", "WebSocket client + WinHTTP GET/POST (connect/poll/send/close/httpGet/httpRequest)",
     ["close","connect","httpGet","httpRequest","poll","send"], "verified"),
    ("mochiwasmy", "WebAssembly bridge — PING only; no __wbg_* imports or global bridge yet",
     ["ping"], "stub"),
    ("mochijsnative", "AOT/JIT + WASM native bridge (eval, jit*, runtime*, value*, context*, hostBase/hostExport/lastError)",
     ["abi","await","bind","call","callValue","contextFree","contextNew","contextStd","detectModule","dumpError",
      "engineVersion","eval","evalModule","exportNames","exportSignature","getException","getGlobal","getProp",
      "hostBase","hostExport","jitCall","jitCleanup","jitCompile","jitFree","jitInfo","jitInvoke","jitNew","jitStats",
      "jsonParse","lastError","loop","runtimeFree","runtimeLimits","runtimeNew","setProp","unbind","valueFree",
      "valueIsException","valueJson","valueNew","valueToInt","valueToNumber","valueToString","valueType","version"], "verified"),
    ("mochisandbox", "Realm-isolated sandbox runtime (create/run/runAsync/clone/free/count/setGlobal) with per-realm intrinsic isolation",
     ["clone","count","create","free","run","runAsync","setGlobal"], "verified"),
]


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def write_json(path, obj):
    write(path, json.dumps(obj, indent=2, ensure_ascii=False) + "\n")


def read_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def pkg(name, desc, extra=None):
    base = {"name": name, "version": "0.1.0", "type": "module", "main": "index.js", "license": "MIT"}
    if desc:
        base["description"] = desc
    if extra:
        base.update(extra)
    return base


STATUS_BADGE = {
    "verified": "verified — exercised against mbjs.exe",
    "operational": "operational — implemented, integration/network dependent",
    "stub": "stub — intentionally unsupported on MochiBerry",
    "broken": "broken — fails to load/init on the current engine build",
}


# -------------------------------------------------------------- node_modules ---
def gen_node_modules():
    for name, desc, status in NODE_MODULES:
        d = os.path.join(ROOT, "node_modules", name)
        if os.path.isdir(d):
            p = os.path.join(d, "package.json")
            if not os.path.exists(p):
                write_json(p, pkg(name, desc))
            r = os.path.join(d, "README.md")
            if FORCE or not os.path.exists(r):
                write(r, module_readme(name, desc, status))
    # aggregate node_modules manifest
    agg = {
        "name": "sevm-node-shims",
        "version": "0.1.0",
        "description": "JS-first Node.js shim layer for the MochiBerry engine (SEVM stack).",
        "type": "module",
        "private": True,
        "generated": _now(),
        "orientations": {
            "pure-js": ["events"],
            "hybrid": [n for n, _, s in NODE_MODULES if n not in ("events",)],
        },
        "modules": [
            {"name": n, "description": d, "status": s,
             "exports": SOURCE.get(n, {}).get("js")}
            for n, d, s in NODE_MODULES
        ],
    }
    write_json(os.path.join(ROOT, "node_modules", "package.json"), agg)
    write(os.path.join(ROOT, "node_modules", "README.md"), node_index_readme())


def module_readme(name, desc, status):
    js = SOURCE.get(name, {}).get("js")
    lines = ["# node_modules/%s" % name, "", desc, "", "- **Orientation**: JS-first (Node.js NPM style)",
             "- **Status**: %s" % STATUS_BADGE.get(status, status), ""]
    if js:
        lines += ["## Exports", "", "```js"]
        lines += ["- `%s`" % e for e in js]
        lines += ["```", ""]
    lines += ["Maintained by the SEVM project. Regenerated by `script/gen_manifests.py`.", ""]
    return "\n".join(lines)


def node_index_readme():
    out = ["# node_modules — JS-first Node.js shim layer", "",
           "NPM-style packages that give the MochiBerry engine a Node.js shape.",
           "Dominant orientation: **JS/script/framework** — but a package may still",
           "call native code (`loadNative()` / `dynalib()`) for the heavy parts.", "",
           "Sibling layer: [`../mochi_modules`](../mochi_modules) = native engine plugins.",
           "",
           "| module | status | description |", "|---|---|---|"]
    for n, d, s in NODE_MODULES:
        out.append("| `%s` | %s | %s |" % (n, s, d))
    out += ["", "## Usage", "", "```js",
            "const { require } = await import('./node_modules/mochinode/index.js');",
            "const crypto = await require('node:crypto');   // -> mochicrypto",
            "const ws = await require('mochiws');", "```", "",
            "`require()` calls `ensureGlobals()` first, so `setTimeout`, `URL`,",
            "`fetch`, `TextEncoder`, `crypto`, etc. are installed automatically.", "",
            "Generated by `script/gen_manifests.py` — see AGENTS.md for the rules.", ""]
    return "\n".join(out)


# -------------------------------------------------------------- mochi_modules ---
def gen_mochi_modules():
    modules = []
    for name, desc, exports, status in MOCHI_MODULES:
        d = os.path.join(ROOT, "mochi_modules", name)
        os.makedirs(os.path.join(d, "lib"), exist_ok=True)
        os.makedirs(os.path.join(d, "native"), exist_ok=True)
        os.makedirs(os.path.join(d, "include"), exist_ok=True)
        os.makedirs(os.path.join(d, "script"), exist_ok=True)

        # copy the engine-loadable plugin into lib/ with the arch-suffixed name
        src_dll = os.path.join(ROOT, "mochi_modules", name + ".dll")
        if os.path.exists(src_dll):
            dst = os.path.join(d, "lib", "%s_x86_64.dll" % name)
            if FORCE or not os.path.exists(dst):
                shutil.copy2(src_dll, dst)
        # copy the C source (keep native/ at root as the legacy flat location)
        src_c = os.path.join(ROOT, "native", name + ".c")
        if os.path.exists(src_c):
            dst = os.path.join(d, "native", name + ".c")
            if FORCE or not os.path.exists(dst):
                shutil.copy2(src_c, dst)

        guard = "MB_%s_ABI_H" % name.upper().replace("-", "_")
        write(os.path.join(d, "include", "%s_abi.h" % name),
              "#ifndef %s\n#define %s\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
              "/* ABI for %s — engine plugin. Entry point: mbjs_init_module().\n"
              "   Runtime exports (verified via mbjs loadNative): */\n"
              % (guard, guard, name) +
              "".join("/*   - %s */\n" % e for e in exports) +
              "#ifdef __cplusplus\n}\n#endif\n#endif\n")
        write(os.path.join(d, "README.md"), mochi_readme(name, desc, exports, status))
        write_json(os.path.join(d, "manifest.json"),
                   mochi_manifest(name, desc, exports, status))
        modules.append({"name": name, "description": desc, "exports": exports, "status": status,
                        "path": "mochi_modules/%s" % name})
    write_json(os.path.join(ROOT, "mochi_modules", "manifest.json"), {
        "name": "sevm-mochi-modules",
        "version": "0.1.0",
        "description": "Native engine plugins for MochiBerry (SEVM). Native-first: DLL/so/dylib.",
        "generated": _now(),
        "abi": {
            "entry_point": "mbjs_init_module",
            "engine_symbol_resolution": "native/mbjs_runtime.h (host image symbols)",
            "forbidden": "plugin must not import mbjs.exe directly (Windows error 126)",
            "throw_contract": "C functions that throw MUST return MBJS_EXCEPTION (not MBJS_UNDEFINED)",
        },
        "platform_bins": {"windows": "_x86_64.dll", "linux": "_x86_64.so / _aarch64.so",
                          "darwin": "_x86_64.dylib / _arm64.dylib"},
        "engine": "E:/bcf/mochiberry/build_manual/mbjs.exe",
        "modules": modules,
    })


def mochi_manifest(name, desc, exports, status):
    return {
        "name": name,
        "version": "0.1.0",
        "type": "native-engine-plugin",
        "description": desc,
        "status": status,
        "entry_point": "mbjs_init_module",
        "runtime_exports": exports,
        "sources": ["native/%s.c" % name],
        "headers": ["include/%s_abi.h" % name],
        "binaries": {"x86_64-windows": "lib/%s_x86_64.dll" % name},
        "js_binding": "node_modules/%s" % name,
        "engine": "E:/bcf/mochiberry/build_manual/mbjs.exe",
    }


def mochi_readme(name, desc, exports, status):
    out = ["# mochi_modules/%s" % name, "", desc, "",
           "- **Orientation**: native-first (engine plugin, loaded by the engine)",
           "- **Status**: %s" % STATUS_BADGE.get(status, status),
           "- **Entry point**: `mbjs_init_module(ctx, name)`", "",
           "## Layout", "", "```",
           "mochi_modules/%s/" % name, "├── README.md", "├── manifest.json",
           "├── native/%s.c" % name, "├── include/%s_abi.h" % name,
           "├── script/", "└── lib/%s_x86_64.dll" % name, "```", ""]
    out += ["## Runtime exports (JS-visible, measured on mbjs.exe)", ""]
    if exports:
        out += ["```js"] + ["- `%s`" % e for e in exports] + ["```", ""]
    else:
        out += ["_none — module fails to initialize on the current engine build._", ""]
    out += ["## JS binding", "",
            "`node_modules/%s/index.js` loads the binary via `mochinode.loadNative('%s.dll')`." % (name, name), "",
            "The flat file `mochi_modules/%s.dll` is kept for loader/legacy " % name,
            "compatibility; `lib/%s_x86_64.dll` is the distribution copy with " % name,
            "arch-suffixed naming.", "",
            "Rebuild: see `native/` script in the root `mochi_modules/README.md`.",
            "Generated by `script/gen_manifests.py`.", ""]
    return "\n".join(out)


def _now():
    return datetime.datetime.now().strftime("%Y-%m-%d")


# ----------------------------------------------------------- JS export facts ---
# Side-effect public globals a module installs on load.
SOURCE = {
    "mochiglobals": {"js": ["setTimeout", "setInterval", "URL", "TextEncoder", "AbortController", "fetch", "crypto"]},
    "mochicrypto": {"js": ["createCipheriv", "createDecipheriv", "createHash", "createHmac",
                           "pbkdf2Sync", "randomBytes", "timingSafeEqual", "subtle", "webcrypto"]},
    "mochizlib":   {"js": ["deflateSync", "gunzipSync", "gzipSync", "inflateSync", "unzipSync", "crc32"]},
    "mochifs":     {"js": ["appendFileSync", "copyFileSync", "existsSync", "mkdirSync",
                           "readFileSync", "readdirSync", "renameSync", "statSync", "writeFileSync"]},
    "mochievents": {"js": ["EventEmitter", "once", "on"]},
    "mochiurl":    {"js": ["URL", "URLSearchParams", "fileURLToPath", "pathToFileURL"]},
    "mochistream": {"js": ["Readable", "Writable", "Transform", "Duplex", "PassThrough"]},
    "mochibuffer": {"js": ["Buffer", "MochiBuffer"]},
    "mochinode":   {"js": ["require", "loadNative", "resolvePackage", "resolveNativePath", "join", "resolvePath"]},
    "mochitimers": {"js": ["setTimeout", "clearTimeout", "setInterval", "clearInterval"]},
    "mochihttp":   {"js": ["request", "get", "ClientRequest", "IncomingMessage", "STATUS_CODES", "METHODS"], "capabilities": ["GET", "POST", "PUT", "PATCH", "DELETE", "request-body"]},
    "mochiws":     {"js": ["WebSocket", "connect", "send", "poll", "close", "httpGet", "httpRequest"]},
    "mochihttps":  {"js": ["request", "get"], "capabilities": ["GET", "POST", "PUT", "PATCH", "DELETE"]},
    "mochisandbox": {"js": ["sandbox", "Sandbox", "count", "load", "isNativeReady"]},
}


def main():
    gen_node_modules()
    gen_mochi_modules()
    print("[OK] node_modules manifests + READMEs")
    print("[OK] mochi_modules per-module folders + manifests")


if __name__ == "__main__":
    main()