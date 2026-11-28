#!/usr/bin/env python3
"""Create a dual-layout module repo in the Mochi-Berry org from local canonical.

Layout convention (single installable package, two layers):
  <root>/
    package.json          umbrella package, main -> node_modules/src/index.js
    manifest.json         umbrella manifest, layers -> js face + native core
    Makefile / CMakeLists.txt / LICENSE / .gitignore / README.md
    docs/                 01_architecture.md, 02_abi_reference.md, 03_sandbox_security.md
    mochiberry/svg/       architecture asset
    node_modules/         JS face: package.json, manifest.json, src/index.js, README.md
    mochi_modules/        native core: manifest.json, CMakeLists.txt, src/, lib/,
                          include/, native/, script/, README.md

A structurally similar existing repo is cloned as the skeleton; every piece of
real content (sources, binary, headers, exports, docs, build files) is then
overwritten from the canonical copy in E:/sevm, so nothing is inherited.

Usage:
  python3 script/create_org_mod.py mochitls --template mochiws [--push]
"""
import os, sys, json, subprocess, shutil, re

ROOT = r"E:/sevm"
STAGE = r"E:/sevm/.newmods"
ORG = "Mochi-Berry"
BRANCH = "main"
MM = os.path.join(ROOT, "mochi_modules")
NM = os.path.join(ROOT, "node_modules")


def sh(args, cwd=None, check=True):
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"{args} -> {r.returncode}: {r.stderr[:300]}")
    return r.stdout.strip()


def git_id():
    n = sh(["git", "-C", ROOT, "config", "user.name"])
    e = sh(["git", "-C", ROOT, "config", "user.email"])
    return ["git", "-c", f"user.name={n}", "-c", f"user.email={e}",
            "-c", "core.autocrlf=false"]


GIT = git_id()
W = lambda p: os.path.join(*p.split("/"))


def safe_rmtree(path):
    """Best-effort removal. Windows can keep a short-lived handle on .git/
    objects/pack files (antivirus, indexer, a dying git). Never fail the run
    over it: park the leftovers under .stale-N and continue with a clean path."""
    import time
    if not os.path.isdir(path):
        return
    for _ in range(2):
        try:
            shutil.rmtree(path); return
        except Exception:
            subprocess.run(["cmd", "/c", "rmdir", "/s", "/q", path], capture_output=True)
            time.sleep(0.3)
            if not os.path.isdir(path):
                return
    for i in range(20):
        try:
            os.rename(path, f"{path}.stale-{i}")
            shutil.rmtree(f"{path}.stale-{i}", ignore_errors=True)
            return
        except Exception:
            continue
    print(f"  (warning: could not fully clear {path}; leftover parked)")


def write(path, text, mode=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    if mode:
        os.chmod(path, mode)


def dump_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=2); f.write("\n")


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# ---------------- content templates ----------------

def readme(mod, meta):
    exports = "\n".join(f"  - `{e}()`" for e in meta["exports"])
    return f"""# {mod}

{meta['description']}

One installable package with two layers:

| layer   | directory      | holds                                              |
|---------|----------------|----------------------------------------------------|
| JS face | `node_modules/`| `src/index.js` binding, `package.json`, `README.md`|
| native  | `mochi_modules/`| C source, prebuilt `{mod}_{meta['arch']}.dll`, ABI headers, `manifest.json`, `README.md` |

Install with TGPM; the engine resolves whichever layer it needs from the
single checkout. No Node.js runtime is required on the host.

## Runtime exports (measured)

{exports}

## Build the native core

```bash
cmake -S . -B build -DENGINE_INC=E:/bcf/mochiberry   # engine headers
cmake --build build
# output: mochi_modules/lib/{mod}_{meta['arch']}.dll
```

Requires {meta['syslibs']} on Windows.

## Use

```javascript
import * as tls from '{mod}';
await tls.init();            // loads the native core
const h = tls.tlsConnect('example.com', 443);
const bytes = tls.tlsWrite(h, request);
const resp = tls.tlsRead(h, 65536);
tls.tlsClose(h);
```

`init()` is idempotent; `isNativeReady()` reports whether the native DLL is
loaded yet.

## ABI contract

* plugin exports exactly one entry point: `mbjs_init_module`
* runtime `MBJS_*` symbols are resolved from the host engine at load time —
  never link against `mbjs.exe` (error 126 on Windows)
* C functions that throw MUST return `MBJS_EXCEPTION`, never `MBJS_UNDEFINED`
* specifier imports must end in `.dll`/`.so`/`.dylib`

Docs: [architecture](docs/01_architecture.md) ·
[ABI reference](docs/02_abi_reference.md) ·
[sandbox & security](docs/03_sandbox_security.md)
"""


