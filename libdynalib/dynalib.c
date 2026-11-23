/*
 * dynalib.c — libdynalib core: container parsers, cross-OS loading, symbol
 * resolution, calling-convention grammar, GC pinning and reverse callbacks.
 *
 * The parsers are libc-free and pure, so they can be exercised against an
 * image that simply lives in a byte buffer: that is how the ELF path is
 * tested on Windows and the PE path on Linux, instead of trusting a parser
 * that only ever runs on its native host.
 */
#include "dynalib.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>

#include "dynalib_internal.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#  if defined(__linux__) || defined(__ANDROID__)
#    include <link.h>
#  endif
#endif

/* ================================================================== */
/* little helpers — libc-free, so the parsers stay freestanding-safe    */
/* ================================================================== */

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

#define DYNALIB_IN(img, len, off, n) \
    ((off) >= 0 && (uint64_t)(off) + (uint64_t)(n) <= (uint64_t)(len))

/* ================================================================== */
/* container sniffing                                                  */
/* ================================================================== */

/* .lbrct (UBerryNix CherryReVA) magic. The container belongs to UBerryNix;
 * only the magic is fixed here and the actual loader is injectable, so the
 * real format ships with the platform, not with this file.               */
#define LBRCT_MAGIC 0x4c425254u     /* "TRBL" little-endian */

