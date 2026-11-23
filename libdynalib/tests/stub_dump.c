/*
 * stub_dump.c — print the exact machine code libdynalib emits for a call
 * stub, so a wrong byte can be read straight off the page instead of being
 * guessed.
 *
 *   stub_dump tests/dbtest.dll dbtest_add "ms_abi"
 */
#include "dynalib_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    DynalibModule *m;
    DynalibSymbol *s;
    DynalibAbiSpec abi;
    const char *err = NULL;
    void *stub;
    size_t i;

    if (argc < 3) {
        fprintf(stderr, "usage: stub_dump <dll> <symbol> [abi]\n");
        return 2;
    }
    m = dynalib_open(argv[1], DYNALIB_FMT_AUTO, &err);
    if (!m) { fprintf(stderr, "open: %s\n", err); return 1; }
    s = dynalib_resolve(m, 0, argv[2], NULL, &err);
    if (!s) { fprintf(stderr, "resolve: %s\n", err); return 1; }

    if (argc > 3) {
        if (dynalib_abi_parse(argv[3], &abi) < 0) {
            fprintf(stderr, "abi parse failed for '%s'\n", argv[3]);
            return 1;
        }
        printf("abi: kind=%d arg_count=%u ret_type=%u\n",
               (int)abi.kind, abi.arg_count, abi.ret_type);
    } else {
        memset(&abi, 0, sizeof abi);
        abi.kind = DYNALIB_ABI_MS64;
        abi.arg_count = 2;
    }

    stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
    if (!stub) { fprintf(stderr, "emit failed\n"); return 1; }

    printf("target  %p\nstub    %p\n", s->fn_ptr, stub);
    printf("bytes:\n");
    for (i = 0; i < 48; i++)
        printf("%02x ", ((unsigned char *)stub)[i]);
    printf("\n");

    if (abi.kind == DYNALIB_ABI_MS64 && abi.arg_count == 0) {
        /* the ms_abi spec carries no type contract, so the raw register
         * convention is used: two ints in rcx/rdx                           */
        int (*fn)(int, int) = (int (*)(int, int))stub;
        int r;
        fflush(stdout);
        printf("calling stub(3,4) ...\n");
        fflush(stdout);
        r = fn(3, 4);
        printf("stub(3,4) = %d\n", r);
    }

    dynalib_trampoline_free(stub, NULL);
    dynalib_symbol_free(s);
    dynalib_close(m);
    return 0;
}
