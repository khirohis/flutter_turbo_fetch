# Benchmark Performance Evaluation Report (`benchmark-results.md`)

This document serves as the **cross-platform executive summary and common evaluation methodology** for the empirical performance benchmarks of `flutter_turbo_fetch`, conducted according to the benchmark specifications defined in [`specs/architectures/benchmark.md`](../specs/architectures/benchmark.md).

For detailed platform-specific measurement environments, full scenario tables, and native stack analyses, refer to the dedicated reports under [`docs/benchmarks/`](benchmarks/):

- 🤖 **[Android Benchmark Report (`docs/benchmarks/android.md`)](benchmarks/android.md)** — `OkHttp3` Shared Connection Pool (API 36 Emulator)
- 🪟 **[Windows Benchmark Report (`docs/benchmarks/windows.md`)](benchmarks/windows.md)** — `WinHTTP` + OS Thread Pool (`TP_POOL`) (Win 11 x64 Desktop)
- 🍎 **[iOS Benchmark Report (`docs/benchmarks/ios.md`)](benchmarks/ios.md)** — iPhone 17 Physical Device Release, `URLSession` (Split-Pipeline Rigorous Protocol, N=10)
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

Due to differing measurement protocols, empirical results are strictly separated into **Part A (Historical Streaming Protocol)** and **Part B (Split-Pipeline Rigorous Protocol)**.

### 2.1 Part A: Historical Streaming Protocol (Android & Windows)

> **Protocol Note**: Evaluated in Dio's standard pipeline (`ResponseType.json`) as a single-trial sequential run. Streaming adapters (`IO`, `dio_http2`) resolve `fetch()` upon response headers and stream chunks inside `transformResponse()`, recording socket download time inside Parse Time.

#### 2.1.1 Total Wall-Clock Time Comparison

| Platform | Scenario | (1) IO (HTTP/1.1) | (2) dio_http2 | **(3) TurboFetch** | Wall-Time Speedup | Detailed Report |
| :--- | :--- | :---: | :---: | :---: | :---: | :--- |
| **Android**<br>*(API 36 Emulator)* | **Scenario A** (90 req, 3 workers) | 6,392 ms | 10,318 ms | **2,344 ms** | **2.7x – 4.4x faster** | [Android Report](benchmarks/android.md) |
| | **Scenario B** (100 req, 5 workers) | 6,471 ms | 11,063 ms | **2,451 ms** | **2.6x – 4.5x faster** | [Android Report](benchmarks/android.md) |
| | **Scenario C** (100 req, 10 workers) | 5,322 ms | 10,765 ms | **1,453 ms** | **3.7x – 7.4x faster** | [Android Report](benchmarks/android.md) |
| **Windows**<br>*(Win 11 x64 Desktop)* | **Scenario A** (90 req, 3 workers) | 250 ms | 516 ms | **118 ms** | **2.1x – 4.4x faster** | [Windows Report](benchmarks/windows.md) |
| | **Scenario B** (100 req, 5 workers) | 171 ms | 502 ms | **62 ms** | **2.8x – 8.1x faster** | [Windows Report](benchmarks/windows.md) |
| | **Scenario C** (100 req, 10 workers) | 173 ms | 789 ms | **50 ms** | **3.5x – 15.8x faster** | [Windows Report](benchmarks/windows.md) |

#### 2.1.2 Effective Per-Request Latency (Avg Net Time + Avg Parse Time)

