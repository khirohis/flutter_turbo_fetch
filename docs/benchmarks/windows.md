# Windows Benchmark Performance Report (`windows.md`)

This document records the empirical benchmark results and technical analysis for **Windows Desktop**, using the `WinHTTP` C++17 native batch reception engine of `flutter_turbo_fetch`.

For cross-platform comparisons and common evaluation methodology, see [`docs/benchmark-results.md`](../benchmark-results.md) and [`specs/architectures/benchmark.md`](../../specs/architectures/benchmark.md).

---

## 1. Measurement Environment

- **Device / OS**: Microsoft Windows 11 (Version 10.0.26300.9818, 64-bit `windows-x64`)
- **Native Engine**: Windows OS Standard `WinHTTP` (`winhttp.dll`, `WINHTTP_PROTOCOL_FLAG_HTTP2`) + Windows Thread Pool (`TP_POOL` / `TP_CLEANUP_GROUP`) via `TurboSessionManager` and `TurboHttpExecutor`
- **Flutter / Dart SDK**: Flutter 3.47.8 / Dart 3.13.7 (FVM)
- **Mock Server**: Node.js v26.12.1 Dual-Stack HTTP/2 TLS Server (`http2.createSecureServer`)
- **Network Interface**: Localhost loopback (`https://localhost:8443`)
- **Execution Mode**: Release AOT (`fvm flutter build windows --release`), 3 presets evaluated sequentially across Scenarios A, B, and C.

---

## 2. Benchmark Scenario Results

### 2.1 Scenario A: Small Payload Sweeps (5KiB to 45KiB, 3 Workers, 10 Rounds = 90 Requests)

**Validation Objective**: Verify response latency for lightweight payloads and evaluate the 24KiB hybrid threshold (Main Isolate synchronous decoding vs. zero-copy Isolate offloading).

| Adapter Configuration | Total Time | Net Time (Avg/P95) | Parse Time (Avg/P95) | Jank Frames (%) | Max Frame | Peak RSS (Δ) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| (1) IO (HTTP/1.1) | 250 ms | 2.7 ms / 7.7 ms | 5.3 ms / 9.9 ms | 0 (0.0%) | 0.0 ms | 123.2 MiB (+20.7 MiB) |
| (2) dio_http2 | 516 ms | 2.8 ms / 7.8 ms | 16.6 ms / 24.6 ms | 0 (0.0%) | 0.0 ms | 124.4 MiB (+7.6 MiB) |
| **(3) TurboFetch** | **118 ms** | **2.4 ms / 3.6 ms** | **1.3 ms / 6.2 ms** | **0 (0.0%)** | **0.0 ms** | **137.3 MiB (+15.5 MiB)** |

---

### 2.2 Scenario B: Standard Payload Sweeps (10KiB to 100KiB, 5 Workers, 10 Rounds = 100 Requests)

**Validation Objective**: Evaluate comprehensive throughput, UI responsiveness, and memory efficiency under typical API response sizes and standard concurrency.

| Adapter Configuration | Total Time | Net Time (Avg/P95) | Parse Time (Avg/P95) | Jank Frames (%) | Max Frame | Peak RSS (Δ) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| (1) IO (HTTP/1.1) | 171 ms | 1.9 ms / 5.0 ms | 6.1 ms / 8.6 ms | 0 (0.0%) | 0.0 ms | 139.0 MiB (+12.2 MiB) |
| (2) dio_http2 | 502 ms | 3.4 ms / 6.5 ms | 24.4 ms / 29.3 ms | 0 (0.0%) | 0.0 ms | 133.0 MiB (+2.7 MiB) |
| **(3) TurboFetch** | **62 ms** | **1.6 ms / 3.4 ms** | **1.2 ms / 4.6 ms** | **0 (0.0%)** | **0.0 ms** | **144.3 MiB (+11.4 MiB)** |

---

### 2.3 Scenario C: High-Concurrency Heavy Sweeps (20KiB to 200KiB, 10 Workers, 10 Rounds = 100 Requests)

**Validation Objective**: Verify load resilience, `WinHTTP` HTTP/2 stream multiplexing, OS Thread Pool scaling, and memory stability at 10 parallel workers.

| Adapter Configuration | Total Time | Net Time (Avg/P95) | Parse Time (Avg/P95) | Jank Frames (%) | Max Frame | Peak RSS (Δ) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| (1) IO (HTTP/1.1) | 173 ms | 3.2 ms / 14.1 ms | 12.5 ms / 15.1 ms | 0 (0.0%) | 0.0 ms | 140.5 MiB (+16.8 MiB) |
| (2) dio_http2 | 789 ms | 4.1 ms / 9.0 ms | 71.9 ms / 92.1 ms | 0 (0.0%) | 0.0 ms | 132.3 MiB (+3.1 MiB) |
| **(3) TurboFetch** | **50 ms** | **2.6 ms / 5.8 ms** | **1.7 ms / 4.9 ms** | **0 (0.0%)** | **7.1 ms** | **131.0 MiB (+10.1 MiB)** |

