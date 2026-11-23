/*
 * dbtest.c — a throwaway DLL for the dynalib test harness: a real load
 * target with arguments, a return value and a native caller that invokes a
 * callback, so the trampoline and callback paths can be exercised end to end
 * instead of only checked byte-by-byte.
 */
#include <stdint.h>

typedef void (*cb0_t)(void);
typedef void (*cb2_t)(int, int);

#if defined(_WIN32)
#define EXPL __declspec(dllexport)
#else
#define EXPL __attribute__((visibility("default")))
#endif

EXPL int dbtest_add(int a, int b) { return a + b; }
EXPL int dbtest_mul(int a, int b) { return a * b; }
EXPL const char *dbtest_greet(void) { return "greetings from dbtest"; }
EXPL int dbtest_square(int a) { return a * a; }

/* --- stack-argument targets -------------------------------------- */
/* 5+ arguments cross into the stack area under MS64 (4 register slots)
 * and under SysV (6 register slots), so these exercise the copy path.   */
EXPL int dbtest_sum5(int a, int b, int c, int d, int e)
{
    return a + b + c + d + e;
}

EXPL int dbtest_sum6(int a, int b, int c, int d, int e, int f)
{
    return a + b + c + d + e + f;
}

EXPL int dbtest_sum8(int a, int b, int c, int d, int e, int f, int g, int h)
{
    return a + b + c + d + e + f + g + h;
}

EXPL int dbtest_sum12(int a, int b, int c, int d, int e, int f,
                      int g, int h, int i, int j, int k, int l)
{
    return a + b + c + d + e + f + g + h + i + j + k + l;
}

/* mixed: register args first, then stack args, with a marker so a
 * mis-forwarded slot shows up as a wrong constant, not a wrong sum     */
EXPL int dbtest_mix7(int a, int b, int c, int d, int e, int f, int g)
{
    return a * 1000000 + b * 100000 + c * 10000 + d * 1000 +
           e * 100 + f * 10 + g;
}

/* --- 64-bit returns ----------------------------------------------- */
EXPL uint64_t dbtest_retu64(uint64_t v) { return v + 0x100000000ULL; }
EXPL int64_t  dbtest_reti64(int64_t v)  { return -v; }
EXPL uint64_t dbtest_sumu64(uint64_t a, uint64_t b, uint64_t c,
                            uint64_t d, uint64_t e)
{
    return a + b + c + d + e;
}

/* --- floating-point targets ---------------------------------------- */
/* The float path is class-based: doubles travel in xmm registers (and on
 * the stack once the xmm budget runs out), integers in the GP registers,
 * and the two budgets are counted independently. These targets make a
 * mis-classified slot visible as a wrong number.                     */

EXPL double dbtest_fadd(double a, double b) { return a + b; }

EXPL double dbtest_fmul3(double a, double b, double c)
{
    return a * b * c;
}

/* mixed classes: int, double, int, double, double */
EXPL double dbtest_fmix5(int a, double b, int c, double d, double e)
{
    return a * 1000.0 + b * 100.0 + c * 10.0 + d + e;
}

/* 9 doubles: the first eight ride xmm0..xmm7, the ninth spills to the
 * stack area — this is what catches a broken SysV xmm overflow path.   */
EXPL double dbtest_fsum9(double a, double b, double c, double d, double e,
                          double f, double g, double h, double i)
{
    return a + b + c + d + e + f + g + h + i;
}

/* a single-precision return: the stub has to widen it for a caller that
 * reads xmm0 as a double                                                */
EXPL float dbtest_fhalf(float v) { return v / 2.0f; }

/* double return with an integer argument in the middle */
EXPL double dbtest_fscale(double v, int n) { return v * n; }

EXPL void dbtest_invoke_cb0(cb0_t cb) { if (cb) cb(); }

EXPL void dbtest_invoke_cb2(int a, int b, cb2_t cb)
{
    if (cb) cb(a, b);
}

/* SysV-flavoured twin so the SysV emitter has a target that is genuinely
 * compiled with the other convention, when the toolchain exposes it.       */
#ifdef _WIN32
EXPL __attribute__((sysv_abi)) int dbtest_add_sysv(int a, int b)
{
    return a * b - 1;
}
#endif
