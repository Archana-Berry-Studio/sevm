#include "mbjs_runtime.h"

#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BCRYPT_CHAIN_MODE_CTR
#define BCRYPT_CHAIN_MODE_CTR L"ChainingModeCTR"
#endif

static MBJSRuntime rt;

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
    if (!s) return rt.NewStringLen(ctx, "", 0);
    return rt.NewStringLen(ctx, s, len);
}

static MBJSValue js_error(MBJSContext *ctx, const char *what)
{
    rt.ThrowTypeError(ctx, "mochicrypto: %s", what);
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
    size_t len;
    size_t i;
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

static void append(char **buf, size_t *cap, size_t *len, const char *s, size_t n)
{
    size_t need = *len + n + 1;
    if (need > *cap) {
        size_t next = *cap ? *cap : 256;
        char *tmp;
        while (next < need) next *= 2;
        tmp = (char *)realloc(*buf, next);
        if (!tmp) return;
        *buf = tmp;
        *cap = next;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static MBJSValue hex_result(MBJSContext *ctx, const unsigned char *data, size_t len)
{
    char *hex = bytes_hex(data, len);
    MBJSValue out;
    if (!hex) return js_error(ctx, "out of memory");
    out = js_str(ctx, hex, strlen(hex));
    free(hex);
    return out;
}

static MBJSValue hash_hex(MBJSContext *ctx, LPCWSTR alg, MBJSValueConst value)
{
    size_t data_len = 0;
    unsigned char *data = hex_bytes(ctx, value, &data_len);
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    unsigned char *obj = NULL;
    unsigned char *digest = NULL;
    DWORD obj_len = 0;
    DWORD digest_len = 0;
    DWORD result_len = 0;
    NTSTATUS status;
    MBJSValue out;
    if (!data) return MBJS_EXCEPTION;
    status = BCryptOpenAlgorithmProvider(&hAlg, alg, NULL, 0);
    if (!BCRYPT_SUCCESS(status)) goto fail;
    status = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_len, sizeof(obj_len), &result_len, 0);
    if (!BCRYPT_SUCCESS(status)) goto fail;
    status = BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&digest_len, sizeof(digest_len), &result_len, 0);
    if (!BCRYPT_SUCCESS(status)) goto fail;
    obj = (unsigned char *)malloc(obj_len);
    digest = (unsigned char *)malloc(digest_len);
    if (!obj || !digest) goto fail;
    status = BCryptCreateHash(hAlg, &hHash, obj, obj_len, NULL, 0, 0);
    if (!BCRYPT_SUCCESS(status)) goto fail;
    status = BCryptHashData(hHash, data, (ULONG)data_len, 0);
    if (!BCRYPT_SUCCESS(status)) goto fail;
    status = BCryptFinishHash(hHash, digest, digest_len, 0);
    if (!BCRYPT_SUCCESS(status)) goto fail;
    out = hex_result(ctx, digest, digest_len);
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    free(obj);
    free(digest);
    free(data);
    return out;
fail:
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    free(obj);
    free(digest);
    free(data);
    return js_error(ctx, "hash failed");
}

static MBJSValue hmac_hex(MBJSContext *ctx, LPCWSTR alg, MBJSValueConst keyValue, MBJSValueConst dataValue)
{
    size_t key_len = 0;
    size_t data_len = 0;
    unsigned char *key = hex_bytes(ctx, keyValue, &key_len);
    unsigned char *data = hex_bytes(ctx, dataValue, &data_len);
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    unsigned char *obj = NULL;
    unsigned char *digest = NULL;
    DWORD obj_len = 0;
    DWORD digest_len = 0;
    DWORD result_len = 0;
    NTSTATUS status;
    MBJSValue out;
    if (!key || !data) {
        free(key);
        free(data);
        return MBJS_EXCEPTION;
    }
    status = BCryptOpenAlgorithmProvider(&hAlg, alg, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status)) goto fail;
    status = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_len, sizeof(obj_len), &result_len, 0);
    if (!BCRYPT_SUCCESS(status)) goto fail;
    status = BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&digest_len, sizeof(digest_len), &result_len, 0);
    if (!BCRYPT_SUCCESS(status)) goto fail;
    obj = (unsigned char *)malloc(obj_len);
    digest = (unsigned char *)malloc(digest_len);
    if (!obj || !digest) goto fail;
    status = BCryptCreateHash(hAlg, &hHash, obj, obj_len, key, (ULONG)key_len, 0);
    if (!BCRYPT_SUCCESS(status)) goto fail;
    status = BCryptHashData(hHash, data, (ULONG)data_len, 0);
    if (!BCRYPT_SUCCESS(status)) goto fail;
    status = BCryptFinishHash(hHash, digest, digest_len, 0);
    if (!BCRYPT_SUCCESS(status)) goto fail;
    out = hex_result(ctx, digest, digest_len);
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    free(obj);
    free(digest);
    free(key);
    free(data);
    return out;
