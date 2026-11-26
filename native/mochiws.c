#define WIN32_LEAN_AND_MEAN
#include "mbjs_runtime.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#pragma comment(lib, "winhttp.lib")

static MBJSRuntime rt;

typedef struct MsgNode {
    char *json;
    struct MsgNode *next;
} MsgNode;

typedef struct WsConn {
    HINTERNET session;
    HINTERNET connect;
    HINTERNET request;
    HINTERNET websocket;
    HANDLE thread;
    CRITICAL_SECTION lock;
    MsgNode *head;
    MsgNode *tail;
    volatile LONG closed;
    volatile LONG refcount;
    char *url;
} WsConn;

static int mochiws_dbg(void){ const char *e = getenv("MOCHIWS_DEBUG"); return e && *e && strcmp(e,"0")!=0 && strcmp(e,"false")!=0; }

static char *js_cstr_len(MBJSContext *ctx, MBJSValueConst v, size_t *out_len)
{
    size_t len = 0;
    const char *p = rt.ToCStringLen(ctx, &len, v);
    char *out;
    if (!p) return NULL;
    out = (char *)malloc(len + 1);
    if (!out) { rt.FreeCString(ctx, p); return NULL; }
    memcpy(out, p, len);
    out[len] = 0;
    rt.FreeCString(ctx, p);
    if (out_len) *out_len = len;
    return out;
}

static MBJSValue js_str(MBJSContext *ctx, const char *s, size_t len)
{
    MBJSValue v = rt.NewStringLen(ctx, s ? s : "", s ? len : 0);
    if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] js_str('%s', %zu) -> tag=%d ptr=%p\n",
            s ? s : "(null)", len, MBJS_VALUE_GET_TAG(v), MBJS_VALUE_GET_PTR(v));
    return v;
}

static MBJSValue js_error(MBJSContext *ctx, const char *what)
{
    if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] js_error('%s')\n", what);
    rt.ThrowTypeError(ctx, "mochiws: %s", what);
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

