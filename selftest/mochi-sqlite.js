/* selftest/mochi-sqlite.js — parameterized statements (bind/reset).
 * Covers the new bindText/bindInt/bindDouble/bindNull/reset/clearBindings/
 * bindParameterCount exports of mochinode_sqlite.dll. */
import { loadNative } from '../node_modules/mochinode/index.js';

const native = await loadNative('mochinode_sqlite.dll');
let failures = 0;
function check(name, cond) {
    if (cond) console.log('ok   ' + name);
    else { console.log('FAIL ' + name); failures++; }
}

/* export surface */
const keys = Object.keys(native).sort();
check('22 exports', keys.length === 22);
for (const e of ['bindText', 'bindInt', 'bindDouble', 'bindNull', 'reset',
                 'clearBindings', 'bindParameterCount'])
    check('has ' + e, typeof native[e] === 'function');

/* in-memory db */
const db = native.open(':memory:');
check('open', db > 0);
const rc = native.exec(db, 'CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, score REAL)');
check('create table', rc === 0);

/* parameterized INSERT */
const ins = native.prepare(db, 'INSERT INTO t(name, score) VALUES(?, ?)');
check('prepare insert', ins > 0);
check('bindParameterCount = 2', native.bindParameterCount(ins) === 2);
check('bindText ok', native.bindText(ins, 1, 'berry') === 0);
check('bindDouble ok', native.bindDouble(ins, 2, 9.5) === 0);
check('step row (101 done)', native.step(ins) === 101);
native.reset(ins);
check('bindText reuse', native.bindText(ins, 1, 'raspberry') === 0);
check('bindDouble reuse', native.bindDouble(ins, 2, 4.25) === 0);
check('step row 2 (101)', native.step(ins) === 101);
native.finalize(ins);

/* parameterized SELECT with WHERE ? */
const sel = native.prepare(db, 'SELECT name, score FROM t WHERE id = ?');
check('prepare select', sel > 0);
check('bindInt ok', native.bindInt(sel, 1, 1) === 0);
check('step select (100)', native.step(sel) === 100);
check('col name', native.columnText(sel, 0) === 'berry');
check('col score', native.columnDouble(sel, 1) === 9.5);
check('step done (101)', native.step(sel) === 101);

/* clearBindings */
native.reset(sel);
native.clearBindings(sel);
check('clearBindings ok', true);
native.finalize(sel);

/* bindNull */
const nins = native.prepare(db, 'INSERT INTO t(name, score) VALUES(?, ?)');
check('bindNull ok', native.bindNull(nins, 2) === 0);
check('bindText 2nd', native.bindText(nins, 1, 'nullable') === 0);
check('step null row (101)', native.step(nins) === 101);
native.finalize(nins);
const q = native.prepare(db, 'SELECT count(*) FROM t');
native.step(q);
check('3 rows inserted', native.columnInt(q, 0) === 3);
native.finalize(q);

/* exec error -> MBJS_EXCEPTION (throw contract) */
let threw = false;
try { native.exec(db, 'SELECT * FROM no_such_table'); }
catch (e) { threw = true; }
check('exec error throws (MBJS_EXCEPTION)', threw);

native.close(db);
console.log(failures === 0 ? '\nSQLITE_ALL_OK' : '\nSQLITE_FAILURES=' + failures);