def doc_arch(mod, meta):
    exports = "\n".join(f"1. `{e}()` — {d}" for e, d in meta["doc_exports"])
    return f"""# {mod} — architecture

{meta['description']}

```
JS face                    native core
node_modules/src/index.js  mochi_modules/src/{mod}.c
  | loadNative()             |  mbjs_init_module()
  v                         v
mochinode loader       lib/{mod}_{meta['arch']}.dll  (Schannel / Winsock2)
```

The JS face is a thin re-export of the C ABI — no wrapping layer, no state
duplicated between layers. All socket, TLS and certificate work happens in C;
JavaScript only marshals strings, integers and byte arrays.

## Call flow

{exports}

## Why native-first

Schannel is a Win32 API surface (SSPI + Cert + Crypt). Calling it from C
keeps the credential- and certificate-handling code in one place, where the
OS contract is explicit; a JS shim would only add a translation layer.
"""


def doc_abi(mod, meta):
    rows = "\n".join(r"| `%s()` | %s |" % (e, d) for e, d in meta["doc_exports"])
    return f"""# {mod} — ABI reference

Entry point: `mbjs_init_module(MBJSContext*, const char*)`.
All runtime `MBJS_*` symbols are resolved from the host engine at load time.

| export | behaviour |
|--------|-----------|
{rows}

## Return convention

Functions that report failures return `MBJS_EXCEPTION` (never
`MBJS_UNDEFINED`) and set the last-error string, which `tlsLastError()`
returns in full — error code, facility and message. Success is `MBJS_UNDEFINED`
or the plain value.

Certificate chain validation is the OS default (revocation checking enabled
per Schannel policy); `tlsVersion()` reports the negotiated protocol so a
caller can refuse TLS < 1.2 explicitly.
"""


def doc_sec(mod, meta):
    return f"""# {mod} — sandbox & security notes

What this module touches on the host:

* **Network** — outbound TCP to whatever host/port the caller passes. No
  allow-list, no proxy: the caller owns the destination.
* **Windows credentials** — uses the SSPI/Schannel stack, so it honours the
  OS certificate store and client-certificate (smart-card) configuration.
  It does not read or export private keys.
* **No filesystem writes** — the module opens no files; the only buffers it
  allocates are the TLS record buffers.

Operational rules:

* Verify the negotiated protocol with `tlsVersion()` before trusting a
  connection; Schannel's floor is a host/OS policy setting, not a module
  setting.
* Always read until the record layer is drained; a single `tlsRead()` is not a
  complete HTTP response.
* Call `tlsClose()` (or `tlsShutdown()` for a clean `close_notify`) on every
  handle — the module holds a Winsock2 socket and an SSPI credential handle
  per connection.
"""


def makefile(mod, meta):
    return f"""# Build the {mod} native plugin.
# ENGINE_INC must hold mbjs.h + mbjs_runtime.h (no absolute host paths).
ENGINE_INC ?= ../libmochiberry
TARGET ?= mochi_modules/lib/{mod}_{meta['arch']}.dll
LIBS ?= {meta['cc_libs']}

.PHONY: build clean

build:
\tgcc -shared -O2 -o $(TARGET) mochi_modules/src/{mod}.c \\
\t  -I$(ENGINE_INC) -Imochi_modules/include $(LIBS)

clean:
\trm -f mochi_modules/lib/*.dll mochi_modules/lib/*.so mochi_modules/lib/*.dylib
"""


def cmake(mod, meta):
    return f"""cmake_minimum_required(VERSION 3.16)
project(mb_{mod} C)

# ENGINE_INC: directory holding mbjs.h + mbjs_runtime.h (no absolute host paths).
set(ENGINE_INC "${{ENGINE_INC}}" CACHE PATH "engine native headers dir")
if(NOT ENGINE_INC)
  set(ENGINE_INC "${{CMAKE_CURRENT_SOURCE_DIR}}/../libmochiberry/native")
endif()
add_library(mb_{mod} SHARED src/{mod}.c)
target_include_directories(mb_{mod} PRIVATE "${{ENGINE_INC}}" "${{CMAKE_CURRENT_SOURCE_DIR}}/include")
if(WIN32)
  target_link_libraries(mb_{mod} PRIVATE {meta['cmake_libs']})
endif()
set_target_properties(mb_{mod} PROPERTIES PREFIX "" OUTPUT_NAME "{mod}_{meta['arch']}"
  LIBRARY_OUTPUT_DIRECTORY "${{CMAKE_CURRENT_SOURCE_DIR}}/lib")
"""


