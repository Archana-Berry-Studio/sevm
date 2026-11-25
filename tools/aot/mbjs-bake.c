/*
 * mbjs-bake.c — MochiBake: an internal organ of MochiBerry.
 *
 * MochiBake reads the PE export/import tables of a native plugin so the AOT
 * pipeline can decide WHICH DLLs the program actually calls (and which
 * symbols it uses from each), then materialises the ones it keeps back onto
 * disk at startup so a single-file bundle can run with no external sources.
 *
 * It is neither node_modules nor mochi_modules: it is part of the engine
 * itself, exposed as the built-in module "mbjs:bake".
 *
 * Built-in exports (JS side):
 *   bake.peInfo(path)   -> JSON {machine,imageSize,exports[],imports[]}
 *   bake.read(path)     -> Uint8Array of the DLL bytes (for embedding)
 *   bake.write(name, bytes) -> path of the materialised file (runtime)
 *   bake.tempDir()      -> the bake scratch directory
 *
 * The same C entry points are used directly by the AOT host.
 */

#include "mbjs.h"
#include "mbjs-libc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* PE helpers (offset-based, no windows.h)                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *base;
    size_t         size;
} BakedImage;

static const uint8_t *pe_rva_to_off(const BakedImage *img, uint32_t rva)
{
    const uint8_t *p = img->base;
    unsigned long off, opt;
    unsigned short nsec, optsize;
    int i;

    if (img->size < 0x40) return NULL;
    off = *(const unsigned long *)(p + 0x3C);           /* e_lfanew */
    if (off + 4 + 20 + 0x38 >= img->size) return NULL;
    if (memcmp(p + off, "PE\0\0", 4) != 0) return NULL;
    nsec = *(const unsigned short *)(p + off + 4 + 2);  /* NumberOfSections */
    optsize = *(const unsigned short *)(p + off + 4 + 16);
    if (optsize < 0x38) return NULL;
    opt = off + 4 + 20;
    /* sections start right after the optional header */
    for (i = 0; i < nsec; i++) {
        const uint8_t *sec = p + opt + optsize + i * 40;
        uint32_t va = *(const uint32_t *)(sec + 12);
        uint32_t vsz = *(const uint32_t *)(sec + 8);
        uint32_t rsz = *(const uint32_t *)(sec + 16);
        uint32_t raw = *(const uint32_t *)(sec + 20);
        uint32_t span = vsz > rsz ? vsz : rsz;
        if (rva >= va && rva < va + span) {
            uint32_t offs = rva - va + raw;
            if (offs < img->size) return p + offs;
        }
    }
    return NULL;
}

static const char *pe_cstr(const BakedImage *img, uint32_t rva)
{
    const uint8_t *q = pe_rva_to_off(img, rva);
    size_t max = 512, i;
    if (!q) return NULL;
    for (i = 0; i < max && (q + i) < img->base + img->size; i++)
        if (q[i] == 0) return (const char *)q;
    return NULL;
}

static int mkdir_win(const char *path);

/* Build a JSON description of the PE: machine, image size, export names,
   import DLL names.  Returns a malloc'd string (free with free()). */
