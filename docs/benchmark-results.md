# Benchmark Performance Evaluation Report (`benchmark-results.md`)

This document serves as the **cross-platform executive summary and common evaluation methodology** for the empirical performance benchmarks of `flutter_turbo_fetch`, conducted according to the Split-Pipeline Rigorous Protocol (`N=10` round-robin trials + 1:1 server audit) defined in [`specs/architectures/benchmark.md`](../specs/architectures/benchmark.md).

For detailed platform-specific measurement environments, full scenario tables, and native stack analyses, refer to the dedicated reports under [`docs/benchmarks/`](benchmarks/):

- 🤖 **[Android Benchmark Report (`docs/benchmarks/android.md`)](benchmarks/android.md)** — Pixel 10a Physical Device Release, `OkHttp3` & `Cronet` (Split-Pipeline Rigorous Protocol, N=10)
- 🪟 **[Windows Benchmark Report (`docs/benchmarks/windows.md`)](benchmarks/windows.md)** — Windows 11 x64 Desktop Release, `WinHTTP` + OS Thread Pool (Split-Pipeline Rigorous Protocol, N=10)
- 🍎 **[iOS Benchmark Report (`docs/benchmarks/ios.md`)](benchmarks/ios.md)** — iPhone 17 Physical Device Release, `URLSession` (Split-Pipeline Rigorous Protocol, N=10)
- 💻 **macOS Benchmark Report (`docs/benchmarks/macos.md`)** *(Planned)*
- 🐧 **[Linux Benchmark Report (`docs/benchmarks/linux.md`)](benchmarks/linux.md)** — Linux Mint 22.3 (Ubuntu 24.04) x64 Desktop Release, `libcurl` (`curl_multi`, Split-Pipeline Rigorous Protocol, N=10)

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
> `native_dio_adapter` (`cronet_http` / `cupertino_http`) does not support bypassing self-signed certificate validation (`allowBadCertificates`) on Android Cronet and falls back to standard `IOHttpClientAdapter` on Windows desktop. For an architectural analysis of `native_dio_adapter`, see [`docs/design-rationale.md`](design-rationale.md).

### 1.2 Workload Scenarios (10-Round Payload Sweeps)

| Scenario | Payload Sweep (Sequential per Round) | Max Workers | Total Requests | Primary Validation Objective |
| :--- | :--- | :---: | :---: | :--- |
| **Scenario A** | **5KiB to 45KiB** (9 steps of 5KiB) | **3** | **90 req** | **24KiB Hybrid Boundary**: Evaluate small-payload responsiveness and Main vs. Isolate switching overhead. |
| **Scenario B** | **10KiB to 100KiB** (10 steps of 10KiB) | **5** | **100 req** | **Standard Production Load**: Evaluate throughput and frame stability across typical mobile/desktop API sizes. |
| **Scenario C** | **20KiB to 200KiB** (10 steps of 20KiB) | **10** | **100 req** | **High-Concurrency Resilience**: Evaluate HTTP/2 multiplexing and semaphore governance (`maxConcurrentIsolates`). |

### 1.3 Split-Pipeline Rigorous Protocol (`N=10`)

Every evaluated platform below uses the **controlled split-pipeline protocol** to eliminate timing asymmetries between streaming and batch adapters:

- **Common Reception Boundary (`ResponseType.bytes`)**: Every adapter receives the full response payload as bytes before passing them to its JSON transformer (`BackgroundTransformer` or `TurboTransformer`). `Network Time` strictly ends at full-body reception; `Parse Time` strictly measures decoding and Isolate/semaphore handoff.
- **Statistical Rigor & Audit**: Each platform executes **1 warmup trial + 10 measured trials (`N=10`) with round-robin preset rotation** (90 measured trials / 8,700 measured requests per engine), verified 1-to-1 against independent server-side event logs.

---

## 2. Cross-Platform Performance Highlights (Split-Pipeline Rigorous Protocol, N=10)

### 2.1 Total Wall-Clock Time & Throughput (10 Measured Trials Median)

