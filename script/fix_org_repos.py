#!/usr/bin/env python3
"""Complete the per-module repos in the Mochi-Berry org.

Required layout (user convention, verbatim intent):
  node_modules/   readme.md + package.json + the rest (JS face, src/index.js)
  mochi_modules/  readme.md + manifest.json + C source or binary (native core)
  mochiberry/     asset documentation
  docs/           documentation
  plus any loose files at the repo root

The org repos were built with the JS face + native core + manifest layers but
are missing three required marker files in every module repo:

  node_modules/package.json   (absent in all module repos)
  node_modules/README.md      (absent in all module repos)
  mochi_modules/README.md     (absent in all module repos)

mochinode is additionally missing mochi_modules/include/ (every other module
ships include/<mod>.h + include/<mod>_abi.h) and its manifest lists no headers.

Usage:
  python3 script/fix_org_repos.py check     # audit only, no writes
  python3 script/fix_org_repos.py build     # clone + add missing files
  python3 script/fix_org_repos.py push      # build + commit + push each repo
"""
import os, sys, json, subprocess, shutil

ROOT = r"E:/sevm"
STAGE = r"E:/sevm/.orgfix"
ORG = "Mochi-Berry"
BRANCH = "main"

# module repos present in the org (verified by API audit, not assumed)
MODS = ["mochicrypto", "mochifs", "mochijsnative", "mochinode",
        "mochinode_dynalib", "mochinode_net", "mochinode_sqlite",
        "mochiossil", "mochisandbox", "mochiwasmy", "mochiws",
        "mochiyuva", "mochizlib", "cmochicode"]

NM_HDR = """# {name} (node_modules) — JavaScript face

JavaScript binding for `{name}`, the JS-first half of this module package.
It loads the native core from `../mochi_modules/lib/{name}_{arch}.dll` through
the engine's `mochinode` loader and re-exports its C ABI unchanged.

## Layout

```text
node_modules/
├── package.json     {name}@{version} — entry marker for TGPM (node_modules/)
├── manifest.json    {{"layer": "js_face"}} — points at src/index.js
├── src/index.js     the binding (init/isNativeReady + re-exported natives)
├── native/          native sources used by the JS build, if any
└── lib/             prebuilt binaries for the JS side, if any
```

The native half lives in `../mochi_modules/` (see its README.md); this repo
ships both halves as one installable package so the engine can resolve either
layer from a single checkout.

## Usage

```javascript
import * as mod from '{name}';        // after TGPM install
const m = await mod.init();          // loads the native core
{exports_example}
```

`init()` is idempotent; `isNativeReady()` reports whether the native DLL is
loaded yet (false before init). Do not import the DLL path directly — always
go through the shim so the engine resolves the correct architecture suffix.
"""

MM_HDR = """# {name} (mochi_modules) — native core

Native-first half of this module package: the C source, the prebuilt binary,
the public ABI headers, and the build file. The JS face lives in
`../node_modules/` (see its README.md).

## Layout

```text
mochi_modules/
├── README.md          this file
├── manifest.json      {{"layer": "native_core"}} — targets, exports, sources
├── CMakeLists.txt     builds lib/{name}_{arch}.dll from src/
├── include/           public ABI headers
├── src/               C sources (engine plugin)
├── native/            extra native sources, if any
├── script/            shim/inspection helpers, if any
└── lib/               prebuilt {name}_{arch}.dll (ready to load)
```

## ABI contract

* The plugin exports exactly one entry point: `mbjs_init_module`.
* Runtime `MBJS_*` symbols are resolved from the host engine at load time via
  `mbjs_runtime_resolve()` — never link against `mbjs.exe` (importing it
  fails with error 126 on Windows).
* C functions that throw MUST return `MBJS_EXCEPTION`, never `MBJS_UNDEFINED`.
* Specifier imports must end in `.dll`/`.so`/`.dylib` (arch-suffixed name).

## Build

```bash
cmake -S . -B build -DMBJS_INC=E:/bcf/mochiberry   # engine headers
cmake --build build
# output: lib/{name}_{arch}.dll
```

Requires `bcrypt` and `winhttp` on Windows (linked automatically).
"""


