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
    unsigned long (*crc32)(unsigned long crc, const unsigned char *buf, unsigned int len);
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
    if (!zapi.uncompress2) zapi.uncompress2 = (int (*)(unsigned char *, unsigned long *, const unsigned char *, unsigned long *))resolve_sym(lib, "uncompress2_z");
    zapi.compressBound = (int (*)(unsigned long))resolve_sym(lib, "compressBound");
    if (!zapi.compressBound) zapi.compressBound = (int (*)(unsigned long))resolve_sym(lib, "compressBound_z");
    zapi.compress2 = (int (*)(unsigned char *, unsigned long *, const unsigned char *, unsigned long, int))resolve_sym(lib, "compress2");
    if (!zapi.compress2) zapi.compress2 = (int (*)(unsigned char *, unsigned long *, const unsigned char *, unsigned long, int))resolve_sym(lib, "compress2_z");
    zapi.crc32 = (unsigned long (*)(unsigned long, const unsigned char *, unsigned int))resolve_sym(lib, "crc32");
    if (!zapi.crc32) zapi.crc32 = (unsigned long (*)(unsigned long, const unsigned char *, unsigned int))resolve_sym(lib, "crc32_z");
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
    return MBJS_EXCEPTION; /* contract: a C func that threw MUST return MBJS_EXCEPTION */
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
    /* lowercase hex alphabet, written as escapes so it cannot be mangled by
       text tooling that redacts digit runs. */
    static const char hexdig[] = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                                   0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0 };
    char *out = (char *)malloc(len * 2 + 1);
    size_t i;
    if (!out) return NULL;
    for (i = 0; i < len; i++) {
        out[i * 2] = hexdig[data[i] >> 4];
        out[i * 2 + 1] = hexdig[data[i] & 15];
    }
    out[len * 2] = 0;
    return out;
}

static MBJSValue hex_out(MBJSContext *ctx, const unsigned char *data, size_t len)
{
    char *hex = bytes_hex(data, len);
    MBJSValue out;
    if (!hex) return js_error(ctx, "out of memory");
    out = js_str(ctx, hex, strlen(hex));
    free(hex);
    return out;
}

/* unpack a hex string into a U32 (used for the crc seed argument) */
static unsigned long hex_u32(const char *hex)
{
    unsigned long v = 0;
    size_t i;
    for (i = 0; i < strlen(hex); i++) {
        int h = hexval(hex[i]);
        if (h < 0) break;
        v = (v << 4) | (unsigned long)h;
    }
    return v;
}

/*
 * Inflate a complete zlib-format stream via uncompress2.
 * accept_bad_adler: when set, a Z_DATA_ERROR (-3) from the trailing checksum
 * is tolerated — this lets us feed streams whose adler32 we zeroed out when
 * reconstructing a zlib wrapper around raw-deflate (gzip/raw framing).
 * Returns 0 on success (out/out_len filled, caller frees), else a zlib rc.
 */
static int inflate_full(const unsigned char *in, size_t in_len,
                        unsigned char **out, size_t *out_len, int accept_bad_adler)
{
    unsigned long cap = (unsigned long)(in_len < 1024 ? 4096 : in_len * 4);
    unsigned long src_len, produced;
    unsigned char *dest;
    int rc;
    for (;;) {
        src_len = (unsigned long)in_len;
        produced = cap;
        dest = (unsigned char *)malloc(cap + 1);
        if (!dest) return -99;
        rc = zapi.uncompress2(dest, &produced, in, &src_len);
        if (rc == 0) { *out = dest; *out_len = (size_t)produced; return 0; }
        if (rc == -5 && cap < 64u * 1024u * 1024u) { free(dest); cap *= 2; continue; }
        if (rc == -3 && accept_bad_adler && produced > 0) { *out = dest; *out_len = (size_t)produced; return 0; }
        free(dest);
        return rc;
    }
}

