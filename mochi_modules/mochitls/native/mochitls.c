/*
 * mochitls.c — native Schannel TLS client module for the SEVM stack.
 *
 * WHAT THIS IS
 * ------------
 * A thin native engine plugin (mochi_modules/mochitls) that implements a real
 * TLS client using Windows Schannel (SSPI) on top of Winsock2:
 *
 *     tlsConnect(host, port)  -> handle (or -1)
 *     tlsWrite(handle, data)  -> plaintext bytes encrypted+sent (or -1)
 *     tlsRead(handle, max)    -> decrypted string (or undefined on EOF/error)
 *     tlsClose(handle)        -> shutdown + close
 *     tlsShutdown(handle)     -> send close_notify (best effort)
 *     tlsLastError([handle])  -> last error string
 *     tlsVersion(handle)      -> negotiated protocol ("TLS 1.2"/"TLS 1.3")
 *
 * The handshake runs a full Schannel client context with default credentials
 * (dwFlags = 0), so Schannel itself validates the server certificate chain
 * against the system trust store AND the hostname (pszTargetName is the host).
 * No SCH_CRED_MANUAL_CRED_VALIDATION is used -> full chain validation is ON.
 *
 * CONVENTIONS (must match AGENTS.md + module-loader-contract.md)
 * --------------------------------------------------------------
 *  - Does NOT import mbjs.exe. All MBJS_* runtime symbols are resolved from
 *    the host image at load time via native/mbjs_runtime.h.
 *  - Exported entry point is `mbjs_init_module`, registered via
 *    AddModuleExportList with a count that EXACTLY equals the table length
 *    (the sizeof/[] trick guarantees this).
 *  - No engine exceptions are thrown: failures use -1 / undefined sentinels
 *    and a readable message retrievable via tlsLastError(). This mirrors the
 *    established mochinode_net convention.
 *  - Single-threaded engine: the handshake and I/O are blocking, with socket
 *    timeouts (SO_RCVTIMEO/SO_SNDTIMEO = 10s) so nothing hangs forever.
 *
 * BUILD
 * -----
 *   cmake -S mochi_modules/mochitls -B mochi_modules/mochitls/build
 *         -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-windows-mingw.cmake
 *   cmake --build mochi_modules/mochitls/build
 *   -> mochi_modules/mochitls/lib/mochitls_x86_64.dll
 */
/* SECURITY_WIN32 must be defined before the SSPI headers so the A/W entry
 * points (AcquireCredentialsHandleA / InitializeSecurityContextA) are
 * declared. winsock2.h MUST come before windows.h (and before the engine
 * headers, which pull in windows.h via mbjs.h). */
#define SECURITY_WIN32 1
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "mbjs_runtime.h"

#include <security.h>
#include <schannel.h>
#include <sspi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static MBJSRuntime rt;

/* ------------------------------------------------------------------ */
/* TLS session state                                                   */
/* ------------------------------------------------------------------ */

#define TLS_MAX_SESSIONS 32
#define TLS_RBUF_SIZE    65536
#define TLS_CONNECT_TIMEOUT_S 10
#define TLS_IO_TIMEOUT_MS 10000

/* ISC_REQ flags: sequence/replay detection + confidentiality (encryption) +
 * allocate the output token (so we must FreeContextBuffer it). */
#define TLS_ISC_FLAGS                                                    \
    (ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |                   \
     ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY)

typedef struct TlsSession {
    int         in_use;
    SOCKET      sock;
    CredHandle  cred;
    int         cred_valid;
    CtxtHandle  ctx;
    int         have_ctx;
    int         handshake_done;
    int         closed;
    char        host[256];
    char        rbuf[TLS_RBUF_SIZE];
    DWORD       rpos;   /* consumed offset in rbuf */
    DWORD       rlen;   /* valid bytes in rbuf */
    char        err[256];
} TlsSession;

static TlsSession g_sess[TLS_MAX_SESSIONS];
static char       g_err[256];
static int        g_wsa_inited;

static void g_seterr(const char *msg)
{
    snprintf(g_err, sizeof(g_err), "%s", msg ? msg : "unknown");
}

static void tls_seterr_str(TlsSession *s, const char *msg)
{
    snprintf(s->err, sizeof(s->err), "%s", msg ? msg : "unknown");
    g_seterr(s->err);
}

