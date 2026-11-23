/* Standalone repro: call the 5-arg trampoline and report the exact fault. */
#include <stdio.h>
#include <stdint.h>
#include <windows.h>

#include "dynalib.h"

static const char *DBTEST = "E:/sevm/libdynalib/tests/dbtest.dll";

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    PEXCEPTION_RECORD r = ep->ExceptionRecord;
    PCONTEXT cx = ep->ContextRecord;
    fprintf(stderr, "VEH code=0x%lx faultaddr=%p rip=%p rsp=%p rbp=%p\n",
            (unsigned long)r->ExceptionCode,
            (void *)(r->NumberParameters > 1
                     ? (void *)r->ExceptionInformation[1] : NULL),
            (void *)r->ExceptionAddress,
            (void *)cx->Rsp,
            (void *)cx->Rbp);
    fprintf(stderr, "  rcx=%llx rdx=%llx r8=%llx r9=%llx r11=%llx\n",
            (unsigned long long)cx->Rcx,
            (unsigned long long)cx->Rdx,
            (unsigned long long)cx->R8,
            (unsigned long long)cx->R9,
            (unsigned long long)cx->R11);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(void)
{
    DynalibModule *m;
    DynalibSymbol *s;
    DynalibAbiSpec abi;
    void *stub;
    int r;

    AddVectoredExceptionHandler(1, veh);

    m = dynalib_open(DBTEST, DYNALIB_FMT_AUTO, NULL);
    if (!m) { printf("open failed\n"); return 1; }
    s = dynalib_resolve(m, 0, "dbtest_sum5", NULL, NULL);
    if (!s) { printf("resolve failed\n"); return 1; }

    dynalib_abi_parse("i32(i32, i32, i32, i32, i32)", &abi);
    printf("arg_count=%u kind=%d\n", (unsigned)abi.arg_count, (int)abi.kind);

    stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
    printf("stub=%p target=%p\n", stub, s->fn_ptr);
    fflush(stdout);

    r = ((int (*)(int, int, int, int, int))stub)(1, 2, 3, 4, 5);
    printf("result=%d (want 15)\n", r);
    return 0;
}