/* deflate into zlib format (2-byte header + deflate + 4-byte adler) */
static int deflate_zlib(const unsigned char *in, size_t in_len, int level,
                        unsigned char **out, size_t *out_len)
{
    unsigned long cap = zapi.compressBound((unsigned long)in_len);
    unsigned char *dest = (unsigned char *)malloc(cap + 1);
    int rc;
    if (!dest) return -99;
    rc = zapi.compress2(dest, &cap, in, (unsigned long)in_len, level);
    if (rc != 0) { free(dest); return rc; }
    *out = dest;
    *out_len = (size_t)cap;
    return 0;
}

/* build [0x78 0x9C] + raw deflate + 4 zero bytes so uncompress2 will accept it */
static unsigned char *wrap_raw_as_zlib(const unsigned char *raw, size_t rawlen, size_t *total)
{
    unsigned char *z = (unsigned char *)malloc(rawlen + 6);
    if (!z) return NULL;
    z[0] = 0x78; z[1] = 0x9c;
    memcpy(z + 2, raw, rawlen);
    z[rawlen + 2] = 0; z[rawlen + 3] = 0; z[rawlen + 4] = 0; z[rawlen + 5] = 0;
    *total = rawlen + 6;
    return z;
}

/* --------------------------- JS-callable exports --------------------------- */

static MBJSValue zlib_inflate(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t in_len = 0;
    unsigned char *in, *out = NULL;
    size_t out_len = 0;
    int rc;
    MBJSValue res;
    (void)this_val;
    if (argc < 1) return js_error(ctx, "inflate needs input hex");
    if (!load_zlib()) return js_error(ctx, "zlib library not found");
    in = hex_bytes(ctx, argv[0], &in_len);
    if (!in) return MBJS_EXCEPTION;
    rc = inflate_full(in, in_len, &out, &out_len, 0);
    free(in);
    if (rc != 0) { if (out) free(out); return js_error(ctx, "inflate failed"); }
    res = hex_out(ctx, out, out_len);
    free(out);
    return res;
}

static MBJSValue zlib_deflate(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t in_len = 0;
    unsigned char *in, *out = NULL;
    size_t out_len = 0;
    int level = 6;
    int rc;
    MBJSValue res;
    (void)this_val;
    if (argc < 1) return js_error(ctx, "deflate needs input hex");
    if (!load_zlib()) return js_error(ctx, "zlib library not found");
    in = hex_bytes(ctx, argv[0], &in_len);
    if (!in) return MBJS_EXCEPTION;
    if (argc > 1) level = (int)js_number(ctx, argv[1]);
    rc = deflate_zlib(in, in_len, level, &out, &out_len);
    free(in);
    if (rc != 0) { if (out) free(out); return js_error(ctx, "deflate failed"); }
    res = hex_out(ctx, out, out_len);
    free(out);
    return res;
}

/* raw DEFLATE only (no zlib wrapper) */
static MBJSValue zlib_deflateRaw(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t in_len = 0;
    unsigned char *in, *out = NULL;
    size_t out_len = 0, rawlen;
    int level = 6, rc;
    MBJSValue res;
    (void)this_val;
    if (argc < 1) return js_error(ctx, "deflateRaw needs input hex");
    if (!load_zlib()) return js_error(ctx, "zlib library not found");
    in = hex_bytes(ctx, argv[0], &in_len);
    if (!in) return MBJS_EXCEPTION;
    if (argc > 1) level = (int)js_number(ctx, argv[1]);
    rc = deflate_zlib(in, in_len, level, &out, &out_len);
    free(in);
    if (rc != 0) { if (out) free(out); return js_error(ctx, "deflateRaw failed"); }
    if (out_len < 6) { free(out); return js_error(ctx, "deflateRaw: unexpected zlib size"); }
    rawlen = out_len - 6; /* drop 2-byte header + 4-byte adler */
    res = hex_out(ctx, out + 2, rawlen);
    free(out);
    return res;
}

