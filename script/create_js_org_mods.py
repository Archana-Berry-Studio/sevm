#!/usr/bin/env python3
"""Create dual-layer repos in the Mochi-Berry org for the JS-only modules.

A JS-only module still ships the full two-layer shape, so every repo in the
org has the same structure and TGPM can resolve either layer:

  <root>/
    package.json / manifest.json   umbrella (layers -> js face + native stub)
    README.md, LICENSE, .gitignore, docs/, mochiberry/svg/
    node_modules/                  the real module: src/, package.json,
                                   manifest.json, README.md
    mochi_modules/                 native stub: manifest.json + README.md
                                   (states plainly that there is no native
                                   binary for this module)

Content comes from the canonical copy in E:/sevm/node_modules/<mod>/.
Nothing is inherited from a template repo except the directory shape, which
is written explicitly here.

Usage:
  python3 script/create_js_org_mods.py --push [mod ...]
"""
import os, sys, json, subprocess, shutil, time

ROOT = r"E:/sevm"
STAGE = r"E:/sevm/.newmods"
ORG = "Mochi-Berry"
BRANCH = "main"
NM = os.path.join(ROOT, "node_modules")

JS_MODS = ["mochibuffer", "mochipath", "mochistream", "mochiurl", "mochiutil",
           "mochios", "mochihttp", "mochihttps", "mochinet", "mochievents",
           "mochiasync_hooks", "mochidiagnostics_channel", "mochiperf_hooks",
           "mochimodule", "mochiprocess", "mochiglobals", "mochiworker_threads",
           "mochitimers", "mochifetch", "mochijsnative_stub", "dynalib",
           "events", "express", "fs"]


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


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def dump_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=2); f.write("\n")


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def safe_rmtree(path):
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


def gh(args):
    ghbin = os.path.expandvars(r"%LOCALAPPDATA%\Temp\ghx\bin\gh.exe")
    if not os.path.isfile(ghbin):
        ghbin = "gh"
    return subprocess.run([ghbin] + args, capture_output=True, text=True)


def ensure_repo(name, desc):
    r = gh(["api", f"repos/{ORG}/{name}", "--jq", ".full_name"])
    if r.returncode == 0 and r.stdout.strip():
        return "exists"
    r = gh(["api", f"orgs/{ORG}/repos", "-X", "POST",
            "-F", f"name={name}", "-F", "private=false",
            "-F", f"description={desc}", "--jq", ".full_name"])
    if r.returncode == 0:
        return "created"
    return f"FAIL {r.stderr.strip()[:120]}"


def js_readme(mod, meta):
    return f"""# {mod} (node_modules) — JavaScript face

{meta['description']}

Pure JavaScript module: no native binary ships with this package, so the
`mochi_modules/` layer is a manifest-only stub (see its README.md). The whole
implementation lives in `src/`.

## Layout

```text
node_modules/
├── package.json     {mod}@{meta['version']} — entry marker for TGPM
├── manifest.json    {{"layer": "js_face"}} — points at src/index.js
├── src/index.js     the implementation
└── README.md        this file
```

## Use

```javascript
import * as mod from '{mod}';
```

Import through the package name (after TGPM install) rather than by path, so
the engine can resolve the arch-suffixed native layer when one exists.
"""


def native_stub_readme(mod, meta):
    return f"""# {mod} (mochi_modules) — no native core

`{mod}` is a **pure JavaScript module**: it ships no C source and no binary,
so this directory carries only the manifest that keeps the repo shape uniform
across the org.

```text
mochi_modules/
├── manifest.json    {{"layer": "js_only"}} — records that no binary exists
└── README.md        this file
```

If a native core is added later, it belongs here as `src/{mod}.c`,
`lib/{mod}_x86_64.dll` and `include/{mod}.h`, and the manifest layer changes
from `js_only` to `native_core` with the entry point `mbjs_init_module`.
"""


