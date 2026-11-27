#!/usr/bin/env python3
"""Build per-module git repos that TGPM can install into node_modules/ or
mochi_modules/ (repo root == module contents, placement inferred by marker:
package.json -> node_modules/<name>/, manifest.json -> mochi_modules/<name>/).

Usage:
  python3 script/make_module_repos.py build [mod,mod,...]   # stage repos (no push)
  python3 script/make_module_repos.py push  [mod,mod,...]   # create+push GitHub repos
  python3 script/make_module_repos.py all   [mod,mod,...]   # build then push
"""
import os, sys, json, subprocess, shutil

ROOT = r"E:/sevm"
STAGE = r"E:/sevm/.modrepos"          # staging dir for repo clones
ORG = "Archana-Berry-Studio"          # fork (memory convention: push only to fork)
NATIVE = os.path.join(ROOT, "native")
NM = os.path.join(ROOT, "node_modules")
MM = os.path.join(ROOT, "mochi_modules")
ARCH = "x86_64"

# which native modules have real source+binary (mochi_modules native-first)
NATIVE_MODS = ["mochizlib", "mochifs", "mochicrypto", "mochiws",
               "mochinode_net", "mochinode_dynalib", "mochinode_sqlite",
               "mochiwasmy", "mochijsnative", "mochitls"]
# which node_modules dirs are real JS-first modules (skip pure-asset dirs)
JS_MODS = ["mochinode", "mochibuffer", "mochipath", "mochistream", "mochiurl",
           "mochiutil", "mochios", "mochihttp", "mochihttps",
           "mochinet", "mochievents", "mochiasync_hooks", "mochidiagnostics_channel",
           "mochiperf_hooks", "mochimodule", "mochiprocess", "mochiglobals",
           "mochiworker_threads", "mochitimers", "mochifetch", "mochijsnative_stub",
           "dynalib", "events", "express", "fs", "net", "sqlite"]

GIT = ["git", "-c", "core.autocrlf=false", "-c", "user.name=Gryzzlay Cutzy",
       "-c", "user.email=gryzzlaycutzy121@users.noreply.github.com"]


def sh(args, cwd=None, check=True):
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"{args} -> {r.returncode}: {r.stderr[:300]}")
    return r.stdout.strip()


def token():
    r = subprocess.run(["git", "credential", "fill"], input="url=https://github.com\n\n",
                       capture_output=True, text=True)
    for line in r.stdout.splitlines():
        if line.startswith("password="):
            return line.split("=", 1)[1]
    return ""


def api(path, method="GET", body=None):
    t = token()
    cmd = ["curl", "-s", "-X", method, "-H", f"Authorization: Bearer {t}",
           "-H", "Accept: application/vnd.github+json"]
    if body is not None:
        cmd += ["-d", json.dumps(body)]
    cmd += [f"https://api.github.com{path}"]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    try:
        return json.loads(out)
    except Exception:
        return {"_raw": out}


def repo_exists(name):
    d = api(f"/repos/{ORG}/{name}")
    return "full_name" in d


def create_repo(name, desc):
    if repo_exists(name):
        return "exists"
    d = api(f"/orgs/{ORG}/repos", "POST",
            {"name": name, "description": desc, "private": False, "auto_init": False})
    if "full_name" in d:
        return "created"
    return f"FAIL: {d.get('message', d)}"


# ---------------- layout builders ----------------

def build_native(mod):
    """Repo root = contents of mochi_modules/<mod>/ (src/ native/ lib/ include/ ...)."""
    src = os.path.join(MM, mod)
    repo = os.path.join(STAGE, mod)
    if os.path.exists(repo):
        shutil.rmtree(repo)
    os.makedirs(repo)
    # copy everything except the flat files we regenerate
    for name in os.listdir(src):
        s = os.path.join(src, name)
        if name == "script":
            # rename script/ -> src/
            shutil.copytree(s, os.path.join(repo, "src"))
        else:
            dst = os.path.join(repo, name)
            if os.path.isdir(s):
                shutil.copytree(s, dst)
            else:
                shutil.copy2(s, dst)
    # ensure src/ has a JS binding (script/index.js -> src/index.js)
    if not os.path.exists(os.path.join(repo, "src")):
        os.makedirs(os.path.join(repo, "src"))
    if not os.path.exists(os.path.join(repo, "src", "index.js")) and \
       os.path.exists(os.path.join(repo, "src", mod + ".js")):
        shutil.copy2(os.path.join(repo, "src", mod + ".js"),
                     os.path.join(repo, "src", "index.js"))
    # update manifest: js_binding -> src/, entry marker stays native
    mf = os.path.join(repo, "manifest.json")
    if os.path.exists(mf):
        d = json.load(open(mf, encoding="utf-8"))
        d["type"] = "native-engine-plugin"
        d["js_binding"] = "src/index.js"
        json.dump(d, open(mf, "w", encoding="utf-8", newline="\n"), indent=2)
    return repo