/* gzip container: 10-byte header + raw deflate + CRC32 + ISIZE */
static MBJSValue zlib_gzip(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t in_len = 0;
    unsigned char *in, *z = NULL, *gz;
    size_t z_len = 0, rawlen, gz_len;
    unsigned long crc, isize;
    int level = 6, rc;
    MBJSValue res;
    static const unsigned char HDR[10] = {0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0xff};
    (void)this_val;
    if (argc < 1) return js_error(ctx, "gzip needs input hex");
    if (!load_zlib() || !zapi.crc32) return js_error(ctx, "zlib (or crc32) library not found");
    in = hex_bytes(ctx, argv[0], &in_len);
    if (!in) return MBJS_EXCEPTION;
    if (argc > 1) level = (int)js_number(ctx, argv[1]);
    rc = deflate_zlib(in, in_len, level, &z, &z_len);
    if (rc != 0) { free(in); if (z) free(z); return js_error(ctx, "gzip deflate failed"); }
    rawlen = z_len - 6;
    crc = zapi.crc32(0, in, (unsigned int)in_len);
    isize = (unsigned long)(in_len & 0xffffffffUL);
    gz_len = 10 + rawlen + 8;
    gz = (unsigned char *)malloc(gz_len + 1);
    if (!gz) { free(in); free(z); return js_error(ctx, "out of memory"); }
    memcpy(gz, HDR, 10);
    memcpy(gz + 10, z + 2, rawlen);
    gz[10 + rawlen + 0] = (unsigned char)(crc & 0xff);
    gz[10 + rawlen + 1] = (unsigned char)((crc >> 8) & 0xff);
    gz[10 + rawlen + 2] = (unsigned char)((crc >> 16) & 0xff);
    gz[10 + rawlen + 3] = (unsigned char)((crc >> 24) & 0xff);
    gz[10 + rawlen + 4] = (unsigned char)(isize & 0xff);
    gz[10 + rawlen + 5] = (unsigned char)((isize >> 8) & 0xff);
    gz[10 + rawlen + 6] = (unsigned char)((isize >> 16) & 0xff);
    gz[10 + rawlen + 7] = (unsigned char)((isize >> 24) & 0xff);
    free(in); free(z);
    res = hex_out(ctx, gz, gz_len);
    free(gz);
    return res;
}

/* raw inflate: accept raw-deflate (no wrapper) */
static MBJSValue zlib_inflateRaw(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t in_len = 0, total = 0;
    unsigned char *in, *z, *out = NULL;
    size_t out_len = 0;
    int rc;
    MBJSValue res;
    (void)this_val;
    if (argc < 1) return js_error(ctx, "inflateRaw needs input hex");
    if (!load_zlib()) return js_error(ctx, "zlib library not found");
    in = hex_bytes(ctx, argv[0], &in_len);
    if (!in) return MBJS_EXCEPTION;
    z = wrap_raw_as_zlib(in, in_len, &total);
    free(in);
    if (!z) return js_error(ctx, "out of memory");
    rc = inflate_full(z, total, &out, &out_len, 1);
    free(z);
    if (rc != 0) { if (out) free(out); return js_error(ctx, "inflateRaw failed"); }
    res = hex_out(ctx, out, out_len);
    free(out);
    return res;
}

