# Android Benchmark Performance Report (`android.md`)

This document records the empirical benchmark results and technical analysis for **Android**, using the `OkHttp3` native batch reception engine of `flutter_turbo_fetch`.

For cross-platform comparisons and common evaluation methodology, see [`docs/benchmark-results.md`](../benchmark-results.md) and [`specs/architectures/benchmark.md`](../../specs/architectures/benchmark.md).

---

## 1. Measurement Environment

- **Device / OS**: Android 16 (API 36) x86_64 Emulator (`sdk gphone64 x86 64`, `emulator-5554`)
- **Rendering Backend**: Impeller (OpenGLES)
- **Native Engine**: `com.squareup.okhttp3:okhttp:4.12.0` (`TurboClientManager` shared `ConnectionPool` + `TurboHttpExecutor`)
- **Flutter / Dart SDK**: Flutter 3.47.8 / Dart 3.13.7 (FVM)
- **Mock Server**: Node.js v26.12.1 Dual-Stack HTTP/2 TLS Server (`http2.createSecureServer`)
- **Network Interface**: Emulator virtual loopback (`https://10.0.2.2:8443`)
- **Execution Mode**: Release AOT (`fvm flutter build apk --release`), 3 presets evaluated sequentially across Scenarios A, B, and C.

---

## 2. Benchmark Scenario Results

### 2.1 Scenario A: Small Payload Sweeps (5KiB to 45KiB, 3 Workers, 10 Rounds = 90 Requests)

**Validation Objective**: Verify response latency for lightweight payloads and evaluate the 24KiB hybrid threshold (Main Isolate synchronous decoding vs. zero-copy Isolate offloading).

| Adapter Configuration | Total Time | Net Time (Avg/P95) | Parse Time (Avg/P95) | Jank Frames (%) | Max Frame | Peak RSS (Δ) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| (1) IO (HTTP/1.1) | 6,392 ms | 47.5 ms / 94.5 ms | 152.1 ms / 257.4 ms | 86 (29.9%) | 197.2 ms | 182.0 MiB (+11.6 MiB) |
| (2) dio_http2 | 10,318 ms | 56.2 ms / 56.2 ms | 227.1 ms / 227.1 ms | 543 (88.7%) | 36.2 ms | 183.6 MiB (+1.8 MiB) |
| **(3) TurboFetch** | **2,344 ms** | **57.5 ms / 238.2 ms** | **8.3 ms / 20.2 ms** | **62 (55.9%)** | **44.2 ms** | **185.4 MiB (+2.0 MiB)** |

---

### 2.2 Scenario B: Standard Payload Sweeps (10KiB to 100KiB, 5 Workers, 10 Rounds = 100 Requests)

**Validation Objective**: Evaluate comprehensive throughput, UI responsiveness, and memory efficiency under typical mobile API response sizes and standard concurrency.

| Adapter Configuration | Total Time | Net Time (Avg/P95) | Parse Time (Avg/P95) | Jank Frames (%) | Max Frame | Peak RSS (Δ) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| (1) IO (HTTP/1.1) | 6,471 ms | 72.1 ms / 137.4 ms | 228.0 ms / 396.0 ms | 69 (29.2%) | 511.5 ms | 193.1 MiB (+9.9 MiB) |
| (2) dio_http2 | 11,063 ms | 59.4 ms / 59.4 ms | 393.4 ms / 393.4 ms | 484 (72.0%) | 47.2 ms | 191.0 MiB (+0.1 MiB) |
| **(3) TurboFetch** | **2,451 ms** | **79.7 ms / 201.5 ms** | **15.6 ms / 69.7 ms** | **42 (64.6%)** | **90.3 ms** | **196.7 MiB (+8.6 MiB)** |

---

### 2.3 Scenario C: High-Concurrency Heavy Sweeps (20KiB to 200KiB, 10 Workers, 10 Rounds = 100 Requests)

**Validation Objective**: Verify load resilience, `OkHttp3` HTTP/2 stream multiplexing, and memory stability at the Isolate semaphore ceiling (10 parallel workers).