---

## 3. Technical Analysis (Windows / WinHTTP + OS Thread Pool)

### 3.1 Effective Per-Request Latency (Net Time + Parse Time)

Comparing the combined **Effective Total Latency per Request (Avg Net Time + Avg Parse Time)** demonstrates the raw efficiency of native C++17 `WinHTTP` batch reception paired with `TurboTransformer`:

| Scenario | (1) IO (HTTP/1.1) | (2) dio_http2 | **(3) TurboFetch** | Speedup Factor |
| :--- | :---: | :---: | :---: | :---: |
| **Scenario A (3 workers, 5–45KiB)** | 8.0 ms | 19.4 ms | **3.7 ms** | **2.2x to 5.2x faster** |
| **Scenario B (5 workers, 10–100KiB)** | 8.0 ms | 27.8 ms | **2.8 ms** | **2.9x to 9.9x faster** |
| **Scenario C (10 workers, 20–200KiB)** | 15.7 ms | 76.0 ms | **4.3 ms** | **3.7x to 17.7x faster** |

### 3.2 Sub-2ms Parse Latency & 24KiB Hybrid Boundary

- Across all three scenarios on Windows, `TurboTransformer` maintained an average Parse Time between **1.2 ms and 1.7 ms** (with P95 between **4.6 ms and 6.2 ms**).
- By contrast, `(1) IO` required **5.3 ms to 12.5 ms** and `(2) dio_http2` degraded from **16.6 ms to 71.9 ms** (P95: **92.1 ms**) as concurrency and payload size increased.
- Because `TurboHttpExecutor` pre-allocates contiguous `std::vector<uint8_t>` buffers using `Content-Length` hints and delivers the complete payload in a single Pigeon `EncodableValue` transfer, Dart's `JsonUtf8Decoder` operates at peak native memory bandwidth without stream listener allocations.

### 3.3 WinHTTP HTTP/2 Multiplexing & Inverted Concurrency Scaling (`118 ms -> 62 ms -> 50 ms`)

Windows exhibits dramatic **Inverted Concurrency Scaling**:

- **Scenario A (90 requests, 5–45KiB, 3 workers)**: **118 ms**
- **Scenario B (100 requests, 10–100KiB, 5 workers)**: **62 ms**
- **Scenario C (100 requests, 20–200KiB, 10 workers)**: **50 ms** (equivalent to **2,000 requests/second** throughput, transferring ~11 MiB of JSON in 0.05 seconds)

1. **Kernel-Assisted SChannel TLS & Shared `HINTERNET` Session**:
   `TurboSessionManager` shares a single HTTP/2-enabled `HINTERNET` session across requests. Once the TLS 1.3 / HTTP/2 connection is established, concurrent `WinHttpOpenRequest` / `WinHttpSendRequest` calls multiplex over the existing connection without repeating TLS handshakes.
2. **Windows OS Thread Pool (`TP_POOL`) Efficiency**:
   Executing synchronous WinHTTP reads in 64KiB blocks across the OS Thread Pool (bounded to 32 threads) allows all 10 concurrent worker streams in Scenario C to read and concatenate socket buffers in parallel across CPU cores, completely off the Dart Main Isolate.
3. **Elimination of Dart Event-Loop Bottlenecks**:
   While `(2) dio_http2` slowed down from **516 ms** (Scenario A) to **789 ms** (Scenario C) due to single-threaded Dart HTTP/2 frame parsing and 64KiB flow-control stalls, `(3) TurboFetch` accelerated to **50 ms** (**15.8x faster** than `dio_http2` and **3.5x faster** than `IO`).

### 3.4 Zero UI Jank (0.0%) and Memory Efficiency

- **Zero Jank Across All Workloads**:
  `(3) TurboFetch` recorded **0 Jank Frames (0.0%)** across Scenarios A, B, and C, with Max Frame Duration staying well below the 16.67 ms (60 FPS) threshold (**7.1 ms** in Scenario C).
- **Bounded RSS Footprint**:
  In Scenario C (highest load), `(3) TurboFetch` achieved the **lowest Peak RSS among all adapters (131.0 MiB, +10.1 MiB delta)**, compared to **140.5 MiB (+16.8 MiB delta)** for `(1) IO`.