| Platform | Scenario | (1) IO (HTTP/1.1) | (2) dio_http2 | **(3) TurboFetch** | Per-Request Speedup |
| :--- | :--- | :---: | :---: | :---: | :---: |
| **Android** | **Scenario A** (5–45KiB) | 199.6 ms | 283.3 ms | **65.8 ms** | **3.0x – 4.3x faster** |
| | **Scenario B** (10–100KiB) | 300.1 ms | 452.8 ms | **95.3 ms** | **3.1x – 4.8x faster** |
| | **Scenario C** (20–200KiB) | 486.8 ms | 846.4 ms | **104.0 ms** | **4.7x – 8.1x faster** |
| **Windows** | **Scenario A** (5–45KiB) | 8.0 ms | 19.4 ms | **3.7 ms** | **2.2x – 5.2x faster** |
| | **Scenario B** (10–100KiB) | 8.0 ms | 27.8 ms | **2.8 ms** | **2.9x – 9.9x faster** |
| | **Scenario C** (20–200KiB) | 15.7 ms | 76.0 ms | **4.3 ms** | **3.7x – 17.7x faster** |

---

### 2.2 Part B: Split-Pipeline Rigorous Protocol (iOS Physical Device, iPhone 17)

> [!IMPORTANT]
> **Measurement Isolation & Protocol Divergence**:
> The iOS physical-device evaluation employs a **controlled split-pipeline protocol** to eliminate timing asymmetries: every adapter receives the full body as bytes (`ResponseType.bytes`) before passing them to its JSON transformer. It also executes **1 warmup trial + 10 measured trials (N=10) with round-robin rotation** and 1:1 server event validation. **These metrics cannot be directly compared against Part A's phase values.**

#### 2.2.1 Total Wall-Clock Time & Throughput (10 Measured Trials Median)

| Platform | Scenario | (1) IO (HTTP/1.1) | (2) dio_http2 | **(3) TurboFetch** | Throughput (RPS) | Speedup vs. dio_http2 | Detailed Report |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| **iOS**<br>*(iPhone 17 Physical Device)* | **Scenario A** (90 req, 3 workers) | 444.8 ms | 724.0 ms | **462.0 ms** | 194.8 req/s | **1.6x faster (36.2%)** | [iOS Report](benchmarks/ios.md) |
| | **Scenario B** (100 req, 5 workers) | 370.5 ms | 1,329.5 ms | **411.8 ms** | 242.8 req/s | **3.2x faster (69.0%)** | [iOS Report](benchmarks/ios.md) |
| | **Scenario C** (100 req, 10 workers) | 429.9 ms | 2,629.9 ms | **454.2 ms** | 220.2 req/s | **5.8x faster (82.7%)** | [iOS Report](benchmarks/ios.md) |

#### 2.2.2 Phase Latency Breakdown (Per-Request Median: Network Time vs. Parse Time)

| Scenario | Preset | Network Time (P50) | Parse Time (P50) | Total Effective Latency (P50) |
| :--- | :--- | :---: | :---: | :---: |
| **Scenario A** (5–45KiB) | (1) IO (HTTP/1.1) | 14.07 ms | 0.35 ms | 14.42 ms |
| | (2) dio_http2 | 24.23 ms | 0.40 ms | 24.63 ms |
| | **(3) TurboFetch** | **14.35 ms** | **0.70 ms** | **15.05 ms** |
| **Scenario B** (10–100KiB) | (1) IO (HTTP/1.1) | 16.90 ms | 0.47 ms | 17.37 ms |
| | (2) dio_http2 | 63.08 ms | 1.36 ms | 64.44 ms |
| | **(3) TurboFetch** | **18.12 ms** | **1.18 ms** | **19.30 ms** |
| **Scenario C** (20–200KiB) | (1) IO (HTTP/1.1) | 39.22 ms | 0.62 ms | 39.84 ms |
| | (2) dio_http2 | 257.67 ms | 2.43 ms | 260.10 ms |
| | **(3) TurboFetch** | **41.34 ms** | **1.65 ms** | **42.99 ms** |

---

## 3. Common Architectural & Measurement Principles

### 3.1 Asymmetric Measurement Boundaries: Net Time vs. Parse Time

Because `BenchmarkTimingAdapter` and `BenchmarkTimingTransformer` instrument Dio's standard pipeline boundaries, recorded timestamps represent different physical phases depending on the adapter's transfer model:

- **(1) IO / (2) dio_http2 (Streaming Model)**:
  - `HttpClientAdapter.fetch()` resolves as soon as **initial HTTP response headers** arrive.
  - Reading and reassembling response body chunks from the TCP/TLS socket occurs downstream inside `Transformer.transformResponse`. Consequently, **socket transfer and HTTP/2 frame processing latency are recorded inside Parse Time**. Under high concurrency (Scenario C), `dio_http2` suffers severe Parse Time inflation because 10 parallel HTTP/2 streams compete on Dart's single event loop and hit the hardcoded 64KiB flow-control window.
- **(3) TurboFetch (Platform Native Batch Model)**:
  - The OS native layer (`OkHttp` on Android, `URLSession` on iOS/macOS, `WinHTTP` on Windows, `libcurl` on Linux) receives, decompresses, and concatenates all HTTP/2 `DATA` frames in background OS threads, returning a single contiguous `Uint8List` via Pigeon before `fetch()` resolves.
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

### 3.4 Hardware Characteristics & Latency Dynamics on High-Performance Hardware (iOS / Apple Silicon)

The iPhone 17 physical-device benchmark revealed subtle trade-offs when operating on high-performance mobile hardware:

1. **Massive HTTP/2 Superiority over Pure Dart (`dio_http2`)**:
   TurboFetch outperforms `dio_http2` by **1.6x in Scenario A, 3.2x in Scenario B, and 5.8x (82.7% faster) in Scenario C**. Pure Dart HTTP/2 struggles under stream multiplexing and single-thread event loop contention, whereas native `URLSession` manages multiplexing and flow control with near-zero Dart thread overhead.
2. **Dynamics vs. `IOHttpClientAdapter` on Apple Silicon**:
   In median wall-clock time, TurboFetch was slightly slower than `IOHttpClientAdapter` (+3.9% in A, +11.1% in B, +5.7% in C). Detailed request-level profiling explains why:
   - **Single-Core JSON Decoding**: iPhone 17's CPU cores decode JSON synchronously on the Main Isolate in **< 0.2 ms for <= 20KiB** and **< 0.7 ms for 100KiB**.
   - **The 24KiB Isolate Boundary**: Payloads >= 25KiB trigger `TurboTransformer`'s Isolate dispatch. For payloads between 25KiB and 100KiB, the Isolate spawn and semaphore scheduling overhead (0.9–1.9 ms) exceeded the raw Main Isolate decoding speed, temporarily inflating Parse Time.
   - **Ultra-Low Latency Wi-Fi LAN (RTT 1–2 ms)**: In local testing, HTTP/1.1 head-of-line blocking penalties are minimal. For payloads >= 120KiB, however, TurboFetch's pure network transmission was **up to 13% faster** than IO (42.7 ms vs. 49.1 ms at 200KiB), proving native HTTP/2 throughput gains as volume scales.

#### 3.4.1 Practical Tuning Recommendations for High-End Hardware

Based on empirical evidence from the iPhone 17 physical-device study, developers targeting modern high-end devices can maximize throughput with two simple adjustments:

- **Raise `isolateThresholdBytes` to 128KiB–256KiB**: On modern Apple Silicon and flagship mobile processors, decoding up to 100KiB synchronously on the Main Isolate takes under 0.7ms, and 200KiB takes ~1.0ms—safely below the 8.33ms (120 FPS) frame budget. Raising the threshold to **128KiB–256KiB** matches the physical Isolate break-even point and eliminates worker dispatch overhead, closing the slight gap with `IOHttpClientAdapter` while maintaining non-blocking safety for genuinely massive responses.
- **Scale `maxConcurrentIsolates` for Burst Concurrency**: Under 10+ parallel worker bursts (Scenario C), increasing semaphore capacity (e.g., to `4`–`6` on 6-core or 8-core CPUs) allows more concurrent decodes without queue stalls.

*(Note: Similar characteristics and tuning may also apply to macOS on Apple Silicon due to the shared Darwin native layer.)*
