/* Dump the emitted stub bytes so they can be disassembled. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "dynalib.h"

int main(void)
{
    DynalibModule *m;
    DynalibSymbol *s;
    DynalibAbiSpec abi;
    void *stub;
    FILE *f;

    m = dynalib_open("E:/sevm/libdynalib/tests/dbtest.dll",
                     DYNALIB_FMT_AUTO, NULL);
    if (!m) { printf("open failed\n"); return 1; }
    s = dynalib_resolve(m, 0, "dbtest_sum5", NULL, NULL);
    if (!s) { printf("resolve failed\n"); return 1; }

    dynalib_abi_parse("i32(i32, i32, i32, i32, i32)", &abi);
    stub = dynalib_trampoline_emit(s->fn_ptr, &abi, NULL);
    printf("stub=%p target=%p\n", stub, s->fn_ptr);

    f = fopen("E:/sevm/libdynalib/tests/stub5.bin", "wb");
    if (f) {
        fwrite(stub, 1, 96, f);
        fclose(f);
        printf("wrote 96 bytes\n");
    }
    return 0;
}