static const char *sec_name(SECURITY_STATUS ss)
{
    switch ((DWORD)ss) {
    case SEC_E_OK:                    return "SEC_E_OK";
    case SEC_E_INCOMPLETE_MESSAGE:    return "SEC_E_INCOMPLETE_MESSAGE";
    case SEC_I_CONTINUE_NEEDED:       return "SEC_I_CONTINUE_NEEDED";
    case SEC_I_COMPLETE_AND_CONTINUE: return "SEC_I_COMPLETE_AND_CONTINUE";
    case SEC_I_INCOMPLETE_CREDENTIALS:return "SEC_I_INCOMPLETE_CREDENTIALS";
    case SEC_I_RENEGOTIATE:           return "SEC_I_RENEGOTIATE";
    case SEC_I_CONTEXT_EXPIRED:       return "SEC_I_CONTEXT_EXPIRED";
    case SEC_E_UNTRUSTED_ROOT:        return "SEC_E_UNTRUSTED_ROOT";
    case SEC_E_CERT_EXPIRED:          return "SEC_E_CERT_EXPIRED";
    case SEC_E_CERT_UNKNOWN:          return "SEC_E_CERT_UNKNOWN";
    case SEC_E_WRONG_PRINCIPAL:       return "SEC_E_WRONG_PRINCIPAL";
    case SEC_E_ILLEGAL_MESSAGE:       return "SEC_E_ILLEGAL_MESSAGE";
    case SEC_E_ALGORITHM_MISMATCH:    return "SEC_E_ALGORITHM_MISMATCH";
    case SEC_E_INVALID_HANDLE:        return "SEC_E_INVALID_HANDLE";
    case SEC_E_INTERNAL_ERROR:        return "SEC_E_INTERNAL_ERROR";
    case SEC_E_INVALID_TOKEN:         return "SEC_E_INVALID_TOKEN";
    case SEC_E_MESSAGE_ALTERED:       return "SEC_E_MESSAGE_ALTERED";
    case SEC_E_OUT_OF_SEQUENCE:       return "SEC_E_OUT_OF_SEQUENCE";
    case SEC_E_BUFFER_TOO_SMALL:      return "SEC_E_BUFFER_TOO_SMALL";
    default:                          return NULL;
    }
}

static void tls_seterr_sec(TlsSession *s, SECURITY_STATUS ss)
{
    const char *n = sec_name(ss);
    if (n)
        snprintf(s->err, sizeof(s->err), "%s (0x%08lx)", n,
                 (unsigned long)(DWORD)ss);
    else
        snprintf(s->err, sizeof(s->err), "Schannel error 0x%08lx",
                 (unsigned long)(DWORD)ss);
    g_seterr(s->err);
}

static int tls_wsa_init(void)
{
    WSADATA wsa;
    if (g_wsa_inited)
        return 0;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        g_seterr("WSAStartup failed");
        return -1;
    }
    g_wsa_inited = 1;
    return 0;
}

static TlsSession *sess_get(double h)
{
    long idx = (long)h;
    if (idx < 1 || idx > TLS_MAX_SESSIONS)
        return NULL;
    if (!g_sess[idx - 1].in_use)
        return NULL;
    return &g_sess[idx - 1];
}

static double sess_new(TlsSession **out)
{
    int i;
    for (i = 0; i < TLS_MAX_SESSIONS; i++) {
        if (!g_sess[i].in_use) {
            memset(&g_sess[i], 0, sizeof(g_sess[i]));
            g_sess[i].in_use = 1;
            g_sess[i].sock = INVALID_SOCKET;
            *out = &g_sess[i];
            return (double)(i + 1);
        }
    }
    g_seterr("no free TLS session slot");
    return 0.0;
}

static void sess_free(TlsSession *s)
{
    if (s)
        s->in_use = 0;
}

/* ------------------------------------------------------------------ */
/* Socket / buffered I/O                                               */
/* ------------------------------------------------------------------ */

static int tls_send_all(TlsSession *s, const char *buf, DWORD len)
{
    DWORD sent = 0;
    while (sent < len) {
        int n = send(s->sock, buf + sent, (int)(len - sent), 0);
        if (n <= 0)
            return -1;
        sent += (DWORD)n;
    }
    return 0;
}

/* Non-blocking connect with select timeout, then restore blocking mode and
 * set I/O timeouts. Returns the connected socket or INVALID_SOCKET. */