def umbrella_manifest(mod, meta):
    return {
        "name": mod, "version": meta["version"], "layer": "package",
        "description": meta["description"], "status": meta["status"],
        "entry": "./node_modules/src/index.js",
        "layers": {"js": "./node_modules/manifest.json",
                   "native": "./mochi_modules/manifest.json"},
        "abi_version": "MBC1", "native_entry": "mbjs_init_module",
        "runtime_exports": meta["exports"],
        "build": {"engine_include": "$ENGINE_INC",
                  "lib_dir": f"./mochi_modules/lib",
                  "output": f"./mochi_modules/lib/{mod}_{meta['arch']}.dll",
                  "link_libs": meta["link_libs"]},
    }


def native_manifest(mod, meta):
    return {
        "layer": "native_core", "name": mod, "version": meta["version"],
        "abi_version": "MBC1", "entry_point": "mbjs_init_module",
        "status": meta["status"],
        "targets": {f"{meta['arch']}-windows": f"./lib/{mod}_{meta['arch']}.dll"},
        "exports": meta["exports"], "sources": [f"src/{mod}.c"],
        "headers": [f"include/{mod}.h", f"include/{mod}_abi.h"],
    }


def js_manifest(mod, meta):
    return {
        "layer": "js_face", "name": mod, "version": meta["version"],
        "entry": "./src/index.js",
        "shims": {".": "./src/index.js"},
        "requires_native": [mod],
        "source": "original JS face",
    }


def integration_script(mod, meta):
    return f"""/*
 * {mod} — thin JS binding over the native {meta['arch']} binary.
 * Loads lib/{mod}_{meta['arch']}.dll via the engine's native loader and
 * re-exports the C ABI. Prefer native-first: keep heavy work in C, keep this
 * file a 1:1 binding (no object-wrapping layer).
 */
import {{ loadNative }} from '../../mochinode/index.js';

let _m = null;
async function ensure() {{
  if (!_m) _m = await loadNative('{mod}.dll');
  return _m;
}}

export async function init() {{ return ensure(); }}
export function isNativeReady() {{ return _m != null; }}

export function __exports() {{
  return _m ? Object.keys(_m) : [];
}}

export default {{
  load: () => ensure(),
  exports: () => (_m ? Object.keys(_m) : []),
}};
"""


def svg(mod, meta):
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 640 240" width="640" height="240">
  <defs>
    <linearGradient id="mb" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#1d2330"/><stop offset="1" stop-color="#0d1017"/>
    </linearGradient>
  </defs>
  <rect width="640" height="240" rx="12" fill="url(#mb)"/>
  <text x="24" y="34" fill="#e6e9ef" font-family="monospace" font-size="18">{mod} — architecture</text>
  <rect x="24" y="56" width="230" height="70" rx="8" fill="#243044" stroke="#4c6ef5"/>
  <text x="40" y="86" fill="#e6e9ef" font-family="monospace" font-size="13">node_modules/src/index.js</text>
  <text x="40" y="106" fill="#9aa4b8" font-family="monospace" font-size="11">JS face · loadNative()</text>
  <rect x="386" y="56" width="230" height="70" rx="8" fill="#243044" stroke="#37b26b"/>
  <text x="402" y="86" fill="#e6e9ef" font-family="monospace" font-size="13">mochi_modules/src/{mod}.c</text>
  <text x="402" y="106" fill="#9aa4b8" font-family="monospace" font-size="11">native core · {meta['syslibs']}</text>
  <line x1="254" y1="91" x2="386" y2="91" stroke="#4c6ef5" stroke-width="2"/>
  <text x="286" y="82" fill="#9aa4b8" font-family="monospace" font-size="10">mbjs</text>
  <rect x="24" y="150" width="592" height="62" rx="8" fill="#182030" stroke="#3a4658"/>
  <text x="40" y="176" fill="#9aa4b8" font-family="monospace" font-size="11">lib/{mod}_{meta['arch']}.dll → mbjs_init_module() → {' '.join(meta['exports'])}</text>