static unsigned char *hex_bytes(const char *text, size_t *out_len)
{
    size_t len = strlen(text);
    size_t i;
    unsigned char *out;
    if (len % 2 != 0) return NULL;
    out = (unsigned char *)malloc(len / 2 + 1);
    if (!out) return NULL;
    for (i = 0; i < len / 2; i++) {
        int hi = hexval(text[i * 2]);
        int lo = hexval(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) { free(out); return NULL; }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
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

static char *json_escape(const char *s)
{
    size_t cap = strlen(s) * 2 + 3;
    size_t len = 0;
    char *out = (char *)malloc(cap);
    size_t i;
    if (!out) return NULL;
    out[len++] = '"';
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        char buf[8];
        size_t n = 0;
        if (c == '"' || c == '\\') { buf[0] = '\\'; buf[1] = (char)c; n = 2; }
        else if (c == '\n') { buf[0] = '\\'; buf[1] = 'n'; n = 2; }
        else if (c == '\r') { buf[0] = '\\'; buf[1] = 'r'; n = 2; }
        else if (c == '\t') { buf[0] = '\\'; buf[1] = 't'; n = 2; }
        else if (c < 0x20) { sprintf(buf, "\\u%04x", c); n = 6; }
        else { buf[0] = (char)c; n = 1; }
        if (len + n + 2 > cap) {
            char *tmp;
            cap *= 2;
            tmp = (char *)realloc(out, cap);
            if (!tmp) { free(out); return NULL; }
            out = tmp;
        }
        memcpy(out + len, buf, n);
        len += n;
    }
    out[len++] = '"';
    out[len] = 0;
    return out;
}

static void queue_push(WsConn *c, char *json)
{
    MsgNode *node = (MsgNode *)malloc(sizeof(MsgNode));
    if (!node) { free(json); return; }
    node->json = json;
    node->next = NULL;
    EnterCriticalSection(&c->lock);
    if (c->tail) c->tail->next = node;
    else c->head = node;
    c->tail = node;
    LeaveCriticalSection(&c->lock);
}

static void queue_event(WsConn *c, const char *type, const char *dataHex, const char *error)
{
    char *json;
    char *errJson = NULL;
    size_t cap;
    if (error) errJson = json_escape(error);
    cap = 128 + (dataHex ? strlen(dataHex) : 0) + (errJson ? strlen(errJson) : 0);
    json = (char *)malloc(cap);
    if (!json) { free(errJson); return; }
    if (dataHex && errJson)
        sprintf(json, "{\"type\":\"%s\",\"data\":\"%s\",\"error\":%s}", type, dataHex, errJson);
    else if (dataHex)
        sprintf(json, "{\"type\":\"%s\",\"data\":\"%s\"}", type, dataHex);
    else if (errJson)
        sprintf(json, "{\"type\":\"%s\",\"error\":%s}", type, errJson);
    else
        sprintf(json, "{\"type\":\"%s\"}", type);
    free(errJson);
    queue_push(c, json);
}

static wchar_t *utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void conn_release(WsConn *c)
{
    if (InterlockedDecrement(&c->refcount) == 0) {
        MsgNode *node = c->head;
        while (node) {
            MsgNode *next = node->next;
            free(node->json);
            free(node);
            node = next;
        }
        DeleteCriticalSection(&c->lock);
        free(c->url);
        free(c);
    }
}

static DWORD WINAPI recv_thread(LPVOID param)
{
    WsConn *c = (WsConn *)param;
    unsigned char buf[16384];
    DWORD read = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
    DWORD err;

    for (;;) {
        unsigned char *msg = NULL;
        size_t msg_len = 0;
        size_t msg_cap = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE msg_type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        int done = 0;

        if (InterlockedCompareExchange(&c->closed, 0, 0)) break;
        do {
            read = 0;
            type = 0;
            err = WinHttpWebSocketReceive(c->websocket, buf, sizeof(buf), &read, &type);
            if (err != ERROR_SUCCESS) {
                if (!InterlockedCompareExchange(&c->closed, 0, 0))
                    queue_event(c, "error", NULL, "websocket receive failed");
                done = -1;
                break;
            }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                queue_event(c, "close", NULL, NULL);
                InterlockedExchange(&c->closed, 1);
                done = -1;
                break;
            }
            if (msg_len + read + 1 > msg_cap) {
                size_t next = msg_cap ? msg_cap * 2 : read + 1;
                unsigned char *tmp;
                while (next < msg_len + read + 1) next *= 2;
                tmp = (unsigned char *)realloc(msg, next);
                if (!tmp) { free(msg); msg = NULL; done = -1; break; }
                msg = tmp;
                msg_cap = next;
            }
            memcpy(msg + msg_len, buf, read);
            msg_len += read;
            msg_type = type;
        } while (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE);

        if (done == -1) { free(msg); break; }
        if (msg && msg_len > 0) {
            char *hex = bytes_hex(msg, msg_len);
            const char *ev = (msg_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || msg_type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) ? "text" : "message";
            if (hex) queue_event(c, ev, hex, NULL);
            free(hex);
        }
        free(msg);
    }

    conn_release(c);
    return 0;
}

