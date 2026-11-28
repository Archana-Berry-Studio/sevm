#!/usr/bin/env python3
"""Give the source repos in the Mochi-Berry org the same two-layer shape.

libmochiberry (engine), libwasmyberry (Zig VM) and takegawa (C utility) are
not installable shim modules: they are the upstream sources those modules are
built from. They still get the org-wide layout so a checkout of any repo in
the org has the same shape, but the manifests say so honestly:

  layer: "source_repo"   instead of "js_face" / "native_core"
  no fake entry point, no fake export list, no fake binary

The native half records the real build outputs (what CMake/Make produces and
where) instead of claiming a prebuilt DLL that does not exist in the repo.

Usage:
  python3 script/fix_vendor_repos.py --push [libmochiberry libwasmyberry takegawa]
"""
import os, re, sys, json, subprocess, shutil, time

ROOT = r"E:/sevm"
STAGE = r"E:/sevm/.vendorfix"
ORG = "Mochi-Berry"

VENDOR = {
    "libmochiberry": {
        "branch": "main",
        "version": "0.1.0",
        "description": "MochiBerry engine — embeddable C runtime with the JS "
                       "engine, module loader and builtin library",
        "build": "cmake -S . -B build && cmake --build build",
        "artifacts": ["build/mbjs (the engine executable)",
                      "build/libmochiberry.* (embeddable library)"],
        "consumers": "the per-module repos under Mochi-Berry build against "
                     "this engine's headers (mbjs.h, mbjs_runtime.h)",
        "notes": "This repository is the engine source. It ships no prebuilt "
                 "binary and no JavaScript package; the two layer "
                 "directories exist so every repo in the org has the same "
                 "shape and the loader can resolve them uniformly.",
    },
    "libwasmyberry": {
        "branch": "master",
        "version": "0.1.0",
        "description": "wasmyberry — the WebAssembly VM (Zig) that backs the "
                       "mochiwasmy engine module",
        "build": "zig build -Doptimize=Release",
        "artifacts": ["zig-out/lib/libwasmyberry.a (static library)",
                      "zig-out/bin/* (test binaries)"],
        "consumers": "mochiwasmy — the engine plugin that embeds this VM",
        "notes": "This repository is the WebAssembly VM source. It ships no "
                 "prebuilt binary and no JavaScript package; the two layer "
                 "directories exist so every repo in the org has the same "
                 "shape and the loader can resolve them uniformly.",
    },
    "takegawa": {
        "branch": "main",
        "version": "0.1.0",
        "description": "takegawa — small C utility library with compatibility "
                       "shims and examples",
        "build": "make",
        "artifacts": ["libtakegawa.* (static/shared library)",
                      "examples/* (sample binaries)"],
        "consumers": "utility layer used by the engine-side compatibility code",
        "notes": "This repository is a C library source tree. It ships no "
                 "prebuilt binary and no JavaScript package; the two layer "
                 "directories exist so every repo in the org has the same "
                 "shape and the loader can resolve them uniformly.",
    },
}


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


def layer_readme(name, meta, layer):
    if layer == "node_modules":
        return f"""# {name} (node_modules) — source layer marker

{meta['description']}

This repository is an **upstream source tree**, not an installable shim
module, so this directory carries no JavaScript package. It exists because
every repo in the `Mochi-Berry` org uses the same two-layer shape:

```text
node_modules/   JS-installable face  (empty for a source repo)
mochi_modules/  native face           (build recipe + artifact map)
```

What lives here for a source repo:

* `manifest.json` — records that this is a source repository, with the build
  command and the artifacts the build produces
* `README.md` — this note

## What the repo actually is

{meta['notes']}

## Build

```bash
{meta['build']}
```

Produces:

{chr(10).join('* ' + a for a in meta['artifacts'])}

## Consumers

{meta['consumers']}
"""
    return f"""# {name} (mochi_modules) — source layer marker

{meta['description']}

This repository is an **upstream source tree**, not an installable shim
module, so this directory contains no prebuilt binary and no plugin entry
point. It records the native build instead:

```text
mochi_modules/
├── manifest.json   build command + artifact map (no fake binary)
└── README.md       this note
```

## Build

```bash
{meta['build']}
```

Produces:

{chr(10).join('* ' + a for a in meta['artifacts'])}

## Consumers

{meta['consumers']}

## Note

{meta['notes']}
"""


