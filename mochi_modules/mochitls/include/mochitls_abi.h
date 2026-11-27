/*
 * mochitls_abi.h — C ABI surface of the mochitls native engine plugin.
 *
 * This module is an engine plugin (exported entry point `mbjs_init_module`),
 * NOT a dynalib, so the JS side reaches it through the engine's native loader
 * (mochinode.loadNative('mochitls.dll')) rather than through these prototypes.
 * The prototypes are kept here for reference/parity and are the exact function
 * signatures the plugin exports to JS.
 *
 * Convention: no engine exceptions are thrown. Failures return -1 (numeric)
 * or undefined (read/EOF) and leave a readable message retrievable via
 * tlsLastError(). The handle is an opaque JS number (index into the plugin's
 * session table) — never dereference it in JS.
 */
#ifndef MOCHITLS_ABI_H
#define MOCHITLS_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Establish a Schannel TLS session: resolve host, connect (10s timeout),
 * run the full client handshake with default credentials (system-trust chain
 * validation + hostname validation). Returns an opaque handle (> 0) or -1. */
int mochitls_tlsConnect(const char *host, int port);

/* Encrypt and send `len` bytes of plaintext. Returns bytes accepted (== len)
 * or -1. */
int mochitls_tlsWrite(long long handle, const char *data, int len);

/* Read + decrypt the next record into `out` (capacity `cap`). Returns the
 * plaintext byte count (>= 0), or -1 on EOF/error. */
int mochitls_tlsRead(long long handle, char *out, int cap);

/* Send close_notify and tear the session down. */
void mochitls_tlsClose(long long handle);

/* Best-effort close_notify only (session stays usable until tlsClose). */
void mochitls_tlsShutdown(long long handle);

/* Last error message for a session (or the global one if handle < 0). */
const char *mochitls_tlsLastError(long long handle);

/* Negotiated protocol name ("TLS 1.2", "TLS 1.3", ...) or "unknown". */
const char *mochitls_tlsVersion(long long handle);

#ifdef __cplusplus
}
#endif

#endif /* MOCHITLS_ABI_H */