static int parse_url_parts(const char *url, wchar_t *host, DWORD hostCap, wchar_t *pathAndExtra, DWORD pathCap, INTERNET_PORT *port, BOOL *secure)
{
    const char *p = url;
    const char *hostStart;
    const char *hostEnd;
    const char *pathStart;
    size_t hostLen;
    char hostUtf8[256];
    char pathUtf8[4096];
    int n;

    *secure = FALSE;
    *port = INTERNET_DEFAULT_HTTP_PORT;
    if (strncmp(p, "wss://", 6) == 0) { *secure = TRUE; *port = INTERNET_DEFAULT_HTTPS_PORT; p += 6; }
    else if (strncmp(p, "ws://", 5) == 0) { *secure = FALSE; *port = INTERNET_DEFAULT_HTTP_PORT; p += 5; }
    else if (strncmp(p, "https://", 8) == 0) { *secure = TRUE; *port = INTERNET_DEFAULT_HTTPS_PORT; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { *secure = FALSE; *port = INTERNET_DEFAULT_HTTP_PORT; p += 7; }
    else return 0;

    hostStart = p;
    while (*p && *p != '/' && *p != '?' && *p != '#' && *p != ':') p++;
    hostEnd = p;
    hostLen = (size_t)(hostEnd - hostStart);
    if (hostLen == 0 || hostLen >= sizeof(hostUtf8)) return 0;
    memcpy(hostUtf8, hostStart, hostLen);
    hostUtf8[hostLen] = 0;

    if (*p == ':') {
        char portBuf[16];
        size_t i = 0;
        p++;
        while (*p >= '0' && *p <= '9' && i < sizeof(portBuf) - 1) portBuf[i++] = *p++;
        portBuf[i] = 0;
        if (i > 0) *port = (INTERNET_PORT)atoi(portBuf);
    }

    pathStart = (*p == '/') ? p : "/";
    if (strlen(pathStart) >= sizeof(pathUtf8)) return 0;
    strcpy(pathUtf8, pathStart);

    n = MultiByteToWideChar(CP_UTF8, 0, hostUtf8, -1, host, (int)hostCap);
    if (n <= 0) return 0;
    n = MultiByteToWideChar(CP_UTF8, 0, pathUtf8, -1, pathAndExtra, (int)pathCap);
    if (n <= 0) return 0;
    return 1;
}

static MBJSValue ws_connect(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    char *url;
    wchar_t host[256] = {0};
    wchar_t pathAndExtra[4096] = {0};
    WsConn *c = NULL;
    INTERNET_PORT port;
    BOOL secure;
    (void)this_val;

    if (argc < 1) return js_error(ctx, "connect needs url");
    url = js_cstr_len(ctx, argv[0], NULL);
    if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] ws_connect argc=%d url='%s'\n", argc, url ? url : "(null)");
    if (!url) return js_error(ctx, "bad url");

    if (!parse_url_parts(url, host, 256, pathAndExtra, 4096, &port, &secure)) {
        if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] parse_url_parts FAILED\n");
        free(url);
        return js_error(ctx, "invalid websocket url");
    }
    if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] parsed host='%ls' port=%u secure=%d path='%ls'\n",
            host, (unsigned)port, (int)secure, pathAndExtra);

    c = (WsConn *)calloc(1, sizeof(WsConn));
    if (!c) { free(url); return js_error(ctx, "out of memory"); }
    c->url = url;
    c->refcount = 2; /* main + recv thread */
    InitializeCriticalSection(&c->lock);

    c->session = WinHttpOpen(L"MochiWA/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c->session) goto fail;
    c->connect = WinHttpConnect(c->session, host, port, 0);
    if (!c->connect) goto fail;
    {
        DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
        c->request = WinHttpOpenRequest(c->connect, L"GET", pathAndExtra, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    }
    if (!c->request) goto fail;
    if (!WinHttpSetOption(c->request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] step: SET_UPGRADE_TO_WS FAILED err=%lu\n", (unsigned long)GetLastError());
        goto fail;
    }
    /* WhatsApp WAF rejects the bare WinHTTP upgrade (404): it expects a
       browser-like handshake with Origin + modern UA (same headers the
       `ws` library sends in Baileys). */
    {
        const wchar_t *hdrs =
            L"Origin: https://web.whatsapp.com\r\n"
            L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            L"(KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36\r\n"
            L"Cache-Control: no-cache\r\n";
        if (!WinHttpAddRequestHeaders(c->request, hdrs, (DWORD)-1,
                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] step: ADD_HEADERS FAILED err=%lu\n", (unsigned long)GetLastError());
        }
    }
    if (!WinHttpSendRequest(c->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] step: SEND_REQUEST FAILED err=%lu\n", (unsigned long)GetLastError());
        goto fail;
    }
    if (!WinHttpReceiveResponse(c->request, NULL)) {
        if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] step: RECEIVE_RESPONSE FAILED err=%lu\n", (unsigned long)GetLastError());
        goto fail;
    }
    {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(c->request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] HTTP status=%lu\n", (unsigned long)status);
        /* dump response headers */
        {
            wchar_t hbuf[1024]; DWORD hsz = sizeof(hbuf);
            if (WinHttpQueryHeaders(c->request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                    WINHTTP_HEADER_NAME_BY_INDEX, hbuf, &hsz, WINHTTP_NO_HEADER_INDEX)) {
                if (mochiws_dbg()) fwprintf(stderr, L"[mochiws-dbg] RESP-HDRS: %ls\n", hbuf);
            }
        }
        /* dump a slice of the response body so we can see WHY the server rejected */
        DWORD avail = 0;
        if (WinHttpQueryDataAvailable(c->request, &avail) && avail > 0) {
            char buf[512];
            DWORD got = 0;
            if (WinHttpReadData(c->request, buf, sizeof(buf) - 1, &got) && got > 0) {
                buf[got] = 0;
                if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] BODY[%lu]: %s\n", (unsigned long)got, buf);
            }
        }
    }
    c->websocket = WinHttpWebSocketCompleteUpgrade(c->request, 0);
    if (!c->websocket) {
        if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] step: COMPLETE_UPGRADE FAILED err=%lu\n", (unsigned long)GetLastError());
        goto fail;
    }

    c->thread = CreateThread(NULL, 0, recv_thread, c, 0, NULL);
    if (!c->thread) goto fail;

    queue_event(c, "open", NULL, NULL);
    {
        char idbuf[64];
        sprintf(idbuf, "%llu", (unsigned long long)(uintptr_t)c);
        if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] WS_CONNECT OK c=%p -> handle='%s' (thread=%p)\n",
                (void *)c, idbuf, (void *)c->thread);
        return js_str(ctx, idbuf, strlen(idbuf));
    }

