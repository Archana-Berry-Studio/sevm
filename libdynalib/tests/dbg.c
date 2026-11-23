/*
 * dbg.c — print the intermediate values dynalib derives from a container,
 * so a parser failure can be diagnosed without guessing.
 *
 *   dbg pe <file>     walk the section table and report the export RVA
 *   dbg elf <file>    walk PT_DYNAMIC and report the DT_ tags found
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static unsigned short rd16(const unsigned char *p)
{ return (unsigned short)(p[0] | (p[1] << 8)); }
static unsigned rd32(const unsigned char *p)
{ return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
        ((unsigned)p[3] << 24); }
static unsigned long long rd64(const unsigned char *p)
{ return (unsigned long long)rd32(p) | ((unsigned long long)rd32(p + 4) << 32); }

static void pe(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    unsigned char *p;
    unsigned e_lfanew, sh_off, nsec, sname, k;
    unsigned ed_rva;
    if (!fp) { fprintf(stderr, "cannot open %s\n", path); return; }
    fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    p = malloc(sz); fread(p, 1, (size_t)sz, fp); fclose(fp);
    e_lfanew = rd32(p + 0x3c);
    printf("e_lfanew=%u  Machine=%04x  NumberOfSections=%u  SizeOfOptionalHeader=%u\n",
           e_lfanew, rd16(p + e_lfanew + 4), rd16(p + e_lfanew + 6),
           rd16(p + e_lfanew + 20));
    nsec = rd16(p + e_lfanew + 6);
    sname = rd16(p + e_lfanew + 20);
    sh_off = e_lfanew + 24 + sname;
    printf("section table at file offset %u (%.16s)\n", sh_off, p + sh_off);
    for (k = 0; k < nsec; k++) {
        unsigned char *s = p + sh_off + k * 40;
        printf("  [%u] %-8s VA=%08x VSize=%08x Raw=%08x RawSize=%08x\n",
               k, (char *)s, rd32(s + 12), rd32(s + 8), rd32(s + 20), rd32(s + 16));
    }
    {
        unsigned opt_off = e_lfanew + 24;
        int is64 = rd16(p + opt_off) == 0x020b;
        unsigned dd = opt_off + (is64 ? 112 : 96);
        ed_rva = rd32(p + dd);
        printf("OptionalHeader magic=%04x  data dirs at %u  ExportRVA=%08x\n",
               rd16(p + opt_off), dd, ed_rva);
        printf("first 48 bytes of the export dir at that rva:\n  ");
        {
            unsigned i;
            int inside = 0;
            for (k = 0; k < nsec; k++) {
                unsigned char *s = p + sh_off + k * 40;
                if (ed_rva >= rd32(s + 12) && ed_rva < rd32(s + 12) + rd32(s + 8))
                    inside = 1;
            }
            printf("inside a section: %s\n", inside ? "yes" : "NO");
            if (inside)
                for (i = 0; i < 48; i++)
                    printf("%02x ", p[ed_rva + i]);
            printf("\n");
        }
    }
    free(p);
}

static void elf(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    unsigned char *p;
    unsigned long long phoff, phnum, pesh, n, off, fsz;
    unsigned i;
    if (!fp) { fprintf(stderr, "cannot open %s\n", path); return; }
    fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    p = malloc(sz); fread(p, 1, (size_t)sz, fp); fclose(fp);
    phoff = rd64(p + 32); phnum = rd16(p + 56); pesh = rd16(p + 54);
    printf("e_phoff=%llu e_phnum=%llu e_phentsize=%llu\n", phoff, phnum, pesh);
    for (n = 0; n < phnum; n++) {
        unsigned char *h = p + phoff + n * pesh;
        printf("  [%llu] type=%llu flags=%llu off=%llu vaddr=%llx paddr=%llx "
               "filesz=%llu memsz=%llu align=%llx\n", n, rd64(h), rd64(h + 8),
               rd64(h + 16), rd64(h + 24), rd64(h + 32), rd64(h + 40),
               rd64(h + 48), rd64(h + 56));
        if (rd64(h) == 2) {
            off = rd64(h + 16); fsz = rd64(h + 40);
            printf("      DT entries at file offset %llu, %llu bytes:\n", off, fsz);
            for (i = 0; i + 16 <= fsz; i += 16) {
                unsigned long long t = rd64(p + off + i);
                unsigned long long v = rd64(p + off + i + 8);
                printf("      tag=%016llx val=%llx\n", t, v);
                if (t == 0) break;
            }
        }
    }
    free(p);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: dbg pe|elf <file>\n");
        return 2;
    }
    if (!strcmp(argv[1], "pe")) pe(argv[2]);
    else if (!strcmp(argv[1], "elf")) elf(argv[2]);
    else { fprintf(stderr, "unknown mode %s\n", argv[1]); return 2; }
    return 0;
}
