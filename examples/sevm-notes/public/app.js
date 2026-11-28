/*
 * examples/sevm-notes/public/app.js — Vue 3 notes app (offline-first).
 *
 * Reads are network-first through the service worker (cached fallback), so a
 * reload with the server down still renders the last-known notes. Writes that
 * cannot reach the server are queued in an IndexedDB outbox and replayed on
 * reconnect — the queue is visible in the UI, never silently dropped.
 */

const { createApp, ref, computed, onMounted } = Vue;

const COLORS = ['blue', 'green', 'amber', 'red', 'purple'];

/* ---------- tiny IndexedDB helpers ---------- */
function idbOpen() {
    return new Promise((resolve, reject) => {
        const req = indexedDB.open('sevm-notes', 1);
        req.onupgradeneeded = () => {
            const db = req.result;
            if (!db.objectStoreNames.contains('notes')) db.createObjectStore('notes', { keyPath: 'id' });
            if (!db.objectStoreNames.contains('outbox')) db.createObjectStore('outbox', { keyPath: 'seq', autoIncrement: true });
        };
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
    });
}

function idbAll(db, store) {
    return new Promise((resolve, reject) => {
        const tx = db.transaction(store, 'readonly');
        const req = tx.objectStore(store).getAll();
        req.onsuccess = () => resolve(req.result || []);
        req.onerror = () => reject(req.error);
    });
}

function idbPut(db, store, value) {
    return new Promise((resolve, reject) => {
        const tx = db.transaction(store, 'readwrite');
        tx.objectStore(store).put(value);
        tx.oncomplete = () => resolve(value);
        tx.onerror = () => reject(tx.error);
    });
}

function idbDelete(db, store, key) {
    return new Promise((resolve, reject) => {
        const tx = db.transaction(store, 'readwrite');
        tx.objectStore(store).delete(key);
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
    });
}

function idbClear(db, store) {
    return new Promise((resolve, reject) => {
        const tx = db.transaction(store, 'readwrite');
        tx.objectStore(store).clear();
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
    });
}