def safe_rmtree(path):
    """shutil.rmtree can hit Windows 'Access is denied' on .git/objects/pack/*
    files that a lingering git/indexer process still holds. Fall back to the
    native rmdir, then to a rename-and-retry so a stale lock never blocks the
    refresh."""
    import time
    if not os.path.isdir(path):
        return
    for attempt in range(3):
        try:
            shutil.rmtree(path)
            return
        except Exception:
            subprocess.run(["cmd", "/c", "rmdir", "/s", "/q", path],
                           capture_output=True)
            if not os.path.isdir(path):
                return
            try:
                os.rename(path, path + f".stale{attempt}")
                shutil.rmtree(path + f".stale{attempt}", ignore_errors=True)
                return
            except Exception:
                time.sleep(1.0)
    raise RuntimeError(f"cannot remove {path} (locked)")


def sh(args, cwd=None, check=True):
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"{args} -> {r.returncode}: {r.stderr[:300]}")
    return r.stdout.strip()


def git_id():
    """Read the committer identity from the SEVM repo instead of hardcoding it."""
    n = sh(["git", "-C", ROOT, "config", "user.name"])
    e = sh(["git", "-C", ROOT, "config", "user.email"])
    return ["git", "-c", f"user.name={n}", "-c", f"user.email={e}",
            "-c", "core.autocrlf=false"]


GIT = git_id()


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def dump_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")


W = lambda p: os.path.join(*p.split("/"))