fail:
    {
        DWORD e = GetLastError();
        char msg[128];
        sprintf(msg, "connect failed (%lu)", (unsigned long)e);
        if (mochiws_dbg()) fprintf(stderr, "[mochiws-dbg] WS_CONNECT FAIL: %s (c=%p ws=%p req=%p)\n",
                msg, (void *)c, (void *)(c ? c->websocket : NULL),
                (void *)(c ? c->request : NULL));
        if (c->websocket) WinHttpCloseHandle(c->websocket);
        if (c->request) WinHttpCloseHandle(c->request);
        if (c->connect) WinHttpCloseHandle(c->connect);
        if (c->session) WinHttpCloseHandle(c->session);
        DeleteCriticalSection(&c->lock);
        free(c->url);
        free(c);
        return js_error(ctx, msg);
    }
}

static WsConn *conn_from_arg(MBJSContext *ctx, MBJSValueConst v)
{
    char *s = js_cstr_len(ctx, v, NULL);
    unsigned long long id = 0;
    WsConn *c;
    if (!s) return NULL;
    id = strtoull(s, NULL, 10);
    free(s);
    c = (WsConn *)(uintptr_t)id;
    return c;
}

static MBJSValue ws_send(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    WsConn *c;
    char *hex;
    size_t len = 0;
    unsigned char *data;
    DWORD err;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
    (void)this_val;
    if (argc < 2) return js_error(ctx, "send needs handle and hex");
    c = conn_from_arg(ctx, argv[0]);
    if (!c) return js_error(ctx, "bad handle");
    if (InterlockedCompareExchange(&c->closed, 0, 0)) return js_error(ctx, "socket closed");
    hex = js_cstr_len(ctx, argv[1], NULL);
    if (!hex) return js_error(ctx, "bad hex");
    data = hex_bytes(hex, &len);
    free(hex);
    if (!data) return js_error(ctx, "invalid hex");
    if (argc > 2) {
        double t = js_number(ctx, argv[2]);
        if ((int)t == 1) type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
    }
    err = WinHttpWebSocketSend(c->websocket, type, data, (DWORD)len);
    free(data);
    if (err != ERROR_SUCCESS) return js_error(ctx, "send failed");
    return MBJS_NewInt32(ctx, 0);
}