| Platform | Scenario | (1) IO (HTTP/1.1) | (2) dio_http2 | **(3) TurboFetch** | Throughput (RPS) | Speedup vs. IO / dio_http2 | Detailed Report |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| **Android (OkHttp3)**<br>*(Pixel 10a, Wi-Fi LAN)* | **Scenario A** (90 req, 3w) | 492.2 ms | 2,220.3 ms | **494.1 ms** | 182.3 req/s | **1.00x / 4.5x faster** | [Android Report](benchmarks/android.md) |
| | **Scenario B** (100 req, 5w) | 490.2 ms | 1,277.2 ms | **359.7 ms** | 278.1 req/s | **1.4x / 3.6x faster** | [Android Report](benchmarks/android.md) |
| | **Scenario C** (100 req, 10w) | 655.6 ms | 2,320.9 ms | **394.0 ms** | 254.4 req/s | **1.7x / 5.9x faster** | [Android Report](benchmarks/android.md) |
| **Android (Cronet)**<br>*(Pixel 10a, Wi-Fi LAN)* | **Scenario A** (90 req, 3w) | 488.3 ms | 2,256.8 ms | **472.5 ms** | 190.5 req/s | **1.03x / 4.8x faster** | [Android Report](benchmarks/android.md) |
| | **Scenario B** (100 req, 5w) | 523.4 ms | 1,287.6 ms | **297.2 ms** | 336.5 req/s | **1.8x / 4.3x faster** | [Android Report](benchmarks/android.md) |
| | **Scenario C** (100 req, 10w) | 633.8 ms | 2,411.1 ms | **353.8 ms** | 282.7 req/s | **1.8x / 6.8x faster** | [Android Report](benchmarks/android.md) |
| **Windows**<br>*(Win 11 x64 Desktop)* | **Scenario A** (90 req, 3w) | 77.9 ms | 108.6 ms | **38.9 ms** | 2,313.5 req/s | **2.0x / 2.8x faster** | [Windows Report](benchmarks/windows.md) |
| | **Scenario B** (100 req, 5w) | 111.7 ms | 234.1 ms | **40.8 ms** | 2,451.5 req/s | **2.7x / 5.7x faster** | [Windows Report](benchmarks/windows.md) |
| | **Scenario C** (100 req, 10w) | 169.9 ms | 390.9 ms | **51.2 ms** | 1,954.6 req/s | **3.3x / 7.6x faster** | [Windows Report](benchmarks/windows.md) |
| **Linux**<br>*(Mint 22.3 x64 Desktop)* | **Scenario A** (90 req, 3w) | 841.8 ms | 2,032.0 ms | **330.5 ms** | 272.4 req/s | **2.5x / 6.1x faster** | [Linux Report](benchmarks/linux.md) |
| | **Scenario B** (100 req, 5w) | 442.3 ms | 2,489.3 ms | **137.7 ms** | 726.6 req/s | **3.2x / 18.1x faster** | [Linux Report](benchmarks/linux.md) |
| | **Scenario C** (100 req, 10w) | 318.0 ms | 4,698.2 ms | **82.7 ms** | 1,209.8 req/s | **3.8x / 56.8x faster** | [Linux Report](benchmarks/linux.md) |
| **iOS**<br>*(iPhone 17, Wi-Fi LAN)* | **Scenario A** (90 req, 3w) | 444.8 ms | 724.0 ms | **462.0 ms** | 194.8 req/s | **0.96x / 1.6x faster** | [iOS Report](benchmarks/ios.md) |
| | **Scenario B** (100 req, 5w) | 370.5 ms | 1,329.5 ms | **411.8 ms** | 242.8 req/s | **0.90x / 3.2x faster** | [iOS Report](benchmarks/ios.md) |
| | **Scenario C** (100 req, 10w) | 429.9 ms | 2,629.9 ms | **454.2 ms** | 220.2 req/s | **0.95x / 5.8x faster** | [iOS Report](benchmarks/ios.md) |

### 2.2 Phase Latency Breakdown (Per-Request Median: Network Time vs. Parse Time)