fail:
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    free(obj);
    free(digest);
    free(key);
    free(data);
    return js_error(ctx, "hmac failed");
}

static MBJSValue crypto_random(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t len;
    unsigned char *buf;
    MBJSValue out;
    (void)this_val;
    if (argc < 1) return js_error(ctx, "randomHex needs a length");
    len = (size_t)js_number(ctx, argv[0]);
    if (len == 0 || len > 1024 * 1024) return js_error(ctx, "invalid random length");
    buf = (unsigned char *)malloc(len);
    if (!buf) return js_error(ctx, "out of memory");
    if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        free(buf);
        return js_error(ctx, "random failed");
    }
    out = hex_result(ctx, buf, len);
    free(buf);
    return out;
}

#define HASH_FN(name, alg) \
static MBJSValue name(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv) \
{ (void)this_val; if (argc < 1) return js_error(ctx, #name " needs data"); return hash_hex(ctx, alg, argv[0]); }

#define HMAC_FN(name, alg) \
static MBJSValue name(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv) \
{ (void)this_val; if (argc < 2) return js_error(ctx, #name " needs key and data"); return hmac_hex(ctx, alg, argv[0], argv[1]); }

HASH_FN(crypto_md5, BCRYPT_MD5_ALGORITHM)
HASH_FN(crypto_sha1, BCRYPT_SHA1_ALGORITHM)
HASH_FN(crypto_sha256, BCRYPT_SHA256_ALGORITHM)
HASH_FN(crypto_sha384, BCRYPT_SHA384_ALGORITHM)
HASH_FN(crypto_sha512, BCRYPT_SHA512_ALGORITHM)
HMAC_FN(crypto_hmac_sha1, BCRYPT_SHA1_ALGORITHM)
HMAC_FN(crypto_hmac_sha256, BCRYPT_SHA256_ALGORITHM)
HMAC_FN(crypto_hmac_sha512, BCRYPT_SHA512_ALGORITHM)

static MBJSValue crypto_pbkdf2(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t pwd_len = 0;
    size_t salt_len = 0;
    unsigned char *pwd;
    unsigned char *salt;
    unsigned char *out_buf;
    BCRYPT_ALG_HANDLE hPrf = NULL;
    ULONGLONG iterations;
    ULONG key_len;
    MBJSValue out;
    (void)this_val;
    if (argc < 5) return js_error(ctx, "pbkdf2 needs password, salt, iterations, length, hash");
    pwd = hex_bytes(ctx, argv[0], &pwd_len);
    salt = hex_bytes(ctx, argv[1], &salt_len);
    if (!pwd || !salt) {
        free(pwd);
        free(salt);
        return MBJS_EXCEPTION;
    }
    iterations = (ULONGLONG)js_number(ctx, argv[2]);
    key_len = (ULONG)js_number(ctx, argv[3]);
    if (iterations == 0 || key_len == 0 || key_len > 1024 * 1024) {
        free(pwd);
        free(salt);
        return js_error(ctx, "invalid pbkdf2 parameters");
    }
    out_buf = (unsigned char *)malloc(key_len);
    if (!out_buf) {
        free(pwd);
        free(salt);
        return js_error(ctx, "out of memory");
    }
    {
        int which = (int)js_number(ctx, argv[4]);
        LPCWSTR alg = BCRYPT_SHA256_ALGORITHM;
        if (which == 1) alg = BCRYPT_SHA1_ALGORITHM;
        else if (which == 3) alg = BCRYPT_SHA384_ALGORITHM;
        else if (which == 4) alg = BCRYPT_SHA512_ALGORITHM;
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hPrf, alg, NULL, 0))) {
            free(pwd);
            free(salt);
            free(out_buf);
            return js_error(ctx, "pbkdf2 provider failed");
        }
    }
    if (!BCRYPT_SUCCESS(BCryptDeriveKeyPBKDF2(hPrf, pwd, (ULONG)pwd_len, salt, (ULONG)salt_len, iterations, out_buf, key_len, 0))) {
        BCryptCloseAlgorithmProvider(hPrf, 0);
        free(pwd);
        free(salt);
        free(out_buf);
        return js_error(ctx, "pbkdf2 failed");
    }
    out = hex_result(ctx, out_buf, key_len);
    BCryptCloseAlgorithmProvider(hPrf, 0);
    free(pwd);
    free(salt);
    free(out_buf);
    return out;
}

static BCRYPT_KEY_HANDLE import_aes_key(BCRYPT_ALG_HANDLE hAlg, const unsigned char *key, ULONG key_len)
{
    BCRYPT_KEY_DATA_BLOB_HEADER header;
    unsigned char *blob;
    BCRYPT_KEY_HANDLE hKey = NULL;
    if (key_len != 16 && key_len != 24 && key_len != 32) return NULL;
    blob = (unsigned char *)malloc(sizeof(header) + key_len);
    if (!blob) return NULL;
    header.dwMagic = BCRYPT_KEY_DATA_BLOB_MAGIC;
    header.dwVersion = BCRYPT_KEY_DATA_BLOB_VERSION1;
    header.cbKeyData = key_len;
    memcpy(blob, &header, sizeof(header));
    memcpy(blob + sizeof(header), key, key_len);
    if (!BCRYPT_SUCCESS(BCryptImportKey(hAlg, NULL, BCRYPT_KEY_DATA_BLOB, &hKey, NULL, 0, blob, (ULONG)(sizeof(header) + key_len), 0))) hKey = NULL;
    free(blob);
    return hKey;
}

static MBJSValue aes_gcm_encrypt(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t key_len = 0; size_t iv_len = 0; size_t aad_len = 0; size_t data_len = 0;
    unsigned char *key; unsigned char *iv; unsigned char *aad; unsigned char *data;
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    unsigned char *out = NULL;
    unsigned char tag[16];
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    ULONG done = 0;
    char *data_hex = NULL;
    char *tag_hex = NULL;
    char *json = NULL;
    size_t cap = 0; size_t len = 0;
    MBJSValue result;
    (void)this_val;
    if (argc < 4) return js_error(ctx, "aesGcmEncrypt needs key, iv, aad, data");
    key = hex_bytes(ctx, argv[0], &key_len);
    iv = hex_bytes(ctx, argv[1], &iv_len);
    aad = hex_bytes(ctx, argv[2], &aad_len);
    data = hex_bytes(ctx, argv[3], &data_len);
    if (!key || !iv || !aad || !data) goto fail;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0))) goto fail;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, (ULONG)((wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t)), 0))) goto fail;
    hKey = import_aes_key(hAlg, key, (ULONG)key_len);
    if (!hKey) goto fail;
    out = (unsigned char *)malloc(data_len + 1);
    if (!out) goto fail;
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = iv;
    auth.cbNonce = (ULONG)iv_len;
    auth.pbAuthData = aad;
    auth.cbAuthData = (ULONG)aad_len;
    auth.pbTag = tag;
    auth.cbTag = sizeof(tag);
    if (!BCRYPT_SUCCESS(BCryptEncrypt(hKey, data, (ULONG)data_len, &auth, NULL, 0, out, (ULONG)data_len, &done, 0))) goto fail;
    data_hex = bytes_hex(out, done);
    tag_hex = bytes_hex(tag, sizeof(tag));
    if (!data_hex || !tag_hex) goto fail;
    append(&json, &cap, &len, "{\"data\":\"", 9);
    append(&json, &cap, &len, data_hex, strlen(data_hex));
    append(&json, &cap, &len, "\",\"tag\":\"", 9);
    append(&json, &cap, &len, tag_hex, strlen(tag_hex));
    append(&json, &cap, &len, "\"}", 2);
    result = js_str(ctx, json ? json : "{}", len);
    free(json);
    free(data_hex);
    free(tag_hex);
    free(out);
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    free(key); free(iv); free(aad); free(data);
    return result;