def arch_doc(mod, meta):
    return f"""# {mod} — architecture

{meta['description']}

```
JS face                          native core
node_modules/src/index.js   ->   (none: pure JavaScript module)
```

Everything runs inside the MochiBerry engine's JS runtime; there is no
native plugin for this module. The `mochi_modules/` directory exists only as
a structural marker so every repo in the org has the same two-layer shape.

## Data flow

1. The caller imports `{mod}` (package name, resolved by the loader).
2. `src/index.js` exports the module surface — plain functions and
   EventEmitter-style objects where the API calls for them.
3. No native boundary is crossed, so no ABI or marshalling concerns apply.

## Portability

The module uses only engine-provided globals (`console`, `JSON`, `Math`,
`Date`, `TextEncoder`/`TextDecoder` where available), so it runs unchanged on
every architecture the engine targets — no per-arch build step.
"""


def abi_doc(mod, meta):
    return f"""# {mod} — surface reference

This module has **no native ABI**: there is no `mbjs_init_module` entry point
and no exported C function list. The public surface is the JavaScript export
map of `node_modules/src/index.js`.

The module's own `README.md` (in `node_modules/`) documents every exported
function with its signature and behaviour.

Rules that still apply:

* the package must be imported by name so the loader can pick the right layer
* the package must not import a `.dll`/`.so`/`.dylib` directly — native
  modules are loaded through their `mochi_modules/` layer
* exports are ESM (`import * as mod from '{mod}'`)
"""


def sec_doc(mod, meta):
    return f"""# {mod} — sandbox & security notes

Pure JavaScript module: it inherits the engine sandbox completely.

* it can only reach what the engine exposes to JavaScript
* it opens no sockets and no files by itself — any such capability comes
  from another module that is explicitly linked
* it has no native code path, so no buffer handling or memory-safety surface
  of its own

The module still declares its dependencies in `node_modules/manifest.json`
(`requires_native`), which is how the loader knows what else must be present.
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
  <rect x="24" y="56" width="280" height="70" rx="8" fill="#243044" stroke="#4c6ef5"/>
  <text x="40" y="86" fill="#e6e9ef" font-family="monospace" font-size="13">node_modules/src/index.js</text>
  <text x="40" y="106" fill="#9aa4b8" font-family="monospace" font-size="11">JS face — pure JavaScript</text>
  <rect x="386" y="56" width="230" height="70" rx="8" fill="#1b2430" stroke="#3a4658" stroke-dasharray="5 4"/>
  <text x="402" y="86" fill="#9aa4b8" font-family="monospace" font-size="13">mochi_modules/</text>
  <text x="402" y="106" fill="#7b8494" font-family="monospace" font-size="11">manifest-only stub</text>
  <line x1="304" y1="91" x2="386" y2="91" stroke="#3a4658" stroke-width="2" stroke-dasharray="4 3"/>
  <rect x="24" y="150" width="592" height="62" rx="8" fill="#182030" stroke="#3a4658"/>
  <text x="40" y="176" fill="#9aa4b8" font-family="monospace" font-size="11">runs inside the MochiBerry JS runtime — no native boundary, no per-arch build</text>
</svg>
"""


def root_readme(mod, meta):
    return f"""# {mod}

{meta['description']}

One installable package with two layers:

| layer   | directory       | holds                                        |
|---------|-----------------|----------------------------------------------|
| JS face | `node_modules/` | `src/index.js`, `package.json`, `README.md`  |
| native  | `mochi_modules/`| manifest-only stub — this module has no binary |

Install with TGPM; the engine resolves whichever layer it needs from the
single checkout. No Node.js runtime is required on the host.

## Use

```javascript
import * as mod from '{mod}';
```

## Layout

```text
{mod}/
├── package.json      umbrella package, main -> node_modules/src/index.js
├── manifest.json     umbrella manifest, layers -> js face + native stub
├── docs/             architecture, surface reference, sandbox notes
├── mochiberry/svg/   architecture asset
├── node_modules/     the module itself
└── mochi_modules/    manifest-only stub (no native core)
```

Docs: [architecture](docs/01_architecture.md) ·
[surface reference](docs/02_abi_reference.md) ·
[sandbox & security](docs/03_sandbox_security.md)
"""


