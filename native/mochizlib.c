#include "mbjs_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

static MBJSRuntime rt;

typedef struct {
    int (*uncompress2)(unsigned char *dest, unsigned long *destLen, const unsigned char *source, unsigned long *sourceLen);
    int (*compressBound)(unsigned long sourceLen);
    int (*compress2)(unsigned char *dest, unsigned long *destLen, const unsigned char *source, unsigned long sourceLen, int level);
} ZApi;

static ZApi zapi;

static void *resolve_sym(void *lib, const char *name)
{
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

static int load_zlib(void)
{
    void *lib = NULL;
    if (zapi.uncompress2) return 1;
#ifdef _WIN32
    lib = (void *)GetModuleHandleA("zlib1.dll");
    if (!lib) lib = (void *)LoadLibraryA("zlib1.dll");
    if (!lib) lib = (void *)LoadLibraryA("libz.dll");
    if (!lib) lib = (void *)LoadLibraryA("zlib.dll");
#else
    lib = dlopen("libz.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libz.so", RTLD_NOW);
#endif
    if (!lib) return 0;
    zapi.uncompress2 = (int (*)(unsigned char *, unsigned long *, const unsigned char *, unsigned long *))resolve_sym(lib, "uncompress2");
    if (!zapi.uncompress2) {
        zapi.uncompress2 = (int (*)(unsigned char *, unsigned long *, const unsigned char *, unsigned long *))resolve_sym(lib, "uncompress2_z");
    }
    zapi.compressBound = (int (*)(unsigned long))resolve_sym(lib, "compressBound");
    if (!zapi.compressBound) zapi.compressBound = (int (*)(unsigned long))resolve_sym(lib, "compressBound_z");
    zapi.compress2 = (int (*)(unsigned char *, unsigned long *, const unsigned char *, unsigned long, int))resolve_sym(lib, "compress2");
    if (!zapi.compress2) zapi.compress2 = (int (*)(unsigned char *, unsigned long *, const unsigned char *, unsigned long, int))resolve_sym(lib, "compress2_z");
    return zapi.uncompress2 && zapi.compressBound && zapi.compress2;
}

static char *js_cstr_len(MBJSContext *ctx, MBJSValueConst v, size_t *out_len)
{
    size_t len = 0;
    const char *p = rt.ToCStringLen(ctx, &len, v);
    char *out;
    if (!p) return NULL;
    out = (char *)malloc(len + 1);
    if (!out) {
        rt.FreeCString(ctx, p);
        return NULL;
    }
    memcpy(out, p, len);
    out[len] = 0;
    rt.FreeCString(ctx, p);
    if (out_len) *out_len = len;
    return out;
}

static MBJSValue js_str(MBJSContext *ctx, const char *s, size_t len)
{
    return rt.NewStringLen(ctx, s ? s : "", s ? len : 0);
}

static MBJSValue js_error(MBJSContext *ctx, const char *what)
{
    rt.ThrowTypeError(ctx, "mochizlib: %s", what);
    return MBJS_UNDEFINED;
}

static double js_number(MBJSContext *ctx, MBJSValueConst v)
{
    double d = 0;
    if (rt.ToFloat64(ctx, &d, v) != 0) return 0;
    return d;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static unsigned char *hex_bytes(MBJSContext *ctx, MBJSValueConst value, size_t *out_len)
{
    char *text = js_cstr_len(ctx, value, NULL);
    size_t len, i;
    unsigned char *out;
    if (!text) return NULL;
    len = strlen(text);
    if (len % 2 != 0) {
        free(text);
        js_error(ctx, "hex input must have even length");
        return NULL;
    }
    out = (unsigned char *)malloc(len / 2 + 1);
    if (!out) {
        free(text);
        js_error(ctx, "out of memory");
        return NULL;
    }
    for (i = 0; i < len / 2; i++) {
        int hi = hexval(text[i * 2]);
        int lo = hexval(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            free(text);
            free(out);
            js_error(ctx, "invalid hex input");
            return NULL;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    free(text);
    out[len / 2] = 0;
    if (out_len) *out_len = len / 2;
    return out;
}

static char *bytes_hex(const unsigned char *data, size_t len)
{
    static const char table[] = "0123456789abcdef";
    char *out = (char *)malloc(len * 2 + 1);
    size_t i;
    if (!out) return NULL;
    for (i = 0; i < len; i++) {
        out[i * 2] = table[data[i] >> 4];
        out[i * 2 + 1] = table[data[i] & 15];
    }
    out[len * 2] = 0;
    return out;
}

static MBJSValue zlib_inflate(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t in_len = 0;
    unsigned char *in;
    unsigned long dest_len;
    unsigned char *dest;
    int rc;
    MBJSValue out;
    (void)this_val;
    if (argc < 1) return js_error(ctx, "inflate needs input hex");
    if (!load_zlib()) return js_error(ctx, "zlib library not found");
    in = hex_bytes(ctx, argv[0], &in_len);
    if (!in) return MBJS_UNDEFINED;
    dest_len = (unsigned long)(in_len < 1024 ? 4096 : in_len * 4);
    for (;;) {
        unsigned long src_len = (unsigned long)in_len;
        dest = (unsigned char *)malloc(dest_len + 1);
        if (!dest) {
            free(in);
            return js_error(ctx, "out of memory");
        }
        rc = zapi.uncompress2(dest, &dest_len, in, &src_len);
        if (rc == 0) break;
        free(dest);
        if (rc == -5 && dest_len < 64u * 1024u * 1024u) {
            dest_len *= 2;
            continue;
        }
        free(in);
        return js_error(ctx, "inflate failed");
    }
    out = js_str(ctx, (char *)bytes_hex(dest, dest_len), dest_len * 2);
    free(dest);
    free(in);
    return out;
}

static MBJSValue zlib_deflate(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t in_len = 0;
    unsigned char *in;
    unsigned long dest_len;
    unsigned char *dest;
    int level = 6;
    int rc;
    char *hex;
    MBJSValue out;
    (void)this_val;
    if (argc < 1) return js_error(ctx, "deflate needs input hex");
    if (!load_zlib()) return js_error(ctx, "zlib library not found");
    in = hex_bytes(ctx, argv[0], &in_len);
    if (!in) return MBJS_UNDEFINED;
    if (argc > 1) level = (int)js_number(ctx, argv[1]);
    dest_len = zapi.compressBound((unsigned long)in_len);
    dest = (unsigned char *)malloc(dest_len + 1);
    if (!dest) {
        free(in);
        return js_error(ctx, "out of memory");
    }
    rc = zapi.compress2(dest, &dest_len, in, (unsigned long)in_len, level);
    if (rc != 0) {
        free(dest);
        free(in);
        return js_error(ctx, "deflate failed");
    }
    hex = bytes_hex(dest, dest_len);
    out = js_str(ctx, hex, strlen(hex));
    free(hex);
    free(dest);
    free(in);
    return out;
}

static int zlib_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("inflateHex", 1, zlib_inflate),
        MBJS_CFUNC_DEF("deflateHex", 2, zlib_deflate),
    };
    return rt.SetModuleExportList(ctx, m, tab, (int)(sizeof(tab) / sizeof(tab[0])));
}

MBJS_MODULE_EXTERN MBJSModuleDef *mbjs_init_module(MBJSContext *ctx, const char *name)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("inflateHex", 1, zlib_inflate),
        MBJS_CFUNC_DEF("deflateHex", 2, zlib_deflate),
    };
    const char *missing = mbjs_runtime_resolve(&rt);
    MBJSModuleDef *m;
    if (missing) return NULL;
    m = rt.NewCModule(ctx, name ? name : "mochizlib", zlib_mod_init);
    if (!m) return NULL;
    if (rt.AddModuleExportList(ctx, m, tab, (int)(sizeof(tab) / sizeof(tab[0]))) != 0) return NULL;
    return m;
}
