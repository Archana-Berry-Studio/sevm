/* dynalib_test.js — end-to-end checks for the SEVM dynamic loader.
 *
 * Covers: integer contracts, stack arguments, 64-bit returns (BigInt),
 * floating-point arguments and returns, and a real Win32/CRT call
 * through msvcrt!sqrt — no C wrapper anywhere in this file.            */

import { dynalib } from '../node_modules/dynalib/index.js';

const DBTEST = 'C:/Users/Administrator/AppData/Local/hermes/cache/scratch/dbtest.dll';
let pass = 0, fail = 0;

function check(label, got, want) {
    const okv = (got === want) ||
        (typeof got === 'number' && typeof want === 'number' &&
         Math.abs(got - want) < 1e-9);
    if (okv) { pass++; console.log('PASS ' + label + ' = ' + got); }
    else { fail++; console.log('FAIL ' + label + ' = ' + got + ' (want ' + want + ')'); }
}

/* --- integers ------------------------------------------------------ */
{
    const add = dynalib(DBTEST, 0, 'dbtest_add', 'i32(i32, i32)');
    check('dbtest_add(3,4)', add.call(3, 4), 7);
    add.close();
}

/* --- stack arguments (5..12) ---------------------------------------- */
{
    const sum5 = dynalib(DBTEST, 0, 'dbtest_sum5', 'i32(i32,i32,i32,i32,i32)');
    check('dbtest_sum5(1..5)', sum5.call(1, 2, 3, 4, 5), 15);
    sum5.close();

    const sum12 = dynalib(DBTEST, 0, 'dbtest_sum12',
        'i32(i32,i32,i32,i32,i32,i32,i32,i32,i32,i32,i32,i32)');
    check('dbtest_sum12(1..12)', sum12.call(1,2,3,4,5,6,7,8,9,10,11,12), 78);
    sum12.close();

    const mix7 = dynalib(DBTEST, 0, 'dbtest_mix7',
        'i32(i32,i32,i32,i32,i32,i32,i32)');
    check('dbtest_mix7 keeps order', mix7.call(1,2,3,4,5,6,7), 1234567);
    mix7.close();
}

/* --- 64-bit returns ------------------------------------------------- */
{
    const ru = dynalib(DBTEST, 0, 'dbtest_retu64', 'u64(u64)');
    const v = ru.call(5n);
    check('dbtest_retu64(5n) is a BigInt', typeof v, 'bigint');
    check('dbtest_retu64(5n)', v, 4294967301n);
    ru.close();
}

/* --- floating point -------------------------------------------------- */
{
    const fadd = dynalib(DBTEST, 0, 'dbtest_fadd', 'f64(f64, f64)');
    check('dbtest_fadd(2.5, 4.25)', fadd.call(2.5, 4.25), 6.75);
    fadd.close();

    const fmul3 = dynalib(DBTEST, 0, 'dbtest_fmul3', 'f64(f64, f64, f64)');
    check('dbtest_fmul3(2,3,4)', fmul3.call(2, 3, 4), 24);
    fmul3.close();

    const fmix = dynalib(DBTEST, 0, 'dbtest_fmix5',
        'f64(i32, f64, i32, f64, f64)');
    check('dbtest_fmix5 mixed classes', fmix.call(1, 2, 3, 4, 5), 1239);
    fmix.close();

    const fsum9 = dynalib(DBTEST, 0, 'dbtest_fsum9',
        'f64(f64,f64,f64,f64,f64,f64,f64,f64,f64)');
    check('dbtest_fsum9(1..9)', fsum9.call(1,2,3,4,5,6,7,8,9), 45);
    fsum9.close();

    const fhalf = dynalib(DBTEST, 0, 'dbtest_fhalf', 'f32(f32)');
    check('dbtest_fhalf(7.0)', fhalf.call(7), 3.5);
    fhalf.close();

    const fscale = dynalib(DBTEST, 0, 'dbtest_fscale', 'f64(f64, i32)');
    check('dbtest_fscale(1.5, 4)', fscale.call(1.5, 4), 6);
    fscale.close();
}

/* --- a real Win32 / CRT call, no C wrapper --------------------------- */
{
    const sqrt = dynalib('C:/Windows/System32/msvcrt.dll', 0, 'sqrt',
                         'f64(f64)');
    check('msvcrt sqrt(2)', sqrt.call(2), 1.4142135623730951);
    sqrt.close();

    const tick = dynalib('C:/Windows/System32/kernel32.dll', 0,
                         'GetTickCount64', 'u64()');
    const t = tick.call();
    check('GetTickCount64 is a BigInt', typeof t, 'bigint');
    console.log('     GetTickCount64 = ' + t);
    tick.close();

    const pid = dynalib('C:/Windows/System32/kernel32.dll', 0,
                        'GetCurrentProcessId', 'u32()');
    console.log('     GetCurrentProcessId = ' + pid.call());
    pid.close();
}

console.log('== ' + pass + ' passed, ' + fail + ' failed ==');