def build_js(mod):
    """Repo root = contents of node_modules/<mod>/ (src/ native/ lib/ package.json)."""
    src = os.path.join(NM, mod)
    repo = os.path.join(STAGE, mod)
    if os.path.exists(repo):
        shutil.rmtree(repo)
    os.makedirs(repo)
    # move JS files into src/
    sdir = os.path.join(repo, "src")
    os.makedirs(sdir, exist_ok=True)
    for name in os.listdir(src):
        s = os.path.join(src, name)
        if os.path.isdir(s):
            # subdirs (lib/, native/, bin/) stay at repo root
            shutil.copytree(s, os.path.join(repo, name))
        elif name.endswith((".js", ".ts", ".mjs")):
            shutil.copy2(s, os.path.join(sdir, name))
        else:
            shutil.copy2(s, os.path.join(repo, name))
    # if native source exists at ROOT/native/<mod>.c, pull it in
    nsrc = os.path.join(NATIVE, mod + ".c")
    if os.path.exists(nsrc):
        os.makedirs(os.path.join(repo, "native"), exist_ok=True)
        shutil.copy2(nsrc, os.path.join(repo, "native", mod + ".c"))
    # if a per-module lib/ exists in mochi_modules, link it here too
    mlib = os.path.join(MM, mod, "lib")
    if os.path.isdir(mlib):
        ldst = os.path.join(repo, "lib")
        os.makedirs(ldst, exist_ok=True)
        for f in os.listdir(mlib):
            shutil.copy2(os.path.join(mlib, f), os.path.join(ldst, f))
    # package.json: point main at src/index.js
    pj = os.path.join(repo, "package.json")
    if os.path.exists(pj):
        d = json.load(open(pj, encoding="utf-8"))
        d["main"] = "src/index.js"
        json.dump(d, open(pj, "w", encoding="utf-8", newline="\n"), indent=2)
    else:
        json.dump({"name": mod, "version": "0.1.0", "type": "module",
                   "main": "src/index.js"},
                  open(pj, "w", encoding="utf-8", newline="\n"), indent=2)
    return repo


def git_init_commit(repo, mod):
    sh(GIT + ["init", "-q", "-b", "main"], cwd=repo)
    # gitignore: no build artifacts in a module repo
    with open(os.path.join(repo, ".gitignore"), "w", encoding="utf-8", newline="\n") as f:
        f.write("*.o\n*.obj\n*.a\n*.lib\nThumbs.db\n.DS_Store\n")
    sh(GIT + ["add", "-A"], cwd=repo)
    sh(GIT + ["commit", "-q", "-m", f"{mod}: module package (per-module layout)"], cwd=repo)


def push_repo(repo, mod, desc):
    status = create_repo(mod, desc)
    if status.startswith("FAIL"):
        return f"repo:{status}"
    url = f"https://github.com/{ORG}/{mod}.git"
    r = subprocess.run(["git", "remote", "get-url", "origin"], cwd=repo,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sh(["git", "remote", "add", "origin", url], cwd=repo)
    else:
        sh(["git", "remote", "set-url", "origin", url], cwd=repo)
    # The staging repo is rebuilt fresh each run (single commit), so a refresh
    # is a non-fast-forward against the remote. We are the sole committer of
    # these module repos, so force-push is safe and keeps the refresh simple.
    out = subprocess.run(["git", "push", "-f", "-u", "origin", "main"], cwd=repo,
                         capture_output=True, text=True)
    if out.returncode == 0:
        return "pushed"
    return f"push:{out.stderr.strip().splitlines()[-1][:120]}"


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "build"
    only = set(sys.argv[2:])
    os.makedirs(STAGE, exist_ok=True)

    todo = []
    for m in NATIVE_MODS:
        if (not only or m in only) and os.path.isdir(os.path.join(MM, m)):
            todo.append((m, "native"))
    for m in JS_MODS:
        if (not only or m in only) and os.path.isdir(os.path.join(NM, m)):
            todo.append((m, "js"))

    report = []
    for mod, kind in todo:
        try:
            repo = build_native(mod) if kind == "native" else build_js(mod)
            git_init_commit(repo, mod)
            msg = "built"
            if mode in ("push", "all"):
                desc = f"{mod} — MochiBerry module (per-module layout, installable via TGPM)"
                msg = push_repo(repo, mod, desc)
            report.append((mod, kind, msg))
        except Exception as e:
            report.append((mod, kind, f"ERR {e}"))

    for mod, kind, msg in report:
        print(f"{msg:20s} {kind:8s} {mod}")
    print(f"== {mode}: {len(report)} modules ==")


if __name__ == "__main__":
    main()
