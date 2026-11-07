# Benchmark Performance Evaluation Report (`benchmark-results.md`)

This document serves as the **cross-platform executive summary and common evaluation methodology** for the empirical performance benchmarks of `flutter_turbo_fetch`, conducted according to the benchmark specifications defined in [`specs/architectures/benchmark.md`](../specs/architectures/benchmark.md).

For detailed platform-specific measurement environments, full scenario tables, and native stack analyses, refer to the dedicated reports under [`docs/benchmarks/`](benchmarks/):

- 🤖 **[Android Benchmark Report (`docs/benchmarks/android.md`)](benchmarks/android.md)** — `OkHttp3` Shared Connection Pool
- 🪟 **[Windows Benchmark Report (`docs/benchmarks/windows.md`)](benchmarks/windows.md)** — `WinHTTP` + OS Thread Pool (`TP_POOL`)
- 🍎 **iOS Benchmark Report (`docs/benchmarks/ios.md`)** — `URLSession` *(Planned)*
- 💻 **macOS Benchmark Report (`docs/benchmarks/macos.md`)** *(Planned)*
- 🐧 **Linux Benchmark Report (`docs/benchmarks/linux.md`)** *(Planned)*

---

## 1. Benchmark Methodology Overview

### 1.1 Evaluated Adapter Presets (3 Configurations)

All benchmark scenarios evaluate three adapter and transformer combinations back-to-back in **Release (AOT)** mode against the same local Node.js HTTP/2 TLS mock server (`example/server/http2_mock_server.mjs`):

| Preset | Adapter (`HttpClientAdapter`) | Transformer (`Transformer`) | Architecture & Transfer Mechanism |
| :--- | :--- | :--- | :--- |
| **(1) IO (HTTP/1.1)** | `IOHttpClientAdapter` | `BackgroundTransformer` | Standard Dio configuration (`dart:io` HTTP/1.1 sockets, per-request `compute` Isolate above 50KiB). |
| **(2) dio_http2** | `Http2Adapter` | `BackgroundTransformer` | Pure Dart HTTP/2 (`package:http2`, 64KiB default receive window, single-isolate frame parsing). |
| **(3) TurboFetch** | **`TurboHttpAdapter`** | **`TurboTransformer`** | **OS native background HTTP/2 batch reception + 24KiB hybrid threshold zero-copy Isolate decoding.** |

> [!NOTE]
> **Why `native_dio_adapter` is excluded**:
> `native_dio_adapter` (`cronet_http` / `cupertino_http`) does not support bypassing self-signed certificate validation (`allowBadCertificates`) on Android Cronet (causing 100% immediate SSL failures against local HTTP/2 test servers) and falls back to standard `IOHttpClientAdapter` on Windows desktop. For an architectural analysis of `native_dio_adapter`, see [`docs/design-rationale.md`](design-rationale.md).

### 1.2 Workload Scenarios (10-Round Payload Sweeps)

| Scenario | Payload Sweep (Sequential per Round) | Max Workers | Total Requests | Primary Validation Objective |
| :--- | :--- | :---: | :---: | :--- |
| **Scenario A** | **5KiB to 45KiB** (9 steps of 5KiB) | **3** | **90 req** | **24KiB Hybrid Boundary**: Evaluate small-payload responsiveness and Main vs. Isolate switching overhead. |
| **Scenario B** | **10KiB to 100KiB** (10 steps of 10KiB) | **5** | **100 req** | **Standard Production Load**: Evaluate throughput and frame stability across typical mobile/desktop API sizes. |
| **Scenario C** | **20KiB to 200KiB** (10 steps of 20KiB) | **10** | **100 req** | **High-Concurrency Resilience**: Evaluate HTTP/2 multiplexing and semaphore governance (`maxConcurrentIsolates`). |

---

## 2. Cross-Platform Performance Highlights

Across both mobile (Android) and desktop (Windows) environments, **(3) TurboFetch** outperforms standard Dio configurations by **2.1x to 15.8x in total wall-clock completion time** and up to **17.7x in per-request effective latency**.

### 2.1 Total Wall-Clock Time Comparison

| Platform | Scenario | (1) IO (HTTP/1.1) | (2) dio_http2 | **(3) TurboFetch** | Wall-Time Speedup | Detailed Report |
| :--- | :--- | :---: | :---: | :---: | :---: | :--- |
| **Android**<br>*(API 36 Emulator)* | **Scenario A** (90 req, 3 workers) | 6,392 ms | 10,318 ms | **2,344 ms** | **2.7x – 4.4x faster** | [Android Report](benchmarks/android.md) |
| | **Scenario B** (100 req, 5 workers) | 6,471 ms | 11,063 ms | **2,451 ms** | **2.6x – 4.5x faster** | [Android Report](benchmarks/android.md) |
| | **Scenario C** (100 req, 10 workers) | 5,322 ms | 10,765 ms | **1,453 ms** | **3.7x – 7.4x faster** | [Android Report](benchmarks/android.md) |
| **Windows**<br>*(Win 11 x64 Desktop)* | **Scenario A** (90 req, 3 workers) | 250 ms | 516 ms | **118 ms** | **2.1x – 4.4x faster** | [Windows Report](benchmarks/windows.md) |
| | **Scenario B** (100 req, 5 workers) | 171 ms | 502 ms | **62 ms** | **2.8x – 8.1x faster** | [Windows Report](benchmarks/windows.md) |
| | **Scenario C** (100 req, 10 workers) | 173 ms | 789 ms | **50 ms** | **3.5x – 15.8x faster** | [Windows Report](benchmarks/windows.md) |

