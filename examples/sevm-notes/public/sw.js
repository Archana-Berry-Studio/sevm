/*
 * examples/sevm-app/public/sw.js — service worker for the SEVM Notes PWA.
 *
 * Strategy:
 *  - Shell (static assets + navigation): stale-while-revalidate, so the app
 *    opens instantly offline from cache and updates in the background.
 *  - /api/* : network-first with a cache fallback, so the notes list renders
 *    when the engine is unreachable (the last successful response is kept).
 *
 * This worker runs in the browser, not the MochiBerry engine.
 */
const CACHE = 'sevm-notes-v1';
const SHELL = [
    '/',
    '/index.html',
    '/app.js',
    '/notes.css',
    '/manifest.webmanifest',
    '/icon.svg',
    '/vue.global.prod.js',
];

self.addEventListener('install', (event) => {
    event.waitUntil(
        caches.open(CACHE).then((c) => Promise.all(SHELL.map((u) => c.add(u))))
    );
    self.skipWaiting();
});

self.addEventListener('activate', (event) => {
    event.waitUntil(
        caches.keys().then((keys) =>
            Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k)))
        )
    );
    self.clients.claim();
});

self.addEventListener('fetch', (event) => {
    const url = new URL(event.request.url);
    if (event.request.method !== 'GET') return;   /* writes go straight through */

    /* API reads: network-first, cache fallback */
    if (url.pathname.startsWith('/api/')) {
        event.respondWith(
            fetch(event.request)
                .then((resp) => {
                    if (resp.ok) {
                        const copy = resp.clone();
                        caches.open(CACHE).then((c) => c.put(event.request, copy));
                    }
                    return resp;
                })
                .catch(() => caches.match(event.request))
        );
        return;
    }

    /* shell: stale-while-revalidate */
    event.respondWith(
        caches.match(event.request).then((cached) => {
            const network = fetch(event.request)
                .then((resp) => {
                    if (resp.ok) {
                        const copy = resp.clone();
                        caches.open(CACHE).then((c) => c.put(event.request, copy));
                    }
                    return resp;
                })
                .catch(() => cached);
            return cached || network;
        })
    );
});
