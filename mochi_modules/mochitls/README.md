# mochitls

Native-first MochiBerry TLS client built on Windows Schannel (SSPI) over
Winsock2. Provides the engine-plugin exports the JS layer needs to speak
HTTPS with full certificate chain validation.

## Layout

```text
mochitls/
├── README.md
├── CMakeLists.txt
├── include/
│   ├── mochitls.h
│   └── mochitls_abi.h
├── native/
│   └── mochitls.c
├── script/
│   └── index.js
└── lib/
    └── mochitls_x86_64.dll
```

## Exports (JS-visible)

| Export           | Args          | Returns                                  |
|------------------|---------------|------------------------------------------|
| `tlsConnect`     | `(host, port)`| handle (> 0) or -1                       |
| `tlsWrite`       | `(handle, data)`| plaintext bytes accepted (== len) or -1 |
| `tlsRead`        | `(handle, max)`| decrypted string, or undefined (EOF/err)|
| `tlsClose`       | `(handle)`    | undefined (sends close_notify, frees)    |
| `tlsShutdown`    | `(handle)`    | undefined (close_notify only)            |
| `tlsLastError`   | `([handle])`  | string                                    |
| `tlsVersion`     | `(handle)`    | "TLS 1.2" / "TLS 1.3" / "unknown"        |

## Security model

`tlsConnect` acquires Schannel client credentials with `SCHANNEL_CRED.dwFlags
= 0` (no `SCH_CRED_MANUAL_CRED_VALIDATION`), so Schannel performs the full
server certificate chain validation against the system trust store. The host
is passed as the SSPI target name, so the hostname is validated too
(`SEC_E_WRONG_PRINCIPAL` on mismatch). TLS 1.2/1.3 (engine default protocol
set) are supported; the negotiated protocol is available via `tlsVersion`.

## Errors

No engine exceptions are thrown (matches the `mochinode_net` convention).
Failures return `-1` / `undefined` and set a readable message retrievable via
`tlsLastError([handle])`. Common Schannel codes (`SEC_E_UNTRUSTED_ROOT`,
`SEC_E_WRONG_PRINCIPAL`, `SEC_E_CERT_UNKNOWN`, ...) are decoded to names plus
the raw hex code.

## Build

```bash
cmake -S mochi_modules/mochitls -B mochi_modules/mochitls/build \
      -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-windows-mingw.cmake
cmake --build mochi_modules/mochitls/build
```

Links `secur32`, `crypt32`, `ws2_32`. The DLL must not import `mbjs.exe`;
engine symbols are resolved at load time via `native/mbjs_runtime.h`.

## Selftest

```bash
E:/bcf/mochiberry/build_manual/mbjs.exe -m selftest/mochi-tls.js
```

Prints `TLS_ALL_OK` when the handshake AND the HTTPS response against
`https://example.com` are both verified.
