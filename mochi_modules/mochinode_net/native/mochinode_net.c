/*
 * mochinode_net.c — minimal blocking TCP/HTTP server for the SEVM stack.
 *
 * Purpose: give the MochiBerry engine a real networking primitive. The
 * engine's own built-ins (mbjs:os) expose setReadHandler/setWriteHandler but
 * NO socket primitives, so a pure-JS HTTP server is impossible. This module
 * wraps Winsock2 and hands the JS layer an accept/read/write/close API. The
 * JS side (the mochinode shim) builds the request/response framing on top.
 *
 * Model is deliberately blocking and single-threaded: one request at a time,
 * driven by a JS loop calling httpAccept(). This keeps the engine's GC and
 * atom table single-owner (see mbjs_runtime.h) with no cross-thread
 * callbacks.
 */
#include "mbjs_runtime.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static MBJSRuntime rt;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int get_fd(MBJSValueConst v, SOCKET *out)
{
    double d = 0;
    if (!rt.ready)
        return -1;
    if (rt.ToFloat64(NULL, &d, v) < 0)
        return -1;
    *out = (SOCKET)(intptr_t)d;
    return 0;
}

static double fd_to_js(SOCKET s)
{
    return (double)(intptr_t)s;
}

static int wsa_init(void)
{
    static int ready = 0;
    WSADATA wsa;
    if (ready)
        return 0;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return -1;
    ready = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* module functions                                                    */
/* ------------------------------------------------------------------ */

/* httpServer(port) -> fd (listen socket), or -1 on error */
static MBJSValue net_httpServer(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    SOCKET s;
    int port;
    double d = 0;
    struct sockaddr_in addr;

    (void)this_val;
    if (argc < 1)
        return MBJS_NewInt32(ctx, -1);
    if (rt.ToFloat64(ctx, &d, argv[0]) < 0)
        return MBJS_NewInt32(ctx, -1);
    port = (int)d;
    if (wsa_init() < 0)
        return MBJS_NewInt32(ctx, -1);

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return MBJS_NewInt32(ctx, -1);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u_short)port);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(s, 64) == SOCKET_ERROR) {
        closesocket(s);
        return MBJS_NewInt32(ctx, -1);
    }
    /* Non-blocking accept so the JS serve loop can yield to the engine
       (and the remote debugger) instead of monopolising the thread. */
    {
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
    }
    return MBJS_NewFloat64(ctx, fd_to_js(s));
}

/* httpAccept(srvfd) -> connfd (blocks until a connection), or -1 */
static MBJSValue net_httpAccept(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    SOCKET srv, conn;
    (void)this_val;
    if (argc < 1 || get_fd(argv[0], &srv) < 0)
        return MBJS_NewInt32(ctx, -1);
    conn = accept(srv, NULL, NULL);
    if (conn == INVALID_SOCKET) {
        /* non-blocking: no pending connection yet -> retry later */
        return MBJS_NewInt32(ctx, -1);
    }
    return MBJS_NewFloat64(ctx, fd_to_js(conn));
}

/* httpRead(fd, max) -> string (up to max bytes), or undefined on EOF/err */
static MBJSValue net_httpRead(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    SOCKET s;
    int cap = 4096;
    char *buf;
    int n;

    (void)this_val;
    if (argc < 1 || get_fd(argv[0], &s) < 0)
        return MBJS_UNDEFINED;
    if (argc >= 2) {
        double d = 0;
        if (rt.ToFloat64(ctx, &d, argv[1]) == 0 && d > 1)
            cap = (int)d;
    }

    buf = (char *)malloc((size_t)cap + 1);
    if (!buf)
        return MBJS_UNDEFINED;
    n = recv(s, buf, cap, 0);
    if (n <= 0) {
        free(buf);
        return MBJS_UNDEFINED;
    }
    {
        MBJSValue v = rt.NewStringLen(ctx, buf, (size_t)n);
        free(buf);
        return v;
    }
}

/* httpWrite(fd, str) -> bytes sent, or -1 on error */
static MBJSValue net_httpWrite(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    SOCKET s;
    const char *data;
    size_t len, sent = 0;
    int rc;

    (void)this_val;
    if (argc < 2 || get_fd(argv[0], &s) < 0)
        return MBJS_NewInt32(ctx, -1);

    data = rt.ToCStringLen(ctx, &len, argv[1]);
    if (!data)
        return MBJS_NewInt32(ctx, -1);

    while (sent < len) {
        rc = send(s, data + sent, (int)(len - sent), 0);
        if (rc <= 0) {
            rt.FreeCString(ctx, data);
            return MBJS_NewInt32(ctx, (int)sent);
        }
        sent += (size_t)rc;
    }
    rt.FreeCString(ctx, data);
    return MBJS_NewFloat64(ctx, (double)sent);
}