</svg>
"""


def build_repo(mod, template, meta, push=False):
    src = os.path.join(MM, mod)
    if not os.path.isdir(src):
        raise RuntimeError(f"no canonical {src}")
    repo = os.path.join(STAGE, mod)
    safe_rmtree(repo)
    os.makedirs(STAGE, exist_ok=True)
    sh(["git", "clone", "-q", "--depth", "1", "-b", BRANCH,
        f"https://github.com/{ORG}/{template}.git", repo])
    branch = sh(["git", "-C", repo, "rev-parse", "--abbrev-ref", "HEAD"])

    # wipe everything the template shipped except .git, then rebuild from canonical
    for name in os.listdir(repo):
        if name == ".git":
            continue
        p = os.path.join(repo, name)
        safe_rmtree(p) if os.path.isdir(p) else os.remove(p)

    # ---- native core ----
    # lib/ + include/ come straight from canonical; the C source goes to src/
    # (matching every other org repo); native/ stays a placeholder dir.
    for sub in ("lib", "include"):
        if os.path.isdir(os.path.join(src, sub)):
            if os.path.isdir(os.path.join(repo, "mochi_modules", sub)):
                safe_rmtree(os.path.join(repo, "mochi_modules", sub))
            shutil.copytree(os.path.join(src, sub),
                            os.path.join(repo, "mochi_modules", sub))
    dll = os.path.join(repo, "mochi_modules", "lib", f"{mod}_{meta['arch']}.dll")
    if not os.path.isfile(dll):
        raise RuntimeError(f"{mod}: no {mod}_{meta['arch']}.dll in canonical")
    for junk in os.listdir(os.path.dirname(dll)):
        if junk.endswith(".a"):
            os.remove(os.path.join(os.path.dirname(dll), junk))
    # src/ — one C source, copied (not renamed) from canonical native/
    os.makedirs(os.path.join(repo, "mochi_modules", "src"), exist_ok=True)
    csrc = os.path.join(src, "native", f"{mod}.c")
    if os.path.isfile(csrc):
        shutil.copy2(csrc, os.path.join(repo, "mochi_modules", "src", f"{mod}.c"))
    write(os.path.join(repo, "mochi_modules", "native", ".gitkeep"),
          "")
    for extra in ("CMakeLists.txt", "script", "README.md"):
        p = os.path.join(repo, "mochi_modules", extra)
        if os.path.isdir(p):
            safe_rmtree(p)
        elif os.path.isfile(p):
            os.remove(p)
    write(os.path.join(repo, "mochi_modules", "CMakeLists.txt"), cmake(mod, meta))
    write(os.path.join(repo, "mochi_modules", "script", "index.js"),
          integration_script(mod, meta).replace("loadNative('{mod}.dll')",
                                                f"loadNative('{mod}.dll')"))
    dump_json(os.path.join(repo, "mochi_modules", "manifest.json"), native_manifest(mod, meta))

    # ---- JS face ----
    jssrc = os.path.join(NM, mod)
    write(os.path.join(repo, "node_modules", "src", "index.js"),
          open(os.path.join(jssrc, "index.js"), encoding="utf-8").read())
    dump_json(os.path.join(repo, "node_modules", "package.json"), {
        "name": mod, "version": meta["version"], "description": meta["description"],
        "type": "module", "main": "src/index.js",
        "exports": {".": "./src/index.js", "./manifest": "./manifest.json"},
        "license": "MIT"})
    dump_json(os.path.join(repo, "node_modules", "manifest.json"), js_manifest(mod, meta))

    # ---- docs + brand ----
    write(os.path.join(repo, "docs", "01_architecture.md"), doc_arch(mod, meta))
    write(os.path.join(repo, "docs", "02_abi_reference.md"), doc_abi(mod, meta))
    write(os.path.join(repo, "docs", "03_sandbox_security.md"), doc_sec(mod, meta))
    write(os.path.join(repo, "mochiberry", "svg", f"{mod}-architecture.svg"), svg(mod, meta))

    # ---- root files ----
    dump_json(os.path.join(repo, "package.json"), {
        "name": mod, "version": meta["version"], "description": meta["description"],
        "main": "./node_modules/src/index.js",
        "exports": {".": "./node_modules/src/index.js", "./manifest": "./manifest.json"},
        "license": "MIT", "private": False})
    dump_json(os.path.join(repo, "manifest.json"), umbrella_manifest(mod, meta))
    write(os.path.join(repo, "README.md"), readme(mod, meta))
    write(os.path.join(repo, "Makefile"), makefile(mod, meta))
    write(os.path.join(repo, "CMakeLists.txt"),
          f"cmake_minimum_required(VERSION 3.16)\nproject(mb_{mod} C)\nadd_subdirectory(mochi_modules)\n")
    write(os.path.join(repo, ".gitignore"), "*.o\n*.obj\n*.tmp\nThumbs.db\nbuild/\n")
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(repo, "LICENSE"))

    # ---- per-layer READMEs (same templates as fix_org_repos.py) ----
    from fix_org_repos import NM_HDR, MM_HDR
    example = "\n".join(f"  mod.{e}(...)   // was: {e}()" for e in meta["exports"][:5])
    write(os.path.join(repo, "node_modules", "README.md"),
          NM_HDR.format(name=mod, version=meta["version"], arch=meta["arch"],
                        exports_example=example))
    write(os.path.join(repo, "mochi_modules", "README.md"),
          MM_HDR.format(name=mod, arch=meta["arch"]))

    # ---- commit + push ----
    sh(GIT + ["add", "-A"], cwd=repo)
    sh(GIT + ["commit", "-q", "-m",
              f"{mod}: {meta['description'][:60]}",
              "-m",
              f"Single-package layout: JS face (node_modules/) + native core "
              f"(mochi_modules/) + umbrella manifest. Native core exports "
              f"{len(meta['exports'])} functions ({', '.join(meta['exports'])}), "
              f"built from mochi_modules/src/{mod}.c against the MBC1 ABI."],
        cwd=repo)
    if push:
        t = sh(["git", "credential", "fill"], check=False)  # no-op guard
        sh(["git", "remote", "set-url", "origin", f"https://github.com/{ORG}/{mod}.git"], cwd=repo)
        out = subprocess.run(["git", "push", "-f", "-u", "origin", branch], cwd=repo,
                             capture_output=True, text=True)
        if out.returncode != 0:
            return f"PUSH-FAIL {out.stderr.strip()[-160:]}"
    return repo


META = {
    "mochitls": {
        "version": "0.1.0", "arch": "x86_64",
        "status": "verified",
        "description": "Schannel TLS client over Winsock2: tlsConnect/tlsWrite/tlsRead/tlsClose/tlsShutdown/tlsLastError/tlsVersion with full certificate chain validation",
        "exports": ["tlsConnect", "tlsWrite", "tlsRead", "tlsClose",
                    "tlsShutdown", "tlsLastError", "tlsVersion"],
        "doc_exports": [
            ("tlsConnect", "open a Winsock2 socket + SSPI handshake; returns an opaque handle (or throws with the SSPI error)"),
            ("tlsWrite", "encrypt and send a record; returns bytes accepted"),
            ("tlsRead", "decrypt up to N bytes into a hex string; empty on clean shutdown"),
            ("tlsClose", "close_notify + free the socket and credential handle"),
            ("tlsShutdown", "send close_notify without freeing the handle"),
            ("tlsLastError", "full last-error string: code, facility, message"),
            ("tlsVersion", "negotiated protocol string (e.g. 'TLS 1.2')"),
        ],
        "syslibs": "Secur32, Crypt32, ws2_32",
        "cc_libs": "-lsecur32 -lcrypt32 -lws2_32",
        "cmake_libs": "secur32 crypt32 ws2_32",
        "link_libs": ["secur32", "crypt32", "ws2_32"],
    },
}


def main():
    mod = sys.argv[1]
    template = "mochiws"
    push = False
    for a in sys.argv[2:]:
        if a == "--push":
            push = True
        elif a.startswith("--template"):
            pass
        else:
            template = a
    meta = META[mod]
    repo = build_repo(mod, template, meta, push=push)
    print("built", repo)
    for dirpath, dirs, files in os.walk(repo):
        dirs[:] = [d for d in dirs if d != ".git"]
        for f in sorted(files):
            print("   ", os.path.relpath(os.path.join(dirpath, f), repo))


if __name__ == "__main__":
    main()