DynalibFormat dynalib_detect_format(const void *image, size_t len)
{
    const unsigned char *p = (const unsigned char *)image;
    if (len >= 2 && p[0] == 'M' && p[1] == 'Z')
        return DYNALIB_FMT_PE_DLL;
    if (len >= 4 && p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F')
        return DYNALIB_FMT_ELF_SO;
    if (len >= 4 && rd32(p) == LBRCT_MAGIC)
        return DYNALIB_FMT_LBRCT;
    return DYNALIB_FMT_AUTO;
}

const char *dynalib_format_name(DynalibFormat f)
{
    switch (f) {
    case DYNALIB_FMT_ELF_SO:   return "elf";
    case DYNALIB_FMT_PE_DLL:   return "pe";
    case DYNALIB_FMT_LBRCT:    return "lbrct";
    case DYNALIB_FMT_SYSCALL:  return "syscall";
    default:                   return "auto";
    }
}

/* ================================================================== */
/* PE export directory                                                 */
/* ================================================================== */

/* IMAGE_EXPORT_DIRECTORY field offsets (both PE32 and PE32+) */
#define PE_EXP_NAME  0x0C
#define PE_EXP_BASE  0x10
#define PE_EXP_NFUNC 0x14
#define PE_EXP_NNAME 0x18
#define PE_EXP_FN    0x1C
#define PE_EXP_NM    0x20
#define PE_EXP_ORD   0x24

/* Optional-header offsets shared by PE32 and PE32+ */
#define PE_OPT_SIZEOFIMAGE 56
#define PE_OPT_DIRECTRIES_IS64 112
#define PE_OPT_DIRECTRIES_IS32 96

/* Translate an RVA into a file offset by walking the section headers. */
static int pe_rva_to_off(const unsigned char *p, size_t len, uint32_t rva,
                        uint32_t *off, bool *ok)
{
    uint32_t e_lfanew = rd32(p + 0x3c);
    const unsigned char *eh = p + e_lfanew;
    uint16_t nsec, sname, k, sh_off;

    *ok = false;
    *off = 0;
    if (!DYNALIB_IN(p, len, e_lfanew, 24) || rd32(eh) != 0x00004550)
        return -1;
    nsec = rd16(eh + 6);            /* IMAGE_FILE_HEADER starts after the four-byte PE signature */
    sname = rd16(eh + 20);          /* SizeOfOptionalHeader */
    sh_off = (uint16_t)(e_lfanew + 24 + sname);   /* after the PE signature */
    for (k = 0; k < nsec; k++) {
        const unsigned char *sh = p + sh_off + k * 40;
        uint32_t va, vs, raw;
        if (!DYNALIB_IN(p, len, sh - p, 40))
            break;
        va  = rd32(sh + 12);
        vs  = rd32(sh + 8);
        raw = rd32(sh + 20);
        if (rva >= va && rva < va + vs && raw) {
            *off = raw + (rva - va);
            *ok  = DYNALIB_IN(p, len, (size_t)*off, 4);
            return 0;
        }
    }
    return 0;
}

/*
 * Every export a PE image advertises: named entries, then the ordinal-only
 * slots that the host linker would never hand out. Also catches forwarded
 * names, whose "address" is really another RVA into the export name table.
 */
static int pe_resolve(const unsigned char *p, size_t len, uint32_t rva,
                      bool live, uint32_t *poff, bool *ok)
{
    if (live) {
        /* a loaded image: the file is the image, so the virtual address is
         * simply base + rva and no section walk is involved                 */
        *poff = rva;
        *ok = DYNALIB_IN(p, len, rva, 1);
        return 0;
    }
    return pe_rva_to_off(p, len, rva, poff, ok);
}

static ssize_t pe_parse_exports(const unsigned char *p, size_t len,
                               DynalibExportEntry *out, size_t cap,
                               uintptr_t *pbase_rva, const char **perr,
                               bool live)
{
    uint32_t e_lfanew, opt_off, ed_off, fn_off, nm_off, ord_off;
    const unsigned char *opt, *ed, *fn, *nm;
    uint32_t n_func, n_name, ord_base, i;
    bool is64, ok = false;
    ssize_t total = 0;

    if (!DYNALIB_IN(p, len, 0, 0x40)) { *perr = "pe: image too short"; return -1; }
    e_lfanew = rd32(p + 0x3c);
    if (!DYNALIB_IN(p, len, e_lfanew, 24) || rd32(p + e_lfanew) != 0x00004550) {
        *perr = "pe: bad PE signature";
        return -1;
    }
    opt_off = e_lfanew + 24;
    if (!DYNALIB_IN(p, len, opt_off, 24)) { *perr = "pe: truncated optional header"; return -1; }
    opt = p + opt_off;
    is64 = (rd16(opt) == 0x020b);
    if (!is64 && rd16(opt) != 0x010b) {
        *perr = "pe: unknown optional header magic";
        return -1;
    }

    {
        size_t dd = opt_off + (is64 ? PE_OPT_DIRECTRIES_IS64 : PE_OPT_DIRECTRIES_IS32);
        if (!DYNALIB_IN(p, len, dd, 8)) { *perr = "pe: truncated data directory"; return -1; }
        ed_off = rd32(p + dd);
        if (!ed_off) { *perr = "pe: image has no export table"; return -1; }
    }
    if (pe_resolve(p, len, ed_off, live, &ed_off, &ok) < 0 || !ok) {
        *perr = "pe: export directory not in a mapped section";
        return -1;
    }
    ed = p + ed_off;
    if (!DYNALIB_IN(p, len, ed_off, 0x28)) { *perr = "pe: truncated export directory"; return -1; }

    ord_base = rd32(ed + PE_EXP_BASE);
    n_func   = rd32(ed + PE_EXP_NFUNC);
    n_name   = rd32(ed + PE_EXP_NNAME);
    fn_off   = rd32(ed + PE_EXP_FN);
    nm_off   = rd32(ed + PE_EXP_NM);
    ord_off  = rd32(ed + PE_EXP_ORD);

    if (!fn_off || !nm_off || n_func == 0) { *perr = "pe: empty export tables"; return -1; }
    if (pe_resolve(p, len, fn_off, live, &fn_off, &ok) < 0 || !ok) {
        *perr = "pe: address-of-functions not in a mapped section";
        return -1;
    }
    if (pe_resolve(p, len, nm_off, live, &nm_off, &ok) < 0 || !ok) {
        *perr = "pe: address-of-names not in a mapped section";
        return -1;
    }
    if (ord_off && (pe_resolve(p, len, ord_off, live, &ord_off, &ok) < 0 || !ok))
        ord_off = 0;

    fn = p + fn_off;
    nm = p + nm_off;
    if (!DYNALIB_IN(p, len, fn_off, (size_t)n_func * 4) ||
        !DYNALIB_IN(p, len, nm_off, (size_t)n_name * 4)) {
        *perr = "pe: export tables beyond image";
        return -1;
    }

    for (i = 0; i < n_name; i++) {
        uint32_t nr = rd32(nm + i * 4);
        uint32_t fnrva = rd32(fn + i * 4);
        uint16_t ordid;
        uint32_t nr_off;
        bool nr_ok = false;
        const char *s;
        uint32_t j;
        if (pbase_rva && total == 0)
            *pbase_rva = (uintptr_t)fnrva;
        /* the name pointers inside the name table are RVAs themselves */
        if (nr == 0) {
            *perr = "pe: null name pointer";
            return -1;
        }
        if (pe_resolve(p, len, nr, live, &nr_off, &nr_ok) < 0 || !nr_ok) {
            *perr = "pe: name pointer not in a mapped section";
            return -1;
        }
        /* an ordinal entry in the address-of-name-ordinals table is a
         * relative index into address-of-functions                         */
        if (ord_off)
            ordid = (uint16_t)(ord_base + rd16((p + ord_off) + i * 2));
        else
            ordid = (uint16_t)(ord_base + i);
        if (out && (size_t)total < cap) {
            s = (const char *)(p + nr_off);
            for (j = 0; j < DYNALIB_EXPORT_NAME_MAX - 1; j++) {
                out[total].name[j] = s[j];
                if (!s[j]) break;
            }
            out[total].name[j] = 0;
            out[total].rva = fnrva;
            out[total].ordinal = ordid;
            out[total].is_ordinal_only = false;
        }
        total++;
    }

    for (i = n_name; i < n_func; i++) {
        uint32_t fnrva = rd32(fn + i * 4);
        if (pbase_rva && total == 0)
            *pbase_rva = (uintptr_t)fnrva;
        if (out && (size_t)total < cap) {
            out[total].name[0] = 0;
            out[total].rva = fnrva;
            out[total].ordinal = (uint16_t)(ord_base + i);
            out[total].is_ordinal_only = true;
        }
        total++;
    }
    return total;
}

/* ================================================================== */
/* ELF dynamic symbol table                                            */
/* ================================================================== */

/*
 * Map an RVA into a file offset through the PT_LOAD segments the loader saw.
 * `entsz` is the symbol entry size the caller expects to read, so the
 * "does this fit on disk" test is exact rather than approximate.
 */
static int elf_rva_to_off(const unsigned char *p, size_t len, uint64_t rva,
                          size_t entsz, uint64_t *off, bool *ok)
{
    uint64_t phoff = rd64(p + 32), phnum = rd16(p + 56), pesh = rd16(p + 54);
    uint64_t n;

    *ok = false;
    *off = 0;
    if (phnum == 0 || pesh < 56) {
        /* no program headers: the rva is effectively a file offset, which
         * is true for fixed-link ET_EXEC images                         */
        *off = rva;
        *ok  = (rva + (uint64_t)entsz <= (uint64_t)len);
        return 0;
    }
    for (n = 0; n < phnum; n++) {
        const unsigned char *h = p + phoff + n * pesh;
        uint64_t o, va, fs, ms;
        if (!DYNALIB_IN(p, len, (size_t)(h - p), 56))
            break;
        o  = rd64(h + 16);
        va = rd64(h + 24);
        fs = rd64(h + 40);
        ms = rd64(h + 48);
        if (rva >= va && rva < va + ms) {
            uint64_t d = o + (rva - va);
            *off = d;
            *ok  = (d + (uint64_t)entsz <= o + fs);
            return 0;
        }
    }
    return 0;
}

/*
 * Stream .dynsym into `out`. Elf64_Sym: st_name@0(4) st_info@4 st_other@5
 * st_shndx@6 st_value@8(8) st_size@16(8); Elf32_Sym: st_name@0(4)
 * st_value@4(4) st_size@8(4).
 */
static ssize_t elf_stream_symtab(const unsigned char *p, size_t len,
                                uint64_t sym_off, uint64_t str_off,
                                bool is64, DynalibExportEntry *out,
                                size_t cap, uintptr_t *pbase_rva)
{
    size_t entsz = is64 ? 24 : 16;
    size_t i = 0, total = 0;

    for (;;) {
        size_t at = sym_off + i * entsz;
        uint32_t value, name;
        if (!DYNALIB_IN(p, len, at, entsz))
            break;
        if (is64) {
            name  = rd32(p + at);
            value = (uint32_t)rd64(p + at + 8);
        } else {
            name  = rd32(p + at);
            value = rd32(p + at + 4);
        }
        if (value == 0 || name == 0) {      /* null symbol or undefined */
            i++;
            continue;
        }
        if (pbase_rva && total == 0)
            *pbase_rva = (uintptr_t)value;
        if (out && total < cap) {
            const char *s;
            size_t j;
            out[total].name[0] = 0;
            out[total].rva = (uint32_t)value;
            out[total].ordinal = 0;
            out[total].is_ordinal_only = false;
            if (DYNALIB_IN(p, len, str_off + name, 1)) {
                s = (const char *)(p + str_off + name);
                for (j = 0; j < DYNALIB_EXPORT_NAME_MAX - 1; j++) {
                    out[total].name[j] = s[j];
                    if (!s[j]) break;
                }
                out[total].name[j] = 0;
            }
        }
        total++;
        i++;
    }
    return (ssize_t)total;
}

/*
 * Stream .dynsym for a whole-image buffer. DT_SYMTAB / DT_STRTAB are found by
 * walking PT_DYNAMIC, which is what lets us enumerate the symbols of an image
 * whose .symtab has been stripped: the dynamic table and its strtab are part
 * of the loader contract, not a debug facility.
 */
static ssize_t elf_parse_exports(const unsigned char *p, size_t len,
                                 DynalibExportEntry *out, size_t cap,
                                 uintptr_t *pbase_rva, const char **perr)
{
    uint64_t phoff = rd64(p + 32), phnum = rd16(p + 56), pesh = rd16(p + 54);
    uint64_t n, dyn_off = 0, dynsz = 0;
    uint64_t symtab = 0, strtab = 0, sym_off, str_off;
    bool is64, ok;
    ssize_t total;

    if (!DYNALIB_IN(p, len, 0, 64)) { *perr = "elf: image too short"; return -1; }
    is64 = (p[4] == 2);                          /* EI_CLASS == ELFCLASS64 */

    for (n = 0; n < phnum; n++) {
        const unsigned char *h = p + phoff + n * pesh;
        if (!DYNALIB_IN(p, len, (size_t)(h - p), 56))
            break;
        if (rd64(h) == 2 /* PT_DYNAMIC */) {
            dyn_off = rd64(h + 16);
            dynsz = rd64(h + 40);
            break;
        }
    }
    if (!dynsz) { *perr = "elf: no PT_DYNAMIC (statically linked?)"; return -1; }

    for (n = 0; n + 16 <= dynsz; n += 16) {
        uint64_t tag, v;
        if (!DYNALIB_IN(p, len, dyn_off + n, 16))
            break;
        tag = rd64(p + dyn_off + n);
        v   = rd64(p + dyn_off + n + 8);
        if (tag == 0) break;                                  /* DT_NULL */
        if (tag == 0x6ffffff9) symtab = v;                    /* DT_SYMTAB */
        if (tag == 5)          strtab = v;                    /* DT_STRTAB */
    }
    if (!symtab || !strtab) { *perr = "elf: no DT_SYMTAB/DT_STRTAB"; return -1; }

    if (elf_rva_to_off(p, len, symtab, is64 ? 24 : 16, &sym_off, &ok) < 0 || !ok) {
        *perr = "elf: DT_SYMTAB not in a mapped segment";
        return -1;
    }
    if (elf_rva_to_off(p, len, strtab, 1, &str_off, &ok) < 0 || !ok) {
        *perr = "elf: DT_STRTAB not in a mapped segment";
        return -1;
    }
    return elf_stream_symtab(p, len, sym_off, str_off, is64, out, cap, pbase_rva);
}

/* ================================================================== */
/* public pure-parser entry point                                      */
/* ================================================================== */

ssize_t dynalib_parse_image(const void *image, size_t len,
                            DynalibExportEntry *out, size_t cap,
                            uintptr_t *pbase_rva, const char **perr)
{
    const unsigned char *p = (const unsigned char *)image;
    DynalibFormat f = dynalib_detect_format(image, len);
    if (perr) *perr = NULL;
    if (f == DYNALIB_FMT_PE_DLL)
        return pe_parse_exports(p, len, out, cap, pbase_rva, perr, false);
    if (f == DYNALIB_FMT_ELF_SO)
        return elf_parse_exports(p, len, out, cap, pbase_rva, perr);
    if (f == DYNALIB_FMT_LBRCT) {
        if (perr) *perr = "lbrct: container parser ships with UBerryNix";
        return -1;
    }
    if (perr) *perr = "unrecognised container magic";
    return -1;
}

/* ================================================================== */
/* platform loading + resolution                                       */
/* ================================================================== */

#if !defined(_WIN32)
/*
 * Base-address fallback that works where musl/bionic lack RTLD_DI_LINKMAP:
 * find the mapping that actually contains the handle the loader returned.
 */
static uintptr_t elf_find_map_base(void *handle)
{
    FILE *fp;
    char line[512];
    uintptr_t base = 0;
    fp = fopen("/proc/self/maps", "r");
    if (!fp)
        return (uintptr_t)handle;
    while (fgets(line, sizeof line, fp)) {
        unsigned long long lo, hi;
        if (sscanf(line, "%llx-%llx", &lo, &hi) == 2)
            if ((uintptr_t)handle >= lo && (uintptr_t)handle < hi) {
                base = (uintptr_t)lo;
                break;
            }
    }
    fclose(fp);
    return base ? base : (uintptr_t)handle;
}
#endif

#if defined(_WIN32)
static int file_readable(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}
#else
static int file_readable(const char *path)
{
    return access(path, R_OK) == 0;
}
#endif

DynalibModule *dynalib_open(const char *path, DynalibFormat fmt_hint,
                            const char **perr)
{
    DynalibModule *m;
    char probe[1024];
    const char *load = path;
    (void)fmt_hint;
    if (perr) *perr = NULL;
    if (!path) { if (perr) *perr = "null path"; return NULL; }

    /* short names are searched in ./mochimodule/ before the default path */
    if (strchr(path, '/') == NULL && strchr(path, '\\') == NULL) {
        const char *ex;
        size_t n;
        if ((ex = strrchr(path, '.')) != NULL) {
            n = (size_t)(ex - path);
            if (snprintf(probe, sizeof probe, "./mochimodule/%.*s", (int)n, path)
                < (int)sizeof probe) {
                memcpy(probe + n + 16, ex, strlen(ex) + 1);
                if (file_readable(probe))
                    load = probe;
            }
        }
    }

    m = (DynalibModule *)calloc(1, sizeof *m);
    if (!m) { if (perr) *perr = "oom"; return NULL; }
    snprintf(m->path, sizeof m->path, "%s", load);
    m->ref_count = 1;

#if defined(_WIN32)
    {
        HMODULE h = LoadLibraryA(load);
        if (!h) {
            char buf[256];
            snprintf(buf, sizeof buf, "LoadLibrary failed (err=%lu): %s",
                     (unsigned long)GetLastError(), load);
            if (perr) *perr = buf;
            free(m);
            return NULL;
        }
        m->os_handle = (void *)h;
        m->base_addr = (uintptr_t)h;
        m->format = DYNALIB_FMT_PE_DLL;
        {
            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)h;
            if (dos->e_magic == 0x5a4d) {
                IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)
                    ((unsigned char *)h + dos->e_lfanew);
                if (nt->Signature == 0x00004550)
                    m->base_size = nt->OptionalHeader.SizeOfImage;
            }
        }
    }