fail:
    free(json); free(data_hex); free(tag_hex); free(out);
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    free(key); free(iv); free(aad); free(data);
    return js_error(ctx, "aes-gcm encrypt failed");
}

static MBJSValue aes_gcm_decrypt(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t key_len = 0; size_t iv_len = 0; size_t aad_len = 0; size_t data_len = 0; size_t tag_len = 0;
    unsigned char *key; unsigned char *iv; unsigned char *aad; unsigned char *data; unsigned char *tag;
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    unsigned char *out = NULL;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    ULONG done = 0;
    MBJSValue result;
    (void)this_val;
    if (argc < 5) return js_error(ctx, "aesGcmDecrypt needs key, iv, aad, data, tag");
    key = hex_bytes(ctx, argv[0], &key_len);
    iv = hex_bytes(ctx, argv[1], &iv_len);
    aad = hex_bytes(ctx, argv[2], &aad_len);
    data = hex_bytes(ctx, argv[3], &data_len);
    tag = hex_bytes(ctx, argv[4], &tag_len);
    if (!key || !iv || !aad || !data || !tag) goto fail;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0))) goto fail;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, (ULONG)((wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t)), 0))) goto fail;
    hKey = import_aes_key(hAlg, key, (ULONG)key_len);
    if (!hKey) goto fail;
    out = (unsigned char *)malloc(data_len + 1);
    if (!out) goto fail;
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = iv;
    auth.cbNonce = (ULONG)iv_len;
    auth.pbAuthData = aad;
    auth.cbAuthData = (ULONG)aad_len;
    auth.pbTag = tag;
    auth.cbTag = (ULONG)tag_len;
    if (!BCRYPT_SUCCESS(BCryptDecrypt(hKey, data, (ULONG)data_len, &auth, NULL, 0, out, (ULONG)data_len, &done, 0))) {
        result = js_str(ctx, "", 0);
        goto cleanup;
    }
    result = hex_result(ctx, out, done);
