# SEVM — SQLite · Express.js · Vue.js · MochiBerry

**SEVM** is a web ecosystem that runs *on top of* the **MochiBerry** engine
with a clean architecture split: the core engine stays lightweight and
untouched, while HTTP, SQLite and the `node:*` shims live as native `.dll` /
`.so` / `.lbrct` extensions loaded at runtime (dlopen-style), with no engine
recompile.

## The two module worlds

SEVM separates **two orientations** of modules — the difference is not
"allowed/not allowed", it is the *dominant orientation*:

| | `node_modules/` | `mochi_modules/` |
|---|---|---|
| orientation | **JS / script / framework** | **native engine plugin** |
| main content | JavaScript packages (`package.json` + `index.js` + `lib/`) | shared objects / DLLs loaded into the engine |
| native lib | **allowed** — a package may carry `.dll` / `.exe` / `.elf` in `bin/` | this IS the core: `.dll` / `.so` / `.lbrct` |
| examples | `express`, `net`, `sqlite`, `events`, `fs`, `mochinode` | `mochinode_net.dll`, `mochinode_sqlite.dll` |

So `node_modules` = NodeJS-style framework packages (scripting/JS dominant, but
may still carry native libs), while `mochi_modules` = native engine plugins
(DLL/so oriented).

## Project structure (NodeJS style)

```
sevm/
├── package.json                 # root manifest (type: module, dependencies)
├── README.md
├── LICENSE
├── node_modules/                # JS/framework packages (NodeJS style)
│   ├── mochinode/               # core loader: require(), loadNative(), resolver
│   │   ├── index.js
│   │   └── package.json
│   ├── express/                 # Express-compatible router
│   │   ├── index.js
│   │   ├── package.json
│   │   └── README.md
│   ├── dynalib/                 # dynalib(file, offset, symbol, abi, opts) API
│   │   ├── index.js
│   │   ├── package.json
│   │   └── README.md
│   ├── net/                     # node:http on top of mochinode_net
│   │   ├── index.js
│   │   ├── lib/http.js          # parsing + response writer
│   │   └── package.json
│   ├── sqlite/                  # node:sqlite on top of mochinode_sqlite
│   ├── events/                  # node:events (EventEmitter)
│   └── fs/                      # node:fs on top of mbjs:std / mbjs:os
├── mochi_modules/               # native engine plugins
│   ├── mochinode_net/           # per-module dir: native/, include/, lib/, script/
│   ├── mochinode_sqlite/
│   ├── mochinode_dynalib/       # libdynalib -> JS bridge
│   └── README.md
├── native/                      # C sources behind mochi_modules plugins
│   ├── mbjs_runtime.h           # engine symbol resolve bootstrap
│   ├── mochinode_net.c
│   ├── mochinode_sqlite.c
│   └── mochinode_poke.c         # probe module
├── libdynalib/                  # C99 dynamic loader subsystem (107/107 test)
└── examples/
    └── sevm-app/                # sample app (NodeJS style)
        ├── package.json
        ├── src/server.js
        └── public/
            ├── index.html       # Vue 3 SPA
            └── vue.global.prod.js
```

## Running the demo

```bash
cd examples/sevm-app
mbjs.exe -m src/server.js

# then:
curl http://127.0.0.1:8080/api/health
curl http://127.0.0.1:8080/api/items
curl -X POST http://127.0.0.1:8080/api/items \
     -H "content-type: application/json" \
     -d '{"title":"Send report to the boss"}'
```

Endpoints:

- `GET /` — Vue 3 SPA (static build, client-side)
- `GET /api/items?q=` — JSON from SQLite
- `POST /api/items` — INSERT into SQLite
- `GET /api/health` — engine/runtime status

## Native engine module contract (verified by disassembly + source)

- Loader: `LoadLibraryA(path)` then `GetProcAddress(h, "mbjs_init_module")`,
  `m = init(ctx, module_name)`.
- The exported symbol **must** be named `mbjs_init_module`.
- The import specifier **must** end in `.dll`; relative paths resolve relative
  to the importing file's directory (`../` is also supported).
- `AddModuleExportList` (names) is called inside `mbjs_init_module`;
  `SetModuleExportList` (values) is called in the module init func.
- A native module **must not** import `mbjs.exe` (Windows error 126) — engine
  symbols are resolved at runtime from the host image (`GetModuleHandle(NULL)`),
  see `native/mbjs_runtime.h`.
- The engine must export the `MBJS_*` symbols; the stripped `tier-core` build
  has no export table, so native plugins cannot resolve.

## Components

| Component | Status | Notes |
|----------|--------|-------|
| `takegawa` (TGPM) | ✅ | separate repo/tool (`Archana-Berry-Studio/takegawa`) |
| `libdynalib` | ✅ 107/107 test | C99 dynamic loader across `.so`/`.dll`/`.lbrct`; also in repo `Archana-Berry-Studio/libmochiberry` |
| `mochi_modules/mochinode_net` | ✅ | blocking TCP/HTTP server (Winsock2) |
| `mochi_modules/mochinode_sqlite` | ✅ | SQLite bridge (sqlite3 3.50.4, prepared stmt) |
| `mochi_modules/mochinode_dynalib` | ✅ | `dynalib()` -> JS bridge (embedded libdynalib, JIT trampoline) |
| `node_modules/*` | ✅ | shims `node:http`, `node:sqlite`, `node:events`, `node:fs`, Express, dynalib |
| Vue SPA | ✅ | `public/index.html` + `vue.global.prod.js` (offline) |
| demo HTTP | ✅ | `examples/sevm-app/src/server.js` -> port 8080 |

## Agent & shim workflow

- `AGENTS.md` holds the architecture contract for agents: the `mochi_modules/`
  vs `node_modules/` boundary, new-module layout, ABI/dynalib rules, and the
  gap-driven flow.
- `MISSING_SHIMS.json` records existing and still-missing shims.
- `python script/scaffold_shim.py <name> mochi` scaffolds a native-first module;
  mode `node` scaffolds a JS-first hybrid package.
- Cross-compile toolchains live in `cmake/` (MinGW, Linux, Android NDK).

New shims: `mochitimers`, `mochiglobals`, `mochifetch`, and `mochihttps`
(delegates to WinHTTP via `mochihttp`).

## Size notes

Core engine (`tier-core` build, stripped) is about **1.21 MB** — still above the
< 1 MB target. SEVM extensions (net + sqlite DLLs) are about **60 KB** each and
do not add to the engine size because they are loaded at runtime.