static MBJSValue zlib_gunzip(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t in_len = 0, total = 0, rawlen;
    unsigned char *in, *raw, *z, *out = NULL;
    size_t out_len = 0, pos;
    unsigned long tcrc, isize, cprc;
    int rc;
    MBJSValue res;
    (void)this_val;
    if (argc < 1) return js_error(ctx, "gunzip needs input hex");
    if (!load_zlib() || !zapi.crc32) return js_error(ctx, "zlib (or crc32) library not found");
    in = hex_bytes(ctx, argv[0], &in_len);
    if (!in) return MBJS_EXCEPTION;
    if (in_len < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) {
        free(in); return js_error(ctx, "gunzip: not a gzip stream");
    }
    pos = 10;
    if (in[3] & 0x04) { /* FEXTRA */
        unsigned xlen = (unsigned)in[pos] | ((unsigned)in[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (in[3] & 0x08) { while (pos < in_len && in[pos] != 0) pos++; pos++; } /* FNAME */
    if (in[3] & 0x10) { while (pos < in_len && in[pos] != 0) pos++; pos++; } /* FCOMMENT */
    if (in[3] & 0x02) pos += 2; /* FHCRC */
    if (pos + 8 > in_len) { free(in); return js_error(ctx, "gunzip: truncated header"); }
    raw = &in[pos];
    rawlen = in_len - pos - 8;
    tcrc = (unsigned long)in[in_len - 8] | ((unsigned long)in[in_len - 7] << 8) |
           ((unsigned long)in[in_len - 6] << 16) | ((unsigned long)in[in_len - 5] << 24);
    isize = (unsigned long)in[in_len - 4] | ((unsigned long)in[in_len - 3] << 8) |
            ((unsigned long)in[in_len - 2] << 16) | ((unsigned long)in[in_len - 1] << 24);
    (void)isize;
    z = wrap_raw_as_zlib(raw, rawlen, &total);
    if (!z) { free(in); return js_error(ctx, "out of memory"); }
    rc = inflate_full(z, total, &out, &out_len, 1);
    free(z);
    if (rc != 0) { free(in); if (out) free(out); return js_error(ctx, "gunzip: inflate failed"); }
    cprc = zapi.crc32(0, out, (unsigned int)out_len);
    free(in);
    if (cprc != tcrc) { free(out); return js_error(ctx, "gunzip: crc mismatch"); }
    if (out_len != (size_t)isize) { free(out); return js_error(ctx, "gunzip: size mismatch"); }
    res = hex_out(ctx, out, out_len);
    free(out);
    return res;
}

static MBJSValue zlib_crc32(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t in_len = 0;
    char *seed = NULL;
    unsigned char *in;
    unsigned long seedv = 0, crc;
    (void)this_val;
    if (argc < 1) return js_error(ctx, "crc32 needs input hex");
    if (!load_zlib() || !zapi.crc32) return js_error(ctx, "zlib (or crc32) library not found");
    in = hex_bytes(ctx, argv[0], &in_len);
    if (!in) return MBJS_EXCEPTION;
    if (argc > 1) seed = js_cstr_len(ctx, argv[1], NULL);
    if (seed) { seedv = hex_u32(seed); free(seed); }
    crc = zapi.crc32(seedv, in, (unsigned int)in_len);
    free(in);
    /* emit big-endian so the JS side reads the uint32 back correctly */
    {
        unsigned char be[4] = { (unsigned char)(crc >> 24), (unsigned char)(crc >> 16),
                                (unsigned char)(crc >> 8), (unsigned char)crc };
        return hex_out(ctx, be, 4);
    }
}

static int zlib_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("inflateHex", 1, zlib_inflate),
        MBJS_CFUNC_DEF("deflateHex", 2, zlib_deflate),
        MBJS_CFUNC_DEF("inflateRawHex", 1, zlib_inflateRaw),
        MBJS_CFUNC_DEF("deflateRawHex", 2, zlib_deflateRaw),
        MBJS_CFUNC_DEF("gzipHex", 2, zlib_gzip),
        MBJS_CFUNC_DEF("gunzipHex", 1, zlib_gunzip),
        MBJS_CFUNC_DEF("crc32Hex", 2, zlib_crc32),
    };
    return rt.SetModuleExportList(ctx, m, tab, (int)(sizeof(tab) / sizeof(tab[0])));
}

MBJS_MODULE_EXTERN MBJSModuleDef *mbjs_init_module(MBJSContext *ctx, const char *name)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("inflateHex", 1, zlib_inflate),
        MBJS_CFUNC_DEF("deflateHex", 2, zlib_deflate),
        MBJS_CFUNC_DEF("inflateRawHex", 1, zlib_inflateRaw),
        MBJS_CFUNC_DEF("deflateRawHex", 2, zlib_deflateRaw),
        MBJS_CFUNC_DEF("gzipHex", 2, zlib_gzip),
        MBJS_CFUNC_DEF("gunzipHex", 1, zlib_gunzip),
        MBJS_CFUNC_DEF("crc32Hex", 2, zlib_crc32),
    };
    const char *missing = mbjs_runtime_resolve(&rt);
    MBJSModuleDef *m;
    if (missing) return NULL;
    m = rt.NewCModule(ctx, name ? name : "mochizlib", zlib_mod_init);
    if (!m) return NULL;
    if (rt.AddModuleExportList(ctx, m, tab, (int)(sizeof(tab) / sizeof(tab[0]))) != 0) return NULL;
    return m;
}