static SOCKET tls_connect_sock(TlsSession *s, const char *host, int port)
{
    struct addrinfo hints, *res = NULL, *it;
    SOCKET sock = INVALID_SOCKET;
    char portstr[16];
    int made = 0;

    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        tls_seterr_str(s, "getaddrinfo failed");
        return INVALID_SOCKET;
    }

    for (it = res; it; it = it->ai_next) {
        u_long nb = 1;
        sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == INVALID_SOCKET)
            continue;
        ioctlsocket(sock, FIONBIO, &nb);
        if (connect(sock, it->ai_addr, (int)it->ai_addrlen) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS ||
                err == WSAEINVAL) {
                fd_set wfds;
                struct timeval tv;
                FD_ZERO(&wfds);
                FD_SET(sock, &wfds);
                tv.tv_sec = TLS_CONNECT_TIMEOUT_S;
                tv.tv_usec = 0;
                if (select(0, NULL, &wfds, NULL, &tv) > 0) {
                    int soerr = 0, slen = (int)sizeof(soerr);
                    getsockopt(sock, SOL_SOCKET, SO_ERROR,
                               (char *)&soerr, &slen);
                    if (soerr == 0)
                        made = 1;
                }
            }
            if (!made) {
                closesocket(sock);
                sock = INVALID_SOCKET;
                continue;
            }
        } else {
            made = 1;
        }
        if (made) {
            u_long blk = 0;
            DWORD tmo = TLS_IO_TIMEOUT_MS;
            ioctlsocket(sock, FIONBIO, &blk);
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                       (const char *)&tmo, sizeof(tmo));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                       (const char *)&tmo, sizeof(tmo));
            break;
        }
    }
    freeaddrinfo(res);
    if (sock == INVALID_SOCKET)
        tls_seterr_str(s, "could not connect (no address succeeded)");
    return sock;
}

/* ------------------------------------------------------------------ */
/* Schannel                                                            */
/* ------------------------------------------------------------------ */

static int tls_setup_creds(TlsSession *s)
{
    SCHANNEL_CRED sc;
    TimeStamp ts;
    SECURITY_STATUS ss;
    memset(&sc, 0, sizeof(sc));
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    /* dwFlags = 0: default client credentials. Schannel validates the server
     * certificate chain against the system trust store and (because the
     * target name is supplied to InitializeSecurityContext) the hostname. */
    sc.dwFlags = 0;
    sc.grbitEnabledProtocols = 0; /* engine default (TLS 1.2 / TLS 1.3) */
    ss = AcquireCredentialsHandleA(NULL, UNISP_NAME, SECPKG_CRED_OUTBOUND,
                                   NULL, &sc, NULL, NULL, &s->cred, &ts);
    if (ss != SEC_E_OK) {
        tls_seterr_sec(s, ss);
        return -1;
    }
    s->cred_valid = 1;
    return 0;
}

/* Full client handshake. Returns 0 on success, -1 on error.
 *
 * Schannel's ISC is fed the WHOLE buffered receive stream and does its own
 * record framing; after each call, any bytes it did NOT consume are reported
 * in the trailing SECBUFFER_EXTRA buffer and must be preserved for the next
 * call. Assuming "one record = one ISC call" desyncs the buffer and makes the
 * later DecryptMessage fail with SEC_E_INVALID_TOKEN. */
