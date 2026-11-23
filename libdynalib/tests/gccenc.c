/* Let gcc encode the exact sequence we emit, then read the bytes back. */
#include <stdio.h>
#include <stdint.h>

__attribute__((naked, noclone)) static void g_sequence(void)
{
    __asm__ __volatile__(""
        "pushq %%rbp\n\t"
        "movq %%rsp, %%rbp\n\t"
        "subq $0x50, %%rsp\n\t"
        "movq 0x30(%%rbp), %%r11\n\t"
        "movq %%r11, 0x20(%%rsp)\n\t"
        "movq 0x40(%%rbp), %%r11\n\t"
        "movq %%r11, 0x28(%%rsp)\n\t"
        "leaq 0(%%rbp), %%rsp\n\t"
        "popq %%rbp\n\t"
        "ret\n"
    );
}

int main(void)
{
    unsigned char *p = (unsigned char *)g_sequence;
    FILE *f;
    int i;
    for (i = 0; i < 48; i++) {
        printf("%02x%s", p[i], (i % 16 == 15) ? "\n" : " ");
        if (p[i] == 0xc3 && p[i - 1] == 0x5d) break;
    }
    printf("\n");
    f = fopen("E:/sevm/libdynalib/tests/gcc_stub.bin", "wb");
    if (f) { fwrite(p, 1, 48, f); fclose(f); }
    return 0;
}