static MBJSValue ws_poll(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    WsConn *c;
    MsgNode *head = NULL, *node;
    size_t cap = 2, len = 1;
    char *out;
    MBJSValue ret;
    (void)this_val;
    if (argc < 1) return js_error(ctx, "poll needs handle");
    c = conn_from_arg(ctx, argv[0]);
    if (!c) return js_error(ctx, "bad handle");

    EnterCriticalSection(&c->lock);
    head = c->head;
    c->head = c->tail = NULL;
    LeaveCriticalSection(&c->lock);

    out = (char *)malloc(cap);
    if (!out) return js_error(ctx, "out of memory");
    out[0] = '[';
    out[1] = 0;
    node = head;
    while (node) {
        MsgNode *next = node->next;
        size_t n = strlen(node->json);
        int needComma = (len > 1);
        while (len + n + 3 > cap) {
            char *tmp;
            cap *= 2;
            tmp = (char *)realloc(out, cap);
            if (!tmp) { free(out); free(node->json); free(node); return js_error(ctx, "out of memory"); }
            out = tmp;
        }
        if (needComma) out[len++] = ',';
        memcpy(out + len, node->json, n);
        len += n;
        out[len] = 0;
        free(node->json);
        free(node);
        node = next;
    }
    if (len + 2 > cap) {
        char *tmp = (char *)realloc(out, len + 2);
        if (!tmp) { free(out); return js_error(ctx, "out of memory"); }
        out = tmp;
    }
    out[len++] = ']';
    out[len] = 0;
    ret = js_str(ctx, out, len);
    free(out);
    return ret;
}

static MBJSValue ws_close(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    WsConn *c;
    (void)this_val;
    if (argc < 1) return js_error(ctx, "close needs handle");
    c = conn_from_arg(ctx, argv[0]);
    if (!c) return js_error(ctx, "bad handle");
    if (InterlockedExchange(&c->closed, 1) == 0) {
        if (c->websocket) WinHttpWebSocketShutdown(c->websocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
        if (c->websocket) { WinHttpCloseHandle(c->websocket); c->websocket = NULL; }
        if (c->request) { WinHttpCloseHandle(c->request); c->request = NULL; }
        if (c->connect) { WinHttpCloseHandle(c->connect); c->connect = NULL; }
        if (c->session) { WinHttpCloseHandle(c->session); c->session = NULL; }
        if (c->thread) {
            WaitForSingleObject(c->thread, 2000);
            CloseHandle(c->thread);
            c->thread = NULL;
        }
        queue_event(c, "close", NULL, NULL);
        conn_release(c);
    }
    return MBJS_NewInt32(ctx, 0);
}

static MBJSValue ws_http_get(MBJSContext *ctx, MBJSValueConst this_val, int argc, MBJSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return js_error(ctx, "httpGet needs url");
    size_t urlLen = 0;
    char *url = js_cstr_len(ctx, argv[0], &urlLen);
    if (!url || urlLen == 0) { free(url); return js_error(ctx, "httpGet needs url"); }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, url, -1, NULL, 0);
    WCHAR *wurl = wlen > 0 ? (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR)) : NULL;
    if (!wurl) { free(url); return js_error(ctx, "httpGet url conversion failed"); }
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, wlen);
    free(url);

    WCHAR scheme[32], host[512], path[8192], extra[1024];
    URL_COMPONENTSW uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszScheme = scheme; uc.dwSchemeLength = 32;
    uc.lpszHostName = host; uc.dwHostNameLength = 512;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 8192;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1024;
    scheme[0] = host[0] = path[0] = extra[0] = 0;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || !host[0]) {
        free(wurl);
        return js_error(ctx, "httpGet bad url");
    }
    free(wurl);

    BOOL secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    INTERNET_PORT port = uc.nPort ? uc.nPort : (secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
    HINTERNET session = WinHttpOpen(L"MochiWA/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = NULL, request = NULL;
    unsigned char *body = NULL;
    char *json = NULL;
    MBJSValue ret = MBJS_UNDEFINED;
    size_t bodyLen = 0, bodyCap = 0;
    DWORD status = 0;
    const char *stage = "open";
    DWORD errCode = 0;
#define HTTP_FAIL(label) do { stage = (label); errCode = GetLastError(); goto http_done; } while (0)

    if (!session) HTTP_FAIL("open");
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 30000);
    connect = WinHttpConnect(session, host, port, 0);
    if (!connect) HTTP_FAIL("connect");
    request = WinHttpOpenRequest(connect, L"GET", path[0] ? path : L"/", NULL,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) HTTP_FAIL("open_request");
    if (!WinHttpSendRequest(request, NULL, 0, NULL, 0, 0, 0)) HTTP_FAIL("send");
    if (!WinHttpReceiveResponse(request, NULL)) HTTP_FAIL("receive");

    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

    bodyCap = 4096;
    body = (unsigned char *)malloc(bodyCap);
    if (!body) HTTP_FAIL("alloc_body");
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
        if (bodyLen + avail + 1 > bodyCap) {
            while (bodyLen + avail + 1 > bodyCap) bodyCap *= 2;
            unsigned char *tmp = (unsigned char *)realloc(body, bodyCap);
            if (!tmp) HTTP_FAIL("grow_body");
            body = tmp;
        }
        DWORD got = 0;
        if (!WinHttpReadData(request, body + bodyLen, avail, &got) || got == 0) break;
        bodyLen += got;
    }
    body[bodyLen] = 0;

    size_t jsonCap = bodyLen * 2 + 256;
    json = (char *)malloc(jsonCap);
    if (!json) HTTP_FAIL("alloc_json");
    int n = snprintf(json, jsonCap,
                     "{\"status\":%lu,\"ok\":%s,\"stage\":\"ok\",\"error\":0,\"headers\":{},\"bodyHex\":\"",
                     (unsigned long)status, (status >= 200 && status < 300) ? "true" : "false");
    for (size_t i = 0; i < bodyLen && n > 0; i++) {
        n += snprintf(json + n, jsonCap - (size_t)n, "%02x", body[i]);
    }
    if (n > 0) {
        int tail = snprintf(json + n, jsonCap - (size_t)n, "\"}");
        if (tail > 0) n += tail;
    }
    ret = js_str(ctx, json, (size_t)n);