static int tls_handshake(TlsSession *s)
{
    SECURITY_STATUS ss;
    DWORD outFlags = 0;
    int first = 1;

    for (;;) {
        SecBuffer outBuf;
        SecBufferDesc outDesc;
        SecBuffer inBufs[3];
        SecBufferDesc inDesc;
        DWORD inLen = 0, consumed = 0;

        /* Compact consumed bytes, then ensure at least one byte is buffered
         * (never on the very first call, which has no input). */
        if (s->rpos > 0) {
            if (s->rpos < s->rlen)
                memmove(s->rbuf, s->rbuf + s->rpos, s->rlen - s->rpos);
            s->rlen -= s->rpos;
            s->rpos = 0;
        }
        if (!first && s->rlen == 0) {
            int n = recv(s->sock, s->rbuf, (int)sizeof(s->rbuf), 0);
            if (n <= 0) {
                tls_seterr_str(s, "recv failed during TLS handshake");
                return -1;
            }
            s->rlen = (DWORD)n;
        }

        if (!first) {
            inLen = s->rlen;
            inBufs[0].pvBuffer = s->rbuf;
            inBufs[0].cbBuffer = inLen;
            inBufs[0].BufferType = SECBUFFER_TOKEN;
            inBufs[1].pvBuffer = NULL;
            inBufs[1].cbBuffer = 0;
            inBufs[1].BufferType = SECBUFFER_EMPTY;
            inBufs[2].pvBuffer = NULL;
            inBufs[2].cbBuffer = 0;
            inBufs[2].BufferType = SECBUFFER_EMPTY;
            inDesc.ulVersion = SECBUFFER_VERSION;
            inDesc.cBuffers = 3;
            inDesc.pBuffers = inBufs;
        }

        outBuf.pvBuffer = NULL;
        outBuf.cbBuffer = 0;
        outBuf.BufferType = SECBUFFER_TOKEN;
        outDesc.ulVersion = SECBUFFER_VERSION;
        outDesc.cBuffers = 1;
        outDesc.pBuffers = &outBuf;

        ss = InitializeSecurityContextA(
            &s->cred,
            first ? NULL : &s->ctx,
            s->host,                /* target name: SNI + hostname validation */
            TLS_ISC_FLAGS,
            0,
            SECURITY_NATIVE_DREP,
            first ? NULL : &inDesc,
            0,
            first ? &s->ctx : NULL, /* new context only on the first call */
            &outDesc,
            &outFlags,
            NULL);

        if (first) {
            s->have_ctx = 1;
            first = 0;
        }

        /* How much of the input did Schannel consume? If it left bytes
         * unconsumed, they are reported in the SECBUFFER_EXTRA buffer
         * (inBufs[1]) and must stay buffered for the next call. */
        if (!first && inLen) {
            if (inBufs[1].BufferType == SECBUFFER_EXTRA &&
                inBufs[1].cbBuffer <= inLen)
                consumed = inLen - inBufs[1].cbBuffer;
            else
                consumed = inLen;
            if (consumed > inLen)
                consumed = inLen;
        }

        if (ss == SEC_E_OK) {
            /* Handshake complete; flush any final token. The consumed input
             * (the server's Finished) is dropped; any leftover is discarded
             * because the handshake is over. */
            s->rpos += consumed;
            if (outBuf.pvBuffer && outBuf.cbBuffer) {
                if (tls_send_all(s, (const char *)outBuf.pvBuffer,
                                 outBuf.cbBuffer) < 0) {
                    if (outBuf.pvBuffer)
                        FreeContextBuffer(outBuf.pvBuffer);
                    tls_seterr_str(s, "send failed (final handshake token)");
                    return -1;
                }
            }
            if (outBuf.pvBuffer)
                FreeContextBuffer(outBuf.pvBuffer);
            s->handshake_done = 1;
            return 0;
        }
        if (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_I_INCOMPLETE_CREDENTIALS ||
            ss == SEC_I_RENEGOTIATE || ss == SEC_I_COMPLETE_AND_CONTINUE) {
            if (outBuf.pvBuffer && outBuf.cbBuffer) {
                if (tls_send_all(s, (const char *)outBuf.pvBuffer,
                                 outBuf.cbBuffer) < 0) {
                    if (outBuf.pvBuffer)
                        FreeContextBuffer(outBuf.pvBuffer);
                    tls_seterr_str(s, "send failed (handshake token)");
                    return -1;
                }
            }
            if (outBuf.pvBuffer)
                FreeContextBuffer(outBuf.pvBuffer);
            /* Keep only the unconsumed bytes. */
            s->rpos += consumed;
            continue;
        }
        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            /* Need more bytes for this record; the buffer is kept as-is
             * (nothing was consumed) and topped up below. */
            int n;
            if (outBuf.pvBuffer)
                FreeContextBuffer(outBuf.pvBuffer);
            if (s->rlen >= sizeof(s->rbuf)) {
                tls_seterr_str(s,
                               "TLS handshake record larger than buffer");
                return -1;
            }
            n = recv(s->sock, s->rbuf + s->rlen,
                     (int)(sizeof(s->rbuf) - s->rlen), 0);
            if (n <= 0) {
                tls_seterr_str(s, "recv failed during TLS handshake");
                return -1;
            }
            s->rlen += (DWORD)n;
            continue;
        }
        /* Real handshake failure (chain validation errors land here). */
        if (outBuf.pvBuffer)
            FreeContextBuffer(outBuf.pvBuffer);
        tls_seterr_sec(s, ss);
        return -1;
    }
}