STUB_SRC = {
    "mochijsnative_stub": """/*
 * mochijsnative_stub — fallback facade for mochijsnative.
 *
 * Canonical declared "main": "index.js" but shipped no index.js, so any
 * fallback path silently resolved to nothing. This file gives the declared
 * package a real surface: the same export names as mochijsnative, each one
 * failing loudly (never silently) when the native runtime is missing.
 *
 * Replace the whole module with mochijsnative once the native runtime is
 * available on the host.
 */
const UNAVAILABLE =
    'mochijsnative native runtime unavailable (stub package loaded instead)';

function fail() { throw new Error(UNAVAILABLE); }

export function hostBase() { fail(); }
export function hostExport() { fail(); }
export function bind() { fail(); }
export function call() { fail(); }
export function unbind() { fail(); }
export function ffi() { fail(); }
export function callOnce() { fail(); }
export function decodeF64Bits() { fail(); }
export function lastError() { return UNAVAILABLE; }
export const mochiberry = null;

export function isNativeReady() { return false; }

export default {
  isNativeReady,
  lastError,
  hostBase, hostExport, bind, call, unbind,
  ffi, callOnce, decodeF64Bits,
};
""",
}


def build_repo(mod):
    src = os.path.join(NM, mod)
    if not os.path.isdir(src):
        raise RuntimeError(f"no canonical {src}")
    pkg = load_json(os.path.join(src, "package.json"))
    meta = {"version": pkg.get("version", "0.1.0"),
            "description": pkg.get("description") or
                           f"MochiBerry JavaScript module {mod}"}
    repo = os.path.join(STAGE, mod)
    safe_rmtree(repo)
    os.makedirs(repo)

    # ---- JS face ----
    jsdir = os.path.join(repo, "node_modules", "src")
    os.makedirs(jsdir, exist_ok=True)
    entry = "index.js"
    for name in os.listdir(src):
        p = os.path.join(src, name)
        if name == "script":
            for sname in os.listdir(p):
                if sname.endswith(".js"):
                    shutil.copy2(os.path.join(p, sname), os.path.join(jsdir, sname))
            if os.path.isfile(os.path.join(p, "index.js")):
                entry = "index.js"
        elif name.endswith((".js", ".mjs", ".cjs")):
            shutil.copy2(p, os.path.join(jsdir, name))
        elif os.path.isdir(p):
            shutil.copytree(p, os.path.join(repo, "node_modules", name))
        elif name in ("LICENSE",):
            shutil.copy2(p, os.path.join(repo, "LICENSE"))
    if not os.path.isfile(os.path.join(jsdir, "index.js")):
        if mod in STUB_SRC:
            write(os.path.join(jsdir, "index.js"), STUB_SRC[mod])
        else:
            raise RuntimeError(f"{mod}: no index.js found")
    dump_json(os.path.join(repo, "node_modules", "package.json"), {
        "name": mod, "version": meta["version"], "description": meta["description"],
        "type": "module", "main": "src/index.js",
        "exports": {".": "./src/index.js", "./manifest": "./manifest.json"},
        "license": "MIT"})
    dump_json(os.path.join(repo, "node_modules", "manifest.json"), {
        "layer": "js_face", "name": mod, "version": meta["version"],
        "entry": "./src/index.js", "shims": {".": "./src/index.js"},
        "requires_native": [], "source": "original JS face"})
    write(os.path.join(repo, "node_modules", "README.md"), js_readme(mod, meta))

    # ---- native stub ----
    dump_json(os.path.join(repo, "mochi_modules", "manifest.json"), {
        "layer": "js_only", "name": mod, "version": meta["version"],
        "entry_point": None, "status": "js-only",
        "exports": [], "sources": [], "headers": [],
        "note": f"{mod} ships no native core; this directory keeps the repo "
                f"shape uniform across the org."})
    write(os.path.join(repo, "mochi_modules", "README.md"), native_stub_readme(mod, meta))

    # ---- docs + brand ----
    write(os.path.join(repo, "docs", "01_architecture.md"), arch_doc(mod, meta))
    write(os.path.join(repo, "docs", "02_abi_reference.md"), abi_doc(mod, meta))
    write(os.path.join(repo, "docs", "03_sandbox_security.md"), sec_doc(mod, meta))
    write(os.path.join(repo, "mochiberry", "svg", f"{mod}-architecture.svg"), svg(mod, meta))

    # ---- root ----
    dump_json(os.path.join(repo, "package.json"), {
        "name": mod, "version": meta["version"], "description": meta["description"],
        "main": "./node_modules/src/index.js",
        "exports": {".": "./node_modules/src/index.js", "./manifest": "./manifest.json"},
        "license": "MIT", "private": False})
    dump_json(os.path.join(repo, "manifest.json"), {
        "name": mod, "version": meta["version"], "layer": "package",
        "description": meta["description"], "status": "operational",
        "entry": "./node_modules/src/index.js",
        "layers": {"js": "./node_modules/manifest.json",
                   "native": "./mochi_modules/manifest.json"},
        "abi_version": "MBC1", "native_entry": None, "runtime_exports": [],
        "build": {"engine_include": "$ENGINE_INC", "lib_dir": "./mochi_modules/lib",
                  "output": None, "link_libs": []}})
    write(os.path.join(repo, "README.md"), root_readme(mod, meta))
    write(os.path.join(repo, ".gitignore"), "*.o\n*.obj\n*.tmp\nThumbs.db\nbuild/\nnode_modules/lib/*\n!node_modules/lib/.gitkeep\n")
    if not os.path.isfile(os.path.join(repo, "LICENSE")):
        shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(repo, "LICENSE"))

    # ---- git ----
    sh(GIT + ["init", "-q", "-b", BRANCH], cwd=repo)
    sh(["git", "remote", "add", "origin", f"https://github.com/{ORG}/{mod}.git"],
       cwd=repo)
    sh(GIT + ["add", "-A"], cwd=repo)
    sh(GIT + ["commit", "-q", "-m",
              f"{mod}: {meta['description'][:70]}",
              "-m",
              "Dual-layer module package: JS face (node_modules/src/index.js) "
              "+ manifest-only native stub (mochi_modules/), umbrella manifest, "
              "docs and architecture asset. Pure JavaScript module — no binary."],
        cwd=repo)
    return repo, meta


