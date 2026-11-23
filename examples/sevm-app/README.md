# sevm-app

Demo aplikasi SEVM: **Vue 3 SPA** di depan, **Express-shaped router** di
tengah, **SQLite** di belakang — semuanya berjalan di atas engine
MochiBerry.

## Run

```bash
cd examples/sevm-app
mbjs.exe -m src/server.js
```

Server mendengarkan pada `http://127.0.0.1:8080` (bind `0.0.0.0`, jadi juga bisa
dibuka dari perangkat lain pada LAN yang sama).

## Layout

```
sevm-app/
├── package.json
├── src/server.js          # entry point aplikasi
└── public/
    ├── index.html         # Vue 3 SPA (build statis, tanpa compiler runtime)
    └── vue.global.prod.js # Vue 3.5.46 vendored (offline)
```

## Endpoint

| method | path | fungsi |
|--------|------|--------|
| GET | `/` | SPA Vue 3 |
| GET | `/vue.global.prod.js` | bundle Vue (offline) |
| GET | `/api/health` | `{ ok, engine, sqlite, requested }` |
| GET | `/api/dynalib?file=` | export table sebuah DLL lewat `dynalib()` |
| GET | `/api/items?q=` | `{ count, items[] }` dari SQLite |
| POST | `/api/items` | insert item baru, balas `201` |

## Catatan

- Database: `app.db` di folder aplikasi (dibuat otomatis).
- Handler memakai API `require()` dari paket `mochinode`, jadi penulisan
  aplikasi terlihat seperti kode NodeJS biasa.