/* Send close_notify (best effort). Returns 0. */
static int tls_shutdown(TlsSession *s)
{
    SecBufferDesc outDesc, inDesc;
    SecBuffer outBuf, inBufs[4];
    SecPkgContext_StreamSizes sizes;
    BYTE *p;
    SECURITY_STATUS ss;

    if (!s->handshake_done || s->closed || !s->have_ctx)
        return 0;

    outBuf.pvBuffer = NULL;
    outBuf.cbBuffer = 0;
    outBuf.BufferType = SECBUFFER_TOKEN;
    outDesc.ulVersion = SECBUFFER_VERSION;
    outDesc.cBuffers = 1;
    outDesc.pBuffers = &outBuf;
    ApplyControlToken(&s->ctx, &outDesc); /* signal shutdown */

    if (QueryContextAttributes(&s->ctx, SECPKG_ATTR_STREAM_SIZES,
                               &sizes) != SEC_E_OK)
        return 0;
    p = (BYTE *)malloc(sizes.cbHeader + sizes.cbTrailer);
    if (!p)
        return 0;
    inBufs[0].pvBuffer = p;
    inBufs[0].cbBuffer = sizes.cbHeader;
    inBufs[0].BufferType = SECBUFFER_STREAM_HEADER;
    inBufs[1].pvBuffer = NULL;
    inBufs[1].cbBuffer = 0;
    inBufs[1].BufferType = SECBUFFER_DATA;
    inBufs[2].pvBuffer = p + sizes.cbHeader;
    inBufs[2].cbBuffer = sizes.cbTrailer;
    inBufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
    inBufs[3].pvBuffer = NULL;
    inBufs[3].cbBuffer = 0;
    inBufs[3].BufferType = SECBUFFER_EMPTY;
    inDesc.ulVersion = SECBUFFER_VERSION;
    inDesc.cBuffers = 4;
    inDesc.pBuffers = inBufs;
    ss = EncryptMessage(&s->ctx, 0, &inDesc, 0);
    if (ss == SEC_E_OK)
        tls_send_all(s, (const char *)p,
                     inBufs[0].cbBuffer + inBufs[1].cbBuffer +
                     inBufs[2].cbBuffer);
    free(p);
    return 0;
}

static void tls_close_session(TlsSession *s)
{
    if (s->have_ctx) {
        DeleteSecurityContext(&s->ctx);
        s->have_ctx = 0;
    }
    if (s->cred_valid) {
        FreeCredentialsHandle(&s->cred);
        s->cred_valid = 0;
    }
    if (s->sock != INVALID_SOCKET) {
        closesocket(s->sock);
        s->sock = INVALID_SOCKET;
    }
    s->closed = 1;
}

/* Read + decrypt the next application-data record.
 *
 * This is the canonical Schannel streaming pattern: the whole receive buffer
 * is handed to DecryptMessage and Schannel does the record framing itself.
 * A partial record is reported as SEC_E_INCOMPLETE_MESSAGE (read more and
 * retry); leftover bytes of the next record come back in SECBUFFER_EXTRA.
 *
 * Returns 0 = plaintext copied (out[0..*got)), 1 = EOF (close_notify),
 * -1 = error (lastError set). */