char *mbjs_bake_pe_info(const char *path)
{
    FILE *f = fopen(path, "rb");
    BakedImage img;
    long flen;
    uint8_t *buf;
    unsigned long off, opt, dd;
    uint32_t exp_rva = 0, imp_rva = 0;
    char *out = NULL;
    size_t cap = 0, len = 0;
    unsigned short machine = 0;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END); flen = ftell(f); fseek(f, 0, SEEK_SET);
    if (flen < 0x40 || flen > 64 * 1024 * 1024) { fclose(f); return NULL; }
    buf = (uint8_t *)malloc(flen);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, flen, f) != (size_t)flen) { free(buf); fclose(f); return NULL; }
    fclose(f);

    img.base = buf; img.size = (size_t)flen;
    off = *(const unsigned long *)(buf + 0x3C);
    if (off + 4 + 20 + 0x38 >= (unsigned long)flen || memcmp(buf + off, "PE\0\0", 4) != 0)
        { free(buf); return NULL; }

    machine = *(const unsigned short *)(buf + off + 4);
    opt = off + 4 + 20;
    /* data directories: optional header + 0x60 (PE32+) / +0x60? PE32: +0x60;
       PE32+ optional header is 0x70 before data dirs?  Both PE32 and PE32+
       put the data directories at offset 0x60 from the start of the
       optional header (IMAGE_OPTIONAL_HEADER32/64 both have 16 dirs at the
       end after a 0x60/0x70 header; the dirs themselves start at +0x60 for
       PE32 and +0x70 for PE32+ — detect via the optional-header magic). */
    {
        unsigned short magic = *(const unsigned short *)(buf + opt);
        dd = opt + (magic == 0x20B ? 0x70 : 0x60);
    }
    if (dd + 16 + 16 < (unsigned long)flen) {
        exp_rva = *(const uint32_t *)(buf + dd + 0);   /* export dir */
        imp_rva = *(const uint32_t *)(buf + dd + 8);   /* import dir */
    }

    cap = 512; out = (char *)malloc(cap); len = 0;
    len += (size_t)snprintf(out + len, cap - len,
        "{\"machine\":%u,\"imageSize\":%lu,\"exports\":[",
        machine, (unsigned long)*(const uint32_t *)(buf + opt + 0x38));

    /* export names */
    if (exp_rva) {
        const uint8_t *e = pe_rva_to_off(&img, exp_rva);
        if (e && e + 40 <= buf + flen) {
            uint32_t nnames = *(const uint32_t *)(e + 24);
            uint32_t anames = *(const uint32_t *)(e + 32);
            uint32_t aords  = *(const uint32_t *)(e + 36);
            uint32_t i;
            for (i = 0; i < nnames; i++) {
                const uint8_t *names = pe_rva_to_off(&img, anames);
                const uint8_t *ords  = pe_rva_to_off(&img, aords);
                const char *nm;
                uint32_t nrva;
                if (!names || !ords || i * 4 + 4 > (unsigned long)(img.size)) break;
                nrva = *(const uint32_t *)(names + i * 4);
                nm = pe_cstr(&img, nrva);
                if (!nm) continue;
                if (len + 24 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
                if (i) { out[len++] = ','; }
                len += (size_t)snprintf(out + len, cap - len, "\"%s\"", nm);
            }
        }
    }
    len += (size_t)snprintf(out + len, cap - len, "],\"imports\":[");

    /* import DLL names */
    if (imp_rva) {
        const uint8_t *d = pe_rva_to_off(&img, imp_rva);
        int first = 1;
        while (d && d + 20 <= buf + flen) {
            uint32_t name_rva = *(const uint32_t *)(d + 12);
            const char *nm;
            if (name_rva == 0 && *(const uint32_t *)(d + 16) == 0) break;
            nm = pe_cstr(&img, name_rva);
            if (nm) {
                if (!first) { out[len++] = ','; }
                first = 0;
                len += (size_t)snprintf(out + len, cap - len, "\"%s\"", nm);
            }
            d += 20;
        }
    }
    len += (size_t)snprintf(out + len, cap - len, "]}");
    free(buf);
    return out;
}