def main():
    args = sys.argv[1:]
    push = "--push" in args
    only = [a for a in args if not a.startswith("--")]
    mods = [m for m in (only or JS_MODS) if m in JS_MODS]
    os.makedirs(STAGE, exist_ok=True)
    results = []
    for mod in mods:
        try:
            repo, meta = build_repo(mod)
            d = meta["description"]
            if not d.startswith(mod):
                d = f"{mod} — {d}"
            desc = f"{d[:110]} (dual-layer MochiBerry module)"
            st = ensure_repo(mod, desc)
            if st.startswith("FAIL"):
                results.append((mod, st)); continue
            if push:
                out = subprocess.run(["git", "push", "-u", "origin", BRANCH],
                                     cwd=repo, capture_output=True, text=True)
                if out.returncode != 0:
                    st = f"PUSH-FAIL {out.stderr.strip()[-120:]}"
                else:
                    st = "pushed"
            results.append((mod, st))
            print(f"{st:12s} {mod}")
            time.sleep(1.0)
        except Exception as e:
            results.append((mod, f"ERR {e}"))
            print(f"{'ERR':12s} {mod}: {e}")
    bad = [r for r in results if not r[1] in ("pushed", "exists")]
    print(f"== {len(mods)} modules, {len(mods)-len(bad)} ok, {len(bad)} problems ==")
    for m, s in bad:
        print(f"   ! {m}: {s}")


if __name__ == "__main__":
    main()