/* selftest/mochi-process.js — process/os metrics: memoryUsage, pid, totalmem,
 * freemem are real (from mbjs:os.memoryInfo + dynalib GetCurrentProcessId). */
import * as proc from '../node_modules/mochiprocess/index.js';
import * as os from '../node_modules/mochios/index.js';

let failures = 0;
function check(name, cond) { if (cond) return; failures++; console.log('FAIL ' + name); }

const mu = proc.memoryUsage();
console.log('  memoryUsage:', JSON.stringify(mu));
check('rss > 0', typeof mu.rss === 'number' && mu.rss > 0);
check('heapUsed > 0', typeof mu.heapUsed === 'number' && mu.heapUsed > 0);
check('heapTotal >= heapUsed', mu.heapTotal >= mu.heapUsed);

console.log('  pid:', proc.pid);
check('pid is real (> 1)', typeof proc.pid === 'number' && proc.pid > 1);

const tm = os.totalmem();
const fm = os.freemem();
console.log('  totalmem:', tm, 'freemem:', fm);
check('totalmem > 0', typeof tm === 'number' && tm > 0);
check('freemem > 0', typeof fm === 'number' && fm > 0);
check('totalmem >= freemem', tm >= fm);

console.log(failures === 0 ? '\nPROCESS_ALL_OK' : '\nPROCESS_FAILURES=' + failures);
if (failures) throw new Error('process failures: ' + failures);