cleanup:
    free(out);
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    free(key); free(iv); free(aad); free(data); free(tag);
    return result;
fail:
    free(out);
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    free(key); free(iv); free(aad); free(data); free(tag);
    return js_error(ctx, "aes-gcm decrypt failed");
}

static MBJSValue aes_cbc_crypt(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv, int decrypt)
{
    size_t key_len = 0; size_t iv_len = 0; size_t data_len = 0;
    unsigned char *key; unsigned char *iv; unsigned char *data;
    unsigned char *input = NULL;
    unsigned char *out = NULL;
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    ULONG done = 0;
    ULONG output_len = 0;
    MBJSValue result;
    (void)this_val;
    if (argc < 3) return js_error(ctx, "aesCrypt needs key, iv, data");
    key = hex_bytes(ctx, argv[0], &key_len);
    iv = hex_bytes(ctx, argv[1], &iv_len);
    data = hex_bytes(ctx, argv[2], &data_len);
    if (!key || !iv || !data) goto fail;
    if (iv_len < 16) goto fail;
    if (!decrypt) {
        int pad = 16 - (int)(data_len % 16);
        size_t padded_len = data_len + (size_t)pad;
        input = (unsigned char *)malloc(padded_len);
        if (!input) goto fail;
        memcpy(input, data, data_len);
        memset(input + data_len, pad, (size_t)pad);
        output_len = (ULONG)padded_len;
    } else {
        input = data;
        output_len = (ULONG)data_len;
        if (output_len % 16 != 0) goto fail;
    }
    out = (unsigned char *)malloc(output_len + 1);
    if (!out) goto fail;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0))) goto fail;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, (ULONG)((wcslen(BCRYPT_CHAIN_MODE_CBC) + 1) * sizeof(wchar_t)), 0))) goto fail;
    hKey = import_aes_key(hAlg, key, (ULONG)key_len);
    if (!hKey) goto fail;
    if (decrypt) {
        NTSTATUS status = BCryptDecrypt(hKey, input, output_len, NULL, iv, 16, out, output_len, &done, 0);
        if (!BCRYPT_SUCCESS(status)) goto fail;
    } else {
        NTSTATUS status = BCryptEncrypt(hKey, input, output_len, NULL, iv, 16, out, output_len, &done, 0);
        if (!BCRYPT_SUCCESS(status)) goto fail;
    }
    if (decrypt && done > 0) {
        unsigned char pad = out[done - 1];
        if (pad > 0 && pad <= 16 && (size_t)pad <= done) done -= pad;
    }
    result = hex_result(ctx, out, done);
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    free(out);
    if (!decrypt) free(input);
    free(key); free(iv); free(data);
    return result;