#else
    {
        void *h = dlopen(load, RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            if (perr) *perr = dlerror();
            free(m);
            return NULL;
        }
        m->os_handle = h;
        m->format = DYNALIB_FMT_ELF_SO;
#if defined(__linux__) || defined(__ANDROID__)
        {
            Dl_info info;
            if (dlinfo(h, RTLD_DI_LINKMAP, &info) == 0) {
                struct link_map *lm = (struct link_map *)&info;
                m->base_addr = (uintptr_t)lm->l_addr;
            } else {
                m->base_addr = elf_find_map_base(h);
            }
        }
#else
        m->base_addr = (uintptr_t)h;
#endif
    }
#endif
    return m;
}

DynalibSymbol *dynalib_resolve(DynalibModule *mod, uintptr_t offset,
                               const char *symbol, const DynalibAbiSpec *abi,
                               const char **perr)
{
    DynalibSymbol *s;
    void *fn = NULL;
    bool absolute;

    if (perr) *perr = NULL;
    if (!mod) { if (perr) *perr = "null module"; return NULL; }

    if (symbol) {
#if defined(_WIN32)
        fn = (void *)GetProcAddress((HMODULE)mod->os_handle, symbol);
#else
        fn = dlsym(mod->os_handle, symbol);
        if (!fn && perr) *perr = dlerror();
#endif
    }
    if (!fn && offset && mod->base_addr) {
        absolute = (offset >= mod->base_addr) &&
                   (mod->base_size == 0 ||
                    offset < mod->base_addr + mod->base_size);
        fn = absolute ? (void *)offset : (void *)(mod->base_addr + offset);
    }
    if (!fn) {
        if (perr) *perr = "symbol and offset both unresolved";
        return NULL;
    }

    s = (DynalibSymbol *)calloc(1, sizeof *s);
    if (!s) { if (perr) *perr = "oom"; return NULL; }
    s->module = mod;
    s->fn_ptr = fn;
    s->rva_offset = (mod->base_addr && (uintptr_t)fn >= mod->base_addr)
                    ? (uintptr_t)((char *)fn - (char *)mod->base_addr) : 0;
    if (abi)
        s->abi = *abi;
    else
        s->abi.kind = DYNALIB_ABI_DEFAULT;
    return s;
}

