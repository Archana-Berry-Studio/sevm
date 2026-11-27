# sevm-app

SEVM demo application: **Vue 3 SPA** in front, **Express-shaped router** in the
middle, **SQLite** in the back — all running on top of the MochiBerry engine.

## Run

```bash
cd examples/sevm-app
mbjs.exe -m src/server.js
```

The server listens on `http://127.0.0.1:8080` (bound to `0.0.0.0`, so it can
also be reached from other devices on the same LAN).

## Layout

```
sevm-app/
├── package.json
├── src/server.js          # application entry point
└── public/
    ├── index.html         # Vue 3 SPA (static build, no runtime compiler)
    └── vue.global.prod.js # Vue 3.5.46 vendored (offline)
```

## Endpoints

| method | path | purpose |
|--------|------|---------|
| GET | `/` | Vue 3 SPA |
| GET | `/vue.global.prod.js` | Vue bundle (offline) |
| GET | `/api/health` | `{ ok, engine, sqlite, requested }` |
| GET | `/api/dynalib?file=` | export table of a DLL via `dynalib()` |
| GET | `/api/items?q=` | `{ count, items[] }` from SQLite |
| POST | `/api/items` | insert a new item, responds `201` |

## Notes

- Database: `app.db` in the application folder (created automatically).
- Handlers use the `require()` API from the `mochinode` package, so writing
  the application looks like ordinary Node.js code.