fail:
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    free(out);
    if (!decrypt) free(input);
    free(key); free(iv); free(data);
    return js_error(ctx, "aes-cbc failed");
}

static MBJSValue crypto_aes_ctr_crypt(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t key_len = 0; size_t iv_len = 0; size_t data_len = 0;
    unsigned char *key; unsigned char *iv; unsigned char *data;
    unsigned char counter[16];
    unsigned char zero[16];
    unsigned char stream[16];
    unsigned char *out = NULL;
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    ULONG done = 0;
    size_t pos;
    MBJSValue result;
    (void)this_val;
    if (argc < 3) return js_error(ctx, "aesCtrCrypt needs key, iv, data");
    key = hex_bytes(ctx, argv[0], &key_len);
    iv = hex_bytes(ctx, argv[1], &iv_len);
    data = hex_bytes(ctx, argv[2], &data_len);
    if (!key || !iv || !data) goto fail;
    if (iv_len < 16) goto fail;
    out = (unsigned char *)malloc(data_len + 1);
    if (!out) goto fail;
    memset(zero, 0, sizeof(zero));
    memcpy(counter, iv, 16);
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0))) goto fail;
    if (!BCRYPT_SUCCESS(BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, (ULONG)((wcslen(BCRYPT_CHAIN_MODE_CBC) + 1) * sizeof(wchar_t)), 0))) goto fail;
    hKey = import_aes_key(hAlg, key, (ULONG)key_len);
    if (!hKey) goto fail;
    for (pos = 0; pos < data_len; pos += 16) {
        size_t take = data_len - pos;
        size_t i;
        if (take > 16) take = 16;
        if (!BCRYPT_SUCCESS(BCryptEncrypt(hKey, counter, 16, NULL, zero, 16, stream, 16, &done, 0))) goto fail;
        for (i = 0; i < take; i++) out[pos + i] = data[pos + i] ^ stream[i];
        for (i = 16; i > 0; i--) {
            counter[i - 1]++;
            if (counter[i - 1] != 0) break;
        }
    }
    result = hex_result(ctx, out, data_len);
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    free(out);
    free(key); free(iv); free(data);
    return result;
fail:
    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    free(out);
    free(key); free(iv); free(data);
    return js_error(ctx, "aes-ctr failed");
}

static MBJSValue crypto_aes_cbc_encrypt(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    return aes_cbc_crypt(ctx, this_val, argc, argv, 0);
}

static MBJSValue crypto_aes_cbc_decrypt(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    return aes_cbc_crypt(ctx, this_val, argc, argv, 1);
}

static MBJSValue crypto_timing_safe_equal(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    size_t a_len = 0; size_t b_len = 0;
    unsigned char *a;
    unsigned char *b;
    int equal = 1;
    size_t i;
    (void)this_val;
    if (argc < 2) return js_error(ctx, "timingSafeEqual needs two values");
    a = hex_bytes(ctx, argv[0], &a_len);
    b = hex_bytes(ctx, argv[1], &b_len);
    if (!a || !b) { free(a); free(b); return MBJS_EXCEPTION; }
    if (a_len != b_len) equal = 0;
    for (i = 0; i < a_len || i < b_len; i++) {
        unsigned char x = i < a_len ? a[i] : 0;
        unsigned char y = i < b_len ? b[i] : 0;
        if (x != y) equal = 0;
    }
    free(a);
    free(b);
    return MBJS_NewInt32(ctx, equal);
}

