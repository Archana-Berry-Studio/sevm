/*
 * examples/sevm-app/src/server.js — Offline-First Notes PWA.
 *
 * Serves:
 *   GET   /                     -> Vue 3 notes SPA (static build, client-side)
 *   GET   /app.js               -> Vue app logic (cacheable by the SW)
 *   GET   /notes.css            -> app styles
 *   GET   /manifest.webmanifest -> PWA manifest
 *   GET   /sw.js                -> service worker
 *   GET   /icon-192.png|512.png -> PWA icons
 *   GET   /vue.global.prod.js   -> Vue bundle (vendored, offline)
 *   GET   /api/health           -> { ok, engine, sqlite, requested }
 *   GET   /api/notes?q=&pinned= -> { count, notes[] }
 *   POST  /api/notes            -> create { title, body, color } -> 201 { id }
 *   PUT   /api/notes/:id        -> update { title, body, color, pinned }
 *   DELETE /api/notes/:id       -> delete
 *
 * The body lives inside an async main() (no top-level await) on purpose:
 * a host that drives the engine loop itself needs the entry promise back.
 */

import { require } from '../../../node_modules/mochinode/index.js';

async function main() {
    const express = (await require('express')).default;
    const sqlite = await require('sqlite');
    const fs = await require('fs');

    const PUBLIC_CANDIDATES = [
        'public/',
        '../public/',
        'examples/sevm-app/public/',
        './',
    ];

    function publicFile(name, mime) {
        for (const dir of PUBLIC_CANDIDATES) {
            const path = dir + name;
            try {
                const data = fs.readFileSync(path);
                if (data !== null && data !== undefined && String(data).length)
                    return String(data);
            } catch (e) { /* try the next candidate */ }
        }
        return null;
    }

    const db = sqlite.open('app.db');
    db.run('CREATE TABLE IF NOT EXISTS notes (' +
           'id INTEGER PRIMARY KEY AUTOINCREMENT, ' +
           'title TEXT NOT NULL, ' +
           'body TEXT NOT NULL DEFAULT \'\', ' +
           'color TEXT NOT NULL DEFAULT \'blue\', ' +
           'pinned INTEGER NOT NULL DEFAULT 0, ' +
           'created_at INTEGER NOT NULL, ' +
           'updated_at INTEGER NOT NULL)');
    const now = Date.now();
    db.run("INSERT OR IGNORE INTO notes(id,title,body,color,pinned,created_at,updated_at) VALUES " +
           "(1,'SEVM PWA is live','Offline-first notes served by the MochiBerry engine.','blue',1," + now + "," + now + ")," +
           "(2,'Try offline','Stop the server (Ctrl+C) and reload — the shell is precached, notes render from cache.','green',0," + now + "," + now + ")," +
           "(3,'Roadmap','Queue writes in IndexedDB when offline, retry on reconnect.','amber',0," + now + "," + now + ")");

    const app = express();

    /* static shell — all cacheable by the service worker */
    function serveStatic(name, mime) {
        app.get('/' + name, (req, res) => {
            const data = publicFile(name, mime);
            if (data === null) { res.status(404).end(name + ' not found'); return; }
            res.end(data, mime);
        });
    }
    /* the app shell lives at the root; also keep /index.html working */
    app.get('/', (req, res) => {
        const data = publicFile('index.html', 'text/html; charset=utf-8');
        if (data === null) { res.status(404).end('index.html not found'); return; }
        res.end(data, 'text/html; charset=utf-8');
    });
    serveStatic('index.html', 'text/html; charset=utf-8');
    serveStatic('app.js', 'application/javascript');
    serveStatic('notes.css', 'text/css; charset=utf-8');
    serveStatic('manifest.webmanifest', 'application/manifest+json');
    serveStatic('sw.js', 'application/javascript');
    serveStatic('vue.global.prod.js', 'application/javascript');
    serveStatic('icon-192.png', 'image/png');
    serveStatic('icon-512.png', 'image/png');

    app.get('/api/health', (req, res) => res.json({
        ok: true,
        engine: 'MochiBerry',
        sqlite: sqlite.version(),
        requested: req.url,
    }));

    const COLORS = { blue: 1, green: 1, amber: 1, red: 1, purple: 1 };
    function normalizeColor(c) { return COLORS[c] ? c : 'blue'; }

    app.get('/api/notes', (req, res) => {
        const q = req.query.q;
        let rows;
        if (q) {
            rows = db.all('SELECT * FROM notes WHERE title LIKE ? OR body LIKE ? ' +
                          'ORDER BY pinned DESC, updated_at DESC', '%' + q + '%', '%' + q + '%');
        } else {
            rows = db.all('SELECT * FROM notes ORDER BY pinned DESC, updated_at DESC');
        }
        res.json({ count: rows.length, notes: rows });
    });

    app.post('/api/notes', (req, res) => {
        let b = {};
        try { b = JSON.parse(req.body || '{}'); } catch (e) { /* noop */ }
        const title = String(b.title || '').trim();
        if (!title) { res.status(400).json({ error: 'title required' }); return; }
        const body = String(b.body || '');
        const color = normalizeColor(b.color);
        const now = Date.now();
        db.run('INSERT INTO notes(title,body,color,pinned,created_at,updated_at) VALUES(?,?,?,0,?,?)',
               title, body, color, now, now);
        const row = db.get('SELECT last_insert_rowid() AS id');
        res.status(201).json({ ok: true, id: row.id });
    });

    app.put('/api/notes/:id', (req, res) => {
        const id = Number(req.params.id);
        let b = {};
        try { b = JSON.parse(req.body || '{}'); } catch (e) { /* noop */ }
        const row = db.get('SELECT * FROM notes WHERE id=?', id);
        if (!row) { res.status(404).json({ error: 'not found' }); return; }
        const title = (b.title !== undefined) ? String(b.title).trim() : row.title;
        const body = (b.body !== undefined) ? String(b.body) : row.body;
        const color = (b.color !== undefined) ? normalizeColor(b.color) : row.color;
        const pinned = (b.pinned !== undefined) ? (b.pinned ? 1 : 0) : row.pinned;
        if (!title) { res.status(400).json({ error: 'title required' }); return; }
        db.run('UPDATE notes SET title=?, body=?, color=?, pinned=?, updated_at=? WHERE id=?',
               title, body, color, pinned, Date.now(), id);
        res.json({ ok: true, id });
    });

    app.delete('/api/notes/:id', (req, res) => {
        const id = Number(req.params.id);
        const row = db.get('SELECT * FROM notes WHERE id=?', id);
        if (!row) { res.status(404).json({ error: 'not found' }); return; }
        db.run('DELETE FROM notes WHERE id=?', id);
        res.json({ ok: true, id });
    });

    const PORT = 8080;
    await app.listen(PORT, () => {
        console.log('SEVM notes PWA listening on http://127.0.0.1:' + PORT);
        console.log('  engine  : MochiBerry');
        console.log('  sqlite  : ' + sqlite.version());
    });
}

main();