http_done:
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    free(body);
    free(json);
    if (MBJS_IsUndefined(ret)) {
        char message[128];
        snprintf(message, sizeof(message), "httpGet failed at %s (%lu)", stage, (unsigned long)errCode);
        fprintf(stderr, "[mochiws-http] %s host=%ls path=%ls port=%u secure=%d\n",
                message, host, path[0] ? path : L"/", (unsigned)port, (int)secure);
        return js_error(ctx, message);
    }
    return ret;
#undef HTTP_FAIL
}

static int ws_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("connect", 1, ws_connect),
        MBJS_CFUNC_DEF("send", 3, ws_send),
        MBJS_CFUNC_DEF("poll", 1, ws_poll),
        MBJS_CFUNC_DEF("close", 1, ws_close),
        MBJS_CFUNC_DEF("httpGet", 1, ws_http_get),
    };
    return rt.SetModuleExportList(ctx, m, tab, (int)(sizeof(tab) / sizeof(tab[0])));
}

MBJS_MODULE_EXTERN MBJSModuleDef *mbjs_init_module(MBJSContext *ctx, const char *name)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("connect", 1, ws_connect),
        MBJS_CFUNC_DEF("send", 3, ws_send),
        MBJS_CFUNC_DEF("poll", 1, ws_poll),
        MBJS_CFUNC_DEF("close", 1, ws_close),
        MBJS_CFUNC_DEF("httpGet", 1, ws_http_get),
    };
    const char *missing = mbjs_runtime_resolve(&rt);
    MBJSModuleDef *m;
    if (missing) return NULL;
    m = rt.NewCModule(ctx, name ? name : "mochiws", ws_mod_init);
    if (!m) return NULL;
    if (rt.AddModuleExportList(ctx, m, tab, (int)(sizeof(tab) / sizeof(tab[0]))) != 0) return NULL;
    return m;
}
