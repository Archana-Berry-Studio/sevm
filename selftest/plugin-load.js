/*
 * selftest/plugin-load.js — every native module must load and export symbols.
 *
 * The engine build is rewritten by other processes on this host, and a build
 * that drops -Wl,--export-all-symbols cannot resolve MBJS_* from the plugin
 * DLLs: every load then dies with "resolveEngine failed at
 * MBJS_NewCModule" or "mbjs_call_module ... initialization error". This test
 * is the canary for that — it loads each plugin and counts its exports.
 *
 * Run: mbjs.exe -m selftest/plugin-load.js
 */
import { loadNative } from '../node_modules/mochinode/index.js';

const MODULES = [
    'mochinode_net', 'mochitls', 'mochicrypto', 'mochifs', 'mochinode_sqlite',
    'mochizlib', 'mochiossil', 'mochisandbox', 'mochinode_dynalib',
    'mochiwasmy', 'mochiws', 'mochijsnative',
];

let failures = 0;
for (const m of MODULES) {
    try {
        const n = await loadNative(m);
        const n_exports = n && typeof n === 'object' ? Object.keys(n).length : 0;
        if (n_exports > 0) {
            print(`ok   ${m}.native: ${n_exports} exports`);
        } else {
            failures++;
            print(`FAIL ${m}.native: 0 exports`);
        }
    } catch (e) {
        failures++;
        print(`FAIL ${m}.native: ${String(e).slice(0, 90)}`);
    }
}

if (failures === 0) {
    print(`ALL_OK (${MODULES.length} native modules)`);
} else {
    print(`FAILED=${failures}`);
}