/* Write an embedded blob to the bake scratch dir.  Returns 0 on success. */
int mbjs_bake_materialize(const char *name, const uint8_t *bytes, size_t len,
                          char *out_path, size_t out_cap)
{
    const char *tmp = getenv("TEMP");
    char dir[1024];
    FILE *f;
    if (!tmp) tmp = "C:/Users/Administrator/AppData/Local/Temp";
    snprintf(dir, sizeof dir, "%s/mochibake", tmp);
    if (mkdir_win(dir) != 0) { /* already exists is fine */ }
    snprintf(out_path, out_cap, "%s/%s", dir, name);
    f = fopen(out_path, "wb");
    if (!f) return -1;
    if (len && fwrite(bytes, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* Direct KERNEL32 import — avoids pulling the CRT spawn machinery (system()
   cost ~360 KB of engine archive and would defeat the whole point). */
extern int CreateDirectoryA(const char *lpPathName, void *lpSecurityAttributes);
__attribute__((weak)) extern int CreateDirectoryW(const void *lpPathName, void *lpSecurityAttributes);

static int mkdir_win(const char *path)
{
    return CreateDirectoryA(path, 0);
}

/* ------------------------------------------------------------------ */
/* JS bindings                                                         */
/* ------------------------------------------------------------------ */

static MBJSValue bake_pe_info(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    const char *path;
    char *json;
    if (argc < 1) return MBJS_UNDEFINED;
    path = MBJS_ToCString(ctx, argv[0]);
    if (!path) return MBJS_UNDEFINED;
    json = mbjs_bake_pe_info(path);
    MBJS_FreeCString(ctx, path);
    if (!json) return MBJS_UNDEFINED;
    {
        MBJSValue v = MBJS_NewString(ctx, json);
        free(json);
        return v;
    }
}

static MBJSValue bake_read(MBJSContext *ctx, MBJSValueConst this_val,
                           int argc, MBJSValueConst *argv)
{
    const char *path;
    size_t len = 0;
    uint8_t *buf;
    MBJSValue ret;
    if (argc < 1) return MBJS_UNDEFINED;
    path = MBJS_ToCString(ctx, argv[0]);
    if (!path) return MBJS_UNDEFINED;
    buf = mbjs_load_file(ctx, &len, path);
    MBJS_FreeCString(ctx, path);
    if (!buf) return MBJS_UNDEFINED;
    ret = MBJS_NewUint8ArrayCopy(ctx, buf, len);
    free(buf);
    return ret;
}

static MBJSValue bake_write(MBJSContext *ctx, MBJSValueConst this_val,
                            int argc, MBJSValueConst *argv)
{
    const char *name;
    MBJSValue bytes_val;
    const uint8_t *bytes = NULL;
    size_t len = 0;
    char path[1024];
    if (argc < 2) return MBJS_UNDEFINED;
    name = MBJS_ToCString(ctx, argv[0]);
    bytes_val = argv[1];
    if (!name) return MBJS_UNDEFINED;
    bytes = MBJS_GetArrayBuffer(ctx, &len, bytes_val);
    if (!bytes) { MBJS_FreeCString(ctx, name); return MBJS_UNDEFINED; }
    if (mbjs_bake_materialize(name, bytes, len, path, sizeof path) != 0) {
        MBJS_FreeCString(ctx, name);
        return MBJS_ThrowTypeError(ctx, "mochibake: cannot materialize %s", name);
    }
    MBJS_FreeCString(ctx, name);
    return MBJS_NewString(ctx, path);
}

static MBJSValue bake_temp_dir(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    const char *tmp = getenv("TEMP");
    char dir[1024];
    if (!tmp) tmp = "C:/Users/Administrator/AppData/Local/Temp";
    snprintf(dir, sizeof dir, "%s/mochibake", tmp);
    return MBJS_NewString(ctx, dir);
}

static const MBJSCFunctionListEntry bake_tab[] = {
    MBJS_CFUNC_DEF("peInfo",  1, bake_pe_info),
    MBJS_CFUNC_DEF("read",    1, bake_read),
    MBJS_CFUNC_DEF("write",   2, bake_write),
    MBJS_CFUNC_DEF("tempDir", 0, bake_temp_dir),
};

static int bake_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    return MBJS_SetModuleExportList(ctx, m, bake_tab, sizeof bake_tab / sizeof bake_tab[0]);
}

/* Plain JS object with the same surface — used by a host that wants the organ
   on the global object without going through module namespaces (a freshly
   registered C module has no namespace object yet). */
MBJSValue mbjs_bake_object(MBJSContext *ctx)
{
    MBJSValue o = MBJS_NewObject(ctx);
    MBJS_SetPropertyStr(ctx, o, "peInfo",  MBJS_NewCFunction(ctx, bake_pe_info, "peInfo", 1));
    MBJS_SetPropertyStr(ctx, o, "read",     MBJS_NewCFunction(ctx, bake_read, "read", 1));
    MBJS_SetPropertyStr(ctx, o, "write",    MBJS_NewCFunction(ctx, bake_write, "write", 2));
    MBJS_SetPropertyStr(ctx, o, "tempDir",  MBJS_NewCFunction(ctx, bake_temp_dir, "tempDir", 0));
    return o;
}

MBJSModuleDef *mbjs_init_module_bake(MBJSContext *ctx, const char *module_name)
{
    MBJSModuleDef *m;
    m = MBJS_NewCModule(ctx, module_name ? module_name : "mbjs:bake", bake_mod_init);
    if (!m) return NULL;
    if (MBJS_AddModuleExportList(ctx, m, bake_tab, sizeof bake_tab / sizeof bake_tab[0]) != 0)
        return NULL;
    return m;
}