static int tls_decrypt(TlsSession *s, char *out, DWORD cap, DWORD *got)
{
    *got = 0;
    for (;;) {
        SECURITY_STATUS ss;
        SecBuffer bufs[4];
        SecBufferDesc desc;
        DWORD avail;

        /* Compact already-consumed bytes. */
        if (s->rpos > 0) {
            if (s->rpos < s->rlen)
                memmove(s->rbuf, s->rbuf + s->rpos, s->rlen - s->rpos);
            s->rlen -= s->rpos;
            s->rpos = 0;
        }
        if (s->rlen == 0) {
            int n = recv(s->sock, s->rbuf, (int)sizeof(s->rbuf), 0);
            if (n <= 0) {
                tls_seterr_str(s, "recv failed (TLS record)");
                return -1;
            }
            s->rlen = (DWORD)n;
        }
        avail = s->rlen;

        bufs[0].pvBuffer = s->rbuf;
        bufs[0].cbBuffer = avail;
        bufs[0].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer = NULL;
        bufs[1].cbBuffer = 0;
        bufs[1].BufferType = SECBUFFER_EMPTY;
        bufs[2].pvBuffer = NULL;
        bufs[2].cbBuffer = 0;
        bufs[2].BufferType = SECBUFFER_EMPTY;
        bufs[3].pvBuffer = NULL;
        bufs[3].cbBuffer = 0;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers = 4;
        desc.pBuffers = bufs;

        ss = DecryptMessage(&s->ctx, &desc, 0, NULL);
        if (ss == SEC_E_OK) {
            /* Anything Schannel did not consume is the start of the next
             * record and is reported in the SECBUFFER_EXTRA buffer. The
             * decrypted plaintext is returned in the SECBUFFER_DATA buffer
             * (with the record split into STREAM_HEADER/DATA/TRAILER). */
            DWORD extra = (bufs[3].BufferType == SECBUFFER_EXTRA)
                              ? bufs[3].cbBuffer : 0;
            DWORD consumed = (avail >= extra) ? (avail - extra) : avail;
            s->rpos = consumed;
            for (int j = 0; j < 4; j++) {
                if (bufs[j].BufferType == SECBUFFER_DATA &&
                    bufs[j].cbBuffer > 0 && bufs[j].pvBuffer) {
                    DWORD n = bufs[j].cbBuffer;
                    if (n > cap)
                        n = cap;
                    memcpy(out, bufs[j].pvBuffer, n);
                    *got = n;
                    return 0;
                }
            }
            continue; /* empty app-data record (e.g. TLS 1.3 ticket) */
        }
        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            /* Need more bytes: top up the buffer and retry. Nothing was
             * consumed, so rpos stays at 0. */
            int n;
            if (s->rlen >= sizeof(s->rbuf)) {
                tls_seterr_str(s, "TLS record larger than receive buffer");
                return -1;
            }
            n = recv(s->sock, s->rbuf + s->rlen,
                     (int)(sizeof(s->rbuf) - s->rlen), 0);
            if (n <= 0) {
                tls_seterr_str(s, "recv failed (incomplete TLS record)");
                return -1;
            }
            s->rlen += (DWORD)n;
            continue;
        }
        if (ss == SEC_I_CONTEXT_EXPIRED) {
            s->rpos = s->rlen; /* discard any leftover */
            return 1; /* peer sent close_notify */
        }
        if (ss == SEC_I_RENEGOTIATE) {
            /* Peer asked to renegotiate: let the handshake code re-parse the
             * buffered handshake records, then continue reading. */
            s->rpos = 0;
            if (tls_handshake(s) < 0)
                return -1;
            continue;
        }
        tls_seterr_sec(s, ss);
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* JS exports                                                          */
/* ------------------------------------------------------------------ */

static MBJSValue tls_tlsConnect(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    const char *host;
    size_t hostLen;
    double d = 0;
    int port;
    TlsSession *s;
    double h;
    (void)this_val;

    if (argc < 2)
        return MBJS_NewInt32(ctx, -1);
    host = rt.ToCStringLen(ctx, &hostLen, argv[0]);
    if (!host || hostLen == 0 || hostLen > 255) {
        rt.FreeCString(ctx, host);
        g_seterr("invalid host");
        return MBJS_NewInt32(ctx, -1);
    }
    if (rt.ToFloat64(ctx, &d, argv[1]) < 0) {
        rt.FreeCString(ctx, host);
        g_seterr("invalid port");
        return MBJS_NewInt32(ctx, -1);
    }
    port = (int)d;
    if (port <= 0 || port > 65535) {
        rt.FreeCString(ctx, host);
        g_seterr("port out of range");
        return MBJS_NewInt32(ctx, -1);
    }
    if (tls_wsa_init() < 0) {
        rt.FreeCString(ctx, host);
        return MBJS_NewInt32(ctx, -1);
    }

    h = sess_new(&s);
    if (h == 0.0) {
        rt.FreeCString(ctx, host);
        return MBJS_NewInt32(ctx, -1);
    }
    snprintf(s->host, sizeof(s->host), "%s", host);
    rt.FreeCString(ctx, host);

    s->sock = tls_connect_sock(s, s->host, port);
    if (s->sock == INVALID_SOCKET) {
        sess_free(s);
        return MBJS_NewInt32(ctx, -1);
    }
    if (tls_setup_creds(s) < 0) {
        tls_close_session(s);
        sess_free(s);
        return MBJS_NewInt32(ctx, -1);
    }
    if (tls_handshake(s) < 0) {
        tls_close_session(s);
        sess_free(s);
        return MBJS_NewInt32(ctx, -1);
    }
    return MBJS_NewFloat64(ctx, h);
}

static MBJSValue tls_tlsWrite(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    TlsSession *s;
    const char *data;
    size_t len;
    SECURITY_STATUS ss;
    SecPkgContext_StreamSizes sizes;
    BYTE *outbuf = NULL;
    SecBuffer bufs[4];
    SecBufferDesc desc;
    DWORD total;
    (void)this_val;

    if (argc < 2)
        return MBJS_NewInt32(ctx, -1);
    {
        double d = 0;
        if (rt.ToFloat64(ctx, &d, argv[0]) < 0)
            return MBJS_NewInt32(ctx, -1);
        s = sess_get(d);
    }
    if (!s) {
        g_seterr("invalid session handle");
        return MBJS_NewInt32(ctx, -1);
    }
    data = rt.ToCStringLen(ctx, &len, argv[1]);
    if (!data)
        return MBJS_NewInt32(ctx, -1);

    ss = QueryContextAttributes(&s->ctx, SECPKG_ATTR_STREAM_SIZES, &sizes);
    if (ss != SEC_E_OK) {
        rt.FreeCString(ctx, data);
        tls_seterr_sec(s, ss);
        return MBJS_NewInt32(ctx, -1);
    }
    if (len > sizes.cbMaximumMessage) {
        rt.FreeCString(ctx, data);
        tls_seterr_str(s, "message too large for a single TLS record");
        return MBJS_NewInt32(ctx, -1);
    }
    total = sizes.cbHeader + (DWORD)len + sizes.cbTrailer;
    outbuf = (BYTE *)malloc(total);
    if (!outbuf) {
        rt.FreeCString(ctx, data);
        g_seterr("out of memory");
        return MBJS_NewInt32(ctx, -1);
    }
    /* Copy plaintext into our own buffer (EncryptMessage may write in place). */
    memcpy(outbuf + sizes.cbHeader, data, len);
    rt.FreeCString(ctx, data);

    bufs[0].pvBuffer = outbuf;
    bufs[0].cbBuffer = sizes.cbHeader;
    bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
    bufs[1].pvBuffer = outbuf + sizes.cbHeader;
    bufs[1].cbBuffer = (DWORD)len;
    bufs[1].BufferType = SECBUFFER_DATA;
    bufs[2].pvBuffer = outbuf + sizes.cbHeader + len;
    bufs[2].cbBuffer = sizes.cbTrailer;
    bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
    bufs[3].pvBuffer = NULL;
    bufs[3].cbBuffer = 0;
    bufs[3].BufferType = SECBUFFER_EMPTY;
    desc.ulVersion = SECBUFFER_VERSION;
    desc.cBuffers = 4;
    desc.pBuffers = bufs;

    ss = EncryptMessage(&s->ctx, 0, &desc, 0);
    if (ss != SEC_E_OK) {
        free(outbuf);
        tls_seterr_sec(s, ss);
        return MBJS_NewInt32(ctx, -1);
    }
    total = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
    if (tls_send_all(s, (const char *)outbuf, total) < 0) {
        free(outbuf);
        tls_seterr_str(s, "send failed (encrypted record)");
        return MBJS_NewInt32(ctx, -1);
    }
    free(outbuf);
    return MBJS_NewFloat64(ctx, (double)len);
}

static MBJSValue tls_tlsRead(MBJSContext *ctx, MBJSValueConst this_val,
                             int argc, MBJSValueConst *argv)
{
    TlsSession *s;
    int cap = 65536;
    char *out;
    DWORD got = 0;
    int rc;
    (void)this_val;

    if (argc < 1)
        return MBJS_UNDEFINED;
    {
        double d = 0;
        if (rt.ToFloat64(ctx, &d, argv[0]) < 0)
            return MBJS_UNDEFINED;
        s = sess_get(d);
    }
    if (!s)
        return MBJS_UNDEFINED;
    if (argc >= 2) {
        double d = 0;
        if (rt.ToFloat64(ctx, &d, argv[1]) == 0 && d > 0 && d < 1 << 20)
            cap = (int)d;
    }
    out = (char *)malloc((size_t)cap);
    if (!out)
        return MBJS_UNDEFINED;
    rc = tls_decrypt(s, out, (DWORD)cap, &got);
    if (rc != 0 || got == 0) {
        free(out);
        return MBJS_UNDEFINED; /* EOF or error; check tlsLastError */
    }
    {
        MBJSValue v = rt.NewStringLen(ctx, out, (size_t)got);
        free(out);
        return v;
    }
}

static MBJSValue tls_tlsClose(MBJSContext *ctx, MBJSValueConst this_val,
                              int argc, MBJSValueConst *argv)
{
    TlsSession *s;
    (void)this_val;
    if (argc < 1)
        return MBJS_UNDEFINED;
    {
        double d = 0;
        if (rt.ToFloat64(ctx, &d, argv[0]) < 0)
            return MBJS_UNDEFINED;
        s = sess_get(d);
    }
    if (s) {
        tls_shutdown(s);
        tls_close_session(s);
        sess_free(s);
    }
    return MBJS_UNDEFINED;
}

static MBJSValue tls_tlsShutdown(MBJSContext *ctx, MBJSValueConst this_val,
                                 int argc, MBJSValueConst *argv)
{
    TlsSession *s;
    (void)this_val;
    if (argc < 1)
        return MBJS_UNDEFINED;
    {
        double d = 0;
        if (rt.ToFloat64(ctx, &d, argv[0]) < 0)
            return MBJS_UNDEFINED;
        s = sess_get(d);
    }
    if (s)
        tls_shutdown(s);
    return MBJS_UNDEFINED;
}

static MBJSValue tls_tlsLastError(MBJSContext *ctx, MBJSValueConst this_val,
                                  int argc, MBJSValueConst *argv)
{
    TlsSession *s = NULL;
    const char *msg;
    (void)this_val;
    if (argc >= 1) {
        double d = 0;
        if (rt.ToFloat64(ctx, &d, argv[0]) == 0)
            s = sess_get(d);
    }
    msg = s ? s->err : g_err;
    if (!msg)
        msg = "";
    return rt.NewStringLen(ctx, msg, strlen(msg));
}

static MBJSValue tls_tlsVersion(MBJSContext *ctx, MBJSValueConst this_val,
                                int argc, MBJSValueConst *argv)
{
    SecPkgContext_ConnectionInfo ci;
    SECURITY_STATUS ss;
    TlsSession *s;
    const char *name = "unknown";
    (void)this_val;
    if (argc < 1)
        return MBJS_UNDEFINED;
    {
        double d = 0;
        if (rt.ToFloat64(ctx, &d, argv[0]) < 0)
            return MBJS_UNDEFINED;
        s = sess_get(d);
    }
    if (!s)
        return MBJS_UNDEFINED;
    ss = QueryContextAttributes(&s->ctx, SECPKG_ATTR_CONNECTION_INFO, &ci);
    if (ss != SEC_E_OK)
        return rt.NewStringLen(ctx, "unknown", 7);
    switch (ci.dwProtocol) {
    case SP_PROT_TLS1_3_CLIENT: name = "TLS 1.3"; break;
    case SP_PROT_TLS1_2_CLIENT: name = "TLS 1.2"; break;
    case SP_PROT_TLS1_1_CLIENT: name = "TLS 1.1"; break;
    case SP_PROT_TLS1_CLIENT:   name = "TLS 1.0"; break;
    default:                    name = "unknown"; break;
    }
    return rt.NewStringLen(ctx, name, strlen(name));
}

/* ------------------------------------------------------------------ */
/* module plumbing                                                     */
/* ------------------------------------------------------------------ */

static const MBJSCFunctionListEntry g_tab[] = {
    MBJS_CFUNC_DEF("tlsConnect", 2, tls_tlsConnect),
    MBJS_CFUNC_DEF("tlsWrite", 2, tls_tlsWrite),
    MBJS_CFUNC_DEF("tlsRead", 2, tls_tlsRead),
    MBJS_CFUNC_DEF("tlsClose", 1, tls_tlsClose),
    MBJS_CFUNC_DEF("tlsShutdown", 1, tls_tlsShutdown),
    MBJS_CFUNC_DEF("tlsLastError", 0, tls_tlsLastError),
    MBJS_CFUNC_DEF("tlsVersion", 1, tls_tlsVersion),
};

/* The count passed to AddModuleExportList/SetModuleExportList MUST equal the
 * exact number of entries in g_tab — derived from the table itself so the two
 * can never disagree. */
#define TLS_EXPORT_COUNT ((int)(sizeof(g_tab) / sizeof(g_tab[0])))

static int tls_mod_init(MBJSContext *ctx, MBJSModuleDef *m)
{
    return rt.SetModuleExportList(ctx, m, g_tab, TLS_EXPORT_COUNT);
}

MBJS_MODULE_EXTERN MBJSModuleDef *mbjs_init_module(MBJSContext *ctx,
                                                    const char *name)
{
    const char *missing = mbjs_runtime_resolve(&rt);
    MBJSModuleDef *m;

    if (missing)
        return NULL;
    m = rt.NewCModule(ctx, name ? name : "mochitls", tls_mod_init);
    if (!m)
        return NULL;
    if (rt.AddModuleExportList(ctx, m, g_tab, TLS_EXPORT_COUNT) != 0)
        return NULL;
    return m;
}
