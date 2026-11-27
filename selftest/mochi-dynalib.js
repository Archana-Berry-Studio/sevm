/* selftest/mochi-dynalib.js — verify dynalib can resolve + call a real C
 * function from a system/native DLL (the "dynalib-pingc" gap). */
import { dynalib } from '../node_modules/dynalib/index.js';

let failures = 0;
function check(name, cond) { if (cond) return; failures++; console.log('FAIL ' + name); }

try {
    /* GetCurrentProcessId() -> DWORD. A pure numeric ping against a system
       DLL: no string-pointer deref, no search-path dependency. */
    const mod = dynalib('kernel32.dll', null, null, null);   /* module handle */
    check('module opened', mod && typeof mod.base === 'number');

    const fn = mod.sym('GetCurrentProcessId', 0, 'u32()');
    check('symbol resolved', typeof fn.call === 'function');

    const v = fn.call();
    console.log('  GetCurrentProcessId() ->', v);
    check('returned a PID', typeof v === 'number' && v > 0);

    /* a string-returning function resolved but we only check the pointer is non-null */
    const ver = mod.sym('GetCommandLineA', 0, 'ptr()');
    const p = ver.call();
    console.log('  GetCommandLineA() ptr ->', typeof p, p);
    check('ptr returned non-null', typeof p === 'number' && p !== 0);

    mod.close();
    check('module closed', true);
} catch (e) {
    console.log('DYNALIB ERROR:', e.message, '| stage:', e.stage);
    failures++;
}

console.log(failures === 0 ? '\nDYNALIB_ALL_OK' : '\nDYNALIB_FAILURES=' + failures);
if (failures) throw new Error('dynalib failures: ' + failures);
