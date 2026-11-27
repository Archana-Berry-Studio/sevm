# sevm-notes — Offline-First Notes PWA on the MochiBerry engine

A small, complete progressive web app served entirely by the SEVM stack:

- **Vue 3** SPA (vendored build, no CDN, works offline)
- **Express-shaped router** from the `express` shim
- **SQLite** persistence through the `sqlite` shim (`mochinode_sqlite.dll`)
- **Service worker** shell precache + API read cache
- **IndexedDB outbox** for writes made while the engine is unreachable

Everything runs on the MochiBerry engine (`mbjs.exe`) — no Node.js runtime on
the host.

## Run

```bash
cd examples/sevm-notes
E:/bcf/mochiberry/build_manual/mbjs.exe -m src/server.js
```

Then open <http://127.0.0.1:8080/> (the server binds `0.0.0.0`, so other
devices on the same LAN can reach it too).

The database is `app.db` in the application folder; it is created on first
run and seeded with three example notes.

## Layout

```text
sevm-notes/
├── package.json
├── app.db             # created on first run (gitignored)
├── src/
│   └── server.js      # entry point: router + SQLite + static assets
└── public/
    ├── index.html     # Vue 3 SPA shell
    ├── app.js         # Vue app: notes CRUD + offline outbox
    ├── notes.css      # styles
    ├── manifest.webmanifest
    ├── sw.js          # service worker
    ├── icon.svg       # app icon
    └── vue.global.prod.js
```

## Endpoints

| method   | path                    | purpose                                   |
|----------|-------------------------|-------------------------------------------|
| GET      | `/`                     | Vue 3 notes SPA                           |
| GET      | `/app.js`               | app logic (cached by the service worker)  |
| GET      | `/notes.css`            | styles                                    |
| GET      | `/manifest.webmanifest` | PWA manifest                              |
| GET      | `/sw.js`                | service worker                            |
| GET      | `/icon.svg`             | app icon                                  |
| GET      | `/vue.global.prod.js`   | vendored Vue build                        |
| GET      | `/api/health`           | `{ ok, engine, sqlite, requested }`       |
| GET      | `/api/notes?q=&pinned=` | `{ count, notes[] }` from SQLite          |
| POST     | `/api/notes`            | create `{ title, body, color }` → `201`   |
| PUT      | `/api/notes/:id`        | update `{ title, body, color, pinned }`   |
| DELETE   | `/api/notes/:id`        | delete                                    |

The `notes` table: `id, title, body, color, pinned, created_at, updated_at`.
Colors are restricted to `blue | green | amber | red | purple`; anything else
falls back to `blue`.

## Offline-first behaviour

- **Reads** go through the service worker with a network-first strategy and a
  cache fallback, so the last successful notes list is shown when the engine
  is unreachable. The app also keeps a local mirror in IndexedDB and renders
  that if the fetch itself fails.
- **Writes** that cannot reach the server are queued in an IndexedDB outbox,
  applied optimistically to the UI, and replayed when the browser comes back
  online. The queue size is visible in the header — pending writes are never
  dropped silently.
- The initial render deliberately does **not** wait for IndexedDB: the notes
  list is fetched first, the outbox is opened lazily.

## Measured limitations (verified on this host, not guesses)

- **Binary files cannot be served byte-exact.** `fs.readFileSync()` returns a
  JS string decoded with lossy UTF-8, so PNG bytes are corrupted in transit.
  The app therefore ships an **SVG icon** (text, byte-exact) instead of PNG
  icons. Serving real binary files needs a byte-buffer read/write path in
  `mochifs` + `net` that does not exist yet.
- **The service worker could not be exercised end-to-end in the headless
  harness used here.** Chrome's `--virtual-time-budget` mode never settles
  `navigator.serviceWorker.register()` and never populates `CacheStorage`,
  so the offline navigation path is not proven by an automated test on this
  host. What *is* verified: the browser fetches `/sw.js` (HTTP 200, 2317
  bytes, byte-exact), the script is stored in the profile's service-worker
  cache, and the shell plus API caching code is the standard
  cache-first/network-first pattern. Verify the offline path in a normal
  browser window with a reload cycle.
- **The HTTP server is cooperative and single-threaded.** One request is
  served at a time with a 5 ms yield between accepts. Concurrent requests
  can queue or be dropped, so fire API calls sequentially or retry on
  failure.
- `Date.now()` in the engine does not return epoch milliseconds (the seeded
  notes show a 1970 timestamp); it is used only for ordering, so it is
  harmless here, but do not use it for wall-clock display.

## Engine fixes this app required

Two bugs in the stack were found while building this app and are fixed in
the repository:

1. `node_modules/net/lib/http.js` — `content-length` was computed from the
   JS string length (UTF-16 code units) instead of the UTF-8 byte count.
   Every response containing non-ASCII characters was silently truncated at
   the client. Fixed with a `utf8Len()` helper.
2. `native/mochinode_net.c` — `httpClose()` called `closesocket()` without a
   graceful shutdown, which is an abortive close on Windows: pending data in
   the kernel send buffer was discarded and an RST was sent. The connection
   now does `shutdown(SD_SEND)` first. Measured before the fix: a 9882-byte
   `app.js` arrived as 9864 bytes and a 12 KB PNG arrived as 0 bytes; after
   the fix every asset is byte-exact.
