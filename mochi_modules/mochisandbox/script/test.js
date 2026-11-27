/*
 * mochisandbox — selftest. Run with the engine:
 *   mbjs.exe -m mochisandbox/script/test.js   (cwd: repo root)
 * Expects: PASS realm-isolation, arith, setGlobal, exception, budget-kill,
 *          strict-deny, free.
 */
import { sandbox, init } from './index.js';

const results = [];
function check(name, cond) {
  results.push([name, !!cond]);
  console.log((cond ? 'PASS ' : 'FAIL ') + name);
}

async function main() {
  await init();

  /* 1. realm isolation: sandbox cannot see host globals */
  const sb = await sandbox({ budgetMs: 5000 });
  let out = sb.run('typeof process');
  check('realm-isolation (typeof process)', out === 'undefined' || out === '"undefined"');
  out = sb.run('typeof dynalib');
  check('realm-isolation (typeof dynalib)', out === 'undefined' || out === '"undefined"');

  /* 2. arithmetic + JSON result */
  out = sb.run('1 + 2 * 3');
  check('arith (7)', out === '7');
  out = sb.run('({a:[1,2,3], b:"x"})');
  check('object json', out === '{"a":[1,2,3],"b":"x"}');

  /* 3. setGlobal primitive injection */
  sb.setGlobal('greeting', 'halo');
  sb.setGlobal('n', 42);
  out = sb.run('greeting + " " + n');
  check('setGlobal primitive', out === '"halo 42"');

  /* 4. exception propagates as a host throw */
  let threw = false, msg = '';
  try { sb.run('throw new Error("boom");'); } catch (e) { threw = true; msg = String(e && e.message || e); }
  check('exception throw', threw && msg.includes('boom'));

  /* 5. budget kill: infinite loop aborted */
  const sb2 = await sandbox({ budgetMs: 300 });
  let killed = false;
  try { sb2.run('for(;;){}'); } catch (e) { killed = true; }
  check('budget-kill infinite loop', killed);
  sb2.free();

  /* 6. strict mode denies dynamic code */
  const sb3 = await sandbox({ budgetMs: 5000, strict: true });
  let denied = false;
  try { sb3.run('eval("1+1")'); } catch (e) { denied = true; }
  check('strict denies eval', denied);
  denied = false;
  try { sb3.run('Function("return 1")()'); } catch (e) { denied = true; }
  check('strict denies Function', denied);
  denied = false;
  try { sb3.run('({}).constructor.constructor("return 2")()'); } catch (e) { denied = true; }
  check('strict denies constructor chain', denied);
  sb3.free();

  /* 7. clone: JSON deep-copy into the realm (globalThis.__clone__) */
  const cp = sb.clone({ a: [1, 2, 3], s: 'x' });
  check('clone deep copy', JSON.stringify(cp) === '{"a":[1,2,3],"s":"x"}');
  check('clone realm __clone__', sb.run('__clone__.a[1]') === '2');
  const cpStr = sb.clone('deep');
  check('clone string', cpStr === 'deep');

  /* 8. free + count (no crash) */
  sb.free();
  check('free (no crash)', true);

  const fails = results.filter(r => !r[1]).length;
  console.log('---');
  if (fails === 0) { console.log('ALL OK'); return; }
  throw new Error('FAILURES: ' + fails);
}

main().catch(e => { console.log('FATAL: ' + (e && e.message || e)); throw e; });