static int crypto_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("randomHex", 1, crypto_random),
        MBJS_CFUNC_DEF("md5Hex", 1, crypto_md5),
        MBJS_CFUNC_DEF("sha1Hex", 1, crypto_sha1),
        MBJS_CFUNC_DEF("sha256Hex", 1, crypto_sha256),
        MBJS_CFUNC_DEF("sha384Hex", 1, crypto_sha384),
        MBJS_CFUNC_DEF("sha512Hex", 1, crypto_sha512),
        MBJS_CFUNC_DEF("hmacSha1Hex", 2, crypto_hmac_sha1),
        MBJS_CFUNC_DEF("hmacSha256Hex", 2, crypto_hmac_sha256),
        MBJS_CFUNC_DEF("hmacSha512Hex", 2, crypto_hmac_sha512),
        MBJS_CFUNC_DEF("pbkdf2Hex", 5, crypto_pbkdf2),
        MBJS_CFUNC_DEF("aesGcmEncryptHex", 4, aes_gcm_encrypt),
        MBJS_CFUNC_DEF("aesGcmDecryptHex", 5, aes_gcm_decrypt),
        MBJS_CFUNC_DEF("aesCtrCryptHex", 3, crypto_aes_ctr_crypt),
        MBJS_CFUNC_DEF("aesCbcEncryptHex", 3, crypto_aes_cbc_encrypt),
        MBJS_CFUNC_DEF("aesCbcDecryptHex", 3, crypto_aes_cbc_decrypt),
        MBJS_CFUNC_DEF("timingSafeEqual", 2, crypto_timing_safe_equal),
    };
    return rt.SetModuleExportList(ctx, m, tab, (int)(sizeof(tab) / sizeof(tab[0])));
}

MBJS_MODULE_EXTERN MBJSModuleDef *mbjs_init_module(MBJSContext *ctx, const char *name)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("randomHex", 1, crypto_random),
        MBJS_CFUNC_DEF("md5Hex", 1, crypto_md5),
        MBJS_CFUNC_DEF("sha1Hex", 1, crypto_sha1),
        MBJS_CFUNC_DEF("sha256Hex", 1, crypto_sha256),
        MBJS_CFUNC_DEF("sha384Hex", 1, crypto_sha384),
        MBJS_CFUNC_DEF("sha512Hex", 1, crypto_sha512),
        MBJS_CFUNC_DEF("hmacSha1Hex", 2, crypto_hmac_sha1),
        MBJS_CFUNC_DEF("hmacSha256Hex", 2, crypto_hmac_sha256),
        MBJS_CFUNC_DEF("hmacSha512Hex", 2, crypto_hmac_sha512),
        MBJS_CFUNC_DEF("pbkdf2Hex", 5, crypto_pbkdf2),
        MBJS_CFUNC_DEF("aesGcmEncryptHex", 4, aes_gcm_encrypt),
        MBJS_CFUNC_DEF("aesGcmDecryptHex", 5, aes_gcm_decrypt),
        MBJS_CFUNC_DEF("aesCtrCryptHex", 3, crypto_aes_ctr_crypt),
        MBJS_CFUNC_DEF("aesCbcEncryptHex", 3, crypto_aes_cbc_encrypt),
        MBJS_CFUNC_DEF("aesCbcDecryptHex", 3, crypto_aes_cbc_decrypt),
        MBJS_CFUNC_DEF("timingSafeEqual", 2, crypto_timing_safe_equal),
    };
    const char *missing = mbjs_runtime_resolve(&rt);
    MBJSModuleDef *m;
    if (missing) return NULL;
    m = rt.NewCModule(ctx, name ? name : "mochicrypto", crypto_mod_init);
    if (!m) return NULL;
    if (rt.AddModuleExportList(ctx, m, tab, (int)(sizeof(tab) / sizeof(tab[0]))) != 0) return NULL;
    return m;
}
