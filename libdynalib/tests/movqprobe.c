
/* verify the correct MOVQ-from-memory encoding: F3 REX.W 0F 7E /r */
#include <stdio.h>
#include <string.h>
#include <windows.h>

int main(void)
{
    /* movq xmm0,[rcx+0] ; movq xmm1,[rcx+8] ; addsd xmm0,xmm1 ; ret */
    unsigned char ok[] = { 0xF3,0x48,0x0F,0x7E,0x01,
                           0xF3,0x48,0x0F,0x7E,0x49,0x08,
                           0xF2,0x0F,0x58,0xC1, 0xC3 };
    /* same but base r10 (REX.B) with disp32 */
    unsigned char r10[] = { 0x49,0x89,0xCA,
                            0xF3,0x49,0x0F,0x7E,0x82,0x00,0x00,0x00,0x00,
                            0xF3,0x49,0x0F,0x7E,0x8A,0x08,0x00,0x00,0x00,
                            0xF2,0x0F,0x58,0xC1, 0xC3 };
    /* cvtss2sd xmm0,xmm0 (F3 0F 5A C0) */
    unsigned char cvt[] = { 0xF3,0x0F,0x5A,0xC0, 0xC3 };

    const unsigned char *seqs[3] = { ok, r10, cvt };
    const char *names[3] = { "movq xmm,[rcx] + addsd",
                             "movq xmm,[r10+disp32]",
                             "cvtss2sd xmm0,xmm0" };
    size_t lens[3] = { sizeof ok, sizeof r10, sizeof cvt };
    unsigned char abuf[16];
    double d0 = 1.5, d1 = 2.25;
    memcpy(abuf + 0, &d0, 8);
    memcpy(abuf + 8, &d1, 8);

    for (int i = 0; i < 3; i++) {
        unsigned char *p = (unsigned char *)VirtualAlloc(NULL, 0x1000,
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD old = 0;
        double (*f)(const unsigned char *);
        double r;
        if (!p) { puts("no page"); return 1; }
        memcpy(p, seqs[i], lens[i]);
        VirtualProtect(p, 0x1000, PAGE_EXECUTE_READ, &old);
        f = (double (*)(const unsigned char *))p;
        r = f(abuf);
        printf("%-28s -> %f\n", names[i], r);
        fflush(stdout);
        VirtualFree(p, 0, MEM_RELEASE);
    }
    return 0;
}
