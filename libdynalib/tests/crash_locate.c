/*
 * crash_locate.c — find where the trampoline actually faults.
 *
 * A vectored exception handler prints the faulting instruction address and a
 * byte window around it, so instead of guessing which byte is wrong the answer
 * comes straight from the machine.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "dynalib_internal.h"

static LONG NTAPI on_exc(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    BYTE *rip = (BYTE *)(uintptr_t)c->Rip;
    BYTE *base;
    long i;

    printf("FAULT code=0x%08lx  rip=%p  rsp=%p  rbp=%p\n",
           (unsigned long)ep->ExceptionRecord->ExceptionCode, rip,
           (void *)c->Rsp, (void *)c->Rbp);
    base = (BYTE *)(uintptr_t)((uintptr_t)rip & ~(uintptr_t)0xFF);
    for (i = 0; i < 64; i++) {
        if (base + i == rip) printf("  -> ");
        else                   printf("     ");
        printf("%02x", base[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");
    fflush(stdout);
    ExitProcess(1);
}

int main(int argc, char **argv)
{
    DynalibModule *m;
    DynalibSymbol *s;
    DynalibAbiSpec abi;
    const char *err = NULL;
    void *stub;
    size_t i;

    setvbuf(stdout, NULL, _IONBF, 0);
    AddVectoredExceptionHandler(1, on_exc);

    if (argc < 3) {
        fprintf(stderr, "usage: crash_locate <dll> <symbol> [abi]\n");
        return 2;
    }
    m = dynalib_open(argv[1], DYNALIB_FMT_AUTO, &err);
    if (!m) { fprintf(stderr, "open: %s\n", err); return 1; }
    s = dynalib_resolve(m, 0, argv[2], NULL, &err);
    if (!s) { fprintf(stderr, "resolve: %s\n", err); return 1; }

    memset(&abi, 0, sizeof abi);
    if (argc > 3) {
        if (dynalib_abi_parse(argv[3], &abi) < 0) {
            fprintf(stderr, "abi parse failed\n");
            return 1;
        }
    } else {
        abi.kind = DYNALIB_ABI_MS64;
        abi.arg_count = 2;
    }

    stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
    if (!stub) { fprintf(stderr, "emit failed\n"); return 1; }

    printf("target %p  stub %p\n", s->fn_ptr, stub);
    printf("bytes:\n");
    for (i = 0; i < 40; i++) printf("%02x ", ((unsigned char *)stub)[i]);
    printf("\n");

    printf("calling ...\n");
    fflush(stdout);
    ((int (*)(int, int))stub)(3, 4);
    printf("returned normally\n");
    return 0;
}