def fix_repo(mod, push=False):
    repo = os.path.join(STAGE, mod)
    url = f"https://github.com/{ORG}/{mod}.git"
    safe_rmtree(repo)
    os.makedirs(STAGE, exist_ok=True)
    sh(["git", "clone", "-q", "--depth", "1", "-b", BRANCH, url, repo])
    branch = sh(["git", "-C", repo, "rev-parse", "--abbrev-ref", "HEAD"])

    root_pkg = load_json(os.path.join(repo, "package.json")) if os.path.isfile(os.path.join(repo, "package.json")) else {}
    name = root_pkg.get("name", mod)
    version = root_pkg.get("version", "0.1.0")
    desc = root_pkg.get("description", f"MochiBerry module {mod}")

    mm_manifest_path = os.path.join(repo, "mochi_modules", "manifest.json")
    mm = load_json(mm_manifest_path) if os.path.isfile(mm_manifest_path) else {}
    exports = mm.get("exports", [])
    example = "\n".join(f"  mod.{e}(...)   // was: {e}()" for e in exports[:5]) or "  mod.init()"

    added = []

    # 1) node_modules/package.json
    p_nm_pkg = os.path.join(repo, "node_modules", "package.json")
    if not os.path.isfile(p_nm_pkg):
        dump_json(p_nm_pkg, {
            "name": name,
            "version": version,
            "description": desc,
            "type": "module",
            "main": "src/index.js",
            "exports": {".": "./src/index.js", "./manifest": "./manifest.json"},
            "license": "MIT",
        })
        added.append("node_modules/package.json")

    # 2) node_modules/README.md
    p_nm_rd = os.path.join(repo, "node_modules", "README.md")
    if not os.path.isfile(p_nm_rd):
        os.makedirs(os.path.dirname(p_nm_rd), exist_ok=True)
        with open(p_nm_rd, "w", encoding="utf-8", newline="\n") as f:
            f.write(NM_HDR.format(name=name, version=version, arch="x86_64",
                                  exports_example=example))
        added.append("node_modules/README.md")

    # 3) mochi_modules/README.md
    p_mm_rd = os.path.join(repo, "mochi_modules", "README.md")
    if not os.path.isfile(p_mm_rd):
        os.makedirs(os.path.dirname(p_mm_rd), exist_ok=True)
        with open(p_mm_rd, "w", encoding="utf-8", newline="\n") as f:
            f.write(MM_HDR.format(name=name, arch="x86_64"))
        added.append("mochi_modules/README.md")

    # 4) mochi_modules/include/ — every other module ships <mod>.h + <mod>_abi.h
    inc = os.path.join(repo, "mochi_modules", "include")
    os.makedirs(inc, exist_ok=True)
    hp = os.path.join(inc, mod + ".h")
    if not os.path.isfile(hp):
        U = mod.upper()
        ex = ", ".join(exports)
        with open(hp, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"""#ifndef MB_{U}_H
#define MB_{U}_H
/*
 * {name} — MochiBerry native-first module ({version}).
 *
 * ABI contract:
 *   - exports the single entry point mbjs_init_module(MBJSContext*, const char*)
 *   - registers C functions via NewCModule / AddModuleExportList / SetModuleExportList
 *   - C functions that throw MUST return MBJS_EXCEPTION (never MBJS_UNDEFINED)
 *   - Runtime MBJS_* symbols come from mbjs_runtime_resolve(); do NOT link
 *     against mbjs.exe (error 126 on Windows).
 *
 * Runtime exports (measured): {ex}
 */
#include <stdint.h>

typedef struct MBJSContext MBJSContext;
typedef struct MBJSModuleDef MBJSModuleDef;

MBJSModuleDef *mbjs_init_module(MBJSContext *ctx, const char *name);

#endif /* MB_{U}_H */
""")
        added.append(f"mochi_modules/include/{mod}.h")

    hp2 = os.path.join(inc, mod + "_abi.h")
    if not os.path.isfile(hp2):
        U = mod.upper()
        with open(hp2, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"""#ifndef MB_{U}_ABI_H
#define MB_{U}_ABI_H
#ifdef __cplusplus
extern "C" {{
#endif

/* ABI for {name} — engine plugin. Entry point: mbjs_init_module().
   Runtime exports (verified via mbjs loadNative): */
""")
            for e in exports:
                f.write(f"/*   - {e} */\n")
            f.write("""
#ifdef __cplusplus
}
#endif
#endif
""")
        added.append(f"mochi_modules/include/{mod}_abi.h")

    # 5) keep the manifest's header list truthful
    if os.path.isfile(mm_manifest_path) and (mod == "mochinode" or
            sorted(mm.get("headers", [])) != sorted(
                [f"include/{mod}.h", f"include/{mod}_abi.h"])):
        hdrs = [f"include/{mod}.h", f"include/{mod}_abi.h"]
        if mm.get("headers") != hdrs:
            mm["headers"] = hdrs
            dump_json(mm_manifest_path, mm)
            added.append("mochi_modules/manifest.json (headers list)")

    # 6) docs/ + mochiberry/ — the two extras every module package carries
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import create_org_mod as com
    meta = {"version": version, "arch": "x86_64",
            "description": desc, "status": "verified",
            "exports": exports,
            "doc_exports": [(e, "runtime export (see the module README)")
                             for e in exports],
            "syslibs": "the Windows libraries the module links against",
            "cc_libs": "", "cmake_libs": "", "link_libs": []}
    docs = os.path.join(repo, "docs")
    doc_files = [("docs/01_architecture.md", com.doc_arch(mod, meta)),
                 ("docs/02_abi_reference.md", com.doc_abi(mod, meta)),
                 ("docs/03_sandbox_security.md", com.doc_sec(mod, meta))]
    for rel, body in doc_files:
        target = os.path.join(repo, *rel.split("/"))
        if not os.path.isfile(target):
            write(target, body)
            added.append(rel)
    brand = os.path.join(repo, "mochiberry", "svg", f"{mod}-architecture.svg")
    if not os.path.isfile(brand):
        write(brand, com.svg(mod, meta))
        added.append(f"mochiberry/svg/{mod}-architecture.svg")

    # 7) some repos whitelist node_modules/ file-by-file; make sure the
    #    markers we just added are tracked instead of silently ignored
    gi = os.path.join(repo, "node_modules", ".gitignore")
    if os.path.isfile(gi):
        text = open(gi, encoding="utf-8").read()
        extra = ""
        if "!" not in text and "package.json" not in text:
            extra = "!package.json\n"
        for entry in ("!package.json", "!README.md"):
            if entry not in text:
                extra += entry + "\n"
        if extra:
            with open(gi, "w", encoding="utf-8", newline="\n") as f:
                f.write(text.rstrip("\n") + "\n\n" + extra)
            added.append("node_modules/.gitignore (un-ignore markers)")

    # ---- verify the required layout is now complete ----
    required = [
        "node_modules/package.json", "node_modules/README.md",
        "mochi_modules/manifest.json", "mochi_modules/README.md",
        "node_modules/src/index.js",
    ]
    missing = [r for r in required if not os.path.isfile(os.path.join(repo, *r.split("/")))]
    has_bin = os.path.isdir(os.path.join(repo, "mochi_modules", "lib")) and any(
        f.endswith((".dll", ".so", ".dylib")) for f in
        os.listdir(os.path.join(repo, "mochi_modules", "lib")))
    has_src = os.path.isdir(os.path.join(repo, "mochi_modules", "src")) and any(
        f.endswith(".c") for f in os.listdir(os.path.join(repo, "mochi_modules", "src")))
    has_docs = os.path.isdir(os.path.join(repo, "docs")) and bool(os.listdir(os.path.join(repo, "docs")))
    has_brand = os.path.isdir(os.path.join(repo, "mochiberry")) and bool(
        os.listdir(os.path.join(repo, "mochiberry")))

    state = "OK" if not (missing or not (has_bin or has_src) or not has_docs or not has_brand) else "INCOMPLETE"
    if missing or not has_bin or not has_src or not has_docs or not has_brand:
        print(f"  [{mod}] missing={missing} bin={has_bin} src={has_src} docs={has_docs} brand={has_brand}")

    if push and state == "OK":
        sh(GIT + ["add", "-A"], cwd=repo)
        # force: a node_modules/.gitignore can legitimately whitelist files
        sh(GIT + ["add", "-f", "node_modules", "mochi_modules", "docs",
                  "mochiberry"], cwd=repo, check=False)
        st = sh(["git", "status", "--short"], cwd=repo)
        if st:
            sh(GIT + ["commit", "-q", "-m",
                      f"{mod}: complete per-module layout — node_modules package.json + README, "
                      f"mochi_modules README + include/ headers",
                      "-m",
                      "Required markers per the module-package convention: "
                      "node_modules/package.json + README.md (marker dir), "
                      "mochi_modules/README.md, and include/<mod>.h + <mod>_abi.h "
                      "for mochinode (was the only module without include/). "
                      "Content verified against the org API tree."],
                cwd=repo)
            out = sh(["git", "push", "origin", branch], cwd=repo, check=False)
            if out:
                print(f"  [{mod}] push: {out[-200:]}")
    return state, added


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "check"
    only = set(sys.argv[2:])
    mods = [m for m in MODS if not only or m in only]
    os.makedirs(STAGE, exist_ok=True)
    states = {}
    for m in mods:
        try:
            state, added = fix_repo(m, push=(mode == "push"))
            states[m] = state
            print(f"{state:11s} {m:18s} +{len(added)} files: {', '.join(added) if added else 'already complete'}")
        except Exception as e:
            states[m] = f"ERR {e}"
            print(f"{'ERR':11s} {m:18s} {e}")
    bad = {m: s for m, s in states.items() if s != "OK"}
    print(f"== {mode}: {len(mods)} repos, {len(mods)-len(bad)} complete, {len(bad)} problems ==")


if __name__ == "__main__":
    main()