| Adapter Configuration | Total Time | Net Time (Avg/P95) | Parse Time (Avg/P95) | Jank Frames (%) | Max Frame | Peak RSS (Δ) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| (1) IO (HTTP/1.1) | 5,322 ms | 175.8 ms / 318.6 ms | 311.0 ms / 392.5 ms | 54 (34.6%) | 87.1 ms | 193.8 MiB (+3.6 MiB) |
| (2) dio_http2 | 10,765 ms | 50.6 ms / 50.6 ms | 795.8 ms / 795.8 ms | 7 (1.2%) | 73.4 ms | 190.3 MiB (+0.0 MiB) |
| **(3) TurboFetch** | **1,453 ms** | **88.7 ms / 145.5 ms** | **15.3 ms / 29.4 ms** | **30 (32.3%)** | **34.5 ms** | **199.9 MiB (+10.7 MiB)** |

---

## 3. Technical Analysis (Android / OkHttp3)

### 3.1 Effective Per-Request Latency (Net Time + Parse Time)

Because `(1) IO` and `(2) dio_http2` resolve `fetch()` upon header arrival and perform socket body streaming inside `Parse Time`, whereas `(3) TurboFetch` completes full body download and native `Okio` buffer concatenation inside `Net Time`, comparing the combined **Effective Total Latency per Request** highlights the true pipeline speedup:

| Scenario | (1) IO (HTTP/1.1) | (2) dio_http2 | **(3) TurboFetch** | Speedup Factor |
| :--- | :---: | :---: | :---: | :---: |
| **Scenario A (3 workers, 5–45KiB)** | 199.6 ms | 283.3 ms | **65.8 ms** | **3.0x to 4.3x faster** |
| **Scenario B (5 workers, 10–100KiB)** | 300.1 ms | 452.8 ms | **95.3 ms** | **3.1x to 4.8x faster** |
| **Scenario C (10 workers, 20–200KiB)** | 486.8 ms | 846.4 ms | **104.0 ms** | **4.7x to 8.1x faster** |

### 3.2 Validation of the 24KiB Hybrid Isolate Threshold (Scenario A)

- In Scenario A, `TurboTransformer` achieved an average parse time of **8.3 ms (P95: 20.2 ms)**, compared to `152.1 ms` for `IO` and `227.1 ms` for `dio_http2`.
- Payloads under 24KiB (`5, 10, 15, 20 KiB`) are decoded synchronously on the Main Isolate using fused `JsonUtf8Decoder`, avoiding the 1–2 ms Isolate spawn overhead on Android, while payloads `>= 25 KiB` are offloaded via zero-copy `Isolate.run`.
- While `(1) IO` experienced a severe UI freeze of **197.2 ms** Max Frame Duration in Scenario A, `(3) TurboFetch` capped Max Frame Duration at **44.2 ms** and completed all 90 requests in **2,344 ms** (2.7x faster than IO, 4.4x faster than `dio_http2`).

### 3.3 OkHttp3 HTTP/2 Multiplexing & Inverted Concurrency Scaling (Scenario C)

- Across Scenarios A, B, and C, `TurboFetch` completed 100 heavy requests (up to 200KiB each) in Scenario C in just **1,453 ms** — significantly faster than Scenario A (**2,344 ms**) and Scenario B (**2,451 ms**).
- **`OkHttp3` 16MiB Receive Window & Shared Pool**: Unlike `dio_http2` (which stalls on its 64KiB receive window, pushing per-request Parse Time to **795.8 ms** and Total Time to **10,765 ms**), `OkHttp3` configures a 16MiB HTTP/2 window and reuses a single TLS connection across all 10 concurrent workers on its native `Dispatcher` thread pool.
- **Semaphore Concurrency Governance**: Even with 10 parallel workers delivering 200KiB payloads at nearly 70 requests/second, `TurboTransformer`'s semaphore kept average Parse Time at **15.3 ms (P95: 29.4 ms)** and achieved the lowest Max Frame Duration across all adapters (**34.5 ms**, compared to **511.5 ms** worst-case freeze on `IO` in Scenario B).

### 3.4 Memory Stability (RSS)

- Baseline Release AOT RSS remained steady between **182 MiB and 199 MiB**.
- Memory expansion (`Δ RSS`) for `TurboFetch` was **+2.0 MiB** in Scenario A, **+8.6 MiB** in Scenario B, and **+10.7 MiB** in Scenario C. Performing chunk aggregation in `Okio`'s native segment pool and transferring the completed `byte[]` once per request prevents intermediate Dart heap fragmentation and GC spikes.

