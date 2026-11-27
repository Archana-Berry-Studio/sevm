/* selftest/fixtures/worker_fact.js — worker side of the worker_threads test.
 * Runs inside its own engine runtime+context, loaded by os.Worker.
 * Uses the public mochiworker_threads shim surface (parentPort, isMainThread). */
import { parentPort, isMainThread } from '../../node_modules/mochiworker_threads/index.js';

if (parentPort === null) throw new Error('worker: parentPort is null inside a worker');
if (isMainThread !== false) throw new Error('worker: isMainThread must be false inside a worker');

parentPort.on('message', (msg) => {
  const n = msg.data.n;
  let f = 1;
  for (let i = 2; i <= n; i++) f *= i;
  parentPort.postMessage({ fact: f, n, isMainThread });
});