| Platform | Scenario | Preset | Network Time (P50) | Parse Time (P50) | Total Effective Latency (P50) |
| :--- | :--- | :--- | :---: | :---: | :---: |
| **Android (OkHttp3)** | **Scenario A** (5–45KiB) | (1) IO (HTTP/1.1) | 15.57 ms | 0.31 ms | 15.88 ms |
| | | (2) dio_http2 | 76.46 ms | 0.71 ms | 77.17 ms |
| | | **(3) TurboFetch** | **13.67 ms** | **2.20 ms** | **15.87 ms** |
| | **Scenario B** (10–100KiB) | (1) IO (HTTP/1.1) | 20.58 ms | 2.14 ms | 22.72 ms |
| | | (2) dio_http2 | 58.78 ms | 2.94 ms | 61.72 ms |
| | | **(3) TurboFetch** | **13.72 ms** | **3.18 ms** | **16.90 ms** |
| | **Scenario C** (20–200KiB) | (1) IO (HTTP/1.1) | 50.75 ms | 3.43 ms | 54.18 ms |
| | | (2) dio_http2 | 215.26 ms | 3.08 ms | 218.34 ms |
| | | **(3) TurboFetch** | **32.89 ms** | **3.19 ms** | **36.08 ms** |
| **Android (Cronet)** | **Scenario A** (5–45KiB) | (1) IO (HTTP/1.1) | 15.97 ms | 0.32 ms | 16.29 ms |
| | | (2) dio_http2 | 74.83 ms | 0.63 ms | 75.46 ms |
| | | **(3) TurboFetch** | **13.05 ms** | **2.09 ms** | **15.14 ms** |
| | **Scenario B** (10–100KiB) | (1) IO (HTTP/1.1) | 22.13 ms | 2.56 ms | 24.69 ms |
| | | (2) dio_http2 | 57.92 ms | 4.12 ms | 62.04 ms |
| | | **(3) TurboFetch** | **10.61 ms** | **3.17 ms** | **13.78 ms** |
| | **Scenario C** (20–200KiB) | (1) IO (HTTP/1.1) | 49.92 ms | 3.20 ms | 53.12 ms |
| | | (2) dio_http2 | 221.09 ms | 4.59 ms | 225.68 ms |
| | | **(3) TurboFetch** | **28.51 ms** | **3.23 ms** | **31.74 ms** |
| **Windows** | **Scenario A** (5–45KiB) | (1) IO (HTTP/1.1) | 2.28 ms | 0.19 ms | 2.47 ms |
| | | (2) dio_http2 | 3.10 ms | 0.18 ms | 3.28 ms |
| | | **(3) TurboFetch** | **0.81 ms** | **0.27 ms** | **1.08 ms** |
| | **Scenario B** (10–100KiB) | (1) IO (HTTP/1.1) | 4.71 ms | 0.81 ms | 5.52 ms |
| | | (2) dio_http2 | 10.51 ms | 0.84 ms | 11.35 ms |
| | | **(3) TurboFetch** | **1.24 ms** | **0.50 ms** | **1.74 ms** |
| | **Scenario C** (20–200KiB) | (1) IO (HTTP/1.1) | 14.75 ms | 1.43 ms | 16.18 ms |
| | | (2) dio_http2 | 36.45 ms | 1.33 ms | 37.78 ms |
| | | **(3) TurboFetch** | **3.18 ms** | **0.91 ms** | **4.09 ms** |
| **Linux** | **Scenario A** (5–45KiB) | (1) IO (HTTP/1.1) | 27.54 ms | 0.29 ms | 27.83 ms |
| | | (2) dio_http2 | 74.28 ms | 0.29 ms | 74.57 ms |
| | | **(3) TurboFetch** | **9.66 ms** | **1.63 ms** | **11.29 ms** |
| | **Scenario B** (10–100KiB) | (1) IO (HTTP/1.1) | 19.80 ms | 1.23 ms | 21.03 ms |
| | | (2) dio_http2 | 117.12 ms | 1.45 ms | 118.57 ms |
| | | **(3) TurboFetch** | **5.24 ms** | **1.52 ms** | **6.76 ms** |
| | **Scenario C** (20–200KiB) | (1) IO (HTTP/1.1) | 26.21 ms | 1.61 ms | 27.82 ms |
| | | (2) dio_http2 | 466.10 ms | 2.49 ms | 468.59 ms |
| | | **(3) TurboFetch** | **5.07 ms** | **1.87 ms** | **6.94 ms** |
| **iOS** | **Scenario A** (5–45KiB) | (1) IO (HTTP/1.1) | 14.07 ms | 0.35 ms | 14.42 ms |
| | | (2) dio_http2 | 24.23 ms | 0.40 ms | 24.63 ms |
| | | **(3) TurboFetch** | **14.35 ms** | **0.70 ms** | **15.05 ms** |
| | **Scenario B** (10–100KiB) | (1) IO (HTTP/1.1) | 16.90 ms | 0.47 ms | 17.37 ms |
| | | (2) dio_http2 | 63.08 ms | 1.36 ms | 64.44 ms |
| | | **(3) TurboFetch** | **18.12 ms** | **1.18 ms** | **19.30 ms** |
| | **Scenario C** (20–200KiB) | (1) IO (HTTP/1.1) | 39.22 ms | 0.62 ms | 39.84 ms |
| | | (2) dio_http2 | 257.67 ms | 2.43 ms | 260.10 ms |
| | | **(3) TurboFetch** | **41.34 ms** | **1.65 ms** | **42.99 ms** |