/* httpClose(fd) */
static MBJSValue net_httpClose(MBJSContext *ctx, MBJSValueConst this_val,
                               int argc, MBJSValueConst *argv)
{
    SOCKET s;
    (void)ctx;
    (void)this_val;
    if (argc >= 1 && get_fd(argv[0], &s) == 0)
        closesocket(s);
    return MBJS_UNDEFINED;
}

/* tcpConnect(host, port) -> fd (connected client socket), or -1 on error.
 * Non-blocking connect + select with a 5s timeout, so the engine is never
 * frozen by an unreachable host. The returned fd is usable with
 * httpRead/httpWrite/httpClose. */
static MBJSValue net_tcpConnect(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    SOCKET s;
    struct sockaddr_in addr;
    const char *host;
    size_t hostLen;
    int port, err;
    double d = 0;
    u_long nb = 1;
    fd_set wfds;
    struct timeval tv;

    (void)this_val;
    if (argc < 2)
        return MBJS_NewInt32(ctx, -1);
    host = rt.ToCStringLen(ctx, &hostLen, argv[0]);
    if (!host || hostLen == 0) {
        rt.FreeCString(ctx, host);
        return MBJS_NewInt32(ctx, -1);
    }
    if (rt.ToFloat64(ctx, &d, argv[1]) < 0) {
        rt.FreeCString(ctx, host);
        return MBJS_NewInt32(ctx, -1);
    }
    port = (int)d;
    if (wsa_init() < 0) {
        rt.FreeCString(ctx, host);
        return MBJS_NewInt32(ctx, -1);
    }

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        rt.FreeCString(ctx, host);
        return MBJS_NewInt32(ctx, -1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr || he->h_length != 4) {
            closesocket(s);
            rt.FreeCString(ctx, host);
            return MBJS_NewInt32(ctx, -1);
        }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }
    rt.FreeCString(ctx, host);

    /* non-blocking connect; wait for completion with a bounded select */
    ioctlsocket(s, FIONBIO, &nb);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS && err != WSAEINVAL) {
            closesocket(s);
            return MBJS_NewInt32(ctx, -1);
        }
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        if (select(0, NULL, &wfds, NULL, &tv) <= 0) {
            closesocket(s);
            return MBJS_NewInt32(ctx, -1);
        }
        {
            int soerr = 0, slen = (int)sizeof(soerr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &slen);
            if (soerr != 0) {
                closesocket(s);
                return MBJS_NewInt32(ctx, -1);
            }
        }
    }
    /* leave the socket non-blocking; httpRead/Write handle partial results */
    return MBJS_NewFloat64(ctx, fd_to_js(s));
}

/* ------------------------------------------------------------------ */
/* module plumbing                                                     */
/* ------------------------------------------------------------------ */

static int net_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("httpServer", 1, net_httpServer),
        MBJS_CFUNC_DEF("httpAccept", 1, net_httpAccept),
        MBJS_CFUNC_DEF("httpRead", 2, net_httpRead),
        MBJS_CFUNC_DEF("httpWrite", 2, net_httpWrite),
        MBJS_CFUNC_DEF("httpClose", 1, net_httpClose),
        MBJS_CFUNC_DEF("tcpConnect", 2, net_tcpConnect),
    };
    return rt.SetModuleExportList(ctx, m, tab, 6);
}

MBJS_MODULE_EXTERN MBJSModuleDef *mbjs_init_module(MBJSContext *ctx,
                                                    const char *name)
{
    const MBJSCFunctionListEntry tab[] = {
        MBJS_CFUNC_DEF("httpServer", 1, net_httpServer),
        MBJS_CFUNC_DEF("httpAccept", 1, net_httpAccept),
        MBJS_CFUNC_DEF("httpRead", 2, net_httpRead),
        MBJS_CFUNC_DEF("httpWrite", 2, net_httpWrite),
        MBJS_CFUNC_DEF("httpClose", 1, net_httpClose),
        MBJS_CFUNC_DEF("tcpConnect", 2, net_tcpConnect),
    };
    const char *missing = mbjs_runtime_resolve(&rt);
    MBJSModuleDef *m;

    if (missing)
        return NULL;
    m = rt.NewCModule(ctx, name ? name : "mochinode_net", net_mod_init);
    if (!m)
        return NULL;
    if (rt.AddModuleExportList(ctx, m, tab, 6) != 0)
        return NULL;
    return m;
}