createApp({
    setup() {
        const notes = ref([]);
        const health = ref({ engine: '…', sqlite: '…' });
        const online = ref(navigator.onLine);
        const q = ref('');
        const newTitle = ref('');
        const newBody = ref('');
        const newColor = ref('blue');
        const colors = COLORS;
        const pendingCount = ref(0);
        const dbReady = ref(null);

        /* Open IDB lazily — the initial render must not wait on IndexedDB
         * (in a real browser it is fast, but never block the notes list on it). */
        async function ensureDb() {
            if (dbReady.value) return dbReady.value;
            try { dbReady.value = await idbOpen(); }
            catch (e) { dbReady.value = null; }
            return dbReady.value;
        }
        const status = ref('');

        const visible = computed(() => {
            const needle = q.value.trim().toLowerCase();
            if (!needle) return notes.value;
            return notes.value.filter(n =>
                String(n.title || '').toLowerCase().includes(needle) ||
                String(n.body || '').toLowerCase().includes(needle));
        });

        function fmt(ts) {
            try { return new Date(Number(ts)).toLocaleString(); } catch (e) { return String(ts); }
        }

        async function mirrorWrite() {
            const db = dbReady.value;
            if (!db) return;
            try {
                await idbClear(db, 'notes');
                for (const n of notes.value) await idbPut(db, 'notes', n);
            } catch (e) { /* mirror is best-effort */ }
        }

        async function mirrorRead() {
            const db = dbReady.value;
            if (!db) return [];
            try { return await idbAll(db, 'notes'); } catch (e) { return []; }
        }

        async function outboxPush(op) {
            const db = dbReady.value;
            if (!db) return;
            await idbPut(db, 'outbox', op);
            await refreshPending();
        }

        async function outboxAll() {
            const db = dbReady.value;
            if (!db) return [];
            try { return await idbAll(db, 'outbox'); } catch (e) { return []; }
        }

        async function refreshPending() {
            const ops = await outboxAll();
            pendingCount.value = ops.length;
        }

        /* network-first fetch; throws OFFLINE when the server is unreachable */
        async function api(method, path, body) {
            const opts = { method, headers: {} };
            if (body !== undefined) {
                opts.headers['content-type'] = 'application/json';
                opts.body = JSON.stringify(body);
            }
            let r;
            try {
                r = await fetch(path, opts);
            } catch (e) {
                const err = new Error('offline');
                err.offline = true;
                throw err;
            }
            if (!r.ok) {
                const err = new Error('http ' + r.status);
                err.status = r.status;
                throw err;
            }
            try { return await r.json(); } catch (e) { return {}; }
        }

        async function load() {
            try {
                const j = await api('GET', '/api/notes');
                notes.value = j.notes || [];
                await mirrorWrite();
                status.value = '';
            } catch (e) {
                const local = await mirrorRead();
                notes.value = local;
                status.value = e.offline ? 'offline — showing cached notes' : ('error: ' + e.message);
            }
            try {
                health.value = await api('GET', '/api/health');
            } catch (e) { /* health is optional */ }
        }

        async function create() {
            const title = newTitle.value.trim();
            if (!title) return;
            const payload = { title, body: newBody.value, color: newColor.value };
            try {
                const j = await api('POST', '/api/notes', payload);
                await load();
                newTitle.value = '';
                newBody.value = '';
            } catch (e) {
                if (!e.offline) { status.value = 'error: ' + e.message; return; }
                await outboxPush({ type: 'create', payload });
                notes.value = notes.value.concat([Object.assign({ id: 'tmp-' + Date.now(), pinned: 0, updated_at: Date.now() }, payload)]);
                await mirrorWrite();
                newTitle.value = '';
                newBody.value = '';
                status.value = 'offline — note queued';
            }
        }

        async function save(n) {
            const payload = { title: n.title, body: n.body, color: n.color, pinned: n.pinned };
            try {
                await api('PUT', '/api/notes/' + n.id, payload);
                n.updated_at = Date.now();
                await mirrorWrite();
            } catch (e) {
                if (!e.offline) { status.value = 'error: ' + e.message; return; }
                await outboxPush({ type: 'update', id: n.id, payload });
                n.updated_at = Date.now();
                await mirrorWrite();
                status.value = 'offline — change queued';
            }
        }

        async function remove(n) {
            try {
                await api('DELETE', '/api/notes/' + n.id);
                notes.value = notes.value.filter(x => x.id !== n.id);
                await mirrorWrite();
            } catch (e) {
                if (!e.offline) { status.value = 'error: ' + e.message; return; }
                await outboxPush({ type: 'delete', id: n.id });
                notes.value = notes.value.filter(x => x.id !== n.id);
                await mirrorWrite();
                status.value = 'offline — deletion queued';
            }
        }

        function togglePin(n) {
            n.pinned = n.pinned ? 0 : 1;
            save(n);
        }

        function setColor(n, c) {
            n.color = c;
            save(n);
        }

        function search() { /* filtering is local (computed) — works offline */ }

        async function flushOutbox() {
            if (!navigator.onLine) return;
            const ops = await outboxAll();
            if (!ops.length) return;
            const db = dbReady.value;
            for (const op of ops) {
                try {
                    if (op.type === 'create') await api('POST', '/api/notes', op.payload);
                    else if (op.type === 'update') await api('PUT', '/api/notes/' + op.id, op.payload);
                    else if (op.type === 'delete') await api('DELETE', '/api/notes/' + op.id);
                    await idbDelete(db, 'outbox', op.seq);
                } catch (e) {
                    if (e.offline) break;
                    /* a permanent server rejection is dropped, not retried forever */
                    await idbDelete(db, 'outbox', op.seq);
                }
            }
            await refreshPending();
            await load();
        }

        onMounted(async () => {
            /* show the notes first — do not gate the render on IndexedDB */
            await load();
            await ensureDb();
            await refreshPending();
            await flushOutbox();

            window.addEventListener('online', async () => {
                online.value = true;
                await flushOutbox();
            });
            window.addEventListener('offline', () => { online.value = false; });

            if ('serviceWorker' in navigator) {
                try {
                    await navigator.serviceWorker.register('/sw.js');
                } catch (e) { status.value = 'service worker registration failed: ' + e.message; }
            }
        });

        return {
            notes, visible, health, online, q, newTitle, newBody, newColor, colors,
            pendingCount, status, fmt, create, save, remove, togglePin, setColor, search,
        };
    },
}).mount('#app');