def build(name, meta, push=False):
    repo = os.path.join(STAGE, name)
    safe_rmtree(repo)
    os.makedirs(STAGE, exist_ok=True)
    sh(["git", "clone", "-q", "--depth", "1", "-b", meta["branch"],
        f"https://github.com/{ORG}/{name}.git", repo])

    nm = os.path.join(repo, "node_modules", name)
    mm = os.path.join(repo, "mochi_modules", name)
    dump_json(os.path.join(nm, "package.json"), {
        "name": name, "version": meta["version"],
        "description": meta["description"],
        "license": "MIT", "private": True,
        "sourceRepository": True,
        "note": "Source repository — no JavaScript entry point is shipped. "
                "The directory is the org-wide node_modules layer marker.",
    })
    dump_json(os.path.join(nm, "manifest.json"), {
        "layer": "source_repo", "name": name, "version": meta["version"],
        "description": meta["description"],
        "entry": None, "shims": {},
        "build": meta["build"], "artifacts": meta["artifacts"],
        "note": meta["notes"],
    })
    write(os.path.join(nm, "README.md"), layer_readme(name, meta, "node_modules"))

    dump_json(os.path.join(mm, "manifest.json"), {
        "layer": "source_repo", "name": name, "version": meta["version"],
        "description": meta["description"],
        "entry_point": None, "status": "source",
        "exports": [], "sources": [], "headers": [],
        "build": meta["build"], "artifacts": meta["artifacts"],
        "consumers": meta["consumers"],
        "note": meta["notes"],
    })
    write(os.path.join(mm, "README.md"), layer_readme(name, meta, "mochi_modules"))

    # root umbrella so the repo also answers to the loader like the rest
    dump_json(os.path.join(repo, "package.json"), {
        "name": name, "version": meta["version"],
        "description": meta["description"],
        "license": "MIT", "private": True,
        "sourceRepository": True,
        "layers": {"js": f"./node_modules/{name}/manifest.json",
                   "native": f"./mochi_modules/{name}/manifest.json"},
    })
    dump_json(os.path.join(repo, "manifest.json"), {
        "name": name, "version": meta["version"], "layer": "source_repository",
        "description": meta["description"], "status": "source",
        "entry": None,
        "layers": {"js": f"./node_modules/{name}/manifest.json",
                   "native": f"./mochi_modules/{name}/manifest.json"},
        "abi_version": "MBC1", "native_entry": None, "runtime_exports": [],
        "build": {"engine_include": "$ENGINE_INC", "lib_dir": None,
                  "output": None, "link_libs": []},
    })

    # Some of these repos gitignore node_modules/ (takegawa does). The marker
    # directories are deliverables, so un-ignore them explicitly and force-add
    # them — otherwise git silently drops the whole layer.
    gi = os.path.join(repo, ".gitignore")
    if os.path.isfile(gi):
        text = open(gi, encoding="utf-8").read()
        rules = ""
        if re.search(r"(?m)^\s*node_modules/?\s*$", text):
            rules += (f"node_modules/*\n!node_modules/{name}/\n"
                      f"!node_modules/{name}/**\n")
        if re.search(r"(?m)^\s*mochi_modules/?\s*$", text):
            rules += (f"mochi_modules/*\n!mochi_modules/{name}/\n"
                      f"!mochi_modules/{name}/**\n")
        if rules and rules not in text:
            with open(gi, "w", encoding="utf-8", newline="\n") as f:
                f.write(text.rstrip("\n") + "\n\n# org-wide layer markers (kept on purpose)\n" + rules)

    sh(GIT + ["add", "-A"], cwd=repo)
    sh(GIT + ["add", "-f", "node_modules", "mochi_modules"], cwd=repo, check=False)
    sh(GIT + ["commit", "-q", "-m",
              f"{name}: add the org-wide two-layer shape",
              "-m",
              "Adds node_modules/<name>/ and mochi_modules/<name>/ so every "
              "repo in the org resolves the same way. The manifests say "
              "layer=source_repo honestly: this tree ships no JavaScript "
              "entry point and no prebuilt binary, only the build recipe "
              "and the artifact map."], cwd=repo)
    if push:
        out = subprocess.run(["git", "push", "origin", meta["branch"]],
                             cwd=repo, capture_output=True, text=True)
        if out.returncode != 0:
            return f"PUSH-FAIL {out.stderr.strip()[-160:]}"
        return "pushed"
    return repo


def main():
    args = sys.argv[1:]
    push = "--push" in args
    only = [a for a in args if not a.startswith("--")]
    names = [n for n in (only or list(VENDOR)) if n in VENDOR]
    for n in names:
        try:
            r = build(n, VENDOR[n], push=push)
            print(f"{r if isinstance(r, str) else 'built':12s} {n}")
        except Exception as e:
            print(f"{'ERR':12s} {n}: {e}")


if __name__ == "__main__":
    main()