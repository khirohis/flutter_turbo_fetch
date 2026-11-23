# Design Rationale & Technical Justifications (`design-rationale.md`)

This document outlines the background, bottleneck analyses, evaluation of existing packages, and technical justifications behind the architectural decisions of `flutter_turbo_fetch`.

---

## 1. Problem Statement & The Three Fundamental Bottlenecks

In Flutter applications (`package:dio`), making concurrent HTTP/2 requests for medium-to-large JSON payloads (tens of KiB to hundreds of KiB or more) causes significant throughput degradation and UI stutter due to three compounding structural bottlenecks:

1. **Limitations of Pure Dart Implementations (`dio_http2_adapter` / `package:http2`)**
   - Both the initial client stream receive window and the connection-level receive window are hardcoded to the RFC default of **65,535 bytes (~64KiB)** (the connection window is hidden internally and cannot be configured). Under concurrent requests exceeding 64KiB in aggregate, communication stalls frequently while awaiting `WINDOW_UPDATE` frames.
   - All streams multiplex over a single `SecureSocket` per host, serializing BoringSSL TLS decryption. Furthermore, parsing granular frames (typically <= 16KiB) monopolizes the single event loop of the Main Isolate.

2. **Overhead of Standard Native Adapters (`native_dio_adapter`: `cronet_http` / `cupertino_http`)**
   - Because these adapters adhere to the streaming interface of `package:http` (`StreamedResponse`), they incur **continuous FFI notifications, memory copies, and round-trip ping-pong for every small chunk (16KiB to 32KiB)** between Native and Dart (Main Isolate).
   - **Android (`cronet_http`)**: Incurs initial dynamic loading latency from Google Play Services (GMS), followed by pull-based ping-pong where Dart must repeatedly invoke `urlRequest.read(byteBuffer)` for each individual chunk.
   - **iOS (`cupertino_http`)**: Relies on `URLSessionDataDelegate`, causing body streaming to pause upon header arrival (`didReceiveResponse:`) until Dart's Main Isolate returns a `ResponseAllow` decision, along with FFI overhead on every `didReceiveData:`.

3. **Main Isolate Event Loop Starvation from JSON Parsing (Secondary Impact)**
   - Dio's standard `BackgroundTransformer` synchronously executes `utf8.decode` on the Main Isolate (allocating multi-megabyte intermediate `String`s) before handing data off to another Isolate. Moreover, it spawns unconstrained `compute` (`Isolate.run`) instances for payloads over 50KiB, causing thread saturation and context-switch churn under high concurrency.
   - When the Main Isolate blocks for even a few milliseconds, chunk processing and window update requests for other active network streams freeze, creating cascading application-level Head-of-Line (HoL) blocking.

---

## 2. Evaluation of Existing `pub.dev` Packages

| Package | Underlying Engine | Chunk Concatenation | Evaluation & Decision |
| :--- | :--- | :--- | :--- |
| **`native_dio_adapter`**<br>(`cronet_http` / `cupertino_http`) | Android: Cronet<br>iOS: `URLSessionDataDelegate` | **Dart side**<br>(Fragmented chunk ping-pong) | **Not Adopted**<br>Adherence to `StreamedResponse` causes per-chunk FFI round-trips and iOS header-pause latency that cannot be bypassed. |
| **`ok_http`** | Android: `OkHttp3` (`jnigen`) | **Dart side**<br>(Stream consumption) | **Not Adopted**<br>Constrained by the streaming model of `package:http`, missing out on the efficiency of batch native aggregation (`response.body.bytes()`). |
| **`rhttp`** | All platforms: Rust `reqwest` + `tokio` | **Rust side batch concatenation** | **Not Adopted**<br>While offering batch byte assembly in Rust (`Body.bytes`), bundling static Rust binaries inflates app size by several MiB, bypasses platform TLS session caches and system certificate/proxy integration, and introduces Rust toolchain dependencies. |
| **`flutter_turbo_fetch`**<br>(This plugin) | **Android: `OkHttp3` (default) / `Cronet` (opt-in)**<br>**iOS / macOS: `URLSession`**<br>**Windows: `WinHTTP`**<br>**Linux: `libcurl` (`curl_multi`)** | **OS Native Background Batch Reception** | **Adopted Architecture**<br>Leverages platform-standard, lightweight network stacks. Completes chunk assembly entirely in OS background memory and delivers a single, unified `Uint8List` to Dart via Pigeon in one trip. |

---

## 3. Technical Foundations of the Core Design Principles

### 3.1 Why not parse JSON in the native layer?

Parsing JSON in native code (Kotlin / Swift / C++) cannot directly allocate Dart heap objects (`Map` / `List`). Doing so would require **double serialization** (native serialization / encoding into binary codecs + Dart deserialization), which is slower than passing raw bytes. The native layer achieves maximum performance by focusing strictly on **batch assembly and single-trip return of raw byte arrays (`Uint8List`)**.

### 3.2 Why `OkHttp3` as Default and `Cronet` as Build-Time Opt-In on Android?