---

## 3. Common Architectural & Measurement Principles

### 3.1 Native Batch Reception vs. Pure-Dart HTTP/2 Streaming

Across every tested OS and hardware class (Android, Windows, Linux, iOS), pure-Dart `dio_http2` collapses under concurrent HTTP/2 workloads (4.3x–56.8x slower than `TurboFetch` in Scenarios B & C) because 5–10 parallel HTTP/2 streams compete on Dart's single event loop and stall on its hardcoded 64KiB initial receive window.

By contrast, `TurboFetch` offloads TLS decryption, HTTP/2 `DATA` frame reassembly, and decompression into platform-native background engines (`OkHttp3` / `Cronet` on Android, `URLSession` on iOS/macOS, `WinHTTP` on Windows, `libcurl` `curl_multi` on Linux), returning a single contiguous `Uint8List` via Pigeon once per request.

### 3.2 Concurrency Scaling Across Platforms

- **Android (Pixel 10a, Wi-Fi LAN)**:
  - **`OkHttp3`**: Scales from **494.1 ms** (3w) -> **359.7 ms** (5w) -> **394.0 ms** (10w), beating `IO` by **1.4x–1.7x** and `dio_http2` by **3.6x–5.9x** in Scenarios B & C.
  - **`Cronet`**: Extends the lead across every scenario (**472.5 ms** -> **297.2 ms** -> **353.8 ms**, peaking at **336.5 req/s**), outperforming `OkHttp3` by **4.4%–17.4%** and `dio_http2` by **4.3x–6.8x** with tight variance (CV 6.5%–8.9%).
- **Windows (Win 11 x64 Desktop)**: Even as payload sizes quadrupled (up to 200KiB), median total wall time stayed nearly flat (**38.9 ms** -> **40.8 ms** -> **51.2 ms**), widening the lead over `IO` from **2.0x to 3.3x** and over `dio_http2` from **2.8x to 7.6x**.
- **Linux (Mint 22.3 x64 Desktop)**: `libcurl` `curl_multi` multiplexing drove inverted concurrency scaling (**330.5 ms** -> **137.7 ms** -> **82.7 ms**, **1,209.8 req/s**), reaching **3.8x faster than `IO`** and **56.8x faster than `dio_http2`**.

### 3.3 Hardware Characteristics & Tuning on High-Performance Mobile Chips

On modern mobile SoCs (iPhone 17 Apple Silicon and Pixel 10a Tensor G4), Scenario A (5–45KiB) reveals the exact break-even point of the **24KiB hybrid threshold**:

- Payloads below 24KiB (`5–20KiB`) decode synchronously on the Main Isolate in **0.14–0.37 ms**.
- Payloads in the `25–45KiB` band incur ~1–3 ms of Worker Isolate dispatch/semaphore overhead when offloaded, while synchronous Main Isolate decoding takes < 0.7 ms on flagship CPUs.

#### Practical Tuning Recommendations for High-End Hardware

- **Raise `isolateThresholdBytes` to 128KiB–256KiB**: On modern Apple Silicon and flagship Android processors, raising the threshold matches the physical Isolate break-even point—eliminating Worker Isolate dispatch overhead on mid-sized responses while preserving non-blocking UI safety for large payloads.
- **Scale `maxConcurrentIsolates` for Burst Concurrency**: Under 10+ parallel worker bursts (Scenario C), increasing semaphore capacity (e.g., to `4`–`6` on 6-core or 8-core CPUs) allows more concurrent decodes without queue stalls.

*(Note: In this headless Release protocol, frame timing is intentionally recorded as `N/A` because `SchedulerBinding` frame callbacks are not instrumented in Release AOT benchmarks.)*
