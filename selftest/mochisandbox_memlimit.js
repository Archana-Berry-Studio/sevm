/*
 * selftest/mochisandbox_memlimit.js — memory ceiling probe (isolated process:
 * an OOM handler could abort, so it must not run inside the main selftest).
 * Run: mbjs.exe -m selftest/mochisandbox_memlimit.js
 */
import { sandbox } from '../node_modules/mochisandbox/index.js';

function trial(memLimit, src) {
    const sb = sandbox({ memLimit, budgetMs: 5000 });
    let result;
    try {
        result = 'nothrow:' + sb.run(src);
    } catch (e) {
        result = 'caught:' + String(e && e.message || e).slice(0, 60);
    }
    sb.free();
    return result;
}

console.log('memLimit=1MB   new Array(1e7).fill(0) :', trial(1024 * 1024, 'new Array(1e7).fill(0).length'));
console.log('memLimit=1MB   new Array(1e6).fill(0) :', trial(1024 * 1024, 'new Array(1e6).fill(0).length'));
console.log('memLimit=4MB   new Array(1e7).fill(0) :', trial(4 * 1024 * 1024, 'new Array(1e7).fill(0).length'));
console.log('memLimit=4KB   "a".repeat(2e5)        :', trial(4 * 1024, '"a".repeat(2e5).length'));
console.log('no memLimit    new Array(1e7).fill(0) :', trial(undefined, 'new Array(1e7).fill(0).length'));
console.log('DONE (process survived)');