`OkHttp3` is the default Android engine because it requires zero extra app setup or Google Play Services (GMS) dependency, supports `allowBadCertificates: true` for local development, defaults its HTTP/2 initial window size to an expanded **16MiB**, and completes chunk assembly in native memory with optimal speed using `Okio`'s shared segment pool via `response.body.bytes()` (or bounded lookahead via `BufferedSource.request(...)` when `maxResponseBytes` is specified to abort oversized responses early).

For applications that need maximum native C++ HTTP/2 throughput or HTTP/3 (QUIC) support, `flutter_turbo_fetch` also provides **build-time `Cronet` selection** (`flutter_turbo_fetch.androidEngine=cronet`). Selecting Cronet excludes `OkHttp3` completely from the APK/AAB and lets the app choose its preferred provider (`play-services-cronet` or `cronet-embedded`), while still buffering chunks natively in Kotlin/JVM before a single-trip Pigeon return—avoiding the per-chunk Dart FFI ping-pong of `cronet_http`.

### 3.3 Why avoid `URLSessionDataDelegate` on iOS / macOS?

Streaming via `URLSessionDataDelegate` incurs cross-thread, cross-language signaling overhead for every `didReceiveResponse:` and `didReceiveData:` invocation. By deliberately omitting `URLSessionDataDelegate` and adopting `dataTask(with:completionHandler:)` across the shared Darwin (`iOS` / `macOS`) implementation, header negotiation through chunk concatenation stays entirely within Apple's native CFNetwork pipeline (C/C++ background engine), while `URLSessionTaskDelegate` (`TurboSessionTaskDelegate`) is used strictly for per-task redirect policy, self-signed certificate trust, and timeout phase tracking alongside direct cancellation via `URLSessionDataTask`.

### 3.4 Why `WinHTTP` (`winhttp.dll`) with OS Thread Pool on Windows?

Windows provides `WinHTTP` as a system-level C API baked directly into the OS (`winhttp.dll`, zero external binary footprint). Enabling `WINHTTP_PROTOCOL_FLAG_HTTP2` on a shared `HINTERNET` session unlocks kernel-assisted SChannel TLS decryption and HTTP/2 multiplexing across concurrent requests. Running synchronous `WinHTTP` calls inside a bounded Windows Thread Pool (`TP_POOL`) eliminates the complex state-machine and `HANDLE_CLOSING` use-after-free hazards of `WINHTTP_FLAG_ASYNC`, while allowing instant cross-thread cancellation via `WinHttpCloseHandle` and direct native buffer assembly into `std::vector<uint8_t>`.

### 3.5 Why `libcurl` with a Single-Threaded `curl_multi` Event Loop on Linux?

On Linux, `libcurl` is the de facto system networking library, linking against OpenSSL and `nghttp2` to provide full HTTP/2 multiplexing with zero custom protocol reimplementation. Rather than running separate `curl_easy_perform` worker threads with `CURLSH*` (which cannot multiplex concurrent HTTP/2 streams over a single TCP connection), `flutter_turbo_fetch` drives all in-flight transfers through a **single background `curl_multi` event loop** (`CURLM*` with HTTP/2 multiplexing enabled). This keeps all blocking network I/O completely off the GTK/Flutter main loop, maximizes true HTTP/2 stream multiplexing across concurrent requests, and supports deterministic cross-thread cancellation without handle teardown races. Completed responses are assembled into contiguous `std::vector<uint8_t>` buffers and dispatched back to the GTK main thread via `g_idle_add` and Pigeon GObject integration.

### 3.6 Why semaphore-governed `Isolate.run` instead of persistent worker pools (`SendPort.send`)?

Persistent worker pools require returning results via `SendPort.send(result)`, which performs a deep copy (**Transitive Copy**) of the entire decoded object graph across isolated heaps (consuming 25% to 40% of total parsing time). In contrast, `Isolate.run` leverages `Isolate.exit` upon worker termination, transferring memory ownership of the object graph to the Main Isolate in **$O(1)$ zero-copy (a few microseconds)**.

---

## 4. Evaluation of Dart ⇄ Native Communication Bridges

### Option A: Pigeon (`StandardMessageCodec` + Type-Safe Code Generation) — **Adopted**

- Returns completed byte arrays as `byte[]` on Android, `FlutterStandardTypedData(bytes: Data)` on iOS/macOS, `std::vector<uint8_t>` on Windows (C++), and `const uint8_t*` (`FlValue` uint8 list) on Linux (GObject) in a single IPC exchange.
- Because data is transferred **once per request** rather than once per chunk, Channel serialization and copy overhead (~0.02 ms to 0.05 ms for 100KiB) is negligible.
- Provides superior thread safety, asynchronous background callback support, and compile-time type safety.

### Option B: Raw FFI / JNI (`jnigen` / `ffigen`) — **Not Adopted**

- While theoretically eliminating one memory copy, it significantly increases implementation complexity regarding asynchronous callback lifecycles, garbage collection integration (`Finalizer`), and thread synchronization without meaningful real-world performance gains for batch transfers.