static ssize_t parse_live_image(const void *image, size_t len,
                                DynalibExportEntry *out, size_t cap,
                                uintptr_t *pbase_rva, const char **perr)
{
    const unsigned char *p = (const unsigned char *)image;
    DynalibFormat f = dynalib_detect_format(image, len);

    if (perr) *perr = NULL;
    if (f == DYNALIB_FMT_PE_DLL)
        return pe_parse_exports(p, len, out, cap, pbase_rva, perr, true);
    if (f == DYNALIB_FMT_ELF_SO) {
        if (perr)
            *perr = "elf: live mapping needs PT_DYNAMIC relocation";
        return -1;
    }
    if (perr) *perr = "unrecognised container magic";
    return -1;
}
size_t dynalib_exports(const DynalibModule *mod, DynalibExportEntry *out,
                      size_t cap)
{
    if (!mod || !out || !cap || !mod->base_addr || !mod->base_size)
        return 0;
    /* the export table lives inside the mapped image, so the parser can run
     * directly over the mapping: no extra file access, no re-map.          */
    return (size_t)parse_live_image((const void *)mod->base_addr,
                                    mod->base_size, out, cap, NULL, NULL);
}

void dynalib_module_unref(DynalibModule *mod)
{
    if (!mod) return;
    if (--mod->ref_count == 0) {
#if defined(_WIN32)
        FreeLibrary((HMODULE)mod->os_handle);
#else
        dlclose(mod->os_handle);
#endif
        free(mod);
    }
}

void dynalib_close(DynalibModule *mod)
{
    dynalib_module_unref(mod);
}

void dynalib_symbol_free(DynalibSymbol *sym)
{
    if (!sym) return;
    if (sym->jit_stub)
        dynalib_trampoline_free(sym->jit_stub, NULL);
    free(sym);
}

uintptr_t dynalib_module_base(const DynalibModule *mod)
{
    return mod ? mod->base_addr : 0;
}
