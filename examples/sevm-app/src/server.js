/*
 * examples/sevm-app/src/server.js - SEVM demo application.
 *
 * Serves:
 *   GET  /               -> Vue 3 SPA (static build, client-side)
 *   GET  /api/items      -> JSON rows from SQLite
 *   POST /api/items      -> INSERT into SQLite
 *   GET  /api/health     -> engine/runtime status
 *
 * The body lives inside an async main() (no top-level await) on purpose:
 * a host that drives the engine loop itself - the debug build, the AOT
 * runner - needs the entry promise back so it can interleave its own work
 * (e.g. the remote debugger) with the engine poll.
 */

import { require } from '../../../node_modules/mochinode/index.js';

async function main() {
    const express = (await require('express')).default;
    const sqlite = await require('sqlite');
    const fs = await require('fs');
    const dynalib = (await require('dynalib')).dynalib;

    const PUBLIC_DIR = 'public/';

    /*
     * The engine resolves relative paths against the process CWD, which is not
     * the same as the script's own directory when the server is launched from
     * elsewhere (a background runner, a service wrapper, ...). The candidates
     * below cover the usual launch shapes; the first one that exists wins.
     */
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
    db.run('CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, done INTEGER NOT NULL DEFAULT 0)');
    db.run("INSERT OR IGNORE INTO items(id,title,done) VALUES " +
           "(1,'Brew coffee for the RPi cluster',0)," +
           "(2,'Ship SEVM to GitHub',1)," +
           "(3,'Measure engine size < 1 MB',1)");

    const app = express();

    app.get('/', (req, res) => {
        const html = publicFile('index.html');
        if (html === null) {
            res.status(404).end('public/index.html not found (run from the app directory)');
            return;
        }
        res.end(html, 'text/html; charset=utf-8');
    });

    app.get('/vue.global.prod.js', (req, res) => {
        const js = publicFile('vue.global.prod.js');
        if (js === null) { res.status(404).end('vue not vendored'); return; }
        res.end(js, 'application/javascript');
    });

    app.get('/api/health', (req, res) => res.json({
        ok: true,
        engine: 'MochiBerry',
        sqlite: sqlite.version(),
        requested: req.url,
    }));

    /*
     * dynalib demo: inspect a PE image's export table and call a function in it
     * straight from the engine, through the C99 libdynalib subsystem.
     */
    app.get('/api/dynalib', (req, res) => {
        const target = req.query.file || 'C:/Python314/DLLs/sqlite3.dll';
        try {
            const mod = dynalib(target, null, null);
            const exports = mod.exports();
            const out = {
                file: target,
                base: mod.base,
                exportCount: exports.length,
                sample: exports.slice(0, 5).map(e => ({
                    name: e.name,
                    rva: '0x' + e.rva.toString(16),
                })),
            };
            mod.close();
            res.json(out);
        } catch (e) {
            res.status(500).json({ error: String(e && e.message ? e.message : e) });
        }
    });

    /*
     * Float demo: a real CRT call with a floating-point argument and return,
     * routed through the trampoline xmm path - proof the SSE slots work.
     */
    app.get('/api/dynalib/float', (req, res) => {
        const v = Number(req.query.v || 2);
        try {
            const sqrt = dynalib('C:/Windows/System32/msvcrt.dll', 0,
                                 'sqrt', 'f64(f64)');
            const out = {
                fn: 'msvcrt!sqrt',
                arg: v,
                result: sqrt.call(v),
            };
            sqrt.close();
            res.json(out);
        } catch (e) {
            res.status(500).json({ error: String(e && e.message ? e.message : e) });
        }
    });

    app.get('/api/items', (req, res) => {
        const q = req.query.q;
        const rows = q
            ? db.all("SELECT * FROM items WHERE title LIKE '%" + q + "%'")
            : db.all('SELECT * FROM items ORDER BY id');
        res.json({ count: rows.length, items: rows });
    });

    app.post('/api/items', (req, res) => {
        let title = 'untitled';
        try { title = JSON.parse(req.body || '{}').title || title; } catch (e) { /* noop */ }
        db.run("INSERT INTO items(title) VALUES ('" + title.replace(/'/g, "''") + "')");
        res.status(201).json({ ok: true, title });
    });

    const PORT = 8080;
    await app.listen(PORT, () => {
        console.log('SEVM app listening on http://127.0.0.1:' + PORT);
        console.log('  engine  : MochiBerry');
        console.log('  sqlite  : ' + sqlite.version());
    });
}

main();
