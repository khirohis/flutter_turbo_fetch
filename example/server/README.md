# HTTP/2 Benchmark Mock Server

A standalone local HTTP/2 server for testing and benchmarking the `example/` app with concurrent requests. Built using standard Node.js `node:http2` without external npm dependencies.

---

## 1. TLS Certificate Setup (`cert.pem` / `key.pem`)

To negotiate standard HTTP/2 (ALPN `h2`) on Android (`OkHttp`) and iOS (`URLSession`), place locally trusted TLS certificates (recommended: [`mkcert`](https://github.com/FiloSottile/mkcert)) in this directory:

```bash
cd example/server
mkcert -key-file key.pem -cert-file cert.pem localhost 127.0.0.1 10.0.2.2
```

---

## 2. Starting the Server

```bash
node example/server/http2_mock_server.mjs
```

The default bind address is `127.0.0.1`. For a physical iPhone, set `HOST` to
one specific Mac LAN address and include that address in the certificate SAN.
`REQUIRE_TLS=1` prevents fallback to cleartext. `BENCHMARK_LOG=1` pre-generates
scenario payloads and logs response status, byte count, and observed protocol
without client addresses. See the [iOS protocol](../../docs/benchmarks/ios/README.md).

---

## 3. Endpoint Specifications

- **`GET /json?sizeKb=100&delayMs=0`**
  - `sizeKb`: Size of the generated JSON payload in KiB (default: `100`).
  - `delayMs`: Artificial server-side response delay in milliseconds (default: `0`).
  - Returns the negotiated protocol (`2.0` or `1.1`) in the `x-http-version` response header.
- **Android Emulator Access URL**:
  - `https://10.0.2.2:8443/json?sizeKb=100`
- **iOS Simulator / Host Access URL**:
  - `https://localhost:8443/json?sizeKb=100`