### 2.2 Effective Per-Request Latency (Avg Net Time + Avg Parse Time)

| Platform | Scenario | (1) IO (HTTP/1.1) | (2) dio_http2 | **(3) TurboFetch** | Per-Request Speedup |
| :--- | :--- | :---: | :---: | :---: | :---: |
| **Android** | **Scenario A** (5–45KiB) | 199.6 ms | 283.3 ms | **65.8 ms** | **3.0x – 4.3x faster** |
| | **Scenario B** (10–100KiB) | 300.1 ms | 452.8 ms | **95.3 ms** | **3.1x – 4.8x faster** |
| | **Scenario C** (20–200KiB) | 486.8 ms | 846.4 ms | **104.0 ms** | **4.7x – 8.1x faster** |
| **Windows** | **Scenario A** (5–45KiB) | 8.0 ms | 19.4 ms | **3.7 ms** | **2.2x – 5.2x faster** |
| | **Scenario B** (10–100KiB) | 8.0 ms | 27.8 ms | **2.8 ms** | **2.9x – 9.9x faster** |
| | **Scenario C** (20–200KiB) | 15.7 ms | 76.0 ms | **4.3 ms** | **3.7x – 17.7x faster** |

---

## 3. Common Architectural & Measurement Principles

### 3.1 Asymmetric Measurement Boundaries: Net Time vs. Parse Time

Because `BenchmarkTimingAdapter` and `BenchmarkTimingTransformer` instrument Dio's standard pipeline boundaries, recorded timestamps represent different physical phases depending on the adapter's transfer model:

- **(1) IO / (2) dio_http2 (Streaming Model)**:
  - `HttpClientAdapter.fetch()` resolves as soon as **initial HTTP response headers** arrive.
  - Reading and reassembling response body chunks from the TCP/TLS socket occurs downstream inside `Transformer.transformResponse`. Consequently, **socket transfer and HTTP/2 frame processing latency are recorded inside Parse Time**. Under high concurrency (Scenario C), `dio_http2` suffers severe Parse Time inflation because 10 parallel HTTP/2 streams compete on Dart's single event loop and hit the hardcoded 64KiB flow-control window.
- **(3) TurboFetch (Platform Native Batch Model)**:
  - The OS native layer (`OkHttp` on Android, `URLSession` on iOS, `WinHTTP` on Windows) receives, decompresses, and concatenates all HTTP/2 `DATA` frames in background OS threads, returning a single contiguous `Uint8List` via Pigeon before `fetch()` resolves.
  - Consequently, **network transmission, TLS decryption, full body download, native buffer assembly, and Pigeon IPC are all accounted for within Net Time**, while **Parse Time strictly measures in-memory fused UTF-8/JSON decoding (`JsonUtf8Decoder`), zero-copy Isolate handoff, and semaphore queuing**.

Evaluating **Effective Total Latency (Net Time + Parse Time)** alongside **Total Wall Time** provides a strictly apples-to-apples comparison across both models.

### 3.2 The "Jank Frames (%)" Denominator Paradox vs. Max Frame Duration

When interpreting UI smoothness metrics on mobile/emulator environments:

1. **Denominator Distortion**:
   Slow adapters (`dio_http2` taking >10 seconds on Android) spend significant time stalled waiting for network window updates, allowing Flutter to render hundreds of idle 60 FPS frames that dilute the jank percentage denominator. Conversely, `TurboFetch` completes all 100 requests in **1.45 seconds** on Android (**50 ms** on Windows), rendering very few total frames during the high-density completion burst (`setState()` progress updates firing 50+ times per second).
2. **True UI Responsiveness (`Max Frame Duration`)**:
   Worst-case UI freezes are accurately captured by **Max Frame Duration**. On Android, standard `(1) IO` caused severe UI freezes up to **511.5 ms** (Scenario B), whereas `(3) TurboFetch` kept worst-case frame duration down to **34.5 ms** in Scenario C (and **0.0 ms to 7.1 ms with 0% Jank** across all scenarios on Windows).

### 3.3 Inverted Concurrency Scaling & Memory Stability

A defining characteristic observed on both Android and Windows is **Inverted Concurrency Scaling**:

- **Android**: Total wall time improved from **2,344 ms** (Scenario A, 3 workers) -> **2,451 ms** (Scenario B, 5 workers) -> **1,453 ms** (Scenario C, 10 workers).
- **Windows**: Total wall time improved from **118 ms** (Scenario A, 3 workers) -> **62 ms** (Scenario B, 5 workers) -> **50 ms** (Scenario C, 10 workers).

Even as payload sizes quadrupled (up to 200KiB) and total transferred volume increased more than 4x, total completion time **decreased** as worker concurrency scaled from 3 to 10. This confirms that:

1. Native HTTP/2 connection multiplexing (`OkHttp` / `WinHTTP`) eliminates socket contention,
2. The **24KiB hybrid threshold** avoids Isolate spawn overhead on small payloads while offloading heavy JSON payloads via zero-copy `Isolate.run`, and
3. **Semaphore concurrency governance (`maxConcurrentIsolates`)** prevents Isolate thread storms and keeps Peak RSS memory growth bounded (~10–15 MiB delta).
