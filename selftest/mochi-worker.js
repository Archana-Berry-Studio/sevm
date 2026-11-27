/* selftest/mochi-worker.js — real worker_threads round trip through the public
 * node_modules/mochiworker_threads shim (backed by the engine's own Worker).
 * Parent posts {n:10}; worker computes 10! and posts back; parent verifies.
 *
 * RUN UNDER `timeout`: creating a Worker keeps the engine alive and the
 * process does not exit on its own (no JS-visible exit API), so the verdict is
 * the printed WORKER_ALL_OK line, not the exit code. The regression harness
 * wraps every selftest in `timeout` and greps for ALL_OK. */
import { Worker, isMainThread, parentPort } from '../node_modules/mochiworker_threads/index.js';
import * as os from 'mbjs:os';

let failures = 0;
function check(name, cond) { if (cond) return; failures++; console.log('FAIL ' + name); }

/* main thread surface */
check('isMainThread is true here', isMainThread === true);
check('parentPort is null here', parentPort === null);
check('Worker is a constructor', typeof Worker === 'function');

const script = os.getcwd()[0] + '/selftest/fixtures/worker_fact.js';
const w = new Worker(script, { resourceLimits: { maxOldGenerationSizeMb: 512 } });
check('threadId is null (no engine thread id)', w.threadId === null);
check('resourceLimits passthrough', JSON.stringify(w.resourceLimits) === JSON.stringify({ maxOldGenerationSizeMb: 512 }));

let reply = null;
w.on('message', (msg) => { reply = msg.data; });
w.postMessage({ n: 10 });

for (let i = 0; i < 1000 && reply === null; i++) await new Promise((r) => os.setTimeout(r, 10));

console.log('  reply: ' + JSON.stringify(reply));
check('worker replied', reply !== null);
check('fact(10) === 3628800', reply && reply.fact === 3628800);
check('worker echoed n', reply && reply.n === 10);
check('worker saw isMainThread false', reply && reply.isMainThread === false);

const t = await w.terminate();
check('terminate resolves to 0', t === 0);

console.log(failures === 0 ? '\nWORKER_ALL_OK' : '\nWORKER_FAILURES=' + failures);
if (failures) throw new Error('worker failures: ' + failures);
